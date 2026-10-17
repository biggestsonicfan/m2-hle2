# Netplay / RPCN (`src/net/`, board-independent)

Part of m2-hle2's load-bearing invariants, indexed in [CLAUDE.md](../../CLAUDE.md): facts reverse-engineered or debugged into the emulator, in no datasheet. Do not re-derive them.

Matchmaking is [RPCN](https://github.com/RipleyTom/rpcn); the design follows `yampnet`
(a sibling checkout, `ai/yampnet`), which worked the protocol out first. These are the parts that
are **silently wrong** rather than loudly wrong when you get them half right.

- **A session is a COLD BOOT on both machines, not a savestate.** The barrier releases, both peers
  reset the board, and every frame from power-on is lockstepped. A savestate (below) is never sent
  to a peer, so this is the only state two copies are certain to share; it is also stronger than the PS3 port's
  shared RNG seed, because no window exists in which the two were allowed to differ. The reset has
  to clear the run loop's own latches too (`emu_board_reset_state`) — an interrupt left in service
  across it swallows the first interrupt of the new boot, which is a divergence on frame 1.
- **The board must read a COMPOSED input mask, not the keyboard's.** `input_state_t` carries
  `net_held` / `use_net` for that. Writing the composed mask over `g_input.held` looks equivalent
  and is not: key events land on the UI thread at arbitrary moments, so the composed value gets
  half-overwritten partway through an emulated frame on one machine and not the other.
- **np2_structs.proto types `uint8` and `uint16` as MESSAGES** (`{ uint32 value = 1 }`, kept from
  the flatbuffers port). A field declared `uint16 serverId = 1` is a length-delimited submessage
  containing a varint, not a bare varint. `flagAttr` and `roomId` *are* bare varints. Reading one
  as the other yields an empty message, not an error.
- **Three room fields are mandatory despite proto3**, all read with `get_verified()`: `teamId` on
  CreateRoom (field 16) and JoinRoom (field 6), and all three fields of `sigOptParam` (17). Omitting
  any is `Malformed`, not a default.
- **`sigOptParam` is what makes peers reachable at all.** Without it `need_signaling` is false in
  `room_manager.rs`, the join reply carries no `signaling_data`, and the host's `UserJoinedRoom`
  notification carries no address — so the host has nobody to punch towards and stays silent.
- **A room password is a FIXED 8 BYTES and needs `passwordSlotMask`.** The server takes the
  password only `if len == 8` and otherwise logs "Invalid password length" and leaves the room
  *open*; the mask (MSB-first, slot i = bit 63-i) is what actually gates the slots. Get either
  wrong and the room looks protected here and is public there.
- **SearchRoom needs `option` bit 0** or every row comes back with no owner name, and its range
  filter is **1-based and capped at 20**.
- **The world count is a u32 while the server count is a u16** (`cmd_server.rs`). Reading the world
  list as u16 shifts every entry two bytes and yields plausible-but-wrong world ids.
- **Every room REPLY is `[u32 LE length][protobuf]`** (`Client::add_data_packet`). Strip it, or the
  length parses as a tag and the message reads as empty.
- **Login's third string is the e-mail verification token, not a second password**, and the server
  compares it only when it has e-mail validation on (off by default). Empty is the normal value.
- **When two peers share a public IPv4, RPCN hands out the peer's LOCAL address with port 3658
  HARDCODED** (`room_manager.rs` and `cmd_misc.rs` both). Two clients on one machine are therefore
  told to punch at their own socket, so `rpcn_session_recv` drops datagrams whose source is our own
  `local_ip:local_port` — without it `peer_heard` latches onto our own port and every real datagram
  from the peer is then discarded as a stray.
- **A punch may come from an address the server never gave; a game packet may not** (issue #108,
  `rpcn_session_hear(..., punch)`). A player in a container advertises its bridge address (172.x)
  and its datagrams leave through the container host's NAT, so its LAN opponent hears it from an
  address nobody named and used to drop every datagram as a stray: both sat at the barrier. A
  punch names its sender, so it re-points a member not yet heard; once heard, nobody moves them.
  `--net-local-ip` / `$M2HLE_NET_LOCAL_IP` puts the container host's LAN address in the keepalive
  instead (`rpcn_client_t.advertised_ip`; `local_ip` stays the socket's own for the self filter).
  It is a process setting, not a `netplay_config_t` field, so the wholesale config copies (file,
  window, MCP) cannot drop it. `tests/net_test.c` part (G) holds both over loopback.
- **Behind Docker Desktop nobody outside the house can reach us at all, so a container relays
  through the web gateway** (`ws_relay.h`, `--net-relay` / `$M2HLE_NET_RELAY`, Pinboard #366).
  Docker Desktop's NAT gives the keepalive a random port that forwards nothing back (only the
  published 3658 does), and RPCN tells guests to punch that port: they punched forever. Now every
  datagram rides the gateway's `/gw/dgram` WebSocket (`[ip][port BE][payload]`, as the web build's
  do), so RPCN and guests see the gateway's public address. It is decided once at session start,
  never mid-room: a room keeps the address it saw at the join. `auto` is on in a container
  (`/.dockerenv`) for the servers `ws_relay_url_for` knows; a process setting, like `local_ip`.
  - The gateway refuses a WebSocket without an `Origin` it lists, so the relay sends the play
    site's. It answers the gateway's pings (30 s) or is dropped.
  - A lost relay drops the RPCN link too (`rpcn_recv_from`), so the heal signs back in and
    reopens it. A heal attempt must not fall back to direct (`relay_required`, all but the last
    try): the gateway shares the droplet with RPCN and came back a second after it in the test,
    and a direct sign-in then left the room unreachable for good.
  - **A guest outside joins a relayed owner through the gateway too** (Pinboard #382). The
    gateway's UDP port is not reachable from every network: a guest behind a carrier-grade NAT
    punched the fly at `143.198.49.181:40xxx` for the whole match and was never heard. So a client
    sending directly keeps the gateway in reserve (`relay_standby_url`), asks where a room's owner
    is before joining (`rpcn_session_probe`, RequestSignalingInfos), and when the server places
    the owner at the gateway's address it turns the relay on, waits for the helper to see the new
    address, and only then joins. Both members then hold the gateway's virtual addresses and it
    carries the match inside itself. Turning the relay on is before the room, so "never mid-room"
    still holds. `auto:ws://...` names the reserve gateway (the tests'); `off` keeps none.
- **A room copies each member's address when it is created or joined, and never refreshes it.**
  The address reaches RPCN only with the first UDP keepalive after login, so a Host or Join sent
  straight after sign-in snapshots nothing — for the life of the room — and two players on one
  public address are then told *different kinds* of address for each other and drop each other's
  datagrams as strays. So Host and Join wait for the signaling helper's reply
  (`netplay_room_or_defer`, at most `NETPLAY_ROOM_WAIT_MS`), and an address the peer has actually
  been heard from is never replaced by one the server reports later (`rpcn_session_set_peer`).
  Found by `tools/web-netplay.mjs`, which hosts 0.2 s after signing in; a person rarely beats the
  keepalive, a script always does.
- **The build family rides in bits 6-7 of the room's revision byte, not bits 28-31** (those are
  the server's: it owns `ROOM_FLAG_ATTR_FULL` there). Lockstep needs bit-identical floats, so a
  lobby refuses a room of an incompatible family with a sentence; `NETPLAY_CROSS_PLAY` (on) makes
  native and wasm one. A web room's byte reads `0x41`, which desktop builds from before the field
  refuse as "a different netplay protocol", so only a desktop build from after cross-play can join one.
- **The wasm and MSVC builds compute the same frames only because of two things C leaves open.**
  Break either and web-vs-desktop matches desync; `tests/det_digest.c` (built in both trees) is the
  check, and it split at frame 2948 of attract on each.
  - *A NaN's sign and payload.* Every COP float that becomes a word goes through
    `sharc_float_to_bits`, which writes NaN as all ones, as the SHARC does. The i960's FP
    instructions go through `i960_nan_result` / `i960_single_to_double` / `i960_double_to_single`.
    Do not memcpy a float to a word in board code, or return a raw `a op b` from an FP instruction.
    LLVM moves negations across multiplies, which flips a NaN's sign, and `Fn_area_coli` branches
    on the sign bit of a NaN ball position.
  - *Strict aliasing.* GCC and Clang build with `-fno-strict-aliasing` (CMakeLists.txt), as MSVC
    always behaves. Without it the wasm build split from MSVC, and a trace compiled into the step
    loop hid the split. That is undefined behaviour at work; the offending access has not been found.
- **Connection trouble is repaired, not reported** (Pinboard #120), and nothing on the machine or the router is touched. Three repairs, each found or held on a local RPCN with two headless clients:
  - *A taken UDP 3658* steps to the next free port (`RPCN_P2P_PORT_TRIES`, `rpcn_session_start`). The server learns the port from the keepalive. Two players behind one public address are still told 3658 (the hardcode above), so an unheard member told 3658 on a LAN address is punched on the next ports too (`rpcn_session_pump_punch`). Two clients on one machine now link with no `p2p_port` at all.
  - *A member whose NAT made a new mapping* is followed once the old address has been silent for `RPCN_PEER_QUIET_MS` (3 s), by first contact's rules: a punch from anywhere, a game packet from the same address on another port (`rpcn_session_hear`). Before, "once heard, nobody moves them" dropped all their datagrams and the stall timer ended the match. `tests/net_test.c` part (G) holds both.
  - *A dropped link to RPCN* signs back in by itself (`netplay_heal_*`, 1 s doubling to 16 s, 8 tries, attempts not logged). Only a login that reached ONLINE, only when the TLS link is gone (`rpcn_is_connected`), never after a refused credential. A running match ends at once: the room calls it off the moment the server drops us. Then the player goes back to their room. If the server lost it (a restart closes every room), the old owner opens it again and the others search for a room owned by that npid for 30 s. A heal's join is `join_soft`: a missing room leaves the session ONLINE, not FAILED. Measured: 7 s from the server coming back to both players linked in a room.
- **Hole punching needs BOTH ends transmitting.** The guest's first packet opens a mapping through
  the *guest's* NAT only; a host that waits to hear something first never opens its own, and two
  peers on different networks sit at the barrier forever.
- **Twitch sign-in is a PASSWORD, not a parallel login path.** RPCN's device flow (commands 63/64,
  unauthenticated) runs once and returns a long-lived login token that the ordinary `Login` accepts
  in place of the password, so everything below `netplay.h` is unchanged by it. Three things bite:
  `TwitchAuthPending` (35) and `TwitchAuthSlowDown` (36) are *not* errors and must not end the flow -
  pending is the answer for most of its life, and slow-down means back off by another interval; the
  whole flow must stay on ONE connection, because the server relaxes that connection's
  unauthenticated read timeout from 10 s to 120 s only after the start succeeds; and an older RPCN
  does not know command 63 at all, so it answers `Malformed` and HANGS UP - a disconnect before the
  start reply means "no Twitch here", not "the network broke".
- **Signing in with Twitch is not the same thing as running the device flow.** The flow exists to GET a token; once one is stored, "with Twitch" is an ordinary `Login` with the token for a password (`netplay_twitch_reuse`). `NETPLAY_CMD_TWITCH_START` used to begin the flow unconditionally, so `{"cmd":"netplay_connect","twitch":1}`, `--net-twitch` and the window's button each sent somebody to twitch.tv on every launch with a good token in the file. Three things keep it that way:
  - The stored token and its owner belong to `g_netplay.cfg` and **survive the wholesale copy in `netplay_do_connect`**. A posted config is a copy that is stale in both directions: the window adopts the stored settings once, so Connect wiped a token the flow had just landed, and resurrected one that "Sign out" had just forgotten.
  - **`twitch_npid` is a guess until the server has answered.** The owner adopted at load is the npid stored beside the token, which is the last account that logged in, not necessarily the one that signed in with Twitch. A login that offered the token and was accepted stamps the owner (`netplay_mirror_stage`); a token refused under its own owner's name is dead and is forgotten. On the machine this was found on, the label named one account and the server accepted the token for another.
  - **At most two rejected logins, then a browser or a stop** (`netplay_twitch_refused`). Retrying a refused credential is how an account gets locked; `rpcn_session_t.credential_refused` is the fact to test, not the error text.
  - *Also true, and outside this repo:* YAMP signs in with Twitch too, and its `settings.ini` password for the same account was refused by the server while the emulator's token was accepted. Two clients sharing one account each hold a token, and they do not both stay good.
- **The settings file is per USER, not per copy of the exe** (`netplay_cfg_path`: `%APPDATA%\m2hle2\netplay.cfg`, `~/.config/m2hle2/netplay.cfg`; `--net-config` overrides). RPCN keeps **one Twitch token per account** (`set_twitch_login` overwrites the hash), so every device flow retires every other stored token. When the file lived in the working directory, each copy (a build tree, the canary download, the fly's stream kit) held its own token, and signing in from one killed the rest: the kit's token was refused, forgotten as dead, and the headless fly cannot approve a code. A legacy `m2hle_netplay.cfg` in the cwd is copied through once, when the per-user file does not exist yet. The save is temp-file + rename, because a stream and its training population share the file.
  - *Two accounts on one machine* (the two-client repro) now need `--net-config` per client. A separate cwd is no longer enough.
  - **YAMP shares the file too** (its `source/net/SharedLogin.cpp`): a Twitch sign-in there writes `twitch_token`/`twitch_npid` here, and its logins offer this file's token first. So the file can change under a running process, and two rules keep one process from putting a dead token back: `netplay_settings_save` gives way to a newer file token when this process has not changed its own since it last synced (`twitch_synced`, `netplay_twitch_merge_disk`), and a refused token first retries with a different token the file now holds (`netplay_twitch_refused`, still one retry). Forgetting it straight away would save an empty token over the one that works.
- **Never hand a server-supplied URL to `ShellExecute`.** The activation URI comes from the RPCN
  server, and `open` will run a local executable or a registered protocol handler just as happily as
  it opens a web page. `netplay_open_url` requires a literal `https://` prefix first, and is off
  entirely for a headless run.
- **A challenge is an announce for a round this end has not begun.** There is no ready message in
  the protocol; the barrier releases when both peers announce the same generation, which is what
  pressing Start does. `lockstep_on_peer_announce` drops every announce that is not the round we
  are already in -- and that is precisely the case a host needs to see, so `netplay.h` latches it
  separately (`peer_ready_gen`, `netplay_peer_ready`). It is a freshness window and not a flag:
  a peer at the barrier announces once per slice, so a challenger who walks away retracts their
  own challenge, where a sticky bool would leave one standing forever.
- **A peer's input record for this round is also its announce** (`lockstep_on_record`). A peer
  sends inputs only once its own barrier has released, and it stops announcing the moment it does.
  When the host releases on the guest's *first* announce, every announce the host sent before that
  reached a guest not yet in the round and was dropped, so without this the guest waits for an
  announce that never comes and the host stalls at frame 0. Over the internet one is usually still
  in flight; over loopback, and through the web gateway on one machine, the race was lost every time.
- **A machine that is WAITING has to keep talking.** Inputs go out once, when a new local frame
  is sampled, and the redundancy in a record rides on the *next* record. A stalled machine samples
  nothing, so when both peers are stalled nobody transmits and a burst of loss is permanent: with a
  delay of 2, five datagrams dropped one way. `netplay_resend_inputs` re-sends
  `[lockstep_resend_floor, last_local_frame]` every 50 ms while stalled. The newest record alone is
  not enough: the lost frame is `2*delay + 1` behind it, past one record's reach above a delay of 4
  (`tests/net_test.c` runs the deadlock at every delay). The same loop paces the barrier announce,
  which used to go out every millisecond -- a thousand datagrams a second at one address is what a
  consumer gateway's flood detection looks for.
- **Both inputs being in is permission to run a frame, not proof that it ran.** An emulator that is
  paused, halted, or never reaching the frame hook is cleared for the same frame on every slice,
  so it never counts as stalled: it shows "playing", sends nothing, and the only evidence is the
  *other* machine's stall timer blaming the network. `netplay_watch_own_board` reports it after 3 s
  and leaves at the stall timeout, the run loop adds whether it was a pause or a halt
  (`netplay_board_stopped`), and a machine in that state keeps re-sending so the peer's stall line
  ("the peer's last input arrived N ms ago") can tell a stopped board from a dead link.
  - *How it surfaced:* the first session with a player on another network. Their board stopped
    finishing frames 45 frames after the reset, twice, over a corrupted screen; the host logged a
    stall at frame 48 and neither side said which machine had stopped. **What stopped that board was
    still open when this was written** -- it does not reproduce here (`tools/grade-reset.mjs` is
    exact, and the CI build boots byte-identical to a local one), so the next report needs that
    machine's `m2hle.log`.
- **A scripted session must not write memory or halt the board.** Both are invisible locally and
  fatal jointly: `write_memory` changes one board and not the other, which is what the frame
  check exists to catch, and halting to think is a stall the peer sees -- m2-hle2 drops a session
  that stalls for fifteen seconds. Inputs need no special path, because `set_input` writes
  `g_input.held` and that is exactly what `netplay_sample_local` transmits.
- **One ComId per ROM set** (`com_id.h`, `M2H` namespace). RPCN partitions everything by it, so a
  single hardcoded id puts every Model 2 game in one room list where the mismatch is found by the
  netcode instead of the browser. Unlisted games get a deterministic base32 hash of the game key;
  `CreateMissing=true` registers a new id on first use, so no `servers.cfg` edit is needed.
- **Two servers, and the PS3-menu lobby (libretro core, web build) lets the player pick**
  (`NETPLAY_SERVER_OFFICIAL` np.rpcs3.net, the default there; `NETPLAY_SERVER_COMMUNITY`
  rpcn.sonicthefighte.rs, the default of the desktop window and the handheld's pad lobby, which
  has no picker). Accounts are per server. What bites:
  - **A Twitch token is a password and must only go to the server that issued it.** Once the
    server can change, the stored `server` no longer says where the token came from, so it is
    kept in `twitch_server` / `twitch_port` (a file without them adopts `server`), and
    `netplay_twitch_here` gates every use. Without it, `netplay_do_connect` offered our token to
    np.rpcs3.net as the password of an account with none stored.
  - **Twitch sign-in is ours only**: the lobbies switch the server to ours before
    `NETPLAY_CMD_TWITCH_START`. The official server has no Twitch.
  - **The official server's certificate is self-signed** (CN=RPCN, to 2030-07-21) and nothing
    pinned it, so a native build could not reach it at all. `NETPLAY_OFFICIAL_FINGERPRINT` is
    used when the player has no pin of their own (`netplay_pin_for`).
  - **The web build names the server in the gateway PATH** (`/gw/stream/np.rpcs3.net`; ours is
    the plain `/gw/stream`), never a query. The gateway from before the choice ignores a query
    and would relay an official-server login to ours; an unknown path is a 404 there. So the
    gateway must be deployed with its `upstreams` before a web build that offers the official
    server, and until then that choice fails cleanly.
- **A linked comm board's datagrams are not lockstep traffic** (`LOCKSTEP_PACKET_COMM`,
  `net/comm_link.h`, Virtual-On). Each board runs alone and reads the other cabinet's packets,
  so nothing gates on them and no desync check covers them. A message must fit the gateway
  relay's 1200-byte datagram (`WS_RELAY_DGRAM_MAX`, smaller than the web socket's 1400): the
  window goes RLE-packed. Dedupe takes each seq once within 32 of the newest, not "newer than the
  last": the master's ring size and its data frame leave in the same pass, and a strict
  newest-wins drops the size when the two arrive swapped. The drain's buffer is a whole
  datagram's (it was an input packet's 84 bytes, which truncated these).
- **A linked board boots into the room's role; nothing pokes it into a running board**
  (`net/comm_role.h`, the profile's `link_role`, Pinboard #969). The room's owner is the MASTER,
  the other the SLAVE. Virtual-On reads its role (`0x1D00028`) only at boot, in the very frame a
  blank board factory-fills its backup block (frame 285), so a write from outside never lands
  between the two. `comm_role_begin` detaches the battery first (the role must not reach the
  player's file); at each frame edge `link_role` writes the role into each copy the ROM made,
  under the ROM's CRC (and the PS3 shell's Arcade writes, which leave the first copy's CRC stale,
  are kept); then the board cold-boots with its backup RAM kept and only then gets its peer.
  The ROM sends nothing until it starts its comm board (CN 1; `m2comm.h` makes no pass before).
  The reset is not a lockstep barrier: `emu_comm_role_pump` runs only while no session steps.
- **A linked board keeps 60 Hz of wall time, whatever the host** (`core/link_pace.h`,
  Pinboard #978). Nothing waits for the other cabinet, so its speed is only kept by both hosts
  running the board to the wall clock: a slow board loses the stage handshake (state 0x24, one
  frame) and moves less often. While `m2comm_linked()`, the emu thread, `retro_run` and the
  page's callback run the frames that are due (a late one owes up to 4; the rest is dropped),
  and nothing runs it faster (fast-forward, unthrottled, a watcher's catch-up). Only the SLAVE
  trims (one frame early after 30 frame edges with 2+ of the master's messages waiting): a
  master that trimmed on the slave's answers would chase the slave's backlog. The board's
  frame stays 416,667 cycles; a linked board refuses a state load and rewind.
- **A linked game's room match opens the link, not a lockstep session** (`netplay_member_start_link`,
  Pinboard #983). For a profile with a `link_role`, each fighter takes the match's side and
  generation (the datagram header) and calls `netplay_comm_open`; the room stays in its lobby
  state and no session steps, which is what lets `emu_comm_role_pump` boot the board into the role.
  There is no barrier, input exchange or desync check; a watcher's board runs alone, and
  `netplay_end_match` closes the link. Each board sends nothing before its comm board starts
  (`netplay_status` `comm.early`, 0 on the RPCN pair test, `tools/von-rpcn-pair.mjs`).
- **A Versus on one machine is two processes, not two boards** (`net/comm_local.h`,
  `core/child_proc.h`, SDL3 `--versus`, Pinboard #987). The board is one set of globals, so the
  MASTER's m2hle starts a second one (`--versus-slave PORT:TOKEN`) and the two link as a room's
  pair does: the same `comm_link.h` datagrams, role boot and pace, over a UDP socket on
  127.0.0.1 with no RPCN. The MASTER sends nothing until the SLAVE's hello (magic and the token
  from its command line) pins its port, then takes datagrams from that port only. The child dies
  with its parent (`PR_SET_PDEATHSIG`; a kill-on-close job on Windows) and the MASTER quits when
  it is gone. No board path changes, so the proto does not move.
