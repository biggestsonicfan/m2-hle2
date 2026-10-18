# Virtual-On: the PS3 port's menus, and the way to online play

Pinboard #957 (Virtual-On, part 6). The PS3 release of Cyber Troopers Virtual-On
(NPJB00321, PSN, 2012) was read in Ghidra. It is Sega's m2ftg emulator wrapper,
the same family as PS3 Sonic the Fighters (TaskSyncIo lockstep, cellRudp,
NPMatching2Session, comId NPWR04277), around the **vonj** program: the words
its hook table patches match vonj's (0x18AB4 `ld 0x5039F4`, 0x1871C
`st -> 0x5024D4`).

What came of it: the libretro core's PS3 shell runs for `vonj` with the PS3's
Arcade settings (`ps3ui_rules_von`, `vonj_apply_menu_settings`). Online Battle
opens the RPCN lobby, and a room's match links two cabinets (step 6). On the
desktop (SDL3), `--versus` plays a Versus as two m2hle on one machine (step 7).
The shell's Versus row is refused on purpose, for the reason below.

## Landmarks in the EBOOT (addresses as Ghidra loads it)

| What | Where |
|---|---|
| i960 symbol table (276 names, ROM and RAM) | 0x00441070 |
| HLE hook table `{u32 i960_pc, u32 opd}`, 127 entries, ends 0,0 | 0x00472CE8..0x004730E0 |
| Board API vtable: get_board 0x184AD0, select_board 0x184D68, step_frame 0x184DF8, reset 0x184EB8, init(board_count) 0x184FD0 | 0x004550E0 |
| Frame driver: board 0, then board 1 in two-board mode; half rate when MainMode 4 / SubMode 0x0A | FUN_00184DF8 |
| Board link: a local 0x700 memcpy between the two boards' comm RAM | FUN_00185988 |
| Write a byte into the board's memory | FUN_0018F6AC(addr, &byte) |
| Main menu: Init (item states +0x3F8..+0x402), Decide | FUN_0011DFDC, FUN_0011E5D8 |
| Arcade menu vtable / Init / Update | 0x00446A14 / 0x11BF24 / 0x11B880 |
| Versus menu vtable / Init / Update | 0x004469DC / 0x11DDC8 / 0x11B5F0 |
| Training, Main, Option, Pause, MultiRule vtables | 0x446934, 0x446A4C, 0x4423DC, 0x44732C, 0x446FE4 |
| GameMode_Set(mode, rec[7]): mode 0 Arcade, 1 Versus, 2 (online) | FUN_000CBEC4 |
| Arcade apply / Versus apply (= Arcade + 0x1D00024 = 1) | FUN_0019F0E4 / FUN_0019F28C |
| Difficulty map {3,3,0,1,2}; play-time map +8 {5,11,17} | table at 0x3D3FB0 (TOC slot 0x461DDC) |

Menu text is message ids into the PS3's data files (not in the EBOOT); the rows
were named from what the apply functions write and from the cabinet's own test
menu strings.

**Arcade menu**: five rows, maximums {4,4,3,1,1} (table 0x3D2BA8 + 0x28), the
saved values at `FUN_000E75B8() + 8`. Decide builds `{row0, !trial, 0, row2,
row1, row3, row4}` and calls GameMode_Set(0). The apply writes:

| Backup RAM | Cabinet test menu (GAME ASSIGNMENTS) | From |
|---|---|---|
| 0x1D00016, 18, 1A | PLAY TIME 1P stage 1~5 / stage 6~8 / versus: (n + 1) * 5 s, 0xFF DEATH-MATCH | rec[3] (0..2 -> 30/60/90 s, 3 -> 0xFF) |
| 0x1D0001B, 1C, 1D, 1F | MATCH COUNT: rounds to win - 1 | rec[4] |
| 0x1D00021 | GAME DIFFICULTY: 0 NORMAL, 1 HARD, 2 VERY HARD, 3 EASY | rec[0] through the map |
| 0x1D00024 | two-board versus (Versus only) | 1 |

0x1D00017 and 0x1D00019 (the penalty and last-stage times) are left alone. Rows
3 and 4 are not written to the board. Factory values (a cleared board):
16..1A = 11 0B 11 11 11 (90, 60, 90, 90, 90 s), 1B..1F = 0, 21 = 0, 34 (free
play) = 0, 28 (cabinet role) = 2 NOLINK.

The game reads these when a game starts, so writing them during the attract
is enough; no reset is needed. This was checked headless: 0x1D00016 = 5 gave a
30 s round, 0x1D0001B = 1 a ROUND 2, 0x1D00034 = 1 FREEPLAY at once.

**Versus menu** (Update 0x11B5F0): two rows, a record with match count and
play time, then `init(1)` on the board API and GameMode_Set(1).

## Why the libretro shell's Versus is refused (and Online was, at first)

A Virtual-On board is one cabinet: one player and the CPU. A versus is two
cabinets linked by their comm boards (a "Twin"). The PS3 runs **both boards in
one process**:
- Its frame driver steps board 0 and then board 1.
- The link is a memcpy of the 0x700-byte comm window between them.
- Online, it sends the pads (lockstep) and each PS3 runs both boards.

m2-hle2 runs one board: its state is in globals (g_m2bc, g_shc, g_m2comm,
g_sound, ...), and RetroArch runs one core in one process. So the shell's
Versus row stays refused, by design: it is not a step to do. The desktop
Versus is two processes instead (step 7, done).

## RPCN: the plan (from YAMP's notes)

YAMP's Virtual-On netplay (rokuYAMP `docs/von-handoff.md`, `von-comm-packet.md`,
`von-netplay-recon.md`) settled on a different design from the PS3's, after
lockstep failed four times. It works over RPCN on two machines. It fits
m2-hle2's one board:

- **One board per machine.** The ROM's own link protocol goes over the wire:
  each frame, the 0x700-byte comm window (cSend 0x5032F0 -> comm RAM 0x1A12000;
  peer's -> 0x1A12700 -> cRecn 0x5024F0).
  - It needs no lockstep and no determinism.
  - Packets may be lost, duplicated or reordered: the ROM's check is
    self-contained (`[+0x556] == [+2] ^ 0xAE5E`).
  - YAMP sends each one RLE-packed (~120 B) three times.
- **The room gives the role**: host = MASTER, guest = SLAVE, through the
  cabinet-role byte 0x1D00028 (2 NOLINK, 1 MASTER, 0 SLAVE) and a soft reset
  into it. Transmit only once the peer's ROM has released its comm board
  (CommBoardReset 0x01A14000 bit 0).
- **Both boards at 60 Hz, and one packet taken per board frame.** The stage
  handshake (state 0x24 at payload +4, the stage at +8) lives for a single
  frame. A board that reads less often than the peer writes misses it, and the
  two load different stages.
- The comm board itself is MAME's m2comm.cpp with a peer: ring up, node id from
  the role, node count 2. m2comm.h has had both halves since #959 (step 1).

What that means here:
- The link is not a `netplay.h` lockstep session. It is a raw datagram channel
  in a room.
- What the i960 reads becomes the peer's packets, which is outside det_digest
  and rollback: a Virtual-On link session has no savestate rewind and no
  desync check, by design.
- It needs its own `NETPLAY_PROTO_REV`.

Steps:
1. Done (#959). Port MAME's linked m2comm (ring, node id, node count) behind a peer
   interface in m2comm.h. `m2comm_set_peer` takes a datagram send/recv
   pair; with none (the default) the board is what it was, bit for bit
   (det_digest, 6000 frames with sound). `tests/m2comm_test.c` links two
   boards in one process. `det_digest --m2comm-phantom` links the real ROM to
   a slave that echoes its window: with a MASTER backup (role byte 1, the
   ROM's CRC at 0x3120, over bytes 0x16..0x37, stored at 0x14) the ring comes
   up node 1 of 2, `net_flag` is 1, and `cRecn+4` takes the echoed packets
   (0xFF -> 0x00 -> 0x10, attract) from frame 590.
2. Done (#964). `net/comm_link.h` is the m2comm peer over the room's P2P
   socket: each message (ring traffic, or the type byte and the window) is a
   `LOCKSTEP_PACKET_COMM` datagram, the room's header, an epoch, a seq and the
   message RLE-packed, sent three times. Each seq is taken once (one up to 32
   behind the newest still counts, so a ring message overtaken by its pass's
   data frame is not lost); a new epoch starts the count again; the inbox
   keeps the 8 newest. A message that packs past 1180 bytes (the gateway
   relay's 1200-byte datagram) is not sent: Virtual-On's attract windows
   pack to 69 at most. `netplay_comm_open` / `_close` link the board to the opponent
   (nothing in a room calls them yet). `tests/comm_link_test.c` runs two boards
   over a lossy stand-in and over UDP on 127.0.0.1; `det_digest
   --m2comm-udp` is the phantom of step 1 over that UDP pair: 1500 frames
   of vonj give the same digests and the same `cRecn+4` as the in-process
   phantom (922 messages, 3 datagrams each, none torn or too big).
3. Done (#969). The room gives the role: its owner is the MASTER, the other
   the SLAVE (`netplay_comm_role`). The ROM reads the role byte 0x1D00028 only
   at boot, in the frame a blank board factory-fills the block, so
   `net/comm_role.h` waits until the ROM has made its block (the frame edge
   after frame 285 from blank RAM), then the profile's `link_role`
   (`vonj_link_role`) writes the role into
   both copies with the ROM's CRC, and the board cold-boots with its backup
   RAM kept: the boot reads the role, and the peer is plugged in only then.
   The ROM starts its comm board (CommBoardReset bit 0, CN 1) in frame 345
   of that boot and sends nothing before it (`m2comm.h` makes no pass
   while CN is 0). `det_digest --room-role host|guest` stands in for the
   room, with the phantom of step 1 (now a master too, for a SLAVE board)
   in-process or over the UDP pair of step 2, through the emulator's own
   `emu_comm_role_pump`. Over 1500 frames: host gives role byte 1, ring node 1 of
   2, link_ID 1, `net_flag` 1, `cRecn+4` 0x10, first send in frame 578; guest
   gives role byte 0, node 2 of 2, link_ID 2, 0x503A08 1, `net_flag` 1,
   `cRecn+4` 0x10, first send in frame 345; the UDP pair's digests equal the
   in-process ones. Without a peer the board is 75145df's bit for bit (vonj
   and STF, 3000 frames with sound); the libretro shell's Arcade launch writes
   the same backup RAM. Nothing in a room calls `netplay_comm_open` yet, so no
   session reads the peer's packets and `NETPLAY_PROTO_REV` stays: the room
   opens the link with steps 4 and 5. `tests/comm_role_test.c` holds the role
   bytes, the CRC, the shell's settings across it and the order of the reset.
4. Done (#978). While a peer is plugged in (`m2comm_linked`), the host runs
   the board on `core/link_pace.h`'s clock, YAMP's `BoardFramesDue`: each
   frame is due one 1/60 s after the last was due, so a late call runs the
   frames it owes (4 at most; the rest is dropped, not repaid), and nothing
   runs it faster, fast-forward and unthrottled runs included. The emu thread
   sleeps to it (`emu_pace_linked`); libretro's `retro_run` (`lr_run_linked`)
   and the page's frame callback (`web_run_linked`) run what is due, from a
   30 Hz display to a 240 Hz one. The two cabinets' clocks drift, and a
   dropped debt leaves the SLAVE behind for good, with the master's frames
   piling up in its inbox (8 kept, the oldest dropped): so a SLAVE that has 2
   or more of them waiting at 30 frame edges in a row runs one frame early
   (the peer's `pending`, `m2comm_backlog`; 0 on the MASTER, whose clock is
   the link's). The board's frame is unchanged (416,667 cycles); a state load
   and rewind are refused on a linked board. `tests/link_pace_test.c` runs
   the clock on called hosts from 30 to 1000 Hz (3600 frames a minute, one a
   call at 60 Hz with 4 ms jitter), on a stall, on the sleeping host, and on
   two cabinets 500 ppm apart (48 of the master's frames lost without the
   trim, none with it). `det_digest --link-host-hz HZ` puts the board on a
   host called HZ times a second of virtual time, with the phantom MASTER on
   its own 60 Hz clock; `--link-unpaced` runs one frame a call as before.
   vonj, 3000 frames, guest over the UDP pair on a 32 Hz host: unpaced the
   board runs 32 frames a second and loses 2320 of the master's messages to
   the inbox; paced it runs 60.2 a second and loses none (the in-process
   phantom: 2308 and none). At 60 Hz with 4 ms jitter and at 144 Hz: 60.2 a
   second, none lost. The 0.2 is 11 early frames while the ring comes up,
   the backlog the master sends before the SLAVE's ROM takes it (11 at 600
   frames and still 11 at 6000). Host on a 32 Hz host: 32 a second unpaced,
   60.0 paced. Pacing moves no board frame: first sends stay at 578 (host)
   and 345 (guest), roles, node ids, link_ID and `net_flag` as in step 3,
   and without a peer the board is 75145df's bit for bit (vonj and STF,
   3000 frames with sound). Still no room calls `netplay_comm_open`, so
   `NETPLAY_PROTO_REV` stays.
5. Done (#983). A room's match of a game with a `link_role` (vonj) opens the
   link, not a lockstep session: each fighter's `netplay_member_start_session`
   takes the match's side and generation (`netplay_member_start_link`) and
   calls `netplay_comm_open`. Nothing steps a session, so the emu thread's role
   pump boots the board into the room's role and plugs the peer in after it.
   A watcher's board runs alone; the match's end closes the link.
   `NETPLAY_PROTO_REV` 23 (above #381's 22): a room session now reads the
   other cabinet's packets. `netplay_status` has the link (`comm`: role, CN 1
   frame, first send, early sends, ring node, the channel's counts) and
   `get_status` the clock (`link_pace`). `tools/von-rpcn-pair.mjs` runs two
   headless m2hle, one board each, on the container's RPCN (127.0.0.1:31313):
   one owns a room, the other joins, both press Start, and it watches 60 s.
   Owner: role byte 1, node 1 of 2, link_ID 1. Guest: role byte 0, node 2 of
   2, link_ID 2, 0x503A08 1. Both: `net_flag` 1, `cRecn+4` 0x10, CN 1 in frame
   345 of the role boot and nothing sent before it. First sends: frame 578
   (owner) and 500 (guest; against step 3's phantom, which sends from its
   first frame, 345: the SLAVE answers its master). 3618 messages each way,
   every one taken: none overflowed, too big or torn; no frame dropped or
   trimmed; 59.8 board frames a second of wall time on both. Without a peer
   the board is 75145df's bit for bit (vonj and STF, 3000 frames with sound),
   and the libretro shell's Arcade launch (Hard, 2 rounds, 60 s) writes the
   same settings to backup RAM as 42c6e58's.

6. Done (#985). The libretro shell's Online Battle row opens the lobby for vonj
   (`ps3ui_rules_von.no_online` = NULL). With `online_arcade` set, the row
   first writes the Arcade rows (`ps3ui_sh_apply_online`): each cabinet keeps
   its own settings and free play, which the boot into the link's role keeps
   (backup RAM with a fresh CRC). They are not exchanged: a linked pair is two
   cabinets, as in the arcade. While the link is open (`st.comm.open`) the room
   is NETPLAY_IN_ROOM, not PLAYING, so the lobby gives the game the screen
   itself (`ps3ui_follow_link`), and SELECT asks "Do you want to exit this
   session?" over the room (`ps3ui_app_ask_leave_link`); Yes leaves the room,
   which ends the match for the other side too. The core shows
   "Cabinet linked to <name>'s, as the MASTER/SLAVE" once
   (`lr_rpcn_report_link`). UI only: no board path changes, proto stays 23.
   Tested with two RetroArch instances (Xvfb) on the container's RPCN: owner
   Create Match, guest Quick Match, both ready; owner MASTER, guest SLAVE,
   "after 0 frames alone"; both show FREEPLAY, pick machines and fight on one
   arena with their clocks in step (81/81, 73/72, 64/63 s), each seeing its
   own machine; guest SELECT, No goes back to the fight, Yes leaves, and the
   owner gets "match 1 called off" and waits for an opponent again.

7. Done (#987). Versus on one machine, as two processes (the owner's choice:
   no second board in one process). The SDL3 frontend's `--versus` makes this
   m2hle the MASTER cabinet: it opens a UDP socket on 127.0.0.1
   (`net/comm_local.h`) and starts itself again (`core/child_proc.h`) with
   `--versus-slave PORT:TOKEN --pad 1 --window-at X,Y`, the SLAVE, in a
   640x480 window beside its own. The SLAVE's hello, with the token, pins its
   port at the MASTER; from then on the two run the room pair's link: the same
   `comm_link.h` datagrams, `comm_role_begin` and the role boot through the
   reset hook, and `link_pace.h`. Each window plays one pad (`--pad N`, MASTER
   the first, SLAVE the second), read with the focus on either window; the
   keyboard plays the window that has it. The SLAVE's `--log` and `--shot`
   files get `-slave` before the extension. Closing either window ends both:
   the child dies with its parent (PR_SET_PDEATHSIG, or a kill-on-close job on
   Windows) and the MASTER quits once its child has gone. Not with `--netplay`.
   No board path changes: proto stays 23. Tested under Xvfb with vonj
   (`--no-nvram`): coin and Start in each window, each picks a machine, and the
   two fight on one arena, each seeing its own machine, the other its enemy,
   their clocks a second apart (74/73, 57/56 s), as step 6's pair. 3023
   messages each way in 3600 frames, every one taken (one in flight at the
   end), none torn, too big or overflowed; ring up, nodes 1 and 2 of 2.
   Killing either process ends the other. `tests/comm_local_test.c`: the
   hello, both ways, a wrong token, a second hello and another port's
   datagrams dropped. Not tested: gamepads (none in the container) and the
   Windows path (it builds the child's command line by CommandLineToArgvW's
   rules; checked on Linux, not compiled).

All seven steps are done. Nothing is left to do: the libretro shell's Versus
row is refused on purpose (see "Why the libretro shell's Versus is refused").

## GGPO: not for Virtual-On

GGPO rolls back one deterministic emulation that holds both players. For STF
that is one board. For Virtual-On it would be both boards on every peer: the
two-board core from above, at twice the CPU, re-simulated on every rollback.
That is too heavy for the handhelds the core runs on. The RPCN link needs
neither the second board nor determinism. So the choice is the link, and no
GGPO for Virtual-On.
