# CLAUDE.md

Seeds a fresh Claude session with the hard-won facts that took trial-and-error to discover — things that cannot be re-derived from the i960 manual or general emulator-design knowledge.

Pair with [PROPOSAL.md](PROPOSAL.md) §8 for the full subsystem gotcha catalogue and §3 for the board-vs-game layering. This file is for the *invariants that must never be re-derived*; the proposal is for the broader context.

---

## Project Overview

A general **Sega Model 2 arcade emulator**, written in C11 with Dear ImGui (via cimgui) and Sokol for cross-platform graphics. The first target is *Sonic The Fighters* (STF), because the bulk of the prior reverse-engineering work happened there — but the architecture is built for the **full Model 2 catalogue** from day one. Generalising across games strengthens every subsystem: most "STF bugs" turn out to be board-level i960 / COP / tile bugs that affect every Model 2 game equally.

**Reference prior project** — the original STF-only implementation lives at `c:\Users\bigge\source\repos\stf-hle\` (the directory containing this file). It is **not deleted**: treat it as a working reference for register-window logic, COP math, the polygon decoder, HLE hook patterns, and the memory-region table. Read freely from it; do not import code wholesale — the new project's layering (board vs game profile, §3 below) means files will need restructuring as they're brought over.

See [PROPOSAL.md](PROPOSAL.md) for the architecture, module map, build commands, and bootstrap checklist.

---

## Board vs. Game Layering (critical)

Code lives in one of two layers. Get this distinction wrong and you'll re-implement board-level fixes in per-game files.

- **Board layer** — anything shared by every Model 2 ROM set: i960 CPU core, memory bus, COP math, tile renderer, 3D polygon decoder, sound block, threading. Lives in shared `.h` modules.
- **Game-profile layer** — anything specific to one ROM set: HLE hook table (addresses), input map, ROM file list + CRC32s, optional quirks struct. Lives in a `game_profile_t` entry resolved from the loaded ROM CRC32s.

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
- **`call` / `ret` frame layout**: align SP to 64 bytes (`(sp + 63) & ~63`), zero the new locals, save `pfp` / `sp` / `rip`, sync `g15` (frame pointer) every call. `ret` restores all locals.
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
- **`0x07000E0E` is a no-op in the HLE**: firmware PM 0x020433 writes 3 FIFO args to bone slot rotation entries `slot[1..3]`, but in STF the args are i960 addresses and the subsequent identity reset and ang commands overwrite those entries. The HLE ignores the command's args.
- **`0x1A803535` mirrors to `tgp_bone`**: the geo3d scanner reads `tgp_bone[player*16+slot]` when it encounters `0x1B803737` in the capture stream. For attract-mode characters (which use `calc_unit_mat` + `0x1A803535`, not the IK chain `0x35806B6B`), `tgp_bone` must be kept in sync with `rot_cache` by copying on every `0x1A803535` save.
- **`0x33806767` (op 0x67) also mirrors to `tgp_bone`.** It stores the current matrix into the TGP slot its one argument names — the same 0x3A00 (P1) / 0x3B00 (P2) window, 0x0C a slot, per firmware PM 0x020597 and `stf-tools/dl-rig.mjs` — so a store in that window has to reach `tgp_bone[]` and not only SHARC DM, or the slot can be drawn stale. `0x3D00`, the kage matrix, comes through the same op and is correctly outside the window.
  - *Measured, and worth knowing before chasing it:* in attract this is currently **inert**. Op 0x67 only ever stores slots 0, 1, 16 and 17 (the two waists and the two chests), and `0x1A803535` — which writes all 32 — was the last writer before the draw in every observed case (60 of 64 selects, the other 4 falling off the front of a truncated stream dump). So the mirror closes a gap rather than fixing a visible symptom, and it will matter wherever 0x67 *is* the last writer. The two ops do not agree: every 0x67 store differed from what 0x35 had left in the slot, by up to 4.5 world units, so if the ordering ever changes this is not a cosmetic difference.
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
  - *Symptom that surfaced this in STF:* the old handler only echoed type words, 13 against the board's 61, so every sway segment (Bean's feathers, Honey's pigtails, Fang's tail) took its matrix off an empty FIFO. The explorer (`vendor/noclip` TECHNICAL.md, "Sway chains") shows the i960's own integrator is switched off. That is a fact about the i960; the coprocessor still carries each point from frame to frame. `cop_replay` now matches every `Fn_osage` word exactly.
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

**Face loop:** `i < n_idx - 8`, always 2 groups behind tail.
**Face type:** `f1 == 2` → triangle; otherwise quad with **A-B-D-C** winding.
**Vertex convention:** `(x, y, -z)` — Z is negated on read.

- **The UV stream runs B,A,C,D for a quad and B,A,C for a triangle — no flips.** Negating Z reverses the winding, so the stream walks each face's loop the other way from the index array. Corners shared along a strip carry one UV in ROM, so the right order is the one that agrees with itself: 96.5% of shared corners for this order, 75.5% for the old A,B,D,C-with-flip-U-and-V, which had been picked by eye and left 38% of corners on a different texel (STF's "CAUTION" sign drew upside down and mirrored). `tools/grade-models.mjs` now holds all 1,795,005 textured corners to the explorer's exactly.
- **A quad whose four corners were already emitted in this model is cut along the same diagonal as before.** A decal is the surface's own faces emitted again with a cut-out texture; if the copy is cut along the other diagonal, a warped quad bulges differently and half the decal sinks behind the surface. The corner key is the explorer's `cornerKey`, bit for bit. This rule, and nothing in the connectivity logic, was the whole of the J = 0.990 that `grade-models.mjs` first measured. It is now J = 1.000000 over 598,728 triangles.
- **The fill follows `model2rd.ipp`, through the explorer's port of it** (`game_render.h` fill shader against `vendor/noclip/js/viewer.js`). Texture-header bits it depends on:
  - bit 13 on a textured face: the transparent renderer, where texel 15 is a hole (four-tap coverage ≥ 0.5 survives)
  - bit 15: checker, drawn on every other pixel
  - bits 8 / 9: mirror X / Y, where an odd repeat of the tile reads back to front

  Sampling is bilinear with the half-texel offset. The lumaram band is indexed with the *filtered* texel (`lumabase + t*120`), not the nearest of 16. Mip level L sits at `((tx-2048)>>L)&2047, ((ty-1024)>>L)&1023`, on alternating sheets. `grade-models.mjs` checks the face flags against the explorer.

**Model-table mesh pointer is encoded, not a raw offset**: actual ROM offset = `(ptr * 4) - 0x02000010 + 0x10` for STF. The `* 4` is hardware; the base may shift per game — verify before trusting on a new ROMset.

**Connectivity bruteforce:** simple rule `(vp[25] & 3) != 2` gives J≈0.708; the empirically correct `connect_when` mask for STF is **`0x45B4`**, found by brute force against the STF reference OBJs. Do NOT replace with a fan-mode or bitmask heuristic (that approach peaked at J≈0.71 and was abandoned). For a new game, re-run the bruteforce against that game's reference renderings before assuming the same mask.

### Memory Bus (board-level)

- **Region table is linear-scanned in declaration order.** TILE (`0x01000000`) MUST appear before H_SYNC (`0x01040000`) or H_SYNC reads route to the TILE handler.
- **IO region initializes to `0xFF`, not `0x00`** (hardware idle state).
- **Tile RAM is 64K and mirrors at `0x01010000`** (MAME `mirror(0x110000)`); the `TILE_MIRROR` region shares TILE's buffer and must precede TILE in the table.
  - *Symptom that surfaced this in STF:* the NEXT MATCH screen had no KNUCKLES nameplate. `rm_char_disp_int` shifts long names one tile left with a table offset of `0xFFFE` loaded by `ldos` (zero-extended), so the plate is written at `0x01011442` and only reaches tile RAM through the mirror. Metal Sonic's plate uses the same offset.
- **`GEO_CAPTURE_SIZE` ≥ 32768.** Smaller sizes wrap mid-frame and produce partial 3D snapshots / flicker.

### Tile Renderer (board-level)

- **16-bit byteswap on pixel bytes**: indices `[0,1,2,3]` are read as `[1,0,3,2]` (XOR low bit of byte index). Within each swapped word, high nibble = left pixel, low nibble = right.
- **Tilemap entry (7-bit fields)**: bit15=priority, bit14=h_flip, bits[13:7]=pal_bank (7-bit, 0–127), bits[6:0]=char (7-bit). Full tile index = `entry & 0x3FFF` (= `(pal_bank<<7)|char`). Palette LUT index = `pal_bank * 16 + color_idx` (stride=16 entries = 32 bytes per bank). Verified: CG87 palette written to pal+0x660 = bank 51×32; tile entry pal_bank=(0x9980>>7)&0x7F=51; pal+51×32=0x660 ✓.
- **Color index 0 is transparent on foreground layers only**; background layers fully opaque (pass `NULL` for `alpha_out`).
- **Four tilemaps, each with its own scroll, and a window mask per pair** (MAME `segaic24` draw_common, `model2_v.cpp` screen_update). Tilemap t sits at tile RAM word `0x1000*t`, H scroll `0x5000+t`, V scroll `0x5004+t` (bit 15 disables), and samples at `(x − hscroll, y + vscroll)`. Pairs 0/1 and 2/3 share a control word (`0x5004` / `0x5006`, bits 14:13) and a mask (`0x6000` / `0x6800`, four words a line, one bit per 8 px):
  - control 0: the even tilemap draws where the mask bit is 0, the odd one where it is 1;
  - control 1: split at line `−vscroll`;
  - control 2/3: split at column `hscroll`.

  Behind the 3D go tilemaps 3 and 2 opaque, then 1 and 0 with tile bit 15 clear. In front go 3, 2, 1 and 0 with bit 15 set.
  - *Symptom that surfaced this in STF:* NEXT MATCH draws each fighter's art as a top half in tilemap 2 and a bottom half in tilemap 3, stitched by a control-1 split. The renderer drew only the even tilemap of each pair, so both fighters were cut off at mid-screen. It also showed leftover "WAITING FOR CHALLENGER" tiles that the split hides.

### Sound board (board-level — `sound.h`, `scsp.h`, `m68k_exec.h`)

The 68000 runs the game's own sound driver (per-game code: three Hiro driver versions across the catalogue), and the SCSP is emulated at its register interface, **one sample at a time in lockstep with the 68000** — 256 clock periods per 44.1 kHz sample. The host audio callback (`core/audio_out.h`) only drains a ring.

- **Do not go back to a key-on event mixer on the audio thread.** The driver reads the chip back and acts on it: the slot monitor's play position CA (0x408/0x409, MSLC-selected) paces its streaming of long samples 8 KB at a time and decides which voice to take back; KYONB clears itself when a voice ends; SCIPD, the timers and the MIDI input buffer are its clock and command channel; the DSP's delay line is in sound RAM. With the old mixer the driver kept 25–32 voices keyed where MAME holds 5–16, and the voice allocation split from MAME 2.5 s into the music.
- **The 68000 sees all 8 MB of sample ROM**: 0x800000 (first 2 MB), 0xA00000 bank 4 (+2 MB), 0xE00000 bank 5 (+6 MB); the sound-control register at 0x400000 only re-banks sets larger than 8 MB. STF keeps 458 of its 602 samples above 0xA00000 — the old 2 MB window copied silence for three quarters of the instruments.
- **i960 bytes go straight into the SCSP's MIDI buffer** (MAME `model2_serial_w`); the UART status at 0x9C0004 reads transmitter-ready. A whole command arrives together on the board, so `emu_service_sound_again` re-runs the sound handler within a slice while the i960's queue has bytes (one byte per frame put commands ~50 ms late).
- **Timing is what makes the music keep time, and it is measured, not assumed:**
  - 68000 instruction time comes from Motorola's tables (`m68k_timing.h`) plus the run-dependent parts (branch taken, DBcc, shift count, MOVEM registers, MULx bits). A bus-access count ran 0.2% fast.
  - SCSP timers count from the write, in clock periods, not whole samples: the driver reloads them inside its interrupt handler, and that delay is part of every period (MAME: 49.884 samples for a 49-sample timer B). Rounding to samples ran 2% fast.
  - The 68000 compares its interrupt lines with the mask `SOUND_IPL_LEAD` (10) periods before an instruction ends, and a mask lowered by the instruction itself (RTE) is only acted on after the next one. Fitted to timer B's period to 0.003%; timer A is still 0.01% short (505.25 vs 505.30 samples), which is what decides the first slot race.
- **MAME quirks reproduced on purpose** (it is the oracle; each is marked `MAME:` in `scsp.h`): timer period (255 − reload) samples; interrupt sources raise lines one at a time in the order A, B, C, MIDI and lines stay up until SCIRE; reading the monitor overwrites MSLC; the DSP touches delay memory only on odd steps.
- **The board's output carries a DC offset** (~5000/32768 in STF, from the DSP path, identical in MAME's WAV). Keep it in the board for grading; `audio_out.h` high-passes it for the host.
- Grade with `tools/mame/snd_capture.py` → `tests/snd_replay.c` → `tools/mame/snd_compare.py` (see tools/README.md, "The sound board").

### HLE Hooks (game-specific addresses, board-level patterns)

These addresses are STF-specific. The **patterns** repeat across the catalogue — every Model 2 game ships some form of COP self-test and frame-loop entry; the addresses change per ROMset.

- **`CoProcessorErr` at `0x74E4` (STF)** must be bypassed. Return via `locals.rip` (saved return address from the call frame), NOT normal IP advance. Without this, STF hangs at the Sega logo. Every Model 2 game will have an equivalent — find by symptom (hang on boot, COP self-test loop).
- **Timer IRQ flag at `0x50008C` (STF)** — write `0x01` to unblock the polled wait loop.
- **Frame pacing** is driven by `variable_diff_calc` (~`0x11A04` in STF) setting a volatile `g_frame_done`. The emu thread runs up to `EMU_STEPS_PER_SLICE` (500,000) instructions per slice — sized to always reach the frame boundary.
- **Never hook `clip_point_check_yoko` / `clip_point_check`** (STF `0x28188` / `0x28250`, FV `0x236BC` / `0x23784`). They return nothing in `g0`. Each runs a point list through op `0x29` and writes one outcode byte per point to `0x50E000` (`0x90` behind the lens, `0x81`/`0x82` off the left/right edge). `area_clip` then draws a ground chunk only when the AND of its four corners' outcodes is zero, and finally draws the chunk the camera stands over.
  - *Symptom that surfaced this in STF:* stage ground chunks popped in and out as the fighters moved. The hooks skipped the write, and `0x50E000` is scratch that `rob_spd_control` fills with fighter X/Y/Z floats, so the cull was ANDing position bytes. Measured by `tools/grade-cull.mjs`: 0 of 299 frames right and 998 chunk toggles with the hooks, against 17 for the ROM's rule; exact in 599 of 599 without them.

### Threading

- **Unlock the emu mutex BEFORE sleeping.** Sleeping inside the critical section freezes the UI.
- **Double-buffered CPU snapshot** (`cpu_snapshot` + `cpu_prev_snapshot`); UI always reads the current snapshot.
- **Sleep granularity**: Windows `Sleep()` ≈ 1 ms; POSIX `usleep()` ≈ 1 µs.

---

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

`cmake` is on `PATH` (`C:\Program Files\CMake\bin\cmake`). The installed toolchain is **Visual Studio 2022**; a `build/` tree configured for an older generator fails with "could not find specified instance of Visual Studio" — configure a fresh directory rather than reusing it.

Everything under `vendor/` is a **git submodule pinned to an exact upstream commit** — `imgui`, `dear_bindings`, `sokol`, `miniz`, `ImGuiFileDialog`, and `noclip` (the last only feeds `tools/`). A tree cloned without them fails configure with the `git submodule update --init …` line to run.

The cimgui C bindings are **not committed**: CMake runs `vendor/dear_bindings/dear_bindings.py` over `vendor/imgui/imgui.h` into `<build>/cimgui-gen/` at build time, with `--replace-prefix ImGui_=ig` to keep the `ig*` spelling that `src/` and `sokol_imgui.h`'s "original cimgui" path expect. That needs Python 3 with `ply` (`python -m pip install ply==3.11`); configure fails with the exact install line if the interpreter CMake picks up cannot import it. Do **not** swap this for the `cimgui/cimgui` repo — that is a different generator, and it produced an `ImGuiIO` ABI mismatch here (`MousePos` updated, `MouseDown` stuck at 0).

```
cmake -S <repo> -B <repo>/build_vs22 -G "Visual Studio 17 2022" -A x64
cmake --build <repo>/build_vs22 --config Release --target ALL_BUILD -j 16
```

Output: `build_vs22\Release\m2hle.exe`. No automated tests — validation is interactive through the GUI. `--headless --mcp --rom <zip> --run` runs the emulator and its bridge with no window, GPU or audio device; the graders launch it that way (`$M2_WINDOW=1` shows the window). The active game profile is resolved by matching ROM CRC32s; STF (sfight + schamp) loads by default if present in the working directory.

**Grading harness** — [tools/](tools/) measures this emulator against an independent implementation of the same ROM formats (the STF explorer, a submodule at `vendor/noclip`), with SHA-256 over a MAME capture as a third point so the two ports cannot simply agree with each other and be wrong together. `node tools/grade-models.mjs` is the one to run after touching `geo3d.h`, and `node tools/grade-pose.mjs` after touching the COP bone handlers in `sharc_exec.h` — the latter replays 328 frames of rig arguments captured off a real board, so it needs a sibling `stf-tools` checkout for `motion-pose.csv` and skips cleanly without one. `node tools/grade-cull.mjs` checks which arena ground chunks get drawn against the ROM's own `area_clip` rule, on the camera the display list was drawn from. `node tools/grade-stages.mjs` plays a round on each of the fifteen stages (picked at ROUND_INIT, `0xAFC8`). It checks every arena part on the stage object clocks, the moving stages' flights, the matrices the COP lays into the display list and the texture scrolls, all against the explorer. Run it after touching the COP matrix handlers or anything a stage routine calls. `node tools/match-replay.mjs` plays attract's preprogrammed Sonic vs Bean replay fight (`--match-replay` skips the intro movie) and holds both fighters frame by frame against a MAME reference taken with `--mame`. The fight is an input replay, so any divergence is a simulation bug; run it after touching the i960 core or any COP handler the fight uses. See [tools/README.md](tools/README.md).

---

## Conventions

- All modules except `demo.c` and `miniz.c` are **header-only `.h` files**. This is intentional — do not split into `.c`/`.h` pairs.
- Default new code to the **board layer**; only move to a `game_profile_t` quirk when there's positive evidence of game-specific behaviour.
- Memory addresses and sizes use `uint32_t`. Sign-extension is handled per-instruction.
- Platform threading is abstracted in `emu_thread.h`: `emu_lock()` / `emu_unlock()` wrap `CRITICAL_SECTION` on Windows and `pthread_mutex_t` on POSIX.
- Logging: `log_msg(severity, fmt, ...)` from `log.h` — `LOG_INFO`, `LOG_WARN`, `LOG_ERROR`. **Log unknown COP commands and unhandled MMIO at WARN** so new-game support work surfaces them automatically.
