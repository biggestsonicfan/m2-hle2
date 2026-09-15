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

#include <stdbool.h>

#include "sokol_gfx.h"

#include "constants.h"
#include "cop.h"
#include "emu_thread.h"
#include "game_profile.h"
#include "game_render.h"
#include "geo3d.h"
#include "memory.h"
#include "rom_loader.h"
#include "video_window.h"

/* Host time spent in each stage, accumulated in microseconds. A frontend can
 * read and reset these to report where a frame goes; nothing else uses them. */
typedef struct {
    int64_t compose_us;   /* tile layers → RGBA textures */
    int64_t scan_us;      /* this frame's 3D output → model list */
    int64_t upload_us;    /* texture atlas + luma/colorxlat LUTs */
    int64_t draw3d_us;    /* model decode + fill/line submission */
    int64_t tiles_us;     /* the three tile-layer quads */
} game_frame_times_t;

static game_frame_times_t g_game_frame_times;

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

/* Compose the tile layers and scan this frame's slice of the 3D output. Call
 * once per host frame, before the pass. read_camera: drive the 3D camera from
 * the game's camera struct (only meaningful once the emu thread runs). */
static inline void game_frame_prepare(video_state_t *video, geo3d_state_t *geo3d,
                                      memory_bus_t *bus, const romset_t *rs,
                                      bool read_camera) {
    int64_t t0 = emu_now_us();
    video_update(video, bus);
    int64_t t1 = emu_now_us();
    g_game_frame_times.compose_us += t1 - t0;

    if (read_camera && geo3d->use_game_view && g_active_profile &&
            g_active_profile->quirks.camera_struct_addr) {
        geo3d_read_game_view(geo3d, bus,
                             g_active_profile->quirks.camera_struct_addr,
                             g_active_profile->quirks.camera_angle_addr);
    }

    if (geo3d->enabled && g_active_profile && rs->main_data && rs->polygons) {
        const game_quirks_t *q = &g_active_profile->quirks;
        if (q->geo_displaylist) {
            /* Authentic hardware path: decode the GEO display list the i960 built
             * in bufferram. Scan the snapshot captured at geo_flush (g_geodl_snap),
             * not live bufferram — the SHARC HLE aliases bufferram and clobbers the
             * list between flushes. Every m2-snake homebrew uses read_start 0x10000. */
            if (g_geodl_snap_ready)
                geo3d_scan_displaylist(geo3d,
                                       g_geodl_snap, BUFF_RAM_SIZE / 4, 0x10000,
                                       rs->main_data, rs->main_data_size,
                                       q->model_table_offset, q->model_table_count,
                                       bus->palette, PALETTE_SIZE);
        } else if (!(g_geo_use_list && g_geodl_snap_ready &&
                     geo3d_scan_geo_list(geo3d, g_geodl_snap, BUFF_RAM_SIZE / 4,
                                         g_geodl_snap_rstart,
                                         (int16_t)mem_read16(bus, H_SYNC_BASE),
                                         (int16_t)mem_read16(bus, V_SYNC_BASE),
                                         rs->main_data, rs->main_data_size,
                                         q->model_table_offset, q->model_table_count))) {
            /* No display list yet (or it did not reach END): rebuild the frame
             * from the COP command stream the old way. */
            geo3d_scan_captures(geo3d,
                                rs->main_data, rs->main_data_size,
                                rs->polygons_size,
                                q->model_table_offset, q->model_table_count,
                                q->mesh_ptr_subtract, q->mesh_ptr_add);
        }
    } else {
        geo3d_lines_reset();
    }
    g_game_frame_times.scan_us += emu_now_us() - t1;
}

/* Draw the game into the viewport (ox, oy, w, h) of the current pass.
 * Layer order: back colour → background tiles → 3D scene → foreground/HUD. */
static inline void game_frame_draw(video_state_t *video, geo3d_state_t *geo3d,
                                   memory_bus_t *bus, const romset_t *rs,
                                   int ox, int oy, int w, int h, float lerp_t) {
    int64_t t0 = emu_now_us();
    /* Decode both 4-bit luma texture banks → GPU atlas for textured fills, and
     * upload the luma/colorxlat tables — each only when a write changed its RAM
     * since the last upload (sampled before uploading, so a write that lands
     * meanwhile uploads again next frame). The atlas is 4 MB: re-sending it
     * every frame was the largest single cost on a handheld GPU. */
    static bool     s_atlas_valid = false, s_luts_valid = false;
    static uint32_t s_atlas_gen, s_luts_gen;
    uint32_t tex_gen = bus->gen_tex, lut_gen = bus->gen_lut;
    if (!s_atlas_valid || tex_gen != s_atlas_gen) {
        game_render_upload_atlas(bus->texram0, bus->texram1, TEXRAM0_SIZE,
                                 bus->tex_dirty[0], bus->tex_dirty[1]);
        s_atlas_valid = true;
        s_atlas_gen   = tex_gen;
    }
    if (!s_luts_valid || lut_gen != s_luts_gen) {
        game_render_upload_luts(bus->luma, bus->colorxlat);
        s_luts_valid = true;
        s_luts_gen   = lut_gen;
    }
    int64_t t1 = emu_now_us();
    g_game_frame_times.upload_us += t1 - t0;

    /* GPU-composed layers hold pens; the colours come from the pen texture. The
     * back layer there is opaque everywhere (the backdrop is its pen 0), so the
     * back colour quad under it would be drawn over whole: it is left out, and
     * the layer drawn without blending (alpha 1 blends to the source exactly). */
    if (video->gpu) {
        game_render_draw_indexed(video->bg_view, video->pal_rgba_view, true, ox, oy, w, h);
    } else {
        game_render_draw_game(video->back_view, ox, oy, w, h);
        game_render_draw_game(video->bg_view, ox, oy, w, h);
    }
    int64_t t2 = emu_now_us();
    if (geo3d->enabled && g_active_profile && rs->main_data && rs->polygons) {
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
            g_geo_flat_color = 1;
            g_backface_cull  = 0;
        }
        /* The homebrew's display-list object order changes every frame (stars
         * move, enemies spawn/die), so captured_prev[i] is a DIFFERENT object —
         * sub-frame interpolation would smear the geometry. Disable it (lerp=1). */
        float dl_lerp = q->geo_displaylist ? 1.0f : lerp_t;
        /* Faces take their colour from palette RAM, as the rasterizer does. */
        g_geo3d_palram      = bus->palette;
        g_geo3d_palram_size = PALETTE_SIZE;
        game_render_draw_captured_models(geo3d,
                                         rs->main_data, rs->main_data_size,
                                         rs->polygons,  rs->polygons_size,
                                         rs->textures,  rs->textures_size,
                                         q->model_table_offset, q->model_table_count,
                                         q->mesh_ptr_subtract, q->mesh_ptr_add,
                                         ox, oy, w, h,
                                         geo3d->cam_x, geo3d->cam_y, geo3d->cam_z,
                                         geo3d->rot_y, geo3d->rot_x, geo3d->fov_deg,
                                         dl_lerp);
        g_geo3d_palram = NULL;
    }
    int64_t t3 = emu_now_us();
    if (video->gpu) game_render_draw_indexed(video->fg_view, video->pal_rgba_view, false, ox, oy, w, h);
    else            game_render_draw_game(video->fg_view, ox, oy, w, h);
    g_game_frame_times.draw3d_us += t3 - t2;
    g_game_frame_times.tiles_us  += (t2 - t1) + (emu_now_us() - t3);
}

#endif /* GAME_FRAME_H */
