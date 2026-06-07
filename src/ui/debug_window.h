/*
 * debug_window.h — i960 CPU opcode unit-test suite.
 *
 * Builds a small isolated test bus (256-byte scratch), drives individual
 * instructions through i960_step, and checks register / CC outcomes.
 * Ported from the stf-hle reference implementation; covers CTRL, COBR,
 * REG-arith/shift/logic/compare/bits, and MEM instruction groups.
 *
 * Open via Debug → CPU Opcode Tests in the menu bar.
 */
#ifndef DEBUG_WINDOW_H
#define DEBUG_WINDOW_H

#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

#include "cimgui.h"
#include "i960.h"
#include "i960_exec.h"
#include "memory.h"
#include "constants.h"

/* ---- Test bus ------------------------------------------------------------ */

static uint8_t       g_dbg_scratch[256];
static memory_bus_t *g_dbg_bus = NULL;

static void dbg_bus_init(void) {
    if (g_dbg_bus) return;
    g_dbg_bus = (memory_bus_t *)calloc(1, sizeof(memory_bus_t));
    mem_add_region(g_dbg_bus, "dbg_scratch", 0, sizeof(g_dbg_scratch), g_dbg_scratch, 0);
}

/* ---- Result record ------------------------------------------------------- */

typedef struct {
    const char *group;
    const char *name;
    int         passed;
    char        detail[96];
} dbg_test_t;

#define DBG_MAX_TESTS 700
static dbg_test_t g_dbg_tests[DBG_MAX_TESTS];
static int        g_dbg_count  = 0;
static int        g_dbg_passed = 0;
static bool       g_dbg_run    = false;

static void dbg_rec(const char *grp, const char *nm, int ok, const char *fmt, ...) {
    if (g_dbg_count >= DBG_MAX_TESTS) return;
    dbg_test_t *t = &g_dbg_tests[g_dbg_count++];
    t->group  = grp;
    t->name   = nm;
    t->passed = ok;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t->detail, sizeof(t->detail), fmt, ap);
    va_end(ap);
    if (ok) g_dbg_passed++;
}

/* ---- CPU helpers --------------------------------------------------------- */

static i960_cpu_t dbg_mk_cpu(uint32_t cc) {
    i960_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    set_cc(&cpu, cc);
    return cpu;
}

/* Encode instruction words */
static uint32_t enc_reg(int op, int s1, int s2, int dst) {
    return ((uint32_t)(op >> 4) << 24) | ((uint32_t)dst << 19)
         | ((uint32_t)s2 << 14) | ((uint32_t)(op & 0xF) << 7) | (uint32_t)s1;
}
static uint32_t enc_ctrl(int op, int32_t disp) {
    return ((uint32_t)op << 24) | ((uint32_t)disp & 0x00FFFFFC);
}
static uint32_t enc_cobr(int op, int s1, int s2, int32_t disp) {
    return ((uint32_t)op << 24) | ((uint32_t)s1 << 19)
         | ((uint32_t)s2 << 14) | ((uint32_t)disp & 0x1FFC);
}
static uint32_t enc_mema(int op, int dst, uint32_t offset) {
    return ((uint32_t)op << 24) | ((uint32_t)dst << 19) | (offset & 0xFFF);
}

/* Write w1/w2 into scratch at 0 and step from IP 0 */
static void dbg_step(i960_cpu_t *cpu, uint32_t w1, uint32_t w2) {
    memcpy(g_dbg_scratch,     &w1, 4);
    memcpy(g_dbg_scratch + 4, &w2, 4);
    cpu->sfr.ip = 0;
    i960_step(cpu, g_dbg_bus);
}

/* ---- Test groups --------------------------------------------------------- */

static void dbg_tests_ctrl(void) {
    const char *G = "CTRL";
    i960_cpu_t  c;

    c = dbg_mk_cpu(CC_NO);
    dbg_step(&c, enc_ctrl(0x08, 8), 0);
    dbg_rec(G, "b taken", c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);

    c = dbg_mk_cpu(CC_NO);
    dbg_step(&c, enc_ctrl(0x0B, 8), 0);
    dbg_rec(G, "bal target",   c.sfr.ip        == 8, "ip=0x%X",  c.sfr.ip);
    dbg_rec(G, "bal link g14", c.globals.g[14] == 4, "g14=0x%X", c.globals.g[14]);

    c = dbg_mk_cpu(CC_NO);
    dbg_step(&c, enc_ctrl(0x09, 8), 0);
    dbg_rec(G, "call target",      c.sfr.ip     == 8, "ip=0x%X",  c.sfr.ip);
    dbg_rec(G, "call frame_depth", c.frame_depth == 1, "depth=%d", c.frame_depth);
    dbg_rec(G, "call rip saved",   c.locals.rip == 4, "rip=0x%X", c.locals.rip);
    memcpy(g_dbg_scratch + 8, &(uint32_t){enc_ctrl(0x0A, 0)}, 4);
    c.sfr.ip = 8;
    i960_step(&c, g_dbg_bus);
    dbg_rec(G, "ret target",      c.sfr.ip     == 4, "ip=0x%X",  c.sfr.ip);
    dbg_rec(G, "ret frame_depth", c.frame_depth == 0, "depth=%d", c.frame_depth);

    c = dbg_mk_cpu(CC_NO); dbg_step(&c, enc_ctrl(0x10, 8), 0);
    dbg_rec(G, "bno cc=NO→taken", c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);
    c = dbg_mk_cpu(CC_G);  dbg_step(&c, enc_ctrl(0x10, 8), 0);
    dbg_rec(G, "bno cc=G→skip",   c.sfr.ip == 4, "ip=0x%X", c.sfr.ip);

    c = dbg_mk_cpu(CC_G);  dbg_step(&c, enc_ctrl(0x11, 8), 0);
    dbg_rec(G, "bg cc=G→taken",   c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);
    c = dbg_mk_cpu(CC_E);  dbg_step(&c, enc_ctrl(0x11, 8), 0);
    dbg_rec(G, "bg cc=E→skip",    c.sfr.ip == 4, "ip=0x%X", c.sfr.ip);
    c = dbg_mk_cpu(CC_L);  dbg_step(&c, enc_ctrl(0x11, 8), 0);
    dbg_rec(G, "bg cc=L→skip",    c.sfr.ip == 4, "ip=0x%X", c.sfr.ip);

    c = dbg_mk_cpu(CC_E);  dbg_step(&c, enc_ctrl(0x12, 8), 0);
    dbg_rec(G, "be cc=E→taken",   c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);
    c = dbg_mk_cpu(CC_G);  dbg_step(&c, enc_ctrl(0x12, 8), 0);
    dbg_rec(G, "be cc=G→skip",    c.sfr.ip == 4, "ip=0x%X", c.sfr.ip);

    c = dbg_mk_cpu(CC_G);  dbg_step(&c, enc_ctrl(0x13, 8), 0);
    dbg_rec(G, "bge cc=G→taken",  c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);
    c = dbg_mk_cpu(CC_E);  dbg_step(&c, enc_ctrl(0x13, 8), 0);
    dbg_rec(G, "bge cc=E→taken",  c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);
    c = dbg_mk_cpu(CC_L);  dbg_step(&c, enc_ctrl(0x13, 8), 0);
    dbg_rec(G, "bge cc=L→skip",   c.sfr.ip == 4, "ip=0x%X", c.sfr.ip);

    c = dbg_mk_cpu(CC_L);  dbg_step(&c, enc_ctrl(0x14, 8), 0);
    dbg_rec(G, "bl cc=L→taken",   c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);
    c = dbg_mk_cpu(CC_E);  dbg_step(&c, enc_ctrl(0x14, 8), 0);
    dbg_rec(G, "bl cc=E→skip",    c.sfr.ip == 4, "ip=0x%X", c.sfr.ip);

    c = dbg_mk_cpu(CC_G);  dbg_step(&c, enc_ctrl(0x15, 8), 0);
    dbg_rec(G, "bne cc=G→taken",  c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);
    c = dbg_mk_cpu(CC_L);  dbg_step(&c, enc_ctrl(0x15, 8), 0);
    dbg_rec(G, "bne cc=L→taken",  c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);
    c = dbg_mk_cpu(CC_E);  dbg_step(&c, enc_ctrl(0x15, 8), 0);
    dbg_rec(G, "bne cc=E→skip",   c.sfr.ip == 4, "ip=0x%X", c.sfr.ip);
    c = dbg_mk_cpu(CC_NO); dbg_step(&c, enc_ctrl(0x15, 8), 0);
    dbg_rec(G, "bne cc=NO→skip",  c.sfr.ip == 4, "ip=0x%X", c.sfr.ip);

    c = dbg_mk_cpu(CC_L);  dbg_step(&c, enc_ctrl(0x16, 8), 0);
    dbg_rec(G, "ble cc=L→taken",  c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);
    c = dbg_mk_cpu(CC_E);  dbg_step(&c, enc_ctrl(0x16, 8), 0);
    dbg_rec(G, "ble cc=E→taken",  c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);
    c = dbg_mk_cpu(CC_G);  dbg_step(&c, enc_ctrl(0x16, 8), 0);
    dbg_rec(G, "ble cc=G→skip",   c.sfr.ip == 4, "ip=0x%X", c.sfr.ip);

    c = dbg_mk_cpu(CC_G);  dbg_step(&c, enc_ctrl(0x17, 8), 0);
    dbg_rec(G, "bo cc=G→taken",   c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);
    c = dbg_mk_cpu(CC_E);  dbg_step(&c, enc_ctrl(0x17, 8), 0);
    dbg_rec(G, "bo cc=E→taken",   c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);
    c = dbg_mk_cpu(CC_L);  dbg_step(&c, enc_ctrl(0x17, 8), 0);
    dbg_rec(G, "bo cc=L→taken",   c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);
    c = dbg_mk_cpu(CC_NO); dbg_step(&c, enc_ctrl(0x17, 8), 0);
    dbg_rec(G, "bo cc=NO→skip",   c.sfr.ip == 4, "ip=0x%X", c.sfr.ip);
}

static void dbg_tests_cobr(void) {
    const char *G = "COBR";
    i960_cpu_t  c;
    uint32_t    cc;

    c = dbg_mk_cpu(CC_NO); dbg_step(&c, enc_cobr(0x20, 0, 0, 0), 0);
    dbg_rec(G, "testno cc=NO→1", c.locals.r[0] == 1, "r0=%u", c.locals.r[0]);
    c = dbg_mk_cpu(CC_G);  dbg_step(&c, enc_cobr(0x20, 0, 0, 0), 0);
    dbg_rec(G, "testno cc=G→0",  c.locals.r[0] == 0, "r0=%u", c.locals.r[0]);

    c = dbg_mk_cpu(CC_G);  dbg_step(&c, enc_cobr(0x21, 0, 0, 0), 0);
    dbg_rec(G, "testg cc=G→1",   c.locals.r[0] == 1, "r0=%u", c.locals.r[0]);
    c = dbg_mk_cpu(CC_E);  dbg_step(&c, enc_cobr(0x21, 0, 0, 0), 0);
    dbg_rec(G, "testg cc=E→0",   c.locals.r[0] == 0, "r0=%u", c.locals.r[0]);

    c = dbg_mk_cpu(CC_E);  dbg_step(&c, enc_cobr(0x22, 0, 0, 0), 0);
    dbg_rec(G, "teste cc=E→1",   c.locals.r[0] == 1, "r0=%u", c.locals.r[0]);
    c = dbg_mk_cpu(CC_L);  dbg_step(&c, enc_cobr(0x22, 0, 0, 0), 0);
    dbg_rec(G, "teste cc=L→0",   c.locals.r[0] == 0, "r0=%u", c.locals.r[0]);

    c = dbg_mk_cpu(CC_G);  dbg_step(&c, enc_cobr(0x25, 0, 0, 0), 0);
    dbg_rec(G, "testne cc=G→1",  c.locals.r[0] == 1, "r0=%u", c.locals.r[0]);
    c = dbg_mk_cpu(CC_L);  dbg_step(&c, enc_cobr(0x25, 0, 0, 0), 0);
    dbg_rec(G, "testne cc=L→1",  c.locals.r[0] == 1, "r0=%u", c.locals.r[0]);
    c = dbg_mk_cpu(CC_E);  dbg_step(&c, enc_cobr(0x25, 0, 0, 0), 0);
    dbg_rec(G, "testne cc=E→0",  c.locals.r[0] == 0, "r0=%u", c.locals.r[0]);
    c = dbg_mk_cpu(CC_NO); dbg_step(&c, enc_cobr(0x25, 0, 0, 0), 0);
    dbg_rec(G, "testne cc=NO→0", c.locals.r[0] == 0, "r0=%u", c.locals.r[0]);

    c = dbg_mk_cpu(CC_G);  dbg_step(&c, enc_cobr(0x27, 0, 0, 0), 0);
    dbg_rec(G, "testo cc=G→1",   c.locals.r[0] == 1, "r0=%u", c.locals.r[0]);
    c = dbg_mk_cpu(CC_NO); dbg_step(&c, enc_cobr(0x27, 0, 0, 0), 0);
    dbg_rec(G, "testo cc=NO→0",  c.locals.r[0] == 0, "r0=%u", c.locals.r[0]);

    /* bbc: branch if bit clear */
    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0; c.locals.r[1] = 0xFFFFFFFE;
    dbg_step(&c, enc_cobr(0x30, 0, 1, 8), 0);
    dbg_rec(G, "bbc bit0 clear→taken", c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);
    cc = get_cc(&c);
    dbg_rec(G, "bbc bit0 clear→CC_E",  cc == CC_E, "cc=0x%X", cc);
    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0; c.locals.r[1] = 0x00000001;
    dbg_step(&c, enc_cobr(0x30, 0, 1, 8), 0);
    dbg_rec(G, "bbc bit0 set→skip",    c.sfr.ip == 4, "ip=0x%X", c.sfr.ip);
    cc = get_cc(&c);
    dbg_rec(G, "bbc bit0 set→CC_NO",   cc == CC_NO, "cc=0x%X", cc);

    /* bbs: branch if bit set */
    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 3; c.locals.r[1] = 0x00000008;
    dbg_step(&c, enc_cobr(0x37, 0, 1, 8), 0);
    dbg_rec(G, "bbs bit3 set→taken",   c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);
    cc = get_cc(&c);
    dbg_rec(G, "bbs bit3 set→CC_E",    cc == CC_E, "cc=0x%X", cc);
    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 3; c.locals.r[1] = 0x00000000;
    dbg_step(&c, enc_cobr(0x37, 0, 1, 8), 0);
    dbg_rec(G, "bbs bit3 clear→skip",  c.sfr.ip == 4, "ip=0x%X", c.sfr.ip);
    cc = get_cc(&c);
    dbg_rec(G, "bbs bit3 clear→CC_NO", cc == CC_NO, "cc=0x%X", cc);

    /* cmpobe */
    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 5; c.locals.r[1] = 5;
    dbg_step(&c, enc_cobr(0x32, 0, 1, 8), 0);
    dbg_rec(G, "cmpobe eq→taken", c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);
    dbg_rec(G, "cmpobe eq→CC_E",  get_cc(&c) == CC_E, "cc=0x%X", get_cc(&c));
    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 3; c.locals.r[1] = 5;
    dbg_step(&c, enc_cobr(0x32, 0, 1, 8), 0);
    dbg_rec(G, "cmpobe ne→skip",  c.sfr.ip == 4, "ip=0x%X", c.sfr.ip);
    dbg_rec(G, "cmpobe ne→CC_L",  get_cc(&c) == CC_L, "cc=0x%X", get_cc(&c));

    /* cmpobne */
    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 3; c.locals.r[1] = 5;
    dbg_step(&c, enc_cobr(0x35, 0, 1, 8), 0);
    dbg_rec(G, "cmpobne ne→taken", c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);
    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 5; c.locals.r[1] = 5;
    dbg_step(&c, enc_cobr(0x35, 0, 1, 8), 0);
    dbg_rec(G, "cmpobne eq→skip",  c.sfr.ip == 4, "ip=0x%X", c.sfr.ip);

    /* cmpobl */
    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 2; c.locals.r[1] = 5;
    dbg_step(&c, enc_cobr(0x34, 0, 1, 8), 0);
    dbg_rec(G, "cmpobl lt→taken", c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);
    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 5; c.locals.r[1] = 5;
    dbg_step(&c, enc_cobr(0x34, 0, 1, 8), 0);
    dbg_rec(G, "cmpobl eq→skip",  c.sfr.ip == 4, "ip=0x%X", c.sfr.ip);

    /* cmpobg */
    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 7; c.locals.r[1] = 3;
    dbg_step(&c, enc_cobr(0x31, 0, 1, 8), 0);
    dbg_rec(G, "cmpobg gt→taken", c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);

    /* signed COBR */
    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = (uint32_t)-1; c.locals.r[1] = (uint32_t)-1;
    dbg_step(&c, enc_cobr(0x3a, 0, 1, 8), 0);
    dbg_rec(G, "cmpibe -1==-1 taken", c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = (uint32_t)-1; c.locals.r[1] = 0;
    dbg_step(&c, enc_cobr(0x3c, 0, 1, 8), 0);
    dbg_rec(G, "cmpibl -1<0 taken",  c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);
    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0xFFFFFFFF; c.locals.r[1] = 0;
    dbg_step(&c, enc_cobr(0x34, 0, 1, 8), 0);
    dbg_rec(G, "cmpobl 0xFF>0 skip", c.sfr.ip == 4, "ip=0x%X", c.sfr.ip);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0; c.locals.r[1] = 0;
    dbg_step(&c, enc_cobr(0x38, 0, 1, 8), 0);
    dbg_rec(G, "cmpibno never",    c.sfr.ip == 4, "ip=0x%X", c.sfr.ip);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0; c.locals.r[1] = 1;
    dbg_step(&c, enc_cobr(0x3f, 0, 1, 8), 0);
    dbg_rec(G, "cmpibo always",    c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);
}

static void dbg_tests_reg_arith(void) {
    const char *G = "Arith";
    i960_cpu_t  c;

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 3; c.locals.r[1] = 7;
    dbg_step(&c, enc_reg(0x590, 0, 1, 2), 0);
    dbg_rec(G, "addo 3+7=10",       c.locals.r[2] == 10, "got=%u", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 1; c.locals.r[1] = 0xFFFFFFFF;
    dbg_step(&c, enc_reg(0x590, 0, 1, 2), 0);
    dbg_rec(G, "addo wrap 0xFF+1",  c.locals.r[2] == 0, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 3; c.locals.r[1] = 10;
    dbg_step(&c, enc_reg(0x592, 0, 1, 2), 0);
    dbg_rec(G, "subo 10-3=7",       c.locals.r[2] == 7, "got=%u", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = (uint32_t)-3; c.locals.r[1] = (uint32_t)-4;
    dbg_step(&c, enc_reg(0x591, 0, 1, 2), 0);
    dbg_rec(G, "addi -3+(-4)=-7",   (int32_t)c.locals.r[2] == -7, "got=%d", (int32_t)c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 10; c.locals.r[1] = 3;
    dbg_step(&c, enc_reg(0x593, 0, 1, 2), 0);
    dbg_rec(G, "subi 3-10=-7",      (int32_t)c.locals.r[2] == -7, "got=%d", (int32_t)c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 6; c.locals.r[1] = 7;
    dbg_step(&c, enc_reg(0x701, 0, 1, 2), 0);
    dbg_rec(G, "mulo 6*7=42",       c.locals.r[2] == 42, "got=%u", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = (uint32_t)-3; c.locals.r[1] = 5;
    dbg_step(&c, enc_reg(0x741, 0, 1, 2), 0);
    dbg_rec(G, "muli -3*5=-15",     (int32_t)c.locals.r[2] == -15, "got=%d", (int32_t)c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 3; c.locals.r[1] = 9;
    dbg_step(&c, enc_reg(0x70b, 0, 1, 2), 0);
    dbg_rec(G, "divo 9/3=3",        c.locals.r[2] == 3, "got=%u", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = (uint32_t)-3; c.locals.r[1] = (uint32_t)-9;
    dbg_step(&c, enc_reg(0x74b, 0, 1, 2), 0);
    dbg_rec(G, "divi -9/-3=3",      (int32_t)c.locals.r[2] == 3, "got=%d", (int32_t)c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 3; c.locals.r[1] = 10;
    dbg_step(&c, enc_reg(0x708, 0, 1, 2), 0);
    dbg_rec(G, "remo 10%3=1",       c.locals.r[2] == 1, "got=%u", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 3; c.locals.r[1] = (uint32_t)-10;
    dbg_step(&c, enc_reg(0x748, 0, 1, 2), 0);
    dbg_rec(G, "remi -10%3=-1",     (int32_t)c.locals.r[2] == -1, "got=%d", (int32_t)c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 3; c.locals.r[1] = (uint32_t)-10;
    dbg_step(&c, enc_reg(0x749, 0, 1, 2), 0);
    dbg_rec(G, "modi -10 mod 3=2",  (int32_t)c.locals.r[2] == 2, "got=%d", (int32_t)c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0x10000; c.locals.r[1] = 0x10000;
    dbg_step(&c, enc_reg(0x670, 0, 1, 2), 0);
    dbg_rec(G, "emul lo=0",         c.locals.r[2] == 0, "got=0x%X", c.locals.r[2]);
    dbg_rec(G, "emul hi=1",         c.locals.r[3] == 1, "got=0x%X", c.locals.r[3]);

    c = dbg_mk_cpu(CC_E); c.locals.r[0] = 1; c.locals.r[1] = 0xFFFFFFFF;
    dbg_step(&c, enc_reg(0x5b0, 0, 1, 2), 0);
    dbg_rec(G, "addc 0xFF+1+carry=1", c.locals.r[2] == 1, "got=0x%X", c.locals.r[2]);
    dbg_rec(G, "addc carry out=CC_E", get_cc(&c) == CC_E, "cc=0x%X", get_cc(&c));

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 1; c.locals.r[1] = 1;
    dbg_step(&c, enc_reg(0x5b0, 0, 1, 2), 0);
    dbg_rec(G, "addc 1+1=2 no carry", c.locals.r[2] == 2, "got=0x%X", c.locals.r[2]);
    dbg_rec(G, "addc no carry→CC_NO", get_cc(&c) == CC_NO, "cc=0x%X", get_cc(&c));

    c = dbg_mk_cpu(CC_E); c.locals.r[0] = 0; c.locals.r[1] = 5;
    dbg_step(&c, enc_reg(0x5b2, 0, 1, 2), 0);
    dbg_rec(G, "subc 5-0-1+1=5",     c.locals.r[2] == 5, "got=0x%X", c.locals.r[2]);
}

static void dbg_tests_reg_shift(void) {
    const char *G = "Shift";
    i960_cpu_t  c;

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 4; c.locals.r[1] = 0x80000000;
    dbg_step(&c, enc_reg(0x598, 0, 1, 2), 0);
    dbg_rec(G, "shro 0x80000000>>4", c.locals.r[2] == 0x08000000, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0; c.locals.r[1] = 0xDEADBEEF;
    dbg_step(&c, enc_reg(0x598, 0, 1, 2), 0);
    dbg_rec(G, "shro >>0=identity",   c.locals.r[2] == 0xDEADBEEF, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 32; c.locals.r[1] = 0xFFFFFFFF;
    dbg_step(&c, enc_reg(0x598, 0, 1, 2), 0);
    dbg_rec(G, "shro >>32=0",         c.locals.r[2] == 0, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 4; c.locals.r[1] = 0x80000000;
    dbg_step(&c, enc_reg(0x59b, 0, 1, 2), 0);
    dbg_rec(G, "shri ->>4=sign ext",  c.locals.r[2] == 0xF8000000, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 32; c.locals.r[1] = 0x80000000;
    dbg_step(&c, enc_reg(0x59b, 0, 1, 2), 0);
    dbg_rec(G, "shri neg>>32=0xFFF",  c.locals.r[2] == 0xFFFFFFFF, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 4; c.locals.r[1] = 1;
    dbg_step(&c, enc_reg(0x59c, 0, 1, 2), 0);
    dbg_rec(G, "shlo 1<<4=16",        c.locals.r[2] == 16, "got=%u", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 32; c.locals.r[1] = 1;
    dbg_step(&c, enc_reg(0x59c, 0, 1, 2), 0);
    dbg_rec(G, "shlo 1<<32=0",        c.locals.r[2] == 0, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 1; c.locals.r[1] = 0x80000000;
    dbg_step(&c, enc_reg(0x59d, 0, 1, 2), 0);
    dbg_rec(G, "rotate 0x80000000 by 1", c.locals.r[2] == 1, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0; c.locals.r[1] = 0xDEADBEEF;
    dbg_step(&c, enc_reg(0x59d, 0, 1, 2), 0);
    dbg_rec(G, "rotate by 0=identity", c.locals.r[2] == 0xDEADBEEF, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO);
    c.locals.r[0] = 4;
    c.locals.r[1] = 0x00000010;
    c.locals.r[2] = 0x00000001;
    dbg_step(&c, enc_reg(0x59a, 0, 1, 2), 0);
    dbg_rec(G, "shrdi high result",   c.locals.r[2] == 0x10000001, "got=0x%X", c.locals.r[2]);
}

static void dbg_tests_reg_logic(void) {
    const char *G = "Logic";
    i960_cpu_t  c;

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0xF0F0; c.locals.r[1] = 0xFF00;
    dbg_step(&c, enc_reg(0x581, 0, 1, 2), 0);
    dbg_rec(G, "and",    c.locals.r[2] == 0xF000, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0xF0F0; c.locals.r[1] = 0xFF00;
    dbg_step(&c, enc_reg(0x587, 0, 1, 2), 0);
    dbg_rec(G, "or",     c.locals.r[2] == 0xFFF0, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0xF0F0; c.locals.r[1] = 0xFF00;
    dbg_step(&c, enc_reg(0x586, 0, 1, 2), 0);
    dbg_rec(G, "xor",    c.locals.r[2] == 0x0FF0, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0x0000FFFF;
    dbg_step(&c, enc_reg(0x58a, 0, 0, 2), 0);
    dbg_rec(G, "not",    c.locals.r[2] == 0xFFFF0000, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0xF0F0; c.locals.r[1] = 0xFF00;
    dbg_step(&c, enc_reg(0x588, 0, 1, 2), 0);
    dbg_rec(G, "nor",    c.locals.r[2] == ~0xFFF0u, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0xF0F0; c.locals.r[1] = 0xFF00;
    dbg_step(&c, enc_reg(0x58e, 0, 1, 2), 0);
    dbg_rec(G, "nand",   c.locals.r[2] == ~0xF000u, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0xFF; c.locals.r[1] = 0x0F;
    dbg_step(&c, enc_reg(0x582, 0, 1, 2), 0);
    dbg_rec(G, "andnot", c.locals.r[2] == 0x00, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0xFF; c.locals.r[1] = 0xFF00;
    dbg_step(&c, enc_reg(0x584, 0, 1, 2), 0);
    dbg_rec(G, "notand", c.locals.r[2] == 0xFF00, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0; c.locals.r[1] = 0xFFFFFF00;
    dbg_step(&c, enc_reg(0x58b, 0, 1, 2), 0);
    dbg_rec(G, "ornot",  c.locals.r[2] == 0xFF, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0xFFFFFF00; c.locals.r[1] = 0xFF;
    dbg_step(&c, enc_reg(0x58d, 0, 1, 2), 0);
    dbg_rec(G, "notor",  c.locals.r[2] == 0xFF, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0xFF00; c.locals.r[1] = 0xFF00;
    dbg_step(&c, enc_reg(0x589, 0, 1, 2), 0);
    dbg_rec(G, "xnor same→all1", c.locals.r[2] == 0xFFFFFFFF, "got=0x%X", c.locals.r[2]);
}

static void dbg_tests_reg_compare(void) {
    const char *G = "Compare";
    i960_cpu_t  c;
    uint32_t    cc;

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 5; c.locals.r[1] = 3;
    dbg_step(&c, enc_reg(0x5a0, 0, 1, 0), 0); cc = get_cc(&c);
    dbg_rec(G, "cmpo src1>src2→G",  cc == CC_G, "cc=0x%X", cc);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 5; c.locals.r[1] = 5;
    dbg_step(&c, enc_reg(0x5a0, 0, 1, 0), 0); cc = get_cc(&c);
    dbg_rec(G, "cmpo src1=src2→E",  cc == CC_E, "cc=0x%X", cc);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 1; c.locals.r[1] = 5;
    dbg_step(&c, enc_reg(0x5a0, 0, 1, 0), 0); cc = get_cc(&c);
    dbg_rec(G, "cmpo src1<src2→L",  cc == CC_L, "cc=0x%X", cc);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0xFFFFFFFF; c.locals.r[1] = 0;
    dbg_step(&c, enc_reg(0x5a1, 0, 1, 0), 0); cc = get_cc(&c);
    dbg_rec(G, "cmpi -1<0→L",       cc == CC_L, "cc=0x%X", cc);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0; c.locals.r[1] = 0xFFFFFFFF;
    dbg_step(&c, enc_reg(0x5a1, 0, 1, 0), 0); cc = get_cc(&c);
    dbg_rec(G, "cmpi 0>-1→G",       cc == CC_G, "cc=0x%X", cc);

    /* chkbit: CC_E when bit set, CC_NO when clear */
    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0; c.locals.r[1] = 0x00000001;
    dbg_step(&c, enc_reg(0x5ae, 0, 1, 0), 0); cc = get_cc(&c);
    dbg_rec(G, "chkbit bit0 set→E",  cc == CC_E, "cc=0x%X", cc);

    c = dbg_mk_cpu(CC_G);  c.locals.r[0] = 1; c.locals.r[1] = 0x00000001;
    dbg_step(&c, enc_reg(0x5ae, 0, 1, 0), 0); cc = get_cc(&c);
    dbg_rec(G, "chkbit bit1 clr→NO", cc == CC_NO, "cc=0x%X", cc);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 31; c.locals.r[1] = 0x80000000;
    dbg_step(&c, enc_reg(0x5ae, 0, 1, 0), 0); cc = get_cc(&c);
    dbg_rec(G, "chkbit bit31 set→E", cc == CC_E, "cc=0x%X", cc);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 5; c.locals.r[1] = 7;
    dbg_step(&c, enc_reg(0x5a4, 0, 1, 2), 0);
    dbg_rec(G, "cmpinco dst=src2+1", c.locals.r[2] == 8,  "got=%u", c.locals.r[2]);
    dbg_rec(G, "cmpinco cc=L",       get_cc(&c) == CC_L,  "cc=0x%X", get_cc(&c));

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 5; c.locals.r[1] = 7;
    dbg_step(&c, enc_reg(0x5a6, 0, 1, 2), 0);
    dbg_rec(G, "cmpdeco dst=src2-1", c.locals.r[2] == 6,  "got=%u", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0x00AB0000; c.locals.r[1] = 0xABCDEF00;
    dbg_step(&c, enc_reg(0x5ac, 0, 1, 0), 0); cc = get_cc(&c);
    dbg_rec(G, "scanbyte found→E",  cc == CC_E, "cc=0x%X", cc);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0x12345678; c.locals.r[1] = 0xABCDEF00;
    dbg_step(&c, enc_reg(0x5ac, 0, 1, 0), 0); cc = get_cc(&c);
    dbg_rec(G, "scanbyte none→NO",  cc == CC_NO, "cc=0x%X", cc);
}

static void dbg_tests_reg_bits(void) {
    const char *G = "Bits";
    i960_cpu_t  c;

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 3; c.locals.r[1] = 0;
    dbg_step(&c, enc_reg(0x583, 0, 1, 2), 0);
    dbg_rec(G, "setbit bit3",    c.locals.r[2] == 8, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 3; c.locals.r[1] = 0xFF;
    dbg_step(&c, enc_reg(0x58c, 0, 1, 2), 0);
    dbg_rec(G, "clrbit bit3",    c.locals.r[2] == 0xF7, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 7; c.locals.r[1] = 0;
    dbg_step(&c, enc_reg(0x580, 0, 1, 2), 0);
    dbg_rec(G, "notbit bit7",    c.locals.r[2] == 0x80, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_E);  c.locals.r[0] = 2; c.locals.r[1] = 0;
    dbg_step(&c, enc_reg(0x58f, 0, 1, 2), 0);
    dbg_rec(G, "alterbit cc=E set",   c.locals.r[2] == 4,    "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_L);  c.locals.r[0] = 2; c.locals.r[1] = 0xFF;
    dbg_step(&c, enc_reg(0x58f, 0, 1, 2), 0);
    dbg_rec(G, "alterbit cc≠E clr",  c.locals.r[2] == 0xFB, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0x12345678;
    dbg_step(&c, enc_reg(0x5ad, 0, 0, 2), 0);
    dbg_rec(G, "bswap",          c.locals.r[2] == 0x78563412, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0x00800000;
    dbg_step(&c, enc_reg(0x641, 0, 0, 2), 0);
    dbg_rec(G, "scanbit pos=23", c.locals.r[2] == 23, "got=%u", c.locals.r[2]);
    dbg_rec(G, "scanbit CC_E",   get_cc(&c) == CC_E, "cc=0x%X", get_cc(&c));

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0;
    dbg_step(&c, enc_reg(0x641, 0, 0, 2), 0);
    dbg_rec(G, "scanbit 0→-1",   c.locals.r[2] == 0xFFFFFFFF, "got=0x%X", c.locals.r[2]);
    dbg_rec(G, "scanbit 0→NO",   get_cc(&c) == CC_NO, "cc=0x%X", get_cc(&c));

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0xFF7FFFFF;
    dbg_step(&c, enc_reg(0x640, 0, 0, 2), 0);
    dbg_rec(G, "spanbit pos=23", c.locals.r[2] == 23, "got=%u", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0x0000FFFF; c.locals.r[1] = 0x12345678; c.locals.r[2] = 0xABCD0000;
    dbg_step(&c, enc_reg(0x650, 0, 1, 2), 0);
    dbg_rec(G, "modify",         c.locals.r[2] == 0xABCD5678, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0xDEADBEEF;
    dbg_step(&c, enc_reg(0x5cc, 0, 0, 2), 0);
    dbg_rec(G, "mov",            c.locals.r[2] == 0xDEADBEEF, "got=0x%X", c.locals.r[2]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0x11111111; c.locals.r[1] = 0x22222222;
    dbg_step(&c, enc_reg(0x5dc, 0, 0, 4), 0);
    dbg_rec(G, "movl[0]",        c.locals.r[4] == 0x11111111, "got=0x%X", c.locals.r[4]);
    dbg_rec(G, "movl[1]",        c.locals.r[5] == 0x22222222, "got=0x%X", c.locals.r[5]);
}

static void dbg_tests_mem(void) {
    const char *G = "Mem";
    i960_cpu_t  c;

    g_dbg_scratch[8] = 0xAB;
    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0;
    dbg_step(&c, enc_mema(0x80, 0, 8), 0);
    dbg_rec(G, "ldob",           c.locals.r[0] == 0xAB, "got=0x%X", c.locals.r[0]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0xCD;
    dbg_step(&c, enc_mema(0x82, 0, 8), 0);
    dbg_rec(G, "stob",           g_dbg_scratch[8] == 0xCD, "mem=0x%X", g_dbg_scratch[8]);

    g_dbg_scratch[8] = 0x80;
    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0;
    dbg_step(&c, enc_mema(0xC0, 0, 8), 0);
    dbg_rec(G, "ldib sign ext",  c.locals.r[0] == 0xFFFFFF80, "got=0x%X", c.locals.r[0]);

    g_dbg_scratch[8] = 0x34; g_dbg_scratch[9] = 0x12;
    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0;
    dbg_step(&c, enc_mema(0x88, 0, 8), 0);
    dbg_rec(G, "ldos",           c.locals.r[0] == 0x1234, "got=0x%X", c.locals.r[0]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0xBEEF;
    dbg_step(&c, enc_mema(0x8a, 0, 8), 0);
    { uint16_t v; memcpy(&v, g_dbg_scratch + 8, 2);
      dbg_rec(G, "stos",         v == 0xBEEF, "mem=0x%X", v); }

    g_dbg_scratch[8] = 0x00; g_dbg_scratch[9] = 0x80;
    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0;
    dbg_step(&c, enc_mema(0xC8, 0, 8), 0);
    dbg_rec(G, "ldis sign ext",  c.locals.r[0] == 0xFFFF8000, "got=0x%X", c.locals.r[0]);

    { uint32_t w = 0xDEADBEEF; memcpy(g_dbg_scratch + 8, &w, 4); }
    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0;
    dbg_step(&c, enc_mema(0x90, 0, 8), 0);
    dbg_rec(G, "ld",             c.locals.r[0] == 0xDEADBEEF, "got=0x%X", c.locals.r[0]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0x12345678;
    dbg_step(&c, enc_mema(0x92, 0, 8), 0);
    { uint32_t w; memcpy(&w, g_dbg_scratch + 8, 4);
      dbg_rec(G, "st",           w == 0x12345678, "mem=0x%X", w); }

    { uint32_t pair[2] = { 0x11223344, 0x55667788 }; memcpy(g_dbg_scratch + 16, pair, 8); }
    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0; c.locals.r[1] = 0;
    dbg_step(&c, enc_mema(0x98, 0, 16), 0);
    dbg_rec(G, "ldl[0]",         c.locals.r[0] == 0x11223344, "got=0x%X", c.locals.r[0]);
    dbg_rec(G, "ldl[1]",         c.locals.r[1] == 0x55667788, "got=0x%X", c.locals.r[1]);

    c = dbg_mk_cpu(CC_NO); c.locals.r[0] = 0;
    dbg_step(&c, enc_mema(0x8C, 0, 42), 0);
    dbg_rec(G, "lda offset",     c.locals.r[0] == 42, "got=%u", c.locals.r[0]);

    c = dbg_mk_cpu(CC_NO);
    dbg_step(&c, enc_mema(0x84, 0, 8), 0);
    dbg_rec(G, "bx",             c.sfr.ip == 8, "ip=0x%X", c.sfr.ip);
}

/* ---- Master runner ------------------------------------------------------- */

static void dbg_run_all(void) {
    g_dbg_count  = 0;
    g_dbg_passed = 0;
    dbg_bus_init();
    dbg_tests_ctrl();
    dbg_tests_cobr();
    dbg_tests_reg_arith();
    dbg_tests_reg_shift();
    dbg_tests_reg_logic();
    dbg_tests_reg_compare();
    dbg_tests_reg_bits();
    dbg_tests_mem();
    g_dbg_run = true;
}

/* ---- ImGui window -------------------------------------------------------- */

static inline void debug_window_draw(bool *p_open) {
    igSetNextWindowSize((ImVec2){720, 560}, ImGuiCond_FirstUseEver);
    if (!igBegin("CPU Opcode Tests", p_open, 0)) { igEnd(); return; }

    if (igButton("Run Tests")) dbg_run_all();

    if (g_dbg_run) {
        int failed = g_dbg_count - g_dbg_passed;
        igSameLine();
        if (failed == 0)
            igTextColored((ImVec4){0.2f, 1.0f, 0.2f, 1.0f},
                          "%d / %d passed", g_dbg_passed, g_dbg_count);
        else
            igTextColored((ImVec4){1.0f, 0.3f, 0.3f, 1.0f},
                          "%d / %d passed  (%d FAILED)",
                          g_dbg_passed, g_dbg_count, failed);
    }

    igSeparator();

    ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                       | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (igBeginTable("##tests", 4, tf)) {
        igTableSetupScrollFreeze(0, 1);
        igTableSetupColumnEx("Group",  ImGuiTableColumnFlags_WidthFixed,   70.0f, 0);
        igTableSetupColumn   ("Test",  ImGuiTableColumnFlags_WidthStretch);
        igTableSetupColumnEx("Result", ImGuiTableColumnFlags_WidthFixed,   56.0f, 0);
        igTableSetupColumn   ("Detail",ImGuiTableColumnFlags_WidthStretch);
        igTableHeadersRow();

        for (int i = 0; i < g_dbg_count; i++) {
            dbg_test_t *t = &g_dbg_tests[i];
            igTableNextRow();
            igTableSetColumnIndex(0); igTextUnformatted(t->group);
            igTableSetColumnIndex(1); igTextUnformatted(t->name);
            igTableSetColumnIndex(2);
            if (t->passed)
                igTextColored((ImVec4){0.2f, 1.0f, 0.2f, 1.0f}, "PASS");
            else
                igTextColored((ImVec4){1.0f, 0.3f, 0.3f, 1.0f}, "FAIL");
            igTableSetColumnIndex(3); igTextUnformatted(t->detail);
        }
        igEndTable();
    }

    igEnd();
}

#endif /* DEBUG_WINDOW_H */
