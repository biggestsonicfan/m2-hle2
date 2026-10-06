# Dreamcast stats read out of a video (Pinboard #511)

`dc-stats.mp4` (in `ai/Sonic Gems Collection`, not committed) is 97 s of a
`HUD=prof` disc running Sonic the Fighters under Flycast. The source is a
1280x720 60 fps capture of the 640x480 picture. `tools/hud_ocr.py` reads the HUD
back out of it:

```sh
python3 -I dreamcast/tools/hud_ocr.py dc-stats.mp4 \
    --log dc-stats.log --jsonl dc-stats.jsonl --csv dc-stats.csv
```

- `dc-stats.log`: the HUD rows as they were on screen. There is one block for
  each 2 s redraw (48 blocks) and each block is headed by its time in the video.
- `dc-stats.jsonl`: the same blocks with every field parsed, one record a line.
  `t` and `t_end` are the window's video seconds and `frames_read` is how many
  frames were voted.
- `dc-stats.csv`: the records as a table.

All 48 windows parse. Spot checks of single frames at 9.5, 20, 51 and 90 s
match the log character for character.

## What the fields are

The rows are `main_dc.c`'s stats rows and `dc_prof.h`'s profile rows (6-10).
They are in the order the build had when the video was taken. Since then,
row 18 puts `pk` first; the tool reads the old order. Rows 3-5 and 11-15 had the
game over them in this capture and are not read.

| row | fields |
|-----|--------|
| 0 | `frame`, `fps`, `shown_fps`, `slice_ms`, `3d` (ms decode + draw) |
| 1 | `loads`, `load_ms`, `refills`, `evictions`, `pinned`, `rom_writes`, `read_errors` (the pager) |
| 2 | `profile`, `gems`, `cache_kb`, `heap_kb` |
| 6 | `stall_d_pct`, `stall_i_pct` (SH-4 cache-miss stalls), `pvr_rnd_ms`, `pvr_reg_ms`, `vbl` |
| 7, 8 | `g_<group>`: % of samples per group of `tools/dc_profmap.py` |
| 9, 10 | `top1`..`top6` and their %: the hottest symbols (first 16 characters) |
| 16 | `i960_ms`, `cop_ms`, `blk_pct`, `aot_pct`, `steps` |
| 17 | `tiles_ms`, `scan_ms`, `sort_ms`, `meshes`, `mesh_builds`, `mesh_hits`, `mesh_cl`, `arena_kb` |
| 18 | `tris`, `runs`, `full`, `tex`, `tex_new`, `tex_drop`, `tex_fail` |
| 19 | `snd`, `codes`, `unknown`, `bgm`, `ring_kb`, `underruns` |

The screen shows only 53 of a row's up to 84 characters. Fields to the right of
that were never on screen and are missing. A field that runs into the edge may
be cut short, and the record names it in `edge`. In this video that is always
`heap_kb` and usually `vbl`, sometimes a row-9 symbol and its %.

Symbol names are snapped to the nearest identifier in `dreamcast/` and `src/`.
Number cells take only digits.

## What it shows (means over the 48 windows)

- 10.6 fps (5.3-19.3). The slice is 49 ms, and nearly all of it is the i960
  (49 ms, of which the COP is 5.5).
- The AOT is off in this build (`aot 0`, `aot_pct 0`). Gems' C takes 7% of the
  samples.
- Time by group: i960 interpreter 26%, geo 18%, draw 19%, BIOS, where the GD-ROM syscalls run, 13% (up to 59%
  during loads), tiles 3.5%.
- Hottest symbols: `geo3d_decode_mod…` 15% (`_model` or `_model_cached`: the HUD keeps 16 characters), `emu_slice_body` 10%, `ib_run`
  8%, `dp_face_colour` 5%, `dp_frame` and `ib_build` 4% each,
  `s24_draw_line` 3%.
- SH-4 stalls: 31% of cycles on the data cache and 13% on the instruction
  cache.
- PVR: render 12.1 ms, registration 4.5 ms.
- Draw: 2,400 triangles a frame, scan 34 ms.

## Where the data-cache stall goes (Pinboard #513)

The stall counters say 31% of the console's cycles wait on the data cache,
but not where. Flycast has no cache model that runs this port (its
STRICT_MODE one stops on a blocked exception after a few seconds), so the
look was taken with a passive one: the interpreter's loads, stores, `pref`,
`movca.l` and `ocb*` fed to a 16 KB direct-mapped, copy-back tag model with
32-byte lines (and an 8 KB instruction cache), counting misses and
write-backs by PC and by RAM line. It changes nothing the program sees.
Over 383 attract-fight frames at idea-340-dreamcast a22cc69:

- 139,000 data misses and 64,000 write-backs a frame. At the ~40 cycles a
  miss costs, that is the counters' 30%.
- The cached model decoder takes a quarter: 12.6 misses a face. Every
  64-byte packed face missed. The quad-diagonal table
  (`geo3d_split_other_way`) was three arrays exactly 32 KB apart, all on one
  cache line, so each lookup missed three times.
- KallistiOS's stereo stream split (`snd_pcm16_split`) missed on every store.
  With `snd_stream_init()`'s 64 KB, the left and right buffers are 32 KB
  apart: the same line again.

The fix:
- The split table is one array of slots.
- The face walk and the corner transform `pref` ahead.
- The sound stream's split buffers are half a cache apart.
- A model wholly outside the window skips its transform and face walk. Only
  its last flat key's carry is worked out. A check build ran both paths on
  every off-screen draw for about 6 minutes of attract mode, fight included:
  36,359 draws (the same few dozen models, frame after frame; 1.06 million
  faces), and the carry came out the same every time.

Over the same frames: data misses 139k -> 108k a frame (-23%), write-backs
64k -> 44k (-31%), instructions -5%. The decoder's misses are 36% fewer. The
console's stall % should show it.

## Re-reading another video

The cell grid is fitted to this capture: (321.9, 87.0) with a pitch of
12.8 x 25.6 in 1280x720. Use `--grid` for one framed differently.

`tools/hud_font.png` is the glyph atlas. It was learned from this video by
`--learn`, starting from a hand-read frame. A capture at another scale may need
its own atlas, made by running `--learn` once. If the HUD's formats change,
`FORMATS` in the tool must follow them.

The HUD has since become a panel of small checked lines (#518, the
README's "The HUD=prof panel"), read by `tools/hud_read.py`; `hud_ocr.py`
stays for this video and others of the old HUD.
