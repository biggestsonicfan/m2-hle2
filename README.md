# m2-hle2

A cross-platform **Sega Model 2 arcade emulator** written in C11, with a Dear ImGui debug
front-end on top of Sokol. *Sonic The Fighters* is the reference title, but the board layer
targets the wider Model 2 / 2A-CRX / 2B-CRX catalogue — *Fighting Vipers* boots on the same
code, and a homebrew ROM runs on it too.

The emulator is **HLE** (high-level emulation): the i960 game code is interpreted for real,
while the geometry coprocessor, parts of the frame loop, and the audio path are intercepted
and reimplemented in C rather than simulated gate-for-gate.

```
git clone --recurse-submodules <this repo>       # or: git submodule update --init
python -m pip install ply==3.11                  # dear_bindings generates the ImGui C bindings
cmake -S . -B build
cmake --build build --config Release -j
build/Release/m2hle.exe --rom <romset>.zip --run
```

Dependencies are git submodules under [vendor/](vendor/); `vendor/noclip` is only needed by
[tools/](tools/), so `git submodule update --init vendor/imgui vendor/dear_bindings vendor/sokol
vendor/miniz vendor/ImGuiFileDialog vendor/imgui_club` is enough to build.

That is the default frontend, the ImGui debugger (D3D11 on Windows, GL core on Linux, Metal on
macOS). `-DM2HLE_FRONTEND=sdl3` builds a fullscreen SDL3 / GLES 3 host with no ImGui for
handhelds (see [packaging/rocknix/](packaging/rocknix/)), and `-DM2HLE_FRONTEND=web` the
Emscripten browser build that deploys to play.sonicthefighte.rs, with online play, remappable keys, gamepads and
touch buttons (see [WEB-PORT.md](WEB-PORT.md) and [WEB-NETPLAY.md](WEB-NETPLAY.md)); both need
only `vendor/sokol` and `vendor/miniz`.

No ROMs, ROM-derived data, or other copyrighted material is included in this repository, and
none will be accepted into it. You must supply your own dumps.

---

## Goal

Booting a game is not the goal; understanding it is. Taken seriously, HLE forces the issue — you
cannot intercept a subsystem until you know exactly what it does, so every hook is a standing
claim about the game's engine, and the emulator is where that claim gets tested against hardware.
The aim for each Model 2B ROM set is therefore a **map of its engine**: what subsystems exist,
what state each one owns, how a frame is assembled out of them, and which parts of that are board
behaviour shared with every other game on the same hardware.

For *Sonic The Fighters* — the reference title, and where the bulk of the prior reverse
engineering already lives — the aim is the complete version of that map: a fully fleshed-out
account of how the game actually works, end to end.

- **Frame loop and game state** — the attract / select / fight / result machine, what drives
  pacing, and what each per-frame entry point is responsible for.
- **Character records** — the layout of a fighter's state block: position, velocity, facing,
  health, the move and state fields, and the transitions between them.
- **Animation and the rig** — how motion data becomes a pose: the bone hierarchy and the COP
  command sequences (`0x1A803535`, `0x33806767`, the `0x35806B6B` two-bone IK) that place each
  joint.
- **Camera** — how the fight camera is framed from the two fighters, and the arena-bounds /
  ring-out classifier behind `0x38007070`.
- **Collision and hit detection** — the volumes, the tests, and where the result is written back.
- **Rendering** — the display list the game builds, object and material selection, texture and
  palette setup, the clip windows.
- **Sound** — the i960 → 68K command protocol, and what each request means to the driver.
- **Input** — the port reads, the button-history buffer, and how a move command is recognised.

The standard for anything entering that map is the same as everywhere else here: it is mapped when
it has been *measured*, not when it is plausible. Pieces that are named but not yet understood stay
labelled as such — [CLAUDE.md](CLAUDE.md) holds the confirmed invariants, and several of its
entries exist precisely because an earlier confident guess turned out to be wrong.

Anything general enough to be shared belongs in the board layer, so mapping one game's engine makes
the next game cheaper instead of being spent on a single ROM set.

## What works

| Subsystem | State |
|---|---|
| Intel i960 KB CPU core | Interpreted, boots STF and FV to gameplay |
| Memory bus | 36 regions, MMIO callbacks, board + game address maps |
| COP / ADSP-21060 SHARC | HLE math engine, ~90 commands, column-major post-multiply matrices |
| 2D tiles | System 24 tile compositor, palettes, per-tile priority against the 3D layer |
| 3D pipeline | Index-array polygon decoder (J = 1.0 vs. reference meshes), textures, flat + luma shading, backface cull, shadows |
| MC68000 sound CPU | Full opcode core with Motorola cycle timing, unit tests |
| SCSP audio | Register-level chip (slots, timers, DSP) run one sample at a time in lockstep with the 68000; host output via sokol_audio |
| Input | Interrupt-driven, through the real 315-5649 I/O ports; optional button macros (`--macros`, or `--macro a=b1+b2` per key; `--pad-map north=b1+b2` on the handheld; the Controls panel in the browser) |
| Debug UI | CPU / memory / bus stats / COP / 3D / object viewer / 68K / breakpoint windows |
| Netplay | RPCN matchmaking + direct peer-to-peer delay lockstep (`--netplay`) |
| Automation | In-process MCP bridge over TCP (`--mcp`) |
| Recording | Capture mode (`--kiosk`): chrome-free window at a fixed capture size, parked off the desktop, run from a tray icon |
| Streaming | Raw board video and audio on one socket and one clock (`--av-port`), and a plugin that paints over the picture (`--overlay`) |

Game profiles live in [src/profiles/](src/profiles/): `sfight`, `fvipers`, `m2snake` (the web
build carries `sfight` only).

## Layout

- [src/board/](src/board/) — everything shared by every Model 2 ROM set: CPU, bus, COP/SHARC,
  tile and 3D renderers, 68K, SCSP, IRQ/timers.
- [src/core/](src/core/) — ROM loading, profile resolution, HLE hook dispatch, emu thread,
  breakpoints/watchpoints, logging.
- [src/net/](src/net/) — netplay: the RPCN client (TLS, protocol, rooms, signaling), the
  lockstep engine, and the glue that gates the emulator's frame loop on it.
- [src/ui/](src/ui/) — ImGui debug windows, game render target, MCP bridge.
- [src/profiles/](src/profiles/) — one `game_profile_t` per ROM set (hook addresses, input map,
  ROM list + CRC32s, quirks).
- [tests/](tests/) — ten standalone CTest targets (bus, i960, ROM, emu, boot, COP, GEO, 68K, input,
  netplay), plus the `cop_replay` and `snd_replay` capture replays the graders drive.
- [mcp_server/](mcp_server/) — Python MCP server that drives a running emulator over the bridge.
- [tools/](tools/) — graders that measure this emulator against an independent implementation
  of the same ROM formats, with a MAME digest as the third point. See [tools/README.md](tools/README.md).
- [vendor/](vendor/) — dependencies, all git submodules pinned to an exact upstream commit:
  Dear ImGui, dear_bindings (generates the `ig*` C bindings into the build tree at build
  time — nothing generated is committed), Sokol, ImGuiFileDialog, imgui_club (the hex editor
  behind the memory viewers), miniz, and noclip.

Everything except the frontends' `main*.c` and `sokol_*impl.c/.m`, `ui/mem_edit.cpp`, and the submodules' `.c` files
is a header-only `.h` module. That is deliberate — see [CLAUDE.md](CLAUDE.md).

## The menu bar

While a game is running the main menu bar gets out of the way and the emulator
fills the window; touching the top edge brings it back. It stays put whenever
there is nothing to play or the board is paused — losing Run and Load ROMs on a
paused emulator would take them away exactly when you are reaching for them.
**Debug → Always show menu bar** pins it open for a session.

## Netplay

Two people can play the same cabinet over the internet. Matchmaking runs over
[**RPCN**](https://github.com/RipleyTom/rpcn), the community server for PlayStation Network
emulation — a TLS session for login and the room list, a UDP exchange to learn each other's
address, and then **direct peer-to-peer traffic that never passes through the server**.

Open the **Netplay** menu → *Netplay window*, or start with `--netplay`. The server box is
prefilled with `rpcn.sonicthefighte.rs`; any RPCN server works, so change it if you run your own.

**Sign in with Twitch** is the short way in: approve a code once in a browser and the server hands
back a login token that stands in for a password from then on, so it is stored and never asked for
again. Your Twitch login becomes your account name. A server without Twitch configured says so
plainly, and the account name / password fields below are the ordinary path — the window will
register an account for you if you have none, and can re-send the verification e-mail if the
server uses them.

Settings live in one file per user — `%APPDATA%\m2hle2\netplay.cfg` on Windows,
`~/.config/m2hle2/netplay.cfg` elsewhere — written once a login is known to work, and shared by
every copy of the emulator on the machine. The Twitch login token is stored there, because the
device flow exists precisely so it only happens once. A password you type is stored too, in clear
text, so an unattended host can sign itself back in. The server keeps one Twitch token per account,
so signing in again anywhere retires the old one; a file shared by all copies is what keeps one
sign-in from logging the others out. `--net-config <file>` uses another file, for a second account
on the same machine. An `m2hle_netplay.cfg` left in the working directory by an older build is
copied over the first time.

**A session is a cold boot, not a savestate.** When both players are ready, *both machines reset
the board* and every frame from power-on is played in lockstep. This emulator has no savestates,
and the only state two copies can be certain to share is the one a board is in a microsecond
after the power comes on — so that is where a match starts, exactly as two arcade cabinets
would. You watch the SEGA logo together, and from there the two boards are the same machine.

What makes that sound is the same property [tools/](tools/) already measures: identical inputs
from a reset produce identical state, frame for frame. Every packet also carries a hash of the
board's state at a frame boundary, so if the two ever *do* diverge, the window says so and names
the frame rather than letting the match quietly become two different games.

Inputs are delay-based lockstep modelled on the Sonic the Fighters PS3 netcode: each frame's
input is keyed by absolute frame number, every packet re-carries the last ten frames so a lost
datagram repairs itself, and the frame delay (default 2) buys that much network latency before
either side has to stall. Whichever key set you press locally drives *your* side of the cabinet,
so the guest plays on P2 without rebinding anything.

**Lobbies are per-game.** RPCN partitions everything by Communication ID, so each ROM set gets
one of its own (`M2HSNCFTR_00` for Sonic The Fighters) rather than every Model 2 game sharing a
list. The browser also shows YAMP's rooms for the same arcade game, greyed out and unjoinable:
YAMP plays the console port, so a cross-emulator match could never stay in sync, but an empty
lobby with people next door is worth telling apart from an empty one. Rooms made by the browser
build can be joined too: the web and desktop builds compute the same frames
([WEB-NETPLAY.md](WEB-NETPLAY.md), "Cross-play"), though joining a web room needs a desktop build
from after that.

Scriptable without the GUI, which is how it gets tested:

```
m2hle --rom sfight.zip --run --netplay       --net-server <host> --net-user <name> --net-pass <password> --net-host --net-start
m2hle --rom sfight.zip --run --netplay       --net-server <host> --net-user <other> --net-pass <password> --net-join <room id> --net-start
```

**Or without a person at the keyboard.** The same buttons are on the MCP bridge
(`netplay_status`, `netplay_connect`, `netplay_host`, `netplay_start`, ...), which is enough to
hold a lobby open, notice that somebody has joined and pressed Start, and accept the match. The
status reports that as `peer.ready` -- RPCN has no "ready" message, so what it really means is
"a peer is announcing a session this end has not begun", which is exactly what pressing Start
does. Two rules a scripted session has to respect: the board **cold-boots** when the barrier
releases, so getting back to a fight is coin-and-START like anybody else; and `write_memory` is a
**desync**, so everything during a session goes through `set_input`. See
[MCP_GUIDE.md](MCP_GUIDE.md#netplay-rpcn).

TLS is Schannel, so netplay currently connects only on Windows; [src/net/tls.h](src/net/tls.h)
is the one file a POSIX backend would go in. The browser build has no sockets at all: it reaches
RPCN through a WebSocket gateway on the RPCN host ([web/gateway/](web/gateway/)). The design follows
[yampnet](https://github.com/biggestsonicfan/YAMPnet), the netplay plugin for YAMP, which
worked the RPCN protocol out first.

## Recording (capture mode)

`--kiosk` is for putting the game on a stream or in a video. The emulator keeps a real window —
OBS's Game Capture hooks a process's swapchain, so `--headless` (no window, no GPU context at
all) can be scripted but never recorded — but the window has no title bar, no close box and no
minimise box, is fixed at the capture resolution, and parks itself just outside the desktop
where nothing can land on it. All that appears is a tray icon.

```
m2hle --rom sfight.zip --kiosk                     # hidden, 1920x1080, running
m2hle --rom sfight.zip --kiosk --kiosk-size 1280x960   # some other capture size
m2hle --rom sfight.zip --kiosk --kiosk-show        # same, but on screen at 0,0
```

In OBS: **Game Capture → Mode: Capture specific window → `[m2hle.exe]: m2-hle`**, with the
window mode set to match by executable. The source arrives at exactly the capture size whatever
the desktop resolution is. ("Capture any fullscreen application" only fires for a window that
covers a whole monitor, so it will not pick this one up unless the capture size happens to be
the monitor's.) The game is letterboxed inside the frame — Model 2 output is 496x384, so at
1920x1080 there are pillarbox bars; crop them in OBS, or pick a capture size of the same shape.

The tray icon's menu is the only way in or out: show or park the window, run or pause the
emulator, leave capture mode (which hands back the normal window and its menus), or exit. The
tooltip carries the presented frame rate, which is the quick answer to "is OBS still getting
frames". `File → Capture mode (OBS)` enters the same mode from a normally-launched session.
Windows only for now — see [src/ui/kiosk.h](src/ui/kiosk.h).

## Raw A/V out (`--av-port`)

`--kiosk` hands the picture to a screen capture, which leaves the sound somewhere else: the
display's clock and the audio device's clock are two unrelated clocks, neither of them the
board's, so the two drift and have to be lined up by ear. `--av-port` hands both to one client
on one socket, stamped with the board's own 44.1 kHz sample counter, so they are in sync by
construction — and the picture comes off an offscreen target at whatever size is asked for,
rendered natively rather than upscaled from the window.

```
m2hle --rom sfight.zip --kiosk --av-port 7180 --av-size 1396x1080
m2hle --rom sfight.zip --run --headless --av-port 7180 --av-mute   # no window at all
```

- **`--av-port N`** — listen on `127.0.0.1:N`, one client at a time, like the MCP bridge.
- **`--av-size WxH`** — the stream's resolution (default 1396x1080). The game is drawn across
  the whole target with no letterbox bars, so pick the board's 496:384 shape and nothing is
  rescaled anywhere; 1396x1080 is that shape to the nearest even pixel.
- **`--av-mute`** — do not open a host audio device. The stream is unaffected: the tap sits at
  the sound board's producer, ahead of the ring the device would drain.

There is no encoding, resampling, PNG or libav anywhere in the emulator — it hands over raw
frames and raw samples and nothing else. `tools/av-record.py` is a reference client that turns
them into an mp4 with ffmpeg; it is about a hundred lines, and reading it is the fastest way to
see the protocol.

**With `--headless`** the emulator brings up a graphics device with no window and no swapchain,
so no desktop session is needed — a server can stream. That path is D3D11 only for now; on a GL
build use `--kiosk`, which streams just as well from a parked window.

A headless run still gets a **tray icon**, because it has no window and no console of its own
once whatever launched it goes away — without one the only way to stop it is Task Manager, and
an orphan sits there holding its ports, its ROM and its A/V socket. The menu has the two items
that matter without a keyboard: restart the sound board, and exit. Exit does not kill the
process; it asks the loop to come down in the same order any other exit does, because the A/V
writer thread is still sending out of buffers the renderer owns. The tooltip carries the board's
frame rate and which ports this process answers on, so the icon says *which* emulator it belongs
to when several are running. `--no-tray` leaves it out, for a service or a Session 0 run where
there is no shell to put an icon in.

**The window mirrors the stream.** With a client connected, the game is rendered once, into the
capture target, and the window shows that target rather than drawing the frame a second time.
The stream itself never contains ImGui, the menu bar, letterbox bars or the cursor.

### The wire format

A 32-byte header once on connect, then packets. Everything is little-endian.

```
char magic[4] = "M2AV";  u16 version = 1;  u16 header_size = 32;
u16 width, height;       u32 pixfmt;      // the bytes 'B','G','R','A'
u32 fps_num, fps_den;                     // nominal board rate, informational
u32 audio_rate = 44100;  u8 channels = 2;  u8 bits = 16;  u16 reserved;

u8 type ('V'|'A');  u8 flags;  u16 reserved;
u32 size;  u64 frame;   // the counter get_status reports as "frames"
u64 sample;             // A: index of the first sample in this packet
                        // V: samples produced when the pictured frame ended
```

`sample` is the shared clock: a video frame's pts is `sample / 44100`. Nothing assumes 735
samples a frame or an exact 60 Hz — a game frame that takes two emulator slices really does
carry two slices of audio, and the stamps say so. `flags` bit 0 means something of *that*
stream was dropped before this packet. A video payload is `width*height*4` bytes of BGRA,
packed, top row first; an audio payload is `size/4` interleaved L,R `int16` frames, raw board
samples including the board's DC offset (about 5000 of 32768 — a real cabinet's amplifier is
AC-coupled, so take it out downstream with `highpass=f=5`).

Video may be dropped and the timestamps make that harmless. Audio may not: the ring holds about
six seconds, and only a client that has stopped reading for that long loses any.

`get_status` over the MCP bridge carries an `av` block — connected, frames and samples sent,
and *why* frames went missing, which is the number that says what to fix:

```json
"av": { "enabled": true, "port": 7180, "width": 1396, "height": 1080, "connected": true,
        "video_sent": 565, "video_dropped": 29,
        "dropped_queue": 0, "dropped_readback": 1, "dropped_missed": 28,
        "audio_sent": 438795, "audio_dropped": 0 }
```

`dropped_queue` is the client not keeping up, `dropped_readback` the GPU not finished with a
copy in time, `dropped_missed` the renderer never reaching that board frame. Measured here at
1396x1080: 95% of board frames delivered headless, 93% under `--kiosk` (where the display's
60 Hz beats against the board's), ~340 MB/s, and no audio dropped at all.

See [src/core/av_stream.h](src/core/av_stream.h) for the transport and the audio tap, and
[src/ui/av_capture.h](src/ui/av_capture.h) for the offscreen target and the asynchronous
readback ring.

## Overlays (`--overlay`)

Model 2 output is 496x384, so a 16:9 stream has 262 px of empty pillarbox either side at
1920x1080. `--overlay` loads a shared library that paints into them — a round counter, a ping
meter, a tournament lower-third, whatever the stream wants — and the emulator composites it over
the finished picture. Without the flag nothing anywhere behaves differently.

```
m2hle --rom sfight.zip --kiosk --av-port 7180 --overlay flyoverlay.dll
m2hle --rom sfight.zip --run --headless --av-port 7180 --overlay flyoverlay.dll --overlay-reload
```

- **`--overlay <path>`** — the library to load. It must export one symbol,
  `m2_overlay_query`, and nothing else.
- **`--overlay-args <string>`** — handed to the plugin verbatim, every frame. The host never
  parses it.
- **`--overlay-game WxH+X+Y`** — where the board goes inside the composed frame, when the
  default 496:384 letterbox is not what the scene wants.
- **`--overlay-reload`** — reload the library when it changes on disk, so a plugin can be
  rebuilt without restarting a live stream. The pixel buffers are host-owned and outlive the
  library, so the columns keep their last content across the swap and nothing blinks.

**The ABI is pixels, not draw calls.** The plugin is handed a set of premultiplied-BGRA buffers
the host owns and fills them; it never touches the GPU. Exporting sokol's `sg_*` state would weld
the plugin to the same sokol commit as the emulator, so a backend change here would silently
break a plugin built last month, and a command list to replay is a retained-mode 2D API to
invent, specify and version. Pixels cannot drift. Premultiplied, because straight alpha is what
puts a dark halo around every piece of white text sitting over the board.

Layers are separate so a repaint costs what it changed: one 262x1080 column is 1.1 MB, where the
whole 8.3 MB canvas would go up every frame — sokol has no partial image update. A plugin marks
a layer dirty when it repainted it, and an untouched layer costs a quad.

Nothing in the host knows what a layer contains, which is the point: an overlay that knows about
hyper meters belongs in a DLL that ships with whatever is drawing it, not in a board emulator.
A plugin that faults, misbehaves or returns a negative count disables itself with a warning and
the board carries on being presented — a live stream is not the place to find out. `get_status`
carries an `overlay` block saying whether it is loaded, how long its last paint took and how
many times it has reloaded.

The contract is [src/ui/overlay_plugin.h](src/ui/overlay_plugin.h), about a hundred lines, and it is
meant to be *copied* into a plugin's tree rather than shared through a submodule; check
`M2_OVERLAY_ABI` with a `_Static_assert` so a skew is a build error and not a blank overlay. The
host side is [src/ui/overlay_host.h](src/ui/overlay_host.h).

## Documents

- [CLAUDE.md](CLAUDE.md) — the load-bearing invariants: facts that were reverse-engineered or
  debugged out of the hardware and appear in no datasheet. Read this before changing the CPU,
  COP, or polygon decoder.
- [IMPLEMENTATION-DRAFT.md](IMPLEMENTATION-DRAFT.md) — the authoritative, dependency-ordered
  build plan, written *after* the first implementation, as the document to hand to a cold
  restart. Supersedes PROPOSAL.md where they disagree.
- [PROPOSAL.md](PROPOSAL.md) — the original architecture proposal, kept for context.
- [MCP_GUIDE.md](MCP_GUIDE.md) — the emulator's automation protocol and tool reference.
- [tools/README.md](tools/README.md) — the grading harness: what it measures, what it cannot,
  and the numbers it currently reports.

---

## How this project evolved

Three generations, each a deliberate restart rather than a refactor:

**1. `stf-hle` — the prototype (early 2026).** A *Sonic The Fighters*-only emulator with a flat
file layout, built on a Sokol + cimgui starter template. It proved the approach — an i960
interpreter plus HLE hooks really can boot a Model 2 game — and produced the first working
register-window logic, COP math, polygon decoder, and memory-region table. It is not part of
this repository's history and is now superseded.

**2. `m2-hle` — the full implementation (2026-05-19 → 2026-06-03, 78 commits).** A rewrite around
a hard **board layer vs. game-profile layer** split, on the bet that most "STF bugs" were really
Model 2 board bugs shared by the whole catalogue. That bet paid off repeatedly. The arc:

- *May 19–21* — bus, i960 core, ROM loader, emu thread, HLE hooks, first boot, then the COP.
  The COP immediately dominated: a `sharc.h` / `sharc_exec.h` split, real sin/cos, a push/pop
  matrix stack, and the discovery that rotation is **accumulated by post-multiply**, not rebuilt
  from stored angles — the single correction that unblocked cameras, bones, and stages.
- *May 22–24* — bone matrix cache, GEO FIFO window capture for the character-select portraits,
  tile palette-bank fixes (which is what finally drew the SEGA logo), frame interpolation.
- *May 25–26* — an MC68000 core and an SCSP HLE mixer, then **Fighting Vipers** as the second
  ROM set. Adding a second game is what validates a board layer; FV booted on shared code.
- *May 26 – Jun 1* — the accuracy grind: 30+ COP commands verified against real hardware, then
  against the SHARC firmware itself, which was disassembled and annotated in-repo. A dozen
  handlers that had shipped as plausible stubs turned out to be wrong (`fmul` returning `a`
  instead of `a*b`; `asin` mislabelled `atan`; `rotate2D` a pass-through).
- *Jun 2–3* — an offline COP-stream replay harness, then two 68K decode bugs found by unit
  tests (`JMP` decoded as `JSR`, `SWAP` as `PEA` — each silently corrupting the stack) and
  live BGM playback.

**3. `m2-hle2` — this repository (2026-06-06 → present, 156 commits).** A clean from-scratch
rebuild following IMPLEMENTATION-DRAFT.md's phase order, carrying the known-good invariants
forward and leaving the dead ends behind. It opened at feature parity — STF and FV booting with
3D, tiles, and audio — and the commits since are the hard remainder:

- *Jun 7* — the attract-mode camera dig (`0x35006A6A` world→model corrected to the true inverse
  `Rᵀ(v − T)`), STF texture UV orientation and per-pixel wrap, per-face flat shading, the full
  MAME colorxlat luma ramp, backface culling, real timer-IRQ delivery, and two genuine COP bugs
  found en route: an undersized GEO capture ring and a SHARC FIFO desync from a miscounted
  command.
- *Jun 8* — ground shadows, a clean `set_windows` redo with decoupled clip events, head-on
  character-select cells.
- *Jun 12* — **homebrew bring-up**: a Snake/Tetris/Pong homebrew ROM built against the STF data
  ROMs now renders 3D through the real GEO display list. That exposed a board-level CPU bug the
  commercial games never tripped — the i960 core did not decode the `+0.0` / `+1.0` FP literals,
  so every `1.0 - x` silently became `-x` and all homebrew 3D collapsed. Also: per-tile bit15
  priority against the 3D layer, the GEO `LIGHT` command, and per-object colorbase.

The through-line is the working method, not the feature list: **build the oracle before the
thing it judges.** Reference meshes for the polygon decoder, a scripted MAME session for the CPU
and COP, and the live debug UI for anything stateful. Datasheet reasoning lost to differential
testing every single time.

---

## The role of AI

This project was built almost entirely as a human–AI pair. Direction, hardware knowledge, ROM
dumps, prior reverse-engineering, and every acceptance decision are the author's; the
implementation, the debugging loops, and the documentation were driven with
[Claude Code](https://claude.com/claude-code). Nearly all 234 commits across `m2-hle` and
`m2-hle2` carry a `Co-Authored-By: Claude` trailer (Sonnet 4.6, then Opus 4.7 / 4.8 / 5 and
Fable 5.1 as they shipped).

What made that work is that the model was given **instruments, not just a prompt**:

- **An MCP bridge inside the emulator.** `--mcp` opens a TCP JSON server; the Python server in
  [mcp_server/](mcp_server/) re-exposes it as MCP tools — registers, memory read/write,
  run/stop/step, breakpoints, COP diagnostics, the GEO capture list, raw command-stream dumps.
  The model drives and inspects the live emulator directly instead of guessing from source.
  Protocol in [MCP_GUIDE.md](MCP_GUIDE.md).
- **An object viewer on the same bridge.** `--objview`, or Debug -> Object viewer, draws one
  model by itself offscreen, from a camera the caller places, and writes a PNG per angle --
  a turntable or the six axis views in one call. Chasing a visual artifact no longer means
  steering the game into the scene that draws it and then fighting the game for the camera,
  and each shot reports how much of the frame the object covered so a miss costs no round
  trip. See [MCP_GUIDE.md](MCP_GUIDE.md#object-viewer-screenshots-of-one-model-from-any-angle).
- **A MAME ground-truth oracle.** A separate harness (not in this repo) runs a Lua bridge inside a
  symbols build of MAME's `model2.cpp` driver and exposes its debugger over MCP. Any disagreement
  between this emulator and real hardware behaviour becomes a diffable trace: set a watchpoint,
  run both, compare registers. IMPLEMENTATION-DRAFT.md calls it the single highest-leverage tool
  in the effort, and the COP/SHARC and 68K work would not have converged without it.
- **Disassembly in the loop.** The IDA disassembly of the STF program ROM and the SHARC
  coprocessor firmware were read directly, annotated, and committed — several COP handlers were
  corrected by reading the firmware's dispatch table rather than by inference.
- **A durable invariants file.** [CLAUDE.md](CLAUDE.md) exists because the expensive facts kept
  getting re-derived — and re-derived *wrong* — across sessions. It is deliberately written as
  "things that cost days and are in no datasheet," including corrections to its own earlier
  entries, so each new session starts from the current best understanding rather than from the
  most plausible one.

The honest limits are worth stating too. The model was fast at breadth — porting a subsystem,
sweeping a parameter space, writing the harness that tests the thing — and reliably wrong when
it reasoned from plausibility instead of measurement. Every load-bearing invariant in CLAUDE.md
that had to be *corrected* was originally a confident guess. The workflow that worked was to make
guessing expensive and measurement cheap: stand up the oracle first, then let the model iterate
against it.

---

## Licence

The dependencies under [vendor/](vendor/) keep their own licences. No licence has been
chosen for the first-party code yet.
