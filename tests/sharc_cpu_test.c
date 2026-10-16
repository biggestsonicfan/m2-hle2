/*
 * sharc_cpu_test.c - the ADSP-21062 SHARC interpreter (src/board/sharc_cpu.h).
 *
 * Part 1 needs no ROM: a hand-assembled program (integer and float compute,
 * a circular DAG buffer, a counted DO loop, CALL / delayed RTS, a flag spin,
 * an external read the board stalls, an external write, IDLE).
 *
 * Part 2 boots Virtual-On's copro program (Model 2B, Pinboard #935) when
 * SHC_VON_IMAGE names the upload (11038 little-endian halfwords, the words
 * the i960 sends to the copro FIFO); it is Sega data, so it is not in the
 * repo and the part is skipped without it. The firmware waits for the sync
 * word 8 (PM 0x2011E), then loops at 0x20125: a word from the FIFO indexes
 * the jump table at DM 0x30000 and the handler reads its arguments and
 * writes its result to the output FIFO.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "board/sharc_cpu.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* ---- The board side: FIFO in / FIFO out, as model2.cpp maps them ----------- */

static uint32_t fin[256], fin_head, fin_tail;
static uint32_t fout[256], fout_n;

static void fifo_flags(void)
{
    shc_set_flag_input(0, fin_head == fin_tail);    /* FLAG0: input empty */
    shc_set_flag_input(1, 0);                       /* FLAG1: output full */
}

static void push(uint32_t v)
{
    fin[fin_tail++ & 0xff] = v;
    fifo_flags();
}

static uint32_t board_rd(uint32_t addr, bool *stall)
{
    if (addr >= 0x400000 && addr < 0xc00000) {
        if (fin_head == fin_tail) {
            *stall = true;
            return 0;
        }
        uint32_t const v = fin[fin_head++ & 0xff];
        fifo_flags();
        return v;
    }
    return 0;   /* buffer RAM and the copro_data ROM: not used here */
}

static void board_wr(uint32_t addr, uint32_t v, bool *stall)
{
    (void)stall;
    if (addr >= 0xc00000 && addr < 0x1400000 && fout_n < 256)
        fout[fout_n++] = v;
}

static void board_reset(void)
{
    fin_head = fin_tail = 0;
    fout_n = 0;
    shc_ext_rd = board_rd;
    shc_ext_wr = board_wr;
    shc_reset();
    fifo_flags();
}

/* ---- Part 1: a hand-assembled program -------------------------------------- */

static const uint64_t k_mini[] = {
    0x0F007FFFFFFFull,  /* 20005 r0=0x7fffffff */
    0x0F0100000001ull,  /* 20006 r1=1 */
    0x013E00001201ull,  /* 20007 r2=r0+r1 */
    0x0F033FC00000ull,  /* 20008 r3=1.5f */
    0x0F0440100000ull,  /* 20009 r4=2.25f */
    0x013E00081534ull,  /* 2000A f5=f3+f4 */
    0x013E00130634ull,  /* 2000B f6=f3*f4 */
    0x0F4700030010ull,  /* 2000C b7=0x30010 (and i7) */
    0x0F3700000003ull,  /* 2000D l7=3 */
    0x0F2100000001ull,  /* 2000E m1=1 */
    0x0F0700000000ull,  /* 2000F r7=0 */
    0x0C0005000002ull,  /* 20010 lcntr=5, do (pc,2) until lce */
    0x013E00029770ull,  /* 20011   r7=r7+1 */
    0x5E7E83800000ull,  /* 20012   dm(i7,m1)=r7 */
    0x06BE0002001Eull,  /* 20013 call 0x2001e */
    0x013E00001981ull,  /* 20014 r9=r8+r1 */
    0x0F1100C00000ull,  /* 20015 i1=0xc00000 */
    0x0F2000000000ull,  /* 20016 m0=0 */
    0x523E84800000ull,  /* 20017 dm(i1,m0)=r9 */
    0x0F1000400000ull,  /* 20018 i0=0x400000 */
    0x071200000000ull,  /* 20019 if flag0_in jump (pc,0) */
    0x503E05000000ull,  /* 2001A r10=dm(i0,m0) */
    0x008000000000ull,  /* 2001B idle */
    0x000000000000ull,  /* 2001C nop */
    0x000000000000ull,  /* 2001D nop */
    0x0A3E04000000ull,  /* 2001E rts (db) */
    0x0F0800001234ull,  /* 2001F   r8=0x1234 */
    0x013E00001B11ull,  /* 20020   r11=r1+r1 */
};

static void test_mini(void)
{
    board_reset();
    for (uint32_t i = 0; i < sizeof k_mini / sizeof k_mini[0]; i++)
        shc_pm_wr48(0x20005 + i, k_mini[i]);
    CHECK(shc_pm_rd48(0x20013) == 0x06BE0002001Eull, "PM 48-bit words read back");
    CHECK(shc_run(100) == 0 && g_shc.stop == SHC_STOP_HALTED, "held until shc_boot");
    shc_boot();

    int n = shc_run(3);
    CHECK(n == 3 && g_shc.stop == SHC_STOP_BUDGET, "ran the 3-cycle budget");
    CHECK(g_shc.r[2] == 0x80000000u, "int add 0x7fffffff + 1");
    CHECK((g_shc.astat & (SHC_AV | SHC_AN | SHC_AZ)) == (SHC_AV | SHC_AN), "AV and AN, not AZ");
    n += shc_run(1000);
    CHECK(g_shc.stop == SHC_STOP_SPIN && g_shc.pc == 0x20019, "spins on FLAG0 at 0x20019");
    CHECK(n == 32, "cycles to the spin (5-pass loop, call, delayed rts)");
    CHECK(u2f(g_shc.r[5]) == 3.75f, "fadd 1.5 + 2.25");
    CHECK(u2f(g_shc.r[6]) == 3.375f, "fmul 1.5 * 2.25");
    CHECK(g_shc.r[7] == 5, "DO loop ran 5 times");
    CHECK(g_shc.blk[1][0x10] == 4 && g_shc.blk[1][0x11] == 5 && g_shc.blk[1][0x12] == 3,
          "circular buffer of 3 wrapped (4 5 3)");
    CHECK(g_shc.dag1.i[7] == 0x30012, "i7 wrapped to b7 + 2");
    CHECK((g_shc.s.stky & SHC_LSEM) && (g_shc.s.stky & SHC_PCEM), "loop and PC stacks empty");
    CHECK(g_shc.r[8] == 0x1234 && g_shc.r[11] == 2, "both delay slots of rts (db) ran");
    CHECK(g_shc.r[9] == 0x1235, "returned after the call");
    CHECK(fout_n == 1 && fout[0] == 0x1235, "external write reached the board");

    n = shc_run(1000);
    CHECK(n == 1 && g_shc.stop == SHC_STOP_SPIN, "still spinning while FLAG0 is set");

    shc_set_flag_input(0, 0);       /* the flag lies: the read finds nothing */
    n = shc_run(1000);
    CHECK(g_shc.stop == SHC_STOP_STALL && g_shc.daddr == 0x2001A, "empty FIFO read stalls (next is 0x2001A)");
    n = shc_run(1000);
    CHECK(n == 0 && g_shc.stop == SHC_STOP_STALL, "and stalls again until a word arrives");

    push(0xCAFEF00Du);
    n = shc_run(1000);
    CHECK(g_shc.r[10] == 0xCAFEF00Du, "the retried read got the word");
    CHECK(g_shc.stop == SHC_STOP_IDLE, "then IDLE");
    CHECK(shc_run(1000) == 0 && g_shc.stop == SHC_STOP_IDLE, "stays idle");
}

/* ---- Part 2: Virtual-On's copro program ------------------------------------ */

/* run until the firmware waits for the FIFO with nothing left in it */
static void von_run(void)
{
    for (int guard = 0; guard < 1000; guard++) {
        shc_run(100000);
        if (g_shc.stop == SHC_STOP_FAULT || g_shc.stop == SHC_STOP_IDLE)
            return;
        if (g_shc.stop != SHC_STOP_BUDGET && fin_head == fin_tail)
            return;
    }
}

static uint32_t von_dm(uint32_t a) { return g_shc.blk[(a & 0x18000) ? 1 : 0][a & 0x7fff]; }

/* the table index of the handler at PM address pc, or -1 */
static int von_index(uint32_t pc)
{
    for (int i = 0; i < 0x100; i++)
        if (von_dm(0x30000 + (uint32_t)i) == pc)
            return i;
    return -1;
}

/* command: index, then argc words; returns the first output word */
static uint32_t von_cmd(int index, int argc, const uint32_t *argv)
{
    fout_n = 0;
    push((uint32_t)index);
    for (int i = 0; i < argc; i++)
        push(argv[i]);
    von_run();
    return fout_n ? fout[0] : 0xDEADBEEFu;
}

static float von_f2(int index, float a, float b)
{
    uint32_t const argv[2] = { f2u(a), f2u(b) };
    return u2f(von_cmd(index, 2, argv));
}

static bool von_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    board_reset();
    uint8_t hw[2];
    uint32_t n = 0;
    while (fread(hw, 1, 2, f) == 2)
        shc_boot_write(n++, (uint16_t)(hw[0] | (hw[1] << 8)));
    fclose(f);
    printf("      %u halfwords uploaded\n", (unsigned)n);
    shc_boot();
    return n == 11038;
}

/* row k of the top: N's row k times M, the last row adds M's translation */
static bool von_mul_ok(const float *m, const float *nn)
{
    bool same = true;
    for (int k = 0; k < 4; k++)
        for (int j = 0; j < 3; j++) {
            float e = nn[3 * k] * m[j] + nn[3 * k + 1] * m[3 + j] + nn[3 * k + 2] * m[6 + j];
            if (k == 3)
                e += m[9 + j];
            same = same && u2f(von_dm(0x30200 + (uint32_t)(3 * k + j))) == e;
        }
    return same;
}

static void test_von_matrix(void)
{
    int const ld = von_index(0x20199), mul = von_index(0x201C4);
    int const psh = von_index(0x2016D), pop = von_index(0x2018F);
    printf("      load %d, multiply %d, push %d, pop %d\n", ld, mul, psh, pop);
    CHECK(ld >= 0 && mul >= 0 && psh >= 0 && pop >= 0, "matrix handlers in the table");
    if (ld < 0 || mul < 0 || psh < 0 || pop < 0)
        return;

    /* M: rows of a 3x3 (column j of row k at 3k + j) and a translation */
    static const float m[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 20, 30 };
    static const float nn[12] = { 2, 0, 1, 0, 3, 0, 1, 1, 1, 1, 2, 3 };
    uint32_t a[12];
    for (int i = 0; i < 12; i++)
        a[i] = f2u(m[i]);
    von_cmd(ld, 12, a);
    bool same = true;
    for (int i = 0; i < 12; i++)
        same = same && von_dm(0x30200 + (uint32_t)i) == a[i];
    CHECK(same, "matrix load fills the top of the stack (0x30200)");

    for (int i = 0; i < 12; i++)
        a[i] = f2u(nn[i]);
    von_cmd(mul, 12, a);
    CHECK(von_mul_ok(m, nn), "matrix multiply N * M (+ translation)");

    von_cmd(psh, 0, NULL);
    CHECK(von_dm(0x30101) == 0x3020C && von_dm(0x30100) == 1, "push: pointer + 12, depth 1");
    CHECK(von_dm(0x3020C) == von_dm(0x30200) && von_dm(0x30217) == von_dm(0x3020B),
          "push copies the top");
    von_cmd(pop, 0, NULL);
    CHECK(von_dm(0x30101) == 0x30200 && von_dm(0x30100) == 0, "pop: back to 0x30200");
}

static void test_von(void)
{
    const char *path = getenv("SHC_VON_IMAGE");
    if (!path || !*path) {
        printf("skip: SHC_VON_IMAGE is not set (Virtual-On's copro upload)\n");
        return;
    }
    if (!von_load(path)) {
        printf("skip: %s is missing or not 11038 halfwords\n", path);
        return;
    }
    von_run();
    CHECK(g_shc.stop == SHC_STOP_SPIN && g_shc.pc == 0x2011E, "boots to the sync wait (0x2011E)");
    push(0x00000008u);
    von_run();
    CHECK(g_shc.stop == SHC_STOP_SPIN && g_shc.pc == 0x20129, "synced: command loop spins on its DO UNTIL NOP (0x20129)");
    CHECK(von_dm(0x30101) == 0x30200 && von_dm(0x30100) == 0, "sync reset the matrix stack");
    CHECK(von_dm(0x30000) == 0x20133 && von_dm(0x30001) == 0x2013B, "table: 0 fadd, 1 fsub");

    CHECK(von_f2(0, 1.5f, 2.25f) == 3.75f, "cmd 0 fadd 1.5 + 2.25 = 3.75");
    CHECK(fout_n == 1, "one word out");
    CHECK(von_f2(1, 5.0f, 2.0f) == 3.0f, "cmd 1 fsub 5 - 2 = 3");
    int const fmul = von_index(0x20143), fdiv = von_index(0x2014B);
    CHECK(fmul >= 0 && von_f2(fmul, -1.5f, 4.0f) == -6.0f, "fmul -1.5 * 4 = -6");
    float const q = fdiv >= 0 ? von_f2(fdiv, 7.0f, 2.0f) : 0.0f;
    CHECK(fabsf(q - 3.5f) <= 1e-6f, "fdiv 7 / 2 = 3.5 (RECIPS + Newton)");
    float const q3 = fdiv >= 0 ? von_f2(fdiv, 1.0f, 3.0f) : 0.0f;
    CHECK(fabsf(q3 - 1.0f / 3.0f) <= 1e-6f, "fdiv 1 / 3");

    int const at = von_index(0x20211);  /* fix(f(y, x) * 65536 / 2pi): atan2 */
    if (at >= 0) {
        uint32_t const one[2] = { f2u(1.0f), f2u(1.0f) };
        uint32_t const r = von_cmd(at, 2, one);
        printf("      cmd %d (0x20211) (1, 1) -> %d\n", at, (int)r);
        CHECK(r >= 8191 && r <= 8193, "angle of (1, 1) is 0x2000 (45 degrees)");
    }

    /* 0x1f takes six floats and ends in x * 1/sqrt(x); all zeros is NaN in
     * MAME (RSQRTS(0) seeds 2^63, the Newton step's square overflows to inf,
     * inf * 0), and Virtual-On's tutorial shadow relies on it (Pinboard #946) */
    uint32_t const zero6[6] = { 0 };
    CHECK(von_cmd(0x1f, 6, zero6) == 0xFFFFFFFFu, "cmd 0x1f of zeros is NaN 0xFFFFFFFF, as in MAME");

    test_von_matrix();
    CHECK(g_shc.stop == SHC_STOP_SPIN && !g_shc.fault, "back in the command loop, no fault");
}

static void test_rsqrts(void)
{
    g_shc.r[1] = 0;
    shc_op_rsqrts(0, 1, 0);
    CHECK(g_shc.r[0] == 0x5F350000u && (g_shc.astat & SHC_AV), "RSQRTS +0 seeds 2^63 * 1.41 (MAME's table), AV");
    g_shc.r[1] = 0x80000000u;
    shc_op_rsqrts(0, 1, 0);
    CHECK(g_shc.r[0] == 0xDF350000u && (g_shc.astat & SHC_AI), "RSQRTS -0 seeds -2^63 * 1.41, AI");
    g_shc.r[1] = f2u(4.0f);
    shc_op_rsqrts(0, 1, 0);
    CHECK(g_shc.r[0] == 0x3EFF8000u, "RSQRTS 4 seeds 0.4995 (MAME's table)");
}

/* MODE1.TRUNC as MAME runs it (Pinboard #949): float add, multiply and
 * FLOAT round to nearest, FIX rounds down, the multifunction FIX toward zero,
 * and a multiply that overflows is infinity */
static void test_trunc_rounding(void)
{
    uint32_t const mode1 = g_shc.mode1;
    g_shc.mode1 |= SHC_M1_TRUNC;
    g_shc.r[1] = 16777219u;
    shc_op_float(0, 1, 0);
    CHECK(g_shc.r[0] == f2u(16777220.0f), "TRUNC: FLOAT 16777219 rounds to nearest (16777220)");
    g_shc.r[1] = f2u(1.0f);
    g_shc.r[2] = f2u(0x1.8p-24f);
    shc_op_fadd(0, 1, 2);
    CHECK(g_shc.r[0] == 0x3F800001u, "TRUNC: 1 + 1.5 ulp/2 rounds to nearest (1 + 1 ulp)");
    CHECK(shc_fmul_bits(f2u(0x1p100f), f2u(0x1p100f)) == 0x7F800000u && (g_shc.astat & SHC_MV),
          "TRUNC: fmul overflow is +inf, MV");
    CHECK(shc_fmul_bits(f2u(0x1p-70f), f2u(0x1p-70f)) == 0x00000001u << 9 && (g_shc.astat & SHC_MU),
          "TRUNC: fmul keeps a denormal product, MU");
    CHECK(shc_fix_bits(f2u(-1.5f), false) == (uint32_t)-2, "TRUNC: FIX -1.5 = -2 (down)");
    CHECK(shc_fix_bits(f2u(-1.5f), true) == (uint32_t)-1, "TRUNC: multifunction FIX -1.5 = -1 (toward zero)");
    g_shc.mode1 = mode1;
}

int main(void)
{
    test_mini();
    test_rsqrts();
    test_trunc_rounding();
    test_von();
    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
