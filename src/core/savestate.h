/*
 * savestate.h — the whole board in one file, and back (Pinboard #423).
 *
 * The layout follows m2emulator's .sta: a zip with one entry per part of the
 * board, each the part's own bytes. Its names are kept where the part is the
 * same (I960, SHARC, M2RAM, M2RAM2, M2BUFRAM, M2TILE, M2CG, M2PAL, M2XLAT,
 * M2LUMA, M2BACK, M2FB, M2TEX0, M2TEX1, M2COPRORAM), so `unzip -l` on either
 * reads the same way and a part can be pulled out and diffed on its own. What
 * m2emulator keeps in its own structures and this emulator keeps elsewhere
 * gets an entry of its own: the 68000 and the SCSP, the COP's FIFO and the
 * GEO's list pointers, the interrupt controller, the run loop's latches.
 *
 * What it is for: a headless board that has to survive the machine going down
 * (stf-fly #421 pauses its emulators, saves them, and takes docker down). A
 * board loaded from a state runs on EXACTLY as the one that saved it would
 * have: tests/det_digest.c `--save-at` / `--load` holds every frame's CPU,
 * RAM, COP, texture RAM and sound board hashes, and every sample, against an
 * uninterrupted run.
 *
 * What it is not:
 *   - Portable between builds. A struct is written as its bytes, so a state
 *     loads only into a build with the same layout: `LAYOUT` records the size
 *     of each, and a mismatch is refused, not guessed at. wasm (32-bit
 *     pointers) never matches a native build; x86-64 against aarch64 has not
 *     been tried.
 *   - A netplay tool. A session is still a cold boot on both machines: a state
 *     is a file one machine has, and loading it inside a session would be a
 *     board on this machine and not the other. emu_thread refuses that.
 *   - A copy of the ROM. MAIN_DATA and XTRA_DATA (48 MB, filled from the data
 *     ROMs at install) are recorded as a hash, and the program ROM too, and a
 *     state loads only over the set it was made from.
 *
 * Pointers. Every board struct that holds one keeps the LIVE pointer across a
 * load (the bus's region table and page maps, the COP's view of COPRO_CTL,
 * the SHARC's of bufferram, the SCSP's clock, RAM and sink, the 68000's
 * callbacks and page maps). The SCSP's LFOs point into static tables, and are
 * written as table and row numbers. Caches that follow the board are told it
 * changed: the change generations, texture RAM's dirty flags, the published
 * GEO state.
 *
 * Header-only. Included from emu_thread.h, after the run loop's latches it
 * saves; the run loop services a save or load between slices
 * (emu_state_service), under the emu mutex.
 */
#ifndef M2HLE_SAVESTATE_H
#define M2HLE_SAVESTATE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"
#include "log.h"
#include "i960.h"
#include "memory.h"
#include "irq_timer.h"
#include "hle_hooks.h"
#include "game_profile.h"
#include "savestate_reg.h"
#include "../board/sound.h"

/* Bumped whenever an entry changes meaning. LAYOUT catches a struct that
 * changed size; this catches one that changed what its bytes mean. */
#define SAVESTATE_VERSION 1

/* ---- Hashes ------------------------------------------------------------------ */

static inline uint64_t savestate_fnv64(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    uint64_t h = 0xCBF29CE484222325ull;
    /* eight bytes a step: 48 MB of data ROM is hashed on every save and load */
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, b + i, 8);
        h ^= w;
        h *= 0x100000001B3ull;
    }
    for (; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

/* ---- Entries ------------------------------------------------------------------ */

/* The bus's plain buffers, by m2emulator's name where it has one. */
typedef struct {
    const char *name;
    size_t      off;      /* offset of the array in memory_bus_t, or of the heap pointer */
    size_t      size;
    int         heap;     /* off names a uint8_t * field */
} savestate_buf_t;

#define SS_BUF(n, f)      { n, offsetof(memory_bus_t, f), sizeof(((memory_bus_t *)0)->f), 0 }
#define SS_HEAP(n, f, sz) { n, offsetof(memory_bus_t, f), (sz), 1 }

static const savestate_buf_t SAVESTATE_BUFS[] = {
    SS_BUF("M2RAM",        ram),
    SS_BUF("M2RAM2",       ram2),
    SS_BUF("M2BUFRAM",     buff_ram),
    SS_BUF("M2COPRORAM",   coprogram),
    SS_BUF("M2TILE",       tile),
    SS_BUF("M2CG",         tmapgfx),
    SS_BUF("M2PAL",        palette),
    SS_BUF("M2XLAT",       colorxlat),
    SS_BUF("M2LUMA",       luma),
    SS_BUF("M2LUMA2",      luma2),
    SS_BUF("M2BACK",       back),
    SS_BUF("M2GEO",        geo),
    SS_BUF("M2GEOPROG",    geo_program),
    SS_BUF("M2GEOCMD",     geo_cmd),
    SS_BUF("M2COPROSHARC", copro_sharc),
    SS_BUF("M2COPROCTL",   copro_ctl),
    SS_BUF("M2MIDI",       midi),
    SS_BUF("M2CPUCTRL",    cpu_ctrl),
    SS_BUF("M2IRQ",        irq),
    SS_BUF("M2TIMERS",     timers),
    SS_BUF("M2ZCLIP",      zclip_3d),
    SS_BUF("M2IO",         io),
    SS_BUF("M2SERIAL",     serial),
    SS_BUF("M2UNKVID",     unknown_vid),
    SS_BUF("M2IAC",        iac),
    SS_HEAP("M2VIDEXT",    vid_ext_ram, VID_EXT_RAM_SIZE),
    SS_HEAP("M2TEX0",      texram0,     TEXRAM0_SIZE),
    SS_HEAP("M2TEX1",      texram1,     TEXRAM1_SIZE),
    SS_HEAP("M2FB",        framebuffer, FRAMEBUFFER_SIZE),
};
#define SAVESTATE_NBUFS (sizeof SAVESTATE_BUFS / sizeof SAVESTATE_BUFS[0])

static inline uint8_t *savestate_buf_ptr(memory_bus_t *bus, const savestate_buf_t *b) {
    uint8_t *field = (uint8_t *)bus + b->off;
    if (!b->heap) return field;
    uint8_t *p;
    memcpy(&p, field, sizeof p);
    return p;
}

/* The struct sizes a state was written with. */
typedef struct {
    uint32_t version;
    uint32_t cpu, cop, sharc, zanzou, irqt, m68k, scsp, uart, shle, geo_live, snaps;
} savestate_layout_t;

static inline savestate_layout_t savestate_layout(void) {
    savestate_layout_t l;
    memset(&l, 0, sizeof l);
    l.version  = SAVESTATE_VERSION;
    l.cpu      = (uint32_t)sizeof(i960_cpu_t);
    l.cop      = (uint32_t)sizeof g_cop;
    l.sharc    = (uint32_t)sizeof g_sharc;
    l.zanzou   = (uint32_t)sizeof g_zz;
    l.irqt     = (uint32_t)sizeof g_irqt;
    l.m68k     = (uint32_t)sizeof(m68k_cpu_t);
    l.scsp     = (uint32_t)sizeof(scsp_t);
    l.uart     = (uint32_t)sizeof(sound_uart_t);
    l.shle     = (uint32_t)sizeof g_shle;
    l.geo_live = (uint32_t)sizeof g_geo_live;
    l.snaps    = (uint32_t)sizeof g_geodl_snaps;
    return l;
}

/* What identifies the ROM a state belongs to. */
typedef struct {
    uint64_t program;      /* the program ROM as loaded (bus->rom) */
    uint64_t data;         /* MAIN_DATA + XTRA_DATA */
    uint64_t sound_rom;    /* the 68000's program, 0 with no sound board */
    uint32_t program_size;
    uint32_t sound_loaded;
} savestate_rom_t;

static inline savestate_rom_t savestate_rom_id(const memory_bus_t *bus) {
    savestate_rom_t r;
    memset(&r, 0, sizeof r);
    r.program      = bus->rom ? savestate_fnv64(bus->rom, bus->rom_size) : 0;
    r.program_size = (uint32_t)bus->rom_size;
    r.data         = savestate_fnv64(bus->main_data, MAIN_DATA_SIZE)
                   ^ (savestate_fnv64(bus->xtra_data, XTRA_DATA_SIZE) * 31u);
    r.sound_loaded = g_sound.rom_loaded;
    r.sound_rom    = g_sound.rom_loaded ? savestate_fnv64(g_sound.rom, sizeof g_sound.rom) : 0;
    return r;
}

/* The run loop's own latches (emu_thread.h): what emu_board_reset_state
 * clears, and the step count the netplay frame check hashes. Passed in by the
 * caller, which owns them. */
typedef struct {
    uint64_t total_steps;
    uint64_t timer_cycles_seen;
    uint64_t frame_clock_sample;
    uint64_t frame_clock_frame;
    int32_t  irq_in_service;
    int32_t  irq_baseline_depth;
    int32_t  irq_from_table;
    int32_t  irq_slices;
    int32_t  vblank_edge;
    int32_t  slice_capped;
    uint32_t emu_frames;
    int32_t  irqt_sound_kick;
    int32_t  irqt_vblank;
} savestate_emu_t;

/* The HLE layer's board-side settings and latches (hle_hooks.h). The settings
 * are in because the board was running under them: STF reads the VS mode, the
 * DAMAGE flag and the region mid-game, not only at boot, and the AI table
 * at the start of every fight. */
typedef struct {
    int32_t  versus_result;
    int32_t  match_replay_stage, replay_stage_pin, match_replay;
    uint32_t match_replay_frame;
    int32_t  region, vs_mode, damage_real, rounds_to_win, round_time, game_type, hidden_chars;
    int32_t  enemy_rank;
} savestate_hle_t;

static inline savestate_hle_t savestate_hle_get(void) {
    savestate_hle_t h;
    memset(&h, 0, sizeof h);
    h.versus_result      = g_versus_result;
    h.match_replay_stage = g_match_replay_stage;
    h.replay_stage_pin   = g_replay_stage_pin;
    h.match_replay       = g_match_replay;
    h.match_replay_frame = g_match_replay_frame;
    h.region             = g_region;
    h.vs_mode            = g_vs_mode;
    h.damage_real        = g_damage_real;
    h.rounds_to_win      = g_rounds_to_win;
    h.round_time         = g_round_time;
    h.game_type          = g_game_type;
    h.hidden_chars       = g_hidden_chars;
    h.enemy_rank         = g_enemy_rank;
    return h;
}

static inline void savestate_hle_set(const savestate_hle_t *h) {
    g_versus_result      = h->versus_result;
    g_match_replay_stage = h->match_replay_stage;
    g_replay_stage_pin   = h->replay_stage_pin;
    g_match_replay       = h->match_replay;
    g_match_replay_frame = h->match_replay_frame;
    g_region             = h->region;
    g_vs_mode            = h->vs_mode;
    g_damage_real        = h->damage_real;
    g_rounds_to_win      = h->rounds_to_win;
    g_round_time         = h->round_time;
    g_game_type          = h->game_type;
    g_hidden_chars       = h->hidden_chars;
    g_enemy_rank         = h->enemy_rank;
}

/* The sound board's own fields, beside the 68000 and the SCSP. out_total is
 * the board's sample clock (det_digest and the frame clock read it). */
typedef struct {
    uint32_t     bank4, bank5;
    int32_t      budget;
    uint32_t     slice_frac;
    int32_t      ahead;
    uint32_t     pad;
    uint64_t     irqs[8];
    uint64_t     out_total;
    sound_uart_t uart;
} savestate_sound_t;

/* The GEO's list pointers and the last published list. */
typedef struct {
    uint32_t wstart, rstart;
    uint32_t snap_index, snap_rstart;
    int32_t  snap_ready;
    int32_t  full_snap;
} savestate_geo_t;

/* ---- SCSP LFO pointers ---------------------------------------------------------- */

/* A slot's LFO points at one of eight waveform tables and one row of
 * scsp_lfo_scale. Written as (table + 1, row + 1), 0 for none. */
static inline uint32_t savestate_lfo_table_id(const int32_t *t) {
    const int32_t *const T[8] = { scsp_plfo_saw, scsp_plfo_sqr, scsp_plfo_tri, scsp_plfo_noi,
                                  scsp_alfo_saw, scsp_alfo_sqr, scsp_alfo_tri, scsp_alfo_noi };
    for (uint32_t i = 0; i < 8; i++) if (t == T[i]) return i + 1;
    return 0;
}
static inline const int32_t *savestate_lfo_table(uint32_t id) {
    const int32_t *const T[8] = { scsp_plfo_saw, scsp_plfo_sqr, scsp_plfo_tri, scsp_plfo_noi,
                                  scsp_alfo_saw, scsp_alfo_sqr, scsp_alfo_tri, scsp_alfo_noi };
    return id >= 1 && id <= 8 ? T[id - 1] : NULL;
}
static inline uint32_t savestate_lfo_scale_id(const int32_t *s) {
    for (uint32_t i = 0; i < 16; i++) if (s == scsp_lfo_scale[i]) return i + 1;
    return 0;
}

/* scsp_t with its pointers made into numbers (in a copy), and back. */
static inline void savestate_scsp_pack(scsp_t *s) {
    for (int i = 0; i < 32; i++) {
        scsp_lfo_t *l[2] = { &s->slot[i].plfo, &s->slot[i].alfo };
        for (int k = 0; k < 2; k++) {
            uintptr_t t = savestate_lfo_table_id(l[k]->table);
            uintptr_t c = savestate_lfo_scale_id(l[k]->scale);
            l[k]->table = (const int32_t *)t;
            l[k]->scale = (const int32_t *)c;
        }
    }
    s->clock = NULL; s->ram = NULL; s->sink = NULL; s->sink_ud = NULL;
}

static inline void savestate_scsp_unpack(scsp_t *s, const scsp_t *live) {
    for (int i = 0; i < 32; i++) {
        scsp_lfo_t *l[2] = { &s->slot[i].plfo, &s->slot[i].alfo };
        for (int k = 0; k < 2; k++) {
            uint32_t t = (uint32_t)(uintptr_t)l[k]->table;
            uint32_t c = (uint32_t)(uintptr_t)l[k]->scale;
            l[k]->table = savestate_lfo_table(t);
            l[k]->scale = c >= 1 && c <= 16 ? scsp_lfo_scale[c - 1] : NULL;
        }
    }
    s->clock = live->clock; s->ram = live->ram; s->sink = live->sink; s->sink_ud = live->sink_ud;
}

/* ---- Info ------------------------------------------------------------------------ */

/* The INFO entry: plain text, for a person reading the zip. Nothing is read
 * back from it but the profile id, which is checked. */
static inline void savestate_info_text(char *out, size_t n, uint64_t frame) {
    const game_profile_t *p = g_active_profile;
    snprintf(out, n,
             "m2hle2 savestate\n"
             "version=%d\n"
             "profile=%s\n"
             "rom_set=%s\n"
             "frame=%llu\n",
             SAVESTATE_VERSION,
             p ? p->id : "",
             p ? (p->rom_set ? p->rom_set : p->id) : "",
             (unsigned long long)frame);
}

/* A profile's own entry, while that profile runs (savestate_reg.h). */
static inline bool savestate_extra_mine(int i) {
    return g_active_profile && strcmp(g_savestate_extra[i].profile, g_active_profile->id) == 0;
}

/* ---- Save ------------------------------------------------------------------------- */

/* A stored entry is an in-memory state (savestate_save_mem): it carries a fixed
 * time, so the same board gives the same bytes whenever it is saved. */
static inline bool savestate__add(mz_zip_archive *z, mz_uint level, const char *name, const void *p, size_t n) {
    if (level != MZ_NO_COMPRESSION) return mz_zip_writer_add_mem(z, name, p, n, level) != 0;
    MZ_TIME_T t = 1767225600; /* 2026-01-01 */
    return mz_zip_writer_add_mem_ex_v2(z, name, p, n, NULL, 0, level, 0, 0, &t, NULL, 0, NULL, 0) != 0;
}

/* Every entry, into an archive the caller has started. */
static inline bool savestate__entries(mz_zip_archive *zp, mz_uint level, const i960_cpu_t *cpu,
                                      memory_bus_t *bus, const savestate_emu_t *emu) {
    bool ok = true;

    char info[512];
    savestate_info_text(info, sizeof info, emu->frame_clock_frame);
    savestate_layout_t layout = savestate_layout();
    savestate_rom_t rom = savestate_rom_id(bus);
    ok = ok && savestate__add(zp, level, "INFO",   info, strlen(info));
    ok = ok && savestate__add(zp, level, "LAYOUT", &layout, sizeof layout);
    ok = ok && savestate__add(zp, level, "ROM",    &rom, sizeof rom);

    /* the processors */
    ok = ok && savestate__add(zp, level, "I960",  cpu, sizeof *cpu);
    /* Host pointers are written as 0 (a load keeps the live ones), so the
     * same board makes the same file in any process. */
    if (ok) {
        sharc_state_t *sh = (sharc_state_t *)malloc(sizeof *sh);
        cop_state_t   *co = (cop_state_t *)malloc(sizeof *co);
        if (!sh || !co) ok = false;
        else {
            memcpy(sh, &g_sharc, sizeof *sh);
            sh->sharc_dm_ext = NULL;
            memcpy(co, &g_cop, sizeof *co);
            co->ctl = NULL;
            ok = savestate__add(zp, level, "SHARC", sh, sizeof *sh) && savestate__add(zp, level, "COP", co, sizeof *co);
        }
        free(sh); free(co);
    }
    ok = ok && savestate__add(zp, level, "ZANZOU", &g_zz, sizeof g_zz);

    /* the bus */
    for (size_t i = 0; ok && i < SAVESTATE_NBUFS; i++)
        ok = savestate__add(zp, level, SAVESTATE_BUFS[i].name, savestate_buf_ptr(bus, &SAVESTATE_BUFS[i]),
                            SAVESTATE_BUFS[i].size);

    /* the GEO */
    savestate_geo_t geo;
    memset(&geo, 0, sizeof geo);
    geo.wstart      = g_geo.wstart;
    geo.rstart      = g_geo.rstart;
    geo.snap_index  = g_geodl_snap == g_geodl_snaps[GEO_PUB_COPIES - 1] ? 1u : 0u;
    geo.snap_rstart = g_geodl_snap_rstart;
    geo.snap_ready  = g_geodl_snap_ready;
    geo.full_snap   = g_geodl_full_snap;
    ok = ok && savestate__add(zp, level, "GEO",      &geo, sizeof geo);
    ok = ok && savestate__add(zp, level, "GEOLIST",  g_geodl_snaps, sizeof g_geodl_snaps);
    ok = ok && savestate__add(zp, level, "GEOSTATE", &g_geo_live, sizeof g_geo_live);

    /* interrupts and timers */
    ok = ok && savestate__add(zp, level, "IRQT", &g_irqt, sizeof g_irqt);

    /* the sound board */
    if (ok) {
        savestate_sound_t snd;
        memset(&snd, 0, sizeof snd);
        snd.bank4 = g_sound.bank4;      snd.bank5      = g_sound.bank5;
        snd.budget = g_sound.budget;    snd.slice_frac = g_sound.slice_frac;
        snd.ahead = g_sound.ahead;      snd.out_total  = g_sound.out_total;
        memcpy(snd.irqs, g_sound.irqs, sizeof snd.irqs);
        snd.uart = g_sound.uart;
        scsp_t *scsp = (scsp_t *)malloc(sizeof *scsp);
        if (!scsp) ok = false;
        else {
            memcpy(scsp, &g_sound.scsp, sizeof *scsp);
            savestate_scsp_pack(scsp);
            ok = savestate__add(zp, level, "M68K",     &g_sound.m68k.cpu, sizeof g_sound.m68k.cpu)
              && savestate__add(zp, level, "M2SNDRAM", g_sound.ram, sizeof g_sound.ram)
              && savestate__add(zp, level, "M2SCSP",   scsp, sizeof *scsp)
              && savestate__add(zp, level, "SOUND",    &snd, sizeof snd)
              && savestate__add(zp, level, "SOUNDHLE", &g_shle, sizeof g_shle);
            free(scsp);
        }
    }

    /* the run loop and the HLE layer */
    savestate_hle_t hle = savestate_hle_get();
    ok = ok && savestate__add(zp, level, "EMU", emu, sizeof *emu);
    ok = ok && savestate__add(zp, level, "HLE", &hle, sizeof hle);

    /* the profile's own */
    for (int i = 0; ok && i < g_savestate_extra_n; i++) {
        if (!savestate_extra_mine(i)) continue;
        char name[64];
        snprintf(name, sizeof name, "P.%s", g_savestate_extra[i].name);
        ok = savestate__add(zp, level, name, g_savestate_extra[i].data, g_savestate_extra[i].size);
    }
    return ok;
}

/* Write the board to `path`. Call between slices, with the emu mutex held
 * where there is one. NULL on success, else what went wrong. */
static inline const char *savestate_save(const char *path, const i960_cpu_t *cpu,
                                         memory_bus_t *bus, const savestate_emu_t *emu) {
    if (!path || !*path) return "no path";
    if (!g_active_profile) return "no ROM is loaded";
    sound_settle();

    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (!mz_zip_writer_init_heap(&z, 0, 64u << 20)) return "zip: cannot start an archive";
    bool ok = savestate__entries(&z, MZ_BEST_SPEED, cpu, bus, emu);

    void *zbuf = NULL;
    size_t zlen = 0;
    if (ok) ok = mz_zip_writer_finalize_heap_archive(&z, &zbuf, &zlen) != 0;
    if (!ok) { mz_zip_writer_end(&z); return "zip: could not write an entry"; }

    /* A state is written beside its final name and renamed onto it, so a
     * process killed mid-write (the machine going down is the use case)
     * leaves the previous state whole. */
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    const char *err = NULL;
    if (!f) err = "cannot open the file for writing";
    else {
        if (fwrite(zbuf, 1, zlen, f) != zlen) err = "short write";
        if (fclose(f) != 0 && !err) err = "close failed";
    }
    mz_zip_writer_end(&z);   /* frees zbuf */
    if (!err) {
#ifdef _WIN32
        remove(path);         /* rename does not replace on Windows */
#endif
        if (rename(tmp, path) != 0) err = "cannot rename the file into place";
    }
    if (err) { remove(tmp); return err; }
    LOG_INFO("savestate: saved %s (%zu KB, frame %llu)", path, zlen >> 10,
             (unsigned long long)emu->frame_clock_frame);
    return NULL;
}

/* The same archive into a caller's buffer (the libretro core's
 * retro_serialize). Entries are STORED, not deflated: the frontend asks for a
 * state far more often than a player saves one to disk (rewind, run-ahead),
 * compresses the files it writes itself, and a stored archive's size depends
 * only on the build and the profile, which retro_serialize_size has to give
 * ahead of time. With `buf` NULL nothing is written and `*len` is the size. */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   end;
    bool     over;
} savestate_mem_sink_t;

static inline size_t savestate__mem_write(void *opaque, mz_uint64 ofs, const void *p, size_t n) {
    savestate_mem_sink_t *m = (savestate_mem_sink_t *)opaque;
    if (m->buf) {
        if (ofs + n > m->cap) { m->over = true; return 0; }
        memcpy(m->buf + ofs, p, n);
    }
    if (ofs + n > m->end) m->end = (size_t)(ofs + n);
    return n;
}

static inline const char *savestate_save_mem(void *buf, size_t cap, size_t *len, const i960_cpu_t *cpu,
                                             memory_bus_t *bus, const savestate_emu_t *emu) {
    if (!g_active_profile) return "no ROM is loaded";
    sound_settle();
    savestate_mem_sink_t m = { (uint8_t *)buf, cap, 0, false };
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    z.m_pWrite     = savestate__mem_write;
    z.m_pIO_opaque = &m;
    if (!mz_zip_writer_init(&z, 0)) return "zip: cannot start an archive";
    bool ok = savestate__entries(&z, MZ_NO_COMPRESSION, cpu, bus, emu)
           && mz_zip_writer_finalize_archive(&z);
    mz_zip_writer_end(&z);
    if (m.over) return "the state is larger than the buffer";
    if (!ok) return "zip: could not write an entry";
    if (len) *len = m.end;
    return NULL;
}

/* ---- Load ------------------------------------------------------------------------- */

/* One entry, which must be exactly `n` bytes, into `dst`. */
static inline bool savestate__get(mz_zip_archive *z, const char *name, void *dst, size_t n) {
    int i = mz_zip_reader_locate_file(z, name, NULL, 0);
    if (i < 0) { LOG_WARN("savestate: no entry %s", name); return false; }
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(z, (mz_uint)i, &st) || st.m_uncomp_size != n) {
        LOG_WARN("savestate: entry %s is %llu bytes, wanted %zu", name,
                 (unsigned long long)st.m_uncomp_size, n);
        return false;
    }
    return mz_zip_reader_extract_to_mem(z, (mz_uint)i, dst, n, 0) != 0;
}

/* Read the board from `path`. Call between slices, with the emu mutex held
 * where there is one, over the same ROM set and profile the state was saved
 * from. Nothing is touched until every entry has been read and checked. On
 * success `emu` holds the run loop's latches for the caller to put back. */
static inline const char *savestate__load_zip(mz_zip_archive *zp, const char *what, i960_cpu_t *cpu,
                                              memory_bus_t *bus, savestate_emu_t *emu) {
    const char *err = NULL;
    char info[512];
    memset(info, 0, sizeof info);
    savestate_layout_t layout, want = savestate_layout();
    savestate_rom_t rom, have_rom;
    {
        int i = mz_zip_reader_locate_file(zp, "INFO", NULL, 0);
        size_t n = 0;
        void *p = i >= 0 ? mz_zip_reader_extract_to_heap(zp, (mz_uint)i, &n, 0) : NULL;
        if (!p) err = "no INFO entry: not an m2hle2 state";
        else { memcpy(info, p, n < sizeof info - 1 ? n : sizeof info - 1); mz_free(p); }
    }
    if (!err) {
        char want_id[96];
        snprintf(want_id, sizeof want_id, "\nprofile=%s\n", g_active_profile->id);
        if (!strstr(info, want_id)) err = "the state is from another profile";
    }
    if (!err && !savestate__get(zp, "LAYOUT", &layout, sizeof layout)) err = "no LAYOUT entry";
    if (!err && layout.version != SAVESTATE_VERSION) err = "the state is from another savestate version";
    if (!err && memcmp(&layout, &want, sizeof layout) != 0) err = "the state is from a build with another layout";
    if (!err && !savestate__get(zp, "ROM", &rom, sizeof rom)) err = "no ROM entry";
    if (!err) {
        have_rom = savestate_rom_id(bus);
        if (rom.program != have_rom.program || rom.program_size != have_rom.program_size)
            err = "the state is from another program ROM";
        else if (rom.data != have_rom.data)
            err = "the state's data ROM differs from the one loaded";
        else if (rom.sound_loaded != have_rom.sound_loaded || rom.sound_rom != have_rom.sound_rom)
            err = "the state's sound board differs from this one";
    }

    /* Everything into scratch first: a state that fails halfway must leave
     * the running board as it was. */
    i960_cpu_t     *n_cpu   = NULL;
    sharc_state_t  *n_sharc = NULL;
    cop_state_t    *n_cop   = NULL;
    zanzou_stream_t n_zz;
    irq_timer_t     n_irqt;
    savestate_geo_t n_geo;
    void           *n_snaps = NULL;
    geo_raster_state_t *n_glive = NULL;
    uint8_t        *n_bufs[SAVESTATE_NBUFS];
    m68k_cpu_t      n_m68k;
    uint8_t        *n_sram = NULL;
    scsp_t         *n_scsp = NULL;
    savestate_sound_t n_snd;
    shle_t          n_shle;
    savestate_emu_t n_emu;
    savestate_hle_t n_hle;
    uint8_t        *n_extra[SAVESTATE_EXTRA_MAX];
    memset(n_bufs, 0, sizeof n_bufs);
    memset(n_extra, 0, sizeof n_extra);

    if (!err) {
        n_cpu   = (i960_cpu_t *)malloc(sizeof *n_cpu);
        n_sharc = (sharc_state_t *)malloc(sizeof *n_sharc);
        n_cop   = (cop_state_t *)malloc(sizeof *n_cop);
        n_snaps = malloc(sizeof g_geodl_snaps);
        n_glive = (geo_raster_state_t *)malloc(sizeof *n_glive);
        n_sram  = (uint8_t *)malloc(sizeof g_sound.ram);
        n_scsp  = (scsp_t *)malloc(sizeof *n_scsp);
        if (!n_cpu || !n_sharc || !n_cop || !n_snaps || !n_glive || !n_sram || !n_scsp) err = "out of memory";
        for (size_t i = 0; !err && i < SAVESTATE_NBUFS; i++)
            if (!(n_bufs[i] = (uint8_t *)malloc(SAVESTATE_BUFS[i].size))) err = "out of memory";
        for (int i = 0; !err && i < g_savestate_extra_n; i++)
            if (savestate_extra_mine(i) && !(n_extra[i] = (uint8_t *)malloc(g_savestate_extra[i].size))) err = "out of memory";
    }
    if (!err) {
        bool ok = savestate__get(zp, "I960",   n_cpu,   sizeof *n_cpu)
               && savestate__get(zp, "SHARC",  n_sharc, sizeof *n_sharc)
               && savestate__get(zp, "COP",    n_cop,   sizeof *n_cop)
               && savestate__get(zp, "ZANZOU", &n_zz,   sizeof n_zz)
               && savestate__get(zp, "IRQT",   &n_irqt, sizeof n_irqt)
               && savestate__get(zp, "GEO",    &n_geo,  sizeof n_geo)
               && savestate__get(zp, "GEOLIST",  n_snaps, sizeof g_geodl_snaps)
               && savestate__get(zp, "GEOSTATE", n_glive, sizeof *n_glive)
               && savestate__get(zp, "M68K",     &n_m68k, sizeof n_m68k)
               && savestate__get(zp, "M2SNDRAM", n_sram,  sizeof g_sound.ram)
               && savestate__get(zp, "M2SCSP",   n_scsp,  sizeof *n_scsp)
               && savestate__get(zp, "SOUND",    &n_snd,  sizeof n_snd)
               && savestate__get(zp, "SOUNDHLE", &n_shle, sizeof n_shle)
               && savestate__get(zp, "EMU",      &n_emu,  sizeof n_emu)
               && savestate__get(zp, "HLE",      &n_hle,  sizeof n_hle);
        for (size_t i = 0; ok && i < SAVESTATE_NBUFS; i++)
            ok = savestate__get(zp, SAVESTATE_BUFS[i].name, n_bufs[i], SAVESTATE_BUFS[i].size);
        for (int i = 0; ok && i < g_savestate_extra_n; i++) {
            if (!savestate_extra_mine(i)) continue;
            char name[64];
            snprintf(name, sizeof name, "P.%s", g_savestate_extra[i].name);
            ok = savestate__get(zp, name, n_extra[i], g_savestate_extra[i].size);
        }
        if (!ok) err = "an entry is missing or the wrong size (see the log)";
    }
    if (!err) {
        /* ---- the commit: nothing below can fail ---- */
        *cpu = *n_cpu;

        uint8_t *dm_ext = g_sharc.sharc_dm_ext;
        uint32_t dm_ext_size = g_sharc.sharc_dm_ext_size;
        g_sharc = *n_sharc;
        g_sharc.sharc_dm_ext      = dm_ext;
        g_sharc.sharc_dm_ext_size = dm_ext_size;

        const uint8_t *ctl = g_cop.ctl;
        memcpy(&g_cop, n_cop, sizeof g_cop);
        g_cop.ctl = ctl;
        g_zz   = n_zz;
        g_irqt = n_irqt;

        for (size_t i = 0; i < SAVESTATE_NBUFS; i++)
            memcpy(savestate_buf_ptr(bus, &SAVESTATE_BUFS[i]), n_bufs[i], SAVESTATE_BUFS[i].size);

        /* GEO: the list pointers, the last list published and the state the
         * lists so far left behind. Both published copies take the live one,
         * so the renderer's next frame draws from what the board has. */
        g_geo.wstart = n_geo.wstart;
        g_geo.rstart = n_geo.rstart;
        memcpy(g_geodl_snaps, n_snaps, sizeof g_geodl_snaps);
        memcpy(&g_geo_live, n_glive, sizeof g_geo_live);
        memcpy(&g_geo_pub[0], &g_geo_live, sizeof g_geo_live);
        memcpy(&g_geo_pub[GEO_PUB_COPIES - 1], &g_geo_live, sizeof g_geo_live);
        memset(g_geo_dirty, 0, sizeof g_geo_dirty);
        g_geodl_snap        = g_geodl_snaps[n_geo.snap_index ? GEO_PUB_COPIES - 1 : 0];
        g_geo_rs            = &g_geo_pub[n_geo.snap_index ? GEO_PUB_COPIES - 1 : 0];
        g_geodl_snap_rstart = n_geo.snap_rstart;
        g_geodl_full_snap   = n_geo.full_snap != 0;
        g_geodl_snap_ready  = n_geo.snap_ready;
        g_geodl_snap_seq++;

        /* Every cache keyed on the bus's contents starts over. */
        bus->gen_tile += 1u << 20; bus->gen_gfx += 1u << 20; bus->gen_pal += 1u << 20;
        bus->gen_tex  += 1u << 20; bus->gen_lut += 1u << 20;
        memset((uint8_t *)bus->tex_dirty, 1, sizeof bus->tex_dirty);

        /* the sound board: the 68000's registers, its RAM, the chip with its
         * live pointers, and the board's own fields. The host ring and what
         * reads it stay as they are; the sample clock is the board's. */
        g_sound.m68k.cpu = n_m68k;
        memcpy(g_sound.ram, n_sram, sizeof g_sound.ram);
        savestate_scsp_unpack(n_scsp, &g_sound.scsp);
        memcpy(&g_sound.scsp, n_scsp, sizeof g_sound.scsp);
        g_sound.bank4      = n_snd.bank4;
        g_sound.bank5      = n_snd.bank5;
        g_sound.budget     = n_snd.budget;
        g_sound.slice_frac = n_snd.slice_frac;
        g_sound.ahead      = n_snd.ahead;
        memcpy(g_sound.irqs, n_snd.irqs, sizeof g_sound.irqs);
        g_sound.uart       = n_snd.uart;
        g_sound.out_total  = n_snd.out_total;
        g_sound.out_due    = n_snd.out_total;
        g_shle = n_shle;
        sound_map_pages(&g_sound);   /* the 68000's direct reads leave out the DSP's range */

        savestate_hle_set(&n_hle);
        for (int i = 0; i < g_savestate_extra_n; i++)
            if (savestate_extra_mine(i)) memcpy(g_savestate_extra[i].data, n_extra[i], g_savestate_extra[i].size);
        *emu = n_emu;
        LOG_INFO("savestate: loaded %s (frame %llu)", what, (unsigned long long)n_emu.frame_clock_frame);
    }

    free(n_cpu); free(n_sharc); free(n_cop); free(n_snaps); free(n_glive); free(n_sram); free(n_scsp);
    for (size_t i = 0; i < SAVESTATE_NBUFS; i++) free(n_bufs[i]);
    for (int i = 0; i < SAVESTATE_EXTRA_MAX; i++) free(n_extra[i]);
    if (err) LOG_WARN("savestate: %s: %s", what, err);
    return err;
}

static inline const char *savestate_load(const char *path, i960_cpu_t *cpu,
                                         memory_bus_t *bus, savestate_emu_t *emu) {
    if (!path || !*path) return "no path";
    if (!g_active_profile) return "no ROM is loaded";
    sound_settle();
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_file(&z, path, 0)) return "cannot open the state (not a zip?)";
    const char *err = savestate__load_zip(&z, path, cpu, bus, emu);
    mz_zip_reader_end(&z);
    return err;
}

/* A state in memory (retro_unserialize). Bytes past the archive's end are
 * allowed: the frontend hands back the whole buffer it sized, padding too. */
static inline const char *savestate_load_mem(const void *data, size_t size, i960_cpu_t *cpu,
                                             memory_bus_t *bus, savestate_emu_t *emu) {
    if (!data || !size) return "no state";
    if (!g_active_profile) return "no ROM is loaded";
    sound_settle();
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_mem(&z, data, size, 0)) return "not a state (not a zip?)";
    const char *err = savestate__load_zip(&z, "(memory)", cpu, bus, emu);
    mz_zip_reader_end(&z);
    return err;
}

#endif /* M2HLE_SAVESTATE_H */
