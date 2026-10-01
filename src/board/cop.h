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
#include "emu_times.h"   /* host time spent in the handlers, for get_status */

/* ---- Limits -------------------------------------------------------------- */

/* Fn_zanzou_reserve streams up to 4 + 16*4 + 2 words; every other command is
 * under 20.  tests/cop_replay replays a whole captured argument list at once,
 * so this has to hold the longest of them. */
#define COP_ARGS_MAX  80


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

    /* COPRO_CTL1 (0x980000), for its upload bit; set by mem_init. While bit 31
     * is up, a FIFO word is a halfword of the SHARC's boot image, not a
     * command (MAME model2b_state::copro_fifo_w). */
    const uint8_t *ctl;
    uint32_t       upload_words;

    /* Reads of an empty reply FIFO. On the board the i960 would stall there
     * until the SHARC answered, so each one is a reply the HLE owes. */
    uint64_t       underflows;
    uint32_t       last_cmd;      /* the newest command word, for that warning */
} cop_state_t;

static cop_state_t g_cop = {0};

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

    /* The boot image the i960 uploads before it lowers the bit: the HLE
     * runs none of it, and none of it is a command. */
    if (g_cop.ctl && (g_cop.ctl[3] & 0x80)) {
        g_cop.upload_words++;
        return;
    }

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
            int64_t t0 = emu_times_cop_begin();
            int zone = hprof_enter(HPROF_COP);
            sharc_exec(g_cop.cur_cmd, g_cop.args, g_cop.args_received);
            hprof_leave(zone);
            emu_times_cop_end(t0);
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
        int zone   = hprof_enter(HPROF_COP);
        bool done  = sharc_zanzou_feed(val);
        hprof_leave(zone);
        if (g_cop_tap)
            for (int k = before; k < g_sharc.reply_count; k++) g_cop_tap(0x30000000u, g_sharc.reply[k]);
        if (done) { g_cop.args_needed = 0; g_cop.cur_cmd = 0; }
        return;
    }

    if (g_cop_tap) g_cop_tap(0x21000000u, val);
    g_cop.cur_cmd       = val;
    g_cop.last_cmd      = val;
    g_cop.args_needed   = sharc_args_for_cmd(val);
    g_cop.args_received = 0;
    if (g_cop.args_needed == COP_ARGS_STREAM) {
        g_sharc.reply_count = 0;
        g_sharc.reply_idx   = 0;
        sharc_zanzou_begin();
        return;
    }
    if (g_cop.args_needed == 0) {
        int64_t t0 = emu_times_cop_begin();
        int zone = hprof_enter(HPROF_COP);
        sharc_exec(val, NULL, 0);
        hprof_leave(zone);
        emu_times_cop_end(t0);
        cop_tap_replies();
        g_cop.cur_cmd = 0;
    }
}

/* A game frame ended: the words captured since the last edge are that frame's
 * draw commands, and the scanner reads exactly those (geo3d.h). The profile's
 * frame-pace hook calls this; so does the run loop for a board_vblank homebrew. */
static inline void cop_geo_frame_edge(void) {
    g_cop.geo_frame_start = g_cop.geo_frame_end;
    g_cop.geo_frame_end   = g_cop.geo_capture_head;
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
    /* A handler that answers fewer words than the firmware, or a command
     * missing from the arg table (0x17002E2E hid this way), shows up here. */
    uint64_t n = ++g_cop.underflows;
    if (n <= 8 || (n & (n - 1)) == 0)
        LOG_WARN("COP: read of an empty reply FIFO (last cmd 0x%08X, IP~0x%08X, %llu so far)",
                 g_cop.last_cmd, g_last_store_ip, (unsigned long long)n);
    return 0;
}

/* 0x980004 bit 0: the reply FIFO is empty (MAME model2_state::fifo_control_r).
 * Boot code waits on it after lowering the upload bit (STF cop_initialize_l1
 * 0xF3C, FV 0x190C). The HLE answers each command at once, so it is up
 * whenever no reply is waiting. */
static inline uint32_t cop_fifo_status(void) {
    return g_sharc.reply_idx < g_sharc.reply_count ? 0u : 1u;
}

/* Reset all COP/SHARC state. Call when a new ROM is installed. */
static inline void cop_reset(void) {
    const uint8_t *ctl = g_cop.ctl;       /* the bus's, not COP state */
    memset(&g_cop, 0, sizeof(g_cop));
    g_cop.ctl = ctl;
    g_zz.phase = 4;                       /* no stream in flight */
    memset(&g_sharc, 0, sizeof(g_sharc));
    sharc_rot_identity();
    /* firmware init (cpres1 PM 0x20080..): DM[0x30300..2] = 0, 1.0, 2.0 */
    g_sharc.dm[0x301] = 0x3F800000u;
    g_sharc.dm[0x302] = 0x40000000u;
}

#endif /* COP_H */
