/*
 * overlay_plugin.h — the whole contract between m2-hle2 and an overlay plugin.
 *
 * The host loads a shared library, asks it once a composed frame for a set of
 * RGBA layers, and composites them over the finished picture. It never learns
 * what a layer contains: "composite an RGBA layer a plugin paints" is not a
 * feature of any one game, and a frame counter, a netplay ping meter and a
 * tournament lower-third are all this same host code.
 *
 * C, no C++, and NO ALLOCATION ACROSS THE BOUNDARY. The host owns every pixel
 * buffer and the plugin owns the pixels in it; that is what makes a hot reload
 * safe, because the pixels outlive the DLL and the columns keep their last
 * content across the swap.
 *
 * THE ABI IS PIXELS, NOT DRAW CALLS. The plugin never touches the GPU. sokol's
 * sg_* state is global inside the host's static library, so exporting it would
 * weld plugin and host to the same sokol commit and a backend change in the
 * emulator would silently break a plugin built last month. A command list to
 * replay is a retained-mode 2D API to invent, specify and version. The pixels
 * are the API that cannot drift.
 *
 * A plugin exports exactly one symbol:
 *
 *     const m2_overlay_api_t *m2_overlay_query(void);
 *
 * This file is COPIED into a plugin's own tree rather than shared across
 * repositories — it is forty lines and it is versioned, and a submodule for it
 * would be worse than the copy. Check M2_OVERLAY_ABI with a _Static_assert on
 * the plugin side so a skew is a build error rather than a blank overlay.
 */
#ifndef M2_OVERLAY_PLUGIN_H
#define M2_OVERLAY_PLUGIN_H

#include <stdint.h>

#define M2_OVERLAY_ABI 1

/* Eight is the table's size; the fly uses three (two columns and a banner). */
#define M2_OVERLAY_MAX_LAYERS 8

/* Levels for m2_overlay_host_t.log — the host's own LOG_INFO / WARN / ERROR. */
#define M2_OVERLAY_LOG_INFO  0
#define M2_OVERLAY_LOG_WARN  1
#define M2_OVERLAY_LOG_ERROR 2

/*
 * One layer the plugin paints. The host owns the memory and the size; the
 * plugin owns the pixels. PREMULTIPLIED BGRA8, top-left origin, `pitch` bytes
 * per row (>= w*4).
 *
 * Premultiplied because the plugin composites internally anyway, and because
 * straight alpha is what puts a dark halo around every piece of white text
 * sitting over the board.
 *
 * HOW A BUFFER COMES TO EXIST. paint() is handed the layers as the host has
 * them: on the first call, and on any call after a size change it has not
 * caught up with, `px` is NULL. A plugin that wants a size sets `w`/`h` (and
 * `id`, `x`, `y`) and returns; the host allocates or reallocates and calls
 * paint() again immediately, at most once more in the same frame. A plugin
 * whose layer sizes are stable — which is all of them, between resizes — is
 * therefore called exactly once per composed frame. paint() must tolerate
 * being called twice for one frame, and must never write through a NULL `px`.
 */
typedef struct {
    int       id;          /* stable across frames: 0..M2_OVERLAY_MAX_LAYERS-1 */
    int       x, y;        /* where it goes in the composed frame             */
    int       w, h;
    uint8_t  *px;          /* host-owned; NULL until the host has this size   */
    int       pitch;
    int       dirty;       /* plugin sets 1 if px changed this call           */
    int       visible;     /* plugin sets 0 to skip compositing entirely      */
} m2_overlay_layer_t;

/* What the host tells the plugin about the frame it is composing. */
typedef struct {
    int      canvas_w, canvas_h;   /* the whole composed picture              */
    int      game_x, game_y;       /* where the board sits inside it          */
    int      game_w, game_h;
    int      top_reserved;         /* menu-bar strip, 0 in kiosk/headless     */
    double   time_s;               /* monotonic, for the banner's pulse       */
    uint64_t frame;                /* board frames since start                */
    const char *config;            /* --overlay-args, verbatim; may be NULL   */
} m2_overlay_frame_t;

/* Host services. Logging only, for now: the plugin does its own I/O. */
typedef struct {
    uint32_t abi;
    void   (*log)(int level, const char *msg);
} m2_overlay_host_t;

typedef struct {
    uint32_t abi;                  /* must equal M2_OVERLAY_ABI               */
    const char *name;              /* for the log line                        */
    int   (*init)(const m2_overlay_host_t *host, void **ud);
    void  (*shutdown)(void *ud);
    /* Called once per composed frame, on the render thread, OUTSIDE any GPU
     * pass. Fills `layers` (up to `max`), returns how many are in use, or a
     * negative number to disable itself. Must not block. */
    int   (*paint)(void *ud, const m2_overlay_frame_t *f,
                   m2_overlay_layer_t *layers, int max);
} m2_overlay_api_t;

/* The one exported symbol a plugin must have. */
typedef const m2_overlay_api_t *(*m2_overlay_entry_fn)(void);
#define M2_OVERLAY_ENTRY_NAME "m2_overlay_query"

#endif /* M2_OVERLAY_PLUGIN_H */
