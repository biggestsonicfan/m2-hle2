/* replay.h -- record online matches, and play them back (Pinboard #572).
 *
 * A replay is follow.h's join point and feed for one match, in one zip:
 *   replay.json   who played whom, when, where, who won (replay__json)
 *   state.sta     the savestate the match starts from: the session's cold boot,
 *                 or for a VS rematch the board where the last match's tail ended
 *   inputs.feed   follow.h's feed from that state: every slice's input word,
 *                 the HLE settings, the run loop's board flags, and every
 *                 frame's check, so a playback that leaves the recording says so
 * The board reads nothing else from outside in a session (CLAUDE.md, "A scripted
 * session must not write memory"), so the feed alone replays it. A write that
 * does land (MCP write_memory) goes in as follow.h's WRITE.
 *
 * Recording (g_replay_rec): when the player has asked for it, each session's
 * reset starts one (emu_netplay_pump), the slice hooks below write the feed, and
 * the result (the profile's versus hook, from emu_slice_count_frame) ends it
 * REPLAY_TAIL_FRAMES later, so the winner's pose is in. A VS session plays on
 * after a result, so the next match starts a new replay from where the board
 * is. A session that ends with no result keeps what it has, unless that is
 * shorter than REPLAY_MIN_FRAMES. A PS3 match is not recorded: its board is
 * not run on our lockstep.
 *
 * The zip is made beside the board (a writer thread; the web build has none and
 * makes it at once) and saved as <p1>-vs-<p2>-<local date>-<time>.m2replay, in
 * the replay folder (native) or handed to the page (web_replay_take).
 *
 * Playback (g_replay_play) joins follow.h as a follower on the replay's state
 * and feed: the replay's profile is installed for it, the battery detached, and
 * everything is put back when it stops. Seeking back is a rejoin and a run to
 * the frame unpaced; forward is the run alone.
 */
#ifndef REPLAY_H
#define REPLAY_H

#include <time.h>
#include "json_min.h"

#define REPLAY_FORMAT      1
#define REPLAY_EXT         ".m2replay"
#define REPLAY_FEED_MAX    (64u << 20)
#define REPLAY_TAIL_FRAMES 240u
#define REPLAY_MIN_FRAMES  600u
#define REPLAY_JSON_MAX    4096
#define REPLAY_NAME_MAX    24

typedef struct {
    char     p1[REPLAY_NAME_MAX], p2[REPLAY_NAME_MAX], recorded_by[REPLAY_NAME_MAX];
    char     server[128], profile[32], romset[32], why[48];
    int      side;              /* the recorder's: 0 1P, 1 2P, 2 watching */
    uint32_t session, match;
    int64_t  when;              /* time(NULL) at the match's first frame */
    game_match_info_t fight;    /* as last seen while fighting */
    bool     fight_seen;
    int      winner;            /* 0 1P, 1 2P, -1 none */
    uint32_t result_frame, frames;
    int32_t  settings[FOLLOW_SET_WORDS];
} replay_meta_t;

/* A finished replay on its way out. */
typedef struct {
    uint8_t      *snap, *feed;
    size_t        snap_len, feed_len;
    replay_meta_t meta;
    char          dir[512];
} replay_job_t;

typedef struct {
    volatile int want;          /* the player's switch */
    char     dir[512];          /* where native builds save ("" = the user dir's replays) */
    bool     on, due;
    uint8_t *feed;
    size_t   len, cap;
    bool     full;
    uint32_t frame, slice;
    bool     held_known, set_known, board_known;
    uint32_t held, board;
    int32_t  set[FOLLOW_SET_WORDS];
    int      use_net_was;
    bool     latched;
    uint32_t tail;              /* frames still to record after the result; 0 = none yet */
    replay_meta_t meta;
    uint8_t *snap;
    size_t   snap_cap, snap_len;
    replay_job_t job;
    bool     writing;
#ifdef _WIN32
    HANDLE   writer;
#elif !defined(__EMSCRIPTEN__)
    pthread_t writer;
#endif
    uint32_t saved;             /* replays saved by this process */
    char     last[720];         /* the newest one's path (native) or name (web) */
    char     error[160];
#ifdef __EMSCRIPTEN__
    uint8_t *ready;             /* a finished zip for the page (web_replay_take) */
    size_t   ready_len;
    char     ready_name[128];
#endif
} replay_rec_t;

static replay_rec_t g_replay_rec;

static inline bool replay_recording(void) { return g_replay_rec.on; }

/* ---- The label ------------------------------------------------------------ */

static inline void replay__copy_name(char *out, const char *in) {
    snprintf(out, REPLAY_NAME_MAX, "%s", in && in[0] ? in : "?");
}

/* The two fighters and the recorder, from the room. */
static inline void replay__names(replay_meta_t *m) {
    for (int side = 0; side < 2; side++) {
        uint16_t id = g_netplay.room.fighter[side];
        if (id) replay__copy_name(side ? m->p2 : m->p1, netplay_member_name(id));
    }
    replay__copy_name(m->recorded_by, g_netplay.session.npid);
}

static inline void replay__sample(emu_thread_ctx_t *ctx, replay_meta_t *m) {
    if (!g_active_profile || !g_active_profile->match_info) return;
    game_match_info_t f;
    memset(&f, 0, sizeof f);
    g_active_profile->match_info(ctx->bus, &f);
    if (!f.fighting) return;
    m->fight      = f;
    m->fight_seen = true;
}

static inline void replay__meta_begin(replay_meta_t *m) {
    memset(m, 0, sizeof *m);
    replay__copy_name(m->p1, "1P");
    replay__copy_name(m->p2, "2P");
    replay__names(m);
    snprintf(m->server, sizeof m->server, "%s", g_netplay.cfg.server);
    if (g_active_profile) {
        snprintf(m->profile, sizeof m->profile, "%s", g_active_profile->id);
        snprintf(m->romset, sizeof m->romset, "%s", profile_rom_set(g_active_profile));
    }
    int lp = g_netplay.local_player;
    m->side    = lp == 0 || lp == 1 ? lp : 2;
    m->session = g_netplay.generation;
    m->match   = g_netplay.match_started;
    m->when    = (int64_t)time(NULL);
    m->winner  = -1;
    follow_settings(m->settings);
}

/* A name as a file name has it: letters, digits, '-' and '_'. */
static inline void replay__file_part(char *out, size_t cap, const char *in) {
    size_t n = 0;
    for (const unsigned char *c = (const unsigned char *)in; *c && n + 1 < cap; c++)
        out[n++] = (isalnum(*c) || *c == '-' || *c == '_') ? (char)*c : '_';
    out[n] = '\0';
    if (!n) snprintf(out, cap, "player");
}

/* <p1>-vs-<p2>-YYYYMMDD-HHMMSS.m2replay, in the recorder's local time. */
static inline void replay_file_name(const replay_meta_t *m, char *out, size_t cap) {
    char a[REPLAY_NAME_MAX], b[REPLAY_NAME_MAX], when[32] = "00000000-000000";
    replay__file_part(a, sizeof a, m->p1);
    replay__file_part(b, sizeof b, m->p2);
    time_t t = (time_t)m->when;
    struct tm *tm = localtime(&t);
    if (tm) strftime(when, sizeof when, "%Y%m%d-%H%M%S", tm);
    snprintf(out, cap, "%s-vs-%s-%s%s", a, b, when, REPLAY_EXT);
}

/* `,"key":"value"` with the value escaped. */
static inline void replay__json_str(char *out, size_t cap, size_t *n, const char *key, const char *v) {
    char esc[256];
    json_escape(esc, sizeof esc, v ? v : "");
    if (*n < cap) *n += (size_t)snprintf(out + *n, cap - *n, ",\"%s\":\"%s\"", key, esc);
}

static inline void replay__json_int(char *out, size_t cap, size_t *n, const char *key, long long v) {
    if (*n < cap) *n += (size_t)snprintf(out + *n, cap - *n, ",\"%s\":%lld", key, v);
}

/* The fight's half of replay.json: characters, stage, rounds, result. */
static inline void replay__json_fight(const replay_meta_t *m, char *out, size_t cap, size_t *n) {
    const game_match_info_t *f = &m->fight;
    char unknown[24];
    for (int side = 0; side < 2 && m->fight_seen; side++) {
        const char *key = side ? "p2_character" : "p1_character";
        snprintf(unknown, sizeof unknown, "#%d", f->chara[side]);
        replay__json_str(out, cap, n, key, f->chara_name[side] ? f->chara_name[side] : unknown);
        replay__json_int(out, cap, n, side ? "p2_character_id" : "p1_character_id", f->chara[side]);
        replay__json_int(out, cap, n, side ? "p2_rounds" : "p1_rounds", f->rounds[side]);
    }
    if (m->fight_seen) {
        snprintf(unknown, sizeof unknown, "#%d", f->stage);
        replay__json_str(out, cap, n, "stage", f->stage_name ? f->stage_name : unknown);
        replay__json_int(out, cap, n, "stage_id", f->stage);
        replay__json_int(out, cap, n, "rounds_to_win", f->rounds_to_win);
    }
    replay__json_int(out, cap, n, "winner", m->winner);
    if (m->winner == 0 || m->winner == 1) {
        replay__json_str(out, cap, n, "winner_name", m->winner ? m->p2 : m->p1);
        replay__json_str(out, cap, n, "loser_name",  m->winner ? m->p1 : m->p2);
        replay__json_int(out, cap, n, "result_frame", m->result_frame);
    }
}

/* replay.json: one flat object, so json_min.h reads every field back. */
static inline size_t replay__json(const replay_meta_t *m, char *out, size_t cap) {
    char date[40] = "";
    time_t t = (time_t)m->when;
    struct tm *tm = gmtime(&t);
    if (tm) strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%SZ", tm);
    size_t n = (size_t)snprintf(out, cap, "{\"format\":%d", REPLAY_FORMAT);
    replay__json_str(out, cap, &n, "emulator", "m2hle " M2HLE_VERSION);
    replay__json_str(out, cap, &n, "romset", m->romset);
    replay__json_str(out, cap, &n, "profile", m->profile);
    replay__json_str(out, cap, &n, "date", date);
    replay__json_int(out, cap, &n, "timestamp", m->when);
    replay__json_str(out, cap, &n, "server", m->server);
    replay__json_int(out, cap, &n, "session", m->session);
    replay__json_int(out, cap, &n, "match", m->match);
    replay__json_str(out, cap, &n, "recorded_by", m->recorded_by);
    replay__json_int(out, cap, &n, "side", m->side);
    replay__json_str(out, cap, &n, "p1", m->p1);
    replay__json_str(out, cap, &n, "p2", m->p2);
    replay__json_fight(m, out, cap, &n);
    replay__json_int(out, cap, &n, "finished", m->winner >= 0);
    replay__json_str(out, cap, &n, "ended", m->why);
    replay__json_int(out, cap, &n, "frames", m->frames);
    replay__json_int(out, cap, &n, "seconds", (m->frames + 30) / 60);
    for (int i = 0; i < FOLLOW_SET_WORDS; i++) {
        char key[16];
        snprintf(key, sizeof key, "set%d", i);
        replay__json_int(out, cap, &n, key, m->settings[i]);
    }
    if (n < cap) n += (size_t)snprintf(out + n, cap - n, "}\n");
    return n < cap ? n : cap - 1;
}

/* ---- The zip ---------------------------------------------------------------- */

/* The replay as a zip on the heap (mz_free it), or NULL. */
static inline void *replay__zip(const replay_job_t *j, size_t *out_len) {
    char json[REPLAY_JSON_MAX];
    size_t jn = replay__json(&j->meta, json, sizeof json);
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    void *buf = NULL;
    *out_len = 0;
    if (!mz_zip_writer_init_heap(&z, 0, j->snap_len / 4 + j->feed_len + 65536)) return NULL;
    bool ok = mz_zip_writer_add_mem(&z, "replay.json", json, jn, MZ_DEFAULT_LEVEL)
           && mz_zip_writer_add_mem(&z, "state.sta", j->snap, j->snap_len, MZ_BEST_SPEED)
           && mz_zip_writer_add_mem(&z, "inputs.feed", j->feed, j->feed_len, MZ_DEFAULT_LEVEL)
           && mz_zip_writer_finalize_heap_archive(&z, &buf, out_len);
    mz_zip_writer_end(&z);
    if (!ok) { mz_free(buf); buf = NULL; *out_len = 0; }
    return buf;
}

/* The replay folder: --replay-dir, else <user dir>/replays. Created here. */
static inline bool replay_dir(char *out, size_t cap) {
    if (g_replay_rec.dir[0]) {
        snprintf(out, cap, "%s", g_replay_rec.dir);
    } else {
#ifdef __EMSCRIPTEN__
        out[0] = '\0';
        return false;
#else
        if (!backup_ram_user_dir(out, cap)) return false;
        size_t n = strlen(out);
        snprintf(out + n, cap - n, BACKUP_RAM_SEP "replays");
#endif
    }
#ifndef __EMSCRIPTEN__
    backup_ram_mkdir(out);
#endif
    return true;
}

static inline void replay__job_free(replay_job_t *j) {
    free(j->snap);
    free(j->feed);
    j->snap = j->feed = NULL;
}

/* Write the finished replay out: a file natively, the page's slot on the web. */
static inline void replay__write_out(void) {
    replay_rec_t *R = &g_replay_rec;
    replay_job_t *j = &R->job;
    char name[160];
    replay_file_name(&j->meta, name, sizeof name);
    size_t len = 0;
    void *zip = replay__zip(j, &len);
    replay__job_free(j);
    if (!zip) { snprintf(R->error, sizeof R->error, "could not make %s", name); return; }
#ifdef __EMSCRIPTEN__
    free(R->ready);
    R->ready = (uint8_t *)malloc(len);
    if (R->ready) memcpy(R->ready, zip, len);
    R->ready_len = R->ready ? len : 0;
    snprintf(R->ready_name, sizeof R->ready_name, "%s", name);
    snprintf(R->last, sizeof R->last, "%s", name);
    R->saved++;
#else
    char path[700], tmp[720];
    snprintf(path, sizeof path, "%s" BACKUP_RAM_SEP "%s", j->dir, name);
    for (int k = 2; k < 100; k++) {   /* two matches in one second: name-2, name-3 ... */
        FILE *probe = fopen(path, "rb");
        if (!probe) break;
        fclose(probe);
        snprintf(path, sizeof path, "%s" BACKUP_RAM_SEP "%.*s-%d%s", j->dir,
                 (int)(strlen(name) - strlen(REPLAY_EXT)), name, k, REPLAY_EXT);
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(zip, 1, len, f) == len;
    if (f) ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(tmp, path) == 0;
    if (!ok) {
        remove(tmp);
        snprintf(R->error, sizeof R->error, "could not write %.120s", path);
        LOG_WARN("replay: %s", R->error);
    } else {
        snprintf(R->last, sizeof R->last, "%s", path);
        R->error[0] = '\0';
        R->saved++;
        LOG_INFO("replay: saved %s (%zu KB)", path, len / 1024);
    }
#endif
    mz_free(zip);
}

#ifdef _WIN32
static DWORD WINAPI replay__writer_proc(LPVOID arg) { (void)arg; replay__write_out(); return 0; }
#elif !defined(__EMSCRIPTEN__)
static void *replay__writer_proc(void *arg) { (void)arg; replay__write_out(); return NULL; }
#endif

static inline void replay__wait(void) {
    replay_rec_t *R = &g_replay_rec;
    if (!R->writing) return;
#ifdef _WIN32
    WaitForSingleObject(R->writer, INFINITE);
    CloseHandle(R->writer);
#elif !defined(__EMSCRIPTEN__)
    pthread_join(R->writer, NULL);
#endif
    R->writing = false;
}

static inline void replay__write_start(void) {
    replay_rec_t *R = &g_replay_rec;
#ifdef _WIN32
    R->writer  = CreateThread(NULL, 0, replay__writer_proc, NULL, 0, NULL);
    R->writing = R->writer != NULL;
#elif !defined(__EMSCRIPTEN__)
    R->writing = pthread_create(&R->writer, NULL, replay__writer_proc, NULL) == 0;
#endif
    if (!R->writing) replay__write_out();
}

/* ---- Recording -------------------------------------------------------------- */

static inline void replay__rec(int type, uint32_t slice, const void *p, uint32_t n) {
    replay_rec_t *R = &g_replay_rec;
    if (R->full) return;
    size_t need = R->len + FOLLOW_REC_BYTES + n;
    if (need > R->cap) {
        size_t cap = R->cap ? R->cap : (1u << 20);
        while (cap < need) cap *= 2;
        uint8_t *b = cap <= REPLAY_FEED_MAX ? (uint8_t *)realloc(R->feed, cap) : NULL;
        if (!b) {   /* the replay ends here; the END below still fits the old block */
            R->full = true;
            return;
        }
        R->feed = b;
        R->cap  = cap;
    }
    follow__rec_hdr(R->feed + R->len, type, R->frame, slice, n);
    if (n) memcpy(R->feed + R->len + FOLLOW_REC_BYTES, p, n);
    R->len = need;
}

/* The board as it stands, as a stored zip in R->snap (follow__lead_snap's way). */
static inline const char *replay__snap(emu_thread_ctx_t *ctx) {
    replay_rec_t *R = &g_replay_rec;
    for (int tries = 0; tries < 2; tries++) {
        if (!R->snap) {
            size_t need = 0;
            const char *err = emu_state_save_mem(ctx, NULL, 0, &need);
            if (err) return err;
            R->snap_cap = need + (1u << 20);
            R->snap = (uint8_t *)malloc(R->snap_cap);
            if (!R->snap) return "out of memory";
        }
        const char *err = emu_state_save_mem(ctx, R->snap, R->snap_cap, &R->snap_len);
        if (!err) return NULL;
        free(R->snap);
        R->snap = NULL;
        if (tries) return err;
    }
    return "no state";
}

/* Start a replay here: the board's state, then an empty feed. */
static inline void replay__begin(emu_thread_ctx_t *ctx) {
    replay_rec_t *R = &g_replay_rec;
    R->due = false;
    const char *err = replay__snap(ctx);
    if (err) {
        snprintf(R->error, sizeof R->error, "could not start a replay: %s", err);
        LOG_WARN("replay: %s", R->error);
        return;
    }
    R->len = 0;
    R->full = false;
    R->frame = R->slice = 0;
    R->held_known = R->set_known = R->board_known = false;
    R->tail = 0;
    replay__meta_begin(&R->meta);
    uint8_t h[FOLLOW_HDR_BYTES] = {0};
    memcpy(h, FOLLOW_MAGIC, 8);
    follow_put32(h + 8,  FOLLOW_VERSION);
    follow_put32(h + 24, (uint32_t)g_emu_steps_per_slice);
    R->cap = 1u << 20;
    R->feed = (uint8_t *)malloc(R->cap);
    if (!R->feed) { R->cap = 0; return; }
    memcpy(R->feed, h, sizeof h);
    R->len = sizeof h;
    R->on  = true;
    LOG_INFO("replay: recording %s vs %s", R->meta.p1, R->meta.p2);
}

/* End the replay: keep it (written beside the board) unless it is a fragment. */
static inline void replay_rec_finish(const char *why) {
    replay_rec_t *R = &g_replay_rec;
    if (!R->on) return;
    R->on = false;
    replay__rec(FOLLOW_END, R->slice, why, (uint32_t)strlen(why));
    R->meta.frames = R->frame;
    snprintf(R->meta.why, sizeof R->meta.why, "%s", why);
    if (R->meta.winner < 0 && R->frame < REPLAY_MIN_FRAMES) {
        LOG_INFO("replay: dropped (%s after %u frames, no result)", why, R->frame);
        free(R->feed);
        R->feed = NULL;
        R->len = R->cap = 0;
        return;
    }
    replay__wait();
    replay_job_t *j = &R->job;
    j->feed = R->feed;  j->feed_len = R->len;
    j->snap = R->snap;  j->snap_len = R->snap_len;
    j->meta = R->meta;
    if (!replay_dir(j->dir, sizeof j->dir)) j->dir[0] = '\0';
    R->feed = NULL;  R->len = R->cap = 0;
    R->snap = NULL;  R->snap_cap = R->snap_len = 0;   /* the next one measures again */
    replay__write_start();
}

/* Something the feed cannot carry changed the board (follow_lead_break). */
static inline void replay_rec_break(const char *why) {
    g_replay_rec.due = false;
    replay_rec_finish(why);
}

/* An MCP write_memory, as it lands (follow_lead_write). */
static inline void replay_rec_write(uint32_t addr, bool rom, const uint8_t *data, uint32_t n) {
    replay_rec_t *R = &g_replay_rec;
    if (!R->on || !n) return;
    uint8_t *p = (uint8_t *)malloc(5u + n);
    if (!p) { replay_rec_finish("out of memory"); return; }
    follow_put32(p, addr);
    p[4] = rom ? 1 : 0;
    memcpy(p + 5, data, n);
    replay__rec(FOLLOW_WRITE, R->slice, p, 5u + n);
    free(p);
}

/* A session's barrier reset has just run (emu_netplay_pump, mutex held). */
static inline void replay_session_reset(emu_thread_ctx_t *ctx) {
    replay_rec_t *R = &g_replay_rec;
    if (!R->want || g_netplay.ps3) return;
    replay__begin(ctx);
}

/* Changes since the last slice, then the input word latched for this one. */
static inline void replay__rec_state(void) {
    replay_rec_t *R = &g_replay_rec;
    int32_t set[FOLLOW_SET_WORDS];
    follow_settings(set);
    if (!R->set_known || memcmp(set, R->set, sizeof set)) {
        uint8_t p[FOLLOW_SET_WORDS * 4];
        for (int i = 0; i < FOLLOW_SET_WORDS; i++) follow_put32(p + i * 4, (uint32_t)set[i]);
        replay__rec(FOLLOW_SET, R->slice, p, sizeof p);
        memcpy(R->set, set, sizeof set);
        R->set_known = true;
    }
    uint32_t board = follow_board_live();
    if (!R->board_known || board != R->board) {
        uint8_t p[4];
        follow_put32(p, board);
        replay__rec(FOLLOW_BOARD, R->slice, p, 4);
        R->board = board;
        R->board_known = true;
    }
    /* As the leader does: one word for the whole slice, even once the session
     * has let go of the board and the host's keys drive it. */
    uint32_t held = g_input.use_net ? g_input.net_held : g_input.held;
    R->use_net_was   = g_input.use_net;
    R->latched       = true;
    g_input.net_held = held;
    g_input.use_net  = 1;
    if (!R->held_known || held != R->held) {
        uint8_t p[4];
        follow_put32(p, held);
        replay__rec(FOLLOW_INPUT, R->slice, p, 4);
        R->held = held;
        R->held_known = true;
    }
}

static inline void replay_slice_begin(emu_thread_ctx_t *ctx) {
    replay_rec_t *R = &g_replay_rec;
    if (R->due && !R->on) {
        if (R->want && netplay_active()) replay__begin(ctx);
        else R->due = false;
    }
    if (!R->on) return;
    /* The session went before a result: a peer left, a desync, a stall. */
    if (!R->tail && !netplay_active()) { replay_rec_finish("the session ended"); return; }
    if (R->full) { replay_rec_finish("too long"); return; }
    replay__rec_state();
}

static inline void replay_slice_end(emu_thread_ctx_t *ctx, bool frame_end) {
    replay_rec_t *R = &g_replay_rec;
    if (R->latched) {
        g_input.use_net = R->use_net_was;
        R->latched = false;
    }
    if (!R->on) return;
    if (!frame_end) { R->slice++; return; }
    uint8_t p[12];
    uint32_t n;
    follow__check_payload(ctx, R->frame, R->slice + 1, p, &n);
    replay__rec(FOLLOW_CHECK, FOLLOW_CHECK_SLICE, p, n);
    R->frame++;
    R->slice = 0;
    if (!R->tail) { replay__sample(ctx, &R->meta); return; }
    if (--R->tail) return;
    replay_rec_finish("result");
    R->due = netplay_active();   /* a VS session plays on: the next match is a new replay */
}

/* The frame's versus verdict (emu_slice_count_frame): 1 = 1P won, 2 = 2P. */
static inline void replay_frame_result(emu_thread_ctx_t *ctx, int versus_result) {
    replay_rec_t *R = &g_replay_rec;
    if (!R->on || R->tail || (versus_result != 1 && versus_result != 2)) return;
    replay__sample(ctx, &R->meta);
    replay__names(&R->meta);
    R->meta.winner       = versus_result - 1;
    R->meta.result_frame = R->frame ? R->frame - 1 : 0;
    R->tail = REPLAY_TAIL_FRAMES;
}

/* The process is going: keep what is being recorded, and wait for the writer. */
static inline void replay_rec_shutdown(void) {
    replay_rec_finish("the emulator closed");
    replay__wait();
}

/* ---- A replay's label, read back ------------------------------------------- */

typedef struct {
    char     p1[32], p2[32], c1[32], c2[32], stage[32], romset[32], profile[32];
    char     winner_name[32], loser_name[32], recorded_by[32], ended[64];
    int      winner;            /* 0 = 1P, 1 = 2P, -1 none */
    uint32_t r1, r2, frames, seconds, timestamp, finished;
} replay_label_t;

static inline void replay_label_get(const char *json, replay_label_t *l) {
    memset(l, 0, sizeof *l);
    l->winner = -1;
    json_get_str(json, "p1", l->p1, sizeof l->p1);
    json_get_str(json, "p2", l->p2, sizeof l->p2);
    json_get_str(json, "p1_character", l->c1, sizeof l->c1);
    json_get_str(json, "p2_character", l->c2, sizeof l->c2);
    json_get_str(json, "stage", l->stage, sizeof l->stage);
    json_get_str(json, "romset", l->romset, sizeof l->romset);
    json_get_str(json, "profile", l->profile, sizeof l->profile);
    json_get_str(json, "winner_name", l->winner_name, sizeof l->winner_name);
    json_get_str(json, "loser_name", l->loser_name, sizeof l->loser_name);
    json_get_str(json, "recorded_by", l->recorded_by, sizeof l->recorded_by);
    json_get_str(json, "ended", l->ended, sizeof l->ended);
    json_get_int(json, "winner", &l->winner);
    json_get_u32(json, "p1_rounds", &l->r1);
    json_get_u32(json, "p2_rounds", &l->r2);
    json_get_u32(json, "frames", &l->frames);
    json_get_u32(json, "seconds", &l->seconds);
    json_get_u32(json, "timestamp", &l->timestamp);
    json_get_u32(json, "finished", &l->finished);
}

/* ---- Playback -------------------------------------------------------------- */

typedef struct {
    bool     loaded;            /* a replay is open (replay_open) */
    bool     on;                /* the board is following it */
    uint8_t *state, *feed;
    size_t   state_len, feed_len;
    char     json[REPLAY_JSON_MAX];
    char     unplayable[160];   /* "" when it can play on this ROM set */
    uint32_t frames;
    bool     fast;
    uint32_t seek;              /* run unpaced until the board reaches this frame */
    const game_profile_t *old_profile;
    int      old_sps;
    int32_t  old_set[FOLLOW_SET_WORDS];
    char     error[160];
} replay_play_t;

static replay_play_t g_replay_play;

static inline bool replay_playing(void) { return g_replay_play.on; }

/* Run without pacing: fast-forward, or a seek not yet reached. */
static inline bool replay_play_unpaced(void) {
    const replay_play_t *P = &g_replay_play;
    return P->on && !g_follow.ended && !g_follow.split && (P->fast || g_follow.frame < P->seek);
}

static inline void replay__close(void) {
    replay_play_t *P = &g_replay_play;
    free(P->state);
    free(P->feed);
    P->state = P->feed = NULL;
    P->state_len = P->feed_len = 0;
    P->loaded = false;
}

static inline void *replay__extract(mz_zip_archive *z, const char *name, size_t *n) {
    int i = mz_zip_reader_locate_file(z, name, NULL, 0);
    *n = 0;
    return i < 0 ? NULL : mz_zip_reader_extract_to_heap(z, (mz_uint)i, n, 0);
}

/* Can this replay play here? "" if so, else why not, into P->unplayable. */
static inline void replay__check_playable(void) {
    replay_play_t *P = &g_replay_play;
    char romset[32] = "", profile[32] = "";
    json_get_str(P->json, "romset", romset, sizeof romset);
    json_get_str(P->json, "profile", profile, sizeof profile);
    const char *here = g_active_profile ? profile_rom_set(g_active_profile) : NULL;
    P->unplayable[0] = '\0';
    if (!here)
        snprintf(P->unplayable, sizeof P->unplayable, "load %s first", romset[0] ? romset : "the game");
    else if (strcmp(romset, here))
        snprintf(P->unplayable, sizeof P->unplayable, "it is a %s replay; %s is loaded", romset, here);
    else if (!profile_by_id(profile))
        snprintf(P->unplayable, sizeof P->unplayable, "this build has no %s profile", profile);
}

/* Open a replay file's bytes (copied). NULL, or why not; the label is then in
 * g_replay_play.json either way it can be read. */
static inline const char *replay_open(const void *data, size_t n) {
    replay_play_t *P = &g_replay_play;
    if (P->on) return "a replay is playing; stop it first";
    replay__close();
    P->json[0] = '\0';
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_mem(&z, data, n, 0)) return "not a replay (not a zip)";
    size_t jn = 0;
    char *json = (char *)replay__extract(&z, "replay.json", &jn);
    P->state = (uint8_t *)replay__extract(&z, "state.sta", &P->state_len);
    P->feed  = (uint8_t *)replay__extract(&z, "inputs.feed", &P->feed_len);
    mz_zip_reader_end(&z);
    if (json) {
        size_t k = jn < sizeof P->json - 1 ? jn : sizeof P->json - 1;
        memcpy(P->json, json, k);
        P->json[k] = '\0';
        /* One line, so it can go into a line protocol's reply as it is. */
        for (size_t i = 0; i < k; i++)
            if ((unsigned char)P->json[i] < 0x20) P->json[i] = ' ';
        mz_free(json);
    }
    uint32_t format = 0;
    if (!json || !P->state || !P->feed) { replay__close(); return "not a replay (a part is missing)"; }
    if (!json_get_u32(P->json, "format", &format) || format > REPLAY_FORMAT) {
        replay__close();
        return "a replay from a newer build";
    }
    json_get_u32(P->json, "frames", &P->frames);
    P->loaded = true;
    replay__check_playable();
    return NULL;
}

/* Open a replay file from disk. */
static inline const char *replay_open_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return "cannot open the file";
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 ? (uint8_t *)malloc((size_t)n) : NULL;
    bool ok = b && fread(b, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    const char *err = ok ? replay_open(b, (size_t)n) : "cannot read the file";
    free(b);
    return err;
}

/* Join the replay at its first frame. */
static inline const char *replay__join(emu_thread_ctx_t *ctx) {
    replay_play_t *P = &g_replay_play;
    const char *err = follow_join(ctx, P->state, P->state_len);
    if (!err) err = follow_feed(P->feed, P->feed_len);
    P->seek = 0;
    return err;
}

/* Put the board back as the player had it: profile, settings, battery, a cold
 * boot. Mutex held. `boot` false when a reset follows anyway (a session's). */
static inline void replay__restore(bool boot) {
    replay_play_t *P = &g_replay_play;
    follow_stop();
    g_active_profile      = P->old_profile;
    g_emu_steps_per_slice = P->old_sps;
    follow_settings_put(P->old_set);
    P->on = false;
    P->fast = false;
    backup_ram_reattach();
    if (boot && g_netplay.reset_board) g_netplay.reset_board(g_netplay.reset_ctx);
    input_reset();
}

/* Play the open replay from its start (mutex held). NULL, or why not. */
static inline const char *replay_play_start(emu_thread_ctx_t *ctx) {
    replay_play_t *P = &g_replay_play;
    if (!P->loaded) return "no replay is open";
    if (P->unplayable[0]) return P->unplayable;
    if (netplay_in_room() || netplay_active()) return "leave the netplay room first";
    if (g_sky_eye.phase != SKY_EYE_OFF) return "SKY EYE is holding the stage; leave it first";
    if (follow_following() && !P->on) return "the board is following another";
    char profile[32] = "";
    json_get_str(P->json, "profile", profile, sizeof profile);
    if (!P->on) {
        P->old_profile = g_active_profile;
        P->old_sps     = g_emu_steps_per_slice;
        follow_settings(P->old_set);
        backup_ram_detach();   /* the replay's battery is in its state */
        replay_rec_break("a replay is playing");
        follow_lead_break("a replay is playing");
    }
    g_active_profile = profile_by_id(profile);
    if (g_hle_extra_session_off) g_hle_extra_session_off();   /* --gems-*: not the board it was */
    if (g_netplay.reset_board) g_netplay.reset_board(g_netplay.reset_ctx);   /* the profile's hooks */
    input_reset();
    P->on = true;
    const char *err = replay__join(ctx);
    if (err) {
        snprintf(P->error, sizeof P->error, "%s", err);
        replay__restore(true);
        return P->error;
    }
    P->error[0] = '\0';
    return NULL;
}

/* Stop: the player's board back, from power-on (mutex held). */
static inline void replay_play_stop(void) {
    if (g_replay_play.on) replay__restore(true);
}

/* A session is about to reset the board (mutex held): leave the replay
 * without a boot of our own. */
static inline void replay_play_yield(void) {
    if (g_replay_play.on) replay__restore(false);
}

/* A game was loaded over the board (mutex held): a recording ends, and a
 * replay lets go -- the board is the new game's now, not the player's old one. */
static inline void replay_game_loaded(void) {
    replay_play_t *P = &g_replay_play;
    replay_rec_break("game loaded");
    if (!P->on) return;
    follow_stop();
    g_emu_steps_per_slice = P->old_sps;
    follow_settings_put(P->old_set);
    P->on = P->fast = false;
    if (P->loaded) replay__check_playable();
}

#ifndef __EMSCRIPTEN__
/* The record switch, kept in <user dir>/replay.cfg ("record=1"). */
static inline bool replay__cfg_path(char *out, size_t cap) {
    if (!backup_ram_user_dir(out, cap)) return false;
    size_t n = strlen(out);
    snprintf(out + n, cap - n, BACKUP_RAM_SEP "replay.cfg");
    return true;
}

static inline void replay_settings_load(void) {
    char path[512], line[64];
    if (!replay__cfg_path(path, sizeof path)) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "record=", 7)) g_replay_rec.want = atoi(line + 7) != 0;
    fclose(f);
}

static inline void replay_settings_save(void) {
    char path[512];
    if (!replay__cfg_path(path, sizeof path)) return;
    char dir[512];
    backup_ram_user_dir(dir, sizeof dir);
    backup_ram_mkdir(dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "record=%d\n", g_replay_rec.want ? 1 : 0);
    fclose(f);
}
#endif

/* Go to `frame`: from the start again if it is behind, then unpaced. */
static inline const char *replay_play_seek(emu_thread_ctx_t *ctx, uint32_t frame) {
    replay_play_t *P = &g_replay_play;
    if (!P->on) return "no replay is playing";
    if (frame < g_follow.frame || g_follow.split || g_follow.ended) {
        const char *err = replay__join(ctx);
        if (err) return err;
    }
    P->seek = frame;
    return NULL;
}

/* {"on","frame","frames","ended","split","why","fast","seeking"} */
static inline void replay_play_status(char *out, size_t cap) {
    const replay_play_t *P = &g_replay_play;
    char why[160];
    json_escape(why, sizeof why, g_follow.why);
    snprintf(out, cap,
             "{\"on\":%d,\"loaded\":%d,\"frame\":%u,\"frames\":%u,\"ended\":%d,\"split\":%d,"
             "\"why\":\"%s\",\"fast\":%d,\"seeking\":%d}",
             P->on ? 1 : 0, P->loaded ? 1 : 0, P->on ? g_follow.frame : 0, P->frames,
             P->on && g_follow.ended ? 1 : 0, P->on && g_follow.split ? 1 : 0, P->on ? why : "",
             P->fast ? 1 : 0, P->on && g_follow.frame < P->seek ? 1 : 0);
}

#endif /* REPLAY_H */
