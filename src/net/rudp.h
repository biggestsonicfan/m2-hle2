/*
 * rudp.h — Sony's RUDP (cellRudp, librudp.sprx) as PS3 Sonic the Fighters uses
 * it, so that a PS3 game running in RPCS3 can talk to us.
 *
 * RPCS3 runs Sony's own librudp (LLE) on top of its P2P socket, so these bytes
 * are exactly what a PS3 sends. The layout below was read out of the decrypted
 * module and then checked against a real PS3-vs-PS3 match captured through
 * RPCS3's sys_net_dump log (every SYN, ACK and message byte for byte).
 *
 * ── LAYERS ─────────────────────────────────────────────────────────────────
 *
 *   UDP datagram on the NP port
 *    └─ RPCS3 P2P header, LITTLE-endian: u16 dst vport, u16 src vport, u16 flags
 *       (STF's game socket is bound on vport 1, flags 1 = DGRAM_P2P)
 *        └─ one or more librudp sub-packets back to back (the PS3 bundles
 *           traffic for one peer for up to 30 ms, 1410 bytes)
 *            └─ u16 BE  (mux type << 14) | length of the whole sub-packet
 *               u16 BE  channel vport (STF: mux type 2, one vport word)
 *               RUDP header (big-endian throughout), payload
 *
 *   RUDP header:
 *     u8  (type << 6) | flags      type 0 DATA, 1 KEEPALIVE, 2 SYN, 3 RST
 *     u8  window, log-encoded free receive slots (0x30 = 48, the default)
 *     u16 seq                       one per SEGMENT; the SYN takes one
 *     u16 ack        if flags & 0x20   next expected seq, cumulative
 *     u16 ack delay  if flags & 0x04   ms the ACK sat before going out
 *     SYN body: u16 syn_flags, u16 ver_max 0x100, u16 ver_min 0x100,
 *               u32 sender's connection id, u16 mss 0x582
 *     RST body: u16 reason, u32 sender's connection id
 *     options   if flags & 0x10 (TLV; skipped here)
 *     payload   DATA only: the rest of the sub-packet
 *   Flag 0x08 marks a latency-critical segment (the PS3 sends it at once).
 *
 * ── HOW STF USES IT ────────────────────────────────────────────────────────
 *
 *   Three channels per peer, told apart by the vport word:
 *     1  reliable, UNORDERED (delivery-critical, not order-critical): the room
 *        owner <-> each member; entrant data, SyncStart, the 60-frame inputs
 *     2  unreliable: every pair; the per-frame input packets
 *     3  unreliable: opened, never used in a match
 *   The SYN's syn_flags say which: 0x0201 on channel 1, 0 on the others. A PS3
 *   resets a connection whose SYN disagrees (QUALITY_LEVEL_MISMATCH), so these
 *   are copied from the capture exactly.
 *
 *   STF never listens: both ends call cellRudpInitiate, so every connection is a
 *   simultaneous open -- both send SYN, both answer with a SYN-ACK, both land in
 *   ESTABLISHED and send an empty ACK. We also accept a SYN when we have not
 *   sent ours yet (answering SYN-ACK, as a passive end would), which a PS3 in
 *   SYN_SENT takes just as well.
 *
 *   No FIN: a PS3 closes with RST reason 0.
 *
 * Reliability here is deliberately simpler than librudp's -- no congestion
 * window and no SACK -- because channel 1 carries a few dozen small messages a
 * match. What has to match is what the PS3 can see: the header, sequence
 * numbering, cumulative ACKs, at-most-once delivery, and librudp's retransmit
 * clock (1 s, doubling to 16 s). A PS3 delays and bundles its ACKs, so a
 * shorter clock resends what has already arrived.
 */
#ifndef RUDP_H
#define RUDP_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define RUDP_CHANNELS     3u
#define RUDP_MSS          0x582u
#define RUDP_MAX_PAYLOAD  (RUDP_MSS - 0x40u)     /* 1346, librudp's per-segment payload */
#define RUDP_WINDOW_BYTE  0x30u
#define RUDP_SENDQ        64u
#define RUDP_GAME_VPORT   1u                     /* STF's P2P socket */

#define RUDP_TYPE_DATA     0u
#define RUDP_TYPE_KEEPALIVE 1u
#define RUDP_TYPE_SYN      2u
#define RUDP_TYPE_RST      3u

#define RUDP_F_ACK        0x20u
#define RUDP_F_OPTIONS    0x10u
#define RUDP_F_LATENCY    0x08u
#define RUDP_F_ACK_DELAY  0x04u
#define RUDP_F_MORE       0x02u

#define RUDP_SYN_RETRY_MS      500u
#define RUDP_CONNECT_TIMEOUT_MS 60000u
#define RUDP_RTO_MS            1000u   /* librudp: 1 s, doubling to 16 s */
#define RUDP_RTO_MAX_MS        16000u
#define RUDP_ACK_DELAY_MS      20u

typedef enum {
    RUDP_IDLE = 0,
    RUDP_SYN_SENT,
    RUDP_SYN_RCVD,
    RUDP_ESTABLISHED,
    RUDP_CLOSED,
} rudp_state_t;

typedef struct {
    bool     used;
    uint16_t seq;
    uint16_t len;
    uint8_t  flags;
    uint8_t  tries;
    uint64_t sent_ms;
    uint8_t  data[256];
} rudp_seg_t;

typedef struct {
    uint16_t     vport;         /* 1, 2 or 3 */
    bool         reliable;
    uint16_t     syn_flags;
    rudp_state_t state;

    uint32_t my_id, peer_id;
    uint16_t isn, peer_isn;
    uint16_t snd_nxt;           /* next data seq */
    uint16_t rcv_nxt;           /* next seq we expect (reliable) */
    uint64_t rcv_mask;          /* bit i: rcv_nxt + i already delivered, i >= 1 */

    uint64_t syn_start_ms, syn_next_ms;
    bool     ack_due;
    uint64_t ack_since_ms;
    uint32_t rx_unacked;

    rudp_seg_t sendq[RUDP_SENDQ];

    uint32_t rx_segments, tx_segments, retransmits, duplicates;
} rudp_chan_t;

typedef bool (*rudp_send_fn)(void *ctx, uint32_t ip_be, uint16_t port, const void *buf, uint32_t len);
/* A message arrived on channel `vport`. */
typedef void (*rudp_deliver_fn)(void *ctx, uint16_t vport, const uint8_t *payload, uint32_t len);

typedef struct {
    uint32_t ip;                /* network order */
    uint16_t port;
    rudp_chan_t ch[RUDP_CHANNELS];

    rudp_send_fn    send;
    void           *send_ctx;
    rudp_deliver_fn deliver;
    void           *deliver_ctx;
} rudp_peer_t;

/* ---- Helpers ------------------------------------------------------------- */

static inline void rudp_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void rudp_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline uint16_t rudp_get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t rudp_get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline int16_t rudp_diff(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(a - b); }

/* A cheap mixer for connection ids and ISNs; librudp hashes its own from the
 * socket, address and time. Only uniqueness matters to the peer. */
static inline uint32_t rudp_mix(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33;
    return (uint32_t)x;
}

static inline rudp_chan_t *rudp_chan(rudp_peer_t *p, uint16_t vport) {
    for (uint32_t i = 0; i < RUDP_CHANNELS; i++)
        if (p->ch[i].vport == vport) return &p->ch[i];
    return NULL;
}

/*
 * Set up a peer. `seed` makes the connection ids and ISNs (any value that
 * differs between peers and runs will do, e.g. a clock).
 */
static inline void rudp_peer_init(rudp_peer_t *p, uint32_t ip, uint16_t port, uint64_t seed,
                                  rudp_send_fn send, void *send_ctx,
                                  rudp_deliver_fn deliver, void *deliver_ctx) {
    memset(p, 0, sizeof(*p));
    p->ip = ip;
    p->port = port;
    p->send = send;
    p->send_ctx = send_ctx;
    p->deliver = deliver;
    p->deliver_ctx = deliver_ctx;
    for (uint32_t i = 0; i < RUDP_CHANNELS; i++) {
        rudp_chan_t *c = &p->ch[i];
        c->vport     = (uint16_t)(i + 1);
        c->reliable  = (i == 0);
        c->syn_flags = (i == 0) ? 0x0201u : 0x0000u;
        c->state     = RUDP_IDLE;
        c->my_id     = rudp_mix(seed * 3u + i) | 1u;
        c->isn       = (uint16_t)rudp_mix(seed * 7u + i + 0x100u);
    }
}

/* One sub-packet in its own datagram: P2P header, mux header, RUDP bytes. */
static inline void rudp_emit(rudp_peer_t *p, const rudp_chan_t *c, const uint8_t *rudp, uint32_t len) {
    if (!p->send || !p->ip || !p->port) return;
    uint8_t buf[6 + 4 + 32 + RUDP_MAX_PAYLOAD];
    if (len > sizeof(buf) - 10) return;
    buf[0] = RUDP_GAME_VPORT; buf[1] = 0;       /* dst vport, LE */
    buf[2] = RUDP_GAME_VPORT; buf[3] = 0;       /* src vport, LE */
    buf[4] = 1; buf[5] = 0;                     /* flags: DGRAM_P2P */
    uint16_t sub = (uint16_t)(4u + len);
    rudp_be16(buf + 6, (uint16_t)(0x8000u | sub));   /* mux type 2 */
    rudp_be16(buf + 8, c->vport);
    memcpy(buf + 10, rudp, len);
    p->send(p->send_ctx, p->ip, p->port, buf, 10u + len);
}

static inline uint32_t rudp_put_syn_body(const rudp_chan_t *c, uint8_t *b) {
    rudp_be16(b, c->syn_flags);
    rudp_be16(b + 2, 0x0100);
    rudp_be16(b + 4, 0x0100);
    rudp_be32(b + 6, c->my_id);
    rudp_be16(b + 10, RUDP_MSS);
    return 12;
}

static inline void rudp_send_syn(rudp_peer_t *p, rudp_chan_t *c, bool with_ack) {
    uint8_t b[32];
    uint32_t n = 0;
    b[n++] = (uint8_t)((RUDP_TYPE_SYN << 6) | (with_ack ? RUDP_F_ACK : 0));
    b[n++] = RUDP_WINDOW_BYTE;
    rudp_be16(b + n, c->isn); n += 2;
    if (with_ack) { rudp_be16(b + n, c->rcv_nxt); n += 2; }
    n += rudp_put_syn_body(c, b + n);
    rudp_emit(p, c, b, n);
}

/* The empty type-0 ACK: seq is snd_nxt and is not consumed. */
static inline void rudp_send_ack(rudp_peer_t *p, rudp_chan_t *c, uint64_t now) {
    uint8_t b[8];
    bool delay = c->ack_due && c->state == RUDP_ESTABLISHED;
    b[0] = (uint8_t)((RUDP_TYPE_DATA << 6) | RUDP_F_ACK | (delay ? RUDP_F_ACK_DELAY : 0));
    b[1] = RUDP_WINDOW_BYTE;
    rudp_be16(b + 2, c->snd_nxt);
    rudp_be16(b + 4, c->rcv_nxt);
    uint32_t n = 6;
    if (delay) {
        uint64_t ms = now > c->ack_since_ms ? now - c->ack_since_ms : 0;
        rudp_be16(b + 6, (uint16_t)(ms > 0xFFFF ? 0xFFFF : ms));
        n = 8;
    }
    rudp_emit(p, c, b, n);
    c->ack_due = false;
    c->rx_unacked = 0;
}

static inline void rudp_send_rst(rudp_peer_t *p, rudp_chan_t *c, uint16_t reason) {
    uint8_t b[10];
    b[0] = (uint8_t)(RUDP_TYPE_RST << 6);
    b[1] = 0;
    rudp_be16(b + 2, 0);
    rudp_be16(b + 4, reason);
    rudp_be32(b + 6, c->my_id);
    rudp_emit(p, c, b, 10);
}

static inline void rudp_established(rudp_chan_t *c) {
    c->state   = RUDP_ESTABLISHED;
    c->snd_nxt = (uint16_t)(c->isn + 1u);
}

/* ---- Opening and closing ------------------------------------------------- */

/* cellRudpInitiate: send SYN and keep sending it until answered. */
static inline void rudp_connect(rudp_peer_t *p, uint16_t vport, uint64_t now) {
    rudp_chan_t *c = rudp_chan(p, vport);
    if (!c || c->state == RUDP_ESTABLISHED || c->state == RUDP_SYN_SENT || c->state == RUDP_SYN_RCVD) return;
    c->state        = RUDP_SYN_SENT;
    c->syn_start_ms = now;
    c->syn_next_ms  = now + RUDP_SYN_RETRY_MS;
    rudp_send_syn(p, c, false);
}

static inline void rudp_connect_all(rudp_peer_t *p, uint64_t now) {
    for (uint32_t i = 0; i < RUDP_CHANNELS; i++) rudp_connect(p, p->ch[i].vport, now);
}

/* cellRudpTerminate: RST reason 0 on every open channel. */
static inline void rudp_close_all(rudp_peer_t *p) {
    for (uint32_t i = 0; i < RUDP_CHANNELS; i++) {
        rudp_chan_t *c = &p->ch[i];
        if (c->state == RUDP_ESTABLISHED || c->state == RUDP_SYN_RCVD) rudp_send_rst(p, c, 0);
        c->state = RUDP_CLOSED;
    }
}

static inline bool rudp_open(const rudp_peer_t *p, uint16_t vport) {
    for (uint32_t i = 0; i < RUDP_CHANNELS; i++)
        if (p->ch[i].vport == vport) return p->ch[i].state == RUDP_ESTABLISHED;
    return false;
}

static inline bool rudp_all_open(const rudp_peer_t *p) {
    for (uint32_t i = 0; i < RUDP_CHANNELS; i++)
        if (p->ch[i].state != RUDP_ESTABLISHED) return false;
    return true;
}

/* ---- Sending ------------------------------------------------------------- */

/*
 * cellRudpWrite. `latency` is STF's write flag 8 (header flag 0x08), which it
 * sets on the per-frame input packet only. False if the channel is not open, the
 * message is too big for one segment, or the reliable queue is full.
 */
static inline bool rudp_write(rudp_peer_t *p, uint16_t vport, const void *msg, uint32_t len,
                              bool latency, uint64_t now) {
    rudp_chan_t *c = rudp_chan(p, vport);
    if (!c || c->state != RUDP_ESTABLISHED || len > sizeof(c->sendq[0].data)) return false;

    uint8_t b[8 + sizeof(c->sendq[0].data)];
    uint8_t flags = latency ? RUDP_F_LATENCY : 0;
    uint32_t n = 0;
    if (c->reliable) flags |= RUDP_F_ACK | RUDP_F_ACK_DELAY;   /* piggyback our ACK */
    b[n++] = (uint8_t)((RUDP_TYPE_DATA << 6) | flags);
    b[n++] = RUDP_WINDOW_BYTE;
    uint16_t seq = c->snd_nxt;
    rudp_be16(b + n, seq); n += 2;
    if (c->reliable) {
        rudp_be16(b + n, c->rcv_nxt); n += 2;
        uint64_t ms = (c->ack_due && now > c->ack_since_ms) ? now - c->ack_since_ms : 0;
        rudp_be16(b + n, (uint16_t)(ms > 0xFFFF ? 0xFFFF : ms)); n += 2;
    }
    memcpy(b + n, msg, len);
    n += len;

    if (c->reliable) {
        rudp_seg_t *slot = NULL;
        for (uint32_t i = 0; i < RUDP_SENDQ && !slot; i++) if (!c->sendq[i].used) slot = &c->sendq[i];
        if (!slot) return false;
        slot->used = true;
        slot->seq = seq;
        slot->len = (uint16_t)len;
        slot->flags = latency ? RUDP_F_LATENCY : 0;
        slot->tries = 1;
        slot->sent_ms = now;
        memcpy(slot->data, msg, len);
        c->ack_due = false;
        c->rx_unacked = 0;
    }
    c->snd_nxt = (uint16_t)(seq + 1u);
    c->tx_segments++;
    rudp_emit(p, c, b, n);
    return true;
}

static inline void rudp_resend(rudp_peer_t *p, rudp_chan_t *c, rudp_seg_t *s, uint64_t now) {
    uint8_t b[8 + sizeof(s->data)];
    uint32_t n = 0;
    b[n++] = (uint8_t)((RUDP_TYPE_DATA << 6) | s->flags | RUDP_F_ACK | RUDP_F_ACK_DELAY);
    b[n++] = RUDP_WINDOW_BYTE;
    rudp_be16(b + n, s->seq); n += 2;
    rudp_be16(b + n, c->rcv_nxt); n += 2;
    rudp_be16(b + n, 0); n += 2;
    memcpy(b + n, s->data, s->len);
    n += s->len;
    rudp_emit(p, c, b, n);
    s->sent_ms = now;
    if (s->tries < 0xFF) s->tries++;
    c->retransmits++;
}

/* ---- Receiving ----------------------------------------------------------- */

static inline void rudp_on_ack(rudp_chan_t *c, uint16_t ack) {
    for (uint32_t i = 0; i < RUDP_SENDQ; i++) {
        rudp_seg_t *s = &c->sendq[i];
        if (s->used && rudp_diff(s->seq, ack) < 0) s->used = false;
    }
}

static inline void rudp_accept_syn(rudp_peer_t *p, rudp_chan_t *c, uint16_t seq, uint32_t peer_id) {
    c->peer_id  = peer_id;
    c->peer_isn = seq;
    c->rcv_nxt  = (uint16_t)(seq + 1u);
    c->rcv_mask = 0;
    c->state    = RUDP_SYN_RCVD;
    rudp_send_syn(p, c, true);
}

static inline void rudp_on_data(rudp_peer_t *p, rudp_chan_t *c, uint16_t seq, const uint8_t *pl,
                                uint32_t len, uint64_t now) {
    if (!c->reliable) {
        /* Unordered and unreliable: librudp hands these over in arrival order
         * and does not look for duplicates. */
        c->rx_segments++;
        if (len && p->deliver) p->deliver(p->deliver_ctx, c->vport, pl, len);
        return;
    }
    int16_t d = rudp_diff(seq, c->rcv_nxt);
    bool fresh = false;
    if (d == 0) {
        fresh = true;
        c->rcv_mask |= 1u;
        while (c->rcv_mask & 1u) { c->rcv_mask >>= 1; c->rcv_nxt++; }
    } else if (d > 0 && d < 64) {
        if (!((c->rcv_mask >> d) & 1u)) { fresh = true; c->rcv_mask |= (1ull << d); }
    }
    if (!c->ack_due) c->ack_since_ms = now;
    c->ack_due = true;
    c->rx_unacked++;
    if (!fresh) { c->duplicates++; return; }
    c->rx_segments++;
    if (len && p->deliver) p->deliver(p->deliver_ctx, c->vport, pl, len);
}

/* One RUDP sub-packet (after the mux header) on channel `c`. */
static inline void rudp_on_segment(rudp_peer_t *p, rudp_chan_t *c, const uint8_t *r, uint32_t len,
                                   uint64_t now) {
    if (len < 4) return;
    uint8_t  type  = (uint8_t)(r[0] >> 6);
    uint8_t  flags = (uint8_t)(r[0] & 0x3Fu);
    uint16_t seq   = rudp_get16(r + 2);
    uint32_t o = 4;
    bool     has_ack = (flags & RUDP_F_ACK) != 0;
    uint16_t ack = 0;
    if (has_ack) { if (o + 2 > len) return; ack = rudp_get16(r + o); o += 2; }
    if (flags & RUDP_F_ACK_DELAY) { if (!has_ack || o + 2 > len) return; o += 2; }

    uint16_t syn_flags = 0; uint32_t id = 0;
    if (type == RUDP_TYPE_SYN) {
        if (o + 12 > len) return;
        syn_flags = rudp_get16(r + o);
        uint16_t vmax = rudp_get16(r + o + 2), vmin = rudp_get16(r + o + 4);
        id = rudp_get32(r + o + 6);
        o += 12;
        if (vmin > 0x100 || vmax < 0x100) { rudp_send_rst(p, c, 4); return; }
        (void)syn_flags;
    } else if (type == RUDP_TYPE_RST) {
        if (o + 6 > len) return;
        id = rudp_get32(r + o + 2);
        o += 6;
    }
    if (flags & RUDP_F_OPTIONS) {
        /* TLV: a type byte (bit 7 = last); type 0 is a lone pad byte, the rest
         * carry a length. SACK, timestamps and retransmit info: all skipped. */
        while (o < len) {
            uint8_t t = r[o++];
            if ((t & 0x7Fu) != 0) {
                if (o >= len) return;
                uint8_t l = r[o++];
                o += l;
            }
            if (t & 0x80u) break;
        }
        if (o > len) return;
    }

    switch (type) {
        case RUDP_TYPE_SYN:
            if (!has_ack) {
                /* Their opening SYN. In IDLE or SYN_SENT that makes us SYN_RCVD;
                 * in SYN_RCVD it is a retry; once ESTABLISHED a SYN with a new id
                 * is the peer starting over. */
                if (c->state == RUDP_ESTABLISHED && id == c->peer_id) { rudp_send_ack(p, c, now); break; }
                if (c->state == RUDP_SYN_RCVD && id == c->peer_id) { rudp_send_syn(p, c, true); break; }
                if (c->state == RUDP_IDLE || c->state == RUDP_CLOSED) c->syn_start_ms = now;
                rudp_accept_syn(p, c, seq, id);
                c->syn_next_ms = now + RUDP_SYN_RETRY_MS;
            } else {
                /* SYN-ACK: they have our SYN. */
                if (ack != (uint16_t)(c->isn + 1u)) { rudp_send_rst(p, c, 2); break; }
                if (c->state == RUDP_SYN_SENT) {
                    c->peer_id  = id;
                    c->peer_isn = seq;
                    c->rcv_nxt  = (uint16_t)(seq + 1u);
                    c->rcv_mask = 0;
                }
                if (c->state == RUDP_SYN_SENT || c->state == RUDP_SYN_RCVD) rudp_established(c);
                if (c->state == RUDP_ESTABLISHED) rudp_send_ack(p, c, now);
            }
            break;

        case RUDP_TYPE_RST:
            if (c->peer_id && id == c->peer_id) c->state = RUDP_CLOSED;
            break;

        case RUDP_TYPE_KEEPALIVE:
            if (!has_ack) {
                uint8_t b[6];
                b[0] = (uint8_t)((RUDP_TYPE_KEEPALIVE << 6) | RUDP_F_ACK);
                b[1] = RUDP_WINDOW_BYTE;
                rudp_be16(b + 2, c->snd_nxt);
                rudp_be16(b + 4, c->rcv_nxt);
                rudp_emit(p, c, b, 6);
            }
            break;

        case RUDP_TYPE_DATA:
        default:
            if (c->state == RUDP_SYN_RCVD && has_ack && ack == (uint16_t)(c->isn + 1u)) rudp_established(c);
            if (c->state != RUDP_ESTABLISHED) break;
            if (has_ack && c->reliable) rudp_on_ack(c, ack);
            if (len > o) rudp_on_data(p, c, seq, r + o, len - o, now);
            break;
    }
}

/*
 * A whole datagram from the peer, RPCS3 P2P header included. False if it is not
 * RUDP for STF's socket (signaling, say), so the caller can look elsewhere.
 */
static inline bool rudp_on_datagram(rudp_peer_t *p, const uint8_t *buf, uint32_t len, uint64_t now) {
    if (len < 6) return false;
    uint16_t dst   = (uint16_t)(buf[0] | (buf[1] << 8));
    uint16_t flags = (uint16_t)(buf[4] | (buf[5] << 8));
    if (dst != RUDP_GAME_VPORT || !(flags & 1u)) return false;
    uint32_t at = 6;
    while (len - at > 1) {
        uint16_t h = rudp_get16(buf + at);
        uint32_t T = h >> 14, L = h & 0x7FFu;
        if (T == 0 || L < 2 * T || at + L > len) break;
        if (T == 2) {
            rudp_chan_t *c = rudp_chan(p, rudp_get16(buf + at + 2));
            if (c) rudp_on_segment(p, c, buf + at + 4, L - 4, now);
        }
        at += L;
    }
    return true;
}

/* ---- Timers -------------------------------------------------------------- */

static inline void rudp_pump(rudp_peer_t *p, uint64_t now) {
    for (uint32_t i = 0; i < RUDP_CHANNELS; i++) {
        rudp_chan_t *c = &p->ch[i];
        if (c->state == RUDP_SYN_SENT || c->state == RUDP_SYN_RCVD) {
            if (now - c->syn_start_ms > RUDP_CONNECT_TIMEOUT_MS) {
                rudp_send_rst(p, c, 3);
                c->state = RUDP_CLOSED;
                continue;
            }
            if (now >= c->syn_next_ms) {
                rudp_send_syn(p, c, c->state == RUDP_SYN_RCVD);
                c->syn_next_ms = now + RUDP_SYN_RETRY_MS;
            }
            continue;
        }
        if (c->state != RUDP_ESTABLISHED) continue;
        if (c->reliable) {
            for (uint32_t k = 0; k < RUDP_SENDQ; k++) {
                rudp_seg_t *s = &c->sendq[k];
                if (!s->used) continue;
                uint32_t rto = RUDP_RTO_MS << (s->tries > 4 ? 4 : s->tries - 1);
                if (rto > RUDP_RTO_MAX_MS) rto = RUDP_RTO_MAX_MS;
                if (now - s->sent_ms >= rto) rudp_resend(p, c, s, now);
            }
            if (c->ack_due && (c->rx_unacked >= 2 || now - c->ack_since_ms >= RUDP_ACK_DELAY_MS))
                rudp_send_ack(p, c, now);
        }
    }
}

#endif /* RUDP_H */
