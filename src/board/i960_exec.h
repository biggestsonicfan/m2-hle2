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

static inline uint32_t reg_read(i960_cpu_t *cpu, int idx) {
    if (idx < 16) return cpu->locals.r[idx];
    if (idx < 32) return cpu->globals.g[idx - 16];
    return 0;
}

static inline void reg_write(i960_cpu_t *cpu, int idx, uint32_t val) {
    if (idx < 16) { cpu->locals.r[idx] = val; return; }
    if (idx < 32) { cpu->globals.g[idx - 16] = val; return; }
}

// REG format: operand value, respecting literal mode bit
static inline uint32_t reg_src(i960_cpu_t *cpu, int idx, int mode) {
    if (mode) return (uint32_t)idx;  // literal
    return reg_read(cpu, idx);
}

// Read an integer register's contents as a double, treating the 32 bits as
// the IEEE-754 binary32 (float) bit pattern. Used by FP instructions whose
// source operand is in a general-purpose register — such operations interpret
// those bits as a float, NOT as an integer count to be converted.
static inline double i960_int_reg_as_double(i960_cpu_t *cpu, int idx) {
    uint32_t u = reg_read(cpu, idx);
    float f;
    memcpy(&f, &u, 4);
    return (double)f;
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

//--- Execute one instruction --------------------------------------------------

static inline int i960_step(i960_cpu_t *cpu, memory_bus_t *bus) {
    // Check HLE hooks before executing
    if (hle_check(cpu, bus) == 0) {
        return 0;  // hook handled it, IP already updated
    }

    uint32_t ip = cpu->sfr.ip;
    bus->cpu_ip = ip;
    uint32_t word1 = mem_read32(bus, ip);
    uint32_t word2 = mem_read32(bus, ip + 4);  // read speculatively
    int instr_len = 4;

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

                // Conditional branches
                case 0x10: // bno
                    if (get_cc(cpu) == CC_NO) { cpu->sfr.ip = ip + disp; return 0; }
                    break;
                case 0x11: // bg
                    if (get_cc(cpu) & CC_G) { cpu->sfr.ip = ip + disp; return 0; }
                    break;
                case 0x12: // be
                    if (get_cc(cpu) & CC_E) { cpu->sfr.ip = ip + disp; return 0; }
                    break;
                case 0x13: // bge
                    if (get_cc(cpu) & CC_G || get_cc(cpu) & CC_E) { cpu->sfr.ip = ip + disp; return 0; }
                    break;
                case 0x14: // bl
                    if (get_cc(cpu) & CC_L) { cpu->sfr.ip = ip + disp; return 0; }
                    break;
                case 0x15: // bne — mask 101: branch if G or L (not if NO or E)
                    if (get_cc(cpu) & (CC_G | CC_L)) { cpu->sfr.ip = ip + disp; return 0; }
                    break;
                case 0x16: // ble
                    if (get_cc(cpu) & CC_L || get_cc(cpu) & CC_E) { cpu->sfr.ip = ip + disp; return 0; }
                    break;
                case 0x17: // bo — mask 111: branch if any condition bit set
                    if (get_cc(cpu) != 0) { cpu->sfr.ip = ip + disp; return 0; }
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
                // test instructions (single operand, check CC)
                case 0x20: // testno
                    reg_write(cpu, src1_idx, (get_cc(cpu) == CC_NO) ? 1 : 0); break;
                case 0x21: // testg
                    reg_write(cpu, src1_idx, (get_cc(cpu) & CC_G) ? 1 : 0); break;
                case 0x22: // teste
                    reg_write(cpu, src1_idx, (get_cc(cpu) & CC_E) ? 1 : 0); break;
                case 0x23: // testge
                    reg_write(cpu, src1_idx, (get_cc(cpu) & (CC_G|CC_E)) ? 1 : 0); break;
                case 0x24: // testl
                    reg_write(cpu, src1_idx, (get_cc(cpu) & CC_L) ? 1 : 0); break;
                case 0x25: // testne — mask 101: set if G or L
                    reg_write(cpu, src1_idx, (get_cc(cpu) & (CC_G | CC_L)) ? 1 : 0); break;
                case 0x26: // testle
                    reg_write(cpu, src1_idx, (get_cc(cpu) & (CC_L|CC_E)) ? 1 : 0); break;
                case 0x27: // testo — mask 111: set if any condition bit set
                    reg_write(cpu, src1_idx, (get_cc(cpu) != 0) ? 1 : 0); break;

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

                // Compare and branch (ordinal / unsigned)
                // CRITICAL: cmpobX instructions DO set the condition codes on
                // real i960 hardware, NOT just branch. The CPU manual states:
                // "Performs a comparison... and updates the condition code
                // register, then branches if the condition is satisfied."
                // A subsequent `bg`/`bl`/`be` after a `cmpobe` uses these
                // updated CC bits — without setting them, those branches use
                // STALE CC values from earlier instructions and produce wrong
                // control flow.
                //
                // Get_start_value's opcode dispatcher (in Sonic the Fighters)
                // is one example of code that depends on this:
                //   cmpobe 4, r6, loc_30B00    ; sets CC, branches if r6==4
                //   bg     loc_30AE8           ; uses CC, branches if r6<4
                // Without the CC update, the bg uses stale CC and the wrong
                // path is taken for opcodes 0..3, causing get_start_value to
                // read unrelated bytes from g2 instead of writing 0 to scratch.
                case 0x31: // cmpobg
                    if (src1 < src2)       set_cc(cpu, CC_L);
                    else if (src1 == src2) set_cc(cpu, CC_E);
                    else                    set_cc(cpu, CC_G);
                    if (src1 > src2) { cpu->sfr.ip = ip + disp; return 0; } break;
                case 0x32: // cmpobe
                    if (src1 < src2)       set_cc(cpu, CC_L);
                    else if (src1 == src2) set_cc(cpu, CC_E);
                    else                    set_cc(cpu, CC_G);
                    if (src1 == src2) { cpu->sfr.ip = ip + disp; return 0; } break;
                case 0x33: // cmpobge
                    if (src1 < src2)       set_cc(cpu, CC_L);
                    else if (src1 == src2) set_cc(cpu, CC_E);
                    else                    set_cc(cpu, CC_G);
                    if (src1 >= src2) { cpu->sfr.ip = ip + disp; return 0; } break;
                case 0x34: // cmpobl
                    if (src1 < src2)       set_cc(cpu, CC_L);
                    else if (src1 == src2) set_cc(cpu, CC_E);
                    else                    set_cc(cpu, CC_G);
                    if (src1 < src2) { cpu->sfr.ip = ip + disp; return 0; } break;
                case 0x35: // cmpobne
                    if (src1 < src2)       set_cc(cpu, CC_L);
                    else if (src1 == src2) set_cc(cpu, CC_E);
                    else                    set_cc(cpu, CC_G);
                    if (src1 != src2) { cpu->sfr.ip = ip + disp; return 0; } break;
                case 0x36: // cmpoble
                    if (src1 < src2)       set_cc(cpu, CC_L);
                    else if (src1 == src2) set_cc(cpu, CC_E);
                    else                    set_cc(cpu, CC_G);
                    if (src1 <= src2) { cpu->sfr.ip = ip + disp; return 0; } break;

                // Compare and branch (integer / signed)
                // Same CC-update fix as the ordinal versions above.
                case 0x38: // cmpibno - never branches
                    break;
                case 0x39: // cmpibg
                    if ((int32_t)src1 < (int32_t)src2)       set_cc(cpu, CC_L);
                    else if ((int32_t)src1 == (int32_t)src2) set_cc(cpu, CC_E);
                    else                                      set_cc(cpu, CC_G);
                    if ((int32_t)src1 > (int32_t)src2) { cpu->sfr.ip = ip + disp; return 0; } break;
                case 0x3a: // cmpibe
                    if ((int32_t)src1 < (int32_t)src2)       set_cc(cpu, CC_L);
                    else if ((int32_t)src1 == (int32_t)src2) set_cc(cpu, CC_E);
                    else                                      set_cc(cpu, CC_G);
                    if ((int32_t)src1 == (int32_t)src2) { cpu->sfr.ip = ip + disp; return 0; } break;
                case 0x3b: // cmpibge
                    if ((int32_t)src1 < (int32_t)src2)       set_cc(cpu, CC_L);
                    else if ((int32_t)src1 == (int32_t)src2) set_cc(cpu, CC_E);
                    else                                      set_cc(cpu, CC_G);
                    if ((int32_t)src1 >= (int32_t)src2) { cpu->sfr.ip = ip + disp; return 0; } break;
                case 0x3c: // cmpibl
                    if ((int32_t)src1 < (int32_t)src2)       set_cc(cpu, CC_L);
                    else if ((int32_t)src1 == (int32_t)src2) set_cc(cpu, CC_E);
                    else                                      set_cc(cpu, CC_G);
                    if ((int32_t)src1 < (int32_t)src2) { cpu->sfr.ip = ip + disp; return 0; } break;
                case 0x3d: // cmpibne
                    if ((int32_t)src1 < (int32_t)src2)       set_cc(cpu, CC_L);
                    else if ((int32_t)src1 == (int32_t)src2) set_cc(cpu, CC_E);
                    else                                      set_cc(cpu, CC_G);
                    if ((int32_t)src1 != (int32_t)src2) { cpu->sfr.ip = ip + disp; return 0; } break;
                case 0x3e: // cmpible
                    if ((int32_t)src1 < (int32_t)src2)       set_cc(cpu, CC_L);
                    else if ((int32_t)src1 == (int32_t)src2) set_cc(cpu, CC_E);
                    else                                      set_cc(cpu, CC_G);
                    if ((int32_t)src1 <= (int32_t)src2) { cpu->sfr.ip = ip + disp; return 0; } break;
                case 0x3f: // cmpibo - always branches
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
                    if (src1 < src2)      set_cc(cpu, CC_L);
                    else if (src1 == src2) set_cc(cpu, CC_E);
                    else                   set_cc(cpu, CC_G);
                    break;
                case 0x5a1: // cmpi (compare integer)
                    if ((int32_t)src1 < (int32_t)src2)      set_cc(cpu, CC_L);
                    else if ((int32_t)src1 == (int32_t)src2) set_cc(cpu, CC_E);
                    else                                      set_cc(cpu, CC_G);
                    break;
                case 0x5a2: // concmpo (conditional compare ordinal)
                    // only updates CC if current CC is not equal
                    if (!(get_cc(cpu) & CC_E)) {
                        if (src1 <= src2) set_cc(cpu, CC_E);
                        else              set_cc(cpu, CC_G);
                    }
                    break;
                case 0x5a3: // concmpi (conditional compare integer)
                    if (!(get_cc(cpu) & CC_E)) {
                        if ((int32_t)src1 <= (int32_t)src2) set_cc(cpu, CC_E);
                        else                                 set_cc(cpu, CC_G);
                    }
                    break;

                // Compare and increment/decrement
                case 0x5a4: // cmpinco (compare, increment ordinal)
                    if (src1 < src2)      set_cc(cpu, CC_L);
                    else if (src1 == src2) set_cc(cpu, CC_E);
                    else                   set_cc(cpu, CC_G);
                    reg_write(cpu, dst_idx, src2 + 1);
                    break;
                case 0x5a5: // cmpinci (compare, increment integer)
                    if ((int32_t)src1 < (int32_t)src2)      set_cc(cpu, CC_L);
                    else if ((int32_t)src1 == (int32_t)src2) set_cc(cpu, CC_E);
                    else                                      set_cc(cpu, CC_G);
                    reg_write(cpu, dst_idx, (uint32_t)((int32_t)src2 + 1));
                    break;
                case 0x5a6: // cmpdeco (compare, decrement ordinal)
                    if (src1 < src2)      set_cc(cpu, CC_L);
                    else if (src1 == src2) set_cc(cpu, CC_E);
                    else                   set_cc(cpu, CC_G);
                    reg_write(cpu, dst_idx, src2 - 1);
                    break;
                case 0x5a7: // cmpdeci (compare, decrement integer)
                    if ((int32_t)src1 < (int32_t)src2)      set_cc(cpu, CC_L);
                    else if ((int32_t)src1 == (int32_t)src2) set_cc(cpu, CC_E);
                    else                                      set_cc(cpu, CC_G);
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
                case 0x584: // notand
                    reg_write(cpu, dst_idx, (~src1) & src2);
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
                    reg_write(cpu, dst_idx, src1 | (~src2));
                    break;
                case 0x58c: // clrbit
                    reg_write(cpu, dst_idx, src2 & ~(1 << (src1 & 31)));
                    break;
                case 0x58d: // notor
                    reg_write(cpu, dst_idx, (~src1) | src2);
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
                case 0x5dc: // movl (move long, 2 regs)
                    reg_write(cpu, dst_idx, reg_read(cpu, src1_idx));
                    reg_write(cpu, dst_idx + 1, reg_read(cpu, src1_idx + 1));
                    break;
                case 0x5ec: // movt (move triple, 3 regs)
                    reg_write(cpu, dst_idx, reg_read(cpu, src1_idx));
                    reg_write(cpu, dst_idx + 1, reg_read(cpu, src1_idx + 1));
                    reg_write(cpu, dst_idx + 2, reg_read(cpu, src1_idx + 2));
                    break;
                case 0x5fc: // movq (move quad, 4 regs)
                    reg_write(cpu, dst_idx, reg_read(cpu, src1_idx));
                    reg_write(cpu, dst_idx + 1, reg_read(cpu, src1_idx + 1));
                    reg_write(cpu, dst_idx + 2, reg_read(cpu, src1_idx + 2));
                    reg_write(cpu, dst_idx + 3, reg_read(cpu, src1_idx + 3));
                    break;

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
                #define WRITE_FLOAT_TO_INT_REG(idx, dval) do { \
                    uint32_t _u_; float _f_ = (float)(dval); \
                    memcpy(&_u_, &_f_, 4); \
                    reg_write(cpu, (idx), _u_); \
                } while(0)

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
                    int32_t i = m1 ? (int32_t)cpu->fp_regs[src1_idx & 3]
                                   : (int32_t)reg_read(cpu, src1_idx);
                    double v = (double)i;
                    FP_DST_WRITE(v);
                    break;
                }
                case 0x675: // cvtilr
                {
                    int32_t i = m1 ? (int32_t)cpu->fp_regs[src1_idx & 3]
                                   : (int32_t)reg_read(cpu, src1_idx);
                    double v = (double)i;
                    FP_DST_WRITE(v);
                    break;
                }

                // Convert real to integer (source is FLOAT, dest is INTEGER)
                case 0x6C0: // cvtri
                {
                    double v = FP_SRC1;  // correctly bit-cast from int reg
                    FP_DST_WRITE_INT((uint32_t)(int32_t)v);
                    break;
                }
                case 0x6C1: // cvtril
                {
                    double v = FP_SRC1;
                    FP_DST_WRITE_INT((uint32_t)(int32_t)v);
                    break;
                }
                case 0x6C2: // cvtzri (truncate toward zero)
                {
                    double v = FP_SRC1;
                    FP_DST_WRITE_INT((uint32_t)(int32_t)v);
                    break;
                }
                case 0x6C3: // cvtzril
                {
                    double v = FP_SRC1;
                    FP_DST_WRITE_INT((uint32_t)(int32_t)v);
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
                    FP_DST_WRITE(a + b);
                    break;
                }
                case 0x78D: // fsubr
                {
                    double a = FP_SRC1;
                    double b = FP_SRC2;
                    FP_DST_WRITE(b - a);
                    break;
                }
                case 0x78C: // fmulr
                {
                    double a = FP_SRC1;
                    double b = FP_SRC2;
                    FP_DST_WRITE(a * b);
                    break;
                }
                case 0x78B: // fdivr
                {
                    double a = FP_SRC1;
                    double b = FP_SRC2;
                    if (a != 0.0) {
                        FP_DST_WRITE(b / a);
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
                case 0x98: // ldl (load long - 2 regs)
                    reg_write(cpu, dst_idx, mem_read32(bus, ea));
                    reg_write(cpu, dst_idx + 1, mem_read32(bus, ea + 4));
                    break;
                case 0x9a: // stl (store long - 2 regs)
                    g_last_store_ip = cpu->sfr.ip;
                    mem_write32(bus, ea, reg_read(cpu, dst_idx));
                    mem_write32(bus, ea + 4, reg_read(cpu, dst_idx + 1));
                    break;
                case 0xA0: // ldt (load triple - 3 regs)
                    reg_write(cpu, dst_idx, mem_read32(bus, ea));
                    reg_write(cpu, dst_idx + 1, mem_read32(bus, ea + 4));
                    reg_write(cpu, dst_idx + 2, mem_read32(bus, ea + 8));
                    break;
                case 0xA2: // stt (store triple - 3 regs)
                    g_last_store_ip = cpu->sfr.ip;
                    mem_write32(bus, ea, reg_read(cpu, dst_idx));
                    mem_write32(bus, ea + 4, reg_read(cpu, dst_idx + 1));
                    mem_write32(bus, ea + 8, reg_read(cpu, dst_idx + 2));
                    break;
                case 0xB0: // ldq (load quad - 4 regs)
                    reg_write(cpu, dst_idx, mem_read32(bus, ea));
                    reg_write(cpu, dst_idx + 1, mem_read32(bus, ea + 4));
                    reg_write(cpu, dst_idx + 2, mem_read32(bus, ea + 8));
                    reg_write(cpu, dst_idx + 3, mem_read32(bus, ea + 12));
                    break;
                case 0xB2: // stq (store quad - 4 regs)
                    g_last_store_ip = cpu->sfr.ip;
                    mem_write32(bus, ea, reg_read(cpu, dst_idx));
                    mem_write32(bus, ea + 4, reg_read(cpu, dst_idx + 1));
                    mem_write32(bus, ea + 8, reg_read(cpu, dst_idx + 2));
                    mem_write32(bus, ea + 12, reg_read(cpu, dst_idx + 3));
                    break;
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

#endif // I960_EXEC_H
