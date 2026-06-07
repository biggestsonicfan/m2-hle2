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

#define SHARC_REPLY_MAX       32
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
