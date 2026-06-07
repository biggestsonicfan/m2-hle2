/*
 * m68k.h — Motorola MC68000 CPU state for the Model 2 sound processor.
 *
 * Model 2B/2C boards run a 68000 at 12 MHz driving one or two SCSP chips
 * (Saturn Custom Sound Processor).  The i960 communicates with the 68000
 * through SCSP registers; from the i960 side these appear at MIDI_BASE
 * (0x9C0000); from the 68K side they appear at M68K_SCSP_BASE (0x100000).
 *
 * This file defines the CPU state only.  Address-space mapping (ROM, SCSP,
 * wave RAM, work RAM) is provided by sound.h via the read/write callbacks in
 * m68k_state_t.
 */
#ifndef M68K_H
#define M68K_H

#include <stdint.h>
#include <string.h>

/* ---- Status register bit masks ------------------------------------------ */
#define M68K_SR_C       0x0001u   /* carry                                   */
#define M68K_SR_V       0x0002u   /* overflow                                */
#define M68K_SR_Z       0x0004u   /* zero                                    */
#define M68K_SR_N       0x0008u   /* negative                                */
#define M68K_SR_X       0x0010u   /* extend (like carry, but for ADDX/SUBX)  */
#define M68K_SR_CCR     0x001Fu   /* entire CCR mask                         */
#define M68K_SR_IPL     0x0700u   /* interrupt priority level mask           */
#define M68K_SR_M       0x1000u   /* master / interrupt state (68010+)       */
#define M68K_SR_S       0x2000u   /* supervisor mode                         */
#define M68K_SR_T0      0x4000u   /* trace enable T0                         */
#define M68K_SR_T1      0x8000u   /* trace enable T1                         */

#define M68K_SR_IPL_SHIFT 8

/* ---- CPU state ----------------------------------------------------------- */

typedef struct {
    uint32_t d[8];     /* D0–D7  data registers                              */
    uint32_t a[8];     /* A0–A7  address registers (A7 = active stack ptr)   */
    uint32_t pc;       /* program counter (24-bit effective on 68000)        */
    uint16_t sr;       /* status register                                    */
    uint32_t usp;      /* user stack pointer  (banked; a[7] when S=0)        */
    uint32_t ssp;      /* supervisor stack ptr (banked; a[7] when S=1)       */
    int      halted;   /* HALT state (unrecoverable bus/address error)       */
    int      stopped;  /* waiting for interrupt after STOP instruction       */
    uint64_t cycles;   /* total cycles executed (approximate)                */
} m68k_cpu_t;

/* ---- Memory callbacks ---------------------------------------------------- */

typedef uint32_t (*m68k_read_fn) (void *ctx, uint32_t addr, int sz);
typedef void     (*m68k_write_fn)(void *ctx, uint32_t addr, uint32_t val, int sz);

typedef struct {
    m68k_cpu_t    cpu;
    m68k_read_fn  read_cb;
    m68k_write_fn write_cb;
    void         *mem_ctx;
} m68k_state_t;

/* ---- Helpers ------------------------------------------------------------- */

static inline int m68k_is_supervisor(const m68k_cpu_t *c) {
    return (c->sr & M68K_SR_S) != 0;
}
static inline int m68k_ipl(const m68k_cpu_t *c) {
    return (c->sr >> M68K_SR_IPL_SHIFT) & 7;
}

/*
 * Initialise CPU state.  PC and SSP are loaded from the reset vectors by
 * m68k_startup() (in m68k_exec.h) once ROM is mapped.
 */
static inline void m68k_reset(m68k_state_t *s) {
    memset(&s->cpu, 0, sizeof(m68k_cpu_t));
    /* start in supervisor mode, all interrupts masked */
    s->cpu.sr = M68K_SR_S | (7u << M68K_SR_IPL_SHIFT);
}

#endif /* M68K_H */
