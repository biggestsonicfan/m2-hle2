/*
 * frame_times.h — where a rendered frame's host time goes.
 *
 * game_frame.h accumulates these, in microseconds, on the thread that renders
 * (the main thread of every frontend). A frontend, the bench or the MCP
 * bridge's get_status reads them; nothing in the emulation does. On its own
 * here so the bridge can report them without pulling in the renderer.
 */
#ifndef FRAME_TIMES_H
#define FRAME_TIMES_H

#include <stdint.h>

typedef struct {
    uint64_t frames;      /* game_frame_draw calls */
    int64_t compose_us;   /* tile layers → RGBA textures */
    int64_t scan_us;      /* this frame's 3D output → model list */
    int64_t upload_us;    /* texture atlas + luma/colorxlat LUTs */
    int64_t draw3d_us;    /* model decode + fill/line submission */
    int64_t tiles_us;     /* the three tile-layer quads */
} game_frame_times_t;

static game_frame_times_t g_game_frame_times;

#endif /* FRAME_TIMES_H */
