/*
 * gfx_readback.h — pull a colour attachment sokol_gfx has just rendered into
 * back to host memory, and write host pixels out as a PNG.
 *
 * sokol_gfx has no readback call of its own, so this reaches through its
 * backend interop (sg_d3d11_* / sg_gl_*) for the native texture behind an
 * sg_image. D3D11 and the two GL backends are implemented; Metal and WebGPU
 * are not, and say so rather than handing back a blank image — a screenshot
 * that is quietly black is worse than one that did not happen.
 *
 * Output is always 8-bit RGBA, top row first, alpha forced opaque: the format
 * miniz's PNG writer wants, and the one an image viewer will not argue with.
 */
#ifndef GFX_READBACK_H
#define GFX_READBACK_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sokol_gfx.h"
#include "miniz.h"

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

/* Why a readback could not happen, for the caller to pass on. Static storage:
 * one readback is in flight at a time, on the render thread. */
static char g_gfx_readback_err[128];

static inline const char *gfx_readback_error(void) {
    return g_gfx_readback_err[0] ? g_gfx_readback_err : "unknown";
}

/* True where this build can read an attachment back at all. Check it before
 * offering a screenshot rather than after taking one. */
static inline bool gfx_readback_supported(void) {
#if defined(SOKOL_D3D11) || defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
    return true;
#else
    return false;
#endif
}

/*
 * Read `img` (w x h, a colour attachment of a pass that has already ended)
 * into `out` — w*h*4 bytes, RGBA, top row first.
 *
 * The call blocks until the GPU has finished with the attachment: both
 * backends' paths map or read straight after the copy, which is a full stall.
 * That is the point — a debug screenshot wants the pixels now, not next frame.
 */
static inline bool gfx_readback_rgba8(sg_image img, int w, int h, uint8_t *out) {
    g_gfx_readback_err[0] = '\0';
    if (w <= 0 || h <= 0 || !out) {
        snprintf(g_gfx_readback_err, sizeof g_gfx_readback_err, "bad readback size %dx%d", w, h);
        return false;
    }

#if defined(SOKOL_D3D11)
    ID3D11Device        *dev = (ID3D11Device *)sg_d3d11_device();
    ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)sg_d3d11_device_context();
    sg_d3d11_image_info  info = sg_d3d11_query_image_info(img);
    ID3D11Texture2D     *src = (ID3D11Texture2D *)info.tex2d;
    if (!dev || !ctx || !src) {
        snprintf(g_gfx_readback_err, sizeof g_gfx_readback_err, "no D3D11 texture behind the image");
        return false;
    }

    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(src, &desc);
    /* A staging copy is the only resource the CPU may map. Everything else is
     * carried over so the copy is format- and size-compatible with the source. */
    desc.Usage          = D3D11_USAGE_STAGING;
    desc.BindFlags      = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags      = 0;

    ID3D11Texture2D *stage = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &desc, NULL, &stage)) || !stage) {
        snprintf(g_gfx_readback_err, sizeof g_gfx_readback_err, "CreateTexture2D(staging) failed");
        return false;
    }
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stage, (ID3D11Resource *)src);

    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stage, 0, D3D11_MAP_READ, 0, &map))) {
        ID3D11Texture2D_Release(stage);
        snprintf(g_gfx_readback_err, sizeof g_gfx_readback_err, "Map(staging) failed");
        return false;
    }

    /* The swapchain's format on this backend is BGRA8, and the offscreen target
     * matches it so the game's own pipelines stay compatible — so the channels
     * usually need swapping on the way out. */
    const bool bgra = (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                       desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    for (int y = 0; y < h; y++) {
        const uint8_t *s = (const uint8_t *)map.pData + (size_t)y * map.RowPitch;
        uint8_t       *d = out + (size_t)y * (size_t)w * 4;
        for (int x = 0; x < w; x++) {
            d[x * 4 + 0] = bgra ? s[x * 4 + 2] : s[x * 4 + 0];
            d[x * 4 + 1] = s[x * 4 + 1];
            d[x * 4 + 2] = bgra ? s[x * 4 + 0] : s[x * 4 + 2];
            d[x * 4 + 3] = 255;
        }
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stage, 0);
    ID3D11Texture2D_Release(stage);
    return true;

#elif defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
    sg_gl_image_info gi = sg_gl_query_image_info(img);
    if (gi.tex[gi.active_slot] == 0) {
        snprintf(g_gfx_readback_err, sizeof g_gfx_readback_err, "no GL texture behind the image");
        return false;
    }
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           gi.tex[gi.active_slot], 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out);
    } else {
        snprintf(g_gfx_readback_err, sizeof g_gfx_readback_err, "readback FBO incomplete");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    sg_reset_state_cache();     /* sokol caches GL state; we changed it behind its back */
    if (!ok) return false;

    /* GL hands back the bottom row first. Flip in place, row by row. */
    static uint8_t row[4 * 4096];
    const size_t stride = (size_t)w * 4;
    if (stride <= sizeof row) {
        for (int y = 0; y < h / 2; y++) {
            uint8_t *a = out + (size_t)y * stride;
            uint8_t *b = out + (size_t)(h - 1 - y) * stride;
            memcpy(row, a, stride); memcpy(a, b, stride); memcpy(b, row, stride);
        }
    }
    for (size_t i = 3; i < (size_t)w * (size_t)h * 4; i += 4) out[i] = 255;
    return true;

#else
    (void)img;
    snprintf(g_gfx_readback_err, sizeof g_gfx_readback_err,
             "no readback path for this sokol backend (Metal / WebGPU)");
    return false;
#endif
}

/*
 * Encode w*h RGBA bytes as a PNG. Returns a buffer the caller frees with
 * gfx_free_png, or NULL, with the length in *out_len.
 *
 * Kept separate from writing it because the browser build has nowhere to write:
 * there the encoded bytes are handed to JavaScript and turned into a Blob.
 */
static inline void *gfx_encode_png(const uint8_t *rgba, int w, int h, size_t *out_len) {
    g_gfx_readback_err[0] = 0;
    *out_len = 0;
    size_t len = 0;
    void  *png = tdefl_write_image_to_png_file_in_memory_ex((void *)rgba, w, h, 4, &len, 6, MZ_TRUE);
    if (!png) {
        snprintf(g_gfx_readback_err, sizeof g_gfx_readback_err, "PNG encode failed");
        return NULL;
    }
    *out_len = len;
    return png;
}

static inline void gfx_free_png(void *png) { if (png) mz_free(png); }

/* Write an encoded PNG out. Returns its size, or 0. */
static inline size_t gfx_save_png(const char *path, const void *png, size_t len) {
    g_gfx_readback_err[0] = 0;
    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(g_gfx_readback_err, sizeof g_gfx_readback_err, "cannot open '%s' for writing", path);
        return 0;
    }
    size_t wrote = fwrite(png, 1, len, f);
    fclose(f);
    if (wrote != len) {
        snprintf(g_gfx_readback_err, sizeof g_gfx_readback_err, "short write to '%s'", path);
        return 0;
    }
    return len;
}

#endif /* GFX_READBACK_H */
