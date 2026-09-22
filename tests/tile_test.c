/*
 * tile_test.c — the tile compositor against its own previous self.
 *
 * s24_draw_tilemap was rewritten for speed (per-line mask words, one cell
 * decode per eight pixels, palette reads only for pixels written). The
 * compositor it replaced is kept here verbatim as the reference, and both are
 * run over random tile RAM, cell graphics, palette, scroll registers, per-line
 * scroll tables, window masks and every pair control mode, with the four
 * layer buffers compared byte for byte. The pen table the CPU compose uses
 * (video_pen_table, built from 96 colour-table bytes) is held to
 * tile_pen_lut, which derives every entry on its own.
 *
 * No ROM, no window: pure functions of the bus.
 */
#define NDEBUG 1
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../src/board/memory.h"
#include "../src/board/tile_renderer.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
} while (0)

/* ---- the previous compositor, verbatim ------------------------------------ */

static inline uint16_t ref_s24_sample(const memory_bus_t *bus, int t, int x, int y,
                                  uint8_t *ci, uint8_t *cat, int *pen) {
    x &= 511; y &= 511;
    uint16_t entry = tileram_word(bus, 0x1000u * (uint32_t)t + (uint32_t)((y >> 3) * 64 + (x >> 3)));
    int bank = (entry >> 7) & 0xFF;
    *ci  = tile_pixel_4bpp(bus, entry & 0x3FFF, x & 7, y & 7);
    *cat = (uint8_t)((entry >> 15) & 1);
    *pen = bank * 16 + *ci;
    return pal_read16(bus, *pen);
}
static inline void ref_s24_draw_tilemap(const memory_bus_t *bus, int t, int cat, bool opaque,
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
            uint16_t color = ref_s24_sample(bus, l, x - h, y + vy, &ci, &pc, &pen);
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

static void ref_render_bg_layer(const memory_bus_t *bus, tile_layers_t *t) {
    int n = VIDEO_WIDTH * VIDEO_HEIGHT;
    uint16_t backc = back_color_555(bus);
    for (int i = 0; i < n; i++) { t->bg[i] = backc; t->bg_alpha[i] = 0; }
    ref_s24_draw_tilemap(bus, 3, 0, true,  t->bg, t->bg_alpha);
    ref_s24_draw_tilemap(bus, 2, 0, true,  t->bg, t->bg_alpha);
    ref_s24_draw_tilemap(bus, 1, 0, false, t->bg, t->bg_alpha);
    ref_s24_draw_tilemap(bus, 0, 0, false, t->bg, t->bg_alpha);
}

static void ref_render_fg_layer(const memory_bus_t *bus, tile_layers_t *t) {
    int n = VIDEO_WIDTH * VIDEO_HEIGHT;
    memset(t->fg, 0, (size_t)n * sizeof t->fg[0]);
    memset(t->alpha, 0, (size_t)n);
    for (int k = 3; k >= 0; k--) ref_s24_draw_tilemap(bus, k, 1, false, t->fg, t->alpha);
}

/* ---- random board state ----------------------------------------------------- */

static uint32_t s_rng = 1;
static uint32_t rnd(void) { s_rng = s_rng * 1664525u + 1013904223u; return s_rng >> 8; }
static void fill(uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)rnd(); }

static void w16(memory_bus_t *bus, uint32_t word, uint16_t v) {
    bus->tile[word * 2] = (uint8_t)v; bus->tile[word * 2 + 1] = (uint8_t)(v >> 8);
}

/* One scenario: every register drawn at random, biased so each feature is
 * exercised often -- per-line H scroll, a disabled V scroll, each pair mode. */
static void scenario(memory_bus_t *bus, unsigned seed) {
    s_rng = seed * 2654435761u + 1;
    fill(bus->tile, TILE_SIZE);
    fill(bus->tmapgfx, TMAPGFX_SIZE);
    /* every third scenario: mostly blank cells, as a HUD layer is */
    if (seed % 3 == 0)
        for (uint32_t k = 0; k < TMAPGFX_SIZE; k += 32) if (rnd() % 10) memset(bus->tmapgfx + k, 0, 32);
    fill(bus->palette, PALETTE_SIZE);
    /* cells: sometimes few distinct ones, so runs of equal cells occur as in a game */
    if (rnd() & 1)
        for (uint32_t w = 0; w < 0x4000; w++) w16(bus, w, (uint16_t)(rnd() & (rnd() & 4 ? 0xFFFF : 0x80FF)));
    for (int t = 0; t < 4; t++) {
        uint16_t h = (uint16_t)(rnd() & 0x1FF), v = (uint16_t)(rnd() & 0x1FF);
        if ((rnd() & 3) == 0) h |= 0x8000;               /* per-line H scroll table */
        if ((rnd() & 7) == 0) v |= 0x8000;               /* tilemap off */
        if (rnd() & 1) v = (uint16_t)(v | (rnd() & 0x3FF));
        w16(bus, 0x5000u + (uint32_t)t, h);
        w16(bus, 0x5004u + (uint32_t)t, v);
    }
    /* pair controls share the V scroll words of tilemaps 0 and 2 (bits 14:13) */
    for (int p = 0; p < 2; p++) {
        uint32_t w = 0x5004u + 2u * (uint32_t)p;
        uint16_t v = (uint16_t)(bus->tile[w * 2] | (bus->tile[w * 2 + 1] << 8));
        v = (uint16_t)((v & ~0x6000u) | ((rnd() & 3) << 13));
        w16(bus, w, v);
    }
}

int main(int argc, char **argv) {
    static memory_bus_t bus;
    static tile_layers_t ours, ref;
    static uint8_t lut[0x8000][3];
    if (!mem_init(&bus, NULL, 0) || !tile_layers_init(&ours) || !tile_layers_init(&ref)) {
        printf("FAIL: init\n"); return 1;
    }
    size_t n = (size_t)VIDEO_WIDTH * VIDEO_HEIGHT;
    int modes_seen[4] = { 0, 0, 0, 0 };
    for (unsigned seed = 1; seed <= 48; seed++) {
        scenario(&bus, seed);
        for (int p = 0; p < 2; p++)
            modes_seen[(tileram_word(&bus, 0x5004u + 2u * (uint32_t)p) & 0x6000) >> 13]++;
        render_bg_layer(&bus, &ours);     ref_render_bg_layer(&bus, &ref);
        render_fg_layer(&bus, &ours);     ref_render_fg_layer(&bus, &ref);
        CHECK(memcmp(ours.bg, ref.bg, n * 2) == 0,         "seed %u: bg colours differ", seed);
        CHECK(memcmp(ours.bg_alpha, ref.bg_alpha, n) == 0, "seed %u: bg alpha differs", seed);
        CHECK(memcmp(ours.fg, ref.fg, n * 2) == 0,         "seed %u: fg colours differ", seed);
        CHECK(memcmp(ours.alpha, ref.alpha, n) == 0,       "seed %u: fg alpha differs", seed);

        /* the pen table: from the colour tables as loaded, and as the blank
         * tables of a game that never fills them */
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 0) fill(bus.colorxlat, COLORXLAT_SIZE);
            else           memset(bus.colorxlat, 0, COLORXLAT_SIZE);
            tile_pen_lut(&bus, lut);
            video_pen_table(&bus);
            int bad = 0;
            for (int c = 0; c < 0x8000; c++)
                if (g_video_pen[c][0] != lut[c][0] || g_video_pen[c][1] != lut[c][1] ||
                    g_video_pen[c][2] != lut[c][2] || g_video_pen[c][3] != 255) bad++;
            CHECK(bad == 0, "seed %u pass %d: %d pen entries differ from tile_pen_lut", seed, pass, bad);
        }
    }
    CHECK(modes_seen[0] && modes_seen[1] && modes_seen[2] && modes_seen[3],
          "every pair control mode was drawn (%d %d %d %d)", modes_seen[0], modes_seen[1], modes_seen[2], modes_seen[3]);
    printf("%s: 48 scenarios, modes %d/%d/%d/%d\n", g_fail ? "FAILED" : "ok",
           modes_seen[0], modes_seen[1], modes_seen[2], modes_seen[3]);

    /* --bench: both compositors over a game-like frame (mode 0 pairs, window
     * masks, whole layers), the cost of one compose each. Not a check. */
    if (argc > 1 && strcmp(argv[1], "--bench") == 0) {
        scenario(&bus, 7);
        for (int p = 0; p < 2; p++) {
            uint32_t w = 0x5004u + 2u * (uint32_t)p;
            w16(&bus, w, (uint16_t)(bus.tile[w * 2] | (bus.tile[w * 2 + 1] << 8)) & 0x9FFF);
        }
        for (int which = 0; which < 2; which++) {
            clock_t t0 = clock();
            for (int k = 0; k < 40; k++) {
                if (which) { render_bg_layer(&bus, &ours);     render_fg_layer(&bus, &ours); }
                else       { ref_render_bg_layer(&bus, &ref);  ref_render_fg_layer(&bus, &ref); }
            }
            printf("%s compose: %.0f us\n", which ? "new" : "old", (clock() - t0) * 1e6 / CLOCKS_PER_SEC / 40);
        }
    }
    return g_fail ? 1 : 0;
}
