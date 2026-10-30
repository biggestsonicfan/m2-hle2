/*
 * emu_multi.h — several boards of one game in one process, side by side
 * (Pinboard #1910, experimental).
 *
 * `--boards N` (2..EMU_MULTI_MAX) runs N cabinets of the loaded set, each its
 * own board from the same power-on, and the window shows them all. The board
 * is the process's globals, so the N take turns in it, as GGPO's twin's two
 * do (core/emu_twin.h): between frames the live board is the one in focus
 * (the one the keyboard, the speakers, the debugger windows and the bridge
 * see); each other one waits in a slot of its own, a rollback snapshot.
 *
 * The UI thread runs them, not the emu thread: a board's picture has to be
 * drawn while that board is in the bus, and the GPU is the UI thread's. So
 * once the boards are made the emu thread only naps (emu_run_running), and
 * each host frame due a board frame (60 Hz) the window, emu mutex held, runs
 * a round: for each board, in from its slot, one frame, its picture drawn
 * into a target of its own (the frontend's `drawn` callback), out to its slot.
 * The round starts with the live board and ends with the one in focus live.
 * A round copies each board in and out once: 2 x N x ~7 MB.
 *
 * Linked. A game whose profile can set a cabinet's role (link_role: Daytona,
 * Virtual-On, Power Sled) gets its boards wired in a ring, as the real
 * cabinets' comm boards are with their TOSlink cables: board 0 is the MASTER,
 * every other one a SLAVE, and board b's sends are board b+1's to take
 * (MAME's ring, board/m2comm.h, m2comm_set_mame_frames: the whole frame, so a
 * ring of more than two works). What a board sends in a round, the next one
 * takes from the next round, so the order the boards run in changes nothing.
 * Each board's g_m2comm_link (outside savestates) goes in and out with it.
 *
 * The battery is detached for the run: the boards' roles go into backup RAM,
 * and the player's own file must not get a SLAVE's.
 *
 * Not a netplay mode and not part of the board: nothing here runs in the
 * det_digest, web or libretro builds, and a single board never comes here.
 * Header-only; included from emu_thread.h. UI thread, emu mutex held.
 */
#ifndef M2HLE_EMU_MULTI_H
#define M2HLE_EMU_MULTI_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "m2comm.h"
#include "rollback.h"
#include "backup_ram.h"

#define EMU_MULTI_MAX      8
#define EMU_MULTI_QUEUE    16u    /* frames waiting for a board; the ring moves one a round */
#define EMU_MULTI_BOOT_MAX 1800   /* frames the cold boot may take to make its backup block */

typedef struct {
    uint32_t head, n;
    uint16_t len[EMU_MULTI_QUEUE];
    uint8_t  msg[EMU_MULTI_QUEUE][M2COMM_MSG_MAX];
} emu_multi_queue_t;

static struct {
    bool              on;
    int               n;
    int               live;        /* the board in the bus */
    int               focus;       /* the player's: live between rounds */
    bool              linked;
    size_t            board;       /* one board's bytes, g_rollback.size */
    uint8_t          *slot[EMU_MULTI_MAX];
    uint32_t          held[EMU_MULTI_MAX];   /* the keys of a board not in focus */
    emu_multi_queue_t ready[EMU_MULTI_MAX];  /* board b takes from ready[b] */
    emu_multi_queue_t sent[EMU_MULTI_MAX];   /* this round's for board b */
    m2comm_link_t     link[EMU_MULTI_MAX];   /* each board's g_m2comm_link, peer NULL */
    m2comm_peer_t     peer[EMU_MULTI_MAX];
    uint32_t        (*op)(struct memory_bus *bus, uint32_t held);   /* the Main's operator, ring_op or link_solo */
    uint32_t          dropped, drop_said;
    uint64_t          rounds;
    int64_t           round_us;    /* the last round's time, for the window's title */
} g_multi;

static int g_multi_want = 0;       /* --boards N; -1 `all`: the game's ring */

/* The boards `--boards` gets for profile p: a linked game's ring holds at most
 * link_cabinets (two when unset), `all` is that many, an unlinked game's
 * boards are as many as asked (`all`: two). */
static inline int emu_multi_count(const game_profile_t *p, int want) {
    int ring = !p || !(p->ring_role || p->link_role) ? 0 : p->link_cabinets ? p->link_cabinets : 2;
    if (ring > EMU_MULTI_MAX) ring = EMU_MULTI_MAX;
    if (want < 0) return ring ? ring : 2;
    return ring && want > ring ? ring : want;
}

/* The profile's role for a board of the ring, or NULL: no link. */
static inline game_link_role_fn emu_multi__role_fn(const game_profile_t *p) {
    return p->ring_role ? p->ring_role : p->link_role;
}

static inline bool emu_multi_on(void) { return g_multi.on; }

/* ---- The ring -------------------------------------------------------------- */

static void emu_multi__send(void *ctx, const uint8_t *msg, uint32_t len) {
    int to = ((int)(intptr_t)ctx + 1) % g_multi.n;
    emu_multi_queue_t *q = &g_multi.sent[to];
    if (q->n >= EMU_MULTI_QUEUE || len > M2COMM_MSG_MAX) { g_multi.dropped++; return; }
    uint32_t at = (q->head + q->n++) % EMU_MULTI_QUEUE;
    q->len[at] = (uint16_t)len;
    memcpy(q->msg[at], msg, len);
}

static uint32_t emu_multi__recv(void *ctx, uint8_t *msg, uint32_t cap) {
    emu_multi_queue_t *q = &g_multi.ready[(int)(intptr_t)ctx];
    if (q->n == 0) return 0;
    uint32_t len = q->len[q->head];
    if (len > cap) len = cap;
    memcpy(msg, q->msg[q->head], len);
    q->head = (q->head + 1) % EMU_MULTI_QUEUE;
    q->n--;
    return len;
}

/* The round's sends, ready for the next round. */
static inline void emu_multi__commit(void) {
    for (int b = 0; b < g_multi.n; b++) {
        emu_multi_queue_t *from = &g_multi.sent[b], *to = &g_multi.ready[b];
        for (; from->n; from->n--, from->head = (from->head + 1) % EMU_MULTI_QUEUE) {
            if (to->n >= EMU_MULTI_QUEUE) { g_multi.dropped++; continue; }
            uint32_t at = (to->head + to->n++) % EMU_MULTI_QUEUE;
            to->len[at] = from->len[from->head];
            memcpy(to->msg[at], from->msg[from->head], from->len[from->head]);
        }
        from->head = 0;
    }
    if (g_multi.dropped != g_multi.drop_said) {
        g_multi.drop_said = g_multi.dropped;
        LOG_WARN("multi: %u comm frame(s) dropped on a full queue", g_multi.dropped);
    }
}

/* ---- A board in and out ---------------------------------------------------- */

static inline void emu_multi__out(emu_thread_ctx_t *ctx, int b) {
    savestate_emu_t e = emu_state_latches(ctx);
    rollback_save_to(g_multi.slot[b], &e);
    g_multi.link[b] = g_m2comm_link;
    g_multi.link[b].peer = NULL;
}

/* rollback_load less savestate_bus_changed: the renderer's caches are the
 * boards' own (the frontend keeps one set per board), and a board's writes
 * mark what they change as it runs. */
static inline void emu_multi__in(emu_thread_ctx_t *ctx, int b) {
    sound_settle();
    const uint8_t *src = g_multi.slot[b];
    size_t off = 0;
    for (int i = 0; i < g_rollback.n; i++) {
        rollback__copy(g_rollback.part[i].p, src + off, g_rollback.part[i].n);
        off += g_rollback.part[i].n;
    }
    savestate_geo_put(&g_rollback.meta.geo);
    savestate_sound_put(&g_rollback.meta.snd);
    savestate_hle_set(&g_rollback.meta.hle);
    savestate_emu_t e = g_rollback.meta.emu;
    emu_state_put_latches(ctx, &e);
    emu_attn_bump();
    ctx->cpu_prev_snapshot = *ctx->cpu;
    ctx->cpu_snapshot      = *ctx->cpu;
    g_m2comm_link = g_multi.link[b];
    g_m2comm_link.peer = g_multi.linked ? &g_multi.peer[b] : NULL;
}

static inline void emu_multi__switch(emu_thread_ctx_t *ctx, int b) {
    if (b == g_multi.live) return;
    emu_multi__out(ctx, g_multi.live);
    emu_multi__in(ctx, b);
    g_multi.live = b;
}

/* ---- A round --------------------------------------------------------------- */

/* Slices to the vblank. False if none came (a halted board). */
static inline bool emu_multi__run_board(emu_thread_ctx_t *ctx) {
    for (int n = 0; n < 64; n++) {
        emu_slice_body(ctx);
        if (g_vblank_edge || ctx->cpu->halted) break;
    }
    return g_vblank_edge != 0;
}

/* A board out of focus: its frame counted, and nothing else heard of it. */
static inline void emu_multi__count_other(void) {
    g_emu_frames++;
    g_dl_frame_now = g_emu_frames;
    g_frame_clock.sample = g_sound.out_due;
    g_frame_clock.frame  = g_emu_frames;
    g_versus_result = 0;
}

/* One frame of every board. drawn(user, b) runs with board b in the bus, just
 * after its frame: the frontend draws its picture there. */
static inline void emu_multi_round(emu_thread_ctx_t *ctx, void (*drawn)(void *user, int b), void *user) {
    if (!g_multi.on) return;
    int64_t t0 = emu_now_us();
    int start = g_multi.live, mute = g_sound_mute;
    for (int i = 0; i < g_multi.n; i++) {
        int b = (start + i) % g_multi.n;
        emu_multi__switch(ctx, b);
        bool mine = b == g_multi.focus;
        g_sound_mute = mine ? mute : 1;     /* settled by the next switch */
        if (mine) netplay_release_inputs();
        else {
            /* A Main nobody plays is worked by the profile's operator from the
             * player's keys (Power Sled's ring_op), as GGPO's SOLO does. */
            uint32_t held = g_multi.held[b];
            if (b == 0 && g_multi.linked && g_multi.op) held = g_multi.op(ctx->bus, g_input.held);
            g_input.net_held = held; g_input.use_net = 1;
        }
        bool ran = emu_multi__run_board(ctx);
        if (ran) {
            if (mine) emu_slice_count_frame(ctx);
            else      emu_multi__count_other();
        }
        if (drawn) drawn(user, b);
    }
    emu_multi__switch(ctx, g_multi.focus);
    g_sound_mute = mute;
    netplay_release_inputs();
    emu_multi__commit();
    g_multi.rounds++;
    g_multi.round_us = emu_now_us() - t0;
}

/* The board the keyboard, the speakers and the tools get, from the next round. */
static inline void emu_multi_set_focus(int b) {
    if (g_multi.on && b >= 0 && b < g_multi.n) g_multi.focus = b;
}

/* ---- Start and stop ---------------------------------------------------------- */

/* The board back to power-on with backup RAM `back` kept (net/comm_role.h's
 * power cycle) and its comm board on board b's end of the ring. */
static inline void emu_multi__boot(emu_thread_ctx_t *ctx, const uint8_t *back, int b) {
    g_netplay.reset_board(g_netplay.reset_ctx);
    if (back) memcpy(ctx->bus->back, back, BACKUP_RAM_KEEP);
    if (back && b && g_multi.linked && g_active_profile && g_active_profile->ring_seat)
        g_active_profile->ring_seat(ctx->bus, b - 1);   /* this slave's place: a ride's colour */
    geodl_snaps_clear();
    input_reset();
    ctx->total_steps       = 0;
    ctx->cpu_prev_snapshot = *ctx->cpu;
    ctx->cpu_snapshot      = *ctx->cpu;
    ctx->frame_deadline_us = 0;
    m2comm_set_mame_frames(g_multi.linked);
    m2comm_set_peer(g_multi.linked ? &g_multi.peer[b] : NULL);
}

static inline void emu_multi__free(void) {
    for (int b = 0; b < EMU_MULTI_MAX; b++) { free(g_multi.slot[b]); g_multi.slot[b] = NULL; }
}

/* n boards of the loaded set, from power-on; board 0 live and in focus.
 * Linked when the profile can set the roles (and `link` asks for it). */
static inline bool emu_multi_start(emu_thread_ctx_t *ctx, const game_profile_t *p, int n, bool link) {
    if (g_multi.on || n < 2 || n > EMU_MULTI_MAX || !p) return false;
    if (!g_netplay.reset_board) { LOG_ERROR("multi: no board-reset hook"); return false; }
#ifdef M2HLE_GGPO
    if (g_ggpo.on) emu_ggpo_stop();   /* SOLO's twin: the boards are ours now */
#endif
    memset(&g_multi, 0, sizeof g_multi);
    g_multi.n = n;
    g_multi.linked = link && emu_multi__role_fn(p);
    g_multi.op = !g_multi.linked ? NULL : p->ring_op ? p->ring_op : p->link_solo;
    game_link_role_fn role = emu_multi__role_fn(p);
    replay_rec_break("multi");
    follow_lead_break("multi");
    backup_ram_detach();
    rollback_layout(ctx->cpu, ctx->bus);
    g_multi.board = g_rollback.size;
    for (int b = 0; b < n; b++) {
        g_multi.slot[b] = (uint8_t *)malloc(g_multi.board);
        if (!g_multi.slot[b]) { emu_multi__free(); backup_ram_reattach(); return false; }
        g_multi.peer[b] = (m2comm_peer_t){ (void *)(intptr_t)b, emu_multi__send, emu_multi__recv, NULL, true };
    }
    g_netplay.reset_board(g_netplay.reset_ctx);
    static uint8_t back[2][BACKUP_RAM_KEEP];   /* MASTER's, SLAVE's */
    int f = 0;
    if (g_multi.linked) {
        /* The boot, alone and unheard, to the frame the ROM has its backup
         * block; then each board reset into its role (emu_twin_start's way). */
        m2comm_set_peer(NULL);
        sound_settle();
        int mute = g_sound_mute;
        g_sound_mute = 1;
        for (; f < EMU_MULTI_BOOT_MAX && !role(ctx->bus, GAME_LINK_MASTER); f++) {
            emu_multi__run_board(ctx);
            emu_multi__count_other();
        }
        sound_settle();
        g_sound_mute = mute;
        if (f == EMU_MULTI_BOOT_MAX) {
            LOG_ERROR("multi: the ROM made no backup block in %d frames; the boards run unlinked", f);
            g_multi.linked = false;
        } else {
            memcpy(back[0], ctx->bus->back, BACKUP_RAM_KEEP);
            role(ctx->bus, GAME_LINK_SLAVE);
            memcpy(back[1], ctx->bus->back, BACKUP_RAM_KEEP);
        }
    }
    /* Every board but 0 made and put away, then 0, which stays in the bus.
     * A role the profile keeps outside backup RAM (Power Sled's unit) is the
     * last link_role's, and goes with the board from there. */
    for (int b = n - 1; b >= 0; b--) {
        if (g_multi.linked) role(ctx->bus, b ? GAME_LINK_SLAVE : GAME_LINK_MASTER);
        emu_multi__boot(ctx, g_multi.linked ? back[b ? 1 : 0] : NULL, b);
        if (b) emu_multi__out(ctx, b);
    }
    g_multi.live  = 0;
    g_multi.focus = g_multi.op ? 1 : 0;   /* a Main with an operator works itself: the player rides */
    g_multi.on = true;
    if (g_multi.linked)
        LOG_INFO("multi: %d linked %s cabinets in a ring, board 1 the MASTER (backup block at frame %d); "
                 "%zu bytes a board", n, p->id, f, g_multi.board);
    else
        LOG_INFO("multi: %d %s boards side by side, unlinked; %zu bytes a board", n, p->id, g_multi.board);
    return true;
}

/* The board in focus goes on alone; the others are gone. */
static inline void emu_multi_stop(emu_thread_ctx_t *ctx) {
    if (!g_multi.on) return;
    (void)ctx;
    sound_settle();
    netplay_release_inputs();
    emu_multi__free();
    g_m2comm_link.peer = NULL;
    m2comm_set_mame_frames(false);
    m2comm_set_peer(NULL);
    rollback_drain();
    backup_ram_reattach();
    LOG_INFO("multi: over after %llu rounds; board %d goes on alone",
             (unsigned long long)g_multi.rounds, g_multi.focus + 1);
    g_multi.on = false;
}

/* The ring's state, for the log and the window. */
static inline void emu_multi_say(void) {
    if (!g_multi.on || !g_multi.linked) return;
    char line[256];
    int k = 0;
    for (int b = 0; b < g_multi.n && k < (int)sizeof line - 32; b++) {
        const m2comm_link_t *l = b == g_multi.live ? &g_m2comm_link : &g_multi.link[b];
        k += snprintf(line + k, sizeof line - (size_t)k, " %d:%s id %u/%u %u/%u", b + 1,
                      l->alive ? "up" : "down", l->id, l->count, l->sent, l->taken);
    }
    LOG_INFO("multi: round %llu:%s, %u dropped", (unsigned long long)g_multi.rounds, line, g_multi.dropped);
}

#endif
