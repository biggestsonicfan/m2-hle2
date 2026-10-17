/*
 * m2comm_test.c — the comm board's link (board/m2comm.h, Pinboard #959): two
 * nodes in one process, joined by datagram queues (m2comm_loop.h). The ring
 * comes up with MAME's ids and count, each node's 0x700-byte window lands raw
 * in the other's, one data frame is taken per pass, a lost token is sent
 * again, and a board with nobody on the link reads as it always did. No ROM.
 */
#include <stdio.h>

#include "m2comm_loop.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

#define WIN 0x700u

static m2comm_node_t A, B;

static void passes(int n) {
    for (int i = 0; i < n; i++) { m2comm__pass(&A.s, &A.l); m2comm__pass(&B.s, &B.l); }
}

static void fill(m2comm_node_t *n, uint8_t seed) {
    for (uint32_t i = 0; i < WIN; i++) n->s.shared[M2COMM_FRAME_START + i] = (uint8_t)(seed + i * 7u);
}

static bool holds(const m2comm_node_t *n, uint8_t seed) {
    for (uint32_t i = 0; i < WIN; i++)
        if (n->s.shared[M2COMM_FRAME_START + WIN + i] != (uint8_t)(seed + i * 7u)) return false;
    return true;
}

static bool ring_says(const m2comm_node_t *n, uint8_t id) {
    return n->s.shared[0] == 1 && n->s.shared[2] == id && n->s.shared[3] == 2;
}

static void test_ring_up(void) {
    m2comm_node_start(&A, 1, WIN);
    m2comm_node_start(&B, 2, WIN);
    m2comm_node_pair(&A, &B);
    passes(M2COMM_LINK_DELAY - 2);
    CHECK(A.s.shared[0] == 0 && A.s.shared[2] == 0xFF && B.s.shared[0] == 0,
          "the ring is down while the master waits");
    passes(4);
    CHECK(ring_says(&A, 1), "master: ring up, node 1 of 2");
    CHECK(ring_says(&B, 2), "slave: ring up, node 2 of 2");
}

static void test_windows(void) {
    fill(&A, 0x11);
    fill(&B, 0x80);
    passes(2);
    CHECK(holds(&B, 0x11), "the master's window lands at the slave's 0x2700");
    CHECK(holds(&A, 0x80), "the slave's window lands at the master's 0x2700");
    CHECK(A.l.taken > 0 && B.l.taken > 0 && A.in.dropped == 0 && B.in.dropped == 0,
          "both take frames, nothing dropped");
}

/* Three master frames queued for the slave: one per pass, in order. */
static void test_one_per_pass(void) {
    uint8_t seeds[3] = { 0x21, 0x42, 0x63 };
    for (int k = 0; k < 3; k++) { fill(&A, seeds[k]); m2comm__send_data(&A.s, &A.l); }
    bool in_order = true;
    for (int k = 0; k < 3; k++) {
        uint8_t z = B.s.zfg;
        m2comm__pass(&B.s, &B.l);
        in_order = in_order && holds(&B, seeds[k]) && B.s.zfg != z;
    }
    CHECK(in_order, "queued frames are taken one per pass, in order, each toggling zfg");
}

/* The slave's first answer to the token is lost: the master asks again. */
static void test_lost_token(void) {
    m2comm_node_start(&A, 1, WIN);
    m2comm_node_start(&B, 2, WIN);
    memset(&A.in, 0, sizeof A.in); memset(&B.in, 0, sizeof B.in);
    for (int i = 0; i < (int)M2COMM_LINK_DELAY + 2; i++) {
        m2comm__pass(&A.s, &A.l);
        memset(&B.in, 0, sizeof B.in);                 /* everything to the slave is lost */
    }
    CHECK(A.s.shared[0] == 0, "no answer: the master's ring stays down");
    passes(M2COMM_RESEND + 2);
    CHECK(ring_says(&A, 1) && ring_says(&B, 2), "the token again: the ring comes up");
}

/* The ring size is lost: the slave stays down until the master repeats it. */
static void test_lost_size(void) {
    m2comm_node_start(&A, 1, WIN);
    m2comm_node_start(&B, 2, WIN);
    memset(&A.in, 0, sizeof A.in); memset(&B.in, 0, sizeof B.in);
    for (int i = 0; i < 400; i++) {
        m2comm__pass(&A.s, &A.l);
        if (A.s.shared[0] == 1) break;
        m2comm__pass(&B.s, &B.l);
    }
    memset(&B.in, 0, sizeof B.in);                     /* the 0xFE is lost */
    passes(2);
    CHECK(B.s.shared[0] == 0, "the size lost: the slave's ring is down");
    passes(M2COMM_RESEND);
    fill(&A, 0x33);
    passes(2);
    CHECK(ring_says(&B, 2) && holds(&B, 0x33), "the size again: the slave is up and taking frames");
}

/* No peer: the board's passes are the ones it always made. */
static void test_alone(void) {
    memset(&g_m2comm, 0, sizeof g_m2comm);
    m2comm_set_peer(NULL);
    g_m2comm.frame_offset = 0x1C0u;
    m2comm__cn_w(1);
    CHECK(g_m2comm.zfg == 1 && g_m2comm.shared[0] == 0 && g_m2comm.shared[2] == 0xFF
          && g_m2comm.shared[3] == 0xFF && g_m2comm.shared[1] == 2 && g_m2comm.shared[0x14] == 0xC0,
          "alone: a start makes one pass, link not up, the slave's reset values");
    m2comm__run(3);
    CHECK(g_m2comm.zfg == 0, "alone: zfg toggles once a pass");
}

/* A board linked to the phantom: its window comes back to it. */
static void test_phantom(void) {
    memset(&g_m2comm, 0, sizeof g_m2comm);
    g_m2comm.frame_offset = 0x1C0u;
    m2comm_phantom_attach(WIN);
    m2comm__cn_w(1);
    g_m2comm.shared[1] = 1;                            /* the ROM: MASTER */
    g_m2comm.shared[0x14] = (uint8_t)WIN; g_m2comm.shared[0x15] = (uint8_t)(WIN >> 8);
    m2comm__run(M2COMM_LINK_DELAY + 2);
    for (int i = 0; i < 300 && g_m2comm.shared[0] != 1; i++) m2comm__run(1);
    CHECK(g_m2comm.shared[0] == 1 && g_m2comm.shared[2] == 1 && g_m2comm.shared[3] == 2,
          "phantom: the board's ring comes up, node 1 of 2");
    for (uint32_t i = 0; i < WIN; i++) g_m2comm.shared[M2COMM_FRAME_START + i] = (uint8_t)(i ^ 0x5A);
    m2comm__run(1); m2comm__run(1);
    bool back = true;
    for (uint32_t i = 0; i < WIN; i++)
        back = back && g_m2comm.shared[M2COMM_FRAME_START + WIN + i] == (uint8_t)(i ^ 0x5A);
    CHECK(back, "phantom: the board's window comes back to it");
    m2comm_set_peer(NULL);
}

int main(void) {
    test_ring_up();
    test_windows();
    test_one_per_pass();
    test_lost_token();
    test_lost_size();
    test_alone();
    test_phantom();
    printf("%s\n", g_fail ? "SOME CHECKS FAILED" : "all checks passed");
    return g_fail ? 1 : 0;
}
