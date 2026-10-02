# How much of m2-hle2 is spaghetti? (Pinboard #321)

A measurement of master at `66f4d4e` (2026-10-01). Nothing in `src/` was changed by it. The untangling since then is under [Progress](#progress), measured again with a corrected tool. `python3 tools/spaghetti.py` repeats it (it needs `pip install lizard`).

## The number

**About a quarter of the emulator's function code, in about 5% of its functions.** 114 of 2,136 hand-written functions hold 10,001 of 38,873 lines of code (25.7%), and each of them has more than 20 independent paths through it, which is about where a function stops fitting in one head. The real knots, over 50 paths, are 15 functions and 6.7% of the code.

| Branchiness (modified CCN) | Functions | Lines | Share of code |
|---|---:|---:|---:|
| over 15 | 189 | 13,103 | 33.7% |
| **over 20** | **114** | **10,001** | **25.7%** |
| over 30 | 52 | 6,102 | 15.7% |
| over 50 | 15 | 2,620 | 6.7% |

The median function has 3 paths. The rest of the code is small, flat functions.

## How it was measured

- **Branchiness** is lizard's modified cyclomatic complexity: one path, plus one per `if`, loop, `&&`, `||` and `?:`, with a whole `switch` counted once. Counting the switch once means a flat table of cases is not marked down for its length.
- **Lines** are lines of code inside functions, without blanks or comments. `src/` is 82,000 lines in all, but about half of that is comments, tables and declarations.
- **Left out:** the three generated files (`scsp_dsp_known.h`, `ps3ui_layout.h`, `ps3ui_fonts.h`). The instruction interpreters and command dispatchers are counted separately: `i960_step_core`, `m68k_step_core`, `sharc_exec` and their decode tables, `mcp_dispatch`, and the two command-line parsers. They are 12 functions and 7.8% of the code. They are long because the i960, the 68000, the COP's 136 commands and the command line are long, and splitting them would add jumps, not take any away. With them counted in, the headline is 33.2%.
- `tools/` (JavaScript and Python graders) and `tests/` are not scored.
- **A file-scope `_Static_assert` blinds lizard to the rest of its file.** lizard takes it for the head of a function, and from there on finds no function at all. #325 put one in `geo3d.h` at line 814, and from then on 20 of its 57 functions went unmeasured, `geo3d_mesh_layers` (100 paths) among them, which made master look better than it was. `tools/spaghetti.py` now hands lizard a copy of `src/` with every `_Static_assert` blanked out and its line count kept, so line numbers still match the source. An assert has no branches, so nothing else moves. Seven other files carry one too (`i960_exec.h`, `tile_renderer.h`, `lockstep.h`, `netplay.h`, `sfight.h`, `overlay_plugin.h`, `video_window.h`); lizard finds the same functions in those with or without the fix.

## What is *not* spaghetti

"Spaghetti" first meant control flow that jumps around: `goto`s, modules reaching into each other. On that meaning m2-hle2 is clean:

- **Layering holds.** `src/board` includes nothing from `ui/` or `net/`, only `core/`'s log, watchpoint, build switches and backup RAM. `src/net` includes neither `board/` nor `ui/`. The one upward include is `core/emu_thread.h` → `net/netplay.h`, the lockstep gate in the run loop.
- **Little copy-paste.** lizard's duplicate finder puts 4.5% of the hand-written code in repeated blocks. The biggest sources are the debugger's i960 self-test tables, the 68000's addressing modes and `geo3d.h`. Only a few blocks repeat across the four front ends (`main*.c`). On the raw tree it reports 31.6%, but that is the generated PS3 layout tables matching themselves.
- **46 `goto`s**, every one a forward jump to a cleanup, exit or shared-tail label. None jumps backwards.

## Where the tangles are

By directory, the share of code in functions over 20 paths (dispatchers excluded):

| | Lines | Over 20 |
|---|---:|---:|
| `main*.c` (four front ends) | 3,886 | 32.4% |
| `ui/` | 13,791 | 28.2% |
| `net/` | 7,572 | 25.4% |
| `board/` | 10,281 | 23.3% |
| `core/` | 2,436 | 20.6% |
| `profiles/` | 907 | 3.7% |

The worst hand-written functions, and what makes each one hard to follow:

| Paths | Lines | Function | Why |
|---:|---:|---|---|
| 157 | 346 | `geo3d_decode_model` (`geo3d.h:1295`) | 15 parameters, static scratch arrays, and a global (`g_geo3d_obj_mesh`) that silently swaps its input. Its copy `geo3d_decode_model_cached` (51 paths, also 15 parameters) repeats the setup. *Untangled (#332): see below.* |
| 132 | 414 | `main` (`main_sdl.c:758`) | Setup, the GL context, netplay and the whole event and render loop in one body. *Untangled (#334): see below.* |
| 101 | 272 | `netplay_window_draw` (`netplay_window.h:218`) | Every connection state's ImGui panel inline. |
| 100 | 165 | `geo3d_mesh_layers` (`geo3d.h:2120`) | The coplanar-layer heuristics. Since #247 only the object viewer uses them (BUBBLEGUM.md §1). |
| 84 | 162 | `mcp_cmd_capture_dl` (`mcp_bridge.h:1650`) | Option parsing, capture and JSON output together. |
| 71 | 142 | `ps3_owner_pump` (`ps3_link.h:1227`) | A port of the PS3's phase machine. It is branchy because the original is (ROOM-MATCH.md). |
| 63 | 59 | `rs_hoist_one` (`retro_shader.h:700`) | A hand-written GLSL tokenizer. |
| 57 | 174 | `rpcn_session_pump_replies` (`rpcn_session.h:981`) | One case per RPCN reply, but with logic inside each case. |
| 54 | 158 | `netplay_publish_status` (`netplay.h:1850`) | A status string built from every state. |
| 53 | 97 | `emu_slice_body` (`emu_thread.h:540`) | The run loop. Every board-time rule in CLAUDE.md lands here. |

Two other smells do not show up in the path count:

- **Globals.** There are 270 `g_` variables. Most are used by one or two files, but 22 are read from five or more. `g_active_profile` is read in 18 files, across every layer. A header-only C emulator with one board is built this way on purpose (it is how MAME's drivers looked for years), but it means a function's inputs are not all in its parameter list.
- **Long parameter lists.** 32 functions take 8 or more parameters, and the geo3d decode and draw family takes 15 to 25 (`geo3d_emit_tri_uv` takes 25). A struct would carry the ROM pointers and sizes.

## Is 25% bad?

It is ordinary for an emulator, and the parts that matter most are better than the average. The two cores that every bug is first blamed on are the other way round from what "spaghetti" suggests. `i960_exec.h` is one long switch with about 7 lines per case, and the COP handlers are ports, cited handler by handler against the firmware. The branchy code is mostly the newest work: netplay, the PS3 lobby, the front ends. That is UI and protocol state, and it grew a feature at a time. If any of it were worth untangling, the first candidates would be `geo3d_decode_model` (a struct for its 15 parameters, one copy instead of two) and `main_sdl.c`'s `main` (split setup from the loop). Both are pure refactors that `grade-models.mjs` and `ab-builds.mjs` can prove changed nothing.

## Progress

Each column is `python3 tools/spaghetti.py` with the `_Static_assert` fix, so `f1c5ee7` here is not the 66f4d4e table above: it has `geo3d_mesh_layers` and the rest of `geo3d.h` back in, and #325's draw work in.

| | master `f1c5ee7` | #332 `461cc2c` | #334 |
|---|---:|---:|---:|
| over 20 paths (the headline) | 25.7% (114 functions) | 25.6% (113) | **24.6%** (113) |
| over 30 | 15.5% (51) | 15.4% (51) | 14.4% (50) |
| over 50 | 6.7% (15) | 6.7% (15) | 5.6% (14) |
| `main*.c` over 20 | 32.3% | 32.3% | 22.8% |
| `board/` over 20 | 23.2% | 22.6% | 22.6% |

The worst ten now, for the next candidates (`geo3d_decode_model` is still first, but #332 untangled its inputs, not its body):

| Paths | Lines | Function |
|---:|---:|---|
| 151 | 328 | `geo3d_decode_model` (`geo3d.h:1346`) |
| 101 | 272 | `netplay_window_draw` (`netplay_window.h:218`) |
| 100 | 165 | `geo3d_mesh_layers` (`geo3d.h:2150`) |
| 84 | 162 | `mcp_cmd_capture_dl` (`mcp_bridge.h:1645`) |
| 71 | 142 | `ps3_owner_pump` (`ps3_link.h:1227`) |
| 63 | 59 | `rs_hoist_one` (`retro_shader.h:700`) |
| 59 | 156 | `geo3d_mesh_build` (`geo3d.h:2373`) |
| 58 | 111 | `lobby_draw` (`pad_lobby.h:329`) |
| 57 | 174 | `rpcn_session_pump_replies` (`rpcn_session.h:981`) |
| 55 | 121 | `hprof__write` (`host_prof.h:494`) |

## Untangled so far

- **`geo3d_decode_model` (Pinboard #332).** Its ROM pointers, sizes and model-table numbers are one struct, `geo3d_models_t` (`geo3d.h`), which `geo3d_models_of` (`game_render.h`) fills from the ROM set and the profile. The decoder, the cached draw, the mesh cache and both `game_render` draw functions take it in place of 13 to 22 separate arguments. The model-table lookup is one function (`geo3d_model_streams`), not three copies, and a polygon-RAM mesh is a field of the struct (`obj_mesh`), not the global `g_geo3d_obj_mesh` the caller had to set and clear around the call. Measured by `tools/spaghetti.py` against master `f1c5ee7`: `geo3d_decode_model` 157 paths and 346 lines to 151 and 328, `game_render_draw_geo_list` 15 parameters to 6, `game_render_draw_captured_models` 22 to 13; 87 fewer lines over 20 paths in all (the first count, 30, was taken with the tool blind to half of `geo3d.h`). The picture is unchanged: 3,589 of 3,589 attract frames hash identical against the build before.
- **`main_sdl.c`'s `main` (Pinboard #334).** The handheld's `main` was 124 paths over 405 lines (132 over 414 at `66f4d4e`): argument and board setup, the GL window, the sokol and renderer start, the game's render target, the netplay lobby, the event loop with the lobby's pad handling, the pacing, the two render passes, the heat guard and the `--stats` sums, all in one body sharing two dozen locals. Each is now its own function: `gl_window_open`, `gfx_start`, `game_target_make` / `_destroy`, `netplay_start`, `poll_events` (with `lobby_event` and `lobby_pad_settle`), `frame_prepare`, `render_frame`, `too_hot`, `stats_report` and `run_loop`. The stats sums are one struct (`g_st`) that the report clears in one go, the render target another (`g_rt`). `main` is 19 paths over 55 lines; the largest piece is `lobby_event` (30 over 44), the lobby's button map. Nothing is reordered: setup and shutdown run in the same order, and an early exit still returns the same code. Checked under Xvfb with llvmpipe, building the handheld before and after: `--shot` at frames 600, 1,200 and 1,800 of attract (`--stats --osd --gl-finish`) differs only in the OSD's fps digits, where two runs of the same build already differ over the whole picture, and with `--netplay` the lobby opens, closes and reopens on F1 with the same pictures. Both exit 0 at `--exit-after`.
