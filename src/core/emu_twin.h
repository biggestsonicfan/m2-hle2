/*
 * emu_twin.h — both cabinets of a comm-board link on one machine, for GGPO
 * (Pinboard #1080).
 *
 * A linked game (Virtual-On, Power Sled) puts each player on a board of their
 * own, the two boards talking through their comm boards (board/m2comm.h).
 * GGPO rolls one simulation back and runs it again on the real inputs, and a
 * link run over the network is not one: the other cabinet's comm frames
 * depend on what it guessed. So a GGPO match of a linked game runs BOTH
 * cabinets on each machine, and the only thing that crosses the network is
 * the two players' inputs, as in any other GGPO match. The two machines run the same pair of
 * boards and see the same frames.
 *
 * The two boards take turns in the one board this process has. Between frames
 * the live board is this machine's player's cabinet (the one the screen, the
 * speakers and the tools see); the other one lives in a snapshot. Each frame:
 *
 *   1. the live board L runs its frame;
 *   2. it is copied out into the next GGPO buffer's half for L;
 *   3. the hidden board H comes in from its last snapshot and runs its
 *      frame, muted;
 *   4. it is copied out into the buffer's half for H, and L goes back in.
 *
 * GGPO's save then hands over that buffer: [board A][board B][link]. Four
 * copies of the board a frame, the least this layout allows (a save of L is
 * the buffer's L half; H's snapshot is the last buffer's H half).
 *
 * The boards' comm link is a pair of queues (emu_twin_link_t, in the buffer's
 * link part). What one board sends in frame F the other can take from frame
 * F+1 on: a frame's sends are committed after both boards ran it, so the order
 * the two run in changes nothing. Each board's g_m2comm_link (outside
 * savestates, being the network's on a real link) goes in the link part too.
 *
 * A swap is a rollback load without savestate_bus_changed: the caches keyed on
 * the bus's change generations are the renderer's, and once L is back the
 * bus holds what they were built from. The hidden board's own writes only
 * mark what they touched, which the renderer then takes again. The renderer
 * must not read the bus while H is in it: on the desktop it runs on the UI
 * thread, so game_frame.h takes the emu mutex around its reads while a twin
 * session runs (emu_twin_ui_lock). The web build and the libretro core draw
 * between frames on the thread that runs them.
 *
 * Roles. Board A is the MASTER (1P's), board B the SLAVE (2P's). A session's
 * cold boot runs alone, muted, until the ROM has made its backup block; the
 * profile's link_role writes each role into a copy of it, and each board is
 * reset with its copy kept (net/comm_role.h's power cycle), B first and
 * stashed, A last. Both machines make the same boards from the same boot.
 * Then the live board is the local player's.
 *
 * A player's word is a whole cabinet's controls: game_profile's P1 set in bits
 * 0..11 (with the cabinet's SERVICE and TEST), its P2 set in bits 12..21:
 * Virtual-On's levers are P1 and P2 of one cabinet.
 *
 * Header-only. Included from emu_ggpo.h, M2HLE_GGPO builds. Emu thread, emu
 * mutex held.
 */
#ifndef M2HLE_EMU_TWIN_H
#define M2HLE_EMU_TWIN_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "m2comm.h"
#include "rollback.h"

#define EMU_TWIN_QUEUE_MAX 16u   /* frames of a link's sends waiting; the boards run in step */
#define EMU_TWIN_BOOT_MAX  1800  /* frames the cold boot may take to make its backup block */
#define EMU_TWIN_P2_SHIFT  12    /* where a word's P2 set starts */

typedef struct {
    uint32_t head, n;
    uint16_t len[EMU_TWIN_QUEUE_MAX];
    uint8_t  msg[EMU_TWIN_QUEUE_MAX][M2COMM_MSG_MAX];
} emu_twin_queue_t;

/* The link part of a twin buffer. POD, no host pointers. */
typedef struct {
    emu_twin_queue_t ready[2];   /* board b takes from ready[b] */
    emu_twin_queue_t sent[2];    /* this frame's for board b, ready from the next */
    m2comm_link_t    link[2];    /* each board's g_m2comm_link, peer NULL */
    uint32_t         dropped;    /* sends a full queue lost */
} emu_twin_link_t;

static struct {
    bool            on;
    int             local;       /* the live board between frames: 0 = A, 1 = B */
    size_t          board;       /* one board's bytes, g_rollback.size */
    size_t          size;        /* a buffer's: two boards and the link */
    void           *pool[ROLLBACK_POOL_MAX + 2];
    int             pool_n;
    uint8_t        *stash;       /* the hidden board when no buffer holds it */
    const uint8_t  *hidden;      /* the hidden board's latest snapshot */
    uint8_t        *pending;     /* the buffer the last frame filled, for the save */
    emu_twin_link_t lk;          /* the link, live */
    m2comm_peer_t   peer[2];
    emu_mutex_t    *ui_lock;     /* the desktop's emu mutex while a session runs */
    uint32_t        drop_said;
} g_twin;

/* ---- The link between the two boards ------------------------------------- */

static void emu_twin__send(void *ctx, const uint8_t *msg, uint32_t len) {
    int to = 1 - (int)(intptr_t)ctx;
    emu_twin_queue_t *q = &g_twin.lk.sent[to];
    if (q->n >= EMU_TWIN_QUEUE_MAX || len > M2COMM_MSG_MAX) { g_twin.lk.dropped++; return; }
    uint32_t at = (q->head + q->n++) % EMU_TWIN_QUEUE_MAX;
    q->len[at] = (uint16_t)len;
    memcpy(q->msg[at], msg, len);
}

static uint32_t emu_twin__recv(void *ctx, uint8_t *msg, uint32_t cap) {
    emu_twin_queue_t *q = &g_twin.lk.ready[(int)(intptr_t)ctx];
    if (q->n == 0) return 0;
    uint32_t len = q->len[q->head];
    if (len > cap) len = cap;
    memcpy(msg, q->msg[q->head], len);
    q->head = (q->head + 1) % EMU_TWIN_QUEUE_MAX;
    q->n--;
    return len;
}

/* A frame's sends, ready for the next frame, once both boards have run it. */
static inline void emu_twin__commit(void) {
    for (int b = 0; b < 2; b++) {
        emu_twin_queue_t *from = &g_twin.lk.sent[b], *to = &g_twin.lk.ready[b];
        for (; from->n; from->n--, from->head = (from->head + 1) % EMU_TWIN_QUEUE_MAX) {
            if (to->n >= EMU_TWIN_QUEUE_MAX) { g_twin.lk.dropped++; continue; }
            uint32_t at = (to->head + to->n++) % EMU_TWIN_QUEUE_MAX;
            to->len[at] = from->len[from->head];
            memcpy(to->msg[at], from->msg[from->head], from->len[from->head]);
        }
        from->head = 0;
    }
    if (g_twin.lk.dropped != g_twin.drop_said) {
        g_twin.drop_said = g_twin.lk.dropped;
        LOG_WARN("twin: %u comm frame(s) dropped on a full queue", g_twin.lk.dropped);
    }
}

/* Board b's link state out of, and into, the live g_m2comm_link. */
static inline void emu_twin__link_out(int b) {
    g_twin.lk.link[b] = g_m2comm_link;
    g_twin.lk.link[b].peer = NULL;
}

static inline void emu_twin__link_in(int b) {
    g_m2comm_link = g_twin.lk.link[b];
    g_m2comm_link.peer = &g_twin.peer[b];
}

/* ---- Buffers -------------------------------------------------------------- */

static inline uint8_t *emu_twin_take(void) {
    if (g_twin.pool_n > 0) return (uint8_t *)g_twin.pool[--g_twin.pool_n];
    return (uint8_t *)malloc(g_twin.size);
}

static inline void emu_twin_free(void *buf) {
    if (!buf) return;
    /* The hidden board's latest may be in it: keep that. */
    const uint8_t *b = (const uint8_t *)buf;
    if (g_twin.hidden >= b && g_twin.hidden < b + g_twin.size) {
        memcpy(g_twin.stash, g_twin.hidden, g_twin.board);
        g_twin.hidden = g_twin.stash;
    }
    if (g_twin.pool_n < (int)(sizeof g_twin.pool / sizeof g_twin.pool[0])) g_twin.pool[g_twin.pool_n++] = buf;
    else free(buf);
}

static inline uint8_t *emu_twin__half(uint8_t *buf, int b) { return buf + (size_t)b * g_twin.board; }

/* ---- Swapping a board in and out ----------------------------------------- */

static inline void emu_twin__save(emu_thread_ctx_t *ctx, uint8_t *dst, int b) {
    savestate_emu_t e = emu_state_latches(ctx);
    rollback_save_to(dst, &e);
    emu_twin__link_out(b);
}

/* A board in from its snapshot. bus_changed: a real rollback, which the
 * renderer has to hear of; a swap is not one (see the top). */
static inline void emu_twin__load(emu_thread_ctx_t *ctx, const uint8_t *src, int b, bool bus_changed) {
    savestate_emu_t e;
    if (bus_changed) {
        rollback_load(src, ctx->bus, &e);
    } else {
        /* rollback_load less savestate_bus_changed. */
        sound_settle();
        size_t off = 0;
        for (int i = 0; i < g_rollback.n; i++) {
            rollback__copy(g_rollback.part[i].p, src + off, g_rollback.part[i].n);
            off += g_rollback.part[i].n;
        }
        savestate_geo_put(&g_rollback.meta.geo);
        savestate_sound_put(&g_rollback.meta.snd);
        savestate_hle_set(&g_rollback.meta.hle);
        e = g_rollback.meta.emu;
    }
    emu_state_put_latches(ctx, &e);
    emu_attn_bump();
    ctx->cpu_prev_snapshot = *ctx->cpu;
    ctx->cpu_snapshot      = *ctx->cpu;
    emu_twin__link_in(b);
}

/* ---- Inputs ---------------------------------------------------------------- */

/* One cabinet's word from the keys held: both key sets, and the cabinet's
 * SERVICE and TEST. */
static inline uint32_t emu_twin_word(uint32_t held) {
    uint32_t w = 0;
    for (int i = 0; i < 10; i++) {
        if (g_netplay.bit_p1[i] && (held & g_netplay.bit_p1[i])) w |= 1u << i;
        if (g_netplay.bit_p2[i] && (held & g_netplay.bit_p2[i])) w |= 1u << (EMU_TWIN_P2_SHIFT + i);
    }
    if (g_netplay.bit_p1[NP_BIT_SERVICE] && (held & g_netplay.bit_p1[NP_BIT_SERVICE])) w |= 1u << NP_BIT_SERVICE;
    if (g_netplay.bit_p1[NP_BIT_TEST] && (held & g_netplay.bit_p1[NP_BIT_TEST]))       w |= 1u << NP_BIT_TEST;
    return w;
}

static inline void emu_twin_apply(uint32_t w) {
    netplay_apply_inputs(w & 0xFFFu, (w >> EMU_TWIN_P2_SHIFT) & 0x3FFu);
}

/* ---- A frame of both boards ----------------------------------------------- */

/* Slices to the vblank. False if none came (a halted board). */
static inline bool emu_twin__run_board(emu_thread_ctx_t *ctx, uint32_t w) {
    emu_twin_apply(w);
    for (int n = 0; n < 64; n++) {
        emu_slice_body(ctx);
        if (g_vblank_edge || ctx->cpu->halted) break;
    }
    return g_vblank_edge != 0;
}

/* The hidden board's frame count: emu_slice_count_frame's board half. Its
 * frame goes nowhere: no replay, no netplay check, no frame budget. */
static inline void emu_twin__count_hidden(void) {
    g_emu_frames++;
    g_dl_frame_now = g_emu_frames;
    g_frame_clock.sample = g_sound.out_due;
    g_frame_clock.frame  = g_emu_frames;
    g_versus_result = 0;
}

/* Both boards' frame on the two players' words. The live board runs first and
 * gets the frame's bookkeeping (count) once it has; the hidden board then runs
 * in its place, and the two go into a fresh buffer the save hands GGPO. *result
 * is board A's versus result, 0 none. False if the live board had no vblank. */
static inline bool emu_twin_frame(emu_thread_ctx_t *ctx, const uint32_t in[2],
                                  void (*count)(emu_thread_ctx_t *), int *result) {
    int L = g_twin.local, H = 1 - L;
    uint8_t *buf = emu_twin_take();
    if (!buf) return false;
    *result = 0;
    bool ran = emu_twin__run_board(ctx, in[L]);
    if (ran) {
        if (L == 0) *result = g_versus_result;
        count(ctx);
    }
    emu_twin__save(ctx, emu_twin__half(buf, L), L);
    emu_twin__load(ctx, g_twin.hidden, H, false);
    int mute = g_sound_mute;
    g_sound_mute = 1;                   /* settled by the load */
    if (emu_twin__run_board(ctx, in[H])) {
        if (H == 0) *result = g_versus_result;
        emu_twin__count_hidden();
    }
    emu_twin__save(ctx, emu_twin__half(buf, H), H);
    emu_twin__load(ctx, emu_twin__half(buf, L), L, false);   /* settles H's sound */
    g_sound_mute = mute;
    emu_twin__commit();
    if (g_twin.pending) emu_twin_free(g_twin.pending);
    g_twin.pending = buf;
    g_twin.hidden  = emu_twin__half(buf, H);
    return ran;
}

/* Both ends of the link, for the log: up or down, data frames out and in. */
static inline void emu_twin_say(uint32_t frame) {
    m2comm_link_t l[2] = { g_twin.lk.link[0], g_twin.lk.link[1] };
    l[g_twin.local] = g_m2comm_link;
    LOG_INFO("twin: frame %u: A %s %u/%u, B %s %u/%u (frames out/in), %u dropped", frame,
             l[0].alive ? "up" : "down", l[0].sent, l[0].taken,
             l[1].alive ? "up" : "down", l[1].sent, l[1].taken, g_twin.lk.dropped);
}

/* ---- GGPO's buffers ------------------------------------------------------- */

/* The save: the buffer the last frame filled, or (the first save, or one after
 * a load) one made now. */
static inline uint8_t *emu_twin_save(emu_thread_ctx_t *ctx) {
    uint8_t *buf = g_twin.pending;
    g_twin.pending = NULL;
    if (!buf) {
        buf = emu_twin_take();
        if (!buf) return NULL;
        emu_twin__save(ctx, emu_twin__half(buf, g_twin.local), g_twin.local);
        memcpy(emu_twin__half(buf, 1 - g_twin.local), g_twin.hidden, g_twin.board);
        g_twin.hidden = emu_twin__half(buf, 1 - g_twin.local);
    } else {
        emu_twin__link_out(g_twin.local);
    }
    memcpy(buf + 2 * g_twin.board, &g_twin.lk, sizeof g_twin.lk);
    return buf;
}

static inline void emu_twin_load(emu_thread_ctx_t *ctx, const void *data) {
    const uint8_t *buf = (const uint8_t *)data;
    if (g_twin.pending) { emu_twin_free(g_twin.pending); g_twin.pending = NULL; }
    memcpy(&g_twin.lk, buf + 2 * g_twin.board, sizeof g_twin.lk);
    emu_twin__load(ctx, buf + (size_t)g_twin.local * g_twin.board, g_twin.local, true);
    g_twin.hidden = buf + (size_t)(1 - g_twin.local) * g_twin.board;
}

static inline uint32_t emu_twin_checksum(const void *data) {
    const uint8_t *buf = (const uint8_t *)data;
    uint64_t h = rollback_checksum(buf);
    h = (h ^ rollback_checksum(buf + g_twin.board)) * 0x100000001B3ull;
    h = (h ^ savestate_fnv64(buf + 2 * g_twin.board, sizeof(emu_twin_link_t))) * 0x100000001B3ull;
    return (uint32_t)(h ^ (h >> 32));
}

static inline void emu_twin_log(FILE *f, const void *data) {
    const uint8_t *buf = (const uint8_t *)data;
    fprintf(f, "board A\n");
    rollback_log(f, buf);
    fprintf(f, "board B\n");
    rollback_log(f, buf + g_twin.board);
    fprintf(f, "%-12s %8zu %016llx\n", "LINK", sizeof(emu_twin_link_t),
            (unsigned long long)savestate_fnv64(buf + 2 * g_twin.board, sizeof(emu_twin_link_t)));
}

/* ---- Start and stop ------------------------------------------------------- */

/* Whether this profile's GGPO match runs both cabinets. */
static inline bool emu_twin_wanted(const game_profile_t *p) { return p && p->link_twin && p->link_role; }

/* Reset into a role: the board reset with backup RAM `back` kept and its
 * comm board on board b's end of the link (comm_role_reset's power cycle). */
static inline void emu_twin__boot(emu_thread_ctx_t *ctx, const uint8_t *back, int b) {
    g_netplay.reset_board(g_netplay.reset_ctx);
    memcpy(ctx->bus->back, back, BACKUP_RAM_KEEP);
    geodl_snaps_clear();
    input_reset();
    ctx->total_steps       = 0;
    ctx->cpu_prev_snapshot = *ctx->cpu;
    ctx->cpu_snapshot      = *ctx->cpu;
    m2comm_set_peer(&g_twin.peer[b]);
}

/* After emu_ggpo_cold_boot and rollback_layout: the two boards made, the local
 * player's live. False if the ROM made no backup block in time. */
static inline bool emu_twin_start(emu_thread_ctx_t *ctx, const game_profile_t *p, int local) {
    memset(&g_twin.lk, 0, sizeof g_twin.lk);
    g_twin.local   = local;
    g_twin.board   = g_rollback.size;
    g_twin.size    = 2 * g_twin.board + sizeof(emu_twin_link_t);
    g_twin.pending = NULL;
    g_twin.drop_said = 0;
    for (int b = 0; b < 2; b++)
        g_twin.peer[b] = (m2comm_peer_t){ (void *)(intptr_t)b, emu_twin__send, emu_twin__recv, NULL, true };
    g_twin.stash = (uint8_t *)malloc(g_twin.board);
    static uint8_t back[2][BACKUP_RAM_KEEP];
    if (!g_twin.stash) return false;
    /* The boot, alone and unheard, to the frame the ROM has its backup block. */
    m2comm_set_peer(NULL);
    sound_settle();
    g_sound_mute = 1;
    int f = 0;
    for (; f < EMU_TWIN_BOOT_MAX && !p->link_role(ctx->bus, GAME_LINK_MASTER); f++) {
        emu_twin__run_board(ctx, 0);
        emu_twin__count_hidden();
    }
    bool made = f < EMU_TWIN_BOOT_MAX;
    if (made) {
        memcpy(back[0], ctx->bus->back, BACKUP_RAM_KEEP);
        p->link_role(ctx->bus, GAME_LINK_SLAVE);
        memcpy(back[1], ctx->bus->back, BACKUP_RAM_KEEP);
        /* Each board reset in its own role: a role the profile keeps outside
         * backup RAM (Power Sled's unit, the program's bytes) is the last
         * link_role's, and goes with the board from there (a savestate extra). */
        int H = 1 - local;
        p->link_role(ctx->bus, H ? GAME_LINK_SLAVE : GAME_LINK_MASTER);
        emu_twin__boot(ctx, back[H], H);
        emu_twin__save(ctx, g_twin.stash, H);
        g_twin.hidden = g_twin.stash;
        p->link_role(ctx->bus, local ? GAME_LINK_SLAVE : GAME_LINK_MASTER);
        emu_twin__boot(ctx, back[local], local);
    }
    sound_settle();
    g_sound_mute = 0;
    if (!made) {
        LOG_ERROR("twin: the ROM made no backup block in %d frames; no linked match", EMU_TWIN_BOOT_MAX);
        m2comm_set_peer(NULL);
        free(g_twin.stash);
        g_twin.stash = NULL;
        return false;
    }
    g_twin.on = true;
    g_rollback.diff = true;
    LOG_INFO("twin: both cabinets on this machine (backup block at frame %d), playing board %c (%s)",
             f, local ? 'B' : 'A', local ? "SLAVE" : "MASTER");
    return true;
}

/* GGPO has handed every buffer back (ggpo_port_close). The live board goes
 * back to being alone. */
static inline void emu_twin_stop(void) {
    if (!g_twin.on) return;
    if (g_twin.pending) { emu_twin_free(g_twin.pending); g_twin.pending = NULL; }
    while (g_twin.pool_n > 0) free(g_twin.pool[--g_twin.pool_n]);
    free(g_twin.stash);
    g_twin.stash  = NULL;
    g_twin.hidden = NULL;
    g_twin.ui_lock = NULL;
    m2comm_set_peer(NULL);
    g_twin.on = false;
    g_rollback.diff = false;
}

#endif
