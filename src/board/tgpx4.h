/*
 * tgpx4.h — the Model 2C coprocessor: a Fujitsu MB86235 "TGPx4" (board-level).
 *
 * Model 2C puts a TGPx4 where 2A has a TGP (tgp.h) and 2B a SHARC
 * (sharc_cpu.h). As on those boards the i960 uploads the program, so it is
 * emulated as a CPU running the game's own code; model2c_cop.h wires it to the
 * bus.
 *
 * The instruction set follows MAME's interpreter (devices/cpu/mb86235/
 * mb86235ops.cpp, BSD-3-Clause, Angelo Salese, ElSemi, Matthew Daniels), the
 * oracle, down to its FEA/FES and FLOG. Departures, each for the same frames on
 * every build or for what the chip does:
 *
 *   - A read of the empty input FIFO stalls the instruction: nothing it would
 *     write is written (registers, memory, the output FIFO, EO), and it runs
 *     again whole once a word has come. MAME lets the stalled pass write its
 *     dummy 0 and runs the instruction again, which pushes a reply twice when
 *     the instruction also writes FO. Its stalls are rare (the i960 has usually
 *     pushed the whole command first); here the program waits on every word.
 *   - A conditional branch on IFE (input FIFO empty) or OFF (output FIFO full)
 *     that finds it true ends the run after its instruction: it is the
 *     program's wait loop, and nothing it does changes until the i960 moves.
 *   - Float to int, int overflow, shifts and NaN bits have x86's answers spelt
 *     out (tgp.h helpers), and FLOG's logarithm is computed here, not by libm.
 *
 * Program RAM is 0x1000 64-bit words; the A and B buses each have 0x400 words
 * inside the chip, and an A-bus address from 0x400 up goes out to the external
 * bus at (EB & 0xFFC000) + (addr & 0x3FFF), word addressed:
 *
 *   0x400000..0x7FFFFF  buffer RAM (i960 0x900000), word & 0x7FFF
 *   0x800000..0x9FFFFF  copro_data ROM
 *
 * Every byte of state is in g_tgx, POD, saved as a savestate extra.
 */
#ifndef TGPX4_H
#define TGPX4_H

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "log.h"
#include "tgp.h"        /* tgp_u2f, tgp_f2u, tgp_f2i */

/* Status bits (mb86235ops.cpp). */
enum {
    TGX_AD = 0x00000001, TGX_AU = 0x00000002, TGX_AV = 0x00000004,
    TGX_AZ = 0x00000008, TGX_AN = 0x00000010,
    TGX_ZC = 0x00000100, TGX_IL = 0x00000200, TGX_NR = 0x00000400, TGX_ZD = 0x00000800,
    TGX_RP = 0x00004000, TGX_LP = 0x00008000,
    TGX_MD = 0x00010000, TGX_MU = 0x00020000, TGX_MV = 0x00040000,
    TGX_MZ = 0x00080000, TGX_MN = 0x00100000,
    TGX_F0 = 0x10000000, TGX_F1 = 0x20000000, TGX_F2 = 0x40000000,
};

#define TGX_PROG_WORDS 0x1000u
#define TGX_FIFO       1024u      /* a power of 2; deeper than the board's 8 */
#define TGX_FIFO_BOARD 8u         /* what IFF / OFF test against */
/* The most instructions one i960 access lets the TGPx4 run (a program that
 * never waits gives the i960 its turn back after this). */
#define TGX_RUN_MAX    2000000u

typedef struct {
    /* CPU */
    uint32_t pc, ppc, delay_pc;
    uint32_t aa[8], ab[8], ma[8], mb[8], ar[8];
    uint32_t sp, eb, eo, rpc, lpc, prp, pwp, pr[24];
    uint32_t mod, st, pcp, pcs[4], pdr, ddr;
    bool     delay_slot;
    bool     stall;               /* this instruction read the empty FIFO */
    bool     wait;                /* this instruction polled an IFE / OFF wait */
    bool     halted;              /* held by the board until a program boots */

    uint64_t prog[TGX_PROG_WORDS];
    uint32_t ram_a[0x400], ram_b[0x400];

    /* FIFOs and the board's control word (model2c_cop.h) */
    uint32_t fin[TGX_FIFO], fin_r, fin_n;
    uint32_t fout[TGX_FIFO], fout_r, fout_n;
    uint32_t coproctl, coprocnt;

    /* Counters for the log */
    uint64_t insns;
    uint32_t pushes, pops, empty_pops, overflows, budget_stops, bad_ops;
} tgx_state_t;

static tgx_state_t g_tgx;

/* Set by the 2C install; outside g_tgx so a reset keeps them. */
static const uint8_t *g_tgx_rom      = NULL;   /* copro_data, LE words */
static uint32_t       g_tgx_rom_size = 0;      /* bytes */
static uint8_t       *g_tgx_bufram   = NULL;   /* i960 0x900000, LE words */

static inline void tgx_bad(const char *what, uint32_t v) {
    if (g_tgx.bad_ops++ < 16) LOG_WARN("TGPx4: %s %02X at pc=%03X", what, v, g_tgx.ppc);
}

/* ---- Buses --------------------------------------------------------------- */

static inline uint32_t tgx_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint32_t tgx_ext_rd(uint32_t addr) {
    addr &= 0xFFFFFFu;
    if (addr >= 0x400000u && addr < 0x800000u)
        return g_tgx_bufram ? tgx_le32(g_tgx_bufram + (addr & 0x7FFFu) * 4u) : 0u;
    if (addr >= 0x800000u && addr < 0xA00000u) {
        uint32_t off = (addr - 0x800000u) * 4u;
        return g_tgx_rom && off + 4u <= g_tgx_rom_size ? tgx_le32(g_tgx_rom + off) : 0u;
    }
    return 0u;
}

static inline void tgx_ext_wr(uint32_t addr, uint32_t v) {
    if (g_tgx.stall) return;
    addr &= 0xFFFFFFu;
    if (addr >= 0x400000u && addr < 0x800000u && g_tgx_bufram) {
        uint8_t *p = g_tgx_bufram + (addr & 0x7FFFu) * 4u;
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
    }
}

static inline uint32_t tgx_read_a(uint32_t addr) {
    if ((addr & 0x3FFFu) >= 0x400u) return tgx_ext_rd((addr & 0x3FFFu) + (g_tgx.eb & 0xFFC000u));
    return g_tgx.ram_a[addr & 0x3FFu];
}
static inline uint32_t tgx_read_b(uint32_t addr) { return g_tgx.ram_b[addr & 0x3FFu]; }

static inline void tgx_write_a(uint32_t addr, uint32_t v) {
    if (g_tgx.stall) return;
    if ((addr & 0x3FFFu) >= 0x400u) tgx_ext_wr((addr & 0x3FFFu) + (g_tgx.eb & 0xFFC000u), v);
    else g_tgx.ram_a[addr & 0x3FFu] = v;
}
static inline void tgx_write_b(uint32_t addr, uint32_t v) {
    if (g_tgx.stall) return;
    g_tgx.ram_b[addr & 0x3FFu] = v;
}

/* ---- FIFOs --------------------------------------------------------------- */

static inline uint32_t tgx_fin_pop(void) {
    if (g_tgx.fin_n == 0) { g_tgx.stall = true; return 0u; }
    uint32_t v = g_tgx.fin[g_tgx.fin_r];
    g_tgx.fin_r = (g_tgx.fin_r + 1u) & (TGX_FIFO - 1u);
    g_tgx.fin_n--;
    return v;
}

static inline void tgx_fout_push(uint32_t v) {
    if (g_tgx.stall) return;
    if (g_tgx.fout_n >= TGX_FIFO) { g_tgx.overflows++; return; }
    g_tgx.fout[(g_tgx.fout_r + g_tgx.fout_n++) & (TGX_FIFO - 1u)] = v;
}

/* ---- Addressing ---------------------------------------------------------- */

static inline void tgx_ar_step(uint32_t r, uint32_t by) {
    if (!g_tgx.stall) g_tgx.ar[r] = (g_tgx.ar[r] + by) & 0x3FFFu;
}

static inline uint32_t tgx_ea(uint32_t mode, uint32_t rx, uint32_t ry, uint32_t disp, bool bbus) {
    uint32_t *ar = g_tgx.ar, res;
    uint32_t bits = bbus ? (g_tgx.mod >> 8) & 7u : (g_tgx.mod >> 12) & 7u;
    switch (mode) {
    case 0x00: return ar[rx];
    case 0x01: res = ar[rx]; tgx_ar_step(rx, 1u);           return res;
    case 0x02: res = ar[rx]; tgx_ar_step(rx, 0xFFFFFFFFu);  return res;
    case 0x03: res = ar[rx]; tgx_ar_step(rx, disp);         return res;
    case 0x04: return ar[rx] + ar[ry];
    case 0x05: res = ar[ry]; tgx_ar_step(ry, 1u);           return ar[rx] + res;
    case 0x06: res = ar[ry]; tgx_ar_step(ry, 0xFFFFFFFFu);  return ar[rx] + res;
    case 0x07: res = ar[ry]; tgx_ar_step(ry, disp);         return ar[rx] + res;
    case 0x08: return ar[rx] + (bbus ? (ar[ry] & 0x7Fu) : (ar[ry] >> 7));
    case 0x09: return ar[rx] + (bbus ? (ar[ry] >> 7) : (ar[ry] & 0x7Fu));
    case 0x0A: return ar[rx] + disp;
    case 0x0B: return ar[rx] + ar[ry] + disp;
    case 0x0C: return disp;                                  /* absolute: no AR */
    case 0x0D: res = ar[ry] & (0x1FFu >> (7u - bits)); tgx_ar_step(ry, 1u);          return ar[rx] + res;
    case 0x0E: res = ar[ry] & (0x1FFu >> (7u - bits)); tgx_ar_step(ry, 0xFFFFFFFFu); return ar[rx] + res;
    case 0x0F: res = ar[ry] & (0x1FFu >> (7u - bits)); tgx_ar_step(ry, disp);        return ar[rx] + res;
    }
    tgx_bad("ea mode", mode);
    return 0u;
}

/* ---- ALU and multiplier -------------------------------------------------- */

static inline void tgx_flags_d(uint32_t v) {
    g_tgx.st &= ~(uint32_t)(TGX_AN | TGX_AZ);
    if (v & 0x80000000u) g_tgx.st |= TGX_AN;
    if (v == 0) g_tgx.st |= TGX_AZ;
}
static inline void tgx_flags_f(float v) {
    g_tgx.st &= ~(uint32_t)(TGX_AN | TGX_AZ);
    if (v < 0.0f) g_tgx.st |= TGX_AN;
    if (v == 0.0f) g_tgx.st |= TGX_AZ;
}

static inline void tgx_prp_inc(void) { if (++g_tgx.prp >= 24u) g_tgx.prp = 0; }
static inline void tgx_pwp_inc(void) { if (++g_tgx.pwp >= 24u) g_tgx.pwp = 0; }

static inline uint32_t tgx_prx(uint32_t which) {
    uint32_t res = g_tgx.pr[g_tgx.prp % 24u];
    switch (which & 7u) {
    case 0: break;
    case 1: tgx_prp_inc(); break;
    case 2: g_tgx.prp = g_tgx.prp == 0 ? 23u : g_tgx.prp - 1u; break;
    case 3: g_tgx.prp = 0; break;
    default: tgx_bad("prx", which & 7u); break;
    }
    return res;
}

static inline uint32_t tgx_const(uint32_t which, bool fp) {
    static const uint32_t ftab[8] = {   /* -1, 0, 0.5, 1, 1.5, 2, 3, 5 */
        0xBF800000u, 0x00000000u, 0x3F000000u, 0x3F800000u,
        0x3FC00000u, 0x40000000u, 0x40400000u, 0x40A00000u,
    };
    if (fp) return ftab[which & 7u];
    switch (which & 7u) {
    case 0: return 0u;
    case 1: return 1u;
    case 2: return 0xFFFFFFFFu;
    }
    tgx_bad("int constant", which & 7u);
    return 0u;
}

/* AAx, ABx, PRx, constants (ALU); MAx, MBx, PRx, constants (multiplier). */
static inline uint32_t tgx_alureg(uint32_t which, bool fp) {
    switch (which >> 3) {
    case 0: return g_tgx.aa[which & 7u];
    case 1: return g_tgx.ab[which & 7u];
    case 2: return tgx_prx(which);
    default: return tgx_const(which, fp);
    }
}
static inline uint32_t tgx_mulreg(uint32_t which, bool fp) {
    switch (which >> 3) {
    case 0: return g_tgx.ma[which & 7u];
    case 1: return g_tgx.mb[which & 7u];
    case 2: return tgx_prx(which);
    default: return tgx_const(which, fp);
    }
}

/* Results go to MAx, MBx, AAx, ABx. */
static inline void tgx_setreg(uint32_t which, uint32_t v) {
    uint32_t i = which & 7u;
    switch ((which >> 3) & 3u) {
    case 0: g_tgx.ma[i] = v; break;
    case 1: g_tgx.mb[i] = v; break;
    case 2: g_tgx.aa[i] = v; break;
    case 3: g_tgx.ab[i] = v; break;
    }
}

/* ln(x) for x > 0 in double, by frexp and the atanh series: the same bits on
 * every build, where libm's log is not promised to be. */
static inline double tgx_ln(double x) {
    int e = 0;
    double m = frexp(x, &e);                  /* x = m * 2^e, m in [0.5, 1) */
    if (m < 0.70710678118654752) { m *= 2.0; e--; }
    double s = (m - 1.0) / (m + 1.0), s2 = s * s, term = s, sum = 0.0;
    for (int k = 1; k < 40; k += 2) { sum += term / (double)k; term *= s2; }
    return 2.0 * sum + (double)e * 0.69314718055994530942;
}

/* CFIB: a float to a byte, with AU below 0, AZ at 0 and AV over 0xFF. */
static inline void tgx_cfib(uint32_t s1, uint32_t dst) {
    uint32_t *st = &g_tgx.st;
    float f = tgp_u2f(s1);
    /* uint32_t(f) on x86-64: NaN gives 0, 256 and up are over 0xFF. */
    uint32_t res = f >= 0.0f && f < 256.0f ? (uint32_t)f : (f >= 256.0f ? 0x100u : 0u);
    if (f < 0.0f) { *st |= TGX_AU; res = 0; }
    *st &= ~(uint32_t)TGX_AZ;
    if (res == 0) *st |= TGX_AZ;
    if (res > 0xFFu) { *st |= TGX_AV; res = 0xFFu; }
    tgx_setreg(dst, res);
}

/* The float half of the ALU, ops 0x00 to 0x0F. */
static inline void tgx_aluop_float(uint32_t op, uint32_t s1, uint32_t s2, uint32_t imm, uint32_t dst) {
    uint32_t *st = &g_tgx.st;
    switch (op) {
    case 0x00: case 0x01: case 0x02: case 0x03: {          /* FADD FADDZ FSUB FSUBZ */
        float f1 = tgp_u2f(s1), f2 = tgp_u2f(s2);
        float d = (op & 2u) ? f2 - f1 : f1 + f2;
        if (op & 1u) {
            *st &= ~(uint32_t)TGX_ZC;
            if (d < 0.0f) { *st |= TGX_ZC; d = 0.0f; }
        }
        tgx_flags_f(d);
        tgx_setreg(dst, tgp_f2u(d));
        break;
    }
    case 0x04: case 0x06: {                                 /* FCMP FABC */
        float f1 = tgp_u2f(s1), f2 = tgp_u2f(s2);
        tgx_flags_f((op & 2u) ? fabsf(f2) - fabsf(f1) : f2 - f1);
        break;
    }
    case 0x05: {                                            /* FABS */
        float d = fabsf(tgp_u2f(s1));
        tgx_flags_f(d);
        tgx_setreg(dst, tgp_f2u(d));
        break;
    }
    case 0x07: break;                                       /* NOP */
    case 0x08: case 0x09: {                                 /* FEA FES */
        /* MAME keeps only the exponent field and puts the new exponent in it:
         * sign and mantissa are lost. As the oracle does. */
        uint32_t e = (s1 >> 23) & 0xFFu;
        e = ((op & 1u) ? e - imm : e + imm) & 0xFFu;
        s1 = (s1 & 0x7F800000u) | e << 23;
        tgx_flags_d(s1);
        tgx_setreg(dst, s1);
        break;
    }
    case 0x0A: {                                            /* FRCP */
        float f = tgp_u2f(s1);
        *st &= ~(uint32_t)TGX_ZD;
        if (f == 0.0f) *st |= TGX_ZD;
        f = 1.0f / f;
        tgx_flags_f(f);
        tgx_setreg(dst, tgp_f2u(f));
        break;
    }
    case 0x0B: {                                            /* FRSQ */
        float f = tgp_u2f(s1);
        *st &= ~(uint32_t)TGX_NR;
        if (f <= 0.0f) *st |= TGX_NR;
        f = 1.0f / sqrtf(f);
        tgx_flags_f(f);
        tgx_setreg(dst, tgp_f2u(f));
        break;
    }
    case 0x0C: {                                            /* FLOG */
        float f = tgp_u2f(s1);
        *st &= ~(uint32_t)TGX_IL;
        if (f <= 0.0f) *st |= TGX_IL;
        /* MAME: log(f) / 0.301030F, the natural log over log10(2). */
        float l = f > 0.0f ? (float)tgx_ln((double)f) : (f == 0.0f ? -HUGE_VALF : NAN);
        f = l / 0.301030f;
        tgx_flags_f(f);
        tgx_setreg(dst, tgp_f2u(f));
        break;
    }
    case 0x0D: {                                            /* CIF */
        float f = (float)(int32_t)s1;
        tgx_flags_f(f);
        tgx_setreg(dst, tgp_f2u(f));
        break;
    }
    case 0x0E: {                                            /* CFI */
        uint32_t v = tgp_f2i(tgp_u2f(s1));
        tgx_flags_d(v);
        tgx_setreg(dst, v);
        break;
    }
    case 0x0F: tgx_cfib(s1, dst); break;                    /* CFIB */
    }
}

/* The integer half, ops 0x10 to 0x1F. */
static inline void tgx_aluop_int(uint32_t op, uint32_t s1, uint32_t s2, uint32_t imm, uint32_t dst) {
    uint32_t *st = &g_tgx.st;
    switch (op) {
    case 0x10: case 0x11: case 0x12: case 0x13: {          /* ADD ADDZ SUB SUBZ */
        uint32_t r = (op & 2u) ? s2 - s1 : s1 + s2;
        if (op & 1u) {
            *st &= ~(uint32_t)TGX_ZC;
            if (r & 0x80000000u) { *st |= TGX_ZC; r = 0; }
        }
        tgx_flags_d(r);
        tgx_setreg(dst, r);
        break;
    }
    case 0x14: tgx_flags_d(s2 - s1); break;                 /* CMP */
    case 0x15: s1 &= 0x7FFFFFFFu; tgx_flags_d(s1); tgx_setreg(dst, s1); break;   /* ABS */
    case 0x16: case 0x17:                                   /* ATR ATRZ */
        if (op & 1u) {
            *st &= ~(uint32_t)TGX_ZC;
            if (s1 & 0x80000000u) { *st |= TGX_ZC; s1 = 0; }
        }
        tgx_setreg(dst, s1);
        break;
    case 0x18: tgx_flags_d(s1 & s2); tgx_setreg(dst, s1 & s2); break;   /* AND */
    case 0x19: tgx_flags_d(s1 | s2); tgx_setreg(dst, s1 | s2); break;   /* OR */
    case 0x1A: tgx_flags_d(s1 ^ s2); tgx_setreg(dst, s1 ^ s2); break;   /* XOR */
    case 0x1B: tgx_flags_d(~s1);     tgx_setreg(dst, ~s1);     break;   /* NOT */
    case 0x1C: tgx_flags_d(s1 >> imm); tgx_setreg(dst, s1 >> imm); break;   /* LSR */
    case 0x1D: case 0x1F:                                   /* LSL ASL */
        tgx_flags_d(s1 << imm); tgx_setreg(dst, s1 << imm); break;
    case 0x1E: {                                            /* ASR */
        uint32_t r = (s1 & 0x80000000u) && imm ? (s1 >> imm) | ~(0xFFFFFFFFu >> imm) : s1 >> imm;
        tgx_flags_d(r);
        tgx_setreg(dst, r);
        break;
    }
    }
}

static inline void tgx_aluop(uint32_t op, uint32_t s1, uint32_t s2, uint32_t imm, uint32_t dst) {
    if (op & 0x10u) tgx_aluop_int(op, s1, s2, imm, dst);
    else            tgx_aluop_float(op, s1, s2, imm, dst);
}

static inline void tgx_mulop(bool fmul, uint32_t s1, uint32_t s2, uint32_t dst) {
    if (fmul) {
        float r = tgp_u2f(s1) * tgp_u2f(s2);
        g_tgx.st &= ~(uint32_t)(TGX_MN | TGX_MZ | TGX_MD);   /* MV and MU stay */
        if (r < 0.0f) g_tgx.st |= TGX_MN;
        if (r == 0.0f) g_tgx.st |= TGX_MZ;
        if (isinf(r)) g_tgx.st |= TGX_MV;
        if (fabsf(r) < FLT_MIN) g_tgx.st |= TGX_MU;          /* zero too, as MAME */
        if (r != r) g_tgx.st |= TGX_MD;
        tgx_setreg(dst, tgp_f2u(r));
    } else {
        uint32_t r = s1 * s2;
        g_tgx.st &= ~(uint32_t)(TGX_MN | TGX_MZ);
        if (r & 0x80000000u) g_tgx.st |= TGX_MN;
        if (r == 0) g_tgx.st |= TGX_MZ;
        tgx_setreg(dst, r);
    }
}

#define TGX_AOP(x) ((uint32_t)((x) >> 56) & 0x1Fu)
#define TGX_AI1(x) ((uint32_t)((x) >> 52) & 0x0Fu)
#define TGX_AI2(x) ((uint32_t)((x) >> 47) & 0x1Fu)
#define TGX_AO(x)  ((uint32_t)((x) >> 42) & 0x1Fu)
#define TGX_MOP(x) ((uint32_t)((x) >> 41) & 0x01u)
#define TGX_MI1(x) ((uint32_t)((x) >> 37) & 0x0Fu)
#define TGX_MI2(x) ((uint32_t)((x) >> 32) & 0x1Fu)
#define TGX_MO(x)  ((uint32_t)((x) >> 27) & 0x1Fu)

/* Whether an ALU op reads a second operand. */
static inline bool tgx_alu_src2(uint32_t op) {
    if ((op & 0x1Cu) == 0x1Cu) return false;     /* shifts */
    if ((op & 0x1Eu) == 0x16u) return false;     /* ATR */
    if ((op & 0x0Fu) == 0x05u) return false;     /* ABS FABS */
    if ((op & 0x18u) == 0x08u) return false;     /* one-operand float ops */
    return true;
}

static inline void tgx_alu1(uint64_t op) {
    if (g_tgx.stall) return;
    if ((op >> 41) & 1u) {
        uint32_t aop = TGX_AOP(op);
        uint32_t s1 = tgx_alureg(TGX_AI1(op), false);
        uint32_t s2 = tgx_alu_src2(aop) ? tgx_alureg(TGX_AI2(op), (aop & 0x10u) == 0) : 0u;
        tgx_aluop(aop, s1, s2, TGX_AI2(op), TGX_AO(op));
    } else {
        bool fmul = TGX_AOP(op) != 0;
        uint32_t s1 = tgx_mulreg(TGX_AI1(op), false);
        uint32_t s2 = tgx_mulreg(TGX_AI2(op), fmul);
        tgx_mulop(fmul, s1, s2, TGX_AO(op));
    }
}

static inline void tgx_alu2(uint64_t op) {
    if (g_tgx.stall) return;
    uint32_t aop = TGX_AOP(op);
    uint32_t a1 = tgx_alureg(TGX_AI1(op), false);
    uint32_t a2 = tgx_alu_src2(aop) ? tgx_alureg(TGX_AI2(op), (aop & 0x10u) == 0) : 0u;
    bool fmul = TGX_MOP(op) != 0;
    uint32_t m1 = tgx_mulreg(TGX_MI1(op), false);
    uint32_t m2 = tgx_mulreg(TGX_MI2(op), fmul);
    tgx_aluop(aop, a1, a2, TGX_AI2(op), TGX_AO(op));
    tgx_mulop(fmul, m1, m2, TGX_MO(op));
}

/* ---- Transfers ----------------------------------------------------------- */

static inline uint32_t tgx_get(uint32_t which) {
    uint32_t i = which & 7u;
    switch (which >> 3) {
    case 0: return g_tgx.ma[i];
    case 1: return g_tgx.aa[i];
    case 2:
        switch (i) {
        case 0: return g_tgx.eb;
        case 1: return g_tgx.eb >> 14;
        case 2: return g_tgx.eb & 0x3FFFu;
        case 3: return g_tgx.eo;
        case 4: return g_tgx.sp;
        case 5: return g_tgx.st;
        case 6: return g_tgx.mod;
        default: return g_tgx.lpc;
        }
    case 3: return g_tgx.ar[i];
    case 4: return g_tgx.mb[i];
    case 5: return g_tgx.ab[i];
    case 6:
        switch (i) {
        case 0: return g_tgx.pr[g_tgx.prp % 24u];
        case 1: return tgx_fin_pop();                       /* FI */
        case 4: return g_tgx.pdr;
        case 5: return g_tgx.ddr;
        case 6: return g_tgx.prp;
        case 7: return g_tgx.pwp;
        }
        break;
    }
    tgx_bad("transfer source", which);
    return 0u;
}

static inline void tgx_set(uint32_t which, uint32_t v) {
    if (g_tgx.stall) return;
    uint32_t i = which & 7u;
    switch (which >> 3) {
    case 0: g_tgx.ma[i] = v; return;
    case 1: g_tgx.aa[i] = v; return;
    case 2:
        switch (i) {
        case 0: g_tgx.eb = v; return;
        case 1: g_tgx.eb = (g_tgx.eb & 0x3FFFu) | (v << 14); return;
        case 2: g_tgx.eb = (g_tgx.eb & 0xFFC000u) | (v << 14); return;   /* sic, MAME */
        case 3: g_tgx.eo = v; return;
        case 4: g_tgx.sp = v; return;
        case 5: g_tgx.st = v; return;
        case 6: g_tgx.mod = v; return;
        default: g_tgx.lpc = v; return;
        }
    case 3: g_tgx.ar[i] = v & 0x3FFFu; return;
    case 4: g_tgx.mb[i] = v; return;
    case 5: g_tgx.ab[i] = v; return;
    case 6:
        switch (i) {
        case 0: g_tgx.pr[g_tgx.pwp % 24u] = v; tgx_pwp_inc(); return;
        case 2: case 3: tgx_fout_push(v); return;           /* FO0, FO1 */
        case 4: g_tgx.pdr = v; return;
        case 5: g_tgx.ddr = v; return;
        case 6: if (v < 24u) g_tgx.prp = v; else tgx_bad("prp", v); return;
        case 7: if (v < 24u) g_tgx.pwp = v; else tgx_bad("pwp", v); return;
        }
        break;
    }
    tgx_bad("transfer destination", which);
}

static inline void tgx_eo_step(uint32_t bits6) {
    if (g_tgx.stall) return;
    g_tgx.eo += (bits6 & 0x20u) ? bits6 - 0x40u : bits6;
}

/* Double transfer type 1 with two ALU ops. */
static inline void tgx_alu2_trans2_1(uint64_t op) {
    uint32_t sd = (uint32_t)(op >> 25) & 3u, ar = 0, br = 0;
    uint32_t m = (uint32_t)op & 0xFu;
    switch (sd) {
    case 0: case 1:
        ar = tgx_get((uint32_t)(op >> 20) & 0x1Fu);
        br = tgx_get(((uint32_t)(op >> 10) & 0xFu) | 0x20u);
        break;
    case 2:
        ar = tgx_read_a(tgx_ea(m, (uint32_t)(op >> 17) & 7u, (uint32_t)(op >> 14) & 7u, 0, false));
        br = tgx_read_b(tgx_ea(m, (uint32_t)(op >> 7) & 7u, (uint32_t)(op >> 4) & 7u, 0, true));
        break;
    default: tgx_bad("trans2_1 sd", sd); break;
    }
    tgx_alu2(op);
    switch (sd) {
    case 0: case 2:
        tgx_set((uint32_t)(op >> 20) & 0x1Fu, ar);
        tgx_set(((uint32_t)(op >> 10) & 0xFu) | 0x20u, br);
        break;
    case 1:
        tgx_write_a(tgx_ea(m, (uint32_t)(op >> 17) & 7u, (uint32_t)(op >> 14) & 7u, 0, false), ar);
        tgx_write_b(tgx_ea(m, (uint32_t)(op >> 7) & 7u, (uint32_t)(op >> 4) & 7u, 0, true), br);
        break;
    }
}

/* Transfer type 1 with two ALU ops. */
static inline void tgx_alu2_trans1_1(uint64_t op) {
    uint32_t m = (uint32_t)op & 0xFu, ry = (uint32_t)(op >> 4) & 7u, disp = (uint32_t)(op >> 7) & 0x1Fu;
    uint32_t res;
    if ((op >> 26) & 1u) {                                  /* external */
        if ((op >> 25) & 1u) {                              /* ext -> int */
            res = tgx_ext_rd(g_tgx.eb + g_tgx.eo);
            tgx_alu2(op);
            uint32_t dr = (uint32_t)(op >> 12) & 0x7Fu;
            if (dr & 0x40u) {
                bool bb = (dr & 0x20u) != 0;
                uint32_t a = tgx_ea(m, dr & 7u, ry, disp, bb) & 0x3FFu;
                if (bb) tgx_write_b(a, res); else tgx_write_a(a, res);
            } else tgx_set(dr, res);
        } else {                                            /* int -> ext */
            uint32_t sr = (uint32_t)(op >> 12) & 0x7Fu;
            if (sr & 0x40u) {
                bool bb = (sr & 0x20u) != 0;
                uint32_t a = tgx_ea(m, sr & 7u, ry, disp, bb) & 0x3FFu;
                res = bb ? tgx_read_b(a) : tgx_read_a(a);
            } else res = tgx_get(sr);
            tgx_alu2(op);
            tgx_ext_wr(g_tgx.eb + g_tgx.eo, res);
        }
        tgx_eo_step((uint32_t)(op >> 19) & 0x3Fu);
        return;
    }
    uint32_t sr = (uint32_t)(op >> 19) & 0x7Fu;
    if (sr & 0x40u) {
        if (sr == 0x58u) res = (uint32_t)op & 0xFFFu;
        else {
            bool bb = (sr & 0x20u) != 0;
            uint32_t a = tgx_ea(m, sr & 7u, ry, disp, bb);
            res = bb ? tgx_read_b(a) : tgx_read_a(a);
        }
    } else res = tgx_get(sr);
    tgx_alu2(op);
    uint32_t dr = (uint32_t)(op >> 12) & 0x7Fu;
    if (dr & 0x40u) {
        if (dr == 0x58u) { tgx_bad("trans1_1 dr", dr); return; }
        bool bb = (dr & 0x20u) != 0;
        uint32_t a = tgx_ea(m, dr & 7u, ry, disp, bb);
        if (bb) tgx_write_b(a, res); else tgx_write_a(a, res);
    } else tgx_set(dr, res);
}

/* Double transfer type 2 with one ALU op. */
static inline void tgx_alu1_trans2_2(uint64_t op) {
    uint32_t sda = (uint32_t)(op >> 38) & 3u, sdb = (uint32_t)(op >> 18) & 3u, ar = 0, br = 0;
    uint32_t ma = (uint32_t)(op >> 20) & 0xFu, mb = (uint32_t)op & 0xFu;
    uint32_t ax = (uint32_t)(op >> 30) & 7u, ay = (uint32_t)(op >> 27) & 7u, ad = (uint32_t)(op >> 24) & 7u;
    uint32_t bx = (uint32_t)(op >> 10) & 7u, by = (uint32_t)(op >> 7) & 7u, bd = (uint32_t)(op >> 4) & 7u;
    switch (sda) {
    case 0: case 1: ar = tgx_get((uint32_t)(op >> 33) & 0x1Fu); break;
    case 2: ar = tgx_read_a(tgx_ea(ma, ax, ay, ad, false)); break;
    default: tgx_bad("trans2_2 sda", sda); break;
    }
    switch (sdb) {
    case 0: case 1: br = tgx_get(((uint32_t)(op >> 13) & 0x1Fu) | 0x20u); break;
    case 2: br = tgx_read_b(tgx_ea(mb, bx, by, bd, true)); break;
    default: tgx_bad("trans2_2 sdb", sdb); break;
    }
    tgx_alu1(op);
    switch (sda) {
    case 0: case 2: tgx_set((uint32_t)(op >> 28) & 0x1Fu, ar); break;
    case 1: tgx_write_a(tgx_ea(ma, ax, ay, ad, false), ar); break;
    }
    switch (sdb) {
    case 0: case 2: tgx_set(((uint32_t)(op >> 8) & 0x1Fu) | 0x20u, br); break;
    case 1: tgx_write_b(tgx_ea(mb, bx, by, bd, true), br); break;
    }
}

/* Transfer type 2 with one ALU op. */
static inline void tgx_alu1_trans1_2(uint64_t op) {
    uint32_t m = (uint32_t)op & 0xFu, ry = (uint32_t)(op >> 4) & 7u, disp = (uint32_t)(op >> 7) & 0x3FFFu;
    uint32_t res;
    if ((op >> 38) & 1u) {                                  /* external */
        if ((op >> 37) & 1u) {                              /* ext -> int */
            res = tgx_ext_rd(g_tgx.eb + g_tgx.eo);
            tgx_alu1(op);
            uint32_t dr = (uint32_t)(op >> 24) & 0x7Fu;
            if (dr & 0x40u) {
                bool bb = (dr & 0x20u) != 0;
                uint32_t a = tgx_ea(m, dr & 7u, ry, disp, bb) & 0x3FFu;
                if (bb) tgx_write_b(a, res); else tgx_write_a(a, res);
            } else tgx_set(dr, res);
        } else {                                            /* int -> ext */
            uint32_t sr = (uint32_t)(op >> 24) & 0x7Fu;
            if (sr & 0x40u) {
                if (sr == 0x58u) res = (uint32_t)op & 0xFFFFFFu;
                else {
                    bool bb = (sr & 0x20u) != 0;
                    uint32_t a = tgx_ea(m, sr & 7u, ry, disp, bb) & 0x3FFu;
                    res = bb ? tgx_read_b(a) : tgx_read_a(a);
                }
            } else res = tgx_get(sr);
            tgx_alu1(op);
            tgx_ext_wr(g_tgx.eb + g_tgx.eo, res);
        }
        tgx_eo_step((uint32_t)(op >> 31) & 0x3Fu);
        return;
    }
    uint32_t sr = (uint32_t)(op >> 31) & 0x7Fu;
    if (sr & 0x40u) {
        if (sr == 0x58u) res = (uint32_t)op & 0xFFFFFFu;
        else {
            bool bb = (sr & 0x20u) != 0;
            uint32_t a = tgx_ea(m, sr & 7u, ry, disp, bb);
            res = bb ? tgx_read_b(a) : tgx_read_a(a);
        }
    } else res = tgx_get(sr);
    tgx_alu1(op);
    uint32_t dr = (uint32_t)(op >> 24) & 0x7Fu;
    if (dr & 0x40u) {
        if (dr == 0x58u) { tgx_bad("trans1_2 dr", dr); return; }
        bool bb = (dr & 0x20u) != 0;
        uint32_t a = tgx_ea(m, dr & 7u, ry, disp, bb);
        if (bb) tgx_write_b(a, res); else tgx_write_a(a, res);
    } else tgx_set(dr, res);
}

/* Transfer type 3: a 32-bit immediate. */
static inline void tgx_trans1_3(uint64_t op) {
    uint32_t dr = (uint32_t)(op >> 19) & 0x7Fu;
    uint32_t imm = (uint32_t)(op >> 27);
    if (dr & 0x40u) {
        bool bb = (dr & 0x20u) != 0;
        uint32_t a = tgx_ea((uint32_t)op & 0xFu, dr & 7u, (uint32_t)(op >> 4) & 7u, (uint32_t)(op >> 7) & 0xFFFu, bb);
        if (bb) tgx_write_b(a, imm); else tgx_write_a(a, imm);
    } else tgx_set(dr, imm);
}

/* ---- Control ------------------------------------------------------------- */

static inline uint32_t tgx_ctl_dst(uint64_t op) {
    uint32_t r = (uint32_t)(op >> 6) & 7u, addr = 0;
    bool hi = (op >> 11) & 1u;
    switch ((uint32_t)(op >> 13) & 7u) {
    case 0: addr = (uint32_t)op & 0xFFFu; break;
    case 1: addr = g_tgx.ar[r]; break;
    case 2: addr = hi ? g_tgx.ab[r] : g_tgx.aa[r]; break;
    case 3: addr = hi ? g_tgx.mb[r] : g_tgx.ma[r]; break;
    case 4: addr = tgx_read_a((uint32_t)op & 0x3FFu); break;
    case 5: addr = tgx_read_b((uint32_t)op & 0x3FFu); break;
    case 6: addr = tgx_read_a(g_tgx.ar[r]); break;
    case 7: addr = tgx_read_b(g_tgx.ar[r]); break;
    }
    return ((op >> 12) & 1u) ? (g_tgx.pc + addr) & 0xFFFu : addr & 0xFFFu;
}

static inline bool tgx_cond(uint32_t which) {
    static const uint32_t tab[19] = {
        TGX_MN, TGX_MZ, TGX_MV, TGX_MU, TGX_ZD, TGX_NR, TGX_IL, TGX_ZC, TGX_AN, TGX_AZ,
        TGX_AV, TGX_AU, TGX_MD, TGX_AD, 0, 0, TGX_F0, TGX_F1, TGX_F2,
    };
    if (which < 19u) return (g_tgx.st & tab[which]) != 0;
    switch (which) {
    case 20: return g_tgx.fin_n >= TGX_FIFO_BOARD;                      /* IFF */
    case 21: return g_tgx.fin_n == 0;                                   /* IFE */
    case 22: return g_tgx.fout_n >= TGX_FIFO_BOARD;                     /* OFF */
    case 23: return g_tgx.fout_n == 0;                                  /* OFE */
    case 24: return false;                                              /* IF, unused */
    }
    tgx_bad("branch condition", which);
    return false;
}

/* A branch on a condition only the i960 can change: IFE true, or OFF true. */
static inline void tgx_note_wait(uint32_t which, bool result) {
    if (result && (which == 21u || which == 22u)) g_tgx.wait = true;
}

static inline void tgx_push_pc(uint32_t v) { g_tgx.pcs[g_tgx.pcp & 3u] = v; g_tgx.pcp = (g_tgx.pcp + 1u) & 3u; }
static inline uint32_t tgx_pop_pc(void)    { g_tgx.pcp = (g_tgx.pcp - 1u) & 3u; return g_tgx.pcs[g_tgx.pcp]; }

static inline void tgx_jump(uint32_t to) { g_tgx.delay_slot = true; g_tgx.delay_pc = to & 0xFFFu; }

/* The branch ops, 0x10 to 0x1B: a delayed jump when the condition holds, and
 * for DCcc and DCALL the return address pushed. */
static inline void tgx_branch(uint64_t op, uint32_t cop, uint32_t ef1) {
    switch (cop) {
    case 0x10: { bool r = tgx_cond(ef1); tgx_note_wait(ef1, r);  if (r)  tgx_jump(tgx_ctl_dst(op)); break; }  /* DBcc */
    case 0x11: { bool r = tgx_cond(ef1); tgx_note_wait(ef1, !r); if (!r) tgx_jump(tgx_ctl_dst(op)); break; }  /* DBNcc */
    case 0x12: tgx_jump(tgx_ctl_dst(op)); break;                        /* DJMP */
    case 0x13:                                                          /* DBLP */
        if (g_tgx.st & TGX_LP) tgx_jump(g_tgx.pc + ((uint32_t)op & 0xFFFu));
        if (--g_tgx.lpc == 1u) g_tgx.st &= ~(uint32_t)TGX_LP;
        break;
    case 0x14: case 0x15: {                                             /* DBBC DBBS */
        bool set = (g_tgx.ar[(op >> 13) & 7u] >> ((op >> 16) & 0xFu)) & 1u;
        if (set == (cop == 0x15u)) tgx_jump(g_tgx.pc + ((uint32_t)op & 0xFFFu));
        break;
    }
    case 0x18: case 0x19: {                                             /* DCcc DCNcc */
        bool r = tgx_cond(ef1);
        if (cop == 0x19u) r = !r;
        if (r) { tgx_jump(tgx_ctl_dst(op)); tgx_push_pc(g_tgx.pc + 1u); }
        break;
    }
    case 0x1A: tgx_jump(tgx_ctl_dst(op)); tgx_push_pc(g_tgx.pc + 1u); break;   /* DCALL */
    case 0x1B: tgx_jump(tgx_pop_pc()); break;                           /* DRET */
    }
}

static inline void tgx_control(uint64_t op) {
    uint32_t cop = (uint32_t)(op >> 22) & 0x1Fu;
    uint32_t ef1 = (uint32_t)(op >> 16) & 0x3Fu;
    uint32_t ef2 = (uint32_t)op & 0xFFFFu;
    switch (cop) {
    case 0x00: break;                                                   /* NOP */
    case 0x01: g_tgx.rpc = ef1 == 0x3Fu ? g_tgx.ar[(ef2 >> 13) & 7u] : ef2; g_tgx.st |= TGX_RP; break;  /* REP */
    case 0x02: g_tgx.lpc = ef1 == 0x3Fu ? g_tgx.ar[(ef2 >> 13) & 7u] : ef2; g_tgx.st |= TGX_LP; break;  /* SETL */
    case 0x03:                                                          /* CLRF */
        if (ef1 & 1u) g_tgx.fin_n = 0;
        if (ef1 & 2u) g_tgx.fout_n = 0;
        break;
    case 0x04: {                                                        /* PUSH */
        uint32_t v = tgx_get((ef2 >> 6) & 0x3Fu);
        if (!g_tgx.stall) {
            g_tgx.sp = (g_tgx.sp - 1u) & 0x3FFu;
            g_tgx.ram_b[g_tgx.sp] = v;
        }
        break;
    }
    case 0x05: break;                                                   /* POP, after the ALU */
    case 0x08: g_tgx.mod = ef2; break;                                  /* SETM */
    case 0x09: g_tgx.mod = (g_tgx.mod & ~0x7000u) | ef2; break;         /* SETMCBSA */
    case 0x0A: g_tgx.mod = (g_tgx.mod & ~0x0E00u) | ef2; break;         /* SETMCBSB */
    case 0x0B: g_tgx.mod = (g_tgx.mod & ~0x0080u) | ef2; break;         /* SETMRF */
    case 0x0C: g_tgx.mod = (g_tgx.mod & ~0x0010u) | ef2; break;         /* SETMRDY */
    case 0x0D: g_tgx.mod = (g_tgx.mod & ~0x0007u) | ef2; break;         /* SETMWAIT */
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:   /* branches */
    case 0x18: case 0x19: case 0x1A: case 0x1B: tgx_branch(op, cop, ef1); break;
    default: tgx_bad("control op", cop); break;
    }
    if ((op >> 63) & 1u) tgx_alu1(op); else tgx_alu2(op);
    if (cop == 0x05u && !g_tgx.stall) {                                 /* POP */
        tgx_set((ef2 >> 6) & 0x3Fu, g_tgx.ram_b[g_tgx.sp & 0x3FFu]);
        g_tgx.sp = (g_tgx.sp + 1u) & 0x3FFu;
    }
}

/* ---- Run ----------------------------------------------------------------- */

/* One instruction. False if it stalled on the empty FIFO: then nothing has
 * changed and the next step runs it again. */
static inline bool tgx_step(void) {
    uint32_t pc = g_tgx.pc, delay_pc = g_tgx.delay_pc, rpc = g_tgx.rpc, st = g_tgx.st;
    bool delay = g_tgx.delay_slot;

    uint32_t cur = g_tgx.pc & 0xFFFu;
    uint64_t op = g_tgx.prog[cur];
    g_tgx.ppc = cur;
    if (g_tgx.delay_slot) {
        g_tgx.pc = g_tgx.delay_pc;
        g_tgx.delay_slot = false;
    } else if (g_tgx.st & TGX_RP) {
        if (--g_tgx.rpc == 1u) g_tgx.st &= ~(uint32_t)TGX_RP;
    } else
        g_tgx.pc = (g_tgx.pc + 1u) & 0xFFFu;

    g_tgx.stall = false;
    switch ((uint32_t)(op >> 61) & 7u) {
    case 0: tgx_alu2_trans2_1(op); break;
    case 1: tgx_alu2_trans1_1(op); break;
    case 4: tgx_alu1_trans2_2(op); break;
    case 5: tgx_alu1_trans1_2(op); break;
    case 2: case 6: tgx_control(op); break;
    case 7: tgx_trans1_3(op); break;
    default: tgx_bad("opcode type", (uint32_t)(op >> 61) & 7u); break;
    }
    if (g_tgx.stall) {
        g_tgx.pc = pc; g_tgx.delay_pc = delay_pc; g_tgx.rpc = rpc; g_tgx.st = st;
        g_tgx.delay_slot = delay;
        g_tgx.stall = false;
        return false;
    }
    g_tgx.insns++;
    return true;
}

/* Run until the program waits on the i960 (a word it has not sent, an IFE or
 * OFF poll), or for TGX_RUN_MAX instructions. */
static inline void tgx_run(void) {
    if (g_tgx.halted) return;
    for (uint32_t n = 0; n < TGX_RUN_MAX; n++) {
        g_tgx.wait = false;
        if (!tgx_step()) return;
        if (g_tgx.wait && !g_tgx.delay_slot) return;
        if (g_tgx.wait) { tgx_step(); return; }      /* its delay slot first */
    }
    if (g_tgx.budget_stops++ < 16) LOG_WARN("TGPx4: ran %u instructions without waiting (pc %03X)", TGX_RUN_MAX, g_tgx.pc);
}

static inline void tgx_reset(void) {
    memset(&g_tgx, 0, sizeof g_tgx);
    g_tgx.halted = true;
}

#endif /* TGPX4_H */
