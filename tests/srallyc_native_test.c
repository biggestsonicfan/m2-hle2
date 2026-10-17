/*
 * srallyc_native_test.c -- Sega Rally's own replay format
 * (profiles/srallyc_native.h, Pinboard #997) on a bare memory bus. No ROM.
 *
 * A race end takes the course's clip and count; put writes them back; play
 * groups the rounds into championships, and the mode dispatch sends attract
 * to the ending with each championship's clips in, then back to the boot.
 *
 * A ctest (CMakeLists.txt, srallyc_native_test).
 */
#define NDEBUG 1
#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "memory.h"
#include "i960.h"
#include "emu_thread.h"
#include "registry.h"

static memory_bus_t bus;
static i960_cpu_t   cpu;
static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); g_fail++; } \
                              else { printf("ok:   " __VA_ARGS__); printf("\n"); } } while (0)

/* Three races' rounds, kept with their own copies of the parts. */
static game_native_round_t g_round[3];
static uint8_t g_clip[3][SRN_CLIP_LEN], g_count[3][4];

static void race(int i, uint32_t course, uint32_t frames, uint8_t fill) {
    mem_write32(&bus, SRN_COURSE, course);
    mem_write32(&bus, SRN_COUNT + 4u * course, frames);
    mem_write32(&bus, SRN_PASSED, 1);
    for (uint32_t k = 0; k < SRN_CLIP_LEN; k++) mem_write8(&bus, srn_clip_addr(course) + k, (uint8_t)(fill + k));
    CHECK(srallyc_hook_native_race_end(&cpu, &bus) == 1, "race %d: the race end runs on", i);
    game_native_round_t *r = &g_round[i];
    CHECK(srallyc_native_take(r) && !srallyc_native_take(r), "race %d: taken once", i);
    memcpy(g_clip[i], r->part[0].data, SRN_CLIP_LEN);
    memcpy(g_count[i], r->part[1].data, 4);
    r->part[0].data = g_clip[i];
    r->part[1].data = g_count[i];
    CHECK(r->part[0].addr == srn_clip_addr(course) && g_clip[i][1] == (uint8_t)(fill + 1),
          "race %d: the clip from course %u's buffer", i, course);
    CHECK(strstr(r->json, "\"keyed\":1") && strstr(r->json, "\"key_frames\":680"), "race %d: keyed, 679 frames", i);
}

static void check_put(void) {
    for (uint32_t k = 0; k < SRN_CLIP_LEN; k++) mem_write8(&bus, srn_clip_addr(1) + k, 0);
    mem_write32(&bus, SRN_COUNT + 4u, 0);
    CHECK(srallyc_native_put(&bus, &g_round[1]) == NULL, "put: course 1's round goes in");
    CHECK(mem_read32(&bus, SRN_COUNT + 4u) == 500 && mem_read8(&bus, srn_clip_addr(1) + 7) == (uint8_t)(0x40 + 7),
          "put: the count and the clip back");
}

/* Frames of the ending until the dispatch sends the board back to its boot. */
static void run_ending(const char *what, uint32_t course_first) {
    mem_write32(&bus, SRN_MODE, 2);
    srallyc_hook_native_dispatch(&cpu, &bus);
    CHECK(mem_read32(&bus, SRN_MODE) == 3 && mem_read32(&bus, SRN_SUB) == 5 && mem_read32(&bus, SRN_STATE) == 5 &&
          mem_read32(&bus, SRN_COURSE) == 3, "%s: attract sent to the lakeside's race end", what);
    mem_write32(&bus, SRN_STATE, 6);
    mem_write32(&bus, SRN_END_STATE, 3);
    for (uint32_t c = course_first; c < SRN_COURSES; c++) {
        mem_write32(&bus, SRN_COURSE, c);
        for (uint32_t f = 0; f < SRN_REEL_FRAMES + 10; f++) srallyc_hook_native_dispatch(&cpu, &bus);
    }
    mem_write32(&bus, SRN_STATE, 7);
    srallyc_hook_native_dispatch(&cpu, &bus);
    CHECK(mem_read32(&bus, SRN_MODE) == 0, "%s: the name entry goes back to the boot", what);
}

static void check_play(void) {
    CHECK(srallyc_native_play(g_round, 3) == NULL && g_native_play.total_frames == 3 * SRN_REEL_FRAMES,
          "play: three rounds, 679 frames each");
    run_ending("championship 1", 0);
    CHECK(mem_read32(&bus, SRN_COUNT + 8u) == 0 && !g_native_play.ended, "championship 1: course 2 empty, more to play");
    CHECK(g_native_play.total == 2 * SRN_REEL_FRAMES, "championship 1: both courses' credits counted");
    run_ending("championship 2", 0);
    CHECK(mem_read32(&bus, SRN_COUNT + 4u) == 0 && mem_read8(&bus, srn_clip_addr(0) + 3) == (uint8_t)(0x80 + 3),
          "championship 2: its forest clip in, course 1 empty");
    CHECK(g_native_play.ended && !g_native_play.on && g_native_play.total == g_native_play.total_frames,
          "play: ended with every frame counted");
    CHECK(srallyc_native_play(NULL, 0) == NULL && !g_native_play.on, "play: n = 0 stops");
}

int main(void) {
    if (!mem_init(&bus, NULL, 0)) { printf("mem_init failed\n"); return 1; }
    i960_reset(&cpu);
    race(0, 0, 750, 0x10);
    race(1, 1, 500, 0x40);
    race(2, 0, 250, 0x80);   /* a new game: the forest again */
    check_put();
    check_play();
    printf(g_fail ? "\nFAILED (%d)\n" : "\nPASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
