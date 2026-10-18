# m2-hle2 and noclip: where they differ, and who is right (Pinboard #298)

Both projects read the same ROMs and the same reverse engineering, and both have been finding bugs on their own. This file compares them: m2-hle2 at `330fe77` and noclip (the STF explorer, `vendor/noclip`) at `2509729`, both from 2026-10-01. For each difference it says which side is right and why. "Right" means it agrees with the board: MAME's `model2_v.cpp` / `model2rd.ipp` for the 3D, and the SHARC firmware (`stf-sharc`) and the i960 listing for the coprocessor. Where neither side has the board's rule, the entry says so.

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
- **Model-table length.** The word before the table is the *last index*, not the count. STF has 5104 entries (both sides say 5103) and FV 5413 (m2-hle2 said 5412 until #1025, noclip 5413). Only the model viewer and the bridge's `dump_model` range use it (`sfight.h`, `fvipers.h`, `mcp_bridge.h`).
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

---

## 6. The games each side has (Pinboard #660)

The two did not know the same games. noclip opens Virtua Cop, Daytona USA (eight builds) and The House of the Dead (three revisions), and m2-hle2 could not even name those sets; m2-hle2 runs Sega Rally Championship, and noclip called that set unrecognised. Neither side was ready to *run* the other's games, so #660 prepares profiles on each side instead: the set is recognised, its chips load with MAME's names, CRCs and offsets, and the player is told what is missing.

### 6.1 In m2-hle2: `vcop`, `daytona`, `hotd`

(`hotd` runs since Pinboard #1034 and is in `g_profiles`; the section is as #660 left it.)

`src/profiles/vcop.h`, `daytona.h` and `hotd.h`, listed in `g_prepared_profiles` (`registry.h`), not in `g_profiles`. Each has a load function and no install function, so no front end can boot one; every front end (desktop, SDL, libretro, web) says instead "*X* ('*set*') is known but cannot run yet; the board lacks: ..." (`profile_prepared_message`, `profile_missing_message`).

- **The ROM list is data** (`src/core/rom_recipe.h`): one line per MAME `ROM_LOAD32_WORD` / `ROM_LOAD16_WORD_SWAP` / `ROM_LOAD` / `ROM_COPY`, the same shape as noclip's `regions` (`[offset, lo, crc, hi, hiCrc]`), so a recipe reads line for line against MAME and the explorer. A recipe's `older` parts stand in for a part the zip lacks: the container's `hotd.zip` is MAME's first release (`hotdo`, `epr-19696.15` / `epr-19697.16`) under the parent's name, and loads that way.
- **The model tables are noclip's** (`games.js`): vcop 0x4C8000, 1234 entries; daytona 0x83FBB4, 3190; hotd 0xE73530, 7477; mesh pointer `* 4 - 0x02000010 + 0x10` on all three.
- **What the board lacks**, the `preliminary` string: Virtua Cop and Daytona are the original Model 2 (a TGP with its program in ROM, and the Model 1 sound board, 68000 + two MultiPCMs); House of the Dead is a 2C (the MB86235 TGPx4, and light guns).
- `tests/prepared_test.c` (a ctest, in the canary) checks the profiles without ROMs, and with them loads each set and walks its model table the way noclip does.

### 6.2 In noclip: `srallyc`

`PREPARED` in `js/games.js`, beside `GAMES`, with the chips from m2-hle2's `srallyc.h`. A drop of the set is named, and the loader says what it lacks (model table, palette, texture sheets, none located yet) rather than "unrecognised". It becomes a game when those are found and it moves into `GAMES`.

### 6.3 A difference found doing it: the model-table entry

noclip reads an entry by stride and field (`readModelEntry`): the 1995 games (STF, FV, HOTD) keep the mesh pointer at +8 of 16 bytes, Virtua Cop at +0 of 16, Daytona at +0 of 20. m2-hle2 reads all three the same way (Pinboard #663): `board/model_table.h` has the layouts (`MODEL_LAYOUT_1995`, `_VCOP`, `_DAYTONA`) and `model_entry_read`, a profile names its layout in `game_quirks_t.model_layout` (zero is 1995's), and every reader of the table (the GEO lookup and walks, the object viewer, the texture extraction, the tests) goes through `profile_model_table`.

`prepared_test` holds the two to noclip's reads on the container's sets: the entry count, an FNV-1a over every entry's uv, mat and mesh words, three sample entries, and the meshes set and in RAM, all taken with node from noclip's `loadRomSet` + `readModelEntry`. Virtua Cop: 1234 entries, `0x2298d18d`; Daytona: 3190, `0xa7bff01d`; House of the Dead: 7477, `0xac1eb34f`.

Daytona has 27 entries (2139-2165) whose pointer has bit 23 clear. On the board that is polygon RAM, which the game fills at run time (MAME `geo_object_data`), not the polygon ROM. noclip's `meshOffsetOf` decodes them into the ROM all the same, to 0xFE0000xx. m2-hle2 leaves them as RAM (`model_mesh_in_rom`); the test counts them apart and does not read them as ROM. §6.8 decodes them from MAME's polygon RAM.

### 6.4 Following the mesh pointer (Pinboard #666)

noclip turns an entry's mesh pointer into a polygon-ROM offset with `meshOffsetOf` (pointer * 4 - `meshPtr.subtract` + `meshPtr.add`, `js/games.js`; 0x02000010 and 0x10 for every game here), and `decodeModel` keeps it only if the first 40-byte record lies inside the polygon ROM. m2-hle2 does both in `model_mesh_resolve` (`board/model_table.h`), with the profile's `mesh_ptr_subtract` / `mesh_ptr_add`. The GEO walk, the mesh cache, the 3D window and the tests all call it, so the arithmetic exists once. The 3D window used to leave out the `+ add` and showed every mesh 0x10 bytes early.

`prepared_test` holds it to noclip on the container's sets. For every ROM-backed entry it checks the resolved offset and the ten words of the first record, which are the ones `decodeModel` reads: the two points at +0..+20, the attribute at +24 (`f1` and the link type) and the normal at +28..+36. It also counts the meshes found and those past the ROM, and checks three entries' offset and attribute. The node script's FNV-1a covers each entry's index, offset and ten words. The numbers come from noclip's master (aefb324):

| set | meshes in ROM | past the ROM | FNV-1a |
|---|---|---|---|
| vcop | 1234 | 0 | 0xab8c6c68 |
| daytona | 3163 | 0 | 0xac46f303 |
| hotd | 7465 | 0 | 0xf2dfe35c |

Daytona's 27 polygon-RAM entries are skipped on both sides, so 3163 + 27 = 3190. Their offsets wrap to 0xFE0000xx, past the 16 MB polygon ROM, so `decodeModel` returns null for them. `model_mesh_resolve` refuses them before doing the arithmetic, so neither side ever reads them as ROM.

### 6.5 Decoding the mesh (Pinboard #669)

After `meshOffsetOf`, noclip's `decodeModel` walks the records, then runs the face loop over the texture header stream (at `matPtr` * 2, moved by each record's signed step in attribute bits 12..16) and the UV stream (at `uvPtr` * 2, moved by three or four pairs every face). It reads a header whenever the model has a stream, and the points only for a textured face, as far as the ROM holds them. A face counts when its corners A and B exist. It becomes two triangles as a quad, one as a triangle, and none as a lone edge.

m2-hle2's decoder (`geo3d_decode_model`) is built from the same pieces: `geo3d_ia_walk`, `geo3d_tho_step`, `geo3d_texhdr_words` and `geo3d_uv_read`. `geo3d_mesh_reads` (`board/geo3d.h`) walks a model-table model with those same pieces and reports what noclip reports. That is the records walked, every textures-ROM byte whose value a face uses (in order), the faces and the textured ones, the highest colour index, and which face each triangle came from.

One difference was found and fixed. noclip reads Virtua Cop's headers at address 0 (`modelTable.matAtZero`; model 0's `mat` is 0), while m2-hle2 took 0 to mean no headers. That cost 243 textured faces. `model_layout_t.mat_at_zero` now says so for Virtua Cop's layout, and `geo3d_model_have_mat` applies it in the decoder and the mesh cache. Daytona's one `mat` of 0 stays without headers on both sides.

`prepared_test` decodes every ROM-backed mesh and compares it with noclip's master (aefb324). noclip's numbers come from `decodeModel` run under node with the polygon and textures ROMs wrapped, so every index read is logged. The FNV-1a covers, per mesh, its index, offset and record count, the count and indices of the textures-ROM bytes read, then 0xFFFFFFFF for a null decode, or else `faceCount`, `texturedFaces`, `paletteMax`, the triangle count and each triangle's face.

| set | meshes | null | records | faces | textured | FNV-1a |
|---|---|---|---|---|---|---|
| vcop | 1234 | 0 | 61375 | 57495 | 57002 | 0xae157b11 |
| daytona | 3163 | 813 | 203300 | 163823 | 138791 | 0x5195b8e6 |
| hotd | 7465 | 0 | 466634 | 378760 | 367185 | 0xdc922fa1 |

On vcop and daytona no header address is in texture RAM (bit 23), negative, or past the ROM, and no set's UV stream starts in texture RAM, so the board's texture-RAM path never comes into it. Daytona's 813 null meshes are ones whose faces are all lone edges or lack A or B; both sides decode them to nothing. The 27 polygon-RAM entries (2139-2165) have no ROM mesh and are skipped here; §6.8 checks them against MAME's polygon RAM.

### 6.6 Decoded header fields (Pinboard #669)

The FNV in §6.5 covers which textures-ROM bytes are read, not what `decodeModel` makes of them. Its haveMat block turns the four header words into the face's texture fields: from th0 `textured` (bit 14), the face flags transparent (bit 13 on a textured face), checker (bit 15), mirror X / Y (bits 8 / 9) and wrap X / Y (bits 6 / 7), and `texw` / `texh` (32 << the 3-bit sizes); from th1 `lumaBase` ((th1 & 0xff) << 7); from th2 `tx`, `sheet` and `ty` (sheet · 1024 added); from th3 `matidx` ((th3 >> 6) & 0x3ff); and the `tw` / `th` it stores (`tw` 0 when untextured).

m2-hle2 gets the same fields from `geo3d_texhdr_decode` and `geo3d_texhdr_tile`, the decoder's own path. `geo3d_mesh_reads` now keeps each header it reads (`geo3d_reads_hdr_t`: the face, the decoded header, the tile rect). `prepared_test` hashes, per mesh, its index and header count, then per header the face and those twelve values (`decode_mix_headers`). noclip's side is the same FNV from a probe line added after the haveMat block in an exported copy of master (aefb324), so every header read is counted, including faces later dropped for a missing A or B.

| set | headers | textured | FNV-1a |
|---|---|---|---|
| vcop | 60141 | 59615 | 0x4add73cb |
| daytona | 200136 | 168923 | 0x06c84e43 |
| hotd | 459169 | 446049 | 0xcd3e0d51 |

All three agreed at once; no field differed and the decoder was not changed. The hash notices a slip: hashing `texy` in place of `ty` (the sheet offset dropped) fails all three. The 27 polygon-RAM entries stay skipped, as in §6.5.

### 6.7 Decoded triangles (Pinboard #672)

§6.5 and §6.6 stop at the header block. The rest of `decodeModel` is the face loop's own reads: the further 40-byte records the index-array walk takes (both corners, f1, the attribute word at +24, the normal at +28..+36), the UV words, and what it hands `emitTri` for each triangle. The triangle choice is in a shared helper now, `geo3d_face_tris`: a triangle's ABC, a quad's ABD + ADC, or ABC + BDC when an earlier copy of the quad was cut the other way (noclip's `quadSplits`). `geo3d_emit_face` and the census both use it. `geo3d_mesh_reads` keeps each triangle (`geo3d_reads_tri_t`, from the walk with no matrix, the split table reset per mesh as `geo3d_decode_model` does):

- its three corners;
- their UVs (0 on an untextured face);
- the material slot (attribute bits 18..22) and attribute bit 17;
- the record's ROM normal;
- the z mode standing, from `geo3d_zsort_step`;
- the four z-source corners;
- the facing point (C, or A without one).

`prepared_test` hashes these per mesh (`decode_mix_tris`): the mesh's index, then per triangle the values above, with floats as their bits and NaN as 0x7fc00000, then the triangle count. noclip's side is a probe at the top of `emitTri` in the same exported copy of master (aefb324).

| set | triangles | FNV-1a |
|---|---|---|
| vcop | 100826 | 0x4a1dafe1 |
| daytona | 299707 | 0xa00063c5 |
| hotd | 664370 | 0xc855317a |

All three agreed at once, and the decoder's output did not change. The hash notices a slip. Each of these fails all three sets:

- ADC wound as ACD;
- the quad UV slots in stream order instead of B,A,C,D;
- the facing point always A;
- the z mode always 1.

Without the per-mesh reset, the split table fills and its probe never ends.

Two things are left out of the hash:

- **The normal `emitTri` stores.** It is the ROM normal normalised, or a cross product when the ROM normal is too short (|n|₁ ≤ 0.5). It comes out of double arithmetic in noclip. For hotd it is a plane normal (`lighting.planeNormals`), which is lighting, not decoding.
- **Face-flag bit 128.** It is a cull rule over values the hash already holds, not a further read; see below.

**Face-flag bit 128** (Pinboard #676). `decodeModel` does store it on every triangle (`flags`, js/model.js line 525 at aefb324): `((att >>> 17) & 1) || !faceN`. `faceN` is "this face has a normal": the ROM normal with |n|₁ > 0.5, or, in a game with `lighting.planeNormals` (hotd), also a non-zero plane of A, B and C. Its only reader is the viewer's `boardDrawsFace`, which skips the facing test when the bit is set. So it is render-side. It takes nothing from the ROM that the triangle hash does not already cover: bit 17, the ROM normal, A and B (the face's first triangle's first two corners) and C (the facing point). A probe on the same export of master recomputed the bit from those hashed values for every triangle and matched noclip's `flags` on all of them:

| set | triangles | bit 128 set | of those, not from bit 17 | mismatches |
|---|---|---|---|---|
| vcop | 100826 | 13594 | 0 | 0 |
| daytona | 299707 | 44618 | 0 | 0 |
| hotd | 664370 | 182983 | 715 | 0 |

In vcop and daytona bit 128 is bit 17 on every triangle: no ROM normal there is short. hotd's 715 others have a zero ROM normal and a degenerate plane. The bit is therefore not added to the hash, and m2-hle2 gets no field for it.

One difference, recorded and not changed: when hotd's plane is degenerate, noclip falls back to the ROM normal. MAME's `geo_parse_nn_ns` does not: `normalize_vector` leaves the zero cross product as it is, so `dotp` is 0 and the face counts as front. m2-hle2's `geo3d_board_normal` / `geo3d_board_cull` do the same as MAME. On the container's hotd that is 2257 triangles (2263 counting the board's float cross product, §6.8) without bit 17 whose plane is degenerate and whose ROM normal is not zero. Those triangles are culled and lit by different rules on the two sides. It is lighting and culling, not decoding, and noclip is not changed here.

noclip's z mode starts at 1, and m2-hle2's at 0 ("none yet", which `geo3d_sort_z` treats as 1). The census reports 0 as 1. In these three sets every mesh's first drawn face names a mode, so this never comes up.

**Face colours** (Pinboard #681). One field `decodeModel` returns is in none of the hashes above: `colors`, set in the haveMat block (js/model.js line 458 at aefb324) by `if (palette[matidx] >= 0) bgr555(palette[matidx], rgb)`. The header-field hash covers `matidx`, not the colour. `palette` is `facePalette(rom)` (js/romset.js line 281): 1024 entries, BGR555, -1 where it has no colour. Read from that source, the three sets are:

- **daytona** (the `DAYTONA_REV_A` profile): 1007 words of the data ROM pair `epr-16724a.6` / `epr-16725a.7`, `mainData` 0x84F4EC (0x4F4EC into the pair), in entries 0–1006. Entries 1007–1023 are -1 (`paletteCount`). Every colour that can be stored is one ROM word.
- **vcop**: 1024 words of the data ROM pair `mpr-17162.8` / `mpr-17163.9`, `mainData` 0x4C0000 (0xC0000 into the pair). `paletteFixes` then puts constants over entries 0xE1–0xE5 (0xB245) and 0x173–0x177 (0xA1B2, 0xBA96 ×3, 0xA1D2). They are the values the boot stores by hand at 0x38B58 after it copies the table, so they are not words of that table.
- **hotd** (the container's `hotdo`): not one table. The colours come from the program ROM pair `epr-19696.15` / `epr-19697.16`: the pointer list at 0xA98F0 (13 sets, each a count and then the colours), with set 0 at entry 0 and the others from `split` 500 on, and the table at 0xA35B8 filled downward from 1023. Which set is written above 500 is `rom.paletteSet`, which the viewer moves with the texture set. The sets in `loadOrder` before it are written first and fill what it leaves. Entry 1023 is then set to the constant 0x801F (`fixed`).

The four ROM pairs are all loaded by the prepared profiles (`profiles/vcop.h`, `daytona.h`, `hotd.h`). Only Daytona's colours are fixed ROM words, though. Virtua Cop's include ten values the boot writes, and House of the Dead's depend on the set the viewer chooses. m2-hle2's decoder takes a face's colour from palette RAM at colorbase + 0x1000, which the game fills as it runs, and falls back to `main_data` + 0x100000 only when there is no palette RAM (`geo3d_palette_color`, STF's layout). So `colors` is not hashed, and no reader or ROM load is added for it. §6.8 compares it with MAME's palette RAM. The rest of `decodeModel`'s return value is covered by the hashes above or is lighting. Every field decodeModel returns is in the mesh checksum, the header-field FNV, or the triangle hash, or is lighting or the derived bit 128, except colors at js/model.js line 458: that value is the facePalette read described above, it is in none of those hashes, and it is not hashed, because Virtua Cop's ten boot-written slots and House of the Dead's viewer-chosen table are not fixed ROM words.

Texel bytes are not a field decodeModel returns, and they are in none of the hashes above. On noclip master aefb324, buildTexram (js/texture.js) copies those bytes and does not read tx, ty, sheet, tw, th, matidx, or lumaBase. Virtua Cop and Daytona go through buildDaytonaTexram; House of the Dead goes through buildRawTexram. Each copy is a 1 MB bank: a CPU address minus 0x02000000 is the data-ROM offset. The boot bank is a constant (Virtua Cop 0x2300000 in mpr-17164.10 / mpr-17165.11, Daytona 0x2500000 in mpr-16808.8 / mpr-16809.9, House of the Dead 0x02000000 in mpr-19704.11 / mpr-19705.12) and is the same on every load. The other bank's address is a word in an already-loaded ROM, indexed by a set number (Virtua Cop: data 0x8190E8 + set×4 in epr-17168a.6 / epr-17169a.7, pointing into mpr-17164.10 / mpr-17165.11; Daytona: program 0x15A0 + set×4 in epr-16722a.12 / epr-16723a.13, pointing into mpr-16528.10 / mpr-16529.11 or mpr-16808.8 / mpr-16809.9; House of the Dead: program 0xA9970 + set×4 in epr-19696.15 / epr-19697.16, pointing into mpr-19704.11 / mpr-19705.12 or mpr-19702.9 / mpr-19703.10). House of the Dead also copies that set's eight 0x2000-byte patch blocks, none for set 0, whose pointers are the words at program 0xC15E0 + set×32, from mpr-19702.9 / mpr-19703.10. No bank address is a word the boot writes, and there is no runtime fill: every copied byte is a fixed ROM word. Which set is copied is not fixed. Virtua Cop uses the viewer's texture-set picker or default set 0. Daytona uses the course records in ROM, or that picker, or default set 0. House of the Dead uses the profile bankSets table, which that picker can override; the shared bank 0 is drawn under set 1. The only read that indexes a pixel with tx, ty, sheet, tw, and th is the viewer's draw path (levelTile and tileTexel in js/viewer.js). lumaBase indexes the luma table there, not texel bytes. matidx selects a colour, not a pixel. Those bytes are not hashed, and no texel reader and no ROM load are added, because the set bank is not one fixed image and that pixel index is only the viewer's draw path. The prepared profiles already load every ROM file these copies read. §6.8 compares these copies with MAME's texture RAM.

### 6.8 The checks §6.7 left out, against MAME (Pinboard #692)

Five things §6.7 records but does not check: lighting, culling, texels, the Daytona polygon-RAM entries, and face colours. The ground truth is the shared MAME build (`~/build/mame-bin/mame-shared/shared`, `-video none -nothrottle`), with a Lua script that dumps palette RAM (0x01800000), the two texture RAMs, both polygon RAMs and the raster's texture RAM at set times. The container's sets lack three support ROMs (vcop's `hd44780_a00.bin`, daytona's `epr-16726.bin` and `epr-16488a.ic12`); zero-filled stand-ins boot with a checksum warning. The container's `hotd.zip` is MAME's `hotdo`, which is noclip's id for it as well.

**Lighting and bit 128** (in `prepared_test`). Each triangle's emitted normal is computed as `decodeModel` does: the face normal when |nx|+|ny|+|nz| > 0.5, else the triangle's cross product; on hotd (`lighting.planeNormals`) the negated plane cross of A, B, C when the face has a C. It is divided by V8's `Math.hypot` (reproduced exactly) and stored as float. The FNV of the three floats and bit 128 per triangle matches noclip at aefb324 bit for bit:

| set | triangles with bit 128 | normals FNV |
|---|---|---|
| vcop | 13594 | 0xB34338BB |
| daytona | 44618 | 0xB84EB618 |
| hotd | 182983 | 0x03EAF696 |

**Against the board's normal.** The test also runs `geo3d_board_normal` (identity matrix, mode 2 on hotd) on every triangle and counts four things. These are triangles culled by a different rule (both-sided on the board ≠ bit 128), lit from another direction (cosine < 0.9999), lit by a zero normal, and lit by a non-unit one. vcop and daytona are 0 in all four. hotd has 2263 culled otherwise, 0 from another direction, 10297 by a zero normal, and 0 non-unit. The 2263 are §6.7's degenerate planes: 2257 whose cross is zero in doubles and 6 (one face) where it is zero in the board's float cross but 2.7e-9 in noclip's double one. The 10297 are faces whose plane is degenerate; MAME's `normalize_vector` leaves those at zero (the 2263 among them; the rest have bit 17 set or a ROM normal of 0.5 or less, which both sides draw two-sided, and noclip lights them with the triangle's own cross product). It is the §6.7 difference, now counted; noclip is not changed.

**Texels.** MAME's texture RAM (the first 1 MB of each sheet, 16-bit words in write order) was compared with what noclip's `buildTexram` builds for the same set. There were 0 differing words in either sheet: vcop under set 0 (40 s and 90 s), daytona under set 0 (40 s; set 3 is the same bank), and hotdo under set 3 at 40 s and set 1 at 90 s. So the bank addresses and House of the Dead's patch blocks are the board's.

**Daytona entries 2139–2165.** Their mesh pointers (0x4…0x51E) are in slow polygon RAM; their UV and material pointers (0x805000…, 0x800000…) have bit 23 set and read the raster's texture RAM. MAME's polygon RAM 0 is the same at every dump from 20 s to 160 s, while the texture RAM changes. Both sides decoded the dumps: `geo3d_mesh_reads` with `obj_mesh`, and `decodeModel` with its `mesh` argument and texture RAM placed at textures byte 0x1000000. Both decode 27 meshes, 69 faces (all textured) and 138 triangles, identical in every field (corners, UVs, material, normal, bit 128, face point, z-corners) at all eight dumps. They have 0 differences against the board's normal as well.

**Face colours.** MAME's palram[0x1000 + i] against `facePalette`:

- **vcop**: all 1024 entries are the same from 20 s to 80 s and at 100 s and 120 s, so the ten boot-written slots are the board's. At 90 s, 401 differ (a fade).
- **daytona**: 1004 the same, 17 unset by noclip (1007–1023, which no face uses), and 3 differ at every time from 20 s to 180 s. They are 430 (noclip 0xA7FF, MAME 0x83FF), 437 (0xCE7F, 0xFE73) and 438 (0xCFE9, 0xFE73), used by 630, 545 and 68 face headers, all textured.
- **hotdo**: the best-matching set changes as the game runs (2, 3, 1…), and at best 10–11 entries differ. Entry 211 (noclip 0xA18E, MAME 0xEBDF; 392 textured headers) differs every time. 1023 (noclip's fixed 0x801F, MAME 0xB340; 18911 textured headers) differs too. 959 and 984–991 are faded values later on. At other times 31 differ (984–993), or 107–117 (532–542 and 684–694, a table none of noclip's sets matches).

None of the differing colours is on an untextured face, so on these sets they reach a textured face only as its colour field. The three Daytona words and hotd's 211 and 1023 are noclip's to look at; the rest are the game changing its palette at run time, which a fixed table cannot hold. m2-hle2 reads palette RAM, as the board does, so nothing changes here.
