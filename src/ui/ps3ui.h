/*
 * ps3ui.h -- a small software compositor for the PS3-style lobby.
 *
 * The PS3 port of Sonic the Fighters (NPUB30927) draws its menus with the
 * Project DIVA engine: AET compositions (After Effects layers with keyframed
 * anchor/position/rotation/scale/opacity, normal or additive blend) that place
 * sprites authored for a 1920x1080 canvas, plus text the program draws into
 * empty "placeholder" layers. This file is that machinery, rebuilt:
 *
 *   - an RGB float canvas at any output size, addressed in 1080p units
 *     (ps3ui_canvas_t.k = output height / 1080)
 *   - sprite images (straight alpha, bilinear-sampled like the PS3's GPU)
 *   - a polygon rasteriser (nonzero, 4x4 supersampled) the sprite painters use
 *   - text through stb_truetype with the open fonts in ps3ui_fonts.h
 *   - an AET player over the layout tables in ps3ui_layout.h
 *
 * Nothing here is Sega's: the sprites are painted by code (ps3ui_sprites.h)
 * and the fonts are OFL. What is the PS3's is the layout -- positions, timing,
 * colours -- measured from the game's own data.
 *
 * Everything runs on the CPU so every frontend (libretro GL/GLES, the web
 * build's WebGL, D3D11) just uploads ps3ui_canvas_t.rgba as a texture, and a
 * test can render a screen to a PNG with no GPU at all.
 */
#ifndef PS3UI_H
#define PS3UI_H

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef PS3UI_MAX_GLYPH_CACHE
/* Big enough that one frame's text never evicts a glyph the frame drew: a
 * recording canvas hands the GPU pointers into this cache. */
#define PS3UI_MAX_GLYPH_CACHE 2048
#endif

/* ---------------------------------------------------------------------------
 * Images
 * ------------------------------------------------------------------------- */

typedef struct {
    int w, h;
    float *px;              /* RGBA, straight alpha, 0..1 */
} ps3ui_image_t;

static inline ps3ui_image_t ps3ui_image_new(int w, int h)
{
    ps3ui_image_t im = { w, h, (float *)calloc((size_t)w * (size_t)h * 4, sizeof(float)) };
    return im;
}

static inline void ps3ui_image_free(ps3ui_image_t *im)
{
    free(im->px);
    im->px = NULL;
    im->w = im->h = 0;
}

static inline float *ps3ui_px(ps3ui_image_t *im, int x, int y)
{
    return im->px + ((size_t)y * (size_t)im->w + (size_t)x) * 4;
}

static inline void ps3ui_rgb_hex(uint32_t hex, float out[3])
{
    out[0] = (float)((hex >> 16) & 0xFF) / 255.0f;
    out[1] = (float)((hex >> 8) & 0xFF) / 255.0f;
    out[2] = (float)(hex & 0xFF) / 255.0f;
}

/* Paint one pixel of an image "over" what is there (straight alpha). */
static inline void ps3ui_image_over(ps3ui_image_t *im, int x, int y, const float rgb[3], float a)
{
    if (x < 0 || y < 0 || x >= im->w || y >= im->h || a <= 0.0f)
        return;
    float *p = ps3ui_px(im, x, y);
    float da = p[3] * (1.0f - a);
    float oa = a + da;
    for (int c = 0; c < 3; c++)
        p[c] = oa > 0.0f ? (rgb[c] * a + p[c] * da) / oa : 0.0f;
    p[3] = oa;
}

/* ---------------------------------------------------------------------------
 * Polygon coverage: nonzero winding, 4x4 samples per pixel. Sprites are
 * painted once, at start-up, so exactness matters more than speed.
 * ------------------------------------------------------------------------- */

#define PS3UI_PATH_MAX 1024

typedef struct {
    float x[PS3UI_PATH_MAX], y[PS3UI_PATH_MAX];
    int start[64];          /* first point of each contour */
    int n, contours;
} ps3ui_path_t;

static inline void ps3ui_path_reset(ps3ui_path_t *p) { p->n = 0; p->contours = 0; }

static inline void ps3ui_path_move(ps3ui_path_t *p, float x, float y)
{
    if (p->contours < 64 && p->n < PS3UI_PATH_MAX) {
        p->start[p->contours++] = p->n;
        p->x[p->n] = x;
        p->y[p->n] = y;
        p->n++;
    }
}

static inline void ps3ui_path_line(ps3ui_path_t *p, float x, float y)
{
    if (p->n < PS3UI_PATH_MAX) {
        p->x[p->n] = x;
        p->y[p->n] = y;
        p->n++;
    }
}

static inline void ps3ui_path_quad(ps3ui_path_t *p, float cx, float cy, float x, float y)
{
    float x0 = p->x[p->n - 1], y0 = p->y[p->n - 1];
    for (int i = 1; i <= 8; i++) {
        float t = (float)i / 8.0f, u = 1.0f - t;
        ps3ui_path_line(p, u * u * x0 + 2 * u * t * cx + t * t * x, u * u * y0 + 2 * u * t * cy + t * t * y);
    }
}

static inline void ps3ui_path_rect(ps3ui_path_t *p, float x0, float y0, float x1, float y1)
{
    ps3ui_path_move(p, x0, y0);
    ps3ui_path_line(p, x1, y0);
    ps3ui_path_line(p, x1, y1);
    ps3ui_path_line(p, x0, y1);
}

/* An arc from angle a0 to a1 (radians, y down so positive is clockwise). */
static inline void ps3ui_path_arc(ps3ui_path_t *p, float cx, float cy, float r, float a0, float a1, int steps)
{
    for (int i = 0; i <= steps; i++) {
        float a = a0 + (a1 - a0) * (float)i / (float)steps;
        ps3ui_path_line(p, cx + r * cosf(a), cy + r * sinf(a));
    }
}

static inline int ps3ui_path_winding(const ps3ui_path_t *p, float sx, float sy)
{
    int w = 0;
    for (int c = 0; c < p->contours; c++) {
        int a = p->start[c], b = c + 1 < p->contours ? p->start[c + 1] : p->n;
        for (int i = a; i < b; i++) {
            int j = i + 1 < b ? i + 1 : a;
            float x0 = p->x[i], y0 = p->y[i], x1 = p->x[j], y1 = p->y[j];
            if (y0 <= sy) {
                if (y1 > sy && (x1 - x0) * (sy - y0) - (sx - x0) * (y1 - y0) > 0)
                    w++;
            } else if (y1 <= sy && (x1 - x0) * (sy - y0) - (sx - x0) * (y1 - y0) < 0) {
                w--;
            }
        }
    }
    return w;
}

/* Coverage of the path into mask (w*h floats, 0..1, max-combined). */
static inline void ps3ui_path_cover(const ps3ui_path_t *p, float *mask, int w, int h)
{
    if (p->n == 0)
        return;
    float minx = p->x[0], maxx = p->x[0], miny = p->y[0], maxy = p->y[0];
    for (int i = 1; i < p->n; i++) {
        if (p->x[i] < minx) minx = p->x[i];
        if (p->x[i] > maxx) maxx = p->x[i];
        if (p->y[i] < miny) miny = p->y[i];
        if (p->y[i] > maxy) maxy = p->y[i];
    }
    int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx), y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            int hit = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++)
                    hit += ps3ui_path_winding(p, (float)x + 0.125f + 0.25f * (float)sx,
                                              (float)y + 0.125f + 0.25f * (float)sy) != 0;
            float c = (float)hit / 16.0f;
            if (c > mask[y * w + x])
                mask[y * w + x] = c;
        }
}

/* Fill a path into an image with one colour. */
static inline void ps3ui_path_fill(ps3ui_image_t *im, const ps3ui_path_t *p, uint32_t rgb, float a)
{
    float *m = (float *)calloc((size_t)im->w * (size_t)im->h, sizeof(float));
    float c[3];
    ps3ui_rgb_hex(rgb, c);
    ps3ui_path_cover(p, m, im->w, im->h);
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++)
            ps3ui_image_over(im, x, y, c, a * m[y * im->w + x]);
    free(m);
}

/* Separable Gaussian blur of a mask, in place (used for glows). */
static inline void ps3ui_blur(float *m, int w, int h, float sigma)
{
    int r = (int)ceilf(sigma * 3.0f);
    float *k = (float *)malloc(sizeof(float) * (size_t)(2 * r + 1));
    float *t = (float *)malloc(sizeof(float) * (size_t)w * (size_t)h);
    float sum = 0.0f;
    for (int i = -r; i <= r; i++)
        sum += k[i + r] = expf(-(float)(i * i) / (2.0f * sigma * sigma));
    for (int i = 0; i <= 2 * r; i++)
        k[i] /= sum;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float s = 0.0f;
            for (int i = -r; i <= r; i++) {
                int xx = x + i;
                if (xx >= 0 && xx < w)
                    s += m[y * w + xx] * k[i + r];
            }
            t[y * w + x] = s;
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float s = 0.0f;
            for (int i = -r; i <= r; i++) {
                int yy = y + i;
                if (yy >= 0 && yy < h)
                    s += t[yy * w + x] * k[i + r];
            }
            m[y * w + x] = s;
        }
    free(t);
    free(k);
}

/* ---------------------------------------------------------------------------
 * The draw list. A canvas with one records what it would have drawn, as
 * textured quads in output pixels, for a GPU to draw (ps3ui_gpu.h): the PS3
 * drew its sprites on the GPU with bilinear sampling and the same two blend
 * states, so this is the faithful path as well as the fast one. Without one,
 * the canvas rasterises on the CPU with the same arithmetic (the tests' path).
 * ------------------------------------------------------------------------- */

enum { PS3UI_DL_IMAGE, PS3UI_DL_RECT, PS3UI_DL_GLYPH };

typedef struct {
    uint8_t kind, blend;
    const void *src;        /* IMAGE: the ps3ui_image_t; GLYPH: its bitmap */
    int sw, sh;             /* GLYPH: bitmap size */
    uint64_t key;           /* GLYPH: identity of the bitmap, for an atlas */
    float x[4], y[4];       /* corners in output pixels: (0,0) (w,0) (0,h) (w,h) of the source */
    float r, g, b, a;       /* tint and opacity */
} ps3ui_dl_item_t;

typedef struct {
    ps3ui_dl_item_t *items;
    int n, cap;
} ps3ui_dl_t;

static inline ps3ui_dl_item_t *ps3ui_dl_push(ps3ui_dl_t *dl)
{
    if (dl->n == dl->cap) {
        int cap = dl->cap ? dl->cap * 2 : 1024;
        ps3ui_dl_item_t *p = (ps3ui_dl_item_t *)realloc(dl->items, sizeof(*p) * (size_t)cap);
        if (!p)
            return NULL;
        dl->items = p;
        dl->cap = cap;
    }
    ps3ui_dl_item_t *it = &dl->items[dl->n++];
    memset(it, 0, sizeof *it);
    it->r = it->g = it->b = 1.0f;
    return it;
}

/* ---------------------------------------------------------------------------
 * Canvas
 * ------------------------------------------------------------------------- */

typedef struct {
    int w, h;
    float k;                /* output pixels per 1080p unit */
    float ox, oy;           /* where the 1920x1080 frame's origin lands */
    float *rgb;             /* w*h*3 (CPU path) */
    uint8_t *rgba;          /* w*h*4, filled by ps3ui_canvas_resolve */
    int clip_x0, clip_y0, clip_x1, clip_y1;
    ps3ui_dl_t *dl;         /* set: record instead of drawing (ps3ui_gpu.h) */
} ps3ui_canvas_t;

/* Size the canvas. The 16:9 layout is fitted inside w x h and centred; the
 * margins, if any, are the background's to fill. */
static inline void ps3ui_canvas_size(ps3ui_canvas_t *cv, int w, int h)
{
    if (cv->dl) {
        cv->w = w;
        cv->h = h;
    } else if (cv->w != w || cv->h != h || !cv->rgb) {
        free(cv->rgb);
        free(cv->rgba);
        cv->rgb = (float *)calloc((size_t)w * (size_t)h * 3, sizeof(float));
        cv->rgba = (uint8_t *)calloc((size_t)w * (size_t)h * 4, 1);
        cv->w = w;
        cv->h = h;
    }
    float kx = (float)w / 1920.0f, ky = (float)h / 1080.0f;
    cv->k = kx < ky ? kx : ky;
    cv->ox = ((float)w - 1920.0f * cv->k) * 0.5f;
    cv->oy = ((float)h - 1080.0f * cv->k) * 0.5f;
    cv->clip_x0 = 0;
    cv->clip_y0 = 0;
    cv->clip_x1 = w;
    cv->clip_y1 = h;
}

static inline void ps3ui_canvas_free(ps3ui_canvas_t *cv)
{
    free(cv->rgb);
    free(cv->rgba);
    memset(cv, 0, sizeof(*cv));
}

static inline void ps3ui_canvas_resolve(ps3ui_canvas_t *cv)
{
    size_t n = (size_t)cv->w * (size_t)cv->h;
    for (size_t i = 0; i < n; i++) {
        for (int c = 0; c < 3; c++) {
            float v = cv->rgb[i * 3 + c];
            v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
            cv->rgba[i * 4 + c] = (uint8_t)(v * 255.0f + 0.5f);
        }
        cv->rgba[i * 4 + 3] = 255;
    }
}

/* A 2D affine map, 1080p layout units: x' = a*x + c*y + e, y' = b*x + d*y + f. */
typedef struct { float a, b, c, d, e, f; } ps3ui_mat_t;

static inline ps3ui_mat_t ps3ui_mat_id(void) { ps3ui_mat_t m = { 1, 0, 0, 1, 0, 0 }; return m; }

static inline ps3ui_mat_t ps3ui_mat_mul(ps3ui_mat_t p, ps3ui_mat_t q)
{
    ps3ui_mat_t r;
    r.a = p.a * q.a + p.c * q.b;
    r.b = p.b * q.a + p.d * q.b;
    r.c = p.a * q.c + p.c * q.d;
    r.d = p.b * q.c + p.d * q.d;
    r.e = p.a * q.e + p.c * q.f + p.e;
    r.f = p.b * q.e + p.d * q.f + p.f;
    return r;
}

static inline ps3ui_mat_t ps3ui_mat_translate(float x, float y) { ps3ui_mat_t m = { 1, 0, 0, 1, x, y }; return m; }
static inline ps3ui_mat_t ps3ui_mat_scale(float x, float y) { ps3ui_mat_t m = { x, 0, 0, y, 0, 0 }; return m; }

/* AE rotation: degrees, clockwise on screen (y down). */
static inline ps3ui_mat_t ps3ui_mat_rotate(float deg)
{
    float r = deg * 3.14159265358979f / 180.0f, c = cosf(r), s = sinf(r);
    ps3ui_mat_t m = { c, s, -s, c, 0, 0 };
    return m;
}

static inline void ps3ui_mat_apply(ps3ui_mat_t m, float x, float y, float *ox, float *oy)
{
    *ox = m.a * x + m.c * y + m.e;
    *oy = m.b * x + m.d * y + m.f;
}

/* Layout units -> canvas pixels. */
static inline ps3ui_mat_t ps3ui_canvas_mat(const ps3ui_canvas_t *cv)
{
    ps3ui_mat_t m = { cv->k, 0, 0, cv->k, cv->ox, cv->oy };
    return m;
}

enum { PS3UI_BLEND_NORMAL = 3, PS3UI_BLEND_ADD = 5 };

static inline void ps3ui_blend_px(float *d, const float *s, float a, int blend)
{
    if (blend == PS3UI_BLEND_ADD) {
        d[0] += s[0] * a;
        d[1] += s[1] * a;
        d[2] += s[2] * a;
        if (d[0] > 1.0f) d[0] = 1.0f;
        if (d[1] > 1.0f) d[1] = 1.0f;
        if (d[2] > 1.0f) d[2] = 1.0f;
    } else {
        d[0] += (s[0] - d[0]) * a;
        d[1] += (s[1] - d[1]) * a;
        d[2] += (s[2] - d[2]) * a;
    }
}

/* Draw an image whose texel (u,v) sits at layout point m*(u,v). Pixels whose
 * centres fall inside the transformed rectangle are shaded, as a GPU would
 * rasterise the quad; the texture is sampled bilinearly, clamped to the
 * sprite, with the half-texel convention. tint multiplies RGB (NULL = none). */
static inline void ps3ui_draw_image(ps3ui_canvas_t *cv, const ps3ui_image_t *im, ps3ui_mat_t m,
                                    float opacity, int blend, const float *tint)
{
    if (!im->px || opacity <= 0.0f)
        return;
    ps3ui_mat_t t = ps3ui_mat_mul(ps3ui_canvas_mat(cv), m);
    float det = t.a * t.d - t.b * t.c;
    if (fabsf(det) < 1e-9f)
        return;
    if (cv->dl) {
        ps3ui_dl_item_t *it = ps3ui_dl_push(cv->dl);
        if (!it)
            return;
        it->kind = PS3UI_DL_IMAGE;
        it->blend = (uint8_t)blend;
        it->src = im;
        ps3ui_mat_apply(t, 0, 0, &it->x[0], &it->y[0]);
        ps3ui_mat_apply(t, (float)im->w, 0, &it->x[1], &it->y[1]);
        ps3ui_mat_apply(t, 0, (float)im->h, &it->x[2], &it->y[2]);
        ps3ui_mat_apply(t, (float)im->w, (float)im->h, &it->x[3], &it->y[3]);
        if (tint) {
            it->r = tint[0];
            it->g = tint[1];
            it->b = tint[2];
        }
        it->a = opacity;
        return;
    }
    /* inverse */
    float ia = t.d / det, ib = -t.b / det, ic = -t.c / det, id = t.a / det;
    float ie = -(ia * t.e + ic * t.f), iff = -(ib * t.e + id * t.f);
    float xs[4], ys[4];
    ps3ui_mat_apply(t, 0, 0, &xs[0], &ys[0]);
    ps3ui_mat_apply(t, (float)im->w, 0, &xs[1], &ys[1]);
    ps3ui_mat_apply(t, 0, (float)im->h, &xs[2], &ys[2]);
    ps3ui_mat_apply(t, (float)im->w, (float)im->h, &xs[3], &ys[3]);
    float minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
    for (int i = 1; i < 4; i++) {
        if (xs[i] < minx) minx = xs[i];
        if (xs[i] > maxx) maxx = xs[i];
        if (ys[i] < miny) miny = ys[i];
        if (ys[i] > maxy) maxy = ys[i];
    }
    int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx), y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    if (x0 < cv->clip_x0) x0 = cv->clip_x0;
    if (y0 < cv->clip_y0) y0 = cv->clip_y0;
    if (x1 > cv->clip_x1) x1 = cv->clip_x1;
    if (y1 > cv->clip_y1) y1 = cv->clip_y1;
    const float W = (float)im->w, H = (float)im->h;
    for (int y = y0; y < y1; y++) {
        float py = (float)y + 0.5f;
        for (int x = x0; x < x1; x++) {
            float px = (float)x + 0.5f;
            float u = ia * px + ic * py + ie, v = ib * px + id * py + iff;
            if (u < 0.0f || v < 0.0f || u >= W || v >= H)
                continue;
            float fu = u - 0.5f, fv = v - 0.5f;
            int u0 = (int)floorf(fu), v0 = (int)floorf(fv);
            float du = fu - (float)u0, dv = fv - (float)v0;
            int ua = u0 < 0 ? 0 : u0, ub = u0 + 1 >= im->w ? im->w - 1 : u0 + 1;
            int va = v0 < 0 ? 0 : v0, vb = v0 + 1 >= im->h ? im->h - 1 : v0 + 1;
            if (ua >= im->w) ua = im->w - 1;
            if (va >= im->h) va = im->h - 1;
            if (ub < 0) ub = 0;
            if (vb < 0) vb = 0;
            const float *p00 = im->px + ((size_t)va * (size_t)im->w + (size_t)ua) * 4;
            const float *p10 = im->px + ((size_t)va * (size_t)im->w + (size_t)ub) * 4;
            const float *p01 = im->px + ((size_t)vb * (size_t)im->w + (size_t)ua) * 4;
            const float *p11 = im->px + ((size_t)vb * (size_t)im->w + (size_t)ub) * 4;
            float s[4];
            for (int c = 0; c < 4; c++) {
                float top = p00[c] + (p10[c] - p00[c]) * du;
                float bot = p01[c] + (p11[c] - p01[c]) * du;
                s[c] = top + (bot - top) * dv;
            }
            if (tint) {
                s[0] *= tint[0];
                s[1] *= tint[1];
                s[2] *= tint[2];
            }
            float a = s[3] * opacity;
            if (a <= 0.0f)
                continue;
            ps3ui_blend_px(cv->rgb + ((size_t)y * (size_t)cv->w + (size_t)x) * 3, s, a, blend);
        }
    }
}

/* A solid rectangle in output pixels. */
static inline void ps3ui_fill_px(ps3ui_canvas_t *cv, int px0, int py0, int px1, int py1, const float c[3], float a,
                                 int blend)
{
    if (px1 <= px0 || py1 <= py0 || a <= 0.0f)
        return;
    if (cv->dl) {
        ps3ui_dl_item_t *it = ps3ui_dl_push(cv->dl);
        if (!it)
            return;
        it->kind = PS3UI_DL_RECT;
        it->blend = (uint8_t)blend;
        it->x[0] = it->x[2] = (float)px0;
        it->x[1] = it->x[3] = (float)px1;
        it->y[0] = it->y[1] = (float)py0;
        it->y[2] = it->y[3] = (float)py1;
        it->r = c[0];
        it->g = c[1];
        it->b = c[2];
        it->a = a;
        return;
    }
    for (int y = py0; y < py1; y++)
        for (int x = px0; x < px1; x++)
            ps3ui_blend_px(cv->rgb + ((size_t)y * (size_t)cv->w + (size_t)x) * 3, c, a, blend);
}

/* Fill the whole output (the 16:9 frame and any margin). */
static inline void ps3ui_clear(ps3ui_canvas_t *cv, uint32_t hex)
{
    float c[3];
    ps3ui_rgb_hex(hex, c);
    ps3ui_fill_px(cv, 0, 0, cv->w, cv->h, c, 1.0f, PS3UI_BLEND_NORMAL);
}

/* A solid rectangle in layout units, snapped to pixel centres. */
static inline void ps3ui_fill_rect(ps3ui_canvas_t *cv, float x0, float y0, float x1, float y1,
                                   uint32_t hex, float a, int blend)
{
    float c[3];
    ps3ui_rgb_hex(hex, c);
    int px0 = (int)ceilf(x0 * cv->k + cv->ox - 0.5f), px1 = (int)ceilf(x1 * cv->k + cv->ox - 0.5f);
    int py0 = (int)ceilf(y0 * cv->k + cv->oy - 0.5f), py1 = (int)ceilf(y1 * cv->k + cv->oy - 0.5f);
    if (px0 < cv->clip_x0) px0 = cv->clip_x0;
    if (py0 < cv->clip_y0) py0 = cv->clip_y0;
    if (px1 > cv->clip_x1) px1 = cv->clip_x1;
    if (py1 > cv->clip_y1) py1 = cv->clip_y1;
    ps3ui_fill_px(cv, px0, py0, px1, py1, c, a, blend);
}

/* ---------------------------------------------------------------------------
 * AET player. The tables (ps3ui_layout.h) are generated from the layouts by
 * tools/ps3ui/gen_layout.py.
 * ------------------------------------------------------------------------- */

typedef struct { float frame, value, tangent; } ps3ui_key_t;

/* n == 0: constant `value`; otherwise keys[first .. first+n). */
typedef struct { uint16_t n, first; float value; } ps3ui_curve_t;

enum { PS3UI_CURVE_AX, PS3UI_CURVE_AY, PS3UI_CURVE_PX, PS3UI_CURVE_PY, PS3UI_CURVE_ROT,
       PS3UI_CURVE_SX, PS3UI_CURVE_SY, PS3UI_CURVE_OP, PS3UI_CURVES };

enum { PS3UI_LAYER_VIDEO = 1, PS3UI_LAYER_COMP = 3 };

typedef struct {
    const char *name;
    uint8_t type;           /* PS3UI_LAYER_* */
    uint8_t blend;          /* PS3UI_BLEND_* */
    uint8_t active;         /* flags bit 0 */
    uint8_t nsrc;           /* video: number of sprite sources (0 = placeholder) */
    float start, end, offset, tscale;
    uint16_t item;          /* video: first source in ps3ui_src[]; comp: comp index */
    uint16_t w, h;          /* video size (placeholders keep it: it is a text box) */
    float frames_per_src;
    ps3ui_curve_t c[PS3UI_CURVES];
} ps3ui_layer_t;

typedef struct {
    const char *name;
    uint16_t first, n;      /* layers[first .. first+n), index 0 on top */
} ps3ui_comp_t;

/* A window's timeline, from the markers on the layer that places it in its
 * scene: open sta_s..sta_e once, idle neu_s..neu_e looped, close end_s..end_e.
 * A looping effect (loop_s/loop_e) has only the idle part (the rest < 0). */
typedef struct {
    const char *name;
    float sta_s, sta_e, neu_s, neu_e, end_s, end_e;
} ps3ui_window_def_t;

typedef struct {
    const ps3ui_comp_t *comps;
    const ps3ui_layer_t *layers;
    const ps3ui_key_t *keys;
    const uint16_t *srcs;   /* sprite ids (ps3ui_sprites.h) */
    int ncomps;
    const ps3ui_window_def_t *wins;
    int nwins;
} ps3ui_scene_t;

/* The DIVA engine's keyframe interpolation: Hermite in frame units. */
static inline float ps3ui_curve_eval(const ps3ui_scene_t *s, const ps3ui_curve_t *c, float f)
{
    if (c->n == 0)
        return c->value;
    const ps3ui_key_t *k = s->keys + c->first;
    if (c->n == 1 || f <= k[0].frame)
        return k[0].value;
    if (f >= k[c->n - 1].frame)
        return k[c->n - 1].value;
    int i = 0;
    while (i + 1 < c->n && k[i + 1].frame <= f)
        i++;
    const ps3ui_key_t *a = &k[i], *b = &k[i + 1];
    float df = b->frame - a->frame;
    if (df <= 0.0f)
        return b->value;
    float t = (f - a->frame) / df, d = f - a->frame;
    return (a->tangent * (t - 1.0f) + b->tangent * t) * (t - 1.0f) * d
         + (2.0f * t - 3.0f) * t * t * (a->value - b->value) + a->value;
}

static inline ps3ui_mat_t ps3ui_layer_mat(const ps3ui_scene_t *s, const ps3ui_layer_t *l, float f)
{
    float ax = ps3ui_curve_eval(s, &l->c[PS3UI_CURVE_AX], f);
    float ay = ps3ui_curve_eval(s, &l->c[PS3UI_CURVE_AY], f);
    float px = ps3ui_curve_eval(s, &l->c[PS3UI_CURVE_PX], f);
    float py = ps3ui_curve_eval(s, &l->c[PS3UI_CURVE_PY], f);
    float rot = ps3ui_curve_eval(s, &l->c[PS3UI_CURVE_ROT], f);
    float sx = ps3ui_curve_eval(s, &l->c[PS3UI_CURVE_SX], f);
    float sy = ps3ui_curve_eval(s, &l->c[PS3UI_CURVE_SY], f);
    ps3ui_mat_t m = ps3ui_mat_translate(px, py);
    if (rot != 0.0f)
        m = ps3ui_mat_mul(m, ps3ui_mat_rotate(rot));
    m = ps3ui_mat_mul(m, ps3ui_mat_scale(sx, sy));
    return ps3ui_mat_mul(m, ps3ui_mat_translate(-ax, -ay));
}

/* A placeholder reached during a play: the program's text/icon goes here, in
 * this z-order. m maps the placeholder's own w x h box to layout units. */
typedef void (*ps3ui_hook_fn)(void *user, ps3ui_canvas_t *cv, const char *name,
                              ps3ui_mat_t m, float opacity, int w, int h);

/* Sprite images by id; the painter lives in ps3ui_sprites.h. */
static const ps3ui_image_t *ps3ui_sprite(int id);

typedef struct {
    const ps3ui_scene_t *scene;
    ps3ui_hook_fn hook;
    void *user;
    /* Per-play overrides the program makes: hide a layer or swap its sprite,
     * by name. Evaluated by ps3ui_play's caller through `filter`. */
    int (*filter)(void *user, const char *layer, int *sprite);
} ps3ui_play_t;

static void ps3ui_play_comp(const ps3ui_play_t *p, ps3ui_canvas_t *cv, int comp, float f,
                            ps3ui_mat_t parent, float opacity, int blend);

static inline void ps3ui_play_layer(const ps3ui_play_t *p, ps3ui_canvas_t *cv, const ps3ui_layer_t *l,
                                    float f, ps3ui_mat_t parent, float opacity, int blend)
{
    if (!l->active || f < l->start || f >= l->end)
        return;
    const ps3ui_scene_t *s = p->scene;
    float op = opacity * ps3ui_curve_eval(s, &l->c[PS3UI_CURVE_OP], f);
    if (op <= 0.0f)
        return;
    ps3ui_mat_t m = ps3ui_mat_mul(parent, ps3ui_layer_mat(s, l, f));
    int b = l->blend == PS3UI_BLEND_NORMAL ? blend : l->blend;
    if (l->type == PS3UI_LAYER_COMP) {
        ps3ui_play_comp(p, cv, l->item, (f - l->start) * l->tscale + l->offset, m, op, b);
        return;
    }
    if (l->type != PS3UI_LAYER_VIDEO)
        return;
    if (l->nsrc == 0) {
        if (p->hook)
            p->hook(p->user, cv, l->name, m, op, l->w, l->h);
        return;
    }
    int si = 0;
    if (l->nsrc > 1 && l->frames_per_src > 0.0f) {
        si = (int)(((f - l->start) * l->tscale + l->offset) / l->frames_per_src);
        if (si < 0) si = 0;
        if (si >= l->nsrc) si = l->nsrc - 1;
    }
    int spr = s->srcs[l->item + si];
    if (p->filter && !p->filter(p->user, l->name, &spr))
        return;
    const ps3ui_image_t *im = ps3ui_sprite(spr);
    if (!im || !im->px)
        return;
    /* The sprite fills the video's box. */
    ps3ui_mat_t fit = m;
    if (im->w != l->w || im->h != l->h)
        fit = ps3ui_mat_mul(m, ps3ui_mat_scale((float)l->w / (float)im->w, (float)l->h / (float)im->h));
    ps3ui_draw_image(cv, im, fit, op, b, NULL);
}

static void ps3ui_play_comp(const ps3ui_play_t *p, ps3ui_canvas_t *cv, int comp, float f,
                            ps3ui_mat_t parent, float opacity, int blend)
{
    const ps3ui_comp_t *c = &p->scene->comps[comp];
    for (int i = (int)c->n - 1; i >= 0; i--) {
        const ps3ui_layer_t *l = &p->scene->layers[c->first + i];
        if (p->filter && l->type == PS3UI_LAYER_COMP) {
            int dummy = -1;
            if (!p->filter(p->user, l->name, &dummy))
                continue;
        }
        ps3ui_play_layer(p, cv, l, f, parent, opacity, blend);
    }
}

static inline int ps3ui_comp_find(const ps3ui_scene_t *s, const char *name)
{
    for (int i = 0; i < s->ncomps; i++)
        if (s->comps[i].name && strcmp(s->comps[i].name, name) == 0)
            return i;
    return -1;
}

/* ---------------------------------------------------------------------------
 * Text
 * ------------------------------------------------------------------------- */

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "ps3ui_fonts.h"

enum { PS3UI_FONT_TITLE, PS3UI_FONT_TEXT, PS3UI_FONT_MONO, PS3UI_FONTS };

typedef struct {
    int font, size_q, sx_q, cp;     /* cap in 1/4 px, x stretch in 1/256 */
    int w, h, xoff, yoff;
    float adv;
    unsigned char *bm;
    unsigned used;
} ps3ui_glyph_t;

static struct {
    int ready;
    stbtt_fontinfo info[PS3UI_FONTS];
    ps3ui_glyph_t cache[PS3UI_MAX_GLYPH_CACHE];
    unsigned tick;
} g_ps3ui_text;

static inline void ps3ui_text_init(void)
{
    if (g_ps3ui_text.ready)
        return;
    const unsigned char *data[PS3UI_FONTS] = { ps3ui_font_title, ps3ui_font_text, ps3ui_font_mono };
    for (int i = 0; i < PS3UI_FONTS; i++)
        stbtt_InitFont(&g_ps3ui_text.info[i], data[i], stbtt_GetFontOffsetForIndex(data[i], 0));
    g_ps3ui_text.ready = 1;
}

/* Scale that makes the font's cap height `cap` pixels. */
static inline float ps3ui_font_scale(int font, float cap)
{
    ps3ui_text_init();
    const stbtt_fontinfo *fi = &g_ps3ui_text.info[font];
    int x0, y0, x1, y1;
    if (!stbtt_GetCodepointBox(fi, 'H', &x0, &y0, &x1, &y1) || y1 <= 0)
        return stbtt_ScaleForPixelHeight(fi, cap * 1.4f);
    return cap / (float)y1;
}

/* Horizontal ink extent of a glyph at cap height `cap`, x stretch 1. */
static inline void ps3ui_glyph_ink(int font, float cap, int cp, float *x0, float *x1)
{
    ps3ui_text_init();
    const stbtt_fontinfo *fi = &g_ps3ui_text.info[font];
    int ix0, iy0, ix1, iy1;
    float sc = ps3ui_font_scale(font, cap);
    if (!stbtt_GetCodepointBox(fi, cp, &ix0, &iy0, &ix1, &iy1)) {
        *x0 = *x1 = 0.0f;
        return;
    }
    *x0 = (float)ix0 * sc;
    *x1 = (float)ix1 * sc;
}

static inline const ps3ui_glyph_t *ps3ui_glyph(int font, float cap_px, float sx, int cp)
{
    ps3ui_text_init();
    int q = (int)lroundf(cap_px * 4.0f), xq = (int)lroundf(sx * 256.0f);
    unsigned oldest = ~0u;
    int slot = 0;
    for (int i = 0; i < PS3UI_MAX_GLYPH_CACHE; i++) {
        ps3ui_glyph_t *g = &g_ps3ui_text.cache[i];
        if (g->used && g->font == font && g->size_q == q && g->sx_q == xq && g->cp == cp) {
            g->used = ++g_ps3ui_text.tick;
            return g;
        }
        if (g->used < oldest) {
            oldest = g->used;
            slot = i;
        }
    }
    ps3ui_glyph_t *g = &g_ps3ui_text.cache[slot];
    if (g->bm)
        stbtt_FreeBitmap(g->bm, NULL);
    const stbtt_fontinfo *fi = &g_ps3ui_text.info[font];
    float sc = ps3ui_font_scale(font, (float)q / 4.0f);
    int adv, lsb;
    stbtt_GetCodepointHMetrics(fi, cp, &adv, &lsb);
    g->font = font;
    g->size_q = q;
    g->sx_q = xq;
    g->cp = cp;
    g->adv = (float)adv * sc * ((float)xq / 256.0f);
    g->bm = stbtt_GetCodepointBitmap(fi, sc * ((float)xq / 256.0f), sc, cp, &g->w, &g->h, &g->xoff, &g->yoff);
    g->used = ++g_ps3ui_text.tick;
    return g;
}

/* Text style.
 *
 * With `metrics` (ps3ui_ps3_metrics[0] for the PS3's font 1, [1] for font 2)
 * text is laid out the way the PS3 lays it out: each character's ink starts
 * where the PS3's does and is as wide, and the pen moves on by the PS3's ink
 * width + 4. Our glyph is stretched horizontally (within 20%) to fill the
 * PS3's ink width, so strings sit exactly where the PS3 puts them.
 * `ps3_cap` is the PS3 font's own cap height (37 for font 1, 40 for font 2):
 * the metrics scale by cap / ps3_cap.
 *
 * mono_adv > 0 instead lays every character on a fixed pitch, centred in its
 * cell, which is how the PS3 sets names (font 1, 23 units a cell). */
typedef struct {
    int font;
    float cap;              /* cap height, 1080p units */
    float track;            /* extra advance per character, 1080p units (no metrics) */
    float mono_adv;         /* 0 = proportional */
    uint32_t rgb;
    float skew;             /* x shear per pixel above the baseline (italic) */
    const uint8_t (*metrics)[2];
    float ps3_cap;
} ps3ui_text_style_t;

/* Where a character goes: ink left offset from the pen, x stretch, advance,
 * all in 1080p units. */
static inline void ps3ui_text_place(const ps3ui_text_style_t *st, int cp, float *ink_off, float *sx, float *adv)
{
    *ink_off = 0.0f;
    *sx = 1.0f;
    if (st->mono_adv > 0.0f) {
        *adv = st->mono_adv;
        return;
    }
    if (st->metrics && cp >= 32 && cp < 127) {
        float k = st->cap / st->ps3_cap;
        float il = (float)st->metrics[cp - 32][0], iw = (float)st->metrics[cp - 32][1];
        *adv = (iw + 4.0f) * k;
        float x0, x1;
        ps3ui_glyph_ink(st->font, st->cap, cp, &x0, &x1);
        if (cp != ' ' && x1 > x0 && iw > 0.0f) {
            float s = iw * k / (x1 - x0);
            *sx = s < 0.8f ? 0.8f : s > 1.25f ? 1.25f : s;
            /* centre our (stretched) ink on the PS3's ink box */
            *ink_off = (iw * k - (x1 - x0) * *sx) * 0.5f;
        }
        (void)il;
        return;
    }
    *adv = ps3ui_glyph(st->font, st->cap, 1.0f, cp)->adv + st->track;
}

static inline float ps3ui_text_width(const ps3ui_text_style_t *st, const char *s)
{
    float w = 0.0f, last_pad = 0.0f;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        float off, sx, adv;
        ps3ui_text_place(st, *p, &off, &sx, &adv);
        w += adv;
        last_pad = st->mono_adv > 0.0f ? 0.0f : st->metrics ? 4.0f * st->cap / st->ps3_cap : st->track;
    }
    return w - last_pad;
}

/* Draw text with its baseline-left at layout (x, y). Glyphs are rasterised at
 * the canvas's own resolution, so text stays sharp at any output size. */
static inline void ps3ui_text(ps3ui_canvas_t *cv, const ps3ui_text_style_t *st, float x, float y,
                              const char *s, float opacity)
{
    if (opacity <= 0.0f)
        return;
    float cap_px = st->cap * cv->k;
    float c[3];
    ps3ui_rgb_hex(st->rgb, c);
    float pen = x, base = y * cv->k + cv->oy;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        float off, sx, adv;
        ps3ui_text_place(st, *p, &off, &sx, &adv);
        const ps3ui_glyph_t *g = ps3ui_glyph(st->font, cap_px, sx, *p);
        float gx;
        if (st->mono_adv > 0.0f) {
            gx = (pen * cv->k + cv->ox) + (st->mono_adv * cv->k - g->adv) * 0.5f;
        } else if (st->metrics && *p >= 32 && *p < 127) {
            /* put the ink's left edge at pen + off */
            float x0, x1;
            ps3ui_glyph_ink(st->font, st->cap, *p, &x0, &x1);
            gx = (pen + off - x0 * sx) * cv->k + cv->ox;
        } else {
            gx = pen * cv->k + cv->ox;
        }
        int bx = (int)lroundf(gx) + g->xoff, by = (int)lroundf(base) + g->yoff;
        if (cv->dl && g->bm && g->w > 0 && g->h > 0) {
            ps3ui_dl_item_t *it = ps3ui_dl_push(cv->dl);
            if (it) {
                float top = st->skew != 0.0f ? -(float)g->yoff * st->skew : 0.0f;
                float bot = st->skew != 0.0f ? -(float)(g->yoff + g->h) * st->skew : 0.0f;
                it->kind = PS3UI_DL_GLYPH;
                it->blend = PS3UI_BLEND_NORMAL;
                it->src = g->bm;
                it->sw = g->w;
                it->sh = g->h;
                it->key = ((uint64_t)(g->font & 3) << 62) | ((uint64_t)(g->size_q & 0xFFFF) << 40)
                        | ((uint64_t)(g->sx_q & 0xFFFF) << 24) | (uint64_t)(g->cp & 0xFFFFFF);
                it->x[0] = (float)bx + top;
                it->x[1] = (float)(bx + g->w) + top;
                it->x[2] = (float)bx + bot;
                it->x[3] = (float)(bx + g->w) + bot;
                it->y[0] = it->y[1] = (float)by;
                it->y[2] = it->y[3] = (float)(by + g->h);
                it->r = c[0];
                it->g = c[1];
                it->b = c[2];
                it->a = opacity;
            }
            pen += adv;
            continue;
        }
        for (int yy = 0; yy < g->h; yy++) {
            int py = by + yy;
            if (py < cv->clip_y0 || py >= cv->clip_y1)
                continue;
            int shear = st->skew != 0.0f ? (int)lroundf(-(float)(g->yoff + yy) * st->skew) : 0;
            for (int xx = 0; xx < g->w; xx++) {
                int px = bx + xx + shear;
                if (px < cv->clip_x0 || px >= cv->clip_x1)
                    continue;
                float a = (float)g->bm[yy * g->w + xx] / 255.0f * opacity;
                if (a > 0.0f)
                    ps3ui_blend_px(cv->rgb + ((size_t)py * (size_t)cv->w + (size_t)px) * 3, c, a,
                                   PS3UI_BLEND_NORMAL);
            }
        }
        pen += adv;
    }
}

#endif /* PS3UI_H */
