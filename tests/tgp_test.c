/*
 * tgp_test.c - the Model 2A TGP (src/board/tgp.h), with no ROM.
 *
 * A float op and a transfer into d in one instruction (Pinboard #661): the
 * next instruction's transfer reads the op's result out of d, and then d holds
 * the word that was moved. Sega Rally's square root (its TGP helper at 0x1AB)
 * is built on it; MAME lets the result win, and its roots come out long.
 */
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "board/tgp.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

int main(void) {
    tgp_reset();
    g_tgp.halted = false;
    g_tgp.prog[0] = 0x1d9db24eu;    /* fsmd : mov $78, d   d = d + p, and x moved into d */
    g_tgp.prog[1] = 0x1c1f2019u;    /* mov d, a            a = what d reads now */
    g_tgp.prog[2] = 0x1e1f1e10u;    /* fdvd                d = d / a */
    g_tgp.ram0[78] = f2u(0.5f);
    g_tgp.d = f2u(0.25f);
    g_tgp.p = f2u(0.5f);

    tgp_step();
    CHECK(u2f(g_tgp.d) == 0.75f, "the float result is in d after its instruction");
    tgp_step();
    CHECK(u2f(g_tgp.a) == 0.75f, "the next transfer reads the result");
    CHECK(u2f(g_tgp.d) == 0.5f, "then d holds the moved word");
    tgp_step();
    CHECK(u2f(g_tgp.d) == 0.5f / 0.75f, "the divide takes x over the seed");

    /* Without a transfer into d the result simply stays. */
    tgp_reset();
    g_tgp.halted = false;
    g_tgp.prog[0] = 0x1d9f1e10u;    /* fsmd */
    g_tgp.prog[1] = 0x1c1f2019u;    /* mov d, a */
    g_tgp.d = f2u(0.25f);
    g_tgp.p = f2u(0.5f);
    tgp_step();
    tgp_step();
    CHECK(u2f(g_tgp.a) == 0.75f && u2f(g_tgp.d) == 0.75f, "a lone float op keeps its result");

    printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
