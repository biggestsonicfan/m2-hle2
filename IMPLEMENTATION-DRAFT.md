# IMPLEMENTATION-DRAFT.md — Rebuilding the Model 2 HLE Emulator from Scratch

This is the document I would write to myself before re-implementing this project cold.
It supersedes the optimistic 14-phase checklist in [PROPOSAL.md](PROPOSAL.md): the real
build grew a SHARC geometry-coprocessor core, a full MC68000 sound-CPU core, an SCSP
audio path with its own thread, board IRQ/timer hardware, and — most importantly — a
**MAME ground-truth verification harness** that turned out to be the single highest-leverage
tool in the entire effort. PROPOSAL.md treats the COP as "left-handed Ry×Rx×Rz rebuild";
that model is now known to be **wrong** (it only coincidentally matched a clean ang
sequence). The truth is post-multiply column-major accumulation, recovered by reading
SHARC firmware and diffing against MAME. A rebuild that doesn't stand up the verification
harness *first* will re-walk every one of those dead ends.

Read alongside [CLAUDE.md](CLAUDE.md) (load-bearing invariants) and the auto-memory index
`MEMORY.md`. Where this doc and PROPOSAL.md disagree, this doc wins.

---

## 0. Guiding principles (learned the hard way)

1. **Build the oracle before the thing it judges.** The MAME MCP bridge (Python async
   client driving a real MAME Model 2 session) is what made COP/SHARC/sound debugging
   tractable. Differential testing against real hardware behaviour beats staring at
   datasheets every single time. Stand it up in Phase 1, not Phase 14.
2. **Default every fix to the board layer.** Almost every "STF bug" was an i960 / COP /
   tile bug shared by the whole catalogue. Only move a fix into a `game_profile_t` quirk
   with *positive evidence* another game wants the opposite behaviour.
3. **Three independent validation oracles, used continuously:**
   - **Reference OBJ Jaccard** for the polygon decoder (`C:\m2\3d\new\stf-poly`, 4405 files).
   - **MAME MCP lockstep** for CPU cores, COP/SHARC math, SCSP behaviour.
   - **The live ImGui debug UI** for everything stateful (regs, memory, capture rings).
4. **Header-only, single TU.** Every module except `main.c`, `sokol_impl.c/.m`, and the
   vendored `.c` files is a `.h`. This is intentional; keep it.
5. **Don't re-derive the invariants in §3.** They each cost days. They are not in any
   datasheet.

---

## 1. What "done" means (milestones)

- **M0 — Skeleton boots.** ImGui window opens, mouse works (watch the cimgui ABI trap, §2),
  menus populate, no ROM loaded.
- **M1 — STF reaches attract.** i960 passes hardware init, `CoProcessorErr` is bypassed,
  timer IRQ faked, frame pacing drives `g_frame_done`, tiles composite to a recognizable
  Sega logo / attract screen.
- **M2 — 3D renders at J=1.0.** COP/SHARC capture stream feeds the geo3d decoder; models
  4402 & 4405 match the reference OBJ set at Jaccard 1.0.
- **M3 — Audio plays.** 68K sound ROM runs, SCSP key-on/off events drive the HLE PCM mixer,
  BGM is audible with correct pitch/pan.
- **M4 — Second game shakeout.** Fighting Vipers (same 2B-CRX, profile already exists) and
  ideally Daytona/VF2 boot far enough to exercise i960 + COP free of STF's HLE bypasses.

The current tree has reached roughly M1–M3 with ongoing 3D and audio accuracy work
(see the `project_*` auto-memories: rocket-metal z-index, IK rotation bug, egg-disp head
window, ghosting fix).

---

## 2. Tech stack & the traps inside it

| Concern | Choice | Trap |
|---|---|---|
| Language | C11, single TU | — |
| UI | Dear ImGui via **dear_bindings flat drop** (imgui 1.92.6) | **Do NOT use the `cimgui/cimgui` submodule.** Its generated C header had an `ImGuiIO` ABI mismatch: `MousePos` updated but `MouseDown` stayed 0 → UI looks alive but ignores clicks. Use a dear_bindings flat drop where `cimgui.cpp` is compiled alongside the imgui `.cpp` it binds. |
| Graphics/windowing | **Sokol** (app/gfx/imgui/glue/audio) | One TU defines `SOKOL_IMPL`. macOS build must compile it as Obj-C (`sokol_impl.m`, `-x objective-c -fobjc-arc`). Backend per platform: D3D11 (Win), GLCORE (Linux), Metal (mac), GLES3 (Emscripten). |
| Audio | **sokol_audio** | Callback runs on a **separate audio thread**. All cross-thread comms via SPSC ring (§9). |
| File dialogs | ImGuiFileDialog | — |
| ROM zip | miniz | Build as plain static lib; stub `miniz_export.h` in the binary dir (GenerateExportHeader is bypassed). |
| Build | **VS 2026 MSBuild via `build.ps1`** | `cmake.exe` is NOT on PATH; use the VS-bundled one to *generate*, then build through `build.ps1` (MSBuild on `build/m2hle.vcxproj`, Release/x64, `/m:16`). Direct `cmake --build` / bare `msbuild` are unreliable here. |
| Web | Emscripten + WebGL2 | `-sUSE_WEBGL2=1 -sALLOW_MEMORY_GROWTH=1`, no filesystem. |

CMake structure: four static libs (`cimgui`, `imguifiledialog`, `miniz`, `sokol`) + the
`m2hle` executable (`WIN32` subsystem on Windows so there's no console). Include paths span
`src/`, `src/board`, `src/core`, `src/ui`, `src/profiles`. Link `winmm` on Windows (timers).

---

## 3. Load-bearing invariants — copy these verbatim, do not re-derive

These are the facts that cost trial-and-error. Grouped by subsystem. **The COP/SHARC
rotation model here is the corrected one; ignore PROPOSAL.md §8's "Ry×Rx×Rz rebuild".**

### 3.1 i960 KB/KA CPU core (board-level, every game)

- `chkbit` sets `CC_NO` (0x0) when the tested bit is 0 — **not** `CC_NE` (0x5). `bno`/`bo`
  depend on this; getting it wrong inverts all chkbit-driven branching. (Symptom that
  surfaced it: Espio's tongue drew every frame in STF attract.)
- `cmpobX`/`cmpibX` **always** update the condition code, even when not branching.
- MEM mode 0x5 is IP-relative: `effective = IP + 8 + disp`, `+8` from the **start** of the
  2-word MEM instruction.
- REG-format literal mode: when m1/m2/m3 flag literal, the 5-bit "register index" field
  IS the immediate (0–31), not a mask.
- FP-from-GPR is a **bit reinterpret** (`memcpy` semantics), never `(float)int`.
- `call`/`ret`: align SP to 64 bytes `(sp+63)&~63`, zero new locals, save pfp/sp/rip,
  sync g15 (frame pointer) every call; `ret` restores all locals.
- Register-pair (`reg_quad`) ops are **big-endian** though the CPU is little-endian overall.

### 3.2 SHARC ADSP-21060 geometry coprocessor (board-level)

The COP is a real ADSP-21060 running geometry firmware. We HLE it as a command interpreter.
`cop.h` owns the i960↔SHARC FIFO (arg accumulator + geo-capture ring); `sharc.h`/`sharc_exec.h`
own the internal math, mirroring the `i960.h`/`i960_exec.h` split.

- **Rotation is accumulated by post-multiply, not rebuilt from stored angles.**
  `rot[3][3]` is **column-major** (SHARC convention): `rot[col][row]`. Each ang command
  post-multiplies the running matrix:
  - `ang_y` (`0x04800909` → PM `0x201BF`): `col0' = c·col0 + s·col2`, `col2' = −s·col0 + c·col2`
  - `ang_x` (`0x04000808` → PM `0x201AA`): `col1' = c·col1 − s·col2`, `col2' = s·col1 + c·col2`
  - `ang_z` (`0x05000A0A` → PM `0x201D4`): `col0' = c·col0 − s·col1`, `col1' = s·col0 + c·col1`
  - Verified from the SHARC firmware dispatch table at DM[0x30000] (`C:\temp\sharc_bone.asm`).
  - **PROPOSAL.md had ang_x/ang_z PM addresses and formulas swapped.** Fixed here.
- Angles are signed 16-bit fixed-point, `0x10000 = 360°`; only low 16 bits meaningful.
  Mask with `(int16_t)(aw & 0xFFFF)` before `cosf/sinf` or large spins overflow and tumble
  attract portraits.
- `pos[]` is accumulated translation T. `0x01800303` (identity) resets T to (0,0,0);
  `0x03000606` (set_pos) does `T += rot × args` (**additive**, not replacement).
- Z-negation convention for `0x14802929` / `0x35006A6A`:
  `rx = M[0]·(ix,iy,−iz)+T[0]`, `ry = M[1]·(ix,iy,−iz)+T[1]`, `rz = M[2]·(ix,iy,+iz)+T[2]`
  (z row uses raw `+iz`). Verified cases 6 & 9 in `verify_14802929_mame.py`.
- `0x07800F0F` returns **world translation T[0..2]**, not rotation. Firmware uses a
  `DM(I7, 0x09)` post-modify-by-9 to position at slot[9]=T[0], then an LCNTR=3 loop emits
  T[0..2]. i960 stores to `g7+0x1F4` for collision/IK. (STF PM 0x02043D, FV PM 0x020402.)
- `0x06800D0D` **zeros** T[0..2] (same post-modify-by-9 positioning). Not rotation.
- `0x07000E0E` is a **no-op in HLE** — firmware writes 3 args to slot[1..3] but STF's args
  are i960 addresses and the subsequent identity reset + ang commands overwrite them.
- `0x1A803535` mirrors `rot_cache` into `tgp_bone[player*16+slot]`. The geo3d scanner reads
  `tgp_bone` when it sees `0x1B803737`; attract characters use `calc_unit_mat`+`0x1A803535`
  (not the `0x35806B6B` IK chain), so `tgp_bone` must be kept in sync on every save.
- `0x2F005E5E` is scalar-then-vector: arg0=scalar, args 1–3=vector → `(s·x, s·y, s·z)`.
- Bone scratch is column-major `[col0|col1|col2|T]`, evolves across successive `0x35806B6B`
  calls, reset-to-dirty when the main matrix changes.
- Log unknown opcodes at WARN; other games will need more dispatch entries.

### 3.3 3D polygon decoder (board-level — cross-validated on two games)

Index-array decoder, J=1.0 on STF 4402/4405 **and** Daytona 2377. The cross-game match is
what proves it's the board's format, not an STF quirk.

- `iFlag = vp[25] & 0x03`: 0=sentinel (end strip), 1=carry far edge (`Index[-4]`,`Index[-2]`),
  2=plain new quad, 3=anchor new strip (`f1==1`→`Index[-1]` else `Index[-2]`; `anchor_b=Index[-3]`).
- Face loop: `i < n_idx - 8` (always 2 groups behind tail).
- Face type: `f1==2` → triangle; else quad with **A-B-D-C** winding.
- Vertex convention: `(x, y, −z)` — Z negated on read. Forgetting flips front/back and
  reverses winding.
- Model-table mesh pointer is encoded: ROM offset = `(ptr*4) − 0x02000010 + 0x10` for STF.
  The `*4` is hardware; the base may shift per game — verify before trusting elsewhere.
- The simple rule `(vp[25]&3)!=2` only reaches J≈0.708. The empirically correct
  `connect_when` mask for STF is `0x45B4`, found by brute force against reference OBJs.
  **Do not** replace with a fan/bitmask heuristic (peaked ≈0.71). Re-run the bruteforce per
  new game.

### 3.4 Memory bus (board-level)

- Region table is **linear-scanned in declaration order**; overlaps resolve by order.
  TILE (`0x01000000`) MUST precede H_SYNC (`0x01040000`).
- IO region initializes to `0xFF` (hardware idle), not `0x00`.
- `GEO_CAPTURE_SIZE` ≥ 32768 — smaller wraps mid-frame → partial 3D snapshots / flicker.

### 3.5 Tile renderer (board-level) — palette format corrected

- 16-bit byteswap on pixel bytes: indices `[0,1,2,3]` read as `[1,0,3,2]` (XOR low bit of
  byte index). High nibble = left pixel, low nibble = right.
- Tilemap entry: full tile index `entry & 0x3FFF`. **`pal_bank = (entry >> 7) & 0xFF`**
  (bit14 is part of the palette field, **not** h_flip as an early note claimed). Palette LUT
  index = `pal_bank*16 + color_idx` (stride 16 entries / 32 bytes per bank). Backdrop =
  `palette[0]`. Verified: CG87 palette at pal+0x660 = bank 51×32, entry pal_bank=51. This is
  for **2D tiles only** — it does not touch 3D model textures.
- Color index 0 transparent on **foreground** layers only; background layers fully opaque
  (pass `NULL` for `alpha_out`).
- Sega System 24 tile path (FV adv_name, STF adv_movie/logo): two layer pairs
  (A=fg regs 0x5000/0x5004, B=bg regs 0x5002/0x5006); hscr bit15=per-row, vscr/ctrl bit15
  =disable + bits[14:13]=window. `render_sys24_pair` handles both.

### 3.6 Sound (board-level blocks, per-game 68K code)

- 68K sound ROM at `0x600000`; wave RAM 512KB at `0x000000`; SCSP 4KB register window at
  `0x100000`. 32 SCSP slots × 0x20 bytes each.
- SCSP sample clock = 22.5792 MHz / 512 = **44100 Hz**. PCM8B=0 → 16-bit signed **big-endian**;
  PCM8B=1 → 8-bit signed. SA=byte offset into wave RAM; LSA/LEA = sample indices from SA;
  loop wraps LEA→LSA.
- **Two-part STF audio root cause:** (a) the i960 enqueues music but `send_sound_code` never
  ran because our HLE shortcuts the i960 IRQ dispatcher; (b) `Int2_Timer` to the 68K was
  missing. Both must be supplied for BGM to start.
- Model 2 IRQ/timer hardware (MAME-sourced, see `reference_model2_irq_timer_hw.md`):
  i960 IRQ controller at `0xE80000/0xE80004`, 4 board timers at `0xF00000` (25 MHz), with a
  pin→handler map; vblank + sound-UART IRQs; SCSP 68K timer formula. Implement real timers
  rather than a single faked flag once you're past first boot.

### 3.7 HLE hooks (game-specific addresses, board-level patterns)

- `CoProcessorErr` (STF `0x74E4`) bypass is mandatory — boot self-test compares COP buffers
  and always fails under HLE. Return via `locals.rip` (saved frame return address), NOT a
  normal IP advance. Every Model 2 game has an equivalent; find by symptom (hang at logo).
- Timer IRQ flag (STF `0x50008C`): write `0x01` to unblock the polled wait.
- Frame pacing: STF's `variable_diff_calc` (~`0x11A04`) sets volatile `g_frame_done`.
- `hle_ret()` must restore the i960 register window exactly like the `ret` instruction —
  otherwise the frame stack leaks depth on every hooked call.
- VsyncScr injection for `draw_health_bars`: `hle_call` into `0x0C40` from the
  interrupt_wait/idle hooks; RAM_BASE must be 0, not pre-incremented.

### 3.8 Threading

- Two threads under `emu_mutex`: UI (Sokol, 60Hz) and emu (i960 at ~25MHz). Audio callback
  is a third thread, fed only through the SPSC ring.
- **Unlock the mutex BEFORE sleeping.** Sleeping in the critical section freezes the UI.
- Double-buffered CPU snapshot (`cpu_snapshot` + `cpu_prev_snapshot`); UI reads current.
- `EMU_STEPS_PER_SLICE = 500000` instructions/slice — sized to always reach a frame boundary.
- Sleep granularity: Windows `Sleep()` ≈ 1ms; POSIX `usleep()` ≈ 1µs.

---

## 4. Module map (what to build, where)

Mirrors the current tree. Header-only unless noted.

```
src/
  main.c                     entry (sokol_main), window/menu orchestration, frame loop   [.c TU]
  sokol_impl.c / .m          SOKOL_IMPL TU (Obj-C on mac)                                [.c/.m TU]
  board/
    constants.h              memory bases, region sizes, PRCB offsets, CC codes, limits
    memory.h                 memory_bus_t, region table, mem_read/write 8/16/32, MMIO cbs
    i960.h                   i960_cpu_t state (globals/locals/SFRs/FP/frame stack)
    i960_exec.h              MEM/REG/CTRL/COBR decode + execute
    cop.h                    cop_state_t: i960↔SHARC FIFO, arg accumulator, geo-capture ring
    sharc.h                  SHARC internal state + math/reply helpers
    sharc_exec.h             SHARC command dispatch + handlers
    geo3d.h                  capture-stream scanner + J=1.0 polygon decoder + wireframe build
    tile_renderer.h          2D tile/sprite compositor (+ System 24 path)
    m68k.h                   MC68000 sound-CPU state
    m68k_exec.h              MC68000 decode/execute
    scsp_hle.h               SCSP HLE PCM mixer + SPSC event ring + audio callback
    sound.h                  sound block: 68K bus, SCSP register window, BGM driver glue
    irq_timer.h              i960 IRQ controller + 4 board timers (real hardware model)
    input.h                  keycode→action map, held/momentary bitmasks, coin, flush
  core/
    log.h                    ring logger + on-disk session log + break-on-warn
    game_profile.h           game_profile_t: variant, load/install fns, hooks[256], input map, quirks
    rom_loader.h             miniz zip extract, CRC32, interleave/swap, clone-parent fallback
    hle_hooks.h              profile-driven hook dispatch, hle_ret/hle_call, g_frame_done
    emu_thread.h             threading, run loop, double-buffered snapshot, frame pacing
    breakpoint.h             64-slot breakpoint table
    watchpoint.h             memory watchpoints
  profiles/
    registry.h               g_profiles[], g_active_profile, CRC32 resolution
    sfight.h                 STF: ROM load, install, hook table, input map, quirks
    fvipers.h                Fighting Vipers: second 2B-CRX profile
  ui/
    cpu_window.h             i960 reg/SFR/frame-stack view, changed-cell highlight
    m68k_window.h            68K reg view
    memview.h / m68k_memview.h  hex/ASCII inspectors (i960 / 68K)
    cop_window.h             COP/SHARC state + reply FIFO
    geo3d_window.h           3D viewer toggles, capture filters, camera sliders
    video_window.h           tile composite → sg_image (drawn into swapchain, no ImGui win)
    game_render.h            textured-quad + line GPU pipelines (GLSL+HLSL), letterbox
    debug_window.h           aggregate debug panel
    breakpoint_window.h / trace_window.h / log_window.h
    mcp_bridge.h             in-emulator side of the MAME verification harness
```

External tooling (not in the build, but essential):

```
mcp_server/                  Python MCP server + MAME bridge clients + per-opcode verifiers
disassembly/                 SHARC firmware annotation + label-verification scripts
```

---

## 5. Build order (dependency-correct phases)

Each phase ends at a checkpoint you can *see* or *diff*. Do not start a phase before its
deps compile and the prior checkpoint passes.

### Phase 0 — Skeleton + the cimgui trap
- `git init`, `.gitignore`, vendor submodules (`sokol`, `ImGuiFileDialog`, `miniz`) +
  **dear_bindings flat cimgui drop** (not the submodule — §2).
- `CMakeLists.txt` (four libs + exe), `build.ps1` (MSBuild VS2026).
- `main.c`: `sokol_main`, ImGui init, one empty window. **Checkpoint: mouse clicks register.**
  If buttons don't respond, you have the ABI mismatch — fix the cimgui drop now.

### Phase 1 — Infrastructure + **the MAME oracle**
- `constants.h`, `log.h` (ring + disk + break-on-warn), `game_profile.h` skeleton.
- **Stand up the MAME MCP bridge** (`mcp_server/`): async Python client driving a real MAME
  Model 2 session, exposing read-memory / step / read-COP-FIFO / read-SCSP. This is the
  oracle every later phase diffs against. Also wire `ida_*` bridge access for the STF IDA db
  and the SHARC firmware annotation scripts. (See `reference_mame_bridge.md`,
  `reference_ida_bridge.md`, `reference_sharc_label_verify.md`.)

### Phase 2 — Memory bus
- `memory.h`: ~31 named regions, declaration-order linear scan (TILE before H_SYNC!),
  MMIO R/W callbacks, IO inits to `0xFF`, idempotent `mem_init`, NULL-data read guard.
- `memview.h` hex inspector. **Checkpoint: write/read round-trips per region in the UI.**

### Phase 3 — i960 core
- `i960.h` state (16 global + 16 local regs with pfp/sp/rip aliases, SFRs, FP, frame stack),
  `i960_exec.h` decode/execute with every §3.1 invariant.
- `cpu_window.h`, `breakpoint.h`/`breakpoint_window.h`, `watchpoint.h`.
- **Checkpoint: lockstep the first N i960 instructions against MAME via the bridge.** This is
  where the harness earns its keep — find the first divergent instruction, fix, repeat.

### Phase 4 — ROM load + profile resolution
- `rom_loader.h`: miniz extract, CRC32 verify, `interleave_32_word`, `load_16_word_swap`,
  `rom_region_copy`, MAME clone child/parent zip fallback.
- `profiles/sfight.h` (load all 7 regions, CRC32-checked, hacked-ROM override; install =
  bus init + XTRA_DATA mirror + PRCB→IP bootstrap) and `registry.h`. CRC32 `72E66A1D`.
- **Checkpoint: STF loads, i960 starts at the reset vector.**

### Phase 5 — Emu thread
- `emu_thread.h`: Win32 CRITICAL_SECTION / POSIX mutex; STOPPED/RUNNING/STEPPING;
  `EMU_STEPS_PER_SLICE` loop; double-buffered snapshot; **unlock before sleep**;
  `g_frame_done` pacing; high-res clock; steps/sec stat.

### Phase 6 — HLE hooks + first boot
- `hle_hooks.h`: profile-driven dispatch, `hle_ret()` (restores frame window like `ret`),
  `hle_call()`, `g_frame_done`.
- STF boot-critical hooks in `sfight.h`: `CoProcessorErr` (0x74E4) bypass via `locals.rip`,
  timer-4 skip + flag write (0x50008C), interrupt_wait/idle nudges, 700000-loop skip,
  `variable_diff_calc` (0x11A04) frame flag.
- **Checkpoint (M1 begins): STF passes the Sega-logo hang and runs its main loop.**

### Phase 7 — COP + SHARC
- `cop.h`: arg accumulator, 32-slot reply FIFO, geo-capture ring (≥32768), MMIO bridge in
  `memory.h` routing COPROGRAM writes→`cop_write`, reads→`cop_read`.
- `sharc.h`/`sharc_exec.h`: column-major post-multiply rotation (§3.2), additive set_pos,
  all confirmed STF opcodes, unknown-opcode WARN log.
- `cop_window.h`. **Verify each opcode with its `verify_*_mame.py` against MAME** before
  trusting it. This is non-negotiable — cpres1 firmware alone diverges from STF's revision
  (`feedback_cpres1_diverges_from_stf.md`); use IDA i960 usage + MAME, not firmware alone.

### Phase 8 — 2D tiles + render path
- `tile_renderer.h` (§3.5, including System 24 path), `video_window.h` (CPU buffer →
  `sg_image`, drawn into the swapchain, no ImGui window), `game_render.h` textured-quad
  pipeline (GLSL+HLSL) + letterbox, drawn **before** `simgui_render` so debug windows overlay.
- **Checkpoint: recognizable attract tiles / Sega logo.**

### Phase 9 — 3D pipeline
- `geo3d.h`: capture-stream scanner (`0x02000404` matrix, `0x03000606` pos, `0x3C007878`
  object marker, `0x1B803737` bone draw, etc.), J=1.0 decoder (§3.3), per-frame wireframe
  builder with ring scoping (`geo_frame_start/end`) and translation lerp by frame counter
  at `0x500020` (the ghosting fix), `geo3d_log_captures`.
- Game knobs in `game_quirks_t`: `model_table_offset/count`, `mesh_ptr_subtract/add`
  (STF: 0xE0004 / 4373 / 0x02000010 / 0x10).
- Line pipeline in `game_render.h` (`game_render_draw_lines(cam,rot,fov)`), `geo3d_window.h`.
- **Checkpoint (M2): models 4402 & 4405 at Jaccard 1.0 vs `stf-poly`.** Compute Jaccard
  offline on demand, not in the run loop.

### Phase 10 — 68K sound CPU
- `m68k.h`/`m68k_exec.h`: full MC68000 core. (Watch the SWAP-vs-PEA decode bug — SWAP was
  being decoded as PEA, pushing to and corrupting the stack; see recent commits.) Build the
  standalone opcode unit tests (`tests/m68k_opcode_test.c`) and the MAME-vs-ours **lockstep
  harness** (`reference_m68k_lockstep.md`) — it proved the core correct and isolated the
  remaining sound bug to the SCSP timer HLE.
- `m68k_window.h`, `m68k_memview.h`.

### Phase 11 — SCSP + audio
- `irq_timer.h`: real i960 IRQ controller + 4 board timers; deliver `Int2_Timer` to the 68K
  and stop shortcutting the i960 IRQ dispatcher so `send_sound_code` actually runs (the
  two-part audio root cause, §3.6).
- `sound.h`: 68K bus, SCSP register window decode, BGM driver glue.
- `scsp_hle.h`: SPSC event ring (emu→audio thread), key-on/off → PCM voice mixer, pitch from
  OCT/FNS, pan from DIPAN, loop LEA→LSA, output float stereo in the sokol_audio callback.
- **Checkpoint (M3): BGM audible, correct pitch/volume, global key-on/off.**

### Phase 12 — Input
- `input.h`: sokol keycode→abstract action (arrows + Z/X/C/V/1/5 P1; I/J/K/L + nav + 2/6 P2;
  F2 Service, F3 Test), held/momentary bitmasks, `input_coin`, `input_flush` (writes held to
  RAM, ORs momentary). STF `read_sw` hook (0x17CC) clears 0x500700 so host `input_flush` is
  authoritative. STF/FV input RAM: held=0x500700, momentary=0x500704
  (`project_fvipers_input_ram.md`).
- `main.c`: route KEY_DOWN/UP (gated on key_repeat only), `input_flush` once per UI tick.

### Phase 13 — UI consolidation + cross-platform
- `debug_window.h` aggregate panel; finish menus.
- Linux (`X11 Xi Xcursor GL dl m` + pthreads), macOS (Obj-C, Quartz/Cocoa/Metal/AudioToolbox),
  Emscripten (WebGL2, no FS). Clang `-Wl,-dead_strip` on Apple.

### Phase 14 — Second-game shakeout
- Boot Fighting Vipers (`fvipers.h` exists) and ideally Daytona/VF2. Triage divergences:
  pure i960 → core; pure COP/SHARC → those modules; genuinely game-specific → profile quirks.

---

## 6. The verification harness (don't skip — this is the multiplier)

Three external bridges, all reachable from Python in `mcp_server/`:

1. **MAME MCP bridge** (`reference_mame_bridge.md`) — async client at
   `C:\Users\bigge\source\repos\ai\claude_mame\`. Drives a real MAME Model 2 session for
   hardware ground truth. Pattern per subsystem: capture the same input stream into both
   MAME and our HLE, diff the outputs. Per-opcode COP verifiers (`verify_*_mame.py`),
   capture scripts (`capture_*`), and lockstep harnesses live here.
2. **IDA Pro MCP bridge** (`reference_ida_bridge.md`) — system-python plugin at
   `localhost:7331`; tools `ida_search_name`, `ida_get_function_disassembly`,
   `ida_get_xrefs_to/from`, `ida_run_python`. When a disassembly line shows a named label,
   call `ida_search_name` to resolve its address — **never guess from context**
   (`feedback_ida_label_lookup.md`).
3. **SHARC firmware annotation** (`disassembly/`, `reference_sharc_label_verify.md`) —
   `verify_labels.py` computes the PM address per line (anchor `_L20080=0x20080`,
   1 statement = 1 word); handler entry = first `if flag0_in jump`. This is how the ang_x/y/z
   PM addresses and post-multiply formulas were recovered.

Lockstep methodology (used for both i960 and m68k): run MAME and ours in step, compare
register/memory state after each instruction, stop at the first divergence. It proved the
68K core correct and narrowed the sound bug to the SCSP timer HLE — exactly the kind of
result that is nearly impossible to reach by inspection alone.

---

## 7. Known-open work (carry-overs, from auto-memory)

These are live debugging threads at the time of writing — a rebuild should expect them:

- **Rocket Metal z-index** (`project_rocket_metal_zindex.md`): attract rocket drawn via
  `0x1B803737` slot 1 renders left+behind; should be centered+front. Root cause is in
  bone-cache content, not the renderer.
- **rob0/rob1 IK rotation bug** (`project_rob_ik_rotation_bug.md`): in-game fighter glitch
  is a genuine `0x35806B6B` IK rotation bug (pose-dependent, MAME-proven up to 0.8 err);
  T is correct, d-alignment diverges. NOT host-thread staleness.
- **adv_movie_egg_disp head window** (`project_egg_disp_head_window.md`): Eggman head inset
  (model 2895) renders pitched flat & spinning; it's set_pos/ang (bone=0), not IK.
- **geo3d ghosting fix** (`project_geo3d_ghosting_fix.md`): ring scan must be scoped to one
  game frame; translations lerped by the frame counter at 0x500020. Keep this in any rewrite.
- **STF cage + pole rendering** (`reference_stf_cage_pole.md`): animated cage net + poles,
  camera-culled via `cage_clip_m` (0x14802929 + focus_dist=280); stretched-net symptom on
  stage 1; i960 math verified correct, so the bug is downstream.

---

## 8. Things I would do differently on a true rebuild

- **Stand up the MAME + IDA bridges in Phase 1**, before the i960 core. In the original
  build they arrived late, after a lot of guess-and-check on COP semantics that the harness
  would have settled in an afternoon.
- **Treat PROPOSAL.md §8's COP rotation section as a cautionary tale.** The "Ry×Rx×Rz
  rebuild" looked right because it matched a clean ang sequence from identity. The moment
  bones accumulated multiple ang commands, it diverged. Build the column-major post-multiply
  model from the SHARC firmware from day one (§3.2).
- **Build the m68k lockstep harness alongside the m68k core, not after.** The SWAP/PEA decode
  bug silently corrupted the stack and would have been near-impossible to find by inspection.
- **Decide the tile palette format from the verified evidence** (`pal_bank=(entry>>7)&0xFF`,
  bit14 = palette) — not the earlier "bit14 = h_flip" reading, which was wrong for 2D tiles.
- **Keep the geo-capture ring scoped per game frame from the start** — the ghosting bug was a
  late discovery that a frame-scoped ring would have prevented.
- **Don't fake a single timer flag forever.** It gets you to attract (M1), but real audio
  (M3) needs the actual IRQ controller + board timers + 68K Int2_Timer delivery. Plan the
  real `irq_timer.h` early even if you stub it first.

---

## 9. Conventions (keep these)

- Header-only `.h` modules except `main.c`, `sokol_impl.*`, vendored `.c`. Don't split into
  `.c`/`.h` pairs.
- Default new code to the **board layer**; only `game_profile_t` quirks get game-specific code.
- Addresses/sizes are `uint32_t`; sign-extension handled per-instruction.
- Threading abstracted in `emu_thread.h` (`emu_lock()`/`emu_unlock()`).
- `log_msg(LOG_INFO|WARN|ERROR, fmt, ...)`. **Log unknown COP opcodes and unhandled MMIO at
  WARN** so new-game support surfaces automatically.
- **Never `rm -rf`** (`feedback_no_rm_rf.md`) — the user deletes folders manually.
- **Build with `build.ps1`** (`feedback_build_script.md`) — VS 2026 MSBuild; direct
  cmake/msbuild invocations fail on this machine.

---

## 10. External reference index

- STF IDA disassembly: `C:\m2\ida72\asm-check\stf_prog.asm` (~477K lines), `decomp/` (~1057 `.S`).
  ROM rebuild: `process-win.bat` (m2asm.py → gcc960 → objcopy → stfbin2rom.py).
  CRC32 `72E66A1D`, MD5 `2A3E32834FC727391C0AFCB18121245E`.
  `stfbin2rom.py --ctools` strips the 44-byte (0x2C) b.out header gcc960 prepends.
- STF 3D reference: `C:\m2\3d\new\stf-poly` (4405 OBJs, 5-digit zero-padded).
- SHARC firmware: `C:\temp\sharc_bone.asm`; annotation in `disassembly/`.
- MAME Model 2 driver: `src/mame/sega/model2.cpp` (memory map, COP opcodes, ROM regions —
  cross-check addresses, don't copy code).
- Prior STF-only project: `C:\Users\bigge\source\repos\stf-hle\` — working reference for
  register-window logic, COP math, polygon decoder, HLE patterns, memory-region table.
```
