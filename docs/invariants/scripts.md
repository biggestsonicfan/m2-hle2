# Game scripts (M2emulator's Lua, Pinboard #958)

Code: `src/core/m2lua.h` (the runtime), `src/ui/m2lua_ui.h` (the desktop's Script
menu and drawing), `src/ui/m2lua_mcp.h` (the bridge's `script_*`),
`web/site/m2hle-cheats.js` with `web_cheats_*` in `src/main_web.c` (the web's
Cheats tab), `lr_lua_*` and `retro_cheat_*` in `src/main_libretro.c` (the
libretro core), `scripts_*` in `src/main_sdl.c` (the handheld). Lua 5.5.1 is the `vendor/lua` submodule, built from `onelua.c`.
Test: `m2lua_test`.

## What M2emulator did (read off EMULATOR.EXE with rea + Ghidra)

- One script per ROM set, `SCRIPTS\<set>.lua`, loaded once at game load. The
  chunk runs, then the global `Options` table is walked ONCE: `name` (default the
  key), `values` (default `{"Off","On"}`), whether `runfunc` / `changefunc` are
  functions. Options added later are never seen.
- Callbacks: `Init` after the load, `Reset` after a board reset, `Frame` after each
  frame, then `runfunc(value)` for every option whose value is not 0, then
  `PostDraw`; `End` at unload. A menu change sets `Options.<key>.value` and calls
  `changefunc(value)`.
- `AND OR XOR NOT SHL SHR` round each argument, work on the low 32 bits and return
  an UNSIGNED 32-bit integer; `SHR` is logical, the count is mod 32. Lua 5.5's own
  `& | ~ << >>` work beside them, but on 64-bit integers: mask with `& 0xFFFFFFFF`.
- `Romset_*` areas are M2emulator's region numbers 0-11, mapped to ours through
  `{0,1,2,3,4,5,6,-1,-1,-1,7,-1}` (-1: nothing; reads give 0).

## Rules

- **The Lua state is touched only with the emu mutex held** (frame edge, reset,
  load, option change, eval), M2emulator's critical section. PostDraw's list goes
  to the window through `draw_mutex` (`m2lua_take_draw`).
- **A script never runs while a session owns the board**: `emu_script_may_run()`
  (`emu_thread.h`) is false during RPCN, GGPO, replay playback, and while
  following or leading a follow. Then the script is suspended and every ROM byte
  it patched is put back (the undo log, `m2lua_unpatch`). When the session ends
  it runs again FROM ITS START (Init re-patches), with the options it had. So
  a script changes nothing the i960 can read during a session, and needs no
  `NETPLAY_PROTO_REV` bump. Proved 2026-10-16: one client with a script writing
  work RAM every frame, one without, 1100 frames on the local RPCN, no desync.
- Bridge `script_option` / `script_eval` / `script_reload` are refused while a
  session owns the board.
- `Romset_Patch*` writes through `m2lua_patch`, which logs the old byte. Never
  write `g_m2lua.rom` regions any other way.
- The desktop reads `<dir>/<set>.lua` (`--scripts DIR`, default `scripts/` in the
  cwd or beside the executable; `--no-scripts`). The web has no files: the Cheats
  tab's text goes in with `m2lua_load_source` and is kept in localStorage
  (`m2hle.cheats.<set>`, `.on`, `.opts`). Given during a session, the text waits,
  unrun, until the session ends.
- `require("model2")` and `require("common")`, which every M2emulator 1.1a
  script starts with, find built-in stand-ins (`m2lua_builtin_searcher`, the
  last of `package.searchers`) when the scripts folder has no such file: the
  memory-map constants, `HEX4`..`HEX32` and the `JOYn`/`JOY_*` numbers, written
  here with M2emulator's names and values. A file in the folder wins. The web
  has no folder and no `package.path`/`cpath`: Emscripten's `fopen` stub opens
  any name and then fails the read (ENOSYS), and Lua's file searcher stops
  require there instead of trying the next searcher (Pinboard #976: srallyc.lua
  in the Cheats tab stopped at "cannot read scripts/model2.lua").
- The web draws text only (`web_cheats_draw`, on a canvas over the picture);
  `Video_CreateSurfaceFromFile` has no file to read there.
- `Model2_Set*` (widescreen, stretch): the sdl3 frontend and the web draw them (`m2lua_wide_apply`, `ui/m2lua_wide.h`, into `g_game_wide`), the others keep and report them. The web's letterbox, render-scale target and filter source take the picture's width (`web_picture_width`). Widescreen is 16:9 at 384 lines, 682 columns: the 3D is Hor+ (same focal lengths, 93 more columns each side; a window that spans the board spans the picture, a narrower one stays put), the tile layers stay 496 wide in the middle. The board composes both chips into one layer behind the 3D and one in front, so `StretchALow`/`BLow` stretch the one behind and `AHigh`/`BHigh` the one in front. Off with no script, `--no-scripts` or a session holding scripts off. Drawing only; the game culls by its own 4:3 view, so things can pop in at the sides, as in M2emulator.

## The libretro core (Pinboard #967)

- The script is `m2hle/scripts/<set>.lua` in RetroArch's system folder (core
  option "Game scripts", read at load). `require` finds `model2.lua` and
  `common.lua` there too (`m2lua_set_path`), so M2emulator 1.1a's `scripts`
  folder is copied in as it is; the repo ships neither file. Checked with
  1.1a's own scripts on srallyc: srallyc.lua's Init and Frame run each
  frame, and daytona.lua (run as srallyc.lua, Daytona does not boot yet)
  gives "Infinite Time: On" in the .cht, whose runfunc holds RAM 0x5010D0 at
  3200 once the cheat is on.
- RetroArch has no menu of a core's own, so **the options are RetroArch
  cheats**. At load the core writes `<set>.cht` beside the script
  (`m2lua_cht_text`; rewritten only when it would differ): one cheat per value
  other than the option's default, code `<key>=<value>` (index or value name,
  several joined by `+`), handler 0 (the core's). The player opens it in Quick
  Menu > Cheats > Load Cheat File. RetroArch calls `retro_cheat_reset`, then
  `retro_cheat_set` for each cheat that is on: they fill a wanted value per
  option (default when no cheat names it), applied with `m2lua_set_option` at
  the next frame edge the script may run. The .cht's RAM-poking cheat types
  (RetroArch's own handler) are not used.
- `lr_lua_may_run`: `emu_script_may_run()`, and no session (`lr_sound_may_go`:
  RetroArch netpacket, RPCN, GGPO), and no savestate of RetroArch's rollback
  netplay context (`RETRO_SAVESTATE_CONTEXT_ROLLBACK_NETPLAY`) in the last 120
  frames. Rewind and run-ahead savestates do not stop it.
- PostDraw's text is drawn over the game with the PS3 menus' fonts
  (`ps3ui_text`, board pixels scaled to the frame, bold = the title font);
  images are not.

## The handheld frontend (sdl3, Pinboard #972)

- `src/main_sdl.c` (`scripts_*`) runs `<dir>/<set>.lua` like the desktop:
  `--scripts DIR` (default `scripts/` in the working directory, else beside the
  executable: `/storage/.local/share/m2hle/scripts` on ROCKNIX), `--no-scripts`.
  It links `lua55` and needs `vendor/lua` like every other frontend.
- No menu: `--script-option CODE` sets the options with the libretro core's
  cheat codes (`m2lua_cheat_code`, `<key>=<value>`, `+` joins), applied once
  after Init. The hooks are `m2lua_frame_hook` / `m2lua_reset_hook`, so the
  RPCN hold-off is `emu_script_may_run()`, as on the desktop.
- PostDraw's text is drawn with sokol's 8x8 debug font, scaled to the font's
  size. Its images (`Video_CreateSurfaceFromFile`, PNG through
  `vendor/stb`'s stb_image) are sokol_gl quads with the image's alpha, at the
  image's size in board pixels scaled with the picture, under the text
  (`scripts_draw`, Pinboard #974). At exit it prints `script <path>: N frames`.
- No script in M2emulator 1.1a draws an image; checked with its srallyc.lua
  plus a 96x48 PNG drawn at board (20, 20): at attract frames 600 and 1200 in
  a 1280x720 window it covers window pixels (212, 38)-(391, 127), with the
  game showing through its transparent part, and is absent with
  `--no-scripts`; the widescreen widths (1278 vs 930) are unchanged.
- Checked on srallyc with M2emulator 1.1a's `scripts` folder unmodified:
  srallyc.lua's Init at load and Frame 1800 times in 1800 frames; daytona.lua
  (required by a srallyc.lua that draws its state) with
  `--script-option timecheat=1`: the runfunc held 0x5010D0 at 3200.
