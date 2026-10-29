/*
 * comm_local_test.c — a Versus's link between two cabinets on one machine
 * (src/net/comm_local.h, Pinboard #987): the SLAVE's hello pins the MASTER's
 * other end, messages pass both ways over 127.0.0.1, and a datagram from
 * anyone else (a wrong token, a second hello, another port) is dropped. A
 * SLAVE started again takes the pin once the old one is quiet (#1632). No ROM.
 */
#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "memory.h"
#include "i960.h"
#include "emu_thread.h"
#include "registry.h"
#include "comm_local.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

static comm_local_t g_m = { .sock = NET_SOCK_INVALID }, g_s = { .sock = NET_SOCK_INVALID },
                    g_x = { .sock = NET_SOCK_INVALID };

/* A message through one end's peer, and what the other end's peer takes. */
static bool passes(comm_local_t *from, comm_local_t *to, uint8_t seed) {
    uint8_t msg[64], got[64];
    for (uint32_t i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)(seed + i * 3u);
    from->peer.send(from->peer.ctx, msg, sizeof msg);
    uint32_t n = 0;
    for (int tries = 0; tries < 200 && !n; tries++) {   /* loopback: at once, or nearly */
        if (to->peer.pending(to->peer.ctx)) n = to->peer.recv(to->peer.ctx, got, sizeof got);
    }
    return n == sizeof msg && memcmp(msg, got, n) == 0;
}

static void drain_some(comm_local_t *c) {
    for (int i = 0; i < 200; i++) comm_local_drain(c);
}

int main(void) {
    CHECK(comm_local_listen(&g_m, 0x1234ABCDu, 0x11), "the MASTER's end opens on a port");
    CHECK(g_m.port != 0 && g_m.to_port == 0 && g_m.role == GAME_LINK_MASTER, "it waits for a hello");
    CHECK(!comm_local_connect(&g_x, 0, 1, 0x33), "a SLAVE needs the MASTER's port");

    /* Another program's hello, with the wrong token, first. */
    CHECK(comm_local_connect(&g_x, g_m.port, 0x1234ABCEu, 0x33), "a stranger with the wrong token");
    drain_some(&g_m);
    CHECK(g_m.to_port == 0 && g_m.strangers == 1, "its hello is dropped");

    CHECK(comm_local_connect(&g_s, g_m.port, 0x1234ABCDu, 0x22), "the SLAVE's end says hello");
    drain_some(&g_m);
    CHECK(g_m.to_port == g_s.port, "the hello pins the SLAVE's port");

    CHECK(passes(&g_m, &g_s, 1), "MASTER to SLAVE");
    CHECK(passes(&g_s, &g_m, 2), "SLAVE to MASTER");
    CHECK(g_s.heard && g_m.heard, "each end has heard the other");
    CHECK(passes(&g_m, &g_s, 3) && passes(&g_s, &g_m, 4), "and again, both ways");

    /* The stranger knows the token now: a second hello and its comm datagrams. */
    uint32_t before = g_m.strangers;
    g_x.token = g_m.token;
    comm_local__hello(&g_x);
    uint8_t msg[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    g_x.peer.send(g_x.peer.ctx, msg, sizeof msg);
    drain_some(&g_m);
    CHECK(g_m.to_port == g_s.port, "a second hello does not move the pin");
    CHECK(g_m.strangers > before && !comm_link_pending(&g_m.link), "the stranger's datagrams are dropped");
    CHECK(passes(&g_s, &g_m, 5), "the SLAVE still gets through");

    /* The SLAVE closes and starts again, on a new port, with the token: once
     * the old one has been quiet COMM_LOCAL_GONE_MS, its hello moves the pin. */
    comm_local_close(&g_s);
    for (uint64_t until = net_now_ms() + COMM_LOCAL_GONE_MS + 100u; net_now_ms() < until; ) drain_some(&g_m);
    comm_local__hello(&g_x);
    drain_some(&g_m);
    CHECK(g_m.to_port == g_x.port, "the SLAVE started again takes the pin");
    CHECK(passes(&g_x, &g_m, 6) && passes(&g_m, &g_x, 7), "and the two pass both ways");

    comm_local_close(&g_x);
    comm_local_close(&g_s);
    comm_local_close(&g_m);
    CHECK(!net_sock_valid(g_m.sock) && g_m.to_port == 0, "closed");
    printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
