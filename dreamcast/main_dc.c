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

/* -DDC_HASH_FRAME=n: at board frame n, a hash of work RAM, the i960's
 * registers and its cycle count goes on row 15 (an A/B of two builds). */
/* Draw every Nth board frame (2: every other). The board runs every frame
 * either way; the frames between are never decoded. */
#ifndef DC_DRAW_EVERY
#define DC_DRAW_EVERY 1
#endif
#ifndef DC_HASH_FRAME
#define DC_HASH_FRAME 0
#endif
#ifndef DC_STATS_DBGIO
#define DC_STATS_DBGIO 0
#endif
#ifndef DC_BENCH_F0
#define DC_BENCH_F0 3500u   /* the fight's frames the bench line times */
#define DC_BENCH_F1 3900u
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
#include "gems.h"

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
    /* Homebrew (m2-pacman) is a disc whose ROM_CODE1.BIN is another program:
     * the set's any_program profile runs it, as on the desktop. */
    if (rs.loaded) profile_adopt_program(rs.maincpu, rs.maincpu_size);
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
static bool s_dc_gems;   /* gems.h is on */

static void dc_add_sound_hook(void) {
    if (strncmp(g_active_profile->id, "sfight", 6) || g_active_profile->any_program ||   /* sfight, sfight_console */
         g_active_profile->hook_count >= HLE_HOOK_TABLE_MAX) return;
    dc_profile = *g_active_profile;
    dc_profile.hooks[dc_profile.hook_count++] =
        (hle_hook_entry_t){ 0x0003F268, dc_hook_sound, "sound_request_special (dc_sound)" };
    g_active_profile = &dc_profile;
}

/* No sound board: the i960's UART has nobody on the line. It can always take a
 * byte (TxRDY, TxEMPTY) and never has one (RxRDY down). The plain region read
 * back the 0x37 a program last wrote to the control register, and m2-pacman's
 * sound probe drained "received" bytes forever. */
static uint32_t dc_uart_read(mem_region_t *r, uint32_t addr, int size) {
    (void)r; (void)size;
    return addr - MIDI_BASE == 4 ? 0x05u : 0u;
}

/* web_install_board, less the sound board (dc_uart_read in its place). */
static void dc_install_board(void) {
    pg_rom_revert();
    pg_anon_clear(0);
    g_active_profile->install_fn(&rs, &cpu, &bus);
    for (int i = 0; i < bus.region_count; i++)
        if (bus.regions[i].base == MIDI_BASE) { bus.regions[i].read_cb = dc_uart_read; mem_regions_changed(&bus); }
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

#include "dc_jit_test.h"
#ifdef IB_WHY
static char g_calib[64];
#endif

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    vid_set_mode(DM_640x480, PM_RGB565);
    if (dp_init() != 0) { printf("pvr_init failed\n"); for (;;) thd_sleep(1000); }
    dc_text(0, "m2-hle2 for Dreamcast: finding the ROM files");

    /* Sound first: its effects stay in RAM, and it reads the disc through
     * KOS's driver, which the pager forbids once it is up. */
    bool sound = ds_init() == 0;

    /* The frame pool takes what the board leaves: texture RAM (2 MB), its
     * framebuffer (0.5 MB) and the heap's own use come out of what is free
     * now. The mesh cache has its own block (GEO3D_MESH_ARENA). */
    const uint32_t keep = TEXRAM0_SIZE + TEXRAM1_SIZE + FRAMEBUFFER_SIZE + (512u << 10);
    uint32_t cache = 8u << 20;
    for (void *p; cache > (1u << 20); cache -= 256u << 10)
        if ((p = memalign(16384, cache + keep))) { free(p); break; }
    if (pg_init(&dc_layout_sfight, cache, VID_EXT_RAM_SIZE) != 0 || dc_romset() != 0) {
        dc_text(1, "the disc lacks a ROM file (dc_layout.h)");
        for (;;) thd_sleep(1000);
    }
    g_mem_window = dc_window;
    if (sound) dc_add_sound_hook();
    /* Sega's own C for STF's hot functions and the COP (gems.h), on by
     * default here: a build without it (no GEMS dir) runs the i960 and our
     * COP. The Dreamcast plays no netplay, so nothing has to agree with it. */
    g_gems_i960 = g_gems_cop = true;
    s_dc_gems = gems_apply(profile_rom_set(g_active_profile));
    char line[128];
    snprintf(line, sizeof line, "profile %s%s, cache %u KB", g_active_profile->id, s_dc_gems ? " +gems" : "", (unsigned)(cache >> 10));
    printf("%s\n", line);
    dp_text(2, line);

    if (!DC_STATS_DBGIO) dbgio_dev_select("null");  /* printf to the serial port was 2% of a fight */
    if (!mem_init(&bus, NULL, 0)) {   /* no map: the i960 would read zeros and fail its COP test */
        dc_text(1, "out of memory for the board's RAM (texture RAM, framebuffer)");
        for (;;) thd_sleep(1000);
    }
    i960_reset(&cpu);
    dc_install_board();
    {   /* what the heap has left once the board is up */
        uint32_t left = 0;
        for (void *p; left < (16u << 20); left += 64u << 10) {
            if (!(p = malloc(left + (64u << 10)))) break;
            free(p);
        }
        snprintf(line, sizeof line, "profile %s%s, cache %u KB, heap %u KB", g_active_profile->id,
                 s_dc_gems ? " +gems" : "", (unsigned)(cache >> 10), (unsigned)(left >> 10));
        printf("%s\n", line);
        dp_text(2, line);
    }
#ifdef IB_WHY
    {   /* calibration: Flycast's SH-4 clock against TMU2, for 1M dt/bf loops and 1M loads */
        static uint32_t arr[16384];
        uint64_t tps = *(volatile uint32_t *)0xFFD80020u + 1u;
        uint32_t n = 1000000u, t0 = IBW_T();
        __asm__ volatile("1: dt %0\n bf 1b" : "+r"(n));
        uint32_t d1 = IBW_D(t0), sum = 0; t0 = IBW_T();
        for (uint32_t i = 0; i < 1000000u; i++) sum += ((volatile uint32_t *)arr)[(i * 7u) & 16383u];
        uint32_t d2 = IBW_D(t0);
        snprintf(g_calib, sizeof g_calib, "calib: loop %u ns, load %u ns (%u)",
                 (unsigned)((uint64_t)d1 * 1000 / tps), (unsigned)((uint64_t)d2 * 1000 / tps), (unsigned)sum);
        printf("%s\n", g_calib);
    }
#endif
#if I960_JIT && IB_JIT_SELFTEST
    jt_run(&cpu, &bus, IB_JIT_SELFTEST, 3);
    thd_sleep(5000);
#endif
    emu_ctx_init(&ctx, &cpu, &bus);
    geo3d_init(&geo);
    ctx.run_state = EMU_RUNNING;

    uint64_t t_last = timer_us_gettime64(), us_slice = 0, us_draw = 0;
    uint32_t f_last = g_emu_frames, slices = 0, loads_last = 0, refills_last = 0, shown = 0, drawn_f = 0;
    uint64_t builds_last = 0, hits_last = 0;
    uint64_t read_last = 0;
    int hashed = 0;
    uint64_t steps_last = 0, ops_last = 0, loop_last = 0, cop_last = 0, aot_last = 0;
    uint64_t us_all = 0, us_dall = 0, n_drawn = 0, us_snd = 0, t_boot = timer_us_gettime64();   /* since boot: slices, draws, sound */
    static char hashed_line[96];
    while (!cpu.halted) {
        dc_pad();
        uint64_t t0 = timer_us_gettime64();
        emu_slice_body(&ctx);
        emu_slice_finish(&ctx);
        uint64_t t1 = timer_us_gettime64();
        /* A board frame not yet shown goes to the PVR when it can take one. */
        if (g_emu_frames - drawn_f >= DC_DRAW_EVERY && dp_frame(&geo, &bus, &rs, &tiles)) { drawn_f = g_emu_frames; shown++; n_drawn++; }
        if (DC_HASH_FRAME && g_emu_frames >= DC_HASH_FRAME && !hashed) {
            uint32_t h = 2166136261u;
            for (uint32_t a = 0x500000u; a < 0x600000u; a += 4) h = (h ^ mem_read32(&bus, a)) * 16777619u;
            for (int r = 0; r < 32; r++) h = (h ^ ((uint32_t *)&cpu.globals)[r]) * 16777619u;
            h = (h ^ (uint32_t)cpu.cycles) * 16777619u;
            snprintf(line, sizeof line, "f%u %08lx ip %lx sl %lu dr %lu/%lu all %lu ms", (unsigned)g_emu_frames,
                     (unsigned long)h, (unsigned long)cpu.sfr.ip, (unsigned long)(us_all / 1000),
                     (unsigned long)(us_dall / 1000), (unsigned long)n_drawn, (unsigned long)((t1 - t_boot) / 1000));
            printf("%s\n", line);
            hashed_line[0] = 0; strncat(hashed_line, line, sizeof hashed_line - 1);
            hashed = 1;
            dp_text(15, hashed_line);
            static char tt_line[96];   /* the draws' parts and the sound, since boot */
            snprintf(tt_line, sizeof tt_line, "ti %lu sc %lu so %lu su %lu snd %lu", (unsigned long)(g_dp.tt_tiles / 1000),
                     (unsigned long)(g_dp.tt_scan / 1000), (unsigned long)(g_dp.tt_sort / 1000),
                     (unsigned long)(g_dp.tt_submit / 1000), (unsigned long)(us_snd / 1000));
            dp_text(14, tt_line);
        }
        {   /* a fixed stretch of the fight (the same frames every run): all of it, its slices, its draws */
            static uint64_t b_t0, b_d0, b_sl, b_p0[5]; static uint32_t b_n0, b_g0[DC_REGIONS + 3]; static char b_line[96];
            uint32_t b_g[DC_REGIONS + 3];   /* page loads: by region, then for dc_rom_at, of the model pack, all */
            memcpy(b_g, g_pg.rg_loads, sizeof g_pg.rg_loads);
            b_g[DC_REGIONS] = g_pg.at_loads; b_g[DC_REGIONS + 1] = g_pg.pak_loads; b_g[DC_REGIONS + 2] = g_pg.loads;
            const uint64_t b_p[5] = { g_dp.tt_tiles, g_dp.tt_scan, g_dp.tt_sort, g_dp.tt_submit, us_snd };
            if (!b_t0 && g_emu_frames > DC_BENCH_F0) { b_t0 = t0; b_d0 = us_dall; b_n0 = (uint32_t)n_drawn; memcpy(b_p0, b_p, sizeof b_p); memcpy(b_g0, b_g, sizeof b_g); }
            if (b_t0 && !b_line[0]) {
                b_sl += t1 - t0;
                if (g_emu_frames >= DC_BENCH_F1) {
                    snprintf(b_line, sizeof b_line, "f%u-%u %lu ms: sl %lu dr %lu", DC_BENCH_F0, (unsigned)g_emu_frames,
                             (unsigned long)((t1 - b_t0) / 1000), (unsigned long)(b_sl / 1000),
                             (unsigned long)((us_dall - b_d0) / 1000));
                    dp_text(3, b_line);
                    static char b_line2[96];   /* the draws' parts in it, and how many were shown */
                    snprintf(b_line2, sizeof b_line2, "ti %lu sc %lu so %lu su %lu snd %lu n %lu",
                             (unsigned long)((b_p[0] - b_p0[0]) / 1000), (unsigned long)((b_p[1] - b_p0[1]) / 1000),
                             (unsigned long)((b_p[2] - b_p0[2]) / 1000), (unsigned long)((b_p[3] - b_p0[3]) / 1000),
                             (unsigned long)((b_p[4] - b_p0[4]) / 1000), (unsigned long)(n_drawn - b_n0));
                    dp_text(4, b_line2);
                    static char b_line3[96];   /* the pager's loads in it: code, data, polygons, textures, pack, rom_at */
                    snprintf(b_line3, sizeof b_line3, "ld %lu: cd %lu da %lu po %lu tx %lu pk %lu at %lu",
                             (unsigned long)(b_g[DC_REGIONS + 2] - b_g0[DC_REGIONS + 2]), (unsigned long)(b_g[0] - b_g0[0]),
                             (unsigned long)(b_g[1] - b_g0[1]), (unsigned long)(b_g[3] - b_g0[3]),
                             (unsigned long)(b_g[4] - b_g0[4]), (unsigned long)(b_g[DC_REGIONS + 1] - b_g0[DC_REGIONS + 1]),
                             (unsigned long)(b_g[DC_REGIONS] - b_g0[DC_REGIONS]));
                    dp_text(5, b_line3);
                }
            }
        }
        uint64_t ts = timer_us_gettime64();
        ds_pump();
        us_snd += timer_us_gettime64() - ts;
        uint64_t t2 = timer_us_gettime64();
        us_slice += t1 - t0;
        us_all += t1 - t0;
        us_draw  += t2 - t1;
        us_dall  += t2 - t1;
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
            snprintf(line, sizeof line, "ld %u (%u ms) flt %u | ev %u pin %u wr %u err %u",
                     (unsigned)loads, (unsigned)read_ms, (unsigned)refills, (unsigned)g_pg.evictions,
                     (unsigned)pg_pinned(), (unsigned)g_pg.rom_writes, (unsigned)g_pg.read_errors);
            STATS_PRINT(line);
            dp_text(1, line);
            snprintf(line, sizeof line, "snd %s codes %u unk %u bgm %d ring %u KB under %u",
                     g_ds.on ? "on" : "off", (unsigned)g_ds.codes, (unsigned)g_ds.unknown,
                     g_ds.bgm == 0xFFFF ? -1 : (int)g_ds.bgm,
                     (unsigned)((g_ds.r_head - g_ds.r_tail) >> 10), (unsigned)g_ds.underruns);
            STATS_PRINT(line);
            dp_text(19, line);   /* the bottom row: the game draws over row 2 */
            snprintf(line, sizeof line, "tris %u runs %u full %u | tex %u new %u drop %u fail %u",
                     g_dp.tris, g_dp.runs, g_dp.faces_dropped, g_dp.count, g_dp.made, g_dp.dropped, g_dp.fails);
            STATS_PRINT(line);
            dp_text(18, line);
            unsigned d = shown ? shown : 1;
            snprintf(line, sizeof line, "tl %u sc %u so %u | mesh %u b %u h %u c %u/%u %uK",
                     (unsigned)(g_dp.us_tiles / 1000 / d), (unsigned)(g_dp.us_scan / 1000 / d),
                     (unsigned)(g_dp.us_sort / 1000 / d), (unsigned)g_geo3d_mesh_count,
                     (unsigned)(g_geo3d_mesh_builds - builds_last), (unsigned)(g_geo3d_mesh_hits - hits_last),
                     g_geo3d_mesh_clears, g_geo3d_mesh_evicts, (unsigned)(g_geo3d_arena_used >> 10));
            STATS_PRINT(line);
            dp_text(17, line);
            unsigned sl = slices ? slices : 1;
            {   /* the i960's loop (the COP's commands inside it), per slice; blocks' share of its steps */
                uint64_t st = g_emu_times.steps - steps_last, ops = g_ib.ops - ops_last;
                const emu_times_t *et = &g_emu_times;
                uint64_t cop = et->cop_timed ? (uint64_t)((double)et->cop_timed_us * (double)et->cop_cmds / (double)et->cop_timed) : 0;
                uint64_t aot = 0;   /* compiled ahead of time (i960_aot.h) */
#if I960_AOT
                aot = g_aot_ops - aot_last; aot_last = g_aot_ops;
#else
                (void)aot_last;
#endif
                snprintf(line, sizeof line, "i960 %u ms (cop %u) /slice, %u%% blk %u%% aot, %u steps",
                         (unsigned)((g_emu_times.loop_us - loop_last) / 1000 / sl), (unsigned)((cop - cop_last) / 1000 / sl),
                         (unsigned)(st ? ops * 100 / st : 0), (unsigned)(st ? aot * 100 / st : 0), (unsigned)(st / sl));
                STATS_PRINT(line);
                dp_text(16, line);
                cop_last = cop; steps_last = g_emu_times.steps; ops_last = g_ib.ops; loop_last = g_emu_times.loop_us;
            }
#ifdef IB_WHY
            snprintf(line, sizeof line, "not blk: slow %u empty %u long %u hor %u irq %u",
                     (unsigned)(g_ib.why[0] / sl), (unsigned)(g_ib.why[1] / sl), (unsigned)(g_ib.why[2] / sl),
                     (unsigned)(g_ib.why[3] / sl), (unsigned)(g_ib.why[4] / sl));
            dp_text(13, line);
            uint64_t tps = *(volatile uint32_t *)0xFFD80020u + 1u;
            snprintf(line, sizeof line, "ms blk %u step %u hook %u (x%u)",
                     (unsigned)(g_ib.ns[0] * 1000 / tps / sl), (unsigned)(g_ib.ns[1] * 1000 / tps / sl),
                     (unsigned)(g_ib.ns[2] * 1000 / tps / sl), (unsigned)(g_ib.why[5] / sl));
            dp_text(12, line);
            {   /* the three blocks that took longest: ip, ms a slice, runs a slice, ops */
                int top[3] = { -1, -1, -1 };
                for (int k = 0; k < 3; k++)
                    for (int x = 0; x < (int)IB_ENTRIES; x++)
                        if (x != top[0] && x != top[1] && (top[k] < 0 || s_ib[x].t > s_ib[top[k]].t)) top[k] = x;
                char *o = line; o += sprintf(o, "top");
                for (int k = 0; k < 3; k++) { const ib_block_t *q = &s_ib[top[k]];
                    o += sprintf(o, " %lx %u/%u/%u", (unsigned long)q->ip, (unsigned)((uint64_t)q->t * 10000 / tps / sl), (unsigned)(q->r / sl), (unsigned)q->n); }
                dp_text(11, line);
                dp_text(10, g_calib);
#if I960_JIT && IB_JIT_SELFTEST
                dp_text(8, g_jt_bench);
                dp_text(7, g_jt_bench2);
#endif
                {   /* the bus slow path: ms a slice and calls, loads and stores; the regions over 1 ms */
                    char *o = line;
                    for (int w = 0; w < 2; w++) {
                        o += sprintf(o, "%s %u/%u", w ? " st" : "bus ld", (unsigned)((uint64_t)g_mslow.t[w] * 1000 / tps / sl), (unsigned)(g_mslow.n[w] / sl));
                        for (int r = 0; r < 16; r++) if ((uint64_t)g_mslow.rt[w][r] * 1000 / tps / sl) o += sprintf(o, " %x:%u", r, (unsigned)((uint64_t)g_mslow.rt[w][r] * 1000 / tps / sl));
                    }
                    dp_text(9, line);
                    memset(&g_mslow, 0, sizeof g_mslow);
                }
                for (int x = 0; x < (int)IB_ENTRIES; x++) s_ib[x].t = s_ib[x].r = 0;
            }
            memset(g_ib.why, 0, sizeof g_ib.why); memset(g_ib.ns, 0, sizeof g_ib.ns);
#endif
#if I960_JIT
            snprintf(line, sizeof line, "jit %u blk %u KB %u fl %u ms %u slow",
                     (unsigned)g_ibj.blocks, (unsigned)(g_ibj.bytes >> 10), (unsigned)g_ibj.flushes,
                     (unsigned)(g_ibj.us_compile / 1000), (unsigned)g_ibj.slow);
            STATS_PRINT(line);
            dp_text(14, line);
#endif
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
