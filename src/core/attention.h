/*
 * attention.h — one word the emu thread's instruction loop watches instead of
 * every rare flag that can stop or divert it.
 *
 * emu_slice_body used to test half a dozen flags after every i960 instruction:
 * the frame hook's g_frame_done, the vsync ACK, the sound UART kick, and the
 * debugger's break-on-warn, watchpoint and break-on-unknown-COP flags. Most are
 * volatile (other threads set them), so each was a load and a branch per
 * instruction that the compiler could not hoist -- on the handheld's in-order
 * A55 the loop's own bookkeeping measured ~12% of RetroArch's main thread.
 *
 * Every place that SETS one of those flags (or arms a breakpoint) also bumps
 * g_emu_attn. The loop's fast path reads only this word; when it moves, that
 * instruction's original checks run in their original order and the slice
 * carries on in the original loop, so nothing is seen an instruction late.
 * A setter that forgets the bump is the one way to break that, which is why
 * they are listed here:
 *   hle_hooks: the profiles' frame hooks (g_frame_done)
 *   irq_timer.h: irqt writes (g_vblank_acked, g_irqt_sound_kick)
 *   log.h: g_log.warn_triggered     watchpoint.h: g_wp.hit
 *   sharc_exec.h: g_sharc.unknown_triggered
 *   breakpoint.h: g_bp.bloom (a breakpoint armed from another thread)
 */
#ifndef ATTENTION_H
#define ATTENTION_H

#include <stdint.h>

static volatile uint32_t g_emu_attn = 0;

static inline void emu_attn_bump(void) { g_emu_attn = g_emu_attn + 1u; }

#endif /* ATTENTION_H */
