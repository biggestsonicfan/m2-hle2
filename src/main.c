/*
 * m2-hle — Sega Model 2 HLE emulator
 * Host shell: sokol window + cimgui (dear_bindings) via sokol_imgui.
 *
 * Phase 4: ROM loading + profile resolution. File→Load ROMs picks a ROM zip,
 * auto-selects the matching profile by basename, runs load_fn + install_fn,
 * and the CPU's initial IP is set from the PRCB reset vector. (Free-running
 * execution arrives with the emu thread in Phase 5.)
 */
#include <stdbool.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

/* net/ FIRST, before anything that might pull in <windows.h>: net_socket.h owns
 * the winsock include order and <winsock2.h> has to precede it. See the note at
 * the top of that header. */
#include "net/netplay.h"

#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"

#include "cimgui.h"
#include "sokol_imgui.h"
#include "ImGuiFileDialog.h"

#include "constants.h"
#include "log.h"
#include "memory.h"
#include "i960.h"
#include "i960_exec.h"
#include "breakpoint.h"
#include "watchpoint.h"
#include "rom_loader.h"
#include "emu_thread.h"
#include "memview.h"
#include "cpu_window.h"
#include "breakpoint_window.h"
#include "cop_window.h"
#include "video_window.h"
#include "geo3d.h"
#include "game_render.h"
#include "game_frame.h"
#include "geo3d_window.h"
#include "objview_window.h"  /* the debug object viewer, driven by hand or over MCP */
#include "sound.h"
#include "audio_out.h"
#include "m68k_window.h"
#include "m68k_memview.h"
#include "input.h"
#include "debug_window.h"
#include "av_stream.h"     /* --av-port raw A/V server (transport + the audio tap) */
#include "av_capture.h"    /* ...and its offscreen target + async GPU readback */
#include "mcp_bridge.h"    /* --mcp TCP debug server (ported from m2-hle) */
#include "netplay_window.h"  /* the RPCN netplay front-end */
#include "kiosk.h"           /* --kiosk: chrome-free capture window + tray icon */
#include "overlay_host.h"    /* --overlay: a plugin paints layers over the picture */

/* registry.h is the single TU that defines g_profiles[] / g_profile_count /
 * g_active_profile and pulls in every per-game profile header. */
#include "registry.h"

static char g_rom_path[512] = {0};
static char g_profile_arg[64] = {0};   /* --profile <id>: e.g. sfight for STF's arcade game */
static int  g_autorun = 0;
static int  g_browse_model = -1;   /* --model N: open single-model browser on N */
static int  g_objview_on    = 0;   /* --objview: open the object viewer at boot */
static int  g_objview_model = -1;  /* --objview N: and select model N */
static int  g_mcp_enable = 0;      /* --mcp: start the TCP debug server */
static int  g_mcp_port   = 7172;   /* --mcp-port N */
static int  g_headless   = 0;      /* --headless: no window, GPU or audio device */
static int  g_net_window = 0;      /* --netplay: open the netplay window at startup */
static int  g_kiosk_on   = 0;      /* --kiosk: capture mode from startup */
static int  g_no_tray    = 0;      /* --no-tray: --headless without its icon */
static int  g_kiosk_w    = KIOSK_DEFAULT_WIDTH;
static int  g_kiosk_h    = KIOSK_DEFAULT_HEIGHT;
static int  g_kiosk_show = 0;      /* --kiosk-show: start it on screen, not parked */
static int  g_av_port    = 0;      /* --av-port N: raw A/V server, 0 = off */
static int  g_av_w       = AV_DEFAULT_WIDTH;
static int  g_av_h       = AV_DEFAULT_HEIGHT;
static int  g_av_mute    = 0;      /* --av-mute: stream the sound, do not play it */

/*
 * Headless netplay (--net-*). The GUI is the normal way in, but a session that
 * can only be started by clicking cannot be scripted, and netplay is precisely
 * the feature where "does it work" means running two of them against a server.
 * These fill the same config the window edits and post the same commands.
 */
static netplay_config_t g_net_cli = { .port = RPCN_DEFAULT_PORT, .frame_delay = 2 };
static int g_net_auto   = 0;      /* 0 none, 1 host, 2 join */
static int g_net_start  = 0;      /* --net-start: begin the session once linked */
static int g_net_twitch = 0;      /* --net-twitch: run the Twitch device flow at boot */

static struct {
    sg_pass_action   pass_action;
    bool             show_demo;
    bool             show_memview;
    bool             show_cpu;
    bool             show_breakpoints;
    bool             show_bus_stats;
    bool             show_cop;
    memory_bus_t     bus;
    i960_cpu_t       cpu;
    emu_thread_ctx_t emu;
    bool             emu_started;
    romset_t         romset;
    ImGuiFileDialog *file_dialog;
    video_state_t    video;
    geo3d_state_t    geo3d;
    bool             show_geo3d;
    bool             show_objview;
    bool             show_m68k_cpu;
    bool             show_m68k_mem;
    bool             show_debug;
    bool             show_netplay;
    bool             always_show_menu;  /* Debug -> Always show menu bar */
    m68k_state_t     m68k_snapshot;   /* prev-frame 68K state for change highlights */
} state;

static void emu_ensure_started(void) {
    if (!state.emu_started) {
        emu_thread_init(&state.emu, &state.cpu, &state.bus);
        mcp_bridge_init(&state.emu, &state.cpu, &state.bus);
        mcp_bridge_set_romset(&state.romset);
        if (g_mcp_enable) mcp_bridge_start(g_mcp_port);
        state.emu_started = true;
    }
}

/* ---- ROM loading -------------------------------------------------------- */

/* The profile to start with: --profile's, or the first registered. */
static const game_profile_t *startup_profile(void) {
    if (g_profile_arg[0]) {
        const game_profile_t *p = profile_by_id(g_profile_arg);
        if (p) return p;
        LOG_WARN("--profile %s: no such profile; using the default", g_profile_arg);
    }
    return g_profile_count > 0 ? g_profiles[0] : NULL;
}

/* Pick the profile that runs the zip's ROM set, by basename (e.g. "fvipers.zip"
 * → fvipers). The active profile is kept when it runs that set, so --profile or
 * a Game-menu choice between STF's Console and Arcade survives the load; else
 * the set's default. Falls back to leaving g_active_profile unchanged if no
 * profile runs the set. */
static void select_profile_for_zip(const char *path) {
    const char *sep_f = strrchr(path, '/');
    const char *sep_b = strrchr(path, '\\');
    const char *base  = (sep_f > sep_b ? sep_f : sep_b);
    base = base ? base + 1 : path;
    char id[64] = {0};
    const char *dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    if (n >= sizeof(id)) n = sizeof(id) - 1;
    memcpy(id, base, n);
    const game_profile_t *p = profile_for_rom_set(id, g_active_profile);
    if (!p) return;
    if (g_active_profile != p)
        LOG_INFO("auto-selected profile: %s", p->display_name);
    g_active_profile = p;
}

static void load_active_profile(const char *primary_zip) {
    select_profile_for_zip(primary_zip);
    if (!g_active_profile) { LOG_ERROR("no active profile selected"); return; }

    /* MAME clone fall-through: build the parent zip path (same directory as the
     * picked zip) from profile->parent_zip_name. NULL = self-contained set. */
    char parent_zip[512] = {0};
    const char *parent_zip_ptr = NULL;
    if (g_active_profile->parent_zip_name) {
        const char *sep_f = strrchr(primary_zip, '/');
        const char *sep_b = strrchr(primary_zip, '\\');
        const char *sep = sep_f > sep_b ? sep_f : sep_b;
        if (sep) {
            size_t dir_len = (size_t)(sep - primary_zip + 1);
            if (dir_len < sizeof(parent_zip) - 32) {
                memcpy(parent_zip, primary_zip, dir_len);
                strcat(parent_zip, g_active_profile->parent_zip_name);
            }
        } else {
            strncpy(parent_zip, g_active_profile->parent_zip_name, sizeof(parent_zip) - 1);
        }
        parent_zip_ptr = parent_zip;
    }

    /* Stop the emu thread while we re-install the ROM: install_fn re-inits the
     * bus and resets the CPU, which must not race the run loop. */
    if (state.emu_started) { emu_stop(&state.emu); emu_sleep_ms(10); }

    if (g_active_profile->load_fn(&state.romset, primary_zip, parent_zip_ptr) == 0) {
        g_active_profile->install_fn(&state.romset, &state.cpu, &state.bus);
        /* Bring up the 68K sound block: attach the MIDI/SCSP bus callbacks, load
         * the 68K program ROM + PCM sample ROM. (install_fn re-inits the bus, so
         * this must run after it.) */
        if (g_active_profile->quirks.enable_68k_sound) {
            sound_reset();
            sound_attach(&state.bus);
            if (state.romset.audiocpu && state.romset.audiocpu_size > 0)
                sound_load_rom(state.romset.audiocpu, (uint32_t)state.romset.audiocpu_size);
            if (state.romset.samples && state.romset.samples_size > 0)
                sound_load_samples(state.romset.samples, (uint32_t)state.romset.samples_size);
        }
        /* Inputs are delivered via the I/O ports (read by the game's vblank
         * interrupt), so attach the I/O read callback after the bus re-init. */
        input_reset();
        input_attach(&state.bus);
        /* geo_displaylist profiles emit camera-space geometry: start from an identity
         * view + focal-matched fov (live-tunable via the set_camera bridge cmd). */
        if (g_active_profile->quirks.geo_displaylist) {
            state.geo3d.cam_x = state.geo3d.cam_y = state.geo3d.cam_z = 0.0f;
            state.geo3d.rot_x = state.geo3d.rot_y = 0.0f;
            state.geo3d.fov_deg = 65.0f;
        }
        emu_ensure_started();
        emu_update_snapshots(&state.emu);
        kiosk_set_label(g_active_profile->display_name);
        LOG_INFO("profile '%s' loaded; reset IP = 0x%08X",
                 g_active_profile->id, state.cpu.sfr.ip);
    }
}

/*
 * The board reset netplay performs at the barrier.
 *
 * A netplay session starts from a COLD BOOT on both machines, because that is
 * the only state two emulators can be certain to share without savestates (see
 * net/netplay.h). This is that reset: everything load_active_profile does after
 * the ROM files are read, and nothing that touches the emu thread — it is CALLED
 * BY the emu thread, with the emu mutex held.
 *
 * install_fn re-runs mem_init, which reallocates and zeroes every region and
 * calls cop_reset() for the COP/SHARC state. The rest is what mem_init does not
 * reach: the interrupt controller, the sound board, the input latch and the run
 * loop's own per-boot flags.
 */
static void netplay_reset_board_cb(void *ctx) {
    (void)ctx;
    if (!g_active_profile || !state.romset.loaded) return;

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

/*
 * Drives the --net-* flags. Called every frame from both the windowed and the
 * headless loop; posts exactly the commands the netplay window's buttons post,
 * as the session reaches each stage. Inert unless --net-server was given.
 */
static void netplay_cli_pump(void) {
    if (!g_net_cli.server[0]) return;

    static int      connected = 0;
    static int      acted     = 0;
    static int      started   = 0;
    static netplay_state_t last = (netplay_state_t)-1;

    if (!connected) {
        if (!state.romset.loaded) return;   /* the profile decides the lobby space */
        connected = 1;
        /* Twitch signs in and then connects itself, so it replaces the connect
         * rather than preceding it. */
        netplay_post(g_net_twitch ? NETPLAY_CMD_TWITCH_START : NETPLAY_CMD_CONNECT, &g_net_cli);
        return;
    }

    netplay_status_t st;
    netplay_get_status(&st);

    if (g_net_twitch) {
        static rpcn_twitch_state_t tw_last = (rpcn_twitch_state_t)-1;
        if (st.twitch_state != tw_last) {
            tw_last = st.twitch_state;
            if (st.twitch_state == RPCN_TWITCH_WAITING)
                LOG_INFO("netplay: Twitch code %s -- approve it at %s",
                         st.twitch_user_code, st.twitch_uri);
            else if (st.twitch_state == RPCN_TWITCH_DONE)
                LOG_INFO("netplay: Twitch signed in as %s", st.twitch_npid);
            else if (st.twitch_state == RPCN_TWITCH_FAILED)
                LOG_INFO("netplay: Twitch failed: %s", st.twitch_error);
        }
    }

    if (st.state != last) {
        last = st.state;
        LOG_INFO("netplay: state = %s", netplay_state_text(st.state));
        if (st.state == NETPLAY_IN_ROOM)
            LOG_INFO("netplay: room %llu — a peer joins it with --net-join %llu",
                     (unsigned long long)st.room_id, (unsigned long long)st.room_id);
    }

    if (!acted && st.state == NETPLAY_ONLINE) {
        if (g_net_auto == 1)      { acted = 1; netplay_post(NETPLAY_CMD_HOST, &g_net_cli); }
        else if (g_net_auto == 2) { acted = 1; netplay_post(NETPLAY_CMD_JOIN, &g_net_cli); }
        else                      { acted = 1; netplay_post(NETPLAY_CMD_SEARCH, &g_net_cli); }
    }

    /* Start only once both ends can actually reach each other: announcing into a
     * void just logs "still waiting" every five seconds. */
    if (g_net_start && !started && st.state == NETPLAY_IN_ROOM && st.peer_heard) {
        started = 1;
        netplay_post(NETPLAY_CMD_START, &g_net_cli);
    }
}

static void open_rom_dialog(void) {
    struct IGFD_FileDialog_Config cfg = IGFD_FileDialog_Config_Get();
    cfg.path = ".";
    cfg.countSelectionMax = 1;
    cfg.flags = ImGuiFileDialogFlags_Modal;
    IGFD_OpenDialog(state.file_dialog, "OpenROM", "Open ROM Set (.zip)", ".zip", cfg);
}

static void draw_file_dialog(void) {
    ImVec2 min_size = {600, 400};
    ImVec2 max_size = {1280, 800};
    if (IGFD_DisplayDialog(state.file_dialog, "OpenROM", 0, min_size, max_size)) {
        if (IGFD_IsOk(state.file_dialog)) {
            char *picked = IGFD_GetFilePathName(state.file_dialog, IGFD_ResultMode_AddIfNoFileExt);
            if (picked) {
                /* load_active_profile auto-selects the profile from the basename. */
                load_active_profile(picked);
                free(picked);
            }
        }
        IGFD_CloseDialog(state.file_dialog);
    }
}

/* ---- UI ----------------------------------------------------------------- */

static void draw_bus_stats_window(void) {
    igSetNextWindowSize((ImVec2){320, 140}, ImGuiCond_FirstUseEver);
    if (!igBegin("Memory bus", &state.show_bus_stats, 0)) { igEnd(); return; }
    igText("regions: %d / %d", state.bus.region_count, MEM_REGIONS_MAX);
    igText("reads:   %llu (unmapped %llu)",
           (unsigned long long)state.bus.reads,
           (unsigned long long)state.bus.unmapped_reads);
    igText("writes:  %llu (unmapped %llu)",
           (unsigned long long)state.bus.writes,
           (unsigned long long)state.bus.unmapped_writes);
    igEnd();
}

/*
 * Whether the main menu bar is on screen this frame.
 *
 * The bar is opaque and sits over the top of the game, which is why the viewport
 * below reserves a strip for it - so while a game is actually running it gets out
 * of the way, and the emulator fills the window. Reaching the top edge with the
 * pointer brings it back.
 *
 * It is NOT hidden when there is nothing to play or when the board is paused.
 * Losing the menu on a paused emulator would mean losing Run, Load ROMs and this
 * very option at the moment someone is most likely looking for them, and the
 * screen is not doing anything worth the room.
 *
 * Decided once per frame and cached, because the game viewport has to reserve
 * exactly the strip the bar will occupy: computing it twice lets the two answers
 * differ on the frame the pointer crosses the edge, which shows up as a one-frame
 * jump in the picture.
 */
static bool s_menu_bar_visible = true;

static bool menu_bar_should_show(void) {
    if (state.always_show_menu) return true;
    if (!state.romset.loaded || !state.emu_started) return true;
    if (!emu_is_running(&state.emu)) return true;

    /* A dropdown is open. Without this the bar vanishes the moment the pointer
     * moves down onto the menu it just opened, taking the menu with it. */
    if (igIsPopupOpen(NULL, ImGuiPopupFlags_AnyPopup)) return true;

    /* The pointer is in the strip the bar occupies. ImGui reports (-FLT_MAX,
     * -FLT_MAX) when the pointer is outside the window, which compares as "above
     * the top edge" and would pin the bar on whenever the mouse left.
     *
     * The strip is deliberately TALLER than the bar. Throwing the pointer at the
     * top of the screen is how everyone reaches for a hidden menu, and in a
     * window that lands on the title bar - outside the client area, where ImGui
     * sees nothing at all. A few pixels of slack mean the flick that overshoots
     * and comes back still catches it, and costs nothing: the strip is only
     * consulted while a game is running and the bar is already hidden. */
    if (!igIsMousePosValid(NULL)) return false;
    return igGetIO()->MousePos.y <= igGetFrameHeight() + 8.0f;
}

static void draw_menu_bar(void) {
    if (!s_menu_bar_visible) return;
    if (!igBeginMainMenuBar()) return;
    if (igBeginMenu("File")) {
        if (igMenuItem("Load ROMs...")) open_rom_dialog();
        igSeparator();
        /* Capture mode: the window loses its chrome, goes to the capture size
         * and parks off the desktop, and a tray icon becomes the way back. */
        if (igMenuItemEx("Capture mode (OBS)", NULL, false, true))
            kiosk_enter(g_kiosk_w, g_kiosk_h, false);
        igSeparator();
        if (igMenuItemEx("Quit", "Esc", false, true)) sapp_request_quit();
        igEndMenu();
    }
    if (igBeginMenu("Profile")) {
        for (size_t i = 0; i < g_profile_count; i++) {
            bool sel = (g_active_profile == g_profiles[i]);
            if (igMenuItemBoolPtr(g_profiles[i]->display_name, NULL, &sel, true))
                g_active_profile = g_profiles[i];
        }
        igEndMenu();
    }
    if (igBeginMenu("Emulation")) {
        bool can_run  = state.emu_started && state.romset.loaded && !state.cpu.halted;
        bool running  = state.emu_started && emu_is_running(&state.emu);
        bool can_step = can_run && !running;
        if (running) {
            if (igMenuItemEx("Pause", "F9", false, true)) emu_stop(&state.emu);
        } else {
            if (igMenuItemEx("Run",   "F9", false, can_run))  emu_run(&state.emu);
        }
        if (igMenuItemEx("Step",     "F5", false, can_step)) emu_step(&state.emu, 1);
        if (igMenuItemEx("Step 10",  "F6", false, can_step)) emu_step(&state.emu, 10);
        if (igMenuItemEx("Step 100", "F7", false, can_step)) emu_step(&state.emu, 100);
        igSeparator();
        igMenuItemBoolPtr("Break on warning", NULL, (bool *)&g_log.break_on_warn, true);
        igEndMenu();
    }
    if (igBeginMenu("Debug")) {
        igMenuItemBoolPtr("CPU registers",    NULL, &state.show_cpu,         true);
        igMenuItemBoolPtr("Memory viewer",    NULL, &state.show_memview,     true);
        igMenuItemBoolPtr("Memory bus stats", NULL, &state.show_bus_stats,   true);
        igMenuItemBoolPtr("Breakpoints",      NULL, &state.show_breakpoints, true);
        igMenuItemBoolPtr("COP diagnostics",  NULL, &state.show_cop,         true);
        igMenuItemBoolPtr("Break on unknown COP cmd", NULL, (bool*)&g_sharc.break_on_unknown, true);
        igMenuItemBoolPtr("3D viewer",        NULL, &state.show_geo3d,       true);
        igMenuItemBoolPtr("Object viewer",    NULL, &state.show_objview,     true);
        if (igMenuItem("Dump 3D captures")) geo3d_log_captures(&state.geo3d);
        if (igMenuItem("Dump COP stream"))  geo3d_dump_capture_stream();
        igMenuItemBoolPtr("68K sound CPU",    NULL, &state.show_m68k_cpu,    true);
        igMenuItemBoolPtr("68K memory viewer", NULL, &state.show_m68k_mem,   true);
        igMenuItemBoolPtr("Log sound writes", NULL, &g_sound.log_writes,     true);
        { bool ws = g_warning_skip != 0;  if (igMenuItemBoolPtr("Warning-screen skip", NULL, &ws, true)) g_warning_skip = ws; }
        { bool cl = g_cam_log != 0;        if (igMenuItemBoolPtr("Log camera CSV",      NULL, &cl, true)) g_cam_log = cl; }
        igSeparator();
        igMenuItemBoolPtr("Always show menu bar", NULL, &state.always_show_menu, true);
        igSeparator();
        igMenuItemBoolPtr("CPU opcode tests", NULL, &state.show_debug, true);
        igMenuItemBoolPtr("ImGui demo window", NULL, &state.show_demo, true);
        igEndMenu();
    }

    /* Netplay gets a menu of its own rather than a line in Debug: it is the one
     * feature here a player rather than a developer reaches for, and the status
     * line is worth being able to read without opening the window — whether the
     * peer is reachable is the question people actually have. */
    if (igBeginMenu("Netplay")) {
        igMenuItemBoolPtr("Netplay window", NULL, &state.show_netplay, true);
        igSeparator();
        {
            netplay_status_t st;
            netplay_get_status(&st);
            igTextDisabled("%s", netplay_state_text(st.state));
            if (st.room_id)
                igTextDisabled("room %llu (%u of %u%s)", (unsigned long long)st.room_id,
                               st.member_count, st.max_slot, st.is_host ? ", yours" : "");
            if (st.peer_npid[0])
                igTextDisabled("peer %s %s", st.peer_npid,
                               st.peer_heard ? "[reachable]"
                                             : st.peer_known ? "[punching]" : "[no address]");
            if (netplay_state_running(st.state))
                igTextDisabled("frame %u, %u stall%s", st.frame, st.stalls,
                               st.stalls == 1 ? "" : "s");
            if (st.error[0]) igTextDisabled("%s", st.error);
        }
        igEndMenu();
    }

    /* Right-side status display (reads the UI snapshot, not the live CPU). */
    if (g_active_profile) {
        igSeparator();
        const char *runstate = "no ROM";
        if (state.romset.loaded)
            runstate = state.cpu.halted ? "halted"
                     : (state.emu_started && emu_is_running(&state.emu) ? "running" : "stopped");
        igText("  %s | %s | IP=0x%08X | %u steps/s",
               g_active_profile->id, runstate,
               state.emu.cpu_snapshot.sfr.ip,
               state.emu.steps_per_second);
    }
    igEndMainMenuBar();
}

/* The tray menu's Run item drives the emu thread through these. */
static bool kiosk_is_running_cb(void *ud) {
    (void)ud;
    return state.emu_started && emu_is_running(&state.emu);
}
static void kiosk_set_running_cb(void *ud, bool run) {
    (void)ud;
    if (!state.emu_started || !state.romset.loaded) return;
    if (run) { if (!emu_is_running(&state.emu) && !state.cpu.halted) emu_run(&state.emu); }
    else if (emu_is_running(&state.emu)) emu_stop(&state.emu);
}

/* Tray item: reboot the 68000 + SCSP, leaving the rest of the board running.
 * The driver can lose its command stream and go permanently silent; this gets
 * the music back without dropping the session. It returns silent until the
 * game's next music cue. */
static void kiosk_restart_sound_cb(void *ud) {
    (void)ud;
    if (!state.emu_started) return;
    emu_sound_restart(&state.emu);
}

static void init(void) {
    log_init();
    LOG_INFO("m2-hle starting (Phase 4: ROM load + profile resolution; %zu profile(s))",
             g_profile_count);

    /* Before anything slow: sokol has already created AND SHOWN the window, so
     * capture mode has to claim it now — parking it after the ROM load would
     * flash a blank 1080p window across the desktop for as long as that takes.
     * kiosk_window_ready subclasses the window (the tray callback, and
     * swallowing close/minimise) and gives it the app's own icon. */
    kiosk_set_hooks(&(kiosk_hooks_t){
        .is_running    = kiosk_is_running_cb,
        .set_running   = kiosk_set_running_cb,
        .restart_sound = kiosk_restart_sound_cb,
    });
    kiosk_window_ready();
    if (g_kiosk_on) kiosk_enter(g_kiosk_w, g_kiosk_h, g_kiosk_show != 0);

    mem_init(&state.bus, NULL, 0);
    i960_reset(&state.cpu);
    bp_init();
    wp_init();

    /* Default to the first registered profile so File→Load ROMs has somewhere
     * to install into without the user clicking through Profile first. */
    g_active_profile = startup_profile();

    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });
    simgui_setup(&(simgui_desc_t){ .logger.func = slog_func });
    state.file_dialog = IGFD_Create();
    video_init(&state.video);
    game_render_init();
    geo3d_init(&state.geo3d);
    g_geo3d_state = &state.geo3d;
    objview_init();
    if (g_objview_on) {
        state.show_objview = true;
        g_objview.v.active = 1;
        if (g_objview_model >= 0) g_objview.v.model = g_objview_model;
    }
    if (g_browse_model >= 0) {            /* --model N: browse + dump its texture tiles */
        state.geo3d.use_captures = false;
        state.geo3d.model_index  = g_browse_model;
        g_dump_model_tex         = g_browse_model;
    }

    /* Host audio output, drained from the sound board's sample ring. --av-mute
     * leaves the device unopened: a machine that is streaming does not need to
     * play through its own speakers, and the A/V tap sits at the producer, so
     * the stream is unaffected by there being no drain. */
    if (g_av_mute) LOG_INFO("audio: --av-mute, no output device opened");
    else           audio_out_init();

    /* The raw A/V server, and the offscreen target it captures from. The
     * target is made here, with the renderer, so the first board frame after a
     * client connects already has somewhere to be drawn. */
    if (g_av_port > 0) {
        if (av_stream_start(g_av_port, g_av_w, g_av_h)) {
            if (!av_capture_init(av_stream_width(), av_stream_height())) {
                LOG_ERROR("av: no video path — stopping the server rather than "
                          "streaming sound against a still picture");
                av_stream_shutdown();
            }
        }
    }

    /* The overlay plugin, if one was asked for. After game_render_init because
     * its layers are GPU images, and before the emu thread because a plugin
     * that wants a feed should be connecting while the ROM loads. */
    overlay_host_init();

    /* Netplay is inert until a session is asked for, but the emu thread polls it
     * every slice, so it has to exist before the thread starts. */
    netplay_init();
    netplay_set_reset_hook(netplay_reset_board_cb, NULL);
    state.show_netplay = g_net_window != 0;

    /* Start the (initially STOPPED) emu thread up front so Run/Step work even
     * before a ROM is chosen via the menu. */
    emu_ensure_started();

    if (g_rom_path[0]) {
        load_active_profile(g_rom_path);
        if (g_autorun && state.romset.loaded) emu_run(&state.emu);
    }

    state.pass_action = (sg_pass_action){
        .colors[0] = {
            .load_action = SG_LOADACTION_CLEAR,
            .clear_value = { 0.08f, 0.10f, 0.14f, 1.0f },
        },
        /* Clear depth to 1.0 — the 3D solid-fill pipeline depth-tests against it. */
        .depth = {
            .load_action = SG_LOADACTION_CLEAR,
            .clear_value = 1.0f,
        },
    };

#if defined(_WIN32)
    /* Grab keyboard focus on launch so the user doesn't have to click the window
     * before input works. SetForegroundWindow on its own is usually blocked by
     * Windows' focus-stealing guard, so briefly attach our input queue to the
     * current foreground thread's to be granted the foreground. Capture mode
     * does its own placement, and a parked window has no business taking the
     * foreground, so this is skipped there. */
    if (!g_kiosk_on) {
        HWND hwnd = (HWND)sapp_win32_get_hwnd();
        if (hwnd) {
            HWND  fg     = GetForegroundWindow();
            DWORD fg_tid = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
            DWORD my_tid = GetCurrentThreadId();
            if (fg_tid && fg_tid != my_tid) AttachThreadInput(fg_tid, my_tid, TRUE);
            ShowWindow(hwnd, SW_SHOW);
            BringWindowToTop(hwnd);
            SetForegroundWindow(hwnd);
            SetFocus(hwnd);
            if (fg_tid && fg_tid != my_tid) AttachThreadInput(fg_tid, my_tid, FALSE);
        }
    }
#endif
}

/* ---- Headless GPU, for --headless --av-port -------------------------------
 *
 * A server has no desktop session, so the A/V stream's ideal shape is no
 * window and no swapchain at all: a graphics device on its own, rendering into
 * the capture target and nowhere else. sokol_gfx will adopt a device somebody
 * else made (sg_environment.d3d11), so all this has to do is make one — and a
 * D3D11 device without a swapchain is just D3D11CreateDevice with no DXGI.
 *
 * The defaults handed to sg_setup are what every pipeline in game_render.h is
 * built against and what ui/av_capture.h then matches its target to. BGRA8 is
 * the format the wire wants, so choosing it here is also what makes the
 * readback a straight memcpy.
 *
 * Only the D3D11 backend. A GL build would need a surfaceless EGL context,
 * which is a different piece of work and not one this machine needs; --kiosk
 * covers the windowed case there.
 */
#if defined(SOKOL_D3D11)
static ID3D11Device        *g_hl_dev;
static ID3D11DeviceContext *g_hl_ctx;

/*
 * Sleep(1) is 15.6 ms unless something in the process has asked Windows for a
 * finer timer, and a headless run has asked for nothing: no window, no audio
 * device. Polling the board's frame clock every 15.6 ms against a 16.7 ms
 * frame catches barely half of them — measured, 37 of 60. A high-resolution
 * waitable timer gives a real millisecond without changing the timer
 * resolution for the whole machine, and needs nothing past kernel32. Where it
 * cannot be made (pre-1803), Sleep is still there and the stream still runs,
 * just with more gaps.
 */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#  define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
static HANDLE g_hl_timer;

static void headless_sleep_ms(int ms) {
    if (g_hl_timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)ms * 10000;    /* relative, 100 ns units */
        if (SetWaitableTimer(g_hl_timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(g_hl_timer, (DWORD)ms + 10);
            return;
        }
    }
    Sleep((DWORD)ms);
}

static bool headless_gpu_init(void) {
    D3D_FEATURE_LEVEL got = 0;
    g_hl_timer = CreateWaitableTimerExW(NULL, NULL,
                                        CREATE_WAITABLE_TIMER_MANUAL_RESET |
                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
    if (!g_hl_timer)
        LOG_WARN("av: no high-resolution timer here - the frame poll falls back to "
                 "Sleep(1), which this machine rounds up to ~15.6 ms");
    UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED;   /* one thread renders */
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags,
                                   NULL, 0, D3D11_SDK_VERSION,
                                   &g_hl_dev, &got, &g_hl_ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, flags,
                               NULL, 0, D3D11_SDK_VERSION,
                               &g_hl_dev, &got, &g_hl_ctx);
        if (SUCCEEDED(hr))
            LOG_WARN("av: no hardware D3D11 device (no session?) - using WARP, which is software");
    }
    if (FAILED(hr) || !g_hl_dev || !g_hl_ctx) {
        LOG_ERROR("av: D3D11CreateDevice failed (0x%08lX)", (unsigned long)hr);
        return false;
    }
    sg_setup(&(sg_desc){
        .environment = {
            .defaults = { .color_format = SG_PIXELFORMAT_BGRA8,
                          .depth_format = SG_PIXELFORMAT_DEPTH_STENCIL,
                          .sample_count = 1 },
            .d3d11    = { .device = g_hl_dev, .device_context = g_hl_ctx },
        },
        .logger.func = slog_func,
    });
    if (!sg_isvalid()) { LOG_ERROR("av: sokol_gfx setup failed on the headless device"); return false; }
    game_render_init();
    video_init(&state.video);
    LOG_INFO("av: headless graphics up (no window, no swapchain); tiles on the %s",
             state.video.gpu ? "GPU" : "CPU");
    return true;
}
#else
static void headless_sleep_ms(int ms) { emu_sleep_ms(ms); }
static bool headless_gpu_init(void) {
    LOG_ERROR("av: --headless --av-port needs the D3D11 backend; use --kiosk on this one");
    return false;
}
#endif

/* --headless: the emulator and its MCP bridge and nothing else. The graders in
 * tools/ drive the game over the bridge and read what they need out of memory
 * and the display list, so a window, a GPU context and an audio device are only
 * a window popping up and taking focus on every run. Sound still runs on the
 * board (the graders capture it); with no device draining its output ring the
 * board drops the samples. The display list is not scanned into 3D models, so
 * get_geo_captures has nothing to report. Runs until the process is killed.
 *
 * --av-port changes that: it brings up a GPU device with no window and no
 * swapchain (headless_gpu_init above) and renders one frame per board frame
 * into the capture target, so a machine with no desktop session can stream.
 * The A/V tap sits at the sound producer, so the samples come through with or
 * without an audio device. */
static int headless_main(void) {
    log_init();
    if (!g_rom_path[0]) { LOG_ERROR("--headless needs --rom"); return 2; }
    if (g_kiosk_on)
        LOG_WARN("--kiosk ignored: --headless has no window to capture. Drop --headless "
                 "to record; OBS hooks a swapchain, so it needs a real window.");
    mem_init(&state.bus, NULL, 0);
    i960_reset(&state.cpu);
    bp_init();
    wp_init();
    g_active_profile = startup_profile();
    geo3d_init(&state.geo3d);
    g_geo3d_state = &state.geo3d;
    objview_init();
    netplay_init();
    netplay_set_reset_hook(netplay_reset_board_cb, NULL);
    netplay_set_open_browser(false);   /* no desktop here - the log carries the URL */

    bool av_on = false;
    if (g_av_port > 0) {
        if (!headless_gpu_init()) return 3;
        if (!av_stream_start(g_av_port, g_av_w, g_av_h)) return 3;
        if (!av_capture_init(av_stream_width(), av_stream_height())) {
            av_stream_shutdown();
            return 3;
        }
        overlay_host_init();
        av_on = true;
    }

    emu_ensure_started();
    load_active_profile(g_rom_path);
    if (!state.romset.loaded) { LOG_ERROR("--headless: ROM set did not load"); return 1; }
    if (g_autorun) emu_run(&state.emu);

    /*
     * The notification-area icon. A headless run has no window and no console
     * once whatever started it goes away, so without this the only way to stop
     * one is Task Manager -- and an orphan sits there holding its ports, its
     * ROM and its A/V socket. The same two items capture mode has: Exit, and
     * Restart sound board.
     *
     * The hooks are the kiosk's; they are about the emu thread, not about a
     * window, and the tray menu is the only thing that drives either.
     */
    if (!g_no_tray) {
        kiosk_set_hooks(&(kiosk_hooks_t){
            .is_running    = kiosk_is_running_cb,
            .set_running   = kiosk_set_running_cb,
            .restart_sound = kiosk_restart_sound_cb,
        });
        char note[128];
        int  o = 0;
        if (g_mcp_enable) o += snprintf(note + o, sizeof note - (size_t)o,
                                        "--mcp %d", g_mcp_port);
        if (g_av_port > 0) snprintf(note + o, sizeof note - (size_t)o,
                                    "%s--av-port %d", o ? ", " : "", g_av_port);
        tray_headless_start(note);
        if (g_active_profile && g_active_profile->display_name)
            kiosk_set_label(g_active_profile->display_name);
    }
    LOG_INFO("headless: running%s%s", g_mcp_enable ? " with the MCP bridge" : "",
             av_on ? " with the A/V server"
                   : (g_mcp_enable ? "" : " (no --mcp: nothing can drive it)"));
    if (av_on && !g_autorun && !g_mcp_enable)
        LOG_WARN("av: the board is stopped and nothing can start it - a client will "
                 "get the stream header and then silence. Add --run.");
    while (!tray_exit_requested() && !mcp_quit_requested()) {
        emu_update_snapshots(&state.emu);
        netplay_cli_pump();
        tray_headless_pump();
        tray_headless_tick(g_emu_frames);
        if (av_on) {
            /* One render per board frame, and only while somebody is reading:
             * with no client this is a poll loop and nothing else. */
            uint64_t av_frame = 0, av_sample = 0;
            if (av_capture_due(&av_frame, &av_sample) && av_stream_active()) {
                game_frame_prepare(&state.video, &state.geo3d, &state.bus,
                                   &state.romset, true);
                /* With an overlay loaded the board is letterboxed into the
                 * target so the plugin has margin to live in; with none it is
                 * drawn across the whole target exactly as before. The pass
                 * action clears to black, so the columns have the right
                 * background before the plugin paints a thing. */
                int ox = 0, oy = 0, gw = av_stream_width(), gh = av_stream_height();
                overlay_host_game_rect(av_stream_width(), av_stream_height(), 0,
                                       &ox, &oy, &gw, &gh);
                /* Outside the pass: sg_update_image cannot run inside one. */
                overlay_host_paint(av_stream_width(), av_stream_height(),
                                   ox, oy, gw, gh, 0, g_emu_frames);
                sg_begin_pass(&(sg_pass){
                    .action      = av_capture_action(),
                    .attachments = { .colors[0]     = av_capture_color_att(),
                                     .depth_stencil = av_capture_depth_att() },
                });
                game_frame_draw(&state.video, &state.geo3d, &state.bus, &state.romset,
                                ox, oy, gw, gh, 1.0f);
                overlay_host_draw();
                sg_end_pass();
                av_capture_submit(av_frame, av_sample);
                sg_commit();
            }
            headless_sleep_ms(1);
        } else {
            emu_sleep_ms(5);
        }
    }

    /*
     * Exit from the tray. Everything comes down in the same order cleanup()
     * uses for a windowed run, and for the same reasons: the A/V writer thread
     * is still sending out of buffers the renderer owns, and the audio tap
     * runs on the emu thread, which is still going at this point.
     */
    LOG_INFO("headless: shutting down");
    av_stream_shutdown();
    if (state.emu_started) emu_thread_shutdown(&state.emu);
    netplay_shutdown();
    overlay_host_shutdown();
    av_capture_shutdown();
    romset_free(&state.romset);
    tray_headless_stop();
    return 0;
}

static void frame(void) {
    /* `quit` over the bridge. Capture mode would otherwise swallow the close,
     * which is the point of capture mode -- so say we mean it. */
    if (mcp_quit_requested()) { kiosk_allow_quit(); sapp_request_quit(); }

    simgui_new_frame(&(simgui_frame_desc_t){
        .width       = sapp_width(),
        .height      = sapp_height(),
        .delta_time  = sapp_frame_duration(),
        .dpi_scale   = sapp_dpi_scale(),
    });

    /* Pull a fresh double-buffered CPU snapshot under the mutex so register
     * highlights track live state without tearing. */
    if (state.emu_started) emu_update_snapshots(&state.emu);

    /* Compose the tile layers and scan this frame's 3D (shared with main_sdl.c). */
    game_frame_prepare(&state.video, &state.geo3d, &state.bus, &state.romset,
                       state.emu_started);
    float lerp_t = state.emu_started ? game_frame_lerp() : 1.0f;

    netplay_cli_pump();

    /* Capture mode draws the game and nothing else: no menu bar, no debug
     * windows, nothing for a recording to pick up. */
    const bool draw_ui = !kiosk_active();

    /* Before anything draws: draw_menu_bar() and the game viewport below must
     * agree on the same answer for this frame. Capture mode has no menu bar at
     * all, so it settles the question before the auto-hide rule is consulted —
     * and the viewport then reserves nothing, which is what gives a recording
     * the whole window. */
    s_menu_bar_visible = draw_ui && menu_bar_should_show();

    /* A bridge caller may ask for the object viewer's window: it owns the
     * viewer's settings but not this flag, which is the frontend's. */
    if (g_objview.window_request >= 0) {
        state.show_objview       = (g_objview.window_request != 0);
        g_objview.window_request = -1;
    }

    if (draw_ui) {
        draw_menu_bar();
        draw_file_dialog();

        if (state.show_cpu)
            cpu_window_draw(&state.emu.cpu_snapshot, &state.emu.cpu_prev_snapshot, &state.show_cpu);
        if (state.show_memview)     memview_draw(&state.bus, &state.show_memview);
        if (state.show_breakpoints) bp_window_draw(&state.show_breakpoints);
        if (state.show_cop)         cop_window_draw(&state.show_cop);
        if (state.show_geo3d) {
            const game_quirks_t *gq = g_active_profile ? &g_active_profile->quirks : NULL;
            geo3d_window_draw(&state.geo3d, &state.show_geo3d,
                              state.romset.main_data, state.romset.main_data_size,
                              gq ? gq->model_table_offset : 0,
                              gq ? gq->model_table_count  : 0,
                              gq ? gq->mesh_ptr_subtract  : 0);
        }
        objview_window_draw(&state.show_objview, &state.romset, &state.bus);
        if (state.show_bus_stats)   draw_bus_stats_window();
        if (state.show_m68k_cpu) {
            m68k_window_draw(&g_sound.m68k, &state.m68k_snapshot, &state.show_m68k_cpu);
            state.m68k_snapshot = g_sound.m68k;
        }
        if (state.show_m68k_mem)    m68k_memview_draw(&state.show_m68k_mem);
        if (state.show_debug)       debug_window_draw(&state.show_debug);
        if (state.show_netplay)     netplay_window_draw(&state.show_netplay);
        if (state.show_demo)        igShowDemoWindow(&state.show_demo);
    }

    /*
     * The A/V stream's own pass, before the swapchain's: sokol does not nest
     * passes, and the board — not the display — is what paces this one. It
     * runs exactly once per game frame, at the stream's own resolution, with
     * lerp_t 1 (there is one picture per board frame, so there is nothing to
     * interpolate towards).
     *
     * The window then MIRRORS that target instead of drawing the game a second
     * time. Drawing it twice would double the 3D decode and the fill work for
     * a picture nobody compares; this way the stream and the window are the
     * same frame, and the only thing the window adds is ImGui on top.
     */
    static bool s_av_mirror = false;
    uint64_t av_frame = 0, av_sample = 0;
    /* Decided before the pass opens, because the overlay plugin has to run
     * (and upload) outside one and av_capture_due() latches: it may be asked
     * exactly once a frame. The short-circuit order is the one it always had. */
    const bool av_due = av_stream_enabled() && av_capture_due(&av_frame, &av_sample) &&
                        av_capture_ready() && av_stream_active();
    if (av_due) {
        int ox = 0, oy = 0, gw = av_stream_width(), gh = av_stream_height();
        overlay_host_game_rect(av_stream_width(), av_stream_height(), 0,
                               &ox, &oy, &gw, &gh);
        overlay_host_paint(av_stream_width(), av_stream_height(),
                           ox, oy, gw, gh, 0, g_emu_frames);
        sg_begin_pass(&(sg_pass){
            .action      = av_capture_action(),
            .attachments = { .colors[0]     = av_capture_color_att(),
                             .depth_stencil = av_capture_depth_att() },
        });
        game_frame_draw(&state.video, &state.geo3d, &state.bus, &state.romset,
                        ox, oy, gw, gh, 1.0f);
        overlay_host_draw();
        sg_end_pass();
        av_capture_submit(av_frame, av_sample);
        s_av_mirror = true;
    }
    if (!av_stream_active()) s_av_mirror = false;

    /* The window's own composition, when it is not mirroring the tap. Painted
     * here, before the swapchain pass opens, for the same reason as above; the
     * draw goes in below, after the board. When s_av_mirror is true there is
     * nothing to do — the target already has the overlay baked into it. */
    int win_ox = 0, win_oy = 0, win_gw = 0, win_gh = 0;
    bool win_overlay = false;
    if (!s_av_mirror && overlay_host_loaded()) {
        int menu_h = s_menu_bar_visible ? (int)(igGetFrameHeight() * sapp_dpi_scale()) : 0;
        win_overlay = overlay_host_game_rect(sapp_width(), sapp_height(), menu_h,
                                             &win_ox, &win_oy, &win_gw, &win_gh);
        if (win_overlay)
            overlay_host_paint(sapp_width(), sapp_height(),
                               win_ox, win_oy, win_gw, win_gh, menu_h, g_emu_frames);
    }

    sg_begin_pass(&(sg_pass){
        .action    = state.pass_action,
        .swapchain = sglue_swapchain(),
    });

    /* Draw the game letterboxed into the swapchain, then ImGui on top.
     * Layer order: back-back colour → background tiles → (3D, Phase 9) → FG/HUD. */
    {
        int ox, oy, w, h;
        /* Reserve the top main-menu-bar strip so the game (and its row-0 HUD) isn't
         * occluded by the opaque ImGui bar drawn on top - and reserve nothing when
         * the bar is hidden, which is what gives the game the whole window.
         * s_menu_bar_visible already folds in capture mode, so this covers the
         * --kiosk case that used to be spelled out here. */
        int menu_h = s_menu_bar_visible ? (int)(igGetFrameHeight() * sapp_dpi_scale()) : 0;
        int avail_h = sapp_height() - menu_h;
        if (avail_h < 1) avail_h = 1;
        /* Letterbox against whatever is actually being shown: the stream's
         * target has its own aspect, chosen by --av-size. */
        int src_w = s_av_mirror ? av_stream_width()  : VIDEO_WIDTH;
        int src_h = s_av_mirror ? av_stream_height() : VIDEO_HEIGHT;
        game_render_letterbox(sapp_width(), avail_h, src_w, src_h, &ox, &oy, &w, &h);
        oy += menu_h;
        /* An overlay owns the whole window: the board goes where the plugin
         * was told it would be, so the window and the stream show the identical
         * composition rather than two letterboxes of the same picture. */
        if (win_overlay) { ox = win_ox; oy = win_oy; w = win_gw; h = win_gh; }
        { static int _cs=0; if ((++_cs % 30)==0) {
            for (int i=0;i<state.geo3d.captured_count;i++){ const captured_model_t *cm=&state.geo3d.captured[i];
                if (cm->model_idx==519 || cm->model_idx==2833)
                    LOG_INFO("CAGE m=%d bone=%d clip=%d mat=%d T=(%.2f,%.2f,%.2f) R0=(%.2f,%.2f,%.2f) R1=(%.2f,%.2f,%.2f) R2=(%.2f,%.2f,%.2f)",
                        cm->model_idx, cm->from_bone, cm->has_clip_win, cm->has_matrix,
                        cm->matrix[3],cm->matrix[7],cm->matrix[11],
                        cm->matrix[0],cm->matrix[1],cm->matrix[2],
                        cm->matrix[4],cm->matrix[5],cm->matrix[6],
                        cm->matrix[8],cm->matrix[9],cm->matrix[10]); } } }
        /* Back colour → background tiles → 3D scene → foreground/HUD — unless
         * the A/V pass above has already drawn this frame, in which case the
         * window shows that target rather than redrawing it. */
        if (s_av_mirror)
            game_render_draw_target(av_capture_color_tex(), true, ox, oy, w, h);
        else
            game_frame_draw(&state.video, &state.geo3d, &state.bus, &state.romset,
                            ox, oy, w, h, lerp_t);

        /* The overlay goes UNDER any debug window that is open, which is right:
         * those are for the person at the keyboard and the overlay is for the
         * stream. simgui_render() is below, outside this block. */
        if (win_overlay) overlay_host_draw();

        /* Programmatic per-model texture extractor (--extract N). Re-runs ~once/
         * sec while set, so you can navigate to a scene where the model's texels
         * are loaded; the ROM-side manifest/colours are always correct. */
        if (g_extract_model >= 0 && g_active_profile && state.romset.main_data) {
            static int _ec = 0;
            if ((_ec++ % 60) == 0) {
                const game_quirks_t *q = &g_active_profile->quirks;
                geo3d_extract_model_texture(g_extract_model,
                    state.romset.main_data, state.romset.main_data_size,
                    state.romset.textures,  state.romset.textures_size,
                    state.bus.texram0, state.bus.texram1,
                    q->model_table_offset, q->model_table_count);
                if (g_extract_seq >= 0) g_extract_seq++;
            }
        }
    }

    simgui_render();
    sg_end_pass();

    /* The object viewer opens offscreen passes of its own, so it runs between
     * the swapchain pass and the commit: sokol does not nest passes, and the
     * decode takes the shared wireframe buffer that game_frame_draw has just
     * finished with. It does nothing at all unless its window is open or an
     * MCP request is waiting. */
    if (!draw_ui) g_objview.preview_open = 0;   /* capture mode shows no windows */
    objview_service(&state.romset, &state.bus);

    sg_commit();

    /* Capture mode's sign of life: the presented frame rate, in the tray
     * tooltip and periodically in the log. Inert otherwise. */
    kiosk_frame_tick((float)sapp_frame_duration());
}

static void cleanup(void) {
    kiosk_shutdown();      /* take the tray icon down before the window goes */
    /* First: the writer thread is still sending out of slots the renderer owns
     * and the target below is about to go. It keeps both rings — the audio tap
     * runs on the emu thread, which is still going at this point, and that is
     * exactly the free this must not do (see av_stream_shutdown). The capture
     * target goes with the rest of the GPU resources further down. */
    av_stream_shutdown();
    if (state.emu_started) emu_thread_shutdown(&state.emu);
    netplay_shutdown();   /* after the emu thread: it is the only thing that pumps it */
    audio_out_shutdown();  /* stop audio after the emu thread (no more ring writes) */
    if (state.file_dialog) { IGFD_Destroy(state.file_dialog); state.file_dialog = NULL; }
    romset_free(&state.romset);
    objview_shutdown();
    overlay_host_shutdown();   /* before game_render_shutdown: it owns sg images */
    av_capture_shutdown();
    game_render_shutdown();
    video_shutdown(&state.video);
    simgui_shutdown();
    sg_shutdown();
    mem_shutdown(&state.bus);
    log_file_close();
}

static void event(const sapp_event* ev) {
    simgui_handle_event(ev);

    /* Alt+F4 and the taskbar's Close reach sokol as a quit request. In capture
     * mode only the tray's Exit item is allowed to end the run — the point of
     * the mode is that nothing on the desktop can stop the recording. */
    if (ev->type == SAPP_EVENTTYPE_QUIT_REQUESTED && !kiosk_quit_allowed()) {
        sapp_cancel_quit();
        return;
    }

    /* Game input flows through the emulated I/O ports (read by the game's vblank
     * interrupt) — set/clear the host held mask here. Routed regardless of ImGui
     * focus so arcade muscle-memory isn't eaten by a debug window. */
    if (ev->type == SAPP_EVENTTYPE_KEY_DOWN && !ev->key_repeat) input_key_down((int)ev->key_code);
    if (ev->type == SAPP_EVENTTYPE_KEY_UP)                      input_key_up((int)ev->key_code);

    if (ev->type != SAPP_EVENTTYPE_KEY_DOWN) return;
    switch (ev->key_code) {
        case SAPP_KEYCODE_ESCAPE: if (!kiosk_active()) sapp_request_quit(); break;
        case SAPP_KEYCODE_F9:
            if (emu_is_running(&state.emu)) emu_stop(&state.emu);
            else if (state.emu_started && state.romset.loaded && !state.cpu.halted)
                emu_run(&state.emu);
            break;
        case SAPP_KEYCODE_F5:
            if (!emu_is_running(&state.emu) && state.romset.loaded) emu_step(&state.emu, 1);
            break;
        case SAPP_KEYCODE_F6:
            if (!emu_is_running(&state.emu) && state.romset.loaded) emu_step(&state.emu, 10);
            break;
        case SAPP_KEYCODE_F8:
            /* Reboot the sound board. The driver can lose its command stream and
             * go permanently silent; this recovers it without dropping the run. */
            emu_sound_restart(&state.emu);
            break;
        case SAPP_KEYCODE_F7:
            if (!emu_is_running(&state.emu) && state.romset.loaded) emu_step(&state.emu, 100);
            break;
        default: break;
    }
}

sapp_desc sokol_main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            strncpy(g_rom_path, argv[++i], sizeof(g_rom_path) - 1);
        } else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            strncpy(g_profile_arg, argv[++i], sizeof(g_profile_arg) - 1);
        } else if (strcmp(argv[i], "--region") == 0 && i + 1 < argc) {
            int r = game_region_parse(argv[++i]);   /* japan | usa | export */
            if (r < 0) LOG_WARN("--region %s: expected japan, usa or export; keeping usa", argv[i]);
            else       g_region = r;
        } else if (strcmp(argv[i], "--run") == 0) {
            g_autorun = 1;
        } else if (strcmp(argv[i], "--camlog") == 0) {
            g_cam_log = 1;            /* dump cam_ours.csv per game frame */
        } else if (strcmp(argv[i], "--match-replay") == 0) {
            g_match_replay = 1;       /* attract mode straight to its replay fight */
        } else if (strcmp(argv[i], "--nowarnskip") == 0) {
            g_warning_skip = 0;       /* keep warning screen → frame-align with MAME */
        } else if (strcmp(argv[i], "--realirq") == 0) {
            g_real_irq = 1;           /* tick board timers → real timer ISR delivery */
        } else if (strcmp(argv[i], "--no-mesh-cache") == 0) {
            /* Decode every model in full every frame, as the renderer did
             * before the mesh cache. The cache is meant to be invisible — it
             * replays the same faces, in the same order, with the same
             * arithmetic — so this is the switch to reach for when something
             * draws wrong: if the picture changes, the cache changed it. */
            g_geo3d_mesh_cache = 0;
        } else if (strcmp(argv[i], "--cpu-tiles") == 0) {
            /* Compose the tile layers on the CPU instead of in a shader. The
             * GPU compositor is built only on the GL backends, so this changes
             * nothing on D3D11 or Metal; on a GL desktop it is the same picture
             * by the older and slower route. */
            g_video_force_cpu_tiles = 1;
        } else if (strcmp(argv[i], "--live-timers") == 0) {
            g_irqt_live = 1;          /* board timers count i960 cycles, IRQs mid-slice */
            LOG_INFO("emu: live board timers on");
        } else if (strcmp(argv[i], "--steps-per-slice") == 0 && i + 1 < argc) {
            int n = atoi(argv[++i]);  /* i960 steps a slice may run without a frame edge */
            if (n >= 1000) g_emu_steps_per_slice = n;
            LOG_INFO("emu: %d i960 steps a slice", g_emu_steps_per_slice);
        } else if (strcmp(argv[i], "--objview") == 0) {
            /* Open the object viewer at boot. A number may follow to select a
             * model; without one the viewer opens on whatever it defaults to and
             * waits for the window or the MCP bridge to say what to look at. */
            g_objview_on = 1;
            if (i + 1 < argc && isdigit((unsigned char)argv[i + 1][0]))
                g_objview_model = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            g_browse_model = atoi(argv[++i]);  /* single-model browser on N */
        } else if (strcmp(argv[i], "--extract") == 0 && i + 1 < argc) {
            g_extract_model = atoi(argv[++i]); /* dump model N's tiles+colours */
        } else if (strcmp(argv[i], "--bank") == 0 && i + 1 < argc) {
            g_uv_bank_mode = atoi(argv[++i]);  /* 0=auto 1=sheet0 2=sheet1 3=swap */
        } else if (strcmp(argv[i], "--rombank") == 0 && i + 1 < argc) {
            g_extract_rombank = atoi(argv[++i]); /* texel source = textures ROM bank N */
        } else if (strcmp(argv[i], "--cyclemaps") == 0) {
            g_extract_seq = 0;                   /* number each dump to capture every map */
        } else if (strcmp(argv[i], "--mcp") == 0) {
            g_mcp_enable = 1;                  /* start TCP debug server */
        } else if (strcmp(argv[i], "--mcp-port") == 0 && i + 1 < argc) {
            g_mcp_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--headless") == 0) {
            g_headless = 1;
        } else if (strcmp(argv[i], "--no-tray") == 0) {
            /* For a service or a Session 0 run, where there is no shell to put
             * an icon in and the process is stopped some other way. */
            g_no_tray = 1;
        } else if (strcmp(argv[i], "--kiosk") == 0) {
            /* Capture mode: a chrome-free window at the capture resolution,
             * parked off the desktop, with a tray icon as the only handle on
             * it. For recording — OBS hooks a swapchain, so unlike --headless
             * there has to be a real window; see src/ui/kiosk.h. */
            g_kiosk_on = 1;
        } else if (strcmp(argv[i], "--kiosk-size") == 0 && i + 1 < argc) {
            int kw = 0, kh = 0;
            if (sscanf(argv[++i], "%dx%d", &kw, &kh) == 2 && kw > 0 && kh > 0) {
                g_kiosk_w = kw; g_kiosk_h = kh;
            } else {
                LOG_WARN("--kiosk-size wants WxH (e.g. 1920x1080); keeping %dx%d",
                         g_kiosk_w, g_kiosk_h);
            }
        } else if (strcmp(argv[i], "--kiosk-show") == 0) {
            g_kiosk_show = 1;                  /* start on screen, not parked */
        } else if (strcmp(argv[i], "--av-port") == 0 && i + 1 < argc) {
            /* Raw A/V server: one local client gets BGRA frames and 16-bit
             * stereo samples on one socket, stamped with the board's own
             * sample clock. No encoding, no resampling — see core/av_stream.h.
             * Unlike --kiosk this needs no window to be visible (and with
             * --headless, no window at all). */
            g_av_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--av-size") == 0 && i + 1 < argc) {
            int aw = 0, ah = 0;
            if (sscanf(argv[++i], "%dx%d", &aw, &ah) == 2 && aw > 0 && ah > 0) {
                g_av_w = aw; g_av_h = ah;
            } else {
                LOG_WARN("--av-size wants WxH (e.g. 1396x1080); keeping %dx%d",
                         g_av_w, g_av_h);
            }
        } else if (strcmp(argv[i], "--overlay") == 0 && i + 1 < argc) {
            /* Load a plugin that paints layers over the finished picture
             * (ui/overlay_plugin.h). Without this nothing changes anywhere. */
            overlay_host_set_path(argv[++i]);
        } else if (strcmp(argv[i], "--overlay-args") == 0 && i + 1 < argc) {
            overlay_host_set_args(argv[++i]);
        } else if (strcmp(argv[i], "--overlay-game") == 0 && i + 1 < argc) {
            if (!overlay_host_set_game_rect(argv[++i]))
                LOG_WARN("--overlay-game wants WxH+X+Y (e.g. 1396x1080+262+0); "
                         "keeping the board's own letterbox");
        } else if (strcmp(argv[i], "--overlay-reload") == 0) {
            overlay_host_set_watch(true);
        } else if (strcmp(argv[i], "--av-mute") == 0) {
            g_av_mute = 1;                     /* stream the sound, do not play it */
        } else if (strcmp(argv[i], "--netplay") == 0) {
            g_net_window = 1;                  /* open the netplay window at startup */
        } else if (strcmp(argv[i], "--net-server") == 0 && i + 1 < argc) {
            snprintf(g_net_cli.server, sizeof(g_net_cli.server), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--net-port") == 0 && i + 1 < argc) {
            g_net_cli.port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--net-user") == 0 && i + 1 < argc) {
            snprintf(g_net_cli.npid, sizeof(g_net_cli.npid), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--net-pass") == 0 && i + 1 < argc) {
            snprintf(g_net_cli.password, sizeof(g_net_cli.password), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--net-token") == 0 && i + 1 < argc) {
            snprintf(g_net_cli.token, sizeof(g_net_cli.token), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--net-fingerprint") == 0 && i + 1 < argc) {
            snprintf(g_net_cli.fingerprint, sizeof(g_net_cli.fingerprint), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--net-room-pass") == 0 && i + 1 < argc) {
            snprintf(g_net_cli.room_password, sizeof(g_net_cli.room_password), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--net-delay") == 0 && i + 1 < argc) {
            g_net_cli.frame_delay = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--net-p2p-port") == 0 && i + 1 < argc) {
            /* Two peers in one machine's network namespace cannot both bind 3658.
             * Only useful for a loopback test — RPCN hands out a peer's LOCAL
             * address with 3658 hardcoded, so a non-default port is not usable
             * for same-NAT play between two real machines. */
            g_net_cli.local_p2p_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--net-host") == 0) {
            g_net_auto = 1;
        } else if (strcmp(argv[i], "--net-join") == 0 && i + 1 < argc) {
            g_net_auto = 2;
#ifdef _WIN32
            g_net_cli.room_id = _strtoui64(argv[++i], NULL, 10);
#else
            g_net_cli.room_id = strtoull(argv[++i], NULL, 10);
#endif
        } else if (strcmp(argv[i], "--net-start") == 0) {
            g_net_start = 1;
        } else if (strcmp(argv[i], "--net-config") == 0 && i + 1 < argc) {
            /* A settings file other than the per-user one every copy shares
             * (netplay.h, "Stored settings"): a second account on one machine. */
            netplay_set_config_path(argv[++i]);
        } else if (strcmp(argv[i], "--macro") == 0 && i + 1 < argc) {
            /* One key that presses several buttons at once: --macro a=b1+b2,
             * --macro kp1=p2:b1+b2+b3. Repeatable; see input_combo_parse. */
            char spec[64];
            snprintf(spec, sizeof spec, "%s", argv[++i]);
            char *eq = strchr(spec, '=');
            int kc = -1;
            uint32_t acts = 0;
            if (eq) { *eq = '\0'; kc = input_keycode_by_name(spec); acts = input_combo_parse(eq + 1); }
            if (kc < 0 || !acts || !input_combo_bind(kc, acts))
                LOG_WARN("--macro wants KEY=COMBO (e.g. a=b1+b2, kp1=p2:b1+b2+b3); ignoring '%s'", argv[i]);
        } else if (strcmp(argv[i], "--macros") == 0) {
            /* The usual fighting-game set on the keys beside Z X C V: A = B1+B2,
             * S = B1+B3, D = B2+B3, F = B1+B2+B3 (in STF, P+K, P+B, K+B, P+K+B). */
            input_combo_bind(SAPP_KEYCODE_A, input_combo_parse("b1+b2"));
            input_combo_bind(SAPP_KEYCODE_S, input_combo_parse("b1+b3"));
            input_combo_bind(SAPP_KEYCODE_D, input_combo_parse("b2+b3"));
            input_combo_bind(SAPP_KEYCODE_F, input_combo_parse("b1+b2+b3"));
        } else if (strcmp(argv[i], "--net-twitch") == 0) {
            /* Sign in through Twitch instead of --net-user/--net-pass. The code
             * and the twitch.tv address go to the log, and a browser is opened
             * if there is a desktop to open one on. */
            g_net_twitch = 1;
        }
    }
    if (g_headless) exit(headless_main());
    /* Capture mode wants the game moving, not a first frame held on pause. */
    if (g_kiosk_on) g_autorun = 1;

    /* The window's own icon, from the same art as the tray icon (app_icon.h),
     * rendered at each size rather than downscaled from one bitmap. */
    static uint8_t icon16[16 * 16 * 4], icon32[32 * 32 * 4], icon64[64 * 64 * 4];
    app_icon_render(16, icon16);
    app_icon_render(32, icon32);
    app_icon_render(64, icon64);

    return (sapp_desc){
        .init_cb     = init,
        .frame_cb    = frame,
        .cleanup_cb  = cleanup,
        .event_cb    = event,
        .width       = g_kiosk_on ? g_kiosk_w : 1280,
        .height      = g_kiosk_on ? g_kiosk_h : 720,
        /* Capture mode needs the framebuffer to BE the capture resolution. With
         * high_dpi off sokol divides the client rect by the display scale, so a
         * 1920x1080 window on a 125% desktop would present a 1536x864
         * swapchain — which is what OBS would then record. */
        .high_dpi    = g_kiosk_on ? true : false,
        .window_title = "m2-hle",
        /* Ctrl-C and Ctrl-V inside an ImGui field. sokol_imgui already points
         * Dear ImGui's clipboard callbacks at sapp_{get,set}_clipboard_string,
         * but those are inert unless this is asked for -- it defaults to off --
         * so every input box silently refused a paste. The netplay window is
         * the one that needs it: an RPCN e-mail token is 64 characters of
         * random text that arrives in a mail client, and retyping it by hand
         * is how it gets entered wrong. */
        .enable_clipboard = true,
        .logger.func = slog_func,
        .icon = {
            .images = {
                { .width = 16, .height = 16, .pixels = SAPP_RANGE(icon16) },
                { .width = 32, .height = 32, .pixels = SAPP_RANGE(icon32) },
                { .width = 64, .height = 64, .pixels = SAPP_RANGE(icon64) },
            },
        },
    };
}
