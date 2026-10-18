/*
 * dc_layout.h -- where the board's ROM regions come from on the disc
 * (Pinboard #342). The disc holds the PS3 release's ROM files as they ship
 * (StF - PS3/stf_rom: rom_code1, rom_data, rom_ep, rom_pol, rom_tex), each one
 * straight and already de-interleaved, and dc_pager.h maps a region's pages
 * onto their sectors. No pack, no zip.
 *
 * Checked against MAME's sfight set (ROM_LOAD32_WORD pairs):
 *   ROM_CODE1  maincpu 0-1 MB          (epr-19001/19002; the region is 2 MB)
 *   ROM_DATA   main_data 0-16 MB       (mpr-19007/08, mpr-19005/06)
 *   ROM_EP     main_data 16-17 MB      (epr-19003/04), its first 920048 bytes:
 *              MAME's last 128,528 are all 0xFF. Mirrored to 31 MB, as MAME.
 *   ROM_POL    polygons 0-16 MB        (mpr-19009/12, mpr-19010/13)
 *   ROM_TEX    textures 0-4 MB and 8-12 MB (mpr-19019/17, mpr-19020/18 low
 *              halves). Not in it: 7-8 MB (the UV streams of models
 *              4319-5102) and 12-16 MB (texel data the emulator never reads).
 * Not shipped at all: the copro tables (sharc_sincos falls back to libm), the
 * sound program and its samples (the sound is ADX, see DREAMCAST-PORT.md).
 */
#ifndef DC_LAYOUT_H
#define DC_LAYOUT_H

#include <stdint.h>

#define DC_SECTOR     2048u            /* a mode 1 sector */
#define DC_REGIONS    7                /* the romset_t buffers, in its order */
#define DC_MAX_SEGS   4

typedef struct {
    const char *file;                  /* ISO 9660 name on the data track */
    uint32_t    file_off, len;         /* the bytes taken from it (file_off page-aligned) */
    uint32_t    reg_off;               /* where they go in the region (page-aligned) */
    uint32_t    period, count;         /* repeated count times, period apart (0: once) */
} dc_seg_t;

typedef struct {
    const char *name;                  /* maincpu, main_data, copro, polygons, ... */
    uint32_t    size;                  /* 0: the set has no such region */
    uint8_t     fill;                  /* what the pages no segment covers read */
    dc_seg_t    seg[DC_MAX_SEGS];
} dc_region_t;

typedef struct {
    const char  *profile;              /* game_profile_t.id */
    dc_region_t  rg[DC_REGIONS];
} dc_layout_t;

static const dc_layout_t dc_layout_sfight = {
    "sfight_console", {
        { "maincpu",   0x0200000, 0x00, { { "ROM_CODE1.BIN", 0, 0x100000, 0 } } },
        { "main_data", 0x2000000, 0xFF, { { "ROM_DATA.BIN", 0, 0x1000000, 0 },
                                          { "ROM_EP.BIN", 0, 920048, 0x1000000, 0x100000, 16 } } },
        { "copro",     0, 0, { { 0 } } },
        { "polygons",  0x1000000, 0x00, { { "ROM_POL.BIN", 0, 0x1000000, 0 } } },
        { "textures",  0x1000000, 0x00, { { "ROM_TEX.BIN", 0, 0x400000, 0 },
                                          { "ROM_TEX.BIN", 0x400000, 0x400000, 0x800000 } } },
        { "audiocpu",  0, 0, { { 0 } } },
        { "samples",   0, 0, { { 0 } } },
    }
};

#endif /* DC_LAYOUT_H */
