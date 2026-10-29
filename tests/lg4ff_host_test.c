/*
 * lg4ff_host_test.c -- ui/lg4ff.h on a pretend G25 (Pinboard #1370), then a
 * pretend G29 in its PS4 mode (#1419): SDL's hidapi calls are this file's, so
 * no hardware.
 *
 * The wheel comes up as a Driving Force EX (C294 rev 1200) beside a Logitech
 * mouse: it is sent the G25's mode switch, once, and nothing else. It comes
 * back as C299: it opens and gets its range. The drive board's force goes to
 * it only while SDL has no haptic for it, a report only for what changed;
 * the shake rides on the push. Losing the focus lets everything go; a failed
 * write closes the wheel; --g25 off touches nothing; the run's end closes it.
 * The G29 comes up as C260: it is sent report 0x30's switch and comes back as
 * C24F, which gets its range.
 *
 * A ctest (CMakeLists.txt, lg4ff_host_test; M2HLE_PADS builds only).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* SDL's hidapi, renamed to this file's fakes before SDL declares it. */
#define SDL_hid_init                fake_hid_init
#define SDL_hid_exit                fake_hid_exit
#define SDL_hid_device_change_count fake_hid_device_change_count
#define SDL_hid_enumerate           fake_hid_enumerate
#define SDL_hid_free_enumeration    fake_hid_free_enumeration
#define SDL_hid_open_path           fake_hid_open_path
#define SDL_hid_write               fake_hid_write
#define SDL_hid_close               fake_hid_close
#include "lg4ff.h"

const game_profile_t *g_active_profile = NULL;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

/* ---- The pretend wheel ------------------------------------------------------ */

static const char *COMPAT = "\\\\?\\hid#vid_046d&pid_c294#7&1f&0&0000#{4d1e55b2}";
static const char *NATIVE = "\\\\?\\hid#vid_046d&pid_c299#7&20&0&0000#{4d1e55b2}";
static uint16_t g_compat_pid = 0xC294, g_native_pid = 0xC299, g_rev = 0x1222;
#define MOUSE  "\\\\?\\hid#vid_046d&pid_c077#7&21&0&0000#{4d1e55b2}"

static SDL_hid_device_info g_info[2];
static bool  g_native;                  /* the wheel switched */
static int   g_open[2];                 /* compat, native: open count */
static int   g_inits, g_exits;
static Uint32 g_changes = 1;
static bool  g_fail_writes;
static struct { int dev; uint8_t b[8]; size_t n; } g_w[64];
static int   g_nw;

int fake_hid_init(void) { g_inits++; return 0; }
int fake_hid_exit(void) { g_exits++; return 0; }
Uint32 fake_hid_device_change_count(void) { return g_changes; }

SDL_hid_device_info *fake_hid_enumerate(unsigned short vid, unsigned short pid) {
    (void)pid;
    if (vid != LOGITECH_VID) return NULL;
    memset(g_info, 0, sizeof g_info);
    g_info[0].path = (char *)MOUSE;
    g_info[0].vendor_id = LOGITECH_VID;
    g_info[0].product_id = 0xC077;
    g_info[0].release_number = 0x1200;
    g_info[0].next = &g_info[1];
    g_info[1].path = (char *)(g_native ? NATIVE : COMPAT);
    g_info[1].vendor_id = LOGITECH_VID;
    g_info[1].product_id = g_native ? g_native_pid : g_compat_pid;
    g_info[1].release_number = g_rev;
    return &g_info[0];
}
void fake_hid_free_enumeration(SDL_hid_device_info *d) { (void)d; }

SDL_hid_device *fake_hid_open_path(const char *path) {
    if (!strcmp(path, COMPAT) && !g_native) { g_open[0]++; return (SDL_hid_device *)&g_open[0]; }
    if (!strcmp(path, NATIVE) && g_native)  { g_open[1]++; return (SDL_hid_device *)&g_open[1]; }
    return NULL;
}
int fake_hid_write(SDL_hid_device *dev, const unsigned char *data, size_t length) {
    if (g_fail_writes) return -1;
    if (g_nw < 64) {
        g_w[g_nw].dev = (int *)dev == &g_open[1] ? 1 : 0;
        memcpy(g_w[g_nw].b, data, length > 8 ? 8 : length);
        g_w[g_nw].n = length;
        g_nw++;
    }
    return (int)length;
}
int fake_hid_close(SDL_hid_device *dev) { (*(int *)dev)--; return 0; }

/* Was an 8-byte report, ID 0, these bytes after it, written to `dev`? */
static bool sent(int dev, const uint8_t *b, size_t n) {
    for (int i = 0; i < g_nw; i++)
        if (g_w[i].dev == dev && g_w[i].n == 8 && !g_w[i].b[0] && !memcmp(g_w[i].b + 1, b, n)) return true;
    return false;
}
#define SENT(dev, ...) sent(dev, (const uint8_t[]){ __VA_ARGS__ }, sizeof((const uint8_t[]){ __VA_ARGS__ }))

/* The forces the tests send, shared by the groups. */
static ffb_out_t none, race;

static void test_switch(void) {
    lg4ff_update(&none, false);
    CHECK(g_inits == 1 && g_nw == 1 && SENT(0, 0xF8, 0x10), "a G25 in compat mode: f8 10, and nothing else");
    CHECK(g_open[0] == 0 && g_open[1] == 0, "...closed again");

    g_nw = 0;
    g_changes++;                        /* the mouse comes, the wheel still compat */
    lg4ff_update(&none, false);
    CHECK(g_nw == 0, "the switch goes once");

    g_native = true;
    g_changes++;
    lg4ff_update(&none, false);
    CHECK(g_open[1] == 1, "back as C299: the G25 opens");
    CHECK(g_nw == 1 && SENT(1, 0xF8, 0x81, 0x0E, 0x01), "...its range 270, and no force");
}

static void test_forces(void) {
    g_nw = 0;
    race = (ffb_out_t){ .push = 0x7FFF, .spring = 0x4000, .damper = 0x2000 };
    lg4ff_update(&race, false);
    CHECK(SENT(1, 0x11, 0x08, 0xFF, 0x80), "a push: the constant force");
    CHECK(SENT(1, 0xF5) && SENT(1, 0xFE, 0x0D, 0x04, 0x04, 0x60) && SENT(1, 0x14), "the centring: the wheel's spring");
    CHECK(SENT(1, 0x81, 0x0C, 0x04, 0x00, 0x04, 0x00, 0x40), "the damper slot started");
    CHECK(g_nw == 5, "...five reports");

    g_nw = 0;
    lg4ff_update(&race, false);
    CHECK(g_nw == 0, "the same force: nothing");

    race.damper = 0x4000;
    lg4ff_update(&race, false);
    CHECK(g_nw == 1 && SENT(1, 0x8C, 0x0C, 0x08, 0x00, 0x08, 0x00, 0x80), "a new damper: only that, as a change");

    g_nw = 0;
    ffb_out_t grass = { .spring = -0x2FFF };
    lg4ff_update(&grass, false);
    CHECK(SENT(1, 0x13, 0x00) && SENT(1, 0xF5) && SENT(1, 0x83), "the grass: no push, the centring and the damper off");
    CHECK(SENT(1, 0x41, 0x0B, 0x80, 0x80, 0x44, 0x11, 0x5F) && g_nw == 4, "...the uncentring started in its slot");
    g_nw = 0;
    grass.spring = 0x2FFF;
    lg4ff_update(&grass, false);
    CHECK(SENT(1, 0x43) && SENT(1, 0xFE, 0x0D, 0x03, 0x03, 0x47) && g_nw == 4, "the centring back: the slot stopped, the spring on");
    ffb_out_t kick = { .push = 0x7FFF };
    lg4ff_update(&kick, false);
}

static void test_let_go(void) {
    g_nw = 0;
    lg4ff_update(&race, true);
    CHECK(SENT(1, 0x13, 0x00) && g_nw == 1, "SDL has the haptic: ours let go");
    g_nw = 0;
    lg4ff_update(&race, true);
    CHECK(g_nw == 0, "...once");

    lg4ff_update(&race, false);
    g_nw = 0;
    lg4ff_release();
    CHECK(SENT(1, 0x13, 0x00) && SENT(1, 0xF5) && SENT(1, 0x83), "the focus lost: everything let go");
}

static void test_reopen(void) {
    g_fail_writes = true;
    lg4ff_update(&race, false);
    CHECK(g_open[1] == 0, "a failed write closes the wheel");
    g_fail_writes = false;
    g_nw = 0;
    g_changes++;
    lg4ff_update(&race, false);
    CHECK(g_open[1] == 1 && SENT(1, 0xF8, 0x81, 0x0E, 0x01) && SENT(1, 0x11, 0x08, 0xFF, 0x80),
          "a device comes: found again, range and force back");

    g_nw = 0;
    lg4ff_shutdown();
    CHECK(SENT(1, 0x13, 0x00) && g_open[1] == 0 && g_exits == 1, "the run's end: let go, closed, hidapi done");

    g_nw = 0;
    g_lg4ff_on = false;
    lg4ff_update(&race, false);
    CHECK(g_nw == 0 && g_inits == 1, "--g25 off: nothing");
}

static void test_options(void) {
    CHECK(lg4ff_option("--g25-range", "1080") && g_lg4ff_range == 900, "--g25-range 1080 is 900");
    CHECK(lg4ff_option("--g25-range", "0") && g_lg4ff_range == 0, "--g25-range 0 leaves it");
    CHECK(lg4ff_option("--g25", "on") && g_lg4ff_on && !lg4ff_option("--g26", "on"), "--g25 on");
}

/* A G29 in its PS4 mode. */
static void test_g29(void) {
    COMPAT = "\\\\?\\hid#vid_046d&pid_c260#7&22&0&0000#{4d1e55b2}";
    NATIVE = "\\\\?\\hid#vid_046d&pid_c24f#7&23&0&0000#{4d1e55b2}";
    g_compat_pid = 0xC260, g_native_pid = 0xC24F, g_rev = 0x8900;
    g_native = false;
    g_lg4ff_range = 270;
    g_nw = 0;
    lg4ff_update(&none, false);
    CHECK(g_nw == 1 && g_w[0].dev == 0 && g_w[0].n == 6
          && !memcmp(g_w[0].b, (const uint8_t[]){ 0x30, 0xF8, 0x09, 0x05, 0x01, 0x01 }, 6),
          "a G29 in PS4 mode: report 30 f8 09 05 01 01, 6 bytes, and nothing else");
    g_native = true;
    g_changes++;
    g_nw = 0;
    lg4ff_update(&none, false);
    CHECK(g_open[1] == 1 && g_nw == 1 && SENT(1, 0xF8, 0x81, 0x0E, 0x01), "back as C24F: the G29 opens, range 270");
    lg4ff_shutdown();
}

int main(void) {
    test_switch();
    test_forces();
    test_let_go();
    test_reopen();
    test_options();
    test_g29();

    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
