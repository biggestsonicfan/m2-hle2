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
 * A GGPO session (core/emu_ggpo.h, Pinboard #584) is recorded the same way:
 * its cold boot starts one (replay_ggpo_begin), its frames are board frames on
 * both players' words. GGPO runs a frame on a guess of the peer's input and
 * runs it again when the guess was wrong, so the recorder goes back with the
 * board: each frame GGPO saves keeps where the feed stood (replay_ggpo_mark),
 * and a load cuts the feed back to it (replay_ggpo_rewind). What is written is
 * then the frames as they were settled. Only the last few frames of a replay's
 * tail, after the result, can still have been a guess when it is written; the
 * file plays them as they ran, frame checks and all.
 *
 * Each round the game plays is also kept in the game's own replay formats
 * (Pinboard #602), as the profile takes them (game_profile_t.native_take):
 *   native/round-NN/<part>   one file per part, byte for byte as the game
 *                            keeps it at its address
 *   native/round-NN/round.json   what the round was, and each part's address
 * replay.json says how many rounds there are ("native_rounds"). An open
 * replay's round goes back into the board with replay_native_install.
 *
 * Two ways to save (g_replay_rec.format, Pinboard #617; NATIVE-REPLAY.md):
 *   REPLAY_SAVE_NATIVE   the default: replay.json and the native rounds only.
 *                        No snapshot is taken, and a match none of whose
 *                        rounds the game kept is not saved.
 *   REPLAY_SAVE_INPUTS   the above plus state.sta and inputs.feed, #572's
 *                        frame-exact record of the whole match.
 * A game with no format of its own (no native_take, Pinboard #877) is always
 * saved the second way (replay__saves_inputs).
 * A local game is recorded too, when the player asks for replays: from the
 * moment the profile says a game is on (game_profile_t.in_play) to its return
 * to attract, every match and the CPU's rounds included. It ends no sooner
 * for a result, so one file holds the whole session, unless the player
 * finishes it first (replay_rec_finish_local, Pinboard #861): a game that
 * never goes back to attract (free play, continues) never ends by itself.
 *
 * The zip is made beside the board (a writer thread; the web build has none and
 * makes it at once) and saved as <p1>-vs-<p2>-<local date>-<time>.m2replay
 * (<romset>-<date>-<time> for a one-player game), in
 * the replay folder (native) or handed to the page (web_replay_take).
 *
 * Playback (g_replay_play) has two views. The default is the game's own: the
 * keyed rounds go to the profile's native_play, the board boots, attract is
 * sent to its replay fight (g_match_replay) and the game's key player plays
 * each round in turn (sfight_native.h). The other, and the only one for a
 * replay with no keyed round, joins follow.h as a follower on the replay's
 * state and feed. Either way the replay's profile is installed for it, the
 * battery detached, and everything is put back when it stops. Seeking back is
 * a restart and a run to the frame unpaced; forward is the run alone.
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
#define REPLAY_NATIVE_MAX  32      /* rounds a replay keeps in the game's formats */
#define REPLAY_NATIVE_JSON 1024

#define REPLAY_SAVE_NATIVE 0       /* g_replay_rec.format: the game's formats only */
#define REPLAY_SAVE_INPUTS 1       /* ... and the snapshot and input feed */
#define REPLAY_VIEW_AUTO   0       /* g_replay_play.view: the game's own if it can */
#define REPLAY_VIEW_NATIVE 1
#define REPLAY_VIEW_INPUTS 2

/* A round in the game's own formats, owned (the profile's buffers are not). */
typedef struct {
    int      nparts;
    char     name[GAME_NATIVE_PARTS_MAX][32];
    uint32_t addr[GAME_NATIVE_PARTS_MAX];
    uint8_t *data[GAME_NATIVE_PARTS_MAX];
    uint32_t len[GAME_NATIVE_PARTS_MAX];
    char     json[REPLAY_NATIVE_JSON];
} replay_native_t;

typedef struct {
    replay_native_t *round;
    int              n, cap;
} replay_natives_t;

static inline void replay__native_free(replay_native_t *r) {
    for (int i = 0; i < r->nparts; i++) free(r->data[i]);
    r->nparts = 0;
}

/* Drop rounds from `keep` on. */
static inline void replay__natives_cut(replay_natives_t *v, int keep) {
    while (v->n > keep) replay__native_free(&v->round[--v->n]);
}

static inline void replay__natives_free(replay_natives_t *v) {
    replay__natives_cut(v, 0);
    free(v->round);
    v->round = NULL;
    v->cap = 0;
}

/* A new empty round at the end, or NULL. */
static inline replay_native_t *replay__natives_add(replay_natives_t *v) {
    if (v->n >= REPLAY_NATIVE_MAX) return NULL;
    if (v->n == v->cap) {
        int cap = v->cap ? v->cap * 2 : 4;
        replay_native_t *b = (replay_native_t *)realloc(v->round, (size_t)cap * sizeof *b);
        if (!b) return NULL;
        v->round = b;
        v->cap   = cap;
    }
    replay_native_t *r = &v->round[v->n++];
    memset(r, 0, sizeof *r);
    return r;
}

/* The round as the profile handed it, copied. */
static inline bool replay__native_copy(replay_native_t *r, const game_native_round_t *in) {
    snprintf(r->json, sizeof r->json, "%s", in->json);
    for (int i = 0; i < in->nparts && i < GAME_NATIVE_PARTS_MAX; i++) {
        const game_native_part_t *p = &in->part[i];
        r->data[i] = (uint8_t *)malloc(p->len ? p->len : 1);
        if (!r->data[i]) return false;
        memcpy(r->data[i], p->data, p->len);
        snprintf(r->name[i], sizeof r->name[i], "%s", p->name);
        r->addr[i] = p->addr;
        r->len[i]  = p->len;
        r->nparts  = i + 1;
    }
    return true;
}

typedef struct {
    char     p1[REPLAY_NAME_MAX], p2[REPLAY_NAME_MAX], recorded_by[REPLAY_NAME_MAX];
    char     server[128], profile[32], romset[32], why[48];
    const char *netcode;        /* "rpcn", "ggpo" or "local" */
    bool     inputs;            /* saved with state.sta and inputs.feed */
    int      side;              /* the recorder's: 0 1P, 1 2P, 2 watching */
    uint32_t session, match;
    int64_t  when;              /* time(NULL) at the match's first frame */
    game_match_info_t fight;    /* as last seen while fighting */
    bool     fight_seen;
    bool     manual;            /* the player finished it (replay_rec_finish_local) */
    int      winner;            /* 0 1P, 1 2P, -1 none */
    uint32_t result_frame, frames;
    int32_t  settings[FOLLOW_SET_WORDS];
} replay_meta_t;

/* A finished replay on its way out. */
typedef struct {
    uint8_t      *snap, *feed;
    size_t        snap_len, feed_len;
    replay_meta_t meta;
    replay_natives_t native;
    char          dir[512];
} replay_job_t;

typedef struct {
    volatile int want;          /* the player's switch */
    volatile int format;        /* REPLAY_SAVE_* */
    bool     local;             /* a local game, not an online match */
    bool     held_off;          /* the player finished this local game's replay: none until it ends */
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
    replay_natives_t native;    /* the match's rounds in the game's formats */
    uint8_t *snap;
    size_t   snap_cap, snap_len;
    replay_job_t job;
    bool     writing;
    uint32_t id;                /* bumped by each replay__begin */
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

typedef struct {
    bool     loaded;            /* a replay is open (replay_open) */
    bool     on;                /* the board is playing it */
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
    int      old_match_replay, old_match_stage;
    replay_natives_t native;    /* its rounds in the game's formats */
    int      view;              /* REPLAY_VIEW_*: what the next start plays */
    bool     native_on;         /* playing through the game's own replay code */
    const game_profile_t *native_prof;
    game_native_round_t nround[REPLAY_NATIVE_MAX];   /* the keyed rounds */
    int      nrounds;
    uint32_t native_frames;     /* their key frames, as g_native_play counts */
    char     error[160];
} replay_play_t;

static replay_play_t g_replay_play;

static inline bool replay_recording(void) { return g_replay_rec.on; }

/* Where the recorder stood when GGPO saved a frame. */
#define REPLAY_MARKS 32         /* GGPO keeps about ten frames */
typedef struct {
    int      at;                /* GGPO's frame number, -1 none */
    uint32_t id;                /* the replay it belongs to, 0 none */
    size_t   len;
    bool     full, held_known, set_known, board_known;
    uint32_t frame, slice, held, board, tail;
    int32_t  set[FOLLOW_SET_WORDS];
    int      native_n;
    replay_meta_t meta;
} replay_mark_t;

/* A GGPO session, as its start (emu_ggpo_start) names it. */
static struct {
    bool          on;
    char          p1[REPLAY_NAME_MAX], p2[REPLAY_NAME_MAX], me[REPLAY_NAME_MAX];
    char          server[128];
    int           side;         /* ours: 0 1P, 1 2P */
    uint32_t      matches;      /* replays begun in this session */
    replay_mark_t marks[REPLAY_MARKS];
} g_replay_ggpo;

/* An online session whose matches are recorded: RPCN's lockstep or GGPO. */
static inline bool replay__session_live(void) { return netplay_active() || g_replay_ggpo.on; }

/* ---- The label ------------------------------------------------------------ */

static inline void replay__copy_name(char *out, const char *in) {
    snprintf(out, REPLAY_NAME_MAX, "%.*s", REPLAY_NAME_MAX - 1, in && in[0] ? in : "?");
}

/* The two fighters and the recorder, from the room (or GGPO's lobby). */
static inline void replay__names(replay_meta_t *m) {
    if (g_replay_ggpo.on) {
        replay__copy_name(m->p1, g_replay_ggpo.p1);
        replay__copy_name(m->p2, g_replay_ggpo.p2);
        replay__copy_name(m->recorded_by, g_replay_ggpo.me);
        return;
    }
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

/* Are the state and the feed saved: asked for, or the game keeps no replay of
 * its own to save instead? */
static inline bool replay__saves_inputs(void) {
    return g_replay_rec.format == REPLAY_SAVE_INPUTS || !g_active_profile || !g_active_profile->native_take;
}

static inline void replay__meta_begin(replay_meta_t *m, bool local) {
    memset(m, 0, sizeof *m);
    replay__copy_name(m->p1, "1P");
    replay__copy_name(m->p2, "2P");
    if (!local) replay__names(m);
    snprintf(m->server, sizeof m->server, "%s", g_replay_ggpo.on ? g_replay_ggpo.server : g_netplay.cfg.server);
    m->netcode = g_replay_ggpo.on ? "ggpo" : "rpcn";
    if (g_active_profile) {
        snprintf(m->profile, sizeof m->profile, "%s", g_active_profile->id);
        snprintf(m->romset, sizeof m->romset, "%s", profile_rom_set(g_active_profile));
    }
    int lp = g_netplay.local_player;
    m->side    = lp == 0 || lp == 1 ? lp : 2;
    m->session = g_netplay.generation;
    m->match   = g_netplay.match_started;
    if (g_replay_ggpo.on) {
        m->side    = g_replay_ggpo.side;
        m->session = 0;
        m->match   = g_replay_ggpo.matches++;
    }
    if (local) {
        m->server[0] = '\0';
        m->netcode   = "local";
        m->side = 0;
        m->session = m->match = 0;
    }
    m->inputs  = replay__saves_inputs();
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

/* A one-player game's replay (Sega Rally's race): side 0 only. */
static inline bool replay__solo(const replay_meta_t *m) {
    return m->fight_seen && m->fight.players == 1;
}

/* <p1>-vs-<p2>-YYYYMMDD-HHMMSS.m2replay, in the recorder's local time;
 * <romset>-YYYYMMDD-HHMMSS.m2replay for a one-player game. */
static inline void replay_file_name(const replay_meta_t *m, char *out, size_t cap) {
    char a[REPLAY_NAME_MAX], b[REPLAY_NAME_MAX], when[32] = "00000000-000000";
    replay__file_part(a, sizeof a, m->p1);
    replay__file_part(b, sizeof b, m->p2);
    time_t t = (time_t)m->when;
    struct tm *tm = localtime(&t);
    if (tm) strftime(when, sizeof when, "%Y%m%d-%H%M%S", tm);
    if (!replay__solo(m)) { snprintf(out, cap, "%s-vs-%s-%s%s", a, b, when, REPLAY_EXT); return; }
    replay__file_part(a, sizeof a, m->romset[0] ? m->romset : m->profile);
    snprintf(out, cap, "%s-%s%s", a, when, REPLAY_EXT);
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

/* Each side's character, rounds won and whether the CPU played it; the CPU's
 * difficulty. A one-player game's is its car as p1_character, with no rounds. */
static inline void replay__json_sides(const game_match_info_t *f, bool solo, char *out, size_t cap, size_t *n) {
    char unknown[24];
    for (int side = 0; side < (solo ? 1 : 2); side++) {
        const char *key = side ? "p2_character" : "p1_character";
        snprintf(unknown, sizeof unknown, "#%d", f->chara[side]);
        replay__json_str(out, cap, n, key, f->chara_name[side] ? f->chara_name[side] : unknown);
        replay__json_int(out, cap, n, side ? "p2_character_id" : "p1_character_id", f->chara[side]);
        if (!solo) replay__json_int(out, cap, n, side ? "p2_rounds" : "p1_rounds", f->rounds[side]);
        replay__json_int(out, cap, n, side ? "p2_cpu" : "p1_cpu", f->cpu[side]);
    }
    if (f->difficulty_name) {
        replay__json_str(out, cap, n, "difficulty", f->difficulty_name);
        replay__json_int(out, cap, n, "difficulty_id", f->difficulty);
    }
}

/* The fight's half of replay.json: characters, stage, rounds, result. A
 * one-player game's says "players":1, its course as the stage, and has no
 * rounds and no winner. */
static inline void replay__json_fight(const replay_meta_t *m, char *out, size_t cap, size_t *n) {
    const game_match_info_t *f = &m->fight;
    char unknown[24];
    bool solo = replay__solo(m);
    replay__json_int(out, cap, n, "players", solo ? 1 : 2);
    if (m->fight_seen) {
        replay__json_sides(f, solo, out, cap, n);
        snprintf(unknown, sizeof unknown, "#%d", f->stage);
        replay__json_str(out, cap, n, "stage", f->stage_name ? f->stage_name : unknown);
        replay__json_int(out, cap, n, "stage_id", f->stage);
        if (!solo) replay__json_int(out, cap, n, "rounds_to_win", f->rounds_to_win);
    }
    replay__json_int(out, cap, n, "winner", m->winner);
    if (m->winner == 0 || m->winner == 1) {
        replay__json_str(out, cap, n, "winner_name", m->winner ? m->p2 : m->p1);
        replay__json_str(out, cap, n, "loser_name",  m->winner ? m->p1 : m->p2);
        replay__json_int(out, cap, n, "result_frame", m->result_frame);
    }
}

/* replay.json: one flat object, so json_min.h reads every field back. */
static inline size_t replay__json(const replay_meta_t *m, int native_n, char *out, size_t cap) {
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
    replay__json_str(out, cap, &n, "netcode", m->netcode);
    replay__json_str(out, cap, &n, "content", m->inputs ? "inputs" : "native");
    replay__json_int(out, cap, &n, "session", m->session);
    replay__json_int(out, cap, &n, "match", m->match);
    replay__json_str(out, cap, &n, "recorded_by", m->recorded_by);
    replay__json_int(out, cap, &n, "side", m->side);
    replay__json_str(out, cap, &n, "p1", m->p1);
    replay__json_str(out, cap, &n, "p2", m->p2);
    replay__json_fight(m, out, cap, &n);
    replay__json_int(out, cap, &n, "finished", m->winner >= 0);
    replay__json_str(out, cap, &n, "ended", m->why);
    replay__json_str(out, cap, &n, "stopped", m->manual ? "manual" : "automatic");
    replay__json_int(out, cap, &n, "frames", m->frames);
    replay__json_int(out, cap, &n, "seconds", (m->frames + 30) / 60);
    replay__json_int(out, cap, &n, "native_rounds", native_n);
    for (int i = 0; i < FOLLOW_SET_WORDS; i++) {
        char key[16];
        snprintf(key, sizeof key, "set%d", i);
        replay__json_int(out, cap, &n, key, m->settings[i]);
    }
    if (n < cap) n += (size_t)snprintf(out + n, cap - n, "}\n");
    return n < cap ? n : cap - 1;
}

/* ---- The zip ---------------------------------------------------------------- */

/* native/round-NN/round.json: the profile's summary, with where each part goes. */
static inline size_t replay__native_json(const replay_native_t *r, char *out, size_t cap) {
    size_t n = strlen(r->json);
    if (n < 2 || r->json[n - 1] != '}') n = (size_t)snprintf(out, cap, "{\"parts\":[");
    else n = (size_t)snprintf(out, cap, "%.*s,\"parts\":[", (int)(n - 1), r->json);
    for (int i = 0; i < r->nparts && n < cap; i++)
        n += (size_t)snprintf(out + n, cap - n, "%s{\"file\":\"%s\",\"addr\":%u,\"len\":%u}",
                              i ? "," : "", r->name[i], r->addr[i], r->len[i]);
    if (n < cap) n += (size_t)snprintf(out + n, cap - n, "]}\n");
    return n < cap ? n : cap - 1;
}

static inline bool replay__zip_native(mz_zip_archive *z, const replay_natives_t *v) {
    char path[96], json[REPLAY_NATIVE_JSON + 1024];
    for (int k = 0; k < v->n; k++) {
        const replay_native_t *r = &v->round[k];
        snprintf(path, sizeof path, "native/round-%02d/round.json", k + 1);
        size_t jn = replay__native_json(r, json, sizeof json);
        if (!mz_zip_writer_add_mem(z, path, json, jn, MZ_DEFAULT_LEVEL)) return false;
        for (int i = 0; i < r->nparts; i++) {
            snprintf(path, sizeof path, "native/round-%02d/%s", k + 1, r->name[i]);
            if (!mz_zip_writer_add_mem(z, path, r->data[i], r->len[i], MZ_DEFAULT_LEVEL)) return false;
        }
    }
    return true;
}

/* The replay as a zip on the heap (mz_free it), or NULL. */
static inline void *replay__zip(const replay_job_t *j, size_t *out_len) {
    char json[REPLAY_JSON_MAX];
    size_t jn = replay__json(&j->meta, j->native.n, json, sizeof json);
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    void *buf = NULL;
    *out_len = 0;
    if (!mz_zip_writer_init_heap(&z, 0, j->snap_len / 4 + j->feed_len + 65536)) return NULL;
    bool ok = mz_zip_writer_add_mem(&z, "replay.json", json, jn, MZ_DEFAULT_LEVEL)
           && (!j->meta.inputs
               || (mz_zip_writer_add_mem(&z, "state.sta", j->snap, j->snap_len, MZ_BEST_SPEED)
                   && mz_zip_writer_add_mem(&z, "inputs.feed", j->feed, j->feed_len, MZ_DEFAULT_LEVEL)))
           && replay__zip_native(&z, &j->native)
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
    replay__natives_free(&j->native);
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
    if (R->full || !R->meta.inputs) return;
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

static inline void replay_rec_finish(const char *why);

/* Start a replay here: the board's state and an empty feed, when they are
 * saved; a local game's (`local`) or an online match's. */
static inline void replay__begin(emu_thread_ctx_t *ctx, bool local) {
    replay_rec_t *R = &g_replay_rec;
    replay_rec_finish(R->local ? "an online match began" : "a new match began");
    R->due = false;
    bool inputs = replay__saves_inputs();
    const char *err = inputs ? replay__snap(ctx) : NULL;
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
    R->id++;
    R->local = local;
    R->held_off = false;
    replay__natives_cut(&R->native, 0);
    replay__meta_begin(&R->meta, local);
    if (!inputs) {
        R->on = true;
        LOG_INFO("replay: recording %s (the game's formats)", local ? "a local game" : "an online match");
        return;
    }
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

/* Would the replay being recorded be kept, were it to end now? */
static inline bool replay__keeps(void) {
    const replay_rec_t *R = &g_replay_rec;
    bool empty = !R->meta.inputs && R->native.n == 0;
    return !(R->meta.winner < 0 && R->frame < REPLAY_MIN_FRAMES) && !empty;
}

/* End the replay: keep it (written beside the board) unless it is a fragment. */
static inline void replay_rec_finish(const char *why) {
    replay_rec_t *R = &g_replay_rec;
    if (!R->on) return;
    R->on = false;
    replay__rec(FOLLOW_END, R->slice, why, (uint32_t)strlen(why));
    R->meta.frames = R->frame;
    snprintf(R->meta.why, sizeof R->meta.why, "%s", why);
    if (!replay__keeps()) {
        bool empty = !R->meta.inputs && R->native.n == 0;
        LOG_INFO("replay: dropped (%s after %u frames, %s)", why, R->frame,
                 empty ? "no round in the game's formats" : "no result");
        replay__natives_cut(&R->native, 0);
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
    j->native = R->native;
    memset(&R->native, 0, sizeof R->native);
    if (!replay_dir(j->dir, sizeof j->dir)) j->dir[0] = '\0';
    R->feed = NULL;  R->len = R->cap = 0;
    R->snap = NULL;  R->snap_cap = R->snap_len = 0;   /* the next one measures again */
    replay__write_start();
}

/* Is a local game being recorded, so the player can finish it now? */
static inline bool replay_rec_local(void) { return g_replay_rec.on && g_replay_rec.local; }

/* The player's "Finish replay" (Pinboard #861): save the local game recorded
 * so far, as its return to attract would. The game plays on unrecorded; the
 * next one it starts is recorded again. Says what became of it: NULL when
 * none was being recorded. Under REPLAY_MIN_FRAMES, or with no round kept in
 * the game's formats, it is dropped, as ever. */
static inline const char *replay_rec_finish_local(void) {
    replay_rec_t *R = &g_replay_rec;
    if (!replay_rec_local()) return NULL;
    const char *said = replay__keeps() ? "replay saved"
                     : !R->meta.inputs && R->native.n == 0 ? "not kept: no round has finished yet"
                     : "not kept: too short";
    R->meta.manual = true;
    replay_rec_finish("finished by the player");
    R->held_off = true;
    return said;
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
    replay__begin(ctx, false);
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

/* Is the player's own game on, the profile says (not attract or a replay)? */
static inline bool replay__local_game(emu_thread_ctx_t *ctx) {
    if (!g_active_profile || !g_active_profile->in_play || replay__session_live()) return false;
    if (g_replay_play.on || follow_following() || g_native_play.on) return false;
    return g_active_profile->in_play(ctx->bus);
}

/* A replay played in the game's own code reads no pad: attract would take a
 * START as a coin-up. The latch is the recorder's, which is off meanwhile. */
static inline void replay__native_latch(void) {
    replay_rec_t *R = &g_replay_rec;
    R->use_net_was   = g_input.use_net;
    R->latched       = true;
    g_input.net_held = 0;
    g_input.use_net  = 1;
}

/* A local game starts a replay when the player wants them, and its return
 * to attract ends it. */
static inline bool replay__local_slice(emu_thread_ctx_t *ctx) {
    replay_rec_t *R = &g_replay_rec;
    if (!R->on) {
        if (R->held_off) {   /* finished by the player: wait for this game to end */
            if (!replay__local_game(ctx)) R->held_off = false;
            return false;
        }
        if (R->want && !R->due && replay__local_game(ctx)) replay__begin(ctx, true);
        return R->on;
    }
    if (!R->local) return true;
    if (replay__session_live())         { replay_rec_finish("an online match began"); return false; }
    if (!replay__local_game(ctx))       { replay_rec_finish("game over"); return false; }
    return true;
}

static inline void replay_slice_begin(emu_thread_ctx_t *ctx) {
    replay_rec_t *R = &g_replay_rec;
    if (g_replay_play.on && g_replay_play.native_on) { replay__native_latch(); return; }
    if (R->due && !R->on) {
        if (R->want && replay__session_live()) replay__begin(ctx, false);
        else R->due = false;
    }
    if (!replay__local_slice(ctx)) return;
    /* The session went before a result: a peer left, a desync, a stall. */
    if (!R->local && !R->tail && !replay__session_live()) { replay_rec_finish("the session ended"); return; }
    if (R->full) { replay_rec_finish("too long"); return; }
    replay__rec_state();
}

/* A round the profile kept in the game's formats this slice: the replay takes
 * it. Taken every slice, recording or not, so a stale one never waits for the
 * next replay; the hook and the take fall in one slice, so GGPO's saves, which
 * are between frames, never split them. */
static inline void replay__take_native(void) {
    replay_rec_t *R = &g_replay_rec;
    if (!g_active_profile || !g_active_profile->native_take) return;
    game_native_round_t round;
    if (!g_active_profile->native_take(&round) || !R->on) return;
    replay_native_t *r = replay__natives_add(&R->native);
    if (r && !replay__native_copy(r, &round)) replay__natives_cut(&R->native, R->native.n - 1);
}

static inline void replay_slice_end(emu_thread_ctx_t *ctx, bool frame_end) {
    replay_rec_t *R = &g_replay_rec;
    if (R->latched) {
        g_input.use_net = R->use_net_was;
        R->latched = false;
    }
    replay__take_native();
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
    R->due = replay__session_live();   /* a VS session plays on: the next match is a new replay */
}

/* The frame's versus verdict (emu_slice_count_frame): 1 = 1P won, 2 = 2P. */
static inline void replay_frame_result(emu_thread_ctx_t *ctx, int versus_result) {
    replay_rec_t *R = &g_replay_rec;
    if (!R->on || R->local || R->tail || (versus_result != 1 && versus_result != 2)) return;
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

/* ---- GGPO ------------------------------------------------------------------ */

/* A GGPO session's cold boot has run (emu_ggpo_start, mutex held): its first
 * match is recorded from here, if the player wants it. */
static inline void replay_ggpo_begin(emu_thread_ctx_t *ctx, const char *p1, const char *p2,
                                     int side, const char *server) {
    replay_rec_t *R = &g_replay_rec;
    g_replay_ggpo.on = true;
    replay__copy_name(g_replay_ggpo.p1, p1);
    replay__copy_name(g_replay_ggpo.p2, p2);
    replay__copy_name(g_replay_ggpo.me, side ? g_replay_ggpo.p2 : g_replay_ggpo.p1);
    snprintf(g_replay_ggpo.server, sizeof g_replay_ggpo.server, "%.*s",
             (int)sizeof g_replay_ggpo.server - 1, server ? server : "");
    g_replay_ggpo.side    = side ? 1 : 0;
    g_replay_ggpo.matches = 0;
    for (int i = 0; i < REPLAY_MARKS; i++) g_replay_ggpo.marks[i].at = -1;
    R->due = false;
    if (R->want) replay__begin(ctx, false);
}

/* The session is over (emu_ggpo_stop, mutex held): keep what it has. */
static inline void replay_ggpo_end(void) {
    if (!g_replay_ggpo.on) return;
    g_replay_ggpo.on = false;
    g_replay_rec.due = false;
    replay_rec_finish("the session ended");
}

/* GGPO has saved frame `at`, between frames: where the feed stands. */
static inline void replay_ggpo_mark(int at) {
    const replay_rec_t *R = &g_replay_rec;
    if (!g_replay_ggpo.on || at < 0) return;
    replay_mark_t *m = &g_replay_ggpo.marks[at % REPLAY_MARKS];
    m->at = at;
    m->id = R->on ? R->id : 0;
    if (!R->on) return;
    m->len  = R->len;   m->full = R->full;
    m->frame = R->frame; m->slice = R->slice; m->tail = R->tail;
    m->held = R->held;  m->held_known = R->held_known;
    m->board = R->board; m->board_known = R->board_known;
    memcpy(m->set, R->set, sizeof m->set);
    m->set_known = R->set_known;
    m->native_n = R->native.n;
    m->meta = R->meta;
}

/* GGPO has loaded frame `at` and will run on from it: the feed goes back to
 * where it stood then. A replay begun after that frame (a VS rematch's) starts
 * again at the next slice, from the board as it now is. */
static inline void replay_ggpo_rewind(int at) {
    replay_rec_t *R = &g_replay_rec;
    if (!g_replay_ggpo.on || !R->on || at < 0) return;
    const replay_mark_t *m = &g_replay_ggpo.marks[at % REPLAY_MARKS];
    if (m->at == at && m->id == R->id) {
        R->len  = m->len;   R->full = m->full;
        R->frame = m->frame; R->slice = m->slice; R->tail = m->tail;
        R->held = m->held;  R->held_known = m->held_known;
        R->board = m->board; R->board_known = m->board_known;
        memcpy(R->set, m->set, sizeof R->set);
        R->set_known = m->set_known;
        replay__natives_cut(&R->native, m->native_n);
        R->meta = m->meta;
        return;
    }
    R->on = false;
    replay__natives_cut(&R->native, 0);
    free(R->feed);
    R->feed = NULL;
    R->len = R->cap = 0;
    R->due = true;
    g_replay_ggpo.matches--;
}

/* ---- A replay's label, read back ------------------------------------------- */

typedef struct {
    char     p1[32], p2[32], c1[32], c2[32], stage[32], romset[32], profile[32];
    char     winner_name[32], loser_name[32], recorded_by[32], ended[64];
    char     netcode[8];        /* "rpcn", "ggpo"; "" in a replay from before GGPO */
    char     difficulty[16];    /* the CPU's; "" before Pinboard #875 */
    char     stopped[12];       /* "manual" (Finish replay) or "automatic"; "" before #875 */
    int      winner;            /* 0 = 1P, 1 = 2P, -1 none */
    uint32_t r1, r2, frames, seconds, timestamp, finished, cpu1, cpu2;
    uint32_t players;           /* 1 a one-player game (Sega Rally), else 2 */
} replay_label_t;

static inline void replay_label_get(const char *json, replay_label_t *l) {
    memset(l, 0, sizeof *l);
    l->winner = -1;
    l->players = 2;
    json_get_u32(json, "players", &l->players);
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
    json_get_str(json, "netcode", l->netcode, sizeof l->netcode);
    json_get_str(json, "difficulty", l->difficulty, sizeof l->difficulty);
    json_get_str(json, "stopped", l->stopped, sizeof l->stopped);
    json_get_u32(json, "p1_cpu", &l->cpu1);
    json_get_u32(json, "p2_cpu", &l->cpu2);
    json_get_int(json, "winner", &l->winner);
    json_get_u32(json, "p1_rounds", &l->r1);
    json_get_u32(json, "p2_rounds", &l->r2);
    json_get_u32(json, "frames", &l->frames);
    json_get_u32(json, "seconds", &l->seconds);
    json_get_u32(json, "timestamp", &l->timestamp);
    json_get_u32(json, "finished", &l->finished);
}

/* ---- Playback -------------------------------------------------------------- */


static inline bool replay_playing(void) { return g_replay_play.on; }

/* Playing as a follower of the replay's feed (the run loop waits on it). */
static inline bool replay_play_follows(void) { return g_replay_play.on && !g_replay_play.native_on; }

/* Run without pacing: fast-forward, or a seek not yet reached. The game's own
 * view also runs unpaced through the boot to the first round (seek 1). */
static inline bool replay_play_unpaced(void) {
    const replay_play_t *P = &g_replay_play;
    if (!P->on) return false;
    if (P->native_on) return !g_native_play.ended && (P->fast || g_native_play.total < P->seek);
    return !g_follow.ended && !g_follow.split && (P->fast || g_follow.frame < P->seek);
}

static inline void replay__close(void) {
    replay_play_t *P = &g_replay_play;
    free(P->state);
    free(P->feed);
    P->state = P->feed = NULL;
    P->state_len = P->feed_len = 0;
    replay__natives_free(&P->native);
    memset(P->nround, 0, sizeof P->nround);
    P->nrounds = 0;
    P->native_frames = 0;
    P->loaded = false;
}

static inline void *replay__extract(mz_zip_archive *z, const char *name, size_t *n) {
    int i = mz_zip_reader_locate_file(z, name, NULL, 0);
    *n = 0;
    return i < 0 ? NULL : mz_zip_reader_extract_to_heap(z, (mz_uint)i, n, 0);
}

/* One part named in a round's round.json, read into the round. */
static inline void replay__open_part(mz_zip_archive *z, replay_native_t *r, int k, const char *part) {
    char file[32] = "", path[96];
    uint32_t addr = 0, len = 0;
    if (!json_get_str(part, "file", file, sizeof file) || !file[0] || strchr(file, '/')) return;
    json_get_u32(part, "addr", &addr);
    snprintf(path, sizeof path, "native/round-%02d/%s", k + 1, file);
    size_t n = 0;
    void *data = replay__extract(z, path, &n);
    if (!data) return;
    json_get_u32(part, "len", &len);
    if (n != len || r->nparts >= GAME_NATIVE_PARTS_MAX) { mz_free(data); return; }
    int i = r->nparts;
    r->data[i] = (uint8_t *)malloc(n ? n : 1);
    if (r->data[i]) memcpy(r->data[i], data, n);
    mz_free(data);
    if (!r->data[i]) return;
    snprintf(r->name[i], sizeof r->name[i], "%s", file);
    r->addr[i] = addr;
    r->len[i]  = (uint32_t)n;
    r->nparts  = i + 1;
}

/* The replay's rounds in the game's formats, as many as replay.json says. */
static inline void replay__open_native(mz_zip_archive *z) {
    replay_play_t *P = &g_replay_play;
    uint32_t rounds = 0;
    json_get_u32(P->json, "native_rounds", &rounds);
    for (uint32_t k = 0; k < rounds && k < REPLAY_NATIVE_MAX; k++) {
        char path[96];
        snprintf(path, sizeof path, "native/round-%02u/round.json", k + 1);
        size_t jn = 0;
        char *json = (char *)replay__extract(z, path, &jn);
        replay_native_t *r = json ? replay__natives_add(&P->native) : NULL;
        if (!r) { mz_free(json); break; }
        snprintf(r->json, sizeof r->json, "%.*s", (int)jn, json);
        /* "parts":[{...},{...}]: each object on its own */
        for (const char *p = strstr(json, "\"parts\""); p && (p = strchr(p, '{')) != NULL; ) {
            const char *e = strchr(p, '}');
            if (!e || (size_t)(e - json) >= jn) break;
            char part[256];
            snprintf(part, sizeof part, "%.*s", (int)(e - p + 1), p);
            replay__open_part(z, r, (int)k, part);
            p = e + 1;
        }
        mz_free(json);
    }
}

/* The rounds the game's own code can play: those it keyed ("keyed":1 in
 * round.json), as game_native_round_t over P->native's buffers. */
static inline void replay__native_rounds(void) {
    replay_play_t *P = &g_replay_play;
    P->nrounds = 0;
    P->native_frames = 0;
    for (int k = 0; k < P->native.n; k++) {
        const replay_native_t *r = &P->native.round[k];
        int keyed = 0;
        uint32_t frames = 0;
        if (!json_get_int(r->json, "keyed", &keyed) || !keyed) continue;
        json_get_u32(r->json, "key_frames", &frames);
        game_native_round_t *in = &P->nround[P->nrounds++];
        memset(in, 0, sizeof *in);
        for (int i = 0; i < r->nparts; i++) {
            in->part[i].name = r->name[i];
            in->part[i].addr = r->addr[i];
            in->part[i].data = r->data[i];
            in->part[i].len  = r->len[i];
        }
        in->nparts = r->nparts;
        snprintf(in->json, sizeof in->json, "%.*s", (int)sizeof in->json - 1, r->json);
        P->native_frames += frames ? frames - 1 : 0;
    }
}

/* Does the next start play in the game's own code? */
static inline bool replay__view_native(const game_profile_t *prof) {
    const replay_play_t *P = &g_replay_play;
    bool can = prof && prof->native_play && P->nrounds > 0;
    if (!P->feed) return can;
    return can && P->view != REPLAY_VIEW_INPUTS;
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
    else if (!P->feed && !replay__view_native(profile_by_id(profile)))
        snprintf(P->unplayable, sizeof P->unplayable, "no round of it is in a form the game can play");
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
    if (json) {
        size_t k = jn < sizeof P->json - 1 ? jn : sizeof P->json - 1;
        memcpy(P->json, json, k);
        P->json[k] = '\0';
        /* One line, so it can go into a line protocol's reply as it is. */
        for (size_t i = 0; i < k; i++)
            if ((unsigned char)P->json[i] < 0x20) P->json[i] = ' ';
        mz_free(json);
        replay__open_native(&z);
        replay__native_rounds();
    }
    mz_zip_reader_end(&z);
    uint32_t format = 0;
    if (!P->state != !P->feed) { free(P->state); free(P->feed); P->state = P->feed = NULL; }
    if (!json || (!P->feed && !P->native.n)) { replay__close(); return "not a replay (a part is missing)"; }
    if (!json_get_u32(P->json, "format", &format) || format > REPLAY_FORMAT) {
        replay__close();
        return "a replay from a newer build";
    }
    json_get_u32(P->json, "frames", &P->frames);
    if (!P->feed) P->frames = P->native_frames;
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

/* The game's own view stops: its rounds out of the profile, attract's jump as
 * the player had it. */
static inline void replay__native_end(void) {
    replay_play_t *P = &g_replay_play;
    if (P->native_on && P->native_prof && P->native_prof->native_play) P->native_prof->native_play(NULL, 0);
    P->native_on   = false;
    P->native_prof = NULL;
    g_match_replay       = P->old_match_replay;
    g_match_replay_stage = P->old_match_stage;
    g_replay_stage_pin   = -1;
}

/* The game's own view from its start: a cold boot, the rounds to the profile,
 * and attract sent straight to its replay fight. Mutex held. */
static inline const char *replay__native_begin(void) {
    replay_play_t *P = &g_replay_play;
    if (g_netplay.reset_board) g_netplay.reset_board(g_netplay.reset_ctx);
    input_reset();
    P->native_prof = g_active_profile;
    P->native_on   = true;
    const char *err = g_active_profile->native_play(P->nround, P->nrounds);
    if (err) return err;
    g_match_replay       = 1;
    g_match_replay_stage = -1;
    P->seek = 1;
    return NULL;
}

/* Put the board back as the player had it: profile, settings, battery, a cold
 * boot. Mutex held. `boot` false when a reset follows anyway (a session's). */
static inline void replay__restore(bool boot) {
    replay_play_t *P = &g_replay_play;
    replay__native_end();
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
    if (g_replay_ggpo.on) return "a GGPO match owns the board";
    if (g_sky_eye.phase != SKY_EYE_OFF) return "SKY EYE is holding the stage; leave it first";
    if (follow_following() && !P->on) return "the board is following another";
    char profile[32] = "";
    json_get_str(P->json, "profile", profile, sizeof profile);
    if (!P->on) {
        P->old_profile = g_active_profile;
        P->old_sps     = g_emu_steps_per_slice;
        P->old_match_replay = g_match_replay;
        P->old_match_stage  = g_match_replay_stage;
        follow_settings(P->old_set);
        backup_ram_detach();   /* the replay's battery is in its state */
        replay_rec_break("a replay is playing");
        follow_lead_break("a replay is playing");
    }
    replay__native_end();   /* a restart from either view */
    g_active_profile = profile_by_id(profile);
    if (g_hle_extra_session_off) g_hle_extra_session_off();   /* --gems-*: not the board it was */
    P->on = true;
    const char *err;
    if (replay__view_native(g_active_profile)) {
        follow_stop();
        err = replay__native_begin();
    } else {
        if (g_netplay.reset_board) g_netplay.reset_board(g_netplay.reset_ctx);   /* the profile's hooks */
        input_reset();
        err = replay__join(ctx);
    }
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
    replay__native_end();
    follow_stop();
    g_emu_steps_per_slice = P->old_sps;
    follow_settings_put(P->old_set);
    P->on = P->fast = false;
    if (P->loaded) replay__check_playable();
}

/* "native" / "inputs" to REPLAY_SAVE_* or REPLAY_VIEW_*; -1 when neither. */
static inline int replay_format_parse(const char *s, bool view) {
    if (!strcmp(s, "native")) return view ? REPLAY_VIEW_NATIVE : REPLAY_SAVE_NATIVE;
    if (!strcmp(s, "inputs")) return view ? REPLAY_VIEW_INPUTS : REPLAY_SAVE_INPUTS;
    if (view && !strcmp(s, "auto")) return REPLAY_VIEW_AUTO;
    return -1;
}

#ifndef __EMSCRIPTEN__
/* The record switch and the save format, kept in <user dir>/replay.cfg
 * ("record=1", "format=native|inputs"). */
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
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strncmp(line, "record=", 7)) g_replay_rec.want = atoi(line + 7) != 0;
        int fmt = !strncmp(line, "format=", 7) ? replay_format_parse(line + 7, false) : -1;
        if (fmt >= 0) g_replay_rec.format = fmt;
    }
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
    fprintf(f, "record=%d\nformat=%s\n", g_replay_rec.want ? 1 : 0,
            g_replay_rec.format == REPLAY_SAVE_INPUTS ? "inputs" : "native");
    fclose(f);
}
#endif

/* Put round `k` (1-based) of the open replay into the board, in the game's
 * own formats, where the game reads it (the profile's native_put): for STF the
 * next attract replay is that round, and the credits' slot for its stage holds
 * it. The player's board, not a replay's or a session's. Mutex held. */
static inline const char *replay_native_install(emu_thread_ctx_t *ctx, int k) {
    const replay_play_t *P = &g_replay_play;
    if (!P->loaded) return "no replay is open";
    if (P->unplayable[0]) return P->unplayable;
    if (k < 1 || k > P->native.n) return "the replay has no such round in the game's format";
    if (P->on) return "a replay is playing; stop it first";
    if (netplay_in_room() || netplay_active() || g_replay_ggpo.on) return "leave the online match first";
    if (follow_following()) return "the board is following another";
    if (!g_active_profile || !g_active_profile->native_put) return "this game takes no rounds back";
    const replay_native_t *r = &P->native.round[k - 1];
    game_native_round_t in;
    memset(&in, 0, sizeof in);
    for (int i = 0; i < r->nparts; i++) {
        in.part[i].name = r->name[i];
        in.part[i].addr = r->addr[i];
        in.part[i].data = r->data[i];
        in.part[i].len  = r->len[i];
    }
    in.nparts = r->nparts;
    const char *err = g_active_profile->native_put(ctx->bus, &in);
    if (!err) follow_lead_break("a replay round was put in");
    return err;
}

/* Go to `frame`: from the start again if it is behind, then unpaced. In the
 * game's own view a frame is one of its key player's, counted over every round. */
static inline const char *replay_play_seek(emu_thread_ctx_t *ctx, uint32_t frame) {
    replay_play_t *P = &g_replay_play;
    if (!P->on) return "no replay is playing";
    if (P->native_on) {
        if (frame < g_native_play.total || g_native_play.ended) {
            const char *err = replay__native_begin();
            if (err) return err;
        }
        P->seek = frame ? frame : 1;
        return NULL;
    }
    if (frame < g_follow.frame || g_follow.split || g_follow.ended) {
        const char *err = replay__join(ctx);
        if (err) return err;
    }
    P->seek = frame;
    return NULL;
}

/* The view a start would play now, or is playing: "native" or "inputs". */
static inline const char *replay__view_name(void) {
    const replay_play_t *P = &g_replay_play;
    if (P->on) return P->native_on ? "native" : "inputs";
    char profile[32] = "";
    json_get_str(P->json, "profile", profile, sizeof profile);
    return P->loaded && replay__view_native(profile_by_id(profile)) ? "native" : "inputs";
}

/* {"on","frame","frames","ended","split","why","fast","seeking","view",...}:
 * frames are the feed's, or in the game's own view its key player's. */
typedef struct { uint32_t frame, frames; bool ended, split; } replay__pos_t;

static inline replay__pos_t replay__pos(const replay_play_t *P, bool nat) {
    replay__pos_t r = {0};
    if (nat) {
        r.frame = g_native_play.total;
        r.frames = g_native_play.total_frames;
        r.ended = g_native_play.ended;
        return r;
    }
    r.frames = P->frames;
    if (!P->on) return r;
    r.frame = g_follow.frame;
    r.ended = g_follow.ended;
    r.split = g_follow.split;
    return r;
}

static inline void replay_play_status(char *out, size_t cap) {
    const replay_play_t *P = &g_replay_play;
    bool nat = P->on && P->native_on;
    char why[160];
    json_escape(why, sizeof why, nat ? "" : g_follow.why);
    replay__pos_t pos = replay__pos(P, nat);
    uint32_t frame = pos.frame, frames = pos.frames;
    bool ended = pos.ended, split = pos.split;
    snprintf(out, cap,
             "{\"on\":%d,\"loaded\":%d,\"frame\":%u,\"frames\":%u,\"ended\":%d,\"split\":%d,"
             "\"why\":\"%s\",\"fast\":%d,\"seeking\":%d,\"native_rounds\":%d,\"view\":\"%s\","
             "\"keyed_rounds\":%d,\"round\":%d,\"rounds\":%d,\"has_inputs\":%d}",
             P->on ? 1 : 0, P->loaded ? 1 : 0, frame, frames, ended ? 1 : 0, split ? 1 : 0,
             P->on ? why : "", P->fast ? 1 : 0, P->on && frame < P->seek ? 1 : 0, P->native.n,
             replay__view_name(), P->nrounds, nat ? g_native_play.round + 1 : 0,
             nat ? g_native_play.rounds : 0, P->feed ? 1 : 0);
}

#endif /* REPLAY_H */
