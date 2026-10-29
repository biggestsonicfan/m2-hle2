# CLAUDE.md

m2-hle2 is a **Sega Model 2 arcade emulator** in C11 (Dear ImGui via cimgui, Sokol). *Sonic The Fighters* (STF) is the first target, where most of the reverse engineering was done, but it is built for the whole Model 2 catalogue: most "STF bugs" are board bugs (i960, COP, tiles) that every game shares.

Every session and subagent reads this file in full, so it holds only the rules and where to look. **Keep it short: add new findings to the topic files below, never here.** The reference prior project is `ai\m2-hle` (read from it, don't import wholesale); `stf-hle` is outdated. [PROPOSAL.md](PROPOSAL.md) is the original plan, historical.

## Board vs. game layering (critical)

- **Board layer**: everything every Model 2 set shares (i960, bus, COP, tiles, 3D decoder, sound, threading), in shared `.h` modules.
- **Game profile**: one ROM set's hook table, input map, ROM list + CRC32s and quirks, a `game_profile_t` picked by the zip's basename (`profile_for_rom_set`).
- **Default to the board layer.** A bug seen in STF is board-level until another game's ROM proves otherwise.

## Where the facts are

The load-bearing invariants (reverse-engineered, in no datasheet, do not re-derive) are in [docs/invariants/](docs/invariants/), one file per subsystem. **Read a subsystem's file before you change it.** A comment citing "CLAUDE.md, <title>" means that file.

| Subsystem | File | Code |
|---|---|---|
| i960 CPU | [i960.md](docs/invariants/i960.md) | `i960*.h` |
| COP (SHARC firmware ports) | [cop.md](docs/invariants/cop.md) | `sharc*.h`, `cop.h` |
| 3D decoder, z-sort, fill | [polygon-decoder.md](docs/invariants/polygon-decoder.md) | `geo3d.h`, `game_render.h` |
| Memory bus | [memory-bus.md](docs/invariants/memory-bus.md) | `memory.h`, `irq_timer.h` |
| Tile renderer | [tile-renderer.md](docs/invariants/tile-renderer.md) | `tile_renderer.h` |
| Sound board (68000, SCSP, UART) | [sound-board.md](docs/invariants/sound-board.md) | `sound*.h`, `scsp.h`, `m68k*.h` |
| Model 1 I/O + sound boards (Daytona, Virtua Cop) | [model1-boards.md](docs/invariants/model1-boards.md) | `model1io.h`, `model1_sound.h`, `z80.h` |
| HLE hooks, STF profiles | [hle-hooks.md](docs/invariants/hle-hooks.md) | `profiles/`, `hle_hooks.h` |
| Homebrew on a game's board | [homebrew.md](docs/invariants/homebrew.md) | `sfight_homebrew.h` |
| Prepared profiles, model tables | [prepared-profiles.md](docs/invariants/prepared-profiles.md) | `registry.h`, `model_table.h` |
| Threading | [threading.md](docs/invariants/threading.md) | `emu_thread.h`, `sound.h` |
| Backup RAM | [backup-ram.md](docs/invariants/backup-ram.md) | `backup_ram.h` |
| Savestates, rewind | [savestates.md](docs/invariants/savestates.md) | `savestate.h`, `rollback.h` |
| One-way follow | [follow.md](docs/invariants/follow.md) | `follow.h` |
| Replays | [replays.md](docs/invariants/replays.md) | `replay.h` |
| Tool-assisted play (TAS) | [tas.md](docs/invariants/tas.md) | `tas.h`, `tas_window.h` |
| Netplay / RPCN | [netplay.md](docs/invariants/netplay.md) | `src/net/` |
| Rooms of more than two | [rooms.md](docs/invariants/rooms.md) | `net/room.h` |
| PS3 cross-play | [ps3-crossplay.md](docs/invariants/ps3-crossplay.md) | `ps3_link.h`, `rudp.h` |
| PS3 menus | [ps3-menus.md](docs/invariants/ps3-menus.md) | `src/ui/ps3ui*.h` |
| GGPO rollback | [ggpo.md](docs/invariants/ggpo.md) | `emu_ggpo.h`, `ggpo_lobby.h` |
| Raw A/V out | [av-out.md](docs/invariants/av-out.md) | `av_stream.h`, `av_capture.h` |
| Overlay plugin swap | [overlay-swap.md](docs/invariants/overlay-swap.md) | `overlay_host.h` |
| Picture filters | [picture-filters.md](docs/invariants/picture-filters.md) | `post_shader.h`, `retro_shader.h` |
| Game scripts (Lua) | [scripts.md](docs/invariants/scripts.md) | `m2lua*.h`, `m2hle-cheats.js` |

Building, the tests, the graders and the STF disassembly: [docs/build.md](docs/build.md). Grader details: [tools/README.md](tools/README.md). The other top-level `*.md` files are per-topic notes (WEB-PORT, SCSP, ROOM-MATCH, MCP_GUIDE, ...); open them when the topic comes up.

## Rules that cut across subsystems

- **MAME is the oracle** (the `mame` skill). Compare with `--profile sfight --region japan`: the default profile is the Console one, which plays attract differently, and it boots USA. `tools/lib/m2hle.mjs` passes both.
- **Prove board-identical changes**: `det_digest` (frame by frame) or `tools/ab-builds.mjs`. A change to what the i960 can read (timing, sound interrupts, hooks that skip code) is a `NETPLAY_PROTO_REV` bump.
- New state a board reset must clear: `tools/grade-reset.mjs`. A new board part goes in `savestate.h`, and rollback then has it too.
- Board maths: no host maths in COP handlers (`sharc_fw_*`), no float-to-word `memcpy` (`sharc_float_to_bits`, `i960_nan_result`), no FMA. Native, wasm and ARM must compute the same frames.
- Interrupts go through `hle_interrupt`, never `hle_call`. No hook waits for a vblank or a timer.
- `mem_init` never frees bus memory; rings and regions live for the process. Call `sound_settle()` before touching `g_sound`. Unlock the emu mutex before sleeping.
- An unknown COP command whose "opcodes" look like floats means an argument count is missing, not a new op. Log unknown COP commands and unhandled MMIO at WARN.
- ROMs: use `$ROMS_DIR` in place, never copy a zip, never run from the ROM folder. Scripted runs use `--headless --mcp --rom <zip> --run`.
- Netplay tests only on the container's RPCN (127.0.0.1:31313) or a local ggpo-server. A scripted session never writes memory or halts the board.
- No Sega asset goes in the repo (PS3 menu art, decoded dumps), and no Gems or Ages C: it lives in the `vendor/gems-c` and `vendor/ages-c` submodules.

## Conventions

- Every module is a **header-only `.h`**, except the entry points (`main*.c`), the sokol implementation units, vendored `miniz.c`, `src/ui/mem_edit.cpp` (imgui_club's C++ MemoryEditor behind a C handle) and `src/net/ggpo_port/ggpo_port.cpp` (GGPO's C API). Don't split modules into `.c`/`.h`, and keep C++ inside those two files.
- **Desktop-only debugger hooks in the board go behind `M2HLE_DEV_TOOLS`** (`core/build_features.h`: 1 on desktop and tests, 0 on sdl3, libretro and web). Test an "is it armed" helper (`bp_armed`, `wp_armed`, `dl_active`, `sndcap_on`), never the raw field; keep the state defined either way (det_digest builds in the web tree). Gate platform code with `_WIN32` / `__linux__` / `__EMSCRIPTEN__`.
- Addresses and sizes are `uint32_t`; sign extension is per instruction.
- Locking: `emu_mutex_lock()` / `emu_mutex_unlock()` (`thread_mutex.h`). Logging: `log_msg` or `LOG_INFO` / `LOG_WARN` / `LOG_ERROR` (`log.h`).
