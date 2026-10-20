/*
 * m2comm_loop.h — the other end of a comm board's link (m2comm.h), in the same
 * process, for the tests (Pinboard #959).
 *
 *   m2comm_q_t        a datagram queue: one board's inbox
 *   m2comm_node_t     a whole node: its board state, link and inbox, and a peer
 *                     that sends into another node's inbox
 *   m2comm_phantom_*  a second cabinet for the real board (g_m2comm) to link
 *                     with: a slave (or, for a board that is the slave, a
 *                     master, #969) whose window is the last one it took, so
 *                     the board gets its own packets back. Virtual-On's check
 *                     on a packet is self-contained ([+0x556] == [+2] ^ 0xAE5E),
 *                     so its ROM takes them.
 */
#ifndef M2COMM_LOOP_H
#define M2COMM_LOOP_H

#include "m2comm.h"

#define M2COMM_Q_LEN 16

typedef struct {
    uint8_t  msg[M2COMM_Q_LEN][M2COMM_MSG_MAX];
    uint32_t len[M2COMM_Q_LEN];
    uint32_t head, tail, dropped;
} m2comm_q_t;

static void m2comm_q_put(m2comm_q_t *q, const uint8_t *msg, uint32_t len) {
    if (q->tail - q->head == M2COMM_Q_LEN || len > M2COMM_MSG_MAX) { q->dropped++; return; }
    memcpy(q->msg[q->tail % M2COMM_Q_LEN], msg, len);
    q->len[q->tail++ % M2COMM_Q_LEN] = len;
}

static uint32_t m2comm_q_get(m2comm_q_t *q, uint8_t *msg, uint32_t cap) {
    if (q->head == q->tail) return 0;
    uint32_t len = q->len[q->head % M2COMM_Q_LEN];
    if (len > cap) len = cap;
    memcpy(msg, q->msg[q->head++ % M2COMM_Q_LEN], len);
    return len;
}

typedef struct m2comm_node {
    m2comm_state_t s;
    m2comm_link_t  l;
    m2comm_q_t     in;
    m2comm_peer_t  peer;
    struct m2comm_node *other;
} m2comm_node_t;

static void m2comm_node__send(void *ctx, const uint8_t *msg, uint32_t len) {
    m2comm_q_put(&((m2comm_node_t *)ctx)->other->in, msg, len);
}

static uint32_t m2comm_node__recv(void *ctx, uint8_t *msg, uint32_t cap) {
    return m2comm_q_get(&((m2comm_node_t *)ctx)->in, msg, cap);
}

/* A started board (CN 1) whose ROM said role (1 master, 2 slave) and window. */
static void m2comm_node_start(m2comm_node_t *n, uint8_t role, uint32_t window) {
    memset(&n->s, 0, sizeof n->s);
    n->s.enabled = n->s.cn = 1;
    n->s.shared[0x01] = role;
    n->s.shared[0x13] = 0x0E;
    n->s.shared[0x14] = (uint8_t)window;
    n->s.shared[0x15] = (uint8_t)(window >> 8);
    m2comm__link_reset(&n->l);
}

static void m2comm_node_pair(m2comm_node_t *a, m2comm_node_t *b) {
    a->other = b; b->other = a;
    a->peer = (m2comm_peer_t){ a, m2comm_node__send, m2comm_node__recv };
    b->peer = (m2comm_peer_t){ b, m2comm_node__send, m2comm_node__recv };
    a->l.peer = &a->peer; b->l.peer = &b->peer;
}

/* ---- the phantom cabinet linked to g_m2comm ---- */

static m2comm_node_t g_m2comm_phantom;
static m2comm_q_t    g_m2comm_board_in;
static bool          g_m2comm_phantom_master;   /* its own clock: a pass each board frame */

static void m2comm_phantom__to_board(void *ctx, const uint8_t *msg, uint32_t len) {
    (void)ctx;
    m2comm_node_t *p = &g_m2comm_phantom;
    uint32_t n = m2comm__window(&p->s);
    if (len == 1u + n && msg[0] <= p->l.count) {           /* a data frame: echo what it took */
        uint8_t echo[M2COMM_MSG_MAX];
        echo[0] = msg[0];
        memcpy(echo + 1, p->s.shared + M2COMM_FRAME_START + n, n);
        m2comm_q_put(&g_m2comm_board_in, echo, len);
        return;
    }
    m2comm_q_put(&g_m2comm_board_in, msg, len);
}

/* The board sends: a slave phantom makes its pass on it at once. A master
 * keeps its own clock (m2comm_phantom_frame), as the board's Z80 does. */
static void m2comm_phantom__from_board(void *ctx, const uint8_t *msg, uint32_t len) {
    (void)ctx;
    m2comm_q_put(&g_m2comm_phantom.in, msg, len);
    if (!g_m2comm_phantom_master) m2comm__pass(&g_m2comm_phantom.s, &g_m2comm_phantom.l);
}

/* A board frame: a master phantom's pass. */
static void m2comm_phantom_frame(void) {
    if (g_m2comm_phantom_master) m2comm__pass(&g_m2comm_phantom.s, &g_m2comm_phantom.l);
}

static uint32_t m2comm_phantom__board_recv(void *ctx, uint8_t *msg, uint32_t cap) {
    (void)ctx;
    return m2comm_q_get(&g_m2comm_board_in, msg, cap);
}

static uint32_t m2comm_phantom__board_pending(void *ctx) {
    (void)ctx;
    return g_m2comm_board_in.tail - g_m2comm_board_in.head;
}

static const m2comm_peer_t m2comm_phantom_board_peer = {
    NULL, m2comm_phantom__from_board, m2comm_phantom__board_recv, m2comm_phantom__board_pending };

/* A phantom that is `role` (1 master, 2 slave); the board's end of the link,
 * for the caller to plug in (m2comm_set_peer, or net/comm_role.h's reset). */
static const m2comm_peer_t *m2comm_phantom_open(uint8_t role, uint32_t window) {
    memset(&g_m2comm_phantom, 0, sizeof g_m2comm_phantom);
    memset(&g_m2comm_board_in, 0, sizeof g_m2comm_board_in);
    g_m2comm_phantom_master = role == 1;
    m2comm_node_start(&g_m2comm_phantom, role, window);
    g_m2comm_phantom.peer = (m2comm_peer_t){ NULL, m2comm_phantom__to_board, m2comm_node__recv };
    g_m2comm_phantom.peer.ctx = &g_m2comm_phantom;
    g_m2comm_phantom.l.peer = &g_m2comm_phantom.peer;
    return &m2comm_phantom_board_peer;
}

/* Link the board (as master: its ROM's role must say so) to a phantom slave. */
static void m2comm_phantom_attach(uint32_t window) {
    m2comm_set_peer(m2comm_phantom_open(2, window));
}

#endif /* M2COMM_LOOP_H */
