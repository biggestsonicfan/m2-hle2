/*
 * rollback.h — the whole board as one flat block of memory, for rollback
 * netplay (Pinboard #575, core/emu_ggpo.h).
 *
 * The same board a savestate holds (savestate.h, which det_digest proves
 * exact), copied raw: no zip, no ROM hash, no pointer packing. A snapshot
 * never leaves the process that made it, so a host pointer inside a struct
 * (the COP's view of COPRO_CTL, the SCSP's RAM and LFO tables) is still the
 * live one when it comes back. What a savestate load does around the copy
 * (the GEO's published state, the bus's change generations, the sound
 * board's page map) a rollback does too, through the same helpers.
 *
 * GGPO saves a frame every frame it runs and keeps the last ten, so the
 * buffers come from a pool and go back to it: ~16 MB a snapshot, allocated
 * once.
 *
 * The run loop's latches (savestate_emu_t) are the caller's: emu_thread.h owns
 * them, and passes them in.
 *
 * Header-only. Included from emu_thread.h after savestate.h. Emu thread, emu
 * mutex held.
 */
#ifndef M2HLE_ROLLBACK_H
#define M2HLE_ROLLBACK_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "savestate.h"

#define ROLLBACK_PARTS_MAX (32 + SAVESTATE_EXTRA_MAX)
#define ROLLBACK_POOL_MAX  16

/* pack: a part that holds host pointers zeroes them (in a copy) before it is
 * hashed, as savestate_save writes it, so two processes' hashes agree. */
typedef struct { const char *name; void *p; size_t n; void (*pack)(void *); } rollback_part_t;

static inline void rollback__pack_sharc(void *p) { ((sharc_state_t *)p)->sharc_dm_ext_w = 0; }
static inline void rollback__pack_cop(void *p)   { ((cop_state_t *)p)->ctl_w = 0; }
static inline void rollback__pack_scsp(void *p)  { savestate_scsp_pack((scsp_t *)p); }

/* The small structs that are built rather than copied. */
typedef struct {
    savestate_geo_t   geo;
    savestate_sound_t snd;
    savestate_hle_t   hle;
    savestate_emu_t   emu;
} rollback_meta_t;

static struct {
    rollback_part_t part[ROLLBACK_PARTS_MAX];
    int             n;
    size_t          size;
    rollback_meta_t meta;               /* staged here on the way in and out */
    void           *pool[ROLLBACK_POOL_MAX];
    int             pool_n;
    uint32_t        live;               /* buffers handed out and not yet back */
} g_rollback;

static inline void rollback__add(const char *name, void *p, size_t n, void (*pack)(void *)) {
    if (g_rollback.n >= ROLLBACK_PARTS_MAX) {
        LOG_ERROR("rollback: more parts than ROLLBACK_PARTS_MAX");
        return;
    }
    g_rollback.part[g_rollback.n].name = name;
    g_rollback.part[g_rollback.n].p = p;
    g_rollback.part[g_rollback.n].n = n;
    g_rollback.part[g_rollback.n].pack = pack;
    g_rollback.n++;
    g_rollback.size += n;
}

/* The parts, in savestate.h's order. Built at a session's start: the bus's
 * heap regions keep their addresses for the life of the process (mem_init
 * clears them in place), and a profile's extras are the running profile's. */
static inline void rollback_layout(i960_cpu_t *cpu, memory_bus_t *bus) {
    g_rollback.n = 0;
    g_rollback.size = 0;
    rollback__add("I960", cpu, sizeof *cpu, NULL);
    rollback__add("SHARC", &g_sharc, sizeof g_sharc, rollback__pack_sharc);
    rollback__add("COP", &g_cop, sizeof g_cop, rollback__pack_cop);
    rollback__add("ZANZOU", &g_zz, sizeof g_zz, NULL);
    for (size_t i = 0; i < SAVESTATE_NBUFS; i++)
        rollback__add(SAVESTATE_BUFS[i].name, savestate_buf_ptr(bus, &SAVESTATE_BUFS[i]), SAVESTATE_BUFS[i].size, NULL);
    rollback__add("GEO.snaps", g_geodl_snaps, sizeof g_geodl_snaps, NULL);
    rollback__add("GEO.live", &g_geo_live, sizeof g_geo_live, NULL);
    rollback__add("IRQT", &g_irqt, sizeof g_irqt, NULL);
    rollback__add("M68K", &g_sound.m68k.cpu, sizeof g_sound.m68k.cpu, NULL);
    rollback__add("SOUND.ram", g_sound.ram, sizeof g_sound.ram, NULL);
    rollback__add("M2SCSP", &g_sound.scsp, sizeof g_sound.scsp, rollback__pack_scsp);
    rollback__add("SHLE", &g_shle, sizeof g_shle, NULL);
    for (int i = 0; i < g_savestate_extra_n; i++)
        if (savestate_extra_mine(i)) rollback__add(g_savestate_extra[i].name, g_savestate_extra[i].data, g_savestate_extra[i].size, NULL);
    rollback__add("META", &g_rollback.meta, sizeof g_rollback.meta, NULL);
}

static inline void *rollback__take(void) {
    g_rollback.live++;
    if (g_rollback.pool_n > 0) return g_rollback.pool[--g_rollback.pool_n];
    return malloc(g_rollback.size);
}

/* A buffer back. The pool keeps up to ROLLBACK_POOL_MAX of them. */
static inline void rollback_free(void *buf) {
    if (!buf) return;
    if (g_rollback.live) g_rollback.live--;
    if (g_rollback.pool_n < ROLLBACK_POOL_MAX) g_rollback.pool[g_rollback.pool_n++] = buf;
    else free(buf);
}

/* Every pooled buffer, at a session's end. Buffers still out (GGPO's own
 * saved frames) are freed by GGPO's free_buffer as it closes. */
static inline void rollback_drain(void) {
    while (g_rollback.pool_n > 0) free(g_rollback.pool[--g_rollback.pool_n]);
}

/* The board into a buffer of g_rollback.size bytes, or NULL. */
static inline void *rollback_save(const savestate_emu_t *emu) {
    sound_settle();
    uint8_t *buf = (uint8_t *)rollback__take();
    if (!buf) return NULL;
    g_rollback.meta.geo = savestate_geo_get();
    g_rollback.meta.snd = savestate_sound_get();
    g_rollback.meta.hle = savestate_hle_get();
    g_rollback.meta.emu = *emu;
    size_t off = 0;
    for (int i = 0; i < g_rollback.n; i++) {
        memcpy(buf + off, g_rollback.part[i].p, g_rollback.part[i].n);
        off += g_rollback.part[i].n;
    }
    return buf;
}

/* The board back from a buffer rollback_save filled; the latches into *emu. */
static inline void rollback_load(const void *data, memory_bus_t *bus, savestate_emu_t *emu) {
    sound_settle();
    const uint8_t *buf = (const uint8_t *)data;
    size_t off = 0;
    for (int i = 0; i < g_rollback.n; i++) {
        memcpy(g_rollback.part[i].p, buf + off, g_rollback.part[i].n);
        off += g_rollback.part[i].n;
    }
    savestate_geo_put(&g_rollback.meta.geo);
    savestate_bus_changed(bus);
    savestate_sound_put(&g_rollback.meta.snd);
    savestate_hle_set(&g_rollback.meta.hle);
    *emu = g_rollback.meta.emu;
}

/* Part i of a snapshot, its host pointers zeroed. */
static inline uint64_t rollback__hash_part(int i, const uint8_t *p) {
    const rollback_part_t *r = &g_rollback.part[i];
    if (!r->pack) return savestate_fnv64(p, r->n);
    void *tmp = malloc(r->n);
    if (!tmp) return 0;
    memcpy(tmp, p, r->n);
    r->pack(tmp);
    uint64_t h = savestate_fnv64(tmp, r->n);
    free(tmp);
    return h;
}

/* A snapshot part by part, for synctest's mismatch logs: the two sides'
 * files differ on the line of the part that moved. */
static inline void rollback_log(FILE *f, const void *data) {
    const uint8_t *buf = (const uint8_t *)data;
    size_t off = 0;
    for (int i = 0; i < g_rollback.n; i++) {
        fprintf(f, "%-12s %8zu %016llx\n", g_rollback.part[i].name, g_rollback.part[i].n,
                (unsigned long long)rollback__hash_part(i, buf + off));
        off += g_rollback.part[i].n;
    }
    /* The built structs one by one: META is the part that says least. */
    const uint8_t *meta = buf + off - sizeof(rollback_meta_t);
    static const struct { const char *name; size_t off, n; } m[] = {
        { "META.geo", offsetof(rollback_meta_t, geo), sizeof(savestate_geo_t) },
        { "META.snd", offsetof(rollback_meta_t, snd), sizeof(savestate_sound_t) },
        { "META.hle", offsetof(rollback_meta_t, hle), sizeof(savestate_hle_t) },
        { "META.emu", offsetof(rollback_meta_t, emu), sizeof(savestate_emu_t) },
    };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++)
        fprintf(f, "%-12s %8zu %016llx\n", m[i].name, m[i].n,
                (unsigned long long)savestate_fnv64(meta + m[i].off, m[i].n));
}

/* What GGPO's synctest compares, and what two machines' checks log: the same
 * board gives the same number in any process. */
static inline uint32_t rollback_checksum(const void *data) {
    const uint8_t *buf = (const uint8_t *)data;
    uint64_t h = 0;
    size_t off = 0;
    for (int i = 0; i < g_rollback.n; i++) {
        h = (h ^ rollback__hash_part(i, buf + off)) * 0x100000001B3ull;
        off += g_rollback.part[i].n;
    }
    return (uint32_t)(h ^ (h >> 32));
}

#endif
