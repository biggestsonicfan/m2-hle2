/*
 * snd_replay.c — replay a MAME capture's MIDI stream through the sound board
 * and record what the board does, for comparison with what MAME's board did.
 *
 * tools/mame/snd-capture.lua records, off a real sfight session, every byte
 * the i960 sent the sound UART with the 68000 clock period it arrived on. This
 * feeds those bytes to board/sound.h at the same sample, from power-on, and
 * writes the board's side in the same capture format (sound.h sndcap) plus the
 * audio as a WAV — so the i960 is taken out of the comparison and whatever
 * differs is the 68000, the SCSP or their timing.
 *
 * Usage: snd_replay <mame-capture-prefix> <out-prefix> [seconds]
 *   writes <out>.bin/.ram.bin/.regs.bin/.json and <out>.wav (44.1 kHz stereo)
 *   $ROMDIR (else $ROMS_DIR): directory with sfight.zip and schamp.zip
 *     (default: the claude_mame oracle's roms)
 *   $SND_SET: another game's board instead, a directory of region images
 *     (m2hle --export-roms; only audiocpu.bin and samples.bin are read)
 *   $SND_RAM_BASE: the .ram.bin dump's first address (default 0x1000)
 */
#define NDEBUG 1
#include "test_rom_dir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sfight.h"
#include "sound.h"

const game_profile_t *g_active_profile = &sfight_profile;

typedef struct { uint64_t t; uint8_t b; } midi_t;

/* $SND_TRACE=<file>: every 68000 instruction between $SND_TRACE_FROM and
 * $SND_TRACE_TO seconds as records of [u32 pc][u32 clock low][u32 clock high],
 * for tools/mame/m68k_timing_compare.py against MAME's trace. */
static FILE    *s_trace;
static uint64_t s_trace_from, s_trace_to;
static void trace_step(uint32_t pc, uint64_t cycles, void *ud) {
    (void)ud;
    if (cycles < s_trace_from || cycles >= s_trace_to) return;
    uint32_t r[3] = { pc, (uint32_t)cycles, (uint32_t)(cycles >> 32) };
    fwrite(r, 4, 3, s_trace);
}

static void wav_header(FILE *f, uint32_t frames) {
    uint32_t data = frames * 4, riff = 36 + data, fmt = 16, rate = SOUND_RATE, bps = SOUND_RATE * 4;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&bps, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

/* The capture IN.bin: its record count, its last clock period and its MIDI
 * bytes (the i960's writes to the UART's data port); 1 if it cannot be read. */
static int load_midi(const char *in, midi_t **midi_out, size_t *nmidi_out, size_t *nrec_out, uint64_t *t_last_out) {
    char path[1024];
    snprintf(path, sizeof path, "%s.bin", in);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long nbytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t nrec = (size_t)nbytes / 16;
    uint32_t *rec = malloc(nrec * 16);
    if (!rec || fread(rec, 16, nrec, f) != nrec) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    /* One slot per record is always enough, and a fixed cap is not: a long
     * capture silently loses every MIDI byte past it, which reads as the board
     * going quiet rather than as a truncated input. */
    midi_t *midi = malloc(sizeof *midi * (nrec ? nrec : 1));
    size_t nmidi = 0;
    uint64_t t_last = 0;
    for (size_t i = 0; i < nrec; i++) {
        uint32_t *r = rec + i * 4;
        if (r[2] > t_last) t_last = r[2];
        /* the UART's data port: 0x9C0000 on a 2B, 0x01C80000 on a 2A or 2C (24 bits kept) */
        if ((r[0] >> 24) == 1 && ((r[0] & 0xFFFFFF) == 0x9C0000 || (r[0] & 0xFFFFFF) == 0xC80000)) {
            midi[nmidi].t = r[2];
            midi[nmidi].b = (uint8_t)r[1];
            nmidi++;
        }
    }
    free(rec);
    *midi_out = midi;
    *nmidi_out = nmidi;
    *nrec_out = nrec;
    *t_last_out = t_last;
    return 0;
}

static romset_t rs;

/* The sound board from power-on, and the $SND_TRACE trace; 1 without the ROMs. */
static int load_board(const char *romdir) {
    char child[1024], parent[1024];
    snprintf(child, sizeof child, "%s/sfight.zip", romdir);
    snprintf(parent, sizeof parent, "%s/schamp.zip", romdir);
    const char *set = getenv("SND_SET");
    if ((set ? romset_load_dir(&rs, set) : sfight_load(&rs, child, parent)) != 0 || !rs.audiocpu || !rs.samples) {
        fprintf(stderr, "ROM load failed (%s)\n", set ? set : romdir); return 1;
    }
    if (getenv("SND_HLE")) g_sound_hle_want = atoi(getenv("SND_HLE"));
    if (getenv("SND_SYNTH")) g_sound_synth_want = atoi(getenv("SND_SYNTH"));   /* the voices in C too (sound_synth.h) */
    if (getenv("SCSP_EXACT")) g_scsp_exact = atoi(getenv("SCSP_EXACT"));   /* the DSP every sample (scsp.h, "A quiet DSP") */
    sound_reset();
    sound_load_rom(rs.audiocpu, (uint32_t)rs.audiocpu_size);
    sound_load_samples(rs.samples, (uint32_t)rs.samples_size);
    if (getenv("SND_TRACE")) {
        s_trace = fopen(getenv("SND_TRACE"), "wb");
        s_trace_from = (uint64_t)(atof(getenv("SND_TRACE_FROM") ? getenv("SND_TRACE_FROM") : "0") * 11289600.0);
        s_trace_to   = (uint64_t)(atof(getenv("SND_TRACE_TO")   ? getenv("SND_TRACE_TO")   : "1e9") * 11289600.0);
        if (s_trace) g_sound_step_trace = trace_step;
    }
    return 0;
}

/* OUT's capture files and its WAV, the capture started; 1 if one cannot be
 * written. */
static int open_outputs(const char *out, uint64_t total, uint32_t (**marks_out)[4], FILE **wav_out) {
    char path[1024];
    const uint32_t frames_per_mark = SOUND_RATE * 1000u / 57524u;   /* MAME's 57.524 Hz frames */
    uint32_t nmarks_cap = (uint32_t)(total / frames_per_mark) + 4;
    uint32_t (*marks)[4] = calloc(nmarks_cap, sizeof *marks);
    snprintf(path, sizeof path, "%s.bin", out);      g_sndcap.f     = fopen(path, "wb");
    snprintf(path, sizeof path, "%s.ram.bin", out);  g_sndcap.ramf  = fopen(path, "wb");
    snprintf(path, sizeof path, "%s.regs.bin", out); g_sndcap.regsf = fopen(path, "wb");
    snprintf(path, sizeof path, "%s.wav", out);      FILE *wav      = fopen(path, "wb");
    if (!marks || !g_sndcap.f || !g_sndcap.ramf || !g_sndcap.regsf || !wav) { fprintf(stderr, "cannot write %s.*\n", out); return 1; }
    g_sndcap.marks = marks;
    if (getenv("SND_RAM_BASE")) g_sndcap.ram_base = (uint32_t)strtoul(getenv("SND_RAM_BASE"), NULL, 0);
    g_sndcap.want = nmarks_cap;
    g_sndcap.active = 1;
    wav_header(wav, 0);
    *marks_out = marks;
    *wav_out = wav;
    return 0;
}

/* TOTAL samples of the board, the MIDI fed at its samples and the audio into
 * WAV; the MIDI bytes fed. */
static size_t replay(const midi_t *midi, size_t nmidi, uint64_t total, FILE *wav) {
    const uint32_t frames_per_mark = SOUND_RATE * 1000u / 57524u;   /* MAME's 57.524 Hz frames */
    size_t next = 0;
    uint32_t mark = 0;
    int16_t block[4096 * 2];
    uint32_t nblock = 0;
    for (uint64_t smp = 0; smp < total; smp++) {
        /* bytes the i960 wrote during this sample go down the UART's line at
         * the clock period it wrote them */
        while (next < nmidi && (midi[next].t >> 8) <= smp) {
            sndcap_put(1, 0x9C0000, midi[next].b | 0xFF0000u, 0);
            sound_uart_write(&g_sound, midi[next].b, midi[next].t);
            next++;
        }
        if (smp % frames_per_mark == 0) sndcap_frame(mark++, 0);
        uint32_t w0 = g_sound.out_w;
        sound_run(1);
        if (g_sound.out_w != w0) {
            block[nblock * 2]     = g_sound.out[w0 * 2];
            block[nblock * 2 + 1] = g_sound.out[w0 * 2 + 1];
            nblock++;
        }
        g_sound.out_r = g_sound.out_w;               /* nobody else drains the ring here */
        if (nblock == 4096) { fwrite(block, 2, nblock * 2, wav); nblock = 0; }
    }
    if (nblock) fwrite(block, 2, nblock * 2, wav);
    return next;
}

static void write_json(const char *out, uint32_t (*marks)[4], uint32_t n, uint32_t nm) {
    char path[1024];
    snprintf(path, sizeof path, "%s.json", out);
    FILE *m = fopen(path, "w");
    fprintf(m, "{\"source\":\"replay-snd\",\"records\":%u,\"ram_base\":4096,\"ram_size\":16384,\"regs_words\":536,\"marks\":[", n);
    for (uint32_t i = 0; i < nm; i++)
        fprintf(m, "%s[%u,%u,%u,%u]", i ? "," : "", marks[i][0], marks[i][1], marks[i][2], marks[i][3]);
    fprintf(m, "]}\n");
    fclose(m);
}

static void report(uint32_t n, uint32_t nm, size_t next) {
    if (g_shle.on) printf("sound HLE: %llu events, %llu key-ons, %llu unknown\n",
                          (unsigned long long)g_shle.events, (unsigned long long)g_shle.keyons, (unsigned long long)g_shle.unknown);
    if (g_shle.on) printf("sound HLE: %llu refill chunks\n", (unsigned long long)g_shle.chunks_total);
    else if (g_sound_hle_want) printf("sound HLE: driver not known (code hash %08X); the board ran\n", shle_code_hash());
    if (g_ssyn.on) printf("sound synth: %llu samples, %llu key-ons\n", (unsigned long long)g_ssyn.samples, (unsigned long long)g_ssyn.keyons);
    printf("replayed: %u records, %u marks; 68000 pc 0x%06X; irqs L1 %llu L2 %llu L3 %llu; midi fed %zu\n",
           n, nm, g_sound.m68k.cpu.pc, (unsigned long long)g_sound.irqs[1],
           (unsigned long long)g_sound.irqs[2], (unsigned long long)g_sound.irqs[3], next);
}

int main(int argc, char **argv) {
    g_sound_hle_want = 0;   /* the board unless SND_HLE=1: what the C driver is graded against */
    if (argc < 3) { fprintf(stderr, "usage: snd_replay <mame-capture-prefix> <out-prefix> [seconds]\n"); return 2; }
    const char *in = argv[1], *out = argv[2];
    double seconds = argc > 3 ? atof(argv[3]) : 0.0;
    const char *romdir = test_rom_dir();

    midi_t *midi;
    size_t nmidi, nrec;
    uint64_t t_last;
    if (load_midi(in, &midi, &nmidi, &nrec, &t_last)) return 1;
    uint64_t total = seconds > 0 ? (uint64_t)(seconds * SOUND_RATE) : (t_last >> 8) + 1;
    printf("capture: %zu records, %zu MIDI bytes, %.2f s; replaying %.2f s\n",
           nrec, nmidi, (double)t_last / 11289600.0, (double)total / SOUND_RATE);

    if (load_board(romdir)) return 1;
    uint32_t (*marks)[4];
    FILE *wav;
    if (open_outputs(out, total, &marks, &wav)) return 1;

    size_t next = replay(midi, nmidi, total, wav);
    uint32_t n = g_sndcap.n, nm = g_sndcap.nmarks;
    sndcap_stop();
    fseek(wav, 0, SEEK_SET);
    wav_header(wav, (uint32_t)total);
    fclose(wav);

    write_json(out, marks, n, nm);
    report(n, nm, next);
    if (s_trace) fclose(s_trace);
    romset_free(&rs);
    return 0;
}
