/*
 * input_log.h — --input-log FILE: when each host key arrives, and the frame
 * the board sees it on (issue #617).
 *
 * Virtual-On's guard and jump need both levers on the same board frame
 * (profiles/vonj.h), and a player on the Windows desktop lost them where the
 * browser kept them, with the same keys. The log says which side splits a
 * pair: the host (keys far apart, or one missing or held over) or the board
 * (both keys in one frame's gap, but the board sees them a frame apart).
 *
 * Three kinds of line, all stamped in milliseconds since the log opened:
 *   key  the UI thread's key event, as sokol gave it (code, name, down or up,
 *        a key repeat), the actions it presses and the host's held mask after
 *        it (g_input.held), with the board's frame count at that moment;
 *   pad  the held mask after the gamepads changed it;
 *   frame  the mask the board reads this slice (a netplay or replay word when
 *        one has the board), written at the slice's start when it changed.
 *
 * Host-side only: nothing here is read by the board, so it changes no frame.
 * Desktop-only (M2HLE_DEV_TOOLS); input_log_on() is the "is it armed" test.
 */
#ifndef INPUT_LOG_H
#define INPUT_LOG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "build_features.h"
#include "emu_times.h"   /* emu_now_us */

static FILE    *g_input_log;
static int64_t  g_input_log_t0;
static uint32_t g_input_log_board;    /* the last board mask written */
static bool     g_input_log_board_any;

static inline bool input_log_on(void) { return M2HLE_DEV_TOOLS && g_input_log != NULL; }

static inline double input_log_ms(void) { return (double)(emu_now_us() - g_input_log_t0) / 1000.0; }

static inline bool input_log_open(const char *path) {
    g_input_log = fopen(path, "w");
    if (!g_input_log) return false;
    g_input_log_t0 = emu_now_us();
    fprintf(g_input_log, "# m2hle --input-log: ms, then key/pad (UI thread) or frame (the board)\n");
    fflush(g_input_log);
    return true;
}

static inline void input_log_close(void) {
    if (g_input_log) fclose(g_input_log);
    g_input_log = NULL;
}

/* The UI thread: a key event and what it left held. */
static inline void input_log_key(int kc, const char *name, bool down, bool repeat,
                                 uint32_t acts, uint32_t held, uint32_t frame) {
    if (!input_log_on()) return;
    fprintf(g_input_log, "%11.3f key   %-4s %3d %-9s%s acts=%08X held=%08X  (board frame %u)\n",
            input_log_ms(), down ? "down" : "up", kc, name && name[0] ? name : "?",
            repeat ? " repeat" : "", acts, held, frame);
    fflush(g_input_log);
}

/* The UI thread: the gamepads changed the held mask. */
static inline void input_log_pad(uint32_t held, uint32_t frame) {
    if (!input_log_on()) return;
    fprintf(g_input_log, "%11.3f pad   held=%08X  (board frame %u)\n", input_log_ms(), held, frame);
    fflush(g_input_log);
}

/* The emu thread, at a slice's start: the mask the board reads now. */
static inline void input_log_frame(uint32_t frame, uint32_t board) {
    if (!input_log_on() || (g_input_log_board_any && board == g_input_log_board)) return;
    g_input_log_board     = board;
    g_input_log_board_any = true;
    fprintf(g_input_log, "%11.3f frame %u board=%08X\n", input_log_ms(), frame, board);
    fflush(g_input_log);
}

#endif /* INPUT_LOG_H */
