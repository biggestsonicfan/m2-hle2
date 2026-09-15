/*
 * m68k_timing.h — how long each 68000 instruction takes, in clock periods.
 *
 * Built once into a 65536-entry table from Motorola's MC68000 User's Manual,
 * section 8 ("Instruction Execution Times"): a base time per instruction plus
 * the effective-address time of each operand (Table 8-1). The table holds the
 * part known from the opcode alone; m68k_exec.h adds what depends on the run:
 * a branch taken or not, a DBcc that expires, a set Scc, the shift count, the
 * registers MOVEM moves, the bits MULU/MULS multiply by.
 *
 * Why it matters: the sound board (sound.h) gives the 68000 256 periods per
 * 44.1 kHz sample, and the SCSP's timers count from the moment the driver
 * reloads them inside its interrupt handler. How long that handler's prologue
 * takes is part of every timer period, so an instruction model that is a few
 * periods short on branches and returns plays the music measurably fast (0.2%
 * with a bus-access count, against MAME's cycle-exact 68000).
 */
#ifndef M68K_TIMING_H
#define M68K_TIMING_H

#include <stdint.h>

static uint8_t m68k_time[65536];
static int     m68k_time_ready;

/* Table 8-1: effective address calculation time (0 = the mode does not exist) */
static inline int m68k_ea_time(int mode, int reg, int is_long) {
    static const int8_t BW[12] = { 0, 0, 4, 4, 6, 8, 10, 8, 12, 8, 10, 4 };
    static const int8_t L[12]  = { 0, 0, 8, 8, 10, 12, 14, 12, 16, 12, 14, 8 };
    int i = mode < 7 ? mode : (reg <= 4 ? 7 + reg : -1);
    if (i < 0) return 0;
    return is_long ? L[i] : BW[i];
}

/* a MOVE destination costs its write, without the extra time -(An) takes as a source */
static inline int m68k_move_dst_time(int mode, int reg, int is_long) {
    if (mode == 4) return is_long ? 8 : 4;
    return m68k_ea_time(mode, reg, is_long);
}

/* jump-type addressing (JMP / JSR / LEA / PEA), indexed by EA: (An), (d16,An), (d8,An,Xn), abs.W, abs.L, (d16,PC), (d8,PC,Xn) */
static inline int m68k_ctl_index(int mode, int reg) {
    switch (mode) {
    case 2: return 0;
    case 5: return 1;
    case 6: return 2;
    case 7: return reg <= 3 ? 3 + reg : -1;
    }
    return -1;
}

static int m68k_time_of(uint16_t op) {
    int grp = op >> 12, mode = (op >> 3) & 7, reg = op & 7;
    int szb = (op >> 6) & 3;
    int L = szb == 2;                                   /* size field 10 = long (groups 0, 4, 5, 8-D) */
    int rn_imm = mode == 0 || mode == 1 || (mode == 7 && reg == 4);

    switch (grp) {
    case 0x0:
        if (op & 0x0100) {                              /* dynamic bit ops / MOVEP */
            if (mode == 1) return szb >= 2 ? 24 : 16;
            int type = szb;                             /* 0 BTST, 1 BCHG, 2 BCLR, 3 BSET */
            if (mode == 0) return type == 0 ? 6 : type == 2 ? 10 : 8;
            return (type == 0 ? 4 : 8) + m68k_ea_time(mode, reg, 0);
        }
        switch ((op >> 8) & 0xF) {
        case 0x8: {                                     /* static bit ops */
            int type = szb;
            if (mode == 0) return type == 0 ? 10 : type == 2 ? 14 : 12;
            return (type == 0 ? 8 : 12) + m68k_ea_time(mode, reg, 0);
        }
        case 0x0: case 0x2: case 0x4: case 0x6: case 0xA:  /* ORI ANDI SUBI ADDI EORI */
            if (mode == 7 && reg == 4) return 20;       /* to CCR / SR */
            if (mode == 0) return L ? 16 : 8;
            return (L ? 20 : 12) + m68k_ea_time(mode, reg, L);
        case 0xC:                                       /* CMPI */
            if (mode == 0) return L ? 14 : 8;
            return (L ? 12 : 8) + m68k_ea_time(mode, reg, L);
        }
        return 34;

    case 0x1: case 0x2: case 0x3: {                     /* MOVE: 4 + source + destination */
        int ml = grp == 2;
        int dmode = (op >> 6) & 7, dreg = (op >> 9) & 7;
        return 4 + m68k_ea_time(mode, reg, ml) + m68k_move_dst_time(dmode, dreg, ml);
    }

    case 0x4: {
        if ((op & 0xF1C0) == 0x41C0) {                  /* LEA */
            static const int8_t T[7] = { 4, 8, 12, 8, 12, 8, 12 };
            int i = m68k_ctl_index(mode, reg); return i < 0 ? 34 : T[i];
        }
        if ((op & 0xF1C0) == 0x4180) return 10 + m68k_ea_time(mode, reg, 0);   /* CHK, no trap */
        switch (op & 0xFFC0) {
        case 0x40C0: return mode == 0 ? 6 : 8 + m68k_ea_time(mode, reg, 0);    /* MOVE from SR */
        case 0x44C0: case 0x46C0: return 12 + m68k_ea_time(mode, reg, 0);      /* MOVE to CCR / SR */
        case 0x4800: return mode == 0 ? 6 : 8 + m68k_ea_time(mode, reg, 0);    /* NBCD */
        case 0x4840:
            if (mode == 0) return 4;                                             /* SWAP */
            { static const int8_t T[7] = { 12, 16, 20, 16, 20, 16, 20 };         /* PEA */
              int i = m68k_ctl_index(mode, reg); return i < 0 ? 34 : T[i]; }
        case 0x4880: case 0x48C0:
            if (mode == 0) return 4;                                             /* EXT */
            switch (mode) {                                                      /* MOVEM R->M, + 4 per word */
            case 2: case 4: return 8;
            case 5: return 12;
            case 6: return 14;
            case 7: return reg == 0 ? 12 : 16;
            }
            return 34;
        case 0x4AC0:
            if (op == 0x4AFC) return 34;                                         /* ILLEGAL */
            return mode == 0 ? 4 : 10 + m68k_ea_time(mode, reg, 0);             /* TAS */
        case 0x4C80: case 0x4CC0:                                                /* MOVEM M->R, + 4 per word */
            switch (mode) {
            case 2: case 3: return 12;
            case 5: return 16;
            case 6: return 18;
            case 7: return reg == 0 ? 16 : reg == 1 ? 20 : reg == 2 ? 16 : 18;
            }
            return 34;
        case 0x4E80: { static const int8_t T[7] = { 16, 18, 22, 18, 20, 18, 22 };   /* JSR */
                       int i = m68k_ctl_index(mode, reg); return i < 0 ? 34 : T[i]; }
        case 0x4EC0: { static const int8_t T[7] = { 8, 10, 14, 10, 12, 10, 14 };    /* JMP */
                       int i = m68k_ctl_index(mode, reg); return i < 0 ? 34 : T[i]; }
        }
        if ((op & 0xFFF0) == 0x4E40) return 34;         /* TRAP */
        if ((op & 0xFFF8) == 0x4E50) return 16;         /* LINK */
        if ((op & 0xFFF8) == 0x4E58) return 12;         /* UNLK */
        if ((op & 0xFFF0) == 0x4E60) return 4;          /* MOVE USP */
        switch (op) {
        case 0x4E70: return 132;                        /* RESET */
        case 0x4E71: return 4;                          /* NOP */
        case 0x4E72: return 4;                          /* STOP */
        case 0x4E73: return 20;                         /* RTE */
        case 0x4E75: return 16;                         /* RTS */
        case 0x4E76: return 4;                          /* TRAPV, no trap */
        case 0x4E77: return 20;                         /* RTR */
        }
        if (szb != 3) switch (op & 0xFF00) {
        case 0x4000: case 0x4200: case 0x4400: case 0x4600:   /* NEGX CLR NEG NOT */
            if (mode == 0) return L ? 6 : 4;
            return (L ? 12 : 8) + m68k_ea_time(mode, reg, L);
        case 0x4A00:                                    /* TST */
            return 4 + m68k_ea_time(mode, reg, L);
        }
        return 34;
    }

    case 0x5:
        if (szb == 3) {
            if (mode == 1) return 12;                   /* DBcc, condition true (adjusted when it branches or expires) */
            return mode == 0 ? 4 : 8 + m68k_ea_time(mode, reg, 0);   /* Scc (+2 on Dn when set) */
        }
        if (mode == 0) return L ? 8 : 4;                /* ADDQ / SUBQ */
        if (mode == 1) return 8;
        return (L ? 12 : 8) + m68k_ea_time(mode, reg, L);

    case 0x6: {
        int cc = (op >> 8) & 0xF, disp8 = op & 0xFF;
        if (cc == 1) return 18;                         /* BSR */
        if (cc == 0) return 10;                         /* BRA */
        return disp8 ? 8 : 12;                          /* Bcc not taken (adjusted when taken) */
    }

    case 0x7:
        return 4;                                       /* MOVEQ */

    case 0x8: case 0xC: {                               /* OR / DIVU / DIVS / SBCD, AND / MULU / MULS / ABCD / EXG */
        if (szb == 3) {
            if (grp == 0x8) return ((op & 0x0100) ? 158 : 140) + m68k_ea_time(mode, reg, 0);
            return 38 + m68k_ea_time(mode, reg, 0);    /* MULx: + 2 per bit counted at run time */
        }
        if ((op & 0x01F0) == 0x0100) return mode == 0 ? 6 : 18;    /* SBCD / ABCD */
        if (grp == 0xC && (op & 0x01F8) == 0x0140) return 6;       /* EXG Dx,Dy */
        if (grp == 0xC && (op & 0x01F8) == 0x0148) return 6;       /* EXG Ax,Ay */
        if (grp == 0xC && (op & 0x01F8) == 0x0188) return 6;       /* EXG Dx,Ay */
        if (op & 0x0100) return (L ? 12 : 8) + m68k_ea_time(mode, reg, L);          /* Dn,<M> */
        return (L ? (rn_imm ? 8 : 6) : 8) + m68k_ea_time(mode, reg, L);             /* <ea>,Dn */
    }

    case 0x9: case 0xD:                                 /* SUB / ADD */
        if (szb == 3) {                                 /* SUBA / ADDA */
            int al = (op & 0x0100) != 0;
            return (al ? (rn_imm ? 8 : 6) : 8) + m68k_ea_time(mode, reg, al);
        }
        if ((op & 0x0130) == 0x0100)                    /* SUBX / ADDX */
            return mode == 0 ? (L ? 12 : 8) : (L ? 30 : 18);
        if (op & 0x0100) return (L ? 12 : 8) + m68k_ea_time(mode, reg, L);
        return (L ? (rn_imm ? 8 : 6) : 8) + m68k_ea_time(mode, reg, L);

    case 0xB:
        if (szb == 3) return 6 + m68k_ea_time(mode, reg, (op & 0x0100) != 0);        /* CMPA */
        if (op & 0x0100) {
            if (mode == 1) return L ? 20 : 12;          /* CMPM */
            if (mode == 0) return L ? 12 : 8;           /* EOR Dn,Dn */
            return (L ? 12 : 8) + m68k_ea_time(mode, reg, L);
        }
        return 6 + m68k_ea_time(mode, reg, L);          /* CMP */

    case 0xE:
        if (szb == 3) return 8 + m68k_ea_time(mode, reg, 0);        /* memory shift, one bit */
        return L ? 8 : 6;                               /* register shift, + 2 per bit */

    default:
        return 34;                                      /* line A / line F */
    }
}

static inline void m68k_timing_init(void) {
    if (m68k_time_ready) return;
    for (int op = 0; op < 65536; op++) m68k_time[op] = (uint8_t)m68k_time_of((uint16_t)op);
    m68k_time_ready = 1;
}

#endif /* M68K_TIMING_H */
