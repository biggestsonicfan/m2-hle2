/*
 * comm_local.h — Versus on one machine: two cabinets, one per process, their
 * comm boards linked over 127.0.0.1 (Pinboard #987, step 7 of
 * VIRTUAL-ON-PS3.md).
 *
 * The PS3 runs both boards of a Versus in one process. m2-hle2's board is one
 * set of globals, so a Versus here is two emulators, each one cabinet, linked
 * as two machines in a room are: the same comm_link.h datagrams, the same
 * role boot (comm_role.h), the same pace (link_pace.h). Only the transport is
 * new: a UDP socket bound to the loopback, and no RPCN, no room, no NAT.
 *
 *   comm_local_listen   the MASTER's end: a socket on a port the OS picks,
 *                       which the frontend hands the SLAVE it starts
 *   comm_local_connect  the SLAVE's end: its own socket, and a hello to the
 *                       MASTER's port, so the MASTER learns where it is
 *   comm_local_begin    the board boots into its role and the link plugs in
 *                       (comm_role_begin), as netplay_comm_open does
 *
 * The hello carries a token the MASTER made up and passed on the SLAVE's
 * command line: the MASTER takes datagrams only from the end that sent it, so
 * another program on the loopback is not a cabinet. Until the hello the
 * MASTER sends nothing (its ROM's first messages are the ring token, which it
 * repeats). The SLAVE says hello again from its recv until the MASTER is
 * heard, in case the first went before the MASTER's socket was read.
 *
 * Drained from the peer's recv and pending, which m2comm.h calls from the
 * board's slice (emu thread, emu mutex held): the socket never blocks.
 */
#ifndef NET_COMM_LOCAL_H
#define NET_COMM_LOCAL_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "net_socket.h"
#include "comm_link.h"
#include "comm_role.h"

#define COMM_LOCAL_MAGIC   0x5356324Du   /* "M2VS" */
#define COMM_LOCAL_REHELLO 60u           /* SLAVE recv calls between hellos */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t token;
} comm_local_hello_t;
#pragma pack(pop)

typedef struct {
    net_sock_t       sock;
    uint16_t         port;       /* ours */
    uint16_t         to_port;    /* the other cabinet's; 0 until its hello (MASTER) */
    uint32_t         token;
    game_link_role_t role;
    bool             heard;      /* a comm datagram has come from the other end */
    uint32_t         quiet;      /* SLAVE: recv calls since the last hello */
    comm_link_t      link;
    m2comm_peer_t    peer;
    uint32_t         strangers;  /* datagrams from anyone else, dropped */
} comm_local_t;

static comm_local_t g_comm_local = { .sock = NET_SOCK_INVALID };

/* A non-blocking UDP socket on 127.0.0.1, on a port the OS picks. */
static inline bool comm_local__open(comm_local_t *c) {
    if (!net_startup()) return false;
    net_sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!net_sock_valid(s)) return false;
    net_set_nonblocking(s);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#ifdef _WIN32
    int n = (int)sizeof a;
#else
    socklen_t n = sizeof a;
#endif
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || getsockname(s, (struct sockaddr *)&a, &n) != 0) {
        net_close(&s);
        return false;
    }
    c->sock = s;
    c->port = ntohs(a.sin_port);
    return c->port != 0;
}

static inline void comm_local__hello(comm_local_t *c) {
    comm_local_hello_t h = { COMM_LOCAL_MAGIC, c->token };
    net_udp_send(c->sock, htonl(INADDR_LOOPBACK), c->to_port, &h, (uint32_t)sizeof h);
    c->quiet = 0;
}

static inline void comm_local__transmit(void *ud, const void *buf, uint32_t len) {
    comm_local_t *c = (comm_local_t *)ud;
    if (c->to_port) net_udp_send(c->sock, htonl(INADDR_LOOPBACK), c->to_port, buf, len);
}

/* One datagram from the loopback: the hello pins the MASTER's other end; a
 * comm datagram from that end goes to the link. */
static inline void comm_local__take(comm_local_t *c, const uint8_t *buf, int got, uint32_t ip, uint16_t port) {
    if (ip != htonl(INADDR_LOOPBACK)) { c->strangers++; return; }
    comm_local_hello_t h;
    if (got == (int)sizeof h) {
        memcpy(&h, buf, sizeof h);
        if (c->role == GAME_LINK_MASTER && h.magic == COMM_LOCAL_MAGIC && h.token == c->token
                && (!c->to_port || c->to_port == port)) {
            c->to_port = port;
            return;
        }
        c->strangers++;
        return;
    }
    if (!c->to_port || port != c->to_port || got < (int)sizeof(lockstep_header_t) || buf[0] != LOCKSTEP_PACKET_COMM) {
        c->strangers++;
        return;
    }
    c->heard = true;
    comm_link_on_datagram(&c->link, buf, (uint32_t)got);
}

static inline void comm_local_drain(comm_local_t *c) {
    uint8_t buf[COMM_LINK_DGRAM_MAX];
    uint32_t ip;
    uint16_t port;
    int got;
    while ((got = net_udp_recv(c->sock, buf, sizeof buf, &ip, &port)) > 0)
        comm_local__take(c, buf, got, ip, port);
}

static inline void comm_local__send(void *ctx, const uint8_t *msg, uint32_t len) {
    comm_link_send(&((comm_local_t *)ctx)->link, msg, len);
}

static inline uint32_t comm_local__recv(void *ctx, uint8_t *msg, uint32_t cap) {
    comm_local_t *c = (comm_local_t *)ctx;
    comm_local_drain(c);
    if (c->role == GAME_LINK_SLAVE && !c->heard && ++c->quiet >= COMM_LOCAL_REHELLO) comm_local__hello(c);
    return comm_link_recv(&c->link, msg, cap);
}

static inline uint32_t comm_local__pending(void *ctx) {
    comm_local_t *c = (comm_local_t *)ctx;
    comm_local_drain(c);
    return comm_link_pending(&c->link);
}

static inline void comm_local__start(comm_local_t *c, game_link_role_t role, uint32_t token, uint16_t epoch) {
    lockstep_header_t h;
    memset(&h, 0, sizeof h);
    c->role  = role;
    c->token = token;
    comm_link_start(&c->link, epoch, &h, comm_local__transmit, c);
    c->peer = (m2comm_peer_t){ c, comm_local__send, comm_local__recv, comm_local__pending };
}

/* The MASTER's end, waiting for the SLAVE's hello. False if no socket. */
static inline bool comm_local_listen(comm_local_t *c, uint32_t token, uint16_t epoch) {
    if (!comm_local__open(c)) return false;
    c->to_port = 0;
    comm_local__start(c, GAME_LINK_MASTER, token, epoch);
    return true;
}

/* The SLAVE's end, sending its hello to the MASTER on master_port. */
static inline bool comm_local_connect(comm_local_t *c, uint16_t master_port, uint32_t token, uint16_t epoch) {
    if (!master_port || !comm_local__open(c)) return false;
    c->to_port = master_port;
    comm_local__start(c, GAME_LINK_SLAVE, token, epoch);
    comm_local__hello(c);
    return true;
}

/* Link the board to the other cabinet as its role: it boots into the role at
 * a frame edge (emu_comm_role_pump, with the frontend's reset hook) and plugs
 * the link in then. Emu mutex held, or before the emu thread runs. */
static inline void comm_local_begin(comm_local_t *c) {
    comm_role_begin(c->role, &c->peer);
}

static inline void comm_local_close(comm_local_t *c) {
    comm_role_end();
    net_close(&c->sock);
    c->to_port = 0;
}

#endif /* NET_COMM_LOCAL_H */
