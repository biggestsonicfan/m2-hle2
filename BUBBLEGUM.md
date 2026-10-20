# Bubblegum fixes: an audit (Pinboard #245, refreshed in #674, scanned again in #682 and #697)

This file lists the places where m2-hle2 patches a symptom and the board has a simpler rule underneath. A patch here means a list of model numbers or addresses, a fitted threshold, a hook that pokes RAM, or a per-game switch for something every board does.

The first audit was of master at `28d3563` (2026-09-30). The refresh was of master at `dab0dbf` (2026-10-12), after Sega Rally (Model 2A) came up and Daytona, Virtua Cop and House of the Dead were prepared. #678 fixed four items. #682 scanned the tree again: #678's branch with master `c279795` merged in (#663, #666 and #669 had landed in between). Sections 1 to 6 keep their `dab0dbf` line numbers unless a line says otherwise, and section 7 uses `c279795` + #678. Nothing in `src/` was changed by any of the audits.

Sections 1 to 4 list the open items, ranked by how many games they touch. Section 5 says what became of the first audit, and section 6 lists what was looked at and left alone on purpose. Section 7 holds what the #682 scan found that no earlier audit listed, and section 8 what the #697 scan found after #694.

---

## 1. Every game: the i960 and its interrupts

### 1.1 Call frames live on a host stack, and pfp is never kept

`call`, `callx` and `ret` push and pop `cpu->frame_stack[]` (`i960_exec.h:339-385`, `:1279-1297`, 64 deep, `constants.h:15`). No frame goes to memory, and `flushreg` (`:840`) does nothing. `pfp` is copied from the caller's `pfp`, so it stays 0. The comment says "pfp = old FP", but the code copies the old pfp (`:354-357`; `i960.h:88-92` admits this). Whether a frame is an interrupt's is a host flag (`frame_irq[]`), not pfp's return-status bits.

Three patches follow from this:

- `EMU_IRQ_TABLE_MAX_SLICES` (`emu_thread.h:494`, used at `:705`). The run loop cannot see an SDK kernel's task switch (`flushreg`, a new frame, `bx`), so it decides that a handler "in service" for 8 slices has switched away.
- "Has the handler returned?" is a test of frame depth (`emu_thread.h:510`), not of the PC's priority and interrupted bit.
- A call chain more than 64 deep halts the CPU.

**The board's rule** (MAME `do_call` / `do_ret`) has four parts:

1. pfp is the old FP with return status in bits 0-2.
2. The locals are spilled to memory at FP, and `flushreg` writes the cache out.
3. `ret` dispatches on `pfp & 7`, and 7 restores PC and AC from the interrupt record.
4. "In service" is then the PC's priority and nothing else.

This touches every game. Homebrew task switching is where it bites now, and FV and Sega Rally take table interrupts through it. Check it with `match-replay`, `grade-reset` and `det_digest`. If the instruction counts move, `NETPLAY_PROTO_REV` needs a bump.

### 1.2 STF's interrupts skip the processor's interrupt rules

A profile that names its handlers (`irq_handler[4]`, STF only, `sfight.h:856`) has them entered as a plain call by `hle_interrupt` (`hle_hooks.h:286-294`, `emu_thread.h:457-471`). The priority is not raised, the interrupt stack is not used, and the PC priority mask is not checked. A profile with `irq_vectors` (FV, Sega Rally, homebrew) goes through `hle_interrupt_on_stack`, which follows MAME `take_interrupt`. A profile with neither gets no interrupts at all, silently. m2snake relies on that (`m2snake.h:136-139`).

CLAUDE.md already says STF's own table names exactly the four profile handlers. **Real fix:** every program takes its interrupts from the PRCB table on the interrupt stack, and `irq_handler` stays only as the program-identity check (`profile_program_matches`). This needs §1.1 first, then a re-grade of STF. CLAUDE.md records that a gcc960 handler broke m2-pacman on the old path, so STF's handlers may have been written around it.

### 1.3 The i960's speed is one fitted factor, for one game

Sega Rally's profile sets `i960_board_timing` (`srallyc.h`; the model is `i960_timing.h`, switched on at `emu_thread.h` `emu_timers_slice_begin`), which charges every instruction MAME's table plus what the board's instruction cache, load scoreboard and bus waits add. Without it, the car-LOD row chosen from a timer read (0x5BADE0) comes out 8 instead of 5-7, and the third car ahead lags at GO (issue #305). The costs were measured on STF's Model 2B board and are used for a 2A game; RAM-resident code, the 2A's RAM at 0x200000 and the backup SRAM are assumed to cost what the 2B's ROM and work RAM do. While it is on, `m2_spin` and `m2_texload` stand aside.

The board's own costs have since been measured piece by piece (Pinboard #607-#670, `tools/stf_ai/board_bench.py` on PR #283):

- an i-cache line miss costs about 10 counts, and a jump to an uncached line about 21;
- a RAM store costs about 2.1;
- using a load's result at once costs about 3.4;
- a tile RAM store costs about 47;
- a COP FIFO write costs about 12.

A model built from these alone gives a median of 1.004 board/model over 189 STF frames. **Real fix:** charge those costs on every board, then decide grader by grader whether to hold to MAME's table. STF and FV stay on the table only because MAME, their oracle, does. STF's `rand` reads the timers, so the change moves the CPU opponent's choices and every MAME pairing. Expect a large re-grade, and a protocol bump.

### 1.4 Floating-point and divide edge cases

- `fdivr` by zero writes 0.0 "because it happens often with uninitialized geometry" (`i960_exec.h:1040-1052`). `divrl` beside it (`:1105`) divides as MAME does and gives an infinity.
- `fsqrtr` of a negative writes 0.0 (`:1068-1073`, still headed "stubs using math.h"). `sqrtrl` (`:1122`) gives the NaN through `i960_nan_result`.
- The integer divides (`divo`, `divi`, `remo`, `remi`, `modi`, `ediv`, `:697-745`) log an ERROR and leave the destination alone. The chip raises a zero-divide fault; check what MAME does before porting.
- Opcodes with no case run as a NOP after an `UNIMPL` WARN:
  - `calls`, `mark`/`fmark`, `atmod`/`atadd`;
  - the fault-if group (0x18-0x1F);
  - `classr`/`classrl`, `movre`, `cpyrsre`, `logeprl` and `remrl`.

  An unknown MEMB mode gives an address of 0 (`:205-207`). None of these is known to be reached; they are gaps, not patches.

The first two are cheap: make them follow the file's NaN rule (`:76-104`), then run `det_digest`, the netplay check and `match-replay`. `fdivr`'s comment says STF divides by zero often, so this may move frames.

## 2. Every game: board pieces wired up per profile

### 2.1 The sound interrupt waits for an STF RAM address

`emu_sound_pending` (`emu_thread.h:439-446`) returns false unless the profile names `sound_queue_count_addr`, and only STF does (0x504001). So FV, Sega Rally and homebrew never get the TxRDY interrupt (intreq bit 10) this way. The comment just above it gives the board's rule: TxRDY is a level, up whenever the UART can take a byte (MAME `sound_ready_w`, `txrdy && intena`). The RAM read is only there to feed the `queue_hi` statistic.

**Fix:** raise bit 10 from the UART state and `intena` alone, and read the counter only when a profile names it. STF's behaviour does not change. Check Sega Rally's and m2-pacman's sound after it.

### 2.2 Board reset, data window and sound board are set up in each installer

- **Reset from the PRCB.** Only `sfight_install` (`sfight.h:209-226`) starts the CPU as MAME's `device_reset` does: on the interrupt stack, at PC `0x001F2002`. `fvipers_install` (`fvipers.h:179-188`) and `srallyc_install` (`srallyc.h:153-157`) set only IP, so FP is 0.
- **The 0x06000000 data window.** All three installers copy `main_data + 0x1000000` into `xtra_data` (`sfight.h:191-202`, `fvipers.h:166-177`, `srallyc.h:142-151`). That is MAME's `model2_base_mem`, a board rule. sfight.h calls it "STF-specific / found empirically". FV's comment says "from offset 0 (not 0x01000000 as in STF)", but its code uses 0x01000000.
- **`enable_68k_sound`.** This is a profile switch for whether the board has a sound board (`game_profile.h:102`, off by default).
  - STF, m2snake, homebrew and Sega Rally set it.
  - FV does not, so Fighting Vipers runs silent on the same 2B board. (Fixed in #1025: `enable_68k_sound`.)
  - HotD sets it false, though its comment says sound.h should take it as it is.

  It should follow from the board variant and an audio CPU ROM being present. Daytona and Virtua Cop have Model 1 sound, which is another board.
- **The videoctl frame bit** (0x0100000C) is served only by the 2A map (`model2a.h:58-66`), whose comment says it is on every Model 2 board. On 2B it is plain storage, so a 2B program that polls it, as Sega Rally does at boot, would hang.

**Fix:** a board-level install that every profile calls. It does the PRCB reset and the data window, sets up the sound board from the ROM set, and serves videoctl on both maps. The three installers then keep only their own work.

### 2.3 The display list goes through the game's model catalogue

`geo3d_scan_geo_list` maps an object's address back to a model-table entry (`geo3d.h:1197-1209` as of `c279795`, `geo3d__geol_object` → `geo3d_lookup_by_pol`) and decodes through it (`:1440`). Every profile sets `mesh_ptr_subtract` / `mesh_ptr_add` to the same 0x02000010 / 0x10. That pair is the board's own rule written as constants: the mesh is at polygon ROM `(oba & 0x7FFFFF) * 4` (MAME `geo_object_data`).

Sega Rally has no table and already reads the mesh at the object's address (`game_render.h:2434-2445` as of `c279795`, taken whenever `model_idx < 0`). So one board has two draw paths, and only the table path has the mesh cache and the colour from table+4.

~~`MODEL_ENTRY_SIZE` is fixed at 16, but Daytona's entries are 20 bytes.~~ **Fixed on master by #663 (`f1a832e`).** `MODEL_ENTRY_SIZE` no longer exists (`git grep` finds it only here). `board/model_table.h:35-37` has three layouts: `MODEL_LAYOUT_1995 { .stride = 16, … }`, `MODEL_LAYOUT_VCOP { .stride = 16, … }` and `MODEL_LAYOUT_DAYTONA { .stride = 20, .uv = 4, .mat = 8, .mesh = 0 }`. `daytona.h:79` is `.model_layout = MODEL_LAYOUT_DAYTONA,` (`vcop.h:63` the same for Virtua Cop). Every table reader goes through `model_entry_read` (`model_table.h:80`; its callers are `geo3d.h:859`, `:978`, `:1209`, `:1440`, `:1837`, `:3343-3344` and `:3424`), which steps by `idx * t->layout.stride` (`:71`). So the 16-against-20 mismatch is gone. #666 (`37a8a3e`) added `model_mesh_resolve` (`model_table.h:113-123`), which refuses a polygon-RAM pointer instead of reading it as ROM.

**Fix:** game frames always read at oba. Keep the table for the object viewer, the explorer and labels; the per-profile stride is already there.

### 2.4 The GEO, as the board has it

- **0x802008 reads the GEO's live write position** on the real board: `set_end_mark` takes the list's length from it. This was found when a probe wedged the board (Pinboard #656). We answer the write start (`memory.h:599`), and so does MAME. Nothing breaks today, because STF only reads it right after setting it. A program that reads it mid-list would differ.
- **Z clip.** Three pieces are missing:
  - `zclip_3d` (0x0181C000) is mapped and saved but never read.
  - `master_z_clip` is not modelled.
  - The side planes are a per-window scissor plus the GPU's eye-plane clip.

  `check_culling`'s `max_z < 0` cull is in the direct-data path (`geo3d.h:2182`) but not in the object path. A game that sets its own z clip would draw what the board drops. The projection's near 0.05 and far 20000 (`game_render.h:2332-2354`) stand in for all of this.
- **The frame edge is a profile choice.** STF and FV mark it at `variable_diff_calc` through a hook (deliberate, for MAME pairing; see CLAUDE.md). `board_vblank` profiles mark it at the vblank. `geo_displaylist` (m2snake) publishes all of bufferram at the end of each slice (`emu_thread.h:786-797`, `:819-834`).
  - The board's own edge is the read-pointer write at 0x803008, which `memory.h` already handles for real lists.
  - `board_vblank` only means "no frame hook", so it could be worked out from the hook table.

## 3. The COP's FIFO

- **An unknown command is given 0 arguments** (`sharc_args_for_cmd`, `sharc_exec.h:179`). Its arguments are then read as commands, which has happened twice (`0x2A805555`, `0x17002E2E`). The command word's low byte is the firmware's dispatch index (0x..5151 is dispatch[0x51]). A table of argument counts by index, read from cpres1's PM, would cover every command, including ones not yet seen.
  - *Detail from the #682 scan, part of this item and not new work:* these firmware commands read arguments or reply and have no case in `sharc_exec.h` and reach the default (`:1817`; `sharc_args_for_cmd`, `:58`, default `:179`): `0x0C801919` `Fn_sqr_r`; `0x0D801B1B-0x10002020` `put_c`, `add_c`, `sub_c`, `mul_c`, `div_c`, `get_c`; `0x14002828` `Fn_tri_shin`; `0x15002A2A` `Fn_get_inner`; `0x23804747` `Fn_mul_matrix_rev`; `0x2E805D5D` `Fn_sub3` (its sibling `add3` is ported); `0x3B007676` `Fn_get_glo_ang_zyx`; `0x3C807979` `Fn_ziku_rot`; `0x3D007A7A` `Fn_mul_matrix3`; `0x3D807B7B` `Fn_scrn_clip`; `0x3E007C7C / 0x3E807D7D` `load_inner_3x3` / `store_inner_3x3`; `0x3F007E7E` `Fn_mul_matrix_inner3`. The dispatch table is `stf-sharc/cpres1.asm:245-334`.
- **Every command empties the reply FIFO** (`sharc_exec.h:679-680`, and `cop.h:175-176` for the stream command). Replies the i960 has not read are dropped, where the board would hand them to the next read. Since an empty read answers 0, this hides a handler that answers too many words. An under-read should be logged at WARN, as an underflow is, not dropped silently.
- The short-argument fallbacks that push zeros (`:985`, `:1218` and others) can only be reached from a truncated `cop_replay` input. They are harmless.

## 4. Smaller things

| What | Where | Fix |
|---|---|---|
| The link board's status bit 0 reads back 0 whatever was written | `memory.h:274-288` | Keep the written bit (MAME m2comm). Sega Rally writes 0 and reads it back. |
| `CPU_CTRL` (0xE00000), `UNKNOWN_VID`, `COPRO_STATUS` (0x980014) and `GEO_CTL1` are plain RAM | `memory.h:726`, `:756`; `constants.h:67-70` | Check each against MAME (`ctrl0_w` drives the input mux). Not known to matter. |
| The 68000 takes no exceptions. ILLEGAL halts. TRAP, TRAPV, line A/F, privilege violations and divide by zero only log. CHK, TAS, ABCD, NBCD and SBCD are stubs. | `m68k_exec.h:792-1554` | Vector through the exception table as MAME does. STF's driver hits none of them; another game's driver might. |
| FV's input map is STF's ("bit masks are likely identical"), and it has no coin mapping | `fvipers.h:236-240` | MAME `INPUT_PORTS_START(fvipers)`. |
| `GEO3D_PALETTE_OFF` 0x00100000, "STF-specific", is the palette fallback for every game | `geo3d.h:53` | Make it a profile value or take it from the list. |
| The transparent renderer's coverage threshold, 0.5 of four taps, comes from the explorer | `game_render.h:647` | Check against `model2rd.ipp`. |
| STF's frame counter 0x500020 is hard-coded in the run loop | `emu_thread.h:946` (`sndcap_frame`) | Debug capture only. Make it a profile field or the vblank count. |
| `warning_skip_addr` (STF 0x500410) is poked every slice | `emu_thread.h:236`, `:507` | Kept on purpose (#254): the graders pair frames by content, and a netplay session always skips. |

### The homebrew display-list walker

`geo3d_scan_displaylist` (`geo3d.h:1052-1078`, with its helpers from `:888`) walks m2-snake's own list format (`geo_displaylist`). The other homebrew, m2-pacman, has used the board's GEO walk since #466. This walker carries the most fitted code left in the renderer:

- the near-plane reject `GEO3D_NEAR_CULL` 4.0 (`:628`, `:643-648`), and a line version whose comment says −6 while the code uses 4.0 (`:690-696`);
- the 1.5-unit "degenerate close-quad" reject for HUD elements (`:649-661`), which hides a col0 decode bug;
- an object-level cull at `matrix[11] <= -1.0`, added against starfield streaks (`:999-1003`);
- the flat-colour ×2 boost, "approximating the colorxlat ramp" (`:614-628`, `game_render.h:649-650`);
- the overrides in `game_frame.h:147-165`: flat colour on, because "model 456's ROM material is a meaningless placeholder", and backface culling off.

All of it is gated on `g_geo_flat_color` / `geo_displaylist`, so no game sees it. (The one leak, a flag nothing cleared, is fixed: see "Fixed in #678".) **Real fix:** put m2-snake's list through the board's walk, as m2-pacman's is, and delete this walker. Failing that, use the clip planes of §2.4 here too.

## 5. What became of the 2026-09-30 audit

| Item | Now |
|---|---|
| The z-sort's patches over the half rule (recede bound, `zsort_standing`, keep-depth, coplanar layers, tie layer, same-matrix runs, held pairs) | **Done (#247, PR #159, `e8332c0`).** Game frames draw at the board's key (`geo3d_flat_depth`). `zsort_standing`, keep-depth, the tie layer, the runs and the held pairs are gone from `src/`. The round-intro grader (#249/#287) holds all fourteen stages against MAME. There the key is nearer MAME by 539,225 changed pixels to 3,496, and no clipping or bending is left. |
| Mode 0 at the head of a model falls back to the nearest corner | **Done** on the game path: `g_geo3d_flat_prev_z` carries across objects and is reset each frame, as `polygon_z` is. The old fallback is left only in the half rule. |
| Vblank and timers by wait hooks | **Done (#253, PR #163, `77e67ca`).** The vblank is on the cycle clock, the timers are live, and every wait hook is gone. |
| COP ready bit hooked per game (STF 0xF3C, FV 0x190C) | **Done (#252, `c59b997`).** `cop_fifo_status` serves 0x980004 for every game. |
| The COP-stream renderer (shadow floor, eye-bake, camera sign dials, `tgp_bone`) | **Done (#251, PR #162, `3d457aa`).** Direct data is walked and the fallback is deleted. |
| `SANITIZE` | **Gone (#252, `c59b997`).** |
| `0x28805151` pushes zeros | **Ported (#252, `c59b997`)**: `Fn_get_glo_ang` (`sharc_exec.h:1779-1789`). |
| `cop_read` on an empty FIFO | **Logs an underflow at WARN (#252, `cop.h:211-217`).** It still answers 0, since nothing models the stall. |
| `ldtime` returns 0 | **Returns the cycle count** (`i960_exec.h:906-909`). |
| `_700000_loop`, `warning_skip_addr`, FV model table, fov | **Settled (#254, `ee9f6c8`)**, as recorded in the first audit. |
| `m68k_exec.h` calling its cycle count "approximate" | **Mostly fixed**: `m68k_exec.h:661` says it is held to MAME's cycle-level 68000, but the field's own comment (`m68k.h:45`) still says "approximate". Comment only. |
| zclip / clip planes, the homebrew HUD reject | **Still open**: §2.4 and §4. |

### Left over from the half rule

These are not on the game path any more, but they are still in the tree:

- `g_geo3d_zsort_recede` 12.0, `GEO3D_ZSORT_KEY0`, `geo3d_sort_z` and the shader's half-rule substitution (`geo3d.h:323-368`, `:455-464`; `game_render.h:431-436`). These run only for `set_camera {"zflat":0}` and the object viewer.
- The coplanar layers (`geo3d.h:466-533`, `:2256-2707`), with their fitted 0.5 gap, 0.02 tie, cos 0.999, 1% overlap and spiral. Only the object viewer turns them on (`objview.h:324`). The game vertex shader still applies a layer offset, which is always 0 there (`game_render.h:437-439`).
- The `set_camera` keys `zsort`, `zrecede`, `zlayers`, `zlayer_steps` and `zkey0`, which do nothing to a game frame unless `zflat` is 0.

They are kept because the object viewer's free camera has no board sort to copy. There are two choices. One is to keep the `zflat` 0 A/B and leave them. The other is to drop that A/B and move the half rule wholly into `objview.h`.

### Timing lessons from the round-intro grader (still true)

- **A board frame is a vblank, and a game frame can take more than one** (since #163). An input schedule written in board frames drifts against MAME's, so drive inputs by the game's counter (0x500020).
- **The A/V stream tags a display list's picture one board frame early**, and MAME's counter runs one ahead of ours. Measure both; don't assume them.

## 6. Looked at and left alone

These were checked and are deliberate, measured or proven board-identical:

- STF's feature hooks: region, damage, rounds, enemy rank, VS mode, the PS3 cross-play group, the replay stage pin, the `versus_result` observer, the native replay hooks, SKY EYE and the COP-error halt.
- The Console profile's DLL traps and free play.
- The texture-loader row hooks and the idle-spin skip, both held by `det_digest`.
- `--sound-hle`'s fitted costs (`sound_hle.h:110-142`): it is an option for STF's driver only, and the 68000 stays the board.
- Host-side choices, not board ones: the GLES vertex-depth renderer list (`game_render.h:208-243`), the view cull's epsilons and the per-pixel depth option (#304).
- The 60 Hz vblank against MAME's 57.52 Hz (`irq_timer.h:58-63`), which is documented.
- The 315-5649 wheel speed, a feel constant for a digital pad on an analog wheel.
- The host camera's game-RAM reads (`camera_struct_addr`, `camera_angle_addr`), which feed only the free camera.
- The window offsets `84 + H-sync`, `130 + V-sync`, which are MAME's `model2_3d_project`.
- The decal "same diagonal" rule, which matches the explorer. Under the flat key it no longer affects depth.
- These files came out clean:
  - `tgp.h` (a generic MB86234 after MAME; a runaway program stops with a WARN), and `model2a.h` apart from §2.2;
  - `tile_renderer.h`, `scsp.h`, `sound.h` and `m68k_timing.h`;
  - `sharc_coli.h` and `sharc_zanzou.h`;
  - `savestate.h` and `rollback.h`.

## 7. Found by the #682 scan

This is a fresh scan of the board, core and renderer, looking for shared behaviour gated on one game, fitted values, and places we answer differently from MAME. It leaves out everything in sections 1 to 4. Line numbers are of `c279795` + #678, and every one was re-read against `idea-682-bubblegum-scan` before this went in. Each item gives the games it reaches and is tagged with what fixing it takes:

- **doc**: a comment or document only.
- **cheap**: no STF frame, timing or netplay change.
- **MAME**: needs a MAME check first, or moves STF.
- **netplay**: changes netplay behaviour (who joins which rooms), so it is not cheap even when no STF frame moves.

MAME references are to `claude_mame/mame/src/mame/sega/model2.cpp` unless another file is named.

### 7.1 The board variant is a label nothing reads

- **`game_profile_t.board` is never read** (`game_profile.h:247`). Every profile sets it (`sfight.h:877`, `fvipers.h:228`, `srallyc.h:171`, `daytona.h:69` and the rest), but no code tests `->board`. Three pieces of 2A hardware therefore exist only because Sega Rally's installer calls them:
  - The 2A map: `model2a_install` (`srallyc.h:140`, its only caller). It sets up the TGP ports, `TGP_ID`, the 16-bit texture RAM, `POLY_COUNT`, the UART at 0x01C80000 and the luma lane.
  - The 2B COP firmware pointer: `g_sharc_copro_rom = rs->copro_data` is set by hand in `sfight.h:182` and `fvipers.h:157`. A 2B profile that forgets it gets `sharc_sincos`'s host `sinf`/`cosf` fallback (`sharc.h:192-194`) in place of the ROM tables, silently. `hotd.h` (2C) does not set it today.
  - The 2A savestate parts (next item).

  Who it affects: the next 2A game (VF2, Virtua Cop 2, Dynamite Cop). It would run on the 2B map with the SHARC HLE and no TGP. The field stays unread. Making anything read it is the board install of §2.2, keyed on `board`, and §2.2 stays open: it moves Fighting Vipers, Sega Rally and homebrew, and needs a protocol bump (see the suggested order). It is not a cheap fix.
- **2A and CPU state are saved under Sega Rally's id only.** `srallyc.h:159`, `:162` and `:163` register `IO5649`, `TGP` and `I960FRAC` with `savestate_extra("srallyc", …)`. `savestate.h:401` and `rollback.h:114-115` write an extra part only when the active profile's id matches. `g_tgp` and `g_io5649` are board state, and `g_i960_cyc_frac` (`i960.h:117`) is CPU state; it stays 0 unless a profile sets the cycle factor, so STF never needs it saved. A second 2A game would lose all three on a save, a load, a rollback or a replay seek: the "loaded state drew no 3D" bug that the comment at `srallyc.h:160` describes. **cheap**: make them core parts, saved when the hardware is attached. That changes `LAYOUT`, so old Sega Rally states stop loading.

### 7.2 The 315-5649 I/O chip is Sega Rally's only

- `input.h:276` attaches the 315-5649 model (`io_5649.h`) only when `quirks.io_5649` is set, and only `srallyc.h:200` sets it. Every other profile gets `input_io_read_cb` (`input.h:244-254`), which serves IN0-IN2 and answers anything else from the plain buffer. MAME fits the same chip to 2B, with `eeprom_w` on port A and `in0_r` on port B (`model2.cpp:3206-3210`, the model2b config).
- Three behaviours go missing with it:
  - Port A's control bit switching IN0's top half to the EEPROM (`io_5649.h:170`).
  - The direction register.
  - The status register: MAME's `315_5649.cpp` answers 0x0C at register 0x0D. We answer 0xFF or the last write (`input.h:252`, `io_5649.h:181`).
- `io_5649.h:170` also reads the EEPROM's DO as 1 always, where MAME `in0_r` (`:1913-1919`) reads a real 93C46. Neither path has an EEPROM.
- Who it affects: every profile but Sega Rally (STF's `sfight` and `sfight_console`, `sfight_homebrew`, m2snake, FV, and the 2C/2.0 profiles), and Sega Rally's EEPROM. **MAME**: check whether STF or FV touch port A or the EEPROM. If they do, attaching the chip moves them.

### 7.3 Memory map against MAME

| What | Ours | MAME | Games | Tag |
|---|---|---|---|---|
| bufferram at reset | zeros (`memory.h:661`, the bus memset) | filled with `0x07800f0f`, an END word (`:300-303`) | every game. A list walk into unwritten bufferram stops at once in MAME, but runs through zeros here. | **MAME** (boot frames) |
| bufferram mirrors | only 0x900000-0x91FFFF (`memory.h:712`) | `.mirror(0x60000)`: also 0x920000, 0x940000, 0x960000 (`:1377`) | any game that uses a mirror. Ours reads 0 and drops writes. | fixed in #694 |
| 2B COP function port 0x880000-0x883FFF | the word goes in as written; reads pop a reply (`memory.h:253-259`) | `copro_function_port_w` puts `(offset>>2)&0xff` in bits 23-30 (`:703-709`), write-only. Our 2A path already does (`model2a.h:45`, `tgp.h:857`). | every 2B game | **MAME**: check what STF writes there. If it already writes whole command words, the encoding is the same and only the read differs. |
| texram0 alias at 0x10F00000 | aliased (`constants.h:146-154`, `memory.h:757`) | not mapped; the 2B texture maps start at 0x11000000 (`:1745`) | STF uploads through it. The comment says object 3545 went blank without it. | **MAME**: a fitted alias MAME does not have. Find out how MAME gets object 3545's tiles. |
| coprocessor ID 0x980030 | 2A only (`model2a.h:183-184`); on 2B it falls past `COPRO_CTL` (0x980000 + 0x24) and is unmapped | `tgpid_r` in `model2_base_mem` (`:1381`), every variant | any game that reads it; no profile we have is known to | fixed in #694 |
| polygon count 0x10400000 | unmapped | `polygon_count_r` (`model2_v.cpp:314`) | any game that reads it; MAME answers its live polygon count, which we do not keep | **MAME** (#694) |
| board timers | write and read masked to 20 bits (`irq_timer.h:156`, `:165`); `TIMERS_SIZE` 0x15 aliases 0x10-0x14 onto timer 0 | 32 bits with the write mask, 0xF00000-0xF0000F only (`:116-125`) | a game that arms a timer above 0xFFFFF | **MAME** (`rand` reads the timers) |
| sound UART control writes | dropped (`sound.h:693`); TxRDY/RxRDY ignore TxEN/RxE | i8251 honours mode and command | a driver that disables TX | **MAME** |
| 2A texture RAM | a read returns texel n (`model2a.h:125-130`); writes narrower than 32 bits are dropped (`:112`) | `.ram()` returns the raw dword; `tex0_w` (`:1312`, mapped at `:1610`) takes any width | 2A games | fixed in #694 |
| `MAIN_DATA` writable, GEO/GEO_PROGRAM reads return stored bytes | `memory.h:754`, `:601-603` | ROM; `geo_r` 0 (`:1141`), `geo_prg_r` 0xFFFFFFFF (`:1121`) | none known | fixed in #694 |

### 7.4 The GEO walk and the renderer

- **Every list starts from STF's camera.** `geo3d.h:1367-1373` resets the raster state for each list: identity matrix, focal 280, light (0,0,1), window 496×384. Only the mode, LOD and z-adjust carry over (`g_geol_held`). MAME's `render_frame_start` (`model2_v.cpp:741-763`) resets only the polygon list, `polygon_z` and `cur_window`, so focus, light, matrices, window and raster centre stay set from earlier lists. Who it affects: any game that sets these once, or not on every frame. **MAME**: STF sets them in every list, so a frame diff should show no change; confirm with `grade-zsort` before trusting that.
- **More than 512 objects a frame are dropped without a word.** `MAX_GEO_MODELS` 512 (`geo3d.h:36`) ends the walk (`:1379`), and direct data past `GEO3D_DIRECT_WORDS` is dropped (`:1239`). MAME counts no objects; its only limit is 32,768 polygons (`model2.h:820`). Who it affects: busy scenes in racing games and Daytona. **cheap**: log a WARN when the cap is hit, then size it.
- **The tile pens skip colorxlat until a game fills it.** `tile_pen_lut` (`tile_renderer.h:482-496`) shows the raw palette colour (`c5 << 3`) while colorxlat's luma-0x40 row is all zero. MAME always goes through colorxlat and gamma, which gives black then. Who it affects: every game's boot frames, and any program that never loads colorxlat (homebrew). **MAME**: it moves STF's pictures before colorxlat loads, and only those.
- **The fill shader shades faces that have no luma base.** `game_render.h:650` and `:792` (GL and HLSL) are `if (al > 0.0) rgb = clamp(color.rgb * al * 2.0, 0.0, 1.0);`, an invented ×2 for textured faces, and `:665` / `:805` are `rgb = color.rgb * clamp(lbpl.y, 0.0, 1.0);` for untextured ones. A face takes this path when its luma base is −1. That happens on the m2snake walker (`g_geo_flat_color`, `geo3d.h:2092`) and when the desktop's "MAME luma ramp" box is cleared (`geo3d_window.h:147`, `game_render.h:2104`, `:2220`); no game's own frame does. A face with no texture LOD (the object viewer) picks its mip from screen derivatives (`:631`, `:775`). **cheap**, alongside the m2snake walker below; STF's frames do not reach either.
- `geo3d.h:1721-1726` gives a zero distance texlod 0, as the patched oracle does (`model2_v.cpp:471-480`, the M2-X11 note). Stock MAME would pick the coarsest level there. Who it affects: any game's faces at zero distance. **doc**: the code comment says so, but no document does.

### 7.5 The COP

- **The bone table's index is masked.** `pm_bone[(int)args[n] & 0xF]` (`sharc_exec.h:1630` and `:1669` with `args[0]`, `:1662` with `args[6]`) wraps past 16, where the firmware addresses PM `0x21F20 + 12n` directly. Slot indices of 16 or more are ignored silently (`:1137`, `:1151`, `:1540`). Who it affects: every 2B game on the SHARC HLE. **cheap** (a WARN); STF stays below 16.
- **Accumulation order not yet held to the firmware.** These add in one expression, where `Fn_trans` adds one term at a time:
  - `0x1C803939` (`:1172-1174`);
  - `0x3A007474`'s trans (`:1658`);
  - `0x35006A6A`'s inverse transform (`:1065-1067`) and `0x31006262`'s rotation from angles (`:1608-1610`).

  Who it affects: every 2B game on the SHARC HLE. `match-replay` holds the attract fight bit-identical, so any of them that it reaches already agree. **MAME** (`cop_replay` on a capture that reaches them).
- `Fn_initialize` (`:1494`) resets only `stack_top`. The firmware also resets the current-slot pointer (DM 0x3033F = 0x305A0). Who it affects: no game known; the HLE has no slot pointer to reset. **doc**.
- Checked and right: every handler's sqrt, atan2, asin and division goes through `sharc_fw_*`. `0x35806B6B` replies 0 on both of the firmware's exits.

### 7.6 Shared code fitted to m2snake

These add to "The homebrew display-list walker" in §4. Every `geo_displaylist` program sees them, and no commercial game does. They are **cheap**, and they go when that walker goes:

- `game_frame.h:85` walks from a fixed 0x10000 ("every m2-snake homebrew uses read_start 0x10000") and ignores `g_geodl_snap_rstart`, which the board walk at `:91` uses.
- `game_frame.h:161-164` (`g_geo_wireframe  = 0;` / `g_backface_cull  = 0;`) forces `g_geo_wireframe = 0; g_backface_cull = 0;`, overwriting the player's settings for the rest of the session.
- `game_frame.h:168` (`float dl_lerp = q->geo_displaylist ? 1.0f : lerp_t;`) turns off the in-between-frame lerp.
- `GEO3D_NEAR_CULL`, the 1.5-unit reject and the ×2 boost (`geo3d.h:623-661`, `:696`) sit in the shared emit helpers. They are gated on `g_geo_flat_color`, not on m2snake.

### 7.7 Looked at in #682 and found right

- These are STF-only features, and gated as such: the profile-id checks in `sky_eye.h:106-107`/`:272`, `gems.h:297`/`:531`, `main_libretro.c:2118` and `main_web.c:1077`. `netplay.h:2280` is not; see §7.8.
- These profile fields are genuinely per game: `vs_rematch`, `enemy_ranks`, `attract_replay`, `match_info`, `native_*`, `in_play`, `model_table_*` / `model_layout`, `any_program`, `preliminary`, `parent_zip_name`.
- These match MAME: the 0x400000 sound bank register (`sound.h:536-541`, `model2snd_ctrl`), the IRQ pin grouping, timer expiry and the idle value, `fifo_control_r`, the tile and char mirrors, the 2A luma lane, the TGP FIFO and upload paths.
- These match `model2rd.ipp`: specular, luminance, the backface cull, the window slice order, and the texel → luma → colorxlat → gamma chain in the fill shader.

### 7.8 PS3 cross-play matches the id prefix

- `netplay.h:2280` is `bool stf = strncmp(g_active_profile->id, "sfight", 6) == 0;`, so `sfight_homebrew` (m2-pacman and any other program on STF's set) counts as STF. On the official server (`netplay_server_is_official`, `:2281`) it takes `RPCN_PS3_COM_ID` and joins the PS3 port's rooms, where the boards cannot agree. Who it affects: homebrew on STF's set (`sfight_homebrew`: m2-pacman and the like) online on the official server; STF's `sfight` and `sfight_console` match either way. **netplay**: an exact compare, as `gems.h:531` does, would stop `sfight_homebrew` counting as STF, so such a player would no longer be put in the PS3 rooms. That is a change in netplay behaviour, not a cheap fix.

## 8. Found by the #697 scan

The #697 scan was of `idea-694-cheap-fixes` at `bc29b61`, and the line numbers here are that commit's. It skipped everything sections 1 to 7 already list. It looked for two kinds of code: code that only works for one game, and shared code built around one game. Each finding below was checked against the source and, where a tag says so, against MAME (`model2.cpp`, `model2_v.cpp`, `model2rd.ipp`) or the firmware (`stf-sharc/cpres1.asm`). The tags are those of section 7. Nothing in `src/` was changed.

### 8.1 Match results come only from STF's hook

- **The result of a match comes from one place.** `g_versus_result` (`hle_hooks.h:28`) is set only by STF's observer at 0xDC3C (`sfight.h:285`). §6 keeps that hook, and it is right for STF. What §6 does not say is that the board-level code relies on the result too:
  - the run loop hands it to the replay and to lockstep (`emu_thread.h:942-945`);
  - a room of three or more rotates its queue only on a result (`netplay_room_rolls`, `netplay.h:3431`);
  - the go-again prompt, a VS rematch and GGPO's result count wait for it (`emu_ggpo.h:183`);
  - a replay ends `REPLAY_TAIL_FRAMES` after it (`replay.h:833-840`).
- **Who it affects:** FV, Sega Rally and homebrew. Each gets its own hashed com id (`comid_resolve`, `com_id.h:304`), so it can host and join rooms. It never sees a result, though:
  - a room of three never rotates;
  - the go-again prompt never comes up;
  - a replay ends only when the session does.
- **A replay of another game is always dropped.** Native recording is the default (`replay.h:40-43`). The recorder (`replay.h:668-669`) drops a recording that has no native round, and only STF's profiles fill those (`native_*`, `match_info` at `sfight.h:884` and `sfight_console.h:373`). So FV, Sega Rally and homebrew online matches never leave a replay, unless the player picks the snapshot format.
- **GGPO turns on VS mode without asking the profile.** `emu_ggpo.h:362` sets `g_vs_mode = 1` for every P2P session. The RPCN path checks `quirks.vs_rematch` first (`netplay_room_vs_mode`, `netplay.h:965`). `g_vs_mode` is read only by STF's hook at 0xE584 today, so no other game moves. A game that gets its own rematch hook would get VS mode in GGPO whatever its profile says.
- **Tag: netplay.** The fix is a per-profile result hook, the way `match_info` already works, plus a snapshot fallback for games without native rounds. That changes what rooms and replays do for those games.

### 8.2 The GEO walk against MAME's

These are board-level and affect every game drawn by the board's walk: STF, FV, Sega Rally and m2-pacman.

| What | Ours | MAME | Games | Tag |
|---|---|---|---|---|
| GEO command 0x0E (`geo_test`) | `geo3d__geol_skip` has no case for it (`geo3d.h:1313-1321`) and returns 0. `geodl_apply_state` also has none (`memory.h:533-541`), so its default advances a single word. | 32 FIFO-test words, a block count, then three words per block (`model2_v.cpp:2264-2300`, dispatched at `:2497`). | Any game whose display list carries the self-test. A frame carrying it reaches no END and draws no 3D, and the state pass reads the test words as commands. None we run is known to send it at frame time. | **fixed in #703** (below) |
| An object's link count (`obc`) | Only `oba`, argument 2, is read (`geo3d.h:1203`). The mesh ends at the first all-zero attribute word (`:1534`, `:1583`), capped at `GEO3D_IA_MAX_VPS` 4096 (`:46`). | `geo_parse_np_*` loops `obc` links (`model2_v.cpp:1222`), and an `obc` of 0 means 0xFFFFF (`:1939-1941`, for VOn and Gunblade). | Every game. An object whose `obc` stops short of the zero word is drawn whole here. One whose mesh has no zero word runs on into the next. | **MAME**: STF's models end on the zero word at J = 1.0 (`grade-models`), so check a game that relies on `obc` |
| Microtexture | `geo3d_texhdr_decode` (`geo3d.h:1610-1630`) ignores th0 bits 10-12 and th2 bits 13-15. | `model2_v.cpp:672-675` takes them, and `model2rd.ipp:126-130`/`:324-328` blend the microtexture in by distance. | The racing games (Sega Rally's road, Daytona, Indy 500). STF's headers carry 0 there as far as the explorer shows. | **MAME** |
| Texture ROM addressing | `geo3d_tex_word` (`geo3d.h:1145-1149`) fails a read past the end of the ROM. The direct-data path then drops the polygon (`:2437`). | The address is masked with `texture_rom_mask` and always reads (`model2_v.cpp:429`, `:450`). | Any game whose texture points address past its ROM, which mirrors on the board. | **MAME** |
| A model with no texture header | Its colour is a hash of the material pointer (`material_ptr_to_color`, `geo3d.h:174-176`), used at `:1216`. A model from polygon RAM is a flat 0.7 grey (`:1218`). | The face takes its colour from the header and the colour tables like any other. | Any game that draws such a model: a debug colour reaches the game frame. | **MAME**: check whether any game frame draws one |
| A model with `tpa` or `tha` = 0 | Taken as "this model has no stream" (`geo3d.h:1966-1972`: `have_uv = uv_word != 0`, and the same for the header). | Address 0 is an ordinary address. | Any game that puts a stream at address 0 of texture ROM. Unlikely, since the table starts there. | doc |
| The walk's guard | `guard < 0x8000u` (`geo3d.h:1388`): a list of more than 32768 commands, jumps included, stops without END and draws no 3D. | No cap. | A game with a very long list, or a jump loop. MAME would hang on the loop. | doc |

### 8.3 The COP

- **`Fn_kage_mat` divides on the host** (`sharc_exec.h:482`, `float inv = 1.0f / s_;`). The comment says "recips + Newton-Raphson", and the firmware does that inline: `recips`, then three refinement steps (cpres1 PM 0x20D1F, `cpres1.asm:4633-4642`). CLAUDE.md says the COP's arithmetic must go through `sharc_fw_*` and never through host maths. The firmware's sequence is not the `_L205D0` call that `sharc_fw_div` follows, so it needs its own helper.
  - **Who it affects:** every game that draws kage shadows (STF, FV). The kage matrices go into the display list, so the change moves only low bits of the shadows. CLAUDE.md records `cop_replay` as exact for this command on a select capture.
  - **Tag: MAME.** Run `cop_replay` before and after, to see whether any word moves.
- **The motion spline returns `v0` when `span <= 0`** (`sharc_exec.h:897-913`). The firmware has no such test (`Fn_fcurve_spl`, `cpres1.asm:5708` on). It divides by span/30 through `_L205D0` whatever the span. **Tag: MAME.** It matters only if a game sends a zero or negative span. STF's motion arguments are exact in `grade-motion`, so STF does not.

### 8.4 Loading and profile choice

- **A ROM directory has no TGP tables.** `romset_load_dir` and `romset_save_dir` (`rom_loader.h:261-302`) know seven files, and `tgp_tables` (`:238`) is not among them. Only `srallyc_load` fills that region (`srallyc.h:112-114`), and only from a zip. So `--export-roms` of Sega Rally writes a directory that boots with no TGP tables: `tgp.h:106` reads 0 for every table entry. **Who it affects:** every Model 2A set loaded from a directory, which today means Sega Rally. **Fixed in #703** (below).
- **The desktop keeps the current profile for a set nobody knows.** `select_profile_for_zip` (`main.c:184-199`) returns true when `profile_for_rom_set` finds nothing and the set is not a prepared one. The zip then loads under the profile already active: `sfight_console` at startup. `main_sdl.c:328-333` and `main_libretro.c:477-482` refuse such a set with `profile_missing_message`. **Who it affects:** any Model 2 set without a profile, opened in the desktop window or with `--rom`; it runs under STF's hooks. **Fixed in #703** (below).
- **Homebrew is recognised only on STF's set.** `profile_program_matches` (`game_profile.h:386-399`) returns true for any profile that names no `irq_handler`, and only `sfight.h:856` names any. It also assumes the handlers sit at vectors 12-15 (`table + 36 + (4 + pin) * 4`). A homebrew program on FV's set therefore runs FV's HLE hooks inside its own code. That is the trap `sfight_homebrew` exists to avoid. **Who it affects:** homebrew on FV's set. Sega Rally plants no hooks (`srallyc.h:174`, `hook_count = 0`), so homebrew on its set already runs hook-free; FV plants one, `fvipers_hook_frame_pace` at 0x11C80 (`fvipers.h:203-208`), which marks a display-list frame edge and makes the match_replay jump if the program's code passes there. **Not cheap**, looked at in #703 (below): the fix this item first suggested, naming FV's handlers in `irq_handler`, would move FV itself onto STF's plain-call interrupt path (§1.2).

### 8.5 Input

- **The service switch is on the wrong bit for STF, FV and m2snake.** `sfight.h:831`, `fvipers.h:257` and `m2snake.h:124` map `GAME_INPUT_SERVICE` to IN0 bit 0x04. In MAME, 0x04 is the test switch (`PORT_SERVICE_NO_TOGGLE`) and SERVICE1 is 0x08 (`model2.cpp:1960-1961`). `srallyc.h:182-183` follows MAME. `sfight_homebrew.h:284` takes STF's map. So "service" opens the test menu in those games, and no input can give a service credit. (FV fixed in #1025, with its coins and test switch.)
  - **Who it affects:** STF (both profiles), FV, m2snake and STF homebrew.
  - **Tag: netplay.** A frame does not move, but the bit is part of netplay's canonical input word, so a mixed-build session would read each other's service button differently. A bot or script that sends "service" to reach the test menu would also need the new name.

### 8.6 The prepared sets (Daytona, Virtua Cop, HOTD)

These boards do not run yet (`preliminary`), so the items are notes for whoever brings them up.

- `daytona.h:24-51` and `vcop.h:20-36` list no `ROM_TGP_TABLES`, though MAME's `MODEL2_CPU_BOARD` (`model2.cpp:3487-3490`) is part of every model2o set.
- `RAM2_SIZE` 0x40000 (`constants.h:44-45`) is 2A/2B's. On model2o, 0x200000-0x21FFFF is RAM and 0x220000-0x23FFFF is the program ROM's second half (`model2.cpp:1620-1621`).
- MAME delays an interrupt-enable write by 80 ns, "vcop2 needs this" (`irq_enable_w`, `model2.cpp:1254-1261`). `irqt_enable_write` (`irq_timer.h:76-79`) applies it at once. **MAME**: this would move STF's interrupt timing, so it is not cheap, and it is §1.2 territory.

## Doc drift found on the way

- Found in #697:
  - The header of `geo3d.h` (`:4-7`) describes the COP capture-stream scanner and a decoder in `game_render.h`, both gone (#251, #339).
  - `io_5649.h:4` names Daytona as a 315-5649 board. Daytona is model2o, with the Model 1 I/O (`model2.cpp:8004`, `:1622`).
  - `hotd.h:10-15` says sound.h "should take it as it is". The 2C's UART sits at 0x01C80000, and `sound.h:656-660` knows that address only through the region name `UART_2A`.
  - `m68k_exec.h:1243-1245` says a Bcc with displacement 0xFF is "treat as 16-bit for 68000 safety", but the code fetches 32 bits. On a 68000 it is an address error (§4, exceptions).
- `model2a.h:20-23` says the 2A I/O is served "when the profile sets quirks.io_5649" (§7.2), and lists the sound UART as still to do. That move is done at `:199-215`. Found in #682.

- `fvipers.h:217-221` still lists `_700000_loop` as a hook to port. It was removed in #254.
- `fvipers.h:166-168`: the data-window comment contradicts its code (§2.2).
- `m68k.h:45` calls the 68000's cycle count "approximate" (§5).
- `m2snake.h:18-21` describes the no-frame-hook 60 Hz pacing from before #253.
- `game_profile.h:146-151` says `geo_displaylist` is "required for non-STF games". Sega Rally and m2-pacman use the board's GEO walk.
- The scanner section's header comment (`geo3d.h:888-894`) is stale, and so is the line reject's "-6" (`:690`).

## Fixed in #678 (branch `idea-678-cheap-fixes`)

Checked with `det_digest --profile sfight` against master `c279795`. A 9000-frame scripted game is identical. 15000 frames of attract differ only in buffer RAM on frames 2508-3258 (the 0x68 commit), and work RAM, COP DM, cycles and the netplay check are identical there too.

- **`0x34006868` `Fn_ld_glb_mat`** is ported from cpres1 PM 0x205A3 (`sharc_exec.h:1233`, commit `315f47d`). It loads the 12 words at bufferram `0x01400000 + arg0` into the current matrix: col0, col1, col2, then T.
  - In STF it changes only the Tornado's shadow in the attract intro, which `name_char_kage_disp` draws under the kage slot 0x3D00. That shadow is now the plane's whole silhouette on the water, as MAME (`-nodrc`) draws it, where it was scattered smudges.
  - The i960 cannot tell the difference, and netplay is unchanged.
  - Any other game that sends 0x68 gets the firmware's behaviour.
- **`0x13002626` no longer clamps asin's argument** (`sharc_exec.h:1471`, commit `832ed32`), as `_L20332` does not. Only an argument with |a| > 1 sees a change, and STF never sends one: removing it changes nothing in either `det_digest` run.
- **`g_geo_flat_color` is set from each frame's own list** in `game_frame_prepare` (`game_frame.h:77`, commit `c50e8e3`). Before this, nothing cleared it. Only a process that runs m2-snake and then another game can see a difference: that game no longer keeps the flat-colour fill.
- **CLAUDE.md:301 now says FV has no `co_processor_error_hang` hook yet** (`fvipers.h:217-220`; commit `0ac619b`). This is a doc change only.

The sound pin (§2.1) and the board install (§2.2) were looked at and are not cheap. Dropping the STF-only gate on the sound pin would start that interrupt in FV, Sega Rally and homebrew, and FV has no sound board turned on yet. Both stay open.

## Fixed in #694 (branch `idea-694-cheap-fixes`)

The cheap items of §7, one commit each. Checked with `det_digest` on this branch against `idea-682-bubblegum-scan` (`f146fd8`): sfight attract (15000 frames), the 9000-frame scripted game, `sfight_console`, `fvipers` and `srallyc` (6000 frames each) are byte-identical, sound hashes included, and none of them logs a new WARN. `mem_test` and `cop_test` pass.

- **Bufferram mirrors** (§7.3). `mem__add_base_extras` (`memory.h:274`) adds `BUFF_RAM_M1`-`M3` at 0x920000, 0x940000 and 0x960000 over the same buffer, as MAME's `.mirror(0x60000)` (`:1377`). A savestate saves buffers by name, so `LAYOUT` and the state are unchanged. Games: any that uses a mirror; STF, FV and Sega Rally log no access there.
- **The coprocessor ID at 0x980030 on every board** (§7.3). `tgp_id_read_cb` (`memory.h:265`, region at `:278`, `constants.h:67-68`) answers MAME's `tgpid_r` (`:1381`) bytes, and the 2A copy in `model2a.h` is gone. Games: every 2B and 2C game that reads it (none we have is known to); Sega Rally reads the same bytes as before.
- **GEO and GEO_PROGRAM reads** (§7.3). `geo_read_cb` (`memory.h:617`) answers 0 past its registers, as `geo_r` (`:1141`); `geo_program_read_cb` (`:626`, wired at `:732`) answers all ones, as `geo_prg_r` (`:1121`). Games: any that reads them back; none we run does.
- **The data ROM is read-only** (§7.3). `MAIN_DATA` and `XTRA_DATA` (`memory.h:780-781`) drop CPU writes with the program ROM's throttled WARN, as MAME's `rom()` (`:1403`, `:1406`). Profiles still fill the buffers at install, and the bridge's `write_memory` with `"rom":1` still patches them. Games: any that writes there by mistake; none we run does.
- **2A texture RAM** (§7.3). `model2a_tex_write_cb` (`model2a.h:105`) takes any width, keeping the low 16 bits of the dword as the lanes hold it, as `tex0_w` (`:1312`) ignores the mask. `model2a_tex_read_cb` (`model2a.h:123`) reads the raw bytes, as MAME's `.ram()` (`:1609-1610`), and 0 past the half the writes fill. Games: Model 2A only, which today is Sega Rally.
- **A WARN at the GEO caps** (§7.4). `geo3d.h:1244` (direct data past `GEO3D_DIRECT_WORDS`) and `:1400` (more than `MAX_GEO_MODELS` objects) each log once. The second is worse than §7.4 says: a list that hits the cap never reaches END, so `game_frame.h` drops the capture and the whole frame draws no 3D, not just the objects past 512. Sizing the cap is still open. Games: none we run hits either.
- **A WARN on an out-of-range bone slot** (§7.5). `sharc_unit_slot_ok` (`sharc_exec.h:226`) and `sharc_inner_slot` (`:235`) replace the silent checks in 0x35, 0x36, 0x37 and `compose_unit`, and the `& 0xF` in 0x45, 0x46, 0x74's `args[6]` and the inner load/store. Each logs once and then does what the code did before. Games: every 2B game on the SHARC HLE; STF stays below 16.

Left, with the reason:

- **The polygon count at 0x10400000** (§7.3) needs a MAME check. `polygon_count_r` (`model2_v.cpp:314`) answers the live `poly_list_index`, and only Sky Target reads it. We keep no live count, so mapping it means inventing a value.
- **The 2A savestate parts** (§7.1). As core parts they change `LAYOUT`, so every older STF savestate and rewind state stops loading, not only Sega Rally's. Keeping `LAYOUT` means saving them when the 2A hardware is attached, which is the board install of §2.2. Only Sega Rally installs 2A today.
- **`game_profile_t.board`** (§7.1) stays unread. Making anything read it is the board install of §2.2, which stays open: it moves Fighting Vipers, Sega Rally and homebrew, and needs a protocol bump.
- **The bufferram fill** (§7.3): still open, **MAME**, not implemented.
- **The 2B COP function-port encoding** (§7.3): still open, **MAME**, not implemented.
- **The texram0 alias at 0x10F00000** (§7.3): still open, **MAME**, not implemented.
- **The 20-bit board timer mask** (§7.3): still open, **MAME**, not implemented. STF's `rand` reads the timers, so a change would also touch netplay.
- **The 315-5649 I/O chip on 2B** (§7.2): still open, **MAME**, not implemented.
- **The per-list camera, light and window reset** (§7.4): still open, **MAME**, not implemented.
- **The fill shader's no-luma ×2** (§7.4) **and the m2snake items** (§7.6) go with the m2snake walker, as §7.6 says.
- `dl_record` still records only 0x900000-0x91FFFF, so a game that writes its list through a mirror is not captured. A dev-tool gap only.

## Fixed in #703 (branch `idea-703-bubblegum-cheap`)

The cheap items of §8.2 and §8.4, one commit each. Checked with `det_digest --cpu --sound` on this branch against `idea-694-cheap-fixes` (`bc29b61`, the same code as `idea-697-bubblegum-scan`): `sfight` attract, `sfight_console`, `fvipers` and `srallyc`, 3000 frames each, are byte-identical. Every ctest passes, the four that load a ROM included.

- **GEO command 0x0E, `geo_test`** (§8.2). `geo3d__geol_skip` (`geo3d.h:1322`) and `geodl_apply_state` (`memory.h:539`) step over 33 words plus three a block, as MAME's `geo_test` (`model2_v.cpp:2264-2300`). Before, the state pass ran the test words as commands, and the draw walk stopped there with no END. `mem_test` (`tests/mem_test.c:79-96`) holds a list whose test words read as a polygon write and an END; it fails both checks on the old walk. Games: any that sends the self-test in a frame's list; STF, FV and Sega Rally do not (their digests do not move).
- **A ROM directory carries the TGP tables** (§8.4). `k_romset_files` and `romset_region` (`rom_loader.h:261-274`) know an eighth file, `tgp_tables.bin`, written only for a set that has the region. `rom_recipe.h:29` asserts that the region enum and the file list agree, and `rom_recipe_region` (`:61`) now goes through `romset_region` for all of them. Games: Model 2A sets loaded from a directory, which today means Sega Rally. An exported `srallyc` directory runs byte-identical to the zip over 1500 frames; the old build split from the zip at frame 1397 on that directory.
- **The desktop refuses a set no profile runs** (§8.4). `select_profile_for_zip` (`main.c:184-205`) logs `profile_missing_message` and loads nothing, as `main_sdl.c:328-333` and `main_libretro.c:477-482` do. `--rom` and File > Load ROMs both go through it. Games: any Model 2 set without a profile, which used to run under `sfight_console`'s hooks. A known set still loads: `sfight`, `fvipers` and `srallyc` checked headless. The web build picks its profile by `?profile=`, not by name, and is unchanged.

Left, with the reason:

- **Homebrew detection on FV's set** (§8.4). Sega Rally needs nothing: it has no hooks. A separate `any_program` profile for it would only lose its `savestate_extra` parts, keyed by the `srallyc` id. FV needs two things. One is a fingerprint of FV's own program that does not change how interrupts reach it, e.g. a new quirk holding FV's table words 13-16, which `profile_program_matches` (`game_profile.h:386-399`) and `profile_for_program` (`:410-424`, which only trusts a profile with `irq_handler`) would read. Naming FV's handlers in `irq_handler`, as this item first suggested, sends FV down `hle_interrupt` (§1.2) and moves every FV frame. The other is a new `fvipers_homebrew` profile beside `sfight_homebrew`. That is a new field and a new profile, not a cheap fix, and there is no FV-set homebrew to test it against. All it guards against is one frame-edge mark at 0x11C80.

## Suggested order

1. **The cheap fixes that move no STF frame.** The asin clamp and the empty 0x68 handler are done (see above). The sound pin (§2.1) and the board install (§2.2) are left, and are not cheap: they move FV, Sega Rally and homebrew, and need a protocol bump. For each, run `cop_replay`, `match-replay` and `det_digest`.
2. **The COP argument table by dispatch index, and keeping unread replies (§3).** Both are ROM-free and testable with `cop_test` / `cop_replay`.
   - These #682 items move no STF frame and are done in #694 (above): the bufferram mirrors, 0x980030, the GEO reads, the data ROM and the 2A texture RAM (§7.3), the GEO cap WARN (§7.4) and the bone slot WARN (§7.5). The 2A savestate parts (§7.1) wait for §2.2, and 0x10400000 (§7.3) for a MAME check.
   - These need MAME first: the 315-5649 on 2B (§7.2), the bufferram fill, the 2B function port and the 0x10F00000 alias (§7.3), and the per-list GEO reset (§7.4).
   - The #697 items: 0x0E in the GEO walk (§8.2), the ROM directory's TGP tables and refusing an unknown set on the desktop (§8.4) are done in #703 (above). Homebrew detection on FV's set (§8.4) needs a fingerprint field and a profile; Sega Rally needs nothing. Beyond those, the result hook for other games (§8.1) and the service bit (§8.5) need the netplay decision, and obc, microtexture, texture ROM masking and the two COP items (§8.2, §8.3) need MAME or `cop_replay` first.
3. **The model catalogue out of the game draw (§2.3).** Run `grade-models`, `grade-stages` and `grade-zsort`. This also unblocks Daytona's 20-byte table.
4. **Real call frames, then table interrupts for STF (§1.1, §1.2).** These remove `EMU_IRQ_TABLE_MAX_SLICES` and the STF interrupt path. Re-grade everything and bump the protocol.
5. **Board i960 costs for every game (§1.3).** This has the biggest payoff: Sega Rally loses its factor, and STF's `rand` matches the board. It also has the biggest re-grade, so do it last, once the cost model is final.
