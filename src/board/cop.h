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

/* Fn_zanzou_reserve streams up to 4 + 16*4 + 2 words; every other command is
 * under 20.  tests/cop_replay replays a whole captured argument list at once,
 * so this has to hold the longest of them. */
#define COP_ARGS_MAX  80

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

/* GEO clip-window capture (set_window @ i960 0x5564).
 *
 * set_window is NOT a COP FIFO command — it writes to GEO MMIO: 0x303 to GEO+0x30
 * (→ geo_win_start) then 6 packed vertex words to the GEO_PROGRAM FIFO
 * (→ geo_win_push). Both writes are intercepted by the callbacks in memory.h.
 *
 * Rather than splice a synthetic sentinel into the COP command stream, we record
 * each completed set_window as a DISCRETE EVENT tagged with the stream position
 * (geo_capture_head) at which it took effect, plus the two corner words. The geo3d
 * scanner replays these in stream order alongside the draws (decoding + full-screen
 * detection live in the geo layer), so the COP command ring stays pure. */
typedef struct {
    bool     active;                    /* a set_window is mid-collection */
    int      count;
    uint32_t words[GEO_WIN_FIFO_MAX];
} geo_win_collect_t;

static geo_win_collect_t g_geo_win = {0};

typedef struct {
    int      head_pos;   /* geo_capture_head when the window took effect (monotonic) */
    uint32_t w0, w1;     /* the two corner vertex words (packed screen coords) */
} geo_win_event_t;

#define GEO_WIN_EVENTS_MAX 128   /* power of 2; ring of recent set_window events */
static geo_win_event_t g_win_events[GEO_WIN_EVENTS_MAX] = {0};
static int g_win_event_head = 0; /* monotonic write index into the event ring */

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
    /* Record a discrete window event at the current stream position. */
    geo_win_event_t *e = &g_win_events[g_win_event_head & (GEO_WIN_EVENTS_MAX - 1)];
    e->head_pos = g_cop.geo_capture_head;
    e->w0       = g_geo_win.words[0];
    e->w1       = g_geo_win.words[1];
    g_win_event_head++;
}

/* sharc_exec.h defines sharc_args_for_cmd / sharc_exec; it transitively
 * includes sharc.h which defines g_sharc and the reply FIFO. */
#include "sharc_exec.h"

/* ---- Public interface ---------------------------------------------------- */

/* A capture of the conversation as the firmware's side of the FIFOs sees it,
 * in the tags of a MAME SHARC-side capture (tools/mame/cop-capture.lua), so
 * tests/cop_replay reads either: 0x21000000 a command word, 0x20000000 an
 * argument, 0x30000000 a word the command answered. NULL when not capturing. */
static void (*g_cop_tap)(uint32_t tag, uint32_t val) = NULL;

static inline void cop_tap_replies(void) {
    if (!g_cop_tap) return;
    for (int k = 0; k < g_sharc.reply_count; k++) g_cop_tap(0x30000000u, g_sharc.reply[k]);
}

/* Called for every 32-bit write to the COPROGRAM region. */
static inline void cop_write(uint32_t val) {
    g_cop.writes++;

    g_cop.geo_capture[g_cop.geo_capture_head & (GEO_CAPTURE_SIZE - 1)] = val;
    g_cop.geo_capture_head++;
    if (g_cop.geo_capture_count < GEO_CAPTURE_SIZE)
        g_cop.geo_capture_count++;

    /* An argument of a fixed-length command: most words are one. */
    if (g_cop.args_needed > 0) {
        if (g_cop_tap) g_cop_tap(0x20000000u, val);
        if (g_cop.args_received < COP_ARGS_MAX)
            g_cop.args[g_cop.args_received++] = val;
        if (--g_cop.args_needed == 0) {
            sharc_exec(g_cop.cur_cmd, g_cop.args, g_cop.args_received);
            cop_tap_replies();
            g_cop.cur_cmd       = 0;
            g_cop.args_received = 0;
        }
        return;
    }

    /* A variable-length command: the handler consumes words until it says it
     * is done, pushing its answers as it goes so the i960's loop finds each
     * one waiting where the board would have left it. */
    if (g_cop.args_needed == COP_ARGS_STREAM) {
        if (g_cop_tap) g_cop_tap(0x20000000u, val);
        int before = g_sharc.reply_count;
        bool done  = sharc_zanzou_feed(val);
        if (g_cop_tap)
            for (int k = before; k < g_sharc.reply_count; k++) g_cop_tap(0x30000000u, g_sharc.reply[k]);
        if (done) { g_cop.args_needed = 0; g_cop.cur_cmd = 0; }
        return;
    }

    if (g_cop_tap) g_cop_tap(0x21000000u, val);
    g_cop.cur_cmd       = val;
    g_cop.args_needed   = sharc_args_for_cmd(val);
    g_cop.args_received = 0;
    if (g_cop.args_needed == COP_ARGS_STREAM) {
        g_sharc.reply_count = 0;
        g_sharc.reply_idx   = 0;
        sharc_zanzou_begin();
        return;
    }
    if (g_cop.args_needed == 0) {
        sharc_exec(val, NULL, 0);
        cop_tap_replies();
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
    g_zz.phase = 4;                       /* no stream in flight */
    memset(&g_sharc, 0, sizeof(g_sharc));
    memset(&g_geo_win, 0, sizeof(g_geo_win));
    sharc_rot_identity();
    /* firmware init (cpres1 PM 0x20080..): DM[0x30300..2] = 0, 1.0, 2.0 */
    g_sharc.dm[0x301] = 0x3F800000u;
    g_sharc.dm[0x302] = 0x40000000u;
}

#endif /* COP_H */
