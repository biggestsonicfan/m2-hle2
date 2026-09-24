/*
 * audio_out.h — play the sound board's output on the host.
 *
 * The emu thread fills g_sound's ring at 44.1 kHz in slice-sized bursts
 * (board/sound.h); sokol_audio's callback drains it at the device's rate. The
 * two clocks never quite agree, so the callback resamples with a ratio nudged
 * by how full the ring is: it plays a touch faster when the ring runs long and
 * slower when it runs short, holding about `target` frames of latency without
 * clicks.
 *
 * A RING THAT RUNS DRY IS REFILLED TO ITS TARGET BEFORE IT PLAYS AGAIN. The
 * board stops producing whenever it stops running: a netplay stall waiting on
 * the peer's input, a pause, a long texture load. When it starts again it runs
 * at 60 Hz, not faster -- the run loop's catch-up clamp gives the lost time
 * up, and lockstep could not run ahead of the peer anyway -- so nothing ever
 * pays the ring back except the 1% rate nudge, which takes ten seconds per
 * 100 ms. Resuming as soon as there were two frames to play left the ring
 * hovering at empty, and every callback until the nudge caught up underran
 * again: a burst of clicks for every lag spike. So a ring about to run dry
 * fades out over the audio it still holds (low_water is sized so the fade
 * ends before the ring does), stays silent until the board has put `target`
 * frames back, and fades in from there: one clean gap per stall, and the
 * full cushion against the next one.
 *
 * THE FADES ARE APPLIED AFTER THE DC BLOCKER, not before. The board's output
 * sits on a large DC offset (below), so fading the raw samples towards zero is
 * itself a step, which the high-pass passes as a thump. After the blocker the
 * signal is centred on zero, and on the way back in the blocker is re-seeded
 * on the first new frame, so the jump from the old level to the new one is
 * never seen as a step either.
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
 * reader jumps forward to the target instead -- one skip, faded out and back in
 * like a stall, rather than seconds of sound running behind the picture.
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
#define AUDIO_FADE   512        /* output frames each fade out or in takes (~11 ms) */

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
    uint32_t low_water;         /* a fill under this starts the fade out */
    bool     smooth_fill;
    double   fill_lp;           /* low-passed fill, when smooth_fill */
    double   fill_k;            /* its per-output-sample coefficient (~0.25 s) */
    uint64_t resyncs;
    double   pos;               /* fractional read position ahead of out_r */
    float    gain;              /* 0..1, the fade (smoothstepped on the way out) */
    bool     fading;            /* heading for zero gain, and then... */
    bool     skip_after_fade;   /* ...a resync (true) or a refill (false) */
    bool     refilling;         /* silent until the ring is back at its target */
    bool     reseed;            /* the next frame restarts the DC blocker on itself */
    float    dc_xl, dc_xr, dc_yl, dc_yr;    /* DC blocker state */
    uint32_t rate;
    uint64_t underruns;         /* times the ring ran dry and was refilled */
} audio_out_t;

static audio_out_t g_audio_out;

static void audio_out_cb(float *buf, int frames, int channels, void *ud) {
    (void)ud;
    audio_out_t *a = &g_audio_out;
    const uint32_t mask = SOUND_OUT_FRAMES - 1;
    const float fade_step = 1.0f / (float)AUDIO_FADE;
    for (int i = 0; i < frames; i++) {
        uint32_t r = g_sound.out_r, w = g_sound.out_w;
        uint32_t fill = (w - r) & mask;
        float yl = 0.0f, yr = 0.0f;

        if (a->refilling) {
            if ((double)fill < a->target) goto out;     /* silence, and consume nothing */
            a->refilling = false;
            a->reseed    = true;                        /* gain is 0: it fades in below */
            a->pos       = 0.0;
            a->fill_lp   = (double)fill;
        }
        if (fill < 2) {
            /* Dry before the fade finished, which low_water is sized to prevent
             * (the fill is read afresh every frame). There is nothing to play:
             * cut to silence and refill. */
            a->gain = 0.0f; a->fading = false; a->refilling = true;
            a->underruns++;
            goto out;
        }
        if (!a->fading) {
            if (fill > a->resync_above)   { a->fading = true; a->skip_after_fade = true;  }
            else if (fill < a->low_water) { a->fading = true; a->skip_after_fade = false; }
        } else if (!a->skip_after_fade && fill >= 2 * a->low_water) {
            a->fading = false;          /* the board came back in time: fade back up */
        }

        {
            float f = (float)a->pos;
            const int16_t *p0 = g_sound.out + r * 2, *p1 = g_sound.out + ((r + 1) & mask) * 2;
            float l  = ((float)p0[0] + ((float)p1[0] - (float)p0[0]) * f) / 32768.0f;
            float rr = ((float)p0[1] + ((float)p1[1] - (float)p0[1]) * f) / 32768.0f;
            if (a->reseed) {
                /* The blocker as though it had always sat at this level: its
                 * output starts at zero, not at the step from the old level. */
                a->dc_xl = l; a->dc_xr = rr; a->dc_yl = 0.0f; a->dc_yr = 0.0f;
                a->reseed = false;
            }
            const float R = 0.9993f;
            yl = l  - a->dc_xl + R * a->dc_yl;
            yr = rr - a->dc_xr + R * a->dc_yr;
            a->dc_xl = l; a->dc_xr = rr; a->dc_yl = yl; a->dc_yr = yr;

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

        if (a->fading) {
            a->gain -= fade_step;
            if (a->gain <= 0.0f) {
                a->gain   = 0.0f;
                a->fading = false;
                if (a->skip_after_fade) {
                    /* Stale: at zero gain, drop the oldest audio down to the
                     * target and come back in on the new position. */
                    uint32_t r2 = g_sound.out_r, fill2 = (g_sound.out_w - r2) & mask;
                    if ((double)fill2 > a->target) {
                        g_sound.out_r = (r2 + (fill2 - (uint32_t)a->target)) & mask;
                        a->pos     = 0.0;
                        a->fill_lp = a->target;
                    }
                    a->reseed = true;
                    a->resyncs++;
                } else {
                    a->refilling = true;
                    a->underruns++;
                }
            }
        } else if (a->gain < 1.0f) {
            a->gain += fade_step;
            if (a->gain > 1.0f) a->gain = 1.0f;
        }
        {
            float g = a->gain * a->gain * (3.0f - 2.0f * a->gain);
            yl *= g; yr *= g;
        }
    out:
        buf[i * channels] = yl;
        if (channels > 1) buf[i * channels + 1] = yr;
    }
}

/* The fade out has to end on audio the ring already holds: AUDIO_FADE output
 * frames at the device's rate with the rate nudge reading at its fastest, and
 * the frames the interpolation keeps behind. */
static inline void audio_out_set_low_water(audio_out_t *a) {
    double src = (double)AUDIO_FADE * (double)SOUND_RATE / (double)a->rate * 1.01;
    a->low_water = (uint32_t)src + 4u;
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
    audio_out_set_low_water(a);
    /* Nothing has played yet: start as a refill, so the first sound waits for
     * a full cushion and comes in on a fade. */
    a->gain      = 0.0f;
    a->fading    = false;
    a->refilling = true;
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
    audio_out_set_low_water(a);
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
