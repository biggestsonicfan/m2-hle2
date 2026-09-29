/*
 * emu_times.h — where an emulated frame's host time goes.
 *
 * The emulation side of what frame_times.h is for the renderer: cumulative
 * host microseconds for the i960, the COP, the sound board and the emu
 * thread's waits, which get_status reports as "emu". Two readings some seconds
 * apart give each one's cost per frame, and pace_us against work_us says
 * whether a board short of 60 fps is slow or only throttled (issue #126).
 *
 * Always compiled in, so it has to stay a few clock reads a frame:
 *   - the i960 loop, the sound board and the waits are timed whole;
 *   - the COP is timed on one command in ~16.5 (a random stride, so it cannot
 *     fall into step with a game's fixed command sequences) and scaled by the
 *     exact command count. STF's attract sends ~1,400 commands a frame, and a
 *     clock read is ~25 ns, so timing each would cost ~6% of the emulation;
 *   - the SCSP (slots, DSP and mix) is timed on one sample in 16, the same way.
 *   That is ~260 clock reads a frame, ~0.6% of STF attract's 1 ms of work.
 *     The 68000 is the rest of the sound board.
 *   With the sound board on its own thread (sound.h, "The sound thread"),
 *   sound_us is that thread's time and mostly overlaps the i960's; work_us is
 *   the emu thread's alone, and sound_wait_us is where it waited on the other.
 *
 * Nothing here is board state: the board never reads it, so it cannot move
 * a netplay session or a grader. Written by the emu thread; the bridge reads
 * it without a lock, as it reads "render".
 */
#ifndef EMU_TIMES_H
#define EMU_TIMES_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
static inline int64_t emu_now_us(void) {
    static LARGE_INTEGER freq = {0};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (int64_t)(now.QuadPart * 1000000 / freq.QuadPart);
}
#else
#  include <time.h>
static inline int64_t emu_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
#endif

/* Frame-time histogram: a frame's work (the slices it took, waits left out)
 * falls in the first bucket whose bound it is under; the last is open. */
#define EMU_TIMES_BUCKETS 8
static const int64_t k_emu_times_bound_us[EMU_TIMES_BUCKETS - 1] = {
    1000, 2000, 4000, 8000, 16667, 33333, 66667
};

typedef struct {
    uint64_t frames;          /* game frames ended */
    uint64_t slices;          /* slices run (a load frame takes several) */
    uint64_t steps;           /* i960 instructions */
    int64_t  work_us;         /* slice bodies and their bookkeeping: all but the waits */
    int64_t  loop_us;         /* the i960 loop, less the sound board run or waited for inside it (COP included) */
    int64_t  sound_us;        /* the sound board, 68000 + SCSP, wherever it ran */
    int64_t  sound_inline_us; /* ...of which on the emu thread (the UART's run-ahead, or no sound thread) */
    int64_t  sound_wait_us;   /* the emu thread waiting for the sound thread to finish (sound_settle) */
    uint64_t sound_jobs;      /* slices of sound handed to the sound thread */
    uint64_t sound_samples;
    int64_t  scsp_timed_us;   /* the sampled SCSP samples */
    uint64_t scsp_timed;
    uint64_t cop_cmds;        /* COP commands executed */
    int64_t  cop_timed_us;    /* the sampled ones */
    uint64_t cop_timed;
    uint32_t cop_skip;        /* commands until the next timed one */
    uint32_t cop_rng;
    int64_t  pace_us;         /* asleep for the 60 Hz pacing */
    int64_t  net_us;          /* the netplay pump, and waiting on the peer */
    uint64_t hist[EMU_TIMES_BUCKETS];
    int64_t  frame_us;        /* the frame in progress, so far */
    int64_t  frame_max_us;    /* the longest frame since get_status last read it */
    volatile int max_reset;   /* get_status read the max: start it again */
} emu_times_t;

static emu_times_t g_emu_times = { .cop_skip = 1, .cop_rng = 0x2545F491u };

/* A COP command is about to run: its start time if this one is timed, else 0. */
static inline int64_t emu_times_cop_begin(void) {
    emu_times_t *t = &g_emu_times;
    t->cop_cmds++;
    if (--t->cop_skip) return 0;
    uint32_t x = t->cop_rng;                  /* xorshift32 */
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    t->cop_rng  = x;
    t->cop_skip = 1 + (x & 31);
    return emu_now_us();
}

static inline void emu_times_cop_end(int64_t t0) {
    if (!t0) return;
    g_emu_times.cop_timed_us += emu_now_us() - t0;
    g_emu_times.cop_timed++;
}

/* A slice's work is done; `frame` if the game's frame ended in it. */
static inline void emu_times_slice(int64_t work_us, bool frame) {
    emu_times_t *t = &g_emu_times;
    t->slices++;
    t->work_us  += work_us;
    t->frame_us += work_us;
    if (!frame) return;
    t->frames++;
    int b = 0;
    while (b < EMU_TIMES_BUCKETS - 1 && t->frame_us >= k_emu_times_bound_us[b]) b++;
    t->hist[b]++;
    if (t->max_reset) { t->max_reset = 0; t->frame_max_us = 0; }
    if (t->frame_us > t->frame_max_us) t->frame_max_us = t->frame_us;
    t->frame_us = 0;
}

/* The "emu" block of get_status. The sampled parts are scaled up to their
 * whole here; the i960 is the loop less the COP's estimate. Reading it starts
 * frame_max_us again. */
static inline void emu_times_json(char *out, int cap) {
    const emu_times_t *t = &g_emu_times;
    int64_t cop = t->cop_timed ? (int64_t)((double)t->cop_timed_us * (double)t->cop_cmds / (double)t->cop_timed) : 0;
    int64_t scsp = t->scsp_timed ? (int64_t)((double)t->scsp_timed_us * (double)t->sound_samples / (double)t->scsp_timed) : 0;
    if (cop > t->loop_us)   cop  = t->loop_us;
    if (scsp > t->sound_us) scsp = t->sound_us;
    int64_t max = t->frame_max_us;
    g_emu_times.max_reset = 1;
    snprintf(out, (size_t)cap,
             "{\"frames\":%llu,\"slices\":%llu,\"steps\":%llu,\"work_us\":%lld,"
             "\"i960_us\":%lld,\"cop_us\":%lld,\"cop_cmds\":%llu,"
             "\"sound_us\":%lld,\"m68k_us\":%lld,\"scsp_us\":%lld,\"sound_samples\":%llu,"
             "\"sound_inline_us\":%lld,\"sound_wait_us\":%lld,\"sound_jobs\":%llu,"
             "\"pace_us\":%lld,\"net_us\":%lld,\"frame_max_us\":%lld,"
             "\"hist_ms\":[1,2,4,8,16.7,33.3,66.7],"
             "\"hist\":[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu]}",
             (unsigned long long)t->frames, (unsigned long long)t->slices,
             (unsigned long long)t->steps, (long long)t->work_us,
             (long long)(t->loop_us - cop), (long long)cop, (unsigned long long)t->cop_cmds,
             (long long)t->sound_us, (long long)(t->sound_us - scsp), (long long)scsp,
             (unsigned long long)t->sound_samples,
             (long long)t->sound_inline_us, (long long)t->sound_wait_us, (unsigned long long)t->sound_jobs,
             (long long)t->pace_us, (long long)t->net_us, (long long)max,
             (unsigned long long)t->hist[0], (unsigned long long)t->hist[1],
             (unsigned long long)t->hist[2], (unsigned long long)t->hist[3],
             (unsigned long long)t->hist[4], (unsigned long long)t->hist[5],
             (unsigned long long)t->hist[6], (unsigned long long)t->hist[7]);
}

#endif /* EMU_TIMES_H */
