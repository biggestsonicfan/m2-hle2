---
name: mame
description: Run MAME as the ground truth for this emulator — find or build it, launch it headless or in a window with the right flags, drive it with an autoboot Lua script, and feed the graders. Use whenever a change must be checked against the real board (pictures, COP replies, fighter state, sound), when asked to "grade in MAME", "check against MAME", "take a MAME capture", or to show the owner something in MAME.
---

# MAME, the board's stand-in

MAME's `model2.cpp` (sfight is Model 2B) is the oracle: its i960, SHARC and
software renderer do what the board does. Nothing is "verified" until it has been
held against MAME. A picture that looks right in our emulator alone proves nothing.
Say in the PR whether MAME was used, and how.

## Always `-nodrc`

Every MAME launch takes `-nodrc`, windowed or headless, scripted or by hand. It
runs the SHARC (`:copro_adsp`, the geometry coprocessor) on the interpreter.
`mame.ini` has `drc 1`, and with the recompiler stock sfight stops on
CO-PROCESSOR ERROR (`co_processor_error_hang`). All graders pass it. Pass it
yourself on any other command line too.

## Where it is

| Machine | Binary | Notes |
|---|---|---|
| Windows | `..\claude_mame\mame\mame.exe` | MSYS2 CLANG64, `SYMBOLS=1`. Graders default to it. |
| Linux dev container | `/home/antigravity/build/mame-bin/mame-shared/shared` | The shared MAME (fork branch `shared`: every fork fix + upstream). Named `shared`, so a search for `mame*` misses it. |

On Linux, use the shared build: don't build a MAME of your own. If it is
missing (the container's disk has been wiped before), rebuild it with
`../claude_mame/build-shared-mame.sh`, detached (`setsid nohup ... &`): hours
from scratch at `-j2`, incremental after that. Do not conclude there is no
MAME. A MAME fix goes on a fork branch that is merged into `shared` (see
`../claude_mame/CLAUDE_MAME.md`), then the script is rerun.

## ROMs

`../claude_mame/mame/roms/` holds `sfight.zip`, `schamp.zip` and `segabill.zip`
(and others). Never point `-rompath` at that folder: its loose `roms/sfight/`
directory is a homebrew program ROM, and MAME prefers it over the zip. Use a
directory that holds only the three zips (symlinks are fine). On Linux that is
`/home/antigravity/build/mameroms`. Also give every run its own
`-nvram_directory` and `-cfg_directory`, so no saved state carries over.

## Headless runs

```
SDL_VIDEODRIVER=dummy ./shared sfight -rompath <3 zips> -nvram_directory <tmp> -cfg_directory <tmp> \
  -snapshot_directory <dir> -nodrc -video none -sound none -nothrottle -skip_gameinfo \
  -seconds_to_run 299 -autoboot_script <script.lua>
```

- `-seconds_to_run` under 300 skips the "this system doesn't work" notice,
  which has no window to be dismissed in. Scripts end the run themselves with
  `manager.machine:exit()`.
- Linux headless runs at about 20-45% of real time under the stream's load. The
  attract replay to frame 1300 takes ~90 s. Detach long runs (`setsid nohup`).
- The ALSA `/dev/snd/seq` line on stderr is harmless.

## Autoboot Lua (see `tools/mame/*.lua` for working examples)

- **Keep the frame notifier in a global**:
  `_G.X = emu.add_machine_frame_notifier(fn)`. A `local` is garbage-collected
  and the script silently stops ~240 frames in.
- Memory: `manager.machine.devices[":maincpu"].spaces["program"]`, with
  `read_u8/u16/u32` and `write_*`. The addresses are the same as in
  `tools/lib/board.mjs`.
- Inputs: `manager.machine.ioport.ports[":IN0"].fields["Coin 1"]:set_value(1)`.
  sfight's fields are `:IN0` Coin 1/2, 1 Player Start, 2 Players Start,
  Service 1, Service Mode, and `:IN1` / `:IN2`
  `P1|P2 Up/Down/Left/Right/Punch/Kick/Barrier`. A wrong name is a nil index,
  so wrap the tick in `pcall` and log the error.
- Snapshots: `manager.machine.screens[":screen"]:snapshot("name.png")` works
  under `-video none`, into `-snapshot_directory/sfight/`.
- MAME's boot runs ~800 frames behind ours: the title's Death Egg is at ~1020 in
  MAME against ~230 here. Wait for a game state (`MODE` 0x50002A,
  `SUB` 0x500030, `STAGE_NUM` 0x500064), not a frame count. A coin before
  ~frame 1800 is lost.

## Graders that take MAME

Set `MAME_EXE=<binary>` and `MAME_ROMPATH=<3 zips>` (plus `SDL_VIDEODRIVER=dummy`
on Linux). Each grader's `--mame` pass writes the reference, and a run without
it grades against that reference.

- `tools/grade-zsort.mjs --mame --stage N`: pictures of the attract replay
  (faces on faces, `--toggle <set_camera switch>` for any render A/B).
- `tools/match-replay.mjs --mame`: both fighters' state, frame by frame.
- `tools/grade-osage.mjs --mame`: sway chains at character select.
- `tools/mame/cop_capture.py`: the SHARC side of the COP FIFOs, for `cop_replay`.
- `tools/mame/snd_capture.py` → `snd_compare.py`: the sound board. It reads
  `$MAME_EXE_NAME`, `$MAME_CWD` and `$MAME_EXTRA_ARGS`
  (`-video none -debugger none` on Linux).

Our side of a grader runs headless on Windows. On Linux it needs a GL context,
so point `M2_EXE` at an xvfb wrapper
(`exec xvfb-run -a -s "-screen 0 1280x1024x24" <build>/Release/m2hle "$@"`),
set `M2_WINDOW=1`, and set `STF_ROM` to the zip.

## Showing the owner

Pinboard's Launch button runs MAME's web (wasm) build, which is too slow for
sfight. Give the owner the native command line instead, with `-window -nodrc`
and the clean rompath. On the Linux container it opens on `$DISPLAY` (`:1`).

## Interactive debugging

`../claude_mame` also has an MCP bridge (`mcp_server/server.py` +
`bridge/bridge.lua`) for watchpoints, breakpoints and memory reads while MAME
runs. It stays paused at start, so set your points and then continue. Its
quirks are in `CLAUDE_MAME.md`.
