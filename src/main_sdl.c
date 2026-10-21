/*
 * m2-hle — fullscreen handheld frontend (SDL3 + GLES 3, no ImGui).
 *
 * Runs one ROM set straight from the command line, the way an EmulationStation
 * launcher calls it: SDL owns the window, the GLES context, the gamepad and the
 * frame timing; sokol_gfx draws through the shared game_frame.h path, so the
 * picture is the debugger's (main.c) without its windows.
 *
 * The emu thread runs the game at its own 60 Hz pace. The host renders at
 * --render-fps (default 30): every rendered frame shows the newest game frame,
 * so a lower render rate skips frames rather than slowing the game.
 *
 *   m2hle [--rom] <set.zip> [--render-fps N] [--window WxH] [--stats] [--osd]
 *         [--log FILE] [--log-level SPEC] [--pad-map LIST] [--shot FRAME:FILE]... [--exit-after N]
 *
 * --pad-map  comma-separated button=action pairs overriding the defaults, e.g.
 *            "south=b1,east=b2,west=b3". Buttons: south east west north start
 *            back l1 r1 l3 r3 guide; actions: b1 b2 b3 b4 start coin service
 *            test none, a racing cabinet's gearn gear1 gear2 gear3 gear4 (one
 *            button a gate, Daytona and Sega Rally), or a combo that holds
 *            several at once (a "macro"), e.g. "north=b1+b2,l3=b1+b2+b3".
 * --shot     save a PNG of the first rendered frame at or after game frame N.
 * --exit-after  quit after N game frames (for scripted checks).
 * --host-prof START:SECS[:FILE]  sample the process's own CPU time for SECS
 *            seconds, START seconds after launch, and write a report to FILE
 *            (default /tmp/m2hle-hostprof-PID.txt). Linux only; the same as
 *            M2HLE_HOSTPROF="start=START secs=SECS out=FILE" (core/host_prof.h).
 * --stats    print frame rates, per-stage host time and temperatures every 5 s.
 * --osd      show a status line in the top-right corner, refreshed every second:
 *            drawn/game frames per second, the hotter of the CPU and GPU
 *            temperature, the local time and the battery level ("+" while
 *            charging). Parts the host does not report are left out.
 * --gl-finish   wait for the GPU after each frame, so --stats can tell CPU
 *            submission time from GPU time (costs throughput; diagnosis only).
 * --max-temp C  quit once either thermal zone reaches C degrees.
 * --render-scale N  draw the game offscreen at N x the board's 496x384 and
 *            scale that to the screen (default 1); 0 draws at screen size.
 *            The fill shader's texture LOD is calibrated to 496x384, and the
 *            GPU shades 36% fewer pixels than at 620x480.
 * --display-scale N  show the game at N x 496x384, centred, instead of as large
 *            as the screen allows (0, the default). 1 is pixel for pixel: on a
 *            640x480 screen, a 496x384 picture with a border. A size that does
 *            not fit the screen falls back to 0.
 * --filter   nearest|linear scaling of the offscreen frame (default linear).
 * --cpu-tiles   compose the tile layers on the CPU instead of in a shader.
 * --no-mesh-cache  decode every display-list model in full every frame.
 * --steps-per-slice N  i960 steps a slice may run short of the vblank before
 *            the run loop lets the UI in (default 500000).
 * --live-timers  accepted and ignored: the board timers always count the
 *            i960's cycles now (irq_timer.h).
 * --fill-shade-rows  give a textured face one row of finished colours (its luma
 *            band, poly_luma and colour together), so a pixel fetches its colour
 *            once instead of a lumaram texel and then a ramp texel.
 * --verify-fill  after every game pass, draw its 3D fills again through the
 *            reference fill shader and the one in use, into two scratch targets,
 *            and compare the bytes (needs --render-scale 1 or more).
 * --verify-gpu-tiles  on every frame the GPU composes the tile layers, pause
 *            the emu thread, compose the same RAM on the CPU as well and
 *            compare the two byte for byte (FG colour only where it shows).
 * --sound    run the sound board (68000 + SCSP, in lockstep with the emu thread)
 *            and play it through SDL. Without it the board is not attached at
 *            all: silent, and the emu thread does no sound work, which is cooler
 *            and cheaper on a handheld. No audio device: a warning, and the game
 *            runs as without --sound.
 * --netplay  sign in to RPCN with the stored netplay settings and open the
 *            lobby: host a room, join one, start a match. The settings are the
 *            desktop's file (netplay.h, "Stored settings"), per user:
 *            $XDG_CONFIG_HOME or ~/.config, then m2hle2/netplay.cfg. Copying the
 *            PC's file there brings its account, and its Twitch login, along.
 *            L1+R1 (or Guide, or F1) opens and closes the lobby.
 * --net-config FILE  read and write the netplay settings here instead.
 * --net-delay N  frames of input delay when hosting (default: the stored value, else 2).
 * --net-host  host a room as soon as the sign-in completes (implies --netplay).
 * --net-room-pass P  lock the rooms hosted here with P (8 characters: netplay.h).
 * --nvram-dir DIR  keep backup RAM (the test menu's settings, bookkeeping,
 *            rankings) in DIR/<set>/backup1 instead of the per-user
 *            ~/.config/m2hle2/nvram (core/backup_ram.h).
 * --no-nvram  boot with blank backup RAM every time and keep nothing.
 * --versus   Versus on this machine (Virtual-On): start a second m2hle beside
 *            this one, each a cabinet with its own window and pad, their comm
 *            boards linked over 127.0.0.1 (net/comm_local.h). This window is
 *            the MASTER and plays the first pad; the other is the SLAVE and
 *            plays the second. Closing either ends the Versus. Windowed
 *            (640x480 unless --window). Not with --netplay.
 * --pad N    only the Nth gamepad (0 the first) plays here; default all.
 * --wheel-axes S,A,B  a racing wheel SDL sees only as a joystick: its wheel,
 *            accelerator and brake axes (default 0,1,2; Sega Rally, Daytona).
 *            Given, any joystick that is not a gamepad counts as a wheel.
 *            A gamepad steers with its left stick and pedals with its
 *            triggers (R2, L2); the wheel's force goes to whichever has it
 *            (--ffb-gain N, 0-100).
 * --fanatec gear|speed|off  a Fanatec base's rev LEDs and display show a
 *            racing game's rpm and its gear (default) or speed (fanatec.h).
 * --g25-range N  a Logitech G25, G27 or DFGT turns N degrees (default 270;
 *            0 leaves it). --g25 off leaves those wheels alone: no mode
 *            switch, no range, no force of ours (lg4ff.h, LOGITECH.md).
 * --wheel-buttons N=action,...  that wheel's buttons, e.g. "4=up,5=down" (the
 *            gear lever), actions as --pad-map's.
 * --window-at X,Y  put the window there.
 * --versus-slave PORT:TOKEN  the SLAVE that --versus starts (not for players).
 * --scripts DIR  the game scripts' folder (M2emulator's Lua, core/m2lua.h):
 *            <set>.lua there runs with the game. Default scripts/ in the
 *            working directory, else beside the executable.
 * --no-scripts  run no game script.
 * --script-option CODE  set the script's options, in the libretro core's
 *            cheat codes: <key>=<value> (the value's index or name), several
 *            joined by '+'. May be given more than once.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>    /* stat: is scripts/ a folder */
#include <unistd.h>      /* readlink: scripts/ beside the executable */

#include <SDL3/SDL.h>
#include <GLES3/gl3.h>

#include "sokol_gfx.h"
#include "sokol_log.h"
#include "sokol_debugtext.h"
#include "sokol_gl.h"        /* a script's PostDraw images (scripts_draw) */

#include "constants.h"
#include "log.h"
#include "crash_trace.h"  /* a backtrace in the log on SIGSEGV (glibc) */
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
#include "input.h"
#include "audio_out.h"   /* audio_out_cb: drains g_sound's ring (only its callback is used here) */
#include "miniz.h"
#include "net/comm_local.h"   /* --versus: the two cabinets' link over 127.0.0.1 */
#include "child_proc.h"   /* --versus: the SLAVE's process */
#include "m2lua.h"       /* M2emulator's game scripts (Pinboard #972) */
#include "m2lua_wide.h"  /* and the picture they ask for */
/* Video_CreateSurfaceFromFile's PNGs (M2emulator loads only PNG). Static, from
 * memory: m2lua.h reads the file. */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma GCC diagnostic pop

/* registry.h is the single TU that defines g_profiles[] / g_profile_count /
 * g_active_profile and pulls in every per-game profile header. */
#include "registry.h"
#include "lightgun_sgl.h"   /* the mouse and touches as light guns (Pinboard #1040) */
#include "sdl_pads.h"      /* gamepads, wheels, force feedback (Pinboard #1047, #1082) */
#include "fanatec.h"       /* a Fanatec base's rev LEDs and display (Pinboard #1361) */
#include "lg4ff.h"         /* a Logitech G25 without Logitech's software (Pinboard #1370) */

#define MAX_SHOTS 16
#define MAX_SCRIPT_OPTS 16

static struct {
    memory_bus_t     bus;
    i960_cpu_t       cpu;
    emu_thread_ctx_t emu;
    romset_t         romset;
    video_state_t    video;
    geo3d_state_t    geo3d;
} state;

static struct {
    const char *rom;
    const char *profile;            /* --profile <id>; NULL = the ROM set's default */
    double      render_fps;
    int         win_w, win_h;       /* 0 = fullscreen */
    bool        stats;
    bool        osd;
    bool        sound;
    const char *log_path;
    const char *log_levels;
    const char *pad_map;
    int         shot_count;
    unsigned    shot_frame[MAX_SHOTS];
    const char *shot_path[MAX_SHOTS];
    unsigned    exit_after;
    bool        gl_finish;
    double      max_temp;
    int         render_scale;       /* offscreen at N x 496x384; 0 = straight to the screen */
    int         display_scale;      /* shown at N x 496x384, centred; 0 = fit the screen */
    bool        linear;
    bool        cpu_tiles;
    bool        verify_gpu_tiles;
    bool        verify_fill;
    bool        netplay;
    bool        net_host;           /* host a room as soon as the sign-in completes */
    const char *net_room_pass;      /* the rooms hosted here are locked with it */
    int         net_delay;          /* 0 = the stored value */
    bool        versus;             /* --versus: the MASTER, starting the SLAVE */
    uint16_t    versus_port;        /* --versus-slave: the MASTER's port; 0 = not a SLAVE */
    uint32_t    versus_token;
    int         pad;                /* --pad: the only gamepad that plays here; -1 = all */
    bool        win_at;
    int         win_x, win_y;
    const char *scripts_dir;        /* --scripts; NULL = the default */
    bool        no_scripts;
    int         script_opt_count;
    const char *script_opt[MAX_SCRIPT_OPTS];   /* --script-option codes */
} opt = { .render_fps = 30.0, .render_scale = 1, .linear = true, .pad = -1 };

static bool g_sdtx_on;              /* sokol's debug text is set up: --osd, the lobby or a script's text */
static bool g_sound_on;             /* the sound board is attached (sound_start succeeded) */

/* Keyboard, for desktop runs: the debugger's layout (input.h), which is
 * m2emulator's (Pinboard #1347), and its INPUT_KEYS_DRIVING on a racing
 * cabinet. F1 is the lobby here, so the service button is MAME's 9 alone. */
static int key_to_action(SDL_Scancode sc) {
    if (input_keys_driving(g_active_profile)) {
        switch (sc) {
            case SDL_SCANCODE_UP:   return GAME_INPUT_P1_B1;    /* accelerator */
            case SDL_SCANCODE_DOWN: return GAME_INPUT_P1_B2;    /* brake */
            case SDL_SCANCODE_Z:    return GAME_INPUT_P1_B3;    /* VR1 .. VR4 */
            case SDL_SCANCODE_X:    return GAME_INPUT_P1_B4;
            case SDL_SCANCODE_C:    return GAME_INPUT_P2_B3;
            case SDL_SCANCODE_V:    return GAME_INPUT_P2_B4;
            case SDL_SCANCODE_W:    return GAME_INPUT_P1_UP;    /* shift up */
            case SDL_SCANCODE_Q:    return GAME_INPUT_P1_DOWN;  /* shift down */
            case SDL_SCANCODE_A:    return GAME_INPUT_GEAR_1;   /* a gear's button */
            case SDL_SCANCODE_S:    return GAME_INPUT_GEAR_2;
            case SDL_SCANCODE_D:    return GAME_INPUT_GEAR_3;
            case SDL_SCANCODE_F:    return GAME_INPUT_GEAR_4;
            case SDL_SCANCODE_G:    return GAME_INPUT_GEAR_N;
            default:                break;
        }
    }
    switch (sc) {
        case SDL_SCANCODE_UP:    return GAME_INPUT_P1_UP;
        case SDL_SCANCODE_DOWN:  return GAME_INPUT_P1_DOWN;
        case SDL_SCANCODE_LEFT:  return GAME_INPUT_P1_LEFT;
        case SDL_SCANCODE_RIGHT: return GAME_INPUT_P1_RIGHT;
        case SDL_SCANCODE_Z:     return GAME_INPUT_P1_B1;
        case SDL_SCANCODE_X:     return GAME_INPUT_P1_B2;
        case SDL_SCANCODE_C:     return GAME_INPUT_P1_B3;
        case SDL_SCANCODE_V:     return GAME_INPUT_P1_B4;
        case SDL_SCANCODE_1:     return GAME_INPUT_P1_START;
        case SDL_SCANCODE_5:     return GAME_INPUT_P1_COIN;
        case SDL_SCANCODE_9:     return GAME_INPUT_SERVICE;
        case SDL_SCANCODE_F2:    return GAME_INPUT_TEST;
        default:                 return -1;
    }
}

/* ---- Sound ----------------------------------------------------------------- */

/* SDL asks for more audio: hand it the board's output through audio_out_cb, the
 * debugger's drain (ring-fill-nudged resampling, DC blocker). The stream is
 * opened at the board's own 44.1 kHz, so SDL converts to the device's rate and
 * audio_out_cb only corrects the two clocks' drift. */
static void sdl_audio_cb(void *ud, SDL_AudioStream *stream, int additional, int total) {
    (void)ud; (void)total;
    float buf[1024 * 2];
    for (int frames = additional / (int)(2 * sizeof(float)); frames > 0; ) {
        int n = frames < 1024 ? frames : 1024;
        audio_out_cb(buf, n, 2, NULL);
        SDL_PutAudioStreamData(stream, buf, n * 2 * (int)sizeof(float));
        frames -= n;
    }
}

/* Open the audio device, then attach the sound board and load its program and
 * sample ROMs (main.c load_active_profile's sound block). Call after load_rom,
 * before the emu thread starts. On any failure the board stays detached and the
 * game runs silent. Returns the stream, or NULL. */
static SDL_AudioStream *sound_start(void) {
    if (!g_active_profile->quirks.enable_68k_sound || !state.romset.audiocpu || state.romset.audiocpu_size == 0) {
        fprintf(stderr, "m2hle: %s has no sound board ROMs; running silent\n", g_active_profile->display_name);
        return NULL;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "m2hle: no audio (%s); running silent\n", SDL_GetError());
        return NULL;
    }
    SDL_AudioSpec spec = { .format = SDL_AUDIO_F32, .channels = 2, .freq = (int)SOUND_RATE };
    SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, sdl_audio_cb, NULL);
    if (!stream) {
        fprintf(stderr, "m2hle: could not open audio (%s); running silent\n", SDL_GetError());
        return NULL;
    }
    sound_select_board(g_active_profile->quirks.model1_sound);
    sound_reset();
    sound_attach(&state.bus);
    sound_load_rom(state.romset.audiocpu, (uint32_t)state.romset.audiocpu_size);
    if (state.romset.samples && state.romset.samples_size > 0)
        sound_load_samples(state.romset.samples, (uint32_t)state.romset.samples_size);
    /* The stream is opened at the board's rate, so the drain works in 44.1 kHz
     * frames and SDL does the conversion. Half the desktop's queue: 93 ms is
     * what this frontend has always held, and a handheld would rather have the
     * latency than the cushion. */
    audio_out_configure(&(audio_out_config_t){ .target = 4096.0 }, SOUND_RATE);
    g_audio_out.ready = true;
    SDL_ResumeAudioStreamDevice(stream);
    printf("m2hle: sound board on\n");
    g_sound_on = true;
    return stream;
}

/* ---- ROM loading (main.c load_active_profile; the sound board: sound_start) */

static bool load_rom(const char *zip) {
    char id[64];
    profile_set_from_path(zip, id, sizeof id);
    const game_profile_t *want = opt.profile ? profile_by_id(opt.profile) : NULL;
    if (opt.profile && !want) fprintf(stderr, "m2hle: --profile %s: no such profile; using the default\n", opt.profile);
    g_active_profile = profile_for_rom_set(id, want);
    if (!g_active_profile) {
        char msg[256];
        profile_missing_message(id, msg, sizeof msg);
        fprintf(stderr, "m2hle: %s\n", msg);
        return false;
    }

    /* MAME clone fall-through: the parent zip sits next to the picked one. A
     * merged set carries the parent's files itself, so a missing parent is fine. */
    char parent[1024];
    const char *parent_ptr = profile_parent_path(g_active_profile, zip, parent, sizeof parent);
    sound_settle();   /* load_fn frees the sample ROMs the sound thread reads */
    if (romset_load(&state.romset, g_active_profile->load_fn, zip, parent_ptr) != 0) {
        fprintf(stderr, "m2hle: could not load '%s' as %s\n", zip, g_active_profile->display_name);
        return false;
    }
    bool homebrew = profile_adopt_program(state.romset.maincpu, state.romset.maincpu_size);
    if (homebrew)
        LOG_INFO("the program ROM is not the set's game: running it as %s", g_active_profile->display_name);
    /* The set's backup RAM, before install_fn boots the board with it. */
    if (g_backup_want != 0) {
        char key[96];
        backup_ram_key(key, sizeof key, profile_rom_set(g_active_profile), homebrew,
                       state.romset.maincpu, state.romset.maincpu_size);
        backup_ram_open(key, true);
    }
    /* The model lookup is built from the ROM's model table: a new set needs a new one. */
    geo3d_lookup_invalidate();
    g_active_profile->install_fn(&state.romset, &state.cpu, &state.bus);
    input_reset();
    input_attach(&state.bus);
    return true;
}

/* ---- Game scripts (core/m2lua.h, Pinboard #972) ------------------------------
 *
 * The desktop's and the libretro core's: <dir>/<set>.lua runs with the game,
 * through the emu thread's hooks, and never while a netplay session has the
 * board (emu_script_may_run). Its options are set from the command line in the
 * libretro core's cheat codes; PostDraw's text is drawn with sokol's 8x8 font
 * and its PNGs with sokol_gl,
 * and Model2_SetWideScreen and the stretches shape the picture (ui/m2lua_wide.h). */

static bool scripts_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* scripts/ in the working directory, else beside the executable (main.c's). */
static void scripts_default_dir(char *out, size_t cap) {
    snprintf(out, cap, "scripts");
    if (scripts_is_dir(out)) return;
    char exe[512] = "";
    long n = (long)readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return;
    exe[n] = 0;
    char *slash = strrchr(exe, '/');
    if (slash) snprintf(out, cap, "%.*s/scripts", (int)(slash - exe), exe);
}

/* --script-option codes onto the options; before the emu thread runs. */
static void scripts_apply_options(void) {
    static int want[M2LUA_OPTION_MAX];
    m2lua_cheat_defaults(want);
    for (int i = 0; i < opt.script_opt_count; i++)
        if (!m2lua_cheat_code(opt.script_opt[i], want))
            fprintf(stderr, "m2hle: --script-option %s: no such option or value\n", opt.script_opt[i]);
    m2lua_cheat_apply(want);
}

static uint8_t *scripts_decode(const uint8_t *buf, int len, int *w, int *h) {
    int comp;
    return stbi_load_from_memory(buf, len, w, h, &comp, 4);
}

/* After load_rom, before the emu thread: the game's script, Init and all. */
static void scripts_load(void) {
    char dir[512];
    if (opt.scripts_dir) snprintf(dir, sizeof dir, "%s", opt.scripts_dir);
    else                 scripts_default_dir(dir, sizeof dir);
    m2lua_set_dir(dir);
    g_m2lua_decode_image = scripts_decode;   /* Init may load its images */
    g_m2lua.disabled   = opt.no_scripts;
    g_emu_script_frame = m2lua_frame_hook;
    g_emu_script_reset = m2lua_reset_hook;
    if (!m2lua_load(profile_rom_set(g_active_profile), &state.bus, &state.romset)) {
        if (g_m2lua.error[0]) fprintf(stderr, "m2hle: script not run: %s\n", g_m2lua.error);
        return;
    }
    scripts_apply_options();
    printf("m2hle: script %s (%d option%s)\n", g_m2lua.path, g_m2lua.noptions,
           g_m2lua.noptions == 1 ? "" : "s");
}

/* PostDraw's images: one texture per loaded surface, made again when the
 * script loads another into its slot (gen), let go when it releases it. Drawn
 * as sokol_gl quads with the image's alpha, under the text. */
static struct {
    bool         ready;
    sgl_pipeline pip;
    sg_sampler   smp;
    sg_image     img[M2LUA_SURFACE_MAX];
    sg_view      view[M2LUA_SURFACE_MAX];
    uint32_t     gen[M2LUA_SURFACE_MAX];
} g_script_img;

static void scripts_images_setup(void) {
    sgl_setup(&(sgl_desc_t){ .logger.func = slog_func });
    sg_pipeline_desc pd = { 0 };
    pd.colors[0].blend = (sg_blend_state){
        .enabled = true,
        .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA, .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        .src_factor_alpha = SG_BLENDFACTOR_ZERO,    .dst_factor_alpha = SG_BLENDFACTOR_ONE };
    g_script_img.pip = sgl_make_pipeline(&pd);
    g_script_img.smp = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR, .mag_filter = SG_FILTER_LINEAR,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE, .wrap_v = SG_WRAP_CLAMP_TO_EDGE });
    g_script_img.ready = true;
}

static void scripts_image_drop(int i) {
    if (g_script_img.view[i].id) sg_destroy_view(g_script_img.view[i]);
    if (g_script_img.img[i].id)  sg_destroy_image(g_script_img.img[i]);
    g_script_img.view[i].id = g_script_img.img[i].id = 0;
    g_script_img.gen[i] = 0;
}

/* Upload what the script loaded since the last frame, free what it released. */
static void scripts_images_sync(void) {
    m2lua_init_once();
    emu_mutex_lock(&g_m2lua.draw_mutex);
    for (int i = 0; i < M2LUA_SURFACE_MAX; i++) {
        m2lua_surface_t *s = &g_m2lua.surfaces[i];
        if (!s->used) {
            free(s->rgba);
            s->rgba = NULL;
            if (g_script_img.img[i].id) scripts_image_drop(i);
            continue;
        }
        if (g_script_img.gen[i] == s->gen) continue;
        scripts_image_drop(i);
        g_script_img.img[i] = sg_make_image(&(sg_image_desc){
            .width = s->w, .height = s->h, .pixel_format = SG_PIXELFORMAT_RGBA8,
            .data.mip_levels[0] = { s->rgba, (size_t)s->w * (size_t)s->h * 4u },
            .label = "script-surface" });
        g_script_img.view[i] = sg_make_view(&(sg_view_desc){ .texture.image = g_script_img.img[i] });
        g_script_img.gen[i] = s->gen;
    }
    emu_mutex_unlock(&g_m2lua.draw_mutex);
}

static void scripts_images_shutdown(void) {
    if (!g_script_img.ready) return;
    for (int i = 0; i < M2LUA_SURFACE_MAX; i++) scripts_image_drop(i);
    sgl_shutdown();
}

/* One image at board (x, y), its own size times k (as M2emulator, which drew
 * it 1:1 on a 496 x 384 screen), recorded for sgl_draw. */
static void scripts_image(const m2lua_draw_t *d, float x, float y, float k) {
    int i = d->surface;
    if (i < 0 || i >= M2LUA_SURFACE_MAX || !g_script_img.view[i].id) return;
    float x1 = x + (float)g_m2lua.surfaces[i].w * k, y1 = y + (float)g_m2lua.surfaces[i].h * k;
    sgl_texture(g_script_img.view[i], g_script_img.smp);
    sgl_begin_quads();
    sgl_v2f_t2f(x,  y,  0.0f, 0.0f);
    sgl_v2f_t2f(x1, y,  1.0f, 0.0f);
    sgl_v2f_t2f(x1, y1, 1.0f, 1.0f);
    sgl_v2f_t2f(x,  y1, 0.0f, 1.0f);
    sgl_end();
}

/* One string at window (x, y), in its font's size (sdtx's cell is 8 pixels). */
static void scripts_text(const m2lua_draw_t *d, int fb_w, int fb_h, float x, float y, float k) {
    bool font = d->font >= 0 && g_m2lua.fonts[d->font].used;
    float cell = (font ? g_m2lua.fonts[d->font].size : M2LUA_TEXT_SIZE) * k;
    if (cell < 4.0f) cell = 4.0f;
    float s = cell / 8.0f;
    sdtx_canvas((float)fb_w / s, (float)fb_h / s);
    sdtx_origin(0.0f, 0.0f);
    sdtx_pos(x / cell, y / cell);
    sdtx_color4b((uint8_t)(d->color >> 16), (uint8_t)(d->color >> 8), (uint8_t)d->color,
                 (uint8_t)(d->color >> 24));
    sdtx_puts(d->text);
}

/* PostDraw's list over the picture at (ox, oy, w, h): board pixels scaled with
 * it. Images first (sgl_draw here), then the text, before the frame's one
 * sdtx_draw. */
static void scripts_draw(int fb_w, int fb_h, int ox, int oy, int w, int h) {
    static m2lua_draw_t list[M2LUA_DRAW_MAX];
    int n = 0;
    if (!g_m2lua.loaded) return;
    if (!g_script_img.ready) scripts_images_setup();
    scripts_images_sync();
    m2lua_take_draw(list, &n);
    if (n == 0) return;
    float k = (float)h / (float)VIDEO_HEIGHT, kx = (float)w / (float)VIDEO_WIDTH;
    sg_apply_viewport(0, 0, fb_w, fb_h, true);
    sg_apply_scissor_rect(0, 0, fb_w, fb_h, true);
    sgl_defaults();
    sgl_enable_texture();
    sgl_load_pipeline(g_script_img.pip);
    sgl_matrix_mode_projection();
    sgl_ortho(0.0f, (float)fb_w, (float)fb_h, 0.0f, -1.0f, 1.0f);
    for (int i = 0; i < n; i++) {
        const m2lua_draw_t *d = &list[i];
        float x = (float)ox + d->x * kx, y = (float)oy + d->y * k;
        if (d->kind == M2LUA_DRAW_SURFACE) scripts_image(d, x, y, k);
        else                               scripts_text(d, fb_w, fb_h, x, y, k);
    }
    sgl_draw();
}

/* The last lines of the in-memory log, for a failure with file logging off. */
static void dump_log_tail(int lines) {
    int count = g_log.count < LOG_MAX_LINES ? g_log.count : LOG_MAX_LINES;
    for (int k = count - lines < 0 ? 0 : count - lines; k < count; k++) {
        int idx = (g_log.count - count + k) % LOG_MAX_LINES;
        fprintf(stderr, "  %s\n", g_log.lines[idx]);
    }
}

/* ---- Screenshots ---------------------------------------------------------- */

static void save_png(const char *path, int w, int h) {
    uint8_t *px = malloc((size_t)w * (size_t)h * 4);
    if (!px) return;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (size_t i = 3; i < (size_t)w * (size_t)h * 4; i += 4) px[i] = 255;
    size_t len = 0;
    void *png = tdefl_write_image_to_png_file_in_memory_ex(px, w, h, 4, &len, 6, MZ_TRUE);
    FILE *f = png ? fopen(path, "wb") : NULL;
    if (f) {
        fwrite(png, 1, len, f);
        fclose(f);
        printf("m2hle: saved %s (%dx%d, game frame %u)\n", path, w, h, g_emu_frames);
    } else {
        fprintf(stderr, "m2hle: could not write %s\n", path);
    }
    mz_free(png);
    free(px);
}

/* ---- --verify-gpu-tiles ----------------------------------------------------- */

static struct {
    uint64_t frames, bad_frames;
    uint8_t  bg[VIDEO_WIDTH * VIDEO_HEIGHT * 4], fg[VIDEO_WIDTH * VIDEO_HEIGHT * 4];
    uint8_t  shown[VIDEO_WIDTH * VIDEO_HEIGHT * 4];
    sg_image rt;
    sg_view  rt_att;
} g_vt;

/* Read a layer target back. GL stores its bottom row first, and the compositor
 * writes screen row y to target row y, so the rows line up with the CPU's. */
static void read_layer(sg_image img, uint8_t *out) {
    sg_gl_image_info gi = sg_gl_query_image_info(img);
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gi.tex[gi.active_slot], 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, VIDEO_WIDTH, VIDEO_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, out);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    sg_reset_state_cache();
}

/* Draw a pen layer the way the frame shows it (game_render_draw_indexed) into a
 * scratch 496x384 target over black, and read it back in screen row order: the
 * quad puts the layer's top row at the top of the viewport, which is the
 * target's last row. */
static void show_layer(sg_view pens, uint8_t *out) {
    if (g_vt.rt.id == 0) {
        g_vt.rt = sg_make_image(&(sg_image_desc){ .width = VIDEO_WIDTH, .height = VIDEO_HEIGHT,
            .pixel_format = SG_PIXELFORMAT_RGBA8, .usage = { .color_attachment = true }, .label = "verify-shown" });
        g_vt.rt_att = sg_make_view(&(sg_view_desc){ .color_attachment.image = g_vt.rt });
    }
    sg_begin_pass(&(sg_pass){
        .action.colors[0] = { .load_action = SG_LOADACTION_CLEAR, .clear_value = { 0, 0, 0, 1 } },
        .attachments.colors[0] = g_vt.rt_att,
    });
    game_render_draw_indexed(pens, state.video.pal_rgba_view, false, 0, 0, VIDEO_WIDTH, VIDEO_HEIGHT);
    sg_end_pass();
    static uint8_t tmp[VIDEO_WIDTH * VIDEO_HEIGHT * 4];
    read_layer(g_vt.rt, tmp);
    for (int y = 0; y < VIDEO_HEIGHT; y++)
        memcpy(out + (size_t)y * VIDEO_WIDTH * 4, tmp + (size_t)(VIDEO_HEIGHT - 1 - y) * VIDEO_WIDTH * 4, (size_t)VIDEO_WIDTH * 4);
}

/* The pens read back from both layers, turned into colours in place through the
 * pen texture's texels (the alpha byte is left as read). */
static void verify_tiles_pens_to_colours(void) {
    for (int i = 0; i < VIDEO_WIDTH * VIDEO_HEIGHT; i++)
        for (int k = 0; k < 2; k++) {
            uint8_t *px = k ? &g_vt.fg[i * 4] : &g_vt.bg[i * 4];
            const uint8_t *c = &state.video.pal_texels[(px[0] | (px[1] << 8)) * 4];
            px[0] = c[0]; px[1] = c[1]; px[2] = c[2];
        }
}

/* How many pixels the indexed quads put on screen in another colour than the
 * CPU's, over both layers (a clear foreground pixel is not counted). */
static int verify_tiles_shown_bad(void) {
    int shown_bad = 0;
    for (int k = 0; k < 2; k++) {
        show_layer(k ? state.video.fg_view : state.video.bg_view, g_vt.shown);
        for (int i = 0; i < VIDEO_WIDTH * VIDEO_HEIGHT; i++) {
            const uint8_t *s = &g_vt.shown[i * 4];
            const uint8_t *c = k ? &state.video.fg_pixels[i * 4] : &state.video.bg_pixels[i * 4];
            if ((k == 0 || c[3]) && (s[0] != c[0] || s[1] != c[1] || s[2] != c[2])) shown_bad++;
        }
    }
    return shown_bad;
}

/* How many pixels of the GPU layers differ from the CPU's, and the first: its
 * index in `*first` and its layer in `*what` (left alone when none differs). */
static int verify_tiles_layers_bad(int *first, const char **what) {
    int bad = 0;
    for (int i = 0; i < VIDEO_WIDTH * VIDEO_HEIGHT; i++) {
        const uint8_t *gb = &g_vt.bg[i * 4], *cb = &state.video.bg_pixels[i * 4];
        const uint8_t *gf = &g_vt.fg[i * 4], *cf = &state.video.fg_pixels[i * 4];
        bool bg_ok = gb[0] == cb[0] && gb[1] == cb[1] && gb[2] == cb[2] && gb[3] == cb[3];
        bool fg_ok = gf[3] == cf[3] && (cf[3] == 0 || (gf[0] == cf[0] && gf[1] == cf[1] && gf[2] == cf[2]));
        if (!bg_ok || !fg_ok) {
            if (*first < 0) { *first = i; *what = bg_ok ? "fg" : "bg"; }
            bad++;
        }
    }
    return bad;
}

/* The first pixel that differed, GPU against CPU. */
static void verify_tiles_report(int bad, int first, const char *what) {
    int x = first % VIDEO_WIDTH, y = first / VIDEO_WIDTH;
    const uint8_t *g = strcmp(what, "bg") ? &g_vt.fg[first * 4] : &g_vt.bg[first * 4];
    const uint8_t *c = strcmp(what, "bg") ? &state.video.fg_pixels[first * 4] : &state.video.bg_pixels[first * 4];
    printf("verify-gpu-tiles: game frame %u, %d pixels differ; first %s at (%d,%d) gpu %02x%02x%02x%02x cpu %02x%02x%02x%02x\n",
           g_emu_frames, bad, what, x, y, g[0], g[1], g[2], g[3], c[0], c[1], c[2], c[3]);
}

/* Compare the GPU layers against a CPU compose of the same RAM: the pens the
 * targets hold, turned into colours through the pen texture's texels, and the
 * colours the indexed quads actually put on screen. */
static void verify_gpu_tiles(void) {
    video_compose_cpu(&state.video, &state.bus);
    read_layer(state.video.bg_image, g_vt.bg);
    read_layer(state.video.fg_image, g_vt.fg);
    verify_tiles_pens_to_colours();
    int shown_bad = verify_tiles_shown_bad();
    if (shown_bad && g_vt.bad_frames < 5)
        printf("verify-gpu-tiles: game frame %u, %d shown pixels differ from the CPU colours\n", g_emu_frames, shown_bad);
    int first = -1;
    const char *what = "";
    int bad = shown_bad + verify_tiles_layers_bad(&first, &what);
    g_vt.frames++;
    if (bad && g_vt.bad_frames++ < 5) verify_tiles_report(bad, first, what);
}

/* ---- --verify-fill ------------------------------------------------------------ */

static struct {
    uint64_t frames, bad_frames, skipped;
    int      w, h;
    sg_image color[2], depth[2];
    sg_view  color_att[2], depth_att[2];
    uint8_t *px[2];
} g_vf;

/* Replay this frame's fills (game_render's log) through the reference and the
 * normal fill shader into two scratch targets the game pass's size, and compare
 * the bytes. Call after the game pass, before anything else uploads. */
static void verify_fill(int w, int h) {
    if (g_fill_log.n == 0) return;
    if (g_fill_log.uploads != 1 || g_fill_log.overflow) { g_vf.skipped++; return; }
    if (g_vf.w != w || g_vf.h != h) {
        for (int k = 0; k < 2; k++) {
            g_vf.color[k] = sg_make_image(&(sg_image_desc){ .width = w, .height = h,
                .pixel_format = SG_PIXELFORMAT_RGBA8, .usage = { .color_attachment = true }, .label = "verify-fill" });
            g_vf.depth[k] = sg_make_image(&(sg_image_desc){ .width = w, .height = h,
                .pixel_format = SG_PIXELFORMAT_DEPTH_STENCIL, .usage = { .depth_stencil_attachment = true }, .label = "verify-fill-depth" });
            g_vf.color_att[k] = sg_make_view(&(sg_view_desc){ .color_attachment.image = g_vf.color[k] });
            g_vf.depth_att[k] = sg_make_view(&(sg_view_desc){ .depth_stencil_attachment.image = g_vf.depth[k] });
            g_vf.px[k] = malloc((size_t)w * (size_t)h * 4);
        }
        g_vf.w = w; g_vf.h = h;
    }
    for (int k = 0; k < 2; k++) {
        sg_begin_pass(&(sg_pass){
            .action = { .colors[0] = { .load_action = SG_LOADACTION_CLEAR, .clear_value = { 0, 0, 0, 0 } },
                        .depth = { .load_action = SG_LOADACTION_CLEAR, .clear_value = 1.0f } },
            .attachments = { .colors[0] = g_vf.color_att[k], .depth_stencil = g_vf.depth_att[k] },
        });
        game_render_replay_fills(k == 0, state.video.back_view, state.video.bg_view, state.video.pal_rgba_view);
        sg_end_pass();
        sg_gl_image_info gi = sg_gl_query_image_info(g_vf.color[k]);
        GLuint fbo;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gi.tex[gi.active_slot], 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, g_vf.px[k]);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        sg_reset_state_cache();
    }
    size_t n = (size_t)w * (size_t)h * 4, bad = 0, first = 0;
    int maxd = 0;
    for (size_t i = 0; i < n; i++) {
        int dlt = abs((int)g_vf.px[0][i] - (int)g_vf.px[1][i]);
        if (dlt) { if (!bad++) first = i; if (dlt > maxd) maxd = dlt; }
    }
    g_vf.frames++;
    if (bad && g_vf.bad_frames++ < 5)
        printf("verify-fill: game frame %u, %zu bytes differ (max %d), first at (%zu,%zu): ref %02x%02x%02x%02x new %02x%02x%02x%02x\n",
               g_emu_frames, bad, maxd, (first / 4) % (size_t)w, (first / 4) / (size_t)w,
               g_vf.px[0][first & ~3u], g_vf.px[0][(first & ~3u) + 1], g_vf.px[0][(first & ~3u) + 2], g_vf.px[0][(first & ~3u) + 3],
               g_vf.px[1][first & ~3u], g_vf.px[1][(first & ~3u) + 1], g_vf.px[1][(first & ~3u) + 2], g_vf.px[1][(first & ~3u) + 3]);
}

/* ---- Main ------------------------------------------------------------------ */

/* --versus, --versus-slave, --pad, --window-at: the arguments argv[i] takes
 * (val its value, NULL if none), 0 if a is none of them, -1 if its value is bad. */
static int parse_versus_arg(const char *a, const char *val) {
    if (!strcmp(a, "--versus")) { opt.versus = true; return 1; }
    if (!val) return 0;
    if (!strcmp(a, "--versus-slave")) {
        unsigned port = 0, token = 0;
        if (sscanf(val, "%u:%u", &port, &token) != 2 || !port || port > 65535u) return -1;
        opt.versus_port  = (uint16_t)port;
        opt.versus_token = token;
        return 2;
    }
    if (!strcmp(a, "--pad")) { opt.pad = atoi(val); return 2; }
    if (!strcmp(a, "--window-at")) {
        if (sscanf(val, "%d,%d", &opt.win_x, &opt.win_y) != 2) return -1;
        opt.win_at = true;
        return 2;
    }
    return 0;
}

/* A Versus is two windows, not with --netplay. */
static bool versus_args_check(void) {
    if (!opt.versus && !opt.versus_port) return true;
    if (opt.netplay) {
        fprintf(stderr, "m2hle: --versus is this machine's two cabinets; it does not go with --netplay\n");
        return false;
    }
    if (opt.versus && opt.versus_port) return false;
    if (opt.win_w <= 0 || opt.win_h <= 0) {
        opt.win_w = 640;   /* two cabinets, two windows: neither fullscreen */
        opt.win_h = 480;
    }
    return true;
}

static bool parse_args(int argc, char **argv) {
    int used;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool more = i + 1 < argc;
        if      (!strcmp(a, "--rom") && more)        opt.rom = argv[++i];
        else if (!strcmp(a, "--profile") && more)    opt.profile = argv[++i];
        else if (!strcmp(a, "--region") && more) {   /* japan | usa | export */
            int r = game_region_parse(argv[++i]);
            if (r < 0) return false;
            g_region = r;
        }
        else if (!strcmp(a, "--vs-mode"))           g_vs_mode = 1;
        else if (more && io5649_wheel_option(a, argv[i + 1])) i++;
        else if (more && lightgun_host_option(a, argv[i + 1])) i++;   /* --gun-flash N */
        else if (more && ffb_option(a, argv[i + 1]))          i++;
        else if (more && fanatec_option(a, argv[i + 1]))      i++;   /* --fanatec gear|speed|off */
        else if (more && lg4ff_option(a, argv[i + 1]))        i++;   /* --g25 off, --g25-range N */
        else if (!strcmp(a, "--wheel-axes") && more)    { if (!wheel_axes_option(argv[++i])) return false; }
        else if (!strcmp(a, "--wheel-buttons") && more) { if (!wheel_buttons_option(argv[++i])) return false; }
        else if (!strcmp(a, "--enemy-rank") && more) {  /* cabinet | easy..hardest | extra1 | extra2 */
            int r = enemy_rank_parse(argv[++i]);
            if (r < -1) return false;
            g_enemy_rank = r;
        }
        else if (!strcmp(a, "--no-nvram"))          g_backup_want = 0;
        else if (!strcmp(a, "--nvram-dir") && more) {
            snprintf(g_backup_dir, sizeof g_backup_dir, "%s", argv[++i]);
            g_backup_want = 1;
        }
        else if (!strcmp(a, "--render-fps") && more) opt.render_fps = atof(argv[++i]);
        else if (!strcmp(a, "--steps-per-slice") && more) {
            int n = atoi(argv[++i]);
            if (n < 1000) return false;
            g_emu_steps_per_slice = n;
        }
        else if (!strcmp(a, "--live-timers"))        ;   /* always on now */
        else if (!strcmp(a, "--window") && more)     { if (sscanf(argv[++i], "%dx%d", &opt.win_w, &opt.win_h) != 2) return false; }
        else if (!strcmp(a, "--stats"))              opt.stats = true;
        else if (!strcmp(a, "--sound"))              opt.sound = true;
        else if (!strcmp(a, "--sound-hle"))          g_sound_hle_want = 1;   /* the sound driver in C (sound_hle.h) */
        else if (!strcmp(a, "--osd"))                opt.osd = true;
        else if (!strcmp(a, "--log") && more)        opt.log_path = argv[++i];
        else if (!strcmp(a, "--log-level") && more)  opt.log_levels = argv[++i];
        else if (!strcmp(a, "--pad-map") && more)    opt.pad_map = argv[++i];
        else if (!strcmp(a, "--exit-after") && more) opt.exit_after = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--host-prof") && more) {
            char spec[400], file[300] = "";
            double start = 0, secs = 20;
            if (sscanf(argv[++i], "%lf:%lf:%299s", &start, &secs, file) < 2) return false;
            snprintf(spec, sizeof spec, "start=%g secs=%g%s%s", start, secs, file[0] ? " out=" : "", file);
            hprof_request(spec);
        }
        else if (!strcmp(a, "--gl-finish"))          opt.gl_finish = true;
        else if (!strcmp(a, "--render-scale") && more) opt.render_scale = atoi(argv[++i]);
        else if (!strcmp(a, "--display-scale") && more) opt.display_scale = atoi(argv[++i]);
        else if (!strcmp(a, "--cpu-tiles"))          opt.cpu_tiles = true;
        else if (!strcmp(a, "--no-mesh-cache"))      g_geo3d_mesh_cache = 0;
        else if (!strcmp(a, "--verify-gpu-tiles"))   opt.verify_gpu_tiles = true;
        else if (!strcmp(a, "--verify-fill"))        opt.verify_fill = true;
        else if (!strcmp(a, "--fill-ref"))           g_game_render_fill_use_ref = 1;
        else if (!strcmp(a, "--fill-no-split"))      g_game_render_fill_split = 0;
        else if (!strcmp(a, "--fill-no-ramp"))       g_game_render_fill_ramp = 0;
        else if (!strcmp(a, "--fill-shade-rows"))    g_game_render_fill_shade = 1;
        else if (!strcmp(a, "--match-replay"))       g_match_replay = 1;   /* attract straight to its replay fight */
        else if (!strcmp(a, "--netplay"))            opt.netplay = true;
        else if (!strcmp(a, "--net-config") && more) netplay_set_config_path(argv[++i]);   /* before netplay_init */
        else if (!strcmp(a, "--net-delay") && more)  opt.net_delay = atoi(argv[++i]);
        else if (!strcmp(a, "--net-host"))           opt.net_host = opt.netplay = true;
        else if (!strcmp(a, "--net-room-pass") && more) opt.net_room_pass = argv[++i];
        else if ((used = parse_versus_arg(a, more ? argv[i + 1] : NULL)) != 0) {
            if (used < 0) return false;
            i += used - 1;
        }
        else if (!strcmp(a, "--filter") && more) {
            const char *f = argv[++i];
            if (!strcmp(f, "linear")) opt.linear = true;
            else if (!strcmp(f, "nearest")) opt.linear = false;
            else return false;
        }
        else if (!strcmp(a, "--scripts") && more)    opt.scripts_dir = argv[++i];
        else if (!strcmp(a, "--no-scripts"))         opt.no_scripts = true;
        else if (!strcmp(a, "--script-option") && more && opt.script_opt_count < MAX_SCRIPT_OPTS)
            opt.script_opt[opt.script_opt_count++] = argv[++i];
        else if (!strcmp(a, "--max-temp") && more)   opt.max_temp = atof(argv[++i]);
        else if (!strcmp(a, "--shot") && more && opt.shot_count < MAX_SHOTS) {
            char *spec = argv[++i], *colon = strchr(spec, ':');
            if (!colon) return false;
            *colon = '\0';
            opt.shot_frame[opt.shot_count] = (unsigned)strtoul(spec, NULL, 0);
            opt.shot_path[opt.shot_count++] = colon + 1;
        }
        else if (a[0] != '-' && !opt.rom)            opt.rom = a;
        else { fprintf(stderr, "m2hle: bad argument '%s'\n", a); return false; }
    }
    if (!versus_args_check()) return false;
    return opt.rom != NULL;
}

static int read_milli(const char *path) {
    FILE *f = fopen(path, "r");
    int v = -1;
    if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
    return v;
}

/* ---- On-screen status line (--osd) ---------------------------------------- */

#define OSD_BATTERY "/sys/class/power_supply/battery/"

static struct {
    Uint64   window_start;   /* 0 until the first sample */
    unsigned renders, frames0;
    char     text[64];
} g_osd;

/* Count one rendered frame; once a second, rebuild the text from the frame
 * rates over that second and fresh sysfs readings. */
static void osd_update(Uint64 now) {
    g_osd.renders++;
    if (g_osd.window_start && now - g_osd.window_start < 1000000000ull) return;

    char fps[16] = "--/--fps";
    if (g_osd.window_start) {
        double secs = (double)(now - g_osd.window_start) / 1e9;
        snprintf(fps, sizeof fps, "%d/%dfps", (int)(g_osd.renders / secs + 0.5),
                 (int)((g_emu_frames - g_osd.frames0) / secs + 0.5));
    }
    g_osd.window_start = now;
    g_osd.renders      = 0;
    g_osd.frames0      = g_emu_frames;

    size_t n = (size_t)snprintf(g_osd.text, sizeof g_osd.text, "%s", fps);
    int cpu = read_milli("/sys/class/thermal/thermal_zone0/temp");
    int gpu = read_milli("/sys/class/thermal/thermal_zone1/temp");
    int hot = cpu > gpu ? cpu : gpu;
    if (hot > 0 && n < sizeof g_osd.text)
        n += (size_t)snprintf(g_osd.text + n, sizeof g_osd.text - n, " %dC", (hot + 500) / 1000);
    time_t t = time(NULL);
    struct tm lt;
    if (localtime_r(&t, &lt) && n < sizeof g_osd.text)
        n += (size_t)snprintf(g_osd.text + n, sizeof g_osd.text - n, " %02d:%02d", lt.tm_hour, lt.tm_min);
    int bat = read_milli(OSD_BATTERY "capacity");
    if (bat >= 0 && n < sizeof g_osd.text) {
        char status[16] = "";
        FILE *f = fopen(OSD_BATTERY "status", "r");
        if (f) { if (!fgets(status, sizeof status, f)) status[0] = '\0'; fclose(f); }
        snprintf(g_osd.text + n, sizeof g_osd.text - n, " %d%%%s", bat,
                 strncmp(status, "Charging", 8) ? "" : "+");
    }
}

/* Draw the text right-aligned in the top-right corner of the current pass,
 * white on a one-pixel black shadow so it reads over any scene. */
static void osd_draw(int fb_w, int fb_h) {
    float scale = fb_h >= 720 ? 3.0f : 2.0f;    /* 8x8 font → 16 px cells at 480p */
    float cols  = (float)fb_w / scale / 8.0f;
    float x     = cols - (float)strlen(g_osd.text) - 0.25f;
    /* sdtx draws through whatever viewport is in force: the game's picture
     * left its own letterbox (and its scissor) set, not the whole screen. */
    sg_apply_viewport(0, 0, fb_w, fb_h, true);
    sg_apply_scissor_rect(0, 0, fb_w, fb_h, true);
    sdtx_canvas((float)fb_w / scale, (float)fb_h / scale);
    sdtx_origin(0.0f, 0.0f);
    sdtx_pos(x + 0.125f, 0.25f + 0.125f);
    sdtx_color3b(0, 0, 0);
    sdtx_puts(g_osd.text);
    sdtx_pos(x, 0.25f);
    sdtx_color3b(255, 255, 255);
    sdtx_puts(g_osd.text);
    /* Drawn by the caller, once: sdtx uploads its vertices at the first
     * sdtx_draw of a frame, so text recorded after it would not show. */
}

/* ---- Netplay (--netplay) ------------------------------------------------------ */
/*
 * The same netplay.h the debugger's window drives, driven from a gamepad. It
 * reads a status snapshot and posts commands, and never touches netplay state
 * itself; the emu thread pumps the session (emu_netplay_pump), as everywhere.
 *
 * There is no keyboard to type an account into, so sign-in is whatever the
 * settings file holds: in practice the PC's file copied across, whose Twitch
 * login token stands in for a password. RPCN allows one session per account, so
 * the PC's emulator and the handheld cannot both be signed in as the same one.
 */

/* Set by the reset below (emu thread): the board forgot what the pad holds. */
static volatile int g_pad_resync;

/* The board reset at the netplay barrier: main.c netplay_reset_board_cb, with
 * this frontend's sound board. Called by the emu thread with the emu mutex held. */
static void sdl_netplay_reset_cb(void *ctx) {
    (void)ctx;
    if (!g_active_profile) return;
    g_active_profile->install_fn(&state.romset, &state.cpu, &state.bus);
    irqt_reset();
    if (g_sound_on) {
        sound_select_board(g_active_profile->quirks.model1_sound);
        sound_reset();
        sound_attach(&state.bus);
        sound_load_rom(state.romset.audiocpu, (uint32_t)state.romset.audiocpu_size);
        if (state.romset.samples && state.romset.samples_size > 0)
            sound_load_samples(state.romset.samples, (uint32_t)state.romset.samples_size);
    }
    input_reset();
    input_attach(&state.bus);
    emu_board_reset_state();
    g_pad_resync = 1;
}

#include "pad_lobby.h"

/* Before the emu thread: it pumps the session from its first slice. */
static void netplay_start(void) {
    g_lobby.net_host      = opt.net_host;
    g_lobby.net_delay     = opt.net_delay;
    g_lobby.net_room_pass = opt.net_room_pass;
    g_lobby.take_pad      = lobby_pad_release;
    lobby_init();
    netplay_set_reset_hook(sdl_netplay_reset_cb, NULL);
    if (lobby_can_sign_in()) lobby_sign_in();
}

/* lobby_event's pad button press: Guide or L1+R1 toggle the lobby; while it is
 * open, the d-pad moves and A or B is held for its release. */
static void lobby_pad_down(const SDL_Event *ev, bool *toggle, bool *used) {
    SDL_Gamepad *pad = SDL_GetGamepadFromID(ev->gbutton.which);
    int b = ev->gbutton.button;
    if (b == SDL_GAMEPAD_BUTTON_GUIDE) *toggle = true;
    else if (pad && ((b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER
                      && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER))
                  || (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER
                      && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))))
        *toggle = true;
    else if (g_lobby.open) {
        *used = true;
        if (b == SDL_GAMEPAD_BUTTON_DPAD_UP)   lobby_move(-1);
        if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) lobby_move(+1);
        if (b == SDL_GAMEPAD_BUTTON_SOUTH || b == SDL_GAMEPAD_BUTTON_EAST)
            g_lobby.pressed = b;
    }
}

/* lobby_event's key press: F1 toggles the lobby; while it is open, every key
 * but Escape is the lobby's, the arrows moving, Return picking, Backspace closing. */
static void lobby_key_down(const SDL_Event *ev, bool *toggle, bool *used) {
    if (ev->key.scancode == SDL_SCANCODE_F1) *toggle = true;
    else if (g_lobby.open && ev->key.scancode != SDL_SCANCODE_ESCAPE) {
        *used = true;
        if (ev->key.scancode == SDL_SCANCODE_UP)        lobby_move(-1);
        if (ev->key.scancode == SDL_SCANCODE_DOWN)      lobby_move(+1);
        if (ev->key.scancode == SDL_SCANCODE_RETURN)    lobby_activate();
        if (ev->key.scancode == SDL_SCANCODE_BACKSPACE) lobby_show(false);
    }
}

/* The netplay lobby takes the pad and the keyboard while it is open: L1+R1,
 * Guide or F1 toggle it; the d-pad moves, A picks and B closes, both on
 * release, so the button is up again before the game resumes. True if the
 * event was the lobby's and the game must not see it. */
static bool lobby_event(const SDL_Event *ev, bool *pad_changed) {
    bool toggle = false, used = false;
    if (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        lobby_pad_down(ev, &toggle, &used);
    } else if (ev->type == SDL_EVENT_GAMEPAD_BUTTON_UP && g_lobby.open) {
        used = true;
        if (ev->gbutton.button == g_lobby.pressed) {
            g_lobby.pressed = -1;
            if (ev->gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) lobby_activate();
            else lobby_show(false);
        }
    } else if (ev->type == SDL_EVENT_KEY_DOWN && !ev->key.repeat) {
        lobby_key_down(ev, &toggle, &used);
    } else if (ev->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && g_lobby.open) {
        used = true;
    }
    if (toggle) {
        g_lobby.pressed = -1;
        lobby_show(!g_lobby.open);
        *pad_changed = true;   /* closing: pick up what is still held */
    }
    return toggle || used;
}

/* The board was reset under the pad (netplay's barrier, a Versus's role
 * boot): it holds nothing now, so press again whatever is still down. */
static void pad_resync(bool *pad_changed) {
    if (!g_pad_resync) return;
    g_pad_resync = 0;
    memset(g_pad_held, 0, sizeof g_pad_held);
    *pad_changed = true;
}

/* Who holds the pad after this round of events. */
static void lobby_pad_settle(bool *pad_changed) {
    static bool lobby_was_open;
    if (lobby_was_open && !g_lobby.open) *pad_changed = true;
    lobby_was_open = g_lobby.open;
    if (g_lobby.open) *pad_changed = false;   /* the lobby has the pad */
}

/* Player 1's light gun on the mouse or a finger (Pinboard #1040), as on the
 * web page (main_web.c, web_gun_event): the mouse aims, its left button fires
 * and its right one reloads; a finger fires where it lands, or reloads when it
 * lands off the picture. render_frame keeps where the picture went, in
 * framebuffer pixels (w 0 while the lobby hides it), and the framebuffer's
 * pixels per window unit, which SDL's mouse is in. lightgun.h ignores the
 * pointer in a session. */
static float g_gun_rect[4], g_gun_px_x = 1.0f, g_gun_px_y = 1.0f, g_gun_fb[2];
static SDL_FingerID g_gun_finger;
static bool g_gun_finger_on;

static void gun_aim_px(float x, float y) {
    lightgun_host_aim(0, x, y, g_gun_rect[0], g_gun_rect[1], g_gun_rect[2], g_gun_rect[3]);
}

static void gun_event(const SDL_Event *ev) {
    if (!lightgun_host_on()) return;
    const bool live = g_gun_rect[2] > 0.0f;
    switch (ev->type) {
        case SDL_EVENT_MOUSE_MOTION:
            if (ev->motion.which != SDL_TOUCH_MOUSEID && live && !g_gun_finger_on)
                gun_aim_px(ev->motion.x * g_gun_px_x, ev->motion.y * g_gun_px_y);
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            if (!g_gun_finger_on) lightgun_host_aim_away(0);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            if (ev->button.which == SDL_TOUCH_MOUSEID) break;
            const bool down = ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            if (down && (!live || g_gun_finger_on)) break;
            if (down) gun_aim_px(ev->button.x * g_gun_px_x, ev->button.y * g_gun_px_y);
            if (ev->button.button == SDL_BUTTON_LEFT)  lightgun_host_button(0, LIGHTGUN_HOST_TRIGGER, down);
            if (ev->button.button == SDL_BUTTON_RIGHT) lightgun_host_button(0, LIGHTGUN_HOST_RELOAD, down);
            break;
        }
        case SDL_EVENT_FINGER_DOWN: {
            if (g_gun_finger_on || !live) break;
            const float x = ev->tfinger.x * g_gun_fb[0], y = ev->tfinger.y * g_gun_fb[1];
            const float *r = g_gun_rect;
            g_gun_finger = ev->tfinger.fingerID;
            g_gun_finger_on = true;
            gun_aim_px(x, y);
            bool on = x >= r[0] && x <= r[0] + r[2] && y >= r[1] && y <= r[1] + r[3];
            lightgun_host_button(0, on ? LIGHTGUN_HOST_TRIGGER : LIGHTGUN_HOST_RELOAD, true);
            break;
        }
        case SDL_EVENT_FINGER_MOTION:
            if (g_gun_finger_on && ev->tfinger.fingerID == g_gun_finger && live)
                gun_aim_px(ev->tfinger.x * g_gun_fb[0], ev->tfinger.y * g_gun_fb[1]);
            break;
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
            if (!g_gun_finger_on || ev->tfinger.fingerID != g_gun_finger) break;
            g_gun_finger_on = false;
            lightgun_host_button(0, LIGHTGUN_HOST_TRIGGER, false);
            lightgun_host_button(0, LIGHTGUN_HOST_RELOAD, false);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            g_gun_finger_on = false;
            lightgun_host_release_all();
            break;
        default: break;
    }
}

/* Drain SDL's queue: the keyboard and gamepads into the board's inputs.
 * False once the player asked to quit. */
static bool poll_events(void) {
    SDL_Event ev;
    bool running = true, pad_changed = false;
    while (SDL_PollEvent(&ev)) {
        if (opt.netplay && lobby_event(&ev, &pad_changed)) continue;
        gun_event(&ev);
        sdl_pads_event(&ev, &pad_changed);
        switch (ev.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.scancode == SDL_SCANCODE_ESCAPE) running = false;
                else if (!ev.key.repeat) input_action_down(key_to_action(ev.key.scancode));
                break;
            case SDL_EVENT_KEY_UP:
                input_action_up(key_to_action(ev.key.scancode));
                break;
            default: break;
        }
    }
    pad_resync(&pad_changed);
    if (opt.netplay) lobby_pad_settle(&pad_changed);
    if (pad_changed) pad_refresh();
    return running;
}

/* ---- The window and the game's target -------------------------------------- */

static SDL_Window   *g_window;
static SDL_GLContext g_gl;

/* A GLES 3 context in a window, fullscreen unless --window. */
static bool gl_window_open(void) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    /* No alpha channel: the tile layers leave alpha < 1, and a compositor
     * would blend the frontend beneath the window through it. */
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    bool fullscreen = opt.win_w <= 0 || opt.win_h <= 0;
    g_window = SDL_CreateWindow("m2hle", fullscreen ? 640 : opt.win_w, fullscreen ? 480 : opt.win_h,
                                SDL_WINDOW_OPENGL | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    g_gl = g_window ? SDL_GL_CreateContext(g_window) : NULL;
    if (!g_gl) {
        fprintf(stderr, "m2hle: GLES 3 window: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(g_window, g_gl);
    /* Paced by the render timer below; vsync on top would stall a skipped frame. */
    SDL_GL_SetSwapInterval(opt.render_fps >= 59.0 ? 1 : 0);
    if (opt.win_at) SDL_SetWindowPosition(g_window, opt.win_x, opt.win_y);
    SDL_HideCursor();
    printf("m2hle %s: %s on %s (%s)\n", M2HLE_VERSION, g_active_profile->display_name,
           (const char *)glGetString(GL_RENDERER), SDL_GetCurrentVideoDriver());
    return true;
}

/* ---- Versus on this machine (--versus) ----------------------------------------- */
/*
 * Two cabinets, as the arcade's Versus is: this process and a second one it
 * starts, each running one board, their comm boards linked over 127.0.0.1
 * (net/comm_local.h). The link is netplay's comm link without the room: the
 * MASTER is this window, the SLAVE the other, each boots into its role
 * (comm_role.h, through the netplay reset hook) and is paced by the link
 * (link_pace.h). Each window plays one pad, so the pads go on being read
 * while the other window has the focus.
 */

static child_proc_t g_versus_child;
static bool         g_versus_child_up;
static char       **g_versus_cmdline;   /* argv as given: parse_args cuts --shot's in two */

static void versus_keep_cmdline(int argc, char **argv) {
    g_versus_cmdline = calloc((size_t)argc + 1u, sizeof *g_versus_cmdline);
    for (int i = 0; g_versus_cmdline && i < argc; i++) g_versus_cmdline[i] = SDL_strdup(argv[i]);
}

#define VERSUS_ARGS_MAX 96
static char  g_versus_buf[VERSUS_ARGS_MAX][320];
static char *g_versus_argv[VERSUS_ARGS_MAX + 1];

/* A file of the SLAVE's: the MASTER's name with "-slave" before its extension,
 * so the two cabinets do not write one log or one screenshot. */
static void versus_slave_file(char *out, size_t cap, const char *path) {
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash)) dot = path + strlen(path);
    snprintf(out, cap, "%.*s-slave%s", (int)(dot - path), path, dot);
}

static bool versus_arg(int *n, const char *fmt, ...) {
    if (*n >= VERSUS_ARGS_MAX) return false;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_versus_buf[*n], sizeof g_versus_buf[*n], fmt, ap);
    va_end(ap);
    g_versus_argv[*n] = g_versus_buf[*n];
    (*n)++;
    return true;
}

/* A file argument's value for the SLAVE: --log's renamed, --shot's FRAME:FILE
 * with FILE renamed. */
static bool versus_file_arg(int *n, const char *a, const char *val) {
    char file[300];
    if (!strcmp(a, "--log")) {
        versus_slave_file(file, sizeof file, val);
        return versus_arg(n, "%s", file);
    }
    const char *colon = strchr(val, ':');
    versus_slave_file(file, sizeof file, colon ? colon + 1 : val);
    return versus_arg(n, "%.*s:%s", colon ? (int)(colon - val) : 0, val, file);
}

/* Copy argv[*i] (and its value) to the SLAVE's command line, unless it is the
 * MASTER's alone (--versus, --pad, --window-at). False when it is full. */
static bool versus_copy_arg(int *n, char **argv, int argc, int *i) {
    const char *a = argv[*i];
    bool more = *i + 1 < argc;
    if (!strcmp(a, "--versus")) return true;
    if ((!strcmp(a, "--pad") || !strcmp(a, "--window-at")) && more) { ++*i; return true; }
    if (!versus_arg(n, "%s", a)) return false;
    if (more && (!strcmp(a, "--log") || !strcmp(a, "--shot"))) return versus_file_arg(n, a, argv[++*i]);
    return true;
}

/* The SLAVE's command line: this one without what is the MASTER's alone,
 * its files renamed, and its end of the link. */
static bool versus_slave_argv(int argc, int x, int y) {
    char **argv = g_versus_cmdline;
    if (!argv) return false;
    int n = 0;
    bool ok = versus_arg(&n, "%s", argv[0]);
    for (int i = 1; i < argc && ok; i++) ok = versus_copy_arg(&n, argv, argc, &i);
    ok = ok && versus_arg(&n, "--versus-slave") && versus_arg(&n, "%u:%u", g_comm_local.port, g_comm_local.token)
            && versus_arg(&n, "--pad") && versus_arg(&n, "1")
            && versus_arg(&n, "--window-at") && versus_arg(&n, "%d,%d", x, y);
    g_versus_argv[n] = NULL;
    return ok;
}

/* The MASTER's window on the left of where it would be, the SLAVE's on the right. */
static void versus_place(int *slave_x, int *slave_y) {
    int x = 0, y = 0, w = 0, h = 0;
    SDL_GetWindowPosition(g_window, &x, &y);
    SDL_GetWindowSize(g_window, &w, &h);
    if (!opt.win_at) {
        x -= w / 2 + 8;
        SDL_SetWindowPosition(g_window, x, y);
    }
    *slave_x = x + w + 16;
    *slave_y = y;
}

/* The link, and on the MASTER the SLAVE's process. Before the emu thread. */
static bool versus_start(int argc) {
    if (!opt.versus && !opt.versus_port) return true;
    SDL_srand(0);
    uint16_t epoch = (uint16_t)(SDL_rand_bits() | 1u);
    bool master = opt.versus;
    if (master ? !comm_local_listen(&g_comm_local, SDL_rand_bits(), epoch)
               : !comm_local_connect(&g_comm_local, opt.versus_port, opt.versus_token, epoch)) {
        fprintf(stderr, "m2hle: --versus: no UDP socket on 127.0.0.1\n");
        return false;
    }
    if (master) {
        if (opt.pad < 0) opt.pad = 0;
        g_sdl_pad_only = opt.pad;
        int sx, sy;
        versus_place(&sx, &sy);
        if (!versus_slave_argv(argc, sx, sy) || !child_proc_start(&g_versus_child, g_versus_argv)) {
            fprintf(stderr, "m2hle: --versus: could not start the second cabinet\n");
            comm_local_close(&g_comm_local);
            return false;
        }
        g_versus_child_up = true;
    }
    netplay_set_reset_hook(sdl_netplay_reset_cb, NULL);
    comm_local_begin(&g_comm_local);
    char title[96];
    snprintf(title, sizeof title, "m2hle Versus: %s cabinet (pad %d)", comm_role_name(g_comm_local.role),
             opt.pad + 1);
    SDL_SetWindowTitle(g_window, title);
    printf("m2hle: Versus: this window is the %s cabinet, pad %d; link on 127.0.0.1:%u\n",
           comm_role_name(g_comm_local.role), opt.pad + 1, g_comm_local.port);
    fflush(stdout);
    return true;
}

/* Two windows, one pad each: a pad plays its window with the focus on the
 * other. Before SDL_Init. */
static void versus_before_sdl(void) {
    if (opt.versus || opt.versus_port) SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
}

/* The MASTER's run loop ends once the other window is closed. */
static bool versus_other_gone(void) {
    return g_versus_child_up && !child_proc_alive(&g_versus_child);
}

static void versus_stop(void) {
    if (!opt.versus && !opt.versus_port) return;
    const comm_link_t *l = &g_comm_local.link;
    printf("m2hle: Versus: sent %u took %u (stale %u torn %u too big %u overflow %u), "
           "ring %s, node %u of %u\n", l->sent, l->taken, l->stale, l->torn, l->too_big, l->overflow,
           g_m2comm_link.alive == M2COMM_LINK_UP ? "up" : "down", g_m2comm_link.id, g_m2comm_link.count);
    comm_local_close(&g_comm_local);
    if (g_versus_child_up) child_proc_stop(&g_versus_child);
    g_versus_child_up = false;
}

/* sokol and the renderers on the new context. 0, or the exit code. */
static int gfx_start(void) {
    sg_setup(&(sg_desc){
        .environment.defaults = { .color_format = SG_PIXELFORMAT_RGBA8,
                                  .depth_format = SG_PIXELFORMAT_DEPTH_STENCIL,
                                  .sample_count = 1 },
        .logger.func = slog_func,
    });
    if (!sg_isvalid()) { fprintf(stderr, "m2hle: sokol_gfx setup failed\n"); return 1; }
    if (opt.stats) { sg_enable_stats(); g_game_frame_gl_counts = true; }   /* the "gl per render" line */
    g_sdtx_on = opt.osd || opt.netplay || g_m2lua.loaded;
    if (g_sdtx_on)
        sdtx_setup(&(sdtx_desc_t){ .fonts[0] = sdtx_font_cpc(), .logger.func = slog_func });
    g_video_force_cpu_tiles = opt.cpu_tiles;
    if (opt.verify_fill && opt.render_scale <= 0) {
        fprintf(stderr, "m2hle: --verify-fill needs the offscreen game pass (--render-scale 1 or more)\n");
        return 2;
    }
    g_game_render_fill_verify = opt.verify_fill;
    game_render_init();
    video_init(&state.video);
    printf("m2hle: tile layers composed on the %s\n", state.video.gpu ? "GPU" : "CPU");
    geo3d_init(&state.geo3d);
    g_geo3d_state = &state.geo3d;
    return 0;
}

/* Offscreen target at the board's resolution (times --render-scale). */
static struct {
    int     w, h;
    sg_image color, depth;
    sg_view  color_att, depth_att, texture;
} g_rt;

static void game_target_make(void) {
    g_rt.w = game_render_wide_width() * opt.render_scale;
    g_rt.h = VIDEO_HEIGHT * opt.render_scale;
    if (opt.render_scale <= 0) return;
    g_rt.color = sg_make_image(&(sg_image_desc){
        .usage = { .color_attachment = true }, .width = g_rt.w, .height = g_rt.h,
        .pixel_format = SG_PIXELFORMAT_RGBA8, .sample_count = 1, .label = "game-target" });
    g_rt.depth = sg_make_image(&(sg_image_desc){
        .usage = { .depth_stencil_attachment = true }, .width = g_rt.w, .height = g_rt.h,
        .pixel_format = SG_PIXELFORMAT_DEPTH_STENCIL, .sample_count = 1, .label = "game-target-depth" });
    g_rt.color_att = sg_make_view(&(sg_view_desc){ .color_attachment.image = g_rt.color });
    g_rt.depth_att = sg_make_view(&(sg_view_desc){ .depth_stencil_attachment.image = g_rt.depth });
    g_rt.texture   = sg_make_view(&(sg_view_desc){ .texture.image = g_rt.color });
}

static void game_target_destroy(void) {
    if (opt.render_scale <= 0) return;
    sg_destroy_view(g_rt.texture);
    sg_destroy_view(g_rt.depth_att);
    sg_destroy_view(g_rt.color_att);
    sg_destroy_image(g_rt.depth);
    sg_destroy_image(g_rt.color);
}

/* ---- Statistics (--stats, --gl-finish) --------------------------------------- */

/* Sums over the current 5 s window; the video counters are where it began. */
static struct {
    Uint64   start, cpu_ns, gpu_ns, swap_ns, tile_gpu_ns, game_gpu_ns;
    uint64_t comp0, uptile0, upgfx0, uppens0, part0, blk0;
    unsigned renders, frames0;
    sg_frame_stats gl;   /* sums of sokol's per-frame counts over the window */
} g_st;

/* Time a GL wait: what it blocked for. */
static Uint64 gl_finish_ns(void) {
    Uint64 f0 = SDL_GetTicksNS();
    glFinish();
    return SDL_GetTicksNS() - f0;
}

/* sg_commit just closed the frame: it is prev_frame now. */
static void stats_add_gl(void) {
    sg_stats st = sg_query_stats();
    const sg_frame_stats *g = &st.prev_frame;
    sg_frame_stats *s = &g_st.gl;
    s->num_passes += g->num_passes;               s->num_apply_pipeline += g->num_apply_pipeline;
    s->num_apply_bindings += g->num_apply_bindings; s->num_apply_uniforms += g->num_apply_uniforms;
    s->num_draw += g->num_draw;
    s->num_update_buffer += g->num_update_buffer; s->size_update_buffer += g->size_update_buffer;
    s->num_append_buffer += g->num_append_buffer; s->size_append_buffer += g->size_append_buffer;
    s->num_update_image += g->num_update_image;   s->size_update_image += g->size_update_image;
    s->gl.num_bind_buffer += g->gl.num_bind_buffer;   s->gl.num_bind_texture += g->gl.num_bind_texture;
    s->gl.num_use_program += g->gl.num_use_program;   s->gl.num_uniform += g->gl.num_uniform;
    s->gl.num_render_state += g->gl.num_render_state;
    s->gl.num_vertex_attrib_pointer += g->gl.num_vertex_attrib_pointer;
}

/* Every 5 s: print the window's rates and per-render costs, then start a new one. */
static void stats_report(Uint64 now, bool audio) {
    if (now - g_st.start < 5000000000ull) return;
    double secs = (double)(now - g_st.start) / 1e9;
    double n = g_st.renders ? (double)g_st.renders : 1.0;
    const game_frame_times_t *t = &g_game_frame_times;
    printf("m2hle: game %.1f fps, render %.1f fps | per render ms: cpu %.2f [compose %.2f scan %.2f "
           "upload %.2f 3d %.2f tiles %.2f]%s%.2f swap %.2f | cpu %.1fC gpu %.1fC\n",
           (g_emu_frames - g_st.frames0) / secs, g_st.renders / secs,
           g_st.cpu_ns / 1e6 / n, t->compose_us / 1e3 / n, t->scan_us / 1e3 / n,
           t->upload_us / 1e3 / n, t->draw3d_us / 1e3 / n, t->tiles_us / 1e3 / n,
           opt.gl_finish ? " gpu " : " ", opt.gl_finish ? g_st.gpu_ns / 1e6 / n : 0.0,
           g_st.swap_ns / 1e6 / n,
           read_milli("/sys/class/thermal/thermal_zone0/temp") / 1000.0,
           read_milli("/sys/class/thermal/thermal_zone1/temp") / 1000.0);
    const video_state_t *v = &state.video;
    uint64_t comps = v->gpu_composes - g_st.comp0;
    printf("m2hle: tiles: %llu composes (%.0f%% of renders, %llu partial, %.0f%% of the screen drawn), "
           "uploads tile %llu gfx %llu pens %llu",
           (unsigned long long)comps, 100.0 * (double)comps / n,
           (unsigned long long)(v->partial_composes - g_st.part0),
           comps ? 100.0 * (double)(v->composed_blocks - g_st.blk0) / ((double)comps * VIDEO_BLK_W * VIDEO_BLK_H) : 0.0,
           (unsigned long long)(v->up_tile - g_st.uptile0), (unsigned long long)(v->up_gfx - g_st.upgfx0),
           (unsigned long long)(v->up_pens - g_st.uppens0));
    if (opt.gl_finish && comps)
        printf(" | gpu %.2f ms per compose", g_st.tile_gpu_ns / 1e6 / (double)comps);
    if (opt.gl_finish && opt.render_scale > 0)
        printf(" | game pass gpu %.2f ms per render", g_st.game_gpu_ns / 1e6 / n);
    if (audio)
        printf(" | sound: %llu underrun frames, %llu dropped total",
               (unsigned long long)g_audio_out.underruns, (unsigned long long)g_sound.out_dropped);
    printf("\n");
    /* What the frame asks of the GL driver, per render (sokol's counts;
     * all drawing goes through sokol). The Mali driver's CPU time is
     * paid per call and per byte, so these are the baseline for it. */
    const sg_frame_stats *g = &g_st.gl;
    printf("m2hle: gl per render: %.0f passes, %.0f pipelines, %.0f bindings, %.0f uniform blocks, "
           "%.0f draws (3d %.0f: %.0f pipelines, %.0f bindings, %.0f uniforms, %.0f buffer writes %.1f KB) | "
           "buffer updates %.1f (%.1f KB), appends %.1f (%.1f KB), image updates %.2f (%.1f KB) | "
           "gl calls: bind buffer %.0f, bind texture %.0f, use program %.0f, uniform %.0f, "
           "render state %.0f, vertex attrib %.0f\n",
           g->num_passes / n, g->num_apply_pipeline / n, g->num_apply_bindings / n, g->num_apply_uniforms / n,
           g->num_draw / n, t->draw3d_draws / n, t->draw3d_pipelines / n, t->draw3d_bindings / n,
           t->draw3d_uniforms / n, t->draw3d_buf_writes / n, t->draw3d_buf_bytes / 1024.0 / n,
           g->num_update_buffer / n, g->size_update_buffer / 1024.0 / n,
           g->num_append_buffer / n, g->size_append_buffer / 1024.0 / n,
           g->num_update_image / n, g->size_update_image / 1024.0 / n,
           g->gl.num_bind_buffer / n, g->gl.num_bind_texture / n, g->gl.num_use_program / n,
           g->gl.num_uniform / n, g->gl.num_render_state / n, g->gl.num_vertex_attrib_pointer / n);
    fflush(stdout);
    memset(&g_st, 0, sizeof g_st);
    g_st.comp0 = v->gpu_composes; g_st.uptile0 = v->up_tile; g_st.upgfx0 = v->up_gfx; g_st.uppens0 = v->up_pens;
    g_st.part0 = v->partial_composes; g_st.blk0 = v->composed_blocks;
    g_st.start = now; g_st.frames0 = g_emu_frames;
    memset(&g_game_frame_times, 0, sizeof g_game_frame_times);
}

/* The target at the picture's width, after a script turned widescreen on or off. */
static void game_target_fit(void) {
    if (!m2lua_wide_apply()) return;
    game_target_destroy();
    game_target_make();
}

/* ---- One render ------------------------------------------------------------------ */

/* Bring the tile layers and the 3D up to the board's latest frame. Returns how
 * long it waited on the GPU, which is not CPU time. */
static Uint64 frame_prepare(void) {
    if (opt.gl_finish) {
        /* Only the tile compose puts GPU work in prepare: the wait after it is
         * that pass's GPU time (uploads included), kept out of the CPU time. */
        uint64_t composes = state.video.gpu_composes;
        game_frame_prepare(&state.video, &state.geo3d, &state.bus, &state.romset, true);
        if (state.video.gpu_composes == composes) return 0;
        Uint64 waited = gl_finish_ns();
        g_st.tile_gpu_ns += waited;
        return waited;
    }
    if (opt.verify_gpu_tiles && state.video.gpu) {
        /* Hold the emu thread so both composes see the same RAM. */
        emu_mutex_lock(&state.emu.mutex);
        uint64_t composes = state.video.gpu_composes + state.video.up_pens;
        game_frame_prepare(&state.video, &state.geo3d, &state.bus, &state.romset, true);
        if (state.video.gpu_composes + state.video.up_pens != composes) verify_gpu_tiles();
        emu_mutex_unlock(&state.emu.mutex);
        return 0;
    }
    game_frame_prepare(&state.video, &state.geo3d, &state.bus, &state.romset, true);
    return 0;
}

/* Where the picture goes on a fb_w x fb_h screen: letterboxed to fit, or at
 * --display-scale, centred, when that fits. */
static void screen_rect(int fb_w, int fb_h, int *ox, int *oy, int *w, int *h) {
    const int pic_w = game_render_wide_width();
    game_rect_t r = game_render_letterbox(fb_w, fb_h, pic_w, VIDEO_HEIGHT);
    *ox = r.x; *oy = r.y; *w = r.w; *h = r.h;
    if (opt.display_scale > 0 && pic_w * opt.display_scale <= fb_w
            && VIDEO_HEIGHT * opt.display_scale <= fb_h) {
        *w  = pic_w * opt.display_scale;
        *h  = VIDEO_HEIGHT * opt.display_scale;
        *ox = (fb_w - *w) / 2;
        *oy = (fb_h - *h) / 2;
    }
}

/* Where the light guns aim from this frame (gun_event), and their crosshairs. */
static void gun_place(int fb_w, int fb_h, int ox, int oy, int w, int h) {
    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(g_window, &win_w, &win_h);
    g_gun_px_x = win_w > 0 ? (float)fb_w / (float)win_w : 1.0f;
    g_gun_px_y = win_h > 0 ? (float)fb_h / (float)win_h : 1.0f;
    g_gun_fb[0] = (float)fb_w;
    g_gun_fb[1] = (float)fb_h;
    g_gun_rect[0] = (float)ox; g_gun_rect[1] = (float)oy;
    g_gun_rect[2] = (float)w;  g_gun_rect[3] = (float)h;
    if (!lightgun_host_on() || w <= 0) return;
    if (!g_script_img.ready) scripts_images_setup();   /* sgl_setup */
    lightgun_sgl_record(ox, oy, w, h, fb_w, fb_h);
    sgl_draw();
}

static const sg_pass_action g_pass_action = {
    .colors[0] = { .load_action = SG_LOADACTION_CLEAR, .clear_value = { 0.0f, 0.0f, 0.0f, 1.0f } },
    .depth     = { .load_action = SG_LOADACTION_CLEAR, .clear_value = 1.0f },
};

/* What the screen pass draws: the game picture (or the lobby's black), the
 * scripts' layer, the OSD, the lobby and the debug text. */
static void render_screen(int fb_w, int fb_h, int ox, int oy, int w, int h, float lerp_t) {
    /* The lobby is drawn on black, to be read; over a match, on the match. */
    if (opt.netplay) lobby_update(SDL_GetTicksNS());
    bool show_game = !(opt.netplay && g_lobby.open && !netplay_state_running(g_lobby.st.state));
    if (show_game && opt.render_scale > 0)
        game_render_draw_target(g_rt.texture, opt.linear, ox, oy, w, h);
    else if (show_game)
        game_frame_draw(&state.video, &state.geo3d, &state.bus, &state.romset, (game_rect_t){ox, oy, w, h}, lerp_t);
    if (show_game) {
        game_rect_t board = game_render_tile_rect((game_rect_t){ ox, oy, w, h }, false);
        scripts_draw(fb_w, fb_h, board.x, board.y, board.w, board.h);
    }
    gun_place(fb_w, fb_h, ox, oy, show_game ? w : 0, h);
    if (opt.osd) {
        osd_update(SDL_GetTicksNS());
        osd_draw(fb_w, fb_h);
    }
    if (opt.netplay) lobby_draw(fb_w, fb_h, SDL_GetTicksNS());
    if (g_sdtx_on) sdtx_draw();
}

/* Draw the board's latest frame, the OSD and the lobby, and present it. */
static void render_frame(void) {
    static int shots_done;
    if (opt.gl_finish) glFinish();   /* the tile compose below starts on an idle GPU */
    Uint64 cpu_start = SDL_GetTicksNS();
    int fb_w, fb_h;
    SDL_GetWindowSizeInPixels(g_window, &fb_w, &fb_h);
    cpu_start += frame_prepare();
    float lerp_t = game_frame_lerp();
    game_target_fit();

    int ox, oy, w, h;
    screen_rect(fb_w, fb_h, &ox, &oy, &w, &h);
    if (opt.render_scale > 0) {
        g_fill_log.n = 0; g_fill_log.uploads = 0; g_fill_log.overflow = false;
        sg_begin_pass(&(sg_pass){
            .action = g_pass_action,
            .attachments = { .colors[0] = g_rt.color_att, .depth_stencil = g_rt.depth_att },
        });
        game_frame_draw(&state.video, &state.geo3d, &state.bus, &state.romset,
                        (game_rect_t){0, 0, g_rt.w, g_rt.h}, lerp_t);
        sg_end_pass();
        if (opt.gl_finish) {
            /* The game pass is the tile layer quads and the 3D fills: its GPU
             * time, kept out of the CPU time. */
            Uint64 waited = gl_finish_ns();
            g_st.game_gpu_ns += waited;
            cpu_start += waited;
        }
        if (opt.verify_fill) verify_fill(g_rt.w, g_rt.h);
    }
    sg_begin_pass(&(sg_pass){
        .action = g_pass_action,
        .swapchain = { .width = fb_w, .height = fb_h, .sample_count = 1,
                       .color_format = SG_PIXELFORMAT_RGBA8,
                       .depth_format = SG_PIXELFORMAT_DEPTH_STENCIL,
                       .gl.framebuffer = 0 },
    });
    render_screen(fb_w, fb_h, ox, oy, w, h, lerp_t);
    int pzone = hprof_enter(HPROF_PRESENT);   /* host_prof.h */
    sg_end_pass();
    sg_commit();
    Uint64 cpu_end = SDL_GetTicksNS();
    g_st.cpu_ns += cpu_end - cpu_start;
    if (opt.stats) stats_add_gl();
    if (opt.gl_finish) {
        glFinish();
        g_st.gpu_ns += SDL_GetTicksNS() - cpu_end;
    }

    while (shots_done < opt.shot_count && g_emu_frames >= opt.shot_frame[shots_done])
        save_png(opt.shot_path[shots_done++], fb_w, fb_h);
    Uint64 swap_start = SDL_GetTicksNS();
    SDL_GL_SwapWindow(g_window);
    g_st.swap_ns += SDL_GetTicksNS() - swap_start;
    hprof_leave(pzone);
    g_st.renders++;
}

/* --max-temp: true once the CPU or GPU zone has reached it (checked every 0.5 s). */
static bool too_hot(Uint64 now) {
    static Uint64 next_check;
    if (opt.max_temp <= 0.0 || now < next_check) return false;
    next_check = now + 500000000ull;
    int hot = read_milli("/sys/class/thermal/thermal_zone0/temp");
    int gpu = read_milli("/sys/class/thermal/thermal_zone1/temp");
    if (gpu > hot) hot = gpu;
    if (hot < (int)(opt.max_temp * 1000.0)) return false;
    printf("m2hle: %.1fC reached --max-temp %.0f, quitting\n", hot / 1000.0, opt.max_temp);
    return true;
}

/* Events and renders until the player quits, the CPU halts, --exit-after or
 * --max-temp. The exit code. */
static int run_loop(bool audio) {
    /* Render at the requested rate; the emu thread keeps its own 60 Hz. */
    const Uint64 period_ns = opt.render_fps > 0.0 ? (Uint64)(1e9 / opt.render_fps) : 0;
    Uint64 deadline = SDL_GetTicksNS();
    g_st.start = deadline;
    g_st.frames0 = g_emu_frames;

    int rc = 0;
    for (bool running = true; running; ) {
        running = poll_events();
        ffb_update();
        { ffb_out_t fo = ffb_poll(); lg4ff_update(&fo, ffb_wheel_haptic()); }
        fanatec_update(&state.bus);
        if (state.cpu.halted) {
            fprintf(stderr, "m2hle: CPU halted at 0x%08X\n", state.cpu.sfr.ip);
            dump_log_tail(12);
            return 1;
        }
        if (opt.exit_after && g_emu_frames >= opt.exit_after) break;
        if (versus_other_gone()) { printf("m2hle: Versus: the other cabinet's window closed\n"); break; }

        if (period_ns) {
            Uint64 now = SDL_GetTicksNS();
            if (now < deadline) SDL_DelayPrecise(deadline - now);
            deadline += period_ns;
            now = SDL_GetTicksNS();
            if (deadline + period_ns < now) deadline = now;   /* fell behind: don't burst */
        }
        render_frame();

        Uint64 now = SDL_GetTicksNS();
        if (too_hot(now)) { rc = 3; running = false; }
        if (opt.stats) stats_report(now, audio);
    }
    return rc;
}

/* After the run loop: the emu thread first, then what it used (the link, the
 * scripts, backup RAM, netplay, sound), the --verify-* reports, then the video
 * and SDL. */
static void shut_down(SDL_AudioStream *audio) {
    emu_thread_shutdown(&state.emu);
    versus_stop();   /* after the emu thread: it reads the link's socket */
    if (g_m2lua.loaded)
        printf("m2hle: script %s: %llu frames%s%s\n", g_m2lua.path, (unsigned long long)g_m2lua.frames,
               g_m2lua.error[0] ? ", last error: " : "", g_m2lua.error);
    m2lua_unload();       /* End(), and the script's ROM patches off */
    backup_ram_flush();   /* the board has stopped: what it holds now is final */
    if (opt.netplay) netplay_shutdown();   /* after the emu thread: it is the only thing that pumps it */
    if (audio) SDL_DestroyAudioStream(audio);   /* after the emu thread: no more ring writes */
    if (opt.verify_fill)
        printf("verify-fill: %llu frames of fills checked, %llu differed, %llu skipped (vertex buffer uploaded twice)\n",
               (unsigned long long)g_vf.frames, (unsigned long long)g_vf.bad_frames, (unsigned long long)g_vf.skipped);
    if (opt.verify_gpu_tiles)
        printf("verify-gpu-tiles: %llu composed frames checked, %llu differed\n",
               (unsigned long long)g_vt.frames, (unsigned long long)g_vt.bad_frames);
    game_target_destroy();
    scripts_images_shutdown();
    if (g_sdtx_on) sdtx_shutdown();
    game_render_shutdown();
    video_shutdown(&state.video);
    sg_shutdown();
    SDL_GL_DestroyContext(g_gl);
    SDL_DestroyWindow(g_window);
    lg4ff_shutdown();
    fanatec_shutdown();
    SDL_Quit();
    romset_free(&state.romset);
    mem_shutdown(&state.bus);
    log_file_close();
}

int main(int argc, char **argv) {
    versus_keep_cmdline(argc, argv);
    if (!parse_args(argc, argv)) {
        fprintf(stderr, "usage: m2hle [--rom] <set.zip> [--render-fps N] [--window WxH] [--stats] [--osd]\n"
                        "             [--log FILE] [--log-level SPEC] [--pad-map LIST] [--shot FRAME:FILE] [--exit-after N]\n"
                        "             [--netplay] [--net-config FILE] [--net-delay N]\n"
                        "             [--versus] [--pad N] [--window-at X,Y]\n"
                        "             [--scripts DIR] [--no-scripts] [--script-option KEY=VALUE]\n");
        return 2;
    }
    if (opt.pad_map && !apply_pad_map(opt.pad_map)) return 2;
    g_sdl_pad_only = opt.pad;

    /* The session log flushes every line to disk and a running game warns
     * about unknown COP commands many times a second: off unless asked for. */
    log_init();
    crash_trace_install();
    log_set_path(opt.log_path ? opt.log_path : "off");
    if (opt.log_levels && !log_set_levels(opt.log_levels))
        fprintf(stderr, "m2hle: --log-level %s: expected LEVEL or CHANNEL=LEVEL, comma separated\n",
                opt.log_levels);

    mem_init(&state.bus, NULL, 0);
    i960_reset(&state.cpu);
    bp_init();
    wp_init();
    if (!load_rom(opt.rom)) { dump_log_tail(12); return 1; }
    scripts_load();

    versus_before_sdl();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC)) {
        fprintf(stderr, "m2hle: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_AudioStream *audio = opt.sound ? sound_start() : NULL;
    if (!gl_window_open()) { SDL_Quit(); return 1; }
    int rc = gfx_start();
    if (rc) return rc;
    game_target_make();

    if (opt.netplay) netplay_start();
    if (!versus_start(argc)) return 1;
    emu_thread_init(&state.emu, &state.cpu, &state.bus);
    emu_run(&state.emu);

    rc = run_loop(audio != NULL);

    shut_down(audio);
    return rc;
}
