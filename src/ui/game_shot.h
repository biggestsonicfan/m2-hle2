/*
 * game_shot.h — a PNG of the game's own picture, taken on request from the
 * MCP bridge's thread and served on the render thread.
 *
 * The picture is what the A/V tap sends (ui/av_capture.h): game_frame_draw
 * into an offscreen target, the board's 496x384 by default, with no ImGui, no
 * menu bar, no picture filter and no letterbox. It is the frame the renderer
 * last prepared, so it works with the board stopped (emu_stop) too: a stopped
 * board keeps its last frame and every host frame draws it again.
 *
 * Before this the only way to see a frame from a script was to run the window
 * on an Xvfb of its own and grab it with ffmpeg (m2-megamix, #1545), and the
 * only shot over the bridge was objview_shot's single model.
 *
 * One request at a time: the bridge arms it, the render thread's pass
 * (game_shot_service) takes it, and the bridge waits for `done`. A run with no
 * renderer (--headless without --av-port) has nothing to serve it, and
 * game_shot_renderer says so before anyone waits.
 */
#ifndef GAME_SHOT_H
#define GAME_SHOT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sokol_gfx.h"
#include "gfx_readback.h"
#include "constants.h"   /* VIDEO_WIDTH, VIDEO_HEIGHT */

#define GAME_SHOT_PATH_MAX 512
#define GAME_SHOT_MAX_DIM  4096

typedef struct {
    volatile int pending;          /* armed by the bridge, cleared by the service */
    volatile int done;             /* set by the service when the file is written (or not) */
    int      ok;
    char     path[GAME_SHOT_PATH_MAX];
    int      w, h;
    unsigned frame;                /* g_emu_frames when it was drawn */
    size_t   bytes;
    char     err[192];
} game_shot_req_t;

static struct {
    game_shot_req_t req;
    volatile int    renderer;      /* a render loop calls game_shot_service */
    sg_image        color_img, depth_img;
    sg_view         color_att, depth_att;
    int             rt_w, rt_h;
} g_game_shot;

/* The render loop says it serves shots. Call once its graphics are up. */
static inline void game_shot_set_renderer(bool on) { g_game_shot.renderer = on ? 1 : 0; }
static inline bool game_shot_renderer(void) { return g_game_shot.renderer != 0; }
static inline bool game_shot_pending(void) { return g_game_shot.req.pending != 0; }

/* Arm a shot of w x h (0 = the board's size) to `path`. False, with err, when
 * one is already waiting or the arguments are bad. */
static inline bool game_shot_arm(const char *path, int w, int h, char *err, int errcap) {
    game_shot_req_t *rq = &g_game_shot.req;
    if (rq->pending) { snprintf(err, (size_t)errcap, "a screenshot is already waiting"); return false; }
    if (!path || !path[0]) { snprintf(err, (size_t)errcap, "missing path"); return false; }
    if (strlen(path) >= sizeof rq->path) { snprintf(err, (size_t)errcap, "path too long"); return false; }
    if (w <= 0) w = VIDEO_WIDTH;
    if (h <= 0) h = VIDEO_HEIGHT;
    if (w > GAME_SHOT_MAX_DIM || h > GAME_SHOT_MAX_DIM) {
        snprintf(err, (size_t)errcap, "size %dx%d: at most %d a side", w, h, GAME_SHOT_MAX_DIM);
        return false;
    }
    strcpy(rq->path, path);
    rq->w = w; rq->h = h;
    rq->ok = 0; rq->frame = 0; rq->bytes = 0; rq->err[0] = '\0';
    rq->done = 0;
    rq->pending = 1;
    return true;
}

static inline void game_shot__discard_target(void) {
    if (g_game_shot.color_att.id) sg_destroy_view(g_game_shot.color_att);
    if (g_game_shot.depth_att.id) sg_destroy_view(g_game_shot.depth_att);
    if (g_game_shot.color_img.id) sg_destroy_image(g_game_shot.color_img);
    if (g_game_shot.depth_img.id) sg_destroy_image(g_game_shot.depth_img);
    g_game_shot.color_att.id = g_game_shot.depth_att.id = 0;
    g_game_shot.color_img.id = g_game_shot.depth_img.id = 0;
    g_game_shot.rt_w = g_game_shot.rt_h = 0;
}

/* A target at w x h in the formats every game pipeline was built against
 * (sg_setup's defaults), single-sampled so it reads back. Kept between shots. */
static inline bool game_shot__ensure_target(int w, int h) {
    if (g_game_shot.color_img.id && g_game_shot.rt_w == w && g_game_shot.rt_h == h) return true;
    game_shot__discard_target();
    sg_desc d = sg_query_desc();
    sg_pixel_format cfmt = d.environment.defaults.color_format;
    sg_pixel_format dfmt = d.environment.defaults.depth_format;
    if (cfmt == SG_PIXELFORMAT_NONE) cfmt = SG_PIXELFORMAT_RGBA8;
    if (dfmt == SG_PIXELFORMAT_NONE) dfmt = SG_PIXELFORMAT_DEPTH_STENCIL;
    g_game_shot.color_img = sg_make_image(&(sg_image_desc){
        .usage.color_attachment = true, .width = w, .height = h,
        .pixel_format = cfmt, .sample_count = 1, .label = "game-shot-color" });
    g_game_shot.depth_img = sg_make_image(&(sg_image_desc){
        .usage.depth_stencil_attachment = true, .width = w, .height = h,
        .pixel_format = dfmt, .sample_count = 1, .label = "game-shot-depth" });
    g_game_shot.color_att = sg_make_view(&(sg_view_desc){
        .color_attachment.image = g_game_shot.color_img, .label = "game-shot-color-att" });
    g_game_shot.depth_att = sg_make_view(&(sg_view_desc){
        .depth_stencil_attachment.image = g_game_shot.depth_img, .label = "game-shot-depth-att" });
    if (sg_query_image_state(g_game_shot.color_img) != SG_RESOURCESTATE_VALID ||
        sg_query_image_state(g_game_shot.depth_img) != SG_RESOURCESTATE_VALID ||
        sg_query_view_state(g_game_shot.color_att) != SG_RESOURCESTATE_VALID ||
        sg_query_view_state(g_game_shot.depth_att) != SG_RESOURCESTATE_VALID) {
        game_shot__discard_target();
        return false;
    }
    g_game_shot.rt_w = w;
    g_game_shot.rt_h = h;
    return true;
}

static inline void game_shot__finish(game_shot_req_t *rq, bool ok) {
    rq->ok = ok ? 1 : 0;
    rq->pending = 0;
    rq->done = 1;
}

/*
 * Serve a waiting shot: draw(w, h) puts the game into the current pass's
 * viewport (0, 0, w, h), as av_capture's pass does. Call it outside any pass,
 * after the frame's game_frame_prepare, before sg_commit.
 */
static inline void game_shot_service(void (*draw)(int w, int h), unsigned frame) {
    game_shot_req_t *rq = &g_game_shot.req;
    if (!rq->pending) return;
    if (!gfx_readback_supported()) {
        snprintf(rq->err, sizeof rq->err, "%s", "no readback path for this graphics backend");
        game_shot__finish(rq, false);
        return;
    }
    const int w = rq->w, h = rq->h;
    if (!game_shot__ensure_target(w, h)) {
        snprintf(rq->err, sizeof rq->err, "could not make a %dx%d target", w, h);
        game_shot__finish(rq, false);
        return;
    }
    sg_begin_pass(&(sg_pass){
        .action = { .colors[0] = { .load_action = SG_LOADACTION_CLEAR, .clear_value = { 0, 0, 0, 1 } },
                    .depth = { .load_action = SG_LOADACTION_CLEAR, .clear_value = 1.0f } },
        .attachments = { .colors[0] = g_game_shot.color_att, .depth_stencil = g_game_shot.depth_att },
    });
    draw(w, h);
    sg_end_pass();

    uint8_t *px = malloc((size_t)w * (size_t)h * 4);
    if (!px) {
        snprintf(rq->err, sizeof rq->err, "out of memory for %dx%d", w, h);
        game_shot__finish(rq, false);
        return;
    }
    bool ok = gfx_readback_rgba8(g_game_shot.color_img, w, h, px);
    if (!ok) snprintf(rq->err, sizeof rq->err, "readback failed: %s", gfx_readback_error());
    size_t len = 0;
    void *png = ok ? gfx_encode_png_rows(px, w, h, &len) : NULL;   /* top row first, as read */
    free(px);
    if (ok && !png) { ok = false; snprintf(rq->err, sizeof rq->err, "%s", gfx_readback_error()); }
    if (ok) {
        rq->bytes = gfx_save_png(rq->path, png, len);
        if (!rq->bytes) { ok = false; snprintf(rq->err, sizeof rq->err, "%s", gfx_readback_error()); }
    }
    gfx_free_png(png);
    rq->frame = frame;
    game_shot__finish(rq, ok);
}

static inline void game_shot_shutdown(void) { game_shot__discard_target(); }

#endif /* GAME_SHOT_H */
