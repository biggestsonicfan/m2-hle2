/*
 * audio_out_test.c -- the host drain (core/audio_out.h) through the stalls a
 * netplay session puts it through. Pure: no ROM, no audio device. A fake board
 * pushes 735 frames of a 440 Hz tone on its DC offset every 1/60 s of simulated
 * time, a fake device pulls 2048-frame callbacks at 48 kHz, and the output is
 * checked for clicks. A click is a corner, not only a step: freezing the wave
 * where it stands (what an underrun used to do) is a change of slope, heard
 * just as clearly. So the check is the second difference, which for the tone
 * itself -- corners of the linear interpolation included -- stays near 0.002
 * of full scale, and at a frozen or cut wave jumps to the tone's slope, 0.017.
 * The first difference (a step) is checked too.
 *
 *  (A) A steady board: no underrun, no resync, no click.
 *  (B) One stall longer than the cushion: one refill, faded, no click.
 *  (C) Stalls shorter than the cushion, every 1.5 s: before, each one ate into
 *      a ring that the 1% nudge could not pay back, and once it ran dry every
 *      callback underran again -- a burst of clicks. Now at most one refill a
 *      stall, and no click.
 *  (D) A board that comes back while the fade out is under way: no click.
 *  (E) A ring pushed far past its target (a suspended device): one resync, no
 *      click.
 */
#define NDEBUG 1
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio_out.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
    else         { printf("ok:   "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define DEV_RATE   48000u
#define CB_FRAMES  2048
#define TONE_AMP   0.3
#define TONE_HZ    440.0
#define DC_OFFSET  5000

/* The tone's steepest step at the device rate, with room for the fade's own
 * slope (512 frames of smoothstep: at most 1.5/512 of the level a frame). */
static const double k_max_step = TONE_AMP * 2.0 * 3.14159265358979 * TONE_HZ / DEV_RATE * 1.2
                               + TONE_AMP * 1.5 / 512.0;
/* Its largest second difference is about 0.002; a corner is 0.017. */
static const double k_max_bend = 0.005;

typedef struct {
    double   max_step, max_bend;
    uint64_t clicks;
    float    prev, prev2;
    int      have;              /* output frames seen, up to 2 */
    uint64_t board_n;       /* frames the fake board has made */
} run_t;

static void board_push(run_t *r, uint32_t n) {
    for (uint32_t i = 0; i < n; i++, r->board_n++) {
        double t = (double)r->board_n / 44100.0;
        int16_t l = (int16_t)(DC_OFFSET + TONE_AMP * 32767.0 * sin(2.0 * 3.14159265358979 * TONE_HZ * t));
        sound_out_push(l, l);
    }
}

static void device_pull(run_t *r) {
    float buf[CB_FRAMES * 2];
    audio_out_cb(buf, CB_FRAMES, 2, NULL);
    for (int i = 0; i < CB_FRAMES; i++) {
        float y = buf[i * 2];
        bool click = false;
        if (r->have >= 1) {
            double d = fabs((double)y - (double)r->prev);
            if (d > r->max_step) r->max_step = d;
            click |= d > k_max_step;
        }
        if (r->have >= 2) {
            double b = fabs((double)y - 2.0 * (double)r->prev + (double)r->prev2);
            if (b > r->max_bend) r->max_bend = b;
            click |= b > k_max_bend;
        }
        r->clicks += click;
        r->prev2 = r->prev; r->prev = y;
        if (r->have < 2) r->have++;
    }
}

/* stalled(ms) says whether the board is stopped at that moment. */
typedef bool (*stall_fn)(double ms);

static void run(run_t *r, double seconds, stall_fn stalled) {
    memset(r, 0, sizeof *r);
    memset(&g_sound, 0, sizeof g_sound);
    memset(&g_audio_out, 0, sizeof g_audio_out);
    audio_out_configure(NULL, DEV_RATE);
    /* Walk simulated time in device frames; the board makes a frame every
     * 800 of them (1/60 s), the device pulls a callback every 2048. */
    uint64_t end = (uint64_t)(seconds * DEV_RATE);
    for (uint64_t t = 0; t < end; t++) {
        if (t % 800 == 0 && !(stalled && stalled((double)t * 1000.0 / DEV_RATE))) board_push(r, 735);
        if (t % CB_FRAMES == 0) device_pull(r);
    }
}

static bool stall_once_400(double ms)   { return ms >= 3000.0 && ms < 3400.0; }
static bool stall_every_120(double ms)  { return ms >= 3000.0 && fmod(ms - 3000.0, 1500.0) < 120.0; }
static bool stall_just_short(double ms) { return ms >= 3000.0 && ms < 3000.0 + 8192.0 * 1000.0 / 44100.0 - 2.0; }

int main(void) {
    run_t r;
    printf("click threshold: %.4f of full scale a frame\n", k_max_step);

    run(&r, 10.0, NULL);
    CHECK(r.clicks == 0 && g_audio_out.underruns == 0 && g_audio_out.resyncs == 0,
          "(A) steady: %llu clicks (max step %.4f, bend %.4f), %llu underruns, %llu resyncs",
          (unsigned long long)r.clicks, r.max_step, r.max_bend,
          (unsigned long long)g_audio_out.underruns, (unsigned long long)g_audio_out.resyncs);

    run(&r, 8.0, stall_once_400);
    CHECK(r.clicks == 0 && g_audio_out.underruns == 1,
          "(B) one 400 ms stall: %llu clicks (max step %.4f, bend %.4f), %llu underruns",
          (unsigned long long)r.clicks, r.max_step, r.max_bend, (unsigned long long)g_audio_out.underruns);

    run(&r, 15.0, stall_every_120);
    CHECK(r.clicks == 0 && g_audio_out.underruns <= 8,
          "(C) 120 ms stalls every 1.5 s: %llu clicks (max step %.4f, bend %.4f), %llu underruns",
          (unsigned long long)r.clicks, r.max_step, r.max_bend, (unsigned long long)g_audio_out.underruns);

    run(&r, 8.0, stall_just_short);
    CHECK(r.clicks == 0 && g_audio_out.underruns <= 1,
          "(D) a stall ending as the ring runs out: %llu clicks (max step %.4f, bend %.4f), %llu underruns",
          (unsigned long long)r.clicks, r.max_step, r.max_bend, (unsigned long long)g_audio_out.underruns);

    /* (E): let it settle, then dump most of a ring in at once. */
    {
        run(&r, 3.0, NULL);
        board_push(&r, g_audio_out.resync_above + 200 - audio_out_queued());
        for (int i = 0; i < 100; i++) {
            device_pull(&r);            /* first: a full ring drops what is pushed */
            board_push(&r, CB_FRAMES * 44100 / DEV_RATE);
        }
        CHECK(r.clicks == 0 && g_audio_out.resyncs == 1 && g_sound.out_dropped == 0,
              "(E) stale ring: %llu clicks (max step %.4f, bend %.4f), %llu resyncs",
              (unsigned long long)r.clicks, r.max_step, r.max_bend, (unsigned long long)g_audio_out.resyncs);
    }

    printf(g_fail ? "\n%d FAILED\n" : "\nall passed\n", g_fail);
    return g_fail ? 1 : 0;
}
