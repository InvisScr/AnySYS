# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

AnySYS is an (almost) universal `SYS` command for 16-bit DOS: it copies a DOS version's kernel files onto a FAT volume and optionally installs boot code, aiming to work across every DOS family. Its whole reason to exist is that it works with most DOS versions without the version-specific `SYS.COM` for the DOS currently running - lead with that when describing it. It is a single file, `src/anysys.c` (GPL-2.0-or-later, C89 style), built with OpenWatcom. It is a standalone program that predates AnyDOS. **AnyDOS** (the universal DOS installer, repository `InvisScr/AnyDOS`) later adopted it because it was a good fit, and spawns it for its system-transfer step. AnyDOS is not GPL, so the two stay separate programs and code must not move between them. Describe AnySYS as its own program that AnyDOS uses, never as a part made for AnyDOS.

## Build

OpenWatcom 2.0 is at `~/opt/openwatcom`; `~/.bashrc` sets `WATCOM`, `INCLUDE=$WATCOM/h`, and puts `binl64` on `PATH`.

```sh
wmake -h -f makefile.wat          # or: wmake -h -f makefile.wat clean
```

`makefile.wat` works on both Linux and DOS/Windows hosts (it switches on `__UNIX__` for `rm`/`del` and the `.o`/`.obj` name); `Build_AnySYS_Watcom.bat` is the Windows-only wrapper. The build is kept warning-clean at `-w4`. AnyDOS's `build.sh` copies `AnySYS.exe` into its `dist/`, so rebuild AnyDOS after changing this.

## Tests

**`tools/qemu/run.sh`** - run after any change to raw placement, the boot-sector merge or sector I/O; expect `6 passed, 0 failed`. It boots real MS-DOS 6.22 in QEMU from a copy of the reference disk in `HDD for AnyDOS/` (outside this tree; override with `REF622=` / `TOSHREF=`), runs AnySYS against generated floppies, and checks kernel placement at cluster 2, survival of every relocated file, `fsck.fat`, a byte match with Toshiba's original disk, that the 6.22 result boots, and that the source defaults match `SYS` (a bare drive is its root; no source is the current drive's root, as checked against real SYS 4.01-6.22), and that `ANYSYS A: A:` works with source and target the same root. `EXE=` tests another build; `tools/qemu/README` explains each case. AUTOEXEC runs `CTTY COM1` so AnySYS's stderr reaches `out/<case>/serial.log` - COMMAND.COM cannot redirect stderr, and every AnySYS failure is reported there.

**DOSBox is not a valid test bed**: it accepts INT 26h boot-sector writes on imgmounted drives without saving them, and AnySYS then reports success.

`TESTING.md` is a manual compatibility matrix from PCem/emulator boots (`+` boots, `-` fails, `?` inconclusive) - add rows there when testing by hand.

## Structure of `anysys.c`

Argument parsing into `Options` → `dos_login_drive()` → OEM profile selection (`/OEM:AUTO|FD|MS|W9X|PC|DR|EDR|TOSHIBA|ME`, which determines the kernel file names; AUTO picks the first profile whose files exist in the source) → system files → optional boot sector.

- **System files:** FreeDOS and MS-DOS 8.0 (`ME`, IO.SYS only) are a plain file copy, and so is every kernel pair on a FAT32 target (`target_is_fat32`); for the IBMBIO.COM profiles each kernel's chain is then ended with FFFFFFFFh (`fat32_end_chain_ff`), since DR-DOS 8.0's IBMBIO.COM stops at nothing else. Two-kernel profiles use **raw placement** on FAT12/16 (`raw_install_system_kernels`): free old kernel entries, empty root entries 0 and 1, clear clusters from 2 for the first kernel (`clear_cluster_run` relocates whatever occupies them - FAT chains relinked, directory entries and moved directories' `.`/`..` repointed; refuses on bad or ownerless clusters), place the second kernel in any contiguous run, update every FAT copy, then write hidden/system/read-only entries. DOS 2.x/3.x loaders need exactly this layout. When the source is on the target drive (even its root, as with `SYS C:` from C:), `preload_kernel` reads both kernels into DOS memory blocks before the disk changes; files that would be copied onto themselves are skipped (`same_file`, via INT 21h AH=60h).
- **Boot sector:** `/BOOTSRC:` transplants a template's boot code while keeping the target's BPB (`merge_boot_template`); the BPB copy stops at the template's jump target so pre-DOS 4 loaders (Toshiba 2.11 starts at 24h) are not overwritten. A 1536-byte template is a FAT32 one (`read_boot_template`): sector 2 and the backup copies are written too (`write_fat32_extras`), and FAT32 BPB bytes 34h-3Fh stay the template's (DR-DOS 8.0 keeps "IBMBIO  COM" there). Also `/BACKUPBS: /DUMPBS: /RESTORBS:`.
- **Sector I/O:** INT 25h/26h wrappers under `#if __DOS__` (16-bit and DOS 4+ packet forms); on DOS 7+ INT 21h 7305h is tried first (`dos_ext_abs_io`; DR-DOS 8.0 lacks it and answers AX=7300h), and a write first locks the target (`dos_lock_for_write`, level 0, released at exit) - MS-DOS 7+ silently drops an unlocked direct write. Test carry *before* discarding the flags word DOS leaves on the stack. FAT reads go through a two-sector cache (`fat_sector`) that every FAT write drops. Inline `_asm` cannot see a C variable named after an instruction (`out`, `in`, ...) - it is a syntax error, and `wmake` has been seen to hang on it instead of failing (run `wcl` directly to see the error).
- **`dos_login_drive()` must run before any INT 25h/26h**: until something makes DOS read the disk, absolute I/O can use the drive's default geometry - an untouched 720K floppy was addressed as single-sided, so every sector from 9 up (i.e. all file data) came from the wrong track while the boot sector, FAT and first root sector read correctly.

Other switches: `/BOOTONLY /UPDATE /DRYRUN /VERBOSE /K: /SKFN: /SCFN:`.
