# Rooms of more than two (`net/room.h`, after the PS3 port's Room Match)

Part of m2-hle2's load-bearing invariants, indexed in [CLAUDE.md](../../CLAUDE.md): facts reverse-engineered or debugged into the emulator, in no datasheet. Do not re-derive them.

Up to eight in a room: two fight, the rest wait in line and watch, and after every result the
winner goes to the front and keeps their side, the loser to the back (the PS3 port's
`np_session_build_fight_entries` / `rotate_queue_after_match`, reverse-engineered from
NPUB30927 -- room.h cites the addresses). The owner writes the room state to RPCN (room bin attr
0x57) and each member its own (member bin attr 0x59); nobody sends room state peer to peer, so a
new owner carries on from the server's copy. What bites:

- **Every match is a cold board reset on EVERY member**, fighters and watchers alike, and a
  lockstep generation of its own. The PS3 port never resets; a savestate never crosses machines, so
  the reset is the only shared state. **The room's owner also decides the region** (room state,
  `g_region`): members on another region boot another game from frame 0.
- **Only the two fighters gate a frame.** Watchers (`LOCKSTEP_WATCHER`) run the fighters' two
  input streams, get a record every `NETPLAY_WATCH_STRIDE` frames (the web gateway caps a player
  at 240 datagrams/s), and ask a fighter to re-send with `LOCKSTEP_PACKET_REPAIR`.
- **A watcher paced to 60 Hz never makes up a frame it waited for.** It fell ~160 frames behind
  over one match and the next match cut it off short of the result. `netplay_catching_up` lets
  the run loop (native, web and libretro alike) run it unpaced while it holds frames ahead.
- **The result is read off the board**: an observe-only hook at STF 0xDC3C (`SFIGHT_BASE_HOOKS`,
  the instruction the PS3 port hooks too) sets `g_versus_result`, which the emu thread hands to
  `netplay_end_frame` at the frame boundary. A board reset clears it.
- **"Everyone ready" starts only the FIRST match.** Ready flags stay set, so honouring them after
  a result started the next match instantly. After that the countdown decides -- and only in a
  room of three or more (`netplay_room_rolls`); a two-seat room is the old one-on-one, where
  both press Start again.
- **`rpcn_poll` hands out a pointer INTO its receive buffer** and must not slide that buffer until
  the NEXT poll. It used to slide it at once, so a reply with a notification behind it in the
  same read was read as the notification's bytes -- every room join failed that way ("the room
  reply carried no room id").
- **RPCN tells members who share a public address that each other is on port 3658**, which is at
  most one of them. Two players never needed more than the first datagram to fix that; three
  never find each other. Members pass on where they hear the others from (`g_rpcn_intro_tag`
  introductions), punched beside the server's address.
- **VS mode can carry one cold boot across matches** (`room_vs_continues`). The owner's VS setting is in the room state (`vs_mode`, like `region`), and so is `session`: the match whose cold boot the boards are running. When a VS match is decided and the two fighters are the room's only players, the owner publishes `match + 1` with `session` unchanged. Each board moves `match_started` on by one when its own board reaches the result (`netplay_end_frame`), never when the room says so, because a watcher can be a whole match behind and would file the wrong result. With anyone waiting in line the owner closes the session instead, and a board that played on past the result stops there (`netplay_member_pump`). The lockstep generation is the session's and does not change across a rematch.
  - **A VS rematch asks first, over the game** (`PS3UI_SCR_AGAIN` in `ps3ui_app.h`; the handheld's `pad_lobby.h` does the same). Without it the boards go back to character select forever and nothing on screen lets a player out. `netplay_status_t.results` counts every match this board fought and saw decided. On each one a fighter gets the PS3's result window with Play again / Exit and a 10 s timer. Timing out means stay; Exit leaves the room, which ends the session on both boards (the owner calls it off), and the one left behind is told so. **It must not depend on the lobby having been opened**: a session joined from the web page's panel or RetroArch's autojoin never opens it, and the first version, which only asked from an open lobby, asked nobody there. The task opens itself for the prompt (`prompt_only`) and closes again, the Console shell lets it over any screen, and the desktop has `netplay_vs_again_overlay`. It is UI only: the board runs on underneath and gets none of the pad while asked, so no protocol change.
    - **Every match asks, not only VS mode** (Pinboard #8). The first version counted VS results only, and every lobby defaults to Arcade rules, and the official server's rooms (the web and libretro default) run the PS3 port's rules, which have no VS mode: the prompt never came up there, and the owner reported it gone. Outside VS the result ends the session, so the prompt has to stay up in `IN_ROOM` (it closes only once we are out of the room), Play again is Start (a one-on-one room clears ready after a result), and the web shell runs the task whenever `netplay_in_room()`. With the lobby open, a non-VS result keeps the lobby's own result screen.
- **RPCN never announces a new owner** -- `leave_room` picks a successor silently -- so every
  departure is followed by a GetRoomDataInternal.
- A local RPCN (`RipleyTom\rpcn`'s built `rpcn.exe --cert-gen`, EmailUrl empty) and the MCP bridge
  run three clients on one machine; that is how every item above was found.
