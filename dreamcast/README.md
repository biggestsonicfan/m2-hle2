# m2-hle2 for the Dreamcast

The board under KallistiOS, with the ROM read off the disc a page at a time,
straight from the PS3 release's ROM files (no zip, no interleave: they are
already the board's address spaces). The PowerVR draws the tile layers and the
3D scene, and sound is Sega's console way: ADX cues out of an AFS, the music
decoded on the SH-4 into a KOS stream, the effects played by the AICA. See
[../DREAMCAST-PORT.md](../DREAMCAST-PORT.md) for what was measured and why it
is built this way.

## What you need

- A KallistiOS toolchain: sh-elf GCC built by KOS's `utils/kos-chain`
  (`make` in that directory; `makejobs=2` in its `Makefile.cfg` here), then KOS
  itself and `utils/makeip`. Source KOS's `environ.sh`.
- `genisoimage`; on the host, `ffmpeg` (with the `hca` decoder and the
  `adpcm_adx` encoder) and Python 3 with numpy, for the sound.
- Your own copy of the PS3 release's `stf_rom` and `sound` folders.
  Neither its files nor anything made from them is ever committed.

## Build

From the repo root:

```sh
. $KOS_BASE/environ.sh
make -C dreamcast OUT=/tmp/dc
# or with STF's i960 code compiled ahead of time (~1.5x the i960's speed;
# AOT_COVER=0.98 by default, DREAMCAST-PORT.md #394)
make -C dreamcast OUT=/tmp/dc AOT="<PS3>/stf_rom/rom_code1.bin"
# GEMS=<dir> (default: ../Sonic Gems Collection/m2hle beside this checkout, if
# it is there) builds in Sega's C from Sonic Gems Collection; GEMS= leaves it
# out. OPTAB=1 dispatches the interpreter through a handler table (off: it
# saves under 1% here and costs 136 KB).

# the sound: the PS3 ADX2 bank -> STF.AFS (~114 MB, ~2 minutes)
python3 dreamcast/tools/mksound.py "<PS3>/sound" /tmp/dc/STF.AFS

sh dreamcast/mkdisc.sh /tmp/dc "<PS3>/stf_rom" /tmp/dc/disc /tmp/dc/STF.AFS   # -> m2hle2.gdi
```

The disc is a three-track GDI. Track 3 holds IP.BIN, 1ST_READ.BIN (unscrambled),
the five ROM files as they ship, STF.AFS and MODELS.PAK. `dc_layout.h` says where each ROM
file lands in the board's regions; what the PS3 files lack (part of the texture
ROM, the copro tables, the sound CPU's program and samples) is listed there.

MODELS.PAK is made by `mkdisc.sh` from `rom_pol.bin`, `rom_tex.bin` and
`sfight.mdlmap` (`tools/dc_mdlpack.py`): the meshes and UV streams the game
reads, laid out in the order it first reads them, so a scene comes off the disc
in a few pages. `NOPAK=1` leaves it out and everything is read from the ROM.
The map holds addresses only. To record it again with a desktop `det_digest`:

```sh
det_digest sfight.zip --profile sfight_console --frames 6000 --model-map 0:6000:att.mdl --out /dev/null
det_digest sfight.zip --profile sfight_console --frames 9000 --script "$S" --model-map 0:9000:fit.mdl --out /dev/null
```

then keep attract's lines and add the fight's that attract did not read, their
frames +100000. `$S` is `520:s,530:,640:1,650:,700:1,710:` (start, pick,
confirm) and then, from frame 900 to 9000, a new input every 30 frames: the
next of r1, l2, d3, u1, r2, 13, l, 4, r, with nothing held between them (the
cycle's place is `(frame / 30) % 18`). `dc_mdlpack.py` starts a new run where
the first frame jumps by more than 30.

`sfight.aotmap`, the code `AOT=` compiles, is recorded the same way, with
Sega's C on as the disc runs it (a `det_digest` built with `M2HLE_GEMS_DIR`):

```sh
det_digest sfight.zip --profile sfight_console --gems --frames 6000 --aot-map 0:6000:att.raw --out /dev/null
det_digest sfight.zip --profile sfight_console --gems --frames 3200 --script "$S" --aot-map 0:3200:fit.raw --out /dev/null
```

then each instruction's weight is its count over its run's total, times
10^9 / 2, summed over the two runs; the hooks of both are kept.

## Sound

The i960 never reaches the sound board. A trap at `sound_request_special`
(`0x3F268`, added by `main_dc.c` to a copy of the profile) takes the code from
g0, as Sega's console DLL does, and `dc_sound.h` plays it:

- `STF.AFS` is a CRI AFS. Entry 0 is `CUES.BIN`, Sega's code table (from the
  DLL, `0x180126a70`, plus its eight per-cue stop codes `0xAE14xx`); the rest
  are ADX v3 files with their loops, one per cue. Format in `mksound.py`.
- Music (category 5) is 44.1 kHz stereo, streamed: the main loop reads it
  through the pager's drive calls into a 128 KB ring, and a thread feeds a
  KOS stream (`snd_stream`) from it. Effects and voices (category 2, 22 kHz
  mono) are decoded at boot and written again as the AICA's 4-bit ADPCM into
  sound RAM (~1 MB); each plays on an AICA channel of its own, 16 at most. A
  channel holds 65534 samples, so the four effects longer than that play at
  half their rate.
  `0xA00001`-`3` stop, `0xA003xx` fades the music over `xx` frames.

## Run

Load `m2hle2.gdi` in Flycast (standalone or the libretro core) or burn it.
It was tested in the libretro core with the core option
`reicast_hle_bios = "enabled"` and no BIOS files. A real BIOS has not been
tried. On screen:

- row 0: board frame, fps (and frames shown), the i960 slice's ms, the
  picture's ms (decode + submit)
- row 1: pager loads (and their ms), slow-path faults, evictions, pinned frames,
  ROM pages written, read errors
- row 2: the profile and the page cache's size
- row 17: the decode's parts (tiles, 3D decode, sort) in ms; the mesh cache's
  meshes, builds and hits
- row 18: triangles drawn, projection runs, frames that hit the triangle cap;
  textures cut, new, dropped, failed
- row 19: sound on/off, codes trapped, codes not in the table, the music's
  entry, the ring's fill, underruns

- rows 3-5 (once the board passes frame 3900): the bench, frames 3500-3900
  of attract: ms in all, in the slice and in the draw; the draw's parts; page
  loads in all, then of code, data, polygons, textures, the model pack, and
  those for `dc_rom_at`

If the board halts, the screen shows the IP, the pager's totals and the last
log lines.

Pad: D-pad, A/B/X/Y = B1-B4, Start, left trigger = coin.

## Linked to MAME

`make LINK=1` builds the replay fight for `tools/dc-lockstep.py`, which holds it
against MAME frame by frame over the serial port. The disc needs `SINCOS=`
(`tools/mksincos.py`), and Flycast needs `tools/flycast-scif.patch`.
DREAMCAST-PORT.md, "Held against MAME over the serial port", has the commands
and what it found.

## Files

| | |
|---|---|
| `main_dc.c` | the frontend: video, pad, run loop, the sound trap, stats |
| `dc_layout.h` | the PS3 ROM files → the board's regions |
| `dc_pager.h` | demand paging of the ROM off the GD-ROM, in software; the model pack |
| `sfight.aotmap` | the i960 code STF runs, by address, for `AOT=` (`tools/i960_aot.py`) |
| `dc_pvr.h` | the picture on the PowerVR: tile layers, 3D, textures, stats text |
| `dc_sound.h` | STF.AFS, the ADX decoder, the music's ring, the SDL2 mix |
| `tools/mksound.py` | host tool: PS3 `stf_all.acb`/`.awb` (HCA) → STF.AFS (ADX) |
| `sfight.mdlmap` | the polygon and texture ROM lines STF draws, by first frame, for MODELS.PAK |
| `../tools/dc_mdlpack.py` | host tool: map + ROM files → MODELS.PAK |
| `dc_link.h` | `LINK=1`: the replay fight's frames over the SCIF to `tools/dc-lockstep.py` |
| `tools/mksincos.py` | host tool: the arcade set's copro ROM → SINCOS.BIN, the COP's sin/cos for the link |
| `tools/flycast-scif.patch` | Flycast: the SCIF over TCP (`FLYCAST_SCIF=host:port`) |
| `mkdisc.sh` | program + ROM files + STF.AFS + MODELS.PAK → GDI |
| `../tests/rom_touch.c` | host tool: which ROM pages a game reads |
