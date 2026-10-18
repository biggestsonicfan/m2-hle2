/*
 * lightgun.h — the Model 2 light-gun interface (board-level; Pinboard #1034).
 *
 * The House of the Dead's guns hang off the 2C's 315-5649 I/O chip through the
 * gun interface board (837-12079: an Altera FLEX and a 315-5338A). MAME
 * (model2.cpp `hotd`, lightgun_mux_r/w) wires it to the chip's second RS-422
 * channel: the game writes a byte to TXD2 to pick what it reads, and reads it
 * back from RXD2. With the chip's 8-bit registers at (k / 2) * 4 + (k & 1) * 2
 * of the 32-bit bus (io_5649.h):
 *
 *   IO+0x14  write  TXD2 (register 0x0A): the mux. 0..7 pick a byte of the
 *                   four 10-bit positions, P1 Y, P1 X, P2 Y, P2 X, low byte
 *                   first; 8 and up the off-screen byte.
 *   IO+0x18  read   RXD2 (register 0x0C): that byte.
 *   IO+0x1A  read   the RS-422 status (register 0x0D): 0x0C, both receive
 *                   buffers full and both transmit buffers empty, as MAME
 *                   answers it.
 *   IO+0x04  read   IN1: bit 0 P1 trigger, bit 1 P2 trigger, active low; the
 *                   rest, and IN2, read high (MAME `vcop2` ports).
 *
 * The off-screen byte is 0xFC with bit 0 for P1 and bit 1 for P2 set while
 * that gun sees no picture: a shot there reloads. MAME raises it within 5 % of
 * a gun's range of either edge; so does this, and a gun's reload button holds
 * it up and pulls the trigger, which is the shot off the screen a player would
 * make.
 *
 * Where a gun points is board state, moved by held bits like the wheel of
 * io_5649.h: the gun's up/down/left/right walk it MAME's key steps a frame
 * (13 across, 10 down), on the board's own vblank count, so netplay, replays
 * and savestates carry it with no word on the wire beyond the pad's. A host
 * with a pointer (a mouse) aims it directly instead (lightgun_point): that
 * position is the host's, not the network's, so a board fed from the network
 * ignores it and keeps to the pad's aim. So does a board a follow or a
 * replay drives: their records hold the pad's word, not the pointer.
 */
#ifndef M2HLE_LIGHTGUN_H
#define M2HLE_LIGHTGUN_H

#include <stdbool.h>
#include <stdint.h>

#include "irq_timer.h"

#define LIGHTGUN_PLAYERS    2
#define LIGHTGUN_STEP_X     13     /* MAME PORT_KEYDELTA: A/D steps a frame */
#define LIGHTGUN_STEP_Y     10
#define LIGHTGUN_STEPS_MAX  64     /* frames caught up at one read */

/* One gun's range: the A/D values at the picture's left/right and top/bottom
 * edges, and where it rests (MAME's PORT_MINMAX and default). */
typedef struct {
    int x_min, x_max, x_rest, y_min, y_max, y_rest;
} lightgun_range_t;

/* The held bits that work one gun (from the profile's quirks). */
typedef struct {
    uint32_t up, down, left, right, trigger, reload;
} lightgun_controls_t;

/* Board state (savestated by the profile). */
typedef struct {
    int32_t  mux;
    int32_t  x[LIGHTGUN_PLAYERS], y[LIGHTGUN_PLAYERS];
    uint64_t last_vbl;
} lightgun_state_t;

/* A host pointer's aim: 0..1 across the picture, or off it. Host input, read
 * only by a board that is not fed from the network. */
typedef struct {
    volatile int   on;        /* a pointer drives this gun */
    volatile int   offscreen; /* it points outside the picture */
    volatile float x, y;
} lightgun_pointer_t;

static lightgun_state_t    g_lightgun;
static lightgun_range_t    g_lightgun_range[LIGHTGUN_PLAYERS];
static lightgun_controls_t g_lightgun_controls[LIGHTGUN_PLAYERS];
static lightgun_pointer_t  g_lightgun_pointer[LIGHTGUN_PLAYERS];
/* The held bits, set by input_attach as io_5649.h's; and "is the board
 * driven from elsewhere" (a session, a follow, a replay), set by emu_thread.h,
 * which sees all three. */
static uint32_t (*g_lightgun_held_fn)(void);
static int      (*g_lightgun_remote_fn)(void);

static inline int lightgun_clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline uint32_t lightgun_held(void) { return g_lightgun_held_fn ? g_lightgun_held_fn() : 0; }
static inline bool lightgun_remote(void) { return g_lightgun_remote_fn && g_lightgun_remote_fn(); }

/* Power-on: both guns at rest, the mux on P1 Y. */
static inline void lightgun_reset(void) {
    g_lightgun = (lightgun_state_t){ 0 };
    for (int p = 0; p < LIGHTGUN_PLAYERS; p++) {
        g_lightgun.x[p] = g_lightgun_range[p].x_rest;
        g_lightgun.y[p] = g_lightgun_range[p].y_rest;
    }
}

/* The host's pointer (thread-safe enough: four words the board reads whole). */
static inline void lightgun_point(int player, bool on, bool offscreen, float x, float y) {
    if (player < 0 || player >= LIGHTGUN_PLAYERS) return;
    lightgun_pointer_t *pt = &g_lightgun_pointer[player];
    pt->x = x; pt->y = y; pt->offscreen = offscreen; pt->on = on;
}

static inline bool lightgun_pointer_live(int p) {
    return g_lightgun_pointer[p].on && !lightgun_remote();
}

/* Where gun `p` points, 0..1 across the picture (for a crosshair). */
static inline void lightgun_aim(int p, float *x, float *y) {
    const lightgun_range_t *r = &g_lightgun_range[p];
    float w = (float)(r->x_max - r->x_min), h = (float)(r->y_max - r->y_min);
    *x = w > 0 ? (float)(g_lightgun.x[p] - r->x_min) / w : 0.5f;
    *y = h > 0 ? (float)(g_lightgun.y[p] - r->y_min) / h : 0.5f;
}

/* Walk the guns for the frames since the last read, or put them where the
 * host's pointers are. */
static inline void lightgun_advance(void) {
    irqt_flush();
    uint64_t now = g_irqt.vbl_count;
    if (now < g_lightgun.last_vbl) g_lightgun.last_vbl = now;    /* the clock was reset */
    uint64_t steps = now - g_lightgun.last_vbl;
    g_lightgun.last_vbl = now;
    if (steps > LIGHTGUN_STEPS_MAX) steps = LIGHTGUN_STEPS_MAX;
    uint32_t held = lightgun_held();
    for (int p = 0; p < LIGHTGUN_PLAYERS; p++) {
        const lightgun_range_t    *r = &g_lightgun_range[p];
        const lightgun_controls_t *c = &g_lightgun_controls[p];
        if (lightgun_pointer_live(p)) {
            const lightgun_pointer_t *pt = &g_lightgun_pointer[p];
            g_lightgun.x[p] = r->x_min + (int)(pt->x * (float)(r->x_max - r->x_min) + 0.5f);
            g_lightgun.y[p] = r->y_min + (int)(pt->y * (float)(r->y_max - r->y_min) + 0.5f);
        } else if (steps) {
            int dx = ((held & c->right) ? 1 : 0) - ((held & c->left) ? 1 : 0);
            int dy = ((held & c->down)  ? 1 : 0) - ((held & c->up)   ? 1 : 0);
            g_lightgun.x[p] += dx * LIGHTGUN_STEP_X * (int)steps;
            g_lightgun.y[p] += dy * LIGHTGUN_STEP_Y * (int)steps;
        }
        g_lightgun.x[p] = lightgun_clamp(g_lightgun.x[p], r->x_min, r->x_max);
        g_lightgun.y[p] = lightgun_clamp(g_lightgun.y[p], r->y_min, r->y_max);
    }
}

/* Gun `p` sees no picture: its reload is held, the host's pointer is off the
 * picture, or it points within MAME's 5 % border. */
static inline bool lightgun_offscreen(int p, uint32_t held) {
    const lightgun_range_t *r = &g_lightgun_range[p];
    if (held & g_lightgun_controls[p].reload) return true;
    if (lightgun_pointer_live(p) && g_lightgun_pointer[p].offscreen) return true;
    int bx = (r->x_max - r->x_min) * 5 / 100, by = (r->y_max - r->y_min) * 5 / 100;
    return g_lightgun.x[p] <= r->x_min + bx || g_lightgun.x[p] >= r->x_max - bx
        || g_lightgun.y[p] <= r->y_min + by || g_lightgun.y[p] >= r->y_max - by;
}

/* IN1: the triggers, active low; reload pulls the trigger too. */
static inline uint8_t lightgun_in1(uint32_t held) {
    uint8_t v = 0xFF;
    for (int p = 0; p < LIGHTGUN_PLAYERS; p++)
        if (held & (g_lightgun_controls[p].trigger | g_lightgun_controls[p].reload)) v &= (uint8_t)~(1u << p);
    return v;
}

/* RXD2: the byte the mux picks. */
static inline uint8_t lightgun_data(void) {
    lightgun_advance();
    int m = g_lightgun.mux;
    if (m >= 8) {
        uint32_t held = lightgun_held();
        uint8_t v = 0xFC;
        for (int p = 0; p < LIGHTGUN_PLAYERS; p++) if (lightgun_offscreen(p, held)) v |= (uint8_t)(1u << p);
        return v;
    }
    int p = (m >> 2) & 1;                       /* 0..3 P1, 4..7 P2 */
    int v = (m & 2) ? g_lightgun.x[p] : g_lightgun.y[p];
    return (uint8_t)((m & 1) ? (v >> 8) & 0x03 : v & 0xFF);
}

#endif /* M2HLE_LIGHTGUN_H */
