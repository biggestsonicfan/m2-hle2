/*
 * m2comm.h — the Model 2 communication board, alone or linked to one other
 * cabinet (Pinboard #935, #959).
 *
 * memory.h's COMM stub answers a game that only asks whether the board is
 * there. Virtual-On talks to it: its probe (0xC5870) writes CN 1 and waits for
 * the board's Z80 to toggle FG bit 7, and its machine select waits for the link
 * state its vblank handler reads from the shared RAM (0x5770B0, 0 = alone).
 * Read as "no board" (CN bit 0 set), it says so and the link state stays 0xFF,
 * so the select never comes.
 *
 * This is MAME's m2comm.cpp (M2COMM_SIMULATION):
 *   0x01A00000..0x01A03FFF  shared RAM, mirrored +0x10000
 *   0x01A04000  CN: reads 0xFE | cn; a write of 1 starts the board (shared RAM
 *               zeroed, [1] = 2 "slave", frame size 0x0E00, frame offset), 0
 *               stops it
 *   0x01A04002  FG: reads 0x7E | fg | (zfg ? 0 : 0x80)
 * Each vblank while the board runs, the Z80 makes one pass (comm_tick). That
 * runs lazily here: an access first catches up on the vblanks counted since
 * the last one (irqt), so nothing hangs off the vblank and the result is the
 * same.
 *
 * With nobody on the link (no peer: every board but a linked session's), a
 * pass toggles zfg and writes "no link yet" ([0] = 0, [2] = [3] = 0xFF), what
 * MAME runs for one cabinet. Every byte of that state is in g_m2comm, POD; a
 * profile that installs it saves it as a savestate extra.
 *
 * Linked (m2comm_set_peer), the passes are MAME's ring for two nodes, over
 * datagrams to and from the other cabinet's board (Pinboard #959, the first
 * RPCN step in VIRTUAL-ON-PS3.md; the other side runs the same code):
 *   - Role. The ROM writes its cabinet's role into [1] (Virtual-On's link
 *     check 0xC5870, step 10): 1 is the master, 2 the slave (the board's own
 *     reset value). FG bit 0 set also makes a master (EPR-16726's way).
 *   - Ring up. The master waits 0xE8 passes, then sends the id token
 *     {0xFF, 1, 0}; the slave counts itself in ({0xFF, 2}) and sends it back,
 *     so the master knows the count; the master sends {0xFE, count, count} and
 *     the slave takes its id and the count from it. Each then writes [0] = 1,
 *     [2] = its id (master 1, slave 2), [3] = the count (2).
 *   - Data. Each pass the master sends {1, window}; the slave answers each one
 *     with {2, window}. A node that takes a data frame copies its window to
 *     0x2000 + frame offset and toggles zfg. The window is the frame offset's
 *     bytes from 0x2000: the ROM halves the frame size into the offset (step
 *     10), so for Virtual-On it is its 0x700-byte comm window (cSend 0x5032F0
 *     -> 0x1A12000 out; the peer's lands at 0x1A12700 -> cRecn 0x5024F0), and
 *     it goes over raw. MAME ships the whole frame size (0xE00): the other
 *     node's half, which the receiver overwrites, rides along for nothing.
 * Where this is not MAME, for datagrams that can be lost, duplicated or late:
 *   - One data frame is taken per pass; the rest wait in the peer for the next
 *     ones. Virtual-On's stage handshake (state 0x24) lives in a single frame
 *     of the window, so a pass that took two would lose it (YAMP,
 *     von-comm-packet.md, "Protocol properties").
 *   - A master whose token or ring size got no answer sends it again every
 *     M2COMM_RESEND passes, where MAME's TCP never loses one.
 *   - No VSYNC frames (0xFC): MAME sends them only for its frame-sync option.
 *   - FG reads take nothing (MAME's read_fg): frames come only at the pass.
 *   - Two nodes, no relay.
 * The link state is g_m2comm_link, not g_m2comm: what the board reads there
 * is the other cabinet's, outside savestates, rewind and det_digest, and
 * nothing in it is board state a savestate could bring back.
 */
#ifndef M2COMM_H
#define M2COMM_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "memory.h"
#include "irq_timer.h"

#define M2COMM_RAM_BASE 0x01A00000u
#define M2COMM_RAM_SIZE 0x4000u

#define M2COMM_FRAME_START 0x2000u           /* MAME frameStart: the node's own window */
#define M2COMM_WINDOW_MAX  ((M2COMM_RAM_SIZE - M2COMM_FRAME_START) / 2u)
#define M2COMM_MSG_MAX     (1u + M2COMM_WINDOW_MAX)
#define M2COMM_LINK_DELAY  0xE8u             /* MAME m_linktimer: 58 fps * 4 s */
#define M2COMM_RESEND      60u               /* passes between a master's repeats */
#define M2COMM_CATCH_UP    4u                /* most passes one access runs, linked */

typedef struct {
    uint8_t  shared[M2COMM_RAM_SIZE];
    uint8_t  cn, fg, zfg, enabled;
    uint32_t frame_offset;          /* MAME m_frameoffset: 0x1C0 (0x180 Power Sled) */
    uint64_t vbl_seen;              /* the vblank count the state is up to */
} m2comm_state_t;

/* The other cabinet: datagrams, each a frame type byte and its payload.
 * recv returns the next one's length (at most cap bytes) or 0 if none has
 * come. pending (may be NULL) is how many are waiting: the host's link clock
 * reads it (core/link_pace.h). The link session owns it; it outlives the
 * board's resets. */
typedef struct {
    void     *ctx;
    void     (*send)(void *ctx, const uint8_t *msg, uint32_t len);
    uint32_t (*recv)(void *ctx, uint8_t *msg, uint32_t cap);
    uint32_t (*pending)(void *ctx);
} m2comm_peer_t;

enum { M2COMM_LINK_DOWN = 0, M2COMM_LINK_UP = 1 };

typedef struct {
    const m2comm_peer_t *peer;      /* NULL: nobody on the link */
    uint8_t  alive;                 /* M2COMM_LINK_DOWN / UP */
    uint8_t  id, count;             /* this node's id and the ring's size */
    uint32_t timer;                 /* the master's countdown to the token */
    uint32_t quiet;                 /* passes the master has gone unanswered */
    uint32_t sent, taken;           /* data frames out and in, for diagnostics */
    uint8_t  msg[M2COMM_MSG_MAX];
} m2comm_link_t;

static m2comm_state_t g_m2comm;
static m2comm_link_t  g_m2comm_link;

/* ---- the link (Pinboard #959): MAME's comm_tick for two nodes ---- */

static inline bool m2comm__is_master(const m2comm_state_t *s) {
    return s->fg == 1u || s->shared[1] == 1u;
}

static inline uint32_t m2comm__window(const m2comm_state_t *s) {
    uint32_t n = (uint32_t)s->shared[0x14] | (uint32_t)s->shared[0x15] << 8;
    return n < M2COMM_WINDOW_MAX ? n : M2COMM_WINDOW_MAX;
}

static inline void m2comm__send(m2comm_link_t *l, uint32_t len) {
    l->peer->send(l->peer->ctx, l->msg, len);
}

static inline void m2comm__send_data(m2comm_state_t *s, m2comm_link_t *l) {
    uint32_t n = m2comm__window(s);
    l->msg[0] = l->id;
    memcpy(l->msg + 1, s->shared + M2COMM_FRAME_START, n);
    m2comm__send(l, 1u + n);
    l->sent++;
}

static inline void m2comm__send_size(m2comm_link_t *l) {
    l->msg[0] = 0xFE; l->msg[1] = l->count; l->msg[2] = l->count;
    m2comm__send(l, 3);
}

static inline void m2comm__ring_up(m2comm_state_t *s, m2comm_link_t *l) {
    l->alive = M2COMM_LINK_UP;
    l->quiet = 0;
    s->shared[0] = 0x01;
    s->shared[2] = l->id;
    s->shared[3] = l->count;
}

/* A token or ring size while the ring comes up (MAME's 0xFF / 0xFE). */
static inline void m2comm__ring_msg(m2comm_state_t *s, m2comm_link_t *l, uint32_t len) {
    if (len != 3) return;
    bool master = m2comm__is_master(s);
    if (l->msg[0] == 0xFF) {
        if (master) { l->id = 1; l->count = l->msg[1]; l->timer = 0; return; }
        l->msg[1]++;                           /* the slave counts itself in */
        m2comm__send(l, len);
    } else if (l->msg[0] == 0xFE && !master) {
        l->id = l->msg[1]; l->count = l->msg[2];
        l->msg[1]--;
        m2comm__send(l, len);
        m2comm__ring_up(s, l);
    }
}

/* The master's side of bringing the ring up: wait, the token, then the size. */
static inline void m2comm__master_up(m2comm_state_t *s, m2comm_link_t *l) {
    if (l->timer > 1u) { l->timer--; return; }
    if (l->timer == 0u) {
        m2comm__send_size(l);
        m2comm__ring_up(s, l);
        return;
    }
    if (l->quiet++ % M2COMM_RESEND) return;    /* the token is out; repeat it now and then */
    l->msg[0] = 0xFF; l->msg[1] = 0x01; l->msg[2] = 0x00;
    m2comm__send(l, 3);
}

static inline void m2comm__pass_down(m2comm_state_t *s, m2comm_link_t *l) {
    s->shared[0] = 0x00;
    s->shared[2] = 0xFF;
    s->shared[3] = 0xFF;
    s->zfg ^= 1u;
    uint32_t len;
    while (l->alive == M2COMM_LINK_DOWN && (len = l->peer->recv(l->peer->ctx, l->msg, sizeof l->msg)) > 0)
        m2comm__ring_msg(s, l, len);
    if (l->alive == M2COMM_LINK_DOWN && m2comm__is_master(s)) m2comm__master_up(s, l);
}

/* The ring is up: take at most one data frame, and answer it (the slave) or
 * send this pass's own (the master). */
static inline void m2comm__pass_up(m2comm_state_t *s, m2comm_link_t *l) {
    bool master = m2comm__is_master(s);
    uint32_t n = m2comm__window(s), len;
    while ((len = l->peer->recv(l->peer->ctx, l->msg, sizeof l->msg)) > 0) {
        if (l->msg[0] > l->count || len != 1u + n) continue;   /* the ring's own traffic, or torn */
        memcpy(s->shared + M2COMM_FRAME_START + n, l->msg + 1, n);
        s->zfg ^= 1u;
        l->taken++;
        l->quiet = 0;
        if (!master) m2comm__send_data(s, l);
        break;
    }
    if (!master) return;
    /* A slave that never got the ring size never answers: tell it again. */
    if (++l->quiet % M2COMM_RESEND == 0u) m2comm__send_size(l);
    m2comm__send_data(s, l);
}

/* One Z80 pass (MAME comm_tick). */
static inline void m2comm__pass(m2comm_state_t *s, m2comm_link_t *l) {
    if (!s->enabled) return;
    if (l->alive == M2COMM_LINK_UP) m2comm__pass_up(s, l);
    else                            m2comm__pass_down(s, l);
}

/* A board start (CN 1) brings the ring up again from scratch. */
static inline void m2comm__link_reset(m2comm_link_t *l) {
    l->alive = M2COMM_LINK_DOWN;
    l->id = l->count = 0;
    l->timer = M2COMM_LINK_DELAY;
    l->quiet = 0;
}

/* ---- the board ---- */

/* n Z80 passes with nobody on the link: never a message. */
static inline void m2comm__ticks(uint64_t n) {
    if (n & 1u) g_m2comm.zfg ^= 1u;
    g_m2comm.shared[0] = 0x00;
    g_m2comm.shared[2] = 0xFF;
    g_m2comm.shared[3] = 0xFF;
}

static inline void m2comm__run(uint64_t n) {
    if (!g_m2comm_link.peer) { m2comm__ticks(n); return; }
    if (n > M2COMM_CATCH_UP) n = M2COMM_CATCH_UP;
    while (n--) m2comm__pass(&g_m2comm, &g_m2comm_link);
}

static inline void m2comm__catch_up(void) {
    irqt_flush();
    uint64_t now = (uint64_t)g_irqt.vbl_count;
    /* now < vbl_seen: irqt was reset under a running board; start counting again. */
    if (g_m2comm.enabled && now > g_m2comm.vbl_seen) m2comm__run(now - g_m2comm.vbl_seen);
    g_m2comm.vbl_seen = now;
}

static inline void m2comm__cn_w(uint8_t v) {
    g_m2comm.cn = v & 1u;
    if (!g_m2comm.cn) {
        g_m2comm.enabled = 0;
        g_m2comm.zfg = 0;
        g_m2comm.fg = 0;
        return;
    }
    g_m2comm.enabled = 1;
    memset(g_m2comm.shared, 0, sizeof g_m2comm.shared);
    g_m2comm.shared[0x01] = 0x02;
    g_m2comm.shared[0x12] = 0x00;
    g_m2comm.shared[0x13] = 0x0E;
    g_m2comm.shared[0x14] = (uint8_t)g_m2comm.frame_offset;
    g_m2comm.shared[0x15] = (uint8_t)(g_m2comm.frame_offset >> 8);
    m2comm__link_reset(&g_m2comm_link);
    m2comm__run(1);
}

static inline uint8_t m2comm__status_r(uint32_t off) {
    switch (off & 3u) {
        case 0:  return (uint8_t)(0xFEu | g_m2comm.cn);
        case 2:  return (uint8_t)(0x7Eu | g_m2comm.fg | (g_m2comm.zfg ? 0u : 0x80u));
        default: return 0;
    }
}

static uint32_t m2comm_status_read_cb(mem_region_t *r, uint32_t addr, int size) {
    m2comm__catch_up();
    uint32_t off = addr - r->base, v = 0;
    for (int k = size - 1; k >= 0; k--) v = (v << 8) | m2comm__status_r(off + (uint32_t)k);
    return v;
}

static void m2comm_status_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    m2comm__catch_up();
    uint32_t off = addr - r->base;
    for (int k = 0; k < size; k++) {
        uint8_t b = (uint8_t)(val >> (8 * k));
        if (((off + (uint32_t)k) & 3u) == 0)      m2comm__cn_w(b);
        else if (((off + (uint32_t)k) & 3u) == 2) g_m2comm.fg = b & 1u;
    }
}

static uint32_t m2comm_ram_read_cb(mem_region_t *r, uint32_t addr, int size) {
    m2comm__catch_up();
    uint32_t off = addr - r->base, v = 0;
    for (int k = size - 1; k >= 0; k--) v = (v << 8) | g_m2comm.shared[(off + (uint32_t)k) & (M2COMM_RAM_SIZE - 1u)];
    return v;
}

static void m2comm_ram_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    m2comm__catch_up();
    uint32_t off = addr - r->base;
    for (int k = 0; k < size; k++)
        g_m2comm.shared[(off + (uint32_t)k) & (M2COMM_RAM_SIZE - 1u)] = (uint8_t)(val >> (8 * k));
}

/* The other cabinet's board, or NULL for nobody (the default). The ring comes
 * up when the game next starts its board, as the ROM's link check does at boot. */
static inline void m2comm_set_peer(const m2comm_peer_t *peer) {
    g_m2comm_link.peer = peer;
    m2comm__link_reset(&g_m2comm_link);
}

/* Whether the comm board is linked to another cabinet: its host then paces it
 * by the link's clock (core/link_pace.h). */
static inline bool m2comm_linked(void) { return g_m2comm_link.peer != NULL; }

/* The master's messages waiting for a slave's next passes; 0 on the master.
 * The master's clock is the link's: it sends a window every pass, and the
 * slave answers each one it takes, so only the slave can fall behind. A
 * master that ran early to work off the slave's answers would only hand the
 * slave one more of its own, and the two would chase each other's backlog. */
static inline uint32_t m2comm_backlog(void) {
    const m2comm_peer_t *p = g_m2comm_link.peer;
    if (!p || !p->pending || m2comm__is_master(&g_m2comm)) return 0u;
    return p->pending(p->ctx);
}

/* Call after mem_init: the COMM status regions answer as the board, and the
 * shared RAM (which mem_init leaves unmapped) is added with its mirror. A
 * peer set before stays: the session outlives the board. */
static inline void m2comm_install(memory_bus_t *bus, uint32_t frame_offset) {
    memset(&g_m2comm, 0, sizeof g_m2comm);
    m2comm__link_reset(&g_m2comm_link);
    g_m2comm.frame_offset = frame_offset;
    irqt_flush();
    g_m2comm.vbl_seen = (uint64_t)g_irqt.vbl_count;
    for (int i = 0; i < bus->region_count; i++) {
        mem_region_t *r = &bus->regions[i];
        if (strcmp(r->name, "COMM") == 0) {
            r->read_cb  = m2comm_status_read_cb;
            r->write_cb = m2comm_status_write_cb;
        }
    }
    for (uint32_t base = M2COMM_RAM_BASE; base <= M2COMM_RAM_BASE + 0x10000u; base += 0x10000u) {
        mem_region_t *r = mem_add_region(bus, "COMM_RAM", base, M2COMM_RAM_SIZE, NULL, 0);
        if (r) {
            r->read_cb  = m2comm_ram_read_cb;
            r->write_cb = m2comm_ram_write_cb;   /* RAM, not a FIFO: ldq/stq step through it */
        }
    }
}

#endif /* M2COMM_H */
