/*
 * comm_link_loop.h — a comm board's link over real datagrams, in one process,
 * for the tests (Pinboard #964).
 *
 *   comm_udp_t           one end: a UDP socket on 127.0.0.1 and a comm_link
 *                        (src/net/comm_link.h) whose transport sends to the
 *                        other end's port, as netplay.h's sends to the opponent
 *                        over the room's socket
 *   comm_udp_phantom_*   m2comm_loop.h's phantom cabinet, reached over such a
 *                        pair: the real board (g_m2comm) sends through one end,
 *                        the phantom (a slave, or a master) answers through
 *                        the other
 */
#ifndef COMM_LINK_LOOP_H
#define COMM_LINK_LOOP_H

#include "net_socket.h"
#include "comm_link.h"
#include "m2comm_loop.h"

typedef struct {
    net_sock_t    sock;
    uint16_t      port, to_port;
    comm_link_t   link;
    m2comm_peer_t peer;
    uint32_t      wire_bytes, wire_dgrams;
} comm_udp_t;

static void comm_udp__transmit(void *ud, const void *buf, uint32_t len) {
    comm_udp_t *u = (comm_udp_t *)ud;
    if (net_udp_send(u->sock, htonl(INADDR_LOOPBACK), u->to_port, buf, len)) {
        u->wire_bytes += len;
        u->wire_dgrams++;
    }
}

/* What has come in, into the link's inbox (netplay_drain_socket's part). */
static void comm_udp_drain(comm_udp_t *u) {
    uint8_t buf[COMM_LINK_DGRAM_MAX];
    uint32_t ip; uint16_t port;
    int got;
    while ((got = net_udp_recv(u->sock, buf, sizeof buf, &ip, &port)) > 0)
        if ((uint32_t)got >= sizeof(lockstep_header_t) && buf[0] == LOCKSTEP_PACKET_COMM)
            comm_link_on_datagram(&u->link, buf, (uint32_t)got);
}

static uint16_t comm_udp__port(net_sock_t s) {
    struct sockaddr_in a;
#ifdef _WIN32
    int n = (int)sizeof a;
#else
    socklen_t n = sizeof a;
#endif
    return getsockname(s, (struct sockaddr *)&a, &n) == 0 ? ntohs(a.sin_port) : 0;
}

/* Two ends on ports the OS picks, each sending to the other. */
static bool comm_udp_pair(comm_udp_t *a, comm_udp_t *b) {
    if (!net_startup() || !net_udp_open(&a->sock, 0) || !net_udp_open(&b->sock, 0)) return false;
    a->port = comm_udp__port(a->sock);
    b->port = comm_udp__port(b->sock);
    a->to_port = b->port;
    b->to_port = a->port;
    lockstep_header_t h = { 0 };
    comm_link_start(&a->link, 0x1111u, &h, comm_udp__transmit, a);
    comm_link_start(&b->link, 0x2222u, &h, comm_udp__transmit, b);
    a->peer = (m2comm_peer_t){ &a->link, comm_link_send, comm_link_recv };
    b->peer = (m2comm_peer_t){ &b->link, comm_link_send, comm_link_recv };
    return a->port && b->port;
}

static void comm_udp_close(comm_udp_t *u) { net_close(&u->sock); }

/* ---- the phantom cabinet over UDP ---- */

static comm_udp_t g_comm_udp_board, g_comm_udp_phantom;

/* The phantom sends: a data frame carries back the window it took. */
static void comm_udp_phantom__send(void *ctx, const uint8_t *msg, uint32_t len) {
    (void)ctx;
    m2comm_node_t *p = &g_m2comm_phantom;
    uint32_t n = m2comm__window(&p->s);
    if (len == 1u + n && msg[0] <= p->l.count) {
        uint8_t echo[M2COMM_MSG_MAX];
        echo[0] = msg[0];
        memcpy(echo + 1, p->s.shared + M2COMM_FRAME_START + n, n);
        comm_link_send(&g_comm_udp_phantom.link, echo, len);
        return;
    }
    comm_link_send(&g_comm_udp_phantom.link, msg, len);
}

static m2comm_peer_t g_comm_udp_phantom_peer = { NULL, comm_udp_phantom__send, comm_link_recv };

/* The board sends over the wire; the phantom drains its end and makes a pass
 * for each message that came, as m2comm_loop.h's makes one per send. */
static void comm_udp_phantom__from_board(void *ctx, const uint8_t *msg, uint32_t len) {
    (void)ctx;
    comm_link_send(&g_comm_udp_board.link, msg, len);
    if (g_m2comm_phantom_master) return;   /* it passes on its own clock */
    comm_udp_drain(&g_comm_udp_phantom);
    while (g_comm_udp_phantom.link.head != g_comm_udp_phantom.link.tail)
        m2comm__pass(&g_m2comm_phantom.s, &g_m2comm_phantom.l);
}

/* A board frame: a master phantom takes what came and makes its pass. */
static void comm_udp_phantom_frame(void) {
    if (!g_m2comm_phantom_master) return;
    comm_udp_drain(&g_comm_udp_phantom);
    m2comm__pass(&g_m2comm_phantom.s, &g_m2comm_phantom.l);
}

static uint32_t comm_udp_phantom__board_recv(void *ctx, uint8_t *msg, uint32_t cap) {
    (void)ctx;
    comm_udp_drain(&g_comm_udp_board);
    return comm_link_recv(&g_comm_udp_board.link, msg, cap);
}

static uint32_t comm_udp_phantom__board_pending(void *ctx) {
    (void)ctx;
    comm_udp_drain(&g_comm_udp_board);
    return comm_link_pending(&g_comm_udp_board.link);
}

static const m2comm_peer_t comm_udp_phantom_board_peer = {
    NULL, comm_udp_phantom__from_board, comm_udp_phantom__board_recv, comm_udp_phantom__board_pending };

/* m2comm_phantom_open, with the two cabinets on either end of a UDP pair.
 * NULL if there are no sockets. */
static const m2comm_peer_t *comm_udp_phantom_open(uint8_t role, uint32_t window) {
    memset(&g_m2comm_phantom, 0, sizeof g_m2comm_phantom);
    if (!comm_udp_pair(&g_comm_udp_board, &g_comm_udp_phantom)) return NULL;
    g_m2comm_phantom_master = role == 1;
    m2comm_node_start(&g_m2comm_phantom, role, window);
    g_comm_udp_phantom_peer.ctx = &g_comm_udp_phantom.link;
    g_m2comm_phantom.l.peer = &g_comm_udp_phantom_peer;
    return &comm_udp_phantom_board_peer;
}

/* m2comm_phantom_attach over a UDP pair. */
static bool comm_udp_phantom_attach(uint32_t window) {
    const m2comm_peer_t *peer = comm_udp_phantom_open(2, window);
    if (peer) m2comm_set_peer(peer);
    return peer != NULL;
}

#endif /* COMM_LINK_LOOP_H */
