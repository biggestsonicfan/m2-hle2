/*
 * dc_math.h -- libm calls the Dreamcast build inlines (Pinboard #394), put in
 * front of every file by the Makefile (-include).
 *
 * newlib's fminf and fmaxf are calls that each call __fpclassifyf twice for
 * the NaN rule: with the bounding spheres, the culls and the depth sort they
 * were 4% of a frame. These keep the rule (a NaN loses to a number) and
 * otherwise return what newlib does, (x < y) ? x : y.
 */
#ifndef DC_MATH_H
#define DC_MATH_H

#include <math.h>

static inline float dc_fminf(float x, float y) {
    if (x != x) return y;
    if (y != y) return x;
    return x < y ? x : y;
}
static inline float dc_fmaxf(float x, float y) {
    if (x != x) return y;
    if (y != y) return x;
    return x > y ? x : y;
}
#define fminf dc_fminf
#define fmaxf dc_fmaxf

/* 1/sqrt(x) by the SH-4's FSRRA (to ~2^-21; sqrtf and a divide were ~40
 * cycles): for normalizing, where the length is not kept. */
static inline float dc_rsqrtf(float x) {
    __asm__("fsrra %0" : "+f"(x));
    return x;
}
#define GEO3D_RSQRTF dc_rsqrtf


/* DC_HOST_MATH (the game build; not LINK=1, which has to match the board
 * word for word): the COP's sin, cos and square roots from the SH-4's own
 * instructions in place of the copro ROM's tables and the firmware's (or
 * Gems') Newton steps. Close, not the same words: FSCA is good to ~2^-21 and
 * FSQRT is correctly rounded, where the firmware's last bits wander. */
#if DC_HOST_MATH
#include <stdint.h>
/* sin and cos of a binary angle (0x10000 = 2 pi, the low 16 bits count),
 * which is FSCA's own input. */
static inline void dc_fsca(uint32_t a, float *s, float *c) {
    register float fs __asm__("fr0"), fc __asm__("fr1");
    __asm__("lds %2,fpul\n\tfsca fpul,dr0" : "=f"(fs), "=f"(fc) : "r"(a) : "fpul");
    *s = fs;
    *c = fc;
}
static inline float dc_fsca_sin(uint32_t a) { float s, c; dc_fsca(a, &s, &c); return s; }
static inline float dc_fsca_cos(uint32_t a) { float s, c; dc_fsca(a, &s, &c); return c; }
static inline float dc_fsqrt(float x) {
    __asm__("fsqrt %0" : "+f"(x));
    return x;
}
#define SHARC_HOST_MATH 1
#define SHARC_HOST_SINCOS(a, s, c) dc_fsca((uint32_t)(a), (s), (c))
#define SHARC_HOST_SQRTF dc_fsqrt
#define GEMS_HOST_MATH   1
#define GEMS_HOST_SIN    dc_fsca_sin
#define GEMS_HOST_COS    dc_fsca_cos
#define GEMS_HOST_SQRTF  dc_fsqrt
#endif

#endif /* DC_MATH_H */
