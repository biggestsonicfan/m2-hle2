/*
 * snd_bench.c -- time the sound board, and the SCSP DSP on its own, over a MAME
 * capture's MIDI stream (the input tests/snd_replay.c uses), and print hashes
 * of everything the board produced so two builds can be held bit-identical.
 *
 * Usage: snd_bench <mame-capture-prefix> [seconds=30] [dsp_iters=400000]
 *   $ROMDIR: directory with sfight.zip and schamp.zip
 */
#define NDEBUG 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sfight.h"
#include "sound.h"

const game_profile_t *g_active_profile = &sfight_profile;

static double now_s(void) { struct timespec ts; timespec_get(&ts, TIME_UTC); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}
#define FNV0 0xCBF29CE484222325ull

typedef struct { uint64_t t; uint8_t b; } midi_t;

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: snd_bench <mame-capture-prefix> [seconds] [dsp_iters]\n"); return 2; }
    double seconds = argc > 2 ? atof(argv[2]) : 30.0;
    long dsp_iters = argc > 3 ? atol(argv[3]) : 400000;
    const char *romdir = getenv("ROMDIR");
    if (!romdir) romdir = "c:/Users/bigge/source/repos/ai/claude_mame/mame/roms";

    char path[1024];
    snprintf(path, sizeof path, "%s.bin", argv[1]);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); long nbytes = ftell(f); fseek(f, 0, SEEK_SET);
    size_t nrec = (size_t)nbytes / 16;
    uint32_t *rec = malloc(nrec * 16);
    if (!rec || fread(rec, 16, nrec, f) != nrec) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);
    midi_t *midi = malloc(sizeof *midi * (nrec ? nrec : 1));
    size_t nmidi = 0;
    for (size_t i = 0; i < nrec; i++) {
        uint32_t *r = rec + i * 4;
        if ((r[0] >> 24) == 1 && (r[0] & 0xFFFFFF) == 0x9C0000) { midi[nmidi].t = r[2]; midi[nmidi].b = (uint8_t)r[1]; nmidi++; }
    }
    free(rec);

    static romset_t rs;
    char child[1024], parent[1024];
    snprintf(child, sizeof child, "%s/sfight.zip", romdir);
    snprintf(parent, sizeof parent, "%s/schamp.zip", romdir);
    if (sfight_load(&rs, child, parent) != 0 || !rs.audiocpu || !rs.samples) { fprintf(stderr, "ROM load failed (%s)\n", romdir); return 1; }
    sound_reset();
    sound_load_rom(rs.audiocpu, (uint32_t)rs.audiocpu_size);
    sound_load_samples(rs.samples, (uint32_t)rs.samples_size);

    /* 1. The whole board, as the emulator runs it. */
    uint64_t total = (uint64_t)(seconds * SOUND_RATE), h_out = FNV0;
    size_t next = 0;
    double t0 = now_s();
    for (uint64_t smp = 0; smp < total; smp++) {
        while (next < nmidi && (midi[next].t >> 8) <= smp) { scsp_midi_in(&g_sound.scsp, midi[next].b); next++; }
        uint32_t w0 = g_sound.out_w;
        sound_run(1);
        if (g_sound.out_w != w0) h_out = fnv(h_out, &g_sound.out[w0 * 2], 4);
        g_sound.out_r = g_sound.out_w;
    }
    double t_board = now_s() - t0;
    scsp_t *s = &g_sound.scsp;
    uint64_t h_ram = fnv(FNV0, s->ram, s->ram_size);
    printf("board: %.2f s of sound in %.3f s  = %.2fx real time\n", seconds, t_board, seconds / t_board);
    printf("hash out %016llx  ram %016llx  dsp steps %d\n", (unsigned long long)h_out, (unsigned long long)h_ram, s->dsp.last_step);

    /* 2. The DSP alone, from the state the board reached. Its delay line is in
     * sound RAM, so this runs on the live state; nothing below is run again. */
    uint64_t h_ef = FNV0;
    t0 = now_s();
    for (long i = 0; i < dsp_iters; i++) {
        s->dsp.mixs[i & 15] = (int32_t)((i * 2654435761u) >> 12) & 0xFFFFF;   /* something to mix */
        scsp_dsp_step(s);
        h_ef = fnv(h_ef, s->dsp.efreg, sizeof s->dsp.efreg);
    }
    double t_dsp = now_s() - t0;
    uint64_t h_dsp = fnv(fnv(FNV0, s->dsp.temp, sizeof s->dsp.temp), s->ram, s->ram_size);
    printf("dsp:   %ld steps-of-program in %.3f s = %.1f ns each (%.1f ns per microcode step)\n",
           dsp_iters, t_dsp, t_dsp * 1e9 / dsp_iters, t_dsp * 1e9 / dsp_iters / (s->dsp.last_step ? s->dsp.last_step : 1));
    printf("hash efreg %016llx  dsp-state %016llx\n", (unsigned long long)h_ef, (unsigned long long)h_dsp);
    romset_free(&rs);
    return 0;
}
