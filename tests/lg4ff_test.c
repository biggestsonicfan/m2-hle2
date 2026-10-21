/*
 * lg4ff_test.c -- the reports a Logitech G25, G27, DFGT or DFP gets (Pinboard
 * #1370, core/lg4ff_proto.h), each against the bytes Logitech's drivers send
 * (LOGITECH.md). No device.
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

int main(void) {
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
    id = lg4ff_identify(0xC294, 0x1350);
    CHECK(id.model == LG4FF_DFGT && id.compat, "C294 rev 13xx: a DFGT");
    id = lg4ff_identify(0xC294, 0x1102);
    CHECK(id.model == LG4FF_DFP && id.compat, "C294 rev 1xxx: a DFP");
    id = lg4ff_identify(0xC294, 0x2100);
    CHECK(id.model == LG4FF_NONE, "C294 rev 21xx: a real Driving Force EX, left alone");
    id = lg4ff_identify(0xC52B, 0x1200);
    CHECK(id.model == LG4FF_NONE, "a Logitech receiver: none");

    uint8_t s[2][LG4FF_REPORT];
    CHECK(lg4ff_mode_switch(s, LG4FF_G25) == 1 && IS(s[0], 0x00, 0xF8, 0x10), "G25 switch: 00 f8 10");
    CHECK(lg4ff_mode_switch(s, LG4FF_G27) == 2 && IS(s[0], 0x00, 0xF8, 0x0A) && IS(s[1], 0x00, 0xF8, 0x09, 0x04, 0x01),
          "G27 switch: f8 0a, f8 09 04 01");
    CHECK(lg4ff_mode_switch(s, LG4FF_DFGT) == 2 && IS(s[1], 0x00, 0xF8, 0x09, 0x03, 0x01), "DFGT switch: ..f8 09 03 01");
    CHECK(lg4ff_mode_switch(s, LG4FF_DFP) == 1 && IS(s[0], 0x00, 0xF8, 0x01), "DFP switch: f8 01");

    uint8_t r[3][LG4FF_REPORT];
    lg4ff_range(r[0], 270);
    CHECK(IS(r[0], 0x00, 0xF8, 0x81, 0x0E, 0x01), "range 270: f8 81 0e 01");
    lg4ff_range(r[0], 1080);
    CHECK(IS(r[0], 0x00, 0xF8, 0x81, 0x84, 0x03), "range past 900: 900");
    CHECK(lg4ff_has_range(LG4FF_G25) && !lg4ff_has_range(LG4FF_DFP), "f8 81 for the G25, not the DFP");

    lg4ff_constant(r[0], 0);
    CHECK(IS(r[0], 0x00, 0x13, 0x00), "no push: slot 1 stopped (13 00)");
    lg4ff_constant(r[0], 0x7FFF);
    CHECK(IS(r[0], 0x00, 0x11, 0x08, 0xFF, 0x80), "full right: 11 08 ff 80");
    lg4ff_constant(r[0], -0x7FFF);
    CHECK(IS(r[0], 0x00, 0x11, 0x08, 0x01, 0x80), "full left: 11 08 01 80");
    lg4ff_constant(r[0], 0x4000);
    CHECK(r[0][3] == 0x80 + 63, "half right: 0x80 + 63");
    CHECK(lg4ff_force_byte(100) == 0x80, "a push below a step is none");

    CHECK(lg4ff_autocenter(r, 0) == 1 && IS(r[0], 0x00, 0xF5), "no spring: f5");
    CHECK(lg4ff_autocenter(r, 0x7FFF) == 3 && IS(r[0], 0x00, 0xF5) && IS(r[1], 0x00, 0xFE, 0x0D, 0x07, 0x07, 0xFF)
          && IS(r[2], 0x00, 0x14), "full spring: f5, fe 0d 07 07 ff, 14 (Linux's curve)");
    lg4ff_autocenter(r, 0x4000);
    CHECK(r[1][3] == 0x04 && r[1][5] == 0x60, "half spring: fe 0d 04 04 60");

    lg4ff_friction(r[0], 0x4000, false);
    CHECK(IS(r[0], 0x00, 0x81, 0x0E, 0x80, 0x80, 0xFF), "friction starts: 81 0e 80 80 ff");
    lg4ff_friction(r[0], 0x7FFF, true);
    CHECK(IS(r[0], 0x00, 0x8C, 0x0E, 0xFF, 0xFF, 0xFF), "friction changes: 8c 0e ff ff ff");
    lg4ff_friction(r[0], 0, true);
    CHECK(IS(r[0], 0x00, 0x83), "friction stops: 83");

    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
