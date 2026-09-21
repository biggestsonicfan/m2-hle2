# WEB-NETPLAY.md

Netplay on **play.sonicthefighte.rs**: the WebAssembly build of *Sonic the Fighters* signing in to `rpcn.sonicthefighte.rs`, finding a match and playing it.

This expands [WEB-PORT.md](WEB-PORT.md) sections 3.4, 4, 5 and milestones M3–M4, and replaces them where the two disagree.

**Status (2026-09-21): built, tested end to end locally, and deployed.** Two browsers, each with a new account, went through the page's own screens: sign-up, lobby, host, join, start and accept. They then played a 40-second match: about 2,400 frames each, random inputs on both sides, zero stalls, no desync. That run used a gateway on this machine against a local RPCN. The gateway now also runs on the RPCN droplet, in Docker behind the existing Caddy ([web/gateway/README.md](web/gateway/README.md), "As deployed"). What remains is in section 8, and the cross-play decision (section 3).

---

## 1. Decisions

| Question | Decided | Notes |
|---|---|---|
| Where the gateway lives | `wss://rpcn.sonicthefighte.rs/gw/…`, Caddy in front on 443 | `rpcn.` is already a direct A record, so no new DNS. It is not proxied by Cloudflare, because a proxy would add a hop to every input packet. |
| Region | The existing San Francisco droplet | All web traffic crosses it (section 6), so the round trip is you → SF → your opponent. |
| Accounts | **Both** Twitch and plain RPCN accounts, including sign-up without Twitch | Name, password and e-mail. RPCN requires an e-mail and keeps it unique; it is never shown to anyone. |
| Cross-play at launch | **Web plays web** until measured (section 3) | Switched by one define, `NETPLAY_CROSS_PLAY`. |
| Transport | Two WebSockets per player | WebTransport / WebRTC are later options (section 7). |

---

## 2. What the RPCN source says

These were checked in `RipleyTom\rpcn`, not assumed.

- **A player's P2P address is the source of their UDP signaling keepalive, and only that** (`udp_server.rs`). The keepalive is 13 bytes: `[1][user_id: u64 LE][local_addr: 4]`. The TCP connection's address is logged and never used.
  - So the gateway's TCP connection to RPCN may use loopback.
  - Its UDP must leave from the droplet's public address. A loopback source would hand every desktop opponent `127.0.0.1`.
- **Two players on one public IPv4 are given each other's `local_addr`, with port 3658 hard-coded** (`cmd_misc.rs`, `room_manager.rs`). Every browser player shares the gateway's address, so browser-vs-browser always takes this branch.
- **RPCN applies no per-IP limits.** No sign-up rate limit by address and no IP bans. The one time-limited operation (`TooSoon`) is per account. Every web player sharing one address costs nothing.
- **A room keeps a copy of each member's address, taken when they create or join it, and never refreshed.** This mattered; see 5.2.

---

## 3. Cross-play: what it is and why web plays web for now

Lockstep netplay sends **inputs, not game state**. Each machine runs the whole game itself, and the two stay in agreement only if, given the same inputs, they compute *bit-identical* results frame after frame. One float rounded differently one frame, and the boards drift apart. They then show different fights, and nothing brings them back. Both boards hash their state and exchange the hash, so a split is caught (`desync_frame`). But once it is caught, all that can be done is tell the players.

"Bit-identical" depends on the **compiler** as much as the source code: how it orders float operations, whether it fuses multiply-adds, how it handles edge cases. Today there are two build families:

- **Desktop (native).** Windows builds with MSVC, and Linux and the handheld with gcc (with `-ffp-contract=off` on ARM). These have been measured to agree with each other (`tools/ab-builds.mjs`, and the ARM parity work).
- **Web (WebAssembly).** Built with clang for wasm, and deterministic against itself: two wasm runs are identical, and wasm floats are specified bit-for-bit on every machine. So **web vs web is as safe as desktop vs desktop.** But it was measured **31 instructions in 134 million** away from the gcc x86-64 build over an attract sequence (WEB-PORT.md section 8). It has never been compared with MSVC, which is what desktop players actually run.

That gap is small, but netplay has no tolerance for small. If web and desktop players were matched today, some matches would end with "the two games stopped matching" partway through. Two players who never see each other's lobby are a better experience than a match that falls apart.

**What happens now.** A room carries its build family in its attribute word. The web version shows desktop rooms greyed out with the reason ("that match is on the desktop version, and the web and desktop versions cannot play each other yet"), and the desktop does the same for web rooms. They are shown, not hidden, so "nobody online" and "someone online on the other version" read differently.

The field sits in the top two bits of the protocol-revision byte. Bits 28–31, which the proposal first chose, belong to the RPCN server. A web room's revision byte therefore reads `0x41`, which **desktop builds already released** read as "a different netplay protocol" and refuse with a sentence. No desktop release is needed to keep the two apart.

**Turning it on.** This is the determinism gate, WEB-PORT.md milestone M2:
1. Run the attract replay fight (and a fight that uses the afterimage op `0x80`, ported since the last measurement) on wasm and on MSVC.
2. Compare the per-frame check values.
3. If they differ, find the first frame that differs and diff that frame's COP conversation.

Once they are identical, set `NETPLAY_CROSS_PLAY 1` (netplay.h) and ship both builds. Old desktop builds will still refuse web rooms, because of the revision byte, so cross-play needs the new desktop build on the desktop side. If the gap turns out to be real and small, it is a board-level fix like the FMA and float→int fixes before it, and it benefits everyone.

---

## 4. The gateway ([web/gateway/](web/gateway/))

Node plus `ws`, about 400 lines, deployed by hand. It is never deployed by `pages.yml`.

- **`/gw/stream`** relays one TLS connection to RPCN.
  - The upstream is fixed in config. Its certificate is either pinned by SHA-256 fingerprint (`rpcn.fingerprint`, for a self-signed one) or validated by chain and name (`rpcn.servername`, for a CA-issued one, which survives renewal). The droplet's is Let's Encrypt, so it uses the name.
  - One WebSocket is one upstream for its whole life, never pooled or reconnected, because the Twitch device flow must stay on one connection.
- **`/gw/dgram`** gives each player a UDP socket on the public address (from a fixed port range) and a **virtual address** from `100.64.0.0/16`. Each outgoing datagram, framed `[ip: 4][port: u16 BE][payload]`, is routed one of four ways:
  1. To the **signaling tag** `100.127.255.254:3657`: the gateway writes the player's virtual address into the keepalive's `local_addr` field and sends it to RPCN's helper. Replies come back labelled with the tag.
     - The browser cannot resolve names, so the client sends to this constant.
     - The client never needs to know its own virtual address, so there is no race against a hello message.
  2. To **a virtual address, port 3658**: delivered inside the gateway to that player, labelled `<sender's virtual address>:3658`, which is exactly what RPCN told the receiver to expect. A datagram to your own address is dropped.
  3. To **0.0.0.0:0**: echoed straight back. This is the page's round-trip probe.
  4. **Anywhere else**: public unicast addresses only, ports ≥ 1024, never the gateway's own address. It goes out of the player's UDP socket. This is how a browser reaches a desktop player, who needs no change at all.
- **Limits:**
  - The `Origin` must be the site.
  - 6 streams and 4 datagram channels per client IP.
  - 240 datagrams/s and 64 KB/s per player, 1,200-byte payloads.
  - Idle streams are closed after 15 minutes, and dead browsers are found by WebSocket ping (every 30 s; no pong by the next ping ends the socket).
  - *The heartbeat's pong handler has to be attached in the `handleUpgrade` callback,* not on the server's `connection` event, which `handleUpgrade` never emits. Registered there, no pong was ever recorded and the live gateway dropped every signed-in session 30-60 s after it opened ("the connection to the gateway closed"). A test now holds a connection through six heartbeats.
- **It never logs a payload byte.** It sees RPCN's protocol in the clear, login tokens included.
- `npm test`: the routing rules, plus an end-to-end run against a stand-in RPCN, signaling helper and desktop peer (15 tests). CI runs them before every deploy.

---

## 5. The client

### 5.1 The web backends

- **`src/net/web_socket.h`** is the only new C file: WebSockets, via `EM_JS`.
  - A socket is usable the moment it is opened. Sends queue until it opens, and a failed open reads as a close with the gateway's reason.
  - The netcode stays a polled state machine on the one thread the web build has: no pthreads, no Asyncify.
- **`net_socket.h`**: in the web build, the UDP functions speak the datagram framing. `net_resolve_ipv4` answers the signaling tag, and `net_local_ipv4_towards` answers 0.
- **`tls.h`** gains a third backend beside Schannel and the stub. It is not TLS at all: the browser encrypts to the gateway and checks its certificate.
- **`netplay.h`**:
  - Settings go to `localStorage` under the settings file's name.
  - The build family is added to the room word and to `netplay_room_reject_reason`.
- **`main_web.c`**: `web_netplay_begin` / `_set` / `_post` stage a command a field at a time. A password can hold any character, and the minimal JSON reader cannot. Also added:
  - `web_netplay_status` (a JSON snapshot)
  - `web_netplay_signout` (forgets the Twitch token and any remembered password)
  - `web_netplay_set_gateway` (`?gw=`)
  - `web_background_tick`

### 5.2 Found and fixed in the shared netcode

Running two web clients on one machine found two bugs that the desktop build shares. The fixes help it too.

- **The barrier could release on one side only.**
  - Sequence: the host presses Start and announces every 50 ms until its barrier releases. The guest accepts and announces once. The host hears that announce and releases, and stops announcing. The guest is left waiting for an announce sent *after* it entered the round, which will never come.
  - Whether a match started came down to an announce being in flight, a window of roughly the one-way latency out of 50 ms. That is almost always true over the internet and almost never on one machine.
  - Fix (`lockstep.h`): a peer's input record for the current round also counts as its announce, because a peer only sends inputs once its own barrier has released. Covered by `tests/net_test.c`.
- **A room taken too soon advertised no address.**
  - RPCN copies a member's address into the room when the room is created or joined. The address only reaches RPCN with the first keepalive after login. The test hosted 0.2 s after signing in and snapshotted zeros.
  - Two players on one public address were then told *different kinds* of address for each other: one got the other's public address from the room, the other got the local address from a lookup. Each discarded the other's datagrams as strays.
  - Fixes:
    - Host and Join wait until the signaling helper has answered (`netplay_take_room`), for at most 4 s.
    - An address the peer has actually been *heard* from is never replaced by one the server reports later (`rpcn_session_set_peer`).

A third fix is web-only (`main_web.c`). When a slice waits on the other player, the web frontend drops that owed time instead of repaying it. Repaid, it put the waiting board straight back a fraction of a frame ahead, so it waited again every frame: ~40 waits a second, indefinitely, after a hidden tab came back.

### 5.3 A hidden tab

A hidden tab gets no animation frames. While a match is on, a small Worker posts a tick every 16 ms and the page runs the board and sound from it, with no picture.

- *Measured in headless Chrome:* the hidden board kept ~57 fps, and the opponent never stalled out.
- Not yet measured in Firefox or Safari.

### 5.4 The panel ([web/site/m2hle-netplay.js](web/site/m2hle-netplay.js))

The **Play online** button in the bar appears once the game is loaded.

- **Signed out:** **Sign in with Twitch**, or an RPCN account with two tabs, *Sign in* and *Create an account*. Sign-in has "the server e-mailed me a code" for servers that validate by e-mail, as the live one does: after a sign-up there the panel moves to sign-in with the details filled in and the code box open, and offers to send the e-mail again. A returning player sees "Welcome back, *name*" and **Continue**.
- **Twitch:** the code, large, with **Copy code** and a real **Open Twitch** link. It is an `<a>`, so no popup blocker applies, and the URL must start with `https://`.
- **Lobby:** **Create a match**; open matches with **Play**; rooms that cannot be joined shown greyed with the reason; a connection-quality line from the gateway round trip. Under *Advanced*: input delay (automatic from the round trip, or 2–6) and a private-match password.
- **Room:** "Waiting for an opponent…" → "*name* joined" / **Start match** → "*name* is ready" / **Accept**, and "Both games restart together when the match begins". A desync is shown in words. The panel folds away once, when the match starts, and the bar says who you are playing; opened again mid-match it stays open, with **End match** and **Leave**.
- Keys typed into the panel never reach the game.
- The page loads no script from another site: the sign-in lives in this origin's `localStorage`, and CI now fails the deploy if a `<script src>` names another host.

---

## 6. Costs and limits

- **Everything web goes through the droplet.** Browser-vs-browser is never peer to peer, and browser-vs-desktop is half relayed. For the same reason, hole punching can never fail on the web side.
- **WebSocket is TCP.** A lost packet holds up those behind it for about a round trip, where UDP plus lockstep's re-sends would not notice. A lossy link shows brief freezes that the desktop build would not have.
- **Load:** well under 100 bytes per record, about 10 KB/s per player. The limit is abuse, not capacity.

---

## 7. Later, if latency is the complaint

- **WebTransport datagrams** in place of the datagram WebSocket: no head-of-line blocking. This needs an HTTP/3 server beside the gateway.
- **WebRTC data channels** between two browsers: a direct path for web-vs-web, with the gateway passing the offers. This needs STUN and a TURN fallback (`coturn` on the droplet). It does nothing for web-vs-desktop.

The datagram seam in `net_socket.h` is narrow enough that either can replace the WebSocket without touching `lockstep.h`.

---

## 8. What is not done

- **Deploy:** the gateway is up on the droplet ([web/gateway/README.md](web/gateway/README.md), "As deployed", which also lists what was still open there: the certificate Caddy serves for `rpcn.`). Not recorded as done: the RPCN fork's `pick_free_npid` fix, without which a Twitch sign-up whose lowercase name collides with an existing account fails.
- **Twitch's success path** (the code and link screen) has not run: the local RPCN has no Twitch client ID. The failure path has run.
- **A desktop client against a web room** has not been run. The refusal logic is in code, and the room word it depends on was checked (`0x41` low byte), but nothing has been held against a real desktop client.
- **Firefox and Safari**, the hidden-tab worker there, and real distances for the automatic input delay.
- **The determinism gate** (section 3).

---

## 9. Testing

Details are in [web/gateway/README.md](web/gateway/README.md), "Testing locally".

- `cd web/gateway && npm test`: the gateway alone. `node web/gateway/test/probe-live.mjs`: the deployed one, from outside.
- `node tools/web-netplay.mjs --seconds 40`: two headless browsers play a match through the real page, a local gateway and a local RPCN.
  - `--hide-a 10` hides one tab mid-match.
  - `--ui-shots DIR` saves the panel at each step.
- `tests/net_test.c`: the lockstep barrier, including the record-releases-barrier case.

**Never test against the live RPCN with the owner's own account or Twitch token.** Two clients on one account each hold a token, only one stays good, and the owner streams with theirs.
