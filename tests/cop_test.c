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
static int      ang_near(uint32_t w, uint32_t want){ uint32_t d = (w - want) & 0xFFFFu; return w <= 0xFFFFu && (d <= 2u || d >= 0xFFFEu); }

static void test_set_pos(void) {
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

    /* ---- Fn_get_matrix (0x02800505): the slot's words as it holds them ---- */
    /* col0, col1, col2, then T -- what a SHARC-side capture of the board shows. */
    cop_write(0x02800505);
    float m[12]; for (int i = 0; i < 12; i++) m[i] = b2f(cop_read());
    int mat_ok = feq(m[0],1)&&feq(m[1],0)&&feq(m[2],0)
              && feq(m[3],0)&&feq(m[4],1)&&feq(m[5],0)
              && feq(m[6],0)&&feq(m[7],0)&&feq(m[8],1)
              && feq(m[9],10)&&feq(m[10],20)&&feq(m[11],30);
    CHECK(mat_ok, "get_matrix = identity columns, then T=(10,20,30)");
}

/* ---- column-major post-multiply Y-rotation (0x04800909, 90 deg) ---- */
static void test_rot_y(void) {
    cop_reset();
    cop_write(0x04800909);
    cop_write(0x00004000);   /* 0x4000 of 0x10000 = 90 deg */
    /* postmul_ry(c=0,s=1): col0' = col2 = (0,0,1); col2' = -col0 = (-1,0,0). */
    float (*r)[3] = g_sharc.rot;
    CHECK(feq(r[0][0],0)&&feq(r[0][1],0)&&feq(r[0][2],1), "ang_y 90: col0 -> (0,0,1)");
    CHECK(feq(r[1][0],0)&&feq(r[1][1],1)&&feq(r[1][2],0), "ang_y 90: col1 unchanged (0,1,0)");
    CHECK(feq(r[2][0],-1)&&feq(r[2][1],0)&&feq(r[2][2],0), "ang_y 90: col2 -> (-1,0,0)");
}

/* ---- Fn_get_glo_ang (0x28805151): the current matrix's three angles ---- */
/* atan2(col2.x, col2.z), asin(col2.y), atan2(col0.y, col1.y), as angle
 * words (cpres1 PM 0x2114F).  After ang_y 90, col2 = (-1,0,0): -90 deg. */
static void test_glo_ang(void) {
    cop_write(0x28805151);
    uint32_t ga0 = cop_read(), ga1 = cop_read(), ga2 = cop_read();
    CHECK(ang_near(ga0, 0xC000) && ang_near(ga1, 0) && ang_near(ga2, 0),
          "get_glo_ang after ang_y 90 = (0xC000, 0, 0)");
    cop_reset();
    cop_write(0x04000808); cop_write(0x00002000);   /* ang_x 45 deg */
    cop_write(0x28805151);
    ga0 = cop_read(); ga1 = cop_read(); ga2 = cop_read();
    CHECK(ang_near(ga0, 0) && ang_near(ga1, 0x2000) && ang_near(ga2, 0),
          "get_glo_ang after ang_x 45 = (0, 0x2000, 0): asin of col2.y");
    cop_reset();
    cop_write(0x05000A0A); cop_write(0x00001000);   /* ang_z 22.5 deg */
    cop_write(0x28805151);
    ga0 = cop_read(); ga1 = cop_read(); ga2 = cop_read();
    /* col0 = (c, -s, 0), col1 = (s, c, 0): atan2(-s, c) */
    CHECK(ang_near(ga0, 0) && ang_near(ga1, 0) && ang_near(ga2, 0xF000),
          "get_glo_ang after ang_z 22.5 = (0, 0, 0xF000)");
}

/* ---- the inner 3x3 tables (0x7C load, 0x7D store, 0x7E multiply) ---- */
/* Two args each, a word index and the table (0: DM 0x31000, else 0x31800),
 * no reply; T is left alone. Fighting Vipers lands a knocked-out fighter
 * with them (Pinboard #1331). Unknown, their arguments went down as commands. */
static void test_inner_tables(void) {
    float (*r)[3] = g_sharc.rot;
    cop_reset();
    uint32_t unk_i3 = g_sharc.unknown_cmds;
    cop_write(0x04000808); cop_write(0x00004000);             /* ang_x 90 */
    cop_write(0x3E807D7D); cop_write(18); cop_write(0);       /* store: table 0, entry 2 */
    float rx[3][3]; memcpy(rx, g_sharc.rot, sizeof rx);
    CHECK(feq(b2f(g_sharc.dm[0x1000 + 18 + 4]), rx[1][1]) && feq(b2f(g_sharc.dm[0x1000 + 18 + 5]), rx[1][2]),
          "0x7D stores the 3x3 at DM 0x31000 + index");
    cop_write(0x08001010);                                    /* identity 3x3 (cop_reset clears DM) */
    cop_write(0x04800909); cop_write(0x00004000);             /* ang_y 90 */
    cop_write(0x3E807D7D); cop_write(27); cop_write(1);       /* store: table 1, entry 3 */
    cop_write(0x04000808); cop_write(0x00004000);             /* then ang_x 90 */
    float want[3][3]; memcpy(want, g_sharc.rot, sizeof want);  /* Ry * Rx, by post-multiply */
    cop_write(0x08001010);                                    /* identity 3x3 (cop_reset clears DM) */
    cop_write(0x03000606); cop_write(f2b(1.0f)); cop_write(f2b(2.0f)); cop_write(f2b(3.0f));
    cop_write(0x3E007C7C); cop_write(27); cop_write(1);       /* load Ry */
    CHECK(feq(r[0][2], 1) && feq(r[2][0], -1) && feq(r[1][1], 1), "0x7C loads the 3x3 from DM 0x31800 + index");
    cop_write(0x3F007E7E); cop_write(18); cop_write(0);       /* compose with Rx */
    int mul_ok = 1;
    for (int c = 0; c < 3; c++) for (int w = 0; w < 3; w++) mul_ok &= feq(r[c][w], want[c][w]);
    CHECK(mul_ok, "0x7E composes the 3x3 with the table entry, as a post-multiply");
    CHECK(feq(g_sharc.pos[0], 1) && feq(g_sharc.pos[1], 2) && feq(g_sharc.pos[2], 3), "0x7C / 0x7E leave T alone");
    /* Fn_ziku_rot (0x79): a turn about an axis into the table 0x7E reads with a 0.
     * 90 degrees about +Y: col0 = (c, 0, -s), col2 = (s, 0, c) (Rodrigues). */
    cop_write(0x3C807979); cop_write(36); cop_write(0x4000);
    cop_write(f2b(0.0f)); cop_write(f2b(1.0f)); cop_write(f2b(0.0f));
    cop_write(0x08001010);
    cop_write(0x3F007E7E); cop_write(36); cop_write(0);
    CHECK(feq(r[0][0], 0) && feq(r[0][2], -1) && feq(r[1][1], 1) && feq(r[2][0], 1) && feq(r[2][2], 0),
          "0x79 writes the axis turn that 0x7E then composes");
    CHECK(g_sharc.unknown_cmds == unk_i3 && cop_fifo_status() == 1, "no unknown commands and no replies");
}

/* ---- Fn_tri_shin (0x14002828): three sides in, three words out ----
 * Reply 1 is 0x4000 - asin(cos B) as an angle word, cos B = (a^2-b^2+c^2)/(2ac);
 * replies 2 and 3 reuse registers the calls clobbered (cpres1 PM 0x20644), so
 * the board answers NaN there unless cos B is exactly +-1.  The words below
 * are what STF's uploaded firmware answers in sharc_cpu.h (Pinboard #1333). */
static void test_tri_shin(void) {
    static const struct { float a, b, c; uint32_t w[3]; } ts[] = {
        { 3, 4, 5,   { 0x25C9, 0xFFFFFFFFu, 0xFFFFFFFFu } },
        { 5, 4, 3,   { 0x25C9, 0xFFFFFFFFu, 0xFFFFFFFFu } },
        { 1, 1, 1,   { 0x2AAB, 0xFFFFFFFFu, 0xFFFFFFFFu } },
        { 2, 3, 4,   { 0x211E, 0xFFFFFFFFu, 0xFFFFFFFFu } },
        { 7, 2, 6.5f,{ 0x0BBD, 0xFFFFFFFFu, 0xFFFFFFFFu } },
        { 1, 2, 4,   { 0x4000, 0xFFFFFFFFu, 0xFFFFFFFFu } },   /* no triangle: cos B < -1 */
        { 0, 1, 1,   { 0x4000, 0xFFFFFFFFu, 0xFFFFFFFFu } },   /* a = 0: divide by zero */
        { 1, 1, 2,   { 0x0000, 0x3F800000u, 0xFFFFFFFFu } },   /* cos B = 1: reply 2 = 1.0 */
        { 2, 1, 1,   { 0x0000, 0xBF000000u, 0xFFFFFFFFu } },   /* cos B = 1: reply 2 = -0.5 */
    };
    cop_reset();
    uint32_t unk_t = g_sharc.unknown_cmds;
    for (size_t i = 0; i < sizeof ts / sizeof ts[0]; i++) {
        cop_write(0x14002828);
        cop_write(f2b(ts[i].a)); cop_write(f2b(ts[i].b)); cop_write(f2b(ts[i].c));
        uint32_t o0 = cop_read(), o1 = cop_read(), o2 = cop_read();
        char msg[96];
        snprintf(msg, sizeof msg, "tri_shin(%g,%g,%g) = %04X %08X %08X", ts[i].a, ts[i].b, ts[i].c,
                 ts[i].w[0], ts[i].w[1], ts[i].w[2]);
        CHECK(o0 == ts[i].w[0] && o1 == ts[i].w[1] && o2 == ts[i].w[2], msg);
    }
    CHECK(g_sharc.unknown_cmds == unk_t && cop_fifo_status() == 1,
          "tri_shin takes 3 args and answers exactly 3 words");
}

/* ---- atan2's coefficients come from DM 0x30290..0x3029B ----
 * STF's firmware writes them at boot; Fighting Vipers' does not, and its
 * i960 writes its own with 0x24804949, 1 ulp above STF's (Pinboard #1334).
 * The words below are what each game's uploaded firmware answers in
 * sharc_cpu.h, FV's after its i960's DM writes are replayed. */
static void test_atan2_coeffs(void) {
    static const uint32_t fv_k[12] = {
        0x3E8930A3u, 0x3FDDB3D7u, 0x39800000u, 0xBF3853AEu, 0xBFB854A8u, 0x4098123Cu,
        0x408A3F7Eu, 0x00000000u, 0x3F060A92u, 0x3FC90FDBu, 0x3F860A92u, 0x40490FDBu,
    };
    static const struct { uint32_t cmd, a[3]; uint32_t stf, fv; } at[] = {
        { 0x13802727, { 0xC536DE6Cu, 0x38E9CD4Du }, 0x7FFF, 0x8000 },
        { 0x13802727, { 0x39483F64u, 0x45871DDCu }, 0x3FFF, 0x4000 },
        { 0x13802727, { 0xC254C850u, 0xC19AFD8Au }, 0x8E3B, 0x8E3A },
        { 0x14002828, { 0x424062EAu, 0x403B162Cu, 0x4249491Fu }, 0x0193, 0x0192 },
        { 0x14002828, { 0x43412756u, 0x431F1DF1u, 0x42E2988Cu }, 0x276E, 0x276D },
        { 0x14002828, { 0x4301A79Bu, 0x4312D519u, 0x4389F79Cu }, 0x02BF, 0x02BE },
        { 0x14002828, { 0x42B328C2u, 0x430E2A46u, 0x4365309Bu }, 0x07A5, 0x07A4 },
        { 0x14002828, { 0x7149F2CAu, 0x3F800000u, 0x0DA24260u }, 0x0001, 0x0000 },
    };
    for (int fv = 0; fv < 2; fv++) {
        cop_reset();
        if (fv) for (uint32_t i = 0; i < 12; i++) { cop_write(0x24804949); cop_write(0x30290u + i); cop_write(fv_k[i]); }
        for (size_t i = 0; i < sizeof at / sizeof at[0]; i++) {
            int na = at[i].cmd == 0x14002828 ? 3 : 2;
            cop_write(at[i].cmd);
            for (int k = 0; k < na; k++) cop_write(at[i].a[k]);
            uint32_t o = cop_read();
            for (int k = 1; k < na; k++) cop_read();
            uint32_t want = fv ? at[i].fv : at[i].stf;
            char msg[96];
            snprintf(msg, sizeof msg, "%s table: %08X(%08X %08X) = %04X", fv ? "FV" : "STF",
                     at[i].cmd, at[i].a[0], at[i].a[1], want);
            CHECK(o == want, msg);
        }
    }
    CHECK(cop_fifo_status() == 1, "atan table: no replies left over");
}

/* ---- 0x980004: reply FIFO status, bit 0 up while it is empty ---- */
static void test_fifo_status(void) {
    cop_reset();
    CHECK(cop_fifo_status() == 1, "FIFO status reads 1 with no replies waiting");
    cop_write(0x07800F0F);
    CHECK(cop_fifo_status() == 0, "FIFO status reads 0 while replies wait");
    cop_read(); cop_read(); cop_read();
    CHECK(cop_fifo_status() == 1, "FIFO status reads 1 once drained");
    uint64_t uf0 = g_cop.underflows;
    CHECK(cop_read() == 0 && g_cop.underflows == uf0 + 1,
          "a read of the empty FIFO answers 0 and is counted");
}

/* ---- model->world transform (0x14802929) end-to-end ---- */
static void test_transform(void) {
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
}

/* ---- Fn_zanzou_reserve (0x40008080): the one variable-length command ----
 *
 * The i960's zanzou_control sends 4 header words, then a 4-word record per
 * trailing part with one word read back after each, then -1, the turn angle
 * and a last word read back.  Drive that conversation through the FIFO and
 * check the trail it lays: a part that moved 1.0 units with the spacing at
 * 0.1 gets ten copies, walking from last frame's matrix to this frame's. */
static void test_zanzou(void) {
    cop_reset();
    /* what the i960 uploads at boot: spacing 0.1, first life 3 */
    cop_write(0x24804949); cop_write(0x32181); cop_write(f2b(0.1f));
    cop_write(0x24804949); cop_write(0x32182); cop_write(3);
    /* part 0's matrix, last frame at the origin and this frame 1.0 along X */
    for (int k = 0; k < 12; k++) {
        sharc_dm_setf(0x32000u + (uint32_t)k, k == 0 || k == 4 || k == 8 ? 1.0f : 0.0f);
        sharc_dm_setf(0x30420u + (uint32_t)k, k == 0 || k == 4 || k == 8 ? 1.0f : 0.0f);
    }
    sharc_dm_setf(0x30420u + 9u, 1.0f);                  /* this frame's T.x = 1 */

    cop_write(0x40008080);
    cop_write(0);            /* player  */
    cop_write(1);            /* part mask: part 0 only */
    cop_write((uint32_t)-4); /* life step */
    cop_write(f2b(0.0f));    /* bone length */
    cop_write(0);            /* record: part 0 */
    cop_write(0x1111); cop_write(0x2222); cop_write(0x3333);
    uint32_t rec_reply = cop_read();
    cop_write(0xFFFFFFFFu);  /* terminator */
    cop_write(0);            /* turn angle */
    uint32_t end_reply = cop_read();
    CHECK(rec_reply == 0x20, "zanzou_reserve answers index+0x20 after each record");
    CHECK(end_reply == f2b(1.0f), "zanzou_reserve's last word is cos of the turn angle");
    CHECK(sharc_dm_get(0x32180u) == 10, "a 1.0-unit move at 0.1 spacing lays ten copies");
    CHECK(sharc_dm_get(0x32300u + 0u) == 0 && sharc_dm_get(0x32300u + 3u) == (uint32_t)-4
          && sharc_dm_get(0x32300u + 4u) == 0x1111,
          "each ring slot carries its part, life step and object numbers");
    CHECK(feq(b2f(sharc_dm_get(0x32300u + 0x1Du)), 0.0f)
          && feq(b2f(sharc_dm_get(0x32300u + 5u * 0x20u + 0x1Du)), 0.5f),
          "the copies walk from last frame's matrix to this frame's");

    /* zanzou_disp's side: the slot is alive and reports what it was given. */
    cop_write(0x42808585); cop_write(0);
    uint32_t info[5]; for (int k = 0; k < 5; k++) info[k] = cop_read();
    CHECK(info[0] == 0 && info[2] == 3 && info[3] == (uint32_t)-4,
          "zanzou_get_info answers the slot's part, life and step");
    CHECK(info[4] == 0x3333, "a young copy takes the third object number");
    /* Life runs 3..12 up the trail; -4 a frame kills the youngest outright. */
    cop_write(0x41008282);
    CHECK(cop_read() == 9, "zanzou_inc counts the live slots");
}

/* ---- unknown command accounting ---- */
static void test_unknown_cmds(void) {
    uint32_t unk0 = g_sharc.unknown_cmds;
    cop_write(0xDEADBEEF);   /* not in dispatch table -> default/unknown path */
    CHECK(g_sharc.unknown_cmds == unk0 + 1, "unknown COP command is counted");
    cop_write(0x0000BEEF);   /* a zero top half is no excuse (issue #125) */
    CHECK(g_sharc.unknown_cmds == unk0 + 2 && g_sharc.unknown_log_count >= 2
          && g_sharc.unknown_log[g_sharc.unknown_log_count - 1].cmd == 0x0000BEEFu,
          "an unknown command with a zero top half is logged too");
}

/* ---- the boot image: COPRO_CTL1 bit 31 up, the FIFO takes halfwords ---- */
static void test_boot_image(void) {
    uint8_t ctl[4] = {0, 0, 0, 0x80};
    g_cop.ctl = ctl;
    uint32_t unk1 = g_sharc.unknown_cmds;
    for (uint32_t k = 0; k < 16; k++) cop_write(0x1234u + k);
    CHECK(g_cop.upload_words == 16 && g_sharc.unknown_cmds == unk1
          && g_cop.cur_cmd == 0 && g_cop.args_needed == 0,
          "words written while uploading are the image, not commands");
    ctl[3] = 0;
    cop_write(0x00800101);   /* push */
    CHECK(g_sharc.stack_top == 1, "commands resume once the bit is lowered");
    cop_write(0x00000000);   /* Fn_initialize, cop_initialize's first word */
    CHECK(g_sharc.stack_top == 0 && g_sharc.unknown_cmds == unk1,
          "Fn_initialize empties the matrix stack");
    g_cop.ctl = NULL;
}

/* ---- Fn_area_table_gen (0x3A): STF's 1/8-unit bins against FV's 1/16 ----
 * One P0 ball at x 0.3 (radius 0.019), P1 ball 0 at x 0.5595 (radius 0.19),
 * P1 ball 1 of radius 0. STF's bins put the pair in reach, FV's finer ones
 * do not, and FV leaves the radius-0 ball's mask alone (Pinboard #1353). */
static void test_area_table(void) {
    static uint8_t bufram[0x20000 * 4];
    cop_reset();
    g_sharc.sharc_dm_ext = bufram; g_sharc.sharc_dm_ext_size = sizeof bufram;
    sharc_dm_setf(0x1403F40u, 0.3f);    sharc_dm_setf(0x1403FA0u, 0.3f);
    sharc_dm_setf(0x1407F40u, 0.5595f); sharc_dm_setf(0x1407FA0u, 0.5595f);
    sharc_dm_setf(0x30600u, 0.019f);    sharc_dm_setf(0x30700u, 0.19f);
    uint32_t am[2][2];
    for (int fv = 0; fv < 2; fv++) {
        g_sharc_area_fv = fv;
        sharc_coli_area_table_gen(0, 0, 0, 0);
        am[fv][0] = sharc_dm_get(0x1403E20u); am[fv][1] = sharc_dm_get(0x1403E21u);
    }
    g_sharc_area_fv = false;
    g_sharc.sharc_dm_ext = NULL; g_sharc.sharc_dm_ext_size = 0;
    CHECK(am[0][0] == 1u && am[0][1] == 0u, "0x3A, STF: 1/8-unit bins keep the pair, a radius-0 P1 ball gets mask 0");
    CHECK(am[1][0] == 0u && am[1][1] == 0xFFFFFFFFu, "0x3A, FV: 1/16-unit bins drop the pair, a radius-0 P1 ball keeps its mask");
}

int main(void) {
    test_set_pos();
    test_rot_y();
    test_glo_ang();
    test_inner_tables();
    test_tri_shin();
    test_atan2_coeffs();
    test_fifo_status();
    test_transform();
    test_zanzou();
    test_unknown_cmds();
    test_boot_image();
    test_area_table();

    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
