/*
 * z80.h — a Zilog Z80 (board-level).
 *
 * The CPU of the Model 1 I/O board (model1io.h) the original Model 2 keeps:
 * Daytona USA's and Virtua Cop's. Every documented instruction and the
 * undocumented ones a compiler-free 1990s program can reach (the IXH/IXL
 * halves, SLL, the DDCB/FDCB copies into a register, the X/Y flag bits, the
 * ED holes as two-byte NOPs), in the T-states Zilog's manual gives. The flag
 * rules are the ones MAME's z80.cpp and the FUSE test suite agree on, MEMPTR
 * (WZ) included, since BIT n,(HL) shows it in X/Y.
 *
 * The board drives it through four callbacks (memory and I/O, byte wide) and
 * z80_run, which executes whole instructions until the cycle budget is spent
 * and returns the cycles it took. An interrupt is a level (z80_set_irq) taken
 * between instructions when IFF1 is set, in the mode IM chose; NMI is an edge
 * (z80_nmi). Nothing here reads a host clock or host maths, so two boards
 * given the same inputs run the same.
 */
#ifndef M2HLE_Z80_H
#define M2HLE_Z80_H

#include <stdint.h>
#include <string.h>

typedef uint8_t (*z80_read_fn)(void *user, uint16_t addr);
typedef void    (*z80_write_fn)(void *user, uint16_t addr, uint8_t val);

typedef struct {
    /* Main and alternate register pairs, AF BC DE HL, high byte first. */
    uint16_t af, bc, de, hl, af2, bc2, de2, hl2;
    uint16_t ix, iy, sp, pc, wz;
    uint8_t  i, r;                /* R counts its low 7 bits; only LD R,A sets bit 7 */
    uint8_t  iff1, iff2, im, halted;
    uint8_t  ei_delay;            /* EI enables interrupts after the next instruction */
    uint8_t  irq_line, nmi_pending, irq_vector;
    uint8_t  q, q_prev;           /* SCF/CCF's X/Y take F's only if the last op set it */
    uint64_t cycles;              /* T-states run since reset */
} z80_state_t;

typedef struct {
    z80_read_fn  read, in;
    z80_write_fn write, out;
    void        *user;
} z80_bus_t;

enum { Z80_CF = 0x01, Z80_NF = 0x02, Z80_PF = 0x04, Z80_XF = 0x08,
       Z80_HF = 0x10, Z80_YF = 0x20, Z80_ZF = 0x40, Z80_SF = 0x80 };

static inline void z80_reset(z80_state_t *z) {
    memset(z, 0, sizeof *z);
    z->af = 0xFFFF;
    z->sp = 0xFFFF;
    z->irq_vector = 0xFF;
}

static inline void z80_set_irq(z80_state_t *z, int level) { z->irq_line = level ? 1 : 0; }
static inline void z80_nmi(z80_state_t *z) { z->nmi_pending = 1; }

/* ---- Flag tables --------------------------------------------------------- */

typedef struct { uint8_t sz[256], szp[256], szhv_inc[256], szhv_dec[256]; int ready; } z80_tables_t;
static z80_tables_t g_z80_tab;

static inline void z80_tables_init(void) {
    if (g_z80_tab.ready) return;
    for (int i = 0; i < 256; i++) {
        int p = 0;
        for (int b = 0; b < 8; b++) p ^= (i >> b) & 1;
        uint8_t sz = (uint8_t)((i ? i & Z80_SF : Z80_ZF) | (i & (Z80_YF | Z80_XF)));
        g_z80_tab.sz[i]  = sz;
        g_z80_tab.szp[i] = (uint8_t)(sz | (p ? 0 : Z80_PF));
        uint8_t inc = sz;
        if (i == 0x80) inc |= Z80_PF;
        if ((i & 0x0F) == 0x00) inc |= Z80_HF;
        g_z80_tab.szhv_inc[i] = inc;
        uint8_t dec = (uint8_t)(sz | Z80_NF);
        if (i == 0x7F) dec |= Z80_PF;
        if ((i & 0x0F) == 0x0F) dec |= Z80_HF;
        g_z80_tab.szhv_dec[i] = dec;
    }
    g_z80_tab.ready = 1;
}

/* ---- Register access ----------------------------------------------------- */

#define Z80_A(z)  ((uint8_t)((z)->af >> 8))
#define Z80_F(z)  ((uint8_t)(z)->af)
#define Z80_SETA(z, v) ((z)->af = (uint16_t)(((z)->af & 0x00FF) | ((uint16_t)(uint8_t)(v) << 8)))
#define Z80_SETF(z, v) ((z)->af = (uint16_t)(((z)->af & 0xFF00) | (uint8_t)(v)), (z)->q = (uint8_t)(v))

typedef struct { z80_state_t *z; const z80_bus_t *bus; int cyc; } z80_ctx_t;

static inline uint8_t z80_rd(z80_ctx_t *c, uint16_t a) { return c->bus->read(c->bus->user, a); }
static inline void    z80_wr(z80_ctx_t *c, uint16_t a, uint8_t v) { c->bus->write(c->bus->user, a, v); }
static inline uint8_t z80_fetch(z80_ctx_t *c) { return z80_rd(c, c->z->pc++); }
static inline uint16_t z80_fetch16(z80_ctx_t *c) {
    uint16_t lo = z80_fetch(c);
    return (uint16_t)(lo | (uint16_t)z80_fetch(c) << 8);
}
static inline uint16_t z80_rd16(z80_ctx_t *c, uint16_t a) {
    return (uint16_t)(z80_rd(c, a) | (uint16_t)z80_rd(c, (uint16_t)(a + 1)) << 8);
}
static inline void z80_wr16(z80_ctx_t *c, uint16_t a, uint16_t v) {
    z80_wr(c, a, (uint8_t)v);
    z80_wr(c, (uint16_t)(a + 1), (uint8_t)(v >> 8));
}
static inline void z80_push(z80_ctx_t *c, uint16_t v) {
    c->z->sp = (uint16_t)(c->z->sp - 2);
    z80_wr(c, (uint16_t)(c->z->sp + 1), (uint8_t)(v >> 8));
    z80_wr(c, c->z->sp, (uint8_t)v);
}
static inline uint16_t z80_pop(z80_ctx_t *c) {
    uint16_t v = z80_rd16(c, c->z->sp);
    c->z->sp = (uint16_t)(c->z->sp + 2);
    return v;
}
static inline void z80_inc_r(z80_state_t *z) { z->r = (uint8_t)((z->r & 0x80) | ((z->r + 1) & 0x7F)); }

/* The 8-bit register an opcode's 3-bit field names, with H/L replaced by the
 * index register's halves under a DD/FD prefix (idx: 0 HL, 1 IX, 2 IY).
 * Field 6, (HL), is the caller's. */
static inline uint8_t z80_get_r8(z80_state_t *z, int n, int idx) {
    switch (n) {
    case 0: return (uint8_t)(z->bc >> 8);
    case 1: return (uint8_t)z->bc;
    case 2: return (uint8_t)(z->de >> 8);
    case 3: return (uint8_t)z->de;
    case 4: return (uint8_t)((idx == 1 ? z->ix : idx == 2 ? z->iy : z->hl) >> 8);
    case 5: return (uint8_t)(idx == 1 ? z->ix : idx == 2 ? z->iy : z->hl);
    case 7: return Z80_A(z);
    }
    return 0xFF;
}

static inline void z80_set_r8(z80_state_t *z, int n, int idx, uint8_t v) {
    uint16_t *hl = idx == 1 ? &z->ix : idx == 2 ? &z->iy : &z->hl;
    switch (n) {
    case 0: z->bc = (uint16_t)((z->bc & 0x00FF) | v << 8); break;
    case 1: z->bc = (uint16_t)((z->bc & 0xFF00) | v); break;
    case 2: z->de = (uint16_t)((z->de & 0x00FF) | v << 8); break;
    case 3: z->de = (uint16_t)((z->de & 0xFF00) | v); break;
    case 4: *hl = (uint16_t)((*hl & 0x00FF) | v << 8); break;
    case 5: *hl = (uint16_t)((*hl & 0xFF00) | v); break;
    case 7: Z80_SETA(z, v); break;
    }
}

/* BC DE HL SP (or AF for push/pop), HL under a prefix being IX/IY. */
static inline uint16_t *z80_rp(z80_state_t *z, int n, int idx) {
    switch (n) {
    case 0: return &z->bc;
    case 1: return &z->de;
    case 2: return idx == 1 ? &z->ix : idx == 2 ? &z->iy : &z->hl;
    default: return &z->sp;
    }
}

/* ---- ALU ----------------------------------------------------------------- */

static inline void z80_alu(z80_state_t *z, int op, uint8_t v) {
    uint8_t a = Z80_A(z), f = Z80_F(z);
    unsigned r;
    switch (op) {
    case 0: /* ADD */
    case 1: /* ADC */
        r = (unsigned)a + v + (op == 1 ? (f & Z80_CF) : 0);
        Z80_SETF(z, g_z80_tab.sz[r & 0xFF] | ((r >> 8) & Z80_CF) | ((a ^ r ^ v) & Z80_HF)
                    | (((v ^ a ^ 0x80) & (v ^ r) & 0x80) >> 5));
        Z80_SETA(z, r);
        break;
    case 2: /* SUB */
    case 3: /* SBC */
    case 7: /* CP */
        r = (unsigned)a - v - (op == 3 ? (f & Z80_CF) : 0);
        {
            uint8_t nf = (uint8_t)(Z80_NF | ((r >> 8) & Z80_CF) | ((a ^ r ^ v) & Z80_HF)
                                   | (((v ^ a) & (a ^ r) & 0x80) >> 5));
            if (op == 7) {
                /* CP takes X/Y from the operand, not the result. */
                nf |= (uint8_t)(((r & 0xFF) ? (r & Z80_SF) : Z80_ZF) | (v & (Z80_YF | Z80_XF)));
                Z80_SETF(z, nf);
            } else {
                Z80_SETF(z, nf | g_z80_tab.sz[r & 0xFF]);
                Z80_SETA(z, r);
            }
        }
        break;
    case 4: a &= v; Z80_SETA(z, a); Z80_SETF(z, g_z80_tab.szp[a] | Z80_HF); break;
    case 5: a ^= v; Z80_SETA(z, a); Z80_SETF(z, g_z80_tab.szp[a]); break;
    case 6: a |= v; Z80_SETA(z, a); Z80_SETF(z, g_z80_tab.szp[a]); break;
    }
}

static inline uint8_t z80_inc8(z80_state_t *z, uint8_t v) {
    v++;
    Z80_SETF(z, (Z80_F(z) & Z80_CF) | g_z80_tab.szhv_inc[v]);
    return v;
}
static inline uint8_t z80_dec8(z80_state_t *z, uint8_t v) {
    v--;
    Z80_SETF(z, (Z80_F(z) & Z80_CF) | g_z80_tab.szhv_dec[v]);
    return v;
}

/* CB-page rotates and shifts: RLC RRC RL RR SLA SRA SLL SRL. */
static inline uint8_t z80_rot(z80_state_t *z, int op, uint8_t v) {
    uint8_t c, r;
    uint8_t cf = Z80_F(z) & Z80_CF;
    switch (op) {
    case 0: c = v >> 7; r = (uint8_t)(v << 1 | c); break;
    case 1: c = v & 1;  r = (uint8_t)(v >> 1 | c << 7); break;
    case 2: c = v >> 7; r = (uint8_t)(v << 1 | cf); break;
    case 3: c = v & 1;  r = (uint8_t)(v >> 1 | cf << 7); break;
    case 4: c = v >> 7; r = (uint8_t)(v << 1); break;
    case 5: c = v & 1;  r = (uint8_t)((v >> 1) | (v & 0x80)); break;
    case 6: c = v >> 7; r = (uint8_t)(v << 1 | 1); break;
    default: c = v & 1; r = (uint8_t)(v >> 1); break;
    }
    Z80_SETF(z, g_z80_tab.szp[r] | c);
    return r;
}

/* BIT n: X/Y from `xy` (the operand for a register, WZ's high byte for memory). */
static inline void z80_bit(z80_state_t *z, int n, uint8_t v, uint8_t xy) {
    uint8_t f = (uint8_t)((Z80_F(z) & Z80_CF) | Z80_HF | (xy & (Z80_YF | Z80_XF)));
    uint8_t m = (uint8_t)(v & (1u << n));
    if (!m) f |= Z80_ZF | Z80_PF;
    if (n == 7 && m) f |= Z80_SF;
    Z80_SETF(z, f);
}

static inline uint16_t z80_add16(z80_state_t *z, uint16_t a, uint16_t b) {
    uint32_t r = (uint32_t)a + b;
    z->wz = (uint16_t)(a + 1);
    Z80_SETF(z, (Z80_F(z) & (Z80_SF | Z80_ZF | Z80_PF)) | (((a ^ r ^ b) >> 8) & Z80_HF)
                | ((r >> 16) & Z80_CF) | ((r >> 8) & (Z80_YF | Z80_XF)));
    return (uint16_t)r;
}

static inline void z80_adc16(z80_state_t *z, uint16_t b) {
    uint16_t a = z->hl;
    uint32_t r = (uint32_t)a + b + (Z80_F(z) & Z80_CF);
    z->wz = (uint16_t)(a + 1);
    Z80_SETF(z, (((a ^ r ^ b) >> 8) & Z80_HF) | ((r >> 16) & Z80_CF)
                | ((r >> 8) & (Z80_SF | Z80_YF | Z80_XF)) | ((r & 0xFFFF) ? 0 : Z80_ZF)
                | (((b ^ a ^ 0x8000) & (b ^ r) & 0x8000) >> 13));
    z->hl = (uint16_t)r;
}

static inline void z80_sbc16(z80_state_t *z, uint16_t b) {
    uint16_t a = z->hl;
    uint32_t r = (uint32_t)a - b - (Z80_F(z) & Z80_CF);
    z->wz = (uint16_t)(a + 1);
    Z80_SETF(z, (((a ^ r ^ b) >> 8) & Z80_HF) | Z80_NF | ((r >> 16) & Z80_CF)
                | ((r >> 8) & (Z80_SF | Z80_YF | Z80_XF)) | ((r & 0xFFFF) ? 0 : Z80_ZF)
                | (((b ^ a) & (a ^ r) & 0x8000) >> 13));
    z->hl = (uint16_t)r;
}

static inline void z80_daa(z80_state_t *z) {
    uint8_t a = Z80_A(z), f = Z80_F(z), r = a;
    if (f & Z80_NF) {
        if ((f & Z80_HF) || (a & 0x0F) > 9) r -= 6;
        if ((f & Z80_CF) || a > 0x99) r -= 0x60;
    } else {
        if ((f & Z80_HF) || (a & 0x0F) > 9) r += 6;
        if ((f & Z80_CF) || a > 0x99) r += 0x60;
    }
    Z80_SETF(z, (f & (Z80_CF | Z80_NF)) | (a > 0x99 ? Z80_CF : 0) | ((a ^ r) & Z80_HF) | g_z80_tab.szp[r]);
    Z80_SETA(z, r);
}

static inline int z80_cond(z80_state_t *z, int cc) {
    uint8_t f = Z80_F(z);
    switch (cc) {
    case 0: return !(f & Z80_ZF);
    case 1: return  (f & Z80_ZF) != 0;
    case 2: return !(f & Z80_CF);
    case 3: return  (f & Z80_CF) != 0;
    case 4: return !(f & Z80_PF);
    case 5: return  (f & Z80_PF) != 0;
    case 6: return !(f & Z80_SF);
    default: return (f & Z80_SF) != 0;
    }
}

/* ---- CB page ------------------------------------------------------------- */

/* A CB op on a register, or on (HL) / (IX+d) / (IY+d) when `ea_mem` is set
 * (`ea` the address; with a prefix the result is also copied into register
 * z field unless that field is 6). */
static inline void z80_exec_cb(z80_ctx_t *c, uint8_t op, int ea_mem, uint16_t ea, int prefixed) {
    z80_state_t *z = c->z;
    int x = op >> 6, y = (op >> 3) & 7, r = op & 7;
    uint8_t v = ea_mem ? z80_rd(c, ea) : z80_get_r8(z, r, 0);
    uint8_t res;
    switch (x) {
    case 0: res = z80_rot(z, y, v); break;
    case 1:
        z80_bit(z, y, v, ea_mem ? (uint8_t)(z->wz >> 8) : v);
        c->cyc += prefixed ? 20 : ea_mem ? 12 : 8;
        return;
    case 2: res = (uint8_t)(v & ~(1u << y)); break;
    default: res = (uint8_t)(v | (1u << y)); break;
    }
    if (ea_mem) {
        z80_wr(c, ea, res);
        if (prefixed && r != 6) z80_set_r8(z, r, 0, res);
        c->cyc += prefixed ? 23 : 15;
    } else {
        z80_set_r8(z, r, 0, res);
        c->cyc += 8;
    }
}

/* ---- ED page ------------------------------------------------------------- */

static inline void z80_block_flags_ld(z80_state_t *z, uint8_t v) {
    uint8_t n = (uint8_t)(v + Z80_A(z));
    uint8_t f = (uint8_t)((Z80_F(z) & (Z80_SF | Z80_ZF | Z80_CF)) | (n & Z80_XF) | ((n << 4) & Z80_YF));
    if (z->bc) f |= Z80_PF;
    Z80_SETF(z, f);
}

static inline void z80_block_flags_cp(z80_state_t *z, uint8_t v) {
    uint8_t a = Z80_A(z);
    uint8_t r = (uint8_t)(a - v);
    uint8_t f = (uint8_t)((Z80_F(z) & Z80_CF) | Z80_NF | (g_z80_tab.sz[r] & ~(Z80_YF | Z80_XF)) | ((a ^ v ^ r) & Z80_HF));
    if (f & Z80_HF) r--;
    f |= (uint8_t)((r & Z80_XF) | ((r << 4) & Z80_YF));
    if (z->bc) f |= Z80_PF;
    Z80_SETF(z, f);
}

static inline void z80_block_flags_io(z80_state_t *z, uint8_t v, uint8_t k) {
    uint8_t b = (uint8_t)(z->bc >> 8);
    unsigned t = (unsigned)v + k;
    uint8_t f = g_z80_tab.sz[b];
    if (v & 0x80) f |= Z80_NF;
    if (t > 0xFF) f |= Z80_HF | Z80_CF;
    f |= (uint8_t)(g_z80_tab.szp[(uint8_t)((t & 7) ^ b)] & Z80_PF);
    Z80_SETF(z, f);
}

static inline void z80_exec_ed(z80_ctx_t *c) {
    z80_state_t *z = c->z;
    uint8_t op = z80_fetch(c);
    z80_inc_r(z);
    int y = (op >> 3) & 7, p = y >> 1, q = y & 1;
    if (op >= 0x40 && op < 0x80) {
        switch (op & 7) {
        case 0: { /* IN r,(C) */
            uint8_t v = c->bus->in(c->bus->user, z->bc);
            z->wz = (uint16_t)(z->bc + 1);
            if (y != 6) z80_set_r8(z, y, 0, v);
            Z80_SETF(z, (Z80_F(z) & Z80_CF) | g_z80_tab.szp[v]);
            c->cyc += 12;
            return;
        }
        case 1: /* OUT (C),r */
            c->bus->out(c->bus->user, z->bc, y == 6 ? 0 : z80_get_r8(z, y, 0));
            z->wz = (uint16_t)(z->bc + 1);
            c->cyc += 12;
            return;
        case 2:
            if (q) z80_adc16(z, *z80_rp(z, p, 0)); else z80_sbc16(z, *z80_rp(z, p, 0));
            c->cyc += 15;
            return;
        case 3: {
            uint16_t nn = z80_fetch16(c);
            if (q) *z80_rp(z, p, 0) = z80_rd16(c, nn); else z80_wr16(c, nn, *z80_rp(z, p, 0));
            z->wz = (uint16_t)(nn + 1);
            c->cyc += 20;
            return;
        }
        case 4: { /* NEG */
            uint8_t a = Z80_A(z);
            Z80_SETA(z, 0);
            z80_alu(z, 2, a);
            c->cyc += 8;
            return;
        }
        case 5: /* RETN / RETI */
            z->pc = z80_pop(c);
            z->wz = z->pc;
            z->iff1 = z->iff2;
            c->cyc += 14;
            return;
        case 6: { static const uint8_t m[8] = { 0, 0, 1, 2, 0, 0, 1, 2 }; z->im = m[y]; c->cyc += 8; return; }
        default:
            switch (y) {
            case 0: z->i = Z80_A(z); c->cyc += 9; return;               /* LD I,A */
            case 1: z->r = Z80_A(z); c->cyc += 9; return;               /* LD R,A */
            case 2: case 3: {                                           /* LD A,I / LD A,R */
                uint8_t v = y == 2 ? z->i : z->r;
                Z80_SETA(z, v);
                Z80_SETF(z, (Z80_F(z) & Z80_CF) | g_z80_tab.sz[v] | (z->iff2 ? Z80_PF : 0));
                c->cyc += 9;
                return;
            }
            case 4: case 5: {                                           /* RRD / RLD */
                uint8_t m = z80_rd(c, z->hl), a = Z80_A(z);
                if (y == 4) {
                    z80_wr(c, z->hl, (uint8_t)((a << 4) | (m >> 4)));
                    a = (uint8_t)((a & 0xF0) | (m & 0x0F));
                } else {
                    z80_wr(c, z->hl, (uint8_t)((m << 4) | (a & 0x0F)));
                    a = (uint8_t)((a & 0xF0) | (m >> 4));
                }
                Z80_SETA(z, a);
                Z80_SETF(z, (Z80_F(z) & Z80_CF) | g_z80_tab.szp[a]);
                z->wz = (uint16_t)(z->hl + 1);
                c->cyc += 18;
                return;
            }
            default: c->cyc += 8; return;                               /* NOP */
            }
        }
    }
    if (op >= 0xA0 && op <= 0xBB && (op & 7) <= 3 && y >= 4) {
        int dec = y & 1, rep = y >= 6;
        int16_t d = dec ? -1 : 1;
        switch (op & 3) {
        case 0: { /* LDI LDD LDIR LDDR */
            uint8_t v = z80_rd(c, z->hl);
            z80_wr(c, z->de, v);
            z->hl = (uint16_t)(z->hl + d); z->de = (uint16_t)(z->de + d); z->bc--;
            z80_block_flags_ld(z, v);
            if (rep && z->bc) { z->pc -= 2; z->wz = (uint16_t)(z->pc + 1); c->cyc += 21; }
            else c->cyc += 16;
            return;
        }
        case 1: { /* CPI CPD CPIR CPDR */
            uint8_t v = z80_rd(c, z->hl);
            z->hl = (uint16_t)(z->hl + d); z->bc--;
            z->wz = (uint16_t)(z->wz + d);
            z80_block_flags_cp(z, v);
            if (rep && z->bc && !(Z80_F(z) & Z80_ZF)) { z->pc -= 2; z->wz = (uint16_t)(z->pc + 1); c->cyc += 21; }
            else c->cyc += 16;
            return;
        }
        case 2: { /* INI IND INIR INDR */
            uint8_t v = c->bus->in(c->bus->user, z->bc);
            z->wz = (uint16_t)(z->bc + d);
            z->bc = (uint16_t)(z->bc - 0x100);
            z80_wr(c, z->hl, v);
            z->hl = (uint16_t)(z->hl + d);
            z80_block_flags_io(z, v, (uint8_t)((z->bc & 0xFF) + d));
            if (rep && (z->bc >> 8)) { z->pc -= 2; c->cyc += 21; } else c->cyc += 16;
            return;
        }
        default: { /* OUTI OUTD OTIR OTDR */
            uint8_t v = z80_rd(c, z->hl);
            z->bc = (uint16_t)(z->bc - 0x100);
            z->wz = (uint16_t)(z->bc + d);
            c->bus->out(c->bus->user, z->bc, v);
            z->hl = (uint16_t)(z->hl + d);
            z80_block_flags_io(z, v, (uint8_t)z->hl);
            if (rep && (z->bc >> 8)) { z->pc -= 2; c->cyc += 21; } else c->cyc += 16;
            return;
        }
        }
    }
    c->cyc += 8;  /* an ED hole: a two-byte NOP */
}

/* ---- One instruction ----------------------------------------------------- */

static inline void z80_step(z80_ctx_t *c) {
    z80_state_t *z = c->z;
    int idx = 0;
    uint8_t op = z80_fetch(c);
    z80_inc_r(z);
    while (op == 0xDD || op == 0xFD) {
        idx = op == 0xDD ? 1 : 2;
        c->cyc += 4;
        op = z80_fetch(c);
        z80_inc_r(z);
    }
    uint16_t *hlp = idx == 1 ? &z->ix : idx == 2 ? &z->iy : &z->hl;

    /* The (HL) operand's address: (IX+d) / (IY+d) under a prefix (fetching d). */
#define Z80_EA() (idx ? (z->wz = (uint16_t)(*hlp + (int8_t)z80_fetch(c))) : z->hl)

    if (op == 0xCB) {
        if (idx) {
            uint16_t ea = (uint16_t)(*hlp + (int8_t)z80_fetch(c));
            z->wz = ea;
            uint8_t sub = z80_fetch(c);
            z80_inc_r(z);
            c->cyc -= 4;  /* the prefix's 4 are inside the 23 / 20 */
            z80_exec_cb(c, sub, 1, ea, 1);
        } else {
            uint8_t sub = z80_fetch(c);
            z80_inc_r(z);
            z80_exec_cb(c, sub, (sub & 7) == 6, z->hl, 0);
        }
        return;
    }
    if (op == 0xED) { z80_exec_ed(c); return; }

    int x = op >> 6, y = (op >> 3) & 7, zz = op & 7, p = y >> 1, q = y & 1;

    if (x == 1) {
        if (op == 0x76) { z->halted = 1; z->pc--; c->cyc += 4; return; }  /* HALT */
        if (y == 6) {        /* LD (HL),r: r is H/L proper even with a prefix */
            uint16_t ea = Z80_EA();
            z80_wr(c, ea, z80_get_r8(z, zz, 0));
            c->cyc += idx ? 15 : 7;
        } else if (zz == 6) { /* LD r,(HL) */
            uint16_t ea = Z80_EA();
            z80_set_r8(z, y, 0, z80_rd(c, ea));
            c->cyc += idx ? 15 : 7;
        } else {
            z80_set_r8(z, y, idx, z80_get_r8(z, zz, idx));
            c->cyc += 4;
        }
        return;
    }
    if (x == 2) {
        uint8_t v;
        if (zz == 6) { uint16_t ea = Z80_EA(); v = z80_rd(c, ea); c->cyc += idx ? 15 : 7; }
        else         { v = z80_get_r8(z, zz, idx); c->cyc += 4; }
        z80_alu(z, y, v);
        return;
    }
    if (x == 0) {
        switch (zz) {
        case 0:
            switch (y) {
            case 0: c->cyc += 4; return;                                      /* NOP */
            case 1: { uint16_t t = z->af; z->af = z->af2; z->af2 = t; c->cyc += 4; return; }
            case 2: {                                                         /* DJNZ */
                int8_t d = (int8_t)z80_fetch(c);
                uint8_t b = (uint8_t)((z->bc >> 8) - 1);
                z->bc = (uint16_t)((z->bc & 0xFF) | b << 8);
                if (b) { z->pc = (uint16_t)(z->pc + d); z->wz = z->pc; c->cyc += 13; } else c->cyc += 8;
                return;
            }
            case 3: { int8_t d = (int8_t)z80_fetch(c); z->pc = (uint16_t)(z->pc + d); z->wz = z->pc; c->cyc += 12; return; }
            default: {                                                        /* JR cc */
                int8_t d = (int8_t)z80_fetch(c);
                if (z80_cond(z, y - 4)) { z->pc = (uint16_t)(z->pc + d); z->wz = z->pc; c->cyc += 12; } else c->cyc += 7;
                return;
            }
            }
        case 1:
            if (q) { *hlp = z80_add16(z, *hlp, *z80_rp(z, p, idx)); c->cyc += 11; }
            else   { *z80_rp(z, p, idx) = z80_fetch16(c); c->cyc += 10; }
            return;
        case 2:
            switch (y) {
            case 0: z80_wr(c, z->bc, Z80_A(z)); z->wz = (uint16_t)(((z->bc + 1) & 0xFF) | Z80_A(z) << 8); c->cyc += 7; return;
            case 1: Z80_SETA(z, z80_rd(c, z->bc)); z->wz = (uint16_t)(z->bc + 1); c->cyc += 7; return;
            case 2: z80_wr(c, z->de, Z80_A(z)); z->wz = (uint16_t)(((z->de + 1) & 0xFF) | Z80_A(z) << 8); c->cyc += 7; return;
            case 3: Z80_SETA(z, z80_rd(c, z->de)); z->wz = (uint16_t)(z->de + 1); c->cyc += 7; return;
            case 4: { uint16_t nn = z80_fetch16(c); z80_wr16(c, nn, *hlp); z->wz = (uint16_t)(nn + 1); c->cyc += 16; return; }
            case 5: { uint16_t nn = z80_fetch16(c); *hlp = z80_rd16(c, nn); z->wz = (uint16_t)(nn + 1); c->cyc += 16; return; }
            case 6: { uint16_t nn = z80_fetch16(c); z80_wr(c, nn, Z80_A(z)); z->wz = (uint16_t)(((nn + 1) & 0xFF) | Z80_A(z) << 8); c->cyc += 13; return; }
            default: { uint16_t nn = z80_fetch16(c); Z80_SETA(z, z80_rd(c, nn)); z->wz = (uint16_t)(nn + 1); c->cyc += 13; return; }
            }
        case 3: {
            uint16_t *rp = z80_rp(z, p, idx);
            *rp = (uint16_t)(*rp + (q ? -1 : 1));
            c->cyc += 6;
            return;
        }
        case 4: case 5: {
            if (y == 6) {
                uint16_t ea = Z80_EA();
                uint8_t v = z80_rd(c, ea);
                z80_wr(c, ea, zz == 4 ? z80_inc8(z, v) : z80_dec8(z, v));
                c->cyc += idx ? 19 : 11;
            } else {
                uint8_t v = z80_get_r8(z, y, idx);
                z80_set_r8(z, y, idx, zz == 4 ? z80_inc8(z, v) : z80_dec8(z, v));
                c->cyc += 4;
            }
            return;
        }
        case 6:
            if (y == 6) {
                uint16_t ea = Z80_EA();
                z80_wr(c, ea, z80_fetch(c));
                c->cyc += idx ? 15 : 10;
            } else {
                z80_set_r8(z, y, idx, z80_fetch(c));
                c->cyc += 7;
            }
            return;
        default: {
            uint8_t a = Z80_A(z), f = Z80_F(z);
            switch (y) {
            case 0: a = (uint8_t)(a << 1 | a >> 7); Z80_SETF(z, (f & (Z80_SF | Z80_ZF | Z80_PF)) | (a & (Z80_YF | Z80_XF | Z80_CF))); break;
            case 1: Z80_SETF(z, (f & (Z80_SF | Z80_ZF | Z80_PF)) | (a & Z80_CF)); a = (uint8_t)(a >> 1 | a << 7); Z80_SETF(z, Z80_F(z) | (a & (Z80_YF | Z80_XF))); break;
            case 2: { uint8_t r = (uint8_t)(a << 1 | (f & Z80_CF)); Z80_SETF(z, (f & (Z80_SF | Z80_ZF | Z80_PF)) | (a >> 7) | (r & (Z80_YF | Z80_XF))); a = r; break; }
            case 3: { uint8_t r = (uint8_t)(a >> 1 | (f & Z80_CF) << 7); Z80_SETF(z, (f & (Z80_SF | Z80_ZF | Z80_PF)) | (a & Z80_CF) | (r & (Z80_YF | Z80_XF))); a = r; break; }
            case 4: Z80_SETA(z, a); z80_daa(z); c->cyc += 4; return;
            case 5: a = (uint8_t)~a; Z80_SETF(z, (f & (Z80_SF | Z80_ZF | Z80_PF | Z80_CF)) | Z80_HF | Z80_NF | (a & (Z80_YF | Z80_XF))); break;
            case 6: Z80_SETF(z, (f & (Z80_SF | Z80_ZF | Z80_PF)) | Z80_CF | ((((z->q_prev ^ f) | a)) & (Z80_YF | Z80_XF))); break;
            default: Z80_SETF(z, ((f & (Z80_SF | Z80_ZF | Z80_PF | Z80_CF)) | ((f & Z80_CF) << 4) | ((((z->q_prev ^ f) | a)) & (Z80_YF | Z80_XF))) ^ Z80_CF); break;
            }
            Z80_SETA(z, a);
            c->cyc += 4;
            return;
        }
        }
    }

    /* x == 3 */
    switch (zz) {
    case 0:                                                                   /* RET cc */
        if (z80_cond(z, y)) { z->pc = z80_pop(c); z->wz = z->pc; c->cyc += 11; } else c->cyc += 5;
        return;
    case 1:
        if (!q) {                                                             /* POP */
            uint16_t v = z80_pop(c);
            if (p == 3) z->af = v; else *z80_rp(z, p, idx) = v;
            c->cyc += 10;
            return;
        }
        switch (p) {
        case 0: z->pc = z80_pop(c); z->wz = z->pc; c->cyc += 10; return;   /* RET */
        case 1: {                                                             /* EXX */
            uint16_t t;
            t = z->bc; z->bc = z->bc2; z->bc2 = t;
            t = z->de; z->de = z->de2; z->de2 = t;
            t = z->hl; z->hl = z->hl2; z->hl2 = t;
            c->cyc += 4;
            return;
        }
        case 2: z->pc = *hlp; c->cyc += 4; return;                           /* JP (HL) */
        default: z->sp = *hlp; c->cyc += 6; return;                          /* LD SP,HL */
        }
    case 2: {                                                                 /* JP cc,nn */
        uint16_t nn = z80_fetch16(c);
        z->wz = nn;
        if (z80_cond(z, y)) z->pc = nn;
        c->cyc += 10;
        return;
    }
    case 3:
        switch (y) {
        case 0: z->pc = z80_fetch16(c); z->wz = z->pc; c->cyc += 10; return;
        case 2: {                                                             /* OUT (n),A */
            uint8_t n = z80_fetch(c);
            c->bus->out(c->bus->user, (uint16_t)(n | Z80_A(z) << 8), Z80_A(z));
            z->wz = (uint16_t)(((n + 1) & 0xFF) | Z80_A(z) << 8);
            c->cyc += 11;
            return;
        }
        case 3: {                                                             /* IN A,(n) */
            uint8_t n = z80_fetch(c);
            uint16_t port = (uint16_t)(n | Z80_A(z) << 8);
            Z80_SETA(z, c->bus->in(c->bus->user, port));
            z->wz = (uint16_t)(port + 1);
            c->cyc += 11;
            return;
        }
        case 4: {                                                             /* EX (SP),HL */
            uint16_t v = z80_rd16(c, z->sp);
            z80_wr16(c, z->sp, *hlp);
            *hlp = v;
            z->wz = v;
            c->cyc += 19;
            return;
        }
        case 5: { uint16_t t = z->de; z->de = z->hl; z->hl = t; c->cyc += 4; return; }  /* EX DE,HL */
        case 6: z->iff1 = z->iff2 = 0; c->cyc += 4; return;                   /* DI */
        default: z->iff1 = z->iff2 = 1; z->ei_delay = 1; c->cyc += 4; return; /* EI */
        }
    case 4: {                                                                 /* CALL cc,nn */
        uint16_t nn = z80_fetch16(c);
        z->wz = nn;
        if (z80_cond(z, y)) { z80_push(c, z->pc); z->pc = nn; c->cyc += 17; } else c->cyc += 10;
        return;
    }
    case 5:
        if (!q) {                                                             /* PUSH */
            z80_push(c, p == 3 ? z->af : *z80_rp(z, p, idx));
            c->cyc += 11;
            return;
        }
        { uint16_t nn = z80_fetch16(c); z->wz = nn; z80_push(c, z->pc); z->pc = nn; c->cyc += 17; return; }
    case 6: z80_alu(z, y, z80_fetch(c)); c->cyc += 7; return;
    default: z80_push(c, z->pc); z->pc = (uint16_t)(y * 8); z->wz = z->pc; c->cyc += 11; return; /* RST */
    }
#undef Z80_EA
}

/* Run whole instructions until `budget` T-states are spent (an instruction
 * that starts inside the budget finishes). Returns the T-states run. */
static inline int z80_run(z80_state_t *z, const z80_bus_t *bus, int budget) {
    z80_tables_init();
    z80_ctx_t c = { z, bus, 0 };
    while (c.cyc < budget) {
        if (z->nmi_pending) {
            z->nmi_pending = 0;
            if (z->halted) { z->halted = 0; z->pc++; }
            z->iff1 = 0;
            z80_inc_r(z);
            z80_push(&c, z->pc);
            z->pc = 0x66;
            z->wz = z->pc;
            c.cyc += 11;
            continue;
        }
        if (z->irq_line && z->iff1 && !z->ei_delay) {
            if (z->halted) { z->halted = 0; z->pc++; }
            z->iff1 = z->iff2 = 0;
            z80_inc_r(z);
            if (z->im == 2) {
                z80_push(&c, z->pc);
                z->pc = z80_rd16(&c, (uint16_t)(z->i << 8 | z->irq_vector));
                c.cyc += 19;
            } else {
                /* IM 0 with the bus floating (0xFF, RST 38h) is IM 1. */
                z80_push(&c, z->pc);
                z->pc = 0x38;
                c.cyc += z->im == 1 ? 13 : 12;
            }
            z->wz = z->pc;
            continue;
        }
        z->ei_delay = 0;
        z->q_prev = z->q;
        z->q = 0;
        if (z->halted) {
            /* HALT re-runs itself: four T-states a NOP, R counting. */
            z80_inc_r(z);
            c.cyc += 4;
            continue;
        }
        z80_step(&c);
    }
    z->cycles += (uint64_t)c.cyc;
    return c.cyc;
}

#endif /* M2HLE_Z80_H */
