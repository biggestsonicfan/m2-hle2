/*
 * ps3ui_sprites.h -- the lobby's sprites, painted by code.
 *
 * Each painter draws one sprite at the size the PS3 authored it (1080p texels)
 * from a model: border widths, the colours at its landmarks and the curve that
 * joins them, fitted to measurements of the original (see tools/ps3ui/README.md
 * for how they are graded). None of Sega's pixels are stored here.
 *
 * The falloffs are all one shape, v(d) = end + (v0 - end) * exp(-(d/s)^p),
 * with s and p fitted per ramp.
 */
#ifndef PS3UI_SPRITES_H
#define PS3UI_SPRITES_H

#include <stdio.h>
#include "ps3ui.h"
#include "ps3ui_layout.h"

static float ps3ui__fall(float d, float s, float p)
{
    return d <= 0.0f ? 1.0f : expf(-powf(d / s, p));
}

static float ps3ui__smooth(float e0, float e1, float x)
{
    float t = (x - e0) / (e1 - e0);
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return t * t * (3.0f - 2.0f * t);
}

/* A colour + alpha, 0..255 per channel. */
typedef struct { float r, g, b, a; } ps3ui__c4;

static ps3ui__c4 ps3ui__hex4(uint32_t rgb, int a)
{
    ps3ui__c4 c = { (float)((rgb >> 16) & 0xFF), (float)((rgb >> 8) & 0xFF), (float)(rgb & 0xFF), (float)a };
    return c;
}

static ps3ui__c4 ps3ui__mix(ps3ui__c4 a, ps3ui__c4 b, float t)
{
    ps3ui__c4 c = { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
    return c;
}

static ps3ui__c4 ps3ui__max4(ps3ui__c4 a, ps3ui__c4 b)
{
    ps3ui__c4 c = { a.r > b.r ? a.r : b.r, a.g > b.g ? a.g : b.g, a.b > b.b ? a.b : b.b, a.a > b.a ? a.a : b.a };
    return c;
}

static void ps3ui__put(ps3ui_image_t *im, int x, int y, ps3ui__c4 c)
{
    if (x < 0 || y < 0 || x >= im->w || y >= im->h)
        return;
    float *p = ps3ui_px(im, x, y);
    p[0] = c.r / 255.0f;
    p[1] = c.g / 255.0f;
    p[2] = c.b / 255.0f;
    p[3] = c.a / 255.0f;
}

static void ps3ui__rect(ps3ui_image_t *im, int x0, int y0, int x1, int y1, ps3ui__c4 c)
{
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            ps3ui__put(im, x, y, c);
}

/* ---- shared colours -------------------------------------------------------- */

#define PS3UI__GREY     0x595959    /* white plates' and rails' outline */
#define PS3UI__BORDER   0x303030    /* dark panels' outline */
#define PS3UI__PANEL    0x132241    /* dark panel body, alpha 0.8 */
#define PS3UI__PANEL_A  0xCC

/* ---- dark panel (hw_pla_*): profiles away from each border ---------------- */

/* d rows below the top border. */
static ps3ui__c4 ps3ui__pla_top(float d)
{
    if (d < 1.0f) return ps3ui__hex4(0xDEE7FD, 0xFE);
    if (d < 2.0f) return ps3ui__hex4(0xD7E1F7, 0xFC);
    if (d < 3.0f) return ps3ui__hex4(0xA4AEC5, 0xEC);
    ps3ui__c4 end = ps3ui__hex4(PS3UI__PANEL, PS3UI__PANEL_A), v0 = ps3ui__hex4(0x525D75, 0xD7);
    float t = ps3ui__fall(d - 3.0f, 5.8f, 1.33f), ta = ps3ui__fall(d - 3.0f, 6.15f, 1.6f);
    ps3ui__c4 c = ps3ui__mix(end, v0, t);
    c.a = end.a + (v0.a - end.a) * ta;
    return c;
}

/* d columns inside the side border. */
static ps3ui__c4 ps3ui__pla_side(float d)
{
    ps3ui__c4 end = ps3ui__hex4(PS3UI__PANEL, PS3UI__PANEL_A), v0 = ps3ui__hex4(0x6F798E, 0xDE);
    float t = ps3ui__fall(d, 5.6f, 1.18f), ta = ps3ui__fall(d, 5.55f, 1.05f);
    ps3ui__c4 c = ps3ui__mix(end, v0, t);
    c.a = end.a + (v0.a - end.a) * ta;
    return c;
}

/* d rows above the bottom border: a three-row rim light over a short ramp. */
static ps3ui__c4 ps3ui__pla_udr(float d)
{
    if (d < 1.0f) return ps3ui__hex4(0x9BA5B8, 0xE9);
    if (d < 2.0f) return ps3ui__hex4(0x97A1B5, 0xE8);
    if (d < 3.0f) return ps3ui__hex4(0x7D889D, 0xE2);
    if (d < 4.0f) return ps3ui__hex4(0x515C76, 0xD8);
    ps3ui__c4 a = ps3ui__hex4(0x3B4762, 0xD3), b = ps3ui__hex4(0x23314E, 0xCF);
    float t = (d - 4.0f) / 5.0f;
    return ps3ui__mix(a, b, t > 1.0f ? 1.0f : t);
}

/* Drop shadows: black, by distance from the border. */
static float ps3ui__shadow_side(float d) { return 49.0f * ps3ui__fall(d, 3.15f, 1.35f); }
static float ps3ui__shadow_bottom(float d) { return 162.0f * ps3ui__fall(d, 9.95f, 2.1f); }

static void ps3ui__paint_pla(ps3ui_image_t *im, int id)
{
    ps3ui__c4 border = ps3ui__hex4(PS3UI__BORDER, 0xFF), black = ps3ui__hex4(0, 0);
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) {
            ps3ui__c4 c = ps3ui__hex4(PS3UI__PANEL, PS3UI__PANEL_A);
            switch (id) {
            case PS3UI_SPR_HW_PLA_OVR_MID:
                c = y < 3 ? border : ps3ui__pla_top((float)(y - 3));
                break;
            case PS3UI_SPR_HW_PLA_SIDE:
                if (x < 8) {
                    black.a = ps3ui__shadow_side((float)(7 - x));
                    c = black;
                } else {
                    c = x < 11 ? border : ps3ui__pla_side((float)(x - 11));
                }
                break;
            case PS3UI_SPR_HW_PLA_UDR_MID:
                if (y < 10) c = ps3ui__pla_udr((float)(9 - y));
                else if (y < 13) c = border;
                else { black.a = ps3ui__shadow_bottom((float)(y - 13)); c = black; }
                break;
            case PS3UI_SPR_HW_PLA_OVR_CNR:
                if (x < 8) {
                    /* the side shadow fades in below the corner */
                    float f = ((float)y - 7.0f) / 11.0f;
                    f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
                    black.a = ps3ui__shadow_side((float)(7 - x)) * f;
                    c = black;
                } else if (y < 3 || x < 11) {
                    c = border;
                } else {
                    c = ps3ui__max4(ps3ui__pla_top((float)(y - 3)), ps3ui__pla_side((float)(x - 11)));
                }
                break;
            case PS3UI_SPR_HW_PLA_UDR_CNR:
                if (y >= 13) {
                    black.a = ps3ui__shadow_bottom((float)(y - 13)) * ps3ui__smooth(0.0f, 20.0f, (float)x);
                    c = black;
                } else if (x < 8) {
                    black.a = y < 10 ? ps3ui__shadow_side((float)(7 - x)) : 0.0f;
                    c = black;
                } else if (x < 11 || y >= 10) {
                    c = border;
                } else {
                    c = ps3ui__max4(ps3ui__pla_udr((float)(9 - y)), ps3ui__pla_side((float)(x - 11)));
                }
                break;
            default:
                break;
            }
            ps3ui__put(im, x, y, c);
        }
}

/* ---- white "pillow" plates ----------------------------------------------- */

/* Alpha of a translucent white plate: opaque at its rims, `mid` inside. The
 * top rim fades over ~10 rows, the bottom over ~5, a left edge (edge pieces)
 * over ~5 columns. Distances are from the inside of each border; < 0 = none. */
static float ps3ui__pillow(float mid, float dt, float db, float dl)
{
    float t = dt >= 0.0f ? ps3ui__fall(dt - 3.0f, 6.2f, 2.0f) : 0.0f;
    float b = db >= 0.0f ? 0.93f * ps3ui__fall(db - 2.0f, 3.6f, 1.0f) : 0.0f;
    float l = dl >= 0.0f ? ps3ui__fall(dl, 4.7f, 1.0f) : 0.0f;
    float k = 1.0f - (1.0f - t) * (1.0f - b) * (1.0f - l);
    float a = mid + (255.0f - mid) * k;
    return a > 255.0f ? 255.0f : a;
}

/* A plate w x h with a 3px outline on the sides named. */
static void ps3ui__paint_plate(ps3ui_image_t *im, float mid, int top, int bottom, int left, int fade_left)
{
    ps3ui__c4 grey = ps3ui__hex4(PS3UI__GREY, 0xFF);
    int y0 = top ? 3 : 0, y1 = bottom ? im->h - 3 : im->h, x0 = left ? 3 : 0;
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) {
            if (y < y0 || y >= y1 || x < x0) {
                ps3ui__put(im, x, y, grey);
                continue;
            }
            float a = ps3ui__pillow(mid, (float)(y - y0), (float)(y1 - 1 - y), fade_left ? (float)(x - x0) : -1.0f);
            ps3ui__put(im, x, y, ps3ui__hex4(0xFCFDFF, (int)(a + 0.5f)));
        }
}

/* ---- the header tab: a white plate whose lower edge curves up to the right -- */

static void ps3ui__paint_tab(ps3ui_image_t *im)
{
    const float cx = 87.0f, cy = -126.1f, r = 175.1f;   /* outer lower edge, right of cx */
    ps3ui__c4 grey = ps3ui__hex4(PS3UI__GREY, 0xFF);
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) {
            float fx = (float)x + 0.5f;
            float edge = 49.0f;                          /* outer lower edge at this x */
            if (fx > cx) {
                float q = r * r - (fx - cx) * (fx - cx);
                edge = q > 0.0f ? cy + sqrtf(q) : 0.0f;
            }
            /* the stem below the tab, where the rail joins */
            if (y >= 49) {
                if (x < 3 || (x >= 13 && x < 16)) ps3ui__put(im, x, y, grey);
                else if (x < 13) ps3ui__put(im, x, y, ps3ui__hex4(0xFFFFFF, 0xFF));
                else ps3ui__put(im, x, y, ps3ui__hex4(0xF8FAFF, 0));
                continue;
            }
            float fy = (float)y + 0.5f;
            /* coverage of the area above the edge, and above edge-3 (inside the border) */
            float cov = edge - fy + 0.5f, inner = edge - 3.0f - fy + 0.5f;
            cov = cov < 0.0f ? 0.0f : cov > 1.0f ? 1.0f : cov;
            inner = inner < 0.0f ? 0.0f : inner > 1.0f ? 1.0f : inner;
            if (cov <= 0.0f) {
                ps3ui__put(im, x, y, ps3ui__hex4(0xF8FAFF, 0));
                continue;
            }
            if (y < 3 || x < 3 || inner <= 0.0f) {
                ps3ui__c4 g = grey;
                g.a = 255.0f * cov;
                ps3ui__put(im, x, y, g);
                continue;
            }
            float a = ps3ui__pillow(198.0f, (float)(y - 3), edge - 3.0f - fy - 0.5f, -1.0f);
            ps3ui__c4 w = ps3ui__hex4(0xFCFDFF, (int)(a + 0.5f));
            ps3ui__put(im, x, y, inner < 1.0f ? ps3ui__mix(grey, w, inner) : w);
        }
    /* the 3x3 dots */
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++)
            ps3ui__rect(im, 14 + 10 * i, 11 + 10 * j, 20 + 10 * i, 17 + 10 * j, ps3ui__hex4(0x8D8D8D, 0xFF));
}

/* ---- the count box's blue glass ------------------------------------------- */

static ps3ui__c4 ps3ui__glass(int y, int h)
{
    static const uint32_t top_rgb[4] = { 0xB5D5E4, 0xA5CBDE, 0x84BED6, 0x3490BA };
    static const int top_a[4] = { 0xDD, 0xCE, 0xB9, 0xA2 };
    if (y < 4) return ps3ui__hex4(top_rgb[y], top_a[y]);
    if (y < 6) return ps3ui__hex4(0x0879AD, 0x8C);
    if (y < 7) return ps3ui__hex4(0x00699C, 0x9A);
    int rim = h - 7;                                   /* last 7 rows: the bright bottom rim */
    if (y < rim) {
        float t = (float)(y - 7) / (float)(rim - 1 - 7);
        return ps3ui__mix(ps3ui__hex4(0x0875A5, 0x8C), ps3ui__hex4(0x004265, 0x8C), t);
    }
    float t = (float)(y - rim) / 5.0f;
    return ps3ui__mix(ps3ui__hex4(0x003C6B, 0xD3), ps3ui__hex4(0x4283AD, 0xF5), t > 1.0f ? 1.0f : t);
}

static void ps3ui__paint_glass(ps3ui_image_t *im, int side)
{
    static const uint32_t edge_rgb[5] = { 0x4A8EB5, 0x4282B5, 0x3979AD, 0x2671A4, 0x006194 };
    static const int edge_a[5] = { 0xF0, 0xE8, 0xE6, 0xC6, 0xA7 };
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) {
            ps3ui__c4 c = ps3ui__glass(y, im->h);
            if (side && x < 5 && y >= 3 && y < im->h - 7)
                c = ps3ui__hex4(edge_rgb[x], edge_a[x]);
            ps3ui__put(im, x, y, c);
        }
}

/* ---- lettering: a glyph of the title face fitted into a measured box ------- */

static void ps3ui__glyph_path(ps3ui_path_t *p, int font, int cp, float x0, float y0, float x1, float y1,
                              float shear)
{
    ps3ui_text_init();
    const stbtt_fontinfo *fi = &g_ps3ui_text.info[font];
    int ix0, iy0, ix1, iy1;
    if (!stbtt_GetCodepointBox(fi, cp, &ix0, &iy0, &ix1, &iy1))
        return;
    stbtt_vertex *v;
    int n = stbtt_GetCodepointShape(fi, cp, &v);
    float sx = (x1 - x0) / (float)(ix1 - ix0), sy = (y1 - y0) / (float)(iy1 - iy0);
#define PS3UI__GX(gx, gy) (x0 + ((float)(gx) - (float)ix0) * sx + (y1 - (y1 - ((float)(gy) - (float)iy0) * sy)) * shear)
#define PS3UI__GY(gy) (y1 - ((float)(gy) - (float)iy0) * sy)
    for (int i = 0; i < n; i++) {
        float x = PS3UI__GX(v[i].x, v[i].y), y = PS3UI__GY(v[i].y);
        if (v[i].type == STBTT_vmove) {
            ps3ui_path_move(p, x, y);
        } else if (v[i].type == STBTT_vline) {
            ps3ui_path_line(p, x, y);
        } else if (v[i].type == STBTT_vcurve) {
            ps3ui_path_quad(p, PS3UI__GX(v[i].cx, v[i].cy), PS3UI__GY(v[i].cy), x, y);
        }
    }
#undef PS3UI__GX
#undef PS3UI__GY
    stbtt_FreeShape(fi, v);
}

/* Letters of the title face, each fitted to its measured ink box. */
typedef struct { int cp; float x0, x1; } ps3ui__letter_t;

static float *ps3ui__letters(int w, int h, const ps3ui__letter_t *l, int n, float y0, float y1, float shear)
{
    float *m = (float *)calloc((size_t)w * (size_t)h, sizeof(float));
    for (int i = 0; i < n; i++) {
        ps3ui_path_t p;
        ps3ui_path_reset(&p);
        /* the box is the sheared ink's extent; fit the upright glyph to it */
        float slant = (y1 - y0) * shear;
        ps3ui__glyph_path(&p, PS3UI_FONT_TITLE, l[i].cp, l[i].x0, y0, l[i].x1 - slant, y1, shear);
        ps3ui_path_cover(&p, m, w, h);
    }
    return m;
}

/* Brute-force distance from each pixel to the nearest pixel of mask >= 0.5
 * (small sprites only). */
static float *ps3ui__dist(const float *m, int w, int h, float maxd)
{
    float *d = (float *)malloc(sizeof(float) * (size_t)w * (size_t)h);
    int R = (int)ceilf(maxd);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float best = maxd;
            if (m[y * w + x] >= 0.5f) {
                d[y * w + x] = 0.0f;
                continue;
            }
            for (int yy = y - R; yy <= y + R; yy++) {
                if (yy < 0 || yy >= h)
                    continue;
                for (int xx = x - R; xx <= x + R; xx++) {
                    if (xx < 0 || xx >= w || m[yy * w + xx] < 0.5f)
                        continue;
                    float dd = sqrtf((float)((xx - x) * (xx - x) + (yy - y) * (yy - y)));
                    if (dd < best)
                        best = dd;
                }
            }
            d[y * w + x] = best;
        }
    return d;
}

/* ---- VS -------------------------------------------------------------------- */

static void ps3ui__paint_vs(ps3ui_image_t *im)
{
    const ps3ui__letter_t L[2] = { { 'V', 24.0f, 153.0f }, { 'S', 161.0f, 275.0f } };
    int w = im->w, h = im->h;
    float *m = ps3ui__letters(w, h, L, 2, 26.0f, 128.0f, 0.0f);
    float *g = (float *)malloc(sizeof(float) * (size_t)w * (size_t)h);
    memcpy(g, m, sizeof(float) * (size_t)w * (size_t)h);
    ps3ui_blur(g, w, h, 11.5f);
    float *rim = (float *)malloc(sizeof(float) * (size_t)w * (size_t)h);
    memcpy(rim, m, sizeof(float) * (size_t)w * (size_t)h);
    ps3ui_blur(rim, w, h, 1.2f);
    /* face gradient stops (y, colour) */
    static const float sy[] = { 29, 47, 65, 71, 86, 122, 125 };
    static const uint32_t sc[] = { 0x94B5DD, 0xC1E1FF, 0xDCFCFF, 0xDCFCFF, 0xB0D0F7, 0xB0D0F7, 0xBBDFFF };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float cov = m[y * w + x];
            float ga = 326.0f * g[y * w + x];
            if (ga > 255.0f) ga = 255.0f;
            ps3ui__c4 glow = ps3ui__hex4(0x63ACFF, (int)ga);
            if (cov <= 0.0f) {
                ps3ui__put(im, x, y, glow);
                continue;
            }
            float fy = (float)y;
            int k = 0;
            while (k < 6 && fy > sy[k + 1]) k++;
            float t = fy <= sy[0] ? 0.0f : fy >= sy[6] ? 1.0f : (fy - sy[k]) / (sy[k + 1] - sy[k]);
            ps3ui__c4 face = fy >= sy[6] ? ps3ui__hex4(sc[6], 255)
                           : ps3ui__mix(ps3ui__hex4(sc[k], 255), ps3ui__hex4(sc[k + 1], 255), fy <= sy[0] ? 0.0f : t);
            /* a near-white rim just inside the outline */
            float edge = 1.0f - ps3ui__smooth(0.6f, 0.95f, rim[y * w + x]);
            face = ps3ui__mix(face, ps3ui__hex4(0xEBF9FF, 255), edge * 0.85f);
            /* letter over glow */
            ps3ui__c4 c = ps3ui__mix(glow, face, cov);
            c.a = glow.a + (255.0f - glow.a) * cov;
            if (cov < 1.0f && c.a > 0.0f) {
                /* keep the colour straight: blend premultiplied */
                float ca = cov * 255.0f, ga2 = glow.a * (1.0f - cov);
                c.r = (face.r * ca + glow.r * ga2) / (ca + ga2);
                c.g = (face.g * ca + glow.g * ga2) / (ca + ga2);
                c.b = (face.b * ca + glow.b * ga2) / (ca + ga2);
            }
            ps3ui__put(im, x, y, c);
        }
    free(m);
    free(g);
    free(rim);
}

/* ---- the glowing dashes either side of VS --------------------------------- */

static void ps3ui__paint_dash(ps3ui_image_t *im)
{
    int w = im->w, h = im->h;
    ps3ui_path_t p;
    ps3ui_path_reset(&p);
    /* 16 rows, each 33 wide, stepping one left per row */
    ps3ui_path_move(&p, 35.0f, 20.0f);
    ps3ui_path_line(&p, 68.0f, 20.0f);
    ps3ui_path_line(&p, 52.0f + 1.0f, 36.0f);
    ps3ui_path_line(&p, 20.0f, 36.0f);
    float *m = (float *)calloc((size_t)w * (size_t)h, sizeof(float));
    ps3ui_path_cover(&p, m, w, h);
    float *d = ps3ui__dist(m, w, h, 24.0f);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float cov = m[y * w + x];
            float dist = d[y * w + x];
            float ga = dist > 0.0f ? 217.0f * ps3ui__fall(dist - 1.0f, 8.4f, 1.45f) : 255.0f;
            ps3ui__c4 c = ps3ui__hex4(0x00A2FF, (int)(ga + 0.5f));
            if (cov > 0.0f) {
                /* core: bright cyan rim, deep sky blue centre, by distance to the nearest edge row */
                float e = fminf((float)y - 20.0f, 35.0f - (float)y);
                float t = ps3ui__fall(e, 2.6f, 1.0f);
                ps3ui__c4 core = ps3ui__mix(ps3ui__hex4(0x00A2FF, 255), ps3ui__hex4(0x08F7FF, 255), t);
                c = ps3ui__mix(c, core, cov);
                c.a = ga + (255.0f - ga) * cov;
            }
            ps3ui__put(im, x, y, c);
        }
    free(m);
    free(d);
}

/* ---- the cyan Z lines ------------------------------------------------------ */

static void ps3ui__paint_joint(ps3ui_image_t *im, int b)
{
    ps3ui_path_t p;
    ps3ui_path_reset(&p);
    if (!b) {
        /* 130x110: a 45-degree band 110 <= x+y <= 130, and a tab x >= 91, y < 14 */
        ps3ui_path_move(&p, 110.0f, 0.0f);
        ps3ui_path_line(&p, 130.0f, 0.0f);
        ps3ui_path_line(&p, 20.0f, 110.0f);
        ps3ui_path_line(&p, 0.0f, 110.0f);
        ps3ui_path_move(&p, 91.0f, 0.0f);
        ps3ui_path_line(&p, 130.0f, 0.0f);
        ps3ui_path_line(&p, 116.0f, 14.0f);
        ps3ui_path_line(&p, 91.0f, 14.0f);
    } else {
        /* 322x86: a band 301 <= x+y <= 321 down to y 72, then a bar to the left
         * whose end is cut at x+y = 86 */
        ps3ui_path_move(&p, 301.0f, 0.0f);
        ps3ui_path_line(&p, 321.0f, 0.0f);
        ps3ui_path_line(&p, 235.0f, 86.0f);
        ps3ui_path_line(&p, 0.0f, 86.0f);
        ps3ui_path_line(&p, 14.0f, 72.0f);
        ps3ui_path_line(&p, 229.0f, 72.0f);
    }
    ps3ui_path_fill(im, &p, 0x00FFFF, 1.0f);
}

/* ---- READY ----------------------------------------------------------------- */

#define PS3UI__READY_SHEAR 0.178f

static void ps3ui__paint_ready(ps3ui_image_t *im)
{
    const ps3ui__letter_t L[5] = { { 'R', 0, 51 }, { 'E', 52, 100 }, { 'A', 96, 147 }, { 'D', 152, 204 },
                                   { 'Y', 211, 262 } };
    int w = im->w, h = im->h;
    float *m = ps3ui__letters(w, h, L, 5, 1.0f, 49.0f, PS3UI__READY_SHEAR);
    float *in = (float *)malloc(sizeof(float) * (size_t)w * (size_t)h);
    memcpy(in, m, sizeof(float) * (size_t)w * (size_t)h);
    ps3ui_blur(in, w, h, 0.8f);
    static const float uy[] = { 2, 4, 8, 12, 16, 20 };
    static const uint32_t uc[] = { 0xF74D31, 0xFC5736, 0xF95B3F, 0xF76147, 0xF1664F, 0xEF6954 };
    static const float ly[] = { 26, 29, 32, 35, 38, 42, 46 };
    static const uint32_t lc[] = { 0xF4290D, 0xEF2810, 0xE32810, 0xD82912, 0xD82A18, 0xDE3021, 0xE73421 };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float cov = m[y * w + x];
            if (cov <= 0.0f) {
                ps3ui__put(im, x, y, ps3ui__hex4(0xA51000, 0));
                continue;
            }
            const float *sy = y < 26 ? uy : ly;
            const uint32_t *sc = y < 26 ? uc : lc;
            int n = y < 26 ? 6 : 7, k = 0;
            float fy = (float)y;
            while (k < n - 2 && fy > sy[k + 1]) k++;
            float t = (fy - sy[k]) / (sy[k + 1] - sy[k]);
            t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
            ps3ui__c4 c = ps3ui__mix(ps3ui__hex4(sc[k], 255), ps3ui__hex4(sc[k + 1], 255), t);
            /* a 1px darker line inside the outline, antialiased to #A51000 outside */
            float edge = 1.0f - ps3ui__smooth(0.55f, 0.9f, in[y * w + x]);
            c = ps3ui__mix(c, ps3ui__hex4(0xC72A16, 255), edge);
            if (cov < 1.0f)
                c = ps3ui__mix(ps3ui__hex4(0xA51000, 255), c, cov);
            c.a = 255.0f * cov;
            ps3ui__put(im, x, y, c);
        }
    free(m);
    free(in);
}

static void ps3ui__paint_ready_light(ps3ui_image_t *im)
{
    const ps3ui__letter_t L[5] = { { 'R', 17, 68 }, { 'E', 69, 117 }, { 'A', 113, 164 }, { 'D', 169, 221 },
                                   { 'Y', 228, 279 } };
    int w = im->w, h = im->h;
    float *m = ps3ui__letters(w, h, L, 5, 19.0f, 68.0f, PS3UI__READY_SHEAR);
    float *g = (float *)malloc(sizeof(float) * (size_t)w * (size_t)h);
    memcpy(g, m, sizeof(float) * (size_t)w * (size_t)h);
    ps3ui_blur(g, w, h, 7.5f);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float cov = m[y * w + x], ga = 222.0f * g[y * w + x];
            if (ga > 255.0f) ga = 255.0f;
            ps3ui__put(im, x, y, ps3ui__hex4(0xFFFFFF, (int)(ga + (255.0f - ga) * cov + 0.5f)));
        }
    free(m);
    free(g);
}

/* ---- "1P" / "2P" ----------------------------------------------------------- */

static void ps3ui__paint_player_icon(ps3ui_image_t *im, int two)
{
    const ps3ui__letter_t one[2] = { { '1', 0, 18 }, { 'P', 34, 67 } };
    const ps3ui__letter_t pair[2] = { { '2', 0, 34 }, { 'P', 39, 72 } };
    int w = im->w, h = im->h;
    float y0 = two ? 1.0f : 0.0f;
    float *m = ps3ui__letters(w, h, two ? pair : one, 2, y0, y0 + 30.0f, 0.0f);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float cov = m[y * w + x];
            /* a white copy 3px lower shows below the grey */
            float under = y >= 3 ? m[(y - 3) * w + x] : 0.0f;
            ps3ui__c4 c = ps3ui__hex4(0x73757B, 0);
            if (under > 0.0f)
                c = ps3ui__hex4(0xFFFFFF, (int)(255.0f * under));
            if (cov > 0.0f) {
                ps3ui__c4 g = ps3ui__hex4(0x73757B, 255);
                c = under > 0.0f ? ps3ui__mix(c, g, cov) : g;
                c.a = 255.0f * (cov + (1.0f - cov) * under);
            }
            ps3ui__put(im, x, y, c);
        }
    free(m);
}

/* ---- gradient stops ------------------------------------------------------- */

/* A colour ramp through measured stops at positions pos[] (ascending). */
typedef struct { float pos; uint32_t rgb; int a; } ps3ui__stop_t;

static ps3ui__c4 ps3ui__ramp(const ps3ui__stop_t *s, int n, float v)
{
    if (v <= s[0].pos)
        return ps3ui__hex4(s[0].rgb, s[0].a);
    for (int i = 0; i + 1 < n; i++)
        if (v <= s[i + 1].pos)
            return ps3ui__mix(ps3ui__hex4(s[i].rgb, s[i].a), ps3ui__hex4(s[i + 1].rgb, s[i + 1].a),
                              (v - s[i].pos) / (s[i + 1].pos - s[i].pos));
    return ps3ui__hex4(s[n - 1].rgb, s[n - 1].a);
}

static float ps3ui__clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

/* ---- the small header tab (hw_ovr_line_cnr_s) ------------------------------ */

static void ps3ui__paint_tab_s(ps3ui_image_t *im)
{
    ps3ui__rect(im, 0, 0, im->w, im->h, ps3ui__hex4(PS3UI__GREY, 255));
    for (int y = 3; y < im->h; y++)
        for (int x = 3; x < im->w; x++) {
            if (x >= 13 && y >= 13)
                continue;
            /* a faint shade dips along the lower middle of the white */
            float dip = 9.0f * ps3ui__smooth(6.0f, 14.0f, (float)y) * ps3ui__fall(fabsf((float)x - 9.0f), 4.0f, 2.0f);
            ps3ui__put(im, x, y, ps3ui__hex4(0xFFFFFF, (int)(254.0f - dip + 0.5f)));
        }
}

static void ps3ui__round_rect(ps3ui_path_t *p, float x0, float y0, float x1, float y1, float r);

/* ---- the lobby's checker "cubes" (pla_deco_cube) --------------------------- */

static void ps3ui__paint_deco_cube(ps3ui_image_t *im)
{
    /* 10px tiles: a white square with 2.5px rounded corners, a grey 6px square
     * set in it three from the left and one from the top */
    ps3ui_path_t p;
    ps3ui_path_reset(&p);
    for (int j = 0; j < im->h / 10; j++)
        for (int i = 0; i < im->w / 10; i++)
            ps3ui__round_rect(&p, 10.0f * i, 10.0f * j, 10.0f * i + 10.0f, 10.0f * j + 10.0f, 2.5f);
    ps3ui_path_fill(im, &p, 0xFFFFFF, 1.0f);
    for (int j = 0; j < im->h / 10; j++)
        for (int i = 0; i < im->w / 10; i++)
            ps3ui__rect(im, 10 * i + 3, 10 * j + 1, 10 * i + 9, 10 * j + 7, ps3ui__hex4(0x8C8E8C, 255));
}

/* ---- the medium white plate (pla_m_*) -------------------------------------- */

/* Its pillow is fitted separately: top, bottom and left ramps of the plate's
 * own height (70), meeting a 0.75 translucent middle. */
static void ps3ui__paint_plate_m(ps3ui_image_t *im, int left)
{
    ps3ui__c4 grey = ps3ui__hex4(PS3UI__GREY, 0xFF);
    const float mid = 191.0f;
    int y0 = 3, y1 = im->h - 3, x0 = left ? 3 : 0;
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) {
            if (y < y0 || y >= y1 || x < x0) {
                ps3ui__put(im, x, y, grey);
                continue;
            }
            float T = ps3ui__fall((float)(y - y0) - 3.52f, 6.71f, 1.21f);
            float B = 0.916f * ps3ui__fall((float)(y1 - 1 - y) - 1.89f, 5.31f, 0.71f);
            float L = left ? ps3ui__fall((float)(x - x0) - 0.71f, 7.0f, 1.18f) : 0.0f;
            float k = 1.0f - (1.0f - T) * (1.0f - B) * (1.0f - L);
            float a = mid + (255.0f - mid) * k;
            /* the white greys a touch where the ramps are steepest */
            ps3ui__c4 c = ps3ui__mix(ps3ui__hex4(0xFAFBFF, 0), ps3ui__hex4(0xFEFEFF, 0),
                                     fabsf(k - 0.35f) / 0.65f > 1.0f ? 1.0f : fabsf(k - 0.35f) / 0.65f);
            c.a = a;
            ps3ui__put(im, x, y, c);
        }
}

/* ---- the lobby list's selected-row cursor (match_cursor_*) ----------------- */

/* A mid blue plate with a light top rim (3 rows), a lit left edge (2 columns)
 * and a pale bottom rim (4 rows). */
static void ps3ui__paint_cursor(ps3ui_image_t *im, int top, int bottom, int left, int round)
{
    static const uint32_t top_rgb[3] = { 0xDEE7FF, 0xD6E3F7, 0x9CB6D6 };
    static const uint32_t btm_rgb[4] = { 0x4581B1, 0x739EBF, 0x8CAECE, 0x8CB0CE };
    static const uint32_t left_rgb[2] = { 0x6293BB, 0x5C8CB6 };
    int h = im->h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < im->w; x++) {
            ps3ui__c4 c = ps3ui__hex4(0x1865A5, 255);
            int b = h - 1 - y;
            int in_top = top && y < 3, in_btm = bottom && b < 4, in_left = left && x < 2;
            if (in_top) c = ps3ui__hex4(top_rgb[y], 255);
            if (in_btm) c = ps3ui__hex4(btm_rgb[3 - b], 255);
            if (in_left) {
                ps3ui__c4 l = ps3ui__hex4(left_rgb[x], 255);
                if (in_btm) c = ps3ui__mix(l, c, b < 2 ? 0.6f : 0.25f);
                else if (in_top && round) c = ps3ui__mix(l, c, y == 0 ? 0.55f : 0.2f);
                else if (!in_top) c = l;
            }
            ps3ui__put(im, x, y, c);
        }
}

/* ---- the info window's translucent navy frame (match_sel_info_*) ----------- */

static void ps3ui__paint_sel_info(ps3ui_image_t *im, int top, int bottom, int left)
{
    int h = im->h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < im->w; x++) {
            ps3ui__c4 c = ps3ui__hex4(0x011937, 0xB3);
            int b = h - 1 - y;
            if (left && x < 2) c = x == 0 ? ps3ui__hex4(0x001F41, 0xD7) : ps3ui__hex4(0x001A3C, 0xBF);
            if (top && y < 2) c = ps3ui__hex4(0x002145, 0xE6);
            if (bottom && b < 2) {
                ps3ui__c4 r = b == 1 ? ps3ui__hex4(0x3A4E6C, 0xBF) : ps3ui__hex4(0x425573, 0xC2);
                if (left && x < 2) {
                    /* the rim rounds into the left edge */
                    if (x == 0 && b == 1) r = ps3ui__hex4(0x082442, 0xD1);
                    else if (x == b) r = ps3ui__hex4(0x1E3552, 0xB9);
                    else r = ps3ui__hex4(0x4A5973, 0xC5);
                }
                c = r;
            }
            ps3ui__put(im, x, y, c);
        }
}

/* ---- the lobby list's row bar (match_sel_mid / _side) ---------------------- */

static ps3ui__c4 ps3ui__sel_row(int y, int h)
{
    static const ps3ui__stop_t body[] = {
        { 6, 0x0576A5, 0x66 }, { 9, 0x0875A5, 0x66 }, { 13, 0x0671A5, 0x66 }, { 14, 0x046F9C, 0x66 },
        { 25, 0x026694, 0x66 }, { 37, 0x025A84, 0x66 }, { 45, 0x01547B, 0x66 }, { 53, 0x004D73, 0x66 },
        { 61, 0x00456B, 0x66 },
    };
    int b = h - 1 - y;
    if (y == 0) return ps3ui__hex4(0x0162A6, 255);
    if (y == 1) return ps3ui__hex4(0x0869AD, 255);
    if (y == 2) return ps3ui__hex4(0x0071B5, 255);
    if (y < 5) return ps3ui__hex4(0x002042, 0xEF);
    if (y == 5) return ps3ui__hex4(0x002042, 0xE8);
    if (b < 3) return ps3ui__hex4(0x00559C, 255);
    if (b == 3) return ps3ui__hex4(0x001C39, 0xF0);
    if (b < 6) return ps3ui__hex4(0x001C39, 0xEA);
    if (b < 8) return ps3ui__hex4(0x004563, 0x6C);
    return ps3ui__ramp(body, 9, (float)y);
}

static void ps3ui__paint_sel(ps3ui_image_t *im, int side)
{
    int h = im->h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < im->w; x++) {
            ps3ui__c4 c = ps3ui__sel_row(y, h);
            if (side) {
                int b = h - 1 - y;
                static const uint32_t edge[3] = { 0x0162A6, 0x0768AB, 0x0473B0 };
                if (x < 3 && y >= 3 && b >= 3) {
                    c = ps3ui__hex4(edge[x], 255);
                } else if (y < 3 && x <= y) {
                    /* the bevel's highlight runs in along the diagonal */
                    c = ps3ui__hex4(x == y && y < 2 ? 0x067FBD : 0x0892CE, 255);
                    if (x == 0 && y == 2) c = ps3ui__hex4(0x0065A5, 255);
                } else if (y < 3 && x < 3) {
                    c = ps3ui__hex4(0x005B9E, 255);
                } else if (y >= 3 && y <= 5 && x >= 3 && x < 6) {
                    /* the dark band is cut back along the diagonal */
                    if (y - 3 > x - 3) c = ps3ui__hex4(0x0572AC, 0x65);
                    else if (y - 3 == x - 3 && x > 3) c = ps3ui__hex4(0x00244A, 0xD3);
                    else if (x == 3) c = ps3ui__hex4(0x00244A, 0xB7);
                } else if (b >= 3 && b <= 5 && x >= 3 && x < 6) {
                    if (b - 3 < 5 - x) c = ps3ui__hex4(0x003F62, b == 5 && x == 4 ? 0x81 : 0x65);
                    else if (b == 4 && x == 3) c = ps3ui__hex4(0x002439, 0xBA);
                    else c = ps3ui__hex4(0x002439, 0xF3);
                } else if (b < 3 && x < 3) {
                    if (b == 2) c = ps3ui__hex4(x == 2 ? 0x005A8B : edge[x], 255);
                    else if (b == 1 && x == 1) c = ps3ui__hex4(0x005DA2, 255);
                    else if (b == 1 && x == 0) c = ps3ui__hex4(0x0061A5, 255);
                }
            }
            ps3ui__put(im, x, y, c);
        }
}

/* ---- the lobby's small list window plate (match_s_win_plt) ----------------- */

static void ps3ui__paint_s_win_plt(ps3ui_image_t *im)
{
    static const ps3ui__stop_t body[] = {
        { 1, 0x4A82B5, 255 }, { 9, 0x4B81B6, 255 }, { 14, 0x487BB2, 255 }, { 17, 0x4279AD, 255 },
        { 21, 0x4276AD, 255 }, { 24, 0x3C72A7, 255 }, { 29, 0x396EA5, 255 }, { 33, 0x336B9F, 255 },
        { 34, 0x396DA5, 255 }, { 35, 0x396DA5, 255 }, { 36, 0x366AA3, 255 },
    };
    ps3ui__rect(im, 0, 0, im->w, im->h, ps3ui__hex4(0x18518C, 255));
    for (int y = 1; y < im->h - 1; y++)
        ps3ui__rect(im, 1, y, im->w - 1, y + 1, ps3ui__ramp(body, 11, (float)y));
}

/* ---- the info window's title bar (match_sel_info_tit) ---------------------- */

static void ps3ui__paint_sel_info_tit(ps3ui_image_t *im)
{
    static const ps3ui__stop_t mid[] = {
        { 0, 0x56EDFF, 0x92 }, { 4, 0x4AE3FF, 0x8B }, { 9, 0x42DBFF, 0x83 }, { 13, 0x3ACAEA, 0x7F },
        { 17, 0x31B6D6, 0x7D }, { 21, 0x39A6C6, 0x7F }, { 27, 0x429EBD, 0x84 }, { 33, 0x4BA1C4, 0x8D },
        { 34, 0x5AA6CE, 0x92 }, { 35, 0x57A7CE, 0x92 },
    };
    static const ps3ui__stop_t end[] = {
        { 0, 0x68F3FF, 0xA1 }, { 10, 0x63F3FF, 0x9A }, { 13, 0x5DEBFF, 0x98 }, { 18, 0x5AD7FF, 0x98 },
        { 22, 0x5ACBEF, 0x98 }, { 26, 0x63C3E7, 0x9B }, { 32, 0x65BCE4, 0x9F }, { 35, 0x6ABAE4, 0xA1 },
    };
    int w = im->w;
    for (int y = 0; y < im->h; y++) {
        ps3ui__c4 m = ps3ui__ramp(mid, 10, (float)y), e = ps3ui__ramp(end, 8, (float)y);
        for (int x = 0; x < w; x++) {
            float d = (float)(x < w - 1 - x ? x : w - 1 - x);
            ps3ui__put(im, x, y, ps3ui__mix(m, e, ps3ui__fall(d, 10.0f, 1.2f)));
        }
    }
}

/* ---- the dark cursor shade (csr_gra_drk): fades out to the right ----------- */

static void ps3ui__paint_csr_gra_drk(ps3ui_image_t *im)
{
    for (int x = 0; x < im->w; x++) {
        float t = ps3ui__clamp01(((float)x - 4.0f) / ((float)im->w - 5.0f));
        float a = 190.0f * (1.0f - 0.5f * t - 0.5f * t * t * (3.0f - 2.0f * t));
        ps3ui__rect(im, x, 0, x + 1, im->h, ps3ui__hex4(0x0B152C, 0));
        ps3ui__rect(im, x, 3, x + 1, im->h - 3, ps3ui__hex4(0x0B152C, (int)(a + 0.5f)));
    }
}

/* ---- the connection-quality faces (disconnect01..04) ----------------------- */

/* A glossy disc in a bright 3px ring, green to red, with a face in the ring's
 * colour. The disc is lit from the lower right, (1 - d/80)^2.3 from a point
 * (14.6, 11.0) off centre, and darkens towards the ring; a gloss ellipse sits
 * in the upper left, cut off square short of the centre. */
typedef struct {
    float cx, cy;                   /* disc centre (the sprites are hand-placed) */
    uint32_t ring_top, ring_btm, dark, base, gloss, face;
} ps3ui__mood_t;

static const ps3ui__mood_t ps3ui__moods[4] = {
    { 23.80f, 23.50f, 0x1AFF0B, 0x00E200, 0x032803, 0x02A501, 0x54D632, 0x61FF00 },
    { 24.53f, 23.95f, 0xFAFF00, 0xD0E200, 0x1E2200, 0x98A500, 0xC3D100, 0xF0FF00 },
    { 24.28f, 23.95f, 0xFFC006, 0xE58100, 0x2E1300, 0xAC5F00, 0xDB8D15, 0xFFA800 },
    { 23.61f, 23.83f, 0xFF5432, 0xEA1A00, 0x420303, 0xB60C01, 0xEC2E19, 0xFF492B },
};

#define PS3UI__FACE_R 23.53f

/* Is (x, y) inside one of the face's strokes? Positions are measured off the
 * sprites. */
static int ps3ui__face_in(int mood, float x, float y)
{
    float d;
    switch (mood) {
    case 0:                                    /* ^ ^ eyes, a smile */
        for (int i = 0; i < 2; i++) {
            d = hypotf(x - (i ? 32.17f : 14.48f), y - 22.42f);
            if (y <= 22.42f && fabsf(d - 5.1f) < 1.17f) return 1;
        }
        d = hypotf(x - 23.82f, y - 25.46f);
        return y >= 32.51f && fabsf(d - 13.66f) < 1.25f;
    case 1:                                    /* upright oval eyes, a flat mouth */
        for (int i = 0; i < 2; i++) {
            float ex = (x - (i ? 32.09f : 16.74f)) / 2.72f, ey = (y - 19.0f) / 4.99f;
            if (ex * ex + ey * ey < 1.0f) return 1;
        }
        return x > 9.91f && x < 39.28f && y > 32.45f && y < 35.06f;
    case 2: {                                  /* half-closed eyes, a ^ mouth */
        for (int i = 0; i < 2; i++) {
            float ex = (x - (i ? 32.81f : 15.76f)) / 4.4f, ey = (y - 19.1f) / 6.37f;
            if (y >= 19.1f && ex * ex + ey * ey < 1.0f) return 1;
        }
        /* two strokes from the apex, 2.6 wide */
        static const float seg[2][4] = { { 24.02f, 31.06f, 13.98f, 38.65f }, { 24.02f, 31.06f, 33.61f, 38.63f } };
        for (int i = 0; i < 2; i++) {
            float ax = seg[i][0], ay = seg[i][1], bx = seg[i][2], by = seg[i][3];
            float vx = bx - ax, vy = by - ay, t = ((x - ax) * vx + (y - ay) * vy) / (vx * vx + vy * vy);
            if (t < 0.0f || t > 1.0f) continue;
            if (hypotf(x - ax - t * vx, y - ay - t * vy) < 1.32f) return 1;
        }
        return 0;
    }
    default:                                   /* slanted angry eyes, a frown */
        for (int i = 0; i < 2; i++) {
            float ex = i ? 31.98f : 15.03f, s = i ? -1.0f : 1.0f;
            /* a disc cut by the brow, a line falling towards the nose */
            if (hypotf(x - ex, y - 20.02f) < 5.53f && (y - 13.86f) - 0.87f * s * (x - (i ? 37.41f : 9.77f)) > 0.0f)
                return 1;
        }
        d = hypotf(x - 23.5f, y - 47.1f);
        return y <= 39.08f && fabsf(d - 14.56f) < 1.26f;
    }
}

static void ps3ui__paint_face(ps3ui_image_t *im, int mood)
{
    const ps3ui__mood_t *m = &ps3ui__moods[mood];
    const float R = PS3UI__FACE_R, Ri = R - 2.9f;
    int w = im->w, h = im->h;
    float *fm = (float *)calloc((size_t)w * (size_t)h, sizeof(float));
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int hit = 0;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++)
                    hit += ps3ui__face_in(mood, (float)x + 0.125f + 0.25f * (float)i, (float)y + 0.125f + 0.25f * (float)j);
            fm[y * w + x] = (float)hit / 16.0f;
        }
    ps3ui__c4 dark = ps3ui__hex4(m->dark, 255), base = ps3ui__hex4(m->base, 255);
    ps3ui__c4 gloss = ps3ui__hex4(m->gloss, 255), face = ps3ui__hex4(m->face, 255);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
            float r = hypotf(fx - m->cx, fy - m->cy);
            ps3ui__c4 ring = ps3ui__mix(ps3ui__hex4(m->ring_top, 255), ps3ui__hex4(m->ring_btm, 255),
                                        ps3ui__clamp01((fy - m->cy + R) / (2.0f * R)));
            /* the inside: lit from the lower right, a gloss up left, dark to the rim */
            float v = 1.0f - hypotf(fx - m->cx - 14.55f, fy - m->cy - 10.96f) / 80.0f;
            v = powf(ps3ui__clamp01(v), 2.3f);
            ps3ui__c4 c = ps3ui__mix(dark, base, v);
            float gx = (fx - m->cx + 7.77f) / 12.05f, gy = (fy - m->cy + 13.44f) / 9.94f;
            float g = 0.99f * expf(-(gx * gx + gy * gy)) * ps3ui__clamp01(m->cx + 5.06f - fx + 0.5f) *
                      ps3ui__clamp01(m->cy - 1.33f - fy + 0.5f);
            c = ps3ui__mix(c, gloss, g);
            c = ps3ui__mix(c, dark, 0.32f * ps3ui__fall(Ri - r, 7.0f, 1.5f));
            c = ps3ui__mix(c, face, fm[y * w + x]);
            /* the ring, antialiased on both edges */
            c = ps3ui__mix(c, ring, ps3ui__clamp01(r - Ri + 0.5f));
            c.a = 255.0f * ps3ui__clamp01(R - r + 0.5f);
            ps3ui__put(im, x, y, c);
        }
    free(fm);
}

/* ---- the progress arrows (process_arrow01..03): three triangles each ------- */

/* Nine steps from green to red, three to a sprite. */
static void ps3ui__paint_arrows(ps3ui_image_t *im, int set)
{
    static const uint32_t step[9] = { 0x15E000, 0x43E100, 0xB5E200, 0xD2E200, 0xD9C500,
                                      0xE29F00, 0xE78900, 0xE86600, 0xE93700 };
    /* left edge, half height, tip x; all centred on y 14 */
    static const float tri[3][3] = { { 0.83f, 13.51f, 22.78f }, { 26.38f, 13.47f, 48.27f }, { 51.94f, 13.42f, 73.92f } };
    for (int i = 0; i < 3; i++) {
        uint32_t c = step[set * 3 + i];
        ps3ui__rect(im, i == 0 ? 0 : i == 1 ? 24 : 50, 0, i == 0 ? 24 : i == 1 ? 50 : im->w, im->h, ps3ui__hex4(c, 0));
        ps3ui_path_t p;
        ps3ui_path_reset(&p);
        ps3ui_path_move(&p, tri[i][0], 14.0f - tri[i][1]);
        ps3ui_path_line(&p, tri[i][2], 14.0f);
        ps3ui_path_line(&p, tri[i][0], 14.0f + tri[i][1]);
        ps3ui_path_fill(im, &p, c, 1.0f);
    }
}

/* ---- discs, rings and flares ---------------------------------------------- */

/* Coverage of a disc, 4x4 samples. */
static float ps3ui__disc_cov(float cx, float cy, float r, int x, int y)
{
    int hit = 0;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            hit += hypotf((float)x + 0.125f + 0.25f * (float)i - cx, (float)y + 0.125f + 0.25f * (float)j - cy) < r;
    return (float)hit / 16.0f;
}

/* sphere_w / _b: a flat disc. */
static void ps3ui__paint_sphere(ps3ui_image_t *im, uint32_t rgb)
{
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++)
            ps3ui__put(im, x, y, ps3ui__hex4(rgb, (int)(255.0f * ps3ui__disc_cov(12.52f, 12.35f, 12.42f, x, y) + 0.5f)));
}

/* flare_b: a soft round shadow, 240 * (1 - (r/55.25)^1.44)^1.56. */
static void ps3ui__paint_flare(ps3ui_image_t *im, uint32_t rgb)
{
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) {
            float r = hypotf((float)x + 0.5f - 54.0f, (float)y + 0.5f - 54.0f) / 55.25f;
            float a = r < 1.0f ? 239.7f * powf(1.0f - powf(r, 1.444f), 1.559f) : 0.0f;
            ps3ui__put(im, x, y, ps3ui__hex4(rgb, (int)(a + 0.5f)));
        }
}

/* ring_flare_w / _b: a disc whose inside fades out towards a hole set a
 * little off its centre. */
static void ps3ui__paint_ring_flare(ps3ui_image_t *im, uint32_t rgb)
{
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) {
            float t = (hypotf((float)x + 0.5f - 45.52f, (float)y + 0.5f - 45.5f) - 30.9f) / (42.59f - 30.9f);
            float a = 234.1f * powf(ps3ui__clamp01(t), 1.179f) * ps3ui__disc_cov(45.13f, 45.73f, 45.13f, x, y);
            ps3ui__put(im, x, y, ps3ui__hex4(rgb, (int)(a + 0.5f)));
        }
}

/* ring_grada_w / _b: a 6px ring that starts square at twelve o'clock and
 * fades out clockwise over 259 degrees. */
static void ps3ui__paint_ring_grada(ps3ui_image_t *im, uint32_t rgb)
{
    const float cx = 51.12f, cy = 51.74f, ro = 51.04f, ri = 45.1f;
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) {
            float acc = 0.0f;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) {
                    float dx = (float)x + 0.125f + 0.25f * (float)i - cx, dy = (float)y + 0.125f + 0.25f * (float)j - cy;
                    float r = hypotf(dx, dy);
                    if (r >= ro || r < ri)
                        continue;
                    float th = atan2f(dx, -dy);
                    if (th < 0.0f) th += 6.2831853f;
                    float a = 279.7f * powf(ps3ui__clamp01(1.0f - th / 4.518f), 1.034f);
                    acc += a > 255.0f ? 255.0f : a;
                }
            ps3ui__put(im, x, y, ps3ui__hex4(rgb, (int)(acc / 16.0f + 0.5f)));
        }
}

/* ---- gold strokes with a dark brown edge (ring_line, slant_line) ---------- */

static float ps3ui__seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay, t = ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy);
    t = ps3ui__clamp01(t);
    return hypotf(px - ax - t * vx, py - ay - t * vy);
}

/* Distance to the drawing's centre lines. */
static float ps3ui__line_art_dist(int which, float x, float y)
{
    if (which) {                               /* slant_line: one rising line, turning up off the top */
        float d = ps3ui__seg_dist(x, y, -20.0f, 134.15f, 207.04f, 3.43f);
        return fminf(d, ps3ui__seg_dist(x, y, 207.04f, 3.43f, 207.04f, -10.0f));
    }
    /* ring_line: three circles, joined to a rail down the left edge */
    float d = fabsf(hypotf(x - 96.88f, y - 46.72f) - 42.41f);
    d = fminf(d, fabsf(hypotf(x - 170.87f, y - 45.76f) - 19.93f));
    d = fminf(d, fabsf(hypotf(x - 153.84f, y - 104.73f) - 33.52f));
    d = fminf(d, ps3ui__seg_dist(x, y, 3.65f, 98.8f, 3.65f, 200.9f));
    d = fminf(d, ps3ui__seg_dist(x, y, 3.65f, 98.8f, 58.09f, 67.95f));
    d = fminf(d, ps3ui__seg_dist(x, y, 3.65f, 190.9f, 121.86f, 123.0f));
    return fminf(d, ps3ui__seg_dist(x, y, 192.62f, 32.87f, 223.03f, 24.12f));
}

static void ps3ui__paint_line_art(ps3ui_image_t *im, int which)
{
    const float wc = which ? 1.37f : 1.46f, wo = which ? 3.49f : 3.57f;
    ps3ui__c4 gold = ps3ui__hex4(0xFFC600, 255), brown = ps3ui__hex4(0x6C5400, 255);
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) {
            int core = 0, out = 0;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) {
                    float d = ps3ui__line_art_dist(which, (float)x + 0.125f + 0.25f * (float)i,
                                                   (float)y + 0.125f + 0.25f * (float)j);
                    core += d < wc;
                    out += d < wo;
                }
            ps3ui__c4 c = out ? ps3ui__mix(brown, gold, (float)core / (float)out) : gold;
            c.a = 255.0f * (float)out / 16.0f;
            ps3ui__put(im, x, y, c);
        }
}

/* ---- "INFORMATION" and "NOW LOADING" ------------------------------------- */

/* tips_info_txt: grey letters over a white copy of themselves 3px lower, the
 * 1P/2P icons' emboss. */
static void ps3ui__paint_info_txt(ps3ui_image_t *im)
{
    const ps3ui__letter_t L[11] = {
        { 'I', 0.00f, 6.47f }, { 'N', 9.98f, 34.50f }, { 'F', 39.06f, 60.20f }, { 'O', 61.65f, 88.85f },
        { 'R', 90.97f, 115.81f }, { 'M', 117.39f, 146.60f }, { 'A', 149.71f, 176.93f }, { 'T', 178.09f, 201.93f },
        { 'I', 205.08f, 211.39f }, { 'O', 214.38f, 241.84f }, { 'N', 243.91f, 268.52f },
    };
    int w = im->w, h = im->h;
    float *m = ps3ui__letters(w, h, L, 11, 0.90f, 21.87f, 0.0f);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float cov = m[y * w + x], under = y >= 3 ? m[(y - 3) * w + x] : 0.0f;
            ps3ui__c4 c = ps3ui__hex4(0x73757B, 0);
            if (under > 0.0f)
                c = ps3ui__hex4(0xFFFFFF, (int)(255.0f * under));
            if (cov > 0.0f) {
                ps3ui__c4 g = ps3ui__hex4(0x73757B, 255);
                c = under > 0.0f ? ps3ui__mix(c, g, cov) : g;
                c.a = 255.0f * (cov + (1.0f - cov) * under);
            }
            ps3ui__put(im, x, y, c);
        }
    free(m);
}

/* now_loading: brushed chrome, light at the top, darkest two thirds down and
 * bright again at the foot, with a lighter rim inside the outline. */
static void ps3ui__paint_now_loading(ps3ui_image_t *im)
{
    const ps3ui__letter_t L[10] = {
        { 'N', 0.00f, 24.16f }, { 'O', 26.13f, 54.37f }, { 'W', 55.73f, 88.30f }, { 'L', 112.96f, 128.74f },
        { 'O', 133.65f, 162.61f }, { 'A', 163.74f, 190.92f }, { 'D', 193.93f, 219.37f }, { 'I', 222.25f, 228.14f },
        { 'N', 232.02f, 256.16f }, { 'G', 257.85f, 284.86f },
    };
    static const ps3ui__stop_t face[] = {
        { 2, 0xCCD5E1, 255 }, { 4, 0xC3CBD7, 255 }, { 6, 0xBAC2CE, 255 }, { 8, 0xACB3BE, 255 },
        { 10, 0x9EA5B0, 255 }, { 12, 0x9096A1, 255 }, { 14, 0x838993, 255 }, { 18, 0x6B7079, 255 },
        { 19, 0x7F848E, 255 }, { 20, 0xC6CDD8, 255 }, { 21, 0xE0E8F3, 255 },
    };
    int w = im->w, h = im->h;
    float *m = ps3ui__letters(w, h, L, 10, 0.89f, 21.87f, 0.0f);
    float *in = (float *)malloc(sizeof(float) * (size_t)w * (size_t)h);
    memcpy(in, m, sizeof(float) * (size_t)w * (size_t)h);
    ps3ui_blur(in, w, h, 0.8f);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float cov = m[y * w + x];
            ps3ui__c4 c = ps3ui__ramp(face, 11, (float)y);
            float edge = 1.0f - ps3ui__smooth(0.55f, 0.9f, in[y * w + x]);
            c = ps3ui__mix(c, ps3ui__hex4(0xD4DCE8, 255), 0.6f * edge);
            c.a = 255.0f * cov;
            ps3ui__put(im, x, y, c);
        }
    free(m);
    free(in);
}

/* ---- the pad guide's caption plate (pad_pla) ------------------------------ */

/* A translucent dark gold plate in a 2px brown frame, warming to gold over
 * its last hundred columns at each end, with a gold rail down its right edge
 * that runs on below it. */
static void ps3ui__paint_pad_pla(ps3ui_image_t *im)
{
    static const ps3ui__stop_t ramp[] = {
        { 0, 0xB58D00, 0xD6 }, { 6, 0xB28A00, 0xD5 }, { 14, 0xAC8600, 0xD2 }, { 30, 0x9F7B00, 0xCC },
        { 54, 0x836600, 0xC1 }, { 78, 0x664F00, 0xB6 }, { 94, 0x564300, 0xB1 }, { 102, 0x544100, 0xB0 },
    };
    ps3ui__c4 brown = ps3ui__hex4(0x6C5400, 255), gold = ps3ui__hex4(0xFFC600, 255);
    int w = im->w, rail = w - 5;               /* gold columns rail..rail+2 */
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < w; x++) {
            ps3ui__c4 c;
            if (x >= rail + 3) c = brown;
            else if (x >= rail) c = y < 2 ? brown : gold;
            else if (y >= 58) c = x == rail - 1 ? brown : x == rail - 2 ? ps3ui__hex4(0x6C5400, 0xF1)
                                                                        : ps3ui__hex4(0x6C5400, 0);
            else if (y < 2 || y >= 56 || x < 2) c = brown;
            else {
                int d = x - 2 < rail - 1 - x ? x - 2 : rail - 1 - x;
                c = ps3ui__ramp(ramp, 8, (float)d);
            }
            ps3ui__put(im, x, y, c);
        }
}

/* ---- the controller diagram (pad_ps3) -------------------------------------- */

/* A line drawing of the pad: a black body, grey buttons and sticks, and 1px
 * grey detail lines. Everything but the face buttons, select, start and the
 * PS button is drawn for the left half and mirrored about x = 173.5.
 * Positions are measured off the sprite's own lines. */
#define PS3UI__PAD_M 347.0f

/* A stroke: a segment (a,b)-(c,d), or an arc (arc=1: centre a,b, radius c,
 * degrees d..e, y down). */
typedef struct { int arc; float a, b, c, d, e; } ps3ui__pad_line_t;

static float ps3ui__pad_line_dist(const ps3ui__pad_line_t *l, float x, float y)
{
    if (!l->arc)
        return ps3ui__seg_dist(x, y, l->a, l->b, l->c, l->d);
    float dx = x - l->a, dy = y - l->b, th = atan2f(dy, dx) * 57.29578f;
    while (th < l->d) th += 360.0f;
    while (th >= l->d + 360.0f) th -= 360.0f;
    if (th <= l->e)
        return fabsf(hypotf(dx, dy) - l->c);
    float x0 = l->a + l->c * cosf(l->d / 57.29578f), y0 = l->b + l->c * sinf(l->d / 57.29578f);
    float x1 = l->a + l->c * cosf(l->e / 57.29578f), y1 = l->b + l->c * sinf(l->e / 57.29578f);
    return fminf(hypotf(x - x0, y - y0), hypotf(x - x1, y - y1));
}

/* Max-combine a stroke's coverage (half width hw) into m, and its mirror image. */
static void ps3ui__pad_stroke(float *m, int w, int h, ps3ui__pad_line_t l, float hw, int mirror)
{
    for (int k = 0; k <= mirror; k++) {
        if (k) {
            if (l.arc) {
                float d = l.d;
                l.a = PS3UI__PAD_M - l.a;
                l.d = 180.0f - l.e;
                l.e = 180.0f - d;
            } else {
                l.a = PS3UI__PAD_M - l.a;
                l.c = PS3UI__PAD_M - l.c;
            }
        }
        float bx0, by0, bx1, by1;
        if (l.arc) { bx0 = l.a - l.c; bx1 = l.a + l.c; by0 = l.b - l.c; by1 = l.b + l.c; }
        else { bx0 = fminf(l.a, l.c); bx1 = fmaxf(l.a, l.c); by0 = fminf(l.b, l.d); by1 = fmaxf(l.b, l.d); }
        int x0 = (int)floorf(bx0 - hw - 1.0f), x1 = (int)ceilf(bx1 + hw + 1.0f);
        int y0 = (int)floorf(by0 - hw - 1.0f), y1 = (int)ceilf(by1 + hw + 1.0f);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                int hit = 0;
                for (int j = 0; j < 4; j++)
                    for (int i = 0; i < 4; i++)
                        hit += ps3ui__pad_line_dist(&l, (float)x + 0.125f + 0.25f * (float)i,
                                                    (float)y + 0.125f + 0.25f * (float)j) < hw;
                float c = (float)hit / 16.0f;
                if (c > m[y * w + x])
                    m[y * w + x] = c;
            }
    }
}

static void ps3ui__pad_strokes(float *m, int w, int h, const ps3ui__pad_line_t *l, int n, float hw, int mirror)
{
    for (int i = 0; i < n; i++)
        ps3ui__pad_stroke(m, w, h, l[i], hw, mirror);
}

/* A closed polygon from a half list of points, and its mirror image. */
static void ps3ui__pad_poly(ps3ui_path_t *p, const float (*pt)[2], int n)
{
    ps3ui_path_move(p, pt[0][0], pt[0][1]);
    for (int i = 1; i < n; i++)
        ps3ui_path_line(p, pt[i][0], pt[i][1]);
    ps3ui_path_move(p, PS3UI__PAD_M - pt[n - 1][0], pt[n - 1][1]);
    for (int i = n - 2; i >= 0; i--)
        ps3ui_path_line(p, PS3UI__PAD_M - pt[i][0], pt[i][1]);
}

static void ps3ui__pad_ink(ps3ui_image_t *im, float *m, uint32_t rgb)
{
    float c[3];
    ps3ui_rgb_hex(rgb, c);
    for (int i = 0; i < im->w * im->h; i++)
        ps3ui_image_over(im, i % im->w, i / im->w, c, m[i]);
    memset(m, 0, sizeof(float) * (size_t)im->w * (size_t)im->h);
}

#define PS3UI__N(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void ps3ui__paint_pad(ps3ui_image_t *im)
{
    const float M = PS3UI__PAD_M, D2R = 0.01745329f;
    int w = im->w, h = im->h;
    ps3ui_path_t p;

    /* the body: the left half's outline from the top middle, down round the
     * grip and up round the stick's housing, then its mirror image */
    static const float body[][2] = {
        { 120.4f, 42.2f }, { 110.4f, 35.0f }, { 107.8f, 30.9f }, { 103.8f, 26.4f }, { 103.9f, 16.9f },
        { 105.8f, 12.4f }, { 105.9f, 9.5f }, { 103.5f, 6.1f }, { 98.4f, 3.2f }, { 92.5f, 1.2f }, { 88.9f, 0.6f },
        { 88.6f, 0.0f }, { 68.8f, 0.0f }, { 68.4f, 0.6f }, { 64.5f, 1.2f }, { 59.4f, 3.1f }, { 56.1f, 5.6f },
        { 52.2f, 13.5f }, { 51.2f, 16.9f }, { 51.0f, 20.4f }, { 45.1f, 25.5f }, { 41.8f, 31.6f }, { 30.1f, 46.2f },
        { 24.2f, 56.2f }, { 20.1f, 67.2f }, { 12.2f, 100.8f }, { 10.1f, 115.0f }, { 6.8f, 148.8f }, { 0.8f, 186.5f },
        { 0.0f, 187.4f }, { 0.0f, 201.8f }, { 0.6f, 202.2f }, { 2.1f, 210.4f }, { 4.2f, 215.5f }, { 6.1f, 218.4f },
        { 11.4f, 223.8f }, { 18.0f, 228.6f }, { 22.1f, 230.9f }, { 29.0f, 233.2f }, { 29.2f, 233.9f },
        { 44.5f, 233.9f }, { 44.8f, 233.2f }, { 50.1f, 231.6f }, { 55.8f, 228.6f }, { 63.8f, 221.8f },
        { 92.0f, 183.9f },
    };
    const float scx = 126.65f, scy = 175.42f, sr = 33.5f;
    ps3ui_path_reset(&p);
    ps3ui_path_move(&p, body[0][0], body[0][1]);
    for (int i = 1; i < PS3UI__N(body); i++)
        ps3ui_path_line(&p, body[i][0], body[i][1]);
    ps3ui_path_arc(&p, scx, scy, sr, 167.2f * D2R, -3.5f * D2R, 24);
    ps3ui_path_line(&p, 160.5f, 172.2f);
    ps3ui_path_line(&p, M - 160.5f, 172.2f);
    ps3ui_path_arc(&p, M - scx, scy, sr, 183.5f * D2R, 12.8f * D2R, 24);
    for (int i = PS3UI__N(body) - 1; i >= 0; i--)
        ps3ui_path_line(&p, M - body[i][0], body[i][1]);
    ps3ui_path_fill(im, &p, 0x000000, 1.0f);

    /* grey parts: shoulder buttons, d-pad, sticks, face buttons, select, start */
    static const float l2[][2] = {
        { 53.4f, 18.6f }, { 57.1f, 17.6f }, { 64.5f, 16.6f }, { 81.9f, 16.7f }, { 93.5f, 19.8f }, { 101.8f, 23.6f },
        { 102.2f, 17.2f }, { 104.5f, 10.5f }, { 103.8f, 8.6f }, { 102.2f, 7.1f }, { 93.9f, 3.2f }, { 83.2f, 1.6f },
        { 73.6f, 1.6f }, { 63.5f, 3.1f }, { 58.9f, 5.2f }, { 56.2f, 8.1f },
    };
    static const float l1[][2] = {
        { 49.0f, 38.2f }, { 50.5f, 39.4f }, { 59.4f, 38.0f }, { 73.1f, 37.6f }, { 79.1f, 38.0f }, { 89.0f, 39.6f },
        { 93.5f, 39.6f }, { 94.6f, 35.4f }, { 94.9f, 30.2f }, { 93.9f, 28.4f }, { 90.6f, 26.4f }, { 79.9f, 24.1f },
        { 66.5f, 23.8f }, { 57.2f, 25.0f }, { 52.9f, 27.1f }, { 51.0f, 29.4f },
    };
    ps3ui_path_reset(&p);
    ps3ui__pad_poly(&p, l2, PS3UI__N(l2));
    ps3ui__pad_poly(&p, l1, PS3UI__N(l1));
    /* the d-pad: four pentagons pointing in at (69.5, 117.8) */
    static const float pent[5][2] = { { -8.6f, -26.9f }, { 8.4f, -26.9f }, { 8.4f, -14.4f }, { 0.0f, -7.8f }, { -8.6f, -14.4f } };
    for (int k = 0; k < 4; k++) {
        float cs = k == 0 ? 1.0f : k == 2 ? -1.0f : 0.0f, sn = k == 1 ? 1.0f : k == 3 ? -1.0f : 0.0f;
        for (int i = 0; i < 5; i++) {
            float x = 69.5f + pent[i][0] * cs - pent[i][1] * sn, y = 117.8f + pent[i][0] * sn + pent[i][1] * cs;
            if (i == 0) ps3ui_path_move(&p, x, y);
            else ps3ui_path_line(&p, x, y);
        }
    }
    for (int k = 0; k < 2; k++) {
        float cx = k ? M - 126.6f : 126.6f;
        ps3ui_path_move(&p, cx + 23.8f, 176.1f);
        ps3ui_path_arc(&p, cx, 176.1f, 23.8f, 0.0f, 6.2831853f, 64);
    }
    static const float face[4][2] = { { 277.5f, 88.0f }, { 248.0f, 117.8f }, { 307.3f, 117.8f }, { 277.5f, 147.5f } };
    for (int k = 0; k < 4; k++) {
        ps3ui_path_move(&p, face[k][0] + 11.9f, face[k][1]);
        ps3ui_path_arc(&p, face[k][0], face[k][1], 11.9f, 0.0f, 6.2831853f, 48);
    }
    ps3ui_path_rect(&p, 136.0f, 113.5f, 149.0f, 120.0f);
    ps3ui_path_move(&p, 197.5f, 113.2f);
    ps3ui_path_line(&p, 213.0f, 117.2f);
    ps3ui_path_line(&p, 197.5f, 121.2f);
    ps3ui_path_fill(im, &p, 0x656666, 1.0f);

    float *m = (float *)calloc((size_t)w * (size_t)h, sizeof(float));
    /* dark grooves across the shoulder buttons and round the sticks' tops */
    static const ps3ui__pad_line_t groove[] = {
        { 0, 56.5f, 14.8f, 66.0f, 13.3f, 0 }, { 0, 66.0f, 13.3f, 84.0f, 13.3f, 0 }, { 0, 84.0f, 13.3f, 98.5f, 18.0f, 0 },
        { 0, 53.5f, 36.2f, 62.0f, 35.2f, 0 }, { 0, 62.0f, 35.2f, 78.0f, 35.2f, 0 }, { 0, 78.0f, 35.2f, 90.0f, 37.6f, 0 },
        { 1, 126.6f, 176.1f, 21.9f, 195.0f, 345.0f },
    };
    ps3ui__pad_strokes(m, w, h, groove, PS3UI__N(groove), 0.7f, 1);
    ps3ui__pad_ink(im, m, 0x000000);

    /* the detail lines */
    static const ps3ui__pad_line_t lines[] = {
        /* the grip and shoulder housing */
        { 0, 17.9f, 79.9f, 25.9f, 66.2f, 0 }, { 0, 25.9f, 66.2f, 38.5f, 52.8f, 0 }, { 0, 38.5f, 52.8f, 45.8f, 46.8f, 0 },
        { 0, 45.8f, 46.8f, 56.4f, 43.5f, 0 }, { 0, 56.4f, 43.5f, 71.5f, 43.1f, 0 }, { 0, 71.5f, 43.1f, 84.8f, 44.9f, 0 },
        { 0, 84.8f, 44.9f, 92.9f, 48.5f, 0 }, { 0, 92.9f, 48.5f, 96.6f, 59.9f, 0 }, { 0, 96.6f, 59.9f, 100.6f, 73.1f, 0 },
        { 0, 42.6f, 35.2f, 36.4f, 64.6f, 0 },
        { 0, 97.5f, 35.5f, 101.4f, 42.1f, 0 }, { 0, 101.4f, 42.1f, 103.7f, 55.9f, 0 },
        { 0, 99.6f, 38.0f, 109.6f, 71.6f, 0 }, { 0, 108.4f, 52.8f, 117.5f, 45.0f, 0 }, { 0, 107.7f, 39.3f, 102.3f, 65.5f, 0 },
        { 0, 103.6f, 53.9f, 109.8f, 71.0f, 0 },
        { 0, 65.4f, 215.0f, 91.6f, 177.5f, 0 },
        /* the circle round the d-pad, and the stick's housing */
        { 1, 69.56f, 117.95f, 54.92f, 6.0f, 28.0f }, { 1, 69.56f, 117.95f, 54.92f, 62.0f, 115.0f },
        { 1, 69.56f, 117.95f, 54.92f, 128.0f, 352.0f },
        { 1, 126.65f, 175.42f, 32.34f, 254.0f, 555.0f },
        { 0, 120.4f, 139.3f, 123.5f, 128.9f, 0 },
        /* the d-pad's plate: a plus, x 24.5..114.5 by y 101.5..133.5, arms 53.5..85.5 */
        { 0, 55.5f, 73.5f, 83.5f, 73.5f, 0 }, { 0, 53.5f, 75.5f, 53.5f, 101.5f, 0 }, { 0, 85.5f, 75.5f, 85.5f, 101.5f, 0 },
        { 0, 26.5f, 101.5f, 53.5f, 101.5f, 0 }, { 0, 85.5f, 101.5f, 112.5f, 101.5f, 0 },
        { 0, 24.5f, 103.5f, 24.5f, 131.5f, 0 }, { 0, 114.5f, 103.5f, 114.5f, 131.5f, 0 },
        { 0, 26.5f, 133.5f, 53.5f, 133.5f, 0 }, { 0, 85.5f, 133.5f, 112.5f, 133.5f, 0 },
        { 0, 53.5f, 133.5f, 53.5f, 160.5f, 0 }, { 0, 85.5f, 133.5f, 85.5f, 160.5f, 0 }, { 0, 55.5f, 162.5f, 83.5f, 162.5f, 0 },
        { 1, 55.5f, 75.5f, 2.0f, 180.0f, 270.0f }, { 1, 83.5f, 75.5f, 2.0f, 270.0f, 360.0f },
        { 1, 26.5f, 103.5f, 2.0f, 180.0f, 270.0f }, { 1, 112.5f, 103.5f, 2.0f, 270.0f, 360.0f },
        { 1, 26.5f, 131.5f, 2.0f, 90.0f, 180.0f }, { 1, 112.5f, 131.5f, 2.0f, 0.0f, 90.0f },
        { 1, 55.5f, 160.5f, 2.0f, 90.0f, 180.0f }, { 1, 83.5f, 160.5f, 2.0f, 0.0f, 90.0f },
        /* the rails across the top */
        { 0, 103.7f, 55.5f, 160.5f, 55.5f, 0 }, { 0, 116.8f, 74.5f, 173.5f, 74.5f, 0 },
        /* the USB port: two rounded outlines */
        { 0, 163.5f, 47.5f, 173.5f, 47.5f, 0 }, { 0, 163.5f, 58.5f, 173.5f, 58.5f, 0 }, { 0, 160.5f, 50.5f, 160.5f, 55.5f, 0 },
        { 1, 163.5f, 50.5f, 3.0f, 180.0f, 270.0f }, { 1, 163.5f, 55.5f, 3.0f, 90.0f, 180.0f },
        { 0, 165.0f, 49.5f, 173.5f, 49.5f, 0 }, { 0, 165.0f, 56.5f, 173.5f, 56.5f, 0 }, { 0, 163.5f, 51.0f, 163.5f, 55.0f, 0 },
        { 0, 162.0f, 169.5f, 173.5f, 169.5f, 0 },
    };
    ps3ui__pad_strokes(m, w, h, lines, PS3UI__N(lines), 0.65f, 1);
    /* the d-pad's arrows, outlined */
    static const float tri[4][3][2] = {
        { { 64.3f, 82.8f }, { 74.7f, 82.8f }, { 69.5f, 77.3f } }, { { 64.3f, 152.8f }, { 74.7f, 152.8f }, { 69.5f, 158.3f } },
        { { 34.2f, 112.6f }, { 34.2f, 123.0f }, { 28.7f, 117.8f } }, { { 104.8f, 112.6f }, { 104.8f, 123.0f }, { 110.3f, 117.8f } },
    };
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < 3; i++) {
            ps3ui__pad_line_t l = { 0, tri[k][i][0], tri[k][i][1], tri[k][(i + 1) % 3][0], tri[k][(i + 1) % 3][1], 0 };
            ps3ui__pad_stroke(m, w, h, l, 0.6f, 0);
        }
    ps3ui__pad_ink(im, m, 0x7E7E7F);
    /* the PS button, a dimmer ring */
    ps3ui__pad_line_t ps = { 1, 173.4f, 136.8f, 11.7f, 0.0f, 360.0f };
    ps3ui__pad_stroke(m, w, h, ps, 1.0f, 0);
    ps3ui__pad_ink(im, m, 0x505050);

    /* the face buttons' symbols */
    static const uint32_t sym[4] = { 0x00A4C1, 0xE288AC, 0xD43261, 0x6488CC };
    for (int k = 0; k < 4; k++) {
        float cx = face[k][0], cy = face[k][1];
        if (k == 0) {
            float v[3][2] = { { cx, cy - 10.0f }, { cx + 8.6f, cy + 5.0f }, { cx - 8.6f, cy + 5.0f } };
            for (int i = 0; i < 3; i++) {
                ps3ui__pad_line_t l = { 0, v[i][0], v[i][1], v[(i + 1) % 3][0], v[(i + 1) % 3][1], 0 };
                ps3ui__pad_stroke(m, w, h, l, 1.0f, 0);
            }
        } else if (k == 1) {
            const float a = 6.3f;
            float v[4][2] = { { cx - a, cy - a }, { cx + a, cy - a }, { cx + a, cy + a }, { cx - a, cy + a } };
            for (int i = 0; i < 4; i++) {
                ps3ui__pad_line_t l = { 0, v[i][0], v[i][1], v[(i + 1) % 4][0], v[(i + 1) % 4][1], 0 };
                ps3ui__pad_stroke(m, w, h, l, 0.85f, 0);
            }
        } else if (k == 2) {
            ps3ui__pad_line_t l = { 1, cx + 0.4f, cy, 8.4f, 0.0f, 360.0f };
            ps3ui__pad_stroke(m, w, h, l, 1.0f, 0);
        } else {
            ps3ui__pad_line_t l1 = { 0, cx - 7.0f, cy - 7.0f, cx + 7.0f, cy + 7.0f, 0 };
            ps3ui__pad_line_t l2 = { 0, cx + 7.0f, cy - 7.0f, cx - 7.0f, cy + 7.0f, 0 };
            ps3ui__pad_stroke(m, w, h, l1, 1.0f, 0);
            ps3ui__pad_stroke(m, w, h, l2, 1.0f, 0);
        }
        ps3ui__pad_ink(im, m, sym[k]);
    }
    free(m);
}

/* ---- sprites no layout references: the program draws them itself ---------- */

enum {
    PS3UI_SPR_X_NET_00 = PS3UI_SPR_COUNT,   /* signal: none, 1, 2, 3 bars, unknown */
    PS3UI_SPR_X_NET_01,
    PS3UI_SPR_X_NET_02,
    PS3UI_SPR_X_NET_03,
    PS3UI_SPR_X_NET_XX,
    PS3UI_SPR_X_BTN_CIRCLE,                 /* the pad's face buttons, as font 1 draws them */
    PS3UI_SPR_X_BTN_CROSS,
    PS3UI_SPR_X_BTN_TRIANGLE,
    PS3UI_SPR_X_BTN_SQUARE,
    PS3UI_SPR_ALL
};

/* Button glyphs ride in strings as these control characters. */
#define PS3UI_BTN_CIRCLE   '\x01'
#define PS3UI_BTN_CROSS    '\x02'
#define PS3UI_BTN_TRIANGLE '\x03'
#define PS3UI_BTN_SQUARE   '\x04'

static void ps3ui__round_rect(ps3ui_path_t *p, float x0, float y0, float x1, float y1, float r)
{
    const float h = 1.5707963f;
    ps3ui_path_move(p, x0 + r, y0);
    ps3ui_path_line(p, x1 - r, y0);
    ps3ui_path_arc(p, x1 - r, y0 + r, r, -h, 0.0f, 6);
    ps3ui_path_line(p, x1, y1 - r);
    ps3ui_path_arc(p, x1 - r, y1 - r, r, 0.0f, h, 6);
    ps3ui_path_line(p, x0 + r, y1);
    ps3ui_path_arc(p, x0 + r, y1 - r, r, h, 2.0f * h, 6);
    ps3ui_path_line(p, x0, y0 + r);
    ps3ui_path_arc(p, x0 + r, y0 + r, r, 2.0f * h, 3.0f * h, 6);
}

/* The signal icon: a rounded plate, an antenna and 0-3 bars. */
static void ps3ui__paint_net(ps3ui_image_t *im, int level)
{
    static const uint32_t fill[5] = { 0x999999, 0x009600, 0x00C800, 0x00FF00, 0x999999 };
    int xx = level == 4, o = xx ? 1 : 0;       /* the "unknown" icon is 50x50, its plate one in */
    ps3ui_path_t p;
    ps3ui_path_reset(&p);
    ps3ui__round_rect(&p, (float)o, (float)o, (float)(o + 48), (float)(o + 48), 4.0f);
    ps3ui_path_fill(im, &p, 0x000000, 1.0f);
    ps3ui_path_reset(&p);
    ps3ui__round_rect(&p, (float)(o + 2), (float)(o + 2), (float)(o + 46), (float)(o + 46), 2.0f);
    ps3ui_path_fill(im, &p, fill[level], 1.0f);
    ps3ui__c4 k = ps3ui__hex4(0x000000, 255);
    /* antenna: an inverted triangle drawn in 3px strokes, on a mast */
    for (int y = 11; y <= 22; y++) {
        int L = 3 + (y - 11), R = 29 - (y - 11);
        for (int x = L; x <= R; x++)
            if (y <= 13 || y >= 20 || x <= L + 2 || x >= R - 2)
                ps3ui__put(im, x + o, y + o, k);
    }
    ps3ui__rect(im, 15 + o, 14 + o, 18 + o, 41 + o, k);
    static const int bx[3] = { 23, 31, 39 }, top[3] = { 28, 21, 11 };
    for (int b = 0; b < 3 && !xx; b++)
        if (level > b)
            ps3ui__rect(im, bx[b], top[b], bx[b] + 3, 41, k);
    if (xx) {
        /* a red slash from the bottom left to the top right */
        ps3ui_path_reset(&p);
        ps3ui_path_move(&p, 45.0f, 0.5f);
        ps3ui_path_line(&p, 50.0f, 3.5f);
        ps3ui_path_line(&p, 4.0f, 49.5f);
        ps3ui_path_line(&p, 0.0f, 46.5f);
        ps3ui_path_fill(im, &p, 0xFF1010, 1.0f);
    }
}

/* A face button: a bevelled dark disc with the symbol in its colour. */
static void ps3ui__paint_button(ps3ui_image_t *im, int which)
{
    const float cx = 24.0f, cy = 24.0f;
    ps3ui_path_t p;
    ps3ui_path_reset(&p);
    ps3ui_path_move(&p, cx + 22.0f, cy);
    ps3ui_path_arc(&p, cx, cy, 22.0f, 0.0f, 6.2831853f, 64);
    ps3ui_path_fill(im, &p, 0x211C21, 1.0f);
    /* inside the 2px ring: a bevel, lighter towards the lower right, round a flat face */
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy, r = sqrtf(dx * dx + dy * dy);
            if (r > 20.0f)
                continue;
            float lit = (dx + dy) / (r > 0.0f ? r : 1.0f) * 0.3536f + 0.5f;
            ps3ui__c4 bevel = ps3ui__mix(ps3ui__hex4(0x4D4A4D, 255), ps3ui__hex4(0x636163, 255), lit);
            ps3ui__c4 c = r > 16.5f ? bevel : ps3ui__hex4(0x333433, 255);
            float edge = 20.0f - r;
            if (edge < 1.0f)
                c = ps3ui__mix(ps3ui__hex4(0x211C21, 255), c, edge);
            ps3ui__put(im, x, y, c);
        }
    static const uint32_t col[4] = { 0xFF4118, 0x7B75B5, 0x39C78C, 0xDE5DDE };
    float *m = (float *)calloc((size_t)im->w * (size_t)im->h, sizeof(float));
    ps3ui_path_reset(&p);
    const float s = 1.5f;                      /* half the 3px stroke */
    switch (which) {
    case 0:                                    /* circle */
        ps3ui_path_move(&p, cx + 10.5f + s, cy);
        ps3ui_path_arc(&p, cx, cy, 10.5f + s, 0.0f, 6.2831853f, 48);
        ps3ui_path_move(&p, cx + 10.5f - s, cy);
        ps3ui_path_arc(&p, cx, cy, 10.5f - s, 6.2831853f, 0.0f, 48);
        break;
    case 1: {                                  /* cross, about 24 across */
        const float a = 10.5f, d = s * 0.7071f;
        ps3ui_path_move(&p, cx - a - d, cy - a + d);
        ps3ui_path_line(&p, cx - a + d, cy - a - d);
        ps3ui_path_line(&p, cx + a + d, cy + a - d);
        ps3ui_path_line(&p, cx + a - d, cy + a + d);
        ps3ui_path_move(&p, cx + a - d, cy - a - d);
        ps3ui_path_line(&p, cx + a + d, cy - a + d);
        ps3ui_path_line(&p, cx - a + d, cy + a + d);
        ps3ui_path_line(&p, cx - a - d, cy + a - d);
        break;
    }
    case 2:                                    /* triangle outline: outer contour, inner reversed */
        for (int k = 0; k < 2; k++) {
            float r = k ? 13.0f - 2.0f * s : 13.0f + 2.0f * s, yo = 2.0f;
            float ay = cy + yo - r, by = cy + yo + r * 0.5f, bx = cx + r * 0.866f, qx = cx - r * 0.866f;
            ps3ui_path_move(&p, cx, ay);
            if (!k) {
                ps3ui_path_line(&p, bx, by);
                ps3ui_path_line(&p, qx, by);
            } else {
                ps3ui_path_line(&p, qx, by);
                ps3ui_path_line(&p, bx, by);
            }
        }
        break;
    default: {                                 /* square outline */
        float a = 10.0f + s, b = 10.0f - s;
        ps3ui_path_rect(&p, cx - a, cy - a, cx + a, cy + a);
        ps3ui_path_move(&p, cx - b, cy - b);
        ps3ui_path_line(&p, cx - b, cy + b);
        ps3ui_path_line(&p, cx + b, cy + b);
        ps3ui_path_line(&p, cx + b, cy - b);
        break;
    }
    }
    ps3ui_path_cover(&p, m, im->w, im->h);
    float c[3];
    ps3ui_rgb_hex(col[which], c);
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++)
            ps3ui_image_over(im, x, y, c, m[y * im->w + x]);
    free(m);
}

/* Button glyphs sit on the text's baseline like a font 1 character: the disc
 * spans 42 above the baseline to 2 below at cap 37, and advances 48. */
static float ps3ui_button_adv(float cap) { return 48.0f * cap / 37.0f; }

static const ps3ui_image_t *ps3ui_sprite(int id);

static void ps3ui_draw_button(ps3ui_canvas_t *cv, int code, float x, float baseline, float cap, float opacity)
{
    const ps3ui_image_t *im = ps3ui_sprite(PS3UI_SPR_X_BTN_CIRCLE + (code - PS3UI_BTN_CIRCLE));
    float k = cap / 37.0f;
    ps3ui_mat_t m = ps3ui_mat_mul(ps3ui_mat_translate(x, baseline - 44.0f * k), ps3ui_mat_scale(k, k));
    ps3ui_draw_image(cv, im, m, opacity, PS3UI_BLEND_NORMAL, NULL);
}

/* ---- the sprite table ------------------------------------------------------ */

static struct {
    ps3ui_image_t img[PS3UI_SPR_ALL];
    uint8_t done[PS3UI_SPR_ALL];
} g_ps3ui_sprites;

static void ps3ui_paint(int id, ps3ui_image_t *im)
{
    switch (id) {
    case PS3UI_SPR_CHIP_BLACK:  ps3ui__rect(im, 0, 0, im->w, im->h, ps3ui__hex4(0x000000, 255)); break;
    case PS3UI_SPR_CHIP_WHITE:  ps3ui__rect(im, 0, 0, im->w, im->h, ps3ui__hex4(0xFFFFFF, 255)); break;
    case PS3UI_SPR_CHIP_CYAN:   ps3ui__rect(im, 0, 0, im->w, im->h, ps3ui__hex4(0x00FFFF, 255)); break;
    case PS3UI_SPR_CHIP_D_BLUE: ps3ui__rect(im, 0, 0, im->w, im->h, ps3ui__hex4(0x184684, 255)); break;
    case PS3UI_SPR_CHIP_L_BLUE: ps3ui__rect(im, 0, 0, im->w, im->h, ps3ui__hex4(0x00BAFF, 255)); break;
    case PS3UI_SPR_BG_GRADA:
        for (int y = 0; y < im->h; y++)
            ps3ui__rect(im, 0, y, im->w, y + 1,
                        ps3ui__mix(ps3ui__hex4(0x122868, 255), ps3ui__hex4(0x001735, 255),
                                   (float)y / (float)(im->h - 1)));
        break;
    case PS3UI_SPR_HW_PLA_CENTER:
        ps3ui__rect(im, 0, 0, im->w, im->h, ps3ui__hex4(PS3UI__PANEL, PS3UI__PANEL_A));
        break;
    case PS3UI_SPR_HW_PLA_OVR_MID: case PS3UI_SPR_HW_PLA_SIDE: case PS3UI_SPR_HW_PLA_UDR_MID:
    case PS3UI_SPR_HW_PLA_OVR_CNR: case PS3UI_SPR_HW_PLA_UDR_CNR:
        ps3ui__paint_pla(im, id);
        break;
    case PS3UI_SPR_HW_OVR_LINE_MID:
        ps3ui__rect(im, 0, 0, 3, im->h, ps3ui__hex4(PS3UI__GREY, 255));
        ps3ui__rect(im, 3, 0, 13, im->h, ps3ui__hex4(0xFFFFFF, 255));
        ps3ui__rect(im, 13, 0, 16, im->h, ps3ui__hex4(PS3UI__GREY, 255));
        break;
    case PS3UI_SPR_HW_OVR_LINE_EDG:
        ps3ui__rect(im, 0, 0, 16, 16, ps3ui__hex4(PS3UI__GREY, 255));
        ps3ui__rect(im, 3, 0, 13, 13, ps3ui__hex4(0xFFFFFF, 0xFD));
        break;
    case PS3UI_SPR_HW_UDR_LINE_MID:
        ps3ui__rect(im, 0, 0, 3, im->h, ps3ui__hex4(PS3UI__GREY, 255));
        ps3ui__rect(im, 3, 0, 6, im->h, ps3ui__hex4(0xFFFFFF, 0xF6));
        ps3ui__rect(im, 6, 0, 9, im->h, ps3ui__hex4(PS3UI__GREY, 255));
        ps3ui__rect(im, 9, 0, 10, im->h, ps3ui__hex4(0xFFFFFF, 0));
        break;
    case PS3UI_SPR_HW_UDR_LINE_CNR:
        /* the thin rail's outside corner, bottom right: grey, white, grey bent 90 degrees */
        for (int y = 0; y < im->h; y++)
            for (int x = 0; x < im->w; x++) {
                ps3ui__c4 c = ps3ui__hex4(PS3UI__GREY, 255);
                if (x < 7 && y < 7 && (x >= 4 || y >= 4)) c = ps3ui__hex4(0xFFFFFF, 0xF8);
                if (x == 0 && y == 0) c = ps3ui__hex4(0xFFFFFF, 0);
                ps3ui__put(im, x, y, c);
            }
        break;
    case PS3UI_SPR_HW_OVR_LINE_CNR_L: ps3ui__paint_tab(im); break;
    case PS3UI_SPR_PLA_S_EDG:        ps3ui__paint_plate(im, 204.0f, 1, 1, 1, 1); break;
    case PS3UI_SPR_PLA_S_MID:        ps3ui__paint_plate(im, 204.0f, 1, 1, 0, 0); break;
    case PS3UI_SPR_MATCH_WIN_TOP_CNR:
        /* the bottom border stops short of the left edge: the panel's white
         * side rail runs on down from there */
        ps3ui__paint_plate(im, 200.0f, 1, 1, 1, 1);
        ps3ui__rect(im, 3, im->h - 3, 13, im->h, ps3ui__hex4(0xFFFFFF, 0xFC));
        break;
    case PS3UI_SPR_MATCH_WIN_TOP_MID: ps3ui__paint_plate(im, 200.0f, 1, 1, 0, 0); break;
    case PS3UI_SPR_MATCH_WIN_BTM_CNR:
        ps3ui__rect(im, 0, 0, im->w, im->h, ps3ui__hex4(0xFFFFFF, 255));
        ps3ui__rect(im, 0, 0, 3, im->h, ps3ui__hex4(PS3UI__GREY, 255));
        ps3ui__rect(im, 0, 13, im->w, im->h, ps3ui__hex4(PS3UI__GREY, 255));
        ps3ui__rect(im, 13, 0, 16, 3, ps3ui__hex4(PS3UI__GREY, 255));
        break;
    case PS3UI_SPR_MATCH_S_WIN_MID:  ps3ui__paint_glass(im, 0); break;
    case PS3UI_SPR_MATCH_S_WIN_SIDE: ps3ui__paint_glass(im, 1); break;
    case PS3UI_SPR_MATCH_VS_VS:      ps3ui__paint_vs(im); break;
    case PS3UI_SPR_MATCH_VS_VS_LINE_LIGHT: ps3ui__paint_dash(im); break;
    case PS3UI_SPR_MATCH_VS_Z_LINE_JOINT_A: ps3ui__paint_joint(im, 0); break;
    case PS3UI_SPR_MATCH_VS_Z_LINE_JOINT_B: ps3ui__paint_joint(im, 1); break;
    case PS3UI_SPR_MATCH_READY:       ps3ui__paint_ready(im); break;
    case PS3UI_SPR_MATCH_READY_LIGHT: ps3ui__paint_ready_light(im); break;
    case PS3UI_SPR_PLA_ICON_1P:       ps3ui__paint_player_icon(im, 0); break;
    case PS3UI_SPR_PLA_ICON_2P:       ps3ui__paint_player_icon(im, 1); break;
    case PS3UI_SPR_HW_OVR_LINE_CNR_S: ps3ui__paint_tab_s(im); break;
    case PS3UI_SPR_PLA_DECO_CUBE:    ps3ui__paint_deco_cube(im); break;
    case PS3UI_SPR_PLA_M_EDG:        ps3ui__paint_plate_m(im, 1); break;
    case PS3UI_SPR_PLA_M_MID:        ps3ui__paint_plate_m(im, 0); break;
    case PS3UI_SPR_MATCH_CURSOR_TOP_CNR: ps3ui__paint_cursor(im, 1, 0, 1, 0); break;
    case PS3UI_SPR_MATCH_CURSOR_TOP_MID: ps3ui__paint_cursor(im, 1, 0, 0, 0); break;
    case PS3UI_SPR_MATCH_CURSOR_SIDE:    ps3ui__paint_cursor(im, 0, 0, 1, 0); break;
    case PS3UI_SPR_MATCH_CURSOR_SIDE_S:  ps3ui__paint_cursor(im, 1, 1, 1, 1); break;
    case PS3UI_SPR_MATCH_CURSOR_MID_S:   ps3ui__paint_cursor(im, 1, 1, 0, 0); break;
    case PS3UI_SPR_MATCH_CURSOR_BTM_CNR: ps3ui__paint_cursor(im, 0, 1, 1, 0); break;
    case PS3UI_SPR_MATCH_CURSOR_BTM_MID: ps3ui__paint_cursor(im, 0, 1, 0, 0); break;
    case PS3UI_SPR_MATCH_CURSOR_BG:      ps3ui__paint_cursor(im, 0, 0, 0, 0); break;
    case PS3UI_SPR_MATCH_SEL_INFO_TOP_CNR: ps3ui__paint_sel_info(im, 1, 0, 1); break;
    case PS3UI_SPR_MATCH_SEL_INFO_BTM_CNR: ps3ui__paint_sel_info(im, 0, 1, 1); break;
    case PS3UI_SPR_MATCH_SEL_INFO_BTM_MID: ps3ui__paint_sel_info(im, 0, 1, 0); break;
    case PS3UI_SPR_MATCH_SEL_INFO_MID:     ps3ui__paint_sel_info(im, 1, 0, 0); break;
    case PS3UI_SPR_MATCH_SEL_INFO_SIDE:    ps3ui__paint_sel_info(im, 0, 0, 1); break;
    case PS3UI_SPR_MATCH_SEL_INFO_CHIP:    ps3ui__paint_sel_info(im, 0, 0, 0); break;
    case PS3UI_SPR_MATCH_SEL_INFO_TIT:     ps3ui__paint_sel_info_tit(im); break;
    case PS3UI_SPR_MATCH_SEL_MID:    ps3ui__paint_sel(im, 0); break;
    case PS3UI_SPR_MATCH_SEL_SIDE:   ps3ui__paint_sel(im, 1); break;
    case PS3UI_SPR_MATCH_S_WIN_PLT:  ps3ui__paint_s_win_plt(im); break;
    case PS3UI_SPR_CSR_GRA_DRK:      ps3ui__paint_csr_gra_drk(im); break;
    case PS3UI_SPR_DISCONNECT01:     ps3ui__paint_face(im, 0); break;
    case PS3UI_SPR_DISCONNECT02:     ps3ui__paint_face(im, 1); break;
    case PS3UI_SPR_DISCONNECT03:     ps3ui__paint_face(im, 2); break;
    case PS3UI_SPR_DISCONNECT04:     ps3ui__paint_face(im, 3); break;
    case PS3UI_SPR_PROCESS_ARROW01:  ps3ui__paint_arrows(im, 0); break;
    case PS3UI_SPR_PROCESS_ARROW02:  ps3ui__paint_arrows(im, 1); break;
    case PS3UI_SPR_PROCESS_ARROW03:  ps3ui__paint_arrows(im, 2); break;
    case PS3UI_SPR_SPHERE_W:         ps3ui__paint_sphere(im, 0xFFFFFF); break;
    case PS3UI_SPR_SPHERE_B:         ps3ui__paint_sphere(im, 0x001334); break;
    case PS3UI_SPR_FLARE_B:          ps3ui__paint_flare(im, 0x001334); break;
    case PS3UI_SPR_RING_FLARE_W:     ps3ui__paint_ring_flare(im, 0xFFFFFF); break;
    case PS3UI_SPR_RING_FLARE_B:     ps3ui__paint_ring_flare(im, 0x001334); break;
    case PS3UI_SPR_RING_GRADA_W:     ps3ui__paint_ring_grada(im, 0xFFFFFF); break;
    case PS3UI_SPR_RING_GRADA_B:     ps3ui__paint_ring_grada(im, 0x001334); break;
    case PS3UI_SPR_RING_LINE:        ps3ui__paint_line_art(im, 0); break;
    case PS3UI_SPR_SLANT_LINE:       ps3ui__paint_line_art(im, 1); break;
    case PS3UI_SPR_TIPS_INFO_TXT:    ps3ui__paint_info_txt(im); break;
    case PS3UI_SPR_NOW_LOADING:      ps3ui__paint_now_loading(im); break;
    case PS3UI_SPR_PAD_PLA:          ps3ui__paint_pad_pla(im); break;
    case PS3UI_SPR_PAD_PS3:          ps3ui__paint_pad(im); break;
    case PS3UI_SPR_X_NET_00: case PS3UI_SPR_X_NET_01: case PS3UI_SPR_X_NET_02: case PS3UI_SPR_X_NET_03:
    case PS3UI_SPR_X_NET_XX:
        ps3ui__paint_net(im, id - PS3UI_SPR_X_NET_00);
        break;
    case PS3UI_SPR_X_BTN_CIRCLE: case PS3UI_SPR_X_BTN_CROSS: case PS3UI_SPR_X_BTN_TRIANGLE:
    case PS3UI_SPR_X_BTN_SQUARE:
        ps3ui__paint_button(im, id - PS3UI_SPR_X_BTN_CIRCLE);
        break;
    default:
        if (id < PS3UI_SPR_COUNT)
            fprintf(stderr, "ps3ui: sprite %s is not painted yet\n", ps3ui_sprite_info[id].name);
        break;
    }
}

/* Size of a sprite: the layout's, or for the extras, their own. */
static void ps3ui_sprite_size(int id, int *w, int *h)
{
    if (id < PS3UI_SPR_COUNT) {
        *w = ps3ui_sprite_info[id].w;
        *h = ps3ui_sprite_info[id].h;
    } else if (id == PS3UI_SPR_X_NET_XX) {
        *w = *h = 50;
    } else {
        *w = *h = 48;
    }
}

static const ps3ui_image_t *ps3ui_sprite(int id)
{
    if (id < 0 || id >= PS3UI_SPR_ALL)
        return NULL;
    if (!g_ps3ui_sprites.done[id]) {
        int w, h;
        ps3ui_sprite_size(id, &w, &h);
        g_ps3ui_sprites.done[id] = 1;
        g_ps3ui_sprites.img[id] = ps3ui_image_new(w, h);
        ps3ui_paint(id, &g_ps3ui_sprites.img[id]);
    }
    return &g_ps3ui_sprites.img[id];
}

#endif /* PS3UI_SPRITES_H */
