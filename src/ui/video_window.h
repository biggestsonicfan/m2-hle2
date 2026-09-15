/*
 * video_window.h — per-frame tile compositor → GPU textures.
 *
 * Produces the textures the swapchain quads in game_render.h draw: the solid
 * back colour, the background tile layer and the foreground/HUD layer. Two
 * ways, same picture:
 *
 *   CPU  render_sys24_pair (tile_renderer.h) composes both layers into RGBA
 *        buffers that are uploaded whole. Every backend; the only path on D3D11
 *        and the dummy backend.
 *   GPU  on GL backends, tile RAM, tile graphics and palette go up as textures
 *        (each only when a write changed it) and a shader composes both layers
 *        into render targets with integer fetches, following render_sys24_pair
 *        step for step. The layer textures are laid out like the CPU uploads
 *        (row 0 = top), so drawing them is unchanged.
 *
 * Either way nothing is composed on a frame where no tile RAM, graphics or
 * palette byte changed.
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

/* Tile RAM bytes the compositor reads: the four tilemaps (0x0000-0x7FFF), both
 * per-row H scroll tables (words 0x4000/0x4400 + 384) and the scroll/control
 * registers (words 0x5000-0x5006). */
#define VIDEO_TILE_GPU_W 256
#define VIDEO_TILE_GPU_H 176
#define VIDEO_GFX_GPU_W  1024
#define VIDEO_GFX_GPU_H  512
#define VIDEO_PAL_GPU_W  256
#define VIDEO_PAL_GPU_H  32
_Static_assert(VIDEO_TILE_GPU_W * VIDEO_TILE_GPU_H >= 0x5007 * 2, "tile texture covers the registers");
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
    uint32_t      composed_tile, composed_gfx, composed_pal;

    /* GPU compositor (GL backends): RAM textures, layer targets, shader. */
    bool          gpu;
    sg_image      tile_ram, gfx_ram, pal_rgba;
    sg_view       tile_ram_view, gfx_ram_view, pal_rgba_view;
    sg_view       bg_att, fg_att;
    sg_sampler    fetch_sampler;
    sg_shader     tile_shader;
    sg_pipeline   tile_pipeline;
    uint32_t      uploaded_tile, uploaded_gfx, uploaded_pal;
    bool          uploaded;
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
 * pair_px is render_sys24_pair for one pixel: per-row H scroll or the uniform
 * one, the per-row window split choosing the even or odd tilemap, the 64x64
 * tilemap entry (palette bank bits 14:7, char 13:0, priority bit 15), and the
 * 4bpp texel with its 16-bit byteswap. sy is the target row, which GL counts
 * from the bottom — so the texture's row 0 holds screen row 0, as the CPU
 * uploads do. tile_params[0] = (FG hscr, FG vscr/ctrl, BG hscr, BG vscr/ctrl),
 * tile_params[1].x = 1 for the FG layer, 0 for BG.
 */
static const char *video_tile_fs_glsl =
    "#version 410\n"
    "uniform vec4 tile_params[2];\n"
    "uniform usampler2D tile_smp;\n"
    "uniform usampler2D gfx_smp;\n"
    "uniform sampler2D pal_smp;\n"
    "out vec4 frag_color;\n"
    "int tb(int off) { return int(texelFetch(tile_smp, ivec2(off & 255, off >> 8), 0).r); }\n"
    "int gb(int off) { return int(texelFetch(gfx_smp, ivec2(off & 1023, off >> 10), 0).r); }\n"
    "vec3 pal(int idx) { return texelFetch(pal_smp, ivec2(idx & 255, idx >> 8), 0).rgb; }\n"
    "ivec3 pair_px(int pair, int hscr, int vc, int sx, int sy) {\n"
    "  int l0 = pair == 0 ? 0x0000 : 0x4000;\n"
    "  int l1 = l0 + 0x2000;\n"
    "  int hstb = pair == 0 ? 0x4000 : 0x4400;\n"
    "  int wy = (sy + (vc & 0x1ff)) & 511;\n"
    "  bool rowscroll = (hscr & 0x8000) != 0;\n"
    "  int rh = hscr;\n"
    "  if (rowscroll) { int b = (hstb + sy) * 2; rh = tb(b) | (tb(b + 1) << 8); }\n"
    "  int h = rh & 0x1ff;\n"
    "  int tmap = l0;\n"
    "  if ((vc & 0x6000) != 0 && rowscroll) {\n"
    "    bool l1_is_even = (rh & 0x200) != 0;\n"
    "    bool use_even = sx < h ? l1_is_even : !l1_is_even;\n"
    "    tmap = use_even ? l0 : l1;\n"
    "  }\n"
    "  int wx = (sx + h) & 511;\n"
    "  int te = tmap + ((wy >> 3) * 64 + (wx >> 3)) * 2;\n"
    "  int entry = tb(te) | (tb(te + 1) << 8);\n"
    "  int px = wx & 7;\n"
    "  int b = gb((entry & 0x3FFF) * 32 + (wy & 7) * 4 + ((px >> 1) ^ 1));\n"
    "  int ci = (px & 1) != 0 ? (b & 15) : (b >> 4);\n"
    "  return ivec3(ci, ((entry >> 7) & 0xFF) * 16 + ci, entry >> 15);\n"
    "}\n"
    "void main() {\n"
    "  int sx = int(gl_FragCoord.x), sy = int(gl_FragCoord.y);\n"
    "  int fg_hscr = int(tile_params[0].x), fg_vc = int(tile_params[0].y);\n"
    "  int bg_hscr = int(tile_params[0].z), bg_vc = int(tile_params[0].w);\n"
    "  bool fg_on = (fg_vc & 0x8000) == 0;\n"
    "  if (tile_params[1].x > 0.5) {\n"
    "    if (!fg_on) { frag_color = vec4(0.0); return; }\n"
    "    ivec3 f = pair_px(0, fg_hscr, fg_vc, sx, sy);\n"
    "    frag_color = vec4(pal(f.y), (f.x != 0 && f.z != 0) ? 1.0 : 0.0);\n"
    "  } else {\n"
    "    vec3 c = pal(0);\n"
    "    if ((bg_vc & 0x8000) == 0) c = pal(pair_px(1, bg_hscr, bg_vc, sx, sy).y);\n"
    "    if (fg_on) {\n"
    "      ivec3 f = pair_px(0, fg_hscr, fg_vc, sx, sy);\n"
    "      if (f.x != 0 && f.z == 0) c = pal(f.y);\n"
    "    }\n"
    "    frag_color = vec4(c, 1.0);\n"
    "  }\n"
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
        .colors[0].pixel_format = SG_PIXELFORMAT_RGBA8,
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
        .pixel_format = SG_PIXELFORMAT_RGBA8, .usage = { .dynamic_update = true }, .label = "tile-palette" });
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
    vid->uploaded = false;
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

/* Compose both layers on the CPU into bg_pixels / fg_pixels (RGBA, row 0 top). */
static inline void video_compose_cpu(video_state_t *vid, memory_bus_t *bus) {
    render_bg_layer(bus, &vid->layers);
    render_fg_layer(bus, &vid->layers);

    int n = VIDEO_WIDTH * VIDEO_HEIGHT;
    for (int i = 0; i < n; i++) {
        int o = i * 4;
        /* BG tiles, OPAQUE (matches MAME TILEMAP_DRAW_OPAQUE for layers C/D);
         * empty cells render palette[0] = the backdrop colour. */
        uint16_t bc = vid->layers.bg[i];
        vid->bg_pixels[o+0] = (uint8_t)BGR555_R(bc);
        vid->bg_pixels[o+1] = (uint8_t)BGR555_G(bc);
        vid->bg_pixels[o+2] = (uint8_t)BGR555_B(bc);
        vid->bg_pixels[o+3] = 255;

        /* FG tiles, alpha-keyed. */
        uint16_t fc = vid->layers.fg[i];
        vid->fg_pixels[o+0] = (uint8_t)BGR555_R(fc);
        vid->fg_pixels[o+1] = (uint8_t)BGR555_G(fc);
        vid->fg_pixels[o+2] = (uint8_t)BGR555_B(fc);
        vid->fg_pixels[o+3] = vid->layers.alpha[i];
    }
}

/* Upload the RAM the GPU compositor reads (each part only if it changed) and
 * draw both layers into their targets. Call outside any other pass. */
static inline void video__compose_gpu(video_state_t *vid, memory_bus_t *bus,
                                      uint32_t tile, uint32_t gfx, uint32_t pal) {
    if (!vid->uploaded || tile != vid->uploaded_tile)
        sg_update_image(vid->tile_ram, &(sg_image_data){
            .mip_levels[0] = { .ptr = bus->tile, .size = VIDEO_TILE_GPU_W * VIDEO_TILE_GPU_H } });
    if (!vid->uploaded || gfx != vid->uploaded_gfx)
        sg_update_image(vid->gfx_ram, &(sg_image_data){
            .mip_levels[0] = { .ptr = bus->tmapgfx, .size = TMAPGFX_SIZE } });
    if (!vid->uploaded || pal != vid->uploaded_pal) {
        for (int i = 0; i < VIDEO_PAL_GPU_W * VIDEO_PAL_GPU_H; i++) {
            uint16_t c = pal_read16(bus, i);
            uint8_t *t = &vid->pal_texels[i * 4];
            t[0] = (uint8_t)BGR555_R(c); t[1] = (uint8_t)BGR555_G(c); t[2] = (uint8_t)BGR555_B(c); t[3] = 255;
        }
        sg_update_image(vid->pal_rgba, &(sg_image_data){
            .mip_levels[0] = { .ptr = vid->pal_texels, .size = sizeof vid->pal_texels } });
    }
    vid->uploaded = true;
    vid->uploaded_tile = tile; vid->uploaded_gfx = gfx; vid->uploaded_pal = pal;

    float params[8] = {
        tileram_word(bus, 0x5000), tileram_word(bus, 0x5004),   /* FG pair: hscr, vscr/ctrl */
        tileram_word(bus, 0x5002), tileram_word(bus, 0x5006),   /* BG pair */
        0.0f, 0.0f, 0.0f, 0.0f,
    };
    const sg_view targets[2] = { vid->bg_att, vid->fg_att };
    for (int layer = 0; layer < 2; layer++) {
        params[4] = (float)layer;
        sg_begin_pass(&(sg_pass){
            .action.colors[0].load_action = SG_LOADACTION_DONTCARE,
            .attachments.colors[0] = targets[layer],
            .label = layer ? "tile-compose-fg" : "tile-compose-bg",
        });
        sg_apply_pipeline(vid->tile_pipeline);
        sg_apply_bindings(&(sg_bindings){
            .views[0] = vid->tile_ram_view, .views[1] = vid->gfx_ram_view, .views[2] = vid->pal_rgba_view,
            .samplers[0] = vid->fetch_sampler,
        });
        sg_apply_uniforms(0, &(sg_range){ .ptr = params, .size = sizeof params });
        sg_draw(0, 3, 1);
        sg_end_pass();
    }
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
    uint32_t tile = bus->gen_tile, gfx = bus->gen_gfx, pal = bus->gen_pal;
    if (vid->composed && tile == vid->composed_tile && gfx == vid->composed_gfx && pal == vid->composed_pal)
        return;
    bool pal_changed = !vid->composed || pal != vid->composed_pal;
    vid->composed      = true;
    vid->composed_tile = tile;
    vid->composed_gfx  = gfx;
    vid->composed_pal  = pal;

    if (vid->gpu) {
        video__compose_gpu(vid, bus, tile, gfx, pal);
    } else {
        video_compose_cpu(vid, bus);
        sg_update_image(vid->bg_image, &(sg_image_data){
            .mip_levels[0] = { .ptr = vid->bg_pixels, .size = sizeof(vid->bg_pixels) },
        });
        sg_update_image(vid->fg_image, &(sg_image_data){
            .mip_levels[0] = { .ptr = vid->fg_pixels, .size = sizeof(vid->fg_pixels) },
        });
    }

    /* 1×1 solid back-back colour (palette[0]); only the palette can change it. */
    if (pal_changed) {
        uint16_t back = back_color_555(bus);
        vid->back_pixel[0] = (uint8_t)BGR555_R(back);
        vid->back_pixel[1] = (uint8_t)BGR555_G(back);
        vid->back_pixel[2] = (uint8_t)BGR555_B(back);
        vid->back_pixel[3] = 255;
        sg_update_image(vid->back_image, &(sg_image_data){
            .mip_levels[0] = { .ptr = vid->back_pixel, .size = sizeof(vid->back_pixel) },
        });
    }
}

#endif /* VIDEO_WINDOW_H */
