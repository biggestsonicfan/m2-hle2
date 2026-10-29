/*
 * lightgun_host.h — a host pointer as a light gun, for every front end
 * (Pinboard #1034, #1040).
 *
 * For a profile with quirks.lightgun (The House of the Dead): a pointer over
 * the game's picture aims a gun (lightgun_point), its trigger is the player's
 * B1 and its reload the player's B2, which lightgun.h makes an off-screen shot.
 * A pointer off the picture points the gun off it, so a shot there reloads too,
 * as on the cabinet. The desktop's mouse (lightgun_ui.h), the web page's mouse
 * and touches (main_web.c), RetroArch's light gun (main_libretro.c) and SDL's
 * mouse and touches (main_sdl.c) all come through here.
 *
 * The crosshairs are drawn where the board's guns point (lightgun_aim), not
 * where the pointer is: in a session or a replay that is the pad's aim.
 * lightgun_host_crosshairs gives them as solid bars in output pixels, for the
 * front ends that draw rectangles (ps3ui's canvas, sokol_gl); the desktop's
 * ImGui draws its own.
 *
 * The reticle is the player's (Pinboard #1496): a ring, a cross, a dot, a box,
 * a picture of their own (a PNG, the desktop's ImGui only; the bars draw the
 * cross for it) or none, at 50..300 % of its size (--gun-reticle STYLE|FILE,
 * --gun-reticle-size P). Host only, like the flash: the board never sees it.
 *
 * The flash (Pinboard #1045), off unless asked for (--gun-flash N): the
 * picture goes white for N drawn frames when a gun's trigger is pulled, as a
 * light gun that reads the screen (a Sinden, say) wants. It is the host's
 * picture only: the board and the A/V stream never see it, and it is no
 * netplay change. The pull is the board's trigger bit, either gun's, so a
 * pad's B1, a replay's and the other player's in a session flash too.
 *
 * No ImGui, no sokol: only the board's input.
 */
#ifndef M2HLE_LIGHTGUN_HOST_H
#define M2HLE_LIGHTGUN_HOST_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_profile.h"
#include "input.h"
#include "lightgun.h"

#define LIGHTGUN_HOST_TRIGGER 0
#define LIGHTGUN_HOST_RELOAD  1

static bool s_lightgun_host_down[LIGHTGUN_PLAYERS][2];   /* trigger, reload held by a pointer */

static inline bool lightgun_host_on(void) {
    return g_active_profile && g_active_profile->quirks.lightgun;
}

/* Gun `p` at (x, y) in a picture at (rx, ry, rw, rh), the same units (output
 * pixels, say). Outside the picture is off-screen. */
static inline void lightgun_host_aim(int p, float x, float y, float rx, float ry, float rw, float rh) {
    if (!lightgun_host_on() || rw <= 0.0f || rh <= 0.0f) return;
    float fx = (x - rx) / rw, fy = (y - ry) / rh;
    bool off = fx < 0.0f || fx > 1.0f || fy < 0.0f || fy > 1.0f;
    lightgun_point(p, true, off, fx, fy);
}

/* Gun `p` back to its pad (a key or a d-pad walked it), or the pointer gone. */
static inline void lightgun_host_release_aim(int p) {
    if (p >= 0 && p < LIGHTGUN_PLAYERS) lightgun_point(p, false, false, 0.5f, 0.5f);
}

/* The pointer left the window: the gun sees no picture. */
static inline void lightgun_host_aim_away(int p) {
    if (p >= 0 && p < LIGHTGUN_PLAYERS) lightgun_point(p, true, true, 0.5f, 0.5f);
}

/* A pointer's trigger (LIGHTGUN_HOST_TRIGGER) or reload, held or let go:
 * the player's B1 or B2, counted with the pad's (input_action_down). */
static inline void lightgun_host_button(int p, int which, bool down) {
    if (p < 0 || p >= LIGHTGUN_PLAYERS || which < 0 || which > 1) return;
    static const int act[LIGHTGUN_PLAYERS][2] = {
        { GAME_INPUT_P1_B1, GAME_INPUT_P1_B2 }, { GAME_INPUT_P2_B1, GAME_INPUT_P2_B2 },
    };
    const int a = act[p][which];
    bool *held = &s_lightgun_host_down[p][which];
    /* input_release_all (a menu came up, focus went) drops the hold under us:
     * then the flag is stale and a press must count again. */
    if (down) {
        if ((*held && g_input_holds[a] > 0) || !lightgun_host_on()) return;
        *held = true;
        input_action_down(a);
    } else {
        if (!*held) return;
        *held = false;
        if (g_input_holds[a] > 0) input_action_up(a);
    }
}

/* Let go of both pointers' buttons (focus lost, a game unloaded). */
static inline void lightgun_host_release_all(void) {
    for (int p = 0; p < LIGHTGUN_PLAYERS; p++) {
        lightgun_host_button(p, LIGHTGUN_HOST_TRIGGER, false);
        lightgun_host_button(p, LIGHTGUN_HOST_RELOAD, false);
    }
}

/* Gun `p` worth a crosshair: P1 always, P2 once it has left its rest. */
static inline bool lightgun_host_shown(int p) {
    if (p == 0) return true;
    return g_lightgun_pointer[p].on || g_lightgun.x[p] != g_lightgun_range[p].x_rest
        || g_lightgun.y[p] != g_lightgun_range[p].y_rest;
}

/* The reticle's shape. RING is the desktop's circle and cross; the bars,
 * which have no circle, draw it as the cross. */
enum {
    LIGHTGUN_RETICLE_RING, LIGHTGUN_RETICLE_CROSS, LIGHTGUN_RETICLE_DOT, LIGHTGUN_RETICLE_BOX,
    LIGHTGUN_RETICLE_IMAGE, LIGHTGUN_RETICLE_OFF, LIGHTGUN_RETICLES
};

static const char *const k_lightgun_reticle_names[LIGHTGUN_RETICLES] = {
    "ring", "cross", "dot", "box", "image", "off",
};

#define LIGHTGUN_RETICLE_SIZE_MIN 50
#define LIGHTGUN_RETICLE_SIZE_MAX 300

static int      g_lightgun_reticle      = LIGHTGUN_RETICLE_RING;
static int      g_lightgun_reticle_size = 100;      /* percent */
static bool     g_lightgun_reticle_tint = true;     /* an image in the player's colour */
static char     g_lightgun_reticle_image[512];      /* the PNG for LIGHTGUN_RETICLE_IMAGE */
static uint32_t g_lightgun_reticle_gen;             /* bumped when the image's path changes */
static bool     g_lightgun_reticle_cli;             /* the command line chose: no saved choice */

/* A style's name, or -1. */
static inline int lightgun_reticle_parse(const char *s) {
    for (int i = 0; i < LIGHTGUN_RETICLES; i++)
        if (!strcmp(s, k_lightgun_reticle_names[i])) return i;
    if (!strcmp(s, "none")) return LIGHTGUN_RETICLE_OFF;
    return -1;
}

/* The reticle's picture is `path` (NULL or "" lets it go). */
static inline void lightgun_reticle_set_image(const char *path) {
    size_t n = path ? strlen(path) : 0;
    if (n >= sizeof g_lightgun_reticle_image) n = sizeof g_lightgun_reticle_image - 1;
    if (n) memcpy(g_lightgun_reticle_image, path, n);
    g_lightgun_reticle_image[n] = '\0';
    g_lightgun_reticle_gen++;
}

static inline int lightgun_reticle_clamp_size(int pc) {
    return pc < LIGHTGUN_RETICLE_SIZE_MIN ? LIGHTGUN_RETICLE_SIZE_MIN
         : pc > LIGHTGUN_RETICLE_SIZE_MAX ? LIGHTGUN_RETICLE_SIZE_MAX : pc;
}

#define LIGHTGUN_HOST_FLASH_MAX 8

static int  g_lightgun_flash;              /* frames of white a pull draws; 0 = off */
static int  s_lightgun_flash_left;
static bool s_lightgun_flash_was[LIGHTGUN_PLAYERS];

/* --gun-flash N (0..8 frames; "on" is 2, "off" 0), --gun-border P (the
 * off-screen border, 0..5 % of the picture, lightgun.h; "mame" is 5),
 * --gun-reticle ring|cross|dot|box|off|FILE.png or --gun-reticle-size P
 * (50..300 %): true when `opt` is one. */
static inline bool lightgun_host_option(const char *opt, const char *val) {
    if (!strcmp(opt, "--gun-reticle")) {
        int st = lightgun_reticle_parse(val);
        if (st < 0) {                                  /* a file */
            lightgun_reticle_set_image(val);
            st = LIGHTGUN_RETICLE_IMAGE;
        }
        g_lightgun_reticle = st;
        g_lightgun_reticle_cli = true;
        return true;
    }
    if (!strcmp(opt, "--gun-reticle-size")) {
        g_lightgun_reticle_size = lightgun_reticle_clamp_size((int)strtol(val, NULL, 0));
        g_lightgun_reticle_cli = true;
        return true;
    }
    if (!strcmp(opt, "--gun-border")) {
        double pc = !strcmp(val, "mame") ? 5.0 : strtod(val, NULL);
        g_lightgun_border = lightgun_clamp((int)(pc * 10.0 + 0.5), 0, LIGHTGUN_BORDER_DEF);
        return true;
    }
    if (strcmp(opt, "--gun-flash") != 0) return false;

    int n = !strcmp(val, "on") ? 2 : !strcmp(val, "off") ? 0 : (int)strtol(val, NULL, 0);
    g_lightgun_flash = n < 0 ? 0 : n > LIGHTGUN_HOST_FLASH_MAX ? LIGHTGUN_HOST_FLASH_MAX : n;
    if (!g_lightgun_flash) s_lightgun_flash_left = 0;
    return true;
}

/* Once a drawn frame: true when this one flashes. A trigger pulled starts N. */
static inline bool lightgun_host_flash_tick(void) {
    if (!lightgun_host_on()) return false;
    uint32_t held = lightgun_held();
    for (int p = 0; p < LIGHTGUN_PLAYERS; p++) {
        bool down = (held & g_lightgun_controls[p].trigger) != 0;
        if (down && !s_lightgun_flash_was[p] && g_lightgun_flash) s_lightgun_flash_left = g_lightgun_flash;
        s_lightgun_flash_was[p] = down;
    }
    if (s_lightgun_flash_left <= 0) return false;
    s_lightgun_flash_left--;
    return true;
}

/* One solid bar of a crosshair, output pixels (x0, y0 top left). */
typedef struct {
    float    x0, y0, x1, y1;
    uint32_t rgb;     /* 0xRRGGBB */
    float    a;
} lightgun_host_bar_t;

#define LIGHTGUN_HOST_BARS (LIGHTGUN_PLAYERS * 8 + 1)

/* The reticle's bars around (cx, cy), grown by e for the outline: a dot,
 * a box or a cross (arm long, gap from the middle, t thick). Returns how many. */
static inline int lightgun_host_shape(float cx, float cy, float e, float arm, float gap, float t, float b[4][4]) {
    if (g_lightgun_reticle == LIGHTGUN_RETICLE_DOT) {
        float d = t * 1.5f;
        const float q[4] = { cx - d - e, cy - d - e, cx + d + e, cy + d + e };
        memcpy(b[0], q, sizeof q);
        return 1;
    }
    if (g_lightgun_reticle == LIGHTGUN_RETICLE_BOX) {
        float r = arm * 0.7f;
        const float q[4][4] = {
            { cx - r - e, cy - r - e, cx + r + e, cy - r + 2 * t + e },
            { cx - r - e, cy + r - 2 * t - e, cx + r + e, cy + r + e },
            { cx - r - e, cy - r - e, cx - r + 2 * t + e, cy + r + e },
            { cx + r - 2 * t - e, cy - r - e, cx + r + e, cy + r + e },
        };
        memcpy(b, q, sizeof q);
        return 4;
    }
    const float q[4][4] = {
        { cx - arm - e, cy - t - e, cx - gap + e, cy + t + e },
        { cx + gap - e, cy - t - e, cx + arm + e, cy + t + e },
        { cx - t - e, cy - arm - e, cx + t + e, cy - gap + e },
        { cx - t - e, cy + gap - e, cx + t + e, cy + arm + e },
    };
    memcpy(b, q, sizeof q);
    return 4;
}

/* A reticle's arm length, the gap at its centre and its thickness. */
typedef struct { float arm, gap, t; } lightgun_host_size_t;

/* Player p's reticle at (cx, cy) into out from n: k 0 its outline, 1 the
 * reticle. Returns the new n. */
static inline int lightgun_host_reticle(int p, int k, float cx, float cy, lightgun_host_size_t sz,
                                        lightgun_host_bar_t *out, int n) {
    float e = k ? 0.0f : 1.0f;           /* the outline's margin */
    uint32_t rgb = k ? (p == 0 ? 0xE02020u : 0x3060FFu) : 0x000000u;
    float a = k ? 1.0f : 0.63f;
    float b[4][4];
    int nb = lightgun_host_shape(cx, cy, e, sz.arm, sz.gap, sz.t, b);
    for (int i = 0; i < nb; i++)
        out[n++] = (lightgun_host_bar_t){ b[i][0], b[i][1], b[i][2], b[i][3], rgb, a };
    return n;
}

/* The crosshairs over a picture at (ox, oy, w, h), in the reticle's shape
 * (the cross for a ring or an image; none when it is off), a dark outline
 * under each, P1 red and P2 blue; under them, on a frame that flashes, the
 * whole picture white. Returns how many bars. Call it once a drawn frame: it
 * counts the flash down. */
static inline int lightgun_host_crosshairs(float ox, float oy, float w, float h,
                                           lightgun_host_bar_t out[LIGHTGUN_HOST_BARS]) {
    if (!lightgun_host_on() || w <= 0.0f || h <= 0.0f) return 0;
    float z = (float)g_lightgun_reticle_size / 100.0f;
    float arm = h / 384.0f * 10.0f * z, gap = arm * 0.3f;     /* ~10 board pixels */
    float t = h / 384.0f * 1.5f * (z < 1.0f ? 1.0f : z);
    if (t < 1.0f) t = 1.0f;
    lightgun_host_size_t sz = { arm, gap, t };
    int n = 0;
    if (lightgun_host_flash_tick()) out[n++] = (lightgun_host_bar_t){ ox, oy, ox + w, oy + h, 0xFFFFFFu, 1.0f };
    if (g_lightgun_reticle == LIGHTGUN_RETICLE_OFF) return n;
    for (int p = 0; p < LIGHTGUN_PLAYERS; p++) {
        if (!lightgun_host_shown(p)) continue;
        float fx, fy;
        lightgun_aim(p, &fx, &fy);
        float cx = ox + fx * w, cy = oy + fy * h;
        for (int k = 0; k < 2; k++) n = lightgun_host_reticle(p, k, cx, cy, sz, out, n);
    }
    return n;
}

#endif /* M2HLE_LIGHTGUN_HOST_H */
