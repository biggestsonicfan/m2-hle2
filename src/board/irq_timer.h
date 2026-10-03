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

#include "attention.h"

#define IRQT_TIMERS 4

/* What a timer that is not counting reads: before its first write and after it
 * expires (MAME model2_timer_cb / machine_reset, m_timervals = 0xfffff). STF's
 * rand (0x66B0) adds all four counts into its state, so a timer the game never
 * arms still feeds every random number with this. It used to read 0. */
#define IRQT_IDLE 0xFFFFFll

typedef struct {
    uint32_t intreq;                 /* pending interrupt bits (0xE80000)   */
    uint32_t intena;                 /* interrupt enable mask  (0xE80004)   */

    int64_t  timer_count[IRQT_TIMERS];   /* current down-count (cycles)     */
    bool     timer_run[IRQT_TIMERS];     /* one-shot armed flag              */

    /* Cycles the i960 has run that the counts do not reflect yet, and the
     * smallest running count, the vblank's included — once pending reaches it
     * an event is due, so the run loop brings the counts up to date then. */
    int64_t  pending;
    int64_t  horizon;

    /* The vblank, on the same clock: where the i960 is in the current video
     * frame, in 1/IRQT_VBLANK_HZ-ths of a cycle (a frame is IRQT_CPU_HZ of
     * them, so 25e6/60 cycles come out exact over any number of frames), and
     * the vblanks since reset. */
    int64_t  vbl_phase;
    uint64_t vbl_count;
} irq_timer_t;

/* The video frame: 60 Hz of the i960's 25 MHz clock, 416,666.67 cycles.
 * MAME's model2 screen is 16 MHz / (656 x 424), 57.52 Hz; the sound board,
 * netplay and every host here run at 60, so the vblank does too. */
#define IRQT_CPU_HZ   25000000
#define IRQT_VBLANK_HZ 60

static irq_timer_t g_irqt = {0};

/* ---- IRQ controller register access (called from memory.h) -------------- */

static inline uint32_t irqt_request_read(void)        { return g_irqt.intreq; }
static inline void     irqt_request_ack (uint32_t d)  { g_irqt.intreq &= d; }   /* write = ACK */
static inline uint32_t irqt_enable_read (void)        { return g_irqt.intena; }
/* A write that enables the sound UART's line is where the board would take its
 * interrupt: TxRDY is already up, so the run loop offers it straight away
 * (emu_thread.h emu_offer_sound) instead of at the next slice. */
static volatile int g_irqt_sound_kick = 0;
static inline void     irqt_enable_write(uint32_t d)  {
    if (d & ~g_irqt.intena & 0x0C00u) { g_irqt_sound_kick = 1; emu_attn_bump(); }
    g_irqt.intena = d;
}

/* Assert a pending bit (from timer expiry / vblank / sound UART). */
static inline void irqt_raise(uint32_t bit) { g_irqt.intreq |= bit; }

/* The board's clock is the i960's: the timers count down by what each
 * instruction costs as it runs (i960.cycles), a timer interrupt is taken
 * between the instructions where it expires, and the vblank comes every
 * 1/60 s of those cycles, whatever the program is doing. The run loop ends a
 * slice on it (emu_thread.h).
 *
 * Games budget work against these timers and wait for the vblank in their own
 * loops. STF's texture loader (unp_send_tex_para_sub) arms timer 4 for what is
 * left of a 500,000-cycle budget since the frame began (check_timer_4_result,
 * off_550008 = 20000 x 25) and yields when its interrupt sets byte_50008C;
 * interrupt_wait_b then spins in _idle until VsyncScr counts the vblank, and a
 * frame that overran one counts a dropped frame (CPU_FAIL). The timers used to
 * be frozen for a slice and the waits replaced by hooks that ran VsyncScr
 * themselves (Pinboard #253, docs/BUBBLEGUM.md §2): the loader never yielded and
 * decoded to the step limit, and no frame was ever late. */

/* Set by irqt_tick when a vblank falls due; the run loop clears it. */
static volatile int g_irqt_vblank = 0;

/* Cycles from now to the next vblank. */
static inline int64_t irqt__vbl_left(void) {
    int64_t per = (int64_t)IRQT_VBLANK_HZ;
    return ((int64_t)IRQT_CPU_HZ - g_irqt.vbl_phase + per - 1) / per;
}

static inline void irqt__horizon(void) {
    int64_t h = irqt__vbl_left();
    for (int t = 0; t < IRQT_TIMERS; t++)
        if (g_irqt.timer_run[t] && g_irqt.timer_count[t] < h) h = g_irqt.timer_count[t];
    g_irqt.horizon = h;
}

/* ---- Timing: advance the board by `cycles` i960 cycles ------------------- */

static inline void irqt_tick(int64_t cycles) {
    for (int t = 0; t < IRQT_TIMERS; t++) {
        if (!g_irqt.timer_run[t]) continue;
        g_irqt.timer_count[t] -= cycles;
        if (g_irqt.timer_count[t] <= 0) {
            uint32_t line = 1u << (t + 2);
            if (g_irqt.intena & line) g_irqt.intreq |= line;
            g_irqt.timer_run[t]   = false;   /* one-shot; handler re-arms */
            g_irqt.timer_count[t] = IRQT_IDLE;
        }
    }
    /* MAME screen_vblank: the line is raised only while it is enabled. No
     * instruction costs more than a frame, so one tick crosses at most one. */
    g_irqt.vbl_phase += cycles * IRQT_VBLANK_HZ;
    if (g_irqt.vbl_phase >= IRQT_CPU_HZ) {
        g_irqt.vbl_phase -= IRQT_CPU_HZ;
        g_irqt.vbl_count++;
        if (g_irqt.intena & 1u) g_irqt.intreq |= 1u;
        g_irqt_vblank = 1;
        emu_attn_bump();
    }
    irqt__horizon();
}

/* Bring the counts up to the cycles run so far. */
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
        g_irqt.timer_count[i] = IRQT_IDLE;
        g_irqt.timer_run[i]   = false;
    }
    g_irqt.intreq = 0;
    g_irqt.intena = 0;
    g_irqt_sound_kick = 0;
    g_irqt.pending = 0;
    g_irqt.vbl_phase = 0;
    g_irqt.vbl_count = 0;
    g_irqt_vblank = 0;
    irqt__horizon();
}

#endif /* IRQ_TIMER_H */
