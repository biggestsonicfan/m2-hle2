/*
 * heat_guard.h -- a host's answer to a device that runs hot, in stages.
 *
 * Fed the hottest thermal zone and the clock once a frame, the guard says how
 * many board frames each drawn picture should cover and whether the sound
 * board should be off. It reads nothing and touches nothing itself: the host
 * (main_libretro.c) reads the zones, skips the draws and detaches the board,
 * which is what keeps it a pure function that tests/heat_test.c can drive
 * through a whole afternoon in a millisecond.
 *
 * The stages, none of which is undone within a load:
 *
 *   1. Hot (at or above the limit): draw every second frame until the device
 *      has cooled HEAT_HYSTERESIS_C degrees below the limit, then every frame
 *      again. The board runs at 60 either way.
 *   2. Hot a second time: every second frame from then on, for good.
 *   3. Still hot at that -- hot a third time, or not below the limit
 *      HEAT_GRACE_US after sticking -- the sound board (the 68000 + SCSP,
 *      stepped a sample at a time, the dearest thing the board does) should go.
 *      `sound_off` is a level, not an edge: it holds while the device is hot
 *      and the host acts on it when it may. It may NOT inside an online match,
 *      where the other board runs the sound board and this one has to
 *      (main_libretro.c lr_sound_for_netplay): the i960 runs different code
 *      with the UART answered and without, and a session is lockstep from a
 *      cold boot. Drawing every second frame is the host's business alone and
 *      is fine in a match.
 *
 * Changing the limit starts the stages over (heat_guard_set_limit); the host
 * decides what to do with a sound board it has already dropped.
 */
#ifndef HEAT_GUARD_H
#define HEAT_GUARD_H

#include <stdbool.h>
#include <stdint.h>

#define HEAT_HYSTERESIS_C   5              /* cooled = this far below the limit */
#define HEAT_GRACE_US       60000000LL     /* a minute at every second frame, before the sound board goes */

typedef struct {
    int     limit_c;      /* 0 = off */
    bool    hot;          /* at or above the limit, and not yet HEAT_HYSTERESIS_C below it */
    int     episodes;     /* times it has gone hot */
    bool    stuck;        /* every second frame for good: the second episode */
    int64_t stuck_us;     /* when it stuck */
    bool    sound_off;    /* the verdict: the sound board should be off right now */
} heat_guard_t;

/* What the last update changed, for the host's messages. */
typedef enum {
    HEAT_NONE = 0,
    HEAT_HOT,             /* first time: every second frame until it cools */
    HEAT_COOLED,          /* every frame again */
    HEAT_STUCK,           /* hot again: every second frame from now on */
    HEAT_COOLED_STUCK,    /* cooled, still every second frame */
    HEAT_HOT_STUCK,       /* hot a third time or more (sound_off says what follows) */
} heat_event_t;

static inline void heat_guard_init(heat_guard_t *g, int limit_c) {
    heat_guard_t z = { 0 };
    *g = z;
    g->limit_c = limit_c > 0 ? limit_c : 0;
}

/* A new limit starts over; the same one changes nothing. */
static inline void heat_guard_set_limit(heat_guard_t *g, int limit_c) {
    if (limit_c < 0) limit_c = 0;
    if (g->limit_c != limit_c) heat_guard_init(g, limit_c);
}

/* c: the hottest zone in degrees, or -1 where there is none. */
static inline heat_event_t heat_guard_update(heat_guard_t *g, int c, int64_t now_us) {
    heat_event_t ev = HEAT_NONE;
    if (g->limit_c <= 0 || c < 0) {
        g->hot = false;
        g->sound_off = false;
        return HEAT_NONE;
    }
    if (!g->hot) {
        if (c >= g->limit_c) {
            g->hot = true;
            g->episodes++;
            if (g->episodes == 1) {
                ev = HEAT_HOT;
            } else if (!g->stuck) {
                g->stuck    = true;
                g->stuck_us = now_us;
                ev = HEAT_STUCK;
            } else {
                ev = HEAT_HOT_STUCK;
            }
        }
    } else if (c <= g->limit_c - HEAT_HYSTERESIS_C) {
        g->hot = false;
        ev = g->stuck ? HEAT_COOLED_STUCK : HEAT_COOLED;
    }
    g->sound_off = g->stuck && g->hot
                && (g->episodes >= 3 || now_us - g->stuck_us >= HEAT_GRACE_US);
    return ev;
}

/* How many board frames each drawn one covers: 2 while hot or stuck. */
static inline int heat_guard_draw_every(const heat_guard_t *g) {
    return (g->hot || g->stuck) ? 2 : 1;
}

#endif /* HEAT_GUARD_H */
