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

/* Tile RAM words the compositor reads, one R16UI texel each: the four tilemaps
 * (words 0x0000-0x3FFF), their per-line H scroll tables (0x4000 + 0x200*t, 384
 * lines) and both window masks (0x6000 / 0x6800, four words a line). The scroll
 * registers (0x5000-0x5007) go in as uniforms. */
#define VIDEO_TILE_GPU_W 256
#define VIDEO_TILE_GPU_H 112
#define VIDEO_TILE_WORDS (VIDEO_TILE_GPU_W * VIDEO_TILE_GPU_H)
#define VIDEO_GFX_GPU_W  1024
#define VIDEO_GFX_GPU_H  512
#define VIDEO_PAL_GPU_W  256
#define VIDEO_PAL_GPU_H  32
/* The GPU layers are recomposed in 8x8 screen blocks: only the blocks a tile RAM
 * change can reach are drawn again (video__tile_dirty). */
#define VIDEO_BLK_W      (VIDEO_WIDTH / 8)
#define VIDEO_BLK_H      (VIDEO_HEIGHT / 8)
_Static_assert(VIDEO_TILE_WORDS >= 0x6800 + VIDEO_HEIGHT * 4, "tile texture covers the window masks");
_Static_assert(VIDEO_TILE_WORDS * 2 <= TILE_SIZE, "tile texture fits tile RAM");
_Static_assert(VIDEO_BLK_W * 8 == VIDEO_WIDTH && VIDEO_BLK_H * 8 == VIDEO_HEIGHT, "screen is whole 8x8 blocks");
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
    /* What those composes uploaded: tile RAM, tile graphics, pen texture. */
    uint64_t      up_tile, up_gfx, up_pens;
    /* Incremental composes: tile_words is the snapshot the GPU was last given and
     * the targets show; a later snapshot is compared with it to find the blocks to
     * draw again. targets_valid: the targets hold a whole compose. */
    uint16_t      tile_words[VIDEO_TILE_WORDS];
    bool          targets_valid;
    uint64_t      partial_composes, composed_blocks;
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
 * bits 14:7, category bit 15, the 4bpp texel with its 16-bit byteswap. The
 * targets get the pen p (bank * 16 + colour index); the pen texture holds
 * tile_pen_lut of every palette entry, so p's screen colour is texel p. y is the
 * target row, which GL counts from the bottom, so the target's row 0 holds
 * screen row 0 as the CPU uploads do. h_scr / v_scr are words 0x5000-0x5003 /
 * 0x5004-0x5007.
 */
static const char *video_tile_fs_glsl =
    "#version 410\n"
    "uniform vec4 tile_params[3];\n"
    "uniform usampler2D tile_smp;\n"
    "uniform usampler2D gfx_smp;\n"
    "layout(location = 0) out vec4 bg_out;\n"
    "layout(location = 1) out vec4 fg_out;\n"
    "int tw(int word) { return int(texelFetch(tile_smp, ivec2(word & 255, word >> 8), 0).r); }\n"
    "int gb(int off) { return int(texelFetch(gfx_smp, ivec2(off & 1023, off >> 10), 0).r); }\n"
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
    /* The targets hold the pen, not its colour: low byte in red, high byte in
     * green (pens are < 4096), alpha 1 where the layer drew. The layer quads look
     * the colour up (game_render_draw_indexed), so a palette write only changes
     * the pen texture and the layers need not be composed again. */
    "vec4 pen_out(int pen, float a) { return vec4(float(pen & 255) / 255.0, float(pen >> 8) / 255.0, 0.0, a); }\n"
    /* Layer order, as the CPU draws it: behind the 3D, the backdrop (pen 0), then
     * tilemaps 3 and 2 opaque, then 1 and 0 where their tiles are category 0, each
     * over the last; in front, tilemaps 3, 2, 1, 0 where category 1. So the first
     * tilemap from 0 up that qualifies decides each layer, and the walk stops once
     * both are decided. tile_params[2][t] is 0 when neither tilemap t nor its pair
     * holds a category 1 tile anywhere: t cannot give the front layer, so once the
     * back one is decided it is not sampled at all. */
    "void main() {\n"
    "  int x = int(gl_FragCoord.x), y = int(gl_FragCoord.y);\n"
    "  int bg = -1, fg = -1;\n"
    "  for (int t = 0; t < 4; t++) {\n"
    "    bool want_fg = fg < 0 && tile_params[2][t] != 0.0;\n"
    "    if (bg >= 0 && !want_fg) continue;\n"
    "    int ci, cat, pen;\n"
    "    if (!tm_px(t, x, y, ci, cat, pen)) continue;\n"
    "    if (bg < 0 && (t >= 2 || (ci != 0 && cat == 0))) bg = pen;\n"
    "    if (fg < 0 && ci != 0 && cat == 1) fg = pen;\n"
    "  }\n"
    "  bg_out = pen_out(bg < 0 ? 0 : bg, 1.0);\n"
    "  fg_out = fg < 0 ? vec4(0.0) : pen_out(fg, 1.0);\n"
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
    d.uniform_blocks[0].size  = 12 * sizeof(float);
    d.uniform_blocks[0].glsl_uniforms[0].glsl_name   = "tile_params";
    d.uniform_blocks[0].glsl_uniforms[0].type        = SG_UNIFORMTYPE_FLOAT4;
    d.uniform_blocks[0].glsl_uniforms[0].array_count = 3;
    static const char *const names[2] = { "tile_smp", "gfx_smp" };
    for (int i = 0; i < 2; i++) {
        d.views[i].texture.stage       = SG_SHADERSTAGE_FRAGMENT;
        d.views[i].texture.image_type  = SG_IMAGETYPE_2D;
        d.views[i].texture.sample_type = SG_IMAGESAMPLETYPE_UINT;
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
        .pixel_format = SG_PIXELFORMAT_R16UI, .usage = { .dynamic_update = true }, .label = "tile-ram" });
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
    vid->targets_valid = false;
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

/* ---- Incremental GPU compose ----------------------------------------------- */

typedef struct {
    uint8_t blk[VIDEO_BLK_H][VIDEO_BLK_W];   /* 1: the 8x8 block must be drawn again */
    int     count;
    bool    full;
} video_dirty_t;

/* Mark the blocks covering screen pixels [x0, x1) x [y0, y1), clipped. */
static inline void video__mark(video_dirty_t *d, int x0, int x1, int y0, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > VIDEO_WIDTH)  x1 = VIDEO_WIDTH;
    if (y1 > VIDEO_HEIGHT) y1 = VIDEO_HEIGHT;
    if (x0 >= x1 || y0 >= y1) return;
    for (int by = y0 >> 3; by <= (y1 - 1) >> 3; by++)
        for (int bx = x0 >> 3; bx <= (x1 - 1) >> 3; bx++)
            if (!d->blk[by][bx]) { d->blk[by][bx] = 1; d->count++; }
}

/* The screen pixels where tilemap cell (cx, cy) of tilemap l can show, marked.
 * A pixel of drawing tilemap t samples tilemap l at ((x - h) & 511, (y + v) & 511)
 * with t's own scroll: t == l, or under a split mode the even tilemap of the pair
 * drawing the odd one. The coordinates wrap at 512, so a cell's 8 pixels can
 * also sit 512 lower; with a per-line H scroll the cell's lines are marked whole. */
static inline void video__mark_cell(video_dirty_t *d, const uint16_t *w, int l, int cx, int cy) {
    for (int k = 0; k < 2; k++) {
        int t = k ? (l & 2) : l;
        if (k && t == l) break;
        uint16_t hscr = w[0x5000 + t], vscr = w[0x5004 + t], ctrl = w[0x5004 + (t & 2)];
        int mode = (ctrl & 0x6000) >> 13;
        if (vscr & 0x8000) continue;             /* t disabled */
        if (mode && (t & 1)) continue;           /* odd tilemap idle under a split */
        if (k && !mode) continue;                /* no split: t draws only itself */
        int y0 = (cy * 8 - (vscr & 0x1FF)) & 511;
        for (int j = 0; j < 2; j++) {
            int ys = y0 - 512 * j;
            if (hscr & 0x8000) {
                video__mark(d, 0, VIDEO_WIDTH, ys, ys + 8);
            } else {
                int x0 = (cx * 8 + (hscr & 0x1FF)) & 511;
                video__mark(d, x0, x0 + 8, ys, ys + 8);
                video__mark(d, x0 - 512, x0 - 504, ys, ys + 8);
            }
        }
    }
}

/* Which blocks the change from `old` to `cur` tile RAM can alter on screen. A
 * scroll or control register change moves everything: full. A row scroll word
 * redraws its line, a window mask word its 128-pixel span of the line, a tilemap
 * cell where it shows. Words the compositor never reads change nothing. */
static inline void video__tile_dirty(video_dirty_t *d, const uint16_t *old, const uint16_t *cur) {
    for (int i = 0; i < VIDEO_TILE_WORDS && !d->full; i += 64) {
        int n = VIDEO_TILE_WORDS - i < 64 ? VIDEO_TILE_WORDS - i : 64;
        if (!memcmp(old + i, cur + i, (size_t)n * sizeof *cur)) continue;
        for (int wi = i; wi < i + n; wi++) {
            if (old[wi] == cur[wi]) continue;
            if (wi < 0x4000) {
                video__mark_cell(d, cur, wi >> 12, wi & 63, (wi >> 6) & 63);
            } else if (wi < 0x4800) {
                int y = (wi - 0x4000) & 0x1FF;
                video__mark(d, 0, VIDEO_WIDTH, y, y + 1);
            } else if (wi >= 0x5000 && wi < 0x5008) {
                d->full = true;
                break;
            } else if (wi >= 0x6000 && wi < 0x7000) {
                int off = (wi - 0x6000) & 0x7FF;         /* 0x6000 and 0x6800 alike */
                if (off < VIDEO_HEIGHT * 4) {
                    int y = off / 4, x = (off % 4) * 128;
                    video__mark(d, x, x + 128, y, y + 1);
                }
            }
        }
    }
}

/* Upload the RAM the GPU compositor reads (each part only if what it is made of
 * changed) and draw both layers in one pass: all of them when the targets are new
 * or graphics or a scroll register changed, otherwise only the blocks the tile
 * RAM change reaches, over the targets' previous contents. A palette or colour
 * table change only uploads the pen texture: the targets hold pens. Tile RAM is
 * snapshot first and everything (upload, registers, the comparison next time)
 * uses the snapshot, so a write landing meanwhile is caught by the next compose.
 * Call outside any other pass. */
static inline void video__compose_gpu(video_state_t *vid, memory_bus_t *bus,
                                      bool tile_changed, bool gfx_changed, bool pens_changed) {
    static uint16_t snap[VIDEO_TILE_WORDS];
    static video_dirty_t d;
    memset(&d, 0, sizeof d);
    d.full = !vid->targets_valid || gfx_changed;   /* pens: the targets hold pens, not colours */
    if (tile_changed) {
        memcpy(snap, bus->tile, sizeof snap);         /* little-endian words, as on the host */
        if (!d.full) video__tile_dirty(&d, vid->tile_words, snap);
        memcpy(vid->tile_words, snap, sizeof snap);
        sg_update_image(vid->tile_ram, &(sg_image_data){
            .mip_levels[0] = { .ptr = vid->tile_words, .size = sizeof vid->tile_words } });
        vid->up_tile++;
    }
    if (gfx_changed) {
        sg_update_image(vid->gfx_ram, &(sg_image_data){
            .mip_levels[0] = { .ptr = bus->tmapgfx, .size = TMAPGFX_SIZE } });
        vid->up_gfx++;
    }
    if (pens_changed) {
        vid->up_pens++;
        const uint8_t (*pc)[32] = vid->pen_chan;
        for (int i = 0; i < VIDEO_PAL_GPU_W * VIDEO_PAL_GPU_H; i++) {
            uint16_t c = pal_read16(bus, i);
            uint8_t *t = &vid->pal_texels[i * 4];
            t[0] = pc[0][c & 31]; t[1] = pc[1][(c >> 5) & 31]; t[2] = pc[2][(c >> 10) & 31]; t[3] = 255;
        }
        sg_update_image(vid->pal_rgba, &(sg_image_data){
            .mip_levels[0] = { .ptr = vid->pal_texels, .size = sizeof vid->pal_texels } });
    }
    if (!d.full && d.count == 0) return;              /* pens only, or words nothing reads */
    if (d.count > VIDEO_BLK_W * VIDEO_BLK_H * 3 / 4) d.full = true;

    /* The dirty blocks as rectangles: runs along each block row, a run extended
     * downwards while the next row has the same run. Too many: draw it all. */
    enum { MAX_RECTS = 48 };
    int rx[MAX_RECTS], ry[MAX_RECTS], rw[MAX_RECTS], rh[MAX_RECTS], nr = 0;
    if (!d.full) {
        for (int by = 0; by < VIDEO_BLK_H && !d.full; by++) {
            for (int bx = 0; bx < VIDEO_BLK_W; ) {
                if (!d.blk[by][bx]) { bx++; continue; }
                int x0 = bx;
                while (bx < VIDEO_BLK_W && d.blk[by][bx]) bx++;
                int r = nr - 1;
                while (r >= 0 && !(rx[r] == x0 && rw[r] == bx - x0 && ry[r] + rh[r] == by)) r--;
                if (r >= 0) { rh[r]++; continue; }
                if (nr == MAX_RECTS) { d.full = true; break; }
                rx[nr] = x0; ry[nr] = by; rw[nr] = bx - x0; rh[nr] = 1; nr++;
            }
        }
    }

    /* Which tilemaps hold any category 1 (in front of the 3D) tile. */
    uint16_t cat1[4] = {0};
    for (int l = 0; l < 4; l++) {
        const uint16_t *cells = vid->tile_words + 0x1000 * l;
        uint16_t any = 0;
        for (int i = 0; i < 0x1000; i++) any |= cells[i];
        cat1[l] = any & 0x8000;
    }
    float params[12];
    for (int t = 0; t < 4; t++) {
        params[t]     = (float)vid->tile_words[0x5000 + t];   /* H scroll */
        params[4 + t] = (float)vid->tile_words[0x5004 + t];   /* V scroll / control */
        params[8 + t] = (cat1[t] | cat1[t ^ 1]) ? 1.0f : 0.0f;  /* t draws itself or, split, its pair */
    }
    sg_load_action load = d.full ? SG_LOADACTION_DONTCARE : SG_LOADACTION_LOAD;
    sg_begin_pass(&(sg_pass){
        .action.colors[0].load_action = load,
        .action.colors[1].load_action = load,
        .attachments.colors[0] = vid->bg_att,
        .attachments.colors[1] = vid->fg_att,
        .label = "tile-compose",
    });
    sg_apply_pipeline(vid->tile_pipeline);
    sg_apply_bindings(&(sg_bindings){
        .views[0] = vid->tile_ram_view, .views[1] = vid->gfx_ram_view,
        .samplers[0] = vid->fetch_sampler,
    });
    sg_apply_uniforms(0, &(sg_range){ .ptr = params, .size = sizeof params });
    if (d.full) {
        sg_draw(0, 3, 1);
        vid->composed_blocks += (uint64_t)(VIDEO_BLK_W * VIDEO_BLK_H);
    } else {
        /* Target row y is screen row y (gl_FragCoord), so a bottom-left scissor
         * takes the block rows as they are. */
        for (int r = 0; r < nr; r++) {
            sg_apply_scissor_rect(rx[r] * 8, ry[r] * 8, rw[r] * 8, rh[r] * 8, false);
            sg_draw(0, 3, 1);
        }
        vid->composed_blocks += (uint64_t)d.count;
        vid->partial_composes++;
    }
    sg_end_pass();
    vid->targets_valid = true;
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
