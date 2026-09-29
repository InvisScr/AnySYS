#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build test floppies for the AnySYS QEMU harness and check the results.

    fatcheck.py fixture OUTDIR
        Write OUTDIR/used.img - a 720K FAT12 floppy whose low clusters are
        all taken: a file, a directory with a file and a nested directory,
        and a file fragmented across the area a kernel has to clear - plus
        OUTDIR/files/, the originals to compare against afterwards. Also
        OUTDIR/blank.img, the same geometry with nothing on it. The data is
        pseudo-random from a fixed seed, so every run is identical.

    fatcheck.py verify IMG FILESDIR KSRCDIR K1 K2
        After AnySYS ran on a copy of used.img: every fixture file still
        reads back byte for byte, both kernels are identical to their
        source, K1 and K2 are root entries 0 and 1 with system attributes,
        K1 starts at cluster 2, and fsck.fat finds nothing wrong.

    fatcheck.py match IMG REF
        The kernel layout of IMG is the same as REF's: root entries 0 and
        1 (name, attributes, start cluster, size), the BPB, and the boot
        code from the boot sector's jump target on.

Exit status 0 means every check passed; each check prints one line.
"""

import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile

# The fixture and its expected placement. Clusters are 1 KB (2 sectors),
# so with this write order the layout is:
#   2 HELLO.TXT   3 SUB   4-6 SUB/F1.BIN   7 SUB/INNER   8-10 F2.BIN
#   11-14,19-24 C.BIN (fragmented around B.BIN)   15-18 B.BIN   25-54 BIG.BIN
FILES = [
    ("HELLO.TXT", 6),
    ("F1.BIN", 3000),
    ("F2.BIN", 3000),
    ("A.BIN", 4000),
    ("B.BIN", 4000),
    ("C.BIN", 10000),
    ("BIG.BIN", 30000),
]
KEPT = ["HELLO.TXT", "SUB/F1.BIN", "SUB/INNER/F2.BIN", "B.BIN", "C.BIN", "BIG.BIN"]
MKFS = ["mkfs.fat", "-C", "-f", "2", "-r", "112", "-S", "512", "-s", "2"]

failures = 0


def run(*cmd):
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


def check(ok, text):
    global failures
    print(("ok    " if ok else "FAIL  ") + text)
    if not ok:
        failures += 1


def fixture(out):
    files = os.path.join(out, "files")
    os.makedirs(files, exist_ok=True)
    rng = random.Random(0x5A5)
    for name, size in FILES:
        with open(os.path.join(files, name), "wb") as f:
            f.write(bytes(rng.getrandbits(8) for _ in range(size)))

    for img in ("used.img", "blank.img"):
        path = os.path.join(out, img)
        if os.path.exists(path):
            os.remove(path)
        run(*MKFS, path, "720")

    img = os.path.join(out, "used.img")
    f = lambda n: os.path.join(files, n)
    run("mcopy", "-i", img, f("HELLO.TXT"), "::/")
    run("mmd", "-i", img, "::/SUB")
    run("mcopy", "-i", img, f("F1.BIN"), "::/SUB/")
    run("mmd", "-i", img, "::/SUB/INNER")
    run("mcopy", "-i", img, f("F2.BIN"), "::/SUB/INNER/")
    run("mcopy", "-i", img, f("A.BIN"), f("B.BIN"), "::/")
    run("mdel", "-i", img, "::/A.BIN")             # leaves a hole C.BIN splits across
    run("mcopy", "-i", img, f("C.BIN"), f("BIG.BIN"), "::/")


class Fat:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        self.bps, self.spc, self.res, self.nfats, self.nroot = struct.unpack_from("<HBHBH", d, 11)
        self.spf = struct.unpack_from("<H", d, 22)[0]
        self.root = (self.res + self.nfats * self.spf) * self.bps

    def root_entry(self, i):
        e = self.d[self.root + 32 * i:self.root + 32 * i + 32]
        name = e[:8].decode("latin1").rstrip() + "." + e[8:11].decode("latin1").rstrip()
        return name.rstrip("."), e[11], struct.unpack_from("<H", e, 26)[0], struct.unpack_from("<I", e, 28)[0]

    def entry_offset(self):
        b = self.d
        if b[0] == 0xEB:
            return b[1] + 2
        if b[0] == 0xE9:
            return struct.unpack_from("<H", b, 1)[0] + 3
        return 0x3E


def fsck(img):
    r = subprocess.run(["fsck.fat", "-n", img], capture_output=True, text=True)
    lines = [l for l in r.stdout.splitlines() if l.strip()]
    ok = r.returncode == 0

    # fsck.fat reads a volume label at 2Bh whatever the boot sector is. A
    # pre-DOS 4 boot sector (no 29h extended BPB signature at 26h) keeps
    # code there, and Toshiba's own untouched disk draws the same "label
    # stored in boot sector" complaint. Only that complaint is excused.
    if not ok and open(img, "rb").read(0x27)[0x26] != 0x29:
        noise = ("fsck.fat ", "Label ", "Volume label ", "  Auto-", "Leaving ")
        complaints = [l for l in lines[:-1] if not l.startswith(noise)]
        if not complaints and any("stored in boot sector" in l for l in lines):
            ok = True
            lines[-1] += " (boot-sector label note ignored: pre-DOS 4 boot code)"
    check(ok, "fsck.fat: " + (lines[-1] if lines else "no output"))


def read_back(img, name, dest):
    return subprocess.run(["mcopy", "-n", "-i", img, "::/" + name, dest],
                          capture_output=True).returncode == 0


def same(a, b):
    with open(a, "rb") as x, open(b, "rb") as y:
        return x.read() == y.read()


def verify(img, filesdir, ksrc, k1, k2):
    tmp = tempfile.mkdtemp()
    try:
        for name in KEPT:
            dest = os.path.join(tmp, "x")
            ok = read_back(img, name, dest) and same(dest, os.path.join(filesdir, os.path.basename(name)))
            check(ok, name + " intact")
        for k in (k1, k2):
            dest = os.path.join(tmp, "x")
            ok = read_back(img, k, dest) and same(dest, os.path.join(ksrc, k))
            check(ok, k + " identical to source")
    finally:
        shutil.rmtree(tmp)

    fat = Fat(img)
    for slot, k in ((0, k1), (1, k2)):
        name, attr, start, _ = fat.root_entry(slot)
        check(name == k and (attr & 0x07) == 0x07,
              "root entry %d is %s, attributes %02Xh" % (slot, name, attr))
    check(fat.root_entry(0)[2] == 2, "%s starts at cluster %d" % (k1, fat.root_entry(0)[2]))
    fsck(img)


def match(img, ref):
    a, b = Fat(img), Fat(ref)
    for slot in (0, 1):
        ea, eb = a.root_entry(slot), b.root_entry(slot)
        check(ea[:1] + ea[2:] == eb[:1] + eb[2:] and (ea[1] & 0x07) == (eb[1] & 0x07),
              "root entry %d: %s cluster %d size %d (reference: %s cluster %d size %d)"
              % (slot, ea[0], ea[2], ea[3], eb[0], eb[2], eb[3]))
    check(a.d[0x0B:0x1E] == b.d[0x0B:0x1E], "BPB matches the reference")
    start = b.entry_offset()
    check(a.d[start:512] == b.d[start:512], "boot code from %02Xh matches the reference" % start)
    fsck(img)


def main(argv):
    if len(argv) >= 3 and argv[1] == "fixture":
        fixture(argv[2])
        return 0
    if len(argv) == 7 and argv[1] == "verify":
        verify(*argv[2:])
    elif len(argv) == 4 and argv[1] == "match":
        match(*argv[2:])
    else:
        print(__doc__, file=sys.stderr)
        return 2
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
