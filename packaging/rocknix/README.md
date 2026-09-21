# m2hle on ROCKNIX (Anbernic RG ARC-S)

The `sdl3` frontend — fullscreen SDL3 window, GLES 3, no ImGui and no Python in
the build — runs on the RK3566 at 58–60 game fps. This directory is what turns
the binary into something EmulationStation launches.

CI builds it on every push to `master`
(`.github/workflows/canary.yml`, job `rocknix`) and attaches
`m2hle-rocknix-arm64.zip` to the rolling `canary` release:

    https://github.com/biggestsonicfan/m2-hle2/releases/download/canary/m2hle-rocknix-arm64.zip

## Install

Unpack it anywhere on the device and run the installer as root:

    scp m2hle-rocknix-arm64.zip rocknix:/storage/
    ssh rocknix 'cd /storage && unzip -o m2hle-rocknix-arm64.zip -d m2hle-install \
                 && bash m2hle-install/install-es.sh'

It is idempotent — re-running only refreshes the binary and the launcher, and
keeps the binary it replaces as `m2hle.bak-<timestamp>` so a canary that breaks
something costs one `cp` to undo. What it does:

| Path | What |
|---|---|
| `/storage/.local/share/m2hle/m2hle` | the aarch64 binary |
| `/storage/.local/bin/start_m2hle.sh` | the launcher ES calls |
| `/storage/.emulationstation/es_systems.cfg` | adds `m2hle` / `m2hle-sa` beside `sm2-emu` under `segamodel2` |
| `/storage/.emulationstation/es_features.cfg` | the per-game options below |

Then put the ROM zip in `/storage/roms/segamodel2/` and pick **m2hle** as the
emulator for it (ES game options → Emulator). Restart ES for the config to be
re-read.

## Options

Per game, through ES's options screen; `start_m2hle.sh` turns them into flags.

| Feature | Values | Flag |
|---|---|---|
| render fps | 30 (cooler) / 45 / 60 | `--render-fps` — the game always runs at 60, this is how often a frame is drawn |
| render scale | 1x (496x384) / 2x (992x768) | `--render-scale` |
| screen size | fit screen / 1x | `--display-scale 1` shows the board's pixels one for one, centred |
| status overlay | on / off | `--osd` — fps, temperature, clock, battery, top right |
| audio | on / off (cooler) | `--sound` — the 68000 + SCSP sound board, in lockstep with the emu thread |

Buttons are fixed in the launcher: the RG ARC-S's A/B/C become Punch / Kick /
Barrier (`--pad-map south=b1,east=b2,r3=b3,west=none`). `--max-temp 90` quits
before the RK3566's ~95 °C trip powers the unit off.

## Building it yourself

The same recipe CI runs, in a container:

    docker run --rm -v "$PWD:/src:ro" -v "$PWD/out:/out" debian:trixie bash -c '
      dpkg --add-architecture arm64 && apt-get update -qq
      DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        crossbuild-essential-arm64 cmake ninja-build git ca-certificates file
      apt-get install -y --no-install-recommends libsdl3-dev:arm64 libgles-dev:arm64 libegl-dev:arm64
      cmake -S /src -B /tmp/b -G Ninja -DCMAKE_BUILD_TYPE=Release -DM2HLE_FRONTEND=sdl3 \
        -DCMAKE_TOOLCHAIN_FILE=/src/packaging/rocknix/aarch64.cmake
      cmake --build /tmp/b -j 8 && aarch64-linux-gnu-strip /tmp/b/m2hle && cp /tmp/b/m2hle /out/'

Debian trixie because it is the one distribution with `libsdl3-dev` for arm64
and a glibc (2.41) that matches ROCKNIX's, so the binary links against the
device's own `libSDL3.so.0`, `libGLESv2.so.2`, `libc.so.6` and `libm.so.6` and
carries none of them (CI fails the build if it asks for any other library). Building against an older glibc would also work; a newer one would not.
