/*
 * m68k_opcode_test.c — standalone unit tests for the MC68000 core
 * (src/board/m68k_exec.h).  Plants opcode words in a flat big-endian memory,
 * runs one instruction via m68k_step(), and asserts register/memory/SP results.
 *
 * Exists because two serious decode bugs shipped undetected in this core:
 *   - MOVEM pre-decrement used the wrong reversed register-mask order
 *     (d5/d6/d7 never saved/restored).
 *   - JMP was decoded as JSR (mask 0xFF80 vs 0xFFC0), so every jump pushed a
 *     return address and leaked the stack.
 * Both have dedicated regression tests below.
 *
 * Build/run:  tests\build_m68k_test.ps1   (or see that script for the cl cmd)
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* Shim out log.h (it pulls in windows.h); m68k_exec.h only uses LOG_* macros. */
#define LOG_H
#define LOG_INFO(...)  ((void)0)
#define LOG_WARN(...)  ((void)0)
#define LOG_ERROR(...) ((void)0)

#include "m68k_exec.h"

/* ---- flat big-endian test memory + bus callbacks ------------------------- */
static uint8_t MEM[0x1000000]; /* 16 MB, 24-bit address space */

static uint32_t t_read(void *ctx, uint32_t a, int sz) {
    (void)ctx; a &= 0xFFFFFF; uint32_t v = 0;
    for (int i = 0; i < sz; i++) v = (v << 8) | MEM[(a + (uint32_t)i) & 0xFFFFFF];
    return v;
}
static void t_write(void *ctx, uint32_t a, uint32_t val, int sz) {
    (void)ctx; a &= 0xFFFFFF;
    for (int i = 0; i < sz; i++) MEM[(a + (uint32_t)(sz - 1 - i)) & 0xFFFFFF] = (uint8_t)(val >> (i * 8));
}

static m68k_state_t S;

static void setup(void) {
    memset(&S, 0, sizeof S);
    S.read_cb = t_read; S.write_cb = t_write; S.mem_ctx = NULL;
    S.cpu.sr = M68K_SR_S | (7u << M68K_SR_IPL_SHIFT); /* supervisor */
}
static void w16(uint32_t a, uint16_t v) { t_write(NULL, a, v, 2); }
static void w32(uint32_t a, uint32_t v) { t_write(NULL, a, v, 4); }
static uint32_t r32(uint32_t a) { return t_read(NULL, a, 4); }

/* ---- tiny assert framework ---------------------------------------------- */
static int g_pass = 0, g_fail = 0;
#define CHECK(name, cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s\n", name); } \
} while (0)
#define CHECKEQ(name, got, exp) do { \
    uint32_t _g = (uint32_t)(got), _e = (uint32_t)(exp); \
    if (_g == _e) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s  got=0x%08X exp=0x%08X\n", name, _g, _e); } \
} while (0)

/* ========================================================================= */

static void test_jmp_an(void) {
    /* jmp (a0) = 0x4ED0 — must NOT push; SP unchanged. (regression: JMP-as-JSR) */
    setup();
    S.cpu.pc = 0x1000; S.cpu.a[0] = 0x2000; S.cpu.a[7] = 0x8000;
    w16(0x1000, 0x4ED0);
    m68k_step(&S);
    CHECKEQ("jmp (a0): pc", S.cpu.pc, 0x2000);
    CHECKEQ("jmp (a0): SP unchanged (no push)", S.cpu.a[7], 0x8000);
}

static void test_jmp_pcrel(void) {
    /* jmp (d16,pc) = 0x4EFA, disp +0x10 — no push. */
    setup();
    S.cpu.pc = 0x1000; S.cpu.a[7] = 0x8000;
    w16(0x1000, 0x4EFA); w16(0x1002, 0x0010);
    m68k_step(&S);
    CHECKEQ("jmp (d16,pc): pc", S.cpu.pc, 0x1002 + 0x10);
    CHECKEQ("jmp (d16,pc): SP unchanged", S.cpu.a[7], 0x8000);
}

static void test_jsr_an(void) {
    /* jsr (a0) = 0x4E90 — push return addr (0x1002), SP-4, pc=a0. */
    setup();
    S.cpu.pc = 0x1000; S.cpu.a[0] = 0x2000; S.cpu.a[7] = 0x8000;
    w16(0x1000, 0x4E90);
    m68k_step(&S);
    CHECKEQ("jsr (a0): pc", S.cpu.pc, 0x2000);
    CHECKEQ("jsr (a0): SP-4", S.cpu.a[7], 0x7FFC);
    CHECKEQ("jsr (a0): return addr on stack", r32(0x7FFC), 0x1002);
}

static void test_rts(void) {
    /* rts = 0x4E75 — pop pc, SP+4. */
    setup();
    S.cpu.pc = 0x1000; S.cpu.a[7] = 0x7FFC; w32(0x7FFC, 0x00003000);
    w16(0x1000, 0x4E75);
    m68k_step(&S);
    CHECKEQ("rts: pc popped", S.cpu.pc, 0x3000);
    CHECKEQ("rts: SP+4", S.cpu.a[7], 0x8000);
}

static void test_jsr_rts_roundtrip(void) {
    /* jsr to a stub that immediately rts — SP must return to start. */
    setup();
    S.cpu.pc = 0x1000; S.cpu.a[0] = 0x2000; S.cpu.a[7] = 0x8000;
    w16(0x1000, 0x4E90);   /* jsr (a0) -> 0x2000 */
    w16(0x2000, 0x4E75);   /* rts */
    m68k_step(&S);         /* jsr */
    m68k_step(&S);         /* rts */
    CHECKEQ("jsr/rts roundtrip: pc back", S.cpu.pc, 0x1002);
    CHECKEQ("jsr/rts roundtrip: SP balanced", S.cpu.a[7], 0x8000);
}

static void test_movem_predec_layout(void) {
    /* movem.l d0-d7,-(sp): D0 must land at the LOWEST address (regression). */
    setup();
    for (int i = 0; i < 8; i++) S.cpu.d[i] = 0x11111111u * (uint32_t)(i + 1);
    S.cpu.pc = 0x1000; S.cpu.a[7] = 0x8020;
    w16(0x1000, 0x48E7); w16(0x1002, 0xFF00); /* movem.l d0-d7,-(sp) (predec mask) */
    m68k_step(&S);
    CHECKEQ("movem predec: SP-32", S.cpu.a[7], 0x8000);
    CHECKEQ("movem predec: D0 at lowest addr", r32(0x8000), 0x11111111);
    CHECKEQ("movem predec: D7 at highest addr", r32(0x801C), 0x88888888);
}

static void test_movem_roundtrip(void) {
    /* movem.l d0-a4,-(sp) then movem.l (sp)+,d0-a4 — preserve regs + SP.
       (regression: d5/d6/d7 were dropped by the broken predec mapping.) */
    setup();
    uint32_t dvals[8], avals[8];
    for (int i = 0; i < 8; i++) { dvals[i] = 0xD0000000u + (uint32_t)i; S.cpu.d[i] = dvals[i]; }
    for (int i = 0; i < 8; i++) { avals[i] = 0xA0000000u + (uint32_t)i; S.cpu.a[i] = avals[i]; }
    S.cpu.a[7] = 0x8000; avals[7] = 0x8000;
    S.cpu.pc = 0x1000;
    w16(0x1000, 0x48E7); w16(0x1002, 0xFFF8); /* movem.l d0-a4,-(sp) */
    /* clobber d0-a4 between save and restore */
    m68k_step(&S);
    for (int i = 0; i < 8; i++) S.cpu.d[i] = 0xDEAD0000u + (uint32_t)i;
    for (int i = 0; i < 5; i++) S.cpu.a[i] = 0xBEEF0000u + (uint32_t)i;
    S.cpu.pc = 0x1004;
    w16(0x1004, 0x4CDF); w16(0x1006, 0x1FFF); /* movem.l (sp)+,d0-a4 */
    m68k_step(&S);
    int ok = 1;
    for (int i = 0; i < 8; i++) if (S.cpu.d[i] != dvals[i]) ok = 0;
    for (int i = 0; i < 5; i++) if (S.cpu.a[i] != avals[i]) ok = 0;
    CHECK("movem d0-a4 roundtrip: all regs restored (incl d5-d7)", ok);
    CHECKEQ("movem d0-a4 roundtrip: SP balanced", S.cpu.a[7], 0x8000);
}

static void test_link_unlk(void) {
    /* link a6,#-8 ; unlk a6 — SP/a6 round-trip. */
    setup();
    S.cpu.a[6] = 0xCAFE0000; S.cpu.a[7] = 0x8000; S.cpu.pc = 0x1000;
    w16(0x1000, 0x4E56); w16(0x1002, 0xFFF8); /* link a6,#-8 */
    m68k_step(&S);
    CHECKEQ("link: SP = old-4-8", S.cpu.a[7], 0x8000 - 4 - 8);
    CHECKEQ("link: a6 = frame ptr", S.cpu.a[6], 0x8000 - 4);
    w16(0x1004, 0x4E5E); S.cpu.pc = 0x1004; /* unlk a6 */
    m68k_step(&S);
    CHECKEQ("unlk: SP restored", S.cpu.a[7], 0x8000);
    CHECKEQ("unlk: a6 restored", S.cpu.a[6], 0xCAFE0000);
}

static void test_moveq(void) {
    setup();
    S.cpu.pc = 0x1000; w16(0x1000, 0x76FF); /* moveq #-1,d3 */
    m68k_step(&S);
    CHECKEQ("moveq #-1,d3", S.cpu.d[3], 0xFFFFFFFF);
    CHECK("moveq sets N", (S.cpu.sr & M68K_SR_N) != 0);
    CHECK("moveq clears Z", (S.cpu.sr & M68K_SR_Z) == 0);
}

static void test_addq(void) {
    setup();
    S.cpu.d[0] = 0x0FFE; S.cpu.pc = 0x1000; w16(0x1000, 0x5440); /* addq.w #2,d0 */
    m68k_step(&S);
    CHECKEQ("addq.w #2,d0", S.cpu.d[0] & 0xFFFF, 0x1000);
}

static void test_lea(void) {
    setup();
    S.cpu.a[1] = 0x2000; S.cpu.pc = 0x1000;
    w16(0x1000, 0x43E9); w16(0x1002, 0x0010); /* lea (0x10,a1),a1 */
    m68k_step(&S);
    CHECKEQ("lea (d16,a1),a1", S.cpu.a[1], 0x2010);
}

static void test_dbf(void) {
    /* dbf d0,loop with d0=2: branches twice (d0 2->1->0), then exits at d0=-1. */
    setup();
    S.cpu.d[0] = 2; S.cpu.pc = 0x1000;
    w16(0x1000, 0x51C8); w16(0x1002, 0xFFFE); /* dbf d0,(*) -> back to 0x1000 */
    int iters = 0;
    while (S.cpu.pc == 0x1000 && iters < 100) { m68k_step(&S); iters++; if (S.cpu.pc == 0x1000) continue; else break; }
    /* After exit, d0 low word must be 0xFFFF and we took exactly 3 executions. */
    CHECKEQ("dbf: d0 low word = -1 at exit", S.cpu.d[0] & 0xFFFF, 0xFFFF);
    CHECKEQ("dbf: iterations (2->1->0->exit)", iters, 3);
}

static void test_swap(void) {
    /* swap d6 = 0x4846 — swap halfwords; must NOT touch SP (regression: SWAP-as-PEA). */
    setup();
    S.cpu.d[6] = 0x12345678; S.cpu.a[7] = 0x8000; S.cpu.pc = 0x1000;
    w16(0x1000, 0x4846);
    m68k_step(&S);
    CHECKEQ("swap d6: halfwords swapped", S.cpu.d[6], 0x56781234);
    CHECKEQ("swap d6: SP unchanged (not PEA)", S.cpu.a[7], 0x8000);
}

static void test_pea(void) {
    /* pea (a0) = 0x4850 — push the effective address (a0), SP-4. */
    setup();
    S.cpu.a[0] = 0x00123456; S.cpu.a[7] = 0x8000; S.cpu.pc = 0x1000;
    w16(0x1000, 0x4850);
    m68k_step(&S);
    CHECKEQ("pea (a0): SP-4", S.cpu.a[7], 0x7FFC);
    CHECKEQ("pea (a0): EA pushed", r32(0x7FFC), 0x00123456);
}

static void test_move_b_w_l(void) {
    setup();
    S.cpu.d[0] = 0x12345678; S.cpu.pc = 0x1000;
    w16(0x1000, 0x3200); /* move.w d0,d1 */
    m68k_step(&S);
    CHECKEQ("move.w d0,d1", S.cpu.d[1] & 0xFFFF, 0x5678);
}

static void test_addr_modes(void) {
    /* MOVE.L through every common src addressing mode — exercises EA decode
       used by the driver's RAM-copy/voice routines. */
    setup(); w32(0x4000, 0x11223344);
    S.cpu.a[0]=0x4000; S.cpu.pc=0x1000; w16(0x1000,0x2010); m68k_step(&S);  /* (a0) */
    CHECKEQ("move.l (a0),d0", S.cpu.d[0], 0x11223344);

    setup(); w32(0x4000,0x11223344); S.cpu.a[0]=0x4000; S.cpu.pc=0x1000; w16(0x1000,0x2018); m68k_step(&S); /* (a0)+ */
    CHECKEQ("move.l (a0)+,d0 val", S.cpu.d[0], 0x11223344);
    CHECKEQ("move.l (a0)+,d0 a0+=4", S.cpu.a[0], 0x4004);

    setup(); w32(0x4000,0xAABBCCDD); S.cpu.a[0]=0x4004; S.cpu.pc=0x1000; w16(0x1000,0x2020); m68k_step(&S); /* -(a0) */
    CHECKEQ("move.l -(a0),d0 val", S.cpu.d[0], 0xAABBCCDD);
    CHECKEQ("move.l -(a0),d0 a0-=4", S.cpu.a[0], 0x4000);

    setup(); w32(0x4004,0xDEADBEEF); S.cpu.a[0]=0x4000; S.cpu.pc=0x1000;
    w16(0x1000,0x2028); w16(0x1002,0x0004); m68k_step(&S);                  /* (4,a0) */
    CHECKEQ("move.l (4,a0),d0", S.cpu.d[0], 0xDEADBEEF);

    setup(); w32(0x4004,0xCAFEF00D); S.cpu.a[0]=0x4000; S.cpu.d[1]=2; S.cpu.pc=0x1000;
    w16(0x1000,0x2030); w16(0x1002,0x1002); m68k_step(&S);                  /* (2,a0,d1.w) */
    CHECKEQ("move.l (2,a0,d1.w),d0", S.cpu.d[0], 0xCAFEF00D);

    setup(); w32(0x4000,0x01020304); S.cpu.pc=0x1000; w16(0x1000,0x2038); w16(0x1002,0x4000); m68k_step(&S); /* (xxx).w */
    CHECKEQ("move.l (xxx).w,d0", S.cpu.d[0], 0x01020304);

    setup(); w32(0x14000,0x0A0B0C0D); S.cpu.pc=0x1000; w16(0x1000,0x2039); w32(0x1002,0x00014000); m68k_step(&S); /* (xxx).l */
    CHECKEQ("move.l (xxx).l,d0", S.cpu.d[0], 0x0A0B0C0D);

    setup(); S.cpu.pc=0x1000; w16(0x1000,0x203C); w32(0x1002,0x12345678); m68k_step(&S); /* #imm */
    CHECKEQ("move.l #imm,d0", S.cpu.d[0], 0x12345678);

    setup(); S.cpu.pc=0x1000; w16(0x1000,0x203A); w16(0x1002,0x0010); w32(0x1012,0x55667788); m68k_step(&S); /* (d16,pc): EA=0x1002+0x10 */
    CHECKEQ("move.l (d16,pc),d0", S.cpu.d[0], 0x55667788);
}

static void test_movem_postinc(void) {
    /* movem.l (a0)+,d0-d3 — load 4 regs ascending, a0+=16. */
    setup();
    w32(0x4000,0xA0); w32(0x4004,0xA1); w32(0x4008,0xA2); w32(0x400C,0xA3);
    S.cpu.a[0]=0x4000; S.cpu.pc=0x1000; w16(0x1000,0x4CD8); w16(0x1002,0x000F);
    m68k_step(&S);
    int ok = S.cpu.d[0]==0xA0 && S.cpu.d[1]==0xA1 && S.cpu.d[2]==0xA2 && S.cpu.d[3]==0xA3;
    CHECK("movem.l (a0)+,d0-d3 values", ok);
    CHECKEQ("movem.l (a0)+,d0-d3 a0+=16", S.cpu.a[0], 0x4010);
}

static void test_addx(void) {
    /* addx.l d1,d0 with X=1: 0x0000FFFF + 0 + 1 = 0x00010000. */
    setup();
    S.cpu.d[0]=0x0000FFFF; S.cpu.d[1]=0; S.cpu.sr |= M68K_SR_X; S.cpu.pc=0x1000;
    w16(0x1000,0xD181); m68k_step(&S);
    CHECKEQ("addx.l d1,d0 (X=1)", S.cpu.d[0], 0x00010000);
}

static void test_mul_div(void) {
    setup(); S.cpu.d[0]=0x8000; S.cpu.pc=0x1000; w16(0x1000,0xC0FC); w16(0x1002,0x0002); m68k_step(&S); /* mulu.w #2,d0 */
    CHECKEQ("mulu.w #2,d0", S.cpu.d[0], 0x10000);

    setup(); S.cpu.d[0]=0xFFFF; S.cpu.pc=0x1000; w16(0x1000,0xC1FC); w16(0x1002,0x0002); m68k_step(&S); /* muls.w #2,d0 (-1*2=-2) */
    CHECKEQ("muls.w #2,d0 (-1*2)", S.cpu.d[0], 0xFFFFFFFE);

    setup(); S.cpu.d[0]=0x00010000; S.cpu.pc=0x1000; w16(0x1000,0x80FC); w16(0x1002,0x0002); m68k_step(&S); /* divu.w #2,d0 */
    CHECKEQ("divu.w #2,d0 (q=0x8000,r=0)", S.cpu.d[0], 0x00008000);

    setup(); S.cpu.d[0]=0x0000000A; S.cpu.pc=0x1000; w16(0x1000,0x80FC); w16(0x1002,0x0003); m68k_step(&S); /* 10/3 = 3 r1 */
    CHECKEQ("divu.w #3,d0 (q=3,r=1)", S.cpu.d[0], 0x00010003);
}

static void test_bit_ops(void) {
    setup(); S.cpu.d[0]=0x00000008; S.cpu.pc=0x1000; w16(0x1000,0x0800); w16(0x1002,0x0003); m68k_step(&S); /* btst #3,d0 */
    CHECK("btst #3,d0 (bit set -> Z=0)", (S.cpu.sr & M68K_SR_Z)==0);
    setup(); S.cpu.d[0]=0x00000000; S.cpu.pc=0x1000; w16(0x1000,0x0800); w16(0x1002,0x0003); m68k_step(&S);
    CHECK("btst #3,d0 (bit clear -> Z=1)", (S.cpu.sr & M68K_SR_Z)!=0);
    setup(); S.cpu.d[0]=0x00000000; S.cpu.pc=0x1000; w16(0x1000,0x08C0); w16(0x1002,0x0005); m68k_step(&S); /* bset #5,d0 */
    CHECKEQ("bset #5,d0", S.cpu.d[0], 0x00000020);
    setup(); S.cpu.d[0]=0xFFFFFFFF; S.cpu.pc=0x1000; w16(0x1000,0x0880); w16(0x1002,0x0004); m68k_step(&S); /* bclr #4,d0 */
    CHECKEQ("bclr #4,d0", S.cpu.d[0], 0xFFFFFFEF);
    setup(); S.cpu.d[0]=0x00000000; S.cpu.pc=0x1000; w16(0x1000,0x0840); w16(0x1002,0x0001); m68k_step(&S); /* bchg #1,d0 */
    CHECKEQ("bchg #1,d0", S.cpu.d[0], 0x00000002);
}

static void test_shifts(void) {
    setup(); S.cpu.d[0]=0x00000001; S.cpu.pc=0x1000; w16(0x1000,0xE388); m68k_step(&S); /* lsl.l #1,d0 */
    CHECKEQ("lsl.l #1,d0", S.cpu.d[0], 0x00000002);
    setup(); S.cpu.d[0]=0x80000000; S.cpu.pc=0x1000; w16(0x1000,0xE288); m68k_step(&S); /* lsr.l #1,d0 */
    CHECKEQ("lsr.l #1,d0", S.cpu.d[0], 0x40000000);
    setup(); S.cpu.d[0]=0x80000000; S.cpu.pc=0x1000; w16(0x1000,0xE280); m68k_step(&S); /* asr.l #1,d0 (sign-extend) */
    CHECKEQ("asr.l #1,d0 (sign)", S.cpu.d[0], 0xC0000000);
    setup(); S.cpu.d[0]=0x00000001; S.cpu.pc=0x1000; w16(0x1000,0xE398); m68k_step(&S); /* rol.l #1,d0 */
    CHECKEQ("rol.l #1,d0", S.cpu.d[0], 0x00000002);
    setup(); S.cpu.d[0]=0x00000001; S.cpu.pc=0x1000; w16(0x1000,0xE298); m68k_step(&S); /* ror.l #1,d0 -> 0x80000000 */
    CHECKEQ("ror.l #1,d0 (wrap)", S.cpu.d[0], 0x80000000);
}

static void test_exg(void) {
    setup();
    S.cpu.d[0]=0x11111111; S.cpu.d[1]=0x22222222; S.cpu.pc=0x1000; w16(0x1000,0xC141); m68k_step(&S); /* exg d0,d1 */
    CHECKEQ("exg d0,d1 -> d0", S.cpu.d[0], 0x22222222);
    CHECKEQ("exg d0,d1 -> d1", S.cpu.d[1], 0x11111111);
}

static void test_scc(void) {
    setup(); S.cpu.sr |= M68K_SR_Z; S.cpu.pc=0x1000; w16(0x1000,0x57C0); m68k_step(&S); /* seq d0 (Z=1) */
    CHECKEQ("seq d0 (Z=1) -> 0xFF", S.cpu.d[0] & 0xFF, 0xFF);
    setup(); S.cpu.sr &= ~M68K_SR_Z; S.cpu.pc=0x1000; w16(0x1000,0x57C0); m68k_step(&S); /* seq d0 (Z=0) */
    CHECKEQ("seq d0 (Z=0) -> 0x00", S.cpu.d[0] & 0xFF, 0x00);
}

static void test_addsub_flags(void) {
    /* addq.b #1,d0 with d0=0x7F -> 0x80: N=1,V=1,C=0,Z=0 */
    setup(); S.cpu.d[0]=0x0000007F; S.cpu.pc=0x1000; w16(0x1000,0x5200); m68k_step(&S);
    CHECKEQ("addq.b #1,0x7F -> 0x80", S.cpu.d[0]&0xFF, 0x80);
    CHECK("addq overflow: V set", (S.cpu.sr & M68K_SR_V)!=0);
    CHECK("addq overflow: N set", (S.cpu.sr & M68K_SR_N)!=0);
    CHECK("addq overflow: C clear", (S.cpu.sr & M68K_SR_C)==0);
    /* subq.b #1,d0 with d0=0x00 -> 0xFF: N=1,C=1,X=1,V=0,Z=0 */
    setup(); S.cpu.d[0]=0x00000000; S.cpu.pc=0x1000; w16(0x1000,0x5300); m68k_step(&S);
    CHECKEQ("subq.b #1,0x00 -> 0xFF", S.cpu.d[0]&0xFF, 0xFF);
    CHECK("subq borrow: C set", (S.cpu.sr & M68K_SR_C)!=0);
    CHECK("subq borrow: X set", (S.cpu.sr & M68K_SR_X)!=0);
    CHECK("subq borrow: N set", (S.cpu.sr & M68K_SR_N)!=0);
}

int main(void) {
    printf("m68k opcode tests\n");
    test_jmp_an();
    test_jmp_pcrel();
    test_jsr_an();
    test_rts();
    test_jsr_rts_roundtrip();
    test_movem_predec_layout();
    test_movem_roundtrip();
    test_link_unlk();
    test_moveq();
    test_addq();
    test_lea();
    test_dbf();
    test_swap();
    test_pea();
    test_move_b_w_l();
    test_addr_modes();
    test_movem_postinc();
    test_addx();
    test_mul_div();
    test_bit_ops();
    test_shifts();
    test_exg();
    test_scc();
    test_addsub_flags();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
