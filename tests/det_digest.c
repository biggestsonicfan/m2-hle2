/*
 * det_digest.c -- one line per game frame saying what the board computed, so two
 * builds can be held against each other. The question it answers is the
 * cross-play gate (WEB-NETPLAY.md section 3): does the WebAssembly build compute
 * the same frames as the desktop one? Lockstep netplay sends inputs, not state,
 * so the two must agree to the bit.
 *
 * It runs the slice both hosts run (emu_slice_body / emu_slice_finish from
 * emu_thread.h: the i960, the COP, the timers and the sound board) with no
 * window, no GPU and no clock, from the same one-zip load the web build uses.
 * Inputs come from a script keyed to game frames, so every run of a script is
 * the same run.
 *
 *   det_digest <merged sfight zip> [--frames N] [--script "449:c,460:,..."]
 *              [--from F] [--out FILE] [--cop FROM:TO:FILE] [--trace F:FILE]
 *
 * Script keys are the web build's ?script= (main_web.c): at frame N hold exactly
 * these; u d l r, 1-4 the buttons, s start, c coin; an uppercase letter is the
 * same control on player 2.
 *
 * Each line: frame, the netplay frame check (what two peers compare), then FNV-1a
 * of work RAM (both banks), buffer RAM, and the COP's data memory. The check
 * alone already fails on the first frame that differs; the rest say where.
 *
 * --cop FROM:TO:FILE writes every word of the COP conversation during those
 * frames (g_cop_tap: command/argument words in, replies out), one per line with
 * its frame, so the first differing reply names the command that split.
 *
 * --inputs FILE replays a netplay session's input log (netplay.h, "The session
 * input log"): each frame gets the two players' words the session ran on, and
 * the check the session logged for it is held against the replay's. Frame 0 of
 * a session is the first frame after the barrier's cold boot, which is this
 * program's first frame. The first frame whose check differs is where the board
 * that wrote the log stopped computing what the inputs say; replay the other
 * player's log too, and compare the two logs' words, to know which board it was
 * and whether they were ever fed the same inputs. --frames defaults to the log.
 *
 * --trace F:FILE writes one line per i960 instruction of game frame F: IP and a
 * hash of the registers (globals, locals, AC, the FP registers). The first line
 * that differs is the instruction that computed something different. The traced
 * frame runs through trace_slice, a copy of emu_slice_body with the line added,
 * so keep the two in step.
 *
 * --sound adds the sound board to each line: a hash of sound RAM and the
 * sample clock. Reading it waits for the sound thread (sound.h), so the run
 * then has no overlap to test; without it the sound thread runs as in a host,
 * and the last line on stderr hashes every sample the board produced, which
 * holds the overlapped run whole. --no-sound-thread (or M2HLE_SOUND_THREAD=0)
 * keeps the sound board on this thread, for the A/B. --sound-hle runs the
 * sound driver in C instead of on the 68000 (sound_hle.h): the i960 columns
 * are the same frame for frame if the i960 cannot tell. --pcm FILE writes
 * every sample the board produced (16-bit stereo, 44.1 kHz, raw).
 *
 * --raw RANGES:FILE writes, at every game frame edge (the frame hook, the
 * instruction tools/mame/boot-lockstep.lua taps on MAME), a record of the board's
 * memory: "M2BF", frame_counter, mode, sub-mode, 0, the IP, then the bytes of
 * each range ("hexaddr:hexlen,...") in address order. The two files line up
 * record for record, for tools/dc-lockstep.py --boot.
 *
 * --region japan|usa|export powers up in that region (USA by default, as the
 * emulator does); --nowarnskip leaves the Japan warning screen in, ~640 game
 * frames, as MAME does. --peek HEXADDR adds that byte to each line (the mode
 * at 50002A, the sub-mode at 500030), to pair frames with a MAME log.
 *
 * --model-map F0:F1:FILE decodes every display list of frames F0..F1 as the
 * Dreamcast's renderer does, uncut, and writes each run of 64-byte lines of
 * the polygon and texture ROMs it read with the frame that first read it
 * (dreamcast/sfight.mdlmap, tools/dc_mdlpack.py).
 *
 * --gems runs Sega's C from Sonic Gems Collection (core/gems.h, both
 * --gems-i960 and --gems-cop), in a build configured with it, as the
 * Dreamcast build does by default: record its --aot-map with this.
 *
 * Two builds of it, one from each configuration, so each has exactly its
 * frontend's compiler flags:
 *   native  --target det_digest in a desktop build tree (not a ctest: it needs a ROM)
 *   wasm    --target det_digest in the web build tree; run as node det_digest.js
 * Then diff the two outputs; tools/README.md, "Cross-play".
 */
#define NDEBUG 1
#include "net/netplay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "log.h"
#include "memory.h"
#include "i960.h"
#include "i960_exec.h"
#include "breakpoint.h"
#include "watchpoint.h"
#include "rom_loader.h"
#include "emu_thread.h"
#include "sound.h"
#include "input.h"
#include "registry.h"

/* --model-map: the 3D decoder's ROM reads come through mdlmap_rom. */
static const uint8_t *mdlmap_rom(const void *p, uint32_t n);
#define GEO3D_ROM(p, n) mdlmap_rom((p), (n))
#include "geo3d.h"
#include "gems.h"
#include "../dreamcast/dc_layout.h"
#define DCS_WRITER
#include "../dreamcast/dc_strips.h"
#include "../dreamcast/dc_texpak.h"

#include <ctype.h>
#include <sys/stat.h>

static memory_bus_t     bus;
static i960_cpu_t       cpu;
static emu_thread_ctx_t emu;
static romset_t         romset;

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}
#define FNV0 0xcbf29ce484222325ull

/* Every sample the board makes, folded in as it is made (on the sound thread). */
static uint64_t snd_out_hash = FNV0, snd_out_n;
static FILE *snd_pcm;          /* --pcm FILE */
static void snd_out_tap(int16_t l, int16_t r, uint64_t index, void *ud) {
    (void)index; (void)ud;
    int16_t lr[2] = { l, r };
    snd_out_hash = fnv(snd_out_hash, lr, sizeof lr);
    if (snd_pcm) fwrite(lr, sizeof lr, 1, snd_pcm);
    snd_out_n++;
}

#define SCRIPT_MAX 1024
static struct { uint32_t frame, held; } script[SCRIPT_MAX];
static int script_n;

/* --raw: the board's memory at each game frame edge, as boot-lockstep.lua writes MAME's. */
static FILE        *raw_out;
static uint32_t     raw_rng[16][2];
static int          raw_n;
static memory_bus_t *raw_bus;
static i960_cpu_t   *raw_cpu;

static void raw_put32(uint32_t v) { uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; fwrite(b, 1, 4, raw_out); }

static void raw_frame(memory_bus_t *bus) {
    (void)bus;
    fwrite("M2BF", 1, 4, raw_out);
    raw_put32(mem_read32(raw_bus, 0x500020));
    uint8_t ms[4] = { mem_read8(raw_bus, 0x50002A), mem_read8(raw_bus, 0x500030), 0, 0 };
    fwrite(ms, 1, 4, raw_out);
    raw_put32(raw_cpu->sfr.ip);
    static uint8_t buf[0x100000];
    for (int i = 0; i < raw_n; i++)
        for (uint32_t o = 0; o < raw_rng[i][1]; o += sizeof buf) {
            uint32_t n = raw_rng[i][1] - o < sizeof buf ? raw_rng[i][1] - o : (uint32_t)sizeof buf;
            for (uint32_t k = 0; k < n; k += 4) {
                uint32_t v = mem_read32(raw_bus, raw_rng[i][0] + o + k);
                buf[k] = (uint8_t)v; buf[k + 1] = (uint8_t)(v >> 8); buf[k + 2] = (uint8_t)(v >> 16); buf[k + 3] = (uint8_t)(v >> 24);
            }
            fwrite(buf, 1, n, raw_out);
        }
}

/* --save-at F:FILE writes a savestate at the end of frame F; --load FILE starts
 * from one. A loaded run prints what the saving run would have printed after F,
 * line for line, and its sample hash (the last lines) covers the samples after
 * F in both. With --mem the state goes through memory as the libretro core's
 * retro_serialize / retro_unserialize take it (stored, padded to its size),
 * and the file holds that buffer. */
static const char *load_path;
static uint32_t    save_frame;
static char        save_path[1024];
static bool        state_mem;

static const char *mem_state_save(emu_thread_ctx_t *emu, const char *path) {
    size_t size = 0;
    const char *err = emu_state_save_mem(emu, NULL, 0, &size);
    if (err) return err;
    size += 4096;   /* as main_libretro.c's LR_STATE_SLACK */
    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) return "out of memory";
    size_t len = 0;
    err = emu_state_save_mem(emu, buf, size, &len);
    if (!err) {
        memset(buf + len, 0, size - len);
        FILE *f = fopen(path, "wb");
        if (!f || fwrite(buf, 1, size, f) != size) err = "cannot write the file";
        if (f) fclose(f);
    }
    free(buf);
    return err;
}

static const char *mem_state_load(emu_thread_ctx_t *emu, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return "cannot open the file";
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(size > 0 ? (size_t)size : 1);
    const char *err = NULL;
    if (!buf || size <= 0 || fread(buf, 1, (size_t)size, f) != (size_t)size) err = "cannot read the file";
    fclose(f);
    if (!err) err = emu_state_load_mem(emu, buf, (size_t)size);
    free(buf);
    return err;
}

static uint32_t keys_mask(const char *p, const char *end) {
    const game_input_map_t *in = &g_active_profile->input;
    uint32_t m = 0;
    for (; p < end; p++) {
        switch (*p) {
            case 'u': m |= in->bits[GAME_INPUT_P1_UP];    break;
            case 'd': m |= in->bits[GAME_INPUT_P1_DOWN];  break;
            case 'l': m |= in->bits[GAME_INPUT_P1_LEFT];  break;
            case 'r': m |= in->bits[GAME_INPUT_P1_RIGHT]; break;
            case '1': m |= in->bits[GAME_INPUT_P1_B1];    break;
            case '2': m |= in->bits[GAME_INPUT_P1_B2];    break;
            case '3': m |= in->bits[GAME_INPUT_P1_B3];    break;
            case '4': m |= in->bits[GAME_INPUT_P1_B4];    break;
            case 's': m |= in->bits[GAME_INPUT_P1_START]; break;
            case 'c': m |= in->bits[GAME_INPUT_P1_COIN];    break;
            case 'U': m |= in->bits[GAME_INPUT_P2_UP];    break;
            case 'D': m |= in->bits[GAME_INPUT_P2_DOWN];  break;
            case 'L': m |= in->bits[GAME_INPUT_P2_LEFT];  break;
            case 'R': m |= in->bits[GAME_INPUT_P2_RIGHT]; break;
            case '!': m |= in->bits[GAME_INPUT_P2_B1];    break;
            case '@': m |= in->bits[GAME_INPUT_P2_B2];    break;
            case '#': m |= in->bits[GAME_INPUT_P2_B3];    break;
            case '$': m |= in->bits[GAME_INPUT_P2_B4];    break;
            case 'S': m |= in->bits[GAME_INPUT_P2_START]; break;
            case 'C': m |= in->bits[GAME_INPUT_P2_COIN];    break;
            default: break;
        }
    }
    return m;
}

static FILE    *cop_out;
static uint32_t cop_from, cop_to;
static void cop_tap(uint32_t tag, uint32_t val) {
    if (g_emu_frames + 1 >= cop_from && g_emu_frames + 1 <= cop_to)
        fprintf(cop_out, "%u %08x %08x\n", (unsigned)g_emu_frames + 1, tag, val);
}

static FILE    *trace_out;
static uint32_t trace_frame;

/* emu_slice_body (emu_thread.h), with a line per instruction. */
static void trace_slice(emu_thread_ctx_t *ctx) {
    g_vblank_edge = 0;
    emu_timers_slice_begin(ctx);
    emu_service_irq(ctx);
    uint64_t steps = 0;
    for (int i = 0; i < g_emu_steps_per_slice && !ctx->request_stop && !ctx->cpu->halted; i++) {
        if (g_irqt_vblank) { g_irqt_vblank = 0; g_vblank_edge = 1; break; }
        if (ctx->step_over_bp) ctx->step_over_bp = 0;
        else if (bp_check(ctx->cpu->sfr.ip)) break;
        uint32_t ip = ctx->cpu->sfr.ip;
        if (i960_step_hot(ctx->cpu, ctx->bus) != 0) break;
        ctx->total_steps++;
        steps++;
        const i960_cpu_t *c = ctx->cpu;
        uint64_t h = fnv(FNV0, c->globals.g, sizeof c->globals.g);
        h = fnv(h, c->locals.r, sizeof c->locals.r);
        h = fnv(h, &c->sfr.ac, sizeof c->sfr.ac);
        h = fnv(h, c->fp_regs, sizeof c->fp_regs);
        fprintf(trace_out, "%08x %016llx\n", ip, (unsigned long long)h);
        if (g_hle_extra) { ctx->total_steps += g_hle_extra; steps += g_hle_extra; i += (int)g_hle_extra; g_hle_extra = 0; }
        if (s_irq_in_service && g_active_profile) emu_service_sound_again(ctx);
        else if (g_irqt_sound_kick && g_active_profile) { g_irqt_sound_kick = 0; emu_offer_sound(ctx); }
        emu_timers_after_step(ctx);
        if (g_log.warn_triggered) break;
        if (g_wp.hit) break;
        if (g_sharc.unknown_triggered) break;
    }
    bool frame = g_vblank_edge != 0;
    if (frame) {
        if (g_active_profile->quirks.board_vblank) { cop_geo_frame_edge(); dl_frame_edge(ctx->bus, g_emu_frames); hle_match_replay_edge(ctx->bus); }
    }
    emu_sound_slice_end(frame);
    if (g_active_profile->quirks.geo_displaylist) geodl_capture(ctx->bus);
    ctx->cpu_prev_snapshot = ctx->cpu_snapshot;
    ctx->cpu_snapshot      = *ctx->cpu;
}

/* --aot-map F0:F1:FILE: the code those game frames run, for the static
 * recompiler (tools/i960_aot.py). One line per program-ROM instruction run:
 * its IP, its two words, how often it ran and whether it was ever reached
 * other than from the instruction before it (a branch, call, return or
 * interrupt landing there). Then a line per hook of the profile. Stepped as
 * trace_slice does. */
#define AOTMAP_ROM 0x100000u
static FILE     *aotmap_out;
static char      aotmap_path[1024];
static uint32_t  aotmap_from, aotmap_to;
static uint32_t *aotmap_hits;
static uint8_t  *aotmap_jump;

static uint32_t aotmap_len(uint32_t w1) {
    uint32_t cls = w1 >> 28, mode = MEM_MODE(w1);
    return cls >= 8 && cls <= 0xC && (mode == 5u || mode >= 0xCu) ? 8u : 4u;
}

static void aotmap_slice(emu_thread_ctx_t *ctx) {
    static uint32_t prev = 1, prev_len;
    if (!aotmap_hits) { aotmap_hits = calloc(AOTMAP_ROM / 4, 4); aotmap_jump = calloc(AOTMAP_ROM / 4, 1); }
    g_vblank_edge = 0;
    emu_timers_slice_begin(ctx);
    emu_service_irq(ctx);
    for (int i = 0; i < g_emu_steps_per_slice && !ctx->request_stop && !ctx->cpu->halted; i++) {
        if (g_irqt_vblank) { g_irqt_vblank = 0; g_vblank_edge = 1; break; }
        if (ctx->step_over_bp) ctx->step_over_bp = 0;
        else if (bp_check(ctx->cpu->sfr.ip)) break;
        uint32_t ip = ctx->cpu->sfr.ip;
        if (ip < AOTMAP_ROM) {
            aotmap_hits[ip >> 2]++;
            if (ip != prev + prev_len) aotmap_jump[ip >> 2] = 1;
            prev = ip; prev_len = aotmap_len(mem_read32(ctx->bus, ip));
        } else prev = 1;
        if (i960_step_hot(ctx->cpu, ctx->bus) != 0) break;
        ctx->total_steps++;
        if (g_hle_extra) { ctx->total_steps += g_hle_extra; i += (int)g_hle_extra; g_hle_extra = 0; prev = 1; }
        if (s_irq_in_service && g_active_profile) emu_service_sound_again(ctx);
        else if (g_irqt_sound_kick && g_active_profile) { g_irqt_sound_kick = 0; emu_offer_sound(ctx); }
        emu_timers_after_step(ctx);
        if (g_log.warn_triggered) break;
        if (g_wp.hit) break;
        if (g_sharc.unknown_triggered) break;
    }
    bool frame = g_vblank_edge != 0;
    if (frame) {
        if (g_active_profile->quirks.board_vblank) { cop_geo_frame_edge(); dl_frame_edge(ctx->bus, g_emu_frames); hle_match_replay_edge(ctx->bus); }
    }
    emu_sound_slice_end(frame);
    if (g_active_profile->quirks.geo_displaylist) geodl_capture(ctx->bus);
    ctx->cpu_prev_snapshot = ctx->cpu_snapshot;
    ctx->cpu_snapshot      = *ctx->cpu;
}

static void aotmap_write(memory_bus_t *b, const char *path) {
    /* The code as the i960 reads it, for the generator (a ROM image: scratch only). */
    char rp[1040];
    snprintf(rp, sizeof rp, "%s.rom", path);
    FILE *f = fopen(rp, "wb");
    for (uint32_t a = 0; f && a < AOTMAP_ROM; a += 4) { uint32_t v = mem_read32(b, a); fwrite(&v, 4, 1, f); }
    if (f) fclose(f);
    for (uint32_t k = 0; aotmap_hits && k < AOTMAP_ROM / 4; k++)
        if (aotmap_hits[k])
            fprintf(aotmap_out, "%08x %08x %08x %u %u\n", k * 4u, mem_read32(b, k * 4u), mem_read32(b, k * 4u + 4u),
                    aotmap_hits[k], aotmap_jump[k]);
    for (size_t i = 0; i < g_active_profile->hook_count; i++)
        fprintf(aotmap_out, "hook %08x\n", g_active_profile->hooks[i].addr);
    for (size_t i = 0; g_active_profile == g_hle_spin_profile && i < g_hle_spin_count; i++)
        fprintf(aotmap_out, "hook %08x\n", g_hle_spin_sites[i]);
}

/* --model-map F0:F1:FILE: the polygon and texture ROM bytes the 3D decoder
 * reads for those game frames' display lists, for the Dreamcast's model pack
 * (tools/dc_mdlpack.py). Each list is decoded as dc_pvr.h's dp_decode does,
 * with nothing culled, so every face's mesh and UV words are read. Then a line
 * per run of 64-byte lines first read in the same frame: region (po, tx),
 * offset, length, that frame. Addresses only. */
#define MDLMAP_LINE 64u
static FILE     *mdlmap_out;
static uint32_t  mdlmap_from, mdlmap_to;
static uint32_t *mdlmap_first[2];      /* polygons, textures: per line, the first frame + 1 */
static int       mdlmap_on;

static const uint8_t *mdlmap_rom(const void *p, uint32_t n) {
    const uint8_t *b = (const uint8_t *)p;
    if (!mdlmap_on) return b;
    const uint8_t *base[2] = { romset.polygons, romset.textures };
    size_t size[2] = { romset.polygons_size, romset.textures_size };
    for (int r = 0; r < 2; r++) {
        if (!base[r] || b < base[r] || b >= base[r] + size[r]) continue;
        uint32_t o = (uint32_t)(b - base[r]), e = o + (n ? n : 1u) - 1u;
        for (uint32_t l = o / MDLMAP_LINE; l <= e / MDLMAP_LINE && l < size[r] / MDLMAP_LINE; l++)
            if (!mdlmap_first[r][l]) mdlmap_first[r][l] = g_emu_frames + 1u;
    }
    return b;
}

static void mdlmap_frame(void) {
    static geo3d_state_t geo;
    const game_quirks_t *q = &g_active_profile->quirks;
    if (!g_geodl_snap_ready || !romset.main_data || !romset.polygons) return;
    for (int r = 0; r < 2; r++)
        if (!mdlmap_first[r]) mdlmap_first[r] = calloc((r ? romset.textures_size : romset.polygons_size) / MDLMAP_LINE + 1, 4);
    const uint32_t *snap = g_geodl_snap;
    if (!geo3d_scan_geo_list(&geo, snap, BUFF_RAM_SIZE / 4, g_geodl_snap_rstart,
                             (int16_t)mem_read16(&bus, H_SYNC_BASE), (int16_t)mem_read16(&bus, V_SYNC_BASE),
                             romset.main_data, romset.main_data_size, q->model_table_offset, q->model_table_count)) return;
    g_geo_rs = geodl_raster_for(snap);
    g_geo3d_palram = bus.palette;
    g_geo3d_palram_size = PALETTE_SIZE;
    g_geo3d_mesh_epoch++;
    mdlmap_on = 1;
    const geo3d_models_t md = {
        .main_data = romset.main_data, .main_data_size = romset.main_data_size,
        .polygons  = romset.polygons,  .polygons_size  = romset.polygons_size,
        .materials = romset.textures,  .materials_size = romset.textures_size,
        .table_off = q->model_table_offset, .table_count = q->model_table_count,
        .mesh_ptr_subtract = q->mesh_ptr_subtract, .mesh_ptr_add = q->mesh_ptr_add,
    };
    for (int k = 0; k < geo.captured_count; k++) {
        const captured_model_t *cm = &geo.captured[k];
        if (cm->direct_len || cm->model_idx < 0) continue;      /* polygon RAM: not ROM */
        geo3d_tris_reset();
        geo3d_lines_reset();
        g_geo3d_obj_tpa = cm->tpa;
        g_geo3d_obj_tha = cm->tha;
        g_geo3d_mode = cm->geo_mode;
        g_geo3d_zadjust = cm->zadjust;
        g_geo3d_lod = cm->geo_lod;
        geo3d_decode_model_cached(&md, cm->model_idx, cm->matrix, cm->color[0], cm->color[1], cm->color[2]);
        g_geo3d_obj_tpa = g_geo3d_obj_tha = 0xFFFFFFFFu;
    }
    mdlmap_on = 0;
    g_geo3d_palram = NULL;
}

/* --strip-keys F0:F1:FILE: the mesh cache's keys (model, material and UV
 * pointers, geo3d_mesh_for_draw's) those frames' display lists draw from ROM,
 * each with the frame that first draws it, for the Dreamcast's strip pack
 * (tools/dc_strips.c). Addresses only. */
#define SPKEY_SLOTS 65536u
static FILE     *spkey_out;
static uint32_t  spkey_from, spkey_to;
static struct { int32_t model; uint32_t mat, uv, frame; uint8_t used; } *spkey;
static uint32_t  spkey_n;

static void spkey_frame(void) {
    static geo3d_state_t geo;
    const game_quirks_t *q = &g_active_profile->quirks;
    if (!g_geodl_snap_ready || !romset.main_data || !romset.polygons) return;
    if (!spkey) spkey = calloc(SPKEY_SLOTS, sizeof *spkey);
    if (!geo3d_scan_geo_list(&geo, g_geodl_snap, BUFF_RAM_SIZE / 4, g_geodl_snap_rstart,
                             (int16_t)mem_read16(&bus, H_SYNC_BASE), (int16_t)mem_read16(&bus, V_SYNC_BASE),
                             romset.main_data, romset.main_data_size, q->model_table_offset, q->model_table_count)) return;
    for (int k = 0; k < geo.captured_count; k++) {
        const captured_model_t *cm = &geo.captured[k];
        if (cm->direct_len || cm->model_idx < 0 || (uint32_t)cm->model_idx >= q->model_table_count) continue;
        uint32_t toff = q->model_table_offset + (uint32_t)cm->model_idx * MODEL_ENTRY_SIZE;
        if ((size_t)toff + MODEL_ENTRY_SIZE > romset.main_data_size) continue;
        if (!read_u32_le(romset.main_data + toff + 8)) continue;
        uint32_t mat = read_u32_le(romset.main_data + toff + 4), uv = read_u32_le(romset.main_data + toff + 0);
        if (cm->tha != 0xFFFFFFFFu) mat = cm->tha;
        if (cm->tpa != 0xFFFFFFFFu) uv = cm->tpa;
        if ((mat && (mat & 0x800000u)) || (uv && (uv & 0x800000u))) continue;   /* texture RAM: decoded every time */
        uint32_t h = geo3d_mesh_hash(cm->model_idx, mat, uv);
        for (uint32_t p = 0; p < SPKEY_SLOTS; p++) {
            __typeof__(*spkey) *e = &spkey[(h + p) & (SPKEY_SLOTS - 1u)];
            if (e->used && e->model == cm->model_idx && e->mat == mat && e->uv == uv) break;
            if (e->used) continue;
            if (spkey_n >= SPKEY_SLOTS / 2) break;
            *e = (__typeof__(*spkey)){ cm->model_idx, mat, uv, g_emu_frames, 1 };
            spkey_n++;
            break;
        }
    }
}

static int spkey_cmp(const void *a, const void *b) {
    const __typeof__(*spkey) *x = a, *y = b;
    if (x->used != y->used) return x->used ? -1 : 1;
    if (x->frame != y->frame) return x->frame < y->frame ? -1 : 1;
    if (x->model != y->model) return x->model < y->model ? -1 : 1;
    if (x->mat != y->mat) return x->mat < y->mat ? -1 : 1;
    return x->uv < y->uv ? -1 : x->uv > y->uv;
}

static void spkey_write(void) {
    const game_quirks_t *q = &g_active_profile->quirks;
    fprintf(spkey_out, "# The mesh cache's keys STF's display lists draw from ROM (det_digest\n"
                       "# --strip-keys), for tools/dc_strips.c: `model mat uv frame`, the frame\n"
                       "# that first draws it. Addresses only.\n");
    fprintf(spkey_out, "table %x %u %x %x\n", q->model_table_offset, q->model_table_count,
            q->mesh_ptr_subtract, q->mesh_ptr_add);
    if (!spkey) return;
    qsort(spkey, SPKEY_SLOTS, sizeof *spkey, spkey_cmp);
    for (uint32_t i = 0; i < spkey_n; i++)
        fprintf(spkey_out, "%d %x %x %u\n", spkey[i].model, spkey[i].mat, spkey[i].uv, spkey[i].frame);
}

/* --tex-pack F0:F1:FILE[:+BASE]: TEXTURES.PAK (dreamcast/dc_texpak.h), the
 * PVR textures the Dreamcast's draw would cut for those frames' ROM meshes,
 * each cut from texture RAM as it is that frame and keyed by the words it was
 * cut from. A FILE that exists is read first and added to, so attract and a
 * fight make one pack; BASE is added to the frames, which order the data.
 * --tex-groups GROUPS (sfight.mdlgroups) lays the textures out by object
 * group (#508), each in the group of the model that first drew it. */
#define TXP_SLOTS 65536u
#define TXG_MAX   64
static const char *txp_path;
static uint32_t    txp_from, txp_to, txp_base;
static struct { uint32_t key, hash, frame, off; uint16_t model; uint8_t used; } *txp;
static uint32_t    txp_n;
static uint8_t    *txp_data;
static size_t      txp_bytes, txp_cap;
/* each mesh key's texture keys, made once */
static struct { int32_t model; uint32_t mat, uv; uint32_t *keys; uint32_t nk; uint8_t used; } *txp_mesh;

static int      txg_n;                       /* groups read; 0: lay out by frame */
static char     txg_name[TXG_MAX + 1][24];
static uint8_t  txg_of[65536];               /* a model's group + 1, 0: none */

/* sfight.mdlgroups: `group NAME`, then model table numbers (hex), each in the
 * first group that names it (as tools/dc_strips.c reads it). */
static int txg_read(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "--tex-groups: cannot read %s\n", path); return -1; }
    char line[1024];
    int cur = -1;
    while (fgets(line, sizeof line, f)) {
        char *c = strchr(line, '#');
        if (c) *c = 0;
        if (!strncmp(line, "group ", 6)) {
            if (txg_n == TXG_MAX) { fprintf(stderr, "--tex-groups: over %d groups\n", TXG_MAX); fclose(f); return -1; }
            cur = txg_n++;
            sscanf(line + 6, "%23s", txg_name[cur]);
            continue;
        }
        for (char *t = strtok(line, " \t\r\n"); t; t = strtok(NULL, " \t\r\n")) {
            unsigned long m = strtoul(t, NULL, 16);
            if (cur >= 0 && m < 65536 && !txg_of[m]) txg_of[m] = (uint8_t)(cur + 1);
        }
    }
    fclose(f);
    snprintf(txg_name[txg_n], sizeof txg_name[0], "other");
    return 0;
}

static void txp_add(uint32_t key, uint32_t hash, uint32_t frame, uint16_t model, const uint8_t *tex) {
    uint32_t h = (key * 2654435761u) ^ hash;
    for (uint32_t p = 0; p < TXP_SLOTS; p++) {
        __typeof__(*txp) *e = &txp[(h + p) & (TXP_SLOTS - 1u)];
        if (e->used && e->key == key && e->hash == hash) return;
        if (e->used) continue;
        if (txp_n >= TXP_SLOTS / 2) return;
        const uint32_t len = dct_bytes(key);
        if (txp_bytes + len > txp_cap) {
            txp_cap = (txp_bytes + len) * 2;
            txp_data = realloc(txp_data, txp_cap);
        }
        memcpy(txp_data + txp_bytes, tex, len);
        *e = (__typeof__(*txp)){ key, hash, frame, (uint32_t)txp_bytes, model, 1 };
        txp_bytes += len;
        txp_n++;
        return;
    }
}

static void txp_load(void) {
    txp = calloc(TXP_SLOTS, sizeof *txp);
    txp_mesh = calloc(SPKEY_SLOTS, sizeof *txp_mesh);
    dct_init();
    FILE *f = fopen(txp_path, "rb");
    if (!f) return;
    dct_head_t h;
    if (fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, "M2TX", 4)) {
        dct_index_t *ix = malloc((size_t)h.n * sizeof *ix);
        uint8_t *d = malloc(h.bytes ? h.bytes : 1);
        uint16_t *mo = malloc((size_t)h.n * sizeof *mo);
        memset(mo, 0xFF, (size_t)h.n * sizeof *mo);
        if (fread(ix, sizeof *ix, h.n, f) == h.n && !fseek(f, (long)h.data_off, SEEK_SET) &&
            fread(d, 1, h.bytes, f) == h.bytes) {
            if (h.models_off && !fseek(f, (long)h.models_off, SEEK_SET) && fread(mo, sizeof *mo, h.n, f) != h.n)
                memset(mo, 0xFF, (size_t)h.n * sizeof *mo);
            for (uint32_t i = 0; i < h.n; i++) txp_add(ix[i].key, ix[i].hash, ix[i].frame, mo[i], d + ix[i].off);
        }
        free(ix);
        free(d);
        free(mo);
    }
    fclose(f);
    fprintf(stderr, "tex-pack: %u textures from %s\n", txp_n, txp_path);
}

static void txp_frame(void) {
    static geo3d_state_t geo;
    static uint8_t blob[4u << 20], cut[32768];
    const game_quirks_t *q = &g_active_profile->quirks;
    if (!g_geodl_snap_ready || !romset.main_data || !romset.polygons) return;
    if (!geo3d_scan_geo_list(&geo, g_geodl_snap, BUFF_RAM_SIZE / 4, g_geodl_snap_rstart,
                             (int16_t)mem_read16(&bus, H_SYNC_BASE), (int16_t)mem_read16(&bus, V_SYNC_BASE),
                             romset.main_data, romset.main_data_size, q->model_table_offset, q->model_table_count)) return;
    for (int k = 0; k < geo.captured_count; k++) {
        const captured_model_t *cm = &geo.captured[k];
        if (cm->direct_len || cm->model_idx < 0 || (uint32_t)cm->model_idx >= q->model_table_count) continue;
        uint32_t toff = q->model_table_offset + (uint32_t)cm->model_idx * MODEL_ENTRY_SIZE;
        if ((size_t)toff + MODEL_ENTRY_SIZE > romset.main_data_size) continue;
        uint32_t raw = read_u32_le(romset.main_data + toff + 8);
        if (!raw) continue;
        uint32_t mat = read_u32_le(romset.main_data + toff + 4), uv = read_u32_le(romset.main_data + toff + 0);
        if (cm->tha != 0xFFFFFFFFu) mat = cm->tha;
        if (cm->tpa != 0xFFFFFFFFu) uv = cm->tpa;
        if ((mat && (mat & 0x800000u)) || (uv && (uv & 0x800000u))) continue;   /* texture RAM: not in the strip pack */
        uint32_t h = geo3d_mesh_hash(cm->model_idx, mat, uv);
        __typeof__(*txp_mesh) *e = NULL;
        for (uint32_t p = 0; p < SPKEY_SLOTS; p++) {
            e = &txp_mesh[(h + p) & (SPKEY_SLOTS - 1u)];
            if (!e->used || (e->model == cm->model_idx && e->mat == mat && e->uv == uv)) break;
        }
        if (!e->used) {
            *e = (__typeof__(*txp_mesh)){ cm->model_idx, mat, uv, NULL, 0, 1 };
            geo3d_cmesh_t m = { .model_idx = cm->model_idx, .mat_ptr = mat, .uv_ptr = uv,
                                .md = { .polygons = romset.polygons, .materials = romset.textures,
                                        .main_data = romset.main_data,
                                        .polygons_size = romset.polygons_size, .materials_size = romset.textures_size,
                                        .table_off = q->model_table_offset, .table_count = q->model_table_count,
                                        .mesh_ptr_subtract = q->mesh_ptr_subtract, .mesh_ptr_add = q->mesh_ptr_add } };
            if (geo3d_mesh_build(&m, raw * 4u - q->mesh_ptr_subtract + q->mesh_ptr_add, mat != 0, uv != 0) &&
                dcs_blob(&m, blob, sizeof blob)) {
                const geo3d_sp_head_t *bh = (const geo3d_sp_head_t *)blob;
                const geo3d_sface_t *sf = (const geo3d_sface_t *)(blob + sizeof *bh +
                                          (((size_t)bh->n_sv * sizeof(vec3_t) + 31u) & ~(size_t)31u));
                e->keys = malloc((bh->n_faces + 1u) * sizeof *e->keys);
                for (unsigned n = 0; n < bh->n_faces; n++) {
                    uint32_t t = sf[n].tex;
                    if (!dct_packable(t)) continue;
                    unsigned j = 0;
                    while (j < e->nk && e->keys[j] != t) j++;
                    if (j == e->nk) e->keys[e->nk++] = t;
                }
            }
            free(m.sv);
            free(m.faces);
        }
        for (uint32_t j = 0; j < e->nk; j++) {
            const uint32_t t = e->keys[j];
            const uint32_t *sheet = (const uint32_t *)(DCT_SHEET(t) ? bus.texram1 : bus.texram0);
            dct_cut(sheet, t, 0, dct_bytes(t), cut);
            txp_add(t, dct_src_hash(sheet, t), g_emu_frames + txp_base, (uint16_t)cm->model_idx, cut);
        }
    }
}

static uint32_t txg_rank[TXG_MAX + 1];   /* a group's place: the first frame that draws from it */

static uint32_t txp_group(uint16_t model) {
    return txg_of[model] ? txg_of[model] - 1u : (uint32_t)txg_n;
}

static int txp_cmp_frame(const void *a, const void *b) {
    const __typeof__(*txp) *x = a, *y = b;
    if (x->used != y->used) return x->used ? -1 : 1;
    if (txg_n) {
        uint32_t gx = txp_group(x->model), gy = txp_group(y->model);
        if (txg_rank[gx] != txg_rank[gy]) return txg_rank[gx] < txg_rank[gy] ? -1 : 1;
        if (gx != gy) return gx < gy ? -1 : 1;
    }
    if (x->frame != y->frame) return x->frame < y->frame ? -1 : 1;
    return dct_cmp(x->key, x->hash, y->key, y->hash);
}

typedef struct { dct_index_t e; uint16_t model; } txp_ix_t;

static int txp_cmp_index(const void *a, const void *b) {
    const txp_ix_t *x = a, *y = b;
    return dct_cmp(x->e.key, x->e.hash, y->e.key, y->e.hash);
}

/* A group's table entry ends at at (a sector): its length, and its line in the log. */
static void txp_group_end(dct_group_t *g, uint32_t at) {
    g->len = at - g->off;
    fprintf(stderr, "  group %-24s %5u KB\n", g->name, g->len >> 10);
}

/* Each group's rank: the first frame any of its textures was drawn in. */
static void txp_rank_groups(void) {
    for (int g = 0; g <= txg_n; g++) txg_rank[g] = ~0u;
    for (uint32_t i = 0; i < TXP_SLOTS; i++)
        if (txp[i].used && txp[i].frame < txg_rank[txp_group(txp[i].model)])
            txg_rank[txp_group(txp[i].model)] = txp[i].frame;
}

/* Lays the sorted textures out in d, every group from a sector, filling ix,
 * grp (*ngrp) and frames; returns the bytes laid. */
static uint32_t txp_lay_out(uint8_t *d, txp_ix_t *ix, dct_group_t *grp, uint32_t *ngrp, uint32_t *frames) {
    uint32_t at = 0;
    for (uint32_t i = 0; i < txp_n; i++) {
        const uint32_t len = dct_bytes(txp[i].key), g = txp_group(txp[i].model);
        if (txg_n && (!i || g != txp_group(txp[i - 1].model))) {
            at = (at + DC_SECTOR - 1) / DC_SECTOR * DC_SECTOR;
            if (*ngrp) txp_group_end(&grp[*ngrp - 1], at);
            grp[*ngrp].off = at;
            snprintf(grp[(*ngrp)++].name, sizeof grp[0].name, "%s", txg_name[g]);
        }
        memcpy(d + at, txp_data + txp[i].off, len);
        ix[i] = (txp_ix_t){ { txp[i].key, txp[i].hash, at, txp[i].frame }, txp[i].model };
        at += (len + 31u) & ~31u;
        frames[txp[i].frame >= 100000u]++;
    }
    if (*ngrp) {
        at = (at + DC_SECTOR - 1) / DC_SECTOR * DC_SECTOR;
        txp_group_end(&grp[*ngrp - 1], at);
    }
    return at;
}

static int txp_write(void) {
    if (txg_n) txp_rank_groups();
    qsort(txp, TXP_SLOTS, sizeof *txp, txp_cmp_frame);
    dct_head_t h = { { 'M', '2', 'T', 'X' }, txp_n, 0, 0, 0, { 0 } };
    static dct_group_t grp[TXG_MAX + 1];
    uint32_t ngrp = 0, frames[4] = { 0 };
    txp_ix_t *ix = calloc(txp_n ? txp_n : 1, sizeof *ix);
    uint8_t *d = calloc(1, txp_bytes + 32u * txp_n + (size_t)DC_SECTOR * (TXG_MAX + 2) + 1);
    const uint32_t at = txp_lay_out(d, ix, grp, &ngrp, frames);
    h.pad[0] = ngrp;   /* the group table (dc_texpak.h), after the index */
    h.pad[1] = (uint32_t)(sizeof h + (size_t)txp_n * sizeof(dct_index_t));
    h.data_off = (uint32_t)((h.pad[1] + ngrp * sizeof(dct_group_t) + DC_SECTOR - 1) / DC_SECTOR * DC_SECTOR);
    h.bytes = at;
    h.models_off = h.data_off + at;
    qsort(ix, txp_n, sizeof *ix, txp_cmp_index);
    FILE *f = fopen(txp_path, "wb");
    if (!f) { fprintf(stderr, "tex-pack: cannot write %s\n", txp_path); return 2; }
    uint8_t *head = calloc(1, h.data_off);
    uint16_t *mo = calloc(txp_n ? txp_n : 1, sizeof *mo);
    memcpy(head, &h, sizeof h);
    for (uint32_t i = 0; i < txp_n; i++) {
        memcpy(head + sizeof h + (size_t)i * sizeof(dct_index_t), &ix[i].e, sizeof(dct_index_t));
        mo[i] = ix[i].model;
    }
    memcpy(head + h.pad[1], grp, ngrp * sizeof(dct_group_t));
    fwrite(head, 1, h.data_off, f);
    fwrite(d, 1, at, f);
    fwrite(mo, sizeof *mo, txp_n, f);
    fclose(f);
    fprintf(stderr, "tex-pack: %u textures (%u before frame 100000, %u after), %.2f MB in %s%s\n",
            txp_n, frames[0], frames[1], (double)(h.models_off + 2u * txp_n) / 1048576.0, txp_path,
            txg_n ? ", by group" : "");
    free(head); free(d); free(ix); free(mo);
    return 0;
}

static void mdlmap_write(void) {
    static const char *name[2] = { "po", "tx" };
    size_t size[2] = { romset.polygons_size, romset.textures_size };
    fprintf(mdlmap_out, "# The polygon (po) and texture (tx) ROM bytes STF's 3D decoder reads, by the\n"
                        "# game frame that first reads them (det_digest --model-map), for\n"
                        "# tools/dc_mdlpack.py: `region offset length frame`, addresses only.\n");
    for (int r = 0; r < 2; r++) {
        uint32_t nl = (uint32_t)(size[r] / MDLMAP_LINE);
        for (uint32_t l = 0; mdlmap_first[r] && l < nl;) {
            uint32_t f = mdlmap_first[r][l], e = l + 1;
            if (!f) { l++; continue; }
            while (e < nl && mdlmap_first[r][e] == f) e++;
            fprintf(mdlmap_out, "%s %06x %x %u\n", name[r], l * MDLMAP_LINE, (e - l) * MDLMAP_LINE, f - 1u);
            l = e;
        }
    }
}

#if I960_BLOCKS
/* --census F0:F1:FILE (an I960_BLOCKS build): what a recompiler would have to
 * cover. Every instruction of those game frames is stepped as trace_slice does
 * and sorted: a hook's, one a block takes (ib_decode), or another, by opcode.
 * Also the runs of block-able instructions between the others, the code's
 * footprint and the memory ops that leave plain memory. */
static FILE    *census_out;
static uint32_t census_from, census_to;
static struct {
    uint64_t n, hook, hook_extra, blk, mmio, frames, xfer, taken;
    uint64_t key[0x2000];          /* the others: CTRL/COBR op, REG 0x1000|op, MEM 0x800|op */
    uint64_t kind[64];             /* the block-able, by IB_ kind */
    uint64_t run[65];              /* runs of block-able instructions, by length (64 = longer) */
    uint64_t run_ins;              /* instructions in them */
    uint32_t cur;
    uint8_t  seen[0x400000u / 32u];
    uint32_t hits[0x400000u / 4u];   /* per instruction word */
    uint64_t mmio_pg[0x1000];         /* off plain memory, by 1 MB region; +0x800 for stores */
} cz;

static void census_end_run(void) {
    if (cz.cur) { cz.run[cz.cur > 64 ? 64 : cz.cur]++; cz.run_ins += cz.cur; cz.cur = 0; }
}

static void census_slice(emu_thread_ctx_t *ctx) {
    i960_cpu_t *c = ctx->cpu; memory_bus_t *b = ctx->bus;
    g_vblank_edge = 0;
    emu_timers_slice_begin(ctx);
    emu_service_irq(ctx);
    hle_filter_sync();
    cz.frames++;
    for (int i = 0; i < g_emu_steps_per_slice && !ctx->request_stop && !c->halted; i++) {
        if (g_irqt_vblank) { g_irqt_vblank = 0; g_vblank_edge = 1; break; }
        if (ctx->step_over_bp) ctx->step_over_bp = 0;
        else if (bp_check(c->sfr.ip)) break;
        uint32_t ip = c->sfr.ip, k = (ip >> 2) & 0xFFFFu;
        bool hook = (s_hle_filter[k >> 3] >> (k & 7u)) & 1u;
        ib_op_t o; bool end = false; uint32_t len = 0, w1 = 0, w2 = 0;
        const uint8_t *p = ip >> 16 < 0x10000u ? b->rd_page[ip >> 16] : NULL;
        if (p && (ip & 0xFFFFu) <= 0xFFF8u) { w1 = mem_le32(p + (ip & 0xFFFFu)); w2 = mem_le32(p + (ip & 0xFFFFu) + 4); len = ib_decode(c, &o, ip, w1, w2, &end); }
        if (ip < 0x400000u) { cz.seen[ip >> 5] |= (uint8_t)(1u << ((ip >> 2) & 7u)); cz.hits[ip >> 2]++; }
        cz.n++;
        if (len && !hook) {
            cz.blk++; cz.cur++; cz.kind[o.kind & 63]++;
            if (o.kind >= IB_LD && o.kind <= IB_STN && o.kind != IB_LDA) {
                uint32_t ea = o.k + *o.a + (*o.b << o.sh);
                bool st = o.kind >= IB_ST && o.kind != IB_LDN;
                if (!(st ? b->wr_page : b->rd_page)[ea >> 16]) { cz.mmio++; cz.mmio_pg[((ea >> 20) & 0x7FFu) | (st ? 0x800u : 0u)]++; }
            }
            if (o.kind >= IB_B) cz.xfer++;
        } else {
            census_end_run();
            if (hook) cz.hook++;
            else {
                uint32_t cls = w1 >> 28, key;
                if (cls <= 3)                 key = w1 >> 24;
                else if (cls >= 5 && cls <= 7) key = 0x1000u | ((w1 >> 7) & 0xFF0u) | ((w1 >> 7) & 0xFu);
                else                          key = 0x800u | (w1 >> 24);
                cz.key[key & 0x1FFFu]++;
                cz.xfer++;
            }
        }
        uint32_t before = c->sfr.ip;
        if (i960_step_hot(c, b) != 0) break;
        if (len && !hook && o.kind >= IB_B && c->sfr.ip != before + len) cz.taken++;
        ctx->total_steps++;
        if (g_hle_extra) { cz.hook_extra += g_hle_extra; ctx->total_steps += g_hle_extra; i += (int)g_hle_extra; g_hle_extra = 0; }
        if (s_irq_in_service && g_active_profile) emu_service_sound_again(ctx);
        else if (g_irqt_sound_kick && g_active_profile) { g_irqt_sound_kick = 0; emu_offer_sound(ctx); }
        emu_timers_after_step(ctx);
        if (g_log.warn_triggered) break;
        if (g_wp.hit) break;
        if (g_sharc.unknown_triggered) break;
    }
    census_end_run();
    bool frame = g_vblank_edge != 0;
    if (frame) {
        if (g_active_profile->quirks.board_vblank) { cop_geo_frame_edge(); dl_frame_edge(ctx->bus, g_emu_frames); hle_match_replay_edge(ctx->bus); }
    }
    emu_sound_slice_end(frame);
    if (g_active_profile->quirks.geo_displaylist) geodl_capture(ctx->bus);
    ctx->cpu_prev_snapshot = ctx->cpu_snapshot;
    ctx->cpu_snapshot      = *ctx->cpu;
}

static void census_write(void) {
    FILE *f = census_out;
    double n = (double)cz.n, fr = cz.frames ? (double)cz.frames : 1.0;
    uint64_t foot = 0;
    for (size_t i = 0; i < sizeof cz.seen; i++) foot += (uint64_t)__builtin_popcount(cz.seen[i]);
    fprintf(f, "frames %llu  instructions %llu (%.0f a frame)  hook-skipped steps %llu\n",
            (unsigned long long)cz.frames, (unsigned long long)cz.n, n / fr, (unsigned long long)cz.hook_extra);
    fprintf(f, "block-able %.2f%%  hooks %.2f%%  others %.2f%%\n", 100.0 * cz.blk / n, 100.0 * cz.hook / n,
            100.0 * (cz.n - cz.blk - cz.hook) / n);
    fprintf(f, "memory ops off plain memory %llu (%.2f%% of instructions)\n", (unsigned long long)cz.mmio, 100.0 * cz.mmio / n);
    fprintf(f, "control transfers %llu (one per %.1f instructions); block branches taken %llu\n",
            (unsigned long long)cz.xfer, n / (double)(cz.xfer ? cz.xfer : 1), (unsigned long long)cz.taken);
    fprintf(f, "code footprint %llu instruction words (%llu KB)\n", (unsigned long long)foot, (unsigned long long)(foot * 4 / 1024));
    uint64_t runs = 0; for (int i = 1; i <= 64; i++) runs += cz.run[i];
    fprintf(f, "runs of block-able instructions: %llu, %.2f long on average\n", (unsigned long long)runs, runs ? (double)cz.run_ins / runs : 0.0);
    uint64_t acc = 0;
    for (int i = 1; i <= 64; i++) if (cz.run[i]) { acc += cz.run[i] * (uint64_t)i; fprintf(f, "  run %2d%s %8llu  (cum. instr %5.1f%%)\n", i, i == 64 ? "+" : " ", (unsigned long long)cz.run[i], 100.0 * acc / (double)(cz.run_ins ? cz.run_ins : 1)); }
    fprintf(f, "block-able by kind (IB_ enum order):\n");
    for (int i = 0; i < 64; i++) if (cz.kind[i]) fprintf(f, "  kind %2d %10llu %6.2f%%\n", i, (unsigned long long)cz.kind[i], 100.0 * cz.kind[i] / n);
    fprintf(f, "off plain memory, by 1 MB region (L load, S store):\n");
    for (int i = 0; i < 0x1000; i++) if (cz.mmio_pg[i] * 1000 > cz.mmio) fprintf(f, "  %c %03xxxxxx %10llu\n", i & 0x800 ? 'S' : 'L', i & 0x7FF, (unsigned long long)cz.mmio_pg[i]);
    fprintf(f, "hottest instructions (IP, count, hook):\n");
    for (int t = 0; t < 12; t++) {
        uint32_t best = 0;
        for (uint32_t i = 1; i < 0x100000u; i++) if (cz.hits[i] > cz.hits[best]) best = i;
        if (!cz.hits[best]) break;
        uint32_t k = best & 0xFFFFu;
        fprintf(f, "  %08x %10u %6.2f%% %s\n", best * 4u, cz.hits[best], 100.0 * cz.hits[best] / n,
                (s_hle_filter[k >> 3] >> (k & 7u)) & 1u ? "hook" : "");
        cz.hits[best] = 0;
    }
    fprintf(f, "others by opcode (CTRL/COBR: op; MEM: 0x8xx; REG: 0x1xxx):\n");
    for (;;) {
        int best = -1;
        for (int i = 0; i < 0x2000; i++) if (cz.key[i] && (best < 0 || cz.key[i] > cz.key[best])) best = i;
        if (best < 0 || cz.key[best] * 2000 < cz.n) break;   /* down to 0.05% */
        fprintf(f, "  0x%04x %10llu %6.2f%%\n", best, (unsigned long long)cz.key[best], 100.0 * cz.key[best] / n);
        cz.key[best] = 0;
    }
}
#endif

/* --inputs: a session's words and checks, indexed by session frame. */
static uint32_t *in_w0, *in_w1, *in_check;
static uint32_t  in_n;

static bool load_inputs(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    uint32_t cap = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        unsigned frame, w0, w1, check;
        if (line[0] == '#' || sscanf(line, "%u %x %x %x", &frame, &w0, &w1, &check) != 4) continue;
        if (frame >= cap) {
            uint32_t n = cap ? cap * 2 : 65536;
            while (n <= frame) n *= 2;
            in_w0 = realloc(in_w0, n * sizeof *in_w0);
            in_w1 = realloc(in_w1, n * sizeof *in_w1);
            in_check = realloc(in_check, n * sizeof *in_check);
            if (!in_w0 || !in_w1 || !in_check) { fclose(f); return false; }
            cap = n;
        }
        in_w0[frame] = w0; in_w1[frame] = w1; in_check[frame] = check;
        if (frame + 1 > in_n) in_n = frame + 1;
    }
    fclose(f);
    return in_n > 0;
}

/* Both words into the board's held mask: netplay_apply_inputs, whose bit tables
 * only exist inside a session. */
static uint32_t words_mask(uint32_t w0, uint32_t w1) {
    static const int p1[10] = { GAME_INPUT_P1_UP, GAME_INPUT_P1_DOWN, GAME_INPUT_P1_LEFT, GAME_INPUT_P1_RIGHT,
        GAME_INPUT_P1_B1, GAME_INPUT_P1_B2, GAME_INPUT_P1_B3, GAME_INPUT_P1_B4, GAME_INPUT_P1_START, GAME_INPUT_P1_COIN };
    static const int p2[10] = { GAME_INPUT_P2_UP, GAME_INPUT_P2_DOWN, GAME_INPUT_P2_LEFT, GAME_INPUT_P2_RIGHT,
        GAME_INPUT_P2_B1, GAME_INPUT_P2_B2, GAME_INPUT_P2_B3, GAME_INPUT_P2_B4, GAME_INPUT_P2_START, GAME_INPUT_P2_COIN };
    const game_input_map_t *in = &g_active_profile->input;
    uint32_t held = 0;
    for (int i = 0; i < 10; i++) {
        if (w0 & (1u << i)) held |= in->bits[p1[i]];
        if (w1 & (1u << i)) held |= in->bits[p2[i]];
    }
    if (w0 & (1u << NP_BIT_SERVICE)) held |= in->bits[GAME_INPUT_SERVICE];
    if (w0 & (1u << NP_BIT_TEST))    held |= in->bits[GAME_INPUT_TEST];
    return held;
}

static void parse_script(const char *s) {
    while (*s && script_n < SCRIPT_MAX) {
        const char *comma = strchr(s, ',');
        const char *end = comma ? comma : s + strlen(s);
        const char *colon = memchr(s, ':', (size_t)(end - s));
        if (colon) {
            script[script_n].frame = (uint32_t)strtoul(s, NULL, 10);
            script[script_n].held  = keys_mask(colon + 1, end);
            script_n++;
        }
        s = comma ? comma + 1 : end;
    }
}

/* A folder of the PS3 release's ROM files (StF - PS3/stf_rom) in place of
 * the zip, placed as the Dreamcast disc places them (dreamcast/dc_layout.h). */
static int load_rom_dir(const char *dir) {
    const dc_layout_t *lay = &dc_layout_sfight;
    uint8_t **ptr[DC_REGIONS] = { &romset.maincpu, &romset.main_data, &romset.copro_data, &romset.polygons,
                                  &romset.textures, &romset.audiocpu, &romset.samples };
    size_t *size[DC_REGIONS] = { &romset.maincpu_size, &romset.main_data_size, &romset.copro_data_size,
                                 &romset.polygons_size, &romset.textures_size, &romset.audiocpu_size,
                                 &romset.samples_size };
    for (int r = 0; r < DC_REGIONS; r++) {
        const dc_region_t *rg = &lay->rg[r];
        *ptr[r] = NULL; *size[r] = rg->size;
        if (!rg->size) continue;
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
            if (!f) { fprintf(stderr, "no %s in %s\n", sg->file, dir); return -1; }
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
        *ptr[r] = b;
    }
    romset.loaded = true;
    profile_adopt_program(romset.maincpu, romset.maincpu_size);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: det_digest <merged sfight zip, a folder of region images, or the PS3 stf_rom folder> [--frames N] [--script S] [--from F] [--out FILE] [--profile ID] [--save-at F:FILE] [--load FILE] [--mem]\n");
        return 2;
    }
    uint32_t frames = 3600, from = 0;
    uint32_t peek_addr = 0;
    bool frames_given = false, sound_cols = false, cpu_cols = false;
    const char *out_path = NULL, *script_text = NULL, *inputs_path = NULL, *profile_id = "sfight";
    for (int i = 2; i < argc; i++) {
        if      (!strcmp(argv[i], "--frames") && i + 1 < argc) { frames = (uint32_t)strtoul(argv[++i], NULL, 10); frames_given = true; }
        else if (!strcmp(argv[i], "--inputs") && i + 1 < argc) inputs_path = argv[++i];
        else if (!strcmp(argv[i], "--from")   && i + 1 < argc) from   = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script_text = argv[++i];
        else if (!strcmp(argv[i], "--out")    && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--sound"))                   sound_cols = true;
        else if (!strcmp(argv[i], "--cpu"))                     cpu_cols = true;
        else if (!strcmp(argv[i], "--no-sound-thread"))         g_sound_thread_want = 0;
        else if (!strcmp(argv[i], "--sound-hle"))               g_sound_hle_want = 1;
        else if (!strcmp(argv[i], "--texload-i960"))            g_texload_hle = 0;
        else if (!strcmp(argv[i], "--spin-i960"))               g_spin_skip = 0;
        else if (!strcmp(argv[i], "--live-timers"))             ;   /* always on now */
        else if (!strcmp(argv[i], "--nowarnskip"))              g_warning_skip = 0;
        else if (!strcmp(argv[i], "--steps-per-slice") && i + 1 < argc) g_emu_steps_per_slice = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--region") && i + 1 < argc) { const char *r = argv[++i]; g_region = !strcmp(r, "japan") ? GAME_REGION_JAPAN : !strcmp(r, "export") ? GAME_REGION_EXPORT : GAME_REGION_USA; }
        else if (!strcmp(argv[i], "--peek")   && i + 1 < argc) peek_addr = (uint32_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--pcm")    && i + 1 < argc) snd_pcm = fopen(argv[++i], "wb");
#if I960_BLOCKS
        else if (!strcmp(argv[i], "--census") && i + 1 < argc) {
            char path[1024];
            if (sscanf(argv[++i], "%u:%u:%1023s", &census_from, &census_to, path) != 3 || !(census_out = fopen(path, "wb"))) {
                fprintf(stderr, "--census F0:F1:FILE\n");
                return 2;
            }
        }
#endif
        else if (!strcmp(argv[i], "--raw") && i + 1 < argc) {
            char *a = argv[++i], *colon = strrchr(a, ':');
            if (!colon || !(raw_out = fopen(colon + 1, "wb"))) { fprintf(stderr, "--raw wants RANGES:FILE\n"); return 2; }
            for (char *p = a; p < colon && raw_n < 16; ) {
                char *e;
                raw_rng[raw_n][0] = (uint32_t)strtoul(p, &e, 16);
                if (*e != ':') break;
                raw_rng[raw_n][1] = (uint32_t)strtoul(e + 1, &e, 16) & ~3u;
                raw_n++;
                p = (*e == ',') ? e + 1 : colon;
            }
        }
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) profile_id = argv[++i];
        else if (!strcmp(argv[i], "--gems"))   g_gems_i960 = g_gems_cop = true;
        else if (!strcmp(argv[i], "--gems-verify")) g_gems_verify = g_gems_cop = true;   /* each trap both ways */
        else if (!strcmp(argv[i], "--aot-map") && i + 1 < argc) {
            if (sscanf(argv[++i], "%u:%u:%1023s", &aotmap_from, &aotmap_to, aotmap_path) != 3
                    || !(aotmap_out = fopen(aotmap_path, "wb"))) {
                fprintf(stderr, "--aot-map F0:F1:FILE\n");
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--strip-keys") && i + 1 < argc) {
            char path[1024] = {0};
            if (sscanf(argv[++i], "%u:%u:%1023s", &spkey_from, &spkey_to, path) != 3
                    || !(spkey_out = fopen(path, "wb"))) {
                fprintf(stderr, "--strip-keys F0:F1:FILE\n");
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--tex-pack") && i + 1 < argc) {
            static char path[1024];
            char *plus;
            if (sscanf(argv[++i], "%u:%u:%1023s", &txp_from, &txp_to, path) != 3) {
                fprintf(stderr, "--tex-pack F0:F1:FILE[:+BASE]\n");
                return 2;
            }
            if ((plus = strstr(path, ":+")) != NULL) { txp_base = (uint32_t)strtoul(plus + 2, NULL, 10); *plus = 0; }
            txp_path = path;
            txp_load();
        }
        else if (!strcmp(argv[i], "--tex-groups") && i + 1 < argc) {
            if (txg_read(argv[++i]) != 0) return 2;
        }
        else if (!strcmp(argv[i], "--model-map") && i + 1 < argc) {
            char path[1024] = {0};
            if (sscanf(argv[++i], "%u:%u:%1023s", &mdlmap_from, &mdlmap_to, path) != 3
                    || !(mdlmap_out = fopen(path, "wb"))) {
                fprintf(stderr, "--model-map F0:F1:FILE\n");
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--trace")  && i + 1 < argc) {
            char path[1024] = {0};
            if (sscanf(argv[++i], "%u:%1023s", &trace_frame, path) != 2 || !(trace_out = fopen(path, "wb"))) {
                fprintf(stderr, "--trace F:FILE\n");
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--load")   && i + 1 < argc) load_path = argv[++i];
        else if (!strcmp(argv[i], "--mem")) state_mem = true;
        else if (!strcmp(argv[i], "--save-at") && i + 1 < argc) {
            if (sscanf(argv[++i], "%u:%1023s", &save_frame, save_path) != 2) {
                fprintf(stderr, "--save-at F:FILE\n");
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--cop")    && i + 1 < argc) {
            char path[1024] = {0};
            if (sscanf(argv[++i], "%u:%u:%1023s", &cop_from, &cop_to, path) != 3 || !(cop_out = fopen(path, "wb"))) {
                fprintf(stderr, "--cop FROM:TO:FILE\n");
                return 2;
            }
            g_cop_tap = cop_tap;
        }
    }

    log_init();
    mem_init(&bus, NULL, 0);
    i960_reset(&cpu);
    bp_init();
    wp_init();
    for (size_t i = 0; i < g_profile_count; i++)
        if (!strcmp(g_profiles[i]->id, profile_id)) g_active_profile = g_profiles[i];
    if (!g_active_profile) { fprintf(stderr, "no %s profile\n", profile_id); return 2; }
    if (script_text) parse_script(script_text);
    if (inputs_path) {
        if (!load_inputs(inputs_path)) { fprintf(stderr, "cannot read an input log from %s\n", inputs_path); return 2; }
        if (!frames_given || frames > in_n) frames = in_n;
        fprintf(stderr, "replaying %u session frames from %s\n", (unsigned)in_n, inputs_path);
    }

    /* A directory of region images (--export-roms) loads as it is, and the PS3
     * release's stf_rom folder as the Dreamcast disc places it. Otherwise the
     * web build's load: one zip, read whole, strict by CRC. */
    if (romset_is_dir(argv[1])) {
        char probe[1024];
        snprintf(probe, sizeof probe, "%s/%s", argv[1], k_romset_files[0]);
        struct stat st;
        if (!stat(probe, &st)) {
            if (romset_load_dir(&romset, argv[1]) != 0) return 2;
        } else if (load_rom_dir(argv[1]) != 0) {
            fprintf(stderr, "ROM load from %s failed\n", argv[1]);
            return 2;
        }
    } else {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *zip = (uint8_t *)malloc((size_t)len);
        if (!zip || fread(zip, 1, (size_t)len, f) != (size_t)len) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
        fclose(f);
        rl_mem_zip_set(zip, (size_t)len, true);
        int rc = g_active_profile->load_fn(&romset, NULL, NULL);
        rl_mem_zip_clear();
        free(zip);
        if (rc != 0) { fprintf(stderr, "ROM load failed; missing: %s\n", g_rl_mem_zip.missing_names); return 2; }
    }

    /* main_web.c web_install_board, which is also what a netplay reset runs. */
    g_active_profile->install_fn(&romset, &cpu, &bus);
    if (g_gems_i960 || g_gems_cop) gems_apply(profile_rom_set(g_active_profile));
    irqt_reset();
    if (g_active_profile->quirks.enable_68k_sound) {
        sound_reset();
        sound_attach(&bus);
        if (romset.audiocpu && romset.audiocpu_size > 0) sound_load_rom(romset.audiocpu, (uint32_t)romset.audiocpu_size);
        if (romset.samples && romset.samples_size > 0)   sound_load_samples(romset.samples, (uint32_t)romset.samples_size);
    }
    input_reset();
    input_attach(&bus);
    emu_board_reset_state();
    emu_ctx_init(&emu, &cpu, &bus);
    emu_run(&emu);
    if (raw_out) { raw_bus = &bus; raw_cpu = &cpu; g_game_frame_edge_cb = raw_frame; }
    sound_set_tap(snd_out_tap, NULL);
    if (load_path) {
        /* A board from a state: what it prints from here on has to be what
         * the run that saved it printed. The sample hash starts here. */
        const char *err = state_mem ? mem_state_load(&emu, load_path) : emu_state_load_now(&emu, load_path);
        if (err) { fprintf(stderr, "--load %s: %s\n", load_path, err); return 2; }
        snd_out_hash = FNV0; snd_out_n = 0;
        fprintf(stderr, "loaded %s at frame %u\n", load_path, (unsigned)g_emu_frames);
    }

    FILE *out = out_path ? fopen(out_path, "wb") : stdout;
    if (!out) { fprintf(stderr, "cannot write %s\n", out_path); return 2; }

    int at = 0;
    uint64_t slices = 0;
    uint32_t mismatches = 0, first_mismatch = UINT32_MAX;
    while (g_emu_frames < frames) {
        /* Inputs change only on a frame boundary, as the lockstep's do. */
        while (at < script_n && script[at].frame <= g_emu_frames) g_input.held = script[at++].held;
        if (in_n && g_emu_frames < in_n) g_input.held = words_mask(in_w0[g_emu_frames], in_w1[g_emu_frames]);
#if I960_BLOCKS
        if (census_out && g_emu_frames + 1 >= census_from && g_emu_frames + 1 <= census_to) census_slice(&emu);
        else
#endif
        if (aotmap_out && g_emu_frames + 1 >= aotmap_from && g_emu_frames + 1 <= aotmap_to) aotmap_slice(&emu);
        else
        if (trace_out && g_emu_frames + 1 == trace_frame) trace_slice(&emu);
        else                                              emu_slice_body(&emu);
        emu_slice_result_t r = emu_slice_finish(&emu);
        slices++;
        if (r == EMU_SLICE_STOPPED) {
            fprintf(stderr, "board stopped at frame %u, IP 0x%08X\n", (unsigned)g_emu_frames, cpu.sfr.ip);
            break;
        }
        if (r == EMU_SLICE_FRAME && in_n && g_emu_frames - 1 < in_n) {
            uint32_t f = g_emu_frames - 1, c = netplay_frame_check(&emu.cpu_snapshot, emu.total_steps);
            if (c != in_check[f]) {
                if (!mismatches) {
                    first_mismatch = f;
                    fprintf(stderr, "first mismatch at session frame %u: the log says %08X, the replay %08X\n",
                            (unsigned)f, in_check[f], c);
                }
                mismatches++;
            }
        }
        if (r == EMU_SLICE_FRAME && mdlmap_out && g_emu_frames >= mdlmap_from && g_emu_frames <= mdlmap_to) mdlmap_frame();
        if (r == EMU_SLICE_FRAME && spkey_out && g_emu_frames >= spkey_from && g_emu_frames <= spkey_to) spkey_frame();
        if (r == EMU_SLICE_FRAME && txp_path && g_emu_frames >= txp_from && g_emu_frames <= txp_to) txp_frame();
        if (r == EMU_SLICE_FRAME && save_frame && g_emu_frames == save_frame) {
            /* ... and the samples from here on are what the loaded run's are. */
            const char *err = state_mem ? mem_state_save(&emu, save_path) : emu_state_save_now(&emu, save_path);
            if (err) { fprintf(stderr, "--save-at %s: %s\n", save_path, err); return 2; }
            snd_out_hash = FNV0; snd_out_n = 0;
            fprintf(stderr, "saved %s at frame %u\n", save_path, (unsigned)g_emu_frames);
        }
        if (r != EMU_SLICE_FRAME || g_emu_frames < from) continue;
        uint32_t check = netplay_frame_check(&emu.cpu_snapshot, emu.total_steps);
        uint64_t ram  = fnv(fnv(FNV0, bus.ram, RAM_SIZE), bus.ram2, RAM2_SIZE);
        uint64_t buf  = fnv(FNV0, bus.buff_ram, BUFF_RAM_SIZE);
        uint64_t dm   = fnv(FNV0, g_sharc.dm, sizeof g_sharc.dm);
        fprintf(out, "%u %08x %016llx %016llx %016llx %llu", (unsigned)g_emu_frames, check,
                (unsigned long long)ram, (unsigned long long)buf, (unsigned long long)dm,
                (unsigned long long)emu.total_steps);
        if (cpu_cols) {
            uint64_t tex  = fnv(fnv(FNV0, bus.texram0, TEXRAM0_SIZE), bus.texram1, TEXRAM1_SIZE);
            uint64_t regs = fnv(fnv(fnv(FNV0, cpu.globals.g, sizeof cpu.globals.g), cpu.locals.r, sizeof cpu.locals.r), &cpu.sfr, sizeof cpu.sfr);
            fprintf(out, " %016llx %016llx %llu", (unsigned long long)tex, (unsigned long long)regs,
                    (unsigned long long)cpu.cycles);
        }
        if (sound_cols) {
            sound_settle();
            fprintf(out, " %016llx %llu", (unsigned long long)fnv(FNV0, g_sound.ram, sizeof g_sound.ram),
                    (unsigned long long)g_sound.out_total);
        }
        if (peek_addr) fprintf(out, " %02x", mem_read8(&bus, peek_addr));
        fputc('\n', out);
    }
    if (out != stdout) fclose(out);
    gems_verify_report();
    fprintf(stderr, "texload: %llu rows in C\n", (unsigned long long)g_texload_rows);
    fprintf(stderr, "spin: %llu idle iterations skipped\n", (unsigned long long)g_spin_iters);
    if (cop_out) fclose(cop_out);
    if (raw_out) fclose(raw_out);
    if (trace_out) fclose(trace_out);
    if (aotmap_out) { aotmap_write(&bus, aotmap_path); fclose(aotmap_out); }
    if (mdlmap_out) { mdlmap_write(); fclose(mdlmap_out); }
    if (spkey_out) { spkey_write(); fclose(spkey_out); }
    if (txp_path && txp_write()) return 2;
#if I960_BLOCKS
    if (census_out) { census_write(); fclose(census_out); }
#endif
#if I960_AOT
    fprintf(stderr, "aot: %llu instructions compiled (%.1f%%)\n", (unsigned long long)g_aot_ops,
            emu.total_steps ? 100.0 * (double)g_aot_ops / (double)emu.total_steps : 0.0);
#endif
    fprintf(stderr, "%u frames, %llu slices, %llu i960 steps\n", (unsigned)g_emu_frames,
            (unsigned long long)slices, (unsigned long long)emu.total_steps);
    sound_settle();
    fprintf(stderr, "sound: %llu samples, output %016llx, sound RAM %016llx (%s, %llu slices handed over)\n",
            (unsigned long long)snd_out_n, (unsigned long long)snd_out_hash,
            (unsigned long long)fnv(FNV0, g_sound.ram, sizeof g_sound.ram),
            sound_thread_on() ? "sound thread" : "one thread", (unsigned long long)g_emu_times.sound_jobs);
    fprintf(stderr, "sound: %s; i960 commands %u, bytes that waited a slice %llu, midi drops %u\n",
            g_shle.on ? "driver in C (--sound-hle)" : "68000", g_sound.code_n,
            (unsigned long long)g_sound.midi_holds, g_sound.scsp.mi_drops);
    if (in_n) {
        if (mismatches) fprintf(stderr, "input log: %u of %u frames' checks differ, the first at session frame %u\n",
                                (unsigned)mismatches, (unsigned)g_emu_frames, (unsigned)first_mismatch);
        else            fprintf(stderr, "input log: every check matches (%u frames)\n", (unsigned)g_emu_frames);
        return mismatches ? 1 : 0;
    }
    return 0;
}
