#ifndef I960_H
#define I960_H

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

    // Running state
    int            halted;
} i960_cpu_t;

static inline void i960_reset(i960_cpu_t *cpu) {
    memset(cpu, 0, sizeof(i960_cpu_t));
}

#endif // I960_H
