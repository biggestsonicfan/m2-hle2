/*
 * dc_strips.c -- STRIPS.PAK for the Dreamcast disc (Pinboard #498), from the
 * PS3 release's ROM files and the keys the disc's recorded frames draw
 * (dreamcast/sfight.strips). Each key's mesh is walked once, as the draw's
 * cache walks it (geo3d_mesh_build), and written as dreamcast/dc_strips.h's
 * blob. The pack is ROM-derived: mkdisc.sh builds it, nothing commits it.
 *
 *   cc -O2 -o dc_strips tools/dc_strips.c -Isrc -Isrc/board -Isrc/core \
 *      -Isrc/net -Isrc/ui -Isrc/profiles -Idreamcast -lm
 *   dc_strips --roms <stf_rom> --keys dreamcast/sfight.strips --out STRIPS.PAK \
 *             [--groups dreamcast/sfight.mdlgroups]
 *
 * With --groups the blobs are laid out by object group (dc_strips.h, #504);
 * a model no group names goes in a group "other".
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "log.h"
#include "memory.h"
#include "geo3d.h"

#define DCS_WRITER
#include "dc_layout.h"
#include "dc_strips.h"

static uint8_t *g_rg[DC_REGIONS];

/* A region as the disc's pager lays it out (dc_layout.h). */
static int load_region(const char *dir, int r) {
    const dc_region_t *rg = &dc_layout_sfight.rg[r];
    if (!rg->size) return 0;
    uint8_t *b = malloc(rg->size);
    if (!b) return -1;
    memset(b, rg->fill, rg->size);
    for (int k = 0; k < DC_MAX_SEGS && rg->seg[k].file; k++) {
        const dc_seg_t *sg = &rg->seg[k];
        char path[1024], low[64];
        size_t n = strlen(sg->file);
        for (size_t c = 0; c <= n && c < sizeof low; c++) low[c] = (char)tolower((unsigned char)sg->file[c]);
        snprintf(path, sizeof path, "%s/%s", dir, sg->file);
        FILE *f = fopen(path, "rb");
        if (!f) { snprintf(path, sizeof path, "%s/%s", dir, low); f = fopen(path, "rb"); }
        if (!f) { fprintf(stderr, "dc_strips: no %s in %s\n", sg->file, dir); return -1; }
        uint32_t reps = sg->count ? sg->count : 1;
        for (uint32_t c = 0; c < reps; c++) {
            uint32_t at = sg->reg_off + c * sg->period, len = sg->len;
            if (at >= rg->size) break;
            if (len > rg->size - at) len = rg->size - at;
            fseek(f, (long)sg->file_off, SEEK_SET);
            if (!fread(b + at, 1, len, f)) { fclose(f); return -1; }
        }
        fclose(f);
    }
    g_rg[r] = b;
    return 0;
}

typedef struct { int32_t model; uint32_t mat, uv, frame, off, len, group, order; } key_t_;

static int key_cmp(const void *a, const void *b) {
    const key_t_ *x = a, *y = b;
    return dcs_key_cmp(x->model, x->mat, x->uv, y->model, y->mat, y->uv);
}

/* sfight.mdlgroups: `group NAME`, then model table numbers (hex), each in the
 * first group that names it. */
#define MAX_GROUPS 64
static char     g_gname[MAX_GROUPS + 1][24];
static int      g_ngroups;
static uint8_t  g_model_group[65536];   /* group + 1, 0: none */

static int read_groups(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "dc_strips: cannot read %s\n", path); return -1; }
    char line[1024];
    int cur = -1;
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#');
        if (h) *h = 0;
        if (!strncmp(line, "group ", 6)) {
            if (g_ngroups == MAX_GROUPS) { fprintf(stderr, "dc_strips: over %d groups\n", MAX_GROUPS); fclose(f); return -1; }
            cur = g_ngroups++;
            sscanf(line + 6, "%23s", g_gname[cur]);
            continue;
        }
        for (char *t = strtok(line, " \t\r\n"); t; t = strtok(NULL, " \t\r\n")) {
            unsigned long m = strtoul(t, NULL, 16);
            if (cur >= 0 && m < 65536 && !g_model_group[m]) g_model_group[m] = (uint8_t)(cur + 1);
        }
    }
    fclose(f);
    snprintf(g_gname[g_ngroups], sizeof g_gname[0], "other");
    return 0;
}

static uint32_t g_rank[MAX_GROUPS + 1];   /* a group's place: by the first frame that draws from it */

static int layout_cmp(const void *a, const void *b) {
    const key_t_ *x = a, *y = b;
    if (g_rank[x->group] != g_rank[y->group]) return g_rank[x->group] < g_rank[y->group] ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

int main(int argc, char **argv) {
    const char *roms = NULL, *keys = NULL, *out = NULL, *groups = NULL;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--roms")) roms = argv[i + 1];
        else if (!strcmp(argv[i], "--keys")) keys = argv[i + 1];
        else if (!strcmp(argv[i], "--out")) out = argv[i + 1];
        else if (!strcmp(argv[i], "--groups")) groups = argv[i + 1];
    }
    if (!roms || !keys || !out) {
        fprintf(stderr, "usage: dc_strips --roms <stf_rom> --keys <sfight.strips> --out <STRIPS.PAK> [--groups <sfight.mdlgroups>]\n");
        return 2;
    }
    if (groups && read_groups(groups) != 0) return 1;
    for (int r = 0; r < DC_REGIONS; r++)
        if (load_region(roms, r) != 0) return 1;
    const dc_region_t *rg = dc_layout_sfight.rg;
    const uint8_t *main_data = g_rg[1], *polygons = g_rg[3], *textures = g_rg[4];
    const size_t main_data_size = rg[1].size, polygons_size = rg[3].size, textures_size = rg[4].size;

    FILE *kf = fopen(keys, "r");
    if (!kf) { fprintf(stderr, "dc_strips: cannot read %s\n", keys); return 1; }
    uint32_t table_off = 0, table_count = 0, sub = 0, add = 0;
    size_t nk = 0, cap = 0;
    key_t_ *k = NULL;
    char line[256];
    while (fgets(line, sizeof line, kf)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (!strncmp(line, "table ", 6)) { sscanf(line + 6, "%x %u %x %x", &table_off, &table_count, &sub, &add); continue; }
        key_t_ e = { 0 };
        if (sscanf(line, "%d %x %x %u", &e.model, &e.mat, &e.uv, &e.frame) != 4) continue;
        if (nk == cap) { cap = cap ? cap * 2 : 1024; k = realloc(k, cap * sizeof *k); }
        e.order = (uint32_t)nk;
        e.group = e.model >= 0 && e.model < 65536 && g_model_group[e.model] ? g_model_group[e.model] - 1u : (uint32_t)g_ngroups;
        k[nk++] = e;
    }
    fclose(kf);
    if (!table_count) { fprintf(stderr, "dc_strips: %s has no table line\n", keys); return 1; }

    /* the blobs, in the keys' order (the frame that first draws them); with
     * groups, group by group, each from a sector, in the order of the first
     * frame that draws from it */
    if (groups) {
        for (int g = 0; g <= g_ngroups; g++) g_rank[g] = ~0u;
        for (size_t i = 0; i < nk; i++) if (k[i].order < g_rank[k[i].group]) g_rank[k[i].group] = k[i].order;
        qsort(k, nk, sizeof *k, layout_cmp);
    }
    static dcs_group_t grp[MAX_GROUPS + 1];
    uint32_t ng = 0;
    size_t blob_cap = 64u << 20, at = 0;
    uint8_t *blobs = calloc(1, blob_cap);
    static uint8_t one[4u << 20];
    uint32_t verts = 0, faces = 0, kept = 0, skipped = 0;
    for (size_t i = 0; i < nk; i++) {
        key_t_ *e = &k[i];
        e->len = 0;
        if (groups && (!i || e->group != k[i - 1].group)) {
            at = (at + DC_SECTOR - 1) / DC_SECTOR * DC_SECTOR;
            grp[ng].off = (uint32_t)at;
            snprintf(grp[ng].name, sizeof grp[ng].name, "%s", g_gname[e->group]);
            if (ng) grp[ng - 1].len = (uint32_t)at - grp[ng - 1].off;
            ng++;
        }
        if (e->model < 0 || (uint32_t)e->model >= table_count) { skipped++; continue; }
        uint32_t toff = table_off + (uint32_t)e->model * MODEL_ENTRY_SIZE;
        if ((size_t)toff + MODEL_ENTRY_SIZE > main_data_size) { skipped++; continue; }
        uint32_t raw = read_u32_le(main_data + toff + 8);
        if (!raw) { skipped++; continue; }
        geo3d_cmesh_t m = { .model_idx = e->model, .mat_ptr = e->mat, .uv_ptr = e->uv,
                            .md = { .polygons = polygons, .materials = textures, .main_data = main_data,
                                    .polygons_size = polygons_size, .materials_size = textures_size,
                                    .table_off = table_off, .table_count = table_count,
                                    .mesh_ptr_subtract = sub, .mesh_ptr_add = add } };
        if (!geo3d_mesh_build(&m, raw * 4u - sub + add, e->mat != 0, e->uv != 0)) { skipped++; continue; }
        size_t len = dcs_blob(&m, one, sizeof one);
        if (len && at + len <= blob_cap) {
            memcpy(blobs + at, one, len);
            e->off = (uint32_t)at;
            e->len = (uint32_t)len;
            at += (len + 31u) & ~(size_t)31u;
            const geo3d_sp_head_t *h = (const geo3d_sp_head_t *)one;
            faces += h->n_faces;
            verts += (uint32_t)((len - sizeof *h - (((size_t)h->n_sv * sizeof(vec3_t) + 31u) & ~(size_t)31u) -
                                 (size_t)h->n_faces * sizeof(geo3d_sface_t)) / sizeof(geo3d_svert_t));
            kept++;
        } else skipped++;
        free(m.sv);
        free(m.faces);
    }
    if (ng) {
        at = (at + DC_SECTOR - 1) / DC_SECTOR * DC_SECTOR;   /* every group whole sectors */
        grp[ng - 1].len = (uint32_t)at - grp[ng - 1].off;
    }

    /* the index, by key */
    qsort(k, nk, sizeof *k, key_cmp);
    size_t ni = 0;
    for (size_t i = 0; i < nk; i++) if (k[i].len) ni++;
    dcs_head_t h = { { 'M', '2', 'S', 'P' }, (uint32_t)ni, 0,
                     (uint32_t)polygons_size, (uint32_t)textures_size, table_off, table_count, sub, add,
                     verts, faces, (uint32_t)at, ng, 0, { 0 } };
    h.groups_off = (uint32_t)(sizeof h + ni * sizeof(dcs_index_t));
    h.data_off = (uint32_t)((h.groups_off + ng * sizeof(dcs_group_t) + DC_SECTOR - 1) / DC_SECTOR * DC_SECTOR);
    FILE *of = fopen(out, "wb");
    if (!of) { fprintf(stderr, "dc_strips: cannot write %s\n", out); return 1; }
    uint8_t *head = calloc(1, h.data_off);
    memcpy(head, &h, sizeof h);
    dcs_index_t *ix = (dcs_index_t *)(head + sizeof h);
    for (size_t i = 0, j = 0; i < nk; i++)
        if (k[i].len) ix[j++] = (dcs_index_t){ k[i].model, k[i].mat, k[i].uv, k[i].off, k[i].len };
    memcpy(head + h.groups_off, grp, ng * sizeof(dcs_group_t));
    fwrite(head, 1, h.data_off, of);
    fwrite(blobs, 1, at, of);
    fclose(of);
    printf("dc_strips: %u meshes (%u skipped), %u faces, %u vertices, %.1f MB\n",
           kept, skipped, faces, verts, (double)(h.data_off + at) / 1048576.0);
    for (uint32_t g = 0; g < ng; g++)
        printf("  group %-24s %5u KB\n", grp[g].name, (unsigned)(grp[g].len >> 10));
    return 0;
}
