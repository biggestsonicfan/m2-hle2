/*
 * ffb_test.c -- the drive board's commands (board/drive_board.h) and what the
 * hosts play for them (core/ffb.h), Pinboard #1047. No ROM.
 *
 * Daytona's own commands decode to their effects (the table in drive_board.h),
 * a pad rumbles only for the pushes and the shake, a wheel gets the spring and
 * friction too, --ffb-gain scales it all and 0 turns it off, and a game with
 * no drive board gets nothing. Sega Rally's port E (Pinboard #1082) speaks its
 * own language: a torque of 1-32 to one side, a pad rumbling for the hard half.
 *
 * A ctest (CMakeLists.txt, ffb_test).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ffb.h"

const game_profile_t *g_active_profile = NULL;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

static ffb_out_t play(uint8_t cmd) {
    g_model1io.drive = cmd;
    return ffb_poll();
}

static game_profile_t prof;

static void test_decode(void) {
    drive_effect_t e = drive_board_decode(0x27);
    CHECK(e.kind == DRIVE_CLUTCH && e.level == 7, "0x27: clutch 7 (standing)");
    e = drive_board_decode(0x3B);
    CHECK(e.kind == DRIVE_CENTER && e.level == 3, "0x3B: centring 3 (at speed)");
    e = drive_board_decode(0x42);
    CHECK(e.kind == DRIVE_UNCENTER && e.level == 2, "0x42: uncentring 2 (the grass)");
    e = drive_board_decode(0x57);
    CHECK(e.kind == DRIVE_PUSH && e.dir == +1 && e.level == 7, "0x57: push right 7 (a crash)");
    e = drive_board_decode(0x63);
    CHECK(e.kind == DRIVE_PUSH && e.dir == -1 && e.level == 3, "0x63: push left 3 (contact)");
    e = drive_board_decode(0x10);
    CHECK(e.kind == DRIVE_SPRING && e.level == 0, "0x10: spring 0 (spinning)");
    CHECK(drive_board_decode(0x71).kind == DRIVE_NONE && drive_board_decode(0xFF).kind == DRIVE_NONE
          && drive_board_decode(0x07).kind == DRIVE_NONE && drive_board_decode(0x07).level == 0,
          "set-up bytes 0x07 0x71 0xFF: no force");
}

static void test_play(void) {
    static const uint8_t rom[1];
    g_active_profile = &prof;
    g_model1io_rom = rom;
    CHECK(play(0x57).strong == 0, "no drive board (no quirks.model1_io): no force");
    prof.quirks.model1_io = true;

    ffb_out_t o = play(0x27);
    CHECK(!o.strong && !o.weak && o.damper > 0x6000 && !o.spring, "clutch: a wheel's damper, a pad feels nothing");
    o = play(0x38);
    CHECK(!o.strong && !o.weak && o.spring == 0, "centring 0: nothing");
    o = play(0x3F);
    CHECK(!o.strong && o.spring == 0x7FFF, "centring 7: a wheel's full spring");
    o = play(0x42);
    CHECK(o.weak > o.strong && o.spring == -0x2FFF && !o.push && !o.damper,
          "uncentring 2: a spring the wrong way round, the weak motor");
    CHECK(play(0x40).spring < 0 && play(0x47).spring == -0x7FFF, "uncentring 0 still pushes, 7 the most");
}

static void test_push_gain(void) {
    ffb_out_t lo = play(0x60), hi = play(0x67);
    CHECK(hi.strong > lo.strong && lo.strong > 0 && hi.push < lo.push && lo.push < 0,
          "push left: stronger with the level, negative force");
    ffb_out_t o = play(0x57);
    CHECK(o.push > 0 && o.strong == hi.strong, "push right: the same kick, positive force");

    CHECK(ffb_option("--ffb-gain", "50") && g_ffb_gain == 50, "--ffb-gain 50");
    CHECK(play(0x57).strong == o.strong / 2, "gain 50: half the motor");
    ffb_option("--ffb-gain", "0");
    o = play(0x57);
    CHECK(!o.strong && !o.weak && !o.push && o.effect.kind == DRIVE_PUSH, "gain 0: off, the command still read");
    CHECK(!ffb_option("--wheel-range", "1"), "other options are not ffb's");
    ffb_option("--ffb-gain", "100");
}

/* Sega Rally: the 315-5649's port E (IO+0x08). */
static void test_rally_decode(void) {
    drive_effect_t e = drive_board_decode_srally(0x80);
    CHECK(e.kind == DRIVE_TORQUE && e.dir == +1 && e.level == 1, "rally 0x80: torque right 1");
    e = drive_board_decode_srally(0xDF);
    CHECK(e.kind == DRIVE_TORQUE && e.dir == -1 && e.level == 32, "rally 0xDF: torque left 32");
    CHECK(drive_board_decode_srally(0x00).kind == DRIVE_NONE && drive_board_decode_srally(0x15).kind == DRIVE_NONE
          && drive_board_decode_srally(0xFF).kind == DRIVE_NONE && drive_board_decode_srally(0xA0).kind == DRIVE_NONE,
          "rally 0x00 0x15 0xA0 0xFF: no force");
}

static void test_rally_poll(void) {
    static uint8_t io[0x40];
    memset(io, 0xFF, sizeof io);
    prof.quirks.model1_io = false;
    prof.quirks.io_5649 = true;
    g_io5649_regs = io;
    g_model1io.drive = 0x57;   /* Daytona's board is not this game's */
    CHECK(ffb_poll().effect.kind == DRIVE_NONE, "rally: port E 0xFF before the first write, no force");
    io[0x08] = 0x8F;
    ffb_out_t o = ffb_poll();
    CHECK(o.effect.kind == DRIVE_TORQUE && o.push > 0 && !o.strong && !o.weak, "rally torque 16 right: a wheel's push, no rumble");
    io[0x08] = 0xD0;
    ffb_out_t t17 = ffb_poll();
    io[0x08] = 0xDF;
    ffb_out_t t32 = ffb_poll();
    CHECK(t17.push < 0 && t32.push == -0x7FFF && t17.strong >= 0x4000 && t32.strong > t17.strong && t32.weak == t32.strong / 3,
          "rally torque left 17 / 32: rumble from 17, full push at 32");
    g_io5649_regs = NULL;
    CHECK(ffb_poll().effect.kind == DRIVE_NONE, "rally: no I/O board yet, no force");
}

int main(void) {
    test_decode();
    test_play();
    test_push_gain();
    test_rally_decode();
    test_rally_poll();

    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
