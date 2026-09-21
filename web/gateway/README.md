# The web gateway

Lets the browser build at **play.sonicthefighte.rs** reach the RPCN server at
**rpcn.sonicthefighte.rs**. A browser tab cannot open TCP or UDP sockets, so each
web player opens two WebSockets here instead:

| Path | What it carries |
|---|---|
| `/gw/stream` | One TLS connection to RPCN (fixed by config; the page cannot name a host). |
| `/gw/dgram` | One UDP socket on the droplet's public address, plus a virtual address from a private pool. One WebSocket message is one datagram: `[ip: 4][port: u16 BE][payload]`. |

Browser-to-browser traffic is routed inside the gateway through the virtual
addresses; browser-to-desktop goes out of the session's real UDP socket. The
design, and why each rule exists, is in [WEB-NETPLAY.md](../../WEB-NETPLAY.md),
section 4. Nothing in RPCN changes, and the desktop build needs no change.

**It never logs payload bytes.** It sees the RPCN protocol in the clear, login
tokens included, which is why it runs on the RPCN host and nowhere else.

## As deployed (2026-09-21)

The droplet runs everything in Docker, so the gateway does too:

- `/root/m2hle-gateway/`: `gateway.mjs`, `rules.mjs`, `package*.json`, `node_modules`
  (from `docker run --rm -v $PWD:/app -w /app node:22-alpine npm ci --omit=dev`),
  `config.json` and [docker-compose.yml](docker-compose.yml). Start it with
  `docker compose up -d`; logs are in `docker logs m2hle-gateway`.
- It uses **host networking**, for the UDP range on the public address and RPCN on
  `127.0.0.1`. It listens **only on 172.18.0.1:8787**, the `forgejo_forgejo` bridge,
  where the existing `caddy` container reaches it. `trustProxy` is that bridge's
  subnet, so client addresses come from Caddy's `X-Forwarded-For`.
- RPCN's certificate is Let's Encrypt, so the config verifies it by name
  (`"servername": "rpcn.sonicthefighte.rs"`) rather than pinning a fingerprint that
  would change at every renewal.
- `/root/forgejo/Caddyfile` routes `handle /gw/*` in the `rpcn.sonicthefighte.rs`
  block to `172.18.0.1:8787`. It is a single-file bind mount: edit it in place
  (`cat new > Caddyfile`), not by replacing the file, or the container keeps the old one.
  Then `docker exec caddy caddy reload --adapter caddyfile --config /etc/caddy/Caddyfile`.
  The copy from before this change is `Caddyfile.bak-20260921-webgw`.
- ufw: `40000:40999/udp` from anywhere, and `8787/tcp` only in on `br-7da2cac187b6`
  (the bridge) to `172.18.0.1`.
- `node test/probe-live.mjs` checks it from outside without signing in: health, the
  origin check, the stream reaching RPCN, the echo, and the address RPCN's helper
  sees (it must be `143.198.49.181:4xxxx`).

**Open at the time of writing: the certificate on `rpcn.sonicthefighte.rs:443`.**
The `noclip` block loads a `*.sonicthefighte.rs` Cloudflare Origin certificate, so
since 2026-08-31 Caddy has served that for `rpcn.` too ("skipping automatic
certificate management because one or more matching certificates are already
loaded") and stopped renewing rpcn's own Let's Encrypt certificate (expires
2026-10-30). `rpcn.` is not proxied by Cloudflare, so browsers refuse the Origin
certificate and the WebSocket cannot open. The fix is `tls { issuer acme }` in the
`rpcn.sonicthefighte.rs` block, so Caddy manages that exact name again.

RPCN on 31313 uses a copy of the same Let's Encrypt certificate (in the
`rpcnstorage` volume, copied 2026-08-01), and nothing renews that copy either.

## Deploying on another host

Everything here is the owner's to do. Order matters only for step 1.

1. **Deploy the RPCN fork's `pick_free_npid` fix first.** Without it, a Twitch
   login whose lowercase name collides with an existing account (usernames are
   unique case-insensitively) fails with "no free account name could be derived
   from that Twitch login". Every web player who uses Twitch signs up through
   that path.
2. **Node 20 or newer**, and this directory copied to `/opt/m2hle-gateway`:
   ```
   sudo mkdir -p /opt/m2hle-gateway && sudo cp gateway.mjs rules.mjs package.json package-lock.json /opt/m2hle-gateway/
   cd /opt/m2hle-gateway && sudo npm ci --omit=dev
   ```
3. **Config**: copy `config.example.json` to `/etc/m2hle-gateway.json` and fill in
   RPCN's certificate fingerprint, the same value the desktop's pin box holds:
   ```
   openssl x509 -in /path/to/rpcn/cert.pem -noout -fingerprint -sha256
   ```
   If RPCN's certificate is CA-issued (as on the droplet), leave `fingerprint` out and
   set `"servername"` to its name instead: a pin would break at every renewal.
   `udp.bind` and `signaling.host` must be the droplet's **public** address,
   `143.198.49.181`, never `127.0.0.1`. RPCN records a player's address from the
   source of their UDP keepalive, and a loopback source would hand every desktop
   opponent `127.0.0.1`. The TCP side (`rpcn.host`) may be loopback.
4. **Firewall** (DigitalOcean cloud firewall, and ufw if it is on): allow inbound
   TCP 80 and 443, and **UDP 40000–40999**. RPCN's 31313/TCP and 3657/UDP rules
   stay as they are.
5. **Caddy** in front, for the certificate: install it, use `Caddyfile` from here
   (or add its `handle /gw/*` block to an existing site). If something else already
   holds 443, put the same `/gw/` location in that server instead: the gateway only
   needs WebSocket upgrades passed through and `X-Forwarded-For` set.
6. **systemd**: `m2hle-gateway.service` from here. The header of that file has the
   three commands.
7. **Check it**: `curl https://rpcn.sonicthefighte.rs/gw/health` answers
   `{"ok":true,...}`. Then open play.sonicthefighte.rs, load the game, and press
   **Play online**.

## Testing locally

`npm test` runs the rules and an end-to-end test against stand-in servers
(no RPCN needed).

For the whole thing on one machine (a local RPCN, this gateway, the web build, two
headless browsers playing each other):

```
# a scratch copy of RPCN: rpcn.exe + its .cfg files + ticket_public.pem, then
rpcn --cert-gen && rpcn
# a gateway config: origins ["http://localhost:8080"], rpcn 127.0.0.1:31313 with the
# fingerprint of that cert.pem, signaling + udp.bind 127.0.0.1, allowPrivateDestinations true
node web/gateway/gateway.mjs --config gw-local.json
node tools/web-serve.mjs --site build_web/site --rom path/to/merged.zip
node tools/web-netplay.mjs --seconds 40                # two web players, a whole match
node tools/web-netplay.mjs --seconds 30 --hide-a 10    # with one tab hidden for 10 s
```

The page takes `?gw=ws://127.0.0.1:8787/gw` to use a local gateway; `web-netplay.mjs`
passes it for you.
