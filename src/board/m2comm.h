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
 *   - MAME ships the whole frame size, and Power Sled reads all of it
 *     (m2comm_set_whole_frame): its ROM keeps a ring of 0x180-byte node slots,
 *     the frame received landing one slot up from the sender's, so slot k
 *     holds the node k hops back. It counts the Rides from slots 1..count
 *     (FUN_14F50; the Main is ID 0x200), and with only the window an empty
 *     slot 2 reads as a second Ride: rider 1 gets no slot and its objects NaN
 *     positions (black 3D). The whole frame does not pack into a datagram
 *     once the race fills it, so the window still goes over, and the receiver
 *     makes the rest: past its window the sender's frame is what it last took
 *     from this node, this node's own frame two slots down.
 * A board set to be the MASTER with nobody on the link waits for a ring that
 * never comes up: Daytona's NETWORK CHECKING ends in CANCELLED and a reboot,
 * forever. Each start of the board as a master alone is counted
 * (lone_masters) for the host to ask about (core/link_check.h, Pinboard
 * #1449), and the host can plug in a virtual TOSlink cable (m2comm_set_cable):
 * the board's output wired to its own input, as a loopback cable on a real
 * comm board. The master then hears its own token and makes a ring of one
 * (Daytona: "1 player can race"), MAME's ring code unchanged. A session's
 * peer, when there is one, takes the cable's place.
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
 * board's resets. twin: the other cabinet is a board this process runs too
 * (core/emu_twin.h, a GGPO match of a linked game). */
typedef struct {
    void     *ctx;
    void     (*send)(void *ctx, const uint8_t *msg, uint32_t len);
    uint32_t (*recv)(void *ctx, uint8_t *msg, uint32_t cap);
    uint32_t (*pending)(void *ctx);
    bool      twin;
} m2comm_peer_t;

enum { M2COMM_LINK_DOWN = 0, M2COMM_LINK_UP = 1 };

typedef struct {
    const m2comm_peer_t *peer;      /* NULL: nobody on the link */
    uint8_t  alive;                 /* M2COMM_LINK_DOWN / UP */
    uint8_t  id, count;             /* this node's id and the ring's size */
    uint32_t timer;                 /* the master's countdown to the token */
    uint32_t quiet;                 /* passes the master has gone unanswered */
    uint32_t sent, taken;           /* data frames out and in, for diagnostics */
    bool     whole_frame;           /* MAME's whole frame from the window (m2comm_set_whole_frame) */
    bool     cable;                 /* the virtual TOSlink: output into input, with no peer */
    bool     lone_seen;             /* this start of the board is counted in lone_masters */
    uint32_t lone_masters;          /* starts as a master with nobody on the link (core/link_check.h) */
    uint8_t  msg[M2COMM_MSG_MAX];
} m2comm_link_t;

static m2comm_state_t g_m2comm;
static m2comm_link_t  g_m2comm_link;

/* ---- the virtual TOSlink cable: what a node sends, it takes ---- */

#define M2COMM_LOOP_DEPTH 4u                 /* a pass sends at most two; the next takes them */

typedef struct {
    uint8_t  msg[M2COMM_LOOP_DEPTH][M2COMM_MSG_MAX];
    uint32_t len[M2COMM_LOOP_DEPTH];
    uint32_t head, n;
} m2comm_loop_t;

static m2comm_loop_t g_m2comm_loop;

static void m2comm__loop_send(void *ctx, const uint8_t *msg, uint32_t len) {
    m2comm_loop_t *q = (m2comm_loop_t *)ctx;
    if (q->n == M2COMM_LOOP_DEPTH) { q->head = (q->head + 1u) % M2COMM_LOOP_DEPTH; q->n--; }
    uint32_t at = (q->head + q->n) % M2COMM_LOOP_DEPTH;
    memcpy(q->msg[at], msg, len);
    q->len[at] = len;
    q->n++;
}

static uint32_t m2comm__loop_recv(void *ctx, uint8_t *msg, uint32_t cap) {
    m2comm_loop_t *q = (m2comm_loop_t *)ctx;
    if (!q->n) return 0;
    uint32_t len = q->len[q->head] < cap ? q->len[q->head] : cap;
    memcpy(msg, q->msg[q->head], len);
    q->head = (q->head + 1u) % M2COMM_LOOP_DEPTH;
    q->n--;
    return len;
}

static const m2comm_peer_t g_m2comm_loop_peer = { &g_m2comm_loop, m2comm__loop_send, m2comm__loop_recv, NULL, false };

/* Who the board talks to: the session's peer, else the cable, else nobody. */
static inline const m2comm_peer_t *m2comm__peer(const m2comm_link_t *l) {
    return l->peer ? l->peer : l->cable ? &g_m2comm_loop_peer : NULL;
}

/* ---- the link (Pinboard #959): MAME's comm_tick for two nodes ---- */

static inline bool m2comm__is_master(const m2comm_state_t *s) {
    return s->fg == 1u || s->shared[1] == 1u;
}

static inline uint32_t m2comm__window(const m2comm_state_t *s) {
    uint32_t n = (uint32_t)s->shared[0x14] | (uint32_t)s->shared[0x15] << 8;
    return n < M2COMM_WINDOW_MAX ? n : M2COMM_WINDOW_MAX;
}

/* The peer's window n taken: MAME's whole frame from it, the rest of the
 * sender's frame being this node's own two slots down (two nodes). */
static inline void m2comm__take_whole(m2comm_state_t *s, uint32_t n) {
    uint32_t size = (uint32_t)s->shared[0x12] | (uint32_t)s->shared[0x13] << 8;
    uint32_t room = M2COMM_RAM_SIZE - M2COMM_FRAME_START - n;
    if (size > room) size = room;
    if (size > n)
        memmove(s->shared + M2COMM_FRAME_START + 2u * n, s->shared + M2COMM_FRAME_START, size - n);
}

static inline void m2comm__send(m2comm_link_t *l, uint32_t len) {
    const m2comm_peer_t *p = m2comm__peer(l);
    p->send(p->ctx, l->msg, len);
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
    const m2comm_peer_t *p = m2comm__peer(l);
    uint32_t len;
    while (l->alive == M2COMM_LINK_DOWN && (len = p->recv(p->ctx, l->msg, sizeof l->msg)) > 0)
        m2comm__ring_msg(s, l, len);
    if (l->alive == M2COMM_LINK_DOWN && m2comm__is_master(s)) m2comm__master_up(s, l);
}

/* The ring is up: take at most one data frame, and answer it (the slave) or
 * send this pass's own (the master). */
static inline void m2comm__pass_up(m2comm_state_t *s, m2comm_link_t *l) {
    bool master = m2comm__is_master(s);
    const m2comm_peer_t *p = m2comm__peer(l);
    uint32_t n = m2comm__window(s), len;
    while ((len = p->recv(p->ctx, l->msg, sizeof l->msg)) > 0) {
        if (l->msg[0] > l->count || len != 1u + n) continue;   /* the ring's own traffic, or torn */
        if (l->whole_frame) m2comm__take_whole(s, n);
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
    l->lone_seen = false;
    g_m2comm_loop.head = g_m2comm_loop.n = 0;
}

/* ---- the board ---- */

/* n Z80 passes with nobody on the link: never a message. A master here waits
 * for nobody; each start of the board as one is counted once. */
static inline void m2comm__ticks(uint64_t n) {
    if (!g_m2comm_link.lone_seen && m2comm__is_master(&g_m2comm)) {
        g_m2comm_link.lone_seen = true;
        g_m2comm_link.lone_masters++;
    }
    if (n & 1u) g_m2comm.zfg ^= 1u;
    g_m2comm.shared[0] = 0x00;
    g_m2comm.shared[2] = 0xFF;
    g_m2comm.shared[3] = 0xFF;
}

static inline void m2comm__run(uint64_t n) {
    if (!m2comm__peer(&g_m2comm_link)) { m2comm__ticks(n); return; }
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
        g_m2comm_link.lone_seen = false;
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

/* The virtual TOSlink cable in (true) or out. With no session's peer, the
 * board then takes what it sends: a master's ring comes up as one node on the
 * passes that follow (the countdown it is in, the token, the ring size). It
 * outlives the board's resets, as a peer does; the host takes it out when it
 * loads another set (link_check_reset). */
static inline void m2comm_set_cable(bool on) {
    if (g_m2comm_link.cable == on) return;
    g_m2comm_link.cable = on;
    g_m2comm_loop.head = g_m2comm_loop.n = 0;
    if (!g_m2comm_link.peer) m2comm__link_reset(&g_m2comm_link);
}

static inline bool m2comm_cable(void) { return g_m2comm_link.cable; }

/* How many times the board has been started as a master with nobody on the
 * link: the host asks about each new one (core/link_check.h). */
static inline uint32_t m2comm_lone_masters(void) { return g_m2comm_link.lone_masters; }

/* After m2comm_install: a data frame taken fills MAME's whole frame size from
 * 0x2000 + offset (the ROM's ring of node slots), not just the window. */
static inline void m2comm_set_whole_frame(bool on) { g_m2comm_link.whole_frame = on; }

/* Whether the comm board is linked to another host's cabinet: its host then
 * paces it by the link's clock (core/link_pace.h). Not a twin's (the other
 * cabinet runs in this process): GGPO paces the pair. */
static inline bool m2comm_linked(void) { return g_m2comm_link.peer != NULL && !g_m2comm_link.peer->twin; }

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
    g_m2comm_link.whole_frame = false;
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
