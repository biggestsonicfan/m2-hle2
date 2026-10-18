/*
 * ffb.h — force feedback for the host (Pinboard #1047): what the cabinet's
 * drive board is doing to the wheel (board/drive_board.h), as a host can play
 * it back: a gamepad's two rumble motors, or a force-feedback wheel's
 * constant, spring and friction effects.
 *
 * Each frontend polls ffb_poll() once a displayed frame and hands the result
 * to its device (main_sdl.c: SDL rumble and haptics; main_libretro.c:
 * RetroArch's rumble; main_web.c: the page's Gamepad vibration and, on a
 * phone, navigator.vibrate). The debugger (main.c) takes no pad, so it has
 * none; MCP's get_status shows the state as "drive". It reads the board's
 * last drive command, so it follows a savestate, a replay or rollback as the
 * board does, and is the same in all of them. Only the I/O board profiles that
 * wire a drive board (quirks.model1_io) give one; every other game gets none.
 *
 * Rumble has no spring or friction, so a gamepad feels only the pushes (the
 * strong motor, harder with the level) and the shake (the weak motor). The
 * centring and friction Daytona holds all race long stay on a wheel.
 *
 * g_ffb_gain (0-100, a host setting: --ffb-gain N, 0 turns it off) scales all
 * of it.
 */
#ifndef M2HLE_FFB_H
#define M2HLE_FFB_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "drive_board.h"
#include "game_profile.h"
#include "model1io.h"

typedef struct {
    drive_effect_t effect;
    uint16_t strong, weak;      /* rumble motors, 0-0xFFFF */
    int16_t  push;              /* a wheel's constant force, + right */
    uint16_t spring, friction;  /* a wheel's coefficients, 0-0x7FFF */
    uint16_t shake;             /* a wheel's sine magnitude, 0-0x7FFF */
} ffb_out_t;

static int g_ffb_gain = 100;

/* --ffb-gain N: true when `opt` is it. */
static inline bool ffb_option(const char *opt, const char *val) {
    if (strcmp(opt, "--ffb-gain") != 0) return false;
    int g = (int)strtol(val, NULL, 0);
    g_ffb_gain = g < 0 ? 0 : g > 100 ? 100 : g;
    return true;
}

/* The drive board's command, or 0 (no force) when the game has none. */
static inline uint8_t ffb_drive_cmd(void) {
    if (!g_active_profile || !g_active_profile->quirks.model1_io || !g_model1io_rom) return 0;
    return g_model1io.drive;
}

static inline uint16_t ffb__scale(uint32_t v, uint32_t max) {
    v = v * (uint32_t)g_ffb_gain / 100u;
    return (uint16_t)(v > max ? max : v);
}

/* What the host should play now. */
static inline ffb_out_t ffb_poll(void) {
    ffb_out_t o;
    memset(&o, 0, sizeof o);
    o.effect = drive_board_decode(ffb_drive_cmd());
    if (g_ffb_gain <= 0) return o;
    uint32_t lv = o.effect.level;
    switch (o.effect.kind) {
        case DRIVE_PUSH: {
            uint32_t f = 0x2000u + (lv + 1u) * 0x0C00u;               /* 0x2C00..0x7FFF */
            o.push   = (int16_t)(o.effect.dir * (int)ffb__scale(f, 0x7FFF));
            o.strong = ffb__scale(0x5000u + lv * 0x1600u, 0xFFFF);    /* 0x5000..0xEA00 */
            o.weak   = (uint16_t)(o.strong / 3u);
            break;
        }
        case DRIVE_VIBRATE:
            o.shake  = ffb__scale(0x1800u + lv * 0x0C00u, 0x7FFF);
            o.weak   = ffb__scale(0x4000u + lv * 0x1600u, 0xFFFF);
            o.strong = (uint16_t)(o.weak / 4u);
            break;
        case DRIVE_SPRING:
        case DRIVE_CENTER:
            o.spring = ffb__scale(lv * 0x1000u + (lv ? 0x0FFFu : 0u), 0x7FFF);   /* 0, 0x1FFF..0x7FFF */
            break;
        case DRIVE_FRICTION:
            o.friction = ffb__scale(lv * 0x1000u + (lv ? 0x0FFFu : 0u), 0x7FFF);
            break;
        default: break;
    }
    return o;
}

#endif /* M2HLE_FFB_H */
