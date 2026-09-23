/*
 * objview.h — the debug object viewer: one model, on its own, drawn offscreen
 * from a camera the caller places, and read back as a PNG.
 *
 * The 3D window's single-model browser already decodes any model-table entry,
 * but it draws into the game's frame, behind the game's camera and under the
 * game's tiles. Chasing a visual artifact there means steering the emulator
 * into the scene that draws the object and then fighting the game for the
 * camera. This draws the model by itself, against a flat background, from
 * wherever the caller asks — and takes several angles inside one host frame,
 * so a sweep around an object is one call rather than a dozen round trips.
 *
 * It is driven from the MCP bridge (mcp_bridge.h, the objview_* commands) and
 * from objview_window.h, which shows the same target live. Everything here
 * runs on the RENDER thread: objview_service() is called once per host frame,
 * between passes, and a bridge request is a small struct the bridge thread
 * fills in and then waits on.
 *
 * What it draws is the emulator's own decoder and the emulator's own fill
 * shader — the same geo3d_decode_model and the same pipelines the frame uses.
 * An artifact that shows here is an artifact the frame has, not a second
 * renderer's opinion of one.
 *
 * Board-level throughout: nothing here knows which game is loaded. The model
 * table it indexes comes from the active profile's quirks.
 */
#ifndef OBJVIEW_H
#define OBJVIEW_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sokol_gfx.h"

#include "constants.h"
#include "emu_thread.h"
#include "game_profile.h"
#include "game_render.h"
#include "geo3d.h"
#include "gfx_readback.h"
#include "log.h"
#include "memory.h"
#include "rom_loader.h"

/* Bounds on what one request may ask for. The target is recreated when the size
 * changes and a whole batch runs inside a single frame, so both of these are
 * really bounds on how long one host frame is allowed to take. */
#define OBJVIEW_MAX_DIM    2048
#define OBJVIEW_MIN_DIM      32
#define OBJVIEW_MAX_SHOTS    64
#define OBJVIEW_PATH_MAX    320

/* ---- Viewer state -------------------------------------------------------- */

typedef struct {
    /* --- what to draw --- */
    int   active;              /* the viewer renders at all */
    int   source;              /* 0: model-table index; 1: live capture index */
    int   model;               /* model-table index, when source == 0 */
    int   capture;             /* capture index, when source == 1 */
    int   use_capture_matrix;  /* place it with the matrix the board gave it —
                                * then pos/rot/scale below are NOT applied */

    /* --- where the object sits in world space --- */
    float pos[3];
    float rot[3];              /* degrees; R = Rz·Ry·Rx */
    float scale;

    /* --- where the camera sits, as an orbit about `target` --- */
    float target[3];
    float yaw, pitch;          /* degrees */
    float dist;
    float fov;                 /* vertical, degrees */
    int   autofit;             /* re-centre and re-range from the model's bounds */
    float fit_margin;          /* 1.0: the bounding sphere exactly fills the frame */

    /* --- how it is drawn --- */
    int   width, height;
    float bg[4];               /* clear colour, straight alpha */
    int   wireframe;           /* overlay the decoder's edges */
    int   textured;            /* 0: flat face colour, texture ignored */
    int   cull;                /* 0 none, 1 CW front, 2 CCW front */
    int   layers;              /* faces lying on faces drawn over them (geo3d_mesh_layers) */

    /* --- what the last render found (callers read, never write) --- */
    int   have_result;
    int   drawn_model;         /* the model index actually decoded */
    int   tris, lines;
    float bmin[3], bmax[3];    /* world-space bounds of what was decoded */
    float cam[3];              /* the eye the last orbit resolved to */
} objview_t;

/* One shot's angle and what came out of it. The coverage and the pixel box are
 * the cheap way to tell an off-screen or hair-thin object from a good frame
 * without opening the image. */
typedef struct {
    float  yaw, pitch;
    float  coverage;                /* fraction of pixels that are not the background */
    int    px0, py0, px1, py1;      /* box of drawn pixels, or all −1 */
    size_t bytes;
    /* The encoded PNG, when the batch asked to keep it (objview_req_t.keep).
     * The browser build has nowhere to write a file, so there this is the only
     * copy and JavaScript reads it straight out of the heap. Freed when the
     * next batch starts, which is how long a caller has to collect it. */
    void  *png;
    char   path[OBJVIEW_PATH_MAX];  /* empty when nothing was written */
} objview_shot_t;

/* A batch of shots, handed from the bridge thread to the render thread. */
typedef struct {
    volatile int pending;
    volatile int done;
    int   ok;
    char  err[192];

    char  path[OBJVIEW_PATH_MAX];   /* one shot: this file. more: <stem>-NNN.png */
    int   count;
    float yaw0, yaw_step;
    float pitch0, pitch_step;
    int   six;                      /* ignore the sweep: the six camera stations */
    int   keep;                     /* hold each encoded PNG in memory for the caller */

    int            shots;
    objview_shot_t shot[OBJVIEW_MAX_SHOTS];
} objview_req_t;

static struct {
    objview_t     v;
    objview_req_t req;

    /* The offscreen target, recreated when width/height change. */
    int       rt_w, rt_h;
    sg_image  color_img, depth_img;
    sg_view   color_att, depth_att, color_tex;

    /* Its own vertex buffers: sokol allows one sg_update_buffer per buffer per
     * frame, and the frame has already spent the renderer's on the game. */
    sg_buffer fill_vbuf, line_vbuf;
    int       fill_cap, line_cap;        /* in vertices */

    /* Its own triangle sink, so a decode here never lands in the buffer the
     * frame was built in (the same reason mcp_bridge.h's model dump has one). */
    geo3d_tri_buf_t tris;

    int  preview_open;        /* objview_window.h is showing the target */
    /* A caller with no window open that wants one service pass anyway: the
     * bridge sets it so a setting change is decoded and measured before it
     * answers, rather than reporting the previous object's triangle count. */
    volatile int      refresh;
    volatile unsigned serial;      /* bumped at the end of every service pass */
    /* Open or close the viewer's window: 1 open, 0 close, -1 nothing asked.
     * The frontend owns the flag the window is drawn from, so a caller that
     * wants the operator to SEE what it is inspecting leaves a request here
     * and the frame callback applies it. */
    volatile int      window_request;
    int  saw_3d;              /* the board has drawn 3D at least once (see objview_probe) */
    int  last_ok;             /* the last service call produced pixels */
    char last_err[192];
} g_objview;

/* ---- Defaults ------------------------------------------------------------ */

static inline void objview_init(void) {
    memset(&g_objview, 0, sizeof g_objview);
    objview_t *v = &g_objview.v;
    v->source     = 0;
    v->model      = 1;
    v->scale      = 1.0f;
    v->yaw        = 30.0f;
    v->pitch      = 20.0f;
    v->dist       = 10.0f;
    v->fov        = 45.0f;
    v->autofit    = 1;
    v->fit_margin = 1.25f;
    v->width      = 512;
    v->height     = 512;
    v->bg[0] = 0.10f; v->bg[1] = 0.11f; v->bg[2] = 0.13f; v->bg[3] = 1.0f;
    v->textured   = 1;
    v->cull       = 0;
    v->layers     = 1;
    g_objview.window_request = -1;
}

/* ---- Readiness ----------------------------------------------------------- */

/*
 * Whether there is anything worth looking at yet.
 *
 * The model table is ROM and readable the moment a set is loaded, but what the
 * object is *made of* is not: the texture sheets are filled by the game's own
 * decompressor and the face palette by its colour setup, both during boot. Ask
 * for a model before then and it decodes into the right shape with no texels
 * and no colours — which reads as an artifact, and is not one. In STF that
 * lands as attract starts; other games will differ, so this measures the state
 * rather than counting frames.
 */
typedef struct {
    int      rom_loaded, profile;
    unsigned frames;
    int      tex_pct;    /* percent of sampled texture-RAM bytes that are non-zero */
    int      pal_pct;    /* percent of sampled palette bytes that are non-zero */
    int      captures;   /* models the board drew in the LAST frame */
    int      saw_3d;     /* the board has drawn 3D at least once since boot */
    unsigned models;     /* entries in the model table */
    int      ready;
} objview_ready_t;

/* Sample every `stride`th byte and report the percentage that are non-zero. */
static inline int objview__fill_pct(const uint8_t *p, size_t n, size_t stride) {
    if (!p || n == 0 || stride == 0) return 0;
    size_t hits = 0, taken = 0;
    for (size_t i = 0; i < n; i += stride) { taken++; if (p[i]) hits++; }
    return taken ? (int)((hits * 100) / taken) : 0;
}

static inline void objview_probe(const romset_t *rs, memory_bus_t *bus, objview_ready_t *out) {
    memset(out, 0, sizeof *out);
    out->rom_loaded = (rs && rs->loaded && rs->main_data && rs->polygons) ? 1 : 0;
    out->profile    = g_active_profile ? 1 : 0;
    out->frames     = g_emu_frames;
    out->models     = g_active_profile ? g_active_profile->quirks.model_table_count : 0;
    out->captures   = g_geo3d_state ? g_geo3d_state->captured_count : 0;
    if (bus) {
        int t0 = objview__fill_pct(bus->texram0, TEXRAM0_SIZE, 1021);
        int t1 = objview__fill_pct(bus->texram1, TEXRAM1_SIZE, 1021);
        out->tex_pct = t0 > t1 ? t0 : t1;
        out->pal_pct = objview__fill_pct(bus->palette, PALETTE_SIZE, 7);
    }
    /* The decisive one, and the reason this is not a frame count: the board
     * having DRAWN 3D at least once. Texture RAM filling up is gradual -- five
     * frames into an STF boot it is 8% full and the palette 36%, which passed
     * a "non-zero" test and drew a correctly shaped, entirely black model. By
     * the time the board submits its first object every part of that is up.
     *
     * Latched, because captured_count is what the LAST frame drew and a scene
     * change can empty it for a frame: "the engine has started" does not stop
     * being true afterwards. */
    if (out->captures > 0) g_objview.saw_3d = 1;
    out->saw_3d = g_objview.saw_3d;
    out->ready = out->rom_loaded && out->profile && out->frames > 0 &&
                 out->saw_3d && out->tex_pct > 0 && out->pal_pct > 0;
}

/* ---- Geometry ------------------------------------------------------------ */

/* pos/rot/scale → the 3x4 row-major matrix geo3d_decode_model applies. */
static inline void objview__placement(const objview_t *v, float *m) {
    const float d2r = 3.14159265358979f / 180.0f;
    float cx = cosf(v->rot[0]*d2r), sx = sinf(v->rot[0]*d2r);
    float cy = cosf(v->rot[1]*d2r), sy = sinf(v->rot[1]*d2r);
    float cz = cosf(v->rot[2]*d2r), sz = sinf(v->rot[2]*d2r);
    float s  = (v->scale != 0.0f) ? v->scale : 1.0f;
    m[0] = s*(cz*cy);  m[1] = s*(cz*sy*sx - sz*cx);  m[2]  = s*(cz*sy*cx + sz*sx);  m[3]  = v->pos[0];
    m[4] = s*(sz*cy);  m[5] = s*(sz*sy*sx + cz*cx);  m[6]  = s*(sz*sy*cx - cz*sx);  m[7]  = v->pos[1];
    m[8] = s*(-sy);    m[9] = s*(cy*sx);             m[10] = s*(cy*cx);             m[11] = v->pos[2];
}

static inline void objview__grow(float *lo, float *hi, float x, float y, float z) {
    if (x < lo[0]) lo[0] = x;
    if (x > hi[0]) hi[0] = x;
    if (y < lo[1]) lo[1] = y;
    if (y > hi[1]) hi[1] = y;
    if (z < lo[2]) lo[2] = z;
    if (z > hi[2]) hi[2] = z;
}

/*
 * Decode the selected model into the viewer's own triangle sink and measure it.
 * Returns false with a reason the caller can pass on.
 */
static inline bool objview__decode(const romset_t *rs, memory_bus_t *bus,
                                   char *err, size_t errcap) {
    objview_t *v = &g_objview.v;

    if (!rs || !rs->loaded || !rs->main_data || !rs->polygons) {
        snprintf(err, errcap, "no ROM set loaded yet");
        return false;
    }
    if (!g_active_profile) { snprintf(err, errcap, "no game profile resolved"); return false; }
    const game_quirks_t *q = &g_active_profile->quirks;

    int          model = v->model;
    const float *mat   = NULL;
    float        place[12];

    if (v->source == 1) {
        if (!g_geo3d_state) { snprintf(err, errcap, "geo3d not initialised"); return false; }
        int n = g_geo3d_state->captured_count;
        if (v->capture < 0 || v->capture >= n) {
            snprintf(err, errcap, "capture %d is outside this frame's %d", v->capture, n);
            return false;
        }
        const captured_model_t *cm = &g_geo3d_state->captured[v->capture];
        model = cm->model_idx;
        if (v->use_capture_matrix && cm->has_matrix) mat = cm->matrix;
    }
    if (model < 0 || (uint32_t)model >= q->model_table_count) {
        snprintf(err, errcap, "model %d is past the table's %u", model, q->model_table_count);
        return false;
    }
    if (!mat) { objview__placement(v, place); mat = place; }

    /* The model table's material pointer is what the single-model browser
     * colours an untextured face with; keep the two agreeing. */
    float cr = 1.0f, cg = 1.0f, cb = 1.0f;
    uint32_t toff = q->model_table_offset + (uint32_t)model * MODEL_ENTRY_SIZE;
    if ((size_t)toff + MODEL_ENTRY_SIZE <= rs->main_data_size)
        material_ptr_to_color(read_u32_le(rs->main_data + toff + 4), &cr, &cg, &cb);

    /* geo3d_decode_model keeps its scratch in statics, so only one decode may be
     * in flight anywhere. The bridge's model dump raises the same flag. */
    if (g_geo3d_dump_busy) {
        snprintf(err, errcap, "another model decode is in flight");
        return false;
    }

    const uint8_t   *saved_pal      = g_geo3d_palram;
    size_t           saved_pal_size = g_geo3d_palram_size;
    geo3d_tri_buf_t *saved_sink     = g_geo3d_tri_sink;

    /* Faces take their colour from palette RAM, exactly as the frame does. */
    if (bus) { g_geo3d_palram = bus->palette; g_geo3d_palram_size = PALETTE_SIZE; }
    g_geo3d_dump_busy    = 1;
    g_geo3d_tri_sink     = &g_objview.tris;
    g_objview.tris.count = 0;
    /* Lines have no sink of their own. Taking the shared buffer is safe here
     * because the frame rebuilds it from scratch on its next pass through
     * geo3d_build_wireframes — which is also why objview_service has to run
     * after the swapchain pass, not before it. */
    g_geo3d_lines.count = 0;
    /* This runs on the render thread, which owns the mesh cache, so the decode
     * may take each face's layer from it (geo3d_mesh_layers). */
    g_geo3d_decode_layers = v->layers;

    geo3d_decode_model(model,
                       rs->main_data, rs->main_data_size,
                       rs->polygons,  rs->polygons_size,
                       rs->textures,  rs->textures_size,
                       q->model_table_offset, q->model_table_count,
                       q->mesh_ptr_subtract, q->mesh_ptr_add,
                       mat, cr, cg, cb);

    g_geo3d_decode_layers = 0;
    g_geo3d_tri_sink    = saved_sink;
    g_geo3d_dump_busy   = 0;
    g_geo3d_palram      = saved_pal;
    g_geo3d_palram_size = saved_pal_size;

    v->drawn_model = model;
    v->tris        = g_objview.tris.count;
    v->lines       = g_geo3d_lines.count;
    v->have_result = 1;

    if (v->tris == 0 && v->lines == 0) {
        memset(v->bmin, 0, sizeof v->bmin);
        memset(v->bmax, 0, sizeof v->bmax);
        snprintf(err, errcap, "model %d decoded to no geometry (empty table entry?)", model);
        return false;
    }

    /* Bounds over everything emitted, so autofit frames a lines-only model too. */
    float lo[3] = {  1e30f,  1e30f,  1e30f };
    float hi[3] = { -1e30f, -1e30f, -1e30f };
    for (int i = 0; i < g_objview.tris.count; i++) {
        const geo3d_tri_t *T = &g_objview.tris.tris[i];
        objview__grow(lo, hi, T->x0, T->y0, T->z0);
        objview__grow(lo, hi, T->x1, T->y1, T->z1);
        objview__grow(lo, hi, T->x2, T->y2, T->z2);
    }
    for (int i = 0; i < g_geo3d_lines.count; i++) {
        const geo3d_line_t *L = &g_geo3d_lines.lines[i];
        objview__grow(lo, hi, L->x0, L->y0, L->z0);
        objview__grow(lo, hi, L->x1, L->y1, L->z1);
    }
    memcpy(v->bmin, lo, sizeof lo);
    memcpy(v->bmax, hi, sizeof hi);

    /* Untextured on request: the fill shader takes a zero tile width as flat
     * colour, which separates a texturing artifact from a geometry one. */
    if (!v->textured) {
        for (int i = 0; i < g_objview.tris.count; i++) {
            g_objview.tris.tris[i].tw = 0.0f;
            g_objview.tris.tris[i].th = 0.0f;
        }
    }
    return true;
}

/* Centre the orbit on the model and pull back far enough to hold its bounding
 * sphere, on whichever of the frame's two axes is the tighter one. */
static inline void objview__autofit(objview_t *v) {
    float c[3], r2 = 0.0f;
    for (int k = 0; k < 3; k++) {
        c[k] = (v->bmin[k] + v->bmax[k]) * 0.5f;
        float half = (v->bmax[k] - v->bmin[k]) * 0.5f;
        r2 += half * half;
    }
    float r = sqrtf(r2);
    if (!(r > 1e-4f)) r = 1.0f;
    memcpy(v->target, c, sizeof c);

    float aspect  = (v->height > 0) ? (float)v->width / (float)v->height : 1.0f;
    float tan_v   = tanf((v->fov > 1.0f ? v->fov : 45.0f) * 0.5f * 3.14159265358979f / 180.0f);
    float tan_h   = tan_v * aspect;
    float tan_min = tan_v < tan_h ? tan_v : tan_h;
    float sin_min = tan_min / sqrtf(1.0f + tan_min * tan_min);
    float margin  = v->fit_margin > 0.1f ? v->fit_margin : 1.0f;
    v->dist = r / sin_min * margin;
    if (v->dist < 0.05f) v->dist = 0.05f;
}

/* The orbit, resolved against gm_mat4_view's convention: at yaw 0 / pitch 0 the
 * eye sits on +Z looking down −Z, which puts the model's front to the camera. */
static inline void objview__eye(const objview_t *v, float yaw_deg, float pitch_deg,
                                float *eye, float *rot_y, float *rot_x) {
    const float d2r = 3.14159265358979f / 180.0f;
    float y = yaw_deg * d2r, p = pitch_deg * d2r;
    eye[0] = v->target[0] + v->dist * cosf(p) * sinf(y);
    eye[1] = v->target[1] + v->dist * sinf(p);
    eye[2] = v->target[2] + v->dist * cosf(p) * cosf(y);
    *rot_y = y;
    *rot_x = p;
}

/* ---- GPU resources ------------------------------------------------------- */

static inline void objview__discard_target(void) {
    if (g_objview.color_tex.id) sg_destroy_view(g_objview.color_tex);
    if (g_objview.color_att.id) sg_destroy_view(g_objview.color_att);
    if (g_objview.depth_att.id) sg_destroy_view(g_objview.depth_att);
    if (g_objview.color_img.id) sg_destroy_image(g_objview.color_img);
    if (g_objview.depth_img.id) sg_destroy_image(g_objview.depth_img);
    g_objview.color_tex.id = 0;
    g_objview.color_att.id = 0;
    g_objview.depth_att.id = 0;
    g_objview.color_img.id = 0;
    g_objview.depth_img.id = 0;
    g_objview.rt_w = 0;
    g_objview.rt_h = 0;
}

/*
 * (Re)create the offscreen target at w x h.
 *
 * Its formats are the swapchain's, taken from the environment sg_setup() was
 * given: the fill and line pipelines were created against those defaults, and
 * sokol will not let a pipeline draw into a pass whose formats differ. Which is
 * also why the target is multisampled exactly when the window is.
 */
static inline bool objview__ensure_target(int w, int h) {
    if (w < OBJVIEW_MIN_DIM) w = OBJVIEW_MIN_DIM;
    if (h < OBJVIEW_MIN_DIM) h = OBJVIEW_MIN_DIM;
    if (w > OBJVIEW_MAX_DIM) w = OBJVIEW_MAX_DIM;
    if (h > OBJVIEW_MAX_DIM) h = OBJVIEW_MAX_DIM;
    if (g_objview.rt_w == w && g_objview.rt_h == h && g_objview.color_att.id) return true;

    objview__discard_target();

    sg_desc d = sg_query_desc();
    sg_pixel_format cfmt = d.environment.defaults.color_format;
    sg_pixel_format dfmt = d.environment.defaults.depth_format;
    int             smp  = d.environment.defaults.sample_count;
    if (cfmt == SG_PIXELFORMAT_NONE) cfmt = SG_PIXELFORMAT_RGBA8;
    if (dfmt == SG_PIXELFORMAT_NONE) dfmt = SG_PIXELFORMAT_DEPTH_STENCIL;
    if (smp  <= 0)                   smp  = 1;

    g_objview.color_img = sg_make_image(&(sg_image_desc){
        .usage.color_attachment = true,
        .width = w, .height = h, .pixel_format = cfmt, .sample_count = smp,
        .label = "objview-color",
    });
    g_objview.depth_img = sg_make_image(&(sg_image_desc){
        .usage.depth_stencil_attachment = true,
        .width = w, .height = h, .pixel_format = dfmt, .sample_count = smp,
        .label = "objview-depth",
    });
    g_objview.color_att = sg_make_view(&(sg_view_desc){
        .color_attachment.image = g_objview.color_img, .label = "objview-color-att" });
    g_objview.depth_att = sg_make_view(&(sg_view_desc){
        .depth_stencil_attachment.image = g_objview.depth_img, .label = "objview-depth-att" });
    /* Only a single-sampled attachment can also be sampled as a texture; with
     * MSAA on it is the preview window that goes without, not the shots. */
    if (smp == 1)
        g_objview.color_tex = sg_make_view(&(sg_view_desc){
            .texture.image = g_objview.color_img, .label = "objview-color-tex" });

    if (sg_query_image_state(g_objview.color_img) != SG_RESOURCESTATE_VALID ||
        sg_query_image_state(g_objview.depth_img) != SG_RESOURCESTATE_VALID ||
        sg_query_view_state(g_objview.color_att)  != SG_RESOURCESTATE_VALID ||
        sg_query_view_state(g_objview.depth_att)  != SG_RESOURCESTATE_VALID) {
        objview__discard_target();
        return false;
    }
    g_objview.rt_w = w;
    g_objview.rt_h = h;
    return true;
}

static inline bool objview__ensure_buffers(int fill_verts, int line_verts) {
    if (fill_verts > g_objview.fill_cap) {
        if (g_objview.fill_vbuf.id) sg_destroy_buffer(g_objview.fill_vbuf);
        int cap = fill_verts + fill_verts / 2 + 3072;
        g_objview.fill_vbuf = sg_make_buffer(&(sg_buffer_desc){
            .usage.stream_update = true,
            .size = (size_t)cap * sizeof(game_render_tex_vertex_t),
            .label = "objview-fill-vbuf" });
        g_objview.fill_cap = g_objview.fill_vbuf.id ? cap : 0;
    }
    if (line_verts > g_objview.line_cap) {
        if (g_objview.line_vbuf.id) sg_destroy_buffer(g_objview.line_vbuf);
        int cap = line_verts + line_verts / 2 + 3072;
        g_objview.line_vbuf = sg_make_buffer(&(sg_buffer_desc){
            .usage.stream_update = true,
            .size = (size_t)cap * sizeof(game_render_line_vertex_t),
            .label = "objview-line-vbuf" });
        g_objview.line_cap = g_objview.line_vbuf.id ? cap : 0;
    }
    return (fill_verts <= g_objview.fill_cap) && (line_verts <= g_objview.line_cap);
}

/* ---- Drawing ------------------------------------------------------------- */

/*
 * Pack this decode into the viewer's buffers. Once per service call: the
 * geometry does not change between the angles of one batch, only the camera.
 *
 * The CPU-side staging arrays are the renderer's own. Borrowing them is safe
 * because this runs on the render thread, after the frame has already uploaded
 * out of them — and it keeps two more multi-megabyte arrays out of the build.
 * The GPU buffers underneath are the viewer's, because sokol allows one
 * sg_update_buffer per buffer per frame and the frame has spent the renderer's.
 */
static inline void objview__upload(int *out_fill_verts, int *out_line_verts) {
    int nt = g_objview.tris.count;
    if (nt > GEO3D_MAX_TRIS) nt = GEO3D_MAX_TRIS;
    int nl = g_geo3d_lines.count;
    if (nl > GEO3D_MAX_LINES) nl = GEO3D_MAX_LINES;

    *out_fill_verts = 0;
    *out_line_verts = 0;
    if (!objview__ensure_buffers(nt * 3, nl * 2)) return;

    if (nt > 0) {
        /* The frame's own packing (game_render_batch_flush), so shading here and
         * shading on screen are the same shading. The colour ramp is a GL-only
         * path and is already spent for this frame by the time we run, so a
         * colour the game has not drawn falls back to the flat path. */
        for (int i = 0; i < nt; i++) {
            const geo3d_tri_t *T = &g_objview.tris.tris[i];
            game_render_tex_vertex_t *v = &g_game_render.fill_verts[i * 3];
            v[0].x=T->x0; v[0].y=T->y0; v[0].z=T->z0; v[0].u=T->u0; v[0].v=T->v0;
            v[1].x=T->x1; v[1].y=T->y1; v[1].z=T->z1; v[1].u=T->u1; v[1].v=T->v1;
            v[2].x=T->x2; v[2].y=T->y2; v[2].z=T->z2; v[2].u=T->u2; v[2].v=T->v2;
            float lb   = g_luma_ramp ? T->lb : -1.0f;
            float ramp = 1.0f;
            if (g_game_render.ramp_enabled) {
                int row = game_render__ramp_row(T->r, T->g, T->b);
                if (row >= 0) ramp = (float)(row + 2);
            }
            for (int k = 0; k < 3; k++) {
                v[k].r=T->r; v[k].g=T->g; v[k].b=T->b; v[k].a=ramp;
                v[k].tx=T->tx; v[k].ty=T->ty; v[k].tw=T->tw; v[k].th=T->th;
                v[k].lb=lb; v[k].pl=T->pl; v[k].fl=T->fl; v[k].texlod=T->texlod;
                /* Plain depth here: the board's polygon z-sort is right under
                 * the board's own camera and turns a floor into a wall under a
                 * free one, and this viewer's camera goes anywhere. A face's
                 * layer holds from any side it is drawn from, so it stays (the
                 * explorer's model view keeps it too); its plane is camera
                 * space for the board's camera and does not. */
                v[k].zs=GEO3D_ZSORT_NONE; v[k].zl=T->zl;
            }
        }
        sg_update_buffer(g_objview.fill_vbuf, &(sg_range){
            .ptr  = g_game_render.fill_verts,
            .size = (size_t)nt * 3 * sizeof(game_render_tex_vertex_t) });
        *out_fill_verts = nt * 3;
    }
    if (nl > 0) {
        for (int i = 0; i < nl; i++) {
            const geo3d_line_t *L = &g_geo3d_lines.lines[i];
            game_render_line_vertex_t *v = &g_game_render.line_verts[i * 2];
            v[0].x=L->x0; v[0].y=L->y0; v[0].z=L->z0; v[0].r=L->r; v[0].g=L->g; v[0].b=L->b; v[0].a=1.0f;
            v[1].x=L->x1; v[1].y=L->y1; v[1].z=L->z1; v[1].r=L->r; v[1].g=L->g; v[1].b=L->b; v[1].a=1.0f;
        }
        sg_update_buffer(g_objview.line_vbuf, &(sg_range){
            .ptr  = g_game_render.line_verts,
            .size = (size_t)nl * 2 * sizeof(game_render_line_vertex_t) });
        *out_line_verts = nl * 2;
    }
}

/* One pass: clear the target and draw the uploaded geometry from one angle. */
static inline void objview__draw(const objview_t *v, float yaw, float pitch,
                                 int fill_verts, int line_verts) {
    float eye[3], rot_y, rot_x;
    objview__eye(v, yaw, pitch, eye, &rot_y, &rot_x);

    float proj[16], view[16], mvp[16], mvp_t[16];
    float aspect  = (g_objview.rt_h > 0) ? (float)g_objview.rt_w / (float)g_objview.rt_h : 1.0f;
    float fov_rad = (v->fov > 1.0f ? v->fov : 45.0f) * 3.14159265358979f / 180.0f;
    gm_mat4_perspective(proj, fov_rad, aspect, 0.1f, 5000.0f);
    /* gm_mat4_view honours g_cam_rot_only, which the scene's eye-bake detector
     * drives frame by frame — the viewer's own camera is never eye-baked. */
    int saved_rot_only = g_cam_rot_only;
    g_cam_rot_only = 0;
    gm_mat4_view(view, eye[0], eye[1], eye[2], rot_y, rot_x);
    g_cam_rot_only = saved_rot_only;
    gm_mat4_mul(mvp, proj, view);
    gm_mat4_transpose(mvp_t, mvp);

    game_render_vs_params_t vs;
    memcpy(vs.mvp, mvp_t, sizeof mvp_t);

    sg_begin_pass(&(sg_pass){
        .action = {
            .colors[0] = { .load_action = SG_LOADACTION_CLEAR,
                           .clear_value = { v->bg[0], v->bg[1], v->bg[2], v->bg[3] } },
            .depth     = { .load_action = SG_LOADACTION_CLEAR, .clear_value = 1.0f },
        },
        .attachments = { .colors[0] = g_objview.color_att, .depth_stencil = g_objview.depth_att },
        .label = "objview-pass",
    });
    sg_apply_viewport(0, 0, g_objview.rt_w, g_objview.rt_h, true);
    sg_apply_scissor_rect(0, 0, g_objview.rt_w, g_objview.rt_h, true);

    if (fill_verts > 0) {
        sg_apply_pipeline(v->cull == 1 ? g_game_render.fill_pipeline_cw :
                          v->cull == 2 ? g_game_render.fill_pipeline_ccw :
                                         g_game_render.fill_pipeline);
        sg_apply_bindings(&(sg_bindings){
            .vertex_buffers[0] = g_objview.fill_vbuf,
            .views[0]          = g_game_render.atlas_view,
            .views[1]          = g_game_render.luma_view,
            .views[2]          = g_game_render.cxlat_view,
            .views[3]          = g_game_render.ramp_view,
            .samplers[0]       = g_game_render.atlas_sampler,
        });
        sg_apply_uniforms(0, &(sg_range){ .ptr = &vs, .size = sizeof vs });
        sg_draw(0, fill_verts, 1);
    }
    if (v->wireframe && line_verts > 0) {
        sg_apply_pipeline(g_game_render.line_pipeline);
        sg_apply_bindings(&(sg_bindings){ .vertex_buffers[0] = g_objview.line_vbuf });
        sg_apply_uniforms(0, &(sg_range){ .ptr = &vs, .size = sizeof vs });
        sg_draw(0, line_verts, 1);
    }
    sg_end_pass();
}

/* ---- Shot bookkeeping ---------------------------------------------------- */

/* How much of the frame the object covers, and where it sits in it. Measured
 * against the clear colour, so it counts drawn pixels rather than bright ones. */
static inline void objview__measure(const uint8_t *px, int w, int h,
                                    const float bg[4], objview_shot_t *s) {
    int br = (int)(bg[0] * 255.0f + 0.5f);
    int bgn= (int)(bg[1] * 255.0f + 0.5f);
    int bb = (int)(bg[2] * 255.0f + 0.5f);
    long hits = 0;
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = px + (size_t)y * (size_t)w * 4;
        for (int x = 0; x < w; x++) {
            int dr = (int)row[x*4+0] - br;
            int dg = (int)row[x*4+1] - bgn;
            int db = (int)row[x*4+2] - bb;
            if (dr*dr + dg*dg + db*db > 12) {      /* a hair above rounding */
                hits++;
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        }
    }
    s->coverage = (float)hits / (float)((long)w * (long)h);
    if (x1 < 0) { s->px0 = s->py0 = s->px1 = s->py1 = -1; }
    else        { s->px0 = x0; s->py0 = y0; s->px1 = x1; s->py1 = y1; }
}

/* "shots/obj.png" + index 3 of 8 → "shots/obj-003.png". A single shot keeps the
 * path it was given, so a one-angle request writes exactly the file it asked
 * for. The extension is only recognised in the last path component, so a dot in
 * a directory name does not become the split point. */
static inline void objview__shot_path(const char *base, int i, int of, char *out, size_t cap) {
    if (of <= 1) { snprintf(out, cap, "%s", base); return; }
    const char *dot = strrchr(base, '.');
    const char *sl  = strrchr(base, '/');
    const char *bs  = strrchr(base, '\\');
    const char *sep = (sl > bs) ? sl : bs;
    if (dot && (!sep || dot > sep))
        snprintf(out, cap, "%.*s-%03d%s", (int)(dot - base), base, i, dot);
    else
        snprintf(out, cap, "%s-%03d.png", base, i);
}

/* The six axis views, named by where the camera stands rather than by which
 * side of the object it sees: a model's own facing is whatever the ROM gave it,
 * and in STF a head faces along its X. */
static inline void objview__six(int i, float *yaw, float *pitch) {
    static const float ang[6][2] = {
        {   0.0f,   0.0f },   /* +Z */
        {  90.0f,   0.0f },   /* +X */
        { 180.0f,   0.0f },   /* -Z */
        { 270.0f,   0.0f },   /* -X */
        {   0.0f,  89.0f },   /* +Y, looking down */
        {   0.0f, -89.0f },   /* -Y, looking up   */
    };
    *yaw   = ang[i % 6][0];
    *pitch = ang[i % 6][1];
}

/* Release whatever the last batch kept. Called before a new batch overwrites
 * the records, and at shutdown. */
static inline void objview_free_shots(void) {
    objview_req_t *rq = &g_objview.req;
    for (int i = 0; i < OBJVIEW_MAX_SHOTS; i++) {
        if (rq->shot[i].png) { gfx_free_png(rq->shot[i].png); rq->shot[i].png = NULL; }
    }
}

/* ---- Per-frame service --------------------------------------------------- */

/*
 * Called once per host frame from the frontend, OUTSIDE any pass — the viewer
 * opens passes of its own and sokol does not nest them. Put it after the
 * swapchain pass has ended: it takes the shared line buffer, which the next
 * frame rebuilds from scratch anyway.
 *
 * A whole shot batch is serviced inside this one call. The geometry is decoded
 * and uploaded once and every angle is a pass over it, and each readback maps
 * the staging copy straight after its pass — so a batch is one long frame
 * rather than one frame per angle, and the bridge thread's wait is short.
 */
static inline void objview_service(const romset_t *rs, memory_bus_t *bus) {
    objview_req_t *rq = &g_objview.req;
    objview_t     *v  = &g_objview.v;
    const bool shooting = (rq->pending != 0);

    const bool refresh = (g_objview.refresh != 0);

    if (!shooting && !refresh && !(v->active && g_objview.preview_open)) return;
    g_objview.refresh = 0;

    char     err[192];
    int      fill_verts = 0, line_verts = 0;
    int      n = 1;
    uint8_t *px = NULL;
    float    eye[3], ry, rx;
    err[0] = '\0';

    if (!g_game_render.initialized) {
        snprintf(err, sizeof err, "the renderer is not initialised (headless run?)");
        goto fail;
    }
    if (!objview__decode(rs, bus, err, sizeof err)) goto fail;
    if (!objview__ensure_target(v->width, v->height)) {
        snprintf(err, sizeof err, "could not create a %dx%d render target", v->width, v->height);
        goto fail;
    }
    v->width  = g_objview.rt_w;      /* report what was actually made */
    v->height = g_objview.rt_h;
    if (v->autofit) objview__autofit(v);

    objview__upload(&fill_verts, &line_verts);
    if (fill_verts == 0 && line_verts == 0) {
        snprintf(err, sizeof err, "nothing to draw for model %d", v->drawn_model);
        goto fail;
    }

    if (!shooting) {
        objview__eye(v, v->yaw, v->pitch, eye, &ry, &rx);
        memcpy(v->cam, eye, sizeof eye);
        objview__draw(v, v->yaw, v->pitch, fill_verts, line_verts);
        g_objview.last_ok     = 1;
        g_objview.last_err[0] = '\0';
        g_objview.serial++;     /* last: a bridge caller polls this for a refresh */
        return;
    }

    if (!gfx_readback_supported()) {
        snprintf(err, sizeof err, "no screenshot path on this graphics backend");
        goto fail;
    }

    n = rq->six ? 6 : rq->count;
    if (n < 1) n = 1;
    if (n > OBJVIEW_MAX_SHOTS) n = OBJVIEW_MAX_SHOTS;

    px = (uint8_t *)malloc((size_t)g_objview.rt_w * (size_t)g_objview.rt_h * 4);
    if (!px) { snprintf(err, sizeof err, "out of memory for the readback buffer"); goto fail; }

    objview_free_shots();      /* whatever the last batch kept, the caller has had */
    rq->shots = 0;
    for (int i = 0; i < n; i++) {
        float yaw, pitch;
        if (rq->six) objview__six(i, &yaw, &pitch);
        else {
            yaw   = rq->yaw0   + rq->yaw_step   * (float)i;
            pitch = rq->pitch0 + rq->pitch_step * (float)i;
        }

        objview__draw(v, yaw, pitch, fill_verts, line_verts);

        objview_shot_t *s = &rq->shot[rq->shots];
        memset(s, 0, sizeof *s);
        s->yaw   = yaw;
        s->pitch = pitch;
        if (rq->path[0]) objview__shot_path(rq->path, i, n, s->path, sizeof s->path);

        if (!gfx_readback_rgba8(g_objview.color_img, g_objview.rt_w, g_objview.rt_h, px)) {
            snprintf(err, sizeof err, "readback failed: %s", gfx_readback_error());
            goto fail;
        }
        objview__measure(px, g_objview.rt_w, g_objview.rt_h, v->bg, s);

        size_t len = 0;
        void  *png = gfx_encode_png(px, g_objview.rt_w, g_objview.rt_h, &len);
        if (!png) {
            snprintf(err, sizeof err, "%s", gfx_readback_error());
            goto fail;
        }
        if (s->path[0] && gfx_save_png(s->path, png, len) == 0) {
            gfx_free_png(png);
            snprintf(err, sizeof err, "%s", gfx_readback_error());
            goto fail;
        }
        s->bytes = len;
        if (rq->keep) s->png = png;      /* collected by the caller, freed next batch */
        else          gfx_free_png(png);
        rq->shots++;
    }
    free(px);
    px = NULL;

    /* Leave the viewer's own angle on the last shot, so the preview window and
     * the files on disk do not disagree about what was looked at. */
    if (rq->shots > 0) {
        v->yaw   = rq->shot[rq->shots - 1].yaw;
        v->pitch = rq->shot[rq->shots - 1].pitch;
        objview__eye(v, v->yaw, v->pitch, eye, &ry, &rx);
        memcpy(v->cam, eye, sizeof eye);
    }
    rq->ok                = 1;
    rq->err[0]            = '\0';
    g_objview.last_ok     = 1;
    g_objview.last_err[0] = '\0';
    g_objview.serial++;
    rq->pending = 0;
    rq->done    = 1;        /* last: the bridge thread is spinning on it */
    return;

fail:
    free(px);
    g_objview.last_ok = 0;
    snprintf(g_objview.last_err, sizeof g_objview.last_err, "%s", err);
    g_objview.serial++;
    if (shooting) {
        rq->ok = 0;
        snprintf(rq->err, sizeof rq->err, "%s", err);
        rq->pending = 0;
        rq->done    = 1;
    }
}

static inline void objview_shutdown(void) {
    objview_free_shots();
    objview__discard_target();
    if (g_objview.fill_vbuf.id) sg_destroy_buffer(g_objview.fill_vbuf);
    if (g_objview.line_vbuf.id) sg_destroy_buffer(g_objview.line_vbuf);
    g_objview.fill_vbuf.id = 0;
    g_objview.line_vbuf.id = 0;
    g_objview.fill_cap = 0;
    g_objview.line_cap = 0;
}

#endif /* OBJVIEW_H */
