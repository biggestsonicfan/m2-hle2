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

## Deploying on the RPCN droplet

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
