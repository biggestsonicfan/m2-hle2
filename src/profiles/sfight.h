/*
 * profiles/sfight.h — Sonic The Fighters (sfight/schamp) profile.
 *
 * MAME-equivalent ROM set: sfight (clone of schamp). load_fn extracts files
 * from sfight.zip first, falls back to schamp.zip for shared files.
 *
 * Local override: if files named sfight/epr-19001.15 and sfight/epr-19002.16
 * exist on disk, they're used INSTEAD of the zip's maincpu pair (CRC check
 * skipped) — supports working with hacked ROMs without re-zipping every time.
 *
 * Per-game install behaviour: copies romset into bus regions, then mirrors
 * romset.main_data[0x01000000..] into bus->xtra_data. The mirror was found
 * empirically (string "SNC_ZIBA" at ROM[0x01000000+0x4012FB]) — STF-specific.
 *
 * PHASE 4 NOTE: this profile carries the loader, installer, input map and
 * quirks (all data the later phases consume). The HLE hook *functions* depend
 * on hle_ret/hle_call/g_cop/g_frame_done from Phase 6, so the hook table is
 * empty for now (hook_count = 0); Phase 6 fills it in.
 */
#ifndef PROFILES_SFIGHT_H
#define PROFILES_SFIGHT_H

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

/* ---- Loader ------------------------------------------------------------- */

#define SFIGHT_LOAD_PAIR_32(dest, dest_sz, name_lo, crc_lo, name_hi, crc_hi, half_sz, offset) \
    do { \
        tmp1 = zip_extract_from_set(child, parent, name_lo, &fsize, crc_lo); \
        tmp2 = zip_extract_from_set(child, parent, name_hi, &fsize, crc_hi); \
        if (!tmp1 || !tmp2) { ok = 0; goto done; } \
        interleave_32_word(dest, dest_sz, tmp1, half_sz, tmp2, half_sz, offset); \
        free(tmp1); tmp1 = NULL; \
        free(tmp2); tmp2 = NULL; \
    } while(0)

#define SFIGHT_LOAD_SAMPLE(dest, dest_sz, name, crc, offset) \
    do { \
        tmp1 = zip_extract_from_set(child, parent, name, &fsize, crc); \
        if (tmp1) { \
            load_16_word_swap(dest, dest_sz, tmp1, fsize, offset); \
            free(tmp1); tmp1 = NULL; \
        } \
    } while(0)

static inline int sfight_load(romset_t *rs, const char *child, const char *parent) {
    romset_free(rs);

    size_t   fsize = 0;
    uint8_t *tmp1 = NULL, *tmp2 = NULL;
    int      ok = 1;

    /* maincpu (2MB) — check for hacked override files first */
    rs->maincpu_size = 0x200000;
    rs->maincpu = (uint8_t *)calloc(1, rs->maincpu_size);
    {
        size_t sz1 = 0, sz2 = 0;
        uint8_t *hack1 = file_load("sfight/epr-19001.15", &sz1);
        uint8_t *hack2 = file_load("sfight/epr-19002.16", &sz2);
        if (hack1 && hack2) {
            LOG_INFO("sfight: using hacked maincpu from sfight/ (%zu, %zu bytes; CRC skipped)", sz1, sz2);
            interleave_32_word(rs->maincpu, rs->maincpu_size, hack1, sz1, hack2, sz2, 0);
            free(hack1); free(hack2);
        } else {
            if (hack1) free(hack1);
            if (hack2) free(hack2);
            SFIGHT_LOAD_PAIR_32(rs->maincpu, rs->maincpu_size,
                "epr-19001.15", 0x9b088511, "epr-19002.16", 0x46f510da, 0x80000, 0x000000);
        }
    }

    /* main_data (32MB) — large data region with a 1MB→15MB mirror */
    rs->main_data_size = 0x2000000;
    rs->main_data = (uint8_t *)calloc(1, rs->main_data_size);
    SFIGHT_LOAD_PAIR_32(rs->main_data, rs->main_data_size,
        "mpr-19007.11", 0x8b8ff751, "mpr-19008.12", 0xa94654f5, 0x400000, 0x0000000);
    SFIGHT_LOAD_PAIR_32(rs->main_data, rs->main_data_size,
        "mpr-19005.9",  0x98cd1127, "mpr-19006.10", 0xe79f0a26, 0x400000, 0x0800000);
    SFIGHT_LOAD_PAIR_32(rs->main_data, rs->main_data_size,
        "epr-19003.7",  0x63bae5c5, "epr-19004.8",  0xc10c9f39, 0x80000,  0x1000000);
    for (uint32_t d = 0x1100000; d < 0x2000000; d += 0x100000)
        rom_region_copy(rs->main_data, rs->main_data_size, 0x1000000, d, 0x100000);

    /* copro_data (8MB) */
    rs->copro_data_size = 0x800000;
    rs->copro_data = (uint8_t *)calloc(1, rs->copro_data_size);
    SFIGHT_LOAD_PAIR_32(rs->copro_data, rs->copro_data_size,
        "mpr-19015.29", 0xc74d99e3, "mpr-19016.30", 0x746ae931, 0x200000, 0x000000);

    /* polygons (16MB) */
    rs->polygons_size = 0x1000000;
    rs->polygons = (uint8_t *)calloc(1, rs->polygons_size);
    SFIGHT_LOAD_PAIR_32(rs->polygons, rs->polygons_size,
        "mpr-19009.17", 0xfd410350, "mpr-19012.21", 0x9bb7b5b6, 0x400000, 0x000000);
    SFIGHT_LOAD_PAIR_32(rs->polygons, rs->polygons_size,
        "mpr-19010.18", 0x6fd94187, "mpr-19013.22", 0x9e232fe5, 0x400000, 0x800000);

    /* textures (16MB) */
    rs->textures_size = 0x1000000;
    rs->textures = (uint8_t *)calloc(1, rs->textures_size);
    SFIGHT_LOAD_PAIR_32(rs->textures, rs->textures_size,
        "mpr-19019.27", 0x59121896, "mpr-19017.25", 0x7b298379, 0x400000, 0x000000);
    SFIGHT_LOAD_PAIR_32(rs->textures, rs->textures_size,
        "mpr-19020.28", 0x9540dba0, "mpr-19018.26", 0x3b7e7a12, 0x400000, 0x800000);

    /* audiocpu (512KB) — byte-swapped on load */
    rs->audiocpu_size = 0x80000;
    rs->audiocpu = (uint8_t *)calloc(1, rs->audiocpu_size);
    tmp1 = zip_extract_from_set(child, parent, "epr-19021.31", &fsize, 0x0b9f7583);
    if (!tmp1) { ok = 0; goto done; }
    load_16_word_swap(rs->audiocpu, rs->audiocpu_size, tmp1, fsize, 0);
    free(tmp1); tmp1 = NULL;

    /* samples (8MB) */
    rs->samples_size = 0x800000;
    rs->samples = (uint8_t *)calloc(1, rs->samples_size);
    SFIGHT_LOAD_SAMPLE(rs->samples, rs->samples_size, "mpr-19022.32", 0x4381869b, 0x000000);
    SFIGHT_LOAD_SAMPLE(rs->samples, rs->samples_size, "mpr-19023.33", 0x07c67f88, 0x200000);
    SFIGHT_LOAD_SAMPLE(rs->samples, rs->samples_size, "mpr-19024.34", 0x15ff76d3, 0x400000);
    SFIGHT_LOAD_SAMPLE(rs->samples, rs->samples_size, "mpr-19025.35", 0x6ad8fb70, 0x600000);

    rs->loaded = true;
    LOG_INFO("sfight: ROM set loaded (maincpu=%zu main_data=%zu copro=%zu poly=%zu tex=%zu audio=%zu samp=%zu)",
             rs->maincpu_size, rs->main_data_size, rs->copro_data_size,
             rs->polygons_size, rs->textures_size, rs->audiocpu_size, rs->samples_size);

done:
    free(tmp1);
    free(tmp2);
    if (!ok) { LOG_ERROR("sfight: ROM set load failed"); romset_free(rs); return -1; }
    return 0;
}

#undef SFIGHT_LOAD_PAIR_32
#undef SFIGHT_LOAD_SAMPLE

/* ---- Installer ---------------------------------------------------------- */

/* Reads a little-endian u32 from a flat buffer. */
static inline uint32_t sfight_read32(const uint8_t *buf, uint32_t off) {
    return  (uint32_t)buf[off]
         | ((uint32_t)buf[off + 1] << 8)
         | ((uint32_t)buf[off + 2] << 16)
         | ((uint32_t)buf[off + 3] << 24);
}

static inline void sfight_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!rs->loaded) { LOG_ERROR("sfight_install: romset not loaded"); return; }

    i960_reset(cpu);
    mem_init(bus, rs->maincpu, rs->maincpu_size);

    if (rs->main_data && bus->main_data) {
        size_t n = rs->main_data_size < MAIN_DATA_SIZE ? rs->main_data_size : MAIN_DATA_SIZE;
        memcpy(bus->main_data, rs->main_data, n);
        LOG_INFO("sfight_install: copied %zu bytes to MAIN_DATA", n);
    }

    /* STF-specific: XTRA_DATA at 0x06000000 mirrors main_data starting at
     * offset 0x01000000. Confirmed empirically — string "SNC_ZIBA" lives at
     * ROM[0x01000000+0x4012FB]. */
    if (rs->main_data && bus->xtra_data) {
        const uint32_t src_off = 0x01000000;
        if (src_off < rs->main_data_size) {
            size_t avail = rs->main_data_size - src_off;
            size_t n = avail < XTRA_DATA_SIZE ? avail : XTRA_DATA_SIZE;
            memcpy(bus->xtra_data, rs->main_data + src_off, n);
            LOG_INFO("sfight_install: mirrored %zu bytes ROM[0x%08X..] → XTRA_DATA", n, src_off);
        }
    }

    /* Read PRCB from maincpu[+0x04] → start_ip_ptr → initial IP. */
    uint32_t sat_ptr  = sfight_read32(rs->maincpu, 0x00);
    uint32_t prcb_ptr = sfight_read32(rs->maincpu, 0x04);
    LOG_INFO("sfight_install: SAT=0x%08X  PRCB=0x%08X", sat_ptr, prcb_ptr);
    if (prcb_ptr + 0x2C < rs->maincpu_size) {
        uint32_t start_ip_ptr = sfight_read32(rs->maincpu, prcb_ptr + PRCB_START_IP);
        if (start_ip_ptr < rs->maincpu_size) {
            cpu->sfr.ip = sfight_read32(rs->maincpu, start_ip_ptr);
        }
    }
    LOG_INFO("sfight_install: initial IP = 0x%08X", cpu->sfr.ip);
}

/* ---- HLE hook functions -------------------------------------------------- */

/* cop_initialize_l1 (0x0F3C): boot-time COP-ready spin loop. Reads
 * COPRO_CONTROL1+4 bit 0 until set. Set the bit and let the instruction run
 * normally — the next iteration sees ready=1 and exits. Without this hook STF
 * hangs on the BACKUP RAM screen forever. */
static int sfight_hook_cop_init_l1(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    uint32_t cur = mem_read32(bus, COPRO_CONTROL1_BASE + 4);
    mem_write32(bus, COPRO_CONTROL1_BASE + 4, cur | 0x01);
    return 1;
}

/* check_timer_4 (0x4A55C): spin loop waiting for a timer interrupt.
 * Skip the whole function — return 0 to the caller. */
static int sfight_hook_check_timer_4(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    cpu->globals.g[0] = 0;
    hle_ret(cpu);
    return 0;
}

/* check_timer_4_spin (0x4A58C): inner polling loop reading byte_50008C.
 * Write 0x01 so the loop exits on its own next iteration. */
static int sfight_hook_check_timer_4_spin(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    mem_write8(bus, 0x0050008C, 0x01);
    return 1;
}

/* interrupt_wait (0x1768): inject VsyncScr then skip the spin loop entirely.
 * The real loop spins until RAM_BASE >= 2 and bit 0 is clear. We skip it by
 * returning to 0x1778 (the instruction after the loop) and use hle_call to run
 * VsyncScr (0x0C40) first so per-frame 2D work fires. */
static int sfight_hook_interrupt_wait(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    hle_call(cpu, 0x00000C40, 0x00001778);
    return 0;
}

/* interrupt_wait_b (0x11580): clear RAM_BASE to 0 so _idle's spin loop runs and
 * execution falls through to the _idle hook (rather than jumping past it). */
static int sfight_hook_interrupt_wait_b(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    mem_write8(bus, RAM_BASE, 0x00);
    return 1;
}

/* _idle (0x11610): inject VsyncScr on first entry; run the ldob normally after.
 * First call: push a VsyncScr frame returning to 0x11610 so the hook fires again.
 * Second call: let ldob execute so the cmpibe at 0x11618 sees the value and exits. */
static int sfight_hook_idle(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    static int s_vsync_fired = 0;
    if (!s_vsync_fired) {
        s_vsync_fired = 1;
        hle_call(cpu, 0x00000C40, 0x00011610);
        return 0;
    }
    s_vsync_fired = 0;
    return 1;
}

/* _700000_loop (0x7264): sound-init delay loop — zero r3 so cmpdeco exits. */
static int sfight_hook_700000_loop(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    cpu->locals.r[3] = 0;
    return 1;
}

/* variable_diff_calc (0x11A04): fires once per game frame at the end of the
 * main loop. Setting g_frame_done lets the emu thread pace to the next 60Hz
 * tick (and ends the slice early, freeing the mutex for the UI).
 * NOTE: the COP geo-capture frame boundary (Phase 9) and the CSV ground-truth
 * dump are layered on here later. */
static int sfight_hook_frame_pace(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu; (void)bus;
    /* Snapshot this frame's slice of the geo-capture ring so the Phase 9
     * scanner reads exactly one game frame's draw commands. */
    g_cop.geo_frame_start = g_cop.geo_frame_end;
    g_cop.geo_frame_end   = g_cop.geo_capture_head;
    g_frame_done = 1;
    return 1;
}

/* co_processor_error_hang (0x77F8): infinite self-branch on COP self-test
 * failure. Halt the CPU and log g4's error code so the UI stays responsive. */
static int sfight_hook_cop_err_hang(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    LOG_ERROR("COP self-test failed — error code 0x%08X  (IP=0x%08X)",
              cpu->globals.g[4], cpu->sfr.ip);
    cpu->halted = 1;
    return 0;
}

/* NOTE: there is intentionally NO read_sw (0x17CC) hook. Inputs are delivered
 * the authentic way — input.h serves the active-low I/O ports (0x1C00000) and
 * the game's own read_sw, called from the VsyncScr vblank interrupt, reads them
 * and builds 0x500700/0x500704 itself. (Phase 12.) */

/* hle_clip_point_check_yoko (0x28188) / hle_clip_point_check (0x28250):
 * point-in-frustum checks. Return 0 (visible) unconditionally so the 3D
 * pipeline never clips objects. */
static int sfight_hook_clip_yoko(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    cpu->globals.g[0] = 0;
    hle_ret(cpu);
    return 0;
}
static int sfight_hook_clip_3d(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)bus;
    cpu->globals.g[0] = 0;
    hle_ret(cpu);
    return 0;
}

/* ---- Profile object ----------------------------------------------------- */

static const game_profile_t sfight_profile = {
    .id               = "sfight",
    .display_name     = "Sonic The Fighters",
    .parent_zip_name  = "schamp.zip",
    .board            = BOARD_MODEL2B_CRX,
    .load_fn      = sfight_load,
    .install_fn   = sfight_install,
    .hook_count   = 11,
    .hooks = {
        { 0x00000F3C, sfight_hook_cop_init_l1,        "cop_initialize_l1"       },
        { 0x0004A55C, sfight_hook_check_timer_4,      "check_timer_4"           },
        { 0x0004A58C, sfight_hook_check_timer_4_spin, "check_timer_4_spin"      },
        { 0x00001768, sfight_hook_interrupt_wait,     "interrupt_wait"          },
        { 0x00011580, sfight_hook_interrupt_wait_b,   "interrupt_wait_b"        },
        { 0x00011610, sfight_hook_idle,               "_idle"                   },
        { 0x00007264, sfight_hook_700000_loop,        "_700000_loop"            },
        { 0x00011A04, sfight_hook_frame_pace,         "frame_pace"              },
        { 0x000077F8, sfight_hook_cop_err_hang,       "co_processor_error_hang" },
        { 0x00028188, sfight_hook_clip_yoko,          "clip_point_check_yoko"   },
        { 0x00028250, sfight_hook_clip_3d,            "clip_point_check"        },
    },
    .input = {
        .held_addr       = 0x00500700,
        .momentary_addr  = 0x00500704,
        .p1_credits_addr = 0x0059C388,
        .p2_credits_addr = 0x0059C38C,
        .bits = {
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
            /* Coins map to IN0 (held byte0) COIN1/COIN2 — active-low at IO+0x02.
             * The game's read_sw edge-detects these and updates credits. */
            [GAME_INPUT_P1_COIN]  = 0x00000001,
            [GAME_INPUT_P2_COIN]  = 0x00000002,
        },
    },
    .quirks = {
        .poly_connect_mask  = 0x45B4,
        .mesh_ptr_subtract  = 0x02000010,
        .mesh_ptr_add       = 0x10,
        .model_table_offset = 0x000E0004,
        .model_table_count  = 5103,
        .camera_struct_addr = 0x00519E98,
        .enable_68k_sound   = true,
        /* Real interrupt handlers (dispatch table @0x46b4): pin0 VsyncScr,
         * pin1 VsyncObj, pin2 Timer, pin3 Other(sound). */
        .irq_handler        = { 0x00000C40, 0x00000D10, 0x00000D30, 0x00000DF0 },
        .sound_queue_count_addr = 0x00504001,   /* byte_504001 */
        .sound_queue_state_addr = 0x00504014,   /* byte_504014 */
        .warning_skip_addr      = 0x00500410,   /* poke 1 → skip boot warning screen */
    },
};

#endif /* PROFILES_SFIGHT_H */
