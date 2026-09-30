/*
 * scsp_lazy_test.c -- the bounds the lazy SCSP relies on (scsp.h, "The chip's
 * own time").
 *
 * The chip makes its samples late, so a 68000 write to sound RAM runs on first
 * only the slots whose range (scsp_slot_range) covers it, and a 68000 access
 * inside the DSP's range (scsp_recouple) makes the whole chip catch up. A range
 * that is too small is silent: the slot reads the new bytes for samples that
 * should have had the old. scsp_fuzz cannot aim at that (where a voice is
 * mid-stretch is exactly what differs between a lazy and a lockstep build), so
 * this holds the ranges themselves: each voice, and each DSP program, runs
 * twice from the same state with different garbage everywhere outside the
 * range it claims, and has to make the same samples.
 *
 * ROM-free, a ctest. Exits non-zero on the first voice or program that reads
 * outside its range. `scsp_lazy_test N` runs N times as many (seeds differ).
 */
#define NDEBUG 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scsp.h"

static uint32_t rs = 1;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }

#define RAM 0x80000u
static uint8_t ram_a[RAM], ram_b[RAM];
static scsp_t chip_a, chip_b;
static uint64_t clk;

static void w(scsp_t *s, uint32_t off, uint16_t v) { scsp_write(s, off, v, 2); }
static void both(uint32_t off, uint16_t v) { w(&chip_a, off, v); w(&chip_b, off, v); }

/* different garbage in the two RAMs outside [lo, hi), the same bytes inside
 * (a range that grew takes in bytes the last one had poisoned) */
static void garbage(uint32_t from, uint32_t to) {
    uint64_t x = (uint64_t)rnd() << 32 | rnd() | 1;
    for (; from < to && (from & 7); from++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; ram_a[from] = (uint8_t)x; ram_b[from] = (uint8_t)~x; }
    for (; from + 8 <= to; from += 8) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        uint64_t y = ~x;
        memcpy(ram_a + from, &x, 8);
        memcpy(ram_b + from, &y, 8);
    }
    for (; from < to; from++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; ram_a[from] = (uint8_t)x; ram_b[from] = (uint8_t)~x; }
}
static void poison(uint32_t lo, uint32_t hi) {
    memcpy(ram_b + lo, ram_a + lo, hi - lo);
    garbage(0, lo);
    garbage(hi, RAM);
}

/* One voice set up at random, the way scsp_fuzz draws them, plus the loops a
 * driver would never key: shorter than a step, LSA past LEA, run backwards. */
static void voice(int sl) {
    uint32_t base = (uint32_t)sl * 0x20;
    uint32_t sa = rnd() & 0x7FFFF, lsa = rnd() & 0xFFFF, lea = rnd() & 0xFFFF;
    uint16_t pitch = (uint16_t)(rnd() & 0x7FFF), lfo = (uint16_t)(rnd() % 2 ? rnd() & 0xFCFF : 0);
    switch (rnd() % 5) {
    case 0: lsa &= 0x0FFF; lea = lsa + (rnd() & 0x3FFF); break;
    case 1: lsa &= 0x0FFF; lea = lsa + 1 + rnd() % 64; break;     /* short: creeps at a high pitch */
    case 2: if (lsa > lea) { uint32_t t = lsa; lsa = lea; lea = t; } break;
    case 3: {                                                          /* a loop about a step long, deep vibrato */
        pitch = (uint16_t)((4 + rnd() % 4) << 11 | (rnd() & 0x3FF));
        int oct = (int)(pitch >> 11) + SCSP_SHIFT - 10;
        uint32_t samples = (((pitch & 0x3FFu) + 1024u) << oct) >> SCSP_SHIFT;
        lsa = samples / 2 + rnd() % (samples + 1);
        lea = lsa + samples + rnd() % (samples / 2 + 1);
        lfo = (uint16_t)((rnd() & 0x1F) << 10 | (rnd() & 3) << 8 | 7 << 5);   /* PLFOS 7, not noise */
        if (((lfo >> 8) & 3) == 3) lfo &= (uint16_t)~0x0300;
        break;
    }
    default: break;
    }
    uint16_t w0 = (uint16_t)((sa >> 16) & 0xF);
    w0 |= (uint16_t)((rnd() & 3) << 5);                               /* LPCTL */
    if (rnd() % 3 == 0) w0 |= 0x0010;                                 /* PCM8B */
    w0 |= (uint16_t)((rnd() % 8 == 0 ? 2 + (rnd() & 1) : 0) << 7);   /* SSCTL: RAM, or no read */
    both(base + 0x02, (uint16_t)sa);
    both(base + 0x04, (uint16_t)lsa);
    both(base + 0x06, (uint16_t)lea);
    both(base + 0x08, (uint16_t)rnd());                               /* envelope */
    both(base + 0x0A, (uint16_t)(rnd() & 0x7FFF));                    /* LPSLNK off sometimes; KRS DL RR */
    both(base + 0x0C, (uint16_t)(rnd() & 0xFF));                      /* TL, no SDIR */
    both(base + 0x0E, 0);                                             /* no FM (its range is all of RAM) */
    both(base + 0x10, pitch);                                         /* OCT FNS */
    both(base + 0x12, lfo);                                           /* LFOs, not the noise waveform */
    both(base + 0x14, 0);
    both(base + 0x16, 0xE000 | 0x0F00);                               /* DISDL 7, centre */
    both(base + 0x00, (uint16_t)(w0 | 0x0800 | 0x1000));             /* KYONB + KYONEX */
}

static int run_same(int n, const char *what, int trial) {
    for (int k = 0; k < n; k++) {
        int16_t la, ra, lb, rb;
        clk += 256;
        scsp_sample(&chip_a, &la, &ra);
        scsp_sample(&chip_b, &lb, &rb);
        if (la != lb || ra != rb) {
            fprintf(stderr, "FAIL %s %d: sample %d differs (%d,%d vs %d,%d)\n", what, trial, k, la, ra, lb, rb);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    const int mult = argc > 1 ? atoi(argv[1]) : 1;
    int fails = 0, whole = 0, bounded = 0;

    /* 1. Voices. The DSP stays stopped, so only the slot reads RAM. */
    for (int t = 0; t < 2000 * mult && !fails; t++) {
        rs = 0x9E3779B9u * (uint32_t)(t + 1);
        for (uint32_t i = 0; i < RAM; i++) ram_a[i] = ram_b[i] = (uint8_t)rnd();
        scsp_reset(&chip_a, ram_a, RAM, &clk);
        scsp_reset(&chip_b, ram_b, RAM, &clk);
        both(0x400, 0x000F);
        int sl = (int)(rnd() & 31);
        voice(sl);
        for (int phase = 0; phase < 3 && !fails; phase++) {
            uint32_t lo = chip_a.slot_lo[sl], hi = chip_a.slot_hi[sl];
            if (!chip_a.slot[sl].active) break;
            if (lo == 0 && hi == RAM) { whole++; break; }             /* claims everything: nothing to hold */
            bounded++;
            poison(lo, hi);
            fails += run_same(8000, "voice", t);
            /* Change the voice mid-note (the range is worked out again): the
             * pitch, as a driver does, or the loop -- its bounds, or its kind
             * under a voice already running backwards. */
            uint32_t b = (uint32_t)sl * 0x20;
            switch (rnd() % 3) {
            case 0: both(b + 0x10, (uint16_t)(rnd() & 0x7FFF)); break;
            case 1: { uint16_t a = (uint16_t)rnd(), e = (uint16_t)rnd(); both(b + 0x04, a < e ? a : e); both(b + 0x06, a < e ? e : a); break; }
            default: both(b, (uint16_t)((chip_a.slot[sl].r[0] & ~0x1060u) | (rnd() & 3) << 5)); break;
            }
        }
    }
    printf("voices: %d bounded, %d all of RAM\n", bounded, whole);

    /* 2. DSP programs, random ones: the delay line and TABLE reads. No voices;
     * MIXS is fed directly. */
    int progs = 0, none = 0;
    for (int t = 0; t < 400 * mult && !fails; t++) {
        rs = 0x7F4A7C15u * (uint32_t)(t + 1);
        for (uint32_t i = 0; i < RAM; i++) ram_a[i] = ram_b[i] = (uint8_t)rnd();
        scsp_reset(&chip_a, ram_a, RAM, &clk);
        scsp_reset(&chip_b, ram_b, RAM, &clk);
        both(0x400, 0x000F);
        both(0x402, (uint16_t)(rnd() & 0x1BF));                        /* RBL, RBP */
        for (int i = 0; i < 64; i++) both(0x700 + i * 2, (uint16_t)rnd());
        for (int i = 0; i < 32; i++) both(0x780 + i * 2, (uint16_t)rnd());
        int len = 1 + (int)(rnd() % 128);
        const int table = (int)(rnd() & 1);                             /* half without TABLE: the ring alone */
        for (int i = 0; i < 512; i++) {
            uint16_t v = i < len * 4 ? (uint16_t)rnd() : 0;
            if (i % 4 == 2 && !table) v &= 0x7FFF;
            if (i % 4 == 1 && ((v >> 6) & 0x3F) > 0x31) v = (uint16_t)((v & ~0x0FC0u) | ((rnd() % 0x32) << 6));
            both(0x800 + i * 2, v);
        }
        both(0xBF0, (uint16_t)scsp_read(&chip_a, 0xBF0, 2));             /* start it */
        for (int i = 0; i < 16; i++) {                                 /* hear every EFREG */
            both((uint32_t)i * 0x20 + 0x16, 0x00E0 | (uint16_t)(i & 1 ? 0x1F : 0x0F));
        }
        uint32_t lo = chip_a.dsp_lo, hi = chip_a.dsp_hi;
        if (lo == hi) none++; else progs++;
        poison(lo, hi);
        for (int k = 0; k < 4000; k++) {
            for (int j = 0; j < 16; j++) chip_a.dsp.mixs[j] = chip_b.dsp.mixs[j] = (int32_t)(rnd() & 0xFFFFF) - 0x80000;
            int16_t la, ra, lb, rb;
            clk += 256;
            scsp_sample(&chip_a, &la, &ra);
            scsp_sample(&chip_b, &lb, &rb);
            if (la != lb || ra != rb || memcmp(chip_a.dsp.temp, chip_b.dsp.temp, sizeof chip_a.dsp.temp)) {
                fprintf(stderr, "FAIL dsp %d: sample %d differs\n", t, k);
                fails++;
                break;
            }
        }
    }
    printf("dsp programs: %d with a delay line, %d without\n", progs, none);
    printf(fails ? "FAILED\n" : "ok\n");
    return fails ? 1 : 0;
}
