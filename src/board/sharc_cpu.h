/*
 * sharc_cpu.h — an ADSP-21062 SHARC interpreter (board-level, Model 2B COP).
 *
 * Model 2B puts an Analog Devices SHARC where Model 2A has the TGP (tgp.h).
 * Games that upload their own coprocessor program (Virtual-On: 11038 halfwords
 * at 0x20000) need it run as a CPU, not answered per command: this module is
 * that CPU. It knows nothing of the board; the board wires it in through the
 * external DM hooks and the flag inputs below.
 *
 * The instruction set, register file, condition codes, loop and PC stacks,
 * MODE1 write latency and every compute operation follow MAME's interpreter
 * (devices/cpu/sharc/{sharc.cpp, sharcops.hxx, compute.hxx, sharcinternal.ipp},
 * BSD-3-Clause, Ville Linde; mame/sega/model2.cpp for the board side), which is
 * the oracle. MAME runs the Model 2B copro on its DRC; this is the interpreter,
 * so where the two disagree this follows the interpreter. Quirks kept on
 * purpose, because MAME computes them that way:
 *   - the "unsigned" operands of the fixed-point multiplier (compute_mul_fixed)
 *     are sign-extended (REG is int32_t in MAME);
 *   - FADD/FSUB pairs of infinities store the integer pattern converted to
 *     float, (float)(sign | 0x7F7FFFFF or 0x7F800000), not the pattern itself;
 *   - Rn = Rn OR LSHIFT Rx BY Ry shifts right arithmetically (the immediate
 *     form logically); BIT TST ASTAT loses the BTF it sets (SET_UREG after);
 *   - MODE1.TRUNC only reaches FIX (floorf; the multifunction FIX BY truncates
 *     toward zero): float add, subtract, multiply and FLOAT round to nearest,
 *     as host floats do. FMUL neither flushes denormals nor saturates: an
 *     overflow is infinity, a denormal product stays (MU). RSQRTS of +-0
 *     seeds +-2^63 from the table. Pinboard #949 replayed 4200 frames of
 *     Virtual-On's COP traffic from MAME (tests/sharc_replay.c) to every
 *     output word identical.
 * MAME's fatal errors (unknown registers, stack over/underflow, unimplemented
 * opcodes) log a WARN and set `fault`; shc_run stops with SHC_STOP_FAULT.
 * Not implemented: DMA transfers (a start logs a WARN), bit-reverse, the
 * timer, multiprocessor/link ports. The ALU/multiplier arithmetic is done on
 * IEEE single bits with memcpy, never FMA (the build has -ffp-contract=off),
 * so native, wasm and ARM compute the same words.
 *
 * Memory (the 2-Mbit part, as MAME's pgm_2m/data_2m):
 *   PM 0x20000-0x24FFF block 0; block 1 mirrored at 0x28000/0x30000/0x38000.
 *   DM 0x00000-0x000FF IOP registers; 0x20000-0x27FFF block 0, block 1 at the
 *      0x28000/0x30000/0x38000 mirrors; 0x40000-0x4FFFF short words of block
 *      0, block 1 at 0x50000/0x60000/0x70000 (sign-extended when MODE1.SSE).
 *   DM >= 0x80000 goes to shc_ext_rd/shc_ext_wr. Model 2B maps there:
 *      0x0400000-0x0BFFFFF FIFO in (read), 0x0C00000-0x13FFFFF FIFO out
 *      (write), 0x1400000-0x1BFFFFF buffer RAM (offset & 0x7FFF), and
 *      0x1C00000-0x1DFFFFF the copro_data ROM.
 *   FLAG0 = input FIFO empty, FLAG1 = output FIFO full (model2.cpp sets both
 *   from the FIFOs' callbacks: call shc_set_flag_input whenever they change,
 *   from inside the hooks too).
 *
 * Boot (model2.cpp copro_fifo_w / copro_boot): shc_reset() holds the core
 * halted, then each upload word goes to shc_boot_write(n++, word & 0xFFFF)
 * (MAME's external_dma_write: HOST boot packs three halfwords into each PM
 * word, low first), then shc_boot() releases it at 0x20005.
 *
 * Timing: synchronous. shc_run(max) executes until max cycles (one per
 * instruction), or the core blocks: an external access the board stalls
 * (the instruction is rolled back and retried by the next run), IDLE, or a
 * busy-wait. A busy-wait is an instruction that leaves the core exactly as it
 * found it: a taken jump to itself with nothing else to do (`if flag0_in jump
 * (pc,0)`) or a one-instruction DO UNTIL loop over a NOP whose condition is
 * still false (`do (pc,1) until not flag0_in`). Only a flag, an interrupt or
 * memory the board changes can end one, so the board has to act first.
 *
 * Savestates: g_shc is plain data (no pointers); the hooks live outside it.
 */
#ifndef SHARC_CPU_H
#define SHARC_CPU_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "log.h"

/* ---- Bits (MAME sharc.h) --------------------------------------------------- */

enum {
    SHC_AZ = 0x1, SHC_AV = 0x2, SHC_AN = 0x4, SHC_AC = 0x8, SHC_AS = 0x10,
    SHC_AI = 0x20, SHC_MN = 0x40, SHC_MV = 0x80, SHC_MU = 0x100,
    SHC_MI = 0x200, SHC_AF = 0x400, SHC_SV = 0x800, SHC_SZ = 0x1000,
    SHC_SS = 0x2000, SHC_BTF = 0x40000,
    SHC_FLG0 = 0x80000, SHC_FLGS = 0x780000,     /* FLG0..FLG3, bits 19-22 */
};
#define SHC_FLG_SHIFT 19

enum {
    SHC_AUS = 0x1, SHC_AVS = 0x2, SHC_AOS = 0x4, SHC_AIS = 0x20,
    SHC_MOS = 0x40, SHC_MVS = 0x80, SHC_MUS = 0x100, SHC_MIS = 0x200,
    SHC_PCFL = 0x200000, SHC_PCEM = 0x400000, SHC_SSOV = 0x800000,
    SHC_SSEM = 0x1000000, SHC_LSOV = 0x2000000, SHC_LSEM = 0x4000000,
};

enum {
    SHC_M1_BR8 = 0x1, SHC_M1_BR0 = 0x2, SHC_M1_SRCU = 0x4,
    SHC_M1_SRD1H = 0x8, SHC_M1_SRD1L = 0x10, SHC_M1_SRD2H = 0x20,
    SHC_M1_SRD2L = 0x40, SHC_M1_SRRFH = 0x80, SHC_M1_SRRFL = 0x400,
    SHC_M1_IRPTEN = 0x1000, SHC_M1_ALUSAT = 0x2000, SHC_M1_SSE = 0x4000,
    SHC_M1_TRUNC = 0x8000,
};
#define SHC_M2_FLGO_SHIFT 15    /* MODE2 FLGnO: 1 = output */

#define SHC_F_SIGN  0x80000000u
#define SHC_F_EXP   0x7f800000u
#define SHC_F_MANT  0x007fffffu
#define SHC_F_INF   0x7f800000u
#define SHC_F_NAN   0xffffffffu
#define SHC_F_MAXT  0x7f7fffffu     /* TRUNC saturates overflow to +-max */

#define SHC_EXT_BASE  0x80000u      /* DM at or above goes to the board */
#define SHC_PCSTACK   30
#define SHC_LSTACK    6

/* ---- Stop reasons ---------------------------------------------------------- */

enum {
    SHC_STOP_BUDGET = 0,    /* ran max_cycles */
    SHC_STOP_STALL,         /* the board stalled an external access */
    SHC_STOP_IDLE,          /* IDLE, waiting for an interrupt */
    SHC_STOP_SPIN,          /* busy-waiting on a flag (see the header) */
    SHC_STOP_HALTED,        /* held: before shc_boot */
    SHC_STOP_FAULT,         /* MAME would have raised a fatal error */
};

/* ---- State ----------------------------------------------------------------- */

typedef struct { uint32_t i[8], m[8], b[8], l[8]; } shc_dag_t;
typedef struct { uint32_t addr, code, type; } shc_laddr_t;
typedef struct {
    uint32_t control, int_index, int_modifier, int_count, chain_ptr;
    uint32_t gen_purpose, ext_index, ext_modifier, ext_count;
} shc_dma_t;

/* PC and loop stacks: one struct so a stalled instruction can put them back */
typedef struct {
    uint32_t pcstk, pcstkp, pcstack[SHC_PCSTACK];
    uint32_t lcntr, curlcntr, lstkp, lcstack[SHC_LSTACK], lastack[SHC_LSTACK];
    shc_laddr_t laddr;
    uint32_t stky;
} shc_stacks_t;

typedef struct {
    uint32_t r[16], reg_alt[16];
    shc_dag_t dag1, dag2, dag1_alt, dag2_alt;     /* dag1 = DM (I0-7), dag2 = PM */
    uint64_t mrf, mrb, px;

    uint32_t pc, daddr, faddr, nfaddr, delay_slot1, delay_slot2;
    uint32_t astat, astat_old, astat_old_old, astat_old_old_old;
    uint32_t mode1, mode2, irptl, imask, ustat1, ustat2;
    shc_stacks_t s;
    struct { uint32_t mode1, astat; } status_stack[5];
    int32_t status_stkp;

    int32_t irq_pending, active_irq, interrupt_active, idle;
    int32_t lat_cycles, lat_reg;                /* MODE1 write latency */
    uint32_t lat_data, lat_prev;
    uint32_t sse_view;                          /* MAME's m_dm_short_view */

    uint32_t flag[4];
    uint32_t syscon, sysstat, msgr[8], dma_status, extdma_shift;
    shc_dma_t dma[8];

    uint32_t ext_latch;     /* the external read done before compute */
    uint32_t stalled;       /* this instruction's external access stalled */
    uint32_t spin;          /* this instruction busy-waits (see Timing) */
    uint32_t halted, fault, stop;
    int32_t cycles;         /* used by the last shc_run */
    uint64_t opcode;

    uint32_t blk[2][0x8000];
} shc_state_t;

static shc_state_t g_shc;

/* ---- Board hooks (outside the state; default: nothing there) -------------- */

static uint32_t shc_ext_rd_none(uint32_t addr, bool *stall)
{
    (void)addr; (void)stall;
    return 0;
}
static void shc_ext_wr_none(uint32_t addr, uint32_t v, bool *stall)
{
    (void)addr; (void)v; (void)stall;
}
static void shc_flag_out_none(int n, int state) { (void)n; (void)state; }

/* A hook that sets *stall must not have done the access: it is retried. */
static uint32_t (*shc_ext_rd)(uint32_t addr, bool *stall) = shc_ext_rd_none;
static void (*shc_ext_wr)(uint32_t addr, uint32_t v, bool *stall) = shc_ext_wr_none;
static void (*shc_flag_out)(int n, int state) = shc_flag_out_none;

/* ---- Small helpers --------------------------------------------------------- */

#define SHC_R(x)  (g_shc.r[(x)])

static void shc_fatal(const char *what, uint32_t v)
{
    if (!g_shc.fault)
        LOG_WARN("sharc: %s (%08X) at PC %05X, op %012llX", what, (unsigned)v,
                 (unsigned)g_shc.pc, (unsigned long long)g_shc.opcode);
    g_shc.fault = 1;
}

static inline float shc_u2f(uint32_t u)
{
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

/* host float -> word; any host NaN becomes the canonical 0xFFFFFFFF */
static inline uint32_t shc_f2u(float f)
{
    uint32_t u;
    if (f != f)
        return SHC_F_NAN;
    memcpy(&u, &f, sizeof u);
    return u;
}

static inline uint32_t shc_sext(uint32_t v, int n)
{
    if (n >= 32)
        return v;
    uint32_t const m = 1u << (n - 1);
    v &= (1u << n) - 1u;
    return (v ^ m) - m;
}

static inline uint32_t shc_mask(int n) { return n >= 32 ? 0xffffffffu : (1u << n) - 1u; }

/* std::rotl(uint32_t, int): a negative count rotates right */
static inline uint32_t shc_rotl(uint32_t x, int s)
{
    int const r = s % 32;
    if (r == 0)
        return x;
    if (r > 0)
        return (x << r) | (x >> (32 - r));
    return (x >> -r) | (x << (32 + r));
}

static inline uint32_t shc_asr(uint32_t x, int n)    /* int32 >> n, 0 <= n < 32 */
{
    return (x & SHC_F_SIGN) ? ~(~x >> n) : (x >> n);
}

/* ---- Memory ---------------------------------------------------------------- */

/* PM word at a: block and the index of its first 32-bit cell (MAME pm_r) */
static inline uint32_t *shc_pm_cell(uint32_t a, uint32_t *slot)
{
    a &= 0xffffff;
    if (a < 0x20000 || a >= 0x40000 || (a & 0x7fff) >= 0x5000)
        return NULL;
    uint32_t const off = a & 0x7fff;
    *slot = off >> 12;
    return &g_shc.blk[(a & 0x18000) ? 1 : 0][(off & 0xfff) + (*slot >> 1) * (3u << 12)];
}

static uint64_t shc_pm_rd48(uint32_t a)
{
    uint32_t slot;
    uint32_t const *b = shc_pm_cell(a, &slot);
    if (!b)
        return 0;
    if (slot & 1)
        return ((uint64_t)b[0x2000] << 16) | (b[0x1000] & 0xffff);
    return ((uint64_t)b[0] << 16) | (b[0x1000] >> 16);
}

static void shc_pm_wr48(uint32_t a, uint64_t v)
{
    uint32_t slot;
    uint32_t *b = shc_pm_cell(a, &slot);
    if (!b)
        return;
    if (slot & 1) {
        b[0x1000] = (b[0x1000] & 0xffff0000u) | (uint32_t)(v & 0xffff);
        b[0x2000] = (uint32_t)(v >> 16);
    } else {
        b[0] = (uint32_t)(v >> 16);
        b[0x1000] = (b[0x1000] & 0xffff) | ((uint32_t)(v & 0xffff) << 16);
    }
}

static inline uint32_t shc_pm_rd32(uint32_t a) { return (uint32_t)(shc_pm_rd48(a) >> 16); }

static void shc_pm_wr32(uint32_t a, uint32_t v)   /* bits 16-47 only */
{
    uint32_t slot;
    uint32_t *b = shc_pm_cell(a, &slot);
    if (b)
        b[(slot & 1) ? 0x2000 : 0] = v;
}

static void shc_iop_w(uint32_t reg, uint32_t v);

static uint32_t shc_iop_r(uint32_t reg)
{
    if (reg >= 0x08 && reg <= 0x0f)
        return g_shc.msgr[reg - 8];
    if (reg == 0x37)
        return g_shc.dma_status;
    if (reg > 0x01)
        LOG_WARN("sharc: IOP read %02X at %05X -> 0", (unsigned)reg, (unsigned)g_shc.pc);
    return 0;
}

static uint32_t shc_short_rd(uint32_t a)
{
    uint32_t const h = a & 0xffff;
    uint32_t const w = g_shc.blk[(a & 0x30000) ? 1 : 0][h >> 1];
    uint32_t const v = (h & 1) ? (w >> 16) : (w & 0xffff);
    return g_shc.sse_view ? shc_sext(v, 16) : v;
}

static void shc_short_wr(uint32_t a, uint32_t v)
{
    uint32_t const h = a & 0xffff;
    uint32_t *w = &g_shc.blk[(a & 0x30000) ? 1 : 0][h >> 1];
    if (h & 1)
        *w = (*w & 0xffff) | (v << 16);
    else
        *w = (*w & 0xffff0000u) | (v & 0xffff);
}

/* Internal DM, or the word an external read latched before compute. */
static uint32_t shc_dm_rd(uint32_t a)
{
    if (a >= SHC_EXT_BASE)
        return g_shc.ext_latch;
    if (a >= 0x40000)
        return shc_short_rd(a);
    if (a >= 0x20000)
        return g_shc.blk[(a & 0x18000) ? 1 : 0][a & 0x7fff];
    return a < 0x100 ? shc_iop_r(a) : 0;
}

/* Internal DM; an external write was already done before compute. */
static void shc_dm_wr(uint32_t a, uint32_t v)
{
    if (a >= SHC_EXT_BASE)
        return;
    if (a >= 0x40000)
        shc_short_wr(a, v);
    else if (a >= 0x20000)
        g_shc.blk[(a & 0x18000) ? 1 : 0][a & 0x7fff] = v;
    else if (a < 0x100)
        shc_iop_w(a, v);
}

/* The external half of a DM access, done before any of the instruction's
 * side effects. False when the board stalled it: the handler returns at once
 * and shc_run rolls the instruction back. */
static bool shc_ext(uint32_t a, bool wr, uint32_t v)
{
    bool stall = false;
    if (a < SHC_EXT_BASE)
        return true;
    if (wr)
        shc_ext_wr(a, v, &stall);
    else
        g_shc.ext_latch = shc_ext_rd(a, &stall);
    if (stall)
        g_shc.stalled = 1;
    return !stall;
}

/* MAME external_dma_write (DMA channel 6 as the HOST boot sets it up) */
static void shc_dma6_write(uint32_t n, uint32_t data)
{
    shc_dma_t *d = &g_shc.dma[6];
    uint32_t const index = (d->int_index & 0x1ffff) | 0x20000;
    uint32_t const mswf = (d->control >> 8) & 1;
    uint32_t const pmode = (d->control >> 6) & 3;
    if (pmode == 0) {
        if ((d->control >> 5) & 1)
            shc_pm_wr32(index, data);
        else
            shc_dm_wr(index, data);
        d->int_index += d->int_modifier;
    } else if (pmode == 2) {
        uint32_t const word = n % 3;
        uint32_t const shift = (mswf ? (2 - word) : word) * 16;
        uint64_t r = shc_pm_rd48(index);
        r &= ~((uint64_t)0xffff << shift);
        r |= (uint64_t)(data & 0xffff) << shift;
        shc_pm_wr48(index, r);
        if (word == 2)
            d->int_index += d->int_modifier;
    } else {
        shc_fatal("external DMA packing mode", pmode);
    }
}

static void shc_dma_reg_w(int ch, uint32_t reg, uint32_t v)
{
    uint32_t *f[8] = {
        &g_shc.dma[ch].int_index, &g_shc.dma[ch].int_modifier,
        &g_shc.dma[ch].int_count, &g_shc.dma[ch].chain_ptr,
        &g_shc.dma[ch].gen_purpose, &g_shc.dma[ch].ext_index,
        &g_shc.dma[ch].ext_modifier, &g_shc.dma[ch].ext_count,
    };
    *f[reg & 7] = v;
}

/* Writes DMA6/7's control register (IOP 0x1C, 0x1D); a start is not emulated. */
static inline void shc_dma_control_w(uint32_t reg, uint32_t v)
{
        g_shc.dma[reg - 0x16].control = v;
        if (v & 1)
            LOG_WARN("sharc: DMA%u start (%08X) not emulated", (unsigned)(reg - 0x16), (unsigned)v);
}

static void shc_iop_w(uint32_t reg, uint32_t v)
{
    if (reg == 0x00) {
        g_shc.syscon = v;
    } else if (reg == 0x04) {
        shc_dma6_write(g_shc.extdma_shift, v);
        g_shc.extdma_shift = (g_shc.extdma_shift + 1) % 3;
    } else if (reg >= 0x08 && reg <= 0x0f) {
        g_shc.msgr[reg - 8] = v;
    } else if (reg == 0x1c || reg == 0x1d) {
        shc_dma_control_w(reg, v);
    } else if (reg >= 0x40 && reg <= 0x4f) {
        shc_dma_reg_w(reg < 0x48 ? 6 : 7, reg, v);
    } else if (reg > 0x03 && reg != 0x14 && reg != 0x17 && reg != 0x20) {
        LOG_WARN("sharc: IOP write %02X = %08X at %05X ignored", (unsigned)reg,
                 (unsigned)v, (unsigned)g_shc.pc);
    }
}

/* ---- Pipeline, stacks ------------------------------------------------------ */

static inline void shc_change_pc(uint32_t n)
{
    g_shc.pc = n;
    g_shc.daddr = n;
    g_shc.faddr = n + 1;
    g_shc.nfaddr = n + 2;
}

static inline void shc_change_pc_delayed(uint32_t n)
{
    g_shc.nfaddr = n;
    g_shc.delay_slot1 = g_shc.pc;
    g_shc.delay_slot2 = g_shc.daddr;
}

static inline void shc_jump(bool delayed, uint32_t n)
{
    if (delayed)
        shc_change_pc_delayed(n);
    else
        shc_change_pc(n);
}

static void shc_push_pc(void)
{
    shc_stacks_t *s = &g_shc.s;
    if (s->pcstkp >= SHC_PCSTACK) {
        shc_fatal("PC stack overflow", s->pcstkp);
        return;
    }
    if (s->pcstkp > 0)
        s->pcstack[s->pcstkp - 1] = s->pcstk;
    s->pcstkp++;
    s->stky &= ~SHC_PCEM;
    if (s->pcstkp >= SHC_PCSTACK)
        s->stky |= SHC_PCFL;
}

static uint32_t shc_pop_pc(void)
{
    shc_stacks_t *s = &g_shc.s;
    if (s->pcstkp == 0) {
        shc_fatal("PC stack underflow", 0);
        return g_shc.pc;
    }
    uint32_t const result = s->pcstk;
    s->pcstkp--;
    s->pcstack[s->pcstkp] = s->pcstk;
    if (s->pcstkp > 0) {
        s->pcstk = s->pcstack[s->pcstkp - 1];
    } else {
        s->pcstk = 0x00ffffff;
        s->stky |= SHC_PCEM;
    }
    s->stky &= ~SHC_PCFL;
    return result;
}

static inline uint32_t shc_laddr_pack(const shc_laddr_t *l)
{
    return (l->type << 30) | (l->code << 24) | l->addr;
}

static inline void shc_laddr_unpack(shc_laddr_t *l, uint32_t v)
{
    l->addr = v & 0xffffff;
    l->code = (v >> 24) & 0x1f;
    l->type = (v >> 30) & 3;
}

static void shc_push_loop(void)
{
    shc_stacks_t *s = &g_shc.s;
    if (s->lstkp >= SHC_LSTACK) {
        shc_fatal("loop stack overflow", s->lstkp);
        return;
    }
    if (s->lstkp > 0) {
        s->lcstack[s->lstkp - 1] = s->curlcntr;
        s->lastack[s->lstkp - 1] = shc_laddr_pack(&s->laddr);
    }
    s->curlcntr = s->lcntr;
    shc_laddr_unpack(&s->laddr, s->lastack[s->lstkp]);
    s->lstkp++;
    s->lcntr = s->lstkp < SHC_LSTACK ? s->lcstack[s->lstkp] : 0xffffffffu;
    s->stky &= ~SHC_LSEM;
}

static void shc_pop_loop(void)
{
    shc_stacks_t *s = &g_shc.s;
    if (s->lstkp == 0) {
        shc_fatal("loop stack underflow", 0);
        return;
    }
    s->lstkp--;
    s->lcntr = s->curlcntr;
    s->lastack[s->lstkp] = shc_laddr_pack(&s->laddr);
    if (s->lstkp > 0) {
        s->curlcntr = s->lcstack[s->lstkp - 1];
        shc_laddr_unpack(&s->laddr, s->lastack[s->lstkp - 1]);
    } else {
        s->curlcntr = 0xffffffffu;
        shc_laddr_unpack(&s->laddr, 0xffffffffu);
        s->stky |= SHC_LSEM;
    }
}

/* ---- Universal registers --------------------------------------------------- */

static uint32_t shc_get_ureg(int ureg);
static void shc_set_ureg(int ureg, uint32_t v);
static void shc_check_interrupts(void);

static void shc_push_status(void)
{
    g_shc.status_stkp++;
    if (g_shc.status_stkp >= 5) {
        shc_fatal("status stack overflow", (uint32_t)g_shc.status_stkp);
        return;
    }
    g_shc.status_stack[g_shc.status_stkp - 1].mode1 = shc_get_ureg(0x7b);
    g_shc.status_stack[g_shc.status_stkp - 1].astat = shc_get_ureg(0x7c);
    g_shc.s.stky &= ~SHC_SSEM;
}

static void shc_pop_status(void)
{
    if (g_shc.status_stkp <= 0) {
        shc_fatal("status stack underflow", 0);
        return;
    }
    g_shc.status_stkp--;
    shc_set_ureg(0x7b, g_shc.status_stack[g_shc.status_stkp].mode1);
    shc_set_ureg(0x7c, (g_shc.astat & SHC_FLGS) |
                       (g_shc.status_stack[g_shc.status_stkp].astat & ~SHC_FLGS));
    if (g_shc.status_stkp == 0)
        g_shc.s.stky |= SHC_SSEM;
}

static void shc_swap_dag_half(shc_dag_t *a, shc_dag_t *b, int base)
{
    for (int k = base; k < base + 4; k++) {
        uint32_t t;
        t = a->i[k]; a->i[k] = b->i[k]; b->i[k] = t;
        t = a->m[k]; a->m[k] = b->m[k]; b->m[k] = t;
        t = a->l[k]; a->l[k] = b->l[k]; b->l[k] = t;
        t = a->b[k]; a->b[k] = b->b[k]; b->b[k] = t;
    }
}

static void shc_swap_regs(int base)
{
    for (int k = base; k < base + 8; k++) {
        uint32_t const t = g_shc.r[k];
        g_shc.r[k] = g_shc.reg_alt[k];
        g_shc.reg_alt[k] = t;
    }
}

/* MAME systemreg_write_latency_effect: MODE1 two instructions after its write */
static void shc_latency_effect(void)
{
    uint32_t const data = g_shc.lat_data;
    uint32_t const diff = data ^ g_shc.lat_prev;
    if (g_shc.lat_reg != 0xb) {
        shc_fatal("latency effect on unknown register", (uint32_t)g_shc.lat_reg);
        return;
    }
    g_shc.mode1 = data;
    if (diff & (SHC_M1_BR8 | SHC_M1_BR0 | SHC_M1_SRCU))
        shc_fatal("MODE1 bit-reverse / MR alternate", data);
    if (diff & SHC_M1_SRD1H) shc_swap_dag_half(&g_shc.dag1, &g_shc.dag1_alt, 4);
    if (diff & SHC_M1_SRD1L) shc_swap_dag_half(&g_shc.dag1, &g_shc.dag1_alt, 0);
    if (diff & SHC_M1_SRD2H) shc_swap_dag_half(&g_shc.dag2, &g_shc.dag2_alt, 4);
    if (diff & SHC_M1_SRD2L) shc_swap_dag_half(&g_shc.dag2, &g_shc.dag2_alt, 0);
    if (diff & SHC_M1_SRRFH) shc_swap_regs(8);
    if (diff & SHC_M1_SRRFL) shc_swap_regs(0);
    if (diff & SHC_M1_SSE)
        g_shc.sse_view = (data & SHC_M1_SSE) ? 1 : 0;
    g_shc.lat_reg = -1;
}

static void shc_add_latency(int reg, uint32_t data, uint32_t prev)
{
    if (g_shc.lat_cycles > 0)
        shc_latency_effect();
    g_shc.lat_cycles = 2;
    g_shc.lat_reg = reg;
    g_shc.lat_data = data;
    g_shc.lat_prev = prev;
}

static uint32_t shc_get_astat(void)
{
    uint32_t r = g_shc.astat;
    uint32_t const outs = (g_shc.mode2 >> SHC_M2_FLGO_SHIFT) & 0xf;
    r &= (outs << SHC_FLG_SHIFT) | ~SHC_FLGS;
    for (int i = 0; i < 4; i++)
        if (!((outs >> i) & 1))
            r |= g_shc.flag[i] << (SHC_FLG_SHIFT + i);
    return r;
}

static uint32_t shc_get_sysreg(int ureg)
{
    switch (ureg & 0xf) {
    case 0x0: return g_shc.ustat1;
    case 0x1: return g_shc.ustat2;
    case 0x9: return g_shc.irptl;
    case 0xa: return g_shc.mode2;
    case 0xb: return g_shc.mode1;
    case 0xc: return shc_get_astat();
    case 0xd: return g_shc.imask;
    case 0xe: return g_shc.s.stky;
    default:
        shc_fatal("GET_UREG unknown register", (uint32_t)ureg);
        return 0;
    }
}

static uint32_t shc_get_ureg6(int ureg)
{
    switch (ureg & 0xf) {
    case 0x4: return g_shc.s.pcstk;
    case 0x5: return g_shc.s.pcstkp;
    case 0x7: return g_shc.s.curlcntr;
    case 0x8: return g_shc.s.lcntr;
    default:
        shc_fatal("GET_UREG unknown register", (uint32_t)ureg);
        return 0;
    }
}

static uint32_t shc_get_px(int ureg)
{
    switch (ureg & 0xf) {
    case 0xb: return (uint32_t)g_shc.px;
    case 0xc: return (uint16_t)g_shc.px;
    case 0xd: return (uint32_t)(g_shc.px >> 16);
    default:
        shc_fatal("GET_UREG unknown register", (uint32_t)ureg);
        return 0;
    }
}

static uint32_t shc_get_ureg(int ureg)
{
    int const reg = ureg & 0xf;
    shc_dag_t const *d = (reg & 8) ? &g_shc.dag2 : &g_shc.dag1;
    switch ((ureg >> 4) & 0xf) {
    case 0x0: return g_shc.r[reg];
    case 0x1: return d->i[reg & 7];
    case 0x2:
        /* MAME: M8-M15 read sign-extended from bit 23, M0-M7 as stored */
        if ((reg & 8) && (d->m[reg & 7] & 0x800000))
            return d->m[reg & 7] | 0xff000000u;
        return d->m[reg & 7];
    case 0x3: return d->l[reg & 7];
    case 0x4: return d->b[reg & 7];
    case 0x6: return shc_get_ureg6(ureg);
    case 0x7: return shc_get_sysreg(ureg);
    case 0xd: return shc_get_px(ureg);
    default:
        shc_fatal("GET_UREG unknown register", (uint32_t)ureg);
        return 0;
    }
}

static void shc_set_pcstkp(uint32_t data)
{
    shc_stacks_t *s = &g_shc.s;
    if (s->pcstkp >= 31)
        return;
    uint32_t const prev = s->pcstkp;
    s->pcstkp = data & 0x1f;
    if (prev > 0)
        s->pcstack[prev - 1] = s->pcstk;
    if (prev < s->pcstkp) {
        if (s->pcstkp >= 31) {
            shc_fatal("PC stack overflow", s->pcstkp);
            return;
        }
        s->pcstk = s->pcstack[s->pcstkp - 1];
        s->stky &= ~SHC_PCEM;
        if (s->pcstkp >= SHC_PCSTACK)
            s->stky |= SHC_PCFL;
    } else if (prev > s->pcstkp) {
        if (s->pcstkp > 0) {
            s->pcstk = s->pcstack[s->pcstkp - 1];
        } else {
            s->pcstk = 0x00ffffff;
            s->stky |= SHC_PCEM;
        }
        s->stky &= ~SHC_PCFL;
    }
}

static void shc_set_ureg6(int ureg, uint32_t data)
{
    shc_stacks_t *s = &g_shc.s;
    switch (ureg & 0xf) {
    case 0x4:
        if (s->pcstkp > 0 && s->pcstkp < 31)
            s->pcstk = data & 0x00ffffff;
        break;
    case 0x5: shc_set_pcstkp(data); break;
    case 0x7:
        if (s->lstkp > 0 && s->lstkp < 7)
            s->curlcntr = data;
        break;
    case 0x8:
        if (s->lstkp < SHC_LSTACK)
            s->lcntr = data;
        break;
    default: shc_fatal("SET_UREG unknown register", (uint32_t)ureg); break;
    }
}

static void shc_set_mode2(uint32_t data)
{
    uint32_t const set = ~g_shc.mode2 & data;
    g_shc.mode2 = data;
    for (int i = 0; i < 4; i++)
        if ((set >> (SHC_M2_FLGO_SHIFT + i)) & 1)
            shc_flag_out(i, (int)((g_shc.astat >> (SHC_FLG_SHIFT + i)) & 1));
}

static void shc_set_astat(uint32_t data)
{
    uint32_t const outs = ((g_shc.mode2 >> SHC_M2_FLGO_SHIFT) & 0xf) << SHC_FLG_SHIFT;
    uint32_t const flags = (g_shc.astat ^ data) & outs;
    g_shc.astat = data;
    for (int i = 0; i < 4; i++)
        if ((flags >> (SHC_FLG_SHIFT + i)) & 1)
            shc_flag_out(i, (int)((data >> (SHC_FLG_SHIFT + i)) & 1));
}

static void shc_set_sysreg(int ureg, uint32_t data)
{
    uint32_t const keep = SHC_LSEM | SHC_LSOV | SHC_SSEM | SHC_SSOV | SHC_PCEM | SHC_PCFL;
    switch (ureg & 0xf) {
    case 0x0: g_shc.ustat1 = data; break;
    case 0x1: g_shc.ustat2 = data; break;
    case 0x9: g_shc.irptl = data; break;
    case 0xa: shc_set_mode2(data); break;
    case 0xb:
        shc_add_latency(0xb, data, g_shc.mode1);
        g_shc.mode1 = data;
        break;
    case 0xc: shc_set_astat(data); break;
    case 0xd:
        shc_check_interrupts();
        g_shc.imask = data;
        break;
    case 0xe: g_shc.s.stky = (g_shc.s.stky & keep) | (data & ~keep); break;
    default: shc_fatal("SET_UREG unknown register", (uint32_t)ureg); break;
    }
}

static void shc_set_px(int ureg, uint32_t data)
{
    if ((ureg & 0xf) == 0xc)
        g_shc.px = (g_shc.px & 0xffffffffffff0000ull) | (data & 0xffff);
    else if ((ureg & 0xf) == 0xd)
        g_shc.px = (g_shc.px & 0xffffull) | ((uint64_t)data << 16);
    else
        shc_fatal("SET_UREG unknown register", (uint32_t)ureg);
}

static void shc_set_ureg(int ureg, uint32_t data)
{
    int const reg = ureg & 0xf;
    shc_dag_t *d = (reg & 8) ? &g_shc.dag2 : &g_shc.dag1;
    switch ((ureg >> 4) & 0xf) {
    case 0x0: g_shc.r[reg] = data; break;
    case 0x1: d->i[reg & 7] = data; break;
    case 0x2: d->m[reg & 7] = data; break;
    case 0x3: d->l[reg & 7] = data; break;
    case 0x4:                       /* loading B loads I too */
        d->b[reg & 7] = data;
        d->i[reg & 7] = data;
        break;
    case 0x6: shc_set_ureg6(ureg, data); break;
    case 0x7: shc_set_sysreg(ureg, data); break;
    case 0xd: shc_set_px(ureg, data); break;
    default: shc_fatal("SET_UREG unknown register", (uint32_t)ureg); break;
    }
}

/* ---- Conditions ------------------------------------------------------------ */

static bool shc_cond_lt(void)
{
    uint32_t const a = g_shc.astat;
    if (a & SHC_AF)
        return (a & SHC_AN) && !(a & SHC_AZ);
    return ((a & SHC_AN) != 0) != ((a & SHC_AV) && !(g_shc.mode1 & SHC_M1_ALUSAT));
}

static bool shc_cond_le(void)
{
    uint32_t const a = g_shc.astat;
    if (a & SHC_AZ)
        return true;
    if (a & SHC_AF)
        return (a & SHC_AN) != 0;
    return ((a & SHC_AN) != 0) != ((a & SHC_AV) && !(g_shc.mode1 & SHC_M1_ALUSAT));
}

/* conditions 0x00-0x0E; 0x10-0x1E are their negations */
static bool shc_cond_base(unsigned low)
{
    static const uint32_t m[15] = {
        SHC_AZ, 0, 0, SHC_AC, SHC_AV, SHC_MV, SHC_MN, SHC_SV, SHC_SZ,
        0, 0, 0, 0, SHC_BTF, 0,
    };
    if (low == 1)
        return shc_cond_lt();
    if (low == 2)
        return shc_cond_le();
    if (low >= 9 && low <= 12)
        return g_shc.flag[low - 9] != 0;
    return (g_shc.astat & m[low]) != 0;
}

static bool shc_if_cond(unsigned c)
{
    if ((c & 0xf) == 0xf)       /* NOT LCE / TRUE */
        return (c & 0x10) ? true : g_shc.s.curlcntr != 1;
    bool const v = shc_cond_base(c & 0xf);
    return (c & 0x10) ? !v : v;
}

static bool shc_do_cond(unsigned c)
{
    if ((c & 0xf) == 0xf)       /* LCE / FALSE (FOREVER) */
        return (c & 0x10) ? false : g_shc.s.curlcntr == 1;
    bool const v = shc_cond_base(c & 0xf);
    return (c & 0x10) ? !v : v;
}

/* ---- Interrupts ------------------------------------------------------------ */

static void shc_check_interrupts(void)
{
    if (!(g_shc.imask & (uint32_t)g_shc.irq_pending) || !(g_shc.mode1 & SHC_M1_IRPTEN) ||
        g_shc.interrupt_active || g_shc.pc == g_shc.delay_slot1 || g_shc.pc == g_shc.delay_slot2)
        return;
    int which = 0;
    while (which < 32 && !((g_shc.irq_pending >> which) & 1))
        which++;
    shc_push_pc();
    g_shc.s.pcstk = g_shc.idle ? g_shc.pc + 1 : g_shc.daddr;
    g_shc.irptl |= 1u << which;
    if (which >= 6 && which <= 8)
        shc_push_status();
    shc_change_pc(0x20000 + (uint32_t)which * 4);
    g_shc.active_irq = which;
    g_shc.irq_pending &= ~(1 << which);
    g_shc.interrupt_active = 1;
}

/* ci: clear the interrupt being served (jumps with the CI bit) */
static void shc_clear_interrupt(void)
{
    if (g_shc.active_irq >= 6 && g_shc.active_irq <= 8)
        shc_pop_status();
    g_shc.interrupt_active = 0;
    g_shc.irptl &= ~(1u << g_shc.active_irq);
}

/* ---- Compute: float helpers (MAME compute.hxx) ----------------------------- */

static const uint32_t shc_recips_lut[128] = {
    0x007f8000, 0x007e0000, 0x007c0000, 0x007a0000, 0x00780000, 0x00760000, 0x00740000, 0x00720000,
    0x00700000, 0x006f0000, 0x006d0000, 0x006b0000, 0x006a0000, 0x00680000, 0x00660000, 0x00650000,
    0x00630000, 0x00610000, 0x00600000, 0x005e0000, 0x005d0000, 0x005b0000, 0x005a0000, 0x00590000,
    0x00570000, 0x00560000, 0x00540000, 0x00530000, 0x00520000, 0x00500000, 0x004f0000, 0x004e0000,
    0x004c0000, 0x004b0000, 0x004a0000, 0x00490000, 0x00470000, 0x00460000, 0x00450000, 0x00440000,
    0x00430000, 0x00410000, 0x00400000, 0x003f0000, 0x003e0000, 0x003d0000, 0x003c0000, 0x003b0000,
    0x003a0000, 0x00390000, 0x00380000, 0x00370000, 0x00360000, 0x00350000, 0x00340000, 0x00330000,
    0x00320000, 0x00310000, 0x00300000, 0x002f0000, 0x002e0000, 0x002d0000, 0x002c0000, 0x002b0000,
    0x002a0000, 0x00290000, 0x00280000, 0x00280000, 0x00270000, 0x00260000, 0x00250000, 0x00240000,
    0x00230000, 0x00230000, 0x00220000, 0x00210000, 0x00200000, 0x001f0000, 0x001f0000, 0x001e0000,
    0x001d0000, 0x001c0000, 0x001c0000, 0x001b0000, 0x001a0000, 0x00190000, 0x00190000, 0x00180000,
    0x00170000, 0x00170000, 0x00160000, 0x00150000, 0x00140000, 0x00140000, 0x00130000, 0x00120000,
    0x00120000, 0x00110000, 0x00100000, 0x00100000, 0x000f0000, 0x000f0000, 0x000e0000, 0x000d0000,
    0x000d0000, 0x000c0000, 0x000c0000, 0x000b0000, 0x000a0000, 0x000a0000, 0x00090000, 0x00090000,
    0x00080000, 0x00070000, 0x00070000, 0x00060000, 0x00060000, 0x00050000, 0x00050000, 0x00040000,
    0x00040000, 0x00030000, 0x00030000, 0x00020000, 0x00020000, 0x00010000, 0x00010000, 0x00000000,
};

static const uint32_t shc_rsqrts_lut[128] = {
    0x00350000, 0x00330000, 0x00320000, 0x00300000, 0x002f0000, 0x002e0000, 0x002d0000, 0x002b0000,
    0x002a0000, 0x00290000, 0x00280000, 0x00270000, 0x00260000, 0x00250000, 0x00230000, 0x00220000,
    0x00210000, 0x00200000, 0x001f0000, 0x001e0000, 0x001e0000, 0x001d0000, 0x001c0000, 0x001b0000,
    0x001a0000, 0x00190000, 0x00180000, 0x00170000, 0x00160000, 0x00160000, 0x00150000, 0x00140000,
    0x00130000, 0x00130000, 0x00120000, 0x00110000, 0x00100000, 0x00100000, 0x000f0000, 0x000e0000,
    0x000e0000, 0x000d0000, 0x000c0000, 0x000b0000, 0x000b0000, 0x000a0000, 0x000a0000, 0x00090000,
    0x00080000, 0x00080000, 0x00070000, 0x00070000, 0x00060000, 0x00050000, 0x00050000, 0x00040000,
    0x00040000, 0x00030000, 0x00030000, 0x00020000, 0x00020000, 0x00010000, 0x00010000, 0x00000000,
    0x007f8000, 0x007e0000, 0x007c0000, 0x007a0000, 0x00780000, 0x00760000, 0x00740000, 0x00730000,
    0x00710000, 0x006f0000, 0x006e0000, 0x006c0000, 0x006a0000, 0x00690000, 0x00670000, 0x00660000,
    0x00640000, 0x00630000, 0x00620000, 0x00600000, 0x005f0000, 0x005e0000, 0x005c0000, 0x005b0000,
    0x005a0000, 0x00590000, 0x00570000, 0x00560000, 0x00550000, 0x00540000, 0x00530000, 0x00520000,
    0x00510000, 0x004f0000, 0x004e0000, 0x004d0000, 0x004c0000, 0x004b0000, 0x004a0000, 0x00490000,
    0x00480000, 0x00470000, 0x00460000, 0x00450000, 0x00450000, 0x00440000, 0x00430000, 0x00420000,
    0x00410000, 0x00400000, 0x003f0000, 0x003e0000, 0x003e0000, 0x003d0000, 0x003c0000, 0x003b0000,
    0x003a0000, 0x003a0000, 0x00390000, 0x00380000, 0x00370000, 0x00370000, 0x00360000, 0x00350000,
};

static inline bool shc_is_zero(uint32_t r) { return (r & 0x7fffffffu) == 0; }
static inline bool shc_is_doz(uint32_t r) { return (r & SHC_F_EXP) == 0; }
static inline bool shc_is_denorm(uint32_t r) { return shc_is_doz(r) && (r & SHC_F_MANT); }
static inline bool shc_emax(uint32_t r) { return (r & SHC_F_EXP) == SHC_F_EXP; }
static inline bool shc_is_nan(uint32_t r) { return shc_emax(r) && (r & SHC_F_MANT); }
static inline bool shc_is_inf(uint32_t r) { return (r & 0x7fffffffu) == SHC_F_INF; }
static inline uint32_t shc_flush(uint32_t r) { return shc_is_doz(r) ? (r & SHC_F_SIGN) : r; }
static inline int shc_unbiased(uint32_t r) { return (int)((r >> 23) & 0xff) - 127; }
static inline uint32_t shc_biased(int e) { return ((uint32_t)(e + 127) & 0xff) << 23; }
static inline bool shc_trunc(void) { return (g_shc.mode1 & SHC_M1_TRUNC) != 0; }
static inline float shc_fr(int x) { return shc_u2f(g_shc.r[x]); }

static bool shc_nan_add(uint32_t a, uint32_t b)
{
    return shc_is_nan(a) || shc_is_nan(b) ||
           (shc_emax(a) && shc_emax(b) && ((a ^ b) & SHC_F_SIGN));
}

static bool shc_nan_sub(uint32_t a, uint32_t b)
{
    return shc_is_nan(a) || shc_is_nan(b) ||
           (shc_emax(a) && shc_emax(b) && !((a ^ b) & SHC_F_SIGN));
}

static bool shc_nan_mul(uint32_t a, uint32_t b)
{
    return (shc_emax(a) && ((a & SHC_F_MANT) || shc_is_zero(b))) ||
           (shc_emax(b) && ((b & SHC_F_MANT) || shc_is_zero(a)));
}

/* int32_t(floorf / nearbyintf), or int32_t(f) for `toward_zero` under TRUNC
 * (MAME's FIX rounds down, its Fm = Fxm * Fym, Ra = FIX Fxa BY Rya toward
 * zero); NaN and out of range give 0x80000000, as on x86 */
static uint32_t shc_fix_bits(uint32_t r, bool toward_zero)
{
    float const f = shc_u2f(r);
    float const v = !shc_trunc() ? nearbyintf(f) : toward_zero ? truncf(f) : floorf(f);
    if (v != v || v >= 2147483648.0f || v < -2147483648.0f)
        return 0x80000000u;
    return (uint32_t)(int32_t)v;
}

static inline void shc_alu_clear(void)
{
    g_shc.astat &= ~(uint32_t)(SHC_AZ | SHC_AN | SHC_AV | SHC_AC | SHC_AS | SHC_AI);
}
static inline void shc_mul_clear(void)
{
    g_shc.astat &= ~(uint32_t)(SHC_MN | SHC_MV | SHC_MU | SHC_MI);
}
static inline void shc_set_az(uint32_t r) { if (r == 0) g_shc.astat |= SHC_AZ; }
static inline void shc_set_an(uint32_t r) { if (r & SHC_F_SIGN) g_shc.astat |= SHC_AN; }
static inline void shc_set_mn(uint32_t r) { if (r & SHC_F_SIGN) g_shc.astat |= SHC_MN; }
static inline void shc_set_sz(uint32_t r) { if (r == 0) g_shc.astat |= SHC_SZ; }
static inline void shc_alu_nan(void)
{
    g_shc.astat |= SHC_AI;
    g_shc.s.stky |= SHC_AIS;
}

static void shc_av_add(uint32_t r, uint32_t a, uint32_t b)
{
    if (~(a ^ b) & (a ^ r) & SHC_F_SIGN) {
        g_shc.astat |= SHC_AV;
        g_shc.s.stky |= SHC_AOS;
    }
}
static void shc_av_sub(uint32_t r, uint32_t a, uint32_t b)
{
    if ((a ^ b) & (a ^ r) & SHC_F_SIGN) {
        g_shc.astat |= SHC_AV;
        g_shc.s.stky |= SHC_AOS;
    }
}
static inline void shc_ac_add(uint32_t r, uint32_t a) { if (r < a) g_shc.astat |= SHC_AC; }
static inline void shc_ac_sub(uint32_t r, uint32_t a) { if (r <= a) g_shc.astat |= SHC_AC; }

static inline uint32_t shc_saturate(uint32_t r) { return shc_asr(r, 31) ^ SHC_F_SIGN; }

/* the integer ALU epilogue: ALUSAT, AN, AZ, store, clear AF */
static void shc_int_out(int rn, uint32_t r)
{
    if ((g_shc.mode1 & SHC_M1_ALUSAT) && (g_shc.astat & SHC_AV))
        r = shc_saturate(r);
    shc_set_an(r);
    shc_set_az(r);
    g_shc.r[rn] = r;
    g_shc.astat &= ~(uint32_t)SHC_AF;
}

/* FLOAT_ALU_RESULT: overflow saturates under TRUNC, underflow flushes */
static uint32_t shc_falu_result(uint32_t r)
{
    if (shc_is_inf(r)) {
        g_shc.astat |= SHC_AV;
        g_shc.s.stky |= SHC_AVS;
        if (shc_trunc())
            r = (r & SHC_F_SIGN) | SHC_F_MAXT;
    } else if (shc_is_doz(r)) {
        g_shc.astat |= SHC_AZ;
        if (r & SHC_F_MANT)
            g_shc.s.stky |= SHC_AUS;
        r &= SHC_F_SIGN;
    }
    return r;
}

/* FADD / FSUB / FAVG on register bits (kind 0, 1, 2) */
static uint32_t shc_faddsub(uint32_t a, uint32_t b, int kind)
{
    uint32_t r;
    if (kind == 1 ? shc_nan_sub(a, b) : shc_nan_add(a, b)) {
        r = SHC_F_NAN;
        shc_alu_nan();
    } else {
        float const x = shc_u2f(shc_flush(a));
        float const y = shc_u2f(shc_flush(b) ^ (kind == 1 ? SHC_F_SIGN : 0));
        float f = x + y;
        if (kind == 2)
            f = f * 0.5f;
        r = shc_falu_result(shc_f2u(f));
        shc_set_an(r);
    }
    g_shc.astat |= SHC_AF;
    return r;
}

static uint32_t shc_fabs_bits(uint32_t x)
{
    uint32_t r;
    if (shc_is_nan(x)) {
        r = SHC_F_NAN;
        shc_alu_nan();
    } else {
        if (shc_is_doz(x)) {
            r = 0;
            g_shc.astat |= SHC_AZ;
        } else {
            r = x & ~SHC_F_SIGN;
        }
        if (x & SHC_F_SIGN)
            g_shc.astat |= SHC_AS;
    }
    g_shc.astat |= SHC_AF;
    return r;
}

/* FMIN / FMAX: std::min(a, b) is (b < a) ? b : a, std::max (a < b) ? b : a */
static uint32_t shc_fminmax(uint32_t a, uint32_t b, bool max)
{
    uint32_t r;
    if (shc_is_nan(a) || shc_is_nan(b)) {
        r = SHC_F_NAN;
        shc_alu_nan();
    } else {
        float const fa = shc_u2f(a), fb = shc_u2f(b);
        r = (max ? (fa < fb) : (fb < fa)) ? b : a;
        if (shc_u2f(r) < 0.0f)
            g_shc.astat |= SHC_AN;
        if (shc_is_zero(r))
            g_shc.astat |= SHC_AZ;
    }
    g_shc.astat |= SHC_AF;
    return r;
}

static uint32_t shc_fmax_bits(uint32_t a, uint32_t b) { return shc_fminmax(a, b, true); }
static uint32_t shc_fmin_bits(uint32_t a, uint32_t b) { return shc_fminmax(a, b, false); }

/* MAME FADD_FSUB, both infinite: the integer pattern converted to float */
static uint32_t shc_inf_quirk(uint32_t x)
{
    uint32_t const v = (x & SHC_F_SIGN) | (shc_trunc() ? SHC_F_MAXT : SHC_F_INF);
    return shc_f2u((float)v);
}

static void shc_faddfsub(uint32_t a, uint32_t b, uint32_t *ra, uint32_t *rs)
{
    if (shc_is_nan(a) || shc_is_nan(b)) {
        *ra = *rs = SHC_F_NAN;
        shc_alu_nan();
    } else if (!shc_emax(a) || !shc_emax(b)) {
        float const x = shc_u2f(shc_flush(a));
        float const y = shc_u2f(shc_flush(b));
        bool const t = shc_trunc();
        *ra = shc_falu_result(shc_f2u(x + y));
        *rs = shc_falu_result(shc_f2u(x - y));
        shc_set_an(*ra | *rs);
    } else {
        if ((a ^ b) & SHC_F_SIGN) {
            *ra = SHC_F_NAN;
            *rs = shc_inf_quirk(a);
        } else {
            *ra = shc_inf_quirk(a);
            *rs = SHC_F_NAN;
        }
        shc_set_an(a);
        g_shc.astat |= SHC_AV | SHC_AI;
        g_shc.s.stky |= SHC_AVS | SHC_AIS;
    }
    g_shc.astat |= SHC_AF;
}

static uint32_t shc_scalb(uint32_t x, int ry)
{
    uint32_t const sign = x & SHC_F_SIGN;
    int32_t const e = (int32_t)((uint32_t)shc_unbiased(x) + g_shc.r[ry]);
    if (e > 127) {
        g_shc.astat |= SHC_AV;
        return sign | SHC_F_INF;
    }
    if (e < -126) {
        g_shc.astat |= SHC_AZ;
        return sign;
    }
    return sign | shc_biased(e) | (x & SHC_F_MANT);
}

static uint32_t shc_fmul_bits(uint32_t a, uint32_t b)
{
    uint32_t r;
    if (shc_nan_mul(a, b)) {
        g_shc.astat |= SHC_MI;
        g_shc.s.stky |= SHC_MIS;
        return SHC_F_NAN;
    }
    r = shc_f2u(shc_u2f(a) * shc_u2f(b));
    shc_set_mn(r);
    if (shc_is_inf(r)) {
        g_shc.astat |= SHC_MV;
        g_shc.s.stky |= SHC_MVS;
    } else if (shc_is_denorm(r)) {
        g_shc.astat |= SHC_MU;
        g_shc.s.stky |= SHC_MUS;
    }
    return r;
}

/* ---- Integer ALU ----------------------------------------------------------- */

#define SHC_X g_shc.r[rx]
#define SHC_Y g_shc.r[ry]
#define SHC_CARRY ((g_shc.astat & SHC_AC) ? 1u : 0u)

static void shc_op_add(int rn, int rx, int ry)
{
    uint32_t const r = SHC_X + SHC_Y;
    shc_alu_clear();
    shc_av_add(r, SHC_X, SHC_Y);
    shc_ac_add(r, SHC_X);
    shc_int_out(rn, r);
}

static void shc_op_sub(int rn, int rx, int ry)
{
    uint32_t const r = SHC_X - SHC_Y;
    shc_alu_clear();
    shc_av_sub(r, SHC_X, SHC_Y);
    shc_ac_sub(r, SHC_X);
    shc_int_out(rn, r);
}

static void shc_op_add_ci(int rn, int rx, int ry)
{
    uint32_t const c = SHC_CARRY;
    uint32_t const r = SHC_X + SHC_Y + c;
    shc_alu_clear();
    shc_av_add(r, SHC_X, SHC_Y);
    shc_ac_add(r, SHC_X);
    if (c == 1 && SHC_Y == 0xffffffffu)
        g_shc.astat |= SHC_AC;
    shc_int_out(rn, r);
}

static void shc_op_sub_ci(int rn, int rx, int ry)
{
    uint32_t const c = SHC_CARRY;
    uint32_t const r = SHC_X - SHC_Y + c - 1;
    shc_alu_clear();
    shc_av_sub(r, SHC_X, SHC_Y);
    if (c != 0 || SHC_Y != 0xffffffffu)
        shc_ac_sub(r, SHC_X);
    shc_int_out(rn, r);
}

/* the compare accumulator: the last eight "greater than"s in ASTAT 24-31 */
static void shc_comp_accum(void)
{
    uint32_t acc = (g_shc.astat >> 1) & 0x7f000000u;
    if ((g_shc.astat & (SHC_AZ | SHC_AN)) == 0)
        acc |= 0x80000000u;
    g_shc.astat = (g_shc.astat & 0x00ffffffu) | acc;
}

static void shc_op_comp(int rn, int rx, int ry)
{
    (void)rn;
    shc_alu_clear();
    if (SHC_X == SHC_Y)
        g_shc.astat |= SHC_AZ;
    else if ((int32_t)SHC_X < (int32_t)SHC_Y)
        g_shc.astat |= SHC_AN;
    shc_comp_accum();
    g_shc.astat &= ~(uint32_t)SHC_AF;
}

static void shc_op_add_ci1(int rn, int rx, int ry)
{
    (void)ry;
    uint32_t const r = SHC_X + SHC_CARRY;
    shc_alu_clear();
    shc_av_add(r, SHC_X, 0);
    shc_ac_add(r, SHC_X);
    shc_int_out(rn, r);
}

static void shc_op_sub_ci1(int rn, int rx, int ry)
{
    (void)ry;
    uint32_t const r = SHC_X + SHC_CARRY - 1;
    shc_alu_clear();
    shc_av_sub(r, SHC_X, 0);
    shc_ac_sub(r, SHC_X);
    shc_int_out(rn, r);
}

static void shc_op_inc(int rn, int rx, int ry)
{
    (void)ry;
    uint32_t const r = SHC_X + 1;
    shc_alu_clear();
    shc_av_add(r, SHC_X, 1);
    shc_ac_add(r, SHC_X);
    shc_int_out(rn, r);
}

static void shc_op_dec(int rn, int rx, int ry)
{
    (void)ry;
    uint32_t const r = SHC_X - 1;
    shc_alu_clear();
    shc_av_sub(r, SHC_X, 1);
    shc_ac_sub(r, SHC_X);
    shc_int_out(rn, r);
}

static void shc_op_neg(int rn, int rx, int ry)
{
    (void)ry;
    uint32_t const r = 0u - SHC_X;
    shc_alu_clear();
    shc_av_sub(r, 0, SHC_X);
    shc_ac_sub(r, 0);
    shc_int_out(rn, r);
}

static void shc_op_abs(int rn, int rx, int ry)
{
    (void)ry;
    uint32_t const x = SHC_X;
    uint32_t r = (x & SHC_F_SIGN) ? 0u - x : x;
    shc_alu_clear();
    if (r & SHC_F_SIGN) {
        g_shc.astat |= SHC_AV;
        g_shc.s.stky |= SHC_AOS;
        if (g_shc.mode1 & SHC_M1_ALUSAT)
            r = 0x7fffffffu;
    }
    shc_set_an(r);
    shc_set_az(r);
    if (x & SHC_F_SIGN)
        g_shc.astat |= SHC_AS;
    g_shc.r[rn] = r;
    g_shc.astat &= ~(uint32_t)SHC_AF;
}

/* PASS, AND, OR, XOR, NOT, MIN, MAX: AN and AZ of the result only */
static void shc_logic_out(int rn, uint32_t r)
{
    shc_alu_clear();
    shc_set_an(r);
    shc_set_az(r);
    g_shc.r[rn] = r;
    g_shc.astat &= ~(uint32_t)SHC_AF;
}

static void shc_op_pass(int rn, int rx, int ry) { (void)ry; shc_logic_out(rn, SHC_X); }
static void shc_op_and(int rn, int rx, int ry) { shc_logic_out(rn, SHC_X & SHC_Y); }
static void shc_op_or(int rn, int rx, int ry) { shc_logic_out(rn, SHC_X | SHC_Y); }
static void shc_op_xor(int rn, int rx, int ry) { shc_logic_out(rn, SHC_X ^ SHC_Y); }
static void shc_op_not(int rn, int rx, int ry) { (void)ry; shc_logic_out(rn, ~SHC_X); }

static void shc_op_min(int rn, int rx, int ry)
{
    shc_logic_out(rn, (int32_t)SHC_Y < (int32_t)SHC_X ? SHC_Y : SHC_X);
}

static void shc_op_max(int rn, int rx, int ry)
{
    shc_logic_out(rn, (int32_t)SHC_X < (int32_t)SHC_Y ? SHC_Y : SHC_X);
}

/* CLIP Rx BY Ry: std::clamp(Rx, -|Ry|, |Ry|), |INT_MIN| wrapping as on x86 */
static void shc_op_clip(int rn, int rx, int ry)
{
    uint32_t const a = (SHC_Y & SHC_F_SIGN) ? 0u - SHC_Y : SHC_Y;
    int32_t const hi = (int32_t)a, lo = (int32_t)(0u - a);
    int32_t const v = (int32_t)SHC_X;
    shc_logic_out(rn, (uint32_t)(v < lo ? lo : (hi < v ? hi : v)));
}

/* ---- Float ALU ------------------------------------------------------------- */

static void shc_op_fadd(int rn, int rx, int ry)
{
    shc_alu_clear();
    g_shc.r[rn] = shc_faddsub(SHC_X, SHC_Y, 0);
}

static void shc_op_fsub(int rn, int rx, int ry)
{
    shc_alu_clear();
    g_shc.r[rn] = shc_faddsub(SHC_X, SHC_Y, 1);
}

static void shc_op_favg(int rn, int rx, int ry)
{
    shc_alu_clear();
    g_shc.r[rn] = shc_faddsub(SHC_X, SHC_Y, 2);
}

/* ABS(Fx + Fy), ABS(Fx - Fy) */
static void shc_fabs_addsub(int rn, uint32_t a, uint32_t b, bool sub)
{
    uint32_t r;
    shc_alu_clear();
    if (sub ? shc_nan_sub(a, b) : shc_nan_add(a, b)) {
        r = SHC_F_NAN;
        shc_alu_nan();
    } else {
        float const x = shc_u2f(shc_flush(a));
        float const y = shc_u2f(shc_flush(b) ^ (sub ? SHC_F_SIGN : 0));
        r = shc_f2u(x + y) & ~SHC_F_SIGN;
        if (r == SHC_F_EXP) {
            g_shc.astat |= SHC_AV;
            g_shc.s.stky |= SHC_AVS;
            if (shc_trunc())
                r = SHC_F_MAXT;
        } else if (shc_is_doz(r)) {
            g_shc.astat |= SHC_AZ;
            if (r & SHC_F_MANT)
                g_shc.s.stky |= SHC_AUS;
            r = 0;
        }
    }
    g_shc.r[rn] = r;
    g_shc.astat |= SHC_AF;
}

static void shc_op_fadd_abs(int rn, int rx, int ry) { shc_fabs_addsub(rn, SHC_X, SHC_Y, false); }
static void shc_op_fsub_abs(int rn, int rx, int ry) { shc_fabs_addsub(rn, SHC_X, SHC_Y, true); }

static void shc_op_fcomp(int rn, int rx, int ry)
{
    (void)rn;
    shc_alu_clear();
    if (shc_is_nan(SHC_X) || shc_is_nan(SHC_Y)) {
        shc_alu_nan();
    } else if (shc_fr(rx) == shc_fr(ry)) {
        g_shc.astat |= SHC_AZ;
    } else if (shc_fr(rx) < shc_fr(ry)) {
        g_shc.astat |= SHC_AN;
    }
    shc_comp_accum();
    g_shc.astat |= SHC_AF;
}

/* -Fx (neg) or PASS Fx */
static void shc_fneg_pass(int rn, uint32_t x, bool neg)
{
    uint32_t r;
    shc_alu_clear();
    if (shc_is_nan(x)) {
        r = SHC_F_NAN;
        shc_alu_nan();
    } else {
        if (shc_is_doz(x)) {
            r = (neg ? ~x : x) & SHC_F_SIGN;
            g_shc.astat |= SHC_AZ;
        } else {
            r = neg ? x ^ SHC_F_SIGN : x;
        }
        shc_set_an(r);
    }
    g_shc.r[rn] = r;
    g_shc.astat |= SHC_AF;
}

static void shc_op_fneg(int rn, int rx, int ry) { (void)ry; shc_fneg_pass(rn, SHC_X, true); }
static void shc_op_fpass(int rn, int rx, int ry) { (void)ry; shc_fneg_pass(rn, SHC_X, false); }

static void shc_op_fabs(int rn, int rx, int ry)
{
    (void)ry;
    shc_alu_clear();
    g_shc.r[rn] = shc_fabs_bits(SHC_X);
}

static void shc_op_scalb(int rn, int rx, int ry)
{
    shc_alu_clear();
    if (shc_is_nan(SHC_X)) {
        shc_alu_nan();
        g_shc.r[rn] = SHC_F_NAN;
    } else {
        uint32_t const r = shc_scalb(SHC_X, ry);
        shc_set_an(r);
        if (shc_is_zero(r))
            g_shc.astat |= SHC_AZ;
        if (shc_is_denorm(r))
            g_shc.s.stky |= SHC_AUS;
        g_shc.r[rn] = r;
    }
    g_shc.astat |= SHC_AF;
}

static void shc_op_logb(int rn, int rx, int ry)
{
    (void)ry;
    uint32_t const x = SHC_X;
    shc_alu_clear();
    if (shc_is_inf(x)) {
        g_shc.r[rn] = SHC_F_INF;
        g_shc.astat |= SHC_AV;
    } else if (shc_is_zero(x)) {
        g_shc.r[rn] = SHC_F_SIGN | SHC_F_INF;
        g_shc.astat |= SHC_AV;
    } else if (shc_is_nan(x)) {
        g_shc.r[rn] = SHC_F_NAN;
        shc_alu_nan();
    } else {
        uint32_t const e = (uint32_t)shc_unbiased(x);
        shc_set_an(e);
        shc_set_az(e);
        g_shc.r[rn] = e;
    }
    g_shc.astat |= SHC_AF;
}

/* FIX Fx [BY Ry]: r_alu is the (scaled) float the integer comes from; the
 * multifunction FIX (`clear` false) truncates toward zero, as in MAME */
static void shc_fix_out(int rn, uint32_t r_alu, uint32_t src, bool clear)
{
    uint32_t const i = shc_fix_bits(r_alu, !clear);
    if (clear)
        shc_alu_clear();
    shc_set_an(i);
    shc_set_az(i);
    if (shc_is_denorm(r_alu))
        g_shc.s.stky |= SHC_AUS;
    if (shc_is_nan(src))
        g_shc.astat |= SHC_AI;
    g_shc.r[rn] = i;
    g_shc.astat |= SHC_AF;
}

static void shc_op_fix(int rn, int rx, int ry)
{
    (void)ry;
    shc_fix_out(rn, SHC_X, SHC_X, true);
}

static void shc_op_fix_scaled(int rn, int rx, int ry)
{
    uint32_t const r_alu = shc_scalb(SHC_X, ry);
    shc_fix_out(rn, r_alu, SHC_X, true);
}

static void shc_float_flags(uint32_t r)
{
    shc_set_an(r);
    if (shc_is_doz(r))
        g_shc.astat |= SHC_AZ;
    if (shc_is_denorm(r))
        g_shc.s.stky |= SHC_AUS;
    g_shc.astat |= SHC_AF;
}

static void shc_op_float(int rn, int rx, int ry)
{
    (void)ry;
    g_shc.r[rn] = shc_f2u((float)(int32_t)SHC_X);
    shc_alu_clear();
    shc_float_flags(g_shc.r[rn]);
}

static void shc_op_float_scaled(int rn, int rx, int ry)
{
    uint32_t const x = shc_f2u((float)(int32_t)SHC_X);
    shc_alu_clear();
    g_shc.r[rn] = shc_scalb(x, ry);
    shc_float_flags(g_shc.r[rn]);
}

static void shc_op_recips(int rn, int rx, int ry)
{
    (void)ry;
    uint32_t const x = SHC_X;
    uint32_t r;
    shc_alu_clear();
    if (shc_is_nan(x)) {
        r = SHC_F_NAN;
        shc_alu_nan();
    } else if (shc_is_zero(x)) {
        r = (x & SHC_F_SIGN) | SHC_F_INF;
        g_shc.astat |= SHC_AV;
    } else {
        int e = -shc_unbiased(x) - 1;
        uint32_t m = shc_recips_lut[(x & SHC_F_MANT) >> 16];
        if (e > 125 || e < -126) {
            e = 0;
            m = 0;
        } else {
            e = (e + 127) & 0xff;
        }
        r = (x & SHC_F_SIGN) | ((uint32_t)e << 23) | m;
        shc_set_an(x);
        if (shc_is_zero(r))
            g_shc.astat |= SHC_AZ;
    }
    g_shc.r[rn] = r;
    g_shc.astat |= SHC_AF;
}

static void shc_op_rsqrts(int rn, int rx, int ry)
{
    (void)ry;
    uint32_t const x = SHC_X;
    uint32_t r;
    if (x > 0x80000000u || shc_is_nan(x)) {
        r = SHC_F_NAN;
    } else {
        int const u = shc_unbiased(x);
        int const half = u >= 0 ? (u >> 1) : ~((~u) >> 1);    /* arithmetic >> 1 */
        r = (x & SHC_F_SIGN) | shc_biased(-half - 1) | shc_rsqrts_lut[(x & 0xffffff) >> 17];
    }
    shc_alu_clear();
    if (x == 0x80000000u)
        g_shc.astat |= SHC_AN;
    if (shc_is_zero(r))
        g_shc.astat |= SHC_AZ;
    if (shc_is_zero(x))
        g_shc.astat |= SHC_AV;
    if (shc_is_nan(x) || (x & SHC_F_SIGN)) {
        g_shc.astat |= SHC_AI;
        g_shc.s.stky |= SHC_AIS;
    }
    g_shc.r[rn] = r;
    g_shc.astat |= SHC_AF;
}

static void shc_op_fcopysign(int rn, int rx, int ry)
{
    uint32_t r;
    shc_alu_clear();
    if (shc_is_nan(SHC_X) || shc_is_nan(SHC_Y)) {
        r = SHC_F_NAN;
        shc_alu_nan();
    } else {
        r = SHC_Y & SHC_F_SIGN;
        if (r)
            g_shc.astat |= SHC_AN;
        if (shc_is_doz(SHC_X))
            g_shc.astat |= SHC_AZ;
        else
            r |= SHC_X & ~SHC_F_SIGN;
    }
    g_shc.r[rn] = r;
    g_shc.astat |= SHC_AF;
}

static void shc_op_fmin(int rn, int rx, int ry)
{
    shc_alu_clear();
    g_shc.r[rn] = shc_fmin_bits(SHC_X, SHC_Y);
}

static void shc_op_fmax(int rn, int rx, int ry)
{
    shc_alu_clear();
    g_shc.r[rn] = shc_fmax_bits(SHC_X, SHC_Y);
}

static void shc_op_fclip(int rn, int rx, int ry)
{
    uint32_t const x = SHC_X, y = SHC_Y;
    uint32_t r;
    shc_alu_clear();
    if (shc_is_nan(x) || shc_is_nan(y)) {
        r = SHC_F_NAN;
        shc_alu_nan();
    } else {
        if (shc_is_doz(x) || shc_is_doz(y)) {
            r = x & SHC_F_SIGN;
            g_shc.astat |= SHC_AZ;
        } else {
            float const hi = shc_u2f(y & ~SHC_F_SIGN), lo = shc_u2f(y | SHC_F_SIGN);
            float const v = shc_u2f(x);
            r = v < lo ? (y | SHC_F_SIGN) : (hi < v ? (y & ~SHC_F_SIGN) : x);
            if (shc_is_zero(r))
                g_shc.astat |= SHC_AZ;
        }
        shc_set_an(r);
    }
    g_shc.r[rn] = r;
    g_shc.astat |= SHC_AF;
}

/* Ra = Rx + Ry, Rs = Rx - Ry */
static void shc_dual_add_sub(int ra, int rs, int rx, int ry)
{
    uint32_t const x = SHC_X, y = SHC_Y;
    uint32_t r_add = x + y, r_sub = x - y;
    bool const av_add = (~(x ^ y) & (x ^ r_add) & SHC_F_SIGN) != 0;
    bool const av_sub = ((x ^ y) & (x ^ r_sub) & SHC_F_SIGN) != 0;
    shc_alu_clear();
    if (av_add || av_sub) {
        g_shc.astat |= SHC_AV;
        g_shc.s.stky |= SHC_AOS;
    }
    if (r_add < x || r_sub <= x)
        g_shc.astat |= SHC_AC;
    if (g_shc.mode1 & SHC_M1_ALUSAT) {
        if (av_add) r_add = shc_saturate(r_add);
        if (av_sub) r_sub = shc_saturate(r_sub);
    }
    if (r_add == 0 || r_sub == 0)
        g_shc.astat |= SHC_AZ;
    shc_set_an(r_add | r_sub);
    g_shc.r[ra] = r_add;
    g_shc.r[rs] = r_sub;
    g_shc.astat &= ~(uint32_t)SHC_AF;
}

/* Fa = Fx + Fy, Fs = Fx - Fy */
static void shc_dual_fadd_fsub(int fa, int fs, int fx, int fy)
{
    uint32_t ra, rs;
    shc_alu_clear();
    shc_faddfsub(g_shc.r[fx], g_shc.r[fy], &ra, &rs);
    g_shc.r[fa] = ra;
    g_shc.r[fs] = rs;
}

typedef void (*shc_alu_fn)(int rn, int rx, int ry);

static const shc_alu_fn shc_alu_tab[256] = {
    [0x01] = shc_op_add, [0x02] = shc_op_sub, [0x05] = shc_op_add_ci,
    [0x06] = shc_op_sub_ci, [0x0a] = shc_op_comp, [0x21] = shc_op_pass,
    [0x22] = shc_op_neg, [0x25] = shc_op_add_ci1, [0x26] = shc_op_sub_ci1,
    [0x29] = shc_op_inc, [0x2a] = shc_op_dec, [0x30] = shc_op_abs,
    [0x40] = shc_op_and, [0x41] = shc_op_or, [0x42] = shc_op_xor,
    [0x43] = shc_op_not, [0x61] = shc_op_min, [0x62] = shc_op_max,
    [0x63] = shc_op_clip, [0x81] = shc_op_fadd, [0x82] = shc_op_fsub,
    [0x89] = shc_op_favg, [0x8a] = shc_op_fcomp, [0x91] = shc_op_fadd_abs,
    [0x92] = shc_op_fsub_abs, [0xa1] = shc_op_fpass, [0xa2] = shc_op_fneg,
    [0xb0] = shc_op_fabs, [0xbd] = shc_op_scalb, [0xc1] = shc_op_logb,
    [0xc4] = shc_op_recips, [0xc5] = shc_op_rsqrts, [0xc9] = shc_op_fix,
    [0xca] = shc_op_float, [0xd9] = shc_op_fix_scaled, [0xda] = shc_op_float_scaled,
    [0xe0] = shc_op_fcopysign, [0xe1] = shc_op_fmin, [0xe2] = shc_op_fmax,
    [0xe3] = shc_op_fclip,
};

static void shc_alu(uint32_t opcode)
{
    int const op = (int)((opcode >> 12) & 0xff);
    int const rn = (int)((opcode >> 8) & 0xf);
    int const rx = (int)((opcode >> 4) & 0xf);
    int const ry = (int)(opcode & 0xf);
    if ((op & 0x70) == 0x70) {      /* 0x7X dual add/sub, 0xFX float dual */
        if (op & 0x80)
            shc_dual_fadd_fsub(rn, op & 0xf, rx, ry);
        else
            shc_dual_add_sub(rn, op & 0xf, rx, ry);
        return;
    }
    if (!shc_alu_tab[op]) {
        shc_fatal("unimplemented ALU operation", (uint32_t)op);
        return;
    }
    shc_alu_tab[op](rn, rx, ry);
}

/* ---- Multiplier ------------------------------------------------------------ */

static void shc_mul_flags(uint64_t r)
{
    uint32_t const hi = (uint32_t)(r >> 32), lo = (uint32_t)r;
    shc_set_mn(lo);
    if (hi != 0 && hi != 0xffffffffu)
        g_shc.astat |= SHC_MV;
    if (hi == 0 && lo != 0)
        g_shc.astat |= SHC_MU;
}

static inline int64_t shc_smul(uint32_t a, uint32_t b)
{
    return (int64_t)(int32_t)a * (int64_t)(int32_t)b;
}

/* round a fractional MR to nearest (ties to even) at bit 32 */
static inline uint64_t shc_round_mr(uint64_t r)
{
    uint64_t const t = r + 0x80000000u;
    return ((uint32_t)r == 0x80000000u) ? (t & ~(uint64_t)0x1ffffffffull) : t;
}

/* MAME compute_mul_fixed: op bit 2 -> MR (bit 1 MRB), bits 7-6 pick
 * SAT/RND (0), Rx*Ry (1), MR + (2), MR - (3); bit 3 fractional, bit 0 round.
 * Both operands are sign-extended whatever bits 4-5 say (REG is int32_t). */
static uint64_t shc_mul_fixed_value(int op, uint64_t mr, bool frac, bool *bad)
{
    if ((op >> 6) == 0) {
        int const k = (op >> 4) & 3;
        if (k == 0) {
            if (frac)
                return mr;
            if (op & 1) {
                int64_t const v = (int64_t)mr;
                return (uint64_t)(v < INT32_MIN ? INT32_MIN : (v > INT32_MAX ? INT32_MAX : v));
            }
            return (mr >> 32) ? 0xffffffffu : mr;
        }
        if (k == 1) {
            uint64_t r = frac ? shc_round_mr(mr) : 0;
            if ((op & 4) && frac)
                r &= ~(uint64_t)0xffffffffu;
            return r;
        }
        *bad = true;
        return 0;
    }
    return 0;   /* the product forms are done by the caller */
}

/* Rx*Ry, MR + Rx*Ry, MR - Rx*Ry (op bits 7-6 = 1..3) */
static uint64_t shc_mul_fixed_product(int op, uint64_t mr, bool frac, int rx, int ry)
{
    uint64_t p = (uint64_t)shc_smul(SHC_X, SHC_Y);
    if (frac)
        p <<= 1;
    uint64_t r = (op >> 6) == 1 ? p : ((op >> 6) == 2 ? mr + p : mr - p);
    if (frac && (op & 1))
        r = shc_round_mr(r);
    if ((op & 4) && frac && (op & 1))
        r &= ~(uint64_t)0xffffffffu;
    return r;
}

/* a fixed multiplier result to Rn, with MN / MV / MU */
static void shc_mul_fixed_out(int rn, uint64_t r, bool frac)
{
    uint32_t const out = frac ? (uint32_t)(r >> 32) : (uint32_t)r;
    uint32_t const hi = (uint32_t)(r >> 32);
    shc_set_mn(out);
    if (!frac && hi != 0 && hi != 0xffffffffu)
        g_shc.astat |= SHC_MV;
    if (frac && hi == 0 && (uint32_t)r != 0)
        g_shc.astat |= SHC_MU;
    g_shc.r[rn] = out;
}

static void shc_mul_fixed(int op, int rn, int rx, int ry)
{
    bool const to_mr = (op & 4) != 0, frac = (op & 8) != 0;
    uint64_t *mr = (op & 2) ? &g_shc.mrb : &g_shc.mrf;
    uint64_t r;
    if ((op >> 6) == 0) {
        bool bad = false;
        r = shc_mul_fixed_value(op, *mr, frac, &bad);
        if (bad) {
            shc_fatal("multiplier operation not implemented", (uint32_t)op);
            return;
        }
    } else {
        r = shc_mul_fixed_product(op, *mr, frac, rx, ry);
    }
    shc_mul_clear();
    if (to_mr) {
        *mr = r;
        shc_set_mn((uint32_t)(r >> 32));
        return;
    }
    shc_mul_fixed_out(rn, r, frac);
}

static void shc_mul(uint32_t opcode)
{
    int const op = (int)((opcode >> 12) & 0xff);
    int const rn = (int)((opcode >> 8) & 0xf);
    int const rx = (int)((opcode >> 4) & 0xf);
    int const ry = (int)(opcode & 0xf);
    uint64_t r;
    switch (op) {
    case 0x14: case 0x15: g_shc.mrf = 0; return;
    case 0x16: case 0x17: g_shc.mrb = 0; return;
    case 0x30:
        shc_mul_clear();
        g_shc.r[rn] = shc_fmul_bits(SHC_X, SHC_Y);
        return;
    case 0x40: r = (uint64_t)SHC_X * (uint64_t)SHC_Y; break;
    case 0x70: r = (uint64_t)shc_smul(SHC_X, SHC_Y); break;
    case 0xb0: r = g_shc.mrf + (uint64_t)shc_smul(SHC_X, SHC_Y); break;
    case 0xb2: r = g_shc.mrb + (uint64_t)shc_smul(SHC_X, SHC_Y); break;
    default:
        if (op >= 0x40 || op < 0x20)
            shc_mul_fixed(op, rn, rx, ry);
        else
            shc_fatal("multiplier operation not implemented", (uint32_t)op);
        return;
    }
    shc_mul_clear();
    shc_mul_flags(r);
    g_shc.r[rn] = (uint32_t)r;
}

/* ---- Shifter --------------------------------------------------------------- */

typedef void (*shc_shift_fn)(int rn, int rx, uint32_t p);

static inline uint32_t shc_lsh(uint32_t x, int s)
{
    if (s < 0)
        return s > -32 ? x >> -s : 0;
    return s < 32 ? x << s : 0;
}

static inline void shc_sv_if(bool c) { if (c) g_shc.astat |= SHC_SV; }

static void shc_sh_lshift(int rn, int rx, int s)
{
    g_shc.r[rn] = shc_lsh(g_shc.r[rx], s);
    shc_sv_if(s > 0);
    shc_set_sz(g_shc.r[rn]);
}

static void shc_sh_lshift_ry(int rn, int rx, uint32_t p) { shc_sh_lshift(rn, rx, (int32_t)p); }
static void shc_sh_lshift_imm(int rn, int rx, uint32_t p) { shc_sh_lshift(rn, rx, (int8_t)(p & 0xff)); }

static void shc_sh_ashift_imm(int rn, int rx, uint32_t p)
{
    int const s = (int8_t)(p & 0xff);
    uint32_t const x = g_shc.r[rx];
    if (s < 0)
        g_shc.r[rn] = shc_asr(x, s > -32 ? -s : 31);
    else
        g_shc.r[rn] = s < 32 ? x << s : 0;
    shc_sv_if(s > 0);
    shc_set_sz(g_shc.r[rn]);
}

static void shc_sh_rot_ry(int rn, int rx, uint32_t p)
{
    g_shc.r[rn] = shc_rotl(g_shc.r[rx], (int32_t)p);
    shc_sv_if((int32_t)p > 0);
    shc_set_sz(g_shc.r[rn]);
}

static void shc_sh_rot_imm(int rn, int rx, uint32_t p)
{
    g_shc.r[rn] = shc_rotl(g_shc.r[rx], (int8_t)(p & 0xff));
    shc_set_sz(g_shc.r[rn]);
}

/* Rn = Rn OR LSHIFT Rx BY Ry: MAME shifts the int32 (arithmetic right) */
static void shc_sh_or_lshift_ry(int rn, int rx, uint32_t p)
{
    int const s = (int8_t)(p & 0xff);
    uint32_t const x = g_shc.r[rx];
    if (s < 0)
        g_shc.r[rn] |= s > -32 ? shc_asr(x, -s) : 0;
    else
        g_shc.r[rn] |= s < 32 ? x << s : 0;
    shc_sv_if(s > 0);
    shc_set_sz(g_shc.r[rn]);
}

/* the immediate form shifts logically and sets SZ from the shifted value */
static void shc_sh_or_lshift_imm(int rn, int rx, uint32_t p)
{
    int const s = (int8_t)(p & 0xff);
    uint32_t const r = shc_lsh(g_shc.r[rx], s);
    shc_sv_if(s > 0);
    shc_set_sz(r);
    g_shc.r[rn] |= r;
}

#define SHC_FBIT(p) ((int)((p) & 0x3f))
#define SHC_FLEN(p) ((int)(((p) >> 6) & 0x3f))

static void shc_sh_fext(int rn, int rx, uint32_t p)
{
    int const bit = SHC_FBIT(p), len = SHC_FLEN(p);
    g_shc.r[rn] = (len == 0 || bit >= 32) ? 0 : shc_asr(g_shc.r[rx], bit) & shc_mask(len);
    shc_set_sz(g_shc.r[rn]);
    shc_sv_if(bit + len > 32);
}

static void shc_sh_fdep(int rn, int rx, uint32_t p)
{
    int const bit = SHC_FBIT(p), len = SHC_FLEN(p);
    g_shc.r[rn] = (len == 0 || bit >= 32) ? 0 : (g_shc.r[rx] & shc_mask(len)) << bit;
    shc_set_sz(g_shc.r[rn]);
    shc_sv_if(bit + len > 32);
}

static void shc_sh_fext_se(int rn, int rx, uint32_t p)
{
    int const bit = SHC_FBIT(p), len = SHC_FLEN(p);
    uint32_t const x = g_shc.r[rx];
    if (len == 0 || bit >= 32)
        g_shc.r[rn] = 0;
    else if (bit + len > 32)
        g_shc.r[rn] = x >> bit;
    else
        g_shc.r[rn] = shc_sext(shc_asr(x, bit), len);
    shc_set_sz(g_shc.r[rn]);
    shc_sv_if(bit + len > 32);
}

static void shc_sh_fdep_se(int rn, int rx, uint32_t p)
{
    int const bit = SHC_FBIT(p), len = SHC_FLEN(p);
    g_shc.r[rn] = (len == 0 || bit >= 32) ? 0 : shc_sext(g_shc.r[rx], len) << bit;
    shc_set_sz(g_shc.r[rn]);
    shc_sv_if(bit + len > 32);
}

static void shc_sh_or_fdep(int rn, int rx, uint32_t p)
{
    int const bit = SHC_FBIT(p), len = SHC_FLEN(p);
    if (len != 0 && bit < 32)
        g_shc.r[rn] |= (g_shc.r[rx] & shc_mask(len)) << bit;
    shc_set_sz(g_shc.r[rn]);
    shc_sv_if(bit + len > 32);
}

/* BSET / BCLR / BTGL (k = 0, 1, 2) */
static void shc_sh_bitop(int rn, int rx, uint32_t p, int k)
{
    uint32_t const x = g_shc.r[rx];
    uint32_t const b = p < 32 ? 1u << p : 0;
    if (p >= 32)
        g_shc.astat |= SHC_SV;
    g_shc.r[rn] = k == 0 ? (x | b) : (k == 1 ? (x & ~b) : (x ^ b));
    shc_set_sz(g_shc.r[rn]);
}

static void shc_sh_bset(int rn, int rx, uint32_t p) { shc_sh_bitop(rn, rx, p, 0); }
static void shc_sh_bclr(int rn, int rx, uint32_t p) { shc_sh_bitop(rn, rx, p, 1); }
static void shc_sh_btgl(int rn, int rx, uint32_t p) { shc_sh_bitop(rn, rx, p, 2); }

static void shc_sh_btst(int rn, int rx, uint32_t p)
{
    (void)rn;
    if (p < 32)
        shc_set_sz(g_shc.r[rx] & (1u << p));
    else
        g_shc.astat |= SHC_SZ | SHC_SV;
}

/* Rn = shift Rx BY Ry, indexed by op >> 2 */
static const shc_shift_fn shc_shift_ry_tab[64] = {
    [0x00] = shc_sh_lshift_ry, [0x02] = shc_sh_rot_ry, [0x08] = shc_sh_or_lshift_ry,
    [0x10] = shc_sh_fext, [0x12] = shc_sh_fext_se, [0x19] = shc_sh_or_fdep,
    [0x30] = shc_sh_bset, [0x31] = shc_sh_bclr, [0x33] = shc_sh_btst,
};

/* Rn = shift Rx BY <data>, indexed by the shift opcode */
static const shc_shift_fn shc_shift_imm_tab[64] = {
    [0x00] = shc_sh_lshift_imm, [0x01] = shc_sh_ashift_imm, [0x02] = shc_sh_rot_imm,
    [0x08] = shc_sh_or_lshift_imm, [0x10] = shc_sh_fext, [0x11] = shc_sh_fdep,
    [0x12] = shc_sh_fext_se, [0x13] = shc_sh_fdep_se, [0x19] = shc_sh_or_fdep,
    [0x30] = shc_sh_bset, [0x31] = shc_sh_bclr, [0x32] = shc_sh_btgl,
    [0x33] = shc_sh_btst,
};

static void shc_shift(uint32_t opcode)
{
    int const op = (int)((opcode >> 14) & 0x3f);
    shc_shift_fn const f = shc_shift_ry_tab[op];
    g_shc.astat &= ~(uint32_t)(SHC_SZ | SHC_SV | SHC_SS);
    if (!f) {
        shc_fatal("shift operation not implemented", (uint32_t)op);
        return;
    }
    f((int)((opcode >> 8) & 0xf), (int)((opcode >> 4) & 0xf), g_shc.r[opcode & 0xf]);
}

static void shc_shift_imm(int shiftop, uint32_t data, int rn, int rx)
{
    shc_shift_fn const f = shc_shift_imm_tab[shiftop & 0x3f];
    g_shc.astat &= ~(uint32_t)(SHC_SZ | SHC_SV | SHC_SS);
    if (!f) {
        shc_fatal("unimplemented shift operation", (uint32_t)shiftop);
        return;
    }
    f(rn, rx, data);
}

/* ---- Multi-function -------------------------------------------------------- */

/* the register fields of a multi-function compute */
typedef struct { int fm, fa, fxm, fym, fxa, fya; } shc_mf_t;

static shc_mf_t shc_mf_fields(uint32_t op)
{
    shc_mf_t f;
    f.fm = (int)((op >> 12) & 0xf);
    f.fa = (int)((op >> 8) & 0xf);
    f.fxm = (int)((op >> 6) & 3);
    f.fym = (int)((op >> 4) & 3) + 4;
    f.fxa = (int)((op >> 2) & 3) + 8;
    f.fya = (int)(op & 3) + 12;
    return f;
}

static void shc_mf_mr_to_reg(uint32_t op)
{
    int const ai = (int)((op >> 12) & 0xf), rk = (int)((op >> 8) & 0xf);
    switch (ai) {
    case 0: shc_set_ureg(rk, (uint32_t)g_shc.mrf); break;
    case 1: shc_set_ureg(rk, (uint32_t)(g_shc.mrf >> 32)); break;
    case 4: shc_set_ureg(rk, (uint32_t)g_shc.mrb); break;
    case 5: shc_set_ureg(rk, (uint32_t)(g_shc.mrb >> 32)); break;
    default: shc_fatal("mr_to_reg: unknown ai", (uint32_t)ai); break;
    }
    shc_mul_clear();
}

static void shc_mf_reg_to_mr(uint32_t op)
{
    int const ai = (int)((op >> 12) & 0xf), rk = (int)((op >> 8) & 0xf);
    uint64_t const v = shc_get_ureg(rk);
    switch (ai) {
    case 0: g_shc.mrf = (g_shc.mrf & ~(uint64_t)0xffffffffu) | v; break;
    case 1: g_shc.mrf = (g_shc.mrf & 0xffffffffu) | (v << 32); break;
    case 4: g_shc.mrb = (g_shc.mrb & ~(uint64_t)0xffffffffu) | v; break;
    case 5: g_shc.mrb = (g_shc.mrb & 0xffffffffu) | (v << 32); break;
    default: shc_fatal("reg_to_mr: unknown ai", (uint32_t)ai); break;
    }
    shc_mul_clear();
}

/* Rm = Rxm * Rym (SSFR), Ra = Rxa +/- Rya */
static void shc_mf_ssfr(uint32_t op)
{
    shc_mf_t const f = shc_mf_fields(op);
    bool const sub = ((op >> 16) & 0x3f) == 0x05;
    uint32_t const xa = g_shc.r[f.fxa], ya = g_shc.r[f.fya];
    uint32_t const r_mul = (uint32_t)(shc_smul(g_shc.r[f.fxm], g_shc.r[f.fym]) >> 31);
    uint32_t const r_alu = sub ? xa - ya : xa + ya;
    shc_mul_clear();
    shc_set_mn(r_mul);
    shc_alu_clear();
    shc_set_an(r_alu);
    shc_set_az(r_alu);
    if (sub) {
        shc_av_sub(r_alu, xa, ya);
        shc_ac_sub(r_alu, xa);
    } else {
        shc_av_add(r_alu, xa, ya);
        shc_ac_add(r_alu, xa);
    }
    g_shc.r[f.fm] = r_mul;
    g_shc.r[f.fa] = r_alu;
    g_shc.astat &= ~(uint32_t)SHC_AF;
}

/* the ALU half of Fm = Fxm * Fym, Fa = <op>; 0x18-0x1F */
static uint32_t shc_mf_alu(int multiop, const shc_mf_t *f)
{
    uint32_t const xa = g_shc.r[f->fxa], ya = g_shc.r[f->fya];
    switch (multiop) {
    case 0x18: return shc_faddsub(xa, ya, 0);
    case 0x19: return shc_faddsub(xa, ya, 1);
    case 0x1c: return shc_faddsub(xa, ya, 2);
    case 0x1d: return shc_fabs_bits(xa);
    case 0x1e: return shc_fmax_bits(xa, ya);
    default:   return shc_fmin_bits(xa, ya);
    }
}

static void shc_mf_fmul_alu(uint32_t op)
{
    shc_mf_t const f = shc_mf_fields(op);
    int const multiop = (int)((op >> 16) & 0x3f);
    shc_mul_clear();
    shc_alu_clear();
    uint32_t const r_mul = shc_fmul_bits(g_shc.r[f.fxm], g_shc.r[f.fym]);
    uint32_t const r_alu = shc_mf_alu(multiop, &f);
    g_shc.r[f.fm] = r_mul;
    g_shc.r[f.fa] = r_alu;
}

/* Fm = Fxm * Fym, Fa = FLOAT Rxa BY Rya */
static void shc_mf_fmul_float_scaled(uint32_t op)
{
    shc_mf_t const f = shc_mf_fields(op);
    shc_mul_clear();
    shc_alu_clear();
    uint32_t const r_mul = shc_fmul_bits(g_shc.r[f.fxm], g_shc.r[f.fym]);
    uint32_t const x = shc_f2u((float)(int32_t)g_shc.r[f.fxa]);
    uint32_t const r_alu = shc_scalb(x, f.fya);
    if (shc_u2f(r_alu) < 0.0f)
        g_shc.astat |= SHC_AN;
    if (shc_is_doz(r_alu))
        g_shc.astat |= SHC_AZ;
    if (shc_is_denorm(r_alu))
        g_shc.s.stky |= SHC_AUS;
    g_shc.r[f.fm] = r_mul;
    g_shc.r[f.fa] = r_alu;
    g_shc.astat |= SHC_AF;
}

/* Fm = Fxm * Fym, Ra = FIX Fxa BY Rya */
static void shc_mf_fmul_fix_scaled(uint32_t op)
{
    shc_mf_t const f = shc_mf_fields(op);
    shc_mul_clear();
    shc_alu_clear();
    uint32_t const r_mul = shc_fmul_bits(g_shc.r[f.fxm], g_shc.r[f.fym]);
    uint32_t const src = g_shc.r[f.fxa];
    uint32_t const r_alu = shc_scalb(src, f.fya);
    g_shc.r[f.fm] = r_mul;
    shc_fix_out(f.fa, r_alu, src, false);
}

/* Fm = Fxm * Fym, Fa = Fxa + Fya, Fs = Fxa - Fya */
static void shc_mf_fmul_dual(uint32_t op)
{
    shc_mf_t const f = shc_mf_fields(op);
    uint32_t ra, rs;
    shc_mul_clear();
    shc_alu_clear();
    uint32_t const r_mul = shc_fmul_bits(g_shc.r[f.fxm], g_shc.r[f.fym]);
    shc_faddfsub(g_shc.r[f.fxa], g_shc.r[f.fya], &ra, &rs);
    g_shc.r[f.fm] = r_mul;
    g_shc.r[f.fa] = ra;
    g_shc.r[(op >> 16) & 0xf] = rs;
}

typedef void (*shc_mf_fn)(uint32_t op);

static const shc_mf_fn shc_mf_tab[0x30] = {
    [0x00] = shc_mf_mr_to_reg, [0x01] = shc_mf_reg_to_mr,
    [0x04] = shc_mf_ssfr, [0x05] = shc_mf_ssfr,
    [0x18] = shc_mf_fmul_alu, [0x19] = shc_mf_fmul_alu,
    [0x1a] = shc_mf_fmul_float_scaled, [0x1b] = shc_mf_fmul_fix_scaled,
    [0x1c] = shc_mf_fmul_alu, [0x1d] = shc_mf_fmul_alu,
    [0x1e] = shc_mf_fmul_alu, [0x1f] = shc_mf_fmul_alu,
};

static void shc_multi(uint32_t op)
{
    int const multiop = (int)((op >> 16) & 0x3f);
    if (multiop >= 0x30) {
        shc_mf_fmul_dual(op);
        return;
    }
    if (!shc_mf_tab[multiop]) {
        shc_fatal("multi-function opcode not implemented", (uint32_t)multiop);
        return;
    }
    shc_mf_tab[multiop](op);
}

/* MAME COMPUTE: the 23-bit compute field */
static void shc_compute(uint32_t opcode)
{
    if (opcode & 0x400000) {
        shc_multi(opcode);
        return;
    }
    switch ((opcode >> 20) & 3) {
    case 0: shc_alu(opcode); break;
    case 1: shc_mul(opcode); break;
    case 2: shc_shift(opcode); break;
    default: shc_fatal("invalid single-function operation", opcode); break;
    }
}

#undef SHC_X
#undef SHC_Y
#undef SHC_CARRY

/* ---- Instructions (MAME sharcops.hxx) -------------------------------------- */

#define SHC_COND(op)    ((unsigned)((op) >> 33) & 0x1f)
#define SHC_COMPUTE(op) ((uint32_t)(op) & 0x7fffff)
#define SHC_BIT(op, n)  ((int)((op) >> (n)) & 1)
#define SHC_F3(op, n)   ((int)((op) >> (n)) & 7)

typedef void (*shc_op_fn)(uint64_t op);

static inline int32_t shc_reladdr(uint64_t op) { return (int32_t)shc_sext((uint32_t)(op >> 27) & 0x3f, 6); }
static inline int32_t shc_rel24(uint64_t op) { return (int32_t)shc_sext((uint32_t)op & 0xffffff, 24); }
static inline void shc_do_compute(uint32_t c) { if (c) shc_compute(c); }
static inline shc_dag_t *shc_dag(int pm) { return pm ? &g_shc.dag2 : &g_shc.dag1; }

/* UPDATE_CIRCULAR_BUFFER: the buffer is [B, B+L) */
static void shc_postmod(shc_dag_t *d, int i, uint32_t mod)
{
    d->i[i] += mod;
    if (d->l[i] == 0)
        return;
    if (d->i[i] >= d->b[i] + d->l[i])
        d->i[i] -= d->l[i];
    else if (d->i[i] < d->b[i])
        d->i[i] += d->l[i];
}

/* a DM or PM data access of a dreg/ureg; DM external halves are done already */
static void shc_xfer_dreg(int pm, uint32_t a, int wr, int dreg, uint32_t v)
{
    if (wr)
        pm ? shc_pm_wr32(a, v) : shc_dm_wr(a, v);
    else
        g_shc.r[dreg] = pm ? shc_pm_rd32(a) : shc_dm_rd(a);
}

static void shc_xfer_ureg(int pm, uint32_t a, int wr, int ureg, uint32_t v)
{
    if (pm && ureg == 0xdb) {               /* PX through PM is always 48-bit */
        if (wr)
            shc_pm_wr48(a, g_shc.px);
        else
            g_shc.px = shc_pm_rd48(a);
    } else if (wr) {
        pm ? shc_pm_wr32(a, v) : shc_dm_wr(a, v);
    } else {
        shc_set_ureg(ureg, pm ? shc_pm_rd32(a) : shc_dm_rd(a));
    }
}

/* a jump that changes nothing but the PC to itself is a busy-wait */
static void shc_goto(int delayed, uint32_t target, bool pure)
{
    if (!delayed && pure && target == g_shc.pc)
        g_shc.spin = 1;
    shc_jump(delayed != 0, target);
}

/* 001: compute / dreg <-> DM / dreg <-> PM */
static void shc_op_dm_pm(uint64_t op)
{
    int const pm_dreg = (int)(op >> 23) & 0xf, dm_dreg = (int)(op >> 33) & 0xf;
    int const pmm = SHC_F3(op, 27), pmi = SHC_F3(op, 30);
    int const dmm = SHC_F3(op, 38), dmi = SHC_F3(op, 41);
    int const pmd = SHC_BIT(op, 37), dmd = SHC_BIT(op, 44);
    uint32_t const pv = g_shc.r[pm_dreg], dv = g_shc.r[dm_dreg];
    if (!shc_ext(g_shc.dag1.i[dmi], dmd, dv))
        return;
    shc_do_compute(SHC_COMPUTE(op));
    shc_xfer_dreg(1, g_shc.dag2.i[pmi], pmd, pm_dreg, pv);
    shc_postmod(&g_shc.dag2, pmi, g_shc.dag2.m[pmm]);
    shc_xfer_dreg(0, g_shc.dag1.i[dmi], dmd, dm_dreg, dv);
    shc_postmod(&g_shc.dag1, dmi, g_shc.dag1.m[dmm]);
}

/* 000 00001: compute */
static void shc_op_compute(uint64_t op)
{
    if (shc_if_cond(SHC_COND(op)))
        shc_do_compute(SHC_COMPUTE(op));
}

/* 010 0/1: compute / ureg <-> DM|PM, pre- (bit 44 clear) or post-modify */
static void shc_op_ureg_dmpm(uint64_t op)
{
    int const i = SHC_F3(op, 41), m = SHC_F3(op, 38);
    int const g = SHC_BIT(op, 32), d = SHC_BIT(op, 31), post = SHC_BIT(op, 44);
    int const ureg = (int)(op >> 23) & 0xff;
    if (!shc_if_cond(SHC_COND(op)))
        return;
    uint32_t const pu = shc_get_ureg(ureg);
    shc_dag_t *dag = shc_dag(g);
    uint32_t const a = post ? dag->i[i] : dag->i[i] + dag->m[m];
    if (!g && !shc_ext(a, d, pu))
        return;
    shc_do_compute(SHC_COMPUTE(op));
    shc_xfer_ureg(g, a, d, ureg, pu);
    if (post)
        shc_postmod(dag, i, dag->m[m]);
}

/* 0110: compute / dreg <-> DM|PM, immediate modify (bit 39 write, bit 40 PM) */
static void shc_op_immmod(uint64_t op)
{
    int const wr = SHC_BIT(op, 39), pm = SHC_BIT(op, 40);
    int const u = SHC_BIT(op, 38), i = SHC_F3(op, 41);
    int const dreg = (int)(op >> 23) & 0xf;
    int32_t const mod = shc_reladdr(op);
    uint32_t const v = g_shc.r[dreg];
    if (!shc_if_cond(SHC_COND(op)))
        return;
    shc_dag_t *dag = shc_dag(pm);
    uint32_t const a = u ? dag->i[i] : dag->i[i] + (uint32_t)mod;
    if (!pm && !shc_ext(a, wr, v))
        return;
    shc_do_compute(SHC_COMPUTE(op));
    shc_xfer_dreg(pm, a, wr, dreg, v);
    if (u)
        shc_postmod(dag, i, (uint32_t)mod);
}

/* 0111: compute / ureg <-> ureg */
static void shc_op_ureg_ureg(uint64_t op)
{
    if (!shc_if_cond((unsigned)(op >> 31) & 0x1f))
        return;
    uint32_t const v = shc_get_ureg((int)(op >> 36) & 0xff);
    shc_do_compute(SHC_COMPUTE(op));
    shc_set_ureg((int)(op >> 23) & 0xff, v);
}

static inline uint32_t shc_shift_data(uint64_t op)
{
    return (uint32_t)(((op >> 8) & 0xff) | ((op >> 19) & 0xf00));
}

static void shc_op_imm_shift_core(uint64_t op)
{
    shc_shift_imm((int)(op >> 16) & 0x3f, shc_shift_data(op), (int)(op >> 4) & 0xf, (int)op & 0xf);
}

/* 1000: immediate shift / dreg <-> DM|PM */
static void shc_op_imm_shift_dmpm(uint64_t op)
{
    int const i = SHC_F3(op, 41), m = SHC_F3(op, 38);
    int const g = SHC_BIT(op, 32), d = SHC_BIT(op, 31);
    int const dreg = (int)(op >> 23) & 0xf;
    if (!shc_if_cond(SHC_COND(op)))
        return;
    uint32_t const v = g_shc.r[dreg];
    shc_dag_t *dag = shc_dag(g);
    if (!g && !shc_ext(dag->i[i], d, v))
        return;
    shc_op_imm_shift_core(op);
    shc_xfer_dreg(g, dag->i[i], d, dreg, v);
    shc_postmod(dag, i, dag->m[m]);
}

/* 000 00010: immediate shift */
static void shc_op_imm_shift(uint64_t op)
{
    if (shc_if_cond(SHC_COND(op)))
        shc_op_imm_shift_core(op);
}

/* 000 00100: compute / modify */
static void shc_op_compute_modify(uint64_t op)
{
    int const g = SHC_BIT(op, 38), m = SHC_F3(op, 27), i = SHC_F3(op, 30);
    if (!shc_if_cond(SHC_COND(op)))
        return;
    shc_do_compute(SHC_COMPUTE(op));
    shc_dag_t *dag = shc_dag(g);
    shc_postmod(dag, i, dag->m[m]);
}

/* a call: pcstk is the return address */
static void shc_call(int delayed, uint32_t target, uint32_t ret)
{
    shc_push_pc();
    g_shc.s.pcstk = ret;
    shc_jump(delayed != 0, target);
}

/* jump's CI and LA bits */
static void shc_jump_ci(uint64_t op)
{
    if (SHC_BIT(op, 24))
        shc_clear_interrupt();
}
static void shc_jump_la(uint64_t op)
{
    if (SHC_BIT(op, 38)) {
        shc_pop_pc();
        shc_pop_loop();
    }
}
static inline bool shc_jump_pure(uint64_t op) { return !SHC_BIT(op, 24) && !SHC_BIT(op, 38); }

/* 000 00110 / 00111: direct (bit 40 relative) jump or call (bit 39) */
static void shc_op_direct(uint64_t op)
{
    int const j = SHC_BIT(op, 26), call = SHC_BIT(op, 39), rel = SHC_BIT(op, 40);
    if (!shc_if_cond(SHC_COND(op)))
        return;
    uint32_t const target = rel ? g_shc.pc + (uint32_t)shc_rel24(op) : (uint32_t)op & 0xffffff;
    if (call) {
        uint32_t ret;
        if (rel)
            ret = g_shc.pc + (j ? 3 : 1);
        else
            ret = j ? g_shc.nfaddr : g_shc.daddr;
        shc_call(j, target, ret);
        return;
    }
    shc_jump_ci(op);
    shc_jump_la(op);
    shc_goto(j, target, shc_jump_pure(op));
}

/* 000 01000 / 01001: indirect (bit 40 PC-relative) jump or call, with compute */
static uint32_t shc_indirect_target(uint64_t op)
{
    if (SHC_BIT(op, 40))
        return g_shc.pc + (uint32_t)shc_reladdr(op);
    return g_shc.dag2.i[SHC_F3(op, 30)] + g_shc.dag2.m[SHC_F3(op, 27)];
}

static void shc_op_indirect(uint64_t op)
{
    int const j = SHC_BIT(op, 26), e = SHC_BIT(op, 25), call = SHC_BIT(op, 39);
    uint32_t const compute = SHC_COMPUTE(op);
    if (!call)
        shc_jump_ci(op);
    bool const taken = shc_if_cond(SHC_COND(op));
    if (!taken) {
        if (e)
            shc_do_compute(compute);
        return;
    }
    if (!e)
        shc_do_compute(compute);
    uint32_t const target = shc_indirect_target(op);
    if (call) {
        shc_call(j, target, j ? g_shc.nfaddr : g_shc.daddr);
        return;
    }
    shc_jump_la(op);
    shc_goto(j, target, shc_jump_pure(op) && (e || compute == 0));
}

/* 110 / 111: indirect (bit 45 relative) jump, else compute / dreg <-> DM */
static void shc_op_jump_dm(uint64_t op)
{
    int const d = SHC_BIT(op, 44), dmi = SHC_F3(op, 41), dmm = SHC_F3(op, 38);
    int const dreg = (int)(op >> 23) & 0xf;
    if (shc_if_cond(SHC_COND(op))) {
        uint32_t const target = SHC_BIT(op, 45)
            ? g_shc.pc + (uint32_t)shc_reladdr(op)
            : g_shc.dag2.i[SHC_F3(op, 30)] + g_shc.dag2.m[SHC_F3(op, 27)];
        shc_goto(0, target, true);
        return;
    }
    uint32_t const v = g_shc.r[dreg];
    if (!shc_ext(g_shc.dag1.i[dmi], d, v))
        return;
    shc_do_compute(SHC_COMPUTE(op));
    shc_xfer_dreg(0, g_shc.dag1.i[dmi], d, dreg, v);
    shc_postmod(&g_shc.dag1, dmi, g_shc.dag1.m[dmm]);
}

/* RTS / RTI body: IF cond return [, compute] / IF ... ELSE compute */
static void shc_return(uint64_t op)
{
    int const j = SHC_BIT(op, 26), e = SHC_BIT(op, 25);
    uint32_t const compute = SHC_COMPUTE(op);
    if (!shc_if_cond(SHC_COND(op))) {
        if (e)
            shc_do_compute(compute);
        return;
    }
    if (!e)
        shc_do_compute(compute);
    shc_jump(j != 0, shc_pop_pc());
}

/* 000 01010 */
static void shc_op_rts(uint64_t op) { shc_return(op); }

/* 000 01011 */
static void shc_op_rti(uint64_t op)
{
    g_shc.irptl &= ~(1u << g_shc.active_irq);
    shc_return(op);
    if (g_shc.active_irq >= 6 && g_shc.active_irq <= 8)
        shc_pop_status();
    g_shc.interrupt_active = 0;
    shc_check_interrupts();
}

static void shc_do(uint32_t address, uint32_t code, uint32_t type)
{
    shc_push_pc();
    shc_push_loop();
    g_shc.s.pcstk = g_shc.pc + 1;
    g_shc.s.laddr.addr = address;
    g_shc.s.laddr.code = code;
    g_shc.s.laddr.type = type;
}

/* 000 01100 / 01101: DO <addr> UNTIL LCE, LCNTR immediate (or a ureg) */
static void shc_op_do_counter(uint64_t op)
{
    int32_t const offset = shc_rel24(op);
    int32_t const dist = offset < 0 ? -offset : offset;
    uint32_t const type = dist == 1 ? 1 : (dist == 2 ? 2 : 3);
    if (SHC_BIT(op, 40))
        g_shc.s.lcntr = shc_get_ureg((int)(op >> 32) & 0xff);
    else
        g_shc.s.lcntr = (uint16_t)(op >> 24);
    shc_do(g_shc.pc + (uint32_t)offset, 0xf, type);
}

/* 000 01110: DO <addr> UNTIL cond */
static void shc_op_do_until(uint64_t op)
{
    shc_do(g_shc.pc + (uint32_t)shc_rel24(op), SHC_COND(op), 0);
}

/* 000 100 G D: ureg <-> DM|PM, direct address */
static void shc_op_ureg_direct(uint64_t op)
{
    int const ureg = (int)(op >> 32) & 0xff, wr = SHC_BIT(op, 40), pm = SHC_BIT(op, 41);
    uint32_t const a = (uint32_t)op;
    uint32_t const v = wr ? shc_get_ureg(ureg) : 0;
    if (!pm && !shc_ext(a, wr, v))
        return;
    shc_xfer_ureg(pm, a, wr, ureg, v);
}

/* 101 G III D: ureg <-> DM|PM, I + offset */
static void shc_op_ureg_indirect(uint64_t op)
{
    int const ureg = (int)(op >> 32) & 0xff, wr = SHC_BIT(op, 40), pm = SHC_BIT(op, 44);
    int const i = SHC_F3(op, 41);
    uint32_t const offset = (pm && !wr) ? ((uint32_t)op & 0xffffff) : (uint32_t)op;
    uint32_t const a = shc_dag(pm)->i[i] + offset;
    uint32_t const v = wr ? shc_get_ureg(ureg) : 0;
    if (!pm && !shc_ext(a, wr, v))
        return;
    shc_xfer_ureg(pm, a, wr, ureg, v);
}

/* 1001: immediate data -> DM|PM */
static void shc_op_imm_dmpm(uint64_t op)
{
    int const i = SHC_F3(op, 41), m = SHC_F3(op, 38), g = SHC_BIT(op, 37);
    uint32_t const data = (uint32_t)op;
    shc_dag_t *dag = shc_dag(g);
    if (!g && !shc_ext(dag->i[i], 1, data))
        return;
    if (g)
        shc_pm_wr32(dag->i[i], data);
    else
        shc_dm_wr(dag->i[i], data);
    shc_postmod(dag, i, dag->m[m]);
}

/* 000 01111: immediate data -> ureg */
static void shc_op_imm_ureg(uint64_t op)
{
    shc_set_ureg((int)(op >> 32) & 0xff, (uint32_t)op);
}

/* 000 10100: BIT SET/CLR/TGL/TST/XOR sreg data */
static void shc_op_sysreg_bitop(uint64_t op)
{
    int const bop = (int)(op >> 37) & 7, sreg = 0x70 | ((int)(op >> 32) & 0xf);
    uint32_t const data = (uint32_t)op;
    uint32_t src = shc_get_ureg(sreg);
    bool btf;
    switch (bop) {
    case 0: src |= data; break;
    case 1: src &= ~data; break;
    case 2: src ^= data; break;
    case 4:
    case 5:
        btf = bop == 4 ? (src & data) == data : src == data;
        if (btf)
            g_shc.astat |= SHC_BTF;
        else
            g_shc.astat &= ~(uint32_t)SHC_BTF;
        break;
    default:
        shc_fatal("sysreg_bitop: invalid bitop", (uint32_t)bop);
        return;
    }
    shc_set_ureg(sreg, src);    /* MAME: a TST of ASTAT writes the old ASTAT back */
}

/* 000 10110 0: MODIFY (I, data) */
static void shc_op_modify(uint64_t op)
{
    shc_postmod(shc_dag(SHC_BIT(op, 38)), (int)(op >> 32) & 7, (uint32_t)op);
}

/* 000 10111: PUSH/POP LOOP, STS, PCSTK */
static void shc_op_push_pop(uint64_t op)
{
    if (op & 0x008000000000ull) shc_push_loop();
    if (op & 0x004000000000ull) shc_pop_loop();
    if (op & 0x002000000000ull) shc_push_status();
    if (op & 0x001000000000ull) shc_pop_status();
    if (op & 0x000800000000ull) shc_push_pc();
    if (op & 0x000400000000ull) shc_pop_pc();
}

static void shc_op_nop(uint64_t op) { (void)op; }

static void shc_op_idle(uint64_t op)
{
    (void)op;
    g_shc.daddr = g_shc.pc;
    g_shc.faddr = g_shc.pc + 1;
    g_shc.nfaddr = g_shc.pc + 2;
    g_shc.idle = 1;
}

static void shc_op_unimpl(uint64_t op)
{
    shc_fatal("unimplemented opcode", (uint32_t)(op >> 32));
}

/* MAME s_sharc_opcode_table: mask and bits on (index << 7) */
static const struct { uint16_t mask, bits; shc_op_fn fn; } shc_op_list[] = {
    { 0xe000, 0x2000, shc_op_dm_pm },          { 0xff00, 0x0100, shc_op_compute },
    { 0xe000, 0x4000, shc_op_ureg_dmpm },      { 0xf000, 0x6000, shc_op_immmod },
    { 0xf000, 0x7000, shc_op_ureg_ureg },      { 0xf000, 0x8000, shc_op_imm_shift_dmpm },
    { 0xff00, 0x0200, shc_op_imm_shift },      { 0xff00, 0x0400, shc_op_compute_modify },
    { 0xff00, 0x0600, shc_op_direct },         { 0xff00, 0x0700, shc_op_direct },
    { 0xff00, 0x0800, shc_op_indirect },       { 0xff00, 0x0900, shc_op_indirect },
    { 0xe000, 0xc000, shc_op_jump_dm },        { 0xe000, 0xe000, shc_op_jump_dm },
    { 0xff00, 0x0a00, shc_op_rts },            { 0xff00, 0x0b00, shc_op_rti },
    { 0xfe00, 0x0c00, shc_op_do_counter },     { 0xff00, 0x0e00, shc_op_do_until },
    { 0xfc00, 0x1000, shc_op_ureg_direct },    { 0xe000, 0xa000, shc_op_ureg_indirect },
    { 0xf000, 0x9000, shc_op_imm_dmpm },       { 0xff00, 0x0f00, shc_op_imm_ureg },
    { 0xff00, 0x1400, shc_op_sysreg_bitop },   { 0xff80, 0x1600, shc_op_modify },
    { 0xff00, 0x1700, shc_op_push_pop },       { 0xff80, 0x0000, shc_op_nop },
    { 0xff80, 0x0080, shc_op_idle },
};

static shc_op_fn shc_ops[512];

static void shc_build_ops(void)
{
    for (unsigned i = 0; i < 512; i++) {
        uint16_t const op = (uint16_t)(i << 7);
        shc_ops[i] = shc_op_unimpl;
        for (size_t k = 0; k < sizeof shc_op_list / sizeof shc_op_list[0]; k++) {
            if ((shc_op_list[k].mask & op) == shc_op_list[k].bits) {
                shc_ops[i] = shc_op_list[k].fn;
                break;
            }
        }
    }
}

/* ---- Execution ------------------------------------------------------------- */

/* MAME's loop-end check, before the instruction at the loop's last address */
static void shc_loop_end(void)
{
    shc_stacks_t *s = &g_shc.s;
    if (s->laddr.type != 0) {
        if (--s->curlcntr == 0) {
            shc_pop_loop();
            shc_pop_pc();
        } else {
            shc_change_pc(s->pcstk);
        }
        return;
    }
    if (g_shc.pc - s->pcstk > 2)
        g_shc.astat = g_shc.astat_old_old_old;
    if (shc_do_cond(s->laddr.code)) {
        shc_pop_loop();
        shc_pop_pc();
    } else {
        /* a one-instruction DO UNTIL over a NOP: waiting on its condition */
        if (s->pcstk == g_shc.pc && g_shc.opcode == 0)
            g_shc.spin = 1;
        shc_change_pc(s->pcstk);
    }
    g_shc.astat = g_shc.astat_old;
}

/* one instruction; SHC_STOP_BUDGET when the core can go on */
static int shc_step(void)
{
    uint32_t const save[8] = {
        g_shc.pc, g_shc.daddr, g_shc.faddr, g_shc.nfaddr,
        g_shc.astat, g_shc.astat_old, g_shc.astat_old_old, g_shc.astat_old_old_old,
    };
    shc_stacks_t save_s;
    bool loop;

    g_shc.pc = g_shc.daddr;
    g_shc.daddr = g_shc.faddr;
    g_shc.faddr = g_shc.nfaddr;
    g_shc.nfaddr++;
    g_shc.astat_old_old_old = g_shc.astat_old_old;
    g_shc.astat_old_old = g_shc.astat_old;
    g_shc.astat_old = g_shc.astat;
    g_shc.opcode = shc_pm_rd48(g_shc.pc);
    g_shc.stalled = 0;
    g_shc.spin = 0;

    loop = !(g_shc.s.stky & SHC_LSEM) && g_shc.pc == g_shc.s.laddr.addr;
    if (loop) {
        save_s = g_shc.s;
        shc_loop_end();
    }

    shc_ops[(g_shc.opcode >> 39) & 0x1ff](g_shc.opcode);

    if (g_shc.stalled) {            /* put the core back: retried next run */
        g_shc.pc = save[0]; g_shc.daddr = save[1]; g_shc.faddr = save[2]; g_shc.nfaddr = save[3];
        g_shc.astat = save[4]; g_shc.astat_old = save[5];
        g_shc.astat_old_old = save[6]; g_shc.astat_old_old_old = save[7];
        if (loop)
            g_shc.s = save_s;
        g_shc.spin = 0;
        return SHC_STOP_STALL;
    }

    if (g_shc.lat_cycles > 0 && --g_shc.lat_cycles <= 0)
        shc_latency_effect();
    g_shc.cycles++;

    if (g_shc.fault)
        return SHC_STOP_FAULT;
    if (g_shc.idle)
        return SHC_STOP_IDLE;
    if (g_shc.spin && g_shc.lat_cycles <= 0)
        return SHC_STOP_SPIN;
    return SHC_STOP_BUDGET;
}

/* Run up to max_cycles instructions; the number run. g_shc.stop says why it
 * stopped (SHC_STOP_*). A stalled access is not counted: it is retried. */
static inline int shc_run(int max_cycles)
{
    g_shc.cycles = 0;
    if (g_shc.fault) {
        g_shc.stop = SHC_STOP_FAULT;
        return 0;
    }
    if (g_shc.halted) {
        g_shc.stop = SHC_STOP_HALTED;
        return 0;
    }
    if (g_shc.irq_pending) {
        shc_check_interrupts();
        g_shc.idle = 0;
    }
    g_shc.stop = SHC_STOP_BUDGET;
    if (g_shc.idle) {
        g_shc.stop = SHC_STOP_IDLE;
        return 0;
    }
    while (g_shc.cycles < max_cycles) {
        int const why = shc_step();
        if (why != SHC_STOP_BUDGET) {
            g_shc.stop = (uint32_t)why;
            break;
        }
    }
    return g_shc.cycles;
}

/* ---- Board interface ------------------------------------------------------- */

/* power-on / reset (MAME device_reset, HOST boot); the core is held halted */
static inline void shc_reset(void)
{
    if (!shc_ops[0])
        shc_build_ops();
    memset(&g_shc, 0, sizeof g_shc);
    shc_dma_t *d = &g_shc.dma[6];
    d->int_index = 0x20000;
    d->int_modifier = 1;
    d->int_count = 0x100;
    d->ext_index = 0x400000;
    d->ext_modifier = 1;
    d->ext_count = 0x600;
    d->control = 0xa1;
    g_shc.pc = 0x20004;
    g_shc.daddr = 0x20005;
    g_shc.faddr = 0x20006;
    g_shc.nfaddr = 0x20007;
    g_shc.s.stky = SHC_PCEM | SHC_SSEM | SHC_LSEM;
    g_shc.imask = 0x0003;
    g_shc.s.pcstk = 0x00ffffff;
    g_shc.s.curlcntr = 0xffffffffu;
    shc_laddr_unpack(&g_shc.s.laddr, 0xffffffffu);
    g_shc.syscon = 0x00000010;
    g_shc.lat_reg = -1;
    g_shc.flag[0] = 1;              /* FIFO in: empty */
    g_shc.halted = 1;
}

/* one halfword of the HOST boot upload (MAME external_dma_write) */
static inline void shc_boot_write(uint32_t index, uint16_t halfword)
{
    shc_dma6_write(index, halfword);
}

/* release the core after the upload: it starts at 0x20005 */
static inline void shc_boot(void)
{
    g_shc.halted = 0;
}

/* FLAG0-3 inputs (Model 2B: FLAG0 = FIFO in empty, FLAG1 = FIFO out full) */
static inline void shc_set_flag_input(int n, int state)
{
    if (n < 0 || n > 3)
        return;
    if ((g_shc.mode2 >> (SHC_M2_FLGO_SHIFT + n)) & 1)
        LOG_WARN("sharc: flag %d input set while it is an output", n);
    g_shc.flag[n] = state ? 1u : 0u;
}

/* IRQ0-2 lines (MAME execute_set_input: IRQ n is interrupt 8 - n) */
static inline void shc_set_irq(int line, int state)
{
    if (line < 0 || line > 2)
        return;
    if (state)
        g_shc.irq_pending |= 1 << (8 - line);
    else
        g_shc.irq_pending &= ~(1 << (8 - line));
}

#undef SHC_COND
#undef SHC_COMPUTE
#undef SHC_BIT
#undef SHC_F3
#undef SHC_FBIT
#undef SHC_FLEN

#endif /* SHARC_CPU_H */
