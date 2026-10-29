/*
 * game_frame.h — the per-frame host work every frontend shares.
 *
 * game_frame_prepare composes the tile layers and turns this game frame's GEO
 * output into the model list; game_frame_draw draws back colour, background
 * tiles, the 3D scene and the foreground tiles into a letterboxed viewport of
 * the current sokol_gfx pass. The ImGui debugger (main.c) and the fullscreen
 * handheld frontend (main_sdl.c) both call these, so the two cannot drift.
 */
#ifndef GAME_FRAME_H
#define GAME_FRAME_H

#include <math.h>
#include <stdbool.h>

#include "sokol_gfx.h"

#include "cabinet_picture.h"   /* the Picture menu's cabinet: its colour tables */
#include "constants.h"
#include "cop.h"
#include "emu_thread.h"
#include "game_profile.h"
#include "game_render.h"
#include "geo3d.h"
#include "memory.h"
#include "rom_loader.h"
#include "stereo.h"
#include "video_window.h"

#include "frame_times.h"   /* g_game_frame_times: host time per stage, per frame */

/* Sub-frame interpolation factor: time since the last game frame ended,
 * normalised over one frame period. Resets when a new frame boundary lands. */
static inline float game_frame_lerp(void) {
    static int     s_last_geo_frame_end = 0;
    static int64_t s_frame_end_us       = 0;
    float lerp_t = 1.0f;
    int cur_frame_end = g_cop.geo_frame_end;
    int64_t now_us    = emu_now_us();
    if (cur_frame_end != s_last_geo_frame_end) {
        s_last_geo_frame_end = cur_frame_end;
        s_frame_end_us       = now_us;
    } else if (s_frame_end_us > 0) {
        lerp_t = (float)(now_us - s_frame_end_us) / (float)EMU_SLICE_US;
        if (lerp_t < 0.0f) lerp_t = 0.0f;
        if (lerp_t > 1.0f) lerp_t = 1.0f;
    }
    return lerp_t;
}

/* The frame's objects' distances from the eye, for the 3D focus (stereo.h). */
static inline void game_frame__note_scene(const geo3d_state_t *geo3d) {
    float zs[MAX_GEO_MODELS];
    int n = 0;
    for (int i = 0; i < geo3d->captured_count; i++) {
        const captured_model_t *c = &geo3d->captured[i];
        if (c->view_space && c->has_matrix) zs[n++] = fabsf(c->matrix[11]);
    }
    stereo_note_scene(zs, n);
}

/* Compose the tile layers and scan this frame's slice of the 3D output. Call
 * once per host frame, before the pass. read_camera: drive the 3D camera from
 * the game's camera struct (only meaningful once the emu thread runs). */
static inline void game_frame_prepare(video_state_t *video, geo3d_state_t *geo3d,
                                      memory_bus_t *bus, const romset_t *rs,
                                      bool read_camera) {
    emu_mutex_t *twin = emu_twin_ui_enter();   /* a GGPO twin's hidden cabinet is not drawn */
    int64_t t0 = emu_now_us();
    int zone = hprof_enter(HPROF_COMPOSE);   /* host_prof.h */
    video_update(video, bus);
    int64_t t1 = emu_now_us();
    g_game_frame_times.compose_us += t1 - t0;
    hprof_enter(HPROF_SCAN);

    if (read_camera && geo3d->use_game_view && g_active_profile &&
            g_active_profile->quirks.camera_struct_addr) {
        geo3d_read_game_view(geo3d, bus,
                             g_active_profile->quirks.camera_struct_addr,
                             g_active_profile->quirks.camera_angle_addr);
    }

    if (geo3d->enabled && g_active_profile && rs->main_data && rs->polygons) {
        const game_quirks_t *q = &g_active_profile->quirks;
        const model_table_t table = profile_model_table(q);
        const geo3d_rom_t rom = { rs->main_data, rs->main_data_size, &table };
        /* One snapshot for the walk and the decode: the texture points and
         * polygon RAM it reads are the ones published with it (memory.h). */
        const uint32_t *snap = g_geodl_snap;
        g_geo_rs = geodl_raster_for(snap);
        g_geol_held = *geodl_held_for(snap);
        /* Set each frame, both ways: a game loaded after m2-snake in the same
         * process used to keep the homebrew's flat colours. */
        g_geo_flat_color = q->geo_displaylist ? 1 : 0;
        if (q->geo_displaylist) {
            /* Authentic hardware path: decode the GEO display list the i960 built
             * in bufferram. Scan the snapshot captured at geo_flush (g_geodl_snap),
             * not live bufferram — the SHARC HLE aliases bufferram and clobbers the
             * list between flushes. Every m2-snake homebrew uses read_start 0x10000. */
            const geo3d_list_t list = { snap, BUFF_RAM_SIZE / 4, 0x10000 };
            if (g_geodl_snap_ready)
                geo3d_scan_displaylist(geo3d, &list, &rom, bus->palette, PALETTE_SIZE);
        } else if (!(g_geodl_snap_ready &&
                     geo3d_scan_geo_list(geo3d, &(const geo3d_list_t){ snap, BUFF_RAM_SIZE / 4, g_geodl_snap_rstart },
                                         (int16_t)mem_read16(bus, H_SYNC_BASE),
                                         (int16_t)mem_read16(bus, V_SYNC_BASE), &rom))) {
            /* No display list yet, or it did not reach END (only the first two
             * frames after boot): no 3D this frame. */
            geo3d->captured_count = 0;
        }
    } else {
        geo3d_lines_reset();
    }
    if (g_stereo.on) game_frame__note_scene(geo3d);
    g_game_frame_times.scan_us += emu_now_us() - t1;
    hprof_leave(zone);
    emu_twin_ui_leave(twin);
}

/* Upload the texture atlas and the luma/colorxlat tables the game changed. */
static inline void game_frame__upload(memory_bus_t *bus) {
    /* Decode both 4-bit luma texture banks → GPU atlas for textured fills, and
     * upload the luma/colorxlat tables — each only when a write changed its RAM
     * since the last upload (sampled before uploading, so a write that lands
     * meanwhile uploads again next frame). The atlas is 4 MB: re-sending it
     * every frame was the largest single cost on a handheld GPU. */
    static bool     s_atlas_valid = false, s_luts_valid = false;
    static uint32_t s_atlas_gen, s_luts_gen;
    uint32_t tex_gen = bus->gen_tex, lut_gen = bus->gen_lut + cabinet_picture_gen();
    if (!s_atlas_valid || tex_gen != s_atlas_gen) {
        game_render_upload_atlas(bus->texram0, bus->texram1, TEXRAM0_SIZE,
                                 bus->tex_dirty[0], bus->tex_dirty[1]);
        s_atlas_valid = true;
        s_atlas_gen   = tex_gen;
    }
    if (!s_luts_valid || lut_gen != s_luts_gen) {
        game_render_upload_luts(bus->luma, cabinet_picture_xlat(bus));
        s_luts_valid = true;
        s_luts_gen   = lut_gen;
    }
}

/* Draw the game into the viewport vp of the current pass.
 * Layer order: back colour → background tiles → 3D scene → foreground/HUD. */
static inline void game_frame_draw(video_state_t *video, geo3d_state_t *geo3d,
                                   memory_bus_t *bus, const romset_t *rs,
                                   game_rect_t vp, float lerp_t) {
    const int ox = vp.x, oy = vp.y, w = vp.w, h = vp.h;
    emu_mutex_t *twin = emu_twin_ui_enter();
    int64_t t0 = emu_now_us();
    int zone = hprof_enter(HPROF_UPLOAD);   /* host_prof.h */
    game_frame__upload(bus);
    int64_t t1 = emu_now_us();
    g_game_frame_times.upload_us += t1 - t0;
    hprof_enter(HPROF_TILES);

    /* GPU-composed layers hold pens; the colours come from the pen texture. The
     * back layer there is opaque everywhere (the backdrop is its pen 0), so the
     * back colour quad under it would be drawn over whole: it is left out, and
     * the layer drawn without blending (alpha 1 blends to the source exactly). */
    game_rect_t bg = game_render_tile_rect(vp, g_game_wide.stretch_low);
    if (stereo_eye()) {
        /* the background at infinity, kept inside this eye's picture (stereo.h) */
        bg.x += (int)lroundf(stereo_infinity_ndc() * 0.5f * (float)vp.w);
        sg_apply_scissor_rect(vp.x, vp.y, vp.w, vp.h, true);
    }
    if (video->gpu) {
        game_render_draw_indexed(video->bg_view, video->pal_rgba_view, true, bg.x, bg.y, bg.w, bg.h);
    } else {
        game_render_draw_game(video->back_view, bg.x, bg.y, bg.w, bg.h);
        game_render_draw_game(video->bg_view, bg.x, bg.y, bg.w, bg.h);
    }
    int64_t t2 = emu_now_us();
    hprof_enter(HPROF_DRAW3D);
    sg_frame_stats gl0;
    if (g_game_frame_gl_counts) gl0 = sg_query_stats().cur_frame;
    if (fbvram_shown(bus)) {
        /* Test mode: the frame buffer the i960 drew in, and no polygons
         * (board/fbvram.h; Last Bronx's title picture). */
        game_render_draw_game(video->fb_view, bg.x, bg.y, bg.w, bg.h);
    } else if (geo3d->enabled && g_active_profile && rs->main_data && rs->polygons) {
        const game_quirks_t *q = &g_active_profile->quirks;
        /* The geo_displaylist path emits geometry already in camera space (the
         * homebrew's own view() did the camera transform; the GEO projects with
         * focal). Homebrew renders SOLID: the tube walls are filled quads
         * (geo_obj_quad) and even its "lines" are thin filled quads — so draw
         * fills, not pure wireframe, force flat per-object colour (model 456's
         * ROM material is a meaningless placeholder), and render double-sided:
         * the homebrew does its own software backface cull. */
        geo3d->lines_only = false;
        if (q->geo_displaylist) {
            g_geo_wireframe  = 0;
            g_backface_cull  = 0;
        }
        /* The homebrew's display-list object order changes every frame (stars
         * move, enemies spawn/die), so captured_prev[i] is a DIFFERENT object —
         * sub-frame interpolation would smear the geometry. Disable it (lerp=1). */
        float dl_lerp = q->geo_displaylist ? 1.0f : lerp_t;
        /* Faces take their colour from palette RAM, as the rasterizer does. */
        g_geo3d_palram      = bus->palette;
        g_geo3d_palram_size = PALETTE_SIZE;
        const geo3d_models_t md = geo3d_models_of(rs, q);
        game_render_draw_captured_models(geo3d, &md, ox, oy, w, h, dl_lerp);
        g_geo3d_palram = NULL;
    }
    int64_t t3 = emu_now_us();
    if (g_game_frame_gl_counts) {
        sg_frame_stats gl1 = sg_query_stats().cur_frame;
        game_frame_times_t *ft = &g_game_frame_times;
        ft->draw3d_draws      += gl1.num_draw - gl0.num_draw;
        ft->draw3d_pipelines  += gl1.num_apply_pipeline - gl0.num_apply_pipeline;
        ft->draw3d_bindings   += gl1.num_apply_bindings - gl0.num_apply_bindings;
        ft->draw3d_uniforms   += gl1.num_apply_uniforms - gl0.num_apply_uniforms;
        ft->draw3d_buf_writes += (gl1.num_update_buffer + gl1.num_append_buffer)
                               - (gl0.num_update_buffer + gl0.num_append_buffer);
        ft->draw3d_buf_bytes  += (gl1.size_update_buffer + gl1.size_append_buffer)
                               - (gl0.size_update_buffer + gl0.size_append_buffer);
    }
    hprof_enter(HPROF_TILES);
    game_rect_t fg = game_render_tile_rect(vp, g_game_wide.stretch_high);
    if (stereo_eye()) {
        /* the HUD a little in front of the screen, kept inside this eye's
         * picture: the 3D batches left their own scissor (stereo.h) */
        fg.x -= (int)lroundf(stereo_infinity_ndc() * STEREO_HUD * 0.5f * (float)vp.w);
        sg_apply_scissor_rect(vp.x, vp.y, vp.w, vp.h, true);
    }
    if (video->gpu) game_render_draw_indexed(video->fg_view, video->pal_rgba_view, false, fg.x, fg.y, fg.w, fg.h);
    else            game_render_draw_game(video->fg_view, fg.x, fg.y, fg.w, fg.h);
    g_game_frame_times.draw3d_us += t3 - t2;
    g_game_frame_times.tiles_us  += (t2 - t1) + (emu_now_us() - t3);
    g_game_frame_times.frames++;
    hprof_leave(zone);
    emu_twin_ui_leave(twin);
}

/* Where an eye's picture goes in the screen area `area` with 3D on (stereo.h):
 * half 0 (left) or 1, letterboxed in its half at the Size setting and centred
 * Spacing / 2 from the middle, never past the middle or the edge. */
static inline game_rect_t game_frame_stereo_rect(game_rect_t area, int h, int pic_w) {
    const int half = area.w / 2;
    game_rect_t r = game_render_letterbox((int)((float)half * g_stereo.size), area.h, pic_w, VIDEO_HEIGHT);
    float cx = (float)area.w * 0.5f * (1.0f + (h ? 1.0f : -1.0f) * g_stereo.spacing);
    r.x = (int)lroundf(cx - (float)r.w * 0.5f);
    const int lo = h ? half : 0, hi = h ? area.w : half;
    if (r.x < lo) r.x = lo;
    if (r.x + r.w > hi) r.x = hi - r.w;
    r.x += area.x;
    r.y = area.y + (area.h - r.h) / 2;
    return r;
}

/* Both eyes side by side over the screen area `area` (stereo.h): the frame drawn
 * once per eye, into game_frame_stereo_rect; pic[] gets the two rectangles,
 * left half first, for what goes over them (the menus). The board is not
 * touched: the second draw decodes the same display list again. */
static inline void game_frame_draw_stereo(video_state_t *video, geo3d_state_t *geo3d,
                                          memory_bus_t *bus, const romset_t *rs,
                                          game_rect_t area, int pic_w, float lerp_t,
                                          game_rect_t pic[2]) {
    for (int h = 0; h < 2; h++) {
        pic[h] = game_frame_stereo_rect(area, h, pic_w);
        g_stereo.eye = stereo_eye_of_half(h ? 1 : -1);
        game_frame_draw(video, geo3d, bus, rs, pic[h], lerp_t);
    }
    g_stereo.eye = 0;
    sg_apply_scissor_rect(area.x, area.y, area.w, area.h, true);
}

#endif /* GAME_FRAME_H */
