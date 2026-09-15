/*
 * video_window.h — per-frame tile compositor → GPU textures.
 *
 * Produces the textures the swapchain quads in game_render.h draw: the solid
 * back colour, the layer behind the 3D (BG) and the layer in front of it (FG).
 * Two ways, same picture:
 *
 *   CPU  render_bg_layer / render_fg_layer (tile_renderer.h) compose both
 *        layers, tile_pen_lut turns palette colours into screen colours, and
 *        the RGBA buffers are uploaded whole. Every backend; the only path on
 *        D3D11 and the dummy backend.
 *   GPU  on GL backends, tile RAM, tile graphics and the pen-converted palette
 *        go up as textures (each only when a write changed what it is made of)
 *        and one shader pass composes both layers into two render targets with
 *        integer fetches, following s24_draw_tilemap and the layer order step for
 *        step. The targets are laid out like the CPU uploads (row 0 = top), so
 *        drawing them is unchanged.
 *
 * Either way nothing is composed on a frame where no tile RAM, tile graphics,
 * palette or colour-translation byte changed.
 *
 * Despite the legacy filename, there is no ImGui window here.
 */
#ifndef VIDEO_WINDOW_H
#define VIDEO_WINDOW_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sokol_gfx.h"

#include "constants.h"
#include "game_render.h"   /* game_render_glsl */
#include "memory.h"
#include "tile_renderer.h"

/* Tile RAM bytes the compositor reads: the four tilemaps (words 0x0000-0x3FFF),
 * their per-line H scroll tables (0x4000 + 0x200*t, 384 lines) and both window
 * masks (0x6000 / 0x6800, four words a line). The scroll registers (0x5000-0x5007)
 * go in as uniforms. */
#define VIDEO_TILE_GPU_W 256
#define VIDEO_TILE_GPU_H 224
#define VIDEO_GFX_GPU_W  1024
#define VIDEO_GFX_GPU_H  512
#define VIDEO_PAL_GPU_W  256
#define VIDEO_PAL_GPU_H  32
_Static_assert(VIDEO_TILE_GPU_W * VIDEO_TILE_GPU_H >= (0x6800 + VIDEO_HEIGHT * 4) * 2, "tile texture covers the window masks");
_Static_assert(VIDEO_TILE_GPU_W * VIDEO_TILE_GPU_H <= TILE_SIZE, "tile texture fits tile RAM");
_Static_assert(VIDEO_GFX_GPU_W * VIDEO_GFX_GPU_H == TMAPGFX_SIZE, "gfx texture is tile graphics RAM");
_Static_assert(VIDEO_PAL_GPU_W * VIDEO_PAL_GPU_H * 2 == PALETTE_SIZE, "one texel per palette entry");

/* Force the CPU compositor on GL backends too (set before video_init). */
static int g_video_force_cpu_tiles = 0;

/*
 * Layered render targets so the 3D scene sits BETWEEN the tile layers:
 *   back_view : solid "back-back" colour (change_bg_color) — drawn first, fills view
 *   bg_view   : background tiles, alpha-keyed — over the back colour, before 3D
 *   fg_view   : foreground / HUD tiles, alpha-keyed — after the 3D pass
 * (alpha 0 where the tile's colour index is 0, so the layer behind shows through)
 */
typedef struct {
    sg_image      bg_image, fg_image, back_image;
    sg_view       bg_view,  fg_view, back_view;
    sg_sampler    sampler;
    uint8_t       bg_pixels[VIDEO_WIDTH * VIDEO_HEIGHT * VIDEO_BPP];
    uint8_t       fg_pixels[VIDEO_WIDTH * VIDEO_HEIGHT * VIDEO_BPP];
    uint8_t       back_pixel[4];   /* 1×1 back-back colour, stretched over the view */
    tile_layers_t layers;
    bool          initialized;

    /* The bus generations the layers show (see memory.h change_gen). */
    bool          composed;
    uint32_t      composed_tile, composed_gfx, composed_pal, composed_lut;
    /* Screen value of each channel's 5 bits (video_pen_channels): a pen's
     * colour is pen_chan[0][r5], pen_chan[1][g5], pen_chan[2][b5]. */
    uint8_t       pen_chan[3][32];

    /* GPU compositor (GL backends): RAM textures, layer targets, shader. */
    bool          gpu;
    sg_image      tile_ram, gfx_ram, pal_rgba;
    sg_view       tile_ram_view, gfx_ram_view, pal_rgba_view;
    sg_view       bg_att, fg_att;
    sg_sampler    fetch_sampler;
    sg_shader     tile_shader;
    sg_pipeline   tile_pipeline;
    uint8_t       pal_texels[VIDEO_PAL_GPU_W * VIDEO_PAL_GPU_H * 4];
    uint64_t      gpu_composes;
} video_state_t;

/* ---- GPU compositor shaders ------------------------------------------------ */

static const char *video_tile_vs_glsl =
    "#version 410\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

/*
 * tm_px is s24_draw_tilemap for one pixel of tilemap t: disabled by V scroll
 * bit 15, the odd tilemap idle under a split mode, the per-line H scroll, the
 * split at line -vscroll (mode 1) or column hscroll (modes 2/3), otherwise the
 * window mask (four words a line, one bit per 8 pixels, inverted for the odd
 * tilemap), then s24_sample: the 64x64 tilemap wrapping at 512, palette bank
 * bits 14:7, category bit 15, the 4bpp texel with its 16-bit byteswap. The pen
 * texture holds tile_pen_lut of every palette entry, so pen p's screen colour is
 * texel p. y is the target row, which GL counts from the bottom, so the target's
 * row 0 holds screen row 0 as the CPU uploads do. h_scr / v_scr are words
 * 0x5000-0x5003 / 0x5004-0x5007.
 */
static const char *video_tile_fs_glsl =
    "#version 410\n"
    "uniform vec4 tile_params[2];\n"
    "uniform usampler2D tile_smp;\n"
    "uniform usampler2D gfx_smp;\n"
    "uniform sampler2D pal_smp;\n"
    "layout(location = 0) out vec4 bg_out;\n"
    "layout(location = 1) out vec4 fg_out;\n"
    "int tw(int word) {\n"
    "  int b = word * 2;\n"
    "  return int(texelFetch(tile_smp, ivec2(b & 255, b >> 8), 0).r)\n"
    "       | (int(texelFetch(tile_smp, ivec2((b + 1) & 255, (b + 1) >> 8), 0).r) << 8);\n"
    "}\n"
    "int gb(int off) { return int(texelFetch(gfx_smp, ivec2(off & 1023, off >> 10), 0).r); }\n"
    "vec3 pal(int pen) { return texelFetch(pal_smp, ivec2(pen & 255, pen >> 8), 0).rgb; }\n"
    "bool tm_px(int t, int x, int y, out int ci, out int cat, out int pen) {\n"
    "  ci = 0; cat = 0; pen = 0;\n"
    "  int hscr = int(tile_params[0][t]), vscr = int(tile_params[1][t]), ctrl = int(tile_params[1][t & 2]);\n"
    "  if ((vscr & 0x8000) != 0) return false;\n"
    "  int mode = (ctrl & 0x6000) >> 13;\n"
    "  if (mode != 0 && (t & 1) != 0) return false;\n"
    "  int row = (hscr & 0x8000) != 0 ? tw(0x4000 + 0x200 * t + y) : hscr;\n"
    "  int h = row & 0x1FF;\n"
    "  int l = t;\n"
    "  if (mode == 1) {\n"
    "    int nv = (-vscr) & 0x3FF;\n"
    "    l = (nv & 0x200) != 0 ? t : (t ^ 1);\n"
    "    if (y >= (nv & 0x1FF)) l ^= 1;\n"
    "  } else if (mode != 0) {\n"
    "    int split_l = (row & 0x200) != 0 ? t : (t ^ 1);\n"
    "    l = x < h ? split_l : (split_l ^ 1);\n"
    "  } else {\n"
    "    int m = tw(((t & 2) != 0 ? 0x6800 : 0x6000) + y * 4 + (x >> 7));\n"
    "    if ((t & 1) != 0) m = ~m & 0xFFFF;\n"
    "    if ((m & (0x8000 >> ((x & 127) >> 3))) != 0) return false;\n"
    "  }\n"
    "  int sx = (x - h) & 511, sy = (y + (vscr & 0x1FF)) & 511;\n"
    "  int entry = tw(0x1000 * l + (sy >> 3) * 64 + (sx >> 3));\n"
    "  int px = sx & 7;\n"
    "  int b = gb((entry & 0x3FFF) * 32 + (sy & 7) * 4 + ((px >> 1) ^ 1));\n"
    "  ci = (px & 1) != 0 ? (b & 15) : (b >> 4);\n"
    "  cat = (entry >> 15) & 1;\n"
    "  pen = ((entry >> 7) & 0xFF) * 16 + ci;\n"
    "  return true;\n"
    "}\n"
    "void main() {\n"
    "  int x = int(gl_FragCoord.x), y = int(gl_FragCoord.y);\n"
    "  bool d[4]; int ci[4], cat[4], pen[4];\n"
    "  for (int t = 0; t < 4; t++) d[t] = tm_px(t, x, y, ci[t], cat[t], pen[t]);\n"
    "  vec3 bg = pal(0);\n"                                   /* the backdrop, palette 0 */
    "  if (d[3]) bg = pal(pen[3]);\n"                         /* tilemaps 3, 2 opaque */
    "  if (d[2]) bg = pal(pen[2]);\n"
    "  if (d[1] && ci[1] != 0 && cat[1] == 0) bg = pal(pen[1]);\n"   /* 1, 0: category 0 */
    "  if (d[0] && ci[0] != 0 && cat[0] == 0) bg = pal(pen[0]);\n"
    "  vec4 fg = vec4(0.0);\n"                                /* 3, 2, 1, 0: category 1 */
    "  for (int t = 3; t >= 0; t--)\n"
    "    if (d[t] && ci[t] != 0 && cat[t] == 1) fg = vec4(pal(pen[t]), 1.0);\n"
    "  bg_out = vec4(bg, 1.0);\n"
    "  fg_out = fg;\n"
    "}\n";

static inline sg_image video__make_img(const char *label) {
    return sg_make_image(&(sg_image_desc){
        .width        = VIDEO_WIDTH,
        .height       = VIDEO_HEIGHT,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .usage        = { .stream_update = true },
        .label        = label,
    });
}

static inline void video__init_gpu(video_state_t *vid) {
    sg_backend backend = sg_query_backend();
    vid->gpu = false;
    if (g_video_force_cpu_tiles || !(backend == SG_BACKEND_GLCORE || backend == SG_BACKEND_GLES3)) return;

    /* Shader and pipeline first: if either fails, the CPU path keeps its
     * stream-updated layer images untouched. */
    sg_shader_desc d;
    memset(&d, 0, sizeof d);
    d.uniform_blocks[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d.uniform_blocks[0].size  = 8 * sizeof(float);
    d.uniform_blocks[0].glsl_uniforms[0].glsl_name   = "tile_params";
    d.uniform_blocks[0].glsl_uniforms[0].type        = SG_UNIFORMTYPE_FLOAT4;
    d.uniform_blocks[0].glsl_uniforms[0].array_count = 2;
    static const sg_image_sample_type types[3] = { SG_IMAGESAMPLETYPE_UINT, SG_IMAGESAMPLETYPE_UINT, SG_IMAGESAMPLETYPE_FLOAT };
    static const char *const names[3] = { "tile_smp", "gfx_smp", "pal_smp" };
    for (int i = 0; i < 3; i++) {
        d.views[i].texture.stage       = SG_SHADERSTAGE_FRAGMENT;
        d.views[i].texture.image_type  = SG_IMAGETYPE_2D;
        d.views[i].texture.sample_type = types[i];
        d.texture_sampler_pairs[i].stage        = SG_SHADERSTAGE_FRAGMENT;
        d.texture_sampler_pairs[i].view_slot    = i;
        d.texture_sampler_pairs[i].sampler_slot = 0;
        d.texture_sampler_pairs[i].glsl_name    = names[i];
    }
    d.samplers[0].stage        = SG_SHADERSTAGE_FRAGMENT;
    d.samplers[0].sampler_type = SG_SAMPLERTYPE_NONFILTERING;
    d.vertex_func.source   = game_render_glsl(backend, video_tile_vs_glsl, 0);
    d.fragment_func.source = game_render_glsl(backend, video_tile_fs_glsl, 1);
    d.label = "tile-compose-shader";
    vid->tile_shader = sg_make_shader(&d);
    vid->tile_pipeline = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = vid->tile_shader,
        .primitive_type = SG_PRIMITIVETYPE_TRIANGLES,
        .color_count = 2,
        .colors[0].pixel_format = SG_PIXELFORMAT_RGBA8,
        .colors[1].pixel_format = SG_PIXELFORMAT_RGBA8,
        .depth.pixel_format = SG_PIXELFORMAT_NONE,
        .sample_count = 1,
        .label = "tile-compose-pipeline",
    });
    if (sg_query_shader_state(vid->tile_shader) != SG_RESOURCESTATE_VALID ||
            sg_query_pipeline_state(vid->tile_pipeline) != SG_RESOURCESTATE_VALID) {
        LOG_WARN("video: GPU tile compositor unavailable, composing on the CPU");
        sg_destroy_pipeline(vid->tile_pipeline);
        sg_destroy_shader(vid->tile_shader);
        return;
    }
    vid->gpu = true;

    vid->tile_ram = sg_make_image(&(sg_image_desc){ .width = VIDEO_TILE_GPU_W, .height = VIDEO_TILE_GPU_H,
        .pixel_format = SG_PIXELFORMAT_R8UI, .usage = { .dynamic_update = true }, .label = "tile-ram" });
    vid->gfx_ram = sg_make_image(&(sg_image_desc){ .width = VIDEO_GFX_GPU_W, .height = VIDEO_GFX_GPU_H,
        .pixel_format = SG_PIXELFORMAT_R8UI, .usage = { .dynamic_update = true }, .label = "tile-gfx-ram" });
    vid->pal_rgba = sg_make_image(&(sg_image_desc){ .width = VIDEO_PAL_GPU_W, .height = VIDEO_PAL_GPU_H,
        .pixel_format = SG_PIXELFORMAT_RGBA8, .usage = { .dynamic_update = true }, .label = "tile-pens" });
    vid->tile_ram_view = sg_make_view(&(sg_view_desc){ .texture.image = vid->tile_ram });
    vid->gfx_ram_view  = sg_make_view(&(sg_view_desc){ .texture.image = vid->gfx_ram });
    vid->pal_rgba_view = sg_make_view(&(sg_view_desc){ .texture.image = vid->pal_rgba });
    vid->fetch_sampler = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_NEAREST, .mag_filter = SG_FILTER_NEAREST,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE, .wrap_v = SG_WRAP_CLAMP_TO_EDGE, .label = "tile-fetch-sampler" });

    /* The layers become render targets; bg_view / fg_view keep their meaning. */
    sg_destroy_view(vid->bg_view);
    sg_destroy_view(vid->fg_view);
    sg_destroy_image(vid->bg_image);
    sg_destroy_image(vid->fg_image);
    vid->bg_image = sg_make_image(&(sg_image_desc){ .width = VIDEO_WIDTH, .height = VIDEO_HEIGHT,
        .pixel_format = SG_PIXELFORMAT_RGBA8, .usage = { .color_attachment = true }, .label = "game-bg-target" });
    vid->fg_image = sg_make_image(&(sg_image_desc){ .width = VIDEO_WIDTH, .height = VIDEO_HEIGHT,
        .pixel_format = SG_PIXELFORMAT_RGBA8, .usage = { .color_attachment = true }, .label = "game-fg-target" });
    vid->bg_view = sg_make_view(&(sg_view_desc){ .texture.image = vid->bg_image, .label = "game-bg-view" });
    vid->fg_view = sg_make_view(&(sg_view_desc){ .texture.image = vid->fg_image, .label = "game-fg-view" });
    vid->bg_att  = sg_make_view(&(sg_view_desc){ .color_attachment.image = vid->bg_image });
    vid->fg_att  = sg_make_view(&(sg_view_desc){ .color_attachment.image = vid->fg_image });
}

static inline void video_init(video_state_t *vid) {
    memset(vid->bg_pixels, 0, sizeof(vid->bg_pixels));
    memset(vid->fg_pixels, 0, sizeof(vid->fg_pixels));
    memset(&vid->layers, 0, sizeof(tile_layers_t));
    tile_layers_init(&vid->layers);

    vid->bg_image = video__make_img("game-bg");
    vid->fg_image = video__make_img("game-fg");
    vid->bg_view = sg_make_view(&(sg_view_desc){
        .texture.image = vid->bg_image, .label = "game-bg-view" });
    vid->fg_view = sg_make_view(&(sg_view_desc){
        .texture.image = vid->fg_image, .label = "game-fg-view" });

    /* 1×1 solid back-back colour texture, stretched over the view by the quad. */
    memset(vid->back_pixel, 0, sizeof(vid->back_pixel));
    vid->back_image = sg_make_image(&(sg_image_desc){
        .width = 1, .height = 1, .pixel_format = SG_PIXELFORMAT_RGBA8,
        .usage = { .stream_update = true }, .label = "game-back" });
    vid->back_view = sg_make_view(&(sg_view_desc){
        .texture.image = vid->back_image, .label = "game-back-view" });

    vid->sampler = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_NEAREST,
        .mag_filter = SG_FILTER_NEAREST,
        .wrap_u     = SG_WRAP_CLAMP_TO_EDGE,
        .wrap_v     = SG_WRAP_CLAMP_TO_EDGE,
        .label      = "game-sampler",
    });

    vid->composed = false;
    vid->gpu_composes = 0;
    video__init_gpu(vid);
    vid->initialized = true;
}

static inline void video_shutdown(video_state_t *vid) {
    if (!vid->initialized) return;
    if (vid->gpu) {
        sg_destroy_pipeline(vid->tile_pipeline);
        sg_destroy_shader(vid->tile_shader);
        sg_destroy_sampler(vid->fetch_sampler);
        sg_destroy_view(vid->fg_att);
        sg_destroy_view(vid->bg_att);
        sg_destroy_view(vid->pal_rgba_view);
        sg_destroy_view(vid->gfx_ram_view);
        sg_destroy_view(vid->tile_ram_view);
        sg_destroy_image(vid->pal_rgba);
        sg_destroy_image(vid->gfx_ram);
        sg_destroy_image(vid->tile_ram);
    }
    sg_destroy_view(vid->bg_view);
    sg_destroy_view(vid->fg_view);
    sg_destroy_view(vid->back_view);
    sg_destroy_sampler(vid->sampler);
    sg_destroy_image(vid->bg_image);
    sg_destroy_image(vid->fg_image);
    sg_destroy_image(vid->back_image);
    tile_layers_free(&vid->layers);
    vid->initialized = false;
}

/* Screen colours for every 15-bit palette colour (tile_pen_lut, rebuilt on use). */
static uint8_t g_video_pen[0x8000][3];

/* tile_pen_lut one channel at a time. Each channel of a pen depends only on its
 * own 5 bits, so 3 x 32 values give every entry of the 0x8000-entry table:
 * lut[c][ch] == chan[ch][(c >> 5 * ch) & 31]. The GPU path needs 8192 pens per
 * palette change, not 32768 built with a division each. --verify-gpu-tiles
 * holds this to tile_pen_lut, which the CPU compositor still uses. */
static inline void video_pen_channels(const memory_bus_t *bus, uint8_t chan[3][32]) {
    int loaded = 0;
    for (int c5 = 0; c5 < 32 && !loaded; c5++)
        for (int ch = 0; ch < 3 && !loaded; ch++)
            if (bus->colorxlat[((uint32_t)ch * 0x2000u + 0x40u + ((uint32_t)c5 << 8)) * 2u]) loaded = 1;
    for (int ch = 0; ch < 3; ch++)
        for (int c5 = 0; c5 < 32; c5++) {
            if (!loaded) { chan[ch][c5] = (uint8_t)(c5 << 3); continue; }
            int v = bus->colorxlat[((uint32_t)ch * 0x2000u + 0x40u + ((uint32_t)c5 << 8)) * 2u];
            int g = (v - 64) * 255 / 191;
            chan[ch][c5] = (uint8_t)(g < 0 ? 0 : g);
        }
}

/* Compose both layers on the CPU into bg_pixels / fg_pixels (RGBA, row 0 top). */
static inline void video_compose_cpu(video_state_t *vid, memory_bus_t *bus) {
    render_bg_layer(bus, &vid->layers);
    render_fg_layer(bus, &vid->layers);
    tile_pen_lut(bus, g_video_pen);

    int n = VIDEO_WIDTH * VIDEO_HEIGHT;
    for (int i = 0; i < n; i++) {
        int o = i * 4;
        /* BG tiles, OPAQUE (matches MAME TILEMAP_DRAW_OPAQUE for layers C/D);
         * empty cells render palette[0] = the backdrop colour. */
        uint16_t bc = vid->layers.bg[i] & 0x7FFF;
        vid->bg_pixels[o+0] = g_video_pen[bc][0];
        vid->bg_pixels[o+1] = g_video_pen[bc][1];
        vid->bg_pixels[o+2] = g_video_pen[bc][2];
        vid->bg_pixels[o+3] = 255;

        /* FG tiles, alpha-keyed. */
        uint16_t fc = vid->layers.fg[i] & 0x7FFF;
        vid->fg_pixels[o+0] = g_video_pen[fc][0];
        vid->fg_pixels[o+1] = g_video_pen[fc][1];
        vid->fg_pixels[o+2] = g_video_pen[fc][2];
        vid->fg_pixels[o+3] = vid->layers.alpha[i];
    }
}

/* Upload the RAM the GPU compositor reads (each part only if what it is made of
 * changed) and draw both layers in one pass. Call outside any other pass. */
static inline void video__compose_gpu(video_state_t *vid, memory_bus_t *bus,
                                      bool tile_changed, bool gfx_changed, bool pens_changed) {
    if (tile_changed)
        sg_update_image(vid->tile_ram, &(sg_image_data){
            .mip_levels[0] = { .ptr = bus->tile, .size = VIDEO_TILE_GPU_W * VIDEO_TILE_GPU_H } });
    if (gfx_changed)
        sg_update_image(vid->gfx_ram, &(sg_image_data){
            .mip_levels[0] = { .ptr = bus->tmapgfx, .size = TMAPGFX_SIZE } });
    if (pens_changed) {
        const uint8_t (*pc)[32] = vid->pen_chan;
        for (int i = 0; i < VIDEO_PAL_GPU_W * VIDEO_PAL_GPU_H; i++) {
            uint16_t c = pal_read16(bus, i);
            uint8_t *t = &vid->pal_texels[i * 4];
            t[0] = pc[0][c & 31]; t[1] = pc[1][(c >> 5) & 31]; t[2] = pc[2][(c >> 10) & 31]; t[3] = 255;
        }
        sg_update_image(vid->pal_rgba, &(sg_image_data){
            .mip_levels[0] = { .ptr = vid->pal_texels, .size = sizeof vid->pal_texels } });
    }

    float params[8];
    for (int t = 0; t < 4; t++) {
        params[t]     = (float)tileram_word(bus, 0x5000u + (uint32_t)t);   /* H scroll */
        params[4 + t] = (float)tileram_word(bus, 0x5004u + (uint32_t)t);   /* V scroll / control */
    }
    sg_begin_pass(&(sg_pass){
        .action.colors[0].load_action = SG_LOADACTION_DONTCARE,
        .action.colors[1].load_action = SG_LOADACTION_DONTCARE,
        .attachments.colors[0] = vid->bg_att,
        .attachments.colors[1] = vid->fg_att,
        .label = "tile-compose",
    });
    sg_apply_pipeline(vid->tile_pipeline);
    sg_apply_bindings(&(sg_bindings){
        .views[0] = vid->tile_ram_view, .views[1] = vid->gfx_ram_view, .views[2] = vid->pal_rgba_view,
        .samplers[0] = vid->fetch_sampler,
    });
    sg_apply_uniforms(0, &(sg_range){ .ptr = params, .size = sizeof params });
    sg_draw(0, 3, 1);
    sg_end_pass();
    vid->gpu_composes++;
}

/*
 * Bring the back colour, BG and FG layer textures up to date with the bus.
 * Call once per frame before the swapchain pass.
 */
static inline void video_update(video_state_t *vid, memory_bus_t *bus) {
    if (!vid->initialized) return;

    /* Nothing 2D changed since the last compose: the layers and the uploaded
     * textures already show it. Sample the generations before composing, so a
     * write that lands mid-compose makes the next frame compose again. */
    uint32_t tile = bus->gen_tile, gfx = bus->gen_gfx, pal = bus->gen_pal, lut = bus->gen_lut;
    bool first = !vid->composed;
    if (!first && tile == vid->composed_tile && gfx == vid->composed_gfx &&
            pal == vid->composed_pal && lut == vid->composed_lut)
        return;
    bool pens_changed = first || pal != vid->composed_pal || lut != vid->composed_lut;
    bool tile_changed = first || tile != vid->composed_tile;
    bool gfx_changed  = first || gfx != vid->composed_gfx;
    vid->composed      = true;
    vid->composed_tile = tile;
    vid->composed_gfx  = gfx;
    vid->composed_pal  = pal;
    vid->composed_lut  = lut;
    if (pens_changed) video_pen_channels(bus, vid->pen_chan);

    if (vid->gpu) {
        video__compose_gpu(vid, bus, tile_changed, gfx_changed, pens_changed);
    } else {
        video_compose_cpu(vid, bus);
        sg_update_image(vid->bg_image, &(sg_image_data){
            .mip_levels[0] = { .ptr = vid->bg_pixels, .size = sizeof(vid->bg_pixels) },
        });
        sg_update_image(vid->fg_image, &(sg_image_data){
            .mip_levels[0] = { .ptr = vid->fg_pixels, .size = sizeof(vid->fg_pixels) },
        });
    }

    /* 1×1 solid back-back colour (palette[0] through the pen tables); only the
     * palette or the colour tables can change it, and pen_chan was rebuilt
     * above when either did. */
    if (pens_changed) {
        uint16_t back = back_color_555(bus);
        vid->back_pixel[0] = vid->pen_chan[0][back & 31];
        vid->back_pixel[1] = vid->pen_chan[1][(back >> 5) & 31];
        vid->back_pixel[2] = vid->pen_chan[2][(back >> 10) & 31];
        vid->back_pixel[3] = 255;
        sg_update_image(vid->back_image, &(sg_image_data){
            .mip_levels[0] = { .ptr = vid->back_pixel, .size = sizeof(vid->back_pixel) },
        });
    }
}

#endif /* VIDEO_WINDOW_H */
