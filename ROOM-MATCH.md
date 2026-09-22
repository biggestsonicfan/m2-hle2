# Room Match: the PS3 port's lobbies, and how m2-hle2 plays them

The reverse engineering behind `src/net/room.h`: how the PS3 port of Sonic the
Fighters (NPUB30927) runs a room of more than two, read out of its EBOOT in
Ghidra. Addresses are the EBOOT's. Where m2-hle2 follows the port and where it
does not, and why, is at the top of `room.h`; `CLAUDE.md` ("Rooms of more than
two") has the invariants that only showed up by running three clients.

The three parts below were written while tracing it and are kept as they were:

1. **The NP session layer** -- room creation and search, the room and member
   binary attributes, choosing the fighters, rotating the line, the owner's
   phase machine.
2. **Match flow and input sync** -- how a match starts, the lockstep the port
   runs over cellRudp, how spectators follow it, and how the result is read
   off the arcade board.
3. **The "Multi" screens** -- the rule menu, the result screen, the observer.

Everything is marked **V** (read in the decompile or disassembly) or **I**
(inferred). The UI text ids are the port's `string_array.farc`
(`rom-extracted/string_array.farc`, an uncompressed FArc holding
`string_array_en.bin`: a big-endian u32 offset table, then NUL-terminated
strings).

---

## Part 1: STF PS3 (NPUB30927) — NPMatching2Session lobby internals

Scope: PPU wrapper, 0xb4000–0xc3000 plus the TaskSession overrides it calls.
**[V]** = read in decompiled or disassembled code. **[I]** = inferred.
Big-endian throughout. `mgr` = the NP manager singleton `*DAT_00401920` (the same object as `DAT_004018e0`). `S` = the TaskSession object (`this`, class `11TaskSession`, which derives from `18NPMatching2Session`).

### 0. Big picture (read this first)

- **The "entry queue" at S+0xC8/+0xCC holds only the 2 fighters of the current or next match.** It is not the whole waiting line. It sits inside the 0xE8-byte **room binary attribute (internal, id 0x57)**, which S mirrors at S+0xB4..S+0x19B. That is why the offsets are +0xC8/+0xCC: blob+0x14/+0x18. [V]
- **The whole waiting line is the `teamId` of each room member.** Each member publishes its own rank (1..N) through SetRoomMemberDataInternal's `teamId` field; a newcomer joins with teamId 0xFF. Everyone sorts on `(teamId<<16)|memberId`. [V]
- **Each member publishes its own member data.** It holds a flags word, its 24-byte stats, and an "entry request" byte (0 none, 1 = 1P Entry, 2 = 2P Entry). That byte is the "Match entry" button. [V]
- **The owner (host) alone drives the room state machine** and writes the room blob with SetRoomDataInternal. Non-owners only follow the phase they read back. [V]
- **Fighters send their 100-byte entrant data to the owner over cellRudp.** The owner also relays some message types to the other peers. [V]
- **After a match, every member works out the new order locally, all with the same rule:** the winner goes first, the waiting members stay in their order, and the loser goes to the back. Each member then republishes its own teamId. [V for the algorithm; I that every machine sees the result]

### 1. Room creation — `np_session_create_join_room` 0xBB34C [V]

SceNpMatching2CreateJoinRoomRequest (stack at local_d0):

| off | field | value |
|---|---|---|
| 0x00 | worldId | `mgr->worlds[mgr+0xC8].id` (`*(mgr + 0x40 + idx*0x3C + 0xC)`) |
| 0x08 | lobbyId | 0 |
| 0x10 | **maxSlot** | `cfg[1] + cfg[2]` (S+0x11 + S+0x12) |
| 0x14 | flagAttr | 0x04000000 (NAT_TYPE_RESTRICTION; per RPCN room_manager.rs:59, and RPCN ignores it) |
| 0x18/0x1C | roomBinAttrInternal | 1 × {id 0x57, ptr S+0xB4, size 0xE8} |
| 0x20/0x24 | roomSearchableIntAttrExternal | 8 attrs, ids 0x4C..0x53 (below) |
| 0x28/0x2C | searchable bin ext | none |
| 0x30/0x34 | roomBinAttrExternal | only if cfg[0x1E] (S+0x2E): 1 × {id 0x55, 4 bytes} |
| 0x38 | roomPassword | 8 random bytes (FUN_000f15c0) if cfg[3] (S+0x13) != 0, else zero |
| 0x3C/0x40 | groupConfig | none |
| 0x44 | passwordSlotMask | bits (63−cfg[3])..63 set, so slots 1..cfg[3]+1 in RPCN's MSB-first numbering |
| 0x48..0x58 | allowed/blocked/joinGroupLabel | none |
| 0x5C/0x60 | roomMemberBinAttrInternal | 1 × {id 0x59, ptr S+0x19C, size 0x20} |
| 0x64 | teamId | 0xFF (also S+0x1BC = 0xFF) |
| 0x68 | sigOptParam | {type 1 = MESH, flag 0, hub 0} (`0x0100000000000000`) |

The config bytes `cfg[k]` are S+0x10+k. They come from `np_session_set_config` 0xB8F84 (via 0x65AF8 ← 0xAD344; menu builder 0xB3358/0xAF230).

- **cfg[0]**: room mode. 0 = one-match session; 1 = **Room Match**, the multi-match lobby. It is copied to blob+4. [V]
- **cfg[1]**: fighters per match. Code caps it at 2 (`build_fight_entries`), and a default of 2 is set in 0xAF230. [V]
- **cfg[2]**: the extra slots, so maxSlot = 2 + cfg[2]. [V]
- **cfg[3]**: the private slot count (password mask). [V]

[I] The UI's "No. of players 2–8" (0x15F) is therefore maxSlot, and "Private slots 0–7" (0x167) is cfg[3]. I did not find the menu writer that turns one into cfg[1]/cfg[2].

Searchable int attrs (all `u32`):
- 0x4C..0x50: cfg[4]..cfg[8] (rule settings)
- 0x51: cfg[9], or `(country[0]<<8)|country[1]` from sceNpManagerGetAccountRegion when cfg[9] == 1
- 0x52: `(cfg[0x1E] ? 0x80000000 : 0) | cfg[0]`
- 0x53: constant **0x0133054E** (game/version tag; JoinRoom from an invite rejects a mismatch, 0xBD8D0)

Bin ext 0x55 (4 bytes) = `[profile byte S+0x44][pct(S+0x40 / games)][pct(wins / games)][0]`. games = S+0x38 and wins = S+0x3C. A percentage is only computed once games ≥ 11, and is `(float)x/games*100`.

**SearchRoom** — `np_session_search_room` 0xB9DAC [V]:
- option = 0x19 (WITH_NPID | NAT_TYPE_FILTER | RANDOM)
- range {1, 20}
- flagFilter 0x70000000 and flagAttr 0, so CLOSED, FULL and HIDDEN rooms are excluded
- int filters: EQ on 0x4C..0x51 only where the search min == max (pairs S+0x1C/0x24 … S+0x21/0x29); always EQ 0x52 and EQ 0x53 == 0x0133054E
- attributes fetched: 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x55 (0x376C48); 5 of them, without 0x55, when not ranked

**JoinRoom** — `np_session_join_room` 0xBD8D0 [V]:
- teamId 0xFF, one member bin attr
- the password pointer is passed when an invite carries one
- an invite whose tag is not 0x0133054E is refused

### 2. Room data internal (id 0x57, 0xE8 bytes, mirrored at S+0xB4) [V]

| blob off | S off | type | meaning |
|---|---|---|---|
| 0x00 | 0xB4 | u32 | random seed, set at match start (`np_session_seed_match` 0xBD7B8 = vtable+0x64) |
| 0x04 | 0xB8 | u8 | room mode (cfg[0]) |
| 0x05 | 0xB9 | u8 | match flags, cleared then set at start: 0x40 = members ≠ fighters (spectators present); 0x80 = some fighter slot has bit30 (a fighter lacks a direct link) |
| 0x08 | 0xBC | 8 B | rule bytes cfg[4..0xB] |
| 0x10 | 0xC4 | u32 | **phase** 0..4 |
| 0x14 | 0xC8 | u32 | fighter count (0..2) |
| 0x18 | 0xCC | u16[2] | fighter memberIds: [0] = 1P, [1] = 2P |
| 0x1C | 0xD0 | u32 | max RTT in ms from the owner to the non-owner fighters (`np_signaling_get_rtt_ms` 0xB5678 = ConnectionInfo code 1 / 1000) |
| 0x20 | 0xD4 | 100 B | fighter slot 0: u32 flags (bit31 = filled, bit30 = no-link) + 96 bytes of entrant data |
| 0x84 | 0x138 | 100 B | fighter slot 1 |

`np_session_entry_position` 0xB8D58 returns 1 or 2 for the fighters, else 0 ("ENTRY 1P/2P").

Writer — `np_session_set_room_data_internal` 0xB9410 [V]:
- SetRoomDataInternal {roomId = mgr+0x130, flagFilter 0x50000000 (CLOSED|HIDDEN), flagAttr, binAttr {0x57, S+0xB4, 0xE8}}
- the flagAttr is `*arg & 0x50000000`, or the cached mgr+0x13C when arg is NULL
- callers pass &0x50000000 to close and hide the room, or &0 to reopen it

The only caller is the owner, from the phase machine.

### 3. Room member data internal (id 0x59, 0x20 bytes, at S+0x19C) + teamId (S+0x1BC) [V]

Writer — `np_session_set_member_data_internal` 0xB92CC: {roomId, memberId 0 (self), **teamId = S+0x1BC**, binAttr {0x59, S+0x19C, 0x20}}.

| off | meaning |
|---|---|
| 0x00 u32 | flags: bit31 = ready for the match (`np_session_set_ready_flag` 0xBBBC4); bit30 = in match (set on entering phase 3); bit29 = queue rotated for this result (set after `rotate_queue`) |
| 0x04 u32 | Battle Points (+4 for a win, +1 for a loss; 0xACB6C) |
| 0x08 u32 | games |
| 0x0C u32 | wins |
| 0x10 u32 | X — shown as a percentage of games; meaning unknown |
| 0x14 u8.. | profile byte (rank or level [I]) and 3 unknown bytes to 0x1B |
| 0x1C u8 | **entry request**: 0 none, 1 = 1P Entry, 2 = 2P Entry (3 pad bytes) |

The byte at 0x00 is cleared to 0 after the result screen (phase 4, 0xC01C4), which clears bits 31, 30 and 29.

**teamId = your place in the waiting line** (1-based). It is 0xFF on join and create, so newcomers sort last.

The **"Match entry" button** is TaskSession vtable+0x54 = 0x25CF08:
- when room mode == 1 and `*(DAT_004010cc+0x140) ≥ 3`, it cycles 0x1C: 0 → 1 → 2 → 0, then calls SetRoomMemberDataInternal
- if that call fails, the old value is restored
- [I] the ≥ 3 test is a member count, so entries only matter in rooms of three or more

### 4. Algorithms

#### 4a. Choosing the fighters — `np_session_build_fight_entries` 0xBEBD0 (owner, phase 1) [V]

```c
need = min(cfg[1], 2);  half = need >> 1;   // 1
for m in room.members: key = (m.teamId<<16) | m.memberId;
sort by key;                                 // line order: rank, then join order
n1 = n2 = 0;
for m in sorted:
  e = m.memberBin ? m.memberBin[0x1C] : none;
  if (e == 1 && n1 < half) { n1++; k = key; }              // first 1P entrant
  else if (e == 2 && n2 < half) { n2++; k = key|0x04000000; } // first 2P entrant
  else if (e == 1 || e == 2) k = key|0x40000000;          // extra entrants
  else                        k = key|0x80000000;          // not entered
sort by k; take the first max(need,1) -> queue;
for each chosen: if (k & 0xF0000000) k = (k & 0xFFFFFF) | 0x02000000;  // fillers
sort chosen by k again, then write queue ids to blob+0x18;
// result: [1P entrant][fillers][2P entrant], so slot 0 = 1P side, slot 1 = 2P side
memset(blob+0x20, 0, 200);
return cfg[1] <= count;
```

#### 4b. Rotating the line after a match — `np_session_rotate_queue_after_match` 0xBE788 (every member, room mode 1 only) [V]

```c
// S+0x1C0 = queue position (1|2) of the WINNER
// set by np_session_report_match_result 0xBB960 when the result word has bit31 set; 0xACB6C builds it
for m in members:
  key = (m.teamId<<16) | m.memberId;
  p = entry_position(m.memberId);
  if (p && S+0x1C0) {
    if (p == S+0x1C0) {
      key = m.memberId;                          // winner -> front
      if (m == me) S+0x1B8 = (p == 1) ? 1 : 2;   // winner keeps his side's entry
    } else {
      key |= 0x80000000;                         // loser -> back
      if (m == me) S+0x1B8 = 0;                  // loser's entry is cleared
    }
  }
sort by key;
S+0x1BC /*my teamId*/ = 1 + index_of(me);
// the caller then sets flags bit29 and calls set_member_data_internal (0xBFB70 / 0xBEA9C)
```

**Propagation.** Nobody sends the whole order. Each member publishes its own new teamId, and the room member list, sorted by teamId, *is* the order. [I] This relies on every member seeing the match result (spectators run the match too). A member that did not see it would keep S+0x1C0 = 0 and would not rotate.

#### 4c. Owner phase machine — `np_session_update_room_phase` 0xBF258 (per frame, from 0xC0850) [V]

The switch was read from the disassembly at 0xBF644 (the table).

- **Phase 0 (lobby).** The owner starts once members > 1 and vtable+0x50 (0x25D190 → `np_session_entries_should_close` 0xB8C64) returns true: `(cfg[2]==0 && members ≥ cfg[1]) || countdown S+0x1CC == 0`.
  - The countdown is set from vtable+0x4C = 1800 frames, or 300 via +0x58 when room mode is 1 and ≥ 3 members.
  - On start, phase = 1 and SetRoomDataInternal with CLOSED|HIDDEN.
- **Phase 1.** `build_fight_entries`:
  - If it succeeds: phase = 2, SetRoomDataInternal, UI calls.
  - If it fails and room mode is 1: phase = 0, count = 0, reopen the room (flag 0).
  - If it fails otherwise: the session ends.
- **Phase 2 (fighters preparing).** Each fighter sends its entrant data (`np_session_send_entrant_data` 0xB6450):
  - The owner writes its own directly into blob+0x20+(p−1)*100 and sets bit31.
  - Other fighters send RUDP type 2 to the owner, who stores it (0xB7A78).
  - If a fighter has left the room: phase = 0, count = 0, clear the slots, reopen.
  - When every fighter has member flag bit31 and every slot has bit31, the owner:
    1. runs `seed_match` (vtable+0x64),
    2. writes the max RTT to blob+0x1C,
    3. calls `np_session_post_match_start` 0xBD0EC (event 6 carries the NpId, member bin data and 100-byte slot of each fighter, and my index),
    4. sets phase = 3 and calls SetRoomDataInternal.
- **Phase 3 (match).** Every member sets flags bit30 and republishes.
  - At the end of the match, each member sets S+0x1C4, runs `rotate_queue`, sets bit29 and republishes.
  - The owner sees S+0x1C4, sets phase = 4 and calls SetRoomDataInternal (room mode 1: reopens with flag 0).
- **Phase 4 (results).** When no member still has bit29 or bit30 (all cleared after the result screen), the owner moves on:
  - room mode 1: phase = 0, countdown = 1800;
  - otherwise: phase = 1.
- **Non-owners.** When the phase in the room data changes, they copy the blob. On phase 3 they call `post_match_start`.

Members leaving (0x1102):
- `np_room_event_callback` 0xB4AE0 records the leaver's id in a ring of 8 at mgr+0x144.
- In phase 3, each leaver produces event 0xD carrying his queue position, 0 for a spectator.

### 5. Host migration [V + RPCN]

Event 0x1105 (RoomOwnerChanged) moves the OWNER flag bit (member +0x44) and sets mgr+0x13A to the new owner. RPCN hands ownership on (succession list, or random; room_manager.rs:1358–1376). The game code has **no migration logic**: all state lives in the room blob and the member data, and `is_owner` (0xB8D1C: mgr+0x138 == mgr+0x13A) switches paths, so the new owner simply continues the phase machine from the room data. RoomDestroyed (0x1104) with cause 1 or 5 sets mgr+0x158 and ends the session.

### 6. RUDP messages (mesh; the owner relays) [V]

Wire format: `[u8 type][u8 dest (0xFF = broadcast; the owner re-sends to all peers, 0xB7504)][u32 flags][payload]`. The (de)serializer is `np_msg_serialize` 0xB5E6C; the factory is 0xAB2D0.

- **Type 2 (entrant data), 107 bytes:** `[2][0][u32 0][u8 slot 0/1][100 B data]`. The first data byte gets 0x40 when the sender lacks a link to the other fighter.
- **Type 0** raises event 0xA. The owner re-sends it to the other fighter when flags & 0x100.
- **Type 1** raises event 0xB.
- **Type 3** raises event 8, and the receiver replies with type 4.
- **Type 4:** an ack, event 9, when u16@+0xC is my memberId; otherwise the owner relays it.

The in-match types (0, 1, 3, 4) are not decoded further here.

### 7. Porting notes (to m2-hle2)

What maps directly onto RPCN:
- one room holds 2..8 members;
- the room binary attribute 0x57 holds the phase, the two fighter ids and the entrant data;
- each member's own binary attribute 0x59 holds ready/in-match/rotated flags, stats and the 1P/2P entry request;
- **teamId is the waiting-line rank**, recomputed by every member after each match (winner first, loser last);
- the owner alone advances the phase;
- ownership migration comes free from the room data.

---

## Part 2: PS3 Sonic the Fighters (NPUB30927): how an online room runs matches

Sources: the live Ghidra database (PPC64 BE, TOC r2=0x408800), the IDA MCP for the arcade STF ROM (port 7331) and `C:\m2\ida72\asm-check\labels.lst`.
**V** marks a claim read directly from code, and each one cites the function it came from. **I** marks an inference.
Functions I renamed in Ghidra are given by their new names, and each keeps its address.

---

### 0. Architecture in one paragraph

The wrapper sits on top of an **unmodified-address arcade i960 program**, and **i960 HLE hooks** tie the two together. The hook table at `0x413df0..0x414050` holds 76 entries of `{u32 i960_pc, u32 opd}`. The wrapper also runs a **delay-based input lockstep** in `TaskSyncIo` (instance `0x7d795c`, TOC slot `0x401268`). A star/mesh of **cellRudp** contexts carries it, with **3 RUDP contexts per peer**. The input exchange is peer to peer among the queued players. Broadcasts go through the room owner. Match membership and side assignment come from the **room's entry queue** (TaskSession `+0xC8` count, `+0xCC` u16 member ids). Queue index 0 is 1P, index 1 is 2P, and anyone else watches. There is **no savestate and no board reset per match**, as far as I can find. A match starts from the running game's attract loop, with START1|START2 forced into the game's input RAM. The game then waits at the start of character select until the lockstep is running.

Hook plumbing (V): `i960hook_GetRegs` (0x84008) returns the i960 register file: r0..r15 at +0x00..+0x3C and g0.. at +0x40, stored big-endian u32. `i960_GetWorkRamBase` (0x801a0 → 0x10deb0, `*(ctx+0x200004)`) returns a host pointer to i960 **0x500000**, which is stored little-endian. That makes `*(u32*)(base+0x68)>>24 & 2` byte `0x500068` bit 1. A hook returns the number of bytes the IP advances. `i960hook_ExecOriginal` (0x572a4) runs the hooked instruction. `i960hook_SkipInstruction` (0x57438) returns 4 or 8, which skips it. `i960hook_EmulateRet` (0x8492c) followed by return 0 performs an i960 `ret`. `FUN_0010ee20(addr,val)` and `FUN_0010ec68(addr)` write and read i960 memory with a byteswap.

The hook addresses match the arcade STF labels exactly. Examples: 0x1768 is interrupt_wait, 0x11610 is `_idle`, 0xA218 is `SEL_INT`, 0xE6EC is `VIC_INT`, and 0x83F4 is `call player_entry` in ADV_DSP. The PS3 file `rom-extracted/stf_rom/rom_code1.bin` does not byte-match `asm-check/rom.bin`. Its packaging or build differs (its IMI start_ip is 0x238, against 0xB0), but the hooked code addresses and the RAM map (0x5000xx labels) are the arcade's.

---

### 1. Message wire formats (V)

#### Classes, vtables and type ids
The factory is `CMessage_CreateFromBuffer` (0xab2d0). It switches on wire byte 0 (a jump table at 0xab348) and allocates the matching class. The vtable addresses come from the TOC slots `0x401770..0x401780`. The five vtable slots are [dtor, deleting dtor, pre (returns err), **Serialize**, post (returns err)].

| type | class | alloc size | vtable | Serialize |
|---|---|---|---|---|
| 0 | CSyncIoMsg | 0x3D4 | 0x3e8640 | `CSyncIoMsg_Serialize` 0x274940 |
| 1 | CSyncIoTcpMsg | 0x210 | 0x3e8690 | `CSyncIoTcpMsg_Serialize` 0x274598 |
| 2 | CUpdateSettingMsg | 0x74 | 0x3e86c0 | `CUpdateSettingMsg_Serialize` 0x274ed0 |
| 3 | CSyncStartMsg | 0x14 | 0x3e86f0 | `CSyncStartMsg_Serialize` 0x274290 |
| 4 | CResponseSyncStartMsg | 0x10 | 0x3e8720 | `CResponseSyncStartMsg_Serialize` 0x274030 |

The archive is `{u32 buf, u32 size, u32 pos, u8 writing, i32 err}`, and fields are copied byte-for-byte as big-endian. `FUN_000b5e6c` serialises twice: a first pass with a NULL buffer to measure, then an allocation, then the write.

#### Common header, 6 bytes (`CMessage_Serialize_header` 0x273e60)
```
[0] u8  type (0..4)
[1] u8  dest: 0x00 = unicast / direct, 0xFF = broadcast via room owner
[2..5] u32 flags  (bit 0x100 = "owner, please relay to queue", SyncIo only)
```

#### Bodies (offsets are wire offsets)
- **CSyncIoMsg (0)**: `[6] u8 slot` is the sender's queue slot (1 = 1P, 2 = 2P). `[7..8] u16 len` is followed by `len` bytes of input packet (max 0x1C0). Then comes `u16 memberId`, then `u16 len2`, then `len2` bytes (max 0x200). The second part is a side payload from `FUN_00126848(obj+0x1d4, buf, 0x200)`. On receipt it is handed to `FUN_00126740` and vfunc +0x34(memberId, 30). **I:** this is probably voice or chat, since it is not used for the game.
- **CSyncIoTcpMsg (1)**: `[6] u8 slot`, `[7..8] u16 len`, then `len` bytes. It carries the 68-byte input packet.
- **CUpdateSettingMsg (2)**: `[6] u8 side (0 = 1P, 1 = 2P)` followed by 100 bytes of the member's settings block (TaskSession/NP `+0x50`). The sender sets bit 0x40 of settings byte 0 when some queued peer's contexts are not in state 2 (`FUN_000b6450`).
- **CSyncStartMsg (3)**: `[6] u8 gen`, `[7] u8 side`, `[8..9] 0`, `[10..11] u16 senderMemberId`. Sent with dest = 0xFF.
- **CResponseSyncStartMsg (4)**: `[6..7] u16 targetMemberId`, which is the SyncStart sender being answered.

#### Transport and routing (V)
The NP globals are at 0x8e42d8 (TOC `0x4018e0` and `0x401920`). My member id is at +0x138 and the room owner's at +0x13A. "Is owner" is `FUN_000b8d1c`. There are 8 member slots at `+0x1A0 + k*0x28`: +0x10 holds the u16 member id, and the RUDP contexts `{i32 ctx, i32 state}` sit at +0x20 (ch0), +0x28 (ch1) and +0x30 (ch2). State 2 means connected. The single poll set is at +0x1A8, and the receive loop is `FUN_000b7c90` (PollWait → `cellRudpRead` 0x542 → `FUN_000b7504`).

| message | sender fn | channel | route |
|---|---|---|---|
| SyncIo, input (16 B) | `FUN_000b871c` | ch1 | **direct to every member in the entry queue** (RUDP flag 8). In relay mode (session flag 0x800000) it goes to the owner with header bit 0x100, and the owner re-sends it to the queued members other than the sender's slot (`FUN_000b7504` case 0) |
| SyncIo, to watchers (68 B) | `FUN_000b7274` | ch1 | direct to every member **not** in the entry queue. Only when relay mode is off |
| SyncIo, voice/chat only | `FUN_000b6fd4` | ch2 | every member, every 10 ticks (`FUN_000bf258`) |
| SyncIoTcp (68 B) | `FUN_000b8360` | ch0 | dest 0xFF. A non-owner sends to the owner, and the owner writes it to everyone. On receipt the owner forwards any 0xFF packet to all other members on ch0 (`FUN_000b7504` prologue) |
| UpdateSetting | `FUN_000b6450` | ch0 | a fighter sends it to the owner. The owner stores it at `session+0xD4+side*100` and sets bit 31 there |
| SyncStart | `FUN_000b6c1c` | ch0 | dest 0xFF, relayed by the owner to all. Sets `session+0x1C9 = 1` |
| ResponseSyncStart | case 3 in `FUN_000b7504` | the ctx it arrived on | goes back to the sender. The owner forwards it to the member whose id is in `[6..7]`. The addressee posts event 9 |

All of this is **cellRudp**, reliable and ordered. No plain-UDP game path exists. The `socket`/`bind` imports belong to the NP signalling layer. Receive dispatch (`FUN_000b7504`) posts: type 0 → event 10 `{slot,len,ptr}`, type 1 → event 11, type 3 → event 8 `{gen,side,0,0,memberId}`, type 4 → event 9 (for me only). Event handlers are registered in `SyncIo_Start_side_nplayers` 0x6cf74: 10→0x6d41c, 11→0x6d928, 8→0x6d1c4, 9→0x6c810.

---

### 2. Input packets and TaskSyncIo state (V)

TaskSyncIo fields (this = 0x7d795c):
- `+0x50` nPlayers, which is always 2.
- `+0x54` my side: 0 = 1P, 1 = 2P, ≥2 = watcher (`SyncIo_Shutdown` sets 2).
- `+0x57` (3 bits) and `+0x58` (5 bits): my side byte and the **match generation**.
- `+0x59` the confirmed remote generation.
- `+0x5A` "send SyncStart next tick". `+0x5B` all ResponseSyncStarts received. `+0x5C`/`+0x5D` the game has passed the SEL_INT barrier. `+0x5E` sync in progress. `+0x5F` finished / torn down.
- `+0x60/+0x64/+0x68/+0x6C` four rings: `ring[side*2 + (gen&1)]`, 0x400 entries × `{i32 frame, u16 input}`, with `0xFFFFFFFF`/`0xFFFF` meaning empty.
- `+0x70..+0x80` the last injected inputs and edges.
- `+0x84` local sample frame. `+0x88` the highest remote frame seen. `+0x8C` **play frame**, the next frame fed to the arcade (−1 means not started).
- `+0x90` current delay. `+0x94` initial delay. `+0x98` = 10.
- `+0x9C` sampling holdoff (10 ticks after a sync start).
- `+0xA0`/`+0xA4` the got / need masks of remote sides for SyncStart. `+0xA8` count of ResponseSyncStarts.
- `+0xAC` emulator speed float (1.0 runs, 0.0 stalls). `+0xB0` sampling-stalled latch. `+0xB4` stall counter.
- `+0xB8` small-packet interval. `+0xBC` big-packet interval.
- `+0xC0` sync timeout (600). `+0xC4` SyncStart resend timer (300, then 180).

**Local pad → side** (`Pad_AssignLocalToSide` 0x58478, called from 0x6cf74): side 0 maps the physical pad to logical player 0 and nothing to player 1. Side 1 is the reverse. A watcher maps no pad at all. A per-side key configuration is applied in `FUN_0006ff0c`. Each logical player has a pad structure at `Pad_GetLogical(i)` = `*0x400f68 + 0x88 + i*0x10C`, and its buttons are the u16 at +0x1C.

**Wire input byte** (`SyncIo_SampleLocalAndSend` 0x6ebfc, and the inverse in 0x6f358). Each bit of the wire byte comes from one pad bit:
| wire bit | pad bit |
|---|---|
| 0x01 | 9 |
| 0x02 | 7 |
| 0x04 | 8 |
| 0x08 | 2 |
| 0x10 | 3 |
| 0x20 | 4 |
| 0x40 | 5 |
| 0x80 | 6 |

That is one byte per frame. **I:** it probably carries 4 directions plus Guard/Punch/Kick and start.

**Small packet (16 B), sent every `+0xB8` frames** (1 by default, 2 with flag 0x400000, 4 in relay mode) through `Session_SendSyncIo_or_Spectator(pkt,16,0)` → `FUN_000b871c`:
```
[0..3]  u32 frame f (BE)
[4]     (side&7)<<5 | (gen&0x1F)
[5]     0
[6..15] input bytes for frames f, f-1, ..., f-9   (10-frame redundancy)
```
**Big packet (68 B), sent every `+0xBC` frames** (60 by default, 12 with 0x400000):
```
[0..3] u32 frame f
[4]    (side<<5)|gen
[5]    bit7 = "delay update", bits6..3 = new delay (only 1P sets it)
[6..7] 0
[8..67] inputs for frames f .. f-59
```
It goes out as **CSyncIoTcpMsg on every 60th frame** (broadcast via the owner, so it reaches watchers). When `+0xBC < 60` it also goes out as CSyncIoMsg to non-queue members (`FUN_000b7274`).

**Receive** (0x6d41c and 0x6d928): the packet is accepted when `side != mine`, `side < 2` and `gen ∈ {mine, mine+1}`. Each input goes into `ring[side][gen&1]` at `frame & 0x3FF` if that slot's frame is older. `+0x88 = max(frame)`. A non-1P side that receives the delay flag adopts `clamp(d,2,10)`.

**Delay**: only the 1P side sets it (`+0x54 == 0`, not relay mode). The ping in ms to queue[1] comes from `FUN_000b5678`. `Delay_FromPingMs` 0x644f8 turns it into frames:
- ms < 80: `int(ms*0.03 + 2.2)`
- otherwise: `int(ms*0.03 + (ms-80)*0.0025 + 2.6)`

The result is +1 with flag 0x400000 and is clamped to [2,10]. It is sent in the big packet whenever it changes. The initial value at `SyncIo_Init_rings` 0x6e67c is the same formula applied to `FUN_000b8e68()`.

**Gate** (`SyncIo_Update_gate_speed` 0x6ddf8, run each tick):
1. Once the generations match (`+0x58 == +0x59`), set `+0x4C = 1`.
2. The first play frame is `+0x94` (the initial delay). It is chosen once both sides' rings hold frames `delay..10` contiguously.
3. After the SEL_INT barrier (`+0x5C`), the speed is 1.0 only when both sides' rings hold frame `+0x8C` **and**, for a fighter, `(+0x84 − +0x8C) ≥ +0x90`. Otherwise the speed is 0.0.
4. `FUN_0011d2c8(speed)` sets the emulator time scale. Events 4 and 5 are posted when a stall starts and ends.

A watcher skips the delay condition and only waits for data. Sampling stops (`+0xB0`) after the gap has been larger than the delay for more than 60 ticks.

**Inject** (`SyncIo_InjectFrameInputs` 0x6f358, vtable 0x3eea90, called per emulated frame): for i = 0 and 1, it reads `ring[i][gen&1][+0x8C]`, converts the byte back to pad bits in logical pad i, computes pressed and released edges, and calls `FUN_00058e48(i)` → `FUN_00103de4(pad, i)`, which feeds the emulated input. Then `+0x8C++`. A missing entry sets `session+0x1C5 = 1` (`FUN_000659d4`), which is a desync/abort.

---

### 3. Match start handshake (V, unless marked I)

1. When the room's shared state turns to 3 ("match"), the NP layer (`FUN_000bf258` → `FUN_000bd0ec`) posts **event 6**. Its payload is `{u8 n = queue count, u8 mySide, n × 0xBC records}`. Each record holds the member's NP id and name, the 0x80-byte stats block, and the 100-byte settings from `session+0xD4+i*100`. `mySide` is my index in the entry queue, and defaults to 2 when I am not in it.
2. `OnMatchSetup_evt6` (0xac6e0) stores the payload at 0x8dcce0, with byte 1 = my side. `NetMatch_StateMachine` (0xacefc) then goes through its states:
   - State 0 calls `SyncIo_Start_side_nplayers(mySide, 2)` and registers event 15 → `OnMatchResult_evt15`.
   - State 1/2 waits until TaskSyncIo is ready.
   - State 3 calls `NetGameMode_Set(2, cfg)`. `cfg` is 8 bytes built from the room data at TaskSession+0xB4.. and is written into the arcade's backup-RAM settings (`FUN_0011dff0` → 0x42-byte block; the rounds/time/difficulty meaning of each byte is **I**). It seeds two PS3-side RNG slots (1 and 3) with **the room's shared seed at TaskSession+0xB4** (`FUN_000f17e8`). It then resumes the emulator task (`FUN_00122544(0)`).
3. The arcade is in attract. The hook at **i960 0x83F4** (`call player_entry` in ADV_DSP, `i960hook_83F4_player_entry_forceStart`) writes `0x30` = START1|START2 into `INTERUPT_FLAGS_MOMENTARY` (0x500704) whenever the mode is 1 or 2. In mode 2 it also calls `SyncIo_BeginNewGeneration` (0x6d2ac). That call resets the counters, clears the new generation's rings, sets `+0x9C=10`, `+0xC0=600` and `+0xC4=300`, does `gen++`, and sets `+0x5A=1`.
4. On the next tick each fighter sends **CSyncStart {gen, side}** to everyone through the owner (`Session_SendSyncStart` 0x65a1c → `FUN_000b6c1c`). Receivers post event 8. `SyncIo_OnSyncStart_evt8` (0x6d1c4) sets bit `side` in `+0xA0`, and once all remote sides named in `+0xA4` have reported it adopts the generation (`+0x59 = gen`). Every **fighter** (`FUN_000b8dd4`, queue index 0 or 1) that receives a SyncStart answers with **CResponseSyncStart{senderId}**. `SyncIo_OnResponseSyncStart_evt9` counts these up to nPlayers−1 and then sets `+0x5B` (sampling may begin). If nothing arrives, SyncStart is re-sent every 180 ticks, and after 600 ticks the match aborts (`FUN_000659d4` + `FUN_000aeec4`).
5. **Barrier at character select**: the hook at **0xA218 `SEL_INT`** (`i960hook_A218_SEL_INT_syncBarrier`) performs an i960 `ret` (skips SEL_INT) until the lockstep is running (`+0x4C && +0x8C ≥ 0`). On that frame it sets `+0x5D`, reseeds RNG slots 1 and 3 from the seed, and zeroes `frame_counter` (0x500020). The next call runs SEL_INT for real. From here on both machines feed the i960 only from the rings.

The two sides agree on these things:
- the generation;
- the seed, which is room data;
- the stage: hook 0xAF84 writes `stage_num = table[seed % 9]` in mode 2, or a PS3 random in mode 1;
- the timer, from settings (hooks 0x96AC → `GAME_TIMER` 0x500028 and 0xB0F8 → `time` 0x500090, both clamped to 30);
- the sides, which are the queue order;
- the delay, which 1P decides.

**I:** there is no explicit board reset. Both peers get from attract to SEL_INT on their own, and determinism rests on the barrier plus the zeroed frame_counter and reseeding. I did not find where, or whether, the emulator is rebooted on entering the online game scene.

---

### 4. Player positions and rotation

- (V) Side is the index in the room's **entry queue** (`session+0xC8/0xCC`, the 0xE8-byte room data blob copied to TaskSession+0xB4 by `FUN_000bf258`). Queue[0] is 1P and queue[1] is 2P. Queue[2..] are waiting players, and non-queued members are watchers (side 2). `FUN_000b8d58` gives slot 1, 2 or more.
- (V) The local physical pad only ever drives its own side (`Pad_AssignLocalToSide`). The arcade's 1P and 2P inputs are written each emulated frame from `ring[0]` and `ring[1]` by `SyncIo_InjectFrameInputs`. **Every** machine (both fighters, the waiting players and the watchers) runs the same emulation from those two streams. Nothing is written into i960 RAM for positions: inputs go through the emulated input path (`FUN_00103de4`). The one RAM write is START1|START2 into 0x500704 at attract.
- (V) Watchers and waiting players do **not** gate the lockstep. `+0xA4` counts only sides <2, ResponseSyncStart comes only from fighters, and the watcher's gate has no delay term. Waiting (queued) players get the 16-byte packets directly from both fighters. Non-queued watchers get the 68-byte 60-frame packets: every 60 frames through the owner's broadcast, or every 12 frames directly when flag 0x400000 is set. They therefore run behind.
- **End of a match** (V):
  - With session flag `0x400000` clear, the hook at **0xE6EC `VIC_INT`** (`i960hook_E6EC_VIC_INT_matchOver`) runs when the VS flag is set (byte 0x500068 bit 1) in mode 2. It calls `SyncIo_BeginNextMatchOrFinish` (0x6daf0), which tears down TaskSyncIo (`+0x5F=1`) and tells the session the match is over (`FUN_000bea9c`: room state 3 → `+0x1C4=1`, ranking via `FUN_000be788`). The owner then moves the room to state 4 (`FUN_000bf258` → `FUN_000b9410`).
  - With **0x400000 set** ("keep playing", **I**), the victory screen continues. Hook 0xE93C suppresses the Start reads (r8=0). At `CTRL_TIMER == 60` it calls 0x6daf0, and fighters **start a new generation inside the same running game**, which is a rematch without going back to the room. Hook 0xE9DC holds the victory screen timer in mode 2.
  - Watchers always take the teardown branch.
- **Rotation** (I, in the NP layer, not traced): the owner rewrites the queue order in the room data, and the next state-3 transition re-runs step 1 with new sides. `FUN_000bb960` records the per-side result and slot (`+0x1C0`), which is probably what the owner's reorder reads. Whether the winner keeps its side is not established here.

---

### 5. Match end detection (V)

The hook at **0xDC3C** (`JUDGE_DSP_INT+0x5B8`, `i960hook_DC3C_JUDGE_postResult`) sits just after the arcade's win-streak bookkeeping (0x500066 / 0x5000A2). It runs once per decided match. When the VS flag is set in mode 2, it posts **event 15** with the i960 RAM base.

`OnMatchResult_evt15` (0xacb6c) reads **`winner` = byte 0x500065** (0 = 1P won, 1 = 2P won) and updates the per-side stats blocks in the 0x8dcce0 records:
- the 1P record's stats sit at +0x3C, with flags at +0xBC;
- the 2P record's stats sit at +0xF8, with flags at +0x178;
- the winner gets 4 points and the loser 1, with a win counter and bit 31 set for the winner.

It then reports each side through `FUN_0006557c(side, stats, flags, &won)` → `FUN_000bb960`. That function stores my own stats and, in ranked rooms, uploads a score (`FUN_0011831c`). The same winner byte is read by the replay hook 0xE584.

The next step is the VIC_INT teardown or rematch described in section 4. When the session ends, event 7 → `FUN_000ace40` saves the stats.

### 6. Other hooks worth knowing
- **Boot delay and warning skip**: 0x1CC and 0x725C (the 700000-iteration delay loops) and 0x7C88 (WARNING_INT).
- **Frame and idle hooks**: 0x1768, 0x1770, 0x11608, 0x11610 and 0x11618 (`_idle`).
- **Mode 1 only**, which is a non-network special mode (**I**):
  - 0xE584 (REPLAY_DSP, winner) sets CHAR_SEL_FLAGS / CHAR_SEL_DISABLE_P2 (0x500248 / 0x50024C) `|= 5`;
  - 0xF57C (CONTINUE_DSP) sets `mode` (0x50002A) = 8.
- **Mode 2 only**: 0xA9B8 skips the `busy_signal_flag` compare in ME_MUKEN_P2_SET.
- Many 0x0xxxxx and 0x7Dxxx hooks deal with character substitution and trophies (`FUN_00057210`) and are not network related.

---

## Part 3: PS3 Sonic the Fighters (NPUB30927): the "Multi" UI and room rotation

(V) means verified in the decompile or disassembly. (I) means inferred.
The UI text ids are decoded with `ps3_ui_strings_en.txt`.
Functions I renamed in Ghidra are shown under their new names, with the old `FUN_` address.

### 0. Object map

`g_multi` = TOC `DAT_004017e8` = `0x8dd464`. It is one block holding all seven Multi tasks (V, `Multi_StaticInitDestroy` 0xb3ab0).

| offset | class (RTTI via vtable[-1]) | vtable | virtuals |
|---|---|---|---|
| +0x000 | TaskMultiMenu | 0x3e8b40 | Init af960, Update b2d80, Dest af7b8, Draw b1934, CursorMove b045c, ValueChange af8bc, CancelExitConfirm b1840, Decide b30f4 |
| +0x34c | TaskMultiMenuRule | 0x3e8b08 | Init b26b8, Update af540, Dest af750, Draw b1230, Decide b24dc, ValueConfirm b10f4, Close af830, Cancel b28ac |
| +0x5e8 | TaskMultiResult | 0x3e8a80 | Init b0544, Update b2938, Dest aecc4, Draw b06ec, CursorMove aec70, Decide b2bac |
| +0x81c | TaskMultiObserver | 0x3e8ab8 | Init aef9c, Update b0214, Dest aee68 |
| +0x950 | TaskMultiName | 0x3e8ae0 | Init b381c, Dest af204, Draw afe4c |
| +0x9b4 | TaskMultiWarning | 0x3e8b78 | Init aedf8, Update aeda8, Dest aed1c, Draw b0010 |
| +0xa38 | TaskMultiBg | 0x3e8ba0 | Init afd54, Dest afcb4, Draw affdc |

The starters are `Multi_StartMenu` af0c4, `Multi_StartObserver` af008, `Multi_StartName` af06c, `Multi_StartWarning` aefd8, `Multi_StartBg` afd18 and `Multi_StartResult` b23cc.

Two data tables are used:
- UI-id table `DAT_00401800` = 0x376a20.
- Main-menu table `DAT_004016c4` = 0x376938.

**Match-info block** = TOC `DAT_004017a0` = `0x8dcce0` (V: `OnMatchSetup_evt6` ac6e0, `np_session_post_match_start` bd0ec, `OnMatchResult_evt15` acb6c):

```
+0x000 u8  count            number of fighter records (<= 2, see §5)
+0x001 u8  mySide           0 = 1P, 1 = 2P, 2 = not fighting (watcher/waiting)
+0x004 rec[count], 0xBC each:
   +0x00 u8  valid
   +0x01 SceNpId (0x24)
   +0x25 char onlineId[17]  name shown in Result/Name
   +0x38 stats block (0x80 zeroed, 0x1C used):
           [0] Battle Points  [1] games  [2] wins  [3] disconnect counter (I)  ...  +0x18 u8 entry flag
   +0x54 settings[100]      from session +0xD4 + i*100
   +0xB8 u32 result         bit31 = WINNER, bits 28..30 = points gained ("+%d")
+0x5e4 MatchCond[0x22]      (§2); +0x602 = cond[0x1E] = ranked, +0x603 = cond[0x1F] invited, +0x605 = cond[0x21]
+0x606 u8 stay-in-room flag, +0x607 u8 "observer chose exit"
+0x608 persistent stats: BP (init -1), games, wins, [3]..., checksummed with FUN_001180a4(...,0x18,0x133048f)
```

### 1. What "Multi" is

**Answer: it is the online (PSN) mode only. It is not local multiplayer.** (V)
- Main menu (ids at 0x376990): Arcade / Offline Versus / **Online Battle** / Scoreboards / ...
  - Item 2 opens **TaskMenuMulti** (`MULTI MENU`, main+0x924, a4cf0).
  - It has two entries: **Ranked Match** (desc 0x5e) and **Player Match** (desc 0x5f) (a615c).
  - The choice ends up in main+0x970/0x978. `OnlineMode_IsPlayerMatch` a2b48 returns 0 for Ranked and 1 for Player Match.
- **TaskMultiMenu** is the ranked/player-match setup screen. Its title is 0x157 "RANKED MATCH" or 0x158 "PLAYER MATCH" (b1934). Its items come from the table at +0x94:

  | # | item | ranked | player |
  |---|---|---|---|
  | 0 | Quick Match | yes | yes |
  | 1 | Custom Match | hidden | yes |
  | 2 | Create Match | hidden | yes |
  | 3 | Matching range (Worldwide/Same Area, value at +0x50) | yes | hidden |
  | 4 | Controls | yes | yes |

  - Hide flags +0x309.., set in af960. Descriptions: 0x62/0x63/0x64/0x6b/0x76, or 0x61 for ranked.
  - In **Ranked** the draw shows your Battle Points / Games / Wins / Losses (0x17f..0x182). Losses = games − wins, and there is a disconnect meter (games ≥ 11) (V b1934).
  - Back opens 0x2a2 / 0x2a3 "Are you sure you want to exit Ranked/Player Match Mode?" (b1840).
- "Room Match" (0x156 / 0x159 / 0x60 / 0x2a4) is **not referenced** by any Multi task (grep of the range). **I:** it is a leftover from the Xbox LIVE port, where the Xbox strings (0x49, 0x187 etc.) also survive.
- Every participant is a separate PS3 or PSN account. Names are PSN online ids (§6). There is no local name entry. The arcade `name_entry` strings belong to the i960 ROM.

### 2. TaskMultiMenuRule ("RULE MENU", title 0x158)

The rule menu opens from **Custom Match** (search) or **Create Match** (create).
- Mode byte: rule+0x241 = g_multi+0x58d. 1 = Create, 0 = Custom (af960 / b30f4).
- Values: 7 ints at rule+0x1bc.. = **g_multi+0x508..+0x520**.
- Defaults come from `Multi_ResetRuleValues` aec08 and the table +0x1c = {0,1,1,0,0,0,0}.
- Maximum index table is at +0x38 = {4,3,3,3,1,1,7}.
- The value text is `label + v + 1` (b1230). In **search** mode each value may be max+1, which shows 0x179 "None specified" (= any). Matching range is the exception (b24dc).
- When you switch mode, Custom sets every value to max+1 except range = 0. Create restores the defaults (b30f4, af960).

| # | label | values (index → text) | default | notes |
|---|---|---|---|---|
| 0 | 0x15f No. of players | 0..4 → 2,3,4,5,6 | 0 (2) | Forced to 2 and locked when the NAT check fails (`FUN_00116190`, b24dc/b26b8). The strings 7 and 8 exist but are unreachable. |
| 1 | 0xdf Round count | 0..3 → 2,3,4,5 | 1 (3) | desc 0x4e "rounds required to win" |
| 2 | 0xf6 Time limit | 0..3 → 10,30,60,99 | 1 (30) | |
| 3 | 0x10f Game type | 0..3 → Type A..D | 0 | desc 0x55+v (barrier-reset rules) |
| 4 | 0x173 Secret character | Off/On | 0 | |
| 5 | 0x176 Matching range | Worldwide/Same Area | 0 | never "None specified" |
| 6 | 0x167 Private slots | 0..7 | 0 | Hidden in Custom. Max = players index + 1 (= players − 1) (b24dc, b10f4). |

**There is no winner-stays vs rotation option, and no points/wins-to-finish option.** The rules are room-search attributes and the per-match game settings.

The values become a 0x22-byte **MatchCond** (`MatchCond_Build` b3358, defaults in `MatchCond_SetDefaults` af230). It is stored at match-info+0x5e4 (`Multi_BuildMatchConditions` b3810 via ad344) and copied into the NP session (`np_session_set_config` b8f84 → session+0x10..+0x31). Layout:

| byte | meaning |
|---|---|
| [0] | **1 = Player Match, 0 = Ranked** (V b3358). This becomes room-blob **+0xB8**, the gate for rotation (§5). |
| [1] | fighters = 2 |
| [2] | players index |
| [3] | private slots |
| [4..9] | create values (players, rounds, time, type, secret, range) |
| [0xC..0x11] and [0x14..0x19] | search filters |
| [0x1C] | create |
| [0x1D] | search |
| [0x1E] | ranked |
| [0x1F] | invited |
| [0x20] | quick |

### 3. TaskMultiResult ("MULTI RESULT")

- **Start and timeout.** `Multi_StartResult` b23cc(matchInfo) starts it from accf0 when the i960 work RAM shows the game-over state.
  - Init b0544 sets a 600-frame (10 s) countdown at +0x22c, drawn "%02d".
  - Update b2938 decrements it only in Player Match (or when an invite is pending) and closes the task when it reaches 0. Ranked waits for a button.
  - It also closes if the room empties (`FUN_00064c74 < 1`).
- **Draw (b06ec).** Only **two** records are drawn, rec[0] (1P) and rec[1] (2P):
  - the name at rec+0x25;
  - **WINNER** if `rec.result` bit31 is set, otherwise **LOSER**;
  - in ranked only (`Multi_ShowPointDelta` ac790: NetGameMode==2 && +0x602), also "%dpt(s)" = rec.stats[0] (BP) and "+%d" = result bits 28..30.
  - **There is no ranking of all room members.**
- **Scoring (V `OnMatchResult_evt15` acb6c).** The winner is the byte at i960 0x500065 (0 = 1P, 1 = 2P). For each side:
  - `games++`;
  - the winner gets `wins++` and `BP += 4`; the loser gets `BP += 1` (ranked only, +0x602);
  - `result = delta<<28 | (won ? 0x80000000 : 0)`.

  Then it calls `np_session_report_match_result` bb960(session, side, stats, &result). That call:
  - stores my own stats in my member bin-attr and uploads the ranked score (`FUN_0011831c`);
  - **if result bit31 (winner) is set, records `session+0x1C0 = slot` (1 or 2) of the winning side.**

  (The 4th argument of FUN_0006557c, "won", is dropped. bb960 tests the result word's sign.)
- **Menu (b2bac)**, ids from +0x54:
  - A fighter sees: View opponent's profile (0x18f) / Exit (0x195).
  - A watcher sees: View Player 1's (0x191) and Player 2's profile (0x193) / Exit.
  - "Send a review" (0x194) is always hidden (flags +0x225..0x229, 2 = hidden).
  - The exit label becomes 0x196 "Return to setup screen" in Player Match when `FUN_0006443c` is set.
- **Session end (I, flow in ac8bc / ad198).** Ranked ends after the match: +0x606 = 0 goes back to the setup menu (state 10). In Player Match, +0x606 = !observerExited, so the player stays in the room (state 0xb) until they leave or the room has no other members.

### 4. TaskMultiObserver ("MULTI OBSERVER")

- It is started in `NetMatch_StateMachine` acefc only when **mySide > 1**, meaning you are not 1P or 2P: `Multi_StartObserver(np_session_is_owner())` sets +0x94c = isHost.
- The observer watches the live match. Their emulator is driven by the fighters' input stream (see `re_match_flow.md`). The task draws nothing of its own.
- On a button press (`FUN_00058fe0`) or an incoming invitation, Update b0214 opens the "OBSERVER_EXIT" confirm:
  - host: 0x2ba "Host players cannot leave a session in progress." (no choice);
  - otherwise: 0x2b9 "stop viewing the match and exit this session?", or 0x2bd / 0x2be when an invitation arrived.
- `Multi_ObserverExitConfirmed` b2450 (Yes, not host) returns true. accf0 then sets match-info+0x607 = 1, calls SyncIo finish (`FUN_0006daf0`) and leaves the room (`FUN_00064418`).
- `Multi_StartWarning` / TaskMultiWarning is separate. It shows 0x2c4 ("Disconnecting during a match raises your Disconnect stat...") for 180 frames (aedf8).

### 5. Rotation (V: decompile of be788 / bebd0 / bd0ec, named by the NP agent, commented by me)

The room's shared data blob (session+0xB4, 0xE8 bytes) is published by the owner. It holds:
- +0xB8 cond[0] (1 = Player Match);
- +0xC4 room state;
- +0xC8 queue count;
- +0xCC u16 queue[] (1P, 2P);
- +0xD4 2×100 settings.

Each member publishes its own member data through `np_session_set_member_data_internal` b92cc:
- bin-attr 0x59 = session+0x19C..+0x1BB (stats + **entry flag at +0x1B8**: 0 none, 1 = "1P Entry", 2 = "2P Entry");
- **teamId = session+0x1BC**, used as the **wait-order number**. It is 0xFF on join (b91f0).

**The queue holds only the two fighters.** `n = min(session+0x11, 2)` (bebd0). Everyone else has mySide 2 (watcher or waiting). There is no ">2 waiting" slot in practice.

**Step A: every client at match end** (`np_session_rotate_queue_after_match` be788, called from bea9c / bf258 when the room state is 3):

```c
if (room.type_b8 != 1) return;              // Player Match only; ranked rooms don't rotate
win = session->winSlot_1c0;                  // 1 or 2, from bb960
for (m in room.members) {
  key = m.teamId<<16 | m.memberId;
  pos = queue_pos(m);                        // 1 = 1P, 2 = 2P, 0 = not queued
  if (pos && win) {
    if (pos == win) { key = m.memberId;                          // winner to the front
                      if (m == me) me.entry_1b8 = pos; }         // re-enter SAME side
    else            { key |= 0x80000000;                         // loser to the back
                      if (m == me) me.entry_1b8 = 0; }
  }
  list.push(key, m);
}
sort_ascending(list);                         // FUN_0025e878 (I: std::sort)
me.teamId_1bc = 1 + index_of(me, list);       // new wait-order number
publish_member_data();                        // b92cc
```

**Step B: owner picks the next two** (`np_session_build_fight_entries` bebd0). It is reached through the OPD at 0x3f1ec0 / 0x3f1e18. I did not find its caller.

```c
n = min(session->nFighters_11, 2); half = n>>1;       // 1
for (m in members sorted by (teamId<<16|memberId)) {
  f = m.binattr ? m.binattr.entry_1c : 0;
  if      (f==1 && c1++ < half) cls = 0x00000000;     // first 1P-Entry
  else if (f==2 && c2++ < half) cls = 0x04000000;     // first 2P-Entry
  else if (f==1 || f==2)        cls = 0x40000000;     // extra entrants
  else                          cls = 0x80000000;     // no entry
  cand.push(cls | (key & 0xFFFFFF), m.memberId);
}
sort_ascending(cand); take first n;
for (c in taken) if (c.key & 0xF0000000) c.key = (c.key & 0xFFFFFF) | 0x02000000;   // filler
sort_ascending(taken);
queue[0] = taken[0] /*1P*/; queue[1] = taken[1] /*2P*/; count = n;
return count >= session->nFighters_11;
```

Then `np_session_post_match_start` bd0ec posts event 6, which becomes the match-info block, with mySide = my queue index or 2.

**What this does in practice** (the arithmetic is V; the player-facing reading is I):
- **The winner stays and keeps their side.** Their entry flag is re-set to their own slot, and their teamId becomes the lowest, so they win the class-0 (1P) or 0x04000000 (2P) spot.
- **The loser goes to the back of the line.** Their entry is cleared and their teamId becomes the highest.
- **The challenger is the next in line and takes the side the loser vacated:**
  - first choice is the earliest waiting member who pressed Match entry for that side (0xe ":Exit :Match entry"; 0x72 "...The top players in 1P Entry and 2P Entry get priority", drawn by the lobby at 0x68860);
  - otherwise the lowest (teamId, memberId) waiting member becomes the filler. The 0x02000000 class sorts after a 1P-entry and before a 2P-entry, so the filler lands on the empty side.
- **Order among waiters is FIFO.** Waiters keep their relative teamId order. A newcomer has teamId 0xFF, so they join at the end.
- **Characters are not handled by this layer.** They are re-chosen per match. **I:** the i960 hook 0xE584 sets CHAR_SEL flags for the winner (see `re_match_flow.md`).

### 6. TaskMultiName ("MULTI NAME")

- It is started for every participant at match start (`Multi_StartName(matchInfo)` in acefc).
- Init b381c loads layout 0x2c4 or 0x2c5 (chosen by `*FUN_0006fc88()`) and reads the two name-plate positions to +0x50 and +0x58.
- Draw afe4c prints `rec[i].onlineId` (+0x25) for each of the `count` (≤ 2) records.
- So it is the in-match **PSN name plates for 1P and 2P**, not name entry.
- Participants per room: **2..6** (rule 0, max index 4). With strict NAT it is 2. **I:** the NP layer has 8 peer slots (DAT_004018e4..0x401900).

### Renames made

af960, b2d80, b1934, b30f4, b1840, b045c, af8bc, af7b8, b26b8, af540, b1230, b24dc, b10f4, b28ac, af830, af750, b0544, b2938, b06ec, b2bac, aec70, aecc4, aef9c, b0214, aee68, b381c, afe4c, af204, aedf8, aeda8, aed1c, b0010, afd54, afcb4, affdc, af0c4, af008, af06c, aefd8, afd18, b23cc, b2450, aec08, aec4c, af1a4, afde8, afe1c, af230, af34c, b3358, b3810, b3ab0, a2b48 (`OnlineMode_IsPlayerMatch`), ac790.

I also added comments at be788 and bebd0.
