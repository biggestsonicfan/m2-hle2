# The documents

Every document in this repository, grouped by what it is for, with a line on each. The front
page, [README.md](../README.md), is for players; everything here goes deeper.

## Using the emulator

- [FEATURES.md](FEATURES.md) — everything the emulator offers, build by build: the games, the
  Console and Arcade versions of Sonic the Fighters, online play, controls, picture filters,
  recording and streaming, the debugger and automation.
- [packaging/libretro/INSTALL.md](../packaging/libretro/INSTALL.md) — installing the core in
  RetroArch (Windows, Linux, macOS, Android) and on ROCKNIX handhelds.
- [packaging/libretro/README.md](../packaging/libretro/README.md) — the RetroArch core: its
  options, its netplay, how it is built.
- [packaging/rocknix/README.md](../packaging/rocknix/README.md) — the standalone handheld build,
  no longer published (the RetroArch core replaced it): installing it, its options, and how it is built.

## How the emulator works

- [TECHNICAL.md](TECHNICAL.md) — the technical overview: the project's goal, what works, the
  source layout, netplay, picture filters, capture mode, the raw A/V stream and its wire format,
  overlay plugins, how the project evolved and the role of AI in it. This was the front page.
- [CLAUDE.md](../CLAUDE.md) — the rules for working on the code, and a table of where the facts
  are. It stays at the root, where Claude Code loads it.
- [invariants/](invariants/) — the load-bearing invariants, one file per subsystem (the i960, the
  COP, the polygon decoder, the sound board, netplay, ...): facts reverse-engineered or debugged
  out of the hardware that appear in no datasheet. Read a subsystem's file before changing it.
- [build.md](build.md) — building every frontend, the unit tests, the graders and the STF
  disassembly.
- [SCSP.md](SCSP.md) — the sound chip: what the SCSP does, what STF's sound driver uses of it,
  why the emulator runs it a voice at a time, and the driver ported to C (`--sound-hle`).
- [SLICE-CLOCKS.md](SLICE-CLOCKS.md) — how the sound board is charged against the i960's frame,
  and the alternatives not taken. Superseded by the vblank on the cycle clock (Pinboard #253);
  kept as the history.
- [PERF-PROFILE.md](PERF-PROFILE.md) — where the emulator spends its time on x86 Linux, from
  `perf` with inline attribution (Pinboard #399): the bench, a 60 fps stream and the board alone.
- [NATIVE-REPLAY.md](NATIVE-REPLAY.md) — Sonic the Fighters' and Sega Rally's own replay
  formats, decoded, and how the emulator saves and plays its replays through the game's own code.
- [MCP_GUIDE.md](MCP_GUIDE.md) — the automation bridge (`--mcp`): the protocol and every
  command, from registers and memory to netplay, the object viewer and overlay swaps.

## Online play

- [ROOM-MATCH.md](ROOM-MATCH.md) — the PS3 release's Room Match and online protocol, read out of
  its EBOOT: the rules that rooms of eight and cross-play with the PS3 follow.
- [VIRTUAL-ON-PS3.md](VIRTUAL-ON-PS3.md) — the PS3 release of Virtual-On read in Ghidra: its
  menus, and the plan that links two cabinets over RPCN.
- [WEB-PORT.md](WEB-PORT.md) — the browser build at arcade.sonicthefighte.rs: what the browser
  takes away, what was decided, and what was measured.
- [WEB-NETPLAY.md](WEB-NETPLAY.md) — online play in the browser build, and cross-play with the
  desktop builds.
- [web/gateway/README.md](../web/gateway/README.md) — the WebSocket gateway that lets a browser
  reach RPCN, and how it is deployed.

## Other ports of the games

What Sega's own ports and others' rebuilds say about the board. Maps, not oracles: MAME stays
the judge.

- [MODEL2A-TGP.md](MODEL2A-TGP.md) — what the Sega Rally recomp teaches about Model 2A and its
  TGP coprocessor.
- [SEGA-RALLY-PS2.md](SEGA-RALLY-PS2.md) — the PS2 release's C port of Sega Rally, read for the
  `srallyc` profile.
- [GEMS-COLLECTION.md](GEMS-COLLECTION.md) — how Sonic Gems Collection (GameCube, PS2) runs
  Sonic the Fighters: its file format and its executables.

## Testing and grading

- [tools/README.md](../tools/README.md) — the grading harness: every grader, what it measures
  the emulator against (MAME, the STF explorer, the ROM's own rules), and what it cannot measure.
- [tools/ps3ui/README.md](../tools/ps3ui/README.md) — how the PS3-style menus are generated from
  the player's own PS3 data and graded against the original.
- [tools/scsp_mednafen/README.md](../tools/scsp_mednafen/README.md) — the SCSP held against
  Mednafen's, sample by sample, where MAME cannot be the judge.

## Audits

Snapshots of the code at one commit, each with what came of it.

- [AUDIT.md](AUDIT.md) — every grader run against MAME on one build (Pinboard #246), and the
  dead code taken out after.
- [BUBBLEGUM.md](BUBBLEGUM.md) — the places that patched a symptom where the board has a simpler
  rule (Pinboard #245). Most are fixed; the file says which.
- [NOCLIP-SYNC.md](NOCLIP-SYNC.md) — where m2-hle2 and the STF explorer ([noclip](https://github.com/biggestsonicfan/noclip)) differ, and
  which side is right (Pinboard #298).
- [SPAGHETTI.md](SPAGHETTI.md) — how much of the code is tangled, measured, and the untangling
  since (Pinboard #321).

## History

Kept for the record. Where they disagree with [invariants/](invariants/), the invariants are current.

- [PROPOSAL.md](PROPOSAL.md) — the original architecture proposal (June 2026).
- [IMPLEMENTATION-DRAFT.md](IMPLEMENTATION-DRAFT.md) — the dependency-ordered plan this
  repository was rebuilt from.
