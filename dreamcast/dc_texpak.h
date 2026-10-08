/*
 * dc_texpak.h -- TEXTURES.PAK, the PVR textures STF's faces cut, made offline
 * (Pinboard #502).
 *
 * The draw cuts each texture a face names (dp_tex_get's key: sheet, corner,
 * log2 size) out of the board's texture RAM, twiddles it as the PVR's 4bpp
 * paletted format and loads it, the first time a face needs it after the
 * game wrote those rows. The pack holds those textures already cut: the same
 * bytes pvr_txr_load takes, so a hit is a disc read and a store-queue copy to
 * video memory instead of a texel-by-texel cut.
 *
 * Texture RAM is filled by the game's loader at run time, so a key alone does
 * not name a texture: the same corner holds another stage's bark next fight.
 * Each entry is keyed by (key, dct_src_hash): the hash of the texture-RAM
 * words the cut reads, taken at run time before the lookup. A texture the
 * pack does not hold, or whose words differ, is cut as before; a pack can
 * show a wrong texture only on a 32-bit hash collision at the same key.
 *
 * Only textures up to 256 on a side are packed (dp_tex_get loads a bigger one
 * 32 KB at a time; it is cut as before).
 *
 * File: dct_head_t; the index, dct_index_t sorted by (key, hash); at data_off
 * (a sector) the textures, each 32-byte aligned, in the order the recorded
 * frames first drew them, so one scene's textures lie together on the disc.
 * Recorded with det_digest --tex-groups (#508), they lie by object group
 * instead (sfight.mdlgroups: a stage, a fighter, common...), as STRIPS.PAK's
 * meshes do: each group from a sector, the groups in the order of the first
 * frame that draws from them, within one by frame. A texture is in the group
 * of the model that first drew it. That took more window reads than the
 * frame order (DREAMCAST-PORT.md), so the disc's pack is recorded without
 * it. At models_off, past the textures, each
 * index entry's model (uint16_t, 0xFFFF: not known), for det_digest alone:
 * the Dreamcast reads the file only up to data_off and the textures it finds.
 * ROM-derived (it is the game's decompressed texture data), so recorded by
 * det_digest --tex-pack from the ROM files and never committed.
 *
 * Host-clean: det_digest includes it too.
 */
#ifndef DC_TEXPAK_H
#define DC_TEXPAK_H

#include <stdint.h>
#include <string.h>

typedef struct {
    char     magic[4];                  /* "M2TX" */
    uint32_t n, data_off;               /* index entries; the textures' offset, a sector multiple */
    uint32_t bytes;                     /* the textures' bytes, for the log */
    uint32_t models_off;                /* the entries' models, or 0 */
    uint32_t pad[3];
} dct_head_t;

typedef struct {
    uint32_t key, hash;                 /* dp_tex_get's key; dct_src_hash of the words it was cut from */
    uint32_t off;                       /* from data_off; the length is dct_bytes(key) */
    uint32_t frame;                     /* the recorded frame that first drew it */
} dct_index_t;

_Static_assert(sizeof(dct_head_t) == 32 && sizeof(dct_index_t) == 16, "TEXTURES.PAK records");

static inline int dct_cmp(uint32_t ka, uint32_t ha, uint32_t kb, uint32_t hb) {
    if (ka != kb) return ka < kb ? -1 : 1;
    if (ha != hb) return ha < hb ? -1 : 1;
    return 0;
}

/* The key's fields (dcs_tex_key): sheet, corner, the tile's size, and the
 * PVR texture's (at least 8 a side). */
#define DCT_SHEET(k) (((k) >> 29) & 1u)
#define DCT_X0(k)    (((k) >> 18) & 2047u)
#define DCT_Y0(k)    (((k) >> 8) & 1023u)
#define DCT_TW(k)    (1u << (((k) >> 4) & 15u))
#define DCT_TH(k)    (1u << ((k) & 15u))
static inline uint32_t dct_side(uint32_t t) { return t < 8 ? 8 : t; }
static inline uint32_t dct_bytes(uint32_t key) { return dct_side(DCT_TW(key)) * dct_side(DCT_TH(key)) / 2; }
static inline int dct_packable(uint32_t key) { return key && DCT_TW(key) <= 256 && DCT_TH(key) <= 256; }

/* Texel (x, y) of a sheet: the layout game_render_upload_atlas decodes. */
static inline unsigned dct_texel(const uint32_t *sheet, unsigned x, unsigned y) {
    uint32_t q = (y >> 1) + (x >= 1024 ? 512u : 0u);
    uint32_t word = sheet[q * 256u + ((x & 1023u) >> 2)] >> (((x >> 1) & 1u) * 16u);
    unsigned sh = (y & 1u) ? ((x & 1u) ? 0 : 4) : ((x & 1u) ? 8 : 12);
    return (word >> sh) & 15u;
}

/* FNV-1a over the sheet words the key's tile reads (a word is 4 texels across
 * and 2 down; the words at its edges hold some of the neighbours' too). */
static inline uint32_t dct_src_hash(const uint32_t *sheet, uint32_t key) {
    const unsigned x0 = DCT_X0(key), y0 = DCT_Y0(key), tw = DCT_TW(key), th = DCT_TH(key);
    uint32_t h = 2166136261u ^ key;
    for (unsigned y = y0 & ~1u; y < y0 + th; y += 2) {
        const uint32_t *row = sheet + ((y & 1023u) >> 1) * 256u;
        for (unsigned x = x0 & ~3u; x < x0 + tw; x += 4) {
            unsigned xx = x & 2047u;
            h = (h ^ row[(xx >= 1024 ? 512u * 256u : 0u) + ((xx & 1023u) >> 2)]) * 16777619u;
        }
    }
    return h;
}

/* i's bits at the even positions, for i < 1024 (dct_init). */
static uint16_t g_dct_spread[1024];
static inline void dct_init(void) {
    for (unsigned i = 0; i < 1024; i++) {
        unsigned s = 0;
        for (int b = 0; b < 10; b++) s |= ((i >> b) & 1u) << (2 * b);
        g_dct_spread[i] = (uint16_t)s;
    }
}
/* The even bits of v, packed: the inverse of g_dct_spread. */
static inline unsigned dct_compact(uint32_t v) {
    unsigned c = 0;
    for (int b = 0; b < 10; b++) c |= ((v >> (2 * b)) & 1u) << b;
    return c;
}

/* Bytes at .. at + chunk of the key's PVR texture (dct_bytes long), cut from
 * the sheet into out. Twiddled 4bpp: square blocks of the shorter side,
 * Morton order with v the low bit, laid one after another along the longer
 * side. A texture over 256x256 is cut 32 KB at a time (chunk 32768): each
 * 32 KB of it is one rectangle of texels, 256x256 inside a bigger block, or
 * whole blocks of a smaller one. */
static void dct_cut(const uint32_t *sheet, uint32_t key, uint32_t at, uint32_t chunk, uint8_t *out) {
    const unsigned x0 = DCT_X0(key), y0 = DCT_Y0(key), tw = DCT_TW(key), th = DCT_TH(key);
    const unsigned W = dct_side(tw), H = dct_side(th), m = W < H ? W : H;
    unsigned lm = 0;
    while ((1u << lm) < m) lm++;
    const uint32_t i0 = at * 2, r = i0 & ((1u << (2 * lm)) - 1u), blk = i0 >> (2 * lm);
    unsigned rx = dct_compact(r >> 1), ry = dct_compact(r), rw, rh;
    if (m > 256) { rw = rh = 256; }
    else if (W > H) { rw = chunk * 2 / m; rh = m; }
    else { rw = m; rh = chunk * 2 / m; }
    if (W > H) rx += blk * m; else ry += blk * m;
    memset(out, 0, chunk);
    for (unsigned y = ry; y < ry + rh; y++)
        for (unsigned x = rx; x < rx + rw; x++) {
            unsigned c = dct_texel(sheet, (x0 + (x & (tw - 1))) & 2047u, (y0 + (y & (th - 1))) & 1023u);
            unsigned bk = W > H ? x >> lm : y >> lm;
            unsigned i = ((bk << (2 * lm)) | g_dct_spread[y & (m - 1)] | (unsigned)g_dct_spread[x & (m - 1)] << 1) - i0;
            out[i >> 1] |= (uint8_t)(c << ((i & 1) * 4));
        }
}

#endif /* DC_TEXPAK_H */
