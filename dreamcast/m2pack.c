/*
 * m2pack.c -- turn a ROM set into the Dreamcast build's disc pack (Pinboard #340).
 *
 * A Model 2 set is ~60 MB of ROM and the Dreamcast has 16 MB, so the DC build
 * reads its ROM off the disc a 4 KB page at a time (dc_pager.h). This runs the
 * set's own profile loader on the host (load_fn: CRCs, interleaving, the mirror
 * copies, all as every other frontend gets them) and writes each region of the
 * romset as a table of pages, each page stored once: a mirror or a repeated
 * page is one more table entry, and an all-zero page is no data at all.
 *
 *   m2pack <set.zip> <out.bin>       (the parent zip is looked for beside it)
 *
 * Layout (little-endian, dc_pack.h):
 *   dc_pack_hdr_t, dc_pack_region_t[nregions], uint32_t table[npages]
 *   (DC_PACK_ZERO for a zero page), then from data_off the unique pages.
 *
 * Build (from the repo root; V = a vendor/ with miniz, as tests/rom_touch.c):
 *   cc -O2 -std=gnu11 -DM2HLE_VERSION='"dev"' -DM2HLE_DEV_TOOLS=0 \
 *      -Isrc -Isrc/board -Isrc/core -Isrc/ui -Isrc/profiles -Isrc/net \
 *      -I$V/sokol -I$V/miniz -Idreamcast dreamcast/m2pack.c $V/miniz/miniz*.c \
 *      -o m2pack -lpthread -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "log.h"
#include "memory.h"
#include "i960.h"
#include "rom_loader.h"
#include "emu_thread.h"
#include "registry.h"
#include "dc_pack.h"

typedef struct { uint64_t hash; uint32_t idx; } seen_t;

static uint64_t fnv64(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static int all_zero(const uint8_t *p) {
    for (size_t i = 0; i < DC_PACK_PAGE; i++) if (p[i]) return 0;
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: m2pack <set.zip> <out.bin>\n"); return 2; }
    const char *zip = argv[1];

    const char *base = strrchr(zip, '/');
    base = base ? base + 1 : zip;
    char id[64] = {0};
    const char *dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    if (n >= sizeof id) n = sizeof id - 1;
    memcpy(id, base, n);
    const game_profile_t *prof = profile_for_rom_set(id, NULL);
    if (!prof) { fprintf(stderr, "no profile for %s\n", id); return 1; }

    char parent[1024] = {0};
    const char *parent_ptr = NULL;
    if (prof->parent_zip_name) {
        size_t dir = (size_t)(base - zip);
        if (dir + strlen(prof->parent_zip_name) >= sizeof parent) return 1;
        memcpy(parent, zip, dir);
        strcat(parent, prof->parent_zip_name);
        parent_ptr = parent;
    }
    romset_t rs = {0};
    if (prof->load_fn(&rs, zip, parent_ptr) != 0 || !rs.loaded) {
        fprintf(stderr, "%s: load failed\n", zip);
        return 1;
    }

    struct { const char *name; const uint8_t *p; size_t size; } reg[DC_PACK_REGIONS] = {
        { "maincpu",  rs.maincpu,    rs.maincpu_size },
        { "main_data", rs.main_data, rs.main_data_size },
        { "copro",    rs.copro_data, rs.copro_data_size },
        { "polygons", rs.polygons,   rs.polygons_size },
        { "textures", rs.textures,   rs.textures_size },
        { "audiocpu", rs.audiocpu,   rs.audiocpu_size },
        { "samples",  rs.samples,    rs.samples_size },
    };

    dc_pack_hdr_t hdr = {0};
    memcpy(hdr.magic, DC_PACK_MAGIC, sizeof hdr.magic);
    snprintf(hdr.profile, sizeof hdr.profile, "%s", prof->id);
    snprintf(hdr.set, sizeof hdr.set, "%s", id);
    hdr.page_size = DC_PACK_PAGE;
    hdr.nregions  = DC_PACK_REGIONS;
    dc_pack_region_t rg[DC_PACK_REGIONS] = {{{0}}};
    for (int r = 0; r < DC_PACK_REGIONS; r++) {
        snprintf(rg[r].name, sizeof rg[r].name, "%s", reg[r].name);
        rg[r].size       = (uint32_t)reg[r].size;
        rg[r].first_page = hdr.npages;
        hdr.npages += (uint32_t)((reg[r].size + DC_PACK_PAGE - 1) / DC_PACK_PAGE);
    }
    uint32_t *table = calloc(hdr.npages, 4);
    const uint8_t **uniq = calloc(hdr.npages, sizeof *uniq);
    seen_t *seen = calloc(hdr.npages, sizeof *seen);
    uint32_t nseen = 0, zero = 0, dup = 0;
    uint8_t tail[DC_PACK_PAGE];

    for (int r = 0; r < DC_PACK_REGIONS; r++) {
        for (size_t off = 0; off < reg[r].size; off += DC_PACK_PAGE) {
            const uint8_t *pg = reg[r].p + off;
            if (reg[r].size - off < DC_PACK_PAGE) {          /* short last page: pad */
                memset(tail, 0, sizeof tail);
                memcpy(tail, pg, reg[r].size - off);
                pg = uniq[hdr.nunique] = memcpy(malloc(DC_PACK_PAGE), tail, DC_PACK_PAGE);
            }
            uint32_t *slot = &table[rg[r].first_page + off / DC_PACK_PAGE];
            if (all_zero(pg)) { *slot = DC_PACK_ZERO; zero++; continue; }
            uint64_t h = fnv64(pg, DC_PACK_PAGE);
            uint32_t found = DC_PACK_ZERO;
            for (uint32_t i = 0; i < nseen; i++)     /* fine for ~20k pages */
                if (seen[i].hash == h && !memcmp(uniq[seen[i].idx], pg, DC_PACK_PAGE)) { found = seen[i].idx; break; }
            if (found != DC_PACK_ZERO) { *slot = found; dup++; continue; }
            uniq[hdr.nunique] = pg;
            seen[nseen++] = (seen_t){ h, hdr.nunique };
            *slot = hdr.nunique++;
        }
    }

    uint32_t head = (uint32_t)(sizeof hdr + sizeof rg + hdr.npages * 4u);
    hdr.data_off = (head + DC_PACK_SECTOR - 1) & ~(uint32_t)(DC_PACK_SECTOR - 1);

    FILE *f = fopen(argv[2], "wb");
    if (!f) { perror(argv[2]); return 1; }
    fwrite(&hdr, sizeof hdr, 1, f);
    fwrite(rg, sizeof rg, 1, f);
    fwrite(table, 4, hdr.npages, f);
    for (uint32_t pad = head; pad < hdr.data_off; pad++) fputc(0, f);
    for (uint32_t i = 0; i < hdr.nunique; i++) fwrite(uniq[i], DC_PACK_PAGE, 1, f);
    fclose(f);

    printf("%s (%s): %u pages in %d regions, %u stored (%.1f MB), %u zero, %u repeats\n",
           id, prof->id, hdr.npages, DC_PACK_REGIONS, hdr.nunique,
           hdr.nunique * (double)DC_PACK_PAGE / 1048576.0, zero, dup);
    for (int r = 0; r < DC_PACK_REGIONS; r++)
        printf("  %-10s %8u KB\n", rg[r].name, rg[r].size / 1024);
    return 0;
}
