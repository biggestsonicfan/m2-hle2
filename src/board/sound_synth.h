/*
 * sound_synth.h — the sound driver's voices played in C, in place of the
 * SCSP's slots (Pinboard #1737). Optional (--sound-synth, which takes the
 * driver in C with it): the chip stays the default and the oracle.
 *
 * With sound_hle.h the 68000 is gone and the driver is C; this takes the next
 * step and lets the driver's notes play straight out of C. The driver keeps
 * talking in the chip's terms -- a voice is a sample address, a loop, a pitch
 * (OCT/FNS), a level (TL), a pan and a send, an envelope and an LFO -- and
 * here each of those is what it means: a 20.12 step through the sample, a
 * gain, a release slope. Nothing of the chip's machinery is left: no lazy
 * slot runs and RAM-ownership granules, no slot monitor latch, no timers or
 * interrupts (sound_hle.h keeps the driver's clock), no sound stack, FM or
 * noise (STF's driver uses none of them; they are logged if they come).
 *
 * Parity. The arithmetic of a voice is MAME's scsp.cpp, which scsp.h follows
 * bit for bit: the pitch step, linear interpolation, the envelope (attack
 * linear, then 3/32 dB a step down eg_table), the LFO tables, the TL/pan/send
 * level tables and the 18-bit DAC with its master volume. So the synth makes
 * the same samples as the chip from the same driver, to the bit
 * (tools/grade-sound-synth.py: snd_replay with SND_HLE=1 against
 * SND_SYNTH=1), and the chip is MAME's: parity with MAME is parity with the
 * chip. Where it is not the same, the grader says so.
 *
 * The reverb. STF's reverb is a program the driver loads into the SCSP's DSP
 * (84 steps, scsp_dsp_known.h). The voices' sends (ISEL/IMXL) go into it and
 * its outputs (EFREG, through slots 0-15's EFSDL/EFPAN) come back onto the
 * mix, so it is kept as the effect unit it is: g_sound.scsp's DSP, its
 * registers written by the driver as before, run once a sample from here.
 *
 * Time. The driver works between samples (shle_run); the voices make their
 * samples late, in blocks, and catch up first whenever the driver writes a
 * voice or reads one back (the play position its streaming polls, KYONB).
 * The driver sees every voice exactly where the chip would have it.
 */
#ifndef SOUND_SYNTH_H
#define SOUND_SYNTH_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "log.h"
#include "scsp.h"

#define SSYN_BLOCK 1024u        /* samples a block holds at most */

/* An LFO: its phase, its rate, and which wave and depth it reads (indices,
 * so a savestate holds no pointers). */
typedef struct {
    uint16_t phase;
    uint16_t pad;
    uint32_t step;
    uint8_t  wave, depth;       /* 0..3 saw/square/triangle/noise; 1..7, 0 = none set yet */
    uint8_t  pad2[2];
} ssyn_lfo_t;

/* A voice. r[] is the slot's sixteen register words the way the driver wrote
 * them (the SCSP_* field macros read it): the driver reads some back, and
 * KYONB in word 0 drops when the voice ends, as on the chip. The rest is the
 * voice decoded. */
typedef struct {
    uint16_t   r[16];
    uint8_t    on;              /* sounding */
    uint8_t    state;           /* SCSP_ATTACK .. SCSP_RELEASE: the envelope's segment */
    uint8_t    backwards;       /* a reverse or alternating loop on its way back */
    uint8_t    pad;
    uint32_t   cur, nxt;        /* where it plays, and the sample after (both 20.12, from SA) */
    uint32_t   step;            /* the pitch: source samples per output sample, 20.12 */
    int32_t    vol;             /* the envelope, 0 .. 0x3FF << SCSP_EG_SHIFT */
    int32_t    ar, d1r, d2r, rr, dl;
    int32_t    eghold;
    ssyn_lfo_t plfo, alfo;
    int32_t    gl, gr;          /* direct out, left and right: TL, pan, DISDL */
    int32_t    gm;              /* the send into the reverb: TL, IMXL */
    int32_t    el, er;          /* the reverb's output it returns (slots 0-15): EFPAN, EFSDL */
} ssyn_voice_t;

typedef struct {
    bool         on;            /* the synth plays the voices (with sound_hle.h) */
    uint8_t      pad[3];
    uint32_t     pending;       /* samples of board time not mixed yet (the chip's `owed`) */
    uint16_t     done[32];      /* how far into them each voice has run */
    ssyn_voice_t v[32];
    uint64_t     samples, keyons;
    uint32_t     warned;        /* features met that are not here, bit per kind, logged once */
    uint32_t     pad2;
} ssyn_t;

static ssyn_t g_ssyn;
/* A host's switch, read at sound_reset: play the voices in C (needs the driver in C). */
static int g_sound_synth_want;

/* the block being made: each sample's direct mix and reverb sends, summed as
 * the voices run into it, cleared as it is mixed */
static int32_t ssyn_acc_l[SSYN_BLOCK], ssyn_acc_r[SSYN_BLOCK];
static int32_t ssyn_acc_mixs[SSYN_BLOCK][16];

enum { SSYN_W_FM = 1, SSYN_W_NOISE = 2, SSYN_W_STACK = 4 };

static void ssyn_warn(uint32_t kind, const char *what) {
    if (g_ssyn.warned & kind) return;
    g_ssyn.warned |= kind;
    LOG_WARN("sound synth: a voice uses %s, which the synth does not play", what);
}

/* ---- what a voice's registers mean ------------------------------------------------ */

/* The pitch. FNS is a fraction of an octave (1 + FNS/1024) and OCT a signed
 * octave, so a voice steps 2^OCT x (1 + FNS/1024) source samples per output
 * sample: OCT 0, FNS 0 plays a sample at the 44.1 kHz it was made at. */
static inline uint32_t ssyn_pitch(unsigned oct, unsigned fns) {
    int o = ((int)oct ^ 8) - 8;                         /* -8 .. 7 */
    uint32_t f = (1u << 10) + fns;                      /* 1.10 */
    int sh = o + SCSP_SHIFT - 10;
    return sh >= 0 ? f << sh : f >> -sh;
}

/* The gains: TL in dB steps, the pan's quieter side, the send levels
 * (scsp_lpan/scsp_rpan hold them as products, by TL | PAN << 8 | SDL << 13).
 * SDIR plays the raw sample, so TL drops out. */
static void ssyn_gains(ssyn_voice_t *v) {
    uint32_t tl = SCSP_SDIR(v) ? 0 : SCSP_TL(v);
    uint32_t dir = tl | SCSP_DIPAN(v) << 8 | SCSP_DISDL(v) << 13;
    uint32_t eff = SCSP_EFPAN(v) << 8 | SCSP_EFSDL(v) << 13;
    v->gl = scsp_lpan[dir];
    v->gr = scsp_rpan[dir];
    v->gm = scsp_lpan[tl | SCSP_IMXL(v) << 13];
    v->el = scsp_lpan[eff];
    v->er = scsp_rpan[eff];
}

/* The LFO rate (LFOF) and, where its depth is set, its wave. A depth of 0
 * leaves the wave it had: MAME's chip only takes one with a depth. */
static void ssyn_lfo(ssyn_voice_t *v) {
    float step = SCSP_LFO_HZ[SCSP_LFOF(v)] * 256.0f / 44100.0f;
    uint32_t phase_step = (uint32_t)((float)(1 << SCSP_LFO_SHIFT) * step);
    if (SCSP_PLFOS(v)) { v->plfo.step = phase_step; v->plfo.wave = (uint8_t)SCSP_PLFOWS(v); v->plfo.depth = (uint8_t)SCSP_PLFOS(v); }
    if (SCSP_ALFOS(v)) { v->alfo.step = phase_step; v->alfo.wave = (uint8_t)SCSP_ALFOWS(v); v->alfo.depth = (uint8_t)SCSP_ALFOS(v); }
}

/* the pitch LFO's factor this sample, 20.12 */
static inline int32_t ssyn_plfo(ssyn_lfo_t *l) {
    static const int32_t *const T[4] = { scsp_plfo_saw, scsp_plfo_sqr, scsp_plfo_tri, scsp_plfo_noi };
    l->phase = (uint16_t)(l->phase + l->step);
    return scsp_pscale[l->depth][T[l->wave][l->phase >> SCSP_LFO_SHIFT] + 128] << (SCSP_SHIFT - SCSP_LFO_SHIFT);
}

/* the amplitude LFO's factor this sample, 20.12 */
static inline int32_t ssyn_alfo(ssyn_lfo_t *l) {
    static const int32_t *const T[4] = { scsp_alfo_saw, scsp_alfo_sqr, scsp_alfo_tri, scsp_alfo_noi };
    l->phase = (uint16_t)(l->phase + l->step);
    return scsp_ascale[l->depth][T[l->wave][l->phase >> SCSP_LFO_SHIFT]] << (SCSP_SHIFT - SCSP_LFO_SHIFT);
}

static inline int32_t ssyn_rate(const int32_t *table, int base, int r) {
    int rate = base + (r << 1);
    return table[rate < 0 ? 0 : rate > 63 ? 63 : rate];
}

/* A key-on: from the sample's start, the envelope from its attack. Key rate
 * scaling raises every rate with the pitch (MAME: even rate 0). */
static void ssyn_key_on(ssyn_voice_t *v) {
    int octave = ((int)SCSP_OCT(v) ^ 8) - 8;
    int base = SCSP_KRS(v) != 0xF ? octave + 2 * (int)SCSP_KRS(v) + ((SCSP_FNS(v) >> 9) & 1) : 0;
    v->on        = 1;
    v->cur       = 0;
    v->nxt       = 1u << SCSP_SHIFT;
    v->step      = ssyn_pitch(SCSP_OCT(v), SCSP_FNS(v));
    v->ar        = ssyn_rate(scsp_ar_table, base, SCSP_AR(v));
    v->d1r       = ssyn_rate(scsp_dr_table, base, SCSP_D1R(v));
    v->d2r       = ssyn_rate(scsp_dr_table, base, SCSP_D2R(v));
    v->rr        = ssyn_rate(scsp_dr_table, base, SCSP_RR(v));
    v->dl        = 0x1F - (int32_t)SCSP_DL(v);
    v->eghold    = SCSP_EGHOLD(v) != 0;
    v->state     = SCSP_ATTACK;
    v->vol       = 0x17F << SCSP_EG_SHIFT;
    v->backwards = 0;
    ssyn_lfo(v);
    if (SCSP_MDL(v) || SCSP_MDXSL(v) || SCSP_MDYSL(v)) ssyn_warn(SSYN_W_FM, "FM");
    if (SCSP_SSCTL(v)) ssyn_warn(SSYN_W_NOISE, "a noise or zero source");
    g_ssyn.keyons++;
}

/* The voice lets go (into its release) or ends; either way KYONB drops,
 * which the driver reads. */
static inline void ssyn_stop(ssyn_voice_t *v, int release) {
    if (release) v->state = SCSP_RELEASE;
    else         v->on = 0;
    v->r[0] &= (uint16_t)~0x0800;
}

/* ---- making samples ------------------------------------------------------------------ */

/* The envelope this sample, 20.12: linear while it attacks, then down the
 * dB curve. */
static inline int32_t ssyn_env(ssyn_voice_t *v) {
    switch (v->state) {
    case SCSP_ATTACK:
        v->vol += v->ar;
        if (v->vol >= (0x3FF << SCSP_EG_SHIFT)) {
            if (!SCSP_LPSLNK(v)) v->state = v->d1r >= (1024 << SCSP_EG_SHIFT) ? SCSP_DECAY2 : SCSP_DECAY1;
            v->vol = 0x3FF << SCSP_EG_SHIFT;
        }
        if (v->eghold) return 0x3FF << (SCSP_SHIFT - 10);
        return (v->vol >> SCSP_EG_SHIFT) << (SCSP_SHIFT - 10);
    case SCSP_DECAY1:
        v->vol -= v->d1r;
        if (v->vol <= 0) v->vol = 0;
        if (v->vol >> (SCSP_EG_SHIFT + 5) <= v->dl) v->state = SCSP_DECAY2;
        break;
    case SCSP_DECAY2:
        if (SCSP_D2R(v) == 0) break;
        v->vol -= v->d2r;
        if (v->vol <= 0) v->vol = 0;
        break;
    default:
        v->vol -= v->rr;
        if (v->vol <= 0) { v->vol = 0; ssyn_stop(v, 0); }
        break;
    }
    return scsp_eg_table[v->vol >> SCSP_EG_SHIFT];
}

/* What a voice reads, fixed while it runs: its registers decoded once a run
 * rather than once a sample (the driver cannot write between). */
typedef struct {
    const uint8_t *ram;
    uint32_t sa, lsa, lea;
    int pcm8, sbctl, lpctl, lpslnk;
} ssyn_run_t;

static inline uint8_t ssyn_ram_b(const ssyn_run_t *c, uint32_t a) {
    a &= 0xFFFFFu;
    return a < SOUND_RAM_SIZE ? c->ram[a] : 0;
}

/* The sample where the voice stands, between the two it falls between:
 * 8-bit or 16-bit PCM out of sound RAM, then SBCTL's bit flips. */
static inline int32_t ssyn_wave(const ssyn_run_t *c, uint32_t cur, uint32_t nxt) {
    int32_t frac = (int32_t)(cur & ((1u << SCSP_SHIFT) - 1)), s;
    if (c->pcm8) {
        int32_t p1 = (int8_t)ssyn_ram_b(c, c->sa + (cur >> SCSP_SHIFT)), p2 = (int8_t)ssyn_ram_b(c, c->sa + (nxt >> SCSP_SHIFT));
        s = ((p1 * 256) * ((1 << SCSP_SHIFT) - frac) + (p2 * 256) * frac) >> SCSP_SHIFT;
    } else {
        uint32_t a1 = c->sa + ((cur >> (SCSP_SHIFT - 1)) & ~1u), a2 = c->sa + ((nxt >> (SCSP_SHIFT - 1)) & ~1u);
        int32_t p1 = (int16_t)(uint16_t)(ssyn_ram_b(c, a1) << 8 | ssyn_ram_b(c, a1 + 1));
        int32_t p2 = (int16_t)(uint16_t)(ssyn_ram_b(c, a2) << 8 | ssyn_ram_b(c, a2 + 1));
        s = (p1 * ((1 << SCSP_SHIFT) - frac) + p2 * frac) >> SCSP_SHIFT;
    }
    if (c->sbctl & 1) s ^= 0x7FFF;
    if (c->sbctl & 2) s = (int16_t)(s ^ 0x8000);
    return s;
}

/* A reverse (LPCTL 2) or alternating (3) loop: the position turns at its
 * ends. */
static inline void ssyn_loop_turn(ssyn_voice_t *v, uint32_t *pos, uint32_t lsa, uint32_t lea, int alt) {
    uint32_t addr = *pos >> SCSP_SHIFT;
    bool back = (addr < lsa || (*pos & 0x80000000u)) && v->backwards;
    if (!alt) {
        if (addr >= lsa && !v->backwards) {
            *pos = (lea << SCSP_SHIFT) - (*pos - (lsa << SCSP_SHIFT));
            v->backwards = 1;
        } else if (back) {
            *pos = (lea << SCSP_SHIFT) - ((lsa << SCSP_SHIFT) - *pos);
        }
    } else if (addr >= lea) {
        *pos = (lea << SCSP_SHIFT) - (*pos - (lea << SCSP_SHIFT));
        v->backwards = 1;
    } else if (back) {
        *pos = (lsa << SCSP_SHIFT) + ((lsa << SCSP_SHIFT) - *pos);
        v->backwards = 0;
    }
}

/* One of the voice's two positions against its loop (LPCTL): none, forward,
 * reverse, alternating. */
static inline void ssyn_loop(ssyn_voice_t *v, int lpctl, uint32_t *pos, uint32_t lsa, uint32_t lea) {
    uint32_t addr = *pos >> SCSP_SHIFT;
    switch (lpctl) {
    case 0:
        if (addr >= lsa && addr >= lea) ssyn_stop(v, 0);
        break;
    case 1:
        if (addr >= lea) *pos = (lsa << SCSP_SHIFT) + (*pos - (lea << SCSP_SHIFT));
        break;
    default:
        ssyn_loop_turn(v, pos, lsa, lea, lpctl == 3);
        break;
    }
}

/* The voice steps on by `step`, and meets its loop. With LPSLNK the attack
 * ends where the loop starts. */
static inline void ssyn_advance(ssyn_voice_t *v, const ssyn_run_t *c, uint32_t step) {
    if (v->backwards) v->cur -= step;
    else              v->cur += step;
    v->nxt = v->cur + (1u << SCSP_SHIFT);
    if ((v->cur >> SCSP_SHIFT) >= c->lsa && !v->backwards && c->lpslnk && v->state == SCSP_ATTACK)
        v->state = SCSP_DECAY1;
    ssyn_loop(v, c->lpctl, &v->cur, c->lsa, c->lea);
    ssyn_loop(v, c->lpctl, &v->nxt, c->lsa, c->lea);
}

#if defined(_MSC_VER) && !defined(__clang__)
#define SSYN_INLINE __forceinline
#else
#define SSYN_INLINE inline __attribute__((always_inline))
#endif

/* ---- the steady voice ------------------------------------------------------------
 *
 * Nearly every sample STF plays comes from a voice in one of two envelope
 * stages: letting go (release, falling by rr a sample) or held (decay 2 with
 * D2R 0, a constant level), with no LFO, played forwards. Neither leaves its
 * stage inside a run, so the envelope is a constant or one table load a
 * sample, and the voice's samples are made into a buffer first and mixed
 * after (a loop the compiler vectorises). The arithmetic is the general
 * loop's, term for term: the interpolation a*256 + ((b - a)*256*f >> 12) is
 * (a*256*(4096 - f) + b*256*f) >> 12 exactly, a*256*4096 being a whole
 * multiple of 4096. `inside`: every byte the run can read is in sound RAM. */

static SSYN_INLINE int32_t ssyn_steady_wave(const ssyn_run_t *c, uint32_t cur, uint32_t nxt, int pcm8, int inside) {
    if (!inside) return ssyn_wave(c, cur, nxt);
    int32_t frac = (int32_t)(cur & ((1u << SCSP_SHIFT) - 1)), s;
    if (pcm8) {
        int32_t p1 = (int8_t)c->ram[c->sa + (cur >> SCSP_SHIFT)], p2 = (int8_t)c->ram[c->sa + (nxt >> SCSP_SHIFT)];
        s = p1 * 256 + (((p2 - p1) * 256 * frac) >> SCSP_SHIFT);
    } else {
        uint32_t a1 = c->sa + ((cur >> (SCSP_SHIFT - 1)) & ~1u), a2 = c->sa + ((nxt >> (SCSP_SHIFT - 1)) & ~1u);
        int32_t p1 = (int16_t)(uint16_t)(c->ram[a1] << 8 | c->ram[a1 + 1]);
        int32_t p2 = (int16_t)(uint16_t)(c->ram[a2] << 8 | c->ram[a2 + 1]);
        s = p1 + (((p2 - p1) * frac) >> SCSP_SHIFT);
    }
    return s;   /* inside is only taken with SBCTL 0 */
}

/* Both positions on by step, against no loop or a forward one; 0 when the
 * voice ends after this sample. */
static SSYN_INLINE int ssyn_steady_step(const ssyn_run_t *c, uint32_t *cur, uint32_t *nxt, uint32_t step) {
    *cur += step;
    *nxt = *cur + (1u << SCSP_SHIFT);
    const uint32_t a0 = *cur >> SCSP_SHIFT, a1 = *nxt >> SCSP_SHIFT;
    if (c->lpctl == 0) return !((a0 >= c->lsa && a0 >= c->lea) || (a1 >= c->lsa && a1 >= c->lea));
    if (a0 >= c->lea) *cur = (c->lsa << SCSP_SHIFT) + (*cur - (c->lea << SCSP_SHIFT));
    if (a1 >= c->lea) *nxt = (c->lsa << SCSP_SHIFT) + (*nxt - (c->lea << SCSP_SHIFT));
    return 1;
}

/* The steady voice's samples from k into buf; returns how many (fewer than
 * to - k when it ended). */
static SSYN_INLINE uint32_t ssyn_steady_make(ssyn_voice_t *v, const ssyn_run_t *c, uint32_t k, uint32_t to,
                                             int32_t *buf, int pcm8, int release, int inside) {
    uint32_t cur = v->cur, nxt = v->nxt, n = 0;
    int32_t vol = v->vol, eg = scsp_eg_table[vol >> SCSP_EG_SHIFT];
    int live = 1;
    while (n < to - k) {
        int32_t s = ssyn_steady_wave(c, cur, nxt, pcm8, inside);
        if (!ssyn_steady_step(c, &cur, &nxt, v->step)) live = 0;
        if (release) {
            vol -= v->rr;
            if (vol <= 0) { vol = 0; live = 0; }
            eg = scsp_eg_table[vol >> SCSP_EG_SHIFT];
        }
        buf[n++] = (int32_t)(((int64_t)s * eg) >> SCSP_SHIFT);
        if (!live) break;
    }
    v->cur = cur; v->nxt = nxt; v->vol = vol;
    if (!live) ssyn_stop(v, 0);
    return n;
}

/* buf's n samples into the block from k: direct out and the reverb send. */
static inline void ssyn_steady_mix(const ssyn_voice_t *v, uint32_t k, const int32_t *buf, uint32_t n) {
    const int32_t gl = v->gl, gr = v->gr, gm = v->gm;
    int32_t *const l = ssyn_acc_l + k, *const r = ssyn_acc_r + k;
    for (uint32_t j = 0; j < n; j++) l[j] += (buf[j] * gl) >> SCSP_SHIFT;
    for (uint32_t j = 0; j < n; j++) r[j] += (buf[j] * gr) >> SCSP_SHIFT;
    if (!gm) return;
    int32_t *const mixs = &ssyn_acc_mixs[k][SCSP_ISEL(v)];
    for (uint32_t j = 0; j < n; j++) mixs[j * 16] += (buf[j] * gm) >> (SCSP_SHIFT - 2);
}

/* Whether a run of n samples reads only inside sound RAM: cur and nxt climb
 * by step a sample at most, and a forward loop only takes them back. */
static inline int ssyn_steady_inside(const ssyn_voice_t *v, const ssyn_run_t *c, uint32_t n) {
    if (c->sbctl || (c->lpctl && c->lsa > c->lea)) return 0;
    uint64_t top = (uint64_t)v->cur + (uint64_t)(n - 1) * v->step + (1u << SCSP_SHIFT);
    if (v->nxt > top) top = v->nxt;
    const uint64_t end = (uint64_t)c->sa + (c->pcm8 ? (top >> SCSP_SHIFT) : ((top >> SCSP_SHIFT) << 1) + 1);
    return end < SOUND_RAM_SIZE && end <= 0xFFFFFu;
}

/* The steady voice from k to `to`, made by the copy compiled for its sample
 * width, stage and whether it stays inside RAM. */
static void ssyn_steady_run(ssyn_voice_t *v, const ssyn_run_t *c, uint32_t k, uint32_t to) {
    static int32_t buf[SSYN_BLOCK];
    const int rel = v->state == SCSP_RELEASE, in = ssyn_steady_inside(v, c, to - k);
    uint32_t n;
    if (c->pcm8) n = in ? (rel ? ssyn_steady_make(v, c, k, to, buf, 1, 1, 1) : ssyn_steady_make(v, c, k, to, buf, 1, 0, 1))
                        : (rel ? ssyn_steady_make(v, c, k, to, buf, 1, 1, 0) : ssyn_steady_make(v, c, k, to, buf, 1, 0, 0));
    else         n = in ? (rel ? ssyn_steady_make(v, c, k, to, buf, 0, 1, 1) : ssyn_steady_make(v, c, k, to, buf, 0, 0, 1))
                        : (rel ? ssyn_steady_make(v, c, k, to, buf, 0, 1, 0) : ssyn_steady_make(v, c, k, to, buf, 0, 0, 0));
    ssyn_steady_mix(v, k, buf, n);
}

/* A voice the steady run can make: no LFO, no SDIR, the sample source, played
 * forwards with no loop or a forward one, letting go or held. */
static inline int ssyn_is_steady(const ssyn_voice_t *v) {
    return !SCSP_PLFOS(v) && !SCSP_ALFOS(v) && !SCSP_SDIR(v) && !SCSP_SSCTL(v) && SCSP_LPCTL(v) < 2 && !v->backwards
        && (v->state == SCSP_RELEASE || (v->state == SCSP_DECAY2 && SCSP_D2R(v) == 0));
}

/* Voice i on from where it is in the block to sample `to`. */
static void ssyn_voice_run(unsigned i, uint32_t to) {
    ssyn_voice_t *v = &g_ssyn.v[i];
    uint32_t k = g_ssyn.done[i];
    if (k >= to) return;
    g_ssyn.done[i] = (uint16_t)to;
    if (!v->on || SCSP_SSCTL(v) == 3) return;          /* SSCTL 3 "cannot be used": the chip holds it */
    const int plfo = SCSP_PLFOS(v) != 0, alfo = SCSP_ALFOS(v) != 0, sdir = SCSP_SDIR(v) != 0;
    const int32_t gl = v->gl, gr = v->gr, gm = v->gm;
    const ssyn_run_t c = { g_sound.ram, SCSP_SA(v), SCSP_LSA(v), SCSP_LEA(v), SCSP_PCM8B(v) != 0,
                           (int)SCSP_SBCTL(v), (int)SCSP_LPCTL(v), SCSP_LPSLNK(v) != 0 };
    if (ssyn_is_steady(v)) { ssyn_steady_run(v, &c, k, to); return; }
    int32_t *mixs = &ssyn_acc_mixs[0][SCSP_ISEL(v)];
    for (; k < to; k++) {
        uint32_t step = v->step;
        if (plfo) step = (uint32_t)(((int64_t)step * ssyn_plfo(&v->plfo)) >> SCSP_SHIFT);
        int32_t s = SCSP_SSCTL(v) ? 0 : ssyn_wave(&c, v->cur, v->nxt);
        ssyn_advance(v, &c, step);
        if (!sdir) {
            if (alfo) s = (int32_t)(((int64_t)s * ssyn_alfo(&v->alfo)) >> SCSP_SHIFT);
            s = (int32_t)(((int64_t)s * ssyn_env(v)) >> SCSP_SHIFT);
        }
        ssyn_acc_l[k] += (s * gl) >> SCSP_SHIFT;
        ssyn_acc_r[k] += (s * gr) >> SCSP_SHIFT;
        mixs[k * 16] += (s * gm) >> (SCSP_SHIFT - 2);
        if (!v->on) break;
    }
}

/* The reverb's outputs onto the mix: the EFREGs slots 0-15 return. */
static inline void ssyn_returns(const scsp_dsp_t *d, int32_t *l, int32_t *r) {
    *l = *r = 0;
    for (int i = 0; i < 16; i++) {
        const ssyn_voice_t *v = &g_ssyn.v[i];
        if (!SCSP_EFSDL(v)) continue;
        *l += (d->efreg[i] * v->el) >> SCSP_SHIFT;
        *r += (d->efreg[i] * v->er) >> SCSP_SHIFT;
    }
}

/* Sample k of the block: its sends through the reverb, the returns onto its
 * direct mix, out to the DAC. */
static inline void ssyn_mix(scsp_t *s, uint32_t k, int32_t *fl, int32_t *fr) {
    int32_t *mx = ssyn_acc_mixs[k], nz = 0;
    for (int j = 0; j < 16; j++) { s->dsp.mixs[j] += mx[j]; nz |= mx[j]; mx[j] = 0; }
    if (scsp_dsp_due(s, nz)) { scsp_dsp_step(s); ssyn_returns(&s->dsp, fl, fr); }
    scsp_sample_out(s, ssyn_acc_l[k] + *fl, ssyn_acc_r[k] + *fr);
    ssyn_acc_l[k] = ssyn_acc_r[k] = 0;
}

/* Make every sample owed: the voices into the block, then the mix. The synth
 * makes them where the chip would sync (a voice written, a write to the
 * reverb's ring or the common registers that matter, a full block, the end
 * of a run), so the quiet reverb's bookkeeping sees the same blocks. */
static void ssyn_flush(void) {
    const uint32_t n = g_ssyn.pending;
    if (!n) return;
    scsp_t *s = &g_sound.scsp;
    for (unsigned i = 0; i < 32; i++) ssyn_voice_run(i, n);
    int32_t fl, fr;
    ssyn_returns(&s->dsp, &fl, &fr);
    for (uint32_t k = 0; k < n; k++) ssyn_mix(s, k, &fl, &fr);
    g_ssyn.pending = 0;
    memset(g_ssyn.done, 0, sizeof g_ssyn.done);
    g_ssyn.samples += n;
}

/* A sample boundary has passed (shle_run): one more sample is owed. */
static inline void ssyn_tick(void) {
    if (++g_ssyn.pending == SSYN_BLOCK) ssyn_flush();
}

/* ---- the driver's side ---------------------------------------------------------------- */

/* A slot register word, as the chip takes it: KYONEX in word 0 keys every
 * slot on or off by its KYONB at once. */
static void ssyn_w16(uint32_t off, uint16_t w) {
    ssyn_voice_t *v = &g_ssyn.v[(off >> 5) & 31];
    unsigned r = (off & 0x1F) >> 1;
    v->r[r] = w;
    switch (r) {
    case 0x0:
        if (SCSP_KYONEX(v)) {
            for (int i = 0; i < 32; i++) {
                ssyn_voice_t *u = &g_ssyn.v[i];
                if (SCSP_KYONB(u) && u->state == SCSP_RELEASE) ssyn_key_on(u);
                if (!SCSP_KYONB(u)) ssyn_stop(u, 1);
            }
            v->r[0] &= (uint16_t)~0x1000;
        }
        break;
    case 0x5:   /* a new release rate takes effect at once, without key scaling (MAME) */
        v->rr = ssyn_rate(scsp_dr_table, 0, SCSP_RR(v));
        v->dl = 0x1F - (int32_t)SCSP_DL(v);
        break;
    case 0x8: v->step = ssyn_pitch(SCSP_OCT(v), SCSP_FNS(v)); break;
    case 0x9: ssyn_lfo(v); break;
    case 0x6: case 0xA: case 0xB: ssyn_gains(v); break;
    }
}

/* A write to a slot register (off < 0x400), sz bytes: a byte merges into its
 * word, a long is two words. What was owed is made first. */
static void ssyn_write(uint32_t off, uint32_t val, int sz) {
    ssyn_flush();
    off &= 0x3FFu;
    if (sz == 1) {
        uint16_t w = g_ssyn.v[off >> 5].r[(off & 0x1F) >> 1];
        w = (off & 1) ? (uint16_t)((w & 0xFF00) | (val & 0xFF)) : (uint16_t)((w & 0x00FF) | ((val & 0xFF) << 8));
        ssyn_w16(off & ~1u, w);
    } else if (sz == 2) {
        ssyn_w16(off & ~1u, (uint16_t)val);
    } else {
        ssyn_w16(off & ~1u, (uint16_t)(val >> 16));
        ssyn_w16((off & ~1u) + 2, (uint16_t)val);
    }
}

/* A slot register word read back, as the driver's peek at the chip reads it:
 * as far as the voice has run (shle_sr16 does not catch the chip up either). */
static inline uint16_t ssyn_peek16(uint32_t off) {
    off &= 0x3FFu;
    return g_ssyn.v[off >> 5].r[(off & 0x1F) >> 1];
}

/* A peek at the sound chip's registers: the voices' from the synth while it
 * plays them, the rest from the chip. */
static inline uint16_t ssyn_scsp_peek16(uint32_t off) {
    off &= 0xFFFFu;
    if (g_ssyn.on && off < 0x400) return ssyn_peek16(off);
    return scsp_peek16(&g_sound.scsp, off);
}

/* Which 4 KB half of its 8 KB window voice i plays in: CA bit 0, what the
 * driver's streaming polls to refill the other half. */
static inline bool ssyn_ca_upper(unsigned i) {
    ssyn_voice_run(i, g_ssyn.pending);
    return (g_ssyn.v[i].cur >> (SCSP_SHIFT + 12)) & 1;
}

/* The driver is about to write [addr, addr + n) of sound RAM: the voices
 * read the old bytes first, and a write into what the reverb reads
 * (dsp_lo..dsp_hi, scsp_recouple's) makes the block and wakes it, as
 * scsp_ram_touch does. */
static inline void ssyn_ram_touch(uint32_t addr, uint32_t n) {
    scsp_t *s = &g_sound.scsp;
    if (!g_ssyn.pending) return;
    if (addr < s->dsp_hi && s->dsp_lo < addr + n) { ssyn_flush(); scsp_dsp_wake(s); return; }
    for (unsigned i = 0; i < 32; i++) ssyn_voice_run(i, g_ssyn.pending);
}

/* A write to the common or DSP registers (off >= 0x400) goes to the chip,
 * which keeps the reverb, the DAC and the clockwork the synth does not; the
 * block is made first where the chip would make its own (scsp_write_syncs). */
static inline void ssyn_chip_write(uint32_t off, uint32_t val, int sz) {
    uint32_t a = off & 0xFFFEu;
    uint16_t w = (uint16_t)(sz == 4 ? val >> 16 : val);
    if (sz == 1) {                                     /* merged into its word, as scsp_write does */
        uint16_t o = scsp_peek16(&g_sound.scsp, a);
        w = (off & 1) ? (uint16_t)((o & 0xFF00) | (val & 0xFF)) : (uint16_t)((o & 0x00FF) | ((val & 0xFF) << 8));
    }
    if (scsp_write_syncs(a, w) || (sz == 4 && scsp_write_syncs(a + 2, (uint16_t)val))) ssyn_flush();
    scsp_write(&g_sound.scsp, off, val, sz);
}

static inline void ssyn_reset(bool on) {
    scsp_tables_init();
    memset(&g_ssyn, 0, sizeof g_ssyn);
    memset(ssyn_acc_l, 0, sizeof ssyn_acc_l);
    memset(ssyn_acc_r, 0, sizeof ssyn_acc_r);
    memset(ssyn_acc_mixs, 0, sizeof ssyn_acc_mixs);
    g_ssyn.on = on;
    for (int i = 0; i < 32; i++) {
        g_ssyn.v[i].state = SCSP_RELEASE;
        ssyn_gains(&g_ssyn.v[i]);
    }
}

#endif /* SOUND_SYNTH_H */
