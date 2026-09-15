/*
 * audio_out.h — play the sound board's output on the host.
 *
 * The emu thread fills g_sound's ring at 44.1 kHz in slice-sized bursts
 * (board/sound.h); sokol_audio's callback drains it at the device's rate. The
 * two clocks never quite agree, so the callback resamples with a ratio nudged
 * by how full the ring is: it plays a touch faster when the ring runs long and
 * slower when it runs short, holding about AUDIO_TARGET frames of latency
 * without clicks. An empty ring (emulator stopped or behind) fades to silence.
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

#define AUDIO_TARGET 4096.0     /* frames of 44.1 kHz audio kept queued (~93 ms) */

typedef struct {
    bool     ready;
    double   pos;               /* fractional read position ahead of out_r */
    float    last_l, last_r;
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
        if (fill < 2) {
            /* hold the last level (the DC blocker then fades it out without a click) */
            l = a->last_l; rr = a->last_r;
            a->underruns++;
        } else {
            float f = (float)a->pos;
            const int16_t *p0 = g_sound.out + r * 2, *p1 = g_sound.out + ((r + 1) & mask) * 2;
            l  = ((float)p0[0] + ((float)p1[0] - (float)p0[0]) * f) / 32768.0f;
            rr = ((float)p0[1] + ((float)p1[1] - (float)p0[1]) * f) / 32768.0f;
            a->last_l = l; a->last_r = rr;
            double adj = ((double)fill - AUDIO_TARGET) / AUDIO_TARGET;
            adj = adj < -1.0 ? -1.0 : adj > 1.0 ? 1.0 : adj;
            a->pos += (double)SOUND_RATE / (double)a->rate * (1.0 + 0.005 * adj);
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

static inline void audio_out_init(void) {
    saudio_setup(&(saudio_desc){
        .num_channels       = 2,
        .stream_userdata_cb = audio_out_cb,
        .logger.func        = slog_func,
    });
    if (!saudio_isvalid()) {
        LOG_WARN("audio: no output device — sound disabled");
        return;
    }
    g_audio_out.rate  = (uint32_t)saudio_sample_rate();
    g_audio_out.ready = true;
    LOG_INFO("audio: %u Hz output, resampled from the board's 44100 Hz", g_audio_out.rate);
}

static inline void audio_out_shutdown(void) {
    if (g_audio_out.ready) saudio_shutdown();
    g_audio_out.ready = false;
}

#endif /* AUDIO_OUT_H */
