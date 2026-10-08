/* follow.h -- follow a board one way (Pinboard #568, issue #228).
 *
 * One board leads: it plays alone (the fly's headless board, driven over MCP)
 * and writes what it does to a directory. Any number of others follow it from
 * there (the web build's ?follow=), sending nothing back. Only the board's
 * inputs cross: a follower runs the same board from the same state on the same
 * inputs, so it shows the same game, which is the whole idea of netplay's
 * lockstep with one side.
 *
 * What the leader writes, into its directory:
 *   follow.json   which segment is newest, where it starts, and the profile
 *   seg-N.sta     the join point: a savestate (savestate.h), taken between slices
 *   seg-N.feed    everything that reached the board after it, in order
 *
 * The feed is a 32-byte header ("M2FOLLOW", version, segment, start frame and
 * slice, the step cap) and then records, all little-endian, each one
 *   u8 type, u8 0, u16 slice, u32 frame, u32 len, payload[len]
 * tagged with the position of the slice it comes before. A position is the
 * frame and the slice within it: a slice normally runs to the vblank, but one
 * can stop at the step cap, and inputs and writes land between slices.
 *   INPUT  u32 held, the word the board read for that slice (g_input)
 *   WRITE  u32 addr, u8 rom, bytes: an MCP write_memory, rom as it means there
 *   SET    the HLE settings (region, VS mode, DAMAGE, rules, CPU difficulty)
 *   CHECK  at the end of each frame (slice 0xFFFF): the netplay frame check,
 *          the frame's slice count, and every FOLLOW_RAM_EVERY frames a hash
 *          of work RAM
 *   END    the segment stops here, its payload says why
 * Anything else that changes the board from outside -- a reset, a state load,
 * a single step, a COP command, SKY EYE, a netplay session -- cannot be put in
 * the feed. It ends the segment (follow_lead_break) and the next slice starts a
 * new one from a new savestate. So does a timer, so that a follower that joins
 * late or splits has a recent place to start from.
 *
 * The follower may run a slice once the feed holds a record from past it: the
 * leader writes records in position order, so that is when it has everything
 * for the slice. Each frame's CHECK is compared to its own; a split stops it
 * (follow_status says so) until it joins again.
 *
 * Both halves hook the slice itself (follow_slice_begin / _end, from
 * emu_slice_body, under the mutex), so every frontend does the same.
 */
#ifndef FOLLOW_H
#define FOLLOW_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <pthread.h>
#endif

#define FOLLOW_MAGIC       "M2FOLLOW"
#define FOLLOW_VERSION     1u
#define FOLLOW_HDR_BYTES   32u
#define FOLLOW_REC_BYTES   12u
#define FOLLOW_CHECK_SLICE 0xFFFFu
#define FOLLOW_RAM_EVERY   60u
#define FOLLOW_SET_WORDS   8
#define FOLLOW_ROM_MAX     65536u          /* ROM patch bytes carried into each segment */
#define FOLLOW_EVERY_DEF   (60u * 60u * 5u) /* a new segment every five minutes */

enum { FOLLOW_INPUT = 1, FOLLOW_WRITE, FOLLOW_SET, FOLLOW_CHECK, FOLLOW_END };

static inline uint64_t follow_key(uint32_t frame, uint32_t slice) {
    return ((uint64_t)frame << 16) | (slice & 0xFFFFu);
}

static inline void follow_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint32_t follow_get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The HLE settings a host can change under a running board (savestate_hle_t's). */
static inline void follow_settings(int32_t s[FOLLOW_SET_WORDS]) {
    s[0] = g_region;      s[1] = g_vs_mode;   s[2] = g_damage_real; s[3] = g_rounds_to_win;
    s[4] = g_round_time;  s[5] = g_game_type; s[6] = g_hidden_chars; s[7] = g_enemy_rank;
}
static inline void follow_settings_put(const int32_t s[FOLLOW_SET_WORDS]) {
    g_region     = s[0]; g_vs_mode   = s[1]; g_damage_real  = s[2]; g_rounds_to_win = s[3];
    g_round_time = s[4]; g_game_type = s[5]; g_hidden_chars = s[6]; g_enemy_rank    = s[7];
}

/* A write as the MCP bridge makes it: "rom" patches what the CPU reads,
 * program ROM included; otherwise the bus's own write map. */
static inline uint32_t follow_bus_write(memory_bus_t *bus, uint32_t addr, bool rom,
                                        const uint8_t *data, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        const uint32_t a = addr + i;
        if (rom) {
            uint8_t *pg = bus->rd_page[a >> 16];
            if (!pg) break;
            pg[a & 0xFFFFu] = data[i];
        } else {
            mem_write8(bus, a, data[i]);
        }
    }
    return i;
}

static inline uint32_t follow_ram_hash(const memory_bus_t *bus) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < RAM_SIZE; i++) { h ^= bus->ram[i]; h *= 16777619u; }
    return h;
}

/* ---- The leader ------------------------------------------------------------ */

typedef struct {
    bool     on;
    char     dir[512];
    FILE    *feed;
    uint32_t seg;
    uint32_t frame;          /* the position of the next slice */
    uint32_t slice;
    uint32_t every;          /* frames between segments, 0 = only when needed */
    uint32_t keep;           /* segments kept on disk, this one included (2) */
    uint32_t seg_frames;     /* frames into this segment */
    bool     join_due;
    char     why[64];        /* why the last segment ended */
    bool     held_known, set_known;
    uint32_t held;
    int32_t  set[FOLLOW_SET_WORDS];
    int      use_net_was;    /* the netplay latch, put back after the slice */
    bool     latched;
    uint8_t *rom;            /* ROM patches so far, as WRITE payloads, replayed per segment */
    uint32_t rom_len;
    uint64_t bytes;          /* written to this segment's feed */
    uint32_t join_ms;        /* what the last join point held the board up */
    uint32_t write_ms;       /* what writing it out took, beside the board */
    uint8_t *snap;           /* the join point as taken: a stored zip, deflated by the writer */
    size_t   snap_cap, snap_len;
    struct { uint32_t seg, frame, slice; int sps; char profile[32]; } job;
    bool     writing;        /* a writer thread is out (joined before the next) */
#ifdef _WIN32
    HANDLE   writer;
#elif !defined(__EMSCRIPTEN__)
    pthread_t writer;
#endif
    char     error[128];
} follow_lead_t;

static follow_lead_t g_follow_lead;

static inline bool follow_leading(void) { return g_follow_lead.on; }

static inline void follow__rec_hdr(uint8_t *h, int type, uint32_t frame, uint32_t slice, uint32_t len) {
    h[0] = (uint8_t)type; h[1] = 0; h[2] = (uint8_t)slice; h[3] = (uint8_t)(slice >> 8);
    follow_put32(h + 4, frame);
    follow_put32(h + 8, len);
}

static inline void follow__lead_rec(int type, uint32_t slice, const void *p, uint32_t n) {
    follow_lead_t *L = &g_follow_lead;
    if (!L->feed) return;
    uint8_t h[FOLLOW_REC_BYTES];
    follow__rec_hdr(h, type, L->frame, slice, n);
    fwrite(h, 1, sizeof h, L->feed);
    if (n) fwrite(p, 1, n, L->feed);
    L->bytes += sizeof h + n;
}

static inline void follow__path(char *out, size_t cap, const char *name, uint32_t seg, const char *ext) {
    if (seg == UINT32_MAX) snprintf(out, cap, "%s/%s", g_follow_lead.dir, name);
    else snprintf(out, cap, "%s/%s-%u%s", g_follow_lead.dir, name, seg, ext);
}

/* follow.json, whole or not at all (temp file + rename), so a reader never
 * sees half of it. */
static inline void follow__lead_index(void) {
    const follow_lead_t *L = &g_follow_lead;
    char path[600], tmp[620];
    follow__path(path, sizeof path, "follow.json", UINT32_MAX, "");
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    fprintf(f, "{\"version\":%u,\"seg\":%u,\"state\":\"seg-%u.sta\",\"feed\":\"seg-%u.feed\","
               "\"frame\":%u,\"slice\":%u,\"profile\":\"%s\",\"steps_per_slice\":%d}\n",
            FOLLOW_VERSION, L->job.seg, L->job.seg, L->job.seg, L->job.frame, L->job.slice,
            L->job.profile, L->job.sps);
    fclose(f);
    remove(path);   /* Windows' rename will not replace */
    rename(tmp, path);
}

/* The join point as the board left it (a stored zip in L->snap) deflated into
 * seg-N.sta, then follow.json names it and the oldest segment goes. Beside the
 * board: the deflate is ~100 ms, six frames the board would otherwise wait. */
static inline void follow__lead_write_out(void) {
    follow_lead_t *L = &g_follow_lead;
    int64_t t0 = emu_now_us();
    char path[600], tmp[620], name[64];
    follow__path(path, sizeof path, "seg", L->job.seg, ".sta");
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    mz_zip_archive in, out;
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    bool rd = mz_zip_reader_init_mem(&in, L->snap, L->snap_len, 0);
    bool wr = rd && mz_zip_writer_init_file(&out, tmp, 0);
    bool ok = wr;
    for (mz_uint i = 0; ok && i < mz_zip_reader_get_num_files(&in); i++) {
        size_t n = 0;
        void *data = mz_zip_reader_extract_to_heap(&in, i, &n, 0);
        mz_zip_reader_get_filename(&in, i, name, sizeof name);
        ok = data && mz_zip_writer_add_mem(&out, name, data, n, MZ_BEST_SPEED);
        mz_free(data);
    }
    if (wr) {
        ok = ok && mz_zip_writer_finalize_archive(&out);
        mz_zip_writer_end(&out);
    }
    if (rd) mz_zip_reader_end(&in);
    if (!ok) { remove(tmp); LOG_WARN("follow: could not write %s", path); return; }
    remove(path);
    rename(tmp, path);
    follow__lead_index();
    if (L->keep && L->job.seg > L->keep) {   /* this segment and the last, by default */
        uint32_t old = L->job.seg - L->keep;
        follow__path(path, sizeof path, "seg", old, ".sta");  remove(path);
        follow__path(path, sizeof path, "seg", old, ".feed"); remove(path);
    }
    L->write_ms = (uint32_t)((emu_now_us() - t0 + 500) / 1000);
}

#ifdef _WIN32
static DWORD WINAPI follow__writer_proc(LPVOID arg) { (void)arg; follow__lead_write_out(); return 0; }
#elif !defined(__EMSCRIPTEN__)
static void *follow__writer_proc(void *arg) { (void)arg; follow__lead_write_out(); return NULL; }
#endif

/* Wait for the last join point's writer. */
static inline void follow__lead_wait(void) {
    follow_lead_t *L = &g_follow_lead;
    if (!L->writing) return;
#ifdef _WIN32
    WaitForSingleObject(L->writer, INFINITE);
    CloseHandle(L->writer);
#elif !defined(__EMSCRIPTEN__)
    pthread_join(L->writer, NULL);
#endif
    L->writing = false;
}

/* Hand the join point to a writer thread; on a host without one, write it now. */
static inline void follow__lead_write_start(void) {
    follow_lead_t *L = &g_follow_lead;
#ifdef _WIN32
    L->writer  = CreateThread(NULL, 0, follow__writer_proc, NULL, 0, NULL);
    L->writing = L->writer != NULL;
#elif !defined(__EMSCRIPTEN__)
    L->writing = pthread_create(&L->writer, NULL, follow__writer_proc, NULL) == 0;
#endif
    if (!L->writing) follow__lead_write_out();
}

/* The board as it stands, as a stored zip in L->snap. NULL, or why not. */
static inline const char *follow__lead_snap(emu_thread_ctx_t *ctx) {
    follow_lead_t *L = &g_follow_lead;
    for (int tries = 0; tries < 2; tries++) {
        if (!L->snap) {
            size_t need = 0;
            const char *err = emu_state_save_mem(ctx, NULL, 0, &need);
            if (err) return err;
            L->snap_cap = need + (1u << 20);
            L->snap = (uint8_t *)malloc(L->snap_cap);
            if (!L->snap) return "out of memory";
        }
        const char *err = emu_state_save_mem(ctx, L->snap, L->snap_cap, &L->snap_len);
        if (!err) return NULL;
        free(L->snap);   /* grown since: measure again */
        L->snap = NULL;
        if (tries) return err;
    }
    return "no state";
}

static inline void follow__lead_close(const char *why) {
    follow_lead_t *L = &g_follow_lead;
    if (!L->feed) return;
    follow__lead_rec(FOLLOW_END, L->slice, why, (uint32_t)strlen(why));
    fclose(L->feed);
    L->feed = NULL;
}

static inline void follow__lead_header(uint32_t seg) {
    follow_lead_t *L = &g_follow_lead;
    uint8_t h[FOLLOW_HDR_BYTES] = {0};
    memcpy(h, FOLLOW_MAGIC, 8);
    follow_put32(h + 8,  FOLLOW_VERSION);
    follow_put32(h + 12, seg);
    follow_put32(h + 16, L->frame);
    follow_put32(h + 20, L->slice);
    follow_put32(h + 24, (uint32_t)g_emu_steps_per_slice);
    fwrite(h, 1, sizeof h, L->feed);
    L->bytes = sizeof h;
}

/* Start a new segment here: a savestate, then an empty feed. False (and the
 * join stays due) if the state could not be taken, e.g. under SKY EYE. */
static inline bool follow__lead_join(emu_thread_ctx_t *ctx) {
    follow_lead_t *L = &g_follow_lead;
    char path[600];
    follow__lead_close(L->why[0] ? L->why : "new segment");
    uint32_t seg = L->seg + 1;
    follow__lead_wait();
    int64_t t0 = emu_now_us();
    const char *err = follow__lead_snap(ctx);   /* refused under SKY EYE */
    L->join_ms = (uint32_t)((emu_now_us() - t0 + 500) / 1000);
    if (err) {
        snprintf(L->error, sizeof L->error, "%s", err);
        return false;
    }
    follow__path(path, sizeof path, "seg", seg, ".feed");
    L->feed = fopen(path, "wb");
    if (!L->feed) { snprintf(L->error, sizeof L->error, "cannot write %.80s", path); return false; }
    L->seg = seg;
    L->error[0] = 0;
    follow__lead_header(seg);
    if (L->rom_len) {   /* ROM is not in the savestate: patch it again */
        uint8_t h[FOLLOW_REC_BYTES];
        for (uint32_t at = 0; at < L->rom_len; ) {
            uint32_t n = follow_get32(L->rom + at + 8);
            follow__rec_hdr(h, FOLLOW_WRITE, L->frame, L->slice, n);
            fwrite(h, 1, sizeof h, L->feed);
            fwrite(L->rom + at + FOLLOW_REC_BYTES, 1, n, L->feed);
            L->bytes += sizeof h + n;
            at += FOLLOW_REC_BYTES + n;
        }
    }
    fflush(L->feed);
    L->job.seg   = seg;
    L->job.frame = L->frame;
    L->job.slice = L->slice;
    L->job.sps   = (int)g_emu_steps_per_slice;
    snprintf(L->job.profile, sizeof L->job.profile, "%s", g_active_profile ? g_active_profile->id : "");
    follow__lead_write_start();
    L->join_due = false;
    L->why[0] = 0;
    L->held_known = L->set_known = false;
    L->seg_frames = 0;
    LOG_INFO("follow: segment %u at frame %u slice %u", seg, L->frame, L->slice);
    return true;
}

/* Lead into `dir` (made if missing). `every` frames between segments, 0 for
 * the default. NULL on success. */
static inline const char *follow_lead_start(const char *dir, uint32_t every) {
    follow_lead_t *L = &g_follow_lead;
    if (L->on) return "already leading";
    if (!dir || !*dir || strlen(dir) >= sizeof L->dir) return "no directory";
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0777);
#endif
    snprintf(L->dir, sizeof L->dir, "%s", dir);
    L->every    = every ? every : FOLLOW_EVERY_DEF;
    L->keep     = 2;
    L->frame    = g_emu_frames;
    L->slice    = 0;
    L->seg      = 0;
    L->join_due = true;
    L->on       = true;
    L->why[0]   = 0;
    return NULL;
}

static inline void follow_lead_stop(void) {
    follow__lead_close("leader stopped");
    follow__lead_wait();
    g_follow_lead.on = false;
}

/* Something the feed cannot carry changed the board: end this segment, and
 * start the next before the next slice. */
static inline void follow_lead_break(const char *why) {
    follow_lead_t *L = &g_follow_lead;
    if (!L->on) return;
    if (!L->join_due) snprintf(L->why, sizeof L->why, "%s", why);
    L->join_due = true;
}

/* An MCP write_memory, as it lands (the caller holds the mutex). */
static inline void follow_lead_write(uint32_t addr, bool rom, const uint8_t *data, uint32_t n) {
    follow_lead_t *L = &g_follow_lead;
    if (!L->on || !n) return;
    uint8_t *p = (uint8_t *)malloc(5u + n);
    if (!p) { follow_lead_break("out of memory"); return; }
    follow_put32(p, addr);
    p[4] = rom ? 1 : 0;
    memcpy(p + 5, data, n);
    follow__lead_rec(FOLLOW_WRITE, L->slice, p, 5u + n);
    if (L->feed) fflush(L->feed);
    if (rom) {
        uint32_t need = L->rom_len + FOLLOW_REC_BYTES + 5u + n;
        uint8_t *r = need <= FOLLOW_ROM_MAX ? (uint8_t *)realloc(L->rom, need) : NULL;
        if (!r) {
            LOG_WARN("follow: more than %u bytes of ROM patches; later segments lose them", FOLLOW_ROM_MAX);
        } else {
            L->rom = r;
            follow__rec_hdr(r + L->rom_len, FOLLOW_WRITE, 0, 0, 5u + n);
            memcpy(r + L->rom_len + FOLLOW_REC_BYTES, p, 5u + n);
            L->rom_len = need;
        }
    }
    free(p);
}

static inline void follow__lead_begin(emu_thread_ctx_t *ctx) {
    follow_lead_t *L = &g_follow_lead;
    if (!L->join_due && L->every && L->seg_frames >= L->every && L->slice == 0)
        L->join_due = true;
    if (L->join_due && !follow__lead_join(ctx)) return;   /* tried again next slice */
    int32_t set[FOLLOW_SET_WORDS];
    follow_settings(set);
    if (!L->set_known || memcmp(set, L->set, sizeof set)) {
        uint8_t p[FOLLOW_SET_WORDS * 4];
        for (int i = 0; i < FOLLOW_SET_WORDS; i++) follow_put32(p + i * 4, (uint32_t)set[i]);
        follow__lead_rec(FOLLOW_SET, L->slice, p, sizeof p);
        memcpy(L->set, set, sizeof set);
        L->set_known = true;
    }
    /* The board reads one word for the whole slice: the host's keys can move
     * under it on another thread, and the follower has to see what it saw. */
    uint32_t held = g_input.use_net ? g_input.net_held : g_input.held;
    L->use_net_was = g_input.use_net;
    L->latched     = true;
    g_input.net_held = held;
    g_input.use_net  = 1;
    if (!L->held_known || held != L->held) {
        uint8_t p[4];
        follow_put32(p, held);
        follow__lead_rec(FOLLOW_INPUT, L->slice, p, 4);
        L->held = held;
        L->held_known = true;
    }
}

static inline void follow__check_payload(emu_thread_ctx_t *ctx, uint32_t frame, uint32_t slices,
                                         uint8_t *p, uint32_t *n) {
    follow_put32(p, netplay_frame_check(ctx->cpu, ctx->total_steps));
    follow_put32(p + 4, slices);
    *n = 8;
    if (frame % FOLLOW_RAM_EVERY == 0) { follow_put32(p + 8, follow_ram_hash(ctx->bus)); *n = 12; }
}

static inline void follow__lead_end(emu_thread_ctx_t *ctx, bool frame_end) {
    follow_lead_t *L = &g_follow_lead;
    if (L->latched) {
        g_input.use_net = L->use_net_was;
        L->latched = false;
    }
    if (!frame_end) { L->slice++; return; }
    uint8_t p[12];
    uint32_t n;
    follow__check_payload(ctx, L->frame, L->slice + 1, p, &n);
    follow__lead_rec(FOLLOW_CHECK, FOLLOW_CHECK_SLICE, p, n);
    if (L->feed) fflush(L->feed);
    L->frame++;
    L->slice = 0;
    L->seg_frames++;
}

/* ---- The follower ---------------------------------------------------------- */

typedef struct {
    bool     on;           /* following: the board runs only on the feed */
    bool     joined;       /* a segment's state is loaded */
    bool     ended;        /* reached its END */
    bool     split;        /* a CHECK differed */
    uint8_t *buf;          /* the feed so far, from its header */
    size_t   len, cap;
    bool     hdr;          /* the header has been read */
    size_t   at;           /* the next record to apply */
    size_t   scan;         /* the next record not yet looked at */
    uint64_t newest;       /* the newest record's key seen */
    uint64_t end_key;      /* the END's key, UINT64_MAX until one is seen */
    uint32_t seg, frame, slice, frame_slices;
    uint32_t checks, rams;
    uint32_t held;         /* the leader's composed word, put in at every slice */
    char     why[128];
} follow_sub_t;

static follow_sub_t g_follow;

static inline bool follow_following(void) { return g_follow.on; }

static inline void follow__split(const char *fmt, uint32_t a, uint32_t b) {
    if (g_follow.split) return;
    g_follow.split = true;
    snprintf(g_follow.why, sizeof g_follow.why, fmt, g_follow.frame, a, b);
    LOG_WARN("follow: split from the leader: %s", g_follow.why);
}

/* Look at what has arrived: the newest position, and an END. */
static inline void follow__scan(void) {
    follow_sub_t *F = &g_follow;
    while (F->scan + FOLLOW_REC_BYTES <= F->len) {
        const uint8_t *r = F->buf + F->scan;
        uint32_t n = follow_get32(r + 8);
        if (F->scan + FOLLOW_REC_BYTES + n > F->len) break;
        uint64_t key = follow_key(follow_get32(r + 4), (uint32_t)r[2] | (uint32_t)r[3] << 8);
        if (key > F->newest || F->newest == UINT64_MAX) F->newest = key;
        if (r[0] == FOLLOW_END && F->end_key == UINT64_MAX) {
            uint32_t w = n < sizeof F->why - 1 ? n : (uint32_t)sizeof F->why - 1;
            memcpy(F->why, r + FOLLOW_REC_BYTES, w);
            F->why[w] = 0;
            F->end_key = key;
        }
        F->scan += FOLLOW_REC_BYTES + n;
    }
}

/* Leave the feed behind: back to an ordinary board, keys and all. */
static inline void follow_stop(void) {
    free(g_follow.buf);
    memset(&g_follow, 0, sizeof g_follow);
    g_input.use_net = 0;
}

/* Join: load the segment's state (the caller holds the mutex) and start an
 * empty feed for it. The feed's header comes in with its first bytes. */
static inline const char *follow_join(emu_thread_ctx_t *ctx, const void *state, size_t size) {
    const char *err = emu_state_load_mem(ctx, state, size);
    if (err) return err;
    free(g_follow.buf);
    memset(&g_follow, 0, sizeof g_follow);
    g_follow.on      = true;
    g_follow.joined  = true;
    g_follow.newest  = UINT64_MAX;
    g_follow.end_key = UINT64_MAX;
    g_input.use_net  = 1;
    return NULL;
}

/* Bytes of the joined segment's feed, in order. NULL, or why they cannot be. */
static inline const char *follow_feed(const void *data, size_t n) {
    follow_sub_t *F = &g_follow;
    if (!F->joined) return "not joined";
    if (F->len + n > F->cap) {
        size_t cap = F->cap ? F->cap : 65536;
        while (cap < F->len + n) cap *= 2;
        uint8_t *b = (uint8_t *)realloc(F->buf, cap);
        if (!b) return "out of memory";
        F->buf = b; F->cap = cap;
    }
    memcpy(F->buf + F->len, data, n);
    F->len += n;
    if (!F->hdr && F->len >= FOLLOW_HDR_BYTES) {
        if (memcmp(F->buf, FOLLOW_MAGIC, 8) || follow_get32(F->buf + 8) != FOLLOW_VERSION) {
            F->joined = false;
            return "not a follow feed of this version";
        }
        F->seg   = follow_get32(F->buf + 12);
        F->frame = follow_get32(F->buf + 16);
        F->slice = follow_get32(F->buf + 20);
        F->frame_slices = F->slice;
        g_emu_steps_per_slice = (int)follow_get32(F->buf + 24);
        F->at = F->scan = FOLLOW_HDR_BYTES;
        F->hdr = true;
    }
    if (F->hdr) follow__scan();
    return NULL;
}

/* The feed holds everything for the next slice. */
static inline bool follow_slice_ready(void) {
    follow_sub_t *F = &g_follow;
    if (!F->joined || !F->hdr || F->split || F->ended) return false;
    uint64_t cur = follow_key(F->frame, F->slice);
    if (F->end_key <= cur) {   /* the board stands where the leader's segment ended */
        F->ended = true;
        return false;
    }
    return F->newest != UINT64_MAX && F->newest > cur;
}

/* Frames the feed holds past the board: run unpaced while there are many. */
static inline uint32_t follow_buffered(void) {
    const follow_sub_t *F = &g_follow;
    if (!follow_slice_ready()) return 0;
    uint32_t newest = (uint32_t)(F->newest >> 16);
    return newest > F->frame ? newest - F->frame : 0;
}

static inline bool follow__apply(emu_thread_ctx_t *ctx, const uint8_t *r, uint32_t n) {
    switch (r[0]) {
    case FOLLOW_INPUT:
        if (n >= 4) g_follow.held = follow_get32(r + FOLLOW_REC_BYTES);
        return true;
    case FOLLOW_WRITE:
        if (n >= 5) follow_bus_write(ctx->bus, follow_get32(r + FOLLOW_REC_BYTES), r[FOLLOW_REC_BYTES + 4] != 0,
                                     r + FOLLOW_REC_BYTES + 5, n - 5);
        return true;
    case FOLLOW_SET:
        if (n >= FOLLOW_SET_WORDS * 4) {
            int32_t s[FOLLOW_SET_WORDS];
            for (int i = 0; i < FOLLOW_SET_WORDS; i++) s[i] = (int32_t)follow_get32(r + FOLLOW_REC_BYTES + i * 4);
            follow_settings_put(s);
        }
        return true;
    case FOLLOW_END:
        g_follow.ended = true;
        return false;
    default:
        return false;   /* a CHECK: the frame's end takes it */
    }
}

/* Apply every record up to and including `key` that is not a CHECK. */
static inline void follow__apply_to(emu_thread_ctx_t *ctx, uint64_t key) {
    follow_sub_t *F = &g_follow;
    while (F->at + FOLLOW_REC_BYTES <= F->len) {
        const uint8_t *r = F->buf + F->at;
        uint32_t n = follow_get32(r + 8);
        if (F->at + FOLLOW_REC_BYTES + n > F->len) break;
        if (follow_key(follow_get32(r + 4), (uint32_t)r[2] | (uint32_t)r[3] << 8) > key) break;
        if (r[0] == FOLLOW_CHECK) break;
        if (!follow__apply(ctx, r, n) && F->ended) break;
        F->at += FOLLOW_REC_BYTES + n;
    }
}

static inline void follow__sub_check(emu_thread_ctx_t *ctx) {
    follow_sub_t *F = &g_follow;
    const uint8_t *r = F->buf + F->at;
    uint32_t n = F->at + FOLLOW_REC_BYTES <= F->len ? follow_get32(r + 8) : 0;
    if (F->at + FOLLOW_REC_BYTES > F->len || r[0] != FOLLOW_CHECK
        || follow_get32(r + 4) != F->frame || n < 8) {
        follow__split("frame %u ended here and not on the leader (%u, %u)", 0, 0);
        return;
    }
    uint8_t mine[12];
    uint32_t mn;
    follow__check_payload(ctx, F->frame, F->frame_slices, mine, &mn);
    const uint8_t *p = r + FOLLOW_REC_BYTES;
    if (follow_get32(p + 4) != F->frame_slices)
        follow__split("frame %u took %u slices, the leader's %u", F->frame_slices, follow_get32(p + 4));
    else if (memcmp(p, mine, 4))
        follow__split("frame %u check %08X, the leader's %08X", follow_get32(mine), follow_get32(p));
    else if (n >= 12 && mn >= 12 && memcmp(p + 8, mine + 8, 4))
        follow__split("frame %u work RAM %08X, the leader's %08X", follow_get32(mine + 8), follow_get32(p + 8));
    F->checks++;
    if (n >= 12) F->rams++;
    F->at += FOLLOW_REC_BYTES + n;
    if (F->at > (1u << 20)) {   /* what has been run is not needed again */
        memmove(F->buf, F->buf + F->at, F->len - F->at);
        F->len  -= F->at;
        F->scan -= F->at;
        F->at    = 0;
    }
}

static inline void follow__sub_begin(emu_thread_ctx_t *ctx) {
    follow__apply_to(ctx, follow_key(g_follow.frame, g_follow.slice));
    /* Here and not only on an INPUT: a host's netplay pump lets go of the
     * latch between slices when it has no session. */
    g_input.net_held = g_follow.held;
    g_input.use_net  = 1;
}

static inline void follow__sub_end(emu_thread_ctx_t *ctx, bool frame_end) {
    follow_sub_t *F = &g_follow;
    F->frame_slices++;
    if (!frame_end) { F->slice++; return; }
    follow__sub_check(ctx);
    F->frame++;
    F->slice = 0;
    F->frame_slices = 0;
}

/* ---- The slice's hooks (emu_slice_body, under the mutex) ------------------- */

static inline void follow_slice_begin(emu_thread_ctx_t *ctx) {
    if (g_follow.on)        follow__sub_begin(ctx);
    else if (g_follow_lead.on) follow__lead_begin(ctx);
}

static inline void follow_slice_end(emu_thread_ctx_t *ctx, bool frame_end) {
    if (g_follow.on)        follow__sub_end(ctx, frame_end);
    else if (g_follow_lead.on) follow__lead_end(ctx, frame_end);
}

#endif /* FOLLOW_H */
