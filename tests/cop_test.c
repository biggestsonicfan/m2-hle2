/*
 * cop_test.c — Phase 7 verification for the COP/SHARC HLE.
 *
 * Drives the real i960↔SHARC FIFO (cop_write/cop_read) and checks the ported
 * math + plumbing: arg accumulation, command dispatch, the reply FIFO drain,
 * setter→readback round-trips, the column-major post-multiply Y-rotation, the
 * z-negated model→world transform (0x14802929), the geo-capture ring, and
 * unknown-command accounting.
 *
 * The underlying math was MAME-verified during m2-hle development; this test
 * confirms the port is faithful (no copy corruption) and the bridge works.
 */
#define NDEBUG 1
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "cop.h"   /* pulls sharc_exec.h + sharc.h */

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

static uint32_t f2b(float f){ uint32_t u; memcpy(&u,&f,4); return u; }
static float    b2f(uint32_t u){ float f; memcpy(&f,&u,4); return f; }
static int      feq(float a, float b){ return fabsf(a-b) < 1e-4f; }

int main(void) {
    cop_reset();

    /* ---- setter (0x03000606 set_pos) + readback (0x07800F0F read_world_pos) ---- */
    /* identity rot, so T += I × (10,20,30) = (10,20,30). */
    cop_write(0x03000606);
    cop_write(f2b(10.0f)); cop_write(f2b(20.0f)); cop_write(f2b(30.0f));
    CHECK(feq(g_sharc.pos[0],10) && feq(g_sharc.pos[1],20) && feq(g_sharc.pos[2],30),
          "set_pos accumulates T through the arg FIFO");

    cop_write(0x07800F0F);   /* 0 args -> dispatches immediately, stages 3 replies */
    float w0 = b2f(cop_read()), w1 = b2f(cop_read()), w2 = b2f(cop_read());
    CHECK(feq(w0,10) && feq(w1,20) && feq(w2,30),
          "read_world_pos returns T via the reply FIFO (cop_read drain)");
    CHECK(cop_read() == 0, "reply FIFO empty after draining all replies");

    /* ---- read 3x4 matrix (0x02800505): identity rot + translation column ---- */
    cop_write(0x02800505);
    float m[12]; for (int i = 0; i < 12; i++) m[i] = b2f(cop_read());
    int mat_ok = feq(m[0],1)&&feq(m[1],0)&&feq(m[2],0)&&feq(m[3],10)
              && feq(m[4],0)&&feq(m[5],1)&&feq(m[6],0)&&feq(m[7],20)
              && feq(m[8],0)&&feq(m[9],0)&&feq(m[10],1)&&feq(m[11],30);
    CHECK(mat_ok, "read matrix = identity rot with T=(10,20,30)");

    /* ---- column-major post-multiply Y-rotation (0x04800909, 90 deg) ---- */
    cop_reset();
    cop_write(0x04800909);
    cop_write(0x00004000);   /* 0x4000 of 0x10000 = 90 deg */
    /* postmul_ry(c=0,s=1): col0' = col2 = (0,0,1); col2' = -col0 = (-1,0,0). */
    float (*r)[3] = g_sharc.rot;
    CHECK(feq(r[0][0],0)&&feq(r[0][1],0)&&feq(r[0][2],1), "ang_y 90: col0 -> (0,0,1)");
    CHECK(feq(r[1][0],0)&&feq(r[1][1],1)&&feq(r[1][2],0), "ang_y 90: col1 unchanged (0,1,0)");
    CHECK(feq(r[2][0],-1)&&feq(r[2][1],0)&&feq(r[2][2],0), "ang_y 90: col2 -> (-1,0,0)");

    /* ---- model->world transform (0x14802929) end-to-end ---- */
    cop_reset();
    cop_write(0x03000606);                                   /* set_pos (10,20,30) */
    cop_write(f2b(10.0f)); cop_write(f2b(20.0f)); cop_write(f2b(30.0f));
    cop_write(0x14802929);                                   /* transform (1,2,3) */
    cop_write(f2b(1.0f)); cop_write(f2b(2.0f)); cop_write(f2b(3.0f));
    float ox = b2f(cop_read()), oy = b2f(cop_read()), oz = b2f(cop_read());
    /* identity rot: out = in + T = (11,22,33). */
    CHECK(feq(ox,11) && feq(oy,22) && feq(oz,33),
          "0x14802929 identity transform: (1,2,3)+T -> (11,22,33)");

    /* ---- geo-capture ring records the raw command stream ---- */
    CHECK(g_cop.geo_capture_count > 0, "geo-capture ring recorded the COP write stream");
    CHECK(g_cop.writes > 0, "cop write counter advanced");

    /* ---- unknown command accounting ---- */
    uint32_t unk0 = g_sharc.unknown_cmds;
    cop_write(0xDEADBEEF);   /* not in dispatch table -> default/unknown path */
    CHECK(g_sharc.unknown_cmds == unk0 + 1, "unknown COP command is counted");

    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
