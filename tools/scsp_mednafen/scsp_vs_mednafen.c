/*
 * scsp_vs_mednafen.c -- scsp.h against Mednafen's SCSP, the same register
 * traffic into both, sample for sample.
 *
 * scsp_fuzz holds two builds of scsp.h to each other and snd_replay holds
 * the board to MAME; neither says where MAME's chip (which scsp.h follows)
 * and a second, independently written chip part ways. Mednafen's is written
 * from Saturn hardware tests (its source says which), so where the two
 * differ is where scsp.h is worth checking against a real board.
 *
 * Each PROBE isolates one feature -- a plain voice, pitch, the envelope,
 * level and pan, 8-bit samples, the loop modes, noise, SBCTL, both LFOs, FM,
 * many voices, the DSP, the timers and interrupts, the slot monitor, MIDI --
 * and runs it from SEEDS random draws. Both chips start from the same sound
 * RAM and see the same writes at the same sample. Per probe it reports how
 * many seeds matched exactly, where the first sample differed (median),
 * and the error: RMS of the difference over RMS of the signal (median and
 * worst), plus mismatches in what the chip reads back.
 *
 * Usage: scsp_vs_mednafen [seeds=40] [samples=8192] [probe]
 *        scsp_vs_mednafen --show <probe> <seed> [samples] [out.wav]
 *   --show prints the first differing samples and read-backs for one draw,
 *   and writes a WAV of it: left = scsp.h, right = Mednafen (left channels).
 *
 * Build: tools/scsp_mednafen/build.sh (fetches Mednafen; see README.md).
 */
#define NDEBUG 1
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scsp.h"
#include "mdfn_scsp.h"

#define REG 0x100000u                  /* where Mednafen maps the registers */
#define SETTLE 256                     /* samples left out of the settled error: the attack */

static uint32_t rs = 1;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }

static uint8_t m2ram[0x80000];
static scsp_t m2;
static uint64_t clk;

/* Both chips, the same access. */
static void w16(uint32_t off, uint16_t v) { scsp_write(&m2, off, v, 2); mdfn_scsp_write(REG + off, v, 2); }
static void w8(uint32_t off, uint8_t v) { scsp_write(&m2, off, v, 1); mdfn_scsp_write(REG + off, v, 1); }
static void slotw(int sl, int word, uint16_t v) { w16((uint32_t)(sl * 0x20 + word * 2), v); }

static int16_t *wa, *wb;               /* --show: both left channels */

/* A read-back, on both sides. */
typedef struct { int n, bad, first_at; uint32_t off, a, b; } reads_t;
static reads_t rd;
static int now;                        /* the sample being produced */
static void rbm(uint32_t off, int sz, uint32_t mask) {
    uint32_t a = scsp_read(&m2, off, sz) & mask, b = mdfn_scsp_read(REG + off, sz) & mask;
    rd.n++;
    if (a != b) {
        if (wa && rd.bad < 8)                                    /* --show: the first few */
            printf("  read 0x%03X at %6d: scsp.h %04X mednafen %04X\n", off, now, a, b);
        if (!rd.bad) { rd.first_at = now; rd.off = off; rd.a = a; rd.b = b; }
        rd.bad++;
    }
}
static void rb(uint32_t off, int sz) { rbm(off, sz, 0xFFFFFFFFu); }
static void midi(uint8_t b) { scsp_midi_in(&m2, b); mdfn_scsp_midi_in(b); }

/* -- the probes ------------------------------------------------------------- */

typedef struct {
    uint16_t w[12];                    /* slot words 0..11, word 0 without the key bits */
} voice_t;

/* A voice that plays: 16-bit, looping over a short span, full attack, no
 * decay, full level, centred, no LFO or FM. Probes change one thing. */
static voice_t plain(void) {
    voice_t v; memset(&v, 0, sizeof v);
    uint32_t sa = (rnd() & 0x3FFFF) & ~1u;
    uint16_t lsa = (uint16_t)(rnd() & 0x3FF), lea = (uint16_t)(lsa + 64 + (rnd() & 0x7FF));
    v.w[0] = (uint16_t)(((sa >> 16) & 0xF) | (1 << 5));         /* LPCTL normal */
    v.w[1] = (uint16_t)sa;
    v.w[2] = lsa;
    v.w[3] = lea;
    v.w[4] = 0x001F;                                             /* AR 31, no decay */
    v.w[5] = 0x001F;                                             /* RR 31 */
    v.w[6] = 0x0000;                                             /* TL 0 */
    v.w[8] = 0x0000;                                             /* OCT 0 FNS 0 */
    v.w[11] = 0xE000;                                            /* DISDL 7, centre */
    return v;
}
static void key_on(int sl, const voice_t *v) {
    for (int i = 1; i < 12; i++) slotw(sl, i, v->w[i]);
    slotw(sl, 0, (uint16_t)(v->w[0] | 0x0800 | 0x1000));
}
static void key_off(int sl, const voice_t *v) { slotw(sl, 0, (uint16_t)((v->w[0] & ~0x0800u) | 0x1000)); }

static void p_pitch(voice_t *v);
/* The envelope, one field at a time. KRS 0xF is "no key scaling". */
static void p_attack(voice_t *v) { v->w[4] = (uint16_t)(rnd() & 0x1F); v->w[5] = (uint16_t)(0x3C00 | 0x1F); }
static void p_krs(voice_t *v) { v->w[4] = (uint16_t)(8 + rnd() % 16); v->w[5] = (uint16_t)(((rnd() & 0xF) << 10) | 0x1F); p_pitch(v); }
static void p_decay(voice_t *v) { v->w[4] = (uint16_t)((rnd() & 0xFFC0) | 0x1F); v->w[5] = (uint16_t)(0x3C00 | (rnd() & 0x3E0) | 0x1F); }
static void p_release(voice_t *v) { v->w[5] = (uint16_t)(0x3C00 | (rnd() & 0x1F)); }
static void p_lpslnk(voice_t *v) { v->w[4] = (uint16_t)(rnd() & 0xFFDF); v->w[5] = (uint16_t)(0x4000 | 0x3C00 | (rnd() & 0x3FF)); }
static void p_pitch(voice_t *v) { v->w[8] = (uint16_t)(rnd() & 0x7BFF); }
static void p_eg(voice_t *v) { v->w[4] = (uint16_t)rnd(); v->w[5] = (uint16_t)(rnd() & 0x7FFF); }
static void p_level(voice_t *v) { v->w[6] = (uint16_t)(rnd() & 0xFF); v->w[11] = (uint16_t)(rnd() & 0xFF00); }
static void p_pcm8(voice_t *v) { v->w[0] |= 0x0010; }
static void p_loop(voice_t *v) {
    v->w[0] = (uint16_t)((v->w[0] & ~0x0060u) | ((rnd() & 3) << 5));
    v->w[2] = (uint16_t)(rnd() & 0xFFF); v->w[3] = (uint16_t)(v->w[2] + (rnd() & 0x3FFF));
    p_pitch(v);
}
static void p_noise(voice_t *v) { v->w[0] |= 0x0080; }
static void p_sbctl(voice_t *v) { v->w[0] |= (uint16_t)((rnd() & 3) << 9); }
static void p_plfo(voice_t *v) { v->w[9] = (uint16_t)(0x0000 | ((rnd() & 0x1F) << 10) | ((rnd() & 3) << 8) | ((1 + rnd() % 7) << 5)); }
static void p_alfo(voice_t *v) { v->w[9] = (uint16_t)(((rnd() & 0x1F) << 10) | ((rnd() & 3) << 3) | (1 + rnd() % 7)); }

typedef struct { const char *name; void (*tweak)(voice_t *); } vprobe_t;
static const vprobe_t VPROBES[] = {
    {"plain", 0}, {"pitch", p_pitch}, {"attack", p_attack}, {"key-scaling", p_krs},
    {"decay", p_decay}, {"release", p_release}, {"loop-link", p_lpslnk},
    {"envelope", p_eg}, {"level-pan", p_level},
    {"pcm8", p_pcm8}, {"loop-modes", p_loop}, {"noise", p_noise}, {"sbctl", p_sbctl},
    {"pitch-lfo", p_plfo}, {"amp-lfo", p_alfo},
};
#define NV (int)(sizeof VPROBES / sizeof VPROBES[0])
static const char *OTHER[] = {"fm", "many-voices", "dsp", "dsp-random", "timers-irq", "monitor", "midi", "fuzz"};
#define NPROBES (NV + 8)
static const char *probe_name(int p) { return p < NV ? VPROBES[p].name : OTHER[p - NV]; }

/* -- one draw ------------------------------------------------------------------ */

typedef struct { int exact, first; double err, settled, gain_db, peak; int irq_bad, irq_first; } result_t;


static void setup_common(void) {
    for (uint32_t i = 0; i < sizeof m2ram; i++) m2ram[i] = (uint8_t)rnd();
    scsp_reset(&m2, m2ram, sizeof m2ram, &clk);
    clk = 0;
    mdfn_scsp_reset();
    uint16_t *mr = mdfn_scsp_ram();
    for (uint32_t i = 0; i < sizeof m2ram / 2; i++) mr[i] = (uint16_t)(m2ram[2 * i] << 8 | m2ram[2 * i + 1]);
    w16(0x400, 0x000F);                                          /* MVOL 15 */
    memset(&rd, 0, sizeof rd);
}

/* A DSP program: STF's own reverb (the first in scsp_dsp_known.h) with random
 * coefficients and delay taps, or a random one. Random programs with random
 * coefficients are mostly silent, so the known one is the useful probe. */
static void random_dsp(int known) {
    w16(0x402, (uint16_t)(rnd() & 0x17F));                       /* RBL/RBP, low in RAM */
    for (int i = 0; i < 64; i++) w16(0x700 + (uint32_t)i * 2, (uint16_t)rnd());
    for (int i = 0; i < 32; i++) w16(0x780 + (uint32_t)i * 2, (uint16_t)rnd());
#if SCSP_DSP_COMPILED
    if (known) {
        for (int i = 0; i < 512; i++) w16(0x800 + (uint32_t)i * 2, scsp_dsp_known[0].mpro[i]);
        return;
    }
#else
    (void)known;
#endif
    int len = 4 + (int)(rnd() % 60);
    for (int i = 0; i < 512; i++) {
        uint16_t w = i < len * 4 ? (uint16_t)rnd() : 0;
        if (i % 4 == 1 && ((w >> 6) & 0x3F) > 0x31) w = (uint16_t)((w & ~0x0FC0u) | ((rnd() % 0x32) << 6));
        w16(0x800 + (uint32_t)i * 2, w);
    }
}

static result_t run(int probe, int seed, int samples) {
    uint32_t h = 2166136261u;                                   /* by name: adding a probe moves no seed */
    for (const char *c = probe_name(probe); *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    rs = 0x9E3779B9u * (uint32_t)(seed + 1) ^ h;
    setup_common();
    voice_t v = plain();
    int nvoice = 1;
    voice_t vs[8];
    if (probe < NV) { if (VPROBES[probe].tweak) VPROBES[probe].tweak(&v); key_on(0, &v); }
    const char *pn = probe_name(probe);
    if (!strcmp(pn, "fm")) {                                    /* slot 1 modulated by slot 0 */
        voice_t c = plain(); p_pitch(&v); p_pitch(&c);
        c.w[7] = (uint16_t)(((5 + rnd() % 11) << 12) | (0x20 << 6) | 0x20);  /* MDL, MDXSL=MDYSL=stack[-32]: slot 0 */
        key_on(0, &v); key_on(1, &c); vs[0] = c; nvoice = 2;
    } else if (!strcmp(pn, "many-voices")) {
        nvoice = 8;
        for (int i = 0; i < 8; i++) {
            vs[i] = plain(); p_pitch(&vs[i]); p_eg(&vs[i]); p_level(&vs[i]);
            if (rnd() & 1) p_loop(&vs[i]);
            if (rnd() % 4 == 0) p_pcm8(&vs[i]);
            key_on(i * 4, &vs[i]);
        }
    } else if (!strcmp(pn, "dsp") || !strcmp(pn, "dsp-random")) {
        random_dsp(!strcmp(pn, "dsp"));
        for (int i = 0; i < 4; i++) {                            /* sends into the DSP, and its outputs mixed */
            vs[i] = plain(); p_pitch(&vs[i]);
            vs[i].w[10] = (uint16_t)(((rnd() & 15) << 3) | (1 + rnd() % 7));   /* ISEL, IMXL */
            vs[i].w[11] = (uint16_t)(0x0000 | ((1 + rnd() % 7) << 5) | (rnd() & 0x1F));  /* EFSDL, EFPAN only */
            key_on(i, &vs[i]);
        }
        for (int i = 4; i < 16; i++) slotw(i, 11, (uint16_t)(((1 + rnd() % 7) << 5) | (rnd() & 0x1F)));
    } else if (!strcmp(pn, "timers-irq")) {
        w16(0x424, (uint16_t)rnd()); w16(0x426, (uint16_t)rnd()); w16(0x428, (uint16_t)rnd());
        w16(0x418, (uint16_t)(rnd() & 0x7FF)); w16(0x41A, (uint16_t)(rnd() & 0x7FF)); w16(0x41C, (uint16_t)(rnd() & 0x7FF));
        w16(0x41E, (uint16_t)(0x01C0 | (rnd() & 0x37)));        /* SCIEB: timers A-C and some others, not the
                                                                    * one-sample interrupt (bit 10), which scsp.h
                                                                    * never raises -- see README.md */
    } else if (!strcmp(pn, "monitor")) {
        p_eg(&v); p_pitch(&v); key_on(5, &v);
    }
    int16_t prev_irq_m2 = 0; (void)prev_irq_m2;
    result_t res = {1, -1, 0, 0, 0, 0, 0, -1};
    double se = 0, ss = 0, se2 = 0, ss2 = 0, sa = 0, sb = 0; int peak = 0;
    for (now = 0; now < samples; now++) {
        /* events, the same on both sides */
        if (probe < NV && now == samples * 3 / 4) key_off(0, &v);
        if (!strcmp(pn, "many-voices") && now > 0 && now % 997 == 0) { int i = (int)(rnd() % nvoice); key_off(i * 4, &vs[i]); }
        if (!strcmp(pn, "fm") && now == samples * 3 / 4) { key_off(0, &v); key_off(1, &vs[0]); }
        if (!strcmp(pn, "timers-irq")) {
            if (now % 7 == 0) rbm(0x420, 2, 0x01FF);            /* SCIPD, less the sample and MIDI-out bits */
            if (now % 211 == 0) w16(0x422, (uint16_t)rnd());     /* SCIRE: acknowledge some */
            if (now % 1499 == 0) w16(0x418 + 2 * (rnd() % 3), (uint16_t)(rnd() & 0x7FF));
        }
        /* MSLC before every read: scsp.h (MAME) resets it to slot 0 on a read,
         * and Mednafen latches the monitor as it produces a sample, so the
         * read comes a sample after the write. */
        if (!strcmp(pn, "monitor")) { if (now % 13 == 0) w16(0x408, (uint16_t)(5 << 11)); else if (now % 13 == 1) rb(0x408, 2); }
        if (!strcmp(pn, "midi")) {
            if (now % 3 == 0) midi((uint8_t)rnd());
            if (now % 5 == 0) rb(0x404, 2);
            if (now == 0) { w16(0x41E, 0x0008); w16(0x424, 0); w16(0x426, 0); w16(0x428, 0); }
        }
        if (!strcmp(pn, "fuzz")) {
            uint32_t ev = rnd() % 64;
            if (ev == 0) { int sl = (int)(rnd() & 31); voice_t q = plain(); p_pitch(&q); p_eg(&q); p_level(&q); p_loop(&q);
                if (rnd() % 4 == 0) p_pcm8(&q); if (rnd() % 6 == 0) p_noise(&q); if (rnd() % 3 == 0) p_plfo(&q);
                if (rnd() % 3 == 0) q.w[9] |= (uint16_t)(rnd() & 0x1F); key_on(sl, &q); }
            else if (ev == 1) { int sl = (int)(rnd() & 31), wd = 1 + (int)(rnd() % 11); slotw(sl, wd, (uint16_t)rnd()); }
            else if (ev == 2) { int sl = (int)(rnd() & 31); slotw(sl, 0, (uint16_t)((scsp_peek16(&m2, (uint32_t)sl * 0x20) & ~0x0800u) | 0x1000)); }
            else if (ev == 3) { w16(0x408, (uint16_t)((rnd() & 31) << 11)); rb(0x408, 2); }
            else if (ev == 4) { uint32_t a = rnd() & 0x3FF; w8(a, (uint8_t)rnd()); }                /* byte writes to slots */
        }
        clk += 256;
        scsp_timers(&m2, clk);
        int16_t al, ar, bl, br;
        scsp_sample(&m2, &al, &ar);
        mdfn_scsp_sample(&bl, &br);
        int ia = scsp_irq_level(&m2), ib = mdfn_scsp_irq_level();
        if (ia != ib) { if (res.irq_first < 0) res.irq_first = now; res.irq_bad++; }
        if (wa) { wa[now] = al; wb[now] = bl; }
        int dl = al - bl, dr = ar - br;
        if ((dl || dr) && res.first < 0) res.first = now;
        se += (double)dl * dl + (double)dr * dr;
        ss += (double)al * al + (double)ar * ar;
        sa += (double)al * al + (double)ar * ar;
        sb += (double)bl * bl + (double)br * br;
        if (now >= SETTLE && (probe >= NV || now < samples * 3 / 4)) {   /* past the attack, before key-off */
            se2 += (double)dl * dl + (double)dr * dr;
            ss2 += (double)al * al + (double)ar * ar;
        }
        if (abs(dl) > peak) peak = abs(dl);
        if (abs(dr) > peak) peak = abs(dr);
    }
    res.exact = res.first < 0 && !rd.bad && !res.irq_bad;
    res.err = ss > 0 ? sqrt(se / ss) : (se > 0 ? 1.0 : 0.0);
    res.settled = ss2 > 0 ? sqrt(se2 / ss2) : (se2 > 0 ? 1.0 : 0.0);
    res.gain_db = sa > 0 && sb > 0 ? 10.0 * log10(sb / sa) : 0.0;
    res.peak = peak;
    return res;
}

/* -- reporting ------------------------------------------------------------------ */

static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static int cmpi(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static void wav(const char *path, int n) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    uint32_t data = (uint32_t)n * 4, u; uint16_t h;
    fwrite("RIFF", 1, 4, f); u = 36 + data; fwrite(&u, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    u = 16; fwrite(&u, 4, 1, f); h = 1; fwrite(&h, 2, 1, f); h = 2; fwrite(&h, 2, 1, f);
    u = 44100; fwrite(&u, 4, 1, f); u = 44100 * 4; fwrite(&u, 4, 1, f); h = 4; fwrite(&h, 2, 1, f); h = 16; fwrite(&h, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (int i = 0; i < n; i++) { fwrite(&wa[i], 2, 1, f); fwrite(&wb[i], 2, 1, f); }
    fclose(f);
}

static int find_probe(const char *s) {
    for (int p = 0; p < NPROBES; p++) if (!strcmp(s, probe_name(p))) return p;
    fprintf(stderr, "no probe %s; probes:", s);
    for (int p = 0; p < NPROBES; p++) fprintf(stderr, " %s", probe_name(p));
    fprintf(stderr, "\n");
    exit(2);
}

/* --timers: when each timer first fires and how often after, on each side,
 * for a few prescales and reloads. The pending bit is acknowledged at once. */
static void timer_table(void) {
    printf("timer ctl  reload | scsp.h first  period | mednafen first  period\n");
    static const int reloads[] = {0x00, 0x80, 0xF0, 0xFE, 0xFF};
    for (int t = 0; t < 3; t++)
        for (int pre = 0; pre < 8; pre += 3)
            for (int k = 0; k < 5; k++) {
                rs = 1; setup_common();
                uint16_t bit = (uint16_t)(0x40 << t);
                w16(0x418 + 2 * (uint32_t)t, (uint16_t)(pre << 8 | reloads[k]));
                int fa[3] = {-1, -1, -1}, fb[3] = {-1, -1, -1}, na = 0, nb = 0;
                for (now = 0; now < 200000 && (na < 3 || nb < 3); now++) {
                    clk += 256; scsp_timers(&m2, clk);
                    int16_t l, r; scsp_sample(&m2, &l, &r); mdfn_scsp_sample(&l, &r);
                    if (na < 3 && (scsp_read(&m2, 0x420, 2) & bit)) { fa[na++] = now; scsp_write(&m2, 0x422, bit, 2); }
                    if (nb < 3 && (mdfn_scsp_read(REG + 0x420, 2) & bit)) { fb[nb++] = now; mdfn_scsp_write(REG + 0x422, bit, 2); }
                }
                printf("  %c    %d    0x%02X  | %8d %8d     | %8d %8d\n", 'A' + t, pre, reloads[k],
                       fa[0], fa[2] - fa[1], fb[0], fb[2] - fb[1]);
            }
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--timers")) { timer_table(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "--show")) {
        if (argc < 4) { fprintf(stderr, "usage: --show <probe> <seed> [samples] [out.wav]\n"); return 2; }
        int p = find_probe(argv[2]), seed = atoi(argv[3]), n = argc > 4 ? atoi(argv[4]) : 8192;
        wa = calloc((size_t)n, 2); wb = calloc((size_t)n, 2);
        result_t r = run(p, seed, n);
        printf("%s seed %d: %s, first differing sample %d, error %.4f (settled %.4f), level %+.2f dB, peak %g, reads %d/%d bad",
               probe_name(p), seed, r.exact ? "exact" : "differs", r.first, r.err, r.settled, r.gain_db, r.peak, rd.bad, rd.n);
        if (rd.bad) printf(" (first at sample %d: 0x%03X scsp.h %04X mednafen %04X)", rd.first_at, rd.off, rd.a, rd.b);
        printf(", irq %d bad (first %d)\n", r.irq_bad, r.irq_first);
        printf("  slot 0 words:"); for (int i = 0; i < 12; i++) printf(" %04X", scsp_peek16(&m2, (uint32_t)i * 2)); printf("\n");
        if (r.first >= 0)
            for (int i = r.first; i < r.first + 12 && i < n; i++) printf("  %6d  scsp.h %6d  mednafen %6d\n", i, wa[i], wb[i]);
        wav(argc > 5 ? argv[5] : "scsp_vs_mednafen.wav", n);
        return 0;
    }
    int seeds = argc > 1 ? atoi(argv[1]) : 40, samples = argc > 2 ? atoi(argv[2]) : 8192;
    int only = argc > 3 ? find_probe(argv[3]) : -1;
    printf("%-12s %7s %8s %8s %8s %8s %8s %11s %8s\n", "probe", "exact", "1st diff", "err med", "settled", "set max", "level dB", "reads bad", "irq bad");
    double *errs = malloc(sizeof(double) * (size_t)seeds), *sets = malloc(sizeof(double) * (size_t)seeds);
    int *firsts = malloc(sizeof(int) * (size_t)seeds);
    for (int p = 0; p < NPROBES; p++) {
        if (only >= 0 && p != only) continue;
        int exact = 0, nf = 0, rbad = 0, rn = 0, ibad = 0; double peak = 0, gain = 0;
        for (int s = 0; s < seeds; s++) {
            result_t r = run(p, s, samples);
            exact += r.exact; errs[s] = r.err; sets[s] = r.settled; gain += r.gain_db; if (r.peak > peak) peak = r.peak;
            if (r.first >= 0) firsts[nf++] = r.first;
            rbad += rd.bad; rn += rd.n; ibad += r.irq_bad > 0;
        }
        qsort(errs, (size_t)seeds, sizeof *errs, cmpd);
        qsort(sets, (size_t)seeds, sizeof *sets, cmpd);
        qsort(firsts, (size_t)nf, sizeof *firsts, cmpi);
        char fd[16], rbs[24];
        if (nf) snprintf(fd, sizeof fd, "%d", firsts[nf / 2]); else snprintf(fd, sizeof fd, "-");
        if (rn) snprintf(rbs, sizeof rbs, "%d/%d", rbad, rn); else snprintf(rbs, sizeof rbs, "-");
        (void)peak;
        printf("%-12s %3d/%-3d %8s %8.4f %8.4f %8.4f %+8.2f %11s %5d/%-3d\n", probe_name(p), exact, seeds, fd,
               errs[seeds / 2], sets[seeds / 2], sets[seeds - 1], gain / seeds, rbs, ibad, seeds);
        fflush(stdout);
    }
    return 0;
}
