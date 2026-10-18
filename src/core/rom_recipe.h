/*
 * rom_recipe.h — a ROM set as data: which chip goes where.
 *
 * A part is one of MAME's ROM_START lines: ROM_LOAD32_WORD (a pair of 16-bit
 * halves interleaved into 32-bit words), ROM_LOAD16_WORD_SWAP, ROM_LOAD or
 * ROM_COPY, at the destination offset MAME loads it at. noclip's profiles
 * (js/games.js, `regions`) state their sets the same way, so a recipe reads
 * line for line against both MAME and the explorer.
 *
 * A profile whose loader is nothing but a chip list states it as a recipe and
 * loads it with rom_recipe_load. Chips are found by name and then by CRC
 * (zip_extract_from_set), so a set that spells a label otherwise still loads.
 */
#ifndef ROM_RECIPE_H
#define ROM_RECIPE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "rom_loader.h"

typedef enum {
    ROM_MAINCPU, ROM_MAIN_DATA, ROM_COPRO_DATA, ROM_POLYGONS, ROM_TEXTURES,
    ROM_AUDIOCPU, ROM_SAMPLES, ROM_TGP_TABLES, ROM_IOCPU,
    ROM_REGION_COUNT
} rom_region_id_t;
_Static_assert(ROM_REGION_COUNT == ROMSET_FILES, "a region the directory form does not know");

typedef enum { ROM_PART_PAIR32, ROM_PART_SWAP16, ROM_PART_BYTES, ROM_PART_COPY } rom_part_kind_t;

typedef struct {
    rom_part_kind_t kind;
    rom_region_id_t region;
    uint32_t        offset;               /* where it lands in the region */
    const char     *name;   uint32_t crc;     /* PAIR32: the low half */
    const char     *hi;     uint32_t hi_crc;  /* PAIR32: the high half */
    uint32_t        from, len;            /* COPY: the window copied to `offset` */
} rom_part_t;

#define ROM_PAIR32(r, off, lo, lo_crc, hi, hi_crc) { ROM_PART_PAIR32, r, off, lo, lo_crc, hi, hi_crc, 0, 0 }
#define ROM_SWAP16(r, off, n, crc)  { ROM_PART_SWAP16, r, off, n, crc, NULL, 0, 0, 0 }
#define ROM_BYTES(r, off, n, crc)   { ROM_PART_BYTES,  r, off, n, crc, NULL, 0, 0, 0 }
#define ROM_MIRROR(r, from, to, len) { ROM_PART_COPY,  r, to, NULL, 0, NULL, 0, from, len }

typedef struct {
    const char       *set;                       /* for the log */
    uint32_t          size[ROM_REGION_COUNT];    /* 0: the set has no such region */
    const rom_part_t *parts;
    size_t            count;
    /* An earlier revision's chips, tried when a part at the same region and
     * offset is not in the zip: a set dumped from the older board under the
     * same name (MAME's hotdo in a hotd.zip) still loads. */
    const rom_part_t *older;
    size_t            older_count;
} rom_recipe_t;

/* The regions are romset_region's, in its order. */
static inline uint8_t **rom_recipe_region(romset_t *rs, rom_region_id_t r, size_t **size) {
    if ((unsigned)r < ROMSET_FILES) return romset_region(rs, (int)r, size);
    *size = NULL;
    return NULL;
}

/* One part into its region, which rom_recipe_load has allocated. */
static inline int rom_recipe_part(romset_t *rs, const char *child, const char *parent,
                                  const rom_part_t *p) {
    size_t *dsz;
    uint8_t **dst = rom_recipe_region(rs, p->region, &dsz);
    if (!dst || !*dst) { LOG_ERROR("rom recipe: %s goes to a region the set has none of", p->name ? p->name : "a mirror"); return -1; }
    if (p->kind == ROM_PART_COPY) {
        if ((uint64_t)p->from + p->len > *dsz || (uint64_t)p->offset + p->len > *dsz) return -1;
        rom_region_copy(*dst, *dsz, p->from, p->offset, p->len);
        return 0;
    }
    size_t n = 0, nh = 0;
    uint8_t *lo = zip_extract_from_set(child, parent, p->name, &n, p->crc);
    if (!lo) return -1;
    int rc = 0;
    if (p->kind == ROM_PART_PAIR32) {
        uint8_t *hi = zip_extract_from_set(child, parent, p->hi, &nh, p->hi_crc);
        if (hi) interleave_32_word(*dst, *dsz, lo, n, hi, nh, p->offset);
        else    rc = -1;
        free(hi);
    } else if (p->kind == ROM_PART_SWAP16) {
        load_16_word_swap(*dst, *dsz, lo, n, p->offset);
    } else if (p->offset < *dsz) {
        size_t room = *dsz - p->offset;
        memcpy(*dst + p->offset, lo, n < room ? n : room);
    }
    free(lo);
    return rc;
}

/* Part i, or the older revision's chip at its place when the zip lacks it.
 * A chip with a stand-in is tried quietly: a hotdo in a hotd.zip is no error
 * (Pinboard #1040). When neither is there, the newer one is tried again aloud,
 * so the log and the page's missing list (rl_mem_zip_note_missing) name it. */
static inline int rom_recipe_part_any(romset_t *rs, const char *child, const char *parent,
                                      const rom_recipe_t *rc, size_t i) {
    const rom_part_t *p = &rc->parts[i];
    bool stand_in = false;
    for (size_t k = 0; k < rc->older_count; k++)
        stand_in |= rc->older[k].region == p->region && rc->older[k].offset == p->offset;
    if (!stand_in) return rom_recipe_part(rs, child, parent, p);
    g_rl_quiet_missing++;
    int got = rom_recipe_part(rs, child, parent, p);
    for (size_t k = 0; got != 0 && k < rc->older_count; k++) {
        const rom_part_t *o = &rc->older[k];
        if (o->region != p->region || o->offset != p->offset) continue;
        if (rom_recipe_part(rs, child, parent, o) != 0) continue;
        LOG_INFO("%s: %s is the earlier revision's", rc->set, o->name);
        got = 0;
    }
    g_rl_quiet_missing--;
    return got == 0 ? 0 : rom_recipe_part(rs, child, parent, p);
}

/* Every region the recipe names, zeroed, then every part in order (a mirror
 * after the chips it copies, as in MAME). 0, or -1 with the set freed. */
static inline int rom_recipe_load(romset_t *rs, const char *child, const char *parent,
                                  const rom_recipe_t *rc) {
    romset_free(rs);
    for (int r = 0; r < ROM_REGION_COUNT; r++) {
        if (!rc->size[r]) continue;
        size_t *sz;
        uint8_t **reg = rom_recipe_region(rs, (rom_region_id_t)r, &sz);
        if (!(*reg = (uint8_t *)calloc(1, rc->size[r]))) { romset_free(rs); return -1; }
        *sz = rc->size[r];
    }
    for (size_t i = 0; i < rc->count; i++) {
        if (rom_recipe_part_any(rs, child, parent, rc, i) != 0) {
            LOG_ERROR("%s: ROM set load failed", rc->set);
            romset_free(rs);
            return -1;
        }
    }
    rs->loaded = true;
    LOG_INFO("%s: ROM set loaded", rc->set);
    return 0;
}

#endif /* ROM_RECIPE_H */
