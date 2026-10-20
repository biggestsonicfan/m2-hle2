# Backup RAM (`core/backup_ram.h`, board-level)

Part of m2-hle2's load-bearing invariants, indexed in [CLAUDE.md](../../CLAUDE.md): facts reverse-engineered or debugged into the emulator, in no datasheet. Do not re-derive them.

The board's battery-backed SRAM at `0x01D00000` (settings, region, coin setup, bookkeeping). The first 16 KB is kept: MAME's `nvram/<set>/backup1`, byte for byte, so the two can trade it.

- **Only a player's own boots keep it.** On by default for the desktop window (`%APPDATA%\m2hle2\nvram`, `~/.config/m2hle2/nvram`), the handheld (sdl3, same folder), the web build (localStorage `m2hle2.nvram.<set>`, flushed on `pagehide`; off for `?script=` runs) and libretro (`RETRO_MEMORY_SAVE_RAM`, the frontend's `.srm`). Off for `--headless` and `--kiosk`, so the graders, the tests, `det_digest` and the fly boot blank as before; `--nvram-dir` / `--no-nvram` override.
- **A netplay session is a cold boot that must be identical on both machines, so it detaches the battery** (`backup_ram_detach` before every netplay reset, and on a PS3 match before the rules hook writes the room's settings into it). The detach flushes the player's copy first; nothing a session does reaches the file. The player's comes back when the room empties (`netplay_restart_alone` → `backup_ram_reattach`) or with the next ROM load.
- **A reset keeps it** (`mem_init` saves and restores it while attached), as the battery does. That includes the bridge's `board_reset` and the idle hold, which are not sessions.
- **Homebrew on a stock set gets its own file**, `<set>-<fnv32 of the program>`, so it neither reads nor clobbers the game's.
- The image is checked once a minute and written only when it changed, then flushed at exit. STF bumps a counter at `0x1D03319` every few seconds for as long as it runs, so a shorter check is a write every few seconds, which wears out a handheld's SD card.
