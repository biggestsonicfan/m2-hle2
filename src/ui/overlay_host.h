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
 *
 * SWAPPING TO A NEW BUILD ON A LIVE STREAM (overlay_host_request_swap, the
 * bridge's {"cmd":"overlay_swap","path":"..."}). --overlay-reload cannot do
 * this on Windows: a loaded DLL is locked, so a new build cannot be written
 * over it and the stream had to come down. A swap names a NEW file instead,
 * and the host tells the viewers before it touches anything:
 *
 *   announce  a "FLY UPDATE" toast over the bottom of the game box, for
 *             announce_s, with the old plugin still painting;
 *   standby   a "PLEASE STAND FLY" card over the game box. Once the stream
 *             has had it for a few frames the old plugin is shut down and the
 *             new one loaded -- a stall of however long the load and its
 *             init() take (about a third of a second for the fly's), spent
 *             showing the card;
 *   hold      the card stays until hold_s after it went up, with the new
 *             plugin painting its columns underneath, then the game returns.
 *
 * The toast and the card are the host's own pixels (the notice[] slots), so
 * they need no plugin and survive the gap between the two. If the new file
 * will not load, the old one -- still on disk, because it is a different
 * file -- is loaded back, and get_status says "rolled_back".
 */
#ifndef OVERLAY_HOST_H
#define OVERLAY_HOST_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sokol_gfx.h"

#include "constants.h"
#include "game_render.h"
#include "json_min.h"
#include "log.h"
#include "overlay_plugin.h"

/* The swap notices are set in the PS3 menus' open fonts. Only main.c includes
 * this file, and it does not include ps3ui.h, which carries the other copy of
 * stb_truetype's implementation: a translation unit wanting both must include
 * ps3ui.h first, and then this skips its own. */
#ifndef STB_TRUETYPE_IMPLEMENTATION
#  define STBTT_STATIC
#  define STB_TRUETYPE_IMPLEMENTATION
#  include "stb_truetype.h"
#endif
#include "ps3ui_fonts.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#  include <pthread.h>
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

    /* The host's own layers, drawn over the plugin's during a swap. */
    overlay_layer_slot_t notice[2];   /* OVERLAY_NOTICE_*                    */
    bool  notice_painted[2];          /* px holds the current text           */

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

/* ---- swap: the request, and where it has got to --------------------------- */

enum { OVERLAY_NOTICE_TOAST, OVERLAY_NOTICE_CARD };

typedef enum {
    OVERLAY_SWAP_IDLE,
    OVERLAY_SWAP_ANNOUNCE,
    OVERLAY_SWAP_STANDBY,
    OVERLAY_SWAP_HOLD,
} overlay_swap_phase_t;

/* What the bridge asks for. Empty texts and non-positive times take the
 * defaults in overlay_host_request_swap. */
typedef struct {
    char   path[512];
    char   args[1024];
    bool   has_args;                  /* replace --overlay-args with args    */
    char   title[64];
    char   note[256];
    char   card[128];                 /* '\n' separates lines                */
    double announce_s;
    double hold_s;
} overlay_swap_req_t;

#define OVERLAY_SWAP_TITLE      "FLY UPDATE"
#define OVERLAY_SWAP_NOTE       "A new overlay was detected. The stream will pause for a moment while it loads."
#define OVERLAY_SWAP_CARD       "PLEASE\nSTAND\nFLY"   /* stf-fly's standby card */
#define OVERLAY_SWAP_ANNOUNCE_S 3.0
#define OVERLAY_SWAP_HOLD_S     1.5

/* The bridge thread writes the mailbox and reads the results; the render
 * thread does everything else. Both sides take the lock only to copy, never
 * across a load. */
typedef struct {
    bool   queued;
    overlay_swap_req_t req;           /* the mailbox                          */

    overlay_swap_phase_t phase;
    overlay_swap_req_t cur;           /* the swap in progress (render thread) */
    double t_phase;
    int    frames;                    /* frames composed in this phase        */

    int    swaps;                     /* completed, whatever the result       */
    char   last[16];                  /* none | ok | rolled_back | failed     */
    char   error[256];
} overlay_swap_t;

static overlay_swap_t g_overlay_swap = { .last = "none" };

#if defined(_WIN32)
static SRWLOCK g_overlay_swap_lock = SRWLOCK_INIT;
#  define overlay_swap__lock()   AcquireSRWLockExclusive(&g_overlay_swap_lock)
#  define overlay_swap__unlock() ReleaseSRWLockExclusive(&g_overlay_swap_lock)
#else
static pthread_mutex_t g_overlay_swap_lock = PTHREAD_MUTEX_INITIALIZER;
#  define overlay_swap__lock()   pthread_mutex_lock(&g_overlay_swap_lock)
#  define overlay_swap__unlock() pthread_mutex_unlock(&g_overlay_swap_lock)
#endif

/* A swap is asked for or under way. While it is, the frame is composed with
 * the overlay's geometry even if no plugin is loaded, so the notices have a
 * game box to sit on. */
static inline bool overlay_host_swapping(void) {
    overlay_swap__lock();
    bool busy = g_overlay_swap.queued || g_overlay_swap.phase != OVERLAY_SWAP_IDLE;
    overlay_swap__unlock();
    return busy;
}

/* Whether a frame composed now should run overlay_host_paint(). */
static inline bool overlay_host_wants_paint(void) {
    return g_overlay.loaded || overlay_host_swapping();
}

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

/* keep_mapped: shut the plugin down but never unmap its code. A swap to a
 * different file does this, because a plugin thread that has not finished
 * leaving by the time shutdown() returns would otherwise go on executing
 * whatever is mapped at that address next -- which, after a swap, is the NEW
 * copy of the same DLL, loaded at the same base. stf-fly's feed thread waits
 * 300 ms and is then let go (its feed_stop), and that is exactly what killed
 * the first swap tried: the orphan ran the new copy's code with the old
 * copy's stack cookie and the CRT fast-failed (c0000409). A megabyte or two
 * of image per swap is the price; same rule as mem_init's regions. */
static inline void overlay_host__unload_ex(bool keep_mapped) {
    if (g_overlay.api && g_overlay.api->shutdown) g_overlay.api->shutdown(g_overlay.ud);
    g_overlay.ud     = NULL;
    g_overlay.api    = NULL;
    g_overlay.loaded = false;
    if (!keep_mapped) overlay_host__dlclose(g_overlay.lib);
    g_overlay.lib = NULL;
    /* The pixels stay. See rule 2 at the top of this file. */
}

static inline void overlay_host__unload(void) { overlay_host__unload_ex(false); }

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
    for (int i = 0; i < M2_OVERLAY_MAX_LAYERS + 2; i++) {
        overlay_layer_slot_t *L = i < M2_OVERLAY_MAX_LAYERS
                                ? &g_overlay.layer[i]
                                : &g_overlay.notice[i - M2_OVERLAY_MAX_LAYERS];
        if (L->view.id) { sg_destroy_view(L->view);  L->view.id = 0; }
        if (L->img.id)  { sg_destroy_image(L->img);  L->img.id  = 0; }
        free(L->px);
        L->px = NULL; L->bytes = 0; L->w = L->h = 0;
    }
    g_overlay.count = 0;
}

/*
 * Ask for a swap to the plugin at r->path (see the top of this file). Called
 * from the bridge thread; returns at once. False, with the reason in err, when
 * the file is not there or a swap is already queued or running -- nothing on
 * the stream has changed in that case.
 */
static inline bool overlay_host_request_swap(const overlay_swap_req_t *r,
                                             char *err, int cap) {
    if (!r || !r->path[0]) {
        snprintf(err, (size_t)cap, "no path");
        return false;
    }
    if (overlay_host__mtime(r->path) == 0) {
        snprintf(err, (size_t)cap, "no file at that path");
        return false;
    }
    overlay_swap_req_t q = *r;
    if (!q.title[0])        snprintf(q.title, sizeof q.title, "%s", OVERLAY_SWAP_TITLE);
    if (!q.note[0])         snprintf(q.note,  sizeof q.note,  "%s", OVERLAY_SWAP_NOTE);
    if (!q.card[0])         snprintf(q.card,  sizeof q.card,  "%s", OVERLAY_SWAP_CARD);
    if (q.announce_s <= 0)  q.announce_s = OVERLAY_SWAP_ANNOUNCE_S;
    if (q.hold_s <= 0)      q.hold_s     = OVERLAY_SWAP_HOLD_S;
    if (q.announce_s > 60)  q.announce_s = 60;
    if (q.hold_s > 60)      q.hold_s     = 60;

    overlay_swap__lock();
    bool busy = g_overlay_swap.queued || g_overlay_swap.phase != OVERLAY_SWAP_IDLE;
    if (!busy) {
        g_overlay_swap.req    = q;
        g_overlay_swap.queued = true;
    }
    overlay_swap__unlock();
    if (busy) {
        snprintf(err, (size_t)cap, "a swap is already in progress");
        return false;
    }
    LOG_INFO("overlay: swap to %s queued", q.path);
    return true;
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
    if (!g_overlay.loaded && !overlay_host_swapping()) return false;
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

/* ---- swap: the notices ---------------------------------------------------- */

enum { OVERLAY_FONT_TITLE, OVERLAY_FONT_TEXT, OVERLAY_FONTS };

static inline const stbtt_fontinfo *overlay_host__font(int font) {
    static stbtt_fontinfo info[OVERLAY_FONTS];
    static bool ready;
    if (!ready) {
        const unsigned char *data[OVERLAY_FONTS] = { ps3ui_font_title, ps3ui_font_text };
        for (int i = 0; i < OVERLAY_FONTS; i++)
            stbtt_InitFont(&info[i], data[i], stbtt_GetFontOffsetForIndex(data[i], 0));
        ready = true;
    }
    return &info[font];
}

/* The scale that makes the font's cap height `cap` pixels. */
static inline float overlay_host__font_scale(int font, float cap) {
    const stbtt_fontinfo *fi = overlay_host__font(font);
    int x0, y0, x1, y1;
    if (!stbtt_GetCodepointBox(fi, 'H', &x0, &y0, &x1, &y1) || y1 <= 0)
        return stbtt_ScaleForPixelHeight(fi, cap * 1.4f);
    return cap / (float)y1;
}

/* Width of one line (up to '\n' or the end), in pixels. */
static inline float overlay_host__text_w(int font, float cap, const char *s) {
    const stbtt_fontinfo *fi = overlay_host__font(font);
    float sc = overlay_host__font_scale(font, cap), w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && *p != '\n'; p++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(fi, *p, &adv, &lsb);
        w += (float)adv * sc;
        if (p[1] && p[1] != '\n')
            w += (float)stbtt_GetCodepointKernAdvance(fi, p[0], p[1]) * sc;
    }
    return w;
}

/* Premultiplied source-over of one colour at coverage a (0..1). */
static inline void overlay_host__blend(uint8_t *d, uint32_t rgb, float a) {
    if (a <= 0) return;
    if (a > 1) a = 1;
    float k = 1.0f - a;
    d[0] = (uint8_t)((float)( rgb        & 0xFF) * a + (float)d[0] * k + 0.5f);
    d[1] = (uint8_t)((float)((rgb >>  8) & 0xFF) * a + (float)d[1] * k + 0.5f);
    d[2] = (uint8_t)((float)((rgb >> 16) & 0xFF) * a + (float)d[2] * k + 0.5f);
    d[3] = (uint8_t)(255.0f * a + (float)d[3] * k + 0.5f);
}

static inline void overlay_host__rect(overlay_layer_slot_t *L, int x, int y, int w, int h,
                                      uint32_t rgb, float a) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > L->w ? L->w : x + w, y1 = y + h > L->h ? L->h : y + h;
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++)
            overlay_host__blend(L->px + (size_t)j * (size_t)L->pitch + (size_t)i * 4u, rgb, a);
}

/* One line of text, its baseline at y, clipped to the layer and to max_x. */
static inline void overlay_host__text(overlay_layer_slot_t *L, int font, float cap,
                                      float x, float y, float max_x,
                                      const char *s, uint32_t rgb) {
    const stbtt_fontinfo *fi = overlay_host__font(font);
    float sc = overlay_host__font_scale(font, cap);
    for (const unsigned char *p = (const unsigned char *)s; *p && *p != '\n'; p++) {
        int adv, lsb, gw, gh, gx, gy;
        stbtt_GetCodepointHMetrics(fi, *p, &adv, &lsb);
        unsigned char *bm = stbtt_GetCodepointBitmap(fi, sc, sc, *p, &gw, &gh, &gx, &gy);
        if (bm) {
            int ox = (int)lroundf(x) + gx, oy = (int)lroundf(y) + gy;
            for (int j = 0; j < gh; j++) {
                int py = oy + j;
                if (py < 0 || py >= L->h) continue;
                for (int i = 0; i < gw; i++) {
                    int px = ox + i;
                    if (px < 0 || px >= L->w || (float)px >= max_x) continue;
                    unsigned char c = bm[j * gw + i];
                    if (c)
                        overlay_host__blend(L->px + (size_t)py * (size_t)L->pitch
                                                  + (size_t)px * 4u, rgb, (float)c / 255.0f);
                }
            }
            stbtt_FreeBitmap(bm, NULL);
        }
        x += (float)adv * sc;
        if (p[1] && p[1] != '\n')
            x += (float)stbtt_GetCodepointKernAdvance(fi, p[0], p[1]) * sc;
    }
}

/* The standby card: stf-fly's (flystf/stream.py standby_card) -- its
 * #0e1116 ground and #c3c2b7 words, one line a row, over the whole game box. */
static inline void overlay_host__paint_card(overlay_layer_slot_t *L, const char *text) {
    overlay_host__rect(L, 0, 0, L->w, L->h, 0x0E1116, 1.0f);
    int lines = 1;
    for (const char *p = text; *p; p++) lines += *p == '\n';
    float cap = (float)L->h / 9.0f * 0.72f;
    float pitch = cap * 1.9f;
    float y = (float)L->h / 2.0f - pitch * (float)(lines - 1) / 2.0f + cap / 2.0f;
    for (const char *line = text; line; ) {
        float w = overlay_host__text_w(OVERLAY_FONT_TITLE, cap, line);
        overlay_host__text(L, OVERLAY_FONT_TITLE, cap, ((float)L->w - w) / 2.0f, y,
                           (float)L->w, line, 0xC3C2B7);
        y += pitch;
        line = strchr(line, '\n');
        if (line) line++;
    }
}

/* The toast: the fly's own banner (stf-fly plugin/src/layout.c layout_banner)
 * in its "Updating the fly" colours -- navy plate, white rail, amber bar and
 * label, pale note. `U` is the banner's unit. */
static inline void overlay_host__toast_size(const overlay_swap_req_t *r, float U, int max_w,
                                            int *w, int *h) {
    float tw = 4 + 14 + overlay_host__text_w(OVERLAY_FONT_TITLE, 9.0f * U, r->title) / U
                  + 10 + overlay_host__text_w(OVERLAY_FONT_TEXT, 8.0f * U, r->note) / U + 14;
    *w = (int)ceilf(tw * U);
    if (*w > max_w) *w = max_w;
    *h = (int)ceilf(30 * U);
}

static inline void overlay_host__paint_toast(overlay_layer_slot_t *L, const overlay_swap_req_t *r,
                                             float U) {
    memset(L->px, 0, L->bytes);
    overlay_host__rect(L, 0, 0, L->w, L->h, 0x132241, 0.92f);
    overlay_host__rect(L, 0, 0, L->w, (int)lroundf(6 * U), 0x595959, 1.0f);
    overlay_host__rect(L, 0, (int)lroundf(1.5f * U), L->w, (int)lroundf(3 * U), 0xFFFFFF, 1.0f);
    overlay_host__rect(L, 0, 0, (int)lroundf(4 * U), L->h, 0xFFD166, 1.0f);
    float base = (6 + 12) * U + 4.5f * U;              /* middle of the plate body */
    float x = (4 + 14) * U;
    overlay_host__text(L, OVERLAY_FONT_TITLE, 9.0f * U, x, base, (float)L->w,
                       r->title, 0xFFD166);
    x += overlay_host__text_w(OVERLAY_FONT_TITLE, 9.0f * U, r->title) + 10 * U;
    overlay_host__text(L, OVERLAY_FONT_TEXT, 8.0f * U, x, base, (float)L->w - 10 * U,
                       r->note, 0xB9C5DE);
}

/* Size, place and (when that changed) repaint the notices for this phase. */
static inline void overlay_host__notices(overlay_swap_phase_t phase, const overlay_swap_req_t *r,
                                         int canvas_h, int game_x, int game_y,
                                         int game_w, int game_h) {
    overlay_layer_slot_t *toast = &g_overlay.notice[OVERLAY_NOTICE_TOAST];
    overlay_layer_slot_t *card  = &g_overlay.notice[OVERLAY_NOTICE_CARD];
    toast->visible = card->visible = false;
    if (phase == OVERLAY_SWAP_IDLE) return;

    if (phase == OVERLAY_SWAP_STANDBY || phase == OVERLAY_SWAP_HOLD) {
        if (card->w != game_w || card->h != game_h || !card->px) {
            overlay_host__resize_slot(card, game_w, game_h);
            g_overlay.notice_painted[OVERLAY_NOTICE_CARD] = false;
        }
        if (card->px) {
            if (!g_overlay.notice_painted[OVERLAY_NOTICE_CARD]) {
                overlay_host__paint_card(card, r->card);
                g_overlay.notice_painted[OVERLAY_NOTICE_CARD] = true;
                card->uploaded = false;
            }
            card->x = game_x;
            card->y = game_y;
            card->visible = true;
        }
    }

    /* 1.6 units a pixel at 1080 lines: a 48-pixel plate, a little larger than
     * the fly's own banner, because this one is for the viewers. */
    float U = (float)canvas_h / 1080.0f * 1.6f;
    if (U < 0.5f) U = 0.5f;
    int tw, th;
    overlay_host__toast_size(r, U, game_w - (int)(32 * U), &tw, &th);
    if (tw <= 0 || th <= 0) return;
    if (toast->w != tw || toast->h != th || !toast->px) {
        overlay_host__resize_slot(toast, tw, th);
        g_overlay.notice_painted[OVERLAY_NOTICE_TOAST] = false;
    }
    if (!toast->px) return;
    if (!g_overlay.notice_painted[OVERLAY_NOTICE_TOAST]) {
        overlay_host__paint_toast(toast, r, U);
        g_overlay.notice_painted[OVERLAY_NOTICE_TOAST] = true;
        toast->uploaded = false;
    }
    toast->x = game_x + (game_w - tw) / 2;
    toast->y = game_y + game_h - th - (int)(24 * U);
    toast->visible = true;
}

/* ---- swap: the phases ----------------------------------------------------- */

static inline void overlay_host__swap_result(const char *last, const char *error) {
    overlay_swap__lock();
    g_overlay_swap.swaps++;
    snprintf(g_overlay_swap.last,  sizeof g_overlay_swap.last,  "%s", last);
    snprintf(g_overlay_swap.error, sizeof g_overlay_swap.error, "%s", error ? error : "");
    overlay_swap__unlock();
}

/* Unload whatever is running and load cur.path; on failure, load back what
 * was running before. The stall this causes is spent showing the card. */
static inline void overlay_host__swap_now(const overlay_swap_req_t *r) {
    char old_path[sizeof g_overlay.path], old_args[sizeof g_overlay.args];
    bool had = g_overlay.loaded;
    snprintf(old_path, sizeof old_path, "%s", g_overlay.path);
    snprintf(old_args, sizeof old_args, "%s", g_overlay.args);

    LOG_INFO("overlay: swapping %s for %s", had ? old_path : "(nothing)", r->path);
    /* The old image stays mapped (see overlay_host__unload_ex) unless the new
     * one has the same path: the loader would hand the old module straight
     * back. Rolling back to a mapped image just takes another reference. */
#if defined(_WIN32)
    bool same = _stricmp(old_path, r->path) == 0;
#else
    bool same = strcmp(old_path, r->path) == 0;
#endif
    if (had) overlay_host__unload_ex(!same);
    overlay_host_set_path(r->path);
    if (r->has_args) overlay_host_set_args(r->args);
    g_overlay.gave_up = false;

    if (overlay_host__load(false)) {
        g_overlay.reloads++;
        overlay_host__swap_result("ok", NULL);
        return;
    }
    if (had) {
        overlay_host_set_path(old_path);
        overlay_host_set_args(old_args);
        if (overlay_host__load(false)) {
            LOG_WARN("overlay: %s would not load - back on %s", r->path, old_path);
            overlay_host__swap_result("rolled_back",
                "the new plugin did not load (see m2hle.log); the old one is running again");
            return;
        }
    }
    g_overlay.gave_up = true;
    LOG_WARN("overlay: swap to %s failed and nothing is loaded", r->path);
    overlay_host__swap_result("failed",
        had ? "neither the new plugin nor the old one would load (see m2hle.log)"
            : "the new plugin did not load (see m2hle.log)");
}

/* Advance the swap by one composed frame. Returns the phase to draw. */
static inline overlay_swap_phase_t overlay_host__swap_step(double now) {
    overlay_swap_t *s = &g_overlay_swap;
    overlay_swap__lock();
    if (s->phase == OVERLAY_SWAP_IDLE && s->queued) {
        s->cur     = s->req;
        s->queued  = false;
        s->phase   = OVERLAY_SWAP_ANNOUNCE;
        s->t_phase = now;
        s->frames  = 0;
        g_overlay.notice_painted[0] = g_overlay.notice_painted[1] = false;
    }
    overlay_swap_phase_t phase = s->phase;
    overlay_swap__unlock();

    overlay_swap_phase_t next = phase;
    switch (phase) {
        case OVERLAY_SWAP_IDLE:
            return phase;
        case OVERLAY_SWAP_ANNOUNCE:
            if (now - s->t_phase >= s->cur.announce_s) next = OVERLAY_SWAP_STANDBY;
            break;
        case OVERLAY_SWAP_STANDBY:
            /* The card has to be in a frame that went OUT before the stall,
             * or the viewers watch the last game frame freeze instead. The
             * A/V capture maps a frame back a few frames after it is drawn
             * (av_capture.h's staging ring), and a render thread blocked in
             * LoadLibrary maps nothing, so two frames of card were measured
             * to be not enough: the stream froze on the game for the whole
             * load. Eight frames and a sixth of a second clear the ring. */
            if (s->frames >= 8 && now - s->t_phase >= 0.15) {
                overlay_host__swap_now(&s->cur);
                next = OVERLAY_SWAP_HOLD;           /* t_phase: when the card went up */
            }
            break;
        case OVERLAY_SWAP_HOLD:
            if (now - s->t_phase >= s->cur.hold_s) next = OVERLAY_SWAP_IDLE;
            break;
    }
    if (next != phase) {
        s->frames = 0;
        if (next != OVERLAY_SWAP_HOLD) s->t_phase = now;
        overlay_swap__lock();
        s->phase = next;
        overlay_swap__unlock();
        if (next == OVERLAY_SWAP_IDLE) LOG_INFO("overlay: swap finished (%s)", s->last);
        phase = next;
    }
    s->frames++;
    return phase;
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
    /* A swap first: it may unload one plugin and load another, and its notices
     * are the host's own, so they go up whether or not a plugin is running. */
    overlay_swap_phase_t phase = overlay_host__swap_step(overlay_host__now_s());
    overlay_host__notices(phase, &g_overlay_swap.cur, canvas_h,
                          game_x, game_y, game_w, game_h);
    for (int i = 0; i < 2; i++) {
        overlay_layer_slot_t *L = &g_overlay.notice[i];
        if (!L->visible) continue;
        overlay_host__ensure_image(L);
        if (L->img.id && !L->uploaded) {
            sg_update_image(L->img, &(sg_image_data){
                .mip_levels[0] = { .ptr = L->px, .size = L->bytes } });
            L->uploaded = true;
        }
    }

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

    if (phase == OVERLAY_SWAP_IDLE) overlay_host__maybe_reload();
}

/* Composite what the last overlay_host_paint() produced. Call INSIDE the pass
 * that is showing the board, after the board has been drawn. */
static inline void overlay_host_draw(void) {
    for (int i = 0; i < g_overlay.count; i++) {
        const overlay_layer_slot_t *L = &g_overlay.layer[i];
        if (!L->visible || !L->view.id) continue;
        game_render_draw_overlay(L->view, L->x, L->y, L->w, L->h);
    }
    /* The swap's notices last: the card over the game box, the toast over it. */
    for (int i = OVERLAY_NOTICE_CARD; i >= OVERLAY_NOTICE_TOAST; i--) {
        const overlay_layer_slot_t *L = &g_overlay.notice[i];
        if (!L->visible || !L->view.id) continue;
        game_render_draw_overlay(L->view, L->x, L->y, L->w, L->h);
    }
}

/* ---- status -------------------------------------------------------------- */

/* The "overlay" block of get_status, so broadcast.py can say on its own
 * console whether the thing is actually running and a probe can assert it. */
static inline void overlay_host_status_json(char *out, int cap) {
    /* The swap, read under its lock: the bridge thread asks while the render
     * thread moves it along. */
    overlay_swap__lock();
    const char *state = g_overlay_swap.queued ? "queued" :
        g_overlay_swap.phase == OVERLAY_SWAP_ANNOUNCE ? "announce" :
        g_overlay_swap.phase == OVERLAY_SWAP_STANDBY  ? "standby"  :
        g_overlay_swap.phase == OVERLAY_SWAP_HOLD     ? "hold"     : "idle";
    int  swaps = g_overlay_swap.swaps;
    char last[16], err[256], err_js[512];
    snprintf(last, sizeof last, "%s", g_overlay_swap.last);
    snprintf(err,  sizeof err,  "%s", g_overlay_swap.error);
    overlay_swap__unlock();
    json_escape(err_js, (int)sizeof err_js, err);
    char swap[800];
    snprintf(swap, sizeof swap,
             "{\"state\":\"%s\",\"swaps\":%d,\"last\":\"%s\",\"error\":\"%s\"}",
             state, swaps, last, err_js);

    if (!g_overlay.want) {
        snprintf(out, (size_t)cap, "{\"loaded\":false,\"swap\":%s}", swap);
        return;
    }
    char name[96];
    snprintf(name, sizeof name, "%s",
             (g_overlay.loaded && g_overlay.api && g_overlay.api->name)
                 ? g_overlay.api->name : "");
    for (char *p = name; *p; p++) if (*p == '"' || *p == '\\') *p = '\'';
    char path_js[1100];
    json_escape(path_js, (int)sizeof path_js, g_overlay.path);

    snprintf(out, (size_t)cap,
             "{\"loaded\":%s,\"name\":\"%s\",\"path\":\"%s\",\"layers\":%d,"
             "\"paint_us\":%.0f,\"reloads\":%d,\"watch\":%s,\"state\":\"%s\",\"swap\":%s}",
             g_overlay.loaded ? "true" : "false", name, path_js, g_overlay.count,
             g_overlay.last_paint_us, g_overlay.reloads,
             g_overlay.watch ? "true" : "false",
             g_overlay.loaded ? "running" : (g_overlay.gave_up ? "failed" : "pending"),
             swap);
}

#endif /* OVERLAY_HOST_H */
