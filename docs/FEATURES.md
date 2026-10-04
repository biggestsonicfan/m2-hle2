# What m2-hle2 can do

This page lists every feature, grouped by what you want to do and then by build. To get started,
read the [front page](../README.md). For how any of this works, read [TECHNICAL.md](TECHNICAL.md).

- [The games](#the-games)
- [The builds](#the-builds)
- [Controls](#controls)
- [Online play](#online-play)
- [Saves and settings](#saves-and-settings)
- [Picture](#picture)
- [Sound](#sound)
- [Recording and streaming](#recording-and-streaming)
- [Debugging and automation](#debugging-and-automation)
- [Command-line options](#command-line-options)

## The games

The emulator picks a **profile** from the name of the ROM set. Where one set has more than one
profile, choose with `--profile ID`, with the desktop's **Profile** menu, or with the RetroArch
core's *Sonic the Fighters version* option.

| Profile | Name | What it is |
|---|---|---|
| `sfight_console` | Sonic the Fighters - Console | The default for `sfight.zip`. |
| `sfight` | Sonic the Fighters - Arcade | The board exactly as it shipped. |
| `fvipers` | Fighting Vipers | Boots to gameplay. |
| `m2snake` | Snake (homebrew) | A homebrew game that runs on STF's data ROMs. |
| `sfight_homebrew` | Homebrew on Sonic the Fighters' board | Chosen by itself. |

**Sonic the Fighters - Console** plays the way Sega's console releases do:
- **Hidden fighters.** At character select, press Start on Amy for **Honey**, on Sonic for
  **Metal Sonic**, or on Bean for **Robotnik**. This works in every mode.
- **Free play** from the first boot. Coins do nothing.
- In RetroArch and the browser, it runs inside the **PS3 release's menus**:
  - MAIN MENU: Arcade, Offline Versus, Online Battle, Help & Options.
  - Arcade and Versus take the PS3's rule settings.
  - Select pauses.
  - Help & Options has the PS3's six control presets and the credits.

**Sonic the Fighters - Arcade** is the one to compare with MAME.

**Settings that apply to both STF profiles:**
- **Region:** USA by default. `--region japan|usa|export` changes it.
- **Damage:** `--damage normal|real`. NORMAL, the factory default, gives the player who is
  behind a catch-up bonus. REAL turns that off.
- **VS mode** (`--vs-mode`): after a decided versus match, both players go back to character
  select. Without it, the winner stays on against the CPU.
- **The warning screen** is skipped unless you pass `--nowarnskip`.
- **Settings and backup RAM:** the region, damage and free-play defaults apply only while the
  board's backup RAM is blank. Once your saved settings exist, change them in the test menu
  (F3 on the desktop, L3 in RetroArch; the browser has no Test key).

**Homebrew.** Put a homebrew program in place of a stock set's program ROMs; [m2-pacman](https://github.com/biggestsonicfan/m2-pacman) is an
example. The emulator notices that the program is not Sega's and runs it with no game-specific
patches. It also gets its own save file, so it never touches the game's.

## The builds

| Build | Where | Notes |
|---|---|---|
| Browser | [play.sonicthefighte.rs](https://play.sonicthefighte.rs) | Nothing to install. Always the Console version. |
| Desktop | `m2hle-windows-x64.zip`, `m2hle-linux-x64.zip` | Dear ImGui window with the debugger. Linux is experimental. |
| Handheld | `m2hle-rocknix-arm64.zip` | A standalone build for ROCKNIX, made for the Anbernic RG ARC-S (58-60 fps). |
| RetroArch core | `m2hle-libretro-{windows-x64,linux-x64,macos-universal,android-arm64,linux-arm64}.zip` | Needs a GL video driver. |

Everything except the browser is on the
[`canary` release](https://github.com/biggestsonicfan/m2-hle2/releases/tag/canary), which is
rebuilt from every change to master.

### Browser

- **Loading a ROM:** drop a merged `sfight.zip` or `schamp.zip` on the page, or choose the
  file. It never leaves your machine. If files are missing, the page explains the difference
  between split and merged sets.
- **The top bar** has Pause (P or the Pause key; offline only) and **Menu**.
- **The Menu** has:
  - Online Battle and RPCN account.
  - Fullscreen, where the browser allows it.
  - Controls.
  - **Feels laggy?**:
    - A five-second lag check, with a report you can copy.
    - Picture size: Full, Medium (1488x1152), Low (992x768) or Arcade (496x384).
    - Sound: *Sound CPU (exact)* or *Lighter driver*, which is STF's sound driver in C.
  - **Console:** the log, filtered by level, with Copy and Clear.
  - **Picture:** the filters (see [Picture](#picture)).
- **Your settings** (keys, pad, touch layout, picture size, sound driver, filter) are kept in
  the browser.
- **URL parameters:**

  | Parameter | Effect |
  |---|---|
  | `?touch=on\|off` | Forces the touch buttons on or off. |
  | `?scale=N` | Picture size. |
  | `?sounddriver=c\|68000` | Sound driver. |
  | `?audioms=N` | Audio buffer in ms (10-250, default 40). |
  | `?audio=fallback` | The older audio path. |
  | `?debug` | A latency overlay. |
  | `?objview[=N]` | The object viewer. |
  | `?script=449:c,460:,517:s` | Player 1's inputs by frame (keys `u d l r 1-4 s c`). Boots with blank backup RAM. |
  | `?pause=N` | Pauses at frame N. |
  | `?gw=URL` | Another netplay gateway. |
  | `?args=...` | A few renderer flags. |

### Desktop

- **Menus:** File, Profile, Emulation, Debug, Video, Netplay.
  - The menu bar hides while a game runs; move the mouse to the top edge to bring it back.
    *Debug → Always show menu bar* keeps it up.
  - The bar's right end shows the profile, the run state and the speed.
- **File:**
  - Load ROMs...
  - Capture mode (OBS) (Windows only)
  - Quit (Esc)
- **Emulation:**
  - Run/Pause (F9).
  - Step 1, 10 or 100 instructions (F5, F6, F7).
  - Break on warning.
- **F8** restarts the sound board.
- **Windows tray icon:** only in capture mode and `--headless` runs, not the normal window.
  - Capture mode: Show window, Run emulation, Restart sound board, Leave capture mode and Exit.
    Its tooltip shows the frame rate.
  - Headless: Restart sound board and Exit, with no pause item. `--no-tray` turns it off.

### Handheld (ROCKNIX)

[packaging/rocknix/README.md](../packaging/rocknix/README.md) covers installing it.
- **Installing:** `install-es.sh` adds it to EmulationStation. The RetroArch core stays the
  default there; pick "m2hle" as the emulator to use the standalone build.
- **Updates:** an **Update m2-hle** entry in the game list, or `m2hle-update.sh`. A background
  check runs at most every six hours.
- **Per-game options in EmulationStation:**
  - render fps (30, 45, 60)
  - render scale (1x, 2x)
  - screen size (fit, 1x)
  - status overlay (fps, temperature, time, battery)
  - audio
  - button macros (X = P+K, Y = K+B, Z = all three)
  - online play
  - update check
- **Heat:** `--max-temp C` quits above C degrees. The heat guard below is the RetroArch core's,
  not this build's.

### RetroArch core

[packaging/libretro/INSTALL.md](../packaging/libretro/INSTALL.md) covers installing it. The core
options are:

| Option | Values |
|---|---|
| Internal resolution | Native (496x384), Double, Triple, Quadruple, Full screen |
| Sonic the Fighters version | Console, Arcade |
| Sound board | enabled, disabled |
| Sound driver | 68000, In C (STF only, about half the work) |
| Sound board on its own core | enabled (the default; sound one frame later), disabled |
| Draw rate | 60, 30 |
| Heat guard | off, 80, 85, 90 °C (Linux and handhelds) |
| Online play | RetroArch, RPCN |
| Input delay | 1-6, 8 frames |

**Heat guard** (the core option; 85 °C by default on the GLES builds):
- When the device gets too hot, it draws every second frame until the device is 5 degrees
  cooler.
- The second time it gets too hot, it stays at every second frame.
- The third time, or if the device has not cooled within a minute, it turns the sound board off.
- It never acts during an online match.

Savestates, rewind and run-ahead are not supported. Online play needs every machine to start
from the same cold boot (see [Online play](#online-play)).

## Controls

The desktop's keyboard defaults are based on the Model 2 Emulator's (m2emulator), with changes. The browser starts with the same keys, except F2 and F3.

| | Player 1 | Player 2 |
|---|---|---|
| Move | arrow keys | I J K L |
| Punch (B1) | Z | Delete |
| Kick (B2) | X | End |
| Barrier (B3) | C | Page Down |
| B4 | V | Home |
| Start | 1 | 2 |
| Coin | 5 | 6 |

On the desktop, F2 is Service and F3 is Test, which opens the operator menu. The browser has
neither.

**Desktop**
- Keyboard only.
- `--macro KEY=COMBO` puts a combination on one key; repeat it for more keys. Examples:
  - `--macro a=b1+b2`
  - `--macro kp1=p2:b1+b2+b3`
- `--macros` gives four ready-made macros:
  - A = Punch+Kick
  - S = Punch+Barrier
  - D = Kick+Barrier
  - F = all three

**Browser**
- **Keyboard:** rebind every action, with up to three keys each, separately for player 1 and
  player 2. Includes macros (P+K, P+B, K+B, P+K+B) and the Pause key.
- **Gamepad:** rebind every button. A second controller can play player 2.
- **Touch:** on-screen buttons.
  - Shown on touch screens, always, or never.
  - Optional vibration.
  - The layout can be changed separately for portrait and landscape: size, opacity, outline,
    size per button, hidden buttons, and where the bar sits.
  - Button 4 and the macro buttons are hidden at first.

**Handheld**
- `--pad-map` maps the pad's buttons to the board's.
  - Pad buttons: south, east, west, north, start, back, l1, r1, l3, r3, guide.
  - Board actions: b1-b4, start, coin, service, test, none, or a `+` combination.
- The launcher's default is `south=b1,east=b2,r3=b3,west=none`. The d-pad or the left stick
  moves.

**RetroArch**
- B, A, Y and X are buttons 1-4. Start is Start and Select is Coin.
- L3 is Test and R3 is Service.
- Under the Console version's menus, Select pauses instead, and the six PS3 presets apply.
- RetroArch's own remapping works on top of all this.

## Online play

- **How a match works:**
  - Matchmaking goes through [RPCN](https://github.com/RipleyTom/rpcn), the server RPCS3 uses.
  - The match itself is peer to peer.
  - Both machines cold-boot the board together and run in lockstep from power-on. The default
    input delay is 2 frames.
  - The two boards are checked against each other as they run, so a desync is caught and not
    played through.
- **Signing in:** with **Twitch**, which uses a code you approve on twitch.tv once and then signs
  in without asking, or with an RPCN account. You can create an account and have its token
  e-mailed again from inside the emulator.
- **Rooms:**
  - Up to eight players. Two fight, the rest watch and queue, and the winner stays on.
  - Pick a side (1P Entry, 2P Entry or either), sit out to only watch, and set a room password.
  - The owner sets the region, VS mode and, in the browser, the damage.
  - After each match, the fighters are asked **Play again / Exit**.
- **Servers:** the official RPCN server (np.rpcs3.net) or the community one
  (rpcn.sonicthefighte.rs). The browser and the RetroArch lobby let you choose. Twitch sign-in
  is on the community server only.
- **Cross-play:**
  - **Browser and desktop:** a browser plays desktop and RetroArch players through a WebSocket
    gateway.
  - **PS3:** on the official server you can join or host rooms of the PS3 release (NPUB30927)
    running in RPCS3. `--net-ps3` allows this on other servers.
- **Separate lobbies:** each game, and each STF version, has its own room list. The desktop can
  also list [YAMP](https://github.com/biggestsonicfan/YAMP)'s rooms, read-only.
- **Fixes connection problems by itself:**
  - If UDP 3658 is taken, it moves to the next free port.
  - If your router changes your address, it follows.
  - If the link to RPCN drops, it signs back in and returns you to your room.
  - Inside a container, a relay is used by itself (`--net-relay`).
- **Where to find it:**

  | Build | Where |
  |---|---|
  | Browser | **Online Battle** |
  | Desktop | **Netplay → Netplay window**, or `--net-*` options |
  | RetroArch | Online play = RPCN, then Online Battle (or L+R). RetroArch's own netplay also works. Not on Android. |
  | Handheld | Online play on, then L1+R1, Guide or F1 opens a lobby you drive with the pad |

## Saves and settings

**Backup RAM** is the board's battery-backed memory: operator settings, coin setup and
bookkeeping. It is kept byte for byte in MAME's format (`nvram/<set>/backup1`), so you can copy
it between the two.

| Build | Where it is kept |
|---|---|
| Desktop, Windows | `%APPDATA%\m2hle2\nvram\<set>\backup1` |
| Desktop and handheld, Linux | `$XDG_CONFIG_HOME/m2hle2/nvram/<set>/backup1`, or `~/.config/m2hle2/...` without it |
| Browser | localStorage |
| RetroArch | the frontend's `.srm` |

- Backup RAM is written once a minute when it has changed, and again on exit.
- `--headless` and `--kiosk` runs start blank and keep nothing, unless you pass `--nvram-dir`.
- `--no-nvram` turns it off.
- An online session never reads or writes your copy.

**Other settings files**, in the same folder:
- `netplay.cfg`: your sign-in and room settings, shared by every copy of the emulator for one
  user (and with YAMP). It holds your password and Twitch token in plain text.
  `--net-config` picks another file.
- `video.cfg`: the picture filter.

RetroArch keeps `m2hle-rpcn.cfg` in its saves folder instead.

## Picture

- **The renderer** draws the board's 3D at any resolution:
  - Desktop window: the window's size.
  - RetroArch: up to 8x (4x on handhelds).
  - Handheld: `--render-scale`.
  - Browser: Full, Medium, Low or Arcade.
- **Draw rate:** the handheld and RetroArch can draw every second frame to save power. The game
  still runs at 60.
- **Filters:**
  - **CRT:** Lost Judgment's scanline filter, by way of YAMP. It works in every build.
  - **Libretro GLSL presets** (`.glslp` or `.glsl`, with their textures) on the OpenGL builds:
    the browser, Linux and RetroArch. Not on the Windows D3D11 desktop build.
    - About 67 of the 78 `crt/` presets in glsl-shaders run in the browser.
    - `.slangp` presets are not supported.
  - Each filter's parameters can be adjusted.
  - *Filter input* sets the size the filter samples at: automatic, or 1x to 4x of 496x384.
- **Where to set it:** desktop *Video* menu, `--crt`, `--shader PRESET`, `--shader-scale N`;
  the browser's *Picture* drawer.

## Sound

- **The sound board** (a 68000 and Yamaha's SCSP) is emulated in lockstep with the main CPU,
  so the music keeps the board's time.
- **Sound driver in C:** `--sound-hle`, or the browser's *Lighter driver* and RetroArch's
  *Sound driver: In C*. It runs STF's sound driver as C instead of on the 68000, through the
  same chip, for about half the work. The game and online play behave the same either way.
- **Turning it off:** `--no-sound-board` on the desktop. The handheld plays sound only with
  `--sound`, which its *audio* option sets.

## Recording and streaming

- **Capture mode for OBS** (Windows): `--kiosk`, or *File → Capture mode*.
  - A fixed-size picture (`--kiosk-size`, default 1920x1080) in a hidden window, with no menus.
    `--kiosk-show` shows it.
  - Driven from the tray.
- **Raw A/V stream:** `--av-port N`.
  - One local client gets BGRA or NV12 frames and 16-bit stereo sound on one socket, every
    packet stamped with the board's own sample clock. TECHNICAL.md has the wire format.
  - Works headless on Windows (D3D11) and Linux (EGL).
  - `--av-size`, `--av-format`, `--av-test-card` and `--av-mute` adjust it.
- **Overlay plugins:** `--overlay PATH` loads a DLL or shared library that draws around the
  game; `--overlay-game` places the game's picture.
  - `--overlay-reload` reloads the plugin when it is rebuilt.
  - The MCP command `overlay_swap` changes plugins while running.

## Debugging and automation

- **The desktop's Debug menu:**
  - CPU registers, a memory viewer, memory bus statistics, breakpoints and watchpoints.
  - COP (coprocessor) diagnostics, with *Break on unknown COP cmd*.
  - The 3D viewer, the object viewer and SKY EYE.
  - Dump 3D captures.
  - The 68000 sound CPU, with its own memory viewer and a sound-write log.
  - The warning-screen skip.
  - CPU opcode tests.
- **Object viewer:** browse every model in the ROM with a free camera. Open it with
  `--objview [N]`, from the Debug menu, or with the browser's `?objview`. For single models,
  use `--model N` and `--extract N`.
- **SKY EYE:** paste a camera link from the STF explorer ([noclip](https://github.com/biggestsonicfan/noclip)) and the game's camera jumps
  there, on the stage it names. *Copy explorer link* goes the other way. STF only; also
  `--sky-eye LINK`.
- **Attract replay:** `--match-replay` skips straight to attract's preprogrammed fight, and
  `--match-replay-stage N` picks its stage.
- **MCP bridge:** `--mcp` (port 7172, or `--mcp-port`) lets a script or an AI agent drive the
  emulator over TCP. It can:
  - read and write memory
  - set breakpoints
  - run frames and set inputs
  - read the status and timings
  - run netplay
  - use the object viewer and SKY EYE
  - swap overlays

  `mcp_server/server.py` exposes it as an MCP server. [MCP_GUIDE.md](MCP_GUIDE.md) lists every
  command.
- **Headless:** `--headless` runs with no window, GPU or audio device. This is how the graders
  in [tools/](../tools/README.md) run it.
- **Logging:** `--log FILE` and `--log-level debug|info|warn|error|off`, also per channel
  (`net=debug`).
- **Profiler** (Linux): `M2HLE_HOSTPROF`, or the handheld's `--host-prof`. It samples the
  emulator's own threads; `tools/hostprof.py` reads the result.

## Command-line options

### Desktop

| Option | Effect |
|---|---|
| `--rom PATH` | The ROM zip to load. |
| `--run` | Start running instead of paused. |
| `--profile ID` | Choose a profile ([The games](#the-games)). |
| `--region japan\|usa\|export` | The board's region, on a blank backup RAM. |
| `--damage normal\|real` | STF's damage setting, on a blank backup RAM. |
| `--vs-mode` | Versus matches go back to character select. |
| `--nowarnskip` | Show the warning screen. |
| `--match-replay`, `--match-replay-stage N` | Go straight to attract's replay fight. |
| `--sky-eye LINK` | Open a SKY EYE camera link. |
| `--no-nvram`, `--nvram-dir DIR` | Backup RAM off, or kept in DIR. |
| `--export-roms DIR` | Write the loaded ROM images to DIR, then quit. |
| `--headless` | No window. |
| `--no-tray` | No tray icon (Windows). |
| `--kiosk`, `--kiosk-size WxH`, `--kiosk-show` | Capture mode (Windows). |
| `--av-port N`, `--av-size WxH`, `--av-format bgra\|nv12`, `--av-test-card`, `--av-mute` | Raw A/V stream. |
| `--overlay PATH`, `--overlay-args S`, `--overlay-game WxH+X+Y`, `--overlay-reload` | Overlay plugin. |
| `--crt`, `--no-shader`, `--shader PRESET`, `--shader-scale N` | Picture filter. |
| `--macro KEY=COMBO`, `--macros` | Key macros. |
| `--no-sound-board`, `--sound-hle` | Sound board off, or STF's driver in C. |
| `--mcp`, `--mcp-port N`, `--mcp-watch-port N` | MCP bridge. |
| `--log FILE`, `--log-level SPEC` | Logging. |
| `--objview [N]`, `--model N`, `--extract N`, `--rombank N`, `--cyclemaps` | Model tools. |
| `--netplay` | Open the Netplay window. |
| `--net-server`, `--net-port`, `--net-fingerprint` | The RPCN server and its certificate. |
| `--net-user`, `--net-pass`, `--net-token`, `--net-twitch` | Signing in. |
| `--net-host`, `--net-players N`, `--net-join ROOMID`, `--net-room-pass`, `--net-start` | Rooms. |
| `--net-delay N` | Input delay. |
| `--net-config FILE` | Another netplay settings file. |
| `--idle-until-match` | Hold the board at power-on until a session starts. |
| `--net-relay on\|off\|auto\|URL`, `--net-local-ip IP` | Containers and relays. |
| `--net-ps3`, `--net-ps3-wire FILE` | PS3 cross-play on another server, and its wire log. |

Options for development: `--texload-i960`, `--spin-i960`, `--no-mesh-cache`, `--cpu-tiles`,
`--steps-per-slice N`, `--net-p2p-port N` (the peer-to-peer UDP port, for two clients on one
machine in a loopback test). `--realirq` and `--live-timers` are accepted and do nothing.

### Handheld

`--rom`, `--profile`, `--region`, `--vs-mode`, `--match-replay`, `--no-nvram`, `--nvram-dir`,
`--log`, `--log-level`, `--sound-hle`, `--netplay`, `--net-config`, `--net-delay`,
`--net-host` and `--net-room-pass` work as on the desktop. The handheld adds:

| Option | Effect |
|---|---|
| `--sound` | Run the sound board (off without it). |
| `--render-fps N` | Draw rate (default 30). |
| `--render-scale N` | Draw scale (0 = the screen's size). |
| `--display-scale N`, `--window WxH`, `--filter nearest\|linear` | Picture size and scaling. |
| `--osd` | Show fps, temperature, time and battery. |
| `--pad-map LIST` | Map the pad's buttons. |
| `--max-temp C` | Quit above C degrees. |
| `--stats`, `--shot FRAME:FILE`, `--exit-after N`, `--host-prof START:SECS[:FILE]` | Measurement. |

### Environment variables

| Variable | Effect |
|---|---|
| `M2HLE_NO_SOUND_BOARD=1` | Sound board off (desktop only). |
| `M2HLE_SOUND_HLE=1` | STF's sound driver in C (desktop only). |
| `M2HLE_SOUND_THREAD=0` | Sound board on the main thread. |
| `M2HLE_NET_RELAY`, `M2HLE_NET_LOCAL_IP` | As `--net-relay` and `--net-local-ip`. |
| `M2HLE_HOSTPROF` | Start the profiler (Linux). |

For testing and A/B:

| Variable | Effect |
|---|---|
| `M2HLE_TEXLOAD_HLE=0` | STF's texture loader on the i960 instead of in C (desktop only). |
| `M2HLE_SPIN_SKIP=0` | Run the idle vblank spin instead of skipping it (desktop only). |
| `M2HLE_UNTHROTTLE=1` | No 60 Hz pacing. |
| `M2HLE_VDEPTH=0\|1` | The sort key as vertex depth instead of fragment depth (on by default on GLES). |
| `M2HLE_VIEW_CULL=0` | Draw faces outside the window instead of culling them. |
| `M2HLE_SCRIPT=449:c,460:,...` | Player 1's inputs by frame, as the browser's `?script` (RetroArch core only). |
| `M2HLE_RPCN_AUTOJOIN=1\|NAME` | Join any open room, or NAME's, once signed in (RetroArch core only). |
