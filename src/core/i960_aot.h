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
    int          cop_ok;  /* the COP's FIFO is the plain callbacks: its stores queue */
    mem_region_t *geo_r, *geop_r;   /* GEO and GEO_PROGRAM, when they are the plain callbacks */
} aot_state_t;

typedef int (*aot_chunk_fn)(i960_cpu_t *cpu, memory_bus_t *bus, aot_state_t *s);

/* An instruction the compiled code hands to the interpreter: its words, and
 * kn = (cycles of its block after it << 10) | instructions of the block after
 * it. One table per chunk; the calls stay small, which matters on a 16 MB
 * console (a slow path written out at every load and store made 6 MB). */
typedef struct { uint32_t ip, w1, w2, kn; } aot_op_t;

/* Leave the function: stopped (or on to another chunk) at IP. */
#define AOT_OUT(IP, HOW) do { AOT_CC_PUT(); s->ip = (IP); return (HOW); } while (0)
/* The condition code lives in the chunk's cc_ while the compiled code runs,
 * so a compare whose code nothing reads costs nothing past its branch: it goes
 * back to ac when the code leaves and around every helper (the interpreter may
 * read or set it), and comes back from ac after one. */
#define AOT_CC_PUT() (cpu->sfr.ac = (cpu->sfr.ac & ~AC_CC_MASK) | cc_)
#define AOT_CC_GET() (cc_ = cpu->sfr.ac & AC_CC_MASK)
/* A helper's call between the two: stopped, the code is left as it is. */
#define AOT_HELP(call) do { AOT_CC_PUT(); if (call) return AOT_STOP; AOT_CC_GET(); } while (0)
/* A block's entry: room for its NB instructions and CB cycles? */
#define AOT_LEAD(IP, NB, CB) do {                                                             \
        if (M2_UNLIKELY((int32_t)(CB) > s->rc || (uint32_t)(NB) > s->rn)) AOT_OUT(IP, AOT_STOP); \
        s->rc -= (CB); s->rn -= (NB); } while (0)

/* The COP's FIFO stores, queued by the compiled code and handed to cop_write
 * before anything else leaves it: every helper below empties the queue
 * first, and so does aot_run before it returns. The HLE reads nothing of the
 * i960's but its words, and only an answer read back (a load, through
 * aot_io_x) or the 3D at the frame's end sees what it did, so the words land
 * in the same order against everything that can tell. ~3000 a fight frame,
 * where a call each with its checks was ~7% of the Dreamcast's frame. */
#define AOT_COPQ 64u
static uint32_t g_aot_copq[AOT_COPQ];
static uint32_t g_aot_copq_n;
static __attribute__((noinline)) void aot_copq_flush(memory_bus_t *bus) {
    uint32_t n = g_aot_copq_n;
    g_aot_copq_n = 0;
    MEM_TALLY(bus->writes, n);
    (void)bus;
    cop_write_n(g_aot_copq, n);
}
#define AOT_COPQ_FLUSH() do { if (g_aot_copq_n) aot_copq_flush(bus); } while (0)

/* The interpreter, out of line. Inlined at every instruction it took a
 * compiler over 6 GB. One that stops the CPU (a frame stack over- or
 * underflow) is not counted, as the run loop does not count it: 1, stop. */
static __attribute__((noinline)) int aot_x(i960_cpu_t *cpu, memory_bus_t *bus, aot_state_t *s,
                                           const aot_op_t *d) {
    AOT_COPQ_FLUSH();
    if (M2_LIKELY(!i960_exec_word(cpu, bus, d->ip, d->w1, d->w2))) return 0;
    s->rn += (d->kn & 1023u) + 1u; s->rc += (int32_t)(d->kn >> 10); s->halt = 1; s->ip = d->ip;
    return 1;
}
/* aot_x for a call and a ret, the ops it is mostly given: the opcode a
 * constant, so the interpreter's switch folds to the one case. */
static __attribute__((noinline)) int aot_call(i960_cpu_t *cpu, memory_bus_t *bus, aot_state_t *s,
                                              const aot_op_t *d) {
    AOT_COPQ_FLUSH();
    if (M2_LIKELY(!i960_exec_word(cpu, bus, d->ip, 0x09000000u | (d->w1 & 0x00FFFFFFu), 0))) return 0;
    s->rn += (d->kn & 1023u) + 1u; s->rc += (int32_t)(d->kn >> 10); s->halt = 1; s->ip = d->ip;
    return 1;
}
static __attribute__((noinline)) int aot_ret(i960_cpu_t *cpu, memory_bus_t *bus, aot_state_t *s,
                                             const aot_op_t *d) {
    AOT_COPQ_FLUSH();
    if (M2_LIKELY(!i960_exec_word(cpu, bus, d->ip, 0x0A000000u, 0))) return 0;
    s->rn += (d->kn & 1023u) + 1u; s->rc += (int32_t)(d->kn >> 10); s->halt = 1; s->ip = d->ip;
    return 1;
}
/* The same with the interpreter's state brought up to date first (IB_SYNC):
 * an access off plain memory, or an instruction that talks to the board.
 * 1: stop after it, as the run loop has to see what it did. */
static __attribute__((noinline)) int aot_slow(i960_cpu_t *cpu, memory_bus_t *bus, aot_state_t *s,
                                              const aot_op_t *d) {
    AOT_COPQ_FLUSH();
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
/* A load or store of one item off plain memory (an MMIO register, the COP's
 * FIFO): aot_slow without the interpreter's decode. The words say which and
 * to which register; the address is the compiled code's. What the bus sees
 * is what i960_exec_word's MEM case does, in its order. */
static int aot_pg(i960_cpu_t *cpu, memory_bus_t *bus, const aot_op_t *d, uint32_t ea_);
static __attribute__((noinline)) int aot_io_x(i960_cpu_t *cpu, memory_bus_t *bus, aot_state_t *s,
                                              const aot_op_t *d, uint32_t ea, int cop_written) {
    AOT_COPQ_FLUSH();
    if (!cop_written && aot_pg(cpu, bus, d, ea)) return 0;
    uint32_t k2 = d->kn >> 10, k1 = k2 + i960_cycle_cost(d->w1), w1 = d->w1, ip = d->ip;
    uint32_t *r = (uint32_t *)&cpu->globals + ((((w1 >> 19) & 0x1Fu) + 16u) & 31u);
    unsigned mode = (w1 >> 10) & 0xFu;
    cpu->sfr.ip = ip; bus->cpu_ip = ip;
    if (cop_written) goto cop_done;   /* aot_io_st/_ld did it, and attn moved */
    /* The COP's FIFO, a word at a time: most of these calls (~4900 a fight
     * frame). The HLE answers a command when its last word lands and reads
     * neither the cycles nor the timers, and only its unknown-command trap
     * moves attn, so the cycles are left to the next sync and the run goes
     * on. What the bus would do besides is the tallies below. */
    if (ea - COPROGRAM_BASE < COPROGRAM_SIZE && (w1 >> 24 == 0x92 || w1 >> 24 == 0x90)
            && !wp_armed() && !dl_active()) {
        const mem_region_t *g = mem_find_region(bus, ea);
        if (g && w1 >> 24 == 0x92 && g->write_cb == coprogram_write_cb) {
            MEM_TALLY(bus->writes, 1);
            g_mem_last_write_ip = ip; g_last_store_ip = ip;
            cop_write(*r);
            goto cop_done;
        }
        if (g && w1 >> 24 == 0x90 && g->read_cb == coprogram_read_cb) {
            MEM_TALLY(bus->reads, 1);
            *r = cop_read();
            goto cop_done;
        }
    }
    cpu->cycles = s->base + (uint32_t)(s->c0 - s->rc) - k1;
    g_irqt.pending += (irqt_count_t)(uint32_t)(cpu->cycles - s_timer_cycles_seen);
    s_timer_cycles_seen = cpu->cycles;
    cpu->cycles += k1 - k2;
    switch (w1 >> 24) {
    case 0x80: *r = mem_read8(bus, ea); break;
    case 0x88: *r = mem_read16(bus, ea); break;
    case 0x90: *r = mem_read32(bus, ea); break;
    case 0xC0: *r = (uint32_t)(int32_t)(int8_t)mem_read8(bus, ea); break;
    case 0xC8: *r = (uint32_t)(int32_t)(int16_t)mem_read16(bus, ea); break;
    case 0x82: g_last_store_ip = ip; mem_write8(bus, ea, (uint8_t)*r); break;
    case 0x8A: g_last_store_ip = ip; mem_write16(bus, ea, (uint16_t)*r); break;
    case 0x92: g_last_store_ip = ip; mem_write32(bus, ea, *r); break;
    case 0xC2: mem_write8(bus, ea, (uint8_t)*r); break;
    default:   mem_write16(bus, ea, (uint16_t)*r); break;   /* 0xCA */
    }
    if (0) {
cop_done:
        if (M2_LIKELY(g_emu_attn == s->attn)) {
            cpu->sfr.ip = ip + ((mode == 5u || mode >= 0xCu) ? 8u : 4u);
            return 0;
        }
        cpu->cycles = s->base + (uint32_t)(s->c0 - s->rc) - k1;
        g_irqt.pending += (irqt_count_t)(uint32_t)(cpu->cycles - s_timer_cycles_seen);
        s_timer_cycles_seen = cpu->cycles;
        cpu->cycles += k1 - k2;
    }
    /* mem_ea's length: the MEMB modes with a displacement are two words */
    cpu->sfr.ip = ip + ((mode == 5u || mode >= 0xCu) ? 8u : 4u);
    if (M2_UNLIKELY(g_emu_attn != s->attn || (g_irqt.intreq & g_irqt.intena & 0x03FFu)
                    || g_irqt.horizon != s->h0)) {
        s->rn += d->kn & 1023u; s->rc += (int32_t)k2; s->ip = cpu->sfr.ip;
        return 1;
    }
    return 0;
}
static inline int aot_io(i960_cpu_t *cpu, memory_bus_t *bus, aot_state_t *s, const aot_op_t *d, uint32_t ea) {
    return aot_io_x(cpu, bus, s, d, ea, 0);
}
/* An ld of the COP's answers (~1200 a fight frame) or of GEO's registers
 * (its status and pointers). */
static __attribute__((noinline)) int aot_io_ld(i960_cpu_t *cpu, memory_bus_t *bus, aot_state_t *s,
                                               const aot_op_t *d, uint32_t ea) {
    int cop = ea - COPROGRAM_BASE < COPROGRAM_SIZE && s->cop_ok;
    if (cop || (s->geo_r && ea - GEO_BASE <= GEO_SIZE - 4u)) {
        AOT_COPQ_FLUSH();
        uint32_t w1 = d->w1, ip = d->ip;
        cpu->sfr.ip = ip; bus->cpu_ip = ip;
        MEM_TALLY(bus->reads, 1);
        ((uint32_t *)&cpu->globals)[(((w1 >> 19) & 0x1Fu) + 16u) & 31u] = cop ? cop_read() : geo_read_cb(s->geo_r, ea, 4);
        if (M2_UNLIKELY(g_emu_attn != s->attn)) return aot_io_x(cpu, bus, s, d, ea, 1);
        unsigned mode = (w1 >> 10) & 0xFu;
        cpu->sfr.ip = ip + ((mode == 5u || mode >= 0xCu) ? 8u : 4u);
        return 0;
    }
    return aot_io_x(cpu, bus, s, d, ea, 0);
}
/* A `st` off plain memory: aot_io's COP FIFO path without the rest of it (the
 * cycle cost, the decode, the switch), for six in ten of its calls. */
static __attribute__((noinline)) int aot_io_st(i960_cpu_t *cpu, memory_bus_t *bus, aot_state_t *s,
                                               const aot_op_t *d, uint32_t ea, uint32_t v) {
    AOT_COPQ_FLUSH();
    /* GEO_PROGRAM's list words and GEO's function ports: ~500 a frame. The
     * callbacks read neither the cycles nor the timers. */
    mem_region_t *g = NULL;
    if (s->geop_r && ea - GEO_PROGRAM_BASE <= GEO_PROGRAM_SIZE - 4u) g = s->geop_r;
    else if (s->geo_r && ea - GEO_BASE <= GEO_SIZE - 4u) g = s->geo_r;
    if (g) {
        uint32_t ip = d->ip;
        cpu->sfr.ip = ip; bus->cpu_ip = ip;
        MEM_TALLY(bus->writes, 1);
        g_mem_last_write_ip = ip; g_last_store_ip = ip;
        if (g == s->geop_r) geo_program_write_cb(g, ea, v, 4);
        else geo_write_cb(g, ea, v, 4);
        if (M2_UNLIKELY(g_emu_attn != s->attn)) return aot_io_x(cpu, bus, s, d, ea, 1);
        unsigned mode = (d->w1 >> 10) & 0xFu;
        cpu->sfr.ip = ip + ((mode == 5u || mode >= 0xCu) ? 8u : 4u);
        return 0;
    }
    if (ea - COPROGRAM_BASE < COPROGRAM_SIZE && !wp_armed() && !dl_active()) {
        const mem_region_t *g = mem_find_region(bus, ea);
        if (g && g->write_cb == coprogram_write_cb) {
            uint32_t ip = d->ip;
            unsigned mode = (d->w1 >> 10) & 0xFu;
            cpu->sfr.ip = ip; bus->cpu_ip = ip;
            MEM_TALLY(bus->writes, 1);
            g_mem_last_write_ip = ip; g_last_store_ip = ip;
            cop_write(v);
            if (M2_UNLIKELY(g_emu_attn != s->attn)) return aot_io_x(cpu, bus, s, d, ea, 1);
            cpu->sfr.ip = ip + ((mode == 5u || mode >= 0xCu) ? 8u : 4u);
            return 0;
        }
    }
    return aot_io_x(cpu, bus, s, d, ea, 0);
}
/* stl / stt / stq off plain memory to the COP's FIFO or the GEO's function
 * ports: ~1000 a fight frame. Neither bursts, so every word goes to the one
 * address, and neither callback reads the cycles or the timers, so the cycles
 * are left to the next sync as aot_io_st leaves them. */
static __attribute__((noinline)) int aot_io_stn(i960_cpu_t *cpu, memory_bus_t *bus, aot_state_t *s,
                                                const aot_op_t *d, uint32_t ea) {
    AOT_COPQ_FLUSH();
    if (!wp_armed() && !dl_active()) {
        mem_region_t *g = mem_find_region(bus, ea);
        if (g && (g->write_cb == coprogram_write_cb
                  || (g->write_cb == geo_write_cb && ea - GEO_BASE < 0x1000u))) {
            uint32_t ip = d->ip, w1 = d->w1, r0 = (w1 >> 19) & 0x1Fu;
            uint32_t n = (w1 >> 24) == 0x9Au ? 2u : (w1 >> 24) == 0xA2u ? 3u : 4u;
            cpu->sfr.ip = ip; bus->cpu_ip = ip;
            g_last_store_ip = ip;
            for (uint32_t k = 0; k < n; k++) {
                MEM_TALLY(bus->writes, 1);
                g_mem_last_write_ip = ip;
                g->write_cb(g, ea, ((uint32_t *)&cpu->globals)[(r0 + k + 16u) & 31u], 4);
            }
            if (M2_UNLIKELY(g_emu_attn != s->attn)) return aot_io_x(cpu, bus, s, d, ea, 1);
            unsigned mode = (w1 >> 10) & 0xFu;
            cpu->sfr.ip = ip + ((mode == 5u || mode >= 0xCu) ? 8u : 4u);
            return 0;
        }
    }
    if (aot_pg(cpu, bus, d, ea)) return 0;
    return aot_slow(cpu, bus, s, d);
}
/* An ldl..ldq off the RAM: the ROM's tables, or the bus's own way. */
static __attribute__((noinline)) int aot_ldn(i960_cpu_t *cpu, memory_bus_t *bus, aot_state_t *s,
                                             const aot_op_t *d, uint32_t ea) {
    AOT_COPQ_FLUSH();
    if (aot_pg(cpu, bus, d, ea)) return 0;
    return aot_slow(cpu, bus, s, d);
}
/* K[I] is a constant: the test folds, and only a `st` site queues for the
 * COP or calls aot_io_st. */
#define AOT_IO(I)   do {                                                                               \
        if ((K[I].w1 >> 24) == 0x92u) {                                                                \
            uint32_t v_ = AR((((K[I].w1 >> 19) & 0x1Fu) + 16u) & 31u);                                 \
            if (M2_LIKELY(ea_ - COPROGRAM_BASE < COPROGRAM_SIZE && s->cop_ok)) {                        \
                g_aot_copq[g_aot_copq_n++] = v_;                                                       \
                if (M2_UNLIKELY(g_aot_copq_n == AOT_COPQ)) aot_copq_flush(bus);                        \
            } else AOT_HELP(aot_io_st(cpu, bus, s, &K[I], ea_, v_));                                  \
        } else if ((K[I].w1 >> 24) == 0x90u) AOT_HELP(aot_io_ld(cpu, bus, s, &K[I], ea_));                \
        else AOT_HELP(aot_io(cpu, bus, s, &K[I], ea_)); } while (0)
#define AOT_X(I)     AOT_HELP(M2_UNLIKELY(aot_x(cpu, bus, s, &K[I])))
#define AOT_CALL(I)  AOT_HELP(M2_UNLIKELY(aot_call(cpu, bus, s, &K[I])))
#define AOT_RET(I)   AOT_HELP(M2_UNLIKELY(aot_ret(cpu, bus, s, &K[I])))
#define AOT_SLOW(I)  AOT_HELP(aot_slow(cpu, bus, s, &K[I]))
#define AOT_IOSTN(I) AOT_HELP(aot_io_stn(cpu, bus, s, &K[I], ea_))
#define AOT_LDN(I)   AOT_HELP(aot_ldn(cpu, bus, s, &K[I], ea_))

/* Register i as reg_read indexes it (globals first), and the condition code. */
/* r0-r15 sit past the 60 bytes an SH-4 load reaches from one base: a base of
 * their own (gq_, rq_: each chunk's), opaque so the compiler keeps it. */
#define AR(i)    (*((i) < 16 ? &gq_[(i)] : &rq_[(i) - 16]))
#define AOT_REGS uint32_t *gq_ = (uint32_t *)&cpu->globals, *rq_ = gq_ + 16; __asm__("" : "+r"(rq_)); \
                 uint32_t cc_ = cpu->sfr.ac & AC_CC_MASK
#define AGETCC   cc_
#define ACC(v)   (cc_ = (v) & AC_CC_MASK)
/* bbc / bbs: CC_E when taken, CC_NO when not. */
#define AOT_CCB(t) ({ bool t_ = (t); cc_ = t_ ? CC_E : CC_NO; t_; })
/* A compare-and-branch: the compare's code, and the branch's test on the
 * operands themselves (L 4, E 2, G 1; mask 0 is never), so the code folds
 * away where nothing reads it. */
#define AOT_CMPB_O(a, b, mask) ({ uint32_t a_ = (a), b_ = (b); cc_ = aot_cc_o(a_, b_); \
        (((mask) & 4u) && a_ < b_) || (((mask) & 2u) && a_ == b_) || (((mask) & 1u) && a_ > b_); })
#define AOT_CMPB_I(a, b, mask) ({ int32_t a_ = (int32_t)(a), b_ = (int32_t)(b); cc_ = aot_cc_i(a_, b_); \
        (((mask) & 4u) && a_ < b_) || (((mask) & 2u) && a_ == b_) || (((mask) & 1u) && a_ > b_); })
/* i960_cmp_cc_o / _i without branches: L 4, E 2, G 1. */
#define AOT_CC_O(a, b) aot_cc_o((a), (b))
#define AOT_CC_I(a, b) aot_cc_i((a), (b))
static inline uint32_t aot_cc_o(uint32_t a, uint32_t b) { return 1u + (uint32_t)(a == b) + 3u * (uint32_t)(a < b); }
static inline uint32_t aot_cc_i(int32_t a, int32_t b)   { return 1u + (uint32_t)(a == b) + 3u * (uint32_t)(a < b); }

/* Loads and stores straight to a page of plain memory, as mem_read32 /
 * mem_write32 make them when that is what they find; anything else is
 * AOT_SLOW. ea_ is the address and p_ its page. */
#define AOT_PG(tab, sz) ((p_ = MEM_PAGE(bus->tab, ea_)) != NULL && (ea_ & MEM_PAGE_OFF) <= MEM_PAGE_OFF + 1u - (sz))
/* ldl..ldq / stl..stq: and a region that bursts (mem_burst_step 4). */
#define AOT_PGN(tab, sz) (AOT_PG(tab, sz) && !bus->regions[bus->page[ea_ >> 16] - 1u].no_burst)
#define AOT_L8(o)  (MEM_TALLY(bus->reads, 1), (uint32_t)p_[(ea_ + (o)) & MEM_PAGE_OFF])
#define AOT_L16(o) (MEM_TALLY(bus->reads, 1), mem_le16(p_ + ((ea_ + (o)) & MEM_PAGE_OFF)))
#define AOT_L32(o) (MEM_TALLY(bus->reads, 1), mem_le32(p_ + ((ea_ + (o)) & MEM_PAGE_OFF)))
#if M2HLE_DEV_TOOLS
/* the watchpoints and the display-list taps: the bus's own write */
#define AOT_S8(IP, o, v)  do { bus->cpu_ip = (IP); mem_write8(bus, ea_ + (o), (uint8_t)(v)); } while (0)
#define AOT_S16(IP, o, v) do { bus->cpu_ip = (IP); mem_write16(bus, ea_ + (o), (uint16_t)(v)); } while (0)
#define AOT_S32(IP, o, v) do { bus->cpu_ip = (IP); mem_write32(bus, ea_ + (o), (v)); } while (0)
#else
#define AOT_S8(IP, o, v)  do { MEM_TALLY(bus->writes, 1); g_mem_last_write_ip = (IP); \
                               p_[(ea_ + (o)) & MEM_PAGE_OFF] = (uint8_t)(v); } while (0)
#define AOT_S16(IP, o, v) do { MEM_TALLY(bus->writes, 1); g_mem_last_write_ip = (IP); \
                               mem_le16_put(p_ + ((ea_ + (o)) & MEM_PAGE_OFF), (v)); } while (0)
#define AOT_S32(IP, o, v) do { MEM_TALLY(bus->writes, 1); g_mem_last_write_ip = (IP); \
                               mem_le32_put(p_ + ((ea_ + (o)) & MEM_PAGE_OFF), (v)); } while (0)
#endif

/* What the compiled code does inline: the RAM (STF's ~45% of them; aot_check
 * sees it is plain), in one test of the first and last item's offsets that
 * also wants them aligned, so the access is a plain word. Anything else
 * (the ROM, the COP's FIFO) goes to a helper, aot_pg's page tables first. */
#define AOT_RAM(sz) (!(((ea_ - RAM_BASE) | (ea_ + ((sz) > 4u ? (sz) - 4u : 0u) - RAM_BASE))           \
                       & ((uint32_t)~(RAM_SIZE - 1u) | ((sz) >= 4u ? 3u : (sz) - 1u))))
#define AOT_RQ(o)   (bus->ram + (ea_ - RAM_BASE) + (o))
#if MEM_LE_DIRECT
#define AOT_RL16(o) (MEM_TALLY(bus->reads, 1), (uint32_t)*(const mem_u16_alias_t *)AOT_RQ(o))
#define AOT_RL32(o) (MEM_TALLY(bus->reads, 1), (uint32_t)*(const mem_u32_alias_t *)AOT_RQ(o))
#else
#define AOT_RL16(o) (MEM_TALLY(bus->reads, 1), mem_le16(AOT_RQ(o)))
#define AOT_RL32(o) (MEM_TALLY(bus->reads, 1), mem_le32(AOT_RQ(o)))
#endif
#define AOT_RL8(o)  (MEM_TALLY(bus->reads, 1), (uint32_t)*AOT_RQ(o))
/* A load off the program ROM or MAIN_DATA, plain pages from s_aot_rom_n /
 * s_aot_md_n bytes on (aot_check; 0 when they are not): nothing the COP's
 * queue does reaches either, so no flush. q_ is the item's host address. */
static const uint8_t *s_aot_rom, *s_aot_md;
static uint32_t s_aot_rom_n, s_aot_md_n;
#define AOT_ROMD(sz) (!(ea_ & ((sz) >= 4u ? 3u : (sz) - 1u))                                       \
                      && ((ea_ < s_aot_rom_n && s_aot_rom_n - ea_ >= (sz)) ? (q_ = s_aot_rom + ea_, 1)  \
                       : (ea_ - MAIN_DATA_BASE < s_aot_md_n && s_aot_md_n - (ea_ - MAIN_DATA_BASE) >= (sz)) \
                          ? (q_ = s_aot_md + (ea_ - MAIN_DATA_BASE), 1) : 0))
#if MEM_LE_DIRECT
#define AOT_OL16(o) (MEM_TALLY(bus->reads, 1), (uint32_t)*(const mem_u16_alias_t *)(q_ + (o)))
#define AOT_OL32(o) (MEM_TALLY(bus->reads, 1), (uint32_t)*(const mem_u32_alias_t *)(q_ + (o)))
#else
#define AOT_OL16(o) (MEM_TALLY(bus->reads, 1), mem_le16(q_ + (o)))
#define AOT_OL32(o) (MEM_TALLY(bus->reads, 1), mem_le32(q_ + (o)))
#endif
#define AOT_OL8(o)  (MEM_TALLY(bus->reads, 1), (uint32_t)q_[(o)])
#if M2HLE_DEV_TOOLS
#define AOT_RS8  AOT_S8
#define AOT_RS16 AOT_S16
#define AOT_RS32 AOT_S32
#else
#define AOT_RS8(IP, o, v)  do { MEM_TALLY(bus->writes, 1); g_mem_last_write_ip = (IP); *AOT_RQ(o) = (uint8_t)(v); } while (0)
#if MEM_LE_DIRECT
#define AOT_RS16(IP, o, v) do { MEM_TALLY(bus->writes, 1); g_mem_last_write_ip = (IP); \
                                *(mem_u16_alias_t *)AOT_RQ(o) = (uint16_t)(v); } while (0)
#define AOT_RS32(IP, o, v) do { MEM_TALLY(bus->writes, 1); g_mem_last_write_ip = (IP); \
                                *(mem_u32_alias_t *)AOT_RQ(o) = (v); } while (0)
#else
#define AOT_RS16(IP, o, v) do { MEM_TALLY(bus->writes, 1); g_mem_last_write_ip = (IP); mem_le16_put(AOT_RQ(o), (v)); } while (0)
#define AOT_RS32(IP, o, v) do { MEM_TALLY(bus->writes, 1); g_mem_last_write_ip = (IP); mem_le32_put(AOT_RQ(o), (v)); } while (0)
#endif
#endif

/* A load or store the compiled code left to a helper, off a page of plain
 * memory as mem_read32 / mem_write32 find it: what the code once did inline,
 * so the same tallies and store marks. 0: not such a page. */
static int aot_pg(i960_cpu_t *cpu, memory_bus_t *bus, const aot_op_t *d, uint32_t ea_) {
    uint32_t w1 = d->w1, op = w1 >> 24, r0 = (w1 >> 19) & 0x1Fu, ip = d->ip;
    uint32_t *gq_ = (uint32_t *)&cpu->globals;
    uint8_t *p_;
#define PG_R(k) gq_[(r0 + (k) + 16u) & 31u]
    switch (op) {
    case 0x80: if (!AOT_PG(rd_page, 1u)) return 0; PG_R(0) = AOT_L8(0); return 1;
    case 0xC0: if (!AOT_PG(rd_page, 1u)) return 0; PG_R(0) = (uint32_t)(int32_t)(int8_t)AOT_L8(0); return 1;
    case 0x88: if (!AOT_PG(rd_page, 2u)) return 0; PG_R(0) = AOT_L16(0); return 1;
    case 0xC8: if (!AOT_PG(rd_page, 2u)) return 0; PG_R(0) = (uint32_t)(int32_t)(int16_t)AOT_L16(0); return 1;
    case 0x90: if (!AOT_PG(rd_page, 4u)) return 0; PG_R(0) = AOT_L32(0); return 1;
    case 0x82: case 0xC2:
        if (!AOT_PG(wr_page, 1u)) return 0;
        if (op == 0x82) g_last_store_ip = ip;
        AOT_S8(ip, 0, PG_R(0)); return 1;
    case 0x8A: case 0xCA:
        if (!AOT_PG(wr_page, 2u)) return 0;
        if (op == 0x8A) g_last_store_ip = ip;
        AOT_S16(ip, 0, PG_R(0)); return 1;
    case 0x92:
        if (!AOT_PG(wr_page, 4u)) return 0;
        g_last_store_ip = ip; AOT_S32(ip, 0, PG_R(0)); return 1;
    case 0x98: case 0xA0: case 0xB0: {
        uint32_t n = op == 0x98 ? 2u : op == 0xA0 ? 3u : 4u;
        if (r0 + n > 32u || !AOT_PGN(rd_page, 4u * n)) return 0;
        for (uint32_t k = 0; k < n; k++) PG_R(k) = AOT_L32(4u * k);
        return 1;
    }
    case 0x9A: case 0xA2: case 0xB2: {
        uint32_t n = op == 0x9A ? 2u : op == 0xA2 ? 3u : 4u;
        if (r0 + n > 32u || !AOT_PGN(wr_page, 4u * n)) return 0;
        g_last_store_ip = ip;
        for (uint32_t k = 0; k < n; k++) AOT_S32(ip, 4u * k, PG_R(k));
        return 1;
    }
    }
#undef PG_R
    return 0;
}

#include I960_AOT_GEN

static bool     s_aot_on;
static int s_aot_cop_ok;
static mem_region_t *s_aot_geo_r, *s_aot_geop_r;
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
    if (s_aot_on) {   /* the devices aot_run's helpers call straight (per slice: ~2% a run) */
        mem_region_t *g = mem_find_region(bus, COPROGRAM_BASE);
        for (uint32_t pg = RAM_BASE >> MEM_PAGE_SHIFT; pg < (RAM_BASE + RAM_SIZE) >> MEM_PAGE_SHIFT; pg++) {   /* AOT_RAM's */
            uint8_t *at = bus->ram + ((pg << MEM_PAGE_SHIFT) - RAM_BASE);
            if (bus->rd_page[pg] != at || (!M2HLE_DEV_TOOLS && bus->wr_page[pg] != at)) {
                LOG_WARN("aot: the RAM is not plain memory, off"); s_aot_on = false; return false;
            }
        }
        {   /* AOT_ROMD's: how far each is plain pages of the one buffer */
            const mem_region_t *r = mem_find_region(bus, ROM_BASE);
            uint32_t n = 0;
            s_aot_rom = r && r->base == ROM_BASE && !MEM_HOST_PAGED(r->data) ? r->data : NULL;   /* paged: AOT_PG */
            while (s_aot_rom && n < r->size && MEM_PAGE(bus->rd_page, ROM_BASE + n) == s_aot_rom + n) n += MEM_PAGE_OFF + 1u;
            s_aot_rom_n = n < (r ? r->size : 0u) ? n : (r ? r->size : 0u);
            r = mem_find_region(bus, MAIN_DATA_BASE);
            s_aot_md = r && r->base == MAIN_DATA_BASE && !MEM_HOST_PAGED(r->data) ? r->data : NULL;
            n = 0;
            while (s_aot_md && n < r->size && MEM_PAGE(bus->rd_page, MAIN_DATA_BASE + n) == s_aot_md + n) n += MEM_PAGE_OFF + 1u;
            s_aot_md_n = n < (r ? r->size : 0u) ? n : (r ? r->size : 0u);
        }
        s_aot_cop_ok = g && g->write_cb == coprogram_write_cb && g->read_cb == coprogram_read_cb
                    && g == mem_find_region(bus, COPROGRAM_BASE + COPROGRAM_SIZE - 4u);
        g = mem_find_region(bus, GEO_BASE);
        s_aot_geo_r = g && g->base == GEO_BASE && g->size >= GEO_SIZE && g->write_cb == geo_write_cb
                    && g->read_cb == geo_read_cb && g == mem_find_region(bus, GEO_BASE + GEO_SIZE - 4u) ? g : NULL;
        g = mem_find_region(bus, GEO_PROGRAM_BASE);
        s_aot_geop_r = g && g->base == GEO_PROGRAM_BASE && g->size >= GEO_PROGRAM_SIZE
                    && g->write_cb == geo_program_write_cb
                    && g == mem_find_region(bus, GEO_PROGRAM_BASE + GEO_PROGRAM_SIZE - 4u) ? g : NULL;
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
    {   /* the devices' plain callbacks, unless a debug tool watches the bus */
        int plain = !wp_armed() && !dl_active();
        s.cop_ok = plain && s_aot_cop_ok;
        s.geo_r  = plain ? s_aot_geo_r : NULL;
        s.geop_r = plain ? s_aot_geop_r : NULL;
    }
    s.rc = s.c0 = room > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)room;
    for (;;) {
        uint32_t c = s.ip >> AOT_SHIFT;
        if (c >= AOT_NCHUNKS || !s_aot_chunk[c]) break;
        if (s_aot_chunk[c](cpu, bus, &s) != AOT_GO) break;
    }
    AOT_COPQ_FLUSH();
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
