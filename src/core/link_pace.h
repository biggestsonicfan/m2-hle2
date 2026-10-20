/*
 * link_pace.h — the board's clock while its comm board is linked (Pinboard
 * #978, step 4 of the RPCN plan in VIRTUAL-ON-PS3.md).
 *
 * Two cabinets on a link each run their own board; neither waits for the
 * other. What keeps them together is that both run at 60 Hz of wall time and
 * take one of the other's frames per board frame (board/m2comm.h). Alone, a
 * host that cannot keep up runs the board slow, and fast-forward runs it fast:
 * nobody loses by it but the player. Linked, both are wrong:
 *   - Game speed is an advantage between two cabinets: the slower board's
 *     pilot moves, turns and fires less often.
 *   - Virtual-On's stage handshake (state 0x24) lives in one frame of the
 *     window. A board that reads less often than the other writes misses it,
 *     and the two load different stages (YAMP, von-netplay-recon.md).
 * So a linked board keeps a fixed timestep against the wall clock, the way
 * YAMP's VirtualClock::BoardFramesDue does:
 *   - Each frame is due one period after the last one was due, not after it
 *     ran: a host callback that came late owes more than one, and runs them.
 *   - At most LINK_PACE_MAX_DEBT frames are owed. Past that (a hitch, a pause,
 *     a hidden tab) the rest is dropped, not repaid: a long stall must not
 *     come back as a burst.
 *   - Nothing runs it faster: fast-forward, unthrottled runs and a watcher's
 *     catch-up get the same 60 Hz.
 *   - The two wall clocks still drift apart, by a frame every minute or so at
 *     a few hundred ppm, and a dropped debt puts the slave frames behind for
 *     good. The master's frames then pile up in the slave's peer (comm_link.h
 *     keeps the 8 newest and drops the oldest, which loses frames the same
 *     way). So a slave with LINK_PACE_BACKLOG or more of them waiting at every
 *     one of LINK_PACE_TRIM_AFTER frame edges runs its next frame one period
 *     early: at most two extra frames a second, until the backlog is gone.
 *     The master never does (m2comm_backlog is 0 there): its clock is the
 *     link's, and the slave only answers what it takes.
 * The board's own frame is unchanged: 416,667 cycles, 60 Hz; this decides only
 * when the host runs it. What it ran is the same board, bit for bit.
 *
 * Two ways to use it:
 *   a host that sleeps (the emu thread): after each frame, link_pace_slept
 *     says until when;
 *   a host that is called (libretro's retro_run, the page's frame callback):
 *     link_pace_owed, then while link_pace_due, run a frame and link_pace_ran.
 * link_pace_stop when the link ends: the next one starts its clock afresh.
 */
#ifndef LINK_PACE_H
#define LINK_PACE_H

#include <stdbool.h>
#include <stdint.h>

#define LINK_PACE_HZ         60
#define LINK_PACE_FRAME_US   (1000000 / LINK_PACE_HZ)   /* emu_thread.h EMU_SLICE_US */
#define LINK_PACE_MAX_DEBT   4u    /* frames owed at most: 1/15 s of catch-up */
#define LINK_PACE_EARLY_US   (LINK_PACE_FRAME_US / 2)   /* how early a called host may run one */
#define LINK_PACE_BACKLOG    2u    /* the peer's messages waiting that count as a backlog */
#define LINK_PACE_TRIM_AFTER 30u   /* frame edges in a row with one, before a frame runs early */

typedef struct {
    int64_t  next_us;       /* when the next frame is due; 0: the clock has not started */
    uint32_t backed;        /* frame edges in a row with a backlog */
    /* Diagnostics. */
    uint64_t frames;        /* frames run on this clock */
    uint64_t dropped;       /* frames owed past the cap, and never run */
    uint32_t trims;         /* frames run early for a backlog */
} link_pace_t;

static inline void link_pace_stop(link_pace_t *p) {
    p->next_us = 0;
    p->backed  = 0;
}

static inline bool link_pace_running(const link_pace_t *p) { return p->next_us != 0; }

/* At `now`: start the clock (the first frame is due now), or drop what is owed
 * past LINK_PACE_MAX_DEBT, so that at most that many are due. */
static inline void link_pace_owed(link_pace_t *p, int64_t now) {
    if (!p->next_us) { p->next_us = now; return; }
    int64_t floor_us = now + LINK_PACE_EARLY_US - (int64_t)LINK_PACE_MAX_DEBT * LINK_PACE_FRAME_US;
    if (p->next_us > floor_us) return;
    uint64_t n = (uint64_t)((floor_us - p->next_us) / LINK_PACE_FRAME_US) + 1u;
    p->dropped += n;
    p->next_us += (int64_t)n * LINK_PACE_FRAME_US;
}

/* A called host: whether a frame is due at `now`. Up to half a period early
 * counts, so a 60 Hz caller's jitter gives one frame a call and not 0, then 2;
 * the frames after it are still due on the same grid, so none is gained. */
static inline bool link_pace_due(const link_pace_t *p, int64_t now) {
    return !p->next_us || now >= p->next_us - LINK_PACE_EARLY_US;
}

/* When the next frame is due (a host that sleeps sleeps until then). */
static inline int64_t link_pace_next(const link_pace_t *p) { return p->next_us; }

/* A frame ran, with `backlog` of the peer's messages still waiting after it
 * (m2comm_backlog: 0 on the master). */
static inline void link_pace_ran(link_pace_t *p, uint32_t backlog) {
    p->frames++;
    if (!p->next_us) return;   /* not started: link_pace_owed starts it */
    p->next_us += LINK_PACE_FRAME_US;
    if (backlog < LINK_PACE_BACKLOG) { p->backed = 0; return; }
    if (++p->backed < LINK_PACE_TRIM_AFTER) return;
    p->backed = 0;
    p->next_us -= LINK_PACE_FRAME_US;
    p->trims++;
}

/* A host that sleeps: a frame ended at `now`; sleep until the time this
 * returns (it may be past: run the next frame at once). The first frame on a
 * new clock starts it. */
static inline int64_t link_pace_slept(link_pace_t *p, int64_t now, uint32_t backlog) {
    if (!p->next_us) {
        p->next_us = now + LINK_PACE_FRAME_US;
        p->frames++;
        return p->next_us;
    }
    link_pace_ran(p, backlog);
    link_pace_owed(p, now);
    return p->next_us;
}

#endif /* LINK_PACE_H */
