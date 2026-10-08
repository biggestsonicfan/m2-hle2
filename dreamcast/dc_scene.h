/*
 * dc_scene.h -- load a scene's textures and meshes whole, as Sonic Gems
 * Collection loads its OBJ_* / TEX_* files (Pinboard #567).
 *
 * The draw makes a texture or a mesh the first time a face needs it: a
 * window read of TEXTURES.PAK, a page of STRIPS.PAK through the pager. This
 * reads the board's RAM once a drawn frame instead, and when the scene it
 * names changes -- the game's mode (attract or a game), the stage, the two
 * fighters -- it drops every texture and mesh the draw held and reads the
 * new scene's groups from the two packs (both recorded by group, each with a
 * table of them: dc_texpak.h, dc_strips.h) in one pass each: the fighters',
 * the stage's, then common. The textures go to video memory staged, keyed by
 * (key, the pack's hash); dp_tex_get adopts one whose hash the texture RAM
 * then has, with no read. The meshes go into the mesh cache as
 * geo3d_mesh_get would have put them there.
 *
 * What it reads (STF; DREAMCAST-PORT.md #567 has how it was found):
 *  - the mode byte at 0x50002A: 3 attract, 9 a game; nothing else is a scene;
 *  - stage_num at 0x500064;
 *  - fa_rob0 / fa_rob1 at 0x500804 / 0x500808, each fighter's id at rob +0x1B0
 *    (a second colour's already +0x1A: 26 is Sonic in his other colours,
 *    whose models are the -mirror group's).
 * The scene must hold for DSC_SETTLE drawn frames before it counts, so a
 * fighter id half written in a change of scene loads nothing.
 *
 * DC_SCENE (the Makefile's SCENE=): 0 off, 1 textures, 2 textures and meshes.
 * Included by dc_pvr.h after the texture cache and before dp_tex_get.
 */
#ifndef DC_SCENE_H
#define DC_SCENE_H

#ifndef DC_SCENE
#define DC_SCENE 0
#endif

#define DSC_SETTLE   3                /* drawn frames a scene holds before it loads */
#define DSC_SLOTS    (DC_SCENE ? 512u : 1u)    /* staged textures, power of two (STF's pack holds 280) */
#define DSC_VRAM_KEEP (1536u << 10)   /* video memory the staging leaves for the draw */

typedef struct { uint32_t key, hash; pvr_ptr_t ptr; uint8_t taken; } dsc_tex_t;

static struct {
    uint8_t   cur[4], cand[4];        /* the scene loaded (mode, stage, P0, P1), the one settling */
    uint8_t   settle, due;            /* frames cand held; load at the next frame */
    dsc_tex_t tex[DSC_SLOTS];
    uint32_t  ntex;
    geo3d_models_t md;                /* the strip pack's models, as the draw named them */
    uint8_t   md_ok;
    /* counters, since boot: scenes loaded, textures staged and adopted, KB read, ms, meshes */
    uint32_t  scenes, staged, adopted, kb, ms, meshes;
} g_dsc;

static inline uint32_t dsc_hash(uint32_t key, uint32_t hash) { return ((key * 2654435761u) ^ hash) & (DSC_SLOTS - 1u); }

/* The staged textures not adopted go back to video memory (none was drawn). */
static void dsc_tex_release(void) {
    for (uint32_t i = 0; i < DSC_SLOTS; i++) {
        dsc_tex_t *t = &g_dsc.tex[i];
        if (t->key && !t->taken) pvr_mem_free(t->ptr);
        t->key = 0;
    }
    g_dsc.ntex = 0;
}

/* dp_tex_get's miss: a staged texture for (key, hash), handed over, or NULL. */
static pvr_ptr_t dsc_tex_adopt(uint32_t key, uint32_t hash) {
    if (!g_dsc.ntex) return NULL;
    for (uint32_t p = 0, h = dsc_hash(key, hash); p < DSC_SLOTS; p++) {
        dsc_tex_t *t = &g_dsc.tex[(h + p) & (DSC_SLOTS - 1u)];
        if (!t->key) return NULL;
        if (t->key == key && t->hash == hash && !t->taken) {
            t->taken = 1;
            g_dsc.adopted++;
            return t->ptr;
        }
    }
    return NULL;
}

/* ---- The scene's groups ------------------------------------------------------ */

static const char *const dsc_stage_name[16] = {
    "south-island", "flying-carpet", "aurora-icefield", "mushroom-hill", "canyon-cruise", "casino-night",
    "dynamite-plant", "giant-wing", "death-egg", "death-egg", "death-egg", NULL, "mushroom-hill",
    "south-island", "south-island", "tails-lab"
};
static const char *const dsc_char_name[16] = {
    "sonic", "tails", "amy", "metal-sonic", "fang", "bark", "knuckles", "espio", "eggman", "eggman-b",
    "bean", "eggman-boss", "egg-ufo", "egg-minion", "rocket-metal", "honey"
};

/* A fighter id's group name into out, or false: 26 on are the second colours. */
static bool dsc_char_group(uint8_t id, char out[24]) {
    if (id < 16) { snprintf(out, 24, "char-%s", dsc_char_name[id]); return true; }
    if (id >= 26 && id < 42 && dsc_char_name[id - 26]) {
        snprintf(out, 24, "char-%s-mirror", dsc_char_name[id - 26]);
        return true;
    }
    return false;
}

/* The scene's group names, in the order they are read; how many. */
static int dsc_groups(const uint8_t sc[4], char names[4][24]) {
    int n = 0;
    for (int f = 0; f < 2; f++)
        if (dsc_char_group(sc[2 + f], names[n]) && (n == 0 || strcmp(names[0], names[n]))) n++;
    if (sc[1] < 16 && dsc_stage_name[sc[1]]) snprintf(names[n++], 24, "stage-%s", dsc_stage_name[sc[1]]);
    snprintf(names[n++], 24, "common");
    return n;
}

/* A group table's entry named name: its offset and length from the data, or false. */
static bool dsc_find(const uint8_t *head, uint32_t data_off, uint32_t ngroups, uint32_t groups_off,
                     const char *name, uint32_t *off, uint32_t *len) {
    if (!ngroups || groups_off + (uint64_t)ngroups * sizeof(dct_group_t) > data_off) return false;
    const dct_group_t *g = (const dct_group_t *)(head + groups_off);
    for (uint32_t i = 0; i < ngroups; i++)
        if (!strncmp(g[i].name, name, sizeof g[i].name)) { *off = g[i].off; *len = g[i].len; return true; }
    return false;
}

/* ---- Textures ------------------------------------------------------------------ */

/* One pack texture staged: false when video memory is down to the draw's share. */
static bool dsc_tex_stage(const dct_index_t *e) {
    const uint32_t bytes = dct_bytes(e->key);
    if (pvr_mem_available() < DSC_VRAM_KEEP + bytes || g_dsc.ntex >= DSC_SLOTS * 3u / 4u) return false;
    uint32_t h = dsc_hash(e->key, e->hash), p = 0;
    while (g_dsc.tex[(h + p) & (DSC_SLOTS - 1u)].key) {
        const dsc_tex_t *t = &g_dsc.tex[(h + p) & (DSC_SLOTS - 1u)];
        if (t->key == e->key && t->hash == e->hash) return true;   /* in two groups' read */
        p++;
    }
    const uint8_t *src = pg_tx_at(e->off, bytes);
    pvr_ptr_t v = src ? pvr_mem_malloc(bytes) : NULL;
    if (!v) return src != NULL;
    pvr_txr_load((void *)src, v, bytes);
    g_dsc.tex[(h + p) & (DSC_SLOTS - 1u)] = (dsc_tex_t){ e->key, e->hash, v, 0 };
    g_dsc.ntex++;
    g_dsc.staged++;
    g_dsc.kb += bytes >> 10;
    return true;
}

/* The pack's textures in [off, off + len), in disc order: the index is by key,
 * so its entries in the group are gathered and sorted first. */
static int dsc_tex_off_cmp(const void *a, const void *b) {
    const uint32_t x = (*(const dct_index_t *const *)a)->off, y = (*(const dct_index_t *const *)b)->off;
    return x < y ? -1 : x > y;
}

static bool dsc_tex_group(uint32_t off, uint32_t len) {
    static const dct_index_t *in[4096];
    const dct_index_t *ix = (const dct_index_t *)(g_pg.tx + 1);
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_pg.tx->n && n < 4096; i++)
        if (ix[i].off >= off && ix[i].off < off + len) in[n++] = &ix[i];
    qsort(in, n, sizeof in[0], dsc_tex_off_cmp);
    for (uint32_t i = 0; i < n; i++)
        if (!dsc_tex_stage(in[i])) return false;
    return true;
}

/* ---- Meshes -------------------------------------------------------------------- */

#if DC_SCENE >= 2 && defined(GEO3D_STRIPS) && GEO3D_MESH_ARENA
/* One pack mesh into the cache, as geo3d_mesh_get puts one there: false when
 * the cache has had its share (the draw needs the rest). */
static bool dsc_mesh_put(const dcs_index_t *e) {
    if (g_geo3d_mesh_count >= GEO3D_MESH_CACHE_SLOTS / 2u || e->len > DC_TX_WINDOW - DC_SECTOR ||
            g_geo3d_arena_used + e->len > GEO3D_MESH_ARENA * 3u / 5u) return false;
    const uint32_t h = geo3d_mesh_hash(e->model_idx, e->mat_ptr, e->uv_ptr);
    for (uint32_t p = 0; p < GEO3D_MESH_CACHE_SLOTS; p++) {
        const geo3d_cmesh_t *c = &g_geo3d_meshes[(h + p) & (GEO3D_MESH_CACHE_SLOTS - 1u)];
        if (!c->used) break;
        if (c->model_idx == e->model_idx && c->mat_ptr == e->mat_ptr && c->uv_ptr == e->uv_ptr) return true;
    }
    const uint8_t *src = pg_win_at(g_pg.sp_fad, g_pg.sp_size, e->off, e->len);
    geo3d_cmesh_t *m = src ? geo3d_mesh_free_slot(h) : NULL;
    if (!m) return src != NULL;
    *m = (geo3d_cmesh_t){ .model_idx = e->model_idx, .mat_ptr = e->mat_ptr, .uv_ptr = e->uv_ptr, .md = g_dsc.md };
    m->used = true;
    m->used_at = g_geo3d_mesh_epoch + 600u;   /* kept by an eviction for ten seconds, drawn or not */
    geo3d_strips_fill(m, (const geo3d_sp_head_t *)src, g_geo3d_arena + g_geo3d_arena_used,
                      src + sizeof(geo3d_sp_head_t), e->len - (uint32_t)sizeof(geo3d_sp_head_t));
    g_geo3d_arena_used += m->arena_len;
    g_geo3d_mesh_bytes += m->arena_len;
    g_geo3d_mesh_count++;
    g_geo3d_mesh_packed++;
    g_dsc.meshes++;
    return true;
}

static int dsc_mesh_off_cmp(const void *a, const void *b) {
    const uint32_t x = (*(const dcs_index_t *const *)a)->off, y = (*(const dcs_index_t *const *)b)->off;
    return x < y ? -1 : x > y;
}

/* The pack's meshes in [off, off + len), in disc order. */
static bool dsc_mesh_group(uint32_t off, uint32_t len) {
    static const dcs_index_t *in[4096];
    const dcs_index_t *ix = (const dcs_index_t *)(g_pg.sp + 1);
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_pg.sp->n && n < 4096; i++)
        if (ix[i].off >= off && ix[i].off < off + len) in[n++] = &ix[i];
    qsort(in, n, sizeof in[0], dsc_mesh_off_cmp);
    for (uint32_t i = 0; i < n; i++)
        if (!dsc_mesh_put(in[i])) return false;
    return true;
}
#endif

/* ---- The scene ------------------------------------------------------------------- */

/* The scene the board's RAM names (mode, stage, P0, P1); false if none. */
static bool dsc_read(memory_bus_t *bus, uint8_t sc[4]) {
    sc[0] = mem_read8(bus, 0x50002Au);
    if (sc[0] != 3 && sc[0] != 9) return false;
    const uint32_t r0 = mem_read32(bus, 0x500804u), r1 = mem_read32(bus, 0x500808u);
    if ((r0 >> 20) != 0x5u || (r1 >> 20) != 0x5u) return false;   /* not in work RAM yet */
    sc[1] = mem_read8(bus, 0x500064u);
    sc[2] = mem_read8(bus, r0 + 0x1B0u);
    sc[3] = mem_read8(bus, r1 + 0x1B0u);
    return true;
}

/* Read the scene's groups: textures, then (SCENE=2) meshes. */
static void dsc_load(void) {
    const uint64_t t0 = timer_us_gettime64();
    char names[4][24];
    const int n = dsc_groups(g_dsc.cur, names);
    const dct_head_t *th = g_pg.tx;
    bool room = true;
    for (int i = 0; i < n && room; i++) {
        uint32_t off, len;
        if (dsc_find((const uint8_t *)th, th->data_off, th->pad[0], th->pad[1], names[i], &off, &len))
            room = dsc_tex_group(off, len);
    }
#if DC_SCENE >= 2 && defined(GEO3D_STRIPS) && GEO3D_MESH_ARENA
    const dcs_head_t *sh = g_pg.sp;
    room = sh && g_dsc.md_ok;
    for (int i = 0; i < n && room; i++) {
        uint32_t off, len;
        if (dsc_find((const uint8_t *)sh, sh->data_off, sh->ngroups, sh->groups_off, names[i], &off, &len))
            room = dsc_mesh_group(off, len);
    }
#endif
    g_dsc.scenes++;
    g_dsc.ms += (uint32_t)((timer_us_gettime64() - t0) / 1000u);
}

/* Once a drawn frame, after dp_tex_invalidate (its late frees done), before
 * the decode. A new scene drops what the draw held; its groups are read at
 * the next frame, once the video memory the drop gives back is free. */
static void dsc_frame(memory_bus_t *bus) {
    if (!DC_SCENE || !g_pg.tx || !g_pg.tx->pad[0] || !g_pg.tx_win) return;
    if (g_dsc.due) { g_dsc.due = 0; dsc_load(); return; }
    uint8_t sc[4];
    if (!dsc_read(bus, sc)) return;
    if (memcmp(sc, g_dsc.cand, 4)) { memcpy(g_dsc.cand, sc, 4); g_dsc.settle = 0; return; }
    if (++g_dsc.settle != DSC_SETTLE || !memcmp(sc, g_dsc.cur, 4)) return;
    memcpy(g_dsc.cur, sc, 4);
    dsc_tex_release();
    dp_tex_drop_all();
#if DC_SCENE >= 2 && GEO3D_MESH_ARENA
    geo3d_mesh_cache_clear();
#endif
    g_dsc.due = 1;
}

#endif
