/*
 * heat_test.c -- the heat guard's stages (core/heat_guard.h), an afternoon of
 * readings in a millisecond. Pure: no ROM, no thermal zones, no frontend.
 *
 *  (A) Off, or no zones to read: nothing ever happens.
 *  (B) One episode: every second frame from the limit up, every frame again
 *      only once the device is 5 degrees below it -- not one degree sooner.
 *  (C) The second episode sticks: cooling no longer brings every frame back.
 *  (D) Still hot: a third episode says the sound board goes, and so does a
 *      minute at or above the limit after sticking; a cool-down withdraws the
 *      verdict (the host's detach is one-way, the verdict is a level).
 *  (E) A new limit starts over; the same limit again changes nothing.
 *
 * The host's half -- reading the zones, skipping draws, detaching the sound
 * board, and refusing to while a session is being set up or played -- is
 * main_libretro.c and is not under test here.
 */
#define NDEBUG 1
#include <stdio.h>

#include "heat_guard.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

#define SEC(n) ((int64_t)(n) * 1000000LL)

/* Two episodes, stuck at the end of the second. Returns the time it stuck. */
static int64_t two_episodes(heat_guard_t *g, int limit, int64_t t) {
    heat_guard_init(g, limit);
    heat_guard_update(g, limit, t);                          /* 1: hot */
    heat_guard_update(g, limit - HEAT_HYSTERESIS_C, t + SEC(30));   /* cooled */
    heat_guard_update(g, limit, t + SEC(60));                /* 2: hot again, stuck */
    return t + SEC(60);
}

int main(void) {
    heat_guard_t g;

    /* (A) Off, and no zones. */
    heat_guard_init(&g, 0);
    CHECK(heat_guard_update(&g, 95, 0) == HEAT_NONE, "A: off, 95 C: no event");
    CHECK(heat_guard_draw_every(&g) == 1 && !g.sound_off, "A: off: every frame, sound stays");
    heat_guard_init(&g, 85);
    CHECK(heat_guard_update(&g, -1, 0) == HEAT_NONE, "A: no zones: no event");
    CHECK(heat_guard_draw_every(&g) == 1 && !g.sound_off, "A: no zones: every frame, sound stays");

    /* (B) One episode, with the hysteresis. */
    heat_guard_init(&g, 85);
    CHECK(heat_guard_update(&g, 70, 0) == HEAT_NONE && heat_guard_draw_every(&g) == 1, "B: 70 C: nothing");
    CHECK(heat_guard_update(&g, 84, SEC(1)) == HEAT_NONE && heat_guard_draw_every(&g) == 1, "B: 84 C: still nothing");
    CHECK(heat_guard_update(&g, 85, SEC(2)) == HEAT_HOT, "B: 85 C: hot");
    CHECK(heat_guard_draw_every(&g) == 2, "B: hot: every second frame");
    CHECK(heat_guard_update(&g, 85, SEC(4)) == HEAT_NONE && heat_guard_draw_every(&g) == 2, "B: staying hot is not a new event");
    CHECK(heat_guard_update(&g, 81, SEC(6)) == HEAT_NONE && heat_guard_draw_every(&g) == 2, "B: 81 C: 4 below is not cooled");
    CHECK(heat_guard_update(&g, 80, SEC(8)) == HEAT_COOLED, "B: 80 C: cooled");
    CHECK(heat_guard_draw_every(&g) == 1, "B: cooled: every frame again");
    CHECK(g.episodes == 1 && !g.stuck && !g.sound_off, "B: one episode, not stuck, sound stays");
    CHECK(heat_guard_update(&g, 84, SEC(10)) == HEAT_NONE && heat_guard_draw_every(&g) == 1, "B: 84 C after cooling: every frame");

    /* (C) The second episode sticks. */
    CHECK(heat_guard_update(&g, 85, SEC(12)) == HEAT_STUCK, "C: hot again: stuck");
    CHECK(heat_guard_draw_every(&g) == 2 && g.stuck && g.episodes == 2, "C: stuck: every second frame");
    CHECK(!g.sound_off, "C: sticking alone does not drop the sound board");
    CHECK(heat_guard_update(&g, 80, SEC(14)) == HEAT_COOLED_STUCK, "C: cooled while stuck");
    CHECK(heat_guard_draw_every(&g) == 2, "C: cooled, still every second frame");
    CHECK(heat_guard_update(&g, 60, SEC(16)) == HEAT_NONE && heat_guard_draw_every(&g) == 2, "C: cold, still every second frame");

    /* (D) Still hot: the third episode. */
    CHECK(heat_guard_update(&g, 85, SEC(18)) == HEAT_HOT_STUCK, "D: a third episode");
    CHECK(g.sound_off, "D: third episode: the sound board should go");
    CHECK(heat_guard_draw_every(&g) == 2, "D: and every second frame");
    CHECK(heat_guard_update(&g, 85, SEC(20)) == HEAT_NONE && g.sound_off, "D: the verdict holds while hot");
    CHECK(heat_guard_update(&g, 80, SEC(22)) == HEAT_COOLED_STUCK && !g.sound_off, "D: cooled: the verdict is withdrawn");
    CHECK(heat_guard_draw_every(&g) == 2, "D: cooled after the third: still every second frame");
    CHECK(heat_guard_update(&g, 85, SEC(24)) == HEAT_HOT_STUCK && g.sound_off, "D: a fourth episode says so again");

    /* (D) Still hot: never cooled after sticking. */
    int64_t stuck_at = two_episodes(&g, 85, 0);
    CHECK(g.stuck && !g.sound_off, "D: stuck, sound stays at first");
    CHECK(heat_guard_update(&g, 86, stuck_at + SEC(30)) == HEAT_NONE && !g.sound_off, "D: 30 s hot at every second frame: sound stays");
    CHECK(heat_guard_update(&g, 86, stuck_at + HEAT_GRACE_US - 1) == HEAT_NONE && !g.sound_off, "D: just short of the minute: sound stays");
    CHECK(heat_guard_update(&g, 86, stuck_at + HEAT_GRACE_US) == HEAT_NONE && g.sound_off, "D: a minute hot at every second frame: the sound board should go");
    CHECK(heat_guard_update(&g, 81, stuck_at + SEC(90)) == HEAT_NONE && g.sound_off, "D: 4 below the limit is still hot: the verdict holds");
    CHECK(heat_guard_update(&g, 80, stuck_at + SEC(100)) == HEAT_COOLED_STUCK && !g.sound_off, "D: cooled at last: withdrawn");
    CHECK(heat_guard_draw_every(&g) == 2, "D: and still every second frame");
    /* Cooled inside the minute and hot again after it: an episode, not the grace. */
    stuck_at = two_episodes(&g, 85, 0);
    heat_guard_update(&g, 80, stuck_at + SEC(20));
    CHECK(!g.sound_off && g.stuck, "D: cooled within the minute: sound stays");
    CHECK(heat_guard_update(&g, 85, stuck_at + SEC(120)) == HEAT_HOT_STUCK && g.sound_off, "D: hot again later: the sound board should go at once");

    /* (E) Limits. */
    two_episodes(&g, 85, 0);
    heat_guard_set_limit(&g, 85);
    CHECK(g.stuck && g.episodes == 2 && g.hot, "E: the same limit changes nothing");
    heat_guard_set_limit(&g, 90);
    CHECK(!g.stuck && g.episodes == 0 && !g.hot && g.limit_c == 90, "E: a new limit starts over");
    CHECK(heat_guard_update(&g, 87, 0) == HEAT_NONE && heat_guard_draw_every(&g) == 1, "E: 87 C is under the new limit");
    CHECK(heat_guard_update(&g, 90, SEC(2)) == HEAT_HOT, "E: 90 C is the new limit");
    heat_guard_set_limit(&g, 0);
    CHECK(heat_guard_update(&g, 99, SEC(4)) == HEAT_NONE && heat_guard_draw_every(&g) == 1 && !g.sound_off, "E: off: nothing, whatever the reading");
    heat_guard_set_limit(&g, -3);
    CHECK(g.limit_c == 0, "E: a negative limit is off");

    printf("%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
