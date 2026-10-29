/*
 * lg4ff.h — a Logitech G25 without Logitech's software (Pinboard #1370), for
 * the front ends that have SDL: the desktop (main.c) and the handheld
 * (main_sdl.c). The G27, the G29, the Driving Force GT and the Driving Force
 * Pro take the same commands and get the same (the G29: Pinboard #1419).
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
 *     sdl_pads.h plays like any other). A G29 is switched from either of its
 *     modes, PS3 or PS4.
 *   - The native wheel is given its turning range (--g25-range N degrees, 270
 *     by default as the cabinets' wheels turn; 0 leaves it as it is), and the
 *     Controls window's (lg4ff_set_range). SDL's own lg4ff driver, which takes
 *     the wheel on Linux once its hidraw node can be read, sets 900 degrees on
 *     the wheel's first report: it is told ours first (lg4ff_sdl_env, before
 *     SDL_Init; Pinboard #1439).
 *   - When SDL has no haptic for the wheel (Windows without Logitech's driver),
 *     the drive board's force (ffb.h) goes to it: the push as its constant
 *     force; the centring as the wheel's own spring; the uncentring as a
 *     reversed spring in a slot; the clutch in its damper slot. A report goes
 *     only when it changes.
 *     Where SDL has the haptic (Linux's hid-lg4ff, Windows with LGS), the
 *     force stays with sdl_pads.h.
 *
 * Linux's kernel driver already switches the mode and gives the force; there
 * the wheel's hidraw node needs a udev rule to open (LOGITECH.md), and without
 * one this says so once, and gives the range through the driver's sysfs
 * `range` file if a udev rule lets us write it (lg4ff__sysfs_range).
 *
 * The force is let go (none, no spring, no damper) when the window loses the
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
#ifdef __linux__
#include <dirent.h>
#include <unistd.h>
#endif

#include "ffb.h"
#include "lg4ff_proto.h"
#include "log.h"

#define LG4FF_SWITCHED_MAX 4

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
    /* What the wheel plays: the force byte (0x80 none), the spring (< 0 the
     * uncentring) and the damper (0 none), as it is when it is opened. */
    int            force, spring, damper;
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

/* SDL's lg4ff driver sets the range it finds in SDL_HIDAPI_LG4FF_RANGE when it
 * opens the wheel, 900 without one; ours for it. Before SDL_Init, and again
 * when the range changes (the wheel may be plugged in again). */
static inline void lg4ff_sdl_env(void) {
    char v[16];
    if (!g_lg4ff_on || !g_lg4ff_range) return;
    snprintf(v, sizeof v, "%d", g_lg4ff_range);
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "SDL_HIDAPI_LG4FF_RANGE", v, true);
}

static void lg4ff__close(void) {
    if (g_lg4ff.dev) SDL_hid_close(g_lg4ff.dev);
    g_lg4ff.dev = NULL;
    g_lg4ff.force = 0x80;
    g_lg4ff.spring = g_lg4ff.damper = 0;
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
static void lg4ff__switch(const SDL_hid_device_info *i, lg4ff_id_t id) {
    lg4ff_model_t m = id.model;
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
    int len;
    int n = lg4ff_mode_switch(r, id, &len);
    bool ok = true;
    for (int k = 0; k < n && ok; k++) ok = SDL_hid_write(d, r[k], (size_t)len) >= 0;
    SDL_hid_close(d);
    if (ok) LOG_INFO("lg4ff: the %s is in %s mode (%04X rev %04X); switched to its own", lg4ff_name(m),
                     id.ps4 ? "its PS4" : "compatibility", i->product_id, i->release_number);
    else    LOG_WARN("lg4ff: the %s's mode switch failed: %s", lg4ff_name(m), SDL_GetError());
}

/* Linux, the wheel left to hid-logitech (no hidraw for us): the range through
 * each bound wheel's /sys/bus/hid/drivers/logitech/<dev>/range, root's unless
 * a udev rule opens it (LOGITECH.md); the number of wheels it took. */
static int lg4ff__sysfs_range(int deg) {
    int n = 0;
#ifdef __linux__
    static bool warned;
    const char *dir = "/sys/bus/hid/drivers/logitech";
    DIR *d = opendir(dir);
    if (!d) return 0;
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        char path[320];
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s/range", dir, e->d_name);
        FILE *f = fopen(path, "w");
        if (!f) {
            if (!warned && access(path, F_OK) == 0)
                LOG_WARN("lg4ff: cannot write %s (the udev rule in LOGITECH.md opens it)", path);
            warned |= access(path, F_OK) == 0;
            continue;
        }
        bool ok = fprintf(f, "%d\n", deg) > 0;
        ok &= fclose(f) == 0;
        if (ok) { n++; LOG_INFO("lg4ff: range %d degrees (%s)", deg, path); }
    }
    closedir(d);
#else
    (void)deg;
#endif
    return n;
}

/* Every Logitech wheel: the compat ones switched, the first native one opened. */
static void lg4ff__find(void) {
    SDL_hid_device_info *list = SDL_hid_enumerate(LOGITECH_VID, 0);
    for (SDL_hid_device_info *i = list; i; i = i->next) {
        lg4ff_id_t id = lg4ff_identify(i->product_id, i->release_number);
        if (id.model == LG4FF_NONE) continue;
        if (id.compat) { lg4ff__switch(i, id); continue; }
        if (g_lg4ff.dev) continue;
        g_lg4ff.dev = SDL_hid_open_path(i->path);
        if (!g_lg4ff.dev) {
            if (!g_lg4ff.warned) {
                LOG_WARN("lg4ff: the %s (%04X) would not open: %s%s", lg4ff_name(id.model), i->product_id,
                         SDL_GetError(), LG4FF__OPEN_HINT);
                if (g_lg4ff_range && lg4ff_has_range(id.model)) lg4ff__sysfs_range(g_lg4ff_range);
            }
            g_lg4ff.warned = true;
            continue;
        }
        g_lg4ff.model = id.model;
        g_lg4ff.force = 0x80;
        g_lg4ff.spring = g_lg4ff.damper = 0;
        LOG_INFO("lg4ff: %s (%04X)", lg4ff_name(id.model), i->product_id);
        if (g_lg4ff_range && lg4ff_has_range(id.model)) {
            uint8_t r[LG4FF_REPORT];
            lg4ff_range(r, g_lg4ff_range);
            if (lg4ff__send(r)) LOG_INFO("lg4ff: range %d degrees", g_lg4ff_range);
        }
    }
    SDL_hid_free_enumeration(list);
}

/* The open wheel takes a range command (a G25, a G27, a G29, a DFGT). */
static inline bool lg4ff_range_ok(void) {
    return g_lg4ff.dev && lg4ff_has_range(g_lg4ff.model);
}

/* The open wheel's name; NULL when none is open. */
static inline const char *lg4ff_open_name(void) {
    return g_lg4ff.dev ? lg4ff_name(g_lg4ff.model) : NULL;
}

/* A Logitech wheel was seen that its hidraw node keeps us from (no udev rule). */
static inline bool lg4ff_locked_out(void) { return g_lg4ff.warned && !g_lg4ff.dev; }

/* The turning range, `deg` degrees (40..900): kept for the next wheel, and
 * sent to the open one now (or through Linux's driver); false when there is
 * none to send it to. */
static bool lg4ff_set_range(int deg) {
    g_lg4ff_range = lg4ff_clamp_range(deg);
    if (!g_lg4ff_on) return false;
    lg4ff_sdl_env();
    if (!lg4ff_range_ok()) return lg4ff__sysfs_range(g_lg4ff_range) > 0;
    uint8_t r[LG4FF_REPORT];
    lg4ff_range(r, g_lg4ff_range);
    if (!lg4ff__send(r)) return false;
    LOG_INFO("lg4ff: range %d degrees", g_lg4ff_range);
    return true;
}

/* The force to `push`, `spring`, `damper`, each only when it changed. The
 * centring is the wheel's own spring, the uncentring a slot's (both stopped
 * on the way from one to the other). */
static void lg4ff__play(int push, int spring, int damper) {
    uint8_t r[3][LG4FF_REPORT];
    int f = lg4ff_force_byte(push);
    if (f != g_lg4ff.force) {
        lg4ff_constant(r[0], push);
        if (!lg4ff__send(r[0])) return;
        g_lg4ff.force = f;
    }
    if (spring != g_lg4ff.spring) {
        int was = g_lg4ff.spring;
        if (spring > 0 || was > 0) {
            int n = lg4ff_autocenter(r, spring > 0 ? spring : 0);
            for (int k = 0; k < n; k++) if (!lg4ff__send(r[k])) return;
        }
        if (spring < 0 || was < 0) {
            lg4ff_uncenter(r[0], spring < 0 ? -spring : 0, was < 0);
            if (!lg4ff__send(r[0])) return;
        }
        g_lg4ff.spring = spring;
    }
    if (damper != g_lg4ff.damper) {
        lg4ff_damper(r[0], damper, g_lg4ff.damper > 0);
        if (!lg4ff__send(r[0])) return;
        g_lg4ff.damper = damper;
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
    lg4ff__play(o->push, o->spring, o->damper);
}

/* The run ends: the force let go, the wheel closed. */
static void lg4ff_shutdown(void) {
    lg4ff_release();
    lg4ff__close();
    if (g_lg4ff.hid) SDL_hid_exit();
    g_lg4ff.hid = false;
}

#endif /* M2HLE_LG4FF_H */
