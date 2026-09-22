#ifndef I960_EXEC_H
#define I960_EXEC_H

#include "i960.h"
#include "memory.h"
#include "log.h"
#include "hle_hooks.h"
#include "trace_window.h"
#include <math.h>
#include <string.h>

//--- Instruction format helpers -----------------------------------------------

// Extract register index for MEM format
#define MEM_SRCDST(w)   (((w) >> 19) & 0x1F)
#define MEM_ABASE(w)    (((w) >> 14) & 0x1F)
#define MEM_REG3(w)     ((w) & 0x1F)
#define MEM_MODE(w)     (((w) >> 10) & 0xF)
#define MEM_SCALE(w)    (((w) >> 7) & 0x7)
#define MEM_OFFSET(w)   ((w) & 0xFFF)

// Extract fields for REG format
#define REG_OPCODE(w)   ((((w) >> 20) & 0xFF0) | (((w) >> 7) & 0xF))
#define REG_SRC1(w)     ((w) & 0x1F)
#define REG_SRC2(w)     (((w) >> 14) & 0x1F)
#define REG_DST(w)      (((w) >> 19) & 0x1F)
#define REG_M1(w)       (((w) >> 11) & 1)
#define REG_M2(w)       (((w) >> 12) & 1)
#define REG_M3(w)       (((w) >> 13) & 1)

// Extract fields for CTRL format
#define CTRL_OPCODE(w)  (((w) >> 24) & 0xFF)

// Extract fields for COBR format
#define COBR_OPCODE(w)  (((w) >> 24) & 0xFF)
#define COBR_SRC1(w)    (((w) >> 19) & 0x1F)
#define COBR_SRC2(w)    (((w) >> 14) & 0x1F)
#define COBR_M1(w)      (((w) >> 13) & 1)

//--- Register read helpers (handle literal mode) ------------------------------

/* Register index 0..15 is r0..r15 (locals), 16..31 is g0..g15 (globals). The
 * CPU struct holds globals then locals, 16 words each, so (idx + 16) & 31 is
 * the word's place counted from globals: one indexed load, no branch on which
 * bank. An index past 31 (movq's dst + 3 can reach 34) reads 0 and writes
 * nothing, as before. */
_Static_assert(offsetof(i960_cpu_t, locals) == offsetof(i960_cpu_t, globals) + 16 * sizeof(uint32_t),
               "reg_read/reg_write need locals to follow globals");

static inline uint32_t reg_read(i960_cpu_t *cpu, int idx) {
    if ((unsigned)idx >= 32u) return 0;
    return ((uint32_t *)&cpu->globals)[(idx + 16) & 31];
}

static inline void reg_write(i960_cpu_t *cpu, int idx, uint32_t val) {
    if ((unsigned)idx >= 32u) return;
    ((uint32_t *)&cpu->globals)[(idx + 16) & 31] = val;
}

// REG format: operand value, respecting literal mode bit
static inline uint32_t reg_src(i960_cpu_t *cpu, int idx, int mode) {
    if (mode) return (uint32_t)idx;  // literal
    return reg_read(cpu, idx);
}

/* ---- NaNs, spelled out ------------------------------------------------------
 * IEEE leaves a NaN's sign and payload to the implementation, and the two build
 * families take it: the desktop (MSVC, x86) and the browser (clang, wasm) came
 * out with NaNs of different sign from the same instruction, and a NaN a game
 * stores is a word it can test. Lockstep netplay needs the two to agree to the
 * bit (tests/det_digest.c), so the FP instructions fix the rule x86 applies,
 * which is what MSVC builds and MAME have always produced:
 *   - a NaN operand is the result, quieted (the first operand of the C
 *     expression when both are NaN);
 *   - otherwise an invalid operation gives the real indefinite (sign set, quiet).
 * Widening a single to double and narrowing back keep the payload's top bits
 * and set the quiet bit, as cvtss2sd / cvtsd2ss do. */
#define I960_QNAN_BIT      0x0008000000000000ull
#define I960_REAL_INDEF    0xFFF8000000000000ull

static inline double i960_bits_to_double(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static inline uint64_t i960_double_to_bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

static inline double i960_nan_result(double r, double a, double b) {
    if (r == r) return r;
    if (a != a) return i960_bits_to_double(i960_double_to_bits(a) | I960_QNAN_BIT);
    if (b != b) return i960_bits_to_double(i960_double_to_bits(b) | I960_QNAN_BIT);
    return i960_bits_to_double(I960_REAL_INDEF);
}

static inline double i960_single_to_double(uint32_t u) {
    if ((u & 0x7F800000u) == 0x7F800000u && (u & 0x007FFFFFu))
        return i960_bits_to_double(((uint64_t)(u >> 31) << 63) | 0x7FF0000000000000ull | I960_QNAN_BIT
                                   | ((uint64_t)(u & 0x007FFFFFu) << 29));
    float f;
    memcpy(&f, &u, 4);
    return (double)f;
}

static inline uint32_t i960_double_to_single(double d) {
    if (d != d) {
        uint64_t u = i960_double_to_bits(d);
        return (uint32_t)(u >> 63) << 31 | 0x7FC00000u | (uint32_t)((u >> 29) & 0x003FFFFFu);
    }
    float f = (float)d;
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// Read an integer register's contents as a double, treating the 32 bits as
// the IEEE-754 binary32 (float) bit pattern. Used by FP instructions whose
// source operand is in a general-purpose register — such operations interpret
// those bits as a float, NOT as an integer count to be converted.
static inline double i960_int_reg_as_double(i960_cpu_t *cpu, int idx) {
    return i960_single_to_double(reg_read(cpu, idx));
}

/* A real converted to a 32-bit integer. One outside the integer range, or a
 * NaN, gives 0x80000000 (the integer indefinite). A C cast leaves that case
 * undefined, and hosts differ: x86 produces 0x80000000 — so MAME does, and so
 * this emulator did on every grader — while AArch64 saturates to 0x7FFFFFFF.
 * `get_kamae_value` (0x2FEF0) runs `cvtri` over stance values some of which are
 * out of range and stores the low half with `stis`: the ARM build kept 0xFFFF
 * where x86 kept 0, and its fights drifted from there. */
static inline uint32_t i960_real_to_int32(double v) {
    if (!(v > -2147483649.0 && v < 2147483648.0)) return 0x80000000u;
    return (uint32_t)(int32_t)v;
}

// Real-to-integer rounding by the AC rounding-control bits (30-31):
// 0 nearest (IEEE ties-to-even), 1 down, 2 up, 3 toward zero.
static inline double i960_round_ac(i960_cpu_t *cpu, double v) {
    switch ((cpu->sfr.ac >> 30) & 3) {
    case 0: {
        double r = floor(v + 0.5);
        if (r - v == 0.5 && fmod(r, 2.0) != 0.0) r -= 1.0;
        return r;
    }
    case 1:  return floor(v);
    case 2:  return ceil(v);
    default: return trunc(v);
    }
}

//--- MEM format effective address calculation ---------------------------------

static inline uint32_t mem_ea(i960_cpu_t *cpu, uint32_t word1, uint32_t word2, int *len) {
    uint32_t abase_val = reg_read(cpu, MEM_ABASE(word1));

    *len = 4;

    // Bit 12 of the mode field distinguishes MEMA (0) from MEMB (1)
    int mode = MEM_MODE(word1);

    if (!(mode & 0x4)) {
        // MEMA format: 12-bit unsigned offset, optional abase
        uint32_t offset = word1 & 0xFFF;
        if (mode & 0x8) {
            // offset(abase)
            return abase_val + offset;
        } else {
            // offset only
            return offset;
        }
    } else {
        // MEMB format: uses mode field for addressing mode selection
        uint32_t index_val = reg_read(cpu, MEM_REG3(word1));
        static const int scale_tab[] = { 1, 2, 4, 8, 16 };
        int scale_idx = MEM_SCALE(word1);
        int scale = (scale_idx <= 4) ? scale_tab[scale_idx] : 1;

        switch (mode) {
            case 0x4:  // (abase)
                return abase_val;
            case 0x5:  // IP + displacement + 8
                *len = 8;
                return cpu->sfr.ip + 8 + word2;
            case 0x7:  // (abase)[index*scale]
                return abase_val + index_val * scale;
            case 0xC:  // displacement
                *len = 8;
                return word2;
            case 0xD:  // displacement(abase)
                *len = 8;
                return word2 + abase_val;
            case 0xE:  // displacement[index*scale]
                *len = 8;
                return word2 + index_val * scale;
            case 0xF:  // displacement(abase)[index*scale]
                *len = 8;
                return word2 + abase_val + index_val * scale;
            default:
                LOG_WARN("mem_ea: unhandled MEMB mode 0x%X at IP=0x%08X", mode, cpu->sfr.ip);
                return 0;
        }
    }
}

//--- Condition code helpers ---------------------------------------------------

static inline void set_cc(i960_cpu_t *cpu, uint32_t cc) {
    cpu->sfr.ac = (cpu->sfr.ac & ~AC_CC_MASK) | (cc & AC_CC_MASK);
}

static inline uint32_t get_cc(i960_cpu_t *cpu) {
    return cpu->sfr.ac & AC_CC_MASK;
}

/* The eight conditions the CTRL branches (bno..bo), the COBR tests (testno..
 * testo) and the compare-and-branches share, in the opcode's low three bits:
 * a mask over the condition code (bit 0 greater, 1 equal, 2 less), true when
 * any masked bit is set -- and, for the empty mask (the "no" forms), true when
 * none is. */
static inline bool i960_cond(uint32_t cc, uint32_t mask) {
    return mask ? (cc & mask) != 0 : cc == 0;
}

/* The condition code a compare leaves: exactly one of L, E, G. */
static inline uint32_t i960_cmp_cc_o(uint32_t a, uint32_t b) { return a < b ? CC_L : a == b ? CC_E : CC_G; }
static inline uint32_t i960_cmp_cc_i(int32_t a, int32_t b)   { return a < b ? CC_L : a == b ? CC_E : CC_G; }

//--- Instruction cost in clock cycles -------------------------------------------

/* What an instruction costs at the i960KB's 25 MHz, as MAME's i960.cpp charges
 * it (its own estimates — "exact timing unknown" — but the numbers the board
 * timers are measured against when MAME is the oracle): 1 for most register and
 * branch ops, 2 for stores, 4 for loads and compare-and-branch, 9 for a call, 7
 * for ret, up to 441 for the transcendental FP ops. Indexed by opcode byte;
 * REG majors 0x58..0x7F by (major, the 4-bit function in bits 7..10). Anything
 * MAME does not name costs 1. */
static uint16_t g_i960_cyc[256];
static uint16_t g_i960_cyc_reg[40 * 16];

static inline void i960_cycle_table_init(void) {
    if (g_i960_cyc[0]) return;
    for (int i = 0; i < 256; i++) g_i960_cyc[i] = 1;
    for (int i = 0; i < 40 * 16; i++) g_i960_cyc_reg[i] = 1;
    static const struct { uint8_t op; uint16_t c; } ops[] = {
        {0x09, 9}, {0x0A, 7}, {0x0B, 5},
        {0x30, 4}, {0x31, 4}, {0x32, 4}, {0x33, 4}, {0x34, 4}, {0x35, 4}, {0x36, 4}, {0x37, 4},
        {0x39, 4}, {0x3A, 4}, {0x3B, 4}, {0x3C, 4}, {0x3D, 4}, {0x3E, 4},
        {0x80, 4}, {0x82, 2}, {0x84, 3}, {0x85, 5}, {0x86, 9}, {0x88, 4}, {0x8A, 2}, {0x8C, 1},
        {0x90, 4}, {0x92, 2}, {0x98, 5}, {0x9A, 3}, {0xA0, 6}, {0xA2, 4}, {0xB0, 7}, {0xB2, 5},
        {0xC0, 4}, {0xC2, 2}, {0xC8, 4}, {0xCA, 2},
    };
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) g_i960_cyc[ops[i].op] = ops[i].c;
    static const struct { uint8_t op, fn; uint16_t c; } reg[] = {
        {0x58,0x0,2}, {0x58,0x3,2}, {0x58,0xC,2}, {0x58,0xE,2}, {0x58,0xF,2},
        {0x5A,0x4,2}, {0x5A,0x5,2}, {0x5A,0x6,2}, {0x5A,0x7,2}, {0x5A,0xC,2}, {0x5A,0xE,2},
        {0x5B,0x0,2}, {0x5B,0x2,2}, {0x5C,0xC,2}, {0x5D,0xC,2}, {0x5E,0xC,3}, {0x5F,0xC,4},
        {0x60,0x0,6}, {0x60,0x2,12}, {0x61,0x0,10}, {0x61,0x1,10},
        {0x64,0x0,10}, {0x64,0x1,10}, {0x64,0x4,7}, {0x64,0x5,10}, {0x65,0x5,10}, {0x66,0x0,9},
        {0x67,0x0,37}, {0x67,0x1,37}, {0x67,0x4,30}, {0x67,0x5,30}, {0x67,0x6,30}, {0x67,0x7,30},
        {0x68,0x0,267}, {0x68,0x1,400}, {0x68,0x2,438}, {0x68,0x3,67}, {0x68,0x5,10}, {0x68,0x8,104},
        {0x68,0x9,334}, {0x68,0xA,37}, {0x68,0xB,69}, {0x68,0xC,406}, {0x68,0xD,406}, {0x68,0xE,293},
        {0x69,0x0,350}, {0x69,0x2,438}, {0x69,0x5,12}, {0x69,0x8,104}, {0x69,0x9,334}, {0x69,0xA,37},
        {0x69,0xB,70}, {0x69,0xC,441}, {0x69,0xD,441}, {0x69,0xE,323},
        {0x6C,0x0,33}, {0x6C,0x1,35}, {0x6C,0x2,43}, {0x6C,0x3,44}, {0x6C,0x9,5}, {0x6D,0x9,6},
        {0x6E,0x1,8}, {0x6E,0x2,8}, {0x70,0x1,18}, {0x70,0x8,37}, {0x70,0xB,37},
        {0x74,0x1,18}, {0x74,0x8,37}, {0x74,0x9,37}, {0x74,0xB,37},
        {0x78,0xB,35}, {0x78,0xC,18}, {0x78,0xD,10}, {0x78,0xF,10},
        {0x79,0xB,77}, {0x79,0xC,36}, {0x79,0xD,13}, {0x79,0xF,13},
    };
    for (size_t i = 0; i < sizeof reg / sizeof reg[0]; i++)
        g_i960_cyc_reg[((reg[i].op - 0x58) << 4) | reg[i].fn] = reg[i].c;
}

static inline unsigned i960_cycle_cost(uint32_t word1) {
    uint32_t op = word1 >> 24;
    if (op - 0x58u < 0x28u) return g_i960_cyc_reg[((op - 0x58u) << 4) | ((word1 >> 7) & 0xFu)];
    return g_i960_cyc[op];
}

//--- Execute one instruction --------------------------------------------------

/* i960_step_hot is forced inline into the loops that run the game (the emu
 * thread's slice, the bench): as a call, the switch's callee-saved register
 * saves and restores around every instruction were ~7% of the emu thread on
 * the RK3566. Everything else calls i960_step, one out-of-line copy. */
#if defined(__GNUC__) || defined(__clang__)
#  define I960_HOT_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#  define I960_HOT_INLINE __forceinline
#else
#  define I960_HOT_INLINE inline
#endif

static I960_HOT_INLINE int i960_step_hot(i960_cpu_t *cpu, memory_bus_t *bus) {
    // Check HLE hooks before executing
    if (hle_check(cpu, bus) == 0) {
        return 0;  // hook handled it, IP already updated
    }

    uint32_t ip = cpu->sfr.ip;
    bus->cpu_ip = ip;
    uint32_t word1, word2;
    mem_fetch2(bus, ip, &word1, &word2);       // word2 read speculatively
    int instr_len = 4;
    /* Only the live timers read this, and the lookup is worth 2-4% of the emu
     * thread on the RK3566 — 6% through a game load — so it is charged only
     * while they are on. */
    if (g_irqt_live) cpu->cycles += i960_cycle_cost(word1);

    // Record in execution trace
    trace_record(ip, word1, cpu->frame_depth);

    uint32_t class = (word1 >> 28) & 0xF;

    switch (class) {
        //--------------------------------------------------------------
        // CTRL format (0x0, 0x1)
        //--------------------------------------------------------------
        case 0x0:
        case 0x1: {
            int opcode = CTRL_OPCODE(word1);

            // Extract displacement (24-bit signed)
            int32_t disp = word1 & 0x00FFFFFC;
            if (disp & 0x00800000) {
                disp |= (int32_t)0xFF000000;  // sign extend
            }

            switch (opcode) {
                case 0x08: // b - unconditional branch
                    cpu->sfr.ip = ip + disp;
                    return 0;

                case 0x09: // call
                {
                    // Save current frame
                    if (cpu->frame_depth < FRAME_STACK_DEPTH) {
                        cpu->frame_stack[cpu->frame_depth] = cpu->locals;
                        cpu->frame_irq[cpu->frame_depth] = 0;
                        cpu->frame_depth++;
                    } else {
                        LOG_ERROR("Frame stack overflow at 0x%08X", ip);
                        cpu->halted = 1;
                        return -1;
                    }

                    // New frame: pfp = old FP, rip = return address
                    uint32_t old_fp = cpu->locals.pfp;
                    uint32_t old_sp = cpu->locals.sp;
                    memset(&cpu->locals, 0, sizeof(local_regs_t));
                    cpu->locals.pfp = old_fp;
                    cpu->locals.sp = (old_sp + FRAME_ALIGN_MASK) & ~FRAME_ALIGN_MASK;  // align to 16-word boundary
                    cpu->locals.rip = ip + 4;               // return address

                    // Also save sp in g15/fp for frame tracking
                    cpu->globals.fp = cpu->locals.pfp;

                    cpu->sfr.ip = ip + disp;
                    return 0;
                }

                case 0x0A: // ret
                {
                    if (cpu->frame_depth > 0) {
                        uint32_t return_ip = cpu->locals.rip;
                        cpu->frame_depth--;
                        cpu->locals = cpu->frame_stack[cpu->frame_depth];
                        if (cpu->frame_irq[cpu->frame_depth]) {   /* interrupt return */
                            cpu->sfr.ac = cpu->frame_irq_ac[cpu->frame_depth];
                            cpu->sfr.pc = cpu->frame_irq_pc[cpu->frame_depth];
                            cpu->frame_irq[cpu->frame_depth] = 0;
                        }
                        cpu->sfr.ip = return_ip;
                        cpu->globals.fp = cpu->locals.pfp;
                        return 0;
                    } else {
                        LOG_WARN("ret with empty frame stack at 0x%08X", ip);
                        cpu->halted = 1;
                        return -1;
                    }
                }

                case 0x0B: // bal - branch and link
                    cpu->globals.g[14] = ip + 4;  // g14 = return address
                    cpu->sfr.ip = ip + disp;
                    return 0;

                // Conditional branches: bno bg be bge bl bne ble bo
                case 0x10: case 0x11: case 0x12: case 0x13:
                case 0x14: case 0x15: case 0x16: case 0x17:
                    if (i960_cond(get_cc(cpu), (uint32_t)opcode & 7u)) { cpu->sfr.ip = ip + disp; return 0; }
                    break;

                default:
                    LOG_WARN("UNIMPL CTRL opcode 0x%02X at 0x%08X", opcode, ip);
                    break;
            }
            instr_len = 4;
            break;
        }

        //--------------------------------------------------------------
        // COBR format (0x2, 0x3)
        //--------------------------------------------------------------
        case 0x2:
        case 0x3: {
            int opcode = COBR_OPCODE(word1);
            int src1_idx = COBR_SRC1(word1);
            int src2_idx = COBR_SRC2(word1);
            int m1 = COBR_M1(word1);

            uint32_t src1 = m1 ? (uint32_t)src1_idx : reg_read(cpu, src1_idx);
            uint32_t src2 = reg_read(cpu, src2_idx);

            // Extract displacement (13-bit signed, bits 2-12)
            int32_t disp = word1 & 0x00001FFC;
            if (disp & 0x00001000) {
                disp |= (int32_t)0xFFFFE000;
            }

            switch (opcode) {
                // test instructions (single operand, check CC): testno .. testo
                case 0x20: case 0x21: case 0x22: case 0x23:
                case 0x24: case 0x25: case 0x26: case 0x27:
                    reg_write(cpu, src1_idx, i960_cond(get_cc(cpu), (uint32_t)opcode & 7u) ? 1 : 0);
                    break;

                // Bit branch — per spec: CC_E if branch taken, CC_NO if not taken
                case 0x30: // bbc (branch if bit clear)
                    if (!(src2 & (1 << (src1 & 31)))) {
                        set_cc(cpu, CC_E);
                        cpu->sfr.ip = ip + disp; return 0;
                    }
                    set_cc(cpu, CC_NO);
                    break;
                case 0x37: // bbs (branch if bit set)
                    if (src2 & (1 << (src1 & 31))) {
                        set_cc(cpu, CC_E);
                        cpu->sfr.ip = ip + disp; return 0;
                    }
                    set_cc(cpu, CC_NO);
                    break;

                // Compare and branch, ordinal (cmpobg .. cmpoble) and integer
                // (cmpibg .. cmpible). The compare records its condition code
                // whether or not the branch is taken -- the manual's "updates
                // the condition code register, then branches" -- and a `bg` /
                // `bl` / `be` that follows reads it. STF's get_start_value
                // opcode dispatcher is one that does:
                //   cmpobe 4, r6, loc_30B00    ; sets CC, branches if r6==4
                //   bg     loc_30AE8           ; uses CC, branches if r6<4
                // With a stale CC the wrong path is taken for opcodes 0..3 and
                // get_start_value reads unrelated bytes from g2.
                case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36:
                    set_cc(cpu, i960_cmp_cc_o(src1, src2));
                    if (i960_cond(get_cc(cpu), (uint32_t)opcode & 7u)) { cpu->sfr.ip = ip + disp; return 0; }
                    break;
                case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e:
                    set_cc(cpu, i960_cmp_cc_i((int32_t)src1, (int32_t)src2));
                    if (i960_cond(get_cc(cpu), (uint32_t)opcode & 7u)) { cpu->sfr.ip = ip + disp; return 0; }
                    break;
                // cmpibno never branches and cmpibo always does. Neither records
                // the compare here (as before; no STF code uses either).
                case 0x38: // cmpibno
                    break;
                case 0x3f: // cmpibo
                    cpu->sfr.ip = ip + disp; return 0;

                default:
                    LOG_WARN("UNIMPL COBR opcode 0x%02X at 0x%08X", opcode, ip);
                    break;
            }
            instr_len = 4;
            break;
        }

        //--------------------------------------------------------------
        // REG format (0x5, 0x6, 0x7)
        //--------------------------------------------------------------
        case 0x5:
        case 0x6:
        case 0x7: {
            int opcode = REG_OPCODE(word1);
            int src1_idx = REG_SRC1(word1);
            int src2_idx = REG_SRC2(word1);
            int dst_idx  = REG_DST(word1);
            int m1 = REG_M1(word1);
            int m2 = REG_M2(word1);
            int m3 = REG_M3(word1);

            uint32_t src1 = reg_src(cpu, src1_idx, m1);
            uint32_t src2 = reg_src(cpu, src2_idx, m2);

            switch (opcode) {
                // Logic
                case 0x581: // and
                    reg_write(cpu, dst_idx, src1 & src2);
                    break;
                case 0x587: // or
                    reg_write(cpu, dst_idx, src1 | src2);
                    break;
                case 0x586: // xor
                    reg_write(cpu, dst_idx, src1 ^ src2);
                    break;
                case 0x58a: // not
                    reg_write(cpu, dst_idx, ~src1);
                    break;

                // Arithmetic (ordinal / unsigned)
                case 0x590: // addo
                    reg_write(cpu, dst_idx, src2 + src1);
                    break;
                case 0x592: // subo  (dst = src2 - src1)
                    reg_write(cpu, dst_idx, src2 - src1);
                    break;

                // Arithmetic (integer / signed)
                case 0x591: // addi
                    reg_write(cpu, dst_idx, (uint32_t)((int32_t)src2 + (int32_t)src1));
                    break;
                case 0x593: // subi
                    reg_write(cpu, dst_idx, (uint32_t)((int32_t)src2 - (int32_t)src1));
                    break;

                // Shifts
                case 0x598: // shro (shift right ordinal)
                    reg_write(cpu, dst_idx, (src1 < 32) ? (src2 >> src1) : 0);
                    break;
                case 0x59a: // shrdi (shift right double integer)
                {
                    // 64-bit shift right: concatenate dst:src2, shift by src1, result in dst
                    uint64_t wide = ((uint64_t)reg_read(cpu, dst_idx) << 32) | src2;
                    uint32_t shift = src1 & 63;
                    reg_write(cpu, dst_idx, (uint32_t)(wide >> shift));
                    break;
                }
                case 0x59b: // shri (shift right integer / arithmetic)
                    reg_write(cpu, dst_idx, (src1 < 32) ? (uint32_t)((int32_t)src2 >> src1) : ((int32_t)src2 < 0 ? 0xFFFFFFFF : 0));
                    break;
                case 0x59c: // shlo (shift left ordinal)
                    reg_write(cpu, dst_idx, (src1 < 32) ? (src2 << src1) : 0);
                    break;
                case 0x59d: // rotate
                {
                    uint32_t shift = src1 & 31;
                    uint32_t result = shift ? ((src2 << shift) | (src2 >> (32 - shift))) : src2;
                    reg_write(cpu, dst_idx, result);
                    break;
                }
                case 0x59e: // shli (shift left integer)
                    reg_write(cpu, dst_idx, (src1 < 32) ? (src2 << src1) : 0);
                    break;

                // Compare
                case 0x5a0: // cmpo (compare ordinal)
                    set_cc(cpu, i960_cmp_cc_o(src1, src2));
                    break;
                case 0x5a1: // cmpi (compare integer)
                    set_cc(cpu, i960_cmp_cc_i((int32_t)src1, (int32_t)src2));
                    break;
                case 0x5a2: // concmpo (conditional compare ordinal)
                    // Compares only when condition-code bit 2 (less, 0b100) is
                    // clear, so `cmpi x, lo; concmpi x, hi` is lo <= x <= hi. It
                    // used to test the equal bit, which let every x below lo
                    // through as in range.
                    if (!(get_cc(cpu) & CC_L)) {
                        if (src1 <= src2) set_cc(cpu, CC_E);
                        else              set_cc(cpu, CC_G);
                    }
                    break;
                case 0x5a3: // concmpi (conditional compare integer)
                    if (!(get_cc(cpu) & CC_L)) {
                        if ((int32_t)src1 <= (int32_t)src2) set_cc(cpu, CC_E);
                        else                                 set_cc(cpu, CC_G);
                    }
                    break;

                // Compare and increment/decrement
                case 0x5a4: // cmpinco (compare, increment ordinal)
                    set_cc(cpu, i960_cmp_cc_o(src1, src2));
                    reg_write(cpu, dst_idx, src2 + 1);
                    break;
                case 0x5a5: // cmpinci (compare, increment integer)
                    set_cc(cpu, i960_cmp_cc_i((int32_t)src1, (int32_t)src2));
                    reg_write(cpu, dst_idx, (uint32_t)((int32_t)src2 + 1));
                    break;
                case 0x5a6: // cmpdeco (compare, decrement ordinal)
                    set_cc(cpu, i960_cmp_cc_o(src1, src2));
                    reg_write(cpu, dst_idx, src2 - 1);
                    break;
                case 0x5a7: // cmpdeci (compare, decrement integer)
                    set_cc(cpu, i960_cmp_cc_i((int32_t)src1, (int32_t)src2));
                    reg_write(cpu, dst_idx, (uint32_t)((int32_t)src2 - 1));
                    break;

                case 0x5ae: // chkbit
                    if (src2 & (1 << (src1 & 31))) set_cc(cpu, CC_E);
                    else                            set_cc(cpu, CC_NO);
                    break;

                case 0x5ac: // scanbyte — CC_E if any byte matches, CC_NO if none
                {
                    uint32_t found = 0;
                    for (int b = 0; b < 4; b++) {
                        if (((src1 >> (b*8)) & 0xFF) == ((src2 >> (b*8)) & 0xFF)) {
                            found = 1;
                            break;
                        }
                    }
                    set_cc(cpu, found ? CC_E : CC_NO);
                    break;
                }

                case 0x5ad: // bswap
                    reg_write(cpu, dst_idx,
                        ((src1 >> 24) & 0xFF) |
                        ((src1 >> 8) & 0xFF00) |
                        ((src1 << 8) & 0xFF0000) |
                        ((src1 << 24) & 0xFF000000));
                    break;

                case 0x5b0: // addc (add with carry)
                {
                    uint64_t result = (uint64_t)src2 + (uint64_t)src1 + (get_cc(cpu) & 0x2 ? 1 : 0);
                    reg_write(cpu, dst_idx, (uint32_t)result);
                    set_cc(cpu, (result >> 32) ? CC_E : CC_NO);
                    break;
                }
                case 0x5b2: // subc (subtract with carry)
                {
                    uint64_t result = (uint64_t)src2 - (uint64_t)src1 - 1 + (get_cc(cpu) & 0x2 ? 1 : 0);
                    reg_write(cpu, dst_idx, (uint32_t)result);
                    set_cc(cpu, (result >> 32) ? CC_NO : CC_E);
                    break;
                }

                // Bit operations
                case 0x580: // notbit
                    reg_write(cpu, dst_idx, src2 ^ (1 << (src1 & 31)));
                    break;
                case 0x583: // setbit
                    reg_write(cpu, dst_idx, src2 | (1 << (src1 & 31)));
                    break;
                /* The four "not" logicals are not two pairs of synonyms: which
                 * operand is inverted is the whole difference. andnot / ornot
                 * invert src1, notand / notor invert src2 (MAME i960.cpp,
                 * Intel's reference). notand used to be a copy of andnot, which
                 * sent every STF mip level to an odd texram address —
                 * sub_4C444 builds the chain's destinations with
                 * `notand g6, 1, g6` to clear bit 0 — so the top quarter of
                 * both sheets was never filled. */
                case 0x584: // notand
                    reg_write(cpu, dst_idx, src1 & (~src2));
                    break;
                case 0x582: // andnot
                    reg_write(cpu, dst_idx, (~src1) & src2);
                    break;
                case 0x588: // nor
                    reg_write(cpu, dst_idx, ~(src1 | src2));
                    break;
                case 0x589: // xnor
                    reg_write(cpu, dst_idx, ~(src1 ^ src2));
                    break;
                case 0x58b: // ornot
                    reg_write(cpu, dst_idx, src2 | (~src1));
                    break;
                case 0x58c: // clrbit
                    reg_write(cpu, dst_idx, src2 & ~(1 << (src1 & 31)));
                    break;
                case 0x58d: // notor
                    reg_write(cpu, dst_idx, src1 | (~src2));
                    break;
                case 0x58e: // nand
                    reg_write(cpu, dst_idx, ~(src1 & src2));
                    break;
                case 0x58f: // alterbit
                    if (get_cc(cpu) & CC_E)
                        reg_write(cpu, dst_idx, src2 | (1 << (src1 & 31)));
                    else
                        reg_write(cpu, dst_idx, src2 & ~(1 << (src1 & 31)));
                    break;

                // Multiply / Divide
                case 0x701: // mulo
                    reg_write(cpu, dst_idx, src2 * src1);
                    break;
                case 0x741: // muli
                    reg_write(cpu, dst_idx, (uint32_t)((int32_t)src2 * (int32_t)src1));
                    break;
                case 0x70b: // divo
                    if (src1 != 0) reg_write(cpu, dst_idx, src2 / src1);
                    else LOG_ERROR("divo: divide by zero at 0x%08X", ip);
                    break;
                case 0x74b: // divi
                    if (src1 != 0) reg_write(cpu, dst_idx, (uint32_t)((int32_t)src2 / (int32_t)src1));
                    else LOG_ERROR("divi: divide by zero at 0x%08X", ip);
                    break;
                case 0x708: // remo
                    if (src1 != 0) reg_write(cpu, dst_idx, src2 % src1);
                    else LOG_ERROR("remo: divide by zero at 0x%08X", ip);
                    break;
                case 0x748: // remi
                    if (src1 != 0) reg_write(cpu, dst_idx, (uint32_t)((int32_t)src2 % (int32_t)src1));
                    else LOG_ERROR("remi: divide by zero at 0x%08X", ip);
                    break;
                case 0x749: // modi
                {
                    if (src1 != 0) {
                        int32_t s1 = (int32_t)src1;
                        int32_t s2 = (int32_t)src2;
                        int32_t r = s2 % s1;
                        // modi returns result with same sign as divisor
                        if (r != 0 && ((r ^ s1) < 0)) r += s1;
                        reg_write(cpu, dst_idx, (uint32_t)r);
                    } else {
                        LOG_ERROR("modi: divide by zero at 0x%08X", ip);
                    }
                    break;
                }

                case 0x670: // emul (extended multiply)
                {
                    uint64_t result = (uint64_t)src2 * (uint64_t)src1;
                    reg_write(cpu, dst_idx, (uint32_t)(result & 0xFFFFFFFF));
                    reg_write(cpu, dst_idx + 1, (uint32_t)(result >> 32));
                    break;
                }
                case 0x671: // ediv (extended divide)
                {
                    if (src1 != 0) {
                        uint64_t dividend = ((uint64_t)reg_read(cpu, src2_idx + 1) << 32) | reg_read(cpu, src2_idx);
                        reg_write(cpu, dst_idx, (uint32_t)(dividend % src1));
                        reg_write(cpu, dst_idx + 1, (uint32_t)(dividend / src1));
                    } else {
                        LOG_ERROR("ediv: divide by zero at 0x%08X", ip);
                    }
                    break;
                }

                // Move variants
                case 0x5cc: // mov
                    reg_write(cpu, dst_idx, src1);
                    break;
                /* movl / movt / movq take a literal source too, and a literal
                 * fills every destination register (MAME i960.cpp). Reading
                 * registers for `movq 0, r4` copied pfp/sp/rip/r3 instead of
                 * zeros: STF's adv_set_action clears each fighter's damage and
                 * crush-stage arrays that way, the garbage stages sent
                 * damage_unit into the Fighting Vipers armour-break code, and it
                 * wrote model numbers like 0x3000 over the forearms and shins
                 * until set_obj stopped the game ("max poly / err poly"). */
                case 0x5dc: // movl (move long, 2 regs)
                case 0x5ec: // movt (move triple, 3 regs)
                case 0x5fc: // movq (move quad, 4 regs)
                {
                    int nreg = opcode == 0x5dc ? 2 : opcode == 0x5ec ? 3 : 4;
                    uint32_t v[4];
                    for (int k = 0; k < nreg; k++) v[k] = m1 ? src1 : reg_read(cpu, src1_idx + k);
                    for (int k = 0; k < nreg; k++) reg_write(cpu, dst_idx + k, v[k]);
                    break;
                }

                // Synchronized moves
                // synmov encoding: src1 (bits 0-4) = dst addr reg, src2 (bits 14-18) = src addr reg
                case 0x600: // synmov (synchronized move, 2 words)
                {
                    uint32_t dst_addr = reg_src(cpu, src1_idx, m1);
                    uint32_t src_addr = reg_src(cpu, src2_idx, m2);
                    mem_write32(bus, dst_addr, mem_read32(bus, src_addr));
                    mem_write32(bus, dst_addr + 4, mem_read32(bus, src_addr + 4));
                    break;
                }
                case 0x601: // synmovl (synchronized move long, 4 words)
                {
                    uint32_t dst_addr = reg_src(cpu, src1_idx, m1);
                    uint32_t src_addr = reg_src(cpu, src2_idx, m2);
                    for (int i = 0; i < 4; i++)
                        mem_write32(bus, dst_addr + i*4, mem_read32(bus, src_addr + i*4));
                    break;
                }
                case 0x602: // synmovq (synchronized move quad, 4 words)
                {
                    uint32_t dst_addr = reg_src(cpu, src1_idx, m1);
                    uint32_t src_addr = reg_src(cpu, src2_idx, m2);

                    // Check for IAC message port
                    if (dst_addr == IAC_MSG_PORT) {
                        uint32_t msg_type = mem_read32(bus, src_addr) >> 24;
                        switch (msg_type) {
                            case IAC_REINITIALIZE:
                            {
                                uint32_t new_sat  = mem_read32(bus, src_addr + 0x04);
                                uint32_t new_prcb = mem_read32(bus, src_addr + 0x08);
                                uint32_t new_ip   = mem_read32(bus, src_addr + 0x0C);
                                LOG_INFO("IAC Reinitialize: SAT=0x%08X PRCB=0x%08X IP=0x%08X",
                                         new_sat, new_prcb, new_ip);

                                memset(&cpu->locals, 0, sizeof(local_regs_t));
                                cpu->frame_depth = 0;

                                uint32_t new_isp = mem_read32(bus, new_prcb + PRCB_INTR_STACK);
                                if (new_isp) {
                                    cpu->locals.sp = new_isp;
                                }

                                cpu->sfr.ip = new_ip;
                                LOG_INFO("Reinitializing to IP=0x%08X", new_ip);
                                return 0;
                            }
                            case IAC_PURGE_CACHE:
                                LOG_DEBUG("IAC Purge instruction cache (noop)");
                                break;
                            default:
                                LOG_WARN("IAC unknown message type 0x%02X at 0x%08X", msg_type, cpu->sfr.ip);
                                break;
                        }
                    } else {
                        for (int i = 0; i < 4; i++)
                            mem_write32(bus, dst_addr + i*4, mem_read32(bus, src_addr + i*4));
                    }
                    break;
                }

                // Misc
                case 0x5b4: // intdis (disable interrupts)
                    cpu->sfr.pc &= ~0x2000;  // clear interrupt enable bit
                    break;
                case 0x5b5: // inten (enable interrupts)
                    cpu->sfr.pc |= 0x2000;
                    break;
                case 0x66d: // flushreg
                    // Flush register cache to memory - noop for us
                    break;
                case 0x66f: // syncf
                    // Synchronize faults - noop for us
                    break;
                case 0x640: // spanbit (scan for first clear bit from MSB)
                {
                    if (src1 == 0xFFFFFFFF) {
                        reg_write(cpu, dst_idx, 0xFFFFFFFF);
                        set_cc(cpu, CC_NO);
                    } else {
                        uint32_t v = ~src1;
                        int pos = 31;
                        while (pos >= 0 && !(v & (1u << pos))) pos--;
                        reg_write(cpu, dst_idx, (uint32_t)pos);
                        set_cc(cpu, CC_E);
                    }
                    break;
                }
                case 0x641: // scanbit (scan for first set bit from MSB)
                {
                    if (src1 == 0) {
                        reg_write(cpu, dst_idx, 0xFFFFFFFF);
                        set_cc(cpu, CC_NO);
                    } else {
                        int pos = 31;
                        while (pos >= 0 && !(src1 & (1u << pos))) pos--;
                        reg_write(cpu, dst_idx, (uint32_t)pos);
                        set_cc(cpu, CC_E);
                    }
                    break;
                }
                case 0x645: // modac (modify arithmetic controls)
                {
                    uint32_t ac = cpu->sfr.ac;
                    reg_write(cpu, dst_idx, ac);
                    cpu->sfr.ac = (ac & ~src1) | (src2 & src1);
                    break;
                }
                case 0x650: // modify
                    reg_write(cpu, dst_idx, (src2 & src1) | (reg_read(cpu, dst_idx) & ~src1));
                    break;
                case 0x651: // extract
                    reg_write(cpu, dst_idx, (reg_read(cpu, dst_idx) & src1) | (src2 & ~src1));
                    break;
                case 0x654: // modtc (modify trace controls)
                {
                    uint32_t tc = cpu->sfr.tc;
                    reg_write(cpu, dst_idx, tc);
                    cpu->sfr.tc = (tc & ~src1) | (src2 & src1);
                    break;
                }
                case 0x655: // modpc (modify process controls)
                {
                    uint32_t pc = cpu->sfr.pc;
                    reg_write(cpu, dst_idx, pc);
                    cpu->sfr.pc = (pc & ~src1) | (src2 & src1);
                    break;
                }
                case 0x673: // ldtime
                    reg_write(cpu, dst_idx, 0);  // stub: return 0 for now
                    break;

                //--------------------------------------------------------------
                // Floating point instructions
                // FP regs: m1/m2=1 means FP register (idx 0-3 = fp0-fp3)
                // When m1/m2=0, the operand is a GENERAL-PURPOSE register but
                // its 32-bit contents are the IEEE-754 bit pattern of a float
                // (NOT an integer count). The macros below correctly bit-cast
                // the integer-register's 32-bit value into a float, then widen
                // to double for the operation. This matches real i960 KB FPU
                // semantics — `addr g0, g1, g2` means "treat g0/g1/g2's 32-bit
                // contents as IEEE floats", not "convert g0/g1's int values to
                // floats and store an int result in g2".
                //
                // The exception: cvtir / cvtilr explicitly READ an integer
                // register as an integer (their whole purpose), and cvtri /
                // cvtril / cvtzri / cvtzril explicitly WRITE an integer
                // register as an integer. Those instructions handle the
                // integer-register access manually below.
                //--------------------------------------------------------------

                // Bit-cast helpers — read int register as float, write float to int register.
                // Uses memcpy for strict-aliasing safety. Modern compilers
                // optimize memcpy of small fixed-size buffers to a register move.
                #define INT_REG_AS_DOUBLE(idx) i960_int_reg_as_double(cpu, (idx))
                #define WRITE_FLOAT_TO_INT_REG(idx, dval) \
                    reg_write(cpu, (idx), i960_double_to_single(dval))

                // In FP-register mode (m=1) the 5-bit operand also encodes the
                // i960 floating-point LITERALS: 16 (0b10000) = +0.0, 22 (0b10110)
                // = +1.0. gcc960 emits these as `0f0.0`/`0f1.0` (e.g. `subr g,0f1.0,g`
                // for `1.0 - x`). Without this, every 1.0 constant read as fp_regs[2]
                // ≈ 0, breaking all `1.0 - x` math (zoom/camera/view collapse).
                #define FP_LIT_OR_REG(idx) ((idx) == 22 ? 1.0 : (idx) == 16 ? 0.0 \
                                                              : cpu->fp_regs[(idx) & 3])
                #define FP_SRC1 (m1 ? FP_LIT_OR_REG(src1_idx) : INT_REG_AS_DOUBLE(src1_idx))
                #define FP_SRC2 (m2 ? FP_LIT_OR_REG(src2_idx) : INT_REG_AS_DOUBLE(src2_idx))
                #define FP_DST_WRITE(val) do { \
                    if (m3) cpu->fp_regs[dst_idx & 3] = (val); \
                    else WRITE_FLOAT_TO_INT_REG(dst_idx, (val)); \
                } while(0)
                #define FP_DST_WRITE_INT(val) reg_write(cpu, dst_idx, (val))
                
                // Convert integer to real (source is INTEGER, dest is FLOAT)
                case 0x674: // cvtir
                {
                    /* Source: read as a true integer regardless of register
                       type. For integer regs, this is just the raw bits. For
                       FP regs (m1=1), reading them as int doesn't really make
                       sense, but we honor the m1 bit anyway. */
                    int32_t i = m1 ? (int32_t)i960_real_to_int32(cpu->fp_regs[src1_idx & 3])
                                   : (int32_t)reg_read(cpu, src1_idx);
                    double v = (double)i;
                    FP_DST_WRITE(v);
                    break;
                }
                case 0x675: // cvtilr
                {
                    int32_t i = m1 ? (int32_t)i960_real_to_int32(cpu->fp_regs[src1_idx & 3])
                                   : (int32_t)reg_read(cpu, src1_idx);
                    double v = (double)i;
                    FP_DST_WRITE(v);
                    break;
                }

                // Convert real to integer (source is FLOAT, dest is INTEGER).
                // cvtri/cvtril round by the AC rounding mode (bits 30-31; 0 at
                // reset = nearest). Only the z forms truncate. Truncating here
                // made every angle STF converts from radians come out one brad
                // low against a MAME capture of the same frame.
                case 0x6C0: // cvtri
                {
                    double v = FP_SRC1;  // correctly bit-cast from int reg
                    FP_DST_WRITE_INT(i960_real_to_int32(i960_round_ac(cpu, v)));
                    break;
                }
                case 0x6C1: // cvtril
                {
                    double v = FP_SRC1;
                    FP_DST_WRITE_INT(i960_real_to_int32(i960_round_ac(cpu, v)));
                    break;
                }
                case 0x6C2: // cvtzri (truncate toward zero)
                {
                    double v = FP_SRC1;
                    FP_DST_WRITE_INT(i960_real_to_int32(v));
                    break;
                }
                case 0x6C3: // cvtzril
                {
                    double v = FP_SRC1;
                    FP_DST_WRITE_INT(i960_real_to_int32(v));
                    break;
                }

                // Move
                case 0x6C9: // fmovr
                {
                    double v = FP_SRC1;
                    FP_DST_WRITE(v);
                    break;
                }

                // Arithmetic
                case 0x78F: // faddr
                {
                    double a = FP_SRC1;
                    double b = FP_SRC2;
                    FP_DST_WRITE(i960_nan_result(a + b, a, b));
                    break;
                }
                case 0x78D: // fsubr
                {
                    double a = FP_SRC1;
                    double b = FP_SRC2;
                    FP_DST_WRITE(i960_nan_result(b - a, b, a));
                    break;
                }
                case 0x78C: // fmulr
                {
                    double a = FP_SRC1;
                    double b = FP_SRC2;
                    FP_DST_WRITE(i960_nan_result(a * b, a, b));
                    break;
                }
                case 0x78B: // fdivr
                {
                    double a = FP_SRC1;
                    double b = FP_SRC2;
                    if (a != 0.0) {
                        FP_DST_WRITE(i960_nan_result(b / a, b, a));
                    } else {
                        // Divide by zero — produce 0.0 silently, happens often with uninitialized geometry
                        FP_DST_WRITE(0.0);
                    }
                    break;
                }

                // Compare
                case 0x684: // fcmpor (compare ordered real)
                case 0x685: // fcmpr
                {
                    double a = FP_SRC1;
                    double b = FP_SRC2;
                    if (a < b)       set_cc(cpu, CC_L);
                    else if (a == b) set_cc(cpu, CC_E);
                    else             set_cc(cpu, CC_G);
                    break;
                }

                // Sqrt, trig, etc. (stubs using math.h)
                case 0x688: // fsqrtr
                {
                    double v = FP_SRC1;
                    FP_DST_WRITE(v >= 0.0 ? sqrt(v) : 0.0);
                    break;
                }

                #undef FP_SRC1
                #undef FP_SRC2
                #undef FP_DST_WRITE
                #undef FP_DST_WRITE_INT

                default:
                    LOG_WARN("UNIMPL REG opcode 0x%03X at 0x%08X", opcode, ip);
                    break;
            }
            instr_len = 4;
            break;
        }

        //--------------------------------------------------------------
        // MEM format (0x8, 0x9, 0xA, 0xB, 0xC)
        //--------------------------------------------------------------
        case 0x8:
        case 0x9:
        case 0xA:
        case 0xB:
        case 0xC: {
            int opcode = (word1 >> 24) & 0xFF;
            int dst_idx = MEM_SRCDST(word1);
            uint32_t ea = mem_ea(cpu, word1, word2, &instr_len);

            switch (opcode) {
                case 0x80: // ldob (load ordinal byte)
                    reg_write(cpu, dst_idx, mem_read8(bus, ea));
                    break;
                case 0x82: // stob (store ordinal byte)
                    g_last_store_ip = cpu->sfr.ip;
                    mem_write8(bus, ea, (uint8_t)reg_read(cpu, dst_idx));
                    break;
                case 0x88: // ldos (load ordinal short)
                    reg_write(cpu, dst_idx, mem_read16(bus, ea));
                    break;
                case 0x8a: // stos (store ordinal short)
                    g_last_store_ip = cpu->sfr.ip;
                    mem_write16(bus, ea, (uint16_t)reg_read(cpu, dst_idx));
                    break;
                case 0x8C: // lda (load address)
                    reg_write(cpu, dst_idx, ea);
                    break;
                case 0x90: // ld (load)
                    reg_write(cpu, dst_idx, mem_read32(bus, ea));
                    break;
                case 0x92: // st (store)
                    g_last_store_ip = cpu->sfr.ip;
                    mem_write32(bus, ea, reg_read(cpu, dst_idx));
                    break;
                // Multi-word loads/stores walk +4 a word only on burst devices;
                // on MMIO every word hits the same address (see mem_burst_step).
                case 0x98: // ldl (load long - 2 regs)
                case 0xA0: // ldt (load triple - 3 regs)
                case 0xB0: // ldq (load quad - 4 regs)
                {
                    int n = opcode == 0x98 ? 2 : opcode == 0xA0 ? 3 : 4;
                    uint32_t a = ea;
                    for (int k = 0; k < n; k++) {
                        reg_write(cpu, dst_idx + k, mem_read32(bus, a));
                        a += mem_burst_step(bus, a);
                    }
                    break;
                }
                case 0x9a: // stl (store long - 2 regs)
                case 0xA2: // stt (store triple - 3 regs)
                case 0xB2: // stq (store quad - 4 regs)
                {
                    g_last_store_ip = cpu->sfr.ip;
                    int n = opcode == 0x9a ? 2 : opcode == 0xA2 ? 3 : 4;
                    uint32_t a = ea;
                    for (int k = 0; k < n; k++) {
                        mem_write32(bus, a, reg_read(cpu, dst_idx + k));
                        a += mem_burst_step(bus, a);
                    }
                    break;
                }
                case 0xC0: // ldib (load integer byte, sign extend)
                    reg_write(cpu, dst_idx, (uint32_t)(int32_t)(int8_t)mem_read8(bus, ea));
                    break;
                case 0xC2: // stib (store integer byte)
                    mem_write8(bus, ea, (uint8_t)reg_read(cpu, dst_idx));
                    break;
                case 0xC8: // ldis (load integer short, sign extend)
                    reg_write(cpu, dst_idx, (uint32_t)(int32_t)(int16_t)mem_read16(bus, ea));
                    break;
                case 0xCA: // stis (store integer short)
                    mem_write16(bus, ea, (uint16_t)reg_read(cpu, dst_idx));
                    break;

                case 0x84: // bx (branch indirect)
                    cpu->sfr.ip = ea;
                    return 0;
                case 0x85: // balx (branch and link indirect)
                    reg_write(cpu, dst_idx, ip + instr_len);
                    cpu->sfr.ip = ea;
                    return 0;
                case 0x86: // callx (call indirect)
                {
                    if (cpu->frame_depth < FRAME_STACK_DEPTH) {
                        cpu->frame_stack[cpu->frame_depth] = cpu->locals;
                        cpu->frame_irq[cpu->frame_depth] = 0;
                        cpu->frame_depth++;
                    } else {
                        LOG_ERROR("Frame stack overflow at 0x%08X", ip);
                        cpu->halted = 1;
                        return -1;
                    }
                    uint32_t old_sp = cpu->locals.sp;
                    uint32_t old_pfp = cpu->locals.pfp;
                    memset(&cpu->locals, 0, sizeof(local_regs_t));
                    cpu->locals.pfp = old_pfp;
                    cpu->locals.sp = (old_sp + FRAME_ALIGN_MASK) & ~FRAME_ALIGN_MASK;
                    cpu->locals.rip = ip + instr_len;
                    cpu->globals.fp = cpu->locals.pfp;
                    cpu->sfr.ip = ea;
                    return 0;
                }

                default:
                    LOG_WARN("UNIMPL MEM opcode 0x%02X at 0x%08X", opcode, ip);
                    break;
            }
            break;
        }

        default:
            LOG_ERROR("Unknown instruction class 0x%X at IP=0x%08X (word=0x%08X)", class, ip, word1);
            cpu->halted = 1;
            return -1;
    }

    cpu->sfr.ip = ip + instr_len;
    return 0;
}

static inline int i960_step(i960_cpu_t *cpu, memory_bus_t *bus) {
    return i960_step_hot(cpu, bus);
}

#endif // I960_EXEC_H
