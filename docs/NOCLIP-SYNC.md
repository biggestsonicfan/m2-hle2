# m2-hle2 and noclip: where they differ, and who is right (Pinboard #298)

Both projects read the same ROMs and the same reverse engineering, and both have been finding bugs on their own. This file compares them: m2-hle2 at `330fe77` and [noclip](https://github.com/biggestsonicfan/noclip) (the STF explorer, `vendor/noclip`) at `2509729`, both from 2026-10-01. For each difference it says which side is right and why. "Right" means it agrees with the board: MAME's `model2_v.cpp` / `model2rd.ipp` for the 3D, and the SHARC firmware ([`stf-sharc`](https://github.com/biggestsonicfan/stf-sharc)) and the i960 listing for the coprocessor. Where neither side has the board's rule, the entry says so.

This branch makes the m2-hle2 changes in section 1. The noclip side (section 2) is a list for whoever works on noclip next; nothing in noclip was changed.

---

## 1. Where noclip is ahead (adopted here)

### 1.1 The texture-header walk and the triangle relink (board level)

The board reads a polygon's texture header at the current address, then moves the address by the signed record count in attribute bits 12–16 (MAME `model2_3d_process_polygon`: `tho = (attr >> 12) & 0x1f`, sign-extended, `command_buffer[1] += tho * 4`). It does this for every polygon, culled or not. A triangle's record has room for two new points, and the geometrizer takes the second from the first (`model2_v.cpp`: "the rope of P1(n) is achieved by P0(n-1)").

noclip walks the stream that way (`js/model.js`, "The material stream is walked, not indexed") and relinks after a triangle. It learned both from the HOTD prototype, which reuses headers and stores zeros after a triangle. m2-hle2 counted one record per *emitted* face (`efi`), and took the stored point.

On Sonic The Fighters and Fighting Vipers the two rules give the same answer: those ROMs store a step of 1 on every face that draws, 0 on the rest, and repeat P0 after a triangle. This branch ports the board's rule to both index-array paths (`geo3d_tho_step` and `geo3d_relink_triangles` in `geo3d.h`), so the next game does not have to find it again. Measured:

- `grade-models.mjs` against noclip 2509729: J = 1.000000 over 598,728 triangles, and 1,795,005 of 1,795,005 textured corners agree.
- `dump_model` before and after: STF is byte-identical. In FV, 5,411 of 5,412 entries are byte-identical. The exception is entry 2619, whose "attribute" words are floats (`0x3cc49ba6`, `0xbda786c2`, …), so its pointer lands on something other than a mesh. Both rules make nonsense of it, and the new one is the nonsense MAME would make.

### 1.2 The submodule, and four grade-stages corrections noclip now makes itself

`grade-stages.mjs` used to add five things `display.js` left out. noclip 2509729 does four of them itself: `pole_disp`'s 3x3 reset before each flame, the sphynx head and the Giant Wing plane drawn from inner slot 8 at 1.6, and Dynamite Plant's second gear starting at 0x800. With the submodule bumped, three of those corrections would have been applied twice: the 1.6 scale squared, and the gear 0x1000 off. They are gone from `OMISSIONS` and from tools/README. The cage shake (`dword_903D0[word_50A1E8[wall]]`) is still missing in noclip and stays in the list.

### 1.3 The SKY EYE link opens the explorer at the board's lens

noclip's stage tab has a board-lens switch (`lens`, `js/viewlink.js`): focal 280 on 384 lines, a 68.9° field of view, the same one `geo3d.h` takes from the display list. m2-hle2's `sky_eye_link` did not set it, so a link opened at the explorer's own field of view. It now adds `lens=1`.

### 1.4 Two stale notes

- CLAUDE.md said the i960's osage integrator is switched off. What is switched off is the wind oscillator. All 52 entries of the table at `0x68AA4` point at `0x689E4`, whose amplitude is 0, and nothing references the live table at `0x68A04`. Bit 0 of a record is a first-frame flag: `osage_init` (0x67600) sets it and 0x67D1C clears it.
- `sharc_exec.h`'s `0x19003232` comment said "evaluated in double". The code is float Horner, in the firmware's order, as CLAUDE.md requires.

---

## 2. Where m2-hle2 is ahead (for noclip)

Each of these was found in m2-hle2 against MAME or the firmware, after the explorer's version was written.

| # | What | Board rule | noclip today |
|---|---|---|---|
| 1 | Depth at the game's camera | One flat key per polygon from attribute bits 10–11 (`float_to_zval`); the nearer key wins and a tie goes to the later polygon. m2-hle2 has drawn game frames this way since #247, and 325,730 of 332,250 changed pixels came out nearer MAME. | The half rule (recede up to a bound, `nearMin`), with layers on top. The unmerged `layers-board-key` branch (`b078c91`, `rankLayers`) moves the layers onto the key. For a view at the game's own camera (SKY EYE eye/ang with the board lens), the flat key is the board. |
| 2 | Checker phase | `(x ^ scanline) & 1` in board pixels, counted from the top (`bpix` in `game_render.h`). `gl_FragCoord` counts from the bottom, which is the other phase on 384 lines. | Phase from `gl_FragCoord`. |
| 3 | Untextured transparent faces | The board draws nothing for them. | Drawn. |
| 4 | Specular, and the untextured colour ramp | m2-hle2 lights with specular and uses the colorxlat solid ramp for STF and FV. | No specular; the solid ramp is not the default. |
| 5 | Motion blend angles (`0x54`/`0x55`, `_L2117C`) | Nine turns, read back with atan2/asin, and the half-turn alternative judged with the register already overwritten to −1 (CLAUDE.md, "Fn_get_sm_ang_r"). | `moves.js` (`chainSample`, `easeLength`) eases each channel on its own. |
| 6 | IK bases (`0x6B`) | m2-hle2's port of the firmware makes six base turns before the two-bone solve. | `pose.js` `solveIK` applies three. Worth checking against `grade-pose`'s capture before changing. |
| 7 | COP arithmetic | Firmware √, 1/√, ÷ and atan2 (`sharc_fw_*`); angles truncate (`floor(rad · 0x4622F983)`). | Host maths; `toAngle` (`motion.js`) rounds. Differs in the low bits, which a replay carries forward. |
| 8 | Afterimages (zanzou) | One 128-slot ring shared by both fighters, at DM `0x32300`; the part mask is mirrored for a fighter facing left. | Per-fighter, unmirrored. |
| 9 | SKY EYE links | m2-hle2 writes `eye`/`ang` (the board's record) and, for older builds, `pos`/`look`/`target`. | `app.js` prefers `pos` over the board's `eye` when both are there, and `pos` is only right on stages that do not fly. So a link from m2-hle2 lands wrong on stages 1, 4 and 7. The fix belongs in noclip: prefer the board's fields. Neither link carries the stage clock (`t`) yet. |
| 10 | Sound | noclip's `tools/sound/board.c` builds m2-hle2's sound board (`sound.h`) from commit `c4ed74f`. Since then: the serial MIDI line, wait states, the interrupt timing, the MADRS fix (no DC offset), the lazy SCSP. | Rebuild `board.wasm` from current m2-hle2, and call `sound_run(n)` in batches. |

Wording, not behaviour:

- `recede` and `nearMin` are explorer settings, not game rules (TECHNICAL.md around lines 811–860, `games.js` around 456–481).
- FV's bank is at 0x1800000 (`games.js` around 258–262).
- The 2P music facts (South Island on Canyon Cruise and Casino Night; North Wind for Sonic against Knuckles) belong in the Music section.
- The `surfaces` premise of `stf-tails-lab-lintel` (`110c103`) is not what MAME does.
- The head-aim default (object 14) is unverified.

---

## 3. Neither side has the board's rule

- **The texel snap.** MAME truncates u, v to 8 fractional bits (`s32(uoz * z * 256.0F)`, `model2rd.ipp`), then subtracts half a texel. noclip rounds to the nearest 1/256 (its issue 44), and m2-hle2's fill copies noclip (`game_render.h`). Truncation is the board. Grade it against MAME (the Tails-lab attract intro, #85) before either side changes.
- **Model-table length.** The word before the table is the *last index*, not the count. STF has 5104 entries (both sides say 5103) and FV 5413 (m2-hle2 says 5412, noclip 5413). Only the model viewer and the bridge's `dump_model` range use it (`sfight.h`, `fvipers.h`, `mcp_bridge.h`).
- **`master_z_clip`** is modelled on neither side.
- **Microtexture** (texture-header 0 bit 12) is drawn on neither side. Count how many STF and FV faces set it before deciding whether it matters.
- **The object viewer's free camera** has no board sort to copy. m2-hle2's layers are noclip's old vote, and noclip's `rankLayers` (`layers-board-key`) is the newer version to port once it lands.

---

## 4. What already agrees

These hold on both sides and are checked by `grade-models`, `grade-pose`, `grade-stages` or `cop_replay`:

- the iFlag table and the link-0 wipe;
- A-B-D-C winding, the UV order and reusing a quad's diagonal;
- the mesh pointer, and ROM normals left unnormalised;
- the rear test at corner C;
- lumaram indexed with the filtered texel, gamma, and clamp against smooth wrap;
- mip addressing;
- both spline tangents, the IK bone pairing and the zanzou constants;
- the SKY EYE camera record.

The Model 1 sound board (`tools/sound/model1*`, Daytona) is noclip's alone.

---

## 5. Keeping them in step

The two drift because each finds a board rule and the other never hears about it. Three habits would keep them close:

1. **Bump `vendor/noclip` when noclip lands a board rule**, and run `grade-models`, `grade-stages` and `grade-pose`. A grader that adds its own correction has to drop it when the explorer gains it (1.2).
2. **Cite the board, not the other project.** A rule that names a MAME line or a firmware address can be checked by either side. A rule that cites the other project only carries its mistakes across, as the double-applied corrections in 1.2 would have.
3. **Cross-link.** When either side fixes a board rule, file one line on the other repo naming the MAME or firmware citation.
