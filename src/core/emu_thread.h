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
 *   - A slice is one video frame of the board's own clock: the i960 runs
 *     until its cycles reach the next vblank (irq_timer.h), and the run loop
 *     paces each vblank to 1/60 s of wall time.
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
#include "pc_profile.h"  /* i960 instruction counts per address (M2HLE_PROFILE builds) */
#include "hle_hooks.h"   /* hle_interrupt, g_active_profile */
#include "sky_eye.h"     /* SKY EYE mode's stage hold (Pinboard #265) */
#include "irq_timer.h"   /* board IRQ controller + timers */
#include "../board/sound.h"  /* sound_run_slice: the 68000 + SCSP */
#include "../net/netplay.h"  /* the lockstep frame gate (inert unless a session is up) */

/* 68K at ~11.3 MHz vs i960 at 25 MHz — run 45% as many steps per slice. */

/* The mutex/thread types and the millisecond sleep live in thread_mutex.h, so
 * net/netplay.h can use the same mutex without including this header (the run
 * loop below calls into netplay, which would otherwise be a cycle). */
#include "thread_mutex.h"
#include "emu_times.h"    /* emu_now_us, and get_status's "emu" timings */

/* ---- CPU saver: hold the board until a match ----------------------------
 *
 * --idle-until-match, or {"cmd":"idle_hold","on":1}. A player who is only
 * waiting for an online opponent (the fly, sitting in its room) has no use for
 * attract: every match begins with a cold boot of both boards (the barrier's
 * reset), so nothing the board does before that survives into the match. While
 * this is set and no session owns the board, the run loop resets the board
 * once, back to power-on, and then does not step it at all: no i960, no COP, no
 * sound board, no frames. Netplay is still pumped every millisecond, so
 * the login, the room and the barrier go on as before, and when the barrier
 * releases the board boots from there exactly as it would have. The result of
 * a match is read at the frame it is decided, so the hold only takes the board
 * back once the session is over.
 *
 * A bridge client's run_frames still runs its frames (a client waiting on one
 * would otherwise hang); the next hold resets the board again. The native run
 * loop only: the web, libretro and handheld hosts step the board themselves. */
static volatile int g_idle_hold;
#define EMU_IDLE_POLL_US 1000   /* as STOPPED: the room's ping, which sets the delay, is answered from the pump */

/* ---- Tuning -------------------------------------------------------------- */

#define EMU_CPU_HZ           25000000               /* i960 KB on Model 2 = 25 MHz */
#define EMU_SLICES_PER_SEC   60
#define EMU_SLICE_US         (1000000 / EMU_SLICES_PER_SEC)  /* ~16667 µs */
/* EMU_STEPS_PER_SLICE is in constants.h (500,000): a cap above the 416,667
 * cycles to a vblank, so a slice reaches its vblank whatever the program does,
 * since an instruction costs at least a cycle. */

/* The i960 steps one slice may run before it ends short of the vblank. A
 * host too slow for a vblank's worth in 16.7 ms sets it lower, so a busy
 * frame spreads over more slices that each fit. The vblank, the timers and the
 * sound board all keep the board's own clock, so the board runs the same:
 * det_digest at 150,000 differs only in frame 1's RAM, where the warning skip
 * (written once a slice) lands. */
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
    /* The last slice used its whole step budget without reaching the game's
     * frame hook: it stopped MID-FRAME, and there is nothing to pace. */
    int          slice_capped;

    /* A board reset asked for from outside a netplay session (the MCP bridge's
     * `board_reset`). Serviced where the barrier's reset is, by the same code;
     * reset_count is how the asker learns it happened. */
    volatile int      request_reset;
    volatile uint32_t reset_count;

    /* A savestate asked for from outside the emu thread (the MCP bridge's
     * `save_state` / `load_state`, the window's menu): 1 save, 2 load, to or
     * from state_path. Serviced where request_reset is, between slices.
     * state_count moves when it has been, and state_error says how
     * ("" for done). */
    volatile int      request_state;
    char              state_path[1024];
    volatile uint32_t state_count;
    char              state_error[160];

    /* Frames left before the board stops itself (emu_run_frames), 0 for none.
     * Counted down at the frame edge in emu_slice_finish, so the stop lands
     * between frame N and frame N+1 however late the asker would have been
     * with an emu_stop. frame_budget_hit tells the run loop the last frame
     * ended that way (see its pacing). */
    volatile uint32_t frame_budget;
    int               frame_budget_hit;

    /* The board is being held at power-on by g_idle_hold (get_status's
     * "idle_hold"). Emu thread writes, anyone reads. */
    volatile int      idle_holding;

    emu_thread_t thread;
} emu_thread_ctx_t;

/* ---- High-res clock + precise sleep ------------------------------------- */

/* emu_now_us is in emu_times.h, where the board's timers need it too. */
#ifdef _WIN32
/*
 * The 60 Hz throttle sleeps here, and Sleep() is only as fine as the timer
 * resolution THIS process asked for. Since Windows 10 2004 another program
 * raising the system timer no longer lends it to a process that did not ask,
 * so an m2hle with no window and no audio device (--headless) got 15.6 ms
 * ticks: Sleep(14) measured 15.5 ms, a frame ran long, the catch-up clamp
 * threw the deadline away, and a stream showed ~52 board fps and a gap every
 * few frames. A high-resolution waitable timer is a real millisecond without
 * touching the machine-wide resolution -- the same fix headless_sleep_ms made
 * for the render poll. One per thread, because a waitable timer is a single
 * deadline. Where it cannot be made (pre-1803), Sleep is still there.
 */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#  define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
/* emu_nap_us waits even under a millisecond, where emu_sleep_us returns at
 * once: the idle polls that a waiting client's latency hangs on (a stopped
 * board waiting to be run, the bridge waiting for it to stop) use it, because
 * Sleep(1) there is the same 15.6 ms. */
static inline void emu_nap_us(int64_t us) {
    if (us <= 0) return;
    static __declspec(thread) HANDLE timer;
    static __declspec(thread) int tried;
    if (!tried) {
        tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL,
                                       CREATE_WAITABLE_TIMER_MANUAL_RESET |
                                       CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
    }
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -us * 10;                 /* relative, 100 ns units */
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, (DWORD)(us / 1000) + 10);
            return;
        }
    }
    Sleep(us < 1000 ? 1 : (DWORD)(us / 1000));
}
static inline void emu_sleep_us(int64_t us) {
    if (us > 1000) emu_nap_us(us);
}
#else
static inline void emu_sleep_us(int64_t us) {
    if (us > 0) usleep((useconds_t)us);
}
static inline void emu_nap_us(int64_t us) { emu_sleep_us(us); }
#endif

/* ---- Real i960 interrupt delivery --------------------------------------
 * Asserts the sound UART IRQ (intreq bit 10) while the i960 sound queue has
 * data, then vectors the highest-priority pending+enabled IRQ to its handler
 * via hle_call (Model 2 interrupt controller — see irq_timer.h). One injection
 * per slice; s_irq_in_service gates re-entry until the handler's ret unwinds
 * the injected frame. Also poke the per-game warning-skip flag. */
static bool s_irq_in_service     = false;
static int  s_irq_baseline_depth = 0;
/* The handler in service came from the program's own interrupt table
 * (game_quirks_t.irq_vectors), and the slices it has been in service for. */
static bool s_irq_from_table     = false;
static int  s_irq_slices         = 0;
static uint64_t s_timer_cycles_seen = 0;   /* cpu->cycles the board's clock has been given */
#include "i960_blocks.h"     /* the decoded-block cache (I960_BLOCKS builds) */
#ifndef I960_AOT
#define I960_AOT 0
#endif
#include "i960_aot.h"        /* the ROM compiled to C (I960_AOT builds) */
/* The slice ended on a vblank: one video frame of the board ran. */
static volatile int g_vblank_edge = 0;

/* Warning-screen auto-skip: a convenience that MOVES THE TIMELINE against MAME.
 * Holding the profile's flag at 1 (STF: SKIP_WARNING, 0x500410) makes the
 * Japan boot go straight to attract instead of drawing the warning for its
 * 640-frame CTRL_TIMER. Measured on STF powered up as Japan (Pinboard #254):
 * with the skip a game frame here is MAME's frame + 800 (+812 by the attract
 * fight, load timing); with --nowarnskip, + 160, which is MAME's boot before
 * the game counts frames (163 frames to mode 1). USA and export skip the screen
 * on their own, so it only matters for Japan. The graders run Japan with the
 * skip on and pair frames by content (pins, camera), not by a fixed offset.
 * A netplay session always skips, so a peer's menu setting cannot split the
 * two boots. */
static volatile int g_warning_skip = 1;

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
    s_irq_from_table     = false;
    s_irq_slices         = 0;
    s_timer_cycles_seen  = 0;   /* install_fn put cpu->cycles back to 0 */
#if I960_BLOCKS
    s_ib_valid           = 0;   /* new code, perhaps */
#endif
#if I960_AOT
    aot_invalidate();
#endif
    g_vblank_edge        = 0;
    g_versus_result      = 0;
    g_replay_stage_pin   = -1;
    g_xplay_barrier      = 0;
    g_xplay_events       = 0;
    g_xplay_ready        = 0;
    g_xplay_mode         = -1;
    g_xplay_also_mode    = -1;
    g_emu_frames         = 0;
    /* The frame number restarts with the board; the sample clock does not —
     * g_sound.out_total survives a reset, and an A/V client mid-stream would
     * hear the seam as a jump backwards in time. */
    g_frame_clock.frame  = 0;
}

/* ---- Savestates (Pinboard #423) ---------------------------------------------
 *
 * savestate.h writes the board; the run loop's latches above are passed to it
 * here, since they are this file's statics. Serviced between slices, under the
 * mutex (emu_netplay_pump), like a board reset. A build with no zip support
 * (M2HLE_NO_ZIP, the Dreamcast) has none: a state is a zip. */
#ifndef M2HLE_NO_ZIP
#include "savestate.h"

static inline savestate_emu_t emu_state_latches(const emu_thread_ctx_t *ctx) {
    savestate_emu_t e;
    memset(&e, 0, sizeof e);
    e.total_steps        = ctx->total_steps;
    e.timer_cycles_seen  = s_timer_cycles_seen;
    e.frame_clock_sample = g_frame_clock.sample;
    e.frame_clock_frame  = g_frame_clock.frame;
    e.irq_in_service     = s_irq_in_service;
    e.irq_baseline_depth = s_irq_baseline_depth;
    e.irq_from_table     = s_irq_from_table;
    e.irq_slices         = s_irq_slices;
    e.vblank_edge        = g_vblank_edge;
    e.slice_capped       = ctx->slice_capped;
    e.emu_frames         = g_emu_frames;
    e.irqt_sound_kick    = g_irqt_sound_kick;
    e.irqt_vblank        = g_irqt_vblank;
    return e;
}

static inline void emu_state_put_latches(emu_thread_ctx_t *ctx, const savestate_emu_t *e) {
    ctx->total_steps     = e->total_steps;
    s_timer_cycles_seen  = e->timer_cycles_seen;
    s_irq_in_service     = e->irq_in_service != 0;
    s_irq_baseline_depth = e->irq_baseline_depth;
    s_irq_from_table     = e->irq_from_table != 0;
    s_irq_slices         = e->irq_slices;
    g_vblank_edge        = e->vblank_edge;
    ctx->slice_capped    = e->slice_capped;
    g_emu_frames         = e->emu_frames;
    g_irqt_sound_kick    = e->irqt_sound_kick;
    g_irqt_vblank        = e->irqt_vblank;
    g_frame_clock.sample = e->frame_clock_sample;
    g_frame_clock.frame  = e->frame_clock_frame;
}

/* Save or load now. The caller holds the mutex (or has no other thread).
 * NULL on success, else why not. */
static inline const char *emu_state_save_now(emu_thread_ctx_t *ctx, const char *path) {
    if (g_sky_eye.phase != SKY_EYE_OFF) return "SKY EYE is holding the stage; leave it first";
    savestate_emu_t e = emu_state_latches(ctx);
    return savestate_save(path, ctx->cpu, ctx->bus, &e);
}

/* A load's last step, from a file or from memory: the run loop's latches, and
 * what follows the board without being part of it. */
static inline const char *emu_state_loaded(emu_thread_ctx_t *ctx, const char *err, const savestate_emu_t *e) {
    if (err) return err;
    emu_state_put_latches(ctx, e);
    emu_attn_bump();
    ctx->cpu_prev_snapshot = *ctx->cpu;
    ctx->cpu_snapshot      = *ctx->cpu;
    return NULL;
}

static inline const char *emu_state_load_now(emu_thread_ctx_t *ctx, const char *path) {
    if (g_sky_eye.phase != SKY_EYE_OFF) return "SKY EYE is holding the stage; leave it first";
    savestate_emu_t e;
    return emu_state_loaded(ctx, savestate_load(path, ctx->cpu, ctx->bus, &e), &e);
}

/* The same in memory, for a host that keeps the state itself (libretro's
 * retro_serialize). `buf` NULL asks only for the size. */
static inline const char *emu_state_save_mem(emu_thread_ctx_t *ctx, void *buf, size_t cap, size_t *len) {
    if (g_sky_eye.phase != SKY_EYE_OFF) return "SKY EYE is holding the stage; leave it first";
    savestate_emu_t e = emu_state_latches(ctx);
    return savestate_save_mem(buf, cap, len, ctx->cpu, ctx->bus, &e);
}

static inline const char *emu_state_load_mem(emu_thread_ctx_t *ctx, const void *data, size_t size) {
    if (g_sky_eye.phase != SKY_EYE_OFF) return "SKY EYE is holding the stage; leave it first";
    savestate_emu_t e;
    return emu_state_loaded(ctx, savestate_load_mem(data, size, ctx->cpu, ctx->bus, &e), &e);
}
#endif /* M2HLE_NO_ZIP */

/* ---- The sound UART ---------------------------------------------------------
 *
 * The game's sound handler (STF send_sound_code) sends ONE byte of a queued
 * command per interrupt, and TxRDY is up again once that byte is out -- about a
 * third of a millisecond on the board. So the whole of a command, and the whole
 * of the queue behind it, is gone within a millisecond or two of being queued.
 *
 * That promptness is load-bearing in STF, because of a bug in the ROM: its
 * command queue AUDIO_2 (0x504020) is 32 entries by the index mask and the
 * count cap, but only 18 longs are reserved for it. Entries 18-31 overlap
 * byte_50406A, sd_nowait_timer, sd_wait_timer, the check_same_sound list at
 * 0x504078 and the sd_flag pointer, and the game writes those every frame
 * (0xB618, 0x39000, 0x3F53C-0x3F5A0, 0x3F440). A command still waiting in one
 * of those slots when they are written goes out as zeros or a pointer -- the
 * lone data bytes `00 00 00` or `0D B1 A8` in sound_codes -- and the driver
 * reads them under MIDI running status as a command of the last status byte.
 * Which slot the stage's BGM command lands in depends on how many sounds have
 * gone before, so the symptom is some stages, some of the time, with the wrong
 * music or none.
 *
 * Offered once a slice, as it used to be, a queued command waited a whole frame
 * for its interrupt, and the per-frame writers got to it first. Now the pin is
 * offered where the board would take it: when the game enables the line after
 * queueing (irqt_enable_write's kick), and when any handler returns (the
 * chain in emu_service_sound_again). A byte is only handed over when the UART
 * can take it (sound_uart_make_room: its holding register is empty, so the
 * game is paced one byte per TxRDY, 320 us apart, as on the board); a byte
 * that does not fit waits in the game's queue for the next slice, as it would
 * wait on the UART. */

/* Is TxRDY's interrupt up? Raises bit 10 if so.
 *
 * TxRDY is a level: it is up whenever the UART can take a byte, whether or not
 * the game has one, and the board raises the pin whenever the line is enabled
 * as well (MAME model2.cpp sound_ready_w, txrdy_r() && intena). So the game
 * takes one more interrupt after the last byte of a burst, and STF needs it:
 * entered with an empty queue and no command half sent, send_sound_code
 * (0x3F1DC-0x3F1FC) puts the queue's count and both indices back to 0 and
 * returns with the line still masked, which ends the burst. This used to raise
 * the pin only while bytes were queued, so that interrupt never came, the
 * write index walked on through all 32 slots, and every command in time landed
 * in slots 18-31 over sd_flag and the rest (issue #112): the sd_flag sequencer
 * then read a command code as a pointer (unmapped reads at IP 0x3F5AC). */
static inline bool emu_sound_pending(emu_thread_ctx_t *ctx, const game_quirks_t *q) {
    if (!q->sound_queue_count_addr) return false;
    uint32_t cnt = mem_read8(ctx->bus, q->sound_queue_count_addr);
    if (cnt > g_sound.queue_hi) g_sound.queue_hi = cnt;
    if (!(g_irqt.intena & 0x400u)) return false;
    irqt_raise(0x400u);                      /* bit 10 = sound */
    return true;
}

/* Can the UART take a byte now? Makes room in the MIDI buffer if it can. */
static inline bool emu_sound_ready(void) {
    if (sound_uart_make_room(true)) return true;
    g_sound.midi_holds++;
    return false;
}

/* The handler for pin 0..3: the profile's, or with irq_vectors the one the
 * program's own interrupt table gives (hle_irq_vector_handler). 0 = none. */
static inline uint32_t emu_irq_handler(emu_thread_ctx_t *ctx, const game_quirks_t *q, int pin) {
    if (q->irq_handler[pin]) return q->irq_handler[pin];
    return q->irq_vectors ? hle_irq_vector_handler(ctx->cpu, ctx->bus, pin, NULL) : 0;
}

/* A profile's handler runs as STF's always have (hle_interrupt); one from the
 * program's own table the way the processor runs it (hle_interrupt_on_stack). */
static inline void emu_irq_enter(emu_thread_ctx_t *ctx, const game_quirks_t *q, int pin, uint32_t h) {
    s_irq_baseline_depth = ctx->cpu->frame_depth;
    uint32_t vector = 0;
    if (q->irq_handler[pin] || !hle_irq_vector_handler(ctx->cpu, ctx->bus, pin, &vector))
        hle_interrupt(ctx->cpu, h);
    else
        hle_interrupt_on_stack(ctx->cpu, ctx->bus, h, vector);
    s_irq_in_service = true;
    s_irq_from_table = !q->irq_handler[pin];
    s_irq_slices     = 0;
    emu_attn_bump();        /* the run loop services a handler on its slow path */
}

/* Take the sound interrupt now, if it is the one to take. Call with no handler
 * in service. */
static inline void emu_offer_sound(emu_thread_ctx_t *ctx) {
    const game_quirks_t *q = &g_active_profile->quirks;
    if (!(g_irqt.intena & 0x0C00u)) return;
    uint32_t h = emu_irq_handler(ctx, q, 3);
    if (!h) return;
    if (!emu_sound_pending(ctx, q)) return;
    if (irqt_pending_pin() != 3 || !emu_sound_ready()) return;
    emu_irq_enter(ctx, q, 3, h);
}

/* A handler from the program's own table need not return: an SDK kernel's
 * vblank handler can switch to another task (flushreg, a new frame and stack,
 * bx), and the frame depth it was entered at is then never seen again. With
 * nothing else to go on, one still in service after this many slices is taken
 * to have done that, so the pins are not masked for good. */
#define EMU_IRQ_TABLE_MAX_SLICES 8

static inline void emu_service_irq(emu_thread_ctx_t *ctx) {
    if (!g_active_profile) return;
    i960_cpu_t          *cpu = ctx->cpu;
    const game_quirks_t *q   = &g_active_profile->quirks;

    g_hle_netplay_board = netplay_active();

    /* Auto-skip the boot warning screen by holding its ack flag at 1. */
    if ((g_warning_skip || netplay_active()) && q->warning_skip_addr) mem_write32(ctx->bus, q->warning_skip_addr, 1);

    /* Did the in-service handler return? (frame unwound to/below baseline) */
    if (s_irq_in_service && cpu->frame_depth <= s_irq_baseline_depth)
        s_irq_in_service = false;

    /* Sound UART TxRDY: keep bit 10 asserted while the line is enabled. */
    emu_sound_pending(ctx, q);

    if (s_irq_in_service) return;
    int pin = irqt_pending_pin();            /* gated by (intreq & intena) */
    if (pin < 0) return;
    uint32_t h = emu_irq_handler(ctx, q, pin);
    if (!h) return;                          /* pin not yet delivered (still HLE) */
    if (pin == 3 && !emu_sound_ready()) return;

    emu_irq_enter(ctx, q, pin, h);           /* vector to handler; ret resumes, AC/PC restored */
}

/* When a handler returns -- the sound handler after its byte, or any other --
 * and the i960 still has bytes queued, take the sound pin again now rather
 * than at the next slice (see "The sound UART" above). Only the sound pin: the
 * frame-paced pins keep their cadence. */
static inline void emu_service_sound_again(emu_thread_ctx_t *ctx) {
    if (!s_irq_in_service || ctx->cpu->frame_depth > s_irq_baseline_depth) return;
    s_irq_in_service = false;
    emu_offer_sound(ctx);
}

/* The slow path is only waiting for the handler in service to return: no
 * vblank, sound kick, breakpoint, halt or debug trap to look at (the flags
 * after the word, as the run loop's return to the fast path reads them). */
static inline bool emu_aot_in_service(emu_thread_ctx_t *ctx, uint32_t *attn) {
    *attn = g_emu_attn;
    return !g_irqt_vblank && !g_irqt_sound_kick && !ctx->step_over_bp && !ctx->cpu->halted
        && !bp_armed() && !g_log.warn_triggered && !wp_tripped() && !g_sharc.unknown_triggered
        && ctx->cpu->frame_depth > s_irq_baseline_depth;
}

/* ---- The board's clock ------------------------------------------------------
 *
 * The board timers and the vblank both count the i960's cycles (irq_timer.h).
 * The run loop hands them the cycles run so far whenever an event is due
 * (pending reaches horizon), and takes the interrupt between the instructions
 * where it falls. */

/* Slice start: the cycles run since the clock was last brought up to date. */
static inline void emu_timers_slice_begin(emu_thread_ctx_t *ctx) {
    i960_cycle_table_init();
    i960_cpu_t *cpu = ctx->cpu;
    /* A board loaded or reset by a host that does not call
     * emu_board_reset_state starts its count again under us. */
    if (cpu->cycles < s_timer_cycles_seen) s_timer_cycles_seen = cpu->cycles;
    g_irqt.pending += (int64_t)(cpu->cycles - s_timer_cycles_seen);
    s_timer_cycles_seen = cpu->cycles;
    irqt_flush();
}

/* After each instruction: count its cycles, and take an interrupt as soon as
 * one is pending and none is in service. The sound pin has its own rules
 * (emu_offer_sound) and is left to them. The cycles since the last step are
 * one instruction's, or a hook's run that ends before the horizon, so their
 * difference is taken in 32 bits (irqt_count_t). */
static inline void emu_timers_after_step(emu_thread_ctx_t *ctx) {
    i960_cpu_t *cpu = ctx->cpu;
    g_irqt.pending += (irqt_count_t)(uint32_t)(cpu->cycles - s_timer_cycles_seen);
    s_timer_cycles_seen = cpu->cycles;
    if (g_irqt.pending >= g_irqt.horizon) irqt_flush();
    if (!s_irq_in_service && (g_irqt.intreq & g_irqt.intena & 0x03FFu) && g_active_profile)
        emu_service_irq(ctx);
}

/* The same on the run loop's fast path, where no handler is in service
 * (entering one bumps the attention word, which sends the loop slow) and the
 * profile is the slice's: two loads an instruction fewer. */
static inline void emu_timers_after_step_fast(emu_thread_ctx_t *ctx, i960_cpu_t *cpu, bool profile) {
    g_irqt.pending += (irqt_count_t)(uint32_t)(cpu->cycles - s_timer_cycles_seen);
    s_timer_cycles_seen = cpu->cycles;
    if (g_irqt.pending >= g_irqt.horizon) irqt_flush();
    if ((g_irqt.intreq & g_irqt.intena & 0x03FFu) && profile)
        emu_service_irq(ctx);
}

/* ---- The sound board against the board's clock ---------------------------
 *
 * The sound board is charged a frame of samples at each vblank, 735 at
 * 44.1 kHz. Mid-frame it still advances as far as the MIDI conversation needs:
 * sound_uart_make_room runs it early and `ahead` owes those samples back at the
 * edge. What it never does is gain time (SLICE-CLOCKS.md). */
static inline bool emu_sound_slice_end(bool frame) {
    if (!frame) return false;
    sound_run_slice(EMU_SLICES_PER_SEC);
    return true;
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
    /* The player pressed a button at "nobody else is in the room": the same
     * reset, back in their own settings (netplay_empty_room_pump). */
    bool alone = step == NETPLAY_STEP_OFF && netplay_take_empty_restart();

    /* A savestate. A load inside a session would change this board and not the
     * peer's, so it is refused there; a save is only a read, and is not. */
    int state = ctx->request_state;
    if (state) {
        ctx->request_state = 0;
        const char *err;
        emu_mutex_lock(&ctx->mutex);
        if (state == 2 && step != NETPLAY_STEP_OFF) err = "a netplay session owns the board";
#ifdef M2HLE_NO_ZIP
        else err = "this build has no savestates";
#else
        else if (state == 2) err = emu_state_load_now(ctx, ctx->state_path);
        else                 err = emu_state_save_now(ctx, ctx->state_path);
#endif
        snprintf(ctx->state_error, sizeof ctx->state_error, "%s", err ? err : "");
        emu_mutex_unlock(&ctx->mutex);
        ctx->state_count++;
    }

    if (step == NETPLAY_STEP_RESET || asked || alone) {
        emu_mutex_lock(&ctx->mutex);
        if (alone)      netplay_restart_alone();
        else if (asked) asked = netplay_reset_board_now();
        else            netplay_do_reset();
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
 *   emu_slice_body    one slice of the board: interrupts, the i960 to the next
 *                     vblank, the sound board, the snapshots. Call with the emu
 *                     mutex held where there is one.
 *   emu_slice_finish  what the slice ended on: a stop (breakpoint, watchpoint,
 *                     halt, ...), a vblank -- counted, and handed to
 *                     netplay -- or neither. The caller paces on the answer. A
 *                     frame that ended as a stop arrived is still a FRAME (and
 *                     the run state is STOPPED as well): it ran, so it is
 *                     counted and paced like any other. */
typedef enum {
    EMU_SLICE_STOPPED,   /* the run state is STOPPED now, mid-frame */
    EMU_SLICE_FRAME,     /* the board reached its vblank: pace to the next 60 Hz tick */
    EMU_SLICE_NO_FRAME,  /* the step cap ended the slice short of the vblank */
} emu_slice_result_t;

/* A pending stop, honoured BEFORE a slice rather than inside one: a slice that
 * has begun runs its frame to the end (see the i960 loop in emu_slice_body).
 * True when the run state is STOPPED now and no slice should run. */
static inline bool emu_slice_should_stop(emu_thread_ctx_t *ctx) {
    if (!ctx->request_stop) return false;
    ctx->request_stop = 0;
    ctx->frame_budget = 0;
    ctx->run_state = EMU_STOPPED;
    return true;
}

/* A handler from the program's own table that has not returned for this many
 * slices has switched task (m2-sdk's break-in): stop waiting for its return. */
static inline void emu_slice_irq_stale(emu_thread_ctx_t *ctx) {
    if (s_irq_in_service && s_irq_from_table && ++s_irq_slices > EMU_IRQ_TABLE_MAX_SLICES) {
        LOG_WARN("emu: interrupt handler never returned (task switch?) -- IP=0x%08X", ctx->cpu->sfr.ip);
        s_irq_in_service = false;
    }
}

/* Whether a slice starts on the slow path (see the loop in emu_slice_body):
 * any of the rare flags already up, or a step over a breakpoint to make. */
static inline bool emu_slice_starts_slow(emu_thread_ctx_t *ctx, bool profile) {
    return ctx->step_over_bp || g_irqt_vblank || g_irqt_sound_kick
        || g_log.warn_triggered || wp_tripped() || g_sharc.unknown_triggered || (profile && s_irq_in_service)
        || ctx->cpu->halted;
}

/* The flags that end a slice after the instruction that raised them. */
static I960_HOT_INLINE bool emu_slice_break_flag(void) {
    if (g_log.warn_triggered) return true;
    if (wp_tripped()) return true;   /* data watchpoint tripped mid-instruction */
    if (g_sharc.unknown_triggered) return true;  /* break-on-unknown COP cmd */
    return false;
}

/* The slow path's checks before an instruction. True when the slice ends here. */
static I960_HOT_INLINE bool emu_slice_slow_gate(emu_thread_ctx_t *ctx, i960_cpu_t *cpu) {
    if (cpu->halted) return true;
    /* The vblank came on the instruction before (its interrupt, if
     * the program enabled one, is taken already): the frame ends. */
    if (g_irqt_vblank) { g_irqt_vblank = 0; g_vblank_edge = 1; return true; }
    if (ctx->step_over_bp) {
        ctx->step_over_bp = 0;
    } else if (bp_check(cpu->sfr.ip)) {
        return true;
    }
    return false;
}

/* The loop's own state, which the slow path's helpers move along. The loop
 * keeps it in locals and hands the slow path a copy: with the struct itself
 * as the loop's state, its address taken, GCC kept the fields in memory and
 * the slice ran 4% more host instructions (Pinboard #387). */
typedef struct {
    int      i;        /* instructions charged to the slice */
    uint32_t steps;    /* instructions run, written back once (see emu_slice_body) */
    uint32_t attn;     /* g_emu_attn as the fast path last saw it */
    bool     bps;      /* a breakpoint is armed */
    bool     slow;     /* every check runs on this instruction */
} emu_slice_loop_t;

/* The slow path's work after an instruction. True when the slice ends here. */
static I960_HOT_INLINE bool emu_slice_slow_after(emu_thread_ctx_t *ctx, i960_cpu_t *cpu, bool profile,
                                                 emu_slice_loop_t *l) {
    l->slow = true;
    if (g_hle_extra) {          /* the hook's run, counted as the i960's */
        l->i     += (int)g_hle_extra;
        l->steps += g_hle_extra;
        g_hle_extra = 0;
    }
    if (profile) {
        if (s_irq_in_service) emu_service_sound_again(ctx);
        else if (g_irqt_sound_kick) { g_irqt_sound_kick = 0; emu_offer_sound(ctx); }
    }
    emu_timers_after_step(ctx);
    if (emu_slice_break_flag()) return true;
    /* Back to the fast path once nothing it was sent here for is still
     * up. STF writes the interrupt registers a few times a frame (the
     * sound kick), and staying slow for the rest of the slice put ~30%
     * of its instructions through every check above. The word is read
     * before the flags: a setter raises its flag and then bumps it, so
     * one that lands after the read is seen at the next instruction.
     * A handler in service stays here until it returns (0.06% of
     * STF's instructions), so the fast path never looks for one. */
    uint32_t now = g_emu_attn;
    if (!g_irqt_vblank && !g_irqt_sound_kick
            && !ctx->step_over_bp && !(profile && s_irq_in_service) && !cpu->halted) {
        l->attn = now;
        l->bps  = bp_armed();
        l->slow = false;
    }
    return false;
}

/* The end of a slice that reached the vblank. A profile with no frame hook
 * marks the geo capture's frame boundary and a capture's frame here, at the
 * vblank, as the profiles' frame hooks do at the end of the game's main loop.
 * Without it geo3d falls back to scanning the WHOLE capture ring (the 24K-word
 * COP boot firmware + every accumulated frame) instead of just this frame's
 * draws, so a homebrew object draw never isolates / renders. */
static inline void emu_slice_frame_edge(emu_thread_ctx_t *ctx) {
    if (g_active_profile && g_active_profile->quirks.board_vblank) {
        cop_geo_frame_edge();
        dl_frame_edge(ctx->bus, g_emu_frames);
        hle_match_replay_edge(ctx->bus);
    }
    sky_eye_edge(ctx->bus);
}

/* After the i960's part of a slice: the sound board's frame, the display
 * list's snapshot, the CPU snapshots and the profiler's sample. */
static inline void emu_slice_after(emu_thread_ctx_t *ctx, bool frame, int64_t prof_t0, uint64_t steps) {
    /* The sound board runs on its own sample clock: a frame's worth of
     * 44.1 kHz samples when the frame ends, the 68000 in lockstep with the
     * SCSP (see emu_sound_slice_end). They run on the sound thread while the
     * next slice's i960 does (sound.h, "The sound thread"). */
#ifdef M2HLE_PROFILE
    int64_t snd_t0 = g_pcprof_on ? emu_now_us() : 0;
#endif
    emu_sound_slice_end(frame);
#ifdef M2HLE_PROFILE
    if (g_pcprof_on) g_pcprof.sound_us += emu_now_us() - snd_t0;
#endif

    /* Snapshot the GEO display list while the i960 is idle (mutex held) — the
     * homebrew is vblank-waiting just past geo_flush, so bufferram holds the
     * intact list before the next frame's SHARC math clobbers it. */
    if (g_active_profile && g_active_profile->quirks.geo_displaylist)
        geodl_capture(ctx->bus);

    ctx->cpu_prev_snapshot = ctx->cpu_snapshot;
    ctx->cpu_snapshot      = *ctx->cpu;
    if (g_pcprof_on)
        pcprof_frame((int32_t)(emu_now_us() - prof_t0), (uint32_t)steps, g_emu_frames);
}

static inline void emu_slice_body(emu_thread_ctx_t *ctx) {
    int64_t  prof_t0 = g_pcprof_on ? emu_now_us() : 0;
    int      hzone   = hprof_enter(HPROF_I960);   /* host_prof.h; COP and sound tag themselves */
    /* A slice runs the board to its next vblank (irq_timer.h), whatever the
     * program is doing then: the frame is the board's, not the game's. */
    g_vblank_edge = 0;
    g_geodl_full_snap = g_active_profile && g_active_profile->quirks.geo_displaylist;
    emu_slice_irq_stale(ctx);
    /* The cycles run outside a slice (a single step, a load). */
    emu_timers_slice_begin(ctx);
    emu_service_irq(ctx);   /* deliver pending i960 interrupts (sound, …) */
    int max_steps = g_emu_steps_per_slice;
    /* The step count is accumulated in a register and written back once.
     * ctx->total_steps is volatile -- netplay hashes it into every frame's
     * desync check -- so incrementing it in the loop was a load and a store
     * to memory per instruction that the compiler was not allowed to keep
     * in a register. The write-back is before anything reads it: the frame
     * edge and netplay_end_frame are both past the loop. */
    /* NOT `!ctx->request_stop`. A slice charges a whole frame to the timers
     * above and to the sound board below whatever the i960 does in between,
     * so a stop that cut the i960 short left that frame to be run again on
     * resume -- and paid for twice: a bare stop/run 30 times a second cost
     * the board 52 fps and put 762 samples to the frame instead of 735. A
     * stop now takes effect between slices (emu_slice_should_stop), and a
     * slice that has begun runs its frame to the end. */
    /* One loop, two modes. The rare flags -- the vblank, the sound
     * kick, break-on-warn, watchpoint, break-on-unknown-COP, a breakpoint armed
     * mid-slice, an interrupt handler entered -- are read through ONE word,
     * g_emu_attn, that each of their setters bumps (attention.h). Until it
     * moves the loop checks nothing else of theirs; the instruction that moves
     * it gets every check in the order below ("slow"), which is the loop as it
     * was, and so does every instruction after it until none of the flags is
     * still up. A slice that starts with any of them set, or with a step over
     * a breakpoint to make, starts slow. Kept as one loop on purpose:
     * i960_step_hot is force-inlined, and a second copy of it pushed GCC past
     * its inlining limits on the handheld -- mem_fetch2 became a call per
     * instruction, and the two-loop version ran 5% MORE instructions. */
    const bool     profile = g_active_profile != NULL;
    int      i;
    uint32_t steps = 0;   /* at most max_steps; 32-bit for the SH-4 */
    uint32_t attn  = g_emu_attn;
    bool     bps   = bp_armed();
    bool     slow  = emu_slice_starts_slow(ctx, profile);
    hle_filter_sync();
    /* The loop's host time, less the sound board it ran early inside it
     * (sound_uart_make_room) or waited on (sound_settle): the i960 and the
     * COP (emu_times.h). */
    const int64_t loop_t0  = emu_now_us();
    const int64_t loop_snd = g_emu_times.sound_inline_us + g_emu_times.sound_wait_us;
    /* A halt comes from the instruction just run: the step's own (it returns
     * -1 and the loop stops at once) or a hook's, which bumps the word, so the
     * fast path does not test it. The CPU and bus are the context's for the
     * whole slice; as locals they stay in registers where ctx->cpu was
     * reloaded after every store. */
    i960_cpu_t   *const cpu = ctx->cpu;
    memory_bus_t *const bus = ctx->bus;
#if I960_AOT
    const bool aot = aot_check(bus);
    uint32_t   svc_attn;
#endif
    for (i = 0; i < max_steps; i++) {
        if (M2_UNLIKELY(slow)) {
            if (emu_slice_slow_gate(ctx, cpu)) break;
        } else if (M2_UNLIKELY(bps) && bp_check(cpu->sfr.ip)) {
            break;
        }
#if I960_AOT
        /* The compiled code (i960_aot.h), on the same terms as a block. */
        if (aot && !slow && !bps && !g_pcprof_on && aot_lead(cpu->sfr.ip)
                && !(g_irqt.intreq & g_irqt.intena & 0x03FFu)) {
            int halt;
            uint32_t k = aot_run(cpu, bus, (uint32_t)(max_steps - i), attn, -1, &halt);
            if (M2_UNLIKELY(halt)) { i += (int)k; steps += k; break; }
            if (k) { g_aot_ops += k; i += (int)k - 1; steps += k - 1; goto ib_ran; }
        } else if (aot && slow && profile && s_irq_in_service && !g_pcprof_on && aot_lead(cpu->sfr.ip)
                && emu_aot_in_service(ctx, &svc_attn)) {
            /* A handler in service, and nothing else the slow path is here
             * for: until it returns no interrupt is taken, so the run is the
             * fast path's, ending at the ret that unwinds it (aot_unwound). */
            int halt;
            uint32_t k = aot_run(cpu, bus, (uint32_t)(max_steps - i), svc_attn, s_irq_baseline_depth, &halt);
            if (M2_UNLIKELY(halt)) { i += (int)k; steps += k; break; }
            if (k) { g_aot_ops += k; i += (int)k - 1; steps += k - 1; goto ib_ran; }
        }
#endif
#if I960_BLOCKS
        /* A decoded block, when nothing can come between its instructions that
         * this path would have to see (i960_blocks.h): it counts as the steps
         * it ran, and the checks after a step follow its last. */
        if (!slow && !bps && !g_pcprof_on) {
            const ib_block_t *b = ib_lookup(cpu, bus, cpu->sfr.ip);
            if (b->n && b->n <= (uint32_t)(max_steps - i)
                    && g_irqt.pending + (irqt_count_t)b->cyc < g_irqt.horizon
                    && !(g_irqt.intreq & g_irqt.intena & 0x03FFu)) {
#ifdef IB_WHY
                uint32_t t0 = IBW_T();
                uint32_t k = ib_run(cpu, bus, b, attn);
                { uint32_t d = IBW_D(t0); g_ib.ns[0] += d; ((ib_block_t *)b)->t += d; ((ib_block_t *)b)->r++; }
#else
                uint32_t k = ib_run(cpu, bus, b, attn);
#endif
                g_ib.ops += k; g_ib.runs++;
                i += (int)k - 1; steps += k - 1;
                goto ib_ran;
            }
#ifdef IB_WHY
            g_ib.why[!b->n ? 1 : b->n > (uint32_t)(max_steps - i) ? 2
                     : !(g_irqt.pending + (irqt_count_t)b->cyc < g_irqt.horizon) ? 3 : 4]++;
        } else {
            g_ib.why[0]++;
#endif
        }
#endif
        PCPROF_TICK(cpu->sfr.ip);
        /* A hook may stand in for several instructions (g_hle_room); on the
         * slow path, or with a breakpoint armed, it is offered only this one,
         * so every check below still sees each instruction. */
#ifdef IB_WHY
        {
            uint32_t t0 = IBW_T();
            int r = i960_step_core(cpu, bus, (slow || bps) ? 1u : (uint32_t)(max_steps - i));
            uint32_t dt = IBW_D(t0);
            if (g_hle_extra) { g_ib.ns[2] += dt; g_ib.why[5] += g_hle_extra; } else g_ib.ns[1] += dt;
            if (M2_UNLIKELY(r != 0)) break;
        }
#else
        if (M2_UNLIKELY(i960_step_core(cpu, bus,
                                       (slow || bps) ? 1u : (uint32_t)(max_steps - i)) != 0)) break;
#endif
#if I960_BLOCKS || I960_AOT
    ib_ran:
#endif
        steps++;
        if (M2_UNLIKELY(slow || g_emu_attn != attn)) {
            emu_slice_loop_t l = { i, steps, attn, bps, slow };
            bool stop = emu_slice_slow_after(ctx, cpu, profile, &l);
            i = l.i; steps = l.steps; attn = l.attn; bps = l.bps; slow = l.slow;
            if (stop) break;
        } else {
            /* Fast: no handler in service (entering one bumps the word). */
            emu_timers_after_step_fast(ctx, cpu, profile);
            if (g_emu_attn != attn) {      /* flagged by the timer service */
                slow = true;
                if (emu_slice_break_flag()) break;
            }
        }
    }
    g_emu_times.loop_us += emu_now_us() - loop_t0
                         - (g_emu_times.sound_inline_us + g_emu_times.sound_wait_us - loop_snd);
    g_emu_times.steps   += steps;
    ctx->total_steps += steps;
    ctx->slice_capped = (i >= max_steps);
    bool frame = g_vblank_edge != 0;
    if (frame) emu_slice_frame_edge(ctx);
    emu_slice_after(ctx, frame, prof_t0, steps);
    hprof_leave(hzone);
}

/* A slice that reached the board's vblank: count the frame and do its
 * bookkeeping. */
static inline void emu_slice_count_frame(emu_thread_ctx_t *ctx) {
    g_emu_frames++;   /* frame clock for the MCP bridge / capture tools */
    g_dl_frame_now = g_emu_frames;
    /* Stamp the frame with the board audio produced up to its end: the
     * slice's own samples, which sound_run_slice handed out in the body
     * above and the sound thread may still be making (out_due is where
     * they end). sample first, frame second (see g_frame_clock). */
    g_frame_clock.sample = g_sound.out_due;
    g_frame_clock.frame  = g_emu_frames;
    /* The netplay frame clock and this frame's state check. Fed the
     * snapshot rather than the live CPU: it was taken under the mutex
     * a few lines up and is the same state, without racing the UI. */
    /* The versus hook's verdict belongs to the frame it happened in: taken
     * here, at the frame boundary every board in a room shares. */
    int versus_result = g_versus_result;
    g_versus_result = 0;
    netplay_end_frame(&ctx->cpu_snapshot, ctx->total_steps, versus_result);
    if (sndcap_on()) sndcap_frame(g_emu_frames, mem_read32(ctx->bus, 0x500020));
    /* A PS3 match plays on the board as it is, with no reset: from here on
     * it is the match's, not the player's (core/backup_ram.h). */
    if (g_xplay_match) backup_ram_detach();
    backup_ram_frame(ctx->bus->back);
    /* The last frame of an emu_run_frames: stop here, after the frame's
     * bookkeeping and before another slice can begin frame N+1. The
     * frame is still a FRAME to the caller. */
    if (ctx->frame_budget && --ctx->frame_budget == 0) {
        ctx->run_state = EMU_STOPPED;
        ctx->frame_budget_hit = 1;
    }
}

/* Does the slice end in a stop: a breakpoint, a watchpoint, a break-on-warn,
 * an unknown COP command, a request or a halt? */
static inline bool emu_slice_stop_wanted(const emu_thread_ctx_t *ctx) {
    return bp_hit() || wp_tripped() || g_log.warn_triggered || g_sharc.unknown_triggered
        || ctx->request_stop || ctx->cpu->halted;
}

/* Stop the board, saying why, and clear the one-shot triggers. */
static inline void emu_slice_stop(emu_thread_ctx_t *ctx) {
    if (bp_hit()) {
        LOG_INFO("emu: breakpoint hit @ 0x%08X", g_bp.hit_addr);
        g_bp.hit = 0;
    }
    if (wp_tripped()) {
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
    ctx->frame_budget = 0;   /* a stop for any other reason ends a run_frames */
    ctx->run_state = EMU_STOPPED;
    if (ctx->cpu->halted) {
        LOG_WARN("emu: CPU halted @ IP=0x%08X (steps=%llu)",
                 ctx->cpu->sfr.ip, (unsigned long long)ctx->total_steps);
    }
}

static inline emu_slice_result_t emu_slice_finish(emu_thread_ctx_t *ctx) {
    /* The frame first, the stop second. The other way round, a stop that
     * landed as a frame ended threw the frame's bookkeeping away: it went
     * uncounted, netplay never heard of it, and -- the part that showed -- it
     * was never paced, so every such stop handed the board a free frame. A
     * bridge client pausing around its reads (flystf) ran the board ~5% fast:
     * 770 samples to the counted frame instead of 735, and a stream whose
     * playout overflowed and jumped every few seconds. */
    bool frame = g_vblank_edge != 0;
    if (frame) emu_slice_count_frame(ctx);
    if (emu_slice_stop_wanted(ctx)) {
        emu_slice_stop(ctx);
        return frame ? EMU_SLICE_FRAME : EMU_SLICE_STOPPED;
    }
    return frame ? EMU_SLICE_FRAME : EMU_SLICE_NO_FRAME;
}

/* One pass of the hold (g_idle_hold): back to power-on if the board has run
 * since its last reset, then nap. A board at power-on has no steps behind it,
 * so a launch with the hold on, or a second pass, only naps. */
static inline void emu_idle_hold(emu_thread_ctx_t *ctx) {
    if (ctx->total_steps != 0) {
        emu_mutex_lock(&ctx->mutex);
        bool reset = netplay_reset_board_now();
        if (reset) {
            ctx->total_steps       = 0;    /* as the barrier's reset: see emu_netplay_pump */
            ctx->cpu_prev_snapshot = ctx->cpu_snapshot;
            ctx->cpu_snapshot      = *ctx->cpu;
            ctx->frame_deadline_us = 0;
        }
        emu_mutex_unlock(&ctx->mutex);
        if (reset) LOG_INFO("emu: idle hold: the board is back at power-on until a match");
    }
    ctx->idle_holding = 1;
    ctx->steps_per_second = 0;         /* the loop's own reading is skipped by the hold */
    emu_nap_us(EMU_IDLE_POLL_US);
}

/* Pace the board to 60 Hz after a slice that reached its vblank: sleep out to
 * the next 16.67ms tick, measured from work_t1, where the slice's work ended. */
static inline void emu_pace_frame(emu_thread_ctx_t *ctx, int64_t work_t1) {
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
    /* A netplay watcher behind the fighters runs flat out until it
     * has caught up (netplay_catching_up). */
    /* The frame that ended an emu_run_frames is not slept out here:
     * the asker is waiting on the stop. The deadline stands, so the
     * first frame of the next run waits out both ticks and the
     * board still keeps 60 Hz. A client that answers within the
     * tick thinks for free, where sleeping first would add a
     * frame per call. */
    bool budget_hit = ctx->frame_budget_hit != 0;
    ctx->frame_budget_hit = 0;
    if (unthrottled || netplay_catching_up()) ctx->frame_deadline_us = 0;
    else if (sleep_us > 0 && !budget_hit) {
        emu_sleep_us(sleep_us);
        g_emu_times.pace_us += emu_now_us() - work_t1;
    }
}

/* Netplay's say over the slice about to run (emu_run_running). True when it
 * runs; false when the loop goes round again without one. */
static inline bool emu_netplay_allows_slice(emu_thread_ctx_t *ctx, int64_t slice_start) {
    /* Netplay decides what this slice may do. STEP_OFF — one predictable
     * branch — whenever no session is running. */
    netplay_step_t np_step = emu_netplay_pump(ctx);
    if (np_step == NETPLAY_STEP_WAIT) {
        /* Stalled waiting for the peer's input, or waiting at the barrier:
         * the board must not advance. Sleep a tick so the UI and the
         * network both get the host CPU, and come back to re-poll. */
        emu_sleep_ms(1);
        g_emu_times.net_us += emu_now_us() - slice_start;
        return false;
    }
    if (np_step == NETPLAY_STEP_RESET) {           /* done above; re-enter */
        g_emu_times.net_us += emu_now_us() - slice_start;
        return false;
    }
    if (np_step == NETPLAY_STEP_OFF && g_idle_hold && !ctx->frame_budget) {
        emu_idle_hold(ctx);
        g_emu_times.net_us += emu_now_us() - slice_start;
        return false;
    }
    return true;
}

/* The run loop's RUNNING state: one slice, then its pacing. False when no
 * slice ran and the loop goes straight round again. */
static inline bool emu_run_running(emu_thread_ctx_t *ctx) {
    if (emu_slice_should_stop(ctx)) return false;
    int64_t slice_start = emu_now_us();

    if (!emu_netplay_allows_slice(ctx, slice_start)) return false;
    ctx->idle_holding = 0;
    int64_t work_t0 = emu_now_us();
    g_emu_times.net_us += work_t0 - slice_start;

    emu_mutex_lock(&ctx->mutex);
    emu_slice_body(ctx);
    emu_mutex_unlock(&ctx->mutex);

    emu_slice_result_t slice = emu_slice_finish(ctx);
    int64_t work_t1 = emu_now_us();
    emu_times_slice(work_t1 - work_t0, slice == EMU_SLICE_FRAME);
    if (slice == EMU_SLICE_STOPPED) {
        /* nothing to pace: the run state is STOPPED now */
    } else if (slice == EMU_SLICE_FRAME) {
        /* The vblank: pace to the next 16.67ms tick. */
        emu_pace_frame(ctx, work_t1);
    } else {
        /* The step cap ended the slice short of the vblank: a program
         * whose instructions cost less than a cycle each on average,
         * which only hooks standing in for many do. The frame is not
         * over; go straight back in, and let the UI have the mutex the
         * next slice releases. */
        emu_yield();
    }
    return true;
}

/* The run loop's STEPPING state: the asked-for instructions, then STOPPED. */
static inline void emu_run_stepping(emu_thread_ctx_t *ctx) {
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

/* The run loop's STOPPED state. Netplay still has to breathe: the login, the
 * room and the peer handshake all happen before anybody presses Run. */
static inline void emu_run_stopped(emu_thread_ctx_t *ctx) {
    ctx->idle_holding = 0;
    emu_netplay_pump(ctx);
    /* ...and a session that is PLAYING cannot, from here: the pump keeps
     * answering "run the frame" and nothing runs it. Say so where the
     * player is looking, and say whether it was a pause or a halt. */
    if (netplay_active()) netplay_board_stopped(ctx->cpu->sfr.ip, ctx->cpu->halted != 0);
    /* A real millisecond, not Sleep(1): a bridge client stepping the
     * board with run_frames waits this long for every run to start. */
    emu_nap_us(1000);
}

static void emu_thread_run_loop(emu_thread_ctx_t *ctx) {
    uint64_t sps_steps_start = ctx->total_steps;
    int64_t  last_sps_time   = emu_now_us();
    hprof_name_thread("m2-emu");

    while (ctx->thread_alive) {
        emu_run_state_t s = ctx->run_state;
        hprof_tick();                   /* the in-process profiler's start / stop (host_prof.h) */

        if (s == EMU_RUNNING) {
            if (!emu_run_running(ctx)) continue;
        }
        else if (s == EMU_STEPPING) {
            emu_run_stepping(ctx);
        }
        else {
            emu_run_stopped(ctx);
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
    hprof_shutdown();                   /* a run cut short still writes its report */
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
    /* The sound thread stays parked for the life of the process, but must not
     * be mid-run when the host frees the sample ROMs it reads. */
    sound_settle();
    emu_mutex_destroy(&ctx->mutex);
    LOG_INFO("emu: thread stopped");
}

static inline void emu_run_frames(emu_thread_ctx_t *ctx, uint32_t frames) {
    if (ctx->run_state == EMU_STOPPED) ctx->step_over_bp = 1;
    g_wp.hit = 0;          /* clear a reported watchpoint so we don't re-stop instantly */
    ctx->request_stop = 0; /* cancel any pending stop (e.g. from a prior ROM reload) so
                            * the run loop doesn't bail out of the first slice */
    ctx->frame_budget = frames;
    ctx->run_state = EMU_RUNNING;
}
/* Run until stopped. emu_run_frames(ctx, n) instead stops by itself at the end
 * of the n-th game frame from here. */
static inline void emu_run(emu_thread_ctx_t *ctx) { emu_run_frames(ctx, 0); }
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

/* Send bytes down the sound UART's line, as the i960 would. */
static inline int emu_sound_midi(emu_thread_ctx_t *ctx, const uint8_t *b, int n) {
    int locked = ctx && ctx->thread_alive;
    if (locked) emu_mutex_lock(&ctx->mutex);
    sound_settle();
    for (int i = 0; i < n; i++) sound_uart_write(&g_sound, b[i], g_sound.m68k.cpu.cycles);
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
