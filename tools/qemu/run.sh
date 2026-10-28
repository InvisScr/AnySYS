#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Run AnySYS under real DOS on QEMU against test floppies, and check what
# it did to them. See README in this directory.
#
#   tools/qemu/run.sh                  every case
#   tools/qemu/run.sh ms622-used       one or more named cases
#
# Cases:
#   toshiba-fresh  blank floppy, Toshiba kernels + boot template; the
#                  result must match Toshiba's own disk
#   toshiba-used   used floppy, Toshiba kernels; files moved out of the
#                  way of cluster 2 must all survive
#   ms622-used     used floppy, MS-DOS 6.22 kernels + boot template; as
#                  above, then the floppy is booted and must reach DOS
#
# Everything lands in tools/qemu/out/<case>/: the floppy, serial.log (all
# AnySYS output, stderr included), and qemu.log.
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out="$here/out"

REFS="/mnt/Games/-User-Data-3/2026 Other/HDD for AnyDOS"
REF622=${REF622:-"$REFS/Done2/Microsoft MS-DOS 6.22 Plus Enhanced Tools.img"}
TOSHREF=${TOSHREF:-"$REFS/Done/Microsoft MS-DOS 2.11 [Toshiba T1100 Plus OEM] (3.5-720k)/disk01.img"}
EXE=${EXE:-"$root/AnySYS.exe"}
TIMEOUT=${TIMEOUT:-120}
CASES=${*:-"toshiba-fresh toshiba-used ms622-used"}

for f in "$EXE" "$REF622" "$TOSHREF"; do
    [ -f "$f" ] || { echo "missing: $f" >&2; [ "$f" = "$EXE" ] && echo "build first: wmake -h -f makefile.wat" >&2; exit 2; }
done
for t in qemu-system-i386 mcopy mkfs.fat fsck.fat python3; do
    command -v "$t" >/dev/null || { echo "missing tool: $t" >&2; exit 2; }
done

# Clear the contents rather than the directory: removing the directory
# itself fails on some mounts even when the unlinks succeeded.
mkdir -p "$out"
rm -rf "$out"/* 2>/dev/null || true
mkdir -p "$out/src/T" "$out/src/MS"

# ---- the machine that runs AnySYS ---------------------------------------

# C: is a copy of the MS-DOS 6.22 reference disk, which is only there to
# boot DOS. Its partition offset comes from its own MBR.
cp "$REF622" "$out/boot.img"
lba=$(python3 -c "import struct,sys; print(struct.unpack_from('<I', open(sys.argv[1],'rb').read(512), 0x1C6)[0])" "$out/boot.img")
C="$out/boot.img@@$((lba * 512))"

# The kernels, taken from the reference disks rather than kept here.
mcopy -n -i "$TOSHREF" ::/TBIOS.SYS ::/TDOS.SYS ::/COMMAND.COM "$out/src/T/"
mcopy -n -i "$C" ::/IO.SYS ::/MSDOS.SYS ::/COMMAND.COM "$out/src/MS/"

# Boot templates: each version's own boot sector. For 6.22 that is the
# hard disk's volume boot record; AnySYS keeps the floppy's BPB either way.
dd if="$TOSHREF" of="$out/TBOOT.BIN" bs=512 count=1 status=none
dd if="$out/boot.img" of="$out/B622.BIN" bs=512 skip="$lba" count=1 status=none

# QUIT.COM writes to QEMU's isa-debug-exit port so the machine stops on
# its own: mov dx,501h / mov al,0 / out dx,al / ret. QEMU exits 1.
printf '\272\001\005\260\000\356\303' > "$out/QUIT.COM"

mmd -i "$C" ::/SRCT ::/SRCMS 2>/dev/null || true
mcopy -o -i "$C" "$out"/src/T/* ::/SRCT/
mcopy -o -i "$C" "$out"/src/MS/* ::/SRCMS/
mcopy -o -i "$C" "$EXE" ::/ANYSYS.EXE
mcopy -o -i "$C" "$out/QUIT.COM" "$out/TBOOT.BIN" "$out/B622.BIN" ::/

python3 "$here/fatcheck.py" fixture "$out"

# ---- running ---------------------------------------------------------------

# qemu_run LOGDIR -- QEMU ARGS: runs until QUIT.COM or the timeout.
# Exit 1 is the clean stop; 124 is the timeout. Plain sh has no locals,
# so this uses its own variable names rather than the callers' $dir.
qemu_run() {
    qdir=$1; shift
    set +e
    timeout "$TIMEOUT" qemu-system-i386 -M pc -m 16 -display none \
        -device isa-debug-exit,iobase=0x501,iosize=0x01 \
        -serial file:"$qdir/serial.log" -no-reboot "$@" > "$qdir/qemu.log" 2>&1
    rc=$?
    set -e
    return $rc
}

# CTTY COM1 moves the console - stdout and stderr both - onto the serial
# port, which QEMU writes to serial.log. COMMAND.COM can only redirect
# stdout, and AnySYS reports every failure on stderr, so without this an
# error leaves nothing behind but a missing "DONE". CRLF throughout:
# COMMAND.COM does not accept bare LF line endings.
autoexec() {
    printf '@ECHO OFF\r\nCTTY COM1\r\n%s\r\nIF ERRORLEVEL 1 ECHO ANYSYS FAILED\r\nECHO ANYSYS DONE\r\nC:\\QUIT.COM\r\n' "$1"
}

# anysys CASE FLOPPY ARGS: run AnySYS on a copy of FLOPPY as A:.
anysys() {
    dir="$out/$1"; mkdir -p "$dir"
    cp "$out/$2" "$dir/floppy.img"
    autoexec "C:\\ANYSYS $3 A: /VERBOSE" > "$dir/AUTOEXEC.BAT"
    mcopy -o -i "$C" "$dir/AUTOEXEC.BAT" ::/
    if qemu_run "$dir" -boot c \
        -drive file="$out/boot.img",format=raw,index=0,media=disk \
        -drive file="$dir/floppy.img",format=raw,if=floppy,index=0; then rc=0; else rc=$?; fi
    tr -d '\r' < "$dir/serial.log" | grep -vE '^$|attrs ' | sed 's/^/    | /'
    [ $rc -eq 1 ] || { echo "    QEMU exit $rc (124 = timed out)"; return 1; }
    grep -q "ANYSYS DONE" "$dir/serial.log" && ! grep -q "ANYSYS FAILED" "$dir/serial.log"
}

# boots CASE: boot the case's floppy on its own and list the directory
# that was moved. Reaching the listing means the loader found the kernel
# at cluster 2 and DOS can read the relocated tree.
boots() {
    dir="$out/$1"; img="$dir/floppy.img"
    printf '@ECHO OFF\r\nCTTY COM1\r\nDIR A:\\SUB\r\nA:\\QUIT.COM\r\n' > "$dir/BOOT.BAT"
    mcopy -o -i "$img" "$dir/BOOT.BAT" ::/AUTOEXEC.BAT
    mcopy -o -i "$img" "$out/QUIT.COM" ::/
    mkdir -p "$dir/boot"
    if qemu_run "$dir/boot" -boot a -drive file="$img",format=raw,if=floppy,index=0; then rc=0; else rc=$?; fi
    if [ $rc -eq 1 ] && grep -q "F1 *BIN" "$dir/boot/serial.log" && grep -q "INNER" "$dir/boot/serial.log"; then
        echo "ok    floppy boots and lists A:\\SUB"
    else
        echo "FAIL  floppy did not boot to DOS (QEMU exit $rc); see $dir/boot/"
        return 1
    fi
}

# ---- cases -----------------------------------------------------------------

passed=0; failed=0
for case in $CASES; do
    echo "== $case"
    ok=1
    case $case in
    toshiba-fresh)
        anysys "$case" blank.img 'C:\SRCT /BOOTSRC:C:\TBOOT.BIN' || ok=0
        [ $ok = 1 ] && { python3 "$here/fatcheck.py" match "$out/$case/floppy.img" "$TOSHREF" || ok=0; }
        ;;
    toshiba-used)
        anysys "$case" used.img 'C:\SRCT' || ok=0
        [ $ok = 1 ] && { python3 "$here/fatcheck.py" verify "$out/$case/floppy.img" \
            "$out/files" "$out/src/T" TBIOS.SYS TDOS.SYS || ok=0; }
        ;;
    ms622-used)
        anysys "$case" used.img 'C:\SRCMS /BOOTSRC:C:\B622.BIN' || ok=0
        [ $ok = 1 ] && { python3 "$here/fatcheck.py" verify "$out/$case/floppy.img" \
            "$out/files" "$out/src/MS" IO.SYS MSDOS.SYS || ok=0; }
        [ $ok = 1 ] && { boots "$case" || ok=0; }
        ;;
    *)
        echo "unknown case: $case" >&2; ok=0
        ;;
    esac
    if [ $ok = 1 ]; then echo "PASS  $case"; passed=$((passed + 1)); else echo "FAIL  $case"; failed=$((failed + 1)); fi
done

echo
echo "$passed passed, $failed failed"
[ $failed -eq 0 ]
