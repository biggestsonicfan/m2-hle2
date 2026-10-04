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

# the sound: the PS3 ADX2 bank -> STF.AFS (~114 MB, ~2 minutes)
python3 dreamcast/tools/mksound.py "<PS3>/sound" /tmp/dc/STF.AFS

sh dreamcast/mkdisc.sh /tmp/dc "<PS3>/stf_rom" /tmp/dc/disc /tmp/dc/STF.AFS   # -> m2hle2.gdi
```

The disc is a three-track GDI. Track 3 holds IP.BIN, 1ST_READ.BIN (unscrambled),
the five ROM files as they ship and STF.AFS. `dc_layout.h` says where each ROM
file lands in the board's regions; what the PS3 files lack (part of the texture
ROM, the copro tables, the sound CPU's program and samples) is listed there.

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
- row 1: pager loads (and their ms), TLB refills, evictions, pinned frames,
  ROM pages written, read errors
- row 2: the profile and the page cache's size
- row 17: the decode's parts (tiles, 3D decode, sort) in ms; the mesh cache's
  meshes, builds and hits
- row 18: triangles drawn, projection runs, frames that hit the triangle cap;
  textures cut, new, dropped, failed
- row 19: sound on/off, codes trapped, codes not in the table, the music's
  entry, the ring's fill, underruns

If the board halts, the screen shows the IP, the pager's totals and the last
log lines.

Pad: D-pad, A/B/X/Y = B1-B4, Start, left trigger = coin.

## Files

| | |
|---|---|
| `main_dc.c` | the frontend: video, pad, run loop, the sound trap, stats |
| `dc_layout.h` | the PS3 ROM files → the board's regions |
| `dc_pager.h` | MMU demand paging of the ROM off the GD-ROM |
| `sfight.aotmap` | the i960 code STF runs, by address, for `AOT=` (`tools/i960_aot.py`) |
| `dc_pvr.h` | the picture on the PowerVR: tile layers, 3D, textures, stats text |
| `dc_sound.h` | STF.AFS, the ADX decoder, the music's ring, the SDL2 mix |
| `tools/mksound.py` | host tool: PS3 `stf_all.acb`/`.awb` (HCA) → STF.AFS (ADX) |
| `mkdisc.sh` | program + ROM files + STF.AFS → GDI |
| `../tests/rom_touch.c` | host tool: which ROM pages a game reads |
