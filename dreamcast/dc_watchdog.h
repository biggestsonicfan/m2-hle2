/* dc_watchdog.h -- a hang stops the Dreamcast with its reason on screen.
 *
 * The SH4's watchdog cannot time a board frame by itself: in watchdog mode it
 * overflows 5.25 ms after the last pet at its slowest clock (KOS's dc/wdt.h
 * gives the overflow period, not a tick), and a frame takes 20-60 ms. #521
 * read 5.25 ms as one of 256 ticks and armed a power-on reset, so on hardware
 * the console reset during the first frame, booted again, and sat on "finding
 * the ROM files" for good (#559). Flycast and redream do not emulate the WDT.
 *
 * So the WDT runs as an interval timer at that clock, an interrupt every
 * 5.25 ms (KOS's wdt_enable_timer takes one every 41 us), and counts: a pet
 * clears the count, and DC_WD_LIMIT interrupts without one hand the interrupted
 * PC and PR to the program's handler, which does not return. */
#ifndef DC_WATCHDOG_H
#define DC_WATCHDOG_H

#include <kos.h>
#include <arch/irq.h>
#include <stdint.h>

#define DC_WD_TICK_US 5250u
#define DC_WD_LIMIT   (2000000u / DC_WD_TICK_US)   /* 2 s without a pet */

/* The SH4's WDT registers: a write carries a key in its high byte. */
#define DC_WD_WTCNT (*(volatile uint16_t *)0xFFC00008)
#define DC_WD_WTCSR (*(volatile uint16_t *)0xFFC0000C)
#define DC_WD_WTCSR_R (*(volatile uint8_t *)0xFFC0000C)
#define DC_WD_CSR(v) ((uint16_t)(0xA500 | ((v) & 0xFF)))
#define DC_WD_TME  0x80u
#define DC_WD_IOVF 0x08u
#define DC_WD_DIV4096 7u

typedef void (*dc_wd_fire_t)(uint32_t pc, uint32_t pr);

static volatile uint32_t g_dc_wd_ticks;
static dc_wd_fire_t g_dc_wd_fire;

static inline void dc_wd_pet(void) { g_dc_wd_ticks = 0; }

static void dc_wd_isr(irq_t src, irq_context_t *cxt, void *data) {
    (void)src; (void)data;
    DC_WD_WTCSR = DC_WD_CSR(DC_WD_WTCSR_R & ~DC_WD_IOVF);
    if (++g_dc_wd_ticks >= DC_WD_LIMIT && g_dc_wd_fire) g_dc_wd_fire(cxt->pc, cxt->pr);
}

static void dc_wd_start(dc_wd_fire_t fire) {
    g_dc_wd_fire = fire;
    g_dc_wd_ticks = 0;
    DC_WD_WTCSR = DC_WD_CSR(DC_WD_DIV4096);   /* stopped, interval mode */
    irq_set_handler(EXC_WDT_ITI, dc_wd_isr, NULL);
    irq_set_priority(IRQ_SRC_WDT, 15);
    DC_WD_WTCNT = (uint16_t)0x5A00;
    DC_WD_WTCSR = DC_WD_CSR(DC_WD_DIV4096 | DC_WD_TME);
}

static void dc_wd_stop(void) {
    DC_WD_WTCSR = DC_WD_CSR(DC_WD_DIV4096);
    irq_set_priority(IRQ_SRC_WDT, 0);
    g_dc_wd_fire = NULL;
}

#endif
