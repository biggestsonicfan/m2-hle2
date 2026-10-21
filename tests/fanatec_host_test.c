/*
 * fanatec_host_test.c -- ui/fanatec.h on a pretend Fanatec base (Pinboard
 * #1361): SDL's hidapi calls are this file's, so no hardware.
 *
 * A Windows-style DD1 lists three collections beside a pedal set: the base's
 * first one opens and its third takes the 0xFF reports. A race lights the rev
 * LEDs (base, rim and RGB) and takes the display for the gear; the same
 * meters again send nothing, a new rpm only the LEDs. A game with no meters
 * gives everything back, and the run's end turns the stripe translator off
 * and closes both. A failed write closes the base, found again when a device
 * comes.
 *
 * A ctest (CMakeLists.txt, fanatec_host_test; M2HLE_PADS builds only).
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
#include "fanatec.h"

const game_profile_t *g_active_profile = NULL;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

/* ---- The pretend base ------------------------------------------------------ */

#define COL01 "\\\\?\\hid#vid_0eb7&pid_0006&mi_00&col01#8&2a1b&0&0000#{4d1e55b2}"
#define COL02 "\\\\?\\hid#vid_0eb7&pid_0006&mi_00&col02#8&2a1b&0&0001#{4d1e55b2}"
#define COL03 "\\\\?\\hid#vid_0eb7&pid_0006&mi_00&col03#8&2a1b&0&0002#{4d1e55b2}"
#define PEDAL "\\\\?\\hid#vid_0eb7&pid_183b&col01#8&99&0&0000#{4d1e55b2}"

static SDL_hid_device_info g_info[4];
static int   g_open[2];                 /* col01, col03: open count */
static int   g_inits, g_exits;
static Uint32 g_changes = 1;
static bool  g_fail_writes;
static struct { int dev; uint8_t b[64]; size_t n; } g_w[64];
static int   g_nw;

int fake_hid_init(void) { g_inits++; return 0; }
int fake_hid_exit(void) { g_exits++; return 0; }
Uint32 fake_hid_device_change_count(void) { return g_changes; }

SDL_hid_device_info *fake_hid_enumerate(unsigned short vid, unsigned short pid) {
    (void)pid;
    if (vid != FANATEC_VID) return NULL;
    const char *paths[4] = { PEDAL, COL02, COL01, COL03 };
    const unsigned short pids[4] = { 0x183B, 0x0006, 0x0006, 0x0006 };
    for (int i = 0; i < 4; i++) {
        memset(&g_info[i], 0, sizeof g_info[i]);
        g_info[i].path = (char *)paths[i];
        g_info[i].vendor_id = FANATEC_VID;
        g_info[i].product_id = pids[i];
        g_info[i].product_string = (wchar_t *)L"Podium Wheel Base DD1";
        g_info[i].next = i < 3 ? &g_info[i + 1] : NULL;
    }
    return &g_info[0];
}
void fake_hid_free_enumeration(SDL_hid_device_info *d) { (void)d; }

/* A handle is the address of its open count. */
SDL_hid_device *fake_hid_open_path(const char *path) {
    if (!strcmp(path, COL01)) { g_open[0]++; return (SDL_hid_device *)&g_open[0]; }
    if (!strcmp(path, COL03)) { g_open[1]++; return (SDL_hid_device *)&g_open[1]; }
    return NULL;   /* only the first and third collections take writes */
}
int fake_hid_write(SDL_hid_device *dev, const unsigned char *data, size_t length) {
    if (g_fail_writes) return -1;
    if (g_nw < 64) {
        g_w[g_nw].dev = (int *)dev == &g_open[1] ? 3 : 1;
        memcpy(g_w[g_nw].b, data, length > 64 ? 64 : length);
        g_w[g_nw].n = length;
        g_nw++;
    }
    return (int)length;
}
int fake_hid_close(SDL_hid_device *dev) { (*(int *)dev)--; return 0; }

/* Was a report starting with these bytes (after the ID) written to collection `dev`? */
static bool sent(int dev, const uint8_t *b, size_t n) {
    for (int i = 0; i < g_nw; i++)
        if (g_w[i].dev == dev && !memcmp(g_w[i].b + (b[0] == 0xFF ? 0 : 1), b, n)) return true;
    return false;
}
#define SENT(dev, ...) sent(dev, (const uint8_t[]){ __VA_ARGS__ }, sizeof((const uint8_t[]){ __VA_ARGS__ }))

/* ---- The pretend game --------------------------------------------------------- */

static game_dash_t g_meters;
static void meters(memory_bus_t *bus, game_dash_t *out) { (void)bus; *out = g_meters; }
static game_profile_t g_racer = { .id = "racer", .dash = meters };
static game_profile_t g_fighter = { .id = "fighter" };
static memory_bus_t g_bus;

int main(void) {
    g_active_profile = &g_fighter;
    fanatec_update(&g_bus);
    CHECK(g_inits == 0 && g_open[0] == 0, "a game with no meters opens nothing");

    g_active_profile = &g_racer;
    g_meters = (game_dash_t){ .on = true, .speed = 180, .gear = 3, .rpm = 8000, .rpm_lo = 5000, .rpm_max = 8500 };
    fanatec_update(&g_bus);
    CHECK(g_open[0] == 1 && g_open[1] == 1, "the base's col01 and col03 open; the pedals and col02 do not");
    CHECK(g_w[0].dev == 1 && g_w[0].n == 8 && g_w[0].b[0] == 0x01, "8-byte reports, ID 1 on a DD1");
    CHECK(SENT(1, 0xF8, 0x09, 0x07, 0x01), "stripe translator on");
    CHECK(SENT(1, 0xF8, 0x13, 0xFF, 0x00), "base revs: 8000 rpm lights eight");
    CHECK(SENT(1, 0xF8, 0x09, 0x08, 0x01, 0xFE), "rim revs: the same eight, LED 0 high");
    CHECK(SENT(3, 0xFF, 0x01, 0x00, 0x07, 0xE0), "RGB colours to col03");
    CHECK(SENT(1, 0xF8, 0x09, 0x01, 0x18, 0x02), "the display taken");
    CHECK(SENT(1, 0xF8, 0x09, 0x01, 0x02, 0x00, 0x4F, 0x00), "the display shows ' 3 '");

    g_nw = 0;
    fanatec_update(&g_bus);
    CHECK(g_nw == 0, "the same meters: nothing sent");

    g_meters.rpm = 5000;
    fanatec_update(&g_bus);
    CHECK(g_nw == 3 && SENT(1, 0xF8, 0x13, 0x01, 0x00), "a new rpm: only the three rev reports");

    g_nw = 0;
    g_fanatec_mode = FANATEC_SPEED;
    fanatec_update(&g_bus);
    CHECK(g_nw == 1 && SENT(1, 0xF8, 0x09, 0x01, 0x02, 0x06, 0x7F, 0x3F), "--fanatec speed: '180'");
    g_fanatec_mode = FANATEC_GEAR;

    g_nw = 0;
    g_active_profile = &g_fighter;
    fanatec_update(&g_bus);
    CHECK(SENT(1, 0xF8, 0x13, 0x00, 0x00) && SENT(1, 0xF8, 0x09, 0x08, 0x00, 0x00), "a game with no meters: the LEDs dark");
    CHECK(SENT(1, 0xF8, 0x09, 0x01, 0x02, 0x00, 0x00, 0x00) && SENT(1, 0xF8, 0x09, 0x01, 0x18, 0x01),
          "...the display blank and given back");
    g_nw = 0;
    fanatec_update(&g_bus);
    CHECK(g_nw == 0, "...once");

    g_active_profile = &g_racer;
    g_fail_writes = true;
    fanatec_update(&g_bus);
    CHECK(g_open[0] == 0 && g_open[1] == 0, "a failed write closes the base");
    g_fail_writes = false;
    g_nw = 0;
    fanatec_update(&g_bus);
    CHECK(g_open[0] == 0 && g_nw == 0, "...and it stays closed while no device comes");
    g_changes++;
    fanatec_update(&g_bus);
    CHECK(g_open[0] == 1 && SENT(1, 0xF8, 0x09, 0x01, 0x02, 0x00, 0x4F, 0x00), "a device comes: found again, the gear back");

    g_nw = 0;
    fanatec_shutdown();
    CHECK(SENT(1, 0xF8, 0x09, 0x07, 0x00) && SENT(1, 0xF8, 0x09, 0x01, 0x18, 0x01), "the run's end: translator off, display back");
    CHECK(g_open[0] == 0 && g_open[1] == 0 && g_exits == 1, "...both closed, hidapi done");

    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
