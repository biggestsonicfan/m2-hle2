#ifndef I960_AOT_H
#define I960_AOT_H
/* The i960's program ROM compiled ahead of time to C (Pinboard #394).
 *
 * tools/i960_aot.py reads the program ROM and a map of the code a game runs
 * (det_digest --aot-map) and writes I960_AOT_GEN, a header of C functions, one
 * per 2^AOT_SHIFT bytes of code. Each is a switch over the basic blocks that
 * start in it and then the blocks themselves, in address order. The common
 * instructions are written out as C with their operands decoded, as the
 * interpreter runs them; the rest call i960_exec_word (aot_exec, out of line).
 * The board cannot tell: det_digest --cpu with and without it must be
 * identical.
 *
 * What makes running it whole the same as stepping it is the block runner's
 * rule (i960_blocks.h), held per basic block: a block runs only if its last
 * cycle ends before the timers' horizon and its last instruction inside the
 * slice, and an access that leaves plain memory (or an instruction that talks
 * to the board: synmov, modpc, ...) first brings the interpreter's state up to
 * date, and stops the run after it if the attention word, the interrupt lines
 * or the horizon moved. A hook's address is never compiled: a block ends
 * before it and the run loop steps it. The run checks at each slice that the
 * profile's hooks are among the generator's and that the ROM is the one it
 * compiled; otherwise the AOT is off. The run loop asks only at the start
 * of a block (aot_lead). */

#if I960_AOT

enum { AOT_STOP = 0, AOT_GO = 1 };

typedef struct {
    uint32_t     ip;      /* where the run is, or stopped */
    int32_t      rc;      /* cycles left before the horizon */
    uint32_t     rn;      /* instructions left in the slice */
    uint32_t     attn;    /* g_emu_attn at entry */
    irqt_count_t h0;      /* g_irqt.horizon at entry */
    uint64_t     base;    /* cpu->cycles at entry */
    int32_t      c0;      /* rc at entry */
    int          halt;
} aot_state_t;

typedef int (*aot_chunk_fn)(i960_cpu_t *cpu, memory_bus_t *bus, aot_state_t *s);

/* An instruction the compiled code hands to the interpreter: its words, and
 * kn = (cycles of its block after it << 10) | instructions of the block after
 * it. One table per chunk; the calls stay small, which matters on a 16 MB
 * console (a slow path written out at every load and store made 6 MB). */
typedef struct { uint32_t ip, w1, w2, kn; } aot_op_t;

/* Leave the function: stopped (or on to another chunk) at IP. */
#define AOT_OUT(IP, HOW) do { s->ip = (IP); return (HOW); } while (0)
/* A block's entry: room for its NB instructions and CB cycles? */
#define AOT_LEAD(IP, NB, CB) do {                                                             \
        if (M2_UNLIKELY((int32_t)(CB) > s->rc || (uint32_t)(NB) > s->rn)) AOT_OUT(IP, AOT_STOP); \
        s->rc -= (CB); s->rn -= (NB); } while (0)

/* The interpreter, out of line. Inlined at every instruction it took a
 * compiler over 6 GB. One that stops the CPU (a frame stack over- or
 * underflow) is not counted, as the run loop does not count it: 1, stop. */
static __attribute__((noinline)) int aot_x(i960_cpu_t *cpu, memory_bus_t *bus, aot_state_t *s,
                                           const aot_op_t *d) {
    if (M2_LIKELY(!i960_exec_word(cpu, bus, d->ip, d->w1, d->w2))) return 0;
    s->rn += (d->kn & 1023u) + 1u; s->rc += (int32_t)(d->kn >> 10); s->halt = 1; s->ip = d->ip;
    return 1;
}
/* The same with the interpreter's state brought up to date first (IB_SYNC):
 * an access off plain memory, or an instruction that talks to the board.
 * 1: stop after it, as the run loop has to see what it did. */
static __attribute__((noinline)) int aot_slow(i960_cpu_t *cpu, memory_bus_t *bus, aot_state_t *s,
                                              const aot_op_t *d) {
    uint32_t k2 = d->kn >> 10, k1 = k2 + i960_cycle_cost(d->w1);
    cpu->sfr.ip = d->ip; bus->cpu_ip = d->ip;
    cpu->cycles = s->base + (uint32_t)(s->c0 - s->rc) - k1;
    g_irqt.pending += (irqt_count_t)(uint32_t)(cpu->cycles - s_timer_cycles_seen);
    s_timer_cycles_seen = cpu->cycles;
    cpu->cycles += k1 - k2;
    if (aot_x(cpu, bus, s, d)) return 1;
    if (M2_UNLIKELY(g_emu_attn != s->attn || (g_irqt.intreq & g_irqt.intena & 0x03FFu)
                    || g_irqt.horizon != s->h0)) {
        s->rn += d->kn & 1023u; s->rc += (int32_t)k2; s->ip = cpu->sfr.ip;
        return 1;
    }
    return 0;
}
#define AOT_X(I)    do { if (M2_UNLIKELY(aot_x(cpu, bus, s, &K[I]))) return AOT_STOP; } while (0)
#define AOT_SLOW(I) do { if (aot_slow(cpu, bus, s, &K[I])) return AOT_STOP; } while (0)

/* Register i as reg_read indexes it (globals first), and the condition code. */
#define AR(i)    (((uint32_t *)&cpu->globals)[i])
#define AGETCC   (cpu->sfr.ac & AC_CC_MASK)
#define ACC(v)   (cpu->sfr.ac = (cpu->sfr.ac & ~AC_CC_MASK) | ((v) & AC_CC_MASK))
/* bbc / bbs: CC_E when taken, CC_NO when not. */
static inline bool aot_ccb(i960_cpu_t *cpu, bool t) { ACC(t ? CC_E : CC_NO); return t; }
#define AOT_CCB(t) aot_ccb(cpu, (t))
/* A compare-and-branch: the compare's code, then the branch's test on it. */
static inline bool aot_cmpb(i960_cpu_t *cpu, uint32_t cc, uint32_t mask) {
    ACC(cc); return mask ? (cc & mask) != 0 : cc == 0;
}
#define AOT_CMPB(cc, mask) aot_cmpb(cpu, (cc), (mask))

/* Loads and stores straight to a page of plain memory, as mem_read32 /
 * mem_write32 make them when that is what they find; anything else is
 * AOT_SLOW. ea_ is the address and p_ its page. */
#define AOT_PG(tab, sz) ((p_ = MEM_PAGE(bus->tab, ea_)) != NULL && (ea_ & 0xFFFFu) <= 0x10000u - (sz))
/* ldl..ldq / stl..stq: and a region that bursts (mem_burst_step 4). */
#define AOT_PGN(tab, sz) (AOT_PG(tab, sz) && !bus->regions[bus->page[ea_ >> 16] - 1u].no_burst)
#define AOT_L8(o)  (MEM_TALLY(bus->reads, 1), (uint32_t)p_[(ea_ + (o)) & 0xFFFFu])
#define AOT_L16(o) (MEM_TALLY(bus->reads, 1), mem_le16(p_ + ((ea_ + (o)) & 0xFFFFu)))
#define AOT_L32(o) (MEM_TALLY(bus->reads, 1), mem_le32(p_ + ((ea_ + (o)) & 0xFFFFu)))
#if M2HLE_DEV_TOOLS
/* the watchpoints and the display-list taps: the bus's own write */
#define AOT_S8(IP, o, v)  do { bus->cpu_ip = (IP); mem_write8(bus, ea_ + (o), (uint8_t)(v)); } while (0)
#define AOT_S16(IP, o, v) do { bus->cpu_ip = (IP); mem_write16(bus, ea_ + (o), (uint16_t)(v)); } while (0)
#define AOT_S32(IP, o, v) do { bus->cpu_ip = (IP); mem_write32(bus, ea_ + (o), (v)); } while (0)
#else
#define AOT_S8(IP, o, v)  do { MEM_TALLY(bus->writes, 1); g_mem_last_write_ip = (IP); \
                               p_[(ea_ + (o)) & 0xFFFFu] = (uint8_t)(v); } while (0)
#define AOT_S16(IP, o, v) do { MEM_TALLY(bus->writes, 1); g_mem_last_write_ip = (IP); \
                               mem_le16_put(p_ + ((ea_ + (o)) & 0xFFFFu), (v)); } while (0)
#define AOT_S32(IP, o, v) do { MEM_TALLY(bus->writes, 1); g_mem_last_write_ip = (IP); \
                               mem_le32_put(p_ + ((ea_ + (o)) & 0xFFFFu), (v)); } while (0)
#endif

#include I960_AOT_GEN

static bool     s_aot_on;
static uint64_t g_aot_ops;    /* instructions run compiled */
static const game_profile_t *s_aot_prof;
static uint32_t s_aot_tick;

/* Is the compiled code this board's? The profile's hooks must all be ones the
 * generator kept out, and the ROM the code it compiled (all of it when the
 * profile changes, a few words each slice after that). */
static inline bool aot_check(memory_bus_t *bus) {
    const game_profile_t *p = g_active_profile;
    if (!p) return false;
    if (p != s_aot_prof) {
        s_aot_prof = p;
        s_aot_on = false;
        for (size_t i = 0; i < p->hook_count; i++) {
            size_t k = 0;
            while (k < AOT_NHOOKS && s_aot_hooks[k] != p->hooks[i].addr) k++;
            if (k == AOT_NHOOKS) { LOG_WARN("aot: hook 0x%08X not known to the code, off", p->hooks[i].addr); return false; }
        }
        uint32_t h = 2166136261u;
        for (uint32_t a = 0; a < AOT_ROM_BYTES; a += 4) h = (h ^ mem_read32(bus, a)) * 16777619u;
        if (h != AOT_FNV) { LOG_WARN("aot: the ROM is not the one compiled, off"); return false; }
        s_aot_on = true;
        LOG_INFO("aot: on");
    }
    if (s_aot_on) {
        const uint32_t *w = s_aot_samp[s_aot_tick++ & 255u];
        if (mem_read32(bus, w[0]) != w[1]) { LOG_WARN("aot: the ROM changed, off"); s_aot_on = false; }
    }
    return s_aot_on;
}

static inline bool aot_lead(uint32_t ip) {
    return ip < (AOT_NCHUNKS << AOT_SHIFT) && (s_aot_lead[ip >> 5] >> ((ip >> 2) & 7u) & 1u);
}

/* Run compiled code from cpu->sfr.ip: at most nmax instructions, all ending
 * before the timers' horizon. Returns how many ran (0: none here); *halt when
 * the instruction after them stopped the CPU. Called where ib_run is: no
 * interrupt pending, nothing the slow path watches. */
static inline uint32_t aot_run(i960_cpu_t *cpu, memory_bus_t *bus, uint32_t nmax, uint32_t attn, int *halt) {
    int64_t room = (int64_t)g_irqt.horizon - (int64_t)g_irqt.pending - 1
                 - (int64_t)(uint32_t)(cpu->cycles - s_timer_cycles_seen);
    if (room <= 0) return 0;
    aot_state_t s;
    s.ip = cpu->sfr.ip; s.rn = nmax; s.attn = attn; s.h0 = g_irqt.horizon;
    s.base = cpu->cycles; s.halt = 0;
    s.rc = s.c0 = room > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)room;
    for (;;) {
        uint32_t c = s.ip >> AOT_SHIFT;
        if (c >= AOT_NCHUNKS || !s_aot_chunk[c]) break;
        if (s_aot_chunk[c](cpu, bus, &s) != AOT_GO) break;
    }
    uint32_t n = nmax - s.rn;
    if (n || s.halt) {
        cpu->sfr.ip = s.ip;
        cpu->cycles = s.base + (uint32_t)(s.c0 - s.rc);
        bus->cpu_ip = s.ip;
    }
    *halt = s.halt;
    return n;
}

#endif /* I960_AOT */
#endif /* I960_AOT_H */
