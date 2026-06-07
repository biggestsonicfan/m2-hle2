# m2-hle

A cross-platform Sega Model 2 arcade HLE emulator in C11. *Sonic The Fighters* is the reference game; the architecture targets the full Model 2 catalogue (2 / 2A-CRX / 2B-CRX / 2C-CRX) from day one.

## Status

Early rebuild. **Phase 0 (host skeleton)** and **Phase 1 (infrastructure + oracle)** are up:
the sokol + cimgui (dear_bindings) window builds and runs, and the board-level headers
(`constants.h`, `log.h`, `game_profile.h`) are in place. The MAME ground-truth oracle lives
in the sibling `claude_mame/` repo; the in-emulator MCP bridge protocol is documented in
[MCP_GUIDE.md](MCP_GUIDE.md).

See [IMPLEMENTATION-DRAFT.md](IMPLEMENTATION-DRAFT.md) for the authoritative build order
(it supersedes PROPOSAL.md where they disagree) and [CLAUDE.md](CLAUDE.md) for load-bearing
invariants reverse-engineered from the prior implementation.

## Stack

C11 · Dear ImGui via [cimgui](https://github.com/cimgui/cimgui) · [Sokol](https://github.com/floooh/sokol) · [ImGuiFileDialog](https://github.com/aiekick/ImGuiFileDialog) · [miniz](https://github.com/richgel999/miniz) · CMake ≥ 3.20 · Emscripten (WebGL2) for the web build.

## Build

```
cmake -S . -B build
cmake --build build --config Release -j
```

Output: `build/Release/demo` (or `demo.exe`). No automated tests — validation is interactive through the ImGui debug windows. The active game profile is resolved at ROM-load time by matching file CRC32s.

## Layout (planned)

- `demo.c`, `miniz.c` — the only `.c` translation units
- `*.h` — header-only modules (CPU, memory bus, COP, tile/3D renderers, HLE hooks, UI windows)
- `vendor/` — third-party sources
- `profiles/` — `game_profile_t` entries (one per ROMset)
