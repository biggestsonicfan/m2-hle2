/*
 * scsp_fuzz.c -- the SCSP alone, under random register traffic, for holding
 * two builds of scsp.h to the same bits.
 *
 * snd_replay grades the chip through a real driver, and STF's driver never
 * uses FM, the noise source, reverse or alternating loops or 8-bit samples.
 * This drives every slot field at random instead -- FM, noise, all four loop
 * kinds, 8/16-bit, both LFOs, EG hold and link, key on/off strobes, byte writes
 * -- with timers, DMA, the MIDI ring, the monitor, sound RAM changing under
 * the chip, and DSP programs (random ones, and the first compiled one in
 * scsp_dsp_known.h, which a third of the scenarios load). It writes every
 * output sample, every value read back, the interrupt level each sample and
 * the chip's end state to one file. Build it from both trees and cmp the
 * files; the coverage line goes to stderr.
 *
 * Usage: scsp_fuzz <out-file> [scenarios=200] [samples=20000] [mpro-dump]
 *   mpro-dump: MPRO as 512 little-endian words, for a tree without
 *   scsp_dsp_known.h (it must be the same program on both sides)
 *
 * No ROM: pure functions of the chip's state. Not a ctest.
 *
 * Every rnd() call is sequenced (never two in one argument list or operand
 * pair), so the stream is the same under every compiler and two builds from
 * different compilers can be compared too.
 */
#define NDEBUG 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scsp.h"

static uint32_t rs = 1;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }
static uint32_t rnd32(void) { uint32_t hi = rnd() << 16; return hi ^ rnd(); }

static FILE *out;
static uint32_t buf[1 << 16]; static int nb;
static void put(uint32_t v) { buf[nb++] = v; if (nb == 1 << 16) { fwrite(buf, 4, nb, out); nb = 0; } }

static unsigned long long cov[12];

static uint16_t known_prog[512];
static int have_known;

static void slot_write(scsp_t *s, int slot, int word, uint16_t v) {
    scsp_write(s, (uint32_t)(slot * 0x20 + word * 2), v, 2);
}

/* A slot set up the way a driver would, with every field drawn at random. */
static void random_voice(scsp_t *s, int sl) {
    uint32_t sa = rnd() & 0x7FFFF, lsa = rnd() & 0xFFFF, lea = rnd() & 0xFFFF;
    if (rnd() & 1) { lsa &= 0x0FFF; lea = lsa + (rnd() & 0x3FFF); }
    uint16_t w0 = (uint16_t)((sa >> 16) & 0xF);
    w0 |= (uint16_t)((rnd() & 3) << 5);                              /* LPCTL */
    if (rnd() % 4 == 0) w0 |= 0x0010;                                 /* PCM8B */
    w0 |= (uint16_t)((rnd() % 8 == 0 ? rnd() & 3 : 0) << 7);          /* SSCTL (noise sometimes) */
    w0 |= (uint16_t)((rnd() % 8 == 0 ? rnd() & 3 : 0) << 9);          /* SBCTL */
    slot_write(s, sl, 1, (uint16_t)sa);
    slot_write(s, sl, 2, (uint16_t)lsa);
    slot_write(s, sl, 3, (uint16_t)lea);
    slot_write(s, sl, 4, (uint16_t)rnd());                            /* D2R D1R EGHOLD AR */
    slot_write(s, sl, 5, (uint16_t)rnd());                            /* LPSLNK KRS DL RR */
    slot_write(s, sl, 6, (uint16_t)(rnd() & 0x3FF));                  /* STWINH SDIR TL */
    slot_write(s, sl, 7, (uint16_t)(rnd() % 3 == 0 ? rnd() : 0));     /* MDL MDXSL MDYSL (FM) */
    slot_write(s, sl, 8, (uint16_t)(rnd() & 0x7BFF));                 /* OCT FNS */
    slot_write(s, sl, 9, (uint16_t)(rnd() % 2 ? rnd() : 0));          /* LFO */
    slot_write(s, sl, 10, (uint16_t)(rnd() & 0x7F));                  /* ISEL IMXL */
    slot_write(s, sl, 11, (uint16_t)rnd());                           /* DISDL DIPAN EFSDL EFPAN */
    slot_write(s, sl, 0, (uint16_t)(w0 | 0x0800 | 0x1000));           /* KYONB + KYONEX */
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: scsp_fuzz <out-file> [scenarios] [samples] [mpro-dump]\n"); return 2; }
    out = fopen(argv[1], "wb");
    if (!out) { fprintf(stderr, "cannot write %s\n", argv[1]); return 1; }
    int scenarios = argc > 2 ? atoi(argv[2]) : 200;
    int samples = argc > 3 ? atoi(argv[3]) : 20000;
    if (argc > 4) {
        FILE *f = fopen(argv[4], "rb");
        if (f) { have_known = fread(known_prog, 2, 512, f) == 512; fclose(f); }
    }
#ifdef SCSP_DSP_COMPILED
    if (!have_known) { memcpy(known_prog, scsp_dsp_known[0].mpro, sizeof known_prog); have_known = 1; }
#endif
    static uint8_t ram[0x80000];
    static scsp_t s;
    uint64_t clock = 0;
    for (int sc = 0; sc < scenarios; sc++) {
        rs = 0x9E3779B9u * (uint32_t)(sc + 1);
        for (uint32_t i = 0; i < sizeof ram; i++) ram[i] = (uint8_t)rnd();
        scsp_reset(&s, ram, sizeof ram, &clock);
        clock = 0;
        scsp_write(&s, 0x400, 0x000F | (rnd() & 1 ? 0x0100 : 0), 2);   /* MVOL, DAC18B sometimes */
        scsp_write(&s, 0x402, (uint16_t)(rnd() & 0x1FF), 2);            /* RBL/RBP */
        /* a DSP program: the compiled one, or random, or none */
        int prog = (int)(rnd() % 3);
        for (int i = 0; i < 64; i++) scsp_write(&s, 0x700 + i * 2, (uint16_t)rnd(), 2);
        for (int i = 0; i < 32; i++) scsp_write(&s, 0x780 + i * 2, (uint16_t)rnd(), 2);
        if (prog == 0 && have_known) {
            for (int i = 0; i < 512; i++) scsp_write(&s, 0x800 + i * 2, known_prog[i], 2);
        } else if (prog == 1) {
            int len = 1 + (int)(rnd() % 128);
            for (int i = 0; i < 512; i++) {
                uint16_t w = i < len * 4 ? (uint16_t)rnd() : 0;
                if (i % 4 == 1 && ((w >> 6) & 0x3F) > 0x31 && rnd() % 16) w = (uint16_t)((w & ~0x0FC0u) | ((rnd() % 0x32) << 6));
                scsp_write(&s, 0x800 + i * 2, w, 2);
            }
        }
        scsp_write(&s, 0x800 + 0x3F0, (uint16_t)scsp_read(&s, 0xBF0, 2), 2);   /* start it */
        /* interrupt levels and enables */
        scsp_write(&s, 0x424, (uint16_t)rnd(), 2);
        scsp_write(&s, 0x426, (uint16_t)rnd(), 2);
        scsp_write(&s, 0x428, (uint16_t)rnd(), 2);
        scsp_write(&s, 0x41E, (uint16_t)(rnd() & 0x7FF), 2);
        for (int n = 0; n < samples; n++) {
            uint32_t ev = rnd() % 64;
            if (ev == 0) random_voice(&s, (int)(rnd() & 31));
            else if (ev == 1) { int sl = (int)(rnd() & 31), wd = (int)(rnd() % 12); slot_write(&s, sl, wd, (uint16_t)rnd()); }
            else if (ev == 2) { int sl = (int)(rnd() & 31); slot_write(&s, sl, 0, (uint16_t)((s.slot[sl].r[0] & ~0x0800u) | 0x1000)); } /* key off */
            else if (ev == 3) { scsp_write(&s, 0x408, (uint16_t)((rnd() & 31) << 11), 2); put(scsp_read(&s, 0x408, 2)); }
            else if (ev == 4) { uint32_t t = 0x418 + 2 * (rnd() % 3); scsp_write(&s, t, (uint16_t)(rnd() & 0x7FF), 2); }
            else if (ev == 5) { put(scsp_read(&s, 0x420, 2)); scsp_write(&s, 0x422, (uint16_t)rnd(), 2); }
            else if (ev == 6) { scsp_midi_in(&s, (uint8_t)rnd()); }
            else if (ev == 7) { put(scsp_read(&s, 0x404, 2)); }
            else if (ev == 8 && rnd() % 16 == 0) {                        /* DMA */
                scsp_write(&s, 0x412, (uint16_t)rnd(), 2);
                scsp_write(&s, 0x414, (uint16_t)rnd(), 2);
                scsp_write(&s, 0x416, (uint16_t)((rnd() & 0x6FFE) | 0x1000 | (prog == 0 ? 0x2000 : 0)), 2);
            }
            else if (ev == 9) { uint32_t a = 0x700 + (rnd() & 63) * 2; scsp_write(&s, a, (uint16_t)rnd(), 2); }
            else if (ev == 10) { uint32_t a = rnd() & 0xFFE; put(scsp_read(&s, a, rnd() & 1 ? 2 : 1)); }
            else if (ev == 11) { uint32_t a = rnd() & 0x3FF; scsp_write(&s, a, rnd(), 1); }       /* byte writes to slots */
            else if (ev == 12) { uint32_t a = rnd() & 0x7FFFE; ram[a] = (uint8_t)rnd(); } /* the 68000 writing sound RAM */
            clock += 256;
            scsp_timers(&s, clock);
            put((uint32_t)scsp_irq_level(&s));
            for (int i = 0; i < 32; i++) { scsp_slot_t *q = &s.slot[i]; if (!q->active) continue; cov[0]++;
                if (SCSP_MDL(q) || SCSP_MDXSL(q) || SCSP_MDYSL(q)) cov[1]++; if (SCSP_SSCTL(q) == 1) cov[2]++; if (SCSP_PCM8B(q)) cov[3]++;
                cov[4 + SCSP_LPCTL(q)]++; if (SCSP_PLFOS(q)) cov[8]++; if (SCSP_ALFOS(q)) cov[9]++; }
            if (!s.dsp.stopped) cov[10]++;
            int16_t l, r;
            scsp_sample(&s, &l, &r);
#ifdef SCSP_DSP_COMPILED
            if (s.dsp.known) cov[11]++;
#endif
            put((uint32_t)(uint16_t)l | (uint32_t)(uint16_t)r << 16);
        }
        for (int i = 0; i < 32; i++) {
            put(s.slot[i].cur); put((uint32_t)s.slot[i].vol); put(s.slot[i].active | s.slot[i].state << 8);
        }
        put(s.sous_ptr); put(s.dsp.dec);
        for (uint32_t i = 0; i < sizeof ram; i += 4) put(*(uint32_t *)(ram + i));
    }
    fwrite(buf, 4, nb, out);
    fclose(out);
    fprintf(stderr, "slot-samples %llu FM %llu noise %llu pcm8 %llu loop0-3 %llu %llu %llu %llu plfo %llu alfo %llu dsp-running %llu compiled %llu\n", cov[0],cov[1],cov[2],cov[3],cov[4],cov[5],cov[6],cov[7],cov[8],cov[9],cov[10],cov[11]);
    return 0;
}
