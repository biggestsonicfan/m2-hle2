/*
 * app_icon.h — the emulator's own icon, drawn into RGBA8 at any size.
 *
 * One piece of art for the two places the shell shows it: the window/taskbar
 * icon sokol_app uploads at startup, and the notification-area icon capture
 * mode puts in the tray (kiosk.h). It is drawn rather than loaded so there is
 * no .ico resource to keep in step with the build, and so each size is
 * rendered at its own resolution instead of being a blurry downscale of one
 * bitmap — a tray icon is 16px on a 100% desktop and 20 or 24px past that.
 *
 * The design grid is 16x16: a rounded plate, deep board-blue to near-black,
 * with a cyan rim and "M2" across the middle. The plate is supersampled so its
 * corners are smooth; the glyphs are 5x7 pixel art scaled by whole pixels
 * (k = size/16) so they stay crisp rather than fuzzy at every size.
 */
#ifndef APP_ICON_H
#define APP_ICON_H

#include <math.h>
#include <stdint.h>

#define APP_ICON_GLYPH_W 5
#define APP_ICON_GLYPH_H 7

static const char *const app_icon_glyph_m[APP_ICON_GLYPH_H] = {
    "X...X",
    "XX.XX",
    "X.X.X",
    "X.X.X",
    "X...X",
    "X...X",
    "X...X",
};
static const char *const app_icon_glyph_2[APP_ICON_GLYPH_H] = {
    ".XXX.",
    "X...X",
    "....X",
    "...X.",
    "..X..",
    ".X...",
    "XXXXX",
};

/* Signed distance to a rounded rect: negative inside, 0 on the edge. */
static inline float app_icon__sdf(float px, float py,
                                  float x0, float y0, float x1, float y1, float r) {
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    const float hx = (x1 - x0) * 0.5f - r, hy = (y1 - y0) * 0.5f - r;
    float dx = fabsf(px - cx) - hx, dy = fabsf(py - cy) - hy;
    const float ax = dx > 0.0f ? dx : 0.0f, ay = dy > 0.0f ? dy : 0.0f;
    float inside = (dx > dy ? dx : dy);
    if (inside > 0.0f) inside = 0.0f;
    return sqrtf(ax * ax + ay * ay) + inside - r;
}

static inline uint8_t app_icon__u8(float v) {
    v = v * 255.0f + 0.5f;
    if (v < 0.0f)   v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    return (uint8_t)v;
}

/*
 * Render the icon at `size` x `size` into `rgba`, R,G,B,A bytes per pixel
 * (what sapp_image_desc wants; kiosk.h swizzles to BGRA for the DIB).
 */
static inline void app_icon_render(int size, uint8_t *rgba) {
    if (size < 8) return;
    const float fs    = (float)size;
    const float inset = fs * 0.025f;          /* leave the outermost pixel clear */
    const float rad   = fs * 0.20f;
    const float rim   = fs * 0.055f;          /* rim: one crisp pixel at 16px */
    const float x0 = inset, y0 = inset, x1 = fs - inset, y1 = fs - inset;

    for (int y = 0; y < size; y++) {
        /* Vertical gradient down the plate: board blue into near-black. */
        const float t = (size > 1) ? (float)y / (float)(size - 1) : 0.0f;
        const float br = 0.090f + (0.030f - 0.090f) * t;
        const float bg = 0.140f + (0.052f - 0.140f) * t;
        const float bb = 0.260f + (0.105f - 0.260f) * t;
        for (int x = 0; x < size; x++) {
            int in = 0, on_rim = 0;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    const float px = (float)x + ((float)sx + 0.5f) * 0.25f;
                    const float py = (float)y + ((float)sy + 0.5f) * 0.25f;
                    const float d  = app_icon__sdf(px, py, x0, y0, x1, y1, rad);
                    if (d <= 0.0f) { in++; if (d >= -rim) on_rim++; }
                }
            }
            const float a  = (float)in / 16.0f;
            const float rf = (a > 0.0f) ? ((float)on_rim / 16.0f) / a : 0.0f;
            /* Rim cyan mixed over the body by how much of the pixel it covers. */
            const float r = br + (0.20f - br) * rf;
            const float g = bg + (0.85f - bg) * rf;
            const float b = bb + (0.94f - bb) * rf;
            uint8_t *p = rgba + ((size_t)y * (size_t)size + (size_t)x) * 4;
            p[0] = app_icon__u8(r);
            p[1] = app_icon__u8(g);
            p[2] = app_icon__u8(b);
            p[3] = app_icon__u8(a);
        }
    }

    /* "M2" in whole pixels, centred on the plate. */
    int k = size / 16;
    if (k < 1) k = 1;
    const int block_w = (APP_ICON_GLYPH_W * 2 + 2) * k;   /* M, 2-wide gap, 2 */
    const int block_h = APP_ICON_GLYPH_H * k;
    const int gx = (size - block_w) / 2;
    const int gy = (size - block_h) / 2;
    for (int gi = 0; gi < 2; gi++) {
        const char *const *glyph = gi ? app_icon_glyph_2 : app_icon_glyph_m;
        const int ox = gx + gi * (APP_ICON_GLYPH_W + 2) * k;
        for (int row = 0; row < APP_ICON_GLYPH_H; row++) {
            for (int col = 0; col < APP_ICON_GLYPH_W; col++) {
                if (glyph[row][col] != 'X') continue;
                for (int dy = 0; dy < k; dy++) {
                    const int py = gy + row * k + dy;
                    if (py < 0 || py >= size) continue;
                    for (int dx = 0; dx < k; dx++) {
                        const int px = ox + col * k + dx;
                        if (px < 0 || px >= size) continue;
                        uint8_t *p = rgba + ((size_t)py * (size_t)size + (size_t)px) * 4;
                        p[0] = 232; p[1] = 252; p[2] = 255; p[3] = 255;
                    }
                }
            }
        }
    }
}

#endif /* APP_ICON_H */
