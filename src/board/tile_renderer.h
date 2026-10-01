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

/* Tile RAM words the compositor reads: the four tilemaps (words 0x0000-0x3FFF),
 * their per-line H scroll tables (0x4000 + 0x200*t), the scroll and control
 * registers (0x5000-0x5007) and both window masks (0x6000 / 0x6800, four words
 * a line). Both compositors work from a snapshot of these, taken when they
 * start, as little-endian words (the host's order). */
#define TILE_SNAP_WORDS 0x7000
_Static_assert(TILE_SNAP_WORDS >= 0x6800 + VIDEO_HEIGHT * 4, "the snapshot covers the window masks");
_Static_assert(TILE_SNAP_WORDS * 2 <= TILE_SIZE, "the snapshot fits tile RAM");
_Static_assert(0x4000 * 32 == TMAPGFX_SIZE, "every cell index is a whole cell of tile graphics");

/* A pen is palette bank * 16 + colour index, so < 4096; this one marks a pixel
 * of the front layer that no tilemap drew. */
#define TILE_PEN_NONE 0x1000

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
/* Draw tilemap t of the tile RAM words w into the pen layer dst, line y over
 * [x0[y], x1[y]): category `cat` only (non-transparent pixels), or every pixel
 * when `opaque`. The layers hold pens, not colours, so a palette write needs
 * no redraw (tile_compose_cpu).
 *
 * A pixel of tilemap l at (x - h, y + vy) & 511 is cell ((y + vy) >> 3, (x - h)
 * >> 3)'s pixel ((x - h) & 7, (y + vy) & 7). The line's window-mask words and
 * split are fixed per line; a cell's row of eight pixels is decoded once when
 * the pixel walk enters the cell, and a cell that cannot draw in this pass
 * (wrong category, or blank -- most of a HUD layer) is stepped over whole.
 * tests/tile_test.c holds it to the pixel-by-pixel original. */
static inline void s24_draw_tilemap(const uint16_t *w, const uint8_t *gfx, int t, int cat, bool opaque,
                                    uint16_t *dst, const int16_t *x0, const int16_t *x1) {
    uint16_t hscr = w[0x5000 + t];
    uint16_t vscr = w[0x5004 + t];
    uint16_t ctrl = w[0x5004 + (t & 2)];
    if (vscr & 0x8000) return;
    int mode = (ctrl & 0x6000) >> 13;
    if (mode && (t & 1)) return;                 /* split modes: the even tilemap draws both */
    const uint16_t *hscrtb = w + 0x4000 + 0x200 * t;
    const uint16_t *maskw  = w + ((t & 2) ? 0x6800 : 0x6000);
    int vy = vscr & 0x1FF;

    for (int y = 0; y < VIDEO_HEIGHT; y++) {
        int xs = x0[y], xe = x1[y];
        if (xs >= xe) continue;
        uint16_t row = (hscr & 0x8000) ? hscrtb[y] : hscr;
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
                uint16_t m = maskw[y * 4 + k];
                mask[k] = (t & 1) ? (uint16_t)~m : m;
            }
        }
        int      ty   = (y + vy) & 511;
        int      cell = (ty >> 3) * 64;          /* the tilemap row's first cell */
        uint16_t *drow = dst + y * VIDEO_WIDTH;
        int     cur = -1;                        /* l << 6 | column of the decoded cell */
        int     bank16 = 0;
        bool    dead = false;                    /* no pixel of the cell can draw in this pass */
        uint8_t pc = 0, nib[8] = { 0 };
        for (int x = xs; x < xe; x++) {
            int l = t;
            if (mode) l = (x < split_x) ? split_l : (split_l ^ 1);
            else if (mask[x >> 7] & (0x8000 >> ((x & 127) >> 3))) continue;
            int tx  = (x - h) & 511;
            int key = (l << 6) | (tx >> 3);
            if (key != cur) {
                cur = key;
                uint16_t entry = w[0x1000 * l + cell + (tx >> 3)];
                bank16 = ((entry >> 7) & 0xFF) * 16;
                pc     = (uint8_t)((entry >> 15) & 1);
                /* 16-bit byteswap: bytes 1, 0, 3, 2; high nibble first */
                const uint8_t *g = gfx + (uint32_t)(entry & 0x3FFF) * 32u + (uint32_t)(ty & 7) * 4u;
                nib[0] = g[1] >> 4; nib[1] = g[1] & 15; nib[2] = g[0] >> 4; nib[3] = g[0] & 15;
                nib[4] = g[3] >> 4; nib[5] = g[3] & 15; nib[6] = g[2] >> 4; nib[7] = g[2] & 15;
                dead = !opaque && (pc != (uint8_t)cat || !(g[0] | g[1] | g[2] | g[3]));
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
            if (opaque || ci != 0) drow[x] = (uint16_t)(bank16 + ci);
        }
    }
}

/* ---- Which screen blocks a tile RAM change reaches ------------------------- */

/* Both compositors redraw in 8x8 screen blocks: only the blocks a tile RAM
 * change can reach are drawn again. */
#define TILE_BLK_W (VIDEO_WIDTH / 8)
#define TILE_BLK_H (VIDEO_HEIGHT / 8)
_Static_assert(TILE_BLK_W * 8 == VIDEO_WIDTH && TILE_BLK_H * 8 == VIDEO_HEIGHT, "screen is whole 8x8 blocks");

typedef struct {
    uint8_t blk[TILE_BLK_H][TILE_BLK_W];     /* 1: the 8x8 block must be drawn again */
    int     count;
    bool    full;
} tile_dirty_t;

/* Mark the blocks covering screen pixels [x0, x1) x [y0, y1), clipped. */
static inline void tile_dirty_mark(tile_dirty_t *d, int x0, int x1, int y0, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > VIDEO_WIDTH)  x1 = VIDEO_WIDTH;
    if (y1 > VIDEO_HEIGHT) y1 = VIDEO_HEIGHT;
    if (x0 >= x1 || y0 >= y1) return;
    for (int by = y0 >> 3; by <= (y1 - 1) >> 3; by++)
        for (int bx = x0 >> 3; bx <= (x1 - 1) >> 3; bx++)
            if (!d->blk[by][bx]) { d->blk[by][bx] = 1; d->count++; }
}

/* The screen pixels where tilemap cell (cx, cy) of tilemap l can show, marked.
 * A pixel of drawing tilemap t samples tilemap l at ((x - h) & 511, (y + v) & 511)
 * with t's own scroll: t == l, or under a split mode the even tilemap of the pair
 * drawing the odd one. The coordinates wrap at 512, so a cell's 8 pixels can
 * also sit 512 lower; with a per-line H scroll the cell's lines are marked whole. */
static inline void tile_dirty_mark_cell(tile_dirty_t *d, const uint16_t *w, int l, int cx, int cy) {
    for (int k = 0; k < 2; k++) {
        int t = k ? (l & 2) : l;
        if (k && t == l) break;
        uint16_t hscr = w[0x5000 + t], vscr = w[0x5004 + t], ctrl = w[0x5004 + (t & 2)];
        int mode = (ctrl & 0x6000) >> 13;
        if (vscr & 0x8000) continue;             /* t disabled */
        if (mode && (t & 1)) continue;           /* odd tilemap idle under a split */
        if (k && !mode) continue;                /* no split: t draws only itself */
        int y0 = (cy * 8 - (vscr & 0x1FF)) & 511;
        for (int j = 0; j < 2; j++) {
            int ys = y0 - 512 * j;
            if (hscr & 0x8000) {
                tile_dirty_mark(d, 0, VIDEO_WIDTH, ys, ys + 8);
            } else {
                int x0 = (cx * 8 + (hscr & 0x1FF)) & 511;
                tile_dirty_mark(d, x0, x0 + 8, ys, ys + 8);
                tile_dirty_mark(d, x0 - 512, x0 - 504, ys, ys + 8);
            }
        }
    }
}

/* Which blocks the change from `old` to `cur` tile RAM words can alter on
 * screen. A scroll or control register change moves everything: full. A row
 * scroll word redraws its line, a window mask word its 128-pixel span of the
 * line, a tilemap cell where it shows. Words the compositor never reads change
 * nothing. */
static inline void tile_dirty_find(tile_dirty_t *d, const uint16_t *old, const uint16_t *cur) {
    for (int i = 0; i < TILE_SNAP_WORDS && !d->full; i += 64) {
        int n = TILE_SNAP_WORDS - i < 64 ? TILE_SNAP_WORDS - i : 64;
        if (!memcmp(old + i, cur + i, (size_t)n * sizeof *cur)) continue;
        for (int wi = i; wi < i + n; wi++) {
            if (old[wi] == cur[wi]) continue;
            if (wi < 0x4000) {
                tile_dirty_mark_cell(d, cur, wi >> 12, wi & 63, (wi >> 6) & 63);
            } else if (wi < 0x4800) {
                int y = (wi - 0x4000) & 0x1FF;
                tile_dirty_mark(d, 0, VIDEO_WIDTH, y, y + 1);
            } else if (wi >= 0x5000 && wi < 0x5008) {
                d->full = true;
                break;
            } else if (wi >= 0x6000 && wi < 0x7000) {
                int off = (wi - 0x6000) & 0x7FF;         /* 0x6000 and 0x6800 alike */
                if (off < VIDEO_HEIGHT * 4) {
                    int y = off / 4, x = (off % 4) * 128;
                    tile_dirty_mark(d, x, x + 128, y, y + 1);
                }
            }
        }
    }
}

/* ---- The CPU compositor --------------------------------------------------- */

/*
 * Both layers as pens, and the RGBA the GPU is given. A frame recomposes only
 * what changed, and the palette (which STF writes a few words of in most
 * frames) is applied last:
 *   - graphics changed, or a scroll or control register: every pixel is drawn
 *     again;
 *   - otherwise a tile RAM change redraws only the blocks it reaches
 *     (tile_dirty_find; the snapshot it is compared with is what the pens were
 *     drawn from, so a write landing mid-compose is caught next time);
 *   - a palette or colour-table change rebuilds the 4096 pen colours and
 *     recolours only the pixels whose pen's colour changed.
 * The result is the eight full-screen passes and colour conversion of before,
 * byte for byte (tests/tile_test.c holds both, frame after random frame).
 */
typedef struct {
    uint16_t bg[VIDEO_WIDTH * VIDEO_HEIGHT];   /* pen behind the 3D; the backdrop is pen 0 */
    uint16_t fg[VIDEO_WIDTH * VIDEO_HEIGHT];   /* pen in front of it, or TILE_PEN_NONE */
    uint16_t words[TILE_SNAP_WORDS];           /* the tile RAM the pens were drawn from */
    uint16_t next[TILE_SNAP_WORDS];
    uint8_t  pencol[TILE_PEN_NONE + 1][4];     /* RGBA of each pen; NONE is clear */
    bool     valid;
} tile_cpu_t;

/* Behind the 3D: the backdrop (pen 0), tilemaps 3 and 2 opaque, then tilemaps 1
 * and 0 where their tiles are category 0. In front: tilemaps 3, 2, 1, 0 where
 * category 1. Line y over [x0[y], x1[y]). */
static inline void tile_cpu_draw(tile_cpu_t *c, const uint8_t *gfx, const int16_t *x0, const int16_t *x1) {
    for (int y = 0; y < VIDEO_HEIGHT; y++)
        for (int x = x0[y]; x < x1[y]; x++) {
            c->bg[y * VIDEO_WIDTH + x] = 0;
            c->fg[y * VIDEO_WIDTH + x] = TILE_PEN_NONE;
        }
    s24_draw_tilemap(c->words, gfx, 3, 0, true,  c->bg, x0, x1);
    s24_draw_tilemap(c->words, gfx, 2, 0, true,  c->bg, x0, x1);
    s24_draw_tilemap(c->words, gfx, 1, 0, false, c->bg, x0, x1);
    s24_draw_tilemap(c->words, gfx, 0, 0, false, c->bg, x0, x1);
    for (int k = 3; k >= 0; k--) s24_draw_tilemap(c->words, gfx, k, 1, false, c->fg, x0, x1);
}

/* Bring bg_rgba / fg_rgba (RGBA, row 0 top) up to date with the bus. `full`
 * draws everything again; tile_changed / pens_changed say what may have changed
 * since the last call (gfx changing is `full`). chan is video_pen_channels of
 * the current colour tables. Returns whether any output byte may have changed. */
static inline bool tile_compose_cpu(tile_cpu_t *c, const memory_bus_t *bus, const uint8_t chan[3][32],
                                    bool full, bool tile_changed, bool pens_changed,
                                    uint8_t *bg_rgba, uint8_t *fg_rgba) {
    static tile_dirty_t d;
    static int16_t x0[VIDEO_HEIGHT], x1[VIDEO_HEIGHT];
    static uint8_t changed[TILE_PEN_NONE + 1];
    memset(&d, 0, sizeof d);
    d.full = full || !c->valid;
    if (d.full || tile_changed) {
        memcpy(c->next, bus->tile, sizeof c->next);   /* little-endian words, as on the host */
        if (!d.full) tile_dirty_find(&d, c->words, c->next);
        memcpy(c->words, c->next, sizeof c->words);
    }
    if (d.count > TILE_BLK_W * TILE_BLK_H * 3 / 4) d.full = true;

    /* Lines to draw again: each block row's dirty blocks, first to last. */
    bool drawn = d.full || d.count;
    for (int by = 0; by < TILE_BLK_H; by++) {
        int b0 = 0, b1 = 0;
        if (d.full) {
            b1 = TILE_BLK_W;
        } else {
            while (b0 < TILE_BLK_W && !d.blk[by][b0]) b0++;
            b1 = TILE_BLK_W;
            while (b1 > b0 && !d.blk[by][b1 - 1]) b1--;
        }
        for (int y = by * 8; y < by * 8 + 8; y++) { x0[y] = (int16_t)(b0 * 8); x1[y] = (int16_t)(b1 * 8); }
    }
    if (drawn) tile_cpu_draw(c, bus->tmapgfx, x0, x1);

    /* The pen colours, and which of them changed. */
    int nchanged = 0;
    if (pens_changed || !c->valid) {
        memset(changed, 0, sizeof changed);
        for (int p = 0; p <= TILE_PEN_NONE; p++) {
            uint16_t col = p < TILE_PEN_NONE ? pal_read16(bus, p) : 0;
            uint8_t rgba[4] = { chan[0][col & 31], chan[1][(col >> 5) & 31], chan[2][(col >> 10) & 31],
                                p < TILE_PEN_NONE ? 255 : 0 };
            if (!c->valid || memcmp(c->pencol[p], rgba, 4)) {
                memcpy(c->pencol[p], rgba, 4);
                changed[p] = 1;
                nchanged++;
            }
        }
    }
    c->valid = true;

    int n = VIDEO_WIDTH * VIDEO_HEIGHT;
    if (d.full) {
        for (int i = 0; i < n; i++) {
            memcpy(bg_rgba + i * 4, c->pencol[c->bg[i]], 4);
            memcpy(fg_rgba + i * 4, c->pencol[c->fg[i]], 4);
        }
        return true;
    }
    if (nchanged) {
        for (int i = 0; i < n; i++) {
            uint16_t b = c->bg[i], f = c->fg[i];
            if (changed[b]) memcpy(bg_rgba + i * 4, c->pencol[b], 4);
            if (changed[f]) memcpy(fg_rgba + i * 4, c->pencol[f], 4);
        }
    }
    if (drawn) {
        for (int y = 0; y < VIDEO_HEIGHT; y++)
            for (int i = y * VIDEO_WIDTH + x0[y]; i < y * VIDEO_WIDTH + x1[y]; i++) {
                memcpy(bg_rgba + i * 4, c->pencol[c->bg[i]], 4);
                memcpy(fg_rgba + i * 4, c->pencol[c->fg[i]], 4);
            }
    }
    return drawn || nchanged;
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

/* tile_pen_lut one channel at a time. Each channel of a pen depends only on its
 * own 5 bits, so 3 x 32 values give every entry of the 0x8000-entry table:
 * lut[c][ch] == chan[ch][(c >> 5 * ch) & 31]. The GPU path needs 8192 pens per
 * palette change, not 32768 built with a division each, and the CPU
 * compositor's pen colours (tile_compose_cpu) come from the same 96 values.
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

#endif /* TILE_RENDERER_H */
