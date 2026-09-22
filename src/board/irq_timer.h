/*
 * irq_timer.h — Model 2 i960 interrupt controller + 4 board timers (board-level).
 *
 * Replaces the plain-buffer HLE of the IRQ/timer MMIO with the real hardware
 * model (verified against MAME model2.cpp — see memory
 * "Model 2 IRQ/timer hardware spec"):
 *
 *   IRQ_REQUEST 0xE80000 : read = intreq (pending bits); WRITE = ACK (intreq &= data)
 *   IRQ_ENABLE  0xE80004 : read/write = intena (mask)
 *   TIMERS      0xF00000 : 4× 20-bit one-shot down-counters @ 25 MHz; on expiry,
 *                          raise intreq bit (TNum+2) if enabled. The i960 Timer
 *                          handler re-arms by writing 0xFFFFF back.
 *
 *   intreq bit → i960 IRQ pin (irq_update): bit0→pin0, bit1→pin1, bits2..9→pin2,
 *   bits10..11→pin3.  Each pin vectors (via the game's _intr_table) to a handler:
 *   pin0=VsyncScr, pin1=VsyncObj, pin2=Timer(board timers), pin3=Other(sound UART).
 *
 * The handler ADDRESSES are game-specific and live in game_profile_t; the
 * mechanism here is board-level.  Interrupt *delivery* (vectoring to a handler
 * via hle_call) is driven from emu_thread.h where the CPU is available.
 */
#ifndef IRQ_TIMER_H
#define IRQ_TIMER_H

#include <stdint.h>
#include <stdbool.h>

#define IRQT_TIMERS 4

typedef struct {
    uint32_t intreq;                 /* pending interrupt bits (0xE80000)   */
    uint32_t intena;                 /* interrupt enable mask  (0xE80004)   */

    int64_t  timer_count[IRQT_TIMERS];   /* current down-count (cycles)     */
    bool     timer_run[IRQT_TIMERS];     /* one-shot armed flag              */

    /* Live timers (g_irqt_live): cycles the i960 has run that the counts do not
     * reflect yet, and the smallest running count — once pending reaches it a
     * timer expires, so the run loop brings the counts up to date then. */
    int64_t  pending;
    int64_t  horizon;
} irq_timer_t;

static irq_timer_t g_irqt = {0};

/* ---- IRQ controller register access (called from memory.h) -------------- */

/* Set when the game ACKs (clears) the vblank pending bit (intreq bit0) — i.e. it
 * finished a frame and consumed the vsync. The emu thread uses this to end a
 * board_vblank slice at the frame boundary instead of busy-spinning the homebrew's
 * vsync wait loop, which both throttles the i960 to 60 Hz and frees the host CPU. */
static volatile int g_vblank_acked = 0;

static inline uint32_t irqt_request_read(void)        { return g_irqt.intreq; }
static inline void     irqt_request_ack (uint32_t d)  {
    if ((g_irqt.intreq & 1u) && !(d & 1u)) g_vblank_acked = 1;   /* vblank consumed */
    g_irqt.intreq &= d;                                          /* write = ACK */
}
static inline uint32_t irqt_enable_read (void)        { return g_irqt.intena; }
static inline void     irqt_enable_write(uint32_t d)  { g_irqt.intena = d; }

/* Assert a pending bit (from timer expiry / vblank / sound UART). */
static inline void irqt_raise(uint32_t bit) { g_irqt.intreq |= bit; }

/* Live board timers. Off (the default): the run loop takes a whole slice's
 * cycles off the timers when the slice starts, so a timer the i960 reads during
 * the slice has not moved, and one cannot expire before the next slice. On: the
 * timers count down by what each instruction costs as it runs (i960.cycles), a
 * timer interrupt is taken between the instructions where it expires, and a
 * slice that ended at a frame edge adds the rest of its 1/60 s at the next start
 * (the board idling until vsync).
 *
 * Games budget work against these timers. STF's texture loader
 * (unp_send_tex_para_sub) arms timer 4 for what is left of a 500,000-cycle
 * budget since the frame began (check_timer_4_result, off_550008 = 20000 x 25)
 * and yields when its interrupt sets byte_50008C; with the timers frozen for the
 * slice it never yields and decodes to the step limit, ~1M instructions a frame.
 * Where interrupts land moves with this, so it stays off where results are held
 * against a capture until they have been graded with it on. */
static int g_irqt_live = 0;

static inline void irqt__horizon(void) {
    int64_t h = INT64_MAX;
    for (int t = 0; t < IRQT_TIMERS; t++)
        if (g_irqt.timer_run[t] && g_irqt.timer_count[t] < h) h = g_irqt.timer_count[t];
    g_irqt.horizon = h;
}

/* ---- Timing: advance one-shot timers by `cycles` i960 cycles ------------- */

static inline void irqt_tick(int64_t cycles) {
    for (int t = 0; t < IRQT_TIMERS; t++) {
        if (!g_irqt.timer_run[t]) continue;
        g_irqt.timer_count[t] -= cycles;
        if (g_irqt.timer_count[t] <= 0) {
            uint32_t line = 1u << (t + 2);
            if (g_irqt.intena & line) g_irqt.intreq |= line;
            g_irqt.timer_run[t] = false;     /* one-shot; handler re-arms */
        }
    }
    irqt__horizon();
}

/* Bring the counts up to the cycles run so far (live timers). */
static inline void irqt_flush(void) {
    if (g_irqt.pending) {
        int64_t c = g_irqt.pending;
        g_irqt.pending = 0;
        irqt_tick(c);
    }
}

/* ---- Board timer register access (0xF00000 + off), 4 bytes per timer ---- */

static inline void irqt_timer_write(uint32_t off, uint32_t val) {
    int t = (int)((off >> 2) & 3u);
    irqt_flush();
    g_irqt.timer_count[t]  = (int64_t)(val & 0xFFFFFu);
    g_irqt.timer_run[t]    = true;
    irqt__horizon();
}

static inline uint32_t irqt_timer_read(uint32_t off) {
    int t = (int)((off >> 2) & 3u);
    irqt_flush();
    int64_t c = g_irqt.timer_count[t];
    return (uint32_t)(c < 0 ? 0 : c) & 0xFFFFFu;
}

/* ---- Delivery helper: which i960 IRQ pin is pending & enabled? ----------
 * Returns the highest-priority pin index (0..3) with an enabled pending bit,
 * or -1 if none.  Mirrors model2.cpp irq_update bit→pin grouping. */
static inline int irqt_pending_pin(void) {
    uint32_t act = g_irqt.intreq & g_irqt.intena;
    if (!act) return -1;
    if (act & 0x0001u) return 0;   /* VsyncScr */
    if (act & 0x0002u) return 1;   /* VsyncObj */
    if (act & 0x03FCu) return 2;   /* board timers (bits 2..9) */
    if (act & 0x0C00u) return 3;   /* sound UART (bits 10..11) */
    return -1;
}

static inline void irqt_reset(void) {
    for (int i = 0; i < IRQT_TIMERS; i++) {
        g_irqt.timer_count[i] = 0;
        g_irqt.timer_run[i]   = false;
    }
    g_irqt.intreq = 0;
    g_irqt.intena = 0;
    g_irqt.pending = 0;
    g_irqt.horizon = INT64_MAX;
}

#endif /* IRQ_TIMER_H */
