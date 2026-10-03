/* dc_jit_test.h -- the SH-4 block compiler held against ib_run, on the
 * Dreamcast itself (-DIB_JIT_SELFTEST=1; main_dc.c runs it after the board is
 * installed and before the first slice).
 *
 * Random blocks of every kind a block takes, each run twice from the same
 * state: by ib_run and by its compiled code. Everything either leaves behind
 * is compared: the registers and the AC, the IP and cycles, the ops run, the
 * board's clock (an access that leaves plain memory syncs it), the last-store
 * globals and the scratch memory the loads and stores reach. The scratch sits
 * on a page edge of work RAM, so some accesses cross it and take the slow
 * path (IB_SYNC); some are unaligned. Everything is put back afterwards. */
#ifndef DC_JIT_TEST_H
#define DC_JIT_TEST_H

#if I960_JIT && IB_JIT_SELFTEST

static uint32_t jt_rng = 0x2545F491u;
static uint32_t jt_rand(void) { jt_rng ^= jt_rng << 13; jt_rng ^= jt_rng >> 17; jt_rng ^= jt_rng << 5; return jt_rng; }
static uint32_t jt_pick(uint32_t n) { return jt_rand() % n; }

#define JT_IDX   27u   /* g11: an index, 0..15 */
#define JT_SCR   28u   /* g12: the scratch */
#define JT_ROM   29u   /* g13: program ROM */

static uint32_t jt_dst(void) {   /* any register but the three above */
    uint32_t r;
    do r = jt_pick(32); while (r == JT_IDX || r == JT_SCR || r == JT_ROM);
    return r;
}

/* One instruction: w[0], w[1]; returns its length in words. */
static int jt_insn(uint32_t *w) {
    static const uint16_t regops[] = { 0x5cc, 0x590, 0x591, 0x592, 0x593, 0x581, 0x587, 0x586, 0x58a,
        0x582, 0x584, 0x598, 0x59c, 0x59e, 0x59b, 0x701, 0x583, 0x58c, 0x580, 0x5a0, 0x5a1,
        0x5a4, 0x5a5, 0x5a6, 0x5a7 };
    static const uint8_t ldops[] = { 0x90, 0x80, 0x88, 0xC0, 0xC8, 0x8C };
    static const uint8_t stops[] = { 0x92, 0x82, 0x8A, 0xC2, 0xCA };
    static const uint8_t nops[]  = { 0x98, 0xA0, 0xB0, 0x9A, 0xA2, 0xB2 };
    static const uint8_t nreg[]  = { 4, 8, 16, 20 };
    uint32_t c = jt_pick(100);
    if (c < 45) {                                   /* REG */
        uint32_t op = regops[jt_pick(sizeof regops / sizeof regops[0])];
        uint32_t m1 = jt_pick(3) == 0, m2 = jt_pick(4) == 0;
        w[0] = ((op >> 4) << 24) | (jt_dst() << 19) | (jt_pick(32) << 14) | (m2 << 12) | (m1 << 11)
             | ((op & 0xFu) << 7) | jt_pick(32);
        return 1;
    }
    if (c < 60) {                                   /* COBR */
        static const uint8_t ops[] = { 0x30, 0x37, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
                                       0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e };
        uint32_t op = ops[jt_pick(sizeof ops)];
        w[0] = (op << 24) | (jt_pick(32) << 19) | (jt_pick(32) << 14) | ((uint32_t)(jt_pick(2)) << 13)
             | (jt_rand() & 0x1FFCu);
        return 1;
    }
    if (c < 68) {                                   /* CTRL: b, b<cc> */
        uint32_t op = jt_pick(5) == 0 ? 0x08 : 0x10 + jt_pick(8);
        w[0] = (op << 24) | (jt_rand() & 0x00FFFFFCu);
        return 1;
    }
    /* MEM */
    uint32_t op, sd;
    bool rom = false;
    if (c < 84)      { op = ldops[jt_pick(sizeof ldops)]; sd = jt_dst(); rom = jt_pick(4) == 0; }
    else if (c < 96) { op = stops[jt_pick(sizeof stops)]; sd = jt_pick(32); }
    else             { op = nops[jt_pick(sizeof nops)];   sd = nreg[jt_pick(4)]; }
    uint32_t base = rom ? JT_ROM : JT_SCR, off = jt_pick(4) ? jt_pick(0x40) * 4u : jt_pick(0x100);
    switch (jt_pick(5)) {
    case 0:  w[0] = (op << 24) | (sd << 19) | (base << 14) | (1u << 13) | off; return 1;              /* MEMA abase+off */
    case 1:  w[0] = (op << 24) | (sd << 19) | (base << 14) | (0x7u << 10) | (jt_pick(5) << 7) | JT_IDX; return 1;
    case 2:  w[0] = (op << 24) | (sd << 19) | (base << 14) | (0xDu << 10); w[1] = off; return 2;
    case 3:  w[0] = (op << 24) | (sd << 19) | (base << 14) | (0xFu << 10) | (jt_pick(5) << 7) | JT_IDX; w[1] = off; return 2;
    default: w[0] = (op << 24) | (sd << 19) | (jt_pick(32) << 14) | (1u << 13) | off;              /* any base: lda only */
             if (op != 0x8C) w[0] = (w[0] & ~(0x1Fu << 14)) | (base << 14);
             return 1;
    }
}

static ib_op_t    jt_ops[IB_MAX];
static ib_block_t jt_blk;

typedef struct {
    i960_cpu_t   cpu;
    irqt_count_t pending;
    uint64_t     seen;
    uint32_t     last_store, last_write, bus_ip, k;
    uint8_t      mem[0x300];
} jt_state_t;

static jt_state_t jt_s0, jt_a, jt_b;

static void jt_take(jt_state_t *s, i960_cpu_t *cpu, memory_bus_t *bus, uint32_t scr) {
    s->cpu = *cpu; s->pending = g_irqt.pending; s->seen = s_timer_cycles_seen;
    s->last_store = g_last_store_ip; s->last_write = g_mem_last_write_ip; s->bus_ip = bus->cpu_ip;
    for (uint32_t i = 0; i < sizeof s->mem; i++) s->mem[i] = mem_read8(bus, scr - 0x40u + i);
}
static void jt_put(const jt_state_t *s, i960_cpu_t *cpu, memory_bus_t *bus, uint32_t scr) {
    *cpu = s->cpu; g_irqt.pending = s->pending; s_timer_cycles_seen = s->seen;
    g_last_store_ip = s->last_store; g_mem_last_write_ip = s->last_write; bus->cpu_ip = s->bus_ip;
    for (uint32_t i = 0; i < sizeof s->mem; i++) bus->wr_page[(scr - 0x40u + i) >> 16][(scr - 0x40u + i) & 0xFFFFu] = s->mem[i];
}

/* Which part differs, or NULL. */
static const char *jt_diff(const jt_state_t *a, const jt_state_t *b) {
    if (a->k != b->k) return "ops run";
    if (memcmp(&a->cpu.globals, &b->cpu.globals, 128)) return "registers";
    if (a->cpu.sfr.ac != b->cpu.sfr.ac) return "ac";
    if (a->cpu.sfr.ip != b->cpu.sfr.ip) return "ip";
    if (a->cpu.cycles != b->cpu.cycles) return "cycles";
    if (memcmp(&a->cpu, &b->cpu, sizeof a->cpu)) return "cpu (other)";
    if (a->pending != b->pending || a->seen != b->seen) return "board clock";
    if (a->last_store != b->last_store) return "g_last_store_ip";
    if (a->last_write != b->last_write) return "g_mem_last_write_ip";
    if (a->bus_ip != b->bus_ip) return "bus cpu_ip";
    if (memcmp(a->mem, b->mem, sizeof a->mem)) return "memory";
    return NULL;
}

/* Run `cases` random blocks; the summary goes to rows row.. of the screen. */
#ifdef IB_WHY
static char     g_jt_bench[64], g_jt_bench2[64];
static uint32_t jt_tick[2], jt_bops;
#endif
static void jt_run(i960_cpu_t *cpu, memory_bus_t *bus, int cases, int row) {
    char line[128];
    /* the scratch: 0x80 below the end of a work-RAM page whose next page is plain RAM too */
    uint32_t scr = 0;
    if (!bus->page_ok) mem_build_pages(bus);
    for (uint32_t p = 0x50u; p < 0xFFFFu && !scr; p++)
        if (bus->wr_page[p] && bus->wr_page[p + 1] && bus->rd_page[p] == bus->wr_page[p]
                && bus->rd_page[p + 1] == bus->wr_page[p + 1])
            scr = (p << 16) + 0xFF80u;
    uint32_t rom = 0;
    for (uint32_t p = 0; p < 0x40u && !rom; p++)
        if (bus->rd_page[p] && !bus->wr_page[p]) rom = (p << 16) + 0x1000u;
    if (!rom) rom = scr - 0x40u;   /* the pager maps ROM pages as they are touched */
    if (!scr) {
        snprintf(line, sizeof line, "jit test: no scratch page (50: rd %p wr %p)", bus->rd_page[0x50], bus->wr_page[0x50]);
        dc_text(row, line);
        return;
    }

    static i960_cpu_t saved; saved = *cpu;
    static jt_state_t keep;  jt_take(&keep, cpu, bus, scr);
    uint32_t pass = 0, fail = 0, nocomp = 0, ops = 0, slowcalls = g_ibj.slow;
    const char *first = NULL; int first_case = -1;
    char kinds[64] = "";
    uint32_t attn = g_emu_attn;

    for (int t = 0; t < cases; t++) {
        /* a block */
        uint32_t ip = 0x00010000u + (uint32_t)t * 0x100u, n = 0, cyc = 0;
        uint32_t want = 1 + jt_pick(IB_MAX);
        while (n < want) {
            uint32_t w[2] = { 0, jt_rand() };
            int len = jt_insn(w);
            bool end = false;
            uint32_t l = ib_decode(cpu, &jt_ops[n], ip, w[0], w[1], &end);
            if (!l) continue;
            cyc += jt_ops[n].cyc & 0x7FFu;
            ip += (uint32_t)len * 4u;
            n++;
        }
        jt_blk.ip = jt_ops[0].ip; jt_blk.n = (uint16_t)n; jt_blk.cyc = (uint16_t)cyc;
        jt_blk.next = ip; jt_blk.op = jt_ops;
        ibj_reset();
        if (!ib_jit_compile(cpu, &jt_blk)) { nocomp++; continue; }

        /* a state */
        for (int r = 0; r < 32; r++) {
            uint32_t v = jt_rand();
            switch (jt_pick(4)) { case 0: v &= 0x3Fu; break; case 1: v = (uint32_t)(int32_t)(int8_t)v; break; default: break; }
            ((uint32_t *)&cpu->globals)[r] = v;
        }
        cpu->globals.g[11] = jt_pick(16);
        cpu->globals.g[12] = scr;
        cpu->globals.g[13] = rom;
        cpu->sfr.ac = (cpu->sfr.ac & ~7u) | (uint32_t)jt_pick(8);
        jt_take(&jt_s0, cpu, bus, scr);

        s_ibj_on = 0;
        jt_a.k = ib_run(cpu, bus, &jt_blk, attn);
        { uint32_t k = jt_a.k; jt_take(&jt_a, cpu, bus, scr); jt_a.k = k; }
        jt_put(&jt_s0, cpu, bus, scr);
        s_ibj_on = 1;
        jt_b.k = ib_run(cpu, bus, &jt_blk, attn);
        { uint32_t k = jt_b.k; jt_take(&jt_b, cpu, bus, scr); jt_b.k = k; }
        jt_put(&jt_s0, cpu, bus, scr);
        ops += n;
#ifdef IB_WHY
        for (int rep = 0; rep < 20; rep++) {   /* the speed: TMU2 ticks in ib_run, each way */
            for (int way = 0; way < 2; way++) {
                s_ibj_on = way;
                uint32_t t0 = IBW_T();
                ib_run(cpu, bus, &jt_blk, attn);
                jt_tick[way] += IBW_D(t0);
                jt_put(&jt_s0, cpu, bus, scr);
            }
            jt_bops += n;
        }
#endif

        const char *d = jt_diff(&jt_a, &jt_b);
        if (!d) { pass++; continue; }
        if (!fail++) {
            first = d; first_case = t;
            int o = 0;
            for (uint32_t i = 0; i < n && o < 60; i++) o += snprintf(kinds + o, sizeof kinds - (size_t)o, "%u ", jt_ops[i].kind);
            /* the first differing register, for the screen */
            if (!strcmp(d, "registers"))
                for (int r = 0; r < 32; r++)
                    if (((uint32_t *)&jt_a.cpu.globals)[r] != ((uint32_t *)&jt_b.cpu.globals)[r]) {
                        snprintf(line, sizeof line, "reg %d: run %08lx jit %08lx", r,
                                 (unsigned long)((uint32_t *)&jt_a.cpu.globals)[r],
                                 (unsigned long)((uint32_t *)&jt_b.cpu.globals)[r]);
                        dc_text(row + 3, line);
                        break;
                    }
        }
    }
#ifdef IB_WHY
    {   /* a register-only block, 16 addo g1,g2,g3, run 5000 times each way */
        uint32_t ip = 0x00010000u, rb[2] = { 0, 0 };
        for (uint32_t n = 0; n < 16; n++) { bool end = false; ib_decode(cpu, &jt_ops[n], ip, 0x59000000u | (19u << 19) | (18u << 14) | 17u, 0, &end); ip += 4; }
        jt_blk.ip = 0x00010000u; jt_blk.n = 16; jt_blk.cyc = 16; jt_blk.next = ip; jt_blk.op = jt_ops;
        ibj_reset();
        if (ib_jit_compile(cpu, &jt_blk))
            for (int way = 0; way < 2; way++) {
                s_ibj_on = way;
                uint32_t t0 = IBW_T();
                for (int r = 0; r < 5000; r++) ib_run(cpu, bus, &jt_blk, attn);
                rb[way] = IBW_D(t0);
            }
        uint64_t tps = *(volatile uint32_t *)0xFFD80020u + 1u;
        snprintf(g_jt_bench2, sizeof g_jt_bench2, "16 addo x5000: ns/op run %u jit %u",
                 (unsigned)((uint64_t)rb[0] * 1000000000ull / tps / 80000u), (unsigned)((uint64_t)rb[1] * 1000000000ull / tps / 80000u));
    }
#endif
    jt_put(&keep, cpu, bus, scr);
    *cpu = saved;
    ibj_reset();
    s_ib_valid = 0;

    snprintf(line, sizeof line, "jit test: %lu pass %lu FAIL %lu uncompiled, %lu ops %lu slow",
             (unsigned long)pass, (unsigned long)fail, (unsigned long)nocomp, (unsigned long)ops,
             (unsigned long)(g_ibj.slow - slowcalls));
    printf("%s\n", line);
    dc_text(row, line);
#ifdef IB_WHY
    {   uint64_t tps = *(volatile uint32_t *)0xFFD80020u + 1u, bo = jt_bops ? jt_bops : 1;
        snprintf(g_jt_bench, sizeof g_jt_bench, "jit test %lu/%lu; ns/op run %u jit %u",
                 (unsigned long)pass, (unsigned long)(pass + fail),
                 (unsigned)((uint64_t)jt_tick[0] * 1000000000ull / tps / bo), (unsigned)((uint64_t)jt_tick[1] * 1000000000ull / tps / bo)); }
#endif
    if (fail) {
        snprintf(line, sizeof line, "first: case %d, %s; ops %s", first_case, first, kinds);
        printf("%s\n", line);
        dc_text(row + 1, line);
        snprintf(line, sizeof line, "k %lu/%lu ip %08lx/%08lx ac %lx/%lx cyc %lu/%lu",
                 (unsigned long)jt_a.k, (unsigned long)jt_b.k,
                 (unsigned long)jt_a.cpu.sfr.ip, (unsigned long)jt_b.cpu.sfr.ip,
                 (unsigned long)jt_a.cpu.sfr.ac, (unsigned long)jt_b.cpu.sfr.ac,
                 (unsigned long)jt_a.cpu.cycles, (unsigned long)jt_b.cpu.cycles);
        dc_text(row + 2, line);
    }
}

#endif
#endif /* DC_JIT_TEST_H */
