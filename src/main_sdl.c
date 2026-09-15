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
 * --verify-gpu-tiles  on every frame the GPU composes the tile layers, pause
 *            the emu thread, compose the same RAM on the CPU as well and
 *            compare the two byte for byte (FG colour only where it shows).
 *
 * Audio is not wired up: the 68K sound board and SCSP stay off.
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

/* ---- ROM loading (main.c load_active_profile, without the sound board) --- */

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

/* Compare the GPU layers against a CPU compose of the same RAM. */
static void verify_gpu_tiles(void) {
    video_compose_cpu(&state.video, &state.bus);
    read_layer(state.video.bg_image, g_vt.bg);
    read_layer(state.video.fg_image, g_vt.fg);
    int bad = 0, first = -1;
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

/* ---- Main ------------------------------------------------------------------ */

static bool parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool more = i + 1 < argc;
        if      (!strcmp(a, "--rom") && more)        opt.rom = argv[++i];
        else if (!strcmp(a, "--render-fps") && more) opt.render_fps = atof(argv[++i]);
        else if (!strcmp(a, "--window") && more)     { if (sscanf(argv[++i], "%dx%d", &opt.win_w, &opt.win_h) != 2) return false; }
        else if (!strcmp(a, "--stats"))              opt.stats = true;
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
    Uint64 stat_start = deadline, stat_cpu_ns = 0, stat_gpu_ns = 0, stat_swap_ns = 0;
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

        Uint64 cpu_start = SDL_GetTicksNS();
        int fb_w, fb_h;
        SDL_GetWindowSizeInPixels(window, &fb_w, &fb_h);
        if (opt.verify_gpu_tiles && state.video.gpu) {
            /* Hold the emu thread so both composes see the same RAM. */
            emu_mutex_lock(&state.emu.mutex);
            uint64_t composes = state.video.gpu_composes;
            game_frame_prepare(&state.video, &state.geo3d, &state.bus, &state.romset, true);
            if (state.video.gpu_composes != composes) verify_gpu_tiles();
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
            sg_begin_pass(&(sg_pass){
                .action = pass_action,
                .attachments = { .colors[0] = rt_color_att, .depth_stencil = rt_depth_att },
            });
            game_frame_draw(&state.video, &state.geo3d, &state.bus, &state.romset,
                            0, 0, rt_w, rt_h, lerp_t);
            sg_end_pass();
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
            fflush(stdout);
            stat_start = now; stat_cpu_ns = stat_gpu_ns = stat_swap_ns = 0;
            stat_renders = 0; stat_frames0 = g_emu_frames;
            memset(&g_game_frame_times, 0, sizeof g_game_frame_times);
        }
    }

    emu_thread_shutdown(&state.emu);
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
