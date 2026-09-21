/*
 * main_web.c -- the browser host (-DM2HLE_FRONTEND=web, Emscripten). WEB-PORT.md
 * is the plan this follows.
 *
 * One ROM set (Sonic the Fighters), no ImGui, no debugger, no MCP bridge, no
 * file system. sokol_app owns the canvas, the WebGL2 context and the frame
 * callback; the page around the canvas (web/site/) owns the keyboard and
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
#include "objview_cmd.h"  /* the debug object viewer, driven from the page */
#include "json_min.h"

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

/* The page is showing the object viewer in place of the game (see the
 * web_objview_* exports at the end of this file). Declared here because
 * frame() reads it. */
static int g_web_objview_show;

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

/* ---- Where a frame's time goes ------------------------------------------------
 *
 * Running totals, cheap enough to keep always. The page's "Feels laggy?" check
 * (web/site/m2hle-tools.js) reads them twice a few seconds apart and reasons from
 * the difference, because "lag" has causes that look alike from the chair and
 * need opposite advice: the emulator not keeping up (CPU), the browser drawing
 * too few frames (GPU, or a throttled or 30 Hz display), single long frames
 * (stutter), sound dropping out, or the other player's connection.
 *
 * The clock is performance.now(), which browsers coarsen on purpose (0.1 ms in
 * Chrome, 1 ms in Firefox). A single slice is not measured well by it; a few
 * hundred of them are. The maxima are since the last read, which resets them.
 */
static struct {
    uint64_t callbacks;         /* frame() calls: what the browser let us draw */
    uint64_t slices;            /* board slices run */
    uint64_t slice_us;          /* time inside them */
    uint64_t render_us;         /* time building and submitting the picture (CPU side) */
    uint64_t long_callbacks;    /* callbacks that arrived more than 25 ms after the last */
    uint64_t forgiven_us;       /* board time dropped because a callback owed too much */
    uint64_t background_ticks;  /* worker ticks that ran the board with no frame (web_background_tick) */
    uint32_t slice_us_max, render_us_max, gap_us_max;
    int64_t  last_cb_us;
    int      gpu_timing;        /* the page wants m2hleFrameBegin/End around the GL work */
} g_web_perf;

/* ---- Render scale -------------------------------------------------------------
 *
 * 0 draws the game straight into the canvas, at the canvas's resolution: the
 * sharpest picture, and on a 4K or high-DPI display around eight million pixels a
 * frame through a fill shader that makes ~10 texture fetches each. An integrated
 * GPU cannot do that at 60 Hz. N >= 1 draws it offscreen at N times the board's
 * own 496x384 and scales that up, which is what arc-s's frontend does for its
 * handheld: at 2 it is a tenth of the pixels of a 4K canvas. The lag check offers
 * it when the evidence points at the GPU. Changeable while running.
 */
static struct {
    int      scale;             /* in force */
    int      want;              /* asked for; applied at the top of the next frame */
    sg_image color, depth;
    sg_view  color_att, depth_att, texture;
} g_web_rt;

static void web_rt_apply(void) {
    if (g_web_rt.want == g_web_rt.scale) return;
    if (g_web_rt.scale > 0) {
        sg_destroy_view(g_web_rt.texture);
        sg_destroy_view(g_web_rt.depth_att);
        sg_destroy_view(g_web_rt.color_att);
        sg_destroy_image(g_web_rt.depth);
        sg_destroy_image(g_web_rt.color);
    }
    g_web_rt.scale = g_web_rt.want;
    if (g_web_rt.scale <= 0) return;
    const int w = VIDEO_WIDTH * g_web_rt.scale, h = VIDEO_HEIGHT * g_web_rt.scale;
    g_web_rt.color = sg_make_image(&(sg_image_desc){
        .usage = { .color_attachment = true }, .width = w, .height = h,
        .pixel_format = SG_PIXELFORMAT_RGBA8, .sample_count = 1, .label = "web-game-target" });
    g_web_rt.depth = sg_make_image(&(sg_image_desc){
        .usage = { .depth_stencil_attachment = true }, .width = w, .height = h,
        .pixel_format = SG_PIXELFORMAT_DEPTH_STENCIL, .sample_count = 1, .label = "web-game-target-depth" });
    g_web_rt.color_att = sg_make_view(&(sg_view_desc){ .color_attachment.image = g_web_rt.color });
    g_web_rt.depth_att = sg_make_view(&(sg_view_desc){ .depth_stencil_attachment.image = g_web_rt.depth });
    g_web_rt.texture   = sg_make_view(&(sg_view_desc){ .texture.image = g_web_rt.color });
    LOG_INFO("web: drawing the game at %dx%d (render scale %d)", w, h, g_web_rt.scale);
}

/* Who plays the sound. The page decides (web/site/m2hle-page.js, "Sound") and
 * says so through web_audio_use_worklet / web_audio_use_fallback, because finding
 * out whether a worklet can be had is asynchronous. Until it answers the board's
 * output goes nowhere, and audio_out_push_begin throws that backlog away. */
typedef enum { WEB_AUDIO_PENDING, WEB_AUDIO_WORKLET, WEB_AUDIO_FALLBACK } web_audio_mode_t;
static web_audio_mode_t g_web_audio = WEB_AUDIO_PENDING;
static double           g_web_audio_nudge = 0.0;
/* Three slices at 48 kHz is 2400 frames; one callback never renders more. */
#define WEB_AUDIO_CHUNK_FRAMES 4096
static float g_web_audio_chunk[WEB_AUDIO_CHUNK_FRAMES * 2];

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

/* ---- A scripted run ---------------------------------------------------------
 *
 * Inputs keyed to GAME FRAMES, and a frame to stop at. A key pressed by wall
 * clock lands on a different frame every run; with this, two runs of the same
 * script are the same run, so one frame can be drawn with and without a render
 * optimisation (?args=) and the two pictures compared. It is also the shape a
 * netplay determinism test needs. Off unless the page is given ?script=.
 *
 *   web_script("449:c,460:,517:s,530:,700:l,712:")   at frame N, hold exactly these
 *   web_pause_at(1000)                               stop stepping at frame 1000
 *
 * Keys, all player 1: u d l r, 1-4 the buttons, s start, c coin.
 */
#define WEB_SCRIPT_MAX 128
static struct { uint32_t frame, held; } g_web_script[WEB_SCRIPT_MAX];
static int      g_web_script_n, g_web_script_at;
static uint32_t g_web_pause_frame;

static uint32_t web_script_mask(const char *keys, const char *end) {
    uint32_t held = 0;
    const game_input_map_t *in = &g_active_profile->input;
    for (const char *p = keys; p < end; p++) {
        switch (*p) {
            case 'u': held |= in->bits[GAME_INPUT_P1_UP];    break;
            case 'd': held |= in->bits[GAME_INPUT_P1_DOWN];  break;
            case 'l': held |= in->bits[GAME_INPUT_P1_LEFT];  break;
            case 'r': held |= in->bits[GAME_INPUT_P1_RIGHT]; break;
            case '1': held |= in->bits[GAME_INPUT_P1_B1];    break;
            case '2': held |= in->bits[GAME_INPUT_P1_B2];    break;
            case '3': held |= in->bits[GAME_INPUT_P1_B3];    break;
            case '4': held |= in->bits[GAME_INPUT_P1_B4];    break;
            case 's': held |= in->bits[GAME_INPUT_P1_START]; break;
            case 'c': held |= in->bits[GAME_INPUT_P1_COIN];  break;
            default: break;
        }
    }
    return held;
}

EMSCRIPTEN_KEEPALIVE void web_script(const char *text) {
    g_web_script_n = g_web_script_at = 0;
    if (!text || !g_active_profile) return;
    for (const char *p = text; *p && g_web_script_n < WEB_SCRIPT_MAX; ) {
        char *colon = NULL;
        unsigned long frame = strtoul(p, &colon, 10);
        if (!colon || *colon != ':') break;
        const char *end = strchr(colon, ',');
        if (!end) end = colon + strlen(colon);
        g_web_script[g_web_script_n].frame = (uint32_t)frame;
        g_web_script[g_web_script_n].held  = web_script_mask(colon + 1, end);
        g_web_script_n++;
        p = *end ? end + 1 : end;
    }
}

EMSCRIPTEN_KEEPALIVE void web_pause_at(unsigned frame) { g_web_pause_frame = frame; }

/* The last slice waited on the other player rather than running. */
static bool g_web_waited;

/* One slice, if netplay allows it. Returns false when the board did not advance
 * (stalled on the peer, or the slice went to the barrier's reset). */
static bool web_slice(void) {
    netplay_step_t np = emu_netplay_pump(&state.emu);
    g_web_waited = (np == NETPLAY_STEP_WAIT);
    if (np == NETPLAY_STEP_WAIT || np == NETPLAY_STEP_RESET) return false;
    if (state.emu.run_state != EMU_RUNNING) return false;
    while (g_web_script_at < g_web_script_n && g_web_script[g_web_script_at].frame <= g_emu_frames)
        g_input.held = g_web_script[g_web_script_at++].held;
    if (g_web_pause_frame && g_emu_frames >= g_web_pause_frame) {
        state.emu.run_state = EMU_STOPPED;
        return false;
    }
    int64_t t0 = emu_now_us();
    emu_slice_body(&state.emu);
    emu_slice_finish(&state.emu);
    uint32_t took = (uint32_t)(emu_now_us() - t0);
    g_web_perf.slices++;
    g_web_perf.slice_us += took;
    if (took > g_web_perf.slice_us_max) g_web_perf.slice_us_max = took;
    return true;
}

static void web_run_owed_slices(void) {
    int64_t now = emu_now_us();
    if (state.last_us == 0) state.last_us = now;
    state.owed_us += now - state.last_us;
    state.last_us  = now;
    const int64_t cap = (int64_t)EMU_SLICE_US * WEB_MAX_SLICES_PER_FRAME;
    if (state.owed_us > cap) {
        /* Board time that will never be run: the game fell behind the wall clock. */
        if (state.emu.run_state == EMU_RUNNING) g_web_perf.forgiven_us += (uint64_t)(state.owed_us - cap);
        state.owed_us = cap;
    }

    if (state.emu.run_state != EMU_RUNNING) {
        /* Not running yet, and netplay still has to breathe: the login and the
         * room happen before the match starts (emu_thread.h, STOPPED branch). */
        emu_netplay_pump(&state.emu);
        state.owed_us = 0;
        return;
    }
    for (int n = 0; n < WEB_MAX_SLICES_PER_FRAME && state.owed_us >= WEB_SLICE_DUE_US; n++) {
        if (!web_slice()) {
            /* Waiting on the other player: let the time go rather than owe it.
             * Owed, it is repaid with an extra slice on the next callback, which
             * puts this board straight back a fraction of a frame ahead of the
             * other one -- and it waits again, every frame, for as long as the
             * two clocks keep that phase. Measured after a hidden tab came back
             * (tools/web-netplay.mjs --hide-a): ~40 waits a second, indefinitely.
             * Dropped, the board that was ahead settles in just behind and stops
             * waiting. Both still run at the board's rate: lockstep holds either
             * one to the other's pace whatever this does. */
            if (g_web_waited) state.owed_us = 0;
            break;
        }
        state.owed_us -= EMU_SLICE_US;
    }
}

/* ---- Sound ---------------------------------------------------------------- */

/* The worklet is up: render for its sample rate and push a chunk per frame. */
EMSCRIPTEN_KEEPALIVE void web_audio_use_worklet(int device_rate) {
    audio_out_push_begin((uint32_t)device_rate);
    g_web_audio_nudge = 0.0;
    g_web_audio = WEB_AUDIO_WORKLET;
    LOG_INFO("audio: worklet, %d Hz", device_rate);
}

/* No worklet (an old browser, an insecure context): the emulator's own callback,
 * which in a browser is a ScriptProcessorNode on the main thread. It
 * double-buffers, so 512 frames is ~23 ms before the device; the queue has to
 * cover a callback plus a late slice or two. Later than the worklet by design --
 * see "The push model" in audio_out.h. */
EMSCRIPTEN_KEEPALIVE void web_audio_use_fallback(void) {
    if (g_web_audio == WEB_AUDIO_FALLBACK) return;
    g_web_audio = WEB_AUDIO_FALLBACK;
    audio_out_init_ex(&(audio_out_config_t){
        .buffer_frames = 512,
        .target        = 2048.0,
        .smooth_fill   = true,
    });
}

/* The rate correction, from whoever holds the queue (the page, averaging the
 * worklet's reports). Clamped here too: it goes straight into the resampler. */
EMSCRIPTEN_KEEPALIVE void web_audio_set_nudge(double nudge) {
    g_web_audio_nudge = nudge < -0.01 ? -0.01 : nudge > 0.01 ? 0.01 : nudge;
}

/* What the board produced since the last call goes to the worklet in one chunk. */
static void web_push_audio(void) {
    if (g_web_audio != WEB_AUDIO_WORKLET) return;
    int frames = audio_out_drain(g_web_audio_chunk, WEB_AUDIO_CHUNK_FRAMES, g_web_audio_nudge);
    if (frames > 0)
        EM_ASM({ Module.m2hleAudioPush($0, $1); }, g_web_audio_chunk, frames);
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
    objview_init();
    state.gfx_ready = true;
    LOG_INFO("web: tile layers composed on the %s", state.video.gpu ? "GPU" : "CPU");

    /* Sound: ask the page for a worklet. It answers later, with one of the two
     * web_audio_use_* exports; a page without the hook gets the fallback now. */
    if (!EM_ASM_INT({
            if (!Module.m2hleAudioStart) return 0;
            Module.m2hleAudioStart();
            return 1;
        }))
        web_audio_use_fallback();

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
    int64_t cb_us = emu_now_us();
    if (g_web_perf.last_cb_us) {
        uint32_t gap = (uint32_t)(cb_us - g_web_perf.last_cb_us);
        if (gap > 25000) g_web_perf.long_callbacks++;
        if (gap > g_web_perf.gap_us_max) g_web_perf.gap_us_max = gap;
    }
    g_web_perf.last_cb_us = cb_us;
    g_web_perf.callbacks++;

    web_run_owed_slices();
    web_push_audio();

    const int64_t render_t0 = emu_now_us();
    if (g_web_perf.gpu_timing) EM_ASM({ Module.m2hleFrameBegin(); });
    web_rt_apply();

    const bool have_game = state.romset.loaded;
    float lerp_t = 1.0f;
    if (have_game) {
        game_frame_prepare(&state.video, &state.geo3d, &state.bus, &state.romset, true);
        lerp_t = game_frame_lerp();
    }

    /* Offscreen first, when there is a render scale: the game at N x 496x384. */
    const bool offscreen = have_game && g_web_rt.scale > 0;
    if (offscreen) {
        sg_begin_pass(&(sg_pass){
            .action = state.pass_action,
            .attachments = { .colors[0] = g_web_rt.color_att, .depth_stencil = g_web_rt.depth_att },
        });
        game_frame_draw(&state.video, &state.geo3d, &state.bus, &state.romset,
                        0, 0, VIDEO_WIDTH * g_web_rt.scale, VIDEO_HEIGHT * g_web_rt.scale, lerp_t);
        sg_end_pass();
    }

    /* The object viewer draws passes of its own, so it runs before the
     * swapchain pass opens -- sokol does not nest them. It costs nothing
     * unless something asked for it: a shot is pending, a setting changed,
     * or the page is showing the viewer instead of the game. */
    if (g_web_objview_show) g_objview.refresh = 1;
    objview_service(&state.romset, &state.bus);
    const bool show_objview = g_web_objview_show && g_objview.color_tex.id && g_objview.rt_w > 0;

    sg_begin_pass(&(sg_pass){ .action = state.pass_action, .swapchain = sglue_swapchain() });
    if (show_objview) {
        /* In place of the game, letterboxed to the viewer target's own shape. */
        int ox, oy, w, h;
        game_render_letterbox(sapp_width(), sapp_height(), g_objview.rt_w, g_objview.rt_h, &ox, &oy, &w, &h);
        game_render_draw_target(g_objview.color_tex, true, ox, oy, w, h);
    } else if (have_game) {
        int ox, oy, w, h;
        game_render_letterbox(sapp_width(), sapp_height(), VIDEO_WIDTH, VIDEO_HEIGHT, &ox, &oy, &w, &h);
        if (offscreen) game_render_draw_target(g_web_rt.texture, true, ox, oy, w, h);
        else           game_frame_draw(&state.video, &state.geo3d, &state.bus, &state.romset, ox, oy, w, h, lerp_t);
    }
    sg_end_pass();
    sg_commit();

    if (g_web_perf.gpu_timing) EM_ASM({ Module.m2hleFrameEnd(); });
    uint32_t render_us = (uint32_t)(emu_now_us() - render_t0);
    g_web_perf.render_us += render_us;
    if (render_us > g_web_perf.render_us_max) g_web_perf.render_us_max = render_us;
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

static uint32_t g_web_pad;   /* the actions the page holds (gamepads, touch buttons, keyboard): web_pad_set */

static void event(const sapp_event *ev) {
    /* No keys here: the page reads the keyboard through the player's own
     * bindings (web/site/m2hle-keys.js) and sends it with the gamepads and the
     * touch buttons through web_pad_set. Mapping sokol's key codes as well would
     * press the default keys on top of the remapped ones. */
    /* A tab that loses focus never sees the key-up: let go of everything. */
    if (ev->type == SAPP_EVENTTYPE_UNFOCUSED) { input_reset(); g_web_pad = 0; }
}

sapp_desc sokol_main(int argc, char *argv[]) {
    /* The page passes ?args=a,b,c through as argv (web/site/m2hle-page.js). These
     * are main_sdl.c's switches for the render work that came from arc-s, each of
     * which turns one optimisation off and leaves the picture the same -- so when
     * the picture is NOT the same, they say which one to look at. All are read by
     * game_render_init / video_init, which run after this. */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "--no-mesh-cache")) g_geo3d_mesh_cache = 0;
        else if (!strcmp(a, "--fill-ref"))      g_game_render_fill_use_ref = 1;
        else if (!strcmp(a, "--fill-no-split")) g_game_render_fill_split = 0;
        else if (!strcmp(a, "--fill-no-ramp"))  g_game_render_fill_ramp = 0;
        else if (!strcmp(a, "--cpu-tiles"))     g_video_force_cpu_tiles = 1;
    }
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

/* Let go of every held input. The page calls this when focus moves into its
 * tools drawer. Whatever the page still holds (a pad, a key whose key-up has not
 * come yet) is pressed again by its next web_pad_set. */
/* Only the local keyboard's mask: under netplay the board reads the composed mask,
 * which belongs to the lockstep and is rebuilt from both players' words each frame. */
EMSCRIPTEN_KEEPALIVE void web_release_keys(void) { input_release_all(); g_web_pad = 0; }

/* The page's gamepads, touch buttons and keyboard (web/site/m2hle-pad.js,
 * m2hle-touch.js, m2hle-keys.js), as one bit per GAME_INPUT_* action, sent whole
 * on every poll and on every key or touch. Only the changes are pressed or
 * released, so a scripted press (?script=) is not let go of every frame -- the same rule as main_sdl.c's pad_refresh. When
 * something clears g_input.held (focus lost, the drawer opened), g_web_pad is
 * cleared with it, and whatever the pad still holds is pressed again on the
 * next poll. */
EMSCRIPTEN_KEEPALIVE void web_pad_set(uint32_t actions) {
    uint32_t changed = actions ^ g_web_pad;
    for (int a = 0; a < GAME_INPUT_COUNT; a++) {
        if (!(changed & (1u << a))) continue;
        if (actions & (1u << a)) input_action_down(a);
        else                     input_action_up(a);
    }
    g_web_pad = actions;
}

/* Game frames since the last board reset. */
EMSCRIPTEN_KEEPALIVE unsigned web_frames(void) {
    return (unsigned)g_emu_frames;
}

/* The running totals above, as JSON, plus what else the lag check reasons from.
 * `reset` clears the maxima, so each reading's worst case is its own. */
EMSCRIPTEN_KEEPALIVE const char *web_perf(int reset) {
    static char out[640];
    netplay_status_t np;
    netplay_get_status(&np);
    snprintf(out, sizeof out,
             "{\"now_us\":%.0f,\"callbacks\":%llu,\"slices\":%llu,\"frames\":%u,"
             "\"slice_us\":%llu,\"render_us\":%llu,\"long_callbacks\":%llu,\"forgiven_us\":%llu,"
             "\"background_ticks\":%llu,\"slice_us_max\":%u,\"render_us_max\":%u,\"gap_us_max\":%u,"
             "\"render_scale\":%d,\"canvas_w\":%d,\"canvas_h\":%d,\"gpu_tiles\":%s,"
             "\"netplay\":\"%s\",\"netplay_stalls\":%u,\"netplay_delay\":%u}",
             (double)emu_now_us(),
             (unsigned long long)g_web_perf.callbacks, (unsigned long long)g_web_perf.slices, (unsigned)g_emu_frames,
             (unsigned long long)g_web_perf.slice_us, (unsigned long long)g_web_perf.render_us,
             (unsigned long long)g_web_perf.long_callbacks, (unsigned long long)g_web_perf.forgiven_us,
             (unsigned long long)g_web_perf.background_ticks, g_web_perf.slice_us_max, g_web_perf.render_us_max, g_web_perf.gap_us_max,
             g_web_rt.scale, sapp_width(), sapp_height(), state.video.gpu ? "true" : "false",
             netplay_state_text(np.state), np.stalls, (unsigned)g_netplay.cfg.frame_delay);
    if (reset) g_web_perf.slice_us_max = g_web_perf.render_us_max = g_web_perf.gap_us_max = 0;
    return out;
}

/* Ask for m2hleFrameBegin/End around each frame's GL work (a GPU timer query on
 * the page's side). Off outside a measurement: it is two calls into JS a frame. */
EMSCRIPTEN_KEEPALIVE void web_perf_gpu_timing(int on) { g_web_perf.gpu_timing = on != 0; }

/* 0 = draw at the canvas's resolution; N = draw at N x 496x384 and scale up. */
EMSCRIPTEN_KEEPALIVE void web_set_render_scale(int scale) {
    g_web_rt.want = scale < 0 ? 0 : scale > 4 ? 4 : scale;
}
EMSCRIPTEN_KEEPALIVE int web_render_scale(void) { return g_web_rt.want; }

/* The sound board at a glance, as JSON: the same figures the desktop build's
 * sound_status bridge command reports, so a web run and a native run of the same
 * scenario can be held against each other. `active` is the mask of SCSP slots
 * sounding -- music holds several for as long as it plays, effects come and go --
 * and the midi_* counters say whether the i960's command bytes all arrived. */
EMSCRIPTEN_KEEPALIVE const char *web_sound_status(void) {
    static char out[512];
    const scsp_t *sc = &g_sound.scsp;
    uint32_t keyed = 0, active = 0;
    for (int i = 0; i < 32; i++) {
        if (sc->slot[i].r[0] & 0x0800) keyed  |= 1u << i;
        if (sc->slot[i].active)        active |= 1u << i;
    }
    snprintf(out, sizeof out,
             "{\"frames\":%u,\"m68k_pc\":\"0x%06X\",\"midi_writes\":%llu,\"midi_fifo\":%u,"
             "\"midi_drops\":%u,\"midi_hi\":%u,\"midi_drains\":%llu,\"keyed\":%u,\"active\":%u,"
             "\"irqs\":[%llu,%llu,%llu],\"out_dropped\":%llu}",
             (unsigned)g_emu_frames, g_sound.m68k.cpu.pc,
             (unsigned long long)g_sound.write_count, (unsigned)((sc->mi_w - sc->mi_r) & 31),
             sc->mi_drops, sc->mi_hi, (unsigned long long)g_sound.midi_drains, keyed, active,
             (unsigned long long)g_sound.irqs[1], (unsigned long long)g_sound.irqs[2],
             (unsigned long long)g_sound.irqs[3], (unsigned long long)g_sound.out_dropped);
    return out;
}

/* The FALLBACK path's queue, for the page's audioStats(): frames of board audio
 * waiting to be played (44.1 kHz), and how often the queue ran dry or was found
 * stale. On the worklet path the queue is the worklet's and the page has it. */

/* ---- The debug object viewer ---------------------------------------------
 *
 * One model on its own, drawn offscreen from a camera the caller places, read
 * back and encoded as a PNG. The command vocabulary is objview_cmd.h's — the
 * same one the desktop build's TCP bridge carries — so a request written
 * against one works against the other.
 *
 * What differs here is the waiting. The desktop bridge has a thread and blocks
 * on the render thread; the browser has one thread, which is also the one
 * drawing, so nothing may block. A request is ARMED by web_objview() and served
 * by the next frame callback, and the caller comes back for the answer:
 *
 *   web_objview('{"cmd":"objview_set","model":3544}')   -> state, serial S
 *   ... await a frame ... state.serial > S means the pass ran
 *   web_objview('{"cmd":"objview_shot","count":8}')     -> {"pending":true}
 *   web_objview_poll()                                   -> {"pending":true} | the batch
 *   web_objview_png(i) / web_objview_png_len(i)          -> the bytes to Blob
 *
 * There is no filesystem here, so a shot writes nothing: each PNG stays in the
 * heap until the next batch replaces it, and JavaScript copies it out.
 */

/* Big enough for a 64-shot batch's records plus the state block. */
static char g_web_objview_out[24576];

EMSCRIPTEN_KEEPALIVE const char *web_objview(const char *json) {
    char cmd[48];
    cmd[0] = '\0';
    if (json) json_get_str(json, "cmd", cmd, sizeof cmd);

    if (!strcmp(cmd, "objview_list")) {
        objview_cmd_list(json, &state.romset, g_web_objview_out, (int)sizeof g_web_objview_out);
    } else if (!strcmp(cmd, "objview_shot")) {
        char err[192];
        if (!objview_cmd_arm_shot(json, err, sizeof err)) {
            objview_cmd_reply(&state.romset, &state.bus,
                              g_web_objview_out, (int)sizeof g_web_objview_out, 0, err);
        } else {
            /* Armed. The next frame callback serves it; web_objview_poll has it. */
            char *p = g_web_objview_out;
            int   left = (int)sizeof g_web_objview_out;
            int   n = snprintf(p, (size_t)left, "{\"ok\":true,\"pending\":true,");
            p += n; left -= n;
            n = objview_cmd_state(&state.romset, &state.bus, p, left);
            p += n; left -= n;
            snprintf(p, (size_t)left, "}");
        }
    } else {
        /* objview_set and objview_status are the same reply; a set also asks for
         * one service pass, and the caller watches `serial` to know it ran. */
        if (!strcmp(cmd, "objview_set")) {
            objview_cmd_apply(json);
            g_objview.refresh = 1;
        }
        objview_cmd_reply(&state.romset, &state.bus,
                          g_web_objview_out, (int)sizeof g_web_objview_out,
                          1, NULL);
    }
    return g_web_objview_out;
}

/* The armed batch's result, or {"pending":true} while the frame callback has
 * not reached it yet. */
EMSCRIPTEN_KEEPALIVE const char *web_objview_poll(void) {
    const objview_req_t *rq = &g_objview.req;
    if (rq->pending || !rq->done) {
        snprintf(g_web_objview_out, sizeof g_web_objview_out,
                 "{\"ok\":true,\"pending\":true,\"shots\":[]}");
        return g_web_objview_out;
    }
    objview_cmd_shot_reply(&state.romset, &state.bus,
                           g_web_objview_out, (int)sizeof g_web_objview_out, 0);
    return g_web_objview_out;
}

/* Shot i's encoded PNG, in the heap. Valid until the next batch is taken. */
EMSCRIPTEN_KEEPALIVE const uint8_t *web_objview_png(int i) {
    if (i < 0 || i >= g_objview.req.shots) return NULL;
    return (const uint8_t *)g_objview.req.shot[i].png;
}
EMSCRIPTEN_KEEPALIVE int web_objview_png_len(int i) {
    if (i < 0 || i >= g_objview.req.shots) return 0;
    return g_objview.req.shot[i].png ? (int)g_objview.req.shot[i].bytes : 0;
}

/*
 * Show the viewer on the canvas instead of the game.
 *
 * The desktop build puts its preview in an ImGui window; there is no ImGui
 * here, so the canvas is the only surface there is. While this is on the frame
 * callback keeps the viewer's target up to date and draws that, letterboxed,
 * in place of the game — the board carries on running behind it, so turning it
 * off returns to a game that never stopped.
 */

EMSCRIPTEN_KEEPALIVE void web_objview_show(int on) {
    g_web_objview_show = on ? 1 : 0;
    if (g_web_objview_show) g_objview.v.active = 1;
}
EMSCRIPTEN_KEEPALIVE int web_objview_showing(void) { return g_web_objview_show; }

EMSCRIPTEN_KEEPALIVE unsigned web_audio_queued(void)    { return audio_out_queued(); }
EMSCRIPTEN_KEEPALIVE unsigned web_audio_underruns(void) { return (unsigned)g_audio_out.underruns; }
EMSCRIPTEN_KEEPALIVE unsigned web_audio_resyncs(void)   { return (unsigned)g_audio_out.resyncs; }

/* ---- Netplay ---------------------------------------------------------------
 *
 * The page's sign-in, lobby and room screens (web/site/m2hle-netplay.js) drive
 * netplay.h through these, the way the desktop's netplay window does: they read
 * a snapshot and post commands, and never touch netplay state directly.
 *
 * A command is staged a field at a time and then posted by name:
 *
 *   web_netplay_begin()                      start from what is stored
 *   web_netplay_set("npid", "sonic")         ... any fields
 *   web_netplay_post("connect")              queue it
 *
 * rather than as one JSON string, because a password can hold any character and
 * json_min.h reads a quote as the end of the value.
 *
 * The server is fixed. The gateway decides where the stream really goes (tls.h,
 * web backend); the name still matters because a Twitch login token is only
 * offered to the server that issued it (netplay_twitch_reuse), and that is this
 * one.
 */
#define WEB_NETPLAY_SERVER "rpcn.sonicthefighte.rs"

static netplay_config_t g_web_np;

static void web_netplay_defaults(netplay_config_t *c) {
    snprintf(c->server, sizeof(c->server), "%s", WEB_NETPLAY_SERVER);
    c->port = RPCN_DEFAULT_PORT;
    c->fingerprint[0] = '\0';
    c->browse_yamp = false;
    c->local_p2p_port = 0;
}

/* Where the gateway is: wss://rpcn.sonicthefighte.rs/gw unless the page says
 * otherwise (?gw=, a local gateway for testing). Only before connecting. */
EMSCRIPTEN_KEEPALIVE void web_netplay_set_gateway(const char *url) {
    if (url && (!strncmp(url, "wss://", 6) || !strncmp(url, "ws://", 5)))
        snprintf(g_web_gateway_url, sizeof(g_web_gateway_url), "%s", url);
}

EMSCRIPTEN_KEEPALIVE void web_netplay_begin(void) {
    memset(&g_web_np, 0, sizeof(g_web_np));
    netplay_stored_settings(&g_web_np);
    web_netplay_defaults(&g_web_np);
}

EMSCRIPTEN_KEEPALIVE int web_netplay_set(const char *key, const char *value) {
    if (!key || !value) return -1;
    netplay_config_t *c = &g_web_np;
#define WEB_NP_STR(name) if (!strcmp(key, #name)) { snprintf(c->name, sizeof(c->name), "%s", value); return 0; }
    WEB_NP_STR(npid)
    WEB_NP_STR(password)
    WEB_NP_STR(token)
    WEB_NP_STR(email)
    WEB_NP_STR(room_password)
#undef WEB_NP_STR
    if (!strcmp(key, "room_id"))     { c->room_id = strtoull(value, NULL, 10); return 0; }
    if (!strcmp(key, "frame_delay")) { c->frame_delay = (uint32_t)strtoul(value, NULL, 10); return 0; }
    return -1;
}

EMSCRIPTEN_KEEPALIVE int web_netplay_post(const char *cmd) {
    static const struct { const char *name; netplay_cmd_kind_t kind; } k[] = {
        { "connect",        NETPLAY_CMD_CONNECT },
        { "disconnect",     NETPLAY_CMD_DISCONNECT },
        { "host",           NETPLAY_CMD_HOST },
        { "join",           NETPLAY_CMD_JOIN },
        { "search",         NETPLAY_CMD_SEARCH },
        { "start",          NETPLAY_CMD_START },
        { "stop",           NETPLAY_CMD_STOP },
        { "create_account", NETPLAY_CMD_CREATE_ACCOUNT },
        { "resend_token",   NETPLAY_CMD_RESEND_TOKEN },
        { "twitch_start",   NETPLAY_CMD_TWITCH_START },
        { "twitch_cancel",  NETPLAY_CMD_TWITCH_CANCEL },
        { "twitch_forget",  NETPLAY_CMD_TWITCH_FORGET },
    };
    if (!cmd) return -1;
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        if (strcmp(cmd, k[i].name)) continue;
        web_netplay_defaults(&g_web_np);
        netplay_post(k[i].kind, &g_web_np);
        return 0;
    }
    return -1;
}

/* Sign out: leave, and forget every credential this browser holds -- the Twitch
 * login token and a remembered password alike. */
EMSCRIPTEN_KEEPALIVE void web_netplay_signout(void) {
    netplay_post(NETPLAY_CMD_DISCONNECT, NULL);
    netplay_post(NETPLAY_CMD_TWITCH_FORGET, NULL);
    g_netplay.cfg.password[0] = '\0';
    g_netplay.cfg.token[0]    = '\0';
    netplay_settings_save();
}

static int web_json_str(char *out, int cap, const char *key, const char *value, bool comma) {
    char esc[640];
    json_escape(esc, (int)sizeof esc, value);
    return snprintf(out, (size_t)cap, "%s\"%s\":\"%s\"", comma ? "," : "", key, esc);
}

/*
 * Everything the page draws, as JSON. `log_from` is the log_count the page last
 * saw: only lines after it are included (all that are still in the ring).
 */
EMSCRIPTEN_KEEPALIVE const char *web_netplay_status(unsigned log_from) {
    static char out[48 * 1024];
    static netplay_status_t st;
    netplay_get_status(&st);
    char *p = out;
    int left = (int)sizeof out, n;
#define PUT(...) do { n = snprintf(p, (size_t)left, __VA_ARGS__); if (n < 0 || n >= left) goto full; p += n; left -= n; } while (0)
#define PUTS(key, val, comma) do { n = web_json_str(p, left, key, val, comma); if (n < 0 || n >= left) goto full; p += n; left -= n; } while (0)

    PUT("{");
    PUTS("state", netplay_state_text(st.state), false);
    PUT(",\"stage\":%d", (int)st.stage);
    PUTS("error", st.error, true);
    PUTS("npid", g_netplay.cfg.npid, true);
    PUT(",\"has_password\":%s", g_netplay.cfg.password[0] ? "true" : "false");
    PUT(",\"family\":\"%s\",\"cross_play\":%s",
        NETPLAY_BUILD_FAMILY == NETPLAY_FAMILY_WASM ? "web" : "desktop", NETPLAY_CROSS_PLAY ? "true" : "false");

    /* The activation address is shown to the player as a link, so it gets the
     * same test netplay_open_url applies before a desktop opens one. */
    const char *uri = !strncmp(st.twitch_uri, "https://", 8) ? st.twitch_uri : "";
    PUT(",\"twitch\":{\"state\":%d,\"signed_in\":%s", (int)st.twitch_state, st.twitch_signed_in ? "true" : "false");
    PUTS("code", st.twitch_user_code, true);
    PUTS("uri", uri, true);
    PUTS("npid", st.twitch_npid, true);
    PUTS("error", st.twitch_error, true);
    PUT("}");

    PUT(",\"account\":{\"state\":%d,\"job\":%d", (int)st.account_state, (int)st.account_job);
    PUTS("error", st.account_error, true);
    PUT("}");

    PUT(",\"room\":{\"id\":\"%llu\",\"host\":%s,\"player\":%d,\"flags\":%u",
        (unsigned long long)st.room_id, st.is_host ? "true" : "false", (int)st.local_player, st.room_flags);
    PUTS("peer", st.peer_npid, true);
    PUT(",\"peer_known\":%s,\"peer_heard\":%s,\"peer_ready\":%s}",
        st.peer_known ? "true" : "false", st.peer_heard ? "true" : "false", st.peer_ready ? "true" : "false");

    PUT(",\"frame\":%u,\"stalls\":%u,\"generation\":%u,\"delay\":%u",
        st.frame, st.stalls, st.generation, (unsigned)g_netplay.cfg.frame_delay);
    if (st.desync_frame != LOCKSTEP_NO_CHECK) PUT(",\"desync\":%u", st.desync_frame);
    else                                      PUT(",\"desync\":null");

    PUT(",\"search_pending\":%s,\"rooms\":[", st.search_pending ? "true" : "false");
    for (uint32_t i = 0; i < st.room_count && i < RPCN_MAX_ROOMS; i++) {
        const rpcn_room_listing_t *r = &st.rooms[i];
        const char *why = netplay_room_reject_reason(r->flag_attr, g_active_profile);
        uint32_t family = (r->flag_attr >> NETPLAY_ROOM_FAMILY_SHIFT) & NETPLAY_ROOM_FAMILY_MASK;
        PUT("%s{\"id\":\"%llu\",\"members\":%u,\"slots\":%u,\"password\":%s,\"delay\":%u,\"web\":%s",
            i ? "," : "", (unsigned long long)r->room_id, r->cur_members, r->max_slots,
            r->has_password ? "true" : "false",
            (r->flag_attr >> NETPLAY_ROOM_DELAY_SHIFT) & NETPLAY_ROOM_DELAY_MASK,
            family == NETPLAY_FAMILY_WASM ? "true" : "false");
        PUTS("owner", r->owner, true);
        PUTS("why", why ? why : "", true);
        PUT("}");
    }
    PUT("]");

    PUT(",\"log_count\":%u,\"log\":[", st.log_count);
    uint32_t first = st.log_count > NETPLAY_LOG_LINES ? st.log_count - NETPLAY_LOG_LINES : 0;
    if (log_from > first) first = log_from;
    for (uint32_t i = first; i < st.log_count; i++) {
        char esc[NETPLAY_LOG_LEN * 2];
        json_escape(esc, (int)sizeof esc, st.log[i % NETPLAY_LOG_LINES]);
        PUT("%s\"%s\"", i > first ? "," : "", esc);
    }
    PUT("]}");
    return out;
full:
    snprintf(out, sizeof out, "{\"state\":\"%s\",\"error\":\"status too large\"}", netplay_state_text(st.state));
    return out;
#undef PUT
#undef PUTS
}

/* The board runs on the wall clock, not on animation frames. A hidden tab gets
 * none, and a throttled one (an occluded window, a power saver) gets them late,
 * so on frames alone the game pauses or stutters and an opponent stalls. The
 * page therefore also calls this from a worker's timer, several times a frame,
 * for as long as the game is loaded (web/site/m2hle-page.js). While frame() is
 * being called it does nothing: the slices are frame()'s, where they line up
 * with the picture. Once frames go quiet it runs the board itself, with no
 * picture. Both share one accumulator, so the hand-over either way neither skips
 * nor repeats board time.
 *
 * Sound is still computed and drained -- the board has to compute it to stay the
 * same board -- but the page suspends its AudioContext while hidden, and
 * m2hleAudioPush drops what arrives then. */
#define WEB_RAF_QUIET_US 50000
EMSCRIPTEN_KEEPALIVE void web_background_tick(void) {
    if (g_web_perf.last_cb_us && emu_now_us() - g_web_perf.last_cb_us < WEB_RAF_QUIET_US) return;
    g_web_perf.background_ticks++;
    web_run_owed_slices();
    web_push_audio();
}

EMSCRIPTEN_KEEPALIVE int web_netplay_active(void) { return netplay_active() ? 1 : 0; }
