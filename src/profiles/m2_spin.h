/*
 * m2_spin.h — a polling loop's idle iterations, skipped on the cycle clock
 * (Pinboard #294, every game since #1713).
 *
 * Between frames a Model 2 game waits in a short loop that reads one word the
 * board moves: a work-RAM byte its vblank handler sets (STF, Fighting Vipers,
 * VF2, Daytona, DOA), or the frame-number bit of the video control word at
 * 0x98000C (House of the Dead, Dynamite Cop, Motor Raid, Last Bronx, Virtua
 * Cop, Virtual-On). STF's is two instructions:
 *
 *   loop: ldob   0x500000, r3      ; MEMB, absolute address
 *         cmpibe r3, g0, loop      ; COBR, back to the load
 *
 * and the 0x98000C one counts its iterations as it goes:
 *
 *   loop: ld     0x98000C, g4
 *         addo   g0, 1, g0
 *         and    g4, 4, g4
 *         cmpibe g5, g4, loop
 *
 * Since the vblank came onto the i960's cycle clock (PR #163) such a loop runs
 * in the interpreter until the board moves the word: 42-88% of each game's
 * i960 instructions (PERF-PROFILE.md, #1713).
 *
 * Nothing moves either word before the timers' horizon (irq_timer.h): work
 * RAM only the i960 writes, its handlers included, and none is taken while
 * none is pending; the frame bit moves at the vblank, which the horizon never
 * passes. So a hook at the loop's head runs as many whole iterations as end
 * before the horizon, at once, and the board cannot tell: the registers and
 * condition code they leave, the instructions and their cycles from the ROM's
 * own words, charged as the i960's (g_hle_extra in hle_hooks.h), so the slice
 * ends on the same instruction and the timers see the same clock. The i960
 * runs the iteration the horizon falls in, and takes the interrupt there.
 *
 * The hook decodes the loop from memory at every call and runs it on a copy of
 * the registers three times (m2_spin_iter), with only the instructions such a
 * loop is made of: loads from work RAM or 0x98000C, register logic and
 * arithmetic, compares, branches. After the first iteration each register must
 * be either fixed or a counter, one that goes up by the same amount every
 * iteration and feeds nothing but itself or another counter: never a compare,
 * a branch, a mask or a load address. So iteration k leaves the fixed
 * registers as the first did and each counter at first + (k - 1) x step. A
 * loop the i960 would leave on this iteration, or anything else, declines.
 *
 * It is left to the i960, which is always exact, when:
 *   - the loop would leave within three iterations (the word already moved);
 *   - an interrupt is pending, or not one whole iteration ends before the
 *     horizon;
 *   - the slice has too few instructions left for one (g_hle_room);
 *   - a data watchpoint is armed, or the debugger steps.
 *
 * A game charged the board's timing (i960_timing.h: Daytona, Sega Rally,
 * Virtua Cop) is skipped too. Its model of the caches and the load scoreboard
 * runs along on a copy; from the second iteration on it must come back to the
 * same state with the same cost, so k iterations add the first one's cost and
 * k - 1 of the second's to the cycle's remainder, and leave the model as the
 * second did.
 *
 * Where to hook is found, not listed: m2_spin_install scans the program ROM,
 * then work RAM, for a backward branch whose loop m2_spin_is_loop accepts, and
 * puts each head in g_hle_spin_sites. Sega Rally unpacks its code into work
 * RAM and Pilot Kids copies its own, so the run loop scans work RAM again every
 * 64 frames (g_hle_spin_rescan). A site is only a place to try: the hook
 * proves the loop again at every call, so the scans read the board but never
 * change it.
 *
 * `--spin-i960`
 turns it off, and det_digest --cpu holds the two against each
 * other frame by frame.
 */
#ifndef M2_SPIN_H
#define M2_SPIN_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "i960.h"
#include "memory.h"
#include "hle_hooks.h"
#include "i960_exec.h"
#include "i960_timing.h"
#include "irq_timer.h"
#include "attention.h"
#include "watchpoint.h"
#include "game_profile.h"
#include "rom_loader.h"
#include "log.h"

/* On unless the command line or a frontend option says otherwise. */
static int g_spin_skip = 1;

/* Iterations done here since start-up, for det_digest. */
static uint64_t g_spin_iters;

/* The longest loop body taken, in instructions. */
#define M2_SPIN_BODY_MAX 8

/* What a poll may read: work RAM, which only the i960 writes, or the video
 * control word, whose frame bit moves only at the vblank (irqt_videoctl). */
static inline bool m2_spin_addr_ok(uint32_t a) {
    return (a >= 0x00500000u && a <= 0x005FFFFFu) || (a & ~3u) == 0x0098000Cu;
}

/* The registers as reg_read numbers them (r0-r15, g0-g15) and the condition
 * code: the loop's state. */
typedef struct {
    uint32_t r[32];
    uint32_t cc;
} m2_spin_regs_t;

/* A REG-format operation the loop may use: 1 logic, shift or compare, which no
 * counter may feed; 2 add or subtract, which a counter may; 3 mov. 0 for
 * anything else. */
static inline int m2_spin_reg_kind(uint32_t op) {
    switch (op) {
    case 0x581: case 0x582: case 0x584: case 0x586: case 0x587:   /* and andnot notand xor or */
    case 0x598: case 0x59c:                                       /* shro shlo */
    case 0x5a0: case 0x5a1:                                       /* cmpo cmpi */
        return 1;
    case 0x590: case 0x591: case 0x592: case 0x593:               /* addo addi subo subi */
        return 2;
    case 0x5cc:                                                   /* mov */
        return 3;
    default:
        return 0;
    }
}

/* One iteration of the loop at `head` on `s`, as i960_step_core would run it:
 * true if it ends on a branch back to head, with its instruction count, its
 * cycles and the address of that branch. False if it leaves, runs longer than
 * M2_SPIN_BODY_MAX, or meets an instruction this does not model. `taint` (or
 * NULL) holds the counters, bit per register: a counter may only be added to
 * or moved, and an iteration that uses one any other way returns false.
 * Under the board's timing `bt` (or NULL) is a copy of its model
 * (i960_timing.h), moved on as i960_bt_charge would, and the cycles are in
 * 1/1024 (I960_CYC_ONE), without the remainder the i960 carries: an
 * instruction the model would charge less than nothing returns false, so the
 * sum is what the i960 adds to its remainder. */
static inline bool m2_spin_iter(memory_bus_t *bus, m2_spin_regs_t *s, uint32_t head, uint32_t *taint,
                                i960_bt_t *bt, unsigned *n_out, uint32_t *cyc_out, uint32_t *last_out) {
    uint32_t pc = head, cyc = 0, t = taint ? *taint : 0;
    for (unsigned n = 1; n <= M2_SPIN_BODY_MAX; n++) {
        uint32_t w1, w2, ea = 0;
        mem_fetch2(bus, pc, &w1, &w2);
        uint32_t cls = w1 >> 28, next = pc + 4u, cost = i960_cycle_cost(w1);
        int32_t disp = 0;
        bool taken = false;

        if (cls == 0x8 || cls == 0x9 || cls == 0xC) {                    /* loads */
            uint32_t op = w1 >> 24, mode = MEM_MODE(w1), ab = MEM_ABASE(w1);
            if (op != 0x80 && op != 0x88 && op != 0x90 && op != 0xC0 && op != 0xC8) return false;
            bool uses_ab = true;
            if (!(mode & 0x4)) { ea = (mode & 0x8) ? s->r[ab] + (w1 & 0xFFFu) : (w1 & 0xFFFu); uses_ab = (mode & 0x8) != 0; }
            else if (mode == 0x4) ea = s->r[ab];
            else if (mode == 0xC) { ea = w2; next = pc + 8u; uses_ab = false; }
            else if (mode == 0xD) { ea = w2 + s->r[ab]; next = pc + 8u; }
            else return false;
            if ((uses_ab && (t >> ab & 1u)) || !m2_spin_addr_ok(ea)) return false;
            uint32_t v = op == 0x80 ? mem_read8(bus, ea)
                       : op == 0x88 ? mem_read16(bus, ea)
                       : op == 0x90 ? mem_read32(bus, ea)
                       : op == 0xC0 ? (uint32_t)(int32_t)(int8_t)mem_read8(bus, ea)
                       :              (uint32_t)(int32_t)(int16_t)mem_read16(bus, ea);
            uint32_t d = MEM_SRCDST(w1);
            s->r[d] = v;
            t &= ~(1u << d);
        } else if (cls >= 0x5 && cls <= 0x7) {                           /* register ops */
            uint32_t op = REG_OPCODE(w1), i1 = REG_SRC1(w1), i2 = REG_SRC2(w1), d = REG_DST(w1);
            int kind = m2_spin_reg_kind(op);
            if (!kind || (REG_M3(w1) && op != 0x5a0 && op != 0x5a1)) return false;   /* m3: dst an SFR */
            uint32_t a = REG_M1(w1) ? i1 : s->r[i1], b = REG_M2(w1) ? i2 : s->r[i2];
            uint32_t ta = REG_M1(w1) ? 0u : (t >> i1 & 1u), tb = REG_M2(w1) ? 0u : (t >> i2 & 1u);
            if (kind == 3) tb = 0;
            if (kind == 1 ? (ta | tb) : (ta & tb)) return false;
            if (op == 0x5a0 || op == 0x5a1) {
                s->cc = op == 0x5a0 ? i960_cmp_cc_o(a, b) : i960_cmp_cc_i((int32_t)a, (int32_t)b);
            } else {
                uint32_t r;
                switch (op) {
                case 0x581: r = a & b; break;
                case 0x582: r = ~a & b; break;
                case 0x584: r = a & ~b; break;
                case 0x586: r = a ^ b; break;
                case 0x587: r = a | b; break;
                case 0x598: r = a < 32 ? b >> a : 0; break;
                case 0x59c: r = a < 32 ? b << a : 0; break;
                case 0x590: case 0x591: r = b + a; break;
                case 0x592: case 0x593: r = b - a; break;
                default:    r = a; break;                                /* mov */
                }
                s->r[d] = r;
                t = (t & ~(1u << d)) | ((ta | tb) << d);
            }
        } else if (cls <= 0x1) {                                         /* b, bno .. bo */
            uint32_t op = CTRL_OPCODE(w1);
            if (op != 0x08 && (op < 0x10 || op > 0x17)) return false;
            disp = (int32_t)(w1 & 0x00FFFFFCu);
            if (disp & 0x00800000) disp |= (int32_t)0xFF000000;
            taken = op == 0x08 || i960_cond(s->cc, op & 7u);
        } else if (cls == 0x3) {                                         /* compare and branch */
            uint32_t op = COBR_OPCODE(w1), i1 = COBR_SRC1(w1), i2 = COBR_SRC2(w1);
            if (!((op >= 0x31 && op <= 0x36) || (op >= 0x39 && op <= 0x3E))) return false;
            if ((!COBR_M1(w1) && (t >> i1 & 1u)) || (t >> i2 & 1u)) return false;
            uint32_t a = COBR_M1(w1) ? i1 : s->r[i1], b = s->r[i2];
            s->cc = op < 0x38 ? i960_cmp_cc_o(a, b) : i960_cmp_cc_i((int32_t)a, (int32_t)b);
            disp = (int32_t)(w1 & 0x00001FFCu);
            if (disp & 0x1000) disp |= (int32_t)0xFFFFE000;
            taken = i960_cond(s->cc, op & 7u);
        } else {
            return false;
        }
        if (bt) {
            int64_t a = (int64_t)cost * I960_CYC_ONE + i960_bt_step(bt, pc, w1, ea, cost);
            if (a < 0) return false;
            cyc += (uint32_t)a;
        } else {
            cyc += cost;
        }
        if (taken) {
            if (pc + (uint32_t)disp != head) return false;               /* it leaves */
            if (taint) *taint = t;
            *n_out = n; *cyc_out = cyc; *last_out = pc;
            return true;
        }
        pc = next;
    }
    return false;
}

/* Skip the idle iterations of the poll loop whose head is at the IP; 1 leaves
 * it to the i960. */
static inline int m2_spin_skip(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!g_spin_skip || g_hle_room < 2 || wp_armed()) return 1;
    if (g_irqt.intreq & g_irqt.intena & 0x03FFu) return 1;

    uint32_t head = cpu->sfr.ip;
    m2_spin_regs_t s1, s2, s3;
    for (int i = 0; i < 32; i++) s1.r[i] = reg_read(cpu, i);
    s1.cc = get_cc(cpu);
    /* Under the board's timing its model runs along (b), and what each
     * iteration leaves it (b1, b2) is what the i960 would. */
    i960_bt_t b = g_i960_bt, b1, b2, *bt = g_i960_bt_on ? &b : NULL;
    unsigned n1, n2, n3;
    uint32_t c1, c2, c3, last, taint = 0;
    if (!m2_spin_iter(bus, &s1, head, NULL, bt, &n1, &c1, &last)) return 1;
    b1 = b;
    s2 = s1;
    if (!m2_spin_iter(bus, &s2, head, NULL, bt, &n2, &c2, &last)) return 1;
    b2 = b;
    /* What the second iteration moved is a counter, or the loop is not one. */
    for (int i = 0; i < 32; i++) if (s2.r[i] != s1.r[i]) taint |= 1u << i;
    if (s2.cc != s1.cc) return 1;
    s3 = s2;
    if (!m2_spin_iter(bus, &s3, head, &taint, bt, &n3, &c3, &last)) return 1;
    if (n2 != n1 || n3 != n1 || c3 != c2 || c2 == 0 || s3.cc != s1.cc) return 1;
    /* From the second on, every iteration costs the same and leaves the
     * timing model as it found it (the first may miss the cache). */
    if (bt ? memcmp(&b, &b2, sizeof b) != 0 : c2 != c1) return 1;
    for (int i = 0; i < 32; i++) {
        bool counter = (taint >> i & 1u) != 0;
        if (s3.r[i] - s2.r[i] != s2.r[i] - s1.r[i] || counter != (s2.r[i] != s1.r[i])) return 1;
    }

    /* k iterations add base + c1 + (k - 1) x c2 to the clock, in 1/1024 with
     * the remainder under the board's timing, and must end before the horizon. */
    irqt_sync();   /* pending lags the run loop's boundary (irq_timer.h) */
    int64_t unit = bt ? I960_CYC_ONE : 1, base = bt ? g_i960_cyc_frac : 0;
    int64_t lim = (g_irqt.horizon - g_irqt.pending) * unit - 1 - base - (int64_t)c1;
    if (lim < 0) return 1;
    int64_t k = 1 + lim / (int64_t)c2;
    if (k > (int64_t)(g_hle_room / n1)) k = g_hle_room / n1;
    if (k < 1) return 1;

    for (int i = 0; i < 32; i++)
        reg_write(cpu, i, s1.r[i] + (uint32_t)(k - 1) * (s2.r[i] - s1.r[i]));
    set_cc(cpu, s1.cc);
    bus->cpu_ip = last;
    int64_t total = base + (int64_t)c1 + (k - 1) * (int64_t)c2;
    if (bt) {
        cpu->cycles += (uint64_t)total >> 10;
        g_i960_cyc_frac = (uint32_t)(total & (I960_CYC_ONE - 1));
        g_i960_bt = k == 1 ? b1 : b2;
    } else {
        cpu->cycles += (uint64_t)total;
    }
    g_hle_extra = (uint32_t)(k * n1 - 1);
    emu_attn_bump();
    g_spin_iters += (uint64_t)k;
    return 0;
}

/* The words alone say the loop ending in the backward branch at `end` may be
 * one m2_spin_iter takes: a body of at most M2_SPIN_BODY_MAX instructions it
 * models, with a load from a fixed address (or from a register the body does
 * not write), and no compare of a register the body counts in. The hook checks
 * it again at every call, so this only keeps the filter to likely loops. */
static inline bool m2_spin_is_loop(memory_bus_t *bus, uint32_t head, uint32_t end) {
    uint32_t written = 0, counted = 0, read_cmp = 0, ab_read = 0, pc = head;
    bool load = false;
    for (unsigned n = 0; n < M2_SPIN_BODY_MAX && pc <= end; n++) {
        uint32_t w1 = mem_read32(bus, pc), cls = w1 >> 28;
        if (cls == 0x8 || cls == 0x9 || cls == 0xC) {
            uint32_t op = w1 >> 24, mode = MEM_MODE(w1);
            if (op != 0x80 && op != 0x88 && op != 0x90 && op != 0xC0 && op != 0xC8) return false;
            if (mode == 0xC) {
                if (!m2_spin_addr_ok(mem_read32(bus, pc + 4u))) return false;
                pc += 4u;
            } else if (mode == 0xD) {
                ab_read |= 1u << MEM_ABASE(w1);
                pc += 4u;
            } else if (mode == 0x4 || (mode & 0xC) == 0x8) {
                ab_read |= 1u << MEM_ABASE(w1);
            } else if (mode & 0x4) {
                return false;
            }
            written |= 1u << MEM_SRCDST(w1);
            load = true;
        } else if (cls >= 0x5 && cls <= 0x7) {
            uint32_t op = REG_OPCODE(w1), d = REG_DST(w1);
            int kind = m2_spin_reg_kind(op);
            if (!kind || (REG_M3(w1) && op != 0x5a0 && op != 0x5a1)) return false;   /* m3: dst an SFR */
            uint32_t src = (REG_M1(w1) ? 0u : 1u << REG_SRC1(w1)) | (REG_M2(w1) || kind == 3 ? 0u : 1u << REG_SRC2(w1));
            if (kind == 1) read_cmp |= src;
            if (op == 0x5a0 || op == 0x5a1) { pc += 4u; continue; }
            if (kind == 2 && (src >> d & 1u)) counted |= 1u << d;
            written |= 1u << d;
        } else if (cls <= 0x1) {
            uint32_t op = CTRL_OPCODE(w1);
            if (op != 0x08 && (op < 0x10 || op > 0x17)) return false;
        } else if (cls == 0x3) {
            uint32_t op = COBR_OPCODE(w1);
            if (!((op >= 0x31 && op <= 0x36) || (op >= 0x39 && op <= 0x3E))) return false;
            read_cmp |= (COBR_M1(w1) ? 0u : 1u << COBR_SRC1(w1)) | 1u << COBR_SRC2(w1);
        } else {
            return false;
        }
        if (pc == end) return load && !(ab_read & written) && !(read_cmp & counted);
        pc += 4u;
    }
    return false;
}

/* The loop a backward branch at `a` closes, or 0: a b / bxx / compare-and-
 * branch whose target is at most M2_SPIN_BODY_MAX words back. */
static inline uint32_t m2_spin_head(memory_bus_t *bus, uint32_t a) {
    uint32_t w = mem_read32(bus, a), cls = w >> 28;
    int32_t disp;
    if (cls <= 0x1) {
        uint32_t op = CTRL_OPCODE(w);
        if (op != 0x08 && (op < 0x10 || op > 0x17)) return 0;
        disp = (int32_t)(w & 0x00FFFFFCu);
        if (disp & 0x00800000) disp |= (int32_t)0xFF000000;
    } else if (cls == 0x3) {
        uint32_t op = COBR_OPCODE(w);
        if (!((op >= 0x31 && op <= 0x36) || (op >= 0x39 && op <= 0x3E))) return 0;
        disp = (int32_t)(w & 0x00001FFCu);
        if (disp & 0x1000) disp |= (int32_t)0xFFFFE000;
    } else {
        return 0;
    }
    if (disp >= 0 || disp < -4 * (M2_SPIN_BODY_MAX + 2) || (uint32_t)-disp > a) return 0;
    return a + (uint32_t)disp;
}

/* The heads of the loops closed in [lo, hi), from g_hle_spin_sites[n] on, up to
 * the list's end. Returns the new count. */
static inline size_t m2_spin_scan(memory_bus_t *bus, uint32_t lo, uint32_t hi, size_t n) {
    size_t first = n;
    for (uint32_t a = lo + 4u; a + 4u <= hi && n < HLE_SPIN_SITES_MAX; a += 4) {
        uint32_t head = m2_spin_head(bus, a);
        if (head >= lo && (n == first || g_hle_spin_sites[n - 1] != head) && m2_spin_is_loop(bus, head, a))
            g_hle_spin_sites[n++] = head;
    }
    return n;
}

/* How many of g_hle_spin_sites are the program ROM's; work RAM's follow. */
static size_t g_spin_rom_sites;

/* Work RAM's loops, again: a program that runs code from work RAM (Sega
 * Rally unpacks its own there, Pilot Kids copies its) has its waits there, and
 * puts them there after the install. The run loop calls this every 64 frames
 * (g_hle_spin_rescan); a site never changes what the board does, so when it
 * runs, or whether, is nobody's business but the host's. */
static inline void m2_spin_rescan(memory_bus_t *bus) {
    uint32_t old[HLE_SPIN_SITES_MAX];
    size_t was = g_hle_spin_count;
    memcpy(old, g_hle_spin_sites, sizeof old);
    size_t n = m2_spin_scan(bus, 0x00500000u, 0x00600000u, g_spin_rom_sites);
    if (n == was && !memcmp(old, g_hle_spin_sites, n * sizeof old[0])) return;
    g_hle_spin_count = n;
    g_hle_filter_gen++;
}

/* Find the poll loops m2_spin_skip takes in a program, and in work RAM, and
 * hook them for `prof` (hle_hooks.h, g_hle_spin_sites). Every game's install
 * does (m2_spin_install): a profile names no idle loop of its own. The scan
 * matches words, and the hook checks the loop again at every call, so a match
 * in data costs nothing but a filter bit. The AOT compiler gets them from the
 * aotmap's hook lines (det_digest), so compiled code stops at each. Returns how
 * many. */
static inline size_t m2_spin_find(memory_bus_t *bus, uint32_t size, const game_profile_t *prof) {
    g_spin_rom_sites   = m2_spin_scan(bus, 0, size, 0);
    g_hle_spin_count   = m2_spin_scan(bus, 0x00500000u, 0x00600000u, g_spin_rom_sites);
    g_hle_spin_profile = prof;
    g_hle_spin_hook    = m2_spin_skip;
    g_hle_spin_rescan  = m2_spin_rescan;
    g_hle_filter_gen++;
    if (g_spin_rom_sites)
        LOG_INFO("spin: %u poll loop%s in the program", (unsigned)g_spin_rom_sites, g_spin_rom_sites == 1 ? "" : "s");
    return g_hle_spin_count;
}

/* A game's install: the profile's, then the poll loops of the program it
 * loaded. Every frontend installs through this. */
static inline void m2_spin_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    g_active_profile->install_fn(rs, cpu, bus);
    if (rs->loaded) m2_spin_find(bus, (uint32_t)rs->maincpu_size, g_active_profile);
    else { g_hle_spin_count = 0; g_hle_spin_profile = NULL; g_hle_spin_rescan = NULL; g_hle_filter_gen++; }
}

/* A timed wait on board timer 2, polled while the vblank byte stays 0 (STF's
 * texture-row senders, 0x4BE58 and 0x4C008; Pinboard #394):
 *
 *   loop: ldob    0x500000, r14
 *         cmpobne 0, r14, out        ; the vblank handler moved it
 *         ldl     0xF00008, r14      ; MMIO: both words read timer 2
 *         lda     0xFFFFF, r14
 *         and     r15, r14, r15
 *         cmpobe  r15, r14, out      ; timer 2 idle
 *         cmpobg  r15, r13, loop     ; until it counts down to r13
 *   out:
 *
 * Each pass reads the timer, which brings the counts up to date and so ends a
 * compiled run (i960_aot.h): ~600 runs a fight frame on the Dreamcast, each
 * through the interpreter's load. The timer counts down by the i960's cycles,
 * so what each pass would read is known: the count less the cycles up to that
 * pass's ldl. A hook on the ldob runs as many whole passes as stay in the loop
 * and end before the timers' horizon, on the same terms as m2_spin_skip; the
 * i960 runs the one that leaves. The code is checked at every call, against
 * words built from the listing above. */
#define M2_MEMB_ABS(op, d)            ((uint32_t)(op) << 24 | (uint32_t)(d) << 19 | 0x3000u)
#define M2_COBR(op, s1, s2, m1, disp) ((uint32_t)(op) << 24 | (uint32_t)(s1) << 19 | (uint32_t)(s2) << 14 \
                                       | (uint32_t)(m1) << 13 | ((uint32_t)(disp) & 0x1FFCu))
#define M2_REG(op, sub, d, s2, s1)    ((uint32_t)(op) << 24 | (uint32_t)(d) << 19 | (uint32_t)(s2) << 14 \
                                       | (uint32_t)(sub) << 7 | (uint32_t)(s1))
static const uint32_t m2_spin_timed_words[10] = {
    M2_MEMB_ABS(0x80, 14), 0x00500000u,          /* ldob    0x500000, r14 */
    M2_COBR(0x35, 0, 14, 1, 32),                 /* cmpobne 0, r14, out */
    M2_MEMB_ABS(0x98, 14), 0x00F00008u,          /* ldl     0xF00008, r14 */
    M2_MEMB_ABS(0x8C, 14), 0x000FFFFFu,          /* lda     0xFFFFF, r14 */
    M2_REG(0x58, 1, 15, 14, 15),                 /* and     r15, r14, r15 */
    M2_COBR(0x32, 15, 14, 0, 8),                 /* cmpobe  r15, r14, out */
    M2_COBR(0x31, 15, 13, 0, -36),               /* cmpobg  r15, r13, loop */
};

static inline int m2_spin_timed(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!g_spin_skip || g_hle_room < 7 || wp_armed()) return 1;
    if (g_irqt.intreq & g_irqt.intena & 0x03FFu) return 1;
    const uint32_t ip = cpu->sfr.ip;
    for (uint32_t i = 0; i < 10; i++)
        if (mem_read32(bus, ip + 4u * i) != m2_spin_timed_words[i]) return 1;
    if (mem_read8(bus, 0x00500000u) != 0) return 1;
    if (!g_irqt.timer_run[2]) return 1;        /* reads 0xFFFFF: leaves at once */

    /* the passes' cycles, and those up to the ldl's read */
    static const uint8_t at[7] = { 0, 8, 12, 20, 28, 32, 36 };
    int64_t c = 0, c_ld = 0;
    for (int i = 0; i < 7; i++) {
        if (i == 2) c_ld = c;
        c += i960_cycle_cost(m2_spin_timed_words[at[i] / 4u]);
    }
    int64_t k = (g_irqt.horizon - g_irqt.pending - 1) / c;   /* ends before the horizon */
    if (k > (int64_t)(g_hle_room / 7u)) k = g_hle_room / 7u;
    /* the reads stay above r13: count - pending - j*c - c_ld > r13 for j < k.
     * Timer 2 runs, so the horizon is at most its count and every read is
     * positive, under 0xFFFFF. */
    const int64_t first = g_irqt.timer_count[2] - g_irqt.pending - c_ld;
    const uint32_t r13 = reg_read(cpu, 13);
    if (first <= (int64_t)r13) return 1;
    int64_t stay = (first - (int64_t)r13 - 1) / c + 1;
    if (k > stay) k = stay;
    if (k < 1) return 1;
    const uint32_t last = (uint32_t)(first - (k - 1) * c);
    if (last >= 0xFFFFFu) return 1;

    reg_write(cpu, 14, 0xFFFFFu);
    reg_write(cpu, 15, last);
    set_cc(cpu, i960_cmp_cc_o(last, r13));
    bus->cpu_ip = ip + 36u;
    cpu->cycles += (uint64_t)(k * c);
    g_hle_extra = (uint32_t)(7 * k - 1);
    emu_attn_bump();
    g_spin_iters += (uint64_t)k;
    return 0;
}

#endif /* M2_SPIN_H */
