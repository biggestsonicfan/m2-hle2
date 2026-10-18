# What m2-hle2 can do

This page lists every feature, grouped by what you want to do and then by build. To get started,
read the [front page](../README.md). For how any of this works, read [TECHNICAL.md](TECHNICAL.md).

- [The games](#the-games)
- [The builds](#the-builds)
- [Controls](#controls)
- [Online play](#online-play)
- [Saves and settings](#saves-and-settings)
- [Replays, savestates and following a board](#replays-savestates-and-following-a-board)
- [Game scripts (Lua)](#game-scripts-lua)
- [Picture](#picture)
- [Sound](#sound)
- [Recording and streaming](#recording-and-streaming)
- [Debugging and automation](#debugging-and-automation)
- [Command-line options](#command-line-options)

## The games

The emulator picks a **profile** from the name of the ROM zip (MAME's set names). Where one set
has more than one profile, the first in this table is the default; choose another with
`--profile ID`, with the desktop's **Profile** menu, or with the RetroArch core's *Sonic the
Fighters version* option.

| Profile | Name | Board | Notes |
|---|---|---|---|
| `sfight_console` | Sonic the Fighters - Console | 2B | The default for `sfight.zip`. Parent `schamp.zip`. |
| `sfight` | Sonic the Fighters - Arcade | 2B | The board exactly as it shipped. |
| `srallyc` | Sega Rally Championship (Rev C) | 2A | |
| `vonj` | Cyber Troopers Virtual-On (Japan, Rev B) | 2B | A clone: `von.zip` goes beside it, or use a merged `vonj.zip`. |
| `dynamcopb`, `dyndeka2b` | Dynamite Cop (Export) / Dynamite Deka 2 (Japan) | 2B | Clones: `dynamcop.zip` goes beside them. |
| `dynamcop`, `dyndeka2` | Dynamite Cop (Export) / Dynamite Deka 2 (Japan) | 2A | |
| `dynamcopc` | Dynamite Cop (USA) | 2C | A clone of `dynamcop.zip`. |
| `fvipers` | Fighting Vipers | 2B | |
| `hotd` | The House of the Dead (Rev A) | 2C | Light guns. MAME's `hotdo.zip` loads too. |
| `daytona` | Daytona USA (Japan, Rev A) | Model 2 | Needs `model1io.zip` beside it. |
| `doa`, `doab` | Dead or Alive (Rev C, Rev B) | 2B | `doab` is a clone of `doa.zip`. |
| `doaa`, `doaab`, `doaae` | Dead or Alive (Rev A, original, Export Rev A) | 2A | Clones of `doa.zip`. |
| `m2snake` | Snake (homebrew) | 2B | A homebrew game that runs on STF's data ROMs. |
| `sfight_homebrew` | Homebrew on Sonic the Fighters' board | 2B | Chosen by itself. |

- The Dynamite Cop clones also load under MAME's older names (`dynmcopb.zip`, `dyndek2b.zip`,
  `dynmcopc.zip`).
- The browser runs every game except the two homebrew profiles; its front page does not list
  Dead or Alive yet.
- Virtua Cop (`vcop`) is prepared but does not run yet.

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

**Settings for the games:**
- **Region:** `--region japan|usa|export`, on a blank backup RAM. `--region-over-saved` makes it
  win over saved settings too, as the browser does. It works for STF, Fighting Vipers, Sega Rally
  and The House of the Dead. Virtual-On's country, and Dead or Alive's Export set's, is in the
  program ROM. Dynamite Cop, Daytona USA and Dead or Alive's other sets ignore it: where they keep
  their country has not been looked for yet.
- **CPU difficulty** (STF): `--enemy-rank cabinet|easy|normal|hard|hardest|extra1|extra2`, or
  the desktop's *Profile → CPU difficulty*. `cabinet` keeps the test menu's setting.
- **Damage** (STF): `--damage normal|real`. NORMAL, the factory default, gives the player who is
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
| Browser | [arcade.sonicthefighte.rs](https://arcade.sonicthefighte.rs) | Nothing to install. `m2hle-web.zip` is the same site, to host yourself. |
| Desktop | `m2hle-windows-x64.zip`, `m2hle-linux-x64.zip` | Dear ImGui window with the debugger. Linux is experimental. |
| RetroArch core | `m2hle-libretro-{windows-x64,linux-x64,macos-universal,android-arm64,linux-arm64}.zip` | Needs a GL video driver. This is the build for handhelds. |
| Handheld (ROCKNIX) | build it yourself | A standalone SDL3 build. No longer on the release. |

The zips are on the
[`canary` release](https://github.com/biggestsonicfan/m2-hle2/releases/tag/canary), which is
rebuilt from every change to master.

### Browser

- **Picking the game:** the front page shows one box per game. `?game=SET` opens a game
  directly, `?profile=ID` a profile. play.sonicthefighte.rs and srally.sonicthefighte.rs are
  old addresses that open STF and Sega Rally.
- **Loading a ROM:** drop the zip on the page, or choose the file. It never leaves your machine.
  A clone set needs its parent picked at the same time (`vonj.zip` with `von.zip`, a Dynamite
  Cop clone with `dynamcop.zip`, `daytona.zip` with `model1io.zip`), or a merged zip. If files
  are missing, the page explains the difference between split and merged sets.
- **Language:** `?lang=ja` shows the page in Japanese. The page's language also sets the
  board's region: English plays USA, Japanese plays Japan, any other language Export.
- **The top bar** has Pause (P or the Pause key; offline only), **Menu**, and *Finish replay*
  while your game is being recorded.
- **The Menu** has:
  - Online Battle, RPCN account and **GGPO lobby** (see [Online play](#online-play)).
  - Fullscreen, where the browser allows it.
  - Controls.
  - **Feels laggy?**:
    - A five-second lag check, with a report you can copy.
    - Picture size: Full, Medium (1488x1152), Low (992x768) or Arcade (496x384).
    - Sound: *Sound CPU (exact)* or *Lighter driver*, which is STF's sound driver in C.
  - **Console:** the log, filtered by level, with Copy and Clear.
  - **Picture:** the filters (see [Picture](#picture)).
  - **Replay:** your saved replays, kept in the browser, to play back.
  - **Cheats:** a Lua game script, kept per game (see [Game scripts](#game-scripts-lua)).
  - **Screenshot** and **Report an issue** (offline only). Report freezes the frame and makes a
    zip of the picture, a savestate, the console and your machine's details, for a GitHub
    issue. Dropping that zip (or its `state.sta`) on a running game loads the state.
- **Your settings** (keys, pad, touch layout, picture size, sound driver, filter) are kept in
  the browser. Each game's backup RAM is kept there too.
- **The arcade** (STF and Sega Rally): the site asks once whether to share. Sharing boots on
  the shared board, with its ranking and settings, and sends your new ranking entries back;
  you can also send anonymous usage events. Change either under *Arcade & your data*.
- **URL parameters:**

  | Parameter | Effect |
  |---|---|
  | `?game=SET`, `?profile=ID` | The game, or one profile of it. |
  | `?lang=ja` | Japanese page, and the Japan region. |
  | `?ggpo=URL` | Another GGPO lobby. |
  | `?follow=DIR` | Follow a board that a leader writes on the same site (see [Following](#replays-savestates-and-following-a-board)). |
  | `?wheel=RANGE,SPEED` | Sega Rally's wheel: full lock and turn speed. |
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
  | `?rom=URL` | Load a ROM from the same site (development; repeat it for a split set). |

### Desktop

- **Menus:** File, Profile, Emulation, Debug, Video, Netplay, Replays, and Script when a game
  script is loaded.
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
- **Replays:** see [Replays](#replays-savestates-and-following-a-board).
- **F8** restarts the sound board.
- **Windows tray icon:** only in capture mode and `--headless` runs, not the normal window.
  - Capture mode: Show window, Run emulation, Restart sound board, Leave capture mode and Exit.
    Its tooltip shows the frame rate.
  - Headless: Restart sound board and Exit, with no pause item. `--no-tray` turns it off.

### Handheld (ROCKNIX)

The release no longer includes this build: the RetroArch core is what runs on the handhelds now.
[packaging/rocknix/README.md](../packaging/rocknix/README.md) covers building and installing it.
- **Installing:** `install-es.sh` adds it to EmulationStation. The RetroArch core stays the
  default there; pick "m2hle" as the emulator to use the standalone build.
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
- **Virtual-On Versus:** `--versus` starts a second copy linked over 127.0.0.1, in two
  640x480 windows, one cabinet each. Not with `--netplay`.

### RetroArch core

[packaging/libretro/INSTALL.md](../packaging/libretro/INSTALL.md) covers installing it. The core
options are:

| Option | Values |
|---|---|
| Internal resolution | Native (496x384), Double, Triple, Quadruple, Full screen |
| Depth | Board, Per pixel (see [Picture](#picture)) |
| Sonic the Fighters version | Console, Arcade |
| Sound board | enabled, disabled |
| Sound driver | 68000, In C (STF only, about half the work) |
| Gems C: main CPU functions, Gems C: coprocessor | off, on (development) |
| Sound board on its own core | enabled (the default; sound one frame later), disabled |
| Draw rate | 60, 30 |
| Heat guard | off, 80, 85, 90 °C (Linux and handhelds) |
| Online play | RetroArch, RPCN, GGPO |
| Input delay | 1-6, 8 frames |
| Game scripts | enabled, disabled |
| Wheel: full lock, Wheel: turn speed | Sega Rally's and Daytona's wheel |
| Light gun: flash on each shot | off, 1-4 frames |
| Force feedback | 100% to 0% (Daytona) |

**Heat guard** (the core option; 85 °C by default on the GLES builds):
- When the device gets too hot, it draws every second frame until the device is 5 degrees
  cooler.
- The second time it gets too hot, it stays at every second frame.
- The third time, or if the device has not cooled within a minute, it turns the sound board off.
- It never acts during an online match.

**Savestates, rewind and fast-forward** work, except during an online match. A state is about
16 MB; run-ahead is best left off. Online play needs every machine to start from the same cold
boot (see [Online play](#online-play)).

The PS3 menus run for STF's Console version and for Virtual-On (its Versus row is not available
here).

## Controls

The desktop's keyboard defaults are based on the Model 2 Emulator's (m2emulator), with changes. The browser starts with the same keys, except F2 and F3.

| | Player 1 | Player 2 |
|---|---|---|
| Move | arrow keys | I J K L |
| B1 (STF: Punch) | Z | Delete |
| B2 (STF: Kick) | X | End |
| B3 (STF: Barrier) | C | Page Down |
| B4 | V | Home |
| Start | 1 | 2 |
| Coin | 5 | 6 |

On the desktop, F2 is Service and F3 is Test, which opens the operator menu. The browser has
neither. The browser's Controls panel names each button for the game: for example Sega Rally's
B1 accelerates and B2 brakes, and Virtual-On's buttons are its two levers' shots and dashes.

**Other controllers:**
- **Wheel** (Sega Rally, Daytona): the board reads a potentiometer. `--wheel-center`,
  `--wheel-range` (full lock) and `--wheel-speed` (turn speed) adjust it; the browser has
  `?wheel=`, RetroArch its Wheel options.
- **Force feedback** (Daytona): the wheel's motor comes through the pad's rumble in the browser,
  RetroArch and the handheld build (`--ffb-gain N` there), or a phone's vibration. The desktop
  debugger has none.
- **Light guns** (The House of the Dead): the mouse or a finger aims. On the desktop the left
  button fires and the right button reloads; RetroArch uses its light gun device. `--gun-flash N`
  flashes the screen for N frames on each shot, as the browser's Controls option does.
- **Twin stick** (Virtual-On): the browser's pad layout has the two levers, and takes a Saturn
  Twin Stick through a Bliss-Box. Touch shows two d-pads.

**Desktop**
- Keyboard, and the mouse as a light gun.
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
- Mouse and touch work as a light gun.

**RetroArch**
- B, A, Y and X are buttons 1-4. Start is Start and Select is Coin.
- L3 is Test and R3 is Service.
- Under the Console version's menus, Select pauses instead, and the six PS3 presets apply.
- RetroArch's own remapping works on top of all this.

## Online play

There are two kinds of online play: **lockstep rooms on RPCN**, and **GGPO rollback** matches
from a lobby. Online matches are saved as replays.

### RPCN rooms

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
- **Virtual-On** links the two cabinets' communication boards, so a room match is a real
  linked Virtual-On match, with the room's owner as the master cabinet.
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

### GGPO rollback

- The lobby is ggpo.sonicthefighte.rs: sign in (an account, or Twitch), join the game's
  channel, chat, and challenge someone.
- The match runs with rollback, peer to peer or relayed through the lobby. A browser's match
  always goes through the lobby.
- **Where to find it:**

  | Build | Where |
  |---|---|
  | Browser | **GGPO lobby** (`?ggpo=URL` for another lobby) |
  | Desktop | `--ggpo-*` options, or MCP (no menu) |
  | RetroArch | Online play = GGPO, set up in `m2hle-ggpo.cfg` in the saves folder (`user=`, `pass=`, `challenge=NAME`, `accept=NAME\|any`, `url=`; no user signs in with Twitch). L+R lists who to challenge. |

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
- **EEPROM:** the board's small serial EEPROM (some games keep settings there, The House of the
  Dead its country) is in savestates but not kept on disk yet: every boot starts with it erased.

**Other settings files**, in the same folder:
- `netplay.cfg`: your sign-in and room settings, shared by every copy of the emulator for one
  user (and with YAMP). It holds your password and Twitch token in plain text.
  `--net-config` picks another file.
- `video.cfg`: the picture filter.
- `replay.cfg`: whether replays are recorded, and in which format.

RetroArch keeps `m2hle-rpcn.cfg` and `m2hle-ggpo.cfg` in its saves folder instead.

## Replays, savestates and following a board

**Replays** (`.m2replay`):
- Games are recorded, online matches and local ones, and saved in `replays` in the settings
  folder (or `--replay-dir DIR`). `--no-record-replays` turns that off.
- Two formats: **the game's own replay** (STF and Sega Rally) or **inputs from power-on**, which
  plays back exactly. Other games save the inputs. `--replay-format` and `--replay-view` choose.
- A replay is a zip named `<p1>-vs-<p2>-<date>-<time>.m2replay`, or
  `<set>-<date>-<time>.m2replay` for a one-player game such as Sega Rally.
- Desktop: the **Replays** menu (Record matches, Save as, Play as, Play a replay..., the Replay
  window, Stop, Finish the replay) or `--replay FILE`. Browser: the **Replay** drawer, which keeps
  the same `.m2replay` files in the browser and can download them. The page hides the format
  choice for Sega Rally, so it saves the game's own format (each course's clip) unless the
  inputs format was picked earlier for STF in that browser. The page's game list
  (`web/site/m2hle-games.js`) still says Sega Rally has no replay format of its own, so the page
  hides the format choice and its replay text describes button recording, which is not what is
  saved.
- [NATIVE-REPLAY.md](NATIVE-REPLAY.md) has the details.

**Savestates:** a zip like m2emulator's `.sta`.
- Desktop: `--load-state FILE` at launch, or MCP's `save_state` and `load_state`.
- RetroArch: its own savestates and rewind (see [RetroArch core](#retroarch-core)).
- Browser: dropping a state or an issue report's zip on a running game.
- A state cannot be loaded during an online session.

**Following a board:** a leader run with `--follow-out DIR` (`--follow-every N` frames apart)
writes its board as it plays; a browser page on the same site with `?follow=DIR` watches it.

## Game scripts (Lua)

Game scripts use M2emulator's script API (`Init`, `Reset`, `Frame`, `PostDraw`, `End`, an
`Options` table, memory reads and writes, ROM patches, text on screen) in Lua 5.5.
- **Desktop and handheld:** `<set>.lua` in a `scripts` folder in the working directory or beside
  the executable, or in `--scripts DIR`. `--no-scripts` turns them off. On the desktop the
  options show as the **Script** menu; the handheld sets one with `--script-option CODE`.
- **RetroArch:** `m2hle/scripts/<set>.lua` in the system folder (core option *Game scripts*).
  Its options become a `<set>.cht`, switched under *Quick Menu → Cheats*.
- **Browser:** the **Cheats** drawer, a script kept per game.
- A script never runs during an online match, a replay or a follow.

## Picture

- **The renderer** draws the board's 3D at any resolution:
  - Desktop window: the window's size.
  - RetroArch: up to 8x (4x on handhelds).
  - Handheld: `--render-scale`.
  - Browser: Full, Medium, Low or Arcade.
- **Draw rate:** the handheld and RetroArch can draw every second frame to save power. The game
  still runs at 60.
- **Arcade** (the real board's picture): the board's tone curve, ghosting, noise and 4:3 shape.
  Desktop *Video → Arcade*, `--arcade` / `--no-arcade`.
- **Depth per pixel:** sorts the 3D per pixel instead of per polygon as the board does. Off by
  default; not the board, and it costs frame rate on GLES. Desktop *Video* menu,
  `--pixel-depth` / `--no-pixel-depth`, RetroArch's *Depth* option, the browser's *Picture*
  drawer.
- **Filters:**
  - **CRT:** Lost Judgment's scanline filter, by way of YAMP. It works in every build.
  - **Libretro GLSL presets** (`.glslp` or `.glsl`, with their textures) on the OpenGL builds:
    the browser, Linux and RetroArch. Not on the Windows D3D11 desktop build.
    - About 67 of the 78 `crt/` presets in glsl-shaders run in the browser.
    - `.slangp` presets are not supported.
  - Each filter's parameters can be adjusted.
  - *Filter input* sets the size the filter samples at: automatic, or 1x to 4x of 496x384.
- **Where to set it:** desktop *Video* menu, `--crt`, `--shader PRESET`, `--shader-scale N`;
  the browser's *Picture* drawer; the handheld's `--filter`.

## Sound

- **The sound board** (a 68000 and Yamaha's SCSP) is emulated in lockstep with the main CPU,
  so the music keeps the board's time. Daytona runs its own Model 1 sound board.
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
  - save and load states
  - run netplay and the GGPO lobby
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
| `--region japan\|usa\|export`, `--region-over-saved` | The board's region, on a blank backup RAM or over saved settings. |
| `--enemy-rank RANK` | STF's CPU difficulty (`cabinet`, `easy` to `hardest`, `extra1`, `extra2`). |
| `--damage normal\|real` | STF's damage setting, on a blank backup RAM. |
| `--vs-mode` | Versus matches go back to character select. |
| `--nowarnskip` | Show the warning screen. |
| `--wheel-center N`, `--wheel-range N`, `--wheel-speed N` | The wheel's pot. |
| `--gun-flash N` | Flash N frames on each light gun shot. |
| `--match-replay`, `--match-replay-stage N` | Go straight to attract's replay fight. |
| `--sky-eye LINK` | Open a SKY EYE camera link. |
| `--no-nvram`, `--nvram-dir DIR` | Backup RAM off, or kept in DIR. |
| `--export-roms DIR` | Write the loaded ROM images to DIR, then quit. |
| `--load-state FILE` | Load a savestate at launch. |
| `--record-replays`, `--no-record-replays`, `--replay-dir DIR` | Record replays, and where. |
| `--replay-format native\|inputs`, `--replay-view native\|inputs`, `--replay FILE` | Replay format to save and to play; play a replay. |
| `--follow-out DIR`, `--follow-every N` | Lead a follow. |
| `--scripts DIR`, `--no-scripts` | Game scripts. |
| `--headless` | No window. |
| `--no-tray` | No tray icon (Windows). |
| `--kiosk`, `--kiosk-size WxH`, `--kiosk-show` | Capture mode (Windows). |
| `--av-port N`, `--av-size WxH`, `--av-format bgra\|nv12`, `--av-test-card`, `--av-mute` | Raw A/V stream. |
| `--overlay PATH`, `--overlay-args S`, `--overlay-game WxH+X+Y`, `--overlay-reload` | Overlay plugin. |
| `--arcade`, `--no-arcade`, `--pixel-depth`, `--no-pixel-depth` | The Arcade picture and depth per pixel. |
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
| `--ggpo-lobby URL\|default`, `--ggpo-user`, `--ggpo-pass`, `--ggpo-signup`, `--ggpo-game` | GGPO lobby sign-in. |
| `--ggpo-challenge NAME`, `--ggpo-accept NAME` | Challenge someone, or accept their challenge. |
| `--ggpo-local PORT`, `--ggpo-remote IP:PORT`, `--ggpo-player 1\|2` | A GGPO match without the lobby. |
| `--ggpo-delay N` | GGPO input delay (default 2). |
| `--ggpo-relay`, `--ggpo-ws` | Through the lobby's relay, or its WebSocket. |
| `--ggpo-synctest N`, `--ggpo-synclog DIR` | Rollback checks (development). |

Options for development: `--texload-i960`, `--spin-i960`, `--i960-timing table|board`,
`--no-mesh-cache`, `--cpu-tiles`, `--steps-per-slice N`, `--net-p2p-port N` (the peer-to-peer
UDP port, for two clients on one machine in a loopback test). `--gems-i960`, `--gems-cop`,
`--gems-verify`, `--ages-i960` and `--ages-verify` run some of the board's code as Sega's own C
from the Gems Collection and Sega Ages ports, which is not in the repo; a build without it says
so. `--realirq` and `--live-timers` are accepted and do nothing.

### Handheld

`--rom`, `--profile`, `--region`, `--enemy-rank`, `--vs-mode`, `--wheel-*`, `--gun-flash`,
`--match-replay`, `--no-nvram`, `--nvram-dir`, `--log`, `--log-level`, `--sound-hle`,
`--scripts`, `--no-scripts`, `--netplay`, `--net-config`, `--net-delay`, `--net-host` and
`--net-room-pass` work as on the desktop. The handheld adds:

| Option | Effect |
|---|---|
| `--sound` | Run the sound board (off without it). |
| `--render-fps N` | Draw rate (default 30). |
| `--render-scale N` | Draw scale (0 = the screen's size). |
| `--display-scale N`, `--window WxH`, `--filter nearest\|linear` | Picture size and scaling. |
| `--osd` | Show fps, temperature, time and battery. |
| `--pad-map LIST` | Map the pad's buttons. |
| `--ffb-gain N` | Force feedback strength. |
| `--script-option CODE` | Turn on a game script's option. |
| `--versus`, `--pad N`, `--window-at X,Y` | Virtual-On Versus on one machine. |
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
| `M2HLE_GEMS_VERIFY_ALL=1` | `--gems-verify` checks every call. |
| `M2HLE_SCRIPT=449:c,460:,...` | Player 1's inputs by frame, as the browser's `?script` (RetroArch core only). |
| `M2HLE_RPCN_AUTOJOIN=1\|NAME` | Join any open room, or NAME's, once signed in (RetroArch core only). |
