# AnySYS

AnySYS is an experimental "(Almost) Universal SYS Command" for 16-bit DOS.
It is written in C89-style Watcom C for Open Watcom / Watcom C targeting a
real-mode DOS EXE.

The program is intended to make an already formatted FAT volume bootable by:

1. copying DOS system files to the target root directory;
2. optionally installing boot code from a 512-byte boot sector template while
   preserving the target volume's BPB/EBPB;
3. supporting backup, dump, and exact restore of a target volume boot sector.

This repository intentionally does not vendor boot sector code from another
project yet. Use `/BOOTSRC:file` with a boot sector template from the DOS
family you want to install.

## Status

This is a first working skeleton. It includes:

- OEM profiles for FreeDOS, MS/Win9x DOS, PC-DOS, DR-DOS-style file names,
  and Toshiba's MS-DOS 2.11 OEM build (`TBIOS.SYS`/`TDOS.SYS`).
- `/OEM:AUTO`, `/OEM:FD`, `/OEM:MS`, `/OEM:W9X`, `/OEM:PC`, `/OEM:DR`,
  `/OEM:EDR`, `/OEM:TOSHIBA`.
- `/BOOTONLY`, `/UPDATE`, `/DRYRUN`, `/VERBOSE`.
- `/K:name`, `/SKFN:name`, `/SCFN:name`.
- `/BOOTSRC:file`, `/BACKUPBS:file`, `/DUMPBS:file`, `/RESTORBS:file`.
- BPB-preserving boot-code transplant for FAT12/16 and FAT32 boot sectors.
  Only the BPB the template has room for is carried over: the copy stops at
  the template's jump target, so pre-DOS 4 loaders that start before 3Eh
  (Toshiba 2.11 starts at 24h) are not overwritten.
- FAT12/16 raw placement for MS-DOS, PC-DOS, and DR-DOS-style kernel pairs:
  the first two root directory entries are reserved for the two kernel files,
  the first kernel always starts at cluster 2 (DOS 2.x/3.x loaders read it
  from the start of the data area), and each kernel file is written to a
  contiguous FAT chain.

Raw placement currently supports FAT12 and FAT16 only. It frees old root-level
copies of the target kernel files, moves unrelated short root entries out of
slots 0 and 1 when possible, moves any file data occupying the clusters the
first kernel needs (relinking FAT chains and repointing directory entries,
including `.` and `..` of moved directories), writes the new kernel data
contiguously, updates all FAT copies, then writes fresh hidden/system/read-only
root entries. It refuses if one of those clusters is marked bad or is in use
without belonging to any file (run CHKDSK /F first). It does
not move long-filename entry sets, and it refuses to use the target root itself
as the source directory during raw replacement. Copy source system files to a
different drive or subdirectory first for that case.

The low-level sector I/O path uses DOS INT 25h/26h wrappers when built with
Watcom for DOS. Test this first in PCem, 86Box or QEMU, or on a disk image,
before touching real media. Do not trust DOSBox for it: it accepts boot
sector writes on image-mounted drives without saving them, and AnySYS still
reports success.

`tools/qemu/run.sh` runs AnySYS under real MS-DOS on QEMU against generated
floppies and checks the result; see `tools/qemu/README`. See `TESTING.md` for
the manual compatibility matrix.

## Build

With Open Watcom available in the DOS or Windows environment:

```bat
Build_AnySYS_Watcom.bat
```

or:

```bat
wmake -f makefile.wat
```

The output is `AnySYS.exe`.

## Usage

```text
ANYSYS [source] drive: [bootsect.img] [options]
ANYSYS drive: /DUMPBS:file
ANYSYS drive: /RESTORBS:file
```

Examples:

```text
ANYSYS C:\FDOS\BIN A: /OEM:FD /BOOTSRC:FDBOOT.BIN /BACKUPBS:A.BAK
ANYSYS A: ABOOT.BIN /OEM:FD /BOOTSRC:FDBOOT.BIN
ANYSYS C: /DUMPBS:CBOOT.BIN
ANYSYS A: /RESTORBS:A.BAK
ANYSYS C:\DOS C: /OEM:MS /UPDATE /DRYRUN /VERBOSE
```

If the optional `bootsect.img` positional argument is supplied, AnySYS writes
the final boot sector image to that file instead of the target boot sector.
System-file installation still operates on the target drive.

## References

Behavioral targets and technical references used while writing original code:

- FreeDOS SYS command help: https://help.fdos.org/en/hhstndrd/base/sys.htm
- FreeDOS SYS repository and GPL-2.0 license: https://github.com/FDOS/sys
- HelpPC INT 25h / INT 26h descriptions: https://stanislavs.org/helppc/int_25.html
  and https://stanislavs.org/helppc/int_26.html
- Microsoft KB note on INT 25h/26h stack behavior:
  https://ftp.zx.net.nz/pub/Patches/ftp.microsoft.com/MISC/KB/en-us/48/744.HTM

## License

AnySYS is offered under the GNU General Public License, version 2 or later.
The full license text is in `LICENSE`; see `COPYING` for the summary.
