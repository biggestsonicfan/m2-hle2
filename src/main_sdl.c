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
 *            test none.
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
 */
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
} opt = { .render_fps = 30.0, .render_scale = 1, .linear = true };

/* ---- Gamepad ------------------------------------------------------------- */

/* Button → action. Directions always come from the d-pad and the left stick. */
typedef struct { const char *name; SDL_GamepadButton button; int action; } pad_bind_t;

static pad_bind_t g_pad_binds[] = {
    { "south", SDL_GAMEPAD_BUTTON_SOUTH,          GAME_INPUT_P1_B1    },
    { "east",  SDL_GAMEPAD_BUTTON_EAST,           GAME_INPUT_P1_B2    },
    { "west",  SDL_GAMEPAD_BUTTON_WEST,           GAME_INPUT_P1_B3    },
    { "north", SDL_GAMEPAD_BUTTON_NORTH,          GAME_INPUT_P1_B4    },
    { "start", SDL_GAMEPAD_BUTTON_START,          GAME_INPUT_P1_START },
    { "back",  SDL_GAMEPAD_BUTTON_BACK,           GAME_INPUT_P1_COIN  },
    { "l1",    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  -1 },
    { "r1",    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, -1 },
    { "l3",    SDL_GAMEPAD_BUTTON_LEFT_STICK,     -1 },
    { "r3",    SDL_GAMEPAD_BUTTON_RIGHT_STICK,    -1 },
    { "guide", SDL_GAMEPAD_BUTTON_GUIDE,          -1 },
};
#define PAD_BIND_COUNT (int)(sizeof g_pad_binds / sizeof g_pad_binds[0])

static int action_by_name(const char *s) {
    static const struct { const char *name; int action; } names[] = {
        { "b1", GAME_INPUT_P1_B1 }, { "b2", GAME_INPUT_P1_B2 }, { "b3", GAME_INPUT_P1_B3 },
        { "b4", GAME_INPUT_P1_B4 }, { "start", GAME_INPUT_P1_START }, { "coin", GAME_INPUT_P1_COIN },
        { "service", GAME_INPUT_SERVICE }, { "test", GAME_INPUT_TEST }, { "none", -1 },
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(s, names[i].name)) return names[i].action;
    return -2;
}

static bool apply_pad_map(const char *list) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", list);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        char *eq = strchr(tok, '=');
        if (!eq) { fprintf(stderr, "--pad-map: '%s' is not button=action\n", tok); return false; }
        *eq = '\0';
        int act = action_by_name(eq + 1);
        int b;
        for (b = 0; b < PAD_BIND_COUNT && strcmp(g_pad_binds[b].name, tok); b++) {}
        if (b == PAD_BIND_COUNT || act == -2) {
            fprintf(stderr, "--pad-map: unknown button or action in '%s=%s'\n", tok, eq + 1);
            return false;
        }
        g_pad_binds[b].action = act;
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
        for (int b = 0; b < PAD_BIND_COUNT; b++)
            if (g_pad_binds[b].action >= 0 && SDL_GetGamepadButton(pad, g_pad_binds[b].button))
                now[g_pad_binds[b].action] = true;
    }
    SDL_free(ids);
    for (int a = 0; a < GAME_INPUT_COUNT; a++) {
        if (now[a] && !g_pad_held[a]) input_action_down(a);
        if (!now[a] && g_pad_held[a]) input_action_up(a);
        g_pad_held[a] = now[a];
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
    g_audio_out.rate  = SOUND_RATE;
    g_audio_out.ready = true;
    SDL_ResumeAudioStreamDevice(stream);
    printf("m2hle: sound board on\n");
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
        else if (!strcmp(a, "--match-replay"))       g_match_replay = 1;   /* attract straight to its replay fight */
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
    sdtx_draw();
}

int main(int argc, char **argv) {
    if (!parse_args(argc, argv)) {
        fprintf(stderr, "usage: m2hle [--rom] <set.zip> [--render-fps N] [--window WxH] [--stats] [--osd]\n"
                        "             [--log FILE] [--pad-map LIST] [--shot FRAME:FILE] [--exit-after N]\n");
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
    SDL_Window *window = SDL_CreateWindow("m2hle", fullscreen ? 640 : opt.win_w, fullscreen ? 480 : opt.win_h,
                                          SDL_WINDOW_OPENGL | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    SDL_GLContext gl = window ? SDL_GL_CreateContext(window) : NULL;
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
    if (opt.osd)
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
        if (opt.render_scale > 0)
            game_render_draw_target(rt_texture, opt.linear, ox, oy, w, h);
        else
            game_frame_draw(&state.video, &state.geo3d, &state.bus, &state.romset, ox, oy, w, h, lerp_t);
        if (opt.osd) {
            osd_update(SDL_GetTicksNS());
            osd_draw(fb_w, fb_h);
        }
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
    if (opt.osd) sdtx_shutdown();
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
