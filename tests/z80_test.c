/*
 * z80_test.c - the Z80 (src/board/z80.h), with no ROM.
 *
 * Short programs whose answers the Zilog manual gives: the arithmetic flags
 * (overflow, half carry, DAA), a block copy, a call and return, the indexed
 * modes, the T-states of a few instructions, and an IM 1 interrupt that wakes
 * a HALT. The core passed ZEXDOC and ZEXALL when it was written (Pinboard
 * #1035); with $Z80_ZEX naming one of those CP/M programs, this runs it too
 * and fails on any line that is not "OK".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board/z80.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

static uint8_t mem[65536];
static uint8_t port_last;
static uint8_t rd(void *u, uint16_t a) { (void)u; return mem[a]; }
static void    wr(void *u, uint16_t a, uint8_t v) { (void)u; mem[a] = v; }
static uint8_t in(void *u, uint16_t a) { (void)u; return (uint8_t)a; }
static void    out(void *u, uint16_t a, uint8_t v) { (void)u; (void)a; port_last = v; }
static const z80_bus_t bus = { rd, in, wr, out, NULL };

/* Load `code` at 0, reset, and run until PC reaches `stop` (or a budget). */
static z80_state_t run(const uint8_t *code, size_t n, uint16_t stop) {
    z80_state_t z;
    memset(mem, 0, sizeof mem);
    memcpy(mem, code, n);
    z80_reset(&z);
    z.sp = 0xF000;
    for (int i = 0; i < 10000 && z.pc != stop; i++) z80_run(&z, &bus, 1);
    return z;
}

/* The text a BDOS call at 5 prints: function 2 a character, 9 a $-ended string. */
static size_t bdos_text(const z80_state_t *z, char *buf, size_t size) {
    uint8_t fn = (uint8_t)z->bc;
    size_t k = 0;
    if (fn == 2) buf[k++] = (char)(uint8_t)z->de;
    else if (fn == 9) for (uint16_t a = z->de; mem[a] != '$' && k < size; a++) buf[k++] = (char)mem[a];
    return k;
}

/* A finished line of ZEX output: a test's line ("....") is an OK or a failure. */
static void zex_line(const char *line, int *oks, int *bad) {
    if (!strstr(line, "....")) return;
    if (strstr(line, "OK")) (*oks)++;
    else { printf("FAIL: %s\n", line); (*bad)++; }
}

/* A CP/M program: BDOS 2 and 9 print, a jump to 0 ends it. */
static int run_zex(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL: cannot open %s\n", path); return 1; }
    memset(mem, 0, sizeof mem);
    size_t n = fread(mem + 0x100, 1, sizeof mem - 0x100, f);
    fclose(f);
    if (!n) return 1;
    mem[0] = 0x76; mem[5] = 0xC9;
    z80_state_t z;
    z80_reset(&z);
    z.pc = 0x100; z.sp = 0xF000;
    char line[128]; size_t ln = 0;
    int bad = 0, oks = 0;
    while (z.pc != 0) {
        if (z.pc == 5) {
            char buf[256];
            size_t k = bdos_text(&z, buf, sizeof buf);
            for (size_t j = 0; j < k; j++) {
                if (buf[j] == '\n' || buf[j] == '\r') {
                    line[ln] = 0;
                    zex_line(line, &oks, &bad);
                    ln = 0;
                } else if (ln + 1 < sizeof line) line[ln++] = buf[j];
            }
        }
        z80_run(&z, &bus, 1);
    }
    printf("%s: %d tests OK, %d failed\n", path, oks, bad);
    return bad || !oks;
}

int main(void) {
    z80_state_t z;

    /* LD A,7Fh; ADD A,1: overflow and half carry, sign set. */
    static const uint8_t add[] = { 0x3E, 0x7F, 0xC6, 0x01, 0x76 };
    z = run(add, sizeof add, 4);
    CHECK((z.af >> 8) == 0x80, "7F + 1 = 80");
    CHECK((z.af & (Z80_SF | Z80_PF | Z80_HF | Z80_ZF | Z80_CF | Z80_NF)) == (Z80_SF | Z80_PF | Z80_HF),
          "7F + 1 sets S, V and H only");

    /* LD A,15h; ADD A,27h; DAA: BCD 15 + 27 = 42. */
    static const uint8_t daa[] = { 0x3E, 0x15, 0xC6, 0x27, 0x27, 0x76 };
    z = run(daa, sizeof daa, 5);
    CHECK((z.af >> 8) == 0x42, "DAA after 15 + 27 gives 42");

    /* SUB to zero, then SBC with the carry clear. */
    static const uint8_t sub[] = { 0x3E, 0x10, 0xD6, 0x10, 0x76 };
    z = run(sub, sizeof sub, 4);
    CHECK((z.af >> 8) == 0 && (z.af & Z80_ZF) && (z.af & Z80_NF) && !(z.af & Z80_CF), "10 - 10 sets Z and N");

    /* LD HL,0100h; LD DE,0200h; LD BC,4; LDIR. */
    static const uint8_t ldir[] = { 0x21, 0x00, 0x01, 0x11, 0x00, 0x02, 0x01, 0x04, 0x00, 0xED, 0xB0, 0x76 };
    memset(mem, 0, sizeof mem);
    z80_reset(&z);
    memcpy(mem, ldir, sizeof ldir);
    memcpy(mem + 0x100, "SEGA", 4);
    z.sp = 0xF000;
    for (int i = 0; i < 100 && z.pc != 11; i++) z80_run(&z, &bus, 1);
    CHECK(memcmp(mem + 0x200, "SEGA", 4) == 0 && z.bc == 0 && z.hl == 0x104 && z.de == 0x204,
          "LDIR copies 4 bytes and leaves BC 0");
    CHECK(!(z.af & Z80_PF), "LDIR ends with P/V clear");

    /* CALL 0010h; HALT; ... 0010: LD A,5Ah; RET. */
    static const uint8_t call[] = { 0xCD, 0x10, 0x00, 0x76, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                    0x3E, 0x5A, 0xC9 };
    z = run(call, sizeof call, 3);
    CHECK((z.af >> 8) == 0x5A && z.sp == 0xF000, "CALL and RET come back with the stack whole");

    /* LD IX,0300h; LD (IX-2),99h; LD A,(IX-2). */
    static const uint8_t ix[] = { 0xDD, 0x21, 0x00, 0x03, 0xDD, 0x36, 0xFE, 0x99, 0xDD, 0x7E, 0xFE, 0x76 };
    z = run(ix, sizeof ix, 11);
    CHECK(mem[0x2FE] == 0x99 && (z.af >> 8) == 0x99, "(IX+d) with a negative displacement");

    /* T-states: NOP 4, LD BC,nn 10, LD (IX+d),n 19, OUT (n),A 11. */
    static const uint8_t t[] = { 0x00, 0x01, 0x34, 0x12, 0xDD, 0x36, 0x00, 0x01, 0xD3, 0x10, 0x76 };
    memset(mem, 0, sizeof mem);
    memcpy(mem, t, sizeof t);
    z80_reset(&z);
    int c1 = z80_run(&z, &bus, 1), c2 = z80_run(&z, &bus, 1), c3 = z80_run(&z, &bus, 1), c4 = z80_run(&z, &bus, 1);
    CHECK(c1 == 4 && c2 == 10 && c3 == 19 && c4 == 11, "T-states of NOP, LD BC,nn, LD (IX+d),n, OUT (n),A");
    CHECK(z.bc == 0x1234 && port_last == 0xFF, "LD BC,nn and OUT (n),A");

    /* IM 1; EI; HALT, and an interrupt: to 38h, then back after the HALT. */
    static const uint8_t im1[] = { 0xED, 0x56, 0xFB, 0x76, 0x00, 0x76 };
    memset(mem, 0, sizeof mem);
    memcpy(mem, im1, sizeof im1);
    mem[0x38] = 0x3E; mem[0x39] = 0x01; mem[0x3A] = 0xFB; mem[0x3B] = 0xED; mem[0x3C] = 0x4D;   /* LD A,1; EI; RETI */
    z80_reset(&z);
    z.sp = 0xF000;
    for (int i = 0; i < 20; i++) z80_run(&z, &bus, 1);
    CHECK(z.halted && z.pc == 3, "HALT waits");
    z80_set_irq(&z, 1);
    z80_run(&z, &bus, 1);
    z80_set_irq(&z, 0);
    CHECK(z.pc == 0x38 && !z.iff1, "IM 1 takes the interrupt at 38h");
    for (int i = 0; i < 4; i++) z80_run(&z, &bus, 1);
    CHECK((z.af >> 8) == 1 && z.pc == 5 && z.iff1, "RETI returns past the HALT with interrupts on");

    const char *zex = getenv("Z80_ZEX");
    if (zex && zex[0]) g_fail += run_zex(zex);
    else printf("skip: ZEXDOC/ZEXALL ($Z80_ZEX not set)\n");

    printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
