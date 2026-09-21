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
| `/storage/.local/bin/m2hle-update.sh` | the updater (below) |
| `/storage/.config/modules/Update m2hle.sh` | its entry in ES's **Tools** |
| `/storage/.local/share/m2hle/VERSION.txt` | which canary is installed |

Then put the ROM zip in `/storage/roms/segamodel2/` and pick **m2hle** as the
emulator for it (ES game options → Emulator). Restart ES for the config to be
re-read.

## Updates

Once installed, the device updates itself from this release; the `scp` above
is only needed the first time.

- **Tools → Update m2hle** checks the canary and, if it is newer, downloads it,
  checks it against the sha256 GitHub publishes for it, runs the new binary once
  to be sure it starts on this device, and then runs the new zip's own
  `install-es.sh`, which also updates the updater. The result is shown on screen.
  If the ES config gained an option, ES is restarted.
- **When a game starts**, the launcher checks in the background, at most every
  6 hours and never delaying the game. If an update is waiting, a notice appears
  after the game exits. Turn it off per game with the "update check" option.

"Newer" means that the zip's sha256 on the release differs from the zip that was
installed, not that the release notes name a new commit: when the handheld CI job
fails, the release keeps the old zip under new notes.

From a shell: `m2hle-update.sh` (update if newer), `--force` (reinstall),
`--check` (exit 0 = update waiting, 1 = current, 2 = could not tell).
Stepping back needs no network: `cp /storage/.local/share/m2hle/m2hle.bak-<stamp>
/storage/.local/share/m2hle/m2hle`.

## Options

Per game, through ES's options screen; `start_m2hle.sh` turns them into flags.

| Feature | Values | Flag |
|---|---|---|
| render fps | 30 (cooler) / 45 / 60 | `--render-fps` — the game always runs at 60, this is how often a frame is drawn |
| render scale | 1x (496x384) / 2x (992x768) | `--render-scale` |
| screen size | fit screen / 1x | `--display-scale 1` shows the board's pixels one for one, centred |
| status overlay | on / off | `--osd` — fps, temperature, clock, battery, top right |
| audio | on / off (cooler) | `--sound` — the 68000 + SCSP sound board, in lockstep with the emu thread |
| button macros | off / on | the top row presses combos: X = Punch+Kick, Y = Kick+Barrier, Z = all three |
| online play | off / on | `--netplay` — sign in to RPCN and open the lobby; see below |
| update check | on / off | the launcher's background update check (see Updates) |

Buttons are fixed in the launcher: the RG ARC-S's A/B/C become Punch / Kick /
Barrier (`--pad-map south=b1,east=b2,r3=b3,west=none`). `--pad-map` also takes
combos (`north=b1+b2`), which is what "button macros" uses. `--max-temp 90` quits
before the RK3566's ~95 °C trip powers the unit off.

## Online play

The handheld plays the same RPCN netplay as the desktop and web builds (it is
the same native build family, so it can join either). It has no keyboard, so it
does not sign in by itself: it uses the desktop's netplay settings file, which
holds the account and, after a Twitch sign-in, the login token that stands in
for a password. Copy it across from the PC once:

    ssh rocknix 'mkdir -p /storage/.config/m2hle2 && chmod 700 /storage/.config/m2hle2'
    scp "%APPDATA%\m2hle2\netplay.cfg" rocknix:/storage/.config/m2hle2/netplay.cfg
    ssh rocknix 'chmod 600 /storage/.config/m2hle2/netplay.cfg'

or put `netplay.cfg` beside `install-es.sh` before running it, which does the
same. Then turn **online play** on in the game's options.

With it on, the game signs in at launch and opens the lobby over it: host a
room, or join one from the list, then **Start the match** — both boards reset
and play from power-on in lockstep. **L1+R1** opens the lobby again at any time
(during a match: leave it); **A** picks, **B** closes. With no settings file, the
lobby offers a Twitch sign-in and shows the code to approve from a phone.

Two things to know:

- **RPCN allows one session per account.** While the PC's emulator (or anything
  else) is signed in as that account, the handheld's sign-in is refused with
  "that account is already logged in". Use a second account for the handheld to
  play against the PC.
- **RPCN keeps one Twitch token per account.** Copying the file shares the
  token, which is fine; running the Twitch sign-in *on the handheld* issues a
  new one and signs every other copy of that account out of Twitch.

TLS comes from the device's own OpenSSL (`libssl.so.3`), opened at the first
connect rather than linked, so the binary still needs only the four libraries
below; without it, netplay says so and the game runs as usual.

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
