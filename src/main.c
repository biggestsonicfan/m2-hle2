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
#include <string.h>

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
#include "geo3d_window.h"
#include "sound.h"
#include "m68k_window.h"
#include "m68k_memview.h"
#include "input.h"
#include "debug_window.h"
#include "mcp_bridge.h"    /* --mcp TCP debug server (ported from m2-hle) */

/* registry.h is the single TU that defines g_profiles[] / g_profile_count /
 * g_active_profile and pulls in every per-game profile header. */
#include "registry.h"

static char g_rom_path[512] = {0};
static int  g_autorun = 0;
static int  g_browse_model = -1;   /* --model N: open single-model browser on N */
static int  g_mcp_enable = 0;      /* --mcp: start the TCP debug server */
static int  g_mcp_port   = 7172;   /* --mcp-port N */

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
    bool             show_m68k_cpu;
    bool             show_m68k_mem;
    bool             show_debug;
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

/* Pick the profile whose id matches the zip basename (e.g. "fvipers.zip" →
 * fvipers). Falls back to leaving g_active_profile unchanged if no match. */
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
    for (size_t i = 0; i < g_profile_count; i++) {
        if (strcmp(g_profiles[i]->id, id) == 0) {
            if (g_active_profile != g_profiles[i])
                LOG_INFO("auto-selected profile: %s", g_profiles[i]->display_name);
            g_active_profile = g_profiles[i];
            return;
        }
    }
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
            if (state.romset.samples && state.romset.samples_size > 0) {
                sound_load_samples(state.romset.samples, (uint32_t)state.romset.samples_size);
                scsp_hle_set_sample_rom(state.romset.samples, (uint32_t)state.romset.samples_size);
            }
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
        LOG_INFO("profile '%s' loaded; reset IP = 0x%08X",
                 g_active_profile->id, state.cpu.sfr.ip);
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

static void draw_menu_bar(void) {
    if (!igBeginMainMenuBar()) return;
    if (igBeginMenu("File")) {
        if (igMenuItem("Load ROMs...")) open_rom_dialog();
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
        if (igMenuItem("Dump 3D captures")) geo3d_log_captures(&state.geo3d);
        if (igMenuItem("Dump COP stream"))  geo3d_dump_capture_stream();
        igMenuItemBoolPtr("68K sound CPU",    NULL, &state.show_m68k_cpu,    true);
        igMenuItemBoolPtr("68K memory viewer", NULL, &state.show_m68k_mem,   true);
        igMenuItemBoolPtr("Log sound writes", NULL, &g_sound.log_writes,     true);
        { bool ws = g_warning_skip != 0;  if (igMenuItemBoolPtr("Warning-screen skip", NULL, &ws, true)) g_warning_skip = ws; }
        { bool cl = g_cam_log != 0;        if (igMenuItemBoolPtr("Log camera CSV",      NULL, &cl, true)) g_cam_log = cl; }
        igSeparator();
        igMenuItemBoolPtr("CPU opcode tests", NULL, &state.show_debug, true);
        igMenuItemBoolPtr("ImGui demo window", NULL, &state.show_demo, true);
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

static void init(void) {
    log_init();
    LOG_INFO("m2-hle starting (Phase 4: ROM load + profile resolution; %zu profile(s))",
             g_profile_count);

    mem_init(&state.bus, NULL, 0);
    i960_reset(&state.cpu);
    bp_init();
    wp_init();

    /* Default to the first registered profile so File→Load ROMs has somewhere
     * to install into without the user clicking through Profile first. */
    if (g_profile_count > 0) g_active_profile = g_profiles[0];

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
    if (g_browse_model >= 0) {            /* --model N: browse + dump its texture tiles */
        state.geo3d.use_captures = false;
        state.geo3d.model_index  = g_browse_model;
        g_dump_model_tex         = g_browse_model;
    }

    /* SCSP HLE PCM mixer + sokol_audio output (sources the 68K wave RAM + SCSP regs). */
    scsp_hle_init(g_sound.wave, M68K_WAVE_SIZE, g_sound.comm, M68K_SCSP_SIZE);

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
     * current foreground thread's to be granted the foreground. */
    {
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

static void frame(void) {
    simgui_new_frame(&(simgui_frame_desc_t){
        .width       = sapp_width(),
        .height      = sapp_height(),
        .delta_time  = sapp_frame_duration(),
        .dpi_scale   = sapp_dpi_scale(),
    });

    /* Pull a fresh double-buffered CPU snapshot under the mutex so register
     * highlights track live state without tearing. */
    if (state.emu_started) emu_update_snapshots(&state.emu);

    /* Composite the tile layers from VRAM into GPU textures for this frame. */
    video_update(&state.video, &state.bus);

    /* Optionally drive the 3D camera from the game's camera struct. */
    if (state.geo3d.use_game_view && g_active_profile &&
            g_active_profile->quirks.camera_struct_addr && state.emu_started) {
        geo3d_read_game_view(&state.geo3d, &state.bus,
                             g_active_profile->quirks.camera_struct_addr,
                             g_active_profile->quirks.camera_angle_addr);
    }

    /* Sub-frame interpolation factor: time since the last game frame ended,
     * normalised over one frame period. Resets when a new frame boundary lands. */
    static int     s_last_geo_frame_end = 0;
    static int64_t s_frame_end_us       = 0;
    float lerp_t = 1.0f;
    if (state.emu_started) {
        int cur_frame_end = g_cop.geo_frame_end;
        int64_t now_us    = emu_now_us();
        if (cur_frame_end != s_last_geo_frame_end) {
            s_last_geo_frame_end = cur_frame_end;
            s_frame_end_us       = now_us;
            lerp_t = 1.0f;
        } else if (s_frame_end_us > 0) {
            lerp_t = (float)(now_us - s_frame_end_us) / (float)EMU_SLICE_US;
            if (lerp_t < 0.0f) lerp_t = 0.0f;
            if (lerp_t > 1.0f) lerp_t = 1.0f;
        }
    }

    /* Scan this frame's slice of the COP geo-capture ring into the model list. */
    if (state.geo3d.enabled && g_active_profile &&
            state.romset.main_data && state.romset.polygons) {
        const game_quirks_t *q = &g_active_profile->quirks;
        if (q->geo_displaylist) {
            /* Authentic hardware path: decode the GEO display list the i960 built
             * in bufferram. Scan the snapshot captured at geo_flush (g_geodl_snap),
             * not live bufferram — the SHARC HLE aliases bufferram and clobbers the
             * list between flushes. Every m2-snake homebrew uses read_start 0x10000. */
            if (g_geodl_snap_ready)
                geo3d_scan_displaylist(&state.geo3d,
                                       g_geodl_snap, BUFF_RAM_SIZE / 4, 0x10000,
                                       state.romset.main_data, state.romset.main_data_size,
                                       q->model_table_offset, q->model_table_count,
                                       state.bus.palette, PALETTE_SIZE);
        } else {
            geo3d_scan_captures(&state.geo3d,
                                state.romset.main_data, state.romset.main_data_size,
                                state.romset.polygons_size,
                                q->model_table_offset, q->model_table_count,
                                q->mesh_ptr_subtract, q->mesh_ptr_add);
        }
    } else {
        geo3d_lines_reset();
    }

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
    if (state.show_bus_stats)   draw_bus_stats_window();
    if (state.show_m68k_cpu) {
        m68k_window_draw(&g_sound.m68k, &state.m68k_snapshot, &state.show_m68k_cpu);
        state.m68k_snapshot = g_sound.m68k;
    }
    if (state.show_m68k_mem)    m68k_memview_draw(&state.show_m68k_mem);
    if (state.show_debug)       debug_window_draw(&state.show_debug);
    if (state.show_demo)        igShowDemoWindow(&state.show_demo);

    sg_begin_pass(&(sg_pass){
        .action    = state.pass_action,
        .swapchain = sglue_swapchain(),
    });

    /* Draw the game letterboxed into the swapchain, then ImGui on top.
     * Layer order: back-back colour → background tiles → (3D, Phase 9) → FG/HUD. */
    {
        int ox, oy, w, h;
        /* Reserve the top main-menu-bar strip so the game (and its row-0 HUD) isn't
         * occluded by the opaque ImGui bar drawn on top. */
        int menu_h = (int)(igGetFrameHeight() * sapp_dpi_scale());
        int avail_h = sapp_height() - menu_h;
        if (avail_h < 1) avail_h = 1;
        game_render_letterbox(sapp_width(), avail_h,
                              VIDEO_WIDTH, VIDEO_HEIGHT, &ox, &oy, &w, &h);
        oy += menu_h;
        /* Decode both 4-bit luma texture banks → GPU atlas for textured fills. */
        game_render_upload_atlas(state.bus.texram0, state.bus.texram1, TEXRAM0_SIZE);
        game_render_upload_luts(state.bus.luma, state.bus.colorxlat);
        { static int _cs=0; if ((++_cs % 30)==0) {
            for (int i=0;i<state.geo3d.captured_count;i++){ const captured_model_t *cm=&state.geo3d.captured[i];
                if (cm->model_idx==519 || cm->model_idx==2833)
                    LOG_INFO("CAGE m=%d bone=%d clip=%d mat=%d T=(%.2f,%.2f,%.2f) R0=(%.2f,%.2f,%.2f) R1=(%.2f,%.2f,%.2f) R2=(%.2f,%.2f,%.2f)",
                        cm->model_idx, cm->from_bone, cm->has_clip_win, cm->has_matrix,
                        cm->matrix[3],cm->matrix[7],cm->matrix[11],
                        cm->matrix[0],cm->matrix[1],cm->matrix[2],
                        cm->matrix[4],cm->matrix[5],cm->matrix[6],
                        cm->matrix[8],cm->matrix[9],cm->matrix[10]); } } }
        /* Layer order: back colour → background tiles → 3D scene → foreground/HUD. */
        game_render_draw_game(state.video.back_view, ox, oy, w, h);
        game_render_draw_game(state.video.bg_view,   ox, oy, w, h);
        if (state.geo3d.enabled && g_active_profile &&
                state.romset.main_data && state.romset.polygons) {
            const game_quirks_t *q = &g_active_profile->quirks;
            /* The geo_displaylist path emits geometry already in camera space (the
             * homebrew's own view() did the camera transform; the GEO projects with
             * focal). Render it as wireframe with the geo3d camera (live-tunable via
             * the set_camera bridge cmd; defaults set on profile load). */
            /* Homebrew renders SOLID: the tube walls are filled quads (geo_obj_quad)
             * and even its "lines" are thin filled quads — so draw fills, not pure
             * wireframe. Keep the bright wireframe overlay on for the rim/divider
             * cores, and force flat per-object colour (model 456's ROM material is a
             * meaningless placeholder). The homebrew does its own software backface
             * cull, so render double-sided (cull none). */
            state.geo3d.lines_only = false;
            if (q->geo_displaylist) {
                g_geo_wireframe  = 0;   /* dividers/rims are filled thin-quads, not wireframe */
                g_geo_flat_color = 1;
                g_backface_cull  = 0;
            }
            /* The homebrew's display-list object order changes every frame (stars
             * move, enemies spawn/die), so captured_prev[i] is a DIFFERENT object —
             * sub-frame interpolation would smear the geometry. Disable it (lerp=1). */
            float dl_lerp = q->geo_displaylist ? 1.0f : lerp_t;
            game_render_draw_captured_models(&state.geo3d,
                                             state.romset.main_data, state.romset.main_data_size,
                                             state.romset.polygons,  state.romset.polygons_size,
                                             state.romset.textures,  state.romset.textures_size,
                                             q->model_table_offset, q->model_table_count,
                                             q->mesh_ptr_subtract, q->mesh_ptr_add,
                                             ox, oy, w, h,
                                             state.geo3d.cam_x, state.geo3d.cam_y, state.geo3d.cam_z,
                                             state.geo3d.rot_y, state.geo3d.rot_x, state.geo3d.fov_deg,
                                             dl_lerp);
        }
        game_render_draw_game(state.video.fg_view,   ox, oy, w, h);

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
    sg_commit();
}

static void cleanup(void) {
    if (state.emu_started) emu_thread_shutdown(&state.emu);
    scsp_hle_shutdown();   /* stop audio after the emu thread (no more ring writes) */
    if (state.file_dialog) { IGFD_Destroy(state.file_dialog); state.file_dialog = NULL; }
    romset_free(&state.romset);
    game_render_shutdown();
    video_shutdown(&state.video);
    simgui_shutdown();
    sg_shutdown();
    mem_shutdown(&state.bus);
    log_file_close();
}

static void event(const sapp_event* ev) {
    simgui_handle_event(ev);

    /* Game input flows through the emulated I/O ports (read by the game's vblank
     * interrupt) — set/clear the host held mask here. Routed regardless of ImGui
     * focus so arcade muscle-memory isn't eaten by a debug window. */
    if (ev->type == SAPP_EVENTTYPE_KEY_DOWN && !ev->key_repeat) input_key_down((int)ev->key_code);
    if (ev->type == SAPP_EVENTTYPE_KEY_UP)                      input_key_up((int)ev->key_code);

    if (ev->type != SAPP_EVENTTYPE_KEY_DOWN) return;
    switch (ev->key_code) {
        case SAPP_KEYCODE_ESCAPE: sapp_request_quit(); break;
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
        } else if (strcmp(argv[i], "--run") == 0) {
            g_autorun = 1;
        } else if (strcmp(argv[i], "--camlog") == 0) {
            g_cam_log = 1;            /* dump cam_ours.csv per game frame */
        } else if (strcmp(argv[i], "--nowarnskip") == 0) {
            g_warning_skip = 0;       /* keep warning screen → frame-align with MAME */
        } else if (strcmp(argv[i], "--realirq") == 0) {
            g_real_irq = 1;           /* tick board timers → real timer ISR delivery */
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
        }
    }
    return (sapp_desc){
        .init_cb     = init,
        .frame_cb    = frame,
        .cleanup_cb  = cleanup,
        .event_cb    = event,
        .width       = 1280,
        .height      = 720,
        .window_title = "m2-hle",
        .logger.func = slog_func,
        .icon.sokol_default = true,
    };
}
