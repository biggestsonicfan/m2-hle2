/*
 * av_capture.h — the GPU half of the A/V stream: an offscreen target at the
 * stream's own resolution, and an asynchronous readback ring that never stalls
 * the thread that renders.
 *
 * ui/gfx_readback.h is the other way of doing this and the right one for a
 * screenshot: it makes a staging texture, copies, maps it on the spot and
 * swaps channels per pixel. Every one of those is a full pipeline stall, which
 * is fine once and hopeless sixty times a second. Here instead:
 *
 *   - AV_CAP_SLOTS staging copies rotate, and the one MAPPED is the copy
 *     issued AV_CAP_LAG submits ago, so the GPU has had two frames to finish
 *     with it. The map is non-blocking (D3D11_MAP_FLAG_DO_NOT_WAIT, or a
 *     zero-timeout fence wait on GL); a copy that is somehow still in flight
 *     is dropped rather than waited for.
 *   - the target is made in the backend's own default colour format, which on
 *     D3D11 is already BGRA8 — the wire format — so nothing is swizzled at all.
 *     The GL backends read back RGBA and the swap happens inside the one copy
 *     that has to happen anyway.
 *   - that copy goes straight into a queue slot owned by core/av_stream.h, and
 *     the writer thread sends it. Nothing here touches a socket.
 *
 * THE PICTURE IS THE GAME AND NOTHING ELSE: the target holds only what
 * game_frame_draw puts there — no ImGui, no menu bar, no cursor. There are no
 * letterbox bars either, because the game is drawn across the whole target;
 * pick an --av-size with the board's 496:384 aspect (1396x1080 is that, to the
 * nearest even pixel) and nothing is rescaled on the way out.
 */
#ifndef AV_CAPTURE_H
#define AV_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

#include "sokol_gfx.h"

#include "av_stream.h"
#include "emu_thread.h"   /* g_frame_clock — the board's own frame/sample pairs */
#include "log.h"

#if defined(SOKOL_D3D11)
#  ifndef COBJMACROS
#    define COBJMACROS
#  endif
#  include <d3d11.h>
#elif defined(SOKOL_GLCORE)
#  define GL_GLEXT_PROTOTYPES
#  include <GL/gl.h>
#  include <GL/glext.h>
#elif defined(SOKOL_GLES3)
#  include <GLES3/gl3.h>
#endif

#if defined(SOKOL_D3D11) || defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
#  define AV_CAPTURE_BACKEND 1
#endif

#define AV_CAP_SLOTS 3   /* staging copies in rotation */
#define AV_CAP_LAG   2   /* map the copy issued this many submits ago */

typedef struct {
    bool     ready;
    int      w, h;
    uint32_t bytes;
    bool     swap_rb;            /* the attachment is RGBA8; the wire wants BGRA */
    bool     flip;               /* GL hands back the bottom row first */

    sg_image color_img, depth_img;
    sg_view  color_att, depth_att, color_tex;
    sg_pass_action action;

    uint64_t submitted, taken;
    uint64_t tag_frame[AV_CAP_SLOTS], tag_sample[AV_CAP_SLOTS];

#if defined(SOKOL_D3D11)
    ID3D11Texture2D *stage[AV_CAP_SLOTS];
#elif defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
    GLuint pbo[AV_CAP_SLOTS];
    GLsync fence[AV_CAP_SLOTS];
    GLuint fbo;
#endif
} av_capture_t;

static av_capture_t g_av_cap;

static inline bool av_capture_ready(void)    { return g_av_cap.ready; }
static inline sg_view av_capture_color_att(void) { return g_av_cap.color_att; }
static inline sg_view av_capture_depth_att(void) { return g_av_cap.depth_att; }
static inline sg_view av_capture_color_tex(void) { return g_av_cap.color_tex; }
static inline sg_pass_action av_capture_action(void) { return g_av_cap.action; }

/* ---- Backend: create and destroy the staging ring ------------------------ */

static inline void av__cap_free_stage(void) {
#if defined(SOKOL_D3D11)
    for (int i = 0; i < AV_CAP_SLOTS; i++) {
        if (g_av_cap.stage[i]) { ID3D11Texture2D_Release(g_av_cap.stage[i]); g_av_cap.stage[i] = NULL; }
    }
#elif defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
    for (int i = 0; i < AV_CAP_SLOTS; i++) {
        if (g_av_cap.fence[i]) { glDeleteSync(g_av_cap.fence[i]); g_av_cap.fence[i] = 0; }
        if (g_av_cap.pbo[i])   { glDeleteBuffers(1, &g_av_cap.pbo[i]); g_av_cap.pbo[i] = 0; }
    }
    if (g_av_cap.fbo) { glDeleteFramebuffers(1, &g_av_cap.fbo); g_av_cap.fbo = 0; }
#endif
}

static inline bool av__cap_make_stage(void) {
#if defined(SOKOL_D3D11)
    ID3D11Device *dev = (ID3D11Device *)sg_d3d11_device();
    ID3D11Texture2D *src = (ID3D11Texture2D *)sg_d3d11_query_image_info(g_av_cap.color_img).tex2d;
    if (!dev || !src) { LOG_ERROR("av: no D3D11 texture behind the capture target"); return false; }

    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(src, &d);
    g_av_cap.swap_rb = !(d.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                         d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    g_av_cap.flip    = false;
    /* A staging copy is the only resource the CPU may map; everything else is
     * carried over so the copy stays format- and size-compatible with it. */
    d.Usage          = D3D11_USAGE_STAGING;
    d.BindFlags      = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags      = 0;
    for (int i = 0; i < AV_CAP_SLOTS; i++) {
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &g_av_cap.stage[i])) ||
            !g_av_cap.stage[i]) {
            LOG_ERROR("av: CreateTexture2D(staging) failed for slot %d", i);
            return false;
        }
    }
    return true;

#elif defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
#  if defined(SOKOL_GLCORE)
    g_av_cap.swap_rb = false;          /* desktop GL reads GL_BGRA directly */
#  else
    g_av_cap.swap_rb = true;           /* GLES 3 only guarantees GL_RGBA */
#  endif
    g_av_cap.flip = true;              /* glReadPixels starts at the bottom row */
    glGenFramebuffers(1, &g_av_cap.fbo);
    for (int i = 0; i < AV_CAP_SLOTS; i++) {
        glGenBuffers(1, &g_av_cap.pbo[i]);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, g_av_cap.pbo[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)g_av_cap.bytes, NULL, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    sg_reset_state_cache();
    return true;

#else
    LOG_ERROR("av: no readback path for this sokol backend (Metal / WebGPU)");
    return false;
#endif
}

/* ---- Backend: issue one copy, and map one that has aged ------------------ */

static inline void av__cap_copy_out(const uint8_t *src, uint32_t src_pitch, uint8_t *dst) {
    const int w = g_av_cap.w, h = g_av_cap.h;
    const uint32_t dst_pitch = (uint32_t)w * 4u;
    for (int y = 0; y < h; y++) {
        const uint8_t *s = src + (size_t)(g_av_cap.flip ? (h - 1 - y) : y) * src_pitch;
        uint8_t       *d = dst + (size_t)y * dst_pitch;
        if (!g_av_cap.swap_rb) {
            memcpy(d, s, dst_pitch);
        } else {
            for (int x = 0; x < w; x++) {
                d[x * 4 + 0] = s[x * 4 + 2];
                d[x * 4 + 1] = s[x * 4 + 1];
                d[x * 4 + 2] = s[x * 4 + 0];
                d[x * 4 + 3] = s[x * 4 + 3];
            }
        }
    }
}

static inline void av__cap_issue(uint32_t idx) {
#if defined(SOKOL_D3D11)
    ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)sg_d3d11_device_context();
    ID3D11Texture2D *src = (ID3D11Texture2D *)sg_d3d11_query_image_info(g_av_cap.color_img).tex2d;
    if (!ctx || !src) return;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)g_av_cap.stage[idx],
                                          (ID3D11Resource *)src);
    /* Hand the command buffer to the GPU now. A windowed app gets this for
     * free from Present; headless there is no Present at all, so without it
     * the copy sits unsubmitted and the non-blocking map two frames later
     * still finds it unfinished. Measured: 30% of frames survived the map
     * without this, and all of them with it. Flush does not wait. */
    ID3D11DeviceContext_Flush(ctx);
#elif defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
    sg_gl_image_info gi = sg_gl_query_image_info(g_av_cap.color_img);
    if (gi.tex[gi.active_slot] == 0) return;
    if (g_av_cap.fence[idx]) { glDeleteSync(g_av_cap.fence[idx]); g_av_cap.fence[idx] = 0; }
    glBindFramebuffer(GL_FRAMEBUFFER, g_av_cap.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           gi.tex[gi.active_slot], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, g_av_cap.pbo[idx]);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
#  if defined(SOKOL_GLCORE)
        glReadPixels(0, 0, g_av_cap.w, g_av_cap.h, GL_BGRA, GL_UNSIGNED_BYTE, (void *)0);
#  else
        glReadPixels(0, 0, g_av_cap.w, g_av_cap.h, GL_RGBA, GL_UNSIGNED_BYTE, (void *)0);
#  endif
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        /* The fence is how the map below asks "done yet?" without waiting. */
        g_av_cap.fence[idx] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    sg_reset_state_cache();   /* sokol caches GL state; we changed it behind its back */
#else
    (void)idx;
#endif
}

/* Copy slot `idx` into `dst` if the GPU has finished with it. Never waits. */
static inline bool av__cap_map(uint32_t idx, uint8_t *dst) {
#if defined(SOKOL_D3D11)
    ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)sg_d3d11_device_context();
    if (!ctx || !g_av_cap.stage[idx]) return false;
    D3D11_MAPPED_SUBRESOURCE m;
    HRESULT hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_av_cap.stage[idx], 0,
                                         D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (FAILED(hr)) return false;          /* still drawing: drop it, do not stall */
    av__cap_copy_out((const uint8_t *)m.pData, m.RowPitch, dst);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_av_cap.stage[idx], 0);
    return true;

#elif defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
    if (!g_av_cap.fence[idx]) return false;
    GLenum st = glClientWaitSync(g_av_cap.fence[idx], 0, 0);
    bool done = (st == GL_ALREADY_SIGNALED || st == GL_CONDITION_SATISFIED);
    glDeleteSync(g_av_cap.fence[idx]);
    g_av_cap.fence[idx] = 0;
    if (!done) return false;               /* abandon these pixels, keep the pace */
    glBindBuffer(GL_PIXEL_PACK_BUFFER, g_av_cap.pbo[idx]);
    const uint8_t *p = (const uint8_t *)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                                         (GLsizeiptr)g_av_cap.bytes,
                                                         GL_MAP_READ_BIT);
    bool ok = p != NULL;
    if (ok) av__cap_copy_out(p, (uint32_t)g_av_cap.w * 4u, dst);
    if (p) glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    sg_reset_state_cache();
    return ok;

#else
    (void)idx; (void)dst;
    return false;
#endif
}

/* ---- Target -------------------------------------------------------------- */

static inline void av_capture_shutdown(void) {
    av__cap_free_stage();
    if (g_av_cap.color_tex.id) sg_destroy_view(g_av_cap.color_tex);
    if (g_av_cap.color_att.id) sg_destroy_view(g_av_cap.color_att);
    if (g_av_cap.depth_att.id) sg_destroy_view(g_av_cap.depth_att);
    if (g_av_cap.color_img.id) sg_destroy_image(g_av_cap.color_img);
    if (g_av_cap.depth_img.id) sg_destroy_image(g_av_cap.depth_img);
    memset(&g_av_cap, 0, sizeof g_av_cap);
}

/* Make the offscreen target. Call once sokol_gfx is up; the target takes the
 * backend's DEFAULT formats because the game's pipelines were built against
 * those and sokol will not draw one into an attachment of another shape. */
static inline bool av_capture_init(int w, int h) {
    if (g_av_cap.ready) return true;
#if !defined(AV_CAPTURE_BACKEND)
    (void)w; (void)h;
    LOG_ERROR("av: this build's graphics backend has no readback path (Metal / WebGPU)");
    return false;
#else
    memset(&g_av_cap, 0, sizeof g_av_cap);
    g_av_cap.w     = w;
    g_av_cap.h     = h;
    g_av_cap.bytes = (uint32_t)w * (uint32_t)h * 4u;

    sg_desc d = sg_query_desc();
    sg_pixel_format cfmt = d.environment.defaults.color_format;
    sg_pixel_format dfmt = d.environment.defaults.depth_format;
    int             smp  = d.environment.defaults.sample_count;
    if (cfmt == SG_PIXELFORMAT_NONE) cfmt = SG_PIXELFORMAT_RGBA8;
    if (dfmt == SG_PIXELFORMAT_NONE) dfmt = SG_PIXELFORMAT_DEPTH_STENCIL;
    if (smp  <= 0)                   smp  = 1;
    if (smp != 1) {
        /* A multisampled attachment cannot be copied into a mappable staging
         * resource, and resolving one per frame is the stall this file exists
         * to avoid. Nothing in this build asks for MSAA, so say so and stop
         * rather than stream something that is quietly not the game. */
        LOG_ERROR("av: the capture path needs a single-sampled backend (this one is %dx)", smp);
        return false;
    }

    g_av_cap.color_img = sg_make_image(&(sg_image_desc){
        .usage.color_attachment = true,
        .width = w, .height = h, .pixel_format = cfmt, .sample_count = 1,
        .label = "av-color" });
    g_av_cap.depth_img = sg_make_image(&(sg_image_desc){
        .usage.depth_stencil_attachment = true,
        .width = w, .height = h, .pixel_format = dfmt, .sample_count = 1,
        .label = "av-depth" });
    g_av_cap.color_att = sg_make_view(&(sg_view_desc){
        .color_attachment.image = g_av_cap.color_img, .label = "av-color-att" });
    g_av_cap.depth_att = sg_make_view(&(sg_view_desc){
        .depth_stencil_attachment.image = g_av_cap.depth_img, .label = "av-depth-att" });
    g_av_cap.color_tex = sg_make_view(&(sg_view_desc){
        .texture.image = g_av_cap.color_img, .label = "av-color-tex" });

    if (sg_query_image_state(g_av_cap.color_img) != SG_RESOURCESTATE_VALID ||
        sg_query_image_state(g_av_cap.depth_img) != SG_RESOURCESTATE_VALID ||
        sg_query_view_state(g_av_cap.color_att)  != SG_RESOURCESTATE_VALID ||
        sg_query_view_state(g_av_cap.depth_att)  != SG_RESOURCESTATE_VALID) {
        LOG_ERROR("av: could not create a %dx%d capture target", w, h);
        av_capture_shutdown();
        return false;
    }
    if (!av__cap_make_stage()) { av_capture_shutdown(); return false; }

    g_av_cap.action = (sg_pass_action){
        .colors[0] = { .load_action = SG_LOADACTION_CLEAR,
                       .clear_value = { 0.0f, 0.0f, 0.0f, 1.0f } },
        .depth     = { .load_action = SG_LOADACTION_CLEAR, .clear_value = 1.0f },
    };
    g_av_cap.ready = true;
    LOG_INFO("av: capture target %dx%d, %d staging copies, %s",
             w, h, AV_CAP_SLOTS,
             g_av_cap.swap_rb ? "channels swapped on readback" : "BGRA straight through");
    return true;
#endif
}

/* ---- Pacing -------------------------------------------------------------- */

/*
 * Has the board finished a frame that has not been captured yet? The pair is
 * the board's own: `frame` is what get_status reports, `sample` is the audio
 * clock at the moment that frame ended. Board frames that went past without a
 * capture are reported to the stream so the next packet carries the flag.
 *
 * This, not the display's vsync, is what paces the stream: a server has no
 * monitor, and a parked or occluded window may not be asked to present.
 */
static inline bool av_capture_due(uint64_t *frame, uint64_t *sample) {
    static uint64_t s_last;
    static bool     s_primed;
    uint64_t f, s;

    /* g_frame_clock is written sample-first, frame-second, and not atomically:
     * take both and re-read the frame to be sure they belong together. */
    for (int spin = 0; ; spin++) {
        f = g_frame_clock.frame;
        s = g_frame_clock.sample;
        if (f == g_frame_clock.frame || spin >= 4) break;
    }
    if (!s_primed) { s_primed = true; s_last = f; return false; }
    if (f == s_last) return false;
    if (f < s_last) s_last = f;                 /* the board was reset under us */
    else if (f > s_last + 1) av_stream_video_missed(f - s_last - 1, f);

    s_last  = f;
    *frame  = f;
    *sample = s;
    return true;
}

/* ---- Per-frame service --------------------------------------------------- */

/*
 * Harvest the copy issued AV_CAP_LAG frames ago into a queue slot, then issue
 * a copy of what was just drawn. Call OUTSIDE any pass, straight after the one
 * that drew into av_capture_color_att().
 */
static inline void av_capture_submit(uint64_t frame, uint64_t sample) {
    static uint64_t s_session;
    if (!g_av_cap.ready) return;

    /* A new client means the copies still in flight belong to the last one.
     * Their tags name board frames that are now in the past, and the first
     * video packet is what sets where the audio starts — so one leaked frame
     * puts the whole session's clock behind by however long the emulator sat
     * between clients. Throw them away rather than send them. */
    if (av_stream_session() != s_session) {
        s_session      = av_stream_session();
        g_av_cap.taken = g_av_cap.submitted;
    }

    if (g_av_cap.submitted - g_av_cap.taken >= AV_CAP_LAG) {
        uint32_t idx = (uint32_t)(g_av_cap.taken % AV_CAP_SLOTS);
        uint8_t *dst = av_stream_video_slot();
        if (!dst)
            av_stream_video_drop(g_av_cap.tag_frame[idx], AV_DROP_QUEUE);
        else if (av__cap_map(idx, dst))
            av_stream_video_commit(g_av_cap.tag_frame[idx], g_av_cap.tag_sample[idx]);
        else
            av_stream_video_drop(g_av_cap.tag_frame[idx], AV_DROP_READBACK);
        g_av_cap.taken++;
    }
    /* Belt and braces: the harvest above keeps this from ever firing, but a
     * ring that laps itself would copy over a slot still waiting to be read. */
    while (g_av_cap.submitted - g_av_cap.taken >= AV_CAP_SLOTS) {
        av_stream_video_drop(g_av_cap.tag_frame[g_av_cap.taken % AV_CAP_SLOTS], AV_DROP_QUEUE);
        g_av_cap.taken++;
    }

    uint32_t idx = (uint32_t)(g_av_cap.submitted % AV_CAP_SLOTS);
    g_av_cap.tag_frame[idx]  = frame;
    g_av_cap.tag_sample[idx] = sample;
    av__cap_issue(idx);
    g_av_cap.submitted++;
}

#endif /* AV_CAPTURE_H */
