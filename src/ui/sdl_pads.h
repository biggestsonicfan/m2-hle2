/*
 * sdl_pads.h — gamepads, racing wheels and their force feedback through SDL3,
 * for the front ends that have SDL: the handheld (main_sdl.c) and the desktop
 * (main.c, which links SDL for its joysticks and haptics only; Pinboard
 * #1082).
 *
 * The pads press the game's actions (input_action_down / _up), recomputed
 * whole on every change (pad_refresh); a driving game also gets their analog
 * wheel and pedals (wheel_host.h), and the drive board's force (ffb.h) goes
 * back to them (ffb_update, once a drawn frame). A front end hands it its SDL
 * events (sdl_pads_event) and calls pad_refresh when one changed something.
 *
 * Options: --pad-map (apply_pad_map), --pad N (g_sdl_pad_only),
 * --wheel-axes, --wheel-buttons, --ffb-gain (sdl_pads_option).
 */
#ifndef M2HLE_SDL_PADS_H
#define M2HLE_SDL_PADS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "input.h"
#include "ffb.h"           /* the drive board's force feedback (Pinboard #1047) */
#include "wheel_host.h"    /* analog wheel and pedals (Pinboard #1082) */

/* --pad: only the Nth gamepad (and the Nth wheel) plays here; -1 = all. */
static int g_sdl_pad_only = -1;

/* ---- Gamepad ------------------------------------------------------------- */

/* Button → action. Directions always come from the d-pad and the left stick. */
typedef struct { const char *name; SDL_GamepadButton button; uint32_t acts; } pad_bind_t;

/* acts is one bit per GAME_INPUT_* action; more than one is a combo. */
#define ACT(a) (1u << (a))
static pad_bind_t g_pad_binds[] = {
    { "south", SDL_GAMEPAD_BUTTON_SOUTH,          ACT(GAME_INPUT_P1_B1)    },
    { "east",  SDL_GAMEPAD_BUTTON_EAST,           ACT(GAME_INPUT_P1_B2)    },
    { "west",  SDL_GAMEPAD_BUTTON_WEST,           ACT(GAME_INPUT_P1_B3)    },
    { "north", SDL_GAMEPAD_BUTTON_NORTH,          ACT(GAME_INPUT_P1_B4)    },
    { "start", SDL_GAMEPAD_BUTTON_START,          ACT(GAME_INPUT_P1_START) },
    { "back",  SDL_GAMEPAD_BUTTON_BACK,           ACT(GAME_INPUT_P1_COIN)  },
    { "l1",    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  0 },
    { "r1",    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0 },
    { "l3",    SDL_GAMEPAD_BUTTON_LEFT_STICK,     0 },
    { "r3",    SDL_GAMEPAD_BUTTON_RIGHT_STICK,    0 },
    { "guide", SDL_GAMEPAD_BUTTON_GUIDE,          0 },
};
#define PAD_BIND_COUNT (int)(sizeof g_pad_binds / sizeof g_pad_binds[0])

/* An action, or a combo of player-1 actions ("b1+b2"); false if neither. */
static bool actions_by_name(const char *s, uint32_t *acts) {
    if (!strcmp(s, "none"))    { *acts = 0; return true; }
    if (!strcmp(s, "service")) { *acts = ACT(GAME_INPUT_SERVICE); return true; }
    if (!strcmp(s, "test"))    { *acts = ACT(GAME_INPUT_TEST); return true; }
    *acts = input_combo_parse(s);
    return *acts && !(*acts >> GAME_INPUT_P2_UP);   /* the pad plays player 1 */
}

static bool apply_pad_map(const char *list) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", list);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        char *eq = strchr(tok, '=');
        if (!eq) { fprintf(stderr, "--pad-map: '%s' is not button=action\n", tok); return false; }
        *eq = '\0';
        uint32_t acts;
        bool ok = actions_by_name(eq + 1, &acts);
        int b;
        for (b = 0; b < PAD_BIND_COUNT && strcmp(g_pad_binds[b].name, tok); b++) {}
        if (b == PAD_BIND_COUNT || !ok) {
            fprintf(stderr, "--pad-map: unknown button or action in '%s=%s'\n", tok, eq + 1);
            return false;
        }
        g_pad_binds[b].acts = acts;
    }
    return true;
}

/* The pad's contribution to each action, recomputed whole on every gamepad
 * event so the d-pad and stick cannot release each other's direction. */
static bool g_pad_held[GAME_INPUT_COUNT];
/* Players 3 and 4's (input_far_down), on a four-player cabinet (Air Walkers):
 * the third pad that plays here is player 3 on player 1's actions, the fourth
 * player 4 on player 2's. */
static bool g_pad_far_held[GAME_INPUT_COUNT];

/* --pad: only that gamepad plays this window; the others play another's. */
static bool pad_plays_here(int i) {
    return g_sdl_pad_only < 0 || i == g_sdl_pad_only;
}

/* ---- Racing wheels (wheel_host.h, Pinboard #1082) -------------------------- */

/* A racing wheel SDL has no gamepad mapping for is a joystick: its wheel and
 * pedal axes (--wheel-axes STEER,ACCEL,BRAKE, default 0,1,2) and its buttons
 * (--wheel-buttons N=action,..., default Logitech's paddles 4=up 5=down for
 * the lever, 9=start, 8=coin, 0=b3 for VIEW). A joystick SDL calls a wheel
 * is one; with --wheel-axes, any joystick that is not a gamepad is. A pedal's
 * released position is where it was when the wheel came, so a pedal that rests
 * at either end of its axis reads the right way round. */
#define WHEEL_BUTTONS 32

static struct {
    int  steer, accel, brake;          /* axis indices */
    bool forced;                       /* --wheel-axes given: any joystick */
    uint32_t button[WHEEL_BUTTONS];    /* acts */
} g_wheel_opt = { 0, 1, 2, false,
                  { [0] = ACT(GAME_INPUT_P1_B3), [4] = ACT(GAME_INPUT_P1_UP), [5] = ACT(GAME_INPUT_P1_DOWN),
                    [8] = ACT(GAME_INPUT_P1_COIN), [9] = ACT(GAME_INPUT_P1_START) } };

static bool wheel_axes_option(const char *val) {
    if (sscanf(val, "%d,%d,%d", &g_wheel_opt.steer, &g_wheel_opt.accel, &g_wheel_opt.brake) != 3) return false;
    g_wheel_opt.forced = true;
    return true;
}

static bool wheel_buttons_option(const char *list) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", list);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        char *eq = strchr(tok, '=');
        uint32_t acts;
        int b = eq ? atoi(tok) : -1;
        if (!eq || b < 0 || b >= WHEEL_BUTTONS || !actions_by_name(eq + 1, &acts)) {
            fprintf(stderr, "--wheel-buttons: '%s' is not N=action (N 0-%d)\n", tok, WHEEL_BUTTONS - 1);
            return false;
        }
        g_wheel_opt.button[b] = acts;
    }
    return true;
}

/* An open wheel's pedals at rest. */
static struct { SDL_JoystickID id; Sint16 rest[2]; } g_wheel_rest[4];

static bool wheel_is(SDL_JoystickID id) {
    if (SDL_IsGamepad(id)) return false;
    return g_wheel_opt.forced || SDL_GetJoystickTypeForID(id) == SDL_JOYSTICK_TYPE_WHEEL;
}

static void wheel_open(SDL_JoystickID id) {
    if (!wheel_is(id)) return;
    SDL_Joystick *joy = SDL_OpenJoystick(id);
    if (!joy) return;
    int s = 0;
    while (s < 4 && g_wheel_rest[s].id && g_wheel_rest[s].id != id) s++;
    if (s < 4) {
        g_wheel_rest[s].id = id;
        const int ax[2] = { g_wheel_opt.accel, g_wheel_opt.brake };
        for (int k = 0; k < 2; k++)
            if (!SDL_GetJoystickAxisInitialState(joy, ax[k], &g_wheel_rest[s].rest[k]))
                g_wheel_rest[s].rest[k] = SDL_GetJoystickAxis(joy, ax[k]);
    }
    printf("m2hle: wheel %s\n", SDL_GetJoystickName(joy));
}

static void wheel_close(SDL_JoystickID id) {
    for (int s = 0; s < 4; s++) if (g_wheel_rest[s].id == id) g_wheel_rest[s].id = 0;
    SDL_Joystick *joy = SDL_GetJoystickFromID(id);
    if (joy && wheel_is(id)) SDL_CloseJoystick(joy);
}

/* One pad's actions into now[]: the d-pad and left stick steer, the binds press. */
static void pad_actions(SDL_Gamepad *pad, bool *now) {
    const int dead = 16000;
    int ax = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    int ay = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    now[GAME_INPUT_P1_UP]    |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP)    || ay < -dead;
    now[GAME_INPUT_P1_DOWN]  |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)  || ay >  dead;
    now[GAME_INPUT_P1_LEFT]  |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)  || ax < -dead;
    now[GAME_INPUT_P1_RIGHT] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || ax >  dead;
    for (int b = 0; b < PAD_BIND_COUNT; b++) {
        if (!g_pad_binds[b].acts || !SDL_GetGamepadButton(pad, g_pad_binds[b].button)) continue;
        for (int a = 0; a < GAME_INPUT_COUNT; a++)
            if (g_pad_binds[b].acts & ACT(a)) now[a] = true;
    }
}

static void pad_refresh(void) {
    bool now[GAME_INPUT_COUNT] = {0}, far_now[GAME_INPUT_COUNT] = {0};
    int count = 0, here = 0;
    /* The wheel and pedals for a driving game: a racing wheel's, else a
     * gamepad's left stick and triggers (R2 accelerator, L2 brake). */
    bool an_on = false, an_wheel = false;
    int an_steer = 0, an_accel = 0, an_brake = 0;
    SDL_JoystickID *jids = SDL_GetJoysticks(&count);
    for (int i = 0, w = 0; jids && i < count; i++) {
        if (!wheel_is(jids[i])) continue;
        SDL_Joystick *joy = SDL_GetJoystickFromID(jids[i]);
        if (!joy || !pad_plays_here(w++)) continue;
        int s = 0;
        while (s < 4 && g_wheel_rest[s].id != jids[i]) s++;
        Sint16 rest[2] = { 32767, 32767 };
        if (s < 4) { rest[0] = g_wheel_rest[s].rest[0]; rest[1] = g_wheel_rest[s].rest[1]; }
        int st = wheel_host_steer(SDL_GetJoystickAxis(joy, g_wheel_opt.steer), 0);
        int ac = wheel_host_pedal(SDL_GetJoystickAxis(joy, g_wheel_opt.accel), rest[0]);
        int br = wheel_host_pedal(SDL_GetJoystickAxis(joy, g_wheel_opt.brake), rest[1]);
        if (!an_wheel) an_steer = st;
        an_on = an_wheel = true;
        if (ac > an_accel) an_accel = ac;
        if (br > an_brake) an_brake = br;
        /* What a session or a replay reads: the turn past half, a pedal past half. */
        now[GAME_INPUT_P1_LEFT]  |= st < -16000;
        now[GAME_INPUT_P1_RIGHT] |= st >  16000;
        now[GAME_INPUT_P1_B1]    |= ac >  16000;
        now[GAME_INPUT_P1_B2]    |= br >  16000;
        int nb = SDL_GetNumJoystickButtons(joy);
        for (int b = 0; b < nb && b < WHEEL_BUTTONS; b++) {
            if (!g_wheel_opt.button[b] || !SDL_GetJoystickButton(joy, b)) continue;
            for (int a = 0; a < GAME_INPUT_COUNT; a++)
                if (g_wheel_opt.button[b] & ACT(a)) now[a] = true;
        }
    }
    SDL_free(jids);
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; i++) {
        if (!pad_plays_here(i)) continue;
        SDL_Gamepad *pad = SDL_GetGamepadFromID(ids[i]);
        if (!pad) continue;
        int nth = here++;
        if (nth >= 2 && nth < 4 && input_four_players()) {
            bool p1[GAME_INPUT_COUNT] = {0};
            pad_actions(pad, p1);
            const int to = nth == 2 ? 0 : GAME_INPUT_P2_UP - GAME_INPUT_P1_UP;
            for (int a = GAME_INPUT_P1_UP; a <= GAME_INPUT_P1_COIN; a++) if (p1[a]) far_now[a + to] = true;
            continue;
        }
        int ax = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        if (!an_wheel) {
            int st = wheel_host_steer(ax, WHEEL_HOST_STICK_DEAD);
            if (!an_on || abs(st) > abs(an_steer)) an_steer = st;
        }
        int ac = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        int br = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        if (ac > an_accel) an_accel = ac;
        if (br > an_brake) an_brake = br;
        an_on = true;
        if (wheel_host_on()) {   /* the triggers as the pedals' buttons, for a session */
            now[GAME_INPUT_P1_B1] |= ac > 16000;
            now[GAME_INPUT_P1_B2] |= br > 16000;
        }
        pad_actions(pad, now);
    }
    SDL_free(ids);
    wheel_host_set(an_on, an_steer, an_accel, an_brake);
    for (int a = 0; a < GAME_INPUT_COUNT; a++) {
        if (now[a] && !g_pad_held[a]) input_action_down(a);
        if (!now[a] && g_pad_held[a]) input_action_up(a);
        g_pad_held[a] = now[a];
        if (far_now[a] && !g_pad_far_held[a]) input_far_down(a);
        if (!far_now[a] && g_pad_far_held[a]) input_far_up(a);
        g_pad_far_held[a] = far_now[a];
    }
}

/* Let go of everything the pad holds, as the game sees it: the netplay lobby
 * takes the pad, and a button held into it must not stay held in the game. */
static void lobby_pad_release(void) {
    wheel_host_set(false, 0, 0, 0);
    for (int a = 0; a < GAME_INPUT_COUNT; a++) {
        if (g_pad_held[a]) input_action_up(a);
        if (g_pad_far_held[a]) input_far_up(a);
        g_pad_held[a] = g_pad_far_held[a] = false;
    }
}

/* ---- Force feedback (ffb.h, Pinboard #1047) ------------------------------- */

/* The drive board's force on the pads that play here: a wheel SDL can drive
 * gets the push, the spring, the friction and the shake as haptic effects,
 * set when the command changes; every other pad rumbles, renewed each
 * rendered frame while it does. */
#define FFB_PADS 4

static struct {
    SDL_JoystickID id;              /* 0: a free slot */
    SDL_Haptic    *haptic;          /* NULL: rumble only */
    int            fx[4];           /* constant, spring, friction, sine; -1 if none */
    bool           on[4];
} g_ffb_pad[FFB_PADS];
static int  g_ffb_last = -1;        /* the command last played; -1: play it again */
static bool g_ffb_rumbling;

static const Uint32 g_ffb_type[4] = { SDL_HAPTIC_CONSTANT, SDL_HAPTIC_SPRING, SDL_HAPTIC_FRICTION, SDL_HAPTIC_SINE };

/* Effect k (g_ffb_type) at strength v, along the wheel. */
static SDL_HapticEffect ffb_effect(int k, int v) {
    SDL_HapticEffect e;
    SDL_zero(e);
    e.type = (Uint16)g_ffb_type[k];
    if (k == 0) {
        e.constant.direction.type = SDL_HAPTIC_STEERING_AXIS;
        e.constant.length = SDL_HAPTIC_INFINITY;
        e.constant.level = (Sint16)v;
    } else if (k == 3) {
        e.periodic.direction.type = SDL_HAPTIC_STEERING_AXIS;
        e.periodic.length = SDL_HAPTIC_INFINITY;
        e.periodic.period = 60;   /* ms */
        e.periodic.magnitude = (Sint16)v;
    } else {
        e.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
        e.condition.length = SDL_HAPTIC_INFINITY;
        e.condition.right_sat[0] = e.condition.left_sat[0] = 0xFFFF;
        e.condition.right_coeff[0] = e.condition.left_coeff[0] = (Sint16)v;
    }
    return e;
}

static void ffb_close_pad(int s) {
    if (g_ffb_pad[s].haptic) SDL_CloseHaptic(g_ffb_pad[s].haptic);
    memset(&g_ffb_pad[s], 0, sizeof g_ffb_pad[s]);
}

/* A wheel's four effects, made once and updated in place. */
static void ffb_open_haptic(int s, SDL_Joystick *joy) {
    if (!joy || !SDL_IsJoystickHaptic(joy)) return;
    SDL_Haptic *h = SDL_OpenHapticFromJoystick(joy);
    if (!h) return;
    Uint32 has = SDL_GetHapticFeatures(h);
    int made = 0;
    for (int k = 0; k < 4; k++) {
        SDL_HapticEffect e = ffb_effect(k, 0);
        g_ffb_pad[s].fx[k] = (has & g_ffb_type[k]) ? SDL_CreateHapticEffect(h, &e) : -1;
        if (g_ffb_pad[s].fx[k] >= 0) made++;
    }
    if (!made) { SDL_CloseHaptic(h); return; }
    if (has & SDL_HAPTIC_AUTOCENTER) SDL_SetHapticAutocenter(h, 0);   /* the game's spring centres it */
    g_ffb_pad[s].haptic = h;
    printf("m2hle: force feedback on %s\n", SDL_GetJoystickName(joy));
}

static void ffb_haptic_play(int s, const ffb_out_t *o) {
    SDL_Haptic *h = g_ffb_pad[s].haptic;
    const int v[4] = { o->push, o->spring, o->friction, o->shake };
    for (int k = 0; k < 4; k++) {
        int id = g_ffb_pad[s].fx[k];
        if (id < 0) continue;
        if (!v[k]) {
            if (g_ffb_pad[s].on[k]) SDL_StopHapticEffect(h, id);
            g_ffb_pad[s].on[k] = false;
            continue;
        }
        SDL_HapticEffect e = ffb_effect(k, v[k]);
        SDL_UpdateHapticEffect(h, id, &e);
        if (!g_ffb_pad[s].on[k]) SDL_RunHapticEffect(h, id, 1);
        g_ffb_pad[s].on[k] = true;
    }
}

static void ffb_update(void) {
    ffb_out_t o = ffb_poll();
    bool rumble = o.strong || o.weak, changed = o.effect.cmd != g_ffb_last;
    if (!changed && !rumble && !g_ffb_rumbling) return;
    /* The gamepads and the racing wheels (wheel_is) that play here. */
    int count = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    for (int s = 0; s < FFB_PADS; s++) {   /* forget the pads that went */
        bool here = false;
        for (int i = 0; ids && i < count && !here; i++) here = ids[i] == g_ffb_pad[s].id;
        if (g_ffb_pad[s].id && !here) ffb_close_pad(s);
    }
    for (int i = 0, np = 0, nw = 0; ids && i < count; i++) {
        SDL_Gamepad  *pad = SDL_IsGamepad(ids[i]) ? SDL_GetGamepadFromID(ids[i]) : NULL;
        SDL_Joystick *joy = pad ? SDL_GetGamepadJoystick(pad)
                          : wheel_is(ids[i]) ? SDL_GetJoystickFromID(ids[i]) : NULL;
        if (!joy || !pad_plays_here(pad ? np++ : nw++)) continue;
        int s = 0;
        while (s < FFB_PADS && g_ffb_pad[s].id != ids[i]) s++;
        if (s == FFB_PADS) {
            for (s = 0; s < FFB_PADS && g_ffb_pad[s].id; s++) {}
            if (s == FFB_PADS) continue;
            g_ffb_pad[s].id = ids[i];
            ffb_open_haptic(s, joy);
        }
        if (g_ffb_pad[s].haptic) { if (changed) ffb_haptic_play(s, &o); }
        else SDL_RumbleJoystick(joy, o.strong, o.weak, rumble ? 100 : 0);
    }
    SDL_free(ids);
    g_ffb_last = o.effect.cmd;
    g_ffb_rumbling = rumble;
}

/* Every force off (the window lost the focus, or the run ends): the wheels'
 * effects go with their haptics, made again by the next ffb_update. */
static void ffb_stop_all(void) {
    for (int s = 0; s < FFB_PADS; s++) if (g_ffb_pad[s].id) ffb_close_pad(s);
    int count = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    for (int i = 0; ids && i < count; i++) {
        SDL_Joystick *joy = SDL_GetJoystickFromID(ids[i]);
        if (joy) SDL_RumbleJoystick(joy, 0, 0, 0);
    }
    SDL_free(ids);
    g_ffb_last = -1;
    g_ffb_rumbling = false;
}

/* The device events: pads and wheels come and go, and move. Sets *changed
 * when pad_refresh has to run. */
static void sdl_pads_event(const SDL_Event *ev, bool *changed) {
    switch (ev->type) {
        case SDL_EVENT_GAMEPAD_ADDED:
            g_ffb_last = -1;   /* give the new pad the force now */
            if (SDL_OpenGamepad(ev->gdevice.which))
                printf("m2hle: gamepad %s\n", SDL_GetGamepadNameForID(ev->gdevice.which));
            *changed = true;
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            SDL_CloseGamepad(SDL_GetGamepadFromID(ev->gdevice.which));
            *changed = true;
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            *changed = true;
            break;
        case SDL_EVENT_JOYSTICK_ADDED:      /* a racing wheel with no gamepad mapping */
            g_ffb_last = -1;
            wheel_open(ev->jdevice.which);
            *changed = true;
            break;
        case SDL_EVENT_JOYSTICK_REMOVED:
            wheel_close(ev->jdevice.which);
            *changed = true;
            break;
        case SDL_EVENT_JOYSTICK_AXIS_MOTION:
        case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
        case SDL_EVENT_JOYSTICK_BUTTON_UP:
            if (wheel_is(ev->jdevice.which)) *changed = true;
            break;
        default: break;
    }
}

/* --pad-map, --pad, --wheel-axes, --wheel-buttons, --ffb-gain: 2 (the
 * arguments it takes) if argv's `a val` is one of them, 0 if it is none, -1 if
 * it is one but wrong. */
static int sdl_pads_option(const char *a, const char *val) {
    if (!val) return 0;
    if (ffb_option(a, val))              return 2;
    if (!strcmp(a, "--pad-map"))         return apply_pad_map(val) ? 2 : -1;
    if (!strcmp(a, "--pad"))             { g_sdl_pad_only = atoi(val); return 2; }
    if (!strcmp(a, "--wheel-axes"))      return wheel_axes_option(val) ? 2 : -1;
    if (!strcmp(a, "--wheel-buttons"))   return wheel_buttons_option(val) ? 2 : -1;
    return 0;
}

#endif /* M2HLE_SDL_PADS_H */
