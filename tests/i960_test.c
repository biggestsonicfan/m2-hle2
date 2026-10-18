/*
 * i960_test.c — standalone unit test for the i960 core decode/execute.
 *
 * Focuses on the load-bearing §3.1 invariants (CLAUDE.md / IMPLEMENTATION-DRAFT)
 * that cost trial-and-error and must never regress:
 *   - chkbit sets CC_NO (not CC_NE) when the tested bit is 0; bno depends on it
 *   - cmpobX always updates CC even when the branch is NOT taken
 *   - MEM mode 0x5 is IP-relative: EA = IP + 8 + disp
 *   - FP-from-GPR is a bit reinterpret (memcpy), not (float)int
 *   - call/ret frame: FP (g15) = SP aligned to 64, SP = FP + 64, locals zeroed,
 *     pfp/sp/rip saved; ret puts the caller's g15 back
 *   - modpc: mask from src2, new bits from src/dst, which gets the old PC
 *   - shrdi divides: arithmetic shift rounded toward zero, dst not read
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

/* shrdi is a dividing shift: arithmetic, rounded toward zero. Sega Rally
 * halves (pedal byte - 0x30) with it; a 64-bit "double" shift took the
 * destination's old bits and made -16 into 0x7FFFFFF8. */
static void test_shrdi(uint32_t CODE) {
    static const struct { int32_t v; int n; int32_t want; } sd[] = {
        { -16, 1, -8 }, { -15, 1, -7 }, { 15, 1, 7 }, { -1, 4, 0 }, { -48, 0, -48 },
    };
    int ok = 1;
    for (unsigned k = 0; k < sizeof sd / sizeof sd[0]; k++) {
        i960_reset(&cpu);
        cpu.globals.g[4] = (uint32_t)sd[k].v;
        cpu.globals.g[5] = 0x12345678;
        cpu.sfr.ip = CODE;
        put(CODE, enc_reg(0x59a, G(5), G(4), 0, sd[k].n, 1, 0)); /* shrdi n, g4, g5 */
        i960_step(&cpu, &bus);
        if ((int32_t)cpu.globals.g[5] != sd[k].want) ok = 0;
    }
    CHECK(ok, "shrdi shifts right dividing (rounds toward zero), ignoring the old dst");
}

/* The game's wait table moves the board timing's costs a cycle a word for
 * each wait it differs from STF's, where the costs were measured (Pinboard
 * #763), and by the DRAM bit where the area and its measurement differ on it
 * (#772). STF's table leaves the measured areas; Sega Rally's puts program ROM
 * at 4 waits and its backup SRAM at 4; a word that is not a count (0,
 * External) leaves the area alone. */
static void put_table(uint8_t *t, const uint32_t *w) {
    for (int a = 0; a < I960_BT_AREAS; a++)
        for (int b = 0; b < 4; b++) t[4 * a + b] = (uint8_t)(w[a] >> (8 * b));
}

/* Every region the bench measured under its own area keeps that cost. */
static int measured_kept(void) {
    for (int r = 0; r < I960_BT_REGIONS; r++) {
        if (r == I960_BT_BACK || r == I960_BT_DLRAM) continue;
        for (int n = 1; n <= 4; n++)
            if (g_i960_bt_ld[r][n] != g_i960_bt_ld0[r][n] || g_i960_bt_st[r][n] != g_i960_bt_st0[r][n]) return 0;
    }
    return 1;
}

static void test_area_waits(void) {
    static const uint32_t stf[I960_BT_AREAS]  = { 4, 2, 0x42, 2, 1, 2, 0x10, 0x10, 0x20, 0x10, 0x10, 0x10, 0x10, 0x10 };
    static const uint32_t rally[I960_BT_AREAS] = { 0x10, 2, 0x42, 2, 0x10, 0x10, 0x10, 0x10, 8, 0x10, 0x10, 0x10, 0x10, 0x10 };
    uint8_t t[I960_BT_AREAS * 4];
    i960_bt_init();
    put_table(t, stf);
    i960_bt_areas(t);
    CHECK(measured_kept() && g_i960_bt_fill[I960_BT_ROM] == 0 && g_i960_bt_brfill[I960_BT_ROM] == 0
          && g_i960_bt_fill[I960_BT_RAM] == -4 * I960_BT_WAIT + I960_BT_FILL_DRAM,
          "STF's wait table leaves the measured costs; code in 1-wait DRAM fills 2.8 cycles sooner");
    CHECK(g_i960_bt_ld[I960_BT_DLRAM][1] == g_i960_bt_ld0[I960_BT_RAM][1] - I960_BT(0.84375)
          && g_i960_bt_st[I960_BT_DLRAM][1] == g_i960_bt_st0[I960_BT_RAM][1] - I960_BT(0.47265625)
          && g_i960_bt_ld[I960_BT_DLRAM][4] == g_i960_bt_ld0[I960_BT_RAM][4] - I960_BT(1.1875),
          "download RAM is work RAM without the DRAM bit");
    CHECK(g_i960_bt_ld[I960_BT_BACK][1] == g_i960_bt_ld0[I960_BT_BACK][1] + 3 * I960_BT_WAIT - I960_BT(1.109375),
          "backup SRAM at 4 waits costs 3 a word over work RAM, less the DRAM bit");
    put_table(t, rally);
    i960_bt_areas(t);
    CHECK(g_i960_bt_ld[I960_BT_ROM][2] == g_i960_bt_ld0[I960_BT_ROM][2] + 4 * I960_BT_WAIT
          && g_i960_bt_fill[I960_BT_ROM] == 8 * I960_BT_WAIT && g_i960_bt_brfill[I960_BT_ROM] == 12 * I960_BT_WAIT,
          "program ROM at 4 waits: +2 a word, +8 a line fill, +12 a branch's");
    memset(t, 0, sizeof t);
    i960_bt_areas(t);
    CHECK(g_i960_bt_ld[I960_BT_ROM][1] == g_i960_bt_ld0[I960_BT_ROM][1] && g_i960_bt_fill[I960_BT_RAM] == 0
          && g_i960_bt_ld[I960_BT_DLRAM][1] == g_i960_bt_ld0[I960_BT_DLRAM][1],
          "an unwritten table leaves the measured costs");
}

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
    cpu.globals.fp  = 0x00000FC0;    /* the caller's frame */
    cpu.sfr.ip = CODE;
    put(CODE,        enc_ctrl(0x09, 0x80));  /* call +0x80 */
    put(CODE + 0x80, enc_ctrl(0x0A, 0));     /* ret        */
    i960_step(&cpu, &bus);   /* call */
    CHECK(cpu.sfr.ip == CODE + 0x80, "call branches to IP+disp");
    CHECK(cpu.frame_depth == 1, "call pushes a frame");
    CHECK(cpu.locals.rip == CODE + 4, "call saves return IP in rip");
    CHECK(cpu.locals.pfp == 0x00000FC0, "call puts the caller's FP in pfp");
    CHECK(cpu.globals.fp == 0x1040, "call: g15/fp is SP aligned up to 64 bytes (0x1010 -> 0x1040)");
    CHECK(cpu.locals.sp == 0x1080, "call: SP starts a frame (64 bytes) above FP");
    CHECK(cpu.locals.r[7] == 0, "call zeroes new frame locals");
    i960_step(&cpu, &bus);   /* ret */
    CHECK(cpu.sfr.ip == CODE + 4, "ret returns to saved rip");
    CHECK(cpu.frame_depth == 0, "ret pops the frame");
    CHECK(cpu.locals.r[7] == 0xDEAD, "ret restores caller locals");
    CHECK(cpu.globals.fp == 0x0FC0, "ret restores the caller's g15/fp");

    /* ---- ret with return status 7: an interrupt return ------------------- */
    /* pfp & 7 == 7 in supervisor mode: the PC and AC saved under the frame
     * (FP-16, FP-12) come back (MAME do_ret). Virtual-On's boot code (0xA30)
     * drops from priority 31 to 0 this way. */
    i960_reset(&cpu);
    cpu.locals.pfp = 0;
    cpu.locals.sp  = RAM_BASE + 0x2010;
    cpu.globals.fp = RAM_BASE + 0x1FC0;
    cpu.sfr.pc = 0x001F0002;
    cpu.sfr.ip = CODE;
    put(CODE,        enc_ctrl(0x09, 0x80));  /* call +0x80 */
    put(CODE + 0x80, enc_ctrl(0x0A, 0));     /* ret        */
    i960_step(&cpu, &bus);   /* call */
    cpu.locals.pfp |= 7u;
    put(cpu.globals.fp - 16u, 0x00000000);   /* saved PC: user, priority 0 */
    put(cpu.globals.fp - 12u, 0x00003001);   /* saved AC */
    i960_step(&cpu, &bus);   /* ret */
    CHECK(cpu.sfr.ip == CODE + 4, "ret (status 7) returns to saved rip");
    CHECK(cpu.sfr.pc == 0, "ret (status 7) restores the PC saved at FP-16");
    CHECK(cpu.sfr.ac == 0x00003001, "ret (status 7) restores the AC saved at FP-12");

    /* ---- movre: three registers <-> an FP register, 80-bit ---------------- */
    i960_reset(&cpu);
    cpu.globals.g[4] = 0;
    cpu.globals.g[5] = 0xC0000000u;          /* 1.5: mantissa 1.1b, explicit integer bit */
    cpu.globals.g[6] = 0xABCD3FFFu;          /* exponent 0x3FFF; the high half is ignored */
    cpu.sfr.ip = CODE;
    put(CODE,     enc_reg(0x6E1, 0, 0, 0, G(4), 0, 1));     /* movre g4, fp0 */
    put(CODE + 4, enc_reg(0x6E1, L(8), 0, 0, 0, 1, 0));     /* movre fp0, r8 */
    put(CODE + 8, enc_reg(0x6E9, L(12), 0, 0, 22, 1, 0));   /* movre 0f1.0, r12 */
    i960_step(&cpu, &bus);
    CHECK(cpu.fp_regs[0] == 1.5, "movre reads an extended real from three registers");
    i960_step(&cpu, &bus);
    CHECK(cpu.locals.r[8] == 0 && cpu.locals.r[9] == 0xC0000000u && cpu.locals.r[10] == 0x3FFFu,
          "movre writes an FP register as an extended real");
    i960_step(&cpu, &bus);
    CHECK(cpu.locals.r[12] == 0 && cpu.locals.r[13] == 0x80000000u && cpu.locals.r[14] == 0x3FFFu,
          "movre writes the literal 1.0 as an extended real");

    /* ---- modpc src, mask, src/dst ---------------------------------------- */
    /* The boot code's `modpc r4, r4, r5` with r4 = 0x1F0000 (the priority
     * field) and r5 = 0 drops the priority to 0 and hands back the old PC. */
    i960_reset(&cpu);
    cpu.sfr.pc = 0x001F2002;
    cpu.locals.r[4] = 0x001F0000;
    cpu.locals.r[5] = 0;
    cpu.sfr.ip = CODE;
    put(CODE, enc_reg(0x655, L(5), L(4), 0, L(4), 0, 0));   /* modpc r4, r4, r5 */
    i960_step(&cpu, &bus);
    CHECK(cpu.sfr.pc == 0x00002002, "modpc: bits under the mask (src2) come from src/dst");
    CHECK(cpu.locals.r[5] == 0x001F2002, "modpc: src/dst gets the old PC");

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

    /* ---- movq with a literal fills every destination, not r0..r3 -------- */
    i960_reset(&cpu);
    cpu.locals.pfp = 0x11111111; cpu.locals.sp = 0x22222222;
    cpu.locals.rip = 0x33333333; cpu.locals.r[3] = 0x44444444;
    cpu.locals.r[4] = cpu.locals.r[5] = cpu.locals.r[6] = cpu.locals.r[7] = 0xFFFFFFFF;
    cpu.sfr.ip = CODE;
    put(CODE, enc_reg(0x5fc, L(4), 0, 0, 0, 1, 0));    /* movq lit#0, r4 */
    i960_step(&cpu, &bus);
    CHECK(cpu.locals.r[4] == 0 && cpu.locals.r[5] == 0 && cpu.locals.r[6] == 0 && cpu.locals.r[7] == 0,
          "movq 0, r4 zeroes r4..r7 (literal, not pfp/sp/rip/r3)");

    i960_reset(&cpu);
    cpu.locals.r[8] = 0xA; cpu.locals.r[9] = 0xB;
    cpu.sfr.ip = CODE;
    put(CODE, enc_reg(0x5dc, G(0), 0, 0, L(8), 0, 0));  /* movl r8, g0 */
    i960_step(&cpu, &bus);
    CHECK(cpu.globals.g[0] == 0xA && cpu.globals.g[1] == 0xB, "movl r8, g0 copies r8:r9 into g0:g1");

    test_shrdi(CODE);
    test_area_waits();

    /* ---- concmpi only compares when CC bit 2 (less) is clear ------------- */
    /* The range test `cmpi x, lo; concmpi x, hi` is lo <= x <= hi. STF's
     * get_en_info checks the angle to the opponent this way. */
    i960_reset(&cpu);
    cpu.locals.r[3] = (uint32_t)-0x2000;   /* x, below the range */
    cpu.locals.r[15] = (uint32_t)-0x1554;  /* lo */
    cpu.locals.r[4] = 0x1554;              /* hi */
    cpu.sfr.ip = CODE;
    put(CODE,     enc_reg(0x5a1, 0, L(15), 0, L(3), 0, 0));  /* cmpi r3, r15 */
    put(CODE + 4, enc_reg(0x5a3, 0, L(4), 0, L(3), 0, 0));   /* concmpi r3, r4 */
    i960_step(&cpu, &bus);
    i960_step(&cpu, &bus);
    CHECK(cc() == CC_L, "concmpi leaves CC_L alone: x below the range stays out of it");

    i960_reset(&cpu);
    cpu.locals.r[3] = 0;
    cpu.locals.r[15] = (uint32_t)-0x1554;
    cpu.locals.r[4] = 0x1554;
    cpu.sfr.ip = CODE;
    put(CODE,     enc_reg(0x5a1, 0, L(15), 0, L(3), 0, 0));
    put(CODE + 4, enc_reg(0x5a3, 0, L(4), 0, L(3), 0, 0));
    i960_step(&cpu, &bus);
    i960_step(&cpu, &bus);
    CHECK(cc() == CC_E, "cmpi + concmpi: x inside the range comes out CC_E");

    /* cmpr against a NaN is unordered, 000, so bge does not take it (MAME
     * cmp_d). coli_recalc_pos clamps a push-out to its limit that way. */
    i960_reset(&cpu);
    cpu.locals.r[15] = 0xFFFFFFFFu;        /* NaN */
    cpu.locals.r[8]  = 0;
    cpu.sfr.ip = CODE;
    put(CODE, enc_reg(0x685, 0, L(8), 0, L(15), 0, 0));     /* cmpr r15, r8 */
    i960_step(&cpu, &bus);
    CHECK(cc() == CC_NO, "cmpr with a NaN sets CC 000 (unordered), not CC_G");

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
