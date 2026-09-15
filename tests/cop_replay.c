/*
 * cop_replay.c — replay a MAME capture of the COP firmware's conversation
 * through the COP HLE and check every word it produced.
 *
 * The capture (scratch tool mame-cop-capture.lua) taps the SHARC's own side of
 * its FIFOs, so it holds exactly what the firmware consumed and produced, with
 * command words told apart from arguments by the PC that read them. A flat
 * stream of (u32 tag, u32 value), in execution order:
 *   0x21000000            a command word (read by the command loop, PM 0x20141)
 *   0x20000000            an argument word the running command read
 *   0x30000000            a word the running command wrote to the i960
 *   0x900000..0x97FFFF    the i960 wrote bufferram (SHARC DM 0x1400000)
 *   0x4000PPOO            the firmware's current matrix just before command OO,
 *                         as command PP left it (value: slot pointer), followed
 *                         by 12 records 0x41000000 holding its words
 * with <prefix>.bufram.bin holding bufferram as it stood when capture began, and
 * $COPRO_ROM optionally naming the COP data ROM (the sin/cos tables).
 *
 * Each command runs through sharc_exec() with the arguments the firmware
 * actually read, so the report separates two kinds of fault: an argument count
 * the HLE's table disagrees with (which, live, desynchronises the FIFO), and a
 * handler that computes the wrong words from the right inputs.
 *
 * Usage: cop_replay <prefix> [examples-per-op] [only-op-hex]
 */
#define NDEBUG 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "cop.h"

typedef struct { const char *name; int in, out; } fw_op_t;
static const fw_op_t FW[256] = {
    [0x00] = { "Fn_initialize", 0, 0 },
    [0x01] = { "Fn_push_matrix", 0, 0 },
    [0x02] = { "Fn_pop_matrix", 0, 0 },
    [0x03] = { "Fn_base_matrix", 0, 0 },
    [0x04] = { "Fn_load_matrix", 12, 0 },
    [0x05] = { "Fn_get_matrix", 0, 12 },
    [0x06] = { "Fn_trans", 3, 0 },
    [0x07] = { "Fn_scale", 3, 0 },
    [0x08] = { "Fn_x_rot", 1, 0 },
    [0x09] = { "Fn_y_rot", 1, 0 },
    [0x0a] = { "Fn_z_rot", 1, 0 },
    [0x0b] = { "Fn_mul_matrix", 12, 0 },
    [0x0c] = { "Fn_inv_matrix", 0, 0 },
    [0x0d] = { "Fn_base_point", 0, 0 },
    [0x0e] = { "Fn_load_point", 3, 0 },
    [0x0f] = { "Fn_get_point", 0, 3 },
    [0x10] = { "Fn_base_3x3", 0, 0 },
    [0x11] = { "Fn_load_3x3", 9, 0 },
    [0x12] = { "Fn_get_3x3", 0, 9 },
    [0x13] = { "Fn_add", 2, 1 },
    [0x14] = { "Fn_sub", 2, 1 },
    [0x15] = { "Fn_mul", 2, 1 },
    [0x16] = { "Fn_div", 2, 1 },
    [0x17] = { "Fn_cvtws", 1, 1 },
    [0x18] = { "Fn_cvtsw", 1, 1 },
    [0x19] = { "Fn_sqr_r", 1, 1 },
    [0x1a] = { "Fn_sqr", 1, 1 },
    [0x1b] = { "Fn_put_c", 1, 0 },
    [0x1c] = { "Fn_get_c", 0, 1 },
    [0x1d] = { "Fn_add_c", 1, 0 },
    [0x1e] = { "Fn_sub_c", 1, 0 },
    [0x1f] = { "Fn_mul_c", 1, 0 },
    [0x20] = { "Fn_div_c", 1, 0 },
    [0x21] = { "Fn_sin", 1, 1 },
    [0x22] = { "Fn_cos", 1, 1 },
    [0x23] = { "Fn_tan", 1, 1 },
    [0x24] = { "Fn_sinx", 2, 1 },
    [0x25] = { "Fn_cosx", 2, 1 },
    [0x26] = { "Fn_asin", 1, 1 },
    [0x27] = { "Fn_atan", 2, 1 },
    [0x28] = { "Fn_tri_shin", 3, 3 },
    [0x29] = { "Fn_point_trans", 3, 3 },
    [0x2a] = { "Fn_get_inner", 6, 1 },
    [0x2b] = { "Fn_get_2d_r", 4, 1 },
    [0x2c] = { "Fn_get_3d_r", 6, 1 },
    [0x2d] = { "Fn_get_2d_len", 2, 1 },
    [0x2e] = { "Fn_get_3d_len", 3, 1 },
    [0x2f] = { "Fn_get_2d_dir", 4, 1 },
    [0x30] = { "Fn_regular_vector", 3, 3 },
    [0x31] = { "Fn_fcurve_lin", 4, 1 },
    [0x32] = { "Fn_fcurve_spl", 6, 1 },
    [0x33] = { "Fn_coli_dist", 0, 0 },
    [0x34] = { "Fn_mov_matrix", 1, 1 },
    [0x35] = { "Fn_st_unit_mat", 2, 0 },
    [0x36] = { "Fn_ld_unit_mat", 2, 0 },
    [0x37] = { "Fn_mul_unit_mat", 2, 0 },
    [0x38] = { "Fn_coli_set_ball_adrs", 1, 0 },
    [0x39] = { "Fn_coli_point_trans", 4, 0 },
    [0x3a] = { "Fn_area_table_gen", 4, 0 },
    [0x3b] = { "Fn_calc_coli_flag", 7, 0 },
    [0x3c] = { "Fn_coli_sink", 0, 0 },
    [0x3d] = { "Fn_coli_trans_mat", 6, 0 },
    [0x3e] = { "Fn_coli_trans_xz", 5, 0 },
    [0x3f] = { "Fn_zyx_rot", 3, 1 },
    [0x40] = { "Fn_calc_unit", 0, 0 },
    [0x41] = { "Fn_kage_leave_x_axis", -1, -1 },
    [0x42] = { "Fn_kage_leave_z_axis", -1, -1 },
    [0x43] = { "Fn_get_matrix_inner", 1, 0 },
    [0x44] = { "Fn_load_matrix_inner", 1, 0 },
    [0x45] = { "Fn_mul_matrix_inner", 1, 0 },
    [0x46] = { "Fn_mul_matrix_inner_rev", 1, 0 },
    [0x47] = { "Fn_mul_matrix_rev", 12, 0 },
    [0x48] = { "Fn_read_ram", 1, 1 },
    [0x49] = { "Fn_write_ram", 2, 0 },
    [0x4a] = { "Fn_osage", 1, 0 },
    [0x4b] = { "Fn_ken2", 0, 0 },
    [0x4c] = { "Fn_ken3", 0, 0 },
    [0x4d] = { "Fn_ken4", 0, 0 },
    [0x4e] = { "Fn_ken5", 0, 0 },
    [0x4f] = { "Fn_base_zy", 0, 0 },
    [0x50] = { "Fn_base_yz", 0, 0 },
    [0x51] = { "Fn_get_glo_ang", 0, 3 },
    [0x52] = { "Fn_base_zyx_ang", 0, 0 },
    [0x53] = { "Fn_base_zyx", 0, 0 },
    [0x54] = { "Fn_get_sm_ang_f", 9, 3 },
    [0x55] = { "Fn_get_sm_ang_r", 0, 0 },
    [0x56] = { "Fn_get_x_axis", 0, 3 },
    [0x57] = { "Fn_get_y_axis", 0, 3 },
    [0x58] = { "Fn_get_z_axis", 0, 3 },
    [0x59] = { "Fn_get_inner_2d", 4, 1 },
    [0x5a] = { "Fn_regular_vector_2d", 2, 2 },
    [0x5b] = { "Fn_rot_2d", 3, 2 },
    [0x5c] = { "Fn_add3", 6, 3 },
    [0x5d] = { "Fn_sub3", 6, 3 },
    [0x5e] = { "Fn_mul3", 4, 3 },
    [0x5f] = { "Fn_div3", 0, 0 },
    [0x60] = { "Fn_calc_unit_2", 0, 0 },
    [0x61] = { "Fn_calc_unit_1", 0, 0 },
    [0x62] = { "Fn_calc_unit_hara", 9, 0 },
    [0x63] = { "Fn_get_loc_pos", 7, 3 },
    [0x64] = { "Fn_2d_coli_put", 0, 0 },
    [0x65] = { "Fn_2d_coli_coli", 0, 0 },
    [0x66] = { "Fn_2d_coli_get", 0, 0 },
    [0x67] = { "Fn_st_glb_mat", 1, 0 },
    [0x68] = { "Fn_ld_glb_mat", 1, 0 },
    [0x69] = { "Fn_mul_mot_yrot", -1, -1 },
    [0x6a] = { "Fn_glo_to_loc", 3, 3 },
    [0x6b] = { "Fn_calc_unit_2_fast", 17, 1 },
    [0x6c] = { "Fn_x_rot_e", 0, 0 },
    [0x6d] = { "Fn_y_rot_e", 0, 0 },
    [0x6e] = { "Fn_z_rot_e", 0, 0 },
    [0x6f] = { "Fn_trans_e", 0, 0 },
    [0x70] = { "Fn_area_coli", 1, 0 },
    [0x71] = { "Fn_ball_to_unit", -1, -1 },
    [0x72] = { "Fn_outside_ball", 3, 0 },
    [0x73] = { "Fn_kage_mat", 2, 0 },
    [0x74] = { "Fn_kage_poly", 5, 0 },
    [0x75] = { "Fn_kage_flag", 6, 0 },
    [0x76] = { "Fn_get_glo_ang_zyx", -1, -1 },
    [0x77] = { "Fn_parts_oidasi", 4, 9 },
    [0x78] = { "Fn_put_poly", 8, 2 },
    [0x79] = { "Fn_ziku_rot", -1, -1 },
    [0x7a] = { "Fn_mul_matrix3", 9, 0 },
    [0x7b] = { "Fn_scrn_clip", -1, -1 },
    [0x7c] = { "Fn_load_inner_3x3", -1, -1 },
    [0x7d] = { "Fn_store_inner_3x3", -1, -1 },
    [0x7e] = { "Fn_mul_matrix_inner3", 2, 0 },
    [0x7f] = { "Fn_coli_copy_unit_matrix", 1, 0 },
    [0x80] = { "Fn_zanzou_reserve", -1, -1 },
    [0x81] = { "Fn_zanzou_init", 0, 0 },
    [0x82] = { "Fn_zanzou_inc", 0, 1 },
    [0x83] = { "Fn_zanzou_load_matrix_inner", -1, -1 },
    [0x84] = { "Fn_zanzou_mul_matrix_inner", 1, 0 },
    [0x85] = { "Fn_zanzou_get_info", 1, 0 },
    [0x86] = { "Fn_zanzou_kill_timer_buffer", 1, 0 },
    [0x87] = { "Fn_zanzou_get_matrix_inner", -1, -1 },
};

#define MAX_IO 256

typedef struct {
    uint64_t count, words, exact, close, wrong, missing, extra, argcount_bad;
    int      in_min, in_max, out_min, out_max, hle_out_min, hle_out_max;
    int      examples;
} op_stat_t;

static op_stat_t S[256];

/* State checks: after command PP, the firmware's matrix against the HLE's. */
typedef struct { uint64_t n, bad; double rmax, tmax; int examples; } state_stat_t;
static state_stat_t ST[256];
static uint8_t   g_bufram[BUFF_RAM_SIZE];

static float b2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static int word_close(uint32_t a, uint32_t b) {
    if (a == b) return 1;
    int ai = a <= 0xFFFFu || a >= 0xFFFF0000u, bi = b <= 0xFFFFu || b >= 0xFFFF0000u;
    if (ai && bi) return abs((int32_t)a - (int32_t)b) <= 1;
    float fa = b2f(a), fb = b2f(b);
    return isfinite(fa) && isfinite(fb) && fabsf(fa - fb) <= 1e-5f * fmaxf(1.0f, fabsf(fa));
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: cop_replay <prefix> [examples] [op]\n"); return 2; }
    int max_ex = argc > 2 ? atoi(argv[2]) : 3;
    int only = argc > 3 ? (int)strtol(argv[3], NULL, 16) : -1;
    char path[1024];

    static uint8_t *rom = NULL;
    const char *rom_path = getenv("COPRO_ROM");
    if (rom_path) {
        FILE *fr = fopen(rom_path, "rb");
        if (fr) {
            rom = (uint8_t *)malloc(0x800000);
            size_t n = fread(rom, 1, 0x800000, fr); fclose(fr);
            g_sharc_copro_rom = rom; g_sharc_copro_rom_size = n;
            printf("COP data ROM: %zu bytes\n", n);
        }
    }

    snprintf(path, sizeof path, "%s.bufram.bin", argv[1]);
    FILE *fb = fopen(path, "rb");
    if (fb) { size_t got = fread(g_bufram, 1, sizeof g_bufram, fb); fclose(fb); printf("bufferram snapshot: %zu bytes\n", got); }

    snprintf(path, sizeof path, "%s.bin", argv[1]);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }

    cop_reset();
    g_sharc.sharc_dm_ext      = g_bufram;
    g_sharc.sharc_dm_ext_size = BUFF_RAM_SIZE;
    for (int i = 0; i < 256; i++) {
        S[i].in_min = S[i].out_min = S[i].hle_out_min = 1 << 30;
        S[i].in_max = S[i].out_max = S[i].hle_out_max = -1;
    }

    uint32_t cmd = 0, args[MAX_IO], outs[MAX_IO];
    int have = 0, nin = 0, nout = 0;
    uint64_t rec = 0, ncmd = 0, bufw = 0, bad_words = 0, snaps = 0;
    int resync = getenv("RESYNC") != NULL;
    const char *draws_path = getenv("DRAWS");
    FILE *fd = draws_path ? fopen(draws_path, "w") : NULL;
    if (fd) fprintf(fd, "record,cmd,prev,depth,model,m0,m1,m2,m3,m4,m5,m6,m7,m8,m9,m10,m11,h0,h1,h2,h3,h4,h5,h6,h7,h8,h9,h10,h11\n");
    uint32_t snap_tag = 0, snap_ptr = 0, snap_words[12]; int snap_n = -1;
    float hsnap[12]; int draw_pending = 0; uint64_t draw_rec = 0; uint32_t draw_tag = 0, draw_ptr = 0, draw_words[12];

    #define MINMAX(lo, hi, v) do { if ((v) < (lo)) (lo) = (v); if ((v) > (hi)) (hi) = (v); } while (0)

    /* Run the command assembled so far and grade what it produced. */
    #define FINISH() do { if (have) {                                                   \
        op_stat_t *s = &S[cmd & 0xFF];                                                  \
        s->count++;                                                                     \
        MINMAX(s->in_min, s->in_max, nin);                                              \
        MINMAX(s->out_min, s->out_max, nout);                                           \
        if (sharc_args_for_cmd(cmd) != nin) s->argcount_bad++;                          \
        sharc_exec(cmd, args, nin < COP_ARGS_MAX ? nin : COP_ARGS_MAX);                 \
        int hn = g_sharc.reply_count;                                                   \
        MINMAX(s->hle_out_min, s->hle_out_max, hn);                                     \
        int bad = 0;                                                                    \
        for (int k = 0; k < nout || k < hn; k++) {                                      \
            if (k >= hn)        { s->missing++; bad = 1; continue; }                    \
            if (k >= nout)      { s->extra++;   bad = 1; continue; }                    \
            s->words++;                                                                 \
            uint32_t h = g_sharc.reply[k];                                              \
            if (h == outs[k]) s->exact++;                                               \
            else if (word_close(outs[k], h)) s->close++;                                \
            else { s->wrong++; bad = 1; }                                               \
        }                                                                               \
        g_sharc.reply_count = g_sharc.reply_idx = 0;                                    \
        if (draw_pending && (cmd & 0xFF) == 0x78 && fd) {                               \
            fprintf(fd, "%llu,%02X,%02X,%d,%08X", (unsigned long long)draw_rec,         \
                    draw_tag & 0xFF, (draw_tag >> 8) & 0xFF,                            \
                    (int)((draw_ptr - 0x305A0u) / 12u), nin > 4 ? args[4] : 0);         \
            for (int q = 0; q < 12; q++) fprintf(fd, ",%.6g", b2f(draw_words[q]));      \
            for (int q = 0; q < 12; q++) fprintf(fd, ",%.6g", hsnap[q]);                \
            fprintf(fd, "\n");                                                         \
        }                                                                               \
        draw_pending = 0;                                                               \
        if (bad && s->examples < max_ex && (only < 0 || only == (int)(cmd & 0xFF))) {   \
            s->examples++;                                                              \
            printf("\n[op %02X %s] command #%llu (record %llu): %d args, board wrote %d, hle %d\n  args:", \
                   cmd & 0xFF, FW[cmd & 0xFF].name ? FW[cmd & 0xFF].name : "?",         \
                   (unsigned long long)ncmd, (unsigned long long)rec, nin, nout, hn);   \
            for (int k = 0; k < nin && k < 24; k++) printf(" %08X(%g)", args[k], b2f(args[k])); \
            printf("\n  board:");                                                       \
            for (int k = 0; k < nout && k < 24; k++) printf(" %08X(%g)", outs[k], b2f(outs[k])); \
            printf("\n  hle:  ");                                                       \
            for (int k = 0; k < hn && k < 24; k++) printf(" %08X(%g)", g_sharc.reply[k], b2f(g_sharc.reply[k])); \
            printf("\n");                                                               \
        }                                                                               \
        have = 0; } } while (0)

    uint32_t buf[2 * 8192];
    size_t got;
    while ((got = fread(buf, 8, 8192, f)) > 0) {
        for (size_t k = 0; k < got; k++, rec++) {
            uint32_t tag = buf[2 * k], val = buf[2 * k + 1];
            if (tag == 0x21000000u) {
                FINISH();
                uint32_t op = val & 0xFF;
                if (val != op * 0x00800101u) { bad_words++; continue; }
                cmd = val; nin = nout = 0; have = 1; ncmd++;
            } else if (tag == 0x20000000u) {
                if (have && nin < MAX_IO) args[nin++] = val;
            } else if (tag == 0x30000000u) {
                if (have && nout < MAX_IO) outs[nout++] = val;
            } else if ((tag & 0xFF000000u) == 0x40000000u) {
                FINISH();                     /* command PP has run: grade the state it left */
                snap_tag = tag; snap_ptr = val; snap_n = 0;
            } else if (tag == 0x41000000u) {
                if (snap_n >= 0 && snap_n < 12) snap_words[snap_n++] = val;
                if (snap_n == 12) {
                    snap_n = -1; snaps++;
                    uint32_t prev = (snap_tag >> 8) & 0xFF, next = snap_tag & 0xFF;
                    float h[12];
                    for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) h[c*3 + r] = g_sharc.rot[c][r];
                    for (int r = 0; r < 3; r++) h[9 + r] = g_sharc.pos[r];
                    double rmax = 0, tmax = 0;
                    for (int q = 0; q < 9; q++) { double d = fabs((double)b2f(snap_words[q]) - h[q]); if (d > rmax) rmax = d; }
                    for (int q = 9; q < 12; q++) { double d = fabs((double)b2f(snap_words[q]) - h[q]); if (d > tmax) tmax = d; }
                    state_stat_t *t = &ST[prev];
                    t->n++;
                    if (rmax > t->rmax) t->rmax = rmax;
                    if (tmax > t->tmax) t->tmax = tmax;
                    if (rmax > 1e-3 || tmax > 1e-3) {
                        t->bad++;
                        if (t->examples < max_ex && (only < 0 || only == (int)prev)) {
                            t->examples++;
                            printf("\n[state after op %02X %s, before %02X] record %llu depth %d: rot err %.4g, T err %.4g\n  board:",
                                   prev, FW[prev].name ? FW[prev].name : "?", next, (unsigned long long)rec,
                                   (int)((snap_ptr - 0x305A0u) / 12u), rmax, tmax);
                            for (int q = 0; q < 12; q++) printf(" %.5g", b2f(snap_words[q]));
                            printf("\n  hle:  ");
                            for (int q = 0; q < 12; q++) printf(" %.5g", h[q]);
                            printf("\n");
                        }
                    }
                    memcpy(hsnap, h, sizeof hsnap);
                    if (next == 0x78) { draw_pending = 1; draw_rec = rec; draw_tag = snap_tag; draw_ptr = snap_ptr; memcpy(draw_words, snap_words, sizeof draw_words); }
                    if (resync) {
                        for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) g_sharc.rot[c][r] = b2f(snap_words[c*3 + r]);
                        for (int r = 0; r < 3; r++) g_sharc.pos[r] = b2f(snap_words[9 + r]);
                        g_sharc.matrix_dirty = true; g_sharc.bone_dirty = true;
                    }
                }
            } else if (tag >= 0x900000u && tag < 0x980000u) {
                bufw++;
                uint32_t off = (tag - 0x900000u) & (BUFF_RAM_SIZE - 1);
                memcpy(g_bufram + (off & ~3u), &val, 4);
            }
        }
    }
    FINISH();
    fclose(f);

    if (fd) fclose(fd);
    printf("\nrecords %llu: %llu commands, %llu bufferram writes, %llu malformed command words, %llu matrix snapshots%s\n\n",
           (unsigned long long)rec, (unsigned long long)ncmd, (unsigned long long)bufw, (unsigned long long)bad_words,
           (unsigned long long)snaps, resync ? " (HLE matrix reset to the board's at each)" : "");
    if (snaps) {
        printf(" state left by   name                          checks      bad   max rot err   max T err\n");
        for (int op = 0; op < 256; op++) {
            state_stat_t *t = &ST[op];
            if (!t->n) continue;
            printf("%c%02X             %-27s %8llu %8llu   %10.4g  %10.4g\n", t->bad ? '*' : ' ', op,
                   FW[op].name ? FW[op].name : "?", (unsigned long long)t->n, (unsigned long long)t->bad, t->rmax, t->tmax);
        }
        printf("\n");
    }
    printf(" op  name                         count  args(board) hle-table  out(board) out(hle)   words    exact    close    WRONG  missing  extra\n");
    for (int op = 0; op < 256; op++) {
        op_stat_t *s = &S[op];
        if (!s->count) continue;
        uint32_t word = (uint32_t)op * 0x00800101u;
        int hle_in = sharc_args_for_cmd(word);
        char ain[16], aout[16], hout[16];
        snprintf(ain, sizeof ain, s->in_min == s->in_max ? "%d" : "%d..%d", s->in_min, s->in_max);
        snprintf(aout, sizeof aout, s->out_min == s->out_max ? "%d" : "%d..%d", s->out_min, s->out_max);
        snprintf(hout, sizeof hout, s->hle_out_min == s->hle_out_max ? "%d" : "%d..%d", s->hle_out_min, s->hle_out_max);
        int flag = (s->argcount_bad || s->wrong || s->missing || s->extra);
        printf("%c%02X  %-27s %7llu  %-10s %4d%s   %-9s %-9s %8llu %8llu %8llu %8llu %8llu %6llu\n",
               flag ? '*' : ' ', op, FW[op].name ? FW[op].name : "?", (unsigned long long)s->count,
               ain, hle_in, s->argcount_bad ? "!" : " ", aout, hout,
               (unsigned long long)s->words, (unsigned long long)s->exact, (unsigned long long)s->close,
               (unsigned long long)s->wrong, (unsigned long long)s->missing, (unsigned long long)s->extra);
    }
    return 0;
}
