/*
 * m2_texload.h — Sega's texture loader in C (Pinboard #178, #228).
 *
 * Model 2 games keep their textures compressed in ROM and unpack them into
 * texture RAM on every scene change: the i960 decodes a Huffman page into a
 * stream of halfwords and a stream of nibbles, copies the first into texture
 * RAM expanding its runs, and box-filters the second into the mip chain. On the
 * VS screen that is 500,000-790,000 i960 instructions a frame for a quarter of
 * a second, 25-42 ms a frame on a handheld's Cortex-A55. The arcade's own CPU
 * runs flat out through it too, so it is not an emulator artefact to fix; it is
 * work to do faster.
 *
 * The routines are Sega library code (STF's labels come from the Fighting
 * Vipers source):
 *
 *   unpack_lod_data         Huffman page -> halfword + nibble streams
 *   send_lod_data           halfword stream -> texture RAM, runs expanded
 *   send_lod_data_q_sub_*   nibble stream -> the next mip level
 *   send_beta_data          an uncompressed page -> every level
 *
 * ---- One row at a time, and the board cannot tell -------------------------
 *
 * Each routine spends its time in a loop over rows, and between rows it looks
 * at a flag that timer 4 raises when the frame's budget is spent, and gives the
 * CPU back. What the CPU does next depends on that: the frame ends sooner or
 * later, the game's rand() moves with the timers, and the CPU fighter decides
 * differently. So the loader cannot just be run faster; the board has to see
 * exactly what it would have seen.
 *
 * A hook sits on the first instruction of each row's body and does that row
 * instruction for instruction. It ports the i960's own code: every register it
 * touches (temporaries included), the condition code, every store in order and
 * the address it was made from. It counts the instructions and their cycles from
 * the ROM's own words, and the run loop charges them as the i960's (g_hle_extra
 * in hle_hooks.h), so the slice ends on the same instruction and the live timers
 * see the same clock. The i960 runs everything else: the preamble,
 * check_timer_4_result, the flag test between rows, the yield and the resume.
 *
 * A row is left to the i960, which is always exact, when:
 *   - the slice has fewer instructions left than the row needs (g_hle_room),
 *     so the slice still ends part-way through it;
 *   - the live timers would reach their next event inside it, or an interrupt
 *     is already pending: on the board it would be taken mid-row;
 *   - a data watchpoint is armed, or the debugger steps;
 *   - the code is not the code this was written from (an FNV over it), or a
 *     decode table names a handler that is not one of the five here.
 * A row runs into a store log first and is committed only once it is known to
 * fit, so a row declined after a dry run leaves nothing behind. Within a row no
 * read sees one of the row's own stores (the tables, the streams and texture
 * RAM are apart, and the mip filter writes behind where it reads), which is
 * what makes the log exact.
 *
 * `--texload-i960` turns it off, and det_digest --cpu holds the two against
 * each other frame by frame: registers, cycles and texture RAM.
 */
#ifndef M2_TEXLOAD_H
#define M2_TEXLOAD_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "i960.h"
#include "memory.h"
#include "log.h"
#include "hle_hooks.h"
#include "i960_exec.h"
#include "irq_timer.h"
#include "attention.h"
#include "watchpoint.h"

/* On unless the command line or a frontend option says otherwise. */
static int g_texload_hle = 1;

/* Rows done in C since start-up, for get_status. */
static uint64_t g_texload_rows;

typedef struct {
    /* The code the hooks stand in for: FNV-1a over its words [code_lo, code_hi).
     * The ports below are written at STF's addresses; the same code elsewhere
     * is relocated by code_lo - 0x4B3FC. */
    uint32_t code_lo, code_hi, code_fnv;
    /* Absolute addresses the code reads. */
    uint32_t handler_table;   /* NUM_0_TO_7_LONGS */
    uint32_t solid;           /* shader_fil_test: i * 0x1111 */
    uint32_t height;          /* word_55C322: the page's H */
    uint32_t indexed;         /* dword_55CD50: the indexed literals */
} m2_texload_t;

/* Sonic The Fighters (both profiles). */
static const m2_texload_t M2_TEXLOAD_STF = {
    .code_lo = 0x0004B3FC, .code_hi = 0x0004C3D0, .code_fnv = 0x999E2680,
    .handler_table = 0x0004A2EC, .solid = 0x0004A324,
    .height = 0x0055C322, .indexed = 0x0055CD50,
};

#define TL_BASE   0x0004B3FCu
#define TL_WORDS  ((0x0004C3D0u - TL_BASE) / 4u)

/* Whether the program's code is the one described: -1 until the first hook
 * after an install works it out (m2_texload_forget, from the profile's
 * install_fn, which every ROM load runs). With it, each word's cycle cost. */
static int s_tl_known = -1;
static uint16_t s_tl_cost[TL_WORDS];

static inline void m2_texload_forget(void) { s_tl_known = -1; }

static inline bool tl_code_known(const m2_texload_t *t, memory_bus_t *bus) {
    if (s_tl_known < 0) {
        uint32_t h = 0x811C9DC5u;
        for (uint32_t a = t->code_lo; a < t->code_hi; a += 4) { h ^= mem_read32(bus, a); h *= 0x01000193u; }
        s_tl_known = h == t->code_fnv && t->code_hi - t->code_lo == TL_WORDS * 4u;
        if (s_tl_known) {
            i960_cycle_table_init();
            for (uint32_t k = 0; k < TL_WORDS; k++)
                s_tl_cost[k] = (uint16_t)i960_cycle_cost(mem_read32(bus, t->code_lo + k * 4u));
        } else {
            LOG_INFO("texload: the program's texture loader is not the one known; the i960 runs it");
        }
    }
    return s_tl_known == 1;
}

/* ---- A row's run ----------------------------------------------------------- */

#define TL_LOG_MAX 4096

typedef struct {
    uint32_t g[16], r[16];
    uint32_t cc;
    uint32_t n, room;      /* instructions run, and the most allowed */
    uint64_t cyc;
    uint32_t ip;           /* the last instruction run (STF address) */
    uint32_t rel;          /* code_lo - TL_BASE */
    uint32_t nlog;
    struct { uint32_t ip, a, v, size; } log[TL_LOG_MAX];
} tl_run_t;

static tl_run_t s_tl_run;

/* One instruction, at STF address a. A row that would outgrow the slice (or a
 * runaway one) gives up, and the i960 runs it. */
#define I(a) do { if (++x->n > x->room) return 0; \
        x->cyc += s_tl_cost[((a) - TL_BASE) >> 2]; x->ip = (a); } while (0)

#define G(k) x->g[k]
#define R(k) x->r[k]
#define RD16(a) mem_read16(bus, (a))
#define RD32(a) mem_read32(bus, (a))
#define ST(sz_, addr_, val_) do { if (x->nlog == TL_LOG_MAX) return 0; \
        x->log[x->nlog].ip = x->ip; x->log[x->nlog].a = (addr_); x->log[x->nlog].v = (val_); \
        x->log[x->nlog].size = (sz_); x->nlog++; } while (0)
#define CMPO(a, b) (x->cc = i960_cmp_cc_o((a), (b)))
#define CMPI(a, b) (x->cc = i960_cmp_cc_i((int32_t)(a), (int32_t)(b)))
/* cmpdeco 1, v, v */
#define CMPDECO1(v) do { x->cc = i960_cmp_cc_o(1u, (v)); (v) -= 1u; } while (0)
#define BL  (x->cc & CC_L)
#define BE  (x->cc & CC_E)
#define BG  (x->cc & CC_G)
#define BGE (x->cc & CC_GE)

static inline uint32_t tl_shr(uint32_t v, uint32_t n) { return n >= 32 ? 0 : v >> n; }
static inline uint32_t tl_shl(uint32_t v, uint32_t n) { return n >= 32 ? 0 : v << n; }

/* subo n, r14 / cmpo r14, 16 / shro n, r13 / bg, then the refill of 16 bits
 * from (r15) when the count is down to 16. `at` is the subo's address. */
#define TL_DROP(at, nbits) do { \
        uint32_t nb_ = (nbits); \
        I(at);      R(14) -= nb_; \
        I(at + 4);  CMPO(R(14), 0x10u); \
        I(at + 8);  R(13) = tl_shr(R(13), nb_); \
        I(at + 12); if (!BG) { \
            I(at + 16); G(0) = tl_shl(G(11), R(14)); \
            I(at + 20); G(11) = RD16(R(15)); \
            I(at + 24); R(15) += 2; \
            I(at + 28); R(13) |= G(0); \
            I(at + 32); R(14) += 0x10; \
        } } while (0)

/* shlo 24 / shro 24 / ldl (r5)[g0*8], g4: the fast table's entry for the next
 * byte of the bit buffer. */
#define TL_PEEK(at) do { \
        I(at);     G(0) = R(13) << 24; \
        I(at + 4); G(0) >>= 24; \
        I(at + 8); G(4) = RD32(R(5) + G(0) * 8u); G(5) = RD32(R(5) + G(0) * 8u + 4u); \
    } while (0)

/* ---- unpack_lod_data: one row of the decode (0x4B9B4 .. 0x4BAE0) ----------
 * r13/r14 the bit buffer and its count, g11 the next halfword, r15 the source;
 * g4/g5 the fast table's entry for the next byte; r8 the texel, r7 its solid
 * colour; r10/r9 the two nibble rows, r11 the halfword stream, g10 the escape,
 * r6/r12 the index width and mask; g13 counts the row, g14 the rows. */
static uint32_t tl_row_unpack(const m2_texload_t *t, tl_run_t *x, memory_bus_t *bus) {
    const uint32_t rel = x->rel;
    I(0x4B9B4); G(13) = RD16(t->height);
    I(0x4B9BC);
    for (;;) {
        /* A leaf in the first byte, or a walk down the tree. */
        I(0x4BA98); CMPI(G(4), 0);
        I(0x4BA9C); G(2) = G(4) >> 24;
        I(0x4BAA0);
        uint32_t handler;
        if (BGE) {
            I(0x4B9C0); G(3) = G(4);
            I(0x4B9C4); R(4) = 8;
            for (;;) {
                I(0x4B9C8); G(4) = RD32(G(3));
                I(0x4B9CC); x->cc = (R(13) >> (R(4) & 31u)) & 1u ? CC_E : CC_NO;
                I(0x4B9D0); R(4) += 1;
                I(0x4B9D4);
                if (BE) {
                    I(0x4B9E8); CMPI(G(4), 0);
                    I(0x4B9EC); G(3) = G(4);
                    I(0x4B9F0); if (BGE) continue;
                } else {
                    I(0x4B9D8); CMPI(G(4), 0);
                    I(0x4B9DC); G(3) += 4;
                    I(0x4B9E0); if (BGE) continue;
                    I(0x4B9E4);
                }
                break;
            }
            I(0x4B9F4); G(0) = G(4) >> 28;
            I(0x4B9F8); R(4) -= 1;
            I(0x4B9FC); G(3) = RD32(t->handler_table + G(0) * 4u);
            TL_DROP(0x4BA04, R(4));
            I(0x4BA28); handler = G(3);
        } else {
            I(0x4BAA4); G(2) &= 0xF;
            TL_DROP(0x4BAA8, G(2));
            I(0x4BACC); handler = G(5);
        }

        switch (handler - rel) {
        case 0x4BA2C:            /* tag 9: indexed literal */
            I(0x4BA2C); G(4) = R(13) & R(12);
            TL_DROP(0x4BA30, R(6));
            I(0x4BA54); G(4) = RD32(t->indexed + G(4) * 4u);
            /* fall through */
        case 0x4BA5C:            /* tag 8: literal */
            I(0x4BA5C); R(8) = G(4) + R(8);
            I(0x4BA60); R(8) &= 0xF;
            I(0x4BA64); R(7) = RD16(t->solid + R(8) * 2u);
            I(0x4BA6C); G(4) >>= 8;
            /* fall through */
        case 0x4BA70:            /* tags C, D: payload on the current colour */
            I(0x4BA70); ST(1, R(10), R(8));
            I(0x4BA74); R(10) -= 2;
            I(0x4BA78); G(4) = R(7) + G(4);
            I(0x4BA7C); ST(2, R(11), G(4));
            I(0x4BA80); R(11) += 2;
            TL_PEEK(0x4BA84);
            I(0x4BA90); CMPDECO1(G(13));
            break;
        case 0x4BAE8:            /* tag B: a run of the current texel */
            I(0x4BAE8); ST(2, R(11), G(10));
            I(0x4BAEC); R(11) += 2;
            I(0x4BAF0); G(1) = G(4) << 24;
            I(0x4BAF4); G(1) >>= 24;
            I(0x4BAF8); G(13) -= G(1);
            do {
                I(0x4BAFC); ST(1, R(10), R(8));
                I(0x4BB00); R(10) -= 2;
                I(0x4BB04); CMPDECO1(G(1));
                I(0x4BB08);
            } while (BL);
            I(0x4BB0C); G(2) = R(7) << 8;
            I(0x4BB10); G(2) = G(4) | G(2);
            I(0x4BB14); ST(2, R(11), G(2));
            I(0x4BB18); R(11) += 2;
            TL_PEEK(0x4BB1C);
            I(0x4BB28); CMPO(0, G(13));
            I(0x4BB2C);
            break;
        case 0x4BB30:            /* tag A: inline literal */
            I(0x4BB30); G(2) = R(13) << 16;
            I(0x4BB34); G(2) >>= 16;
            TL_DROP(0x4BB38, 0x10u);
            I(0x4BB5C); G(1) = R(13) & 0xF;
            TL_DROP(0x4BB60, 4u);
            TL_PEEK(0x4BB84);
            I(0x4BB90); R(8) = G(1) + R(8);
            I(0x4BB94); R(8) &= 0xF;
            I(0x4BB98); R(7) = RD16(t->solid + R(8) * 2u);
            I(0x4BBA0); CMPDECO1(G(13));
            I(0x4BBA4); ST(1, R(10), R(8));
            I(0x4BBA8); R(10) -= 2;
            I(0x4BBAC); G(2) = R(7) + G(2);
            I(0x4BBB0); ST(2, R(11), G(2));
            I(0x4BBB4); R(11) += 2;
            I(0x4BBB8);
            break;
        default:
            return 0;            /* a handler this port does not know */
        }
        I(0x4BA94);
        if (BE) break;
    }
    I(0x4BAD0); G(0) = R(9);
    I(0x4BAD4); R(9) = R(10);
    I(0x4BAD8); R(10) = G(0);
    I(0x4BADC); CMPDECO1(G(14));
    I(0x4BAE0);
    return BL ? 0x4B974 : 0x4BAE4;
}

/* ---- send_beta_data: one row (0x4BD30 .. 0x4BD98) --------------------------
 * Sixteen bytes at a time from (r15) to (r10), the low halves first. */
static uint32_t tl_row_send_beta(const m2_texload_t *t, tl_run_t *x, memory_bus_t *bus) {
    (void)t;
    I(0x4BD30); R(10) = R(11);
    I(0x4BD34); R(11) += 0x400;
    I(0x4BD38); G(13) = R(13) >> 3;
    do {
        I(0x4BD3C); for (uint32_t k = 0; k < 4; k++) G(k) = RD32(R(15) + 4u * k);
        I(0x4BD40); R(15) += 0x10;
        uint32_t at = 0x4BD44;
        for (int k = 0; k < 4; k++) {
            I(at); ST(2, R(10), G(k)); at += 4;
            I(at); R(10) += 2;         at += 4;
        }
        for (int k = 0; k < 4; k++) { I(at); G(4 + k) = G(k) >> 16; at += 4; }
        for (int k = 0; k < 4; k++) {
            I(at); ST(2, R(10), G(4 + k)); at += 4;
            I(at); R(10) += 2;             at += 4;
        }
        I(0x4BD94); CMPDECO1(G(13));
        I(0x4BD98);
    } while (BL);
    return 0x4BD9C;
}

/* ---- send_lod_data: one row (0x4BF64 .. 0x4BFE8) ---------------------------
 * The halfword stream (r9, the next halfword in g2) into texture RAM at r10; a
 * halfword equal to the escape (r8) is followed by the run's pair and count. */
static uint32_t tl_row_send_lod(const m2_texload_t *t, tl_run_t *x, memory_bus_t *bus) {
    I(0x4BF64); R(10) = R(11);
    I(0x4BF68); R(11) += 0x400;
    I(0x4BF6C); R(13) = RD16(t->height);
    uint32_t end;
    for (;;) {
        I(0x4BF74); CMPO(R(8), G(2));
        I(0x4BF78); R(13) -= 1;
        I(0x4BF7C);
        if (!BE) {
            I(0x4BF80); ST(2, R(10), G(2));
            I(0x4BF84); R(10) += 2;
            I(0x4BF88); G(2) = RD16(R(9));
            I(0x4BF8C); CMPI(0, R(13));
            I(0x4BF90); R(9) += 2;
            I(0x4BF94); if (BL) continue;
            end = 0x4BF98;
            break;
        }
        I(0x4BFA4); G(1) = RD16(R(9));
        I(0x4BFA8); R(9) += 2;
        I(0x4BFAC); R(13) += 1;
        I(0x4BFB0); G(2) = G(1) >> 8;
        I(0x4BFB4); G(4) = G(2) << 8;
        I(0x4BFB8); G(2) |= G(4);
        I(0x4BFBC); G(3) = G(1) ^ G(4);
        I(0x4BFC0); R(13) -= G(3);
        do {
            I(0x4BFC4); ST(2, R(10), G(2));
            I(0x4BFC8); R(10) += 2;
            I(0x4BFCC); CMPDECO1(G(3));
            I(0x4BFD0);
        } while (BL);
        I(0x4BFD4); G(2) = RD16(R(9));
        I(0x4BFD8); CMPI(0, R(13));
        I(0x4BFDC); R(9) += 2;
        I(0x4BFE0); if (BL) continue;
        end = 0x4BFE4;
        break;
    }
    I(end);     CMPDECO1(R(12));
    I(end + 4);
    return BL ? 0x4BF14 : end + 8;
}

/* ---- send_lod_data_q_sub_norm / _anim: one row -----------------------------
 * Four nibbles a word from (r9) downwards (the last word in g8, its packed
 * halfword in r6 and its average in r5): the halfword to texture RAM at r10
 * (norm) or the ring at g11 (anim), the average back into the stream at r3. */
static uint32_t tl_row_send_q(tl_run_t *x, memory_bus_t *bus, bool anim) {
    const uint32_t d = anim ? 0x134u : 0;   /* _anim is _norm moved down */
    if (anim) {
        I(0x4C334); R(13) = G(13);
    } else {
        I(0x4C1F8); R(10) = G(11);
        I(0x4C1FC); G(11) = G(10) + G(11);
        I(0x4C200); R(13) = G(13);
    }
    uint32_t *out = anim ? &G(11) : &R(10);
    uint32_t end;
    for (;;) {
        I(0x4C204 + d); CMPO(R(8), G(8));
        I(0x4C208 + d); R(9) -= 4;
        I(0x4C20C + d);
        if (!BE) {
            I(0x4C210 + d); G(2) = R(8) >> 12;
            I(0x4C214 + d); R(6) = R(8) | G(2);
            I(0x4C218 + d); ST(2, *out, R(6));
            I(0x4C21C + d); *out += 2;
            I(0x4C220 + d); G(8) = R(8);
            I(0x4C224 + d); G(2) = R(8) >> 16;
            I(0x4C228 + d); G(2) = R(8) + G(2);
            I(0x4C22C + d); R(8) = RD32(R(9));
            I(0x4C230 + d); G(3) = G(2) >> 8;
            I(0x4C234 + d); G(2) = G(2) + G(3);
            I(0x4C238 + d); G(2) >>= 2;
            I(0x4C23C + d); R(5) = G(2) & 0xF;
            I(0x4C240 + d); ST(1, R(3), R(5));
            I(0x4C244 + d); R(3) -= 2;
            I(0x4C248 + d); CMPDECO1(R(13));
            I(0x4C24C + d); if (BL) continue;
            end = 0x4C250 + d;
        } else {
            I(0x4C268 + d); ST(2, *out, R(6));
            I(0x4C26C + d); *out += 2;
            I(0x4C270 + d); ST(1, R(3), R(5));
            I(0x4C274 + d); R(3) -= 2;
            I(0x4C278 + d); R(8) = RD32(R(9));
            I(0x4C27C + d); CMPDECO1(R(13));
            I(0x4C280 + d); if (BL) continue;
            end = 0x4C284 + d;
        }
        break;
    }
    I(end);      G(0) = R(3);
    I(end + 4);  R(3) = R(4);
    I(end + 8);  R(4) = G(0);
    I(end + 12); CMPDECO1(R(12));
    I(end + 16);
    return BL ? (anim ? 0x4C2E4 : 0x4C1A8) : end + 20;
}
static uint32_t tl_row_send_q_norm(const m2_texload_t *t, tl_run_t *x, memory_bus_t *bus) {
    (void)t; return tl_row_send_q(x, bus, false);
}
static uint32_t tl_row_send_q_anim(const m2_texload_t *t, tl_run_t *x, memory_bus_t *bus) {
    (void)t; return tl_row_send_q(x, bus, true);
}

#undef I
#undef G
#undef R
#undef RD16
#undef RD32
#undef ST
#undef CMPO
#undef CMPI
#undef CMPDECO1
#undef BL
#undef BE
#undef BG
#undef BGE
#undef TL_DROP
#undef TL_PEEK

typedef uint32_t (*tl_row_fn)(const m2_texload_t *, tl_run_t *, memory_bus_t *);

/* Run one row in C if the board cannot tell; 1 leaves it to the i960. */
static inline int m2_texload_row(const m2_texload_t *t, i960_cpu_t *cpu, memory_bus_t *bus, tl_row_fn fn) {
    if (!g_texload_hle || g_hle_room < 2 || wp_armed() || !tl_code_known(t, bus)) return 1;
    const bool live = g_irqt_live != 0;
    if (live && (g_irqt.intreq & g_irqt.intena & 0x03FCu)) return 1;

    tl_run_t *x = &s_tl_run;
    memcpy(x->g, cpu->globals.g, sizeof x->g);
    memcpy(x->r, cpu->locals.r, sizeof x->r);
    x->cc   = cpu->sfr.ac & AC_CC_MASK;
    x->n    = 0;
    x->room = g_hle_room;
    x->cyc  = 0;
    x->rel  = t->code_lo - TL_BASE;
    x->nlog = 0;
    uint32_t end = fn(t, x, bus);
    if (!end) return 1;
    /* The timers would come due inside the row, and on the board an interrupt
     * could be taken there. */
    if (live && g_irqt.pending + (int64_t)x->cyc >= g_irqt.horizon) return 1;

    for (uint32_t k = 0; k < x->nlog; k++) {
        uint32_t ip = x->log[k].ip + x->rel;
        bus->cpu_ip = ip;
        g_last_store_ip = ip;
        if (x->log[k].size == 1) mem_write8(bus, x->log[k].a, x->log[k].v & 0xFFu);
        else                     mem_write16(bus, x->log[k].a, x->log[k].v & 0xFFFFu);
    }
    bus->cpu_ip = x->ip + x->rel;
    memcpy(cpu->globals.g, x->g, sizeof x->g);
    memcpy(cpu->locals.r, x->r, sizeof x->r);
    cpu->sfr.ac = (cpu->sfr.ac & ~(uint32_t)AC_CC_MASK) | x->cc;
    cpu->sfr.ip = end + x->rel;
    if (live) cpu->cycles += x->cyc;
    g_hle_extra = x->n - 1u;
    emu_attn_bump();
    g_texload_rows++;
    return 0;
}

#endif /* M2_TEXLOAD_H */
