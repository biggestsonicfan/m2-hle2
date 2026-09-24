/*
 * ps3ui_gpu.h -- draw a ps3ui draw list with sokol_gl.
 *
 * The PS3 composited its menus on the GPU: straight-alpha sprites sampled
 * bilinearly, blended "normal" (src-alpha, one-minus-src-alpha) or "add"
 * (src-alpha, one). This does the same with sokol_gfx, so the menus cost the
 * CPU almost nothing on any frontend (GL, GLES, WebGL, D3D11). ps3ui.h's CPU
 * path is the same arithmetic, kept for the tests and the grader.
 *
 *   ps3ui_gpu_setup()                       once sokol_gfx is up (after sgl_setup)
 *   ps3ui_gpu_record(&cv, w, h)             before drawing a frame of the UI into cv
 *   ps3ui_gpu_draw(&cv)                     inside a render pass: queue it for sgl_draw()
 *   ps3ui_gpu_shutdown()
 *
 * Sprites become textures the first time they are drawn; glyphs are packed into
 * one atlas and uploaded at most once a frame (sokol's rule for dynamic images).
 */
#ifndef PS3UI_GPU_H
#define PS3UI_GPU_H

#include "ps3ui.h"
#include "sokol_gfx.h"
#include "sokol_gl.h"

#define PS3UI_GPU_TEXTURES 128
#define PS3UI_ATLAS 1024
#define PS3UI_ATLAS_SLOTS 2048

typedef struct {
    const void *src;
    sg_image img;
    sg_view view;
} ps3ui_gpu_tex_t;

typedef struct {
    uint64_t key;
    int x, y, w, h;
} ps3ui_gpu_glyph_t;

static struct {
    int ready;
    sgl_pipeline pip[2];            /* normal, add */
    sg_sampler linear, nearest;
    ps3ui_gpu_tex_t tex[PS3UI_GPU_TEXTURES];
    int ntex;
    sg_image white_img;
    sg_view white;
    /* the glyph atlas: A8 coverage kept on the CPU, uploaded as RGBA8 white */
    sg_image atlas_img;
    sg_view atlas;
    uint8_t *atlas_px;
    int shelf_x, shelf_y, shelf_h;
    int dirty;
    uint64_t frame_uploaded;
    uint64_t frame;
    ps3ui_gpu_glyph_t slots[PS3UI_ATLAS_SLOTS];
    int nslots;
    ps3ui_dl_t dl;
} g_ps3ui_gpu;

static void ps3ui_gpu_setup(void)
{
    if (g_ps3ui_gpu.ready)
        return;
    sg_pipeline_desc pd = { 0 };
    pd.colors[0].blend.enabled = true;
    pd.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    pd.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ZERO;
    pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
    pd.colors[0].write_mask = SG_COLORMASK_RGB;
    g_ps3ui_gpu.pip[0] = sgl_make_pipeline(&pd);
    pd.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE;
    g_ps3ui_gpu.pip[1] = sgl_make_pipeline(&pd);

    g_ps3ui_gpu.linear = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR, .mag_filter = SG_FILTER_LINEAR,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE, .wrap_v = SG_WRAP_CLAMP_TO_EDGE });
    g_ps3ui_gpu.nearest = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_NEAREST, .mag_filter = SG_FILTER_NEAREST,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE, .wrap_v = SG_WRAP_CLAMP_TO_EDGE });

    static const uint32_t white = 0xFFFFFFFFu;
    g_ps3ui_gpu.white_img = sg_make_image(&(sg_image_desc){
        .width = 1, .height = 1, .pixel_format = SG_PIXELFORMAT_RGBA8,
        .data.mip_levels[0] = { &white, 4 } });
    g_ps3ui_gpu.white = sg_make_view(&(sg_view_desc){ .texture = { .image = g_ps3ui_gpu.white_img } });

    g_ps3ui_gpu.atlas_px = (uint8_t *)calloc((size_t)PS3UI_ATLAS * PS3UI_ATLAS * 4, 1);
    g_ps3ui_gpu.atlas_img = sg_make_image(&(sg_image_desc){
        .width = PS3UI_ATLAS, .height = PS3UI_ATLAS, .pixel_format = SG_PIXELFORMAT_RGBA8,
        .usage = { .dynamic_update = true } });
    g_ps3ui_gpu.atlas = sg_make_view(&(sg_view_desc){ .texture = { .image = g_ps3ui_gpu.atlas_img } });
    g_ps3ui_gpu.ready = 1;
}

static void ps3ui_gpu_shutdown(void)
{
    if (!g_ps3ui_gpu.ready)
        return;
    for (int i = 0; i < g_ps3ui_gpu.ntex; i++) {
        sg_destroy_view(g_ps3ui_gpu.tex[i].view);
        sg_destroy_image(g_ps3ui_gpu.tex[i].img);
    }
    sg_destroy_view(g_ps3ui_gpu.white);
    sg_destroy_image(g_ps3ui_gpu.white_img);
    sg_destroy_view(g_ps3ui_gpu.atlas);
    sg_destroy_image(g_ps3ui_gpu.atlas_img);
    sg_destroy_sampler(g_ps3ui_gpu.linear);
    sg_destroy_sampler(g_ps3ui_gpu.nearest);
    sgl_destroy_pipeline(g_ps3ui_gpu.pip[0]);
    sgl_destroy_pipeline(g_ps3ui_gpu.pip[1]);
    free(g_ps3ui_gpu.atlas_px);
    free(g_ps3ui_gpu.dl.items);
    memset(&g_ps3ui_gpu, 0, sizeof g_ps3ui_gpu);
}

/* A sprite's texture, made the first time it is drawn: RGBA8, straight alpha,
 * as the PS3's textures were. */
static sg_view ps3ui_gpu_texture(const ps3ui_image_t *im)
{
    for (int i = 0; i < g_ps3ui_gpu.ntex; i++)
        if (g_ps3ui_gpu.tex[i].src == im)
            return g_ps3ui_gpu.tex[i].view;
    if (g_ps3ui_gpu.ntex == PS3UI_GPU_TEXTURES)
        return g_ps3ui_gpu.white;
    size_t n = (size_t)im->w * (size_t)im->h;
    uint8_t *px = (uint8_t *)malloc(n * 4);
    for (size_t i = 0; i < n * 4; i++) {
        float v = im->px[i];
        px[i] = (uint8_t)((v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v) * 255.0f + 0.5f);
    }
    ps3ui_gpu_tex_t *t = &g_ps3ui_gpu.tex[g_ps3ui_gpu.ntex++];
    t->src = im;
    t->img = sg_make_image(&(sg_image_desc){
        .width = im->w, .height = im->h, .pixel_format = SG_PIXELFORMAT_RGBA8,
        .data.mip_levels[0] = { px, n * 4 } });
    t->view = sg_make_view(&(sg_view_desc){ .texture = { .image = t->img } });
    free(px);
    return t->view;
}

/* A glyph's place in the atlas; packs it on first use. Returns 0 if the atlas
 * is full (it is then emptied for the next frame). */
static int ps3ui_gpu_glyph(const ps3ui_dl_item_t *it, int *gx, int *gy)
{
    for (int i = 0; i < g_ps3ui_gpu.nslots; i++)
        if (g_ps3ui_gpu.slots[i].key == it->key && g_ps3ui_gpu.slots[i].w == it->sw
            && g_ps3ui_gpu.slots[i].h == it->sh) {
            *gx = g_ps3ui_gpu.slots[i].x;
            *gy = g_ps3ui_gpu.slots[i].y;
            return 1;
        }
    if (g_ps3ui_gpu.shelf_x + it->sw + 1 > PS3UI_ATLAS) {
        g_ps3ui_gpu.shelf_x = 0;
        g_ps3ui_gpu.shelf_y += g_ps3ui_gpu.shelf_h + 1;
        g_ps3ui_gpu.shelf_h = 0;
    }
    if (g_ps3ui_gpu.shelf_y + it->sh > PS3UI_ATLAS || g_ps3ui_gpu.nslots == PS3UI_ATLAS_SLOTS) {
        /* full: start over; this frame's later glyphs wait for the next one */
        g_ps3ui_gpu.nslots = 0;
        g_ps3ui_gpu.shelf_x = g_ps3ui_gpu.shelf_y = g_ps3ui_gpu.shelf_h = 0;
        memset(g_ps3ui_gpu.atlas_px, 0, (size_t)PS3UI_ATLAS * PS3UI_ATLAS * 4);
        g_ps3ui_gpu.dirty = 1;
        return 0;
    }
    int x = g_ps3ui_gpu.shelf_x, y = g_ps3ui_gpu.shelf_y;
    const uint8_t *bm = (const uint8_t *)it->src;
    for (int r = 0; r < it->sh; r++)
        for (int c = 0; c < it->sw; c++) {
            uint8_t *p = g_ps3ui_gpu.atlas_px + ((size_t)(y + r) * PS3UI_ATLAS + (size_t)(x + c)) * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = bm[r * it->sw + c];
        }
    ps3ui_gpu_glyph_t *s = &g_ps3ui_gpu.slots[g_ps3ui_gpu.nslots++];
    s->key = it->key;
    s->x = x;
    s->y = y;
    s->w = it->sw;
    s->h = it->sh;
    g_ps3ui_gpu.shelf_x += it->sw + 1;
    if (it->sh > g_ps3ui_gpu.shelf_h)
        g_ps3ui_gpu.shelf_h = it->sh;
    g_ps3ui_gpu.dirty = 1;
    *gx = x;
    *gy = y;
    return 1;
}

/* Point a canvas at the draw list for a frame of w x h output pixels. */
static void ps3ui_gpu_record(ps3ui_canvas_t *cv, int w, int h)
{
    g_ps3ui_gpu.dl.n = 0;
    cv->dl = &g_ps3ui_gpu.dl;
    ps3ui_canvas_size(cv, w, h);
}

static void ps3ui_gpu_quad(const ps3ui_dl_item_t *it, float u0, float v0, float u1, float v1)
{
    sgl_c4f(it->r, it->g, it->b, it->a);
    sgl_v2f_t2f(it->x[0], it->y[0], u0, v0);
    sgl_v2f_t2f(it->x[1], it->y[1], u1, v0);
    sgl_v2f_t2f(it->x[3], it->y[3], u1, v1);
    sgl_v2f_t2f(it->x[2], it->y[2], u0, v1);
}

/* Queue the recorded frame on sokol_gl's default context. The caller's pass
 * then runs sgl_draw(). The viewport is the whole w x h of the canvas. */
static void ps3ui_gpu_draw(const ps3ui_canvas_t *cv)
{
    if (!g_ps3ui_gpu.ready || !cv->dl)
        return;
    g_ps3ui_gpu.frame++;
    /* pack this frame's glyphs first, so the atlas is uploaded once */
    for (int i = 0; i < cv->dl->n; i++) {
        const ps3ui_dl_item_t *it = &cv->dl->items[i];
        int gx, gy;
        if (it->kind == PS3UI_DL_GLYPH)
            ps3ui_gpu_glyph(it, &gx, &gy);
    }
    if (g_ps3ui_gpu.dirty && g_ps3ui_gpu.frame_uploaded != g_ps3ui_gpu.frame) {
        sg_update_image(g_ps3ui_gpu.atlas_img, &(sg_image_data){
            .mip_levels[0] = { g_ps3ui_gpu.atlas_px, (size_t)PS3UI_ATLAS * PS3UI_ATLAS * 4 } });
        g_ps3ui_gpu.frame_uploaded = g_ps3ui_gpu.frame;
        g_ps3ui_gpu.dirty = 0;
    }
    sgl_defaults();
    sgl_viewport(0, 0, cv->w, cv->h, true);
    sgl_matrix_mode_projection();
    sgl_ortho(0.0f, (float)cv->w, (float)cv->h, 0.0f, -1.0f, 1.0f);
    sgl_enable_texture();
    for (int i = 0; i < cv->dl->n; i++) {
        const ps3ui_dl_item_t *it = &cv->dl->items[i];
        sgl_load_pipeline(g_ps3ui_gpu.pip[it->blend == PS3UI_BLEND_ADD ? 1 : 0]);
        if (it->kind == PS3UI_DL_IMAGE) {
            sgl_texture(ps3ui_gpu_texture((const ps3ui_image_t *)it->src), g_ps3ui_gpu.linear);
            sgl_begin_quads();
            ps3ui_gpu_quad(it, 0.0f, 0.0f, 1.0f, 1.0f);
            sgl_end();
        } else if (it->kind == PS3UI_DL_RECT) {
            sgl_texture(g_ps3ui_gpu.white, g_ps3ui_gpu.nearest);
            sgl_begin_quads();
            ps3ui_gpu_quad(it, 0.0f, 0.0f, 1.0f, 1.0f);
            sgl_end();
        } else {
            int gx, gy;
            if (!ps3ui_gpu_glyph(it, &gx, &gy))
                continue;
            const float k = 1.0f / (float)PS3UI_ATLAS;
            sgl_texture(g_ps3ui_gpu.atlas, g_ps3ui_gpu.nearest);
            sgl_begin_quads();
            ps3ui_gpu_quad(it, (float)gx * k, (float)gy * k, (float)(gx + it->sw) * k, (float)(gy + it->sh) * k);
            sgl_end();
        }
    }
}

#endif /* PS3UI_GPU_H */
