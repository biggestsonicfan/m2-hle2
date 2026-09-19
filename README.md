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
| Memory bus | 31 regions, MMIO callbacks, board + game address maps |
| COP / ADSP-21060 SHARC | HLE math engine, ~60 commands, column-major post-multiply matrices |
| 2D tiles | System 24 tile compositor, palettes, per-tile priority against the 3D layer |
| 3D pipeline | Index-array polygon decoder (J = 1.0 vs. reference meshes), textures, flat + luma shading, backface cull, shadows |
| MC68000 sound CPU | Full opcode core with unit tests |
| SCSP audio | HLE PCM mixer, BGM playback via sokol_audio |
| Input | Interrupt-driven, through the real 315-5649 I/O ports |
| Debug UI | CPU / memory / COP / GEO / 68K / trace / breakpoint / video windows |
| Netplay | RPCN matchmaking + direct peer-to-peer delay lockstep (`--netplay`) |
| Automation | In-process MCP bridge over TCP (`--mcp`) |

Game profiles live in [src/profiles/](src/profiles/): `sfight`, `fvipers`, `m2snake`.

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
- [tests/](tests/) — nine standalone CTest targets (bus, i960, ROM, emu, boot, COP, GEO, 68K, input).
- [mcp_server/](mcp_server/) — Python MCP server that drives a running emulator over the bridge.
- [tools/](tools/) — graders that measure this emulator against an independent implementation
  of the same ROM formats, with a MAME digest as the third point. See [tools/README.md](tools/README.md).
- [vendor/](vendor/) — dependencies, all git submodules pinned to an exact upstream commit:
  Dear ImGui, dear_bindings (generates the `ig*` C bindings into the build tree at build
  time — nothing generated is committed), Sokol, ImGuiFileDialog, imgui_club (the hex editor
  behind the memory viewers), miniz, and noclip.

Everything except `main.c`, `sokol_impl.c/.m`, `ui/mem_edit.cpp`, and the submodules' `.c` files
is a header-only `.h` module. That is deliberate — see [CLAUDE.md](CLAUDE.md).

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

Settings live in `m2hle_netplay.cfg` in the working directory, written once a login is known to
work. The Twitch login token is stored there, because the device flow exists precisely so it only
happens once; a typed password never is.

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
lobby with people next door is worth telling apart from an empty one.

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
is the one file a POSIX backend would go in. The design follows
[yampnet](https://github.com/biggestsonicfan/YAMPnet), the netplay plugin for YAMP, which
worked the RPCN protocol out first.

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

**3. `m2-hle2` — this repository (2026-06-06 → present, 35 commits).** A clean from-scratch
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
[Claude Code](https://claude.com/claude-code). Nearly all 113 commits across `m2-hle` and
`m2-hle2` carry a `Co-Authored-By: Claude` trailer (Sonnet 4.6, then Opus 4.7 / 4.8 / 5 as they
shipped).

What made that work is that the model was given **instruments, not just a prompt**:

- **An MCP bridge inside the emulator.** `--mcp` opens a TCP JSON server; the Python server in
  [mcp_server/](mcp_server/) re-exposes it as MCP tools — registers, memory read/write,
  run/stop/step, breakpoints, COP diagnostics, the GEO capture list, raw command-stream dumps.
  The model drives and inspects the live emulator directly instead of guessing from source.
  Protocol in [MCP_GUIDE.md](MCP_GUIDE.md).
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
