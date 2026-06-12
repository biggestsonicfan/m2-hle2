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
#include <string.h>

#include "constants.h"
#include "log.h"
#include "i960.h"
#include "i960_exec.h"
#include "memory.h"
#include "breakpoint.h"
#include "hle_hooks.h"   /* g_frame_done, hle_call, g_active_profile */
#include "irq_timer.h"   /* board IRQ controller + timers */
#include "../board/sound.h"  /* sound_step (68K), SCSP */

/* 68K at ~11.3 MHz vs i960 at 25 MHz — run 45% as many steps per slice. */
#define M68K_STEPS_PER_SLICE  (EMU_STEPS_PER_SLICE * 45 / 100)

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
   typedef HANDLE           emu_thread_t;
   typedef CRITICAL_SECTION emu_mutex_t;
#  define emu_mutex_init(m)     InitializeCriticalSection(m)
#  define emu_mutex_destroy(m)  DeleteCriticalSection(m)
#  define emu_mutex_lock(m)     EnterCriticalSection(m)
#  define emu_mutex_unlock(m)   LeaveCriticalSection(m)
#  define emu_sleep_ms(ms)      Sleep((DWORD)(ms))
#else
#  include <pthread.h>
#  include <unistd.h>
#  include <time.h>
   typedef pthread_t       emu_thread_t;
   typedef pthread_mutex_t emu_mutex_t;
#  define emu_mutex_init(m)     pthread_mutex_init(m, NULL)
#  define emu_mutex_destroy(m)  pthread_mutex_destroy(m)
#  define emu_mutex_lock(m)     pthread_mutex_lock(m)
#  define emu_mutex_unlock(m)   pthread_mutex_unlock(m)
#  define emu_sleep_ms(ms)      usleep((useconds_t)((ms) * 1000))
#endif

/* ---- Tuning -------------------------------------------------------------- */

#define EMU_CPU_HZ           25000000               /* i960 KB on Model 2 = 25 MHz */
#define EMU_SLICES_PER_SEC   60
#define EMU_SLICE_US         (1000000 / EMU_SLICES_PER_SEC)  /* ~16667 µs */
/* EMU_STEPS_PER_SLICE is in constants.h (500,000), sized to always reach
 * the per-game frame-boundary hook within one batch. */

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
    hle_call(cpu, h, cpu->sfr.ip);           /* vector to handler; ret resumes */
    s_irq_in_service = true;
    g_irqt.deliver_count++;
    g_irqt.deliver_by_pin[pin & 3]++;
}

/* ---- Run loop ------------------------------------------------------------ */

static void emu_thread_run_loop(emu_thread_ctx_t *ctx) {
    uint64_t sps_steps_start = ctx->total_steps;
    int64_t  last_sps_time   = emu_now_us();

    while (ctx->thread_alive) {
        emu_run_state_t s = ctx->run_state;

        if (s == EMU_RUNNING) {
            int64_t slice_start = emu_now_us();

            emu_mutex_lock(&ctx->mutex);
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
            if (g_real_irq) irqt_tick(EMU_CPU_HZ / EMU_SLICES_PER_SEC);
            emu_service_irq(ctx);   /* deliver pending i960 interrupts (sound, …) */
            for (int i = 0;
                 i < EMU_STEPS_PER_SLICE
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
                if (i960_step(ctx->cpu, ctx->bus) != 0) break;
                ctx->total_steps++;
                if (g_log.warn_triggered) break;
                if (g_wp.hit) break;   /* data watchpoint tripped mid-instruction */
                if (g_sharc.unknown_triggered) break;  /* break-on-unknown COP cmd */
            }
            /* Run the 68K sound CPU proportional to the i960 batch. */
            sound_step(M68K_STEPS_PER_SLICE);

            /* Snapshot the GEO display list while the i960 is idle (mutex held) — the
             * homebrew is vblank-waiting just past geo_flush, so bufferram holds the
             * intact list before the next frame's SHARC math clobbers it. */
            if (g_active_profile && g_active_profile->quirks.geo_displaylist)
                geodl_capture(ctx->bus);

            ctx->cpu_prev_snapshot = ctx->cpu_snapshot;
            ctx->cpu_snapshot      = *ctx->cpu;
            emu_mutex_unlock(&ctx->mutex);

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
            } else if (g_frame_done || (board_vblank && g_vblank_acked)) {
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
                int64_t sleep_us = ctx->frame_deadline_us - emu_now_us();
                if (sleep_us > 0) emu_sleep_us(sleep_us);
            } else {
                /* No game-pace hook yet — fall back to fixed slice timing
                 * so the host CPU doesn't pin at 100%. */
                int64_t elapsed   = emu_now_us() - slice_start;
                int64_t remaining = EMU_SLICE_US - elapsed;
                if (remaining > 0) emu_sleep_us(remaining);
                else               emu_sleep_us(500);   /* board_vblank slices can exceed one frame; always yield briefly so the UI / MCP thread can grab the emu mutex (else get_status etc. starve) */
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
            emu_sleep_ms(1);
        }

        int64_t now = emu_now_us();
        if (now - last_sps_time >= 1000000) {
            ctx->steps_per_second = (uint32_t)(ctx->total_steps - sps_steps_start);
            sps_steps_start = ctx->total_steps;
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

static inline void emu_thread_init(emu_thread_ctx_t *ctx, i960_cpu_t *cpu, memory_bus_t *bus) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->cpu = cpu;
    ctx->bus = bus;
    ctx->run_state = EMU_STOPPED;
    ctx->thread_alive = 1;
    ctx->cpu_snapshot = *cpu;
    ctx->cpu_prev_snapshot = *cpu;
    emu_mutex_init(&ctx->mutex);

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
