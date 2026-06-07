/*
 * profiles/fvipers.h — Fighting Vipers (fvipers Rev D) profile.
 *
 * MAME ROM set: fvipers (parent — self-contained, no fallback zip).
 * Board: Model 2B CRX — same hardware as Sonic The Fighters (its successor).
 * ROM filenames and CRCs sourced from MAME src/mame/sega/model2.cpp.
 *
 * main_data layout differs from STF: two independently-mirrored 512KB
 * windows at 0x1000000 and 0x1800000, each tiled 8× across 0x100000 slots.
 */
#ifndef PROFILES_FVIPERS_H
#define PROFILES_FVIPERS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "log.h"
#include "rom_loader.h"
#include "game_profile.h"
#include "memory.h"
#include "i960.h"
#include "hle_hooks.h"
#include "input.h"

/* ---- Hook addresses ----------------------------------------------------- */

#define FVIPERS_HOOK_ADDR_COP_INIT_L1       0x0000190C
#define FVIPERS_HOOK_ADDR_CHECK_TIMER_4     0x0004A88C
#define FVIPERS_HOOK_ADDR_INTERRUPT_WAIT    0x00002238
#define FVIPERS_HOOK_ADDR_INTERRUPT_WAIT_B       0x0001184C
#define FVIPERS_HOOK_ADDR_INTERRUPT_WAIT_B_SPIN  0x000118DC
#define FVIPERS_HOOK_ADDR_FRAME_PACE        0x00011C80
#define FVIPERS_HOOK_ADDR_READ_SW           0x0000229C
#define FVIPERS_HOOK_ADDR_CLIP_YOKO         0x000236BC
#define FVIPERS_HOOK_ADDR_CLIP_3D           0x00023784

/* TODO: find in IDA */
#define FVIPERS_HOOK_ADDR_CHECK_TIMER_SPIN  0xFFFFFFFF  /* TODO: STF 0x0004A58C */
#define FVIPERS_HOOK_ADDR_IDLE              0xFFFFFFFF  /* TODO: STF 0x00011610 */
#define FVIPERS_HOOK_ADDR_700000_LOOP       0xFFFFFFFF  /* TODO: STF 0x00007264 */
#define FVIPERS_HOOK_ADDR_COP_ERR_HANG      0xFFFFFFFF  /* TODO: STF 0x000077F8 */

/* VsyncScr: tile/sprite layer update called once per vsync (STF: 0x00000C40). */
#define FVIPERS_VSYNC_SCR_ADDR  0x000013A0

/* Return address after the interrupt_wait spin loop — first instruction after
 * both back-edges at 0x2240 and 0x2244 (confirmed via IDA). */
#define FVIPERS_INTERRUPT_WAIT_RETURN  0x00002248

/* Timer interrupt flag byte zeroed by check_timer_4_spin (STF: 0x0050008C). */
#define FVIPERS_TIMER_FLAG_ADDR  0xFFFFFFFF  /* TODO */

/* prev_held address zeroed by read_sw — confirmed via IDA (INTERUPT_FLAGS_HELD). */
#define FVIPERS_PREV_HELD_ADDR  0x00500700

/* ---- ROM loader --------------------------------------------------------- */

#define FVIPERS_LOAD_PAIR_32(dest, dest_sz, lo, crc_lo, hi, crc_hi, half_sz, offset) \
    do { \
        tmp1 = zip_extract_from_set(child, parent, lo, &fsize, crc_lo); \
        tmp2 = zip_extract_from_set(child, parent, hi, &fsize, crc_hi); \
        if (!tmp1 || !tmp2) { ok = 0; goto done; } \
        interleave_32_word(dest, dest_sz, tmp1, half_sz, tmp2, half_sz, offset); \
        free(tmp1); tmp1 = NULL; \
        free(tmp2); tmp2 = NULL; \
    } while(0)

#define FVIPERS_LOAD_SAMPLE(dest, dest_sz, name, crc, offset) \
    do { \
        tmp1 = zip_extract_from_set(child, parent, name, &fsize, crc); \
        if (tmp1) { \
            load_16_word_swap(dest, dest_sz, tmp1, fsize, offset); \
            free(tmp1); tmp1 = NULL; \
        } \
    } while(0)

static inline int fvipers_load(romset_t *rs, const char *child, const char *parent) {
    romset_free(rs);

    size_t   fsize = 0;
    uint8_t *tmp1 = NULL, *tmp2 = NULL;
    int      ok = 1;

    /* maincpu (2MB region — 4 files interleaved into two 256KB windows) */
    rs->maincpu_size = 0x200000;
    rs->maincpu = (uint8_t *)calloc(1, rs->maincpu_size);
    FVIPERS_LOAD_PAIR_32(rs->maincpu, rs->maincpu_size,
        "epr-18606d.15", 0x7334de7d, "epr-18607d.16", 0x700d2ade, 0x20000, 0x000000);
    FVIPERS_LOAD_PAIR_32(rs->maincpu, rs->maincpu_size,
        "epr-18604d.13", 0x704fdfcf, "epr-18605d.14", 0x7dddf81f, 0x20000, 0x040000);

    /* main_data (32MB) */
    rs->main_data_size = 0x2000000;
    rs->main_data = (uint8_t *)calloc(1, rs->main_data_size);
    FVIPERS_LOAD_PAIR_32(rs->main_data, rs->main_data_size,
        "mpr-18614.11", 0x0ebc899f, "mpr-18615.12", 0x018abdb7, 0x400000, 0x0000000);
    FVIPERS_LOAD_PAIR_32(rs->main_data, rs->main_data_size,
        "mpr-18612.9",  0x1f174cd1, "mpr-18613.10", 0xf057cdf2, 0x400000, 0x0800000);
    /* 512KB pair at 0x1000000, mirrored across 0x1100000–0x1700000 */
    FVIPERS_LOAD_PAIR_32(rs->main_data, rs->main_data_size,
        "epr-18610d.7", 0xa1871703, "epr-18611d.8", 0x39a75fee, 0x80000, 0x1000000);
    for (uint32_t d = 0x1100000; d <= 0x1700000; d += 0x100000)
        rom_region_copy(rs->main_data, rs->main_data_size, 0x1000000, d, 0x100000);
    /* 512KB pair at 0x1800000, mirrored across 0x1900000–0x1f00000 */
    FVIPERS_LOAD_PAIR_32(rs->main_data, rs->main_data_size,
        "epr-18608d.5", 0x5bc11881, "epr-18609d.6", 0xcd426035, 0x80000, 0x1800000);
    for (uint32_t d = 0x1900000; d <= 0x1f00000; d += 0x100000)
        rom_region_copy(rs->main_data, rs->main_data_size, 0x1800000, d, 0x100000);

    /* copro_data (8MB) */
    rs->copro_data_size = 0x800000;
    rs->copro_data = (uint8_t *)calloc(1, rs->copro_data_size);
    FVIPERS_LOAD_PAIR_32(rs->copro_data, rs->copro_data_size,
        "mpr-18622.29", 0xc74d99e3, "mpr-18623.30", 0x746ae931, 0x200000, 0x000000);

    /* polygons (16MB — 3 pairs filling 12MB) */
    rs->polygons_size = 0x1000000;
    rs->polygons = (uint8_t *)calloc(1, rs->polygons_size);
    FVIPERS_LOAD_PAIR_32(rs->polygons, rs->polygons_size,
        "mpr-18616.17", 0x15a239be, "mpr-18619.21", 0x9d5e8e2b, 0x200000, 0x000000);
    FVIPERS_LOAD_PAIR_32(rs->polygons, rs->polygons_size,
        "mpr-18617.18", 0xa62cab7d, "mpr-18620.22", 0x4d432afd, 0x200000, 0x400000);
    FVIPERS_LOAD_PAIR_32(rs->polygons, rs->polygons_size,
        "mpr-18618.19", 0xadab589f, "mpr-18621.23", 0xf5eeaa95, 0x200000, 0x800000);

    /* textures (16MB — 2 pairs; 0x400000–0x7FFFFF unpopulated) */
    rs->textures_size = 0x1000000;
    rs->textures = (uint8_t *)calloc(1, rs->textures_size);
    FVIPERS_LOAD_PAIR_32(rs->textures, rs->textures_size,
        "mpr-18626.27", 0x9df0a961, "mpr-18624.25", 0x1d74433e, 0x200000, 0x000000);
    FVIPERS_LOAD_PAIR_32(rs->textures, rs->textures_size,
        "mpr-18627.28", 0x946175a0, "mpr-18625.26", 0x182fd572, 0x200000, 0x800000);

    /* audiocpu (512KB) — byte-swapped on load */
    rs->audiocpu_size = 0x80000;
    rs->audiocpu = (uint8_t *)calloc(1, rs->audiocpu_size);
    tmp1 = zip_extract_from_set(child, parent, "epr-18628.31", &fsize, 0xaa7dd79f);
    if (!tmp1) { ok = 0; goto done; }
    load_16_word_swap(rs->audiocpu, rs->audiocpu_size, tmp1, fsize, 0);
    free(tmp1); tmp1 = NULL;

    /* samples (8MB) */
    rs->samples_size = 0x800000;
    rs->samples = (uint8_t *)calloc(1, rs->samples_size);
    FVIPERS_LOAD_SAMPLE(rs->samples, rs->samples_size, "mpr-18629.32", 0x5d0006cc, 0x000000);
    FVIPERS_LOAD_SAMPLE(rs->samples, rs->samples_size, "mpr-18630.33", 0x9d405615, 0x200000);
    FVIPERS_LOAD_SAMPLE(rs->samples, rs->samples_size, "mpr-18631.34", 0x9dae5b45, 0x400000);
    FVIPERS_LOAD_SAMPLE(rs->samples, rs->samples_size, "mpr-18632.35", 0x39da6805, 0x600000);

    rs->loaded = true;
    LOG_INFO("fvipers: ROM set loaded (maincpu=%zu main_data=%zu copro=%zu poly=%zu tex=%zu audio=%zu samp=%zu)",
             rs->maincpu_size, rs->main_data_size, rs->copro_data_size,
             rs->polygons_size, rs->textures_size, rs->audiocpu_size, rs->samples_size);

done:
    free(tmp1);
    free(tmp2);
    if (!ok) { LOG_ERROR("fvipers: ROM set load failed"); romset_free(rs); return -1; }
    return 0;
}

#undef FVIPERS_LOAD_PAIR_32
#undef FVIPERS_LOAD_SAMPLE

/* ---- Installer ---------------------------------------------------------- */

static inline uint32_t fvipers_read32(const uint8_t *buf, uint32_t off) {
    return  (uint32_t)buf[off]
         | ((uint32_t)buf[off + 1] << 8)
         | ((uint32_t)buf[off + 2] << 16)
         | ((uint32_t)buf[off + 3] << 24);
}

static inline void fvipers_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!rs->loaded) { LOG_ERROR("fvipers_install: romset not loaded"); return; }

    i960_reset(cpu);
    mem_init(bus, rs->maincpu, rs->maincpu_size);

    if (rs->main_data && bus->main_data) {
        size_t n = rs->main_data_size < MAIN_DATA_SIZE ? rs->main_data_size : MAIN_DATA_SIZE;
        memcpy(bus->main_data, rs->main_data, n);
        LOG_INFO("fvipers_install: copied %zu bytes to MAIN_DATA", n);
    }

    /* XTRA_DATA at 0x06000000: map main_data from offset 0 (not 0x01000000 as in STF).
     * bg_col_set reads a 0xFFFF-terminated color table at bus 0x06CE064C =
     * main_data[0xCE064C], which lives in the first 4MB pair (mpr-18614/18615). */
    if (rs->main_data && bus->xtra_data) {
        const uint32_t src_off = 0x01000000;
        if (src_off < rs->main_data_size) {
            size_t avail = rs->main_data_size - src_off;
            size_t n = avail < XTRA_DATA_SIZE ? avail : XTRA_DATA_SIZE;
            memcpy(bus->xtra_data, rs->main_data + src_off, n);
            LOG_INFO("fvipers_install: mirrored %zu bytes ROM[0x%08X..] → XTRA_DATA", n, src_off);
        }
    }

    uint32_t sat_ptr  = fvipers_read32(rs->maincpu, 0x00);
    uint32_t prcb_ptr = fvipers_read32(rs->maincpu, 0x04);
    LOG_INFO("fvipers_install: SAT=0x%08X  PRCB=0x%08X", sat_ptr, prcb_ptr);
    if (prcb_ptr + 0x2C < rs->maincpu_size) {
        uint32_t start_ip_ptr = fvipers_read32(rs->maincpu, prcb_ptr + PRCB_START_IP);
        if (start_ip_ptr < rs->maincpu_size) {
            cpu->sfr.ip = fvipers_read32(rs->maincpu, start_ip_ptr);
        }
    }
    LOG_INFO("fvipers_install: initial IP = 0x%08X", cpu->sfr.ip);
}

/* ---- HLE hook functions ------------------------------------------------- */

/* cop_initialize_l1 (0x190C): boot-time COP-ready spin.
 * Sets the ready bit so the loop exits on its next iteration. */
static int fvipers_hook_cop_init_l1(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    uint32_t cur = mem_read32(bus, COPRO_CONTROL1_BASE + 4);
    mem_write32(bus, COPRO_CONTROL1_BASE + 4, cur | 0x01);
    return 1;
}

/* check_timer_4 (0x4A88C): spin loop waiting for timer interrupt.
 * Skip the whole function — return 0 to caller. */
static int fvipers_hook_check_timer_4(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    cpu->globals.g[0] = 0;
    hle_ret(cpu);
    return 0;
}

/* check_timer_4_spin: inner polling loop — write 0x01 so it exits.
 * Address TODO; implement mirrors STF 0x0004A58C. */
static int fvipers_hook_check_timer_4_spin(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    mem_write8(bus, FVIPERS_TIMER_FLAG_ADDR, 0x01);
    return 1;
}

/* interrupt_wait (0x2238): inject VsyncScr then skip the spin loop.
 * FVIPERS_INTERRUPT_WAIT_RETURN must be set to the instruction after the loop. */
static int fvipers_hook_interrupt_wait(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    hle_call(cpu, FVIPERS_VSYNC_SCR_ADDR, FVIPERS_INTERRUPT_WAIT_RETURN);
    return 0;
}

/* interrupt_wait_b (0x1184C): frame-timing preamble — let it execute naturally. */
static int fvipers_hook_interrupt_wait_b(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu; (void)bus;
    return 1;
}

/* interrupt_wait_b spin (0x118DC): change-detection loop on byte_500000.
 * g0 holds the snapshot taken at 0x118D4; we write g0^1 so the cmpibe
 * immediately falls through — simulating one vsync toggle. */
static int fvipers_hook_interrupt_wait_b_spin(i960_cpu_t *cpu, memory_bus_t *bus) {
    uint32_t snap = cpu->globals.g[0];
    mem_write8(bus, RAM_BASE, snap ^ 1);
    return 1;
}

/* _idle: inject VsyncScr on first entry; let ldob RAM_BASE execute on second.
 * Address TODO; implement mirrors STF 0x00011610. */
static int fvipers_hook_idle(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    static int s_vsync_fired = 0;
    if (!s_vsync_fired) {
        s_vsync_fired = 1;
        hle_call(cpu, FVIPERS_VSYNC_SCR_ADDR, FVIPERS_HOOK_ADDR_IDLE);
        return 0;
    }
    s_vsync_fired = 0;
    return 1;
}

/* _700000_loop: sound-init delay loop — zero r3 so cmpdeco exits.
 * Address TODO; implement mirrors STF 0x00007264. */
static int fvipers_hook_700000_loop(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    cpu->locals.r[3] = 0;
    return 1;
}

/* variable_diff_calc (0x11C80): fires once per game frame at end of main_loop.
 * Snapshots the geo capture ring boundary and signals the emu thread. */
static int fvipers_hook_frame_pace(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu; (void)bus;
    g_cop.geo_frame_start = g_cop.geo_frame_end;
    g_cop.geo_frame_end   = g_cop.geo_capture_head;
    g_frame_done = 1;
    return 1;
}

/* co_processor_error_hang: halt on COP self-test failure.
 * Address TODO; implement mirrors STF 0x000077F8. */
static int fvipers_hook_cop_err_hang(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    LOG_ERROR("fvipers: COP self-test failed — error code 0x%08X  (IP=0x%08X)",
              cpu->globals.g[4], cpu->sfr.ip);
    cpu->halted = 1;
    return 0;
}

/* read_sw (0x229C): zero prev_held so the momentary-diff computation starts clean. */
static int fvipers_hook_read_sw(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    mem_write32(bus, FVIPERS_PREV_HELD_ADDR, 0);
    return 1;
}

/* clip_point_check_yoko (0x236BC): horizontal frustum cull — return 0 (always visible). */
static int fvipers_hook_clip_yoko(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    cpu->globals.g[0] = 0;
    hle_ret(cpu);
    return 0;
}

/* clip_point_check (0x23784): generic frustum cull — return 0 (always visible). */
static int fvipers_hook_clip_3d(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    cpu->globals.g[0] = 0;
    hle_ret(cpu);
    return 0;
}

/* ---- Profile object ----------------------------------------------------- */

/*
 * Active hooks: the 8 addresses confirmed above.
 * Remaining TODOs (check_timer_4_spin, _idle, _700000_loop, cop_err_hang):
 * fill in FVIPERS_HOOK_ADDR_* above and add the entry here when found.
 */
#define FVIPERS_HOOK_COUNT 9

static const game_profile_t fvipers_profile = {
    .id               = "fvipers",
    .display_name     = "Fighting Vipers",
    .parent_zip_name  = NULL,   /* fvipers is the parent set — self-contained */
    .board            = BOARD_MODEL2B_CRX,
    .load_fn          = fvipers_load,
    .install_fn       = fvipers_install,
    .hook_count       = FVIPERS_HOOK_COUNT,
    .hooks = {
        { FVIPERS_HOOK_ADDR_COP_INIT_L1,      fvipers_hook_cop_init_l1,      "cop_initialize_l1"    },
        { FVIPERS_HOOK_ADDR_CHECK_TIMER_4,     fvipers_hook_check_timer_4,    "check_timer_4"        },
        { FVIPERS_HOOK_ADDR_INTERRUPT_WAIT,    fvipers_hook_interrupt_wait,   "interrupt_wait"       },
        { FVIPERS_HOOK_ADDR_INTERRUPT_WAIT_B,      fvipers_hook_interrupt_wait_b,      "interrupt_wait_b"      },
        { FVIPERS_HOOK_ADDR_INTERRUPT_WAIT_B_SPIN, fvipers_hook_interrupt_wait_b_spin, "interrupt_wait_b_spin" },
        { FVIPERS_HOOK_ADDR_FRAME_PACE,        fvipers_hook_frame_pace,       "frame_pace"           },
        { FVIPERS_HOOK_ADDR_READ_SW,           fvipers_hook_read_sw,          "read_sw"              },
        { FVIPERS_HOOK_ADDR_CLIP_YOKO,         fvipers_hook_clip_yoko,        "clip_point_check_yoko"},
        { FVIPERS_HOOK_ADDR_CLIP_3D,           fvipers_hook_clip_3d,          "clip_point_check"     },
    },
    .input = {
        /* TODO: find held/momentary/credits addresses in fvipers RAM.
         * Locate read_sw (0x229C) in IDA; the stores nearby are these. */
        .held_addr       = 0x00500700,  /* INTERUPT_FLAGS_HELD     — confirmed IDA */
        .momentary_addr  = 0x00500704,  /* INTERUPT_FLAGS_MOMENTARY — confirmed IDA */
        .p1_credits_addr = 0x00000000,  /* TODO: not found in IDA database */
        .p2_credits_addr = 0x00000000,  /* TODO: not found in IDA database */
        .bits = {
            /* Same I/O board as STF — bit masks are likely identical. */
            [GAME_INPUT_P1_UP]    = 0x00002000,
            [GAME_INPUT_P1_DOWN]  = 0x00001000,
            [GAME_INPUT_P1_LEFT]  = 0x00008000,
            [GAME_INPUT_P1_RIGHT] = 0x00004000,
            [GAME_INPUT_P1_B1]    = 0x00000100,
            [GAME_INPUT_P1_B2]    = 0x00000200,
            [GAME_INPUT_P1_B3]    = 0x00000400,
            [GAME_INPUT_P1_START] = 0x00000010,
            [GAME_INPUT_P2_UP]    = 0x00200000,
            [GAME_INPUT_P2_DOWN]  = 0x00100000,
            [GAME_INPUT_P2_LEFT]  = 0x00800000,
            [GAME_INPUT_P2_RIGHT] = 0x00400000,
            [GAME_INPUT_P2_B1]    = 0x00010000,
            [GAME_INPUT_P2_B2]    = 0x00020000,
            [GAME_INPUT_P2_B3]    = 0x00040000,
            [GAME_INPUT_P2_START] = 0x00000020,
            [GAME_INPUT_SERVICE]  = 0x00000004,
        },
    },
    .quirks = {
        /* Start with STF values; re-run poly bruteforce against fvipers
         * reference renders before finalising (see CLAUDE.md §3D Polygon Decoder). */
        .poly_connect_mask  = 0x45B4,       /* tentative — verify against fvipers OBJs */
        .mesh_ptr_subtract  = 0x02000010,   /* TODO: verify encoding for fvipers */
        .mesh_ptr_add       = 0x10,
        .model_table_offset = 0x000E0004,   /* TODO: find in IDA */
        .model_table_count  = 5412,            /* TODO */
        .camera_struct_addr = 0x00515598,   /* TODO: find camera struct in RAM */
    },
};

#endif /* PROFILES_FVIPERS_H */
