/*
 * rpcs3_signal.h — RPCS3's peer signaling, so an RPCS3 player sees us as a
 * connected room member.
 *
 * RPCS3 does not use Sony's signaling. It has its own (rpcs3/Emu/NP/
 * signaling_handler.cpp), carried on the NP port (UDP 3658) under RPCS3's P2P
 * framing, and a PS3 game there gets its Matching2 "Established" event (0x5102)
 * only from it. Sonic the Fighters opens its RUDP channels on that event, so
 * nothing else happens until this has run.
 *
 * ── ON THE WIRE ────────────────────────────────────────────────────────────
 *
 * Every RPCS3 P2P datagram starts with the destination vport, u16 LITTLE-endian.
 * Vport 0 is RPCS3's own traffic, with one more byte, the subset: 0 = RPCN's
 * UDP pong, 1 = signaling. A signaling datagram is therefore 75 bytes:
 *
 *     00 00 01 | 72-byte signaling_packet
 *
 *   off size
 *   0   4   'S','I','G','N'
 *   4   4   version, u32 LE = 3
 *   8   8   timestamp_sender,   u64 LE, microseconds of the SENDER's clock
 *   16  8   timestamp_receiver, u64 LE, microseconds of the RECEIVER's clock
 *   24  4   command, u32 LE (RPCS3_SIG_*)
 *   28  4   the address this packet was sent to, raw network-order octets
 *   32  2   the port it was sent to, u16 LE
 *   34  36  SceNpId: 16-byte handle, NUL, 3 zero bytes, 8 opt, 8 reserved
 *   70  2   padding
 *
 * RPCS3 drops anything that is not exactly 72 bytes after the header, has the
 * wrong signature or version, or carries an npid with a character outside
 * [A-Za-z0-9_-] (np_helpers.cpp is_valid_npid).
 *
 * ── WHAT RPCS3 NEEDS FROM US ───────────────────────────────────────────────
 *
 *   * THE PEER IS ITS NPID, NOT ITS ADDRESS. RPCS3 looks every packet up by the
 *     npid inside it. Ours must be byte for byte the npid RPCN lists for us in
 *     the room.
 *   * RPCS3 marks us ACTIVE, and the game gets Established, ONLY when it gets a
 *     CONNECT_ACK for its own CONNECT. It repeats CONNECT every 200 ms until then.
 *     Our own CONNECT is for punching our NAT; RPCS3 answers it with a
 *     CONNECT_ACK that it REPEATS EVERY 200 ms UNTIL IT GETS A CONFIRM, so every
 *     CONNECT_ACK we get is answered with one.
 *   * ECHO THE TIMESTAMPS EXACTLY. RPCS3 works out its round trip as its own
 *     clock minus the echoed value and narrows the average to u32, which THROWS
 *     on a garbage echo and takes its signaling thread down. CONNECT_ACK and PONG
 *     echo the request's timestamp_sender; CONFIRM echoes the CONNECT_ACK's
 *     timestamp_receiver.
 *   * CONNECT, CONNECT_ACK, CONFIRM and INFO move the peer's address to the
 *     packet's source, and the game sends everything to that address from then
 *     on. So signaling and game traffic must leave from one socket, which they do:
 *     both go out on the session's P2P socket.
 *   * RPCS3 declares a peer dead after 60 s without a valid signaling packet.
 *     Answering its PINGs (every 10 s) keeps it alive; we ping too, for our own
 *     round trip and to hold NAT mappings open.
 *   * On leaving, send FINISHED: RPCS3 does not stop signaling on UserLeftRoom,
 *     and without it the game only notices at the 60 s timeout.
 *
 * Nothing here knows about rooms or games. The caller says who to connect to and
 * feeds it every datagram; it says who is connected.
 */
#ifndef RPCS3_SIGNAL_H
#define RPCS3_SIGNAL_H

#include "net_socket.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RPCS3_SIG_PACKET_SIZE  72u
#define RPCS3_SIG_WIRE_SIZE    75u      /* 00 00 01 + packet */
#define RPCS3_SIG_VERSION      3u
#define RPCS3_SIG_MAX_PEERS    8u

enum {
    RPCS3_SIG_PING         = 0,
    RPCS3_SIG_PONG         = 1,
    RPCS3_SIG_CONNECT      = 2,
    RPCS3_SIG_CONNECT_ACK  = 3,
    RPCS3_SIG_CONFIRM      = 4,
    RPCS3_SIG_FINISHED     = 5,
    RPCS3_SIG_FINISHED_ACK = 6,
    RPCS3_SIG_INFO         = 7,
};

/* RPCS3's own intervals (signaling_handler.h): CONNECT every 200 ms, PING every
 * 500 ms until the first PONG and every 10 s after, FINISHED every 500 ms. */
#define RPCS3_SIG_CONNECT_US    200000u
#define RPCS3_SIG_PING_FAST_US  500000u
#define RPCS3_SIG_PING_US      5000000u
#define RPCS3_SIG_FINISH_US     500000u
#define RPCS3_SIG_FINISH_TRIES  6u
#define RPCS3_SIG_DEAD_US     60000000u
#define RPCS3_SIG_RTT_SAMPLES   6u

typedef struct {
    bool     used;
    char     npid[17];
    uint32_t ip;              /* network order; where the peer is heard from */
    uint16_t port;

    bool     connecting;      /* sending CONNECT until it is acknowledged */
    bool     active;          /* our CONNECT was acknowledged */
    bool     peer_active;     /* we acknowledged theirs, so THEIR game has Established */
    bool     finishing;       /* sending FINISHED until acknowledged */
    uint32_t finish_tries;
    bool     dead;            /* they finished, or timed out */

    uint64_t next_connect_us;
    uint64_t next_ping_us;
    uint64_t next_finish_us;
    uint64_t last_rx_us;
    bool     got_pong;

    uint32_t rtt_us[RPCS3_SIG_RTT_SAMPLES];
    uint32_t rtt_count, rtt_next;
} rpcs3_sig_peer_t;

typedef bool (*rpcs3_sig_send_fn)(void *ctx, uint32_t ip_be, uint16_t port, const void *buf, uint32_t len);
typedef void (*rpcs3_sig_log_fn)(void *ctx, const char *msg);

typedef struct {
    char              my_npid[17];
    rpcs3_sig_peer_t  peers[RPCS3_SIG_MAX_PEERS];
    rpcs3_sig_send_fn send;
    void             *send_ctx;
    rpcs3_sig_log_fn  log;
    void             *log_ctx;
} rpcs3_sig_t;

static inline void rpcs3_sig_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void rpcs3_sig_put_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static inline uint32_t rpcs3_sig_get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t rpcs3_sig_get_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static inline void rpcs3_sig_note(rpcs3_sig_t *s, const char *fmt, ...) {
    if (!s->log) return;
    char buf[200];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    s->log(s->log_ctx, buf);
}

static inline void rpcs3_sig_init(rpcs3_sig_t *s, const char *my_npid,
                                  rpcs3_sig_send_fn send, void *send_ctx) {
    rpcs3_sig_log_fn log = s->log;
    void *log_ctx = s->log_ctx;
    memset(s, 0, sizeof(*s));
    s->log = log;
    s->log_ctx = log_ctx;
    snprintf(s->my_npid, sizeof(s->my_npid), "%s", my_npid ? my_npid : "");
    s->send = send;
    s->send_ctx = send_ctx;
}

/* RPCS3's is_valid_npid, so that we never learn a name it would refuse. */
static inline bool rpcs3_sig_valid_npid(const uint8_t *id) {
    for (int i = 0; i < 16; i++) {
        uint8_t c = id[i];
        if (!c) break;
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return id[16] == 0 && id[17] == 0 && id[18] == 0 && id[19] == 0;
}

static inline rpcs3_sig_peer_t *rpcs3_sig_find(rpcs3_sig_t *s, const char *npid) {
    for (uint32_t i = 0; i < RPCS3_SIG_MAX_PEERS; i++)
        if (s->peers[i].used && strncmp(s->peers[i].npid, npid, 16) == 0) return &s->peers[i];
    return NULL;
}

static inline rpcs3_sig_peer_t *rpcs3_sig_add(rpcs3_sig_t *s, const char *npid) {
    rpcs3_sig_peer_t *p = rpcs3_sig_find(s, npid);
    if (p) return p;
    for (uint32_t i = 0; i < RPCS3_SIG_MAX_PEERS; i++) {
        if (s->peers[i].used) continue;
        p = &s->peers[i];
        memset(p, 0, sizeof(*p));
        p->used = true;
        snprintf(p->npid, sizeof(p->npid), "%s", npid);
        return p;
    }
    return NULL;
}

static inline void rpcs3_sig_send(rpcs3_sig_t *s, rpcs3_sig_peer_t *p, uint32_t cmd,
                                  uint64_t ts_sender, uint64_t ts_receiver) {
    if (!p->ip || !p->port || !s->send) return;
    uint8_t w[RPCS3_SIG_WIRE_SIZE];
    memset(w, 0, sizeof(w));
    w[2] = 1;                                   /* vport 0 (LE), subset 1 */
    uint8_t *k = w + 3;
    k[0] = 'S'; k[1] = 'I'; k[2] = 'G'; k[3] = 'N';
    rpcs3_sig_put_u32(k + 4, RPCS3_SIG_VERSION);
    rpcs3_sig_put_u64(k + 8, ts_sender);
    rpcs3_sig_put_u64(k + 16, ts_receiver);
    rpcs3_sig_put_u32(k + 24, cmd);
    memcpy(k + 28, &p->ip, 4);                  /* where it was sent: raw octets */
    k[32] = (uint8_t)(p->port & 0xFF);          /* the port, u16 LE */
    k[33] = (uint8_t)(p->port >> 8);
    size_t n = strlen(s->my_npid);
    memcpy(k + 34, s->my_npid, n > 16 ? 16 : n);
    s->send(s->send_ctx, p->ip, p->port, w, sizeof(w));
}

static inline void rpcs3_sig_add_rtt(rpcs3_sig_peer_t *p, uint64_t now, uint64_t echoed) {
    if (echoed == 0 || echoed > now || now - echoed > 5000000u) return;
    p->rtt_us[p->rtt_next] = (uint32_t)(now - echoed);
    p->rtt_next = (p->rtt_next + 1) % RPCS3_SIG_RTT_SAMPLES;
    if (p->rtt_count < RPCS3_SIG_RTT_SAMPLES) p->rtt_count++;
}

/* Average round trip in µs, as RPCS3 computes its own (the mean of the last
 * six), or 0 before the first sample. */
static inline uint32_t rpcs3_sig_rtt_us(const rpcs3_sig_peer_t *p) {
    if (!p || !p->rtt_count) return 0;
    uint64_t sum = 0;
    for (uint32_t i = 0; i < p->rtt_count; i++) sum += p->rtt_us[i];
    return (uint32_t)(sum / p->rtt_count);
}

/* Begin (or keep) connecting to `npid`, last known at ip:port (network order).
 * An address we have since HEARD them from is kept: RPCS3 moves its own copy
 * of ours the same way. */
static inline rpcs3_sig_peer_t *rpcs3_sig_start(rpcs3_sig_t *s, const char *npid, uint32_t ip, uint16_t port) {
    rpcs3_sig_peer_t *p = rpcs3_sig_add(s, npid);
    if (!p) return NULL;
    if (!p->last_rx_us && ip && port) { p->ip = ip; p->port = port; }
    if (!p->active && !p->connecting) {
        p->connecting = true;
        p->dead = false;
        p->next_connect_us = 0;
        rpcs3_sig_note(s, "signaling: connecting to %s", p->npid);
    }
    return p;
}

/* Say goodbye: FINISHED until acknowledged, a few times at most. */
static inline void rpcs3_sig_finish(rpcs3_sig_t *s, const char *npid) {
    rpcs3_sig_peer_t *p = rpcs3_sig_find(s, npid);
    if (!p || p->dead) return;
    p->connecting = false;
    p->finishing = true;
    p->finish_tries = 0;
    p->next_finish_us = 0;
}

static inline void rpcs3_sig_finish_all(rpcs3_sig_t *s) {
    for (uint32_t i = 0; i < RPCS3_SIG_MAX_PEERS; i++)
        if (s->peers[i].used) rpcs3_sig_finish(s, s->peers[i].npid);
}

static inline void rpcs3_sig_forget(rpcs3_sig_t *s, const char *npid) {
    rpcs3_sig_peer_t *p = rpcs3_sig_find(s, npid);
    if (p) memset(p, 0, sizeof(*p));
}

/*
 * A datagram from ip:port. True if it was signaling (whatever became of it), so
 * the caller does not pass it on as game traffic.
 */
static inline bool rpcs3_sig_on_datagram(rpcs3_sig_t *s, uint32_t ip, uint16_t port,
                                         const uint8_t *buf, uint32_t len, uint64_t now) {
    if (len < 3 || buf[0] != 0 || buf[1] != 0 || buf[2] != 1) return false;
    if (len != RPCS3_SIG_WIRE_SIZE) return true;          /* RPCS3's own rule */
    const uint8_t *k = buf + 3;
    if (k[0] != 'S' || k[1] != 'I' || k[2] != 'G' || k[3] != 'N') return true;
    if (rpcs3_sig_get_u32(k + 4) != RPCS3_SIG_VERSION) return true;
    if (!rpcs3_sig_valid_npid(k + 34)) return true;

    char npid[17];
    memcpy(npid, k + 34, 16);
    npid[16] = '\0';
    uint64_t ts_sender   = rpcs3_sig_get_u64(k + 8);
    uint64_t ts_receiver = rpcs3_sig_get_u64(k + 16);
    uint32_t cmd         = rpcs3_sig_get_u32(k + 24);

    rpcs3_sig_peer_t *p = rpcs3_sig_find(s, npid);
    /* RPCS3 creates a peer for an unknown CONNECT or INFO, and answers FINISHED
     * even without one. We do the same for CONNECT: a member the room has not
     * told us about yet is still one we will want. */
    if (!p && (cmd == RPCS3_SIG_CONNECT || cmd == RPCS3_SIG_INFO)) p = rpcs3_sig_add(s, npid);
    if (!p) {
        if (cmd == RPCS3_SIG_FINISHED) {
            rpcs3_sig_peer_t tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.ip = ip; tmp.port = port;
            rpcs3_sig_send(s, &tmp, RPCS3_SIG_FINISHED_ACK, now, ts_sender);
        }
        return true;
    }

    p->last_rx_us = now;
    bool moves = cmd == RPCS3_SIG_CONNECT || cmd == RPCS3_SIG_CONNECT_ACK
              || cmd == RPCS3_SIG_CONFIRM || cmd == RPCS3_SIG_INFO;
    if (moves && (p->ip != ip || p->port != port)) {
        char a[32];
        rpcs3_sig_note(s, "signaling: %s is at %s", p->npid, net_addr_text(a, sizeof(a), ip, port));
        p->ip = ip;
        p->port = port;
    }

    switch (cmd) {
        case RPCS3_SIG_CONNECT:
            /* The answer that makes THEIR game see us. */
            rpcs3_sig_send(s, p, RPCS3_SIG_CONNECT_ACK, ts_sender, now);
            if (!p->peer_active) rpcs3_sig_note(s, "signaling: %s is connecting; acknowledged", p->npid);
            p->peer_active = true;
            p->dead = false;
            break;
        case RPCS3_SIG_CONNECT_ACK:
            rpcs3_sig_add_rtt(p, now, ts_sender);
            rpcs3_sig_send(s, p, RPCS3_SIG_CONFIRM, now, ts_receiver);
            if (!p->active) {
                rpcs3_sig_note(s, "signaling: connected to %s (%u us)", p->npid, rpcs3_sig_rtt_us(p));
                p->next_ping_us = now + RPCS3_SIG_PING_FAST_US;
            }
            p->active = true;
            p->connecting = false;
            p->dead = false;
            break;
        case RPCS3_SIG_CONFIRM:
            p->peer_active = true;
            break;
        case RPCS3_SIG_PING:
            rpcs3_sig_send(s, p, RPCS3_SIG_PONG, ts_sender, now);
            break;
        case RPCS3_SIG_PONG:
            rpcs3_sig_add_rtt(p, now, ts_sender);
            p->got_pong = true;
            break;
        case RPCS3_SIG_FINISHED:
            rpcs3_sig_send(s, p, RPCS3_SIG_FINISHED_ACK, now, ts_sender);
            rpcs3_sig_note(s, "signaling: %s finished", p->npid);
            p->active = p->peer_active = p->connecting = false;
            p->dead = true;
            break;
        case RPCS3_SIG_FINISHED_ACK:
            p->finishing = false;
            p->active = p->peer_active = false;
            p->dead = true;
            break;
        default:
            break;
    }
    return true;
}

static inline void rpcs3_sig_pump(rpcs3_sig_t *s, uint64_t now) {
    for (uint32_t i = 0; i < RPCS3_SIG_MAX_PEERS; i++) {
        rpcs3_sig_peer_t *p = &s->peers[i];
        if (!p->used) continue;
        if (p->finishing) {
            if (now >= p->next_finish_us) {
                if (p->finish_tries++ >= RPCS3_SIG_FINISH_TRIES) { p->finishing = false; p->dead = true; continue; }
                rpcs3_sig_send(s, p, RPCS3_SIG_FINISHED, now, 0);
                p->next_finish_us = now + RPCS3_SIG_FINISH_US;
            }
            continue;
        }
        if (p->dead) continue;
        if (p->connecting && now >= p->next_connect_us) {
            rpcs3_sig_send(s, p, RPCS3_SIG_CONNECT, now, 0);
            p->next_connect_us = now + RPCS3_SIG_CONNECT_US;
        }
        if (p->active && now >= p->next_ping_us) {
            rpcs3_sig_send(s, p, RPCS3_SIG_PING, now, 0);
            p->next_ping_us = now + (p->got_pong ? RPCS3_SIG_PING_US : RPCS3_SIG_PING_FAST_US);
        }
        if ((p->active || p->peer_active) && p->last_rx_us && now - p->last_rx_us > RPCS3_SIG_DEAD_US) {
            rpcs3_sig_note(s, "signaling: nothing from %s for 60 s; dead", p->npid);
            p->active = p->peer_active = false;
            p->dead = true;
        }
    }
}

/* Connected both ways: RPCS3 has Established, and so do we. */
static inline bool rpcs3_sig_linked(const rpcs3_sig_peer_t *p) {
    return p && !p->dead && (p->active || p->peer_active);
}

#endif /* RPCS3_SIGNAL_H */
