# Bubblegum fixes: an audit (Pinboard #245)

This file lists the places where m2-hle2 patches a symptom (a list of model numbers, a fitted threshold, a hook that pokes RAM) and the board has a simpler rule underneath. It is the audit of master at `28d3563` (2026-09-30), and nothing in `src/` was changed by it. Line numbers are as of that commit.

The board side of the sound path (`sound.h`, `scsp.h`, `m68k_*`), the tile renderer and the host audio came out clean: their constants cite MAME, the manual or a measurement. The console-DLL traps, the region, damage and VS hooks, the cross-play hooks and the `versus_result` observer are deliberate features, not patches. Everything else below is ranked by how much it would condense.

---

## 1. The z-sort: six rules standing in for one

**Done in PR #159 (Pinboard #247).** Game frames now draw each face at its board key (`geo3d_flat_depth`), and the patches in the table below are gone from the game draw. The rest of this section is the audit as it was written. Pinboard #249 then went looking for the camera-dependent breakage that #244's patch had answered, using the round intro on every stage (`tools/grade-round-intro.mjs`); see "After the flat key" at the end of this section.

**The board's rule** (MAME `model2_v.cpp`, `model2_3d_process_polygon` and `model2_3d_frame_end`) is short:

- Each polygon gets one z, chosen by attribute bits 10–11: mode 0 is the previous polygon's z, mode 1 the nearest corner, mode 2 the farthest corner, mode 3 is 1e10.
- `float_to_zval` (our `geo3d_board_zkey`, already ported) turns that z into a 16-bit bucket.
- Buckets are drawn nearest first, the newest polygon first within a bucket, and a pixel is written only once.

So there is no per-pixel depth, and the nearer key wins outright. A tie goes to the later polygon, and windows are drawn from last to first.

**What we do instead** keeps the GPU's interpolated depth and lets a polygon only *recede* towards its key, by at most 12 units (`geo3d.h:408-456`, shader at `game_render.h:359-366`). This is the explorer's compromise (`vendor/noclip/js/viewer.js:104-113`), and it was made for a *free* camera: pulling a polygon forward "under a free one turns a floor into a wall". m2-hle2 draws every game frame from the board's own camera, so that reason does not apply here. The half-rule then needed these patches:

| Patch | Where | Keyed on | Symptom it fixed |
|---|---|---|---|
| Recede limit `g_geo3d_zsort_recede = 12.0` | `geo3d.h:440` | fitted constant | Aurora's ice wedges (the root patch; also clamps mode 3, "behind everything", to 12 units) |
| `zsort_standing = {4278}` | `sfight.h:789`, `geo3d.h:458-484` | **a model-number list** in the profile | Aurora pillars sinking through ice that is "too deep to recede" |
| `geo3d_mesh_keep_depth` | `geo3d.h:2460, 2792, 2984` | 0.02–0.5 gap thresholds | Tails-lab CAUTION screen (#85) |
| Coplanar layers + group planes, `geo3d_mesh_layers` | `geo3d.h:2502-2922` | 0.5 gap, 0.02 tie, cos 0.999, 1% overlap, `layer_steps=4`, mode-2 exclusion | pyramid shadows (580), reels (188), emblem (194), gloves (1813/1818), #75, #85 |
| `geo3d_tie_layer` | `geo3d.h:168-247` | identical corner set and bit-identical matrix | Tails' pupils through closed eyes |
| Same-matrix runs, `geo3d_run_get` | `geo3d.h:3274-3380` | consecutive draws with a bit-identical matrix (max 16) | Casino slot machine 186/187/188 (Pinboard #133) |
| (and the held pairs, `zheld`, PR #156) | | | Lunar Fox emblem (#225) |

Each comment says, in its own words, that "on the board the nearer key wins and a tie goes to the later polygon". The layers already compute that ordering (`geo3d_mesh_draw_layers`, `geo3d.h:2961`), but only for the pairs the heuristics picked out first.

**The real fix, in a few lines:** give every vertex of a polygon the *same* clip-space depth, taken from that polygon's board key (`gl_Position.z = depth(key) * w`, inside the window's slice of the depth range). Then draw in submission order with `LEQUAL`, which is already the compare function (`game_render.h:1244`). That reproduces the board's fill exactly:

- the nearer key wins;
- equal keys go to the later draw;
- checker and transparent holes already `discard`, which is the board's "no fill here";
- a 16-bit key fits the 24-bit depth buffer with room for a window index.

The recede limit, the standing list, keep-depth, layers, planes, ties, runs and held pairs would all go, along with their `set_camera` toggles (`mcp_bridge.h:277-297`). The quad split "same diagonal as before" would stop mattering for depth. The object viewer keeps the explorer's half-rule, because it has a free camera.

**Risk, to be measured, not assumed:** commit `17e9b72` says an *unbounded* recede turned half of Aurora's rink black behind the walrus reflection. That was a recede on a depth buffer, which is not the board's rule. MAME draws Aurora correctly with the full rule, so a faithful flat key should too. `grade-zsort.mjs --stage 1/5` and the Aurora, Tails-lab and Casino pictures are the test.

Two smaller gaps in the same area:

- A mode-0 polygon at the head of a model falls back to the nearest corner (`zset = false` per decode, `geo3d.h:3073`). On the board, `polygon_z` carries over from the previous object.
- `check_culling`'s `master_z_clip` and `max_z < 0` culls, and the four clip planes, are not modelled; `zclip_3d` RAM is mapped but never read. The homebrew HUD's "drop triangles over 1.5 units near the camera" reject (`geo3d.h:683-694`) hides a col0 decode bug that those planes would not have hidden.

## 2. Frame sync and timers are hooks, not interrupts

*Fixed in Pinboard #253.* The vblank is raised on the i960's cycle clock (416,667 cycles) and delivered to the profiles' handlers, the timers are always live, and the wait hooks are gone: STF's `interrupt_wait` (0x1768), `interrupt_wait_b` (0x11580), `_idle` (0x11610), `check_timer_4` (0x4A55C) and `check_timer_4_spin` (0x4A58C), and FV's 0x2238, 0x1184C/0x118DC and 0x4A88C. The side bugs went with them: `_idle`'s static that survived a reset, and FV's `read_sw` hook that zeroed 0x500700. `EMU_FRAME_STEPS_MAX` is gone too; `EMU_STEPS_PER_SLICE` is only a cap now, and `EMU_IRQ_TABLE_MAX_SLICES` stays, for an SDK kernel's task switch. CLAUDE.md (HLE Hooks) has the rest.

**COP ready bit.** The ready bit is set per game by a hook (STF 0xF3C, FV 0x190C), although `cop.h` already tracks the upload bit. A board-level read callback, "ready once the upload bit falls", would be about 15 lines and would cover every game.

## 3. The old COP-stream renderer (retired, Pinboard #251)

`geo3d_scan_captures` was the fallback for a GEO display list that did not reach END, and for any list with direct data (GEO 0x02/0x12), which `geo3d_scan_geo_list` could not walk. It held the most obviously fitted code in the tree: the shadow floor from a running minimum of the feet's Y, the eye-bake ±3.0 test, the `cam_mode != 9` experiment and the five `g_cam_sign_*` dials, the identity view for clip-window cells, the model 3333 skip, and `g_sharc.tgp_bone` with its four writers.

`geo3d_scan_geo_list` now walks direct data (MAME `geo_direct_data`), and all of the above is deleted. Measured first: no STF or FV frame sends direct data (attract in both games, and a round on each STF stage), and every list but the first two after boot reaches END, so the fallback only ever drew those two frames. A list that does not reach END now draws no 3D.

## 4. Small, cheap ones

| What | Where | Fix |
|---|---|---|
| `SANITIZE` turns NaN or overflow into 0.0 in 8 COP handlers | `sharc_exec.h:680` | Contradicts the rule that NaN leaves the chip as all ones. Delete it and re-run `cop_replay`. |
| `0x28805151` pushes three zeros | `sharc_exec.h:1820` | Port PM 0x20EF5 (reads the slot through DM 0x3033F). |
| `cop_read` returns 0 on an empty FIFO | `cop.h:233` | The board would stall. At least log an underflow at WARN. This is how `0x17002E2E` once hid. |
| `ldtime` returns 0 | `i960_exec.h:901` | Return the cycle count. |
| `_700000_loop` hook zeroes the sound-init delay | `sfight.h:283` | **Removed (#254).** It moved no game frame: boot slices are not frames here, and every column of `det_digest` but work RAM (frames 2-32) was the same. Only the netplay check (instruction count) changed, so `NETPLAY_PROTO_REV` is 11. |
| `warning_skip_addr` is on by default | `emu_thread.h:365` | **Labelled, measured (#254).** Japan boot: a frame here is MAME's + 800 with the skip (+812 by the attract fight), + 160 without (`det_digest --region japan --nowarnskip`). Kept on, since the graders pair frames by content; a netplay session now always skips. |
| FV model table / mesh pointer values are STF's with TODOs | `fvipers.h:~327` | **Verified (#254).** FV's table is at 0xE0004 too, with its length (5412) in the word before; its 3711 mesh pointers decode with STF's encoding inside its polygon ROM. |
| `fov = 65` for display-list profiles | `main.c:246-251` | **Derived (#254).** `geo3d_scan_displaylist` sets fov = 2·atan(192/fy) from the list's focal command (68.9° for m2-sdk's 280). The window's centre is the screen's for a full-screen window, and fx ≠ fy cannot be shown by the host camera. |

## Doc drift found on the way

- ~~CLAUDE.md says `0x07000E0E` is a no-op~~ — fixed: CLAUDE.md now describes `Fn_load_point`.
- `m68k_exec.h:659` says the step's cycle count is "approximate", but it has been held to MAME clock for clock since #119.

## Suggested order

1. ~~**The flat board key (§1).**~~ Done in PR #159. `grade-zsort` and `grade-round-intro` hold it against MAME.
2. Direct data in the display-list scan, then retire the fallback (§3). Done (#251).
3. The COP ready callback and the small COP items (§2 COP bit, §4). These are cheap, ROM-free and testable with `cop_replay`.
4. Vblank on the cycle clock plus live timers (§2). This has the biggest payoff in removed hooks and the biggest re-grade, so it should be done last.
