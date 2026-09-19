/*
 * sharc.h — ADSP-21060 SHARC geometry coprocessor HLE state (board-level).
 *
 * The ADSP-21060 SHARC is the geometry coprocessor on the Model 2 board.
 * cop.h owns the i960↔SHARC FIFO interface (arg accumulator, geo_capture ring).
 * This file owns the SHARC's internal computation state and math helpers.
 *
 * Structured after the i960.h / i960_exec.h split:
 *   sharc.h      — state definition and math / reply helpers
 *   sharc_exec.h — command dispatch and handlers (analogous to i960_exec.h)
 *
 * Invariants (per CLAUDE.md — do NOT re-derive):
 *
 *   Rotation: accumulated column-major rot[3][3] post-multiplied per ang command.
 *     ang_y (0x04800909→0x201BF): new col0 = c·col0 + s·col2,  new col2 = −s·col0 + c·col2.
 *     ang_x (0x04000808→0x201AA): new col1 = c·col1 − s·col2,  new col2 = s·col1 + c·col2.
 *     ang_z (0x05000A0A→0x201D4): new col0 = c·col0 − s·col1,  new col1 = s·col0 + c·col1.
 *   (Exact SHARC firmware formulas; CLAUDE.md had ang_x/ang_z PM addresses swapped.)
 *
 *   Angles: signed 16-bit fixed-point, 0x10000 = 360°.
 *
 *   World-pos snapshot: 0x04800909 (set Y angle) snapshots pos[] → world_pos[].
 *   0x07800F0F returns world_pos[], NOT pos[].
 *
 *   Z-negation convention for 0x14802929 / 0x35006A6A:
 *     rx = M[0]·(ix, iy, −iz) + T[0]
 *     ry = M[1]·(ix, iy, −iz) + T[1]
 *     rz = M[2]·(ix, iy, +iz) + T[2]   ← z row uses raw iz, NOT negated
 *   (Verified cases 6 and 9 in verify_14802929_mame.py against MAME SHARC.)
 *
 *   Bone scratch: column-major [col0|col1|col2|T], evolves across successive
 *   0x35806B6B calls; reset to dirty when main matrix changes.
 */
#ifndef SHARC_H
#define SHARC_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>

#include "constants.h"
#include "../core/log.h"

/* ---- Limits -------------------------------------------------------------- */

#define SHARC_REPLY_MAX       1024  /* Fn_osage answers 13 words a sway segment in one command */
#define SHARC_UNKNOWN_LOG_MAX 64

/* ---- State --------------------------------------------------------------- */

typedef struct {
    /* Reply FIFO — filled by sharc_exec, drained by cop_read (cop.h). */
    uint32_t reply[SHARC_REPLY_MAX];
    int      reply_count;
    int      reply_idx;

    /* SHARC internal register state — set by setter commands. */
    /* pos[]: accumulated world-space translation T.
     *   0x01800303 (identity) resets to (0,0,0).
     *   0x03000606 (set_pos)  does T += rot × args  (additive, not replacement).
     * Used as the T column of the matrix. */
    float    pos[3];
    /* Accumulated rotation matrix — column-major, matching SHARC memory layout.
     *   rot[col][row], indices: rot[0]=col0(ix), rot[1]=col1(iy), rot[2]=col2(iz).
     *   Initialised to identity on reset / 0x01800303.
     *   ang_y (0x04800909→0x201BF): new col0 = c*col0 + s*col2, new col2 = −s*col0 + c*col2.
     *   ang_x (0x04000808→0x201AA): new col1 = c*col1 − s*col2, new col2 = s*col1 + c*col2.
     *   ang_z (0x05000A0A→0x201D4): new col0 = c*col0 − s*col1, new col1 = s*col0 + c*col1.
     *   Verified from dispatch table at DM[0x30000]: index=cmd>>23, handler calls PM sub.
     *   Note: CLAUDE.md had ang_x/ang_z PM addresses and formulas attributed to wrong cmds. */
    float    rot[3][3];

    /* Debug shadow of last angle set for each command — NOT used for matrix building. */
    int32_t  ang[3];        /* X=0x04000808  Y=0x04800909  Z=0x05000A0A */

    /* Snapshot of pos[] taken when Y angle is set (0x04800909).
     * Not returned by 0x07800F0F (which returns rotation entries slot[1..3]),
     * but kept as a diagnostic / stack save/restore value. */
    float    world_pos[3];

    /* 3×4 rotation+translation matrix, row-major:
     *   row0 = [R00 R01 R02 Tx]
     *   row1 = [R10 R11 R12 Ty]
     *   row2 = [R20 R21 R22 Tz]
     * Rebuilt lazily from rot[]/pos[] when matrix_dirty is set.
     *
     * Z-negation convention (rows 0/1 only): m[r][2] = −rot[2][r] for r∈{0,1},
     * but m[2][2] = rot[2][2].  This compensates for the z-negation the i960
     * applies to the input iz before 0x14802929 / 0x35006A6A (rows 0/1 negate,
     * row 2 does not), making our row-major transform produce identical output
     * to the SHARC's raw column-major multiply. */
    float    matrix[3][4];
    bool     matrix_dirty;

    /* Matrix stack for 0x00800101 (push) / 0x01000202 (pop). */
#define SHARC_STACK_DEPTH 8
    struct {
        float   rot[3][3];   /* accumulated rotation (column-major, same as sharc) */
        float   pos[3];
        float   world_pos[3];
        int32_t ang[3];      /* debug shadow */
    } stack[SHARC_STACK_DEPTH];
    int stack_top;

    /* Running bone-chain scratch state for 0x35806B6B (calc_rob_angle_cont).
     *   bone_col[0..8] — column-major 3×3 rotation
     *   bone_T[0..2]   — world-space joint position
     *   bone_dirty     — re-initialise from matrix on next call
     * Reset to dirty whenever the main matrix changes. */
    float bone_col[9];
    float bone_T[3];
    bool  bone_dirty;

    /* TGP bone slot storage (written by 0x35806B6B, read by geometry decoder).
     *   P1 bone N → slot N    (TGP addrs 0x3A30..0x3AB4)
     *   P2 bone N → slot 16+N (TGP addrs 0x3B30..0x3BB4)
     * Each slot: col0(xyz)|col1(xyz)|col2(xyz)|T(xyz) — column-major 3×4. */
    float tgp_bone[32][12];

    /* Bone matrix cache — SHARC DM[0x30420..0x305A0].
     * Written by 0x1A803535 (save current matrix → slot).
     * Read by 0x1B003636 (plain load) and 0x1B803737 (load + C×B multiply).
     *   P1 slots 0..15 → rot_cache[0..15]
     *   P2 slots 0..15 → rot_cache[16..31]
     * Layout per slot: word[col*3+row] = rot[col][row], words[9..11] = pos[0..2]. */
    float rot_cache[32][12];

    /* PM bone scratch — written by 0x22004444/0x21804343 (save bone to slot N),
     * read by 0x22804545 (load bone from slot N) and 0x23004646 (bone × pm_bone[N]).
     * Mirrors SHARC PM[0x21F20..0x21FFF]: stride 12, up to 16 slots.
     * Layout per slot: [col0(xyz)|col1(xyz)|col2(xyz)|T(xyz)] — col-major 3×4. */
    float pm_bone[16][12];

    /* Shadow projection matrix saved by 0x33806767 (inside make_kage_matrix push/pop).
     * Layout: [col][row], same column-major convention as rot[][]. */
    float shadow_rot[3][3];

    /* Pointer to shared animation data buffer (i960 BUFF_RAM = SHARC DM[0x01400000]).
     * Set by mem_init after BUFF_RAM is allocated; zeroed by cop_reset (set again by
     * mem_init afterwards).  Command 0x25004A4A (read_anim_data) streams typed blocks
     * from here: type 1 loads a 12-word col-major matrix into the current slot. */
    uint8_t *sharc_dm_ext;       /* points to memory_bus_t::buff_ram */
    uint32_t sharc_dm_ext_size;  /* bytes (BUFF_RAM_SIZE) */

    /* Active collision-ball buffer base (set by 0x1C003838).
     * P1 = 0xFA00, P2 = 0x1FA00 (byte offset into sharc_dm_ext). */
    uint32_t coli_buf_base;

    /* The firmware's own data memory, DM 0x30000..0x32FFF, one word each —
     * what Fn_write_ram (0x49) fills and Fn_read_ram (0x48) reads, and the
     * state the collision commands hand each other (sharc_dm_get/set; the
     * unit-matrix cache at 0x30420..0x3059F is rot_cache, not this array).
     * DM 0x30000..0x377FF: Fn_zanzou_init clears 0x5480 words from 0x32180,
     * and the afterimage ring reaches 0x332FF, so 0x3000 words is not enough. */
    uint32_t dm[0x7800];

    /* Activity counters. */
    uint32_t unknown_cmds;
    uint32_t transform_count;
    uint32_t matrix_read_count;

    /* IP of most recent call to each key command (zero = not yet seen). */
    uint32_t ip_set_pos;
    uint32_t ip_set_ang_x;
    uint32_t ip_set_ang_y;
    uint32_t ip_set_ang_z;
    uint32_t ip_read_matrix;
    uint32_t ip_rot_transform;
    uint32_t ip_full_transform;
    uint32_t ip_sin_scale;
    uint32_t ip_cos_scale;
    uint32_t ip_atan2;

    /* Break-on-unknown command control. */
    volatile int break_on_unknown;
    volatile int unknown_triggered;
    uint32_t     unknown_trigger_cmd;
    uint32_t     unknown_trigger_ip;

    /* Deduplicated log of unknown commands seen this session. */
    struct {
        uint32_t cmd;
        uint32_t first_ip;
        uint32_t count;
    } unknown_log[SHARC_UNKNOWN_LOG_MAX];
    int unknown_log_count;

    /* Diagnostic snapshot of most recent 0x14802929 transform. */
    float   dbg_xform_pos[3];
    int32_t dbg_xform_ang[3];
    float   dbg_xform_in[3];
    float   dbg_xform_out[3];
} sharc_state_t;

static sharc_state_t g_sharc = {0};

/* ---- Bit-reinterpret helpers -------------------------------------------- */

static inline float sharc_bits_to_float(uint32_t u) {
    float f; memcpy(&f, &u, 4); return f;
}
static inline uint32_t sharc_float_to_bits(float f) {
    uint32_t u; memcpy(&u, &f, 4); return u;
}

/* Signed 16-bit fixed-point angle (0x10000 = 360°) → radians. */
static inline float sharc_angle_to_rad(int32_t fp) {
    int16_t a = (int16_t)(fp & 0xFFFF);
    return ((float)a / 65536.0f) * (2.0f * 3.14159265358979f);
}

/* COP data ROM, as the SHARC sees it at DM 0x1C00000 (one little-endian float
 * a word). Set once by the game profile; lives outside g_sharc so cop_reset()
 * leaves it alone. NULL on a board/profile that did not load one. */
static const uint8_t *g_sharc_copro_rom      = NULL;
static size_t         g_sharc_copro_rom_size = 0;

/* sin/cos of a signed 16-bit angle the way the firmware takes them (_L202C1):
 * straight out of the ROM, sin at DM 0x1C10000 + angle, cos 0x20000 further on.
 * The tables are rounded to six decimals, so cos(0x4000) is exactly 0 and
 * cos(-0x4000) is -1e-6 — values the i960 reads back, forwards and branches
 * on, which cosf() (-4.37e-8) does not reproduce. Falls back to libm without
 * a ROM. */
static inline void sharc_sincos(int32_t angle, float *s, float *c) {
    int32_t a = (int16_t)(angle & 0xFFFF);
    if (g_sharc_copro_rom && g_sharc_copro_rom_size >= 0x38000u * 4u) {
        memcpy(s, g_sharc_copro_rom + (size_t)(0x10000 + a) * 4u, 4);
        memcpy(c, g_sharc_copro_rom + (size_t)(0x30000 + a) * 4u, 4);
        return;
    }
    float r = sharc_angle_to_rad(angle);
    *s = sinf(r);
    *c = cosf(r);
}

/* ---- The coprocessor's own arithmetic ------------------------------------
 *
 * The firmware never takes an exact square root, reciprocal or arctangent. It
 * seeds from the chip's RSQRTS / RECIPS tables (8 bits of mantissa) and runs
 * three Newton steps, and its atan2 is Analog Devices' runtime routine: range
 * reduction by those divides and a rational minimax polynomial. The answers
 * agree with sqrtf / 1/x / atan2f to six digits and differ in the low bits,
 * and the i960 feeds them back: a fighter's facing, its distance to the other
 * and the push-out between them go into next frame's positions. Host libm
 * drifted the first attract fight off the board within ~350 frames and turned
 * a thrown grab around 690 frames in, so each helper below is the firmware's
 * register sequence, operation for operation (cpres1.asm), with the seeds from
 * MAME's adsp21062 (compute.hxx). */

static const uint32_t k_sharc_recips_lut[128] = {
    0x007f8000, 0x007e0000, 0x007c0000, 0x007a0000, 0x00780000, 0x00760000, 0x00740000, 0x00720000,
    0x00700000, 0x006f0000, 0x006d0000, 0x006b0000, 0x006a0000, 0x00680000, 0x00660000, 0x00650000,
    0x00630000, 0x00610000, 0x00600000, 0x005e0000, 0x005d0000, 0x005b0000, 0x005a0000, 0x00590000,
    0x00570000, 0x00560000, 0x00540000, 0x00530000, 0x00520000, 0x00500000, 0x004f0000, 0x004e0000,
    0x004c0000, 0x004b0000, 0x004a0000, 0x00490000, 0x00470000, 0x00460000, 0x00450000, 0x00440000,
    0x00430000, 0x00410000, 0x00400000, 0x003f0000, 0x003e0000, 0x003d0000, 0x003c0000, 0x003b0000,
    0x003a0000, 0x00390000, 0x00380000, 0x00370000, 0x00360000, 0x00350000, 0x00340000, 0x00330000,
    0x00320000, 0x00310000, 0x00300000, 0x002f0000, 0x002e0000, 0x002d0000, 0x002c0000, 0x002b0000,
    0x002a0000, 0x00290000, 0x00280000, 0x00280000, 0x00270000, 0x00260000, 0x00250000, 0x00240000,
    0x00230000, 0x00230000, 0x00220000, 0x00210000, 0x00200000, 0x001f0000, 0x001f0000, 0x001e0000,
    0x001d0000, 0x001c0000, 0x001c0000, 0x001b0000, 0x001a0000, 0x00190000, 0x00190000, 0x00180000,
    0x00170000, 0x00170000, 0x00160000, 0x00150000, 0x00140000, 0x00140000, 0x00130000, 0x00120000,
    0x00120000, 0x00110000, 0x00100000, 0x00100000, 0x000f0000, 0x000f0000, 0x000e0000, 0x000d0000,
    0x000d0000, 0x000c0000, 0x000c0000, 0x000b0000, 0x000a0000, 0x000a0000, 0x00090000, 0x00090000,
    0x00080000, 0x00070000, 0x00070000, 0x00060000, 0x00060000, 0x00050000, 0x00050000, 0x00040000,
    0x00040000, 0x00030000, 0x00030000, 0x00020000, 0x00020000, 0x00010000, 0x00010000, 0x00000000,
};

static const uint32_t k_sharc_rsqrts_lut[128] = {
    0x00350000, 0x00330000, 0x00320000, 0x00300000, 0x002f0000, 0x002e0000, 0x002d0000, 0x002b0000,
    0x002a0000, 0x00290000, 0x00280000, 0x00270000, 0x00260000, 0x00250000, 0x00230000, 0x00220000,
    0x00210000, 0x00200000, 0x001f0000, 0x001e0000, 0x001e0000, 0x001d0000, 0x001c0000, 0x001b0000,
    0x001a0000, 0x00190000, 0x00180000, 0x00170000, 0x00160000, 0x00160000, 0x00150000, 0x00140000,
    0x00130000, 0x00130000, 0x00120000, 0x00110000, 0x00100000, 0x00100000, 0x000f0000, 0x000e0000,
    0x000e0000, 0x000d0000, 0x000c0000, 0x000b0000, 0x000b0000, 0x000a0000, 0x000a0000, 0x00090000,
    0x00080000, 0x00080000, 0x00070000, 0x00070000, 0x00060000, 0x00050000, 0x00050000, 0x00040000,
    0x00040000, 0x00030000, 0x00030000, 0x00020000, 0x00020000, 0x00010000, 0x00010000, 0x00000000,
    0x007f8000, 0x007e0000, 0x007c0000, 0x007a0000, 0x00780000, 0x00760000, 0x00740000, 0x00730000,
    0x00710000, 0x006f0000, 0x006e0000, 0x006c0000, 0x006a0000, 0x00690000, 0x00670000, 0x00660000,
    0x00640000, 0x00630000, 0x00620000, 0x00600000, 0x005f0000, 0x005e0000, 0x005c0000, 0x005b0000,
    0x005a0000, 0x00590000, 0x00570000, 0x00560000, 0x00550000, 0x00540000, 0x00530000, 0x00520000,
    0x00510000, 0x004f0000, 0x004e0000, 0x004d0000, 0x004c0000, 0x004b0000, 0x004a0000, 0x00490000,
    0x00480000, 0x00470000, 0x00460000, 0x00450000, 0x00450000, 0x00440000, 0x00430000, 0x00420000,
    0x00410000, 0x00400000, 0x003f0000, 0x003e0000, 0x003e0000, 0x003d0000, 0x003c0000, 0x003b0000,
    0x003a0000, 0x003a0000, 0x00390000, 0x00380000, 0x00370000, 0x00370000, 0x00360000, 0x00350000,
};

#define SHARC_FLOAT_NAN 0xFFFFFFFFu   /* MAME's FLOAT_CANONICAL_NAN */

static inline int sharc_float_is_nan(uint32_t v)  { return (v & 0x7F800000u) == 0x7F800000u && (v & 0x007FFFFFu); }
static inline int sharc_float_is_zero(uint32_t v) { return (v & 0x7FFFFFFFu) == 0; }

/* Fn = RECIPS Fx */
static inline float sharc_recips(float x) {
    uint32_t v = sharc_float_to_bits(x), sign = v & 0x80000000u, r;
    if (sharc_float_is_nan(v))       r = SHARC_FLOAT_NAN;
    else if (sharc_float_is_zero(v)) r = sign | 0x7F800000u;
    else {
        int32_t e = -((int32_t)((v >> 23) & 0xFFu) - 127) - 1;
        uint32_t m = k_sharc_recips_lut[(v & 0x007FFFFFu) >> 16];
        if (e > 125 || e < -126) { e = 0; m = 0; }
        else                     e = (e + 127) & 0xFF;
        r = sign | ((uint32_t)e << 23) | m;
    }
    return sharc_bits_to_float(r);
}

/* Fn = RSQRTS Fx */
static inline float sharc_rsqrts(float x) {
    uint32_t v = sharc_float_to_bits(x), r;
    if (v > 0x80000000u || sharc_float_is_nan(v)) r = SHARC_FLOAT_NAN;
    else {
        int32_t ue = (int32_t)((v >> 23) & 0xFFu) - 127;
        int32_t e  = -(ue >> 1) - 1;
        r = (v & 0x80000000u) | ((uint32_t)((e + 127) & 0xFF) << 23) | k_sharc_rsqrts_lut[(v & 0xFFFFFFu) >> 17];
    }
    return sharc_bits_to_float(r);
}

/* Rn = LOGB Fx */
static inline int32_t sharc_logb(float x) {
    uint32_t v = sharc_float_to_bits(x);
    if ((v & 0x7FFFFFFFu) == 0x7F800000u) return 0x7F800000;
    if (sharc_float_is_zero(v))           return (int32_t)0xFF800000u;
    if (sharc_float_is_nan(v))            return (int32_t)SHARC_FLOAT_NAN;
    return (int32_t)((v >> 23) & 0xFFu) - 127;
}

/* _L205D0 and its inline copies: num / d by RECIPS and three Newton steps with
 * two = 2.0. *last gets the step register's final value, which atan2 goes on
 * to use. */
static inline float sharc_fw_div_ex(float num, float d, float *last) {
    float f3 = sharc_recips(d), f4 = num, f12 = f3 * d;
    f4 = f3 * f4; f3 = 2.0f - f12;
    f12 = f3 * f12; f4 = f3 * f4; f3 = 2.0f - f12;
    f12 = f3 * f12; f4 = f3 * f4; f3 = 2.0f - f12;
    if (last) *last = f3;
    return f3 * f4;
}
static inline float sharc_fw_div(float num, float d) { return sharc_fw_div_ex(num, d, NULL); }

/* _L2029B: 1/sqrt(x) by RSQRTS and three Newton steps; 0 for a zero input */
static inline float sharc_fw_rsqrt(float x) {
    if (sharc_float_to_bits(x) == 0) return 0.0f;
    float f4 = sharc_rsqrts(x), f12;
    for (int i = 0; i < 3; i++) {
        f12 = f4 * f4;
        f12 = x * f12;
        f4 = 0.5f * f4; f12 = 3.0f - f12;
        f4 = f4 * f12;
    }
    return f4;
}

/* _L202AE: sqrt(x) = x * the same 1/sqrt; a zero input comes back untouched */
static inline float sharc_fw_sqrt(float x) {
    if (sharc_float_to_bits(x) == 0) return x;
    float f4 = sharc_rsqrts(x), f15;
    for (int i = 0; i < 3; i++) {
        f15 = f4 * f4;
        f15 = x * f15;
        f4 = 0.5f * f4; f15 = 3.0f - f15;
        f4 = f4 * f15;
    }
    return x * f4;
}

/* _L202D1: atan2(y, x) in radians, [-pi, pi]. The coefficients are the ones
 * _L20154 writes to DM 0x30290 at boot. */
static inline float sharc_fw_atan2(float y, float x) {
    const float TAN15 = sharc_bits_to_float(0x3E8930A2u), SQRT3 = sharc_bits_to_float(0x3FDDB3D7u),
                EPS   = sharc_bits_to_float(0x39800000u),
                P0 = sharc_bits_to_float(0xBF3853ADu), P1 = sharc_bits_to_float(0xBFB854A7u),
                Q0 = sharc_bits_to_float(0x4098123Bu), Q1 = sharc_bits_to_float(0x408A3F7Du),
                HALF_PI = sharc_bits_to_float(0x3FC90FDAu), PI = sharc_bits_to_float(0x40490FDAu);
    const float OCTANT[4] = { 0.0f, sharc_bits_to_float(0x3F060A91u), HALF_PI, sharc_bits_to_float(0x3F860A91u) };
    uint32_t xb = sharc_float_to_bits(x), yb = sharc_float_to_bits(y);
    float f0 = y, f15;
    if (!(xb & 0x7F800000u)) {                               /* _L2032F: pass treats a denormal as zero */
        if (!(yb & 0x7F800000u)) return y;
        f15 = HALF_PI;
    } else {
        float f2 = (xb & 0x80000000u) ? PI : 0.0f;
        int32_t de = (int32_t)((uint32_t)sharc_logb(y) - (uint32_t)sharc_logb(x));
        if (de >= 0x7C) {
            f15 = HALF_PI;                                   /* _L2032C */
        } else {
            if (de <= -0x7C) {
                f15 = 0.0f;                                  /* _L20329 */
            } else {
                float f7;
                f0 = sharc_fw_div_ex(y, x, &f7);
                if (sharc_float_to_bits(f2) != 0) f0 = -f0;
                int k = 0;
                f15 = fabsf(f0);
                f7 = 1.0f;
                if (!(f15 <= f7)) { f15 = sharc_fw_div_ex(1.0f, f15, &f7); k = 2; }
                if (!(f15 < TAN15)) {                        /* _L202FD */
                    k++;
                    float f14 = SQRT3 * f15;
                    f7 = f14 - f7;
                    f15 = SQRT3 + f15;
                    f15 = sharc_fw_div_ex(f7, f15, &f7);
                }
                if (!(fabsf(f15) <= EPS)) {                  /* _L2030B */
                    float g = f15 * f15;
                    float num = g * P0; num = num + P1; num = num * g;
                    float den = g + Q0; den = den * g; den = den + Q1;
                    float r = sharc_fw_div(num, den);
                    r = r * f15;
                    f15 = r + f15;
                }
                if (k - 1 > 0) f15 = -f15;                   /* _L2031F */
                f15 = f15 + OCTANT[k];
            }
            if (sharc_float_to_bits(f2) != 0) f15 = f2 - f15; /* _L20324 */
        }
    }
    if (sharc_float_to_bits(f0) & 0x80000000u) f15 = -f15;   /* _L20326 */
    return f15;
}

/* An integral-valued float as the 32-bit integer `fix` produces. Out of range,
 * or NaN, is 0x80000000 on every host: a C cast leaves that undefined, x86 gives
 * 0x80000000 (as MAME's `compute_fix` does on x86) and AArch64 saturates. */
static inline uint32_t sharc_float_to_int32(float v) {
    if (!(v > -2147483649.0f && v < 2147483648.0f)) return 0x80000000u;
    return (uint32_t)(int32_t)v;
}

/* An angle the firmware hands back (_L202CA): radians times 0x4622F983
 * (32768/pi), `fix`ed under MODE1 TRUNCATE (MODE1 = 0x18000, so floor), then
 * the low 16 bits, zero-extended; callers read it with ldis. */
static inline uint32_t sharc_angle_word(float rad) {
    float scaled = rad * sharc_bits_to_float(0x4622F983u);
    return sharc_float_to_int32(floorf(scaled)) & 0xFFFFu;
}
static inline uint32_t sharc_fw_atan2_word(float y, float x) { return sharc_angle_word(sharc_fw_atan2(y, x)); }

/* ---- Reply staging ------------------------------------------------------- */

static inline void sharc_push_f(float v) {
    if (g_sharc.reply_count < SHARC_REPLY_MAX)
        g_sharc.reply[g_sharc.reply_count++] = sharc_float_to_bits(v);
}
static inline void sharc_push_u(uint32_t v) {
    if (g_sharc.reply_count < SHARC_REPLY_MAX)
        g_sharc.reply[g_sharc.reply_count++] = v;
}

/* ---- Rotation post-multiply helpers ------------------------------------- */

/* ang_y (0x04800909 / 0x201BF): post-multiply rot by Ry — modifies col0 and col2. */
static inline void sharc_postmul_ry(float c, float s) {
    float (*r)[3] = g_sharc.rot;
    int row;
    for (row = 0; row < 3; row++) {
        float c0 = r[0][row], c2 = r[2][row];
        r[0][row] =  c*c0 + s*c2;
        r[2][row] = -s*c0 + c*c2;
    }
}

/* ang_x (0x04000808 / 0x201AA): post-multiply rot by Rx — modifies col1 and col2. */
static inline void sharc_postmul_rx(float c, float s) {
    float (*r)[3] = g_sharc.rot;
    int row;
    for (row = 0; row < 3; row++) {
        float c1 = r[1][row], c2 = r[2][row];
        r[1][row] =  c*c1 - s*c2;
        r[2][row] =  s*c1 + c*c2;
    }
}

/* ang_z (0x05000A0A / 0x201D4): post-multiply rot by Rz — modifies col0 and col1. */
static inline void sharc_postmul_rz(float c, float s) {
    float (*r)[3] = g_sharc.rot;
    int row;
    for (row = 0; row < 3; row++) {
        float c0 = r[0][row], c1 = r[1][row];
        r[0][row] =  c*c0 - s*c1;
        r[1][row] =  s*c0 + c*c1;
    }
}

/* pre-multiply rot by Ry: rot = Ry × rot — modifies each column's x and z rows.
 * Used by 0x34806969 to apply a world-space facing angle to a bone matrix. */
static inline void sharc_premul_ry(float c, float s) {
    float (*r)[3] = g_sharc.rot;
    int col;
    for (col = 0; col < 3; col++) {
        float r0 = r[col][0], r2 = r[col][2];
        r[col][0] =  c*r0 - s*r2;
        r[col][2] =  s*r0 + c*r2;
    }
}

/* Reset rot[] to identity. */
static inline void sharc_rot_identity(void) {
    memset(g_sharc.rot, 0, sizeof(g_sharc.rot));
    g_sharc.rot[0][0] = g_sharc.rot[1][1] = g_sharc.rot[2][2] = 1.0f;
}

/* ---- Matrix builder ------------------------------------------------------ */

/*
 * Converts the column-major rot[3][3] (SHARC convention) to our row-major
 * matrix[3][4] (HLE convention) with z-negation:
 *   rows 0,1: m[r][2] = −rot[2][r]  (compensates for −iz in 14802929 rows 0/1)
 *   row  2:   m[2][2] =  rot[2][2]  (row 2 uses raw +iz, so no sign flip)
 * All other elements: m[r][c] = rot[c][r].
 */
static inline void sharc_build_matrix(void) {
    float (*r)[3] = g_sharc.rot;
    float (*m)[4] = g_sharc.matrix;

    m[0][0] =  r[0][0]; m[0][1] =  r[1][0]; m[0][2] = -r[2][0]; m[0][3] = g_sharc.pos[0];
    m[1][0] =  r[0][1]; m[1][1] =  r[1][1]; m[1][2] = -r[2][1]; m[1][3] = g_sharc.pos[1];
    m[2][0] =  r[0][2]; m[2][1] =  r[1][2]; m[2][2] =  r[2][2]; m[2][3] = g_sharc.pos[2];

    g_sharc.matrix_dirty = false;
}

static inline void sharc_transform_vec3(float ix, float iy, float iz,
                                         float *ox, float *oy, float *oz,
                                         bool with_translation) {
    if (g_sharc.matrix_dirty) sharc_build_matrix();
    float (*m)[4] = g_sharc.matrix;
    *ox = m[0][0]*ix + m[0][1]*iy + m[0][2]*iz + (with_translation ? m[0][3] : 0.0f);
    *oy = m[1][0]*ix + m[1][1]*iy + m[1][2]*iz + (with_translation ? m[1][3] : 0.0f);
    *oz = m[2][0]*ix + m[2][1]*iy + m[2][2]*iz + (with_translation ? m[2][3] : 0.0f);
}

#endif /* SHARC_H */
