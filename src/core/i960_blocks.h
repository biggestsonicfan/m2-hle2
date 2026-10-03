/* i960_blocks.h -- the run loop's decoded-block cache (I960_BLOCKS builds).
 *
 * The interpreter pays for every instruction again each time it runs it: the
 * hook filter, the fetch, the cycle table, the format and opcode switches, the
 * operand fields. On the Dreamcast that was ~200 SH-4 instructions an i960
 * instruction, half the frame (DREAMCAST-PORT.md). Here a run of plain
 * instructions -- the register ALU ops, loads and stores, compares and
 * branches, through a `b` and on past a conditional one -- is decoded once into ops that point straight at their registers,
 * and replayed from then on.
 *
 * The replay has to be the interpreter, instruction for instruction, as the
 * board sees it. So a block only runs when nothing can happen between its
 * instructions that the run loop's fast path would have seen: no timer due
 * before its last cycle (pending + its cycles < horizon), no interrupt
 * waiting, the slice room for all of it, no breakpoint, nothing that sent the
 * loop slow. An access that leaves plain memory (MMIO: the COP FIFO, the
 * timers) is the only thing inside a block that can change that, and it gets
 * the interpreter's state first -- the IP, the cycles with this instruction's
 * charged, the board's clock up to the one before -- and the block stops after
 * it if the attention word, the interrupt lines or the horizon moved.
 *
 * Blocks come only from code on read-only direct pages (the program ROM), stop
 * before any address the hook filter flags (a hook is only ever entered by
 * the interpreter), and are dropped when the profile, and so the filter,
 * changes. Anything not decoded here -- calls, returns, FP, the rare forms --
 * is left to i960_step_core. */
#ifndef I960_BLOCKS_H
#define I960_BLOCKS_H

#ifndef I960_BLOCKS
#define I960_BLOCKS 0
#endif

#if I960_BLOCKS

#ifndef IB_ENTRIES
#define IB_ENTRIES 1024u          /* direct-mapped by IP; a power of two */
#endif
#ifndef IB_POOL
#define IB_POOL    (IB_ENTRIES * 8u)   /* ops the blocks share; full, it starts over */
#endif
#ifndef IB_MISSES
#define IB_MISSES  8              /* misses in a row that evict a block */
#endif
#define IB_MAX     16             /* ops a block at most */
#ifndef I960_JIT
#define I960_JIT   0              /* compile blocks to SH-4 code (i960_jit_sh4.h) */
#endif

enum {
    IB_MOV, IB_ADD, IB_SUB, IB_AND, IB_OR, IB_XOR, IB_NOT, IB_ANDNOT, IB_NOTAND,
    IB_SHRO, IB_SHLO, IB_SHRI, IB_MULO, IB_SETBIT, IB_CLRBIT, IB_NOTBIT,
    IB_CMPO, IB_CMPI, IB_CMPINCO, IB_CMPINCI, IB_CMPDECO, IB_CMPDECI,
    IB_LDA, IB_LD, IB_LDOB, IB_LDOS, IB_LDIB, IB_LDIS,
    IB_ST, IB_STOB, IB_STOS, IB_STIB, IB_STIS, IB_LDN, IB_STN,
    /* the block's last op */
    IB_B, IB_BCC, IB_BBC, IB_BBS, IB_CMPOB, IB_CMPIB,
};

typedef struct {
    uint8_t         kind;
    uint8_t         sh;       /* mem: the index's shift; LDN/STN: words; BCC/CMP?B: the mask */
    uint16_t        cyc;      /* i960_cycle_cost */
    uint32_t        ip;
    uint32_t       *d;        /* destination; a store's source */
    const uint32_t *a, *b;    /* src1, src2; mem: abase, index */
    uint32_t        k;        /* mem: displacement; branch: target; LDN/STN: first register */
} ib_op_t;

/* A block's ops are a run of the pool: a block of 16 inline ops each was
 * 800 KB at 2048 blocks, which the Dreamcast's heap needed back. */
typedef struct {
    uint32_t ip;              /* tag; 1 = empty (no i960 IP is odd) */
    uint16_t n, cyc;          /* ops; their cycles */
    uint32_t next;            /* the IP after the last op, when it does not branch */
    ib_op_t *op;
#if I960_JIT
    const struct ibj_meta *jit;   /* its code, or NULL */
#endif
#ifdef IB_WHY
    uint32_t t, r;            /* TMU2 ticks in it, runs (main_dc's census) */
#endif
} ib_block_t;

static ib_block_t            s_ib[IB_ENTRIES];
static ib_op_t               s_ib_pool[IB_POOL];
static uint32_t              s_ib_used;          /* pool ops handed out */
static uint8_t               s_ib_miss[IB_ENTRIES];  /* misses since the block's last hit */
static const ib_block_t      s_ib_none = { 1, 0, 0, 0, NULL };  /* "step it" */
static const game_profile_t *s_ib_profile = NULL;
static int                   s_ib_valid   = 0;
static const uint32_t        s_ib_lit[32] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31 };
static uint32_t              s_ib_sink;          /* a write to "register 32+" */
static struct { uint64_t ops, runs, builds; uint64_t why[6], ns[3]; } g_ib;
#ifdef IB_WHY   /* TMU2's raw count (KOS's clock: counts down, reloads once a second) */
#define IBW_T()   (*(volatile uint32_t *)0xFFD80024u)
static inline uint32_t IBW_D(uint32_t t0) { uint32_t t1 = IBW_T(); return t0 >= t1 ? t0 - t1 : t0 + *(volatile uint32_t *)0xFFD80020u - t1; }
#endif   /* why: steps not in a block: slow, empty, too long, horizon, irq */

#include "i960_jit_sh4.h"

/* Drop every block: a new board, a new profile (the hook filter's), new code. */
static inline void ib_flush(void) {
    for (uint32_t i = 0; i < IB_ENTRIES; i++) s_ib[i].ip = 1;
    memset(s_ib_miss, 0, sizeof s_ib_miss);
    s_ib_used  = 0;
    s_ib_valid = 1;
#if I960_JIT
    ibj_reset();
    g_ibj.flushes++;
#endif
}

static inline uint32_t *ib_reg(i960_cpu_t *cpu, uint32_t idx) {
    return idx < 32u ? &((uint32_t *)&cpu->globals)[(idx + 16u) & 31u] : &s_ib_sink;
}
static inline const uint32_t *ib_src(i960_cpu_t *cpu, uint32_t idx, uint32_t lit) {
    return lit ? &s_ib_lit[idx & 31u] : ib_reg(cpu, idx);
}

/* Decode a MEM instruction's address into op: k + *a + (*b << sh). False for
 * the modes mem_ea warns about. */
static inline bool ib_mem_ea(i960_cpu_t *cpu, ib_op_t *o, uint32_t ip, uint32_t w1, uint32_t w2, uint32_t *len) {
    static const uint32_t zero = 0;
    uint32_t mode = MEM_MODE(w1);
    o->a = &zero; o->b = &zero; o->sh = 0; o->k = 0; *len = 4;
    if (!(mode & 4u)) {
        o->k = w1 & 0xFFFu;
        if (mode & 8u) o->a = ib_reg(cpu, MEM_ABASE(w1));
        return true;
    }
    uint32_t s = MEM_SCALE(w1);
    uint8_t sh = s <= 4u ? (uint8_t)s : 0;
    switch (mode) {
        case 0x4: o->a = ib_reg(cpu, MEM_ABASE(w1)); return true;
        case 0x5: *len = 8; o->k = ip + 8u + w2; return true;
        case 0x7: o->a = ib_reg(cpu, MEM_ABASE(w1)); o->b = ib_reg(cpu, MEM_REG3(w1)); o->sh = sh; return true;
        case 0xC: *len = 8; o->k = w2; return true;
        case 0xD: *len = 8; o->k = w2; o->a = ib_reg(cpu, MEM_ABASE(w1)); return true;
        case 0xE: *len = 8; o->k = w2; o->b = ib_reg(cpu, MEM_REG3(w1)); o->sh = sh; return true;
        case 0xF: *len = 8; o->k = w2; o->a = ib_reg(cpu, MEM_ABASE(w1)); o->b = ib_reg(cpu, MEM_REG3(w1)); o->sh = sh; return true;
        default:  return false;
    }
}

/* Decode one instruction into o. Returns its length, 0 when it is not one a
 * block takes; *end is set for a branch, the block's last op. */
static inline uint32_t ib_decode(i960_cpu_t *cpu, ib_op_t *o, uint32_t ip, uint32_t w1, uint32_t w2, bool *end) {
    uint32_t len = 4;
    o->ip  = ip;
    o->cyc = (uint16_t)i960_cycle_cost(w1);
    o->d = &s_ib_sink; o->a = o->b = &s_ib_lit[0]; o->k = 0; o->sh = 0;
    switch (w1 >> 28) {
    case 0x0: case 0x1: {
        uint32_t op = CTRL_OPCODE(w1);
        int32_t disp = (int32_t)(w1 & 0x00FFFFFCu);
        if (disp & 0x00800000) disp |= (int32_t)0xFF000000;
        o->k = ip + (uint32_t)disp;
        if (op == 0x08) { o->kind = IB_B; return 4; }
        if (op >= 0x10 && op <= 0x17) { o->kind = IB_BCC; o->sh = (uint8_t)(op & 7u); return 4; }
        return 0;
    }
    case 0x2: case 0x3: {
        uint32_t op = COBR_OPCODE(w1);
        int32_t disp = (int32_t)(w1 & 0x00001FFCu);
        if (disp & 0x00001000) disp |= (int32_t)0xFFFFE000;
        o->k = ip + (uint32_t)disp;
        o->a = ib_src(cpu, COBR_SRC1(w1), COBR_M1(w1));
        o->b = ib_reg(cpu, COBR_SRC2(w1));
        o->sh = (uint8_t)(op & 7u);
        if (op == 0x30) { o->kind = IB_BBC; return 4; }
        if (op == 0x37) { o->kind = IB_BBS; return 4; }
        if (op >= 0x31 && op <= 0x36) { o->kind = IB_CMPOB; return 4; }
        if (op >= 0x39 && op <= 0x3e) { o->kind = IB_CMPIB; return 4; }
        return 0;
    }
    case 0x5: case 0x6: case 0x7: {
        o->a = ib_src(cpu, REG_SRC1(w1), REG_M1(w1));
        o->b = ib_src(cpu, REG_SRC2(w1), REG_M2(w1));
        o->d = ib_reg(cpu, REG_DST(w1));
        switch (REG_OPCODE(w1)) {
            case 0x5cc: o->kind = IB_MOV;     return 4;
            case 0x590: case 0x591: o->kind = IB_ADD; return 4;
            case 0x592: case 0x593: o->kind = IB_SUB; return 4;
            case 0x581: o->kind = IB_AND;     return 4;
            case 0x587: o->kind = IB_OR;      return 4;
            case 0x586: o->kind = IB_XOR;     return 4;
            case 0x58a: o->kind = IB_NOT;     return 4;
            case 0x582: o->kind = IB_ANDNOT;  return 4;
            case 0x584: o->kind = IB_NOTAND;  return 4;
            case 0x598: o->kind = IB_SHRO;    return 4;
            case 0x59c: case 0x59e: o->kind = IB_SHLO; return 4;
            case 0x59b: o->kind = IB_SHRI;    return 4;
            case 0x701: o->kind = IB_MULO;    return 4;
            case 0x583: o->kind = IB_SETBIT;  return 4;
            case 0x58c: o->kind = IB_CLRBIT;  return 4;
            case 0x580: o->kind = IB_NOTBIT;  return 4;
            case 0x5a0: o->kind = IB_CMPO;    return 4;
            case 0x5a1: o->kind = IB_CMPI;    return 4;
            case 0x5a4: o->kind = IB_CMPINCO; return 4;
            case 0x5a5: o->kind = IB_CMPINCI; return 4;
            case 0x5a6: o->kind = IB_CMPDECO; return 4;
            case 0x5a7: o->kind = IB_CMPDECI; return 4;
            default:    return 0;
        }
    }
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: {
        uint32_t op = w1 >> 24, dst = MEM_SRCDST(w1);
        switch (op) {
            case 0x8C: o->kind = IB_LDA;  break;
            case 0x90: o->kind = IB_LD;   break;
            case 0x80: o->kind = IB_LDOB; break;
            case 0x88: o->kind = IB_LDOS; break;
            case 0xC0: o->kind = IB_LDIB; break;
            case 0xC8: o->kind = IB_LDIS; break;
            case 0x92: o->kind = IB_ST;   break;
            case 0x82: o->kind = IB_STOB; break;
            case 0x8A: o->kind = IB_STOS; break;
            case 0xC2: o->kind = IB_STIB; break;
            case 0xCA: o->kind = IB_STIS; break;
            case 0x98: case 0xA0: case 0xB0: o->kind = IB_LDN; break;
            case 0x9A: case 0xA2: case 0xB2: o->kind = IB_STN; break;
            default:   return 0;
        }
        uint8_t kind = o->kind;
        if (!ib_mem_ea(cpu, o, ip, w1, w2, &len)) return 0;
        if (kind == IB_LDN || kind == IB_STN) {
            /* the count rides in d's place: the words go through reg_read/reg_write */
            o->d = (uint32_t *)(uintptr_t)(op == 0x98 || op == 0x9A ? 2u : op == 0xA0 || op == 0xA2 ? 3u : 4u);
            o->cyc |= (uint16_t)(dst << 11);   /* first register, above any cost a block takes */
        } else {
            o->d = ib_reg(cpu, dst);
        }
        return len;
    }
    default:
        return 0;
    }
}

/* Build the block at ip. An empty one (n = 0) says "step it". */
static inline void ib_build(i960_cpu_t *cpu, memory_bus_t *bus, ib_block_t *b, uint32_t ip) {
    g_ib.builds++;
    if (s_ib_used > IB_POOL - IB_MAX) ib_flush();  /* the blocks a pool's worth back go */
#if I960_JIT
    if (s_ibj_used + IBJ_ROOM > IB_JIT_BYTES) ib_flush();
#endif
    b->ip = ip; b->n = 0; b->cyc = 0; b->op = &s_ib_pool[s_ib_used];
#ifdef IB_WHY
    b->t = b->r = 0;
#endif
    for (;;) {
        uint32_t k = (ip >> 2) & 0xFFFFu;
        if (s_hle_filter[k >> 3] & (1u << (k & 7u))) break;          /* a hook's address */
        const uint8_t *p = MEM_PAGE(bus->rd_page, ip);
        if (!p || MEM_PAGE(bus->wr_page, ip) || (ip & 0xFFFFu) > 0xFFF8u) break;  /* ROM only */
        p += ip & 0xFFFFu;
        uint32_t w1 = mem_le32(p), w2 = mem_le32(p + 4);
        bool end = false;
        ib_op_t *o = &b->op[b->n];
        uint32_t len = ib_decode(cpu, o, ip, w1, w2, &end);
        if (!len) break;
        b->cyc += (uint16_t)(o->cyc & 0x7FFu);
        b->n++;
        ip = o->kind == IB_B ? o->k : ip + len;   /* a block follows a `b` */
        if (end || b->n == IB_MAX) break;
    }
    b->next = ip;
    s_ib_used += b->n;
#if I960_JIT
#ifdef _arch_dreamcast
    uint64_t t0 = timer_us_gettime64();
    ib_jit_compile(cpu, b);
    g_ibj.us_compile += timer_us_gettime64() - t0;
#else
    ib_jit_compile(cpu, b);
#endif
#endif
}

static inline const ib_block_t *ib_lookup(i960_cpu_t *cpu, memory_bus_t *bus, uint32_t ip) {
    if (M2_UNLIKELY(s_ib_profile != g_active_profile || !s_ib_valid)) {
        ib_flush();
        s_ib_profile = g_active_profile;
    }
    uint32_t x = (ip >> 2) & (IB_ENTRIES - 1u);
    ib_block_t *b = &s_ib[x];
    if (M2_LIKELY(b->ip == ip)) { s_ib_miss[x] = 0; return b; }
    /* A frame's code is more than the table holds, and most of it runs once a
     * frame: replacing a block at every miss rebuilt the hot ones again and
     * again. Code that only passes by is stepped; a block goes to code that
     * comes back to the slot IB_MISSES times before its holder does. */
    if (b->ip != 1 && ++s_ib_miss[x] < IB_MISSES) return &s_ib_none;
    s_ib_miss[x] = 0;
    ib_build(cpu, bus, b, ip);
    return b;
}

/* Run a block (its preconditions met: emu_slice_body). Returns the
 * instructions it ran, all of them unless an MMIO access moved something the
 * run loop has to see; the IP, cycles and bus->cpu_ip are as the interpreter
 * leaves them after the last. The board's clock (pending) is the caller's,
 * as after any step. */
static inline uint32_t ib_run(i960_cpu_t *cpu, memory_bus_t *bus, const ib_block_t *blk, uint32_t attn) {
#if I960_JIT
    if (blk->jit && s_ibj_on) return ib_jit_run(cpu, bus, blk, attn);
#endif
    const uint64_t base = cpu->cycles;
    const irqt_count_t h0 = g_irqt.horizon;
    uint32_t c = 0;                       /* cycles charged, this op's included */
    uint32_t next = blk->next;
    const ib_op_t *o = blk->op, *const end = o + blk->n;
    uint32_t ac = cpu->sfr.ac;
/* The interpreter's state, for an access that leaves plain memory. */
#define IB_SYNC() do { cpu->sfr.ip = o->ip; bus->cpu_ip = o->ip; cpu->sfr.ac = ac;              \
        cpu->cycles = base + (c - (o->cyc & 0x7FFu));                                            \
        g_irqt.pending += (irqt_count_t)(uint32_t)(cpu->cycles - s_timer_cycles_seen);           \
        s_timer_cycles_seen = cpu->cycles; cpu->cycles = base + c; slow = true; } while (0)
#define IB_EA()   (o->k + *o->a + (*o->b << o->sh))
#define IB_RD(sz)  (M2_UNLIKELY(!MEM_PAGE(bus->rd_page, ea) || (ea & 0xFFFFu) > 0x10000u - (sz)))
#define IB_WR(sz)  (M2_UNLIKELY(!MEM_PAGE(bus->wr_page, ea) || (ea & 0xFFFFu) > 0x10000u - (sz)))
#define IB_CC(v)  (ac = (ac & ~AC_CC_MASK) | ((v) & AC_CC_MASK))
    for (; o < end; o++) {
        bool slow = false;
        c += o->cyc & 0x7FFu;
        uint32_t s1 = *o->a, s2 = *o->b;
        switch (o->kind) {
        case IB_MOV:     *o->d = s1; continue;
        case IB_ADD:     *o->d = s2 + s1; continue;
        case IB_SUB:     *o->d = s2 - s1; continue;
        case IB_AND:     *o->d = s1 & s2; continue;
        case IB_OR:      *o->d = s1 | s2; continue;
        case IB_XOR:     *o->d = s1 ^ s2; continue;
        case IB_NOT:     *o->d = ~s1; continue;
        case IB_ANDNOT:  *o->d = ~s1 & s2; continue;
        case IB_NOTAND:  *o->d = s1 & ~s2; continue;
        case IB_SHRO:    *o->d = s1 < 32u ? s2 >> s1 : 0; continue;
        case IB_SHLO:    *o->d = s1 < 32u ? s2 << s1 : 0; continue;
        case IB_SHRI:    *o->d = s1 < 32u ? (uint32_t)((int32_t)s2 >> s1) : ((int32_t)s2 < 0 ? 0xFFFFFFFFu : 0); continue;
        case IB_MULO:    *o->d = s2 * s1; continue;
        case IB_SETBIT:  *o->d = s2 | (1u << (s1 & 31u)); continue;
        case IB_CLRBIT:  *o->d = s2 & ~(1u << (s1 & 31u)); continue;
        case IB_NOTBIT:  *o->d = s2 ^ (1u << (s1 & 31u)); continue;
        case IB_CMPO:    IB_CC(i960_cmp_cc_o(s1, s2)); continue;
        case IB_CMPI:    IB_CC(i960_cmp_cc_i((int32_t)s1, (int32_t)s2)); continue;
        case IB_CMPINCO: IB_CC(i960_cmp_cc_o(s1, s2)); *o->d = s2 + 1u; continue;
        case IB_CMPINCI: IB_CC(i960_cmp_cc_i((int32_t)s1, (int32_t)s2)); *o->d = s2 + 1u; continue;
        case IB_CMPDECO: IB_CC(i960_cmp_cc_o(s1, s2)); *o->d = s2 - 1u; continue;
        case IB_CMPDECI: IB_CC(i960_cmp_cc_i((int32_t)s1, (int32_t)s2)); *o->d = s2 - 1u; continue;

        case IB_LDA:  *o->d = o->k + s1 + (s2 << o->sh); continue;
        case IB_LD:   { uint32_t ea = o->k + s1 + (s2 << o->sh); if (IB_RD(4)) IB_SYNC(); *o->d = mem_read32(bus, ea); break; }
        case IB_LDOB: { uint32_t ea = o->k + s1 + (s2 << o->sh); if (IB_RD(1)) IB_SYNC(); *o->d = mem_read8(bus, ea); break; }
        case IB_LDOS: { uint32_t ea = o->k + s1 + (s2 << o->sh); if (IB_RD(2)) IB_SYNC(); *o->d = mem_read16(bus, ea); break; }
        case IB_LDIB: { uint32_t ea = o->k + s1 + (s2 << o->sh); if (IB_RD(1)) IB_SYNC(); *o->d = (uint32_t)(int32_t)(int8_t)mem_read8(bus, ea); break; }
        case IB_LDIS: { uint32_t ea = o->k + s1 + (s2 << o->sh); if (IB_RD(2)) IB_SYNC(); *o->d = (uint32_t)(int32_t)(int16_t)mem_read16(bus, ea); break; }
        case IB_ST:   { uint32_t ea = o->k + s1 + (s2 << o->sh); if (IB_WR(4)) IB_SYNC(); bus->cpu_ip = o->ip; g_last_store_ip = o->ip; mem_write32(bus, ea, *o->d); break; }
        case IB_STOB: { uint32_t ea = o->k + s1 + (s2 << o->sh); if (IB_WR(1)) IB_SYNC(); bus->cpu_ip = o->ip; g_last_store_ip = o->ip; mem_write8(bus, ea, (uint8_t)*o->d); break; }
        case IB_STOS: { uint32_t ea = o->k + s1 + (s2 << o->sh); if (IB_WR(2)) IB_SYNC(); bus->cpu_ip = o->ip; g_last_store_ip = o->ip; mem_write16(bus, ea, (uint16_t)*o->d); break; }
        case IB_STIB: { uint32_t ea = o->k + s1 + (s2 << o->sh); if (IB_WR(1)) IB_SYNC(); bus->cpu_ip = o->ip; mem_write8(bus, ea, (uint8_t)*o->d); break; }
        case IB_STIS: { uint32_t ea = o->k + s1 + (s2 << o->sh); if (IB_WR(2)) IB_SYNC(); bus->cpu_ip = o->ip; mem_write16(bus, ea, (uint16_t)*o->d); break; }
        /* Multi-word: the interpreter's loop, always from its state (a burst
         * device or MMIO decides the stride). */
        case IB_LDN: {
            uint32_t a = o->k + s1 + (s2 << o->sh), n = (uint32_t)(uintptr_t)o->d, r = (uint32_t)o->cyc >> 11;
            IB_SYNC();
            for (uint32_t k = 0; k < n; k++) { reg_write(cpu, (int)(r + k), mem_read32(bus, a)); a += mem_burst_step(bus, a); }
            break;
        }
        case IB_STN: {
            uint32_t a = o->k + s1 + (s2 << o->sh), n = (uint32_t)(uintptr_t)o->d, r = (uint32_t)o->cyc >> 11;
            IB_SYNC();
            g_last_store_ip = o->ip;
            for (uint32_t k = 0; k < n; k++) { mem_write32(bus, a, reg_read(cpu, (int)(r + k))); a += mem_burst_step(bus, a); }
            break;
        }

        /* A branch taken leaves the block; one not taken goes on in it, and a
         * `b` was followed when the block was built. */
        case IB_B:     continue;
        case IB_BCC:   if (i960_cond(ac & AC_CC_MASK, o->sh)) goto taken; continue;
        case IB_BBC:   if (!(s2 & (1u << (s1 & 31u)))) { IB_CC(CC_E); goto taken; } IB_CC(CC_NO); continue;
        case IB_BBS:   if (s2 & (1u << (s1 & 31u)))    { IB_CC(CC_E); goto taken; } IB_CC(CC_NO); continue;
        case IB_CMPOB: IB_CC(i960_cmp_cc_o(s1, s2)); if (i960_cond(ac & AC_CC_MASK, o->sh)) goto taken; continue;
        case IB_CMPIB: IB_CC(i960_cmp_cc_i((int32_t)s1, (int32_t)s2)); if (i960_cond(ac & AC_CC_MASK, o->sh)) goto taken; continue;
        default: continue;
        }
        /* A memory op. One that left plain memory may have moved what the run
         * loop watches: stop after it, as the interpreter's next check would. */
        if (M2_UNLIKELY(slow)) {
            ac = cpu->sfr.ac;   /* an MMIO handler that looks at the CPU leaves it alone, but */
            if (g_emu_attn != attn || (g_irqt.intreq & g_irqt.intena & 0x03FFu) || g_irqt.horizon != h0) {
                o++;
                if (o < end) next = o->ip;
                goto done;
            }
        }
    }
    goto done;
taken:
    next = o->k;
    o++;
done:
    cpu->sfr.ac = ac;
    cpu->cycles = base + c;
    cpu->sfr.ip = next;
    bus->cpu_ip = o[-1].ip;
#undef IB_SYNC
#undef IB_EA
#undef IB_RD
#undef IB_WR
#undef IB_CC
    return (uint32_t)(o - blk->op);
}

#endif /* I960_BLOCKS */
#endif /* I960_BLOCKS_H */
