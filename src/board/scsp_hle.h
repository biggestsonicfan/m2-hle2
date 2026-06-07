/*
 * scsp_hle.h — SCSP High-Level Emulation: raw PCM voice mixer.
 *
 * Intercepts SCSP slot key-on/key-off events from sound.h (written via the
 * 68K SCSP register window), and plays back raw PCM samples from the shared
 * 512 KB wave RAM buffer.
 *
 * Hardware facts (MAME scsp.cpp):
 *   - 32 slots, each 0x20 bytes in the SCSP register window
 *   - SCSP sample clock: 22.5792 MHz / 512 = 44100 Hz
 *   - PCM8B=0: 16-bit signed big-endian samples
 *   - PCM8B=1: 8-bit signed samples
 *   - SA: start address (byte offset into 512 KB wave RAM)
 *   - LSA: loop start address (sample index relative to SA)
 *   - LEA: loop end address (sample index relative to SA)
 *   - Loop: on reaching LEA, voice wraps back to LSA
 *
 * Thread model:
 *   Emu thread  → sound.h key-on/off → scsp_ring_push() into SPSC ring
 *   Audio thread (sokol_audio callback) → drains ring, updates voice state,
 *                 mixes active voices from wave RAM, outputs float stereo
 */
#ifndef SCSP_HLE_H
#define SCSP_HLE_H

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#include "sokol_audio.h"
#include "sokol_log.h"   /* slog_func (passed to saudio_setup in scsp_hle_init) */
#include "log.h"

/* ---- SPSC event ring buffer --------------------------------------------- */

#define SCSP_RING_SIZE  2048u
#define SCSP_RING_MASK  (SCSP_RING_SIZE - 1u)

typedef enum {
    SCSP_EVT_KEYON  = 1,
    SCSP_EVT_KEYOFF = 2,
} scsp_evt_kind_t;

/* Slot event — carries all slot parameters needed for playback. */
typedef struct {
    uint8_t  type;      /* scsp_evt_kind_t */
    uint8_t  slot;      /* 0–31 */
    uint8_t  pcm8b;     /* 0=16-bit, 1=8-bit */
    uint8_t  loop;      /* LPCTL: 0=no loop, else loop */
    uint32_t sa;        /* start address (byte offset into wave RAM) */
    uint32_t lsa;       /* loop start (sample index from SA) */
    uint32_t lea;       /* loop end   (sample index from SA) */
    uint16_t oct;       /* OCT pitch field (0..15)  */
    uint16_t fns;       /* FNS pitch field (0..1023) */
    uint8_t  tl;        /* total level attenuation 0=loud..255=silent */
    uint8_t  disdl;     /* direct send level 0=off..7=full */
    uint8_t  dipan;     /* direct pan (bit4=side, bits[3:0]=level) */
    uint8_t  _pad;
} scsp_evt_t;

typedef struct {
    scsp_evt_t       buf[SCSP_RING_SIZE];
    volatile unsigned head;
    volatile unsigned tail;
} scsp_ring_t;

static inline void scsp_ring_push(scsp_ring_t *r, scsp_evt_t e) {
    unsigned next = (r->head + 1u) & SCSP_RING_MASK;
    if (next == r->tail) return; /* full — drop */
    r->buf[r->head] = e;
#ifdef _MSC_VER
    _ReadWriteBarrier();
#else
    __sync_synchronize();
#endif
    r->head = next;
}

static inline int scsp_ring_pop(scsp_ring_t *r, scsp_evt_t *out) {
    if (r->tail == r->head) return 0;
#ifdef _MSC_VER
    _ReadWriteBarrier();
#else
    __sync_synchronize();
#endif
    *out    = r->buf[r->tail];
    r->tail = (r->tail + 1u) & SCSP_RING_MASK;
    return 1;
}

/* ---- Voice state --------------------------------------------------------- */

#define SCSP_VOICES 32

typedef struct {
    bool     active;
    bool     pcm8b;
    bool     loop;
    bool     releasing;     /* key-off issued; env ramping down to silence */
    uint32_t sa;            /* byte offset into wave RAM */
    uint32_t lsa;           /* loop start, sample index */
    uint32_t lea;           /* loop end, sample index */
    double   pos;           /* current position, fractional sample index */
    double   step;          /* phase increment per output sample (pitch) */
    float    vol_l, vol_r;  /* per-voice linear gain (TL/DISDL/DIPAN) */
    float    env;           /* current envelope gain 0..1 (declick ramp) */
    float    env_step;      /* per-sample env delta toward target */
    uint32_t age;           /* diagnostic: audio callbacks since key-on */
} scsp_voice_t;

/* Linear declick envelope rates (per output sample @44.1kHz). */
#define SCSP_ENV_ATTACK   (1.0f / 64.0f)    /* ~1.5 ms ramp up   */
#define SCSP_ENV_RELEASE  (1.0f / 512.0f)   /* ~12 ms ramp down  */

/* ---- Global state ------------------------------------------------------- */

typedef struct {
    scsp_ring_t        ring;
    scsp_voice_t       voices[SCSP_VOICES];
    bool               loaded;
    const uint8_t     *wave;        /* pointer to g_sound.wave (512 KB) */
    uint32_t           wave_size;
    const uint8_t     *comm;        /* pointer to g_sound.comm (SCSP slot regs) */
    uint32_t           comm_size;
    const uint8_t     *sample_rom;  /* romset.samples — not owned; NULL until loaded */
    uint32_t           sample_rom_size;
    uint32_t           keyon_count; /* diagnostic: incremented on every KEYON push */
    uint32_t           keyoff_count;/* diagnostic: incremented on every KEYOFF push */
    uint32_t           cb_count;    /* diagnostic: audio callback fire count       */
    uint32_t           out_rate;    /* sokol_audio output sample rate (Hz)         */
    /* diagnostics: parameters of the most recent key-on */
    uint16_t           dbg_oct, dbg_fns;
    uint8_t            dbg_tl, dbg_disdl, dbg_dipan;
    float              dbg_step, dbg_vol_l, dbg_vol_r;
} scsp_hle_t;

/* SCSP sample clock = 22.5792 MHz / 512 = 44100 Hz (native voice playback rate). */
#define SCSP_NATIVE_RATE 44100.0

static scsp_hle_t g_scsp = {0};

/* ---- Sample read helpers ------------------------------------------------- */

static inline float scsp_read_sample(const scsp_hle_t *sc, const scsp_voice_t *v, uint32_t idx) {
    if (v->pcm8b) {
        uint32_t byte_off = v->sa + idx;
        const uint8_t *src;
        uint32_t       src_size;
        if (byte_off < sc->wave_size) {
            src = sc->wave; src_size = sc->wave_size;
        } else if (sc->sample_rom && byte_off - sc->wave_size < sc->sample_rom_size) {
            byte_off -= sc->wave_size; src = sc->sample_rom; src_size = sc->sample_rom_size;
        } else {
            return 0.0f;
        }
        if (byte_off >= src_size) return 0.0f;
        return (float)(int8_t)src[byte_off] / 128.0f;
    } else {
        uint32_t byte_off = v->sa + idx * 2u;
        const uint8_t *src;
        uint32_t       src_size;
        if (byte_off < sc->wave_size) {
            src = sc->wave; src_size = sc->wave_size;
        } else if (sc->sample_rom && byte_off - sc->wave_size < sc->sample_rom_size) {
            byte_off -= sc->wave_size; src = sc->sample_rom; src_size = sc->sample_rom_size;
        } else {
            return 0.0f;
        }
        if (byte_off + 1u >= src_size) return 0.0f;
        int16_t s = (int16_t)((uint16_t)src[byte_off] << 8 | src[byte_off + 1]);
        return (float)s / 32768.0f;
    }
}

/* ---- live slot-register read -------------------------------------------- */

/* Recompute a voice's pitch (step) and volume/pan (vol_l/vol_r) from the SCSP
 * slot registers the 68K writes into comm[].  The driver programs pitch/TL/pan
 * *after* key-on (volume envelopes, vibrato), so the key-on snapshot is stale;
 * reading live each output block tracks the real dynamics.
 *
 * Slot N regs at comm[N*0x20]:  +0x0D = TL,  +0x10/11 = OCT[14:11]/FNS[9:0],
 *   +0x16/17 = DISDL[15:13]/DIPAN[12:8]. */
static inline void scsp_refresh_voice(scsp_voice_t *v, int slot, double out_rate) {
    if (!g_scsp.comm) return;
    uint32_t off = (uint32_t)slot * 0x20u;
    if (off + 0x18u > g_scsp.comm_size) return;
    const uint8_t *c = g_scsp.comm;

    uint32_t pw  = ((uint32_t)c[off + 0x10] << 8) | c[off + 0x11];
    int      oct = (int)((pw >> 11) & 0xF);
    int      fns = (int)(pw & 0x3FF);
    int      oct_s = (oct ^ 8) - 8;                 /* signed -8..+7 */
    double   ratio = (1024.0 + (double)fns) / 1024.0;
    if (oct_s >= 0) ratio *= (double)(1u << oct_s);
    else            ratio /= (double)(1u << (-oct_s));
    v->step = ratio * (SCSP_NATIVE_RATE / out_rate);

    uint8_t  tl    = c[off + 0x0D];
    uint32_t dw    = ((uint32_t)c[off + 0x16] << 8) | c[off + 0x17];
    int      disdl = (int)((dw >> 13) & 7);
    int      dipan = (int)((dw >> 8) & 0x1F);
    float    vol   = powf(2.0f, -(float)tl / 16.0f); /* TL: 0.375 dB/step */
    if (disdl) vol *= powf(2.0f, (float)disdl - 7.0f);  /* DISDL 0 = full (no DSP path) */
    float    patt  = (dipan & 0x0F) ? powf(2.0f, -(float)(dipan & 0x0F) / 2.0f) : 1.0f;
    if (dipan & 0x10) { v->vol_l = vol * patt; v->vol_r = vol;        }  /* pan right */
    else              { v->vol_l = vol;        v->vol_r = vol * patt; }  /* pan left  */
}

/* ---- sokol_audio render callback ---------------------------------------- */

static void scsp_audio_cb(float *buf, int num_frames, int num_channels, void *ud) {
    (void)ud; (void)num_channels;

    g_scsp.cb_count++;

    double out_rate = g_scsp.out_rate ? (double)g_scsp.out_rate : SCSP_NATIVE_RATE;

    /* Drain ring: apply key-on / key-off events */
    scsp_evt_t e;
    while (scsp_ring_pop(&g_scsp.ring, &e)) {
        if (e.slot >= SCSP_VOICES) continue;
        scsp_voice_t *v = &g_scsp.voices[e.slot];
        if (e.type == SCSP_EVT_KEYON) {
            v->sa    = e.sa;
            v->lsa   = e.lsa;
            v->lea   = e.lea;
            v->pcm8b = e.pcm8b;
            v->loop  = e.loop != 0;
            v->pos   = 0.0;

            /* Pitch: step = (1024+FNS)/1024 * 2^((OCT^8)-8), resampled to output. */
            int oct_s = ((int)e.oct ^ 8) - 8;          /* signed -8..+7 */
            double ratio = (1024.0 + (double)e.fns) / 1024.0;
            if (oct_s >= 0) ratio *= (double)(1u << oct_s);
            else            ratio /= (double)(1u << (-oct_s));
            v->step = ratio * (SCSP_NATIVE_RATE / out_rate);

            /* Volume: TL attenuation (0.375 dB/step) × DISDL send level.
             * DISDL 0 = "no direct send" (effects-only on real HW); we have no DSP
             * effects path, so treat 0 as full level rather than muting the voice. */
            int disdl = e.disdl ? (int)e.disdl : 7;
            float vol = powf(2.0f, -(float)e.tl / 16.0f) * powf(2.0f, (float)disdl - 7.0f);
            /* The driver often programs TL/DISDL *after* key-on (volume envelopes),
             * so at key-on they can read as silent (TL=0xFF/DISDL=0).  Until we read
             * them live, fall back to an audible default rather than muting. */
            if (vol < 0.02f) vol = 0.6f;
            float patt = (e.dipan & 0x0F)
                       ? powf(2.0f, -(float)(e.dipan & 0x0F) / 2.0f) : 1.0f;
            if (e.dipan & 0x10) { v->vol_l = vol * patt; v->vol_r = vol; }  /* pan right */
            else                { v->vol_l = vol; v->vol_r = vol * patt; }  /* pan left  */

            v->env       = 0.0f;            /* attack ramp from silence (declick) */
            v->env_step  = SCSP_ENV_ATTACK;
            v->releasing = false;
            v->active    = true;
            v->age       = 0;

            g_scsp.dbg_oct = e.oct; g_scsp.dbg_fns = e.fns; g_scsp.dbg_tl = e.tl;
            g_scsp.dbg_disdl = e.disdl; g_scsp.dbg_dipan = e.dipan;
            g_scsp.dbg_step = (float)v->step;
            g_scsp.dbg_vol_l = v->vol_l; g_scsp.dbg_vol_r = v->vol_r;
        } else { /* KEYOFF: ramp down to silence, then deactivate (declick) */
            if (v->active) { v->releasing = true; v->env_step = -SCSP_ENV_RELEASE; }
        }
    }

    memset(buf, 0, (size_t)num_frames * 2 * sizeof(float));

    const float master = 0.5f;
    for (int slot = 0; slot < SCSP_VOICES; slot++) {
        scsp_voice_t *v = &g_scsp.voices[slot];
        if (!v->active) continue;

        v->age++;
        /* Track the driver's live pitch/volume/pan for this block (not key-off). */
        if (!v->releasing) scsp_refresh_voice(v, slot, out_rate);

        for (int i = 0; i < num_frames; i++) {
            /* Loop / end handling */
            if (v->lea > 0 && v->pos >= (double)v->lea) {
                if (v->loop && v->lsa < v->lea)
                    v->pos -= (double)(v->lea - v->lsa);   /* wrap to loop start */
                else { v->active = false; break; }          /* one-shot end */
            }

            float s = scsp_read_sample(&g_scsp, v, (uint32_t)v->pos);
            float g = v->env * master;
            buf[i * 2 + 0] += s * g * v->vol_l;
            buf[i * 2 + 1] += s * g * v->vol_r;

            /* Advance declick envelope */
            v->env += v->env_step;
            if (v->env >= 1.0f) { v->env = 1.0f; v->env_step = 0.0f; }
            else if (v->env <= 0.0f) {
                v->env = 0.0f;
                if (v->releasing) { v->active = false; break; }
            }

            v->pos += v->step;
        }
    }
}

/* ---- Public API --------------------------------------------------------- */

static inline void scsp_hle_init(const uint8_t *wave_ram, uint32_t wave_size,
                                 const uint8_t *comm, uint32_t comm_size) {
    memset(&g_scsp, 0, sizeof(g_scsp));
    g_scsp.wave      = wave_ram;
    g_scsp.wave_size = wave_size;
    g_scsp.comm      = comm;
    g_scsp.comm_size = comm_size;

    saudio_setup(&(saudio_desc){
        .num_channels       = 2,
        .stream_userdata_cb = scsp_audio_cb,
        .user_data          = NULL,
        .logger.func        = slog_func,
    });

    if (!saudio_isvalid()) {
        LOG_WARN("scsp_hle: saudio_setup failed (no audio device?) — audio disabled");
        return;
    }
    g_scsp.loaded   = true;
    g_scsp.out_rate = (uint32_t)saudio_sample_rate();
    LOG_INFO("scsp_hle: PCM mixer ready (pitch+env+vol), %d Hz", saudio_sample_rate());
}

static inline void scsp_hle_shutdown(void) {
    if (g_scsp.loaded) saudio_shutdown();
    memset(&g_scsp, 0, sizeof(g_scsp));
}

static inline void scsp_hle_set_sample_rom(const uint8_t *buf, uint32_t size) {
    g_scsp.sample_rom      = buf;
    g_scsp.sample_rom_size = size;
}

/*
 * Fire slot 31 from the start of the sample ROM (sa = wave_size so
 * scsp_read_sample falls through into sample_rom[0]).  16-bit signed
 * big-endian, no loop — plays until the end of sample_rom_size.
 * Call once after scsp_hle_set_sample_rom() to verify the pipeline.
 */
static inline void scsp_hle_test_play_roms(void) {
    if (!g_scsp.loaded || !g_scsp.sample_rom) return;
    uint32_t lea = g_scsp.sample_rom_size / 2u; /* sample count for 16-bit */
    scsp_ring_push(&g_scsp.ring, (scsp_evt_t){
        SCSP_EVT_KEYON, 31, 0, 0,
        g_scsp.wave_size, /* sa: byte offset that lands at sample_rom[0] */
        0, lea
    });
    LOG_INFO("scsp_hle: test play — slot 31, sample_rom[0..%u samples]", lea);
}

#endif /* SCSP_HLE_H */
