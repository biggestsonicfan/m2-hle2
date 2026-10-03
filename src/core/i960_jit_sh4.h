/* i960_jit_sh4.h -- the decoded blocks compiled to SH-4 code (I960_JIT builds,
 * the Dreamcast; included by i960_blocks.h).
 *
 * ib_run replays a block's ops through a switch, ~25 SH-4 instructions an op
 * before the op's own work. Here each block is turned once into straight SH-4
 * code that does exactly what ib_run does, op for op, and the replay is a call.
 *
 * The code keeps no i960 register in an SH-4 one: every op loads its sources
 * from the cpu and stores its result there (r14 = the globals, r13 = the
 * locals), so the C around it sees the same state at every call out. The
 * condition code lives in r9 (the AC) between ops. A load or store tries the
 * bus's direct page (r12 = rd_page, r11 = wr_page) and an aligned address
 * inline; anything else -- MMIO, a page edge, an unaligned word, the
 * multi-word forms -- calls ibj_slow, which runs that op the way ib_run does
 * (IB_SYNC included) and says whether the block has to stop after it.
 *
 * What ib_run sets on the way and nothing inside the block reads -- the bus's
 * cpu_ip, g_last_store_ip, g_mem_last_write_ip -- is set when something could
 * read it: before a call out, and at the end (ibj_catch_up). The cycles and
 * the next IP are the wrapper's, from the ops the code says it ran.
 *
 * The code returns k, the ops run, or -k when the k-th was a branch taken. */
#ifndef I960_JIT_SH4_H
#define I960_JIT_SH4_H

#if I960_JIT

#ifndef IB_JIT_BYTES
#define IB_JIT_BYTES (384u * 1024u)    /* the code buffer; full, every block goes */
#endif
#define IBJ_ROOM     2048u             /* a block's code at most, with its pool */
#define IBJ_POOL     64                /* constants a block at most */

typedef int32_t (*ibj_fn_t)(i960_cpu_t *cpu, void *sfr, uint8_t **rd, uint8_t **wr);

struct ibj_meta {
    ibj_fn_t fn;
    uint16_t cpre[IB_MAX + 1];   /* cycles of the first k ops */
    uint16_t stmask;             /* ops that store (bus->cpu_ip, g_mem_last_write_ip) */
    uint16_t lsmask;             /* ops that set g_last_store_ip */
};

/* Pages of its own: Flycast drops the SH-4 code it compiled from a 4 KB page
 * whenever that page is written, so data beside the code costs it a recompile
 * on every write. */
static uint8_t  s_ibj_buf[IB_JIT_BYTES] __attribute__((aligned(4096)));
static uint32_t s_ibj_used;
#ifndef IB_JIT_ON
#define IB_JIT_ON 1
#endif
static int      s_ibj_on = IB_JIT_ON;    /* 0: ib_run replays every block (the self-test) */
static struct { uint32_t blocks, fails, bytes, slow, flushes; uint64_t us_compile; } g_ibj;

/* The block in flight, for ibj_slow. */
static struct {
    i960_cpu_t        *cpu;
    memory_bus_t      *bus;
    const ib_block_t  *blk;
    uint64_t           base;
    irqt_count_t       h0;
    uint32_t           attn;
    int                bus_idx, ls_idx, mw_idx;   /* the op each global was last set for */
} s_ibj;

static inline void ibj_reset(void) { s_ibj_used = 0; }

/* ---- The emitter ---------------------------------------------------------- */

typedef struct {
    uint16_t *p, *start;
    uint32_t  pool[IBJ_POOL];
    int       npool;
    uint16_t *pfix[IBJ_POOL * 2];
    int       pidx[IBJ_POOL * 2], nfix;
    uint16_t *epi[IB_MAX * 2 + 2];       /* `bra epilogue`s to patch */
    int       nepi;
    i960_cpu_t *cpu;
    bool      fail;
} ibj_t;

#define IBJ_R(n) ((uint16_t)(n))
static inline void ibj_e(ibj_t *j, uint16_t w) { *j->p++ = w; }
#define E_MOV(m, n)     ibj_e(j, 0x6003 | IBJ_R(n) << 8 | IBJ_R(m) << 4)
#define E_MOVI(i, n)    ibj_e(j, 0xE000 | IBJ_R(n) << 8 | (uint16_t)((i) & 0xFF))
#define E_LDL(d, m, n)  ibj_e(j, 0x5000 | IBJ_R(n) << 8 | IBJ_R(m) << 4 | (uint16_t)(d))   /* mov.l @(d*4,Rm),Rn */
#define E_STL(m, d, n)  ibj_e(j, 0x1000 | IBJ_R(n) << 8 | IBJ_R(m) << 4 | (uint16_t)(d))   /* mov.l Rm,@(d*4,Rn) */
#define E_LDL0(m, n)    ibj_e(j, 0x000E | IBJ_R(n) << 8 | IBJ_R(m) << 4)                   /* mov.l @(r0,Rm),Rn */
#define E_LDIND(sz, m, n) ibj_e(j, (sz) == 4 ? 0x6002 | IBJ_R(n) << 8 | IBJ_R(m) << 4 : (sz) == 2 ? 0x6001 | IBJ_R(n) << 8 | IBJ_R(m) << 4 : 0x6000 | IBJ_R(n) << 8 | IBJ_R(m) << 4)
#define E_STIND(sz, m, n) ibj_e(j, (sz) == 4 ? 0x2002 | IBJ_R(n) << 8 | IBJ_R(m) << 4 : (sz) == 2 ? 0x2001 | IBJ_R(n) << 8 | IBJ_R(m) << 4 : 0x2000 | IBJ_R(n) << 8 | IBJ_R(m) << 4)
#define E_PUSH(m)       ibj_e(j, 0x2F06 | IBJ_R(m) << 4)
#define E_POP(n)        ibj_e(j, 0x60F6 | IBJ_R(n) << 8)
#define E_ADD(m, n)     ibj_e(j, 0x300C | IBJ_R(n) << 8 | IBJ_R(m) << 4)
#define E_ADDI(i, n)    ibj_e(j, 0x7000 | IBJ_R(n) << 8 | (uint16_t)((i) & 0xFF))
#define E_SUB(m, n)     ibj_e(j, 0x3008 | IBJ_R(n) << 8 | IBJ_R(m) << 4)
#define E_AND(m, n)     ibj_e(j, 0x2009 | IBJ_R(n) << 8 | IBJ_R(m) << 4)
#define E_OR(m, n)      ibj_e(j, 0x200B | IBJ_R(n) << 8 | IBJ_R(m) << 4)
#define E_XOR(m, n)     ibj_e(j, 0x200A | IBJ_R(n) << 8 | IBJ_R(m) << 4)
#define E_TST(m, n)     ibj_e(j, 0x2008 | IBJ_R(n) << 8 | IBJ_R(m) << 4)
#define E_NOT(m, n)     ibj_e(j, 0x6007 | IBJ_R(n) << 8 | IBJ_R(m) << 4)
#define E_NEG(m, n)     ibj_e(j, 0x600B | IBJ_R(n) << 8 | IBJ_R(m) << 4)
#define E_EXTUB(m, n)   ibj_e(j, 0x600C | IBJ_R(n) << 8 | IBJ_R(m) << 4)
#define E_EXTUW(m, n)   ibj_e(j, 0x600D | IBJ_R(n) << 8 | IBJ_R(m) << 4)
#define E_CMPEQ(m, n)   ibj_e(j, 0x3000 | IBJ_R(n) << 8 | IBJ_R(m) << 4)
#define E_CMPHI(m, n)   ibj_e(j, 0x3006 | IBJ_R(n) << 8 | IBJ_R(m) << 4)   /* T = Rn > Rm, unsigned */
#define E_CMPGT(m, n)   ibj_e(j, 0x3007 | IBJ_R(n) << 8 | IBJ_R(m) << 4)   /* T = Rn > Rm, signed */
#define E_SHLD(m, n)    ibj_e(j, 0x400D | IBJ_R(n) << 8 | IBJ_R(m) << 4)
#define E_SHAD(m, n)    ibj_e(j, 0x400C | IBJ_R(n) << 8 | IBJ_R(m) << 4)
#define E_SHLL(n)       ibj_e(j, 0x4000 | IBJ_R(n) << 8)
#define E_SHLL2(n)      ibj_e(j, 0x4008 | IBJ_R(n) << 8)
#define E_SHLR16(n)     ibj_e(j, 0x4029 | IBJ_R(n) << 8)
#define E_MULL(m, n)    ibj_e(j, 0x0007 | IBJ_R(n) << 8 | IBJ_R(m) << 4)
#define E_STSMACL(n)    ibj_e(j, 0x001A | IBJ_R(n) << 8)
#define E_MOVT(n)       ibj_e(j, 0x0029 | IBJ_R(n) << 8)
#define E_TSTI(i)       ibj_e(j, 0xC800 | (uint16_t)((i) & 0xFF))
#define E_ANDI(i)       ibj_e(j, 0xC900 | (uint16_t)((i) & 0xFF))
#define E_XORI(i)       ibj_e(j, 0xCA00 | (uint16_t)((i) & 0xFF))
#define E_BT(d)         ibj_e(j, 0x8900 | (uint16_t)((d) & 0xFF))
#define E_BF(d)         ibj_e(j, 0x8B00 | (uint16_t)((d) & 0xFF))
#define E_BRA(d)        ibj_e(j, 0xA000 | (uint16_t)((d) & 0xFFF))
#define E_JSR(m)        ibj_e(j, 0x400B | IBJ_R(m) << 8)
#define E_RTS()         ibj_e(j, 0x000B)
#define E_NOP()         ibj_e(j, 0x0009)
#define E_STSLPR()      ibj_e(j, 0x4F22)
#define E_LDSLPR()      ibj_e(j, 0x4F26)

/* Point a bt/bf/bra emitted at `at` at the current position. */
static inline void ibj_land(ibj_t *j, uint16_t *at) {
    if (!at) return;
    int32_t d = (int32_t)(j->p - (at + 2));       /* in instructions, from at + 4 bytes */
    bool bra = (*at & 0xF000) == 0xA000;
    if (bra ? (d < -2048 || d > 2047) : (d < -128 || d > 127)) { j->fail = true; return; }
    *at = bra ? (uint16_t)((*at & 0xF000) | (d & 0xFFF)) : (uint16_t)((*at & 0xFF00) | (d & 0xFF));
}

/* rn = v */
static inline void ibj_imm(ibj_t *j, int rn, uint32_t v) {
    if ((int32_t)v >= -128 && (int32_t)v <= 127) { E_MOVI((int32_t)v, rn); return; }
    int i = 0;
    while (i < j->npool && j->pool[i] != v) i++;
    if (i == j->npool) {
        if (j->npool == IBJ_POOL) { j->fail = true; return; }
        j->pool[j->npool++] = v;
    }
    if (j->nfix == IBJ_POOL * 2) { j->fail = true; return; }
    j->pfix[j->nfix] = j->p; j->pidx[j->nfix++] = i;
    ibj_e(j, 0xD000 | IBJ_R(rn) << 8);                /* mov.l @(disp,pc),rn: patched */
}

/* A source's place: its cpu word (0..31, see ib_reg), or -1 for a constant. */
static inline int ibj_slot(ibj_t *j, const uint32_t *p) {
    ptrdiff_t off = p - (const uint32_t *)j->cpu;
    return off >= 0 && off < 32 ? (int)off : -1;
}
static inline bool ibj_const(ibj_t *j, const uint32_t *p) { return ibj_slot(j, p) < 0; }

static inline void ibj_ld(ibj_t *j, int rn, const uint32_t *p) {
    int s = ibj_slot(j, p);
    if (s < 0)       ibj_imm(j, rn, *p);
    else if (s < 16) E_LDL(s, 14, rn);
    else             E_LDL(s - 16, 13, rn);
}
static inline void ibj_st(ibj_t *j, int rm, const uint32_t *p) {
    int s = ibj_slot(j, p);
    if (s < 0)       { if (p != &s_ib_sink) j->fail = true; }
    else if (s < 16) E_STL(rm, s, 14);
    else             E_STL(rm, s - 16, 13);
}
/* The same, as one instruction (a delay slot), or a nop for the sink. */
static inline void ibj_st_slot(ibj_t *j, int rm, const uint32_t *p) {
    int s = ibj_slot(j, p);
    if (s < 0) { E_NOP(); if (p != &s_ib_sink) j->fail = true; }
    else if (s < 16) E_STL(rm, s, 14);
    else             E_STL(rm, s - 16, 13);
}

static inline void ibj_addk(ibj_t *j, int rn, uint32_t v) {
    if (!v) return;
    if ((int32_t)v >= -128 && (int32_t)v <= 127) { E_ADDI((int32_t)v, rn); return; }
    ibj_imm(j, 3, v); E_ADD(3, rn);
}

/* r1 = the op's address, k + *a + (*b << sh) */
static inline void ibj_ea(ibj_t *j, const ib_op_t *o) {
    bool ac = ibj_const(j, o->a), bc = ibj_const(j, o->b);
    uint32_t k = o->k + (ac ? *o->a : 0u) + (bc ? *o->b << o->sh : 0u);
    if (ac) ibj_imm(j, 1, k);
    else  { ibj_ld(j, 1, o->a); ibj_addk(j, 1, k); }
    if (!bc) {
        ibj_ld(j, 2, o->b);
        switch (o->sh) {
            case 0: break;
            case 1: E_SHLL(2); break;
            case 2: E_SHLL2(2); break;
            case 3: E_SHLL2(2); E_SHLL(2); break;
            default: E_SHLL2(2); E_SHLL2(2); break;
        }
        E_ADD(2, 1);
    }
}

/* r3 = the condition code of compare(r1, r2): L, E or G. */
static inline void ibj_cmp(ibj_t *j, bool sgn) {
    if (sgn) E_CMPGT(1, 2); else E_CMPHI(1, 2);  /* s2 > s1: L */
    E_MOVT(3); E_SHLL2(3);
    E_CMPEQ(1, 2); E_MOVT(4); E_ADD(4, 4); E_OR(4, 3);
    if (sgn) E_CMPGT(2, 1); else E_CMPHI(2, 1);  /* s1 > s2: G */
    E_MOVT(4); E_OR(4, 3);
    E_MOVI(-8, 4); E_AND(4, 9); E_OR(3, 9);      /* ac = (ac & ~7) | cc */
}

/* Leave the block, the op i's branch taken, when T is `when`. */
static inline void ibj_exit_taken(ibj_t *j, int i, bool when) {
    if (when) E_BF(1); else E_BT(1);             /* over the bra and its slot */
    if (j->nepi == (int)(sizeof j->epi / sizeof j->epi[0])) { j->fail = true; return; }
    j->epi[j->nepi++] = j->p; E_BRA(0);
    E_MOVI(-(i + 1), 0);
}

static int32_t ibj_slow(uint32_t ea, uint32_t i);

/* The tail of a memory op: r1 the address; for a store, r2 the value. */
static inline void ibj_mem(ibj_t *j, const ib_op_t *o, int i, int sz, bool store, int ext) {
    uint16_t *miss1, *miss2 = NULL, *done;
    E_MOV(1, 0); E_SHLR16(0); E_SHLL2(0);
    E_LDL0(store ? 11 : 12, 3);
    E_TST(3, 3); miss1 = j->p; E_BT(0);
    E_EXTUW(1, 0); E_ADD(3, 0);
    if (sz > 1) { E_TSTI(sz - 1); miss2 = j->p; E_BF(0); }
    if (store) {
        done = j->p; E_BRA(0);
        E_STIND(sz, 2, 0);
    } else {
        E_LDIND(sz, 0, 2);
        if (ext == 1) E_EXTUB(2, 2);
        if (ext == 2) E_EXTUW(2, 2);
        done = j->p; E_BRA(0);
        ibj_st_slot(j, 2, o->d);
    }
    /* the slow path: ibj_slow(ea, i), and leave after op i if it says so */
    ibj_land(j, miss1); ibj_land(j, miss2);
    E_STL(9, 1, 10);                 /* the AC, for the C */
    ibj_imm(j, 2, (uint32_t)(uintptr_t)&ibj_slow);
    E_MOV(1, 4);
    E_JSR(2);
    E_MOVI(i, 5);
    E_LDL(1, 10, 9);
    E_TST(0, 0);
    E_BT(1);
    if (j->nepi == (int)(sizeof j->epi / sizeof j->epi[0])) { j->fail = true; return; }
    j->epi[j->nepi++] = j->p; E_BRA(0);
    E_MOVI(i + 1, 0);
    ibj_land(j, done);
}

/* The slow path alone (LDN/STN). */
static inline void ibj_call(ibj_t *j, int i) {
    E_STL(9, 1, 10);
    ibj_imm(j, 2, (uint32_t)(uintptr_t)&ibj_slow);
    E_MOV(1, 4);
    E_JSR(2);
    E_MOVI(i, 5);
    E_LDL(1, 10, 9);
    E_TST(0, 0);
    E_BT(1);
    if (j->nepi == (int)(sizeof j->epi / sizeof j->epi[0])) { j->fail = true; return; }
    j->epi[j->nepi++] = j->p; E_BRA(0);
    E_MOVI(i + 1, 0);
}

/* r3 = 1 << (s1 & 31) */
static inline void ibj_bitmask(ibj_t *j, const uint32_t *a) {
    if (ibj_const(j, a)) { ibj_imm(j, 3, 1u << (*a & 31u)); return; }
    ibj_ld(j, 0, a); E_ANDI(31); E_MOVI(1, 3); E_SHLD(0, 3);
}

static bool ibj_op(ibj_t *j, const ib_op_t *o, int i) {
    const uint32_t *a = o->a, *b = o->b;
    switch (o->kind) {
    case IB_MOV:    ibj_ld(j, 1, a); ibj_st(j, 1, o->d); break;
    case IB_ADD:
        if (ibj_const(j, a) && (int32_t)*a >= -128 && (int32_t)*a <= 127) { ibj_ld(j, 2, b); ibj_addk(j, 2, *a); }
        else { ibj_ld(j, 1, a); ibj_ld(j, 2, b); E_ADD(1, 2); }
        ibj_st(j, 2, o->d); break;
    case IB_SUB:
        if (ibj_const(j, a) && (int32_t)*a >= -127 && (int32_t)*a <= 128) { ibj_ld(j, 2, b); ibj_addk(j, 2, 0u - *a); }
        else { ibj_ld(j, 1, a); ibj_ld(j, 2, b); E_SUB(1, 2); }
        ibj_st(j, 2, o->d); break;
    case IB_AND:    ibj_ld(j, 1, a); ibj_ld(j, 2, b); E_AND(1, 2); ibj_st(j, 2, o->d); break;
    case IB_OR:     ibj_ld(j, 1, a); ibj_ld(j, 2, b); E_OR(1, 2);  ibj_st(j, 2, o->d); break;
    case IB_XOR:    ibj_ld(j, 1, a); ibj_ld(j, 2, b); E_XOR(1, 2); ibj_st(j, 2, o->d); break;
    case IB_NOT:    ibj_ld(j, 1, a); E_NOT(1, 2); ibj_st(j, 2, o->d); break;
    case IB_ANDNOT: ibj_ld(j, 1, a); ibj_ld(j, 2, b); E_NOT(1, 1); E_AND(1, 2); ibj_st(j, 2, o->d); break;
    case IB_NOTAND: ibj_ld(j, 1, a); ibj_ld(j, 2, b); E_NOT(2, 2); E_AND(1, 2); ibj_st(j, 2, o->d); break;
    case IB_SHRO: case IB_SHLO: case IB_SHRI:
        if (ibj_const(j, a)) {
            uint32_t s = *a;
            if (s >= 32u && o->kind != IB_SHRI) { E_MOVI(0, 2); ibj_st(j, 2, o->d); break; }
            if (s > 31u) s = 31u;
            ibj_ld(j, 2, b);
            if (s) {
                E_MOVI(o->kind == IB_SHLO ? (int32_t)s : -(int32_t)s, 3);
                if (o->kind == IB_SHRI) E_SHAD(3, 2); else E_SHLD(3, 2);
            }
        } else {
            ibj_ld(j, 1, a); ibj_ld(j, 2, b);
            E_MOVI(31, 3); E_CMPHI(3, 1);             /* T = count > 31 */
            if (o->kind == IB_SHRI) {
                E_BF(0); E_MOV(3, 1);                 /* bf over one: count = 31 */
                E_NEG(1, 1); E_SHAD(1, 2);
            } else {
                E_MOVT(3); E_ADDI(-1, 3);             /* r3 = T ? 0 : ~0 */
                if (o->kind == IB_SHRO) { E_NEG(1, 1); }
                E_SHLD(1, 2); E_AND(3, 2);
            }
        }
        ibj_st(j, 2, o->d); break;
    case IB_MULO:   ibj_ld(j, 1, a); ibj_ld(j, 2, b); E_MULL(1, 2); E_STSMACL(2); ibj_st(j, 2, o->d); break;
    case IB_SETBIT: ibj_bitmask(j, a); ibj_ld(j, 2, b); E_OR(3, 2); ibj_st(j, 2, o->d); break;
    case IB_CLRBIT: ibj_bitmask(j, a); ibj_ld(j, 2, b); E_NOT(3, 3); E_AND(3, 2); ibj_st(j, 2, o->d); break;
    case IB_NOTBIT: ibj_bitmask(j, a); ibj_ld(j, 2, b); E_XOR(3, 2); ibj_st(j, 2, o->d); break;
    case IB_CMPO: case IB_CMPI:
        ibj_ld(j, 1, a); ibj_ld(j, 2, b); ibj_cmp(j, o->kind == IB_CMPI); break;
    case IB_CMPINCO: case IB_CMPINCI: case IB_CMPDECO: case IB_CMPDECI:
        ibj_ld(j, 1, a); ibj_ld(j, 2, b);
        ibj_cmp(j, o->kind == IB_CMPINCI || o->kind == IB_CMPDECI);
        E_ADDI(o->kind == IB_CMPINCO || o->kind == IB_CMPINCI ? 1 : -1, 2);
        ibj_st(j, 2, o->d); break;

    case IB_LDA:  ibj_ea(j, o); ibj_st(j, 1, o->d); break;
    case IB_LD:   ibj_ea(j, o); ibj_mem(j, o, i, 4, false, 0); break;
    case IB_LDOB: ibj_ea(j, o); ibj_mem(j, o, i, 1, false, 1); break;
    case IB_LDOS: ibj_ea(j, o); ibj_mem(j, o, i, 2, false, 2); break;
    case IB_LDIB: ibj_ea(j, o); ibj_mem(j, o, i, 1, false, 0); break;
    case IB_LDIS: ibj_ea(j, o); ibj_mem(j, o, i, 2, false, 0); break;
    case IB_ST:   ibj_ea(j, o); ibj_ld(j, 2, o->d); ibj_mem(j, o, i, 4, true, 0); break;
    case IB_STOB: case IB_STIB: ibj_ea(j, o); ibj_ld(j, 2, o->d); ibj_mem(j, o, i, 1, true, 0); break;
    case IB_STOS: case IB_STIS: ibj_ea(j, o); ibj_ld(j, 2, o->d); ibj_mem(j, o, i, 2, true, 0); break;
    case IB_LDN: case IB_STN: ibj_ea(j, o); ibj_call(j, i); break;

    case IB_B: break;
    case IB_BCC:
        E_MOV(9, 0);
        if (o->sh) { E_TSTI(o->sh); ibj_exit_taken(j, i, false); }   /* taken: cc & mask */
        else       { E_TSTI(7);     ibj_exit_taken(j, i, true); }    /* taken: cc == 0 */
        break;
    case IB_BBC: case IB_BBS:
        ibj_bitmask(j, a); ibj_ld(j, 2, b);
        E_TST(3, 2);                                  /* T = the bit is clear */
        E_MOVI(-8, 4); E_AND(4, 9);
        E_MOVT(0);
        if (o->kind == IB_BBS) E_XORI(1);
        E_ADD(0, 0); E_OR(0, 9);                      /* cc = taken ? E : 0 */
        ibj_exit_taken(j, i, o->kind == IB_BBC);
        break;
    case IB_CMPOB: case IB_CMPIB:
        ibj_ld(j, 1, a); ibj_ld(j, 2, b); ibj_cmp(j, o->kind == IB_CMPIB);
        E_MOV(3, 0);
        if (o->sh) { E_TSTI(o->sh); ibj_exit_taken(j, i, false); }
        else       { E_TSTI(7);     ibj_exit_taken(j, i, true); }
        break;
    default: return false;
    }
    return !j->fail;
}

static inline uint16_t ibj_store_bits(const ib_block_t *b, bool ls) {
    uint16_t m = 0;
    for (uint32_t i = 0; i < b->n; i++) {
        uint8_t k = b->op[i].kind;
        bool st = k == IB_ST || k == IB_STOB || k == IB_STOS || k == IB_STN;
        if (st || (!ls && (k == IB_STIB || k == IB_STIS))) m |= (uint16_t)(1u << i);
    }
    return m;
}

/* Compile b. False leaves it to ib_run. */
static bool ib_jit_compile(i960_cpu_t *cpu, ib_block_t *b) {
    b->jit = NULL;
    if (!b->n || s_ibj_used + IBJ_ROOM > IB_JIT_BYTES) return false;
    struct ibj_meta *m = (struct ibj_meta *)(s_ibj_buf + s_ibj_used);
    static ibj_t jj;
    ibj_t *j = &jj;
    j->start = j->p = (uint16_t *)((uint8_t *)m + ((sizeof *m + 31u) & ~31u));
    j->npool = j->nfix = j->nepi = 0; j->cpu = cpu; j->fail = false;

    uint32_t c = 0;
    m->cpre[0] = 0;
    for (uint32_t i = 0; i < b->n; i++) { c += b->op[i].cyc & 0x7FFu; m->cpre[i + 1] = (uint16_t)c; }
    m->stmask = ibj_store_bits(b, false);
    m->lsmask = ibj_store_bits(b, true);

    /* prologue: r4 cpu, r5 &sfr, r6 rd_page, r7 wr_page */
    E_PUSH(14); E_PUSH(13); E_PUSH(12); E_PUSH(11); E_PUSH(10); E_PUSH(9); E_STSLPR();
    E_MOV(4, 14); E_MOV(4, 13); E_ADDI(64, 13);
    E_MOV(5, 10); E_MOV(6, 12); E_MOV(7, 11);
    E_LDL(1, 10, 9);
    for (uint32_t i = 0; i < b->n; i++)
        if (!ibj_op(j, &b->op[i], (int)i)) { g_ibj.fails++; return false; }
    E_MOVI((int32_t)b->n, 0);
    for (int e = 0; e < j->nepi; e++) ibj_land(j, j->epi[e]);
    E_STL(9, 1, 10);
    E_LDSLPR(); E_POP(9); E_POP(10); E_POP(11); E_POP(12); E_POP(13);
    E_RTS(); E_POP(14);
    /* the constants, after the code */
    if ((uintptr_t)j->p & 2u) E_NOP();
    uint32_t *pool = (uint32_t *)j->p;
    for (int i = 0; i < j->npool; i++) pool[i] = j->pool[i];
    j->p = (uint16_t *)(pool + j->npool);
    for (int f = 0; f < j->nfix; f++) {
        uintptr_t at = (uintptr_t)j->pfix[f], to = (uintptr_t)&pool[j->pidx[f]];
        intptr_t d = (intptr_t)(to - ((at & ~(uintptr_t)3u) + 4u)) / 4;
        if (d < 0 || d > 255) { j->fail = true; break; }
        *j->pfix[f] |= (uint16_t)d;
    }
    if (j->fail) { g_ibj.fails++; return false; }
    uint32_t end = (uint32_t)((uint8_t *)j->p - s_ibj_buf);
    end = (end + 31u) & ~31u;
    m->fn = (ibj_fn_t)(void *)j->start;
#ifdef _arch_dreamcast
    icache_sync_range((uintptr_t)m, end - s_ibj_used);
#endif
    g_ibj.blocks++;
    g_ibj.bytes += end - s_ibj_used;
    s_ibj_used = end;
    b->jit = m;
    return true;
}

/* ---- The C side ----------------------------------------------------------- */

static inline int ibj_top(uint32_t m) { int i = -1; while (m) { i++; m >>= 1; } return i; }

/* What ib_run would have set by op i (before it): the last store's IP. */
static inline void ibj_catch_up(uint32_t i) {
    const struct ibj_meta *m = s_ibj.blk->jit;
    uint32_t below = (1u << i) - 1u;
    int s = ibj_top(m->stmask & below);
    if (s > s_ibj.bus_idx) { s_ibj.bus->cpu_ip = s_ibj.blk->op[s].ip; s_ibj.bus_idx = s; }
    if (s > s_ibj.mw_idx)  { g_mem_last_write_ip = s_ibj.blk->op[s].ip; s_ibj.mw_idx = s; }
    s = ibj_top(m->lsmask & below);
    if (s > s_ibj.ls_idx)  { g_last_store_ip = s_ibj.blk->op[s].ip; s_ibj.ls_idx = s; }
}

/* Op i, from its address ea, as ib_run runs it. Nonzero: stop after it. */
static int32_t ibj_slow(uint32_t ea, uint32_t i) {
    i960_cpu_t *cpu = s_ibj.cpu;
    memory_bus_t *bus = s_ibj.bus;
    const ib_op_t *o = &s_ibj.blk->op[i];
    const uint64_t base = s_ibj.base;
    const uint32_t c = s_ibj.blk->jit->cpre[i + 1];
    const uint32_t ac = cpu->sfr.ac;
    bool slow = false;
    g_ibj.slow++;
    ibj_catch_up(i);
#define IB_SYNC() do { cpu->sfr.ip = o->ip; bus->cpu_ip = o->ip; s_ibj.bus_idx = (int)i; cpu->sfr.ac = ac; \
        cpu->cycles = base + (c - (o->cyc & 0x7FFu));                                            \
        g_irqt.pending += (irqt_count_t)(uint32_t)(cpu->cycles - s_timer_cycles_seen);           \
        s_timer_cycles_seen = cpu->cycles; cpu->cycles = base + c; slow = true; } while (0)
#define IB_RD(sz)  (M2_UNLIKELY(!bus->rd_page[ea >> 16] || (ea & 0xFFFFu) > 0x10000u - (sz)))
#define IB_WR(sz)  (M2_UNLIKELY(!bus->wr_page[ea >> 16] || (ea & 0xFFFFu) > 0x10000u - (sz)))
#define IB_STORED(ls) do { bus->cpu_ip = o->ip; s_ibj.bus_idx = s_ibj.mw_idx = (int)i;            \
        if (ls) { g_last_store_ip = o->ip; s_ibj.ls_idx = (int)i; } } while (0)
    switch (o->kind) {
    case IB_LD:   if (IB_RD(4)) IB_SYNC(); *o->d = mem_read32(bus, ea); break;
    case IB_LDOB: if (IB_RD(1)) IB_SYNC(); *o->d = mem_read8(bus, ea); break;
    case IB_LDOS: if (IB_RD(2)) IB_SYNC(); *o->d = mem_read16(bus, ea); break;
    case IB_LDIB: if (IB_RD(1)) IB_SYNC(); *o->d = (uint32_t)(int32_t)(int8_t)mem_read8(bus, ea); break;
    case IB_LDIS: if (IB_RD(2)) IB_SYNC(); *o->d = (uint32_t)(int32_t)(int16_t)mem_read16(bus, ea); break;
    case IB_ST:   if (IB_WR(4)) IB_SYNC(); IB_STORED(1); mem_write32(bus, ea, *o->d); break;
    case IB_STOB: if (IB_WR(1)) IB_SYNC(); IB_STORED(1); mem_write8(bus, ea, (uint8_t)*o->d); break;
    case IB_STOS: if (IB_WR(2)) IB_SYNC(); IB_STORED(1); mem_write16(bus, ea, (uint16_t)*o->d); break;
    case IB_STIB: if (IB_WR(1)) IB_SYNC(); IB_STORED(0); mem_write8(bus, ea, (uint8_t)*o->d); break;
    case IB_STIS: if (IB_WR(2)) IB_SYNC(); IB_STORED(0); mem_write16(bus, ea, (uint16_t)*o->d); break;
    case IB_LDN: {
        uint32_t a = ea, n = (uint32_t)(uintptr_t)o->d, r = (uint32_t)o->cyc >> 11;
        IB_SYNC();
        for (uint32_t k = 0; k < n; k++) { reg_write(cpu, (int)(r + k), mem_read32(bus, a)); a += mem_burst_step(bus, a); }
        break;
    }
    case IB_STN: {
        uint32_t a = ea, n = (uint32_t)(uintptr_t)o->d, r = (uint32_t)o->cyc >> 11;
        IB_SYNC();
        g_last_store_ip = o->ip; s_ibj.ls_idx = s_ibj.mw_idx = (int)i;
        for (uint32_t k = 0; k < n; k++) { mem_write32(bus, a, reg_read(cpu, (int)(r + k))); a += mem_burst_step(bus, a); }
        break;
    }
    default: break;
    }
#undef IB_SYNC
#undef IB_RD
#undef IB_WR
#undef IB_STORED
    if (M2_UNLIKELY(slow))
        return g_emu_attn != s_ibj.attn || (g_irqt.intreq & g_irqt.intena & 0x03FFu) || g_irqt.horizon != s_ibj.h0;
    return 0;
}

/* Run blk's code: ib_run's contract (i960_blocks.h). */
static inline uint32_t ib_jit_run(i960_cpu_t *cpu, memory_bus_t *bus, const ib_block_t *blk, uint32_t attn) {
    const struct ibj_meta *m = blk->jit;
    s_ibj.cpu = cpu; s_ibj.bus = bus; s_ibj.blk = blk;
    s_ibj.base = cpu->cycles; s_ibj.h0 = g_irqt.horizon; s_ibj.attn = attn;
    s_ibj.bus_idx = s_ibj.ls_idx = s_ibj.mw_idx = -1;
    int32_t r = m->fn(cpu, &cpu->sfr, bus->rd_page, bus->wr_page);
    uint32_t k, next;
    if (r < 0) { k = (uint32_t)-r; next = blk->op[k - 1].k; }
    else       { k = (uint32_t)r;  next = k < blk->n ? blk->op[k].ip : blk->next; }
    cpu->cycles = s_ibj.base + m->cpre[k];
    cpu->sfr.ip = next;
    ibj_catch_up(k);
    bus->cpu_ip = blk->op[k - 1].ip;
    return k;
}

#endif /* I960_JIT */
#endif /* I960_JIT_SH4_H */
