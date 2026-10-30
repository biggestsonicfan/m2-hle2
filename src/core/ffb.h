/*
 * ffb.h — force feedback for the host (Pinboard #1047): what the cabinet's
 * drive board is doing to the wheel (board/drive_board.h), as a host can play
 * it back: a gamepad's two rumble motors, or a force-feedback wheel's
 * constant, spring and damper effects.
 *
 * Each frontend polls ffb_poll() once a displayed frame and hands the result
 * to its device (main_sdl.c and main.c: SDL rumble and haptics, sdl_pads.h;
 * main_libretro.c: RetroArch's rumble; main_web.c: the page's Gamepad
 * vibration and, on a phone, navigator.vibrate). MCP's get_status shows the
 * state as "drive". It reads the board's
 * last drive command, so it follows a savestate, a replay or rollback as the
 * board does, and is the same in all of them. Only the I/O boards that wire a
 * drive board give one: Daytona's Model 1 I/O board (quirks.model1_io, its
 * `drive`) and Sega Rally's 315-5649 (quirks.io_5649, port E; Pinboard #1082),
 * each decoded in its game's language, and Rail Chase 2's motion seat
 * (quirks.seat_board, port E too; board/seat_board.h). Every other game gets
 * none.
 *
 * A wheel plays Daytona's clutch as a damper, not a friction: a wheel's
 * friction sticks and slips when held against it, a buzz the cabinet's held
 * motor never makes (Pinboard #1447). Its uncentering is a spring the wrong
 * way round (`spring` negative), which turns the wheel on the way it points.
 *
 * Rumble has no spring or damper, so a gamepad feels only the pushes (the
 * strong motor, harder with the level) and the uncentering (the weak motor,
 * the grass). The centring and the clutch Daytona holds all race long stay on
 * a wheel. Sega
 * Rally's force is one steady torque, the wheel's centring most of the race:
 * a wheel plays all of it as a constant force, a pad rumbles only for the
 * hard half (FFB_TORQUE_RUMBLE and up), the kicks of a slide or a crash.
 *
 * Rail Chase 2's seat is air cylinders, not a motor, and a pad cannot tilt:
 * each time the game moves it (valve lines change) the pad gives a jolt that
 * fades over FFB_SEAT_FRAMES board frames, harder the more lines change, on
 * the strong (left) motor for one side of the seat and the weak (right) for
 * the other. A shot kicks it, a hit rocks it, a banked curve leans it; a
 * DualSense, or any pad SDL rumbles, feels them. This one needs the frame of
 * the last change, kept here (host state, not the board's): it counts the
 * board's frames, so a savestate or rewind only starts it afresh.
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
#include "io_5649.h"
#include "model1io.h"
#include "seat_board.h"

typedef struct {
    drive_effect_t effect;
    uint16_t strong, weak;      /* rumble motors, 0-0xFFFF */
    int16_t  push;              /* a wheel's constant force, + right */
    int16_t  spring;            /* its centring, -0x7FFF..0x7FFF; < 0 uncentring */
    uint16_t damper;            /* its clutch, 0-0x7FFF */
} ffb_out_t;

static int g_ffb_gain = 100;

#define FFB_TORQUE_RUMBLE 17     /* Sega Rally: a pad rumbles from this strength (1-32) */
#define FFB_SEAT_FRAMES   12     /* Rail Chase 2: how long a seat jolt lasts */

/* The seat's last command, whether a ride is on, and the last move. */
static struct {
    uint8_t  cmd;
    bool     live;
    uint64_t at;
    int      left, right;
} g_ffb_seat;

/* --ffb-gain N: true when `opt` is it. */
static inline bool ffb_option(const char *opt, const char *val) {
    if (strcmp(opt, "--ffb-gain") != 0) return false;
    int g = (int)strtol(val, NULL, 0);
    g_ffb_gain = g < 0 ? 0 : g > 100 ? 100 : g;
    return true;
}

/* The drive board's command, or 0 (no force) when the game has none. */
static inline uint8_t ffb_drive_cmd(void) {
    if (!g_active_profile) return 0;
    if (g_active_profile->quirks.model1_io) return g_model1io_rom ? g_model1io.drive : 0;
    if (g_active_profile->quirks.io_5649)   return io5649_drive();
    return 0;
}

/* The command in its game's language. */
static inline drive_effect_t ffb_decode(uint8_t cmd) {
    if (g_active_profile && g_active_profile->quirks.io_5649 && !g_active_profile->quirks.model1_io)
        return drive_board_decode_srally(cmd);
    return drive_board_decode(cmd);
}

static inline uint16_t ffb__scale(uint32_t v, uint32_t max) {
    v = v * (uint32_t)g_ffb_gain / 100u;
    return (uint16_t)(v > max ? max : v);
}

/* Rail Chase 2: the jolt of the seat's last move, fading. */
static inline void ffb__seat(ffb_out_t *o, uint8_t cmd) {
    uint64_t now = g_irqt.vbl_count;
    if (cmd != g_ffb_seat.cmd) {
        bool was = g_ffb_seat.live;
        if (seat_board_check(cmd)) g_ffb_seat.live = false;
        else if (seat_board_ride(cmd)) g_ffb_seat.live = true;
        if (was && g_ffb_seat.live) {
            seat_board_motion(g_ffb_seat.cmd, cmd, &g_ffb_seat.left, &g_ffb_seat.right);
            g_ffb_seat.at = now;
        }
        g_ffb_seat.cmd = cmd;
    }
    if (!g_ffb_seat.live || now < g_ffb_seat.at || now - g_ffb_seat.at >= FFB_SEAT_FRAMES) return;
    uint32_t fade = FFB_SEAT_FRAMES - (uint32_t)(now - g_ffb_seat.at);   /* 12..1 */
    int l = g_ffb_seat.left, r = g_ffb_seat.right;
    if (!l && !r) return;
    o->effect.kind  = DRIVE_SEAT;
    o->effect.level = (uint8_t)(((l > r ? l : r) * 7 * fade + 4u * FFB_SEAT_FRAMES - 1u) / (4u * FFB_SEAT_FRAMES));
    o->effect.dir   = (int8_t)(l > r ? -1 : r > l ? +1 : 0);
    if (g_ffb_gain <= 0) return;
    o->strong = ffb__scale(l ? (0x4000u + (uint32_t)l * 0x3000u) * fade / FFB_SEAT_FRAMES : 0u, 0xFFFF);
    o->weak   = ffb__scale(r ? (0x4000u + (uint32_t)r * 0x3000u) * fade / FFB_SEAT_FRAMES : 0u, 0xFFFF);
}

/* What the host should play now. */
static inline ffb_out_t ffb_poll(void) {
    ffb_out_t o;
    memset(&o, 0, sizeof o);
    o.effect = ffb_decode(ffb_drive_cmd());
    if (g_active_profile && g_active_profile->quirks.seat_board) {
        o.effect.kind = DRIVE_NONE;
        ffb__seat(&o, o.effect.cmd);
        return o;
    }
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
        case DRIVE_UNCENTER:
            o.spring = (int16_t)-(int)ffb__scale(lv * 0x1000u + 0x0FFFu, 0x7FFF);   /* -0x0FFF..-0x7FFF */
            o.weak   = ffb__scale(0x4000u + lv * 0x1600u, 0xFFFF);
            o.strong = (uint16_t)(o.weak / 4u);
            break;
        case DRIVE_SPRING:
        case DRIVE_CENTER:
            o.spring = (int16_t)ffb__scale(lv * 0x1000u + (lv ? 0x0FFFu : 0u), 0x7FFF);   /* 0, 0x1FFF..0x7FFF */
            break;
        case DRIVE_CLUTCH:
            o.damper = ffb__scale(lv * 0x1000u + (lv ? 0x0FFFu : 0u), 0x7FFF);
            break;
        case DRIVE_TORQUE:                                             /* lv 1..32 */
            o.push = (int16_t)(o.effect.dir * (int)ffb__scale(lv * 0x0400u - (lv == 32u), 0x7FFF));
            if (lv >= FFB_TORQUE_RUMBLE) {
                o.strong = ffb__scale(0x4000u + (lv - FFB_TORQUE_RUMBLE) * 0x0B00u, 0xFFFF);   /* 0x4000..0xEB00 */
                o.weak   = (uint16_t)(o.strong / 3u);
            }
            break;
        default: break;
    }
    return o;
}

#endif /* M2HLE_FFB_H */
