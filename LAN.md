# Direct play: LAN cabinets and Tailscale (Pinboard #1891)

A rollback (GGPO) match straight from one machine to another: no RPCN, no GGPO
lobby, no server of ours. One player **hosts** on a port, the other **joins** the
host's address. Anything that routes between the two machines works: the same
LAN, a [Tailscale](https://tailscale.com) tailnet, or a port forwarded on the
host's router. Both must run the same build (the host refuses another netplay
revision) and the same game (the same profile: Arcade or Console). The joiner
plays in the host's region for the match.

The host is player 1, the joiner player 2. Both boards cold boot together, as in
a lobby match. After a result, "go again" ends the match: a host waits for the
next player, and a joiner joins again.

## Desktop

**Netplay > Direct play (LAN / Tailscale)**. *Host* on a port (7000 by default):
the window shows the addresses to give the other player, the LAN one and, with
Tailscale up, the tailnet one (`100.x.y.z`). Or type the host's address
(`192.168.1.20:7000`, `100.101.102.103:7000`, a MagicDNS name) and *Join*.

From the command line: `m2hle --rom sfight.zip --lan-host 7000` and
`m2hle --rom sfight.zip --lan-join 100.101.102.103:7000`. Over the MCP bridge:
`{"cmd":"ggpo_lobby","do":"lan_host","port":"7000"}`,
`{"cmd":"ggpo_lobby","do":"lan_join","addr":"IP:PORT"}`.

The host needs its port open to **UDP** (and TCP, for browsers). A firewall
that asks: allow m2hle. Over Tailscale nothing else is needed.

## RetroArch (libretro core)

Two ways, both direct:

- **Online play "RetroArch"**: RetroArch's own netplay already connects
  straight to an IP and port (Netplay > Host / Connect to Netplay Host). Use the
  host's LAN or Tailscale address.
- **Online play "GGPO"** with a line in `m2hle-ggpo.cfg` (the saves folder):
  `lan_host=7000` on one machine, `lan_join=100.101.102.103:7000` on the other.
  Rollback rather than lockstep; the notifications say where the host is and when
  the match starts.

## Web

The web version can join a desktop or RetroArch host, not host (a browser has
no UDP and cannot listen). Open the **GGPO lobby** panel and use **Direct play**:

- The site is https, and an https page may only open `wss://`. On Tailscale the
  host gets one with its tailnet certificate, through Tailscale's HTTPS proxy
  (it passes WebSockets on):
  `tailscale serve --bg --https=8443 http://127.0.0.1:7000`. The joiner types
  `wss://<host>.<tailnet>.ts.net:8443`. (HTTPS certificates must be enabled for
  the tailnet.)
- A page served over plain http (your own copy, `node tools/web-serve.mjs`) can
  join `ws://IP:PORT` directly, LAN or tailnet.
- `?lan=wss://host.tailnet.ts.net:8443` in the page's address fills it in and
  joins when the panel opens.

## How it works

`src/net/ggpo_lan.h`; the invariants are in
[docs/invariants/ggpo.md](docs/invariants/ggpo.md), *Direct play*.
