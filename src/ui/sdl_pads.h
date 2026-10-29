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
#include "fanatec_proto.h" /* fanatec_is_base: SDL knows only some bases (Pinboard #1416) */
#include "ffb.h"           /* the drive board's force feedback (Pinboard #1047) */
#include "wheel_host.h"    /* analog wheel and pedals (Pinboard #1082) */

/* --pad: only the Nth gamepad (and the Nth wheel) plays here; -1 = all. */
static int g_sdl_pad_only = -1;

/* ---- Gamepad ------------------------------------------------------------- */

/* Button → action. Directions always come from the d-pad and the left stick. */
typedef struct { const char *name; SDL_GamepadButton button; uint32_t acts; } pad_bind_t;

/* acts is one bit per GAME_INPUT_* action; more than one is a combo. */
#define ACT(a) (1u << (a))
#define PAD_BINDS_DEFAULT {                                                \
    { "south", SDL_GAMEPAD_BUTTON_SOUTH,          ACT(GAME_INPUT_P1_B1)    },\
    { "east",  SDL_GAMEPAD_BUTTON_EAST,           ACT(GAME_INPUT_P1_B2)    },\
    { "west",  SDL_GAMEPAD_BUTTON_WEST,           ACT(GAME_INPUT_P1_B3)    },\
    { "north", SDL_GAMEPAD_BUTTON_NORTH,          ACT(GAME_INPUT_P1_B4)    },\
    { "start", SDL_GAMEPAD_BUTTON_START,          ACT(GAME_INPUT_P1_START) },\
    { "back",  SDL_GAMEPAD_BUTTON_BACK,           ACT(GAME_INPUT_P1_COIN)  },\
    { "l1",    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  0 },                     \
    { "r1",    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0 },                     \
    { "l3",    SDL_GAMEPAD_BUTTON_LEFT_STICK,     0 },                     \
    { "r3",    SDL_GAMEPAD_BUTTON_RIGHT_STICK,    0 },                     \
    { "guide", SDL_GAMEPAD_BUTTON_GUIDE,          0 },                     \
}
static pad_bind_t g_pad_binds[] = PAD_BINDS_DEFAULT;
#define PAD_BIND_COUNT (int)(sizeof g_pad_binds / sizeof g_pad_binds[0])

/* Which host devices play besides the keyboard (the desktop's Controls
 * window, Pinboard #1147): every pad and wheel, none, gamepads only or
 * racing wheels only. */
enum { PAD_DEVICE_ANY, PAD_DEVICE_NONE, PAD_DEVICE_GAMEPAD, PAD_DEVICE_WHEEL, PAD_DEVICE_COUNT };
static int g_pad_device = PAD_DEVICE_ANY;
static const char *const g_pad_device_names[PAD_DEVICE_COUNT] = { "any", "keyboard", "gamepad", "wheel" };

static bool pad_device_takes(bool wheel) {
    return g_pad_device == PAD_DEVICE_ANY || g_pad_device == (wheel ? PAD_DEVICE_WHEEL : PAD_DEVICE_GAMEPAD);
}

/* One device the Controls window picked for a part (the gamepad, the wheel,
 * the pedals), kept by SDL's GUID, with its name to show while it is
 * unplugged; guid "" = no pick, SDL's types decide. Two of the same model
 * share a GUID: the first plugged in is the one. */
typedef struct { char guid[33]; char name[64]; } pad_pick_t;
static pad_pick_t g_pad_pick;   /* the gamepad that plays; "" = every gamepad */

static bool pad_pick_is(const pad_pick_t *p, SDL_JoystickID id) {
    if (!p->guid[0] || !id) return false;
    char g[33];
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), g, sizeof g);
    return !strcmp(g, p->guid);
}

static void pad_pick_set(pad_pick_t *p, SDL_JoystickID id) {
    memset(p, 0, sizeof *p);
    if (!id) return;
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), p->guid, sizeof p->guid);
    const char *n = SDL_GetJoystickNameForID(id);
    snprintf(p->name, sizeof p->name, "%s", n ? n : "?");
}

/* The connected device the pick names; 0 if it is not plugged in. */
static SDL_JoystickID pad_pick_find(const pad_pick_t *p) {
    if (!p->guid[0]) return 0;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count), id = 0;
    for (int i = 0; ids && i < count && !id; i++) if (pad_pick_is(p, ids[i])) id = ids[i];
    SDL_free(ids);
    return id;
}

/* "<guid> <name>" for a controls file; "" for no pick. */
static void pad_pick_format(const pad_pick_t *p, char *buf, size_t cap) {
    snprintf(buf, cap, "%s%s%s", p->guid, p->guid[0] ? " " : "", p->guid[0] ? p->name : "");
}

static void pad_pick_parse(pad_pick_t *p, const char *v) {
    memset(p, 0, sizeof *p);
    size_t n = strcspn(v, " ");
    if (n != 32) return;
    memcpy(p->guid, v, 32);
    snprintf(p->name, sizeof p->name, "%s", v[n] ? v + n + 1 : "?");
}

/* An action, or a combo of one player's actions ("b1+b2", "p2:b3") or the
 * cabinet's own (service, test, gearn, gear1-gear4); false if neither. A
 * pad plays player 1, but Daytona's view buttons VR3 / VR4 are player 2's
 * B3 / B4, as on the keyboard (Pinboard #1446). Players 3 and 4 are the
 * third and fourth pads (pad_refresh). */
static bool actions_by_name(const char *s, uint32_t *acts) {
    if (!strcmp(s, "none"))    { *acts = 0; return true; }
    *acts = input_combo_parse(s);
    return *acts && !(*acts & INPUT_COMBO_FAR);
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
 * at either end of its axis reads the right way round. A wheel's d-pad (its
 * first hat) is four more buttons after its own 32 (Pinboard #1446). */
#define WHEEL_BUTTONS 36
#define WHEEL_HAT     32               /* hat 0's up, right, down, left: 32-35 */

/* The Controls window can pick the wheel's device (any joystick, gamepads
 * too: a wheel SDL took for something else) and a second device for the
 * pedals (pedals on their own USB lead, as Fanatec's and others' are), and
 * set each pedal's released position, which its Detect button reads, and
 * the wheel's straight, which its Centre button reads. */
typedef struct {
    int  steer, accel, brake;          /* axis indices; accel and brake on the pedals' device */
    bool forced;                       /* --wheel-axes given: any joystick */
    uint32_t button[WHEEL_BUTTONS];    /* acts */
    pad_pick_t dev, pedals;            /* "" = a joystick SDL calls a wheel; the wheel's own pedals */
    int  rest[2];                      /* accelerator, brake released */
    bool rest_set[2];                  /* false: where it was when the device came */
    int  center;                       /* the wheel axis held straight */
    bool center_set;                   /* false: the axis's middle */
} wheel_opt_t;
#define WHEEL_OPT_DEFAULT { 0, 1, 2, false,                                                                 \
                  { [0] = ACT(GAME_INPUT_P1_B3), [4] = ACT(GAME_INPUT_P1_UP), [5] = ACT(GAME_INPUT_P1_DOWN), \
                    [8] = ACT(GAME_INPUT_P1_COIN), [9] = ACT(GAME_INPUT_P1_START) } }
static wheel_opt_t g_wheel_opt = WHEEL_OPT_DEFAULT;

/* A wheel's own layout, which replaces the one above while that is untouched
 * (no controls file, --wheel-axes or --wheel-buttons changed it). The G25 as
 * SDL lists it (Pinboard #1446, the owner's): axes 0 wheel, 1 clutch, 2
 * accelerator, 3 brake (SDL_hidapi_lg4ff.c; the same on Windows without it);
 * buttons 0-3 the shifter's black ones, 4 / 5 the paddles, 6 / 7 the two on
 * the wheel, 8-11 the shifter's red row, 12-17 gears 1-6, 18 reverse. The
 * red row is the four view buttons (Daytona's VR1-4: P1 B3 / B4, P2 B3 / B4),
 * so the pedals' own buttons stay off. */
typedef struct { uint16_t vid, pid; const char *name; wheel_opt_t opt; } wheel_preset_t;
static const wheel_preset_t g_wheel_presets[] = {
    { 0x046D, 0xC299, "Logitech G25", { 0, 2, 3, false,
        { [0] = ACT(GAME_INPUT_TEST), [3] = ACT(GAME_INPUT_SERVICE),
          [4] = ACT(GAME_INPUT_P1_UP), [5] = ACT(GAME_INPUT_P1_DOWN),
          [6] = ACT(GAME_INPUT_P1_START), [7] = ACT(GAME_INPUT_P1_COIN),
          [11] = ACT(GAME_INPUT_P1_B3), [8] = ACT(GAME_INPUT_P1_B4),
          [9] = ACT(GAME_INPUT_P2_B3), [10] = ACT(GAME_INPUT_P2_B4),
          [12] = ACT(GAME_INPUT_GEAR_1), [13] = ACT(GAME_INPUT_GEAR_2),
          [14] = ACT(GAME_INPUT_GEAR_3), [15] = ACT(GAME_INPUT_GEAR_4) } } },
};

/* The gamepad's and the wheel's binds as they are without --pad-map,
 * --wheel-axes, --wheel-buttons or a controls file; every device plays. */
static void sdl_pads_default(void) {
    static const pad_bind_t binds[] = PAD_BINDS_DEFAULT;
    static const wheel_opt_t wheel = WHEEL_OPT_DEFAULT;
    for (int b = 0; b < PAD_BIND_COUNT; b++) g_pad_binds[b].acts = binds[b].acts;
    g_wheel_opt = wheel;
    memset(&g_pad_pick, 0, sizeof g_pad_pick);
    g_pad_device = PAD_DEVICE_ANY;
}

/* Is the wheel's layout (axes, buttons) the built-in one? */
static bool wheel_layout_default(void) {
    static const wheel_opt_t d = WHEEL_OPT_DEFAULT;
    return !g_wheel_opt.forced && g_wheel_opt.steer == d.steer && g_wheel_opt.accel == d.accel &&
           g_wheel_opt.brake == d.brake && !memcmp(g_wheel_opt.button, d.button, sizeof d.button);
}

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

/* Every open joystick's axes where they were when it came (a pedal's
 * released position, unless the Controls window set one). */
#define JOY_REST_AXES 8
static struct { SDL_JoystickID id; Sint16 rest[JOY_REST_AXES]; } g_joy_rest[8];

static int joy_rest(SDL_JoystickID id, int axis) {
    for (int s = 0; s < 8; s++)
        if (g_joy_rest[s].id == id && axis >= 0 && axis < JOY_REST_AXES) return g_joy_rest[s].rest[axis];
    return 32767;
}

/* A Fanatec wheel base by its IDs (fanatec_is_base). SDL 3.4's wheel list
 * has the ten PIDs hid-fanatecff knows; Fanatec's SDK knows more, which SDL
 * takes for plain joysticks (Pinboard #1416). */
static bool joy_is_fanatec_base(SDL_JoystickID id) {
    return SDL_GetJoystickVendorForID(id) == FANATEC_VID && fanatec_is_base(SDL_GetJoystickProductForID(id));
}

/* A racing wheel: one SDL calls a wheel, or a Fanatec base. */
static bool joy_is_racing_wheel(SDL_JoystickID id) {
    return SDL_GetJoystickTypeForID(id) == SDL_JOYSTICK_TYPE_WHEEL || joy_is_fanatec_base(id);
}

/* The wheel: the picked device, else a racing wheel (any joystick that is not
 * a gamepad, with --wheel-axes). A Fanatec base stays the wheel even when SDL
 * has a gamepad mapping for it (Linux makes one for a device with gamepad
 * buttons). */
static bool wheel_is(SDL_JoystickID id) {
    if (g_wheel_opt.dev.guid[0]) return pad_pick_is(&g_wheel_opt.dev, id);
    if (joy_is_fanatec_base(id)) return true;
    if (SDL_IsGamepad(id)) return false;
    return g_wheel_opt.forced || joy_is_racing_wheel(id);
}

/* Has this wheel's steering axis moved since it came? A base can list twice
 * (hid-fanatecff: "only the first ID points to the real device"), and the
 * copy that never moves must not hold the steering at the centre. */
static struct { SDL_JoystickID id; Sint16 first; bool moved; } g_wheel_seen[8];
static int g_wheel_seen_next;
static bool wheel_moved(SDL_JoystickID id, Sint16 v) {
    for (int i = 0; i < 8; i++)
        if (g_wheel_seen[i].id == id) return g_wheel_seen[i].moved |= v != g_wheel_seen[i].first;
    int n = g_wheel_seen_next++ & 7;
    g_wheel_seen[n].id = id;
    g_wheel_seen[n].first = v;
    g_wheel_seen[n].moved = false;
    return false;
}

/* The built-in layout, while it is untouched, becomes the connected wheel's
 * own (g_wheel_presets): when a wheel comes, and when a game's controls load. */
static void wheel_preset_apply(void) {
    if (!wheel_layout_default()) return;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    for (int i = 0; ids && i < count; i++) {
        if (!wheel_is(ids[i])) continue;
        Uint16 vid = SDL_GetJoystickVendorForID(ids[i]), pid = SDL_GetJoystickProductForID(ids[i]);
        for (size_t k = 0; k < sizeof g_wheel_presets / sizeof g_wheel_presets[0]; k++) {
            const wheel_preset_t *w = &g_wheel_presets[k];
            if (w->vid != vid || w->pid != pid) continue;
            g_wheel_opt.steer = w->opt.steer;
            g_wheel_opt.accel = w->opt.accel;
            g_wheel_opt.brake = w->opt.brake;
            memcpy(g_wheel_opt.button, w->opt.button, sizeof g_wheel_opt.button);
            printf("m2hle: wheel layout: %s's\n", w->name);
            SDL_free(ids);
            return;
        }
    }
    SDL_free(ids);
}

/* A wheel button held: one of its own, or (WHEEL_HAT up) a direction of its d-pad. */
static bool wheel_button_down(SDL_Joystick *joy, int b) {
    if (b < WHEEL_HAT) return b < SDL_GetNumJoystickButtons(joy) && SDL_GetJoystickButton(joy, b);
    static const Uint8 dir[4] = { SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_DOWN, SDL_HAT_LEFT };
    return b < WHEEL_BUTTONS && SDL_GetNumJoystickHats(joy) > 0 && (SDL_GetJoystickHat(joy, 0) & dir[b - WHEEL_HAT]);
}

/* A picked pedal device that is not the wheel itself. */
static bool pedals_is(SDL_JoystickID id) {
    return pad_pick_is(&g_wheel_opt.pedals, id) && !wheel_is(id);
}

/* A gamepad that plays as one: not the wheel or the pedals, and the picked
 * one if there is a pick. */
static bool gamepad_is(SDL_JoystickID id) {
    if (!SDL_IsGamepad(id) || wheel_is(id) || pedals_is(id)) return false;
    return !g_pad_pick.guid[0] || pad_pick_is(&g_pad_pick, id);
}

/* Every joystick is opened, so that any can be picked as the wheel or the
 * pedals, and its axes shown, without plugging it in again. */
static void wheel_open(SDL_JoystickID id) {
    SDL_Joystick *joy = SDL_OpenJoystick(id);
    if (!joy) return;
    int s = 0;
    while (s < 8 && g_joy_rest[s].id && g_joy_rest[s].id != id) s++;
    if (s < 8) {
        g_joy_rest[s].id = id;
        int n = SDL_GetNumJoystickAxes(joy);
        for (int k = 0; k < JOY_REST_AXES && k < n; k++)
            if (!SDL_GetJoystickAxisInitialState(joy, k, &g_joy_rest[s].rest[k]))
                g_joy_rest[s].rest[k] = SDL_GetJoystickAxis(joy, k);
    }
    /* Every joystick that is not a gamepad, with what it was taken for: what a
     * wheel's owner sends when it does not steer. */
    if (!SDL_IsGamepad(id))
        printf("m2hle: %s %s (%04X:%04X, %d axes, %d buttons)\n", wheel_is(id) ? "wheel" : "joystick",
               SDL_GetJoystickName(joy), SDL_GetJoystickVendor(joy), SDL_GetJoystickProduct(joy),
               SDL_GetNumJoystickAxes(joy), SDL_GetNumJoystickButtons(joy));
}

static void wheel_close(SDL_JoystickID id) {
    for (int s = 0; s < 8; s++) if (g_joy_rest[s].id == id) g_joy_rest[s].id = 0;
    SDL_Joystick *joy = SDL_GetJoystickFromID(id);
    if (joy) SDL_CloseJoystick(joy);
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

/* The wheel and pedals for a driving game: a racing wheel's, else a
 * gamepad's left stick and triggers (R2 accelerator, L2 brake). */
typedef struct {
    bool on, wheel, moved;
    int  steer, accel, brake;
} pad_analog_t;

/* The pedals' two axes: their own device when one is picked (none while it
 * is unplugged), else the wheel's. */
static void wheel_pedals(SDL_JoystickID wheel, int ped[2]) {
    SDL_JoystickID pid = wheel;
    if (g_wheel_opt.pedals.guid[0] && !pad_pick_is(&g_wheel_opt.pedals, wheel)) pid = pad_pick_find(&g_wheel_opt.pedals);
    SDL_Joystick *pj = pid ? SDL_GetJoystickFromID(pid) : NULL;
    const int pax[2] = { g_wheel_opt.accel, g_wheel_opt.brake };
    ped[0] = ped[1] = 0;
    for (int k = 0; pj && k < 2; k++)
        ped[k] = wheel_host_pedal(SDL_GetJoystickAxis(pj, pax[k]),
                                  g_wheel_opt.rest_set[k] ? g_wheel_opt.rest[k] : joy_rest(pid, pax[k]));
}

/* One racing wheel that plays here into an and now[]. */
static void pad_wheel_read(SDL_JoystickID jid, SDL_Joystick *joy, bool *now, pad_analog_t *an) {
    int ped[2];
    wheel_pedals(jid, ped);
    Sint16 raw = SDL_GetJoystickAxis(joy, g_wheel_opt.steer);
    int st = g_wheel_opt.center_set ? wheel_host_steer_centred(raw, g_wheel_opt.center) : wheel_host_steer(raw, 0);
    int ac = ped[0], br = ped[1];
    /* The first wheel steers, or the first that has moved. */
    bool moved = wheel_moved(jid, raw);
    if (!an->wheel || (moved && !an->moved)) { an->steer = st; an->moved = moved; }
    an->on = an->wheel = true;
    if (ac > an->accel) an->accel = ac;
    if (br > an->brake) an->brake = br;
    /* What a session or a replay reads: the turn past half, a pedal past half. */
    now[GAME_INPUT_P1_LEFT]  |= st < -16000;
    now[GAME_INPUT_P1_RIGHT] |= st >  16000;
    now[GAME_INPUT_P1_B1]    |= ac >  16000;
    now[GAME_INPUT_P1_B2]    |= br >  16000;
    for (int b = 0; b < WHEEL_BUTTONS; b++) {
        if (!g_wheel_opt.button[b] || !wheel_button_down(joy, b)) continue;
        for (int a = 0; a < GAME_INPUT_COUNT; a++)
            if (g_wheel_opt.button[b] & ACT(a)) now[a] = true;
    }
}

/* The third and fourth gamepads of a four-player cabinet: players 3 and 4. */
static void pad_far_read(SDL_Gamepad *pad, int nth, bool *far_now) {
    bool p1[GAME_INPUT_COUNT] = {0};
    pad_actions(pad, p1);
    const int to = nth == 2 ? 0 : GAME_INPUT_P2_UP - GAME_INPUT_P1_UP;
    for (int a = GAME_INPUT_P1_UP; a <= GAME_INPUT_P1_COIN; a++) if (p1[a]) far_now[a + to] = true;
}

/* A gamepad's stick and triggers into an (a wheel, if any, steers), and its
 * buttons into now[]. */
static void pad_gamepad_read(SDL_Gamepad *pad, bool *now, pad_analog_t *an) {
    int ax = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    if (!an->wheel) {
        int st = wheel_host_steer(ax, WHEEL_HOST_STICK_DEAD);
        if (!an->on || abs(st) > abs(an->steer)) an->steer = st;
    }
    int ac = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    int br = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    if (ac > an->accel) an->accel = ac;
    if (br > an->brake) an->brake = br;
    an->on = true;
    if (wheel_host_on()) {   /* the triggers as the pedals' buttons, for a session */
        now[GAME_INPUT_P1_B1] |= ac > 16000;
        now[GAME_INPUT_P1_B2] |= br > 16000;
    }
    pad_actions(pad, now);
}

/* Press and let go what changed since the last refresh. */
static void pad_publish(const bool *now, const bool *far_now) {
    for (int a = 0; a < GAME_INPUT_COUNT; a++) {
        if (now[a] && !g_pad_held[a]) input_action_down(a);
        if (!now[a] && g_pad_held[a]) input_action_up(a);
        g_pad_held[a] = now[a];
        if (far_now[a] && !g_pad_far_held[a]) input_far_down(a);
        if (!far_now[a] && g_pad_far_held[a]) input_far_up(a);
        g_pad_far_held[a] = far_now[a];
    }
}

/* Every racing wheel that plays here. */
static void pad_wheels_read(bool *now, pad_analog_t *an) {
    int count = 0;
    SDL_JoystickID *jids = pad_device_takes(true) ? SDL_GetJoysticks(&count) : NULL;
    for (int i = 0, w = 0; jids && i < count; i++) {
        if (!wheel_is(jids[i])) continue;
        SDL_Joystick *joy = SDL_GetJoystickFromID(jids[i]);
        if (!joy || !pad_plays_here(w++)) continue;
        pad_wheel_read(jids[i], joy, now, an);
    }
    SDL_free(jids);
}

static void pad_refresh(void) {
    bool now[GAME_INPUT_COUNT] = {0}, far_now[GAME_INPUT_COUNT] = {0};
    int count = 0, here = 0;
    pad_analog_t an = {0};
    pad_wheels_read(now, &an);
    SDL_JoystickID *ids = pad_device_takes(false) ? SDL_GetGamepads(&count) : NULL;
    for (int i = 0, g = 0; ids && i < count; i++) {
        if (!gamepad_is(ids[i]) || !pad_plays_here(g++)) continue;
        SDL_Gamepad *pad = SDL_GetGamepadFromID(ids[i]);
        if (!pad) continue;
        int nth = here++;
        if (nth >= 2 && nth < 4 && input_four_players()) pad_far_read(pad, nth, far_now);
        else                                             pad_gamepad_read(pad, now, &an);
    }
    SDL_free(ids);
    wheel_host_set(an.on, an.steer, an.accel, an.brake);
    pad_publish(now, far_now);
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
 * gets the push, the spring (its coefficient negative for the uncentring) and
 * the damper (the clutch) as haptic effects, set when the command changes;
 * every other pad rumbles, renewed each rendered frame while it does. */
#define FFB_PADS 4

static struct {
    SDL_JoystickID id;              /* 0: a free slot */
    SDL_Haptic    *haptic;          /* NULL: rumble only */
    int            fx[3];           /* constant, spring, damper; -1 if none */
    bool           on[3];
} g_ffb_pad[FFB_PADS];
static int  g_ffb_last = -1;        /* the command last played; -1: play it again */
static bool g_ffb_rumbling;

static const Uint32 g_ffb_type[3] = { SDL_HAPTIC_CONSTANT, SDL_HAPTIC_SPRING, SDL_HAPTIC_DAMPER };

/* Effect k (g_ffb_type) at strength v, along the wheel; a spring's v < 0
 * pushes away from the centre. */
static SDL_HapticEffect ffb_effect(int k, int v) {
    SDL_HapticEffect e;
    SDL_zero(e);
    e.type = (Uint16)g_ffb_type[k];
    if (k == 0) {
        e.constant.direction.type = SDL_HAPTIC_STEERING_AXIS;
        e.constant.length = SDL_HAPTIC_INFINITY;
        e.constant.level = (Sint16)v;
    } else {
        e.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
        e.condition.length = SDL_HAPTIC_INFINITY;
        /* The uncentring stops at its own strength, not at the wheel's lock. */
        e.condition.right_sat[0] = e.condition.left_sat[0] = (Uint16)(v < 0 ? -v * 2 + 1 : 0xFFFF);
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
    if (!joy) return;
    /* A wheel without its force says why (a base whose driver is missing,
     * or a device another program holds; Pinboard #1416). */
    bool wheel = wheel_is(SDL_GetJoystickID(joy));
    if (!SDL_IsJoystickHaptic(joy)) {
        if (wheel) printf("m2hle: no force feedback on %s: SDL sees no haptic device\n", SDL_GetJoystickName(joy));
        return;
    }
    SDL_Haptic *h = SDL_OpenHapticFromJoystick(joy);
    if (!h) {
        if (wheel) printf("m2hle: no force feedback on %s: %s\n", SDL_GetJoystickName(joy), SDL_GetError());
        return;
    }
    Uint32 has = SDL_GetHapticFeatures(h);
    int made = 0;
    for (int k = 0; k < 3; k++) {
        SDL_HapticEffect e = ffb_effect(k, 0);
        g_ffb_pad[s].fx[k] = (has & g_ffb_type[k]) ? SDL_CreateHapticEffect(h, &e) : -1;
        if (g_ffb_pad[s].fx[k] >= 0) made++;
    }
    if (!made) {
        if (wheel) printf("m2hle: no force feedback on %s: no effect it takes (features %08X)\n",
                          SDL_GetJoystickName(joy), (unsigned)has);
        SDL_CloseHaptic(h);
        return;
    }
    if (has & SDL_HAPTIC_AUTOCENTER) SDL_SetHapticAutocenter(h, 0);   /* the game's spring centres it */
    g_ffb_pad[s].haptic = h;
    printf("m2hle: force feedback on %s\n", SDL_GetJoystickName(joy));
}

static void ffb_haptic_play(int s, const ffb_out_t *o) {
    SDL_Haptic *h = g_ffb_pad[s].haptic;
    const int v[3] = { o->push, o->spring, o->damper };
    for (int k = 0; k < 3; k++) {
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

/* Close the slots of the pads that went. */
static void ffb_forget_gone(const SDL_JoystickID *ids, int count) {
    for (int s = 0; s < FFB_PADS; s++) {
        bool here = false;
        for (int i = 0; ids && i < count && !here; i++) here = ids[i] == g_ffb_pad[s].id;
        if (g_ffb_pad[s].id && !here) ffb_close_pad(s);
    }
}

/* The slot of pad id, given one (and its haptic opened) if it has none yet;
 * -1 when all are taken. */
static int ffb_pad_slot(SDL_JoystickID id, SDL_Joystick *joy) {
    int s = 0;
    while (s < FFB_PADS && g_ffb_pad[s].id != id) s++;
    if (s < FFB_PADS) return s;
    for (s = 0; s < FFB_PADS && g_ffb_pad[s].id; s++) {}
    if (s == FFB_PADS) return -1;
    g_ffb_pad[s].id = id;
    ffb_open_haptic(s, joy);
    return s;
}

/* Device id's joystick, if it is a gamepad or a racing wheel (wheel_is) that
 * plays here; np and nw count the gamepads and wheels seen. */
static SDL_Joystick *ffb_joystick(SDL_JoystickID id, int *np, int *nw) {
    SDL_Gamepad  *pad = gamepad_is(id) ? SDL_GetGamepadFromID(id) : NULL;
    SDL_Joystick *joy = pad ? SDL_GetGamepadJoystick(pad)
                      : wheel_is(id) ? SDL_GetJoystickFromID(id) : NULL;
    if (!joy || !pad_plays_here(pad ? (*np)++ : (*nw)++) || !pad_device_takes(!pad)) return NULL;
    return joy;
}

static void ffb_update(void) {
    ffb_out_t o = ffb_poll();
    bool rumble = o.strong || o.weak, changed = o.effect.cmd != g_ffb_last;
    if (!changed && !rumble && !g_ffb_rumbling) return;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    ffb_forget_gone(ids, count);
    for (int i = 0, np = 0, nw = 0; ids && i < count; i++) {
        SDL_Joystick *joy = ffb_joystick(ids[i], &np, &nw);
        int s = joy ? ffb_pad_slot(ids[i], joy) : -1;
        if (s < 0) continue;
        if (g_ffb_pad[s].haptic) { if (changed) ffb_haptic_play(s, &o); }
        else SDL_RumbleJoystick(joy, o.strong, o.weak, rumble ? 100 : 0);
    }
    SDL_free(ids);
    g_ffb_last = o.effect.cmd;
    g_ffb_rumbling = rumble;
}

/* A wheel (wheel_is) whose force SDL plays: lg4ff.h leaves it to us then. */
static bool ffb_wheel_haptic(void) {
    for (int s = 0; s < FFB_PADS; s++)
        if (g_ffb_pad[s].haptic && wheel_is(g_ffb_pad[s].id)) return true;
    return false;
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

/* Every force off while the window is in the background, the haptics kept
 * open: the next ffb_update starts the effects again. Closing and opening a
 * wheel's haptic is slow where a driver stands behind it (DirectInput with
 * Logitech's software), and alt-tab must not wait on it (Pinboard #1447). */
static void ffb_pause(void) {
    for (int s = 0; s < FFB_PADS; s++) {
        if (!g_ffb_pad[s].haptic) continue;
        for (int k = 0; k < 3; k++) {
            if (g_ffb_pad[s].on[k]) SDL_StopHapticEffect(g_ffb_pad[s].haptic, g_ffb_pad[s].fx[k]);
            g_ffb_pad[s].on[k] = false;
        }
    }
    if (g_ffb_rumbling) {
        for (int s = 0; s < FFB_PADS; s++) {
            SDL_Joystick *joy = g_ffb_pad[s].id ? SDL_GetJoystickFromID(g_ffb_pad[s].id) : NULL;
            if (joy && !g_ffb_pad[s].haptic) SDL_RumbleJoystick(joy, 0, 0, 0);
        }
    }
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
        case SDL_EVENT_JOYSTICK_ADDED:      /* every joystick: any can be the wheel or the pedals */
            g_ffb_last = -1;
            wheel_open(ev->jdevice.which);
            wheel_preset_apply();
            *changed = true;
            break;
        case SDL_EVENT_JOYSTICK_REMOVED:
            wheel_close(ev->jdevice.which);
            *changed = true;
            break;
        case SDL_EVENT_JOYSTICK_AXIS_MOTION:
        case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
        case SDL_EVENT_JOYSTICK_BUTTON_UP:
        case SDL_EVENT_JOYSTICK_HAT_MOTION:
            if (wheel_is(ev->jdevice.which) || pedals_is(ev->jdevice.which)) *changed = true;
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
