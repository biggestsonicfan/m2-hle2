/* The tile generators' requests, counted (make TILECOUNT=1, Pinboard #571).
 * Off, nothing here is compiled.
 *
 * Every cell a generator draws is put in one of three groups against the last
 * time the same cell was drawn:
 *   0  the request is the same, every field the generator reads;
 *   1  the char id changed, but the char's source bytes are the same and so is
 *      everything else;
 *   2  new.
 * Two generators:
 *   - dp_ls_cell, one line-scroll tilemap cell (tilemap 2, and 0 under .both).
 *     It reads the entry (char id, palette bank, category), `opaque`, the
 *     bank's 16 colours in both formats and the char's 32 bytes. No flip: the
 *     board has none. The scroll is the strips' and is not drawn.
 *   - the CPU layers (dp_ls_rest / tile_cpu_draw), drawn by 8x8 screen block
 *     over each row's span. A block's request is, per tilemap drawn, its 9-bit
 *     V scroll, and per line, at its first and last pixel: the column in the
 *     cell (the 9-bit H scroll), the tilemap the split picks, whether the mask
 *     hides the pixel, and the entry sampled there.
 *     A block draws pens, so colours are not part of it; the source is each
 *     sampled cell's 4 bytes of the line.
 * The request and the source are each held as an FNV hash: with the char ids
 * in it (req), and with the char ids replaced by their bytes (src). Group 0 is
 * both the same, group 1 src the same and req not, group 2 src different. One
 * total per HUD window (dc_tc_row), or, with DC_TC_TO set (EXTRA=
 * "-DDC_TC_FROM=a -DDC_TC_TO=b"), one total over board frames a..b only, kept
 * on the HUD after b; every draw still updates the last request. */
#ifndef DC_TILECOUNT_H
#define DC_TILECOUNT_H

#ifndef DC_TILE_COUNT
#define DC_TILE_COUNT 0
#endif

#ifndef DC_TC_TO
#define DC_TC_FROM 0u
#define DC_TC_TO   0u
#endif

#if DC_TILE_COUNT
typedef struct { uint32_t req, src; } dc_tc_key_t;

static struct {
    dc_tc_key_t ls[2][0x1000];                   /* tilemap 2's cells, tilemap 0's */
    dc_tc_key_t blk[TILE_BLK_H][TILE_BLK_W];
    uint32_t    n[2][3];                         /* [ls, cpu][group] this window */
    uint32_t    draws;                           /* CPU redraws counted */
} g_tc;

/* Whether this draw is counted: always, or inside board frames DC_TC_FROM..TO. */
static inline bool dc_tc_on(void) {
    return !DC_TC_TO || (g_emu_frames >= DC_TC_FROM && g_emu_frames <= DC_TC_TO);
}

static inline uint32_t dc_tc_mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

static inline void dc_tc_tally(int gen, dc_tc_key_t *last, dc_tc_key_t now) {
    int g = now.src != last->src ? 2 : now.req != last->req ? 1 : 0;
    if (dc_tc_on()) g_tc.n[gen][g]++;
    *last = now;
}

static void dc_tc_ls(int which, const uint8_t *gfx, int i, uint16_t e, bool opaque,
                     const uint16_t *p565, const uint16_t *p1555) {
    int bank16 = ((e >> 7) & 0xFF) * 16;
    dc_tc_key_t k = { 2166136261u, 2166136261u };
    k.req = dc_tc_mix(dc_tc_mix(k.req, e), opaque);
    k.src = dc_tc_mix(dc_tc_mix(k.src, e & ~0x3FFFu), opaque);
    for (int p = 0; p < 16; p++) {
        uint32_t c = p565[bank16 + p] | (uint32_t)p1555[bank16 + p] << 16;
        k.req = dc_tc_mix(k.req, c);
        k.src = dc_tc_mix(k.src, c);
    }
    const uint8_t *g = gfx + (uint32_t)(e & 0x3FFF) * 32u;
    for (int b = 0; b < 32; b++) k.src = dc_tc_mix(k.src, g[b]);
    dc_tc_tally(0, &g_tc.ls[which][i], k);
}

/* Tilemap t's part of block (bx, by)'s key: what s24_draw_tilemap and
 * s24_draw_line read, and nothing they skip. A tilemap the blit returns from
 * adds nothing; per line, the 9-bit scroll, and per sampled pixel the tilemap
 * the split picks, whether the mask hides it, and that cell's entry. */
static void dc_tc_blk_layer(dc_tc_key_t *k, const uint16_t *w, const uint8_t *gfx, int t, int bx, int by) {
    uint16_t hscr = w[0x5000 + t], vscr = w[0x5004 + t], ctrl = w[0x5004 + (t & 2)];
    int mode = (ctrl & 0x6000) >> 13;
    if ((vscr & 0x8000) || (mode && (t & 1))) return;
    const uint16_t *maskw = w + ((t & 2) ? 0x6800 : 0x6000);
    int vy = vscr & 0x1FF;
    k->req = dc_tc_mix(dc_tc_mix(k->req, t), vy);
    k->src = dc_tc_mix(dc_tc_mix(k->src, t), vy);
    for (int y = by * 8; y < by * 8 + 8; y++) {
        uint16_t row = (hscr & 0x8000) ? w[0x4000 + 0x200 * t + y] : hscr;
        int h = row & 0x1FF, ty = (y + vy) & 511;
        s24_line_t ln;
        s24_line_split(&ln, maskw, t, mode, vscr, row, h, y);
        for (int x = bx * 8; x < bx * 8 + 8; x += 7) {
            int l = x < ln.split_x ? ln.split_l : ln.split_l ^ 1;
            bool hid = !mode && (ln.mask[x >> 7] & (0x8000 >> ((x & 127) >> 3)));
            int tx = (x - h) & 511;
            uint16_t e = hid ? 0 : w[0x1000 * l + (ty >> 3) * 64 + (tx >> 3)];
            const uint8_t *g = gfx + (uint32_t)(e & 0x3FFF) * 32u + (uint32_t)(ty & 7) * 4u;
            uint32_t pos = (uint32_t)(tx & 7) | (uint32_t)l << 3 | (uint32_t)hid << 5;
            k->req = dc_tc_mix(dc_tc_mix(k->req, pos), e);
            k->src = dc_tc_mix(dc_tc_mix(dc_tc_mix(k->src, pos), e & ~0x3FFFu),
                               hid ? 0u : (uint32_t)g[0] | g[1] << 8 | g[2] << 16 | (uint32_t)g[3] << 24);
        }
    }
}

/* The blocks the CPU layers draw this time: each row's span [x0, x1), by block. */
static void dc_tc_cpu(const uint16_t *w, const uint8_t *gfx, const int16_t *x0, const int16_t *x1, bool ls) {
    if (dc_tc_on()) g_tc.draws++;
    for (int by = 0; by < TILE_BLK_H; by++) {
        int a = x0[by * 8], b = x1[by * 8];
        for (int bx = a >> 3; bx < (b + 7) >> 3 && a < b; bx++) {
            dc_tc_key_t k = { 2166136261u, 2166136261u };
            for (int t = ls ? 1 : 3; t >= 0; t--) dc_tc_blk_layer(&k, w, gfx, t, bx, by);
            dc_tc_tally(1, &g_tc.blk[by][bx], k);
        }
    }
}

/* The totals as one HUD row; per window, the next one starts at zero. */
static void dc_tc_row(char *line, size_t n) {
    snprintf(line, n, "TC%s ls %lu/%lu/%lu cpu %lu/%lu/%lu d%lu", DC_TC_TO ? " f" : "",
             (unsigned long)g_tc.n[0][0], (unsigned long)g_tc.n[0][1], (unsigned long)g_tc.n[0][2],
             (unsigned long)g_tc.n[1][0], (unsigned long)g_tc.n[1][1], (unsigned long)g_tc.n[1][2],
             (unsigned long)g_tc.draws);
    if (!DC_TC_TO) { memset(g_tc.n, 0, sizeof g_tc.n); g_tc.draws = 0; }
}

#define DC_TC_LS(ls, gfx, i, e, opaque) \
    dc_tc_ls((ls) == &g_ls.t0, gfx, i, e, opaque, g_dp.pen565, g_dp.pen1555)
#define DC_TC_CPU(w, gfx, x0, x1, ls) dc_tc_cpu(w, gfx, x0, x1, ls)
#else
#define DC_TC_LS(ls, gfx, i, e, opaque) ((void)0)
#define DC_TC_CPU(w, gfx, x0, x1, ls)   ((void)0)
#endif

#endif
