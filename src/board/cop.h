/*
 * cop.h — i960↔SHARC FIFO interface (board-level).
 *
 * The Model 2 COP is the ADSP-21060 SHARC geometry coprocessor.  The i960
 * communicates with it by writing a command word followed by float/int
 * arguments to the COPROGRAM region (0x00880000); results are read back from
 * the same region.
 *
 * This file owns only the MMIO half of the COP subsystem:
 *   - arg accumulator (cur_cmd, args[], args_needed)
 *   - geo_capture ring (raw COPROGRAM write stream for the polygon decoder)
 *   - cop_write / cop_read / cop_reset
 *
 * All SHARC computation state lives in g_sharc (sharc.h).
 * All command handlers live in sharc_exec.h.
 *
 * architecture note: memory.h installs MMIO callbacks on the COPROGRAM region
 * that forward all 32-bit writes to cop_write() and reads to cop_read().
 */
#ifndef COP_H
#define COP_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "constants.h"

/* ---- Limits -------------------------------------------------------------- */

#define COP_ARGS_MAX  20

/* Synthetic sentinel emitted into geo_capture when a set_window call is
 * intercepted by the GEO_PROGRAM write callback in memory.h.
 * Followed by 6 packed window words so the geo3d scanner can extract the
 * clip-window center for the next draw call. */
#define GEO_WIN_SENTINEL 0xFEEDFACEu
#define GEO_WIN_FIFO_MAX 6

/* ---- State --------------------------------------------------------------- */

typedef struct {
    /* Command accumulator — collects args before dispatching to sharc_exec. */
    uint32_t cur_cmd;
    int      args_needed;
    uint32_t args[COP_ARGS_MAX];
    int      args_received;

    /* Raw command-stream ring buffer for the polygon decoder (geo3d.h).
     * Size must be a power of 2 and ≥ GEO_CAPTURE_SIZE. */
    uint32_t geo_capture[GEO_CAPTURE_SIZE];
    int      geo_capture_head;
    int      geo_capture_count;

    /* Per-frame ring range — updated by the frame-pace hook so the scanner
     * reads only one game frame's worth of entries instead of the full ring.
     * geo_frame_start = head at start of last complete frame.
     * geo_frame_end   = head at end   of last complete frame. */
    volatile int geo_frame_start;
    volatile int geo_frame_end;

    /* MMIO activity counters. */
    uint32_t writes;
    uint32_t reads;
} cop_state_t;

static cop_state_t g_cop = {0};

/* GEO FIFO window capture state — tracks in-progress set_window calls.
 * Filled by the GEO_PROGRAM write callback in memory.h; emits a
 * GEO_WIN_SENTINEL + 6 words into geo_capture once all 6 words arrive. */
typedef struct {
    bool     active;
    int      count;
    uint32_t words[GEO_WIN_FIFO_MAX];
} geo_win_state_t;

static geo_win_state_t g_geo_win = {0};

static inline void geo_win_start(void) {
    g_geo_win.active = true;
    g_geo_win.count  = 0;
}

static inline void geo_win_push(uint32_t val) {
    if (!g_geo_win.active) return;
    if (g_geo_win.count < GEO_WIN_FIFO_MAX)
        g_geo_win.words[g_geo_win.count++] = val;
    if (g_geo_win.count < GEO_WIN_FIFO_MAX) return;

    g_geo_win.active = false;
    /* Emit GEO_WIN_SENTINEL + 6 words into the geo_capture ring so the
     * geo3d scanner can associate the clip window with the next draw. */
    uint32_t *gc   = g_cop.geo_capture;
    int      *head = &g_cop.geo_capture_head;
    int      *cnt  = &g_cop.geo_capture_count;
    gc[(*head)++ & (GEO_CAPTURE_SIZE-1)] = GEO_WIN_SENTINEL;
    if (*cnt < GEO_CAPTURE_SIZE) (*cnt)++;
    for (int _i = 0; _i < GEO_WIN_FIFO_MAX; _i++) {
        gc[(*head)++ & (GEO_CAPTURE_SIZE-1)] = g_geo_win.words[_i];
        if (*cnt < GEO_CAPTURE_SIZE) (*cnt)++;
    }
}

/* sharc_exec.h defines sharc_args_for_cmd / sharc_exec; it transitively
 * includes sharc.h which defines g_sharc and the reply FIFO. */
#include "sharc_exec.h"

/* ---- Public interface ---------------------------------------------------- */

/* Called for every 32-bit write to the COPROGRAM region. */
static inline void cop_write(uint32_t val) {
    g_cop.writes++;

    g_cop.geo_capture[g_cop.geo_capture_head & (GEO_CAPTURE_SIZE - 1)] = val;
    g_cop.geo_capture_head++;
    if (g_cop.geo_capture_count < GEO_CAPTURE_SIZE)
        g_cop.geo_capture_count++;

    if (g_cop.args_needed > 0) {
        if (g_cop.args_received < COP_ARGS_MAX)
            g_cop.args[g_cop.args_received++] = val;
        if (--g_cop.args_needed == 0) {
            sharc_exec(g_cop.cur_cmd, g_cop.args, g_cop.args_received);
            g_cop.cur_cmd       = 0;
            g_cop.args_received = 0;
        }
        return;
    }

    g_cop.cur_cmd       = val;
    g_cop.args_needed   = sharc_args_for_cmd(val);
    g_cop.args_received = 0;
    if (g_cop.args_needed == 0) {
        sharc_exec(val, NULL, 0);
        g_cop.cur_cmd = 0;
    }
}

/* Called for every read from the COPROGRAM region. */
static inline uint32_t cop_read(void) {
    g_cop.reads++;
    if (g_sharc.reply_idx < g_sharc.reply_count) {
        uint32_t v = g_sharc.reply[g_sharc.reply_idx++];
        if (g_sharc.reply_idx >= g_sharc.reply_count) {
            g_sharc.reply_idx   = 0;
            g_sharc.reply_count = 0;
        }
        return v;
    }
    return 0;
}

/* Reset all COP/SHARC state. Call when a new ROM is installed. */
static inline void cop_reset(void) {
    memset(&g_cop, 0, sizeof(g_cop));
    memset(&g_sharc, 0, sizeof(g_sharc));
    memset(&g_geo_win, 0, sizeof(g_geo_win));
    sharc_rot_identity();
    g_sharc.matrix_dirty = true;
}

#endif /* COP_H */
