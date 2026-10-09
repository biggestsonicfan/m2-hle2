/*
 * emu_ggpo.h — rollback netplay through GGPO (Pinboard #575).
 *
 * The other way to play online. The RPCN lockstep (net/netplay.h) waits for
 * the peer's input before it runs a frame; GGPO runs the frame at once on a
 * guess of the peer's input and, when the real input arrives and differs, goes
 * back to the last frame both agreed on and runs forward again. That needs the
 * whole board saved every frame and put back on demand: core/rollback.h, the
 * savestate's parts copied raw.
 *
 * A session is a COLD BOOT on both machines, as an RPCN session is: the same
 * board reset (netplay_do_reset's), and every frame from power-on is GGPO's.
 * Both players' inputs are one canonical word each (net/netplay.h's bit
 * order), composed into the board's held mask the same way.
 *
 * One GGPO frame is one board frame, slices run to the vblank. Everything GGPO
 * does happens inside ggpo_idle / ggpo_advance_frame, under the emu mutex: a
 * rollback is load_game_state, then advance_frame for every frame again. Those
 * frames' samples were heard the first time, so the sound board runs muted
 * through them (g_sound_mute), its clock still moving.
 *
 * --ggpo-synctest N runs GGPO's own determinism test instead of a session:
 * both players are this machine's, and every frame is rolled back N frames
 * and run again, the snapshot's checksum held against the first run. A
 * mismatch is counted (ggpo_port_sync_errors) and, with --ggpo-synclog DIR,
 * both snapshots are written there part by part (rollback_log).
 *
 * GGPO is the ggpo CMake target, which defines M2HLE_GGPO: the desktop, the
 * web build and the libretro core. The desktop's emu thread runs a session
 * through emu_ggpo_step; the web build and the libretro core, which step the
 * board from the host's frame callback, through emu_ggpo_tick, and pace
 * themselves. The handheld, the tests and det_digest get the stubs at the
 * bottom.
 *
 * Header-only. Included from emu_thread.h; emu thread (the host's one thread
 * where there is no emu thread).
 */
#ifndef M2HLE_EMU_GGPO_H
#define M2HLE_EMU_GGPO_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { EMU_GGPO_OFF = 0, EMU_GGPO_P2P, EMU_GGPO_SYNCTEST } emu_ggpo_mode_t;

/* What the command line asked for. Read when the emu thread starts running. */
typedef struct {
    emu_ggpo_mode_t mode;
    uint16_t        local_port;
    char            remote_ip[64];
    uint16_t        remote_port;
    int             player;            /* 1 or 2 */
    int             delay;             /* local input delay, frames */
    int             synctest_frames;   /* how far synctest rolls back */
    char            synclog_dir[240];
} emu_ggpo_cfg_t;

static emu_ggpo_cfg_t g_ggpo_cfg = { EMU_GGPO_OFF, 7000, "", 7001, 1, 2, 1, "" };

#ifdef M2HLE_GGPO
#include "../net/ggpo_lobby.h"   /* the lobby that sets up a match (ggpo.sonicthefighte.rs) */
#endif

/* "IP:PORT" into the config; false if it is not one. */
static inline bool emu_ggpo__remote(const char *v) {
    const char *colon = strrchr(v, ':');
    if (!colon || colon == v || (size_t)(colon - v) >= sizeof g_ggpo_cfg.remote_ip) return false;
    int port = atoi(colon + 1);
    if (port <= 0 || port > 65535) return false;
    memcpy(g_ggpo_cfg.remote_ip, v, (size_t)(colon - v));
    g_ggpo_cfg.remote_ip[colon - v] = '\0';
    g_ggpo_cfg.remote_port = (uint16_t)port;
    return true;
}

/* The command line's --ggpo-* flags; true when argv[*i] was one (and its value
 * taken). Parsed in every build, so a build without GGPO can say so. */
static inline bool emu_ggpo_cli_arg(int argc, char **argv, int *i) {
#ifdef M2HLE_GGPO
    if (ggl_cli_arg(argc, argv, i)) return true;
#endif
    const char *a = argv[*i];
    if (strncmp(a, "--ggpo-", 7) != 0 || *i + 1 >= argc) return false;
    const char *v = argv[++*i];
    a += 7;
    if (strcmp(a, "local") == 0)          g_ggpo_cfg.local_port = (uint16_t)atoi(v);
    else if (strcmp(a, "remote") == 0) {
        if (emu_ggpo__remote(v)) g_ggpo_cfg.mode = EMU_GGPO_P2P;
        else LOG_WARN("--ggpo-remote %s: expected IP:PORT", v);
    }
    else if (strcmp(a, "player") == 0)    g_ggpo_cfg.player = atoi(v) == 2 ? 2 : 1;
    else if (strcmp(a, "delay") == 0)     g_ggpo_cfg.delay = atoi(v);
    else if (strcmp(a, "synctest") == 0) {
        g_ggpo_cfg.mode = EMU_GGPO_SYNCTEST;
        g_ggpo_cfg.synctest_frames = atoi(v) > 0 ? atoi(v) : 1;
    }
    else if (strcmp(a, "synclog") == 0)   snprintf(g_ggpo_cfg.synclog_dir, sizeof g_ggpo_cfg.synclog_dir, "%s", v);
    else { --*i; return false; }
#ifndef M2HLE_GGPO
    LOG_WARN("--ggpo-%s: this build has no GGPO (M2HLE_GGPO off)", a);
    g_ggpo_cfg.mode = EMU_GGPO_OFF;
#endif
    return true;
}

#ifdef M2HLE_GGPO

#include "../net/ggpo_port/ggpo_port.h"
#include "rollback.h"

#define EMU_GGPO_INPUT_SIZE  ((int)sizeof(uint32_t))   /* one word a player */
#define EMU_GGPO_SKIP_MAX    8                         /* frames a timesync may hold us */

static struct {
    bool              on;          /* a session is open */
    bool              running;     /* GGPO said RUNNING */
    bool              resim;       /* inside a rollback's advance_frame */
    int               skip;        /* frames to hold for a timesync */
    emu_thread_ctx_t *ctx;
    uint32_t          frames;      /* frames run live */
    uint32_t          rollbacks;   /* loads */
    uint32_t          resim_frames;
    uint32_t          logged_errors;
    int               check_frame; /* the last check frame saved, -1 none */
    uint32_t          check_sum;
    char              check_line[96]; /* the last check logged, for a host with its own log */
    uint32_t          check_seq;   /* bumped with each one */
} g_ggpo;

/* Every EMU_GGPO_CHECK_EVERY frames a session logs the board's checksum, taken
 * when that frame was last saved; by EMU_GGPO_CHECK_LAG frames later no
 * rollback can reach it, so both machines' lines must agree. */
#define EMU_GGPO_CHECK_EVERY 600
#define EMU_GGPO_CHECK_LAG   30

static inline bool emu_ggpo_active(void) { return g_ggpo.on; }

/* One side's word from one key set; the cabinet's service/test only on 1P. */
static inline uint32_t emu_ggpo__word(uint32_t held, const uint32_t *bits, bool cabinet) {
    uint32_t word = 0;
    for (int i = 0; i < 10; i++)
        if (bits[i] && (held & bits[i])) word |= 1u << i;
    if (cabinet) {
        if (g_netplay.bit_p1[NP_BIT_SERVICE] && (held & g_netplay.bit_p1[NP_BIT_SERVICE]))
            word |= 1u << NP_BIT_SERVICE;
        if (g_netplay.bit_p1[NP_BIT_TEST] && (held & g_netplay.bit_p1[NP_BIT_TEST]))
            word |= 1u << NP_BIT_TEST;
    }
    return word;
}

/* This machine's player: either key set plays our side (netplay_sample_local). */
static inline uint32_t emu_ggpo_sample_local(void) {
    uint32_t held = g_input.held;
    bool cabinet = g_ggpo_cfg.player == 1;
    return emu_ggpo__word(held, g_netplay.bit_p1, cabinet) | emu_ggpo__word(held, g_netplay.bit_p2, false);
}

/* One board frame on these two words: slices to the vblank, then its count. */
static inline bool emu_ggpo_run_frame(emu_thread_ctx_t *ctx, const uint32_t in[2]) {
    netplay_apply_inputs(in[0], in[1]);
    for (int n = 0; n < 64; n++) {
        emu_slice_body(ctx);
        if (g_vblank_edge || ctx->cpu->halted) break;
    }
    if (!g_vblank_edge) return false;
    uint32_t budget = ctx->frame_budget;
    if (g_ggpo.resim) ctx->frame_budget = 0;   /* a run_frames counts frames once */
    emu_slice_count_frame(ctx);
    if (g_ggpo.resim) ctx->frame_budget = budget;
    return true;
}

/* ---- GGPO's callbacks ---------------------------------------------------- */

/* With --ggpo-synclog, each check frame's parts one by one, so two
 * machines' files say which part split. A re-save overwrites it. */
static inline void emu_ggpo_check_file(int frame, const void *buf) {
    if (!g_ggpo_cfg.synclog_dir[0]) return;
    char path[300];
    snprintf(path, sizeof path, "%s/check-%05d.log", g_ggpo_cfg.synclog_dir, frame);
    FILE *f = fopen(path, "w");
    if (!f) return;
    rollback_log(f, buf);
    fclose(f);
}

static void *emu_ggpo_cb_save(void *ud, int *len, int *checksum, int frame) {
    emu_thread_ctx_t *ctx = (emu_thread_ctx_t *)ud;
    (void)frame;
    savestate_emu_t e = emu_state_latches(ctx);
    void *buf = rollback_save(&e);
    *len = buf ? (int)g_rollback.size : 0;
    /* Only synctest reads it, and it is a pass over 16 MB. */
    *checksum = (buf && g_ggpo_cfg.mode == EMU_GGPO_SYNCTEST) ? (int)rollback_checksum(buf) : 0;
    if (buf && g_ggpo_cfg.mode == EMU_GGPO_P2P && frame > 0 && frame % EMU_GGPO_CHECK_EVERY == 0) {
        g_ggpo.check_frame = frame;
        g_ggpo.check_sum = rollback_checksum(buf);
        emu_ggpo_check_file(frame, buf);
    }
    return buf;
}

static bool emu_ggpo_cb_load(void *ud, const void *buf, int len) {
    emu_thread_ctx_t *ctx = (emu_thread_ctx_t *)ud;
    if ((size_t)len != g_rollback.size) return false;
    savestate_emu_t e;
    rollback_load(buf, ctx->bus, &e);          /* settles the sound thread first */
    emu_state_put_latches(ctx, &e);
    emu_attn_bump();
    ctx->cpu_prev_snapshot = *ctx->cpu;
    ctx->cpu_snapshot      = *ctx->cpu;
    g_sound_mute = 1;                          /* until the frames again are run */
    g_ggpo.rollbacks++;
    return true;
}

static void emu_ggpo_cb_free(void *ud, void *buf) { (void)ud; rollback_free(buf); }

static bool emu_ggpo_cb_advance(void *ud) {
    emu_thread_ctx_t *ctx = (emu_thread_ctx_t *)ud;
    uint32_t in[2] = { 0, 0 };
    int disc = 0;
    if (ggpo_port_sync_input(in, (int)sizeof in, &disc) != GGPO_PORT_OK) return false;
    g_ggpo.resim = true;
    emu_ggpo_run_frame(ctx, in);
    g_ggpo.resim = false;
    g_ggpo.resim_frames++;
    ggpo_port_advance_frame();
    return true;
}

static void emu_ggpo_cb_log(void *ud, const char *filename, const void *buf, int len) {
    (void)ud;
    FILE *f = NULL;
    if ((size_t)len != g_rollback.size || ggpo_port_fopen(&f, filename, "w") != 0 || !f) return;
    rollback_log(f, buf);
    fclose(f);
}

static void emu_ggpo_cb_event(void *ud, int code, int handle, int a, int b) {
    (void)ud; (void)handle;
    /* GGPO's handle is the order players were added in; the only peer a
     * session has is the other player. */
    int player = g_ggpo_cfg.player == 1 ? 1 : 0;
    switch (code) {
    case GGPO_PORT_EV_CONNECTED:     LOG_INFO("ggpo: connected to player %d", player + 1); break;
    case GGPO_PORT_EV_SYNCHRONIZING: break;
    case GGPO_PORT_EV_SYNCHRONIZED:  LOG_INFO("ggpo: synchronized with player %d", player + 1); break;
    case GGPO_PORT_EV_RUNNING:       LOG_INFO("ggpo: running"); g_ggpo.running = true; break;
    case GGPO_PORT_EV_DISCONNECTED:
        LOG_WARN("ggpo: player %d disconnected; the session ends", player + 1);
        g_ggpo_cfg.mode = EMU_GGPO_OFF;
        break;
    case GGPO_PORT_EV_TIMESYNC:
        g_ggpo.skip = a < EMU_GGPO_SKIP_MAX ? a : EMU_GGPO_SKIP_MAX;
        break;
    case GGPO_PORT_EV_INTERRUPTED:   LOG_WARN("ggpo: player %d silent; dropped in %d ms", player + 1, a); break;
    case GGPO_PORT_EV_RESUMED:       LOG_INFO("ggpo: player %d back", player + 1); break;
    default: break;
    }
    (void)b;
}

/* ---- Start and stop ------------------------------------------------------ */

/* The cold boot both machines make: netplay_do_reset's, and emu_netplay_pump's
 * bookkeeping after it. Mutex held. False without a reset hook. */
static inline bool emu_ggpo_cold_boot(emu_thread_ctx_t *ctx) {
    if (!g_netplay.reset_board) return false;
    replay_rec_break("ggpo session");
    follow_lead_break("ggpo session");
    backup_ram_detach();
    if (g_hle_extra_session_off) g_hle_extra_session_off();
    g_netplay.reset_board(g_netplay.reset_ctx);
    geodl_snaps_clear();
    input_reset();
    ctx->total_steps       = 0;
    ctx->cpu_prev_snapshot = *ctx->cpu;
    ctx->cpu_snapshot      = *ctx->cpu;
    ctx->frame_deadline_us = 0;
    return true;
}

static inline int emu_ggpo_open(emu_thread_ctx_t *ctx) {
    ggpo_port_cb_t cb = { ctx, emu_ggpo_cb_save, emu_ggpo_cb_load, emu_ggpo_cb_free,
                          emu_ggpo_cb_advance, emu_ggpo_cb_log, emu_ggpo_cb_event };
    const emu_ggpo_cfg_t *c = &g_ggpo_cfg;
    ggpo_port_reset_errors();
    ggpo_port_set_synclog_dir(c->synclog_dir);
    if (c->mode == EMU_GGPO_SYNCTEST)
        return ggpo_port_start_synctest(&cb, EMU_GGPO_INPUT_SIZE, c->synctest_frames);
    int err = ggpo_port_start_p2p(&cb, EMU_GGPO_INPUT_SIZE, c->local_port, c->player,
                                  c->remote_ip, c->remote_port);
    if (err == GGPO_PORT_OK) {
        ggpo_port_set_frame_delay(c->delay);
        ggpo_port_set_disconnect_timeout(5000);
    }
    return err;
}

/* Mutex held. */
static inline bool emu_ggpo_start(emu_thread_ctx_t *ctx) {
    memset(&g_ggpo, 0, sizeof g_ggpo);
    g_ggpo.ctx = ctx;
    g_ggpo.check_frame = -1;
    netplay_build_masks(g_active_profile);
    if (!emu_ggpo_cold_boot(ctx)) {
        LOG_ERROR("ggpo: no board-reset hook; not starting");
        return false;
    }
    rollback_layout(ctx->cpu, ctx->bus);
    int err = emu_ggpo_open(ctx);
    if (err != GGPO_PORT_OK) {
        LOG_ERROR("ggpo: could not start the session (GGPO error %d)", err);
        rollback_drain();
        return false;
    }
    g_ggpo.on = true;
    if (g_ggpo_cfg.mode == EMU_GGPO_SYNCTEST)
        LOG_INFO("ggpo: synctest, rolling back %d frame(s) every frame; snapshot %zu bytes",
                 g_ggpo_cfg.synctest_frames, g_rollback.size);
    else if (!ggpo_port_bound())
        LOG_INFO("ggpo: player %d through the lobby's transport, delay %d; snapshot %zu bytes",
                 g_ggpo_cfg.player, g_ggpo_cfg.delay, g_rollback.size);
    else
        LOG_INFO("ggpo: player %d on UDP %u, peer %s:%u, delay %d; snapshot %zu bytes",
                 g_ggpo_cfg.player, (unsigned)g_ggpo_cfg.local_port, g_ggpo_cfg.remote_ip,
                 (unsigned)g_ggpo_cfg.remote_port, g_ggpo_cfg.delay, g_rollback.size);
    return true;
}

/* Mutex held. The board goes back to the keyboard and the player's battery. */
static inline void emu_ggpo_stop(void) {
    if (!g_ggpo.on) return;
    ggpo_port_close();      /* hands GGPO's saved frames back through free_buf */
    rollback_drain();
    sound_settle();
    g_sound_mute = 0;
    netplay_release_inputs();
    backup_ram_reattach();
    LOG_INFO("ggpo: session over: %u frames, %u rollbacks, %u frames run again, %u sync errors",
             g_ggpo.frames, g_ggpo.rollbacks, g_ggpo.resim_frames, ggpo_port_sync_errors());
    g_ggpo.on = false;
}

/* ---- The run loop's step ------------------------------------------------- */

/* A rollback's frames are over: what the sound thread made of them is muted,
 * what comes next is heard. Mutex held. */
static inline void emu_ggpo_unmute(void) {
    if (!g_sound_mute) return;
    sound_settle();
    g_sound_mute = 0;
}

/* Synctest's mismatches as they come, not one line a frame. */
static inline void emu_ggpo_report_errors(void) {
    uint32_t n = ggpo_port_sync_errors();
    if (n == g_ggpo.logged_errors) return;
    if (g_ggpo.logged_errors != 0 && n - g_ggpo.logged_errors < 60) return;
    LOG_WARN("ggpo: synctest: %u sync error(s), frame %u: %s", n, g_ggpo.frames, ggpo_port_last_error());
    g_ggpo.logged_errors = n;
}

static inline void emu_ggpo_report_check(void) {
    if (g_ggpo.check_frame < 0 || g_ggpo.frames < (uint32_t)g_ggpo.check_frame + EMU_GGPO_CHECK_LAG) return;
    snprintf(g_ggpo.check_line, sizeof g_ggpo.check_line, "ggpo: frame %d board %08x (%u rollbacks, %u frames again)",
             g_ggpo.check_frame, g_ggpo.check_sum, g_ggpo.rollbacks, g_ggpo.resim_frames);
    LOG_INFO("%s", g_ggpo.check_line);
    g_ggpo.check_seq++;
    g_ggpo.check_frame = -1;
}

/* This frame's inputs in, and the frame run. Mutex held. True when it ran. */
static inline bool emu_ggpo_frame(emu_thread_ctx_t *ctx) {
    ggpo_port_idle(0);
    emu_ggpo_unmute();
    int err;
    if (g_ggpo_cfg.mode == EMU_GGPO_SYNCTEST) {
        uint32_t held = g_input.held;
        uint32_t w0 = emu_ggpo__word(held, g_netplay.bit_p1, true);
        uint32_t w1 = emu_ggpo__word(held, g_netplay.bit_p2, false);
        err = ggpo_port_add_local_input(0, &w0, EMU_GGPO_INPUT_SIZE);
        if (err == GGPO_PORT_OK) err = ggpo_port_add_local_input(1, &w1, EMU_GGPO_INPUT_SIZE);
    } else {
        uint32_t w = emu_ggpo_sample_local();
        err = ggpo_port_add_local_input(0, &w, EMU_GGPO_INPUT_SIZE);
    }
    if (err != GGPO_PORT_OK) return false;     /* not running yet, or too far ahead */
    uint32_t in[2] = { 0, 0 };
    int disc = 0;
    if (ggpo_port_sync_input(in, (int)sizeof in, &disc) != GGPO_PORT_OK) return false;
    bool ran = emu_ggpo_run_frame(ctx, in);
    ggpo_port_advance_frame();
    emu_ggpo_unmute();
    g_ggpo.frames++;
    emu_ggpo_report_errors();
    emu_ggpo_report_check();
    return ran;
}

/* One pass of a session for a host that paces itself (the web build, the
 * libretro core): opened or closed as wanted, then this frame run, unless a
 * timesync holds it. Takes the emu mutex. */
typedef enum { EMU_GGPO_TICK_IDLE, EMU_GGPO_TICK_HELD, EMU_GGPO_TICK_RAN } emu_ggpo_tick_t;

static inline emu_ggpo_tick_t emu_ggpo_tick(emu_thread_ctx_t *ctx) {
    emu_ggpo_tick_t r = EMU_GGPO_TICK_HELD;
    emu_mutex_lock(&ctx->mutex);
    if (g_ggpo_cfg.mode == EMU_GGPO_OFF || !g_ggpo.on) {
        bool go = g_ggpo_cfg.mode != EMU_GGPO_OFF && emu_ggpo_start(ctx);
        if (!go) {
            emu_ggpo_stop();
            g_ggpo_cfg.mode = EMU_GGPO_OFF;
        }
        r = EMU_GGPO_TICK_IDLE;
    } else if (g_ggpo.skip > 0) {
        g_ggpo.skip--;                         /* ahead of the peer: hold a frame */
    } else if (emu_ggpo_frame(ctx)) {
        r = EMU_GGPO_TICK_RAN;
    }
    emu_mutex_unlock(&ctx->mutex);
    return r;
}

/* The run loop's RUNNING state while a session is wanted or open, in place of
 * emu_run_running. False when no frame ran and the loop goes straight round. */
static inline bool emu_ggpo_step(emu_thread_ctx_t *ctx) {
    if (emu_slice_should_stop(ctx)) return false;
    int64_t t0 = emu_now_us();
    emu_ggpo_tick_t r = emu_ggpo_tick(ctx);
    if (r == EMU_GGPO_TICK_IDLE) return false;
    bool ran = r == EMU_GGPO_TICK_RAN;
    int64_t t1 = emu_now_us();
    emu_times_slice(t1 - t0, ran);
    if (ran && emu_slice_stop_wanted(ctx)) {
        emu_slice_stop(ctx);
        return true;
    }
    /* A held or a waiting frame still takes its 60 Hz tick: GGPO's clock is
     * frames, and polling flat out would only send the peer more. */
    emu_pace_frame(ctx, t1);
    return true;
}

/* The lobby, every pass of the run loop, running or not. Outside the mutex. */
static inline void emu_ggpo_lobby_pump(void) {
    ggl_pump(g_ggpo.on, g_active_profile ? g_active_profile->id : "m2");
}

/* Whether the run loop hands RUNNING to emu_ggpo_step. */
static inline bool emu_ggpo_wanted(void) { return g_ggpo_cfg.mode != EMU_GGPO_OFF || g_ggpo.on; }

/* At the emu thread's end. */
static inline void emu_ggpo_shutdown(emu_thread_ctx_t *ctx) {
    emu_mutex_lock(&ctx->mutex);
    emu_ggpo_stop();
    emu_mutex_unlock(&ctx->mutex);
}

#else  /* !M2HLE_GGPO */

static inline bool emu_ggpo_active(void) { return false; }
static inline bool emu_ggpo_wanted(void) { return false; }
static inline void emu_ggpo_lobby_pump(void) {}
static inline bool emu_ggpo_step(emu_thread_ctx_t *ctx) { (void)ctx; return false; }
typedef enum { EMU_GGPO_TICK_IDLE, EMU_GGPO_TICK_HELD, EMU_GGPO_TICK_RAN } emu_ggpo_tick_t;
static inline emu_ggpo_tick_t emu_ggpo_tick(emu_thread_ctx_t *ctx) { (void)ctx; return EMU_GGPO_TICK_IDLE; }
static inline void emu_ggpo_shutdown(emu_thread_ctx_t *ctx) { (void)ctx; }

#endif

#endif
