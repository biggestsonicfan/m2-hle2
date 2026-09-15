/*
 * hle_hooks.h — pre-instruction HLE hook dispatch.
 *
 * Each game profile carries a hooks[] table populated at compile time in the
 * profile header.  Before the CPU decodes an instruction at IP, hle_check()
 * walks the active profile's table.  If a match is found the hook is called
 * and 0 is returned so the step loop skips normal decode (the hook must
 * advance IP or simulate a return).  Returns 1 when no hook fires.
 *
 * hle_ret() — helper for hooks that bypass complete functions.  Mirrors the
 * i960 `ret` instruction: restores the previous register window and jumps to
 * the saved return address.  Call this inside any hook that short-circuits an
 * entire function (e.g. CoProcessorErr).
 */
#ifndef HLE_HOOKS_H
#define HLE_HOOKS_H

#include "i960.h"
#include "memory.h"
#include "log.h"
#include "game_profile.h"

/* Frame-pacing flag — set to 1 by the per-game frame-boundary hook.
 * Polled by emu_thread_run_loop after each step batch; when set the slice is
 * cut short and the thread sleeps until the next 16.67 ms tick. */
static volatile int g_frame_done = 0;

/* Monotonic count of completed game frames. The emu thread bumps it at every
 * frame boundary (HLE pace hook or board vblank ACK). Tooling outside the
 * emulator needs a frame clock to pace a capture by — MAME's drivers use the
 * screen's frame notifier for exactly this — and steps/s is not one. */
static volatile unsigned g_emu_frames = 0;

/* Simulate an i960 `ret`: restore the previous register window and set IP to
 * the saved return address.  Use this in hooks that bypass whole functions. */
static inline void hle_ret(i960_cpu_t *cpu) {
    if (cpu->frame_depth > 0) {
        uint32_t ret_ip = cpu->locals.rip;
        cpu->frame_depth--;
        cpu->locals = cpu->frame_stack[cpu->frame_depth];
        if (cpu->frame_irq[cpu->frame_depth]) {   /* interrupt return, as `ret` */
            cpu->sfr.ac = cpu->frame_irq_ac[cpu->frame_depth];
            cpu->sfr.pc = cpu->frame_irq_pc[cpu->frame_depth];
            cpu->frame_irq[cpu->frame_depth] = 0;
        }
        cpu->sfr.ip = ret_ip;
        cpu->globals.fp = cpu->locals.pfp;
    } else {
        LOG_WARN("hle_ret: empty frame stack at IP=0x%08X", cpu->sfr.ip);
        cpu->halted = 1;
    }
}

/* Inject a call to target: push current frame with rip = ret_ip, redirect IP.
 * When target executes `ret`, it returns to ret_ip and the saved frame is restored. */
static inline void hle_call(i960_cpu_t *cpu, uint32_t target, uint32_t ret_ip) {
    if (cpu->frame_depth < FRAME_STACK_DEPTH) {
        cpu->frame_stack[cpu->frame_depth] = cpu->locals;
        cpu->frame_irq[cpu->frame_depth] = 0;
        cpu->locals.rip = ret_ip;
        cpu->frame_depth++;
    } else {
        LOG_WARN("hle_call: frame stack full at IP=0x%08X", cpu->sfr.ip);
    }
    cpu->sfr.ip = target;
}

/*
 * Deliver an interrupt: vector to handler between two instructions and resume
 * at the current IP when it returns.
 *
 * Not a call. The processor stores PC and AC in the interrupt frame and `ret`
 * puts both back, so the interrupted code finds its condition code exactly as
 * it left it. A call does not, and an interrupt landing between a compare and
 * its branch then branches on the handler's last compare instead.
 *
 * That is how STF crashed in attract: a timer interrupt arrived between
 * `cmpo r14, 0x10` and `bg` in unpack_lod_data's bit-buffer refill (0x4BAAC /
 * 0x4BAB4), the refill branch went the wrong way, the Huffman decode lost sync, and its
 * output ran off the end of the halfword buffer into the code tree.
 */
static inline void hle_interrupt(i960_cpu_t *cpu, uint32_t handler) {
    int d = cpu->frame_depth;
    hle_call(cpu, handler, cpu->sfr.ip);
    if (cpu->frame_depth == d + 1) {
        cpu->frame_irq[d]    = 1;
        cpu->frame_irq_ac[d] = cpu->sfr.ac;
        cpu->frame_irq_pc[d] = cpu->sfr.pc;
    }
}

/* Dispatch: walk the active profile's hook table and call the first match. */
static inline int hle_check(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!g_active_profile || g_active_profile->hook_count == 0)
        return 1;
    uint32_t ip = cpu->sfr.ip;
    const hle_hook_entry_t *h = g_active_profile->hooks;
    size_t n = g_active_profile->hook_count;
    for (size_t i = 0; i < n; i++) {
        if (h[i].addr == ip)
            return h[i].fn(cpu, bus);
    }
    return 1;
}

#endif /* HLE_HOOKS_H */
