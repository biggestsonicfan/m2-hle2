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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sokol_app.h"

#include "constants.h"
#include "game_profile.h"
#include "eeprom_93c46.h"
#include "model1io.h"
#include "io_5649.h"
#include "lightgun.h"
#include "log.h"
#include "memory.h"
#include "savestate_reg.h"

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

    /* Players 3 and 4 of a four-player cabinet (quirks.four_player_sides), in
     * `held`'s layout with player 3 on player 1's bits and player 4 on player
     * 2's (input_far_down). Only the host's own pads fill it, and the board
     * reads it only while nothing has taken the input (use_net: netplay,
     * GGPO, a replay, a follow), so every session stays two players and
     * plays the same frames everywhere. */
    volatile uint32_t far_held;
} input_state_t;

static input_state_t g_input = {0};

/* How many host sources hold each action: a key, a pad button, and a combo
 * (below) can all hold Punch at once, and letting go of one must not release
 * the others. Only the host's input thread touches this. */
static uint8_t g_input_holds[GAME_INPUT_COUNT];
static uint8_t g_input_far_holds[GAME_INPUT_COUNT];   /* far_held's, the same way */

/* Combos ("macros", as the Gems Collection and HD ports call them): one host key
 * that holds several actions at once, e.g. B1+B2. Host-side only -- the board
 * sees the same held mask it would if the buttons were pressed together, so
 * netplay transmits it like any other press. `key` is the frontend's own key
 * code (sokol's in main.c); main_sdl.c binds pad buttons its own way and the web
 * page composes combos itself (web/site/m2hle-keys.js, m2hle-pad.js). */
#define INPUT_COMBO_MAX 16
typedef struct { int key; uint32_t acts; } input_combo_t;   /* acts: 1 << GAME_INPUT_* */
/* In a combo's acts: the actions are player 3's (player 1's) or 4's (player
 * 2's) of a four-player cabinet, pressed with input_far_down. */
#define INPUT_COMBO_FAR (1u << 31)
static input_combo_t g_input_combos[INPUT_COMBO_MAX];
static int           g_input_combo_count;

/* Let go of everything the host holds; the netplay override is left alone. */
static inline void input_release_all(void) {
    g_input.held = 0;
    g_input.far_held = 0;
    for (int a = 0; a < GAME_INPUT_COUNT; a++) g_input_holds[a] = g_input_far_holds[a] = 0;
}

static inline void input_reset(void) { input_release_all(); g_input.net_held = 0; }

/* The keyboard's map (Pinboard #1147): up to INPUT_KEY_SLOTS sokol key codes
 * per action, 0 for none. Host-side only, like the combos below: the board
 * sees the actions, so a remap changes nothing a session or replay sends.
 * The desktop's Controls window edits it and keeps it per profile
 * (ui/controls_window.h); input_keys_default puts these back.
 *
 * The defaults are m2emulator 1.1a's, which most players learnt the games on
 * (docs/m2emulator-controls.md, Pinboard #1347): F1 the service button and F2
 * the test switch (9 is MAME's service button too), and on a racing cabinet
 * INPUT_KEYS_DRIVING over the rest. */
#define INPUT_KEY_SLOTS 2
#define INPUT_KEYS_DEFAULT {                                                         \
    [GAME_INPUT_P1_UP]    = { SAPP_KEYCODE_UP },     [GAME_INPUT_P1_DOWN]  = { SAPP_KEYCODE_DOWN },      \
    [GAME_INPUT_P1_LEFT]  = { SAPP_KEYCODE_LEFT },   [GAME_INPUT_P1_RIGHT] = { SAPP_KEYCODE_RIGHT },     \
    [GAME_INPUT_P1_B1]    = { SAPP_KEYCODE_Z },      [GAME_INPUT_P1_B2]    = { SAPP_KEYCODE_X },         \
    [GAME_INPUT_P1_B3]    = { SAPP_KEYCODE_C },      [GAME_INPUT_P1_B4]    = { SAPP_KEYCODE_V },         \
    [GAME_INPUT_P1_START] = { SAPP_KEYCODE_1 },      [GAME_INPUT_P1_COIN]  = { SAPP_KEYCODE_5 },         \
    [GAME_INPUT_P2_UP]    = { SAPP_KEYCODE_I },      [GAME_INPUT_P2_DOWN]  = { SAPP_KEYCODE_K },         \
    [GAME_INPUT_P2_LEFT]  = { SAPP_KEYCODE_J },      [GAME_INPUT_P2_RIGHT] = { SAPP_KEYCODE_L },         \
    [GAME_INPUT_P2_B1]    = { SAPP_KEYCODE_DELETE }, [GAME_INPUT_P2_B2]    = { SAPP_KEYCODE_END },       \
    [GAME_INPUT_P2_B3]    = { SAPP_KEYCODE_PAGE_DOWN }, [GAME_INPUT_P2_B4] = { SAPP_KEYCODE_HOME },      \
    [GAME_INPUT_P2_START] = { SAPP_KEYCODE_2 },      [GAME_INPUT_P2_COIN]  = { SAPP_KEYCODE_6 },         \
    [GAME_INPUT_SERVICE]  = { SAPP_KEYCODE_F1, SAPP_KEYCODE_9 }, [GAME_INPUT_TEST] = { SAPP_KEYCODE_F2 }, \
}
/* A racing cabinet (quirks.wheel: Daytona USA, Sega Rally) as m2emulator lays
 * it out: the arrows drive (Up the accelerator, Down the brake, Left / Right
 * the wheel) and Z X C V are the view buttons VR1-VR4, which the profiles put
 * on P1 B3 / B4 and P2 B3 / B4. A S D F are its gears 1-4 and G neutral (the
 * gear buttons, io_5649.h); the lever's shifts, which it does not have, go on
 * Q (down) and W (up). */
#define INPUT_KEYS_DRIVING {                                                         \
    [GAME_INPUT_P1_UP]    = { SAPP_KEYCODE_W },      [GAME_INPUT_P1_DOWN]  = { SAPP_KEYCODE_Q },         \
    [GAME_INPUT_P1_B1]    = { SAPP_KEYCODE_UP },     [GAME_INPUT_P1_B2]    = { SAPP_KEYCODE_DOWN },      \
    [GAME_INPUT_P1_B3]    = { SAPP_KEYCODE_Z },      [GAME_INPUT_P1_B4]    = { SAPP_KEYCODE_X },         \
    [GAME_INPUT_P2_B3]    = { SAPP_KEYCODE_C },      [GAME_INPUT_P2_B4]    = { SAPP_KEYCODE_V },         \
    [GAME_INPUT_GEAR_1]   = { SAPP_KEYCODE_A },      [GAME_INPUT_GEAR_2]   = { SAPP_KEYCODE_S },         \
    [GAME_INPUT_GEAR_3]   = { SAPP_KEYCODE_D },      [GAME_INPUT_GEAR_4]   = { SAPP_KEYCODE_F },         \
    [GAME_INPUT_GEAR_N]   = { SAPP_KEYCODE_G },                                                         \
}
static int g_input_keys[GAME_INPUT_COUNT][INPUT_KEY_SLOTS] = INPUT_KEYS_DEFAULT;

/* Does profile p play on INPUT_KEYS_DRIVING? */
static inline bool input_keys_driving(const game_profile_t *p) {
    return p && p->quirks.wheel.accel && p->quirks.wheel.brake;
}

/* The default keys for profile p (NULL: the plain layout). */
static inline void input_keys_default(const game_profile_t *p) {
    static const int def[GAME_INPUT_COUNT][INPUT_KEY_SLOTS] = INPUT_KEYS_DEFAULT;
    static const int drive[GAME_INPUT_COUNT][INPUT_KEY_SLOTS] = INPUT_KEYS_DRIVING;
    memcpy(g_input_keys, def, sizeof def);
    if (!input_keys_driving(p)) return;
    for (int a = 0; a < GAME_INPUT_COUNT; a++)
        if (drive[a][0]) memcpy(g_input_keys[a], drive[a], sizeof drive[a]);
}

/* The actions' names in a controls file ("p1_b1", "service"), by GAME_INPUT_*. */
static const char *const g_input_action_names[GAME_INPUT_COUNT] = {
    "p1_up", "p1_down", "p1_left", "p1_right", "p1_b1", "p1_b2", "p1_b3", "p1_b4", "p1_start", "p1_coin",
    "p2_up", "p2_down", "p2_left", "p2_right", "p2_b1", "p2_b2", "p2_b3", "p2_b4", "p2_start", "p2_coin",
    "service", "test", "gearn", "gear1", "gear2", "gear3", "gear4",
};

static inline int input_action_by_name(const char *s) {
    for (int a = 0; a < GAME_INPUT_COUNT; a++) if (!strcmp(s, g_input_action_names[a])) return a;
    return -1;
}

/* The first action key code kc presses, or -1. */
static inline int input_keycode_to_action(int kc) {
    if (kc <= 0) return -1;
    for (int a = 0; a < GAME_INPUT_COUNT; a++)
        for (int s = 0; s < INPUT_KEY_SLOTS; s++)
            if (g_input_keys[a][s] == kc) return a;
    return -1;
}

/* input_keycode_by_name's lower-cased copy of s in n (16 bytes): its length,
 * or -1 when s is too long to be a key's name. */
static inline int input_keycode_lower(const char *s, char *n) {
    size_t len = 0;
    for (; s[len] && len < 15; len++) n[len] = (char)(s[len] | 0x20);
    n[len] = '\0';
    if (s[len]) return -1;
    return (int)len;
}

/* The keys f1-f12 and kp0-kp9; -1 for the rest. */
static inline int input_keycode_fn_kp(const char *n, size_t len) {
    if (n[0] == 'f' && len >= 2 && len <= 3) {
        int f = atoi(n + 1);
        if (f >= 1 && f <= 12) return SAPP_KEYCODE_F1 + (f - 1);
    }
    if (len == 3 && n[0] == 'k' && n[1] == 'p' && n[2] >= '0' && n[2] <= '9') return SAPP_KEYCODE_KP_0 + (n[2] - '0');
    return -1;
}

/* The keys named by a letter or digit, f1-f12 and kp0-kp9; -1 for the rest. */
static inline int input_keycode_short(const char *n, size_t len) {
    if (len == 1 && n[0] >= 'a' && n[0] <= 'z') return SAPP_KEYCODE_A + (n[0] - 'a');
    if (len == 1 && n[0] >= '0' && n[0] <= '9') return SAPP_KEYCODE_0 + (n[0] - '0');
    return input_keycode_fn_kp(n, len);
}

/* The other keys' names, the ones input_keycode_short does not make. */
static const struct { const char *name; int kc; } g_input_key_names[] = {
    { "space", SAPP_KEYCODE_SPACE }, { "tab", SAPP_KEYCODE_TAB }, { "insert", SAPP_KEYCODE_INSERT },
    { "pageup", SAPP_KEYCODE_PAGE_UP }, { "pagedown", SAPP_KEYCODE_PAGE_DOWN }, { "home", SAPP_KEYCODE_HOME },
    { "end", SAPP_KEYCODE_END }, { "delete", SAPP_KEYCODE_DELETE },
    { "up", SAPP_KEYCODE_UP }, { "down", SAPP_KEYCODE_DOWN }, { "left", SAPP_KEYCODE_LEFT },
    { "right", SAPP_KEYCODE_RIGHT }, { "enter", SAPP_KEYCODE_ENTER }, { "backspace", SAPP_KEYCODE_BACKSPACE },
    { "apostrophe", SAPP_KEYCODE_APOSTROPHE }, { "comma", SAPP_KEYCODE_COMMA }, { "minus", SAPP_KEYCODE_MINUS },
    { "period", SAPP_KEYCODE_PERIOD }, { "slash", SAPP_KEYCODE_SLASH }, { "semicolon", SAPP_KEYCODE_SEMICOLON },
    { "equal", SAPP_KEYCODE_EQUAL }, { "lbracket", SAPP_KEYCODE_LEFT_BRACKET },
    { "backslash", SAPP_KEYCODE_BACKSLASH }, { "rbracket", SAPP_KEYCODE_RIGHT_BRACKET },
    { "grave", SAPP_KEYCODE_GRAVE_ACCENT }, { "capslock", SAPP_KEYCODE_CAPS_LOCK },
    { "kpdecimal", SAPP_KEYCODE_KP_DECIMAL }, { "kpdivide", SAPP_KEYCODE_KP_DIVIDE },
    { "kpmultiply", SAPP_KEYCODE_KP_MULTIPLY }, { "kpsubtract", SAPP_KEYCODE_KP_SUBTRACT },
    { "kpadd", SAPP_KEYCODE_KP_ADD }, { "kpenter", SAPP_KEYCODE_KP_ENTER },
    { "lshift", SAPP_KEYCODE_LEFT_SHIFT }, { "lctrl", SAPP_KEYCODE_LEFT_CONTROL },
    { "lalt", SAPP_KEYCODE_LEFT_ALT }, { "rshift", SAPP_KEYCODE_RIGHT_SHIFT },
    { "rctrl", SAPP_KEYCODE_RIGHT_CONTROL }, { "ralt", SAPP_KEYCODE_RIGHT_ALT },
};

/* A sokol key code by name, for --macro and the controls files: a-z, 0-9,
 * f1-f12, kp0-kp9 and g_input_key_names. -1 if unknown. */
static inline int input_keycode_by_name(const char *s) {
    char n[16];
    int len = input_keycode_lower(s, n);
    if (len < 0) return -1;
    int kc = input_keycode_short(n, (size_t)len);
    if (kc >= 0) return kc;
    for (size_t i = 0; i < sizeof g_input_key_names / sizeof g_input_key_names[0]; i++)
        if (!strcmp(n, g_input_key_names[i].name)) return g_input_key_names[i].kc;
    return -1;
}

/* kc's name as input_keycode_by_name reads it, in buf (16 bytes); NULL for a
 * key with no name. */
static inline const char *input_keycode_name(int kc, char *buf) {
    if (kc >= SAPP_KEYCODE_A && kc <= SAPP_KEYCODE_Z) { buf[0] = (char)('a' + kc - SAPP_KEYCODE_A); buf[1] = 0; return buf; }
    if (kc >= SAPP_KEYCODE_0 && kc <= SAPP_KEYCODE_9) { buf[0] = (char)('0' + kc - SAPP_KEYCODE_0); buf[1] = 0; return buf; }
    if (kc >= SAPP_KEYCODE_F1 && kc <= SAPP_KEYCODE_F12) { snprintf(buf, 16, "f%d", kc - SAPP_KEYCODE_F1 + 1); return buf; }
    if (kc >= SAPP_KEYCODE_KP_0 && kc <= SAPP_KEYCODE_KP_9) { snprintf(buf, 16, "kp%d", kc - SAPP_KEYCODE_KP_0); return buf; }
    for (size_t i = 0; i < sizeof g_input_key_names / sizeof g_input_key_names[0]; i++)
        if (g_input_key_names[i].kc == kc) { snprintf(buf, 16, "%s", g_input_key_names[i].name); return buf; }
    return NULL;
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
    if (!bit) return;
    g_input.held &= ~bit;
    /* An action whose mask shares a bit with this one (Virtual-On's B3 is
     * both triggers, B1 the left one) keeps it while still held. */
    for (int a = 0; a < GAME_INPUT_COUNT; a++)
        if (g_input_holds[a]) g_input.held |= g_active_profile->input.bits[a] & bit;
}

/* Does the profile's cabinet seat four (players 3 and 4 have controls)? */
static inline bool input_four_players(void) {
    return g_active_profile && g_active_profile->quirks.four_player_sides;
}

/* Player 3 or 4 presses or lets go of an action: a player-1 action
 * (GAME_INPUT_P1_*) is player 3's, a player-2 one player 4's, as the cabinet
 * wires them (input_sides_read_cb). Nothing on a two-player profile. */
static inline void input_far_down(int act) {
    if (act < 0 || act >= GAME_INPUT_COUNT || !input_four_players()) return;
    if (g_input_far_holds[act] < 255) g_input_far_holds[act]++;
    g_input.far_held |= g_active_profile->input.bits[act];
}

static inline void input_far_up(int act) {
    if (act < 0 || act >= GAME_INPUT_COUNT || !input_four_players()) return;
    uint32_t bit = g_active_profile->input.bits[act];
    if (g_input_far_holds[act] > 0 && --g_input_far_holds[act] > 0) return;
    g_input.far_held &= ~bit;
    for (int a = 0; a < GAME_INPUT_COUNT; a++)
        if (g_input_far_holds[a]) g_input.far_held |= g_active_profile->input.bits[a] & bit;
}

/* ---- Combos ---------------------------------------------------------------- */

/* The n characters at s as a combo button (any case): its index among up,
 * down, left, right, b1-b4, start, coin, or -1. */
static inline int input_combo_name(const char *s, size_t n) {
    static const char *const names[] = { "up", "down", "left", "right", "b1", "b2", "b3", "b4", "start", "coin" };
    for (int i = 0; i < (int)(sizeof names / sizeof names[0]); i++) {
        if (strlen(names[i]) != n) continue;
        size_t c = 0;
        while (c < n && (s[c] | 0x20) == names[i][c]) c++;
        if (c == n) return i;
    }
    return -1;
}

/* The n characters at s as one of the cabinet's own buttons (any case):
 * service, test, and a racing cabinet's gearn, gear1-gear4 (io_5649.h). Its
 * GAME_INPUT_*, or -1. */
static inline int input_cabinet_name(const char *s, size_t n) {
    static const struct { const char *name; int act; } names[] = {
        { "service", GAME_INPUT_SERVICE }, { "test", GAME_INPUT_TEST },
        { "gearn", GAME_INPUT_GEAR_N }, { "gear1", GAME_INPUT_GEAR_1 }, { "gear2", GAME_INPUT_GEAR_2 },
        { "gear3", GAME_INPUT_GEAR_3 }, { "gear4", GAME_INPUT_GEAR_4 },
    };
    for (int i = 0; i < (int)(sizeof names / sizeof names[0]); i++) {
        if (strlen(names[i].name) != n) continue;
        size_t c = 0;
        while (c < n && (s[c] | 0x20) == names[i].name[c]) c++;
        if (c == n) return names[i].act;
    }
    return -1;
}

/* Parse a combo: '+'-separated b1 b2 b3 b4 up down left right start coin,
 * optionally after "p2:" for player 2, or "p3:" / "p4:" for players 3 and 4
 * of a four-player cabinet (INPUT_COMBO_FAR) -- "b1+b2", "p2:b1+b2+b3",
 * "p3:up". Without a prefix (or with "p1:") it may name the cabinet's own
 * buttons too: "service", "gear3". Returns the action mask, or 0 if the text
 * is not one. */
/* A combo's "pN:" prefix, skipped: the player's first action in *base, and
 * INPUT_COMBO_FAR for players 3 and 4. */
static inline uint32_t input__combo_player(const char **s, int *base) {
    const char *p = *s;
    *base = GAME_INPUT_P1_UP;
    if (!((p[0] == 'p' || p[0] == 'P') && p[1] >= '1' && p[1] <= '4' && p[2] == ':')) return 0;
    if (p[1] == '2' || p[1] == '4') *base = GAME_INPUT_P2_UP;
    *s = p + 3;
    return p[1] >= '3' ? INPUT_COMBO_FAR : 0;
}

static inline uint32_t input_combo_parse(const char *s) {
    int base;
    uint32_t acts = input__combo_player(&s, &base);
    while (*s) {
        size_t n = strcspn(s, "+");
        int found = input_combo_name(s, n);
        int cab = found < 0 && base == GAME_INPUT_P1_UP && !acts ? input_cabinet_name(s, n) : -1;
        if (found < 0 && cab < 0) return 0;
        acts |= 1u << (cab >= 0 ? cab : base + found);
        s += n;
        if (*s == '+') s++;
    }
    return acts & ~INPUT_COMBO_FAR ? acts : 0;
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
    for (int a = 0; a < GAME_INPUT_COUNT; a++)
        if (acts & (1u << a)) { if (acts & INPUT_COMBO_FAR) input_far_down(a); else input_action_down(a); }
}

static inline void input_actions_up(uint32_t acts) {
    for (int a = 0; a < GAME_INPUT_COUNT; a++)
        if (acts & (1u << a)) { if (acts & INPUT_COMBO_FAR) input_far_up(a); else input_action_up(a); }
}

/* The combo `key` is bound to, or 0. */
static inline uint32_t input_combo_for_key(int key) {
    for (int i = 0; i < g_input_combo_count; i++)
        if (g_input_combos[i].key == key) return g_input_combos[i].acts;
    return 0;
}

/* The actions key code kc presses (1 << GAME_INPUT_*): a key the map binds
 * to more than one presses them all. */
static inline uint32_t input_keycode_actions(int kc) {
    uint32_t acts = 0;
    if (kc <= 0) return 0;
    for (int a = 0; a < GAME_INPUT_COUNT; a++)
        for (int s = 0; s < INPUT_KEY_SLOTS; s++)
            if (g_input_keys[a][s] == kc) acts |= 1u << a;
    return acts;
}

static inline void input_key_down(int kc) {
    input_actions_down(input_keycode_actions(kc));
    input_actions_down(input_combo_for_key(kc));
}

static inline void input_key_up(int kc) {
    input_actions_up(input_keycode_actions(kc));
    input_actions_up(input_combo_for_key(kc));
}

/* acts as input_combo_parse (or sdl_pads.h's actions_by_name) reads it, in
 * buf: "b1+b2", "p2:up", "service", "gear3", "none". "" for a mix it cannot
 * write (two players' actions, or a cabinet button with another). */
static inline const char *input_combo_format(uint32_t acts, char *buf, size_t cap) {
    static const char *const names[] = { "up", "down", "left", "right", "b1", "b2", "b3", "b4", "start", "coin" };
    const uint32_t p1 = (1u << GAME_INPUT_P2_UP) - 1, p2 = ((1u << GAME_INPUT_SERVICE) - 1) & ~p1;
    acts &= ~INPUT_COMBO_FAR;
    buf[0] = '\0';
    if (!acts) { snprintf(buf, cap, "none"); return buf; }
    for (int a = GAME_INPUT_SERVICE; a < GAME_INPUT_COUNT; a++)   /* service, test, gearn .. gear4 */
        if (acts == 1u << a) { snprintf(buf, cap, "%s", g_input_action_names[a]); return buf; }
    if ((acts & p1) && (acts & p2)) return buf;
    if (acts & ~(p1 | p2)) return buf;
    int base = (acts & p1) ? GAME_INPUT_P1_UP : GAME_INPUT_P2_UP;
    size_t n = 0;
    if (base == GAME_INPUT_P2_UP) n += (size_t)snprintf(buf, cap, "p2:");
    for (int i = 0; i < 10 && n < cap; i++)
        if (acts & (1u << (base + i)))
            n += (size_t)snprintf(buf + n, cap - n, "%s%s", n && buf[n - 1] != ':' ? "+" : "", names[i]);
    return buf;
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
        case 0x02: return eeprom_in0((uint8_t)~(held & 0xFFu));  /* IN0 system */
        case 0x04: return (uint8_t)~((held >> 8)  & 0xFFu);  /* IN1 P1     */
        case 0x06: return (uint8_t)~((held >> 16) & 0xFFu);  /* IN2 P2     */
        default:   if (off >= IO_BOARD_SPAN) return 0;           /* no device there */
                   return r->data ? r->data[off] : 0xFFu;    /* DIPs / idle */
    }
}

/* Port A (IO+0x00) is the EEPROM's; the rest keep the plain buffer. */
static void input_io_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    uint32_t off = addr - r->base;
    for (int i = 0; i < size; i++) {
        uint8_t b = (uint8_t)(val >> (8 * i));
        if (off + (uint32_t)i == 0x00) eeprom_port_a(b);
        if (r->data && off + (uint32_t)i < r->size) r->data[off + (uint32_t)i] = b;
    }
}

static uint32_t input_board_held(void) { return g_input.use_net ? g_input.net_held : g_input.held; }
static int      input_board_from_net(void) { return g_input.use_net; }

static inline void input_lightgun_setup(void);

/* io_5649.h's controls: the wheel, pedals and shifts from the quirks, a
 * gear's own button from the input map. */
static inline void input_io5649_controls(void) {
    const game_quirks_t *q = &g_active_profile->quirks;
    const uint32_t *bits = g_active_profile->input.bits;
    g_io5649_controls = (io5649_controls_t){ q->wheel.left, q->wheel.right, q->wheel.accel,
                                             q->wheel.brake, q->wheel.shift_up, q->wheel.shift_down,
                                             { bits[GAME_INPUT_GEAR_N], bits[GAME_INPUT_GEAR_1], bits[GAME_INPUT_GEAR_2],
                                               bits[GAME_INPUT_GEAR_3], bits[GAME_INPUT_GEAR_4] },
                                             q->wheel.bike };
    io5649_controls_t *c = &g_io5649_controls;
    memcpy(c->ch, q->analog.ch, sizeof c->ch);
    for (int ch = 0; ch < 8; ch++) c->layout |= q->analog.ch[ch] != 0;
    c->in_high = q->analog.in_high;
    c->up      = q->analog.up;
    c->down    = q->analog.down;
    c->press[0] = q->analog.press[0];
    c->press[1] = q->analog.press[1];
    c->rev      = q->analog.rev;
    c->seat     = q->seat_board;
    /* A gun stick on a channel (Gunblade NY, Behind Enemy Lines): the guns'
     * controls and ranges, as a light gun's. */
    if (c->layout && q->gun[0].x_max) input_lightgun_setup();
}

/* A 315-5649 board (a wheel): io_5649.h serves the ports and takes the writes. */
static inline void input_attach_5649(mem_region_t *r) {
    input_io5649_controls();
    g_io5649_held_fn     = input_board_held;
    g_io5649_from_net_fn = input_board_from_net;
    g_io5649_regs        = r->data;      /* port E: the drive board's command (ffb.h) */
    r->read_cb  = io5649_read_cb;
    r->write_cb = io5649_write_cb;
}

/* A board with light guns (lightgun.h): the triggers on IN1, the guns on the
 * second serial channel; IN0 and the rest as input_io_read_cb serves them. */
static uint32_t input_gun_read_cb(mem_region_t *r, uint32_t addr, int size) {
    uint32_t off = addr - r->base, v = 0;
    uint32_t held = input_board_held();
    for (int i = 0; i < size; i++) {
        uint8_t b;
        switch (off + (uint32_t)i) {
            case 0x02: b = eeprom_in0((uint8_t)~(held & 0xFFu)); break;
            case 0x04: b = lightgun_in1(held); break;
            case 0x06: b = 0xFF; break;
            case 0x18: b = lightgun_data(); break;
            case 0x1A: b = 0x0C; break;
            default:   b = off + (uint32_t)i >= IO_BOARD_SPAN ? 0
                             : r->data && off + (uint32_t)i < r->size ? r->data[off + (uint32_t)i] : 0xFFu; break;
        }
        v |= (uint32_t)b << (8 * i);
    }
    return v;
}

static void input_gun_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    uint32_t off = addr - r->base;
    for (int i = 0; i < size; i++) {
        uint8_t b = (uint8_t)(val >> (8 * i));
        if (off + (uint32_t)i == 0x14) g_lightgun.mux = b;
        if (off + (uint32_t)i == 0x00) eeprom_port_a(b);
        if (r->data && off + (uint32_t)i < r->size) r->data[off + (uint32_t)i] = b;
    }
}

/* The guns' controls and ranges from the profile, both at rest. */
static inline void input_lightgun_setup(void) {
    const game_quirks_t *q = &g_active_profile->quirks;
    for (int p = 0; p < LIGHTGUN_PLAYERS; p++) {
        g_lightgun_controls[p] = (lightgun_controls_t){ q->gun[p].up, q->gun[p].down, q->gun[p].left,
                                                        q->gun[p].right, q->gun[p].trigger, q->gun[p].reload };
        g_lightgun_range[p] = (lightgun_range_t){ q->gun[p].x_min, q->gun[p].x_max, q->gun[p].x_rest,
                                                  q->gun[p].y_min, q->gun[p].y_max, q->gun[p].y_rest };
    }
    g_lightgun_held_fn     = input_board_held;
    lightgun_reset();
}

static inline void input_attach_lightgun(mem_region_t *r) {
    input_lightgun_setup();
    r->read_cb  = input_gun_read_cb;
    r->write_cb = input_gun_write_cb;
}

/* A four-player cabinet (quirks.four_player_sides; Air Walkers): the program
 * writes the side it reads into port F bit 7 (the 315-5649's register 5,
 * IO+0x0A), and while it is up IN1 / IN2 answer players 3 and 4 and IN0's
 * START bits (0x10, 0x20) START3 and START4 (MAME model2a_airwlkrs_state).
 * COIN3 and COIN4 are IN0's 0x40 and 0x80 on either side. Players 3 and 4
 * are g_input.far_held, read only when no session has the input: under
 * use_net the far side reads released. The switch itself is the board's, so
 * player 1 never presses player 3's buttons. A two-player cabinet setting
 * leaves the bit down. */
static uint8_t g_io_side;

#define IO_PORT_F 0x0Au   /* register 5: (5 / 2) * 4 + (5 & 1) * 2 */

static uint32_t input_sides_read_cb(mem_region_t *r, uint32_t addr, int size) {
    uint32_t off = addr - r->base, v = 0;
    uint32_t held = input_board_held();
    uint32_t far_bits = input_board_from_net() ? 0u : g_input.far_held;
    uint32_t side = g_io_side ? far_bits : held;
    const uint32_t *bits = g_active_profile->input.bits;
    uint32_t coins = ((far_bits & bits[GAME_INPUT_P1_COIN]) ? 0x40u : 0u) | ((far_bits & bits[GAME_INPUT_P2_COIN]) ? 0x80u : 0u);
    for (int i = 0; i < size; i++) {
        uint8_t b;
        switch (off + (uint32_t)i) {
            case 0x02: b = eeprom_in0((uint8_t)~((held & 0xCFu) | (side & 0x30u) | coins)); break;
            case 0x04: b = (uint8_t)~((side >> 8)  & 0xFFu); break;
            case 0x06: b = (uint8_t)~((side >> 16) & 0xFFu); break;
            default:   b = r->data && off + (uint32_t)i < r->size ? r->data[off + (uint32_t)i] : 0xFFu; break;
        }
        v |= (uint32_t)b << (8 * i);
    }
    return v;
}

static void input_sides_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    uint32_t off = addr - r->base;
    for (int i = 0; i < size; i++) {
        uint8_t b = (uint8_t)(val >> (8 * i));
        if (off + (uint32_t)i == 0x00) eeprom_port_a(b);
        if (off + (uint32_t)i == IO_PORT_F) g_io_side = (uint8_t)(b >> 7);
        if (r->data && off + (uint32_t)i < r->size) r->data[off + (uint32_t)i] = b;
    }
}

/* Power-on: players 1 and 2. The side is board state (savestates, rollback). */
static inline void input_attach_sides(mem_region_t *r) {
    g_io_side = 0;
    savestate_extra(g_active_profile->id, "IOSIDE", &g_io_side, sizeof g_io_side);
    r->read_cb  = input_sides_read_cb;
    r->write_cb = input_sides_write_cb;
}

/* Power Sled's board (quirks.sled): IN0 and IN1's four rider buttons as
 * input_io_read_cb serves them, port E (+0x08) the Cancel Network Check
 * switch, and the pedals on the analog mux io_5649.h keeps (its write
 * callback picks the channel and serves port A). */
static uint8_t input_sled_analog(uint32_t held, int ch) {
    const game_quirks_t *q = &g_active_profile->quirks;
    switch (ch) {
        case 1: return held & q->pedal.p1_right ? q->pedal.right_down : q->pedal.right_up;
        case 3: return held & q->pedal.p1_left  ? q->pedal.left_down  : q->pedal.left_up;
        case 5: return held & q->pedal.p2_right ? q->pedal.right_down : q->pedal.right_up;
        case 7: return held & q->pedal.p2_left  ? q->pedal.left_down  : q->pedal.left_up;
        default: return 0x00;
    }
}

static uint32_t input_sled_read_cb(mem_region_t *r, uint32_t addr, int size) {
    uint32_t off = addr - r->base, v = 0;
    uint32_t held = input_board_held();
    for (int i = 0; i < size; i++) {
        uint8_t b;
        switch (off + (uint32_t)i) {
            case 0x02: b = eeprom_in0((uint8_t)~(held & 0xFFu)); break;
            case 0x04: b = (uint8_t)~(((held >> 8) & 0x0Fu) | ((held >> 13) & 0xF0u)); break;
            case 0x06: b = 0xFF; break;
            case 0x08: b = (held & g_active_profile->quirks.pedal.cancel_net) ? 0xFD : 0xFF; break;
            case 0x1E:
                b = input_sled_analog(held, g_io5649.channel);
                g_io5649.channel = (g_io5649.channel + 1) & 7;
                break;
            default:   b = r->data && off + (uint32_t)i < r->size ? r->data[off + (uint32_t)i] : 0xFFu; break;
        }
        v |= (uint32_t)b << (8 * i);
    }
    return v;
}

static inline void input_attach_sled(mem_region_t *r) {
    r->read_cb  = input_sled_read_cb;
    r->write_cb = io5649_write_cb;
}

/* The I/O region's callbacks for the profile's cabinet. */
static inline void input_attach_io(mem_region_t *r) {
    const game_profile_t *p = g_active_profile;
    if (p && p->quirks.io_5649)                input_attach_5649(r);
    else if (p && p->quirks.lightgun)          input_attach_lightgun(r);
    else if (p && p->quirks.four_player_sides) input_attach_sides(r);
    else if (p && p->quirks.sled)              input_attach_sled(r);
    else { r->read_cb = input_io_read_cb; r->write_cb = input_io_write_cb; }
}

/* Install the I/O read callback on the bus's I/O region. Call after the ROM is
 * installed (install_fn re-inits the bus). Writes (coin-meter strobe at IO+0x00,
 * IO+0x40) keep hitting the plain buffer — harmless. */
static inline void input_attach(memory_bus_t *bus) {
    g_io5649_regs = NULL;
    if (g_active_profile && g_active_profile->quirks.model1_io) {
        /* The install put the board's window at IO_BASE (model1io.h); the
         * Z80 reads the controls through io_5649.h's model. */
        input_io5649_controls();
        g_io5649_held_fn     = input_board_held;
        g_io5649_from_net_fn = input_board_from_net;
        g_model1io_vr4       = g_active_profile->quirks.model1_io_vr4;
        LOG_INFO("input: Model 1 I/O board");
        return;
    }
    if (g_active_profile && g_active_profile->quirks.model1_io2) {
        /* The Advanced board's Z80 reads the buttons and, through its FPGA,
         * the guns (model1io.h). */
        if (g_active_profile->quirks.lightgun) input_lightgun_setup();
        LOG_INFO("input: Advanced Model 1 I/O board");
        return;
    }
    for (int i = 0; i < bus->region_count; i++) {
        if (bus->regions[i].base == IO_BASE) {
            input_attach_io(&bus->regions[i]);
            mem_regions_changed(bus);
            LOG_INFO("input: I/O port read callback attached @ 0x%08X", IO_BASE);
            return;
        }
    }
    LOG_WARN("input: I/O region not found — input inactive");
}

#endif /* INPUT_H */
