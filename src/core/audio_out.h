/*
 * audio_out.h — play the sound board's output on the host.
 *
 * The emu thread fills g_sound's ring at 44.1 kHz in slice-sized bursts
 * (board/sound.h); sokol_audio's callback drains it at the device's rate. The
 * two clocks never quite agree, so the callback resamples with a ratio nudged
 * by how full the ring is: it plays a touch faster when the ring runs long and
 * slower when it runs short, holding about `target` frames of latency without
 * clicks. An empty ring (emulator stopped or behind) fades to silence.
 *
 * HOW MUCH IS QUEUED IS THE HOST'S CHOICE (audio_out_config_t), because it is a
 * trade between latency and underruns and the hosts differ:
 *   - The desktop build keeps AUDIO_TARGET (~186 ms). Its emu thread delivers a
 *     slice at a time and can run long (STF's texture loads), and an underrun on
 *     a live stream is worse than latency nobody is measuring.
 *   - The web build steps the board from the same thread that feeds the audio
 *     callback, a slice per display frame, so the queue only has to cover one
 *     callback plus a late frame or two: ~70 ms, with a 1024-frame callback.
 *     At the desktop figure a punch was heard a quarter of a second after it
 *     landed.
 *
 * A RING FAR OVER ITS TARGET IS STALE, NOT EARLY. The rate nudge is 1% at most,
 * so it takes ~19 s to work off a full ring, and a full ring is what a browser
 * produces every time: WebAudio stays suspended until the first click or key, and
 * the board has been filling the ring since it booted. Past `resync_above` the
 * reader jumps forward to the target instead -- one skip, faded like an underrun,
 * rather than seconds of sound running behind the picture.
 *
 * The board's output sits on a DC offset (about 5000 of 32768 in STF — the DSP
 * path; MAME's WAV has the same one). A real cabinet's amplifier is AC-coupled,
 * so the host side takes it out with a one-pole high-pass (~5 Hz); the board's
 * own samples stay as they are, for grading.
 */
#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

#include "sokol_audio.h"
#include "sokol_log.h"
#include "log.h"
#include "../board/sound.h"

#define AUDIO_TARGET 8192.0     /* frames of 44.1 kHz audio kept queued (~186 ms) */
#define AUDIO_RESUME 256        /* frames faded back in after an underrun (~5 ms) */

/* What a host asks for. Zero in any field means the desktop default. */
typedef struct {
    int    buffer_frames;       /* the device callback's size; 0 = sokol_audio's 2048 */
    double target;              /* 44.1 kHz frames kept queued; 0 = AUDIO_TARGET */
    bool   smooth_fill;         /* steer the rate by a low-passed fill, not the instantaneous one */
} audio_out_config_t;

typedef struct {
    bool     ready;
    double   target;            /* frames of 44.1 kHz audio kept queued */
    uint32_t resync_above;      /* a fill past this is stale: jump to the target */
    bool     smooth_fill;
    double   fill_lp;           /* low-passed fill, when smooth_fill */
    double   fill_k;            /* its per-output-sample coefficient (~0.25 s) */
    uint64_t resyncs;
    double   pos;               /* fractional read position ahead of out_r */
    float    last_l, last_r;
    float    hold_l, hold_r;                /* level held through an underrun */
    uint32_t resume;                        /* frames left of the fade back in */
    float    dc_xl, dc_xr, dc_yl, dc_yr;    /* DC blocker state */
    uint32_t rate;
    uint64_t underruns;
} audio_out_t;

static audio_out_t g_audio_out;

static void audio_out_cb(float *buf, int frames, int channels, void *ud) {
    (void)ud;
    audio_out_t *a = &g_audio_out;
    const uint32_t mask = SOUND_OUT_FRAMES - 1;
    for (int i = 0; i < frames; i++) {
        uint32_t r = g_sound.out_r, w = g_sound.out_w;
        uint32_t fill = (w - r) & mask;
        float l, rr;
        if (fill > a->resync_above) {
            /* Stale: drop the oldest audio down to the target, and come back in
             * through the same fade an underrun uses, so the skip is not a click. */
            uint32_t skip = fill - (uint32_t)a->target;
            r = (r + skip) & mask;
            g_sound.out_r = r;
            fill -= skip;
            a->pos = 0.0;
            a->fill_lp = (double)fill;
            a->hold_l = a->last_l; a->hold_r = a->last_r;
            a->resume = AUDIO_RESUME;
            a->resyncs++;
        }
        if (fill < 2) {
            /* Hold the last level (the DC blocker then fades it out without a
             * click) and arm a fade back in: coming off a hold straight onto a
             * live sample is a step, and a step is the click you hear. */
            a->hold_l = a->last_l; a->hold_r = a->last_r;
            a->resume = AUDIO_RESUME;
            l = a->last_l; rr = a->last_r;
            a->underruns++;
        } else {
            float f = (float)a->pos;
            const int16_t *p0 = g_sound.out + r * 2, *p1 = g_sound.out + ((r + 1) & mask) * 2;
            l  = ((float)p0[0] + ((float)p1[0] - (float)p0[0]) * f) / 32768.0f;
            rr = ((float)p0[1] + ((float)p1[1] - (float)p0[1]) * f) / 32768.0f;
            if (a->resume) {
                float g = 1.0f - (float)a->resume / (float)AUDIO_RESUME;
                l  = a->hold_l + (l  - a->hold_l) * g;
                rr = a->hold_r + (rr - a->hold_r) * g;
                a->resume--;
            }
            a->last_l = l; a->last_r = rr;
            /* The instantaneous fill saws up a slice at a time and down a callback
             * at a time. Against a small target that swing is a large part of the
             * error, and steering by it wobbles the pitch at the callback rate; a
             * host with a small target steers by the average instead. The loop's
             * own time constant (target / 1% of the rate: seconds) is far longer
             * than the filter's, so it stays well damped. */
            double level = (double)fill;
            if (a->smooth_fill) {
                a->fill_lp += (level - a->fill_lp) * a->fill_k;
                level = a->fill_lp;
            }
            double adj = (level - a->target) / a->target;
            adj = adj < -1.0 ? -1.0 : adj > 1.0 ? 1.0 : adj;
            a->pos += (double)SOUND_RATE / (double)a->rate * (1.0 + 0.010 * adj);
            uint32_t adv = (uint32_t)a->pos;
            if (adv > fill - 1) adv = fill - 1;
            a->pos -= adv;
            g_sound.out_r = (r + adv) & mask;
        }
        const float R = 0.9993f;
        float yl = l - a->dc_xl + R * a->dc_yl, yr = rr - a->dc_xr + R * a->dc_yr;
        a->dc_xl = l; a->dc_xr = rr; a->dc_yl = yl; a->dc_yr = yr;
        buf[i * channels] = yl;
        if (channels > 1) buf[i * channels + 1] = yr;
    }
}

/* The drain's own settings, apart from opening a device: a host that opens its
 * own (main_sdl.c's SDL stream) still has to come through here. Leaving the
 * fields zero is not "the default" -- target 0 divides by zero in the rate
 * steering and resync_above 0 calls every callback stale, which drains the ring
 * and underruns on every one. */
static inline void audio_out_configure(const audio_out_config_t *cfg, uint32_t rate) {
    audio_out_t *a = &g_audio_out;
    a->target      = (cfg && cfg->target > 0.0) ? cfg->target : AUDIO_TARGET;
    a->smooth_fill = cfg && cfg->smooth_fill;
    a->fill_lp     = a->target;
    /* Stale is three times the target, but never so close to the top of the ring
     * that an ordinary swing reaches it: with the desktop target that cap is what
     * applies, and it sits where the ring was already about to drop samples. */
    uint32_t stale = (uint32_t)(a->target * 3.0), cap = SOUND_OUT_FRAMES / 8u * 7u;
    a->resync_above = stale < cap ? stale : cap;
    a->rate        = rate;
    a->fill_k      = 1.0 / (0.25 * (double)rate);
}

static inline void audio_out_init_ex(const audio_out_config_t *cfg) {
    audio_out_t *a = &g_audio_out;
    audio_out_configure(cfg, SOUND_RATE);   /* the device's real rate, below */

    saudio_setup(&(saudio_desc){
        .num_channels       = 2,
        .buffer_frames      = cfg ? cfg->buffer_frames : 0,
        .stream_userdata_cb = audio_out_cb,
        .logger.func        = slog_func,
    });
    if (!saudio_isvalid()) {
        LOG_WARN("audio: no output device — sound disabled");
        return;
    }
    a->rate   = (uint32_t)saudio_sample_rate();
    a->fill_k = 1.0 / (0.25 * (double)a->rate);
    a->ready  = true;
    LOG_INFO("audio: %u Hz output, resampled from the board's 44100 Hz; %d-frame callback, %.0f frames (%.0f ms) queued",
             a->rate, saudio_buffer_frames(), a->target, a->target * 1000.0 / (double)SOUND_RATE);
}

static inline void audio_out_init(void) { audio_out_init_ex(NULL); }

/* ---- The push model ---------------------------------------------------------
 *
 * Everything above is PULL: a device callback asks for frames and the queue
 * lives in the board's ring. A host whose audio runs where this code cannot be
 * called from has to PUSH instead. The web build is that host: an AudioWorklet
 * runs on the browser's audio thread, which without SharedArrayBuffer shares no
 * memory with the wasm heap, so the page posts it chunks and the worklet keeps
 * the queue (web/site/m2hle-audio-worklet.js).
 *
 * It is worth the second model. The pull path in a browser is a
 * ScriptProcessorNode, which runs on the main thread and double-buffers: a
 * 1024-frame node costs ~46 ms before the device is reached, on top of a queue
 * that has to cover a whole callback. The worklet takes 128 frames at a time,
 * so the only latency left to choose is the jitter cushion.
 *
 * audio_out_drain renders everything the board has produced since the last call
 * -- same interpolation, same DC blocker -- and returns the frames written
 * (interleaved stereo). `nudge` is the rate correction, the same sign as the
 * pull model's: positive when the far queue runs long, so each output frame
 * covers a little more of the source and fewer are produced. The caller gets it
 * from whoever holds the queue.
 */
static inline void audio_out_push_begin(uint32_t device_rate) {
    audio_out_t *a = &g_audio_out;
    a->rate = device_rate ? device_rate : SOUND_RATE;
    a->pos  = 0.0;
    a->dc_xl = a->dc_xr = a->dc_yl = a->dc_yr = 0.0f;
    /* Whatever the board produced before there was anywhere to send it is old. */
    g_sound.out_r = g_sound.out_w;
}

static inline int audio_out_drain(float *buf, int cap_frames, double nudge) {
    audio_out_t *a = &g_audio_out;
    const uint32_t mask = SOUND_OUT_FRAMES - 1;
    const double step = (double)SOUND_RATE / (double)a->rate * (1.0 + nudge);
    int n = 0;
    while (n < cap_frames) {
        uint32_t r = g_sound.out_r, fill = (g_sound.out_w - r) & mask;
        if (fill < 2) break;                /* one frame stays behind to interpolate from */
        float f = (float)a->pos;
        const int16_t *p0 = g_sound.out + r * 2, *p1 = g_sound.out + ((r + 1) & mask) * 2;
        float l  = ((float)p0[0] + ((float)p1[0] - (float)p0[0]) * f) / 32768.0f;
        float rr = ((float)p0[1] + ((float)p1[1] - (float)p0[1]) * f) / 32768.0f;
        a->pos += step;
        uint32_t adv = (uint32_t)a->pos;
        if (adv > fill - 1) adv = fill - 1;
        a->pos -= adv;
        g_sound.out_r = (r + adv) & mask;

        const float R = 0.9993f;
        float yl = l - a->dc_xl + R * a->dc_yl, yr = rr - a->dc_xr + R * a->dc_yr;
        a->dc_xl = l; a->dc_xr = rr; a->dc_yl = yl; a->dc_yr = yr;
        buf[n * 2]     = yl;
        buf[n * 2 + 1] = yr;
        n++;
    }
    return n;
}

/* Frames of board audio waiting to be played, for a host that reports it. */
static inline uint32_t audio_out_queued(void) {
    return (g_sound.out_w - g_sound.out_r) & (SOUND_OUT_FRAMES - 1);
}

static inline void audio_out_shutdown(void) {
    if (g_audio_out.ready) saudio_shutdown();
    g_audio_out.ready = false;
}

#endif /* AUDIO_OUT_H */
