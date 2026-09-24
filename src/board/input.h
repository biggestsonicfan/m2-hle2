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
#include <stdlib.h>
#include <string.h>

#include "sokol_app.h"

#include "constants.h"
#include "game_profile.h"
#include "log.h"
#include "memory.h"

typedef struct {
    volatile uint32_t held;   /* active-high, 0x500700 bit layout; the host keyboard */

    /* Netplay override. During a lockstepped session the board must see the mask
     * COMPOSED from the two peers' transmitted input words and nothing else — the
     * local keyboard is sampled into a word and then forgotten. Keeping that in a
     * second field rather than writing it over `held` matters: key events arrive
     * on the UI thread at arbitrary moments, so a composed mask left in `held`
     * would be half-overwritten by whatever the player is pressing partway
     * through the emulated frame, on one machine and not the other.
     * net/netplay.h sets these; the read callback below prefers them when
     * `use_net` is set, and hands the board back to the keyboard when it is not. */
    volatile uint32_t net_held;
    volatile int      use_net;
} input_state_t;

static input_state_t g_input = {0};

/* How many host sources hold each action: a key, a pad button, and a combo
 * (below) can all hold Punch at once, and letting go of one must not release
 * the others. Only the host's input thread touches this. */
static uint8_t g_input_holds[GAME_INPUT_COUNT];

/* Combos ("macros", as the Gems Collection and HD ports call them): one host key
 * that holds several actions at once, e.g. B1+B2. Host-side only -- the board
 * sees the same held mask it would if the buttons were pressed together, so
 * netplay transmits it like any other press. `key` is the frontend's own key
 * code (sokol's in main.c); main_sdl.c binds pad buttons its own way and the web
 * page composes combos itself (web/site/m2hle-keys.js, m2hle-pad.js). */
#define INPUT_COMBO_MAX 16
typedef struct { int key; uint32_t acts; } input_combo_t;   /* acts: 1 << GAME_INPUT_* */
static input_combo_t g_input_combos[INPUT_COMBO_MAX];
static int           g_input_combo_count;

/* Let go of everything the host holds; the netplay override is left alone. */
static inline void input_release_all(void) {
    g_input.held = 0;
    for (int a = 0; a < GAME_INPUT_COUNT; a++) g_input_holds[a] = 0;
}

static inline void input_reset(void) { input_release_all(); g_input.net_held = 0; }

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

/* A sokol key code by name, for --macro: a-z, 0-9, f1-f12, kp0-kp9, space,
 * tab, insert, pageup, pagedown, home, end, delete. -1 if unknown. */
static inline int input_keycode_by_name(const char *s) {
    char n[16];
    size_t len = 0;
    for (; s[len] && len < sizeof n - 1; len++) n[len] = (char)(s[len] | 0x20);
    n[len] = '\0';
    if (s[len]) return -1;
    if (len == 1 && n[0] >= 'a' && n[0] <= 'z') return SAPP_KEYCODE_A + (n[0] - 'a');
    if (len == 1 && n[0] >= '0' && n[0] <= '9') return SAPP_KEYCODE_0 + (n[0] - '0');
    if (n[0] == 'f' && len >= 2 && len <= 3) {
        int f = atoi(n + 1);
        if (f >= 1 && f <= 12) return SAPP_KEYCODE_F1 + (f - 1);
    }
    if (len == 3 && n[0] == 'k' && n[1] == 'p' && n[2] >= '0' && n[2] <= '9') return SAPP_KEYCODE_KP_0 + (n[2] - '0');
    static const struct { const char *name; int kc; } named[] = {
        { "space", SAPP_KEYCODE_SPACE }, { "tab", SAPP_KEYCODE_TAB }, { "insert", SAPP_KEYCODE_INSERT },
        { "pageup", SAPP_KEYCODE_PAGE_UP }, { "pagedown", SAPP_KEYCODE_PAGE_DOWN }, { "home", SAPP_KEYCODE_HOME },
        { "end", SAPP_KEYCODE_END }, { "delete", SAPP_KEYCODE_DELETE },
    };
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++)
        if (!strcmp(n, named[i].name)) return named[i].kc;
    return -1;
}

/* Press / release an abstract action (GAME_INPUT_*) — the entry point for any
 * host device: the keyboard below, a gamepad in main_sdl.c. */
static inline void input_action_down(int act) {
    if (act < 0 || act >= GAME_INPUT_COUNT || !g_active_profile) return;
    uint32_t bit = g_active_profile->input.bits[act];
    if (g_input_holds[act] < 255) g_input_holds[act]++;
    if (bit) g_input.held |= bit;
}

/* Released when the last source holding it lets go. An up with no down counted
 * (a key pressed before the window had focus, a bit set over MCP) still clears. */
static inline void input_action_up(int act) {
    if (act < 0 || act >= GAME_INPUT_COUNT || !g_active_profile) return;
    uint32_t bit = g_active_profile->input.bits[act];
    if (g_input_holds[act] > 0 && --g_input_holds[act] > 0) return;
    if (bit) g_input.held &= ~bit;
}

/* ---- Combos ---------------------------------------------------------------- */

/* Parse a combo: '+'-separated b1 b2 b3 b4 up down left right start coin,
 * optionally after "p2:" for player 2 -- "b1+b2", "p2:b1+b2+b3". Returns the
 * action mask, or 0 if the text is not one. */
static inline uint32_t input_combo_parse(const char *s) {
    static const char *const names[] = { "up", "down", "left", "right", "b1", "b2", "b3", "b4", "start", "coin" };
    const int per_player = GAME_INPUT_P2_UP - GAME_INPUT_P1_UP;
    int base = GAME_INPUT_P1_UP;
    if ((s[0] == 'p' || s[0] == 'P') && (s[1] == '1' || s[1] == '2') && s[2] == ':') {
        if (s[1] == '2') base = GAME_INPUT_P2_UP;
        s += 3;
    }
    uint32_t acts = 0;
    while (*s) {
        size_t n = 0;
        while (s[n] && s[n] != '+') n++;
        int found = -1;
        for (int i = 0; i < per_player; i++) {
            size_t len = 0;
            while (names[i][len]) len++;
            bool eq = len == n;
            for (size_t c = 0; eq && c < n; c++) eq = (s[c] | 0x20) == names[i][c];
            if (eq) { found = i; break; }
        }
        if (found < 0) return 0;
        acts |= 1u << (base + found);
        s += n;
        if (*s == '+') s++;
    }
    return acts;
}

/* Bind `key` to a combo; binding the same key again replaces it. */
static inline bool input_combo_bind(int key, uint32_t acts) {
    for (int i = 0; i < g_input_combo_count; i++)
        if (g_input_combos[i].key == key) { g_input_combos[i].acts = acts; return true; }
    if (g_input_combo_count >= INPUT_COMBO_MAX) return false;
    g_input_combos[g_input_combo_count++] = (input_combo_t){ key, acts };
    return true;
}

static inline void input_actions_down(uint32_t acts) {
    for (int a = 0; a < GAME_INPUT_COUNT; a++) if (acts & (1u << a)) input_action_down(a);
}

static inline void input_actions_up(uint32_t acts) {
    for (int a = 0; a < GAME_INPUT_COUNT; a++) if (acts & (1u << a)) input_action_up(a);
}

/* The combo `key` is bound to, or 0. */
static inline uint32_t input_combo_for_key(int key) {
    for (int i = 0; i < g_input_combo_count; i++)
        if (g_input_combos[i].key == key) return g_input_combos[i].acts;
    return 0;
}

static inline void input_key_down(int kc) {
    input_action_down(input_keycode_to_action(kc));
    input_actions_down(input_combo_for_key(kc));
}

static inline void input_key_up(int kc) {
    input_action_up(input_keycode_to_action(kc));
    input_actions_up(input_combo_for_key(kc));
}

/* ---- I/O port serving ---------------------------------------------------- */

/* MMIO read callback for the I/O region. Serves the active-low IN0/IN1/IN2
 * bytes from g_input.held; everything else falls back to the idle buffer
 * (0xFF — e.g. DIP switches). The game's interrupt-driven read_sw consumes
 * these and writes 0x500700/0x500704 itself. */
static uint32_t input_io_read_cb(mem_region_t *r, uint32_t addr, int size) {
    (void)size;
    uint32_t off  = addr - r->base;
    uint32_t held = g_input.use_net ? g_input.net_held : g_input.held;
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
            mem_regions_changed(bus);
            LOG_INFO("input: I/O port read callback attached @ 0x%08X", IO_BASE);
            return;
        }
    }
    LOG_WARN("input: I/O region not found — input inactive");
}

#endif /* INPUT_H */
