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

#endif /* DC_MATH_H */
