/*
 * wheel_host_test.c -- a host's analog wheel and pedals on the 315-5649
 * (board/io_5649.h, ui/wheel_host.h), Pinboard #1082. No ROM.
 *
 * The wheel's pot reads where the host's wheel is turned, a pedal how far it
 * is down; a held bit still floors a pedal and LEFT / RIGHT still walk the
 * wheel while the host's is centred; a session, follow or replay reads only
 * the held bits; and with no host device nothing changes.
 *
 * A ctest (CMakeLists.txt, wheel_host_test).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wheel_host.h"

const game_profile_t *g_active_profile = NULL;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

enum { K_LEFT = 0x8000, K_RIGHT = 0x4000, K_ACCEL = 0x0100, K_BRAKE = 0x0200 };
static uint32_t g_held;
static int g_net, g_remote;
static uint32_t held(void)  { return g_held; }
static int from_net(void)   { return g_net; }
static int remote(void)     { return g_remote; }

static int steer_after(int frames) {
    g_irqt.vbl_count += (uint64_t)frames;
    io5649_advance();
    return io5649_analog(0);
}

int main(void) {
    CHECK(wheel_host_steer(0, WHEEL_HOST_STICK_DEAD) == 0 && wheel_host_steer(2000, WHEEL_HOST_STICK_DEAD) == 0,
          "steer: inside the dead zone is the centre");
    CHECK(wheel_host_steer(32767, WHEEL_HOST_STICK_DEAD) == 32767 && wheel_host_steer(-32768, WHEEL_HOST_STICK_DEAD) == -32767,
          "steer: full lock both ways");
    CHECK(wheel_host_steer_centred(3000, 3000) == 0 && wheel_host_steer_centred(32767, 3000) == 32767 &&
          wheel_host_steer_centred(-32768, 3000) == -32767,
          "a wheel straight at 3000: 0 there, the full turn either side (#1439)");
    CHECK(wheel_host_steer_centred(-14884, 3000) > -16384 - 2 && wheel_host_steer_centred(-14884, 3000) < -16384 + 2,
          "half way to the left end from 3000: half the turn");
    CHECK(wheel_host_steer_centred(1234, 0) == wheel_host_steer(1234, 0), "straight at 0: as without a centre");
    CHECK(wheel_host_pedal(32767, 32767) == 0 && wheel_host_pedal(-32768, 32767) == 32767,
          "pedal resting high: up 0, down full");
    CHECK(wheel_host_pedal(-32768, -32768) == 0 && wheel_host_pedal(0, -32768) > 16000 && wheel_host_pedal(0, -32768) < 16500,
          "pedal resting low: half way is half");

    static game_profile_t prof;
    g_active_profile = &prof;
    wheel_host_set(true, 32767, 0, 0);
    CHECK(!g_io5649_host.on, "no wheel in the game: the host's is not used");
    prof.quirks.io_5649 = true;

    g_io5649_controls = (io5649_controls_t){ K_LEFT, K_RIGHT, K_ACCEL, K_BRAKE, 0, 0 };
    g_io5649_held_fn = held;
    g_io5649_from_net_fn = from_net;
    g_io5649_remote_fn = remote;
    io5649_reset();

    CHECK(steer_after(1) == 0x80 && io5649_analog(1) == IO5649_PEDAL_OFF, "power-on: centred, pedal up");
    g_held = K_RIGHT;
    CHECK(steer_after(1) == 0x8A, "no host wheel: RIGHT walks it 10 a frame");
    g_held = 0;
    steer_after(20);

    wheel_host_set(true, 32767, 32767, 16384);
    CHECK(steer_after(0) == 0x80 + 0x58, "host wheel full right: the pot at once, no frame needed");
    wheel_host_set(true, -16384, 32767, 16384);
    CHECK(steer_after(0) == 0x80 - 0x2C, "host wheel half left");
    CHECK(io5649_analog(1) == IO5649_PEDAL_ON && io5649_analog(2) == 0x80, "pedals: floored, half way");
    g_held = K_BRAKE;
    CHECK(io5649_analog(2) == IO5649_PEDAL_ON, "a held brake floors it");

    wheel_host_set(true, 0, 0, 0);
    g_held = K_LEFT;
    CHECK(steer_after(1) == 0x80 - 0x2C - 10, "host wheel centred: LEFT still walks it, from where it was");
    wheel_host_set(true, 1000, 0, 0);
    CHECK(steer_after(1) == 0x80 + 3, "host wheel turned: it wins over LEFT");

    g_held = 0;
    wheel_host_set(true, 32767, 32767, 0);
    g_remote = 1;
    CHECK(steer_after(1) == 0x80 && io5649_analog(1) == IO5649_PEDAL_OFF,
          "driven from elsewhere: the host's wheel and pedal are not read (it walks back)");
    g_remote = 0;
    g_net = 1;
    CHECK(io5649_analog(1) == IO5649_PEDAL_OFF, "a networked board: nor here");
    g_net = 0;

    wheel_host_set(false, 32767, 32767, 32767);
    CHECK(io5649_analog(1) == IO5649_PEDAL_OFF, "host off: the held bits alone");

    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
