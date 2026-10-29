/*
 * tgpx4_test.c - the Model 2C TGPx4 (src/board/tgpx4.h), with no ROM.
 *
 * A read of the empty input FIFO leaves the instruction undone, and it runs
 * whole once the word comes: one reply for one word, where MAME's stalled pass
 * pushes a second. A wait on IFE ends the run. Plus the ALU bits every build
 * must agree on.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "board/tgpx4.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* Instruction words. The ALU field of each is NOP (7), the multiplier's
 * MA0 * MA0 -> MA0 (0 here). */
#define ALU_NOP    ((uint64_t)7 << 56)
#define MOV(sr, dr) ((uint64_t)1 << 61 | ALU_NOP | (uint64_t)(sr) << 19 | (uint64_t)(dr) << 12)
#define CTL(cop, ef1, ef2) ((uint64_t)2 << 61 | ALU_NOP | (uint64_t)(cop) << 22 | (uint64_t)(ef1) << 16 | (uint64_t)(ef2))
/* MOV2 (one ALU op, NOP): sr bits 31-37, dr 24-30, a 14-bit displacement.
 * MOV3: dr bits 19-25, the 32-bit immediate from bit 27, a 12-bit
 * displacement. The memory operand's mode is bits 0-3, its displacement from
 * bit 7. */
#define MOV2(sr, dr) ((uint64_t)5 << 61 | (uint64_t)7 << 56 | (uint64_t)1 << 41 | (uint64_t)(sr) << 31 | (uint64_t)(dr) << 24)
#define MOV3(dr, imm) ((uint64_t)7 << 61 | (uint64_t)(imm) << 27 | (uint64_t)(dr) << 19)
#define R_AA0 0x08u
#define R_FI  0x31u
#define R_FO0 0x32u

static void push(uint32_t v) {
    g_tgx.fin[(g_tgx.fin_r + g_tgx.fin_n++) & (TGX_FIFO - 1u)] = v;
}

int main(void) {
    /* FI -> AA0: undone while empty, done once a word has come. */
    tgx_reset();
    g_tgx.prog[0] = MOV(R_FI, R_AA0);
    g_tgx.aa[0] = 0xAAAAAAAAu;
    CHECK(!tgx_step(), "a read of the empty FIFO stalls");
    CHECK(g_tgx.pc == 0 && g_tgx.aa[0] == 0xAAAAAAAAu, "and leaves pc and the destination alone");
    push(0x1234u);
    CHECK(tgx_step() && g_tgx.aa[0] == 0x1234u && g_tgx.pc == 1, "the word comes, the move happens");

    /* FI -> FO0: one reply for one word. */
    tgx_reset();
    g_tgx.prog[0] = MOV(R_FI, R_FO0);
    tgx_step();
    CHECK(g_tgx.fout_n == 0, "a stalled move to FO pushes nothing");
    push(7u);
    tgx_step();
    CHECK(g_tgx.fout_n == 1 && g_tgx.fout[0] == 7u, "the move pushes its word once");

    /* An echo loop: wait on IFE, move FI to FO, jump back. */
    tgx_reset();
    g_tgx.prog[0] = CTL(0x10, 21, 0);             /* DBIFE 0 */
    g_tgx.prog[1] = CTL(0x00, 0, 0);              /* NOP (delay slot) */
    g_tgx.prog[2] = MOV(R_FI, R_FO0);
    g_tgx.prog[3] = CTL(0x12, 0, 0);              /* DJMP 0 */
    g_tgx.prog[4] = CTL(0x00, 0, 0);
    g_tgx.halted = false;
    tgx_run();
    CHECK(g_tgx.insns == 2 && g_tgx.budget_stops == 0, "the wait on IFE ends the run after its delay slot");
    push(1u); push(2u); push(3u);
    tgx_run();
    CHECK(g_tgx.fout_n == 3 && g_tgx.fout[0] == 1u && g_tgx.fout[2] == 3u && g_tgx.fin_n == 0,
          "three words in, three replies out, in order");
    CHECK(g_tgx.budget_stops == 0, "and the run ends at the wait, not the budget");

    /* FADD AA0 + 1.0 -> AA1 (control NOP carrying the ALU). */
    tgx_reset();
    g_tgx.prog[0] = (uint64_t)2 << 61 | (uint64_t)0 << 56 | (uint64_t)0 << 52 | (uint64_t)0x1B << 47 | (uint64_t)0x11 << 42;
    g_tgx.aa[0] = f2u(2.0f);
    tgx_step();
    CHECK(u2f(g_tgx.aa[1]) == 3.0f, "FADD reads the float constant 1.0");

    /* The ALU's undefined corners, spelt out. */
    tgx_reset();
    tgx_aluop(0x0E, 0x7FC00000u, 0, 0, 0x10);     /* CFI NaN -> AA0 */
    CHECK(g_tgx.aa[0] == 0x80000000u, "CFI of NaN is x86's 0x80000000");
    tgx_aluop(0x0E, f2u(-3.75f), 0, 0, 0x10);
    CHECK(g_tgx.aa[0] == (uint32_t)-3, "CFI truncates");
    tgx_aluop(0x0F, f2u(300.0f), 0, 0, 0x10);     /* CFIB */
    CHECK(g_tgx.aa[0] == 0xFFu && (g_tgx.st & TGX_AV), "CFIB clamps to 0xFF and sets AV");
    tgx_aluop(0x1E, 0x80000010u, 0, 4, 0x10);     /* ASR */
    CHECK(g_tgx.aa[0] == 0xF8000001u, "ASR keeps the sign");
    tgx_aluop(0x0A, 0, 0, 0, 0x10);               /* FRCP 0 */
    CHECK(isinf(u2f(g_tgx.aa[0])) && (g_tgx.st & TGX_ZD), "FRCP of 0 is infinity and sets ZD");
    tgx_aluop(0x0B, f2u(-1.0f), 0, 0, 0x10);      /* FRSQ -1 */
    CHECK(g_tgx.aa[0] == 0xFFFFFFFFu && (g_tgx.st & TGX_NR), "a NaN result is written as all ones");
    tgx_aluop(0x0C, f2u(1.0f), 0, 0, 0x10);       /* FLOG 1 */
    CHECK(g_tgx.aa[0] == 0u, "FLOG of 1 is 0");
    tgx_aluop(0x0C, f2u(8.0f), 0, 0, 0x10);
    CHECK(fabsf(u2f(g_tgx.aa[0]) - logf(8.0f) / 0.301030f) < 1e-5f, "FLOG is ln over 0.30103, as MAME");

    /* The absolute address mode (EA type 0x0C): the displacement field is the
     * address, no AR. Its AR field names AR0, set to 0x40 here so an added AR
     * would miss by 0x40. MOV1, MOV2 and MOV3, and past 0x400 the external bus
     * at EB (buffer RAM). */
    {
        static uint8_t buf[0x8000u * 4u];
        memset(buf, 0, sizeof buf);
        g_tgx_bufram = buf;
        tgx_reset();
        g_tgx.ar[0] = 0x40u;
        g_tgx.eb = 0x400000u;
        g_tgx.ram_b[0x15] = 0x77u;
        buf[0x3F01u * 4u] = 0x99u;
        g_tgx.prog[0] = MOV(R_FI, 0x40u) | 0xCu | (uint64_t)0x1B << 7;            /* MOV1 FI, A(1BH) */
        g_tgx.prog[1] = MOV(0x60u, R_AA0) | 0xCu | (uint64_t)0x15 << 7;           /* MOV1 B(15H), AA0 */
        g_tgx.prog[2] = MOV2(0x40u, R_AA0 + 1u) | 0xCu | (uint64_t)0x3F01 << 7;   /* MOV2 A(3F01H), AA1 */
        g_tgx.prog[3] = MOV2(R_FI, 0x60u) | 0xCu | (uint64_t)0x2A5 << 7;          /* MOV2 FI, B(2A5H) */
        g_tgx.prog[4] = MOV3(0x40u, 0xCAFEu) | 0xCu | (uint64_t)0xFF0 << 7;       /* MOV3 #CAFE, A(0FF0H) */
        push(0x55u);
        push(0x66u);
        for (int i = 0; i < 5; i++) tgx_step();
        CHECK(g_tgx.ram_a[0x1B] == 0x55u && g_tgx.ram_a[0x5B] == 0u, "MOV1 absolute write: no AR added");
        CHECK(g_tgx.aa[0] == 0x77u, "MOV1 absolute read of B");
        CHECK(g_tgx.aa[1] == 0x99u, "MOV2 absolute read past 400H goes to the external bus");
        CHECK(g_tgx.ram_b[0x2A5] == 0x66u, "MOV2 absolute write to B");
        CHECK(tgx_le32(buf + 0xFF0u * 4u) == 0xCAFEu, "MOV3 absolute write to the external bus");
        CHECK(g_tgx.ar[0] == 0x40u, "and no AR moves");
        g_tgx_bufram = NULL;
    }

    double worst = 0.0;
    for (double x = 1e-30; x < 1e30; x *= 1.37) {
        double e = fabs(tgx_ln(x) - log(x));
        if (e > worst) worst = e;
    }
    CHECK(worst < 1e-12, "tgx_ln agrees with libm");

    printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
