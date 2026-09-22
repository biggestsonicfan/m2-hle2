/*
 * tile_renderer.h — Model 2 2D tile compositor (board-level).
 *
 * The Sega System 24 tilemap chip: four scrolling 64x64 tilemaps of 8x8 4bpp
 * cells, composited behind and in front of the 3D scene (s24_draw_tilemap,
 * below, for the register model).
 *
 * Memory: the TILE region (0x01000000) holds the four tilemaps at words
 * 0x1000*t, the per-line H scroll tables, the scroll and control registers
 * and the window masks; TMAPGFX (0x01080000) the cell graphics, 32 bytes an
 * 8x8 cell; PALETTE (0x01800000) the BGR555 colours.
 *
 * Tilemap entry (16-bit, little-endian):
 *   bit 15     category (priority against the 3D)
 *   bits 14-7  palette bank (8-bit; bit 14 is a palette bit, not H-flip —
 *              see tile_pixel_4bpp)
 *   cell index       = entry & 0x3FFF
 *   palette index    = pal_bank * 16 + color_idx  (stride=16, 32 bytes/bank)
 *
 * Pixel format invariants (per CLAUDE.md — do NOT re-derive):
 *   16-bit byteswap on pixel bytes: indices [0,1,2,3] read as [1,0,3,2]
 *   (XOR low bit of byte index within each 16-bit word).
 *   Within each swapped byte: high nibble = left pixel, low nibble = right.
 *   Color index 0 is transparent wherever a draw is not opaque.
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

/* ---- Tile RAM ------------------------------------------------------------ */

/* Read a 16-bit LE word from the tile RAM at a WORD offset (MAME tile_ram[]).
 * tile_ram is word-addressed; our bus->tile is byte-addressed → byte = word*2. */
static inline uint16_t tileram_word(const memory_bus_t *bus, uint32_t word_off) {
    uint32_t b = word_off * 2;
    if (b + 1 >= TILE_SIZE) return 0;
    return (uint16_t)(bus->tile[b] | (bus->tile[b + 1] << 8));
}

/* ---- The four tilemaps --------------------------------------------------- */

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
/* Draw tilemap t into dst: category `cat` only (non-transparent pixels), or
 * every pixel when `opaque`. dst_alpha gets 255 where something drew (for an
 * opaque draw, where the pen is not 0, which MAME copies as transparent).
 *
 * A pixel of tilemap l at (x - h, y + vy) & 511 is cell ((y + vy) >> 3, (x - h)
 * >> 3)'s pixel ((x - h) & 7, (y + vy) & 7). The line's window-mask words and
 * split are fixed per line; a cell's row of eight pixels is decoded once when
 * the pixel walk enters the cell, and a cell that cannot draw in this pass
 * (wrong category, or blank -- most of a HUD layer) is stepped over whole;
 * the palette is read only for a pixel that is written. Eight full-screen
 * passes a compose made this the largest cost of a frame on the CPU path
 * (D3D11); tests/tile_test.c holds it to the pixel-by-pixel original. */
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
        uint16_t mask[4] = { 0, 0, 0, 0 };       /* mode 0: one bit per 8 pixels, MSB first */
        if (mode == 1) {
            int nv = (-(int)vscr) & 0x3FF, v = nv & 0x1FF;
            split_l = (nv & 0x200) ? t : t ^ 1;
            if (y >= v) split_l ^= 1;
        } else if (mode) {
            split_l = (row & 0x200) ? t : t ^ 1;
            split_x = h;
        } else {
            for (int k = 0; k < 4; k++) {
                uint16_t m = tileram_word(bus, maskw + (uint32_t)(y * 4 + k));
                mask[k] = (t & 1) ? (uint16_t)~m : m;
            }
        }
        int      ty   = (y + vy) & 511;
        uint32_t cell = (uint32_t)(ty >> 3) * 64u;     /* the tilemap row's first cell */
        uint16_t *drow = dst + y * VIDEO_WIDTH;
        uint8_t  *arow = dst_alpha ? dst_alpha + y * VIDEO_WIDTH : NULL;
        int     cur = -1;                        /* l << 6 | column of the decoded cell */
        int     bank16 = 0;
        bool    dead = false;                    /* no pixel of the cell can draw in this pass */
        uint8_t pc = 0, nib[8] = { 0 };
        for (int x = 0; x < VIDEO_WIDTH; x++) {
            int l = t;
            if (mode) l = (x < split_x) ? split_l : (split_l ^ 1);
            else if (mask[x >> 7] & (0x8000 >> ((x & 127) >> 3))) continue;
            int tx  = (x - h) & 511;
            int key = (l << 6) | (tx >> 3);
            if (key != cur) {
                cur = key;
                uint16_t entry = tileram_word(bus, 0x1000u * (uint32_t)l + cell + (uint32_t)(tx >> 3));
                bank16 = ((entry >> 7) & 0xFF) * 16;
                pc     = (uint8_t)((entry >> 15) & 1);
                unsigned any = 0;
                for (int p = 0; p < 8; p++) any |= nib[p] = tile_pixel_4bpp(bus, entry & 0x3FFF, p, ty & 7);
                dead = !opaque && (pc != (uint8_t)cat || !any);
            }
            if (dead) {
                /* a non-opaque draw writes a pixel only where ci != 0 and the
                 * category matches: none in this cell, so on to the next one --
                 * or to the split, where the other tilemap's cell begins */
                int run = 8 - (tx & 7);
                if (mode && x < split_x && x + run > split_x) run = split_x - x;
                x += run - 1;
                continue;
            }
            uint8_t ci = nib[tx & 7];
            int pen = bank16 + ci;
            if (opaque) {
                drow[x] = pal_read16(bus, pen);
                if (arow) arow[x] = pen ? 255 : 0;
            } else if (ci != 0 && pc == (uint8_t)cat) {
                drow[x] = pal_read16(bus, pen);
                if (arow) arow[x] = 255;
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

/* Screen colours for every 15-bit palette colour, as RGBA8 with alpha 255:
 * tile_pen_lut's table laid out for the compose loop. Built from
 * video_pen_channels' 3 x 32 values (which is what tile_pen_lut's 32768 x 3
 * come to, see below) and only when those change: the colour tables rarely
 * do, and a compose rebuilt it with 98,304 divisions every frame. */
static uint8_t g_video_pen[0x8000][4];
static uint8_t g_video_pen_chan[3][32];
static bool    g_video_pen_ok;

/* tile_pen_lut one channel at a time. Each channel of a pen depends only on its
 * own 5 bits, so 3 x 32 values give every entry of the 0x8000-entry table:
 * lut[c][ch] == chan[ch][(c >> 5 * ch) & 31]. The GPU path needs 8192 pens per
 * palette change, not 32768 built with a division each, and the CPU
 * compositor's table (video_pen_table) is laid out from the same 96 values.
 * --verify-gpu-tiles holds this to tile_pen_lut. */
static inline void video_pen_channels(const memory_bus_t *bus, uint8_t chan[3][32]) {
    int loaded = 0;
    for (int c5 = 0; c5 < 32 && !loaded; c5++)
        for (int ch = 0; ch < 3 && !loaded; ch++)
            if (bus->colorxlat[((uint32_t)ch * 0x2000u + 0x40u + ((uint32_t)c5 << 8)) * 2u]) loaded = 1;
    for (int ch = 0; ch < 3; ch++)
        for (int c5 = 0; c5 < 32; c5++) {
            if (!loaded) { chan[ch][c5] = (uint8_t)(c5 << 3); continue; }
            int v = bus->colorxlat[((uint32_t)ch * 0x2000u + 0x40u + ((uint32_t)c5 << 8)) * 2u];
            int g = (v - 64) * 255 / 191;
            chan[ch][c5] = (uint8_t)(g < 0 ? 0 : g);
        }
}

static inline void video_pen_table(const memory_bus_t *bus) {
    uint8_t chan[3][32];
    video_pen_channels(bus, chan);
    if (g_video_pen_ok && memcmp(chan, g_video_pen_chan, sizeof chan) == 0) return;
    memcpy(g_video_pen_chan, chan, sizeof chan);
    g_video_pen_ok = true;
    for (int c = 0; c < 0x8000; c++) {
        g_video_pen[c][0] = chan[0][c & 0x1F];
        g_video_pen[c][1] = chan[1][(c >> 5) & 0x1F];
        g_video_pen[c][2] = chan[2][(c >> 10) & 0x1F];
        g_video_pen[c][3] = 255;
    }
}

#endif /* TILE_RENDERER_H */
