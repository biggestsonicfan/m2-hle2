/*
 * i960_test.c — standalone unit test for the i960 core decode/execute.
 *
 * Focuses on the load-bearing §3.1 invariants (CLAUDE.md / IMPLEMENTATION-DRAFT)
 * that cost trial-and-error and must never regress:
 *   - chkbit sets CC_NO (not CC_NE) when the tested bit is 0; bno depends on it
 *   - cmpobX always updates CC even when the branch is NOT taken
 *   - MEM mode 0x5 is IP-relative: EA = IP + 8 + disp
 *   - FP-from-GPR is a bit reinterpret (memcpy), not (float)int
 *   - call/ret frame: SP aligned to 64, locals zeroed, pfp/sp/rip saved, g15 synced
 * plus a few plain ALU / load-store sanity checks.
 *
 * Code is assembled by hand into work RAM and executed via i960_step().
 */
#define NDEBUG 1
#include <stdio.h>
#include <string.h>

#include "i960_exec.h"

/* i960_exec.h → hle_hooks.h references g_active_profile (defined by the app in
 * registry.h). The test links no profiles, so define it NULL: hle_check then
 * short-circuits and every instruction executes normally. */
const game_profile_t *g_active_profile = NULL;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

/* ---- instruction encoders ------------------------------------------------ */

static uint32_t enc_reg(uint32_t op, int dst, int src2, int m2,
                        int src1, int m1, int m3) {
    uint32_t hi = (op >> 4) & 0xFF, lo = op & 0xF;
    return (hi << 24) | ((dst & 0x1F) << 19) | ((src2 & 0x1F) << 14)
         | ((m3 & 1) << 13) | ((m2 & 1) << 12) | ((m1 & 1) << 11)
         | ((lo & 0xF) << 7) | (src1 & 0x1F);
}
/* COBR: compare/branch. m1 = literal flag on src1. disp is 13-bit (bits 2..12). */
static uint32_t enc_cobr(uint32_t op, int src1, int src2, int m1, int32_t disp) {
    return (op << 24) | ((src1 & 0x1F) << 19) | ((src2 & 0x1F) << 14)
         | ((m1 & 1) << 13) | ((uint32_t)disp & 0x1FFC);
}
/* CTRL: branch/call/ret. disp is 24-bit (bits 2..23). */
static uint32_t enc_ctrl(uint32_t op, int32_t disp) {
    return (op << 24) | ((uint32_t)disp & 0x00FFFFFC);
}
/* MEMB mode 0x5 (IP-relative, 2 words). word2 carries the displacement. */
static uint32_t enc_memb5(uint32_t op, int dst) {
    return (op << 24) | ((dst & 0x1F) << 19) | (0x5 << 10);
}
/* MEMA: offset(abase) when use_abase, else offset only. */
static uint32_t enc_mema(uint32_t op, int dst, int abase, int use_abase, uint32_t off) {
    uint32_t mode = use_abase ? 0x8 : 0x0;
    return (op << 24) | ((dst & 0x1F) << 19) | ((abase & 0x1F) << 14)
         | (mode << 10) | (off & 0xFFF);
}

#define G(n) (16 + (n))   /* global reg index */
#define L(n) (n)          /* local  reg index */

static memory_bus_t bus;
static i960_cpu_t   cpu;

static void put(uint32_t addr, uint32_t w) { mem_write32(&bus, addr, w); }
static uint32_t cc(void) { return cpu.sfr.ac & AC_CC_MASK; }

int main(void) {
    mem_init(&bus, NULL, 0);
    wp_init();

    const uint32_t CODE = RAM_BASE + 0x1000;

    /* ---- chkbit -> CC_NO when bit clear, CC_E when set ------------------- */
    i960_reset(&cpu);
    cpu.globals.g[4] = 0x00000000;            /* g4 = 0 */
    cpu.sfr.ip = CODE;
    put(CODE, enc_reg(0x5ae, 0, G(4), 0, 0, 1, 0)); /* chkbit lit#0, g4 */
    i960_step(&cpu, &bus);
    CHECK(cc() == CC_NO, "chkbit on clear bit sets CC_NO (0x0), not CC_NE");

    i960_reset(&cpu);
    cpu.globals.g[4] = 0x00000001;            /* g4 bit0 set */
    cpu.sfr.ip = CODE;
    put(CODE, enc_reg(0x5ae, 0, G(4), 0, 0, 1, 0));
    i960_step(&cpu, &bus);
    CHECK(cc() == CC_E, "chkbit on set bit sets CC_E");

    /* bno must branch after a chkbit that cleared CC to CC_NO. */
    i960_reset(&cpu);
    cpu.globals.g[4] = 0;
    cpu.sfr.ip = CODE;
    put(CODE,     enc_reg(0x5ae, 0, G(4), 0, 0, 1, 0)); /* chkbit -> CC_NO */
    put(CODE + 4, enc_ctrl(0x10, 0x40));                /* bno +0x40       */
    i960_step(&cpu, &bus);   /* chkbit */
    i960_step(&cpu, &bus);   /* bno    */
    CHECK(cpu.sfr.ip == CODE + 4 + 0x40, "bno branches when chkbit left CC_NO");

    /* ---- cmpobe updates CC even when the branch is NOT taken ------------- */
    i960_reset(&cpu);
    cpu.globals.g[2] = 3;
    cpu.sfr.ip = CODE;
    /* cmpobe lit#5, g2, +0x20 : 5 != 3 so no branch, but CC must become CC_G */
    put(CODE, enc_cobr(0x32, 5, G(2), 1, 0x20));
    i960_step(&cpu, &bus);
    CHECK(cpu.sfr.ip == CODE + 4, "cmpobe not-taken falls through");
    CHECK(cc() == CC_G, "cmpobe updates CC (5>3 -> CC_G) even when not taken");

    /* ---- MEM mode 0x5 IP-relative: EA = IP + 8 + disp ------------------- */
    i960_reset(&cpu);
    cpu.sfr.ip = CODE;
    put(CODE,     enc_memb5(0x8C, L(5)));   /* lda (IP-relative) -> r5 */
    put(CODE + 4, 0x40);                     /* disp word2 */
    i960_step(&cpu, &bus);
    CHECK(cpu.locals.r[5] == CODE + 8 + 0x40, "MEM mode 0x5 EA = IP + 8 + disp");
    CHECK(cpu.sfr.ip == CODE + 8, "MEMB instruction is 8 bytes long");

    /* ---- FP-from-GPR is a bit reinterpret, not (float)int --------------- */
    i960_reset(&cpu);
    cpu.globals.g[0] = 0x3FC00000;  /* 1.5f bits */
    cpu.globals.g[1] = 0x40200000;  /* 2.5f bits */
    cpu.sfr.ip = CODE;
    /* faddr g0, g1, g2 (all int regs: m1=m2=m3=0) -> g2 = bits of 4.0f */
    put(CODE, enc_reg(0x78F, G(2), G(1), 0, G(0), 0, 0));
    i960_step(&cpu, &bus);
    CHECK(cpu.globals.g[2] == 0x40800000,
          "faddr reads GPRs as float bits (1.5+2.5 -> 4.0f bits)");

    /* ---- call/ret frame mechanics --------------------------------------- */
    i960_reset(&cpu);
    cpu.locals.pfp = 0x0000AAAA;
    cpu.locals.sp  = 0x00001010;     /* not 64-aligned */
    cpu.locals.r[7] = 0xDEAD;        /* a local that ret must restore */
    cpu.sfr.ip = CODE;
    put(CODE,        enc_ctrl(0x09, 0x80));  /* call +0x80 */
    put(CODE + 0x80, enc_ctrl(0x0A, 0));     /* ret        */
    i960_step(&cpu, &bus);   /* call */
    CHECK(cpu.sfr.ip == CODE + 0x80, "call branches to IP+disp");
    CHECK(cpu.frame_depth == 1, "call pushes a frame");
    CHECK(cpu.locals.rip == CODE + 4, "call saves return IP in rip");
    CHECK(cpu.locals.pfp == 0x0000AAAA, "call preserves pfp into new frame");
    CHECK(cpu.locals.sp == 0x1040, "call aligns SP up to 64 bytes (0x1010 -> 0x1040)");
    CHECK(cpu.globals.fp == cpu.locals.pfp, "call syncs g15/fp to pfp");
    CHECK(cpu.locals.r[7] == 0, "call zeroes new frame locals");
    i960_step(&cpu, &bus);   /* ret */
    CHECK(cpu.sfr.ip == CODE + 4, "ret returns to saved rip");
    CHECK(cpu.frame_depth == 0, "ret pops the frame");
    CHECK(cpu.locals.r[7] == 0xDEAD, "ret restores caller locals");

    /* ---- plain ALU + load/store sanity ---------------------------------- */
    i960_reset(&cpu);
    cpu.globals.g[0] = 100; cpu.globals.g[1] = 40;
    cpu.sfr.ip = CODE;
    put(CODE, enc_reg(0x590, G(2), G(1), 0, G(0), 0, 0)); /* addo g0,g1,g2 */
    i960_step(&cpu, &bus);
    CHECK(cpu.globals.g[2] == 140, "addo g0+g1 -> g2");

    i960_reset(&cpu);
    cpu.globals.g[0] = 100; cpu.globals.g[1] = 40;
    cpu.sfr.ip = CODE;
    put(CODE, enc_reg(0x593, G(2), G(1), 0, G(0), 0, 0)); /* subi: dst=src2-src1 */
    i960_step(&cpu, &bus);
    CHECK(cpu.globals.g[2] == (uint32_t)(40 - 100), "subi computes src2-src1 (signed)");

    /* st g0 -> [r3]; ld [r3] -> g4   (round trip through the bus) */
    i960_reset(&cpu);
    cpu.globals.g[0] = 0xCAFEBABE;
    cpu.locals.r[3]  = RAM_BASE + 0x200;
    cpu.sfr.ip = CODE;
    put(CODE,     enc_mema(0x92, G(0), L(3), 1, 0));  /* st  g0, (r3) */
    put(CODE + 4, enc_mema(0x90, G(4), L(3), 1, 0));  /* ld  (r3), g4 */
    i960_step(&cpu, &bus);
    i960_step(&cpu, &bus);
    CHECK(mem_read32(&bus, RAM_BASE + 0x200) == 0xCAFEBABE, "st wrote to memory");
    CHECK(cpu.globals.g[4] == 0xCAFEBABE, "ld read it back into g4");

    /* ---- watchpoint fires on the bus write ------------------------------ */
    wp_init();
    g_wp.hit = 0;
    wp_add(RAM_BASE + 0x300, RAM_BASE + 0x304, true, false, "wp_test");
    i960_reset(&cpu);
    cpu.globals.g[0] = 0x12345678;
    cpu.locals.r[3]  = RAM_BASE + 0x300;
    cpu.sfr.ip = CODE;
    put(CODE, enc_mema(0x92, G(0), L(3), 1, 0));      /* st g0, (r3) */
    i960_step(&cpu, &bus);
    CHECK(g_wp.hit && g_wp.hit_addr == RAM_BASE + 0x300, "write watchpoint fires");

    mem_shutdown(&bus);
    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
