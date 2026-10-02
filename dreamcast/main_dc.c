/*
 * main_dc.c -- the Dreamcast frontend (Pinboard #340): the board and a game
 * profile under KallistiOS, the ROM off the disc a page at a time (dc_pager.h).
 *
 * Milestone D1: the board runs, the tile layers are on screen, the pad is mapped,
 * and the numbers that decide the rest are printed (dbgio and the screen):
 * board fps, the slice's and the compose's time, and the pager's traffic. No
 * 3D (the PowerVR is D2), no netplay. Sound is Sega's console way, a trap at
 * the game's sound call playing ADX cues off the disc (dc_sound.h, #342).
 *
 * The disc holds 1ST_READ.BIN and the PS3 release's ROM files as they ship
 * (dc_layout.h); see dreamcast/README.md for the build and the disc image.
 */
/* net/ first, as in main.c: net_socket.h owns the socket include order. */
#include "net/netplay.h"

#include <kos.h>
#include <dc/maple/controller.h>
#include <dc/biosfont.h>

#include "constants.h"
#include "log.h"
#include "memory.h"
#include "i960.h"
#include "i960_exec.h"
#include "rom_loader.h"
#include "emu_thread.h"
#include "input.h"
#include "registry.h"
#include "tile_renderer.h"

#include "dc_pager.h"
#include "dc_sound.h"

KOS_INIT_FLAGS(INIT_IRQ | INIT_CONTROLLER | INIT_CDROM);

static memory_bus_t     bus;
static i960_cpu_t       cpu;
static emu_thread_ctx_t ctx;
static romset_t         rs;
static tile_cpu_t       tiles;
static uint16_t         pen565[TILE_PEN_NONE + 1];

/* ---- The ROM, as windows ---------------------------------------------------- */

/* memory.h's g_mem_window: the board's MAIN_DATA and XTRA_DATA are the ROM's
 * main_data (STF's XTRA_DATA is its second 16 MB, sfight_install), and
 * VID_EXT_RAM is anonymous. Everything else the board allocates. */
static uint8_t *dc_window(const char *name, size_t size) {
    if (!strcmp(name, "MAIN_DATA") && rs.main_data_size >= size) return rs.main_data;
    if (!strcmp(name, "XTRA_DATA") && rs.main_data_size >= 0x1000000u + size) return rs.main_data + 0x1000000u;
    if (!strcmp(name, "VID_EXT_RAM")) return pg_anon(0, (uint32_t)size);
    return NULL;
}

static int dc_romset(void) {
    static const struct { const char *name; size_t off_p, off_s; } map[] = {
        { "maincpu",   offsetof(romset_t, maincpu),    offsetof(romset_t, maincpu_size) },
        { "main_data", offsetof(romset_t, main_data),  offsetof(romset_t, main_data_size) },
        { "copro",     offsetof(romset_t, copro_data), offsetof(romset_t, copro_data_size) },
        { "polygons",  offsetof(romset_t, polygons),   offsetof(romset_t, polygons_size) },
        { "textures",  offsetof(romset_t, textures),   offsetof(romset_t, textures_size) },
        { "audiocpu",  offsetof(romset_t, audiocpu),   offsetof(romset_t, audiocpu_size) },
        { "samples",   offsetof(romset_t, samples),    offsetof(romset_t, samples_size) },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        size_t size = 0;
        uint8_t *p = pg_region(map[i].name, &size);
        *(uint8_t **)((char *)&rs + map[i].off_p) = size ? p : NULL;
        *(size_t *)((char *)&rs + map[i].off_s)   = size;
    }
    rs.loaded = rs.maincpu != NULL;
    for (size_t i = 0; i < g_profile_count; i++)
        if (!strcmp(g_profiles[i]->id, g_pg.lay->profile)) g_active_profile = g_profiles[i];
    if (!g_active_profile) { printf("no profile %s in this build\n", g_pg.lay->profile); return -1; }
    return rs.loaded ? 0 : -1;
}

/* sound_request_special (0x3F268): the code in g0 goes to dc_sound.h and the
 * function is skipped, so the sound board is never written (Sega's DLL traps
 * it the same way). The profiles are const: the frontend runs a copy of the
 * active one with the trap added. */
static int dc_hook_sound(i960_cpu_t *c, memory_bus_t *b) {
    (void)b;
    ds_code(c->globals.g[0]);
    hle_ret(c);
    return 0;
}

static game_profile_t dc_profile;

static void dc_add_sound_hook(void) {
    if (strcmp(g_active_profile->id, "sfight") || g_active_profile->hook_count >= HLE_HOOK_TABLE_MAX) return;
    dc_profile = *g_active_profile;
    dc_profile.hooks[dc_profile.hook_count++] =
        (hle_hook_entry_t){ 0x0003F268, dc_hook_sound, "sound_request_special (dc_sound)" };
    g_active_profile = &dc_profile;
}

/* web_install_board, less the sound board. */
static void dc_install_board(void) {
    pg_rom_revert();
    pg_anon_clear(0);
    g_active_profile->install_fn(&rs, &cpu, &bus);
    irqt_reset();
    input_reset();
    input_attach(&bus);
    emu_board_reset_state();
}

/* ---- The pad ---------------------------------------------------------------- */

static void dc_pad(void) {
    static const struct { uint32_t mask; int act; } map[] = {
        { CONT_DPAD_UP,    GAME_INPUT_P1_UP },   { CONT_DPAD_DOWN,  GAME_INPUT_P1_DOWN },
        { CONT_DPAD_LEFT,  GAME_INPUT_P1_LEFT }, { CONT_DPAD_RIGHT, GAME_INPUT_P1_RIGHT },
        { CONT_A, GAME_INPUT_P1_B1 }, { CONT_B, GAME_INPUT_P1_B2 },
        { CONT_X, GAME_INPUT_P1_B3 }, { CONT_Y, GAME_INPUT_P1_B4 },
        { CONT_START, GAME_INPUT_P1_START },
    };
    static uint32_t was;
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t *st = dev ? (cont_state_t *)maple_dev_status(dev) : NULL;
    uint32_t now = 0;
    if (st) {
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (st->buttons & map[i].mask) now |= 1u << map[i].act;
        if (st->ltrig > 128) now |= 1u << GAME_INPUT_P1_COIN;
    }
    for (int a = 0; a < GAME_INPUT_COUNT; a++) {
        uint32_t b = 1u << a;
        if ((now & b) && !(was & b)) input_action_down(a);
        if (!(now & b) && (was & b)) input_action_up(a);
    }
    was = now;
}

/* ---- The picture -------------------------------------------------------------- */

#define DC_OX ((640 - VIDEO_WIDTH) / 2)
#define DC_OY ((480 - VIDEO_HEIGHT) / 2)

/* Both tile layers as pens (tile_cpu_draw, the board's own compositor), then
 * straight to the RGB565 framebuffer: the front layer where it drew, the back
 * one elsewhere. No RGBA layers in between (video_window.h keeps 1.5 MB of
 * them for the GPU). Only when tile RAM, graphics or a colour changed. */
static bool dc_compose(void) {
    static uint32_t s_tile = ~0u, s_gfx = ~0u, s_pal = ~0u, s_lut = ~0u;
    static int16_t x0[VIDEO_HEIGHT], x1[VIDEO_HEIGHT];
    bool redraw = bus.gen_tile != s_tile || bus.gen_gfx != s_gfx;
    bool recolour = bus.gen_pal != s_pal || bus.gen_lut != s_lut;
    if (!redraw && !recolour) return false;
    s_tile = bus.gen_tile; s_gfx = bus.gen_gfx; s_pal = bus.gen_pal; s_lut = bus.gen_lut;
    if (redraw) {
        memcpy(tiles.words, bus.tile, sizeof tiles.words);
        for (int y = 0; y < VIDEO_HEIGHT; y++) { x0[y] = 0; x1[y] = VIDEO_WIDTH; }
        tile_cpu_draw(&tiles, bus.tmapgfx, x0, x1);
    }
    if (recolour) {
        uint8_t chan[3][32];
        video_pen_channels(&bus, chan);
        for (int p = 0; p < TILE_PEN_NONE; p++) {
            uint16_t c = pal_read16(&bus, p);
            uint8_t r = chan[0][c & 31], g = chan[1][(c >> 5) & 31], b = chan[2][(c >> 10) & 31];
            pen565[p] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
        }
    }
    for (int y = 0; y < VIDEO_HEIGHT; y++) {
        const uint16_t *bg = tiles.bg + y * VIDEO_WIDTH, *fg = tiles.fg + y * VIDEO_WIDTH;
        uint16_t *d = vram_s + (DC_OY + y) * 640 + DC_OX;
        for (int x = 0; x < VIDEO_WIDTH; x++)
            d[x] = pen565[fg[x] != TILE_PEN_NONE ? fg[x] : bg[x]];
    }
    return true;
}

/* ---- Main ----------------------------------------------------------------------- */

static void dc_text(int row, const char *s) {
    for (int y = 0; y < 24; y++) memset(vram_s + (row * 24 + y) * 640, 0, 640 * 2);
    bfont_draw_str(vram_s + row * 24 * 640 + 8, 640, true, s);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    vid_set_mode(DM_640x480, PM_RGB565);
    memset(vram_s, 0, 640 * 480 * 2);
    dc_text(0, "m2-hle2 for Dreamcast: finding the ROM files");

    /* Sound first: its effects stay in RAM, and it reads the disc through
     * KOS's driver, which the pager forbids once it is up. */
    bool sound = ds_init() == 0;

    /* The frame pool takes what the board leaves: texture RAM (2 MB), its
     * framebuffer (0.5 MB) and the heap's own use come out of what is free now. */
    uint32_t cache = 8u << 20;
    for (void *p; cache > (1u << 20); cache -= 256u << 10)
        if ((p = memalign(16384, cache + (3u << 20)))) { free(p); break; }
    if (pg_init(&dc_layout_sfight, cache, VID_EXT_RAM_SIZE) != 0 || dc_romset() != 0) {
        dc_text(1, "the disc lacks a ROM file (dc_layout.h)");
        for (;;) thd_sleep(1000);
    }
    g_mem_window = dc_window;
    if (sound) dc_add_sound_hook();
    printf("profile %s, cache %u KB\n", g_active_profile->id, (unsigned)(cache >> 10));

    mem_init(&bus, NULL, 0);
    i960_reset(&cpu);
    dc_install_board();
    emu_ctx_init(&ctx, &cpu, &bus);
    ctx.run_state = EMU_RUNNING;

    uint64_t t_last = timer_us_gettime64(), us_slice = 0, us_draw = 0;
    uint32_t f_last = g_emu_frames, slices = 0, loads_last = 0, refills_last = 0;
    uint64_t read_last = 0;
    char line[128];
    while (!cpu.halted) {
        dc_pad();
        uint64_t t0 = timer_us_gettime64();
        uint32_t f = g_emu_frames;
        emu_slice_body(&ctx);
        emu_slice_finish(&ctx);
        uint64_t t1 = timer_us_gettime64();
        if (g_emu_frames != f) dc_compose();
        ds_pump();
        uint64_t t2 = timer_us_gettime64();
        us_slice += t1 - t0;
        us_draw  += t2 - t1;
        slices++;
        if (t2 - t_last >= 2000000) {
            uint32_t fr = g_emu_frames - f_last;
            double sec = (double)(t2 - t_last) / 1e6;
            uint32_t loads = g_pg.loads - loads_last, refills = g_pg.refills - refills_last;
            uint32_t read_ms = (uint32_t)((g_pg.read_ns - read_last) / 1000000);
            snprintf(line, sizeof line, "frame %u %.1f fps slice %u draw %u ms log %d",
                     (unsigned)g_emu_frames, fr / sec, (unsigned)(us_slice / 1000 / (slices ? slices : 1)),
                     (unsigned)(us_draw / 1000 / (fr ? fr : 1)), g_log.count);
            printf("%s\n", line);
            dc_text(0, line);
            /* Per 2 s: loads (their read time), refills; since boot: the rest. */
            snprintf(line, sizeof line, "ld %u (%u ms) tlb %u | ev %u pin %u wr %u err %u",
                     (unsigned)loads, (unsigned)read_ms, (unsigned)refills, (unsigned)g_pg.evictions,
                     (unsigned)pg_pinned(), (unsigned)g_pg.rom_writes, (unsigned)g_pg.read_errors);
            printf("%s\n", line);
            dc_text(1, line);
            snprintf(line, sizeof line, "snd %s codes %u unk %u bgm %d ring %u KB under %u",
                     g_ds.dev ? "on" : "off", (unsigned)g_ds.codes, (unsigned)g_ds.unknown,
                     g_ds.bgm == 0xFFFF ? -1 : (int)g_ds.bgm,
                     (unsigned)((g_ds.r_head - g_ds.r_tail) >> 10), (unsigned)g_ds.underruns);
            printf("%s\n", line);
            dc_text(2, line);
            t_last = t2; f_last = g_emu_frames; us_slice = us_draw = 0; slices = 0;
            loads_last = g_pg.loads; refills_last = g_pg.refills; read_last = g_pg.read_ns;
        }
    }
    /* No serial console in Flycast's libretro core: the reason goes on screen,
     * with the last lines of the board's log. */
    snprintf(line, sizeof line, "the i960 halted at %08lx, frame %u", (unsigned long)cpu.sfr.ip,
             (unsigned)g_emu_frames);
    printf("%s\n", line);
    dc_text(3, line);
    snprintf(line, sizeof line, "pager: %u loads %u refills %u errors %u writes",
             (unsigned)g_pg.loads, (unsigned)g_pg.refills, (unsigned)g_pg.read_errors,
             (unsigned)g_pg.rom_writes);
    dc_text(4, line);
    for (int i = 0, n = g_log.count < 12 ? g_log.count : 12; i < n; i++) {
        const char *s = g_log.lines[(g_log.count - n + i) % LOG_MAX_LINES];
        snprintf(line, sizeof line, "%.52s", s);
        printf("%s\n", s);
        dc_text(6 + i, line);
    }
    for (;;) thd_sleep(1000);
    return 0;
}
