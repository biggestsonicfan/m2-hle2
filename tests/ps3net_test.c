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
 */
#define NDEBUG 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void) {
    test_capture();
    test_pair();
    test_signaling();
    printf(g_fail ? "\n%d FAILED\n" : "\nall passed\n", g_fail);
    return g_fail ? 1 : 0;
}
