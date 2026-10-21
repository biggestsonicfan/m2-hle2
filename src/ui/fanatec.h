/*
 * fanatec.h — a Fanatec wheel base's rev LEDs and display show the racing
 * game's meters (Pinboard #1361), for the front ends that have SDL: the
 * desktop (main.c) and the handheld (main_sdl.c).
 *
 * The force needs nothing of ours: the base is a DirectInput wheel on Windows
 * (Fanatec's driver, FAWForceFeedbackDrv64) and SDL lists it as a wheel, so
 * sdl_pads.h's haptics drive it like any other (on Linux, the hid-fanatecff
 * module gives it the force). What a haptic API has no word for is the rim's
 * lights: here the game's dashboard (game_profile_t.dash) goes to the base as
 * Fanatec's own SDK sends it (core/fanatec_proto.h, FANATEC.md), through
 * SDL's hidapi. The rev LEDs fill from the profile's rpm_lo and flash at the
 * limiter; the display shows the gear, or the speed (--fanatec speed).
 *
 * It opens the first Fanatec base it finds while a game with a dashboard runs,
 * sends a report only when what it shows changes, and gives the LEDs and the
 * display back to the wheel when the game has none, the window loses the
 * focus or the run ends (fanatec_release). On Linux the base's hidraw node
 * needs a udev rule (FANATEC.md); without one it says so once and stays off.
 *
 * Options: --fanatec gear | speed | off (fanatec_option); gear by default.
 */
#ifndef M2HLE_FANATEC_H
#define M2HLE_FANATEC_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "fanatec_proto.h"
#include "game_profile.h"
#include "log.h"
#include "memory.h"

enum { FANATEC_OFF, FANATEC_GEAR, FANATEC_SPEED };
static int g_fanatec_mode = FANATEC_GEAR;

static struct {
    bool           hid;          /* SDL_hid_init done */
    SDL_hid_device *dev;         /* the base's first collection */
    SDL_hid_device *dev64;       /* its third (the 0xFF reports); NULL: none */
    bool           multi;
    Uint32         changes;      /* SDL_hid_device_change_count when last looked */
    Uint64         looked_ms;
    bool           warned;
    /* What the base shows; -1 / "?" = not sent yet. */
    int            mask;
    char           text[4];
    bool           display;      /* the display is ours */
} g_fanatec;

/* --fanatec gear|speed|off: true when `opt` is it. */
static inline bool fanatec_option(const char *opt, const char *val) {
    if (strcmp(opt, "--fanatec") != 0) return false;
    g_fanatec_mode = !strcmp(val, "off") || !strcmp(val, "0") ? FANATEC_OFF
                   : !strcmp(val, "speed") ? FANATEC_SPEED : FANATEC_GEAR;
    return true;
}

static void fanatec__close(void) {
    if (g_fanatec.dev64 && g_fanatec.dev64 != g_fanatec.dev) SDL_hid_close(g_fanatec.dev64);
    if (g_fanatec.dev) SDL_hid_close(g_fanatec.dev);
    g_fanatec.dev = g_fanatec.dev64 = NULL;
}

/* False (and the base closed: it went) when the write failed. */
static bool fanatec__send(const uint8_t *buf, size_t len) {
    SDL_hid_device *d = buf[0] == 0xFF ? g_fanatec.dev64 : g_fanatec.dev;
    if (!d) return true;
    if (SDL_hid_write(d, buf, len) >= 0) return true;
    LOG_WARN("fanatec: write failed (%s); the base is closed", SDL_GetError());
    fanatec__close();
    return false;
}

static bool fanatec__cmd(void (*build)(uint8_t *, bool, bool), bool arg) {
    uint8_t r[FANATEC_REPORT];
    build(r, g_fanatec.multi, arg);
    return fanatec__send(r, sizeof r);
}

/* The base's third collection on Windows, named as the SDK names it from the
 * first: col01 -> col03, and the instance 0000# -> 0002#. Elsewhere one node
 * takes every report. */
static void fanatec__open_64(const char *path) {
    char p[512];
    snprintf(p, sizeof p, "%s", path);
    char *c = SDL_strcasestr(p, "col01");
    if (!c) { g_fanatec.dev64 = g_fanatec.dev; return; }
    c[4] = '3';
    char *n = SDL_strcasestr(p, "0000#");
    if (n) n[3] = '2';
    g_fanatec.dev64 = SDL_hid_open_path(p);
}

/* Said when a base will not open (a macro argument cannot hold an #if). */
#if defined(__linux__)
#define FANATEC__OPEN_HINT " (a udev rule for 0eb7 gives hidraw to the user; FANATEC.md)"
#else
#define FANATEC__OPEN_HINT ""
#endif

/* The first wheel base that opens. */
static void fanatec__find(void) {
    SDL_hid_device_info *list = SDL_hid_enumerate(FANATEC_VID, 0);
    for (SDL_hid_device_info *i = list; i && !g_fanatec.dev; i = i->next) {
        if (!fanatec_is_base(i->product_id)) continue;
        /* Windows lists each collection; the first is the one with the commands. */
        if (SDL_strcasestr(i->path, "col0") && !SDL_strcasestr(i->path, "col01")) continue;
        g_fanatec.dev = SDL_hid_open_path(i->path);
        if (!g_fanatec.dev) {
            if (!g_fanatec.warned)
                LOG_WARN("fanatec: %ls (%04X) would not open: %s%s", i->product_string ? i->product_string : L"base",
                         i->product_id, SDL_GetError(), FANATEC__OPEN_HINT);
            g_fanatec.warned = true;
            continue;
        }
        g_fanatec.multi = fanatec_multi_tlc(i->product_id);
        if (g_fanatec.multi) fanatec__open_64(i->path);
        LOG_INFO("fanatec: %ls (%04X) shows the game's meters", i->product_string ? i->product_string : L"base",
                 i->product_id);
        g_fanatec.mask = -1;
        memcpy(g_fanatec.text, "???", 4);
        g_fanatec.display = false;
        /* A rim with a rev stripe shows the rim's rev bits as revs. */
        fanatec__cmd(fanatec_rev_translator, true);
    }
    SDL_hid_free_enumeration(list);
}

/* The LEDs dark and the display back to the wheel's menu; the base stays
 * open (it shows nothing of ours until fanatec_update has a dashboard). */
static void fanatec_release(void) {
    if (!g_fanatec.dev) return;
    uint8_t r[FANATEC_REPORT64];
    if (g_fanatec.mask) {
        fanatec_base_revs(r, g_fanatec.multi, 0);
        if (!fanatec__send(r, FANATEC_REPORT)) return;
        fanatec_rim_revs(r, g_fanatec.multi, 0);
        if (!fanatec__send(r, FANATEC_REPORT)) return;
        if (g_fanatec.multi && g_fanatec.dev64) {
            fanatec_rgb_revs(r, 0);
            if (!fanatec__send(r, FANATEC_REPORT64)) return;
        }
    }
    if (g_fanatec.display) {
        fanatec_display(r, g_fanatec.multi, "   ");
        if (!fanatec__send(r, FANATEC_REPORT)) return;
        if (!fanatec__cmd(fanatec_display_mode, false)) return;
    }
    g_fanatec.mask = 0;
    memcpy(g_fanatec.text, "   ", 4);
    g_fanatec.display = false;
}

/* Once a drawn frame: the game's meters to the base. */
static void fanatec_update(memory_bus_t *bus) {
    game_dash_t d = { .speed = -1, .gear = -1 };
    bool dash = g_fanatec_mode != FANATEC_OFF && g_active_profile && g_active_profile->dash && bus;
    if (!dash) { fanatec_release(); return; }
    if (!g_fanatec.hid) {
        if (SDL_hid_init() != 0) { g_fanatec_mode = FANATEC_OFF; return; }
        g_fanatec.hid = true;
        g_fanatec.changes = SDL_hid_device_change_count();
        fanatec__find();
        g_fanatec.looked_ms = SDL_GetTicks();
    }
    if (!g_fanatec.dev) {
        /* Look again when a device comes or goes, or every few seconds where
         * SDL cannot tell (change count 0). */
        Uint32 ch = SDL_hid_device_change_count();
        Uint64 now = SDL_GetTicks();
        if (ch == g_fanatec.changes && (ch || now - g_fanatec.looked_ms < 3000)) return;
        g_fanatec.changes = ch;
        g_fanatec.looked_ms = now;
        fanatec__find();
        if (!g_fanatec.dev) return;
    }
    g_active_profile->dash(bus, &d);

    uint8_t r[FANATEC_REPORT64];
    int mask = fanatec_rev_mask(&d, SDL_GetTicks() / 125 & 1);
    if (mask != g_fanatec.mask) {
        fanatec_base_revs(r, g_fanatec.multi, (uint16_t)mask);
        if (!fanatec__send(r, FANATEC_REPORT)) return;
        fanatec_rim_revs(r, g_fanatec.multi, (uint16_t)mask);
        if (!fanatec__send(r, FANATEC_REPORT)) return;
        if (g_fanatec.multi && g_fanatec.dev64) {
            fanatec_rgb_revs(r, (uint16_t)mask);
            if (!fanatec__send(r, FANATEC_REPORT64)) return;
        }
        g_fanatec.mask = mask;
    }

    if (d.on && !g_fanatec.display) {
        if (!fanatec__cmd(fanatec_display_mode, true)) return;
        g_fanatec.display = true;
        memcpy(g_fanatec.text, "???", 4);
    }
    if (!d.on && g_fanatec.display) { fanatec_release(); return; }
    if (!g_fanatec.display) return;
    char text[4];
    fanatec_dash_text(&d, g_fanatec_mode == FANATEC_SPEED, text);
    if (memcmp(text, g_fanatec.text, 3) != 0) {
        fanatec_display(r, g_fanatec.multi, text);
        if (!fanatec__send(r, FANATEC_REPORT)) return;
        memcpy(g_fanatec.text, text, 4);
    }
}

/* The run ends: everything back to the wheel, the base closed. */
static void fanatec_shutdown(void) {
    if (g_fanatec.dev) {
        fanatec_release();
        fanatec__cmd(fanatec_rev_translator, false);
    }
    fanatec__close();
    if (g_fanatec.hid) SDL_hid_exit();
    g_fanatec.hid = false;
}

#endif /* M2HLE_FANATEC_H */
