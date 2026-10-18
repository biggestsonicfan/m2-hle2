# Audit against MAME (Pinboard #246), 2026-10-01

Every grader in `tools/` run on one Linux build of master (157319a) against the
container's MAME (`~/build/mame-bin/claude_mame-mame/m2`) and the explorer, then
dead code taken out and the board proven unchanged. This file is the record.

## What the graders say

| Grader | Oracle | Result |
|---|---|---|
| ctests (19) | — | 19 / 19 pass |
| `grade-all --stage 0` (models, texram, colors) | explorer + MAME colorxlat | models J = 1.000000; texram 10 pass; colorxlat byte-exact |
| `match-replay` | MAME | same fight over 1299 frames; fighter state bit-identical to +321, rig from +382 (the known baseline) |
| `grade-motion` | MAME capture | 12 pass |
| `grade-pose` | MAME capture ([`stf-tools`](https://github.com/biggestsonicfan/stf-tools)) | 8 pass |
| `grade-osage` | MAME, select screen | 14 pass, after the fix below |
| `grade-cull` | the ROM's `area_clip` | 4 pass, 14 toggles, as the ROM's rule |
| `grade-stages` | explorer | 53 pass, 1 skipped |
| `grade-zsort --stage 1` | MAME | 4 pass; 4538 of 4734 changed pixels nearer MAME with the board key |
| `grade-zsort --stage 5` | MAME | 4 pass; 39,844 of 40,156 nearer MAME |
| `grade-reset` | itself | 22 pass |
| `grade-round-intro` | MAME | 14 pass, stages 0–13; median 60–2781 pixels a frame more than 48 off MAME |
| `grade-lunar-fox` | MAME | 4 pass; emblem mean 0.46, worst 0.74 a channel |

Nothing here is a new divergence from MAME. The open differences are the ones
already written down: the replay fight's rig from +382 and bufferram
`0x90E804` (MAME `0x80000000`, here 0).

## Two graders that had rotted

Both drove the game by fixed frame counts, and PR #163 (the slice is the board's
vblank) moved what a frame count reaches.

- **`grade-all --stage N` graded attract's stage, whatever N was.**
  `capture.mjs` held `stage_num` through attract and waited for the stage to
  load, but attract only fights on the Flying Carpet and `change_scene` reads
  `stage_num` once, at ROUND_INIT. The capture now plays into a round and pins
  the stage at `0xAFC8`, as `grade-stages` does. That code is one function,
  `reachRound` in `lib/dl.mjs`, shared by both. `capture.mjs`'s own
  `reachStage` / `holdFrames` are gone.
- **`grade-osage` captured at the wrong moment.** It waited 900 frames, coined
  up, waited 400 more, and ran unthrottled. So the coin and the stick landed on
  frames nobody chose. Fighter 10's cursor never moved, and fighter 4 was caught
  while the select screen opened (chain matrices 0.42 off orthonormal). Master's
  binary fails the same way, so the board is not the cause. It now waits for
  mode 7 / sub 5 to hold, as `osage-select.lua` does on MAME, and runs paced.

## Bloat removed

About 300 lines of the emulator, none of which the board ran:

- **UV / bank experiments in `geo3d.h`**: `g_uv_swap`, `g_uv_flip_u/v`,
  `g_uv_quad_order`, `g_uv_bank_mode`, `g_dbg_tex_*`, `g_dbg_face_uv`, and
  the window controls and `--bank` flag that set them. The UV order is settled
  (CLAUDE.md, "The UV stream runs B,A,C,D"), and `grade-models` holds it.
- **The COP-stream renderer's leftovers** (#251 deleted the renderer):
  - `g_cam_log` / `--camlog` / "Log camera CSV"
  - `geo3d_dump_capture_stream` / "Dump COP stream"
  - `GEO3D_SCAN_FALLBACK_MAX` and `is_sane_float`
  - the set_window event ring in `cop.h` (`g_geo_win`, `g_win_events` and the two MMIO taps that filled them), which nothing has read since the scanner went
- **Unused MCP commands**: `dump_face_uv` and `dump_tex_stats`, with `json_u32hex`.
- **The instruction trace window** (`trace_window.h`), which nothing opened, and
  its `trace_record` call in the i960's step loop.
- **Smaller dead code**: `overlay_host_loaded`; in `m68k_exec.h`, an unused
  `bit8` / `sub6` and an empty `if`.

**Proof the board did not move:** `ab-builds --sound` against master's binary is
IDENTICAL at frames 600, 1800 and 3600, sound board included. The ctests pass,
and so does `grade-all --stage 0`.

## Kept on purpose

- `ps3ui_canvas_free` / `ps3ui_image_free`: unreferenced, but the destructors
  of a public pair.
- The `web_*` exports: called from `web/site/*.js`, not from C.
- `g_light_*`, `g_geo_flat_color`, `g_backface_cull` / wireframe: used by the
  board light, homebrew and the object viewer.
- The geo_capture ring: `dump_geo_stream` (tools/lib/m2hle.mjs) reads it.
- MCP commands no script calls (`capture_snd_finish`, `clear_watchpoint`,
  `dump_geo_list`, `dump_midi_log`, `list_watchpoints`, `netplay_*`,
  `reset_sound`, `set_geo_isolate`, `snd_watch`, `sound_codes`): each is
  a documented debug tool.

## For the owner to decide

- **The half rule (`set_camera {"zflat":0}`) and the coplanar layers.** The
  board key beats the half rule on every stage graded (above, and #247), so the
  half rule only lives on as an A/B switch. The layers serve only the object
  viewer. Removing the game-draw half rule would take out a good part of
  `geo3d.h`'s z-sort code, but it also removes the A/B. I left it.
- **Sound grading** (`snd_capture.py` → `snd_replay` → `snd_compare.py`) was
  not re-run: the capture is a 25-minute MAME run. No sound code changed, and
  `ab-builds --sound` holds the board bit-identical.
