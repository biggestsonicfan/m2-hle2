/*
 * ggpo_lobby.h — the GGPO lobby: sign in, find a player, play them (Pinboard #575).
 *
 * The lobby is ggpo-server (ggpo.sonicthefighte.rs, its own repository): JSON
 * text over a WebSocket for accounts, a channel per game and challenges, and
 * a UDP rendezvous for the match itself. When a challenge is accepted the
 * server hands both sides a `match` with a 128-bit key each and its UDP
 * address. From then on everything is UDP on one socket of ours:
 *
 *   to the server   "GGPO" | type | key16 | payload
 *                   HELLO (1) until it answers PEER, then every 10 s;
 *                   RELAY (4) a GGPO packet for the peer, while we have no
 *                   direct path
 *   from the server "GGPO" | SEEN (2) / PEER (3) | [len][ip ascii][port BE]
 *                   "GGPO" | DATA (5) | a GGPO packet the peer relayed
 *   peer to peer    "GGPO" | PUNCH (0x10) | heard, every 250 ms for 10 s;
 *                   anything else is GGPO's own
 *
 * GGPO itself is told its peer is GGL_VIRTUAL_IP:GGL_VIRTUAL_PORT and gets
 * this file's transport (ggpo_port_set_transport): every packet from the
 * peer, direct or relayed, is reported as coming from there, so a match can
 * start on the relay and move to the direct path without GGPO noticing. A
 * side goes direct once a punch says the peer hears it (`heard`), or a GGPO
 * packet arrives straight from the peer, who only sends so once it does.
 *
 * Threads. The pump (ggl_pump) runs on the emu thread outside the emu mutex,
 * like emu_netplay_pump: the WebSocket connect blocks. The transport runs on
 * the emu thread inside GGPO, under the mutex. Everyone else (the window, the
 * MCP bridge, the command line) posts commands (ggl_post) and reads a copy of
 * the status (ggl_status), both under g_ggl_lock, which nothing holds while
 * taking another lock.
 *
 * Header-only. Included from emu_ggpo.h (M2HLE_GGPO builds only).
 */
#ifndef M2HLE_GGPO_LOBBY_H
#define M2HLE_GGPO_LOBBY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "net_socket.h"
#include "ws_relay.h"
#include "../core/json_min.h"
#include "../core/thread_mutex.h"
#include "ggpo_port/ggpo_port.h"

#define GGL_DEFAULT_URL   "wss://ggpo.sonicthefighte.rs/ws"
#define GGL_USERS_MAX     32
#define GGL_CHAT_MAX      8
#define GGL_CMDS_MAX      16
#define GGL_NAME_MAX      33
#define GGL_VIRTUAL_IP    "169.254.0.1"   /* never on a wire: GGPO's name for the peer */
#define GGL_VIRTUAL_PORT  1
#define GGL_HELLO_MS      250
#define GGL_KEEPALIVE_MS  10000
#define GGL_PUNCH_MS      250
#define GGL_PUNCH_FOR_MS  10000
#define GGL_PING_MS       30000
#define GGL_RETRY_MS      3000            /* --ggpo-challenge: between tries */

enum { GGL_HELLO = 1, GGL_SEEN = 2, GGL_PEER = 3, GGL_RELAY = 4, GGL_DATA = 5, GGL_PUNCH = 0x10 };

typedef enum {
    GGL_OFF = 0,
    GGL_CONNECTING,     /* a connect is posted; the pump opens it next */
    GGL_CONNECTED,      /* welcomed, not signed in */
    GGL_SIGNED_IN,
    GGL_CHANNEL,        /* in a game's channel */
    GGL_MATCH,          /* playing */
    GGL_FAILED,         /* the connection is gone; error says why */
} ggl_stage_t;

typedef enum {
    GGL_CMD_CONNECT, GGL_CMD_LOGIN, GGL_CMD_SIGNUP, GGL_CMD_TWITCH, GGL_CMD_JOIN,
    GGL_CMD_CHALLENGE, GGL_CMD_ACCEPT, GGL_CMD_DECLINE, GGL_CMD_CANCEL, GGL_CMD_CHAT,
    GGL_CMD_END, GGL_CMD_DISCONNECT,
} ggl_cmd_kind_t;

typedef struct { char name[GGL_NAME_MAX]; char state[12]; } ggl_user_t;

/* What the window and the bridge see. */
typedef struct {
    ggl_stage_t stage;
    char        url[256];
    char        user[GGL_NAME_MAX];
    char        game[GGL_NAME_MAX];
    char        error[192];
    bool        twitch;                     /* the server offers Twitch sign-in */
    char        twitch_code[16];            /* a device flow is waiting on this code */
    char        twitch_uri[128];
    int         n_users;
    ggl_user_t  users[GGL_USERS_MAX];
    char        in_id[24], in_from[GGL_NAME_MAX];     /* a challenge to us */
    char        out_id[24], out_to[GGL_NAME_MAX];     /* ours, waiting */
    char        match[24], opponent[GGL_NAME_MAX];
    int         side;                       /* 0: we challenged (player 1) */
    bool        peer_known, direct;
    uint32_t    sent_direct, sent_relay, got_direct, got_relay;
    int         chat_n;                     /* lines in chat[], oldest first */
    char        chat[GGL_CHAT_MAX][160];
} ggl_status_t;

/* What the command line sets: the lobby then signs in, joins and challenges
 * or accepts by itself. */
typedef struct {
    char url[256];
    char user[GGL_NAME_MAX];
    char pass[128];
    bool signup;                    /* create the account first */
    char game[GGL_NAME_MAX];        /* "" = the ROM set's name */
    char challenge[GGL_NAME_MAX];   /* challenge this player when they are idle */
    char accept[GGL_NAME_MAX];      /* accept from this player, or "any" */
    bool force_relay;               /* never go direct (tests the relay) */
} ggl_auto_t;

typedef struct { ggl_cmd_kind_t kind; char a[256]; char b[128]; } ggl_cmd_t;

static ggl_auto_t g_ggl_auto;

static struct {
    /* Shared, under lock. */
    emu_mutex_t  lock;
    bool         lock_ready;
    ggl_cmd_t    cmds[GGL_CMDS_MAX];
    int          n_cmds;
    ggl_status_t pub;

    /* The emu thread's own. */
    ggl_status_t st;
    ws_relay_t   ws;
    char         pass[128];         /* what the last sign-in used (a token after Twitch) */
    char         flow[64];          /* a Twitch device flow */
    int64_t      twitch_next, twitch_every;
    int64_t      ping_next, retry_next;
    uint32_t     req;
    char         udp_host[128];
    uint16_t     udp_port;
    bool         autologin_done, autojoin_done;
    /* The match's UDP side. */
    net_sock_t   sock;
    bool         sock_open;
    uint8_t      key[16];
    uint32_t     srv_ip, peer_ip;
    uint16_t     srv_port, peer_port;
    bool         heard;             /* the peer reaches us directly */
    int64_t      hello_next, punch_next, punch_until;
    uint32_t     virt_ip;
    bool         ggpo_seen;         /* GGPO's session opened for this match */
} g_ggl;

/* ---- Shared side --------------------------------------------------------- */

static inline void ggl__lock_init(void) {
    /* The first post or read comes from the main thread before the emu thread
     * pumps, so there is no race on this. */
    if (!g_ggl.lock_ready) { emu_mutex_init(&g_ggl.lock); g_ggl.lock_ready = true; }
}

static inline void ggl_post(ggl_cmd_kind_t kind, const char *a, const char *b) {
    ggl__lock_init();
    emu_mutex_lock(&g_ggl.lock);
    if (g_ggl.n_cmds < GGL_CMDS_MAX) {
        ggl_cmd_t *c = &g_ggl.cmds[g_ggl.n_cmds++];
        c->kind = kind;
        snprintf(c->a, sizeof c->a, "%s", a ? a : "");
        snprintf(c->b, sizeof c->b, "%s", b ? b : "");
    }
    emu_mutex_unlock(&g_ggl.lock);
}

static inline ggl_status_t ggl_status(void) {
    ggl__lock_init();
    emu_mutex_lock(&g_ggl.lock);
    ggl_status_t s = g_ggl.pub;
    emu_mutex_unlock(&g_ggl.lock);
    return s;
}

static inline const char *ggl_stage_name(ggl_stage_t s) {
    static const char *n[] = { "off", "connecting", "connected", "signed_in", "channel", "match", "failed" };
    return (unsigned)s < sizeof n / sizeof n[0] ? n[s] : "?";
}

/* ---- The WebSocket ------------------------------------------------------- */

static inline void ggl__fail(const char *why) {
    snprintf(g_ggl.st.error, sizeof g_ggl.st.error, "%s", why);
    LOG_WARN("ggpo lobby: %s", why);
    ws_relay_close(&g_ggl.ws);
    g_ggl.st.stage = GGL_FAILED;
}

/* One JSON message; `body` is what goes after {"t":"...", without the brace. */
static inline void ggl__send(const char *t, const char *body) {
    if (!g_ggl.ws.open) return;
    char msg[1024];
    int n = snprintf(msg, sizeof msg, "{\"t\":\"%s\",\"id\":%u%s%s}", t, ++g_ggl.req,
                     body && body[0] ? "," : "", body ? body : "");
    if (n <= 0 || n >= (int)sizeof msg) return;
    if (!ws_relay_send_frame(&g_ggl.ws, 0x1, (const uint8_t *)msg, (uint32_t)n, NULL, 0))
        ggl__fail("the connection to the lobby was lost");
}

/* body = "key":"escaped value" */
static inline void ggl__send_str(const char *t, const char *key, const char *value) {
    char esc[512], body[600];
    json_escape(esc, sizeof esc, value);
    snprintf(body, sizeof body, "\"%s\":\"%s\"", key, esc);
    ggl__send(t, body);
}

static inline void ggl__send_login(const char *t, const char *user, const char *pass) {
    char u[96], p[300], body[420];
    json_escape(u, sizeof u, user);
    json_escape(p, sizeof p, pass);
    snprintf(body, sizeof body, "\"user\":\"%s\",\"pass\":\"%s\"", u, p);
    ggl__send(t, body);
}

/* The next text message into out (NUL-terminated): its length, 0 for none
 * waiting or a control frame, -1 when the connection is gone. */
static inline int ggl__ws_frame(char *out, uint32_t cap) {
    ws_relay_t *r = &g_ggl.ws;
    uint32_t h;
    uint64_t len;
    if (!ws_relay_frame_head(r, &h, &len)) return WS_RELAY_MORE;
    bool masked = (r->in[1] & 0x80) != 0;
    if (len > sizeof r->in - 14) { ws_relay_fail(r, "the lobby sent a message too large"); return -1; }
    if (r->in_used < h + (masked ? 4 : 0) + len) return WS_RELAY_MORE;
    uint8_t b0 = r->in[0];
    uint8_t *pl = r->in + h + (masked ? 4 : 0);
    if (masked)
        for (uint32_t i = 0; i < (uint32_t)len; i++) pl[i] ^= r->in[h + (i & 3)];
    uint32_t total = (uint32_t)(pl - r->in) + (uint32_t)len;
    int got = 0;
    if ((b0 & 0x0F) == 0x1 && (b0 & 0x80)) {
        got = len < cap ? (int)len : (int)cap - 1;
        memcpy(out, pl, (size_t)got);
        out[got] = '\0';
    } else if (ws_relay_control(r, b0, pl, len) < 0) {
        return -1;
    }
    memmove(r->in, r->in + total, r->in_used - total);
    r->in_used -= total;
    return got;
}

/* The next text message, reading the socket when the buffer has none. */
static inline int ggl__ws_recv(char *out, uint32_t cap) {
    ws_relay_t *r = &g_ggl.ws;
    if (!r->open) return -1;
    for (int pass = 0; pass < 64; pass++) {
        int got = ggl__ws_frame(out, cap);
        if (got == -1) return -1;
        if (got > 0) return got;
        if (got == 0) continue;
        int n = ws_relay_read(r, r->in + r->in_used, (uint32_t)sizeof r->in - r->in_used);
        if (n < 0) { ws_relay_fail(r, "the connection to the lobby was lost"); return -1; }
        if (n == 0) return 0;
        r->in_used += (uint32_t)n;
    }
    return 0;
}

/* ---- The match's UDP ------------------------------------------------------ */

static inline void ggl__udp_head(uint8_t *b, uint8_t type) {
    memcpy(b, "GGPO", 4);
    b[4] = type;
    memcpy(b + 5, g_ggl.key, 16);
}

static inline void ggl__hello(void) {
    uint8_t b[21];
    ggl__udp_head(b, GGL_HELLO);
    net_udp_send(g_ggl.sock, g_ggl.srv_ip, g_ggl.srv_port, b, sizeof b);
}

static inline void ggl__punch(void) {
    uint8_t b[6] = { 'G', 'G', 'P', 'O', GGL_PUNCH, (uint8_t)g_ggl.heard };
    net_udp_send(g_ggl.sock, g_ggl.peer_ip, g_ggl.peer_port, b, sizeof b);
}

static inline bool ggl__send_cb(void *ctx, uint32_t ip_be, uint16_t port, const void *data, uint32_t len) {
    (void)ctx; (void)ip_be; (void)port;
    if (g_ggl.st.direct) {
        g_ggl.st.sent_direct++;
        return net_udp_send(g_ggl.sock, g_ggl.peer_ip, g_ggl.peer_port, data, len);
    }
    uint8_t b[21 + 1400];
    if (len > 1400) return false;
    ggl__udp_head(b, GGL_RELAY);
    memcpy(b + 21, data, len);
    g_ggl.st.sent_relay++;
    return net_udp_send(g_ggl.sock, g_ggl.srv_ip, g_ggl.srv_port, b, 21 + len);
}

/* SEEN / PEER: [len][ip ascii][port BE] after the type byte. */
static inline bool ggl__addr(const uint8_t *b, int n, uint32_t *ip, uint16_t *port) {
    if (n < 8 || b[5] == 0 || b[5] > 45 || n < 6 + b[5] + 2) return false;
    char s[48];
    memcpy(s, b + 6, b[5]);
    s[b[5]] = '\0';
    *ip = net_resolve_ipv4(s);       /* a dotted quad: no lookup */
    *port = (uint16_t)(b[6 + b[5]] << 8 | b[7 + b[5]]);
    return *ip != 0;
}

/* A datagram from the server: a GGPO packet's length once moved to the front
 * of buf, else 0. */
static inline int ggl__from_server(uint8_t *buf, int n) {
    if (n < 5 || memcmp(buf, "GGPO", 4) != 0) return 0;
    if (buf[4] == GGL_DATA) {
        memmove(buf, buf + 5, (size_t)n - 5);
        g_ggl.st.got_relay++;
        return n - 5;
    }
    uint32_t ip; uint16_t port;
    if (buf[4] == GGL_PEER && !g_ggl.st.peer_known && ggl__addr(buf, n, &ip, &port)) {
        g_ggl.peer_ip = ip;
        g_ggl.peer_port = port;
        g_ggl.st.peer_known = true;
        g_ggl.punch_until = net_now_ms() + GGL_PUNCH_FOR_MS;
        LOG_INFO("ggpo lobby: the peer is at %.*s:%u", buf[5], (const char *)buf + 6, (unsigned)port);
    }
    return 0;
}

/* Something came straight from the peer. */
static inline int ggl__from_peer(const uint8_t *buf, int n) {
    if (n >= 6 && memcmp(buf, "GGPO", 4) == 0 && buf[4] == GGL_PUNCH) {
        g_ggl.heard = true;
        if (buf[5] && !g_ggl.st.direct && !g_ggl_auto.force_relay) {
            g_ggl.st.direct = true;
            LOG_INFO("ggpo lobby: direct to the peer");
        }
        return 0;
    }
    g_ggl.heard = true;
    if (!g_ggl.st.direct && !g_ggl_auto.force_relay) {
        g_ggl.st.direct = true;
        LOG_INFO("ggpo lobby: direct to the peer");
    }
    g_ggl.st.got_direct++;
    return n;
}

static inline int ggl__recv_cb(void *ctx, void *out, uint32_t cap, uint32_t *ip_be, uint16_t *port) {
    (void)ctx;
    uint8_t buf[1600];
    for (int pass = 0; pass < 64; pass++) {
        uint32_t ip; uint16_t p;
        int n = net_udp_recv(g_ggl.sock, buf, sizeof buf, &ip, &p);
        if (n <= 0) return 0;
        int got = 0;
        if (ip == g_ggl.srv_ip && p == g_ggl.srv_port) got = ggl__from_server(buf, n);
        else if (g_ggl.st.peer_known && ip == g_ggl.peer_ip && p == g_ggl.peer_port) got = ggl__from_peer(buf, n);
        if (got <= 0 || (uint32_t)got > cap) continue;
        memcpy(out, buf, (size_t)got);
        *ip_be = g_ggl.virt_ip;
        *port = GGL_VIRTUAL_PORT;
        return got;
    }
    return 0;
}

/* Hellos until the server pairs us, then keepalives; punches for a while. */
static inline void ggl__udp_tick(int64_t now) {
    if (now >= g_ggl.hello_next) {
        ggl__hello();
        g_ggl.hello_next = now + (g_ggl.st.peer_known ? GGL_KEEPALIVE_MS : GGL_HELLO_MS);
    }
    if (g_ggl.st.peer_known && now < g_ggl.punch_until && now >= g_ggl.punch_next && !g_ggl_auto.force_relay) {
        ggl__punch();
        g_ggl.punch_next = now + GGL_PUNCH_MS;
    }
}

static inline void ggl__udp_close(void) {
    if (g_ggl.sock_open) net_close(&g_ggl.sock);
    g_ggl.sock_open = false;
    ggpo_port_set_transport(NULL);
}

/* ---- Messages from the lobby --------------------------------------------- */

static inline void ggl__chat_line(const char *line) {
    ggl_status_t *s = &g_ggl.st;
    if (s->chat_n == GGL_CHAT_MAX) {
        memmove(s->chat[0], s->chat[1], sizeof s->chat[0] * (GGL_CHAT_MAX - 1));
        s->chat_n--;
    }
    snprintf(s->chat[s->chat_n++], sizeof s->chat[0], "%s", line);
}

static inline int ggl__find_user(const char *name) {
    for (int i = 0; i < g_ggl.st.n_users; i++)
        if (strcmp(g_ggl.st.users[i].name, name) == 0) return i;
    return -1;
}

/* One user object's fields, at p (just past its '{'). */
static inline void ggl__set_user(const char *obj, const char *state_default) {
    char name[GGL_NAME_MAX], state[12];
    if (!json_get_str(obj, "name", name, sizeof name)) return;
    if (!json_get_str(obj, "state", state, sizeof state)) snprintf(state, sizeof state, "%s", state_default);
    int i = ggl__find_user(name);
    if (i < 0) {
        if (g_ggl.st.n_users >= GGL_USERS_MAX) return;
        i = g_ggl.st.n_users++;
    }
    snprintf(g_ggl.st.users[i].name, GGL_NAME_MAX, "%s", name);
    snprintf(g_ggl.st.users[i].state, sizeof g_ggl.st.users[i].state, "%s", state);
}

static inline void ggl__on_channel(const char *m) {
    json_get_str(m, "game", g_ggl.st.game, sizeof g_ggl.st.game);
    g_ggl.st.n_users = 0;
    const char *p = json_value_at(m, "users");
    while (p && (p = strchr(p, '{')) != NULL) {
        ggl__set_user(p, "idle");
        p = strchr(p, '}');
    }
    g_ggl.st.stage = GGL_CHANNEL;
    LOG_INFO("ggpo lobby: in channel %s with %d player(s)", g_ggl.st.game, g_ggl.st.n_users);
}

static inline void ggl__on_left(const char *m) {
    char name[GGL_NAME_MAX];
    if (!json_get_str(m, "name", name, sizeof name)) return;
    int i = ggl__find_user(name);
    if (i < 0) return;
    memmove(&g_ggl.st.users[i], &g_ggl.st.users[i + 1], sizeof(ggl_user_t) * (size_t)(g_ggl.st.n_users - i - 1));
    g_ggl.st.n_users--;
}

static inline void ggl__on_state(const char *m) {
    char name[GGL_NAME_MAX], state[12];
    if (!json_get_str(m, "name", name, sizeof name) || !json_get_str(m, "state", state, sizeof state)) return;
    int i = ggl__find_user(name);
    if (i >= 0) snprintf(g_ggl.st.users[i].state, sizeof g_ggl.st.users[i].state, "%s", state);
}

static inline void ggl__on_signed_in(const char *m) {
    const char *user = json_value_at(m, "user");
    if (user) json_get_str(user, "name", g_ggl.st.user, sizeof g_ggl.st.user);
    char token[128];
    if (json_get_str(m, "token", token, sizeof token)) snprintf(g_ggl.pass, sizeof g_ggl.pass, "%s", token);
    g_ggl.st.twitch_code[0] = g_ggl.flow[0] = '\0';
    g_ggl.st.stage = GGL_SIGNED_IN;
    LOG_INFO("ggpo lobby: signed in as %s", g_ggl.st.user);
}

static inline void ggl__on_challenged(const char *m) {
    const char *from = json_value_at(m, "from");
    json_get_str(m, "id", g_ggl.st.in_id, sizeof g_ggl.st.in_id);
    if (from) json_get_str(from, "name", g_ggl.st.in_from, sizeof g_ggl.st.in_from);
    LOG_INFO("ggpo lobby: %s challenges us", g_ggl.st.in_from);
    const char *a = g_ggl_auto.accept;
    if (a[0] && (strcmp(a, "any") == 0 || strcmp(a, g_ggl.st.in_from) == 0))
        ggl__send_str("accept", "challenge", g_ggl.st.in_id);
}

static inline void ggl__on_challenge_over(const char *m) {
    char id[24], why[24] = "";
    if (!json_get_str(m, "id", id, sizeof id)) return;
    json_get_str(m, "why", why, sizeof why);
    if (strcmp(id, g_ggl.st.in_id) == 0) g_ggl.st.in_id[0] = g_ggl.st.in_from[0] = '\0';
    if (strcmp(id, g_ggl.st.out_id) == 0) {
        LOG_INFO("ggpo lobby: our challenge to %s: %s", g_ggl.st.out_to, why);
        g_ggl.st.out_id[0] = g_ggl.st.out_to[0] = '\0';
        g_ggl.retry_next = net_now_ms() + GGL_RETRY_MS;
    }
}

static inline void ggl__on_twitch_code(const char *m) {
    json_get_str(m, "flow", g_ggl.flow, sizeof g_ggl.flow);
    json_get_str(m, "user_code", g_ggl.st.twitch_code, sizeof g_ggl.st.twitch_code);
    json_get_str_unescaped(m, "verification_uri", g_ggl.st.twitch_uri, sizeof g_ggl.st.twitch_uri);
    int every = 5;
    json_get_int(m, "interval", &every);
    g_ggl.twitch_every = (every > 0 ? every : 5) * 1000;
    g_ggl.twitch_next = net_now_ms() + g_ggl.twitch_every;
    LOG_INFO("ggpo lobby: enter %s at %s", g_ggl.st.twitch_code, g_ggl.st.twitch_uri);
}

static inline void ggl__on_error(const char *m) {
    char what[32] = "", err[64] = "";
    json_get_str(m, "for", what, sizeof what);
    json_get_str(m, "error", err, sizeof err);
    if (strcmp(what, "twitch_poll") == 0) {
        if (strcmp(err, "pending") == 0) return;
        if (strcmp(err, "slow_down") == 0) { g_ggl.twitch_every += 5000; return; }
        g_ggl.flow[0] = g_ggl.st.twitch_code[0] = '\0';
    }
    snprintf(g_ggl.st.error, sizeof g_ggl.st.error, "%s: %s", what, err);
    LOG_WARN("ggpo lobby: %s failed: %s", what, err);
}

static inline void ggl__on_welcome(const char *m) {
    const char *udp = json_value_at(m, "udp");
    uint32_t port = 0;
    if (udp) {
        json_get_str(udp, "host", g_ggl.udp_host, sizeof g_ggl.udp_host);
        json_get_u32(udp, "port", &port);
    }
    g_ggl.udp_port = (uint16_t)port;
    const char *tw = json_value_at(m, "twitch");
    g_ggl.st.twitch = tw && strncmp(tw, "true", 4) == 0;
    g_ggl.st.stage = GGL_CONNECTED;
}

/* The match: a UDP socket, the server's address, and GGPO told to start. */
static inline bool ggl__open_match(const char *host, uint32_t port) {
    g_ggl.srv_ip = net_resolve_ipv4(host);
    g_ggl.srv_port = (uint16_t)port;
    if (!g_ggl.srv_ip || !port) { ggl__fail("the lobby's UDP address does not resolve"); return false; }
    if (!net_udp_open(&g_ggl.sock, 0)) { ggl__fail("could not open a UDP socket for the match"); return false; }
    g_ggl.sock_open = true;
    g_ggl.virt_ip = net_resolve_ipv4(GGL_VIRTUAL_IP);
    ggpo_transport_t t = { NULL, ggl__send_cb, ggl__recv_cb };
    ggpo_port_set_transport(&t);
    g_ggpo_cfg.mode = EMU_GGPO_P2P;
    g_ggpo_cfg.player = g_ggl.st.side == 0 ? 1 : 2;
    snprintf(g_ggpo_cfg.remote_ip, sizeof g_ggpo_cfg.remote_ip, "%s", GGL_VIRTUAL_IP);
    g_ggpo_cfg.remote_port = GGL_VIRTUAL_PORT;
    return true;
}

static inline void ggl__hex16(const char *hex, uint8_t *out) {
    for (int i = 0; i < 16; i++) {
        unsigned v = 0;
        if (!json__hex_nibble(hex[2 * i], &v)) v = 0;
        v <<= 4;
        if (!json__hex_nibble(hex[2 * i + 1], &v)) v &= ~0xFu;
        out[i] = (uint8_t)v;
    }
}

static inline void ggl__on_match(const char *m) {
    ggl_status_t *s = &g_ggl.st;
    char key[40] = "", host[128] = "";
    uint32_t port = 0;
    int side = 0;
    json_get_str(m, "match", s->match, sizeof s->match);
    json_get_int(m, "side", &side);
    json_get_str(m, "key", key, sizeof key);
    const char *opp = json_value_at(m, "opponent");
    if (opp) json_get_str(opp, "name", s->opponent, sizeof s->opponent);
    const char *udp = json_value_at(m, "udp");
    if (udp) { json_get_str(udp, "host", host, sizeof host); json_get_u32(udp, "port", &port); }
    if (strlen(key) != 32) { ggl__fail("the match came without a key"); return; }
    ggl__hex16(key, g_ggl.key);
    s->side = side;
    s->peer_known = s->direct = g_ggl.heard = g_ggl.ggpo_seen = false;
    s->sent_direct = s->sent_relay = s->got_direct = s->got_relay = 0;
    s->in_id[0] = s->in_from[0] = s->out_id[0] = s->out_to[0] = '\0';
    g_ggl.hello_next = g_ggl.punch_next = 0;
    if (!ggl__open_match(host, port)) return;
    s->stage = GGL_MATCH;
    LOG_INFO("ggpo lobby: match %s against %s, player %d", s->match, s->opponent, side == 0 ? 1 : 2);
}

/* The match is over, from either end: GGPO stops, the socket goes. */
static inline void ggl__match_done(const char *why) {
    if (g_ggl.st.stage != GGL_MATCH) return;
    g_ggpo_cfg.mode = EMU_GGPO_OFF;          /* emu_ggpo_step stops the session */
    LOG_INFO("ggpo lobby: match %s over (%s)", g_ggl.st.match, why);
    g_ggl.st.match[0] = '\0';
    g_ggl.st.stage = g_ggl.st.game[0] ? GGL_CHANNEL : GGL_SIGNED_IN;
}

static inline void ggl__on_chat(const char *m) {
    char from[GGL_NAME_MAX] = "", text[140] = "", line[180];
    json_get_str(m, "from", from, sizeof from);
    json_get_str_unescaped(m, "text", text, sizeof text);
    snprintf(line, sizeof line, "%s: %s", from, text);
    ggl__chat_line(line);
}

static inline void ggl__on_joined(const char *m) {
    const char *u = json_value_at(m, "user");
    if (u) ggl__set_user(u, "idle");
}
static inline void ggl__on_challenge_sent(const char *m) {
    json_get_str(m, "id", g_ggl.st.out_id, sizeof g_ggl.st.out_id);
}
static inline void ggl__on_match_over(const char *m) { (void)m; ggl__match_done("the server ended it"); }
static inline void ggl__on_kicked(const char *m)     { (void)m; ggl__fail("signed in somewhere else"); }

static inline void ggl__on_message(const char *m) {
    static const struct { const char *t; void (*on)(const char *); } h[] = {
        { "welcome", ggl__on_welcome },         { "signed_in", ggl__on_signed_in },
        { "twitch_code", ggl__on_twitch_code }, { "channel", ggl__on_channel },
        { "joined", ggl__on_joined },           { "left", ggl__on_left },
        { "state", ggl__on_state },             { "chat", ggl__on_chat },
        { "challenge_sent", ggl__on_challenge_sent }, { "challenged", ggl__on_challenged },
        { "challenge_over", ggl__on_challenge_over }, { "match", ggl__on_match },
        { "match_over", ggl__on_match_over },   { "kicked", ggl__on_kicked },
        { "error", ggl__on_error },
    };
    char t[24];
    if (!json_get_str(m, "t", t, sizeof t)) return;
    for (size_t i = 0; i < sizeof h / sizeof h[0]; i++)
        if (strcmp(t, h[i].t) == 0) { h[i].on(m); return; }
}

/* ---- Commands ------------------------------------------------------------ */

static inline void ggl__connect(const char *url) {
    ws_relay_close(&g_ggl.ws);
    bool dflt = !url[0] || strcmp(url, "default") == 0;
    snprintf(g_ggl.st.url, sizeof g_ggl.st.url, "%s", dflt ? GGL_DEFAULT_URL : url);
    g_ggl.st.error[0] = '\0';
    g_ggl.st.stage = GGL_CONNECTING;
    g_ggl.ping_next = net_now_ms() + GGL_PING_MS;
    /* Blocks for the connect and the upgrade: the pump holds no lock here. */
    if (!net_startup() || !ws_relay_open(&g_ggl.ws, g_ggl.st.url)) {
        ggl__fail(g_ggl.ws.error[0] ? g_ggl.ws.error : "could not reach the lobby");
        return;
    }
    LOG_INFO("ggpo lobby: connected to %s", g_ggl.st.url);
}

static inline void ggl__challenge(const char *name) {
    snprintf(g_ggl.st.out_to, sizeof g_ggl.st.out_to, "%s", name);
    ggl__send_str("challenge", "to", name);
}

static inline void ggl__run_cmd(const ggl_cmd_t *c) {
    switch (c->kind) {
    case GGL_CMD_CONNECT:    ggl__connect(c->a); break;
    case GGL_CMD_LOGIN:
    case GGL_CMD_SIGNUP:
        snprintf(g_ggl.pass, sizeof g_ggl.pass, "%s", c->b);
        ggl__send_login(c->kind == GGL_CMD_SIGNUP ? "signup" : "login", c->a, c->b);
        break;
    case GGL_CMD_TWITCH:     ggl__send("twitch_start", ""); break;
    case GGL_CMD_JOIN:       ggl__send_str("join", "game", c->a); break;
    case GGL_CMD_CHALLENGE:  ggl__challenge(c->a); break;
    case GGL_CMD_ACCEPT:     ggl__send_str("accept", "challenge", g_ggl.st.in_id); break;
    case GGL_CMD_DECLINE:    ggl__send_str("decline", "challenge", g_ggl.st.in_id); break;
    case GGL_CMD_CANCEL:     ggl__send_str("cancel", "challenge", g_ggl.st.out_id); break;
    case GGL_CMD_CHAT:       ggl__send_str("chat", "text", c->a); break;
    case GGL_CMD_END:        ggl__match_done("ended here"); break;
    case GGL_CMD_DISCONNECT:
        ggl__match_done("disconnected");
        ws_relay_close(&g_ggl.ws);
        g_ggl.st.stage = GGL_OFF;
        break;
    }
}

/* ---- The pump ------------------------------------------------------------ */

/* What the command line asked for, a step at a time as the lobby gets there:
 * sign in, join, then challenge (accepting is ggl__on_challenged's). */
static inline void ggl__auto_signin(const ggl_auto_t *a, const char *rom_set) {
    const ggl_status_t *s = &g_ggl.st;
    if (s->stage == GGL_CONNECTED && a->user[0] && !g_ggl.autologin_done) {
        g_ggl.autologin_done = true;
        snprintf(g_ggl.pass, sizeof g_ggl.pass, "%s", a->pass);
        ggl__send_login(a->signup ? "signup" : "login", a->user, a->pass);
    }
    if (s->stage == GGL_SIGNED_IN && !g_ggl.autojoin_done && (a->user[0] || a->game[0])) {
        g_ggl.autojoin_done = true;
        ggl__send_str("join", "game", a->game[0] ? a->game : rom_set);
    }
}

static inline void ggl__auto(int64_t now, const char *rom_set) {
    const ggl_auto_t *a = &g_ggl_auto;
    const ggl_status_t *s = &g_ggl.st;
    ggl__auto_signin(a, rom_set);
    if (s->stage != GGL_CHANNEL || !a->challenge[0] || s->out_to[0] || now < g_ggl.retry_next) return;
    int i = ggl__find_user(a->challenge);
    if (i >= 0 && strcmp(s->users[i].state, "idle") == 0) {
        ggl__challenge(a->challenge);
        g_ggl.retry_next = now + GGL_RETRY_MS;
    }
}

/* The match's bookkeeping: GGPO's session opening and ending. */
static inline void ggl__match_tick(int64_t now, bool ggpo_on) {
    if (g_ggl.st.stage == GGL_MATCH) {
        ggl__udp_tick(now);
        if (ggpo_on) g_ggl.ggpo_seen = true;
        else if (g_ggl.ggpo_seen || g_ggpo_cfg.mode == EMU_GGPO_OFF) {
            /* GGPO ended it (the peer went) or would not start. */
            ggl__send_str("match_end", "match", g_ggl.st.match);
            ggl__match_done("the session ended");
        }
    }
    if (g_ggl.st.stage != GGL_MATCH && g_ggl.sock_open && !ggpo_on) ggl__udp_close();
}

static inline void ggl__read_ws(void) {
    static char msg[16384];
    for (int i = 0; i < 32 && g_ggl.ws.open; i++) {
        int n = ggl__ws_recv(msg, sizeof msg);
        if (n < 0) { ggl__fail(g_ggl.ws.error[0] ? g_ggl.ws.error : "the lobby closed the connection"); return; }
        if (n == 0) return;
        ggl__on_message(msg);
    }
}

static inline void ggl__timers(int64_t now) {
    if (g_ggl.flow[0] && now >= g_ggl.twitch_next) {
        ggl__send_str("twitch_poll", "flow", g_ggl.flow);
        g_ggl.twitch_next = now + g_ggl.twitch_every;
    }
    if (g_ggl.ws.open && now >= g_ggl.ping_next) {
        ggl__send("ping", "");
        g_ggl.ping_next = now + GGL_PING_MS;
    }
}

static inline int ggl__take_cmds(ggl_cmd_t *out) {
    emu_mutex_lock(&g_ggl.lock);
    int n = g_ggl.n_cmds;
    memcpy(out, g_ggl.cmds, sizeof(ggl_cmd_t) * (size_t)n);
    g_ggl.n_cmds = 0;
    emu_mutex_unlock(&g_ggl.lock);
    return n;
}

/* Emu thread, outside the emu mutex. ggpo_on: a GGPO session is open. */
static inline void ggl_pump(bool ggpo_on, const char *rom_set) {
    ggl__lock_init();
    ggl_cmd_t cmds[GGL_CMDS_MAX];
    int n = ggl__take_cmds(cmds);
    if (n == 0 && g_ggl.st.stage == GGL_OFF && !g_ggl.sock_open) return;
    for (int i = 0; i < n; i++) ggl__run_cmd(&cmds[i]);
    int64_t now = net_now_ms();
    ggl__read_ws();
    ggl__timers(now);
    ggl__auto(now, rom_set);
    ggl__match_tick(now, ggpo_on);
    emu_mutex_lock(&g_ggl.lock);
    g_ggl.pub = g_ggl.st;
    emu_mutex_unlock(&g_ggl.lock);
}

/* The command line's --ggpo-lobby* flags; true when argv[*i] was one. */
static inline bool ggl_cli_arg(int argc, char **argv, int *i) {
    const char *a = argv[*i];
    if (strcmp(a, "--ggpo-relay") == 0) { g_ggl_auto.force_relay = true; return true; }
    if (strcmp(a, "--ggpo-signup") == 0) { g_ggl_auto.signup = true; return true; }
    if (strncmp(a, "--ggpo-", 7) != 0 || *i + 1 >= argc) return false;
    static const struct { const char *flag; size_t off, cap; } f[] = {
        { "lobby",     offsetof(ggl_auto_t, url),       sizeof g_ggl_auto.url },
        { "user",      offsetof(ggl_auto_t, user),      sizeof g_ggl_auto.user },
        { "pass",      offsetof(ggl_auto_t, pass),      sizeof g_ggl_auto.pass },
        { "game",      offsetof(ggl_auto_t, game),      sizeof g_ggl_auto.game },
        { "challenge", offsetof(ggl_auto_t, challenge), sizeof g_ggl_auto.challenge },
        { "accept",    offsetof(ggl_auto_t, accept),    sizeof g_ggl_auto.accept },
    };
    for (size_t k = 0; k < sizeof f / sizeof f[0]; k++) {
        if (strcmp(a + 7, f[k].flag) != 0) continue;
        snprintf((char *)&g_ggl_auto + f[k].off, f[k].cap, "%s", argv[++*i]);
        if (k == 0) ggl_post(GGL_CMD_CONNECT, g_ggl_auto.url, NULL);
        return true;
    }
    return false;
}

#endif
