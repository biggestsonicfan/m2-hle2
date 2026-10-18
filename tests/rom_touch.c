/*
 * rom_touch.c -- which ROM pages a game actually reads, and when (Pinboard #340).
 *
 * The Dreamcast has 16 MB of RAM and a Model 2 set is 50-60 MB of ROM, so a
 * port has to keep most of the ROM on the disc and hold only what is in use.
 * How much that is decides the whole design, so this measures it rather than
 * guessing: the ROM buffers are mapped PROT_NONE, a SIGSEGV handler marks the
 * 4 KB page an access lands in and opens it, and every --window frames the
 * pages are closed again. Every reader is counted -- the i960 through the bus,
 * the COP's table reads, the 3D decode and the 68000 -- without touching them.
 *
 *   rom_touch <romset.zip> [--frames N] [--window N] [--sound] [--no-render]
 *             [--press FRAME:ACTION[:FRAMES]]... [--csv FILE]
 *
 * ACTION is a game_input_t index (input_action_down), e.g. 9 = P1 coin,
 * 8 = P1 start, 4 = P1 B1. Per window it prints, for each region, the pages
 * read in that window and the pages read so far; at the end, the largest
 * window and the total. Linux only (mprotect + SA_SIGINFO).
 *
 * Build (from the repo root; V = a vendor/ with miniz built, whose
 * miniz_export.h is a stub defining MINIZ_EXPORT empty):
 *   cc -O2 -std=gnu11 -DM2HLE_VERSION='"dev"' -DM2HLE_DEV_TOOLS=0 \
 *      -Isrc -Isrc/board -Isrc/core -Isrc/ui -Isrc/profiles -Isrc/net \
 *      -I$V/sokol -I$V/sokol/util -I$V/miniz -I$V/stb tests/rom_touch.c \
 *      $V/miniz/miniz*.c -o rom_touch -lpthread -lm
 * Run it away from a folder holding a loose sfight/ (it replaces the program).
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define SOKOL_GFX_IMPL
#define SOKOL_DUMMY_BACKEND
#include "sokol_gfx.h"
#undef SOKOL_GFX_IMPL

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
#include "game_frame.h"
#include "sound.h"
#include "input.h"
#include "registry.h"

static memory_bus_t     bus;
static i960_cpu_t       cpu;
static emu_thread_ctx_t ctx;
static romset_t         romset;
static video_state_t    video;
static geo3d_state_t    geo3d;

/* ---- Tracked regions -------------------------------------------------------- */

#ifndef PG                 /* -DPG=65536u: count in the bus's 64 KB pages */
#define PG 4096u
#endif
#define MAX_TRACK 12

typedef struct {
    const char *name;
    uint8_t    *lo, *hi;        /* page-aligned interior of the buffer */
    size_t      pages;
    uint8_t    *win;            /* touched in this window */
    uint8_t    *ever;           /* touched since the start */
    size_t      win_n, ever_n, peak_win;
} track_t;

static track_t g_tr[MAX_TRACK];
static int     g_ntr;
static volatile uint64_t g_faults;

static void track(const char *name, uint8_t *buf, size_t size) {
    if (!buf || !size || g_ntr == MAX_TRACK) return;
    uintptr_t lo = ((uintptr_t)buf + PG - 1) & ~(uintptr_t)(PG - 1);
    uintptr_t hi = ((uintptr_t)buf + size) & ~(uintptr_t)(PG - 1);
    if (hi <= lo) return;
    track_t *t = &g_tr[g_ntr++];
    t->name  = name;
    t->lo    = (uint8_t *)lo;
    t->hi    = (uint8_t *)hi;
    t->pages = (hi - lo) / PG;
    t->win   = calloc(t->pages, 1);
    t->ever  = calloc(t->pages, 1);
}

static void close_all(void) {
    for (int i = 0; i < g_ntr; i++) {
        track_t *t = &g_tr[i];
        memset(t->win, 0, t->pages);
        t->win_n = 0;
        mprotect(t->lo, (size_t)(t->hi - t->lo), PROT_NONE);
    }
}

static void on_segv(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    uint8_t *a = (uint8_t *)si->si_addr;
    for (int i = 0; i < g_ntr; i++) {
        track_t *t = &g_tr[i];
        if (a < t->lo || a >= t->hi) continue;
        size_t p = (size_t)(a - t->lo) / PG;
        if (!t->win[p])  { t->win[p] = 1;  t->win_n++; }
        if (!t->ever[p]) { t->ever[p] = 1; t->ever_n++; }
        g_faults++;
        mprotect(t->lo + p * PG, PG, PROT_READ | PROT_WRITE);
        return;
    }
    signal(sig, SIG_DFL);   /* not ours: crash as normal */
}

/* ---- ROM load (main_web.c's web_install_board) --------------------------------------------------- */

static int load_rom(const char *zip, bool sound) {
    const char *base = strrchr(zip, '/');
    base = base ? base + 1 : zip;
    char id[64] = {0};
    const char *dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    if (n >= sizeof id) n = sizeof id - 1;
    memcpy(id, base, n);
    for (size_t i = 0; i < g_profile_count; i++)
        if (strcmp(g_profiles[i]->id, id) == 0) g_active_profile = g_profiles[i];
    if (!g_active_profile) { fprintf(stderr, "no profile for %s\n", zip); return -1; }

    char parent[512] = {0};
    const char *parent_ptr = NULL;
    if (g_active_profile->parent_zip_name) {
        const char *sep = strrchr(zip, '/');
        size_t dir_len = sep ? (size_t)(sep - zip + 1) : 0;
        memcpy(parent, zip, dir_len);
        strcat(parent, g_active_profile->parent_zip_name);
        parent_ptr = parent;
    }
    if (g_active_profile->load_fn(&romset, zip, parent_ptr) != 0) return -1;
    g_active_profile->install_fn(&romset, &cpu, &bus);
    irqt_reset();
    if (sound && g_active_profile->quirks.enable_68k_sound) {
        sound_reset();
        sound_attach(&bus);
        if (romset.audiocpu) sound_load_rom(romset.audiocpu, (uint32_t)romset.audiocpu_size);
        if (romset.samples)  sound_load_samples(romset.samples, (uint32_t)romset.samples_size);
    }
    input_reset();
    input_attach(&bus);
    emu_board_reset_state();
    return 0;
}

/* ---- Main ------------------------------------------------------------------------ */

#define MAX_PRESS 64
typedef struct { uint64_t frame, len; int act; } press_t;

int main(int argc, char **argv) {
    const char *zip = NULL, *csv_path = NULL;
    uint64_t frames = 3600, window = 60;
    bool sound = false, render = true;
    press_t press[MAX_PRESS];
    int npress = 0;
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--window") && i + 1 < argc) window = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--csv")    && i + 1 < argc) csv_path = argv[++i];
        else if (!strcmp(argv[i], "--sound"))     sound = true;
        else if (!strcmp(argv[i], "--no-render")) render = false;
        else if (!strcmp(argv[i], "--press") && i + 1 < argc && npress < MAX_PRESS) {
            press_t *p = &press[npress++];
            unsigned long long f = 0, l = 6; int a = 0;
            if (sscanf(argv[++i], "%llu:%d:%llu", &f, &a, &l) < 2) { fprintf(stderr, "bad --press\n"); return 2; }
            p->frame = f; p->act = a; p->len = l;
        }
        else zip = argv[i];
    }
    if (!zip) { fprintf(stderr, "usage: rom_touch <romset.zip> [options]\n"); return 2; }
    if (!window) window = 60;

    mem_init(&bus, NULL, 0);
    i960_reset(&cpu);
    bp_init();
    wp_init();
    sg_setup(&(sg_desc){
        .environment.defaults = { .color_format = SG_PIXELFORMAT_RGBA8,
                                  .depth_format = SG_PIXELFORMAT_DEPTH_STENCIL, .sample_count = 1 },
    });
    video_init(&video);
    game_render_init();
    geo3d_init(&geo3d);
    g_geo3d_state = &geo3d;
    if (load_rom(zip, sound) != 0) { fprintf(stderr, "ROM load failed\n"); return 1; }

    /* What a Dreamcast would have to page: the bus's copies of the data ROMs and
     * the set's own buffers that the COP, the 3D decode and the sound board read.
     * maincpu (2 MB, the i960's program) is left out: it would stay resident. */
    track("bus.main_data",  bus.main_data, MAIN_DATA_SIZE);
    track("bus.xtra_data",  bus.xtra_data, XTRA_DATA_SIZE);
    track("rs.main_data",   romset.main_data, romset.main_data_size);
    track("rs.copro_data",  romset.copro_data, romset.copro_data_size);
    track("rs.polygons",    romset.polygons, romset.polygons_size);
    track("rs.textures",    romset.textures, romset.textures_size);
    track("rs.samples",     romset.samples, romset.samples_size);
    track("rs.audiocpu",    romset.audiocpu, romset.audiocpu_size);

    struct sigaction sa = {0};
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);

    FILE *csv = csv_path ? fopen(csv_path, "w") : NULL;
    if (csv) {
        fprintf(csv, "frame");
        for (int i = 0; i < g_ntr; i++) fprintf(csv, ",%s.win,%s.ever", g_tr[i].name, g_tr[i].name);
        fprintf(csv, "\n");
    }
    printf("pages of %u KB; per window of %llu frames: read in window / read so far (of total)\n",
           PG / 1024, (unsigned long long)window);

    emu_ctx_init(&ctx, &cpu, &bus);
    ctx.run_state = EMU_RUNNING;

    close_all();
    uint64_t next = window;
    while (g_emu_frames < frames && !cpu.halted) {
        uint32_t f = g_emu_frames;
        for (int i = 0; i < npress; i++) {
            if (f == press[i].frame)               input_action_down(press[i].act);
            if (f == press[i].frame + press[i].len) input_action_up(press[i].act);
        }
        emu_slice_body(&ctx);
        emu_slice_finish(&ctx);
        if (render && g_emu_frames != f) {
            game_frame_prepare(&video, &geo3d, &bus, &romset, true);
            sg_begin_pass(&(sg_pass){ .swapchain = { .width = 640, .height = 480, .sample_count = 1,
                .color_format = SG_PIXELFORMAT_RGBA8, .depth_format = SG_PIXELFORMAT_DEPTH_STENCIL } });
            int ox, oy, w, h;
            game_render_letterbox(640, 480, VIDEO_WIDTH, VIDEO_HEIGHT, &ox, &oy, &w, &h);
            game_frame_draw(&video, &geo3d, &bus, &romset, ox, oy, w, h, 1.0f);
            sg_end_pass();
            sg_commit();
        }
        if (g_emu_frames >= next) {
            size_t win_kb = 0;
            printf("frame %6u:", g_emu_frames);
            if (csv) fprintf(csv, "%u", g_emu_frames);
            for (int i = 0; i < g_ntr; i++) {
                track_t *t = &g_tr[i];
                if (t->win_n > t->peak_win) t->peak_win = t->win_n;
                win_kb += t->win_n * (PG / 1024);
                if (t->win_n || t->ever_n)
                    printf(" %s %zu/%zu", strchr(t->name, '.') + 1, t->win_n * (PG / 1024), t->ever_n * (PG / 1024));
                if (csv) fprintf(csv, ",%zu,%zu", t->win_n, t->ever_n);
            }
            printf(" | window %zu KB\n", win_kb);
            if (csv) fprintf(csv, "\n");
            fflush(stdout);
            close_all();
            next += window;
        }
    }

    printf("--- %u frames, %llu page faults ---\n", g_emu_frames, (unsigned long long)g_faults);
    size_t peak = 0, ever = 0, all = 0;
    for (int i = 0; i < g_ntr; i++) {
        track_t *t = &g_tr[i];
        printf("%-14s %8zu KB  largest window %7zu KB  ever %7zu KB (%4.1f%%)\n", t->name,
               t->pages * (PG / 1024), t->peak_win * (PG / 1024), t->ever_n * (PG / 1024),
               100.0 * t->ever_n / t->pages);
        peak += t->peak_win; ever += t->ever_n; all += t->pages;
    }
    printf("%-14s %8zu KB  largest windows %6zu KB  ever %7zu KB\n", "sum", all * (PG / 1024),
           peak * (PG / 1024), ever * (PG / 1024));
    if (csv) fclose(csv);
    return 0;
}
