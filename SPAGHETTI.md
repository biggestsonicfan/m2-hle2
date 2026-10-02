# How much of m2-hle2 is spaghetti? (Pinboard #321)

A measurement of master at `66f4d4e` (2026-10-01). Nothing in `src/` was changed by it. `python3 tools/spaghetti.py` repeats it (it needs `pip install lizard`).

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
| 157 | 346 | `geo3d_decode_model` (`geo3d.h:1295`) | 15 parameters, static scratch arrays, and a global (`g_geo3d_obj_mesh`) that silently swaps its input. Its copy `geo3d_decode_model_cached` (51 paths, also 15 parameters) repeats the setup. |
| 132 | 414 | `main` (`main_sdl.c:758`) | Setup, the GL context, netplay and the whole event and render loop in one body. |
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

## Since then

**The model decoder (Pinboard #339, measured against master `3955e6c`).** The first candidate above is done. `geo3d.h` had the polygon format written out three times: in `geo3d_decode_model`, in the mesh cache's `geo3d_mesh_build`, and partly again in `geo3d_decode_model_cached` and `geo3d_decode_direct`. Now there is one of each step, and the four callers share them: the strip walk (`geo3d_strip_walk`), the face walk with its texture header and UVs (`geo3d_walk_next`, `geo3d_texhdr_read`), the palette colour, the board's facing and lighting (`geo3d_board_facing`, `geo3d_board_light`) and the face emit (`geo3d_emit_face`). A `geo3d_rom_t` carries the ROM pointers, sizes and model-table quirks that were ten parameters of every call (`geo3d_rom_of` in `core/geo_rom.h` makes one from a ROM set and its profile), and a `geo3d_paint_t` carries the colour, tile and lighting arguments of what was the 25-parameter `geo3d_emit_tri_uv`.

| | master | with #339 |
|---|---:|---:|
| `geo3d_decode_model` | 157 paths, 346 lines | 42 paths, 82 lines |
| `geo3d_decode_direct` | 47 paths, 90 lines | 30 paths, 57 lines |
| `game_render_draw_geo_list` | 15 parameters | 6 |
| `geo3d.h` | 3,065 lines | 2,947 |
| over 20 paths, all of `src/` | 9,401 lines, 24.5% | 9,183 lines, 24.0% |
| over 20 paths, `board/` | 18.5% | 16.3% |
| over 50 paths | 12 functions, 5.6% | 11 functions, 4.7% |

It emits the same bytes: `arc_bench --draw-digest` over 12,000 frames of attract is identical with the mesh cache and without it, and so is every triangle and line of all 4,405 models through both decoders under 48 settings each (matrix or none, wireframe, layers, board lighting, mode 2, flat colour, the flat key). The full decoder, which only `--no-mesh-cache` and the object viewer run, is about 7% slower (0.31 to 0.33 ms a frame on x86); the cached draw is unchanged.

Still open, in the order they would pay: `main_sdl.c`'s `main` (setup, then one function per loop stage: events, netplay, pacing, draw), `mcp_cmd_capture_dl` (parse options, capture, write JSON as three functions), and `netplay_window_draw` (one function per connection state). `geo3d_scan_displaylist` and `geo3d_lookup_build` still take the ROM as loose parameters and could take a `geo3d_rom_t`.
