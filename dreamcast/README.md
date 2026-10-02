# m2-hle2 for the Dreamcast

The board under KallistiOS, with the ROM read off the disc a page at a time.
Milestone D1: tile layers only, no 3D and no sound. See
[../DREAMCAST-PORT.md](../DREAMCAST-PORT.md) for what was measured and why it
is built this way.

## What you need

- A KallistiOS toolchain (sh-elf GCC and KOS, built by KOS's `utils/dc-chain`).
  Source its `environ.sh`.
- `genisoimage`, and KOS's `utils/makeip` built.
- A vendor/ with miniz (the repo's submodule).
- Your own ROM set. Neither the pack nor the disc is ever committed.

## Build

From the repo root:

```sh
. $KOS_BASE/environ.sh
make -C dreamcast OUT=/tmp/dc                    # VENDOR=<dir> if not ./vendor

# the pack, built on the host from your set (build line in m2pack.c's header)
cc -O2 -std=gnu11 -DM2HLE_VERSION='"dev"' -DM2HLE_DEV_TOOLS=0 \
   -Isrc -Isrc/board -Isrc/core -Isrc/ui -Isrc/profiles -Isrc/net \
   -Ivendor/sokol -Ivendor/miniz -Idreamcast dreamcast/m2pack.c vendor/miniz/miniz*.c \
   -o /tmp/dc/m2pack -lpthread -lm
/tmp/dc/m2pack $ROMS_DIR/sfight.zip /tmp/dc/M2PACK.BIN

sh dreamcast/mkdisc.sh /tmp/dc /tmp/dc/M2PACK.BIN /tmp/dc/disc   # -> m2hle2.gdi
```

The disc is a three-track GDI. Track 3 holds IP.BIN, 1ST_READ.BIN (unscrambled)
and M2PACK.BIN. miniz needs a stub `miniz_export.h`; the Makefile writes it
into `$(OUT)/gen`.

## Run

Load `m2hle2.gdi` in Flycast (standalone or the libretro core) or burn it.
It was tested in the libretro core (commit 04669eb) with the core option
`reicast_hle_bios = "enabled"` and no BIOS files. A real BIOS has not been
tried. On screen:

- row 0: board frame, fps, the i960 slice's and the tile compose's ms
- row 1: pager loads (and their ms), TLB refills, evictions, pinned frames,
  ROM pages written, read errors

If the board halts, the screen shows the IP, the pager's totals and the last
log lines.

Pad: D-pad, A/B/X/Y = B1-B4, Start.

## Files

| | |
|---|---|
| `main_dc.c` | the frontend: video, pad, run loop, stats |
| `dc_pager.h` | MMU demand paging of the ROM off the GD-ROM |
| `dc_pack.h` | the pack's format |
| `m2pack.c` | host tool: ROM set → pack (deduplicated pages) |
| `mkdisc.sh` | pack + program → GDI |
| `../tests/rom_touch.c` | host tool: which ROM pages a game reads |
