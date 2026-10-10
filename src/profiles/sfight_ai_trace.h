/*
 * sfight_ai_trace.h -- log STF's random numbers and its CPU fighters' decisions,
 * one CSV line per event (Pinboard #597). Observe only: it hangs off
 * g_hle_watch_hook (hle_hooks.h), reads registers and RAM, and the board runs
 * exactly as it would without it.
 *
 * The random number generator is `rand` (0x66B0). It is not a recurrence: it
 * adds the four board timers into `random` (0x500098),
 *     random += (T0 << 4) + (T1 << 8) + (T2 << 12) + (T3 << 16)
 * and answers (random >> 4) & 0xFFFF. So its values are only as good as the
 * board's timing, and a trace taken on another board (MAME, the PS3 port, the
 * real one) need not have the same ones. The trace does not assume the formula:
 * each R line carries the four timer words the code loaded and the state before
 * and after, and tools/stf_ai/ai_trace.py solves the advance from them.
 *
 * The CPU fighter is a bytecode interpreter (enemy_param_mngr 0x3B794 →
 * select_enemy_command 0x3C214, one script at 0x947C8 for every character):
 * sec_cc_next (0x3C284) fetches each command at g6, condition commands fall to
 * sec_cc_fault (0x3E1CC) when they fail, and an action command returns into
 * stick_ctrl (0x3E4F0), which turns the chosen waza into stick and buttons.
 * Almost every random draw the AI makes goes into the byte at 0xCB of its AI
 * record (g4), which the k_jump commands compare against their thresholds.
 * tools/stf_ai/ai_trace.py reads this log.
 *
 * Lines (all numbers hex but frame):
 *   S,frame,random                       start of a frame
 *   R,frame,site,old,new,value,t0,t1,t2,t3   rand called from site
 *   C,frame,player,pc,op                 command fetched at script pc
 *   F,frame,player,push                  a condition failed; push = 0x114(g4)
 *   A,frame,player,waza,level,rnd,status,chara   stick_ctrl entered
 *   I,frame,t0,t1,t2,t3,vbl              _idle entered (0x11608): the timers
 *                                        and the cycles left to the vblank
 *   T,frame,byte                         a byte sent on the RS-422 link: the
 *                                        m2-sdk strobe (0x07 to TXD1) after a
 *                                        data byte in TXD2. A patched program
 *                                        (stfdisasm rng-serial) reports its
 *                                        RNG this way; the stock game sends none.
 * player is byte 4 of the fighter's rob (0 = 1P side). waza = 0xBA(g4),
 * level = 0x1200(g7), rnd = 0xCB(g4), status = 0x40(g4), chara = 0xC9(g4).
 */
#ifndef PROFILES_SFIGHT_AI_TRACE_H
#define PROFILES_SFIGHT_AI_TRACE_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "i960.h"
#include "memory.h"
#include "hle_hooks.h"
#include "game_profile.h"

#define SAT_RAND_LOAD_T0 0x000066C0u   /* r13 = T0 */
#define SAT_RAND_LOAD_T1 0x000066CCu   /* r13 = T1 */
#define SAT_RAND_LOAD_T2 0x000066D8u   /* r13 = T2 */
#define SAT_RAND_LOAD_T3 0x000066E4u   /* r13 = T3 */
#define SAT_RAND_STORE   0x000066ECu   /* st r15, random: r15 new, RAM old */
#define SAT_CC_NEXT      0x0003C284u   /* sec_cc_next: g6 = command */
#define SAT_CC_FAULT     0x0003E1CCu   /* sec_cc_fault */
#define SAT_STICK_CTRL   0x0003E4F0u   /* stick_ctrl entry */
#define SAT_IDLE         0x00011608u   /* interrupt_wait_b: ldob 0x500000, g0 */
#define SAT_RANDOM       0x00500098u

static const uint32_t s_sat_sites[] = {
    SAT_RAND_LOAD_T0, SAT_RAND_LOAD_T1, SAT_RAND_LOAD_T2, SAT_RAND_LOAD_T3,
    SAT_RAND_STORE, SAT_CC_NEXT, SAT_CC_FAULT, SAT_STICK_CTRL, SAT_IDLE,
};

static struct {
    FILE    *out;
    int      on;          /* the active profile is STF's */
    uint32_t frame;
    int      started;     /* an S line went out for frame */
    uint32_t t[4];
    mem_region_t *io;     /* the I/O region whose writes the trace taps */
} s_sat;

#define SAT_TXD1 0x12u    /* IO_BASE + 0x12: the strobe (command 0x07 = send) */
#define SAT_TXD2 0x14u    /* IO_BASE + 0x14: the data byte */

static inline uint32_t sat_player(i960_cpu_t *cpu, memory_bus_t *bus) {
    return mem_read8(bus, cpu->globals.g[7] + 4u);
}

/* A timer as a read would see it, without bringing the board's counts up to
 * date (irqt_timer_read flushes; this must not touch the board). */
static inline uint32_t sat_timer(int t) {
    if (!g_irqt.timer_run[t]) return (uint32_t)IRQT_IDLE;
    int64_t c = g_irqt.timer_count[t] - g_irqt.pending;
    return (uint32_t)(c < 0 ? 0 : c) & 0xFFFFFu;
}

static void sat_watch(i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!s_sat.out || !s_sat.on) return;
    uint32_t ip = cpu->sfr.ip, g4 = cpu->globals.g[4], g7 = cpu->globals.g[7];
    switch (ip) {
    case SAT_RAND_LOAD_T0: case SAT_RAND_LOAD_T1:
    case SAT_RAND_LOAD_T2: case SAT_RAND_LOAD_T3:
        s_sat.t[(ip - SAT_RAND_LOAD_T0) / 12u] = cpu->locals.r[13];
        break;
    case SAT_RAND_STORE: {
        uint32_t nw = cpu->locals.r[15];
        fprintf(s_sat.out, "R,%u,%X,%X,%X,%X,%X,%X,%X,%X\n", s_sat.frame,
                cpu->locals.r[2] - 4u, mem_read32(bus, SAT_RANDOM), nw, (nw >> 4) & 0xFFFFu,
                s_sat.t[0], s_sat.t[1], s_sat.t[2], s_sat.t[3]);
        break;
    }
    case SAT_CC_NEXT: {
        uint32_t g6 = cpu->globals.g[6];
        fprintf(s_sat.out, "C,%u,%u,%X,%X\n", s_sat.frame, sat_player(cpu, bus), g6,
                mem_read8(bus, g6));
        break;
    }
    case SAT_CC_FAULT:
        fprintf(s_sat.out, "F,%u,%u,%X\n", s_sat.frame, sat_player(cpu, bus),
                mem_read32(bus, g4 + 0x114u));
        break;
    case SAT_STICK_CTRL:
        fprintf(s_sat.out, "A,%u,%u,%X,%X,%X,%X,%X\n", s_sat.frame, sat_player(cpu, bus),
                mem_read8(bus, g4 + 0xBAu), mem_read8(bus, g7 + 0x1200u),
                mem_read8(bus, g4 + 0xCBu), mem_read32(bus, g4 + 0x40u),
                mem_read8(bus, g4 + 0xC9u));
        break;
    case SAT_IDLE:
        fprintf(s_sat.out, "I,%u,%X,%X,%X,%X,%lld\n", s_sat.frame, sat_timer(0),
                sat_timer(1), sat_timer(2), sat_timer(3),
                (long long)(irqt__vbl_left() - g_irqt.pending));
        break;
    default:
        break;   /* another address on a site's filter bit */
    }
}

/* The I/O region's writes, stored exactly as the bus's plain path stores
 * them; a strobe also logs the byte latched in TXD2. */
static void sat_io_write(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    uint32_t off = addr - r->base;
    if (!r->data || off + (uint32_t)size > r->size) return;
    uint32_t old = 0;
    for (int i = 0; i < size; i++) old |= (uint32_t)r->data[off + i] << (8 * i);
    for (int i = 0; i < size; i++) r->data[off + i] = (uint8_t)(val >> (8 * i));
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1u;
    if (r->change_gen && old != (val & mask)) mem__note_change(r, off, (uint32_t)size);
    if (off == SAT_TXD1 && (val & 0xFF) == 0x07 && s_sat.out && s_sat.on)
        fprintf(s_sat.out, "T,%u,%X\n", s_sat.frame, r->data[SAT_TXD2]);
}

/* Hang sat_io_write on the bus's I/O region (a ROM load rebuilds the bus). */
static void sat_tap_io(memory_bus_t *bus) {
    for (int i = 0; i < bus->region_count; i++) {
        mem_region_t *r = &bus->regions[i];
        if (r->base != IO_BASE) continue;
        if (r->write_cb == sat_io_write) return;
        if (r->write_cb) return;   /* someone else owns its writes: leave it */
        r->write_cb = sat_io_write;
        s_sat.io = r;
        mem_regions_changed(bus);
        return;
    }
}

/* Start logging to path ("-" is stdout). Returns 0 on success. */
static int sfight_ai_trace_open(const char *path) {
    s_sat.out = strcmp(path, "-") == 0 ? stdout : fopen(path, "w");
    if (!s_sat.out) return -1;
    fprintf(s_sat.out, "# m2hle sfight_ai_trace v1\n");
    s_sat.frame = 0;
    s_sat.started = 0;
    g_hle_watch_sites = s_sat_sites;
    g_hle_watch_count = sizeof s_sat_sites / sizeof s_sat_sites[0];
    g_hle_watch_hook  = sat_watch;
    g_hle_filter_gen++;
    return 0;
}

static void sfight_ai_trace_close(void) {
    if (!s_sat.out) return;
    g_hle_watch_hook = NULL;
    g_hle_watch_count = 0;
    if (s_sat.io && s_sat.io->write_cb == sat_io_write) s_sat.io->write_cb = NULL;
    s_sat.io = NULL;
    g_hle_filter_gen++;
    if (s_sat.out != stdout) fclose(s_sat.out); else fflush(stdout);
    s_sat.out = NULL;
}

/* The host calls this before each slice, with the number of the game frame
 * the slice runs in; a frame of several slices gets one S line. */
static void sfight_ai_trace_frame(memory_bus_t *bus, uint32_t frame) {
    if (!s_sat.out || (s_sat.started && frame == s_sat.frame)) return;
    const game_profile_t *p = g_active_profile;
    s_sat.on = p && p->rom_set && strcmp(p->rom_set, "sfight") == 0;
    s_sat.frame = frame;
    s_sat.started = 1;
    sat_tap_io(bus);
    if (s_sat.on) fprintf(s_sat.out, "S,%u,%X\n", frame, mem_read32(bus, SAT_RANDOM));
}

#endif /* PROFILES_SFIGHT_AI_TRACE_H */
