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
 *         [--log FILE] [--pad-map LIST] [--shot FRAME:FILE]... [--exit-after N]
 *
 * --pad-map  comma-separated button=action pairs overriding the defaults, e.g.
 *            "south=b1,east=b2,west=b3". Buttons: south east west north start
 *            back l1 r1 l3 r3 guide; actions: b1 b2 b3 b4 start coin service
 *            test none, or a combo that holds several at once (a "macro"),
 *            e.g. "north=b1+b2,l3=b1+b2+b3".
 * --shot     save a PNG of the first rendered frame at or after game frame N.
 * --exit-after  quit after N game frames (for scripted checks).
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
 * --steps-per-slice N  i960 steps a 60 Hz slice may run without reaching a
 *            frame edge (default 500000). Lower keeps game loads, which run
 *            ~1M steps a frame, inside the slice on a slow CPU.
 * --live-timers  count the board timers down by each i960 instruction's cycles
 *            and take their interrupts mid-slice (irq_timer.h g_irqt_live), so
 *            a game's own time budget can end its work: STF's texture loads
 *            then yield where the board's do.
 * --fill-shade-rows  give a textured face one row of finished colours (its luma
 *            band, poly_luma and colour together), so a pixel fetches its colour
 *            once instead of a lumaram texel and then a ramp texel.
 * --fill-gather  read the bilinear 2x2 with one textureGather where the taps are
 *            the atlas's own (needs a GLES 3.1 context; falls back if there is
 *            none). The same texels, a quarter of the texture operations.
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
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL3/SDL.h>
#include <GLES3/gl3.h>

#include "sokol_gfx.h"
#include "sokol_log.h"
#include "sokol_debugtext.h"

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
#include "input.h"
#include "audio_out.h"   /* audio_out_cb: drains g_sound's ring (only its callback is used here) */
#include "miniz.h"

/* registry.h is the single TU that defines g_profiles[] / g_profile_count /
 * g_active_profile and pulls in every per-game profile header. */
#include "registry.h"

#define MAX_SHOTS 16

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
    double      render_fps;
    int         win_w, win_h;       /* 0 = fullscreen */
    bool        stats;
    bool        osd;
    bool        sound;
    const char *log_path;
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
} opt = { .render_fps = 30.0, .render_scale = 1, .linear = true };

static bool g_sound_on;             /* the sound board is attached (sound_start succeeded) */

/* ---- Gamepad ------------------------------------------------------------- */

/* Button → action. Directions always come from the d-pad and the left stick. */
typedef struct { const char *name; SDL_GamepadButton button; uint32_t acts; } pad_bind_t;

/* acts is one bit per GAME_INPUT_* action; more than one is a combo. */
#define ACT(a) (1u << (a))
static pad_bind_t g_pad_binds[] = {
    { "south", SDL_GAMEPAD_BUTTON_SOUTH,          ACT(GAME_INPUT_P1_B1)    },
    { "east",  SDL_GAMEPAD_BUTTON_EAST,           ACT(GAME_INPUT_P1_B2)    },
    { "west",  SDL_GAMEPAD_BUTTON_WEST,           ACT(GAME_INPUT_P1_B3)    },
    { "north", SDL_GAMEPAD_BUTTON_NORTH,          ACT(GAME_INPUT_P1_B4)    },
    { "start", SDL_GAMEPAD_BUTTON_START,          ACT(GAME_INPUT_P1_START) },
    { "back",  SDL_GAMEPAD_BUTTON_BACK,           ACT(GAME_INPUT_P1_COIN)  },
    { "l1",    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  0 },
    { "r1",    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0 },
    { "l3",    SDL_GAMEPAD_BUTTON_LEFT_STICK,     0 },
    { "r3",    SDL_GAMEPAD_BUTTON_RIGHT_STICK,    0 },
    { "guide", SDL_GAMEPAD_BUTTON_GUIDE,          0 },
};
#define PAD_BIND_COUNT (int)(sizeof g_pad_binds / sizeof g_pad_binds[0])

/* An action, or a combo of player-1 actions ("b1+b2"); false if neither. */
static bool actions_by_name(const char *s, uint32_t *acts) {
    if (!strcmp(s, "none"))    { *acts = 0; return true; }
    if (!strcmp(s, "service")) { *acts = ACT(GAME_INPUT_SERVICE); return true; }
    if (!strcmp(s, "test"))    { *acts = ACT(GAME_INPUT_TEST); return true; }
    *acts = input_combo_parse(s);
    return *acts && !(*acts >> GAME_INPUT_P2_UP);   /* the pad plays player 1 */
}

static bool apply_pad_map(const char *list) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", list);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        char *eq = strchr(tok, '=');
        if (!eq) { fprintf(stderr, "--pad-map: '%s' is not button=action\n", tok); return false; }
        *eq = '\0';
        uint32_t acts;
        bool ok = actions_by_name(eq + 1, &acts);
        int b;
        for (b = 0; b < PAD_BIND_COUNT && strcmp(g_pad_binds[b].name, tok); b++) {}
        if (b == PAD_BIND_COUNT || !ok) {
            fprintf(stderr, "--pad-map: unknown button or action in '%s=%s'\n", tok, eq + 1);
            return false;
        }
        g_pad_binds[b].acts = acts;
    }
    return true;
}

/* The pad's contribution to each action, recomputed whole on every gamepad
 * event so the d-pad and stick cannot release each other's direction. */
static bool g_pad_held[GAME_INPUT_COUNT];

static void pad_refresh(void) {
    bool now[GAME_INPUT_COUNT] = {0};
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; i++) {
        SDL_Gamepad *pad = SDL_GetGamepadFromID(ids[i]);
        if (!pad) continue;
        const int dead = 16000;
        int ax = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        int ay = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        now[GAME_INPUT_P1_UP]    |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP)    || ay < -dead;
        now[GAME_INPUT_P1_DOWN]  |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)  || ay >  dead;
        now[GAME_INPUT_P1_LEFT]  |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)  || ax < -dead;
        now[GAME_INPUT_P1_RIGHT] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || ax >  dead;
        for (int b = 0; b < PAD_BIND_COUNT; b++) {
            if (!g_pad_binds[b].acts || !SDL_GetGamepadButton(pad, g_pad_binds[b].button)) continue;
            for (int a = 0; a < GAME_INPUT_COUNT; a++)
                if (g_pad_binds[b].acts & ACT(a)) now[a] = true;
        }
    }
    SDL_free(ids);
    for (int a = 0; a < GAME_INPUT_COUNT; a++) {
        if (now[a] && !g_pad_held[a]) input_action_down(a);
        if (!now[a] && g_pad_held[a]) input_action_up(a);
        g_pad_held[a] = now[a];
    }
}

/* Let go of everything the pad holds, as the game sees it: the netplay lobby
 * takes the pad, and a button held into it must not stay held in the game. */
static void lobby_pad_release(void) {
    for (int a = 0; a < GAME_INPUT_COUNT; a++) {
        if (g_pad_held[a]) input_action_up(a);
        g_pad_held[a] = false;
    }
}

/* Keyboard, for desktop runs: the debugger's layout (input.h). */
static int key_to_action(SDL_Scancode sc) {
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
        case SDL_SCANCODE_F2:    return GAME_INPUT_SERVICE;
        case SDL_SCANCODE_F3:    return GAME_INPUT_TEST;
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
    const char *sep = strrchr(zip, '/');
    const char *base = sep ? sep + 1 : zip;
    const char *dot = strrchr(base, '.');
    char id[64] = {0};
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    if (n >= sizeof id) n = sizeof id - 1;
    memcpy(id, base, n);

    g_active_profile = NULL;
    for (size_t i = 0; i < g_profile_count; i++)
        if (!strcmp(g_profiles[i]->id, id)) g_active_profile = g_profiles[i];
    if (!g_active_profile) {
        fprintf(stderr, "m2hle: no profile for ROM set '%s'. Supported:", id);
        for (size_t i = 0; i < g_profile_count; i++) fprintf(stderr, " %s", g_profiles[i]->id);
        fprintf(stderr, "\n");
        return false;
    }

    /* MAME clone fall-through: the parent zip sits next to the picked one. A
     * merged set carries the parent's files itself, so a missing parent is fine. */
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
        fprintf(stderr, "m2hle: could not load '%s' as %s\n", zip, g_active_profile->display_name);
        return false;
    }
    g_active_profile->install_fn(&state.romset, &state.cpu, &state.bus);
    input_reset();
    input_attach(&state.bus);
    return true;
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

/* Compare the GPU layers against a CPU compose of the same RAM: the pens the
 * targets hold, turned into colours through the pen texture's texels, and the
 * colours the indexed quads actually put on screen. */
static void verify_gpu_tiles(void) {
    video_compose_cpu(&state.video, &state.bus);
    read_layer(state.video.bg_image, g_vt.bg);
    read_layer(state.video.fg_image, g_vt.fg);
    for (int i = 0; i < VIDEO_WIDTH * VIDEO_HEIGHT; i++)
        for (int k = 0; k < 2; k++) {
            uint8_t *px = k ? &g_vt.fg[i * 4] : &g_vt.bg[i * 4];
            const uint8_t *c = &state.video.pal_texels[(px[0] | (px[1] << 8)) * 4];
            px[0] = c[0]; px[1] = c[1]; px[2] = c[2];
        }
    int shown_bad = 0;
    for (int k = 0; k < 2; k++) {
        show_layer(k ? state.video.fg_view : state.video.bg_view, g_vt.shown);
        for (int i = 0; i < VIDEO_WIDTH * VIDEO_HEIGHT; i++) {
            const uint8_t *s = &g_vt.shown[i * 4];
            const uint8_t *c = k ? &state.video.fg_pixels[i * 4] : &state.video.bg_pixels[i * 4];
            if ((k == 0 || c[3]) && (s[0] != c[0] || s[1] != c[1] || s[2] != c[2])) shown_bad++;
        }
    }
    if (shown_bad && g_vt.bad_frames < 5)
        printf("verify-gpu-tiles: game frame %u, %d shown pixels differ from the CPU colours\n", g_emu_frames, shown_bad);
    int bad = shown_bad, first = -1;
    const char *what = "";
    for (int i = 0; i < VIDEO_WIDTH * VIDEO_HEIGHT; i++) {
        const uint8_t *gb = &g_vt.bg[i * 4], *cb = &state.video.bg_pixels[i * 4];
        const uint8_t *gf = &g_vt.fg[i * 4], *cf = &state.video.fg_pixels[i * 4];
        bool bg_ok = gb[0] == cb[0] && gb[1] == cb[1] && gb[2] == cb[2] && gb[3] == cb[3];
        bool fg_ok = gf[3] == cf[3] && (cf[3] == 0 || (gf[0] == cf[0] && gf[1] == cf[1] && gf[2] == cf[2]));
        if (!bg_ok || !fg_ok) {
            if (first < 0) { first = i; what = bg_ok ? "fg" : "bg"; }
            bad++;
        }
    }
    g_vt.frames++;
    if (bad && g_vt.bad_frames++ < 5) {
        int x = first % VIDEO_WIDTH, y = first / VIDEO_WIDTH;
        const uint8_t *g = strcmp(what, "bg") ? &g_vt.fg[first * 4] : &g_vt.bg[first * 4];
        const uint8_t *c = strcmp(what, "bg") ? &state.video.fg_pixels[first * 4] : &state.video.bg_pixels[first * 4];
        printf("verify-gpu-tiles: game frame %u, %d pixels differ; first %s at (%d,%d) gpu %02x%02x%02x%02x cpu %02x%02x%02x%02x\n",
               g_emu_frames, bad, what, x, y, g[0], g[1], g[2], g[3], c[0], c[1], c[2], c[3]);
    }
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

static bool parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool more = i + 1 < argc;
        if      (!strcmp(a, "--rom") && more)        opt.rom = argv[++i];
        else if (!strcmp(a, "--render-fps") && more) opt.render_fps = atof(argv[++i]);
        else if (!strcmp(a, "--steps-per-slice") && more) {
            int n = atoi(argv[++i]);
            if (n < 1000) return false;
            g_emu_steps_per_slice = n;
        }
        else if (!strcmp(a, "--live-timers"))        g_irqt_live = 1;
        else if (!strcmp(a, "--window") && more)     { if (sscanf(argv[++i], "%dx%d", &opt.win_w, &opt.win_h) != 2) return false; }
        else if (!strcmp(a, "--stats"))              opt.stats = true;
        else if (!strcmp(a, "--sound"))              opt.sound = true;
        else if (!strcmp(a, "--osd"))                opt.osd = true;
        else if (!strcmp(a, "--log") && more)        opt.log_path = argv[++i];
        else if (!strcmp(a, "--pad-map") && more)    opt.pad_map = argv[++i];
        else if (!strcmp(a, "--exit-after") && more) opt.exit_after = (unsigned)strtoul(argv[++i], NULL, 0);
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
        else if (!strcmp(a, "--fill-gather"))        g_game_render_fill_gather = 1;
        else if (!strcmp(a, "--match-replay"))       g_match_replay = 1;   /* attract straight to its replay fight */
        else if (!strcmp(a, "--netplay"))            opt.netplay = true;
        else if (!strcmp(a, "--net-config") && more) netplay_set_config_path(argv[++i]);   /* before netplay_init */
        else if (!strcmp(a, "--net-delay") && more)  opt.net_delay = atoi(argv[++i]);
        else if (!strcmp(a, "--net-host"))           opt.net_host = opt.netplay = true;
        else if (!strcmp(a, "--net-room-pass") && more) opt.net_room_pass = argv[++i];
        else if (!strcmp(a, "--filter") && more) {
            const char *f = argv[++i];
            if (!strcmp(f, "linear")) opt.linear = true;
            else if (!strcmp(f, "nearest")) opt.linear = false;
            else return false;
        }
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

#ifndef NETPLAY_DEFAULT_SERVER
#define NETPLAY_DEFAULT_SERVER "rpcn.sonicthefighte.rs"
#endif

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

typedef enum {
    LB_SIGN_IN, LB_TWITCH, LB_TWITCH_CANCEL, LB_CLOSE, LB_DISCONNECT, LB_HOST,
    LB_REFRESH, LB_JOIN, LB_START, LB_LEAVE_ROOM, LB_STOP,
} lobby_act_t;

typedef struct {
    lobby_act_t act;
    uint64_t    room;
    bool        enabled;
    char        text[64];
} lobby_row_t;

#define LOBBY_ROWS (RPCN_MAX_ROOMS + 8)

static struct {
    bool             open;
    int              sel;
    int              pressed;       /* A or B went down in the lobby; it acts on release */
    bool             have_cfg;      /* the settings file named a server or an account */
    netplay_config_t cfg;
    int              last_state;    /* -1 until the first snapshot */
    Uint64           next_search;
    Uint64           match_start;   /* when PLAYING began, for the "vs" banner */
    netplay_status_t st;
    lobby_row_t      rows[LOBBY_ROWS];
    int              nrows;
} g_lobby = { .last_state = -1, .pressed = -1 };

static void lobby_move(int dir);

static void lobby_show(bool open) {
    if (open && !g_lobby.open) lobby_pad_release();   /* the game sees the pad let go */
    g_lobby.open = open;
    g_lobby.sel  = 0;
}

static void lobby_init(void) {
    netplay_init();
    netplay_set_reset_hook(sdl_netplay_reset_cb, NULL);
    netplay_set_open_browser(false);   /* the code and the address are on screen */

    g_lobby.have_cfg = netplay_stored_settings(&g_lobby.cfg);
    netplay_config_t *c = &g_lobby.cfg;
    if (!c->server[0]) snprintf(c->server, sizeof c->server, "%s", NETPLAY_DEFAULT_SERVER);
    if (!c->port)        c->port = RPCN_DEFAULT_PORT;
    if (!c->frame_delay) c->frame_delay = 2;
    if (opt.net_delay > 0) c->frame_delay = (uint32_t)opt.net_delay;
    if (opt.net_room_pass) snprintf(c->room_password, sizeof c->room_password, "%s", opt.net_room_pass);
    lobby_show(true);
}

/* A stored login signs straight in: choosing netplay was the request. */
static bool lobby_can_sign_in(void) {
    return g_lobby.cfg.twitch_token[0] || (g_lobby.cfg.npid[0] && g_lobby.cfg.password[0]);
}

/* Sign in as the stored account: the Twitch login token when there is one (the
 * netplay window's Connect does the same), otherwise the stored password. */
static void lobby_sign_in(void) {
    netplay_config_t c = g_lobby.cfg;
    if (c.twitch_token[0]) {
        c.npid[0] = '\0';   /* the token's owner signs in, whoever else was stored */
        netplay_post(NETPLAY_CMD_TWITCH_START, &c);
    } else {
        netplay_post(NETPLAY_CMD_CONNECT, &c);
    }
}

static void lobby_row(lobby_act_t act, bool enabled, uint64_t room, const char *fmt, ...) {
    if (g_lobby.nrows >= LOBBY_ROWS) return;
    lobby_row_t *r = &g_lobby.rows[g_lobby.nrows++];
    r->act = act;
    r->room = room;
    r->enabled = enabled;
    va_list args;
    va_start(args, fmt);
    vsnprintf(r->text, sizeof r->text, fmt, args);
    va_end(args);
}

/* Once per rendered frame: take a snapshot, react to state changes and rebuild
 * the menu for the state the session is in. */
static void lobby_update(Uint64 now) {
    netplay_get_status(&g_lobby.st);
    const netplay_status_t *st = &g_lobby.st;

    if ((int)st->state != g_lobby.last_state) {
        int was = g_lobby.last_state;
        g_lobby.last_state = (int)st->state;
        g_lobby.sel = 0;
        if (st->state == NETPLAY_PLAYING || st->state == NETPLAY_WATCHING) {
            g_lobby.match_start = now;
            lobby_show(false);
        } else if (was == NETPLAY_PLAYING || was == NETPLAY_WATCHING) {
            lobby_show(true);   /* the match ended: say why, and what next */
        }
        if (st->state == NETPLAY_ONLINE) g_lobby.next_search = 0;   /* list the rooms now */
        if (st->state == NETPLAY_ONLINE && opt.net_host) {
            opt.net_host = false;   /* once: a later sign-in is the player's */
            netplay_post(NETPLAY_CMD_HOST, &g_lobby.cfg);
        }
    }
    if (st->state == NETPLAY_ONLINE && g_lobby.open && !st->search_pending
            && now >= g_lobby.next_search) {
        netplay_post(NETPLAY_CMD_SEARCH, &g_lobby.cfg);
        g_lobby.next_search = now + 5000000000ull;
    }

    g_lobby.nrows = 0;
    const netplay_config_t *c = &g_lobby.cfg;
    bool twitch_busy = st->twitch_state == RPCN_TWITCH_STARTING || st->twitch_state == RPCN_TWITCH_WAITING;
    switch (st->state) {
        case NETPLAY_OFF:
        case NETPLAY_FAILED:
            if (twitch_busy) {
                lobby_row(LB_TWITCH_CANCEL, true, 0, "Cancel the Twitch sign-in");
            } else {
                if (c->twitch_token[0])
                    lobby_row(LB_SIGN_IN, true, 0, "Sign in as %s (Twitch)",
                              c->twitch_npid[0] ? c->twitch_npid : c->npid);
                else if (c->npid[0] && c->password[0])
                    lobby_row(LB_SIGN_IN, true, 0, "Sign in as %s", c->npid);
                if (!c->twitch_token[0])
                    lobby_row(LB_TWITCH, true, 0, "Sign in with Twitch");
            }
            lobby_row(LB_CLOSE, true, 0, "Play offline");
            break;
        case NETPLAY_CONNECTING:
            lobby_row(LB_DISCONNECT, true, 0, "Cancel");
            break;
        case NETPLAY_ONLINE:
            lobby_row(LB_HOST, true, 0, "Host a room for %u (delay %u)",
                      (unsigned)(c->max_players >= 2 ? c->max_players : 2), (unsigned)c->frame_delay);
            for (uint32_t i = 0; i < st->room_count; i++) {
                const rpcn_room_listing_t *r = &st->rooms[i];
                const char *why = netplay_room_reject_reason(r->flag_attr, g_active_profile);
                if (why)                              lobby_row(LB_JOIN, false, 0, "%.16s: other version", r->owner);
                else if (r->has_password)             lobby_row(LB_JOIN, false, 0, "%.16s: locked", r->owner);
                else if (r->cur_members >= r->max_slots) lobby_row(LB_JOIN, false, 0, "%.16s: full", r->owner);
                else lobby_row(LB_JOIN, true, r->room_id, "Join %.16s (delay %u)", r->owner,
                               (unsigned)((r->flag_attr >> NETPLAY_ROOM_DELAY_SHIFT) & NETPLAY_ROOM_DELAY_MASK));
            }
            lobby_row(LB_REFRESH, !st->search_pending, 0, st->search_pending ? "Searching..." : "Refresh the list");
            lobby_row(LB_DISCONNECT, true, 0, "Sign out");
            lobby_row(LB_CLOSE, true, 0, "Back to the game");
            break;
        case NETPLAY_IN_ROOM:
            /* Start is "ready": the owner starts once every player in the room is. */
            if (st->me.flags & ROOM_MEMBER_READY)
                lobby_row(LB_STOP, true, 0, "Not ready");
            else
                lobby_row(LB_START, st->member_count >= 2, 0, st->peer_ready ? "Ready - they are waiting" : "Ready");
            lobby_row(LB_LEAVE_ROOM, true, 0, "Leave the room");
            lobby_row(LB_CLOSE, true, 0, "Back to the game");
            break;
        case NETPLAY_SYNCING:
            lobby_row(LB_STOP, true, 0, "Cancel");
            lobby_row(LB_CLOSE, true, 0, "Back to the game");
            break;
        case NETPLAY_PLAYING:
            lobby_row(LB_CLOSE, true, 0, "Back to the match");
            lobby_row(LB_STOP, true, 0, "Leave the match");
            break;
        case NETPLAY_WATCHING:
            lobby_row(LB_CLOSE, true, 0, "Back to the match");
            lobby_row(LB_STOP, true, 0, "Stop watching");
            break;
    }
    if (g_lobby.sel >= g_lobby.nrows) g_lobby.sel = g_lobby.nrows - 1;
    if (g_lobby.sel < 0) g_lobby.sel = 0;
    /* Never rest on a row that cannot be picked ("Start" before anyone joins). */
    if (g_lobby.nrows && !g_lobby.rows[g_lobby.sel].enabled) lobby_move(+1);
}

static void lobby_move(int dir) {
    for (int k = 1; k <= g_lobby.nrows; k++) {
        int i = ((g_lobby.sel + dir * k) % g_lobby.nrows + g_lobby.nrows) % g_lobby.nrows;
        if (g_lobby.rows[i].enabled) { g_lobby.sel = i; return; }
    }
}

static void lobby_activate(void) {
    if (g_lobby.sel >= g_lobby.nrows || !g_lobby.rows[g_lobby.sel].enabled) return;
    const lobby_row_t *r = &g_lobby.rows[g_lobby.sel];
    netplay_config_t c = g_lobby.cfg;
    switch (r->act) {
        case LB_SIGN_IN:       lobby_sign_in(); break;
        case LB_TWITCH:        netplay_post(NETPLAY_CMD_TWITCH_START, &c); break;
        case LB_TWITCH_CANCEL: netplay_post(NETPLAY_CMD_TWITCH_CANCEL, &c); break;
        case LB_CLOSE:         lobby_show(false); break;
        case LB_DISCONNECT:    netplay_post(NETPLAY_CMD_DISCONNECT, &c); break;
        case LB_HOST:          netplay_post(NETPLAY_CMD_HOST, &c); break;
        case LB_REFRESH:       netplay_post(NETPLAY_CMD_SEARCH, &c); break;
        case LB_JOIN:          c.room_id = r->room; netplay_post(NETPLAY_CMD_JOIN, &c); break;
        case LB_START:         netplay_post(NETPLAY_CMD_START, &c); break;
        case LB_STOP:          netplay_post(NETPLAY_CMD_STOP, &c); break;
        case LB_LEAVE_ROOM:    netplay_post(NETPLAY_CMD_LEAVE_ROOM, &c); break;
    }
}

/* Text at cell (x, y), with the OSD's one-pixel shadow, wrapped at `cols`; with
 * draw false, only measured. Returns the row after the last one. */
static float lobby_text_ex(float x, float y, int cols, uint8_t r, uint8_t g, uint8_t b,
                           const char *s, bool draw) {
    char line[128];
    int width = cols - (int)x - 1;
    if (width < 8) width = 8;
    if (width > (int)sizeof line - 1) width = (int)sizeof line - 1;
    while (*s) {
        int n = (int)strlen(s);
        if (n > width) {
            n = width;
            for (int k = width; k > width / 2; k--) if (s[k] == ' ') { n = k; break; }
        }
        if (draw) {
            memcpy(line, s, (size_t)n);
            line[n] = '\0';
            sdtx_pos(x + 0.125f, y + 0.125f);
            sdtx_color3b(0, 0, 0);
            sdtx_puts(line);
            sdtx_pos(x, y);
            sdtx_color3b(r, g, b);
            sdtx_puts(line);
        }
        s += n;
        while (*s == ' ') s++;
        y += 1.0f;
    }
    return y;
}

static float lobby_text(float x, float y, int cols, uint8_t r, uint8_t g, uint8_t b, const char *s) {
    return lobby_text_ex(x, y, cols, r, g, b, s, true);
}

static void lobby_draw(int fb_w, int fb_h) {
    const netplay_status_t *st = &g_lobby.st;
    float scale = fb_h >= 720 ? 3.0f : 2.0f;
    int   cols  = (int)((float)fb_w / scale / 8.0f);
    char  buf[640];
    sg_apply_viewport(0, 0, fb_w, fb_h, true);
    sg_apply_scissor_rect(0, 0, fb_w, fb_h, true);
    sdtx_canvas((float)fb_w / scale, (float)fb_h / scale);
    sdtx_origin(0.0f, 0.0f);

    if (!g_lobby.open) {
        /* In a match: who against, for a few seconds, and a desync for good. */
        if (st->state == NETPLAY_PLAYING && SDL_GetTicksNS() - g_lobby.match_start < 5000000000ull) {
            snprintf(buf, sizeof buf, "P%d vs %s", st->local_player + 1, st->peer_npid);
            lobby_text(1.0f, 0.25f, cols, 120, 220, 255, buf);
        }
        if (st->state == NETPLAY_WATCHING && SDL_GetTicksNS() - g_lobby.match_start < 5000000000ull) {
            const char *n1 = "?", *n2 = "?";
            for (uint32_t i = 0; i < st->member_count; i++) {
                if (st->members[i].side == 0) n1 = st->members[i].npid;
                if (st->members[i].side == 1) n2 = st->members[i].npid;
            }
            snprintf(buf, sizeof buf, "Watching %s vs %s", n1, n2);
            lobby_text(1.0f, 0.25f, cols, 120, 220, 255, buf);
        }
        if (netplay_state_running(st->state) && st->desync_frame != LOCKSTEP_NO_CHECK) {
            snprintf(buf, sizeof buf, "DESYNC at frame %u", st->desync_frame);
            lobby_text(1.0f, 1.25f, cols, 255, 90, 90, buf);
        }
        return;
    }

    float y = 1.0f;
    snprintf(buf, sizeof buf, "NETPLAY - %s", netplay_state_text(st->state));
    y = lobby_text(1.0f, y, cols, 255, 220, 120, buf);
    if (st->state >= NETPLAY_ONLINE && st->state != NETPLAY_FAILED) {
        snprintf(buf, sizeof buf, "Signed in as %s", g_lobby.cfg.twitch_token[0] && g_lobby.cfg.twitch_npid[0]
                 ? g_lobby.cfg.twitch_npid : g_lobby.cfg.npid);
        y = lobby_text(1.0f, y, cols, 170, 170, 170, buf);
    }
    y += 0.5f;

    if (!g_lobby.have_cfg && st->state == NETPLAY_OFF && st->twitch_state == RPCN_TWITCH_IDLE) {
        snprintf(buf, sizeof buf, "No netplay settings. Copy netplay.cfg from the PC (%%APPDATA%%\\m2hle2) to %s",
                 netplay_cfg_path());
        y = lobby_text(1.0f, y, cols, 255, 255, 255, buf) + 0.5f;
    }
    if (st->twitch_state == RPCN_TWITCH_WAITING) {
        snprintf(buf, sizeof buf, "Twitch code: %s", st->twitch_user_code);
        y = lobby_text(1.0f, y, cols, 255, 255, 140, buf);
        snprintf(buf, sizeof buf, "Approve it at %s", st->twitch_uri);
        y = lobby_text(1.0f, y, cols, 255, 255, 255, buf);
        y = lobby_text(1.0f, y, cols, 170, 170, 170,
                       "(this signs the account's other devices out of Twitch)") + 0.5f;
    } else if (st->twitch_state == RPCN_TWITCH_STARTING) {
        y = lobby_text(1.0f, y, cols, 255, 255, 255, "Asking the server for a Twitch code...") + 0.5f;
    }
    if (st->state == NETPLAY_IN_ROOM || st->state == NETPLAY_SYNCING) {
        snprintf(buf, sizeof buf, "Room %llu - %u of %u%s", (unsigned long long)st->room_id,
                 st->member_count, st->max_slot, st->is_host ? " (you run it)" : "");
        y = lobby_text(1.0f, y, cols, 255, 255, 255, buf);
        /* The line, front first: who is up next, and who is on which side. */
        for (uint32_t i = 0; i < st->member_count; i++) {
            const netplay_member_status_t *m = &st->members[i];
            const char *role = m->side == 0 ? "1P" : m->side == 1 ? "2P"
                             : (m->data.flags & ROOM_MEMBER_WATCH) ? "watching"
                             : (m->data.flags & ROOM_MEMBER_READY) ? "ready" : "";
            snprintf(buf, sizeof buf, "%d. %.16s%s  %s  %u-%u", m->line_pos + 1, m->npid,
                     m->is_me ? " (you)" : "", role,
                     (unsigned)m->data.wins, (unsigned)(m->data.games - m->data.wins));
            y = lobby_text(1.0f, y, cols, m->is_me ? 255 : 220, 255, m->is_me ? 140 : 220, buf);
        }
        if (st->auto_start_s) {
            snprintf(buf, sizeof buf, "Next match in %u s", st->auto_start_s);
            y = lobby_text(1.0f, y, cols, 120, 255, 140, buf);
        } else if (st->peer_ready && st->state == NETPLAY_IN_ROOM) {
            y = lobby_text(1.0f, y, cols, 120, 255, 140, "Others are ready - say Ready to start");
        }
        if (st->state == NETPLAY_SYNCING)
            y = lobby_text(1.0f, y, cols, 255, 255, 140, "Waiting for the opponent to start...");
        y += 0.5f;
    }
    if (st->state == NETPLAY_ONLINE && st->room_count == 0 && !st->search_pending)
        y = lobby_text(1.0f, y, cols, 170, 170, 170, "No rooms yet - host one.") + 0.5f;

    for (int i = 0; i < g_lobby.nrows; i++) {
        const lobby_row_t *r = &g_lobby.rows[i];
        snprintf(buf, sizeof buf, "%s %s", i == g_lobby.sel ? ">" : " ", r->text);
        if (!r->enabled)            lobby_text(1.0f, y, cols, 110, 110, 110, buf);
        else if (i == g_lobby.sel)  lobby_text(1.0f, y, cols, 255, 255, 120, buf);
        else                        lobby_text(1.0f, y, cols, 230, 230, 230, buf);
        y += 1.0f;
    }

    /* The reason, when there is one, or the session's last word, just above the
     * key hint on the bottom row. */
    float rows_total = (float)fb_h / scale / 8.0f;
    float hint = rows_total - 1.5f;
    bool  failed = st->error[0] && (st->state == NETPLAY_FAILED || st->state == NETPLAY_OFF);
    const char *last = failed ? st->error
                     : st->log_count ? st->log[(st->log_count - 1) % NETPLAY_LOG_LINES] : "";
    float foot = hint - 0.5f - (lobby_text_ex(1.0f, 0.0f, cols, 0, 0, 0, last, false));
    if (foot < y + 0.5f) foot = y + 0.5f;
    if (failed) lobby_text(1.0f, foot, cols, 255, 110, 110, last);
    else        lobby_text(1.0f, foot, cols, 150, 150, 150, last);
    lobby_text(1.0f, hint, cols, 150, 150, 150, "A select  B close  L1+R1 lobby");
}

int main(int argc, char **argv) {
    if (!parse_args(argc, argv)) {
        fprintf(stderr, "usage: m2hle [--rom] <set.zip> [--render-fps N] [--window WxH] [--stats] [--osd]\n"
                        "             [--log FILE] [--pad-map LIST] [--shot FRAME:FILE] [--exit-after N]\n"
                        "             [--netplay] [--net-config FILE] [--net-delay N]\n");
        return 2;
    }
    if (opt.pad_map && !apply_pad_map(opt.pad_map)) return 2;

    /* The session log flushes every line to disk and a running game warns
     * about unknown COP commands many times a second: off unless asked for. */
    log_init();
    if (opt.log_path) {
        g_log.file = fopen(opt.log_path, "w");
        if (!g_log.file) fprintf(stderr, "m2hle: could not open log %s\n", opt.log_path);
    }
    g_log.file_open_attempted = 1;

    mem_init(&state.bus, NULL, 0);
    i960_reset(&state.cpu);
    bp_init();
    wp_init();
    if (!load_rom(opt.rom)) { dump_log_tail(12); return 1; }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "m2hle: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_AudioStream *audio = opt.sound ? sound_start() : NULL;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    /* textureGather is ES 3.1; everything else the frontend draws is 3.0. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, g_game_render_fill_gather ? 1 : 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    /* No alpha channel: the tile layers leave alpha < 1, and a compositor
     * would blend the frontend beneath the window through it. */
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    bool fullscreen = opt.win_w <= 0 || opt.win_h <= 0;
    SDL_Window *window = SDL_CreateWindow("m2hle", fullscreen ? 640 : opt.win_w, fullscreen ? 480 : opt.win_h,
                                          SDL_WINDOW_OPENGL | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    SDL_GLContext gl = window ? SDL_GL_CreateContext(window) : NULL;
    if (!gl && g_game_render_fill_gather) {
        /* No ES 3.1 here: fall back to 3.0 and the four fetches. */
        fprintf(stderr, "m2hle: no GLES 3.1 context (%s); --fill-gather off\n", SDL_GetError());
        g_game_render_fill_gather = 0;
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        if (window) SDL_DestroyWindow(window);
        window = SDL_CreateWindow("m2hle", fullscreen ? 640 : opt.win_w, fullscreen ? 480 : opt.win_h,
                                  SDL_WINDOW_OPENGL | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
        gl = window ? SDL_GL_CreateContext(window) : NULL;
    }
    if (!gl) {
        fprintf(stderr, "m2hle: GLES 3 window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl);
    /* Paced by the render timer below; vsync on top would stall a skipped frame. */
    SDL_GL_SetSwapInterval(opt.render_fps >= 59.0 ? 1 : 0);
    SDL_HideCursor();
    printf("m2hle %s: %s on %s (%s)\n", M2HLE_VERSION, g_active_profile->display_name,
           (const char *)glGetString(GL_RENDERER), SDL_GetCurrentVideoDriver());

    sg_setup(&(sg_desc){
        .environment.defaults = { .color_format = SG_PIXELFORMAT_RGBA8,
                                  .depth_format = SG_PIXELFORMAT_DEPTH_STENCIL,
                                  .sample_count = 1 },
        .logger.func = slog_func,
    });
    if (!sg_isvalid()) { fprintf(stderr, "m2hle: sokol_gfx setup failed\n"); return 1; }
    if (opt.osd || opt.netplay)
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

    /* Offscreen target at the board's resolution (times --render-scale). */
    int rt_w = VIDEO_WIDTH * opt.render_scale, rt_h = VIDEO_HEIGHT * opt.render_scale;
    sg_image rt_color = {0}, rt_depth = {0};
    sg_view  rt_color_att = {0}, rt_depth_att = {0}, rt_texture = {0};
    if (opt.render_scale > 0) {
        rt_color = sg_make_image(&(sg_image_desc){
            .usage = { .color_attachment = true }, .width = rt_w, .height = rt_h,
            .pixel_format = SG_PIXELFORMAT_RGBA8, .sample_count = 1, .label = "game-target" });
        rt_depth = sg_make_image(&(sg_image_desc){
            .usage = { .depth_stencil_attachment = true }, .width = rt_w, .height = rt_h,
            .pixel_format = SG_PIXELFORMAT_DEPTH_STENCIL, .sample_count = 1, .label = "game-target-depth" });
        rt_color_att = sg_make_view(&(sg_view_desc){ .color_attachment.image = rt_color });
        rt_depth_att = sg_make_view(&(sg_view_desc){ .depth_stencil_attachment.image = rt_depth });
        rt_texture   = sg_make_view(&(sg_view_desc){ .texture.image = rt_color });
    }

    /* Before the emu thread: it pumps the session from its first slice. */
    if (opt.netplay) {
        lobby_init();
        if (lobby_can_sign_in()) lobby_sign_in();
    }
    emu_thread_init(&state.emu, &state.cpu, &state.bus);
    emu_run(&state.emu);

    sg_pass_action pass_action = {
        .colors[0] = { .load_action = SG_LOADACTION_CLEAR, .clear_value = { 0.0f, 0.0f, 0.0f, 1.0f } },
        .depth     = { .load_action = SG_LOADACTION_CLEAR, .clear_value = 1.0f },
    };

    const Uint64 period_ns = opt.render_fps > 0.0 ? (Uint64)(1e9 / opt.render_fps) : 0;
    Uint64 deadline = SDL_GetTicksNS();
    Uint64 stat_start = deadline, stat_cpu_ns = 0, stat_gpu_ns = 0, stat_swap_ns = 0, stat_tile_gpu_ns = 0;
    Uint64 stat_game_gpu_ns = 0;
    uint64_t stat_comp0 = 0, stat_uptile0 = 0, stat_upgfx0 = 0, stat_uppens0 = 0, stat_part0 = 0, stat_blk0 = 0;
    Uint64 next_temp_check = deadline;
    unsigned stat_renders = 0, stat_frames0 = g_emu_frames;
    int shots_done = 0;
    bool running = true;
    int rc = 0;

    while (running) {
        SDL_Event ev;
        bool pad_changed = false;
        while (SDL_PollEvent(&ev)) {
            /* The netplay lobby takes the pad and the keyboard while it is open:
             * L1+R1, Guide or F1 toggle it; the d-pad moves, A picks and B closes,
             * both on release, so the button is up again before the game resumes. */
            if (opt.netplay) {
                bool toggle = false, used = false;
                if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    SDL_Gamepad *pad = SDL_GetGamepadFromID(ev.gbutton.which);
                    int b = ev.gbutton.button;
                    if (b == SDL_GAMEPAD_BUTTON_GUIDE) toggle = true;
                    else if (pad && ((b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER
                                      && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER))
                                  || (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER
                                      && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))))
                        toggle = true;
                    else if (g_lobby.open) {
                        used = true;
                        if (b == SDL_GAMEPAD_BUTTON_DPAD_UP)   lobby_move(-1);
                        if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) lobby_move(+1);
                        if (b == SDL_GAMEPAD_BUTTON_SOUTH || b == SDL_GAMEPAD_BUTTON_EAST)
                            g_lobby.pressed = b;
                    }
                } else if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_UP && g_lobby.open) {
                    used = true;
                    if (ev.gbutton.button == g_lobby.pressed) {
                        g_lobby.pressed = -1;
                        if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) lobby_activate();
                        else lobby_show(false);
                    }
                } else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                    if (ev.key.scancode == SDL_SCANCODE_F1) toggle = true;
                    else if (g_lobby.open && ev.key.scancode != SDL_SCANCODE_ESCAPE) {
                        used = true;
                        if (ev.key.scancode == SDL_SCANCODE_UP)        lobby_move(-1);
                        if (ev.key.scancode == SDL_SCANCODE_DOWN)      lobby_move(+1);
                        if (ev.key.scancode == SDL_SCANCODE_RETURN)    lobby_activate();
                        if (ev.key.scancode == SDL_SCANCODE_BACKSPACE) lobby_show(false);
                    }
                } else if (ev.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && g_lobby.open) {
                    used = true;
                }
                if (toggle) {
                    g_lobby.pressed = -1;
                    lobby_show(!g_lobby.open);
                    pad_changed = true;   /* closing: pick up what is still held */
                    continue;
                }
                if (used) continue;
            }
            switch (ev.type) {
                case SDL_EVENT_QUIT: running = false; break;
                case SDL_EVENT_KEY_DOWN:
                    if (ev.key.scancode == SDL_SCANCODE_ESCAPE) running = false;
                    else if (!ev.key.repeat) input_action_down(key_to_action(ev.key.scancode));
                    break;
                case SDL_EVENT_KEY_UP:
                    input_action_up(key_to_action(ev.key.scancode));
                    break;
                case SDL_EVENT_GAMEPAD_ADDED:
                    if (SDL_OpenGamepad(ev.gdevice.which))
                        printf("m2hle: gamepad %s\n", SDL_GetGamepadNameForID(ev.gdevice.which));
                    pad_changed = true;
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    SDL_CloseGamepad(SDL_GetGamepadFromID(ev.gdevice.which));
                    pad_changed = true;
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                case SDL_EVENT_GAMEPAD_BUTTON_UP:
                case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                    pad_changed = true;
                    break;
                default: break;
            }
        }
        if (opt.netplay) {
            /* The board was reset under the pad (netplay's barrier): it holds
             * nothing now, so press again whatever is still down. */
            if (g_pad_resync) {
                g_pad_resync = 0;
                memset(g_pad_held, 0, sizeof g_pad_held);
                pad_changed = true;
            }
            static bool lobby_was_open;
            if (lobby_was_open && !g_lobby.open) pad_changed = true;
            lobby_was_open = g_lobby.open;
            if (g_lobby.open) pad_changed = false;   /* the lobby has the pad */
        }
        if (pad_changed) pad_refresh();
        if (state.cpu.halted) {
            fprintf(stderr, "m2hle: CPU halted at 0x%08X\n", state.cpu.sfr.ip);
            dump_log_tail(12);
            rc = 1;
            break;
        }
        if (opt.exit_after && g_emu_frames >= opt.exit_after) break;

        /* Render at the requested rate; the emu thread keeps its own 60 Hz. */
        if (period_ns) {
            Uint64 now = SDL_GetTicksNS();
            if (now < deadline) SDL_DelayPrecise(deadline - now);
            deadline += period_ns;
            now = SDL_GetTicksNS();
            if (deadline + period_ns < now) deadline = now;   /* fell behind: don't burst */
        }

        if (opt.gl_finish) glFinish();   /* the tile compose below starts on an idle GPU */
        Uint64 cpu_start = SDL_GetTicksNS();
        int fb_w, fb_h;
        SDL_GetWindowSizeInPixels(window, &fb_w, &fb_h);
        if (opt.gl_finish) {
            /* Only the tile compose puts GPU work in prepare: the wait after it is
             * that pass's GPU time (uploads included), kept out of the CPU time. */
            uint64_t composes = state.video.gpu_composes;
            game_frame_prepare(&state.video, &state.geo3d, &state.bus, &state.romset, true);
            if (state.video.gpu_composes != composes) {
                Uint64 f0 = SDL_GetTicksNS();
                glFinish();
                Uint64 waited = SDL_GetTicksNS() - f0;
                stat_tile_gpu_ns += waited;
                cpu_start += waited;
            }
        } else if (opt.verify_gpu_tiles && state.video.gpu) {
            /* Hold the emu thread so both composes see the same RAM. */
            emu_mutex_lock(&state.emu.mutex);
            uint64_t composes = state.video.gpu_composes + state.video.up_pens;
            game_frame_prepare(&state.video, &state.geo3d, &state.bus, &state.romset, true);
            if (state.video.gpu_composes + state.video.up_pens != composes) verify_gpu_tiles();
            emu_mutex_unlock(&state.emu.mutex);
        } else {
            game_frame_prepare(&state.video, &state.geo3d, &state.bus, &state.romset, true);
        }
        float lerp_t = game_frame_lerp();

        int ox, oy, w, h;
        game_render_letterbox(fb_w, fb_h, VIDEO_WIDTH, VIDEO_HEIGHT, &ox, &oy, &w, &h);
        if (opt.display_scale > 0 && VIDEO_WIDTH * opt.display_scale <= fb_w
                && VIDEO_HEIGHT * opt.display_scale <= fb_h) {
            w  = VIDEO_WIDTH * opt.display_scale;
            h  = VIDEO_HEIGHT * opt.display_scale;
            ox = (fb_w - w) / 2;
            oy = (fb_h - h) / 2;
        }
        if (opt.render_scale > 0) {
            g_fill_log.n = 0; g_fill_log.uploads = 0; g_fill_log.overflow = false;
            sg_begin_pass(&(sg_pass){
                .action = pass_action,
                .attachments = { .colors[0] = rt_color_att, .depth_stencil = rt_depth_att },
            });
            game_frame_draw(&state.video, &state.geo3d, &state.bus, &state.romset,
                            0, 0, rt_w, rt_h, lerp_t);
            sg_end_pass();
            if (opt.gl_finish) {
                /* The game pass is the tile layer quads and the 3D fills: its GPU
                 * time, kept out of the CPU time. */
                Uint64 f0 = SDL_GetTicksNS();
                glFinish();
                Uint64 waited = SDL_GetTicksNS() - f0;
                stat_game_gpu_ns += waited;
                cpu_start += waited;
            }
            if (opt.verify_fill) verify_fill(rt_w, rt_h);
        }
        sg_begin_pass(&(sg_pass){
            .action = pass_action,
            .swapchain = { .width = fb_w, .height = fb_h, .sample_count = 1,
                           .color_format = SG_PIXELFORMAT_RGBA8,
                           .depth_format = SG_PIXELFORMAT_DEPTH_STENCIL,
                           .gl.framebuffer = 0 },
        });
        /* The lobby is drawn on black, to be read; over a match, on the match. */
        if (opt.netplay) lobby_update(SDL_GetTicksNS());
        bool show_game = !(opt.netplay && g_lobby.open && !netplay_state_running(g_lobby.st.state));
        if (show_game && opt.render_scale > 0)
            game_render_draw_target(rt_texture, opt.linear, ox, oy, w, h);
        else if (show_game)
            game_frame_draw(&state.video, &state.geo3d, &state.bus, &state.romset, ox, oy, w, h, lerp_t);
        if (opt.osd) {
            osd_update(SDL_GetTicksNS());
            osd_draw(fb_w, fb_h);
        }
        if (opt.netplay) lobby_draw(fb_w, fb_h);
        if (opt.osd || opt.netplay) sdtx_draw();
        sg_end_pass();
        sg_commit();
        Uint64 cpu_end = SDL_GetTicksNS();
        stat_cpu_ns += cpu_end - cpu_start;
        if (opt.gl_finish) {
            glFinish();
            stat_gpu_ns += SDL_GetTicksNS() - cpu_end;
        }

        while (shots_done < opt.shot_count && g_emu_frames >= opt.shot_frame[shots_done])
            save_png(opt.shot_path[shots_done++], fb_w, fb_h);
        Uint64 swap_start = SDL_GetTicksNS();
        SDL_GL_SwapWindow(window);
        stat_swap_ns += SDL_GetTicksNS() - swap_start;
        stat_renders++;

        Uint64 now = SDL_GetTicksNS();
        if (opt.max_temp > 0.0 && now >= next_temp_check) {
            next_temp_check = now + 500000000ull;
            int hot = read_milli("/sys/class/thermal/thermal_zone0/temp");
            int gpu = read_milli("/sys/class/thermal/thermal_zone1/temp");
            if (gpu > hot) hot = gpu;
            if (hot >= (int)(opt.max_temp * 1000.0)) {
                printf("m2hle: %.1fC reached --max-temp %.0f, quitting\n", hot / 1000.0, opt.max_temp);
                rc = 3;
                running = false;
            }
        }
        if (opt.stats && now - stat_start >= 5000000000ull) {
            double secs = (double)(now - stat_start) / 1e9;
            double n = stat_renders ? (double)stat_renders : 1.0;
            const game_frame_times_t *t = &g_game_frame_times;
            printf("m2hle: game %.1f fps, render %.1f fps | per render ms: cpu %.2f [compose %.2f scan %.2f "
                   "upload %.2f 3d %.2f tiles %.2f]%s%.2f swap %.2f | cpu %.1fC gpu %.1fC\n",
                   (g_emu_frames - stat_frames0) / secs, stat_renders / secs,
                   stat_cpu_ns / 1e6 / n, t->compose_us / 1e3 / n, t->scan_us / 1e3 / n,
                   t->upload_us / 1e3 / n, t->draw3d_us / 1e3 / n, t->tiles_us / 1e3 / n,
                   opt.gl_finish ? " gpu " : " ", opt.gl_finish ? stat_gpu_ns / 1e6 / n : 0.0,
                   stat_swap_ns / 1e6 / n,
                   read_milli("/sys/class/thermal/thermal_zone0/temp") / 1000.0,
                   read_milli("/sys/class/thermal/thermal_zone1/temp") / 1000.0);
            const video_state_t *v = &state.video;
            uint64_t comps = v->gpu_composes - stat_comp0;
            printf("m2hle: tiles: %llu composes (%.0f%% of renders, %llu partial, %.0f%% of the screen drawn), "
                   "uploads tile %llu gfx %llu pens %llu",
                   (unsigned long long)comps, 100.0 * (double)comps / n,
                   (unsigned long long)(v->partial_composes - stat_part0),
                   comps ? 100.0 * (double)(v->composed_blocks - stat_blk0) / ((double)comps * VIDEO_BLK_W * VIDEO_BLK_H) : 0.0,
                   (unsigned long long)(v->up_tile - stat_uptile0), (unsigned long long)(v->up_gfx - stat_upgfx0),
                   (unsigned long long)(v->up_pens - stat_uppens0));
            if (opt.gl_finish && comps)
                printf(" | gpu %.2f ms per compose", stat_tile_gpu_ns / 1e6 / (double)comps);
            if (opt.gl_finish && opt.render_scale > 0)
                printf(" | game pass gpu %.2f ms per render", stat_game_gpu_ns / 1e6 / n);
            if (audio)
                printf(" | sound: %llu underrun frames, %llu dropped total",
                       (unsigned long long)g_audio_out.underruns, (unsigned long long)g_sound.out_dropped);
            printf("\n");
            stat_game_gpu_ns = 0;
            fflush(stdout);
            stat_comp0 = v->gpu_composes; stat_uptile0 = v->up_tile; stat_upgfx0 = v->up_gfx; stat_uppens0 = v->up_pens;
            stat_part0 = v->partial_composes; stat_blk0 = v->composed_blocks;
            stat_tile_gpu_ns = 0;
            stat_start = now; stat_cpu_ns = stat_gpu_ns = stat_swap_ns = 0;
            stat_renders = 0; stat_frames0 = g_emu_frames;
            memset(&g_game_frame_times, 0, sizeof g_game_frame_times);
        }
    }

    emu_thread_shutdown(&state.emu);
    if (opt.netplay) netplay_shutdown();   /* after the emu thread: it is the only thing that pumps it */
    if (audio) SDL_DestroyAudioStream(audio);   /* after the emu thread: no more ring writes */
    if (opt.verify_fill)
        printf("verify-fill: %llu frames of fills checked, %llu differed, %llu skipped (vertex buffer uploaded twice)\n",
               (unsigned long long)g_vf.frames, (unsigned long long)g_vf.bad_frames, (unsigned long long)g_vf.skipped);
    if (opt.verify_gpu_tiles)
        printf("verify-gpu-tiles: %llu composed frames checked, %llu differed\n",
               (unsigned long long)g_vt.frames, (unsigned long long)g_vt.bad_frames);
    if (opt.render_scale > 0) {
        sg_destroy_view(rt_texture);
        sg_destroy_view(rt_depth_att);
        sg_destroy_view(rt_color_att);
        sg_destroy_image(rt_depth);
        sg_destroy_image(rt_color);
    }
    if (opt.osd || opt.netplay) sdtx_shutdown();
    game_render_shutdown();
    video_shutdown(&state.video);
    sg_shutdown();
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    romset_free(&state.romset);
    mem_shutdown(&state.bus);
    log_file_close();
    return rc;
}
