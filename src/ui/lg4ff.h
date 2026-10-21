/*
 * lg4ff.h — a Logitech G25 without Logitech's software (Pinboard #1370), for
 * the front ends that have SDL: the desktop (main.c) and the handheld
 * (main_sdl.c). The G27, the Driving Force GT and the Driving Force Pro take
 * the same commands and get the same.
 *
 * G Hub does not drive a G25, and without Logitech Gaming Software 5.10 (or
 * the old Profiler) Windows sees a Driving Force EX: 200 degrees, the pedals
 * on one axis, the shifter and the clutch missing, and no force (no
 * DirectInput effect driver). What that software does to fix it was read from
 * its drivers (core/lg4ff_proto.h, LOGITECH.md); here it is done through SDL's
 * hidapi:
 *
 *   - A wheel still in compatibility mode is sent its mode switch, once. It
 *     drops off the bus and comes back as itself (SDL lists it again, a wheel
 *     sdl_pads.h plays like any other).
 *   - The native wheel is given its turning range (--g25-range N degrees, 270
 *     by default as the cabinets' wheels turn; 0 leaves it as it is).
 *   - When SDL has no haptic for the wheel (Windows without Logitech's driver),
 *     the drive board's force (ffb.h) goes to it: the push as its constant
 *     force, with the shake on top; the centring as the wheel's own spring;
 *     the friction in its friction slot. A report goes only when it changes.
 *     Where SDL has the haptic (Linux's hid-lg4ff, Windows with LGS), the
 *     force stays with sdl_pads.h.
 *
 * Linux's kernel driver already switches the mode and gives the force; there
 * the wheel's hidraw node needs a udev rule to open (LOGITECH.md), and without
 * one this says so once and does nothing else.
 *
 * The force is let go (none, no spring, no friction) when the window loses the
 * focus or the run ends (lg4ff_release, lg4ff_shutdown).
 *
 * Options: --g25 off (lg4ff_option) leaves every Logitech wheel alone;
 * --g25-range N.
 */
#ifndef M2HLE_LG4FF_H
#define M2HLE_LG4FF_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "ffb.h"
#include "lg4ff_proto.h"
#include "log.h"

#define LG4FF_SWITCHED_MAX 4
#define LG4FF_SHAKE_MS     30   /* half the period sdl_pads.h gives its sine */

static bool g_lg4ff_on = true;
static int  g_lg4ff_range = 270;

static struct {
    bool           hid;          /* SDL_hid_init done */
    SDL_hid_device *dev;         /* the native wheel */
    lg4ff_model_t  model;
    Uint32         changes;
    Uint64         looked_ms;
    bool           warned;
    char           switched[LG4FF_SWITCHED_MAX][256];   /* paths sent the mode switch */
    int            nswitched;
    /* What the wheel plays: the force byte (0x80 none), the spring and the
     * friction (0 none), as it is when it is opened. */
    int            force, spring, friction;
} g_lg4ff;

/* --g25 on|off, --g25-range N: true when `opt` is one. */
static inline bool lg4ff_option(const char *opt, const char *val) {
    if (!strcmp(opt, "--g25")) {
        g_lg4ff_on = !(!strcmp(val, "off") || !strcmp(val, "0"));
        return true;
    }
    if (!strcmp(opt, "--g25-range")) {
        int r = (int)strtol(val, NULL, 0);
        g_lg4ff_range = r <= 0 ? 0 : lg4ff_clamp_range(r);
        return true;
    }
    return false;
}

static void lg4ff__close(void) {
    if (g_lg4ff.dev) SDL_hid_close(g_lg4ff.dev);
    g_lg4ff.dev = NULL;
    g_lg4ff.force = 0x80;
    g_lg4ff.spring = g_lg4ff.friction = 0;
}

/* False (and the wheel closed: it went) when the write failed. */
static bool lg4ff__send(const uint8_t *buf) {
    if (!g_lg4ff.dev) return false;
    if (SDL_hid_write(g_lg4ff.dev, buf, LG4FF_REPORT) >= 0) return true;
    LOG_WARN("lg4ff: write failed (%s); the wheel is closed", SDL_GetError());
    lg4ff__close();
    return false;
}

static bool lg4ff__switched(const char *path) {
    for (int i = 0; i < g_lg4ff.nswitched; i++) if (!strcmp(g_lg4ff.switched[i], path)) return true;
    return false;
}

#if defined(__linux__)
#define LG4FF__OPEN_HINT " (a udev rule for 046d gives hidraw to the user; LOGITECH.md)"
#else
#define LG4FF__OPEN_HINT ""
#endif

/* A compat-mode wheel: its mode switch, once a path (a wheel that will not
 * switch is not sent it for ever). */
static void lg4ff__switch(const SDL_hid_device_info *i, lg4ff_model_t m) {
    if (lg4ff__switched(i->path)) return;
    if (g_lg4ff.nswitched < LG4FF_SWITCHED_MAX)
        snprintf(g_lg4ff.switched[g_lg4ff.nswitched++], sizeof g_lg4ff.switched[0], "%s", i->path);
    SDL_hid_device *d = SDL_hid_open_path(i->path);
    if (!d) {
        if (!g_lg4ff.warned)
            LOG_WARN("lg4ff: the %s in compatibility mode would not open: %s%s", lg4ff_name(m), SDL_GetError(),
                     LG4FF__OPEN_HINT);
        g_lg4ff.warned = true;
        return;
    }
    uint8_t r[2][LG4FF_REPORT];
    int n = lg4ff_mode_switch(r, m);
    bool ok = true;
    for (int k = 0; k < n && ok; k++) ok = SDL_hid_write(d, r[k], LG4FF_REPORT) >= 0;
    SDL_hid_close(d);
    if (ok) LOG_INFO("lg4ff: the %s is in compatibility mode (%04X rev %04X); switched to its own", lg4ff_name(m),
                     i->product_id, i->release_number);
    else    LOG_WARN("lg4ff: the %s's mode switch failed: %s", lg4ff_name(m), SDL_GetError());
}

/* Every Logitech wheel: the compat ones switched, the first native one opened. */
static void lg4ff__find(void) {
    SDL_hid_device_info *list = SDL_hid_enumerate(LOGITECH_VID, 0);
    for (SDL_hid_device_info *i = list; i; i = i->next) {
        lg4ff_id_t id = lg4ff_identify(i->product_id, i->release_number);
        if (id.model == LG4FF_NONE) continue;
        if (id.compat) { lg4ff__switch(i, id.model); continue; }
        if (g_lg4ff.dev) continue;
        g_lg4ff.dev = SDL_hid_open_path(i->path);
        if (!g_lg4ff.dev) {
            if (!g_lg4ff.warned)
                LOG_WARN("lg4ff: the %s (%04X) would not open: %s%s", lg4ff_name(id.model), i->product_id,
                         SDL_GetError(), LG4FF__OPEN_HINT);
            g_lg4ff.warned = true;
            continue;
        }
        g_lg4ff.model = id.model;
        g_lg4ff.force = 0x80;
        g_lg4ff.spring = g_lg4ff.friction = 0;
        LOG_INFO("lg4ff: %s (%04X)", lg4ff_name(id.model), i->product_id);
        if (g_lg4ff_range && lg4ff_has_range(id.model)) {
            uint8_t r[LG4FF_REPORT];
            lg4ff_range(r, g_lg4ff_range);
            if (lg4ff__send(r)) LOG_INFO("lg4ff: range %d degrees", g_lg4ff_range);
        }
    }
    SDL_hid_free_enumeration(list);
}

/* The force to `push`, `spring`, `friction`, each only when it changed. */
static void lg4ff__play(int push, int spring, int friction) {
    uint8_t r[3][LG4FF_REPORT];
    int f = lg4ff_force_byte(push);
    if (f != g_lg4ff.force) {
        lg4ff_constant(r[0], push);
        if (!lg4ff__send(r[0])) return;
        g_lg4ff.force = f;
    }
    if (spring != g_lg4ff.spring) {
        int n = lg4ff_autocenter(r, spring);
        for (int k = 0; k < n; k++) if (!lg4ff__send(r[k])) return;
        g_lg4ff.spring = spring;
    }
    if (friction != g_lg4ff.friction) {
        lg4ff_friction(r[0], friction, g_lg4ff.friction > 0);
        if (!lg4ff__send(r[0])) return;
        g_lg4ff.friction = friction;
    }
}

/* Everything let go; the wheel stays open. */
static void lg4ff_release(void) {
    if (g_lg4ff.dev) lg4ff__play(0, 0, 0);
}

/* Once a drawn frame: `o` the force (ffb_poll), `haptic` whether SDL plays it
 * on a wheel already (ffb_wheel_haptic). */
static void lg4ff_update(const ffb_out_t *o, bool haptic) {
    if (!g_lg4ff_on) return;
    if (!g_lg4ff.hid) {
        if (SDL_hid_init() != 0) { g_lg4ff_on = false; return; }
        g_lg4ff.hid = true;
        g_lg4ff.changes = SDL_hid_device_change_count();
        lg4ff__find();
        g_lg4ff.looked_ms = SDL_GetTicks();
    } else {
        /* Look again when a device comes or goes (the switched wheel coming
         * back), or every few seconds where SDL cannot tell (count 0). */
        Uint32 ch = SDL_hid_device_change_count();
        Uint64 now = SDL_GetTicks();
        if (ch != g_lg4ff.changes || (!ch && !g_lg4ff.dev && now - g_lg4ff.looked_ms >= 3000)) {
            g_lg4ff.changes = ch;
            g_lg4ff.looked_ms = now;
            lg4ff__find();
        }
    }
    if (!g_lg4ff.dev) return;
    if (haptic) { lg4ff_release(); return; }
    int push = o->push;
    if (o->shake) push += (SDL_GetTicks() / LG4FF_SHAKE_MS & 1) ? o->shake : -(int)o->shake;
    lg4ff__play(push, o->spring, o->friction);
}

/* The run ends: the force let go, the wheel closed. */
static void lg4ff_shutdown(void) {
    lg4ff_release();
    lg4ff__close();
    if (g_lg4ff.hid) SDL_hid_exit();
    g_lg4ff.hid = false;
}

#endif /* M2HLE_LG4FF_H */
