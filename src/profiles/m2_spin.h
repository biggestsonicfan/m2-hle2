/*
 * m2_spin.h — a polling loop's idle iterations, skipped on the cycle clock
 * (Pinboard #294).
 *
 * Between frames a Model 2 game waits for the vblank interrupt in a two-
 * instruction loop over a work-RAM byte the handler moves:
 *
 *   loop: ldob   0x500000, r3      ; MEMB, absolute address
 *         cmpibe r3, g0, loop      ; COBR, back to the load
 *
 * Since the vblank came onto the i960's cycle clock (PR #163) that loop runs in
 * the interpreter until the interrupt: on the ARC-S two thirds of STF's i960
 * instructions, ~110,000 a frame.
 *
 * Nothing but an interrupt can end it, and nothing can be taken before the
 * timers' horizon (irq_timer.h) while none is pending. So a hook on the load
 * runs as many whole iterations as end before the horizon, at once: the value
 * read, the condition code, the instructions and their cycles from the ROM's
 * own words, charged as the i960's (g_hle_extra in hle_hooks.h), so the slice
 * ends on the same instruction and the timers see the same clock. The i960
 * runs the iteration the horizon falls in, and takes the interrupt there.
 *
 * The loop is decoded from memory at every call, so a hook on anything else
 * declines. It is left to the i960, which is always exact, when:
 *   - the loop would leave on this iteration (the byte already moved);
 *   - an interrupt is pending, or not one whole iteration ends before the
 *     horizon;
 *   - the slice has fewer instructions left than two (g_hle_room);
 *   - a data watchpoint is armed, or the debugger steps.
 *
 * `--spin-i960` turns it off, and det_digest --cpu holds the two against each
 * other frame by frame.
 */
#ifndef M2_SPIN_H
#define M2_SPIN_H

#include <stdbool.h>
#include <stdint.h>

#include "i960.h"
#include "memory.h"
#include "hle_hooks.h"
#include "i960_exec.h"
#include "irq_timer.h"
#include "attention.h"
#include "watchpoint.h"
#include "game_profile.h"
#include "log.h"

/* On unless the command line or a frontend option says otherwise. */
static int g_spin_skip = 1;

/* Iterations done here since start-up, for det_digest. */
static uint64_t g_spin_iters;

/* The loop at ip, decoded from memory: the load's words (ld, ea) and the
 * compare-and-branch's (cb). False when the IP holds anything else. */
static inline bool m2_spin_match(memory_bus_t *bus, uint32_t ip, uint32_t *ld, uint32_t *ea, uint32_t *cb) {
    uint32_t unused;
    mem_fetch2(bus, ip, ld, ea);
    mem_fetch2(bus, ip + 8u, cb, &unused);

    /* ldob / ldos / ld from a work-RAM address (MEMB, mode 0xC). */
    uint32_t lop = *ld >> 24;
    if ((lop != 0x80 && lop != 0x88 && lop != 0x90) || (*ld & 0x3C00u) != 0x3000u) return false;
    if (*ea < 0x00500000u || *ea > 0x005FFFFFu) return false;
    /* A compare-and-branch back to the load. */
    uint32_t cop = *cb >> 24;
    if (!((cop >= 0x31 && cop <= 0x36) || (cop >= 0x39 && cop <= 0x3E))) return false;
    int32_t disp = (int32_t)(*cb & 0x00001FFCu);
    if (disp & 0x1000) disp |= (int32_t)0xFFFFE000;
    return ip + 8u + (uint32_t)disp == ip;
}

/* Skip the idle iterations of the load + compare-and-branch loop at the IP;
 * 1 leaves it to the i960. */
static inline int m2_spin_skip(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!g_spin_skip || g_hle_room < 2 || wp_armed()) return 1;
    if (g_irqt.intreq & g_irqt.intena & 0x03FFu) return 1;

    uint32_t ip = cpu->sfr.ip, ld, ea, cb;
    if (!m2_spin_match(bus, ip, &ld, &ea, &cb)) return 1;
    uint32_t lop = ld >> 24, cop = cb >> 24;

    uint32_t v = lop == 0x80 ? mem_read8(bus, ea) : lop == 0x88 ? mem_read16(bus, ea) : mem_read32(bus, ea);
    int dst = (int)MEM_SRCDST(ld);
    uint32_t old = reg_read(cpu, dst);
    reg_write(cpu, dst, v);
    uint32_t s1 = COBR_M1(cb) ? COBR_SRC1(cb) : reg_read(cpu, (int)COBR_SRC1(cb));
    uint32_t s2 = reg_read(cpu, (int)COBR_SRC2(cb));
    uint32_t cc = cop < 0x38 ? i960_cmp_cc_o(s1, s2) : i960_cmp_cc_i((int32_t)s1, (int32_t)s2);
    if (!i960_cond(cc, cop & 7u)) { reg_write(cpu, dst, old); return 1; }   /* it leaves */

    int64_t c = (int64_t)i960_cycle_cost(ld) + i960_cycle_cost(cb);
    int64_t k = (g_irqt.horizon - g_irqt.pending - 1) / c;
    if (k > (int64_t)(g_hle_room / 2u)) k = g_hle_room / 2u;
    if (k < 1) { reg_write(cpu, dst, old); return 1; }

    set_cc(cpu, cc);
    bus->cpu_ip = ip + 8u;
    cpu->cycles += (uint64_t)(k * c);
    g_hle_extra = (uint32_t)(2 * k - 1);
    emu_attn_bump();
    g_spin_iters += (uint64_t)k;
    return 0;
}

/* An absolute ldob/ldos/ld from work RAM, then a compare-and-branch back to it. */
static inline bool m2_spin_is_loop(memory_bus_t *bus, uint32_t a) {
    uint32_t ld = mem_read32(bus, a), lop = ld >> 24;
    if ((lop != 0x80 && lop != 0x88 && lop != 0x90) || (ld & 0x3C00u) != 0x3000u) return false;
    uint32_t ea = mem_read32(bus, a + 4u), cb = mem_read32(bus, a + 8u), cop = cb >> 24;
    if (ea < 0x00500000u || ea > 0x005FFFFFu) return false;
    if (!((cop >= 0x31 && cop <= 0x36) || (cop >= 0x39 && cop <= 0x3E))) return false;
    return (cb & 0x00001FFCu) == 0x1FF8u;                               /* disp -8: back to the load */
}

/* Find the loops m2_spin_skip takes in a program and hook them for `prof`
 * (hle_hooks.h, g_hle_spin_sites): for a profile with no addresses of its own,
 * homebrew on a game's board. The scan matches the words m2_spin_skip checks,
 * which checks them again at every call, so a match in data costs nothing but
 * a filter bit. Returns how many. */
static inline size_t m2_spin_find(memory_bus_t *bus, uint32_t size, const game_profile_t *prof) {
    size_t n = 0;
    for (uint32_t a = 0; a + 12u <= size && n < HLE_SPIN_SITES_MAX; a += 4)
        if (m2_spin_is_loop(bus, a)) g_hle_spin_sites[n++] = a;
    g_hle_spin_count   = n;
    g_hle_spin_profile = n ? prof : NULL;
    g_hle_spin_hook    = m2_spin_skip;
    g_hle_filter_gen++;
    if (n) LOG_INFO("spin: %u idle loop%s in the program", (unsigned)n, n == 1 ? "" : "s");
    return n;
}

#endif /* M2_SPIN_H */
