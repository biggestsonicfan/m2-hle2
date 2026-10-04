# CLAUDE.md

Seeds a fresh Claude session with the hard-won facts that took trial-and-error to discover — things that cannot be re-derived from the i960 manual or general emulator-design knowledge.

[PROPOSAL.md](docs/PROPOSAL.md) is the original plan: its §3 has the board-vs-game rationale, and its §8 is an older subset of this file. This file is for the *invariants that must never be re-derived*; the proposal is historical context.

---

## Project Overview

A general **Sega Model 2 arcade emulator**, written in C11 with Dear ImGui (via cimgui) and Sokol for cross-platform graphics. The first target is *Sonic The Fighters* (STF), because the bulk of the prior reverse-engineering work happened there — but the architecture is built for the **full Model 2 catalogue** from day one. Generalising across games strengthens every subsystem: most "STF bugs" turn out to be board-level i960 / COP / tile bugs that affect every Model 2 game equally.

**Reference prior project** — the previous full implementation lives at `c:\Users\bigge\source\repos\ai\m2-hle\` (same `src/{board,core,ui,profiles}` layering, corrected COP math, 68K + SCSP, MCP harness). Treat it as a working reference for register-window logic, COP math, the polygon decoder, HLE hook patterns, and the memory-region table. The older STF-only `c:\Users\bigge\source\repos\stf-hle\` still exists but is badly outdated — consult it only when asked. Read freely from m2-hle; do not import code wholesale — the new project's layering (board vs game profile, §3 below) means files will need restructuring as they're brought over.

See [PROPOSAL.md](docs/PROPOSAL.md) for the architecture, module map, build commands, and bootstrap checklist.

---

## Board vs. Game Layering (critical)

Code lives in one of two layers. Get this distinction wrong and you'll re-implement board-level fixes in per-game files.

- **Board layer** — anything shared by every Model 2 ROM set: i960 CPU core, memory bus, COP math, tile renderer, 3D polygon decoder, sound block, threading. Lives in shared `.h` modules.
- **Game-profile layer** — anything specific to one ROM set: HLE hook table (addresses), input map, ROM file list + CRC32s, optional quirks struct. Lives in a `game_profile_t` entry resolved from the ROM set's name (the zip basename, `profile_for_rom_set`); the CRC32s validate each file.

**Default to the board layer.** If a bug surfaces in STF, your first hypothesis should be "this is board-level and another game is also affected" — not "this is STF-specific." Only move a fix to the per-game layer when you have positive evidence (e.g. another game's ROM relies on the opposite behaviour).

---

## Implementation Invariants (do not re-derive)

These are facts reverse-engineered or debugged into the original implementation. Not in any datasheet. Treat as load-bearing.

### i960 CPU (board-level — every Model 2 game)

- **`chkbit` sets `CC_NO` (0x0) when the tested bit is 0**, not `CC_NE` (0x5). `bno` depends on this; getting it wrong inverts all `chkbit`+`bno` / `chkbit`+`bo` branch logic.
  - *Symptom that surfaced this in STF:* Espio's tongue drew every frame in the Rocket Metal attract cutscene because `bno rd_te_pass` never branched. Since this is i960-core behaviour, the same bug would surface in any Model 2 game that uses `chkbit`+`bno` — which is most of them.
- **`cmpobX` / `cmpibX` always update the condition code**, even when the branch is not taken. Downstream `bg` / `bl` / `be` read those CCs.
- **MEM mode 0x5 is IP-relative**: `effective = IP + 8 + disp`, where `+8` is relative to the *start* of the 2-word MEM instruction, not the next.
- **FP-from-GPR is bit-reinterpret, not int→float convert** — `memcpy` semantics.
- **`call` / `ret` frame layout**: the new frame is `(sp + 63) & ~63`, and that is `g15` (FP); SP starts 64 above it (MAME `do_call`). Zero the new locals, save `pfp` / `sp` / `rip`. `ret` restores all locals and the caller's `g15` (`frame_fp[]`).
  - *Symptom that surfaced this (m2-pacman issue #1):* `g15` used to be set to `pfp`, which this model leaves at 0, and SP to the frame base. STF's compiler never addresses through FP, so nothing showed. gcc960 code keeps its locals at `fp+N`: the homebrew's `main` counted its loops in ROM and never left its boot loop.
- **The processor starts on the PRCB's interrupt stack, at PC `0x001F2002`** (priority 31, interrupted state, supervisor; MAME `device_reset`): FP = PRCB+0x18, SP = FP+64 (`sfight_install`). An IAC re-initialise moves neither. m2-sdk's kernel enters `main` with `b` and never leaves the interrupted state, so its interrupts nest on the stack it is on.
- **`modpc src, mask, src/dst`: the mask is src2, the new bits come from src/dst, which then gets the old PC** (MAME `i960.cpp`). It used to take the mask from src1 and the bits from src2, so the boot code's `modpc r4, r4, r5` (drop the priority to 0) raised it to 31.
- **An interrupt pin's vector is byte `pin` of the interrupt control register** (the program's `synmov` to `0xFF000004`, which lands in the IAC block), **and its handler is word 9 + (vector − 8) of the PRCB's interrupt table** (`hle_irq_vector_handler`). STF's table names exactly its profile's `irq_handler` (vectors 12–15: `0xC40`, `0xD10`, `0xD30`, `0xDF0`). A profile with `irq_vectors` takes a pin it names no handler for this way, on the interrupt stack with MAME's padding frame and the vector's priority (`hle_interrupt_on_stack`). STF's handlers still run on the interrupted SP (`hle_interrupt`), as they always have; a gcc960 handler that stores `g0`–`g15` from its SP broke m2-pacman's picture that way.
- **Register-pair (`reg_quad`) ops are big-endian** even though the CPU is little-endian overall.
- **The four "not" logicals invert different operands**: `andnot` = src2 & ~src1, `notand` = src1 & ~src2, `ornot` = src2 | ~src1, `notor` = src1 | ~src2 (MAME `i960.cpp`). `notand` used to be a copy of `andnot`.
  - *Symptom that surfaced this in STF:* the top quarter of both texture sheets was never written. `sub_4C444` clears bit 0 of each mip destination with `notand g6, 1, g6`, so every mip level went to the wrong address. Once fixed, `tools/grade-texram.mjs` shows both sheets byte-identical to the explorer.
- **`movl` / `movt` / `movq` honour the literal flag: a literal source fills every destination register** (MAME `i960.cpp`). They used to read registers regardless, so `movq 0, r4` copied pfp/sp/rip/r3 into r4–r7.
  - *Symptom that surfaced this in STF:* a few thousand frames into a fight the game stopped itself on its "max poly / err poly" screen. `adv_set_action` "clears" each fighter's damage and crush-stage arrays with `movq 0, r4` + `stq`, so the stages held a return address (0xBD80). `damage_unit` indexed the empty crush table with it, ran the Fighting Vipers armour-break effect, and `efc_crush_parts_set` wrote model numbers like 0x3000 over the forearms and shins; `set_obj` rejected them.
- **An interrupt is not a call: `ret` from a handler restores AC and PC.** Deliver interrupts with `hle_interrupt`, never `hle_call`. Otherwise a handler's compares leak into the condition code of the instruction it interrupted.
  - *Symptom that surfaced this in STF:* attract crashed about 45 s in. A timer interrupt landed between `cmpo r14, 0x10` and `bg` in `unpack_lod_data`'s bit-buffer refill, the Huffman decode lost sync, and its output overran into the code tree. It only showed once the `notand` fix changed attract timing; the bug predates that fix.
- **`concmpo` / `concmpi` compare only when condition-code bit 2 (less, `0b100`) is clear**, and then set E (`src1 <= src2`) or G. Otherwise they leave the code alone. `cmpi x, lo` + `concmpi x, hi` + `be` is the ROM's range test `lo <= x <= hi`. They used to test the equal bit instead.
  - *Symptom that surfaced this in STF:* in the attract fight, Bean won the mutual grab that Sonic wins on the board. `get_en_info` range-tests the facing angle against ±0x1554 this way to build the enemy-info flags at `rob+0x720`, which the throw logic reads, so the flags were wrong from the first frame of the fight. `tools/match-replay.mjs` found it (see tools/README.md, "match_replay").

### Coprocessor (COP) — board-level math, every Model 2 game

The SHARC firmware itself is the reference for every handler here: `C:\Users\bigge\source\repos\ai\stf-sharc` holds annotated, reassemblable sources with Sega's own handler labels. They reassemble bit-for-bit to the ROM images. `cpres1.asm` is the COP (transform/math engine, 136 commands) and `cpres2.asm` the GEO feed. A `cpres1 PM 0x2xxxx` citation in `sharc_exec.h` is an address in `cpres1.asm`.

- **Rotation is accumulated by post-multiply**, not rebuilt from stored angles. `g_sharc.rot[3][3]` is column-major (SHARC convention): `rot[col][row]`. Each ang command post-multiplies the running matrix:
  - `ang_y` (0x04800909 → PM 0x201BF): `new_col0 = c·col0 + s·col2`, `new_col2 = −s·col0 + c·col2`
  - `ang_x` (0x04000808 → PM 0x201AA): `new_col1 = c·col1 − s·col2`, `new_col2 = s·col1 + c·col2`
  - `ang_z` (0x05000A0A → PM 0x201D4): `new_col0 = c·col0 − s·col1`, `new_col1 = s·col0 + c·col1`
  - Verified by reading SHARC firmware dispatch table at DM[0x30000] (originally `C:\temp\sharc_bone.asm`; the firmware sources now live in `stf-sharc`, below).
  - **Previous versions of this doc had ang_x and ang_z PM addresses and formulas swapped — now corrected.**
  - The old "M = Ry_LH × Rx × Rz rebuild" was only correct for a single clean ang sequence from identity.
- **Z-negation in matrix storage** (HLE convention): `matrix[r][2] = −rot[2][r]` for rows 0 and 1; `matrix[2][2] = rot[2][2]`. The 0x14802929/0x35006A6A handlers use `−iz` for rows 0/1 and `+iz` for row 2. This asymmetric negation together with the negated col2 storage produces output identical to the SHARC's raw column-major multiply.
- **Angles are 16-bit signed fixed-point**, `0x10000 = 360°`. Only the low 16 bits are meaningful.
- **`0x07800F0F` returns world translation T[0..2], NOT rotation entries**: both STF (PM 0x02043D) and FV (PM 0x020402) use `DM(I7, 0x09)` (hex `0x00006E7E48000000`) — a post-modify-by-9 instruction whose immediate is encoded in bits[31:27] of the lower instruction word. This advances I7 from slot[0] to slot[9]=T[0]; the LCNTR=3 loop then outputs T[0], T[1], T[2]. The i960 stores these to `g7+0x1F4` for collision/IK. The STF annotation correctly named this `read_world_pos`. Previous CLAUDE.md entry was wrong ("slot[1..3]=rotation").
- **`0x06800D0D` zeros T[0..2] (world translation), NOT rotation entries**: same `DM(I7, 0x09)` post-modify-by-9 positions I7 at slot[9]; three zero-writes hit T[0..2]. Both STF (PM 0x02042A) and FV (PM 0x0203EF) are identical. Previous CLAUDE.md entry was wrong ("slot[1..3]=rotation").
- **`0x07000E0E` (`Fn_load_point`, PM 0x020433) sets T, the current translation, to its three args.** Like 0x0D and 0x0F it reaches T through `DM(I7, 9)`, not the rotation entries. `calc_unit_mat` opens every part with `load_point(0, +0x678, 0)`. This file used to call it a no-op; ignored, it left whatever T the previous matrix had, and STF's unit matrices came out tens of units away.
- **The TGP bone slots are bufferram, and nothing keeps a host copy of them.** P1 at 0x3A00 and P2 at 0x3B00, 0x0C words a slot (col0, col1, col2, T), in the SHARC's external data space, which is bufferram. The IK (`0x6B`) and `0x67` write there, the i960 reads there, and so does `dump_tgp`. `g_sharc.tgp_bone`, a mirror that `0x35`, `0x37`, `0x67` and the IK kept for the COP-stream renderer, went with that renderer (Pinboard #251).
- **`0x33806767` (op 0x67) stores the current matrix into the TGP slot its one argument names** (firmware PM 0x020597, `stf-tools/dl-rig.mjs`). `0x3D00`, the kage matrix, comes through the same op, just past the two fighters' slots.
  - *Measured, and worth knowing before chasing it:* op 0x67 only ever stores slots 0, 1, 16 and 17 in attract (the two waists and the two chests), and `0x1A803535` was the last to compose those parts before the draw in every observed case (60 of 64 selects, the other 4 falling off the front of a truncated stream dump). The two ops do not agree: every 0x67 store differed from what 0x35 had composed, by up to 4.5 world units, so if the ordering ever changes this is not a cosmetic difference.
- **`0x35806B6B` (op 0x6B) two-bone IK — `args[12]` is the LOWER bone, `args[13]` the UPPER, and `args[14]` / `args[15]` name their slots in that order.** The upper bone (upper arm, thigh) hangs at the pivot; the lower (forearm, shin) starts one *upper*-bone length along the upper bone's own +X. `args[14]` is the lower slot — the left arm's pair is 0x3A30 (slot 4, forearm) and 0x3A24 (slot 3, upper arm).
  - The pairing is pinned by a MAME capture of a real fight (`stf-tools/motion-pose.csv`), which holds `args[12]` against the character record's forearm and `args[13]` against its upper arm; the two differ (0.3932 against 0.3464 for a shin and thigh), so it is not a coin toss.
  - *Why this hides:* getting it backwards still lands the hand or foot exactly on the IK target and still bends the limb by the right angle, because the triangle's two edges add to the same point whichever order they are walked in. Only the joint between them moves, to the far corner of that parallelogram — the bones swap ends and the knee folds backwards. It was worth 0.385 world units, one arm bone, and it took `tools/grade-pose.mjs` rather than a screenshot to say so.
  - The rotations either side of it were already right: both turns come out of one post-multiply chain, the first giving the lower bone's frame and the second the upper's, so the lower's has to be kept before the second turn overwrites it.
- **`0x19003232` (`Fn_fcurve_spl`, the motion Hermite) uses both tangents**: args are (span, t, v0, v1, m0, m1), with m0 the earlier key's out-tangent and m1 the later key's in-tangent. Both are scaled by span/30 (firmware constant `0x3D08882F`, cpres1 PM 0x210E9). Every spline channel of every motion goes through it: `get_fcurve_value_f` hands the coprocessor the segment and reads the value back.
  - *Symptom that surfaced this in STF:* the handler dropped m1. Stance motion 278 came out 2–3 binary radians off the board (MAME `motion-pose.csv`), and a turn (motion 265) about 15° off. `tools/grade-motion.mjs` now holds both fighters' motion arguments exact across the attract intro and a fight.
- **`0x1A003434` (`Fn_mov_matrix`) writes the current matrix into the GEO display list** at the byte offset its argument names (12 words, col0/col1/col2/T as the slot holds them), then replies 0. `set_obj_tpd` opens every draw with it and adds its own object command, and that draw carries replaced texture points.
  - *Symptom that surfaced this in STF:* the heads were empty shells. The eyes are drawn this way, their texture points shifted by the gaze (`snc_eye_thd_set` → `clip_medama` → `move_tpd_req`), and with a reply-only stub they took whatever matrix an earlier list had left at that offset.
- **Fight collision is a COP chain that passes state along in the firmware's own memory** (`sharc_coli.h`, ported from `cpres1.asm`). The chain is `0x7F`/`0x38`/`0x39` (ball world positions, previous positions kept 0x60 words on) → `0x3E` (into the P0→P1 frame) → `0x3A` (broad phase) → `0x70` ×2 (arena: **22 replies**) → `0x3B` (narrow phase: **4 replies**, plus the unit-overlap table at bufferram `0x90F600` that `coli_attack_chk` reads) → `0x3D` (push-out onto the unit matrices) / `0x72` (1 reply).
  - The radii, radius scales and ball→unit maps arrive through `Fn_write_ram` (`0x49`) at boot and on character load, so `g_sharc.dm` has to take those writes.
  - *Symptom that surfaced this in STF:* with stubs, no hit ever landed, fighters walked through each other, and every round timed out as a draw.
- **Projectiles take their hits from `0x3B807777` (`Fn_parts_oidasi`, op 0x77), not from the fight chain.** 4 args `(x, y, z, radius)`, **9 replies**: push-out x/z, last fighter hit (−1 none), its ball and unit, then P0 ball mask, P0 unit mask, P1 ball mask, P1 unit mask. `sub_8AE48` stores the masks at tobi +0x38/+0x3C/+0x40/+0x42, and `sub_2BEF4` sends a projectile into the damage routine only when the *target's* ball mask is non-zero. It reads the world balls `0x39` left at `0x1403E80` / `0x1407E80`, skipping a fighter whose ball 13 is more than 3.0 away. Flying parts (`epc_oidasi`) use the push-out from the same command.
  - The firmware's push factor is `1 − 2·d/(R+r)`, not `1 − d/(R+r)`: its reciprocal loop reuses `f11`, which the square root left at 3.0 rather than 2.0. Port the register semantics, not the intent.
  - *Symptom that surfaced this in STF:* the handler returned nine zeros, so no projectile ever touched a fighter — Fang's corks passed straight through. Driving Fang (B1 fires the cork) against the CPU is the quick check.
- **Command `0x2F005E5E` is scalar-then-vector**: arg0 = scalar, args 1–3 = vector → returns `(s*x, s*y, s*z)`.
- **The COP's arithmetic is not libm, and the i960 feeds it back.** √ (`_L202AE`), 1/√ (`_L2029B`) and ÷ (`_L205D0`) are seeded from the chip's 8-bit `rsqrts`/`recips` tables and run three Newton steps; atan2 (`_L202D1`) is Analog Devices' runtime routine; angles come back as `floor(rad · 0x4622F983)` & 0xFFFF (MODE1 = 0x18000, TRUNCATE). They agree with `sqrtf`/`/`/`atan2f` to six digits and differ in the low bits — which fighters' facings, distances and push-outs carry into the next frame. Use `sharc_fw_sqrt`, `sharc_fw_rsqrt`, `sharc_fw_div`, `sharc_fw_atan2`, `sharc_angle_word` (`sharc.h`), never host maths, and follow the firmware's operation order: `Fn_trans` adds its three terms to T one at a time, and the motion spline (`0x32`) is float Horner in the firmware's order, not a double Hermite.
  - *Symptom that surfaced this in STF:* the first attract fight (a replay — identical inputs on every board) drifted off MAME within ~350 frames and turned a thrown grab around ~690 frames in. `tools/match-replay.mjs` holds the fight against MAME frame by frame; `tests/cop_replay.c` shows these ops exact against a SHARC-side capture.
- **`0x2A805555` (`Fn_get_sm_ang_r`) and `0x2A005454` (`_f`) are real, and the motion blend reads them.** 9 args, 3 replies: the current matrix becomes identity (T too), is turned by nine angles (`_r`: z x y x y z z y x; `_f`: x y z z y x y x z; a zero angle skipped) and read back by `_L2117C` as atan2(col2.x, col2.z), **asin**(col2.y), atan2(col0.y, col1.y). Each also has a half-turn alternative, and the firmware keeps the smaller set — but it has overwritten the register holding |alt₀| with −1.0 by then, so the alternative is judged by `−1 + |alt₁| + |alt₂|`. Port the bug. `_r` used to return three zeros; in the attract fight that moved Sonic's z off MAME's on the fifth frame of a new motion (+348), and the error grew from there.
- **`0x02800505` (`Fn_get_matrix`) replies with the slot's raw words**: col0, col1, col2, T — not the row-major, Z-negated render matrix. Its readers are i960 code (`rd_ypos_ck_skp`, `os_set_matrix`, `calc_effect_matrix`).
- **`0x02000404` (`Fn_load_matrix`) is its mirror: the 12 words go into the slot raw, col0, col1, col2, T** (cpres1 PM 0x203AA). Its senders are `osage_dsp` (the camera at character select), `tails_heli_disp`, `ken_camera` and a few effect routines.
  - *Symptom that surfaced this in STF:* at character select, Fang's tail drew as stretched spikes and Bean's feathers lay on the floor. The handler still read the old 0x05's row-major, Z-negated format, so the camera came in sheared, and `os_set_matrix` composed every sway chain onto it with `0x45`. `Fn_osage` itself was exact on the board's records the whole time, and attract never sends 0x04, so only a capture at select shows it: `tools/grade-osage.mjs`.
- **`0x08001010` (`Fn_base_3x3`, cpres1 PM 0x20460) sets the current 3x3 to the identity and leaves T alone.** Whatever is drawn next faces the screen from the position reached. It is not a state no-op.
  - *Symptom that surfaced this in STF:* the handler returned without touching the matrix. The Flying Carpet's corner flames, the Death Egg's Earth card and `kira_kira_disp`'s sparkles were drawn turned with the world. They should face the camera. `tools/grade-stages.mjs` now holds every such draw in bufferram against the firmware.
- **The matrix multiplies run the opposite way to the listing's comments.**
  - `_L201EA` (`0x0B` `Fn_mul_matrix`, `0x37`, `0x45`) makes each new column rot × M's column, with T′ = rot × M_T + T. That is current · M in row-major terms, a post-multiply.
  - `_L2021F` (`0x46`, `0x47`) is M · current.
  - `sharc_compose` / `sharc_compose_rev` have it right. A replay written from the "M * current" comments does not, and `grade-cull.mjs`'s did.
- **`0x17002E2E` (`Fn_get_3d_len`, 3 args, 1 reply) was missing from the table**, so its floats went down as commands and the i960 read an empty FIFO as the length. A command the HLE does not know desynchronises everything after it: when the log has `SHARC: unknown cmd` lines whose "commands" look like floats, that is an argument count missing, not a new op.
- **`0x25004A4A` (`Fn_osage`, 1 arg) answers a variable number of words**: one echo per record in its bufferram stream, and for each segment record (type 5) another **12**, the segment's draw matrix. `osage_copro` → `os_set_osage_after` → `set_obj_fifo` reads those 12 words straight into the display list. The segment also writes its new point and carry back into the record, and `set_situation_flags` reads them next frame. That makes this real i960 state, not just drawing. The port is `sharc_osage` (cpres1 PM 0x2076E), and `SHARC_REPLY_MAX` has to hold a whole chain set (61 words for Bean).
  - *Symptom that surfaced this in STF:* the old handler only echoed type words, 13 against the board's 61, so every sway segment (Bean's feathers, Honey's pigtails, Fang's tail) took its matrix off an empty FIFO. What the i960 has switched off is its wind oscillator, not the chain: all 52 entries of the table at `0x68AA4` point at `0x689E4`, amplitude 0, and the live table at `0x68A04` is never referenced. Bit 0 of a record is a first-frame flag (`osage_init` 0x67600 sets it, 0x67D1C clears it). The coprocessor carries each point from frame to frame. `cop_replay` now matches every `Fn_osage` word exactly.
- **`0x40008080` (`Fn_zanzou_reserve`) is the one COP command whose length the opcode does not give you.** Every other handler reads a fixed number of words; this one reads until the i960 sends a terminator and answers as it goes: 4 header words (player, part mask, life step, bone length), then per trailing body part 4 words in (index + three object numbers) and **1 word back**, then `-1`, the turn angle, and **1 word back**. `4 + 4n + 2` in, `n + 1` out. `sharc_args_for_cmd` answers `COP_ARGS_STREAM` for it and `cop.h` feeds `sharc_zanzou_feed` a word at a time (`sharc_zanzou.h`); `sharc_exec` runs the same state machine over a whole captured argument list so `cop_replay` still works. Before that the arg table did not know it at all, so every word of the stream — floats, part indices, `0xFFFFFFFF` — went down as a *command*.
  - The engine is the COP's, not the game's: the i960 only names the parts and the spacing. The firmware measures how far each part moved since last frame (`0x7F Fn_coli_copy_unit_matrix` is what leaves that snapshot at DM `0x32000`/`0x320C0`, so it is part of this and not only of collision), lays one interpolated copy of the part's matrix per 0.1 world units travelled into a 128-slot ring at DM `0x32300`, and `0x82` ages them. `0x85` picks one of three model numbers by how much life a copy has left. A jump of more than 13.0 squared units is a teleport and lays nothing.
  - DM needed growing: the ring reaches `0x332FF` and `Fn_zanzou_init` clears `0x5480` words from `0x32180`, where `g_sharc.dm` stopped at `0x32FFF`.
  - *Which moves use it:* motion-script action `0x26` (table `_uk_player_actions` at ROM `0x1D1AC`, 13-byte record) sets `rob+0xC60` (part mask), `+0xC62` (life step), `+0xA1E` (turn angle) and `zanzou_ma` (spacing). Attract never plays one, so the quick way to exercise the path is to poke those three fields during any fight — `tests/cop_test.c` drives the FIFO conversation directly.

- **While COPRO_CTL1 (`0x980000`) bit 31 is up, a FIFO word is a halfword of the SHARC's boot image, not a command** (MAME `copro_fifo_w`; `g_cop.upload_words`). STF's `load_cop_loop` (`0xF14`) writes 14862 of them at boot, lowers the bit, waits for the COP's ready flag and then sends `0x00000000`, which is opcode 0x00 `Fn_initialize` (empty the matrix stack), not padding.
  - *Symptom that surfaced this:* `get_cop_diagnostics` reported 14863 unknown commands and logged none, because the unknown path dropped any word with a zero top half without logging it (issue #125). It no longer does.

- **`0x980004` reads the reply FIFO's status: bit 0 is up while it is empty** (MAME `fifo_control_r`; `cop_fifo_status`, the COPRO_CTL read callback in `memory.h`). STF's `cop_initialize` waits on it at `0xF3C` and FV's at `0x190C`; each profile used to hook its own loop. A read of the empty FIFO answers 0 and logs a WARN (`g_cop.underflows`): on the board the i960 would stall there, so it means a reply count is wrong.
- **`0x28805151` (`Fn_get_glo_ang`, cpres1 PM 0x2114F) answers the current matrix's three angles**: atan2(col2.x, col2.z), asin(col2.y) (`_L20332`, no clamp; `sharc_fw_asin_word`), atan2(col0.y, col1.y). It used to answer three zeros. Attract never sends it; `cop_test` holds it.

- *Note:* command opcodes documented here are the ones confirmed in STF. Other games may use additional opcodes — log unknown commands at WARN and extend the dispatch table.

### 3D Polygon Decoder (board-level — confirmed against two games)

Index-array decoder. Confirmed **J=1.0 on 4402/4405 STF models** AND originally reverse-engineered from Daytona at **J=1.0 on 2377 Daytona models**. The cross-game validation is what makes this load-bearing: the algorithm is the Model 2 board's polygon format, not an STF quirk.

STF reference dataset: `C:\m2\3d\new\stf-poly` — 4405 OBJ files, 5-digit zero-padded filenames (e.g. `00001.obj`). Treat as ground truth for STF.

**iFlag = `vp[25] & 0x03`:**

| iFlag | Meaning |
|-------|---------|
| 0 | Sentinel — previous group ended; start fresh strip |
| 1 | Carry far edge of previous face (`Index[-4]`, `Index[-2]`) |
| 2 | Plain new quad group |
| 3 | Anchor new strip off previous corner (`f1==1` → `Index[-1]`, else `Index[-2]`; `anchor_b = Index[-3]`) |

**iFlag 0 wipes its group at the head of a mesh too.** The board culls any polygon whose own link type is 0 (MAME `check_culling`, `model2_v.cpp`, before `model2_3d_process_polygon` rasterises). AM2's one-link 2×2 shadow card at y = −2 is a single quad of link type 0 (`attr 00040401`), so it draws nothing: 70 STF entries decode empty. Do not skip the wipe at the head. The explorer's Daytona port once did, and drew a square the board culls (noclip#24, fixed in #26).

**Face loop:** `i < n_idx - 8`, always 2 groups behind tail.
**Face type:** `f1 == 2` → triangle; otherwise quad with **A-B-D-C** winding.
**Vertex convention:** `(x, y, -z)` — Z is negated on read.

- **The UV stream runs B,A,C,D for a quad and B,A,C for a triangle — no flips.** Negating Z reverses the winding, so the stream walks each face's loop the other way from the index array. Corners shared along a strip carry one UV in ROM, so the right order is the one that agrees with itself: 96.5% of shared corners for this order, 75.5% for the old A,B,D,C-with-flip-U-and-V, which had been picked by eye and left 38% of corners on a different texel (STF's "CAUTION" sign drew upside down and mirrored). `tools/grade-models.mjs` now holds all 1,795,005 textured corners to the explorer's exactly.
- **A quad whose four corners were already emitted in this model is cut along the same diagonal as before.** A decal is the surface's own faces emitted again with a cut-out texture; if the copy is cut along the other diagonal, a warped quad bulges differently and half the decal sinks behind the surface. The corner key is the explorer's `cornerKey`, bit for bit. This rule, and nothing in the connectivity logic, was the whole of the J = 0.990 that `grade-models.mjs` first measured. It is now J = 1.000000 over 598,728 triangles.
- **A game frame draws each face at the board's own sort key, and that key is the whole depth test** (`geo3d_flat_depth`, Pinboard #247). Bits 10–11 of the polygon's attribute pick its z: 0 is the previous polygon's (carried across objects, reset each frame, set by culled polygons too), 1 the nearest corner, 2 the farthest, 3 1e10. `float_to_zval` turns it into a 16-bit key (`geo3d_board_zkey`), and the face is drawn flat at depth (key + 0.5) / 65536 under LEQUAL, in submission order: the nearer key wins and a tie goes to the later polygon, as on the board (MAME `model2_v.cpp`). The vertex shader passes the depth as a flat varying and the fill writes it (`gl_FragDepth` / `SV_Depth`).
  - *What it replaced:* the explorer's half of the rule (a polygon may take its sorted depth only to recede, by at most a bound) and seven patches over it, each from a picture MAME had right: the recede bound, the profile's `zsort_standing` list (Aurora's ice pillars, issue #78; Casino Night's slot reels, Pinboard #244), keep-depth for far-corner pairs (the Tails-lab screen, #85), the coplanar layers and their planes (Flying Carpet's pyramid shadows and Casino Night's reels, #75), decal ties across draws (Tails' closed eyes), same-matrix runs (the slot machine, Pinboard #133) and held pairs (#225). All are gone from the game draw. The layers stay for the object viewer, whose free camera has no board sort to copy.
  - *Measured* with `grade-zsort --toggle zflat` on all fifteen stages' replays against MAME: 325,730 of 332,250 changed pixels nearer MAME with the key, 2,655 nearer the half rule (stages 0–13; stage 14 replays as stage 0 on both boards). No stage has more than 289 pixels nearer without, and those are a pixel's edge here and there, not a picture. The off side is the half rule with no patches; against the half rule with all seven, before they were taken out, the key was nearer MAME on every stage as well. The Death Egg II cutscene (`grade-lunar-fox --fixrand`) holds too: the emblem's worst crop is 0.74 a channel, where the held pairs got 1.67, and every space-shot frame is nearer MAME than with `zflat` 0, the key-0 eye clip included.
  - `set_camera {"zflat":0}` brings back the half rule alone for an A/B.
  - **On GLES the key is the vertex z, not `gl_FragDepth`** (`FLAT_VERTEX_DEPTH`, `g_game_render_vertex_depth`; Pinboard #300). Writing the depth turns off early z, and on the ARC-S's Mali a fight's overdraw then made the frame GPU-bound: a vs-CPU match ran at ~49 fps (the swap waiting ~6 ms) while attract held 60. The vertex shader puts slice·65536 + key on an odd multiple of 2^-25 just under depth 0.5, the middle of its 24-bit step, where z/w rounds by far less than a step; the match holds 60. The libretro core's frames read back (glReadPixels) are identical to `gl_FragDepth`'s in all 4206 frames of attract and a fight. llvmpipe, which extrapolates z from its own plane equation, differs on a few pixels in 11 of 14,242 attract frames, so desktop GL and D3D11 still write the depth. `$M2HLE_VDEPTH=0/1` overrides the choice. Do not move the value off the step's middle: a quarter step in put 8,261 llvmpipe frames off.
- **Polygons the board gives sort key 0 all tie, and the board has no near plane** (`GEO3D_ZSORT_KEY0`, `geo3d_sort_z`; MAME `float_to_zval`, `check_culling`). A sort corner behind the eye, or more than 12 binary orders nearer than the window's z_adjust, is key 0, and a tie goes to the later polygon. The board clips only against the four sides through the eye. Under the flat key that falls out: key 0 is the nearest depth, LESS_EQUAL hands a tie to the later draw, and a flat face's vertices all take clip z = 0, so the clipper cuts only at the eye plane (w = 0), never at the near or far plane. Under the half rule (`zflat` 0), `GEO3D_ZSORT_KEY0` does the same with one sort z just behind the near plane; `set_camera {"zkey0":0}` turns that off.
  - *Symptom that surfaced this in STF (Pinboard #242):* in the Death Egg II cutscene's space shot the camera sits inside the Lunar Fox, drawn at scale 0.01 with z_adjust 4.0, and 727 of its hull's 759 faces, the canopy (231) and Sonic's head (3027) are key 0. The near plane cut the hull open, and the canopy covered Sonic's head, which MAME draws over it. `grade-zsort --stage 1` / `5`: no pixel changes.
- **The fill follows `model2rd.ipp`, through the explorer's port of it** (`game_render.h` fill shader against `vendor/noclip/js/viewer.js`). Texture-header bits it depends on:
  - bit 13 on a textured face: the transparent renderer, where texel 15 is a hole (four-tap coverage ≥ 0.5 survives)
  - bit 15: checker, drawn where (x ^ scanline) & 1 in board pixels. The fill works that out from the clip position (`bpix`), never `gl_FragCoord`, whose rows count from the bottom: on a 384-row target that is the other phase. `set_camera {"checker":0}` brings the old phase back (`GEO3D_FACE_CHECKER_ODD`).
    - *Symptom that surfaced this in STF (Pinboard #242):* the Lunar Fox's launch smoke and the space shot's canopy were 6–22 a channel off MAME in `grade-lunar-fox`; with the board's phase they are within 3.3. `grade-zsort --toggle checker`: 205,629 of 216,150 changed pixels nearer MAME on stage 0, 25,037 of 31,988 on stage 1, 131,103 of 138,002 on stage 5.
  - bits 8 / 9: mirror X / Y, where an odd repeat of the tile reads back to front
  - bits 6 / 7: smooth wrap X / Y (face flags 256 / 512, the explorer's bits). Only with the bit does the bilinear pair at a tile's last texel blend into the next copy's first. Without it the board clamps: it takes the nearer texel of the pair (`fetch_bilinear_texel`, `!tex_wrap_x && u1 == 0`). MAME clears wrap on a mirrored axis, but the mirrored tap is the edge texel again there, so no masking is needed.
    - *Symptom that surfaced this in STF (issue #81):* South Island's sky ring (one panorama over four 256×256 tiles in 16 segments; its faces set neither bit) showed a one-pixel seam of the tile's far edge at every join. The ground and water set both bits. `set_camera {"texclamp":0}` brings back the old always-wrap filter for an A/B, and `grade-zsort.mjs --stage 0 --toggle texclamp` grades it against MAME.

  Sampling is bilinear with the half-texel offset. The lumaram band is indexed with the *filtered* texel (`lumabase + t*120`), not the nearest of 16. Mip level L sits at `((tx-2048)>>L)&2047, ((ty-1024)>>L)&1023`, on alternating sheets. `grade-models.mjs` checks the face flags against the explorer.

- **The GEO mode word's bit 1 means "no normals": the face's normal is (B − A) × (C − A) of its transformed corners, not the ROM normal** (`geo3d_board_normal`; MAME `geo_parse_nn_*`, picked by `mode & 3`). Either way the rear test and the lighting take N·P at C, the link's first new point, which is the corner the geometrizer reads after the normal. A warped quad's ROM normal misses B and C in about one STF face in seven. `set_camera {"nnormals":0}` turns the corner normal off.
  - *Symptom that surfaced this in STF (Pinboard #160):* fighter shadows came out shredded, and Knuckles' dreadlocks at character select worst of all. `rob_kage_disp_test` draws every shadow in mode 2 (`set_mmode`, GEO command 07), under a kage matrix that flattens the fighter onto the floor. A ROM normal pushed through that matrix says nothing about which way the flat face points, so the rear test dropped faces at random. The COP side (`Fn_kage_mat` / `_poly` / `_flag`) was exact against MAME the whole time (`cop_replay` on a select capture).

- **Direct data (GEO 0x02/0x12) is walked and drawn, though no STF or FV frame sends it** (`geodl_direct_len`, `geo3d_decode_direct`; MAME `geo_direct_data`). The command carries texture point and header addresses and two corners, then per polygon an attribute (`attr & 3` = 0 ends it), luma, distance, one corner, and a second when `attr & 1`. Every word but the attribute is pushed `>> 8`. A list carrying it used to stop the walk short of END, and the frame went to the COP-stream renderer (`geo3d_scan_captures`) with its fitted camera signs, eye-bake test and shadow floor. That renderer is gone (Pinboard #251): a list that does not reach END now draws no 3D.
  - *Measured before it went:* about four minutes of STF and FV attract and a round on each of STF's fifteen stages send no direct data, and every list reaches END but the first two after boot. So the fallback drew only those two frames, and the port is checked against MAME's code, not a picture. The first game that sends it is its first test.

**Model-table mesh pointer is encoded, not a raw offset**: actual ROM offset = `(ptr * 4) - 0x02000010 + 0x10` for STF. The `* 4` is hardware; the base may shift per game — verify before trusting on a new ROMset.

**Connectivity is the iFlag table above and nothing else.** The simple rule `(vp[25] & 3) != 2` gives J≈0.708. A bitmask heuristic, including a brute-forced `connect_when` mask `0x45B4` that the profiles once carried as `poly_connect_mask`, peaked at J≈0.71 and was abandoned; no decoder in this repo or in m2-hle ever read the field, and it was removed. Do NOT go back to a fan-mode or bitmask rule. For a new game, check the decoder against that game's reference renderings (`grade-models.mjs` is the model).

### Memory Bus (board-level)

- **Region table is linear-scanned in declaration order.** TILE (`0x01000000`) MUST appear before H_SYNC (`0x01040000`) or H_SYNC reads route to the TILE handler.
- **IO region initializes to `0xFF`, not `0x00`** (hardware idle state).
- **Tile RAM is 64K and mirrors at `0x01010000`** (MAME `mirror(0x110000)`); the `TILE_MIRROR` region shares TILE's buffer and must precede TILE in the table.
  - *Symptom that surfaced this in STF:* the NEXT MATCH screen had no KNUCKLES nameplate. `rm_char_disp_int` shifts long names one tile left with a table offset of `0xFFFE` loaded by `ldos` (zero-extended), so the plate is written at `0x01011442` and only reaches tile RAM through the mirror. Metal Sonic's plate uses the same offset.
- **A board timer that is not counting reads `0xFFFFF`, not 0** (`IRQT_IDLE`; MAME `machine_reset` / `model2_timer_cb`): before its first write and after it expires. `mem_init` resets the timers, so every boot starts so. STF's `rand` (`0x66B0`) adds all four timers into its state at `0x500098`, and STF never arms timer 2, so from character select on every random number (the CPU opponent's choices included) took a 0 the board does not give. Attract never reads an idle timer, so attract and the replay fight are unchanged.
- **`GEO_CAPTURE_SIZE` ≥ 32768.** Smaller sizes wrap mid-frame and produce partial 3D snapshots / flicker.
- **`mem_init` clears the six heap regions in place on a re-init; it never frees them** (`mem_region_fresh`). `main_data`, `xtra_data`, `vid_ext_ram`, `texram0`, `texram1` and `framebuffer` keep their addresses for the life of the process. A netplay session re-runs `mem_init` on the emu thread while other threads hold those pointers, and none of them take the emu mutex for the whole of their read: the frame callback loads `texram0` once and decodes two million texels through it (`game_render_upload_atlas`). Do not go back to `free` + `calloc`, and do not try to fix the readers one at a time instead — `1c62bf0` did that for the MCP bridge and the crash simply moved. `tests/mem_test.c` holds the addresses across a re-init.
  - *Symptom that surfaced this in STF:* accepting a netplay challenge killed any client that had a window, every time, between "barrier released … resetting the board" and the first frame. Headless clients have no frame callback and never saw it, so the graders and the bot's training runs were all green. It faults rather than reading stale bytes because a 1 MB block is its own mapping and `free()` hands the pages straight back.
  - *How it was found, which is worth repeating:* Windows had already kept a dump (`%LOCALAPPDATA%\CrashDumps`, LocalDumps is on for this machine) and `cdb` is installed with the Windows Kit. The release build has no PDB, and did not need one: the faulting instruction was a bounds-checked, in-range read through a heap pointer, the pointer in the register was not either of the bus's current `texram` pointers, and the netplay log ring in the dump said the reset had just run. The WER record's PE timestamp (`P3`) says which exe a fault offset belongs to.

### Tile Renderer (board-level)

- **16-bit byteswap on pixel bytes**: indices `[0,1,2,3]` are read as `[1,0,3,2]` (XOR low bit of byte index). Within each swapped word, high nibble = left pixel, low nibble = right.
- **Tilemap entry** (MAME `segaic24` tile_info): bit15=priority, bits[14:7]=pal_bank (**8-bit**, `(entry >> 7) & 0xFF` — bit 14 is a palette bit, *not* h_flip; `change_bg_color` uses it), char = low bits. Full tile index = `entry & 0x3FFF`. Palette LUT index = `pal_bank * 16 + color_idx` (stride=16 entries = 32 bytes per bank). Verified: CG87 palette written to pal+0x660 = bank 51×32; tile entry pal_bank=(0x9980>>7)&0xFF=51; pal+51×32=0x660 ✓.
- **Color index 0 is transparent on foreground layers only**; background layers fully opaque (pass `NULL` for `alpha_out`).
- **Four tilemaps, each with its own scroll, and a window mask per pair** (MAME `segaic24` draw_common, `model2_v.cpp` screen_update). Tilemap t sits at tile RAM word `0x1000*t`, H scroll `0x5000+t`, V scroll `0x5004+t` (bit 15 disables), and samples at `(x − hscroll, y + vscroll)`. Pairs 0/1 and 2/3 share a control word (`0x5004` / `0x5006`, bits 14:13) and a mask (`0x6000` / `0x6800`, four words a line, one bit per 8 px):
  - control 0: the even tilemap draws where the mask bit is 0, the odd one where it is 1;
  - control 1: split at line `−vscroll`;
  - control 2/3: split at column `hscroll`.

  Behind the 3D go tilemaps 3 and 2 opaque, then 1 and 0 with tile bit 15 clear. In front go 3, 2, 1 and 0 with bit 15 set.
  - *Symptom that surfaced this in STF:* NEXT MATCH draws each fighter's art as a top half in tilemap 2 and a bottom half in tilemap 3, stitched by a control-1 split. The renderer drew only the even tilemap of each pair, so both fighters were cut off at mid-screen. It also showed leftover "WAITING FOR CHALLENGER" tiles that the split hides.

### Sound board (board-level — `sound.h`, `scsp.h`, `m68k_exec.h`)

The 68000 runs the game's own sound driver (per-game code: three Hiro driver versions across the catalogue), and the SCSP is emulated at its register interface, **bit for bit as if it ran one sample at a time in lockstep with the 68000** — 256 clock periods per 44.1 kHz sample. The host audio callback (`core/audio_out.h`) only drains a ring. [SCSP.md](docs/SCSP.md) has what the chip does and what STF's driver uses of it.

- **The SCSP makes its samples late, a voice at a time, and is exact because everything that could see the difference makes it catch up first** (`scsp.h`, "The chip's own time"; Pinboard #166). `scsp_tick` marks a sample owed; a full `scsp_sync` (every slot, then the DSP and the mix, in order) runs at the end of `sound_run` and before any register write that changes what the chip makes. The slot monitor, a slot's KYONB and a 68000 write inside a slot's sample run only that slot on (STF's driver writes MSLC nearly every sample and reads the monitor every ~5; neither may sync the chip, or there is nothing to gain). FM, the noise source and a slot reading the DSP's delay line make the chip `coupled`, which is the old per-sample chip exactly.
  - **A slot's RAM range (`scsp_slot_range`) must hold for the voice's whole life until its registers change**, not just one run. Two traps found by `tests/scsp_lazy_test.c` (a ctest): a forward loop shorter than one pitch step lands past its end again and creeps on through RAM, and MAME's alternating loop reads a position that stepped below 0 as past LEA and reflects it far beyond the loop. Anything not provably bounded claims all of RAM. Add a loop or addressing mode here only with `scsp_lazy_test` extended to it.
  - The DSP's delay line pages are left out of the 68000's direct read map (`sound_map_pages`), so a read there syncs first. STF's is alone at 0x70000–0x7FFFF.
- **Do not go back to a key-on event mixer on the audio thread.** The driver reads the chip back and acts on it: the slot monitor's play position CA (0x408/0x409, MSLC-selected) paces its streaming of long samples 8 KB at a time and decides which voice to take back; KYONB clears itself when a voice ends; SCIPD, the timers and the MIDI input buffer are its clock and command channel; the DSP's delay line is in sound RAM. With the old mixer the driver kept 25–32 voices keyed where MAME holds 5–16, and the voice allocation split from MAME 2.5 s into the music.
- **The 68000 sees all 8 MB of sample ROM**: 0x800000 (first 2 MB), 0xA00000 bank 4 (+2 MB), 0xE00000 bank 5 (+6 MB); the sound-control register at 0x400000 only re-banks sets larger than 8 MB. STF keeps 458 of its 602 samples above 0xA00000 — the old 2 MB window copied silence for three quarters of the instruments.
- **The i960's bytes reach the SCSP over a serial line, not at once** (`sound_uart_t`, `sound.h`; MAME 5039631dda3 routed the i8251's TxD into the SCSP's receiver). The UART's bit clock is free-running from power-on with its ticks at 18 µs mod 32 µs of board time; a byte written to an idle UART starts at the next tick, the UART holds ONE more byte that starts when the line frees (10 bits on), and the SCSP takes a byte 9.5 bits (304 µs) after its start bit. TxRDY, the i960's interrupt, is up whenever the holding register is empty, so the i960 sends a command's third byte 320 µs after the second (MAME's capture, byte for byte). The run loop offers the i960 the interrupt only then (`sound_uart_make_room(true)`), running the sound board ahead inside the slice to get there; a writer that is not the game queues up to 32 bytes. Read off MAME's `diserial` LOG_TX/LOG_RX logging; `tools/mame/snd_timing.py` measures the byte-to-interrupt delay on a capture (313 µs median, MAME and ours). Before this the byte landed 4 µs after the i960 wrote it, and every command was a millisecond early. `emu_service_sound_again` still re-runs the sound handler within a slice while the i960's queue has bytes (one byte per frame put commands ~50 ms late).
- **Take the sound interrupt when the game enables it, not once a slice** (`emu_offer_sound`, kicked by `irqt_enable_write`). STF's command queue `AUDIO_2` (`0x504020`) is 32 entries by its index mask and count cap, but the ROM reserves only 18 longs. Entries 18-31 overlap `byte_50406A`, `sd_nowait_timer`, `sd_wait_timer`, the `check_same_sound` list at `0x504078` and the `sd_flag` pointer, which the game writes every frame (`0xB618`, `0x39000`, `0x3F53C`-`0x3F5A0`, `0x3F440`). On the board the UART empties the queue within a millisecond, so nothing waits long enough to be hit. Offered once a slice, a command waited up to a frame and went out as zeros or a pointer (`00 00 00`, `0D B1 A8`), which the driver reads under MIDI running status.
  - *Symptom that surfaced this in STF:* some stages, some of the time, played the wrong music or none, depending on which slot the BGM command happened to land in. The i960 sent the right code and the 68000 played the right song for it (held against MAME song by song); the command was destroyed in between. `sound_codes` over the bridge shows every command the i960 sent, framed, and `queue_hi` the queue's depth.
  - The MIDI buffer's backpressure is `sound_uart_make_room`: the board runs early inside the slice, and the slice owes the samples back (`g_sound.ahead`), so the clock never gains one. The sound board now decides when the i960 takes an interrupt, so its state is board state: `slice_frac` resets with it, and a change here is a `NETPLAY_PROTO_REV` bump.
- **In a 2P game the ROM plays South Island on Canyon Cruise and Casino Night** (`stage_bgm_select`: `stage_num` 4 or 5 with `gameprogram == 2` sends `0xAE1004`; IDA shows the constants as `prcb` / `prcb+1`, i.e. 4 and 5), and Sonic against Knuckles plays North Wind on any stage. That is the arcade's behaviour, not a sound bug.
- **Sega's own console DLL has no sound board to copy** (`stf-pxd-w64-d3d12_retail.dll`, trap table entry 15). It emulates neither the 68000 nor the SCSP. A trap at `sound_request_special` (`0x3F268`) takes the code out of g0 and returns, so the queue stays empty and the UART is never written. The code is looked up in a sorted table at DLL `0x180126a70` (123 entries: code, category, cue name) and played as a CRI ADX2 cue from `rom/sound/stf_all.acb` (`FUN_180004b80`). Category 5 is BGM (`bgm00`–`bgm18`, one playing at a time) and 2 is effects and voices, some 90 of them. A code not in the table plays nothing. `0xA00001`/`2`/`3` stop both categories, BGM only or effects only. `0xA003xx` fades BGM to silence over `xx` frames. The `0xAE14xx` codes stop one looping cue by name. It is re-recorded audio behind a code table, not a model of the board, so it cannot serve as an oracle for `scsp.h`. MAME stays the reference.
- **Timing is what makes the music keep time, and it is measured, not assumed:**
  - 68000 instruction time comes from Motorola's tables (`m68k_timing.h`) plus the run-dependent parts (branch taken, DBcc, shift count, MOVEM registers, MULx bits, and DIVU/DIVS by the operands: `m68k_divu_cycles`, the chip's 15-step restoring division as Musashi counts it, where the table charged the worst case). A bus-access count ran 0.2% fast.
  - **Every sound RAM and SCSP bus cycle costs one wait state** (`m68k_state_t.wmap`, charged per access in `m68k_rb`/`rw`/`rl` and the writes; MAME `model2_snd` `before_delay`). The driver's timer handlers reload their timers 166 / 200 clocks after the vector fetch in MAME; without the wait states ours took 128 / 160, and that 40 clocks in every handler is most of what made the board run 0.2% fast against current MAME (notes within 30 ms went from 22% to 71% on it alone).
  - **An interrupt exception is 48 clocks plus its five wait states**, not 44: MAME's cycle-level m68000 spends 8 on the autovector acknowledge (`state_interrupt_df`, m68000-sdf.cpp).
  - **A pending interrupt is taken straight after an RTE that unmasks it.** MAME's trace shows timer C's handler entered from timer B's RTE with no instruction between; the old rule that let one instruction run first put the two handlers 70 clocks further apart, every period.
  - **The cycle-level check is `tools/mame/m68k-trace.lua` + `m68k_timing_compare.py`**: MAME's instruction order and the clock of every opcode fetch (the debugger's `{tracelog}` action writes nothing under `-debugger none`, even with `focus`), against snd_replay's `$SND_TRACE`. It pairs identical instruction sequences, so the prefetch cannot skew it; the driver's handler paths agree to 0-8 clocks over 24-52 instructions. Find a timing bug with it, not by fitting a constant.
  - SCSP timers count from the write, in clock periods, not whole samples: the driver reloads them inside its interrupt handler, and that delay is part of every period (MAME: 49.884 samples for a 49-sample timer B). Rounding to samples ran 2% fast.
  - The 68000 samples its interrupt lines `SOUND_IPL_LEAD` (4) periods before an instruction ends: MAME's m68000 loads the pending interrupt where the microcode moves IR to IRD, with the instruction's last prefetch, 4 clocks from the end of most instructions. The 10 it used to be was a fit that covered the missing wait states and exception clocks. With everything above, timer B's period is 50.0146 samples against MAME's 50.0142 and timer A's 505.37 against 505.44 over 90 s of attract (8 ppm; `snd_timing.py`). *Open:* for an instruction whose last prefetch comes early (`ror.l #8` is 24 clocks with the prefetch first, and the driver's poll loop is ten of them), MAME samples up to 20 clocks before the end, so its fire-to-handler latency is bimodal (46 clocks in half the cases, then a tail) where ours is one hump; the means match, the medians differ by 7 clocks, and that shape is what still decides the first slot race, at 31.7 s.
- **MAME quirks reproduced on purpose** (it is the oracle; each is marked `MAME:` in `scsp.h`): timer period (255 − reload) samples; interrupt sources raise lines one at a time in the order A, B, C, MIDI and lines stay up until SCIRE; the slot monitor (0x408) is latched once a sample for the slot MSLC named then, so a read right after a new MSLC still sees the old slot (MAME 458507e06bc, from hardware; it used to be worked out at the read and reset MSLC); a slot with SDIR bypasses TL into the DSP feed and the direct mix too (MAME, flashbeats); the DSP touches delay memory only on odd steps.
- **The oracle moved on 2026-09-23** (claude_mame's MAME merged upstream `1d6dbfafe53`) and the board followed it on 2026-09-28: the serial line, the wait states, the exception time, the monitor latch. A capture from a MAME older than that grades this board at 22%, and the board before that MAME at 91%; `tools/scsp_mednafen/README.md` has both tables. Grade against the current build, and say which in a PR.
- **DSP MADRS is 32 words at 0x780-0x7BF; 0x7C0-0x7FF is NOT a mirror** (`scsp_w16`). MAME mirrors it ("MADRS is mirrored twice", a 2014 array-bounds fix, not hardware); Mednafen maps nothing there. Sega's driver decides it: STF's writes 0x700-0x7FF in one pass, the tap addresses and then zeros over 0x7C0-0x7FF, so with the mirror every tap read one address and the reverb settled on a constant, +19000 on EFREG 0/1.
  - *Symptom that surfaced this in STF:* the board's output, and MAME's WAV, sat on a DC offset of ~5000/32768, which CLAUDE.md used to record as a fact of the board. `tools/scsp_mednafen/snd_lockstep` (mirror mode, `SND_LOCKSTEP_STATE=1`) found it as the one DSP register block the two chips held differently. With the fix the EFREG means agree with Mednafen's to 0.1% and the offset is gone.
  - This is a known departure from the oracle: MAME's WAV keeps the offset. `snd_compare.py` grades envelopes, so its numbers do not move; a sample-level comparison against MAME's WAV has to remove each side's mean. `audio_out.h` still high-passes the host output, for any game whose DSP does leave one.
- Grade with `tools/mame/snd_capture.py` → `tests/snd_replay.c` → `tools/mame/snd_compare.py` (see tools/README.md, "The sound board"). To prove a sound-board change bit-exact, `snd_replay` the capture through both builds and `cmp` the outputs: the board-state A/B (`ab-builds`) never sees the SCSP's output.

- **`--sound-hle` runs STF's sound driver in C instead of on the 68000** (`sound_hle.h`, docs/SCSP.md "The driver in C"; Pinboard #173). Off by default; it takes only STF's program ROM (a hash of its code), so the 68000 stays the path for everything else, homebrew included. The web page offers it under Feels laggy? (`web_set_sound_driver`): with no session it restarts the sound board at once, in a session it waits for the next match's cold boot, because a sound reset moves the UART's clock and the i960 reads that. The chip is scsp.h either way: the port writes it and sound RAM through the paths the 68000's bus takes, streaming included, and keeps the driver's own RAM layout, so captures and RAM dumps read the same. `tools/grade-sound-hle.py` holds it to the board (99.9-100% of key-ons, a few ms).
  - **The i960 cannot tell, and that is what makes it safe online**: board time (`m68k.cpu.cycles`) runs without the 68000, so the UART is clocked the same; only a slice sending more than ~50 bytes could differ (the run-ahead cap), and STF never does. `det_digest --sound-hle` is identical frame for frame to the 68000 over attract and a scripted game. Keep it so: nothing the i960 reads may depend on the driver's timing.
  - **Do not play samples straight from ROM instead of streaming them.** A long looped sample's loop is what the driver's streaming makes of it (it copies whole 0x200 chunks past the sample's end, relying on the ROM repeating the loop start there); the sample table's loop fields disagree with that by 2 bytes in 26 of 95 loops and by 12 KB in one. The first version did, and was not the board.
  - Boot facts the port needed, each measured on the board: the driver takes no interrupt for 2.94 s after reset (and after a MIDI `0xFF` restart), keeps the master volume at 0 until then (a DSP start-up transient is under it), clears sound RAM for 0.4 s first (voices fall silent as it passes their windows), and InitSCSP reads MIBUF four times, which throws away what waited in the MIDI FIFO (the i960's first command at power-on is lost on the board too).
- **The SCSP's MIDI output is the i960 UART's receive line** (MAME `midi_out_cb` → i8251 `write_rxd`): a byte the 68000 writes to MOBUF is what the i960 reads at `0x9C0000`, with RxRDY (status bit 1) up while one waits (`sound_midi_read_cb`). STF's driver never writes MOBUF. m2-pacman's sound program answers the i960's ping (`0xF0`) with `0x5A` there, and without the line the game ran silent.
- **A status read that finds TxRDY down runs the sound board on until it rises** (`sound_midi_read_cb` → `sound_uart_make_room(true)`, the interrupt path's accounted run-ahead). Nothing clocks the line while the i960 runs its slice, so a program that polls `0x9C0004` bit 0 instead of taking the interrupt saw the holding register full for the rest of the slice: about two bytes a frame. STF sends on the interrupt and is untouched (`ab-builds --sound` identical).
  - *Symptom that surfaced this (m2-pacman):* the game's own SPEED panel fell from 99% to 31% when a ghost caught Pac-Man, and the death never played; the i960 sat in the program's UART flush loop (`0x117E0`). MAME holds 99% through the same inputs.
- **`0x4E72` is `STOP #imm`; `0x4E74` (RTD, 68010+) and `0x4AFC` are illegal on the 68000.** The two used to be swapped, and a driver that idles on `STOP` (m2-pacman's) halted the sound CPU. STF's never executes either.

### Backup RAM (`core/backup_ram.h`, board-level)

The board's battery-backed SRAM at `0x01D00000` (settings, region, coin setup, bookkeeping). The first 16 KB is kept: MAME's `nvram/<set>/backup1`, byte for byte, so the two can trade it.

- **Only a player's own boots keep it.** On by default for the desktop window (`%APPDATA%\m2hle2\nvram`, `~/.config/m2hle2/nvram`), the handheld (sdl3, same folder), the web build (localStorage `m2hle2.nvram.<set>`, flushed on `pagehide`; off for `?script=` runs) and libretro (`RETRO_MEMORY_SAVE_RAM`, the frontend's `.srm`). Off for `--headless` and `--kiosk`, so the graders, the tests, `det_digest` and the fly boot blank as before; `--nvram-dir` / `--no-nvram` override.
- **A netplay session is a cold boot that must be identical on both machines, so it detaches the battery** (`backup_ram_detach` before every netplay reset, and on a PS3 match before the rules hook writes the room's settings into it). The detach flushes the player's copy first; nothing a session does reaches the file. The player's comes back when the room empties (`netplay_restart_alone` → `backup_ram_reattach`) or with the next ROM load.
- **A reset keeps it** (`mem_init` saves and restores it while attached), as the battery does. That includes the bridge's `board_reset` and the idle hold, which are not sessions.
- **Homebrew on a stock set gets its own file**, `<set>-<fnv32 of the program>`, so it neither reads nor clobbers the game's.
- The image is checked once a minute and written only when it changed, then flushed at exit. STF bumps a counter at `0x1D03319` every few seconds for as long as it runs, so a shorter check is a write every few seconds, which wears out a handheld's SD card.

### Homebrew on a game's board (`profiles/sfight_homebrew.h`)

Homebrew ships as a stock set with the program EPROMs swapped (m2-pacman: `sfight` with `epr-19001.15`, `epr-19002.16`, `epr-19021.31` replaced). The game's profile would plant its HLE hooks and interrupt handlers in the homebrew's code, so after a load `profile_adopt_program` reads the program's own interrupt table (ROM word 1 → PRCB → +0x14) and, if it does not name the profile's handlers, runs the set's `any_program` profile instead: no hooks, `irq_vectors`, `board_vblank`. A patched build of the game keeps its table and so its profile. Every profile's slice ends at the board's vblank (HLE Hooks, "Frame pacing"), so `board_vblank` only says the program has no frame hook: the run loop marks the capture's frame and makes the match_replay jump at the vblank instead. A handler still in service after 8 slices is taken to have switched task (m2-sdk's break-in does `flushreg` + `bx`).

### HLE Hooks (game-specific addresses, board-level patterns)

These addresses are STF-specific. The **patterns** repeat across the catalogue — every Model 2 game ships some form of COP self-test and frame-loop entry; the addresses change per ROMset.

- **`CoProcessorErr` at `0x74E4` (STF)** must be bypassed. Return via `locals.rip` (saved return address from the call frame), NOT normal IP advance. Without this, STF hangs at the Sega logo. Every Model 2 game will have an equivalent — find by symptom (hang on boot, COP self-test loop).
  - *In this project* the COP answers the self-test itself, so no profile carries a bypass at `0x74E4`. What STF hooks instead is the failure hang, `co_processor_error_hang` at `0x77F8` (`sfight_hook_cop_err_hang`, FV has its own): it halts the CPU and logs g4's error code so the UI stays live. The `locals.rip` return is `hle_ret` (`hle_hooks.h`), which hooks that skip a whole function (`xplay_sel_int_barrier`, 0xA218) still use.
- **No hook waits for a vblank or a timer: the board raises them on the i960's own cycle clock, and the game's handlers end its wait loops** (Pinboard #253, `irq_timer.h`). The vblank comes every 25,000,000 / 60 cycles (416,667) and a slice runs exactly to it; the timers always count cycles (`--live-timers` is accepted and does nothing). STF's `interrupt_wait` 0x1768, `interrupt_wait_b` 0x11580, the `_idle` wait hook at 0x11610 (a cycle-exact spin skip sits there now, below), `check_timer_4` 0x4A55C and `check_timer_4_spin` 0x4A58C are gone, and FV's 0x2238, 0x1184C/0x118DC and 0x4A88C with them: the Timer handler sets the flag at `0x50008C` itself, and VsyncScr (STF 0xC40, FV 0x13A0) runs as the vblank interrupt. Do not bring a wait hook back. It ran VsyncScr with `hle_call` at the game's convenience, ended the slice wherever the game's frame happened to end, and needed `EMU_FRAME_STEPS_MAX` to keep the sound board running through a load.
  - **The game's frame still ends at `variable_diff_calc`** (STF ~`0x11A04`, FV 0x11C80): the frame-pace hook marks the display-list capture there (`dl_game_frame_edge`) and makes the match_replay jump (`hle_game_frame_edge`), because MAME's `match-replay.lua` and every capture sample there. Made at the vblank, which falls mid-frame, the jump moved the replay fight off MAME at +127; at the hook it holds to +321, as before.
  - `EMU_STEPS_PER_SLICE` (500,000) is now only a cap: a slice cannot run more instructions than a vblank's 416,667 cycles. A lower `--steps-per-slice` ends slices short of the vblank and the board runs the same: `det_digest --steps-per-slice 150000` differs only in frame 1's RAM, where the warning skip, written once a slice, lands.
  - *Bugs this took with it:* `_idle`'s static survived a board reset, and FV's `read_sw` hook zeroed the last pad (0x500700) before every read, so every held button looked newly pressed.
- **Never hook `clip_point_check_yoko` / `clip_point_check`** (STF `0x28188` / `0x28250`, FV `0x236BC` / `0x23784`). They return nothing in `g0`. Each runs a point list through op `0x29` and writes one outcode byte per point to `0x50E000` (`0x90` behind the lens, `0x81`/`0x82` off the left/right edge). `area_clip` then draws a ground chunk only when the AND of its four corners' outcodes is zero, and finally draws the chunk the camera stands over.
  - *Symptom that surfaced this in STF:* stage ground chunks popped in and out as the fighters moved. The hooks skipped the write, and `0x50E000` is scratch that `rob_spd_control` fills with fighter X/Y/Z floats, so the cull was ANDing position bytes. Measured by `tools/grade-cull.mjs`: 0 of 299 frames right and 998 chunk toggles with the hooks, against 17 for the ROM's rule; exact in 599 of 599 without them.

- **STF's texture loader runs in C, one row at a time, and the board cannot tell** (`profiles/m2_texload.h`, Pinboard #178, #228). Hooks on the first instruction of each row's body (`unpack_lod_data` 0x4B9B4, `send_beta_data` 0x4BD30, `send_lod_data` 0x4BF64, `send_lod_data_q_sub_norm` / `_anim` 0x4C1F8 / 0x4C334) port that row instruction for instruction: every register and temporary, the condition code, every store in order from its own address, and the instruction count and cycles from the ROM's words. The run loop charges them as the i960's (`g_hle_room` / `g_hle_extra`, `hle_hooks.h`). The i960 runs everything else: the preamble, `check_timer_4_result`, the yield between rows and the resume. A row is left to the i960 when it would not end before the slice does, when the live timers would come due inside it or an interrupt is pending, when a watchpoint is armed, or when the code's FNV is not the known one.
  - **Do not go back to hooking the routines whole** (#151's first version). Texture RAM came out the same, but a page went in one call where the ROM yields on a timer-4 budget. The game's `rand` (0x66B0) mixes in the timers, so `random` (0x500098) moved from frame 556 of attract on, and with it the CPU fighter's choices. That needed a `NETPLAY_PROTO_REV` bump; the row hooks do not.
  - `det_digest --cpu` (texram, registers, cycles) against `--texload-i960` is byte-identical frame for frame over select, the VS screen and a fight, with the timers frozen and live alike (and again since they are always live, #253), on x86 and on the ARC-S's A55. 89-98% of a load frame's i960 instructions run as C rows, at ~1.4 ns each on x86 against ~6 interpreted, and ~8 ns on the A55; the ARC-S's VS-screen load frames went from 34-45 ms to 20-23 ms.

- **SKY EYE mode moves the camera by writing STF's camera record at `loc_1F3A0` in `camera_control`, not with the debug menu's `debug_flag` bit 5** (`core/sky_eye.h`, Pinboard #265). Bit 5 is what the menu's SKY EYE page sets to stop `camera_work`, but `control_init`, `collision`, `enemy_control`, `object_init`, `adv_movie_cont` (attract goes blank) and a dozen more skip their work on it too. `loc_1F3A0` is where the `camera_work` path and the bit-5 path meet, before `area_check` and the view read `fa_camera` +0x18..+0x28. A stage asked for has arrived only when the loaded record's texture words (`0x504800`+0x0C) match the ROM record's (`0x8F3D0` + n·256 + 0x0C); holding `stage_num` alone is not arriving.

- **`_idle`'s wait for the vblank is skipped on the cycle clock, in whole iterations, and the board cannot tell** (`profiles/m2_spin.h`, hook at `0x11610`, Pinboard #294). Since PR #163 the loop (`ldob 0x500000, r3` + `cmpibe r3, g0`) ran interpreted until the interrupt: two thirds of STF's i960 instructions. The hook decodes the loop from memory, and runs as many iterations as end before the timers' horizon (`g_irqt.horizon`), charging their instructions and cycles as the i960's; the i960 runs the one the horizon falls in. It declines with an interrupt pending, a watchpoint armed or under two instructions of room. `det_digest --cpu` against `--spin-i960` is identical over 4000 frames of attract and a 9000-frame scripted game; on the ARC-S the i960 thread fell 36-51% and the process 10-13%.

- **STF has two profiles on one ROM set, and the CONSOLE one is the default** (`sfight_console`, `profiles/sfight_console.h`; the arcade game is `sfight`). It adds the Honey / hidden-character traps Sega's console emulator DLL installs (its table at DLL RVA `0x1E8870`, named in YAMP's `StfHooks.inc`), ported handler by handler from Ghidra. One of them, `get_frame_dat+0x140`, zeroes the head-tilt term of the motion blend for EVERY fighter, so the console profile does not play attract's replay fight the way MAME does. Anything held against MAME or the explorer must run `--profile sfight`: `tools/lib/m2hle.mjs` passes it by default, and a new launcher has to as well.
  - **Both STF profiles power up as USA** (`g_region`, `--region japan|usa|export`): the hook at `0x62688` (`init_game_assignments`, the `stob` of `country_val`'s factory default) loads it on every boot whose backup RAM is blank (see "Backup RAM" below). A player's saved backup RAM keeps its own region, so there `--region` (and the free-play and DAMAGE defaults below) apply only after the test menu's INITIALIZE. MAME's sfight boots as Japan, so anything held against MAME must also pass `--region japan` — `tools/lib/m2hle.mjs` does by default. USA skips the Japan-only warning, adds the FBI picture to attract and uses the US names.
  - The console build's ROM images (YAMP's loose `rom/stf_rom/*.bin`) are byte-identical to the arcade set's code, data, EPROM and polygon ROMs; only the texture ROM's layout differs. Every console difference is in the DLL's traps, none in data. Honey's VS portrait (sprite `0x96`) is her silhouette over a "???" plate, and that is correct, not a missing asset.
  - **The Console profile boots on FREE PLAY**: `sfc_hook_free_play` at `0x62754` (`sram_clear_for_coin_assign`) stores 26 in CREDITS_REQUIRED (`0x1D03324` and its RAM copy `0x59C324`), where the factory default is 0. The ROM itself tests for 26 (`26:FREE PLAY`). The Arcade profile still boots on coins. Coins do nothing on free play, which is harmless for a script that inserts them anyway.
  - **DAMAGE** (`g_damage_real`, `--damage real|normal`) is the GAME ASSIGNMENTS flag byte's bit 7 (`0x59C353`), put in by `damage_default` at `0x62674` in `init_game_assignments`, like the region. The ROM's labels run the other way from what you might guess: the test menu's `DAMAGE_TYPE` is {NORMAL, REAL} by the bit, and `ketchup` (`0x19740`, called by `damage_calculation`; `ACT_RC_DOWN_ATTACK` too) scales a hit by the energy gap only when the bit is CLEAR. So **NORMAL (0, the factory default) is the catch-up damage and REAL turns it off.** Only a room on our server sets it (the owner's `damage_real` in the room state, NORMAL unless the host picks REAL, as the console plays); everything else boots NORMAL too, so the graders are untouched.
  - **VS mode** (`g_vs_mode`, `--vs-mode`, off by default) is the DLL's trap at `0xE584` (`next_round+0x1A4`), in `SFIGHT_BASE_HOOKS` for both profiles. A decided versus match jumps to `0xF524`, the ROM's own "both continue" path, so both players go back to character select. Off, the winner stays on against the CPU and the loser is out. The DLL's VS stage pick (`0xAF84`) draws from host RNG and is not ported, and neither is its `vs_match_count = 3`.

### Threading

- **The renderer reads the GEO state published with its list, never the live copy** (`g_geo_rs`, `geodl_raster_for`). Texture RAM, polygon RAM and the material slots are applied on the emu thread at each publish and copied beside that list's snapshot. STF lays each frame's replaced texture points (eyes, mouths) from `0x805000` on, a different set each frame, so a render still drawing list N from live RAM took list N+1's points.
- **The sound board runs on its own thread, and anything that touches it calls `sound_settle()` first** (`sound.h`, "The sound thread"). The frame's samples are handed over at the frame edge (`sound_advance`) and run while the next slice's i960 does. The i960's side (the UART callbacks, `sound_uart_make_room`), a reset, a capture and every bridge command wait for the run first, so the board does the same work in the same order: `det_digest` (thread on and off, and the single-threaded wasm build) and `ab-builds --sound` against master are identical, every sound sample included. Two things are new: the samples appear up to a frame later, so the frame clock is `g_sound.out_due`, not `out_total`, and a host that drains the ring on its own thread reads to `out_pub` (libretro); and a new reader of `g_sound` has to settle first or it reads a run half done. `M2HLE_SOUND_THREAD=0` (or the libretro option) turns it off.
  - *Measured* unthrottled on x86 (STF attract): 855 fps to ~1000. A third of frames still wait (`sound_wait_us` in get_status's "emu"): a slice that starts with a sound byte queued has to know TxRDY, which needs the 68000's clock to the cycle. A host with other work between slices (a draw, a pacing sleep) covers that.
- **Unlock the emu mutex BEFORE sleeping.** Sleeping inside the critical section freezes the UI.
- **Double-buffered CPU snapshot** (`cpu_snapshot` + `cpu_prev_snapshot`); UI always reads the current snapshot.
- **Sleep granularity**: POSIX `usleep()` ≈ 1 µs, but Windows `Sleep()` is **~15.6 ms**, not 1 ms,
  unless something in the process has asked for a finer timer. A window or an audio device
  usually has; a `--headless` run has not, and a 1 ms poll there really waits 15.6 ms (measured
  — see the A/V section below, where it cost a third of the frames). Since Windows 10 2004 another
  program raising the system timer no longer lends it to a process that did not ask, so the 60 Hz
  throttle (`emu_sleep_us`) waits on a per-thread `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` timer too,
  falling back to `Sleep` only where one cannot be made. *Measured with `Sleep`:* `Sleep(14)` took
  15.5 ms, the catch-up clamp threw the deadline away, and a headless stream ran at ~52 board fps
  with a gap every few frames.
- **The netplay pump runs outside the mutex** (`emu_netplay_pump`, `emu_thread.h`) and runs in the
  STOPPED branch too. A TLS connect blocks for seconds, so pumping it under the mutex freezes the
  UI; pumping it only while RUNNING means a player who connects before pressing Run never logs in.

### Raw A/V out (`--av-port`, host-side — `core/av_stream.h`, `ui/av_capture.h`)

One local client gets BGRA frames and 16-bit stereo samples on one socket, every packet stamped
with the board's own 44.1 kHz counter (`g_sound.out_total`, latched per game frame in
`g_frame_clock`). Four of these were measured, not reasoned out, and three of them fail quietly.

- **The audio has to be tapped at the producer, in `sound_out_push`.** The host ring below it
  has exactly one reader (`audio_out.h`), and a host with no audio device drains nothing — so a
  second reader of that ring only ever sees what the first already counted into `out_dropped`.
  `sound_set_tap` is that tap and `out_total` is the clock that goes with it; `sound_reset`
  leaves both alone, so a board reset does not move an A/V client's clock backwards.
- **A headless D3D11 device never Presents, so nothing submits the command buffer, and a
  staging `CopyResource` is still unfinished when the non-blocking `Map` reaches it two frames
  later.** `av__cap_issue` calls `ID3D11DeviceContext_Flush` after the copy for that reason
  (`Flush` does not wait). *Measured:* 30% of frames survived the map without it and 100% with
  it — 16 fps became 55. A windowed run gets the same flush free from Present, which is exactly
  why this does not show up until there is no window.
- **`Sleep(1)` is ~15.6 ms in a process that has neither a window nor an audio device**, because
  nothing in it has asked Windows for a finer timer. Polling the frame clock at 15.6 ms against
  a 16.7 ms frame caught 37 of 60. The headless loop uses a
  `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` waitable timer instead — a real millisecond, no
  `timeBeginPeriod`, nothing past kernel32.
- **Copies still in flight when a client goes must not reach the next one.** The first video
  packet of a session is what sets where the audio starts, so one leaked frame tagged with an
  old board frame drags the audio read head back by however long the emulator sat between
  clients — and the whole next session is then spent lapping through stale audio. `av_capture`
  drops its in-flight copies when `av_stream_session()` changes, and `av__serve` clamps the
  origin to what the ring still holds. *Symptom:* a second client got 0.6 fps and a lapped
  audio ring where the first had got 13.
- **The rings are allocated once and never freed** (`av_stream_shutdown` says so). The tap runs
  on the emu thread and clearing the function pointer does not retire a call already inside it.
  Same shape as the texram crash under "Memory Bus", same answer.
- **On Linux, stream with `--headless --av-port`, not from a window.** Capture mode is Win32-only,
  so `--kiosk` on Linux is an ordinary ImGui window, and on Xvfb every swap copies the whole back
  buffer out of the GPU into the X server after waiting for the GPU to finish. In the fly's
  docker container (Mesa d3d12 over WSL, the GPU shared with the stream's CUDA encoder) the frame
  callback took ~6 ms and the swap ~19 ms: the board ran at 60 and the tap got 33-39 frames, all
  counted as `dropped_missed` (a 16x16 window still paid ~10 ms). Headless is a surfaceless EGL
  context (`headless_gpu_init` in `main.c`, libEGL by `dlopen`): nothing is presented, no X server
  is needed, and the same bench held 60/60. `GALLIUM_DRIVER=d3d12` picks the GPU as it does for a
  window; with none, Mesa gives llvmpipe, where the fill shader is 98% of the process.
  *Symptom:* the stream's "board 17-26 fps in, ~700 gapped" while `get_status` said 60.

### Overlay plugin swap (`overlay_swap`, `ui/overlay_host.h`, host-side)

- **A swap to a different file shuts the old plugin down but never `FreeLibrary`s it** (`overlay_host__unload_ex(true)`). stf-fly's `feed_stop` waits 300 ms for its feed thread and then lets it go. After a swap the new copy of the same DLL is mapped at the old base, so the orphan runs the new copy's code with the old copy's stack cookie, and the CRT fast-fails (`c0000409`, in `flyoverlay!m2_overlay_query+…` on a `BaseThreadInitThunk` stack). That was the very first swap tried. `--overlay-reload` still frees, because it reloads the same path, and the loader would otherwise hand back the old module.
- **The standby card has to be delivered before the load stalls the render thread.** The A/V capture reads frames back a few frames late, and a thread blocked in `LoadLibrary` reads back nothing. With two frames of card before the load, the stream froze on the last game frame for the whole load (measured). The swap waits 8 frames and 0.15 s.

### Picture filters (`ui/post_shader.h`, `ui/retro_shader.h`, host-side)

The built-in CRT (YAMP's port of Lost Judgment's filter) goes through sokol with an HLSL and a GLSL source, so it runs on every backend. A libretro GLSL preset runs as raw GL between sokol passes (then `sg_reset_state_cache`), so only the GL builds have it; `tests/retro_shader_test.c` holds the parser and the rewrites below.

- **The game is drawn into the filter's own target, and a GL target keeps its bottom row first.** The CRT samples `(p.x, 1 - p.y)` on GL and `p` on D3D, and takes `abs()` of its derivatives, because GL's window y runs the other way. The libretro chain flips the source on the way into `Orig`, so v = 0 is the TOP of the picture in every pass, as RetroArch's GL driver has it, and only the last pass flips onto the screen.
- **WebGL rejects libretro shaders a phone's GLES driver accepts**, so `rs_gl_program` tries, in order: the shader as written as ES 3.00; every `lowp`/`mediump` made `highp` (a uniform declared at different precisions in the two stages will not link: zfast_crt_geo); a GLSL 1.10-to-ES 3.00 token rewrite (WebGL2 gives ES 1.00 shaders no `fwidth`: gizmo-crt); ES 1.00. Non-constant global initialisers (crt-hyllian, crt-royale) are moved into `main()` behind a macro defined where the declaration was, so an `#if` that is not taken leaves no assignment behind, and the compiler is asked again until it stops naming lines. The error reported is where the FIRST attempt ended, not the last fallback's: the last one's complaints are about the fallback.
- **What still cannot run on WebGL, measured against glsl-shaders' `crt/` (67 of 78 run):** real slang files with a `.glsl` name (crt-blurPi), `##` token pasting (ntsc-pass2-2phase), and desktop GLSL's implicit int-to-float conversions (the rest of crt-royale, mame_hlsl). Those need a type-aware compiler.
- **`PassPrevN` is the output N passes back, so `PassPrev1` is this pass's input and `PassPrev(p+1)` is `Orig`; `PrevN` is the history of `Orig` frames.** guest-dr-venom's green-and-magenta columns are its default mask, not a colour bug (pixels alternate (205,246,205) / (246,205,246)).

### Netplay / RPCN (`src/net/`, board-independent)

Matchmaking is [RPCN](https://github.com/RipleyTom/rpcn); the design follows `yampnet`
(a sibling checkout, `ai/yampnet`), which worked the protocol out first. These are the parts that
are **silently wrong** rather than loudly wrong when you get them half right.

- **A session is a COLD BOOT on both machines, not a savestate.** The barrier releases, both peers
  reset the board, and every frame from power-on is lockstepped. There are no savestates here, so
  this is the only state two copies are certain to share; it is also stronger than the PS3 port's
  shared RNG seed, because no window exists in which the two were allowed to differ. The reset has
  to clear the run loop's own latches too (`emu_board_reset_state`) — an interrupt left in service
  across it swallows the first interrupt of the new boot, which is a divergence on frame 1.
- **The board must read a COMPOSED input mask, not the keyboard's.** `input_state_t` carries
  `net_held` / `use_net` for that. Writing the composed mask over `g_input.held` looks equivalent
  and is not: key events land on the UI thread at arbitrary moments, so the composed value gets
  half-overwritten partway through an emulated frame on one machine and not the other.
- **np2_structs.proto types `uint8` and `uint16` as MESSAGES** (`{ uint32 value = 1 }`, kept from
  the flatbuffers port). A field declared `uint16 serverId = 1` is a length-delimited submessage
  containing a varint, not a bare varint. `flagAttr` and `roomId` *are* bare varints. Reading one
  as the other yields an empty message, not an error.
- **Three room fields are mandatory despite proto3**, all read with `get_verified()`: `teamId` on
  CreateRoom (field 16) and JoinRoom (field 6), and all three fields of `sigOptParam` (17). Omitting
  any is `Malformed`, not a default.
- **`sigOptParam` is what makes peers reachable at all.** Without it `need_signaling` is false in
  `room_manager.rs`, the join reply carries no `signaling_data`, and the host's `UserJoinedRoom`
  notification carries no address — so the host has nobody to punch towards and stays silent.
- **A room password is a FIXED 8 BYTES and needs `passwordSlotMask`.** The server takes the
  password only `if len == 8` and otherwise logs "Invalid password length" and leaves the room
  *open*; the mask (MSB-first, slot i = bit 63-i) is what actually gates the slots. Get either
  wrong and the room looks protected here and is public there.
- **SearchRoom needs `option` bit 0** or every row comes back with no owner name, and its range
  filter is **1-based and capped at 20**.
- **The world count is a u32 while the server count is a u16** (`cmd_server.rs`). Reading the world
  list as u16 shifts every entry two bytes and yields plausible-but-wrong world ids.
- **Every room REPLY is `[u32 LE length][protobuf]`** (`Client::add_data_packet`). Strip it, or the
  length parses as a tag and the message reads as empty.
- **Login's third string is the e-mail verification token, not a second password**, and the server
  compares it only when it has e-mail validation on (off by default). Empty is the normal value.
- **When two peers share a public IPv4, RPCN hands out the peer's LOCAL address with port 3658
  HARDCODED** (`room_manager.rs` and `cmd_misc.rs` both). Two clients on one machine are therefore
  told to punch at their own socket, so `rpcn_session_recv` drops datagrams whose source is our own
  `local_ip:local_port` — without it `peer_heard` latches onto our own port and every real datagram
  from the peer is then discarded as a stray.
- **A punch may come from an address the server never gave; a game packet may not** (issue #108,
  `rpcn_session_hear(..., punch)`). A player in a container advertises its bridge address (172.x)
  and its datagrams leave through the container host's NAT, so its LAN opponent hears it from an
  address nobody named and used to drop every datagram as a stray: both sat at the barrier. A
  punch names its sender, so it re-points a member not yet heard; once heard, nobody moves them.
  `--net-local-ip` / `$M2HLE_NET_LOCAL_IP` puts the container host's LAN address in the keepalive
  instead (`rpcn_client_t.advertised_ip`; `local_ip` stays the socket's own for the self filter).
  It is a process setting, not a `netplay_config_t` field, so the wholesale config copies (file,
  window, MCP) cannot drop it. `tests/net_test.c` part (G) holds both over loopback.
- **Behind Docker Desktop nobody outside the house can reach us at all, so a container relays
  through the web gateway** (`ws_relay.h`, `--net-relay` / `$M2HLE_NET_RELAY`, Pinboard #366).
  Docker Desktop's NAT gives the keepalive a random port that forwards nothing back (only the
  published 3658 does), and RPCN tells guests to punch that port: they punched forever. Now every
  datagram rides the gateway's `/gw/dgram` WebSocket (`[ip][port BE][payload]`, as the web build's
  do), so RPCN and guests see the gateway's public address. It is decided once at session start,
  never mid-room: a room keeps the address it saw at the join. `auto` is on in a container
  (`/.dockerenv`) for the servers `ws_relay_url_for` knows; a process setting, like `local_ip`.
  - The gateway refuses a WebSocket without an `Origin` it lists, so the relay sends the play
    site's. It answers the gateway's pings (30 s) or is dropped.
  - A lost relay drops the RPCN link too (`rpcn_recv_from`), so the heal signs back in and
    reopens it. A heal attempt must not fall back to direct (`relay_required`, all but the last
    try): the gateway shares the droplet with RPCN and came back a second after it in the test,
    and a direct sign-in then left the room unreachable for good.
  - **A guest outside joins a relayed owner through the gateway too** (Pinboard #382). The
    gateway's UDP port is not reachable from every network: a guest behind a carrier-grade NAT
    punched the fly at `143.198.49.181:40xxx` for the whole match and was never heard. So a client
    sending directly keeps the gateway in reserve (`relay_standby_url`), asks where a room's owner
    is before joining (`rpcn_session_probe`, RequestSignalingInfos), and when the server places
    the owner at the gateway's address it turns the relay on, waits for the helper to see the new
    address, and only then joins. Both members then hold the gateway's virtual addresses and it
    carries the match inside itself. Turning the relay on is before the room, so "never mid-room"
    still holds. `auto:ws://...` names the reserve gateway (the tests'); `off` keeps none.
- **A room copies each member's address when it is created or joined, and never refreshes it.**
  The address reaches RPCN only with the first UDP keepalive after login, so a Host or Join sent
  straight after sign-in snapshots nothing — for the life of the room — and two players on one
  public address are then told *different kinds* of address for each other and drop each other's
  datagrams as strays. So Host and Join wait for the signaling helper's reply
  (`netplay_room_or_defer`, at most `NETPLAY_ROOM_WAIT_MS`), and an address the peer has actually
  been heard from is never replaced by one the server reports later (`rpcn_session_set_peer_addr`).
  Found by `tools/web-netplay.mjs`, which hosts 0.2 s after signing in; a person rarely beats the
  keepalive, a script always does.
- **The build family rides in bits 6-7 of the room's revision byte, not bits 28-31** (those are
  the server's: it owns `ROOM_FLAG_ATTR_FULL` there). Lockstep needs bit-identical floats, so a
  lobby refuses a room of an incompatible family with a sentence; `NETPLAY_CROSS_PLAY` (on) makes
  native and wasm one. A web room's byte reads `0x41`, which desktop builds from before the field
  refuse as "a different netplay protocol", so only a desktop build from after cross-play can join one.
- **The wasm and MSVC builds compute the same frames only because of two things C leaves open.**
  Break either and web-vs-desktop matches desync; `tests/det_digest.c` (built in both trees) is the
  check, and it split at frame 2948 of attract on each.
  - *A NaN's sign and payload.* Every COP float that becomes a word goes through
    `sharc_float_to_bits`, which writes NaN as all ones, as the SHARC does. The i960's FP
    instructions go through `i960_nan_result` / `i960_single_to_double` / `i960_double_to_single`.
    Do not memcpy a float to a word in board code, or return a raw `a op b` from an FP instruction.
    LLVM moves negations across multiplies, which flips a NaN's sign, and `Fn_area_coli` branches
    on the sign bit of a NaN ball position.
  - *Strict aliasing.* GCC and Clang build with `-fno-strict-aliasing` (CMakeLists.txt), as MSVC
    always behaves. Without it the wasm build split from MSVC, and a trace compiled into the step
    loop hid the split. That is undefined behaviour at work; the offending access has not been found.
- **Connection trouble is repaired, not reported** (Pinboard #120), and nothing on the machine or the router is touched. Three repairs, each found or held on a local RPCN with two headless clients:
  - *A taken UDP 3658* steps to the next free port (`RPCN_P2P_PORT_TRIES`, `rpcn_session_start`). The server learns the port from the keepalive. Two players behind one public address are still told 3658 (the hardcode above), so an unheard member told 3658 on a LAN address is punched on the next ports too (`rpcn_session_pump_punch`). Two clients on one machine now link with no `p2p_port` at all.
  - *A member whose NAT made a new mapping* is followed once the old address has been silent for `RPCN_PEER_QUIET_MS` (3 s), by first contact's rules: a punch from anywhere, a game packet from the same address on another port (`rpcn_session_hear`). Before, "once heard, nobody moves them" dropped all their datagrams and the stall timer ended the match. `tests/net_test.c` part (G) holds both.
  - *A dropped link to RPCN* signs back in by itself (`netplay_heal_*`, 1 s doubling to 16 s, 8 tries, attempts not logged). Only a login that reached ONLINE, only when the TLS link is gone (`rpcn_is_connected`), never after a refused credential. A running match ends at once: the room calls it off the moment the server drops us. Then the player goes back to their room. If the server lost it (a restart closes every room), the old owner opens it again and the others search for a room owned by that npid for 30 s. A heal's join is `join_soft`: a missing room leaves the session ONLINE, not FAILED. Measured: 7 s from the server coming back to both players linked in a room.
- **Hole punching needs BOTH ends transmitting.** The guest's first packet opens a mapping through
  the *guest's* NAT only; a host that waits to hear something first never opens its own, and two
  peers on different networks sit at the barrier forever.
- **Twitch sign-in is a PASSWORD, not a parallel login path.** RPCN's device flow (commands 63/64,
  unauthenticated) runs once and returns a long-lived login token that the ordinary `Login` accepts
  in place of the password, so everything below `netplay.h` is unchanged by it. Three things bite:
  `TwitchAuthPending` (35) and `TwitchAuthSlowDown` (36) are *not* errors and must not end the flow -
  pending is the answer for most of its life, and slow-down means back off by another interval; the
  whole flow must stay on ONE connection, because the server relaxes that connection's
  unauthenticated read timeout from 10 s to 120 s only after the start succeeds; and an older RPCN
  does not know command 63 at all, so it answers `Malformed` and HANGS UP - a disconnect before the
  start reply means "no Twitch here", not "the network broke".
- **Signing in with Twitch is not the same thing as running the device flow.** The flow exists to GET a token; once one is stored, "with Twitch" is an ordinary `Login` with the token for a password (`netplay_twitch_reuse`). `NETPLAY_CMD_TWITCH_START` used to begin the flow unconditionally, so `{"cmd":"netplay_connect","twitch":1}`, `--net-twitch` and the window's button each sent somebody to twitch.tv on every launch with a good token in the file. Three things keep it that way:
  - The stored token and its owner belong to `g_netplay.cfg` and **survive the wholesale copy in `netplay_do_connect`**. A posted config is a copy that is stale in both directions: the window adopts the stored settings once, so Connect wiped a token the flow had just landed, and resurrected one that "Sign out" had just forgotten.
  - **`twitch_npid` is a guess until the server has answered.** The owner adopted at load is the npid stored beside the token, which is the last account that logged in, not necessarily the one that signed in with Twitch. A login that offered the token and was accepted stamps the owner (`netplay_mirror_stage`); a token refused under its own owner's name is dead and is forgotten. On the machine this was found on, the label named one account and the server accepted the token for another.
  - **At most two rejected logins, then a browser or a stop** (`netplay_twitch_refused`). Retrying a refused credential is how an account gets locked; `rpcn_session_t.credential_refused` is the fact to test, not the error text.
  - *Also true, and outside this repo:* YAMP signs in with Twitch too, and its `settings.ini` password for the same account was refused by the server while the emulator's token was accepted. Two clients sharing one account each hold a token, and they do not both stay good.
- **The settings file is per USER, not per copy of the exe** (`netplay_cfg_path`: `%APPDATA%\m2hle2\netplay.cfg`, `~/.config/m2hle2/netplay.cfg`; `--net-config` overrides). RPCN keeps **one Twitch token per account** (`set_twitch_login` overwrites the hash), so every device flow retires every other stored token. When the file lived in the working directory, each copy (a build tree, the canary download, the fly's stream kit) held its own token, and signing in from one killed the rest: the kit's token was refused, forgotten as dead, and the headless fly cannot approve a code. A legacy `m2hle_netplay.cfg` in the cwd is copied through once, when the per-user file does not exist yet. The save is temp-file + rename, because a stream and its training population share the file.
  - *Two accounts on one machine* (the two-client repro) now need `--net-config` per client. A separate cwd is no longer enough.
  - **YAMP shares the file too** (its `source/net/SharedLogin.cpp`): a Twitch sign-in there writes `twitch_token`/`twitch_npid` here, and its logins offer this file's token first. So the file can change under a running process, and two rules keep one process from putting a dead token back: `netplay_settings_save` gives way to a newer file token when this process has not changed its own since it last synced (`twitch_synced`, `netplay_twitch_merge_disk`), and a refused token first retries with a different token the file now holds (`netplay_twitch_refused`, still one retry). Forgetting it straight away would save an empty token over the one that works.
- **Never hand a server-supplied URL to `ShellExecute`.** The activation URI comes from the RPCN
  server, and `open` will run a local executable or a registered protocol handler just as happily as
  it opens a web page. `netplay_open_url` requires a literal `https://` prefix first, and is off
  entirely for a headless run.
- **A challenge is an announce for a round this end has not begun.** There is no ready message in
  the protocol; the barrier releases when both peers announce the same generation, which is what
  pressing Start does. `lockstep_on_peer_announce` drops every announce that is not the round we
  are already in -- and that is precisely the case a host needs to see, so `netplay.h` latches it
  separately (`peer_ready_gen`, `netplay_peer_ready`). It is a freshness window and not a flag:
  a peer at the barrier announces once per slice, so a challenger who walks away retracts their
  own challenge, where a sticky bool would leave one standing forever.
- **A peer's input record for this round is also its announce** (`lockstep_on_record`). A peer
  sends inputs only once its own barrier has released, and it stops announcing the moment it does.
  When the host releases on the guest's *first* announce, every announce the host sent before that
  reached a guest not yet in the round and was dropped, so without this the guest waits for an
  announce that never comes and the host stalls at frame 0. Over the internet one is usually still
  in flight; over loopback, and through the web gateway on one machine, the race was lost every time.
- **A machine that is WAITING has to keep talking.** Inputs go out once, when a new local frame
  is sampled, and the redundancy in a record rides on the *next* record. A stalled machine samples
  nothing, so when both peers are stalled nobody transmits and a burst of loss is permanent: with a
  delay of 2, five datagrams dropped one way. `netplay_resend_inputs` re-sends
  `[lockstep_resend_floor, last_local_frame]` every 50 ms while stalled. The newest record alone is
  not enough: the lost frame is `2*delay + 1` behind it, past one record's reach above a delay of 4
  (`tests/net_test.c` runs the deadlock at every delay). The same loop paces the barrier announce,
  which used to go out every millisecond -- a thousand datagrams a second at one address is what a
  consumer gateway's flood detection looks for.
- **Both inputs being in is permission to run a frame, not proof that it ran.** An emulator that is
  paused, halted, or never reaching the frame hook is cleared for the same frame on every slice,
  so it never counts as stalled: it shows "playing", sends nothing, and the only evidence is the
  *other* machine's stall timer blaming the network. `netplay_watch_own_board` reports it after 3 s
  and leaves at the stall timeout, the run loop adds whether it was a pause or a halt
  (`netplay_board_stopped`), and a machine in that state keeps re-sending so the peer's stall line
  ("the peer's last input arrived N ms ago") can tell a stopped board from a dead link.
  - *How it surfaced:* the first session with a player on another network. Their board stopped
    finishing frames 45 frames after the reset, twice, over a corrupted screen; the host logged a
    stall at frame 48 and neither side said which machine had stopped. **What stopped that board was
    still open when this was written** -- it does not reproduce here (`tools/grade-reset.mjs` is
    exact, and the CI build boots byte-identical to a local one), so the next report needs that
    machine's `m2hle.log`.
- **A scripted session must not write memory or halt the board.** Both are invisible locally and
  fatal jointly: `write_memory` changes one board and not the other, which is what the frame
  check exists to catch, and halting to think is a stall the peer sees -- m2-hle2 drops a session
  that stalls for fifteen seconds. Inputs need no special path, because `set_input` writes
  `g_input.held` and that is exactly what `netplay_sample_local` transmits.
- **One ComId per ROM set** (`com_id.h`, `M2H` namespace). RPCN partitions everything by it, so a
  single hardcoded id puts every Model 2 game in one room list where the mismatch is found by the
  netcode instead of the browser. Unlisted games get a deterministic base32 hash of the game key;
  `CreateMissing=true` registers a new id on first use, so no `servers.cfg` edit is needed.
- **Two servers, and the PS3-menu lobby (libretro core, web build) lets the player pick**
  (`NETPLAY_SERVER_OFFICIAL` np.rpcs3.net, the default there; `NETPLAY_SERVER_COMMUNITY`
  rpcn.sonicthefighte.rs, the default of the desktop window and the handheld's pad lobby, which
  has no picker). Accounts are per server. What bites:
  - **A Twitch token is a password and must only go to the server that issued it.** Once the
    server can change, the stored `server` no longer says where the token came from, so it is
    kept in `twitch_server` / `twitch_port` (a file without them adopts `server`), and
    `netplay_twitch_here` gates every use. Without it, `netplay_do_connect` offered our token to
    np.rpcs3.net as the password of an account with none stored.
  - **Twitch sign-in is ours only**: the lobbies switch the server to ours before
    `NETPLAY_CMD_TWITCH_START`. The official server has no Twitch.
  - **The official server's certificate is self-signed** (CN=RPCN, to 2030-07-21) and nothing
    pinned it, so a native build could not reach it at all. `NETPLAY_OFFICIAL_FINGERPRINT` is
    used when the player has no pin of their own (`netplay_pin_for`).
  - **The web build names the server in the gateway PATH** (`/gw/stream/np.rpcs3.net`; ours is
    the plain `/gw/stream`), never a query. The gateway from before the choice ignores a query
    and would relay an official-server login to ours; an unknown path is a 404 there. So the
    gateway must be deployed with its `upstreams` before a web build that offers the official
    server, and until then that choice fails cleanly.

### Rooms of more than two (`net/room.h`, after the PS3 port's Room Match)

Up to eight in a room: two fight, the rest wait in line and watch, and after every result the
winner goes to the front and keeps their side, the loser to the back (the PS3 port's
`np_session_build_fight_entries` / `rotate_queue_after_match`, reverse-engineered from
NPUB30927 -- room.h cites the addresses). The owner writes the room state to RPCN (room bin attr
0x57) and each member its own (member bin attr 0x59); nobody sends room state peer to peer, so a
new owner carries on from the server's copy. What bites:

- **Every match is a cold board reset on EVERY member**, fighters and watchers alike, and a
  lockstep generation of its own. The PS3 port never resets; this emulator has no savestates, so
  the reset is the only shared state. **The room's owner also decides the region** (room state,
  `g_region`): members on another region boot another game from frame 0.
- **Only the two fighters gate a frame.** Watchers (`LOCKSTEP_WATCHER`) run the fighters' two
  input streams, get a record every `NETPLAY_WATCH_STRIDE` frames (the web gateway caps a player
  at 240 datagrams/s), and ask a fighter to re-send with `LOCKSTEP_PACKET_REPAIR`.
- **A watcher paced to 60 Hz never makes up a frame it waited for.** It fell ~160 frames behind
  over one match and the next match cut it off short of the result. `netplay_catching_up` lets
  the run loop (native, web and libretro alike) run it unpaced while it holds frames ahead.
- **The result is read off the board**: an observe-only hook at STF 0xDC3C (`SFIGHT_BASE_HOOKS`,
  the instruction the PS3 port hooks too) sets `g_versus_result`, which the emu thread hands to
  `netplay_end_frame` at the frame boundary. A board reset clears it.
- **"Everyone ready" starts only the FIRST match.** Ready flags stay set, so honouring them after
  a result started the next match instantly. After that the countdown decides -- and only in a
  room of three or more (`netplay_room_rolls`); a two-seat room is the old one-on-one, where
  both press Start again.
- **`rpcn_poll` hands out a pointer INTO its receive buffer** and must not slide that buffer until
  the NEXT poll. It used to slide it at once, so a reply with a notification behind it in the
  same read was read as the notification's bytes -- every room join failed that way ("the room
  reply carried no room id").
- **RPCN tells members who share a public address that each other is on port 3658**, which is at
  most one of them. Two players never needed more than the first datagram to fix that; three
  never find each other. Members pass on where they hear the others from (`g_rpcn_intro_tag`
  introductions), punched beside the server's address.
- **VS mode can carry one cold boot across matches** (`room_vs_continues`). The owner's VS setting is in the room state (`vs_mode`, like `region`), and so is `session`: the match whose cold boot the boards are running. When a VS match is decided and the two fighters are the room's only players, the owner publishes `match + 1` with `session` unchanged. Each board moves `match_started` on by one when its own board reaches the result (`netplay_end_frame`), never when the room says so, because a watcher can be a whole match behind and would file the wrong result. With anyone waiting in line the owner closes the session instead, and a board that played on past the result stops there (`netplay_member_pump`). The lockstep generation is the session's and does not change across a rematch.
  - **A VS rematch asks first, over the game** (`PS3UI_SCR_AGAIN` in `ps3ui_app.h`; the handheld's `pad_lobby.h` does the same). Without it the boards go back to character select forever and nothing on screen lets a player out. `netplay_status_t.results` counts every match this board fought and saw decided. On each one a fighter gets the PS3's result window with Play again / Exit and a 10 s timer. Timing out means stay; Exit leaves the room, which ends the session on both boards (the owner calls it off), and the one left behind is told so. **It must not depend on the lobby having been opened**: a session joined from the web page's panel or RetroArch's autojoin never opens it, and the first version, which only asked from an open lobby, asked nobody there. The task opens itself for the prompt (`prompt_only`) and closes again, the Console shell lets it over any screen, and the desktop has `netplay_vs_again_overlay`. It is UI only: the board runs on underneath and gets none of the pad while asked, so no protocol change.
    - **Every match asks, not only VS mode** (Pinboard #8). The first version counted VS results only, and every lobby defaults to Arcade rules, and the official server's rooms (the web and libretro default) run the PS3 port's rules, which have no VS mode: the prompt never came up there, and the owner reported it gone. Outside VS the result ends the session, so the prompt has to stay up in `NETPLAY_IN_ROOM` (it closes only once we are out of the room), Play again is Start (a one-on-one room clears ready after a result), and the web shell runs the task whenever `netplay_in_room()`. With the lobby open, a non-VS result keeps the lobby's own result screen.
- **RPCN never announces a new owner** -- `leave_room` picks a successor silently -- so every
  departure is followed by a GetRoomDataInternal.
- A local RPCN (`RipleyTom\rpcn`'s built `rpcn.exe --cert-gen`, EmailUrl empty) and the MCP bridge
  run three clients on one machine; that is how every item above was found.

### The PS3 release's menus (`src/ui/ps3ui*.h`, libretro core + web build)

The libretro core and the web build present the Console version as the PS3 release (NPUB30927) does: title, MAIN MENU, Arcade / Offline Versus with the PS3's rule settings, SELECT = pause, Help & Options (the PS3's button presets), and the online lobby (the PS3's TaskSession over `net/netplay.h`). The RPCN lobby used to be RetroArch core options (`retro_lobby.h`, removed); it is now drawn by the core. `tools/ps3ui/README.md` has the pipeline and the facts behind it.

- **No Sega asset is in the repo, and none may be added.** Layouts are numbers generated from the player's own PS3 data (`tools/ps3ui/gen_layout.py`); every sprite is painted by code (`ps3ui_sprites.h`) and graded against the original (`tools/ps3ui/grade.py`); the fonts are OFL (`licenses/`). Do not commit decoded PNGs, JSON dumps or pixel tables.
- **Frontends draw through the GPU path only** (`ps3ui_gpu.h`, sokol_gl). The CPU path (`ps3ui_canvas_t` without a draw list) is 24 ms a frame at 720p and exists for `tests/ps3ui_render.c` and the grader. A recording canvas hands the GPU pointers into the glyph cache, so the cache (`PS3UI_MAX_GLYPH_CACHE`) must hold a whole frame's text.
- **Windows and timers run on the task's 60 Hz clock, not the draw's.** Open a window in the update (`ps3ui_app_windows`), never in a draw function: a frontend that skips a draw left every window stuck on its first, transparent frame.
- **The shell is the Console version's only** (`g_shell_on` = profile `sfight_console`). The Arcade version stays the board as shipped: coins on SELECT, no shell, its lobby at load and on L + R.
- **The shell holds the board still under its offline menus -- never while netplay runs** (`ps3ui_shell_board_paused`): the barrier's reset and the match need the board stepped.
- **Arcade / Versus settings go through `sfight_apply_menu_settings`** (`profiles/sfight.h`, ported from the PS3's `Settings_ApplyArcade` / `Settings_ApplyRoomRules`): the work copy at 0x59C340 is what the game reads; time and barriers are read only at boot, so they are also written to `time` (0x500090) and 0x50A424; the block's CRC-16 at 0x1D03302 is recomputed. `sfight_settings_test` (a ctest) holds it.
- **Player Match's rules are room rules, and a community room publishes them as value + 1** (Pinboard #333; docs/ROOM-MATCH.md Part 3 §2). Custom Match is the PS3's search filter, Create Match its room. RPCN hands every room all eight int attrs, 0 when unset, so a raw index 0 would make every old room match "2 rounds, 10 s, Type A"; 0 is "unset" and lists as the factory rules. The rules reach the board through the cold boot's `init_game_assignments` hooks (`rounds_default` 0x624F8, `damage_default` 0x62674 for the game-type bits, `xplay_game_time`), so a netplay match never writes the settings block by hand.
- **A test core must not sign in as a live account.** The libretro core copies the desktop's per-user netplay settings on first run; on the dev machine that file is the fly bot's (saltyfreeman) Twitch login, and a test RetroArch signed in as it (it failed only because UDP 3658 was taken). Test with a blank `m2hle-rpcn.cfg` in the RetroArch saves folder.

---

### Cross-play with the PS3 port (`net/ps3_link.h`, `rudp.h`, `rpcs3_signal.h`)

PS3 Sonic the Fighters (NPUB30927) is Sega's own i960 arcade emulator in a PPU wrapper, and it plays
online over RPCN inside RPCS3. On the official server (np.rpcs3.net) m2hle plays its rooms by its
rules; the community server keeps m2hle's own protocol. Every rule below was read out of the PS3
EBOOT (Ghidra) and checked against a PS3-vs-PS3 match captured through RPCS3's `sys_net_dump` log.
`tools/ps3-audit.py` holds an RPCS3 log against m2hle's `--net-ps3-wire` log.

- **The layers are RPCS3's, not Sony's.** RPCS3 signaling (75-byte `SIGN` v3 packets on vport 0,
  subset 1) is what gives the PS3 game its "Established"; then Sony RUDP runs LLE (librudp.sprx) under
  RPCS3's 6-byte P2P header (vport 1). Signaling and game traffic must share one socket.
- **RPCS3 marks us connected only when it gets a CONNECT_ACK for ITS CONNECT**, and its RTT narrows to
  u32 and throws on a garbage echo: echo `timestamp_sender` in CONNECT_ACK/PONG and
  `timestamp_receiver` in CONFIRM, exactly.
- **Every RUDP connection is a simultaneous open on three channels** (mux vport 1/2/3). Channel 1 is
  reliable but UNORDERED (syn_flags `0x0201`), 2 and 3 unreliable (`0`); a PS3 resets a SYN whose
  flags differ. There is no FIN: a PS3 closes with RST reason 0.
- **Use librudp's retransmit clock (1 s, doubling to 16 s).** A PS3 delays and bundles its ACKs; a
  250 ms clock resent 24 messages a match that had already arrived. A PS3's own "resend" 30 ms after a
  segment, with options set, is it piggybacking an ACK on its outstanding segment -- harmless.
- **The lockstep is the PS3's TaskSyncIo, ported state for state** (`ps3_sio_*`). Frame n plays ring
  entry n of both sides; the first frame played is the room's delay, the same on both machines because
  it comes from the round trip the owner writes into the room. It ticks at 60 Hz of WALL CLOCK: compare
  `now > next_tick_us + 100 ms`, never subtract -- the subtraction underflowed, ticked on every pump,
  sampled at twice real time, and the PS3 abandoned every match 25 s in.
- **A PS3 drops a SyncStart that arrives before its lockstep task is running**, yet its network layer
  still answers it. Our board reaches its forced START sooner, so our SyncStart went unheard until the
  5 s resend. Send ours again when theirs arrives (`ss_echoed`).
- **When our board falls more than `delay` frames behind its own sampling, the PS3's rule stops
  sampling until it catches up** -- which at 60 Hz pacing it never does. Run unpaced while behind
  (`ps3_link_hurry`, through `netplay_catching_up`).
- **The match's settings are the room's.** At match start the PS3 writes the room rules through its
  table at EBOOT 0x377AB0 into the game assignments (0x59C340 / 0x1D03340): rounds, energy 1, time,
  the flag byte (AUTOMATIC always on; game type A-D sets HYPER MODE and BARRIER RESET), barrier 5.
  Stage = seed % 9 (0xAF84). A board that is not in attract is rebooted first (FUN_000ac554).
- **m2hle can own a PS3 room too** (`ps3_owner_pump`, ported from `np_session_update_room_phase` 0xBF258 and held against an RPCS3 log of a PS3 owner). Three things are not obvious:
  - **Step only on the echo.** The PS3 owner advances only when the room's phase, as the server sent it back, equals its own; RPCN echoes SetRoomDataInternal to the writer too. One write in flight at a time, or phases reach members out of order.
  - **Room flags travel with the phase**: phase 1 closes and hides the room (flagFilter/flagAttr `0x50000000`), phase 4 reopens it (filter `0x50000000`, attr 0).
  - **Room byte 5 changes the PS3's lockstep** (`SyncIo_Init_rings` 0x6E67C): `0x40` (more members than fighters) means a small packet every 2 frames, a 60-frame packet every 12 straight to the watchers, one frame more delay, and the match ends at VIC_DSP when CTRL_TIMER is 60 (hook 0xE93C), not at VIC_INT. `0x80` (a fighter has no link) sends inputs through the owner with header flag `0x100`. A member that ignores these plays a different first frame from the PS3.
- **The peer-to-peer socket is bound before the TLS connect** (a taken port fails at once), so the session takes its own `net_startup` reference first and holds it until `rpcn_session_stop`. Without it, a sign-in with no MCP bridge or A/V stream up failed with `could not bind UDP (10093)` (WSANOTINITIALISED) on Windows: every test ran with the bridge, which had started Winsock for it.
- **RPCS3's RPCN password is a derived key**, not what the player typed: PBKDF2/SHA3-256 as 64 hex characters (`rpcn_settings_dialog.cpp` `derive_password`). `netplay_config_t.password` must hold 64 characters; at `char[64]` it lost the last one and the server said "wrong password".
- **The two boards still compute different fights** from the same inputs: the PS3's COP is
  single-precision C with FMA, its `rand` an MT19937, and it has no sound CPU. So its result can come
  before ours: the member's post-match update (flags 0xE0, place in line) has to follow our own result
  whichever order they arrive in.

## STF Disassembly Reference

Authoritative IDA disassembly: `C:\m2\ida72\asm-check\`.

| File | Description |
|------|-------------|
| `stf_prog.asm` | Full ~477K-line IDA disassembly — primary reference for STF ROM addresses, struct layouts, function names |
| `decomp/` | ~1057 per-function `.S` files named after game subsystems |
| `process-win.bat` | ROM rebuild chain: `m2asm.py` → `gcc960` → `objcopy` (intel960) → `stfbin2rom.py` → `epr-19001.15` / `epr-19002.16` |

**ROM identity:** CRC32 `72E66A1D`, MD5 `2A3E32834FC727391C0AFCB18121245E`.
**`stfbin2rom.py --ctools`** strips the 44-byte (`0x2C`) b.out header `gcc960` prepends.

For other Model 2 games, see **MAME** (`src/mame/sega/model2.cpp`) as a cross-reference for memory map, COP opcodes, and ROM region layouts. Don't copy code; do cross-check addresses.

---

## Build

`cmake` is on `PATH` (`C:\Program Files\CMake\bin\cmake`). The toolchain is **Visual Studio 2026 (VS 18)**, and `build\` is its tree: the graders launch it, and `build.ps1` and `run_tests.ps1` hard-code it. `build_vs22` is a VS 2022 tree from before the switch; do not build or stream from it. A tree configured for a generator that is not installed fails with "could not find specified instance of Visual Studio" — configure a fresh directory rather than reusing it.

Everything under `vendor/` is a **git submodule pinned to an exact upstream commit** — `imgui`, `dear_bindings`, `sokol`, `miniz`, `ImGuiFileDialog`, `imgui_club` (its `imgui_memory_editor` is the hex grid both memory viewers are built on), `stb` (stb_truetype, for the PS3-menu fonts; every frontend needs it), and `noclip` (the last only feeds `tools/`). A tree cloned without them fails configure with the `git submodule update --init …` line to run.

The cimgui C bindings are **not committed**: CMake runs `vendor/dear_bindings/dear_bindings.py` over `vendor/imgui/imgui.h` into `<build>/cimgui-gen/` at build time, with `--replace-prefix ImGui_=ig` to keep the `ig*` spelling that `src/` and `sokol_imgui.h`'s "original cimgui" path expect. That needs Python 3 with `ply` (`python -m pip install ply==3.11`); configure fails with the exact install line if the interpreter CMake picks up cannot import it. Do **not** swap this for the `cimgui/cimgui` repo — that is a different generator, and it produced an `ImGuiIO` ABI mismatch here (`MousePos` updated, `MouseDown` stuck at 0).

```
cmake -S <repo> -B <repo>/build -G "Visual Studio 18 2026" -A x64
cmake --build <repo>/build --config Release --target ALL_BUILD -j 16
```

The other frontends are `-DM2HLE_FRONTEND=sdl3` (the handheld, below), `web` (`emcmake cmake -S . -B build_web -DM2HLE_FRONTEND=web`; see docs/WEB-PORT.md) and `libretro` (a RetroArch core; `-DM2HLE_LIBRETRO_GLES=ON` for GLES 3; see packaging/libretro/).

Output: `build\Release\m2hle.exe`. The unit tests in `tests/` build alongside it (`M2HLE_BUILD_TESTS`, on by default) and run under `ctest` or `run_tests.ps1`; CI (`.github/workflows/canary.yml`) runs every ROM-free one — `mem_test`, `i960_test`, `cop_test`, `m68k_test`, `emu_test`, `net_test`, `ps3net_test`, `tile_test`, `heat_test`, `audio_out_test`, `scsp_dsp_test`, `scsp_dsp_test_masks`, `scsp_lazy_test`, `retro_shader_test`, `sfight_settings_test` — since `rom_test`, `boot_test`, `geo_test` and `input_test` load a ROM (`$ROMDIR`, else `$ROMS_DIR`, else the owner's Windows path; `tests/test_rom_dir.h`). `cop_replay`, `snd_replay`, `snd_bench`, `det_digest`, `ps3ui_render` and the fuzzers `i960_fuzz`, `m68k_fuzz` and `scsp_fuzz` are built but are not ctests: capture replays, benches, a renderer for the graders and fuzz drivers. Everything beyond that is graded by `tools/` or checked interactively. `--headless --mcp --rom <zip> --run` runs the emulator and its bridge with no window, GPU or audio device; the graders launch it that way (`$M2_WINDOW=1` shows the window). Nothing loads without `--rom <zip>` (or File → Load ROMs in the window); `schamp.zip` is found beside `sfight.zip`. **In the dev container every stock set is in `$ROMS_DIR` (`~/build/mameroms`): use it in place, never copy a zip into a scratch or build directory, and never run the emulator from the ROM's folder** (its log lands there, and a loose `sfight/epr-1900x` in the working directory replaces the zip's program; tools/README.md). The profile is picked by the set's name, and `--profile` or the Profile menu chooses among profiles for the same set.

**The handheld build** (`-DM2HLE_FRONTEND=sdl3`) is the same board with a fullscreen SDL3/GLES 3 host and no ImGui — it runs on the Anbernic RG ARC-S (RK3566, ROCKNIX) at 58-60 game fps. CI cross-compiles it on every push to master and attaches `m2hle-rocknix-arm64.zip` to the `canary` release; [packaging/rocknix/](packaging/rocknix/) holds the launcher EmulationStation calls, the installer, the toolchain file and the notes. Two things there are load-bearing: the container is **debian:trixie**, the one distribution with `libsdl3-dev` for arm64 and ROCKNIX's own glibc 2.41, so the binary carries no libraries of its own; and `CMAKE_TOOLCHAIN_FILE` must be **absolute**, since a relative one is looked for from the build directory and CMake then quietly configures for the host. Netplay's TLS on Linux is the system OpenSSL opened with `dlopen` (`net/tls.h`), not linked, so it does not add a fifth library; do not "fix" that with `-lssl`.

**Grading harness** — [tools/](tools/) measures this emulator against an independent implementation of the same ROM formats (the STF explorer, a submodule at `vendor/noclip`), with SHA-256 over a MAME capture as a third point so the two ports cannot simply agree with each other and be wrong together. `node tools/grade-models.mjs` is the one to run after touching `geo3d.h`, and `node tools/grade-pose.mjs` after touching the COP bone handlers in `sharc_exec.h` — the latter replays 328 frames of rig arguments captured off a real board, so it needs a sibling `stf-tools` checkout for `motion-pose.csv` and skips cleanly without one. `node tools/grade-cull.mjs` checks which arena ground chunks get drawn against the ROM's own `area_clip` rule, on the camera the display list was drawn from. `node tools/grade-stages.mjs` plays a round on each of the fifteen stages (picked at ROUND_INIT, `0xAFC8`). It checks every arena part on the stage object clocks, the moving stages' flights, the matrices the COP lays into the display list and the texture scrolls, all against the explorer. Run it after touching the COP matrix handlers or anything a stage routine calls. `node tools/grade-zsort.mjs --stage 1` / `--stage 5` holds the 3D pictures against MAME's where faces lie on faces, on the same replay; run it after touching the z-sort in `geo3d.h` or the fill shader's depth. `node tools/match-replay.mjs` plays attract's preprogrammed Sonic vs Bean replay fight (`--match-replay` skips the intro movie) and holds both fighters frame by frame against a MAME reference taken with `--mame`. The fight is an input replay, so any divergence is a simulation bug; run it after touching the i960 core or any COP handler the fight uses. `node tools/grade-reset.mjs` needs no oracle at all: it boots, plays into attract, performs the netplay barrier's reset with no session (`board_reset` over the bridge) and holds the boot that follows against the first one, byte for byte, twice. Run it after adding any state a board reset has to clear -- a static in a hook, a latch in the run loop, a region in `mem_init`. `node tools/ab-builds.mjs` holds two *builds* to the same board state (an optimisation's proof), `node tools/bench-builds.mjs` times them headless, and `node tools/bench-render.mjs` times the renderer's stages on the real D3D11 device; `tile_test` (a ctest) holds the tile compositor to its pixel-by-pixel original. See [tools/README.md](tools/README.md).

---

## Conventions

- All modules except the entry points (`main.c`, `main_sdl.c`, `main_web.c`, `main_libretro.c`), the sokol implementation units (`sokol_*impl.c`, `sokol_impl.m`) and the vendored `miniz.c` are **header-only `.h` files**. This is intentional — do not split into `.c`/`.h` pairs.
  - The one deliberate exception is `src/ui/mem_edit.cpp`, the single C++ translation unit: `vendor/imgui_club`'s `MemoryEditor` is a C++ struct against the ImGui C++ API, and C11 sources cannot include it. It hands out the C handle declared in `mem_edit.h`; keep C++ from spreading past that file.
- Default new code to the **board layer**; only move to a `game_profile_t` quirk when there's positive evidence of game-specific behaviour.
- **Desktop-only debugger hooks in the board go behind `M2HLE_DEV_TOOLS`** (`core/build_features.h`): 1 for the desktop and the tests, 0 for sdl3, libretro and web (CMakeLists.txt). Test an "is it armed" helper (`bp_armed`, `wp_armed`, `dl_active`, `sndcap_on`), never the raw field, so the check is a constant false where nothing can arm it. Keep the state and functions defined either way: det_digest is built in the web tree. Platform code is gated where it is used by the compiler's macros (`_WIN32`, `__linux__`, `__EMSCRIPTEN__`).
- Memory addresses and sizes use `uint32_t`. Sign-extension is handled per-instruction.
- Platform threading is abstracted in `thread_mutex.h` (pulled in by `emu_thread.h`): `emu_mutex_lock()` / `emu_mutex_unlock()` wrap `CRITICAL_SECTION` on Windows and `pthread_mutex_t` on POSIX.
- Logging: `log_msg(severity, fmt, ...)` from `log.h` (levels `LOG_LVL_*`), or the `LOG_INFO` / `LOG_WARN` / `LOG_ERROR` macros that wrap it. **Log unknown COP commands and unhandled MMIO at WARN** so new-game support work surfaces them automatically.
