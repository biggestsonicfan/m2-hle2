/*
 * emu_test.c — standalone Phase 5 verification for the emu thread.
 *
 * Loads a 2-instruction infinite loop (g0++ ; branch back) into work RAM, runs
 * it on the real background emu thread, and confirms: the thread executes
 * (total_steps climbs), the program makes progress (g0 increments), the
 * published snapshot tracks it, pause works, and single-step advances a
 * precise number of instructions.
 *
 * NOTE: the test never grabs the emu mutex while RUNNING. With no frame-pacing
 * hook yet (Phase 6), a 500k-step slice exceeds the 16.67ms budget and the emu
 * thread holds the mutex ~100% of the time, so a competing lock would starve
 * for seconds. We read the volatile stats / last-published snapshot instead,
 * stopping before reading register state. (In the real app the Phase-6 frame
 * hook ends slices early, freeing the mutex each tick for the UI.)
 */
#define NDEBUG 1
#include <stdio.h>

#include "emu_thread.h"   /* pulls i960_exec, memory, breakpoint, hle dispatch */

/* hle_check references g_active_profile (defined by the app in registry.h).
 * No profiles linked here → NULL means hle_check short-circuits. */
const game_profile_t *g_active_profile = NULL;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

static uint32_t enc_reg(uint32_t op, int dst, int src2, int m2, int src1, int m1, int m3) {
    uint32_t hi = (op >> 4) & 0xFF, lo = op & 0xF;
    return (hi << 24) | ((dst & 0x1F) << 19) | ((src2 & 0x1F) << 14)
         | ((m3 & 1) << 13) | ((m2 & 1) << 12) | ((m1 & 1) << 11)
         | ((lo & 0xF) << 7) | (src1 & 0x1F);
}
static uint32_t enc_ctrl(uint32_t op, int32_t disp) {
    return (op << 24) | ((uint32_t)disp & 0x00FFFFFC);
}
#define G(n) (16 + (n))

int main(void) {
    static memory_bus_t bus;
    static i960_cpu_t   cpu;
    static emu_thread_ctx_t ctx;

    mem_init(&bus, NULL, 0);
    i960_reset(&cpu);

    const uint32_t CODE = RAM_BASE + 0x2000;
    mem_write32(&bus, CODE,     enc_reg(0x590, G(0), G(0), 0, 1, 1, 0)); /* addo 1,g0,g0 */
    mem_write32(&bus, CODE + 4, enc_ctrl(0x08, -4));                     /* b CODE       */
    cpu.sfr.ip = CODE;

    emu_thread_init(&ctx, &cpu, &bus);
    CHECK(ctx.thread_alive, "emu thread started");
    CHECK(ctx.run_state == EMU_STOPPED, "thread starts STOPPED");

    /* Free-run ~120ms, then pause and inspect the published snapshot. */
    emu_run(&ctx);
    emu_sleep_ms(120);
    emu_stop(&ctx);
    emu_sleep_ms(60);   /* let the current slice finish + publish snapshot */

    CHECK(ctx.run_state == EMU_STOPPED, "pause returns to STOPPED");
    CHECK(ctx.total_steps > 100000, "free-run accumulates steps");
    CHECK(ctx.cpu_snapshot.globals.g[0] > 1000, "program made progress (g0 incremented)");
    CHECK(ctx.cpu_snapshot.sfr.ip == CODE || ctx.cpu_snapshot.sfr.ip == CODE + 4,
          "IP stays within the loop body");

    /* While paused, no further steps execute. */
    uint64_t steps_a = ctx.total_steps;
    emu_sleep_ms(60);
    uint64_t steps_b = ctx.total_steps;
    CHECK(steps_a == steps_b, "no steps execute while paused");

    /* Single-step exactly 5 instructions. */
    uint64_t before = ctx.total_steps;
    emu_step(&ctx, 5);
    CHECK(ctx.total_steps == before + 5, "emu_step(5) advances exactly 5 instructions");
    CHECK(ctx.run_state == EMU_STOPPED, "returns to STOPPED after stepping");

    /* steps/second sampling: run >1s so the sampler updates at least once. */
    emu_run(&ctx);
    emu_sleep_ms(1100);
    emu_stop(&ctx);
    emu_sleep_ms(60);
    CHECK(ctx.steps_per_second > 0, "steps_per_second sampler reports throughput");
    printf("info: steps/s = %u, total = %llu\n",
           ctx.steps_per_second, (unsigned long long)ctx.total_steps);

    emu_thread_shutdown(&ctx);
    CHECK(!ctx.thread_alive, "emu thread shut down cleanly");

    mem_shutdown(&bus);
    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
