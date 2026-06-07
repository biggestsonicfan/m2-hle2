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
                                      uint8_t *color_idx) {
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
                                            sx + scroll_x, sy + scroll_y, &color_idx);
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
 * Render a Sega System 24 tilemap layer PAIR with scroll/window support, after
 * MAME's segaic24 draw_common.  Two pairs share one register set each:
 *
 *   pair 0 ("A"): layers 0/1 @ TILE bytes 0x0000 / 0x2000   (foreground, alpha-keyed)
 *     hscr=word 0x5000 (0x100A000), vscr+ctrl=word 0x5004 (0x100A008), hscrtb=word 0x4000
 *   pair 1 ("B"): layers 2/3 @ TILE bytes 0x4000 / 0x6000   (background, opaque)
 *     hscr=word 0x5002 (0x100A004), vscr+ctrl=word 0x5006 (0x100A00C), hscrtb=word 0x4400
 *
 * Register bits (per pair): hscr bit15 = per-row H scroll via hscrtb, low9 = uniform H.
 *   vscr/ctrl (same word): bit15 = layer disable, bits[14:13] = window/split mode,
 *   low9 = uniform V scroll.
 *
 * Effects covered: FV adv_name_set fade and STF adv_movie_draw_text (pair 0, mode-2
 * per-row split, layer0/1), STF scrolling_bg_for_stf_logo (pair 1, vertical scroll),
 * STF sub_56B80 (pair 1, per-row H scroll).  The non-scroll path reduces to a plain
 * single-layer render.  (scrambles_textures' stride-4 hscrtb packing is not handled.)
 *
 * `opaque`: pair 1 (BG) writes every pixel (color idx 0 → palette[bank*16]); pair 0
 * (FG) writes alpha = (color idx != 0).
 */
static inline void render_sys24_pair(const memory_bus_t *bus, uint16_t *out,
                                     uint8_t *alpha, int pair, bool opaque) {
    const int MW = 64, MH = 64;
    uint32_t l0_off = (pair == 0) ? 0x0000u : 0x4000u;   /* even layer tilemap */
    uint32_t l1_off = l0_off + 0x2000u;                  /* odd layer tilemap  */
    uint32_t hscr_w = (pair == 0) ? 0x5000u : 0x5002u;
    uint32_t vc_w   = (pair == 0) ? 0x5004u : 0x5006u;   /* vscr + ctrl share a word */
    uint32_t hstb_w = (pair == 0) ? 0x4000u : 0x4400u;   /* per-row H scroll table   */

    uint16_t hscr = tileram_word(bus, hscr_w);
    uint16_t vc   = tileram_word(bus, vc_w);
    int vy = vc & 0x1ff;
    uint16_t backc = opaque ? back_color_555(bus) : 0;

    int n = VIDEO_WIDTH * VIDEO_HEIGHT;
    if (vc & 0x8000) {  /* layer pair disabled — backdrop (BG) / transparent (FG) */
        for (int i = 0; i < n; i++) { out[i] = backc; if (alpha) alpha[i] = 0; }
        return;
    }

    bool window    = (vc & 0x6000) != 0;
    bool rowscroll = (hscr & 0x8000) != 0;

    for (int sy = 0; sy < VIDEO_HEIGHT; sy++) {
        int wy = sy + vy;
        uint16_t rh = rowscroll ? tileram_word(bus, hstb_w + (uint32_t)sy) : hscr;
        int h = rh & 0x1ff;
        /* Per-row window split (MAME case 2/3): left of x=h uses l1, right uses
         * l1^1, where l1 = (rh & 0x200) ? even-layer : odd-layer. */
        int l1_is_even = (rh & 0x200) ? 1 : 0;

        for (int sx = 0; sx < VIDEO_WIDTH; sx++) {
            uint32_t tmap;
            if (window && rowscroll) {
                int left = (sx < h);
                int use_even = left ? l1_is_even : !l1_is_even;
                tmap = use_even ? l0_off : l1_off;
            } else {
                tmap = l0_off;   /* plain single-layer / uniform scroll */
            }
            uint8_t ci;
            uint16_t color = tile_sample_px(bus, tmap, MW, MH, sx + h, wy, &ci);
            int i = sy * VIDEO_WIDTH + sx;
            out[i] = color;
            if (alpha) alpha[i] = opaque ? 255 : (ci != 0 ? 255 : 0);
        }
    }
}

/* Background = sys24 pair B (layers 2/3), opaque.  Empty cells render palette[0]. */
static inline void render_bg_layer(const memory_bus_t *bus, tile_layers_t *t) {
    render_sys24_pair(bus, t->bg, t->bg_alpha, 1, true);
}

/* Foreground / HUD = sys24 pair A (layers 0/1), color-0 transparent. */
static inline void render_fg_layer(const memory_bus_t *bus, tile_layers_t *t) {
    render_sys24_pair(bus, t->fg, t->alpha, 0, false);
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
