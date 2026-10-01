/*
 * m2_texload.h — Sega's texture loader in C (Pinboard #178).
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
 * Vipers source), so this is written against a table of addresses rather than
 * against one game:
 *
 *   unpack_lod_data         Huffman page -> halfword + nibble streams
 *   send_lod_data           halfword stream -> texture RAM, runs expanded
 *   send_lod_data_q         nibble stream -> every mip level
 *   send_beta_data          an uncompressed page -> every level
 *
 * Each one is replaced whole, at its entry, and does exactly the stores the
 * i960 would: texture RAM, the page header words, the code tree, the 8-bit
 * decode table (with the handler addresses the i960 would jump through), both
 * output streams, the animation ring. Ported from the i960 code itself, not
 * from the explorer's JavaScript (vendor/noclip/js/texture.js), which is the
 * same algorithm for every byte that reaches texture RAM but not for every
 * intermediate: a run of 256 packs its count differently.
 *
 * What is NOT the same is time. On the board the three copy routines give up
 * the CPU when the frame's budget runs out (check_timer_4_result arms timer 4;
 * its interrupt sets a flag they test once a row) and carry on next frame,
 * which is why a VS screen's load is spread over fifteen frames. Here a page is
 * done in one go, and timer 4 is never armed. The texture RAM a load leaves is
 * identical; the frame on which it is complete comes sooner. That changes what
 * the i960 executes, so both boards of a netplay session must agree on it.
 *
 * A routine is left to the i960 when it is resuming a page it gave up on (the
 * hook was switched on mid-load), when the board is in its RAM self-test
 * (RAMBASE_START), or when the game's jump table is not the one the addresses
 * describe.
 */
#ifndef M2_TEXLOAD_H
#define M2_TEXLOAD_H

#include <stdbool.h>
#include <stdint.h>

#include "i960.h"
#include "memory.h"
#include "log.h"
#include "hle_hooks.h"

/* On unless the command line or a frontend option says otherwise. */
static int g_texload_hle = 1;

/* Pages done in C since start-up, for get_status and the log. */
static uint64_t g_texload_pages;

typedef struct {
    /* Entries of the four routines. */
    uint32_t unpack, send_beta, send_lod, send_lod_q;
    /* The four handlers the decode loop jumps to by tag (NUM_0_TO_7_LONGS
     * entries 8..13): tag 8 literal, 9 indexed literal, 0xA inline literal,
     * 0xB run, 0xC/0xD plain payload. */
    uint32_t h_lit, h_idx, h_inline, h_run, h_plain;
    uint32_t handler_table;   /* NUM_0_TO_7_LONGS */
    uint32_t solid;           /* shader_fil_test: i * 0x1111 */
    uint32_t literals;        /* the ROM's 256-entry literal table */
    /* The code the hooks stand in for: FNV-1a over its words [code_lo, code_hi).
     * A patched program that keeps the game's profile but moves a routine is
     * left alone rather than having its code replaced by the wrong thing. */
    uint32_t code_lo, code_hi, code_fnv;
    /* RAM. */
    uint32_t rambase_start;   /* nonzero during the RAM self-test */
    uint32_t yield;           /* dword_550080: a routine is part-way through */
    uint32_t yield_q;         /* dword_5500F4: send_lod_data_q_sub_* is */
    uint32_t flags;           /* dword_55C2F4: the page's request flags */
    uint32_t mip;             /* mip_pyramid_tex0: each level's texram address */
    uint32_t hdr;             /* word_55C320: W, H, W*H, nodes, nsym, bits, len, nidx, ibits, escape */
    uint32_t tree;            /* off_55C344 */
    uint32_t indexed;         /* dword_55CD50 */
    uint32_t fast;            /* dword_545000: 256 x (value, handler) */
    uint32_t halfwords;       /* dword_5502F0 */
    uint32_t nib;             /* byte_55C2EC: the nibble stream's top word */
    uint32_t ring;            /* dword_5D0000: the animation ring's pointer */
} m2_texload_t;

/* Sonic The Fighters (both profiles). */
static const m2_texload_t M2_TEXLOAD_STF = {
    .unpack = 0x0004B3FC, .send_beta = 0x0004BC70, .send_lod = 0x0004BE40, .send_lod_q = 0x0004BFF0,
    .h_lit = 0x0004BA5C, .h_idx = 0x0004BA2C, .h_inline = 0x0004BB30, .h_run = 0x0004BAE8,
    .h_plain = 0x0004BA70, .handler_table = 0x0004A2EC, .solid = 0x0004A324,
    .literals = 0x02300010,
    .code_lo = 0x0004B3FC, .code_hi = 0x0004C3D0, .code_fnv = 0x999E2680,
    .rambase_start = 0x00500000, .yield = 0x00550080, .yield_q = 0x005500F4,
    .flags = 0x0055C2F4, .mip = 0x0055C2F8, .hdr = 0x0055C320, .tree = 0x0055C344,
    .indexed = 0x0055CD50, .fast = 0x00545000, .halfwords = 0x005502F0,
    .nib = 0x0055C2EC, .ring = 0x005D0000,
};

/* Header words, as offsets from hdr. */
#define TL_W      0x00   /* 16-bit */
#define TL_H      0x02   /* 16-bit */
#define TL_WH     0x04
#define TL_NODES  0x08
#define TL_NSYM   0x0C
#define TL_BITS   0x10
#define TL_LEN    0x14
#define TL_NIDX   0x18
#define TL_IBITS  0x1C
#define TL_ESCAPE 0x20

#define tl_r8(a)      mem_read8(bus, (a))
#define tl_r16(a)     mem_read16(bus, (a))
#define tl_r32(a)     mem_read32(bus, (a))
#define tl_w8(a, v)   mem_write8(bus, (a), (v))
#define tl_w16(a, v)  mem_write16(bus, (a), (v))
#define tl_w32(a, v)  mem_write32(bus, (a), (v))

/* Whether the program's code is the one described: -1 until the first hook
 * after an install works it out (m2_texload_forget, from the profile's
 * install_fn, which every ROM load runs). */
static int s_tl_known = -1;

static inline void m2_texload_forget(void) { s_tl_known = -1; }

static inline bool tl_code_known(const m2_texload_t *t, memory_bus_t *bus) {
    if (s_tl_known < 0) {
        uint32_t h = 0x811C9DC5u;
        for (uint32_t a = t->code_lo; a < t->code_hi; a += 4) { h ^= tl_r32(a); h *= 0x01000193u; }
        s_tl_known = h == t->code_fnv;
        if (!s_tl_known) LOG_INFO("texload: the program's texture loader is not the one known; the i960 runs it");
    }
    return s_tl_known == 1;
}

/* A routine the i960 has to run itself: resuming, self-test, other code. */
static inline bool tl_leave_to_i960(const m2_texload_t *t, memory_bus_t *bus) {
    return !g_texload_hle || tl_r32(t->yield) != 0 || tl_r32(t->yield_q) != 0
        || tl_r8(t->rambase_start) != 0 || !tl_code_known(t, bus);
}

/* ---- The bit reader (unpack_lod_data's r13/r14/g11/r15) -------------------
 * LSB first, refilled 16 bits at a time from little-endian halfwords whenever
 * the count is 16 or less, the next halfword already fetched. */
typedef struct {
    uint32_t buf, cnt, next, p;
} tl_bits_t;

static inline void tl_refill(memory_bus_t *bus, tl_bits_t *b) {
    if (b->cnt <= 16) {
        b->buf |= b->next << b->cnt;
        b->next = tl_r16(b->p);
        b->p += 2;
        b->cnt += 16;
    }
}

/* shro by 32 or more is 0 on the i960; C leaves it undefined. */
static inline uint32_t tl_shr(uint32_t v, uint32_t n) { return n >= 32 ? 0 : v >> n; }

static inline uint32_t tl_take(memory_bus_t *bus, tl_bits_t *b, uint32_t n) {
    uint32_t v = n >= 32 ? b->buf : b->buf & ((1u << n) - 1u);
    b->cnt -= n;
    b->buf = tl_shr(b->buf, n);
    tl_refill(bus, b);
    return v;
}

/* ---- make_huf_8bit --------------------------------------------------------
 * The 8-bit decode table: for every next byte of the stream, either the leaf it
 * starts with (its value ORed with the code length in bits 24..27, then the
 * handler the decode loop jumps to) or, for a code longer than 8 bits, the
 * address of the tree node to walk on from. Tree entries are addresses for a
 * branch and negative for a leaf; a node's 0-child sits right after it. */
static void tl_make_huf(const m2_texload_t *t, memory_bus_t *bus, uint32_t depth, uint32_t node, uint32_t code) {
    if (depth == 8) { tl_w32(t->fast + code * 8u, node); return; }
    uint32_t len = depth + 1;
    for (int side = 0; side < 2; side++) {
        uint32_t child = side == 0 ? node + 4u : tl_r32(node);
        uint32_t c = side == 0 ? code : code | (1u << depth);
        uint32_t v = tl_r32(child);
        if ((int32_t)v < 0) {
            uint32_t e = v | (len << 24);
            uint32_t h = tl_r32(t->handler_table + (e >> 28) * 4u);
            for (uint32_t i = c; i < 0x100u; i += 1u << len) {
                tl_w32(t->fast + i * 8u, e);
                tl_w32(t->fast + i * 8u + 4u, h);
            }
        } else {
            tl_make_huf(t, bus, len, child, c);
        }
    }
}

/* ---- unpack_lod_data ------------------------------------------------------ */
static inline int m2_texload_unpack(const m2_texload_t *t, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (tl_leave_to_i960(t, bus)) return 1;
    /* The jump table has to be the one the handlers below stand for. */
    const uint32_t want[6] = { t->h_lit, t->h_idx, t->h_inline, t->h_run, t->h_plain, t->h_plain };
    for (int k = 0; k < 6; k++)
        if (tl_r32(t->handler_table + (8u + (uint32_t)k) * 4u) != want[k]) return 1;

    tl_bits_t b = { 0, 0, 0, cpu->globals.g[3] };
    b.next = tl_r16(b.p); b.p += 2;
    tl_refill(bus, &b);
    tl_refill(bus, &b);

    uint32_t w = (tl_take(bus, &b, 8) + 1u) >> 1;
    tl_w16(t->hdr + TL_W, w);
    uint32_t h = (tl_take(bus, &b, 8) + 1u) >> 1;
    tl_w16(t->hdr + TL_H, h);
    tl_w32(t->hdr + TL_WH, w * h);
    uint32_t bits = tl_take(bus, &b, 8);   tl_w32(t->hdr + TL_BITS, bits);
    uint32_t nsym = tl_take(bus, &b, 16);  tl_w32(t->hdr + TL_NSYM, nsym);
    tl_w32(t->hdr + TL_LEN, tl_take(bus, &b, 16));
    uint32_t nidx = tl_take(bus, &b, 16);  tl_w32(t->hdr + TL_NIDX, nidx);
    uint32_t ibits = tl_take(bus, &b, 4);  tl_w32(t->hdr + TL_IBITS, ibits);
    uint32_t escape = tl_take(bus, &b, 16); tl_w32(t->hdr + TL_ESCAPE, escape);

    /* The code table. Symbols below 0x100 index the ROM's literal table, 0x100
     * and 0x101 are the indexed and inline literals, 0x102..0x121 runs,
     * 0x122..0x141 plain payloads, and 0x142 on a branch to node s - 0x142. */
    uint32_t nodes = nsym * 2u - 1u;
    /* The tree has to fit between off_55C344 and the indexed table; a page
     * that says otherwise is left to the i960 (which rereads the same header,
     * so the words written above come out the same). */
    if (nsym == 0 || t->tree + nodes * 4u > t->indexed) return 1;
    tl_w32(t->hdr + TL_NODES, nodes);
    uint32_t mask = bits >= 32 ? 0xFFFFFFFFu : (1u << bits) - 1u;
    uint32_t at = t->tree;
    uint32_t n = nodes;
    do {
        uint32_t s = b.buf & mask;
        b.cnt -= bits;
        b.buf = tl_shr(b.buf, bits);
        tl_refill(bus, &b);
        uint32_t v;
        if (s >= 0x142u) {
            v = t->tree + (s - 0x142u) * 4u;
        } else if (s >= 0x102u) {
            bool plain = s >= 0x122u;
            uint32_t k = s - (plain ? 0x122u : 0x102u) + 1u;
            if (k >= 0x11u) k = (k - 0x10u) << 4;
            v = (plain ? 0xD0000000u : 0xB0000000u) | k;
        } else if (s < 0x100u) {
            uint32_t e = tl_r32(t->literals + s * 4u);
            v = (e & 0xFu) ? (0x80000000u | e) : (0xC0000000u | (e >> 8));
        } else {
            v = s == 0x100u ? 0x90000000u : 0xA0000000u;
        }
        tl_w32(at, v);
        at += 4;
    } while (n-- > 1u);

    /* The indexed literals: 16-bit payload, 4-bit texel step. */
    for (uint32_t i = 0; i < nidx; i++) {
        uint32_t hi = tl_take(bus, &b, 16);
        uint32_t lo = tl_take(bus, &b, 4);
        tl_w32(t->indexed + i * 4u, (hi << 8) | lo);
    }

    tl_make_huf(t, bus, 0, t->tree, 0);

    /* The two output streams. */
    uint32_t out;
    if (tl_r32(t->flags) & 2u) {             /* into the animation ring */
        out = tl_r32(t->ring);
        tl_w32(t->ring, out + 0x2004u);
        tl_w32(out, escape);
        out += 4;
    } else {
        out = t->halfwords;
    }
    uint32_t texel = 0, solid = 0;
    uint32_t imask = ibits >= 32 ? 0xFFFFFFFFu : (1u << ibits) - 1u;
    uint32_t odd = t->nib + 3u, even = t->nib + 2u;
    uint32_t fv = tl_r32(t->fast + (b.buf & 0xFFu) * 8u);
    uint32_t fh = tl_r32(t->fast + (b.buf & 0xFFu) * 8u + 4u);

    /* A bad page must not hang the board: no real one comes near this. */
    uint64_t guard = (uint64_t)(w ? w : 1) * (uint64_t)(h + 0x200u) * 2u + 0x10000u;

    for (uint32_t row = w;;) {
        uint32_t left = h;
        for (;;) {
            if (guard-- == 0) {
                LOG_WARN("texload: page at 0x%08X ran past its size; stopped", cpu->globals.g[3]);
                goto done;
            }
            uint32_t v = fv, handler = fh;
            if ((int32_t)v >= 0) {
                /* A code longer than 8 bits: walk the tree, one bit a node. */
                uint32_t node = v, bit = 8;
                for (;;) {
                    v = tl_r32(node);
                    bool one = (b.buf >> (bit & 31u)) & 1u;
                    bit++;
                    if (one) node = v; else node += 4;
                    if ((int32_t)v < 0) break;
                }
                bit -= 1;
                handler = tl_r32(t->handler_table + (v >> 28) * 4u);
                b.cnt -= bit; b.buf = tl_shr(b.buf, bit); tl_refill(bus, &b);
            } else {
                uint32_t len = (v >> 24) & 0xFu;
                b.cnt -= len; b.buf = tl_shr(b.buf, len); tl_refill(bus, &b);
            }

            bool last;
            if (handler == t->h_run) {
                tl_w16(out, escape); out += 2;
                uint32_t cnt = v & 0xFFu;
                left -= cnt;
                uint32_t k = cnt;
                do { tl_w8(odd, texel); odd -= 2; } while (k-- > 1u);
                tl_w16(out, (solid << 8) | v); out += 2;
                last = left == 0;
            } else if (handler == t->h_inline) {
                uint32_t payload = tl_take(bus, &b, 16);
                uint32_t step = tl_take(bus, &b, 4);
                texel = (texel + step) & 0xFu;
                solid = tl_r16(t->solid + texel * 2u);
                last = left == 1u; left--;
                tl_w8(odd, texel); odd -= 2;
                tl_w16(out, payload + solid); out += 2;
            } else {
                if (handler == t->h_idx) {
                    uint32_t i = b.buf & imask;
                    b.cnt -= ibits; b.buf = tl_shr(b.buf, ibits); tl_refill(bus, &b);
                    v = tl_r32(t->indexed + i * 4u);
                }
                if (handler == t->h_idx || handler == t->h_lit) {
                    texel = (texel + v) & 0xFu;
                    solid = tl_r16(t->solid + texel * 2u);
                    v >>= 8;
                } else if (handler != t->h_plain) {
                    LOG_WARN("texload: unknown decode handler 0x%08X; page left part-done", handler);
                    goto done;
                }
                tl_w8(odd, texel); odd -= 2;
                tl_w16(out, v + solid); out += 2;
                last = left == 1u; left--;
            }
            fv = tl_r32(t->fast + (b.buf & 0xFFu) * 8u);
            fh = tl_r32(t->fast + (b.buf & 0xFFu) * 8u + 4u);
            if (last) break;
        }
        uint32_t sw = odd; odd = even; even = sw;
        if (row-- <= 1u) break;
    }
done:
    g_texload_pages++;
    hle_ret(cpu);
    return 0;
}

/* ---- send_lod_data --------------------------------------------------------
 * The halfword stream into the full-size level, one row pair (0x400 bytes of
 * texture RAM) per W; a halfword equal to the escape is followed by one whose
 * high byte is the repeated texel pair and whose low byte is the count. */
static inline int m2_texload_send_lod(const m2_texload_t *t, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (tl_leave_to_i960(t, bus)) return 1;
    uint32_t flags = tl_r32(t->flags);
    if (!(flags & 2u)) {
        uint32_t escape = tl_r32(t->hdr + TL_ESCAPE);
        uint32_t dst = tl_r32(t->mip);
        uint32_t src;
        if (flags & 4u) {
            src = tl_r32(t->ring);
            tl_w32(t->ring, src + 0x2004u);
            escape = tl_r32(src);
            src += 4;
        } else {
            src = t->halfwords;
        }
        uint32_t hw = tl_r16(src); src += 2;
        uint32_t rows = tl_r16(t->hdr + TL_W), h = tl_r16(t->hdr + TL_H);
        do {
            uint32_t o = dst;
            dst += 0x400;
            int32_t left = (int32_t)h;
            do {
                left--;
                if (hw == escape) {
                    uint32_t packed = tl_r16(src); src += 2;
                    left++;
                    uint32_t hi = packed >> 8, value = hi | (hi << 8);
                    uint32_t cnt = packed ^ (hi << 8);
                    left -= (int32_t)cnt;
                    do { tl_w16(o, value); o += 2; } while (cnt-- > 1u);
                } else {
                    tl_w16(o, hw); o += 2;
                }
                hw = tl_r16(src); src += 2;
            } while (left > 0);
        } while (rows-- > 1u);
    }
    g_texload_pages++;
    hle_ret(cpu);
    return 0;
}

/* ---- send_lod_data_q ------------------------------------------------------
 * Every smaller level from the nibble stream: four nibbles arrive as one word,
 * packed together they are this level's halfword and their average is the next
 * level's nibble, written back into the stream in place. The i960 keeps the
 * last word and its two results, so a run of equal words costs one sum; the
 * results are the same either way, but the first word compared against is -1
 * and the "results" before it are the frame's zeroed locals. */
static inline int m2_texload_send_lod_q(const m2_texload_t *t, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (tl_leave_to_i960(t, bus)) return 1;
    bool anim = tl_r32(t->flags) & 2u;
    uint32_t w = tl_r16(t->hdr + TL_W), h = tl_r16(t->hdr + TL_H);
    uint32_t level_at = t->mip;
    uint32_t ring = anim ? tl_r32(t->ring) : 0;
    for (;;) {
        w >>= 1; h >>= 1;
        if (anim ? w * h == 0 : (w == 0 || h == 0)) break;
        uint32_t dst = 0;
        if (!anim) { level_at += 4; dst = tl_r32(level_at); }
        uint32_t last = 0xFFFFFFFFu, half = 0, nib = 0;
        uint32_t rd = t->nib, odd = t->nib + 3u, even = t->nib + 2u;
        uint32_t word = tl_r32(rd);
        for (uint32_t r = w;;) {
            uint32_t o = dst;
            dst += 0x400;
            for (uint32_t c = h;;) {
                rd -= 4;
                if (word != last) {
                    half = word | (word >> 12);
                    last = word;
                    uint32_t sum = (word >> 16) + word;
                    word = tl_r32(rd);
                    sum += sum >> 8;
                    nib = (sum >> 2) & 0xFu;
                } else {
                    word = tl_r32(rd);
                }
                if (anim) { tl_w16(ring, half); ring += 2; }
                else      { tl_w16(o, half);    o += 2;    }
                tl_w8(odd, nib); odd -= 2;
                if (c-- <= 1u) break;
            }
            uint32_t sw = odd; odd = even; even = sw;
            if (r-- <= 1u) break;
        }
    }
    if (anim) tl_w32(t->ring, ring);
    hle_ret(cpu);
    return 0;
}

/* ---- send_beta_data --------------------------------------------------------
 * An uncompressed page: W+1, H+1, a header length, then every level's texels in
 * texture RAM order, sixteen bytes at a time (four words, low halves first). */
static inline int m2_texload_send_beta(const m2_texload_t *t, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (tl_leave_to_i960(t, bus)) return 1;
    uint32_t p = cpu->globals.g[3];
    uint32_t w = tl_r8(p) + 1u, h = tl_r8(p + 1) + 1u;
    p += 2;
    p += tl_r8(p);
    if (p & 0xFu) return 1;   /* the i960 prints "Send Tex Align Error!!" */
    uint32_t level_at = t->mip;
    for (;;) {
        uint32_t dst = tl_r32(level_at);
        level_at += 4;
        w >>= 1; h >>= 1;
        if (w < 1 || h < 8) break;
        for (uint32_t r = w;;) {
            uint32_t o = dst;
            dst += 0x400;
            for (uint32_t g = h >> 3;;) {
                uint32_t q[4] = { tl_r32(p), tl_r32(p + 4), tl_r32(p + 8), tl_r32(p + 12) };
                p += 16;
                for (int k = 0; k < 4; k++) { tl_w16(o, q[k]);       o += 2; }
                for (int k = 0; k < 4; k++) { tl_w16(o, q[k] >> 16); o += 2; }
                if (g-- <= 1u) break;
            }
            if (r-- <= 1u) break;
        }
    }
    g_texload_pages++;
    hle_ret(cpu);
    return 0;
}

#undef tl_r8
#undef tl_r16
#undef tl_r32
#undef tl_w8
#undef tl_w16
#undef tl_w32

#endif /* M2_TEXLOAD_H */
