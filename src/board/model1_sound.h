/*
 * model1_sound.h — the Model 1 sound board's chips: two MultiPCMs (Yamaha
 * YMW-258-F, Sega 315-5560), a YM3438's timers and the 68000's own i8251.
 * Daytona USA runs on the original Model 2, which kept this board in place of
 * the SCSP one (MAME src/mame/shared/segam1audio.cpp); sound.h runs it when a
 * profile sets quirks.model1_sound (Pinboard #1035).
 *
 * 68000 address map (10 MHz):
 *   0x000000-0x03FFFF  program ROM
 *   0x080000-0x09FFFF  program ROM +0x20000 again (the upper socket's mirror)
 *   0xC20000-0xC20003  i8251, on the low byte (umask 0x00ff)
 *   0xC40000-0xC40007  MultiPCM 1, low byte; 0xC50000 its sample bank
 *   0xC60000-0xC60007  MultiPCM 2, low byte; 0xC70000 its sample bank
 *   0xD00000-0xD00007  YM3438, low byte
 *   0xF00000-0xF0FFFF  RAM
 * The i960's UART bytes arrive at the board's i8251, whose RxRDY is the
 * 68000's level 2 interrupt; what the 68000 sends goes back to the i960's
 * receiver (MAME model2.cpp model2o: m1audio rxd_handler -> i8251 write_rxd).
 *
 * MultiPCM: MAME's gew.cpp and multipcm.cpp (BSD-3-Clause, Miguel Angel
 * Horna) by way of noclip's port (tools/sound/model1_chips.cpp, branch
 * daytona-music), the device plumbing taken off. Each chip addresses 4 MB:
 * the first megabyte of its ROM at 0, a bank of it the 68000 picks at
 * 0x100000, the rest reads 0. The chip makes 10 MHz / 224 = 44642.857
 * samples a second; here it makes the board's 44.1 kHz ones, so the pitch
 * steps, the envelope rates and the LFOs are scaled by that ratio and play at
 * the speed MAME's do. The tables are built once with the host's maths:
 * they feed the output samples only, which nothing on the board reads.
 *
 * YM3438: only the timers and the busy flag (ymfm's fm_engine_base and
 * ym2612, MAME's core for it). The driver keeps its tempo by polling the
 * timer flags, but Daytona never keys an FM voice: measured in MAME over 60 s
 * of attract and a race, 108,708 writes to the chip, register 0x28 written 12
 * times and all of them key-offs. A key-on is logged at WARN.
 */
#ifndef MODEL1_SOUND_H
#define MODEL1_SOUND_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "log.h"

#define M1SND_CPU_HZ      10000000u
#define M1SND_OUT_HZ      44100u         /* SOUND_RATE */
#define M1SND_VOICES      28
#define M1SND_PCM_SIZE    0x400000u      /* each chip's ROM in romset.samples */
#define M1SND_YM_PRESCALE 144u           /* YM3438 clocks a sample: 24 operators x 6 */
#define M1SND_TX_FIFO     32u

#define MPCM_TL_SHIFT  12
#define MPCM_EG_SHIFT  16
#define MPCM_LFO_SHIFT 8

enum { MPCM_ATTACK, MPCM_DECAY1, MPCM_DECAY2, MPCM_RELEASE };

typedef struct {
    uint32_t start, loop, end;
    uint8_t  attack_reg, decay1_reg, decay2_reg, decay_level;
    uint8_t  release_reg, key_rate_scale, lfo_vibrato_reg, lfo_amplitude_reg;
    uint8_t  format;
} mpcm_sample_t;

typedef struct {
    int32_t volume;
    uint8_t state;
    int32_t attack_rate, decay1_rate, decay2_rate, release_rate, decay_level;
} mpcm_eg_t;

/* MAME's lfo_t keeps pointers to its tables; this keeps which ones. */
typedef struct {
    uint16_t phase;
    uint32_t phase_step;
    uint8_t  amplitude;   /* 1 = the amplitude tables, 0 = the pitch ones */
    uint8_t  scale;       /* which of the eight scales */
} mpcm_lfo_t;

typedef struct {
    uint8_t       regs[11];
    bool          playing;
    mpcm_sample_t sample;
    uint32_t      offset;
    uint8_t       octave;
    uint16_t      pitch;
    uint32_t      step;
    uint32_t      pan;
    uint32_t      total_level, dest_total_level;
    int32_t       total_level_step;
    int32_t       prev_sample;
    mpcm_eg_t     eg;
    uint8_t       lfo_frequency;
    mpcm_lfo_t    pitch_lfo;
    uint8_t       vibrato;
    mpcm_lfo_t    amplitude_lfo;
    uint8_t       tremolo;
} mpcm_slot_t;

typedef struct {
    mpcm_slot_t slots[M1SND_VOICES];
    int32_t     cur_slot;
    int32_t     address;
    uint32_t    bank;
} mpcm_t;

typedef struct {
    uint16_t addr;
    uint8_t  r24, r25, r26, r27;
    uint8_t  status;           /* bit 0 timer A, bit 1 timer B */
    uint8_t  running[2];
    int64_t  timer_at[2];      /* YM clock the timer expires on, -1 = stopped */
    uint64_t busy_end;         /* YM clock the busy flag drops */
    uint64_t now;              /* YM clocks (8 MHz) */
    uint8_t  keyon_warned;
} m1ym_t;

typedef struct {
    mpcm_t   pcm[2];
    m1ym_t   ym;
    /* the 68000's i8251 */
    uint8_t  rx_data, rxrdy, mode_done, rxe;
    uint32_t rx_overruns, rx_dropped;
    uint8_t  tx[M1SND_TX_FIFO];        /* bytes the 68000 sent the i960 */
    uint8_t  tx_r, tx_w;
    uint32_t cyc_frac;                 /* 68000 clocks owed to the next sample, x44100 */
} m1snd_t;

/* ---- the tables every chip shares ------------------------------------------- */

typedef struct {
    bool     built;
    uint32_t attack_step[0x40], decay_release_step[0x40], freq_step[0x400];
    int32_t  left_pan[0x800], right_pan[0x800], lin_to_exp[0x400], tl_steps[2];
    int32_t  pitch_table[256], amplitude_table[256];
    int32_t  pitch_scale[8][256], amplitude_scale[8][256];
    uint32_t lfo_step[8];
} mpcm_tables_t;
static mpcm_tables_t g_mpcm_t;

static const int8_t g_mpcm_value_to_channel[32] = {
    0, 1, 2, 3, 4, 5, 6, -1,
    7, 8, 9, 10, 11, 12, 13, -1,
    14, 15, 16, 17, 18, 19, 20, -1,
    21, 22, 23, 24, 25, 26, 27, -1,
};

static const double g_mpcm_base_times[64] = {
    0,       0,       0,       0,
    6222.95, 4978.37, 4148.66, 3556.01,
    3111.47, 2489.21, 2074.33, 1778.00,
    1555.74, 1244.63, 1037.19, 889.02,
    777.87,  622.31,  518.59,  444.54,
    388.93,  311.16,  259.32,  222.27,
    194.47,  155.60,  129.66,  111.16,
    97.23,   77.82,   64.85,   55.60,
    48.62,   38.91,   32.43,   27.80,
    24.31,   19.46,   16.24,   13.92,
    12.15,   9.75,    8.12,    6.98,
    6.08,    4.90,    4.08,    3.49,
    3.04,    2.49,    2.13,    1.90,
    1.72,    1.41,    1.18,    1.04,
    0.91,    0.73,    0.59,    0.50,
    0.45,    0.45,    0.45,    0.45,
};

static inline uint32_t mpcm_fixed(uint32_t bits, double v) { return (uint32_t)((double)(1u << bits) * v); }

/* The level-and-pan gains. */
static inline void mpcm_tables_pan(mpcm_tables_t *T) {
    for (int32_t level = 0; level < 0x80; ++level) {
        const double total_level = pow(10.0, ((double)level * -24.0 / 64.0) / 20.0) / 4.0;
        for (int32_t pan = 0; pan < 0x10; ++pan) {
            double pl, pr;
            if (pan == 0x8)      { pl = 0.0; pr = 0.0; }
            else if (pan == 0x0) { pl = 1.0; pr = 1.0; }
            else if (pan & 0x8) {
                const int32_t inv = 0x10 - pan;
                pl = 1.0;
                pr = (inv & 7) == 7 ? 0.0 : pow(10.0, ((double)inv * -12.0 / 4.0) / 20.0);
            } else {
                pr = 1.0;
                pl = (pan & 7) == 7 ? 0.0 : pow(10.0, ((double)pan * -12.0 / 4.0) / 20.0);
            }
            T->left_pan[(pan << 7) | level]  = (int32_t)mpcm_fixed(MPCM_TL_SHIFT, pl * total_level);
            T->right_pan[(pan << 7) | level] = (int32_t)mpcm_fixed(MPCM_TL_SHIFT, pr * total_level);
        }
    }
}

/* The pitch steps, the envelope's rates and the level fade's, and the level
 * curve; ratio the chip's samples to one of the board's. */
static inline void mpcm_tables_eg(mpcm_tables_t *T, double ratio) {
    /* The pitch step at the chip's rate, 4096 x rate x (1024 + i) / 1024:
     * integers, (10 MHz / 224) x 4096 / 1024 = 10 MHz / 56. */
    for (uint32_t i = 0; i < 0x400; ++i)
        T->freq_step[i] = (uint32_t)((uint64_t)M1SND_CPU_HZ * (1024u + i) / 56u);
    const double adr = 14.32833;
    for (int32_t i = 4; i < 0x40; ++i) {
        T->attack_step[i] = (uint32_t)((double)(0x400 << MPCM_EG_SHIFT) / (g_mpcm_base_times[i] * 44100.0 / 1000.0) * ratio);
        T->decay_release_step[i] = (uint32_t)((double)(0x400 << MPCM_EG_SHIFT) / (g_mpcm_base_times[i] * adr * 44100.0 / 1000.0) * ratio);
    }
    T->attack_step[0] = T->attack_step[1] = T->attack_step[2] = T->attack_step[3] = 0;
    T->attack_step[0x3f] = 0x400 << MPCM_EG_SHIFT;
    T->decay_release_step[0] = T->decay_release_step[1] = T->decay_release_step[2] = T->decay_release_step[3] = 0;
    T->tl_steps[0] = (int32_t)(-(double)(0x80 << MPCM_TL_SHIFT) / (78.2 * 44100.0 / 1000.0) * ratio);
    T->tl_steps[1] = (int32_t)((double)(0x80 << MPCM_TL_SHIFT) / (78.2 * 2 * 44100.0 / 1000.0) * ratio);
    for (int32_t i = 0; i < 0x400; ++i) {
        const double db = -(96.0 - (96.0 * (double)i / (double)0x400));
        T->lin_to_exp[i] = (int32_t)mpcm_fixed(MPCM_TL_SHIFT, pow(10.0, db / 20.0));
    }
}

/* The LFOs' waves, their depths and their rates. */
static inline void mpcm_tables_lfo(mpcm_tables_t *T) {
    static const float lfo_freq[8] = { 0.168f, 2.019f, 3.196f, 4.206f, 5.215f, 5.888f, 6.224f, 7.066f };
    static const float phase_limit[8] = { 0.0f, 3.378f, 5.065f, 6.750f, 10.114f, 20.170f, 40.180f, 79.307f };
    static const float amp_limit[8] = { 0.0f, 0.4f, 0.8f, 1.5f, 3.0f, 6.0f, 12.0f, 24.0f };
    for (int32_t i = 0; i < 256; ++i) {
        if (i < 64)       T->pitch_table[i] = i * 2 + 128;
        else if (i < 128) T->pitch_table[i] = 383 - i * 2;
        else if (i < 192) T->pitch_table[i] = 384 - i * 2;
        else              T->pitch_table[i] = i * 2 - 383;
        T->amplitude_table[i] = i < 128 ? 255 - (i * 2) : (i * 2) - 256;
    }
    for (int32_t t = 0; t < 8; ++t) {
        for (int32_t i = -128; i < 128; ++i)
            T->pitch_scale[t][i + 128] = (int32_t)mpcm_fixed(MPCM_LFO_SHIFT, pow(2.0, ((double)phase_limit[t] * i / 128.0) / 1200.0));
        for (int32_t i = 0; i < 256; ++i)
            T->amplitude_scale[t][i] = (int32_t)mpcm_fixed(MPCM_LFO_SHIFT, pow(10.0, ((double)-amp_limit[t] * i / 256.0) / 20.0));
        T->lfo_step[t] = mpcm_fixed(MPCM_LFO_SHIFT, (double)lfo_freq[t] * 256.0 / (double)M1SND_OUT_HZ);
    }
}

static inline void mpcm_tables_build(void) {
    mpcm_tables_t *T = &g_mpcm_t;
    if (T->built) return;
    /* MAME's envelope and level-fade rates are per chip sample; these are per
     * board sample, which comes this much less often. */
    const double ratio = (double)M1SND_CPU_HZ / 224.0 / (double)M1SND_OUT_HZ;
    mpcm_tables_pan(T);
    mpcm_tables_eg(T, ratio);
    mpcm_tables_lfo(T);
    T->built = true;
}

/* ---- MultiPCM ------------------------------------------------------------------- */

/* A byte of the chip's 4 MB space (rom: this chip's 4 MB of romset.samples). */
static inline uint8_t mpcm_byte(const mpcm_t *p, const uint8_t *rom, uint32_t rom_size, uint32_t a) {
    a &= 0x3FFFFFu;
    if (a >= 0x200000u) return 0;
    if (a >= 0x100000u) a = (p->bank << 20) | (a & 0xFFFFFu);
    return a < rom_size ? rom[a] : 0;
}

static inline void mpcm_init_sample(const mpcm_t *p, const uint8_t *rom, uint32_t n, mpcm_sample_t *s, uint32_t index) {
    uint32_t a = index * 12u;
#define MB(o) mpcm_byte(p, rom, n, a + (o))
    s->start = ((uint32_t)MB(0) << 16) | ((uint32_t)MB(1) << 8) | MB(2);
    s->format = (uint8_t)((s->start >> 20) & 0xFEu);
    s->start &= 0x3FFFFFu;
    s->loop = ((uint32_t)MB(3) << 8) | MB(4);
    s->end = 0x10000u - (((uint32_t)MB(5) << 8) | MB(6));
    s->attack_reg = (MB(8) >> 4) & 0xF;
    s->decay1_reg = MB(8) & 0xF;
    s->decay2_reg = MB(9) & 0xF;
    s->decay_level = (MB(9) >> 4) & 0xF;
    s->release_reg = MB(10) & 0xF;
    s->key_rate_scale = (MB(10) >> 4) & 0xF;
    s->lfo_vibrato_reg = MB(7);
    s->lfo_amplitude_reg = MB(11) & 0xF;
#undef MB
}

static inline uint32_t mpcm_get_rate(const uint32_t *steps, int32_t rate, uint32_t val) {
    if (val == 0) return steps[0];
    if (val == 0xF) return steps[0x3F];
    int r = 4 * (int)val + rate;
    if (r < 0) r = 0;
    if (r > 0x3F) r = 0x3F;
    return steps[r];
}

static inline void mpcm_eg_calc(mpcm_slot_t *sl) {
    int32_t octave = sl->octave;
    if (octave & 8) octave -= 16;
    int32_t rate = sl->sample.key_rate_scale != 0xF
                 ? (octave + sl->sample.key_rate_scale) * 2 + ((sl->pitch >> 9) & 1) : 0;
    sl->eg.attack_rate  = (int32_t)mpcm_get_rate(g_mpcm_t.attack_step, rate, sl->sample.attack_reg);
    sl->eg.decay1_rate  = (int32_t)mpcm_get_rate(g_mpcm_t.decay_release_step, rate, sl->sample.decay1_reg);
    sl->eg.decay2_rate  = (int32_t)mpcm_get_rate(g_mpcm_t.decay_release_step, rate, sl->sample.decay2_reg);
    sl->eg.release_rate = (int32_t)mpcm_get_rate(g_mpcm_t.decay_release_step, rate, sl->sample.release_reg);
    sl->eg.decay_level  = 0xF - sl->sample.decay_level;
}

static inline int32_t mpcm_eg_update(mpcm_slot_t *sl) {
    mpcm_eg_t *eg = &sl->eg;
    switch (eg->state) {
    case MPCM_ATTACK:
        eg->volume += (int32_t)(((int64_t)((0x817 << (MPCM_EG_SHIFT - 1)) - eg->volume) * eg->attack_rate) >> 24);
        if (eg->volume >= (0x3FF << MPCM_EG_SHIFT)) {
            eg->state = eg->decay1_rate >= (0x400 << MPCM_EG_SHIFT) ? MPCM_DECAY2 : MPCM_DECAY1;
            eg->volume = 0x3FF << MPCM_EG_SHIFT;
        }
        break;
    case MPCM_DECAY1:
        eg->volume -= eg->decay1_rate;
        if (eg->volume <= 0) eg->volume = 0;
        if (eg->volume >> (MPCM_EG_SHIFT + 6) <= eg->decay_level) eg->state = MPCM_DECAY2;
        break;
    case MPCM_DECAY2:
        eg->volume -= eg->decay2_rate;
        if (eg->volume <= 0) eg->volume = 0;
        break;
    case MPCM_RELEASE:
        eg->volume -= eg->release_rate;
        if (eg->volume <= 0) { eg->volume = 0; sl->playing = false; }
        break;
    default:
        return 1 << MPCM_TL_SHIFT;
    }
    return g_mpcm_t.lin_to_exp[eg->volume >> MPCM_EG_SHIFT];
}

static inline void mpcm_retrigger(mpcm_slot_t *sl) {
    sl->offset = 0;
    sl->prev_sample = 0;
    sl->total_level = sl->dest_total_level << MPCM_TL_SHIFT;
    mpcm_eg_calc(sl);
    sl->eg.state = MPCM_ATTACK;
    sl->eg.volume = (0x3FF - 0x2A0) << MPCM_EG_SHIFT;
}

/* MAME: (freq_step << oct) / chip rate, a step per chip sample; here per
 * board sample, freq_step being at the chip's rate. */
static inline void mpcm_update_step(mpcm_slot_t *sl) {
    const uint8_t oct = (uint8_t)((sl->octave - 1) & 0xF);
    uint32_t pitch = g_mpcm_t.freq_step[sl->pitch];
    if (oct & 0x8) pitch >>= (16 - oct);
    else           pitch <<= oct;
    sl->step = pitch / M1SND_OUT_HZ;
}

static inline void mpcm_lfo_compute(mpcm_lfo_t *lfo, uint32_t freq, uint32_t scale, int amplitude) {
    lfo->phase_step = g_mpcm_t.lfo_step[freq];
    lfo->amplitude = (uint8_t)amplitude;
    lfo->scale = (uint8_t)scale;
}

static inline int32_t mpcm_lfo_step(mpcm_lfo_t *lfo) {
    lfo->phase = (uint16_t)(lfo->phase + lfo->phase_step);
    const int32_t *table = lfo->amplitude ? g_mpcm_t.amplitude_table : g_mpcm_t.pitch_table;
    const int32_t *scale = lfo->amplitude ? g_mpcm_t.amplitude_scale[lfo->scale] : g_mpcm_t.pitch_scale[lfo->scale];
    int32_t p = scale[table[(lfo->phase >> MPCM_LFO_SHIFT) & 0xFF]];
    return p << (MPCM_TL_SHIFT - MPCM_LFO_SHIFT);
}

static void mpcm_write_slot(mpcm_t *p, const uint8_t *rom, uint32_t n, mpcm_slot_t *sl, int32_t reg, uint8_t data) {
    sl->regs[reg] = data;
    switch (reg) {
    case 0:
        sl->pan = (data >> 4) & 0xF;
        break;
    case 1:
        mpcm_init_sample(p, rom, n, &sl->sample, sl->regs[1] | ((uint32_t)(sl->regs[2] & 1) << 8));
        sl->regs[7] = (uint8_t)((sl->sample.attack_reg << 4) | sl->sample.decay1_reg);
        sl->regs[8] = (uint8_t)((sl->sample.decay_level << 4) | sl->sample.decay2_reg);
        sl->regs[9] = (uint8_t)((sl->sample.key_rate_scale << 4) | sl->sample.release_reg);
        mpcm_write_slot(p, rom, n, sl, 6, sl->sample.lfo_vibrato_reg);
        mpcm_write_slot(p, rom, n, sl, 10, sl->sample.lfo_amplitude_reg);
        if (sl->playing) mpcm_retrigger(sl);
        break;
    case 2:
    case 3:
        sl->octave = sl->regs[3] >> 4;
        sl->pitch = (uint16_t)(((sl->regs[3] & 0xF) << 6) | (sl->regs[2] >> 2));
        mpcm_update_step(sl);
        break;
    case 4:
        if (data & 0x80) {
            sl->playing = true;
            mpcm_retrigger(sl);
        } else if (sl->playing) {
            if (sl->sample.release_reg != 0xF) sl->eg.state = MPCM_RELEASE;
            else sl->playing = false;
        }
        break;
    case 5:
        sl->dest_total_level = (data >> 1) & 0x7F;
        if (!(data & 1))
            sl->total_level_step = (sl->total_level >> MPCM_TL_SHIFT) > sl->dest_total_level
                                 ? g_mpcm_t.tl_steps[0] : g_mpcm_t.tl_steps[1];
        else
            sl->total_level = sl->dest_total_level << MPCM_TL_SHIFT;
        break;
    case 6:
    case 10:
        sl->lfo_frequency = (sl->regs[6] >> 3) & 7;
        sl->vibrato = sl->regs[6] & 7;
        sl->tremolo = sl->regs[10] & 7;
        if (data) {
            mpcm_lfo_compute(&sl->pitch_lfo, sl->lfo_frequency, sl->vibrato, 0);
            mpcm_lfo_compute(&sl->amplitude_lfo, sl->lfo_frequency, sl->tremolo, 1);
        }
        break;
    case 7:
    case 8:
    case 9:
        sl->sample.attack_reg = sl->regs[7] >> 4;
        sl->sample.decay1_reg = sl->regs[7] & 0xF;
        sl->sample.decay_level = sl->regs[8] >> 4;
        sl->sample.decay2_reg = sl->regs[8] & 0xF;
        sl->sample.key_rate_scale = sl->regs[9] >> 4;
        sl->sample.release_reg = sl->regs[9] & 0xF;
        mpcm_eg_calc(sl);
        break;
    }
}

static inline void mpcm_write(mpcm_t *p, const uint8_t *rom, uint32_t n, uint32_t offset, uint8_t data) {
    switch (offset) {
    case 0:
        if (p->address < 11 && p->cur_slot >= 0) mpcm_write_slot(p, rom, n, &p->slots[p->cur_slot], p->address, data);
        break;
    case 1:
        p->cur_slot = g_mpcm_value_to_channel[data & 0x1F];
        break;
    case 2:
        p->address = data;
        break;
    }
}

/* One output sample, as gew_pcm_device::sound_stream_update makes it, clamped
 * to 16 bits. */
static inline void mpcm_sample(mpcm_t *p, const uint8_t *rom, uint32_t n, int32_t *outl, int32_t *outr) {
    int32_t smpl = 0, smpr = 0;
    for (int32_t i = 0; i < M1SND_VOICES; ++i) {
        mpcm_slot_t *sl = &p->slots[i];
        if (!sl->playing) continue;
        uint32_t vol = (sl->total_level >> MPCM_TL_SHIFT) | (sl->pan << 7);
        uint32_t spos = sl->offset >> MPCM_TL_SHIFT;
        uint32_t step = sl->step;
        int32_t csample;
        int32_t fpart = (int32_t)(sl->offset & ((1u << MPCM_TL_SHIFT) - 1));
        if (sl->sample.format & 4) {             /* 12-bit, two samples in three bytes */
            uint32_t adr = sl->sample.start + (spos >> 1) * 3;
            if (!(spos & 1))
                csample = (int16_t)(uint16_t)(mpcm_byte(p, rom, n, adr) << 8 | ((mpcm_byte(p, rom, n, adr + 1) & 0xF) << 4));
            else
                csample = (int16_t)(uint16_t)((mpcm_byte(p, rom, n, adr + 2) << 8) | (mpcm_byte(p, rom, n, adr + 1) & 0xF0));
        } else {
            csample = (int16_t)(uint16_t)(mpcm_byte(p, rom, n, sl->sample.start + spos) << 8);
        }
        int32_t s = (csample * fpart + sl->prev_sample * ((1 << MPCM_TL_SHIFT) - fpart)) >> MPCM_TL_SHIFT;
        if (sl->vibrato) {
            step = step * (uint32_t)mpcm_lfo_step(&sl->pitch_lfo);
            step >>= MPCM_TL_SHIFT;
        }
        sl->offset += step;
        if (spos ^ (sl->offset >> MPCM_TL_SHIFT)) sl->prev_sample = csample;
        if (sl->offset >= (sl->sample.end << MPCM_TL_SHIFT))
            sl->offset -= (sl->sample.end - sl->sample.loop) << MPCM_TL_SHIFT;
        if ((sl->total_level >> MPCM_TL_SHIFT) != sl->dest_total_level)
            sl->total_level += (uint32_t)sl->total_level_step;
        if (sl->tremolo) {
            s = s * mpcm_lfo_step(&sl->amplitude_lfo);
            s >>= MPCM_TL_SHIFT;
        }
        s = (s * mpcm_eg_update(sl)) >> 10;
        smpl += (g_mpcm_t.left_pan[vol & 0x7FF] * s) >> MPCM_TL_SHIFT;
        smpr += (g_mpcm_t.right_pan[vol & 0x7FF] * s) >> MPCM_TL_SHIFT;
    }
    *outl = smpl < -32768 ? -32768 : smpl > 32767 ? 32767 : smpl;
    *outr = smpr < -32768 ? -32768 : smpr > 32767 ? 32767 : smpr;
}

/* ---- YM3438: timers and busy ----------------------------------------------------- */

static inline void m1ym_update_timer(m1ym_t *y, int t, int enable, int32_t delta) {
    if (enable && !y->running[t]) {
        int32_t period = t == 0 ? 1024 - (int32_t)(((uint32_t)y->r24 << 2) | (y->r25 & 3u))
                                : 16 * (256 - (int32_t)y->r26);
        period += delta;
        y->timer_at[t] = (int64_t)y->now + (int64_t)period * M1SND_YM_PRESCALE;
        y->running[t] = 1;
    } else if (!enable) {
        y->timer_at[t] = -1;
        y->running[t] = 0;
    }
}

static inline void m1ym_mode_write(m1ym_t *y, uint8_t data) {
    y->r27 = data;
    uint8_t reset = (uint8_t)(((data >> 5) & 1) << 1 | ((data >> 4) & 1));
    y->status &= (uint8_t)~reset;
    /* timer B's x16 is free-running, so its first tick comes early (ymfm) */
    m1ym_update_timer(y, 1, (data >> 1) & 1, -(int32_t)((y->now / M1SND_YM_PRESCALE) & 15));
    m1ym_update_timer(y, 0, data & 1, 0);
}

static inline void m1ym_advance(m1ym_t *y, uint64_t to) {
    for (;;) {
        int t = -1;
        for (int i = 0; i < 2; i++)
            if (y->timer_at[i] >= 0 && (uint64_t)y->timer_at[i] <= to && (t < 0 || y->timer_at[i] < y->timer_at[t])) t = i;
        if (t < 0) break;
        y->now = (uint64_t)y->timer_at[t];
        if (t == 0 && (y->r27 & 0x04)) y->status |= 1;
        else if (t == 1 && (y->r27 & 0x08)) y->status |= 2;
        y->running[t] = 0;
        m1ym_update_timer(y, t, 1, 0);
    }
    if (to > y->now) y->now = to;
}

static inline uint8_t m1ym_read(const m1ym_t *y, uint32_t offset) {
    if ((offset & 3) != 0) return 0;
    return (uint8_t)(y->status | (y->now < y->busy_end ? 0x80 : 0));
}

static inline void m1ym_write(m1ym_t *y, uint32_t offset, uint8_t data) {
    switch (offset & 3) {
    case 0: y->addr = data; return;
    case 2: y->addr = (uint16_t)(0x100 | data); return;
    case 1:
        if (y->addr & 0x100) return;
        switch (y->addr) {
        case 0x24: y->r24 = data; break;
        case 0x25: y->r25 = data; break;
        case 0x26: y->r26 = data; break;
        case 0x27: m1ym_mode_write(y, data); break;
        case 0x28:
            if ((data & 0xF0) && !y->keyon_warned) {
                y->keyon_warned = 1;
                LOG_WARN("model1 sound: YM3438 FM key-on (0x28 = 0x%02X), the board has no FM voices", data);
            }
            break;
        }
        break;
    case 3:
        if (!(y->addr & 0x100)) return;
        break;
    }
    y->busy_end = y->now + 32u * 6u;
}

/* ---- the board ------------------------------------------------------------------ */

static inline void m1snd_reset(m1snd_t *s) {
    mpcm_tables_build();
    memset(s, 0, sizeof *s);
    for (int c = 0; c < 2; c++) s->pcm[c].cur_slot = 0;
    s->ym.timer_at[0] = s->ym.timer_at[1] = -1;
}

/* A byte off the serial line into the 68000's receiver. A receiver that is
 * off ignores it; one that still holds a byte loses the old one (overrun). */
static inline void m1snd_rx(m1snd_t *s, uint8_t b) {
    if (!s->rxe) { s->rx_dropped++; return; }
    if (s->rxrdy) s->rx_overruns++;
    s->rx_data = b;
    s->rxrdy = 1;
}

static inline uint8_t m1snd_uart_read(m1snd_t *s, uint32_t reg) {
    if (reg == 0) { s->rxrdy = 0; return s->rx_data; }
    return (uint8_t)(0x85 | (s->rxrdy ? 0x02 : 0));   /* TxRDY, TxEMPTY, DSR; RxRDY */
}

static inline void m1snd_uart_write(m1snd_t *s, uint32_t reg, uint8_t v) {
    if (reg == 0) {
        if (((s->tx_w - s->tx_r) & 0xFFu) < M1SND_TX_FIFO) s->tx[s->tx_w++ % M1SND_TX_FIFO] = v;
        return;
    }
    /* The first control write after a reset is the mode, the rest commands;
     * command bit 6 is an internal reset back to expecting a mode. */
    if (!s->mode_done) { s->mode_done = 1; return; }
    if (v & 0x40) { s->mode_done = 0; s->rxe = 0; return; }
    s->rxe = (v >> 2) & 1;
}

static inline bool m1snd_tx_ready(const m1snd_t *s) { return s->tx_r != s->tx_w; }
static inline uint8_t m1snd_tx_pop(m1snd_t *s) { return s->tx[s->tx_r++ % M1SND_TX_FIFO]; }

/* The 68000 clocks to the next board sample: 10 MHz / 44.1 kHz = 226.757. */
static inline int32_t m1snd_sample_cycles(m1snd_t *s) {
    s->cyc_frac += M1SND_CPU_HZ % M1SND_OUT_HZ;
    int32_t n = (int32_t)(M1SND_CPU_HZ / M1SND_OUT_HZ);
    if (s->cyc_frac >= M1SND_OUT_HZ) { s->cyc_frac -= M1SND_OUT_HZ; n++; }
    return n;
}

/* The board's output sample: the two MultiPCMs at 0.5 each (segam1audio's
 * routes; the YM3438 makes nothing). samples: romset.samples, chip 1's 4 MB
 * then chip 2's. */
static inline void m1snd_sample(m1snd_t *s, const uint8_t *samples, uint32_t size, int16_t *l, int16_t *r) {
    int32_t l1 = 0, r1 = 0, l2 = 0, r2 = 0;
    uint32_t n1 = size < M1SND_PCM_SIZE ? size : M1SND_PCM_SIZE;
    uint32_t n2 = size > M1SND_PCM_SIZE ? size - M1SND_PCM_SIZE : 0;
    if (n1) mpcm_sample(&s->pcm[0], samples, n1, &l1, &r1);
    if (n2) mpcm_sample(&s->pcm[1], samples + M1SND_PCM_SIZE, n2, &l2, &r2);
    int32_t ol = (l1 + l2) >> 1, or_ = (r1 + r2) >> 1;
    *l = (int16_t)ol;
    *r = (int16_t)or_;
}

#endif /* MODEL1_SOUND_H */
