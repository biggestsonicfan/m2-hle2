/*
 * ps3net_test.c — the PS3 cross-play wire layers, without a server or an RPCS3.
 *
 * Three parts:
 *  (A) Sony RUDP (net/rudp.h) against bytes a real PS3 sent: a PS3 build of STF
 *      in RPCS3 was captured playing another through RPCS3's sys_net_dump log.
 *      Our SYN has to be byte for byte the PS3's, and our answer to the PS3's
 *      opening SYNs has to be what the other PS3 answered.
 *  (B) RUDP against itself: simultaneous open on all three channels, then
 *      messages over a lossy, reordering wire. Channel 1 must deliver every
 *      message exactly once; channels 2 and 3 whatever arrives.
 *  (C) RPCS3's signaling (net/rpcs3_signal.h): the 75-byte layout, and the
 *      handshake between two ends that makes each one's game see the other.
 *  (D) The room's owner and its line (net/ps3_link.h): choosing the fighters
 *      (np_session_build_fight_entries), the line after a result
 *      (np_session_rotate_queue_after_match), and the lockstep's shape under
 *      the room's match flags (SyncIo_Init_rings); the owner's phase machine
 *      stepped by hand; and the sign-in's bind before the TLS connect.
 */
#define NDEBUG 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps3_link.h"
#include "rpcs3_signal.h"
#include "rudp.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

static uint32_t hex(const char *s, uint8_t *out) {
    uint32_t n = 0;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        unsigned v;
        sscanf(s, "%2x", &v);
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

/* ---- A wire: datagrams queued in both directions ------------------------- */

#define WIRE_MAX 4096
typedef struct { uint8_t data[1600]; uint32_t len; } dgram_t;
typedef struct {
    dgram_t  q[WIRE_MAX];
    uint32_t head, count;
    uint32_t sent;
    uint32_t drop_every;   /* drop every Nth datagram (0 = none) */
} wire_t;

static bool wire_send(void *ctx, uint32_t ip, uint16_t port, const void *buf, uint32_t len) {
    (void)ip; (void)port;
    wire_t *w = (wire_t *)ctx;
    w->sent++;
    if (w->drop_every && w->sent % w->drop_every == 0) return true;
    if (w->count == WIRE_MAX || len > sizeof(w->q[0].data)) return false;
    dgram_t *d = &w->q[(w->head + w->count++) % WIRE_MAX];
    memcpy(d->data, buf, len);
    d->len = len;
    return true;
}

static bool wire_take(wire_t *w, dgram_t *out) {
    if (!w->count) return false;
    *out = w->q[w->head];
    w->head = (w->head + 1) % WIRE_MAX;
    w->count--;
    return true;
}

/* What a peer delivered, per channel. */
typedef struct {
    uint32_t count[4];
    uint32_t seen[64];     /* channel 1: times each message number arrived */
    uint8_t  last[4][300];
    uint32_t last_len[4];
} inbox_t;

static void inbox_deliver(void *ctx, uint16_t vport, const uint8_t *p, uint32_t len) {
    inbox_t *in = (inbox_t *)ctx;
    if (vport > 3) return;
    in->count[vport]++;
    if (len <= sizeof(in->last[0])) { memcpy(in->last[vport], p, len); in->last_len[vport] = len; }
    if (vport == 1 && len >= 1 && p[0] < 64) in->seen[p[0]]++;
}

/* ---- (A) Against a real PS3 ---------------------------------------------- */

static void test_capture(void) {
    printf("-- (A) RUDP against a captured PS3\n");

    /* The joining PS3's first datagram: SYNs for channels 1, 2 and 3 (as the
     * game passed it to sendto; RPCS3 adds its 6-byte P2P header). */
    static const char *ps3_syns =
        "80 14 00 01 80 30 4A 3A 02 01 01 00 01 00 6B E2 9A D2 05 82 "
        "80 14 00 02 80 30 BF 92 00 00 01 00 01 00 C4 AC 47 BE 05 82 "
        "80 14 00 03 80 30 BD 3A 00 00 01 00 01 00 D2 44 11 0A 05 82";

    /* Our SYN, given the PS3's ISN and connection id, is its SYN. */
    wire_t *w = calloc(1, sizeof(wire_t));
    inbox_t in;
    memset(&in, 0, sizeof(in));
    rudp_peer_t p;
    rudp_peer_init(&p, 0x0100007F, 3658, 1, wire_send, w, inbox_deliver, &in);
    p.ch[0].isn = 0x4A3A; p.ch[0].my_id = 0x6BE29AD2;
    p.ch[1].isn = 0xBF92; p.ch[1].my_id = 0xC4AC47BE;
    p.ch[2].isn = 0xBD3A; p.ch[2].my_id = 0xD244110A;
    rudp_connect_all(&p, 1000);
    uint8_t want[128];
    uint32_t wn = hex(ps3_syns, want);
    uint8_t got[128];
    uint32_t gn = 0;
    dgram_t d;
    bool framed = true;
    while (wire_take(w, &d)) {
        framed = framed && d.len > 6 && d.data[0] == 1 && d.data[1] == 0 && d.data[2] == 1
                 && d.data[3] == 0 && d.data[4] == 1 && d.data[5] == 0;
        memcpy(got + gn, d.data + 6, d.len - 6);
        gn += d.len - 6;
    }
    CHECK(framed, "each datagram carries RPCS3's P2P header: vport 1 -> 1, DGRAM_P2P");
    CHECK(gn == wn && memcmp(got, want, wn) == 0, "our three SYNs are the PS3's, byte for byte");

    /* A fresh peer that has not opened yet gets the PS3's SYNs: it must answer
     * each with a SYN-ACK acknowledging ISN+1 and carrying the channel's flags. */
    memset(w, 0, sizeof(*w));
    rudp_peer_t q;
    rudp_peer_init(&q, 0x0100007F, 3658, 2, wire_send, w, inbox_deliver, &in);
    uint8_t dg[256] = { 1, 0, 1, 0, 1, 0 };
    uint32_t n = 6 + hex(ps3_syns, dg + 6);
    rudp_on_datagram(&q, dg, n, 2000);
    uint32_t synacks = 0;
    bool acks_ok = true, flags_ok = true;
    static const uint16_t isn_plus1[3] = { 0x4A3B, 0xBF93, 0xBD3B };
    while (wire_take(w, &d)) {
        const uint8_t *r = d.data + 10;
        uint16_t vport = (uint16_t)((d.data[8] << 8) | d.data[9]);
        if (r[0] != 0xA0) continue;      /* SYN | ACK */
        synacks++;
        uint16_t ack = (uint16_t)((r[4] << 8) | r[5]);
        uint16_t sf  = (uint16_t)((r[6] << 8) | r[7]);
        if (vport < 1 || vport > 3 || ack != isn_plus1[vport - 1]) acks_ok = false;
        if (sf != (vport == 1 ? 0x0201 : 0x0000)) flags_ok = false;
    }
    CHECK(synacks == 3, "the PS3's three SYNs are each answered with a SYN-ACK");
    CHECK(acks_ok, "each SYN-ACK acknowledges the PS3's ISN + 1");
    CHECK(flags_ok, "syn_flags 0x0201 on channel 1 and 0 on 2 and 3, as the PS3 sends");
    CHECK(q.ch[0].state == RUDP_SYN_RCVD, "a SYN we did not initiate leaves the channel in SYN_RCVD");

    /* The PS3's empty ACK completes it. It acknowledges our ISN + 1. */
    uint8_t ack[64] = { 1, 0, 1, 0, 1, 0 };
    uint32_t an = 6;
    for (uint32_t c = 0; c < 3; c++) {
        uint16_t a = (uint16_t)(q.ch[c].isn + 1u);
        uint16_t seq = isn_plus1[c];
        uint8_t sub[10] = { 0x80, 0x0A, 0x00, (uint8_t)(c + 1), 0x20, 0x30,
                            (uint8_t)(seq >> 8), (uint8_t)seq, (uint8_t)(a >> 8), (uint8_t)a };
        memcpy(ack + an, sub, 10);
        an += 10;
    }
    rudp_on_datagram(&q, ack, an, 2100);
    CHECK(rudp_all_open(&q), "the PS3's bundled ACKs open all three channels");

    /* Its first reliable message, the entrant data (message 2), flags 0: no ACK
     * piggybacked, sequence ISN + 1. */
    static const char *entrant_hdr = "80 73 00 01 00 30 4A 3B 02 00 00 00 00 00 01";
    uint8_t msg[200] = { 1, 0, 1, 0, 1, 0 };
    uint32_t mn = 6 + hex(entrant_hdr, msg + 6);
    memset(msg + mn, 0, 100);
    mn += 100;
    memset(&in, 0, sizeof(in));
    rudp_on_datagram(&q, msg, mn, 2200);
    CHECK(in.count[1] == 1 && in.last_len[1] == 107 && in.last[1][0] == 2,
          "the PS3's entrant data arrives as one 107-byte message on channel 1");
    rudp_on_datagram(&q, msg, mn, 2300);
    CHECK(in.count[1] == 1, "the same segment again is not delivered twice");

    /* A captured SyncIo, unreliable and latency-critical on channel 2. */
    static const char *syncio =
        "80 25 00 02 08 30 1D A0 00 00 00 00 00 00 01 00 10 00 00 00 00 01 00 00 "
        "FF FF FF FF FF FF FF FF FF 00 10 00 00";
    uint8_t si[80] = { 1, 0, 1, 0, 1, 0 };
    uint32_t sn = 6 + hex(syncio, si + 6);
    rudp_on_datagram(&q, si, sn, 2400);
    CHECK(in.count[2] == 1 && in.last_len[2] == 29, "a captured SyncIo arrives as a 29-byte message on channel 2");

    /* And ours looks the same: flag 0x08, no ACK on an unreliable channel. */
    memset(w, 0, sizeof(*w));
    rudp_write(&q, 2, in.last[2], 29, true, 2500);
    CHECK(wire_take(w, &d) && d.len == 6 + 4 + 4 + 29 && d.data[10] == 0x08 && d.data[11] == 0x30,
          "our SyncIo goes out as type 0, flag 0x08, window 0x30, no ACK");
    free(w);
}

/* ---- (B) RUDP against itself --------------------------------------------- */

static void run_pair(rudp_peer_t *a, rudp_peer_t *b, wire_t *ab, wire_t *ba, uint64_t *now, int rounds) {
    for (int r = 0; r < rounds; r++) {
        dgram_t d;
        /* Deliver in a scrambled order: every other queued datagram goes first. */
        dgram_t hold[64];
        uint32_t nh = 0;
        while (wire_take(ab, &d)) { if (nh < 64 && (ab->sent + nh) % 3 == 0) hold[nh++] = d; else rudp_on_datagram(b, d.data, d.len, *now); }
        for (uint32_t i = 0; i < nh; i++) rudp_on_datagram(b, hold[i].data, hold[i].len, *now);
        nh = 0;
        while (wire_take(ba, &d)) { if (nh < 64 && (ba->sent + nh) % 3 == 0) hold[nh++] = d; else rudp_on_datagram(a, d.data, d.len, *now); }
        for (uint32_t i = 0; i < nh; i++) rudp_on_datagram(a, hold[i].data, hold[i].len, *now);
        *now += 10;
        rudp_pump(a, *now);
        rudp_pump(b, *now);
    }
}

static void test_pair(void) {
    printf("-- (B) RUDP against itself\n");
    wire_t *ab = calloc(1, sizeof(wire_t)), *ba = calloc(1, sizeof(wire_t));
    inbox_t ia, ib;
    memset(&ia, 0, sizeof(ia));
    memset(&ib, 0, sizeof(ib));
    rudp_peer_t a, b;
    rudp_peer_init(&a, 1, 1, 11, wire_send, ab, inbox_deliver, &ia);
    rudp_peer_init(&b, 2, 2, 22, wire_send, ba, inbox_deliver, &ib);
    uint64_t now = 1000;
    rudp_connect_all(&a, now);
    rudp_connect_all(&b, now);
    run_pair(&a, &b, ab, ba, &now, 20);
    CHECK(rudp_all_open(&a) && rudp_all_open(&b), "a simultaneous open establishes all three channels on both ends");

    /* One end late: b only starts once a's SYNs have been waiting. */
    wire_t *ab2 = calloc(1, sizeof(wire_t)), *ba2 = calloc(1, sizeof(wire_t));
    rudp_peer_t c, e;
    rudp_peer_init(&c, 1, 1, 33, wire_send, ab2, inbox_deliver, &ia);
    rudp_peer_init(&e, 2, 2, 44, wire_send, ba2, inbox_deliver, &ib);
    rudp_connect_all(&c, now);
    run_pair(&c, &e, ab2, ba2, &now, 5);
    rudp_connect_all(&e, now);
    run_pair(&c, &e, ab2, ba2, &now, 20);
    CHECK(rudp_all_open(&c) && rudp_all_open(&e), "an end that answers before initiating still ends up open");

    /* Forty reliable messages over a wire that drops every fourth datagram. */
    ab->drop_every = 4;
    ba->drop_every = 4;
    memset(&ib, 0, sizeof(ib));
    for (uint8_t i = 0; i < 40; i++) {
        uint8_t m[20];
        memset(m, i, sizeof(m));
        m[0] = i;
        rudp_write(&a, 1, m, sizeof(m), false, now);
        rudp_write(&a, 2, m, sizeof(m), true, now);
        run_pair(&a, &b, ab, ba, &now, 3);
    }
    /* librudp's clock: 1 s, doubling. A segment lost twice waits 1 + 2 s. */
    for (int k = 0; k < 6000; k++) {
        uint32_t left = 0;
        for (uint32_t q = 0; q < RUDP_SENDQ; q++) if (a.ch[0].sendq[q].used) left++;
        if (!left) break;
        run_pair(&a, &b, ab, ba, &now, 1);
    }
    bool once = true;
    for (int i = 0; i < 40; i++) if (ib.seen[i] != 1) once = false;
    CHECK(once, "every reliable message arrives exactly once despite loss and reordering");
    uint32_t queued = 0;
    for (uint32_t k = 0; k < RUDP_SENDQ; k++) if (a.ch[0].sendq[k].used) queued++;
    CHECK(queued == 0, "and every one of them has been acknowledged");
    CHECK(ib.count[2] > 20 && ib.count[2] < 40, "the unreliable channel delivers what arrives and retransmits nothing");

    ab->drop_every = ba->drop_every = 0;   /* an RST is sent once, like librudp's */
    rudp_close_all(&a);
    run_pair(&a, &b, ab, ba, &now, 3);
    CHECK(b.ch[0].state == RUDP_CLOSED, "an RST with the right connection id closes the far end");
    free(ab); free(ba); free(ab2); free(ba2);
}

/* ---- (C) Signaling ------------------------------------------------------- */

static void test_signaling(void) {
    printf("-- (C) RPCS3 signaling\n");
    wire_t *ab = calloc(1, sizeof(wire_t)), *ba = calloc(1, sizeof(wire_t));
    rpcs3_sig_t A, B;
    memset(&A, 0, sizeof(A));
    memset(&B, 0, sizeof(B));
    rpcs3_sig_init(&A, "stftest1", wire_send, ab);
    rpcs3_sig_init(&B, "m2hle_peer", wire_send, ba);
    uint32_t ipA = 0x0A32A8C0, ipB = 0x0B32A8C0;   /* 192.168.50.10 / .11 */
    uint64_t now = 5000000;
    rpcs3_sig_start(&A, "m2hle_peer", ipB, 3658);
    rpcs3_sig_pump(&A, now);

    dgram_t d;
    CHECK(wire_take(ab, &d) && d.len == 75, "a signaling datagram is 75 bytes");
    CHECK(d.data[0] == 0 && d.data[1] == 0 && d.data[2] == 1, "on vport 0, subset 1");
    CHECK(memcmp(d.data + 3, "SIGN", 4) == 0 && d.data[7] == 3 && d.data[8] == 0,
          "signature SIGN, version 3 little-endian");
    CHECK(d.data[3 + 24] == RPCS3_SIG_CONNECT, "the first packet is a CONNECT");
    CHECK(memcmp(d.data + 3 + 28, &ipB, 4) == 0 && d.data[3 + 32] == (3658 & 0xFF) && d.data[3 + 33] == (3658 >> 8),
          "it carries where it was sent: raw address octets, port little-endian");
    CHECK(strcmp((const char *)d.data + 3 + 34, "stftest1") == 0 && d.data[3 + 34 + 16] == 0,
          "and the sender's npid, NUL-padded to 16");
    uint64_t sent_ts = rpcs3_sig_get_u64(d.data + 3 + 8);

    /* B gets it, knows nothing of A yet, and must acknowledge it anyway. */
    rpcs3_sig_on_datagram(&B, ipA, 3658, d.data, d.len, now + 1000);
    CHECK(wire_take(ba, &d) && d.data[3 + 24] == RPCS3_SIG_CONNECT_ACK, "a CONNECT from a stranger is acknowledged");
    CHECK(rpcs3_sig_get_u64(d.data + 3 + 8) == sent_ts, "the CONNECT_ACK echoes the CONNECT's timestamp exactly");
    rpcs3_sig_on_datagram(&A, ipB, 3658, d.data, d.len, now + 2000);
    CHECK(wire_take(ab, &d) && d.data[3 + 24] == RPCS3_SIG_CONFIRM, "the CONNECT_ACK is answered with a CONFIRM");
    rpcs3_sig_peer_t *pa = rpcs3_sig_find(&A, "m2hle_peer");
    CHECK(pa && pa->active && rpcs3_sig_rtt_us(pa) == 2000, "A is connected and measured its round trip");
    rpcs3_sig_on_datagram(&B, ipA, 3658, d.data, d.len, now + 3000);
    rpcs3_sig_peer_t *pb = rpcs3_sig_find(&B, "stftest1");
    CHECK(pb && pb->peer_active, "B knows A has it as connected");

    /* RPCS3 moves a peer to the source of its handshake packets. */
    rpcs3_sig_start(&B, "stftest1", 0, 0);
    rpcs3_sig_pump(&B, now + 4000);
    CHECK(wire_take(ba, &d) && d.data[3 + 24] == RPCS3_SIG_CONNECT, "B connects too, to punch its own NAT");
    rpcs3_sig_on_datagram(&A, 0x0C32A8C0, 50000, d.data, d.len, now + 5000);
    CHECK(pa->ip == 0x0C32A8C0 && pa->port == 50000, "a CONNECT from a new address moves the peer there");

    /* A foreign npid with a bad character is dropped. */
    uint8_t bad[75];
    memcpy(bad, d.data, 75);
    bad[3 + 34] = '!';
    memset(ab, 0, sizeof(*ab));
    rpcs3_sig_on_datagram(&A, ipB, 3658, bad, 75, now + 6000);
    CHECK(ab->count == 0, "a packet whose npid RPCS3 would refuse is ignored");

    /* Leaving: FINISHED until acknowledged. */
    rpcs3_sig_finish(&A, "m2hle_peer");
    rpcs3_sig_pump(&A, now + 7000);
    CHECK(wire_take(ab, &d) && d.data[3 + 24] == RPCS3_SIG_FINISHED, "leaving sends FINISHED");
    rpcs3_sig_on_datagram(&B, ipA, 3658, d.data, d.len, now + 8000);
    CHECK(wire_take(ba, &d) && d.data[3 + 24] == RPCS3_SIG_FINISHED_ACK && pb->dead,
          "which is acknowledged, and the peer is gone");
    free(ab); free(ba);
}

/* A room of three with no network: us (16, the owner) and members 33 and 40. */
static void room_of_three(rpcn_session_t *s, ps3_link_t *L) {
    memset(s, 0, sizeof(*s));
    memset(L, 0, sizeof(*L));
    s->room_id = 1;
    s->stage = RPCN_STAGE_HOSTING;
    s->my_member_id = s->owner_id = 16;
    s->room_bin_len = PS3_ROOM_BIN_SIZE;
    s->room_bin[4] = 1;                          /* a Room Match */
    const uint16_t ids[2] = { 33, 40 };
    for (int i = 0; i < 2; i++) {
        s->peers[i].used = true;
        s->peers[i].member_id = ids[i];
        s->peers[i].team_id = 0xFF;
        s->peers[i].bin_len = PS3_MEMBER_BIN_SIZE;
    }
    L->session = s;
    L->team = 0xFF;
    L->hosting = true;
}

static void test_owner(void) {
    static rpcn_session_t s;
    static ps3_link_t L;

    /* Nobody asked for a side: the first two in line, by join order. */
    room_of_three(&s, &L);
    CHECK(ps3_owner_choose(&L) && ps3_be32(L.blob + 0x14) == 2 && ps3_be16(L.blob + 0x18) == 16
          && ps3_be16(L.blob + 0x1A) == 33, "with no entries the first two in line fight, 1P first");

    /* 40 asked for 2P: it gets 2P, and the first in line fills 1P. */
    room_of_three(&s, &L);
    s.peers[1].bin[0x1C] = 2;
    CHECK(ps3_owner_choose(&L) && ps3_be16(L.blob + 0x18) == 16 && ps3_be16(L.blob + 0x1A) == 40,
          "a 2P entry takes 2P, a filler takes the empty side");

    /* 40 asked for 1P and is last in line: it still takes 1P. */
    room_of_three(&s, &L);
    s.peers[0].team_id = 1;
    L.team = 2;
    s.peers[1].team_id = 3;
    s.peers[1].bin[0x1C] = 1;
    CHECK(ps3_owner_choose(&L) && ps3_be16(L.blob + 0x18) == 40 && ps3_be16(L.blob + 0x1A) == 33,
          "a 1P entry beats the line; the front of the line fills 2P");

    /* After 16 (1P) beat 33 (2P): 16 to the front asking for 1P again, 40
     * next, the loser 33 to the back. */
    room_of_three(&s, &L);
    L.fighter_count = 2; L.fighters[0] = 16; L.fighters[1] = 33;
    ps3_rotate(&L, 1);
    CHECK(L.team == 1 && L.me[0x1C] == 1 && (ps3_be32(L.me) & PS3_MFLAG_ROTATED),
          "the winner goes to the front, asks for its side again and marks bit 29");
    room_of_three(&s, &L);
    s.my_member_id = 40;                         /* the same result, as the waiting member sees it */
    s.peers[1].member_id = 16;
    L.fighter_count = 2; L.fighters[0] = 16; L.fighters[1] = 33;
    ps3_rotate(&L, 1);
    CHECK(L.team == 2 && L.me[0x1C] == 0, "a waiting member moves up behind the winner");
    room_of_three(&s, &L);
    s.my_member_id = 33;
    s.peers[0].member_id = 16;
    L.fighter_count = 2; L.fighters[0] = 16; L.fighters[1] = 33;
    L.me[0x1C] = 2;
    ps3_rotate(&L, 1);
    CHECK(L.team == 3 && L.me[0x1C] == 0, "the loser goes to the back and asks for nothing");

    /* A member that did not see the result reads it off the fighters. */
    room_of_three(&s, &L);
    L.fighter_count = 2; L.fighters[0] = 33; L.fighters[1] = 40;
    ps3_put32(s.peers[1].bin, PS3_MFLAG_ROTATED | PS3_MFLAG_IN_MATCH);   /* 40, 2P, asked for nothing */
    s.peers[1].team_id = 3;                                              /* ... at the back */
    CHECK(ps3_published_winner(&L) == 1, "2P went to the back asking for nothing: 1P won");
    s.peers[1].team_id = 1;
    s.peers[1].bin[0x1C] = 2;
    CHECK(ps3_published_winner(&L) == 2, "2P went to the front asking for 2P again: 2P won");

    /* The lockstep follows the room's match flags. */
    room_of_three(&s, &L);
    L.room_rtt_ms = 16;
    ps3_sio_start(&L, 0);
    CHECK(L.sio.small_every == 1 && L.sio.big_every == 60 && L.sio.init_delay == 2,
          "two fighters alone: every frame, 60-frame packets, delay 2 at 16 ms");
    L.match_flags = PS3_MATCH_SPECTATORS;
    ps3_sio_start(&L, 0);
    CHECK(L.sio.small_every == 2 && L.sio.big_every == 12 && L.sio.init_delay == 3,
          "with watchers: every 2nd frame, every 12th to them, one frame more delay");
    L.match_flags = PS3_MATCH_RELAY | PS3_MATCH_SPECTATORS;
    ps3_sio_start(&L, 0);
    CHECK(L.sio.small_every == 4 && L.sio.init_delay == 10, "relay mode: every 4th frame, delay 10");
}

/* The owner's phase machine, stepped by hand: the server's echo is simulated
 * by copying the owner's blob into the session, as RPCN's self-notification
 * would. A room of two: us (16) and 33. */
static void echo(rpcn_session_t *s, const ps3_link_t *L) {
    memcpy(s->room_bin, L->blob, PS3_ROOM_BIN_SIZE);
    s->room_bin_len = PS3_ROOM_BIN_SIZE;
}

static void test_owner_pump(void) {
    static rpcn_session_t s;
    static ps3_link_t L;
    room_of_three(&s, &L);
    s.peers[1].used = false;                     /* just 33 */
    L.max_slot = 2;
    L.blob[4] = 1;
    echo(&s, &L);
    uint64_t t = 1000000;

    ps3_owner_pump(&L, t);
    CHECK(ps3_be32(L.blob + 0x10) == PS3_PHASE_CHOOSING, "a full room of two starts choosing at once");
    ps3_owner_pump(&L, t + 1000);
    CHECK(ps3_be32(L.blob + 0x10) == PS3_PHASE_CHOOSING && s.room_bin[0x13] == PS3_PHASE_LOBBY,
          "no step until the server has echoed the last write");

    echo(&s, &L);
    ps3_owner_pump(&L, t + 2000);
    CHECK(ps3_be32(L.blob + 0x10) == PS3_PHASE_PREPARING && ps3_be16(L.blob + 0x18) == 16
          && ps3_be16(L.blob + 0x1A) == 33, "then preparing, with the two of us as 1P and 2P");

    echo(&s, &L);
    L.fighter_count = 2; L.fighters[0] = 16; L.fighters[1] = 33;   /* what ps3_read_room takes from the echo */
    ps3_owner_pump(&L, t + 3000);
    CHECK(!L.own_entrant, "our own entrant data waits for a link to the other fighter");
    ps3_owner_pump(&L, t + 3000 + PS3_LINK_WAIT_US + 1);
    CHECK(L.own_entrant && (ps3_be32(L.blob + 0x20) & PS3_SLOT_NO_LINK) && (ps3_be32(L.me) & PS3_MFLAG_READY),
          "or, after the wait, goes in marked as having none");

    /* 33's entrant data: refused from anyone else, and for the wrong side. */
    ps3_peer_t from;
    memset(&from, 0, sizeof(from));
    snprintf(from.npid, sizeof(from.npid), "m2hletest");
    uint8_t m[7 + PS3_ENTRANT_SIZE];
    memset(m, 0, sizeof(m));
    m[0] = PS3_MSG_UPDATE_SETTING;
    m[6] = 1;
    from.member_id = 40;
    ps3_on_message(&L, &from, 1, m, sizeof(m));
    CHECK(!(ps3_be32(L.blob + 0x84) & PS3_SLOT_FILLED), "a member who is not 2P cannot fill 2P's slot");
    from.member_id = 33;
    m[6] = 0;
    ps3_on_message(&L, &from, 1, m, sizeof(m));
    CHECK(ps3_be32(L.blob + 0x20) & PS3_SLOT_NO_LINK, "nor 2P fill 1P's");
    m[6] = 1;
    ps3_on_message(&L, &from, 1, m, sizeof(m));
    CHECK(ps3_be32(L.blob + 0x84) & PS3_SLOT_FILLED, "2P's own entrant data fills its slot");

    ps3_owner_pump(&L, t + 7000000);
    CHECK(ps3_be32(L.blob + 0x10) == PS3_PHASE_PREPARING, "not before 2P has also set ready");
    ps3_put32(s.peers[0].bin, PS3_MFLAG_READY);
    ps3_owner_pump(&L, t + 7001000);
    CHECK(ps3_be32(L.blob + 0x10) == PS3_PHASE_MATCH && L.blob[5] == PS3_MATCH_RELAY,
          "both ready: the match, in relay mode since we had no link");

    echo(&s, &L);
    ps3_owner_pump(&L, t + 8000000);
    CHECK(ps3_be32(L.blob + 0x10) == PS3_PHASE_MATCH, "a fighting owner waits for its own match");
    L.match_seen = true;
    L.match = false;
    ps3_owner_pump(&L, t + 9000000);
    CHECK(ps3_be32(L.blob + 0x10) == PS3_PHASE_RESULTS, "and moves to the results when its match is over");

    echo(&s, &L);
    ps3_put32(s.peers[0].bin, PS3_MFLAG_IN_MATCH | PS3_MFLAG_ROTATED);
    ps3_owner_pump(&L, t + 10000000);
    CHECK(ps3_be32(L.blob + 0x10) == PS3_PHASE_RESULTS, "results last while anyone is still marked in the match");
    ps3_put32(s.peers[0].bin, 0);
    ps3_owner_pump(&L, t + 11000000);
    CHECK(ps3_be32(L.blob + 0x10) == PS3_PHASE_LOBBY, "then back to the lobby");

    /* A fighter whose lockstep gave out still marks itself done, with its
     * place in line unchanged. */
    room_of_three(&s, &L);
    L.fighter_count = 2; L.fighters[0] = 16; L.fighters[1] = 33;
    L.team = 2;
    ps3_rotate(&L, 0);
    CHECK((ps3_be32(L.me) & PS3_MFLAG_ROTATED) && L.team == 2, "no result: bit 29 and the same place");
    /* ... which a member reading the fighters does not take for a result. */
    room_of_three(&s, &L);
    L.fighter_count = 2; L.fighters[0] = 33; L.fighters[1] = 40;
    s.peers[0].team_id = 2;
    s.peers[0].bin[0x1C] = 1;
    ps3_put32(s.peers[0].bin, PS3_MFLAG_ROTATED);
    CHECK(ps3_published_winner(&L) == 0, "a fighter at the same place asking for its side is not a winner");
    /* The common case: last match's winner, staying on at teamId 1 and asking
     * for its side again, whose lockstep gives out this match. */
    s.peers[0].team_id = 1;
    CHECK(ps3_published_winner(&L) == 1, "(without the mark, last match's winner looks like this match's)");
    s.peers[0].bin[PS3_ME_NO_RESULT] = 1;
    CHECK(ps3_published_winner(&L) == 0, "marked 'no result', last match's winner is not read as this one's");
    ps3_put32(s.peers[1].bin, PS3_MFLAG_ROTATED);   /* 40, 2P, at the back asking for nothing */
    s.peers[1].team_id = 2;
    CHECK(ps3_published_winner(&L) == 1, "the other fighter's real result still counts");
    s.peers[1].team_id = 1;
    s.peers[1].bin[0x1C] = 2;
    CHECK(ps3_published_winner(&L) == 2, "and says who won when the marked fighter lost");
    /* Our own no-result rotation marks us, and the next match clears it. */
    room_of_three(&s, &L);
    L.fighter_count = 2; L.fighters[0] = 16; L.fighters[1] = 33;
    L.team = 1; L.me[0x1C] = 1;
    ps3_rotate(&L, 0);
    CHECK(L.me[PS3_ME_NO_RESULT] == 1 && L.team == 1 && L.me[0x1C] == 1, "our no-result rotation is marked");
    s.room_bin_len = PS3_ROOM_BIN_SIZE;
    ps3_put32(s.room_bin + 0x10, PS3_PHASE_PREPARING);
    s.room_rev++;
    ps3_read_room(&L);
    ps3_put32(s.room_bin + 0x10, PS3_PHASE_MATCH);
    s.room_rev++;
    ps3_read_room(&L);
    CHECK(L.me[PS3_ME_NO_RESULT] == 0, "and the mark is gone when the next match starts");
    ps3_match_end(&L, "test");
}

/* The peer-to-peer port is bound before the TLS connect, and on Windows that
 * needs the socket library up first: with nothing else having started it, a
 * sign-in failed with "could not bind UDP (10093)". Nothing here has started
 * it, and the server does not exist, so the connect is what must fail. */
static void test_bind_order(void) {
    static rpcn_session_t s;
    memset(&s, 0, sizeof(s));
    /* A port nothing holds, found with the library up and then let go of. */
    uint16_t port = 0;
    net_startup();
    for (uint16_t p = 3747; p < 3800 && !port; p++) {
        net_sock_t k = NET_SOCK_INVALID;
        if (net_udp_open(&k, p)) { net_close(&k); port = p; }
    }
    net_shutdown_lib();
    CHECK(port != 0 && g_net_refs == 0, "a free port, and nothing holds the socket library");
    rpcn_session_config_t c;
    memset(&c, 0, sizeof(c));
    c.server = "127.0.0.1";
    c.port = 1;
    c.npid = "nobody";
    c.password = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    c.token = "";
    c.com_id = "NPWR03869_00";
    c.local_p2p_port = port;
    c.ps3 = true;
    bool ok = rpcn_session_start(&s, &c);
    const char *err = rpcn_session_error(&s);
    printf("      (%s)\n", err);
    CHECK(!ok && s.stage == RPCN_STAGE_FAILED && !strstr(err, "could not bind"),
          "a first sign-in gets as far as the connect: the socket library is up for the bind");
    CHECK(s.net_held && g_net_refs == 1, "the session holds its one reference on the library");
    rpcn_session_stop(&s);
    CHECK(!s.net_held && g_net_refs == 0, "and gives it back when it stops");
    rpcn_session_stop(&s);
    CHECK(g_net_refs == 0, "a second stop gives back nothing more");
}

int main(void) {
    test_capture();
    test_pair();
    test_signaling();
    test_owner();
    test_owner_pump();
    test_bind_order();
    printf(g_fail ? "\n%d FAILED\n" : "\nall passed\n", g_fail);
    return g_fail ? 1 : 0;
}
