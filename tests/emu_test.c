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

static int s_empty_resets;
static void empty_room_reset(void *c) { (void)c; s_empty_resets++; }

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

    /* run_frames (issue #98): the board stops itself at the edge of the N-th
     * frame, from inside the slice's bookkeeping, not when a stop arrives.
     * Driven through emu_slice_finish directly -- this test has no frame hook. */
    {
        unsigned f0 = g_emu_frames;
        emu_run_frames(&ctx, 3);
        for (int i = 0; i < 2; i++) {
            g_frame_done = 1;
            CHECK(emu_slice_finish(&ctx) == EMU_SLICE_FRAME && ctx.run_state == EMU_RUNNING,
                  "run_frames: frames before the last keep running");
        }
        g_frame_done = 0;
        CHECK(emu_slice_finish(&ctx) == EMU_SLICE_NO_FRAME && ctx.run_state == EMU_RUNNING,
              "run_frames: a slice that ends mid-frame spends nothing");
        g_frame_done = 1;
        CHECK(emu_slice_finish(&ctx) == EMU_SLICE_FRAME, "run_frames: the last frame is still a frame");
        CHECK(ctx.run_state == EMU_STOPPED && ctx.frame_budget == 0 && ctx.frame_budget_hit,
              "run_frames: the board stops at the end of frame N");
        CHECK(g_emu_frames == f0 + 3, "run_frames: exactly N frames counted");
        ctx.frame_budget_hit = 0;

        emu_run_frames(&ctx, 5);
        ctx.request_stop = 1;
        g_frame_done = 0;
        emu_slice_finish(&ctx);
        CHECK(ctx.run_state == EMU_STOPPED && ctx.frame_budget == 0,
              "run_frames: a stop from elsewhere drops the rest of the budget");

        emu_run(&ctx);
        CHECK(ctx.frame_budget == 0, "run_frames: a plain run carries no budget");
        g_frame_done = 1;
        emu_slice_finish(&ctx);
        CHECK(ctx.run_state == EMU_RUNNING && !ctx.frame_budget_hit, "run_frames: ...and never stops itself");
        g_frame_done = 0;
        ctx.run_state = EMU_STOPPED;
    }

    /* The sound board comes off the bus mid-run (sound_detach: the libretro
     * core's heat guard): the UART is a plain region again, the 68000 + SCSP
     * step nothing, and sound_attach puts it all back. The 68000 boots into
     * zeroed RAM here; what it executes is beside the point, the SCSP's
     * sample clock is what sound_run has to stop and restart. */
    {
        static const uint8_t vectors[16] = { 0, 0, 0x10, 0, 0, 0, 0, 0x10 };   /* SSP 0x1000, PC 0x10 */
        sound_load_rom(vectors, sizeof vectors);
        sound_reset();
        sound_attach(&bus);
        uint64_t t0 = g_sound.out_total;
        sound_run(64);
        CHECK(g_sound.out_total == t0 + 64, "sound attached: sound_run(64) makes 64 samples");
        CHECK(mem_read8(&bus, MIDI_BASE + 4) == 0x05, "sound attached: the UART answers transmitter-ready");
        uint64_t w0 = g_sound.write_count;
        mem_write8(&bus, MIDI_BASE, 0x80);
        CHECK(g_sound.write_count == w0 + 1, "sound attached: a UART byte reaches the board");
        sound_detach(&bus);
        CHECK(g_sound.detached, "sound_detach: marked detached");
        t0 = g_sound.out_total;
        sound_run(64);
        CHECK(g_sound.out_total == t0, "detached: sound_run steps nothing");
        mem_write8(&bus, MIDI_BASE, 0x81);
        CHECK(g_sound.write_count == w0 + 1, "detached: a UART byte no longer reaches the board");
        CHECK(bus.midi[0] == 0x81, "detached: it lands in the plain region instead");
        CHECK(mem_read8(&bus, MIDI_BASE + 4) == 0x00, "detached: the UART status is the region's own byte");
        sound_reset();
        sound_attach(&bus);
        CHECK(!g_sound.detached, "attached again: the flag is cleared");
        t0 = g_sound.out_total;
        sound_run(64);
        CHECK(g_sound.out_total == t0 + 64, "attached again: the board runs");

        /* Backpressure (sound_make_midi_room). This 68000 never reads MIDI, so
         * the ring fills: 31 bytes fit, and the 32nd runs the board early -- at
         * most a slice -- and is then dropped and counted. The samples run early
         * belong to the slice and are owed back, so the clock comes out exact. */
        sound_reset();
        t0 = g_sound.out_total;
        for (int i = 0; i < 31; i++) mem_write8(&bus, MIDI_BASE, 0x10);
        CHECK(scsp_midi_room(&g_sound.scsp) == 0 && g_sound.out_total == t0,
              "backpressure: 31 bytes fill the MIDI ring without running the board");
        mem_write8(&bus, MIDI_BASE, 0x11);
        CHECK(g_sound.scsp.mi_drops == 1, "backpressure: a byte the driver never takes is dropped and counted");
        CHECK(g_sound.ahead > 0 && g_sound.ahead <= SOUND_AHEAD_MAX &&
              g_sound.out_total == t0 + (uint64_t)g_sound.ahead,
              "backpressure: the board ran early, by no more than a slice");
        sound_run_slice(60);
        CHECK(g_sound.ahead == 0 && g_sound.out_total == t0 + 735,
              "backpressure: the slice owes the early samples back (735 in all)");
        sound_run_slice(60);
        CHECK(g_sound.out_total == t0 + 1470, "backpressure: the next slice is a whole one");
        sound_reset();
        CHECK(g_sound.ahead == 0 && g_sound.slice_frac == 0,
              "sound_reset clears the slice carry (two cold-booted boards must agree)");
    }

    /* The sound UART's interrupt is offered when the game enables its line
     * (emu_offer_sound), so the enable write kicks -- on the rising edge only. */
    irqt_reset();
    irqt_enable_write(0x021);
    CHECK(!g_irqt_sound_kick, "irq kick: enabling other lines does not kick the sound pin");
    irqt_enable_write(0x421);
    CHECK(g_irqt_sound_kick, "irq kick: enabling the sound line kicks it");
    g_irqt_sound_kick = 0;
    irqt_enable_write(0x421);
    CHECK(!g_irqt_sound_kick, "irq kick: rewriting an enabled line does not");
    irqt_reset();
    CHECK(!g_irqt_sound_kick, "irq kick: irqt_reset clears it");

    /* A room left the board in VS mode and then emptied (netplay_empty_room_pump):
     * the player is asked for a button, a button already down does not count, a
     * fresh press cold boots the board in the player's own VS mode and region. */
    {
        g_netplay.reset_board = empty_room_reset;
        g_netplay.own_saved   = true;
        g_netplay.own_vs_mode = 0;
        g_netplay.own_region  = GAME_REGION_USA;
        g_vs_mode = 1;
        g_region  = GAME_REGION_JAPAN;
        g_netplay.vs_board = true;
        g_input.held = 0x10;
        netplay_empty_room_pump();
        CHECK(g_netplay.empty_prompt, "empty room: a VS board with nobody else in the room asks for a button");
        netplay_empty_room_pump();
        CHECK(!netplay_take_empty_restart(), "empty room: a button held from before does not restart");
        g_input.held = 0;
        netplay_empty_room_pump();
        g_input.held = 0x10;
        netplay_empty_room_pump();
        CHECK(!g_netplay.empty_prompt && netplay_take_empty_restart(), "empty room: pressing it again does");
        CHECK(!netplay_take_empty_restart(), "empty room: the press is taken once");
        netplay_restart_alone();
        CHECK(s_empty_resets == 1, "empty room: the restart resets the board");
        CHECK(g_vs_mode == 0 && g_region == GAME_REGION_USA, "empty room: ...in the player's own VS mode and region");
        g_input.held = 0;
        netplay_empty_room_pump();
        CHECK(!g_netplay.empty_prompt, "empty room: a board that is not in VS mode is not asked");
    }

    mem_shutdown(&bus);
    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
