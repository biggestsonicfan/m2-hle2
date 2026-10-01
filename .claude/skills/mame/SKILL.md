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
| Linux dev container | `/home/antigravity/build/claude_mame-mame/m2` | Named `m2` (`SUBTARGET=m2`), so a search for `mame*` misses it. |

The Linux build lives on the container's overlay disk and has been wiped before.
If it is missing, rebuild it (~40 min at `-j3` beside a live stream). Do not
conclude there is no MAME. The recipe is in `../claude_mame/CLAUDE_MAME.md`,
"Build environment (Linux)":

```
sudo apt-get install -y pkg-config libsdl2-dev libsdl2-ttf-dev libfontconfig-dev libpulse-dev \
     libasound2-dev libxinerama-dev libxi-dev libxrandr-dev
mkdir -p ~/build/claude_mame-mame
git -C ../claude_mame/mame archive HEAD | tar -x -C ~/build/claude_mame-mame
cd ~/build/claude_mame-mame
setsid nohup sh -c 'nice -n 19 make SUBTARGET=m2 SOURCES=src/mame/sega/model2.cpp REGENIE=1 \
     NOWERROR=1 USE_QTDEBUG=0 PYTHON_EXECUTABLE=/usr/bin/python3 -j3 > /tmp/mame-build.log 2>&1' &
```

Build on local disk, not on the repos share (9p is far too slow), and detach it
with `setsid nohup`. A plain background job dies with the session, and make then
has to resume where it stopped.

## ROMs

`../claude_mame/mame/roms/` holds `sfight.zip`, `schamp.zip` and `segabill.zip`
(and others). Never point `-rompath` at that folder: its loose `roms/sfight/`
directory is a homebrew program ROM, and MAME prefers it over the zip. Use a
directory of zips with no loose folder. In the dev container that is `$ROMS_DIR`
(`/home/antigravity/build/mameroms`): the container's one ROM folder, symlinks to
every stock set it has, kept by antigravity-dev-docker's `link-roms.sh`. If a set
is not there, it is not in the container; do not copy zips anywhere. Also give every run its own
`-nvram_directory` and `-cfg_directory`, so no saved state carries over.

## Headless runs

```
SDL_VIDEODRIVER=dummy ./m2 sfight -rompath <3 zips> -nvram_directory <tmp> -cfg_directory <tmp> \
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
