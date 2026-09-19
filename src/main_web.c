/*
 * main_web.c -- the browser host (-DM2HLE_FRONTEND=web, Emscripten). WEB-PORT.md
 * is the plan this follows.
 *
 * One ROM set (Sonic the Fighters), no ImGui, no debugger, no MCP bridge, no
 * file system. sokol_app owns the canvas, the WebGL2 context, the keyboard and
 * the frame callback; the page around the canvas (web/shell.html) owns
 * everything a player reads or clicks, and reaches in through the functions
 * exported at the bottom of this file.
 *
 * ONE THREAD. GitHub Pages cannot send the COOP/COEP headers SharedArrayBuffer
 * needs, so there is no emu thread here: the frame callback steps the board
 * itself with emu_slice_body / emu_slice_finish -- the same slice the native
 * emu thread runs (emu_thread.h), not a copy of it. That is affordable: measured
 * as wasm under Node, a slice of STF with the sound board costs ~1.5 ms.
 *
 * Pacing is by time, not by callback count. requestAnimationFrame follows the
 * display (60, 120, 144 Hz...) and the board runs at EMU_SLICES_PER_SEC, so an
 * accumulator decides how many slices a callback owes.
 *
 * Netplay is pumped from the same place the native run loop pumps it
 * (emu_netplay_pump). With no second thread the emu mutex is never contended;
 * it is still taken, so the shared code does not need to know.
 */

/* net/ first, as in main.c: net_socket.h owns the socket include order. */
#include "net/netplay.h"

#include <emscripten.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"

#include "constants.h"
#include "log.h"
#include "memory.h"
#include "i960.h"
#include "i960_exec.h"
#include "breakpoint.h"
#include "watchpoint.h"
#include "rom_loader.h"
#include "emu_thread.h"
#include "geo3d.h"
#include "game_render.h"
#include "video_window.h"
#include "game_frame.h"
#include "sound.h"
#include "audio_out.h"
#include "input.h"

/* The single TU that defines g_profiles[] / g_active_profile. Under M2HLE_WEB it
 * registers sfight alone. */
#include "registry.h"

/* A callback that arrives late (a hidden tab, a long GC pause) owes many slices.
 * Running them all would fast-forward the game; this is as many as one callback
 * will repay before the rest is forgiven. */
#define WEB_MAX_SLICES_PER_FRAME 3
/* A slice is due once this much of its period has gone by. Below 1.0 so that a
 * 60 Hz display, whose callbacks land a hair early or late around 16.67 ms, gets
 * exactly one slice per callback instead of a 0-then-2 stutter. */
#define WEB_SLICE_DUE_US ((int64_t)EMU_SLICE_US * 85 / 100)

static struct {
    sg_pass_action   pass_action;
    memory_bus_t     bus;
    i960_cpu_t       cpu;
    emu_thread_ctx_t emu;
    romset_t         romset;
    video_state_t    video;
    geo3d_state_t    geo3d;
    int64_t          last_us;      /* the previous callback, for the accumulator */
    int64_t          owed_us;      /* board time not yet run */
    bool             gfx_ready;
} state;

/* ---- The board ------------------------------------------------------------ */

/* What load_active_profile does in main.c once the ROM files are read, and what
 * netplay's cold boot at the barrier repeats: install, then everything mem_init
 * does not reach. Called with no slice in progress (there is one thread). */
static void web_install_board(void) {
    g_active_profile->install_fn(&state.romset, &state.cpu, &state.bus);
    irqt_reset();
    if (g_active_profile->quirks.enable_68k_sound) {
        sound_reset();
        sound_attach(&state.bus);
        if (state.romset.audiocpu && state.romset.audiocpu_size > 0)
            sound_load_rom(state.romset.audiocpu, (uint32_t)state.romset.audiocpu_size);
        if (state.romset.samples && state.romset.samples_size > 0)
            sound_load_samples(state.romset.samples, (uint32_t)state.romset.samples_size);
    }
    input_reset();
    input_attach(&state.bus);
    emu_board_reset_state();
}

static void web_netplay_reset_cb(void *ctx) {
    (void)ctx;
    if (!g_active_profile || !state.romset.loaded) return;
    web_install_board();
}

/* One slice, if netplay allows it. Returns false when the board did not advance
 * (stalled on the peer, or the slice went to the barrier's reset), so the caller
 * keeps the time it owes instead of spending it. */
static bool web_slice(void) {
    netplay_step_t np = emu_netplay_pump(&state.emu);
    if (np == NETPLAY_STEP_WAIT || np == NETPLAY_STEP_RESET) return false;
    if (state.emu.run_state != EMU_RUNNING) return false;
    emu_slice_body(&state.emu);
    emu_slice_finish(&state.emu);
    return true;
}

static void web_run_owed_slices(void) {
    int64_t now = emu_now_us();
    if (state.last_us == 0) state.last_us = now;
    state.owed_us += now - state.last_us;
    state.last_us  = now;
    const int64_t cap = (int64_t)EMU_SLICE_US * WEB_MAX_SLICES_PER_FRAME;
    if (state.owed_us > cap) state.owed_us = cap;

    if (state.emu.run_state != EMU_RUNNING) {
        /* Not running yet, and netplay still has to breathe: the login and the
         * room happen before the match starts (emu_thread.h, STOPPED branch). */
        emu_netplay_pump(&state.emu);
        state.owed_us = 0;
        return;
    }
    for (int n = 0; n < WEB_MAX_SLICES_PER_FRAME && state.owed_us >= WEB_SLICE_DUE_US; n++) {
        if (!web_slice()) break;
        state.owed_us -= EMU_SLICE_US;
    }
}

/* ---- sokol_app callbacks -------------------------------------------------- */

static void init(void) {
    log_init();
    LOG_INFO("m2-hle web %s starting", M2HLE_VERSION);

    mem_init(&state.bus, NULL, 0);
    i960_reset(&state.cpu);
    bp_init();
    wp_init();
    if (g_profile_count > 0) g_active_profile = g_profiles[0];

    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });
    game_render_init();
    video_init(&state.video);
    geo3d_init(&state.geo3d);
    g_geo3d_state = &state.geo3d;
    state.gfx_ready = true;
    LOG_INFO("web: tile layers composed on the %s", state.video.gpu ? "GPU" : "CPU");

    audio_out_init();

    netplay_init();
    netplay_set_reset_hook(web_netplay_reset_cb, NULL);
    netplay_set_open_browser(false);   /* the page shows the link; see WEB-PORT.md 3.2 */

    emu_ctx_init(&state.emu, &state.cpu, &state.bus);

    state.pass_action = (sg_pass_action){
        .colors[0] = { .load_action = SG_LOADACTION_CLEAR, .clear_value = { 0.0f, 0.0f, 0.0f, 1.0f } },
        .depth     = { .load_action = SG_LOADACTION_CLEAR, .clear_value = 1.0f },
    };

    /* Tell the page the exports below can be called now. */
    EM_ASM({ if (Module.onM2hleReady) Module.onM2hleReady(); });
}

static void frame(void) {
    web_run_owed_slices();

    const bool have_game = state.romset.loaded;
    float lerp_t = 1.0f;
    if (have_game) {
        game_frame_prepare(&state.video, &state.geo3d, &state.bus, &state.romset, true);
        lerp_t = game_frame_lerp();
    }

    sg_begin_pass(&(sg_pass){ .action = state.pass_action, .swapchain = sglue_swapchain() });
    if (have_game) {
        int ox, oy, w, h;
        game_render_letterbox(sapp_width(), sapp_height(), VIDEO_WIDTH, VIDEO_HEIGHT, &ox, &oy, &w, &h);
        game_frame_draw(&state.video, &state.geo3d, &state.bus, &state.romset, ox, oy, w, h, lerp_t);
    }
    sg_end_pass();
    sg_commit();
}

static void cleanup(void) {
    netplay_shutdown();
    audio_out_shutdown();
    romset_free(&state.romset);
    game_render_shutdown();
    video_shutdown(&state.video);
    sg_shutdown();
    mem_shutdown(&state.bus);
}

static void event(const sapp_event *ev) {
    /* Inputs reach the game through the emulated I/O ports (input.h); under
     * netplay the board reads the composed mask instead, and this is what
     * netplay_sample_local transmits. */
    if (ev->type == SAPP_EVENTTYPE_KEY_DOWN && !ev->key_repeat) input_key_down((int)ev->key_code);
    if (ev->type == SAPP_EVENTTYPE_KEY_UP)                      input_key_up((int)ev->key_code);
    /* A tab that loses focus never sees the key-up: let go of everything. */
    if (ev->type == SAPP_EVENTTYPE_UNFOCUSED) input_reset();
}

sapp_desc sokol_main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    return (sapp_desc){
        .init_cb      = init,
        .frame_cb     = frame,
        .cleanup_cb   = cleanup,
        .event_cb     = event,
        .high_dpi     = true,
        .window_title = "Sonic the Fighters",
        .html5 = {
            .canvas_selector = "#canvas",
            .canvas_resize   = false,   /* the page sizes the canvas; sokol tracks it */
        },
        .logger.func  = slog_func,
    };
}

/* ---- What the page calls -------------------------------------------------- */

/*
 * Load the ROM set from the bytes of the one zip the player picked. `zip` is a
 * malloc'd buffer the page filled; this takes it over and frees it before
 * returning, because the zip is 18 MB that nothing needs once the regions are
 * extracted. Strict: every file must be in the zip by CRC (rom_loader.h).
 *
 * Returns 0 and starts the board, or -1 (web_rom_missing names what was not
 * found) or -2 (no profile).
 */
EMSCRIPTEN_KEEPALIVE int web_rom_load(uint8_t *zip, int len) {
    if (!zip || len <= 0) { free(zip); return -1; }
    if (!g_active_profile) { free(zip); return -2; }

    state.emu.run_state = EMU_STOPPED;
    rl_mem_zip_set(zip, (size_t)len, true);
    int rc = g_active_profile->load_fn(&state.romset, NULL, NULL);
    /* Drop the pointer, keep the report: the page reads it after a failed load. */
    rl_mem_zip_clear();
    free(zip);
    if (rc != 0) return -1;

    web_install_board();
    emu_run(&state.emu);
    state.last_us = 0;
    state.owed_us = 0;
    LOG_INFO("web: '%s' loaded; reset IP = 0x%08X", g_active_profile->id, state.cpu.sfr.ip);
    return 0;
}

/* The files the last web_rom_load did not find, space separated ("" if none). */
EMSCRIPTEN_KEEPALIVE const char *web_rom_missing(void) {
    return g_rl_mem_zip.missing_names;
}

/* 0 = no game loaded, 1 = running, 2 = loaded but stopped (halted CPU, ...). */
EMSCRIPTEN_KEEPALIVE int web_state(void) {
    if (!state.romset.loaded) return 0;
    return state.emu.run_state == EMU_RUNNING ? 1 : 2;
}

/* Game frames since the last board reset. */
EMSCRIPTEN_KEEPALIVE unsigned web_frames(void) {
    return (unsigned)g_emu_frames;
}
