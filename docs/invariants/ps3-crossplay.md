# Cross-play with the PS3 port (`net/ps3_link.h`, `rudp.h`, `rpcs3_signal.h`)

Part of m2-hle2's load-bearing invariants, indexed in [CLAUDE.md](../../CLAUDE.md): facts reverse-engineered or debugged into the emulator, in no datasheet. Do not re-derive them.

PS3 Sonic the Fighters (NPUB30927) is Sega's own i960 arcade emulator in a PPU wrapper, and it plays
online over RPCN inside RPCS3. On the official server (np.rpcs3.net) m2hle plays its rooms by its
rules; the community server keeps m2hle's own protocol. Every rule below was read out of the PS3
EBOOT (Ghidra) and checked against a PS3-vs-PS3 match captured through RPCS3's `sys_net_dump` log.
`tools/ps3-audit.py` holds an RPCS3 log against m2hle's `--net-ps3-wire` log.

- **The layers are RPCS3's, not Sony's.** RPCS3 signaling (75-byte `SIGN` v3 packets on vport 0,
  subset 1) is what gives the PS3 game its "Established"; then Sony RUDP runs LLE (librudp.sprx) under
  RPCS3's 6-byte P2P header (vport 1). Signaling and game traffic must share one socket.
- **RPCS3 marks us connected only when it gets a CONNECT_ACK for ITS CONNECT**, and its RTT narrows to
  u32 and throws on a garbage echo: echo `timestamp_sender` in CONNECT_ACK/PONG and
  `timestamp_receiver` in CONFIRM, exactly.
- **Every RUDP connection is a simultaneous open on three channels** (mux vport 1/2/3). Channel 1 is
  reliable but UNORDERED (syn_flags `0x0201`), 2 and 3 unreliable (`0`); a PS3 resets a SYN whose
  flags differ. There is no FIN: a PS3 closes with RST reason 0.
- **Use librudp's retransmit clock (1 s, doubling to 16 s).** A PS3 delays and bundles its ACKs; a
  250 ms clock resent 24 messages a match that had already arrived. A PS3's own "resend" 30 ms after a
  segment, with options set, is it piggybacking an ACK on its outstanding segment -- harmless.
- **The lockstep is the PS3's TaskSyncIo, ported state for state** (`ps3_sio_*`). Frame n plays ring
  entry n of both sides; the first frame played is the room's delay, the same on both machines because
  it comes from the round trip the owner writes into the room. It ticks at 60 Hz of WALL CLOCK: compare
  `now > next_tick + 100 ms`, never subtract -- the subtraction underflowed, ticked on every pump,
  sampled at twice real time, and the PS3 abandoned every match 25 s in.
- **A PS3 drops a SyncStart that arrives before its lockstep task is running**, yet its network layer
  still answers it. Our board reaches its forced START sooner, so our SyncStart went unheard until the
  5 s resend. Send ours again when theirs arrives (`ss_echoed`).
- **When our board falls more than `delay` frames behind its own sampling, the PS3's rule stops
  sampling until it catches up** -- which at 60 Hz pacing it never does. Run unpaced while behind
  (`ps3_link_hurry`, through `netplay_catching_up`).
- **The match's settings are the room's.** At match start the PS3 writes the room rules through its
  table at EBOOT 0x377AB0 into the game assignments (0x59C340 / 0x1D03340): rounds, energy 1, time,
  the flag byte (AUTOMATIC always on; game type A-D sets HYPER MODE and BARRIER RESET), barrier 5.
  Stage = seed % 9 (0xAF84). A board that is not in attract is rebooted first (FUN_000ac554).
- **m2hle can own a PS3 room too** (`ps3_owner_pump`, ported from `np_session_update_room_phase` 0xBF258 and held against an RPCS3 log of a PS3 owner). Three things are not obvious:
  - **Step only on the echo.** The PS3 owner advances only when the room's phase, as the server sent it back, equals its own; RPCN echoes SetRoomDataInternal to the writer too. One write in flight at a time, or phases reach members out of order.
  - **Room flags travel with the phase**: phase 1 closes and hides the room (flagFilter/flagAttr `0x50000000`), phase 4 reopens it (filter `0x50000000`, attr 0).
  - **Room byte 5 changes the PS3's lockstep** (`SyncIo_Init_rings` 0x6E67C): `0x40` (more members than fighters) means a small packet every 2 frames, a 60-frame packet every 12 straight to the watchers, one frame more delay, and the match ends at VIC_DSP when CTRL_TIMER is 60 (hook 0xE93C), not at VIC_INT. `0x80` (a fighter has no link) sends inputs through the owner with header flag `0x100`. A member that ignores these plays a different first frame from the PS3.
- **The peer-to-peer socket is bound before the TLS connect** (a taken port fails at once), so the session takes its own `net_startup` reference first and holds it until `rpcn_session_stop`. Without it, a sign-in with no MCP bridge or A/V stream up failed with `could not bind UDP (10093)` (WSANOTINITIALISED) on Windows: every test ran with the bridge, which had started Winsock for it.
- **RPCS3's RPCN password is a derived key**, not what the player typed: PBKDF2/SHA3-256 as 64 hex characters (`rpcn_settings_dialog.cpp` `derive_password`). `netplay_config_t.password` must hold 64 characters; at `char[64]` it lost the last one and the server said "wrong password".
- **The two boards still compute different fights** from the same inputs: the PS3's COP is
  single-precision C with FMA, its `rand` an MT19937, and it has no sound CPU. So its result can come
  before ours: the member's post-match update (flags 0xE0, place in line) has to follow our own result
  whichever order they arrive in.
