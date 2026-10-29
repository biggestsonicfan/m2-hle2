/*
 * ggpo_lan.h — a GGPO match straight to an address: LAN cabinets, Tailscale
 * (Pinboard #1891).
 *
 * No lobby, no RPCN, no server of ours: one machine hosts on a port, the other
 * joins its address, and the match is the lobby's (a cold boot on both boards,
 * then rollback netplay, emu_ggpo.h). Anything that routes between the two
 * machines will do: a LAN, a Tailscale tailnet (100.x addresses), a forwarded
 * port.
 *
 *   host   UDP on the port, and TCP on the same port for a browser (below).
 *          The host is player 1. It plays the first JOIN that matches it, and
 *          after the match it waits for the next one.
 *   join   "IP:PORT" (or a name), UDP from any port; or a ws:// / wss:// URL,
 *          the browser's way. The joiner is player 2.
 *
 * Datagrams, both ways (the browser's are WebSocket binary messages):
 *
 *   "M2LN" | JOIN (1)   | rev | [len][game id]    every 250 ms until answered
 *   "M2LN" | ACCEPT (2) | [len][game id] | region the match is on, in the
 *                                                 host's region
 *   "M2LN" | REFUSE (3) | [len][why]              another game, another
 *                                                 build, a match already on
 *   "M2LN" | BYE (4)                              the player left
 *   anything else from the peer is GGPO's own
 *
 * rev is NETPLAY_PROTO_REV: two builds that would not run the same frames
 * do not start. The region (g_region) is a setting, not the build's, and the
 * page picks it from its language: the joiner plays the match in the host's
 * and has its own back after it.
 *
 * A browser has no UDP and cannot listen, so the web build only joins, over a
 * WebSocket the host accepts on its TCP port (a plain RFC 6455 server below:
 * the upgrade, masked frames in, unmasked out). A page served over https may
 * only open wss://, so a host the page reaches needs TLS in front of that
 * port: `tailscale serve` does it with the tailnet's certificate (LAN.md).
 *
 * As in the lobby, GGPO is told its peer is GGL_VIRTUAL_IP and gets this
 * file's transport, and the status is the lobby's (g_ggl.st): stage
 * GGL_LAN_HOST / GGL_LAN_JOIN while waiting, GGL_MATCH (match "lan") while
 * playing, so the go-again prompt, the bridge and the page need nothing new
 * to follow a match.
 *
 * Threads: all of it runs on the emu thread, from ggl_pump (outside the emu
 * mutex) and inside GGPO's receive (under it), as the lobby's transport does.
 *
 * Header-only. Included from ggpo_lobby.h (M2HLE_GGPO builds only).
 */
#ifndef M2HLE_GGPO_LAN_H
#define M2HLE_GGPO_LAN_H

#define GGL_LAN_PORT       7000    /* the default host port */
#define GGL_LAN_JOIN_MS    250
#define GGL_LAN_JOIN_FOR   15000   /* ms a joiner waits for an answer */
#define GGL_LAN_CONNS      4       /* browsers at the host's door at once */
#define GGL_LAN_UPGRADE_MS 5000    /* for a browser's upgrade request */

enum { GGL_LAN_JOIN_PKT = 1, GGL_LAN_ACCEPT = 2, GGL_LAN_REFUSE = 3, GGL_LAN_BYE = 4 };

#ifndef __EMSCRIPTEN__
/* A browser's WebSocket at the host. */
typedef struct {
    bool       open, upgraded;
    net_sock_t sock;
    uint32_t   ip;
    uint16_t   port;
    int64_t    since;
    uint32_t   in_used;
    uint8_t    in[4096];
} ggl_lan_conn_t;
#endif

static struct {
    bool       on;              /* hosting or joining */
    bool       host;
    bool       ws;              /* the peer is on a WebSocket */
    bool       matched;         /* the match's transport is ours */
    bool       ws_open;         /* the joiner opened the lobby's WebSocket */
    char       game[GGL_NAME_MAX];
    char       addr[256];       /* the host's address, as the joiner was given it */
    uint16_t   port;
    net_sock_t udp;
    bool       udp_open;
    uint32_t   peer_ip;
    uint16_t   peer_port;
    int64_t    join_next, join_until;
    bool       region_taken;    /* the joiner plays in the host's region... */
    int        region_own;      /* ...and has this one back after it */
#ifndef __EMSCRIPTEN__
    net_sock_t     lsock;
    bool           lsock_open;
    ggl_lan_conn_t conn[GGL_LAN_CONNS];
    int            peer_conn;   /* the browser playing, -1 none */
#endif
} g_lan;

static inline bool ggl_lan_on(void) { return g_lan.on || g_lan.matched; }

static inline void ggl__match_done(const char *why);

/* ---- The browser's door (native host) ------------------------------------ */

#ifndef __EMSCRIPTEN__

/* SHA-1, for the upgrade's Sec-WebSocket-Accept alone. */
static inline uint32_t ggl_lan__rol(uint32_t v, int n) { return v << n | v >> (32 - n); }

static inline void ggl_lan__sha1(const uint8_t *msg, uint32_t len, uint8_t out[20]) {
    uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
    uint8_t blk[64];
    uint64_t bits = (uint64_t)len * 8;
    uint32_t total = (len + 8) / 64 * 64 + 64;
    for (uint32_t off = 0; off < total; off += 64) {
        for (uint32_t i = 0; i < 64; i++) {
            uint32_t p = off + i;
            blk[i] = p < len ? msg[p] : p == len ? 0x80 : 0;
            if (p >= total - 8) blk[i] = (uint8_t)(bits >> (8 * (total - 1 - p)));
        }
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)blk[4 * i] << 24 | (uint32_t)blk[4 * i + 1] << 16 | (uint32_t)blk[4 * i + 2] << 8 | blk[4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = ggl_lan__rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
            uint32_t t = ggl_lan__rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = ggl_lan__rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 20; i++) out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
}

static inline void ggl_lan__base64(const uint8_t *in, int n, char *out) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = b64[(v >> 18) & 63];
        out[o++] = b64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? b64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? b64[v & 63] : '=';
    }
    out[o] = '\0';
}

/* Sec-WebSocket-Accept for a request's key (RFC 6455 section 4.2.2). */
static inline void ggl_lan__ws_accept(const char *key, char out[29]) {
    char cat[128];
    int n = snprintf(cat, sizeof cat, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    uint8_t sha[20];
    ggl_lan__sha1((const uint8_t *)cat, (uint32_t)(n > 0 && n < (int)sizeof cat ? n : 0), sha);
    ggl_lan__base64(sha, 20, out);
}

static inline void ggl_lan__conn_close(int i) {
    ggl_lan_conn_t *c = &g_lan.conn[i];
    if (c->open) net_close(&c->sock);
    c->open = c->upgraded = false;
    c->in_used = 0;
}

/* One frame to a browser: binary, or a control frame; servers do not mask. */
static inline bool ggl_lan__conn_send(int i, uint8_t opcode, const void *data, uint32_t len) {
    ggl_lan_conn_t *c = &g_lan.conn[i];
    if (!c->open || !c->upgraded || len > 65535) return false;
    uint8_t f[4 + 1600];
    uint32_t h = 2;
    f[0] = (uint8_t)(0x80 | opcode);
    if (len < 126) f[1] = (uint8_t)len;
    else { f[1] = 126; f[2] = (uint8_t)(len >> 8); f[3] = (uint8_t)len; h = 4; }
    if (h + len > sizeof f) return false;
    memcpy(f + h, data, len);
    return net_tcp_send_all(c->sock, f, h + len);
}

/* The upgrade request, once it is all in: 1 upgraded, 0 not yet, -1 refused. */
static inline int ggl_lan__conn_upgrade(int i) {
    ggl_lan_conn_t *c = &g_lan.conn[i];
    c->in[c->in_used < sizeof c->in ? c->in_used : sizeof c->in - 1] = 0;
    char *end = strstr((char *)c->in, "\r\n\r\n");
    if (!end) return c->in_used + 1 >= sizeof c->in ? -1 : 0;
    char *k = (char *)c->in;
    char key[64] = "";
    while ((k = strchr(k, '\n')) != NULL && k < end) {
        k++;
        if (ws_relay__strnicmp(k, "Sec-WebSocket-Key:", 18) != 0) continue;
        k += 18;
        while (*k == ' ') k++;
        size_t n = strcspn(k, "\r\n ");
        if (n < sizeof key) { memcpy(key, k, n); key[n] = '\0'; }
        break;
    }
    if (!key[0]) {
        static const char no[] = "HTTP/1.1 426 Upgrade Required\r\nUpgrade: websocket\r\nContent-Length: 0\r\n\r\n";
        net_tcp_send_all(c->sock, no, (uint32_t)sizeof no - 1);
        return -1;
    }
    char accept[29], reply[256];
    ggl_lan__ws_accept(key, accept);
    int n = snprintf(reply, sizeof reply,
                     "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    if (!net_tcp_send_all(c->sock, reply, (uint32_t)n)) return -1;
    uint32_t head = (uint32_t)(end + 4 - (char *)c->in);
    memmove(c->in, c->in + head, c->in_used - head);
    c->in_used -= head;
    c->upgraded = true;
    return 1;
}

/* The next whole frame's binary payload, unmasked in place: its length, 0 for
 * none yet (or a control frame, answered), -1 to close. */
static inline int ggl_lan__conn_frame(int i, uint8_t *out, uint32_t cap) {
    ggl_lan_conn_t *c = &g_lan.conn[i];
    if (c->in_used < 2) return 0;
    uint8_t b0 = c->in[0], op = b0 & 0x0F;
    uint64_t len = c->in[1] & 0x7F;
    uint32_t h = 2;
    if (!(c->in[1] & 0x80)) return -1;              /* a client must mask */
    if (len == 126) {
        if (c->in_used < 4) return 0;
        len = (uint64_t)c->in[2] << 8 | c->in[3];
        h = 4;
    } else if (len == 127) {
        return -1;                                  /* nothing of ours is that large */
    }
    if (h + 4 + len > sizeof c->in) return -1;
    if (c->in_used < h + 4 + len) return 0;
    const uint8_t *mask = c->in + h;
    uint8_t *pl = c->in + h + 4;
    for (uint32_t k = 0; k < (uint32_t)len; k++) pl[k] ^= mask[k & 3];
    int got = 0;
    if (op == 0x8) return -1;                       /* close */
    if (op == 0x9) ggl_lan__conn_send(i, 0xA, pl, (uint32_t)len);   /* ping */
    else if (op == 0x2 && (b0 & 0x80) && len <= cap) {
        memcpy(out, pl, (size_t)len);
        got = (int)len;
    }
    uint32_t total = h + 4 + (uint32_t)len;
    memmove(c->in, c->in + total, c->in_used - total);
    c->in_used -= total;
    return got;
}

/* What the browser has sent, read without blocking; false once it is gone. */
static inline bool ggl_lan__conn_fill(int i) {
    ggl_lan_conn_t *c = &g_lan.conn[i];
    if (c->in_used + 1 >= sizeof c->in) return true;
    int got = (int)recv(c->sock, (char *)c->in + c->in_used, (int)(sizeof c->in - 1 - c->in_used), 0);
    if (got > 0) { c->in_used += (uint32_t)got; return true; }
    if (got == 0) return false;
    return net_would_block(net_errno());
}

#endif

/* ---- Datagrams ----------------------------------------------------------- */

static inline bool ggl_lan__is_ctl(const uint8_t *b, int n) { return n >= 5 && memcmp(b, "M2LN", 4) == 0; }

/* A datagram to wherever `to` is: the joiner's WebSocket, a browser at the
 * host (conn >= 0), or UDP. */
static inline bool ggl_lan__send_to(int conn, uint32_t ip, uint16_t port, const void *b, uint32_t n) {
#ifndef __EMSCRIPTEN__
    if (conn >= 0) return ggl_lan__conn_send(conn, 0x2, b, n);
    if (!g_lan.host && g_lan.ws) return ggl__ws_send(true, b, n);
    return net_udp_send(g_lan.udp, ip, port, b, n);
#else
    (void)conn; (void)ip; (void)port;
    return ggl__ws_send(true, b, n);
#endif
}

static inline void ggl_lan__ctl(int conn, uint32_t ip, uint16_t port, uint8_t type, const char *text) {
    uint8_t b[6 + 200] = { 'M', '2', 'L', 'N', type };
    uint32_t n = 5;
    if (type == GGL_LAN_JOIN_PKT) b[n++] = (uint8_t)NETPLAY_PROTO_REV;
    if (text) {
        size_t len = strlen(text);
        if (len > 199) len = 199;
        b[n++] = (uint8_t)len;
        memcpy(b + n, text, len);
        n += (uint32_t)len;
    }
    if (type == GGL_LAN_ACCEPT) b[n++] = (uint8_t)g_region;
    ggl_lan__send_to(conn, ip, port, b, n);
}

/* The peer's: to it, as the match's transport sends. */
static inline bool ggl_lan__to_peer(const void *b, uint32_t n) {
#ifndef __EMSCRIPTEN__
    return ggl_lan__send_to(g_lan.host ? g_lan.peer_conn : -1, g_lan.peer_ip, g_lan.peer_port, b, n);
#else
    return ggl_lan__send_to(-1, 0, 0, b, n);
#endif
}

static inline void ggl_lan__bye(void) {
    static const uint8_t b[5] = { 'M', '2', 'L', 'N', GGL_LAN_BYE };
    for (int i = 0; i < 3; i++) ggl_lan__to_peer(b, 5);
}

/* [len][text] at b + at, into out. */
static inline void ggl_lan__text(const uint8_t *b, int n, int at, char *out, size_t cap) {
    out[0] = '\0';
    if (n <= at || n < at + 1 + b[at]) return;
    size_t len = b[at] < cap ? b[at] : cap - 1;
    memcpy(out, b + at + 1, len);
    out[len] = '\0';
}

/* The match: GGPO told to start, through this file's transport. */
static bool ggl_lan__send_cb(void *ctx, uint32_t ip_be, uint16_t port, const void *data, uint32_t len);
static int  ggl_lan__recv_cb(void *ctx, void *out, uint32_t cap, uint32_t *ip_be, uint16_t *port);

static inline void ggl_lan__start(int conn, uint32_t ip, uint16_t port, const char *peer_text) {
    ggl_status_t *s = &g_ggl.st;
    g_lan.matched = true;
    g_lan.peer_ip = ip;
    g_lan.peer_port = port;
#ifndef __EMSCRIPTEN__
    g_lan.peer_conn = conn;
    if (g_lan.host) g_lan.ws = conn >= 0;
#else
    (void)conn;
#endif
    s->side = g_lan.host ? 0 : 1;
    snprintf(s->match, sizeof s->match, "lan");
    snprintf(s->user, sizeof s->user, "%s", g_lan.host ? "1P" : "2P");
    snprintf(s->opponent, sizeof s->opponent, "%s", peer_text);
    s->peer_known = s->direct = true;
    s->sent_direct = s->sent_relay = s->got_direct = s->got_relay = 0;
    s->error[0] = '\0';
    s->stage = GGL_MATCH;
    g_ggl.ggpo_seen = false;
    g_ggl.transport_on = true;
    g_ggl.ring_n = g_ggl.ring_head = 0;
    g_ggl.virt_ip = ggl__virtual_ip();
    ggpo_transport_t t = { NULL, ggl_lan__send_cb, ggl_lan__recv_cb };
    ggpo_port_set_transport(&t);
    g_ggpo_cfg.mode = EMU_GGPO_P2P;
    g_ggpo_cfg.player = g_lan.host ? 1 : 2;
    snprintf(g_ggpo_cfg.remote_ip, sizeof g_ggpo_cfg.remote_ip, "%s", GGL_VIRTUAL_IP);
    g_ggpo_cfg.remote_port = GGL_VIRTUAL_PORT;
    LOG_INFO("lan: match against %s, player %d", peer_text, g_ggpo_cfg.player);
}

/* Is (conn, ip, port) the peer? */
static inline bool ggl_lan__is_peer(int conn, uint32_t ip, uint16_t port) {
    if (!g_lan.matched) return false;
#ifndef __EMSCRIPTEN__
    if (g_lan.host && g_lan.ws) return conn == g_lan.peer_conn;
    if (!g_lan.host && g_lan.ws) return conn == -2;   /* our own WebSocket */
#endif
    (void)conn;
    return ip == g_lan.peer_ip && port == g_lan.peer_port;
}

/* Host: a JOIN, from a UDP address or a browser (conn). */
static inline void ggl_lan__on_join(int conn, uint32_t ip, uint16_t port, const uint8_t *b, int n) {
    char game[GGL_NAME_MAX], who[64], why[200];
    ggl_lan__text(b, n, 6, game, sizeof game);
    net_addr_text(who, sizeof who, ip, port);
    if (ggl_lan__is_peer(conn, ip, port)) { ggl_lan__ctl(conn, ip, port, GGL_LAN_ACCEPT, g_lan.game); return; }   /* our ACCEPT was lost */
    if (g_lan.matched || g_ggl.st.stage != GGL_LAN_HOST) {
        ggl_lan__ctl(conn, ip, port, GGL_LAN_REFUSE, "the host is already playing");
        return;
    }
    if (n < 6 || b[5] != (uint8_t)NETPLAY_PROTO_REV) {
        snprintf(why, sizeof why, "a different version of m2-hle (the host's netplay rev is %d, yours %d)",
                 NETPLAY_PROTO_REV, n >= 6 ? b[5] : -1);
        ggl_lan__ctl(conn, ip, port, GGL_LAN_REFUSE, why);
        LOG_WARN("lan: refused %s: netplay rev %d", who, n >= 6 ? b[5] : -1);
        return;
    }
    if (strcmp(game, g_lan.game) != 0) {
        snprintf(why, sizeof why, "the host plays %s, not %s", g_lan.game, game);
        ggl_lan__ctl(conn, ip, port, GGL_LAN_REFUSE, why);
        LOG_WARN("lan: refused %s: it plays %s", who, game);
        return;
    }
    ggl_lan__ctl(conn, ip, port, GGL_LAN_ACCEPT, g_lan.game);
    ggl_lan__start(conn, ip, port, who);
}

/* A control datagram from (conn, ip, port). */
static inline void ggl_lan__on_ctl(int conn, uint32_t ip, uint16_t port, const uint8_t *b, int n) {
    char text[200];
    switch (b[4]) {
    case GGL_LAN_JOIN_PKT:
        if (g_lan.host) ggl_lan__on_join(conn, ip, port, b, n);
        break;
    case GGL_LAN_ACCEPT:
        if (!g_lan.host && !g_lan.matched && g_ggl.st.stage == GGL_LAN_JOIN) {
            int at = 6 + (n > 5 ? b[5] : 0);   /* past [len][game] */
            if (n > at && b[at] <= GAME_REGION_EXPORT && b[at] != g_region) {
                g_lan.region_own = g_region;
                g_lan.region_taken = true;
                g_region = b[at];
                LOG_INFO("lan: playing in the host's region (%d, ours %d)", g_region, g_lan.region_own);
            }
            ggl_lan__start(-1, ip, port, g_lan.addr);
        }
        break;
    case GGL_LAN_REFUSE:
        if (!g_lan.host && !g_lan.matched) {
            ggl_lan__text(b, n, 5, text, sizeof text);
            snprintf(g_ggl.st.error, sizeof g_ggl.st.error, "the host said no: %s", text);
            LOG_WARN("lan: %s", g_ggl.st.error);
            g_ggl.st.stage = GGL_FAILED;
        }
        break;
    case GGL_LAN_BYE:
        if (g_lan.matched && g_ggl.st.stage == GGL_MATCH) ggl__match_done("the other player left");
        break;
    }
}

/* One datagram in: control, or (the peer's) GGPO's into the ring. */
static inline void ggl_lan__datagram(int conn, uint32_t ip, uint16_t port, const uint8_t *b, int n) {
    if (ggl_lan__is_ctl(b, n)) { ggl_lan__on_ctl(conn, ip, port, b, n); return; }
    if (!ggl_lan__is_peer(conn, ip, port)) return;
    g_ggl.st.got_direct++;
    ggl__ring_push(b, n);
}

/* ---- Reading ------------------------------------------------------------- */

#ifndef __EMSCRIPTEN__
static inline void ggl_lan__read_udp(void) {
    uint8_t buf[1600];
    for (int pass = 0; g_lan.udp_open && pass < 64; pass++) {
        uint32_t ip; uint16_t port;
        int n = net_udp_recv(g_lan.udp, buf, sizeof buf, &ip, &port);
        if (n <= 0) return;
        ggl_lan__datagram(-1, ip, port, buf, n);
    }
}

/* The host's door: new browsers, upgrades, frames. */
static inline void ggl_lan__read_conns(int64_t now) {
    if (g_lan.lsock_open) {
        net_sock_t s;
        uint32_t ip; uint16_t port;
        while (net_tcp_accept(g_lan.lsock, &s, &ip, &port)) {
            int i = 0;
            while (i < GGL_LAN_CONNS && g_lan.conn[i].open) i++;
            if (i == GGL_LAN_CONNS) { net_close(&s); continue; }
            ggl_lan_conn_t *c = &g_lan.conn[i];
            memset(c, 0, sizeof *c);
            c->open = true;
            c->sock = s;
            c->ip = ip;
            c->port = port;
            c->since = now;
        }
    }
    uint8_t buf[1600];
    for (int i = 0; i < GGL_LAN_CONNS; i++) {
        ggl_lan_conn_t *c = &g_lan.conn[i];
        if (!c->open) continue;
        bool alive = ggl_lan__conn_fill(i);
        if (alive && !c->upgraded) {
            int up = ggl_lan__conn_upgrade(i);
            if (up < 0 || (up == 0 && now - c->since > GGL_LAN_UPGRADE_MS)) alive = false;
        }
        for (int k = 0; alive && c->upgraded && k < 64; k++) {
            uint32_t before = c->in_used;
            int n = ggl_lan__conn_frame(i, buf, sizeof buf);
            if (n < 0) alive = false;
            else if (n > 0) ggl_lan__datagram(i, c->ip, c->port, buf, n);
            if (c->in_used == before) break;
        }
        if (alive) continue;
        bool was_peer = g_lan.matched && g_lan.ws && i == g_lan.peer_conn;
        ggl_lan__conn_close(i);
        if (was_peer) {
            g_lan.peer_conn = -1;
            if (g_ggl.st.stage == GGL_MATCH) ggl__match_done("the browser closed its connection");
        }
    }
}
#endif

/* The joiner's WebSocket: the host's datagrams. Text is not ours. */
static inline void ggl_lan__read_ws(void) {
    uint8_t msg[1600];
    for (int i = 0; i < 64 && ggl__ws_is_open(); i++) {
        bool binary = false;
        int n = ggl__ws_recv(msg, sizeof msg, &binary);
        if (n < 0) {
            const char *e = ggl__ws_error();
            snprintf(g_ggl.st.error, sizeof g_ggl.st.error, "%s", e[0] ? e : "the host closed the connection");
            ggl__ws_close();
            if (g_ggl.st.stage == GGL_MATCH) ggl__match_done("the host closed the connection");
            else if (g_ggl.st.stage == GGL_LAN_JOIN) g_ggl.st.stage = GGL_FAILED;
            return;
        }
        if (n == 0) return;
        if (binary) ggl_lan__datagram(-2, 0, 0, msg, n);
    }
}

static inline void ggl_lan__read(int64_t now) {
    (void)now;
    if (!g_lan.host && g_lan.ws) { ggl_lan__read_ws(); return; }
#ifndef __EMSCRIPTEN__
    ggl_lan__read_udp();
    if (g_lan.host) ggl_lan__read_conns(now);
#endif
}

/* ---- GGPO's transport ---------------------------------------------------- */

static bool ggl_lan__send_cb(void *ctx, uint32_t ip_be, uint16_t port, const void *data, uint32_t len) {
    (void)ctx; (void)ip_be; (void)port;
    if (!g_lan.matched) return false;
    g_ggl.st.sent_direct++;
    return ggl_lan__to_peer(data, len);
}

static int ggl_lan__recv_cb(void *ctx, void *out, uint32_t cap, uint32_t *ip_be, uint16_t *port) {
    (void)ctx;
    if (g_ggl.ring_n == 0) ggl_lan__read(net_now_ms());
    int got = ggl__ring_pop(out, cap);
    if (got <= 0) return 0;
    *ip_be = g_ggl.virt_ip;
    *port = GGL_VIRTUAL_PORT;
    return got;
}

/* ---- Commands and the pump ----------------------------------------------- */

/* Everything closed: no host, no joiner. */
static inline void ggl_lan__region_back(void) {
    if (!g_lan.region_taken) return;
    g_region = g_lan.region_own;
    g_lan.region_taken = false;
}

static inline void ggl_lan__close(void) {
    ggl_lan__region_back();
#ifndef __EMSCRIPTEN__
    if (g_lan.udp_open) net_close(&g_lan.udp);
    if (g_lan.lsock_open) net_close(&g_lan.lsock);
    for (int i = 0; i < GGL_LAN_CONNS; i++) ggl_lan__conn_close(i);
#endif
    if (g_lan.ws_open) ggl__ws_close();
    g_lan.udp_open = g_lan.ws_open = false;
#ifndef __EMSCRIPTEN__
    g_lan.lsock_open = false;
#endif
    g_lan.on = g_lan.matched = g_lan.ws = false;
}

/* The match is over (ggl__match_done): the host waits for the next player,
 * the joiner is done. GGPO lets go of the transport at its next stop. */
static inline void ggl_lan__match_over(void) {
    ggl_lan__region_back();
    g_ggl.st.match[0] = '\0';
    g_ggl.st.stage = g_lan.host ? GGL_LAN_HOST : GGL_OFF;
    if (g_lan.host) snprintf(g_ggl.st.user, sizeof g_ggl.st.user, "1P");
}

/* The addresses a joiner could use for this machine: the LAN's, and the
 * tailnet's when Tailscale is up (its DNS address routes through it). */
static inline void ggl_lan__host_text(uint16_t port) {
#ifndef __EMSCRIPTEN__
    static const uint8_t ts[4] = { 100, 100, 100, 100 };
    uint32_t ts_ip;
    memcpy(&ts_ip, ts, 4);
    uint32_t lan = net_local_ipv4_towards(0), tail = net_local_ipv4_towards(ts_ip);
    char a[32], b[32];
    net_addr_text(a, sizeof a, lan, port);
    net_addr_text(b, sizeof b, tail, port);
    if (lan && tail && tail != lan) snprintf(g_ggl.st.url, sizeof g_ggl.st.url, "%s or %s", a, b);
    else if (lan || tail) snprintf(g_ggl.st.url, sizeof g_ggl.st.url, "%s", lan ? a : b);
    else snprintf(g_ggl.st.url, sizeof g_ggl.st.url, "port %u", (unsigned)port);
#else
    (void)port;
#endif
}

static inline void ggl_lan__reset(const char *game) {
    ggl_lan__close();
    memset(&g_lan, 0, sizeof g_lan);
#ifndef __EMSCRIPTEN__
    g_lan.peer_conn = -1;
#endif
    snprintf(g_lan.game, sizeof g_lan.game, "%s", game);
    ggl_status_t *s = &g_ggl.st;
    s->error[0] = s->match[0] = s->opponent[0] = s->user[0] = s->game[0] = '\0';
    s->n_users = s->chat_n = 0;
    s->peer_known = s->direct = false;
    snprintf(s->game, sizeof s->game, "%s", game);
}

static inline void ggl_lan__fail(const char *why) {
    snprintf(g_ggl.st.error, sizeof g_ggl.st.error, "%s", why);
    LOG_WARN("lan: %s", why);
    ggl_lan__close();
    g_ggl.st.stage = GGL_FAILED;
}

/* GGL_CMD_LAN_HOST: port as text, "" for GGL_LAN_PORT. */
static inline void ggl_lan_host(const char *port_text, const char *game) {
    ggl_lan__reset(game);
    int port = port_text && port_text[0] ? atoi(port_text) : GGL_LAN_PORT;
#ifdef __EMSCRIPTEN__
    (void)port;
    ggl_lan__fail("a browser cannot host: join a desktop or libretro host instead");
#else
    char why[160];
    if (port <= 0 || port > 65535) { ggl_lan__fail("the port must be 1..65535"); return; }
    if (!net_startup() || !net_udp_open(&g_lan.udp, (uint16_t)port)) {
        snprintf(why, sizeof why, "UDP port %d is taken (another program, or another m2-hle)", port);
        ggl_lan__fail(why);
        return;
    }
    g_lan.udp_open = true;
    g_lan.lsock_open = net_tcp_listen(&g_lan.lsock, (uint16_t)port);
    if (!g_lan.lsock_open) LOG_WARN("lan: TCP port %d is taken: browsers cannot join this host", port);
    g_lan.on = g_lan.host = true;
    g_lan.port = (uint16_t)port;
    ggl_lan__host_text((uint16_t)port);
    snprintf(g_ggl.st.user, sizeof g_ggl.st.user, "1P");
    g_ggl.st.stage = GGL_LAN_HOST;
    LOG_INFO("lan: hosting %s on %s (UDP%s)", game, g_ggl.st.url, g_lan.lsock_open ? ", and TCP for browsers" : "");
#endif
}

/* A ws:// or wss:// URL (the browser's road; natively, to test it). */
static inline bool ggl_lan__is_url(const char *a) {
    return strncmp(a, "ws://", 5) == 0 || strncmp(a, "wss://", 6) == 0;
}

/* GGL_CMD_LAN_JOIN: "IP:PORT", "name:port", "IP" (GGL_LAN_PORT), or a URL. */
static inline void ggl_lan_join(const char *addr, const char *game) {
    ggl_lan__reset(game);
    while (*addr == ' ') addr++;
    snprintf(g_lan.addr, sizeof g_lan.addr, "%s", addr);
    snprintf(g_ggl.st.url, sizeof g_ggl.st.url, "%s", addr);
    if (!addr[0]) { ggl_lan__fail("join where? give the host's address, IP:PORT"); return; }
    g_lan.on = true;
    g_lan.join_until = net_now_ms() + GGL_LAN_JOIN_FOR;
#ifdef __EMSCRIPTEN__
    g_lan.ws = true;
#else
    g_lan.ws = ggl_lan__is_url(addr);
#endif
    if (g_lan.ws) {
        char url[sizeof g_lan.addr];
        if (ggl_lan__is_url(addr)) snprintf(url, sizeof url, "%s", addr);
        else snprintf(url, sizeof url, "ws://%.240s", addr);
        memcpy(g_lan.addr, url, sizeof url);
        g_lan.ws_open = true;
        if (!ggl__ws_open(url)) {
            const char *e = ggl__ws_error();
            char why[240];
            snprintf(why, sizeof why, "could not reach %.100s%s%.100s", url, e[0] ? ": " : "", e);
            ggl_lan__fail(why);
            return;
        }
    }
#ifndef __EMSCRIPTEN__
    else {
        char host[200];
        snprintf(host, sizeof host, "%s", addr);
        char *colon = strrchr(host, ':');
        int port = GGL_LAN_PORT;
        if (colon) { port = atoi(colon + 1); *colon = '\0'; }
        if (port <= 0 || port > 65535) { ggl_lan__fail("the port must be 1..65535"); return; }
        g_lan.peer_ip = net_startup() ? net_resolve_ipv4(host) : 0;
        g_lan.peer_port = (uint16_t)port;
        if (!g_lan.peer_ip) {
            char why[240];
            snprintf(why, sizeof why, "%s is not an address this machine can find", host);
            ggl_lan__fail(why);
            return;
        }
        if (!net_udp_open(&g_lan.udp, 0)) { ggl_lan__fail("could not open a UDP socket"); return; }
        g_lan.udp_open = true;
    }
#endif
    g_ggl.st.stage = GGL_LAN_JOIN;
    LOG_INFO("lan: joining %s", g_lan.addr);
}

/* GGL_CMD_END and GGL_CMD_DISCONNECT: a host's END plays on (waits for the
 * next joiner), DISCONNECT stops hosting. The match's transport stays GGPO's
 * until the session has stopped (ggl_lan_pump). */
static inline void ggl_lan_end(bool stop) {
    if (g_lan.matched && g_ggl.st.stage == GGL_MATCH) ggl_lan__bye();
    ggl__match_done(stop ? "disconnected" : "ended here");
    if (stop || !g_lan.host) {
        bool matched = g_lan.matched;
        ggl_lan__close();
        g_lan.matched = matched;
        g_ggl.st.stage = GGL_OFF;
    }
}

/* Emu thread, outside the emu mutex, from ggl_pump. */
static inline void ggl_lan_pump(int64_t now, bool ggpo_on) {
    ggl_status_t *s = &g_ggl.st;
    if (g_lan.on) ggl_lan__read(now);
    if (g_lan.on && s->stage == GGL_LAN_JOIN && !g_lan.matched) {
        if (now >= g_lan.join_until) {
            char why[300];
            snprintf(why, sizeof why, "no answer from %.200s: is it hosting, and does its firewall let %s in?",
                     g_lan.addr, g_lan.ws ? "TCP" : "UDP");
            ggl_lan__fail(why);
            return;
        }
        if (now >= g_lan.join_next && (!g_lan.ws || ggl__ws_is_open())) {
            ggl_lan__ctl(-1, g_lan.peer_ip, g_lan.peer_port, GGL_LAN_JOIN_PKT, g_lan.game);
            g_lan.join_next = now + GGL_LAN_JOIN_MS;
        }
    }
    if (g_lan.matched && s->stage == GGL_MATCH) {
        if (ggpo_on) g_ggl.ggpo_seen = true;
        else if (g_ggl.ggpo_seen || g_ggpo_cfg.mode == EMU_GGPO_OFF) {
            /* GGPO ended it (the peer went) or would not start. */
            ggl_lan__bye();
            ggl__match_done("the session ended");
        }
    }
    /* GGPO has let go of a match that is over: so does the peer. */
    if (g_lan.matched && s->stage != GGL_MATCH && !ggpo_on) {
        ggpo_port_set_transport(NULL);
        g_ggl.transport_on = false;
        g_ggl.ring_n = g_ggl.ring_head = 0;
        g_lan.matched = false;
#ifndef __EMSCRIPTEN__
        if (g_lan.peer_conn >= 0) ggl_lan__conn_close(g_lan.peer_conn);
        g_lan.peer_conn = -1;
#endif
        if (g_lan.on && g_lan.host) g_lan.ws = false;
        else ggl_lan__close();
    }
}

#endif
