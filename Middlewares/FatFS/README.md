# FatFs middleware

This directory vendors the official ChaN FatFs R0.15a source package.

- `ff.c`, `ff.h`, `ffconf.h`, and `diskio.h` are kept as the upstream
  filesystem layer.
- `ffconf.h` is configured for one fixed 512-byte FAT/FAT32 volume, no LFN,
  no exFAT, no heap allocation, and no re-entrant/RTOS support.
- The physical media adapter is `App/Storage/sd_diskio.c`; it is not part of
  FatFs itself.
- `ffunicode.c`, `ffsystem.c`, and the upstream example `diskio.c` are not
  built because the selected configuration does not require them.

Upstream source: https://elm-chan.org/fsw/ff/
Revision: FatFs R0.15a, 2024-11-22

The upstream license is preserved in `LICENSE.txt`.
