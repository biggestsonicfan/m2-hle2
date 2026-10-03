/*
 * ws_relay.h — a desktop client's datagrams through the web gateway (Pinboard #366).
 *
 * A player whose UDP nobody outside can reach -- the fly streaming from a Docker
 * container is the case that found it -- never links with a player outside the
 * house. RPCN tells the peer to punch the address and port it saw our keepalive
 * come from, and Docker Desktop's userspace proxy gave that keepalive a port of
 * its own, which forwards nothing back. Only the published UDP 3658 reaches the
 * container, and RPCN never names it. Punching cannot fix that: the mapping the
 * peer is told about is one only RPCN may answer through.
 *
 * The browser build has the same problem with no UDP at all, and the gateway on
 * the RPCN host solves it (web/gateway/gateway.mjs, WEB-NETPLAY.md section 4):
 * /gw/dgram is a WebSocket whose messages are datagrams, framed
 * [ip: 4, network order][port: u16 BE][payload], sent and received from a UDP
 * socket on the gateway's public address. That address is reachable by anybody,
 * so a client behind it is too. This file is the native end of that socket, so
 * a desktop client can take the same road: the WebSocket is an ordinary outbound
 * TCP connection, which any NAT passes.
 *
 * What the gateway does with it is unchanged from a browser's:
 *   - a keepalive sent to WS_RELAY_SIGNALING_TAG:3657 goes to RPCN's helper
 *     with the session's virtual address (100.64.x.x) written into it, and the
 *     helper's answer comes back from the tag;
 *   - a datagram to a virtual address goes to that session without leaving the
 *     gateway (two players behind the gateway are on one public address, so RPCN
 *     hands each the other's virtual address);
 *   - anything else leaves from the gateway's UDP port.
 * rpcn_client.h maps the tag to and from the real helper's address, so nothing
 * above it can tell a relayed client from a direct one.
 *
 * The gateway admits a WebSocket only from an Origin in its list (it exists to
 * stop other web pages' scripts). A native client has no page; it sends the play
 * site's, WS_RELAY_ORIGIN. It also pings each socket every 30 s and closes one
 * that does not answer, so ws_relay_recv answers pings, and has to be called.
 *
 * wss:// goes through tls.h with ordinary chain and name validation (the gateway
 * sits behind Caddy's Let's Encrypt certificate). ws:// is plain TCP, for a
 * gateway run locally to test against (`node gateway.mjs`, listening on 8787).
 *
 * Only what the gateway sends is understood: unfragmented binary messages,
 * pings and close. A fragmented or text message closes the relay.
 */
#ifndef WS_RELAY_H
#define WS_RELAY_H

/* The web build's datagrams always take this road (web_socket.h). */
#ifndef __EMSCRIPTEN__

#include "net_socket.h"
#include "tls.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  define ws_relay__stricmp  _stricmp
#  define ws_relay__strnicmp _strnicmp
#else
#  include <strings.h>
#  define ws_relay__stricmp  strcasecmp
#  define ws_relay__strnicmp strncasecmp
#endif

#ifndef _WIN32
#  include <netinet/tcp.h>
#endif

/* Must match SIGNALING_TAG in web/gateway/rules.mjs (and web_socket.h). */
#define WS_RELAY_SIGNALING_TAG_BYTES { 100, 127, 255, 254 }
#define WS_RELAY_ORIGIN    "https://play.sonicthefighte.rs"
#define WS_RELAY_DGRAM_MAX 1200          /* the gateway's limits.maxPayload */
#define WS_RELAY_TIMEOUT_MS 8000

/* The public gateway, and the RPCN servers it is an upstream for. */
#define WS_RELAY_GATEWAY   "wss://rpcn.sonicthefighte.rs/gw"
#define WS_RELAY_OURS      "rpcn.sonicthefighte.rs"
#define WS_RELAY_OFFICIAL  "np.rpcs3.net"

typedef struct {
    bool         open;
    bool         secure;          /* wss:// through tls.h, else a plain socket */
    tls_client_t tls;
    net_sock_t   sock;
    uint8_t      in[16384];
    uint32_t     in_used;
    uint32_t     seed;            /* the frame masks; they need not be secret */
    uint32_t     sent, received;
    char         url[256];
    char         error[192];
} ws_relay_t;

/* The /gw/dgram URL for `server` on the public gateway, or false when the gateway
 * relays nothing for that server (a test server on this machine, a LAN one). */
static inline bool ws_relay_url_for(const char *server, char *out, size_t cap) {
    if (!server) return false;
    if (ws_relay__stricmp(server, WS_RELAY_OURS) == 0) {
        snprintf(out, cap, "%s/dgram", WS_RELAY_GATEWAY);
        return true;
    }
    if (ws_relay__stricmp(server, WS_RELAY_OFFICIAL) == 0) {
        snprintf(out, cap, "%s/dgram/%s", WS_RELAY_GATEWAY, WS_RELAY_OFFICIAL);
        return true;
    }
    return false;
}

static inline void ws_relay_close(ws_relay_t *r) {
    if (!r->url[0]) return;   /* never opened: its sockets were never set to invalid */
    if (r->secure) tls_close(&r->tls);
    else           net_close(&r->sock);
    r->open    = false;
    r->in_used = 0;
}

static inline uint32_t ws_relay_rand(ws_relay_t *r) {
    uint32_t x = r->seed ? r->seed : (uint32_t)net_now_us() * 2654435761u + 1u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return r->seed = x;
}

static inline bool ws_relay_write(ws_relay_t *r, const void *data, uint32_t len) {
    return r->secure ? tls_send_all(&r->tls, data, len) : net_tcp_send_all(r->sock, data, len);
}

/* Bytes now, without waiting: >0 read, 0 nothing yet, -1 closed. */
static inline int ws_relay_read(ws_relay_t *r, void *buf, uint32_t cap) {
    if (r->secure) return tls_recv(&r->tls, buf, cap);
    int got = (int)recv(r->sock, (char *)buf, (int)cap, 0);
    if (got > 0) return got;
    if (got < 0 && net_would_block(net_errno())) return 0;
    return -1;
}

static inline void ws_relay_fail(ws_relay_t *r, const char *why) {
    snprintf(r->error, sizeof(r->error), "%s", why);
    ws_relay_close(r);
}

/* One masked client frame (RFC 6455 5.2). The gateway's payloads are all well
 * under 64 KB, so the 64-bit length form is never needed. */
static inline bool ws_relay_send_frame(ws_relay_t *r, uint8_t opcode, const uint8_t *a, uint32_t alen,
                                       const uint8_t *b, uint32_t blen) {
    uint32_t len = alen + blen;
    if (len > 0xFFFF) return false;
    uint8_t f[8 + 6 + WS_RELAY_DGRAM_MAX + 128];
    if (len + 8 > sizeof(f)) return false;
    uint32_t h = 0;
    f[h++] = (uint8_t)(0x80 | opcode);
    if (len < 126) f[h++] = (uint8_t)(0x80 | len);
    else { f[h++] = 0x80 | 126; f[h++] = (uint8_t)(len >> 8); f[h++] = (uint8_t)len; }
    uint32_t m = ws_relay_rand(r);
    uint8_t mask[4] = { (uint8_t)m, (uint8_t)(m >> 8), (uint8_t)(m >> 16), (uint8_t)(m >> 24) };
    memcpy(f + h, mask, 4);
    h += 4;
    for (uint32_t i = 0; i < alen; i++) f[h + i] = a[i] ^ mask[i & 3];
    for (uint32_t i = 0; i < blen; i++) f[h + alen + i] = b[i] ^ mask[(alen + i) & 3];
    return ws_relay_write(r, f, h + len);
}

/* Opens the WebSocket. Blocks for the connect and the upgrade, as the TLS connect
 * to RPCN does; the caller runs outside the emu mutex. */
static inline bool ws_relay_open(ws_relay_t *r, const char *url) {
    memset(r, 0, sizeof(*r));
    r->sock     = NET_SOCK_INVALID;
    r->tls.sock = NET_SOCK_INVALID;   /* a zeroed one is fd 0, which a close would take */
    snprintf(r->url, sizeof(r->url), "%s", url ? url : "");

    const char *p = r->url;
    uint16_t port;
    if (ws_relay__strnicmp(p, "wss://", 6) == 0)     { r->secure = true;  p += 6; port = 443; }
    else if (ws_relay__strnicmp(p, "ws://", 5) == 0) { r->secure = false; p += 5; port = 80; }
    else { ws_relay_fail(r, "the relay must be a ws:// or wss:// URL"); return false; }

    char host[128];
    size_t n = strcspn(p, ":/");
    if (!n || n >= sizeof(host)) { ws_relay_fail(r, "the relay URL names no host"); return false; }
    memcpy(host, p, n);
    host[n] = 0;
    p += n;
    if (*p == ':') {
        char *after;
        port = (uint16_t)strtoul(p + 1, &after, 10);
        p = after;
        if (!port) { ws_relay_fail(r, "the relay URL's port is not a number"); return false; }
    }
    const char *path = *p ? p : "/";

    if (r->secure) {
        if (!tls_connect(&r->tls, host, port, NULL)) {
            snprintf(r->error, sizeof(r->error), "%.180s", tls_last_error(&r->tls));
            ws_relay_close(r);
            return false;
        }
        r->sock = r->tls.sock;
    } else {
        /* The socket library is the session's to hold (rpcn_session_start). */
        if (!net_tcp_connect(&r->sock, host, port, WS_RELAY_TIMEOUT_MS)) {
            ws_relay_fail(r, "could not connect");
            return false;
        }
    }
    /* Every input of a match is one small message. Nagle would hold each for the
     * previous one's ACK. */
    int one = 1;
    setsockopt(r->sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));

    uint8_t key[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t v = ws_relay_rand(r);
        memcpy(key + i, &v, 4);
    }
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char key64[25];
    for (int i = 0, o = 0; i < 16; i += 3) {
        uint32_t v = (uint32_t)key[i] << 16 | (i + 1 < 16 ? (uint32_t)key[i + 1] << 8 : 0)
                   | (i + 2 < 16 ? key[i + 2] : 0);
        key64[o++] = b64[(v >> 18) & 63];
        key64[o++] = b64[(v >> 12) & 63];
        key64[o++] = i + 1 < 16 ? b64[(v >> 6) & 63] : '=';
        key64[o++] = i + 2 < 16 ? b64[v & 63] : '=';
        key64[o] = 0;
    }
    char req[640];
    int len = snprintf(req, sizeof(req),
                       "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                       "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nOrigin: %s\r\n"
                       "User-Agent: m2hle\r\n\r\n",
                       path, host, key64, WS_RELAY_ORIGIN);
    if (len <= 0 || len >= (int)sizeof(req) || !ws_relay_write(r, req, (uint32_t)len)) {
        ws_relay_fail(r, "could not send the WebSocket upgrade");
        return false;
    }

    /* The reply's head; anything after it is already frames. */
    uint64_t deadline = net_now_ms() + WS_RELAY_TIMEOUT_MS;
    for (;;) {
        r->in[r->in_used] = 0;
        char *end = strstr((char *)r->in, "\r\n\r\n");
        if (end) {
            if (strncmp((char *)r->in, "HTTP/1.1 101", 12) != 0) {
                char why[160];
                size_t line = strcspn((char *)r->in, "\r\n");
                snprintf(why, sizeof(why), "the gateway refused the relay: %.*s",
                         (int)(line < 100 ? line : 100), (char *)r->in);
                ws_relay_fail(r, why);
                return false;
            }
            uint32_t head = (uint32_t)(end + 4 - (char *)r->in);
            memmove(r->in, r->in + head, r->in_used - head);
            r->in_used -= head;
            break;
        }
        if (r->in_used + 1 >= sizeof(r->in)) { ws_relay_fail(r, "the gateway's reply is too long"); return false; }
        int got = ws_relay_read(r, r->in + r->in_used, (uint32_t)sizeof(r->in) - 1 - r->in_used);
        if (got < 0) { ws_relay_fail(r, "the gateway closed the connection"); return false; }
        if (got > 0) { r->in_used += (uint32_t)got; continue; }
        if (net_now_ms() > deadline) { ws_relay_fail(r, "the gateway did not answer"); return false; }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(r->sock, &rd);
        struct timeval tv = { 0, 50000 };
        select((int)r->sock + 1, &rd, NULL, NULL, &tv);
    }
    r->open = true;
    return true;
}

static inline bool ws_relay_send(ws_relay_t *r, uint32_t ip_be, uint16_t port, const void *data, uint32_t len) {
    if (!r->open || len > WS_RELAY_DGRAM_MAX) return false;
    uint8_t head[6];
    memcpy(head, &ip_be, 4);
    head[4] = (uint8_t)(port >> 8);
    head[5] = (uint8_t)port;
    if (!ws_relay_send_frame(r, 0x2, head, 6, (const uint8_t *)data, len)) {
        ws_relay_fail(r, "the connection to the gateway was lost");
        return false;
    }
    r->sent++;
    return true;
}

/* The next datagram: its length, 0 = none waiting, -1 = the relay is closed
 * (r->error says why). Pings are answered here. */
static inline int ws_relay_recv(ws_relay_t *r, void *buf, uint32_t cap, uint32_t *out_ip_be, uint16_t *out_port) {
    if (!r->open) return -1;
    for (int pass = 0; pass < 64; pass++) {
        /* A whole frame waiting? */
        if (r->in_used >= 2) {
            uint8_t b0 = r->in[0], b1 = r->in[1];
            uint32_t h = 2;
            uint64_t len = b1 & 0x7F;
            if (len == 126) {
                if (r->in_used < 4) goto more;
                len = (uint32_t)r->in[2] << 8 | r->in[3];
                h = 4;
            } else if (len == 127) {
                if (r->in_used < 10) goto more;
                len = 0;
                for (int i = 0; i < 8; i++) len = len << 8 | r->in[2 + i];
                h = 10;
            }
            bool masked = (b1 & 0x80) != 0;
            if (len > sizeof(r->in) - 14) { ws_relay_fail(r, "the gateway sent a message too large"); return -1; }
            if (r->in_used < h + (masked ? 4 : 0) + len) goto more;
            uint8_t *pl = r->in + h + (masked ? 4 : 0);
            if (masked)
                for (uint32_t i = 0; i < (uint32_t)len; i++) pl[i] ^= r->in[h + (i & 3)];
            uint32_t total = (uint32_t)(pl - r->in) + (uint32_t)len;
            uint8_t op = b0 & 0x0F;
            int out = 0;
            if (!(b0 & 0x80) || op == 0x0 || op == 0x1) {
                ws_relay_fail(r, "the gateway sent a message the relay does not understand");
                return -1;
            }
            if (op == 0x8) {
                char why[160];
                snprintf(why, sizeof(why), "the gateway closed the relay%s%.*s", len > 2 ? ": " : "",
                         len > 2 ? (int)(len - 2 < 100 ? len - 2 : 100) : 0, (char *)pl + 2);
                ws_relay_fail(r, why);
                return -1;
            }
            if (op == 0x9 && !ws_relay_send_frame(r, 0xA, pl, (uint32_t)len, NULL, 0)) {
                ws_relay_fail(r, "the connection to the gateway was lost");
                return -1;
            }
            if (op == 0x2 && len >= 6 && len - 6 <= cap) {
                if (out_ip_be) memcpy(out_ip_be, pl, 4);
                if (out_port)  *out_port = (uint16_t)(pl[4] << 8 | pl[5]);
                memcpy(buf, pl + 6, (size_t)len - 6);
                out = (int)len - 6;
            }
            memmove(r->in, r->in + total, r->in_used - total);
            r->in_used -= total;
            if (out) { r->received++; return out; }
            continue;
        }
    more:;
        int got = ws_relay_read(r, r->in + r->in_used, (uint32_t)sizeof(r->in) - r->in_used);
        if (got < 0) { ws_relay_fail(r, "the connection to the gateway was lost"); return -1; }
        if (got == 0) return 0;
        r->in_used += (uint32_t)got;
    }
    return 0;
}

#endif /* !__EMSCRIPTEN__ */
#endif /* WS_RELAY_H */
