# m2-hle2

A cross-platform **Sega Model 2 arcade emulator** written in C11, with a Dear ImGui debug
front-end on top of Sokol. *Sonic The Fighters* is the reference title, but the board layer
targets the wider Model 2 / 2A-CRX / 2B-CRX catalogue — *Fighting Vipers* boots on the same
code, and a homebrew ROM runs on it too.

The emulator is **HLE** (high-level emulation): the i960 game code is interpreted for real,
while the geometry coprocessor, parts of the frame loop, and the audio path are intercepted
and reimplemented in C rather than simulated gate-for-gate.

```
cmake -S . -B build
cmake --build build --config Release -j
build/Release/m2hle.exe --rom <romset>.zip --run
```

No ROMs, ROM-derived data, or other copyrighted material is included in this repository, and
none will be accepted into it. You must supply your own dumps.

---

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
| Automation | In-process MCP bridge over TCP (`--mcp`) |

Game profiles live in [src/profiles/](src/profiles/): `sfight`, `fvipers`, `m2snake`.

## Layout

- [src/board/](src/board/) — everything shared by every Model 2 ROM set: CPU, bus, COP/SHARC,
  tile and 3D renderers, 68K, SCSP, IRQ/timers.
- [src/core/](src/core/) — ROM loading, profile resolution, HLE hook dispatch, emu thread,
  breakpoints/watchpoints, logging.
- [src/ui/](src/ui/) — ImGui debug windows, game render target, MCP bridge.
- [src/profiles/](src/profiles/) — one `game_profile_t` per ROM set (hook addresses, input map,
  ROM list + CRC32s, quirks).
- [tests/](tests/) — nine standalone CTest targets (bus, i960, ROM, emu, boot, COP, GEO, 68K, input).
- [mcp_server/](mcp_server/) — Python MCP server that drives a running emulator over the bridge.
- [tools/](tools/) — graders that measure this emulator against an independent implementation
  of the same ROM formats, with a MAME digest as the third point. See [tools/README.md](tools/README.md).
- [vendor/](vendor/) — cimgui (dear_bindings), Sokol, ImGuiFileDialog, miniz, vendored in tree.

Everything except `main.c`, `sokol_impl.c/.m`, and the vendored `.c` files is a header-only
`.h` module. That is deliberate — see [CLAUDE.md](CLAUDE.md).

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

The vendored dependencies under [vendor/](vendor/) keep their own licences. No licence has been
chosen for the first-party code yet.
