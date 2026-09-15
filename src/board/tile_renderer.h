/*
 * tile_renderer.h — Model 2 2D tile compositor (board-level).
 *
 * The Model 2B has four scrolling tile layers (A/B foreground, C/D background)
 * composited over the 3D scene.  All layers share the same hardware format.
 *
 * Memory layout (offsets into the TILE region at 0x01000000):
 *   0x0000–0x3FFF  foreground tilemap  (layers A/B, text/HUD)
 *   0x4000–0x7FFF  background tilemap  (layers C/D, sky/stage)
 *   TMAPGFX region (0x01080000)  character graphics, 8×8 tiles, 4bpp
 *   PALETTE region (0x01800000)  RGB555 color entries
 *
 * Tilemap entry (16-bit, little-endian):
 *   bit 15     priority / enable
 *   bit 14     H-flip
 *   bits 13-7  palette bank (7-bit, 0–127 → 128 × 16-color palettes)
 *   bits 6-0   tile character index (7-bit)
 *   full tile index  = entry & 0x3FFF  (same as (pal_bank<<7)|char)
 *   palette LUT idx  = pal_bank * 16 + color_idx  (stride=16, 32 bytes/bank)
 *
 * Pixel format invariants (per CLAUDE.md — do NOT re-derive):
 *   16-bit byteswap on pixel bytes: indices [0,1,2,3] read as [1,0,3,2]
 *   (XOR low bit of byte index within each 16-bit word).
 *   Within each swapped byte: high nibble = left pixel, low nibble = right.
 *   Color index 0 is transparent on foreground layers only; background layers fully opaque.
 *
 * Palette format: BGR555 packed as little-endian u16.
 *   R = bits [4:0]   G = bits [9:5]   B = bits [14:10]
 */
#ifndef TILE_RENDERER_H
#define TILE_RENDERER_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "memory.h"

/* ---- Color conversion ---------------------------------------------------- */

#define BGR555_R(c) ( ((c)        & 0x1F) << 3 )
#define BGR555_G(c) ( (((c) >>  5) & 0x1F) << 3 )
#define BGR555_B(c) ( (((c) >> 10) & 0x1F) << 3 )

/* ---- Layer buffers ------------------------------------------------------- */

typedef struct {
    uint16_t *bg;       /* background layer [VIDEO_WIDTH × VIDEO_HEIGHT] BGR555 */
    uint16_t *fg;       /* foreground layer [VIDEO_WIDTH × VIDEO_HEIGHT] BGR555 */
    uint8_t  *alpha;    /* foreground alpha [VIDEO_WIDTH × VIDEO_HEIGHT] 0=transparent */
    uint8_t  *bg_alpha; /* background alpha — 0 where the back-back color shows through */
} tile_layers_t;

static inline int tile_layers_init(tile_layers_t *t) {
    int n = VIDEO_WIDTH * VIDEO_HEIGHT;
    t->bg       = (uint16_t *)calloc(n, sizeof(uint16_t));
    t->fg       = (uint16_t *)calloc(n, sizeof(uint16_t));
    t->alpha    = (uint8_t  *)calloc(n, sizeof(uint8_t));
    t->bg_alpha = (uint8_t  *)calloc(n, sizeof(uint8_t));
    return (t->bg && t->fg && t->alpha && t->bg_alpha) ? 1 : 0;
}

static inline void tile_layers_free(tile_layers_t *t) {
    free(t->bg);       t->bg       = NULL;
    free(t->fg);       t->fg       = NULL;
    free(t->alpha);    t->alpha    = NULL;
    free(t->bg_alpha); t->bg_alpha = NULL;
}

/* ---- Palette read -------------------------------------------------------- */

/* Read one BGR555 entry from palette RAM. */
static inline uint16_t pal_read16(const memory_bus_t *bus, int idx) {
    uint32_t off = (uint32_t)idx * 2;
    if (off + 1 < PALETTE_SIZE)
        return (uint16_t)(bus->palette[off] | (bus->palette[off + 1] << 8));
    return 0;
}

/* ---- Tile pixel decode --------------------------------------------------- */

/*
 * Decode one 4bpp pixel at (px, py) within tile tile_idx.
 * Each 8×8 tile = 32 bytes; each row = 4 bytes = 8 pixels.
 * Byteswap: within each 16-bit word the two bytes are swapped (XOR byte
 * index with 1).  Then: high nibble = left pixel, low nibble = right pixel.
 */
static inline uint8_t tile_pixel_4bpp(const memory_bus_t *bus,
                                       int tile_idx, int px, int py) {
    uint32_t base  = (uint32_t)tile_idx * 32;
    int byte_pair  = px >> 1;           /* 0–3 within row */
    int swapped    = byte_pair ^ 1;     /* 16-bit byteswap: swap 0↔1, 2↔3 */
    uint32_t boff  = base + (uint32_t)py * 4 + (uint32_t)swapped;
    if (boff >= TMAPGFX_SIZE) return 0;
    uint8_t b = bus->tmapgfx[boff];
    return (px & 1) ? (b & 0x0F) : (b >> 4);
}

/* ---- Layer render -------------------------------------------------------- */

/*
 * Render one tile layer into a BGR555 output buffer.
 *
 *   output       : VIDEO_WIDTH × VIDEO_HEIGHT u16 destination
 *   alpha_out    : VIDEO_WIDTH × VIDEO_HEIGHT u8  alpha (NULL = opaque/BG)
 *   tmap_offset  : byte offset of the tilemap within bus->tile[]
 *   map_w/map_h  : tilemap dimensions in tiles
 *   scroll_x/y   : scroll offset in pixels (wraps at map_w*8, map_h*8)
 */
/* Read a 16-bit LE word from the tile RAM at a WORD offset (MAME tile_ram[]).
 * tile_ram is word-addressed; our bus->tile is byte-addressed → byte = word*2. */
static inline uint16_t tileram_word(const memory_bus_t *bus, uint32_t word_off) {
    uint32_t b = word_off * 2;
    if (b + 1 >= TILE_SIZE) return 0;
    return (uint16_t)(bus->tile[b] | (bus->tile[b + 1] << 8));
}

/* Sample one tile-layer pixel at world coords (wx,wy) of the tilemap at
 * `tmap_offset` (byte offset into bus->tile).  Returns BGR555 colour; sets
 * *color_idx (0 = transparent on FG layers).
 *
 * Sega System 24 tile cell (MAME segaic24 tile_info):
 *   color (palette bank) = (val >> 7) & 0xFF   (8-bit, bits[14:7]; bit14 is a
 *     palette bit, NOT h_flip — change_bg_color uses it)
 *   char index           = val & 0x3FFF
 *   priority/category     = bit 15
 */
static inline uint16_t tile_sample_px(const memory_bus_t *bus, uint32_t tmap_offset,
                                      int map_w, int map_h, int wx, int wy,
                                      uint8_t *color_idx, uint8_t *prio) {
    int map_px_w = map_w * 8;
    int map_px_h = map_h * 8;
    wx &= (map_px_w - 1);
    wy &= (map_px_h - 1);

    int tile_col = wx >> 3, tile_row = wy >> 3;
    int px = wx & 7, py = wy & 7;

    uint32_t te_off = tmap_offset + (uint32_t)(tile_row * map_w + tile_col) * 2;
    uint16_t entry  = 0;
    if (te_off + 1 < TILE_SIZE)
        entry = (uint16_t)(bus->tile[te_off] | (bus->tile[te_off + 1] << 8));

    int pal_bank = (entry >> 7) & 0xFF;
    int tile_idx = entry & 0x3FFF;

    uint8_t ci = tile_pixel_4bpp(bus, tile_idx, px, py);
    if (color_idx) *color_idx = ci;
    if (prio)      *prio = (uint8_t)((entry >> 15) & 1);   /* bit15: 1 = in front of 3D */
    return pal_read16(bus, pal_bank * 16 + ci);   /* stride = 16 */
}

static inline void render_tile_layer(const memory_bus_t *bus,
                                      uint16_t *output,
                                      uint8_t  *alpha_out,
                                      uint32_t  tmap_offset,
                                      int map_w, int map_h,
                                      int scroll_x, int scroll_y) {
    for (int sy = 0; sy < VIDEO_HEIGHT; sy++) {
        for (int sx = 0; sx < VIDEO_WIDTH; sx++) {
            uint8_t  color_idx;
            uint16_t color = tile_sample_px(bus, tmap_offset, map_w, map_h,
                                            sx + scroll_x, sy + scroll_y, &color_idx, NULL);
            int i = sy * VIDEO_WIDTH + sx;
            output[i] = color;
            if (alpha_out)
                alpha_out[i] = (color_idx != 0) ? 255 : 0;
        }
    }
}

/* ---- Board-layer convenience wrappers ------------------------------------ */

/*
 * Back-back (backdrop) color: solid background behind the BG tile layer.
 * This is palette entry 0 — MAME's screen_update does bitmap.fill(pen(0)) as the
 * backdrop before drawing any tilemap.  (change_bg_color's 0x1002 write is a tile
 * palette entry, not the backdrop.)
 */
static inline uint16_t back_color_555(const memory_bus_t *bus) {
    if (!bus->palette || PALETTE_SIZE < 2) return 0;
    return (uint16_t)bus->palette[0] | ((uint16_t)bus->palette[1] << 8);
}

/*
 * The Sega System 24 tilemap chip, drawn the way MAME's segaic24 draw_common
 * does and layered the way model2_v.cpp screen_update does.
 *
 * Four 64x64 tilemaps, t = 0..3, at tile RAM words 0x1000*t. Each has its own
 * H scroll (word 0x5000+t) and V scroll (0x5004+t; bit 15 disables it). A
 * pixel samples its tilemap at (x - hscroll, y + vscroll), wrapping at 512.
 * H scroll bit 15 takes a per-line value from 0x4000 + 0x200*t instead.
 *
 * The tilemaps come in pairs, 0/1 and 2/3, sharing a control word (0x5004 and
 * 0x5006, bits 14:13) and a window mask (0x6000 and 0x6800):
 *   - control 0: both tilemaps draw, and the mask chooses between them. It holds
 *     four words a line, one bit per 8 pixels, MSB first; the even tilemap draws
 *     where the bit is 0 and the odd one where it is 1. This is how a screen is
 *     cut into arbitrary regions: STF's NEXT MATCH halves the screen along a
 *     zigzag, one fighter's art in each tilemap.
 *   - control 1: the pair splits at line v = -vscroll; bit 9 of -vscroll
 *     picks which tilemap is above. Control 2/3: it splits at column h = the
 *     H scroll value, bit 9 picking which is left. The odd tilemap's registers
 *     are ignored.
 *
 * Tile bit 15 is the tile's category. Behind the 3D: tilemaps 3 and 2 opaque
 * (pen 0 of a nonzero bank still shows), then tilemaps 1 and 0, category 0 only.
 * In front of the 3D: tilemaps 3, 2, 1, 0, category 1 only. Colour index 0 is
 * transparent wherever the draw is not opaque.
 */
static inline uint16_t s24_sample(const memory_bus_t *bus, int t, int x, int y,
                                  uint8_t *ci, uint8_t *cat, int *pen) {
    x &= 511; y &= 511;
    uint16_t entry = tileram_word(bus, 0x1000u * (uint32_t)t + (uint32_t)((y >> 3) * 64 + (x >> 3)));
    int bank = (entry >> 7) & 0xFF;
    *ci  = tile_pixel_4bpp(bus, entry & 0x3FFF, x & 7, y & 7);
    *cat = (uint8_t)((entry >> 15) & 1);
    *pen = bank * 16 + *ci;
    return pal_read16(bus, *pen);
}

/* Draw tilemap t into dst: category `cat` only (non-transparent pixels), or
 * every pixel when `opaque`. dst_alpha gets 255 where something drew (for an
 * opaque draw, where the pen is not 0, which MAME copies as transparent). */
static inline void s24_draw_tilemap(const memory_bus_t *bus, int t, int cat, bool opaque,
                                    uint16_t *dst, uint8_t *dst_alpha) {
    uint16_t hscr = tileram_word(bus, 0x5000u + (uint32_t)t);
    uint16_t vscr = tileram_word(bus, 0x5004u + (uint32_t)t);
    uint16_t ctrl = tileram_word(bus, 0x5004u + (uint32_t)(t & 2));
    if (vscr & 0x8000) return;
    int mode = (ctrl & 0x6000) >> 13;
    if (mode && (t & 1)) return;                 /* split modes: the even tilemap draws both */
    uint32_t hscrtb = 0x4000u + 0x200u * (uint32_t)t;
    uint32_t maskw  = (t & 2) ? 0x6800u : 0x6000u;
    int vy = vscr & 0x1FF;

    for (int y = 0; y < VIDEO_HEIGHT; y++) {
        uint16_t row = (hscr & 0x8000) ? tileram_word(bus, hscrtb + (uint32_t)y) : hscr;
        int h = row & 0x1FF;
        int split_l = t, split_x = 0x7FFF;       /* x < split_x: split_l, else split_l ^ 1 */
        if (mode == 1) {
            int nv = (-(int)vscr) & 0x3FF, v = nv & 0x1FF;
            split_l = (nv & 0x200) ? t : t ^ 1;
            if (y >= v) split_l ^= 1;
        } else if (mode) {
            split_l = (row & 0x200) ? t : t ^ 1;
            split_x = h;
        }
        for (int x = 0; x < VIDEO_WIDTH; x++) {
            int l = t;
            if (mode) {
                l = (x < split_x) ? split_l : (split_l ^ 1);
            } else {
                uint16_t m = tileram_word(bus, maskw + (uint32_t)(y * 4 + (x >> 7)));
                if (t & 1) m = (uint16_t)~m;
                if (m & (0x8000 >> ((x & 127) >> 3))) continue;
            }
            uint8_t ci, pc; int pen;
            uint16_t color = s24_sample(bus, l, x - h, y + vy, &ci, &pc, &pen);
            int i = y * VIDEO_WIDTH + x;
            if (opaque) {
                dst[i] = color;
                if (dst_alpha) dst_alpha[i] = pen ? 255 : 0;
            } else if (ci != 0 && pc == (uint8_t)cat) {
                dst[i] = color;
                if (dst_alpha) dst_alpha[i] = 255;
            }
        }
    }
}

/* Everything behind the 3D: the backdrop (palette 0), tilemaps 3 and 2 opaque,
 * then tilemaps 1 and 0 where their tiles are category 0. */
static inline void render_bg_layer(const memory_bus_t *bus, tile_layers_t *t) {
    int n = VIDEO_WIDTH * VIDEO_HEIGHT;
    uint16_t backc = back_color_555(bus);
    for (int i = 0; i < n; i++) { t->bg[i] = backc; t->bg_alpha[i] = 0; }
    s24_draw_tilemap(bus, 3, 0, true,  t->bg, t->bg_alpha);
    s24_draw_tilemap(bus, 2, 0, true,  t->bg, t->bg_alpha);
    s24_draw_tilemap(bus, 1, 0, false, t->bg, t->bg_alpha);
    s24_draw_tilemap(bus, 0, 0, false, t->bg, t->bg_alpha);
}

/* Everything in front of the 3D: tilemaps 3, 2, 1, 0 where their tiles are
 * category 1. */
static inline void render_fg_layer(const memory_bus_t *bus, tile_layers_t *t) {
    int n = VIDEO_WIDTH * VIDEO_HEIGHT;
    memset(t->fg, 0, (size_t)n * sizeof t->fg[0]);
    memset(t->alpha, 0, (size_t)n);
    for (int k = 3; k >= 0; k--) s24_draw_tilemap(bus, k, 1, false, t->fg, t->alpha);
}

/* ---- Pen colour --------------------------------------------------------- */

/*
 * A 15-bit palette colour as the screen shows it. The board does not put
 * palette RAM on screen as is: each channel's 5 bits index its colour
 * translation table at luma 0x40, and the result goes through the monitor
 * curve max((v - 64) * 255 / 191, 0) — model2.cpp palette_w, the same tables
 * and curve the 3D fill ends on. Skipping it made every tile a brighter,
 * more saturated colour than the polygons drawn against it, so a 3D cloud or
 * sign whose edge matches the sky on the board showed as a box.
 *
 * Built once a frame into `lut` (0x8000 entries, RGB). Until the game has
 * loaded the tables (all zero at boot, and never for a program that does not
 * use them) the palette colour is shown directly.
 */
static inline void tile_pen_lut(const memory_bus_t *bus, uint8_t lut[0x8000][3]) {
    int loaded = 0;
    for (int c5 = 0; c5 < 32 && !loaded; c5++)
        for (int ch = 0; ch < 3 && !loaded; ch++)
            if (bus->colorxlat[((uint32_t)ch * 0x2000u + 0x40u + ((uint32_t)c5 << 8)) * 2u]) loaded = 1;
    for (int c = 0; c < 0x8000; c++) {
        int c5[3] = { c & 0x1F, (c >> 5) & 0x1F, (c >> 10) & 0x1F };
        for (int ch = 0; ch < 3; ch++) {
            if (!loaded) { lut[c][ch] = (uint8_t)(c5[ch] << 3); continue; }
            int v = bus->colorxlat[((uint32_t)ch * 0x2000u + 0x40u + ((uint32_t)c5[ch] << 8)) * 2u];
            int g = (v - 64) * 255 / 191;
            lut[c][ch] = (uint8_t)(g < 0 ? 0 : g);
        }
    }
}

/* ---- Compositor ---------------------------------------------------------- */

/*
 * Composite BG + FG into an RGBA8 output buffer.
 * Layer order: BG → (3D scene slot — empty until Phase 9) → FG.
 */
static inline void composite_layers(const tile_layers_t *t,
                                     uint8_t *rgba_out,
                                     int width, int height) {
    int n = width * height;
    for (int i = 0; i < n; i++) {
        uint16_t c = t->bg[i];
        if (t->alpha[i]) c = t->fg[i];
        int o = i * 4;
        rgba_out[o + 0] = (uint8_t)BGR555_R(c);
        rgba_out[o + 1] = (uint8_t)BGR555_G(c);
        rgba_out[o + 2] = (uint8_t)BGR555_B(c);
        rgba_out[o + 3] = 255;
    }
}

#endif /* TILE_RENDERER_H */
