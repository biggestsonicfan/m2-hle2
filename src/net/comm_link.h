/*
 * comm_link.h — a linked comm board's datagrams over the room's P2P socket
 * (Pinboard #964, step 2 of VIRTUAL-ON-PS3.md's RPCN plan).
 *
 * board/m2comm.h runs MAME's two-node ring over a peer: a send/recv pair of
 * datagrams, each a frame type byte and its payload (the master's token and
 * ring size, then one 0x700-byte window per pass for Virtual-On). This is that
 * pair over a lossy, unordered UDP socket, the same one the room's lockstep
 * traffic uses: comm_link_send / comm_link_recv have m2comm_peer_t's shapes,
 * and the owner (netplay.h, or a test) hands it a transport and feeds it what
 * arrives. Like lockstep.h it knows nothing of sockets or the board.
 *
 * Wire (LOCKSTEP_PACKET_COMM): the room's 12-byte header, then
 *   u16 epoch   the sender's link, new each comm_link_start
 *   u16 len     the message's unpacked length
 *   u32 seq     the sender's message count in this epoch, from 1
 *   ...         the message, RLE-packed (comm_link_pack)
 * What follows from YAMP's link (rokuYAMP von-comm-packet.md), which runs the
 * same ROM traffic over the same kind of socket:
 *   - Packed. The window is mostly zero: about 120 bytes of 0x701.
 *   - Sent COMM_LINK_COPIES times. Nothing is resent later and nothing is
 *     acknowledged: Virtual-On's check on a packet is self-contained
 *     ([+0x556] == [+2] ^ 0xAE5E), and m2comm.h's master repeats its own ring
 *     messages, so a lost frame is only a frame the ROM did not get.
 *   - Each message taken once. The copies are dropped, and so is one more
 *     than COMM_LINK_WINDOW behind the newest; one that comes a little late
 *     (the ring size after the data frame sent in the same pass) is still
 *     taken. A new epoch (the peer's board started again) starts the count
 *     again.
 *   - The inbox keeps the COMM_LINK_INBOX newest. m2comm.h takes one data
 *     frame per pass; a peer that sent more than that between two passes loses
 *     its oldest ones here rather than the newest.
 * A message that packs to more than fits a datagram (COMM_LINK_DGRAM_MAX, the
 * gateway relay's limit) is not sent and is counted: the ROM sees a lost frame.
 */
#ifndef COMM_LINK_H
#define COMM_LINK_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "lockstep.h"

#define LOCKSTEP_PACKET_COMM 7u           /* lockstep.h's types end at PONG (6) */

#define COMM_LINK_MSG_MAX   0x1001u       /* m2comm.h M2COMM_MSG_MAX: a type byte and a window */
#define COMM_LINK_DGRAM_MAX 1200u         /* ws_relay.h WS_RELAY_DGRAM_MAX, the smallest transport */
#define COMM_LINK_COPIES    3u
#define COMM_LINK_INBOX     8u
#define COMM_LINK_WINDOW    32u           /* seqs remembered behind the newest */

#pragma pack(push, 1)
typedef struct {
    lockstep_header_t header;
    uint16_t epoch;
    uint16_t len;
    uint32_t seq;
} comm_link_head_t;
#pragma pack(pop)

_Static_assert(sizeof(comm_link_head_t) == 20, "comm link head must stay 20 bytes");

#define COMM_LINK_PACKED_MAX (COMM_LINK_DGRAM_MAX - (uint32_t)sizeof(comm_link_head_t))

typedef struct {
    /* The transport: one datagram to the other cabinet. */
    void   (*transmit)(void *ud, const void *buf, uint32_t len);
    void    *ud;
    lockstep_header_t header;      /* the owner's, stamped LOCKSTEP_PACKET_COMM */

    uint16_t epoch;                /* ours */
    uint32_t seq;                  /* our last message's */
    bool     heard;                /* a message has been taken from the peer */
    uint16_t peer_epoch;
    uint32_t peer_seq;             /* the newest taken from peer_epoch */
    uint32_t peer_seen;            /* bit k: peer_seq - k has been taken */

    uint8_t  inbox[COMM_LINK_INBOX][COMM_LINK_MSG_MAX];
    uint32_t inbox_len[COMM_LINK_INBOX];
    uint32_t head, tail;

    /* Diagnostics. */
    uint32_t sent, taken, stale, torn, too_big, overflow;
    uint32_t packed_peak;          /* the largest packed message sent */
} comm_link_t;

/* ---- RLE: 0x00..0x7F = that many plus one literal bytes follow;
 *           0x80..0xFF = the next byte, (c - 0x80 + 3) times. ---- */

static inline uint32_t comm_link__run(const uint8_t *src, uint32_t at, uint32_t n) {
    uint32_t r = 1;
    while (at + r < n && r < 130u && src[at + r] == src[at]) r++;
    return r;
}

/* Packed length, or 0 if it would not fit in cap. */
static inline uint32_t comm_link_pack(const uint8_t *src, uint32_t n, uint8_t *dst, uint32_t cap) {
    uint32_t i = 0, o = 0;
    while (i < n) {
        uint32_t r = comm_link__run(src, i, n);
        if (r >= 3u) {
            if (o + 2u > cap) return 0;
            dst[o++] = (uint8_t)(0x80u + r - 3u);
            dst[o++] = src[i];
            i += r;
            continue;
        }
        uint32_t lit = 0;                        /* literals up to the next run of three */
        while (i + lit < n && lit < 128u && comm_link__run(src, i + lit, n) < 3u) lit++;
        if (o + 1u + lit > cap) return 0;
        dst[o++] = (uint8_t)(lit - 1u);
        memcpy(dst + o, src + i, lit);
        o += lit;
        i += lit;
    }
    return o;
}

/* Unpacked length, or 0 if the input is torn or does not come to exactly n. */
static inline uint32_t comm_link_unpack(const uint8_t *src, uint32_t len, uint8_t *dst, uint32_t n) {
    uint32_t i = 0, o = 0;
    while (i < len) {
        uint8_t c = src[i++];
        if (c >= 0x80u) {
            uint32_t r = c - 0x80u + 3u;
            if (i >= len || o + r > n) return 0;
            memset(dst + o, src[i++], r);
            o += r;
        } else {
            uint32_t lit = c + 1u;
            if (i + lit > len || o + lit > n) return 0;
            memcpy(dst + o, src + i, lit);
            i += lit;
            o += lit;
        }
    }
    return o == n ? o : 0;
}

/* ---- the link ---- */

/* A new link: our epoch (different from the last one, so the peer starts its
 * count again), the room's header for our datagrams, and the transport. */
static inline void comm_link_start(comm_link_t *l, uint16_t epoch, const lockstep_header_t *header,
                                   void (*transmit)(void *, const void *, uint32_t), void *ud) {
    memset(l, 0, sizeof *l);
    l->epoch = epoch;
    l->header = *header;
    l->header.type = (uint8_t)LOCKSTEP_PACKET_COMM;
    l->transmit = transmit;
    l->ud = ud;
}

/* m2comm_peer_t send: one message out, packed, COMM_LINK_COPIES times. */
static inline void comm_link_send(void *ctx, const uint8_t *msg, uint32_t len) {
    comm_link_t *l = (comm_link_t *)ctx;
    uint8_t dgram[COMM_LINK_DGRAM_MAX];
    uint32_t packed = len ? comm_link_pack(msg, len, dgram + sizeof(comm_link_head_t), COMM_LINK_PACKED_MAX) : 0;
    if (!len || len > COMM_LINK_MSG_MAX || !packed) { l->too_big++; return; }
    comm_link_head_t h = { l->header, l->epoch, (uint16_t)len, ++l->seq };
    memcpy(dgram, &h, sizeof h);
    for (uint32_t k = 0; k < COMM_LINK_COPIES; k++)
        l->transmit(l->ud, dgram, (uint32_t)sizeof h + packed);
    if (packed > l->packed_peak) l->packed_peak = packed;
    l->sent++;
}

/* A message the peer has not been heard to send before. */
static inline bool comm_link__fresh(comm_link_t *l, const comm_link_head_t *h) {
    if (!l->heard || h->epoch != l->peer_epoch) {
        l->heard = true;
        l->peer_epoch = h->epoch;
        l->peer_seq = h->seq;
        l->peer_seen = 1u;
        return true;
    }
    int32_t ahead = (int32_t)(h->seq - l->peer_seq);
    if (ahead > 0) {
        l->peer_seen = (uint32_t)ahead < COMM_LINK_WINDOW ? l->peer_seen << ahead | 1u : 1u;
        l->peer_seq = h->seq;
        return true;
    }
    uint32_t behind = (uint32_t)-ahead;
    if (behind >= COMM_LINK_WINDOW || (l->peer_seen >> behind & 1u)) return false;
    l->peer_seen |= 1u << behind;
    return true;
}

/* A LOCKSTEP_PACKET_COMM datagram the owner has accepted as the peer's. */
static inline void comm_link_on_datagram(comm_link_t *l, const uint8_t *buf, uint32_t len) {
    comm_link_head_t h;
    if (len < sizeof h) { l->torn++; return; }
    memcpy(&h, buf, sizeof h);
    if (!h.len || h.len > COMM_LINK_MSG_MAX) { l->torn++; return; }
    uint8_t msg[COMM_LINK_MSG_MAX];
    if (!comm_link_unpack(buf + sizeof h, len - (uint32_t)sizeof h, msg, h.len)) { l->torn++; return; }
    if (!comm_link__fresh(l, &h)) { l->stale++; return; }
    if (l->tail - l->head == COMM_LINK_INBOX) { l->head++; l->overflow++; }
    memcpy(l->inbox[l->tail % COMM_LINK_INBOX], msg, h.len);
    l->inbox_len[l->tail++ % COMM_LINK_INBOX] = h.len;
    l->taken++;
}

/* m2comm_peer_t recv: the oldest message waiting, or 0. */
static inline uint32_t comm_link_recv(void *ctx, uint8_t *msg, uint32_t cap) {
    comm_link_t *l = (comm_link_t *)ctx;
    if (l->head == l->tail) return 0;
    uint32_t len = l->inbox_len[l->head % COMM_LINK_INBOX];
    if (len > cap) len = cap;
    memcpy(msg, l->inbox[l->head++ % COMM_LINK_INBOX], len);
    return len;
}

/* m2comm_peer_t pending: the messages waiting in the inbox. */
static inline uint32_t comm_link_pending(void *ctx) {
    const comm_link_t *l = (const comm_link_t *)ctx;
    return l->tail - l->head;
}

#endif /* COMM_LINK_H */
