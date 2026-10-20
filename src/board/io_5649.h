/*
 * io_5649.h — the 315-5649 I/O board's wheel, pedals and gearbox (board-level).
 *
 * The driving games' I/O board (Sega Rally, Daytona on 2A) is an 8-bit device
 * on the 32-bit bus: MAME maps it at 0x01C00000 with umask32 0x00ff00ff, so
 * device register k sits at byte (k / 2) * 4 + (k & 1) * 2. Ports B-D are the
 * IN0-IN2 bytes input.h serves for every game (0x02, 0x04, 0x06); register 0xF
 * (0x1E) is the analog mux: a write picks the channel (data & 7), a read
 * answers that channel's A/D value and steps to the next (MAME 315_5649.cpp).
 *
 * Sega Rally reads three channels: 0 the wheel (a pot centred on 0x80), 1 the
 * accelerator and 2 the brake (0x00 released). The gear lever is IN1 bits 4-6,
 * active high: N 0, 1st 2, 2nd 1, 3rd 6, 4th 5 (MAME `daytona_gearbox_r`).
 *
 * A host has buttons, not pots, so the board is fed the profile's held bits and
 * this file turns them into what the pots would read:
 *   - the wheel turns toward full lock while LEFT or RIGHT is held and back to
 *     centre when neither is, `speed` A/D steps a frame. `center` and `range`
 *     are the pot's adjustment: full lock reads center ± range.
 *   - each pedal reads its released or pressed value.
 *   - UP / DOWN shift the gear lever one gate on each press (N, 1st .. 4th),
 *     and a gear's own button (GAME_INPUT_GEAR_N .. GEAR_4, MAME's GEARS port)
 *     puts it in that gate; either way it stays there until the next shift.
 *
 * The defaults are the cabinet's spec, from the Sega Rally Championship
 * owner's manual (input test, FIG 8.4): HANDLE centre 7DH-83H, full left under
 * 2DH, full right over D3H; ACCEL and BRAKE under 30H released, over C0H
 * pressed. 0x80 ± 0x58 (0x28..0xD8) and pedals 0x20 / 0xE0 sit inside that,
 * the same margins MAME's Daytona gives its wheel and pedals (0x20-0xE0).
 *
 * A host with an analog wheel or pedals (a racing wheel, a pad's stick and
 * triggers; Pinboard #1082) gives their positions instead (io5649_host_analog):
 * the wheel then reads where it is turned, center + steer * range, and a pedal
 * how far it is pressed, between the released and pressed values. A pedal's
 * held bit still floors it, and LEFT / RIGHT still walk the wheel while the
 * host's wheel is centred (a pad's d-pad beside its stick). That position is
 * the host's, not the network's, so, as with a light gun's pointer
 * (lightgun.h), only a board this host's hands drive reads it: one in a
 * session, a follow or a replay keeps to the held bits. A replay or a follow recorded with an analog wheel carries only
 * the held bits, so it plays the race as the d-pad would have.
 *
 * Port E (IO+0x08, register 4) is the drive board's (drive_board.h): the game
 * writes it, the board's buffer keeps the byte (savestated with the rest of
 * the I/O window), and io5649_drive reads it for the host's force feedback.
 *
 * Determinism: the state moves only on the board's own vblank count
 * (g_irqt.vbl_count, brought up to date at the read) and the held bits, so it
 * is board state like any other: a profile registers `g_io5649` with
 * savestate_extra, and a board fed from the network (netplay, a replay) uses
 * the default adjustment whatever this machine's settings are, since the two
 * boards would otherwise read different wheels from the same inputs.
 */
#ifndef M2HLE_IO_5649_H
#define M2HLE_IO_5649_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "eeprom_93c46.h"
#include "irq_timer.h"
#include "memory.h"

/* The manual's spec (see above). */
#define IO5649_WHEEL_CENTER  0x80
#define IO5649_WHEEL_RANGE   0x58
#define IO5649_WHEEL_SPEED   10      /* A/D steps a frame: full lock in ~9 frames */
#define IO5649_PEDAL_OFF     0x20
#define IO5649_PEDAL_ON      0xE0
#define IO5649_STEPS_MAX     64      /* frames caught up at one read */
#define IO5649_AXIS_MAX      32767   /* io5649_host_analog's full scale */

/* The held bits that drive it; a profile fills these from its input map.
 * gear[g] is the button for gate g (0 = N, 1..4). */
typedef struct {
    uint32_t left, right, accel, brake, shift_up, shift_down;
    uint32_t gear[5];
} io5649_controls_t;

/* The pot adjustment: host settings (--wheel-center, --wheel-range,
 * --wheel-speed), not board state. */
typedef struct {
    int center, range, speed;
} io5649_wheel_t;

static io5649_wheel_t g_io5649_wheel = { IO5649_WHEEL_CENTER, IO5649_WHEEL_RANGE, IO5649_WHEEL_SPEED };

/* Board state (savestated by the profile). */
typedef struct {
    int32_t  channel;    /* analog mux */
    int32_t  steer;      /* wheel A/D value */
    int32_t  gear;       /* 0 = N, 1..4 */
    uint32_t prev_held;  /* for the shift edges */
    uint64_t last_vbl;
} io5649_state_t;

static io5649_state_t     g_io5649;
static io5649_controls_t  g_io5649_controls;
/* Set by input_attach: the mask the board reads, and whether it comes from
 * the network (then the adjustment is the spec's). */
static uint32_t (*g_io5649_held_fn)(void);
static int      (*g_io5649_from_net_fn)(void);
/* "Is the board driven from elsewhere" (a session, a follow, a replay), set by
 * emu_thread.h as lightgun.h's. */
static int      (*g_io5649_remote_fn)(void);

/* A host's analog wheel and pedals: steer -32767 (full left) .. 32767 (full
 * right), accel and brake 0 (released) .. 32767 (floored). Host input, read
 * only by a board that is not driven from elsewhere. */
typedef struct {
    volatile int on;          /* an analog device drives them */
    volatile int steer, accel, brake;
} io5649_host_t;

static io5649_host_t g_io5649_host;

/* The board's I/O window (the bus's buffer, set by input.h): port E there is
 * the drive board's last command. */
static const uint8_t *g_io5649_regs;

/* --wheel-center N, --wheel-range N, --wheel-speed N (decimal or 0x hex): true
 * when `opt` is one of them. Out-of-range values are clamped where they are used. */
static inline bool io5649_wheel_option(const char *opt, const char *val) {
    int *field = !strcmp(opt, "--wheel-center") ? &g_io5649_wheel.center
               : !strcmp(opt, "--wheel-range")  ? &g_io5649_wheel.range
               : !strcmp(opt, "--wheel-speed")  ? &g_io5649_wheel.speed : NULL;
    if (!field) return false;
    *field = (int)strtol(val, NULL, 0);
    return true;
}

static inline int io5649_clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* The adjustment in force: this machine's, or the spec's on a networked board. */
static inline io5649_wheel_t io5649_wheel(void) {
    io5649_wheel_t w = g_io5649_wheel;
    if (g_io5649_from_net_fn && g_io5649_from_net_fn())
        w = (io5649_wheel_t){ IO5649_WHEEL_CENTER, IO5649_WHEEL_RANGE, IO5649_WHEEL_SPEED };
    w.center = io5649_clamp(w.center, 0, 255);
    w.range  = io5649_clamp(w.range, 0, 128);
    w.speed  = io5649_clamp(w.speed, 1, 255);
    return w;
}

static inline uint32_t io5649_held(void) { return g_io5649_held_fn ? g_io5649_held_fn() : 0; }

/* The host's wheel and pedals (thread-safe enough: words the board reads whole).
 * Out-of-range values are clamped. on = false hands them back to the held bits. */
static inline void io5649_host_analog(bool on, int steer, int accel, int brake) {
    g_io5649_host.steer = io5649_clamp(steer, -IO5649_AXIS_MAX, IO5649_AXIS_MAX);
    g_io5649_host.accel = io5649_clamp(accel, 0, IO5649_AXIS_MAX);
    g_io5649_host.brake = io5649_clamp(brake, 0, IO5649_AXIS_MAX);
    g_io5649_host.on    = on;
}

static inline bool io5649_host_live(void) {
    return g_io5649_host.on && !(g_io5649_from_net_fn && g_io5649_from_net_fn())
        && !(g_io5649_remote_fn && g_io5649_remote_fn());
}

/* The drive board's command (port E), 0 before there is a board. */
static inline uint8_t io5649_drive(void) { return g_io5649_regs ? g_io5649_regs[0x08] : 0; }

/* Power-on: wheel at the spec's centre (the same on every machine; a local
 * adjustment takes it from there), lever in 1st, mux on channel 0. */
static inline void io5649_reset(void) {
    g_io5649 = (io5649_state_t){ 0 };
    g_io5649.steer = IO5649_WHEEL_CENTER;
    g_io5649.gear  = 1;
}

static inline void io5649_shift(uint32_t held) {
    uint32_t pressed = held & ~g_io5649.prev_held;
    g_io5649.prev_held = held;
    if (pressed & g_io5649_controls.shift_up)   g_io5649.gear = io5649_clamp(g_io5649.gear + 1, 0, 4);
    if (pressed & g_io5649_controls.shift_down) g_io5649.gear = io5649_clamp(g_io5649.gear - 1, 0, 4);
    for (int g = 0; g < 5; g++)
        if (pressed & g_io5649_controls.gear[g]) g_io5649.gear = g;
}

/* Turn the wheel for the frames since the last read. */
static inline void io5649_advance(void) {
    irqt_flush();
    uint64_t now = g_irqt.vbl_count;
    if (now < g_io5649.last_vbl) g_io5649.last_vbl = now;       /* the clock was reset */
    uint64_t steps = now - g_io5649.last_vbl;
    g_io5649.last_vbl = now;
    uint32_t held = io5649_held();
    io5649_shift(held);
    io5649_wheel_t w = io5649_wheel();
    int dir = ((held & g_io5649_controls.right) ? 1 : 0) - ((held & g_io5649_controls.left) ? 1 : 0);
    if (io5649_host_live() && (g_io5649_host.steer || !dir)) {   /* where the host's wheel is turned */
        int d = g_io5649_host.steer * w.range;                     /* rounded to the nearest step */
        d = (d + (d < 0 ? -IO5649_AXIS_MAX / 2 : IO5649_AXIS_MAX / 2)) / IO5649_AXIS_MAX;
        g_io5649.steer = io5649_clamp(w.center + d, 0, 255);
        return;
    }
    if (!steps) return;
    if (steps > IO5649_STEPS_MAX) steps = IO5649_STEPS_MAX;
    int target = w.center + dir * w.range;
    int move = (int)steps * w.speed;
    int s = g_io5649.steer;
    g_io5649.steer = s < target ? io5649_clamp(s + move, s, target) : io5649_clamp(s - move, target, s);
}

/* A pedal: floored while its bit is held, else where the host's pedal is. */
static inline uint8_t io5649_pedal(bool held, int host) {
    if (held) return IO5649_PEDAL_ON;
    if (!io5649_host_live()) return IO5649_PEDAL_OFF;
    return (uint8_t)(IO5649_PEDAL_OFF + (host * (IO5649_PEDAL_ON - IO5649_PEDAL_OFF) + IO5649_AXIS_MAX / 2) / IO5649_AXIS_MAX);
}

static inline uint8_t io5649_analog(int ch) {
    uint32_t held = io5649_held();
    switch (ch) {
        case 0:  return (uint8_t)io5649_clamp(g_io5649.steer, 0, 255);
        case 1:  return io5649_pedal(held & g_io5649_controls.accel, g_io5649_host.accel);
        case 2:  return io5649_pedal(held & g_io5649_controls.brake, g_io5649_host.brake);
        default: return 0x00;
    }
}

/* The gear lever's IN1 bits (bits 0-3 and 7 read high, as MAME's port does). */
static inline uint8_t io5649_gearbox(void) {
    static const uint8_t gate[5] = { 0, 2, 1, 6, 5 };
    return (uint8_t)(0x8F | gate[io5649_clamp(g_io5649.gear, 0, 4)] << 4);
}

static inline uint8_t io5649_read_byte(mem_region_t *r, uint32_t off) {
    uint32_t held = io5649_held();
    switch (off) {
        case 0x02: {                                  /* IN0 system */
            uint8_t in0 = (uint8_t)~(held & 0xFFu);
            /* Port A bit 0 switches IN0's top half to the EEPROM's
             * (eeprom_93c46.h). Sega Rally clocks its EEPROM with the bit up,
             * and a START (bit 6) held then took it to the test menu. */
            return eeprom_in0(in0);
        }
        case 0x04: io5649_advance(); return io5649_gearbox();
        case 0x06: return 0x00;                       /* IN2: hand brake, unused */
        case 0x1E: {
            io5649_advance();
            uint8_t v = io5649_analog(g_io5649.channel);
            g_io5649.channel = (g_io5649.channel + 1) & 7;
            return v;
        }
        default:   if (off >= IO_BOARD_SPAN) return 0;       /* constants.h */
                   return r->data && off < r->size ? r->data[off] : 0xFFu;
    }
}

static uint32_t io5649_read_cb(mem_region_t *r, uint32_t addr, int size) {
    uint32_t off = addr - r->base, v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)io5649_read_byte(r, off + (uint32_t)i) << (8 * i);
    return v;
}

static void io5649_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    uint32_t off = addr - r->base;
    for (int i = 0; i < size; i++) {
        uint8_t b = (uint8_t)(val >> (8 * i));
        if (off + (uint32_t)i == 0x1E) g_io5649.channel = b & 7;
        if (off + (uint32_t)i == 0x00) eeprom_port_a(b);
        if (r->data && off + (uint32_t)i < r->size) r->data[off + (uint32_t)i] = b;
    }
}

#endif /* M2HLE_IO_5649_H */
