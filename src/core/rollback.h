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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "savestate.h"

#define ROLLBACK_PARTS_MAX (32 + SAVESTATE_EXTRA_MAX)
#define ROLLBACK_POOL_MAX  16

/* pack: a part that holds host pointers zeroes them (in a copy) before it is
 * hashed, as savestate_save writes it, so two processes' hashes agree. ptrs:
 * those pointers are the only thing pack changes, so a packed copy is a
 * portable one (rollback_save_portable); META's pack drops host clocks a
 * state has to keep. */
typedef struct { const char *name; void *p; size_t n; void (*pack)(void *); bool ptrs; } rollback_part_t;

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

/* The two host sample clocks survive a board reset on purpose (an A/V client
 * would hear the seam), so two machines carry whatever they had run before
 * the cold boot. Nothing on the board reads them. */
static inline void rollback__pack_meta(void *p) {
    rollback_meta_t *m = (rollback_meta_t *)p;
    m->snd.out_total = 0;
    m->emu.frame_clock_sample = 0;
}

static struct {
    rollback_part_t part[ROLLBACK_PARTS_MAX];
    int             n;
    size_t          size;
    rollback_meta_t meta;               /* staged here on the way in and out */
    void           *pool[ROLLBACK_POOL_MAX];
    int             pool_n;
    uint32_t        live;               /* buffers handed out and not yet back */
} g_rollback;

static inline void rollback__add(const char *name, void *p, size_t n, void (*pack)(void *), bool ptrs) {
    if (g_rollback.n >= ROLLBACK_PARTS_MAX) {
        LOG_ERROR("rollback: more parts than ROLLBACK_PARTS_MAX");
        return;
    }
    g_rollback.part[g_rollback.n].name = name;
    g_rollback.part[g_rollback.n].p = p;
    g_rollback.part[g_rollback.n].n = n;
    g_rollback.part[g_rollback.n].pack = pack;
    g_rollback.part[g_rollback.n].ptrs = ptrs;
    g_rollback.n++;
    g_rollback.size += n;
}

/* The parts, in savestate.h's order. Built at a session's start: the bus's
 * heap regions keep their addresses for the life of the process (mem_init
 * clears them in place), and a profile's extras are the running profile's. */
static inline void rollback_layout(i960_cpu_t *cpu, memory_bus_t *bus) {
    g_rollback.n = 0;
    g_rollback.size = 0;
    rollback__add("I960", cpu, sizeof *cpu, NULL, false);
    rollback__add("SHARC", &g_sharc, sizeof g_sharc, rollback__pack_sharc, true);
    rollback__add("COP", &g_cop, sizeof g_cop, rollback__pack_cop, true);
    rollback__add("ZANZOU", &g_zz, sizeof g_zz, NULL, false);
    for (size_t i = 0; i < SAVESTATE_NBUFS; i++)
        rollback__add(SAVESTATE_BUFS[i].name, savestate_buf_ptr(bus, &SAVESTATE_BUFS[i]), SAVESTATE_BUFS[i].size, NULL, false);
    rollback__add("GEO.snaps", g_geodl_snaps, sizeof g_geodl_snaps, NULL, false);
    rollback__add("GEO.live", &g_geo_live, sizeof g_geo_live, NULL, false);
    rollback__add("IRQT", &g_irqt, sizeof g_irqt, NULL, false);
    rollback__add("M68K", &g_sound.m68k.cpu, sizeof g_sound.m68k.cpu, NULL, false);
    rollback__add("SOUND.ram", g_sound.ram, sizeof g_sound.ram, NULL, false);
    rollback__add("M2SCSP", &g_sound.scsp, sizeof g_sound.scsp, rollback__pack_scsp, true);
    rollback__add("SHLE", &g_shle, sizeof g_shle, NULL, false);
    for (int i = 0; i < g_savestate_extra_n; i++)
        if (savestate_extra_mine(i)) rollback__add(g_savestate_extra[i].name, g_savestate_extra[i].data, g_savestate_extra[i].size, NULL, false);
    rollback__add("META", &g_rollback.meta, sizeof g_rollback.meta, rollback__pack_meta, false);
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

/* The board into `buf`, g_rollback.size bytes the caller owns (libretro's
 * rewind buffer, Pinboard #585). */
static inline void rollback_save_to(void *dst, const savestate_emu_t *emu) {
    sound_settle();
    uint8_t *buf = (uint8_t *)dst;
    g_rollback.meta.geo = savestate_geo_get();
    g_rollback.meta.snd = savestate_sound_get();
    g_rollback.meta.hle = savestate_hle_get();
    g_rollback.meta.emu = *emu;
    size_t off = 0;
    for (int i = 0; i < g_rollback.n; i++) {
        memcpy(buf + off, g_rollback.part[i].p, g_rollback.part[i].n);
        off += g_rollback.part[i].n;
    }
}

/* The board into a pooled buffer of g_rollback.size bytes, or NULL. */
static inline void *rollback_save(const savestate_emu_t *emu) {
    void *buf = rollback__take();
    if (buf) rollback_save_to(buf, emu);
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

/* ---- A flat state that leaves the process (libretro, Pinboard #585) ------------- */

/* The same parts behind a header, the host pointers packed as savestate.h
 * packs them, so a state from one process loads in another. RetroArch's rewind
 * serialises every frame, and the zip's CRC and ROM hash over 16 MB cost more
 * than the frame; this is a copy. The header carries the checks the zip's
 * INFO, LAYOUT and ROM entries do, plus the part list itself. */
#define ROLLBACK_FLAT_MAGIC   0x524C324Du    /* "M2LR" */
#define ROLLBACK_FLAT_VERSION 1u

typedef struct {
    uint32_t           magic, version;
    savestate_layout_t layout;
    savestate_rom_t    rom;
    char               profile[64];
    uint64_t           parts;               /* FNV-64 of the part names and sizes */
    uint64_t           size;                /* the parts' bytes after the header */
} rollback_flat_head_t;

#define ROLLBACK_FLAT_HEAD ((sizeof(rollback_flat_head_t) + 63) & ~(size_t)63)

static inline uint64_t rollback__parts_id(void) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (int i = 0; i < g_rollback.n; i++) {
        uint64_t n = g_rollback.part[i].n;
        h = (h ^ savestate_fnv64(g_rollback.part[i].name, strlen(g_rollback.part[i].name))) * 0x100000001B3ull;
        h = (h ^ savestate_fnv64(&n, sizeof n)) * 0x100000001B3ull;
    }
    return h;
}

/* The bytes a flat state takes, for the layout rollback_layout built. */
static inline size_t rollback_flat_size(void) { return ROLLBACK_FLAT_HEAD + g_rollback.size; }

static inline void rollback__flat_head(rollback_flat_head_t *h, const savestate_rom_t *rom) {
    memset(h, 0, sizeof *h);
    h->magic   = ROLLBACK_FLAT_MAGIC;
    h->version = ROLLBACK_FLAT_VERSION;
    h->layout  = savestate_layout();
    h->rom     = *rom;
    snprintf(h->profile, sizeof h->profile, "%s", g_active_profile ? g_active_profile->id : "");
    h->parts   = rollback__parts_id();
    h->size    = g_rollback.size;
}

/* Packs the pointer parts of a flat state in place. The buffer is the
 * caller's and need not be aligned, so each goes through an aligned copy. */
static inline void rollback__flat_pack(uint8_t *buf) {
    static void *tmp;
    static size_t tmp_n;
    size_t off = 0;
    for (int i = 0; i < g_rollback.n; i++) {
        const rollback_part_t *r = &g_rollback.part[i];
        if (r->ptrs && r->pack) {
            if (tmp_n < r->n) {
                free(tmp);
                tmp = malloc(r->n);
                tmp_n = tmp ? r->n : 0;
            }
            if (!tmp) return;
            memcpy(tmp, buf + off, r->n);
            r->pack(tmp);
            memcpy(buf + off, tmp, r->n);
        }
        off += r->n;
    }
}

/* The board into `dst`, rollback_flat_size() bytes. `rom` is the loaded ROM's
 * savestate_rom_id, which the caller works out once per load: hashing 48 MB
 * every frame is what this format is for avoiding. */
static inline void rollback_flat_save(void *dst, const savestate_rom_t *rom, const savestate_emu_t *emu) {
    rollback_flat_head_t h;
    rollback__flat_head(&h, rom);
    memset(dst, 0, ROLLBACK_FLAT_HEAD);
    memcpy(dst, &h, sizeof h);
    uint8_t *body = (uint8_t *)dst + ROLLBACK_FLAT_HEAD;
    rollback_save_to(body, emu);
    rollback__flat_pack(body);
}

static inline bool rollback_flat_is(const void *src, size_t n) {
    uint32_t magic;
    if (n < sizeof magic) return false;
    memcpy(&magic, src, sizeof magic);
    return magic == ROLLBACK_FLAT_MAGIC;
}

/* Why `src` cannot be loaded over this board, or NULL. */
static inline const char *rollback_flat_check(const void *src, size_t n, const savestate_rom_t *rom) {
    rollback_flat_head_t h, want;
    if (n < ROLLBACK_FLAT_HEAD) return "the state is too short";
    memcpy(&h, src, sizeof h);
    rollback__flat_head(&want, rom);
    if (h.magic != want.magic || h.version != want.version) return "the state is from another flat-state version";
    if (strncmp(h.profile, want.profile, sizeof h.profile) != 0) return "the state is from another profile";
    if (memcmp(&h.layout, &want.layout, sizeof h.layout) != 0 || h.parts != want.parts || h.size != want.size)
        return "the state is from a build with another layout";
    if (h.rom.program != rom->program || h.rom.program_size != rom->program_size)
        return "the state is from another program ROM";
    if (h.rom.data != rom->data) return "the state's data ROM differs from the one loaded";
    if (h.rom.sound_loaded != rom->sound_loaded || h.rom.sound_rom != rom->sound_rom)
        return "the state's sound board differs from this one";
    if (n - ROLLBACK_FLAT_HEAD < h.size) return "the state is too short";
    return NULL;
}

/* The board back from a state rollback_flat_check passed; the latches into
 * *emu. The live host pointers stay, as savestate.h's load keeps them. */
static inline void rollback_flat_load(const void *src, memory_bus_t *bus, savestate_emu_t *emu) {
    static scsp_t live;
    uint64_t dm_ext = g_sharc.sharc_dm_ext_w;
    uint32_t dm_ext_size = g_sharc.sharc_dm_ext_size;
    uint64_t ctl = g_cop.ctl_w;
    live.clock_w = g_sound.scsp.clock_w; live.ram_w = g_sound.scsp.ram_w;
    live.sink_w = g_sound.scsp.sink_w;   live.sink_ud_w = g_sound.scsp.sink_ud_w;
    rollback_load((const uint8_t *)src + ROLLBACK_FLAT_HEAD, bus, emu);
    g_sharc.sharc_dm_ext_w    = dm_ext;
    g_sharc.sharc_dm_ext_size = dm_ext_size;
    g_cop.ctl_w = ctl;
    savestate_scsp_unpack(&g_sound.scsp, &live);
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
    rollback_meta_t meta_c;
    memcpy(&meta_c, buf + off - sizeof meta_c, sizeof meta_c);
    rollback__pack_meta(&meta_c);
    const uint8_t *meta = (const uint8_t *)&meta_c;
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
