/*
 * fanatec_test.c -- the reports a Fanatec base is sent for a game's meters
 * (core/fanatec_proto.h), Pinboard #1361, byte for byte as Fanatec's SDK
 * builds them (FANATEC.md). No device, no ROM.
 *
 * A ctest (CMakeLists.txt, fanatec_test).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fanatec_proto.h"

const game_profile_t *g_active_profile = NULL;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

static bool same(const uint8_t *a, const uint8_t *b, size_t n) { return memcmp(a, b, n) == 0; }

int main(void) {
    CHECK(fanatec_is_base(0x0006) && fanatec_is_base(0x0E03) && fanatec_is_base(0x0020), "DD1, CSL Elite PS4, DD: bases");
    CHECK(!fanatec_is_base(0x183B) && !fanatec_is_base(0x6204), "pedals, shifter: not bases");
    CHECK(fanatec_multi_tlc(0x0006) && fanatec_multi_tlc(0x0020) && !fanatec_multi_tlc(0x0001) && !fanatec_multi_tlc(0x0E03),
          "multi-TLC: DD1 and DD yes, CSW V2 and CSL Elite PS4 no");

    uint8_t r[FANATEC_REPORT64];
    fanatec_base_revs(r, false, 0x1FF);
    CHECK(same(r, (const uint8_t[]){ 0x00, 0xF8, 0x13, 0xFF, 0x01, 0, 0, 0 }, 8), "base revs, all lit: 00 f8 13 ff 01");
    fanatec_base_revs(r, true, 0x003);
    CHECK(same(r, (const uint8_t[]){ 0x01, 0xF8, 0x13, 0x03, 0x00, 0, 0, 0 }, 8), "base revs on multi-TLC: report ID 1");
    fanatec_rim_revs(r, false, 0x001);
    CHECK(same(r, (const uint8_t[]){ 0x00, 0xF8, 0x09, 0x08, 0x01, 0x00, 0, 0 }, 8), "rim revs: LED 0 is bit 8");
    fanatec_rim_revs(r, false, 0x100);
    CHECK(same(r, (const uint8_t[]){ 0x00, 0xF8, 0x09, 0x08, 0x00, 0x01, 0, 0 }, 8), "rim revs: LED 8 is bit 0");
    fanatec_rev_translator(r, false, true);
    CHECK(same(r, (const uint8_t[]){ 0x00, 0xF8, 0x09, 0x07, 0x01, 0, 0, 0 }, 8), "stripe translator on: f8 09 07 01");

    fanatec_rgb_revs(r, 0x109);   /* LEDs 0, 3, 8 */
    CHECK(r[0] == 0xFF && r[1] == 0x01 && r[2] == 0x00, "RGB colours: ff 01 00");
    CHECK(r[3] == 0x07 && r[4] == 0xE0, "RGB LED 0 green: 0x07E0 big-endian");
    CHECK(r[9] == 0x00 && r[10] == 0x1F, "RGB LED 3 red: 0x001F (red low)");
    CHECK(r[19] == 0xF8 && r[20] == 0x00, "RGB LED 8 blue: 0xF800");
    CHECK(r[5] == 0 && r[6] == 0, "RGB LED 1 dark");

    CHECK(fanatec_seg('0') == 0x3F && fanatec_seg('8') == 0x7F && fanatec_seg('N') == 0x54 && fanatec_seg('r') == 0x50,
          "the SDK's font: 0 8 N R");
    CHECK(fanatec_seg(' ') == 0 && fanatec_seg('-') == 0x40, "blank, and the middle bar");
    fanatec_display(r, false, "1 3");
    CHECK(same(r, (const uint8_t[]){ 0x00, 0xF8, 0x09, 0x01, 0x02, 0x06, 0x00, 0x4F }, 8), "display: f8 09 01 02 d0 d1 d2");
    fanatec_display_mode(r, true, true);
    CHECK(same(r, (const uint8_t[]){ 0x01, 0xF8, 0x09, 0x01, 0x18, 0x02, 0, 0 }, 8), "display ours: f8 09 01 18 02");
    fanatec_display_mode(r, false, false);
    CHECK(same(r, (const uint8_t[]){ 0x00, 0xF8, 0x09, 0x01, 0x18, 0x01, 0, 0 }, 8), "display back: f8 09 01 18 01");

    /* Daytona's meters: 8500 at the limiter, the lights from 5000. */
    game_dash_t d = { .on = true, .speed = 187, .gear = 3, .rpm = 4000, .rpm_lo = 5000, .rpm_max = 8500 };
    CHECK(fanatec_rev_mask(&d, false) == 0, "below rpm_lo: dark");
    d.rpm = 5000;
    CHECK(fanatec_rev_mask(&d, false) == 0x001, "at rpm_lo: one LED");
    d.rpm = 8499;
    CHECK(fanatec_rev_mask(&d, false) == 0x1FF, "just under the limiter: all nine");
    d.rpm = 6750;
    CHECK(fanatec_rev_mask(&d, false) == 0x01F, "half way: five");
    d.rpm = 8500;
    CHECK(fanatec_rev_mask(&d, false) == 0x1FF && fanatec_rev_mask(&d, true) == 0, "at the limiter: all, flashing");
    d.on = false;
    CHECK(fanatec_rev_mask(&d, false) == 0, "no car: dark");

    char t[4];
    d.on = true;
    fanatec_dash_text(&d, false, t);
    CHECK(!strcmp(t, " 3 "), "gear in the middle");
    d.gear = 0;
    fanatec_dash_text(&d, false, t);
    CHECK(!strcmp(t, " N "), "neutral");
    fanatec_dash_text(&d, true, t);
    CHECK(!strcmp(t, "187"), "speed");
    d.speed = 7;
    fanatec_dash_text(&d, true, t);
    CHECK(!strcmp(t, "  7"), "a slow speed, right-aligned");
    d.gear = -1; d.speed = 1234;
    fanatec_dash_text(&d, false, t);
    CHECK(!strcmp(t, "999"), "no gears: the speed, at most 999");
    d.speed = -1;
    fanatec_dash_text(&d, false, t);
    CHECK(!strcmp(t, "   "), "nothing known: blank");

    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
