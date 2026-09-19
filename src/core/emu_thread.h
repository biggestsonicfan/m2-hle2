/*
 * emu_thread.h — background CPU run loop with double-buffered snapshots.
 *
 * Threading model:
 *   - Main thread: ImGui frame at 60Hz; reads ctx->cpu_snapshot under mutex.
 *   - Emu thread:  drives ctx->cpu via i960_step in EMU_STEPS_PER_SLICE
 *                  batches; publishes the result into ctx->cpu_snapshot
 *                  under the same mutex before sleeping.
 *
 * IMPORTANT invariants from CLAUDE.md:
 *   - The mutex MUST be released before sleeping. Sleeping while holding
 *     the mutex freezes the UI thread.
 *   - cpu_snapshot is double-buffered (snapshot + prev_snapshot) so the UI
 *     can compute changed-since-last-frame diffs without tearing.
 *   - g_frame_done is the frame-boundary trip flag set by the per-game HLE
 *     pacing hook. Until that hook lands (Phase 6) it stays 0 and we fall back
 *     to fixed-slice timing so the CPU doesn't busy-spin.
 *
 * PHASE 5 NOTE: the 68K sound stepping (Phase 10) and the real i960 interrupt
 * delivery (Phase 11, emu_service_irq + irq_timer.h) are NOT wired here yet.
 * They slot back into the RUNNING branch when those phases land.
 */
#ifndef EMU_THREAD_H
#define EMU_THREAD_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "log.h"
#include "i960.h"
#include "i960_exec.h"
#include "memory.h"
#include "breakpoint.h"
#include "hle_hooks.h"   /* g_frame_done, hle_call, g_active_profile */
#include "irq_timer.h"   /* board IRQ controller + timers */
#include "../board/sound.h"  /* sound_run_slice: the 68000 + SCSP */
#include "../net/netplay.h"  /* the lockstep frame gate (inert unless a session is up) */

/* 68K at ~11.3 MHz vs i960 at 25 MHz — run 45% as many steps per slice. */

/* The mutex/thread types and the millisecond sleep live in thread_mutex.h, so
 * net/netplay.h can use the same mutex without including this header (the run
 * loop below calls into netplay, which would otherwise be a cycle). */
#include "thread_mutex.h"

/* ---- Tuning -------------------------------------------------------------- */

#define EMU_CPU_HZ           25000000               /* i960 KB on Model 2 = 25 MHz */
#define EMU_SLICES_PER_SEC   60
#define EMU_SLICE_US         (1000000 / EMU_SLICES_PER_SEC)  /* ~16667 µs */
/* EMU_STEPS_PER_SLICE is in constants.h (500,000), sized to always reach
 * the per-game frame-boundary hook within one batch. */

/* The i960 steps one slice may run before it ends without a frame edge.
 * Ordinary frames need well under 100k (STF attract p99 ~53k); only loads
 * reach the default — texture decompression runs ~1M steps a game frame. A
 * host too slow for 500k steps in 16.7 ms sets it lower, so a load spreads
 * over more slices that each fit (the sound board and the renderer keep time)
 * instead of fewer that do not. Where interrupts land during a load moves
 * with it, so tools that compare against MAME keep the default. */
static volatile int g_emu_steps_per_slice = EMU_STEPS_PER_SLICE;

/* ---- Run state ----------------------------------------------------------- */

typedef enum {
    EMU_STOPPED,    /* paused, single-stepping allowed */
    EMU_RUNNING,    /* free-running under the emu thread */
    EMU_STEPPING,   /* execute N then back to STOPPED */
} emu_run_state_t;

typedef struct {
    /* Shared state */
    i960_cpu_t   *cpu;
    memory_bus_t *bus;
    emu_mutex_t   mutex;

    /* Thread control (UI writes, emu thread reads) */
    volatile emu_run_state_t run_state;
    volatile int             step_count;
    volatile int             request_stop;
    volatile int             thread_alive;

    /* Stats (emu thread writes, UI reads) */
    volatile uint64_t        total_steps;
    volatile uint32_t        steps_per_second;

    /* UI-facing snapshots (taken under mutex) */
    i960_cpu_t   cpu_snapshot;
    i960_cpu_t   cpu_prev_snapshot;

    /* Frame-pace deadline tracking */
    int64_t      frame_deadline_us;

    /* Set before transitioning from STOPPED→RUNNING so the run loop executes
     * the current instruction once before re-checking breakpoints. Without
     * this, resuming after a BP hit immediately re-fires the same BP because
     * the IP hasn't advanced. */
    volatile int step_over_bp;

    /* A board reset asked for from outside a netplay session (the MCP bridge's
     * `board_reset`). Serviced where the barrier's reset is, by the same code;
     * reset_count is how the asker learns it happened. */
    volatile int      request_reset;
    volatile uint32_t reset_count;

    emu_thread_t thread;
} emu_thread_ctx_t;

/* ---- High-res clock + precise sleep ------------------------------------- */

#ifdef _WIN32
static inline int64_t emu_now_us(void) {
    static LARGE_INTEGER freq = {0};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (int64_t)(now.QuadPart * 1000000 / freq.QuadPart);
}
static inline void emu_sleep_us(int64_t us) {
    if (us > 1000) Sleep((DWORD)(us / 1000));
}
#else
static inline int64_t emu_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static inline void emu_sleep_us(int64_t us) {
    if (us > 0) usleep((useconds_t)us);
}
#endif

/* ---- Real i960 interrupt delivery --------------------------------------
 * Asserts the sound UART IRQ (intreq bit 10) while the i960 sound queue has
 * data, then vectors the highest-priority pending+enabled IRQ to its handler
 * via hle_call (Model 2 interrupt controller — see irq_timer.h). One injection
 * per slice; s_irq_in_service gates re-entry until the handler's ret unwinds
 * the injected frame. Also poke the per-game warning-skip flag. */
static bool s_irq_in_service     = false;
static int  s_irq_baseline_depth = 0;

/* Warning-screen auto-skip. ON normally; turn OFF to keep our attract timeline
 * frame-aligned with MAME (which shows the warning for its full duration) when
 * comparing camera data by the game frame counter. */
static volatile int g_warning_skip = 1;

/* match_replay: 0 off, 1 armed, 2 done (the jump was made), -1 the profile has
 * no attract replay. See game_quirks_t.attract_replay. */
static volatile int      g_match_replay = 0;
static volatile uint32_t g_match_replay_frame = 0;

/* At a frame edge: if armed and attract mode is at the profile's movie step
 * with the movie set up, write the movie state a natural boot has when the
 * replay starts and move on to the replay step. */
static inline void emu_match_replay_edge(emu_thread_ctx_t *ctx) {
    if (g_match_replay != 1 || !g_active_profile) return;
    const attract_replay_t *ar = &g_active_profile->quirks.attract_replay;
    if (!ar->step_addr) { g_match_replay = -1; return; }
    if (mem_read8(ctx->bus, ar->step_addr) != ar->from_step) return;
    if (ar->ready_addr && mem_read32(ctx->bus, ar->ready_addr) == 0) return;
    for (int i = 0; i < ar->state_count; i++)
        mem_write32(ctx->bus, ar->state_addr + 4u * (uint32_t)i, ar->state[i]);
    mem_write8(ctx->bus, ar->step_addr, ar->to_step);
    g_match_replay = 2;
    g_match_replay_frame = g_emu_frames;
    LOG_INFO("match_replay: attract step %u -> %u at frame %u", ar->from_step, ar->to_step, g_emu_frames);
}

/* ---- The frame clock ------------------------------------------------------
 *
 * The board's audio sample count at each game-frame boundary. Written here, on
 * the emu thread; read by whatever renders the frame, so a picture can be
 * stamped on the same timebase as the samples that go with it — which is what
 * lets an A/V consumer put the two back together without assuming 735 samples
 * a frame or an exact 60 Hz (src/core/av_stream.h).
 *
 * `sample` is published BEFORE `frame`, and a reader re-reads `frame` after
 * taking both: the pair is not written atomically, and the frame number is
 * what says the sample beside it is the matching one.
 */
static struct {
    volatile uint64_t sample;   /* g_sound.out_total when `frame` ended */
    volatile uint64_t frame;    /* the g_emu_frames value get_status reports */
} g_frame_clock;

/* Clear the run loop's own per-boot latches. Part of a board reset, and separate
 * from install_fn because these live here: a handler left "in service" across a
 * reset would swallow the first interrupt of the new boot, which on two
 * netplayed machines is a divergence on frame 1. */
static inline void emu_board_reset_state(void) {
    s_irq_in_service     = false;
    s_irq_baseline_depth = 0;
    g_frame_done         = 0;
    g_vblank_acked       = 0;
    g_emu_frames         = 0;
    /* The frame number restarts with the board; the sample clock does not —
     * g_sound.out_total survives a reset, and an A/V client mid-stream would
     * hear the seam as a jump backwards in time. */
    g_frame_clock.frame  = 0;
}

static inline void emu_service_irq(emu_thread_ctx_t *ctx) {
    if (!g_active_profile) return;
    i960_cpu_t          *cpu = ctx->cpu;
    const game_quirks_t *q   = &g_active_profile->quirks;

    /* Auto-skip the boot warning screen by holding its ack flag at 1. */
    if (g_warning_skip && q->warning_skip_addr) mem_write32(ctx->bus, q->warning_skip_addr, 1);

    /* Did the in-service handler return? (frame unwound to/below baseline) */
    if (s_irq_in_service && cpu->frame_depth <= s_irq_baseline_depth)
        s_irq_in_service = false;

    /* Sound UART TxRDY: keep bit 10 asserted while the i960 has bytes to send. */
    if (q->sound_queue_count_addr) {
        uint32_t cnt   = mem_read8(ctx->bus, q->sound_queue_count_addr);
        uint32_t state = q->sound_queue_state_addr
                       ? mem_read8(ctx->bus, q->sound_queue_state_addr) : 0xFFu;
        if (cnt > 0 || state != 0xFFu) irqt_raise(0x400u);   /* bit 10 = sound */
    }

    if (s_irq_in_service) return;
    int pin = irqt_pending_pin();            /* gated by (intreq & intena) */
    if (pin < 0) return;
    uint32_t h = q->irq_handler[pin];
    if (!h) return;                          /* pin not yet delivered (still HLE) */

    s_irq_baseline_depth = cpu->frame_depth;
    hle_interrupt(cpu, h);                   /* vector to handler; ret resumes, AC/PC restored */
    s_irq_in_service = true;
    g_irqt.deliver_count++;
    g_irqt.deliver_by_pin[pin & 3]++;
}

/* The sound UART is ready for its next byte the moment the last one is out
 * (MAME: all three bytes of a command land in the SCSP together), so when the
 * sound handler returns and the i960 still has bytes queued, run it again in
 * this slice instead of the next — one byte per frame put every sound command
 * ~50 ms late. Only the sound pin: the frame-paced pins keep their cadence. */
static inline void emu_service_sound_again(emu_thread_ctx_t *ctx) {
    if (!s_irq_in_service || ctx->cpu->frame_depth > s_irq_baseline_depth) return;
    s_irq_in_service = false;
    const game_quirks_t *q = &g_active_profile->quirks;
    if (!q->sound_queue_count_addr || !q->irq_handler[3]) return;
    /* The 68000 does not run until sound_run_slice, later in this slice, so
     * every re-service here piles onto an undrained 32-byte MIDI ring. Stop
     * while a whole command still fits and let the rest go next slice: the
     * board's UART paces the bytes the same way, by reporting not-ready. */
    if (scsp_midi_room(&g_sound.scsp) < 4) { s_irq_in_service = true; return; }
    uint32_t cnt   = mem_read8(ctx->bus, q->sound_queue_count_addr);
    uint32_t state = q->sound_queue_state_addr ? mem_read8(ctx->bus, q->sound_queue_state_addr) : 0xFFu;
    if (cnt == 0 && state == 0xFFu) return;
    irqt_raise(0x400u);
    if (irqt_pending_pin() != 3) return;
    s_irq_baseline_depth = ctx->cpu->frame_depth;
    hle_interrupt(ctx->cpu, q->irq_handler[3]);
    s_irq_in_service = true;
    g_irqt.deliver_count++;
    g_irqt.deliver_by_pin[3]++;
}

/* ---- Board timers against the i960's clock (irq_timer.h g_irqt_live) ------ */

static uint64_t s_timer_cycles_seen = 0;   /* cpu->cycles the timers have been given */
static uint64_t s_slice_cycles0     = 0;   /* cpu->cycles when this game frame began */

/* Slice start. Frozen timers: a whole slice of cycles. Live: only the cycles run
 * since the timers were last brought up to date — the frame's idle share is
 * charged at the frame edge that ends it (emu_timers_frame_edge), before the
 * game re-arms its frame timer in the vsync handler. Charging it here instead
 * put the wait ahead of the new frame's budget, and STF then skipped half its
 * sway chains at character select, where the board runs them all. */
static inline void emu_timers_slice_begin(emu_thread_ctx_t *ctx) {
    if (!g_real_irq) return;
    if (!g_irqt_live) { irqt_tick(EMU_CPU_HZ / EMU_SLICES_PER_SEC); return; }
    i960_cycle_table_init();
    i960_cpu_t *cpu = ctx->cpu;
    g_irqt.pending += (int64_t)(cpu->cycles - s_timer_cycles_seen);
    s_timer_cycles_seen = cpu->cycles;
    irqt_flush();
}

/* The game's frame has ended: the board would spin here until vsync, so give the
 * timers the rest of this 1/60 s before the frame that follows starts to count. */
static inline void emu_timers_frame_edge(emu_thread_ctx_t *ctx) {
    if (!g_real_irq || !g_irqt_live) return;
    i960_cpu_t *cpu = ctx->cpu;
    g_irqt.pending += (int64_t)(cpu->cycles - s_timer_cycles_seen);
    s_timer_cycles_seen = cpu->cycles;
    irqt_flush();
    int64_t idle = EMU_CPU_HZ / EMU_SLICES_PER_SEC - (int64_t)(cpu->cycles - s_slice_cycles0);
    if (idle > 0) irqt_tick(idle);
    s_slice_cycles0 = cpu->cycles;
}

/* After each instruction, live timers only: count its cycles, and take a timer
 * interrupt as soon as one is pending and none is in service. */
static inline void emu_timers_after_step(emu_thread_ctx_t *ctx) {
    i960_cpu_t *cpu = ctx->cpu;
    g_irqt.pending += (int64_t)(cpu->cycles - s_timer_cycles_seen);
    s_timer_cycles_seen = cpu->cycles;
    if (g_irqt.pending >= g_irqt.horizon) irqt_flush();
    if (!s_irq_in_service && (g_irqt.intreq & g_irqt.intena & 0x03FCu) &&
            g_active_profile && g_active_profile->quirks.irq_handler[2])
        emu_service_irq(ctx);
}

/* ---- Run loop ------------------------------------------------------------ */

/* Pump netplay and hand back what this slice may do.
 *
 * OUTSIDE the emu mutex on purpose: a TLS connect blocks for seconds, and
 * holding the mutex across it freezes the UI. A RESET is performed here, under
 * the mutex, because it rewrites the whole board.
 *
 * Called from the STOPPED branch as well as the RUNNING one — a player connects
 * and takes a room before pressing Run, and a netplay session that is only
 * pumped while the board is running would sit there never logging in. */
static inline netplay_step_t emu_netplay_pump(emu_thread_ctx_t *ctx) {
    netplay_step_t step = netplay_begin_frame();

    /* A reset on request is the barrier's reset without the barrier. Only while
     * no session owns the board: inside one it would be a reset on this machine
     * and not the other, which is a desync by definition. A request that arrives
     * then is dropped, and the asker times out. */
    bool asked = ctx->request_reset != 0;
    if (asked) ctx->request_reset = 0;
    if (asked && step != NETPLAY_STEP_OFF) asked = false;

    if (step == NETPLAY_STEP_RESET || asked) {
        emu_mutex_lock(&ctx->mutex);
        if (asked) asked = netplay_reset_board_now();
        else       netplay_do_reset();
        /* THE STEP COUNT IS PART OF THE BOARD, because the frame check hashes
         * it -- `netplay_frame_check` calls it "the instruction count since
         * reset" and it has to actually be one. Left running, it carries the
         * whole life of the process into the first session: an emulator that
         * has been playing the CPU for ten minutes and one that just launched
         * cold-boot to identical architectural state and still hash
         * differently, so the peers report DESYNC at frame 0 every time,
         * however correct the reset was. */
        ctx->total_steps       = 0;
        ctx->cpu_prev_snapshot = ctx->cpu_snapshot;
        ctx->cpu_snapshot      = *ctx->cpu;
        emu_mutex_unlock(&ctx->mutex);
        if (asked) ctx->reset_count++;
    }
    return step;
}

/* ---- One slice ---------------------------------------------------------------
 *
 * The RUNNING branch of the run loop, in two halves, so every host runs the
 * same slice: the native emu thread below, and a host with no second thread (the
 * web build steps the board from its frame callback). Neither half sleeps, and
 * neither touches the mutex or netplay's frame gate -- those stay with the
 * caller, which is what differs between hosts.
 *
 *   emu_slice_body    one slice of the board: interrupts, the i960 to the frame
 *                     hook, the sound board, the snapshots. Call with the emu
 *                     mutex held where there is one.
 *   emu_slice_finish  what the slice ended on: a stop (breakpoint, watchpoint,
 *                     halt, ...), a game frame boundary -- counted, and handed to
 *                     netplay -- or neither. The caller paces on the answer. */
typedef enum {
    EMU_SLICE_STOPPED,   /* the run state is STOPPED now */
    EMU_SLICE_FRAME,     /* a game frame ended: pace to the next 60 Hz tick */
    EMU_SLICE_NO_FRAME,  /* the slice ran out without reaching the frame hook */
} emu_slice_result_t;

static inline void emu_slice_body(emu_thread_ctx_t *ctx) {
    g_frame_done = 0;
    /* Board-level vblank (opt-in per profile): raise the vsync pending
     * bit once per 60 Hz slice, like the real board / MAME at scanline
     * 384. A self-pacing homebrew that polls + ACKs intreq bit0 then
     * advances one frame per slice (fallback timing paces the slice) —
     * no HLE hook or hardcoded address needed. Inert unless polled. */
    bool board_vblank = g_active_profile && g_active_profile->quirks.board_vblank;
    if (board_vblank) {
        irqt_raise(0x1u);
        g_vblank_acked = 0;     /* the homebrew's vsync-ACK ends this slice */
        /* Mark the geo capture frame boundary, exactly as sfight/fvipers
         * do in their HLE frame hook. Without it geo3d falls back to
         * scanning the WHOLE capture ring (the 24K-word COP boot firmware
         * + every accumulated frame) instead of just this frame's draws,
         * so a homebrew object draw never isolates / renders. */
        g_cop.geo_frame_start = g_cop.geo_frame_end;
        g_cop.geo_frame_end   = g_cop.geo_capture_head;
    }
    /* Additive: advance the board timers one frame of cycles so the
     * enabled timer IRQ (bit5) expires and vectors its ISR. */
    emu_timers_slice_begin(ctx);
    emu_service_irq(ctx);   /* deliver pending i960 interrupts (sound, …) */
    int max_steps = g_emu_steps_per_slice;
    for (int i = 0;
         i < max_steps
         && !g_frame_done
         && !(board_vblank && g_vblank_acked)   /* stop at the frame's vsync-ACK */
         && !ctx->request_stop
         && !ctx->cpu->halted;
         i++)
    {
        if (ctx->step_over_bp) {
            ctx->step_over_bp = 0;
        } else if (bp_check(ctx->cpu->sfr.ip)) {
            break;
        }
        if (i960_step_hot(ctx->cpu, ctx->bus) != 0) break;
        ctx->total_steps++;
        if (s_irq_in_service && g_active_profile) emu_service_sound_again(ctx);
        if (g_irqt_live) emu_timers_after_step(ctx);
        if (g_log.warn_triggered) break;
        if (g_wp.hit) break;   /* data watchpoint tripped mid-instruction */
        if (g_sharc.unknown_triggered) break;  /* break-on-unknown COP cmd */
    }
    /* The game's frame ended on the instruction the loop stopped at, so
     * this is between two frames' display lists: mark it for a capture. */
    if (g_frame_done || (board_vblank && g_vblank_acked)) {
        emu_timers_frame_edge(ctx);
        dl_frame_edge(ctx->bus, g_emu_frames);
        emu_match_replay_edge(ctx);
    }

    /* The sound board runs on its own sample clock: a slice's worth of
     * 44.1 kHz samples, the 68000 in lockstep with the SCSP. */
    sound_run_slice(EMU_SLICES_PER_SEC);

    /* Snapshot the GEO display list while the i960 is idle (mutex held) — the
     * homebrew is vblank-waiting just past geo_flush, so bufferram holds the
     * intact list before the next frame's SHARC math clobbers it. */
    if (g_active_profile && g_active_profile->quirks.geo_displaylist)
        geodl_capture(ctx->bus);

    ctx->cpu_prev_snapshot = ctx->cpu_snapshot;
    ctx->cpu_snapshot      = *ctx->cpu;
}

static inline emu_slice_result_t emu_slice_finish(emu_thread_ctx_t *ctx) {
    bool board_vblank = g_active_profile && g_active_profile->quirks.board_vblank;
    if (g_bp.hit || g_wp.hit || g_log.warn_triggered || g_sharc.unknown_triggered || ctx->request_stop || ctx->cpu->halted) {
        if (g_bp.hit) {
            LOG_INFO("emu: breakpoint hit @ 0x%08X", g_bp.hit_addr);
            g_bp.hit = 0;
        }
        if (g_wp.hit) {
            LOG_INFO("emu: watchpoint %s @ 0x%08X = 0x%08X (IP=0x%08X)",
                     g_wp.hit_write ? "write" : "read",
                     g_wp.hit_addr, g_wp.hit_val, g_wp.hit_ip);
            /* leave g_wp.hit set so a poller can report it; cleared on next run */
        }
        if (g_log.warn_triggered) {
            g_log.warn_triggered = 0;
            LOG_INFO("emu: break-on-warn @ IP=0x%08X", ctx->cpu->sfr.ip);
        }
        if (g_sharc.unknown_triggered) {
            LOG_INFO("emu: unknown COP cmd 0x%08X @ IP=0x%08X",
                     g_sharc.unknown_trigger_cmd, g_sharc.unknown_trigger_ip);
            g_sharc.unknown_triggered = 0;
        }
        ctx->request_stop = 0;
        ctx->run_state = EMU_STOPPED;
        if (ctx->cpu->halted) {
            LOG_WARN("emu: CPU halted @ IP=0x%08X (steps=%llu)",
                     ctx->cpu->sfr.ip, (unsigned long long)ctx->total_steps);
        }
        return EMU_SLICE_STOPPED;
    }
    if (g_frame_done || (board_vblank && g_vblank_acked)) {
        g_emu_frames++;   /* frame clock for the MCP bridge / capture tools */
        /* Stamp the frame with the board audio produced up to its end: the
         * slice's own samples are already in, sound_run_slice ran in the body
         * above. sample first, frame second (see g_frame_clock). */
        g_frame_clock.sample = g_sound.out_total;
        g_frame_clock.frame  = g_emu_frames;
        /* The netplay frame clock and this frame's state check. Fed the
         * snapshot rather than the live CPU: it was taken under the mutex
         * a few lines up and is the same state, without racing the UI. */
        netplay_end_frame(&ctx->cpu_snapshot, ctx->total_steps);
        if (g_sndcap.active) sndcap_frame(g_emu_frames, mem_read32(ctx->bus, 0x500020));
        return EMU_SLICE_FRAME;
    }
    return EMU_SLICE_NO_FRAME;
}

static void emu_thread_run_loop(emu_thread_ctx_t *ctx) {
    uint64_t sps_steps_start = ctx->total_steps;
    int64_t  last_sps_time   = emu_now_us();

    while (ctx->thread_alive) {
        emu_run_state_t s = ctx->run_state;

        if (s == EMU_RUNNING) {
            int64_t slice_start = emu_now_us();

            /* Netplay decides what this slice may do. STEP_OFF — one predictable
             * branch — whenever no session is running. */
            netplay_step_t np_step = emu_netplay_pump(ctx);
            if (np_step == NETPLAY_STEP_WAIT) {
                /* Stalled waiting for the peer's input, or waiting at the barrier:
                 * the board must not advance. Sleep a tick so the UI and the
                 * network both get the host CPU, and come back to re-poll. */
                emu_sleep_ms(1);
                continue;
            }
            if (np_step == NETPLAY_STEP_RESET) continue;   /* done above; re-enter */

            emu_mutex_lock(&ctx->mutex);
            emu_slice_body(ctx);
            emu_mutex_unlock(&ctx->mutex);

            emu_slice_result_t slice = emu_slice_finish(ctx);
            if (slice == EMU_SLICE_STOPPED) {
                /* nothing to pace: the run state is STOPPED now */
            } else if (slice == EMU_SLICE_FRAME) {
                /* Frame boundary (HLE hook, or the homebrew's vsync-ACK) — pace to
                 * the next 16.67ms tick. For board_vblank this also ends the i960's
                 * vsync busy-spin, throttling it to 60 Hz and freeing the host CPU. */
                int64_t now = emu_now_us();
                if (ctx->frame_deadline_us == 0) {
                    ctx->frame_deadline_us = now + EMU_SLICE_US;
                } else {
                    ctx->frame_deadline_us += EMU_SLICE_US;
                    /* Catch-up clamp: if we've fallen >1 frame behind (paused
                     * in debugger, heavy host load), reset rather than spin. */
                    if (ctx->frame_deadline_us < now - (int64_t)EMU_SLICE_US) {
                        ctx->frame_deadline_us = now + EMU_SLICE_US;
                    }
                }
                /* M2HLE_UNTHROTTLE=1: no 60 Hz pacing, for automated runs that
                 * have to reach rare states (the UI still gets the mutex, since
                 * every frame boundary unlocks it). */
                static int unthrottled = -1;
                if (unthrottled < 0) { const char *e = getenv("M2HLE_UNTHROTTLE"); unthrottled = e && e[0] == '1'; }
                int64_t sleep_us = ctx->frame_deadline_us - emu_now_us();
                if (unthrottled) ctx->frame_deadline_us = 0;
                else if (sleep_us > 0) emu_sleep_us(sleep_us);
            } else {
                /* No game-pace hook yet — fall back to fixed slice timing
                 * so the host CPU doesn't pin at 100%. */
                int64_t elapsed   = emu_now_us() - slice_start;
                int64_t remaining = EMU_SLICE_US - elapsed;
                if (remaining > 0) emu_sleep_us(remaining);
                /* A slice that ran past its frame (board_vblank homebrew, or a game
                 * stuck in its own error loop) still has to let the UI / MCP thread
                 * take the mutex. emu_sleep_us rounds anything under 1 ms to nothing
                 * on Windows, and a critical section is not fair, so a sub-ms sleep
                 * let the UI starve ("Not Responding"). */
                else               emu_sleep_ms(1);
            }
        }
        else if (s == EMU_STEPPING) {
            emu_mutex_lock(&ctx->mutex);
            int n = ctx->step_count;
            for (int i = 0; i < n && !ctx->cpu->halted; i++) {
                if (i960_step(ctx->cpu, ctx->bus) != 0) break;
                ctx->total_steps++;
            }
            ctx->cpu_snapshot = *ctx->cpu;
            ctx->run_state = EMU_STOPPED;
            emu_mutex_unlock(&ctx->mutex);
        }
        else {
            /* STOPPED. Netplay still has to breathe: the login, the room and the
             * peer handshake all happen before anybody presses Run. */
            emu_netplay_pump(ctx);
            /* ...and a session that is PLAYING cannot, from here: the pump keeps
             * answering "run the frame" and nothing runs it. Say so where the
             * player is looking, and say whether it was a pause or a halt. */
            if (netplay_active()) netplay_board_stopped(ctx->cpu->sfr.ip, ctx->cpu->halted != 0);
            emu_sleep_ms(1);
        }

        int64_t now = emu_now_us();
        if (now - last_sps_time >= 1000000) {
            /* A netplay reset puts the step count back to zero under us, so
             * the mark from a second ago can be ahead of it. Unsigned, that
             * subtraction is a billions-per-second reading for one sample. */
            uint64_t steps = ctx->total_steps;
            ctx->steps_per_second = (uint32_t)(steps >= sps_steps_start
                                               ? steps - sps_steps_start
                                               : steps);
            sps_steps_start = steps;
            last_sps_time   = now;
        }
    }
}

#ifdef _WIN32
static DWORD WINAPI emu_thread_proc(LPVOID p) {
    emu_thread_run_loop((emu_thread_ctx_t *)p);
    return 0;
}
#else
static void *emu_thread_proc(void *p) {
    emu_thread_run_loop((emu_thread_ctx_t *)p);
    return NULL;
}
#endif

/* ---- Public API ---------------------------------------------------------- */

/* The context without a thread behind it: for a host that steps the board itself
 * with emu_slice_body / emu_slice_finish (the web build, which has one thread).
 * thread_alive stays 0, which is also what tells emu_sound_restart and
 * emu_sound_midi there is no run loop to lock out. */
static inline void emu_ctx_init(emu_thread_ctx_t *ctx, i960_cpu_t *cpu, memory_bus_t *bus) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->cpu = cpu;
    ctx->bus = bus;
    ctx->run_state = EMU_STOPPED;
    ctx->cpu_snapshot = *cpu;
    ctx->cpu_prev_snapshot = *cpu;
    emu_mutex_init(&ctx->mutex);
}

static inline void emu_thread_init(emu_thread_ctx_t *ctx, i960_cpu_t *cpu, memory_bus_t *bus) {
    emu_ctx_init(ctx, cpu, bus);
    ctx->thread_alive = 1;

#ifdef _WIN32
    ctx->thread = CreateThread(NULL, 0, emu_thread_proc, ctx, 0, NULL);
#else
    pthread_create(&ctx->thread, NULL, emu_thread_proc, ctx);
#endif
    LOG_INFO("emu: thread started");
}

static inline void emu_thread_shutdown(emu_thread_ctx_t *ctx) {
    ctx->thread_alive = 0;
    ctx->request_stop = 1;
#ifdef _WIN32
    WaitForSingleObject(ctx->thread, 2000);
    CloseHandle(ctx->thread);
#else
    pthread_join(ctx->thread, NULL);
#endif
    emu_mutex_destroy(&ctx->mutex);
    LOG_INFO("emu: thread stopped");
}

static inline void emu_run(emu_thread_ctx_t *ctx) {
    if (ctx->run_state == EMU_STOPPED) ctx->step_over_bp = 1;
    g_wp.hit = 0;          /* clear a reported watchpoint so we don't re-stop instantly */
    ctx->request_stop = 0; /* cancel any pending stop (e.g. from a prior ROM reload) so
                            * the run loop doesn't bail out of the first slice */
    ctx->run_state = EMU_RUNNING;
}
/* ---- Sound board backdoor -------------------------------------------------
 *
 * Reboot the 68000 + SCSP without touching the rest of the board, so a driver
 * that has lost its command stream can be recovered mid-session instead of
 * restarting the emulator. sound_reset() rewrites the 68000, the SCSP and all
 * of sound RAM, and the run loop is inside those for most of a slice, so this
 * waits for the slice boundary the way cop_exec does.
 *
 * The host output ring is deliberately left alone: the audio device is draining
 * it on another thread and emptying it here would click. The MIDI-ring
 * diagnostics are carried across, so a restart does not erase the evidence of
 * why it was needed.
 *
 * Note the driver comes back silent: it boots fresh, but the i960 does not
 * re-send the track that was playing, so music returns at the next cue the game
 * sends (usually the next round). Pass bytes to emu_sound_midi to kick one. */
static inline void emu_sound_restart(emu_thread_ctx_t *ctx) {
    int locked = ctx && ctx->thread_alive;
    if (locked) emu_mutex_lock(&ctx->mutex);
    uint32_t drops = g_sound.scsp.mi_drops;
    uint8_t  hi    = g_sound.scsp.mi_hi;
    uint64_t drains = g_sound.midi_drains;
    sound_reset();
    g_sound.scsp.mi_drops = drops;
    g_sound.scsp.mi_hi    = hi;
    g_sound.midi_drains   = drains;
    if (locked) emu_mutex_unlock(&ctx->mutex);
    LOG_INFO("sound: board restarted (carried over: midi drops=%u hi=%u drains=%llu)",
             drops, (unsigned)hi, (unsigned long long)drains);
}

/* Push bytes straight into the SCSP's MIDI input, as the i960's UART would. */
static inline int emu_sound_midi(emu_thread_ctx_t *ctx, const uint8_t *b, int n) {
    int locked = ctx && ctx->thread_alive;
    if (locked) emu_mutex_lock(&ctx->mutex);
    for (int i = 0; i < n; i++) scsp_midi_in(&g_sound.scsp, b[i]);
    if (locked) emu_mutex_unlock(&ctx->mutex);
    return n;
}

static inline void emu_stop(emu_thread_ctx_t *ctx)  { ctx->request_stop = 1; }
static inline int  emu_is_running(emu_thread_ctx_t *ctx) { return ctx->run_state == EMU_RUNNING; }

static inline void emu_step(emu_thread_ctx_t *ctx, int count) {
    emu_mutex_lock(&ctx->mutex);
    ctx->cpu_prev_snapshot = *ctx->cpu;
    ctx->step_count = count;
    ctx->run_state = EMU_STEPPING;
    emu_mutex_unlock(&ctx->mutex);
    /* Stepping completes within a few hundred microseconds; spin briefly. */
    while (ctx->run_state == EMU_STEPPING) emu_sleep_ms(0);
}

/* UI takes a fresh snapshot at the top of each frame so register-window
 * change highlights don't lag behind the live state. */
static inline void emu_update_snapshots(emu_thread_ctx_t *ctx) {
    emu_mutex_lock(&ctx->mutex);
    ctx->cpu_prev_snapshot = ctx->cpu_snapshot;
    ctx->cpu_snapshot      = *ctx->cpu;
    emu_mutex_unlock(&ctx->mutex);
}

#endif /* EMU_THREAD_H */
