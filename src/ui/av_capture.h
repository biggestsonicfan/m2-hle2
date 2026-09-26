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
 * NV12 (--av-format nv12). Two more passes turn the finished target, overlay
 * included, into BT.709 limited-range planes before anything is read back:
 * luma into an RGBA8 target a quarter as wide (four Y samples a texel), and
 * chroma into one a quarter as wide and half as tall (Cb,Cr,Cb,Cr: two 2x2
 * box means a texel). RGBA8 because every backend reads it back as it is,
 * where GLES 3 guarantees no GL_RED or GL_RG readback at all. The passes also
 * put the rows top-first on GL, so the CPU copy is a plain one on every
 * backend. On the fly's stream this replaced ~1.3 cores of swscale.
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
    bool     nv12;               /* convert on the GPU; see the top of the file */
    bool     want_card;          /* --av-test-card, asked for before init */

    sg_image color_img, depth_img;
    sg_view  color_att, depth_att, color_tex;
    sg_pass_action action;

    /* NV12: the two plane targets and the passes that fill them. */
    sg_image    y_img, c_img;
    sg_view     y_att, c_att;
    sg_shader   y_shd, c_shd;
    sg_pipeline y_pip, c_pip;
    sg_sampler  smp;

    /* --av-test-card: a still drawn over the game, for checking the planes. */
    sg_image    card_img;
    sg_view     card_tex;
    sg_shader   card_shd;
    sg_pipeline card_pip;

    uint64_t submitted, taken;
    uint64_t tag_frame[AV_CAP_SLOTS], tag_sample[AV_CAP_SLOTS];

#if defined(SOKOL_D3D11)
    ID3D11Texture2D *stage[AV_CAP_SLOTS];     /* BGRA: the picture; NV12: the Y plane */
    ID3D11Texture2D *stage_c[AV_CAP_SLOTS];   /* NV12: the CbCr plane */
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
        if (g_av_cap.stage_c[i]) { ID3D11Texture2D_Release(g_av_cap.stage_c[i]); g_av_cap.stage_c[i] = NULL; }
    }
#elif defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
    for (int i = 0; i < AV_CAP_SLOTS; i++) {
        if (g_av_cap.fence[i]) { glDeleteSync(g_av_cap.fence[i]); g_av_cap.fence[i] = 0; }
        if (g_av_cap.pbo[i])   { glDeleteBuffers(1, &g_av_cap.pbo[i]); g_av_cap.pbo[i] = 0; }
    }
    if (g_av_cap.fbo) { glDeleteFramebuffers(1, &g_av_cap.fbo); g_av_cap.fbo = 0; }
#endif
}

#if defined(SOKOL_D3D11)
/* A ring of staging copies of `img`: the only resource the CPU may map. Its
 * description is carried over so the copy stays format- and size-compatible. */
static inline bool av__cap_d3d_stage(sg_image img, ID3D11Texture2D **ring, DXGI_FORMAT *fmt) {
    ID3D11Device *dev = (ID3D11Device *)sg_d3d11_device();
    ID3D11Texture2D *src = (ID3D11Texture2D *)sg_d3d11_query_image_info(img).tex2d;
    if (!dev || !src) { LOG_ERROR("av: no D3D11 texture behind the capture target"); return false; }

    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(src, &d);
    if (fmt) *fmt = d.Format;
    d.Usage          = D3D11_USAGE_STAGING;
    d.BindFlags      = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags      = 0;
    for (int i = 0; i < AV_CAP_SLOTS; i++) {
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &ring[i])) || !ring[i]) {
            LOG_ERROR("av: CreateTexture2D(staging) failed for slot %d", i);
            return false;
        }
    }
    return true;
}
#endif

static inline bool av__cap_make_stage(void) {
#if defined(SOKOL_D3D11)
    g_av_cap.flip = false;
    if (g_av_cap.nv12) {
        g_av_cap.swap_rb = false;      /* the planes are RGBA8 laid out as bytes */
        return av__cap_d3d_stage(g_av_cap.y_img, g_av_cap.stage, NULL) &&
               av__cap_d3d_stage(g_av_cap.c_img, g_av_cap.stage_c, NULL);
    }
    DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
    if (!av__cap_d3d_stage(g_av_cap.color_img, g_av_cap.stage, &f)) return false;
    g_av_cap.swap_rb = !(f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    return true;

#elif defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
#  if defined(SOKOL_GLCORE)
    g_av_cap.swap_rb = false;          /* desktop GL reads GL_BGRA directly */
#  else
    g_av_cap.swap_rb = true;           /* GLES 3 only guarantees GL_RGBA */
#  endif
    g_av_cap.flip = true;              /* glReadPixels starts at the bottom row */
    if (g_av_cap.nv12) {
        /* The conversion passes already put the rows top-first, in bytes. */
        g_av_cap.swap_rb = false;
        g_av_cap.flip    = false;
    }
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

/* One NV12 plane: `rows` rows of `row_bytes`, from a mapping with its own pitch. */
static inline void av__cap_copy_plane(const uint8_t *src, uint32_t src_pitch, uint8_t *dst,
                                      uint32_t row_bytes, int rows) {
    if (src_pitch == row_bytes) { memcpy(dst, src, (size_t)row_bytes * (size_t)rows); return; }
    for (int y = 0; y < rows; y++)
        memcpy(dst + (size_t)y * row_bytes, src + (size_t)y * src_pitch, row_bytes);
}

#if defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
/* Read `img` (RGBA8, `w` x `h` texels) into the bound pack buffer at `offset`. */
static inline void av__cap_gl_read(sg_image img, GLenum fmt, int w, int h, size_t offset) {
    sg_gl_image_info gi = sg_gl_query_image_info(img);
    if (gi.tex[gi.active_slot] == 0) return;
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           gi.tex[gi.active_slot], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
        glReadPixels(0, 0, w, h, fmt, GL_UNSIGNED_BYTE, (void *)offset);
}
#endif

static inline void av__cap_issue(uint32_t idx) {
#if defined(SOKOL_D3D11)
    ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)sg_d3d11_device_context();
    ID3D11Texture2D *src = (ID3D11Texture2D *)sg_d3d11_query_image_info(
        g_av_cap.nv12 ? g_av_cap.y_img : g_av_cap.color_img).tex2d;
    if (!ctx || !src) return;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)g_av_cap.stage[idx],
                                          (ID3D11Resource *)src);
    if (g_av_cap.nv12) {
        ID3D11Texture2D *c = (ID3D11Texture2D *)sg_d3d11_query_image_info(g_av_cap.c_img).tex2d;
        if (c) ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)g_av_cap.stage_c[idx],
                                                     (ID3D11Resource *)c);
    }
    /* Hand the command buffer to the GPU now. A windowed app gets this for
     * free from Present; headless there is no Present at all, so without it
     * the copy sits unsubmitted and the non-blocking map two frames later
     * still finds it unfinished. Measured: 30% of frames survived the map
     * without this, and all of them with it. Flush does not wait. */
    ID3D11DeviceContext_Flush(ctx);
#elif defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
    if (g_av_cap.fence[idx]) { glDeleteSync(g_av_cap.fence[idx]); g_av_cap.fence[idx] = 0; }
    if (g_av_cap.nv12) {
        const int w = g_av_cap.w, h = g_av_cap.h;
        glBindFramebuffer(GL_FRAMEBUFFER, g_av_cap.fbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, g_av_cap.pbo[idx]);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        av__cap_gl_read(g_av_cap.y_img, GL_RGBA, w / 4, h,     0);
        av__cap_gl_read(g_av_cap.c_img, GL_RGBA, w / 4, h / 2, (size_t)w * (size_t)h);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        g_av_cap.fence[idx] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        sg_reset_state_cache();
        return;
    }
    sg_gl_image_info gi = sg_gl_query_image_info(g_av_cap.color_img);
    if (gi.tex[gi.active_slot] == 0) return;
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
    if (!g_av_cap.nv12) {
        av__cap_copy_out((const uint8_t *)m.pData, m.RowPitch, dst);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_av_cap.stage[idx], 0);
        return true;
    }
    /* Both planes were copied together, so the chroma is at least as far along;
     * still, a map that would wait is a dropped frame, never a stall. */
    D3D11_MAPPED_SUBRESOURCE mc;
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_av_cap.stage_c[idx], 0,
                                 D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mc);
    bool ok = SUCCEEDED(hr);
    if (ok) {
        const uint32_t w = (uint32_t)g_av_cap.w;
        const int      h = g_av_cap.h;
        av__cap_copy_plane((const uint8_t *)m.pData,  m.RowPitch,  dst, w, h);
        av__cap_copy_plane((const uint8_t *)mc.pData, mc.RowPitch, dst + (size_t)w * (size_t)h,
                           w, h / 2);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_av_cap.stage_c[idx], 0);
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_av_cap.stage[idx], 0);
    return ok;

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
    if (ok && g_av_cap.nv12) memcpy(dst, p, g_av_cap.bytes);   /* both planes, in order */
    else if (ok)             av__cap_copy_out(p, (uint32_t)g_av_cap.w * 4u, dst);
    if (p) glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    sg_reset_state_cache();
    return ok;

#else
    (void)idx; (void)dst;
    return false;
#endif
}

/* ---- NV12 conversion and the test card ---------------------------------- */

/*
 * One fragment program, three uses, picked by a define:
 *   (none)  Y:    a texel holds the luma of source pixels 4x .. 4x+3 of its row
 *   CHROMA  CbCr: a texel holds Cb,Cr of the 2x2 blocks at 4x and 4x+2, rows 2y, 2y+1
 *   CARD    copy: the test card into the capture target
 * BT.709, limited range: Y = 16 + 219 Y', C = 128 + 224 C'. Pure red is Y 63,
 * Cb 102, Cr 240. The chroma is the mean of the 2x2 block before the matrix;
 * the matrix is linear, so that is the mean of the four pixels' chroma.
 *
 * Sources are fetched by texel, never filtered. On GL a target's texel row 0 is
 * the picture's BOTTOM row (that is why the BGRA path flips on readback), so
 * FLIP reads picture row y from texel row h-1-y. That leaves the planes top-row
 * first, and the card copy bottom-row first like everything else drawn there.
 */
static const char *av__cap_fs_hlsl =
    "Texture2D<float4> tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "static const float3 KY = float3(0.2126, 0.7152, 0.0722);\n"
    "float3 px(int x, int y) { return tex.Load(int3(x, y, 0)).rgb; }\n"
    "float luma(float3 c) { return (16.0 + 219.0 * dot(c, KY)) / 255.0; }\n"
    "float2 chroma(float3 c) {\n"
    "  float y = dot(c, KY);\n"
    "  return (128.0 + 224.0 * float2((c.b - y) / 1.8556, (c.r - y) / 1.5748)) / 255.0;\n"
    "}\n"
    "float3 box(int x, int y) {\n"
    "  return 0.25 * (px(x, y) + px(x + 1, y) + px(x, y + 1) + px(x + 1, y + 1));\n"
    "}\n"
    "float4 main(float4 pos : SV_Position) : SV_Target0 {\n"
    "  int2 o = int2(pos.xy);\n"
    "#if CHROMA\n"
    "  int x = o.x * 4, y = o.y * 2;\n"
    "  return float4(chroma(box(x, y)), chroma(box(x + 2, y)));\n"
    "#elif CARD\n"
    "  return float4(px(o.x, o.y), 1.0);\n"
    "#else\n"
    "  int x = o.x * 4;\n"
    "  return float4(luma(px(x, o.y)), luma(px(x + 1, o.y)),\n"
    "                luma(px(x + 2, o.y)), luma(px(x + 3, o.y)));\n"
    "#endif\n"
    "}\n";

static const char *av__cap_fs_glsl =
    "uniform sampler2D tex_smp;\n"
    "out vec4 frag_color;\n"
    "const vec3 KY = vec3(0.2126, 0.7152, 0.0722);\n"
    "vec3 px(int x, int y) {\n"
    "#ifdef FLIP\n"
    "  y = textureSize(tex_smp, 0).y - 1 - y;\n"
    "#endif\n"
    "  return texelFetch(tex_smp, ivec2(x, y), 0).rgb;\n"
    "}\n"
    "float luma(vec3 c) { return (16.0 + 219.0 * dot(c, KY)) / 255.0; }\n"
    "vec2 chroma(vec3 c) {\n"
    "  float y = dot(c, KY);\n"
    "  return (128.0 + 224.0 * vec2((c.b - y) / 1.8556, (c.r - y) / 1.5748)) / 255.0;\n"
    "}\n"
    "vec3 box(int x, int y) {\n"
    "  return 0.25 * (px(x, y) + px(x + 1, y) + px(x, y + 1) + px(x + 1, y + 1));\n"
    "}\n"
    "void main() {\n"
    "  ivec2 o = ivec2(gl_FragCoord.xy);\n"
    "#if defined(CHROMA)\n"
    "  int x = o.x * 4, y = o.y * 2;\n"
    "  frag_color = vec4(chroma(box(x, y)), chroma(box(x + 2, y)));\n"
    "#elif defined(CARD)\n"
    "  frag_color = vec4(px(o.x, o.y), 1.0);\n"
    "#else\n"
    "  int x = o.x * 4;\n"
    "  frag_color = vec4(luma(px(x, o.y)), luma(px(x + 1, o.y)),\n"
    "                    luma(px(x + 2, o.y)), luma(px(x + 3, o.y)));\n"
    "#endif\n"
    "}\n";

/* A triangle that covers the target, from the vertex index: no vertex buffer. */
static const char *av__cap_vs_hlsl =
    "float4 main(uint vid : SV_VertexID) : SV_Position {\n"
    "  return float4((float)((vid & 1) << 2) - 1.0, (float)((vid & 2) << 1) - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *av__cap_vs_glsl =
    "void main() {\n"
    "  gl_Position = vec4(float((gl_VertexID & 1) << 2) - 1.0,\n"
    "                     float((gl_VertexID & 2) << 1) - 1.0, 0.0, 1.0);\n"
    "}\n";

/* `define` is "CHROMA", "CARD" or NULL. */
static inline sg_shader av__cap_shader(const char *define, const char *label) {
    static char vs[1024], fs[4096];
    const sg_backend backend = sg_query_backend();
    char def[64] = "";
    sg_shader_desc d;
    memset(&d, 0, sizeof d);

    if (backend == SG_BACKEND_GLCORE || backend == SG_BACKEND_GLES3) {
        const char *head = backend == SG_BACKEND_GLES3
            ? "#version 300 es\nprecision highp float;\nprecision highp int;\n"
              "precision highp sampler2D;\n"
            : "#version 410\n";
        if (define) snprintf(def, sizeof def, "#define %s 1\n", define);
        snprintf(vs, sizeof vs, "%s%s", head, av__cap_vs_glsl);
        snprintf(fs, sizeof fs, "%s#define FLIP 1\n%s%s", head, def, av__cap_fs_glsl);
        d.vertex_func.source   = vs;
        d.fragment_func.source = fs;
    } else if (backend == SG_BACKEND_D3D11) {
        if (define) snprintf(def, sizeof def, "#define %s 1\n", define);
        snprintf(fs, sizeof fs, "%s%s", def, av__cap_fs_hlsl);
        d.vertex_func.source         = av__cap_vs_hlsl;
        d.vertex_func.d3d11_target   = "vs_4_0";
        d.fragment_func.source       = fs;
        d.fragment_func.d3d11_target = "ps_4_0";
    }
    d.views[0].texture.stage             = SG_SHADERSTAGE_FRAGMENT;
    d.views[0].texture.image_type        = SG_IMAGETYPE_2D;
    d.views[0].texture.sample_type       = SG_IMAGESAMPLETYPE_FLOAT;
    d.views[0].texture.hlsl_register_t_n = 0;
    d.samplers[0].stage             = SG_SHADERSTAGE_FRAGMENT;
    d.samplers[0].sampler_type      = SG_SAMPLERTYPE_NONFILTERING;
    d.samplers[0].hlsl_register_s_n = 0;
    d.texture_sampler_pairs[0].stage        = SG_SHADERSTAGE_FRAGMENT;
    d.texture_sampler_pairs[0].view_slot    = 0;
    d.texture_sampler_pairs[0].sampler_slot = 0;
    d.texture_sampler_pairs[0].glsl_name    = "tex_smp";
    d.label = label;
    return sg_make_shader(&d);   /* compiles now, so the buffers may be reused */
}

static inline sg_pipeline av__cap_pipeline(sg_shader shd, sg_pixel_format fmt, const char *label) {
    sg_pipeline_desc p;
    memset(&p, 0, sizeof p);
    p.shader               = shd;
    p.primitive_type       = SG_PRIMITIVETYPE_TRIANGLES;
    p.colors[0].pixel_format = fmt;
    p.depth.pixel_format   = SG_PIXELFORMAT_NONE;
    p.sample_count         = 1;
    p.label                = label;
    return sg_make_pipeline(&p);
}

/* Draw `src` through `pip` into `att`, covering it. Outside any pass. */
static inline void av__cap_run(sg_pipeline pip, sg_view src, sg_view att) {
    sg_begin_pass(&(sg_pass){
        .action      = { .colors[0] = { .load_action  = SG_LOADACTION_DONTCARE,
                                        .store_action = SG_STOREACTION_STORE } },
        .attachments = { .colors[0] = att },
    });
    sg_apply_pipeline(pip);
    sg_apply_bindings(&(sg_bindings){ .views[0] = src, .samplers[0] = g_av_cap.smp });
    sg_draw(0, 3, 1);
    sg_end_pass();
}

/* The test card, top row first, RGBA8: eight full-strength bars (white,
 * yellow, cyan, green, magenta, red, blue, black) over the top two thirds and
 * a grey ramp, black to white, along the bottom third. A picture that comes
 * out upside down shows the ramp at the top. */
static inline uint8_t *av__cap_card_pixels(int w, int h) {
    static const uint8_t bars[8][3] = {
        { 255, 255, 255 }, { 255, 255, 0 }, { 0, 255, 255 }, { 0, 255, 0 },
        { 255, 0, 255 },   { 255, 0, 0 },   { 0, 0, 255 },   { 0, 0, 0 },
    };
    uint8_t *px = (uint8_t *)malloc((size_t)w * (size_t)h * 4u);
    if (!px) return NULL;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *p = px + ((size_t)y * (size_t)w + (size_t)x) * 4u;
            if (y < h * 2 / 3) {
                const uint8_t *c = bars[x * 8 / w];
                p[0] = c[0]; p[1] = c[1]; p[2] = c[2];
            } else {
                p[0] = p[1] = p[2] = (uint8_t)(x * 255 / (w - 1));
            }
            p[3] = 255;
        }
    }
    return px;
}

/* Everything the NV12 path and the card draw with. After the capture target. */
static inline bool av__cap_make_convert(sg_pixel_format cfmt) {
    g_av_cap.smp = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_NEAREST, .mag_filter = SG_FILTER_NEAREST,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE, .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
        .label = "av-convert-smp" });
    if (sg_query_sampler_state(g_av_cap.smp) != SG_RESOURCESTATE_VALID) return false;

    if (g_av_cap.nv12) {
        const int w = g_av_cap.w, h = g_av_cap.h;
        g_av_cap.y_img = sg_make_image(&(sg_image_desc){
            .usage.color_attachment = true, .width = w / 4, .height = h,
            .pixel_format = SG_PIXELFORMAT_RGBA8, .sample_count = 1, .label = "av-y" });
        g_av_cap.c_img = sg_make_image(&(sg_image_desc){
            .usage.color_attachment = true, .width = w / 4, .height = h / 2,
            .pixel_format = SG_PIXELFORMAT_RGBA8, .sample_count = 1, .label = "av-cbcr" });
        g_av_cap.y_att = sg_make_view(&(sg_view_desc){
            .color_attachment.image = g_av_cap.y_img, .label = "av-y-att" });
        g_av_cap.c_att = sg_make_view(&(sg_view_desc){
            .color_attachment.image = g_av_cap.c_img, .label = "av-cbcr-att" });
        g_av_cap.y_shd = av__cap_shader(NULL, "av-y-shader");
        g_av_cap.c_shd = av__cap_shader("CHROMA", "av-cbcr-shader");
        g_av_cap.y_pip = av__cap_pipeline(g_av_cap.y_shd, SG_PIXELFORMAT_RGBA8, "av-y-pip");
        g_av_cap.c_pip = av__cap_pipeline(g_av_cap.c_shd, SG_PIXELFORMAT_RGBA8, "av-cbcr-pip");
        if (sg_query_view_state(g_av_cap.y_att)     != SG_RESOURCESTATE_VALID ||
            sg_query_view_state(g_av_cap.c_att)     != SG_RESOURCESTATE_VALID ||
            sg_query_pipeline_state(g_av_cap.y_pip) != SG_RESOURCESTATE_VALID ||
            sg_query_pipeline_state(g_av_cap.c_pip) != SG_RESOURCESTATE_VALID) {
            LOG_ERROR("av: could not create the NV12 conversion passes");
            return false;
        }
    }

    if (g_av_cap.want_card) {
        uint8_t *px = av__cap_card_pixels(g_av_cap.w, g_av_cap.h);
        if (!px) return false;
        sg_image_desc id = {
            .width = g_av_cap.w, .height = g_av_cap.h,
            .pixel_format = SG_PIXELFORMAT_RGBA8, .label = "av-card" };
        id.data.mip_levels[0] = (sg_range){ px, (size_t)g_av_cap.w * (size_t)g_av_cap.h * 4u };
        g_av_cap.card_img = sg_make_image(&id);
        free(px);
        g_av_cap.card_tex = sg_make_view(&(sg_view_desc){
            .texture.image = g_av_cap.card_img, .label = "av-card-tex" });
        g_av_cap.card_shd = av__cap_shader("CARD", "av-card-shader");
        g_av_cap.card_pip = av__cap_pipeline(g_av_cap.card_shd, cfmt, "av-card-pip");
        if (sg_query_view_state(g_av_cap.card_tex)     != SG_RESOURCESTATE_VALID ||
            sg_query_pipeline_state(g_av_cap.card_pip) != SG_RESOURCESTATE_VALID) {
            LOG_ERROR("av: could not create the test card");
            return false;
        }
    }
    return true;
}

/* Ask for the test card in place of the game. Before av_capture_init. */
static inline void av_capture_set_test_card(bool on) { g_av_cap.want_card = on; }

/* ---- Target -------------------------------------------------------------- */

static inline void av_capture_shutdown(void) {
    av__cap_free_stage();
    /* sokol ignores a destroy of a handle that was never made. */
    sg_destroy_pipeline(g_av_cap.card_pip);
    sg_destroy_shader(g_av_cap.card_shd);
    sg_destroy_view(g_av_cap.card_tex);
    sg_destroy_image(g_av_cap.card_img);
    sg_destroy_pipeline(g_av_cap.y_pip);
    sg_destroy_pipeline(g_av_cap.c_pip);
    sg_destroy_shader(g_av_cap.y_shd);
    sg_destroy_shader(g_av_cap.c_shd);
    sg_destroy_view(g_av_cap.y_att);
    sg_destroy_view(g_av_cap.c_att);
    sg_destroy_image(g_av_cap.y_img);
    sg_destroy_image(g_av_cap.c_img);
    sg_destroy_sampler(g_av_cap.smp);
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
    bool want_card = g_av_cap.want_card;
    memset(&g_av_cap, 0, sizeof g_av_cap);
    g_av_cap.want_card = want_card;
    g_av_cap.w     = w;
    g_av_cap.h     = h;
    g_av_cap.nv12  = av_stream_format() == AV_FORMAT_NV12;
    g_av_cap.bytes = g_av_cap.nv12 ? (uint32_t)w * (uint32_t)h * 3u / 2u
                                   : (uint32_t)w * (uint32_t)h * 4u;
    if (g_av_cap.nv12 && ((w & 3) || (h & 1))) {
        LOG_ERROR("av: NV12 needs a width that is a multiple of 4 and an even height (%dx%d)", w, h);
        return false;
    }

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
    if (!av__cap_make_convert(cfmt)) { av_capture_shutdown(); return false; }
    if (!av__cap_make_stage())       { av_capture_shutdown(); return false; }

    g_av_cap.action = (sg_pass_action){
        .colors[0] = { .load_action = SG_LOADACTION_CLEAR,
                       .clear_value = { 0.0f, 0.0f, 0.0f, 1.0f } },
        .depth     = { .load_action = SG_LOADACTION_CLEAR, .clear_value = 1.0f },
    };
    g_av_cap.ready = true;
    LOG_INFO("av: capture target %dx%d, %d staging copies, %s%s",
             w, h, AV_CAP_SLOTS,
             g_av_cap.nv12    ? "NV12 converted on the GPU" :
             g_av_cap.swap_rb ? "channels swapped on readback" : "BGRA straight through",
             g_av_cap.card_img.id ? ", test card in place of the game" : "");
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

    /* The card goes over whatever was drawn; the conversion then reads the
     * finished target, overlay and all. */
    if (g_av_cap.card_img.id)
        av__cap_run(g_av_cap.card_pip, g_av_cap.card_tex, g_av_cap.color_att);
    if (g_av_cap.nv12) {
        av__cap_run(g_av_cap.y_pip, g_av_cap.color_tex, g_av_cap.y_att);
        av__cap_run(g_av_cap.c_pip, g_av_cap.color_tex, g_av_cap.c_att);
    }

    uint32_t idx = (uint32_t)(g_av_cap.submitted % AV_CAP_SLOTS);
    g_av_cap.tag_frame[idx]  = frame;
    g_av_cap.tag_sample[idx] = sample;
    av__cap_issue(idx);
    g_av_cap.submitted++;
}

#endif /* AV_CAPTURE_H */
