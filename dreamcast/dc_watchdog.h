/* dc_watchdog.h -- a hang stops the Dreamcast with its reason on screen.
 *
 * Not the SH4's watchdog. In watchdog mode it overflows 5.25 ms after the last
 * pet at its slowest clock (KOS's dc/wdt.h gives the overflow period, not a
 * tick), and a board frame takes 20-60 ms. #521 read 5.25 ms as one of 256
 * ticks and armed a power-on reset, so on hardware the console reset during
 * the first frame, booted again and sat on "finding the ROM files" for good
 * (#559). Flycast and redream do not emulate that reset, and redream stops on
 * the WDT's interval mode, so the count is kept by a thread instead.
 *
 * The thread outranks every other and wakes every DC_WD_TICK_MS; a pet clears
 * the count, and DC_WD_LIMIT wakes without one hand the watched thread's PC
 * and PR to the program's handler, which does not return. KOS preempts, so a
 * loop that spins with interrupts on is caught; one with them off is not. */
#ifndef DC_WATCHDOG_H
#define DC_WATCHDOG_H

#include <kos.h>
#include <kos/thread.h>
#include <stdint.h>

#define DC_WD_TICK_MS 100u
#define DC_WD_LIMIT   (2000u / DC_WD_TICK_MS)   /* 2 s without a pet */

typedef void (*dc_wd_fire_t)(uint32_t pc, uint32_t pr);

static volatile uint32_t g_dc_wd_ticks;
static volatile dc_wd_fire_t g_dc_wd_fire;
static kthread_t *g_dc_wd_watched;

static inline void dc_wd_pet(void) { g_dc_wd_ticks = 0; }

static void *dc_wd_thread(void *arg) {
    (void)arg;
    for (;;) {
        thd_sleep(DC_WD_TICK_MS);
        dc_wd_fire_t fire = g_dc_wd_fire;
        if (fire && ++g_dc_wd_ticks >= DC_WD_LIMIT)
            fire(g_dc_wd_watched->context.pc, g_dc_wd_watched->context.pr);
    }
    return NULL;
}

/* Watches the calling thread from now on. */
static void dc_wd_start(dc_wd_fire_t fire) {
    static bool made;
    g_dc_wd_watched = thd_get_current();
    g_dc_wd_ticks = 0;
    g_dc_wd_fire = fire;
    if (made) return;
    kthread_attr_t attr = { .create_detached = true, .prio = 1, .label = "watchdog" };
    made = thd_create_ex(&attr, dc_wd_thread, NULL) != NULL;
}

static void dc_wd_stop(void) { g_dc_wd_fire = NULL; }

#endif
