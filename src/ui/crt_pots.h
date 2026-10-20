/*
 * crt_pots.h -- the adjustments a CRT arcade monitor has on its chassis, over
 * the game's picture: a drive (gain) and a cutoff (bias) pot for each of red,
 * green and blue, and the H/V position and size pots. Host-side only; the
 * board never sees them.
 *
 *   crt_pots_set(id, value)             one pot (CRT_POT_*), clamped to its range
 *   crt_pots_place(&ox, &oy, &w, &h)    move and size the picture's rectangle
 *   crt_pots_record(ox, oy, w, h, cw, ch)
 *                                       record the colour passes into sokol_gl,
 *                                       over the picture just drawn; the caller's
 *                                       sgl_draw() draws them
 *
 * The colour is done in the framebuffer with blending, so it works over every
 * way the game is drawn (straight in, from the offscreen target, through a
 * filter) without a target of its own: a pass that multiplies by min(gain, 1),
 * one that adds dst * (gain - 1) above 1, then one that adds or takes away the
 * bias. The framebuffer clamps between them, as a monitor clips a driven gun.
 * Every pot at its centre records nothing.
 */
#ifndef CRT_POTS_H
#define CRT_POTS_H

#include <stdbool.h>
#include <math.h>

#include "sokol_gfx.h"
#include "sokol_gl.h"

enum {
    CRT_POT_GAIN_R, CRT_POT_GAIN_G, CRT_POT_GAIN_B,
    CRT_POT_BIAS_R, CRT_POT_BIAS_G, CRT_POT_BIAS_B,
    CRT_POT_H_POS, CRT_POT_V_POS, CRT_POT_H_SIZE, CRT_POT_V_SIZE,
    CRT_POT_COUNT
};

/* Each pot's range and centre: gains and sizes are factors, the bias a level
 * out of 1, positions a fraction of the picture's own width or height. */
static const struct { float lo, hi, centre; } crt_pot_range[CRT_POT_COUNT] = {
    { 0.0f, 2.0f, 1.0f }, { 0.0f, 2.0f, 1.0f }, { 0.0f, 2.0f, 1.0f },
    { -0.5f, 0.5f, 0.0f }, { -0.5f, 0.5f, 0.0f }, { -0.5f, 0.5f, 0.0f },
    { -0.25f, 0.25f, 0.0f }, { -0.25f, 0.25f, 0.0f },
    { 0.5f, 1.5f, 1.0f }, { 0.5f, 1.5f, 1.0f },
};

enum { CRT_PIP_MUL, CRT_PIP_BOOST, CRT_PIP_ADD, CRT_PIP_SUB, CRT_PIP_COUNT };

static struct {
    float    pot[CRT_POT_COUNT];
    bool     init;
    unsigned gen;                 /* bumped on every change: a still picture is drawn again */
    sgl_pipeline pip[CRT_PIP_COUNT];
} g_crt_pots;

static void crt_pots__defaults(void) {
    if (g_crt_pots.init) return;
    for (int i = 0; i < CRT_POT_COUNT; i++) g_crt_pots.pot[i] = crt_pot_range[i].centre;
    g_crt_pots.init = true;
}

static void crt_pots_set(int id, float v) {
    if (id < 0 || id >= CRT_POT_COUNT || !(v == v)) return;
    crt_pots__defaults();
    if (v < crt_pot_range[id].lo) v = crt_pot_range[id].lo;
    if (v > crt_pot_range[id].hi) v = crt_pot_range[id].hi;
    g_crt_pots.pot[id] = v;
    g_crt_pots.gen++;
}

static float crt_pots_get(int id) {
    crt_pots__defaults();
    return id >= 0 && id < CRT_POT_COUNT ? g_crt_pots.pot[id] : 0.0f;
}

/* The H/V pots: size about the rectangle's centre, then move it. */
static void crt_pots_place(int *ox, int *oy, int *w, int *h) {
    crt_pots__defaults();
    const float *p = g_crt_pots.pot;
    const int nw = (int)lroundf((float)*w * p[CRT_POT_H_SIZE]);
    const int nh = (int)lroundf((float)*h * p[CRT_POT_V_SIZE]);
    *ox += (*w - nw) / 2 + (int)lroundf((float)*w * p[CRT_POT_H_POS]);
    *oy += (*h - nh) / 2 + (int)lroundf((float)*h * p[CRT_POT_V_POS]);
    *w = nw;
    *h = nh;
}

static sgl_pipeline crt_pots__pipeline(sg_blend_factor src, sg_blend_factor dst, sg_blend_op op) {
    sg_pipeline_desc pd = { 0 };
    pd.colors[0].blend.enabled = true;
    pd.colors[0].blend.src_factor_rgb = src;
    pd.colors[0].blend.dst_factor_rgb = dst;
    pd.colors[0].blend.op_rgb = op;
    pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ZERO;
    pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
    pd.colors[0].write_mask = SG_COLORMASK_RGB;
    return sgl_make_pipeline(&pd);
}

static void crt_pots__setup(void) {
    if (g_crt_pots.pip[0].id) return;
    g_crt_pots.pip[CRT_PIP_MUL]   = crt_pots__pipeline(SG_BLENDFACTOR_ZERO, SG_BLENDFACTOR_SRC_COLOR, SG_BLENDOP_ADD);
    g_crt_pots.pip[CRT_PIP_BOOST] = crt_pots__pipeline(SG_BLENDFACTOR_DST_COLOR, SG_BLENDFACTOR_ONE, SG_BLENDOP_ADD);
    g_crt_pots.pip[CRT_PIP_ADD]   = crt_pots__pipeline(SG_BLENDFACTOR_ONE, SG_BLENDFACTOR_ONE, SG_BLENDOP_ADD);
    g_crt_pots.pip[CRT_PIP_SUB]   = crt_pots__pipeline(SG_BLENDFACTOR_ONE, SG_BLENDFACTOR_ONE, SG_BLENDOP_REVERSE_SUBTRACT);
}

/* One pass of colour c over the rectangle, if it does anything at all. */
static void crt_pots__pass(int pip, const float c[3], float none, float x0, float y0, float x1, float y1) {
    if (c[0] == none && c[1] == none && c[2] == none) return;
    sgl_load_pipeline(g_crt_pots.pip[pip]);
    sgl_begin_quads();
    sgl_c3f(c[0], c[1], c[2]);
    sgl_v2f(x0, y0); sgl_v2f(x1, y0); sgl_v2f(x1, y1); sgl_v2f(x0, y1);
    sgl_end();
}

static void crt_pots_record(int ox, int oy, int w, int h, int cw, int ch) {
    crt_pots__defaults();
    const float *p = g_crt_pots.pot;
    float mul[3], boost[3], add[3], sub[3];
    for (int i = 0; i < 3; i++) {
        const float g = p[CRT_POT_GAIN_R + i], b = p[CRT_POT_BIAS_R + i];
        mul[i]   = g < 1.0f ? g : 1.0f;
        boost[i] = g > 1.0f ? g - 1.0f : 0.0f;
        add[i]   = b > 0.0f ? b : 0.0f;
        sub[i]   = b < 0.0f ? -b : 0.0f;
    }
    bool neutral = true;
    for (int i = CRT_POT_GAIN_R; i <= CRT_POT_BIAS_B; i++) neutral &= p[i] == crt_pot_range[i].centre;
    if (neutral) return;
    crt_pots__setup();
    sgl_defaults();
    sgl_viewport(0, 0, cw, ch, true);
    sgl_matrix_mode_projection();
    sgl_ortho(0.0f, (float)cw, (float)ch, 0.0f, -1.0f, 1.0f);
    const float x0 = (float)ox, y0 = (float)oy, x1 = (float)(ox + w), y1 = (float)(oy + h);
    crt_pots__pass(CRT_PIP_MUL, mul, 1.0f, x0, y0, x1, y1);
    crt_pots__pass(CRT_PIP_BOOST, boost, 0.0f, x0, y0, x1, y1);
    crt_pots__pass(CRT_PIP_ADD, add, 0.0f, x0, y0, x1, y1);
    crt_pots__pass(CRT_PIP_SUB, sub, 0.0f, x0, y0, x1, y1);
}

#endif /* CRT_POTS_H */
