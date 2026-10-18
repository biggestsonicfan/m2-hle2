/*
 * wheel_host.h — a host's analog wheel and pedals for the driving games, for
 * every front end (Pinboard #1082).
 *
 * For a profile with a wheel (quirks.io_5649: Sega Rally; quirks.model1_io:
 * Daytona USA), a racing wheel's or a pad's axes go to the board as
 * io_5649.h's host wheel and pedals (io5649_host_analog): the wheel reads
 * where it is turned, a pedal how far it is pressed. The front ends turn their
 * devices' axes into its scale here: main_sdl.c (a gamepad's left stick and
 * triggers, a racing wheel SDL sees as a joystick), main_web.c (the page's
 * Gamepad API axes and trigger values) and main_libretro.c (RetroArch's
 * analog stick and analog triggers).
 *
 * The board reads them only while no session, follow or replay drives it
 * (io5649_host_live); a front end keeps pressing LEFT / RIGHT and the pedal
 * buttons from the same axes as before, which is what those read.
 *
 * No SDL, no sokol: only the board's input.
 */
#ifndef M2HLE_WHEEL_HOST_H
#define M2HLE_WHEEL_HOST_H

#include <stdbool.h>
#include <stdlib.h>

#include "game_profile.h"
#include "io_5649.h"

#define WHEEL_HOST_STICK_DEAD 2500   /* a pad's stick: the centre it never quite finds */

static inline bool wheel_host_on(void) {
    return g_active_profile && (g_active_profile->quirks.io_5649 || g_active_profile->quirks.model1_io);
}

/* A stick or wheel axis (-32768..32767) as the wheel's -32767..32767, with
 * `dead` around the centre read as the centre and the rest spread over the
 * full turn. */
static inline int wheel_host_steer(int v, int dead) {
    if (v < -IO5649_AXIS_MAX) v = -IO5649_AXIS_MAX;
    int m = abs(v);
    if (m <= dead) return 0;
    if (dead >= IO5649_AXIS_MAX) return 0;
    m = (int)((long)(m - dead) * IO5649_AXIS_MAX / (IO5649_AXIS_MAX - dead));
    return v < 0 ? -m : m;
}

/* A pedal axis (-32768..32767) that reads `rest` released and the far end
 * from it pressed (a wheel's pedals rest at either end, or in the middle), as
 * 0..32767. */
static inline int wheel_host_pedal(int v, int rest) {
    int end = rest > 0 ? -32768 : 32767;
    int span = abs(end - rest), d = abs(v - rest);
    if (span <= 0) return 0;
    if (d > span) d = span;
    return (int)((long)d * IO5649_AXIS_MAX / span);
}

/* The board's wheel and pedals from the host (on = false: back to the held bits). */
static inline void wheel_host_set(bool on, int steer, int accel, int brake) {
    io5649_host_analog(on && wheel_host_on(), steer, accel, brake);
}

#endif /* M2HLE_WHEEL_HOST_H */
