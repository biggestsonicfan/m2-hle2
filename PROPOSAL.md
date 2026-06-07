# Model 2 HLE Emulator — Rebuild Proposal

A bootstrap outline for recreating the project as a **general Sega Model 2 arcade emulator** in a fresh Claude Code session. *Sonic The Fighters* (STF) is the **reference game** — it's where the bulk of the prior reverse-engineering happened and the first title that must boot end-to-end — but the architecture must accommodate the full Model 2 catalogue from day one. Generalising across games strengthens every subsystem: a fix for Virtua Cop's tile compositor may fix STF's HUD; a Daytona polygon-format finding already drove STF's 3D decoder; a COP matrix bug found in Virtua Fighter 2 would surface in Fighting Vipers and STF identically.

---

## 1. Project Goal

Build a cross-platform (Win/Linux/macOS/WASM) Sega Model 2 emulator that:

- Boots arbitrary Model 2 / 2A / 2B / 2C ROM sets, identified by a **game-profile registry**
- Executes Intel i960 code accurately enough to drive game logic (shared across the entire catalogue)
- Replaces unknown/expensive hardware (COP, geometry engine, DSP sound, I/O boards) with **per-board HLE hooks** that fall back to a generic shim where a board-level behaviour is known but a per-game hook is not yet registered
- Renders the Model 2 wireframe/textured 3D pipeline and 2D tile layers
- Provides a live ImGui-based debugging UI (CPU regs, memory, trace, breakpoints, 3D viewer, tile/video viewer, log)

**First milestone:** STF boots to attract, 3D models render at J=1.0 against the reference OBJ set, tile compositor matches reference screenshots.
**Second milestone:** A second title (Daytona USA or Virtua Fighter 2) boots far enough to validate the i960 core and COP independent of STF-specific HLE bypasses.

---

## 2. Tech Stack

| Concern | Choice |
|---|---|
| Language | C11 (single-TU `demo.c` + header-only modules) |
| UI | Dear ImGui via **dear_bindings** (cimgui flat drop, imgui 1.92.6) |
| Graphics / windowing | **Sokol** (sokol_app, sokol_gfx, sokol_imgui, sokol_glue) |
| File dialogs | **ImGuiFileDialog** |
| ROM zip extraction | **miniz** |
| Build | CMake ≥ 3.20 |
| Threads | Win32 `CRITICAL_SECTION` / POSIX `pthread_mutex_t` |
| Web target | Emscripten + WebGL2 |

---

## 3. Model 2 Hardware — What's Shared, What Varies

The architecture has to clearly separate **board-level** code (shared by every Model 2 game) from **game-level** code (per-ROMset profiles).

| Layer | Shared across catalogue | Varies per game/board |
|-------|-------------------------|-----------------------|
| i960 KB/KA CPU core | ✅ Fully shared | Clock rate is per-board (Model 2 = 25 MHz; 2A/2B/2C may differ) |
| Memory map | ✅ Region layout is board-wide | Some MMIO ports are game-specific |
| Geometry / COP | ✅ Math semantics shared | Command opcodes are mostly shared; some game-specific encodings exist |
| Tile renderer (2D) | ✅ Format shared | Per-game palette and layer-priority quirks |
| 3D polygon ROM format | ✅ Encoding shared | Per-game model table layout, animation systems |
| Sound (SCSP / MultiPCM / DSB) | ✅ Hardware blocks shared | Per-board sound-CPU code and per-game sample sets |
| Input | ✅ I/O port layout shared | Per-game button maps, light-gun vs joystick vs steering |
| HLE hooks | ❌ All per-game ROM addresses | Each ROMset has its own hook table |

**Implication for the codebase:** a single `model2_board_t` struct (CPU + memory + COP + GPU + sound + I/O) is instantiated once at startup. A `game_profile_t` lookup keyed by ROM CRC32 supplies the per-game hook table, input map, and any quirks (palette-bank overrides, polygon-decoder masks, etc.). Adding a new game = adding a `game_profile_t` entry plus per-game hooks. No core code changes.

### Board variants (rough)

- **Model 2** — original (Daytona USA, Virtua Fighter 2)
- **Model 2A-CRX** — cost-reduced (Virtua Cop, Virtua Fighter 2.1)
- **Model 2B-CRX** — improved geometry, texture filtering (Virtua Cop 2, Fighting Vipers, **Sonic The Fighters**, Last Bronx)
- **Model 2C-CRX** — final revision (Top Skater, Dynamite Cop, Sega Rally)

STF is **2B-CRX**. Plan the geometry/texture path for 2B first, but keep the engine layer factor-out clean enough that 2A and 2C variants can swap in.

---

## 4. Architectural Conventions

- All modules except `demo.c` and `miniz.c` are **header-only `.h` files** (single-TU build).
- Two threads share emulator state under `emu_mutex`:
  - **Main/UI thread** — Sokol render at 60 Hz
  - **Emu thread** — CPU at board-specified clock (~25 MHz on Model 2), publishes a `cpu_snapshot` copy for the UI
- Memory is dispatched through a `MemRegion` table (~48 named regions on Model 2, MMIO via R/W callbacks).
- HLE hooks intercept by ROM address before the i960 decoder runs (256-slot table per active game profile).
- The active `game_profile_t` is resolved at ROM-load time by matching loaded file CRC32s against a registry.

---

## 5. Implementation Checklist

### Phase 0 — Project skeleton ✅
- [x] `git init`, `.gitignore`, `LICENSE`, `README.md`
- [x] Vendor third-party sources: `sokol/`, `ImGuiFileDialog/`, `miniz/` as submodules; `cimgui/` as dear_bindings flat drop (imgui 1.92.6) — replaced original cimgui/cimgui submodule due to ABI mismatch causing ImGui to be unresponsive to mouse input
- [ ] `update_deps.sh` to refresh upstream deps
- [x] `CMakeLists.txt`: static libs (`cimgui`, `ImGuiFileDialog`, `sokol`, `miniz`); per-platform link flags; Emscripten branch stubbed
- [ ] `CMakeSettings.json` for VS integration
- [x] Verify hello-world builds Release on Windows (`main.c`, not `demo.c` — entry point is `sokol_main`)

### Phase 1 — Core infrastructure ✅
- [x] `src/board/constants.h` — board memory base addresses, region sizes, PRCB offsets, CC codes (`CC_NO=0` for `chkbit`), `EMU_STEPS_PER_SLICE=500000`
- [x] `src/core/log.h` — ring-buffer logger, on-disk session log, `g_log.warn_triggered` / `g_log.break_on_warn`; log display surfaced via ImGui console in the main window (no separate `log_window.h`)
- [x] `src/core/game_profile.h` — `game_profile_t` struct: board variant, `load_fn` / `install_fn` function pointers, `hooks[256]` + `hook_count`, `input_map`, `quirks`; profile resolved by name (CRC32 registry deferred to Phase 14)

### Phase 2 — Memory bus (board-level) ✅
- [x] `src/board/memory.h` — `memory_bus_t` with 31 named regions, `mem_read8/16/32` / `mem_write8/16/32` dispatch, MMIO callbacks, `mem_init` idempotent (frees heap before reset), NULL data guard on reads; IO region initialised to `0xFF`
- [x] `src/ui/memview.h` — 16×16 hex/ASCII inspector, byte/u32/f32 edit, region-label display

### Phase 3 — i960 CPU (board-level, fully shared) ✅
- [x] `src/board/i960.h` — `i960_cpu_t`: 16 global + 16 local regs (union with named aliases `pfp`/`sp`/`rip`), SFRs, FP regs, register-window frame stack (`FRAME_STACK_DEPTH`), `halted` flag, AC
- [x] `src/board/i960_exec.h` — MEM/REG/CTRL/COBR decode; `call`/`ret` register-window push/pop; ALU/branch/load-store; `chkbit→CC_NO` fix; `reg_quad` big-endian; MEM mode 0x5 = `IP+8+disp`; FP-from-GPR as `memcpy` reinterpret
- [x] `src/ui/cpu_window.h` — collapsing SFR/global/local/frame-stack sections, changed-cell yellow highlight
- [x] `src/ui/breakpoint_window.h` + `src/core/breakpoint.h` — add/remove/toggle UI, 64-slot table, last-hit in red
- [x] `src/ui/trace_window.h` — stub no-op `trace_record` (real trace deferred)

### Phase 4 — ROM loading & game-profile resolution ✅
- [x] `src/core/rom_loader.h` — miniz zip extraction, CRC32 validation, `interleave_32_word`, `load_16_word_swap`, `rom_region_copy`, `romset_free`; MAME clone child/parent zip fallback
- [x] `src/profiles/sfight.h` — STF load (all 7 ROM regions, CRC32-verified, hacked-ROM override path) + install (bus init, XTRA_DATA mirror, PRCB→IP bootstrap); `src/profiles/registry.h` defines `g_profiles[]`/`g_profile_count`/`g_active_profile`
- [ ] Second game profile (Daytona or VF2) — deferred to Phase 14

### Phase 5 — Emu thread (board-level) ✅
- [x] `src/core/emu_thread.h` — Win32 `CRITICAL_SECTION` / POSIX `pthread_mutex_t`; `EMU_STOPPED`/`RUNNING`/`STEPPING` states; `EMU_STEPS_PER_SLICE=500000` run loop; double-buffered `cpu_snapshot`/`cpu_prev_snapshot`; mutex released before sleep; `g_frame_done` frame-pacing; high-res clock (`QueryPerformanceCounter` / `CLOCK_MONOTONIC`); `steps_per_second` stat

### Phase 6 — HLE hook system ✅
- [x] `src/core/hle_hooks.h` — profile-driven dispatch (`g_active_profile->hooks[]`); `hle_ret()` helper that properly restores the i960 register window (mirrors `ret` instruction — no frame-depth leak); `g_frame_done` volatile flag
- [x] STF boot-critical hooks in `src/profiles/sfight.h` (8 hooks): `CoProcessorErr` (0x74E4) bypass, `check_timer_4` skip, `check_timer_4_spin` timer-flag write, `interrupt_wait`/`interrupt_wait_b`/`_idle` RAM_BASE nudge, `_700000_loop` delay skip, `variable_diff_calc` (0x11A04) frame-pace flag

### Phase 7 — Geometry engine / COP (board-level, shared math) ✅
- [x] `src/board/cop.h` — `cop_state_t` (command accumulator + 32-slot reply FIFO + pos/scale/ang/world_pos + lazy matrix + 32768-slot geo-capture ring + diagnostic snapshot); `cop_build_matrix` with left-handed Y rotation (`+sy` bottom-left) and `Ry × Rx × Rz` order; 20+ command dispatch covering STF opcodes including `0x04800909` world-pos snapshot, `0x07800F0F` bone transform reads `world_pos[]`, `0x2F005E5E` scalar-then-vector, `0x14802929` vec3×matrix
- [x] MMIO bridge: COPROGRAM region read/write callbacks in `src/board/memory.h` route writes into `cop_write` (which captures into the geo ring) and reads drain the reply FIFO via `cop_read`

### Phase 8 — 2D tile renderer + game render path ✅
- [x] `src/board/tile_renderer.h` — composite BG/FG layers from tilemap + palette into BGR555; 16-bit byteswap on pixel bytes (XOR low bit of byte index); tilemap-entry pal_bank encoding `(pal_bank << 8) | (entry & 0xFF)`; FG color-0 transparency; H/V flip + scroll wrap; `composite_layers` → RGBA8
- [x] `src/ui/video_window.h` — CPU pixel buffer + sokol `sg_image`/`sg_view`/`sg_sampler` ownership; `video_update` composites tiles and uploads to GPU. **No ImGui window** — the original "video window with view modes" idea was dropped in favour of drawing the game directly into the swapchain, matching the stf-hle reference architecture
- [x] `src/ui/game_render.h` — GLSL + HLSL textured-quad pipeline (`game_render_init/shutdown`), `game_render_letterbox` aspect-ratio math, `game_render_draw_game(view, ox, oy, w, h)` draws the tile composite as a letterboxed fullscreen quad inside the swapchain pass **before** `simgui_render` so ImGui debug windows overlay on top. Will be extended with the line pipeline in Phase 9.

### Phase 9 — 3D pipeline (board-level decoder, per-game model tables) ✅
- [x] `src/board/geo3d.h` — `vec3_t`, `apply_matrix`, `read_float_le`/`read_u32_le`/`u32_as_float`/`is_sane_float`, `captured_model_t`, `geo3d_state_t`, model-table → pol-pointer lookup, `material_ptr_to_color`, COP capture-stream scanner (handles `0x02000404` matrix, `0x03000606` position, `0x3C007878` object marker), J=1.0 index-array polygon decoder (iFlag 0..3 dispatch, A-B-D-C quad winding, `(x, y, -z)` vertex convention), `geo3d_build_wireframes` per-frame builder, `geo3d_log_captures` debug dump. Algorithm confirmed J=1.0 on **STF 4402/4405** AND **Daytona 2377** — board-level, not STF-specific.
- [x] Game-specific knobs moved into `game_quirks_t`: `model_table_offset`, `model_table_count`, `mesh_ptr_subtract`, `mesh_ptr_add` (STF: 0xE0004 / 4373 / 0x02000010 / 0x10). `poly_connect_mask` retained for the legacy "simple" rule but unused by the J=1.0 path.
- [x] `src/ui/game_render.h` extended with line pipeline: GLSL+HLSL shaders, streamed `sg_buffer`, `gm_mat4_perspective/view/mul/transpose`, `game_render_draw_lines(cam, rot, fov)` drawn into the same letterboxed viewport as the tile quad, after the tile draw and before `simgui_render`
- [x] `src/ui/geo3d_window.h` — enabled / use-captures / use-matrix / test-triangle toggles, capture-filter range, single-model index, camera xyz + rot_x/y + fov sliders, live capture/line counts

### Phase 10 — Sound (stub) ✅
- [x] `src/board/sound.h` — MMIO read/write callbacks attached to the MIDI region; `sound_state_t` accumulates read/write counters; `g_sound.log_writes` toggle (wired into the Debug menu) emits per-write LOG_INFO when on. No synthesis yet — real SCSP/MultiPCM lands after a second game boots and we have cross-validation.

### Phase 11 — Input ✅
- [x] `src/core/game_profile.h` — replaced the I/O-port-style `game_input_bit_t input_map[]` with a richer `game_input_map_t input` that pins the held/momentary RAM addresses, per-player credit-byte addresses, and a `bits[GAME_INPUT_COUNT]` mask table. The bit→RAM-address translation is therefore per-profile; the keyboard→action mapping is board-level.
- [x] `src/board/input.h` — `input_state_t` (pending + held bitmasks), `input_keycode_to_action()` mapping sokol keycodes to abstract actions (arrows + Z/X/C/V/1/5 for P1; I/J/K/L + Del/End/PgDn/Home/2/6 for P2; F2 Service / F3 Test), `input_key_down/up`, `input_coin(bus, player)`, `input_flush(bus)` writes held into RAM and ORs momentary
- [x] `src/profiles/sfight.h` — populated `.input` with `held_addr=0x500700`, `momentary_addr=0x500704`, credit addrs `0x59C388/0x59C38C`, and STF's 17 bit assignments from the original input.h
- [x] STF-specific `read_sw` hook (0x000017CC) — clears `0x500700` at the start of the per-frame input-read so the host UI's `input_flush` is the only authoritative source of held bits
- [x] `src/main.c` — `event()` routes KEY_DOWN/UP through `input_key_down/up` (gated only on key_repeat, not ImGui focus); `frame()` calls `input_flush` once per UI tick after the snapshot pull

### Phase 12 — Top-level UI (partially complete)
- [x] `src/main.c` — Sokol entry (`sokol_main`), ImGui setup, window orchestration, frame loop, full menu bar (File / Profile / Emulation / Debug)
- [ ] `debug_window.h` — aggregate debug controls / panel layout (current debug panels are individual windows wired from the menu bar)

### Phase 13 — Cross-platform polish
- [ ] Linux (`X11 Xi Xcursor GL dl m` + pthreads), macOS (`-x objective-c`, QuartzCore/Cocoa/Metal), Emscripten (WebGL2, no filesystem)
- [ ] Clang dead-strip linker flag

### Phase 14 — Second-game shakeout
- [ ] Boot Daytona USA or VF2 far enough to observe i960 + COP independent of STF HLE
- [ ] Triage divergences: pure i960 bugs (fix in core), pure COP bugs (fix in cop.h), genuinely game-specific (move to profile quirks)

---

## 6. External References

- **STF disassembly** — IDA-generated `stf_prog.asm` (~477K lines) at `C:\m2\ida72\asm-check\`. `decomp/` holds ~1057 per-function `.S` files. Primary reference for STF ROM addresses, struct layouts, and function names.
- **STF ROM rebuild toolchain** — `process-win.bat`: `m2asm.py` → `gcc960` → `objcopy` → `stfbin2rom.py` → `epr-19001.15` / `epr-19002.16`. CRC32 `72E66A1D`, MD5 `2A3E32834FC727391C0AFCB18121245E`.
- **STF 3D reference geometry** — `C:\m2\3d\new\stf-poly`, 4405 OBJs, 5-digit zero-padded filenames.
- **Daytona 3D reference** — index-array decoder originally validated against 2377 Daytona models at J=1.0. If a Daytona reference OBJ set is also on disk, use it as the second cross-check for the polygon decoder.
- **MAME Model 2 driver** — `src/mame/sega/model2.cpp` is a useful (LLE-oriented) cross-reference for board memory map, COP command opcodes, and ROM region layouts. Don't copy code; do cross-check addresses.

---

## 7. Validation Strategy

No automated tests — all validation is interactive via the GUI:

- [ ] **STF boot path**: passes hardware init, reaches attract mode, tiles match reference screenshots, 3D models 4402 & 4405 render at J=1.0
- [ ] **Cross-game smoke test**: at least one non-STF Model 2 ROM set loads, i960 reaches the title's first idle/wait loop without obvious decoder faults
- [ ] **Subsystem windows** confirm sensible state: register highlighting under call/ret, memory inspector shows expected struct layouts, trace shows balanced call/return with register-window pushes
- [ ] Polygon-decoder Jaccard score is computed offline against the reference OBJ set on demand (don't bake into the run-loop)

---

## 8. Subsystem-Specific Gotchas (mined from current source)

These are non-obvious facts a textbook reading of the i960 manual or "generic emulator design" will NOT give you. Most are **board-level** (apply to every Model 2 game); a few are STF-specific and noted as such. Each one cost trial-and-error to find.

### i960 CPU core — applies to every Model 2 game

- **`cmpobX` / `cmpibX` always update CC** — even when the branch is not taken. Downstream `bg`/`bl`/`be` read those CCs. Skipping the CC update silently breaks branches several instructions later.
- **`chkbit` sets `CC_NO` (=0b000) when the tested bit is 0**, not `CC_NE`. `bno` depends on this.
- **REG-format literal mode**: the m1/m2/m3 bits flag *literal vs register*; when literal, the 5-bit "register index" field IS the immediate (0–31). It is not a mask.
- **FP regs from GPRs are bit-reinterpreted, not converted** — when an FP instruction sources from a GPR, treat the 32 bits as an IEEE-754 binary32 pattern (memcpy semantics), do NOT do `(float)int_value`.
- **`call` / `ret` frame layout**: align SP to 64 bytes (`(sp + 63) & ~63`), zero the new locals, save pfp/sp/rip, sync `g15` (frame pointer) every call. `ret` restores all locals. Skipping local-zero leaves garbage in uninitialized variables.
- **MEM mode 0x5 is IP-relative with `IP + 8 + disp`** — the `+8` is relative to the *start* of the 2-word MEM instruction, not the next.
- **Register-pair (`reg_quad`) ops are big-endian** even though the CPU is little-endian overall.

### Memory bus & MMIO — board-level

- **Region table is linear-scanned in order; overlapping regions resolve by table order.** TILE (`0x01000000`) must appear BEFORE H_SYNC (`0x01040000`) or H_SYNC reads route to the TILE handler.
- **IO region is initialized to `0xFF`, not `0x00`** — hardware idle state.
- **`GEO_CAPTURE_SIZE` must be ≥ 32768.** It used to be 4096 and wrapped mid-frame, producing partial 3D snapshots and visible flicker.

### Coprocessor (COP) / geometry engine — board-level math, all games

- **Y-rotation is LEFT-HANDED**: `[ c 0 s ; 0 1 0 ; +s 0 c ]` (note `+s` in the bottom-left). Rotation order is `Ry × Rx × Rz`.
- **Angles are 16-bit signed fixed-point**, `0x10000 = 360°`; only the low 16 bits are meaningful (high bits are sign-extension).
- **World-position snapshot semantics**: command `0x04800909` (set angle) snapshots current `pos[]` into `world_pos[]`. Command `0x07800F0F` (bone transform) reads `world_pos[]`, NOT current `pos[]`. Required for animated bones; if position is overwritten between angle-set and bone-transform, bones still anchor to the snapshot.
- **Command `0x2F005E5E` is scalar-then-vector**: arg0 = scalar, args 1–3 = vector → returns `(s*x, s*y, s*z)`.
- *Note:* command opcodes documented here are the ones confirmed in STF. Other games may use additional opcodes — log unknown commands at WARN and add to the dispatch table as they appear.

### HLE hooks — game-specific addresses, board-level patterns

- **`CoProcessorErr` (STF: 0x74E4) bypass is mandatory** — boot self-test compares COP buffers; with HLE COP the test always fails. Return by jumping to `locals.rip` (saved return address from the call frame). Every Model 2 game ships some form of COP self-test — the pattern repeats, the address won't.
- **Timer interrupt is faked** by writing `0x01` to the polled flag address (STF: `0x50008C`). Same pattern across games; the address is per-ROMset.
- **Frame pacing is hook-driven**: STF's `variable_diff_calc` (~0x11A04) sets a volatile `g_frame_done`. Other games will have their own frame-boundary function — find it via the same "main loop ends here" pattern.

### 3D pipeline — board-level decoder, per-game model tables

- **Vertex Z is negated on read**: `v.z = -read_float_le(vp + offs)` for both vertices of a pair. Forgetting this flips models front-to-back and reverses face winding.
- **Model-table mesh pointer is encoded, not a raw offset**: actual ROM offset = `(ptr * 4) - 0x02000010 + 0x10` (STF; verify against other games — the multiply-by-4 is hardware, the base may shift).
- **Polygon connectivity is in `vp[25] & 0x03`**: 0 = sentinel, 1 = carry far edge (strip continue), 2 = new quad, 3 = new strip. Simple rule `(vp[25] & 3) != 2` gives ~0.708 Jaccard; the empirically correct `connect_when` mask is **`0x45B4`**, found by brute force against the STF reference OBJ set. The decoder algorithm itself was reverse-engineered from **Daytona first** (J=1.0 on 2377 models) then ported to STF (J=1.0 on 4402/4405) — strong evidence the algorithm is board-level, not game-specific.
- **Reference data**: STF reference OBJs are at `C:\m2\3d\new\stf-poly` (4405 files). Use models 4402 and 4405 as the first integration test.

### 2D tile renderer — board-level

- **Tile pixel bytes are 16-bit byte-swapped**: byte indices `[0,1,2,3]` are read as `[1,0,3,2]` (XOR low bit of byte index). Within each swapped word, *high* nibble = left pixel, *low* nibble = right pixel.
- **Tilemap entry layout**: full tile index = `(pal_bank << 8) | (entry & 0xFF)`; palette LUT index = `pal_bank * 16 + color_idx` (16 colors per bank).
- **Color index 0 is transparent on foreground layers only.** Background layers are fully opaque — pass `NULL` for `alpha_out`.

### Threading

- **Double-buffered CPU snapshot** (`cpu_snapshot` + `cpu_prev_snapshot`): UI always reads the current snapshot. The swap is not atomic but structured so the UI doesn't read mid-update.
- **Always unlock the mutex BEFORE `Sleep()` / `usleep()`** in the emu thread run loop. Reversing this freezes the UI.
- **Sleep granularity differs by OS**: Windows `Sleep()` ≈ 1 ms; POSIX `usleep()` ≈ 1 µs.

---

## 9. Why "Model 2 first, STF as reference"?

Earlier iterations of this project were scoped narrowly to STF. That worked, but it made every fix feel game-specific even when the underlying bug was board-level. Concrete examples:

- The **chkbit→CC_NO** fix was discovered via an STF symptom (Espio's tongue), but it's an i960 instruction-decode bug — it affects every Model 2 game.
- The **index-array polygon decoder** was reverse-engineered from **Daytona** and then ported to STF. Doing it in this order proved the algorithm was board-level. A narrowly STF-scoped project would have likely produced a worse decoder.
- The **left-handed Y-rotation** in COP is hardware behaviour, not a game choice. Confirming it against a second game would have closed the question sooner.

Designing for the catalogue from the start makes every reverse-engineering finding more valuable, and it gives the project natural validation oracles (different games stress different subsystems).
