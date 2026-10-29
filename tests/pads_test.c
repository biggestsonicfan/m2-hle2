/*
 * pads_test.c -- the Controls window's device picks (ui/sdl_pads.h, Pinboard
 * #1147) on SDL's virtual joysticks: no hardware.
 *
 * A wheel base whose pedals are a second device (a Fanatec on its own pedal
 * lead): the pedals read from the picked device, each from where it rests; a
 * joystick SDL does not call a wheel plays as one once picked; with a
 * gamepad picked the other gamepad plays nothing, and a gamepad picked as the
 * wheel is not a gamepad any more. A pick's file text reads back.
 *
 * A ctest (CMakeLists.txt, pads_test; M2HLE_PADS builds only).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "input.h"
#include "sdl_pads.h"

const game_profile_t *g_active_profile = NULL;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

/* A virtual device; type GAMEPAD with the standard buttons is a gamepad to SDL. */
static SDL_JoystickID attach_vid(const char *name, SDL_JoystickType type, Uint16 vendor, Uint16 product,
                                 int naxes, int nbuttons) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = (Uint16)type;
    d.vendor_id = vendor;
    d.product_id = product;
    d.naxes = (Uint16)naxes;
    d.nbuttons = (Uint16)nbuttons;
    d.name = name;
    if (type == SDL_JOYSTICK_TYPE_GAMEPAD)
        d.button_mask = (1u << SDL_GAMEPAD_BUTTON_SOUTH) | (1u << SDL_GAMEPAD_BUTTON_EAST) |
                        (1u << SDL_GAMEPAD_BUTTON_WEST) | (1u << SDL_GAMEPAD_BUTTON_NORTH) |
                        (1u << SDL_GAMEPAD_BUTTON_START) | (1u << SDL_GAMEPAD_BUTTON_BACK);
    return SDL_AttachVirtualJoystick(&d);
}

static SDL_JoystickID attach(const char *name, SDL_JoystickType type, Uint16 product, int naxes, int nbuttons) {
    return attach_vid(name, type, 0x1234, product, naxes, nbuttons);
}

/* SDL's events into sdl_pads, as the front end does, then the presses. */
static void pump(void) {
    SDL_Event ev;
    bool changed = false;
    SDL_UpdateJoysticks();
    while (SDL_PollEvent(&ev)) sdl_pads_event(&ev, &changed);
    pad_refresh();
}

static void axis(SDL_JoystickID id, int a, int v) {
    SDL_SetJoystickVirtualAxis(SDL_GetJoystickFromID(id), a, (Sint16)v);
}

static void button(SDL_JoystickID id, int b, bool down) {
    SDL_SetJoystickVirtualButton(SDL_GetJoystickFromID(id), b, down);
}

/* The five devices every group plays with. */
static SDL_JoystickID base, pedals, stick, pad1, pad2;

static void attach_devices(void) {
    base   = attach("Test Wheel Base", SDL_JOYSTICK_TYPE_WHEEL, 1, 2, 16);
    pedals = attach("Test Pedals", SDL_JOYSTICK_TYPE_UNKNOWN, 2, 3, 0);
    stick  = attach("Test Odd Wheel", SDL_JOYSTICK_TYPE_UNKNOWN, 3, 3, 8);
    pad1   = attach("Test Pad One", SDL_JOYSTICK_TYPE_GAMEPAD, 4, 6, 15);
    pad2   = attach("Test Pad Two", SDL_JOYSTICK_TYPE_GAMEPAD, 5, 6, 15);
    CHECK(base && pedals && stick && pad1 && pad2, "five virtual devices attached");
    pump();
    CHECK(SDL_GetJoystickFromID(pedals) && SDL_GetJoystickFromID(stick),
          "every joystick is opened, not only the ones SDL calls wheels");
    CHECK(SDL_IsGamepad(pad1) && SDL_IsGamepad(pad2), "the virtual gamepads are gamepads");
}

/* No picks: SDL's wheel is the wheel, its own axes 1 and 2 the pedals. */
static void test_no_pick(void) {
    CHECK(wheel_is(base) && !wheel_is(pedals) && !wheel_is(stick), "no pick: the device SDL calls a wheel");
    axis(base, 0, 20000);
    pump();
    CHECK(g_io5649_host.steer > 15000, "no pick: the base steers");
    axis(pedals, 1, 32767);
    pump();
    CHECK(g_io5649_host.accel == 0, "no pick: the pedal device is not read");
}

/* The pedals on their own device, resting at the bottom of their axes. */
static void test_pedals_pick(void) {
    pad_pick_set(&g_wheel_opt.pedals, pedals);
    g_wheel_opt.accel = 1; g_wheel_opt.brake = 2;
    g_wheel_opt.rest[0] = g_wheel_opt.rest[1] = -32768;
    g_wheel_opt.rest_set[0] = g_wheel_opt.rest_set[1] = true;
    axis(pedals, 1, -32768); axis(pedals, 2, -32768);
    pump();
    CHECK(pedals_is(pedals) && g_io5649_host.accel == 0 && g_io5649_host.brake == 0,
          "pedals picked: both released at their rest");
    axis(pedals, 1, 32767);
    pump();
    CHECK(g_io5649_host.accel == IO5649_AXIS_MAX && g_io5649_host.brake == 0 && g_pad_held[GAME_INPUT_P1_B1],
          "pedals picked: the accelerator floored, from the pedal device");
    axis(pedals, 1, -32768); axis(pedals, 2, 0);
    pump();
    CHECK(g_io5649_host.accel == 0 && g_io5649_host.brake > 16000 && g_io5649_host.brake < 16500,
          "pedals picked: the brake half down");
    axis(base, 1, 32767);
    pump();
    CHECK(g_io5649_host.accel == 0, "pedals picked: the base's own axis 1 is not the accelerator");
    axis(base, 1, 0); axis(pedals, 2, -32768);
}

/* A joystick SDL does not call a wheel, picked as the wheel. */
static void test_wheel_pick(void) {
    pad_pick_set(&g_wheel_opt.dev, stick);
    pump();
    CHECK(wheel_is(stick) && !wheel_is(base), "wheel picked: the picked joystick and not SDL's wheel");
    axis(stick, 0, -30000); axis(base, 0, 30000);
    pump();
    CHECK(g_io5649_host.steer < -25000, "wheel picked: it steers, the base does not");
    button(stick, 0, true);
    pump();
    CHECK(g_pad_held[GAME_INPUT_P1_B3], "wheel picked: its buttons press (0 = button 3)");
    button(stick, 0, false);
    axis(stick, 0, 0); axis(base, 0, 0);
}

/* The gamepad pick. */
static void test_gamepad_pick(void) {
    memset(&g_wheel_opt.dev, 0, sizeof g_wheel_opt.dev);
    pump();
    button(pad1, SDL_GAMEPAD_BUTTON_SOUTH, true);
    pump();
    CHECK(g_pad_held[GAME_INPUT_P1_B1], "no gamepad pick: either gamepad plays");
    pad_pick_set(&g_pad_pick, pad2);
    pump();
    CHECK(!g_pad_held[GAME_INPUT_P1_B1] && gamepad_is(pad2) && !gamepad_is(pad1),
          "gamepad picked: the other one plays nothing");
    button(pad1, SDL_GAMEPAD_BUTTON_SOUTH, false);
    button(pad2, SDL_GAMEPAD_BUTTON_SOUTH, true);
    pump();
    CHECK(g_pad_held[GAME_INPUT_P1_B1], "gamepad picked: it plays");
    button(pad2, SDL_GAMEPAD_BUTTON_SOUTH, false);
}

/* A gamepad picked as the wheel is the wheel, not a gamepad. */
static void test_gamepad_as_wheel(void) {
    pad_pick_set(&g_wheel_opt.dev, pad2);
    pump();
    CHECK(wheel_is(pad2) && !gamepad_is(pad2), "a gamepad picked as the wheel stops being a gamepad");
}

/* A pick's file text reads back; anything not a GUID is no pick. */
static void test_pick_text(void) {
    char txt[112];
    pad_pick_t back;
    pad_pick_format(&g_wheel_opt.pedals, txt, sizeof txt);
    pad_pick_parse(&back, txt);
    CHECK(!strcmp(back.guid, g_wheel_opt.pedals.guid) && !strcmp(back.name, "Test Pedals"), "a pick's text reads back");
    pad_pick_parse(&back, "any");
    CHECK(!back.guid[0], "text that is not a GUID is no pick");
}

/* Unplugged: the picked pedals read nothing, and no device is found. */
static void test_unplug(void) {
    memset(&g_wheel_opt.dev, 0, sizeof g_wheel_opt.dev);
    axis(pedals, 1, 32767);
    pump();
    CHECK(g_io5649_host.accel == IO5649_AXIS_MAX, "pedals down before the unplug");
    SDL_DetachVirtualJoystick(pedals);
    pump();
    CHECK(!pad_pick_find(&g_wheel_opt.pedals) && g_io5649_host.accel == 0, "pedals unplugged: no accelerator");

    sdl_pads_default();
    CHECK(!g_pad_pick.guid[0] && !g_wheel_opt.dev.guid[0] && !g_wheel_opt.pedals.guid[0] && !g_wheel_opt.rest_set[0],
          "the defaults drop every pick");
}

/* A Fanatec base SDL has no word for (0EB7:0E07, in the SDK's base table,
 * not on SDL's wheel list) is the wheel; Fanatec's pedals on their own
 * lead (0EB7:183B) are not. */
static void test_fanatec_base(void) {
    SDL_DetachVirtualJoystick(base);
    SDL_JoystickID fbase = attach_vid("FANATEC Wheel", SDL_JOYSTICK_TYPE_UNKNOWN, 0x0EB7, 0x0E07, 3, 16);
    SDL_JoystickID fped = attach_vid("FANATEC ClubSport Pedals", SDL_JOYSTICK_TYPE_UNKNOWN, 0x0EB7, 0x183B, 3, 0);
    pump();
    CHECK(SDL_GetJoystickTypeForID(fbase) != SDL_JOYSTICK_TYPE_WHEEL, "SDL does not call 0EB7:0E07 a wheel");
    CHECK(wheel_is(fbase) && !wheel_is(fped), "a Fanatec base is the wheel, its pedals are not");
    axis(fbase, 0, -20000);
    pump();
    CHECK(g_io5649_host.steer < -15000, "the Fanatec base steers");
    SDL_DetachVirtualJoystick(fbase);
    SDL_DetachVirtualJoystick(fped);
    pump();
}

/* A GT DD Pro (0EB7:0020, on SDL's list) listed twice, the copy that
 * never moves first: the one that moves steers. */
static void test_fanatec_twice(void) {
    SDL_JoystickID dead = attach_vid("FANATEC Wheel", SDL_JOYSTICK_TYPE_WHEEL, 0x0EB7, 0x0020, 3, 16);
    pump();
    SDL_JoystickID live = attach_vid("FANATEC Wheel", SDL_JOYSTICK_TYPE_WHEEL, 0x0EB7, 0x0020, 3, 16);
    pump();
    CHECK(wheel_is(dead) && wheel_is(live), "both copies of the base are wheels");
    axis(live, 0, 20000);
    pump();
    CHECK(g_io5649_host.steer > 15000, "the copy that moves steers, not the dead one listed first");
    SDL_DetachVirtualJoystick(dead);
    SDL_DetachVirtualJoystick(live);
    pump();
}

/* A G25 (046D:C299: 4 axes, 19 buttons, the shifter's d-pad as a hat)
 * gets its own layout while the built-in one is untouched (Pinboard
 * #1446), and its d-pad is four more buttons. */
static void test_g25(void) {
    sdl_pads_default();
    SDL_VirtualJoystickDesc g;
    SDL_INIT_INTERFACE(&g);
    g.type = SDL_JOYSTICK_TYPE_WHEEL;
    g.vendor_id = 0x046D;
    g.product_id = 0xC299;
    g.naxes = 4;
    g.nbuttons = 19;
    g.nhats = 1;
    g.name = "G25 Racing Wheel";
    SDL_JoystickID g25 = SDL_AttachVirtualJoystick(&g);
    pump();
    CHECK(g_wheel_opt.steer == 0 && g_wheel_opt.accel == 2 && g_wheel_opt.brake == 3, "G25: axes 0, 2, 3");
    CHECK(g_wheel_opt.button[6] == ACT(GAME_INPUT_P1_START) && g_wheel_opt.button[7] == ACT(GAME_INPUT_P1_COIN) &&
          g_wheel_opt.button[12] == ACT(GAME_INPUT_GEAR_1) && g_wheel_opt.button[15] == ACT(GAME_INPUT_GEAR_4) &&
          g_wheel_opt.button[9] == ACT(GAME_INPUT_P2_B3) && !g_wheel_opt.button[16],
          "G25: start 6, coin 7, gears 12-15, VR3 on 9");
    button(g25, 6, true);
    pump();
    CHECK(g_pad_held[GAME_INPUT_P1_START], "G25: button 6 presses Start");
    button(g25, 6, false);
    g_wheel_opt.button[WHEEL_HAT + 2] = ACT(GAME_INPUT_P1_DOWN);
    SDL_SetJoystickVirtualHat(SDL_GetJoystickFromID(g25), 0, SDL_HAT_RIGHTDOWN);
    pump();
    CHECK(g_pad_held[GAME_INPUT_P1_DOWN] && !g_pad_held[GAME_INPUT_P1_START], "G25: the d-pad's down presses");
    SDL_SetJoystickVirtualHat(SDL_GetJoystickFromID(g25), 0, SDL_HAT_CENTERED);
    pump();
    CHECK(!g_pad_held[GAME_INPUT_P1_DOWN], "G25: the d-pad let go");
    uint32_t acts = 0;
    CHECK(actions_by_name("p2:b3", &acts) && acts == ACT(GAME_INPUT_P2_B3) && !actions_by_name("p3:b1", &acts),
          "a wheel button can be P2's B3 (VR3), not player 3's");
    sdl_pads_default();
    g_wheel_opt.button[1] = ACT(GAME_INPUT_P1_B1);   /* a controls file's own layout */
    wheel_preset_apply();
    CHECK(g_wheel_opt.accel == 1 && !g_wheel_opt.button[6], "G25: a changed layout stays as it is");
    SDL_DetachVirtualJoystick(g25);
    pump();
}

int main(void) {
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) { printf("FAIL: SDL_Init: %s\n", SDL_GetError()); return 1; }

    attach_devices();
    test_no_pick();
    test_pedals_pick();
    test_wheel_pick();
    test_gamepad_pick();
    test_gamepad_as_wheel();
    test_pick_text();
    test_unplug();
    test_fanatec_base();
    test_fanatec_twice();
    test_g25();

    SDL_Quit();
    printf("%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
