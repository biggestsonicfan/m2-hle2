/*
 * overlay_host.h — m2-hle2's side of ui/overlay_plugin.h: load a shared
 * library, ask it once a composed frame for a set of premultiplied BGRA
 * layers, and composite them over the finished picture.
 *
 * NOTHING HERE KNOWS WHAT IS BEING EMULATED, and nothing here knows what a
 * layer contains. That is the whole point: the host shell is deliberately
 * board-independent (see kiosk.h, which says so outright), so an overlay that
 * knows about hyper meters and compound eyes lives in a DLL that ships with
 * whatever is drawing it, and this file is the ninety lines that let it.
 *
 * Nothing in here changes any behaviour when no plugin is loaded: every entry
 * point returns immediately and overlay_host_game_rect() answers false so the
 * caller keeps the rect it already had.
 *
 * THREE RULES THAT ARE NOT OBVIOUS:
 *
 *  1. sg_update_image() may not be called inside a render pass, so the work
 *     is split: overlay_host_paint() runs the plugin and uploads, BEFORE
 *     sg_begin_pass(); overlay_host_draw() draws the quads, INSIDE the pass.
 *     Paint once a frame; draw in whichever pass is showing the board.
 *
 *  2. The pixel buffers are host-owned and outlive the DLL. That is what
 *     makes the hot reload in overlay_host__maybe_reload() safe — the columns
 *     keep their last content across the swap and nothing blinks — and it is
 *     also the same rule as mem_init's heap regions (see CLAUDE.md, "Memory
 *     Bus"): a buffer another thread might be reading is not freed because
 *     the thing that filled it went away.
 *
 *  3. A plugin that faults or misbehaves must not take down a stream that is
 *     live. Every failure path here is a LOG_WARN and a disabled overlay, and
 *     the board carries on being presented. A plugin can disable itself by
 *     returning a negative count from paint().
 */
#ifndef OVERLAY_HOST_H
#define OVERLAY_HOST_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sokol_gfx.h"

#include "constants.h"
#include "game_render.h"
#include "log.h"
#include "overlay_plugin.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#  include <sys/stat.h>
#  include <time.h>
#endif

/* ---- state --------------------------------------------------------------- */

typedef struct {
    /* Host-owned pixels, and the GPU image that carries them. Both survive a
     * plugin reload; only a size change throws either away. */
    uint8_t *px;
    size_t   bytes;
    int      w, h, pitch;
    int      x, y;
    bool     visible;
    bool     uploaded;          /* px has reached the image at least once    */

    sg_image img;
    sg_view  view;
    int      img_w, img_h;
} overlay_layer_slot_t;

typedef struct {
    /* configuration, from the command line */
    bool  want;                       /* --overlay was given                 */
    char  path[512];
    char  args[1024];
    bool  watch;                      /* --overlay-reload                    */
    bool  have_game_rect;             /* --overlay-game WxH+X+Y              */
    int   game_rect[4];

    /* the loaded plugin */
    void *lib;
    const m2_overlay_api_t *api;
    void *ud;
    bool  loaded;
    bool  gave_up;                    /* a failure we will not retry         */

    overlay_layer_slot_t layer[M2_OVERLAY_MAX_LAYERS];
    int   count;                      /* layers in use this frame            */

    /* reload watch */
    uint64_t mtime;                   /* of the DLL we are running           */
    uint64_t pending_mtime;           /* a newer one we have seen             */
    int      pending_frames;          /* ...for this many consecutive checks  */
    int      reloads;

    /* status */
    double   last_paint_us;
    uint64_t paints;
    int      second_passes;
} overlay_host_t;

static overlay_host_t g_overlay;

static inline bool overlay_host_loaded(void) { return g_overlay.loaded; }

/* ---- platform: the library, and the file's timestamp --------------------- */

static inline uint64_t overlay_host__mtime(const char *path) {
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return 0;
    return ((uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32) |
            (uint64_t)fa.ftLastWriteTime.dwLowDateTime;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (uint64_t)st.st_mtime;
#endif
}

static inline void *overlay_host__dlopen(const char *path) {
#if defined(_WIN32)
    /* Widen so a plugin under a path with non-ASCII in it still loads, and
     * ask for the DLL's own directory on the search path so a plugin may
     * bring libraries of its own without putting them beside m2hle.exe. */
    wchar_t wide[1024];
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, wide,
                                (int)(sizeof wide / sizeof wide[0]));
    if (n <= 0) return NULL;
    return (void *)LoadLibraryExW(wide, NULL,
                                  LOAD_WITH_ALTERED_SEARCH_PATH);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

static inline void overlay_host__dlclose(void *lib) {
#if defined(_WIN32)
    if (lib) FreeLibrary((HMODULE)lib);
#else
    if (lib) dlclose(lib);
#endif
}

static inline void *overlay_host__dlsym(void *lib, const char *sym) {
#if defined(_WIN32)
    return (void *)GetProcAddress((HMODULE)lib, sym);
#else
    return dlsym(lib, sym);
#endif
}

static inline const char *overlay_host__dlerror(void) {
#if defined(_WIN32)
    static char buf[256];
    DWORD e = GetLastError();
    snprintf(buf, sizeof buf, "windows error %lu", (unsigned long)e);
    return buf;
#else
    const char *e = dlerror();
    return e ? e : "unknown";
#endif
}

static inline double overlay_host__now_s(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

/* ---- the one host service the plugin gets -------------------------------- */

static void overlay_host__log(int level, const char *msg) {
    if (!msg) return;
    switch (level) {
        case M2_OVERLAY_LOG_ERROR: LOG_ERROR("overlay: %s", msg); break;
        case M2_OVERLAY_LOG_WARN:  LOG_WARN ("overlay: %s", msg); break;
        default:                   LOG_INFO ("overlay: %s", msg); break;
    }
}

static const m2_overlay_host_t g_overlay_services = {
    .abi = M2_OVERLAY_ABI,
    .log = overlay_host__log,
};

/* ---- command line -------------------------------------------------------- */

static inline void overlay_host_set_path(const char *path) {
    snprintf(g_overlay.path, sizeof g_overlay.path, "%s", path ? path : "");
    g_overlay.want = g_overlay.path[0] != 0;
}

static inline void overlay_host_set_args(const char *args) {
    snprintf(g_overlay.args, sizeof g_overlay.args, "%s", args ? args : "");
}

static inline void overlay_host_set_watch(bool on) { g_overlay.watch = on; }

/* --overlay-game WxH+X+Y — where the board goes inside the composed frame,
 * for the cases where the 496:384 letterbox is not what the scene wants.
 * Returns false (and changes nothing) on a string it cannot read. */
static inline bool overlay_host_set_game_rect(const char *spec) {
    int w = 0, h = 0, x = 0, y = 0;
    if (!spec) return false;
    if (sscanf(spec, "%dx%d+%d+%d", &w, &h, &x, &y) != 4) {
        if (sscanf(spec, "%dx%d", &w, &h) != 2) return false;
        x = y = 0;
    }
    if (w <= 0 || h <= 0) return false;
    g_overlay.game_rect[0] = x;
    g_overlay.game_rect[1] = y;
    g_overlay.game_rect[2] = w;
    g_overlay.game_rect[3] = h;
    g_overlay.have_game_rect = true;
    return true;
}

/* ---- load / unload ------------------------------------------------------- */

static inline void overlay_host__unload(void) {
    if (g_overlay.api && g_overlay.api->shutdown) g_overlay.api->shutdown(g_overlay.ud);
    g_overlay.ud     = NULL;
    g_overlay.api    = NULL;
    g_overlay.loaded = false;
    overlay_host__dlclose(g_overlay.lib);
    g_overlay.lib = NULL;
    /* The pixels stay. See rule 2 at the top of this file. */
}

/* Load the configured plugin. Any failure is a warning and a disabled
 * overlay; the emulator is never held up by one. */
static inline bool overlay_host__load(bool quiet) {
    if (!g_overlay.want || !g_overlay.path[0]) return false;

    void *lib = overlay_host__dlopen(g_overlay.path);
    if (!lib) {
        if (!quiet) LOG_WARN("overlay: cannot load %s (%s) - carrying on without it",
                             g_overlay.path, overlay_host__dlerror());
        return false;
    }
    m2_overlay_entry_fn entry =
        (m2_overlay_entry_fn)overlay_host__dlsym(lib, M2_OVERLAY_ENTRY_NAME);
    if (!entry) {
        LOG_WARN("overlay: %s exports no %s - not an overlay plugin",
                 g_overlay.path, M2_OVERLAY_ENTRY_NAME);
        overlay_host__dlclose(lib);
        return false;
    }
    const m2_overlay_api_t *api = entry();
    if (!api) {
        LOG_WARN("overlay: %s declined to start (%s returned NULL)",
                 g_overlay.path, M2_OVERLAY_ENTRY_NAME);
        overlay_host__dlclose(lib);
        return false;
    }
    if (api->abi != M2_OVERLAY_ABI) {
        LOG_WARN("overlay: %s is ABI %u, this build speaks %u - rebuild the plugin",
                 g_overlay.path, (unsigned)api->abi, (unsigned)M2_OVERLAY_ABI);
        overlay_host__dlclose(lib);
        return false;
    }
    if (!api->paint) {
        LOG_WARN("overlay: %s has no paint()", g_overlay.path);
        overlay_host__dlclose(lib);
        return false;
    }
    void *ud = NULL;
    if (api->init && api->init(&g_overlay_services, &ud) != 0) {
        LOG_WARN("overlay: %s failed to initialise", api->name ? api->name : g_overlay.path);
        overlay_host__dlclose(lib);
        return false;
    }
    g_overlay.lib    = lib;
    g_overlay.api    = api;
    g_overlay.ud     = ud;
    g_overlay.loaded = true;
    g_overlay.mtime  = overlay_host__mtime(g_overlay.path);
    g_overlay.pending_mtime  = 0;
    g_overlay.pending_frames = 0;
    LOG_INFO("overlay: %s loaded from %s%s",
             api->name ? api->name : "(unnamed)", g_overlay.path,
             g_overlay.watch ? " (watching for rebuilds)" : "");
    return true;
}

/* Call once after sg_setup(). Does nothing at all without --overlay. */
static inline void overlay_host_init(void) {
    if (!g_overlay.want) return;
    if (!overlay_host__load(false)) g_overlay.gave_up = true;
}

static inline void overlay_host_shutdown(void) {
    if (g_overlay.loaded) overlay_host__unload();
    for (int i = 0; i < M2_OVERLAY_MAX_LAYERS; i++) {
        overlay_layer_slot_t *L = &g_overlay.layer[i];
        if (L->view.id) { sg_destroy_view(L->view);  L->view.id = 0; }
        if (L->img.id)  { sg_destroy_image(L->img);  L->img.id  = 0; }
        free(L->px);
        L->px = NULL; L->bytes = 0; L->w = L->h = 0;
    }
    g_overlay.count = 0;
}

/* ---- geometry ------------------------------------------------------------ */

/*
 * Where the board goes inside a canvas the overlay is sharing. Returns false
 * when no plugin is loaded, and then leaves the rect alone so the caller keeps
 * whatever it was already doing — which is how "nothing changes with no
 * plugin" stays true by construction rather than by inspection.
 *
 * The default is the board's own 496:384 letterboxed into the canvas below
 * `top`, which at 1920x1080 leaves 262 px either side — the same column width
 * overlay.html computes from the same arithmetic, and the same thing
 * --kiosk-size 1920x1080 already letterboxes to.
 */
static inline bool overlay_host_game_rect(int canvas_w, int canvas_h, int top,
                                           int *ox, int *oy, int *w, int *h) {
    if (!g_overlay.loaded) return false;
    if (canvas_w <= 0 || canvas_h <= 0) return false;

    if (g_overlay.have_game_rect) {
        *ox = g_overlay.game_rect[0];
        *oy = g_overlay.game_rect[1];
        *w  = g_overlay.game_rect[2];
        *h  = g_overlay.game_rect[3];
        return true;
    }
    int avail = canvas_h - top;
    if (avail < 1) avail = 1;
    game_render_letterbox(canvas_w, avail, VIDEO_WIDTH, VIDEO_HEIGHT, ox, oy, w, h);
    *oy += top;
    return true;
}

/* ---- per-frame ----------------------------------------------------------- */

static inline void overlay_host__resize_slot(overlay_layer_slot_t *L, int w, int h) {
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (L->w == w && L->h == h && (w == 0 || L->px)) return;

    free(L->px);
    L->px    = NULL;
    L->bytes = 0;
    L->w = w; L->h = h; L->pitch = w * 4;
    L->uploaded = false;
    if (w > 0 && h > 0) {
        L->bytes = (size_t)w * (size_t)h * 4u;
        L->px    = (uint8_t *)calloc(1, L->bytes);
        if (!L->px) { L->bytes = 0; L->w = L->h = L->pitch = 0; }
    }
    /* The GPU image follows the buffer, not the other way round. */
    if (L->view.id) { sg_destroy_view(L->view);  L->view.id = 0; }
    if (L->img.id)  { sg_destroy_image(L->img);  L->img.id  = 0; }
    L->img_w = L->img_h = 0;
}

static inline void overlay_host__ensure_image(overlay_layer_slot_t *L) {
    if (L->w <= 0 || L->h <= 0 || !L->px) return;
    if (L->img.id && L->img_w == L->w && L->img_h == L->h) return;
    if (L->view.id) { sg_destroy_view(L->view);  L->view.id = 0; }
    if (L->img.id)  { sg_destroy_image(L->img);  L->img.id  = 0; }
    L->img = sg_make_image(&(sg_image_desc){
        .width        = L->w,
        .height       = L->h,
        .pixel_format = SG_PIXELFORMAT_RGBA8,   /* the shader does the BGRA swap */
        .usage        = { .stream_update = true },
        .label        = "overlay-layer",
    });
    L->view = sg_make_view(&(sg_view_desc){
        .texture.image = L->img, .label = "overlay-layer-view" });
    L->img_w = L->w;
    L->img_h = L->h;
}

/* Fill the descriptor array from what the host currently holds, so the plugin
 * sees its own buffers back and only has to say what changed. */
static inline void overlay_host__fill_descs(m2_overlay_layer_t *d, int max) {
    for (int i = 0; i < max; i++) {
        overlay_layer_slot_t *L = &g_overlay.layer[i];
        d[i].id      = i;
        d[i].x       = L->x;
        d[i].y       = L->y;
        d[i].w       = L->w;
        d[i].h       = L->h;
        d[i].px      = L->px;
        d[i].pitch   = L->pitch;
        d[i].dirty   = 0;
        d[i].visible = 0;
    }
}

static inline void overlay_host__maybe_reload(void) {
    if (!g_overlay.watch || !g_overlay.loaded) return;
    uint64_t m = overlay_host__mtime(g_overlay.path);
    if (m == 0 || m == g_overlay.mtime) { g_overlay.pending_frames = 0; return; }
    /* A linker writes a DLL in pieces. Only swap once the timestamp has stopped
     * moving, or we load half a file and log a mystery. */
    if (m != g_overlay.pending_mtime) {
        g_overlay.pending_mtime  = m;
        g_overlay.pending_frames = 1;
        return;
    }
    if (++g_overlay.pending_frames < 3) return;

    LOG_INFO("overlay: %s changed on disk - reloading", g_overlay.path);
    overlay_host__unload();
    if (overlay_host__load(true)) {
        g_overlay.reloads++;
    } else {
        /* The old DLL's file is gone, so there is nothing to take back. */
        LOG_WARN("overlay: the reload failed and the old plugin is already "
                 "unloaded - the overlay is off until the next good build");
        g_overlay.mtime = overlay_host__mtime(g_overlay.path);
        g_overlay.pending_mtime  = 0;
        g_overlay.pending_frames = 0;
    }
}

/*
 * Run the plugin for this composed frame and upload whatever it repainted.
 * MUST be called outside a render pass (sg_update_image says so).
 *
 * canvas_w/h is the whole picture; game_* is where the board sits in it (from
 * overlay_host_game_rect); top is the menu-bar strip the window reserves, 0
 * for a kiosk or a headless tap.
 */
static inline void overlay_host_paint(int canvas_w, int canvas_h,
                                       int game_x, int game_y,
                                       int game_w, int game_h,
                                       int top, uint64_t board_frame) {
    g_overlay.count = 0;
    if (!g_overlay.loaded) return;

    m2_overlay_frame_t f = {
        .canvas_w     = canvas_w,
        .canvas_h     = canvas_h,
        .game_x       = game_x,
        .game_y       = game_y,
        .game_w       = game_w,
        .game_h       = game_h,
        .top_reserved = top,
        .time_s       = overlay_host__now_s(),
        .frame        = board_frame,
        .config       = g_overlay.args[0] ? g_overlay.args : NULL,
    };

    m2_overlay_layer_t d[M2_OVERLAY_MAX_LAYERS];
    double t0 = f.time_s;
    int n = 0;

    /* Up to two passes, and the second one only ever happens on a size change:
     * the first tells the host what size the plugin wants, the second paints
     * into the buffer the host then made. See the contract in
     * overlay_plugin.h — a plugin with stable layer sizes is called once. */
    for (int pass = 0; pass < 2; pass++) {
        overlay_host__fill_descs(d, M2_OVERLAY_MAX_LAYERS);
        n = g_overlay.api->paint(g_overlay.ud, &f, d, M2_OVERLAY_MAX_LAYERS);
        if (n < 0) {
            LOG_WARN("overlay: %s disabled itself",
                     g_overlay.api->name ? g_overlay.api->name : g_overlay.path);
            overlay_host__unload();
            g_overlay.gave_up = true;
            return;
        }
        if (n > M2_OVERLAY_MAX_LAYERS) n = M2_OVERLAY_MAX_LAYERS;

        bool resized = false;
        for (int i = 0; i < n; i++) {
            overlay_layer_slot_t *L = &g_overlay.layer[i];
            if (d[i].w != L->w || d[i].h != L->h || (d[i].w > 0 && !L->px)) {
                overlay_host__resize_slot(L, d[i].w, d[i].h);
                resized = true;
            }
        }
        /* Slots the plugin has stopped using go back to nothing, so a plugin
         * that drops a layer does not leave a megabyte and an image behind. */
        for (int i = n; i < M2_OVERLAY_MAX_LAYERS; i++) {
            overlay_layer_slot_t *L = &g_overlay.layer[i];
            if (L->px || L->img.id) overlay_host__resize_slot(L, 0, 0);
        }
        if (!resized) break;
        if (pass == 1) break;
        g_overlay.second_passes++;
    }

    for (int i = 0; i < n; i++) {
        overlay_layer_slot_t *L = &g_overlay.layer[i];
        L->x       = d[i].x;
        L->y       = d[i].y;
        L->visible = d[i].visible != 0 && L->px && L->w > 0 && L->h > 0;
        if (!L->visible) continue;
        overlay_host__ensure_image(L);
        /* Only on a frame the plugin marked dirty. sokol has no partial image
         * update, which is exactly why the layer split matters: a repaint of
         * one 262x1080 column is 1.1 MB, not the whole 8.3 MB canvas. An image
         * that has just been (re)made has to be filled whatever it says. */
        if (L->img.id && (d[i].dirty || !L->uploaded)) {
            sg_update_image(L->img, &(sg_image_data){
                .mip_levels[0] = { .ptr = L->px, .size = L->bytes } });
            L->uploaded = true;
        }
    }

    g_overlay.count = n;
    g_overlay.last_paint_us = (overlay_host__now_s() - t0) * 1e6;
    g_overlay.paints++;

    overlay_host__maybe_reload();
}

/* Composite what the last overlay_host_paint() produced. Call INSIDE the pass
 * that is showing the board, after the board has been drawn. */
static inline void overlay_host_draw(void) {
    for (int i = 0; i < g_overlay.count; i++) {
        const overlay_layer_slot_t *L = &g_overlay.layer[i];
        if (!L->visible || !L->view.id) continue;
        game_render_draw_overlay(L->view, L->x, L->y, L->w, L->h);
    }
}

/* ---- status -------------------------------------------------------------- */

/* The "overlay" block of get_status, so broadcast.py can say on its own
 * console whether the thing is actually running and a probe can assert it. */
static inline void overlay_host_status_json(char *out, int cap) {
    if (!g_overlay.want) {
        snprintf(out, (size_t)cap, "{\"loaded\":false}");
        return;
    }
    char name[96];
    snprintf(name, sizeof name, "%s",
             (g_overlay.loaded && g_overlay.api && g_overlay.api->name)
                 ? g_overlay.api->name : "");
    for (char *p = name; *p; p++) if (*p == '"' || *p == '\\') *p = '\'';

    snprintf(out, (size_t)cap,
             "{\"loaded\":%s,\"name\":\"%s\",\"layers\":%d,\"paint_us\":%.0f,"
             "\"reloads\":%d,\"watch\":%s,\"state\":\"%s\"}",
             g_overlay.loaded ? "true" : "false", name, g_overlay.count,
             g_overlay.last_paint_us, g_overlay.reloads,
             g_overlay.watch ? "true" : "false",
             g_overlay.loaded ? "running" : (g_overlay.gave_up ? "failed" : "pending"));
}

#endif /* OVERLAY_HOST_H */
