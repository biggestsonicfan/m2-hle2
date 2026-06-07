/*
 * trace_window.h — execution trace (Phase 3 STUB).
 *
 * i960_exec.h calls trace_record() once per instruction. A real ring-buffer
 * trace + ImGui window is deferred; for now this is a no-op so the core builds
 * and runs without trace overhead.
 */
#ifndef TRACE_WINDOW_H
#define TRACE_WINDOW_H

#include <stdint.h>

static inline void trace_record(uint32_t ip, uint32_t word, int frame_depth) {
    (void)ip; (void)word; (void)frame_depth;
}

#endif /* TRACE_WINDOW_H */
