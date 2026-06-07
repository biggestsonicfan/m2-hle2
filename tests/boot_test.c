/*
 * boot_test.c — Phase 6 verification (milestone M1): with the STF boot-critical
 * HLE hooks active, the i960 must pass the COP self-test / timer / idle hangs
 * and reach the per-frame main loop (variable_diff_calc @0x11A04) repeatedly,
 * rather than hanging at the reset vector or halting.
 *
 * Runs the CPU single-threaded (no emu thread needed) against the real ROM set
 * from the claude_mame oracle. Links miniz.
 */
#define NDEBUG 1
#include <stdio.h>

#include "sfight.h"        /* sfight_profile, load/install, hooks, hle_check */
#include "i960_exec.h"

#define ROMDIR "c:/Users/bigge/source/repos/ai/claude_mame/mame/roms/"
#define FRAME_PACE_IP 0x00011A04u

/* hle_check dispatches through g_active_profile — point it at STF. */
const game_profile_t *g_active_profile = &sfight_profile;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    static romset_t     rs;
    static memory_bus_t bus;
    static i960_cpu_t   cpu;

    if (sfight_load(&rs, ROMDIR "sfight.zip", ROMDIR "schamp.zip") != 0) {
        printf("FAIL: ROM load\n"); return 1;
    }
    sfight_install(&rs, &cpu, &bus);
    printf("info: reset IP = 0x%08X\n", cpu.sfr.ip);

    const uint64_t STEP_CAP = 120000000ull;  /* safety cap */
    const int      TARGET_FRAMES = 60;        /* ~1s of game frames */

    uint64_t steps = 0;
    int      frames = 0;
    uint32_t max_ip = 0;
    uint64_t first_frame_step = 0;

    while (steps < STEP_CAP && !cpu.halted && frames < TARGET_FRAMES) {
        if (cpu.sfr.ip == FRAME_PACE_IP) {
            if (frames == 0) first_frame_step = steps;
            frames++;
        }
        if (cpu.sfr.ip > max_ip && cpu.sfr.ip < ROM_SIZE * 2) max_ip = cpu.sfr.ip;
        if (i960_step(&cpu, &bus) != 0) break;
        steps++;
    }

    printf("info: ran %llu steps, frames=%d, halted=%d, final IP=0x%08X, max IP seen=0x%08X\n",
           (unsigned long long)steps, frames, cpu.halted, cpu.sfr.ip, max_ip);
    if (frames > 0)
        printf("info: first main-loop frame after %llu steps\n",
               (unsigned long long)first_frame_step);

    CHECK(!cpu.halted, "CPU did not halt (no COP self-test failure)");
    CHECK(cpu.sfr.ip != 0x000000B0, "advanced past the reset vector");
    CHECK(frames >= 10, "reached the per-frame main loop (0x11A04) >= 10 times");

    romset_free(&rs);
    mem_shutdown(&bus);
    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
