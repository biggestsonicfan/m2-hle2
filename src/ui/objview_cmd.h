/*
 * objview_cmd.h — the object viewer's command vocabulary, without a transport.
 *
 * JSON in, JSON out, and nothing about how either got here. Two transports
 * carry it:
 *
 *   ui/mcp_bridge.h   the desktop's TCP bridge. It has a thread of its own, so
 *                     it blocks on the render thread and answers once.
 *   main_web.c        the browser build, which is single-threaded: a request is
 *                     armed and the caller comes back for the answer after the
 *                     frame callback has served it.
 *
 * That difference is the whole reason this file exists. Everything either side
 * would otherwise have copied — the field names, the defaults, the reply shape
 * — is here once, so a command written against the desktop bridge works in the
 * browser and the guide describes one thing rather than two.
 *
 * The waiting is NOT here. Each transport owns that, because each has a
 * different notion of how to wait.
 */
#ifndef OBJVIEW_CMD_H
#define OBJVIEW_CMD_H

#include <stdio.h>
#include <string.h>

#include "game_profile.h"
#include "json_min.h"
#include "memory.h"
#include "objview.h"
#include "rom_loader.h"

/* ---- Reply ---------------------------------------------------------------- */

/* The viewer's whole state, the readiness probe and the last decode's result,
 * as JSON fields (no braces) for the caller to wrap. Every command answers with
 * these, so a caller never has to ask a second time what it just did. */
static inline int objview_cmd_state(const romset_t *rs, memory_bus_t *bus, char *buf, int cap) {
    const objview_t *v = &g_objview.v;
    objview_ready_t rdy;
    objview_probe(rs, bus, &rdy);
    char esc[256];
    json_escape(esc, sizeof(esc), g_objview.last_err);
    return snprintf(buf, (size_t)cap,
        "\"ready\":%s,\"rom_loaded\":%s,\"profile\":%s,\"frames\":%u,"
        "\"tex_pct\":%d,\"pal_pct\":%d,\"models\":%u,\"captures\":%d,\"saw_3d\":%s,"
        "\"gpu_readback\":%s,\"window\":%s,"
        "\"active\":%s,\"source\":%d,\"model\":%d,\"capture\":%d,\"use_capture_matrix\":%s,"
        "\"pos\":[%.4f,%.4f,%.4f],\"rot\":[%.3f,%.3f,%.3f],\"scale\":%.4f,"
        "\"yaw\":%.3f,\"pitch\":%.3f,\"dist\":%.4f,\"fov\":%.2f,"
        "\"autofit\":%s,\"fit_margin\":%.3f,"
        "\"target\":[%.4f,%.4f,%.4f],\"eye\":[%.4f,%.4f,%.4f],"
        "\"width\":%d,\"height\":%d,\"bg\":[%.3f,%.3f,%.3f],"
        "\"wireframe\":%s,\"textured\":%s,\"cull\":%d,"
        "\"drawn_model\":%d,\"tris\":%d,\"lines\":%d,"
        "\"bmin\":[%.4f,%.4f,%.4f],\"bmax\":[%.4f,%.4f,%.4f],"
        "\"serial\":%u,\"last_ok\":%s,\"last_error\":\"%s\"",
        rdy.ready ? "true" : "false",
        rdy.rom_loaded ? "true" : "false",
        rdy.profile ? "true" : "false",
        rdy.frames, rdy.tex_pct, rdy.pal_pct, rdy.models, rdy.captures,
        rdy.saw_3d ? "true" : "false",
        gfx_readback_supported() ? "true" : "false",
        g_objview.preview_open ? "true" : "false",
        v->active ? "true" : "false",
        v->source, v->model, v->capture,
        v->use_capture_matrix ? "true" : "false",
        v->pos[0], v->pos[1], v->pos[2],
        v->rot[0], v->rot[1], v->rot[2], v->scale,
        v->yaw, v->pitch, v->dist, v->fov,
        v->autofit ? "true" : "false", v->fit_margin,
        v->target[0], v->target[1], v->target[2],
        v->cam[0], v->cam[1], v->cam[2],
        v->width, v->height, v->bg[0], v->bg[1], v->bg[2],
        v->wireframe ? "true" : "false",
        v->textured ? "true" : "false",
        v->cull,
        v->drawn_model, v->tris, v->lines,
        v->bmin[0], v->bmin[1], v->bmin[2],
        v->bmax[0], v->bmax[1], v->bmax[2],
        g_objview.serial,
        g_objview.last_ok ? "true" : "false", esc);
}

static inline void objview_cmd_reply(const romset_t *rs, memory_bus_t *bus,
                                     char *resp, int cap, int ok, const char *err) {
    char *p = resp;
    int   left = cap, n;
    if (ok) {
        n = snprintf(p, (size_t)left, "{\"ok\":true,");
    } else {
        char esc[256];
        json_escape(esc, sizeof(esc), err ? err : "");
        n = snprintf(p, (size_t)left, "{\"ok\":false,\"error\":\"%s\",", esc);
    }
    p += n; left -= n;
    n = objview_cmd_state(rs, bus, p, left);
    p += n; left -= n;
    snprintf(p, (size_t)left, "}");
}

/* ---- Settings ------------------------------------------------------------- */

/* Apply whatever settings this request carries. Every field is optional; an
 * omitted one keeps its value, so a caller can nudge one angle at a time. */
static inline void objview_cmd_apply(const char *req) {
    objview_t *v = &g_objview.v;
    int   i;
    float f;
    char  s[64];

    if (json_get_int(req, "active", &i)) v->active = i ? 1 : 0;
    /* Show the viewer's window, so whoever is at the machine sees the object
     * being inspected. The frontend applies this on its next frame, and a build
     * with no window (the browser) simply leaves the request standing. */
    if (json_get_int(req, "window", &i)) g_objview.window_request = i ? 1 : 0;
    if (json_get_int(req, "model",   &i)) { v->model   = i < 0 ? 0 : i; v->source = 0; }
    if (json_get_int(req, "capture", &i)) { v->capture = i < 0 ? 0 : i; v->source = 1; }
    /* An explicit source wins over the one implied above. */
    if (json_get_str(req, "source", s, sizeof s))
        v->source = (!strcmp(s, "capture") || !strcmp(s, "1")) ? 1 : 0;
    if (json_get_int(req, "use_capture_matrix", &i)) v->use_capture_matrix = i ? 1 : 0;

    if (json_get_f32(req, "pos_x", &f)) v->pos[0] = f;
    if (json_get_f32(req, "pos_y", &f)) v->pos[1] = f;
    if (json_get_f32(req, "pos_z", &f)) v->pos[2] = f;
    if (json_get_f32(req, "rot_x", &f)) v->rot[0] = f;
    if (json_get_f32(req, "rot_y", &f)) v->rot[1] = f;
    if (json_get_f32(req, "rot_z", &f)) v->rot[2] = f;
    if (json_get_f32(req, "scale", &f)) v->scale  = f;

    if (json_get_f32(req, "yaw",   &f)) v->yaw   = f;
    if (json_get_f32(req, "pitch", &f)) v->pitch = f;
    if (json_get_f32(req, "fov",   &f)) v->fov   = f;
    /* A distance or a target named by hand is a request to stop auto-framing:
     * leaving autofit on would overwrite it on the very next pass, which reads
     * as the setting having been ignored. */
    if (json_get_f32(req, "dist",     &f)) { v->dist      = f; v->autofit = 0; }
    if (json_get_f32(req, "target_x", &f)) { v->target[0] = f; v->autofit = 0; }
    if (json_get_f32(req, "target_y", &f)) { v->target[1] = f; v->autofit = 0; }
    if (json_get_f32(req, "target_z", &f)) { v->target[2] = f; v->autofit = 0; }
    if (json_get_int(req, "autofit",    &i)) v->autofit    = i ? 1 : 0;
    if (json_get_f32(req, "fit_margin", &f)) v->fit_margin = f;

    if (json_get_int(req, "width",  &i)) v->width  = i;
    if (json_get_int(req, "height", &i)) v->height = i;
    if (json_get_f32(req, "bg_r", &f)) v->bg[0] = f;
    if (json_get_f32(req, "bg_g", &f)) v->bg[1] = f;
    if (json_get_f32(req, "bg_b", &f)) v->bg[2] = f;
    if (json_get_int(req, "wireframe", &i)) v->wireframe = i ? 1 : 0;
    if (json_get_int(req, "textured",  &i)) v->textured  = i ? 1 : 0;
    if (json_get_int(req, "cull",      &i)) v->cull = (i == 1 || i == 2) ? i : 0;
    if (json_get_int(req, "layers",    &i)) v->layers = i ? 1 : 0;
}

/* ---- Shots ---------------------------------------------------------------- */

/*
 * Fill in the shot request and hand it to the render side. Returns 0 with a
 * reason if it cannot be armed at all.
 *
 * Angles come from one of three spellings, checked in this order:
 *   "six":1                    the six camera stations: +Z, +X, -Z, -X, +Y, -Y
 *   "count":N, "yaw_step":D    a turntable from yaw0 in steps of D (D defaults
 *                              to a full turn spread over N, which is what
 *                              "several angles" almost always means)
 *   neither                    one shot at the viewer's current yaw and pitch
 *
 * "path" names the file for a single shot; for several it is the stem and the
 * shots land at <stem>-000.png and so on. It may be left out entirely, and in
 * the browser build it always is: with no path nothing is written and each
 * shot's PNG stays in memory for the caller to collect. Any setting
 * objview_cmd_apply takes may be passed here too, so one call can select the
 * object, place it and shoot it.
 */
static inline int objview_cmd_arm_shot(const char *req, char *err, size_t errcap) {
    objview_req_t *rq = &g_objview.req;

    if (!gfx_readback_supported()) {
        snprintf(err, errcap, "this build cannot read the GPU back (Metal / WebGPU)");
        return 0;
    }
    if (rq->pending) { snprintf(err, errcap, "a shot batch is already in flight"); return 0; }

    char path[OBJVIEW_PATH_MAX];
    path[0] = '\0';
    json_get_str(req, "path", path, sizeof path);

    objview_cmd_apply(req);

    int   count = 1, six = 0, keep = 0;
    float yaw0   = g_objview.v.yaw,   yaw_step   = 0.0f;
    float pitch0 = g_objview.v.pitch, pitch_step = 0.0f;
    json_get_int(req, "six",   &six);
    json_get_int(req, "count", &count);
    json_get_int(req, "keep",  &keep);
    json_get_f32(req, "yaw0",       &yaw0);
    json_get_f32(req, "pitch0",     &pitch0);
    json_get_f32(req, "yaw_step",   &yaw_step);
    json_get_f32(req, "pitch_step", &pitch_step);
    if (count < 1) count = 1;
    if (count > OBJVIEW_MAX_SHOTS) count = OBJVIEW_MAX_SHOTS;
    if (count > 1 && yaw_step == 0.0f && pitch_step == 0.0f) yaw_step = 360.0f / (float)count;

    snprintf(rq->path, sizeof rq->path, "%s", path);
    /* With nowhere to write, the encoded PNG is the only copy: keep it. */
    rq->keep       = keep || path[0] == '\0';
    rq->count      = count;
    rq->six        = six ? 1 : 0;
    rq->yaw0       = yaw0;
    rq->yaw_step   = yaw_step;
    rq->pitch0     = pitch0;
    rq->pitch_step = pitch_step;
    rq->shots      = 0;
    rq->ok         = 0;
    rq->err[0]     = '\0';
    rq->done       = 0;
    rq->pending    = 1;      /* last: the render side reads this to start */
    return 1;
}

/*
 * The finished batch, once rq->done. Each shot reports its coverage (the
 * fraction of the image that is not the background) and the pixel box the
 * object drew into — enough to tell an off-screen or hair-thin result from a
 * good frame without opening the image.
 */
static inline void objview_cmd_shot_reply(const romset_t *rs, memory_bus_t *bus,
                                          char *resp, int cap, unsigned elapsed_ms) {
    const objview_req_t *rq = &g_objview.req;
    if (!rq->ok) { objview_cmd_reply(rs, bus, resp, cap, 0, rq->err); return; }

    char *p = resp;
    int   left = cap, n;
    n = snprintf(p, (size_t)left, "{\"ok\":true,\"shots\":[");
    p += n; left -= n;
    for (int i = 0; i < rq->shots && left > 384; i++) {
        const objview_shot_t *s = &rq->shot[i];
        char esc[OBJVIEW_PATH_MAX * 2];
        json_escape(esc, sizeof(esc), s->path);
        n = snprintf(p, (size_t)left,
                     "%s{\"index\":%d,\"path\":\"%s\",\"yaw\":%.2f,\"pitch\":%.2f,"
                     "\"coverage\":%.5f,\"box\":[%d,%d,%d,%d],\"bytes\":%u}",
                     i ? "," : "", i, esc, s->yaw, s->pitch, s->coverage,
                     s->px0, s->py0, s->px1, s->py1, (unsigned)s->bytes);
        p += n; left -= n;
    }
    n = snprintf(p, (size_t)left, "],\"count\":%d,\"elapsed_ms\":%u,", rq->shots, elapsed_ms);
    p += n; left -= n;
    n = objview_cmd_state(rs, bus, p, left);
    p += n; left -= n;
    snprintf(p, (size_t)left, "}");
}

/* ---- The model table ------------------------------------------------------ */

/*
 * How much geometry each model-table entry holds, over a range.
 *
 * Most of the table is empty in any given game, and an empty entry looks
 * exactly like a broken one from a screenshot. This decodes without drawing, so
 * a caller can find the models worth looking at before it starts looking.
 */
static inline void objview_cmd_list(const char *req, const romset_t *rs, char *resp, int cap) {
    uint32_t first = 0, count = 64;
    int      nonempty_only = 1;
    json_get_u32(req, "first", &first);
    json_get_u32(req, "count", &count);
    json_get_int(req, "nonempty_only", &nonempty_only);

    if (!rs || !rs->loaded || !rs->main_data || !rs->polygons) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"no ROM loaded yet\"}");
        return;
    }
    if (!g_active_profile) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"no game profile\"}");
        return;
    }
    const game_quirks_t *q = &g_active_profile->quirks;
    if (first >= q->model_table_count) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"model %u is past the table's %u\"}",
                 first, q->model_table_count);
        return;
    }
    if (count > q->model_table_count - first) count = q->model_table_count - first;
    if (count > 4096) count = 4096;
    if (g_geo3d_dump_busy) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"another model decode is in flight\"}");
        return;
    }

    /* A private sink and the dump flag, for the reason the bridge's model dump
     * has them: nothing else must find the buffer it is drawing from emptied. */
    static geo3d_tri_buf_t list_buf;
    geo3d_tri_buf_t *saved = g_geo3d_tri_sink;
    g_geo3d_dump_busy = 1;
    g_geo3d_tri_sink  = &list_buf;

    char *p = resp;
    int   left = cap, n;
    n = snprintf(p, (size_t)left, "{\"ok\":true,\"first\":%u,\"count\":%u,\"models\":[",
                 first, count);
    p += n; left -= n;

    uint32_t nonempty = 0, emitted = 0;
    for (uint32_t m = first; m < first + count; m++) {
        list_buf.count = 0;
        geo3d_decode_model((int)m,
                           rs->main_data, rs->main_data_size,
                           rs->polygons,  rs->polygons_size,
                           rs->textures,  rs->textures_size,
                           q->model_table_offset, q->model_table_count,
                           q->mesh_ptr_subtract, q->mesh_ptr_add,
                           NULL, 1.0f, 1.0f, 1.0f);
        int tris = list_buf.count;
        if (tris > 0) nonempty++;
        if (nonempty_only && tris == 0) continue;
        if (left < 128) continue;            /* keep the JSON well-formed */
        n = snprintf(p, (size_t)left, "%s{\"model\":%u,\"tris\":%d}", emitted ? "," : "", m, tris);
        p += n; left -= n;
        emitted++;
    }

    g_geo3d_tri_sink  = saved;
    g_geo3d_dump_busy = 0;

    snprintf(p, (size_t)left, "],\"listed\":%u,\"nonempty\":%u,\"table_count\":%u}",
             emitted, nonempty, q->model_table_count);
}

#endif /* OBJVIEW_CMD_H */
