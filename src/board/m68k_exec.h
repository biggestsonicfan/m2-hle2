/*
 * m68k_exec.h — MC68000 instruction executor.
 *
 * Call m68k_startup() once after ROM is loaded to set PC/SSP from the reset
 * vectors, then call m68k_step() to execute one instruction per call.
 *
 * 68K address space (Model 2B/2C sound block):
 *   0x000000–0x07FFFF  sound program ROM (read-only)
 *   0x100000–0x10FFFF  SCSP registers (stub)
 *   0x200000–0x27FFFF  SCSP wave RAM (512 KB)
 *   0xFF8000–0xFFFFFF  68K on-chip work RAM (32 KB)
 *
 * Implementation status:
 *   Implemented:  MOVE/MOVEM/MOVEQ/LEA/PEA, ADD/SUB/CMP/AND/OR/EOR,
 *                 NOT/NEG/NEGX/CLR/TST/EXT/SWAP, ADDQ/SUBQ/ADDX/SUBX,
 *                 BCC/BRA/BSR/JMP/JSR/RTS/RTE/RTR, LINK/UNLK,
 *                 Scc/DBcc, BTST/BCHG/BCLR/BSET (imm + reg),
 *                 LSL/LSR/ASL/ASR/ROL/ROR/ROXL/ROXR,
 *                 MULU/MULS, DIVU/DIVS, EXG,
 *                 NOP/RESET/STOP/ILLEGAL/TRAP/TRAPV,
 *                 MOVE CCR/SR, MOVE to CCR/SR, MOVE USP.
 *   Stubbed:      ABCD/SBCD/NBCD, PACK/UNPK, CHK, TAS.
 */
#ifndef M68K_EXEC_H
#define M68K_EXEC_H

#include <stdint.h>
#include "m68k.h"
#include "log.h"

/* ================================================================ helpers */

#define SZ_B 1
#define SZ_W 2
#define SZ_L 4

static inline uint32_t m68k_sz_mask(int sz) {
    return sz == SZ_B ? 0xFFu : sz == SZ_W ? 0xFFFFu : 0xFFFFFFFFu;
}
static inline uint32_t m68k_sz_msb(int sz) {
    return sz == SZ_B ? 0x80u : sz == SZ_W ? 0x8000u : 0x80000000u;
}
static inline int32_t m68k_sign_ext(uint32_t v, int sz) {
    if (sz == SZ_B) return (int8_t)(uint8_t)v;
    if (sz == SZ_W) return (int16_t)(uint16_t)v;
    return (int32_t)v;
}

/* ---- bus access ---- */
static inline uint8_t  m68k_rb(m68k_state_t *s, uint32_t a)
    { return (uint8_t)s->read_cb(s->mem_ctx, a & 0xFFFFFFu, 1); }
static inline uint16_t m68k_rw(m68k_state_t *s, uint32_t a)
    { return (uint16_t)s->read_cb(s->mem_ctx, a & 0xFFFFFFu, 2); }
static inline uint32_t m68k_rl(m68k_state_t *s, uint32_t a)
    { return s->read_cb(s->mem_ctx, a & 0xFFFFFFu, 4); }
static inline void m68k_wb(m68k_state_t *s, uint32_t a, uint8_t  v)
    { s->write_cb(s->mem_ctx, a & 0xFFFFFFu, v, 1); }
static inline void m68k_ww(m68k_state_t *s, uint32_t a, uint16_t v)
    { s->write_cb(s->mem_ctx, a & 0xFFFFFFu, v, 2); }
static inline void m68k_wl(m68k_state_t *s, uint32_t a, uint32_t v)
    { s->write_cb(s->mem_ctx, a & 0xFFFFFFu, v, 4); }

/* ---- instruction stream fetch ---- */
static inline uint16_t m68k_fetch(m68k_state_t *s) {
    uint16_t w = m68k_rw(s, s->cpu.pc);
    s->cpu.pc += 2;
    return w;
}
static inline uint32_t m68k_fetch_l(m68k_state_t *s) {
    uint32_t v = m68k_rl(s, s->cpu.pc);
    s->cpu.pc += 4;
    return v;
}

/* ---- stack (always A7) ---- */
static inline void     m68k_push_l(m68k_state_t *s, uint32_t  v) { s->cpu.a[7] -= 4; m68k_wl(s, s->cpu.a[7], v); }
static inline void     m68k_push_w(m68k_state_t *s, uint16_t  v) { s->cpu.a[7] -= 2; m68k_ww(s, s->cpu.a[7], v); }
static inline uint32_t m68k_pop_l (m68k_state_t *s) { uint32_t v = m68k_rl(s, s->cpu.a[7]); s->cpu.a[7] += 4; return v; }
static inline uint16_t m68k_pop_w (m68k_state_t *s) { uint16_t v = m68k_rw(s, s->cpu.a[7]); s->cpu.a[7] += 2; return v; }

/* ---- write to Dn preserving upper bytes for sub-word ops ---- */
static inline void m68k_dn_write(m68k_cpu_t *c, int n, uint32_t v, int sz) {
    uint32_t mask = m68k_sz_mask(sz);
    c->d[n] = (c->d[n] & ~mask) | (v & mask);
}

/* ================================================================ flags */

/* Logic ops: clear C/V, update N/Z. */
static inline void m68k_flags_logic(m68k_cpu_t *c, uint32_t r, int sz) {
    c->sr &= ~(M68K_SR_N | M68K_SR_Z | M68K_SR_V | M68K_SR_C);
    if ((r & m68k_sz_mask(sz)) == 0) c->sr |= M68K_SR_Z;
    if (r & m68k_sz_msb(sz))         c->sr |= M68K_SR_N;
}

/* ADD flags: update N, Z, V, C, X. */
static inline void m68k_flags_add(m68k_cpu_t *c, uint32_t dst, uint32_t src, int sz) {
    uint32_t mask = m68k_sz_mask(sz), msb = m68k_sz_msb(sz);
    uint64_t u    = (uint64_t)(dst & mask) + (uint64_t)(src & mask);
    uint32_t r    = (uint32_t)u & mask;
    int carry     = (int)(u >> (sz * 8)) & 1;
    int v         = (~(dst ^ src) & (dst ^ r) & msb) != 0;
    c->sr &= ~(M68K_SR_N | M68K_SR_Z | M68K_SR_V | M68K_SR_C | M68K_SR_X);
    if (r == 0)  c->sr |= M68K_SR_Z;
    if (r & msb) c->sr |= M68K_SR_N;
    if (v)       c->sr |= M68K_SR_V;
    if (carry)   c->sr |= M68K_SR_C | M68K_SR_X;
}

/* ADDX flags: like ADD but preserve Z if result is non-zero (extended add). */
static inline void m68k_flags_addx(m68k_cpu_t *c, uint32_t dst, uint32_t src, int sz) {
    uint32_t mask = m68k_sz_mask(sz), msb = m68k_sz_msb(sz);
    uint64_t u    = (uint64_t)(dst & mask) + (uint64_t)(src & mask);
    uint32_t r    = (uint32_t)u & mask;
    int carry     = (int)(u >> (sz * 8)) & 1;
    int v         = (~(dst ^ src) & (dst ^ r) & msb) != 0;
    /* Z is cleared if result non-zero, but NOT set if result is zero */
    if (r != 0)  c->sr &= ~M68K_SR_Z;
    c->sr &= ~(M68K_SR_N | M68K_SR_V | M68K_SR_C | M68K_SR_X);
    if (r & msb) c->sr |= M68K_SR_N;
    if (v)       c->sr |= M68K_SR_V;
    if (carry)   c->sr |= M68K_SR_C | M68K_SR_X;
}

/* SUB flags: update N, Z, V, C, X.  Borrow sets C/X. */
static inline void m68k_flags_sub(m68k_cpu_t *c, uint32_t dst, uint32_t src, int sz) {
    uint32_t mask = m68k_sz_mask(sz), msb = m68k_sz_msb(sz);
    uint64_t u    = (uint64_t)(dst & mask) - (uint64_t)(src & mask);
    uint32_t r    = (uint32_t)u & mask;
    int borrow    = (int)(u >> 63) & 1;
    int v         = ((dst ^ src) & (dst ^ r) & msb) != 0;
    c->sr &= ~(M68K_SR_N | M68K_SR_Z | M68K_SR_V | M68K_SR_C | M68K_SR_X);
    if (r == 0)  c->sr |= M68K_SR_Z;
    if (r & msb) c->sr |= M68K_SR_N;
    if (v)       c->sr |= M68K_SR_V;
    if (borrow)  c->sr |= M68K_SR_C | M68K_SR_X;
}

/* SUBX: like SUB but preserve Z if result non-zero. */
static inline void m68k_flags_subx(m68k_cpu_t *c, uint32_t dst, uint32_t src, int sz) {
    uint32_t mask = m68k_sz_mask(sz), msb = m68k_sz_msb(sz);
    uint64_t u    = (uint64_t)(dst & mask) - (uint64_t)(src & mask);
    uint32_t r    = (uint32_t)u & mask;
    int borrow    = (int)(u >> 63) & 1;
    int v         = ((dst ^ src) & (dst ^ r) & msb) != 0;
    if (r != 0)  c->sr &= ~M68K_SR_Z;
    c->sr &= ~(M68K_SR_N | M68K_SR_V | M68K_SR_C | M68K_SR_X);
    if (r & msb) c->sr |= M68K_SR_N;
    if (v)       c->sr |= M68K_SR_V;
    if (borrow)  c->sr |= M68K_SR_C | M68K_SR_X;
}

/* CMP: same arithmetic as SUB but X is not modified. */
static inline void m68k_flags_cmp(m68k_cpu_t *c, uint32_t dst, uint32_t src, int sz) {
    uint32_t mask = m68k_sz_mask(sz), msb = m68k_sz_msb(sz);
    uint64_t u    = (uint64_t)(dst & mask) - (uint64_t)(src & mask);
    uint32_t r    = (uint32_t)u & mask;
    int borrow    = (int)(u >> 63) & 1;
    int v         = ((dst ^ src) & (dst ^ r) & msb) != 0;
    c->sr &= ~(M68K_SR_N | M68K_SR_Z | M68K_SR_V | M68K_SR_C);
    if (r == 0)  c->sr |= M68K_SR_Z;
    if (r & msb) c->sr |= M68K_SR_N;
    if (v)       c->sr |= M68K_SR_V;
    if (borrow)  c->sr |= M68K_SR_C;
    /* X unchanged */
}

/* NEG: flags as if subtracting from zero. */
static inline void m68k_flags_neg(m68k_cpu_t *c, uint32_t src, int sz) {
    m68k_flags_sub(c, 0, src, sz);
    /* C is set if result non-zero (unlike SUB: C set if borrow) */
    /* Actually for NEG: C is set if src != 0 */
    if (src & m68k_sz_mask(sz)) c->sr |= M68K_SR_C; else c->sr &= ~M68K_SR_C;
}

/* NEGX: negate with extend. */
static inline uint32_t m68k_do_negx(m68k_cpu_t *c, uint32_t src, int sz) {
    int x = (c->sr & M68K_SR_X) != 0;
    uint32_t mask = m68k_sz_mask(sz), msb = m68k_sz_msb(sz);
    uint64_t u = 0 - (uint64_t)(src & mask) - x;
    uint32_t r = (uint32_t)u & mask;
    int borrow = (int)(u >> 63) & 1;
    int v      = (src & msb) != 0 && (r & msb) != 0; /* both source and result negative */
    if (r != 0) c->sr &= ~M68K_SR_Z;
    c->sr &= ~(M68K_SR_N | M68K_SR_V | M68K_SR_C | M68K_SR_X);
    if (r & msb) c->sr |= M68K_SR_N;
    if (v)       c->sr |= M68K_SR_V;
    if (borrow)  c->sr |= M68K_SR_C | M68K_SR_X;
    return r;
}

/* ================================================================ condition codes */

static inline int m68k_test_cc(const m68k_cpu_t *c, int cc) {
    int C = (c->sr & M68K_SR_C) != 0;
    int V = (c->sr & M68K_SR_V) != 0;
    int Z = (c->sr & M68K_SR_Z) != 0;
    int N = (c->sr & M68K_SR_N) != 0;
    switch (cc & 0xF) {
    case  0: return 1;           /* T  — true               */
    case  1: return 0;           /* F  — false              */
    case  2: return !C && !Z;    /* HI — higher             */
    case  3: return  C ||  Z;    /* LS — lower or same      */
    case  4: return !C;          /* CC/HS — carry clear     */
    case  5: return  C;          /* CS/LO — carry set       */
    case  6: return !Z;          /* NE — not equal          */
    case  7: return  Z;          /* EQ — equal              */
    case  8: return !V;          /* VC — overflow clear     */
    case  9: return  V;          /* VS — overflow set       */
    case 10: return !N;          /* PL — plus               */
    case 11: return  N;          /* MI — minus              */
    case 12: return  N == V;     /* GE — greater or equal   */
    case 13: return  N != V;     /* LT — less than          */
    case 14: return !Z && (N==V);/* GT — greater than       */
    case 15: return  Z || (N!=V);/* LE — less or equal      */
    }
    return 0;
}

/* ================================================================ supervisor / SP bank switching */

static inline void m68k_enter_supervisor(m68k_cpu_t *c) {
    if (!(c->sr & M68K_SR_S)) {
        c->usp  = c->a[7];
        c->a[7] = c->ssp;
        c->sr  |= M68K_SR_S;
    }
}
static inline void m68k_leave_supervisor(m68k_cpu_t *c) {
    if (c->sr & M68K_SR_S) {
        c->ssp  = c->a[7];
        c->a[7] = c->usp;
        c->sr  &= ~M68K_SR_S;
    }
}

/* ================================================================ effective address */

/*
 * m68k_ea_read — decode EA (mode:reg) from the instruction stream, apply any
 * pre/post-increment side effects, and return the value of the given size.
 */
static uint32_t m68k_ea_read(m68k_state_t *s, int mode, int reg, int sz) {
    switch (mode) {
    case 0: /* Dn */ return s->cpu.d[reg] & m68k_sz_mask(sz);
    case 1: /* An — sign-extend for word size */
        return (sz == SZ_W) ? (uint32_t)(int16_t)(uint16_t)s->cpu.a[reg] : s->cpu.a[reg];
    case 2: /* (An) */ {
        uint32_t a = s->cpu.a[reg];
        return sz==SZ_B ? m68k_rb(s,a) : sz==SZ_W ? m68k_rw(s,a) : m68k_rl(s,a);
    }
    case 3: /* (An)+ */ {
        uint32_t a = s->cpu.a[reg];
        /* A7 (SP) always increments by at least 2 to stay word-aligned */
        s->cpu.a[reg] += (sz == SZ_B && reg == 7) ? 2 : sz;
        return sz==SZ_B ? m68k_rb(s,a) : sz==SZ_W ? m68k_rw(s,a) : m68k_rl(s,a);
    }
    case 4: /* -(An) */ {
        s->cpu.a[reg] -= (sz == SZ_B && reg == 7) ? 2 : sz;
        uint32_t a = s->cpu.a[reg];
        return sz==SZ_B ? m68k_rb(s,a) : sz==SZ_W ? m68k_rw(s,a) : m68k_rl(s,a);
    }
    case 5: /* (d16,An) */ {
        int16_t d16 = (int16_t)m68k_fetch(s);
        uint32_t a  = s->cpu.a[reg] + (int32_t)d16;
        return sz==SZ_B ? m68k_rb(s,a) : sz==SZ_W ? m68k_rw(s,a) : m68k_rl(s,a);
    }
    case 6: /* (d8,An,Xn) */ {
        uint16_t ext = m68k_fetch(s);
        int8_t   d8  = (int8_t)(ext & 0xFF);
        int      xi  = (ext >> 12) & 7, xd = (ext >> 15) & 1;
        uint32_t xv  = xd ? s->cpu.a[xi] : s->cpu.d[xi];
        if (!((ext >> 11) & 1)) xv = (uint32_t)(int16_t)(uint16_t)xv; /* word index */
        uint32_t a   = s->cpu.a[reg] + (int32_t)(int8_t)d8 + xv;
        return sz==SZ_B ? m68k_rb(s,a) : sz==SZ_W ? m68k_rw(s,a) : m68k_rl(s,a);
    }
    case 7:
        switch (reg) {
        case 0: { /* (xxx).W */
            uint32_t a = (uint32_t)(int16_t)m68k_fetch(s);
            return sz==SZ_B ? m68k_rb(s,a) : sz==SZ_W ? m68k_rw(s,a) : m68k_rl(s,a);
        }
        case 1: { /* (xxx).L */
            uint32_t a = m68k_fetch_l(s);
            return sz==SZ_B ? m68k_rb(s,a) : sz==SZ_W ? m68k_rw(s,a) : m68k_rl(s,a);
        }
        case 2: { /* (d16,PC) */
            uint32_t base = s->cpu.pc;
            int16_t  d16  = (int16_t)m68k_fetch(s);
            uint32_t a    = base + (int32_t)d16;
            return sz==SZ_B ? m68k_rb(s,a) : sz==SZ_W ? m68k_rw(s,a) : m68k_rl(s,a);
        }
        case 3: { /* (d8,PC,Xn) */
            uint32_t base = s->cpu.pc;
            uint16_t ext  = m68k_fetch(s);
            int8_t   d8   = (int8_t)(ext & 0xFF);
            int      xi   = (ext >> 12) & 7, xd = (ext >> 15) & 1;
            uint32_t xv   = xd ? s->cpu.a[xi] : s->cpu.d[xi];
            if (!((ext >> 11) & 1)) xv = (uint32_t)(int16_t)(uint16_t)xv;
            uint32_t a    = base + (int32_t)(int8_t)d8 + xv;
            return sz==SZ_B ? m68k_rb(s,a) : sz==SZ_W ? m68k_rw(s,a) : m68k_rl(s,a);
        }
        case 4: { /* #imm */
            if (sz == SZ_B) { return m68k_fetch(s) & 0xFF; }
            if (sz == SZ_W) { return m68k_fetch(s); }
            return m68k_fetch_l(s);
        }
        }
    }
    LOG_WARN("m68k: ea_read: unknown mode=%d reg=%d sz=%d pc=0x%06X", mode, reg, sz, s->cpu.pc);
    return 0;
}

/*
 * m68k_ea_write — compute effective address (with side effects for modes
 * 3/4) and write a value of the given size.  PC-relative and immediate modes
 * are not valid write targets and are treated as bugs.
 */
static void m68k_ea_write(m68k_state_t *s, int mode, int reg, int sz, uint32_t val) {
    uint32_t a;
    switch (mode) {
    case 0: /* Dn */
        m68k_dn_write(&s->cpu, reg, val, sz);
        return;
    case 1: /* An — always 32-bit, sign-extend word */
        s->cpu.a[reg] = (sz == SZ_W) ? (uint32_t)(int16_t)(uint16_t)val : val;
        return;
    case 2: a = s->cpu.a[reg]; break;
    case 3: /* (An)+ — write then increment */
        a = s->cpu.a[reg];
        s->cpu.a[reg] += (sz == SZ_B && reg == 7) ? 2 : sz;
        break;
    case 4: /* -(An) — decrement then write */
        s->cpu.a[reg] -= (sz == SZ_B && reg == 7) ? 2 : sz;
        a = s->cpu.a[reg];
        break;
    case 5: { int16_t d16 = (int16_t)m68k_fetch(s); a = s->cpu.a[reg] + (int32_t)d16; break; }
    case 6: {
        uint16_t ext = m68k_fetch(s);
        int8_t   d8  = (int8_t)(ext & 0xFF);
        int      xi  = (ext >> 12) & 7, xd = (ext >> 15) & 1;
        uint32_t xv  = xd ? s->cpu.a[xi] : s->cpu.d[xi];
        if (!((ext >> 11) & 1)) xv = (uint32_t)(int16_t)(uint16_t)xv;
        a = s->cpu.a[reg] + (int32_t)(int8_t)d8 + xv;
        break;
    }
    case 7:
        switch (reg) {
        case 0: a = (uint32_t)(int16_t)m68k_fetch(s); break;
        case 1: a = m68k_fetch_l(s); break;
        default:
            LOG_WARN("m68k: ea_write: invalid mode 7 reg=%d pc=0x%06X", reg, s->cpu.pc);
            return;
        }
        break;
    default:
        LOG_WARN("m68k: ea_write: unknown mode=%d reg=%d pc=0x%06X", mode, reg, s->cpu.pc);
        return;
    }
    if      (sz == SZ_B) m68k_wb(s, a, (uint8_t)val);
    else if (sz == SZ_W) m68k_ww(s, a, (uint16_t)val);
    else                 m68k_wl(s, a, val);
}

/*
 * m68k_rmr / m68k_rmw — read-modify-write helpers.
 *
 * m68k_rmr: decode EA consuming extension words EXACTLY ONCE, read the
 *   current value, store the decoded address in *ap (0 for register modes).
 * m68k_rmw: write the result value back to the address previously decoded
 *   by m68k_rmr (modes 0/1 use register write; memory modes use *ap).
 *
 * Using these pairs instead of ea_read + ea_write prevents the double-fetch
 * bug that arises for modes 5/6/7 where both ea_read and ea_write would each
 * call m68k_fetch to decode the same displacement/extension word.
 */
static inline uint32_t m68k_rmr(m68k_state_t *s, int mode, int reg, int isz, uint32_t *ap) {
    m68k_cpu_t *c = &s->cpu;
    uint32_t a;
    switch (mode) {
    case 0: *ap = 0; return c->d[reg] & m68k_sz_mask(isz);
    case 1: *ap = 1; return (isz==SZ_W)?(uint32_t)(int16_t)c->a[reg]:c->a[reg];
    case 2: a = c->a[reg]; break;
    case 3: a = c->a[reg]; c->a[reg] += (isz==SZ_B&&reg==7)?2:isz; break;
    case 4: c->a[reg] -= (isz==SZ_B&&reg==7)?2:isz; a = c->a[reg]; break;
    case 5: { int16_t d16=(int16_t)m68k_fetch(s); a=c->a[reg]+(int32_t)d16; break; }
    case 6: {
        uint16_t ext=m68k_fetch(s); int8_t d8=(int8_t)(ext&0xFF);
        int xi=(ext>>12)&7, xd=(ext>>15)&1;
        uint32_t xv=xd?c->a[xi]:c->d[xi];
        if (!((ext>>11)&1)) xv=(uint32_t)(int16_t)(uint16_t)xv;
        a=c->a[reg]+(int32_t)d8+xv; break;
    }
    case 7:
        switch (reg) {
        case 0: a=(uint32_t)(int16_t)m68k_fetch(s); break;
        case 1: a=m68k_fetch_l(s); break;
        case 2: { uint32_t b=c->pc; int16_t d=(int16_t)m68k_fetch(s); a=b+(int32_t)d; break; }
        case 3: { uint32_t b=c->pc; uint16_t ext=m68k_fetch(s);
                  int xi=(ext>>12)&7, xd=(ext>>15)&1;
                  uint32_t xv=xd?c->a[xi]:c->d[xi];
                  if (!((ext>>11)&1)) xv=(uint32_t)(int16_t)(uint16_t)xv;
                  a=b+(int32_t)(int8_t)(ext&0xFF)+xv; break; }
        default: *ap=0; return 0;
        }
        break;
    default: *ap=0; return 0;
    }
    *ap = a;
    return isz==SZ_B?m68k_rb(s,a):isz==SZ_W?m68k_rw(s,a):m68k_rl(s,a);
}
static inline void m68k_rmw(m68k_state_t *s, int mode, int reg, int isz, uint32_t a, uint32_t val) {
    m68k_cpu_t *c = &s->cpu;
    if (mode == 0) { m68k_dn_write(c, reg, val, isz); return; }
    if (mode == 1) { c->a[reg]=(isz==SZ_W)?(uint32_t)(int16_t)(uint16_t)val:val; return; }
    if (isz==SZ_B) m68k_wb(s,a,(uint8_t)val);
    else if (isz==SZ_W) m68k_ww(s,a,(uint16_t)val);
    else m68k_wl(s,a,val);
}
/* Write-only RMW (CLR, Scc) where no prior read was needed. */
static inline void m68k_rmw_w(m68k_state_t *s, int mode, int reg, int isz, uint32_t val) {
    if (mode < 2) { m68k_ea_write(s, mode, reg, isz, val); return; }
    uint32_t a;
    m68k_rmr(s, mode, reg, isz, &a); /* consume extension words, discard value */
    m68k_rmw(s, mode, reg, isz, a, val);
}

/*
 * m68k_ea_addr — compute the effective address without performing a memory
 * access (used by LEA, PEA, JSR/JMP, MOVEM to/from memory).
 * Post-increment (mode 3) and immediate (mode 7.4) are not valid here.
 */
static uint32_t m68k_ea_addr(m68k_state_t *s, int mode, int reg) {
    switch (mode) {
    case 2: return s->cpu.a[reg];
    case 3: return s->cpu.a[reg]; /* (An)+ — caller handles post-increment */
    case 4: return s->cpu.a[reg]; /* -(An) — caller handles pre-decrement  */
    case 5: { int16_t d16 = (int16_t)m68k_fetch(s); return s->cpu.a[reg] + (int32_t)d16; }
    case 6: {
        uint16_t ext = m68k_fetch(s);
        int8_t   d8  = (int8_t)(ext & 0xFF);
        int      xi  = (ext >> 12) & 7, xd = (ext >> 15) & 1;
        uint32_t xv  = xd ? s->cpu.a[xi] : s->cpu.d[xi];
        if (!((ext >> 11) & 1)) xv = (uint32_t)(int16_t)(uint16_t)xv;
        return s->cpu.a[reg] + (int32_t)(int8_t)d8 + xv;
    }
    case 7:
        switch (reg) {
        case 0: return (uint32_t)(int16_t)m68k_fetch(s);
        case 1: return m68k_fetch_l(s);
        case 2: { uint32_t b = s->cpu.pc; int16_t d16 = (int16_t)m68k_fetch(s); return b + (int32_t)d16; }
        case 3: {
            uint32_t b    = s->cpu.pc;
            uint16_t ext  = m68k_fetch(s);
            int8_t   d8   = (int8_t)(ext & 0xFF);
            int      xi   = (ext >> 12) & 7, xd = (ext >> 15) & 1;
            uint32_t xv   = xd ? s->cpu.a[xi] : s->cpu.d[xi];
            if (!((ext >> 11) & 1)) xv = (uint32_t)(int16_t)(uint16_t)xv;
            return b + (int32_t)(int8_t)d8 + xv;
        }
        }
    }
    LOG_WARN("m68k: ea_addr: unknown mode=%d reg=%d pc=0x%06X", mode, reg, s->cpu.pc);
    return 0;
}

/* ================================================================ shift/rotate helpers */

static inline uint32_t m68k_do_lsl(m68k_cpu_t *c, uint32_t v, int cnt, int sz) {
    uint32_t mask = m68k_sz_mask(sz), msb = m68k_sz_msb(sz);
    v &= mask;
    int last_out = 0;
    if (cnt > 0) {
        last_out = (cnt <= sz*8) ? (int)((v >> (sz*8 - cnt)) & 1) : 0;
        v = (cnt >= sz*8) ? 0 : (v << cnt) & mask;
    }
    c->sr &= ~(M68K_SR_N | M68K_SR_Z | M68K_SR_V | M68K_SR_C | M68K_SR_X);
    if (v == 0)  c->sr |= M68K_SR_Z;
    if (v & msb) c->sr |= M68K_SR_N;
    if (cnt > 0 && last_out) c->sr |= M68K_SR_C | M68K_SR_X;
    return v;
}

static inline uint32_t m68k_do_lsr(m68k_cpu_t *c, uint32_t v, int cnt, int sz) {
    uint32_t mask = m68k_sz_mask(sz), msb = m68k_sz_msb(sz);
    v &= mask;
    int last_out = 0;
    if (cnt > 0) {
        last_out = (cnt <= sz*8) ? (int)((v >> (cnt-1)) & 1) : 0;
        v = (cnt >= sz*8) ? 0 : (v >> cnt);
    }
    c->sr &= ~(M68K_SR_N | M68K_SR_Z | M68K_SR_V | M68K_SR_C | M68K_SR_X);
    if (v == 0)  c->sr |= M68K_SR_Z;
    if (v & msb) c->sr |= M68K_SR_N;
    if (cnt > 0 && last_out) c->sr |= M68K_SR_C | M68K_SR_X;
    return v;
}

static inline uint32_t m68k_do_asl(m68k_cpu_t *c, uint32_t v, int cnt, int sz) {
    uint32_t mask = m68k_sz_mask(sz), msb = m68k_sz_msb(sz);
    v &= mask;
    int overflow = 0, last_out = 0;
    for (int i = 0; i < cnt; i++) {
        last_out = (v & msb) != 0;
        v = (v << 1) & mask;
        if (((v & msb) != 0) != last_out) overflow = 1;
    }
    c->sr &= ~(M68K_SR_N | M68K_SR_Z | M68K_SR_V | M68K_SR_C | M68K_SR_X);
    if (v == 0)       c->sr |= M68K_SR_Z;
    if (v & msb)      c->sr |= M68K_SR_N;
    if (overflow)     c->sr |= M68K_SR_V;
    if (cnt > 0 && last_out) c->sr |= M68K_SR_C | M68K_SR_X;
    return v;
}

static inline uint32_t m68k_do_asr(m68k_cpu_t *c, uint32_t v, int cnt, int sz) {
    uint32_t mask = m68k_sz_mask(sz), msb = m68k_sz_msb(sz);
    v &= mask;
    int sign = (v & msb) != 0, last_out = 0;
    if (cnt > 0) {
        last_out = (cnt <= sz*8) ? (int)((v >> (cnt-1)) & 1) : sign;
        if (cnt >= sz*8) v = sign ? mask : 0;
        else { v >>= cnt; if (sign) v |= mask << (sz*8 - cnt); v &= mask; }
    }
    c->sr &= ~(M68K_SR_N | M68K_SR_Z | M68K_SR_V | M68K_SR_C | M68K_SR_X);
    if (v == 0)  c->sr |= M68K_SR_Z;
    if (v & msb) c->sr |= M68K_SR_N;
    /* V is always 0 for ASR */
    if (cnt > 0 && last_out) c->sr |= M68K_SR_C | M68K_SR_X;
    return v;
}

static inline uint32_t m68k_do_rol(m68k_cpu_t *c, uint32_t v, int cnt, int sz) {
    uint32_t mask = m68k_sz_mask(sz), msb = m68k_sz_msb(sz);
    v &= mask;
    int bits = sz * 8;
    cnt &= (bits - 1);
    if (cnt) v = ((v << cnt) | (v >> (bits - cnt))) & mask;
    int last_bit = (v & 1) != 0; /* last bit rotated into bit0 is the MSB we just shifted out */
    /* C = last bit rotated out (bit that entered from the top is v&1 when cnt>0... actually C = MSB of result if cnt>0) */
    /* For ROL: C = bit rotated out = original bit at position (bits-cnt) */
    /* Simpler: C = bit 0 of result after rotation (which was the last bit shifted out of MSB) */
    /* Actually C = the last bit rotated out of MSB = (original >> (bits-1)) if cnt==1 */
    /* Standard: C = last bit shifted into C = the bit shifted out of MSB */
    /* After rotation: the bit that just came from MSB is now at bit 0 */
    last_bit = (v & 1) != 0;
    c->sr &= ~(M68K_SR_N | M68K_SR_Z | M68K_SR_V | M68K_SR_C);
    if (v == 0)   c->sr |= M68K_SR_Z;
    if (v & msb)  c->sr |= M68K_SR_N;
    if (last_bit) c->sr |= M68K_SR_C;
    return v;
}

static inline uint32_t m68k_do_ror(m68k_cpu_t *c, uint32_t v, int cnt, int sz) {
    uint32_t mask = m68k_sz_mask(sz), msb = m68k_sz_msb(sz);
    v &= mask;
    int bits = sz * 8;
    cnt &= (bits - 1);
    if (cnt) v = ((v >> cnt) | (v << (bits - cnt))) & mask;
    int last_bit = (v & msb) != 0;
    c->sr &= ~(M68K_SR_N | M68K_SR_Z | M68K_SR_V | M68K_SR_C);
    if (v == 0)   c->sr |= M68K_SR_Z;
    if (v & msb)  c->sr |= M68K_SR_N;
    if (last_bit) c->sr |= M68K_SR_C;
    return v;
}

static inline uint32_t m68k_do_roxl(m68k_cpu_t *c, uint32_t v, int cnt, int sz) {
    uint32_t mask = m68k_sz_mask(sz), msb = m68k_sz_msb(sz);
    v &= mask;
    int bits = sz * 8;
    int x = (c->sr & M68K_SR_X) != 0;
    int last_out = x;
    for (int i = 0; i < cnt; i++) {
        last_out = (v & msb) != 0;
        v = ((v << 1) | x) & mask;
        x = last_out;
    }
    c->sr &= ~(M68K_SR_N | M68K_SR_Z | M68K_SR_V | M68K_SR_C | M68K_SR_X);
    if (v == 0)    c->sr |= M68K_SR_Z;
    if (v & msb)   c->sr |= M68K_SR_N;
    if (cnt > 0 && last_out) c->sr |= M68K_SR_C | M68K_SR_X;
    (void)bits;
    return v;
}

static inline uint32_t m68k_do_roxr(m68k_cpu_t *c, uint32_t v, int cnt, int sz) {
    uint32_t mask = m68k_sz_mask(sz), msb = m68k_sz_msb(sz);
    v &= mask;
    int bits = sz * 8;
    int x = (c->sr & M68K_SR_X) != 0;
    int last_out = x;
    for (int i = 0; i < cnt; i++) {
        last_out = v & 1;
        v = ((v >> 1) | ((uint32_t)x << (bits - 1))) & mask;
        x = last_out;
    }
    c->sr &= ~(M68K_SR_N | M68K_SR_Z | M68K_SR_V | M68K_SR_C | M68K_SR_X);
    if (v == 0)    c->sr |= M68K_SR_Z;
    if (v & msb)   c->sr |= M68K_SR_N;
    if (cnt > 0 && last_out) c->sr |= M68K_SR_C | M68K_SR_X;
    (void)bits;
    return v;
}

/* ================================================================ startup */

/*
 * Load SSP and PC from the reset vectors (big-endian longs at 0 and 4).
 * Call once after the ROM is mapped.
 */
static inline void m68k_startup(m68k_state_t *s) {
    s->cpu.ssp  = m68k_rl(s, 0x000000);
    s->cpu.a[7] = s->cpu.ssp;
    s->cpu.pc   = m68k_rl(s, 0x000004);
    LOG_INFO("m68k: startup SSP=0x%08X PC=0x%08X", s->cpu.ssp, s->cpu.pc);
}

/* ================================================================ main step */

/*
 * Execute one instruction.  Returns approximate cycle count (not cycle-
 * accurate; useful only for rough timing).  Returns 0 if halted.
 */
static inline int m68k_step(m68k_state_t *s) {
    if (s->cpu.halted)  return 0;
    if (s->cpu.stopped) return 4;

    m68k_cpu_t *c = &s->cpu;
    uint32_t    op_pc = c->pc;
    uint16_t    op    = m68k_fetch(s);
    int         grp   = (op >> 12) & 0xF;

    c->cycles += 4;

    switch (grp) {

    /* ---- groups 1/2/3: MOVE.B / MOVE.L / MOVE.W ---- */
    case 0x1: case 0x2: case 0x3: {
        int sz      = (grp == 1) ? SZ_B : (grp == 2) ? SZ_L : SZ_W;
        int src_m   = (op >> 3) & 7;
        int src_r   = (op >> 0) & 7;
        int dst_r   = (op >> 9) & 7;
        int dst_m   = (op >> 6) & 7;
        uint32_t v  = m68k_ea_read(s, src_m, src_r, sz);
        /* MOVEA: destination address register, no flags */
        if (dst_m == 1) {
            c->a[dst_r] = (sz == SZ_W) ? (uint32_t)(int16_t)(uint16_t)v : v;
        } else {
            m68k_ea_write(s, dst_m, dst_r, sz, v);
            m68k_flags_logic(c, v, sz);
        }
        break;
    }

    /* ---- group 0: bit manipulation + immediate ops ---- */
    case 0x0: {
        /* Distinguish immediate bit ops (bit 8 set → static/imm form) and
         * register-based bit ops (bits 8-6 = 1xx, bit 11-8 encode Dn). */
        int bit8 = (op >> 8) & 1;

        /* BTST/BCHG/BCLR/BSET register form: 0000 Dn 1 op mode reg */
        if ((op & 0x0138) == 0x0100) { /* bit 8=1, bits 6-3 = 00 (mode bits) - actually need better decode */
            /* Decode: 0000 Dn 1 op ea where bits [8:6] = 1 + 2-bit op */
        }

        /* Decode based on bits [11:8] for immediate ops */
        int sub = (op >> 8) & 0xF;
        int mode = (op >> 3) & 7, reg = op & 7;

        if (sub == 0x0) { /* ORI */
            int sz = (op>>6)&3;
            if (sz == 3) { /* ORI to SR */
                if (!m68k_is_supervisor(c)) { LOG_WARN("m68k: ORI SR privilege violation pc=0x%06X", op_pc); break; }
                uint16_t imm = m68k_fetch(s); c->sr |= imm; break;
            }
            int isz = (sz==0)?SZ_B:(sz==1)?SZ_W:SZ_L;
            uint32_t imm = m68k_ea_read(s, 7, 4, isz);
            if (sz == 0 && mode == 7 && reg == 4) { /* ORI to CCR */
                c->sr |= imm & M68K_SR_CCR; break;
            }
            uint32_t ea; uint32_t dst = m68k_rmr(s, mode, reg, isz, &ea);
            uint32_t r   = dst | imm;
            m68k_rmw(s, mode, reg, isz, ea, r);
            m68k_flags_logic(c, r, isz);
            break;
        }
        if (sub == 0x2) { /* ANDI */
            int sz = (op>>6)&3;
            if (sz == 3) {
                if (!m68k_is_supervisor(c)) { LOG_WARN("m68k: ANDI SR privilege violation pc=0x%06X", op_pc); break; }
                uint16_t imm = m68k_fetch(s);
                uint16_t old_sr = c->sr;
                c->sr &= imm;
                if ((old_sr & M68K_SR_S) && !(c->sr & M68K_SR_S)) m68k_leave_supervisor(c);
                break;
            }
            int isz = (sz==0)?SZ_B:(sz==1)?SZ_W:SZ_L;
            uint32_t imm = m68k_ea_read(s, 7, 4, isz);
            if (sz == 0 && mode == 7 && reg == 4) { c->sr &= ~(~imm & M68K_SR_CCR); break; }
            uint32_t ea; uint32_t dst = m68k_rmr(s, mode, reg, isz, &ea);
            uint32_t r   = dst & imm;
            m68k_rmw(s, mode, reg, isz, ea, r);
            m68k_flags_logic(c, r, isz);
            break;
        }
        if (sub == 0x4) { /* SUBI */
            int sz = (op>>6)&3; if (sz==3) goto unknown_op;
            int isz = (sz==0)?SZ_B:(sz==1)?SZ_W:SZ_L;
            uint32_t imm = m68k_ea_read(s, 7, 4, isz);
            uint32_t ea; uint32_t dst = m68k_rmr(s, mode, reg, isz, &ea);
            uint32_t r   = (dst - imm) & m68k_sz_mask(isz);
            m68k_rmw(s, mode, reg, isz, ea, r);
            m68k_flags_sub(c, dst, imm, isz);
            break;
        }
        if (sub == 0x6) { /* ADDI */
            int sz = (op>>6)&3; if (sz==3) goto unknown_op;
            int isz = (sz==0)?SZ_B:(sz==1)?SZ_W:SZ_L;
            uint32_t imm = m68k_ea_read(s, 7, 4, isz);
            uint32_t ea; uint32_t dst = m68k_rmr(s, mode, reg, isz, &ea);
            uint32_t r   = (dst + imm) & m68k_sz_mask(isz);
            m68k_rmw(s, mode, reg, isz, ea, r);
            m68k_flags_add(c, dst, imm, isz);
            break;
        }
        if (sub == 0xA) { /* EORI */
            int sz = (op>>6)&3;
            if (sz == 3) {
                if (!m68k_is_supervisor(c)) { LOG_WARN("m68k: EORI SR privilege pc=0x%06X", op_pc); break; }
                uint16_t imm = m68k_fetch(s); c->sr ^= imm; break;
            }
            int isz = (sz==0)?SZ_B:(sz==1)?SZ_W:SZ_L;
            uint32_t imm = m68k_ea_read(s, 7, 4, isz);
            if (sz == 0 && mode == 7 && reg == 4) { c->sr ^= imm & M68K_SR_CCR; break; }
            uint32_t ea; uint32_t dst = m68k_rmr(s, mode, reg, isz, &ea);
            uint32_t r   = dst ^ imm;
            m68k_rmw(s, mode, reg, isz, ea, r);
            m68k_flags_logic(c, r, isz);
            break;
        }
        if (sub == 0xC) { /* CMPI */
            int sz = (op>>6)&3; if (sz==3) goto unknown_op;
            int isz = (sz==0)?SZ_B:(sz==1)?SZ_W:SZ_L;
            uint32_t imm = m68k_ea_read(s, 7, 4, isz);
            uint32_t dst = m68k_ea_read(s, mode, reg, isz);
            m68k_flags_cmp(c, dst, imm, isz);
            break;
        }
        /* Bit operations (immediate and register forms) */
        /* 0000 1000 00 ea: BTST #imm    (bit 11-8 = 0x8, bits 7-6 = 00) */
        /* 0000 Dn 1 op ea: BTST/BCHG/BCLR/BSET register form            */
        {
            int bit_op, bit_num_from_reg, dn;
            if ((op & 0x0100) && !(op & 0x0800)) {
                /* Register form: 0000 Dn 1 xx ea */
                dn              = (op >> 9) & 7;
                bit_op          = (op >> 6) & 3;
                bit_num_from_reg = 1;
            } else if ((op & 0x0F00) == 0x0800) {
                /* Immediate form: 0000 1000 xx ea — BTST/BCHG/BCLR/BSET #imm */
                dn              = 0; /* unused */
                bit_op          = (op >> 6) & 3;
                bit_num_from_reg = 0;
            } else {
                goto unknown_op;
            }
            int bit_num;
            if (bit_num_from_reg) {
                bit_num = c->d[dn] & (mode == 0 ? 31 : 7);
            } else {
                bit_num = m68k_fetch(s) & (mode == 0 ? 31 : 7);
            }
            uint32_t mask2;
            uint32_t v;
            if (mode == 0) {
                mask2 = 1u << bit_num;
                v     = c->d[reg];
                c->sr = (v & mask2) ? (c->sr & ~M68K_SR_Z) : (c->sr | M68K_SR_Z);
                if (bit_op == 1) c->d[reg] ^=  mask2;
                if (bit_op == 2) c->d[reg] &= ~mask2;
                if (bit_op == 3) c->d[reg] |=  mask2;
            } else {
                mask2 = 1u << (bit_num & 7);
                uint32_t a = m68k_ea_addr(s, mode, reg);
                uint8_t  bv = m68k_rb(s, a);
                c->sr = (bv & mask2) ? (c->sr & ~M68K_SR_Z) : (c->sr | M68K_SR_Z);
                if (bit_op == 1) m68k_wb(s, a, bv ^ (uint8_t)mask2);
                if (bit_op == 2) m68k_wb(s, a, bv & (uint8_t)~mask2);
                if (bit_op == 3) m68k_wb(s, a, bv | (uint8_t)mask2);
            }
            break;
        }
        break;
    }

    /* ---- group 4: misc ---- */
    case 0x4: {
        int mode = (op >> 3) & 7, reg = op & 7;
        int sub6 = (op >> 6) & 0x3F;

        /* 0100 1110 0111 0001: NOP */
        if (op == 0x4E71) break;

        /* 0100 1110 0111 0000: RESET */
        if (op == 0x4E70) {
            LOG_INFO("m68k: RESET at 0x%06X", op_pc);
            break;
        }

        /* 0100 1110 0111 0011: RTE */
        if (op == 0x4E73) {
            if (!m68k_is_supervisor(c)) { LOG_WARN("m68k: RTE privilege violation"); break; }
            c->sr = m68k_pop_w(s);
            c->pc = m68k_pop_l(s);
            if (!(c->sr & M68K_SR_S)) m68k_leave_supervisor(c);
            break;
        }

        /* 0100 1110 0111 0101: RTS */
        if (op == 0x4E75) { c->pc = m68k_pop_l(s); break; }

        /* 0100 1110 0111 0111: RTR */
        if (op == 0x4E77) {
            c->sr = (c->sr & ~M68K_SR_CCR) | (m68k_pop_w(s) & M68K_SR_CCR);
            c->pc = m68k_pop_l(s);
            break;
        }

        /* 0100 1110 0111 0100: STOP #imm */
        if (op == 0x4E74) {
            if (!m68k_is_supervisor(c)) { LOG_WARN("m68k: STOP privilege violation"); break; }
            uint16_t imm = m68k_fetch(s);
            c->sr = imm;
            if (!(c->sr & M68K_SR_S)) m68k_leave_supervisor(c);
            c->stopped = 1;
            break;
        }

        /* 0100 1110 0111 0010: ILLEGAL */
        if (op == 0x4E72 || op == 0x4AFC) {
            LOG_WARN("m68k: ILLEGAL at 0x%06X", op_pc);
            c->halted = 1;
            break;
        }

        /* 0100 1110 0111 0110: TRAPV */
        if (op == 0x4E76) {
            if (c->sr & M68K_SR_V) { LOG_WARN("m68k: TRAPV taken at 0x%06X", op_pc); }
            break;
        }

        /* 0100 1110 01 vector: TRAP #n */
        if ((op & 0xFFF0) == 0x4E40) {
            LOG_WARN("m68k: TRAP #%d at 0x%06X", op & 0xF, op_pc);
            break;
        }

        /* 0100 1110 010 reg: LINK An, #d16 */
        if ((op & 0xFFF8) == 0x4E50) {
            int16_t d16 = (int16_t)m68k_fetch(s);
            m68k_push_l(s, c->a[reg]);
            c->a[reg] = c->a[7];
            c->a[7]  += (int32_t)d16;
            break;
        }

        /* 0100 1110 011 reg: UNLK An */
        if ((op & 0xFFF8) == 0x4E58) {
            c->a[7] = c->a[reg];
            c->a[reg] = m68k_pop_l(s);
            break;
        }

        /* 0100 1110 100 reg: MOVE USP → An  (0=from An to USP, 1=from USP to An) */
        if ((op & 0xFFF0) == 0x4E60) {
            if (!m68k_is_supervisor(c)) { LOG_WARN("m68k: MOVE USP privilege"); break; }
            if (op & 8) c->a[reg] = c->usp;
            else        c->usp    = c->a[reg];
            break;
        }

        /* 0100 1110 11 ea: JMP (0x4EC0) / 0100 1110 10 ea: JSR (0x4E80).
         * NOTE: mask must be 0xFFC0 — bit 6 distinguishes JMP from JSR. Using
         * 0xFF80 makes the JMP test dead (0x4EC0 & 0xFF80 == 0x4E80) so every
         * JMP was wrongly executed as JSR, pushing a return address and leaking
         * the stack (turned jump-table dispatch into a runaway rts-chain). */
        if ((op & 0xFFC0) == 0x4EC0) { /* JMP */
            c->pc = m68k_ea_addr(s, mode, reg);
            break;
        }
        if ((op & 0xFFC0) == 0x4E80) { /* JSR */
            uint32_t dest = m68k_ea_addr(s, mode, reg);
            m68k_push_l(s, c->pc);
            c->pc = dest;
            break;
        }

        /* 0100 0000 11 ea: MOVE from SR */
        if ((op & 0xFFC0) == 0x40C0) {
            if (!m68k_is_supervisor(c)) { LOG_WARN("m68k: MOVE from SR privilege pc=0x%06X", op_pc); break; }
            m68k_ea_write(s, mode, reg, SZ_W, c->sr);
            break;
        }

        /* 0100 0010 11 ea: MOVE from CCR */
        if ((op & 0xFFC0) == 0x42C0) {
            m68k_ea_write(s, mode, reg, SZ_W, c->sr & M68K_SR_CCR);
            break;
        }

        /* 0100 0100 11 ea: MOVE to CCR */
        if ((op & 0xFFC0) == 0x44C0) {
            uint32_t v = m68k_ea_read(s, mode, reg, SZ_W);
            c->sr = (c->sr & ~M68K_SR_CCR) | (v & M68K_SR_CCR);
            break;
        }

        /* 0100 0110 11 ea: MOVE to SR */
        if ((op & 0xFFC0) == 0x46C0) {
            if (!m68k_is_supervisor(c)) { LOG_WARN("m68k: MOVE to SR privilege pc=0x%06X", op_pc); break; }
            uint16_t v = (uint16_t)m68k_ea_read(s, mode, reg, SZ_W);
            uint16_t old = c->sr;
            c->sr = v;
            if ((old & M68K_SR_S) && !(v & M68K_SR_S)) m68k_leave_supervisor(c);
            break;
        }

        /* 0100 1000 01 ea: PEA — control addressing modes only (mode >= 2).
         * Mode 0 in this range is SWAP (0x4840-0x4847) and mode 1 is illegal,
         * so the mask must exclude them or SWAP gets executed as PEA (pushes). */
        if ((op & 0xFFC0) == 0x4840 && mode >= 2) {
            m68k_push_l(s, m68k_ea_addr(s, mode, reg));
            break;
        }

        /* 0100 1000 01 sz Dn: EXT */
        if ((op & 0xFEB8) == 0x4880) {
            int long_ext = (op >> 6) & 1; /* 0=word-extend, 1=long-extend */
            if (!long_ext) {
                c->d[reg] = (c->d[reg] & 0xFFFF0000u) | (uint32_t)(uint16_t)(int16_t)(int8_t)(uint8_t)c->d[reg];
                m68k_flags_logic(c, c->d[reg], SZ_W);
            } else {
                c->d[reg] = (uint32_t)(int32_t)(int16_t)(uint16_t)c->d[reg];
                m68k_flags_logic(c, c->d[reg], SZ_L);
            }
            break;
        }

        /* 0100 1000 0100 0 reg: SWAP Dn */
        if ((op & 0xFFF8) == 0x4840) {
            uint32_t v = c->d[reg];
            c->d[reg] = (v >> 16) | (v << 16);
            m68k_flags_logic(c, c->d[reg], SZ_L);
            break;
        }

        /* 0100 1100 1 ea: MOVEM memory→regs */
        /* 0100 1000 1 ea: MOVEM regs→memory  (already handled PEA above; check again) */
        if ((op & 0xFB80) == 0x4880 && (op & 0x0040)) {
            /* MOVEM: bit 10 = direction (0=regs→mem, 1=mem→regs) */
            int to_regs = (op >> 10) & 1;
            int sz_long = (op >> 6) & 1; /* 0=word, 1=long */
            int isz     = sz_long ? SZ_L : SZ_W;
            uint16_t reglist = m68k_fetch(s);
            if (!to_regs) {
                /* registers → memory */
                if (mode == 4) {
                    /* pre-decrement: 68K uses the REVERSED register mask —
                       bit 0 = A7, … bit 7 = A0, bit 8 = D7, … bit 15 = D0.
                       Iterate bits ascending so the lowest-numbered register
                       (D0) lands at the lowest address, matching (An)+ loads. */
                    for (int i = 0; i < 16; i++) {
                        if (reglist & (1u << i)) {
                            uint32_t rv = (i < 8) ? c->a[7-i] : c->d[15-i];
                            if (isz == SZ_W) { s->cpu.a[reg] -= 2; m68k_ww(s, s->cpu.a[reg], (uint16_t)rv); }
                            else             { s->cpu.a[reg] -= 4; m68k_wl(s, s->cpu.a[reg], rv); }
                        }
                    }
                } else {
                    uint32_t a = m68k_ea_addr(s, mode, reg);
                    for (int i = 0; i < 16; i++) {
                        if (reglist & (1u << i)) {
                            uint32_t rv = (i < 8) ? c->d[i] : c->a[i-8];
                            if (isz == SZ_W) { m68k_ww(s, a, (uint16_t)rv); a += 2; }
                            else             { m68k_wl(s, a, rv);             a += 4; }
                        }
                    }
                }
            } else {
                /* memory → registers */
                uint32_t a = m68k_ea_addr(s, mode, reg);
                for (int i = 0; i < 16; i++) {
                    if (reglist & (1u << i)) {
                        if (i < 8) {
                            c->d[i] = (isz == SZ_W) ? (uint32_t)(int16_t)m68k_rw(s, a) : m68k_rl(s, a);
                        } else {
                            c->a[i-8] = (isz == SZ_W) ? (uint32_t)(int16_t)m68k_rw(s, a) : m68k_rl(s, a);
                        }
                        a += isz;
                    }
                }
                /* post-increment updates An if mode was 3 */
                if (mode == 3) s->cpu.a[reg] = a;
            }
            break;
        }

        /* NEG / NEGX / CLR / NOT / TST : decoded by bits [11:6] */
        {
            int sz_bits = (op >> 6) & 3; if (sz_bits == 3) goto grp4_misc;
            int isz     = (sz_bits==0)?SZ_B:(sz_bits==1)?SZ_W:SZ_L;
            int hi2     = (op >> 10) & 3; /* bits 11-10 */
            int lo2     = (op >>  8) & 3; /* bits  9-8  */

            if (hi2 == 0 && lo2 == 0) { /* NEGX */
                uint32_t ea; uint32_t src = m68k_rmr(s, mode, reg, isz, &ea);
                c->sr |= M68K_SR_Z; /* Z is only cleared, never set by NEGX */
                uint32_t r   = m68k_do_negx(c, src, isz);
                m68k_rmw(s, mode, reg, isz, ea, r);
                break;
            }
            if (hi2 == 0 && lo2 == 2) { /* CLR */
                m68k_rmw_w(s, mode, reg, isz, 0);
                c->sr &= ~(M68K_SR_N | M68K_SR_V | M68K_SR_C);
                c->sr |= M68K_SR_Z;
                break;
            }
            if (hi2 == 1 && lo2 == 0) { /* NEG */
                uint32_t ea; uint32_t src = m68k_rmr(s, mode, reg, isz, &ea);
                uint64_t u   = (uint64_t)0 - (uint64_t)(src & m68k_sz_mask(isz));
                uint32_t r   = (uint32_t)u & m68k_sz_mask(isz);
                m68k_rmw(s, mode, reg, isz, ea, r);
                m68k_flags_sub(c, 0, src, isz);
                /* Special NEG carry: C is set if src != 0 */
                if (src & m68k_sz_mask(isz)) c->sr |= M68K_SR_C | M68K_SR_X;
                else                         c->sr &= ~(M68K_SR_C | M68K_SR_X);
                break;
            }
            if (hi2 == 1 && lo2 == 2) { /* NOT */
                uint32_t ea; uint32_t src = m68k_rmr(s, mode, reg, isz, &ea);
                uint32_t r   = (~src) & m68k_sz_mask(isz);
                m68k_rmw(s, mode, reg, isz, ea, r);
                m68k_flags_logic(c, r, isz);
                break;
            }
            if (hi2 == 2 && lo2 == 2) { /* TST */
                uint32_t src = m68k_ea_read(s, mode, reg, isz);
                m68k_flags_logic(c, src, isz);
                break;
            }
        }
        grp4_misc:
        /* LEA: 0100 An 111 ea */
        if ((op & 0xF1C0) == 0x41C0) {
            c->a[(op>>9)&7] = m68k_ea_addr(s, (op>>3)&7, op&7);
            break;
        }
        /* PEA: 0100 1000 01 ea — control modes only (mode>=2; mode 0 = SWAP) */
        if ((op & 0xFFC0) == 0x4840 && ((op>>3)&7) >= 2) {
            m68k_push_l(s, m68k_ea_addr(s, (op>>3)&7, op&7));
            break;
        }
        /* CHK, TAS, ABCD, NBCD — stub */
        LOG_WARN("m68k: unimplemented group4 op=0x%04X pc=0x%06X", op, op_pc);
        break;
    }

    /* ---- group 5: ADDQ / SUBQ / Scc / DBcc ---- */
    case 0x5: {
        int mode = (op >> 3) & 7, reg = op & 7;
        int sz_bits = (op >> 6) & 3;
        int data8   = (op >> 9) & 7; if (!data8) data8 = 8;
        int subtype = (op >> 8) & 1; /* 0=ADDQ, 1=SUBQ; overridden for Scc/DBcc */

        if (sz_bits == 3) {
            /* Scc / DBcc */
            int cc = (op >> 8) & 0xF;
            if (mode == 1) {
                /* DBcc: 0101 cc 11 001 Dn */
                int16_t d16 = (int16_t)m68k_fetch(s);
                if (!m68k_test_cc(c, cc)) {
                    uint16_t cnt = (uint16_t)(c->d[reg] & 0xFFFF) - 1;
                    c->d[reg] = (c->d[reg] & 0xFFFF0000u) | cnt;
                    if (cnt != 0xFFFF) { c->pc = op_pc + 2 + (int32_t)d16; }
                }
                break;
            } else {
                /* Scc: set byte if condition true */
                uint8_t val2 = m68k_test_cc(c, cc) ? 0xFF : 0x00;
                m68k_rmw_w(s, mode, reg, SZ_B, val2);
                break;
            }
        }
        /* ADDQ / SUBQ */
        int isz = (sz_bits==0)?SZ_B:(sz_bits==1)?SZ_W:SZ_L;
        if (mode == 1) {
            /* ADDQ/SUBQ to An: always 32-bit, no flags */
            if (subtype) c->a[reg] -= (uint32_t)data8;
            else         c->a[reg] += (uint32_t)data8;
        } else {
            uint32_t ea; uint32_t dst = m68k_rmr(s, mode, reg, isz, &ea);
            uint32_t r;
            if (subtype) { r = (dst - (uint32_t)data8) & m68k_sz_mask(isz); m68k_flags_sub(c, dst, (uint32_t)data8, isz); }
            else         { r = (dst + (uint32_t)data8) & m68k_sz_mask(isz); m68k_flags_add(c, dst, (uint32_t)data8, isz); }
            m68k_rmw(s, mode, reg, isz, ea, r);
        }
        break;
    }

    /* ---- group 6: BCC / BRA / BSR ---- */
    case 0x6: {
        int cc   = (op >> 8) & 0xF;
        int disp = (int8_t)(op & 0xFF);
        uint32_t target;
        if (disp == 0) {
            /* 16-bit displacement follows */
            int16_t d16 = (int16_t)m68k_fetch(s);
            target = op_pc + 2 + (int32_t)d16;
        } else if (disp == -1) {
            /* 32-bit displacement (68020+, treat as 16-bit for 68000 safety) */
            int32_t d32 = (int32_t)m68k_fetch_l(s);
            target = op_pc + 2 + d32;
        } else {
            target = op_pc + 2 + disp;
        }
        if (cc == 1) {
            /* BSR */
            m68k_push_l(s, c->pc);
            c->pc = target;
        } else if (cc == 0 || m68k_test_cc(c, cc)) {
            /* BRA or BCC taken */
            c->pc = target;
        }
        break;
    }

    /* ---- group 7: MOVEQ ---- */
    case 0x7: {
        if (op & 0x0100) goto unknown_op;
        int dn = (op >> 9) & 7;
        c->d[dn] = (uint32_t)(int32_t)(int8_t)(uint8_t)(op & 0xFF);
        m68k_flags_logic(c, c->d[dn], SZ_L);
        break;
    }

    /* ---- group 8: OR / DIVU / DIVS / SBCD ---- */
    case 0x8: {
        int dn   = (op >> 9) & 7;
        int dir  = (op >> 8) & 1;   /* 0 = Dn←Dn|EA, 1 = EA←EA|Dn */
        int mode = (op >> 3) & 7, reg = op & 7;
        int sz_bits = (op >> 6) & 3;

        if (sz_bits == 3) {
            /* DIVU.W: 1000 Dn 011 ea */
            if (!dir) {
                uint16_t divisor = (uint16_t)m68k_ea_read(s, mode, reg, SZ_W);
                if (!divisor) { LOG_WARN("m68k: DIVU by zero pc=0x%06X", op_pc); break; }
                uint32_t q = c->d[dn] / divisor, rem = c->d[dn] % divisor;
                if (q > 0xFFFF) {
                    c->sr |= M68K_SR_V | M68K_SR_N;
                    c->sr &= ~(M68K_SR_Z | M68K_SR_C);
                } else {
                    c->d[dn] = ((rem & 0xFFFF) << 16) | (q & 0xFFFF);
                    m68k_flags_logic(c, q, SZ_W);
                }
            } else {
                /* DIVS.W: 1000 Dn 111 ea */
                int16_t divisor = (int16_t)m68k_ea_read(s, mode, reg, SZ_W);
                if (!divisor) { LOG_WARN("m68k: DIVS by zero pc=0x%06X", op_pc); break; }
                int32_t q = (int32_t)c->d[dn] / divisor;
                int32_t rem = (int32_t)c->d[dn] % divisor;
                if (q > 32767 || q < -32768) {
                    c->sr |= M68K_SR_V | M68K_SR_N;
                    c->sr &= ~(M68K_SR_Z | M68K_SR_C);
                } else {
                    c->d[dn] = (((uint32_t)rem & 0xFFFF) << 16) | ((uint32_t)q & 0xFFFF);
                    m68k_flags_logic(c, (uint32_t)q, SZ_W);
                }
            }
            break;
        }

        int isz = (sz_bits==0)?SZ_B:(sz_bits==1)?SZ_W:SZ_L;
        uint32_t src, dst, r;
        if (!dir) { src = m68k_ea_read(s, mode, reg, isz); r = (c->d[dn] | src) & m68k_sz_mask(isz); m68k_dn_write(c, dn, r, isz); }
        else      { uint32_t ea8; dst = m68k_rmr(s, mode, reg, isz, &ea8); r = (dst | (c->d[dn] & m68k_sz_mask(isz))) & m68k_sz_mask(isz); m68k_rmw(s, mode, reg, isz, ea8, r); }
        m68k_flags_logic(c, r, isz);
        break;
    }

    /* ---- group 9: SUB / SUBA / SUBX ---- */
    case 0x9: {
        int dn   = (op >> 9) & 7;
        int dir  = (op >> 8) & 1;
        int mode = (op >> 3) & 7, reg = op & 7;
        int sz_bits = (op >> 6) & 3;

        if (sz_bits == 3) {
            /* SUBA */
            int isz = dir ? SZ_L : SZ_W;
            int32_t src = m68k_sign_ext(m68k_ea_read(s, mode, reg, isz), isz);
            c->a[dn] -= (uint32_t)src;
            break;
        }
        /* SUBX: 1001 Dn 1 sz 00 Dm  or  1001 Dn 1 sz 01 -(Am) */
        if (dir && mode <= 1) {
            int isz = (sz_bits==0)?SZ_B:(sz_bits==1)?SZ_W:SZ_L;
            uint32_t src, dst;
            if (mode == 0) { src = c->d[reg] & m68k_sz_mask(isz); dst = c->d[dn] & m68k_sz_mask(isz); }
            else           {
                s->cpu.a[reg] -= (sz_bits==0&&reg==7)?2:isz;
                src = sz_bits==0?m68k_rb(s,s->cpu.a[reg]):sz_bits==1?m68k_rw(s,s->cpu.a[reg]):m68k_rl(s,s->cpu.a[reg]);
                s->cpu.a[dn]  -= (sz_bits==0&&dn==7)?2:isz;
                dst = sz_bits==0?m68k_rb(s,s->cpu.a[dn]):sz_bits==1?m68k_rw(s,s->cpu.a[dn]):m68k_rl(s,s->cpu.a[dn]);
            }
            int x = (c->sr & M68K_SR_X) != 0;
            uint64_t u = (uint64_t)(dst & m68k_sz_mask(isz)) - (uint64_t)(src & m68k_sz_mask(isz)) - x;
            uint32_t r = (uint32_t)u & m68k_sz_mask(isz);
            if (mode == 0) m68k_dn_write(c, dn, r, isz);
            else { if(isz==SZ_B) m68k_wb(s,s->cpu.a[dn],(uint8_t)r); else if(isz==SZ_W) m68k_ww(s,s->cpu.a[dn],(uint16_t)r); else m68k_wl(s,s->cpu.a[dn],r); }
            m68k_flags_subx(c, dst, src + x, isz);
            break;
        }
        int isz = (sz_bits==0)?SZ_B:(sz_bits==1)?SZ_W:SZ_L;
        uint32_t src2, r2;
        if (!dir) {
            src2 = m68k_ea_read(s, mode, reg, isz);
            r2   = (c->d[dn] - src2) & m68k_sz_mask(isz);
            m68k_flags_sub(c, c->d[dn], src2, isz);
            m68k_dn_write(c, dn, r2, isz);
        } else {
            src2 = c->d[dn] & m68k_sz_mask(isz);
            uint32_t ea2; uint32_t dst2 = m68k_rmr(s, mode, reg, isz, &ea2);
            r2   = (dst2 - src2) & m68k_sz_mask(isz);
            m68k_flags_sub(c, dst2, src2, isz);
            m68k_rmw(s, mode, reg, isz, ea2, r2);
        }
        break;
    }

    /* ---- group A: reserved ---- */
    case 0xA:
        LOG_WARN("m68k: line-A emulator trap op=0x%04X pc=0x%06X", op, op_pc);
        break;

    /* ---- group B: CMP / CMPA / CMPM / EOR ---- */
    case 0xB: {
        int dn   = (op >> 9) & 7;
        int dir  = (op >> 8) & 1;
        int mode = (op >> 3) & 7, reg = op & 7;
        int sz_bits = (op >> 6) & 3;

        if (sz_bits == 3) {
            /* CMPA */
            int isz  = dir ? SZ_L : SZ_W;
            int32_t src = m68k_sign_ext(m68k_ea_read(s, mode, reg, isz), isz);
            int32_t dst2 = (int32_t)c->a[dn];
            m68k_flags_cmp(c, (uint32_t)dst2, (uint32_t)src, SZ_L);
            break;
        }
        int isz = (sz_bits==0)?SZ_B:(sz_bits==1)?SZ_W:SZ_L;
        if (!dir) {
            /* CMP Dn ← compare */
            uint32_t src2 = m68k_ea_read(s, mode, reg, isz);
            m68k_flags_cmp(c, c->d[dn], src2, isz);
        } else {
            if (mode == 1) {
                /* CMPM (Am)+,(An)+ */
                uint32_t src2 = m68k_ea_read(s, 3, reg, isz);
                uint32_t dst2 = m68k_ea_read(s, 3, dn,  isz);
                m68k_flags_cmp(c, dst2, src2, isz);
            } else {
                /* EOR Dn, ea */
                uint32_t eaB; uint32_t dst2 = m68k_rmr(s, mode, reg, isz, &eaB);
                uint32_t r    = dst2 ^ (c->d[dn] & m68k_sz_mask(isz));
                m68k_rmw(s, mode, reg, isz, eaB, r);
                m68k_flags_logic(c, r, isz);
            }
        }
        break;
    }

    /* ---- group C: AND / MULU / MULS / ABCD / EXG ---- */
    case 0xC: {
        int dn   = (op >> 9) & 7;
        int dir  = (op >> 8) & 1;
        int mode = (op >> 3) & 7, reg = op & 7;
        int sz_bits = (op >> 6) & 3;

        if (sz_bits == 3) {
            if (!dir) {
                /* MULU.W */
                uint16_t src = (uint16_t)m68k_ea_read(s, mode, reg, SZ_W);
                uint32_t r   = (c->d[dn] & 0xFFFF) * src;
                c->d[dn] = r;
                m68k_flags_logic(c, r, SZ_L);
            } else {
                /* MULS.W */
                int16_t src2 = (int16_t)(uint16_t)m68k_ea_read(s, mode, reg, SZ_W);
                int32_t r2   = (int32_t)(int16_t)(uint16_t)(c->d[dn] & 0xFFFF) * src2;
                c->d[dn] = (uint32_t)r2;
                m68k_flags_logic(c, (uint32_t)r2, SZ_L);
            }
            break;
        }
        /* EXG: 1100 Dn 1 op 0 Rn */
        if (dir && (sz_bits == 1 || sz_bits == 2)) {
            if (sz_bits == 1) { /* EXG Dm,Dn */
                uint32_t tmp = c->d[dn]; c->d[dn] = c->d[reg]; c->d[reg] = tmp;
            } else { /* EXG Am,An */
                uint32_t tmp = c->a[dn]; c->a[dn] = c->a[reg]; c->a[reg] = tmp;
            }
            break;
        }
        if (dir && sz_bits == 3) {
            /* EXG Dn, An: 1100 Dn 1 01000 An */
            uint32_t tmp = c->d[dn]; c->d[dn] = c->a[reg]; c->a[reg] = tmp;
            break;
        }
        int isz = (sz_bits==0)?SZ_B:(sz_bits==1)?SZ_W:SZ_L;
        uint32_t src2, r2;
        if (!dir) { src2 = m68k_ea_read(s, mode, reg, isz); r2 = (c->d[dn] & src2) & m68k_sz_mask(isz); m68k_dn_write(c, dn, r2, isz); }
        else      { uint32_t eaC; uint32_t vC = m68k_rmr(s, mode, reg, isz, &eaC); r2 = (vC & (c->d[dn] & m68k_sz_mask(isz))) & m68k_sz_mask(isz); m68k_rmw(s, mode, reg, isz, eaC, r2); }
        m68k_flags_logic(c, r2, isz);
        break;
    }

    /* ---- group D: ADD / ADDA / ADDX ---- */
    case 0xD: {
        int dn   = (op >> 9) & 7;
        int dir  = (op >> 8) & 1;
        int mode = (op >> 3) & 7, reg = op & 7;
        int sz_bits = (op >> 6) & 3;

        if (sz_bits == 3) {
            /* ADDA */
            int isz  = dir ? SZ_L : SZ_W;
            int32_t src = m68k_sign_ext(m68k_ea_read(s, mode, reg, isz), isz);
            c->a[dn] += (uint32_t)src;
            break;
        }
        /* ADDX: 1101 Dn 1 sz 00 Dm  or  1101 Dn 1 sz 01 -(Am) */
        if (dir && mode <= 1) {
            int isz = (sz_bits==0)?SZ_B:(sz_bits==1)?SZ_W:SZ_L;
            uint32_t src3, dst3;
            if (mode == 0) { src3 = c->d[reg] & m68k_sz_mask(isz); dst3 = c->d[dn] & m68k_sz_mask(isz); }
            else           {
                s->cpu.a[reg] -= (sz_bits==0&&reg==7)?2:isz;
                src3 = sz_bits==0?m68k_rb(s,s->cpu.a[reg]):sz_bits==1?m68k_rw(s,s->cpu.a[reg]):m68k_rl(s,s->cpu.a[reg]);
                s->cpu.a[dn]  -= (sz_bits==0&&dn==7)?2:isz;
                dst3 = sz_bits==0?m68k_rb(s,s->cpu.a[dn]):sz_bits==1?m68k_rw(s,s->cpu.a[dn]):m68k_rl(s,s->cpu.a[dn]);
            }
            int x3 = (c->sr & M68K_SR_X) != 0;
            uint64_t u3 = (uint64_t)(dst3 & m68k_sz_mask(isz)) + (uint64_t)(src3 & m68k_sz_mask(isz)) + x3;
            uint32_t r3 = (uint32_t)u3 & m68k_sz_mask(isz);
            if (mode == 0) m68k_dn_write(c, dn, r3, isz);
            else { if(isz==SZ_B) m68k_wb(s,s->cpu.a[dn],(uint8_t)r3); else if(isz==SZ_W) m68k_ww(s,s->cpu.a[dn],(uint16_t)r3); else m68k_wl(s,s->cpu.a[dn],r3); }
            m68k_flags_addx(c, dst3, src3 + x3, isz);
            break;
        }
        int isz = (sz_bits==0)?SZ_B:(sz_bits==1)?SZ_W:SZ_L;
        uint32_t src4, r4;
        if (!dir) {
            src4 = m68k_ea_read(s, mode, reg, isz);
            r4   = (c->d[dn] + src4) & m68k_sz_mask(isz);
            m68k_flags_add(c, c->d[dn], src4, isz);
            m68k_dn_write(c, dn, r4, isz);
        } else {
            src4 = c->d[dn] & m68k_sz_mask(isz);
            uint32_t ea4; uint32_t dst4 = m68k_rmr(s, mode, reg, isz, &ea4);
            r4   = (dst4 + src4) & m68k_sz_mask(isz);
            m68k_flags_add(c, dst4, src4, isz);
            m68k_rmw(s, mode, reg, isz, ea4, r4);
        }
        break;
    }

    /* ---- group E: shifts / rotates ---- */
    case 0xE: {
        int dir  = (op >> 8) & 1; /* 0=right, 1=left */
        int type = (op >> 3) & 3; /* 0=AS, 1=LS, 2=ROX, 3=RO */
        int mode = (op >> 3) & 7; /* doubles as mode for memory form */
        int reg  = op & 7;
        int sz_bits = (op >> 6) & 3;

        if (sz_bits == 3) {
            /* Memory shift/rotate: one-bit shift of word at EA */
            uint32_t eaE; uint32_t v = m68k_rmr(s, mode, reg, SZ_W, &eaE);
            uint32_t r2;
            switch ((op >> 9) & 7) {
            case 0: r2 = dir ? m68k_do_asl(c, v, 1, SZ_W) : m68k_do_asr(c, v, 1, SZ_W); break;
            case 1: r2 = dir ? m68k_do_lsl(c, v, 1, SZ_W) : m68k_do_lsr(c, v, 1, SZ_W); break;
            case 2: r2 = dir ? m68k_do_roxl(c, v, 1, SZ_W): m68k_do_roxr(c, v, 1, SZ_W); break;
            case 3: r2 = dir ? m68k_do_rol(c, v, 1, SZ_W) : m68k_do_ror(c, v, 1, SZ_W); break;
            default: r2 = v; break;
            }
            m68k_rmw(s, mode, reg, SZ_W, eaE, r2);
            break;
        }

        /* Register shift/rotate */
        int isz  = (sz_bits==0)?SZ_B:(sz_bits==1)?SZ_W:SZ_L;
        int ir   = (op >> 5) & 1; /* 0=immediate, 1=register count */
        int cnt  = ir ? (int)(c->d[(op>>9)&7] % 64) : (((op>>9)&7) ? (op>>9)&7 : 8);

        uint32_t v = c->d[reg] & m68k_sz_mask(isz);
        uint32_t r2;
        switch (type) {
        case 0: r2 = dir ? m68k_do_asl(c,v,cnt,isz) : m68k_do_asr(c,v,cnt,isz); break;
        case 1: r2 = dir ? m68k_do_lsl(c,v,cnt,isz) : m68k_do_lsr(c,v,cnt,isz); break;
        case 2: r2 = dir ? m68k_do_roxl(c,v,cnt,isz): m68k_do_roxr(c,v,cnt,isz); break;
        case 3: r2 = dir ? m68k_do_rol(c,v,cnt,isz) : m68k_do_ror(c,v,cnt,isz); break;
        default: r2 = v; break;
        }
        m68k_dn_write(c, reg, r2, isz);
        break;
    }

    /* ---- group F: reserved / line-F ---- */
    case 0xF:
        LOG_WARN("m68k: line-F op=0x%04X pc=0x%06X", op, op_pc);
        break;

    unknown_op:
    default:
        LOG_WARN("m68k: unknown opcode=0x%04X pc=0x%06X", op, op_pc);
        break;
    }

    return (int)(c->cycles);
}

/* ================================================================ interrupt */

/*
 * Deliver an interrupt at the given level (1–7).  Returns 1 if accepted.
 *
 * Autovector rule: level N uses vector (24+N), stored at byte address
 * (24+N)*4 in wave RAM (written there by the 68K driver's CopyVectors).
 *
 * Stack frame (68000 format, no format word):
 *   [SP+0..1]  old SR
 *   [SP+2..5]  old PC
 */
static inline int m68k_interrupt(m68k_state_t *s, int level) {
    m68k_cpu_t *c = &s->cpu;
    int cur_ipl = (c->sr >> 8) & 7;
    if (level != 7 && level <= cur_ipl) return 0;   /* masked */

    uint32_t vec_addr = (uint32_t)(24 + level) * 4;
    uint32_t handler  = s->read_cb(s->mem_ctx, vec_addr, 4);
    if (!handler || handler == 0xFFFFFFFFu) return 0; /* vector not set */

    uint16_t old_sr = c->sr;
    uint32_t old_pc = c->pc;

    /* Enter supervisor mode, raise IPL to current interrupt level. */
    c->sr |= M68K_SR_S;
    c->sr &= ~(uint16_t)(M68K_SR_T1 | M68K_SR_T0);
    c->sr = (uint16_t)((c->sr & ~M68K_SR_IPL) | (uint16_t)(level << 8));

    /* Switch A7 to SSP if coming from user mode. */
    if (!(old_sr & M68K_SR_S)) {
        c->usp  = c->a[7];
        c->a[7] = c->ssp;
    }

    /* Push PC then SR (RTE pops SR then PC). */
    c->a[7] -= 4; m68k_wl(s, c->a[7], old_pc);
    c->a[7] -= 2; m68k_ww(s, c->a[7], old_sr);

    c->pc      = handler;
    c->stopped = 0;
    c->halted  = 0;
    return 1;
}

#endif /* M68K_EXEC_H */
