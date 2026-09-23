# m2-hle as a libretro core

m2-hle, the Sega Model 2 emulator, as a core for RetroArch. RetroArch handles the window, video
driver, audio, controllers and remapping, menus and the netplay lobby; the core is the board.
The first supported game is Sonic The Fighters.

## Install

Step by step, for RetroArch and for ROCKNIX, with every setting and file: **INSTALL.md**.

Every zip holds the core, `m2hle_libretro.info` (so RetroArch lists the core by name),
`VERSION.txt` and this file. Put the core in RetroArch's cores folder and the `.info` in its info folder. **Settings >
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
  It also adds **Update m2-hle** to the Sega Model 2 game list, which updates the core (and the
  standalone m2hle, if that is installed too) from the canary release, picking the zip for the
  device's CPU.
  See INSTALL.md, "Updating".
  Other handheld distributions: copy the two files by hand as above.

## Updating

The canary release is rebuilt from every change, so it always has the newest core. RetroArch's
Online Updater doesn't know this core: on Windows, Linux, macOS and Android, download the zip
again and replace the two files. **Information > Core Information** shows the installed
build's version (`r<count>-<commit>`), the same as the zip's `VERSION.txt`.

On ROCKNIX, **Update m2-hle** in the Sega Model 2 game list (or `m2hle-update.sh` over ssh)
does it. It finds the zip
for the device's CPU on the release, checks the download against GitHub's sha256 and that the
core will load on the device, and installs it with the new zip's `install-rocknix.sh`. The
script is `packaging/rocknix/m2hle-update.sh`.

## Content

Load `sfight.zip`. Keep the parent set `schamp.zip` in the same folder: `sfight` is a clone of
it. The ROM set is chosen by the zip's file name. No ROMs are included; supply your own.

## Controls

**The Console version** (the default) is the game as its PS3 release presents it: PUSH START
over the attract, then the PS3's **MAIN MENU** -- Arcade, Offline Versus, Online Battle and
Help & Options -- drawn by the core in the PS3's own layout. The pad works as a PS3 pad:
bottom button (RetroPad B) confirms, right (A) goes back, and **Select opens the pause menu**
(Resume Game, Help & Options, Exit Game) -- it is on free play, so there is no coin. Punch,
kick and guard are set per button in **Help & Options > Controls**, with the PS3's six presets
(Standard, Arcade stick 1 to 5): top, right, left and bottom face buttons and L1, L2, R1, R2.
Arcade and Offline Versus have the PS3's rule settings (difficulty, rounds, time, attack,
barriers, game type); Offline Versus asks player 2 to press Start on port 2.

**The Arcade version** is the board as it shipped: RetroPad B, A, Y and X are the cabinet's
buttons 1 to 4, Start is Start and Select inserts a coin.

Either way L3 is Test and R3 is Service, port 1 is player 1 and port 2 is player 2, and
RetroArch's **Quick Menu > Controls** remaps on top, as with any core.

## Core options

| Option | Values | Applies |
|---|---|---|
| Internal resolution | Native (496x384), Double, Triple, Quadruple, Full screen | at once |
| Draw rate | every frame (60), every second frame (30, cooler) | at once |
| Heat guard | off, 80, 85, 90 C | at once |
| Sound board | enabled, disabled | next load |
| Online play | RetroArch, RPCN | next load |
| Input delay (frames) | 1 to 8 | the next session you host |

- **Full screen** draws the game at the size of the window or screen, fitted to its 496:384
  shape, so RetroArch has nothing left to scale. Where the core can't find the size, it uses
  Double.
- **Draw rate** at 30 halves the graphics work. The game itself still runs at 60.
- **Heat guard** switches to drawing every second frame when the device passes the chosen
  temperature, and back once it's 5 degrees cooler. If that happens a second time it stays at
  every second frame, and if the device is still hot after that (hot a third time, or not below
  the limit a minute later) it switches the sound board off, until the game is next loaded.
  None of this touches an online match: the draw rate is this machine's business only, and the
  sound board stays on while a match is being set up or played, because the other board runs it
  too. It goes off once the match is over. The guard reads Linux's thermal zones, so it does
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
  (play.sonicthefighte.rs), in the PS3 release's own online lobby, drawn by the core and
  driven with the pad. Set **Online play** to RPCN and reload the game, then choose **Online
  Battle** from the main menu (the Arcade version opens the lobby at load, and L + R together
  open it again). Signing in needs no keyboard:
  - **Sign in with Twitch.** The lobby shows a twitch.tv/activate address and a code. Approve
    it there, and later launches sign straight in.
  - **Sign in with an RPCN account.** Type the name and password on the lobby's on-screen
    keyboard (cross types a key, square deletes, Start or Done finishes).

  From there it is the PS3's **PLAYER MATCH**: Quick Match (joins a room that will have you,
  or makes one), Custom Match (the room list: cross joins, square refreshes), Create Match (the
  room's size, game type and input delay). A room of two shows the VS lobby -- cross is Ready,
  and the 30-second countdown readies you when it runs out -- and a room of three or more shows
  the ROOM MATCH list, where cross asks for 1P or 2P and the room's owner can Skip the
  countdown with square.

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
over `RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE`; RPCN is `src/net/netplay.h`. The menus are
`src/ui/ps3ui_shell.h` (the offline shell) and `src/ui/ps3ui_app.h` (the online lobby),
rebuilt from the PS3 release's layouts and drawn with sokol_gl (`src/ui/ps3ui_gpu.h`); see
`tools/ps3ui/README.md`. While a menu has the screen the core reports a 16:9 picture of its own.
(`src/ui/pad_lobby.h` is the old text lobby, for the SDL3 handheld build.)
