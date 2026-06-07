/*
 * input.h — board-level keyboard → Model 2 I/O port plumbing.
 *
 * AUTHENTIC (interrupt-driven) input path — NOT the "write cooked bits into
 * game RAM" shortcut. The host maintains a 32-bit `held` mask (active-high, in
 * the same bit layout the game assembles at INTERUPT_FLAGS_HELD / 0x500700).
 * We install an MMIO read callback on the I/O region (0x01C00000) that serves
 * the raw, ACTIVE-LOW port bytes the hardware would present:
 *
 *   IO+0x02  IN0 (system): bit0 COIN1, bit1 COIN2, bit2 SERVICE/test,
 *                          bit3 SERVICE1, bit4 START1, bit5 START2
 *   IO+0x04  IN1 (P1): bit0-3 = B1-B4, bit4 DOWN, bit5 UP, bit6 RIGHT, bit7 LEFT
 *   IO+0x06  IN2 (P2): same as P1
 *
 * The game's own read_sw (STF 0x17CC), called from the VsyncScr vblank
 * interrupt, reads these ports (`not r7`) and produces held / momentary itself.
 * Verified against the STF IDA disassembly + MAME model2 input port map.
 *
 * `held` byte layout (== 0x500700): byte0 = IN0, byte1 = IN1, byte2 = IN2, so a
 * served port byte is simply ~(held >> (8*port_index)). The per-game action →
 * bit assignment lives in game_profile_t.input.bits (board-standard for M2).
 */
#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>
#include <stdint.h>

#include "sokol_app.h"

#include "constants.h"
#include "game_profile.h"
#include "log.h"
#include "memory.h"

typedef struct {
    volatile uint32_t held;   /* active-high, 0x500700 bit layout; read by the IO cb */
} input_state_t;

static input_state_t g_input = {0};

static inline void input_reset(void) { g_input.held = 0; }

/* Translate a sokol key code into the abstract action enum, or -1. */
static inline int input_keycode_to_action(int kc) {
    switch (kc) {
        /* P1 */
        case SAPP_KEYCODE_UP:        return GAME_INPUT_P1_UP;
        case SAPP_KEYCODE_DOWN:      return GAME_INPUT_P1_DOWN;
        case SAPP_KEYCODE_LEFT:      return GAME_INPUT_P1_LEFT;
        case SAPP_KEYCODE_RIGHT:     return GAME_INPUT_P1_RIGHT;
        case SAPP_KEYCODE_Z:         return GAME_INPUT_P1_B1;
        case SAPP_KEYCODE_X:         return GAME_INPUT_P1_B2;
        case SAPP_KEYCODE_C:         return GAME_INPUT_P1_B3;
        case SAPP_KEYCODE_V:         return GAME_INPUT_P1_B4;
        case SAPP_KEYCODE_1:         return GAME_INPUT_P1_START;
        case SAPP_KEYCODE_5:         return GAME_INPUT_P1_COIN;
        /* P2 */
        case SAPP_KEYCODE_I:         return GAME_INPUT_P2_UP;
        case SAPP_KEYCODE_K:         return GAME_INPUT_P2_DOWN;
        case SAPP_KEYCODE_J:         return GAME_INPUT_P2_LEFT;
        case SAPP_KEYCODE_L:         return GAME_INPUT_P2_RIGHT;
        case SAPP_KEYCODE_DELETE:    return GAME_INPUT_P2_B1;
        case SAPP_KEYCODE_END:       return GAME_INPUT_P2_B2;
        case SAPP_KEYCODE_PAGE_DOWN: return GAME_INPUT_P2_B3;
        case SAPP_KEYCODE_HOME:      return GAME_INPUT_P2_B4;
        case SAPP_KEYCODE_2:         return GAME_INPUT_P2_START;
        case SAPP_KEYCODE_6:         return GAME_INPUT_P2_COIN;
        /* System */
        case SAPP_KEYCODE_F2:        return GAME_INPUT_SERVICE;
        case SAPP_KEYCODE_F3:        return GAME_INPUT_TEST;
        default:                     return -1;
    }
}

static inline void input_key_down(int kc) {
    int act = input_keycode_to_action(kc);
    if (act < 0 || act >= GAME_INPUT_COUNT || !g_active_profile) return;
    uint32_t bit = g_active_profile->input.bits[act];
    if (bit) g_input.held |= bit;
}

static inline void input_key_up(int kc) {
    int act = input_keycode_to_action(kc);
    if (act < 0 || act >= GAME_INPUT_COUNT || !g_active_profile) return;
    uint32_t bit = g_active_profile->input.bits[act];
    if (bit) g_input.held &= ~bit;
}

/* ---- I/O port serving ---------------------------------------------------- */

/* MMIO read callback for the I/O region. Serves the active-low IN0/IN1/IN2
 * bytes from g_input.held; everything else falls back to the idle buffer
 * (0xFF — e.g. DIP switches). The game's interrupt-driven read_sw consumes
 * these and writes 0x500700/0x500704 itself. */
static uint32_t input_io_read_cb(mem_region_t *r, uint32_t addr, int size) {
    (void)size;
    uint32_t off  = addr - r->base;
    uint32_t held = g_input.held;
    switch (off) {
        case 0x02: return (uint8_t)~(held         & 0xFFu);  /* IN0 system */
        case 0x04: return (uint8_t)~((held >> 8)  & 0xFFu);  /* IN1 P1     */
        case 0x06: return (uint8_t)~((held >> 16) & 0xFFu);  /* IN2 P2     */
        default:   return r->data ? r->data[off] : 0xFFu;    /* DIPs / idle */
    }
}

/* Install the I/O read callback on the bus's I/O region. Call after the ROM is
 * installed (install_fn re-inits the bus). Writes (coin-meter strobe at IO+0x00,
 * IO+0x40) keep hitting the plain buffer — harmless. */
static inline void input_attach(memory_bus_t *bus) {
    for (int i = 0; i < bus->region_count; i++) {
        if (bus->regions[i].base == IO_BASE) {
            bus->regions[i].read_cb = input_io_read_cb;
            LOG_INFO("input: I/O port read callback attached @ 0x%08X", IO_BASE);
            return;
        }
    }
    LOG_WARN("input: I/O region not found — input inactive");
}

#endif /* INPUT_H */
