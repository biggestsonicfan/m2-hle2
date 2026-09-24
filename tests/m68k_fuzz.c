/*
 * m68k_fuzz.c -- the whole sound board running random code, for holding two
 * builds of m68k_exec.h / sound.h to the same bits.
 *
 * snd_replay runs only the instructions STF's driver uses. This loads the real
 * program ROM and samples, fills sound RAM with random words (a third of the
 * scenarios with words taken from the driver's ROM, a third dense in the
 * forms m68k_step runs fast), aims every vector into that code and the address
 * registers at each region of the map and at 64 KB page ends, and runs the
 * board a sample at a time: 68000, SCSP and interrupts, as sound_run does. It
 * writes each sample's register hash, PC, SR, clock and output, and each
 * scenario's sound RAM hash, to one file. Build it from both trees and cmp.
 *
 * Usage: m68k_fuzz <out-file> [scenarios=600] [samples=3000]
 *   $ROMDIR: directory with sfight.zip and schamp.zip
 *
 * Not a ctest.
 *
 * Every rnd() call is sequenced (never two in one argument list or operand
 * pair), so the stream is the same under every compiler and two builds from
 * different compilers can be compared too.
 */
#define NDEBUG 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sfight.h"
#include "sound.h"

const game_profile_t *g_active_profile = &sfight_profile;

static uint32_t rs = 1;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }
static uint32_t rnd32(void) { uint32_t hi = rnd() << 16; return hi ^ rnd(); }

static FILE *out;
static uint32_t buf[1 << 16]; static int nb;
static void put(uint32_t v) { buf[nb++] = v; if (nb == 1 << 16) { fwrite(buf, 4, nb, out); nb = 0; } }

static uint32_t interesting(void) {
    static const uint32_t base[] = {
        0x000000, 0x010000, 0x07FFF0, 0x0FFFF0, 0x100000, 0x100400, 0x100FF0, 0x400000,
        0x5FFFF0, 0x600000, 0x60FFF0, 0x67FFF0, 0x680000, 0x7FFFF0, 0x800000, 0x80FFF0,
        0x9FFFF0, 0xA00000, 0xDFFFF0, 0xE00000, 0xFFFFF0, 0xFF0000 };
    uint32_t b = base[rnd() % (sizeof base / sizeof base[0])];
    return (b + (rnd() % 32)) & 0xFFFFFF;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: m68k_fuzz <out-file> [scenarios] [samples]\n"); return 2; }
    out = fopen(argv[1], "wb");
    if (!out) { fprintf(stderr, "cannot write %s\n", argv[1]); return 1; }
    int scenarios = argc > 2 ? atoi(argv[2]) : 600;
    int samples = argc > 3 ? atoi(argv[3]) : 3000;
    const char *romdir = getenv("ROMDIR");
    if (!romdir) romdir = "c:/Users/bigge/source/repos/ai/claude_mame/mame/roms";
    char child[1024], parent[1024];
    snprintf(child, sizeof child, "%s/sfight.zip", romdir);
    snprintf(parent, sizeof parent, "%s/schamp.zip", romdir);
    static romset_t rset;
    if (sfight_load(&rset, child, parent)) { fprintf(stderr, "ROM load failed (%s)\n", romdir); return 1; }
    sound_reset();
    sound_load_rom(rset.audiocpu, (uint32_t)rset.audiocpu_size);
    sound_load_samples(rset.samples, (uint32_t)rset.samples_size);
    unsigned long long cyc = 0, halts = 0;
    for (int sc = 0; sc < scenarios; sc++) {
        rs = 0x85EBCA6Bu * (uint32_t)(sc + 1);
        sound_reset();
        for (uint32_t i = 0x400; i < SOUND_RAM_SIZE; i++) g_sound.ram[i] = (uint8_t)rnd();
        /* half the scenarios run mostly real instructions: RAM seeded with
         * words from the driver's own ROM */
        if (sc % 3 == 2)                                /* dense in the fast forms */
            for (uint32_t i = 0x400; i < SOUND_RAM_SIZE; i += 2) {
                uint32_t w, k = rnd() % 10, r1 = rnd() & 7, r2 = rnd() & 7;
                switch (k) {
                case 0: w = 0x4EFA; break;
                case 1: w = 0x6000 | (rnd() % 16) << 8; w |= rnd() % 4 ? rnd() & 0xFF : 0; break;
                case 2: w = 0x1010 | r1 << 9 | r2; break;
                case 3: w = 0x1168 | r1 << 9 | r2; break;
                case 4: w = 0xD0FC | r1 << 9; break;
                case 5: w = 0xE098 | r1 << 9 | r2; break;
                default: w = rnd() & 0xFFFF; break;      /* extension words and anything else */
                }
                g_sound.ram[i] = (uint8_t)(w >> 8); g_sound.ram[i + 1] = (uint8_t)w;
            }
        else if (sc & 1)
            for (uint32_t i = 0x400; i < SOUND_RAM_SIZE; i += 2) {
                uint32_t o = (rnd() % (M68K_ROM_SIZE / 2)) * 2;
                g_sound.ram[i] = g_sound.rom[o]; g_sound.ram[i + 1] = g_sound.rom[o + 1];
            }
        for (uint32_t v = 0; v < 0x100; v++) {          /* every vector into RAM code */
            uint32_t t = (0x400 + (rnd() % (SOUND_RAM_SIZE - 0x800))) & ~1u;
            g_sound.ram[v * 4] = 0; g_sound.ram[v * 4 + 1] = (uint8_t)(t >> 16);
            g_sound.ram[v * 4 + 2] = (uint8_t)(t >> 8); g_sound.ram[v * 4 + 3] = (uint8_t)t;
        }
        m68k_cpu_t *c = &g_sound.m68k.cpu;
        for (int i = 0; i < 8; i++) c->d[i] = rnd() & 1 ? rnd32() : interesting();
        for (int i = 0; i < 7; i++) c->a[i] = interesting();
        c->a[7] = c->ssp = 0x40000 + (rnd() & 0x3FFFC);
        c->usp = 0x20000 + (rnd() & 0x1FFFC);
        c->pc = (0x400 + (rnd() % (SOUND_RAM_SIZE - 0x800))) & ~1u;
        c->sr = (uint16_t)(0x2000 | (rnd() & 0x071F));
        c->halted = c->stopped = 0;
        scsp_write(&g_sound.scsp, 0x424, (uint16_t)rnd(), 2);   /* interrupt levels */
        scsp_write(&g_sound.scsp, 0x41E, (uint16_t)rnd(), 2);   /* and enables */
        for (int n = 0; n < samples && !c->halted; n++) {
            if (rnd() % 64 == 0) scsp_midi_in(&g_sound.scsp, (uint8_t)rnd());
            if (rnd() % 256 == 0) { int r = (int)(rnd() % 7); c->a[r] = interesting(); }   /* keep aiming at the edges */
            sound_run(1);
            g_sound.out_r = g_sound.out_w;
            uint32_t h = 0x811C9DC5u;
            for (int i = 0; i < 8; i++) { h = (h ^ c->d[i]) * 16777619u; h = (h ^ c->a[i]) * 16777619u; }
            put(h); put(c->pc); put(c->sr); put((uint32_t)c->cycles);
            put((uint32_t)(uint16_t)g_sound.out[((g_sound.out_w - 1) & (SOUND_OUT_FRAMES - 1)) * 2]);
        }
        halts += c->halted != 0;
        cyc += c->cycles;
        uint32_t h = 0x811C9DC5u;
        for (uint32_t i = 0; i < SOUND_RAM_SIZE; i++) h = (h ^ g_sound.ram[i]) * 16777619u;
        put(h);
    }
    fwrite(buf, 4, nb, out);
    fclose(out);
    fprintf(stderr, "68000 clocks %llu (%.1f M instructions at ~6 clocks), halted scenarios %llu of %d\n",
            cyc, cyc / 6e6, halts, scenarios);
    return 0;
}
