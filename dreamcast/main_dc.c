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
#include <dc/wdt.h>
#include <arch/stack.h>
#include <assert.h>

/* -DDC_HASH_FRAME=n: at board frame n, a hash of work RAM, the i960's
 * registers and its cycle count goes on row 15 (an A/B of two builds). */
/* Draw every Nth board frame (2: every other). The board runs every frame
 * either way; the frames between are never decoded. */
#ifndef DC_DRAW_EVERY
#define DC_DRAW_EVERY 1
#endif
/* At most this many board frames a second (0: as fast as it goes). A Dreamcast
 * never gets there, but Redream's SH-4 runs faster than one and would run the
 * game too fast. */
#ifndef DC_FPS_CAP
#define DC_FPS_CAP 60
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
/* -DDC_LINK=1 (make LINK=1): attract's replay fight, held against MAME over
 * the serial port (dc_link.h). LINK_GEMS=1 keeps Gems on in it. */
#ifndef DC_LINK
#define DC_LINK 0
#endif
#ifndef DC_LINK_GEMS
#define DC_LINK_GEMS 0
#endif
/* -DDC_HUD_MIN=1 (make HUD=min): no stats on screen, only the board's frames
 * a second, small, in the top right corner (the homebrew discs). Boot errors
 * still show. -DDC_HUD_NONE=1 (make HUD=none) drops the counter too: a clean
 * picture to hold against MAME's. */
#ifndef DC_HUD_MIN
#define DC_HUD_MIN 0
#endif
#ifndef DC_HUD_NONE
#define DC_HUD_NONE 0
#endif
/* -DDC_HUD_PROF=1 (make HUD=prof): the full stats and the hardware profile
 * (dc_prof.h): the SH-4's counters, the PVR's times, where the program's time
 * goes by symbol. As a panel to be read back out of a capture of the video
 * (#518): small, tagged, checked lines (hud_line; dreamcast/README.md). */
#ifndef DC_HUD_PROF
#define DC_HUD_PROF 0
#endif

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
#if DC_LINK
#include "dc_link.h"
#endif

KOS_INIT_FLAGS(INIT_IRQ | INIT_CONTROLLER | INIT_CDROM);

#if DC_HUD_PROF
/* HUD=prof's panel, rows of dc_pvr.h's 8x16 font. A line is a two-letter
 * tag, for the window's lines the window's number (four digits), then
 * key=value fields, then a space and the CRC-8 of all that before it in two
 * hex digits (tools/hud_font5x7.py crc8): a reader of the video keeps only
 * lines whose check holds. The top band: LV, every frame drawn; ID, once; the bench (B0-B3) and AO when they come. The bottom band: a 2-s
 * window's numbers, its lines all the same window's. R on the pad shows the
 * top band only, then nothing, then all of it again. Tools/hud_read.py
 * reads it; dreamcast/README.md says what each field is. */
#define HUD_TOP_ROWS 7
enum { HUD_LV, HUD_ID, HUD_B0, HUD_B1, HUD_B2, HUD_B3, HUD_AO,
       HUD_WN = 14, HUD_FT, HUD_CP, HUD_PG, HUD_LD, HUD_RD, HUD_DR, HUD_MS, HUD_TX, HUD_SN,
       HUD_HW, HUD_PV, HUD_G0, HUD_G1, HUD_S0, HUD_S1 };
static uint32_t s_hud_win;   /* the window's number */

static uint8_t hud_crc8(const char *s, int n) {
    uint8_t c = 0;
    for (int i = 0; i < n; i++) {
        c ^= (uint8_t)s[i];
        for (int b = 0; b < 8; b++) c = c & 0x80 ? (uint8_t)(c << 1 ^ 0x07) : (uint8_t)(c << 1);
    }
    return c;
}

/* A panel line: at most 74 characters of text (a longer one ends in '>'),
 * then its check. win: the window's number goes after the tag. */
static void hud_line(int row, const char *tag, int win, const char *body) {
    char l[80];
    int n = win ? snprintf(l, 75, "%s %04u %s", tag, (unsigned)(s_hud_win % 10000u), body)
                : snprintf(l, 75, "%s %s", tag, body);
    if (n > 74) { n = 74; l[73] = '>'; }
    sprintf(l + n, " %02X", hud_crc8(l, n));
    dp_text_row(row, l);
}

/* ms to a tenth, as "%u.%u" */
#define HUD_MS10(us) (unsigned)((us) / 1000u), (unsigned)((us) / 100u % 10u)
#endif

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
    /* The link plays match-replay's profile, the arcade game (dc_link.h). */
    const char *want = DC_LINK && !strcmp(g_pg.lay->profile, "sfight_console") ? "sfight" : g_pg.lay->profile;
    for (size_t i = 0; i < g_profile_count; i++)
        if (!strcmp(g_profiles[i]->id, want)) g_active_profile = g_profiles[i];
    if (!g_active_profile) { printf("no profile %s in this build\n", want); return -1; }
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
#if DC_HUD_PROF
        {   /* R: the whole panel, its top band only, none of it, in turn */
            static int r_was, mode;
            int r = st->rtrig > 128;
            if (r && !r_was) {
                mode = (mode + 1) % 3;
                g_dp.text_hide = mode == 0 ? 0 : mode == 1 ? ~((1u << HUD_TOP_ROWS) - 1) : ~0u;
            }
            r_was = r;
        }
#endif
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
    dp_text_row(row, s);
    dp_text_frame();
}

/* A failed assert, KOS's or ours (dc_fatal), on screen: no serial console in
 * the field. The PVR may be in any state, so the frame buffer is drawn into
 * directly once the last render has had time to land; the watchdog goes off,
 * or it would reset the message away. The return addresses are what KOS's
 * stack walk finds (saved PRs after a call); out/pass2.syms or addr2line on
 * m2hle2.elf names them. */
static void dc_assert(const char *file, int line, const char *expr, const char *msg, const char *func) {
    irq_disable();
    wdt_disable();
    spu_disable();
    for (volatile uint32_t i = 0; i < 20000000u; i++) {}   /* a render in flight lands (~0.1 s) */
    vid_set_mode(DM_640x480, PM_RGB565);
    vid_clear(0, 0, 96);
    char l[64];
    int y = 24;
#define DC_ASSERT_LINE(...) do { snprintf(l, sizeof l, __VA_ARGS__); \
        bfont_draw_str(vram_s + y * 640 + 20, 640, false, l); y += 24; } while (0)
    DC_ASSERT_LINE("m2-hle2 stopped: an assertion failed");
#ifdef DC_GIT
    DC_ASSERT_LINE("build %.13s, board frame %u", DC_GIT, (unsigned)g_emu_frames);
#else
    DC_ASSERT_LINE("board frame %u", (unsigned)g_emu_frames);
#endif
    y += 12;
    DC_ASSERT_LINE("%.50s", expr ? expr : "?");
    if (msg) DC_ASSERT_LINE("%.50s", msg);
    const char *f = file ? strrchr(file, '/') : NULL;
    DC_ASSERT_LINE("%.30s:%d", f ? f + 1 : file ? file : "?", line);
    if (func) DC_ASSERT_LINE("in %.46s", func);
    y += 12;
    DC_ASSERT_LINE("called from:");
    uintptr_t sp, ra[24];
    int n = 0;
    __asm__ volatile("mov r15, %0" : "=r"(sp));
    while (n < 24 && arch_stk_unwind_step(sp, &ra[n], &sp)) n++;
    for (int i = 0; i < n; i += 4) {
        int k = 0;
        for (int j = i; j < n && j < i + 4; j++) k += snprintf(l + k, sizeof l - k, "%08lx ", (unsigned long)ra[j]);
        bfont_draw_str(vram_s + y * 640 + 20, 640, false, l);
        y += 24;
    }
#undef DC_ASSERT_LINE
    for (;;) {}
}

#include "dc_jit_test.h"
#if DC_HUD_PROF
#include "dc_prof.h"
#endif
#ifdef IB_WHY
static char g_calib[64];
#endif

/* The cable's 640x480 mode; with DC_FRAME512, a 512x384 frame in the middle of
 * it: the same signal, the border black. 64 pixels in, and 48 lines (24 a
 * field when interlaced). */
static void dc_video_mode(void) {
    vid_set_mode(DM_640x480, PM_RGB565);
    if (!DC_FRAME512) return;
    vid_mode_t m = *vid_mode;
    m.width   = DC_SCR_W;
    m.height  = DC_SCR_H;
    m.bitmapx += (640 - DC_SCR_W) / 2;
    m.bitmapy += (m.flags & VID_INTERLACE) ? (480 - DC_SCR_H) / 4 : (480 - DC_SCR_H) / 2;
    m.fb_size = DC_SCR_W * DC_SCR_H * 2;
    vid_set_mode_ex(&m);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    dc_video_mode();
    if (dp_init() != 0) { printf("pvr_init failed\n"); for (;;) thd_sleep(1000); }
    dc_text(0, "m2-hle2 for Dreamcast: finding the ROM files");

    /* Sound first: its effects stay in RAM, and it reads the disc through
     * KOS's driver, which the pager forbids once it is up. */
    bool sound = ds_init() == 0;

    /* The COP's sin and cos tables, when the disc has them (SINCOS.BIN,
     * dreamcast/tools/mksincos.py; the link disc does): without them
     * sharc_sincos takes libm's, which part from the board's in the low bits. */
    {
        uint32_t size = 0, fad = pg_find_file("SINCOS.BIN", &size);
        uint32_t *t = fad && size == 0x20000u * 4u ? memalign(32, size) : NULL;
        if (t && cdrom_read_sectors(t, fad, size / 2048) == ERR_OK) g_sharc_sincos = t;
        else free(t);
    }

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
    g_gems_i960 = g_gems_cop = !DC_LINK || DC_LINK_GEMS;
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
#if DC_LINK
    dc_text(1, "link: waiting for the host on the serial port");
    dc_link_init(&bus, s_dc_gems ? "sfight japan gems" : "sfight japan");
    dc_text(1, g_link.on ? "link: on, attract's replay fight against MAME" : "link: nobody answered, running unlinked");
#endif
    i960_reset(&cpu);
    dc_install_board();
    uint32_t left = 0;
    {   /* what the heap has left once the board is up */
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
    if (DC_HUD_MIN) g_dp.text_rows = 0;   /* the boot lines go */
#if DC_HUD_PROF
    dc_prof_init();   /* off: the HW line says so */
    for (int r = 0; r < DC_TEXT_ROWS; r++) dp_text_row(r, "");   /* the boot lines go */
    {   /* the build and the console, once */
#if defined(I960_AOT) && I960_AOT
        const int aot = 1;
#else
        const int aot = 0;
#endif
#ifndef DC_GIT
#define DC_GIT "unknown"
#endif
        static const char *const cable[] = { "vga", "none", "rgb", "cmp" };
        static const char *const region[] = { "unk", "jp", "us", "eu" };
        int cab = vid_check_cable(), rg = flashrom_get_region();
        snprintf(line, sizeof line, "git=%.13s gems=%d aot=%d jit=%d cap=%d cab=%s rg=%s il=%d pal=%d",
                 DC_GIT, s_dc_gems, aot, I960_JIT, DC_FPS_CAP,
                 cab >= 0 && cab < 4 ? cable[cab] : "?", rg >= 0 && rg < 4 ? region[rg] : "?",
                 (vid_mode->flags & VID_INTERLACE) != 0, (vid_mode->flags & VID_PAL) != 0);
        hud_line(HUD_ID, "ID", 0, line);
    }
    uint64_t ft_last = 0, ft_min = ~0ull, ft_max = 0, ft_sum = 0, sl_max = 0, snd_last = 0;
    uint32_t ft_n = 0, ft_33 = 0, ft_50 = 0, ft_dt = 0;
    uint32_t sk_last = 0, rds_last = 0, sec_last = 0, sp_last = 0, at_last = 0, pak_last = 0, txr_last = 0;
    uint64_t dns_last = 0, sns_last = 0, txns_last = 0;
    uint32_t rg_last[DC_REGIONS] = { 0 };
#endif

    uint64_t t_last = timer_us_gettime64(), us_slice = 0, us_draw = 0;
    uint32_t f_last = g_emu_frames, slices = 0, loads_last = 0, refills_last = 0, shown = 0, drawn_f = 0;
    uint64_t builds_last = 0, hits_last = 0;
    uint64_t read_last = 0;
    int hashed = 0;
    uint64_t steps_last = 0, ops_last = 0, loop_last = 0, cop_last = 0, aot_last = 0;
    uint64_t us_all = 0, us_dall = 0, n_drawn = 0, us_snd = 0, t_boot = timer_us_gettime64();   /* since boot: slices, draws, sound */
    static char hashed_line[96];
    /* A hang resets the console: the watchdog is petted once a loop (a board
     * frame and its draw) and by the pager's drive poll, and wraps after
     * 256 ticks of 5.25 ms, 1.34 s. Flycast does not emulate it. */
    assert_set_handler(dc_assert);
    wdt_enable_watchdog(0, WDT_CLK_DIV_4096, WDT_RST_POWER_ON);
    while (!cpu.halted) {
        wdt_pet();
        dc_pad();
        uint64_t t0 = timer_us_gettime64();
        emu_slice_body(&ctx);
        emu_slice_finish(&ctx);
        uint64_t t1 = timer_us_gettime64();
        /* A board frame not yet shown goes to the PVR when it can take one. */
#if DC_HUD_MIN && !DC_HUD_NONE
        {   /* the minimal HUD: board frames a second, top right, over the last second */
            static uint64_t fps_t0;
            static uint32_t fps_f0;
            if (!fps_t0) { fps_t0 = t1; fps_f0 = g_emu_frames; dp_corner("-- fps"); }
            else if (t1 - fps_t0 >= 1000000) {
                char fc[16];
                uint32_t tenths = (uint32_t)(((uint64_t)(g_emu_frames - fps_f0) * 10000000u + (t1 - fps_t0) / 2) / (t1 - fps_t0));
                snprintf(fc, sizeof fc, "%u.%u fps", (unsigned)(tenths / 10), (unsigned)(tenths % 10));
                dp_corner(fc);
                fps_t0 = t1; fps_f0 = g_emu_frames;
            }
        }
#endif
#if DC_HUD_PROF
        if (g_emu_frames - drawn_f >= DC_DRAW_EVERY) {   /* this frame's numbers, in it */
            snprintf(line, sizeof line, "f=%u d=%u v=%u t=%u dt=%u.%u", (unsigned)g_emu_frames, (unsigned)(n_drawn + 1),
                     (unsigned)pvr_get_vbl_count(), (unsigned)((t1 - t_boot) / 1000), HUD_MS10(ft_dt));
            hud_line(HUD_LV, "LV", 0, line);
        }
#endif
        if (g_emu_frames - drawn_f >= DC_DRAW_EVERY && dp_frame(&geo, &bus, &rs, &tiles)) {
            drawn_f = g_emu_frames; shown++; n_drawn++;
#if DC_HUD_PROF
            {   /* the time from the last frame handed to the PVR to this one */
                uint64_t now = timer_us_gettime64();
                if (ft_last) {
                    uint64_t dt = now - ft_last;
                    ft_dt = (uint32_t)dt;
                    if (dt < ft_min) ft_min = dt;
                    if (dt > ft_max) ft_max = dt;
                    ft_sum += dt; ft_n++;
                    ft_33 += dt > 33400; ft_50 += dt > 50100;
                }
                ft_last = now;
            }
#endif
        }
#if DC_FPS_CAP
        {   /* wait while the board is ahead of the clock; behind by over 0.1 s, the clock starts again */
            static uint64_t cap_t0;
            static uint32_t cap_f0;
            uint64_t now = timer_us_gettime64();
            uint64_t due = cap_t0 + (uint64_t)(g_emu_frames - cap_f0) * 1000000u / DC_FPS_CAP;
            if (!cap_t0 || now > due + 100000u) { cap_t0 = now; cap_f0 = g_emu_frames; }
            else {   /* asleep for all but the last millisecond, which a wake-up can overshoot */
                if (due > now + 2000u) thd_sleep((unsigned)((due - now) / 1000u) - 1u);
                while (timer_us_gettime64() < due) thd_pass();
            }
        }
#endif
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
            snprintf(tt_line, sizeof tt_line, "ti %lu sc %lu so %lu su %lu snd %lu tx %lu", (unsigned long)(g_dp.tt_tiles / 1000),
                     (unsigned long)(g_dp.tt_scan / 1000), (unsigned long)(g_dp.tt_sort / 1000),
                     (unsigned long)(g_dp.tt_submit / 1000), (unsigned long)(us_snd / 1000),
                     (unsigned long)(g_dp.tt_tex / 1000));
            printf("%s\n", tt_line);
            dp_text(14, tt_line);
        }
        {   /* a fixed stretch of the fight (the same frames every run): all of it, its slices, its draws */
            static uint64_t b_t0, b_d0, b_sl, b_p0[5], b_tx0; static uint32_t b_rd0; static uint32_t b_n0, b_g0[DC_REGIONS + 3]; static char b_line[96];
            static uint32_t b_dr0, b_sk0, b_sp0;
            uint32_t b_g[DC_REGIONS + 3];   /* page loads: by region, then for dc_rom_at, of the model pack, all */
            memcpy(b_g, g_pg.rg_loads, sizeof g_pg.rg_loads);
            b_g[DC_REGIONS] = g_pg.at_loads; b_g[DC_REGIONS + 1] = g_pg.pak_loads; b_g[DC_REGIONS + 2] = g_pg.loads;
            const uint64_t b_p[5] = { g_dp.tt_tiles, g_dp.tt_scan, g_dp.tt_sort, g_dp.tt_submit, us_snd };
            if (!b_t0 && g_emu_frames > DC_BENCH_F0) { b_t0 = t0; b_d0 = us_dall; b_n0 = (uint32_t)n_drawn; memcpy(b_p0, b_p, sizeof b_p); memcpy(b_g0, b_g, sizeof b_g); b_tx0 = g_dp.tt_tex; b_rd0 = g_pg.tx_reads;
                b_dr0 = g_pg.reads; b_sk0 = g_pg.seeks; b_sp0 = g_pg.sp_loads; }
            if (b_t0 && !b_line[0]) {
                b_sl += t1 - t0;
                if (g_emu_frames >= DC_BENCH_F1) {
                    snprintf(b_line, sizeof b_line, "f%u-%u %lu ms: sl %lu dr %lu tx %lu/%lu", DC_BENCH_F0, (unsigned)g_emu_frames,
                             (unsigned long)((t1 - b_t0) / 1000), (unsigned long)(b_sl / 1000),
                             (unsigned long)((us_dall - b_d0) / 1000), (unsigned long)((g_dp.tt_tex - b_tx0) / 1000),
                             (unsigned long)(g_pg.tx_reads - b_rd0));
                    printf("%s\n", b_line);
#if DC_HUD_PROF
                    snprintf(line, sizeof line, "f=%u-%u ms=%lu sl=%lu dr=%lu tx=%lu txr=%lu n=%lu", DC_BENCH_F0, (unsigned)g_emu_frames,
                             (unsigned long)((t1 - b_t0) / 1000), (unsigned long)(b_sl / 1000),
                             (unsigned long)((us_dall - b_d0) / 1000), (unsigned long)((g_dp.tt_tex - b_tx0) / 1000),
                             (unsigned long)(g_pg.tx_reads - b_rd0), (unsigned long)(n_drawn - b_n0));
                    hud_line(HUD_B0, "B0", 0, line);
                    snprintf(line, sizeof line, "ti=%lu sc=%lu so=%lu su=%lu snd=%lu",
                             (unsigned long)((b_p[0] - b_p0[0]) / 1000), (unsigned long)((b_p[1] - b_p0[1]) / 1000),
                             (unsigned long)((b_p[2] - b_p0[2]) / 1000), (unsigned long)((b_p[3] - b_p0[3]) / 1000),
                             (unsigned long)((b_p[4] - b_p0[4]) / 1000));
                    hud_line(HUD_B1, "B1", 0, line);
                    snprintf(line, sizeof line, "ld=%lu cd=%lu da=%lu po=%lu tx=%lu pk=%lu at=%lu",
                             (unsigned long)(b_g[DC_REGIONS + 2] - b_g0[DC_REGIONS + 2]), (unsigned long)(b_g[0] - b_g0[0]),
                             (unsigned long)(b_g[1] - b_g0[1]), (unsigned long)(b_g[3] - b_g0[3]),
                             (unsigned long)(b_g[4] - b_g0[4]), (unsigned long)(b_g[DC_REGIONS + 1] - b_g0[DC_REGIONS + 1]),
                             (unsigned long)(b_g[DC_REGIONS] - b_g0[DC_REGIONS]));
                    hud_line(HUD_B2, "B2", 0, line);
                    snprintf(line, sizeof line, "rd=%lu sk=%lu sp=%lu", (unsigned long)(g_pg.reads - b_dr0),
                             (unsigned long)(g_pg.seeks - b_sk0), (unsigned long)(g_pg.sp_loads - b_sp0));
                    hud_line(HUD_B3, "B3", 0, line);
#else
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
                    static char b_line4[64];   /* commands to the drive, the seeks among them, strip pack pages */
                    snprintf(b_line4, sizeof b_line4, "rd %lu sk %lu sp %lu", (unsigned long)(g_pg.reads - b_dr0),
                             (unsigned long)(g_pg.seeks - b_sk0), (unsigned long)(g_pg.sp_loads - b_sp0));
                    printf("%s | %s\n", b_line3, b_line4);
                    dp_text(6, b_line4);
#endif
                }
            }
        }
        uint64_t ts = timer_us_gettime64();
        ds_pump();
        us_snd += timer_us_gettime64() - ts;
        uint64_t t2 = timer_us_gettime64();
        us_slice += t1 - t0;
        us_all += t1 - t0;
#if DC_HUD_PROF
        if (t1 - t0 > sl_max) sl_max = t1 - t0;
#endif
        us_draw  += t2 - t1;
        us_dall  += t2 - t1;
        slices++;
        if (t2 - t_last >= 2000000) {
            uint32_t fr = g_emu_frames - f_last;
            double sec = (double)(t2 - t_last) / 1e6;
            (void)sec;
            uint32_t loads = g_pg.loads - loads_last, refills = g_pg.refills - refills_last;
            uint32_t read_ms = (uint32_t)((g_pg.read_ns - read_last) / 1000000);
#if DC_HUD_PROF
            {   /* the window's lines (HUD_WN on), all of the same window */
                s_hud_win++;
                uint64_t us = t2 - t_last;
                unsigned d = shown ? shown : 1, sl = slices ? slices : 1;
                unsigned fps10 = (unsigned)((uint64_t)fr * 10000000u / us), sh10 = (unsigned)((uint64_t)shown * 10000000u / us);
                snprintf(line, sizeof line, "f=%u fps=%u.%u sh=%u.%u sl=%u 3d=%u+%u",
                         (unsigned)g_emu_frames, fps10 / 10, fps10 % 10, sh10 / 10, sh10 % 10,
                         (unsigned)(us_slice / 1000 / sl), (unsigned)(g_dp.us_decode / 1000 / d),
                         (unsigned)(g_dp.us_submit / 1000 / d));
                hud_line(HUD_WN, "WN", 1, line);
                /* frames to the PVR: their spacing, those late by a field or two; the slowest slice; the sound's pump */
                uint64_t ft_avg = ft_n ? ft_sum / ft_n : 0;
                snprintf(line, sizeof line, "ft=%u.%u/%u.%u/%u.%u s33=%u s50=%u slx=%u snd=%u",
                         HUD_MS10(ft_n ? ft_min : 0), HUD_MS10(ft_avg), HUD_MS10(ft_max), (unsigned)ft_33, (unsigned)ft_50,
                         (unsigned)(sl_max / 1000), (unsigned)((us_snd - snd_last) / 1000));
                hud_line(HUD_FT, "FT", 1, line);
                ft_min = ~0ull; ft_max = ft_sum = 0; ft_n = ft_33 = ft_50 = 0; sl_max = 0; snd_last = us_snd;
                {   /* the i960's loop (the COP's commands inside it), per slice; blocks' and AOT's share of its steps */
                    uint64_t st = g_emu_times.steps - steps_last, ops = g_ib.ops - ops_last;
                    const emu_times_t *et = &g_emu_times;
                    uint64_t cop = et->cop_timed ? (uint64_t)((double)et->cop_timed_us * (double)et->cop_cmds / (double)et->cop_timed) : 0;
                    uint64_t aot = 0;
#if I960_AOT
                    aot = g_aot_ops - aot_last; aot_last = g_aot_ops;
#else
                    (void)aot_last;
#endif
                    snprintf(line, sizeof line, "i960=%u cop=%u blk=%u aot=%u st=%u",
                             (unsigned)((g_emu_times.loop_us - loop_last) / 1000 / sl), (unsigned)((cop - cop_last) / 1000 / sl),
                             (unsigned)(st ? ops * 100 / st : 0), (unsigned)(st ? aot * 100 / st : 0), (unsigned)(st / sl));
                    hud_line(HUD_CP, "CP", 1, line);
                    cop_last = cop; steps_last = g_emu_times.steps; ops_last = g_ib.ops; loop_last = g_emu_times.loop_us;
#if I960_AOT && !I960_JIT
                    if (aot_off_why()[0]) {   /* the compiled code refused: say why (#509) */
                        snprintf(line, sizeof line, "off=%s", aot_off_why());
                        hud_line(HUD_AO, "AO", 0, line);
                    }
#endif
#if I960_JIT
                    snprintf(line, sizeof line, "jit=%u kb=%u fl=%u ms=%u slow=%u",
                             (unsigned)g_ibj.blocks, (unsigned)(g_ibj.bytes >> 10), (unsigned)g_ibj.flushes,
                             (unsigned)(g_ibj.us_compile / 1000), (unsigned)g_ibj.slow);
                    hud_line(HUD_AO, "JT", 0, line);
#endif
                }
                /* the pager: page loads (their time), faults; since boot, evictions, pinned, ROM writes,
                 * errors; its cache's KB, the heap's left at boot */
                snprintf(line, sizeof line, "ld=%u ms=%u flt=%u ev=%u pin=%u wr=%u err=%u c=%u h=%u",
                         (unsigned)loads, (unsigned)read_ms, (unsigned)refills, (unsigned)g_pg.evictions,
                         (unsigned)pg_pinned(), (unsigned)g_pg.rom_writes, (unsigned)g_pg.read_errors,
                         (unsigned)(cache >> 10), (unsigned)(left >> 10));
                hud_line(HUD_PG, "PG", 1, line);
                /* the loads by what they were for: code, data, polygons, textures, model pack, dc_rom_at, strip pack */
                snprintf(line, sizeof line, "cd=%u da=%u po=%u tx=%u pk=%u at=%u sp=%u",
                         (unsigned)(g_pg.rg_loads[0] - rg_last[0]), (unsigned)(g_pg.rg_loads[1] - rg_last[1]),
                         (unsigned)(g_pg.rg_loads[3] - rg_last[3]), (unsigned)(g_pg.rg_loads[4] - rg_last[4]),
                         (unsigned)(g_pg.pak_loads - pak_last), (unsigned)(g_pg.at_loads - at_last),
                         (unsigned)(g_pg.sp_loads - sp_last));
                hud_line(HUD_LD, "LD", 1, line);
                memcpy(rg_last, g_pg.rg_loads, sizeof rg_last);
                pak_last = g_pg.pak_loads; at_last = g_pg.at_loads; sp_last = g_pg.sp_loads;
                /* the drive: commands, seeks, KB; their ms, the seeks' ms, the longest; the texture pack's reads, ms */
                snprintf(line, sizeof line, "rd=%u sk=%u kb=%u ms=%u skms=%u max=%u.%u txr=%u txms=%u",
                         (unsigned)(g_pg.reads - rds_last), (unsigned)(g_pg.seeks - sk_last),
                         (unsigned)((g_pg.sectors - sec_last) * 2u), (unsigned)((g_pg.drive_ns - dns_last) / 1000000u),
                         (unsigned)((g_pg.seek_ns - sns_last) / 1000000u), HUD_MS10(g_pg.drive_max_ns / 1000u),
                         (unsigned)(g_pg.tx_reads - txr_last), (unsigned)((g_pg.tx_read_ns - txns_last) / 1000000u));
                hud_line(HUD_RD, "RD", 1, line);
                rds_last = g_pg.reads; sk_last = g_pg.seeks; sec_last = g_pg.sectors; dns_last = g_pg.drive_ns;
                sns_last = g_pg.seek_ns; g_pg.drive_max_ns = 0; txr_last = g_pg.tx_reads; txns_last = g_pg.tx_read_ns;
                /* a drawn frame's parts (ms): tiles, scan, sort; its triangles, runs, faces dropped for a full list */
                snprintf(line, sizeof line, "tl=%u sc=%u so=%u tri=%u run=%u full=%u",
                         (unsigned)(g_dp.us_tiles / 1000 / d), (unsigned)(g_dp.us_scan / 1000 / d),
                         (unsigned)(g_dp.us_sort / 1000 / d), g_dp.tris, g_dp.runs, g_dp.faces_dropped);
                hud_line(HUD_DR, "DR", 1, line);
                /* the mesh cache: meshes, built and hit in the window, clears/evictions, arena KB */
                snprintf(line, sizeof line, "n=%u b=%u h=%u clr=%u ev=%u kb=%u",
                         (unsigned)g_geo3d_mesh_count, (unsigned)(g_geo3d_mesh_builds - builds_last),
                         (unsigned)(g_geo3d_mesh_hits - hits_last), (unsigned)g_geo3d_mesh_clears,
                         (unsigned)g_geo3d_mesh_evicts, (unsigned)(g_geo3d_arena_used >> 10));
                hud_line(HUD_MS, "MS", 1, line);
                /* textures: the pack's hits of all lookups; slots, made, dropped, failed */
                snprintf(line, sizeof line, "pk=%u/%u tex=%u new=%u drop=%u fail=%u",
                         (unsigned)g_dp.tx_hits, (unsigned)(g_dp.tx_hits + g_dp.tx_miss), g_dp.count, g_dp.made,
                         g_dp.dropped, g_dp.fails);
                hud_line(HUD_TX, "TX", 1, line);
                snprintf(line, sizeof line, "on=%d codes=%u unk=%u bgm=%d ring=%u und=%u",
                         g_ds.on, (unsigned)g_ds.codes, (unsigned)g_ds.unknown, g_ds.bgm == 0xFFFF ? -1 : (int)g_ds.bgm,
                         (unsigned)((g_ds.r_head - g_ds.r_tail) >> 10), (unsigned)g_ds.underruns);
                hud_line(HUD_SN, "SN", 1, line);
                static char pl[DC_PROF_LINES][80];
                static const char *const tag[DC_PROF_LINES] = { "HW", "PV", "G0", "G1", "S0", "S1" };
                dc_prof_report(us, pl);
                for (int i = 0; i < DC_PROF_LINES; i++) {
                    STATS_PRINT(pl[i]);
                    if (pl[i][0]) hud_line(HUD_HW + i, tag[i], 1, pl[i]);
                    else dp_text_row(HUD_HW + i, "");
                }
            }
#else
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
            snprintf(line, sizeof line, "pk %u/%u rd %u %ums | tex %u new %u drop %u fail %u | tris %u runs %u full %u",
                     (unsigned)g_dp.tx_hits, (unsigned)(g_dp.tx_hits + g_dp.tx_miss), (unsigned)g_pg.tx_reads,
                     (unsigned)(g_pg.tx_read_ns / 1000000), g_dp.count, g_dp.made, g_dp.dropped, g_dp.fails,
                     g_dp.tris, g_dp.runs, g_dp.faces_dropped);
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
#if I960_AOT && !I960_JIT
                if (aot_off_why()[0]) {   /* the compiled code refused: say why (#509) */
                    snprintf(line, sizeof line, "aot off: %s", aot_off_why());
                    dp_text(14, line);
                }
#endif
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
#endif   /* DC_HUD_PROF */
            builds_last = g_geo3d_mesh_builds; hits_last = g_geo3d_mesh_hits;
            g_dp.us_tiles = g_dp.us_scan = g_dp.us_sort = 0;
            t_last = t2; f_last = g_emu_frames; us_slice = us_draw = 0; slices = 0; shown = 0;
            g_dp.us_decode = g_dp.us_submit = 0; g_dp.made = g_dp.dropped = 0;
            loads_last = g_pg.loads; refills_last = g_pg.refills; read_last = g_pg.read_ns;
        }
    }
    wdt_disable();   /* the reason stays on screen */
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
