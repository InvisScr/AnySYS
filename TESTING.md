# AnySYS Test Log

Automated checks live in `tools/qemu/` (`tools/qemu/run.sh`): real DOS on
QEMU against generated floppies, checking kernel placement, file survival,
filesystem consistency and, for MS-DOS 6.22, that the result boots. Run it
after any change to raw placement, the boot-sector merge or sector I/O.

This file records manual compatibility tests. A `+` means the target booted
successfully. A `-` means the target did not boot in that test. A `?` means
the result is inconclusive because some outside factor may explain the failure,
such as disk size, memory size, BIOS behavior, or DOS version limits.

Unless otherwise stated, the hard drive was prepared with:

```text
FORMAT C:
```

## AnySYS 1

### PCem Slot 1 CPU

| Test | Result |
| --- | --- |
| MS-DOS 6.22 from MS-DOS 6.22 | + |
| MS-DOS 5.0a from MS-DOS 6.22 | + |
| MS-DOS 4.01 from MS-DOS 6.22 | + |
| MS-DOS 5.0a from MS-DOS 4.01, no pre-format | + |
| MS-DOS 3.3a from MS-DOS 5.0a, no pre-format | ? |
| MS-DOS 3.3a from MS-DOS 6.22 | ? |

### PCem 386DX

| Test | Result |
| --- | --- |
| MS-DOS 3.3a from MS-DOS 3.3a | + |
| MS-DOS 5.0a from MS-DOS 3.3a, no pre-format | - |
| MS-DOS 5.0a from MS-DOS 3.3a | ? |
| MS-DOS 4.01 from MS-DOS 6.22, no pre-format | ? |
| MS-DOS 4.01 from MS-DOS 6.22 | + |

## Notes

- Source files `IO.SYS`, `MSDOS.SYS`, and `COMMAND.COM` may need further
  testing when they have read-only, hidden, or system attributes in the source
  directory.
- In the tests above, those source attributes were removed so the files could
  be copied, and were not reset afterward.
- AnySYS currently clears attributes on destination files before overwriting
  them and sets system/hidden/read-only attributes on copied kernel files. It
  does not intentionally modify source file attributes.
