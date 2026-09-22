/*
 * pc_profile.h — where the BOARD's time goes: an i960 instruction count per
 * code address, plus the host microseconds and step count of every game frame
 * in the window.
 *
 * The host profilers answer "which emulator function costs"; this answers
 * "which ROM routine asked for it", which is the question a state-by-state
 * speed complaint ("the VS screen hitches") actually poses. Map the addresses
 * back to names with the IDA bridge (tools/prof-report.mjs).
 *
 * It is compiled out entirely unless M2HLE_PROFILE is defined (CMake option
 * M2HLE_PROFILE, default OFF): the tick sits in the i960 step loop, which is
 * half the emulator's time, and a branch there is not free. Build a third,
 * instrumented binary to take the measurement; optimise and A/B the normal one.
 */
#ifndef M2HLE_PC_PROFILE_H
#define M2HLE_PC_PROFILE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef M2HLE_PROFILE

/* Code lives in the first 4 MB (ROM at 0, work RAM well above it); one counter
 * per instruction word, 4 MB of counters. */
#define PCPROF_SPAN   0x00400000u
#define PCPROF_SLOTS  (PCPROF_SPAN / 4u)
#define PCPROF_FRAMES 65536             /* frames kept in the window */

typedef struct {
    uint32_t *counts;
    int       on;
    uint64_t  steps;                    /* i960 instructions while armed */
    uint32_t  nframes;
    int32_t   frame_us[PCPROF_FRAMES];  /* host microseconds of each slice */
    uint32_t  frame_steps[PCPROF_FRAMES];
    uint32_t  frame_no[PCPROF_FRAMES];  /* g_emu_frames at the frame's end */
    int64_t   sound_us;                 /* of the window's time: the 68000 + SCSP */
} pcprof_t;

static pcprof_t g_pcprof;
#define g_pcprof_on (g_pcprof.on)

static inline void pcprof_arm(int on) {
    if (on) {
        if (!g_pcprof.counts) g_pcprof.counts = (uint32_t *)calloc(PCPROF_SLOTS, 4);
        else memset(g_pcprof.counts, 0, (size_t)PCPROF_SLOTS * 4);
        g_pcprof.steps = 0;
        g_pcprof.nframes = 0;
        g_pcprof.sound_us = 0;
    }
    g_pcprof.on = on && g_pcprof.counts != NULL;
}

/* Called for every instruction while armed. Kept tiny and inlined. */
#define PCPROF_TICK(ip) \
    do { if (g_pcprof.on) { g_pcprof.steps++; \
         if ((uint32_t)(ip) < PCPROF_SPAN) g_pcprof.counts[(uint32_t)(ip) >> 2]++; } } while (0)

static inline void pcprof_frame(int32_t us, uint32_t steps, uint32_t frame_no) {
    if (!g_pcprof.on || g_pcprof.nframes >= PCPROF_FRAMES) return;
    uint32_t i = g_pcprof.nframes++;
    g_pcprof.frame_us[i]    = us;
    g_pcprof.frame_steps[i] = steps;
    g_pcprof.frame_no[i]    = frame_no;
}

/* Two CSVs: <path> is addr,count for every address that ran, <path>.frames.csv
 * is frame,us,steps. Returns the number of distinct addresses written. */
static inline int pcprof_write(const char *path) {
    if (!g_pcprof.counts) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "addr,count\n");
    int n = 0;
    for (uint32_t i = 0; i < PCPROF_SLOTS; i++)
        if (g_pcprof.counts[i]) { fprintf(f, "%u,%u\n", i * 4u, g_pcprof.counts[i]); n++; }
    fclose(f);

    char fp[1024];

    snprintf(fp, sizeof fp, "%s.frames.csv", path);
    if ((f = fopen(fp, "wb")) != NULL) {
        fprintf(f, "#sound_us,%lld\n", (long long)g_pcprof.sound_us);
        fprintf(f, "frame,us,steps\n");
        for (uint32_t i = 0; i < g_pcprof.nframes; i++)
            fprintf(f, "%u,%d,%u\n", g_pcprof.frame_no[i], g_pcprof.frame_us[i], g_pcprof.frame_steps[i]);
        fclose(f);
    }
    return n;
}

#else   /* the normal build: nothing of it survives */

#define PCPROF_TICK(ip)   ((void)0)
#define g_pcprof_on       0
static inline void pcprof_arm(int on) { (void)on; }
static inline void pcprof_frame(int32_t us, uint32_t steps, uint32_t frame_no) {
    (void)us; (void)steps; (void)frame_no;
}
static inline int pcprof_write(const char *path) { (void)path; return -1; }

#endif  /* M2HLE_PROFILE */
#endif  /* M2HLE_PC_PROFILE_H */
