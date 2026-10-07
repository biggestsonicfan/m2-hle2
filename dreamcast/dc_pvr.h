/*
 * dc_pvr.h -- the picture on the PowerVR (Pinboard #353, milestone D2).
 *
 * KOS's PVR API, not GLdc: the board's geometry decoder (geo3d.h) already
 * hands over eye-space triangles and the projection is two multiplies, so a
 * GL that transforms every vertex again in software would only cost time.
 *
 * One scene a board frame, when the PVR is ready for it (a frame it is not is
 * dropped, never waited for):
 *
 *   opaque list       the back tile layer, then every opaque 3D face, far to
 *                     near in the board's own order, depth compare ALWAYS
 *   translucent list  (presorted) faces with see-through texels and checker
 *                     faces, depth-tested against what the opaque list wrote;
 *                     then the front tile layer and the stats text
 *
 * The board has no depth buffer: it gives a polygon one sort key and fills
 * near buckets first, a pixel only where nothing has (geo3d.h, "z-sort").
 * Drawing far to near with ALWAYS is that rule run backwards, ties included.
 * The vertex z is still the true 1/w, which the PVR needs for perspective-
 * correct texturing. The punch-through list cannot do this: the PVR forces
 * its compare to GEQUAL (Flycast does too), so faces with holes go to the
 * translucent list instead, where the compare is the header's.
 *
 * Textures: a face names a tile of a texture sheet, 4 bits a texel. Each tile
 * becomes its own twiddled 4bpp paletted PVR texture, cut out of texture RAM
 * the first time a face uses it and dropped when the game writes over it
 * (bus->tex_dirty). The palette is a grey ramp; the face's colour ramp (luma
 * RAM, poly luma, colorxlat: the desktop's shade rows) is fitted as base *
 * texel + offset, the PVR's modulate with offset colour. Bank 1 is the same
 * ramp with texel 15 clear, for faces with holes; bank 2 the checker.
 */
#ifndef DC_PVR_H
#define DC_PVR_H

#include <dc/pvr.h>
#include <dc/biosfont.h>

#include "geo3d.h"
#include "dc_strips.h"

#include "tile_renderer.h"

/* The frame is the cable's 640x480; DC_FRAME512 (make FRAME512=1) makes it
 * 512x384, set in the middle of the 640x480 signal (dc_video_mode). That is
 * the same picture on a Dreamcast, but Flycast scales a frame to fill the
 * screen whatever its size, unless it has tools/flycast-frame512.patch.
 *
 * What of the board's 496x384 shows is the view, DC_VIEW_X/Y/W/H (make
 * VIEW=x,y,w,h; default the whole screen): scaled to fill the frame, aspect
 * kept, centred, black around it. The whole board is 1.25 (620x480) in the
 * 640x480 frame and 1.0 in the 512x384 one (8 pixels in: the PVR renders
 * whole 32-pixel tiles). m2-sonic's Mega Drive picture, 320x224 at (88,80),
 * is 2.0: 640x448. Only the view is drawn; at a whole-number scale the tile
 * layers are point sampled. DC_HUD_NONE (make HUD=none) draws the view 1:1,
 * so with no VIEW the board sits at (72,48) of the 640x480 frame, a picture to
 * crop and hold against MAME's pixel for pixel (#478).
 *
 * DC_FILL (make FILL=1, #503) stretches the view over the whole frame instead,
 * DC_SX across and DC_SY down: the board 1.29 x 1.25 to 640x480. That is the
 * shape an arcade monitor gives it (496x384 on a 4:3 tube), and MAME's. */
#ifndef DC_FRAME512
#define DC_FRAME512 0
#endif
#if DC_FRAME512
#define DC_SCR_W 512
#define DC_SCR_H 384
#else
#define DC_SCR_W 640
#define DC_SCR_H 480
#endif
#ifndef DC_VIEW_X
#define DC_VIEW_X 0
#define DC_VIEW_Y 0
#define DC_VIEW_W 496
#define DC_VIEW_H 384
#endif
#if defined(DC_HUD_NONE) && DC_HUD_NONE
#define DC_S   1.0f
#else
#define DC_S   ((float)DC_SCR_W * DC_VIEW_H < (float)DC_SCR_H * DC_VIEW_W ? \
                (float)DC_SCR_W / DC_VIEW_W : (float)DC_SCR_H / DC_VIEW_H)
#endif
#ifndef DC_FILL
#define DC_FILL 0
#endif
#if DC_FILL && !(defined(DC_HUD_NONE) && DC_HUD_NONE)
#define DC_SX  ((float)DC_SCR_W / DC_VIEW_W)
#define DC_SY  ((float)DC_SCR_H / DC_VIEW_H)
#else
#define DC_SX  DC_S
#define DC_SY  DC_S
#endif
#define DC_VX0 ((float)(int)(((float)DC_SCR_W - DC_VIEW_W * DC_SX) * 0.5f))  /* the view on screen */
#define DC_VY0 ((float)(int)(((float)DC_SCR_H - DC_VIEW_H * DC_SY) * 0.5f))
#define DC_VX1 (DC_VX0 + DC_VIEW_W * DC_SX)
#define DC_VY1 (DC_VY0 + DC_VIEW_H * DC_SY)
#define DC_X0  (DC_VX0 - DC_VIEW_X * DC_SX)                                 /* board pixel (0,0) */
#define DC_Y0  (DC_VY0 - DC_VIEW_Y * DC_SY)
#define DC_FILTER (DC_SX == (float)(int)DC_SX && DC_SY == (float)(int)DC_SY ? PVR_FILTER_NONE : PVR_FILTER_BILINEAR)
#define DC_NEAR GEO3D_NEAR

/* ---- Textures cut from texture RAM ----------------------------------------- */

#define DC_TEX_SLOTS 1024u   /* power of two */
typedef struct {
    uint32_t  key;           /* 0: free */
    pvr_ptr_t ptr;
    uint16_t  w, h;          /* the PVR texture's size (>= 8 a side) */
    float     su, sv;        /* 1 / w, 1 / h */
    uint16_t  q0, q1;        /* the texture-RAM KB rows it was cut from */
    uint8_t   sheet;
    uint8_t   hdr_var;       /* what hdr was compiled for (dp_face's variant), 0: nothing */
    pvr_poly_hdr_t hdr;      /* the last header a face with this texture needed */
} dc_tex_t;

static struct {
    dc_tex_t  tex[DC_TEX_SLOTS];
    unsigned  count, made, dropped, fails;
    uint32_t  gen_tex;
    pvr_ptr_t bg, fg, text;          /* the two tile layers (pair 0, tilemap 0's line-scroll textures too), the stats text */
    pvr_ptr_t lbg[2], lfg[2];        /* the tile layers, double-buffered: pair 0 is bg/fg (dp_tiles_send) */
    uint8_t   lcur;                  /* the pair the last frame drew */
    pvr_ptr_t checker;
    uint16_t  pen565[TILE_PEN_NONE + 1], pen1555[TILE_PEN_NONE + 1];
    uint32_t  text_rows;             /* bit r: screen row r has text */
    uint32_t  text_hide;             /* bit r: row r is not drawn (HUD=prof's R trigger) */
    uint8_t   text_w[32];            /* HUD=prof: row r's text, in cells */
    unsigned  corner_w;              /* the corner counter's width in texels (0: none) */
    unsigned  tris, faces_dropped, runs;
    uint32_t  us_decode, us_submit;
    uint32_t  us_tiles, us_scan, us_sort;   /* parts of us_decode */
    uint64_t  tt_tiles, tt_scan, tt_sort, tt_submit;   /* the same, since boot (DC_HASH_FRAME) */
    uint64_t  tt_tex;                /* us making textures (dp_tex_get's cut or pack read and load), since boot */
    uint32_t  tx_hits, tx_miss;      /* textures the pack had, and packable ones it had not */
} g_dp;

/* Tilemaps 2 and 0 as strips (dp_ls_strips, below). */
typedef struct {
    pvr_ptr_t back, front;
    uint16_t  cells[0x1000];      /* the entries the textures hold */
    uint8_t   front_set[0x1000];  /* the cell is category 1 with a pixel set */
    unsigned  front_cells;
    uint16_t  h[VIDEO_HEIGHT];    /* each line's H scroll, and the V scroll */
    uint16_t  vy;
} dp_ls_t;
static struct {
    bool    on, both;             /* tilemap 2 on the PVR; tilemap 0 too, and no tile on the CPU */
    dp_ls_t t2, t0;               /* t0's textures are the CPU layers' (unused while both) */
} g_ls;

static uint8_t  g_dp_cut[256 * 256 / 2];

static inline uint32_t dp_log2(uint32_t v) { uint32_t l = 0; while ((1u << l) < v) l++; return l; }

/* Video memory a dropped texture held is given back a frame later: the PVR
 * may still be drawing the previous frame from it while this one is decoded,
 * and a texture loaded over it would show there. */
#define DC_TEX_LATE (DC_TEX_SLOTS * 2u)
static pvr_ptr_t g_dp_late[DC_TEX_LATE];
static unsigned  g_dp_late_n;
static bool      g_dp_full;    /* the table or video memory ran out: drop all at the next frame */

static void dp_tex_free(pvr_ptr_t p) {
    if (g_dp_late_n < DC_TEX_LATE) g_dp_late[g_dp_late_n++] = p;
    else pvr_mem_free(p);   /* cannot happen: drops come before the decode, each slot at most twice */
}
/* At a frame's start, the PVR ready for it: the frame drawn from what the last
 * one dropped is done, so give the memory back. */
static void dp_tex_free_late(void) {
    for (unsigned i = 0; i < g_dp_late_n; i++) pvr_mem_free(g_dp_late[i]);
    g_dp_late_n = 0;
}

static void dp_tex_drop_all(void) {
    for (unsigned i = 0; i < DC_TEX_SLOTS; i++)
        if (g_dp.tex[i].key) { dp_tex_free(g_dp.tex[i].ptr); g_dp.tex[i].key = 0; }
    g_dp.count = 0;
}

static inline uint32_t dp_tex_hash(uint32_t key) { return (key * 2654435761u) >> 22; }

/* Open addressing has no holes: after a delete, put every live entry back
 * where a lookup will find it (a probe stops at the first free slot). */
static void dp_tex_rehash(void) {
    static dc_tex_t old[DC_TEX_SLOTS];
    memcpy(old, g_dp.tex, sizeof old);
    for (unsigned i = 0; i < DC_TEX_SLOTS; i++) g_dp.tex[i].key = 0;
    for (unsigned i = 0; i < DC_TEX_SLOTS; i++) {
        if (!old[i].key) continue;
        uint32_t h = dp_tex_hash(old[i].key);
        unsigned p = 0;
        while (g_dp.tex[(h + p) & (DC_TEX_SLOTS - 1u)].key) p++;
        g_dp.tex[(h + p) & (DC_TEX_SLOTS - 1u)] = old[i];
    }
}

/* Texture RAM changed: drop every texture cut from a KB row the game wrote.
 * Called before a frame's decode, so no face holds a slot yet. */
static void dp_tex_invalidate(memory_bus_t *bus) {
    dp_tex_free_late();
    if (g_dp_full || g_dp.count >= DC_TEX_SLOTS * 3 / 4) { dp_tex_drop_all(); g_dp_full = false; }
    if (bus->gen_tex == g_dp.gen_tex) return;
    g_dp.gen_tex = bus->gen_tex;
    static uint8_t dirty[2][1024];
    for (int s = 0; s < 2; s++)
        for (int q = 0; q < 1024; q++) {
            dirty[s][q] = bus->tex_dirty[s][q];
            bus->tex_dirty[s][q] = 0;
        }
    unsigned gone = 0;
    for (unsigned i = 0; i < DC_TEX_SLOTS; i++) {
        dc_tex_t *t = &g_dp.tex[i];
        if (!t->key) continue;
        for (unsigned q = t->q0; q <= t->q1; q++)
            if (dirty[t->sheet][q]) {
                dp_tex_free(t->ptr);
                t->key = 0;
                g_dp.count--;
                g_dp.dropped++;
                gone++;
                break;
            }
    }
    if (gone) dp_tex_rehash();
}

/* The PVR texture for a face's tile, cut and loaded on first use; NULL if it
 * cannot be (too big, no video memory). */
static dc_tex_t *dp_tex_get(memory_bus_t *bus, float ftx, float fty, float ftw, float fth, unsigned fl) {
    unsigned sheet = (fl & GEO3D_FACE_SHEET1) ? 1 : 0;
    unsigned x0 = (unsigned)(int)ftx & 2047u, y0 = (unsigned)(int)fty & 1023u;
    unsigned tw = (unsigned)ftw, th = (unsigned)fth;
    if (tw > 1024 || th > 1024 || !tw || !th) return NULL;
    unsigned lw = dp_log2(tw), lh = dp_log2(th);
    uint32_t key = 0x80000000u | sheet << 29 | x0 << 18 | y0 << 8 | lw << 4 | lh;
    uint32_t h = dp_tex_hash(key);
    dc_tex_t *t = NULL;
    for (unsigned p = 0; p < DC_TEX_SLOTS; p++) {
        dc_tex_t *e = &g_dp.tex[(h + p) & (DC_TEX_SLOTS - 1u)];
        if (e->key == key) return e;
        if (!e->key) { t = e; break; }
    }
    /* Faces decoded earlier this frame hold their slots, so nothing is dropped
     * now: this face goes untextured and the next frame starts afresh. */
    if (!t || g_dp.count >= DC_TEX_SLOTS - 1u) { g_dp_full = true; g_dp.fails++; return NULL; }

    /* Cut from texture RAM (dct_cut), or from the texture pack when it holds
     * this tile as the words are now (dc_texpak.h). A texture over 256x256
     * (the water, a monitor's picture) is cut and loaded 32 KB at a time. */
    unsigned W = tw < 8 ? 8 : tw, H = th < 8 ? 8 : th;
    const uint32_t bytes = W * H / 2, chunk = bytes < sizeof g_dp_cut ? bytes : (uint32_t)sizeof g_dp_cut;
    pvr_ptr_t p = pvr_mem_malloc(bytes);
    if (!p) { g_dp_full = true; g_dp.fails++; return NULL; }
    const uint32_t *src = (const uint32_t *)(sheet ? bus->texram1 : bus->texram0);
    const uint64_t t0 = timer_us_gettime64();
    const uint8_t *packed = NULL;
    if (g_pg.tx && dct_packable(key)) {
        const dct_index_t *e = pg_tx_find(key, dct_src_hash(src, key));
        if (e) packed = pg_tx_at(e->off, bytes);
        if (packed) g_dp.tx_hits++; else g_dp.tx_miss++;
    }
    if (packed) pvr_txr_load((void *)packed, p, bytes);
    else
        for (uint32_t at = 0; at < bytes; at += chunk) {
            dct_cut(src, key, at, chunk, g_dp_cut);
            pvr_txr_load(g_dp_cut, (uint8_t *)p + at, chunk);
        }
    g_dp.tt_tex += timer_us_gettime64() - t0;
    *t = (dc_tex_t){ key, p, (uint16_t)W, (uint16_t)H, 1.0f / (float)W, 1.0f / (float)H,
                     (uint16_t)((y0 >> 1) + (x0 >= 1024 ? 512 : 0)),
                     (uint16_t)(((y0 + th - 1) >> 1) + (x0 >= 1024 ? 512 : 0)), (uint8_t)sheet };
    g_dp.count++;
    g_dp.made++;
    return t;
}

/* ---- Colours ------------------------------------------------------------------- */

/* The screen colour of face colour c5 at luma index li: colorxlat, then
 * max(c - 64, 0) * 255 / 191 (game_render__shade_px). */
static uint8_t g_dp_cx[256];   /* a colorxlat byte as a level: 0 to 64 black, then a line to 255 */
static inline void dp_shade(const memory_bus_t *bus, const int c5[3], int li, int out[3]) {
    if (!g_dp_cx[255])
        for (int v = 0; v < 256; v++) g_dp_cx[v] = (uint8_t)(v > 64 ? (v - 64) * 255 / 191 : 0);
    for (int ch = 0; ch < 3; ch++) out[ch] = g_dp_cx[bus->colorxlat[ch * 0x4000 + ((c5[ch] << 8) + li) * 2]];
}

static inline uint32_t dp_argb(const int c[3]) {
    return 0xFF000000u | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | (uint32_t)c[2];
}

static inline int dp_clamp255(float v) { return v <= 0.0f ? 0 : v >= 1.0f ? 255 : (int)(v * 255.0f + 0.5f); }

/* Faces whose ramp is no line at all get a palette bank of their own: m2-sdk's
 * m2_sprite.h palette mode puts a sprite's 16 pens in a colorxlat row, one
 * per texel, which base * texel + offset cannot show. Homebrew only (STF's
 * ramps are near enough lines, and its discs stay as they were). The banks
 * alternate between two halves frame by frame, so a frame the PVR is still
 * drawing keeps its palettes. */
#define DP_PAL_FIRST 3u
#define DP_PAL_HALF  30u
static bool     g_dp_pal_on;
static unsigned g_dp_pal_half, g_dp_pal_n;
static uint32_t g_dp_pal_key[DP_PAL_HALF];
static uint16_t g_dp_pal_col[DP_PAL_HALF][16];

static inline uint16_t dp_1555(const int c[3]) {
    return (uint16_t)(0x8000u | (uint32_t)(c[0] >> 3) << 10 | (uint32_t)(c[1] >> 3) << 5 | (uint32_t)(c[2] >> 3));
}

/* The bank holding these 16 colours (texel 15 a hole when trans), 0 if none is left. */
static unsigned dp_pal_bank(const int col[16][3], bool trans) {
    uint16_t c[16];
    uint32_t key = trans ? 0x9E3779B9u : 0x7F4A7C15u;
    for (int t = 0; t < 16; t++) {
        c[t] = trans && t == 15 ? 0 : dp_1555(col[t]);
        key = (key ^ c[t]) * 16777619u;
    }
    for (unsigned i = 0; i < g_dp_pal_n; i++)
        if (g_dp_pal_key[i] == key && !memcmp(g_dp_pal_col[i], c, sizeof c))
            return DP_PAL_FIRST + g_dp_pal_half * DP_PAL_HALF + i;
    if (g_dp_pal_n >= DP_PAL_HALF) return 0;
    unsigned i = g_dp_pal_n++, bank = DP_PAL_FIRST + g_dp_pal_half * DP_PAL_HALF + i;
    g_dp_pal_key[i] = key;
    memcpy(g_dp_pal_col[i], c, sizeof c);
    for (int t = 0; t < 16; t++) pvr_set_pal_entry(bank * 16u + (unsigned)t, c[t]);
    return bank;
}

/* A face's ramp is seldom a line: colorxlat's ramps start at about 88 and
 * the shade takes off 64 first (dp_shade), so the dark texels of a dim face
 * are all black, and the ramp rises from some texel on. A line from texel 0
 * to texel 15 lifts every texel below that knee; over an attract picture the
 * dark half came out ~20 levels too bright against MAME's (#486). So the
 * opaque and see-through grey ramps come again with a knee at every half
 * texel, 0 up to texel k and a line from there to 15, and a face takes the
 * bank of its own knee, found where the brightest channel's ramp, followed
 * back from texel 15, meets texel 0's colour. The same base and offset. */
typedef struct { float r, g, b, lb, pl; } dp_col_in_t;
#define DP_KNEE_FIRST 3u
#define DP_KNEES      26u   /* k = 0.5 .. 13 */
static void dp_knee_init(void) {
    for (unsigned n = 1; n <= DP_KNEES; n++) {
        float k = 0.5f * (float)n;
        for (int t = 0; t < 16; t++) {
            float g = (float)t > k ? ((float)t - k) / (15.0f - k) : 0.0f;
            uint32_t v = (uint32_t)(g * 31.0f + 0.5f);
            uint16_t c = (uint16_t)(0x8000u | v << 10 | v << 5 | v);
            pvr_set_pal_entry((DP_KNEE_FIRST + (n - 1) * 2) * 16u + (unsigned)t, c);
            pvr_set_pal_entry((DP_KNEE_FIRST + (n - 1) * 2 + 1) * 16u + (unsigned)t, t == 15 ? 0 : c);
        }
    }
}

static unsigned dp_knee_bank(const int col[16][3], bool trans) {
    int k = 0;   /* the channel that rises the most */
    for (int c = 1; c < 3; c++)
        if (col[15][c] - col[0][c] > col[15][k] - col[0][k]) k = c;
    const int lo = col[0][k], hi = col[15][k];
    if (hi - lo < 16) return 0;
    /* The last texel still at texel 0's colour, then where the line from
     * texel 15 through the next one comes down to it. */
    int f = 0;
    while (f < 14 && col[f + 1][k] <= lo + 2) f++;
    if (f == 0) return 0;
    if (f >= 14) return DP_KNEE_FIRST + (DP_KNEES - 1) * 2 + (trans ? 1u : 0u);
    float slope = (float)(hi - col[f + 1][k]) / (float)(14 - f);
    float knee = slope > 0.0f ? (float)(f + 1) - (float)(col[f + 1][k] - lo) / slope : (float)f;
    int n = (int)(knee * 2.0f + 0.5f);
    n = n < 1 ? 1 : n > (int)DP_KNEES ? (int)DP_KNEES : n;
    return DP_KNEE_FIRST + (unsigned)(n - 1) * 2 + (trans ? 1u : 0u);
}

/* STF's few ramps that are palettes (#486): the hut's emblem, the moon on
 * the lab monitor, the panel beside it. Their pens fall and rise again
 * (orange, black, white), and no line or knee can show that: the emblem
 * came out one flat grey. Such a face gets a bank of its own from the banks
 * past the knees, kept from frame to frame by its colours, and a bank used
 * this frame or the last (which the PVR may still be drawing) is never
 * written over. Out of banks, the face keeps its knee. */
#define DP_POOL_FIRST (DP_KNEE_FIRST + DP_KNEES * 2u)
#define DP_POOL_N     (64u - DP_POOL_FIRST)
#ifndef DP_POOL_DROP
#define DP_POOL_DROP  16   /* the fall between two texels that makes a ramp a palette */
#endif
static uint32_t g_dp_frame;
static struct { uint32_t key, used; uint16_t c[16]; } g_dp_pool[DP_POOL_N];

static unsigned dp_pool_bank(const int col[16][3], bool trans) {
    uint16_t c[16];
    uint32_t key = trans ? 0x9E3779B9u : 0x7F4A7C15u;
    for (int t = 0; t < 16; t++) {
        c[t] = trans && t == 15 ? 0 : dp_1555(col[t]);
        key = (key ^ c[t]) * 16777619u;
    }
    unsigned free_i = DP_POOL_N;
    for (unsigned i = 0; i < DP_POOL_N; i++) {
        if (g_dp_pool[i].used && g_dp_pool[i].key == key && !memcmp(g_dp_pool[i].c, c, sizeof c)) {
            g_dp_pool[i].used = g_dp_frame;
            return DP_POOL_FIRST + i;
        }
        if (free_i == DP_POOL_N && (!g_dp_pool[i].used || g_dp_pool[i].used + 1u < g_dp_frame)) free_i = i;
    }
    if (free_i == DP_POOL_N) return 0;
    g_dp_pool[free_i].key = key;
    g_dp_pool[free_i].used = g_dp_frame;
    memcpy(g_dp_pool[free_i].c, c, sizeof c);
    for (int t = 0; t < 16; t++) pvr_set_pal_entry((DP_POOL_FIRST + free_i) * 16u + (unsigned)t, c[t]);
    return DP_POOL_FIRST + free_i;
}

/* A textured face's ramp, texel 0 to 15, as a knee bank or a pool bank, with
 * the base and offset of the line from texel 0 to 15 (#486). Faces share
 * their colour, luma ramp and light within a frame and from frame to frame,
 * so the answer is kept by those inputs until luma or colorxlat changes
 * (gen_lut). A pool bank is taken again only while it still holds the same
 * pens, and is stamped as used this frame. Walking the ramp for every face
 * cost the bench (frames 3500-3900) 5 s of the decode. */
#define DP_RAMP_CACHE 1024u
static struct { uint32_t lut, k0, k1, base, off, pkey; uint8_t pal; } g_dp_ramp[DP_RAMP_CACHE];

static void dp_face_ramp(const memory_bus_t *bus, const dp_col_in_t *T, const int c5[3], int poly, bool trans,
                         uint32_t *base, uint32_t *offset, uint8_t *pal) {
    const uint32_t k0 = (uint32_t)c5[0] | (uint32_t)c5[1] << 5 | (uint32_t)c5[2] << 10 | (uint32_t)poly << 15 |
                        (trans ? 1u << 23 : 0u) | 1u << 24, k1 = (uint32_t)T->lb;
    const uint32_t h = ((k0 * 2654435761u) ^ (k1 * 40503u)) >> 22 & (DP_RAMP_CACHE - 1u);
    const uint32_t lut = bus->gen_lut | 1u;
    if (g_dp_ramp[h].lut == lut && g_dp_ramp[h].k0 == k0 && g_dp_ramp[h].k1 == k1) {
        const uint8_t pb = g_dp_ramp[h].pal;
        if (pb < DP_POOL_FIRST || (g_dp_pool[pb - DP_POOL_FIRST].key == g_dp_ramp[h].pkey &&
                                   g_dp_pool[pb - DP_POOL_FIRST].used + 1u >= g_dp_frame)) {
            if (pb >= DP_POOL_FIRST) g_dp_pool[pb - DP_POOL_FIRST].used = g_dp_frame;
            *base = g_dp_ramp[h].base; *offset = g_dp_ramp[h].off; *pal = pb;
            return;
        }
    }
    int col[16][3], drop = 0;
    for (int t = 0; t < 16; t++) {
        uint32_t lbyte = 2u * ((uint32_t)T->lb + (uint32_t)t * 8u);
        int lram = lbyte < LUMA_SIZE ? bus->luma[lbyte] : 0;
        int li = (lram * poly) >> 8;
        dp_shade(bus, c5, li > 63 ? 63 : li, col[t]);
        for (int k = 0; t && k < 3; k++)
            drop = col[t - 1][k] - col[t][k] > drop ? col[t - 1][k] - col[t][k] : drop;
    }
    if (drop > DP_POOL_DROP && (*pal = (uint8_t)dp_pool_bank(col, trans))) {
        *base = 0xFFFFFFFFu;
        *offset = 0;
    } else {
        int b[3];
        for (int k = 0; k < 3; k++) b[k] = col[15][k] > col[0][k] ? col[15][k] - col[0][k] : 0;
        *base = dp_argb(b);
        *offset = dp_argb(col[0]);
        *pal = (uint8_t)dp_knee_bank(col, trans);
    }
    g_dp_ramp[h].lut = lut; g_dp_ramp[h].k0 = k0; g_dp_ramp[h].k1 = k1;
    g_dp_ramp[h].pkey = *pal >= DP_POOL_FIRST ? g_dp_pool[*pal - DP_POOL_FIRST].key : 0;
    g_dp_ramp[h].base = *base; g_dp_ramp[h].off = *offset; g_dp_ramp[h].pal = *pal;
}

/* A face's colour (untextured), or its base and offset (textured): the fill
 * shader's chain at texel 0 and texel 15, joined by a line; or, when that
 * line misses (homebrew), every texel's colour in a palette bank (*pal). */
static void dp_face_colour(const memory_bus_t *bus, const dp_col_in_t *T, bool tex, bool trans,
                           uint32_t *base, uint32_t *offset, uint8_t *pal) {
    *pal = 0;
    int c5[3] = { (int)(T->r * 31.0f + 0.5f), (int)(T->g * 31.0f + 0.5f), (int)(T->b * 31.0f + 0.5f) };
    for (int k = 0; k < 3; k++) c5[k] = c5[k] < 0 ? 0 : c5[k] > 31 ? 31 : c5[k];
    float pl = T->pl < 0.0f ? 0.0f : T->pl > 1.0f ? 1.0f : T->pl;
    int poly = (int)(pl * 255.0f + 0.5f);
    *offset = 0;
    if (!tex) {
        if (T->lb < 0.0f) {
            int c[3] = { dp_clamp255(T->r * pl), dp_clamp255(T->g * pl), dp_clamp255(T->b * pl) };
            *base = dp_argb(c);
        } else {
            int c[3];
            dp_shade(bus, c5, poly >> 2 > 63 ? 63 : poly >> 2, c);
            *base = dp_argb(c);
        }
        return;
    }
    if (T->lb < 0.0f) {   /* colour * texel * 2 */
        int c[3] = { dp_clamp255(T->r * 2.0f), dp_clamp255(T->g * 2.0f), dp_clamp255(T->b * 2.0f) };
        *base = dp_argb(c);
        return;
    }
    if (!g_dp_pal_on) { dp_face_ramp(bus, T, c5, poly, trans, base, offset, pal); return; }
    int ends[2][3];
    for (int e = 0; e < 2; e++) {
        uint32_t lbyte = 2u * ((uint32_t)T->lb + (e ? 120u : 0u));
        int lram = lbyte < LUMA_SIZE ? bus->luma[lbyte] : 0;
        int li = (lram * poly) >> 8;
        dp_shade(bus, c5, li > 63 ? 63 : li, ends[e]);
    }
    int b[3];
    for (int k = 0; k < 3; k++) b[k] = ends[1][k] > ends[0][k] ? ends[1][k] - ends[0][k] : 0;
    *base = dp_argb(b);
    *offset = dp_argb(ends[0]);
    int col[16][3], worst = 0;
    for (int t = 0; t < 16; t++) {
        uint32_t lbyte = 2u * ((uint32_t)T->lb + (uint32_t)t * 8u);
        int lram = lbyte < LUMA_SIZE ? bus->luma[lbyte] : 0;
        int li = (lram * poly) >> 8;
        dp_shade(bus, c5, li > 63 ? 63 : li, col[t]);
        for (int k = 0; k < 3; k++) {
            int e = col[t][k] - (ends[0][k] + b[k] * t / 15);
            e = e < 0 ? -e : e;
            worst = e > worst ? e : worst;
        }
    }
    if (worst > 24 && (*pal = (uint8_t)dp_pal_bank(col, trans))) {
        *base = 0xFFFFFFFFu;
        *offset = 0;
    }
}

/* ---- Init ------------------------------------------------------------------------ */

/* The second pair of layer textures holds the board's rows and a few more,
 * which the filter may touch below the last: declared 512x512 to the PVR,
 * it is never drawn past the view. */
#define DP_LAYER_ROWS (VIDEO_HEIGHT + 8)
_Static_assert(DC_VIEW_Y + DC_VIEW_H <= VIDEO_HEIGHT, "the layers' second pair holds the board's rows");

static int dp_init(void) {
    pvr_init_params_t params = {
        { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_0 },
        768 * 1024, 0, 0, 1 /* presorted translucent list */, 2, 0,
    };
    if (pvr_init(&params) != 0) return -1;
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    pvr_set_pal_format(PVR_PAL_ARGB1555);
    for (int t = 0; t < 16; t++) {
        uint32_t g = (uint32_t)(t * 31 + 7) / 15;
        uint32_t c = 0x8000u | g << 10 | g << 5 | g;
        pvr_set_pal_entry(t, c);
        pvr_set_pal_entry(16 + t, t == 15 ? 0 : c);
        pvr_set_pal_entry(32 + t, t == 0 ? 0xFFFFu : 0);
    }
    dp_knee_init();
    dct_init();
    g_dp.bg   = pvr_mem_malloc(512 * 512 * 2);
    g_dp.fg   = pvr_mem_malloc(512 * 512 * 2);
    g_dp.lbg[0] = g_dp.bg; g_dp.lfg[0] = g_dp.fg;
    g_dp.lbg[1] = pvr_mem_malloc(512 * DP_LAYER_ROWS * 2);   /* the view's rows only: it never wraps */
    g_dp.lfg[1] = pvr_mem_malloc(512 * DP_LAYER_ROWS * 2);
    g_dp.text = pvr_mem_malloc(1024 * 512 * 2);
    g_ls.t2.back  = pvr_mem_malloc(512 * 512 * 2);
    g_ls.t2.front = pvr_mem_malloc(512 * 512 * 2);
    g_ls.t0.back  = g_dp.bg;
    g_ls.t0.front = g_dp.fg;
    /* The checker: texel (x ^ y) & 1 clear, 8x8 twiddled. */
    memset(g_dp_cut, 0, 32);
    for (unsigned y = 0; y < 8; y++)
        for (unsigned x = 0; x < 8; x++) {
            unsigned i = g_dct_spread[y] | (unsigned)g_dct_spread[x] << 1;
            g_dp_cut[i >> 1] |= (uint8_t)((((x ^ y) & 1u) ? 1u : 0u) << ((i & 1) * 4));
        }
    g_dp.checker = pvr_mem_malloc(32);
    if (!g_dp.bg || !g_dp.fg || !g_dp.lbg[1] || !g_dp.lfg[1] || !g_dp.text || !g_dp.checker || !g_ls.t2.back || !g_ls.t2.front) return -1;
    pvr_txr_load(g_dp_cut, g_dp.checker, 32);
    memset((void *)g_dp.bg, 0, 512 * 512 * 2);
    memset((void *)g_dp.fg, 0, 512 * 512 * 2);
    memset((void *)g_dp.lbg[1], 0, 512 * DP_LAYER_ROWS * 2);
    memset((void *)g_dp.lfg[1], 0, 512 * DP_LAYER_ROWS * 2);
    memset((void *)g_dp.text, 0, 1024 * 512 * 2);
    g_dp.gen_tex = ~0u;
    return 0;
}

/* ---- Text --------------------------------------------------------------------------- */

#if DC_HUD_PROF
/* HUD=prof's panel (Pinboard #518): 30 rows of 78 characters from column 1
 * (an 8x16 cell each, the first and last of a row's black), white on black
 * at a texel a pixel, a row's band only as wide as its text, so that a capture of the
 * console's video can be read back by a program (tools/hud_read.py). The
 * font is tools/hud_font5x7.py's: each 5x7 glyph bold (two pixels a stroke,
 * already in dc_hudfont.h) and each of its rows twice, at (1, 1) in its cell. */
#include "dc_hudfont.h"
#define DC_TEXT_ROWS 30
#define DC_TEXT_H    16
#define DC_TEXT_COLS 78

static void dp_text_cell(uint16_t *d, char ch) {
    const uint8_t *g = g_hudfont[(unsigned char)ch >= 32 && (unsigned char)ch < 127 ? ch - 32 : '?' - 32];
    for (int y = 0; y < 7; y++) {
        uint32_t on = (uint32_t)g[y] << 1;   /* the cell's 8 pixels, bit 7 the leftmost */
        uint32_t *a = (uint32_t *)(d + (1 + 2 * y) * 1024), *b = (uint32_t *)(d + (2 + 2 * y) * 1024);
        for (int x = 0; x < 4; x++) {   /* two pixels a word, the left one low */
            uint32_t w = (on >> (7 - 2 * x) & 1 ? 0xFFFFu : 0) | (on >> (6 - 2 * x) & 1 ? 0xFFFF0000u : 0);
            a[x] = b[x] = w;
        }
    }
}

/* Only the cells whose character changed are drawn again: video memory is
 * slow to write, and the LV row changes every frame. A cell's top and bottom
 * lines are always black (dp_init cleared the texture). */
static void dp_text_row(int row, const char *s) {
    static char last[DC_TEXT_ROWS][DC_TEXT_COLS + 1];
    if (row < 0 || row >= DC_TEXT_ROWS) return;
    uint16_t *d = (uint16_t *)g_dp.text + row * DC_TEXT_H * 1024;
    char *was = last[row];
    size_t n = strlen(s), m = strlen(was);
    if (n > DC_TEXT_COLS) n = DC_TEXT_COLS;
    for (size_t c = 0; c < n || c < m; c++) {
        char ch = c < n ? s[c] : ' ', old = c < m ? was[c] : ' ';
        if (ch != old) dp_text_cell(d + (c + 1) * 8, ch);
    }
    memcpy(was, s, n);
    was[n] = 0;
    g_dp.text_w[row] = (uint8_t)(n + 2);   /* a black cell either side */
    if (n) g_dp.text_rows |= 1u << row;
    else g_dp.text_rows &= ~(1u << row);
}
#else
#define DC_TEXT_ROWS 20
#define DC_TEXT_H    24

/* A line of text on screen row `row` (24 px rows, 0-19), in the text texture
 * at the same place. */
/* Video memory is slow to write: a row is redrawn only when its text changes,
   and only as wide as the old or new text (bfont's opaque cells clear behind). */
static void dp_text_row(int row, const char *s) {
    static char last[20][88];
    if (row < 0 || row >= 20) return;
    if (!strncmp(last[row], s, sizeof last[row] - 1)) return;
    uint16_t *d = (uint16_t *)g_dp.text + row * 24 * 1024;
    size_t n = strlen(s), was = strlen(last[row]);
    if (n > 84) n = 84;
    char buf[88];
    memcpy(buf, s, n); buf[n] = 0;
    bfont_draw_str(d + 8, 1024, true, buf);
    if (was > n)
        for (int y = 0; y < 24; y++) memset(d + y * 1024 + 8 + n * 12, 0, (was - n) * 12 * 2);
    memcpy(last[row], buf, n + 1);
    g_dp.text_rows |= 1u << row;
}
#endif

/* The stats rows; the minimal HUD (DC_HUD_MIN) shows none of them. */
#if DC_HUD_MIN
#define dp_text(row, s) ((void)(row), (void)(s))
#else
#define dp_text dp_text_row
#endif

/* A short line at half size in the frame's top right corner (the minimal
 * HUD's fps counter), kept in the text texture below the 20 rows. */
static void dp_corner(const char *s) {
    static char last[24];
    if (!strncmp(last, s, sizeof last - 1)) return;
    uint16_t *d = (uint16_t *)g_dp.text + 480 * 1024;
    size_t n = strlen(s), was = strlen(last);
    if (n > 20) n = 20;
    char buf[24];
    memcpy(buf, s, n); buf[n] = 0;
    bfont_draw_str(d, 1024, true, buf);
    if (was > n)
        for (int y = 0; y < 24; y++) memset(d + y * 1024 + n * 12, 0, (was - n) * 12 * 2);
    memcpy(last, buf, n + 1);
    g_dp.corner_w = (unsigned)n * 12;
}

/* ---- Submission ------------------------------------------------------------------- */

/* Straight into a store queue and on to the TA (KOS's direct rendering: an open
 * list holds the queues): pvr_prim's call and copy were ~4% of a frame. */
static inline void dp_vertex(uint32_t flags, float x, float y, float z, float u, float v,
                             uint32_t argb, uint32_t oargb) {
    pvr_vertex_t *d = pvr_dr_target();
    d->flags = flags; d->x = x; d->y = y; d->z = z; d->u = u; d->v = v; d->argb = argb; d->oargb = oargb;
    pvr_dr_commit(d);
}

static inline void dp_hdr(const pvr_poly_hdr_t *h) {
    uint32_t *d = pvr_dr_target();
    const uint32_t *s = (const uint32_t *)h;
    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; d[4] = s[4]; d[5] = s[5]; d[6] = s[6]; d[7] = s[7];
    pvr_dr_commit(d);
}

/* A board-sized layer texture (512x512, a texel a board pixel): the view's
 * part of it, on the view. */
static void dp_layer(pvr_list_t list, pvr_ptr_t tex, int fmt, bool depth_always, float z) {
    const float k = 1.0f / 512.0f;
    const float u0 = DC_VIEW_X * k, v0 = DC_VIEW_Y * k;
    const float u1 = (DC_VIEW_X + DC_VIEW_W) * k, v1 = (DC_VIEW_Y + DC_VIEW_H) * k;
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_poly_cxt_txr(&cxt, list, fmt | PVR_TXRFMT_NONTWIDDLED, 512, 512, tex, DC_FILTER);
    cxt.depth.comparison = depth_always ? PVR_DEPTHCMP_ALWAYS : PVR_DEPTHCMP_GEQUAL;
    pvr_poly_compile(&hdr, &cxt);
    dp_hdr(&hdr);
    dp_vertex(PVR_CMD_VERTEX,     DC_VX0, DC_VY0, z, u0, v0, 0xFFFFFFFFu, 0);
    dp_vertex(PVR_CMD_VERTEX,     DC_VX1, DC_VY0, z, u1, v0, 0xFFFFFFFFu, 0);
    dp_vertex(PVR_CMD_VERTEX,     DC_VX0, DC_VY1, z, u0, v1, 0xFFFFFFFFu, 0);
    dp_vertex(PVR_CMD_VERTEX_EOL, DC_VX1, DC_VY1, z, u1, v1, 0xFFFFFFFFu, 0);
}

/* A black bar over everything drawn so far: the 3D past the board's screen. */
static void dp_bar(float x0, float y0, float x1, float y1) {
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.blend.src = PVR_BLEND_ONE;
    cxt.blend.dst = PVR_BLEND_ZERO;
    pvr_poly_compile(&hdr, &cxt);
    dp_hdr(&hdr);
    dp_vertex(PVR_CMD_VERTEX,     x0, y0, 1.0e3f, 0.0f, 0.0f, 0xFF000000u, 0);
    dp_vertex(PVR_CMD_VERTEX,     x1, y0, 1.0e3f, 0.0f, 0.0f, 0xFF000000u, 0);
    dp_vertex(PVR_CMD_VERTEX,     x0, y1, 1.0e3f, 0.0f, 0.0f, 0xFF000000u, 0);
    dp_vertex(PVR_CMD_VERTEX_EOL, x1, y1, 1.0e3f, 0.0f, 0.0f, 0xFF000000u, 0);
}

/* Text row r: 640 texels by DC_TEXT_H (HUD=prof: as wide as its text), at
 * the frame's left (at 0.8 in a 512x384 one, so that all the rows fit). */
static void dp_text_rect(int r) {
    const float k = DC_SCR_W / 640.0f, h = (float)DC_TEXT_H;
    float y = (float)r * h * k, v0 = (float)r * h / 512.0f;
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                     1024, 512, g_dp.text, DC_FRAME512 ? PVR_FILTER_BILINEAR : PVR_FILTER_NONE);
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    pvr_poly_compile(&hdr, &cxt);
    dp_hdr(&hdr);
#if DC_HUD_PROF
    const float w = (float)g_dp.text_w[r] * 8.0f;
#else
    const float w = 640.0f;
#endif
    float v1 = v0 + h / 512.0f, u1 = w / 1024.0f, y1 = y + h * k, x1 = w * k;
    dp_vertex(PVR_CMD_VERTEX,     0.0f, y,  1.0e3f, 0.0f, v0, 0xFFFFFFFFu, 0);
    dp_vertex(PVR_CMD_VERTEX,     x1,   y,  1.0e3f, u1,   v0, 0xFFFFFFFFu, 0);
    dp_vertex(PVR_CMD_VERTEX,     0.0f, y1, 1.0e3f, 0.0f, v1, 0xFFFFFFFFu, 0);
    dp_vertex(PVR_CMD_VERTEX_EOL, x1,   y1, 1.0e3f, u1,   v1, 0xFFFFFFFFu, 0);
}

static void dp_corner_rect(void) {
    if (!g_dp.corner_w) return;
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                     1024, 512, g_dp.text, PVR_FILTER_BILINEAR);
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    pvr_poly_compile(&hdr, &cxt);
    dp_hdr(&hdr);
    float x1 = DC_SCR_W - 12.0f, x0 = x1 - (float)g_dp.corner_w * 0.5f, y0 = 8.0f, y1 = 20.0f;
    float u1 = (float)g_dp.corner_w / 1024.0f, v0 = 480.0f / 512.0f, v1 = 504.0f / 512.0f;
    dp_vertex(PVR_CMD_VERTEX,     x0, y0, 1.0e3f, 0.0f, v0, 0xFFFFFFFFu, 0);
    dp_vertex(PVR_CMD_VERTEX,     x1, y0, 1.0e3f, u1,   v0, 0xFFFFFFFFu, 0);
    dp_vertex(PVR_CMD_VERTEX,     x0, y1, 1.0e3f, 0.0f, v1, 0xFFFFFFFFu, 0);
    dp_vertex(PVR_CMD_VERTEX_EOL, x1, y1, 1.0e3f, u1,   v1, 0xFFFFFFFFu, 0);
}

/* ---- The tile layers ------------------------------------------------------------- */

/* Both layers as pens (tile_cpu_draw), then into their textures: the back one
 * RGB565, the front one ARGB1555 with its empty pixels clear. A full redraw
 * was ~330 ms of SH-4 time, so as tile_compose_cpu does it, only the 8x8 blocks
 * a tile RAM change reaches are drawn again (tile_dirty_find), and only their
 * pixels converted. A colour change converts everything, and only when one of
 * the pens on screen changed: most of STF's palette writes are 3D colours. */
static uint8_t g_dp_pen_used[TILE_PEN_NONE + 1];   /* a superset of the pens on screen */

/* A row's pixels xs..xe (multiples of 16) into pair `pair`: converted in RAM,
 * then sent by the store queues, 32 bytes a burst, where 32-bit stores into
 * video memory each waited on the bus. */
static void dp_tiles_convert(const tile_cpu_t *tiles, const uint16_t *bgpen, int pair, int y, int xs, int xe) {
    static uint32_t rb[256] __attribute__((aligned(32))), rf[256] __attribute__((aligned(32)));
    const uint16_t *bg = tiles->bg + y * VIDEO_WIDTH, *fg = tiles->fg + y * VIDEO_WIDTH;
    for (int x = xs; x < xe; x += 2) {
        g_dp_pen_used[bg[x]] = g_dp_pen_used[bg[x + 1]] = 1;
        g_dp_pen_used[fg[x]] = g_dp_pen_used[fg[x + 1]] = 1;
        rb[x >> 1] = bgpen[bg[x]] | (uint32_t)bgpen[bg[x + 1]] << 16;
        rf[x >> 1] = g_dp.pen1555[fg[x]] | (uint32_t)g_dp.pen1555[fg[x + 1]] << 16;
    }
    pvr_txr_load(rb + (xs >> 1), (uint16_t *)g_dp.lbg[pair] + y * 512 + xs, (size_t)(xe - xs) * 2);
    pvr_txr_load(rf + (xs >> 1), (uint16_t *)g_dp.lfg[pair] + y * 512 + xs, (size_t)(xe - xs) * 2);
}

/* The CPU layers are double-buffered: the PVR may still be drawing the last
 * frame from one pair while this frame is decoded (pvr_check_ready says the
 * last scene was taken, not that it is drawn), so a frame's changes go into
 * the other pair, and the frame draws that. Each pair keeps the rows it has
 * not had yet: a pair gets the last frame's changes along with this one's.
 * Tilemap 0's line-scroll textures (pair 0) and tilemap 2's stay single. */
static struct {
    bool    all[2];                          /* the pair needs every row of the view */
    int16_t x0[2][VIDEO_HEIGHT], x1[2][VIDEO_HEIGHT];   /* else row y's pixels x0..x1 (none: x0 >= x1) */
} g_dp_owe;

static void dp_owe_add(int pair, bool all, const int16_t *x0, const int16_t *x1) {
    if (g_dp_owe.all[pair]) return;
    if (all) { g_dp_owe.all[pair] = true; return; }
    int16_t *o0 = g_dp_owe.x0[pair], *o1 = g_dp_owe.x1[pair];
    for (int y = 0; y < VIDEO_HEIGHT; y++) {
        if (x0[y] >= x1[y]) continue;
        if (o0[y] >= o1[y]) { o0[y] = x0[y]; o1[y] = x1[y]; continue; }
        if (x0[y] < o0[y]) o0[y] = x0[y];
        if (x1[y] > o1[y]) o1[y] = x1[y];
    }
}

/* This frame's changes (all, or rows x0..x1): into the pair the PVR is not
 * reading, with what that pair still owed; the other pair now owes them. */
static void dp_tiles_send(const tile_cpu_t *tiles, const uint16_t *bgpen, bool all, const int16_t *x0, const int16_t *x1) {
    int pair = g_dp.lcur ^ 1;
    dp_owe_add(pair, all, x0, x1);
    dp_owe_add(pair ^ 1, all, x0, x1);
    if (g_dp_owe.all[pair]) memset(g_dp_pen_used, 0, sizeof g_dp_pen_used);
    for (int y = DC_VIEW_Y; y < DC_VIEW_Y + DC_VIEW_H; y++) {
        int a = DC_VIEW_X, b = DC_VIEW_X + DC_VIEW_W;
        if (!g_dp_owe.all[pair]) { a = g_dp_owe.x0[pair][y]; b = g_dp_owe.x1[pair][y]; }
        if (a >= b) continue;
        a &= ~15; b = (b + 15) & ~15;   /* the store queues' 32-byte bursts; VIDEO_WIDTH is a multiple of 16 */
        dp_tiles_convert(tiles, bgpen, pair, y, a, b);
    }
    g_dp_owe.all[pair] = false;
    for (int y = 0; y < VIDEO_HEIGHT; y++) g_dp_owe.x0[pair][y] = g_dp_owe.x1[pair][y] = 0;
    g_dp.lcur = (uint8_t)pair;
}

/* Tilemap 2 with a per-line H scroll (the title's starfield, #358): redrawn on
 * the CPU, each scroll change was a whole line, ~2,100 of the 2,976 blocks a
 * frame (~129 ms). When tilemap 2 covers the back layer alone (its pair in
 * mode 0 with no window mask bit set on the view, so tilemap 3 draws nowhere
 * that shows), the PVR scrolls it instead: the whole 512x512 tilemap sits in
 * two textures, every pixel (RGB565) and its category 1 pixels (ARGB1555), and
 * each run of lines with one scroll is a strip whose U starts at -scroll. Only
 * the cells the game changes are drawn into them. Tilemaps 1 and 0 stay on the
 * CPU, in a back layer with its empty pixels clear, drawn behind the 3D by
 * depth: the translucent list's first quad, at the strips' z, GEQUAL.
 *
 * When tilemap 0 has its pair to itself as well, it goes the same way, its
 * category 0 pixels in one texture and its category 1 pixels in the other, and
 * the CPU draws no tile at all (m2-sonic: Mega Drive planes B and A, each
 * scrolled by line, which were a whole-view redraw every frame, ~25 ms;
 * Pinboard #481). Its pair's window layer, and pair 2/3's, show only round the
 * picture, past the view. */
static bool dp_ls_ok(const uint16_t *w, int t) {
    if (!(w[0x5000 + t] & 0x8000) || (w[0x5004 + t] & 0xE000)) return false;
    uint16_t vm[4] = { 0, 0, 0, 0 };              /* the mask bits of the view's columns */
    for (int c = DC_VIEW_X >> 3; c <= (DC_VIEW_X + DC_VIEW_W - 1) >> 3; c++) vm[c >> 4] |= (uint16_t)(0x8000u >> (c & 15));
    const uint16_t *m = w + (t ? 0x6800 : 0x6000) + DC_VIEW_Y * 4;
    for (int i = 0; i < DC_VIEW_H * 4; i++)
        if (m[i] & vm[i & 3]) return false;
    return true;
}

static uint8_t g_dp_chr_new[TMAPGFX_SIZE / 32 / 8];   /* chars changed since the last redraw (dp_tiles_chars) */

/* Cell i of a tilemap into both its textures; true if it shows in front. The
 * back texture has every pixel (tilemap 2, `opaque`, RGB565) or the category 0
 * ones (tilemap 0, ARGB1555). */
static bool dp_ls_cell(dp_ls_t *ls, const uint8_t *gfx, int i, uint16_t e, bool opaque) {
    int bank16 = ((e >> 7) & 0xFF) * 16;
    bool cat = (e >> 15) != 0, any = false;
    const uint8_t *g = gfx + (uint32_t)(e & 0x3FFF) * 32u;
    const uint16_t *p565 = g_dp.pen565 + bank16, *p1555 = g_dp.pen1555 + bank16;
    unsigned o = (unsigned)(i >> 6) * 8u * 256u + (unsigned)(i & 63) * 4u;   /* in 32-bit words */
    uint32_t *db = (uint32_t *)ls->back + o, *df = (uint32_t *)ls->front + o;
    for (int r = 0; r < 8; r++, g += 4, db += 256, df += 256) {
        const uint8_t nib[8] = { g[1] >> 4, g[1] & 15, g[0] >> 4, g[0] & 15, g[3] >> 4, g[3] & 15, g[2] >> 4, g[2] & 15 };
        for (int x = 0; x < 8; x += 2) {
            uint8_t a = nib[x], b = nib[x + 1];
            uint32_t f = (a ? p1555[a] : 0u) | (uint32_t)(b ? p1555[b] : 0u) << 16;
            db[x >> 1] = opaque ? p565[a] | (uint32_t)p565[b] << 16 : cat ? 0u : f;
            df[x >> 1] = cat ? f : 0u;
            any |= (a | b) != 0;
        }
    }
    return cat && any;
}

/* Bring a tilemap's textures to its entries w (0x1000 of them): every cell, or
 * only those whose entry changed, whose char changed (`chars`: g_dp_chr_new)
 * or whose palette bank changed colour (`banks`, or NULL). A Mega Drive game
 * cycles a few colours and animates a few patterns every few frames, and
 * redrawing the whole tilemap for each cost m2-sonic ~7 ms a frame (19 to
 * 12 ms of tiles on the Dreamcast). */
static void dp_ls_draw(dp_ls_t *ls, const uint16_t *w, const uint8_t *gfx, bool all, bool opaque,
                       bool chars, const uint8_t *banks) {
    if (all) {
        memset(ls->front_set, 0, sizeof ls->front_set);
        ls->front_cells = 0;
    }
    bool scan = all || chars || banks;
    for (int i = 0; i < 0x1000; i += 64) {
        if (!scan && !memcmp(w + i, ls->cells + i, 64 * sizeof *w)) continue;
        for (int k = i; k < i + 64; k++) {
            uint16_t e = w[k];
            if (!all) {
                unsigned c = e & 0x3FFFu;
                if (e == ls->cells[k] && !(chars && (g_dp_chr_new[c >> 3] >> (c & 7) & 1)) &&
                    !(banks && banks[(e >> 7) & 0xFF])) continue;
                ls->front_cells -= ls->front_set[k];
            }
            ls->cells[k] = e;
            ls->front_set[k] = dp_ls_cell(ls, gfx, k, e, opaque);
            ls->front_cells += ls->front_set[k];
        }
    }
}

/* A tilemap as strips: a run of lines with one scroll, cut where the texture
 * wraps (bilinear filtering wraps with it, so the cut does not show). */
static void dp_ls_strips(const dp_ls_t *ls, pvr_list_t list, pvr_ptr_t tex, int fmt, bool depth_always, float z) {
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_poly_cxt_txr(&cxt, list, fmt | PVR_TXRFMT_NONTWIDDLED, 512, 512, tex, DC_FILTER);
    cxt.depth.comparison = depth_always ? PVR_DEPTHCMP_ALWAYS : PVR_DEPTHCMP_GEQUAL;
    pvr_poly_compile(&hdr, &cxt);
    dp_hdr(&hdr);
    const float k = 1.0f / 512.0f;
    const int vx0 = DC_VIEW_X, vx1 = DC_VIEW_X + DC_VIEW_W, vy1 = DC_VIEW_Y + DC_VIEW_H;
    for (int y = DC_VIEW_Y; y < vy1; ) {
        int h = ls->h[y], ty = (y + ls->vy) & 511, e = y + 1;
        while (e < vy1 && ls->h[e] == h && ((e + ls->vy) & 511)) e++;
        float sy0 = DC_Y0 + (float)y * DC_SY, sy1 = DC_Y0 + (float)e * DC_SY;
        float v0 = (float)ty * k, v1 = (float)(ty + e - y) * k;
        int u = (-h) & 511, xs = 512 - u;           /* screen x 0 samples u; xs wraps to 0 */
        int cut[3] = { vx0, xs < vx0 ? vx0 : xs < vx1 ? xs : vx1, vx1 };
        for (int q = 0; q < 2; q++) {
            if (cut[q] >= cut[q + 1]) continue;
            float ua = (float)((u + cut[q]) & 511) * k, ub = ua + (float)(cut[q + 1] - cut[q]) * k;
            float xa = DC_X0 + (float)cut[q] * DC_SX, xb = DC_X0 + (float)cut[q + 1] * DC_SX;
            dp_vertex(PVR_CMD_VERTEX,     xa, sy0, z, ua, v0, 0xFFFFFFFFu, 0);
            dp_vertex(PVR_CMD_VERTEX,     xb, sy0, z, ub, v0, 0xFFFFFFFFu, 0);
            dp_vertex(PVR_CMD_VERTEX,     xa, sy1, z, ua, v1, 0xFFFFFFFFu, 0);
            dp_vertex(PVR_CMD_VERTEX_EOL, xb, sy1, z, ub, v1, 0xFFFFFFFFu, 0);
        }
        y = e;
    }
}

/* tile_cpu_draw less tilemaps 2 and 3: the back layer clear where nothing drew. */
static void dp_ls_rest(tile_cpu_t *c, const uint8_t *gfx, const int16_t *x0, const int16_t *x1) {
    for (int y = 0; y < VIDEO_HEIGHT; y++)
        for (int x = x0[y]; x < x1[y]; x++)
            c->bg[y * VIDEO_WIDTH + x] = c->fg[y * VIDEO_WIDTH + x] = TILE_PEN_NONE;
    s24_draw_tilemap(c->words, gfx, 1, 0, false, c->bg, x0, x1);
    s24_draw_tilemap(c->words, gfx, 0, 0, false, c->bg, x0, x1);
    s24_draw_tilemap(c->words, gfx, 1, 1, false, c->fg, x0, x1);
    s24_draw_tilemap(c->words, gfx, 0, 1, false, c->fg, x0, x1);
}

/* m2-sdk's tile framebuffer (m2_tilefb.h) gives every screen cell a char of its
 * own and draws the sprites into them, so char RAM changes in every frame that
 * anything moves; m2-pacman redrew the whole screen for it (~70 ms a frame,
 * Pinboard #463). The KBs written since the last redraw (memory.h gfx_dirty)
 * are hashed a char at a time, and only the cells that show a char whose hash
 * changed are drawn again. */
static uint32_t g_dp_chr_hash[TMAPGFX_SIZE / 32];

static bool dp_tiles_chars(memory_bus_t *bus) {
    bool any = false;
    memset(g_dp_chr_new, 0, sizeof g_dp_chr_new);
    volatile uint8_t *dk = bus->gfx_dirty;
    for (unsigned k = 0; k < TMAPGFX_SIZE >> 10; k++) {
        if (!dk[k]) continue;
        dk[k] = 0;
        const uint32_t *p = (const uint32_t *)(bus->tmapgfx + k * 1024u);
        for (unsigned c = k * 32u; c < k * 32u + 32u; c++, p += 8) {
            uint32_t h = 2166136261u;
            for (int j = 0; j < 8; j++) h = (h ^ p[j]) * 16777619u;
            if (h == g_dp_chr_hash[c]) continue;
            g_dp_chr_hash[c] = h;
            g_dp_chr_new[c >> 3] |= (uint8_t)(1u << (c & 7));
            any = true;
        }
    }
    return any;
}

/* The cells of tilemaps 0-3 that show a changed char, marked. */
static void dp_tiles_mark_chars(tile_dirty_t *d, const uint16_t *w) {
    for (int l = 0; l < 4; l++)
        for (int i = 0; i < 0x1000; i++) {
            unsigned c = w[l * 0x1000 + i] & 0x3FFFu;
            if (!(g_dp_chr_new[c >> 3] >> (c & 7) & 1)) continue;
            tile_dirty_mark_cell(d, w, l, i & 63, i >> 6);
        }
}

_Static_assert(offsetof(memory_bus_t, tile) % 4 == 0, "dp_tiles reads tile RAM as words");
_Static_assert(offsetof(memory_bus_t, tmapgfx) % 4 == 0, "dp_tiles_chars reads char RAM as words");
_Static_assert(TILE_SNAP_WORDS % 512 == 0, "dp_tiles copies whole KBs");

/* What dp_tiles keeps between frames: the bus generations it last drew, the
 * blocks that changed this time and each row's span of them. */
typedef struct {
    uint32_t tile, gfx, pal, lut;   /* bus->gen_* as of the last draw */
    bool     valid;                 /* a draw has happened */
    tile_dirty_t d;
    int16_t  x0[VIDEO_HEIGHT], x1[VIDEO_HEIGHT];   /* each row's changed span, x0 >= x1: none */
    uint8_t  banks[256];            /* the palette banks whose colours changed */
} dp_tiles_state_t;
static dp_tiles_state_t s_tl;   /* zero, so bss; the first draw puts the sentinels in */

/* The pen colours: tilemap 2's textures are drawn in them. Marks the banks
 * whose colours changed and returns whether any did; *all when a pen the
 * CPU layers use did. */
static bool dp_tiles_pens(memory_bus_t *bus, uint8_t *banks, bool *all) {
    bool bank_any = false;
    memset(banks, 0, 256);
    uint8_t chan[3][32];
    video_pen_channels(bus, chan);
    for (int p = 0; p < TILE_PEN_NONE; p++) {
        uint16_t c = pal_read16(bus, p);
        uint8_t r = chan[0][c & 31], g = chan[1][(c >> 5) & 31], b = chan[2][(c >> 10) & 31];
        uint16_t c565  = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
        uint16_t c1555 = (uint16_t)(0x8000u | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
        if (c565 != g_dp.pen565[p] || c1555 != g_dp.pen1555[p]) {
            if (g_dp_pen_used[p]) *all = true;
            banks[p >> 4] = 1;
            bank_any = true;
            g_dp.pen565[p] = c565;
            g_dp.pen1555[p] = c1555;
        }
    }
    g_dp.pen565[TILE_PEN_NONE] = 0;
    g_dp.pen1555[TILE_PEN_NONE] = 0;
    return bank_any;
}

/* The line-scroll layers: decide which tilemaps the PVR draws (g_ls.on, .both;
 * a change is a full redraw) and draw them. banks: the recoloured palette
 * banks, NULL when no colour changed. */
static void dp_tiles_ls(memory_bus_t *bus, const uint16_t *n, tile_dirty_t *d, bool chars, const uint8_t *banks) {
    bool ls = dp_ls_ok(n, 2), both = ls && dp_ls_ok(n, 0), ls_all = d->full;
    if (ls != g_ls.on || both != g_ls.both) { d->full = ls_all = true; g_ls.on = ls; g_ls.both = both; }
    if (chars && !d->full && !both) dp_tiles_mark_chars(d, n);
    if (ls) {
        for (int y = 0; y < VIDEO_HEIGHT; y++) g_ls.t2.h[y] = n[0x4400 + y] & 0x1FF;
        g_ls.t2.vy = n[0x5006] & 0x1FF;
        dp_ls_draw(&g_ls.t2, n + 0x2000, bus->tmapgfx, ls_all, true, chars, banks);
    }
    if (both) {
        for (int y = 0; y < VIDEO_HEIGHT; y++) g_ls.t0.h[y] = n[0x4000 + y] & 0x1FF;
        g_ls.t0.vy = n[0x5004] & 0x1FF;
        dp_ls_draw(&g_ls.t0, n, bus->tmapgfx, ls_all, false, chars, banks);
    }
}

/* Bring the CPU layers' copy of tile RAM up to date and mark the blocks that
 * changed. Only a KB written since the last redraw can differ (memory.h
 * tile_dirty). Under line scroll, pair 2/3's cells, line scroll and mask
 * (words 0x2000-0x3FFF, 0x4400-0x47FF, 0x6800-0x6FFF: KBs 16-31, 34-35,
 * 52-55) stay as they were for the CPU's layers, which no longer draw them.
 * These used to be counted in 2 KB units, so under a line scroll tilemap 1's
 * changes never reached the CPU's copy (found reading this for Pinboard
 * #463). */
static void dp_tiles_copy(memory_bus_t *bus, tile_cpu_t *tiles, const uint16_t *n, tile_dirty_t *d) {
    bool ls = g_ls.on, both = g_ls.both;
    volatile uint8_t *dk = bus->tile_dirty;
    for (int k = 0; k < TILE_SNAP_WORDS / 512 && both; k++) dk[k] = 0;   /* the CPU's copy waits for d.full */
    for (int k = 0; k < TILE_SNAP_WORDS / 512 && !both; k++) {
        if (!d->full && !dk[k]) continue;
        dk[k] = 0;
        if (ls && ((k >= 16 && k < 32) || k == 34 || k == 35 || (k >= 52 && k < 56))) continue;
        if (!d->full) tile_dirty_find_range(d, tiles->words, n, k * 512, k * 512 + 512);
        memcpy(tiles->words + k * 512, n + k * 512, 1024);
    }
    if (d->count > TILE_BLK_W * TILE_BLK_H * 3 / 4) d->full = true;
}

/* Each row's span of changed blocks, in pixels and clipped to the view (the
 * whole view when d->full; x0 >= x1 for a row with nothing). */
static void dp_tiles_extents(const tile_dirty_t *d, int16_t *x0, int16_t *x1) {
    for (int by = 0; by < TILE_BLK_H; by++) {
        int b0 = 0, b1 = TILE_BLK_W;
        if (!d->full) {
            while (b0 < TILE_BLK_W && !d->blk[by][b0]) b0++;
            while (b1 > b0 && !d->blk[by][b1 - 1]) b1--;
        }
        int a = b0 * 8 < DC_VIEW_X ? DC_VIEW_X : b0 * 8;   /* nothing past the view */
        int b = b1 * 8 > DC_VIEW_X + DC_VIEW_W ? DC_VIEW_X + DC_VIEW_W : b1 * 8;
        for (int y = by * 8; y < by * 8 + 8; y++) {
            bool in = y >= DC_VIEW_Y && y < DC_VIEW_Y + DC_VIEW_H;
            x0[y] = (int16_t)a; x1[y] = (int16_t)(in ? b : a);
        }
    }
}

/* The CPU layers' pixels to the PVR's: the whole view (all) or the changed
 * spans (dirty), into the pair the PVR is not reading (dp_tiles_send). */
static void dp_tiles_show(tile_cpu_t *tiles, const dp_tiles_state_t *s, bool all, bool dirty) {
    const uint16_t *bgpen = g_ls.on ? g_dp.pen1555 : g_dp.pen565;
    if (all || dirty) dp_tiles_send(tiles, bgpen, all, s->x0, s->x1);
}

/* Tile RAM or the characters changed: the dirty cells, their rows' extents,
 * and the CPU layer's draw of them. */
static void dp_tiles_redraw(memory_bus_t *bus, tile_cpu_t *tiles, dp_tiles_state_t *s, bool chars, const uint8_t *banks) {
    tile_dirty_t *d = &s->d;
    const uint16_t *n = (const uint16_t *)bus->tile;
    dp_tiles_ls(bus, n, d, chars, banks);
    dp_tiles_copy(bus, tiles, n, d);
    dp_tiles_extents(d, s->x0, s->x1);
    if (g_ls.both || !(d->full || d->count)) return;
    if (g_ls.on) dp_ls_rest(tiles, bus->tmapgfx, s->x0, s->x1);
    else         tile_cpu_draw(tiles, bus->tmapgfx, s->x0, s->x1);
}

/* Only colours changed: the textured layers take the new pens. */
static void dp_tiles_recolour(memory_bus_t *bus, const uint8_t *banks) {
    if (g_ls.on)   dp_ls_draw(&g_ls.t2, g_ls.t2.cells, bus->tmapgfx, false, true, false, banks);
    if (g_ls.both) dp_ls_draw(&g_ls.t0, g_ls.t0.cells, bus->tmapgfx, false, false, false, banks);
}

static void dp_tiles(memory_bus_t *bus, tile_cpu_t *tiles) {
    dp_tiles_state_t *s = &s_tl;
    tile_dirty_t *d = &s->d;
    if (!s->valid) s->tile = s->gfx = s->pal = s->lut = ~0u;   /* no generation matches the first draw */
    bool redraw = bus->gen_tile != s->tile || bus->gen_gfx != s->gfx || !s->valid;
    bool recolour = bus->gen_pal != s->pal || bus->gen_lut != s->lut || !s->valid;
    if (!redraw && !recolour) return;
    memset(d, 0, sizeof *d);
    d->full = !s->valid;
    bool chars = bus->gen_gfx != s->gfx && dp_tiles_chars(bus);
    bool all = false, bank_any = false;
    s->tile = bus->gen_tile; s->gfx = bus->gen_gfx; s->pal = bus->gen_pal; s->lut = bus->gen_lut;
    /* The pen colours first: tilemap 2's textures are drawn in them. */
    if (recolour) bank_any = dp_tiles_pens(bus, s->banks, &all);
    if (redraw) dp_tiles_redraw(bus, tiles, s, chars, bank_any ? s->banks : NULL);
    else if (bank_any) dp_tiles_recolour(bus, s->banks);
    if (d->full || !s->valid) all = true;
    s->valid = true;
    if (g_ls.both) {   /* the CPU layers are tilemap 0's textures, pair 0: the next CPU frame goes into pair 1 */
        g_dp.lcur = 0;
        return;
    }
    dp_tiles_show(tiles, s, all, redraw && d->count);
}

/* ---- The 3D scene ------------------------------------------------------------------ */

typedef struct {
    float ax, bx, ay, by;     /* screen x = bx + ax * x / -z, y = by + ay * y / -z */
    float x0, y0, x1, y1;     /* the window, in screen pixels: the board draws nothing outside it */
} dp_proj_t;
#define DP_MAX_RUNS 256
static dp_proj_t g_dp_proj[DP_MAX_RUNS];
static uint32_t  g_dp_order[2][GEO3D_MAX_TRIS];

/* The frame's faces, as geo3d hands them over (GEO3D_DC_SINK): a mesh's
 * corners projected once into the pool, and per face a record with all the
 * PVR needs, its colours worked out, its texture found and its sort word made.
 * The submit then only copies. */
typedef struct { float sx, sy, w, x, y, z; } dcv_t;   /* screen x, y, 1/-z; eye space */
#define DC_MAX_VERTS 8192
#define DCF_TRANS   1u   /* the translucent list */
#define DCF_CHECKER 2u
#ifndef DP_WCLIP
#define DP_WCLIP    1    /* clip a face to its window (0: the PVR's frame edge only) */
#endif
#define DCF_CLIP    4u   /* a corner is in front of the near plane or outside the window */
#define DCF_QUAD    8u   /* four corners, a strip (a packed mesh's quad, geo3d_dc_sface) */
typedef struct {
    uint16_t  v[4];
    uint8_t   run, kind, var, pal;   /* var: the header's variant, dp_hdr_compile's face bits << 1;
                                        pal: the face's own palette bank (dp_pal_bank), 0: none */
    dc_tex_t *tex;
    uint32_t  base, off;
    float     u[4], t[4];            /* the corners' texture coordinates, in [0, 1] units */
} dcf_t;
static dcv_t    g_dcv[DC_MAX_VERTS];
static int      g_dcv_n;
static dcf_t    g_dcf[GEO3D_MAX_TRIS];
static int      g_dcf_n;
static uint32_t g_dcf_key[GEO3D_MAX_TRIS];
static const dp_proj_t *g_dp_cur;    /* the run being decoded */
static int      g_dp_cur_run, g_dp_cur_slice;
static memory_bus_t *g_dp_bus;
static const uint8_t *g_dp_rs_textures;   /* the romset's: the strip pack's meshes read only it */

/* The last face's inputs and what came of them: the two halves of a quad come
 * one after the other, and a model's faces share textures. */
static struct {
    float    tx, ty, tw, th, fl;  dc_tex_t *tex;              /* dp_tex_get */
    uint32_t skey; dc_tex_t *stex;                            /* dp_tex_key */
    dp_col_in_t c; bool ctex, ctrans; uint32_t base, off; uint8_t pal;   /* dp_face_colour */
} g_dp_memo;

static inline void dp_memo_reset(void) {
    memset(&g_dp_memo, 0, sizeof g_dp_memo);
    g_dp_memo.tw = g_dp_memo.c.r = -1.0f;
}

static int geo3d_dc_verts(const vec3_t *tv, int n) {
    if (g_dcv_n + n > DC_MAX_VERTS) { g_dp.faces_dropped++; return -1; }
    const dp_proj_t *P = g_dp_cur;
    dcv_t *o = &g_dcv[g_dcv_n];
    for (int i = 0; i < n; i++, o++) {
        float x = tv[i].x, y = tv[i].y, z = tv[i].z;
        float w = z < 0.0f ? -1.0f / z : 0.0f;
        o->sx = P->bx + P->ax * x * w;
        o->sy = P->by + P->ay * y * w;
        o->w = w; o->x = x; o->y = y; o->z = z;
    }
    int base = g_dcv_n;
    g_dcv_n += n;
    return base;
}

/* The face's texture, found once for both its triangles (the memo: the two
 * halves of a quad and a model's faces share them). */
static inline dc_tex_t *dp_face_tex(float tx, float ty, float tw, float th, float fl, unsigned f) {
    if ((f & GEO3D_FACE_CHECKER) || !(tw > 0.0f)) return NULL;
    if (tx == g_dp_memo.tx && ty == g_dp_memo.ty && tw == g_dp_memo.tw && th == g_dp_memo.th && fl == g_dp_memo.fl)
        return g_dp_memo.tex;
    dc_tex_t *tex = dp_tex_get(g_dp_bus, tx, ty, tw, th, f);
    g_dp_memo.tx = tx; g_dp_memo.ty = ty; g_dp_memo.tw = tw; g_dp_memo.th = th;
    g_dp_memo.fl = fl; g_dp_memo.tex = tex;
    return tex;
}

static inline void dp_face_col(float r, float g, float b, float lb, float pl, bool ctex, bool ctrans) {
    if (r != g_dp_memo.c.r || g != g_dp_memo.c.g || b != g_dp_memo.c.b || pl != g_dp_memo.c.pl ||
            lb != g_dp_memo.c.lb || ctex != g_dp_memo.ctex || ctrans != g_dp_memo.ctrans) {
        g_dp_memo.c = (dp_col_in_t){ r, g, b, lb, pl };
        g_dp_memo.ctex = ctex;
        g_dp_memo.ctrans = ctrans;
        dp_face_colour(g_dp_bus, &g_dp_memo.c, ctex, ctrans, &g_dp_memo.base, &g_dp_memo.off, &g_dp_memo.pal);
    }
}

/* One triangle's record: corners a, b, c of the pool, texture coordinates in
 * texels (u[i], v[i]: corner i), the face's kind bits and header variant, and
 * key the board's flat key or < 0 for the nearest corner's. */
static inline void dp_tri_put(int a, int b, int c, const float *u, const float *v, int i, int j, int k,
                              unsigned kind, unsigned var, dc_tex_t *tex, int32_t key) {
    if (g_dcf_n >= GEO3D_MAX_TRIS) { g_dp.faces_dropped++; return; }
    const int t = g_dcf_n++;
    dcf_t *F = &g_dcf[t];
    const dcv_t *A = &g_dcv[a], *B = &g_dcv[b], *C = &g_dcv[c];
    F->v[0] = (uint16_t)a; F->v[1] = (uint16_t)b; F->v[2] = (uint16_t)c;
    F->run  = (uint8_t)g_dp_cur_run;
    const dp_proj_t *P = &g_dp_proj[g_dp_cur_run];
    bool in = A->z <= -DC_NEAR && B->z <= -DC_NEAR && C->z <= -DC_NEAR && (!DP_WCLIP || P->x0 < -1.0e29f ||
              (fminf(fminf(A->sx, B->sx), C->sx) >= P->x0 && fmaxf(fmaxf(A->sx, B->sx), C->sx) <= P->x1 &&
               fminf(fminf(A->sy, B->sy), C->sy) >= P->y0 && fmaxf(fmaxf(A->sy, B->sy), C->sy) <= P->y1));
    F->kind = (uint8_t)(kind | (in ? 0u : DCF_CLIP));
    F->var  = (uint8_t)var;
    F->tex  = tex;
    F->base = g_dp_memo.base; F->off = g_dp_memo.off;
    F->pal  = tex ? g_dp_memo.pal : 0;
    float su = tex ? tex->su : 0.0f, sv = tex ? tex->sv : 0.0f;
    F->u[0] = u[i] * su; F->t[0] = v[i] * sv;
    F->u[1] = u[j] * su; F->t[1] = v[j] * sv;
    F->u[2] = u[k] * su; F->t[2] = v[k] * sv;
    /* The sort word (dp_sort): the board's flat key, or the nearest corner's. */
    uint32_t q = key >= 0 ? (uint32_t)key : geo3d_board_zkey(fminf(-A->z, fminf(-B->z, -C->z)));
    q = q > 0xFFFFu ? 0xFFFFu : q;
    /* complemented, so ascending passes give descending order */
    g_dcf_key[t] = ~(((uint32_t)g_dp_cur_slice << 29) | q << 13 | (0x1FFFu - (uint32_t)t));
}

static inline unsigned dp_kind(unsigned f, float tw) {
    return ((f & GEO3D_FACE_CHECKER) ? DCF_CHECKER | DCF_TRANS : 0u) |
           ((f & GEO3D_FACE_TRANSPARENT) && tw > 0.0f ? DCF_TRANS : 0u);
}
#define DP_VAR(f) (((f) & (GEO3D_FACE_TRANSPARENT | GEO3D_FACE_MIRROR_X | GEO3D_FACE_MIRROR_Y)) << 1)

static void geo3d_dc_tri(int a, int b, int c, float ua, float va, float ub, float vb, float uc, float vc,
                         float r, float g, float b_, float tx, float ty, float tw, float th,
                         float lb, float pl, float fl) {
    if (g_dcf_n >= GEO3D_MAX_TRIS) { g_dp.faces_dropped++; return; }
    unsigned f = (unsigned)(fl + 0.5f);
    float u[3] = { ua, ub, uc }, v[3] = { va, vb, vc };
    if (tw > 256.0f || th > 256.0f) dp_big_window(&tx, &ty, &tw, &th, u, v, 3);
    dc_tex_t *tex = dp_face_tex(tx, ty, tw, th, fl, f);
    dp_face_col(r, g, b_, lb, pl, tex != NULL || (tw > 0.0f && !(f & GEO3D_FACE_CHECKER)),
                (f & GEO3D_FACE_TRANSPARENT) != 0);
    uint32_t key = g_geo3d_emit_flat >= 0.0f ? (uint32_t)(g_geo3d_emit_flat * 65536.0f) : 0u;
    dp_tri_put(a, b, c, u, v, 0, 1, 2, dp_kind(f, tw), DP_VAR(f), tex,
               g_geo3d_emit_flat >= 0.0f ? (int32_t)(key > 0xFFFFu ? 0xFFFFu : key) : -1);
}

static void geo3d_dc_face(int v0, const geo3d_cface_t *F, int cut, float r, float g, float b, float pl, int32_t key) {
    if (g_dcf_n >= GEO3D_MAX_TRIS) { g_dp.faces_dropped++; return; }
    unsigned f = (unsigned)(F->fl + 0.5f);
    float tx = F->tx, ty = F->ty, tw = F->tw, th = F->th;
    const float *uu = F->uvu, *vv = F->uvv;
    float wu[4], wv[4];
    if (tw > 256.0f || th > 256.0f) {
        memcpy(wu, F->uvu, sizeof wu); memcpy(wv, F->uvv, sizeof wv);
        if (dp_big_window(&tx, &ty, &tw, &th, wu, wv, F->is_tri ? 3 : 4)) { uu = wu; vv = wv; }
    }
    dc_tex_t *tex = dp_face_tex(tx, ty, tw, th, F->fl, f);
    dp_face_col(r, g, b, F->lb, pl, tex != NULL || (F->tw > 0.0f && !(f & GEO3D_FACE_CHECKER)),
                (f & GEO3D_FACE_TRANSPARENT) != 0);
    const unsigned kind = dp_kind(f, F->tw), var = DP_VAR(f);
    const int a = v0 + F->ai, bb = v0 + F->bi, c = v0 + F->ci, d = v0 + F->di;
    if (cut == 0) {
        dp_tri_put(a, bb, c, uu, vv, 0, 1, 2, kind, var, tex, key);
    } else if (cut == 1) {
        dp_tri_put(a, bb, c, uu, vv, 0, 1, 2, kind, var, tex, key);
        dp_tri_put(bb, d, c, uu, vv, 1, 3, 2, kind, var, tex, key);
    } else {
        dp_tri_put(a, bb, d, uu, vv, 0, 1, 3, kind, var, tex, key);
        dp_tri_put(a, d, c, uu, vv, 0, 3, 2, kind, var, tex, key);
    }
}

/* ---- Packed meshes (STRIPS.PAK, dc_strips.h) ------------------------------------- */

/* The texture a packed face names by its key (dcs_tex_key), cut as dp_tex_get
 * cuts it; the memo holds the last. */
static inline dc_tex_t *dp_tex_key(uint32_t key) {
    if (key == g_dp_memo.skey) return g_dp_memo.stex;
    dc_tex_t *tex = dp_tex_get(g_dp_bus, (float)((key >> 18) & 2047u), (float)((key >> 8) & 1023u),
                               (float)(1u << ((key >> 4) & 15u)), (float)(1u << (key & 15u)),
                               (key >> 29) & 1u ? GEO3D_FACE_SHEET1 : 0u);
    g_dp_memo.skey = key;
    g_dp_memo.stex = tex;
    return tex;
}

/* A packed face's record, dp_tri_put's for its n corners in strip order (o:
 * indexes into the face's A B C D) with their u, v as packed; a quad is one
 * record, its key the nearest of the four. */
static inline void dp_strip_put(int v0, const geo3d_sface_t *F, const geo3d_svert_t *s, const int *o, int n,
                                unsigned kind, unsigned var, dc_tex_t *tex, int32_t key) {
    if (g_dcf_n >= GEO3D_MAX_TRIS) { g_dp.faces_dropped++; return; }
    const int t = g_dcf_n++;
    dcf_t *R = &g_dcf[t];
    const int c[4] = { v0 + F->ai, v0 + F->bi, v0 + F->ci, v0 + F->di };
    const dp_proj_t *P = &g_dp_proj[g_dp_cur_run];
    float zn = 1.0e30f, x0 = 1.0e30f, y0 = 1.0e30f, x1 = -1.0e30f, y1 = -1.0e30f;
    bool in = true;
    for (int k = 0; k < n; k++) {
        const dcv_t *V = &g_dcv[c[o[k]]];
        R->v[k] = (uint16_t)c[o[k]];
        R->u[k] = s[o[k]].u; R->t[k] = s[o[k]].v;
        zn = fminf(zn, -V->z);
        in = in && V->z <= -DC_NEAR;
        x0 = fminf(x0, V->sx); x1 = fmaxf(x1, V->sx);
        y0 = fminf(y0, V->sy); y1 = fmaxf(y1, V->sy);
    }
    in = in && (!DP_WCLIP || P->x0 < -1.0e29f || (x0 >= P->x0 && x1 <= P->x1 && y0 >= P->y0 && y1 <= P->y1));
    R->run  = (uint8_t)g_dp_cur_run;
    R->kind = (uint8_t)(kind | (n == 4 ? DCF_QUAD : 0u) | (in ? 0u : DCF_CLIP));
    R->var  = (uint8_t)var;
    R->tex  = tex;
    R->base = g_dp_memo.base; R->off = g_dp_memo.off;
    R->pal  = tex ? g_dp_memo.pal : 0;
    uint32_t q = key >= 0 ? (uint32_t)key : geo3d_board_zkey(zn);
    q = q > 0xFFFFu ? 0xFFFFu : q;
    g_dcf_key[t] = ~(((uint32_t)g_dp_cur_slice << 29) | q << 13 | (0x1FFFu - (uint32_t)t));
}

/* geo3d_dc_face for a packed face: its texture by key, its corners' u, v as
 * packed, a quad one record (cut 1: A B C D, cut 2: B A D C). */
static void geo3d_dc_sface(int v0, const geo3d_sface_t *F, const geo3d_svert_t *s, int cut,
                           float r, float g, float b, float pl, int32_t key) {
    if (g_dcf_n >= GEO3D_MAX_TRIS) { g_dp.faces_dropped++; return; }
    const unsigned f = F->fl;
    const bool textured = (F->bits & GEO3D_SF_TEXTURED) != 0;
    dc_tex_t *tex = F->tex ? dp_tex_key(F->tex) : NULL;
    dp_face_col(r, g, b, F->lb, pl, tex != NULL || (textured && !(f & GEO3D_FACE_CHECKER)),
                (f & GEO3D_FACE_TRANSPARENT) != 0);
    const unsigned kind = dp_kind(f, textured ? 1.0f : 0.0f), var = DP_VAR(f);
    static const int o1[4] = { 0, 1, 2, 3 }, o2[4] = { 1, 0, 3, 2 };
    dp_strip_put(v0, F, s, cut == 2 ? o2 : o1, cut == 0 ? 3 : 4, kind, var, tex, key);
}

/* geo3d_mesh_get's: the packed mesh m names, if STRIPS.PAK has it, copied
 * into the arena at at (room bytes). */
static int geo3d_strips_load(struct geo3d_cmesh *m, uint8_t *at, size_t room) {
    const dcs_head_t *h = g_pg.sp;
    const geo3d_models_t *md = &m->md;
    if (!h || md->materials != g_dp_rs_textures || md->polygons_size != h->polygons_size ||
            md->materials_size != h->textures_size || md->table_off != h->table_off ||
            md->table_count != h->table_count || md->mesh_ptr_subtract != h->mesh_ptr_subtract ||
            md->mesh_ptr_add != h->mesh_ptr_add) return 0;
    const dcs_index_t *ix = (const dcs_index_t *)(h + 1), *e = NULL;
    uint32_t lo = 0, hi = h->n;
    while (lo < hi && !e) {
        uint32_t mid = (lo + hi) / 2;
        int c = dcs_key_cmp(ix[mid].model_idx, ix[mid].mat_ptr, ix[mid].uv_ptr, m->model_idx, m->mat_ptr, m->uv_ptr);
        if (!c) e = &ix[mid];
        else if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    if (!e) return 0;
    geo3d_sp_head_t sh;
    pg_sp_copy(&sh, e->off, sizeof sh);
    const uint32_t body = e->len - (uint32_t)sizeof sh;
    if (body > room) return -1;
    pg_sp_copy(at, e->off + (uint32_t)sizeof sh, body);
    const size_t sv_bytes = ((size_t)sh.n_sv * sizeof(vec3_t) + 31u) & ~(size_t)31u;
    m->sv      = (vec3_t *)at;
    m->faces   = NULL;
    m->sfaces  = (const geo3d_sface_t *)(at + sv_bytes);
    m->strips  = (const geo3d_svert_t *)(m->sfaces + sh.n_faces);
    m->n_sv    = sh.n_sv;
    m->n_faces = sh.n_faces;
    m->bc      = (vec3_t){ sh.bc[0], sh.bc[1], sh.bc[2] };
    m->br      = sh.br;
    m->arena_len = body;
    return 1;
}

static void geo3d_dc_tri_xyz(float x0, float y0, float z0, float u0, float v0,
                             float x1, float y1, float z1, float u1, float v1,
                             float x2, float y2, float z2, float u2, float v2,
                             float r, float g, float b, float tx, float ty, float tw, float th,
                             float lb, float pl, float fl) {
    const vec3_t c[3] = { { x0, y0, z0 }, { x1, y1, z1 }, { x2, y2, z2 } };
    int v = geo3d_dc_verts(c, 3);
    if (v >= 0) geo3d_dc_tri(v, v + 1, v + 2, u0, v0, u1, v1, u2, v2, r, g, b, tx, ty, tw, th, lb, pl, fl);
}

/* The run's culling planes (game_render__view_cull_planes) from its
 * projection's x, y and w rows (gm_mat4_geo_projection). */
static void dp_cull_planes(const float *gp, int x0, int y0, int x1, int y1) {
    const float W = (float)VIDEO_WIDTH, H = (float)VIDEO_HEIGHT, M = 4.0f;
    const float r0[4] = { 2.0f * gp[0] / W, 0.0f, -(2.0f * gp[2] / W - 1.0f), 0.0f };
    const float r1[4] = { 0.0f, 2.0f * gp[1] / H, -(1.0f - 2.0f * gp[3] / H), 0.0f };
    const float r3[4] = { 0.0f, 0.0f, -1.0f, 0.0f };
    const float aL = 2.0f * ((float)x0 - M) / W - 1.0f, aR = 2.0f * ((float)x1 + M) / W - 1.0f;
    const float bT = 1.0f - 2.0f * ((float)y0 - M) / H, bB = 1.0f - 2.0f * ((float)y1 + M) / H;
    for (int k = 0; k < 4; k++) {
        g_geo3d_cull_plane[0][k] = r0[k] - aL * r3[k];
        g_geo3d_cull_plane[1][k] = aR * r3[k] - r0[k];
        g_geo3d_cull_plane[2][k] = bT * r3[k] - r1[k];
        g_geo3d_cull_plane[3][k] = r1[k] - bB * r3[k];
        g_geo3d_cull_plane[4][k] = r3[k];
    }
    for (int k = 0; k < 5; k++) {
        const float *q = g_geo3d_cull_plane[k];
        g_geo3d_cull_nlen[k] = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    }
    g_geo3d_cull_on = 1;
}

/* This frame's display list, decoded into g_dcf run by run, as
 * game_render_draw_geo_list does it. */
static void dp_decode(geo3d_state_t *geo, memory_bus_t *bus, const romset_t *rs) {
    const game_quirks_t *q = &g_active_profile->quirks;
    geo3d_tris_reset();
    geo3d_lines_reset();
    g_dcv_n = g_dcf_n = 0;
    g_dp_bus = bus;
    g_dp_rs_textures = rs->textures;
    dp_memo_reset();
    g_dp_pal_on = g_active_profile && g_active_profile->any_program;
    g_dp_pal_half ^= 1u;
    g_dp_pal_n = 0;
    g_dp_frame++;
    g_geo3d_mesh_epoch++;   /* the mesh cache keeps what this frame and the last drew */
    g_dp.runs = 0;
    g_dp.faces_dropped = 0;
    if (!g_geodl_snap_ready || !rs->main_data || !rs->polygons) { geo->captured_count = 0; return; }
    const uint32_t *snap = g_geodl_snap;
    g_geo_rs = geodl_raster_for(snap);
    bool walked = geo3d_scan_geo_list(geo, snap, BUFF_RAM_SIZE / 4, g_geodl_snap_rstart,
                                      (int16_t)mem_read16(bus, H_SYNC_BASE), (int16_t)mem_read16(bus, V_SYNC_BASE),
                                      rs->main_data, rs->main_data_size, q->model_table_offset, q->model_table_count);
    if (!walked) { geo->captured_count = 0; return; }
    g_geo3d_palram = bus->palette;
    g_geo3d_palram_size = PALETTE_SIZE;
    g_geo3d_flat_prev_z = 1.0e10f;
    g_geo3d_flat_list = 1;
    const int count = geo->captured_count;
    int windows = geo->geo_windows > 0 ? geo->geo_windows : 1;
    for (int i = 0; i < count; ) {
        const captured_model_t *c0 = &geo->captured[i];
        int j = i;
        while (j < count && geo->captured[j].window == c0->window &&
               !memcmp(geo->captured[j].gproj, c0->gproj, sizeof c0->gproj) &&
               !memcmp(geo->captured[j].vp, c0->vp, sizeof c0->vp)) j++;
        int x0 = c0->vp[0] < DC_VIEW_X ? DC_VIEW_X : c0->vp[0];
        int y0 = c0->vp[1] < DC_VIEW_Y ? DC_VIEW_Y : c0->vp[1];
        int x1 = c0->vp[2] > DC_VIEW_X + DC_VIEW_W ? DC_VIEW_X + DC_VIEW_W : c0->vp[2];
        int y1 = c0->vp[3] > DC_VIEW_Y + DC_VIEW_H ? DC_VIEW_Y + DC_VIEW_H : c0->vp[3];
        if (!(x1 > x0 && y1 > y0) || g_dp.runs >= DP_MAX_RUNS) { i = j; continue; }
        dp_cull_planes(c0->gproj, x0, y0, x1, y1);
        int run = (int)g_dp.runs++;
        g_dp_proj[run] = (dp_proj_t){ c0->gproj[0] * DC_SX, DC_X0 + c0->gproj[2] * DC_SX, -c0->gproj[1] * DC_SY, DC_Y0 + c0->gproj[3] * DC_SY,
                                      DC_X0 + (float)x0 * DC_SX, DC_Y0 + (float)y0 * DC_SY,
                                      DC_X0 + (float)x1 * DC_SX, DC_Y0 + (float)y1 * DC_SY };
        if (!x0 && !y0 && x1 == VIDEO_WIDTH && y1 == VIDEO_HEIGHT) {   /* the whole frame: the bars round it hide the rest */
            g_dp_proj[run].x0 = g_dp_proj[run].y0 = -1.0e30f;
            g_dp_proj[run].x1 = g_dp_proj[run].y1 = 1.0e30f;
        }
        int slice = windows - 1 - (int)c0->window;
        g_dp_cur = &g_dp_proj[run];
        g_dp_cur_run = run;
        g_dp_cur_slice = slice < 0 ? 0 : slice > 7 ? 7 : slice;
        for (int k = i; k < j; k++) {
            const captured_model_t *cm = &geo->captured[k];
            g_light_dir[0] = cm->light[0]; g_light_dir[1] = cm->light[1]; g_light_dir[2] = cm->light[2];
            g_geo3d_obj_tpa = cm->tpa;
            g_geo3d_obj_tha = cm->tha;
            g_geo3d_board_luma = 1;
            g_geo3d_mode = cm->geo_mode;
            g_geo3d_zadjust = cm->zadjust;
            g_geo3d_lod = cm->geo_lod;
            if (cm->direct_len) {
                geo3d_decode_direct(geo->direct_words + cm->direct_off, cm->direct_len,
                                    rs->textures, rs->textures_size, rs->main_data, rs->main_data_size,
                                    cm->gproj[0], cm->gproj[1]);
            } else {
                geo3d_models_t from = {
                    rs->main_data, rs->main_data_size, rs->polygons, rs->polygons_size,
                    rs->textures, rs->textures_size, q->model_table_offset, q->model_table_count,
                    q->mesh_ptr_subtract, q->mesh_ptr_add, NULL, 0 };
                if (cm->model_idx < 0) {
                    uint32_t word = cm->dbg_mesh_ptr & 0x7FFFu;
                    from.obj_mesh      = (const uint8_t *)&g_geo_rs->polyram[(cm->dbg_mesh_ptr & 0x01000000u) ? 1 : 0][word];
                    from.obj_mesh_size = (0x8000u - word) * 4u;
                }
                geo3d_decode_model_cached(&from, cm->model_idx, cm->matrix, cm->color[0], cm->color[1], cm->color[2]);
            }
            g_geo3d_obj_tpa = g_geo3d_obj_tha = 0xFFFFFFFFu;
            g_geo3d_board_luma = 0;
        }
        i = j;
    }
    g_geo3d_flat_list = 0;
    g_geo3d_cull_on = 0;
    g_geo3d_palram = NULL;
}

/* Far to near: slice, then key, descending; a tie goes to the later polygon
 * (the board's LEQUAL in submission order), so within a key the index runs
 * up. A radix sort on the words geo3d_dc_tri made: their low 13 bits are the
 * index itself, already in order, so a stable sort of bits 13-31 is the
 * whole sort: two passes (10 bits, then 9), both counts from one read. */
static void dp_sort(int n) {
    static unsigned lo[1025], hi[513];
    const uint32_t *a = g_dcf_key;
    uint32_t *b = g_dp_order[1], *o = g_dp_order[0];
    memset(lo, 0, sizeof lo); memset(hi, 0, sizeof hi);
    for (int t = 0; t < n; t++) { uint32_t k = a[t]; lo[((k >> 13) & 1023u) + 1]++; hi[(k >> 23) + 1]++; }
    for (int k = 0; k < 1024; k++) lo[k + 1] += lo[k];
    for (int k = 0; k < 512; k++) hi[k + 1] += hi[k];
    for (int t = 0; t < n; t++) { uint32_t k = a[t]; b[lo[(k >> 13) & 1023u]++] = k; }
    for (int t = 0; t < n; t++) { uint32_t k = b[t]; o[hi[k >> 23]++] = k & 0x1FFFu; }
}

typedef struct { float x, y, z, u, v; } dp_ev_t;   /* eye space, uv in [0, 1] units */

/* The colour and texture headers no texture holds: [list][0 colour, 1 checker]. */
static pvr_poly_hdr_t g_dp_hdr_plain[2][2];
static uint8_t        g_dp_hdr_plain_ok;

static void dp_hdr_compile(pvr_poly_hdr_t *hdr, pvr_list_t list, const dc_tex_t *tex, bool checker, unsigned fl,
                           unsigned pal) {
    pvr_poly_cxt_t cxt;
    if (tex) {
        int bank = pal ? (int)pal : (fl & GEO3D_FACE_TRANSPARENT) ? 1 : 0;
        /* A face with its own palette bank is a homebrew sprite (m2_sprite.h), drawn 1:1 on the
         * board. Bilinear blends its edge texels with what lies past the quad in the atlas and with
         * the hole's black: a dark border round every m2-sonic sprite (#473). */
        pvr_poly_cxt_txr(&cxt, list, PVR_TXRFMT_PAL4BPP | PVR_TXRFMT_4BPP_PAL(bank) | PVR_TXRFMT_TWIDDLED,
                         tex->w, tex->h, tex->ptr, pal && g_dp_pal_on ? PVR_FILTER_NONE : PVR_FILTER_BILINEAR);
        cxt.gen.specular = true;
        cxt.txr.uv_flip = (pvr_uv_flip_t)(((fl & GEO3D_FACE_MIRROR_X) ? PVR_UVFLIP_U : 0) |
                                          ((fl & GEO3D_FACE_MIRROR_Y) ? PVR_UVFLIP_V : 0));
    } else if (checker) {
        pvr_poly_cxt_txr(&cxt, list, PVR_TXRFMT_PAL4BPP | PVR_TXRFMT_4BPP_PAL(2) | PVR_TXRFMT_TWIDDLED,
                         8, 8, g_dp.checker, PVR_FILTER_NONE);
    } else {
        pvr_poly_cxt_col(&cxt, list);
    }
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = list == PVR_LIST_OP_POLY ? PVR_DEPTHCMP_ALWAYS : PVR_DEPTHCMP_GEQUAL;
    pvr_poly_compile(hdr, &cxt);
}

/* Corners i, j, k of record F as a triangle, near-clipped in eye space when
 * one is in front of the plane, and clipped to the run's window. */
static void dp_tri_out(const dcf_t *F, int i, int j, int k, bool checker) {
    const uint32_t base = F->base, off = F->off;
    const float ck = 0.125f / DC_SX, cky = 0.125f / DC_SY;   /* the checker's texel: two board pixels */
    const dcv_t *V0 = &g_dcv[F->v[i]], *V1 = &g_dcv[F->v[j]], *V2 = &g_dcv[F->v[k]];
    if (!(F->kind & DCF_CLIP)) {
        /* Nothing to clip, the usual case: three corners straight out. */
        if (checker) {   /* screen-space checker: one z, so the PVR maps it affinely */
            float z = fmaxf(fmaxf(V0->w, V1->w), V2->w);
            dp_vertex(PVR_CMD_VERTEX,     V0->sx, V0->sy, z, (V0->sx - DC_X0) * ck, (V0->sy - DC_Y0) * cky, base, off);
            dp_vertex(PVR_CMD_VERTEX,     V1->sx, V1->sy, z, (V1->sx - DC_X0) * ck, (V1->sy - DC_Y0) * cky, base, off);
            dp_vertex(PVR_CMD_VERTEX_EOL, V2->sx, V2->sy, z, (V2->sx - DC_X0) * ck, (V2->sy - DC_Y0) * cky, base, off);
        } else {
            dp_vertex(PVR_CMD_VERTEX,     V0->sx, V0->sy, V0->w, F->u[i], F->t[i], base, off);
            dp_vertex(PVR_CMD_VERTEX,     V1->sx, V1->sy, V1->w, F->u[j], F->t[j], base, off);
            dp_vertex(PVR_CMD_VERTEX_EOL, V2->sx, V2->sy, V2->w, F->u[k], F->t[k], base, off);
        }
        g_dp.tris++;
        return;
    }

    /* Near clip (Sutherland-Hodgman against z = -DC_NEAR): 3 corners in, 4 out at most. */
    const dp_proj_t *P = &g_dp_proj[F->run];
    dp_ev_t in[3] = { { V0->x, V0->y, V0->z, F->u[i], F->t[i] }, { V1->x, V1->y, V1->z, F->u[j], F->t[j] },
                      { V2->x, V2->y, V2->z, F->u[k], F->t[k] } };
    dp_ev_t out[4];
    int n = 0;
    for (int e = 0; e < 3; e++) {
        const dp_ev_t *a = &in[e], *b = &in[(e + 1) % 3];
        bool ain = a->z <= -DC_NEAR, bin = b->z <= -DC_NEAR;
        if (ain) out[n++] = *a;
        if (ain != bin) {
            float s = (-DC_NEAR - a->z) / (b->z - a->z);
            out[n++] = (dp_ev_t){ a->x + s * (b->x - a->x), a->y + s * (b->y - a->y), -DC_NEAR,
                                  a->u + s * (b->u - a->u), a->v + s * (b->v - a->v) };
        }
    }
    if (n < 3) return;
    /* Then the window's four sides, in screen space (#486): a face the board
     * clips to a small window, the white flash on the lab monitor's inset
     * picture, otherwise covered the whole frame. 1/z, u/z and v/z run
     * straight across the screen, so the cut corner's texture is in place. */
    typedef struct { float x, y, w, u, v; } dp_sv_t;   /* u, v times w */
    dp_sv_t pa[10], pb[10], *src = pa, *dst = pb;
    for (int k = 0; k < n; k++) {
        float iw = -1.0f / out[k].z;
        float x = P->bx + P->ax * out[k].x * iw, y = P->by + P->ay * out[k].y * iw;
        src[k] = (dp_sv_t){ x, y, iw, out[k].u * iw, out[k].v * iw };
    }
    for (int e = 0; e < 4 && n >= 3; e++) {
        const float lim = e == 0 ? P->x0 : e == 1 ? P->x1 : e == 2 ? P->y0 : P->y1;
        const float sg = e & 1 ? -1.0f : 1.0f;   /* inside: sg * (coordinate - lim) >= 0 */
        int m = 0;
        for (int k = 0; k < n; k++) {
            const dp_sv_t *a = &src[k], *b = &src[(k + 1) % n];
            float da = sg * ((e < 2 ? a->x : a->y) - lim), db = sg * ((e < 2 ? b->x : b->y) - lim);
            if (da >= 0.0f) dst[m++] = *a;
            if ((da >= 0.0f) != (db >= 0.0f)) {
                float t = da / (da - db);
                dst[m++] = (dp_sv_t){ a->x + t * (b->x - a->x), a->y + t * (b->y - a->y), a->w + t * (b->w - a->w),
                                      a->u + t * (b->u - a->u), a->v + t * (b->v - a->v) };
            }
        }
        dp_sv_t *sw = src; src = dst; dst = sw;
        n = m;
    }
    if (n < 3) return;
    float zc = 0.0f;
    if (checker) for (int k = 0; k < n; k++) zc = fmaxf(zc, src[k].w);
    /* A convex polygon as a strip: 0, 1, n-1, 2, n-2, ... */
    for (int k = 0, lo = 1, hi = n - 1; k < n; k++) {
        int v = k == 0 ? 0 : (k & 1) ? lo++ : hi--;
        const dp_sv_t *q = &src[v];
        if (checker)   /* the checker's texels lie on the screen, not the face */
            dp_vertex(k == n - 1 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, q->x, q->y, zc,
                      (q->x - DC_X0) * ck, (q->y - DC_Y0) * cky, base, off);
        else
            dp_vertex(k == n - 1 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, q->x, q->y, q->w,
                      q->u / q->w, q->v / q->w, base, off);
    }
    g_dp.tris++;
}

/* One face: its header if it differs from the last face's, then its corners,
 * near-clipped in eye space when one is in front of the plane. */
static void dp_face(int t, pvr_list_t list, uint32_t *last_state) {
    const dcf_t *F = &g_dcf[t];
    const dc_tex_t *tex = F->tex;
    const bool checker = (F->kind & DCF_CHECKER) != 0;
    uint32_t state = tex ? (uint32_t)(uintptr_t)tex->ptr ^ F->var ^ (uint32_t)F->pal << 24 : checker ? 2u : 4u;
    if (state != *last_state) {
        /* compiled once per texture and variant (or per list, untextured) */
        *last_state = state;
        const int li = list == PVR_LIST_OP_POLY ? 0 : 1;
        if (tex && F->pal && g_dp_pal_on) {   /* a bank of its own: compiled each time (homebrew sprites, a few) */
            pvr_poly_hdr_t h;
            dp_hdr_compile(&h, list, F->tex, false, F->var >> 1, F->pal);
            dp_hdr(&h);
        } else if (tex) {
            uint8_t want = (uint8_t)(0x80u | li << 6 | F->var);
            dc_tex_t *mt = F->tex;
            if (mt->hdr_var != want) {
                dp_hdr_compile(&mt->hdr, list, mt, false, F->var >> 1, 0);
                mt->hdr_var = want;
            }
            if (F->pal) {   /* a knee or pool bank: the same header with its palette bits (mode3 26:21) */
                pvr_poly_hdr_t h = mt->hdr;
                h.mode3 = (h.mode3 & ~PVR_TXRFMT_4BPP_PAL(0x3Fu)) | PVR_TXRFMT_4BPP_PAL((uint32_t)F->pal);
                dp_hdr(&h);
            } else {
                dp_hdr(&mt->hdr);
            }
        } else {
            if (!g_dp_hdr_plain_ok) {
                for (int l = 0; l < 2; l++)
                    for (int c = 0; c < 2; c++)
                        dp_hdr_compile(&g_dp_hdr_plain[l][c], l ? PVR_LIST_TR_POLY : PVR_LIST_OP_POLY, NULL, c != 0, 0, 0);
                g_dp_hdr_plain_ok = 1;
            }
            dp_hdr(&g_dp_hdr_plain[li][checker ? 1 : 0]);
        }
    }

    if ((F->kind & (DCF_QUAD | DCF_CLIP | DCF_CHECKER)) == DCF_QUAD) {
        /* a packed quad, nothing to clip: one strip, its corners as dp_strip_put ordered them */
        const uint32_t base = F->base, off = F->off;
        for (int k = 0; k < 4; k++) {
            const dcv_t *V = &g_dcv[F->v[k]];
            dp_vertex(k == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, V->sx, V->sy, V->w, F->u[k], F->t[k], base, off);
        }
        g_dp.tris += 2;
        return;
    }
    dp_tri_out(F, 0, 1, 2, checker);
    if (F->kind & DCF_QUAD) dp_tri_out(F, 1, 3, 2, checker);   /* the strip's second triangle */
}

/* One frame, if the PVR is ready for it; false if it was dropped. */
static bool dp_frame(geo3d_state_t *geo, memory_bus_t *bus, const romset_t *rs, tile_cpu_t *tiles) {
    if (pvr_check_ready() != 0) return false;
    uint64_t t0 = timer_us_gettime64();
    dp_tiles(bus, tiles);
    dp_tex_invalidate(bus);
    uint64_t ta = timer_us_gettime64();
    dp_decode(geo, bus, rs);
    uint64_t tb = timer_us_gettime64();
    int n = g_dcf_n;
    dp_sort(n);
    uint64_t t1 = timer_us_gettime64();
    g_dp.us_tiles += (uint32_t)(ta - t0);
    g_dp.us_scan  += (uint32_t)(tb - ta);
    g_dp.us_sort  += (uint32_t)(t1 - tb);
    g_dp.tt_tiles += ta - t0; g_dp.tt_scan += tb - ta; g_dp.tt_sort += t1 - tb;
    g_dp.tris = 0;

    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    if (g_ls.on)
        dp_ls_strips(&g_ls.t2, PVR_LIST_OP_POLY, g_ls.t2.back, PVR_TXRFMT_RGB565, true, 1.0e-4f);
    else
        dp_layer(PVR_LIST_OP_POLY, g_dp.lbg[g_dp.lcur], PVR_TXRFMT_RGB565, true, 1.0e-4f);
    uint32_t state = 0;
    for (int k = 0; k < n; k++) {
        int t = (int)g_dp_order[0][k];
        if (!(g_dcf[t].kind & DCF_TRANS)) dp_face(t, PVR_LIST_OP_POLY, &state);
    }
    pvr_list_finish();
    pvr_list_begin(PVR_LIST_TR_POLY);
    if (g_ls.both)   /* tilemap 0 behind the 3D: over the strips only */
        dp_ls_strips(&g_ls.t0, PVR_LIST_TR_POLY, g_ls.t0.back, PVR_TXRFMT_ARGB1555, false, 1.0e-4f);
    else if (g_ls.on)   /* tilemaps 1 and 0 */
        dp_layer(PVR_LIST_TR_POLY, g_dp.lbg[g_dp.lcur], PVR_TXRFMT_ARGB1555, false, 1.0e-4f);
    state = 0;
    for (int k = 0; k < n; k++) {
        int t = (int)g_dp_order[0][k];
        if (g_dcf[t].kind & DCF_TRANS) dp_face(t, PVR_LIST_TR_POLY, &state);
    }
    if (g_ls.on && g_ls.t2.front_cells)
        dp_ls_strips(&g_ls.t2, PVR_LIST_TR_POLY, g_ls.t2.front, PVR_TXRFMT_ARGB1555, true, 1.0e3f);
    if (!g_ls.both)
        dp_layer(PVR_LIST_TR_POLY, g_dp.lfg[g_dp.lcur], PVR_TXRFMT_ARGB1555, true, 1.0e3f);
    else if (g_ls.t0.front_cells)
        dp_ls_strips(&g_ls.t0, PVR_LIST_TR_POLY, g_ls.t0.front, PVR_TXRFMT_ARGB1555, true, 1.0e3f);
    if (DC_VX0 > 0.0f) {
        dp_bar(0.0f, 0.0f, DC_VX0, DC_SCR_H);
        dp_bar(DC_VX1, 0.0f, DC_SCR_W, DC_SCR_H);
    }
    if (DC_VY0 > 0.0f) {
        dp_bar(DC_VX0, 0.0f, DC_VX1, DC_VY0);
        dp_bar(DC_VX0, DC_VY1, DC_VX1, DC_SCR_H);
    }
    for (int r = 0; r < DC_TEXT_ROWS; r++)
        if (g_dp.text_rows & ~g_dp.text_hide & (1u << r)) dp_text_rect(r);
    dp_corner_rect();
    pvr_list_finish();
    pvr_scene_finish();
    uint64_t t2 = timer_us_gettime64();
    g_dp.us_decode += (uint32_t)(t1 - t0);
    g_dp.us_submit += (uint32_t)(t2 - t1);
    g_dp.tt_submit += t2 - t1;
    return true;
}

/* Text only (no disc, or the board halted). */
static void dp_text_frame(void) {
    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_TR_POLY);
    for (int r = 0; r < DC_TEXT_ROWS; r++)
        if (g_dp.text_rows & ~g_dp.text_hide & (1u << r)) dp_text_rect(r);
    dp_corner_rect();
    pvr_list_finish();
    pvr_scene_finish();
}

#endif /* DC_PVR_H */
