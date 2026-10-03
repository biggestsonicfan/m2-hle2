/*
 * main_dc.c -- the Dreamcast frontend (Pinboard #340): the board and a game
 * profile under KallistiOS, the ROM off the disc a page at a time (dc_pager.h).
 *
 * The board runs, the pad is mapped, and the picture is the PowerVR's (D2,
 * dc_pvr.h): the tile layers and the 3D scene from the board's display list.
 * The numbers that decide the rest are on the screen (and dbgio with
 * -DDC_STATS_DBGIO=1: the serial port costs ~2% of the SH-4): board
 * fps, the slice's and the picture's time, and the pager's traffic. No netplay. Sound is Sega's console way, a trap at
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

#ifndef DC_STATS_DBGIO
#define DC_STATS_DBGIO 0
#endif
#define STATS_PRINT(l) do { if (DC_STATS_DBGIO) printf("%s\n", (l)); } while (0)

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
#include "dc_pvr.h"

KOS_INIT_FLAGS(INIT_IRQ | INIT_CONTROLLER | INIT_CDROM);

static memory_bus_t     bus;
static i960_cpu_t       cpu;
static emu_thread_ctx_t ctx;
static romset_t         rs;
static tile_cpu_t       tiles;
static geo3d_state_t    geo;

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
    if (strncmp(g_active_profile->id, "sfight", 6) ||   /* sfight, sfight_console */
         g_active_profile->hook_count >= HLE_HOOK_TABLE_MAX) return;
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

/* ---- Main ----------------------------------------------------------------------- */

static void dc_text(int row, const char *s) {
    dp_text(row, s);
    dp_text_frame();
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    vid_set_mode(DM_640x480, PM_RGB565);
    if (dp_init() != 0) { printf("pvr_init failed\n"); for (;;) thd_sleep(1000); }
    dc_text(0, "m2-hle2 for Dreamcast: finding the ROM files");

    /* Sound first: its effects stay in RAM, and it reads the disc through
     * KOS's driver, which the pager forbids once it is up. */
    bool sound = ds_init() == 0;

    /* The frame pool takes what the board leaves: texture RAM (2 MB), its
     * framebuffer (0.5 MB), the mesh cache (GEO3D_MESH_CACHE_BYTES and one
     * mesh over) and the heap's own use come out of what is free now. */
    const uint32_t keep = TEXRAM0_SIZE + TEXRAM1_SIZE + FRAMEBUFFER_SIZE + GEO3D_MESH_CACHE_BYTES + (768u << 10);
    uint32_t cache = 8u << 20;
    for (void *p; cache > (1u << 20); cache -= 256u << 10)
        if ((p = memalign(16384, cache + keep))) { free(p); break; }
    if (pg_init(&dc_layout_sfight, cache, VID_EXT_RAM_SIZE) != 0 || dc_romset() != 0) {
        dc_text(1, "the disc lacks a ROM file (dc_layout.h)");
        for (;;) thd_sleep(1000);
    }
    g_mem_window = dc_window;
    if (sound) dc_add_sound_hook();
    char line[128];
    snprintf(line, sizeof line, "profile %s, cache %u KB", g_active_profile->id, (unsigned)(cache >> 10));
    printf("%s\n", line);
    dp_text(2, line);

    if (!DC_STATS_DBGIO) dbgio_dev_select("null");  /* printf to the serial port was 2% of a fight */
    mem_init(&bus, NULL, 0);
    i960_reset(&cpu);
    dc_install_board();
    emu_ctx_init(&ctx, &cpu, &bus);
    geo3d_init(&geo);
    ctx.run_state = EMU_RUNNING;

    uint64_t t_last = timer_us_gettime64(), us_slice = 0, us_draw = 0;
    uint32_t f_last = g_emu_frames, slices = 0, loads_last = 0, refills_last = 0, shown = 0, drawn_f = 0;
    uint64_t builds_last = 0, hits_last = 0;
    uint64_t read_last = 0;
    while (!cpu.halted) {
        dc_pad();
        uint64_t t0 = timer_us_gettime64();
        uint32_t f = g_emu_frames;
        emu_slice_body(&ctx);
        emu_slice_finish(&ctx);
        uint64_t t1 = timer_us_gettime64();
        /* A board frame not yet shown goes to the PVR when it can take one. */
        if (g_emu_frames != drawn_f && dp_frame(&geo, &bus, &rs, &tiles)) { drawn_f = g_emu_frames; shown++; }
        (void)f;
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
            snprintf(line, sizeof line, "frame %u %.1f fps (shown %.1f) slice %u ms 3d %u+%u ms",
                     (unsigned)g_emu_frames, fr / sec, shown / sec,
                     (unsigned)(us_slice / 1000 / (slices ? slices : 1)),
                     (unsigned)(g_dp.us_decode / 1000 / (shown ? shown : 1)),
                     (unsigned)(g_dp.us_submit / 1000 / (shown ? shown : 1)));
            STATS_PRINT(line);
            dp_text(0, line);
            /* Per 2 s: loads (their read time), refills; since boot: the rest. */
            snprintf(line, sizeof line, "ld %u (%u ms) tlb %u | ev %u pin %u wr %u err %u",
                     (unsigned)loads, (unsigned)read_ms, (unsigned)refills, (unsigned)g_pg.evictions,
                     (unsigned)pg_pinned(), (unsigned)g_pg.rom_writes, (unsigned)g_pg.read_errors);
            STATS_PRINT(line);
            dp_text(1, line);
            snprintf(line, sizeof line, "snd %s codes %u unk %u bgm %d ring %u KB under %u",
                     g_ds.dev ? "on" : "off", (unsigned)g_ds.codes, (unsigned)g_ds.unknown,
                     g_ds.bgm == 0xFFFF ? -1 : (int)g_ds.bgm,
                     (unsigned)((g_ds.r_head - g_ds.r_tail) >> 10), (unsigned)g_ds.underruns);
            STATS_PRINT(line);
            dp_text(19, line);   /* the bottom row: the game draws over row 2 */
            snprintf(line, sizeof line, "tris %u runs %u full %u | tex %u new %u drop %u fail %u",
                     g_dp.tris, g_dp.runs, g_dp.faces_dropped, g_dp.count, g_dp.made, g_dp.dropped, g_dp.fails);
            STATS_PRINT(line);
            dp_text(18, line);
            unsigned d = shown ? shown : 1;
            snprintf(line, sizeof line, "tiles %u scan %u sort %u ms | mesh %u built %u hit %u",
                     (unsigned)(g_dp.us_tiles / 1000 / d), (unsigned)(g_dp.us_scan / 1000 / d),
                     (unsigned)(g_dp.us_sort / 1000 / d), (unsigned)g_geo3d_mesh_count,
                     (unsigned)(g_geo3d_mesh_builds - builds_last), (unsigned)(g_geo3d_mesh_hits - hits_last));
            STATS_PRINT(line);
            dp_text(17, line);
            builds_last = g_geo3d_mesh_builds; hits_last = g_geo3d_mesh_hits;
            g_dp.us_tiles = g_dp.us_scan = g_dp.us_sort = 0;
            t_last = t2; f_last = g_emu_frames; us_slice = us_draw = 0; slices = 0; shown = 0;
            g_dp.us_decode = g_dp.us_submit = 0; g_dp.made = g_dp.dropped = 0;
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
