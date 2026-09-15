/*
 * arc_bench.c — headless speed test for small ARM handhelds (RG ARC-S, RK3566).
 *
 * Runs the same work the app does per 60 Hz slice, timed per stage, with no
 * window and no GL:
 *   emu     the RUNNING branch of emu_thread_run_loop (IRQ service, i960 batch
 *           to the frame hook, frame edge, optional 68K) minus mutex and sleep
 *   render  main.c frame() minus ImGui: tile compositor, GEO scan, atlas/LUT
 *           decode + upload, vertex build — sokol_gfx on its dummy backend, so
 *           every CPU-side cost runs and the GPU calls are no-ops
 *
 * Audio is out of scope: no SCSP mixer, and the 68K only runs with --sound.
 *
 *   arc_bench <romset.zip> [--seconds N] [--threads] [--pace] [--sound]
 *             [--no-render] [--max-temp C] [--report S]
 *
 * Default: one thread, emu then render, flat out (throughput).
 * --pace     sleep each slice out to 1/60 s.
 * --threads  the app's layout: emu on its own thread, render on the main thread,
 *            both paced to 60 Hz (implies --pace) — the thermal soak.
 * --render-fps N  like --threads, but the render loop is capped at N fps while
 *            emu stays at 60 Hz (frameskip).
 * --max-temp abort once any thermal zone reaches C degrees (default 85).
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SOKOL_GFX_IMPL
#define SOKOL_DUMMY_BACKEND
#define SOKOL_TRACE_HOOKS
#include "sokol_gfx.h"
#undef SOKOL_GFX_IMPL   /* the implementation half has no include guard */

/* ---- --draw-digest: what every draw call would put on screen ---------------
 * Through sokol's trace hooks, each draw is hashed from the pipeline's label,
 * the viewport and scissor in force, the uniform bytes, and the exact vertex
 * bytes it covers (from a shadow copy of every buffer update/append). One line
 * per frame. Two renderers that draw the same thing in the same order produce
 * the same file, however they batch their buffer uploads. */

#define DIGEST_SLOTS 4096
typedef struct { uint8_t *data; size_t size; } digest_buf_t;
static struct {
    FILE        *out;
    digest_buf_t bufs[DIGEST_SLOTS];
    uint64_t     pip_label[DIGEST_SLOTS];
    int          pip_stride[DIGEST_SLOTS];
    uint32_t     cur_pip, cur_vbuf;
    int          cur_vbuf_off;
    int          viewport[4], scissor[4];
    uint64_t     uniforms;
    uint64_t     frame_hash, frame_draws, frame;
} g_dg;

static uint64_t fnv1a(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}
#define FNV_SEED 0xcbf29ce484222325ull

static void dg_make_pipeline(const sg_pipeline_desc *d, sg_pipeline r, void *u) {
    (void)u;
    uint32_t s = r.id & (DIGEST_SLOTS - 1);
    g_dg.pip_label[s]  = fnv1a(FNV_SEED, d->label ? d->label : "", d->label ? strlen(d->label) : 0);
    g_dg.pip_stride[s] = d->layout.buffers[0].stride;
}
static void dg_store(sg_buffer b, const sg_range *data, size_t at) {
    digest_buf_t *db = &g_dg.bufs[b.id & (DIGEST_SLOTS - 1)];
    if (db->size < at + data->size) {
        db->data = realloc(db->data, at + data->size);
        db->size = at + data->size;
    }
    memcpy(db->data + at, data->ptr, data->size);
}
static void dg_update_buffer(sg_buffer b, const sg_range *d, void *u) { (void)u; dg_store(b, d, 0); }
static void dg_append_buffer(sg_buffer b, const sg_range *d, int result, void *u) { (void)u; dg_store(b, d, (size_t)result); }
static void dg_viewport(int x, int y, int w, int h, bool tl, void *u) {
    (void)tl; (void)u; g_dg.viewport[0] = x; g_dg.viewport[1] = y; g_dg.viewport[2] = w; g_dg.viewport[3] = h;
}
static void dg_scissor(int x, int y, int w, int h, bool tl, void *u) {
    (void)tl; (void)u; g_dg.scissor[0] = x; g_dg.scissor[1] = y; g_dg.scissor[2] = w; g_dg.scissor[3] = h;
}
static void dg_pipeline(sg_pipeline p, void *u) { (void)u; g_dg.cur_pip = p.id; }
static void dg_bindings(const sg_bindings *b, void *u) {
    (void)u; g_dg.cur_vbuf = b->vertex_buffers[0].id; g_dg.cur_vbuf_off = b->vertex_buffer_offsets[0];
}
static void dg_uniforms(int ub, const sg_range *d, void *u) {
    (void)u; g_dg.uniforms = fnv1a(fnv1a(FNV_SEED, &ub, sizeof ub), d->ptr, d->size);
}
static void dg_draw(int base, int count, int inst, void *u) {
    (void)u;
    uint32_t ps = g_dg.cur_pip & (DIGEST_SLOTS - 1);
    int stride = g_dg.pip_stride[ps];
    const digest_buf_t *db = &g_dg.bufs[g_dg.cur_vbuf & (DIGEST_SLOTS - 1)];
    size_t from = (size_t)g_dg.cur_vbuf_off + (size_t)base * (size_t)stride;
    size_t n = (size_t)count * (size_t)stride;
    uint64_t h = fnv1a(FNV_SEED, &g_dg.pip_label[ps], sizeof(uint64_t));
    h = fnv1a(h, g_dg.viewport, sizeof g_dg.viewport);
    h = fnv1a(h, g_dg.scissor, sizeof g_dg.scissor);
    h = fnv1a(h, &g_dg.uniforms, sizeof g_dg.uniforms);
    h = fnv1a(h, &count, sizeof count);
    h = fnv1a(h, &inst, sizeof inst);
    if (db->data && from + n <= db->size) h = fnv1a(h, db->data + from, n);
    else h = fnv1a(h, "short", 5);
    g_dg.frame_hash = fnv1a(g_dg.frame_hash, &h, sizeof h);
    g_dg.frame_draws++;
}
static void dg_commit(void *u) {
    (void)u;
    fprintf(g_dg.out, "%llu %llu %016llx\n", (unsigned long long)g_dg.frame,
            (unsigned long long)g_dg.frame_draws, (unsigned long long)g_dg.frame_hash);
    g_dg.frame++;
    g_dg.frame_hash = FNV_SEED;
    g_dg.frame_draws = 0;
}
static void digest_install(const char *path) {
    g_dg.out = fopen(path, "w");
    if (!g_dg.out) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    g_dg.frame_hash = FNV_SEED;
    sg_install_trace_hooks(&(sg_trace_hooks){
        .make_pipeline = dg_make_pipeline, .update_buffer = dg_update_buffer,
        .append_buffer = dg_append_buffer, .apply_viewport = dg_viewport,
        .apply_scissor_rect = dg_scissor, .apply_pipeline = dg_pipeline,
        .apply_bindings = dg_bindings, .apply_uniforms = dg_uniforms,
        .draw = dg_draw, .commit = dg_commit,
    });
}

#include "constants.h"
#include "log.h"
#include "memory.h"
#include "i960.h"
#include "i960_exec.h"
#include "breakpoint.h"
#include "watchpoint.h"
#include "rom_loader.h"
#include "emu_thread.h"
#include "geo3d.h"
#include "game_render.h"
#include "video_window.h"
#include "sound.h"
#include "input.h"
#include "registry.h"

#define FB_W 640
#define FB_H 480

static memory_bus_t     bus;
static i960_cpu_t       cpu;
static emu_thread_ctx_t ctx;
static romset_t         romset;
static video_state_t    video;
static geo3d_state_t    geo3d;

static int64_t now_us(void) { return emu_now_us(); }

static int read_milli(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}

/* ---- Stats: cumulative counters, reported as deltas ---------------------- */

enum { R_TILES, R_SCAN, R_ATLAS, R_LUTS, R_DRAW, R_STAGES };
static const char *const r_names[R_STAGES] = { "tiles", "scan", "atlas", "luts", "draw" };

typedef struct {
    uint64_t slices, frames, steps;
    int64_t  us, max_us;
} emu_stats_t;

typedef struct {
    uint64_t frames;
    int64_t  us, max_us;
    int64_t  stage_us[R_STAGES];
} render_stats_t;

static emu_stats_t    g_es;
static render_stats_t g_rs;

/* ---- ROM load (main.c load_active_profile, without the emu thread) ------- */

static void select_profile_for_zip(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char id[64] = {0};
    const char *dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    if (n >= sizeof(id)) n = sizeof(id) - 1;
    memcpy(id, base, n);
    for (size_t i = 0; i < g_profile_count; i++)
        if (strcmp(g_profiles[i]->id, id) == 0) g_active_profile = g_profiles[i];
}

static int load_rom(const char *zip, bool sound) {
    select_profile_for_zip(zip);
    if (!g_active_profile) { fprintf(stderr, "no profile for %s\n", zip); return -1; }

    char parent[512] = {0};
    const char *parent_ptr = NULL;
    if (g_active_profile->parent_zip_name) {
        const char *sep = strrchr(zip, '/');
        size_t dir_len = sep ? (size_t)(sep - zip + 1) : 0;
        if (dir_len < sizeof(parent) - 64) {
            memcpy(parent, zip, dir_len);
            strcat(parent, g_active_profile->parent_zip_name);
            parent_ptr = parent;
        }
    }
    if (g_active_profile->load_fn(&romset, zip, parent_ptr) != 0) return -1;
    g_active_profile->install_fn(&romset, &cpu, &bus);

    if (sound && g_active_profile->quirks.enable_68k_sound) {
        sound_reset();
        sound_attach(&bus);
        if (romset.audiocpu && romset.audiocpu_size > 0)
            sound_load_rom(romset.audiocpu, (uint32_t)romset.audiocpu_size);
        if (romset.samples && romset.samples_size > 0) {
            sound_load_samples(romset.samples, (uint32_t)romset.samples_size);
            scsp_hle_set_sample_rom(romset.samples, (uint32_t)romset.samples_size);
        }
    }
    input_reset();
    input_attach(&bus);
    return 0;
}

/* ---- Emu: one slice of emu_thread_run_loop's RUNNING branch -------------- */

static bool g_with_68k = false;

/* --profile: i960 instruction count per code address (ROM code is < 4 MB),
 * sampled between game frames --profile-from and --profile-to. */
#define PROF_WORDS (0x400000 / 4)
static uint32_t *g_prof = NULL;
static uint64_t  g_prof_from = 900, g_prof_to = 900 + 3450;

__attribute__((noinline)) static void emu_slice(void) {
    const game_quirks_t *q = &g_active_profile->quirks;
    int64_t a = now_us();
    g_frame_done = 0;
    bool board_vblank = q->board_vblank;
    if (board_vblank) {
        irqt_raise(0x1u);
        g_vblank_acked = 0;
        g_cop.geo_frame_start = g_cop.geo_frame_end;
        g_cop.geo_frame_end   = g_cop.geo_capture_head;
    }
    if (g_real_irq) irqt_tick(EMU_CPU_HZ / EMU_SLICES_PER_SEC);
    emu_service_irq(&ctx);
    int i;
    bool prof = g_prof && g_es.frames >= g_prof_from && g_es.frames < g_prof_to;
    for (i = 0;
         i < EMU_STEPS_PER_SLICE && !g_frame_done
         && !(board_vblank && g_vblank_acked) && !cpu.halted;
         i++)
    {
        if (prof && cpu.sfr.ip < 0x400000) g_prof[cpu.sfr.ip >> 2]++;
        if (bp_check(cpu.sfr.ip)) break;
        if (i960_step(&cpu, &bus) != 0) break;
        ctx.total_steps++;
        if (g_log.warn_triggered) break;
        if (g_wp.hit) break;
        if (g_sharc.unknown_triggered) break;
    }
    bool frame = g_frame_done || (board_vblank && g_vblank_acked);
    if (frame) dl_frame_edge(&bus, g_emu_frames);
    if (g_with_68k) sound_step(M68K_STEPS_PER_SLICE);
    if (q->geo_displaylist) geodl_capture(&bus);
    ctx.cpu_prev_snapshot = ctx.cpu_snapshot;
    ctx.cpu_snapshot      = cpu;
    if (frame) g_emu_frames++;
    g_log.warn_triggered = 0; g_wp.hit = 0; g_sharc.unknown_triggered = 0;

    int64_t d = now_us() - a;
    g_es.slices++;
    g_es.frames += frame;
    g_es.steps  += (uint64_t)i;
    g_es.us     += d;
    if (d > g_es.max_us) g_es.max_us = d;
}

/* ---- --verify-tiles: the per-pixel pair renderer the run-based one replaced,
 * kept verbatim as the oracle. Each rendered frame both fill their own layer
 * buffers (stale values included) and all four must match byte for byte. */

static void ref_sys24_pair(const memory_bus_t *bus, uint16_t *out, uint8_t *alpha,
                           int pair, bool opaque, uint16_t *out_lo, uint8_t *alpha_lo) {
    const int MW = 64, MH = 64;
    uint32_t l0_off = (pair == 0) ? 0x0000u : 0x4000u;
    uint32_t l1_off = l0_off + 0x2000u;
    uint32_t hscr_w = (pair == 0) ? 0x5000u : 0x5002u;
    uint32_t vc_w   = (pair == 0) ? 0x5004u : 0x5006u;
    uint32_t hstb_w = (pair == 0) ? 0x4000u : 0x4400u;
    uint16_t hscr = tileram_word(bus, hscr_w);
    uint16_t vc   = tileram_word(bus, vc_w);
    int vy = vc & 0x1ff;
    uint16_t backc = opaque ? back_color_555(bus) : 0;
    int n = VIDEO_WIDTH * VIDEO_HEIGHT;
    if (vc & 0x8000) {
        for (int i = 0; i < n; i++) { out[i] = backc; if (alpha) alpha[i] = 0; }
        return;
    }
    bool window    = (vc & 0x6000) != 0;
    bool rowscroll = (hscr & 0x8000) != 0;
    for (int sy = 0; sy < VIDEO_HEIGHT; sy++) {
        int wy = sy + vy;
        uint16_t rh = rowscroll ? tileram_word(bus, hstb_w + (uint32_t)sy) : hscr;
        int h = rh & 0x1ff;
        int l1_is_even = (rh & 0x200) ? 1 : 0;
        for (int sx = 0; sx < VIDEO_WIDTH; sx++) {
            uint32_t tmap;
            if (window && rowscroll) {
                int left = (sx < h);
                int use_even = left ? l1_is_even : !l1_is_even;
                tmap = use_even ? l0_off : l1_off;
            } else {
                tmap = l0_off;
            }
            uint8_t ci, pr;
            uint16_t color = tile_sample_px(bus, tmap, MW, MH, sx + h, wy, &ci, &pr);
            int i = sy * VIDEO_WIDTH + sx;
            if (out_lo && !opaque && ci != 0 && pr == 0) {
                out_lo[i] = color;
                if (alpha_lo) alpha_lo[i] = 255;
                if (alpha)    alpha[i]    = 0;
            } else {
                out[i] = color;
                if (alpha) alpha[i] = opaque ? 255 : (ci != 0 ? 255 : 0);
            }
        }
    }
}

static bool          g_verify_tiles = false;
static tile_layers_t g_ref_layers, g_new_layers;
static uint64_t      g_verify_frames = 0, g_verify_bad = 0;
static int64_t       g_ref_us = 0, g_new_us = 0;
static uint64_t      g_changed[4], g_changed_mark = 0, g_changed_hist[64];   /* tile, gfx, pal, any */
static int           g_changed_buckets = 0;

static void verify_tiles(void) {
    int64_t ref_start = now_us();
    ref_sys24_pair(&bus, g_ref_layers.bg, g_ref_layers.bg_alpha, 1, true, NULL, NULL);
    ref_sys24_pair(&bus, g_ref_layers.fg, g_ref_layers.alpha, 0, false,
                   g_ref_layers.bg, g_ref_layers.bg_alpha);
    int64_t new_start = now_us();
    render_bg_layer(&bus, &g_new_layers);   /* timing only: the same work video_update did */
    render_fg_layer(&bus, &g_new_layers);
    g_new_us += now_us() - new_start;
    g_ref_us += new_start - ref_start;

    /* How often does the 2D input change between rendered frames at all? */
    static uint8_t prev_tile[TILE_SIZE], prev_gfx[TMAPGFX_SIZE], prev_pal[PALETTE_SIZE];
    bool dt = memcmp(prev_tile, bus.tile, TILE_SIZE) != 0;
    bool dg = memcmp(prev_gfx, bus.tmapgfx, TMAPGFX_SIZE) != 0;
    bool dp = memcmp(prev_pal, bus.palette, PALETTE_SIZE) != 0;
    if (dt) { memcpy(prev_tile, bus.tile, TILE_SIZE); g_changed[0]++; }
    if (dg) { memcpy(prev_gfx, bus.tmapgfx, TMAPGFX_SIZE); g_changed[1]++; }
    if (dp) { memcpy(prev_pal, bus.palette, PALETTE_SIZE); g_changed[2]++; }
    if (dt || dg || dp) g_changed[3]++;
    if (g_verify_frames % 600 == 0) {
        g_changed_hist[g_changed_buckets++ % 64] = g_changed[3] - g_changed_mark;
        g_changed_mark = g_changed[3];
    }
    size_t n = (size_t)VIDEO_WIDTH * VIDEO_HEIGHT;
    const tile_layers_t *a = &video.layers, *b = &g_ref_layers;
    bool same = !memcmp(a->bg, b->bg, n * sizeof(uint16_t)) && !memcmp(a->fg, b->fg, n * sizeof(uint16_t))
             && !memcmp(a->alpha, b->alpha, n) && !memcmp(a->bg_alpha, b->bg_alpha, n);
    g_verify_frames++;
    if (!same && g_verify_bad++ < 5)
        printf("verify-tiles: MISMATCH at rendered frame %llu (game frame %llu)\n",
               (unsigned long long)g_verify_frames, (unsigned long long)g_es.frames);
}

/* ---- Render: main.c frame() with the ImGui and debug-dump parts removed -- */

__attribute__((noinline)) static void render_frame(void) {
    const game_quirks_t *q = &g_active_profile->quirks;
    int64_t t[R_STAGES + 1];
    t[0] = now_us();

    video_update(&video, &bus);
    t[1] = now_us();
    if (g_verify_tiles) { verify_tiles(); t[0] += now_us() - t[1]; t[1] = now_us(); }

    if (geo3d.use_game_view && q->camera_struct_addr)
        geo3d_read_game_view(&geo3d, &bus, q->camera_struct_addr, q->camera_angle_addr);
    if (geo3d.enabled && romset.main_data && romset.polygons) {
        if (q->geo_displaylist) {
            if (g_geodl_snap_ready)
                geo3d_scan_displaylist(&geo3d, g_geodl_snap, BUFF_RAM_SIZE / 4, 0x10000,
                                       romset.main_data, romset.main_data_size,
                                       q->model_table_offset, q->model_table_count,
                                       bus.palette, PALETTE_SIZE);
        } else if (!(g_geo_use_list && g_geodl_snap_ready &&
                     geo3d_scan_geo_list(&geo3d, g_geodl_snap, BUFF_RAM_SIZE / 4,
                                         g_geodl_snap_rstart,
                                         (int16_t)mem_read16(&bus, H_SYNC_BASE),
                                         (int16_t)mem_read16(&bus, V_SYNC_BASE),
                                         romset.main_data, romset.main_data_size,
                                         q->model_table_offset, q->model_table_count))) {
            geo3d_scan_captures(&geo3d, romset.main_data, romset.main_data_size,
                                romset.polygons_size,
                                q->model_table_offset, q->model_table_count,
                                q->mesh_ptr_subtract, q->mesh_ptr_add);
        }
    } else {
        geo3d_lines_reset();
    }
    t[2] = now_us();

    sg_begin_pass(&(sg_pass){
        .swapchain = { .width = FB_W, .height = FB_H, .sample_count = 1,
                       .color_format = SG_PIXELFORMAT_RGBA8,
                       .depth_format = SG_PIXELFORMAT_DEPTH_STENCIL },
    });
    int ox, oy, w, h;
    game_render_letterbox(FB_W, FB_H, VIDEO_WIDTH, VIDEO_HEIGHT, &ox, &oy, &w, &h);
    game_render_upload_atlas(bus.texram0, bus.texram1, TEXRAM0_SIZE);
    t[3] = now_us();
    game_render_upload_luts(bus.luma, bus.colorxlat);
    t[4] = now_us();

    game_render_draw_game(video.back_view, ox, oy, w, h);
    game_render_draw_game(video.bg_view,   ox, oy, w, h);
    if (geo3d.enabled && romset.main_data && romset.polygons) {
        g_geo3d_palram      = bus.palette;
        g_geo3d_palram_size = PALETTE_SIZE;
        game_render_draw_captured_models(&geo3d,
                                         romset.main_data, romset.main_data_size,
                                         romset.polygons,  romset.polygons_size,
                                         romset.textures,  romset.textures_size,
                                         q->model_table_offset, q->model_table_count,
                                         q->mesh_ptr_subtract, q->mesh_ptr_add,
                                         ox, oy, w, h,
                                         geo3d.cam_x, geo3d.cam_y, geo3d.cam_z,
                                         geo3d.rot_y, geo3d.rot_x, geo3d.fov_deg,
                                         1.0f);
        g_geo3d_palram = NULL;
    }
    game_render_draw_game(video.fg_view, ox, oy, w, h);
    sg_end_pass();
    sg_commit();
    t[5] = now_us();

    int64_t d = t[5] - t[0];
    g_rs.frames++;
    g_rs.us += d;
    if (d > g_rs.max_us) g_rs.max_us = d;
    for (int s = 0; s < R_STAGES; s++) g_rs.stage_us[s] += t[s + 1] - t[s];
}

/* ---- Threads and pacing --------------------------------------------------- */

static volatile int g_run = 1;

static void pace(int64_t *deadline, int64_t slice_start, int64_t period_us) {
    *deadline = *deadline ? *deadline + period_us : slice_start + period_us;
    int64_t now = now_us();
    if (*deadline < now - period_us) *deadline = now + period_us;
    if (*deadline > now) emu_sleep_us(*deadline - now);
}

static void *emu_thread_main(void *p) {
    (void)p;
    int64_t deadline = 0;
    while (g_run && !cpu.halted) {
        int64_t a = now_us();
        emu_slice();
        pace(&deadline, a, EMU_SLICE_US);
    }
    return NULL;
}

/* ---- Reporting ------------------------------------------------------------ */

typedef struct {
    int64_t        wall;
    emu_stats_t    es;
    render_stats_t rs;
    long           log_bytes;
} mark_t;

static mark_t mark_now(void) {
    mark_t m = { now_us(), g_es, g_rs, g_log.file ? ftell(g_log.file) : 0 };
    g_es.max_us = 0;
    g_rs.max_us = 0;
    return m;
}

static int hottest_milli(int *cpu_t, int *gpu_t) {
    *cpu_t = read_milli("/sys/class/thermal/thermal_zone0/temp");
    *gpu_t = read_milli("/sys/class/thermal/thermal_zone1/temp");
    return *cpu_t > *gpu_t ? *cpu_t : *gpu_t;
}

static void report(const mark_t *a, const mark_t *b, double t) {
    double wall = (b->wall - a->wall) / 1e6;
    uint64_t sl = b->es.slices - a->es.slices;
    uint64_t rf = b->rs.frames - a->rs.frames;
    double sl_n = sl ? (double)sl : 1.0, rf_n = rf ? (double)rf : 1.0;
    int ct, gt;
    hottest_milli(&ct, &gt);
    int freq = read_milli("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
    printf("t=%6.1fs game=%5.1ffps emu=%5.2fms(max %5.1f) %4.0fk/slice | "
           "render=%5.1ffps %5.2fms(max %5.1f) [",
           t, (b->es.frames - a->es.frames) / wall,
           (b->es.us - a->es.us) / 1000.0 / sl_n, b->es.max_us / 1000.0,
           (b->es.steps - a->es.steps) / 1000.0 / sl_n,
           rf / wall, (b->rs.us - a->rs.us) / 1000.0 / rf_n, b->rs.max_us / 1000.0);
    for (int s = 0; s < R_STAGES; s++)
        printf("%s%s %.2f", s ? " " : "", r_names[s],
               (b->rs.stage_us[s] - a->rs.stage_us[s]) / 1000.0 / rf_n);
    printf("] | cpu=%4.1fC gpu=%4.1fC %4dMHz log=%ldB\n",
           ct / 1000.0, gt / 1000.0, freq / 1000, b->log_bytes - a->log_bytes);
    fflush(stdout);
}

int main(int argc, char **argv) {
    const char *zip = NULL;
    double seconds = 60.0, report_s = 5.0, max_temp = 85.0, render_fps = 0.0;
    const char *prof_path = NULL, *digest_path = NULL;
    uint64_t max_frames = 0;
    bool do_pace = false, threads = false, render = true;
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--seconds")  && i + 1 < argc) seconds  = atof(argv[++i]);
        else if (!strcmp(argv[i], "--report")   && i + 1 < argc) report_s = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max-temp") && i + 1 < argc) max_temp = atof(argv[++i]);
        else if (!strcmp(argv[i], "--render-fps") && i + 1 < argc) render_fps = atof(argv[++i]);
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) prof_path = argv[++i];
        else if (!strcmp(argv[i], "--verify-tiles")) g_verify_tiles = true;
        else if (!strcmp(argv[i], "--no-mesh-cache")) g_geo3d_mesh_cache = 0;
        else if (!strcmp(argv[i], "--draw-digest") && i + 1 < argc) digest_path = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--profile-from") && i + 1 < argc) g_prof_from = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--profile-to") && i + 1 < argc) g_prof_to = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--pace"))      do_pace = true;
        else if (!strcmp(argv[i], "--threads"))   threads = do_pace = true;
        else if (!strcmp(argv[i], "--sound"))     g_with_68k = true;
        else if (!strcmp(argv[i], "--no-render")) render  = false;
        else zip = argv[i];
    }
    if (!zip) { fprintf(stderr, "usage: arc_bench <romset.zip> [options]\n"); return 2; }
    /* A render cap only makes sense with emu on its own 60 Hz thread (frameskip). */
    if (render_fps > 0.0) threads = do_pace = true;
    int64_t render_period_us = render_fps > 0.0 ? (int64_t)(1e6 / render_fps) : EMU_SLICE_US;
    if (prof_path) g_prof = calloc(PROF_WORDS, sizeof(uint32_t));
    if (g_verify_tiles) {
        if (threads) { fprintf(stderr, "--verify-tiles needs a single thread\n"); return 2; }
        tile_layers_init(&g_ref_layers);
        tile_layers_init(&g_new_layers);
    }

    mem_init(&bus, NULL, 0);
    i960_reset(&cpu);
    bp_init();
    wp_init();
    if (g_profile_count > 0) g_active_profile = g_profiles[0];

    sg_setup(&(sg_desc){
        .environment.defaults = { .color_format = SG_PIXELFORMAT_RGBA8,
                                  .depth_format = SG_PIXELFORMAT_DEPTH_STENCIL,
                                  .sample_count = 1 },
    });
    if (digest_path) {
        if (threads) { fprintf(stderr, "--draw-digest needs a single thread\n"); return 2; }
        digest_install(digest_path);
    }
    video_init(&video);
    game_render_init();
    geo3d_init(&geo3d);
    g_geo3d_state = &geo3d;

    int64_t t0 = now_us();
    if (load_rom(zip, g_with_68k) != 0) { fprintf(stderr, "ROM load failed\n"); return 1; }
    printf("loaded %s as '%s' in %.2fs (threads=%d pace=%d render-fps=%.0f sound=%d render=%d max-temp=%.0fC)\n",
           zip, g_active_profile->id, (now_us() - t0) / 1e6,
           threads, do_pace, 1e6 / render_period_us, g_with_68k, render, max_temp);

    memset(&ctx, 0, sizeof(ctx));
    ctx.cpu = &cpu;
    ctx.bus = &bus;

    pthread_t emu_th;
    if (threads) pthread_create(&emu_th, NULL, emu_thread_main, NULL);

    mark_t first = mark_now(), last = first;
    int64_t start = first.wall;
    int64_t next_report = start + (int64_t)(report_s * 1e6), next_temp = start, deadline = 0;
    int rc = 0;

    while (now_us() - start < (int64_t)(seconds * 1e6)) {
        int64_t a = now_us();
        if (!threads) emu_slice();
        if (render) render_frame();
        else if (threads) emu_sleep_us(EMU_SLICE_US);
        if (do_pace && (render || !threads)) pace(&deadline, a, render_period_us);

        if (cpu.halted) { printf("CPU halted @ 0x%08X\n", cpu.sfr.ip); rc = 1; break; }
        if (g_prof && g_es.frames >= g_prof_to) break;
        if (max_frames && g_es.frames >= max_frames) break;

        int64_t now = now_us();
        if (now >= next_temp) {
            int ct, gt;
            int hottest = hottest_milli(&ct, &gt);
            if (hottest >= (int)(max_temp * 1000)) {
                mark_t m = mark_now();
                report(&last, &m, (now - start) / 1e6);
                printf("ABORT: %.1fC reached the %.0fC limit\n", hottest / 1000.0, max_temp);
                rc = 3;
                break;
            }
            next_temp = now + 500000;
        }
        if (now >= next_report) {
            mark_t m = mark_now();
            report(&last, &m, (now - start) / 1e6);
            last = m;
            next_report = now + (int64_t)(report_s * 1e6);
        }
    }

    g_run = 0;
    if (threads) pthread_join(emu_th, NULL);
    mark_t end = mark_now();
    printf("--- total ---\n");
    report(&first, &end, (end.wall - start) / 1e6);
    printf("mesh cache: %s, %llu builds, %llu hits, %u meshes held\n", g_geo3d_mesh_cache ? "on" : "off",
           (unsigned long long)g_geo3d_mesh_builds, (unsigned long long)g_geo3d_mesh_hits, g_geo3d_mesh_count);

    if (g_verify_tiles)
        printf("verify-tiles: %llu rendered frames, %llu mismatched; both layers "
               "per frame: per-pixel %.3f ms, run-based %.3f ms\n",
               (unsigned long long)g_verify_frames, (unsigned long long)g_verify_bad,
               g_ref_us / 1000.0 / (g_verify_frames ? g_verify_frames : 1),
               g_new_us / 1000.0 / (g_verify_frames ? g_verify_frames : 1));
    if (g_verify_tiles) {
        printf("2D input changed on %llu of %llu rendered frames (tile %llu, gfx %llu, palette %llu)\n"
               "changed frames per 600 rendered:",
               (unsigned long long)g_changed[3], (unsigned long long)g_verify_frames,
               (unsigned long long)g_changed[0], (unsigned long long)g_changed[1],
               (unsigned long long)g_changed[2]);
        for (int k = 0; k < g_changed_buckets && k < 64; k++)
            printf(" %llu", (unsigned long long)g_changed_hist[k]);
        printf("\n");
    }

    if (g_prof) {
        FILE *pf = fopen(prof_path, "w");
        if (pf) {
            fprintf(pf, "# frames %llu..%llu\n", (unsigned long long)g_prof_from,
                    (unsigned long long)(g_es.frames < g_prof_to ? g_es.frames : g_prof_to));
            for (uint32_t w = 0; w < PROF_WORDS; w++)
                if (g_prof[w]) fprintf(pf, "%08X %u\n", w << 2, g_prof[w]);
            fclose(pf);
            printf("profile written to %s\n", prof_path);
        }
    }
    return rc;
}
