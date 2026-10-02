/*
 * dc_pack.h -- the Dreamcast build's ROM pack, as m2pack.c writes it and
 * dc_pager.h reads it off the disc (Pinboard #340). Little-endian, like both
 * machines that touch it.
 */
#ifndef DC_PACK_H
#define DC_PACK_H

#include <stdint.h>

#define DC_PACK_MAGIC   "M2PACK1"
#define DC_PACK_PAGE    4096u          /* the SH-4 MMU's small page */
#define DC_PACK_SECTOR  2048u          /* a mode 1 sector: data_off is aligned to it */
#define DC_PACK_ZERO    0xFFFFFFFFu    /* table entry for an all-zero page */
#define DC_PACK_REGIONS 7              /* the romset_t buffers, in its order */

typedef struct {
    char     magic[8];
    char     profile[24];              /* game_profile_t.id the pack was made with */
    char     set[24];                  /* the zip's base name, for the profile lookup */
    uint32_t page_size;
    uint32_t nregions;
    uint32_t npages;                   /* table entries: every page of every region */
    uint32_t nunique;                  /* pages stored after data_off */
    uint32_t data_off;                 /* byte offset of stored page 0 */
    uint32_t reserved[3];
} dc_pack_hdr_t;

typedef struct {
    char     name[16];                 /* maincpu, main_data, copro, polygons, ... */
    uint32_t size;
    uint32_t first_page;               /* its first entry in the table */
} dc_pack_region_t;

#endif /* DC_PACK_H */
