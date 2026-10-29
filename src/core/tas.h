/*
 * tas.h -- tool-assisted play on the desktop (Pinboard #1462).
 *
 * A movie is a start (the board's state when it began, a savestate zip held in
 * memory) and one input per board frame after it: the held word the board
 * reads, and the light guns' aim for a gun game (lightgun.h's GUN payload).
 * The board is the same board a replay or a follower runs, fed the same way
 * (g_input.net_held, lightgun_feed_put), so the movie plays back exactly.
 *
 * Frame `f` of the movie is the f-th board frame after the start; its input
 * is what the board reads while it runs that frame. The cursor (`frame`) is
 * the frame the board is about to run: frames 0 .. frame-1 are behind it.
 *
 *   - Recording (the default) writes what the virtual pad holds (and the
 *     host's own keys, with keys_too) into the frame about to run. At the end
 *     of the movie that adds a frame; inside it, it writes over the frame,
 *     and with `cut` a frame that differs from the old one drops the old rest
 *     of the movie (it is still in Undo and in any branch).
 *   - Playing (recording off) reads the movie, as a seek always does.
 *
 * Going back is a seek: the greenzone keeps the board as it was at some
 * frames (rollback.h's in-process copy, a few MB each, within `budget_mb`),
 * and a seek loads the nearest one at or before the frame and runs the movie
 * from there, unpaced and unheard. A seek always runs at least one frame, so
 * the picture is the target's own. Every frame also keeps the frame check
 * (netplay_frame_check) it ended on the first time it ran, and a frame that
 * ends on another one with the same inputs is a desync (the emulator's bug,
 * never the player's): `desync` names the first.
 *
 * Undo, three ways:
 *   - Undo rewind goes back to where the last run of rewinds began (a stack).
 *   - Undo / redo inputs put back the movie as it was before an edit, a take
 *     that wrote over it, or a branch load.
 *   - Branches keep whole movies (a "best" so far) beside the one being
 *     played; loading one puts its inputs back and seeks to where it was saved.
 *
 * Threads: everything here is under the emu mutex. The slice's hooks run in
 * the slice body; tas_pump runs between slices on the emu thread (seeks and
 * the greenzone); the window calls the rest with the mutex taken.
 *
 * Desktop only: every hook tests tas_live(), a constant false in a build
 * without M2HLE_DEV_TOOLS (build_features.h), whose frontends have no window
 * to arm it. A board nobody drives this way runs as before.
 *
 * Header-only. Included from emu_thread.h after replay.h.
 */
#ifndef M2HLE_TAS_H
#define M2HLE_TAS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "build_features.h"
#include "json_min.h"
#include "../board/lightgun.h"

#define TAS_FORMAT        1
#define TAS_BRANCHES      9
#define TAS_UNDO_MAX      64
#define TAS_UNDO_BYTES    (64u << 20)    /* the undo stack's inputs, all told */
#define TAS_BACK_MAX      64             /* undo-rewind positions */
#define TAS_GZ_EVERY_DEF  30u            /* a greenzone state every this many frames */
#define TAS_GZ_BUDGET_DEF 512u           /* MB of greenzone */
#define TAS_INPUT_BYTES   (4 + LIGHTGUN_FEED_BYTES)   /* a frame in a file */

typedef struct {
    uint32_t held;                       /* the board's held word (0x500700 layout) */
    uint8_t  gun[LIGHTGUN_FEED_BYTES];   /* lightgun_feed_put's payload; all 0: the pad's aim */
} tas_input_t;

/* A movie's inputs, and the check each frame ended on (0: not run yet). */
typedef struct {
    tas_input_t *in;
    uint32_t    *check;
    uint32_t     n, cap;
} tas_track_t;

typedef struct {
    tas_track_t track;
    uint32_t    frame;          /* where the board was when it was taken */
    bool        used;
    char        label[48];
    char        when[24];
} tas_branch_t;

typedef struct {
    tas_track_t track;
    uint32_t    frame;
} tas_undo_t;

typedef struct { uint32_t frame; void *buf; } tas_gz_t;

/* The virtual pad: what recording writes into the next frame. */
typedef struct {
    uint32_t hold;              /* held until untoggled */
    uint32_t once;              /* held for the next frame only */
    bool     aim_on[LIGHTGUN_PLAYERS];   /* the pad sets the gun's aim (else the host's mouse) */
    bool     aim_off[LIGHTGUN_PLAYERS];  /* ...pointing off the picture (a reload) */
    float    aim_x[LIGHTGUN_PLAYERS], aim_y[LIGHTGUN_PLAYERS];
} tas_pad_t;

typedef struct {
    bool        on;             /* the board runs on the movie */
    tas_track_t mv;
    uint32_t    frame;          /* the frame the board is about to run */
    bool        mid;            /* the last slice stopped short of a vblank */
    bool        stale;          /* the board ran frames whose inputs changed since */
    uint32_t    rerecords;
    /* The start. */
    uint8_t    *anchor;         /* savestate zip */
    size_t      anchor_len;
    int32_t     settings[FOLLOW_SET_WORDS];
    int         warning_skip;
    char        profile[32];
    /* Options. */
    bool        recording;
    bool        keys_too;       /* the host's keys and pads count with the virtual pad */
    bool        cut;            /* a take that differs drops the rest of the movie */
    int         slowdown;       /* 1, 2, 4, 8: frames are this many times as long */
    uint32_t    gz_every;
    uint32_t    budget_mb;
    tas_pad_t   pad;
    /* The greenzone. */
    tas_gz_t   *gz;
    uint32_t    gz_n, gz_cap;
    uint32_t    gz_every_now;   /* gz_every, doubled while the budget is short */
    size_t      state_size;
    /* A seek, asked for (seek_req >= 0) and under way. */
    int64_t     seek_req;
    bool        seek_resume;    /* run on, paced, once it lands */
    bool        seeking;
    uint32_t    seek_to;
    bool        resume_after;
    /* A start asked for by a thread that cannot take the board (the window,
     * after a reset it asked for at the same time). */
    bool        start_req;
    /* Undo. */
    uint32_t    back[TAS_BACK_MAX];
    int         back_n;
    bool        rewinding;      /* a run of rewinds with no frame played since */
    bool        take_saved;     /* this take's undo point is pushed */
    tas_undo_t  undo[TAS_UNDO_MAX], redo[TAS_UNDO_MAX];
    int         undo_n, redo_n;
    tas_branch_t branch[TAS_BRANCHES];
    /* The slice's latch. */
    bool        latched;
    int         use_net_was;
    /* What happened. */
    int64_t     desync;         /* the first frame that ended on another check, or -1 */
    char        msg[200];
} tas_t;

static tas_t g_tas = {
    .recording = true, .keys_too = true, .cut = true, .slowdown = 1,
    .gz_every = TAS_GZ_EVERY_DEF, .budget_mb = TAS_GZ_BUDGET_DEF,
    .seek_req = -1, .desync = -1,
};

static inline bool tas_live(void) { return M2HLE_DEV_TOOLS && g_tas.on; }

/* ---- Tracks ---------------------------------------------------------------- */

static inline bool tas__track_reserve(tas_track_t *t, uint32_t n) {
    if (n <= t->cap) return true;
    uint32_t cap = t->cap ? t->cap : 4096;
    while (cap < n) cap *= 2;
    tas_input_t *in = (tas_input_t *)realloc(t->in, (size_t)cap * sizeof *in);
    if (!in) return false;
    t->in = in;
    uint32_t *ck = (uint32_t *)realloc(t->check, (size_t)cap * sizeof *ck);
    if (!ck) return false;
    t->check = ck;
    t->cap = cap;
    return true;
}

static inline void tas__track_free(tas_track_t *t) {
    free(t->in);
    free(t->check);
    memset(t, 0, sizeof *t);
}

static inline bool tas__track_copy(tas_track_t *dst, const tas_track_t *src) {
    dst->n = 0;
    if (!tas__track_reserve(dst, src->n ? src->n : 1)) return false;
    if (src->n) {
        memcpy(dst->in, src->in, (size_t)src->n * sizeof *src->in);
        memcpy(dst->check, src->check, (size_t)src->n * sizeof *src->check);
    }
    dst->n = src->n;
    return true;
}

static inline bool tas__input_eq(const tas_input_t *a, const tas_input_t *b) {
    return a->held == b->held && !memcmp(a->gun, b->gun, sizeof a->gun);
}

/* The first frame where two tracks differ (their shorter length if one is the
 * other's start; the longer one's if they are the same). */
static inline uint32_t tas__first_diff(const tas_track_t *a, const tas_track_t *b) {
    uint32_t n = a->n < b->n ? a->n : b->n;
    for (uint32_t f = 0; f < n; f++)
        if (!tas__input_eq(&a->in[f], &b->in[f])) return f;
    return n;
}

/* ---- The greenzone ---------------------------------------------------------- */

static inline void tas__gz_clear(void) {
    for (uint32_t i = 0; i < g_tas.gz_n; i++) free(g_tas.gz[i].buf);
    g_tas.gz_n = 0;
}

/* The index of the newest state at or before `frame` (frame 0's is always kept). */
static inline int tas__gz_at_or_before(uint32_t frame) {
    int best = -1;
    for (uint32_t i = 0; i < g_tas.gz_n; i++) {
        if (g_tas.gz[i].frame > frame) break;
        best = (int)i;
    }
    return best;
}

static inline bool tas__gz_has(uint32_t frame) {
    int i = tas__gz_at_or_before(frame);
    return i >= 0 && g_tas.gz[i].frame == frame;
}

static inline void tas__gz_drop(uint32_t i) {
    free(g_tas.gz[i].buf);
    memmove(&g_tas.gz[i], &g_tas.gz[i + 1], (g_tas.gz_n - i - 1) * sizeof g_tas.gz[0]);
    g_tas.gz_n--;
}

/* The states after `frame`: what they hold was run on inputs that changed. */
static inline void tas__gz_after(uint32_t frame) {
    while (g_tas.gz_n && g_tas.gz[g_tas.gz_n - 1].frame > frame) tas__gz_drop(g_tas.gz_n - 1);
}

static inline size_t tas_gz_bytes(void) { return (size_t)g_tas.gz_n * g_tas.state_size; }

/* Keep within the budget: first the states off the grid (frame advance's
 * single frames), farthest from the cursor first; then the grid thinned by
 * doubling its step. Frame 0 always stays. */
static inline void tas__gz_trim(void) {
    size_t budget = (size_t)g_tas.budget_mb << 20;
    if (budget < 4 * g_tas.state_size) budget = 4 * g_tas.state_size;
    while (tas_gz_bytes() > budget && g_tas.gz_n > 1) {
        int pick = -1;
        uint32_t pick_d = 0;
        for (uint32_t i = 1; i < g_tas.gz_n; i++) {
            uint32_t f = g_tas.gz[i].frame;
            if (f % g_tas.gz_every_now == 0) continue;
            uint32_t d = f > g_tas.frame ? f - g_tas.frame : g_tas.frame - f;
            if (pick < 0 || d > pick_d) { pick = (int)i; pick_d = d; }
        }
        if (pick >= 0) { tas__gz_drop((uint32_t)pick); continue; }
        if (g_tas.gz_every_now > (1u << 20)) break;
        g_tas.gz_every_now *= 2;
    }
}

static inline uint32_t tas__gz_every(void) {
    if (g_tas.gz_every_now < g_tas.gz_every) g_tas.gz_every_now = g_tas.gz_every;
    return g_tas.gz_every_now;
}

/* The board as it is now, at the cursor. Between slices, mutex held. */
static inline void tas__gz_save(emu_thread_ctx_t *ctx) {
    if (g_tas.mid || tas__gz_has(g_tas.frame)) return;
    void *buf = malloc(g_tas.state_size);
    if (!buf) { snprintf(g_tas.msg, sizeof g_tas.msg, "out of memory for the greenzone"); return; }
    savestate_emu_t e = emu_state_latches(ctx);
    rollback_save_to(buf, &e);
    if (g_tas.gz_n == g_tas.gz_cap) {
        uint32_t cap = g_tas.gz_cap ? g_tas.gz_cap * 2 : 64;
        tas_gz_t *gz = (tas_gz_t *)realloc(g_tas.gz, cap * sizeof *gz);
        if (!gz) { free(buf); return; }
        g_tas.gz = gz;
        g_tas.gz_cap = cap;
    }
    uint32_t at = g_tas.gz_n;
    while (at > 0 && g_tas.gz[at - 1].frame > g_tas.frame) at--;
    memmove(&g_tas.gz[at + 1], &g_tas.gz[at], (g_tas.gz_n - at) * sizeof g_tas.gz[0]);
    g_tas.gz[at].frame = g_tas.frame;
    g_tas.gz[at].buf = buf;
    g_tas.gz_n++;
    tas__gz_trim();
}

static inline void tas__gz_load(emu_thread_ctx_t *ctx, int i) {
    savestate_emu_t e;
    rollback_load(g_tas.gz[i].buf, ctx->bus, &e);   /* settles the sound thread first */
    emu_state_put_latches(ctx, &e);
    emu_attn_bump();
    ctx->cpu_prev_snapshot = *ctx->cpu;
    ctx->cpu_snapshot      = *ctx->cpu;
    g_tas.frame = g_tas.gz[i].frame;
    g_tas.mid = false;
    g_tas.stale = false;
    follow_lead_break("TAS seek");
    replay_rec_break("TAS seek");
}

/* Frames from `f` on were run on other inputs: their states and checks go. */
static inline void tas__dirty(uint32_t f) {
    if (f < g_tas.frame || (f == g_tas.frame && g_tas.mid)) g_tas.stale = true;
    tas__gz_after(f);
    for (uint32_t i = f; i < g_tas.mv.n; i++) g_tas.mv.check[i] = 0;
    if (g_tas.desync >= (int64_t)f) g_tas.desync = -1;
}

/* ---- Undo -------------------------------------------------------------------- */

static inline size_t tas__undo_bytes(void) {
    size_t b = 0;
    for (int i = 0; i < g_tas.undo_n; i++) b += (size_t)g_tas.undo[i].track.n * TAS_INPUT_BYTES;
    for (int i = 0; i < g_tas.redo_n; i++) b += (size_t)g_tas.redo[i].track.n * TAS_INPUT_BYTES;
    return b;
}

static inline void tas__stack_drop_oldest(tas_undo_t *s, int *n) {
    tas__track_free(&s[0].track);
    memmove(&s[0], &s[1], (size_t)(*n - 1) * sizeof s[0]);
    (*n)--;
    memset(&s[*n], 0, sizeof s[0]);
}

static inline void tas__stack_clear(tas_undo_t *s, int *n) {
    for (int i = 0; i < *n; i++) tas__track_free(&s[i].track);
    *n = 0;
}

static inline void tas__stack_push(tas_undo_t *s, int *n, const tas_track_t *t, uint32_t frame) {
    if (*n == TAS_UNDO_MAX) tas__stack_drop_oldest(s, n);
    memset(&s[*n], 0, sizeof s[0]);
    if (!tas__track_copy(&s[*n].track, t)) { tas__track_free(&s[*n].track); return; }
    s[*n].frame = frame;
    (*n)++;
}

/* The movie as it is, before it changes. */
static inline void tas__undo_push(void) {
    tas__stack_push(g_tas.undo, &g_tas.undo_n, &g_tas.mv, g_tas.frame);
    tas__stack_clear(g_tas.redo, &g_tas.redo_n);
    while (tas__undo_bytes() > TAS_UNDO_BYTES && g_tas.undo_n > 1)
        tas__stack_drop_oldest(g_tas.undo, &g_tas.undo_n);
}

/* ---- Seeking ------------------------------------------------------------------ */

static inline void tas_seek(uint32_t frame, bool resume) {
    g_tas.seek_req = frame;
    g_tas.seek_resume = resume;
}

/* Back `k` frames. A run of rewinds is one step for Undo rewind. */
static inline void tas_rewind(uint32_t k, bool resume) {
    if (!g_tas.on) return;
    uint32_t from = g_tas.seek_req >= 0 ? (uint32_t)g_tas.seek_req
                  : g_tas.seeking ? g_tas.seek_to : g_tas.frame;
    if (!g_tas.rewinding) {
        if (g_tas.back_n == TAS_BACK_MAX) {
            memmove(g_tas.back, g_tas.back + 1, (TAS_BACK_MAX - 1) * sizeof g_tas.back[0]);
            g_tas.back_n--;
        }
        g_tas.back[g_tas.back_n++] = from;
        g_tas.rewinding = true;
    }
    tas_seek(from > k ? from - k : 0, resume);
}

/* Forward again to where the last rewinds began. */
static inline bool tas_undo_rewind(bool resume) {
    if (!g_tas.on || !g_tas.back_n) return false;
    uint32_t to = g_tas.back[--g_tas.back_n];
    if (to > g_tas.mv.n) to = g_tas.mv.n;
    g_tas.rewinding = false;
    tas_seek(to, resume);
    return true;
}

/* After the inputs changed under the board: run it back up to where it was
 * on the new ones, if what changed is behind it. */
static inline void tas__resync(uint32_t changed) {
    if (changed < g_tas.frame || g_tas.mid) {
        uint32_t to = g_tas.frame <= g_tas.mv.n ? g_tas.frame : g_tas.mv.n;
        tas_seek(to, false);
    }
}

/* ---- Edits from the window ------------------------------------------------------ */

static inline void tas_edit_held(uint32_t f, uint32_t xor_bits) {
    if (!g_tas.on || f >= g_tas.mv.n || !xor_bits) return;
    tas__undo_push();
    g_tas.mv.in[f].held ^= xor_bits;
    tas__dirty(f);
    tas__resync(f);
}

static inline void tas_insert(uint32_t f, uint32_t count) {
    if (!g_tas.on || f > g_tas.mv.n || !count) return;
    if (!tas__track_reserve(&g_tas.mv, g_tas.mv.n + count)) return;
    tas__undo_push();
    memmove(&g_tas.mv.in[f + count], &g_tas.mv.in[f], (g_tas.mv.n - f) * sizeof g_tas.mv.in[0]);
    memmove(&g_tas.mv.check[f + count], &g_tas.mv.check[f], (g_tas.mv.n - f) * sizeof g_tas.mv.check[0]);
    for (uint32_t i = 0; i < count; i++) {
        memset(&g_tas.mv.in[f + i], 0, sizeof g_tas.mv.in[0]);
        if (f > 0) g_tas.mv.in[f + i] = g_tas.mv.in[f - 1];   /* the frame before, held on */
    }
    g_tas.mv.n += count;
    tas__dirty(f);
    tas__resync(f);
}

static inline void tas_delete(uint32_t f, uint32_t count) {
    if (!g_tas.on || f >= g_tas.mv.n || !count) return;
    if (count > g_tas.mv.n - f) count = g_tas.mv.n - f;
    tas__undo_push();
    memmove(&g_tas.mv.in[f], &g_tas.mv.in[f + count], (g_tas.mv.n - f - count) * sizeof g_tas.mv.in[0]);
    memmove(&g_tas.mv.check[f], &g_tas.mv.check[f + count], (g_tas.mv.n - f - count) * sizeof g_tas.mv.check[0]);
    g_tas.mv.n -= count;
    tas__dirty(f);
    if (g_tas.frame > g_tas.mv.n) tas_seek(g_tas.mv.n, false);
    else tas__resync(f);
}

/* Drop everything after the cursor. */
static inline void tas_truncate(void) {
    if (!g_tas.on || g_tas.frame >= g_tas.mv.n) return;
    tas__undo_push();
    g_tas.mv.n = g_tas.frame;
    tas__dirty(g_tas.frame);
}

/* Undo (redo false) or redo the movie's last change. */
static inline bool tas_undo(bool redo) {
    if (!g_tas.on) return false;
    tas_undo_t *from = redo ? g_tas.redo : g_tas.undo, *to = redo ? g_tas.undo : g_tas.redo;
    int *from_n = redo ? &g_tas.redo_n : &g_tas.undo_n, *to_n = redo ? &g_tas.undo_n : &g_tas.redo_n;
    if (!*from_n) return false;
    tas__stack_push(to, to_n, &g_tas.mv, g_tas.frame);
    tas_undo_t *u = &from[*from_n - 1];
    uint32_t changed = tas__first_diff(&g_tas.mv, &u->track);
    tas_track_t keep = g_tas.mv;
    g_tas.mv = u->track;
    u->track = keep;
    tas__track_free(&u->track);
    (*from_n)--;
    tas__dirty(changed);
    if (g_tas.frame > g_tas.mv.n) tas_seek(g_tas.mv.n, false);
    else tas__resync(changed);
    return true;
}

static inline void tas_branch_save(int i) {
    if (!g_tas.on || i < 0 || i >= TAS_BRANCHES) return;
    tas_branch_t *b = &g_tas.branch[i];
    if (!tas__track_copy(&b->track, &g_tas.mv)) return;
    b->frame = g_tas.frame;
    b->used = true;
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    strftime(b->when, sizeof b->when, "%H:%M:%S", &tmv);
}

/* Put a branch's inputs back (Undo inputs takes it out again) and go to the
 * frame it was saved at. */
static inline void tas_branch_load(int i) {
    if (!g_tas.on || i < 0 || i >= TAS_BRANCHES || !g_tas.branch[i].used) return;
    tas_branch_t *b = &g_tas.branch[i];
    tas__undo_push();
    uint32_t changed = tas__first_diff(&g_tas.mv, &b->track);
    if (!tas__track_copy(&g_tas.mv, &b->track)) return;
    tas__dirty(changed);
    g_tas.rewinding = false;
    tas_seek(b->frame <= g_tas.mv.n ? b->frame : g_tas.mv.n, false);
}

/* ---- Start and stop ------------------------------------------------------------- */

/* Why the board cannot be taken now, or NULL. */
static inline const char *tas__busy(void) {
    if (!M2HLE_DEV_TOOLS)            return "this build has no TAS tools";
    if (!g_active_profile)           return "no ROM is loaded";
    if (replay__session_live())      return "a netplay session owns the board";
    if (replay_playing())            return "a replay is playing";
    if (follow_following())          return "the board is following another";
    if (m2comm_linked())             return "the board is linked to another cabinet";
    if (g_sky_eye.phase != SKY_EYE_OFF) return "SKY EYE is holding the stage";
    return NULL;
}

static inline void tas__reset_movie(void) {
    tas__gz_clear();
    tas__track_free(&g_tas.mv);
    tas__stack_clear(g_tas.undo, &g_tas.undo_n);
    tas__stack_clear(g_tas.redo, &g_tas.redo_n);
    for (int i = 0; i < TAS_BRANCHES; i++) {
        tas__track_free(&g_tas.branch[i].track);
        memset(&g_tas.branch[i], 0, sizeof g_tas.branch[i]);
    }
    g_tas.frame = 0;
    g_tas.mid = false;
    g_tas.stale = false;
    g_tas.rerecords = 0;
    g_tas.back_n = 0;
    g_tas.rewinding = false;
    g_tas.take_saved = false;
    g_tas.seek_req = -1;
    g_tas.seeking = false;
    g_tas.desync = -1;
    g_tas.gz_every_now = g_tas.gz_every;
}

/* The board as it is now becomes frame 0 of the movie (the anchor). */
static inline const char *tas__take_board(emu_thread_ctx_t *ctx) {
    rollback_layout(ctx->cpu, ctx->bus);
    g_tas.state_size = g_rollback.size;
    g_tas.on = true;
    g_tas.frame = 0;
    g_tas.mid = false;
    g_tas.stale = false;
    g_tas.gz_every_now = g_tas.gz_every;
    tas__gz_save(ctx);
    if (!g_tas.gz_n) { g_tas.on = false; return "out of memory for the first state"; }
    follow_lead_break("TAS start");
    replay_rec_break("TAS start");
    return NULL;
}

/* A new movie from the board as it is. Mutex held, between slices. */
static inline const char *tas_start(emu_thread_ctx_t *ctx) {
    const char *err = tas__busy();
    if (err) return err;
    tas__reset_movie();
    free(g_tas.anchor);
    g_tas.anchor = NULL;
    size_t len = 0;
    err = emu_state_save_mem(ctx, NULL, 0, &len);
    if (err) return err;
    g_tas.anchor = (uint8_t *)malloc(len);
    if (!g_tas.anchor) return "out of memory";
    err = emu_state_save_mem(ctx, g_tas.anchor, len, &g_tas.anchor_len);
    if (err) { free(g_tas.anchor); g_tas.anchor = NULL; return err; }
    follow_settings(g_tas.settings);
    g_tas.warning_skip = g_warning_skip;
    snprintf(g_tas.profile, sizeof g_tas.profile, "%s", g_active_profile->id);
    err = tas__take_board(ctx);
    snprintf(g_tas.msg, sizeof g_tas.msg, "%s", err ? err : "recording from here");
    return err;
}

/* The board back to the movie's start (the anchor) and the movie kept: what a
 * movie file's load does, and Restart. Mutex held. */
static inline const char *tas__restart(emu_thread_ctx_t *ctx) {
    if (!g_tas.anchor) return "no movie";
    const char *err = tas__busy();
    if (err) return err;
    if (strcmp(g_tas.profile, g_active_profile->id)) return "the movie is another game's";
    follow_settings_put(g_tas.settings);
    g_warning_skip = g_tas.warning_skip;
    err = emu_state_load_mem(ctx, g_tas.anchor, g_tas.anchor_len);
    if (err) return err;
    tas__gz_clear();
    return tas__take_board(ctx);
}

/* Let the board go; the movie stays (to save, or to take up again). */
static inline void tas_stop(const char *why) {
    if (!g_tas.on) return;
    if (g_tas.seeking) g_sound_mute = 0;
    g_tas.on = false;
    g_tas.seeking = false;
    g_tas.seek_req = -1;
    tas__gz_clear();
    if (why) snprintf(g_tas.msg, sizeof g_tas.msg, "stopped: %s", why);
}

/* Something the movie cannot carry changed the board (a reset, a state load). */
static inline void tas_break(const char *why) { tas_stop(why); }

/* Take the movie up again from its start: the board goes back to it. */
static inline const char *tas_resume(emu_thread_ctx_t *ctx) {
    const char *err = tas__restart(ctx);
    snprintf(g_tas.msg, sizeof g_tas.msg, "%s", err ? err : "back at the movie's start");
    if (!err) tas_seek(g_tas.mv.n, false);
    return err;
}

/* After a seek the pad holds what the movie holds at the cursor, so going on
 * from there records the same frames until a button is changed. */
static inline void tas__pad_from_movie(void) {
    g_tas.pad.once = 0;
    if (g_tas.frame >= g_tas.mv.n) return;
    const tas_input_t *in = &g_tas.mv.in[g_tas.frame];
    g_tas.pad.hold = in->held;
    for (int p = 0; p < LIGHTGUN_PLAYERS; p++) {
        const uint8_t *q = in->gun + p * 12;
        if (!g_tas.pad.aim_on[p] || !q[0]) continue;
        uint32_t x = 0, y = 0;
        for (int i = 0; i < 4; i++) { x |= (uint32_t)q[4 + i] << (8 * i); y |= (uint32_t)q[8 + i] << (8 * i); }
        g_tas.pad.aim_off[p] = q[1] != 0;
        g_tas.pad.aim_x[p] = lightgun__bitsf(x);
        g_tas.pad.aim_y[p] = lightgun__bitsf(y);
    }
}

/* ---- The slice's hooks (emu_slice_body, under the mutex) ------------------------- */

/* The virtual pad's input for the frame about to run. */
static inline tas_input_t tas__pad_input(void) {
    tas_input_t in;
    memset(&in, 0, sizeof in);
    in.held = g_tas.pad.hold | g_tas.pad.once;
    if (g_tas.keys_too) in.held |= g_input.held;
    if (g_active_profile && g_active_profile->quirks.lightgun) {
        uint8_t host[LIGHTGUN_FEED_BYTES];
        lightgun_latch(host);   /* the host's pointers, as a recorder takes them */
        lightgun_unlatch();
        memcpy(in.gun, host, sizeof in.gun);
        for (int p = 0; p < LIGHTGUN_PLAYERS; p++) {
            if (!g_tas.pad.aim_on[p]) continue;
            uint8_t *q = in.gun + p * 12;
            uint32_t x = lightgun__fbits(g_tas.pad.aim_x[p]), y = lightgun__fbits(g_tas.pad.aim_y[p]);
            q[0] = 1; q[1] = g_tas.pad.aim_off[p] ? 1 : 0; q[3] = 0;   /* q[2]: the host's border */

            for (int i = 0; i < 4; i++) { q[4 + i] = (uint8_t)(x >> (8 * i)); q[8 + i] = (uint8_t)(y >> (8 * i)); }
        }
    }
    return in;
}

/* The input frame `f` runs on: the movie's, or (recording) the pad's written in.
 * A seek asked for and not taken up yet plays too: a movie opened on a
 * running board had its first frame taken from the pad before the seek began,
 * and that take cut the rest of the movie (Pinboard #1640). */
static inline tas_input_t tas__frame_input(uint32_t f) {
    bool play = g_tas.seeking || ((!g_tas.recording || g_tas.seek_req >= 0) && f < g_tas.mv.n);
    if (play || g_tas.mid) return f < g_tas.mv.n ? g_tas.mv.in[f] : (tas_input_t){ 0 };
    tas_input_t in = tas__pad_input();
    if (f < g_tas.mv.n) {
        if (tas__input_eq(&in, &g_tas.mv.in[f])) return in;
        if (!g_tas.take_saved) {
            tas__undo_push();
            g_tas.take_saved = true;
            g_tas.rerecords++;
        }
        if (g_tas.cut) g_tas.mv.n = f;
        else { g_tas.mv.in[f] = in; tas__dirty(f); return in; }
        tas__dirty(f);
    }
    if (!tas__track_reserve(&g_tas.mv, f + 1)) return in;
    g_tas.mv.in[f] = in;
    g_tas.mv.check[f] = 0;
    g_tas.mv.n = f + 1;
    return in;
}

static inline void tas_slice_begin(emu_thread_ctx_t *ctx) {
    (void)ctx;
    if (!tas_live()) return;
    tas_input_t in = tas__frame_input(g_tas.frame);
    g_tas.use_net_was = g_input.use_net;
    g_tas.latched = true;
    g_input.net_held = in.held;
    g_input.use_net  = 1;
    if (g_active_profile && g_active_profile->quirks.lightgun) lightgun_feed_put(in.gun);
}

static inline void tas_slice_end(emu_thread_ctx_t *ctx, bool frame_end) {
    if (!g_tas.latched) return;
    g_tas.latched = false;
    g_input.use_net = g_tas.use_net_was;
    lightgun_unlatch();
    if (!g_tas.on) return;
    if (!frame_end) { g_tas.mid = true; return; }
    g_tas.mid = false;
    uint32_t f = g_tas.frame;
    if (f < g_tas.mv.n) {
        uint32_t check = netplay_frame_check(ctx->cpu, ctx->total_steps);
        if (!check) check = 1;
        if (g_tas.mv.check[f] && g_tas.mv.check[f] != check && g_tas.desync < 0) {
            g_tas.desync = f;
            LOG_WARN("tas: frame %u ended on check %08X, %08X the first time (same inputs)",
                     f, check, g_tas.mv.check[f]);
        }
        g_tas.mv.check[f] = check;
    }
    g_tas.frame = f + 1;
    if (g_tas.seeking) {
        if (g_tas.frame >= g_tas.seek_to) {
            g_tas.seeking = false;
            sound_settle();
            g_sound_mute = 0;
            tas__pad_from_movie();
            if (g_tas.resume_after) ctx->frame_budget = 0;   /* run on: emu_slice_count_frame stops at none */
        }
    } else {
        g_tas.pad.once = 0;
        g_tas.rewinding = false;
        g_tas.take_saved = g_tas.take_saved && g_tas.frame < g_tas.mv.n;
    }
}

/* Run unpaced: a seek on its way. */
static inline bool tas_unpaced(void) { return tas_live() && g_tas.seeking; }

/* How many 60 Hz frames each board frame lasts (slow motion). */
static inline int tas_slowdown(void) {
    return tas_live() && !g_tas.seeking && g_tas.slowdown > 1 ? g_tas.slowdown : 1;
}

/* ---- Between slices (the emu thread, outside the mutex) --------------------------- */

/* Whether a seek to target starts from greenzone state g rather than running
 * on from here. */
static inline bool tas__seek_loads(uint32_t target, int g) {
    if (target == 0 && g_tas.frame == 0 && !g_tas.mid && !g_tas.stale) return false;
    return g_tas.mid || g_tas.stale || target < g_tas.frame || (target == g_tas.frame && target > 0)
        || g_tas.gz[g].frame > g_tas.frame;
}

static inline void tas__seek_service(emu_thread_ctx_t *ctx) {
    uint32_t target = (uint32_t)g_tas.seek_req;
    bool resume = g_tas.seek_resume;
    g_tas.seek_req = -1;
    if (target > g_tas.mv.n) target = g_tas.mv.n;
    /* A stop asked for and not done yet (F11 on a running board) counts as done. */
    bool was_running = ctx->run_state == EMU_RUNNING && !ctx->frame_budget && !ctx->request_stop;
    g_tas.take_saved = false;   /* a take from the new place is a new undo step */
    /* Land by running into the target, so its picture is drawn. */
    int g = tas__gz_at_or_before(target ? target - 1 : 0);
    if (g < 0) return;
    if (tas__seek_loads(target, g)) tas__gz_load(ctx, g);
    g_tas.resume_after = resume || was_running;
    if (g_tas.frame < target) {
        sound_settle();
        g_sound_mute = 1;
        g_tas.seeking = true;
        g_tas.seek_to = target;
        ctx->request_stop = 0;
        ctx->step_over_bp = 1;
        ctx->frame_budget = target - g_tas.frame;
        ctx->run_state = EMU_RUNNING;
    } else {
        tas__pad_from_movie();
        if (!g_tas.resume_after) {
            ctx->frame_budget = 0;
            ctx->run_state = EMU_STOPPED;
        }
    }
}

/* A movie on: a seek paused, asked for, and the greenzone's states. */
static inline void tas__pump_on(emu_thread_ctx_t *ctx) {
    /* A pause in the middle of a seek: it stops where it is. */
    if (g_tas.seeking && ctx->run_state == EMU_STOPPED) {
        g_tas.seeking = false;
        sound_settle();
        g_sound_mute = 0;
    }
    if (g_tas.seek_req >= 0) tas__seek_service(ctx);
    if (!g_tas.mid && (g_tas.frame % tas__gz_every() == 0 || ctx->run_state == EMU_STOPPED))
        tas__gz_save(ctx);
}

/* reset_pending: a reset is asked for and not done yet (a start waits for it). */
static inline void tas_pump(emu_thread_ctx_t *ctx, bool session, bool reset_pending) {
    if (!M2HLE_DEV_TOOLS || (!g_tas.on && g_tas.seek_req < 0 && !g_tas.start_req)) return;
    emu_mutex_lock(&ctx->mutex);
    if (g_tas.start_req && !reset_pending) {
        g_tas.start_req = false;
        const char *err = tas_start(ctx);
        if (err) snprintf(g_tas.msg, sizeof g_tas.msg, "cannot start: %s", err);
    }
    if (g_tas.on && session) tas_break("a netplay session took the board");
    if (g_tas.on) {
        tas__pump_on(ctx);
    } else {
        g_tas.seek_req = -1;
    }
    emu_mutex_unlock(&ctx->mutex);
}

/* ---- Movie files (.m2tas) --------------------------------------------------------
 *
 * A zip: tas.json (the label), start.sta (the anchor), inputs.bin (the movie)
 * and branch-N.bin for each branch kept. An inputs file is "M2TASIN1", then
 * u32 frames, u32 bytes a frame (TAS_INPUT_BYTES), then each frame's held
 * word and GUN payload, all little-endian. */

#define TAS_INPUTS_MAGIC "M2TASIN1"

static inline uint8_t *tas__inputs_pack(const tas_track_t *t, size_t *len) {
    size_t n = 16 + (size_t)t->n * TAS_INPUT_BYTES;
    uint8_t *b = (uint8_t *)malloc(n);
    if (!b) return NULL;
    memcpy(b, TAS_INPUTS_MAGIC, 8);
    follow_put32(b + 8, t->n);
    follow_put32(b + 12, TAS_INPUT_BYTES);
    for (uint32_t f = 0; f < t->n; f++) {
        uint8_t *p = b + 16 + (size_t)f * TAS_INPUT_BYTES;
        follow_put32(p, t->in[f].held);
        memcpy(p + 4, t->in[f].gun, LIGHTGUN_FEED_BYTES);
    }
    *len = n;
    return b;
}

static inline bool tas__inputs_unpack(tas_track_t *t, const uint8_t *b, size_t n) {
    if (n < 16 || memcmp(b, TAS_INPUTS_MAGIC, 8)) return false;
    uint32_t frames = follow_get32(b + 8), rec = follow_get32(b + 12);
    if (rec < 4 || (size_t)frames * rec > n - 16) return false;
    t->n = 0;
    if (!tas__track_reserve(t, frames ? frames : 1)) return false;
    for (uint32_t f = 0; f < frames; f++) {
        const uint8_t *p = b + 16 + (size_t)f * rec;
        memset(&t->in[f], 0, sizeof t->in[f]);
        t->in[f].held = follow_get32(p);
        uint32_t g = rec - 4 < LIGHTGUN_FEED_BYTES ? rec - 4 : LIGHTGUN_FEED_BYTES;
        memcpy(t->in[f].gun, p + 4, g);
        t->check[f] = 0;
    }
    t->n = frames;
    return true;
}

static inline const char *tas_save_file(const char *path) {
    if (!g_tas.anchor) return "no movie";
    char json[1024];
    int k = snprintf(json, sizeof json,
                     "{\"format\":%d,\"profile\":\"%s\",\"frames\":%u,\"rerecords\":%u,"
                     "\"warning_skip\":%d,\"settings\":\"%d,%d,%d,%d,%d,%d,%d,%d\",\"branches\":\"",
                     TAS_FORMAT, g_tas.profile, g_tas.mv.n, g_tas.rerecords, g_tas.warning_skip,
                     g_tas.settings[0], g_tas.settings[1], g_tas.settings[2], g_tas.settings[3],
                     g_tas.settings[4], g_tas.settings[5], g_tas.settings[6], g_tas.settings[7]);
    for (int i = 0; i < TAS_BRANCHES && k < (int)sizeof json - 40; i++)
        if (g_tas.branch[i].used) k += snprintf(json + k, sizeof json - (size_t)k, "%d:%u;", i, g_tas.branch[i].frame);
    snprintf(json + k, sizeof json - (size_t)k, "\"}");
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (!mz_zip_writer_init_file(&z, path, 0)) return "cannot write the file";
    size_t ilen = 0;
    uint8_t *ins = tas__inputs_pack(&g_tas.mv, &ilen);
    bool ok = ins
           && mz_zip_writer_add_mem(&z, "tas.json", json, strlen(json), MZ_DEFAULT_LEVEL)
           && mz_zip_writer_add_mem(&z, "start.sta", g_tas.anchor, g_tas.anchor_len, MZ_BEST_SPEED)
           && mz_zip_writer_add_mem(&z, "inputs.bin", ins, ilen, MZ_DEFAULT_LEVEL);
    free(ins);
    for (int i = 0; ok && i < TAS_BRANCHES; i++) {
        if (!g_tas.branch[i].used) continue;
        char name[32];
        snprintf(name, sizeof name, "branch-%d.bin", i);
        size_t blen = 0;
        uint8_t *b = tas__inputs_pack(&g_tas.branch[i].track, &blen);
        ok = b && mz_zip_writer_add_mem(&z, name, b, blen, MZ_DEFAULT_LEVEL);
        free(b);
    }
    ok = ok && mz_zip_writer_finalize_archive(&z);
    mz_zip_writer_end(&z);
    if (!ok) return "could not write the movie";
    snprintf(g_tas.msg, sizeof g_tas.msg, "saved %u frames", g_tas.mv.n);
    return NULL;
}

static inline void *tas__extract(mz_zip_archive *z, const char *name, size_t *n) {
    int i = mz_zip_reader_locate_file(z, name, NULL, 0);
    *n = 0;
    return i < 0 ? NULL : mz_zip_reader_extract_to_heap(z, (mz_uint)i, n, 0);
}

/* tas.json's fields. */
typedef struct {
    char     profile[32], settings[128], branches[128];
    uint32_t format, rerecords;
    int      wskip;
} tas__label_t;

/* tas.json (*json, jn bytes, NUL-ended here) into lab, and the inputs into t;
 * NULL, or why the movie will not load. */
static inline const char *tas__read_label(char **json, size_t jn, tas__label_t *lab, tas_track_t *t,
                                          const uint8_t *ins, size_t in_n) {
    memset(lab, 0, sizeof *lab);
    lab->wskip = 1;
    char *j = (char *)realloc(*json, jn + 1);
    if (j) { *json = j; j[jn] = 0; }
    json_get_u32(*json, "format", &lab->format);
    json_get_str(*json, "profile", lab->profile, sizeof lab->profile);
    json_get_u32(*json, "rerecords", &lab->rerecords);
    json_get_int(*json, "warning_skip", &lab->wskip);
    json_get_str(*json, "settings", lab->settings, sizeof lab->settings);
    json_get_str(*json, "branches", lab->branches, sizeof lab->branches);
    if (lab->format != TAS_FORMAT)                      return "a movie of another format";
    if (strcmp(lab->profile, g_active_profile->id))      return "a movie of another game (load its ROM first)";
    if (!tas__inputs_unpack(t, ins, in_n))              return "the movie's inputs are damaged";
    return NULL;
}

/* The branches tas.json lists ("N:frame;..."), each from its branch-N.bin. */
static inline void tas__load_branches(mz_zip_archive *z, const char *branches) {
    for (const char *p = branches; *p;) {
        int i = -1;
        unsigned fr = 0;
        if (sscanf(p, "%d:%u", &i, &fr) == 2 && i >= 0 && i < TAS_BRANCHES) {
            char name[32];
            snprintf(name, sizeof name, "branch-%d.bin", i);
            size_t bn = 0;
            uint8_t *b = (uint8_t *)tas__extract(z, name, &bn);
            if (b && tas__inputs_unpack(&g_tas.branch[i].track, b, bn)) {
                g_tas.branch[i].used = true;
                g_tas.branch[i].frame = fr;
                snprintf(g_tas.branch[i].when, sizeof g_tas.branch[i].when, "from the file");
            }
            free(b);
        }
        const char *s = strchr(p, ';');
        if (!s) break;
        p = s + 1;
    }
}

/* Open a movie: the board goes to its start, and the movie is played up to
 * its end (resume false: it stops there). Mutex held, between slices. */
static inline const char *tas_load_file(emu_thread_ctx_t *ctx, const char *path, bool to_end) {
    const char *err = tas__busy();
    if (err) return err;
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_file(&z, path, 0)) return "not a movie (not a zip)";
    size_t jn = 0, an = 0, in_n = 0;
    char *json = (char *)tas__extract(&z, "tas.json", &jn);
    uint8_t *anchor = (uint8_t *)tas__extract(&z, "start.sta", &an);
    uint8_t *ins = (uint8_t *)tas__extract(&z, "inputs.bin", &in_n);
    tas_track_t t;
    memset(&t, 0, sizeof t);
    tas__label_t lab;
    if (!json || !anchor || !ins) err = "not a movie (an entry is missing)";
    else err = tas__read_label(&json, jn, &lab, &t, ins, in_n);
    if (!err) {
        tas__reset_movie();
        free(g_tas.anchor);
        g_tas.anchor = anchor;
        g_tas.anchor_len = an;
        anchor = NULL;
        g_tas.mv = t;
        memset(&t, 0, sizeof t);
        g_tas.rerecords = lab.rerecords;
        snprintf(g_tas.profile, sizeof g_tas.profile, "%s", lab.profile);
        g_tas.warning_skip = lab.wskip;
        follow_settings(g_tas.settings);
        sscanf(lab.settings, "%d,%d,%d,%d,%d,%d,%d,%d", &g_tas.settings[0], &g_tas.settings[1], &g_tas.settings[2],
               &g_tas.settings[3], &g_tas.settings[4], &g_tas.settings[5], &g_tas.settings[6], &g_tas.settings[7]);
        tas__load_branches(&z, lab.branches);
        err = tas__restart(ctx);
        if (!err) tas_seek(to_end ? g_tas.mv.n : 0, false);
        snprintf(g_tas.msg, sizeof g_tas.msg, "%s", err ? err : "movie opened");
    }
    tas__track_free(&t);
    free(json);
    free(anchor);
    free(ins);
    mz_zip_reader_end(&z);
    return err;
}

#endif /* M2HLE_TAS_H */
