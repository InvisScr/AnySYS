# AnySYS

AnySYS is an "(Almost) Universal SYS Command" for 16-bit DOS. It is written
in C89-style Watcom C for Open Watcom / Watcom C targeting a real-mode DOS
EXE.

It is a drop-in replacement for DOS's own `SYS`, used the same way:

```text
ANYSYS A: C:          system files from A: to C:
ANYSYS C:\DOS6 A:     system files from the folder C:\DOS6 to A:
```

On top of that familiar form, AnySYS expands what `SYS` can do:

- **Any folder as the source.** The system files can come from any directory,
  not just the root of a boot disk, so one disk can hold the files for many
  DOS versions side by side.
- **More operating systems.** One program handles MS-DOS, PC-DOS, DR-DOS,
  FreeDOS, Windows 9x and Me, and OEM builds such as Toshiba's MS-DOS 2.11,
  instead of each DOS needing its own `SYS` (see `/OEM`).
- **More ways to do the job.** Boot code can come from a separate template,
  the boot sector can be backed up, dumped or restored exactly, and a dry run
  shows what would happen without writing anything.
- **More reliable results.** For the oldest DOS versions, which need their
  system files in an exact spot on the disk, AnySYS moves any files that are
  in the way instead of failing with "No room for system on destination disk".

AnySYS is a program in its own right. Because it was a good fit,
[AnyDOS](https://github.com/InvisScr/AnyDOS), the universal DOS installer,
later adopted it for its system-transfer step and runs it as a separate
program.

In detail, the program makes an already formatted FAT volume bootable by:

1. copying DOS system files to the target root directory;
2. optionally installing boot code from a 512-byte boot sector template while
   preserving the target volume's BPB/EBPB;
3. supporting backup, dump, and exact restore of a target volume boot sector.

This repository intentionally does not vendor boot sector code from another
project yet. Use `/BOOTSRC:file` with a boot sector template from the DOS
family you want to install.

## Status

Version 1.0 is the first release. It has been tested in emulators only, not
yet on a real period PC. It includes:

- OEM profiles for FreeDOS, MS/Win9x DOS, PC-DOS, DR-DOS-style file names,
  Toshiba's MS-DOS 2.11 OEM build (`TBIOS.SYS`/`TDOS.SYS`), and MS-DOS 8.0
  (Windows Me), whose kernel is `IO.SYS` alone.
- `/OEM:AUTO`, `/OEM:FD`, `/OEM:MS`, `/OEM:W9X`, `/OEM:PC`, `/OEM:DR`,
  `/OEM:EDR`, `/OEM:TOSHIBA`, `/OEM:ME`.
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
not move long-filename entry sets.

When the source is on the target drive - including its root, as with
`ANYSYS C:` after booting from C: - AnySYS reads the system files into memory
before it changes anything on the disk, and writes them from there. A file
that would be copied onto itself, such as `COMMAND.COM` in that case, is left
as it is.

The low-level sector I/O path uses DOS INT 25h/26h wrappers when built with
Watcom for DOS. Test this first in PCem, 86Box or QEMU, or on a disk image,
before touching real media. Do not trust DOSBox for it: it accepts boot
sector writes on image-mounted drives without saving them, and AnySYS still
reports success.

`tools/qemu/run.sh` runs AnySYS under real MS-DOS on QEMU against generated
floppies and checks the result; see `tools/qemu/README`. See `TESTING.md` for
the manual compatibility matrix.

## Build

With Open Watcom 2.0 set up (`WATCOM`, `INCLUDE=%WATCOM%\h` and its binaries
on `PATH`), from the repository root.

On Linux:

```sh
wmake -h -f makefile.wat
```

On DOS or Windows:

```bat
wmake -f makefile.wat
```

or, on Windows only:

```bat
Build_AnySYS_Watcom.bat
```

The output is `AnySYS.exe`; `wmake -f makefile.wat clean` removes it.

## Usage

```text
ANYSYS [source] drive: [bootsect.img] [options]
ANYSYS drive: /DUMPBS:file
ANYSYS drive: /RESTORBS:file
```

The source is a drive or a folder holding the system files and COMMAND.COM.
As with `SYS`, a drive on its own means its root (`A:` is `A:\`), and a
source left out means the root of the drive DOS booted from (DOS 4.0 and
later; earlier DOS versions report no boot drive, so the current drive's
root is used, as their `SYS` did). With `/OEM` left on `AUTO`, AnySYS
picks the DOS family from the system files it finds there.

The same form as `SYS`:

```text
ANYSYS A: C:
ANYSYS C:\DOS6 A:
```

With the expanded options:

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
