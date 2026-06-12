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
    uint32_t timer_reload[IRQT_TIMERS];  /* last programmed reload (20-bit)  */
    bool     timer_run[IRQT_TIMERS];     /* one-shot armed flag              */

    /* diagnostics */
    uint64_t deliver_count;          /* interrupts vectored to a handler    */
    uint32_t deliver_by_pin[4];
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

/* timers→interrupts rework, additive step: when nonzero, the run loop ticks the
 * board timers so the (already-enabled) timer IRQ fires and its ISR runs, ON TOP
 * of the existing HLE bypasses (no bypass removed → no idle-loop hang).  DEFAULT
 * ON: the timer ISR drives the real attract-mode progression (our bypass froze
 * it).  Toggle off via --realirq=0 path / UI if needed. */
static int g_real_irq = 1;

/* ---- Board timer register access (0xF00000 + off), 4 bytes per timer ---- */

static inline void irqt_timer_write(uint32_t off, uint32_t val) {
    int t = (int)((off >> 2) & 3u);
    g_irqt.timer_reload[t] = val & 0xFFFFFu;
    g_irqt.timer_count[t]  = (int64_t)(val & 0xFFFFFu);
    g_irqt.timer_run[t]    = true;
}

static inline uint32_t irqt_timer_read(uint32_t off) {
    int t = (int)((off >> 2) & 3u);
    int64_t c = g_irqt.timer_count[t];
    return (uint32_t)(c < 0 ? 0 : c) & 0xFFFFFu;
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
    uint64_t dc = g_irqt.deliver_count;
    /* preserve nothing; full reset */
    (void)dc;
    for (int i = 0; i < IRQT_TIMERS; i++) {
        g_irqt.timer_count[i]  = 0;
        g_irqt.timer_reload[i] = 0;
        g_irqt.timer_run[i]    = false;
    }
    g_irqt.intreq = 0;
    g_irqt.intena = 0;
    g_irqt.deliver_count = 0;
    g_irqt.deliver_by_pin[0]=g_irqt.deliver_by_pin[1]=
    g_irqt.deliver_by_pin[2]=g_irqt.deliver_by_pin[3]=0;
}

#endif /* IRQ_TIMER_H */
