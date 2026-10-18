/*
 * lightgun_sgl.h — the light guns' crosshairs through sokol_gl, for the front
 * ends without ImGui: the web page (main_web.c) and SDL3 (main_sdl.c)
 * (Pinboard #1040).
 *
 * lightgun_host_crosshairs gives the bars; this records them as sgl quads over
 * the whole framebuffer, blended, for the caller's sgl_draw in its swapchain
 * pass. sgl_setup must have run.
 */
#ifndef M2HLE_LIGHTGUN_SGL_H
#define M2HLE_LIGHTGUN_SGL_H

#include "sokol_gfx.h"
#include "sokol_gl.h"

#include "lightgun_host.h"

static sgl_pipeline s_lightgun_sgl_pip;

/* The crosshairs over a picture at (ox, oy, w, h) on a cw x ch framebuffer,
 * top-down pixels. Records nothing for a game without guns. */
static inline void lightgun_sgl_record(int ox, int oy, int w, int h, int cw, int ch) {
    lightgun_host_bar_t bar[LIGHTGUN_HOST_BARS];
    int n = lightgun_host_crosshairs((float)ox, (float)oy, (float)w, (float)h, bar);
    if (n <= 0) return;
    if (!s_lightgun_sgl_pip.id) {
        sg_pipeline_desc pd = { 0 };
        pd.colors[0].blend.enabled = true;
        pd.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
        pd.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ZERO;
        pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
        s_lightgun_sgl_pip = sgl_make_pipeline(&pd);
    }
    sgl_defaults();
    sgl_viewport(0, 0, cw, ch, true);
    sgl_scissor_rect(0, 0, cw, ch, true);
    sgl_load_pipeline(s_lightgun_sgl_pip);
    sgl_matrix_mode_projection();
    sgl_ortho(0.0f, (float)cw, (float)ch, 0.0f, -1.0f, 1.0f);
    sgl_begin_quads();
    for (int i = 0; i < n; i++) {
        const lightgun_host_bar_t *b = &bar[i];
        uint8_t r = (uint8_t)(b->rgb >> 16), g = (uint8_t)(b->rgb >> 8), bl = (uint8_t)b->rgb;
        uint8_t a = (uint8_t)(b->a * 255.0f + 0.5f);
        sgl_c4b(r, g, bl, a);
        sgl_v2f(b->x0, b->y0);
        sgl_v2f(b->x1, b->y0);
        sgl_v2f(b->x1, b->y1);
        sgl_v2f(b->x0, b->y1);
    }
    sgl_end();
    sgl_load_default_pipeline();
}

#endif /* M2HLE_LIGHTGUN_SGL_H */
