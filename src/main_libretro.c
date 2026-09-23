/*
 * main_libretro.c -- m2-hle as a libretro core (-DM2HLE_FRONTEND=libretro).
 *
 * The frontend (RetroArch, or ROCKNIX's RetroArch on the handheld) owns the
 * window, the GL context, the audio device, the controllers and their
 * remapping, the menus and the frame pacing. This file is the board and the
 * glue: one retro_run is one 60 Hz slice, stepped on the frontend's thread with
 * emu_slice_body / emu_slice_finish -- the slice every other host runs, the way
 * the web build (main_web.c) steps it, since there is no emu thread here either.
 *
 * Video is hardware-rendered: RetroArch hands over a GL framebuffer and
 * sokol_gfx draws the game into it through game_frame.h, the same path the
 * handheld frontend uses. GL 4.1 core on a desktop, GLES 3 on ARM
 * (M2HLE_LIBRETRO_GLES). The context is RetroArch's and shared, so sokol's
 * state cache is thrown away at the start of every frame.
 *
 * ONLINE PLAY, two ways -- the core option "Online play" (read at load):
 *
 *   RetroArch  RetroArch hosts (with a password if one is set), lists the
 *              session in its lobby and connects the players; the core runs
 *              lockstep over that connection (net/pkt_lockstep.h, the
 *              netpacket interface). RetroArch's own netplay needs savestates,
 *              which this emulator does not have, so this is the only way it
 *              can carry a session.
 *   RPCN       the desktop's and the website's netplay (net/netplay.h), with
 *              the lobby the PS3 port of the game has: its menus, rebuilt from
 *              the PS3's own layouts and drawn by the core (ui/ps3ui_app.h),
 *              driven with the pad. Plays against the PC and the browser.
 *
 * Either way a session is a cold boot on both machines and lockstep from there.
 *
 * THE PS3'S INTERFACE: with the Console version the core is the PS3 port as a
 * player meets it (ui/ps3ui_shell.h) -- PUSH START over the attract, the main
 * menu, Arcade and Offline Versus with the PS3's rule settings, SELECT for the
 * pause menu, Help & Options with the PS3's button presets, and Online Battle,
 * which is the RPCN lobby. Menus are a 16:9 frame of their own; the pause menu
 * sits over the game. Signing in needs no keyboard: Twitch shows a code, and an
 * RPCN account is typed on the lobby's own on-screen keyboard. The Arcade
 * version is the board as it shipped (coins on SELECT); its RPCN lobby opens at
 * load and with L + R together.
 */

/* net/ first, as in main.c: net_socket.h owns the socket include order. */
#include "net/netplay.h"

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro/libretro.h"

#include "sokol_gfx.h"
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
#include "heat_guard.h"
#include "geo3d.h"
#include "game_render.h"
#include "video_window.h"
#include "game_frame.h"
#include "sound.h"
#include "input.h"
#include "net/pkt_lockstep.h"
#include "sokol_gl.h"
#include "ps3ui_app.h"
#include "ps3ui_gpu.h"
#include "ps3ui_shell.h"

/* registry.h is the single TU that defines g_profiles[] / g_active_profile. */
#include "registry.h"

/* The largest picture the frontend's framebuffer is made for: it allocates this
 * much once, colour and depth. Full screen draws at the output's size, up to it. */
#if defined(M2HLE_LIBRETRO_GLES)
#define LR_MAX_SCALE    4                    /* 1984x1536: a handheld's screen is far smaller */
/* Full screen: a handheld's screen (640x480 on the RG ARC-S) is near the board's
 * own size, so drawing at it is cheap and leaves RetroArch nothing to rescale. */
#define LR_DEFAULT_RES  "fullscreen"
/* The RG ARC-S reached its first throttle point (83 C) in two minutes of attract
 * mode with the sound board on, and restarted itself twice under that load. */
#define LR_DEFAULT_SOUND "disabled"
#define LR_DEFAULT_HEAT  "85"
#else
#define LR_MAX_SCALE    8                    /* 3968x3072: a 4K monitor, filled */
#define LR_DEFAULT_RES  "double"
#define LR_DEFAULT_SOUND "enabled"
#define LR_DEFAULT_HEAT  "off"
#endif
#define LR_ASPECT       ((float)VIDEO_WIDTH / (float)VIDEO_HEIGHT)   /* as main_sdl.c letterboxes */
#define LR_AUDIO_FRAMES 4096
/* How long one retro_run may wait on the other player before it gives the frame
 * back to the frontend (and draws the last picture again). */
#define LR_NET_WAIT_US  10000

static struct {
    memory_bus_t     bus;
    i960_cpu_t       cpu;
    emu_thread_ctx_t emu;
    romset_t         romset;
    video_state_t    video;
    geo3d_state_t    geo3d;
} state;

/* ---- Frontend callbacks ---------------------------------------------------- */

static retro_environment_t        env_cb;
static retro_video_refresh_t      video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t         input_poll_cb;
static retro_input_state_t        input_state_cb;
static retro_log_printf_t         log_cb;

static void lr_log(enum retro_log_level lvl, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (log_cb) log_cb(lvl, "[m2hle] %s\n", buf);
    else        fprintf(stderr, "[m2hle] %s\n", buf);
}

/* A line on screen for a few seconds. */
static void lr_message(const char *msg, unsigned frames) {
    struct retro_message m = { msg, frames };
    if (env_cb) env_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &m);
    lr_log(RETRO_LOG_INFO, "%s", msg);
}

/* A notification that has to stay up (a sign-in code): the frontend's newer
 * message interface where it has one, with a duration in milliseconds. */
static void lr_notify(const char *msg, unsigned ms) {
    unsigned version = 0;
    if (env_cb && env_cb(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION, &version) && version >= 1) {
        struct retro_message_ext m = { msg, ms, 3, RETRO_LOG_INFO, RETRO_MESSAGE_TARGET_ALL,
                                       RETRO_MESSAGE_TYPE_NOTIFICATION, -1 };
        env_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &m);
        lr_log(RETRO_LOG_INFO, "%s", msg);
    } else {
        lr_message(msg, ms * 60 / 1000);
    }
}

/* ---- Core options ------------------------------------------------------------ */

typedef enum { LR_ONLINE_RETROARCH, LR_ONLINE_RPCN } lr_online_t;

static struct {
    int         scale;          /* the game drawn at N x 496x384; 0 = at the output's size */
    bool        sound;          /* the sound board is attached (read at load) */
    lr_online_t online;         /* read at load */
    int         net_delay;      /* frames of input delay a session hosted here uses */
    int         draw_every;     /* 1 = every frame, 2 = every second frame (cooler) */
    int         heat_limit;     /* degrees C above which drawing drops to every second frame; 0 = off */
    const char *profile;        /* a profile id, or NULL for the ROM set's default (read at load) */
} opt = { .scale = 0, .sound = true, .online = LR_ONLINE_RETROARCH, .net_delay = 2, .draw_every = 1 };

/* The heat guard's stages (core/heat_guard.h); lr_heat_tick feeds it once a retro_run. */
static heat_guard_t g_heat;

/* The PS3's interface (ui/ps3ui_shell.h): the Console version has it. */
static bool g_shell_on;

static struct retro_core_option_v2_category option_cats[] = {
    { "video",  "Video",       "The picture." },
    { "online", "Online play", "Netplay: RetroArch's own sessions, or RPCN rooms shared with the PC and the website." },
    { NULL, NULL, NULL },
};

static struct retro_core_option_v2_definition option_defs[] = {
    { "m2hle_resolution", "Internal resolution", NULL,
      "The size the game is drawn at. Native is the board's own 496x384; Full screen draws at the size of the "
      "window or screen, so nothing is scaled afterwards.", NULL, "video",
      { { "native", "Native (496x384)" }, { "double", "Double (992x768)" }, { "triple", "Triple (1488x1152)" },
        { "quadruple", "Quadruple (1984x1536)" }, { "fullscreen", "Full screen" }, { NULL, NULL } },
      LR_DEFAULT_RES },
    { "m2hle_stf_version", "Sonic the Fighters version", NULL,
      "Console: the game as Sega's console release runs it, with Honey, Metal Sonic and Robotnik selectable "
      "(Start on Amy, Sonic or Bean at character select). Arcade: the arcade board as it shipped. Each has its own "
      "online rooms. Takes effect when the game is next loaded.",
      NULL, NULL,
      { { "console", "Console" }, { "arcade", "Arcade" }, { NULL, NULL } },
      "console" },
    { "m2hle_sound", "Sound board", NULL,
      "Run the 68000 + SCSP sound board. Off is silent and cheaper on a handheld. Takes effect when the game is next loaded.",
      NULL, NULL,
      { { "enabled", NULL }, { "disabled", NULL }, { NULL, NULL } },
      LR_DEFAULT_SOUND },
    { "m2hle_draw_rate", "Draw rate", NULL,
      "Draw every frame, or every second one. The board runs at 60 either way; at 30 RetroArch shows each "
      "picture twice, which halves the graphics work and runs cooler on a handheld.", NULL, "video",
      { { "60", "Every frame (60)" }, { "30", "Every second frame (30, cooler)" }, { NULL, NULL } },
      "60" },
    { "m2hle_heat_guard", "Heat guard", NULL,
      "Above this temperature, draw every second frame until the device has cooled 5 degrees below it. "
      "Hot a second time: every second frame from then on. Still hot after that: the sound board is "
      "switched off until the game is next loaded (never during an online match). Reads the kernel's "
      "thermal zones; does nothing where there are none.", NULL, "video",
      { { "off", "Off" }, { "80", "80 C" }, { "85", "85 C" }, { "90", "90 C" }, { NULL, NULL } },
      LR_DEFAULT_HEAT },
    { "m2hle_online", "Online play", NULL,
      "RetroArch: host and join through RetroArch's Netplay menu and lobby (password, player list and all). "
      "RPCN: the same rooms as the PC and the website, in the game's own online lobby, which opens at load and "
      "with L + R together, and is driven with the pad (sign in with Twitch or an RPCN account). Takes effect "
      "when the game is next loaded.",
      NULL, "online",
      { { "retroarch", "RetroArch" }, { "rpcn", "RPCN" }, { NULL, NULL } },
      "retroarch" },
    { "m2hle_net_delay", "Input delay (frames)", NULL,
      "Frames between a button press and the board seeing it in a session this machine hosts. More hides more "
      "network latency. A player who joins uses the host's.",
      NULL, "online",
      { { "1", NULL }, { "2", NULL }, { "3", NULL }, { "4", NULL }, { "5", NULL }, { "6", NULL }, { "8", NULL }, { NULL, NULL } },
      "2" },
    { NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },
};

/* The options are sent once and never change: the RPCN lobby is the core's
 * own screen now, not rows in this table. */
static struct retro_core_options_v2 options_v2 = { option_cats, option_defs };

static const char *lr_var(const char *key) {
    struct retro_variable v = { key, NULL };
    return env_cb && env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) ? v.value : NULL;
}

/* at_load: also the options that only take effect when a game is loaded. */
static void lr_read_options(bool at_load) {
    const char *v;
    if ((v = lr_var("m2hle_resolution"))) {
        opt.scale = !strcmp(v, "native") ? 1 : !strcmp(v, "double") ? 2 : !strcmp(v, "triple") ? 3
                  : !strcmp(v, "quadruple") ? 4 : !strcmp(v, "fullscreen") ? 0 : 2;
        if (opt.scale > LR_MAX_SCALE) opt.scale = LR_MAX_SCALE;
    }
    if ((v = lr_var("m2hle_net_delay"))) {
        int d = atoi(v);
        opt.net_delay = d < 1 ? 1 : d > 8 ? 8 : d;
        g_ps3ui_app.default_delay = opt.net_delay;
    }
    if ((v = lr_var("m2hle_draw_rate")))  opt.draw_every = strcmp(v, "30") ? 1 : 2;
    if ((v = lr_var("m2hle_heat_guard"))) {
        opt.heat_limit = atoi(v);   /* "off" -> 0 */
        heat_guard_set_limit(&g_heat, opt.heat_limit);   /* a new limit starts its stages over */
    }
    if (!at_load) return;
    if ((v = lr_var("m2hle_sound")))  opt.sound  = strcmp(v, "disabled") != 0;
    if ((v = lr_var("m2hle_online"))) opt.online = strcmp(v, "rpcn") ? LR_ONLINE_RETROARCH : LR_ONLINE_RPCN;
    if ((v = lr_var("m2hle_stf_version"))) opt.profile = strcmp(v, "arcade") ? NULL : "sfight";
}

static void lr_set_options(void) {
    unsigned version = 0;
    if (env_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2) {
        /* Its answer says whether the frontend groups options into categories,
         * NOT whether it took them (libretro.h). */
        env_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options_v2);
        return;
    }
    /* An old frontend: the v0 table, first value is the default. */
    static struct retro_variable vars[] = {
        { "m2hle_resolution", "Internal resolution; " LR_DEFAULT_RES "|native|double|triple|quadruple|fullscreen" },
        { "m2hle_stf_version", "Sonic the Fighters version; console|arcade" },
        { "m2hle_sound",      "Sound board; " LR_DEFAULT_SOUND "|enabled|disabled" },
        { "m2hle_draw_rate",  "Draw rate; 60|30" },
        { "m2hle_heat_guard", "Heat guard; " LR_DEFAULT_HEAT "|off|80|85|90" },
        { "m2hle_online",     "Online play; retroarch|rpcn" },
        { "m2hle_net_delay",  "Input delay (frames); 2|1|3|4|5|6|8" },
        { NULL, NULL },
    };
    env_cb(RETRO_ENVIRONMENT_SET_VARIABLES, vars);
}

/* ---- The board ---------------------------------------------------------------- */

static bool g_sound_on;
static bool g_game_loaded;
static bool g_halt_reported;

static void lr_attach_sound(void) {
    sound_reset();
    sound_attach(&state.bus);
    sound_load_rom(state.romset.audiocpu, (uint32_t)state.romset.audiocpu_size);
    if (state.romset.samples && state.romset.samples_size > 0)
        sound_load_samples(state.romset.samples, (uint32_t)state.romset.samples_size);
}

/* A cold boot of the loaded set: what load does once the ROM files are read,
 * and what a netplay barrier repeats on both machines (main_web.c
 * web_install_board). With no slice in progress -- there is one thread. */
static void lr_install_board(void) {
    g_active_profile->install_fn(&state.romset, &state.cpu, &state.bus);
    irqt_reset();
    if (g_sound_on) lr_attach_sound();
    input_reset();
    input_attach(&state.bus);
    emu_board_reset_state();
    g_halt_reported = false;
}

/* A session's board has the sound board attached, whatever the option says: the
 * 68000 answers the i960 (the MIDI UART's status, the driver's replies), so a
 * board without it runs different code from one with it, and every other host
 * attaches it (main.c netplay_reset_board_cb). Both reset paths go through here. */
static void lr_sound_for_netplay(void) {
    if (g_sound_on || !g_active_profile || !g_active_profile->quirks.enable_68k_sound
            || !state.romset.audiocpu || state.romset.audiocpu_size == 0) return;
    g_sound_on = true;
    lr_notify("Sound board on for the online match: the other board runs it too", 5000);
}

static void lr_netplay_reset_cb(void *ctx) {
    (void)ctx;
    if (!g_active_profile || !state.romset.loaded) return;
    lr_sound_for_netplay();
    lr_install_board();
}

/* The pkt_lockstep reset: emu_netplay_pump's, for a session it does not run. The
 * step count is part of the board -- the frame check hashes it. */
static void lr_pkt_reset(void) {
    lr_sound_for_netplay();
    lr_install_board();
    state.emu.total_steps       = 0;
    state.emu.cpu_prev_snapshot = state.emu.cpu_snapshot;
    state.emu.cpu_snapshot      = state.cpu;
    if (state.emu.run_state != EMU_RUNNING) emu_run(&state.emu);
}

static bool lr_load_rom(const char *zip) {
    const char *sep = strrchr(zip, '/'), *bsl = strrchr(zip, '\\');
    if (bsl && (!sep || bsl > sep)) sep = bsl;
    const char *base = sep ? sep + 1 : zip;
    const char *dot = strrchr(base, '.');
    char id[64] = {0};
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    if (n >= sizeof id) n = sizeof id - 1;
    memcpy(id, base, n);

    g_active_profile = profile_for_rom_set(id, profile_by_id(opt.profile));
    if (!g_active_profile) {
        char msg[256];
        int k = snprintf(msg, sizeof msg, "m2hle: no profile for ROM set '%s'. Supported:", id);
        for (size_t i = 0; i < g_profile_count && k > 0 && (size_t)k < sizeof msg; i++)
            k += snprintf(msg + k, sizeof msg - (size_t)k, " %s", profile_rom_set(g_profiles[i]));
        lr_message(msg, 600);
        return false;
    }

    /* MAME clone fall-through: the parent zip sits next to the picked one. */
    char parent[1024] = {0};
    const char *parent_ptr = NULL;
    if (g_active_profile->parent_zip_name) {
        size_t dir_len = sep ? (size_t)(sep - zip + 1) : 0;
        if (dir_len + strlen(g_active_profile->parent_zip_name) < sizeof parent) {
            memcpy(parent, zip, dir_len);
            strcat(parent, g_active_profile->parent_zip_name);
            parent_ptr = parent;
        }
    }
    if (g_active_profile->load_fn(&state.romset, zip, parent_ptr) != 0) {
        char msg[256];
        snprintf(msg, sizeof msg, "m2hle: could not load %s as %s", base, g_active_profile->display_name);
        lr_message(msg, 600);
        return false;
    }
    g_sound_on = opt.sound && g_active_profile->quirks.enable_68k_sound
              && state.romset.audiocpu && state.romset.audiocpu_size > 0;
    lr_install_board();
    return true;
}

/* ---- Input ------------------------------------------------------------------------ */

/* RetroPad -> the cabinet, per player. RetroArch remaps on top of this. */
typedef struct { unsigned id; int p1, p2; } lr_bind_t;
static const lr_bind_t lr_binds[] = {
    { RETRO_DEVICE_ID_JOYPAD_UP,     GAME_INPUT_P1_UP,    GAME_INPUT_P2_UP    },
    { RETRO_DEVICE_ID_JOYPAD_DOWN,   GAME_INPUT_P1_DOWN,  GAME_INPUT_P2_DOWN  },
    { RETRO_DEVICE_ID_JOYPAD_LEFT,   GAME_INPUT_P1_LEFT,  GAME_INPUT_P2_LEFT  },
    { RETRO_DEVICE_ID_JOYPAD_RIGHT,  GAME_INPUT_P1_RIGHT, GAME_INPUT_P2_RIGHT },
    { RETRO_DEVICE_ID_JOYPAD_B,      GAME_INPUT_P1_B1,    GAME_INPUT_P2_B1    },
    { RETRO_DEVICE_ID_JOYPAD_A,      GAME_INPUT_P1_B2,    GAME_INPUT_P2_B2    },
    { RETRO_DEVICE_ID_JOYPAD_Y,      GAME_INPUT_P1_B3,    GAME_INPUT_P2_B3    },
    { RETRO_DEVICE_ID_JOYPAD_X,      GAME_INPUT_P1_B4,    GAME_INPUT_P2_B4    },
    { RETRO_DEVICE_ID_JOYPAD_START,  GAME_INPUT_P1_START, GAME_INPUT_P2_START },
    { RETRO_DEVICE_ID_JOYPAD_SELECT, GAME_INPUT_P1_COIN,  GAME_INPUT_P2_COIN  },
    { RETRO_DEVICE_ID_JOYPAD_L3,     GAME_INPUT_TEST,     -1 },
    { RETRO_DEVICE_ID_JOYPAD_R3,     GAME_INPUT_SERVICE,  -1 },
};
#define LR_BIND_COUNT (int)(sizeof lr_binds / sizeof lr_binds[0])

static void lr_set_input_descriptors(void) {
    static const char *names[] = { "Up", "Down", "Left", "Right", "Button 1", "Button 2", "Button 3",
                                   "Button 4", "Start", "Coin", "Test", "Service" };
    struct retro_input_descriptor desc[2 * LR_BIND_COUNT + 3];
    int n = 0;
    for (unsigned port = 0; port < 2; port++)
        for (int i = 0; i < LR_BIND_COUNT; i++) {
            if (port == 1 && lr_binds[i].p2 < 0) continue;
            desc[n++] = (struct retro_input_descriptor){ port, RETRO_DEVICE_JOYPAD, 0, lr_binds[i].id, names[i] };
        }
    if (opt.online == LR_ONLINE_RPCN) {
        desc[n++] = (struct retro_input_descriptor){ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "Online lobby (with R)" };
        desc[n++] = (struct retro_input_descriptor){ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Online lobby (with L)" };
    }
    desc[n] = (struct retro_input_descriptor){ 0, 0, 0, 0, NULL };
    env_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
}

static bool lr_btn(unsigned port, unsigned id) {
    return input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, id) != 0;
}

/* One port's pad as the board's held mask, the d-pad and left stick both steering. */
static uint32_t lr_port_held(unsigned port) {
    const game_input_map_t *in = &g_active_profile->input;
    const int dead = 16000;
    int ax = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
    int ay = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
    uint32_t held = 0;
    for (int i = 0; i < LR_BIND_COUNT; i++) {
        int act = port == 0 ? lr_binds[i].p1 : lr_binds[i].p2;
        if (act < 0) continue;
        bool on = lr_btn(port, lr_binds[i].id);
        if (lr_binds[i].id == RETRO_DEVICE_ID_JOYPAD_UP)    on |= ay < -dead;
        if (lr_binds[i].id == RETRO_DEVICE_ID_JOYPAD_DOWN)  on |= ay >  dead;
        if (lr_binds[i].id == RETRO_DEVICE_ID_JOYPAD_LEFT)  on |= ax < -dead;
        if (lr_binds[i].id == RETRO_DEVICE_ID_JOYPAD_RIGHT) on |= ax >  dead;
        if (on) held |= in->bits[act];
    }
    return held;
}

/* A held mask as netplay.h's canonical word (NP_BIT_*): either side's bits count
 * as this player's, as netplay_sample_local reads them. */
static uint32_t lr_held_to_word(uint32_t held) {
    static const int p1[10] = { GAME_INPUT_P1_UP, GAME_INPUT_P1_DOWN, GAME_INPUT_P1_LEFT, GAME_INPUT_P1_RIGHT,
        GAME_INPUT_P1_B1, GAME_INPUT_P1_B2, GAME_INPUT_P1_B3, GAME_INPUT_P1_B4, GAME_INPUT_P1_START, GAME_INPUT_P1_COIN };
    const game_input_map_t *in = &g_active_profile->input;
    uint32_t word = 0;
    for (int i = 0; i < 10; i++)
        if (in->bits[p1[i]] && (held & in->bits[p1[i]])) word |= 1u << i;
    if (in->bits[GAME_INPUT_SERVICE] && (held & in->bits[GAME_INPUT_SERVICE])) word |= 1u << NP_BIT_SERVICE;
    if (in->bits[GAME_INPUT_TEST]    && (held & in->bits[GAME_INPUT_TEST]))    word |= 1u << NP_BIT_TEST;
    return word;
}

/* Two players' words into the board's held mask: netplay_apply_inputs, whose
 * bit tables are only filled for an RPCN session. */
static void lr_apply_words(uint32_t w0, uint32_t w1) {
    static const int p1[10] = { GAME_INPUT_P1_UP, GAME_INPUT_P1_DOWN, GAME_INPUT_P1_LEFT, GAME_INPUT_P1_RIGHT,
        GAME_INPUT_P1_B1, GAME_INPUT_P1_B2, GAME_INPUT_P1_B3, GAME_INPUT_P1_B4, GAME_INPUT_P1_START, GAME_INPUT_P1_COIN };
    static const int p2[10] = { GAME_INPUT_P2_UP, GAME_INPUT_P2_DOWN, GAME_INPUT_P2_LEFT, GAME_INPUT_P2_RIGHT,
        GAME_INPUT_P2_B1, GAME_INPUT_P2_B2, GAME_INPUT_P2_B3, GAME_INPUT_P2_B4, GAME_INPUT_P2_START, GAME_INPUT_P2_COIN };
    const game_input_map_t *in = &g_active_profile->input;
    uint32_t held = 0;
    for (int i = 0; i < 10; i++) {
        if (w0 & (1u << i)) held |= in->bits[p1[i]];
        if (w1 & (1u << i)) held |= in->bits[p2[i]];
    }
    if (w0 & (1u << NP_BIT_SERVICE)) held |= in->bits[GAME_INPUT_SERVICE];
    if (w0 & (1u << NP_BIT_TEST))    held |= in->bits[GAME_INPUT_TEST];
    g_input.net_held = held;
    g_input.use_net  = 1;
}

/* ---- Online play: RetroArch (netpacket) ------------------------------------------ */

static pkt_lockstep_t          g_pkt;
static retro_netpacket_send_t  g_pkt_send_fn;
static retro_netpacket_poll_receive_t g_pkt_poll_fn;
static uint32_t                g_pkt_note_seen;

static uint64_t lr_now_ms(void) { return (uint64_t)(emu_now_us() / 1000); }

static void lr_pkt_send(const void *buf, size_t len, void *ud) {
    (void)ud;
    /* Reliable and flushed: this is a frame's input, and lockstep waits on it. */
    if (g_pkt_send_fn) g_pkt_send_fn(RETRO_NETPACKET_RELIABLE | RETRO_NETPACKET_FLUSH_HINT, buf, len,
                                     RETRO_NETPACKET_BROADCAST);
}

static void RETRO_CALLCONV lr_np_start(uint16_t client_id, retro_netpacket_send_t send_fn,
                                       retro_netpacket_poll_receive_t poll_receive_fn) {
    g_pkt_send_fn = send_fn;
    g_pkt_poll_fn = poll_receive_fn;
    uint32_t tag = g_active_profile ? netplay_game_tag(g_active_profile) : 0;
    pkt_lockstep_start(&g_pkt, client_id, (uint32_t)opt.net_delay, tag, lr_pkt_send, NULL);
}

static void RETRO_CALLCONV lr_np_receive(const void *buf, size_t len, uint16_t client_id) {
    (void)client_id;
    pkt_lockstep_receive(&g_pkt, buf, len, lr_now_ms());
}

static void RETRO_CALLCONV lr_np_stop(void) {
    pkt_lockstep_stop(&g_pkt);
    g_pkt_send_fn = NULL;
    g_pkt_poll_fn = NULL;
    netplay_release_inputs();
}

static bool RETRO_CALLCONV lr_np_connected(uint16_t client_id) {
    return pkt_lockstep_connected(&g_pkt, client_id);
}

static void RETRO_CALLCONV lr_np_disconnected(uint16_t client_id) {
    pkt_lockstep_disconnected(&g_pkt, client_id);
    if (!pkt_lockstep_playing(&g_pkt)) netplay_release_inputs();
}

/* Only a build of the same version computes the same frames, so it is the
 * protocol: RetroArch refuses a peer whose string differs. */
static struct retro_netpacket_callback lr_netpacket = {
    lr_np_start, lr_np_receive, lr_np_stop, NULL, lr_np_connected, lr_np_disconnected,
    "m2hle2-lockstep-1/" M2HLE_VERSION,
};

/* ---- Online play: RPCN (netplay.h + ui/ps3ui_app.h) ---------------------------------- */

/* Make every directory along `path` that is missing. */
static void lr_mkdirs(const char *path) {
    char buf[480];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; ; p++) {
        if (*p == '/' || *p == '\\' || *p == '\0') {
            char c = *p;
            *p = '\0';
#ifdef _WIN32
            CreateDirectoryA(buf, NULL);   /* fails harmlessly when it exists */
#else
            mkdir(buf, 0700);
#endif
            if (!c) break;
            *p = c;
        }
    }
}

/* The RPCN settings file lives with RetroArch's saves, as the frontend's data:
 * <saves>/m2hle-rpcn.cfg, where <saves> is already this core's own folder when
 * RetroArch sorts saves by core. Where it does not exist yet, the desktop's
 * per-user file (%APPDATA%\m2hle2, ~/.config/m2hle2) is copied in once, so a
 * machine that already signed in there does not have to again.
 *
 * RPCN keeps ONE Twitch token per account: a sign-in here retires the one the
 * desktop emulator holds for the same account, and the other way round
 * (CLAUDE.md, Netplay, "The settings file is per USER"). */
static void lr_rpcn_config_path(void) {
    const char *saves = NULL;
    if (!env_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &saves) || !saves || !saves[0]) return;
    char path[512];
    lr_mkdirs(saves);   /* a fresh RetroArch has not made it yet */
    snprintf(path, sizeof path, "%s/m2hle-rpcn.cfg", saves);
    FILE *have = fopen(path, "rb");
    if (have) {
        fclose(have);
    } else {
        FILE *src = fopen(netplay_cfg_path(), "rb");   /* the per-user file: no path is set yet */
        FILE *dst = src ? fopen(path, "wb") : NULL;
        if (dst) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, src)) > 0) fwrite(buf, 1, n, dst);
            lr_log(RETRO_LOG_INFO, "RPCN: copied the desktop's netplay settings to %s", path);
        }
        if (dst) fclose(dst);
        if (src) fclose(src);
    }
    netplay_set_config_path(path);
    lr_log(RETRO_LOG_INFO, "RPCN: settings in %s", path);
}

/* What the session says while the lobby is not on screen -- a match, the room
 * emptying, a desync -- as the frontend's notifications, and the session's log
 * into the frontend's. Everything else is on the lobby's own screens. */
static void lr_rpcn_report(void) {
    static int last_state = -1;
    static uint32_t log_seen;
    const netplay_status_t *st = &g_ps3ui_app.st;
    if (!g_ps3ui_app.be.get_status) return;   /* no session behind the lobby yet */

    /* The session's own log (sign-in, rooms, barrier, stalls, desync) into
     * RetroArch's, so a netplay problem can be read afterwards. */
    if (st->log_count - log_seen > NETPLAY_LOG_LINES) log_seen = st->log_count - NETPLAY_LOG_LINES;
    for (; log_seen < st->log_count; log_seen++)
        lr_log(RETRO_LOG_INFO, "rpcn: %s", st->log[log_seen % NETPLAY_LOG_LINES]);
    char msg[400];

    if ((int)st->state != last_state) {
        last_state = (int)st->state;
        if (st->state == NETPLAY_PLAYING) {
            /* What the overlay used to put in a corner of the picture. A
             * notification is the frontend's own, so it is in its font and
             * fades the way every other message it shows does. */
            snprintf(msg, sizeof msg, "%dP against %s", st->local_player + 1,
                     st->peer_npid[0] ? st->peer_npid : "?");
            lr_notify(msg, 5000);
        } else if (st->state == NETPLAY_WATCHING) {
            const char *n1 = "?", *n2 = "?";
            for (uint32_t i = 0; i < st->member_count; i++) {
                if (st->members[i].side == 0) n1 = st->members[i].npid;
                if (st->members[i].side == 1) n2 = st->members[i].npid;
            }
            snprintf(msg, sizeof msg, "Watching %s against %s", n1, n2);
            lr_notify(msg, 5000);
        }
    }
    /* The room emptied with the board still in its VS mode
     * (netplay_empty_room_pump). Said again every few seconds while it holds,
     * short enough each time that it is gone soon after the restart. */
    static int64_t empty_said_us;
    if (st->empty_room && (!empty_said_us || emu_now_us() - empty_said_us > 4000000)) {
        lr_notify("There are no other players in the lobby. Press any button to restart the game.", 5000);
        empty_said_us = emu_now_us();
    } else if (!st->empty_room) {
        empty_said_us = 0;
    }
    /* A desync is the one thing the old overlay kept on screen for good; say it
     * once, and the status row goes on saying the match is finished. */
    static uint32_t desync_said = LOCKSTEP_NO_CHECK;
    bool in_session = st->state >= NETPLAY_IN_ROOM && st->state != NETPLAY_FAILED;
    if (in_session && st->desync_frame != LOCKSTEP_NO_CHECK && st->desync_frame != desync_said) {
        desync_said = st->desync_frame;
        snprintf(msg, sizeof msg, "The two boards no longer agree (frame %u) - the match is over",
                 st->desync_frame);
        lr_notify(msg, 10000);
    } else if (st->desync_frame == LOCKSTEP_NO_CHECK) {
        desync_said = LOCKSTEP_NO_CHECK;
    }
}

/* M2HLE_RPCN_AUTOJOIN=<owner> (or 1 for any open room): once signed in, join
 * that player's room and challenge them -- press Start, which the host sees as
 * a challenge to accept. What the lobby's own buttons do, for driving a session
 * from a shell (ssh) while somebody else holds the pad. Unset, nothing happens. */
static void lr_rpcn_autojoin(void) {
    static bool read, joined, started;
    static const char *want;
    static int64_t next_search_us, joined_us;
    static uint32_t rooms_logged = 0xFFFFFFFFu;
    if (!read) {
        read = true;
        want = getenv("M2HLE_RPCN_AUTOJOIN");
        if (want && !want[0]) want = NULL;
        if (want) lr_log(RETRO_LOG_INFO, "rpcn autojoin: looking for %s", strcmp(want, "1") ? want : "any open room");
    }
    if (!want) return;
    const netplay_status_t *st = &g_ps3ui_app.st;
    netplay_config_t c = g_ps3ui_app.cfg;
    int64_t now = emu_now_us();

    if (st->state == NETPLAY_ONLINE && !st->search_pending && st->room_count != rooms_logged) {
        rooms_logged = st->room_count;
        char line[512];
        int n = snprintf(line, sizeof line, "rpcn autojoin: %u room(s):", (unsigned)st->room_count);
        for (uint32_t i = 0; i < st->room_count && n > 0 && (size_t)n < sizeof line; i++)
            n += snprintf(line + n, sizeof line - (size_t)n, " %.16s(%u/%u%s%s)", st->rooms[i].owner,
                          st->rooms[i].cur_members, st->rooms[i].max_slots,
                          st->rooms[i].has_password ? ",locked" : "",
                          netplay_room_reject_reason(st->rooms[i].flag_attr, g_active_profile) ? ",other version" : "");
        lr_log(RETRO_LOG_INFO, "%s", line);
    }
    /* A join that did not take (the room went away, or filled) is tried again. */
    if (joined && !started && st->state == NETPLAY_ONLINE && now - joined_us > 10000000) joined = false;

    if (st->state == NETPLAY_ONLINE && !joined) {
        for (uint32_t i = 0; i < st->room_count; i++) {
            const rpcn_room_listing_t *r = &st->rooms[i];
            if (strcmp(want, "1") && strcmp(r->owner, want)) continue;
            if (r->has_password || r->cur_members >= r->max_slots) continue;
            if (netplay_room_reject_reason(r->flag_attr, g_active_profile)) continue;
            c.room_id = r->room_id;
            netplay_post(NETPLAY_CMD_JOIN, &c);
            joined = true;
            joined_us = now;
            lr_log(RETRO_LOG_INFO, "rpcn autojoin: joining %.16s's room %llu", r->owner, (unsigned long long)r->room_id);
            return;
        }
        if (!st->search_pending && now >= next_search_us) {
            netplay_post(NETPLAY_CMD_SEARCH, &c);
            next_search_us = now + 3000000;
        }
    }
    if (st->state == NETPLAY_IN_ROOM && st->peer_known && !started) {
        netplay_post(NETPLAY_CMD_START, &c);
        started = true;
        lr_log(RETRO_LOG_INFO, "rpcn autojoin: in the room with %s - ready (Start)", st->peer_npid);
    }
}

static void lr_rpcn_init(void) {
    lr_rpcn_config_path();   /* before netplay_init reads the file */
    ps3ui_app_netplay(&g_ps3ui_app, opt.net_delay);
    netplay_set_reset_hook(lr_netplay_reset_cb, NULL);
    /* The Console version reaches it through the shell's Online Battle; the
     * Arcade version has no shell, so its lobby is the first thing on screen. */
    if (!g_shell_on) ps3ui_app_open(&g_ps3ui_app);
}

/* The RetroPad as the lobby's pad: B (bottom) is cross, the confirm button on a
 * Western PS3, A (right) circle, Y (left) square, X (top) triangle. */
static uint32_t lr_lobby_pad_port(unsigned port) {
    static const struct { unsigned id; uint32_t bit; } map[] = {
        { RETRO_DEVICE_ID_JOYPAD_UP, PS3UI_PAD_UP },       { RETRO_DEVICE_ID_JOYPAD_DOWN, PS3UI_PAD_DOWN },
        { RETRO_DEVICE_ID_JOYPAD_LEFT, PS3UI_PAD_LEFT },   { RETRO_DEVICE_ID_JOYPAD_RIGHT, PS3UI_PAD_RIGHT },
        { RETRO_DEVICE_ID_JOYPAD_B, PS3UI_PAD_CROSS },     { RETRO_DEVICE_ID_JOYPAD_A, PS3UI_PAD_CIRCLE },
        { RETRO_DEVICE_ID_JOYPAD_Y, PS3UI_PAD_SQUARE },    { RETRO_DEVICE_ID_JOYPAD_X, PS3UI_PAD_TRIANGLE },
        { RETRO_DEVICE_ID_JOYPAD_START, PS3UI_PAD_START }, { RETRO_DEVICE_ID_JOYPAD_SELECT, PS3UI_PAD_SELECT },
        { RETRO_DEVICE_ID_JOYPAD_L, PS3UI_PAD_L1 },        { RETRO_DEVICE_ID_JOYPAD_R, PS3UI_PAD_R1 },
    };
    const int dead = 16000;
    int ax = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
    int ay = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
    uint32_t pad = 0;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (lr_btn(port, map[i].id)) pad |= map[i].bit;
    if (ay < -dead) pad |= PS3UI_PAD_UP;
    if (ay >  dead) pad |= PS3UI_PAD_DOWN;
    if (ax < -dead) pad |= PS3UI_PAD_LEFT;
    if (ax >  dead) pad |= PS3UI_PAD_RIGHT;
    return pad;
}

static uint32_t lr_lobby_pad(void) { return lr_lobby_pad_port(0); }

/* ---- The PS3 shell (Console version) -------------------------------------------- */

static float g_volume = 1.0f;  /* the shell's Settings: music and effects, one board output */

static void lr_shell_reset(void *u) {
    (void)u;
    lr_install_board();
    if (state.emu.run_state != EMU_RUNNING) emu_run(&state.emu);
}

static void lr_shell_apply(void *u, const uint8_t v[8], int versus) {
    (void)u;
    sfight_apply_menu_settings(&state.bus, v, versus);
    lr_log(RETRO_LOG_INFO, "%s: difficulty %u, rounds %u, time %u, attack %u, barriers %u, type %u",
           versus ? "Offline Versus" : "Arcade", v[0], v[4], v[3], v[5], v[6], v[7]);
}

static void lr_shell_volume(void *u, int music, int se) {
    (void)u;
    g_volume = (float)(music + se) / 10.0f;   /* 5 and 5, the PS3's defaults, are the board as it is */
}

/* A RetroPad port as the board's buttons, the way the shell's Controls screen
 * sets them: the PS3 names the face buttons by shape -- top triangle, right
 * circle, left square, bottom cross -- and gives each, and L1 L2 R1 R2, a
 * punch, kick or guard (or a combination). SELECT is the pause menu's. */
static uint32_t lr_port_held_shell(unsigned port) {
    const game_input_map_t *in = &g_active_profile->input;
    static const unsigned keys[PS3UI_KEYS] = {
        RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_B,
        RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_L2, RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_R2,
    };
    static const uint8_t pkg[PS3UI_BTN_CODES] = { 0, 1, 2, 4, 1 | 4, 1 | 2, 2 | 4, 1 | 2 | 4 };
    const int p = port == 0 ? GAME_INPUT_P1_B1 : GAME_INPUT_P2_B1;   /* B1 punch, B2 kick, B3 guard */
    const uint8_t *code = ps3ui_shell_buttons(&g_ps3ui_shell);
    uint32_t held = lr_port_held(port);
    /* the face buttons and shoulders are the Controls screen's, not lr_binds' */
    for (int b = 0; b < 3; b++) held &= ~in->bits[p + b];
    held &= ~in->bits[port == 0 ? GAME_INPUT_P1_B4 : GAME_INPUT_P2_B4];
    held &= ~in->bits[port == 0 ? GAME_INPUT_P1_COIN : GAME_INPUT_P2_COIN];
    for (int k = 0; k < PS3UI_KEYS; k++) {
        if (!lr_btn(port, keys[k])) continue;
        for (int b = 0; b < 3; b++)
            if (pkg[code[k]] & (1 << b)) held |= in->bits[p + b];
    }
    return held;
}

/* The shell's frame; true when the game gets the pad. *board_paused: the
 * shell holds the board still (its offline menus). */
static bool lr_shell_input(bool *board_paused) {
    bool session = pkt_lockstep_playing(&g_pkt) || netplay_active();
    ps3ui_shell_frame(&g_ps3ui_shell, lr_lobby_pad(), lr_lobby_pad_port(1), session);
    /* Online Battle in RetroArch's own netplay mode: say where it lives */
    if (g_ps3ui_shell.scr == PS3UI_SH_ONLINE && opt.online != LR_ONLINE_RPCN) {
        ps3ui_shell_go(&g_ps3ui_shell, PS3UI_SH_MAIN);
        g_ps3ui_shell.dlg_kind = PS3UI_SHDLG_NONE;
        ps3ui_dialog_ask(&g_ps3ui_shell.dlg, "Online play is set to RetroArch's netplay. Host or join from "
                         "RetroArch's Netplay menu, or set Online play to RPCN in the core's options.", 0);
    }
    *board_paused = ps3ui_shell_board_paused(&g_ps3ui_shell);
    return ps3ui_shell_game_pad(&g_ps3ui_shell);
}

/* Once a retro_run: L + R together opens the lobby (a new press, so holding
 * them does not open it again after it closes), and while it is open it has
 * the pad. True when it took the pad from the game this frame. */
static bool lr_lobby_input(void) {
    static bool lr_was;
    ps3ui_app_t *a = &g_ps3ui_app;
    bool lr = lr_btn(0, RETRO_DEVICE_ID_JOYPAD_L) && lr_btn(0, RETRO_DEVICE_ID_JOYPAD_R);
    if (lr && !lr_was && !a->open) ps3ui_app_open(a);
    lr_was = lr;
    uint32_t pad = lr_lobby_pad();
    bool had = ps3ui_app_visible(a);
    ps3ui_app_frame(a, had ? pad : 0);
    return had || ps3ui_app_visible(a);
}

/* ---- Video ---------------------------------------------------------------------------- */

static struct retro_hw_render_callback hw_render;
static bool g_gfx_ready;

/* ---- Handing the context back ------------------------------------------------------------
 *
 * The GL context is RetroArch's, and after retro_run it draws our framebuffer to
 * the screen with whatever state we left. sokol leaves plenty: GL_SCISSOR_TEST
 * is on for good (sokol enables it at setup) with the rectangle of our last
 * pass, sampler objects stay bound to the texture units, and depth, blend and
 * the colour mask are whatever the last pipeline wanted. On the ARC-S that was
 * a black screen in play (RetroArch's "gl" driver on GLES samples our frame
 * through our sampler object) and, at native size, a picture clipped to the
 * 496x384 scissor in the corner of the 640x480 screen. The menu drew fine,
 * because it sets its own state. So the state goes back to GL's defaults after
 * every frame. The entry points come from the frontend: on Windows sokol keeps
 * its loaded ones to itself. */
#if defined(_WIN32)
#define LR_GLAPI __stdcall
#else
#define LR_GLAPI
#endif
static struct {
    void (LR_GLAPI *Disable)(unsigned cap);
    void (LR_GLAPI *ColorMask)(unsigned char r, unsigned char g, unsigned char b, unsigned char a);
    void (LR_GLAPI *DepthMask)(unsigned char flag);
    void (LR_GLAPI *StencilMask)(unsigned mask);
    void (LR_GLAPI *BindSampler)(unsigned unit, unsigned sampler);
    void (LR_GLAPI *ActiveTexture)(unsigned texture);
    void (LR_GLAPI *BindTexture)(unsigned target, unsigned texture);
    void (LR_GLAPI *BindVertexArray)(unsigned array);
    void (LR_GLAPI *BindBuffer)(unsigned target, unsigned buffer);
    void (LR_GLAPI *UseProgram)(unsigned program);
    void (LR_GLAPI *PixelStorei)(unsigned pname, int param);
} lr_gl;

static void lr_gl_load(void) {
    retro_hw_get_proc_address_t get = hw_render.get_proc_address;
    memset(&lr_gl, 0, sizeof lr_gl);
    if (!get) return;
    #define LR_GL(name) *(retro_proc_address_t *)&lr_gl.name = get("gl" #name)
    LR_GL(Disable); LR_GL(ColorMask); LR_GL(DepthMask); LR_GL(StencilMask); LR_GL(BindSampler);
    LR_GL(ActiveTexture); LR_GL(BindTexture); LR_GL(BindVertexArray); LR_GL(BindBuffer);
    LR_GL(UseProgram); LR_GL(PixelStorei);
    #undef LR_GL
}

static void lr_gl_restore(void) {
    if (!lr_gl.Disable) return;
    lr_gl.Disable(0x0C11);   /* GL_SCISSOR_TEST */
    lr_gl.Disable(0x0B71);   /* GL_DEPTH_TEST */
    lr_gl.Disable(0x0B90);   /* GL_STENCIL_TEST */
    lr_gl.Disable(0x0BE2);   /* GL_BLEND */
    lr_gl.Disable(0x0B44);   /* GL_CULL_FACE */
    lr_gl.Disable(0x8037);   /* GL_POLYGON_OFFSET_FILL */
    if (lr_gl.ColorMask)   lr_gl.ColorMask(1, 1, 1, 1);
    if (lr_gl.DepthMask)   lr_gl.DepthMask(1);
    if (lr_gl.StencilMask) lr_gl.StencilMask(0xFFFFFFFFu);
    for (unsigned unit = 0; unit < SG_MAX_VIEW_BINDSLOTS; unit++) {
        if (lr_gl.BindSampler) lr_gl.BindSampler(unit, 0);
        if (lr_gl.ActiveTexture && lr_gl.BindTexture) {
            lr_gl.ActiveTexture(0x84C0 + unit);   /* GL_TEXTURE0 + unit */
            lr_gl.BindTexture(0x0DE1, 0);         /* GL_TEXTURE_2D */
        }
    }
    if (lr_gl.ActiveTexture)   lr_gl.ActiveTexture(0x84C0);
    if (lr_gl.BindVertexArray) lr_gl.BindVertexArray(0);
    if (lr_gl.BindBuffer) {
        lr_gl.BindBuffer(0x8892, 0);   /* GL_ARRAY_BUFFER */
        lr_gl.BindBuffer(0x8893, 0);   /* GL_ELEMENT_ARRAY_BUFFER */
    }
    if (lr_gl.UseProgram)  lr_gl.UseProgram(0);
    if (lr_gl.PixelStorei) {
        lr_gl.PixelStorei(0x0CF5, 4);  /* GL_UNPACK_ALIGNMENT */
        lr_gl.PixelStorei(0x0D05, 4);  /* GL_PACK_ALIGNMENT */
    }
}

static void lr_context_reset(void) {
    lr_gl_load();
#if !defined(M2HLE_LIBRETRO_GLES)
    /* sokol's Windows GL loader resolves the entry points itself (opengl32 +
     * wglGetProcAddress) from whatever context is current: RetroArch's, now. */
#endif
    sg_setup(&(sg_desc){
        .environment.defaults = { .color_format = SG_PIXELFORMAT_RGBA8,
                                  .depth_format = SG_PIXELFORMAT_DEPTH_STENCIL,
                                  .sample_count = 1 },
        .logger.func = slog_func,
    });
    if (!sg_isvalid()) { lr_log(RETRO_LOG_ERROR, "sokol_gfx setup failed"); return; }
    game_render_init();
    video_init(&state.video);
    sgl_setup(&(sgl_desc_t){ .logger.func = slog_func });
    ps3ui_gpu_setup();
    g_gfx_ready = true;
    lr_log(RETRO_LOG_INFO, "GL context ready; tile layers composed on the %s", state.video.gpu ? "GPU" : "CPU");
}

static void lr_context_destroy(void) {
    if (!g_gfx_ready) return;
    g_gfx_ready = false;
    ps3ui_gpu_shutdown();
    sgl_shutdown();
    game_render_shutdown();
    video_shutdown(&state.video);
    sg_shutdown();
}

static bool lr_init_hw_render(void) {
    memset(&hw_render, 0, sizeof hw_render);
#if defined(M2HLE_LIBRETRO_GLES)
    hw_render.context_type  = RETRO_HW_CONTEXT_OPENGLES3;
    hw_render.version_major = 3;
    hw_render.version_minor = 0;
#else
    hw_render.context_type  = RETRO_HW_CONTEXT_OPENGL_CORE;
    hw_render.version_major = 4;   /* the shaders are GLSL 4.10 (game_render.h) */
    hw_render.version_minor = 1;
#endif
    hw_render.context_reset      = lr_context_reset;
    hw_render.context_destroy    = lr_context_destroy;
    hw_render.depth              = true;
    hw_render.stencil            = true;
    hw_render.bottom_left_origin = true;   /* drawn as to a GL window's framebuffer */
    return env_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render);
}

/* ---- The size the game is drawn at ------------------------------------------------ */

/* libretro has no call for the size of the output, so for "Full screen" ask the
 * GL context RetroArch has current during retro_run what it draws into: the
 * window's client area (WGL) or the EGL surface (the handheld's KMS screen,
 * X11 or Wayland through EGL). EGL is found in the process rather than linked,
 * so the core does not gain a library. False where neither answers. */
#if defined(_WIN32)
static bool lr_output_size(int *w, int *h) {
    HDC dc = wglGetCurrentDC();
    HWND wnd = dc ? WindowFromDC(dc) : NULL;
    RECT rc;
    if (!wnd || !GetClientRect(wnd, &rc)) return false;
    *w = rc.right - rc.left;
    *h = rc.bottom - rc.top;
    return *w > 0 && *h > 0;
}
#else
#include <dlfcn.h>
static bool lr_output_size(int *w, int *h) {
    typedef void *(*get_fn)(void);
    typedef void *(*get_surface_fn)(int32_t);
    typedef unsigned (*query_fn)(void *, void *, int32_t, int32_t *);
    static get_fn get_display;
    static get_surface_fn get_surface;
    static query_fn query_surface;
    static bool tried;
    if (!tried) {
        tried = true;
        void *egl = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_NOLOAD);
        if (!egl) egl = dlopen("libEGL.so", RTLD_LAZY | RTLD_NOLOAD);   /* Android's name */
        if (!egl) return false;
        get_display   = (get_fn)dlsym(egl, "eglGetCurrentDisplay");
        get_surface   = (get_surface_fn)dlsym(egl, "eglGetCurrentSurface");
        query_surface = (query_fn)dlsym(egl, "eglQuerySurface");
    }
    if (!get_display || !get_surface || !query_surface) return false;
    const int32_t EGL_DRAW_ = 0x3059, EGL_WIDTH_ = 0x3057, EGL_HEIGHT_ = 0x3056;
    void *dpy = get_display(), *surf = get_surface(EGL_DRAW_);
    int32_t ew = 0, eh = 0;
    if (!dpy || !surf || !query_surface(dpy, surf, EGL_WIDTH_, &ew) || !query_surface(dpy, surf, EGL_HEIGHT_, &eh))
        return false;
    *w = ew;
    *h = eh;
    return ew > 0 && eh > 0;
}
#endif

/* This frame's picture size. Full screen: the largest 496:384 picture the output
 * holds, within what the frontend's framebuffer was made for. */
static void lr_render_size(int *w, int *h) {
    if (opt.scale > 0) { *w = VIDEO_WIDTH * opt.scale; *h = VIDEO_HEIGHT * opt.scale; return; }
    int ow, oh;
    if (!lr_output_size(&ow, &oh)) { *w = VIDEO_WIDTH * 2; *h = VIDEO_HEIGHT * 2; return; }
    if (ow > VIDEO_WIDTH * LR_MAX_SCALE)  ow = VIDEO_WIDTH * LR_MAX_SCALE;
    if (oh > VIDEO_HEIGHT * LR_MAX_SCALE) oh = VIDEO_HEIGHT * LR_MAX_SCALE;
    if ((int64_t)ow * VIDEO_HEIGHT > (int64_t)oh * VIDEO_WIDTH) { *h = oh; *w = oh * VIDEO_WIDTH / VIDEO_HEIGHT; }
    else                                                         { *w = ow; *h = ow * VIDEO_HEIGHT / VIDEO_WIDTH; }
    if (*w < VIDEO_WIDTH || *h < VIDEO_HEIGHT) { *w = VIDEO_WIDTH; *h = VIDEO_HEIGHT; }
}

/* The size last told to the frontend, so a new one (an option, a resized
 * window) is announced once. */
static int g_geom_w, g_geom_h;

/* The frontend can show the last picture again (video_cb with NULL). */
static bool g_can_dupe;

/* ---- Heat ------------------------------------------------------------------------------
 *
 * The hottest of the kernel's thermal zones, read at most every two seconds;
 * -1 where there are none (Windows, a desktop without them). What the guard
 * does with it is core/heat_guard.h: every second frame while hot, for good
 * the second time, and the sound board off if it is still hot at that. */
static int lr_hottest_c(void) {
#if defined(_WIN32)
    return -1;
#else
    static int64_t next_us;
    static int last = -1;
    int64_t now = emu_now_us();
    if (now < next_us) return last;
    next_us = now + 2000000;
    int hot = -1;
    for (int z = 0; z < 8; z++) {
        char path[64];
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", z);
        FILE *f = fopen(path, "r");
        if (!f) break;
        int milli;
        if (fscanf(f, "%d", &milli) == 1 && milli / 1000 > hot) hot = milli / 1000;
        fclose(f);
    }
    last = hot;
    return hot;
#endif
}

/* Whether the sound board may come off the board right now: not while a
 * session is being set up or played, either kind (the RetroArch one from its
 * barrier on, the RPCN one from SYNCING on). A session is a cold boot with the
 * sound board attached on both machines (lr_sound_for_netplay) and lockstep
 * from there, and a board without it runs different i960 code, so detaching it
 * would be a desync. The guard waits for the match to end instead. */
static bool lr_sound_may_go(void) {
    if (g_pkt.state != PKT_OFF && g_pkt.state != PKT_ENDED) return false;
    if (g_netplay.enabled && (g_netplay.state == NETPLAY_SYNCING || netplay_running_match())) return false;
    return true;
}

/* Once a retro_run: the guard's stages on the hottest zone, its messages, and
 * the sound board when it says so and it may go. Drawing every second frame is
 * this host's business alone and touches nothing the other player's board has
 * to agree with, so that part needs no such care. */
static void lr_heat_tick(void) {
    static bool waiting_said;   /* the "after the match" line, once per verdict */
    int c = lr_hottest_c();
    if (c < 0) return;
    char msg[128];
    heat_event_t ev = heat_guard_update(&g_heat, c, emu_now_us());
    switch (ev) {
        case HEAT_HOT:
            snprintf(msg, sizeof msg, "Hot (%d C): drawing every second frame until it cools", c);
            lr_notify(msg, 5000);
            break;
        case HEAT_COOLED:
            lr_notify("Cooled down: drawing every frame again", 3000);
            break;
        case HEAT_STUCK:
            snprintf(msg, sizeof msg, "Hot again (%d C): drawing every second frame from now on", c);
            lr_notify(msg, 5000);
            break;
        case HEAT_COOLED_STUCK:
            snprintf(msg, sizeof msg, "Cooled down (%d C); staying at every second frame", c);
            lr_notify(msg, 3000);
            break;
        default:
            break;
    }
    if (!g_heat.sound_off) { waiting_said = false; return; }
    if (!g_sound_on) {
        /* Nothing left to drop: the option had it off, or an earlier tick did. */
        if (ev == HEAT_HOT_STUCK) {
            snprintf(msg, sizeof msg, "Hot again (%d C)", c);
            lr_notify(msg, 3000);
        }
        return;
    }
    if (!lr_sound_may_go()) {
        if (!waiting_said) {
            waiting_said = true;
            snprintf(msg, sizeof msg, "Still hot (%d C): the sound board goes off when the match is over", c);
            lr_notify(msg, 5000);
        }
        return;
    }
    sound_detach(&state.bus);
    g_sound.out_r = g_sound.out_w;   /* this host is the ring's reader: nothing left to play */
    g_sound_on = false;
    snprintf(msg, sizeof msg, "Still hot (%d C): sound board off until the game is next loaded", c);
    lr_notify(msg, 5000);
    lr_log(RETRO_LOG_WARN, "heat guard: %d C with every second frame drawn; sound board detached", c);
}

/* How many board frames each drawn frame covers, this frame. */
static int lr_draw_every(void) {
    return heat_guard_draw_every(&g_heat) == 2 ? 2 : opt.draw_every;
}

/* The lobby's frame: 16:9, the largest the output holds within the frontend's
 * framebuffer, never below 1280x720 -- the PS3's own frame. */
static void lr_lobby_size(int *w, int *h) {
    int ow = 1280, oh = 720;
    if (!lr_output_size(&ow, &oh)) { ow = 1280; oh = 720; }
    if (ow > VIDEO_WIDTH * LR_MAX_SCALE)  ow = VIDEO_WIDTH * LR_MAX_SCALE;
    if (oh > VIDEO_HEIGHT * LR_MAX_SCALE) oh = VIDEO_HEIGHT * LR_MAX_SCALE;
    if ((int64_t)ow * 9 > (int64_t)oh * 16) { *h = oh; *w = oh * 16 / 9; }
    else                                     { *w = ow; *h = ow * 9 / 16; }
    if (*h < 720) { *w = 1280; *h = 720; }
}

static void lr_draw(bool ran) {
    int w, h;
    ps3ui_view_t view = g_shell_on ? ps3ui_shell_view(&g_ps3ui_shell)
                      : ps3ui_app_visible(&g_ps3ui_app) ? PS3UI_VIEW_FULL : PS3UI_VIEW_GAME;
    bool lobby = view == PS3UI_VIEW_FULL, overlay = view == PS3UI_VIEW_OVERLAY;
    if (lobby) lr_lobby_size(&w, &h);
    else       lr_render_size(&w, &h);
    /* Show the last picture again, drawing nothing: when the board did not move
     * (waiting on the other player), and on the frames the draw rate skips. */
    static unsigned phase;
    bool skip = !lobby && !overlay && (!ran || (++phase % (unsigned)lr_draw_every()) != 0);
    if (skip && g_can_dupe && g_gfx_ready && g_geom_w == w && g_geom_h == h) {
        video_cb(NULL, (unsigned)w, (unsigned)h, 0);
        return;
    }
    if (w != g_geom_w || h != g_geom_h) {
        g_geom_w = w;
        g_geom_h = h;
        struct retro_game_geometry geom = { (unsigned)w, (unsigned)h, VIDEO_WIDTH * LR_MAX_SCALE,
                                            VIDEO_HEIGHT * LR_MAX_SCALE, lobby ? 16.0f / 9.0f : LR_ASPECT };
        env_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom);
    }
    if (!g_gfx_ready) { video_cb(NULL, (unsigned)w, (unsigned)h, 0); return; }
    sg_reset_state_cache();   /* the context is shared: RetroArch drew with it since */
    if (ran && !lobby) game_frame_prepare(&state.video, &state.geo3d, &state.bus, &state.romset, true);
    static ps3ui_canvas_t lobby_cv;
    if (lobby || overlay) {
        ps3ui_gpu_record(&lobby_cv, w, h);
        if (g_shell_on) ps3ui_shell_draw(&g_ps3ui_shell, &lobby_cv);
        else            ps3ui_app_draw(&g_ps3ui_app, &lobby_cv);
    }

    sg_begin_pass(&(sg_pass){
        .action = {
            .colors[0] = { .load_action = SG_LOADACTION_CLEAR, .clear_value = { 0.0f, 0.0f, 0.0f, 1.0f } },
            .depth     = { .load_action = SG_LOADACTION_CLEAR, .clear_value = 1.0f },
        },
        .swapchain = { .width = w, .height = h, .sample_count = 1,
                       .color_format = SG_PIXELFORMAT_RGBA8,
                       .depth_format = SG_PIXELFORMAT_DEPTH_STENCIL,
                       .gl.framebuffer = (uint32_t)hw_render.get_current_framebuffer() },
    });
    if (!lobby)
        game_frame_draw(&state.video, &state.geo3d, &state.bus, &state.romset, 0, 0, w, h, 1.0f);
    if (lobby || overlay) {
        ps3ui_gpu_draw(&lobby_cv);   /* over the game, for the title prompt and the pause menu */
        sgl_draw();
    }
    sg_end_pass();
    sg_commit();
    lr_gl_restore();
    video_cb(RETRO_HW_FRAME_BUFFER_VALID, (unsigned)w, (unsigned)h, 0);
}

/* ---- Audio ------------------------------------------------------------------------------ */

static int16_t g_audio[LR_AUDIO_FRAMES * 2];
static float   g_dc_x[2], g_dc_y[2];

/* Everything the board produced this retro_run, through a DC blocker: the
 * board's output carries a DC offset (CLAUDE.md, "Sound board"), which
 * audio_out.h takes out for the other hosts. */
static void lr_push_audio(void) {
    if (!g_sound_on) {
        /* Silence at the board's rate, so the frontend's audio clock still runs. */
        static int16_t silence[(SOUND_RATE / EMU_SLICES_PER_SEC + 1) * 2];
        audio_batch_cb(silence, SOUND_RATE / EMU_SLICES_PER_SEC);
        return;
    }
    const float R = 0.99715f;   /* ~20 Hz at 44.1 kHz */
    size_t n = 0;
    uint32_t r = g_sound.out_r, w = g_sound.out_w;
    while (r != w && n < LR_AUDIO_FRAMES) {
        for (int c = 0; c < 2; c++) {
            float x = (float)g_sound.out[r * 2 + c];
            float y = x - g_dc_x[c] + R * g_dc_y[c];
            g_dc_x[c] = x;
            g_dc_y[c] = y;
            int v = (int)lrintf(y * g_volume);
            g_audio[n * 2 + c] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
        }
        n++;
        r = (r + 1) & (SOUND_OUT_FRAMES - 1);
    }
    g_sound.out_r = r;
    if (n) audio_batch_cb(g_audio, n);
}

/* ---- Stepping ------------------------------------------------------------------------------ */

/* retro_run calls, and those that gave the frame back without running the
 * board (waiting on the other player): the netplay's cost, as the player sees it. */
static uint32_t g_lr_runs, g_lr_skips;

/* One slice of the board, and what it ended on. */
static void lr_slice(void) {
    emu_slice_body(&state.emu);
    emu_slice_result_t res = emu_slice_finish(&state.emu);
    if (res == EMU_SLICE_FRAME && pkt_lockstep_playing(&g_pkt)) {
        uint32_t check = netplay_frame_check(&state.emu.cpu_snapshot, state.emu.total_steps);
        pkt_lockstep_end_frame(&g_pkt, check);
        /* Both machines log the same line for the same frame, or they have split. */
        if (g_pkt.frame % 600 == 0)
            lr_log(RETRO_LOG_INFO, "netplay frame %u check 0x%08X (P%u, delay %u, %u stalls, %u of %u runs skipped%s)",
                   g_pkt.frame - 1, check, g_pkt.local_player + 1, g_pkt.frame_delay, g_pkt.stalls,
                   g_lr_skips, g_lr_runs,
                   g_pkt.desync_frame != LOCKSTEP_NO_CHECK ? ", DESYNC" : "");
    }
    if (res == EMU_SLICE_STOPPED) {
        if (state.cpu.halted) {
            if (!g_halt_reported) {
                char msg[96];
                snprintf(msg, sizeof msg, "m2hle: the i960 halted at 0x%08X", state.cpu.sfr.ip);
                lr_message(msg, 600);
                g_halt_reported = true;
            }
        } else {
            emu_run(&state.emu);   /* a debug stop (break-on-warn ...) has no debugger to go to here */
        }
    }
}

/* RetroArch session: run the frame lockstep allows, waiting briefly for it. */
static bool lr_run_pkt(uint32_t local_held) {
    int64_t give_up = emu_now_us() + LR_NET_WAIT_US;
    for (;;) {
        uint32_t w0 = 0, w1 = 0;
        pkt_step_t step = pkt_lockstep_begin_frame(&g_pkt, lr_held_to_word(local_held), lr_now_ms(), &w0, &w1);
        switch (step) {
            case PKT_STEP_OFF:
                netplay_release_inputs();
                g_input.held = local_held;
                lr_slice();
                return true;
            case PKT_STEP_RESET:
                lr_pkt_reset();
                continue;
            case PKT_STEP_READY:
                lr_apply_words(w0, w1);
                lr_slice();
                return true;
            case PKT_STEP_WAIT:
                if (emu_now_us() >= give_up) return false;
                if (g_pkt_poll_fn) g_pkt_poll_fn();
                emu_sleep_us(500);
                break;
        }
    }
}

/* RPCN: netplay.h decides, exactly as the emu thread's run loop asks it. */
static bool lr_run_rpcn(uint32_t local_held) {
    g_input.held = local_held;   /* netplay_sample_local reads it */
    int64_t give_up = emu_now_us() + LR_NET_WAIT_US;
    for (;;) {
        netplay_step_t step = emu_netplay_pump(&state.emu);
        if (step == NETPLAY_STEP_RESET) continue;   /* done inside the pump; ask again */
        if (step == NETPLAY_STEP_WAIT) {
            if (emu_now_us() >= give_up) return false;
            emu_sleep_us(500);
            continue;
        }
        lr_slice();
        /* A watcher behind the fighters runs a few extra slices a frame until it
         * has caught up (netplay_catching_up): one retro_run is one slice, and
         * nothing else would ever close the gap. */
        for (int extra = 0; extra < 3 && netplay_catching_up(); extra++) {
            if (emu_netplay_pump(&state.emu) != NETPLAY_STEP_READY) break;
            lr_slice();
        }
        return true;
    }
}

/* ---- libretro API ---------------------------------------------------------------------------- */

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_set_environment(retro_environment_t cb) {
    env_cb = cb;
    struct retro_log_callback logging;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) log_cb = logging.log;
    lr_set_options();
    static const struct retro_controller_description pads[] = {
        { "RetroPad", RETRO_DEVICE_JOYPAD },
    };
    static const struct retro_controller_info ports[] = {
        { pads, 1 }, { pads, 1 }, { NULL, 0 },
    };
    cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void *)ports);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)           { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)             { (void)cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb)                 { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb)               { input_state_cb = cb; }
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }

RETRO_API void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof *info);
    info->library_name     = "m2-hle";
    info->library_version  = M2HLE_VERSION;
    info->valid_extensions = "zip";
    info->need_fullpath    = true;   /* the loader opens the zip, and its parent beside it */
    info->block_extract    = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info) {
    memset(info, 0, sizeof *info);
    /* Before the first frame the output's size is not known: full screen
     * starts from double and lr_draw announces the real size. */
    int scale = opt.scale > 0 ? opt.scale : 2;
    info->geometry.base_width   = (unsigned)(VIDEO_WIDTH * scale);
    info->geometry.base_height  = (unsigned)(VIDEO_HEIGHT * scale);
    info->geometry.max_width    = VIDEO_WIDTH * LR_MAX_SCALE;
    info->geometry.max_height   = VIDEO_HEIGHT * LR_MAX_SCALE;
    info->geometry.aspect_ratio = LR_ASPECT;
    info->timing.fps            = (double)EMU_SLICES_PER_SEC;
    info->timing.sample_rate    = (double)SOUND_RATE;
}

RETRO_API void retro_init(void) {
    /* The session log flushes every line and a running game warns many times a
     * second: kept in memory only. */
    log_init();
    g_log.file_open_attempted = 1;
    mem_init(&state.bus, NULL, 0);
    i960_reset(&state.cpu);
    bp_init();
    wp_init();
    geo3d_init(&state.geo3d);
    g_geo3d_state = &state.geo3d;
}

RETRO_API void retro_deinit(void) {
    mem_shutdown(&state.bus);
}

RETRO_API bool retro_load_game(const struct retro_game_info *game) {
    if (!game || !game->path) return false;
    lr_read_options(true);
    heat_guard_init(&g_heat, opt.heat_limit);   /* a new load starts the guard's stages over */

    if (!env_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &g_can_dupe)) g_can_dupe = false;
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
    if (!lr_init_hw_render()) {
        lr_message("m2hle needs a hardware-rendered video driver (Settings > Drivers > Video: glcore or gl)", 600);
        return false;
    }
    if (!lr_load_rom(game->path)) return false;
    g_shell_on = g_active_profile && !strcmp(g_active_profile->id, "sfight_console");
    if (g_shell_on)
        ps3ui_shell_init(&g_ps3ui_shell, (ps3ui_host_t){ lr_shell_reset, lr_shell_apply, lr_shell_volume, NULL },
                         &g_ps3ui_app);
    g_volume = 1.0f;
    lr_set_input_descriptors();

    if (opt.online == LR_ONLINE_RETROARCH) {
        if (!env_cb(RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE, &lr_netpacket))
            lr_log(RETRO_LOG_WARN, "this frontend has no netpacket interface: no RetroArch netplay");
    } else {
        lr_rpcn_init();
    }

    emu_ctx_init(&state.emu, &state.cpu, &state.bus);
    emu_run(&state.emu);
    g_game_loaded = true;
    lr_log(RETRO_LOG_INFO, "m2-hle %s: %s, sound board %s, online play %s", M2HLE_VERSION,
           g_active_profile->display_name, g_sound_on ? "on" : "off",
           opt.online == LR_ONLINE_RPCN ? "RPCN" : "RetroArch");
    return true;
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num) {
    (void)type; (void)info; (void)num;
    return false;
}

RETRO_API void retro_unload_game(void) {
    if (opt.online == LR_ONLINE_RPCN) netplay_shutdown();
    ps3ui_app_close(&g_ps3ui_app);
    netplay_release_inputs();
    romset_free(&state.romset);
    g_game_loaded = false;
}

RETRO_API void retro_reset(void) {
    /* Inside a session a reset on one machine is a desync by definition. */
    if (pkt_lockstep_playing(&g_pkt) || netplay_active()) return;
    lr_install_board();
    if (state.emu.run_state != EMU_RUNNING) emu_run(&state.emu);
}

RETRO_API void retro_run(void) {
    bool updated = false;
    if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
        lr_read_options(false);   /* a new size is announced by lr_draw */
    }

    input_poll_cb();
    uint32_t held = g_shell_on ? lr_port_held_shell(0) : lr_port_held(0);
    /* Port 2 is the second player on this machine -- not in a session, where
     * each machine is one player. */
    bool in_session = pkt_lockstep_playing(&g_pkt) || netplay_active();
    if (!in_session) held |= g_shell_on ? lr_port_held_shell(1) : lr_port_held(1);
    bool board_paused = false;
    if (g_shell_on && !lr_shell_input(&board_paused)) held = 0;

    bool ran;
    if (board_paused) {
        ran = false;   /* the shell's menus: the board waits, as the PS3 suspends it */
    } else if (opt.online == LR_ONLINE_RPCN) {
        /* The lobby on screen has the pad; the game gets none of it. With the
         * shell, the shell has already run the lobby (Online Battle). */
        if (g_shell_on) ps3ui_app_frame(&g_ps3ui_app, ps3ui_app_visible(&g_ps3ui_app) ? lr_lobby_pad() : 0);
        else if (lr_lobby_input()) held = 0;
        lr_rpcn_report();
        lr_rpcn_autojoin();
        ran = lr_run_rpcn(held);
    } else {
        ran = lr_run_pkt(held);
        if (g_pkt.note_seq != g_pkt_note_seen) {
            g_pkt_note_seen = g_pkt.note_seq;
            lr_message(g_pkt.note, 240);
        }
    }

    g_lr_runs++;
    if (!ran) g_lr_skips++;
    lr_heat_tick();
    lr_push_audio();
    lr_draw(ran);
}

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

/* No savestates: the board's state is spread across headers that were never
 * written to be snapshotted (see pkt_lockstep.h for what netplay does instead). */
RETRO_API size_t retro_serialize_size(void) { return 0; }
RETRO_API bool   retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
RETRO_API bool   retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }

RETRO_API void   retro_cheat_reset(void) {}
RETRO_API void   retro_cheat_set(unsigned index, bool enabled, const char *code) {
    (void)index; (void)enabled; (void)code;
}

RETRO_API void  *retro_get_memory_data(unsigned id) {
    return id == RETRO_MEMORY_SYSTEM_RAM ? state.bus.main_data : NULL;
}
RETRO_API size_t retro_get_memory_size(unsigned id) {
    return id == RETRO_MEMORY_SYSTEM_RAM && state.bus.main_data ? MAIN_DATA_SIZE : 0;
}
