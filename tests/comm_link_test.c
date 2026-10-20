/*
 * comm_link_test.c — a linked comm board's datagrams (src/net/comm_link.h,
 * Pinboard #964): the window's packing, newest-wins on copies and late
 * datagrams, two m2comm.h nodes over a link that loses, repeats and reorders,
 * the same two over real UDP sockets on 127.0.0.1, and the board linked to the
 * phantom cabinet over UDP (comm_link_loop.h). No ROM.
 */
#include <stdio.h>
#include <stdlib.h>

#include "comm_link_loop.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

#define WIN 0x700u

static uint8_t g_a[COMM_LINK_MSG_MAX], g_b[COMM_LINK_MSG_MAX], g_p[COMM_LINK_DGRAM_MAX * 2];

static bool round_trips(const uint8_t *src, uint32_t n) {
    uint32_t p = comm_link_pack(src, n, g_p, sizeof g_p);
    return p && comm_link_unpack(g_p, p, g_b, n) == n && memcmp(src, g_b, n) == 0;
}

/* A window like Virtual-On's: a frame type, a short header, a mostly zero body. */
static void von_like(uint8_t *m, uint32_t seed) {
    memset(m, 0, 1u + WIN);
    m[0] = 1;
    for (uint32_t i = 1; i < 48; i++) m[i] = (uint8_t)(seed * 31u + i * 13u);
    for (uint32_t i = 0x300; i < 0x308; i++) m[i] = (uint8_t)(seed + i);
    m[0x557] = 0x5E; m[0x558] = 0xAE;
}

static void test_pack(void) {
    von_like(g_a, 7);
    uint32_t p = comm_link_pack(g_a, 1u + WIN, g_p, COMM_LINK_PACKED_MAX);
    CHECK(round_trips(g_a, 1u + WIN) && p > 0 && p < 160u, "a mostly zero window packs small and comes back");
    for (uint32_t i = 0; i < 1u + WIN; i++) g_a[i] = (uint8_t)(i * 7u + (i >> 3));
    CHECK(round_trips(g_a, 1u + WIN), "a window with no runs comes back");
    srand(964);
    for (uint32_t i = 0; i < 1u + WIN; i++) g_a[i] = (uint8_t)rand();
    CHECK(comm_link_pack(g_a, 1u + WIN, g_p, COMM_LINK_PACKED_MAX) == 0, "noise does not fit a datagram: pack says so");
    for (uint32_t i = 0; i < 300; i++) g_a[i] = (uint8_t)(i < 130 ? 9 : i < 131 ? 1 : i < 134 ? 2 : i);
    CHECK(round_trips(g_a, 300) && round_trips(g_a, 1) && round_trips(g_a, 2) && round_trips(g_a, 131),
          "runs at the edges of a token come back");
    uint32_t q = comm_link_pack(g_a, 300, g_p, sizeof g_p);
    CHECK(comm_link_unpack(g_p, q - 1u, g_b, 300) == 0 && comm_link_unpack(g_p, q, g_b, 299) == 0,
          "torn or wrong-length input is refused");
}

/* ---- a stand-in socket: datagrams kept, and handed over lost/doubled/swapped ---- */

typedef struct { uint8_t d[COMM_LINK_DGRAM_MAX]; uint32_t len; } dgram_t;
typedef struct { dgram_t q[64]; uint32_t n, count; comm_link_t *to; } wire_t;

static wire_t g_wa, g_wb;   /* A -> B, B -> A */

static void wire_transmit(void *ud, const void *buf, uint32_t len) {
    wire_t *w = (wire_t *)ud;
    if (w->n < 64) { memcpy(w->q[w->n].d, buf, len); w->q[w->n++].len = len; }
}

/* Hand over what is waiting: every `lose`th datagram lost, then the rest in
 * reverse order (the copies of a message, and messages, swapped). */
static void wire_deliver(wire_t *w, uint32_t lose) {
    for (uint32_t i = w->n; i-- > 0;) {
        if (lose && ++w->count % lose == 0) continue;
        comm_link_on_datagram(w->to, w->q[i].d, w->q[i].len);
    }
    w->n = 0;
}

/* ---- one link's datagrams: a sends, b takes ---- */

static comm_link_t g_la, g_lb;
static uint8_t     g_m[4] = { 0xFF, 1, 0, 0 }, g_got[8];
static dgram_t     g_first;

static uint32_t take(void) { return comm_link_recv(&g_lb, g_got, sizeof g_got); }

static void test_copies(void) {
    lockstep_header_t h = { 0 };
    comm_link_start(&g_la, 1, &h, wire_transmit, &g_wa);
    comm_link_start(&g_lb, 2, &h, wire_transmit, &g_wb);
    g_wa.to = &g_lb; g_wa.n = 0;
    comm_link_send(&g_la, g_m, 3);
    CHECK(g_wa.n == COMM_LINK_COPIES && g_wa.q[0].d[0] == LOCKSTEP_PACKET_COMM, "a message goes out three times, typed COMM");
    g_first = g_wa.q[0];
    wire_deliver(&g_wa, 0);
    CHECK(take() == 3 && memcmp(g_got, g_m, 3) == 0 && take() == 0 && g_lb.stale == 2,
          "the peer takes it once; the copies are stale");
}

static void test_late(void) {
    g_m[1] = 2; comm_link_send(&g_la, g_m, 3);
    g_m[1] = 3; comm_link_send(&g_la, g_m, 3);
    wire_deliver(&g_wa, 0);                               /* reversed: seq 3 before seq 2 */
    bool newer = take() == 3 && g_got[1] == 3;
    bool late = take() == 3 && g_got[1] == 2;
    CHECK(newer && late && take() == 0, "one a little late is still taken, once");
    comm_link_on_datagram(&g_lb, g_first.d, g_first.len);
    CHECK(take() == 0, "a late copy of the first is dropped");
}

static void test_window_edge(void) {
    uint32_t taken0 = g_lb.taken, stale0 = g_lb.stale;
    dgram_t edge[3];
    for (uint32_t k = 0; k <= COMM_LINK_WINDOW; k++) {    /* seqs 4..36, lost; three kept */
        comm_link_send(&g_la, g_m, 3);
        if (k < 3) edge[k] = g_wa.q[0];
        g_wa.n = 0;
    }
    comm_link_send(&g_la, g_m, 3);                        /* seq 37 gets there: 4 and 5 are 33 and 32 behind it, 6 is 31 */
    comm_link_on_datagram(&g_lb, g_wa.q[0].d, g_wa.q[0].len);
    g_wa.n = 0;
    for (int k = 0; k < 3; k++) comm_link_on_datagram(&g_lb, edge[k].d, edge[k].len);
    while (take()) {}
    CHECK(g_lb.taken - taken0 == 2 && g_lb.stale - stale0 == 2,
          "one a window behind the newest is dropped, one just inside is taken");
}

static void test_epoch_torn_overflow(void) {
    lockstep_header_t h = { 0 };
    comm_link_start(&g_la, 9, &h, wire_transmit, &g_wa); /* the peer's board started again */
    g_m[1] = 4; comm_link_send(&g_la, g_m, 3);
    wire_deliver(&g_wa, 0);
    CHECK(take() == 3 && g_got[1] == 4, "a new epoch starts the count again");
    g_first.d[sizeof(comm_link_head_t)] = 0x7F;           /* a literal run past the end */
    uint32_t torn = g_lb.torn;
    comm_link_on_datagram(&g_lb, g_first.d, g_first.len);
    CHECK(g_lb.torn == torn + 1 && take() == 0, "a torn datagram is dropped");
    for (uint8_t k = 0; k < COMM_LINK_INBOX + 3; k++) { g_m[1] = (uint8_t)(10 + k); comm_link_send(&g_la, g_m, 3); }
    for (uint32_t i = 0; i < g_wa.n; i++) comm_link_on_datagram(&g_lb, g_wa.q[i].d, g_wa.q[i].len);
    g_wa.n = 0;
    take();
    CHECK(g_lb.overflow == 3 && g_got[1] == 13, "a full inbox loses its oldest");
}

/* ---- two boards over the stand-in ---- */

static m2comm_node_t A, B;
static comm_link_t   LA, LB;

static void lossy_passes(int n, uint32_t lose) {
    for (int i = 0; i < n; i++) {
        m2comm__pass(&A.s, &A.l); wire_deliver(&g_wa, lose);
        m2comm__pass(&B.s, &B.l); wire_deliver(&g_wb, lose);
    }
}

static void fill(m2comm_state_t *s, uint8_t seed) {
    for (uint32_t i = 0; i < WIN; i++) s->shared[M2COMM_FRAME_START + i] = (uint8_t)(i < 64 ? seed + i * 7u : 0);
}

static bool holds(const m2comm_state_t *s, uint8_t seed) {
    for (uint32_t i = 0; i < WIN; i++)
        if (s->shared[M2COMM_FRAME_START + WIN + i] != (uint8_t)(i < 64 ? seed + i * 7u : 0)) return false;
    return true;
}

static bool ring_says(const m2comm_state_t *s, uint8_t id) {
    return s->shared[0] == 1 && s->shared[2] == id && s->shared[3] == 2;
}

static void link_nodes(m2comm_node_t *n, comm_link_t *l, uint16_t epoch, wire_t *out, comm_link_t *other_in) {
    lockstep_header_t h = { 0 };
    comm_link_start(l, epoch, &h, wire_transmit, out);
    out->to = other_in; out->n = 0; out->count = 0;
    n->peer = (m2comm_peer_t){ l, comm_link_send, comm_link_recv };
    n->l.peer = &n->peer;
}

/* Every second datagram lost: with three copies each message still gets there. */
static void test_lossy_link(void) {
    m2comm_node_start(&A, 1, WIN);
    m2comm_node_start(&B, 2, WIN);
    link_nodes(&A, &LA, 0xA, &g_wa, &LB);
    link_nodes(&B, &LB, 0xB, &g_wb, &LA);
    lossy_passes(M2COMM_LINK_DELAY + 4, 2);
    CHECK(ring_says(&A.s, 1) && ring_says(&B.s, 2), "lossy link: the ring comes up, node 1 and node 2 of 2");
    fill(&A.s, 0x11); fill(&B.s, 0x80);
    lossy_passes(3, 2);
    CHECK(holds(&B.s, 0x11) && holds(&A.s, 0x80), "lossy link: each window lands raw in the other's 0x2700");
    CHECK(LA.too_big == 0 && LB.too_big == 0 && LA.packed_peak < 200u, "lossy link: windows pack small");
}

/* Nothing gets through for a while: the ring stays up and frames come again. */
static void test_outage(void) {
    lossy_passes(30, 1);
    fill(&A.s, 0x42); fill(&B.s, 0x24);
    lossy_passes(3, 0);
    CHECK(ring_says(&A.s, 1) && ring_says(&B.s, 2) && holds(&B.s, 0x42) && holds(&A.s, 0x24),
          "after an outage the next windows land");
}

/* ---- the same over real UDP ---- */

static comm_udp_t UA, UB;

static void udp_passes(int n) {
    for (int i = 0; i < n; i++) {
        m2comm__pass(&A.s, &A.l); comm_udp_drain(&UB);
        m2comm__pass(&B.s, &B.l); comm_udp_drain(&UA);
    }
}

static void test_udp(void) {
    bool up = comm_udp_pair(&UA, &UB);
    CHECK(up, "udp: two sockets on 127.0.0.1");
    if (!up) return;
    m2comm_node_start(&A, 1, WIN);
    m2comm_node_start(&B, 2, WIN);
    A.l.peer = &UA.peer; B.l.peer = &UB.peer;
    udp_passes(M2COMM_LINK_DELAY + 4);
    CHECK(ring_says(&A.s, 1) && ring_says(&B.s, 2), "udp: the ring comes up");
    fill(&A.s, 0x5A); fill(&B.s, 0xA5);
    udp_passes(3);
    CHECK(holds(&B.s, 0x5A) && holds(&A.s, 0xA5), "udp: each window lands raw in the other's");
    printf("      udp: %u datagrams, %u bytes; largest packed message %u bytes\n",
           (unsigned)UA.wire_dgrams, (unsigned)UA.wire_bytes, (unsigned)UA.link.packed_peak);
    comm_udp_close(&UA); comm_udp_close(&UB);
}

/* The board's own g_m2comm, as the ROM drives it, linked to the phantom over UDP. */
static void test_udp_phantom(void) {
    memset(&g_m2comm, 0, sizeof g_m2comm);
    g_m2comm.frame_offset = 0x1C0u;
    bool up = comm_udp_phantom_attach(WIN);
    CHECK(up, "udp phantom: attached");
    if (!up) return;
    m2comm__cn_w(1);
    g_m2comm.shared[1] = 1;
    g_m2comm.shared[0x14] = (uint8_t)WIN; g_m2comm.shared[0x15] = (uint8_t)(WIN >> 8);
    for (int i = 0; i < 400 && g_m2comm.shared[0] != 1; i++) m2comm__run(1);
    CHECK(ring_says(&g_m2comm, 1), "udp phantom: the board's ring comes up, node 1 of 2");
    fill(&g_m2comm, 0x5A);
    m2comm__run(1); m2comm__run(1);
    CHECK(holds(&g_m2comm, 0x5A), "udp phantom: the board's window comes back to it over the wire");
    uint32_t big = g_comm_udp_board.link.too_big;
    for (uint32_t i = 0; i < WIN; i++) g_m2comm.shared[M2COMM_FRAME_START + i] = (uint8_t)(i ^ 0x5A);
    m2comm__run(1); m2comm__run(1);
    CHECK(g_comm_udp_board.link.too_big > big && holds(&g_m2comm, 0x5A),
          "udp phantom: a window too big for a datagram is not sent; the last one stays");
    m2comm_set_peer(NULL);
    comm_udp_close(&g_comm_udp_board); comm_udp_close(&g_comm_udp_phantom);
}

int main(void) {
    test_pack();
    test_copies();
    test_late();
    test_window_edge();
    test_epoch_torn_overflow();
    test_lossy_link();
    test_outage();
    test_udp();
    test_udp_phantom();
    printf("%s\n", g_fail ? "SOME CHECKS FAILED" : "all checks passed");
    return g_fail ? 1 : 0;
}
