/*
 * srallyc_native.h -- Sega Rally's own replay: each course's highlight clip,
 * kept in the game's format and played back by the game's own replay, the
 * championship ending (Pinboard #997). NATIVE-REPLAY.md, "Sega Rally", has
 * the format in full.
 *
 * The game records a clip of every course it races into a buffer of its own,
 * 0x9AB0 bytes at 0x556420 + course * 0x9AB0 (forest, desert, mountain,
 * lakeside), with the frames recorded in 0x217260[course]. Each record is the
 * 0x58-byte car block (0x213B40 is the player's) taken every 4 frames: records
 * 0..224 are the player's car, 225..449 the nearest rival's. 0x5E50E8 clears
 * the course's count at the race start; when the car passes one of the
 * course's three points (the table at 0x5E5010) the game picks the nearest
 * car and records 250 frames of both, so a course holds up to 750 frames.
 * 0x5E53C0 (course, frame, side) reads a clip back, the player's frames from
 * 0 and the rival's from 900, interpolating between records.
 *
 * The championship's ending (sub-state 5's states 5 and 6, 0x5E7CB0 and
 * 0x5E78B0, after the lakeside is won) is the game's replay: the podium, then
 * the staff credits over each course's clip, forest to lakeside, from
 * 0x5E7E80. A course never raced plays as an empty road, as on the board.
 *
 * Recording: at the championship's race end (0x5BAE40, sub-state 5 state 4,
 * which every race reaches, won or timed out) the course's buffer and count
 * are taken. A round is a race; "keyed" when the clip has frames.
 *
 * Playback (srallyc_native_play): rounds are grouped into championships,
 * each a run of rounds whose courses climb. At the main loop's mode dispatch
 * (0x5A2200), once attract is up, a championship's clips go back into their
 * buffers (the other courses' counts zeroed) and the game is put where the
 * lakeside's race end puts it: a player's game (mode 3), championship
 * (sub-state 5), state 5, course 3. When the ending reaches its name entry,
 * the next championship goes in after attract is back, or the play ends; the
 * board is sent back to its boot (mode 0) either way, and no name is asked
 * for. g_native_play counts the frames the credits play a recorded course.
 *
 * The recording hook only reads, and keeps what it read outside the board.
 */
#ifndef M2HLE_SRALLYC_NATIVE_H
#define M2HLE_SRALLYC_NATIVE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "game_profile.h"
#include "hle_hooks.h"
#include "i960.h"
#include "log.h"
#include "memory.h"

#define SRN_MODE       0x00202098u   /* 2 attract, 3 a player's game */
#define SRN_SUB        0x0020209Cu   /* 5 the championship */
#define SRN_STATE      0x002020ACu   /* the sub-state's state */
#define SRN_PASSED     0x002020B0u   /* the race was won */
#define SRN_COURSE     0x00214354u   /* 0 forest .. 3 lakeside */
#define SRN_CAR        0x0020A8C4u
#define SRN_CLIP       0x00556420u
#define SRN_CLIP_LEN   0x9AB0u
#define SRN_COUNT      0x00217260u
#define SRN_END_STATE  0x00218C50u   /* the ending's own state; 3 the credits */
#define SRN_COURSES    4
#define SRN_CLIP_MAX   750u          /* three clips of 250 frames */
/* The credits play each course for 679 frames (measured: 847, 1526, 2205,
 * 2884 and 3563 from a cold-boot playback). */
#define SRN_REEL_FRAMES 679u

#define SRN_RACE_END   0x005BAE40u
#define SRN_DISPATCH   0x005A2200u

static inline uint32_t srn_clip_addr(uint32_t course) { return SRN_CLIP + course * SRN_CLIP_LEN; }

/* ---- Recording ----------------------------------------------------------- */

static struct {
    bool     pending;
    uint32_t course, count, passed, car;
    uint8_t  clip[SRN_CLIP_LEN];
    uint8_t  count_le[4];
} s_srn;

/* 0x5BAE40, the championship's race end: the course's clip as it stands. */
static int srallyc_hook_native_race_end(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    if (g_native_play.on) return 1;
    uint32_t course = mem_read32(bus, SRN_COURSE);
    if (course >= SRN_COURSES) return 1;
    s_srn.course = course;
    s_srn.count  = mem_read32(bus, SRN_COUNT + 4u * course);
    s_srn.passed = mem_read32(bus, SRN_PASSED);
    s_srn.car    = mem_read8(bus, SRN_CAR);
    for (uint32_t i = 0; i < SRN_CLIP_LEN; i++) s_srn.clip[i] = mem_read8(bus, srn_clip_addr(course) + i);
    for (int i = 0; i < 4; i++) s_srn.count_le[i] = (uint8_t)(s_srn.count >> (8 * i));
    s_srn.pending = true;
    LOG_INFO("native replay: course %u %s, a clip of %u frames", course,
             s_srn.passed ? "won" : "not won", s_srn.count);
    return 1;
}

static bool srallyc_native_take(game_native_round_t *out) {
    if (!s_srn.pending) return false;
    s_srn.pending = false;
    bool keyed = s_srn.count > 0 && s_srn.count <= SRN_CLIP_MAX;
    out->nparts = 2;
    out->part[0] = (game_native_part_t){ "clip.bin", srn_clip_addr(s_srn.course), s_srn.clip, SRN_CLIP_LEN };
    out->part[1] = (game_native_part_t){ "count.bin", SRN_COUNT + 4u * s_srn.course, s_srn.count_le, 4 };
    snprintf(out->json, sizeof out->json,
             "{\"game\":\"srallyc\",\"course\":%u,\"passed\":%d,\"car\":%u,\"clip_frames\":%u,"
             "\"keyed\":%d,\"key_frames\":%u}",
             s_srn.course, s_srn.passed ? 1 : 0, s_srn.car, s_srn.count, keyed ? 1 : 0,
             keyed ? SRN_REEL_FRAMES + 1 : 0);
    return true;
}

/* ---- The way back in ----------------------------------------------------- */

static inline const game_native_part_t *srn_find(const game_native_round_t *in, const char *name,
                                                 uint32_t len) {
    for (int i = 0; i < in->nparts; i++)
        if (!strcmp(in->part[i].name, name) && in->part[i].len == len) return &in->part[i];
    return NULL;
}

/* The round's course, from where its clip goes; -1 when it has none. */
static inline int srn_course(const game_native_round_t *in) {
    const game_native_part_t *c = srn_find(in, "clip.bin", SRN_CLIP_LEN);
    if (!c || c->addr < SRN_CLIP || (c->addr - SRN_CLIP) % SRN_CLIP_LEN) return -1;
    uint32_t course = (c->addr - SRN_CLIP) / SRN_CLIP_LEN;
    return course < SRN_COURSES ? (int)course : -1;
}

static inline void srn_write(memory_bus_t *bus, uint32_t addr, const uint8_t *in, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) mem_write8(bus, addr + i, in[i]);
}

/* The clip and its count back in the course's buffer. */
static const char *srallyc_native_put(memory_bus_t *bus, const game_native_round_t *in) {
    int course = srn_course(in);
    const game_native_part_t *n = srn_find(in, "count.bin", 4);
    if (course < 0 || !n || n->addr != SRN_COUNT + 4u * (uint32_t)course) return "the round has no clip";
    srn_write(bus, srn_clip_addr((uint32_t)course), srn_find(in, "clip.bin", SRN_CLIP_LEN)->data, SRN_CLIP_LEN);
    srn_write(bus, n->addr, n->data, 4);
    return NULL;
}

/* ---- Playback ------------------------------------------------------------ */

static struct {
    const game_native_round_t *rounds;
    int  n;
    int  next;        /* the first round of the next championship */
    int  last;        /* one past the playing championship's last round */
    bool in_ending;
    int  round_of[SRN_COURSES];    /* each course's round in it; -1 none */
    int  reel_course;              /* the course the credits are on */
} s_srp;

/* One past the last round of the championship that starts at `first`: the
 * rounds while their courses climb. */
static inline int srn_pass_end(const game_native_round_t *rounds, int n, int first) {
    int k = first + 1;
    while (k < n && srn_course(&rounds[k]) > srn_course(&rounds[k - 1])) k++;
    return k;
}

static const char *srallyc_native_play(const game_native_round_t *rounds, int n) {
    for (int i = 0; i < n; i++) {
        const game_native_part_t *c = srn_find(&rounds[i], "count.bin", 4);
        if (srn_course(&rounds[i]) < 0 || !c) return "a round cannot be played: it has no clip";
    }
    memset(&s_srp, 0, sizeof s_srp);
    memset(&g_native_play, 0, sizeof g_native_play);
    if (n <= 0) return NULL;
    s_srp.rounds = rounds;
    s_srp.n      = n;
    g_native_play.on           = true;
    g_native_play.rounds       = n;
    g_native_play.total_frames = (uint32_t)n * SRN_REEL_FRAMES;
    return NULL;
}

/* Attract is up: the next championship's clips in, and the game sent to the
 * lakeside's race end, won. */
static inline void srn_begin_ending(memory_bus_t *bus) {
    int first = s_srp.next;
    s_srp.last = srn_pass_end(s_srp.rounds, s_srp.n, first);
    s_srp.reel_course = -1;
    for (uint32_t c = 0; c < SRN_COURSES; c++) {
        s_srp.round_of[c] = -1;
        mem_write32(bus, SRN_COUNT + 4u * c, 0);
    }
    for (int k = first; k < s_srp.last; k++) {
        srallyc_native_put(bus, &s_srp.rounds[k]);
        s_srp.round_of[srn_course(&s_srp.rounds[k])] = k;
    }
    mem_write32(bus, SRN_COURSE, 3);
    mem_write32(bus, SRN_STATE, 5);
    mem_write32(bus, SRN_SUB, 5);
    mem_write32(bus, SRN_MODE, 3);
    s_srp.in_ending = true;
    g_native_play.round = first;
    LOG_INFO("native replay: the ending plays rounds %d-%d", first + 1, s_srp.last);
}

/* A frame of the ending: count the credits over a recorded course. False
 * once the ending is over. */
static inline bool srn_ending_frame(memory_bus_t *bus) {
    uint32_t state = mem_read32(bus, SRN_STATE);
    if (mem_read32(bus, SRN_MODE) != 3 || mem_read32(bus, SRN_SUB) != 5 || (state != 5 && state != 6))
        return false;
    uint32_t course = mem_read32(bus, SRN_COURSE);
    if (mem_read32(bus, SRN_END_STATE) != 3 || course >= SRN_COURSES || s_srp.round_of[course] < 0)
        return true;
    if ((int)course != s_srp.reel_course) {
        s_srp.reel_course    = (int)course;
        g_native_play.round  = s_srp.round_of[course];
        g_native_play.frame  = 0;
        g_native_play.frames = SRN_REEL_FRAMES;
    }
    if (g_native_play.frame < SRN_REEL_FRAMES) {
        g_native_play.frame++;
        g_native_play.total++;
    }
    return true;
}

/* 0x5A2200, the main loop's mode dispatch, once a frame. */
static int srallyc_hook_native_dispatch(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    if (!g_native_play.on) return 1;
    if (!s_srp.in_ending) {
        if (mem_read32(bus, SRN_MODE) == 2) srn_begin_ending(bus);
        return 1;
    }
    if (srn_ending_frame(bus)) return 1;
    /* The name entry: back to the boot, and attract, with no name asked. */
    s_srp.in_ending = false;
    s_srp.next      = s_srp.last;
    mem_write32(bus, SRN_MODE, 0);
    mem_write32(bus, SRN_SUB, 0);
    if (s_srp.next >= s_srp.n) {
        g_native_play.on    = false;
        g_native_play.ended = true;
        LOG_INFO("native replay: the ending is over");
    }
    return 1;
}

#define SRALLYC_NATIVE_HOOK_COUNT 2
#define SRALLYC_NATIVE_HOOKS                                                     \
    { SRN_RACE_END, srallyc_hook_native_race_end, "race end (native replay)" }, \
    { SRN_DISPATCH, srallyc_hook_native_dispatch, "mode dispatch (native replay)" },

#endif /* M2HLE_SRALLYC_NATIVE_H */
