/*
 * video_window.h — per-frame tile compositor → GPU texture upload.
 *
 * Owns the CPU-side pixel buffer, the sokol image/view/sampler used as the
 * source texture for the swapchain quad in game_render.h.
 *
 * Despite the legacy filename, there is no ImGui window here.  The actual
 * on-screen draw happens in the swapchain pass via game_render_draw_game().
 */
#ifndef VIDEO_WINDOW_H
#define VIDEO_WINDOW_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sokol_gfx.h"

#include "constants.h"
#include "memory.h"
#include "tile_renderer.h"

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
} video_state_t;

static inline sg_image video__make_img(const char *label) {
    return sg_make_image(&(sg_image_desc){
        .width        = VIDEO_WIDTH,
        .height       = VIDEO_HEIGHT,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .usage        = { .stream_update = true },
        .label        = label,
    });
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

    vid->initialized = true;
}

static inline void video_shutdown(video_state_t *vid) {
    if (!vid->initialized) return;
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

/*
 * Render BG and FG tile layers into separate alpha-keyed RGBA textures plus a
 * 1×1 solid back-back colour, then upload.  Both tile layers carry the per-pixel
 * alpha key from the tile renderer; the back colour is the backmost solid fill.
 * Call once per frame before the swapchain pass.
 */
static inline void video_update(video_state_t *vid, memory_bus_t *bus) {
    if (!vid->initialized) return;

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

    /* 1×1 solid back-back colour (change_bg_color → palette[0x1002]). */
    uint16_t back = back_color_555(bus);
    vid->back_pixel[0] = (uint8_t)BGR555_R(back);
    vid->back_pixel[1] = (uint8_t)BGR555_G(back);
    vid->back_pixel[2] = (uint8_t)BGR555_B(back);
    vid->back_pixel[3] = 255;

    sg_update_image(vid->bg_image, &(sg_image_data){
        .mip_levels[0] = { .ptr = vid->bg_pixels, .size = sizeof(vid->bg_pixels) },
    });
    sg_update_image(vid->fg_image, &(sg_image_data){
        .mip_levels[0] = { .ptr = vid->fg_pixels, .size = sizeof(vid->fg_pixels) },
    });
    sg_update_image(vid->back_image, &(sg_image_data){
        .mip_levels[0] = { .ptr = vid->back_pixel, .size = sizeof(vid->back_pixel) },
    });
}

#endif /* VIDEO_WINDOW_H */
