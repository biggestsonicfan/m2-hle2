/*
 * lg4ff_test.c -- the reports a Logitech G25, G27, G29, DFGT or DFP gets
 * (Pinboard #1370, the G29 #1419; core/lg4ff_proto.h), each against the bytes
 * Logitech's drivers send (LOGITECH.md). No device.
 *
 * A ctest (CMakeLists.txt, lg4ff_test).
 */
#include <stdio.h>
#include <string.h>

#include "lg4ff_proto.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

static bool is(const uint8_t *r, const uint8_t *want) { return !memcmp(r, want, LG4FF_REPORT); }
#define IS(r, ...) is(r, (const uint8_t[LG4FF_REPORT]){ __VA_ARGS__ })

static void test_identify_g25_g27(void) {
    lg4ff_id_t id;
    id = lg4ff_identify(0xC294, 0x1200);
    CHECK(id.model == LG4FF_G25 && id.compat, "C294 rev 12xx: a G25 in compatibility mode");
    id = lg4ff_identify(0xC298, 0x1221);
    CHECK(id.model == LG4FF_G25 && id.compat, "C298 rev 12xx: a G25 in DFP mode");
    id = lg4ff_identify(0xC299, 0x1222);
    CHECK(id.model == LG4FF_G25 && !id.compat, "C299: the G25 itself");
    id = lg4ff_identify(0xC294, 0x1232);
    CHECK(id.model == LG4FF_G27 && id.compat, "rev 123x: a G27, under C294");
    id = lg4ff_identify(0xC299, 0x1230);
    CHECK(id.model == LG4FF_G27 && id.compat, "...and under C299 (G25 mode)");
    id = lg4ff_identify(0xC29B, 0x1230);
    CHECK(id.model == LG4FF_G27 && !id.compat, "C29B: the G27 itself");
    id = lg4ff_identify(0xC294, 0x1340);
    CHECK(id.model == LG4FF_DFGT && id.compat, "C294 rev 13xx below 1350: a DFGT");
    id = lg4ff_identify(0xC294, 0x1350);
    CHECK(id.model == LG4FF_G29 && id.compat && !id.ps4, "C294 rev 1350: a G29 in its PS3 mode, not a DFGT");
    id = lg4ff_identify(0xC29A, 0x1352);
    CHECK(id.model == LG4FF_G29 && id.compat, "...under C29A too (LCore.exe)");
}

static void test_identify_g29_others(void) {
    lg4ff_id_t id;
    id = lg4ff_identify(0xC294, 0x8900);
    CHECK(id.model == LG4FF_G29 && id.compat, "...and rev 89xx (Linux)");
    id = lg4ff_identify(0xC260, 0x0100);
    CHECK(id.model == LG4FF_G29 && id.compat && id.ps4, "C260: a G29 in its PS4 mode");
    id = lg4ff_identify(0xC24F, 0x0100);
    CHECK(id.model == LG4FF_G29 && !id.compat, "C24F: the G29 itself");
    id = lg4ff_identify(0xC29A, 0x1300);
    CHECK(id.model == LG4FF_DFGT && !id.compat, "C29A rev 1300: the DFGT itself");
    id = lg4ff_identify(0xC294, 0x1102);
    CHECK(id.model == LG4FF_DFP && id.compat, "C294 rev 1xxx: a DFP");
    id = lg4ff_identify(0xC294, 0x2100);
    CHECK(id.model == LG4FF_NONE, "C294 rev 21xx: a real Driving Force EX, left alone");
    id = lg4ff_identify(0xC52B, 0x1200);
    CHECK(id.model == LG4FF_NONE, "a Logitech receiver: none");
}

static void test_mode_switch(void) {
    uint8_t s[2][LG4FF_REPORT];
    int len = 0;
#define SW(m) lg4ff_mode_switch(s, (lg4ff_id_t){ m, true, false }, &len)
    CHECK(SW(LG4FF_G25) == 1 && len == 8 && IS(s[0], 0x00, 0xF8, 0x10), "G25 switch: 00 f8 10");
    CHECK(SW(LG4FF_G27) == 2 && IS(s[0], 0x00, 0xF8, 0x0A) && IS(s[1], 0x00, 0xF8, 0x09, 0x04, 0x01),
          "G27 switch: f8 0a, f8 09 04 01");
    CHECK(SW(LG4FF_DFGT) == 2 && IS(s[1], 0x00, 0xF8, 0x09, 0x03, 0x01), "DFGT switch: ..f8 09 03 01");
    CHECK(SW(LG4FF_DFP) == 1 && IS(s[0], 0x00, 0xF8, 0x01), "DFP switch: f8 01");
    CHECK(SW(LG4FF_G29) == 1 && len == 8 && IS(s[0], 0x00, 0xF8, 0x09, 0x05, 0x01), "G29 (PS3 mode) switch: 00 f8 09 05 01");
    CHECK(lg4ff_mode_switch(s, lg4ff_identify(0xC260, 0), &len) == 1 && len == 6
          && !memcmp(s[0], (const uint8_t[]){ 0x30, 0xF8, 0x09, 0x05, 0x01, 0x01 }, 6),
          "G29 (PS4 mode) switch: report 30, f8 09 05 01 01, 6 bytes");
#undef SW
}

/* The reports the force tests build. */
static uint8_t r[3][LG4FF_REPORT];

static void test_range(void) {
    lg4ff_range(r[0], 270);
    CHECK(IS(r[0], 0x00, 0xF8, 0x81, 0x0E, 0x01), "range 270: f8 81 0e 01");
    lg4ff_range(r[0], 1080);
    CHECK(IS(r[0], 0x00, 0xF8, 0x81, 0x84, 0x03), "range past 900: 900");
    CHECK(lg4ff_has_range(LG4FF_G25) && lg4ff_has_range(LG4FF_G29) && !lg4ff_has_range(LG4FF_DFP),
          "f8 81 for the G25 and G29, not the DFP");
}

static void test_constant(void) {
    lg4ff_constant(r[0], 0);
    CHECK(IS(r[0], 0x00, 0x13, 0x00), "no push: slot 1 stopped (13 00)");
    lg4ff_constant(r[0], 0x7FFF);
    CHECK(IS(r[0], 0x00, 0x11, 0x08, 0xFF, 0x80), "full right: 11 08 ff 80");
    lg4ff_constant(r[0], -0x7FFF);
    CHECK(IS(r[0], 0x00, 0x11, 0x08, 0x01, 0x80), "full left: 11 08 01 80");
    lg4ff_constant(r[0], 0x4000);
    CHECK(r[0][3] == 0x80 + 63, "half right: 0x80 + 63");
    CHECK(lg4ff_force_byte(100) == 0x80, "a push below a step is none");
}

static void test_autocenter(void) {
    CHECK(lg4ff_autocenter(r, 0) == 1 && IS(r[0], 0x00, 0xF5), "no spring: f5");
    CHECK(lg4ff_autocenter(r, 0x7FFF) == 3 && IS(r[0], 0x00, 0xF5) && IS(r[1], 0x00, 0xFE, 0x0D, 0x07, 0x07, 0xFF)
          && IS(r[2], 0x00, 0x14), "full spring: f5, fe 0d 07 07 ff, 14 (Linux's curve)");
    lg4ff_autocenter(r, 0x4000);
    CHECK(r[1][3] == 0x04 && r[1][5] == 0x60, "half spring: fe 0d 04 04 60");
}

static void test_damper(void) {
    lg4ff_damper(r[0], 0x4000, false);
    CHECK(IS(r[0], 0x00, 0x81, 0x0C, 0x08, 0x00, 0x08, 0x00, 0x80), "damper starts: 81 0c 08 00 08 00 80");
    lg4ff_damper(r[0], 0x7FFF, true);
    CHECK(IS(r[0], 0x00, 0x8C, 0x0C, 0x0F, 0x00, 0x0F, 0x00, 0xFF), "damper changes: 8c 0c 0f 00 0f 00 ff");
    lg4ff_damper(r[0], 0, true);
    CHECK(IS(r[0], 0x00, 0x83), "damper stops: 83");
}

static void test_uncenter(void) {
    lg4ff_uncenter(r[0], 0x2FFF, false);
    CHECK(IS(r[0], 0x00, 0x41, 0x0B, 0x80, 0x80, 0x44, 0x11, 0x5F),
          "uncentring 2 starts: 41 0b, the centre, k 4 both sides negative, clip 5f");
    lg4ff_uncenter(r[0], 0x7FFF, true);
    CHECK(IS(r[0], 0x00, 0x4C, 0x0B, 0x80, 0x80, 0xEE, 0x11, 0xFF), "uncentring 7 changes: 4c 0b 80 80 ee 11 ff");
    lg4ff_uncenter(r[0], 0, true);
    CHECK(IS(r[0], 0x00, 0x43), "uncentring stops: 43");
}

int main(void) {
    test_identify_g25_g27();
    test_identify_g29_others();
    test_mode_switch();
    test_range();
    test_constant();
    test_autocenter();
    test_damper();
    test_uncenter();

    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
