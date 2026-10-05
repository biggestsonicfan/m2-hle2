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
# FRAME512=1 makes the frame 512x384 in the middle of the 640x480 signal
# (Flycast stretches it; DREAMCAST-PORT.md #471).
# HUD=min drops the stats rows: only the board's frames a second, small, top
# right (Pinboard #475's disc: HUD=min FRAME512=1). Under Flycast a FRAME512
# picture is enlarged and that corner falls off the screen; Redream shows it.
# HUD=none draws nothing over the game at all, not even that: with FRAME512=0
# the board's 496x384 sits 1:1 at (72,48) of the frame, a picture to crop and
# hold against MAME's pixel for pixel (Pinboard #478).
# FPS_CAP=60 (the default) holds the board to 60 frames a second, for Redream,
# whose SH-4 is faster than a Dreamcast's; FPS_CAP=0 takes the cap off.

# the sound: the PS3 ADX2 bank -> STF.AFS (~114 MB, ~2 minutes)
python3 dreamcast/tools/mksound.py "<PS3>/sound" /tmp/dc/STF.AFS

sh dreamcast/mkdisc.sh /tmp/dc "<PS3>/stf_rom" /tmp/dc/disc /tmp/dc/STF.AFS   # -> m2hle2.gdi
```

The disc is a three-track GDI. Track 3 holds IP.BIN, 1ST_READ.BIN (unscrambled),
the five ROM files as they ship, STF.AFS and MODELS.PAK. `dc_layout.h` says where each ROM
file lands in the board's regions; what the PS3 files lack (part of the texture
ROM, the copro tables, the sound CPU's program and samples) is listed there.

For an emulator or player that takes no GDI, `CDI=1` makes a self-booting CD-R
image instead, `m2hle2.cdi` (DiscJuggler; Pinboard #480): one audio/data CD with
the same files on its data track at LBA 11702 and 1ST_READ.BIN scrambled, as a
MIL-CD boots. It needs KOS's `scramble` and `cdi4dc` from img4dc, which is
written for Windows; `tools/build-cdi4dc.sh` builds a Linux one in
`~/build/tools/dc/img4dc`. The program finds its files on either disc (the
data track's TOC entry), so nothing else changes. Tested in Flycast's libretro
core and Redream.

```sh
sh dreamcast/tools/build-cdi4dc.sh                                    # once
CDI=1 sh dreamcast/mkdisc.sh /tmp/dc "<PS3>/stf_rom" /tmp/dc/disc /tmp/dc/STF.AFS   # -> m2hle2.cdi
```

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
against MAME frame by frame over the serial port. The same disc, under
`dc-lockstep.py --boot`, plays from power-on instead and is held to MAME and to
the desktop build from the first frame. The disc needs `SINCOS=`
(`tools/mksincos.py`), and Flycast needs `tools/flycast-scif.patch`.
DREAMCAST-PORT.md, "Held against MAME over the serial port", has the commands
and what it found.

## m2-pacman

The disc also runs homebrew: put a program in place of `rom_code1.bin` and keep
the other four files. [m2-pacman](https://github.com/biggestsonicfan/m2-pacman)'s
`roms/pacman_web/game.bin` (the build with no SHARC and an idle `sinr`) is the
one to use; the SHARC build runs too, slower.

```sh
mkdir /tmp/pac && for f in rom_data rom_ep rom_pol rom_tex; do ln -s "<PS3>/stf_rom/$f.bin" /tmp/pac/; done
ln -s <m2-pacman>/roms/pacman_web/game.bin /tmp/pac/rom_code1.bin
make -C dreamcast OUT=/tmp/dcpac AOT_MAP=pacman.aotmap AOT=<m2-pacman>/roms/pacman_web/game.bin
sh dreamcast/mkdisc.sh /tmp/dcpac /tmp/pac /tmp/dcpac/disc
```

`main_dc.c` runs `profile_adopt_program` on the disc's program, as the desktop
does, so homebrew gets `sfight_homebrew` and none of STF's hooks; Gems' traps
are STF's code and stay off (`gems.h`). There is no sound board, so the UART's
status reads TxRDY and TxEMPTY with nothing to receive (`dc_uart_read`): the
game's ping goes unanswered, it says NO SOUND, and plays on.

`pacman.aotmap` is 4000 frames of the game's attract, recorded like
`sfight.aotmap` with a `det_digest` that leaves the sound board out, as the disc
does. Its counts are flatter than STF's: `AOT_COVER` is 0.9 for it by default
(14,600 instructions, 2.4 MB of text), since 0.99 (33,000) runs the board out
of RAM.

Measured in Flycast (Pinboard #463), attract, a frame's budget being 17 ms:

| | fps | i960 / slice | tiles / frame |
|---|---|---|---|
| SHARC build, interpreted | 5.6 | 110 ms | |
| `pacman_web`, interpreted | 6.4 | 85 ms | 69 ms |
| `pacman_web`, AOT 0.9 | 9.3 | 37 ms | 69 ms |
| `pacman_web`, AOT 0.9, tiles by char | 24.5 | 36 ms | 3 ms |

m2-sdk's tile framebuffer gives every screen cell a char of its own and draws
the sprites into the chars, so char RAM changes in every frame anything moves.
`dp_tiles` used to redraw the whole screen for any such change; it now hashes
the chars of the KBs written (`gfx_dirty`, `memory.h`) and redraws the cells
that show one that changed. The i960 is what is left.

## Homebrew discs: pacman_geo and m2-sonic

m2-pacman's `pacman_geo` and [m2-sonic](https://github.com/biggestsonicfan/m2-sonic)
draw their sprites as polygons through the GEO, so they have discs of their own
(Pinboard #469). Build the game with its debug panel off, and the disc with
`HUD=min`: nothing on screen but the board's frames a second, small, top right.

```sh
# m2-pacman, panel off: roms/pacman_geo/game.bin
cmake -B /tmp/bpac -S <m2-pacman> -DM2_GAME=pacman_geo -DM2_SDK=<m2-sdk> -DPAC_DEFS=PAC_NO_PANEL && cmake --build /tmp/bpac
ln -s <m2-pacman>/roms/pacman_geo/game.bin /tmp/pac/rom_code1.bin   # + the four PS3 files, as above
make -C dreamcast OUT=/tmp/dcpac HUD=min AOT_MAP=pacman_geo.aotmap AOT=/tmp/pac/rom_code1.bin
sh dreamcast/mkdisc.sh /tmp/dcpac /tmp/pac /tmp/dcpac/disc

# m2-sonic, panel off: roms/sonic/game.bin (needs your cartridge, see its README)
cmake -B /tmp/bson -S <m2-sonic> -DM2_SDK=<m2-sdk> -DSONIC_DEFS=SONIC_NO_PANEL && cmake --build /tmp/bson
```

m2-sonic keeps the Mega Drive cartridge in the two data EPROMs, so its disc's
`rom_ep.bin` is those, not the PS3 file: `epr-19003.7` and `epr-19004.8`
interleaved a 16-bit word at a time (1 MB; past the cartridge it is 0xFF, so
the disc's layout is unchanged). The other three PS3 files stay. Its AOT map
holds words of the recompiled cartridge, so it is not in the repo: record your
own with the sound-less `det_digest` (`--aot-map 0:6000:sonic.aotmap`, the
program from `roms/sonic`) and build with `AOT_MAP=sonic.aotmap AOT_COVER=0.95`.

Two things in `dc_pvr.h` were needed for them:

- `m2_sprite.h`'s atlas is a 512x512 tile, past the PVR's 256. `dp_big_window`
  draws a face from the 256-or-smaller window of the tile its coordinates fall
  in.
- m2-sonic's palettes are the Mega Drive's, sixteen colours that no
  base-times-grey-plus-offset line fits. For homebrew (`any_program`), a face
  the line misses by more than 24 a channel gets a 16-colour PVR palette bank
  of its own (banks 3-62, two halves used on alternate frames so a bank is not
  rewritten while the last frame's list still reads it).
- Those faces are point sampled. `m2_sprite.h` draws a sprite 1:1, and
  bilinear filtering pulled in the texels past the quad's edge and the
  hole's black: a dark border round every m2-sonic sprite.

Neither game has sound here: there is no sound board, so the ping goes
unanswered (see above). Measured in Flycast's libretro core, attract, by
the disc's own counter (the Dreamcast's timer):

| | board fps |
|---|---|
| `pacman_geo`, AOT 0.95 | 28-31 |
| m2-sonic, AOT 0.95 | 8-11 (the game drops Mega Drive frames to keep time) |

Count by the Dreamcast's clock, not the host's. Under RetroArch on Xvfb
the guest ran about 2.5 times faster than the host's clock: m2-sonic's frame count
went up 28 a host second where the timer says 11, and read that way the two
discs gave ~37 and ~30.

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
| `dc_link.h` | `LINK=1`: the replay fight's frames, or from power-on the board's memory as CRCs, over the SCIF to `tools/dc-lockstep.py` |
| `tools/mksincos.py` | host tool: the arcade set's copro ROM → SINCOS.BIN, the COP's sin/cos for the link |
| `tools/flycast-scif.patch` | Flycast: the SCIF over TCP (`FLYCAST_SCIF=host:port`) |
| `mkdisc.sh` | program + ROM files + STF.AFS + MODELS.PAK → GDI (`CDI=1`: CDI) |
| `tools/build-cdi4dc.sh` | a Linux cdi4dc (img4dc) for `CDI=1` |
| `../tests/rom_touch.c` | host tool: which ROM pages a game reads |
