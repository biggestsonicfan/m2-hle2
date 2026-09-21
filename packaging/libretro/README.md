# m2-hle as a libretro core

m2-hle, the Sega Model 2 emulator, as a core for RetroArch. RetroArch handles the window, video
driver, audio, controllers and remapping, menus and the netplay lobby; the core is the board.
The first supported game is Sonic The Fighters.

## Install

Step by step, for RetroArch and for ROCKNIX, with every setting and file: **INSTALL.md**.

Every zip holds the core, `m2hle_libretro.info` (so RetroArch lists the core by name) and this
file. Put the core in RetroArch's cores folder and the `.info` in its info folder. **Settings >
Directory** in RetroArch shows where both are on your system.

| Zip | For | Video driver |
|---|---|---|
| `m2hle-libretro-windows-x64.zip` | RetroArch on Windows | `glcore` or `gl` |
| `m2hle-libretro-linux-x64.zip` | RetroArch on desktop Linux | `glcore` or `gl` |
| `m2hle-libretro-macos-universal.zip` | RetroArch on macOS (Apple Silicon and Intel) | `glcore` or `gl` |
| `m2hle-libretro-android-arm64.zip` | RetroArch on Android | `gl` |
| `m2hle-libretro-linux-arm64.zip` | ARM Linux handhelds: ROCKNIX, Knulli, muOS, ArkOS and others | `gl` |

The core draws with OpenGL: GL 4.1 on a desktop, GLES 3 on Android and the handhelds. If
RetroArch is set to Vulkan or Direct3D, it normally switches to its GL driver for this core by
itself; if it doesn't, pick `glcore` or `gl` in **Settings > Drivers**.

- **Android:** in RetroArch, **Load Core > Install or Restore a Core** and pick
  `m2hle_libretro_android.so`.
- **macOS:** the core is unsigned. If macOS blocks it, allow it in **System Settings > Privacy &
  Security**.
- **ROCKNIX:** unpack the zip on the device and run `bash install-rocknix.sh` as root, over ssh. It
  installs the core, adds **RetroArch / m2hle** to Sega Model 2's emulator list in
  EmulationStation, and keeps the core's files out of the ROM folder. Restart EmulationStation
  afterwards. `bash install-rocknix.sh --make-default` also makes it Model 2's default emulator.
  Other handheld distributions: copy the two files by hand as above.

## Content

Load `sfight.zip`. Keep the parent set `schamp.zip` in the same folder: `sfight` is a clone of
it. The ROM set is chosen by the zip's file name. No ROMs are included; supply your own.

## Controls

RetroPad B, A, Y and X are the cabinet's buttons 1 to 4, Start is Start and Select inserts a
coin. L3 is Test and R3 is Service. Port 1 is player 1 and port 2 is player 2. Remap them in
RetroArch's **Quick Menu > Controls**, as with any core.

## Core options

| Option | Values | Applies |
|---|---|---|
| Internal resolution | Native (496x384), Double, Triple, Quadruple, Full screen | at once |
| Draw rate | every frame (60), every second frame (30, cooler) | at once |
| Heat guard | off, 80, 85, 90 C | at once |
| Sound board | enabled, disabled | next load |
| Online play | RetroArch, RPCN | next load |
| RPCN sign-in | signed out, Twitch, RPCN account | at once |
| Input delay (frames) | 1 to 8 | the next session you host |

- **Full screen** draws the game at the size of the window or screen, fitted to its 496:384
  shape, so RetroArch has nothing left to scale. Where the core can't find the size, it uses
  Double.
- **Draw rate** at 30 halves the graphics work. The game itself still runs at 60.
- **Heat guard** switches to drawing every second frame when the device passes the chosen
  temperature, and back once it's 5 degrees cooler. It reads Linux's thermal zones, so it does
  nothing on Windows or macOS. The handheld build has it at 85 C by default.
- **Sound board** off is silent and cheaper on a handheld. An online match turns it on anyway,
  because the other player's board always runs it and the two games have to match.

No savestates, so no rewind or run-ahead either.

## Online play

The core has two kinds of netplay. Either way, a match starts both boards from a cold boot at the
same moment and runs them in lockstep from there.

- **RetroArch** (the default): use RetroArch's own **Netplay** menu to host, with a password if
  you like, or join from its lobby. The host is player 1 and the first player to join is
  player 2. Both players need the same build of the core. RetroArch's usual netplay needs
  savestates and can't run this core, so the core carries the match itself over RetroArch's
  connection.
- **RPCN**: the same rooms as the m2-hle desktop emulator and the website
  (play.sonicthefighte.rs). Set **Online play** to RPCN and reload the game, then choose an
  **RPCN sign-in**:
  - **Sign in using Twitch.** A notification shows a twitch.tv/activate address and a code.
    Approve it there, and later launches sign straight in.
  - **RPCN account.** Cores can't open a text box, so the account goes in through the one
    RetroArch does have, the cheat code. In **Quick Menu > Cheats**, add a cheat whose code is
    `rpcn:NAME:PASSWORD:TOKEN` and apply it. TOKEN is the one RPCN e-mailed you; leave `:TOKEN`
    off if the server doesn't use one. The password can't contain `:`. RetroArch saves cheats
    to a plain-text file.

  Then press **L1+R1** in game for the rooms: host, join, and start a match.

  RPCN keeps one Twitch login per account, so signing in here with Twitch signs the desktop
  emulator's Twitch login out, and the other way round. **RPCN isn't available on Android**: its
  sign-in needs the system's OpenSSL, which Android doesn't have. RetroArch netplay works there.

Every RPCN match writes an input log, `netplay-<date>-s<session>-p<player>.inputs`, next to the
core's `m2hle-rpcn.cfg` in RetroArch's saves. If two players saw different games,
`det_digest --inputs <log>` replays it and finds the frame where the boards split
(`tests/det_digest.c`).

## Building

The canary workflow (`.github/workflows/canary.yml`, jobs `libretro` and `libretro-linux-arm64`)
builds every zip above. By hand:

    cmake -S . -B build_lr -DM2HLE_FRONTEND=libretro                 # GL 4.1 core
    cmake -S . -B build_lr -DM2HLE_FRONTEND=libretro -DM2HLE_LIBRETRO_GLES=ON   # GLES 3
    cmake --build build_lr --config Release

- **Android:** the NDK's `android.toolchain.cmake` with `-DANDROID_ABI=arm64-v8a
  -DANDROID_PLATFORM=android-24`; GLES is chosen automatically.
- **ARM Linux:** cross-compile with `packaging/libretro/aarch64-generic.cmake`, as absolute
  path, in a container whose glibc is no newer than the oldest device you want to reach (CI uses
  Debian bullseye, 2.31).

Every build exports only the `retro_*` API. On Linux and Android an undefined symbol is a link
error (`-z defs`), so a missing piece fails the build instead of RetroArch's load.

How it fits together: `src/main_libretro.c` runs one board slice per `retro_run` with
`emu_slice_body` / `emu_slice_finish`, like every other m2-hle host. It draws into RetroArch's
framebuffer through `game_frame.h` with sokol_gfx, and resets GL to its defaults after each
frame, because RetroArch draws with the same context and sokol's leftover state (its scissor
test above all) blacked out the picture on GLES. RetroArch netplay is `src/net/pkt_lockstep.h`
over `RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE`; RPCN is `src/net/netplay.h` with the gamepad
lobby in `src/ui/pad_lobby.h`.
