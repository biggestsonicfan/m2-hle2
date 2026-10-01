/*
 * tgp.h — the Model 2A coprocessor: a Fujitsu MB86234 "TGP" (board-level).
 *
 * The original Model 2 and Model 2A put a TGP where Model 2B has a SHARC.
 * Every 2A game (VF2, Sega Rally, Virtua Cop 2, Manx TT, ...) talks to it the
 * same way, so it is emulated as a CPU running the game's own program, not as a
 * handler per command: the program differs per game (Sega Rally's rotations are
 * ops 0x29-0x2B, STF's SHARC ones 0x08-0x0A), and its arithmetic comes from the
 * CPU board's table ROMs, not from libm. See MODEL2A-TGP.md.
 *
 * The instruction set, the register file, the table lookups and the FIFO wiring
 * follow MAME (devices/cpu/mb86233/mb86233.cpp, BSD-3-Clause, Olivier Galibert;
 * mame/sega/model2.cpp for the board side), which is the oracle. Only floating-
 * point mode is implemented, as there: every Sega program switches to it first.
 *
 * The i960's side (model2.cpp model2_tgp_mem):
 *   0x00880000..0x00883FFF  function port: a write pushes
 *                           ((byte_offset >> 4) & 0xFF) << 23 | (data & 0x800FFFFF)
 *   0x00884000..0x00887FFF  FIFO: a write pushes the word (or, while the
 *                           control register's bit 31 is set, stores the next
 *                           program word); a read pops a reply
 *   0x00980000              copro control: bit 31 set = upload, cleared = boot
 *   0x00980004              reads 1 while the reply FIFO is empty
 *   0x00980030..3F          ID bytes
 *
 * Timing: the TGP runs synchronously. Each push runs it until it wants a word
 * the FIFO does not hold. MAME runs it alongside the i960 with 8-word FIFOs that
 * stall whichever side gets ahead; since every exchange is a complete command
 * followed by a read of its replies, the words that come out are the same. The
 * reply FIFO here is deep so the TGP never has to wait for the i960.
 */
#ifndef TGP_H
#define TGP_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "log.h"

/* ---- Status flags (MAME mb86233.h st_flags) ------------------------------ */

enum {
    TGP_F_ZRC = 0x00000001, TGP_F_ZRD = 0x00000002,
    TGP_F_SGC = 0x00000004, TGP_F_SGD = 0x00000008,
    TGP_F_CPC = 0x00000010, TGP_F_CPD = 0x00000020,
    TGP_F_OVC = 0x00000040, TGP_F_OVD = 0x00000080,
    TGP_F_DVZC = 0x00000400, TGP_F_DVZD = 0x00000800,
    TGP_F_ZX0 = 0x08000000, TGP_F_ZX1 = 0x10000000, TGP_F_ZX2 = 0x20000000,
    TGP_F_ZC0 = 0x40000000, TGP_F_ZC1 = 0x80000000u,
};

#define TGP_ALU_FLAGS  (TGP_F_ZRD | TGP_F_SGD | TGP_F_CPD | TGP_F_OVD | TGP_F_DVZD)

#define TGP_PROG_WORDS  0x1000u     /* program RAM, 0x000-0xFFF */
#define TGP_FIFO_IN     64u         /* power of 2 */
#define TGP_FIFO_OUT    4096u       /* power of 2; deep, see the header */
/* A run that never asks for input is a program that has lost its way (or one
 * waiting on something not emulated). Stop it rather than hang the board. */
#define TGP_RUN_BUDGET  4000000u

typedef struct {
    /* CPU */
    uint32_t st, a, b, d, p;
    uint32_t alu_stmask, alu_stset, alu_r1, alu_r2;
    uint16_t ppc, pc, sp, b0, b1, x0, x1, i0, i1, vsmr, pcs[4], mask, m;
    uint8_t  r, rpc, c0, c1, sft, vsm;
    bool     gpio0, gpio1, gpio2, gpio3;
    bool     stall;                 /* this instruction read an empty FIFO */
    bool     halted;                /* held by the board: before boot, during upload */

    uint32_t prog[TGP_PROG_WORDS];
    uint32_t ram0[0x100];           /* data 0x000-0x0FF */
    uint32_t ram1[0x200];           /* data 0x200-0x3FF */

    /* Board glue (model2.cpp model2_tgp_state) */
    uint32_t bank_reg;              /* RF 3: bits 22-23 select external memory for I/O space */
    uint32_t sincos_base, inv_base, isqrt_base, atan_base[4];
    uint32_t coproctl, coprocnt;

    uint32_t fin[TGP_FIFO_IN];   uint32_t fin_r, fin_n;
    uint32_t fout[TGP_FIFO_OUT]; uint32_t fout_r, fout_n;

    /* Counters for the log / bridge. */
    uint64_t insns;
    uint32_t pushes, pops, empty_pops, overflows, budget_stops;
} tgp_state_t;

static tgp_state_t g_tgp;

/* Set by the 2A board install. Outside g_tgp so a reset keeps them. */
static const uint8_t *g_tgp_tables      = NULL;  /* copro_tgp_tables, 0x10000 LE words */
static const uint8_t *g_tgp_copro_data  = NULL;  /* copro_data ROM, LE words */
static uint32_t       g_tgp_copro_words = 0;     /* a power of 2, or 0 */
static uint8_t       *g_tgp_bufferram   = NULL;  /* i960 0x900000, 0x8000 words */

/* ---- Helpers ------------------------------------------------------------- */

static inline uint32_t tgp_le32(const uint8_t *p, uint32_t w) {
    p += (size_t)w * 4u;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint32_t tgp_table(uint32_t index) {
    return g_tgp_tables ? tgp_le32(g_tgp_tables, index & 0xFFFFu) : 0u;
}

static inline float tgp_u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* A NaN is written as all ones, as sharc_float_to_bits does and for the same
 * reason: IEEE leaves its sign and payload to the compiler, and two builds that
 * disagree about them compute different frames (CLAUDE.md, "wasm and MSVC"). */
static inline uint32_t tgp_f2u(float f) {
    if (f != f) return 0xFFFFFFFFu;
    uint32_t u; memcpy(&u, &f, 4); return u;
}

/* float -> int32 with x86's answer for what C leaves undefined (NaN, out of
 * range: 0x80000000), so every build agrees. */
static inline uint32_t tgp_f2i(float f) {
    if (!(f >= -2147483648.0f && f < 2147483648.0f)) return 0x80000000u;
    return (uint32_t)(int32_t)f;
}

static inline uint32_t tgp_sext(uint32_t v, int bits) {
    uint32_t m = 1u << (bits - 1);
    v &= (m << 1) - 1u;
    return (v ^ m) - m;
}

static inline uint32_t tgp_set_exp(uint32_t v, uint32_t e)  { return (v & 0x807FFFFFu) | ((e & 0xFFu) << 23); }
static inline uint32_t tgp_set_mant(uint32_t v, uint32_t m) { return (v & 0x7F800000u) | ((m & 0x00800000u) << 8) | (m & 0x007FFFFFu); }
static inline uint32_t tgp_get_exp(uint32_t v)  { return (v >> 23) & 0xFFu; }
static inline uint32_t tgp_get_mant(uint32_t v) { return (v & 0x80000000u) ? v | 0x7F800000u : v & 0x807FFFFFu; }

/* ---- FIFOs --------------------------------------------------------------- */

static inline void tgp_out_push(uint32_t v) {
    if (g_tgp.fout_n >= TGP_FIFO_OUT) { g_tgp.overflows++; return; }
    g_tgp.fout[(g_tgp.fout_r + g_tgp.fout_n++) & (TGP_FIFO_OUT - 1)] = v;
}

static inline bool tgp_out_empty(void) { return g_tgp.fout_n == 0; }

/* ---- The board's table lookups (model2.cpp copro_*_r / _w) --------------- */

static inline uint32_t tgp_sincos_r(uint32_t off) {
    uint32_t ang = g_tgp.sincos_base + off * 0x4000u;
    uint32_t index = ang & 0x3FFFu;
    if (ang & 0x4000u) {
        int32_t i = 0x4000 - (int32_t)index;
        index = (uint32_t)(i < 0x3FFF ? i : 0x3FFF);
    }
    uint32_t result = tgp_table(index);
    if (ang & 0x8000u) result ^= 0x80000000u;
    return result;
}

static inline uint32_t tgp_inv_r(uint32_t off) {
    uint32_t base = g_tgp.inv_base;
    uint32_t index = ((base >> 9) & 0x3FFEu) | (off & 1u);
    uint32_t result = tgp_table(index | 0x8000u);
    uint8_t bexp = (uint8_t)(base >> 23);
    uint8_t exp  = (uint8_t)((result >> 23) + (0x7Fu - bexp));
    result = (result & 0x007FFFFFu) | ((uint32_t)exp << 23);
    if ((base & 0x80000000u) && off) result |= 0x80000000u;
    return result;
}

static inline uint32_t tgp_isqrt_r(uint32_t off) {
    uint32_t base = g_tgp.isqrt_base;
    uint32_t index = 0x2000u ^ (((base >> 10) & 0x3FFEu) | (off & 1u));
    uint32_t result = tgp_table(index | 0xC000u);
    uint8_t bexp = (uint8_t)((base >> 24) & 0x7Fu);
    uint8_t exp  = (uint8_t)((result >> 23) + (0x3Fu - bexp));
    result = (result & 0x807FFFFFu) | ((uint32_t)exp << 23);
    if (!(off & 1u)) result &= 0x7FFFFFFFu;
    return result;
}

static inline void tgp_atan_w(uint32_t off, uint32_t v) {
    g_tgp.atan_base[off & 3u] = v;
    g_tgp.gpio0 = (g_tgp.atan_base[0] & 0x7FFFFFFFu) <= (g_tgp.atan_base[1] & 0x7FFFFFFFu);
}

static inline uint32_t tgp_atan_r(void) {
    const uint32_t *ab = g_tgp.atan_base;
    uint8_t ie = (uint8_t)(0x88u - (ab[3] >> 23));
    bool s0 = ab[0] & 0x80000000u, s1 = ab[1] & 0x80000000u;
    bool s2 = (ab[0] & 0x7FFFFFFFu) <= (ab[1] & 0x7FFFFFFFu);
    uint32_t im = ab[3] & 0x7FFFFFu;
    uint32_t index = ie <= 0x17 ? (im | 0x800000u) >> ie : 0u;
    if (index == 0x4000u) index = 0x3FFFu;
    uint32_t result = tgp_table(index | 0x4000u);
    if (s0 ^ s1 ^ s2) result >>= 16;
    if (s2) result += 0x4000u;
    if ((s0 && !s2) || (s1 && s2)) result += 0x8000u;
    return result & 0xFFFFu;
}

/* ---- Address spaces ------------------------------------------------------ */

static inline uint32_t tgp_data_rd(uint32_t ea) {
    ea &= 0xFFFFu;
    if (ea < 0x100u) return g_tgp.ram0[ea];
    if (ea >= 0x200u && ea < 0x400u) return g_tgp.ram1[ea - 0x200u];
    return 0u;
}

static inline void tgp_data_wr(uint32_t ea, uint32_t v) {
    ea &= 0xFFFFu;
    if (ea < 0x100u) g_tgp.ram0[ea] = v;
    else if (ea >= 0x200u && ea < 0x400u) g_tgp.ram1[ea - 0x200u] = v;
}

static inline uint32_t tgp_prog_rd(uint32_t ea) {
    ea &= 0xFFFFu;
    return ea < TGP_PROG_WORDS ? g_tgp.prog[ea] : 0u;
}

/* I/O space: with bank bits 22-23 set, all of it is external memory (bank bit 23
 * the copro_data ROM, bit 22 bufferram); otherwise only the table ports answer
 * (MAME's view over 0x0000-0xFFFF hides them while a bank is selected). */
static inline uint32_t tgp_io_rd(uint32_t ea) {
    ea &= 0xFFFFu;
    if (g_tgp.bank_reg & 0xC00000u) {
        uint32_t adr = (g_tgp.bank_reg & 0xFF0000u) | ea;
        if (adr & 0x800000u)
            return g_tgp_copro_words ? tgp_le32(g_tgp_copro_data, adr & (g_tgp_copro_words - 1u)) : 0u;
        if (adr & 0x400000u)
            return g_tgp_bufferram ? tgp_le32(g_tgp_bufferram, adr & 0x7FFFu) : 0u;
        return 0u;
    }
    switch (ea) {
    case 0x20: case 0x21: case 0x22: case 0x23: return tgp_sincos_r(ea - 0x20u);
    case 0x24: case 0x25: case 0x26: case 0x27: return tgp_atan_r();
    case 0x28: case 0x29: return tgp_inv_r(ea - 0x28u);
    case 0x2A: case 0x2B: return tgp_isqrt_r(ea - 0x2Au);
    default: return 0u;
    }
}

static inline void tgp_io_wr(uint32_t ea, uint32_t v) {
    ea &= 0xFFFFu;
    if (g_tgp.bank_reg & 0xC00000u) {
        uint32_t adr = (g_tgp.bank_reg & 0xFF0000u) | ea;
        if ((adr & 0x400000u) && g_tgp_bufferram) {
            uint8_t *p = g_tgp_bufferram + (size_t)(adr & 0x7FFFu) * 4u;
            p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
        }
        return;
    }
    switch (ea) {
    case 0x20: case 0x21: case 0x22: case 0x23: g_tgp.sincos_base = v; break;
    case 0x24: case 0x25: case 0x26: case 0x27: tgp_atan_w(ea - 0x24u, v); break;
    case 0x28: case 0x29: g_tgp.inv_base = v; break;
    case 0x2A: case 0x2B: g_tgp.isqrt_base = v; break;
    default: break;
    }
}

/* Register file (model2.cpp copro_tgp_rf_map): 1 = input FIFO, 2 = output
 * FIFO, 3 = bank register, 0 = written by the programs (a busy flag or LEDs). */
static inline uint32_t tgp_rf_rd(uint32_t r) {
    if ((r & 0xFu) != 1u) return 0u;
    if (g_tgp.fin_n == 0) { g_tgp.stall = true; return 0u; }
    uint32_t v = g_tgp.fin[g_tgp.fin_r];
    g_tgp.fin_r = (g_tgp.fin_r + 1u) & (TGP_FIFO_IN - 1u);
    g_tgp.fin_n--;
    return v;
}

static inline void tgp_rf_wr(uint32_t r, uint32_t v) {
    switch (r & 0xFu) {
    case 2: tgp_out_push(v); break;
    case 3: g_tgp.bank_reg = v; break;
    default: break;
    }
}

/* ---- Registers ----------------------------------------------------------- */

static inline uint32_t tgp_read_reg(uint32_t r) {
    r &= 0x3Fu;
    if (r >= 0x20u && r < 0x30u) return tgp_rf_rd(r & 0x1Fu);
    switch (r) {
    case 0x00: return g_tgp.b0;
    case 0x01: return g_tgp.b1;
    case 0x02: return g_tgp.x0;
    case 0x03: return g_tgp.x1;
    case 0x0C: return g_tgp.c0;
    case 0x0D: return g_tgp.c1;
    case 0x10: return g_tgp.a;
    case 0x11: return tgp_get_exp(g_tgp.a);
    case 0x12: return tgp_get_mant(g_tgp.a);
    case 0x13: return g_tgp.b;
    case 0x14: return tgp_get_exp(g_tgp.b);
    case 0x15: return tgp_get_mant(g_tgp.b);
    case 0x19: return g_tgp.d;
    case 0x1A: return tgp_get_exp(g_tgp.d);
    case 0x1B: return tgp_get_mant(g_tgp.d);
    case 0x1C: return g_tgp.p;
    case 0x1D: return tgp_get_exp(g_tgp.p);
    case 0x1E: return tgp_get_mant(g_tgp.p);
    case 0x1F: return g_tgp.sft;
    case 0x34: return g_tgp.rpc;
    default:   return 0u;
    }
}

static inline void tgp_write_reg(uint32_t r, uint32_t v) {
    r &= 0x3Fu;
    if (r >= 0x20u && r < 0x30u) { tgp_rf_wr(r & 0x1Fu, v); return; }
    switch (r) {
    case 0x00: g_tgp.b0 = (uint16_t)v; break;
    case 0x01: g_tgp.b1 = (uint16_t)v; break;
    case 0x02: g_tgp.x0 = (uint16_t)v; break;
    case 0x03: g_tgp.x1 = (uint16_t)v; break;
    case 0x05: g_tgp.i0 = (uint16_t)v; break;
    case 0x06: g_tgp.i1 = (uint16_t)v; break;
    case 0x08: g_tgp.sp = (uint16_t)v; break;
    case 0x0A: g_tgp.vsm = (uint8_t)(v & 7u); g_tgp.vsmr = (uint16_t)((8u << g_tgp.vsm) - 1u); break;
    case 0x0C:
        g_tgp.c0 = (uint8_t)v;
        if (g_tgp.c0 == 1) g_tgp.st |= TGP_F_ZC0; else g_tgp.st &= ~(uint32_t)TGP_F_ZC0;
        break;
    case 0x0D:
        g_tgp.c1 = (uint8_t)v;
        if (g_tgp.c1 == 1) g_tgp.st |= TGP_F_ZC1; else g_tgp.st &= ~(uint32_t)TGP_F_ZC1;
        break;
    case 0x10: g_tgp.a = v; break;
    case 0x11: g_tgp.a = tgp_set_exp(g_tgp.a, v); break;
    case 0x12: g_tgp.a = tgp_set_mant(g_tgp.a, v); break;
    case 0x13: g_tgp.b = v; break;
    case 0x14: g_tgp.b = tgp_set_exp(g_tgp.b, v); break;
    case 0x15: g_tgp.b = tgp_set_mant(g_tgp.b, v); break;
    case 0x19: g_tgp.d = v; break;
    case 0x1A: g_tgp.d = tgp_set_exp(g_tgp.d, v); break;
    case 0x1B: g_tgp.d = tgp_set_mant(g_tgp.d, v); break;
    case 0x1C: g_tgp.p = v; break;
    case 0x1D: g_tgp.p = tgp_set_exp(g_tgp.p, v); break;
    case 0x1E: g_tgp.p = tgp_set_mant(g_tgp.p, v); break;
    case 0x1F: g_tgp.sft = (uint8_t)v; break;
    case 0x34: g_tgp.rpc = (uint8_t)v; break;
    case 0x3C: g_tgp.mask = (uint16_t)v; break;
    default: break;
    }
}

/* ---- ALU ------------------------------------------------------------------
 * alu_pre computes into temporaries; post_1 commits the integer ops before the
 * instruction's transfer is written, post_2 the floating-point ones after it
 * (MAME's note: a transfer beats an integer op to the same register, a float op
 * beats a transfer). */

static inline void tgp_sz_int(uint32_t v) {
    g_tgp.alu_stset = v ? ((v & 0x80000000u) ? TGP_F_SGD : 0u) : TGP_F_ZRD;
}
static inline void tgp_sz_fp(uint32_t v) {
    g_tgp.alu_stset = (v & 0x7FFFFFFFu) ? ((v & 0x80000000u) ? TGP_F_SGD : 0u) : TGP_F_ZRD;
}

static inline void tgp_alu_pre(uint32_t alu) {
    tgp_state_t *t = &g_tgp;
    uint8_t sft = t->sft;
    switch (alu) {
    case 0x00: break;
    case 0x01: t->alu_stmask = TGP_ALU_FLAGS; t->alu_r1 = t->d & t->a; tgp_sz_int(t->alu_r1); break;  /* andd */
    case 0x02: t->alu_stmask = TGP_ALU_FLAGS; t->alu_r1 = t->d | t->a; tgp_sz_int(t->alu_r1); break;  /* orad */
    case 0x03: t->alu_stmask = TGP_ALU_FLAGS; t->alu_r1 = t->d ^ t->a; tgp_sz_int(t->alu_r1); break;  /* eord */
    case 0x04: t->alu_stmask = TGP_ALU_FLAGS; t->alu_r1 = ~t->d;       tgp_sz_int(t->alu_r1); break;  /* notd */
    case 0x05:                                                                                         /* fcpd */
        t->alu_stmask = TGP_ALU_FLAGS;
        tgp_sz_fp(tgp_f2u(tgp_u2f(t->d) - tgp_u2f(t->a)));
        break;
    case 0x06: t->alu_stmask = TGP_ALU_FLAGS; t->alu_r1 = tgp_f2u(tgp_u2f(t->d) + tgp_u2f(t->a)); tgp_sz_fp(t->alu_r1); break; /* fadd */
    case 0x07: t->alu_stmask = TGP_ALU_FLAGS; t->alu_r1 = tgp_f2u(tgp_u2f(t->d) - tgp_u2f(t->a)); tgp_sz_fp(t->alu_r1); break; /* fsbd */
    case 0x08: t->alu_stmask = 0; t->alu_r1 = tgp_f2u(tgp_u2f(t->a) * tgp_u2f(t->b)); t->alu_stset = 0; break;               /* fml */
    case 0x09:                                                                                         /* fmsd */
        t->alu_stmask = TGP_ALU_FLAGS;
        t->alu_r1 = tgp_f2u(tgp_u2f(t->d) + tgp_u2f(t->p));
        t->alu_r2 = tgp_f2u(tgp_u2f(t->a) * tgp_u2f(t->b));
        tgp_sz_fp(t->alu_r1);
        break;
    case 0x0A:                                                                                         /* fmrd */
        t->alu_stmask = TGP_ALU_FLAGS;
        t->alu_r1 = tgp_f2u(tgp_u2f(t->d) - tgp_u2f(t->p));
        t->alu_r2 = tgp_f2u(tgp_u2f(t->a) * tgp_u2f(t->b));
        tgp_sz_fp(t->alu_r1);
        break;
    case 0x0B: t->alu_stmask = TGP_ALU_FLAGS; t->alu_r1 = t->d & 0x7FFFFFFFu; tgp_sz_fp(t->alu_r1); break;                 /* fabd */
    case 0x0C: t->alu_stmask = TGP_ALU_FLAGS; t->alu_r1 = tgp_f2u(tgp_u2f(t->d) + tgp_u2f(t->p)); tgp_sz_fp(t->alu_r1); break; /* fsmd */
    case 0x0D:                                                                                         /* fspd */
        t->alu_stmask = TGP_ALU_FLAGS;
        t->alu_r1 = t->p;
        t->alu_r2 = tgp_f2u(tgp_u2f(t->a) * tgp_u2f(t->b));
        tgp_sz_fp(t->alu_r1);
        break;
    case 0x0E: t->alu_stmask = TGP_ALU_FLAGS; t->alu_r1 = tgp_f2u((float)(int32_t)t->d); tgp_sz_int(t->alu_r1); break;    /* cxfd */
    case 0x0F: {                                                                                       /* cfxd */
        t->alu_stmask = TGP_ALU_FLAGS;
        float f = tgp_u2f(t->d);
        switch ((t->m >> 1) & 3u) {
        case 0: t->alu_r1 = tgp_f2i(roundf(f)); break;
        case 1: t->alu_r1 = tgp_f2i(ceilf(f));  break;
        case 2: t->alu_r1 = tgp_f2i(floorf(f)); break;
        default: t->alu_r1 = tgp_f2i(f);        break;
        }
        tgp_sz_int(t->alu_r1);
        break;
    }
    case 0x10: t->alu_stmask = TGP_ALU_FLAGS; t->alu_r1 = tgp_f2u(tgp_u2f(t->d) / tgp_u2f(t->a)); tgp_sz_fp(t->alu_r1); break; /* fdvd */
    case 0x11: t->alu_stmask = TGP_ALU_FLAGS; t->alu_r1 = t->d ? t->d ^ 0x80000000u : 0u; tgp_sz_fp(t->alu_r1); break;      /* fned */
    case 0x13: t->alu_stmask = TGP_ALU_FLAGS; t->alu_r1 = tgp_f2u(tgp_u2f(t->b) + tgp_u2f(t->a)); tgp_sz_fp(t->alu_r1); break; /* d = b + a */
    case 0x14: t->alu_stmask = TGP_ALU_FLAGS; t->alu_r1 = tgp_f2u(tgp_u2f(t->b) - tgp_u2f(t->a)); tgp_sz_fp(t->alu_r1); break; /* d = b - a */
    /* Shifts by 32 or more are undefined in C; the chip's answer is not known,
     * so take the one x86 gives (count mod 32), as MAME does in practice. */
    case 0x16: t->alu_stmask = TGP_ALU_FLAGS; t->alu_r1 = t->d >> (sft & 31u); tgp_sz_int(t->alu_r1); break;                         /* lsrd */
    case 0x17: t->alu_stmask = TGP_ALU_FLAGS; t->alu_r1 = t->d << (sft & 31u); tgp_sz_int(t->alu_r1); break;                         /* lsld */
    case 0x18: t->alu_stmask = TGP_ALU_FLAGS; t->alu_r1 = (uint32_t)((int32_t)t->d >> (sft & 31u)); tgp_sz_int(t->alu_r1); break;    /* asrd */
    case 0x19: t->alu_stmask = TGP_ALU_FLAGS; t->alu_r1 = t->d << (sft & 31u); tgp_sz_int(t->alu_r1); break;                         /* asld */
    case 0x1A: t->alu_stmask = TGP_ALU_FLAGS; t->alu_r1 = t->d + t->a; tgp_sz_int(t->alu_r1); break;   /* addd */
    case 0x1B: t->alu_stmask = TGP_ALU_FLAGS; t->alu_r1 = t->d - t->a; tgp_sz_int(t->alu_r1); break;   /* subd */
    default: {
        static uint32_t warned;
        if (!(warned & (1u << (alu & 31u)))) {
            warned |= 1u << (alu & 31u);
            LOG_WARN("TGP: unhandled alu op %02X at %03X", alu, g_tgp.ppc);
        }
        break;
    }
    }
}

static inline void tgp_alu_st(void) {
    g_tgp.st = (g_tgp.st & ~g_tgp.alu_stmask) | g_tgp.alu_stset;
}

static inline void tgp_alu_post_1(uint32_t alu) {
    switch (alu) {
    case 0x01: case 0x02: case 0x03: case 0x04:
    case 0x0E: case 0x0F: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B:
        g_tgp.d = g_tgp.alu_r1; tgp_alu_st(); break;
    default: break;
    }
}

static inline void tgp_alu_post_2(uint32_t alu) {
    switch (alu) {
    case 0x05: tgp_alu_st(); break;
    case 0x06: case 0x07: case 0x0B: case 0x0C:
    case 0x10: case 0x11: case 0x13: case 0x14:
        g_tgp.d = g_tgp.alu_r1; tgp_alu_st(); break;
    case 0x08: g_tgp.p = g_tgp.alu_r1; break;
    case 0x09: case 0x0A: case 0x0D:
        g_tgp.d = g_tgp.alu_r1; g_tgp.p = g_tgp.alu_r2; tgp_alu_st(); break;
    default: break;
    }
}

/* ---- Effective addresses ------------------------------------------------- */

static inline uint16_t tgp_ea_pre(uint32_t r, uint16_t b, uint16_t x) {
    switch (r & 0x180u) {
    case 0x000: return (uint16_t)(r & 0x7Fu);
    case 0x080: case 0x100: return (uint16_t)((r & 0x7Fu) + b + x);
    default:
        switch (r & 0x60u) {
        case 0x00: return (uint16_t)(b + x);
        case 0x20: return x;
        case 0x40: return (uint16_t)(b + (x & g_tgp.vsmr));
        default:   return (uint16_t)(x & g_tgp.vsmr);
        }
    }
}

static inline void tgp_ea_post(uint32_t r, uint16_t *x, uint16_t i) {
    if (!(r & 0x100u)) return;
    if (!(r & 0x080u)) *x = (uint16_t)(*x + i);
    else               *x = (uint16_t)(*x + tgp_sext(r, 5));
}

#define TGP_EA0(r)      tgp_ea_pre((r), g_tgp.b0, g_tgp.x0)
#define TGP_EA1(r)      tgp_ea_pre((r), g_tgp.b1, g_tgp.x1)
#define TGP_POST0(r)    tgp_ea_post((r), &g_tgp.x0, g_tgp.i0)
#define TGP_POST1(r)    tgp_ea_post((r), &g_tgp.x1, g_tgp.i1)

static inline void tgp_write_internal_1(uint32_t r, uint32_t v, bool bank) {
    uint16_t ea = TGP_EA1(r);
    tgp_data_wr(bank ? ea + 0x200u : ea, v);
    TGP_POST1(r);
}

static inline void tgp_write_io_1(uint32_t r, uint32_t v) {
    tgp_io_wr(TGP_EA1(r), v);
    TGP_POST1(r);
}

static inline void tgp_pcs_push(void) {
    for (int i = 3; i; i--) g_tgp.pcs[i] = g_tgp.pcs[i - 1];
    g_tgp.pcs[0] = g_tgp.pc;
}

static inline void tgp_pcs_pop(void) {
    g_tgp.pc = g_tgp.pcs[0];
    for (int i = 0; i != 3; i++) g_tgp.pcs[i] = g_tgp.pcs[i + 1];
}

/* ---- One instruction (MAME execute_run) ----------------------------------
 * Returns false when it stalled on an empty input FIFO: the pc is back on the
 * instruction, nothing was committed, and it runs again once a word arrives. */
static bool tgp_step(void) {
    tgp_state_t *t = &g_tgp;
    t->ppc = t->pc;
    uint32_t op = tgp_prog_rd(t->pc++);
    bool rep = false;
    t->stall = false;
    t->insns++;

    switch ((op >> 26) & 0x3Fu) {
    case 0x00: {                                    /* lab */
        uint32_t r1 = op & 0x1FFu, r2 = (op >> 9) & 0x1FFu;
        uint32_t alu = (op >> 21) & 0x1Fu, sub = (op >> 18) & 7u;
        tgp_alu_pre(alu);
        uint32_t v1, v2;
        switch (sub) {
        case 0: case 1:                             /* lab mem, mem (e) */
            v1 = tgp_data_rd(TGP_EA0(r1));
            v2 = tgp_io_rd(TGP_EA1(r2));
            break;
        case 3:                                     /* lab mem, mem + 0x200 */
            v1 = tgp_data_rd(TGP_EA0(r1));
            v2 = tgp_data_rd(TGP_EA1(r2) + 0x200u);
            break;
        case 4:                                     /* lab mem + 0x200, mem */
            v1 = tgp_data_rd(TGP_EA0(r1) + 0x200u);
            v2 = tgp_data_rd(TGP_EA1(r2));
            break;
        default:
            LOG_WARN("TGP: unhandled lab subop %u at %03X", sub, t->ppc);
            goto lab_done;
        }
        TGP_POST0(r1);
        TGP_POST1(r2);
        t->a = v1;
        t->b = v2;
    lab_done:
        tgp_alu_post_1(alu);
        tgp_alu_post_2(alu);
        break;
    }

    case 0x07: {                                    /* ld / mov */
        uint32_t r1 = op & 0x1FFu, r2 = (op >> 9) & 0x1FFu;
        uint32_t alu = (op >> 21) & 0x1Fu, sub = (op >> 18) & 7u;
        uint32_t v;
        tgp_alu_pre(alu);
        switch (sub) {
        case 0: case 1:                             /* mov mem, mem (e) */
            v = tgp_data_rd(TGP_EA0(r1)); TGP_POST0(r1);
            tgp_alu_post_1(alu); tgp_write_io_1(r2, v);
            break;
        case 2:                                     /* mov mem (e), mem */
            v = tgp_io_rd(TGP_EA0(r1)); TGP_POST0(r1);
            tgp_alu_post_1(alu); tgp_write_internal_1(r2, v, false);
            break;
        case 3:                                     /* mov mem, mem + 0x200 */
            v = tgp_data_rd(TGP_EA0(r1)); TGP_POST0(r1);
            tgp_alu_post_1(alu); tgp_write_internal_1(r2, v, true);
            break;
        case 4:                                     /* mov mem + 0x200, mem */
            v = tgp_data_rd(TGP_EA0(r1) + 0x200u); TGP_POST0(r1);
            tgp_alu_post_1(alu); tgp_write_internal_1(r2, v, false);
            break;
        case 5:                                     /* mov mem (o), mem */
            v = tgp_prog_rd(TGP_EA0(r1)); TGP_POST0(r1);
            tgp_alu_post_1(alu); tgp_write_internal_1(r2, v, false);
            break;
        case 7:
            switch (r2 >> 6) {
            case 0:                                 /* mov reg, mem */
                v = tgp_read_reg(r2); if (t->stall) goto stalled;
                tgp_alu_post_1(alu); tgp_write_internal_1(r1, v, false);
                break;
            case 1:                                 /* mov reg, mem (e) */
                v = tgp_read_reg(r2); if (t->stall) goto stalled;
                tgp_alu_post_1(alu); tgp_write_io_1(r1, v);
                break;
            case 2:                                 /* mov mem + 0x200, reg */
                v = tgp_data_rd(TGP_EA1(r1) + 0x200u); TGP_POST1(r1);
                tgp_alu_post_1(alu); tgp_write_reg(r2, v);
                break;
            case 3:                                 /* mov mem, reg */
                v = tgp_data_rd(TGP_EA1(r1)); TGP_POST1(r1);
                tgp_alu_post_1(alu); tgp_write_reg(r2, v);
                break;
            case 4:                                 /* mov mem (e), reg */
                v = tgp_io_rd(TGP_EA1(r1)); TGP_POST1(r1);
                tgp_alu_post_1(alu); tgp_write_reg(r2, v);
                break;
            case 5:                                 /* mov mem (o), reg */
                v = tgp_prog_rd(TGP_EA0(r1)); TGP_POST0(r1);
                tgp_alu_post_1(alu); tgp_write_reg(r2, v);
                break;
            case 6:                                 /* mov reg, reg */
                v = tgp_read_reg(r1); if (t->stall) goto stalled;
                tgp_alu_post_1(alu); tgp_write_reg(r2, v);
                break;
            default:
                tgp_alu_post_1(alu);
                LOG_WARN("TGP: unhandled ld/mov subop 7/%u at %03X", r2 >> 6, t->ppc);
                break;
            }
            break;
        default:
            tgp_alu_post_1(alu);
            LOG_WARN("TGP: unhandled ld/mov subop %u at %03X", sub, t->ppc);
            break;
        }
        tgp_alu_post_2(alu);
        break;
    }

    case 0x0D:                                      /* stm / clm */
        if (((op >> 17) & 7u) == 5) t->m = (uint16_t)op;   /* stmh: bit 0 float, bits 1-2 rounding */
        else LOG_WARN("TGP: unimplemented opcode 0D/%u at %03X", (op >> 17) & 7u, t->ppc);
        break;

    case 0x0E:                                      /* lipl / lia / lib / lid */
        switch ((op >> 24) & 3u) {
        case 0: t->p = (t->p & 0xFF000000u) | (op & 0xFFFFFFu); break;
        case 1: t->a = tgp_sext(op, 24); break;
        case 2: t->b = tgp_sext(op, 24); break;
        case 3: t->d = tgp_sext(op, 24); break;
        }
        break;

    case 0x0F: {                                    /* rep / clr0 / clr1 / set */
        uint32_t alu = (op >> 20) & 0x1Fu;
        tgp_alu_pre(alu);
        switch ((op >> 17) & 7u) {
        case 0:                                     /* clr0 */
            if (op & 0x0004u) t->a = 0;
            if (op & 0x0008u) t->b = 0;
            if (op & 0x0010u) t->d = 0;
            break;
        case 1: break;                              /* clr1: flag mapping unknown */
        case 2: {                                   /* rep */
            uint32_t n = (op & 0x8000u) ? tgp_read_reg(op) : op;
            if (t->stall) goto stalled;
            t->r = (uint8_t)n;
            rep = true;
            break;
        }
        case 3: break;                              /* set: flag mapping unknown (0x800 = interrupt enable) */
        default:
            LOG_WARN("TGP: unimplemented opcode 0F/%u at %03X", (op >> 17) & 7u, t->ppc);
            break;
        }
        if (rep) return true;                       /* MAME: rep_start skips the repeat count and post_1 */
        tgp_alu_post_1(alu);
        break;
    }

    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        tgp_write_reg(op >> 24, tgp_sext(op, 24));  /* ldi */
        break;

    case 0x2F: case 0x3F: {                         /* conditional branches */
        uint32_t cond = (op >> 20) & 0x1Fu, sub = (op >> 17) & 7u, data = op & 0xFFFFu;
        bool pass = false;
        switch (cond) {
        case 0x00: pass = t->st & TGP_F_ZRD; break;                     /* zrd */
        case 0x01: pass = !(t->st & TGP_F_SGD); break;                  /* ged */
        case 0x02: pass = t->st & (TGP_F_ZRD | TGP_F_SGD); break;       /* led */
        case 0x0A: pass = t->gpio0; break;
        case 0x0B: pass = t->gpio1; break;
        case 0x0C: pass = t->gpio2; break;
        case 0x10: pass = !(t->st & TGP_F_ZC0); break;                  /* c0 != 1 */
        case 0x11: pass = !(t->st & TGP_F_ZC1); break;
        case 0x12: pass = t->gpio3; break;
        case 0x16: pass = true; break;                                  /* alw */
        default: {
            static uint32_t warned;
            if (!(warned & (1u << cond))) {
                warned |= 1u << cond;
                LOG_WARN("TGP: unimplemented condition %02X at %03X", cond, t->ppc);
            }
            break;
        }
        }
        if (op & 0x40000000u) pass = !pass;
        if (pass) {
            uint32_t v;
            switch (sub) {
            case 0: t->pc = (uint16_t)data; break;                      /* brif #adr */
            case 1:                                                     /* brul */
                if (op & 0x4000u) { v = tgp_read_reg(op); if (t->stall) goto stalled; }
                else { v = tgp_data_rd(TGP_EA0(op)); TGP_POST0(op); }
                t->pc = (uint16_t)v;
                break;
            case 2: tgp_pcs_push(); t->pc = (uint16_t)data; break;      /* bsif #adr */
            case 3:                                                     /* bsul */
                if (op & 0x4000u) { v = tgp_read_reg(op); if (t->stall) goto stalled; }
                else { v = tgp_data_rd(TGP_EA0(op)); TGP_POST0(op); }
                tgp_pcs_push();
                t->pc = (uint16_t)v;
                break;
            case 5: tgp_pcs_pop(); break;                               /* rtif */
            case 6:                                                     /* ldif adr, rn */
                v = tgp_data_rd(TGP_EA0(op)); TGP_POST0(op);
                tgp_write_reg(op >> 9, v);
                break;
            default:
                LOG_WARN("TGP: unimplemented branch subtype %u at %03X", sub, t->ppc);
                break;
            }
        }
        if (sub < 2) {
            if (cond == 0x10 && t->c0 != 1) { if (--t->c0 == 1) t->st |= TGP_F_ZC0; }
            if (cond == 0x11 && t->c1 != 1) { if (--t->c1 == 1) t->st |= TGP_F_ZC1; }
        }
        break;
    }

    default: {
        static uint64_t warned;
        uint32_t k = (op >> 26) & 0x3Fu;
        if (!(warned & (1ull << k))) {
            warned |= 1ull << k;
            LOG_WARN("TGP: unimplemented opcode type %02X at %03X (%08X)", k, t->ppc, op);
        }
        break;
    }
    }

    if (t->r != 1) { t->pc = t->ppc; t->r--; }
    return true;

stalled:
    /* The FIFO read found nothing: nothing of this instruction is committed and
     * it runs again when a word arrives. The only reads that can stall are
     * register reads of RF 1, which come before any write in every form. */
    t->pc = t->ppc;
    t->insns--;
    return false;
}

/* Run until the program waits for input (or is halted). */
static void tgp_run(void) {
    if (g_tgp.halted) return;
    for (uint32_t n = 0; n < TGP_RUN_BUDGET; n++)
        if (!tgp_step()) return;
    if (g_tgp.budget_stops++ < 8)
        LOG_WARN("TGP: ran %u instructions without reading its FIFO (pc %03X); stopped",
                 TGP_RUN_BUDGET, g_tgp.pc);
}

/* ---- Board side ---------------------------------------------------------- */

static inline void tgp_cpu_reset(void) {
    tgp_state_t *t = &g_tgp;
    t->pc = t->ppc = 0;
    t->st = TGP_F_ZRC | TGP_F_ZRD | TGP_F_ZX0 | TGP_F_ZX1 | TGP_F_ZX2 | TGP_F_ZC0 | TGP_F_ZC1;
    t->sp = 0;
    t->a = t->b = t->d = t->p = 0;
    t->r = t->rpc = t->c0 = t->c1 = 1;
    t->b0 = t->b1 = t->x0 = t->x1 = t->i0 = t->i1 = 0;
    t->sft = t->vsm = 0;
    t->vsmr = 7;
    t->mask = 0;
    t->m = 1;
    t->alu_stmask = t->alu_stset = t->alu_r1 = t->alu_r2 = 0;
    memset(t->pcs, 0, sizeof t->pcs);
    t->stall = false;
}

/* Power-on / board reset: the TGP is held until the i960 has uploaded a program
 * (model2_tgp_state::machine_reset). */
static inline void tgp_reset(void) {
    memset(&g_tgp, 0, sizeof g_tgp);
    tgp_cpu_reset();
    g_tgp.halted = true;
}

/* A word into the input FIFO, from the FIFO port or the function port. */
static inline void tgp_push(uint32_t v) {
    g_tgp.pushes++;
    if (g_tgp.fin_n >= TGP_FIFO_IN) {
        /* Only while the program is halted (it drains the FIFO otherwise). */
        g_tgp.overflows++;
        return;
    }
    g_tgp.fin[(g_tgp.fin_r + g_tgp.fin_n++) & (TGP_FIFO_IN - 1u)] = v;
    tgp_run();
}

/* i960 0x880000..0x883FFF (model2.cpp copro_function_port_w). */
static inline void tgp_function_port_w(uint32_t byte_off, uint32_t data) {
    tgp_push((data & 0x800FFFFFu) | (((byte_off >> 4) & 0xFFu) << 23));
}

/* i960 0x884000..0x887FFF write. */
static inline void tgp_fifo_w(uint32_t data) {
    if (g_tgp.coproctl & 0x80000000u) {
        g_tgp.prog[g_tgp.coprocnt & (TGP_PROG_WORDS - 1u)] = data;
        g_tgp.coprocnt++;
    } else {
        tgp_push(data);
    }
}

/* i960 0x884000..0x887FFF read. On the board an empty FIFO stalls the i960
 * until the TGP answers; here the TGP has already run as far as its input
 * lets it, so an empty FIFO means it is waiting for words the i960 has not
 * sent, which is the i960 waiting on itself. */
static inline uint32_t tgp_fifo_r(void) {
    g_tgp.pops++;
    if (g_tgp.fout_n == 0) {
        if (g_tgp.empty_pops++ < 16)
            LOG_WARN("TGP: i960 read an empty reply FIFO (TGP pc %03X)", g_tgp.pc);
        return 0u;
    }
    uint32_t v = g_tgp.fout[g_tgp.fout_r];
    g_tgp.fout_r = (g_tgp.fout_r + 1u) & (TGP_FIFO_OUT - 1u);
    g_tgp.fout_n--;
    return v;
}

/* i960 0x980000 (model2.cpp copro_ctl1_w): only a change of bit 31 alone
 * starts an upload (halting the TGP) or boots what was uploaded. */
static inline void tgp_ctl_w(uint32_t data) {
    if ((data ^ g_tgp.coproctl) == 0x80000000u) {
        if (data & 0x80000000u) {
            g_tgp.coprocnt = 0;
            g_tgp.halted = true;
        } else {
            LOG_INFO("TGP: boot, %u program words", g_tgp.coprocnt);
            g_tgp.coproctl = data;
            g_tgp.halted = false;
            tgp_cpu_reset();
            tgp_run();
            return;
        }
    }
    g_tgp.coproctl = data;
}

#endif /* TGP_H */
