/*
 * sharc_coli.h — the COP's collision commands, ported from the firmware
 * (stf-sharc cpres1.asm; a `PM 0x2xxxx` below is an address there).
 *
 * Fight collision is a chain the i960 drives every frame, and each link leaves
 * state in the COP's memory for the next:
 *
 *   calc_unit_mat, per fighter:
 *     0x7F Fn_coli_copy_unit_matrix   last frame's unit matrices -> DM 0x32000
 *     0x38 Fn_coli_set_ball_adrs      which ball table the next 0x39s write
 *     0x39 Fn_coli_point_trans  xN    each ball local -> world, at 0x1403E80/0x1407E80,
 *                                     the position it replaces kept 0x60 words on
 *   coli_cont_cop:
 *     0x3E Fn_coli_trans_xz           both fighters' balls, current and previous, into
 *                                     a frame with P0 at the origin and P1 on +x
 *     0x3A Fn_area_table_gen          64-bin broad phase per axis -> candidate masks
 *     0x70 Fn_area_coli  x2           each fighter's balls against the arena: 22 replies
 *     0x3B Fn_calc_coli_flag          narrow phase: 4 replies (tested, hits, push depth,
 *                                     lift) and the unit-overlap table at 0x1403D80 that
 *                                     coli_attack_chk reads (i960 0x90F600) — every melee
 *                                     and throw contact comes out of this
 *     0x3D Fn_coli_trans_mat          the push-out applied to the unit matrices
 *     0x72 Fn_outside_ball x2         balls outside the arena square
 *
 *   and outside the chain, off the world balls 0x39 left:
 *     0x77 Fn_parts_oidasi            one loose sphere (a projectile, a flying part)
 *                                     against both fighters: 9 replies, the unit masks
 *                                     being every projectile hit
 *
 * The tables the chain reads (radii, radius scale, ball->unit maps, pointer
 * tables) are uploaded by the i960 with Fn_write_ram at boot and on character
 * load, so DM writes have to land (g_sharc.dm, sharc_dm_get/set).
 *
 * The firmware's floats are IEEE singles; so are these. Its `fix` truncates
 * (MODE1 TRUNCATE). Zero tests on floats are on the bits (-0.0 is not zero).
 */
#ifndef SHARC_COLI_H
#define SHARC_COLI_H

#include <math.h>
#include <string.h>
#include "sharc.h"

/* ---- the firmware's address space ----------------------------------------- */

static inline uint32_t sharc_dm_get(uint32_t a) {
    if (a >= 0x30420u && a < 0x305A0u) {                 /* unit-matrix cache */
        uint32_t o = a - 0x30420u;
        return sharc_float_to_bits(g_sharc.rot_cache[o / 12u][o % 12u]);
    }
    if (a >= 0x30000u && a < 0x33000u) return g_sharc.dm[a - 0x30000u];
    if (a >= 0x1400000u && g_sharc.sharc_dm_ext) {        /* BUFF_RAM, shared with the i960 */
        uint64_t o = (uint64_t)(a - 0x1400000u) * 4u;
        if (o + 4u <= g_sharc.sharc_dm_ext_size) { uint32_t v; memcpy(&v, g_sharc.sharc_dm_ext + o, 4); return v; }
    }
    return 0;
}

static inline void sharc_dm_set(uint32_t a, uint32_t v) {
    if (a >= 0x30420u && a < 0x305A0u) {
        uint32_t o = a - 0x30420u;
        g_sharc.rot_cache[o / 12u][o % 12u] = sharc_bits_to_float(v);
        return;
    }
    if (a >= 0x30000u && a < 0x33000u) { g_sharc.dm[a - 0x30000u] = v; return; }
    if (a >= 0x1400000u && g_sharc.sharc_dm_ext) {
        uint64_t o = (uint64_t)(a - 0x1400000u) * 4u;
        if (o + 4u <= g_sharc.sharc_dm_ext_size) memcpy(g_sharc.sharc_dm_ext + o, &v, 4);
    }
}

static inline float sharc_dm_getf(uint32_t a)          { return sharc_bits_to_float(sharc_dm_get(a)); }
static inline void  sharc_dm_setf(uint32_t a, float f) { sharc_dm_set(a, sharc_float_to_bits(f)); }


/* _L20D98: a ball bitmask remapped to units, through player r1's ball->unit table */
static inline uint32_t sharc_coli_remap_player(uint32_t mask, uint32_t player) {
    uint32_t tbl = (player & 1u) ? 0x307A0u : 0x306A0u;
    uint32_t r = 0;
    for (uint32_t b = 0; b < 32u; b++)
        if (mask & (1u << b)) r |= 1u << (sharc_dm_get(tbl + b) & 31u);
    return r;
}

/* _L20D97: the same, for the player DM 0x307F2 names */
static inline uint32_t sharc_coli_remap(uint32_t mask) {
    return sharc_coli_remap_player(mask, sharc_dm_get(0x307F2u));
}

/* ---- Fn_coli_set_ball_adrs (0x38, PM 0x20DA8) ------------------------------- */
static inline void sharc_coli_set_ball_adrs(uint32_t a0) {
    sharc_dm_set(0x3033Eu, a0 == 1u ? 0x1407E80u : 0x1403E80u);
    g_sharc.coli_buf_base = a0 == 1u ? 0x1FA00u : 0xFA00u;
}

/* ---- Fn_coli_point_trans (0x39, PM 0x20DC2): the new position replaces the
 *      old one, and the old one is kept 0x60 words on — the previous frame's
 *      position the swept tests in 0x3A/0x3B need. */
static inline void sharc_coli_point_trans(float w[3], uint32_t off) {
    uint32_t base = sharc_dm_get(0x3033Eu) + off;
    for (uint32_t c = 0; c < 3u; c++) {
        uint32_t old = sharc_dm_get(base + c);
        sharc_dm_setf(base + c, w[c]);
        sharc_dm_set(base + 0x60u + c, old);
    }
}

/* ---- Fn_coli_copy_unit_matrix (0x7F, PM 0x20DB3) ----------------------------- */
static inline void sharc_coli_copy_unit_matrix(uint32_t a0) {
    uint32_t src = a0 == 1u ? 0x304E0u : 0x30420u, dst = a0 == 1u ? 0x320C0u : 0x32000u;
    for (uint32_t k = 0; k < 192u; k++) sharc_dm_set(dst + k, sharc_dm_get(src + k));
}

/* ---- Fn_coli_trans_mat (0x3D, PM 0x204F2): push-out added to every unit's T ---- */
static inline void sharc_coli_trans_mat(const float d0[3], const float d1[3]) {
    for (int s = 0; s < 16; s++)
        for (int c = 0; c < 3; c++) {
            g_sharc.rot_cache[s][9 + c]      = g_sharc.rot_cache[s][9 + c] + d0[c];
            g_sharc.rot_cache[16 + s][9 + c] = g_sharc.rot_cache[16 + s][9 + c] + d1[c];
        }
}

/* ---- Fn_coli_trans_xz (0x3E, PM 0x210B2) ------------------------------------- */
static inline void sharc_coli_trans_xz(float X, float Z, float ox, float oy, float oz) {
    float r2 = X * X;
    r2 = r2 + Z * Z;
    float k = sharc_fw_rsqrt(r2);                                        /* _L2029B */
    float c = X * k, s = Z * k;
    static const uint32_t SRC[4] = { 0x1403E80u, 0x1403EE0u, 0x1407E80u, 0x1407EE0u };
    static const uint32_t DST[4] = { 0x1403F40u, 0x1403FA0u, 0x1407F40u, 0x1407FA0u };
    for (int p = 0; p < 4; p++)
        for (uint32_t b = 0; b < 32u; b++) {
            float x = sharc_dm_getf(SRC[p] + 3u * b), y = sharc_dm_getf(SRC[p] + 3u * b + 1u), z = sharc_dm_getf(SRC[p] + 3u * b + 2u);
            float dx = x - ox, dz = z - oz, dy = y - oy;
            float ax = c * dx;
            ax = ax + s * dz;
            float az = c * dz;
            az = az - s * dx;
            sharc_dm_setf(DST[p] + 3u * b, ax);
            sharc_dm_setf(DST[p] + 3u * b + 1u, dy);
            sharc_dm_setf(DST[p] + 3u * b + 2u, az);
        }
}

/* ---- Fn_area_table_gen (0x3A, PM 0x20DDF): per axis, P0's balls (swept from
 *      the previous to the current position, grown by their radii) mark 64 bins
 *      of 1/8 unit; a P1 ball keeps the P0 balls whose extent overlaps its own. */
static inline void sharc_coli_area_table_gen(uint32_t m0, uint32_t m1, uint32_t k0, uint32_t k1) {
    sharc_dm_set(0x3041Au, k0);
    sharc_dm_set(0x3041Bu, k1);
    for (uint32_t j = 0; j < 32u; j++) sharc_dm_set(0x1403E20u + j, 0xFFFFFFFFu);
    uint32_t sh0 = m0, sh1 = m1;          /* the firmware never reloads these between axes */
    for (uint32_t d = 0; d < 3u; d++) {
        uint32_t lo[64] = {0}, hi[64] = {0}, bit = 1;
        for (uint32_t i = 0; i < 32u; i++, bit <<= 1) {
            float a = sharc_dm_getf(0x1403F40u + d + 3u * i), b = sharc_dm_getf(0x1403FA0u + d + 3u * i);
            if (b - a < 0.0f) { float t = a; a = b; b = t; }
            uint32_t Rb = sharc_dm_get(0x30600u + i);
            if (Rb == 0) continue;
            float R = sharc_bits_to_float(Rb);
            uint32_t cur = sh0; sh0 >>= 1;
            if ((cur & 1u) && k0 != 4u) R = R * sharc_dm_getf(0x306F0u);
            float f0 = a - R, f1 = b + R;
            float u = f0 * 8.0f; u = u + 32.0f; u = u - 0.5f;
            lo[(int32_t)u & 63] |= bit;
            u = f1 * 8.0f; u = u + 32.0f; u = u - 0.5f;
            hi[(int32_t)u & 63] |= bit;
        }
        for (int n = 1; n < 63; n++) lo[n] |= lo[n - 1];       /* lo[63] stays raw */
        for (int n = 62; n >= 1; n--) hi[n] |= hi[n + 1];      /* hi[0] stays raw */
        for (int n = 0; n < 64; n++) { sharc_dm_set(0x30340u + (uint32_t)n, lo[n]); sharc_dm_set(0x30380u + (uint32_t)n, hi[n]); }
        for (uint32_t j = 0; j < 32u; j++) {
            float a = sharc_dm_getf(0x1407F40u + d + 3u * j), b = sharc_dm_getf(0x1407FA0u + d + 3u * j);
            if (b - a < 0.0f) { float t = a; a = b; b = t; }
            uint32_t Rb = sharc_dm_get(0x30700u + j), mask = 0;
            if (Rb != 0) {
                float R = sharc_bits_to_float(Rb);
                uint32_t cur = sh1; sh1 >>= 1;
                if ((cur & 1u) && k1 != 4u) R = R * sharc_dm_getf(0x307F0u);
                float f0 = a - R, f1 = b + R;
                float u = f0 * 8.0f; u = u + 32.0f;
                if (!(u < 0.0f) && !(u - 64.0f >= 0.0f)) {
                    u = u - 0.5f;
                    mask = hi[(int32_t)u & 63];
                    float v = f1 * 8.0f; v = v + 32.0f;
                    if (!(v < 0.0f) && !(v - 64.0f >= 0.0f)) {
                        v = v - 0.5f;
                        mask &= lo[(int32_t)v & 63];
                    } else {
                        mask = 0;
                    }
                }
            }
            sharc_dm_set(0x1403E20u + j, sharc_dm_get(0x1403E20u + j) & mask);
        }
    }
    for (uint32_t k = 0; k < 16u; k++) sharc_dm_set(0x1403D80u + k, 0);
}

/* ---- Fn_calc_coli_flag (0x3B, PM 0x20ED5): the narrow phase ------------------ */
static inline void sharc_coli_calc_flag(uint32_t mode, uint32_t am0, uint32_t am1, uint32_t nz0,
                                        uint32_t nz1, uint32_t en0, uint32_t en1) {
    sharc_dm_set(0x30417u, en0);
    sharc_dm_set(0x30418u, en1);
    uint32_t tested = 0, hits = 0;
    float pen = 0.0f, lift = 0.0f;
    for (uint32_t k = 0; k < 0x60u; k++) {
        sharc_dm_set(0x30340u + k, sharc_dm_get(0x1403F40u + k));
        sharc_dm_set(0x303A0u + k, sharc_dm_get(0x1403FA0u + k));
    }
    uint32_t bj = 1;
    for (uint32_t r15 = 32; ; r15--, bj <<= 1) {
        uint32_t j = 32u - r15;
        uint32_t m = sharc_dm_get(0x1403E20u + j);
        uint32_t R1b = m ? sharc_dm_get(0x30720u + r15) : 0;
        if (m && R1b) {
            float R1 = sharc_bits_to_float(R1b);
            if ((am1 & bj) && sharc_dm_get(0x3041Bu) != 4u) R1 = R1 * sharc_dm_getf(0x307F0u);
            sharc_dm_setf(0x30416u, R1);
            uint32_t off = sharc_dm_get(0x30741u + r15);
            float P1c[3], P1p[3];
            for (uint32_t c = 0; c < 3u; c++) {
                P1c[c] = sharc_dm_getf(0x1407F40u + off + c); sharc_dm_setf(0x30410u + c, P1c[c]);
                P1p[c] = sharc_dm_getf(0x1407FA0u + off + c); sharc_dm_setf(0x30413u + c, P1p[c]);
            }
            uint32_t sw = (am1 & bj) ? 0xFFFFFFFFu : am0;
            if (en1 & bj) {
                uint32_t bi = 1;
                for (uint32_t r14 = 32; ; r14--, bi <<= 1) {
                    uint32_t R0b = ((en0 & bi) && (m & bi)) ? sharc_dm_get(0x30620u + r14) : 0;
                    if (R0b) {
                        tested++;
                        float R0 = sharc_bits_to_float(R0b);
                        if ((am0 & bi) && sharc_dm_get(0x3041Au) != 4u) R0 = R0 * sharc_dm_getf(0x306F0u);
                        uint32_t p0 = sharc_dm_get(0x30662u + r14);
                        float d[3];
                        for (uint32_t c = 0; c < 3u; c++) { d[c] = P1c[c] - sharc_dm_getf(p0 + c); sharc_dm_setf(0x30406u + c, d[c]); }
                        float D2;
                        if (sw & bi) {                              /* swept: closest approach over the frame */
                            float dp[3], v[3];
                            for (uint32_t c = 0; c < 3u; c++) { dp[c] = P1p[c] - sharc_dm_getf(p0 + 0x60u + c); v[c] = d[c] - dp[c]; }
                            float e = dp[0] * v[0]; e = e + dp[1] * v[1]; e = e + dp[2] * v[2];
                            float ne = -e;
                            if (ne <= 0.0f) {
                                D2 = dp[0] * dp[0] + dp[1] * dp[1]; D2 = D2 + dp[2] * dp[2];
                            } else {
                                float vv = v[0] * v[0] + v[1] * v[1]; vv = vv + v[2] * v[2];
                                if (vv - ne <= 0.0f) {
                                    D2 = d[0] * d[0] + d[1] * d[1]; D2 = D2 + d[2] * d[2];
                                } else {
                                    float t = sharc_fw_div(ne, vv);        /* _L205D0 */
                                    float q0 = v[0] * t + dp[0], q1 = v[1] * t + dp[1], q2 = v[2] * t + dp[2];
                                    D2 = q0 * q0 + q1 * q1; D2 = D2 + q2 * q2;
                                }
                            }
                        } else {
                            D2 = d[0] * d[0] + d[1] * d[1]; D2 = D2 + d[2] * d[2];
                        }
                        float S = R1 + R0, S2 = S * S;
                        if (!(D2 - S2 >= 0.0f)) {                  /* touching: strictly inside */
                            hits++;
                            sharc_dm_set(0x307F3u, r14);
                            sharc_dm_set(0x307F4u, r15);
                            if (mode == 4u || mode == 5u) {
                                float a = S2 - d[0] * d[0];
                                a = a - d[2] * d[2];
                                float x = sharc_fw_sqrt(a);
                                x = mode == 4u ? x - d[1] : x + d[1];
                                if (!(lift - x >= 0.0f)) lift = x;
                            } else if (!(sw & bi)) {
                                float x = (R1 + R0) - sharc_fw_sqrt(D2);
                                float k = ((nz0 & bi) && (nz1 & bj)) ? 0.0f : sharc_dm_getf(0x30301u);
                                x = x * k;
                                if (x - pen >= 0.0f) pen = x;
                            }
                        } else {
                            m &= ~bi;
                        }
                    }
                    if (r14 == 1u) break;
                }
            }
        }
        sharc_dm_set(0x1407E20u + j, m);
        if (r15 == 1u) break;
    }
    /* _L21083: every surviving ball pair marks unit u0 (P0) touching unit u1 (P1) */
    for (uint32_t j = 0; j < 32u; j++) {
        uint32_t u1 = sharc_dm_get(0x307A0u + j);
        if (u1 == 0) continue;
        uint32_t m = sharc_dm_get(0x1407E20u + j);
        for (uint32_t i = 0; i < 32u; i++) {
            if (!(m & (1u << i))) continue;
            uint32_t u0 = sharc_dm_get(0x306A0u + i);
            if (u0 == 0) continue;
            if (((am1 >> j) & 1u) && (i == 30u || i == 31u)) continue;
            if (((am0 >> i) & 1u) && (j == 30u || j == 31u)) continue;
            sharc_dm_set(0x1403D80u + u0, sharc_dm_get(0x1403D80u + u0) | (1u << (u1 & 31u)));
        }
    }
    sharc_push_u(tested);
    sharc_push_u(hits);
    sharc_push_f(pen);
    sharc_push_f(lift);
}

/* ---- Fn_area_coli (0x70, PM 0x20BBE): one fighter's balls against the arena:
 *      ground/soko/low masks, the four wall clearances and penetrations, the
 *      nearer wall pair kept; 22 replies. */
static inline void sharc_coli_area_coli(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4) {
    sharc_dm_set(0x30801u, a0);
    sharc_dm_set(0x30800u, a1);
    sharc_dm_set(0x30802u, a2);
    sharc_dm_set(0x30804u, a3);
    uint32_t player = a4 & 0x7FFFFFFFu;
    sharc_dm_set(0x307F2u, player);
    uint32_t table = 0x1407E80u, rad = 0x30700u;
    if (player == 0) {
        table = 0x1403E80u; rad = 0x30600u;
        for (uint32_t k = 0; k < 16u; k++) sharc_dm_set(0x30817u + k, 0);
    }
    sharc_dm_set(0x30803u, table);
    for (uint32_t k = 0; k < 18u; k++) sharc_dm_set(0x30805u + k, 0);
    for (uint32_t k = 0; k < 4u; k++) sharc_dm_set(0x30813u + k, 0x42C80000u);     /* 100.0 */
    float Y0 = sharc_bits_to_float(a0), soko = sharc_bits_to_float(a1);
    float A = sharc_bits_to_float(a2), H = sharc_bits_to_float(a3);
    uint32_t low = 0;
    for (uint32_t k = 0; k < 32u; k++) {
        float x = sharc_dm_getf(table + 3u * k), y = sharc_dm_getf(table + 3u * k + 1u), z = sharc_dm_getf(table + 3u * k + 2u);
        uint32_t Rb = sharc_dm_get(rad + k);
        if (Rb == 0) continue;
        float R = sharc_bits_to_float(Rb);
        uint32_t bit = 1u << k;
        float top = y + R;
        if (!(top <= sharc_dm_getf(0x3080Au))) sharc_dm_setf(0x3080Au, top);
        float bot = y - R;
        if (H <= bot) continue;
        low |= bit;
        if (bot <= 0.05f)          sharc_dm_set(0x30805u, sharc_dm_get(0x30805u) | bit);   /* ground */
        if (top <= -0.1f)          sharc_dm_set(0x30806u, sharc_dm_get(0x30806u) | bit);
        if (bot <= 0.05f + soko)   sharc_dm_set(0x30807u, sharc_dm_get(0x30807u) | bit);
        if (bot <= 0.05f + Y0)     sharc_dm_set(0x30808u, sharc_dm_get(0x30808u) | bit);
        for (uint32_t s = 0; s < 4u; s++) {                  /* +X, -X, +Z, -Z */
            float p = (s & 2u) ? z : x;
            float f7 = p + R, g = A - f7;
            if (s & 1u) { f7 = p - R; g = A + f7; }
            int penetrating = 0;
            float pv = 0.0f;
            if ((sharc_float_to_bits(f7) >> 31) == (s & 1u)) {
                float af = fabsf(f7);
                if (!(A > af)) {
                    penetrating = 1;
                    if (A < af) {
                        sharc_dm_set(0x30809u, sharc_dm_get(0x30809u) | bit);
                        pv = af - A;
                    } else {
                        pv = 0.001f;
                    }
                }
            }
            if (penetrating) {
                if (!(pv < sharc_dm_getf(0x3080Bu + s))) sharc_dm_setf(0x3080Bu + s, pv);
            } else {
                if (!(g >= sharc_dm_getf(0x30813u + s))) sharc_dm_setf(0x30813u + s, g);
            }
        }
    }
    uint32_t pen_mask = sharc_dm_get(0x30809u);
    if ((low & pen_mask) == low && (a4 & 0x80000000u))
        for (uint32_t s = 0; s < 4u; s++) sharc_dm_set(0x3080Bu + s, 0);
    float A2 = A + A;
    for (uint32_t s = 0; s < 4u; s++) {
        if (sharc_dm_getf(0x30813u + s) == 100.0f) continue;
        float v = A2 - sharc_dm_getf(0x30813u + (s ^ 1u));
        sharc_dm_set(0x3080Fu + s, (0.0f - v) == 0.0f ? 0x3A83126Eu : sharc_float_to_bits(v));
    }
    for (uint32_t p = 0x3080Fu; p <= 0x30811u; p += 2u) {   /* each wall pair keeps its nearer side */
        uint32_t a = sharc_dm_get(p), b = sharc_dm_get(p + 1u);
        if (a & 0x80000000u) a = 0;
        if (b & 0x80000000u) b = 0;
        if (sharc_bits_to_float(a) > sharc_bits_to_float(b)) a = 0; else b = 0;
        sharc_dm_set(p, a);
        sharc_dm_set(p + 1u, b);
    }
    float fx = sharc_dm_getf(0x3080Fu) + sharc_dm_getf(0x30810u);
    float fz = sharc_dm_getf(0x30811u) + sharc_dm_getf(0x30812u);
    if (fx < fz) { sharc_dm_set(0x30811u, 0); sharc_dm_set(0x30812u, 0); }
    else         { sharc_dm_set(0x3080Fu, 0); sharc_dm_set(0x30810u, 0); }
    for (uint32_t k = 0; k < 18u; k++) sharc_push_u(sharc_dm_get(0x30805u + k));
    sharc_push_u(sharc_coli_remap(sharc_dm_get(0x30805u)));
    sharc_push_u(sharc_coli_remap(sharc_dm_get(0x30809u)));
    sharc_push_u(sharc_coli_remap(sharc_dm_get(0x30807u)));
    sharc_push_u(sharc_coli_remap(sharc_dm_get(0x30808u)));
}

/* ---- Fn_outside_ball (0x72, PM 0x20D6F): balls outside the arena square ------- */
static inline void sharc_coli_outside_ball(uint32_t sel, float dx, float dz) {
    uint32_t table = sel == 0 ? 0x1403E80u : 0x1407E80u;
    float R = sharc_dm_getf(0x30802u);
    uint32_t n = 0;
    for (uint32_t k = 0; k < 32u; k++) {
        float x = fabsf(sharc_dm_getf(table + 3u * k) + dx);
        float z = fabsf(sharc_dm_getf(table + 3u * k + 2u) + dz);
        if (x > R || !(z < R)) n++;
    }
    sharc_push_u(n);
}

/* _L2034C: |v|, through _L202AE. The caller's division below reuses f11, and
 * the square root leaves 3.0 there (0 for a zero vector), so hand that back. */
static inline float sharc_coli_mag3(float x, float y, float z, float *f11) {
    float s = x * x + y * y;
    s = s + z * z;
    *f11 = sharc_float_to_bits(s) == 0 ? 0.0f : 3.0f;
    return sharc_fw_sqrt(s);
}

/* _L20B80: one fighter's balls against a sphere at c, radius in DM 0x3031B.
 * i3 = 0x30300 is the scratch frame; the offsets below are the firmware's. */
static inline void sharc_coli_parts_trace(uint32_t table, uint32_t rad, const float c[3]) {
    sharc_dm_set(0x3031Fu, 0);                               /* a hit this pass */
    sharc_dm_set(0x30312u, 0);                               /* hit ball mask */
    float f11;
    float d13 = sharc_coli_mag3(sharc_dm_getf(table + 0x27u) - c[0], sharc_dm_getf(table + 0x28u) - c[1],
                                sharc_dm_getf(table + 0x29u) - c[2], &f11);
    if (!(d13 <= 3.0f)) return;                              /* ball 13 more than 3 units off */
    for (uint32_t k = 0; k < 32u; k++) {
        uint32_t Rb = sharc_dm_get(rad + k);
        if (Rb == 0) continue;
        float dx = sharc_dm_getf(table + 3u * k) - c[0];
        float dy = sharc_dm_getf(table + 3u * k + 1u) - c[1];
        float dz = sharc_dm_getf(table + 3u * k + 2u) - c[2];
        float dist = sharc_coli_mag3(dx, dy, dz, &f11);
        float S = sharc_bits_to_float(Rb) + sharc_dm_getf(0x3031Bu);
        if (!(dist <= S)) continue;
        /* dist/S by RECIPS and three Newton steps — but f11 holds the sqrt's 3.0
         * where 2.0 belongs, so this settles near 2·dist/S, not on it. That is
         * the board. */
        float f4 = sharc_recips(S), f14 = f4 * S, f2 = dist * f4;
        f4 = f11 - f14;
        for (int it = 0; it < 2; it++) { f14 = f4 * f14; f2 = f2 * f4; f4 = f11 - f14; }
        float f0 = sharc_dm_getf(0x30301u) - f2 * f4;        /* 1.0 - ... */
        sharc_dm_setf(0x3031Cu, sharc_dm_getf(0x3031Cu) + dx * f0);
        sharc_dm_setf(0x3031Eu, sharc_dm_getf(0x3031Eu) + dz * f0);
        sharc_dm_setf(0x3031Fu, f0);
        sharc_dm_set(0x30319u, k);
        sharc_dm_set(0x30312u, sharc_dm_get(0x30312u) | (1u << k));
    }
}

/* ---- Fn_parts_oidasi (0x77, PM 0x20B1F): a loose sphere — a projectile, a
 *      flying part — against both fighters' balls. 9 replies: the push-out in
 *      x and z, the last fighter hit (-1 none), its ball and unit, then each
 *      fighter's hit ball mask and unit mask. Every projectile hit comes out of
 *      the unit masks (tobi +0x40/+0x42). */
static inline void sharc_coli_parts_oidasi(float x, float y, float z, uint32_t r) {
    const float c[3] = { x, y, z };
    sharc_dm_set(0x3031Bu, r);
    sharc_dm_set(0x30318u, 0);
    sharc_dm_set(0x30319u, 0);
    for (uint32_t k = 0x30313u; k <= 0x30316u; k++) sharc_dm_set(k, 0);
    sharc_dm_set(0x3031Au, 0xFFFFFFFFu);
    sharc_dm_set(0x3031Cu, 0);
    sharc_dm_set(0x3031Eu, 0);
    sharc_dm_set(0x3031Fu, 0);
    for (uint32_t p = 0; p < 2u; p++) {
        sharc_coli_parts_trace(p ? 0x1407E80u : 0x1403E80u, p ? 0x30700u : 0x30600u, c);
        uint32_t mask = sharc_dm_get(0x30312u);
        sharc_dm_set(0x30313u + p, mask);
        sharc_dm_set(0x30315u + p, sharc_coli_remap_player(mask, p));
        if (sharc_dm_get(0x3031Fu) != 0) {
            sharc_dm_set(0x3031Au, p);
            sharc_dm_set(0x30318u, sharc_dm_get((p ? 0x307A0u : 0x306A0u) + sharc_dm_get(0x30319u)));
        }
    }
    sharc_push_u(sharc_dm_get(0x3031Cu));
    sharc_push_u(sharc_dm_get(0x3031Eu));
    sharc_push_u(sharc_dm_get(0x3031Au));
    sharc_push_u(sharc_dm_get(0x30319u));
    sharc_push_u(sharc_dm_get(0x30318u));
    sharc_push_u(sharc_dm_get(0x30313u));
    sharc_push_u(sharc_dm_get(0x30315u));
    sharc_push_u(sharc_dm_get(0x30314u));
    sharc_push_u(sharc_dm_get(0x30316u));
}

/* ---- Fn_ball_to_unit (0x71, PM 0x20D8E) --------------------------------------- */
static inline void sharc_coli_ball_to_unit(uint32_t mask, uint32_t player) {
    sharc_dm_set(0x307F2u, player);
    sharc_push_u(sharc_coli_remap(mask));
}

#endif /* SHARC_COLI_H */
