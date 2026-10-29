#ifndef I960_H
#define I960_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "constants.h"

//--- Register file ------------------------------------------------------------

typedef struct {
    union {
        uint32_t r[16];
        struct {
            uint32_t pfp;   // r0  - previous frame pointer
            uint32_t sp;    // r1  - stack pointer
            uint32_t rip;   // r2  - return instruction pointer
            uint32_t r3;
            uint32_t r4;
            uint32_t r5;
            uint32_t r6;
            uint32_t r7;
            uint32_t r8;
            uint32_t r9;
            uint32_t r10;
            uint32_t r11;
            uint32_t r12;
            uint32_t r13;
            uint32_t r14;
            uint32_t r15;
        };
    };
} local_regs_t;

typedef struct {
    union {
        uint32_t g[16];
        struct {
            uint32_t g0;
            uint32_t g1;
            uint32_t g2;
            uint32_t g3;
            uint32_t g4;
            uint32_t g5;
            uint32_t g6;
            uint32_t g7;
            uint32_t g8;
            uint32_t g9;
            uint32_t g10;
            uint32_t g11;
            uint32_t g12;
            uint32_t g13;
            uint32_t g14;
            uint32_t fp;    // g15 - frame pointer
        };
    };
} global_regs_t;

typedef struct {
    uint32_t ip;    // Instruction Pointer
    uint32_t ac;    // Arithmetic Controls
    uint32_t pc;    // Process Controls
    uint32_t tc;    // Trace Controls
} sfr_t;

//--- CPU state ----------------------------------------------------------------

typedef struct i960_cpu {
    global_regs_t  globals;
    local_regs_t   locals;
    sfr_t          sfr;

    // Floating point registers (fp0-fp3, stored as double for HLE)
    double         fp_regs[4];

    // Frame stack for register window save/restore
    local_regs_t   frame_stack[FRAME_STACK_DEPTH];
    int            frame_depth;

    /* Interrupt frames. The processor saves PC and AC in the frame it builds
     * for an interrupt, and `ret` restores both when the frame's return status
     * says it was one. frame_irq[d] marks frame_stack[d] that way; every push
     * sets it, every pop reads it. Without it a handler's own compares leak
     * into the condition code of whatever instruction it interrupted. */
    uint8_t        frame_irq[FRAME_STACK_DEPTH];
    uint32_t       frame_irq_ac[FRAME_STACK_DEPTH];
    uint32_t       frame_irq_pc[FRAME_STACK_DEPTH];

    /* The caller's frame pointer (g15) at each push, put back by `ret`. A call
     * makes g15 the new frame's base, (SP + 63) & ~63, and SP that + 64 (MAME
     * do_call). g15 used to be set to pfp, which this model leaves at 0, so
     * code that keeps its locals at fp+N (every gcc960 function) wrote them
     * into ROM; STF's compiler never addresses through fp. */
    uint32_t       frame_fp[FRAME_STACK_DEPTH];

    /* `flushreg` puts the cached frames in memory, each at its FP in the
     * hardware's layout (r0 pfp, r1 sp, r2 the IP the frame resumes at), and a
     * `ret` into one loads it back from there, at pfp. frame_stack[d] for d <
     * frame_flushed is stale then. frame_mem says a flush ever happened, so a
     * `ret` with nothing cached reads memory instead of halting. Dynamite Cop's
     * task switch (0x2E80) rewrites the frame it returns into that way. */
    int            frame_flushed;
    int            frame_mem;

    // Running state
    int            halted;

    /* Clock cycles the instructions run so far cost, at MAME's per-opcode
     * estimates (i960_cycle_cost), or the board's (i960_timing.h). Only
     * live board timers read it. */
    uint64_t       cycles;

    /* The PRCB the processor was started or last re-initialised with (IAC
     * reinitialize). Its interrupt table is where an interrupt pin's vector
     * finds its handler (hle_irq_vector_handler). */
    uint32_t       prcb;

    /* The PRCB's interrupt table and interrupt stack, as the processor took
     * them when it read the PRCB (i960_prcb_cache): it does not read them
     * again at an interrupt. Pilot Kids and Zero Gunner clear their RAM PRCB
     * after the IAC reinitialize and run on (MAME patches the clear out).
     * prcb_cached is the PRCB they came from; 0, nothing taken yet. */
    uint32_t       prcb_cached, intr_table, intr_stack;
} i960_cpu_t;

/* How the i960 is charged: MAME's per-opcode table (i960_cycle_cost), or, for a
 * profile that sets game_quirks_t.i960_board_timing, the table plus what the
 * board's caches and wait states add (i960_timing.h). The board model's
 * state -- the cache tags, the load scoreboard, the cycle's remainder -- is
 * board state that profile saves (savestate_extra), and a reset clears it.
 * The hooks that charge a run of instructions themselves (m2_spin.h,
 * m2_texload.h) decline while it is on. */
#define I960_CYC_ONE  1024u
#define I960_BT_LINES 32u
typedef struct {
    uint32_t tag[I960_BT_LINES];  /* line number + 1 held by each cache line, 0 empty */
    uint32_t last_line;           /* the line the last instruction ended in */
    uint32_t next_ip;             /* where the last instruction falls through to */
    uint32_t pend0, pend1;        /* registers the last load / the one before fill */
    uint32_t brrun;               /* line + 1 that runs on from a branch's missed line, 0 none */
    uint8_t  prev_op, prev_valid, last_valid, pad;
    uint8_t  line_ld, line_st, pad2[2];  /* the line holds a long instruction / only long stores */
} i960_bt_t;
static bool      g_i960_bt_on;
static i960_bt_t g_i960_bt;
static uint32_t  g_i960_cyc_frac;
/* --i960-timing: -1 the profile's choice, 0 the table, 1 the board's. */
static int       g_i960_timing_force = -1;

/* Room to push a frame. A full cache drops the frames flushreg already put in
 * memory (i960.h, frame_flushed): a program that switches tasks never pops
 * them, as Dynamite Cop's main loop does not (0x142C, a frame a pass). */
static inline bool i960_frame_room(i960_cpu_t *cpu) {
    if (cpu->frame_depth < FRAME_STACK_DEPTH) return true;
    int k = cpu->frame_flushed;
    if (k <= 0) return false;
    size_t n = (size_t)(cpu->frame_depth - k);
    memmove(cpu->frame_stack,  cpu->frame_stack + k,  n * sizeof cpu->frame_stack[0]);
    memmove(cpu->frame_irq,    cpu->frame_irq + k,    n * sizeof cpu->frame_irq[0]);
    memmove(cpu->frame_irq_ac, cpu->frame_irq_ac + k, n * sizeof cpu->frame_irq_ac[0]);
    memmove(cpu->frame_irq_pc, cpu->frame_irq_pc + k, n * sizeof cpu->frame_irq_pc[0]);
    memmove(cpu->frame_fp,     cpu->frame_fp + k,     n * sizeof cpu->frame_fp[0]);
    cpu->frame_depth   = (int)n;
    cpu->frame_flushed = 0;
    return true;
}

static inline void i960_reset(i960_cpu_t *cpu) {
    memset(cpu, 0, sizeof(i960_cpu_t));
    g_i960_cyc_frac = 0;
    memset(&g_i960_bt, 0, sizeof g_i960_bt);
}

#endif // I960_H
