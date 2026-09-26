/*
 * snd_lockstep.c -- a sound capture through the real sound board, with
 * Mednafen's SCSP beside scsp.h or in its place.
 *
 * snd_replay feeds a capture's MIDI bytes (tools/mame/snd-capture.lua, or
 * tools/snd_stimuli.py) to board/sound.h at the clock period they arrived
 * on. This does the same, and hooks the 68000's bus so a second chip sees
 * the driver too:
 *
 *   --chip mirror    (default) the board is unchanged: the 68000 runs against
 *                    scsp.h, bit for bit what snd_replay does. Mednafen's chip
 *                    is kept in lockstep beside it -- every register write
 *                    and read (reads have side effects: the MIDI buffer pops),
 *                    every sound RAM write, every MIDI byte, one sample per
 *                    board sample. So both chips play the SAME register
 *                    traffic and only the chips differ. Whatever Mednafen
 *                    would have answered a read is compared, not used.
 *
 *   --chip mednafen  the 68000 runs against Mednafen's chip: its reads come
 *                    from Mednafen and its interrupt level is Mednafen's, so
 *                    the driver reacts to Mednafen's timers, monitor and
 *                    MIDI. This is the whole board with the other chip, to
 *                    set beside MAME's WAV. Sound RAM stays the 68000's own
 *                    (the board's array), mirrored into Mednafen's on every
 *                    write; the DSP's delay-line writes are not copied back,
 *                    which the driver never reads.
 *
 * Writes <out>.wav (the chip the board ran on; in mirror mode scsp.h) and in
 * mirror mode <out>.mednafen.wav, and prints the time each chip took and,
 * per 5 s, both chips' loudness and the difference between them.
 * tools/scsp_mednafen/wav_compare.py holds either WAV against MAME's.
 *
 *   --chip scsp      scsp.h alone, no hooks: snd_replay's board, for timing
 *                    against --chip mednafen.
 *
 * Usage: snd_lockstep <capture-prefix> <out-prefix> [seconds] [--chip mirror|mednafen|scsp]
 *   $ROMDIR: the directory with sfight.zip and schamp.zip
 */
#define NDEBUG 1
#define _POSIX_C_SOURCE 199309L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sfight.h"
#include "sound.h"
#include "mdfn_scsp.h"

const game_profile_t *g_active_profile = &sfight_profile;

#define REG 0x100000u
enum { MIRROR, MDFN, SCSP_ONLY };
static int mode = MIRROR;              /* --chip */
#define use_mdfn (mode == MDFN)
static unsigned long long midi_dropped;
static unsigned long long nreads, nread_bad, read_bad_at[0x30];

static double secs(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static uint32_t (*orig_read)(void *, uint32_t, int);
static void (*orig_write)(void *, uint32_t, uint32_t, int);

static inline int is_scsp(uint32_t a) { return a >= M68K_SCSP_BASE && a < M68K_SCSP_BASE + M68K_SCSP_SIZE; }

/* A long is two words on this chip, as scsp_read/scsp_write take it. */
static uint32_t mdfn_rd(uint32_t off, int sz) {
    if (sz == 4) return mdfn_scsp_read(REG + off, 2) << 16 | mdfn_scsp_read(REG + off + 2, 2);
    return mdfn_scsp_read(REG + off, sz);
}
static void mdfn_wr(uint32_t off, uint32_t v, int sz) {
    if (sz == 4) { mdfn_scsp_write(REG + off, v >> 16, 2); mdfn_scsp_write(REG + off + 2, v & 0xFFFF, 2); }
    else mdfn_scsp_write(REG + off, v, sz);
}

static uint32_t hook_read(void *ctx, uint32_t addr, int sz) {
    addr &= 0xFFFFFFu;
    if (!is_scsp(addr)) return orig_read(ctx, addr, sz);
    uint32_t off = addr - M68K_SCSP_BASE;
    uint32_t b = mdfn_rd(off, sz);
    if (use_mdfn) return b;
    uint32_t a = orig_read(ctx, addr, sz);
    nreads++;
    if (a != b) { nread_bad++; if (off < 0x460 && off >= 0x400) read_bad_at[(off - 0x400) >> 1]++; }
    return a;
}

static void hook_write(void *ctx, uint32_t addr, uint32_t val, int sz) {
    addr &= 0xFFFFFFu;
    if (is_scsp(addr)) {
        mdfn_wr(addr - M68K_SCSP_BASE, val, sz);
        if (use_mdfn) return;                   /* scsp.h is not in this board */
    } else if (addr + (uint32_t)sz <= 0x080000u) {
        /* Sound RAM, into Mednafen's words: bytes are big-endian on the bus. */
        uint16_t *mr = mdfn_scsp_ram();
        for (int i = 0; i < sz; i++) {
            uint32_t a = addr + (uint32_t)i;
            uint8_t b = (uint8_t)(val >> (8 * (sz - 1 - i)));
            uint16_t *w = &mr[a >> 1];
            *w = (a & 1) ? (uint16_t)((*w & 0xFF00) | b) : (uint16_t)((*w & 0x00FF) | (b << 8));
        }
    }
    orig_write(ctx, addr, val, sz);
}

/* sound_run's loop, for one sample, with the interrupt level taken from
 * Mednafen's chip instead of scsp.h's. Kept line for line with sound.h. */
static void run_sample_mdfn(int16_t *l, int16_t *r) {
    m68k_state_t *m = &g_sound.m68k;
    g_sound.budget += SOUND_CYCLES_PER_SAMPLE;
    while (g_sound.budget > 0) {
        if (m->cpu.halted) { g_sound.budget = 0; break; }
        uint64_t c0 = m->cpu.cycles;
        int lvl = mdfn_scsp_irq_level();
        int ipl = m68k_ipl(&m->cpu);
        if (lvl > ipl && lvl > g_sound.mask_seen && m68k_interrupt(m, lvl)) {
            g_sound.irqs[lvl]++;
        } else if (m->cpu.stopped) {
            g_sound.mask_seen = ipl;
            m->cpu.cycles += (uint64_t)g_sound.budget;
            g_sound.budget = 0;
            break;
        } else {
            g_sound.mask_seen = ipl;
            m68k_step(m);
            g_sound.budget -= (int32_t)(m->cpu.cycles - c0);
            continue;
        }
        g_sound.mask_seen = m68k_ipl(&m->cpu);
        g_sound.budget -= (int32_t)(m->cpu.cycles - c0);
    }
    mdfn_scsp_sample(l, r);
}

static void wav_header(FILE *f, uint32_t frames) {
    uint32_t data = frames * 4, riff = 36 + data, fmt = 16, rate = SOUND_RATE, bps = SOUND_RATE * 4;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&bps, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

typedef struct { uint64_t t; uint8_t b; } midi_t;

int main(int argc, char **argv) {
    const char *in = NULL, *out = NULL; double seconds = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--chip") && i + 1 < argc) {
            i++;
            mode = !strcmp(argv[i], "mednafen") ? MDFN : !strcmp(argv[i], "scsp") ? SCSP_ONLY : MIRROR;
        }
        else if (!in) in = argv[i];
        else if (!out) out = argv[i];
        else seconds = atof(argv[i]);
    }
    if (!in || !out) { fprintf(stderr, "usage: snd_lockstep <capture-prefix> <out-prefix> [seconds] [--chip mirror|mednafen|scsp]\n"); return 2; }
    const char *romdir = getenv("ROMDIR");
    if (!romdir) romdir = "c:/Users/bigge/source/repos/ai/claude_mame/mame/roms";

    char path[1024];
    snprintf(path, sizeof path, "%s.bin", in);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); long nbytes = ftell(f); fseek(f, 0, SEEK_SET);
    size_t nrec = (size_t)nbytes / 16;
    uint32_t *rec = malloc(nrec * 16 + 16);
    if (!rec || fread(rec, 16, nrec, f) != nrec) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);
    midi_t *midi = malloc(sizeof *midi * (nrec ? nrec : 1));
    size_t nmidi = 0; uint64_t t_last = 0;
    for (size_t i = 0; i < nrec; i++) {
        uint32_t *r = rec + i * 4;
        if (r[2] > t_last) t_last = r[2];
        if ((r[0] >> 24) == 1 && (r[0] & 0xFFFFFF) == 0x9C0000) { midi[nmidi].t = r[2]; midi[nmidi].b = (uint8_t)r[1]; nmidi++; }
    }
    free(rec);
    uint64_t total = seconds > 0 ? (uint64_t)(seconds * SOUND_RATE) : (t_last >> 8) + 1;

    static romset_t rs;
    char child[1024], parent[1024];
    snprintf(child, sizeof child, "%s/sfight.zip", romdir);
    snprintf(parent, sizeof parent, "%s/schamp.zip", romdir);
    if (sfight_load(&rs, child, parent) != 0 || !rs.audiocpu || !rs.samples) { fprintf(stderr, "ROM load failed (%s)\n", romdir); return 1; }
    sound_reset();
    sound_load_rom(rs.audiocpu, (uint32_t)rs.audiocpu_size);
    sound_load_samples(rs.samples, (uint32_t)rs.samples_size);
    mdfn_scsp_reset();
    {   /* the same RAM at power-on (sound_reset copied the vectors into it) */
        uint16_t *mr = mdfn_scsp_ram();
        for (uint32_t i = 0; i < SOUND_RAM_SIZE / 2; i++) mr[i] = (uint16_t)(g_sound.ram[2 * i] << 8 | g_sound.ram[2 * i + 1]);
    }
    if (mode != SCSP_ONLY) {
        orig_read = g_sound.m68k.read_cb;   g_sound.m68k.read_cb = hook_read;
        orig_write = g_sound.m68k.write_cb; g_sound.m68k.write_cb = hook_write;
    }

    printf("capture: %zu MIDI bytes, %.2f s; replaying %.2f s on %s\n", nmidi, (double)t_last / 11289600.0,
           (double)total / SOUND_RATE, mode == MDFN ? "Mednafen's chip" : mode == SCSP_ONLY ? "scsp.h alone" : "scsp.h, Mednafen's chip in lockstep");

    snprintf(path, sizeof path, "%s.wav", out); FILE *wa = fopen(path, "wb");
    FILE *wb = NULL;
    if (mode == MIRROR) { snprintf(path, sizeof path, "%s.mednafen.wav", out); wb = fopen(path, "wb"); }
    if (!wa || (mode == MIRROR && !wb)) { fprintf(stderr, "cannot write %s.*\n", out); return 1; }
    wav_header(wa, 0); if (wb) wav_header(wb, 0);

    const uint64_t win = 5 * SOUND_RATE;
    /* Per window, per chip: the sum and the sum of squares (for the DC and
     * the level without it) and the cross term. The board's output carries a
     * DC offset (CLAUDE.md: ~5000, from the DSP path, in MAME's WAV too), so
     * levels and the error are taken with each chip's own mean removed, and
     * the error after the one gain that best maps scsp.h onto Mednafen. */
    double ma = 0, mb = 0, sa = 0, sb = 0, sab = 0;
    size_t next = 0;
    double t0 = secs();
    if (mode == MIRROR)
        printf("%8s %8s %8s %10s %10s %8s %9s %9s\n", "seconds", "ours DC", "mdfn DC", "ours dBFS", "mdfn dBFS",
               "gain dB", "residual", "corr");
    for (uint64_t smp = 0; smp < total; smp++) {
        while (next < nmidi && (midi[next].t >> 8) <= smp) {
            if (mode != MDFN) scsp_midi_in(&g_sound.scsp, midi[next].b);
            if (mode != SCSP_ONLY && !mdfn_scsp_midi_in(midi[next].b)) midi_dropped++;
            next++;
        }
        int16_t al = 0, ar = 0, bl, br;
        if (use_mdfn) {
            run_sample_mdfn(&bl, &br);
            al = bl; ar = br;
        } else {
            uint32_t w0 = g_sound.out_w;
            sound_run(1);
            if (g_sound.out_w != w0) { al = g_sound.out[w0 * 2]; ar = g_sound.out[w0 * 2 + 1]; }
            g_sound.out_r = g_sound.out_w;
            if (mode == MIRROR) mdfn_scsp_sample(&bl, &br); else bl = br = 0;
        }
        int16_t pa[2] = {al, ar}, pb[2] = {bl, br};
        fwrite(pa, 2, 2, wa);
        if (wb) {
            fwrite(pb, 2, 2, wb);
            ma += (double)al + ar; mb += (double)bl + br;
            sa += (double)al * al + (double)ar * ar; sb += (double)bl * bl + (double)br * br;
            sab += (double)al * bl + (double)ar * br;
            if ((smp + 1) % win == 0) {
                double n = 2.0 * win, da = ma / n, db = mb / n;
                double va = sa / n - da * da, vb = sb / n - db * db, cab = sab / n - da * db;
                double g = va > 0 ? cab / va : 0.0;                          /* best gain, ours -> Mednafen */
                double res = vb > 0 ? sqrt(fmax(0.0, vb - g * cab) / vb) : 0.0; /* what the gain leaves, of Mednafen */
                printf("%8.0f %8.0f %8.0f %10.1f %10.1f %+8.1f %9.3f %9.4f\n", (double)(smp + 1) / SOUND_RATE, da, db,
                       10 * log10(va / (32768.0 * 32768.0) + 1e-12), 10 * log10(vb / (32768.0 * 32768.0) + 1e-12),
                       g > 0 ? 20 * log10(g) : 0.0, res, va > 0 && vb > 0 ? cab / sqrt(va * vb) : 0.0);
                ma = mb = sa = sb = sab = 0;
            }
        }
    }
    double t_all = secs() - t0;
    fseek(wa, 0, SEEK_SET); wav_header(wa, (uint32_t)total); fclose(wa);
    if (wb) { fseek(wb, 0, SEEK_SET); wav_header(wb, (uint32_t)total); fclose(wb); }
    double audio = (double)total / SOUND_RATE;
    printf("time: %.2f s for %.1f s of audio, %.1fx real time\n", t_all, audio, audio / t_all);
    if (mode == MIRROR) {
        printf("reads: %llu, %llu answered differently by Mednafen; by register:", nreads, nread_bad);
        for (int i = 0; i < 0x30; i++) if (read_bad_at[i]) printf(" 0x%03X %llu", 0x400 + i * 2, read_bad_at[i]);
        printf("\n");
    }
    if (mode != SCSP_ONLY) printf("MIDI bytes Mednafen's 4-byte input FIFO dropped: %llu of %zu\n", midi_dropped, next);
    printf("68000: irqs L1 %llu L2 %llu L3 %llu L4 %llu L5 %llu; midi fed %zu\n",
           (unsigned long long)g_sound.irqs[1], (unsigned long long)g_sound.irqs[2], (unsigned long long)g_sound.irqs[3],
           (unsigned long long)g_sound.irqs[4], (unsigned long long)g_sound.irqs[5], next);
    romset_free(&rs);
    return 0;
}
