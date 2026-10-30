/*
 * profiles/m2_library.h — the rest of MAME's Model 2 library (Pinboard #1872).
 *
 * The CRX games no other profile runs, each as data: its chip list (MAME's
 * ROM_START, written out by tools/mame_rom_recipe.py), its board, its
 * 315-5881 key if it has one, and its controls. They share one install per
 * board (m2lib_install), the one every newer profile spells out for itself:
 * the coprocessor the board has, the data windows, the 315-5881, and the
 * processor's start from the PRCB (MAME i960 device_reset: the interrupt
 * stack, priority 31, interrupted, supervisor). Interrupts go through the
 * program's own table and the frame edge is the board's vblank, as on
 * Dynamite Cop and Pilot Kids: no hooks.
 *
 *   Model 2A (TGP):     manxtt, skytargt, vcop2, zeroguna
 *   Model 2B (SHARC):   vstriker, vstrikero, zerogun, zerogunj, dynabb,
 *                       gunblade, rchase2, indy500, indy500d, sgt24h,
 *                       overrevb
 *   Model 2C (TGPx4):   overrev, stcc, skisuprg, segawski, waverunr,
 *                       topskatr, topskatru, topskatruo, topskatrj, bel
 *
 * Which game is on which board, and its I/O, is MAME's machine config
 * (src/mame/sega/model2.cpp); the controls are its INPUT_PORTS. What MAME
 * runs beside the CRX board and nothing here models yet is left out, each
 * game saying so: Manx TT's Model 1 sound board (the DX's extra voices),
 * the comm board's program. Touring Car's and Top Skater's MPEG music
 * boards are board/dsb2.h.
 * Every game's country is REGION_TODO until someone looks for its setting.
 */
#ifndef PROFILES_M2_LIBRARY_H
#define PROFILES_M2_LIBRARY_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "constants.h"
#include "log.h"
#include "rom_loader.h"
#include "rom_recipe.h"
#include "game_profile.h"
#include "memory.h"
#include "i960.h"
#include "model2a.h"
#include "model2b_cop.h"
#include "model2c_cop.h"
#include "crypt_5881.h"
#include "io_5649.h"
#include "m2comm.h"
#include "lightgun.h"
#include "savestate_reg.h"

static inline uint32_t m2lib_read32(const uint8_t *buf, uint32_t off) {
    return (uint32_t)buf[off] | (uint32_t)buf[off + 1] << 8
         | (uint32_t)buf[off + 2] << 16 | (uint32_t)buf[off + 3] << 24;
}

/* The board's coprocessor, and its state for a savestate. */
static inline void m2lib_install_cop(const romset_t *rs, memory_bus_t *bus, board_variant_t board,
                                     const char *tag) {
    switch (board) {
    case BOARD_MODEL2A_CRX:
        model2a_install(bus, rs->tgp_tables, rs->copro_data, rs->copro_data_size);
        savestate_extra(tag, "TGP", &g_tgp, sizeof g_tgp);
        break;
    case BOARD_MODEL2B_CRX:
        model2b_cop_install(bus, rs->copro_data, rs->copro_data_size);
        savestate_extra(tag, "M2BCOP", &g_m2bc, sizeof g_m2bc);
        savestate_extra(tag, "SHARC", &g_shc, sizeof g_shc);
        break;
    default:
        model2c_cop_install(bus, rs->copro_data, rs->copro_data_size);
        savestate_extra(tag, "TGPX4", &g_tgx, sizeof g_tgx);
        break;
    }
}

/* main_data at 0x02000000; the "extra" window at 0x06000000 is main_data
 * from 0x1000000 (MAME model2_base_mem). */
static inline void m2lib_data_windows(const romset_t *rs, memory_bus_t *bus) {
    if (rs->main_data && bus->main_data) {
        size_t n = rs->main_data_size < MAIN_DATA_SIZE ? rs->main_data_size : MAIN_DATA_SIZE;
        memcpy(bus->main_data, rs->main_data, n);
    }
    if (rs->main_data && bus->xtra_data && rs->main_data_size > 0x1000000u) {
        size_t n = rs->main_data_size - 0x1000000u;
        memcpy(bus->xtra_data, rs->main_data + 0x1000000u, n < XTRA_DATA_SIZE ? n : XTRA_DATA_SIZE);
    }
}

/* key: the 315-5881's (ROM_PARAMETER ":315_5881:key"), 0 for none. */
static inline void m2lib_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus,
                                 board_variant_t board, uint32_t key, const char *tag) {
    if (!rs->loaded) { LOG_ERROR("%s_install: romset not loaded", tag); return; }
    i960_reset(cpu);
    mem_init(bus, rs->maincpu, rs->maincpu_size);
    m2lib_install_cop(rs, bus, board, tag);
    savestate_extra(tag, "I960FRAC", &g_i960_cyc_frac, sizeof g_i960_cyc_frac);
    savestate_extra(tag, "I960BT", &g_i960_bt, sizeof g_i960_bt);
    io5649_reset();
    savestate_extra(tag, "IO5649", &g_io5649, sizeof g_io5649);
    savestate_extra(tag, "IO5649AX", &g_io5649_ax, sizeof g_io5649_ax);
    savestate_extra(tag, "GUN", &g_lightgun, sizeof g_lightgun);
    m2lib_data_windows(rs, bus);
    /* MAME fits a comm board to every Model 2, alone on the link (m2comm.h,
     * its default frame offset): Super GT 24h and Over Rev check the network
     * at boot and stop there without one. */
    m2comm_install(bus, 0x1C0u);
    savestate_extra(tag, "M2COMM", &g_m2comm, sizeof g_m2comm);
    if (key) {
        crypt_5881_install(bus, key);
        savestate_extra(tag, "C5881", &g_c5881, sizeof g_c5881);
    }
    uint32_t prcb = m2lib_read32(rs->maincpu, 0x04);
    cpu->prcb = prcb;
    if (prcb + 0x2C < rs->maincpu_size) {
        uint32_t isp = m2lib_read32(rs->maincpu, prcb + PRCB_INTR_STACK);
        cpu->globals.fp = isp;
        cpu->locals.sp  = isp + 64;
        cpu->sfr.pc     = 0x001F2002u;
        uint32_t start_ip_ptr = m2lib_read32(rs->maincpu, prcb + PRCB_START_IP);
        if (start_ip_ptr < rs->maincpu_size) cpu->sfr.ip = m2lib_read32(rs->maincpu, start_ip_ptr);
    }
    LOG_INFO("%s_install: PRCB=0x%08X initial IP=0x%08X", tag, prcb, cpu->sfr.ip);
}

/* The recipe, or `alt` when the zip holds that one instead: a set dumped under
 * the other's name (the container's dynabb.zip is MAME's dynabb97). Both are
 * tried quietly; when neither loads, the recipe is tried aloud so the log
 * names its missing chips. */
static inline int m2lib_load(romset_t *rs, const char *child, const char *parent,
                             const rom_recipe_t *rc, const rom_recipe_t *alt) {
    if (!alt) return rom_recipe_load(rs, child, parent, rc);
    g_rl_quiet_missing++;
    int got = rom_recipe_load(rs, child, parent, rc);
    if (got != 0) got = rom_recipe_load(rs, child, parent, alt);
    g_rl_quiet_missing--;
    return got == 0 ? 0 : rom_recipe_load(rs, child, parent, rc);
}

#define M2LIB_N(a) (sizeof a / sizeof a[0])
/* A set's recipe, its region sizes last; `old` is an earlier revision's
 * chips (rom_recipe_t.older), NULL for none. */
#define M2LIB_RECIPE(name, old, old_n, ...) \
    static const rom_recipe_t name##_recipe = { \
        .set = #name, .size = { __VA_ARGS__ }, \
        .parts = name##_parts, .count = M2LIB_N(name##_parts), \
        .older = old, .older_count = old_n, \
    };
/* Its loader and install; `alt` as m2lib_load's. */
#define M2LIB_FUNCS(name, brd, key, alt) \
    static inline int name##_load(romset_t *rs, const char *child, const char *parent) { \
        return m2lib_load(rs, child, parent, &name##_recipe, alt); \
    } \
    static inline void name##_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) { \
        m2lib_install(rs, cpu, bus, brd, key, #name); \
    }
/* One set: its recipe, loader and install. `sizes` and `key` are what
 * tools/mame_rom_recipe.py prints beside the parts. */
#define M2LIB_SET(name, sizes, brd, key) \
    M2LIB_RECIPE(name, NULL, 0, sizes) \
    M2LIB_FUNCS(name, brd, key, NULL)

/* ---- Chip lists (tools/mame_rom_recipe.py over MAME's model2.cpp) -------- */

/* MAME's M1AUDIO_CPU_REGION, M1AUDIO_MPCM1_REGION, M1AUDIO_MPCM2_REGION, cpu4, eeprom: no board here reads them. */
static const rom_part_t manxtt_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-18822d.12", 0x4f435990, "epr-18823d.13", 0xb8eddb5c),
    ROM_PAIR32(ROM_MAINCPU, 0x0040000, "epr-18824d.14", 0xaca9f61f, "epr-18825d.15", 0x5a1d7799),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-18751.10", 0x773ad43d, "mpr-18752.11", 0x4da3719e),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0400000, "mpr-18749.8", 0xc3fe0eea, "mpr-18750.9", 0x40b55494),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "mpr-18747.6", 0xa65ec1e8, "mpr-18748.7", 0x375e3748),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0c00000, "epr-18862.4", 0x9adc3a30, "epr-18863.5", 0x603742e9),
    ROM_MIRROR(ROM_MAIN_DATA, 0x0c00000, 0x0d00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x0c00000, 0x0e00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x0c00000, 0x0f00000, 0x100000),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-18753.16", 0x33ddaa0d, "mpr-18756.20", 0x28713617),
    ROM_PAIR32(ROM_POLYGONS, 0x0400000, "mpr-18754.17", 0x09aabde5, "mpr-18757.21", 0x25fc92e9),
    ROM_PAIR32(ROM_POLYGONS, 0x0800000, "mpr-18755.18", 0xbf094d9e, "mpr-18758.22", 0x1b5473d0),
    ROM_PAIR32(ROM_COPRO_DATA, 0x0000000, "mpr-18761.28", 0x4e39ec05, "mpr-18762.29", 0x4ab165d8),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-18760.25", 0x4e3a4a89, "mpr-18759.24", 0x278d8742),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "epr-18924a.30", 0xad6f40ec),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-18763.31", 0x1bcb2283),
    ROM_SWAP16(ROM_SAMPLES, 0x0200000, "mpr-18764.32", 0x0dc6a860),
    ROM_SWAP16(ROM_SAMPLES, 0x0400000, "mpr-18765.36", 0xca4a803c),
    ROM_SWAP16(ROM_SAMPLES, 0x0600000, "mpr-18766.37", 0xe41892ea),
    ROM_PAIR32(ROM_TGP_TABLES, 0x0000000, "opr-14742a.45", 0x90c6b117, "opr-14743a.46", 0xae7f446b),
};
#define MANXTT_SIZES [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2400000, [ROM_COPRO_DATA] = 0x800000, [ROM_POLYGONS] = 0x1000000, [ROM_TEXTURES] = 0x400000, [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000, [ROM_TGP_TABLES] = 0x40000

static const rom_part_t zeroguna_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-20437.12", 0xfad30cc0, "epr-20438.13", 0xca364408),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-20296.10", 0x072d8a5e, "mpr-20297.11", 0xba6a825b),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "mpr-20294.8", 0xa0bd1474, "mpr-20295.9", 0xc548cced),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-20298.16", 0x8ab782fc, "mpr-20299.20", 0x90e20cdb),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-20301.25", 0x52010fb2, "mpr-20300.24", 0x6f042792),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "epr-20302.30", 0x44ff50d2),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-20303.31", 0xc040973f),
    ROM_SWAP16(ROM_SAMPLES, 0x0200000, "mpr-20304.32", 0x6decfe83),
    ROM_PAIR32(ROM_TGP_TABLES, 0x0000000, "opr-14742a.45", 0x90c6b117, "opr-14743a.46", 0xae7f446b),
};
#define ZEROGUNA_SIZES [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, [ROM_POLYGONS] = 0x800000, [ROM_TEXTURES] = 0x400000, [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x400000, [ROM_TGP_TABLES] = 0x40000
#define ZEROGUNA_KEY 0x042c0d13u

static const rom_part_t skytargt_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-18406.12", 0xfde9c00a, "epr-18407.13", 0x35f8b529),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-18415.10", 0xd7a1bbd7, "mpr-18416.11", 0xb77c9243),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "mpr-18417.8", 0xa0d03f63, "mpr-18418.9", 0xc7a6f97f),
    ROM_PAIR32(ROM_MAIN_DATA, 0x1000000, "epr-18404.6", 0xf1407ec4, "epr-18405.7", 0x00b40f9e),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1100000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1200000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1300000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1400000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1500000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1600000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1700000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1800000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1900000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1a00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1b00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1c00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1d00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1e00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1f00000, 0x100000),
    ROM_PAIR32(ROM_COPRO_DATA, 0x0000000, "mpr-18420.28", 0x92b87817, "mpr-18419.29", 0x74542d87),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-18413.16", 0x1c4d416c, "mpr-18409.20", 0x666037ef),
    ROM_PAIR32(ROM_POLYGONS, 0x0800000, "mpr-18414.17", 0x858885ba, "mpr-18410.21", 0xb821a695),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-18412.25", 0x4db52f8b, "mpr-18411.24", 0x9c2dc40c),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "epr-18408.30", 0x6deb9657),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-18424.31", 0x590a4338),
    ROM_SWAP16(ROM_SAMPLES, 0x0200000, "mpr-18423.32", 0xc356d765),
    ROM_SWAP16(ROM_SAMPLES, 0x0400000, "mpr-18422.36", 0xb4f3cea6),
    ROM_SWAP16(ROM_SAMPLES, 0x0600000, "mpr-18421.37", 0x00522390),
    ROM_PAIR32(ROM_TGP_TABLES, 0x0000000, "opr-14742a.45", 0x90c6b117, "opr-14743a.46", 0xae7f446b),
};
#define SKYTARGT_SIZES [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2400000, [ROM_COPRO_DATA] = 0x800000, [ROM_POLYGONS] = 0x2000000, [ROM_TEXTURES] = 0x1000000, [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000, [ROM_TGP_TABLES] = 0x40000

static const rom_part_t vcop2_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-18524.12", 0x1858988b, "epr-18525.13", 0x0c13df3f),
    ROM_PAIR32(ROM_MAINCPU, 0x0100000, "epr-18518.14", 0x7842951b, "epr-18519.15", 0x31a30edc),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-18516.10", 0xa3928ff0, "mpr-18517.11", 0x4bd73da4),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0400000, "mpr-18514.8", 0x791283c5, "mpr-18515.9", 0x6ba1ffec),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "mpr-18522.6", 0x61d18536, "mpr-18523.7", 0x61d08dc4),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0c00000, "epr-18520.4", 0x1d4ec5e8, "epr-18521.5", 0xb8b3781c),
    ROM_MIRROR(ROM_MAIN_DATA, 0x0c00000, 0x0d00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x0c00000, 0x0e00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x0c00000, 0x0f00000, 0x100000),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-18513.16", 0x777a3633, "mpr-18510.20", 0xe83de997),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-18512.25", 0xd9bc7e71, "mpr-18511.24", 0xcae77a4f),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "epr-18530.30", 0xac9c8357),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-18529.31", 0xf76715b1),
    ROM_SWAP16(ROM_SAMPLES, 0x0200000, "mpr-18528.32", 0x287a2f9a),
    ROM_SWAP16(ROM_SAMPLES, 0x0400000, "mpr-18527.36", 0xe6a49314),
    ROM_SWAP16(ROM_SAMPLES, 0x0600000, "mpr-18526.37", 0x6516d9b5),
    ROM_PAIR32(ROM_TGP_TABLES, 0x0000000, "opr-14742a.45", 0x90c6b117, "opr-14743a.46", 0xae7f446b),
};
#define VCOP2_SIZES [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2400000, [ROM_COPRO_DATA] = 0x800000, [ROM_POLYGONS] = 0x2000000, [ROM_TEXTURES] = 0x400000, [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000, [ROM_TGP_TABLES] = 0x40000

static const rom_part_t vstriker_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-18068a.15", 0xafc69b54, "epr-18069a.16", 0x0243250c),
    ROM_PAIR32(ROM_MAINCPU, 0x0040000, "epr-18066a.13", 0xe658b33a, "epr-18067a.14", 0x49e94047),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-18055.11", 0x5aba9fc0, "mpr-18056.12", 0x017f0c55),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0400000, "mpr-18053.9", 0x46c770c8, "mpr-18054.10", 0x437af66e),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "epr-18070a.7", 0x1961e2fc, "epr-18071a.8", 0xb2492dca),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-18057.17", 0x890d8806, "mpr-18059.21", 0xc5cdf534),
    ROM_PAIR32(ROM_POLYGONS, 0x0400000, "mpr-18058.18", 0xd4cbdf7c, "mpr-18060.22", 0x93d5c95f),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-18062.27", 0x126e7de3, "mpr-18061.25", 0xc37f1c67),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "epr-18072.31", 0x73eabb58),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-18063.32", 0xb74d7c8a),
    ROM_SWAP16(ROM_SAMPLES, 0x0200000, "mpr-18064.33", 0x783b9910),
    ROM_SWAP16(ROM_SAMPLES, 0x0400000, "mpr-18065.34", 0x046b55fe),
};
#define VSTRIKER_SIZES [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, [ROM_POLYGONS] = 0x800000, [ROM_TEXTURES] = 0x400000, [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x600000

static const rom_part_t vstrikero_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-18068.15", 0x74a47795, "epr-18069.16", 0xf6c3fcbf),
    ROM_PAIR32(ROM_MAINCPU, 0x0040000, "epr-18066.13", 0xe774229e, "epr-18067.14", 0x7dfd950c),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-18055.11", 0x5aba9fc0, "mpr-18056.12", 0x017f0c55),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0400000, "mpr-18053.9", 0x46c770c8, "mpr-18054.10", 0x437af66e),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "epr-18070.7", 0xf52e4db5, "epr-18071.8", 0x1be63a7d),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-18057.17", 0x890d8806, "mpr-18059.21", 0xc5cdf534),
    ROM_PAIR32(ROM_POLYGONS, 0x0400000, "mpr-18058.18", 0xd4cbdf7c, "mpr-18060.22", 0x93d5c95f),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-18062.27", 0x126e7de3, "mpr-18061.25", 0xc37f1c67),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "epr-18072.31", 0x73eabb58),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-18063.32", 0xb74d7c8a),
    ROM_SWAP16(ROM_SAMPLES, 0x0200000, "mpr-18064.33", 0x783b9910),
    ROM_SWAP16(ROM_SAMPLES, 0x0400000, "mpr-18065.34", 0x046b55fe),
};
#define VSTRIKERO_SIZES [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, [ROM_POLYGONS] = 0x800000, [ROM_TEXTURES] = 0x400000, [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x600000

static const rom_part_t zerogun_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-20439.15", 0x10125381, "epr-20440.16", 0xce872747),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-20296.11", 0x072d8a5e, "mpr-20297.12", 0xba6a825b),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "mpr-20294.9", 0xa0bd1474, "mpr-20295.10", 0xc548cced),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-20298.17", 0x8ab782fc, "mpr-20299.21", 0x90e20cdb),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-20301.27", 0x52010fb2, "mpr-20300.25", 0x6f042792),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "epr-20302.31", 0x44ff50d2),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-20303.32", 0xc040973f),
    ROM_SWAP16(ROM_SAMPLES, 0x0200000, "mpr-20304.33", 0x6decfe83),
};
#define ZEROGUN_SIZES [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, [ROM_POLYGONS] = 0x800000, [ROM_TEXTURES] = 0x400000, [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x400000
#define ZEROGUN_KEY 0x042c0d13u

static const rom_part_t zerogunj_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-20290.15", 0x9ce3ad21, "epr-20291.16", 0x7267a03d),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-20296.11", 0x072d8a5e, "mpr-20297.12", 0xba6a825b),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "mpr-20294.9", 0xa0bd1474, "mpr-20295.10", 0xc548cced),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-20298.17", 0x8ab782fc, "mpr-20299.21", 0x90e20cdb),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-20301.27", 0x52010fb2, "mpr-20300.25", 0x6f042792),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "epr-20302.31", 0x44ff50d2),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-20303.32", 0xc040973f),
    ROM_SWAP16(ROM_SAMPLES, 0x0200000, "mpr-20304.33", 0x6decfe83),
};
#define ZEROGUNJ_SIZES [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, [ROM_POLYGONS] = 0x800000, [ROM_TEXTURES] = 0x400000, [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x400000
#define ZEROGUNJ_KEY 0x042c0d13u

/* MAME's dynabb97, which the container's dynabb.zip holds (m2lib_load). */
static const rom_part_t dynabb97_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-19833a.15", 0xd99ed1b2, "epr-19834a.16", 0x24192bb1),
    ROM_PAIR32(ROM_MAINCPU, 0x0100000, "epr-19831a.13", 0x0527ea40, "epr-19832a.14", 0x2f380a40),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-19841.11", 0x989309af, "mpr-19842.12", 0xeec54070),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "mpr-19839.9", 0xd5a74cf4, "mpr-19840.10", 0x45704e95),
    ROM_PAIR32(ROM_MAIN_DATA, 0x1000000, "mpr-19837.7", 0xc02187d9, "mpr-19838.8", 0x546b61cd),
    ROM_PAIR32(ROM_MAIN_DATA, 0x1800000, "mpr-19835.5", 0xa3b0a37c, "mpr-19836.6", 0xd70a32aa),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-19843.17", 0x019bc583, "mpr-19845.21", 0x2d23e73a),
    ROM_PAIR32(ROM_POLYGONS, 0x0800000, "mpr-19844.18", 0x150198d6, "mpr-19846.22", 0xfe53cd17),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-19848.27", 0x4c0526b7, "mpr-19847.25", 0xfe55edbd),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "epr-19849.31", 0xb0d5bff0),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-19850.32", 0xe1fd27bf),
    ROM_SWAP16(ROM_SAMPLES, 0x0200000, "mpr-19851.33", 0xdc644077),
    ROM_SWAP16(ROM_SAMPLES, 0x0400000, "mpr-19852.34", 0xcfda4efd),
    ROM_SWAP16(ROM_SAMPLES, 0x0600000, "mpr-19853.35", 0xcfc64857),
};
#define DYNABB97_SIZES [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, [ROM_POLYGONS] = 0x1000000, [ROM_TEXTURES] = 0x800000, [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000

/* The chips where MAME's indy500to and stcca differ from indy500 and stcc:
 * the container's zips are those revisions (rom_recipe_t.older). */
static const rom_part_t indy500to_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-18254a.15", 0xad0f1fc5, "epr-18255a.16", 0x784daab8),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-18237.11", 0x37e4255a, "mpr-18238.12", 0xbf837bac),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "mpr-18239.9", 0x9a2db86e, "mpr-18240.10", 0xab46a35f),
    ROM_PAIR32(ROM_MAIN_DATA, 0x1000000, "epr-18389.7", 0xd22ea019, "epr-18390.8", 0x38e796e5),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1100000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1200000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1300000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1400000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1500000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1600000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1700000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1800000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1900000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1a00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1b00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1c00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1d00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1e00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1f00000, 0x100000),
    ROM_PAIR32(ROM_COPRO_DATA, 0x0000000, "epr-18249.29", 0xa399f023, "epr-18250.30", 0x7479ad52),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-18233.17", 0x48a024d3, "mpr-18234.21", 0x1178bfc8),
    ROM_PAIR32(ROM_POLYGONS, 0x0800000, "mpr-18235.18", 0xe7d70d59, "mpr-18236.22", 0x6ca29e0e),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-18232.27", 0xf962347d, "mpr-18231.25", 0x673d5338),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "epr-18391.31", 0x79579b72),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-18241.32", 0x3a380ae1),
    ROM_SWAP16(ROM_SAMPLES, 0x0200000, "mpr-18242.33", 0x1cc3deae),
    ROM_SWAP16(ROM_SAMPLES, 0x0400000, "mpr-18243.34", 0xa00a0053),
    ROM_SWAP16(ROM_SAMPLES, 0x0600000, "mpr-18244.35", 0xbfa75beb),
};
static const rom_part_t stcca_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-19272a.15", 0x20cedd05, "epr-19273a.16", 0x1b0ab4d6),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-19257.11", 0xac28ee24, "mpr-19258.12", 0xf5ba7d78),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "epr-19270.9", 0x7bd1d04e, "epr-19271.10", 0xd2d74f85),
    ROM_MIRROR(ROM_MAIN_DATA, 0x0800000, 0x0900000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x0800000, 0x0a00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x0800000, 0x0b00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x0800000, 0x0c00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x0800000, 0x0d00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x0800000, 0x0e00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x0800000, 0x0f00000, 0x100000),
    ROM_PAIR32(ROM_COPRO_DATA, 0x0000000, "mpr-19255.29", 0xd78bf030, "mpr-19256.30", 0xcb2b2d9e),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-19251.17", 0xe06ff7ba, "mpr-19252.21", 0x68509993),
    ROM_PAIR32(ROM_POLYGONS, 0x0800000, "epr-19266.18", 0x41464ee2, "epr-19267.22", 0x780f994d),
    ROM_MIRROR(ROM_POLYGONS, 0x0800000, 0x0900000, 0x100000),
    ROM_MIRROR(ROM_POLYGONS, 0x0800000, 0x0a00000, 0x200000),
    ROM_MIRROR(ROM_POLYGONS, 0x0800000, 0x0c00000, 0x400000),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-19254.27", 0x1ec49c02, "mpr-19253.25", 0x41ba79fb),
    ROM_MIRROR(ROM_TEXTURES, 0x0000000, 0x0400000, 0x400000),
    ROM_PAIR32(ROM_TEXTURES, 0x0800000, "epr-19269.28", 0x01881121, "epr-19268.26", 0xbc4e081c),
    ROM_MIRROR(ROM_TEXTURES, 0x0800000, 0x0900000, 0x100000),
    ROM_MIRROR(ROM_TEXTURES, 0x0800000, 0x0a00000, 0x100000),
    ROM_MIRROR(ROM_TEXTURES, 0x0800000, 0x0b00000, 0x100000),
    ROM_MIRROR(ROM_TEXTURES, 0x0800000, 0x0c00000, 0x100000),
    ROM_MIRROR(ROM_TEXTURES, 0x0800000, 0x0d00000, 0x100000),
    ROM_MIRROR(ROM_TEXTURES, 0x0800000, 0x0e00000, 0x100000),
    ROM_MIRROR(ROM_TEXTURES, 0x0800000, 0x0f00000, 0x100000),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "epr-19274.31", 0x2dcc08ae),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-19259.32", 0x4d55dbfc),
    ROM_SWAP16(ROM_SAMPLES, 0x0400000, "mpr-19261.34", 0xb88878ff),
};

static const rom_part_t dynabb_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-19170.15", 0xe00eb49e, "epr-19171.16", 0x9878d67d),
    ROM_PAIR32(ROM_MAINCPU, 0x0100000, "epr-19168.13", 0x041da66b, "epr-19169.14", 0x91a5acef),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "mpr-19176.9", 0x2c4e90f5, "mpr-19177.10", 0xb0f1e512),
    ROM_PAIR32(ROM_MAIN_DATA, 0x1000000, "mpr-19174.7", 0x057e5200, "mpr-19175.8", 0x85254156),
    ROM_PAIR32(ROM_MAIN_DATA, 0x1800000, "mpr-19172.5", 0x9214aaaf, "mpr-19173.6", 0x31adbeed),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-19178.17", 0x0d621e21, "mpr-19180.21", 0xd2e311a5),
    ROM_PAIR32(ROM_POLYGONS, 0x0800000, "mpr-19179.18", 0x337a4ec2, "mpr-19181.22", 0x09a86c33),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-19183.27", 0x5e29074b, "mpr-19182.25", 0xc899923d),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "epr-19184.31", 0xc013a163),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-19185.32", 0x5175b7d8),
    ROM_SWAP16(ROM_SAMPLES, 0x0200000, "mpr-19186.33", 0xf23440b5),
    ROM_SWAP16(ROM_SAMPLES, 0x0400000, "mpr-19187.34", 0x20918769),
};
#define DYNABB_SIZES [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, [ROM_POLYGONS] = 0x1000000, [ROM_TEXTURES] = 0x800000, [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x600000

static const rom_part_t gunblade_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-18988a.15", 0xf63f1ad2, "epr-18989a.16", 0xc1c84d65),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-18974.11", 0xe29ecaff, "mpr-18975.12", 0xd8187582),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "mpr-18976.9", 0xc95c15eb, "mpr-18977.10", 0xdb8f5b6f),
    ROM_PAIR32(ROM_COPRO_DATA, 0x0000000, "mpr-18986.29", 0x04820f7b, "mpr-18987.30", 0x2419367f),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-18980.17", 0x22345534, "mpr-18981.21", 0x2544a33d),
    ROM_PAIR32(ROM_POLYGONS, 0x0800000, "mpr-18982.18", 0xd0a92b2a, "mpr-18983.22", 0x1b4af982),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-18985.27", 0xad6166e3, "mpr-18984.25", 0x756f6f37),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "epr-18990.31", 0x02b1b0d1),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-18978.32", 0x0f78b3e3),
    ROM_SWAP16(ROM_SAMPLES, 0x0400000, "mpr-18979.34", 0xf13ea36f),
};
#define GUNBLADE_SIZES [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, [ROM_POLYGONS] = 0x1000000, [ROM_TEXTURES] = 0x800000, [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000

/* MAME's iocpu (epr-17895.ic8, the Drive I/O board's Z80) is not in the
 * zip, and nothing on the main board reads it: board/seat_board.h answers
 * for that board. */
static const rom_part_t rchase2_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-18045a.15", 0xbfca0314, "epr-18046a.16", 0x0b8d3074),
    ROM_PAIR32(ROM_MAINCPU, 0x0100000, "epr-18074a.13", 0xca4b58df, "epr-18075a.14", 0xb82672e4),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-18037.11", 0xdea8f896, "mpr-18038.12", 0x441f7709),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0400000, "mpr-18039.9", 0xb98c6f06, "mpr-18040.10", 0x0d872667),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "mpr-18041.7", 0xe511ab0a, "mpr-18042.8", 0xe9a04159),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0c00000, "mpr-18043.5", 0xff84dfd6, "mpr-18044.6", 0xab9b406d),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-18031.17", 0x25d0deae, "mpr-18032.21", 0xdbae35c2),
    ROM_PAIR32(ROM_POLYGONS, 0x0400000, "mpr-18033.18", 0x1e75946c, "mpr-18034.22", 0x215235ad),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-18035.27", 0x4423f66e, "mpr-18036.25", 0x69221cf5),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "epr-18047.31", 0x4c31d459),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-18029.32", 0xf6804150),
    ROM_SWAP16(ROM_SAMPLES, 0x0400000, "mpr-18030.34", 0x1167615d),
};
#define RCHASE2_SIZES [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, [ROM_POLYGONS] = 0x1000000, [ROM_TEXTURES] = 0x1000000, [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000

static const rom_part_t indy500_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-18598a.15", 0x3cdcac0f, "epr-18599a.16", 0x32bde9a2),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-18237.11", 0x37e4255a, "mpr-18238.12", 0xbf837bac),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "mpr-18239.9", 0x9a2db86e, "mpr-18240.10", 0xab46a35f),
    ROM_PAIR32(ROM_MAIN_DATA, 0x1000000, "epr-18596.7", 0x8be1a5cd, "epr-18597.8", 0x44824e38),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1100000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1200000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1300000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1400000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1500000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1600000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1700000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1800000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1900000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1a00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1b00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1c00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1d00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1e00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1f00000, 0x100000),
    ROM_PAIR32(ROM_COPRO_DATA, 0x0000000, "epr-18249.29", 0xa399f023, "epr-18250.30", 0x7479ad52),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-18233.17", 0x48a024d3, "mpr-18234.21", 0x1178bfc8),
    ROM_PAIR32(ROM_POLYGONS, 0x0800000, "mpr-18235.18", 0xe7d70d59, "mpr-18236.22", 0x6ca29e0e),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-18232.27", 0xf962347d, "mpr-18231.25", 0x673d5338),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "epr-18600.31", 0x269ee4a0),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-18241.32", 0x3a380ae1),
    ROM_SWAP16(ROM_SAMPLES, 0x0200000, "mpr-18242.33", 0x1cc3deae),
    ROM_SWAP16(ROM_SAMPLES, 0x0400000, "mpr-18243.34", 0xa00a0053),
    ROM_SWAP16(ROM_SAMPLES, 0x0600000, "mpr-18244.35", 0xbfa75beb),
};
#define INDY500_SIZES [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, [ROM_POLYGONS] = 0x1000000, [ROM_TEXTURES] = 0x800000, [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000

static const rom_part_t indy500d_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-18251a.15", 0xfdabb40b, "epr-18252a.16", 0x4935832a),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-18237.11", 0x37e4255a, "mpr-18238.12", 0xbf837bac),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "mpr-18239.9", 0x9a2db86e, "mpr-18240.10", 0xab46a35f),
    ROM_PAIR32(ROM_MAIN_DATA, 0x1000000, "epr-18245.7", 0x854b1037, "epr-18246.8", 0x1a68acdc),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1100000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1200000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1300000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1400000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1500000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1600000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1700000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1800000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1900000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1a00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1b00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1c00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1d00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1e00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1f00000, 0x100000),
    ROM_PAIR32(ROM_COPRO_DATA, 0x0000000, "epr-18249.29", 0xa399f023, "epr-18250.30", 0x7479ad52),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-18233.17", 0x48a024d3, "mpr-18234.21", 0x1178bfc8),
    ROM_PAIR32(ROM_POLYGONS, 0x0800000, "mpr-18235.18", 0xe7d70d59, "mpr-18236.22", 0x6ca29e0e),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-18232.27", 0xf962347d, "mpr-18231.25", 0x673d5338),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "epr-18253.31", 0x2934e034),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-18241.32", 0x3a380ae1),
    ROM_SWAP16(ROM_SAMPLES, 0x0200000, "mpr-18242.33", 0x1cc3deae),
    ROM_SWAP16(ROM_SAMPLES, 0x0400000, "mpr-18243.34", 0xa00a0053),
    ROM_SWAP16(ROM_SAMPLES, 0x0600000, "mpr-18244.35", 0xbfa75beb),
};
#define INDY500D_SIZES [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, [ROM_POLYGONS] = 0x1000000, [ROM_TEXTURES] = 0x800000, [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000

/* MAME's cpu4: no board here reads them. */
static const rom_part_t sgt24h_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-19155.15", 0x593952fd, "epr-19156.16", 0xa91fc4ee),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-19146.11", 0xd66b5b0f, "mpr-19147.12", 0xd5558f48),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "mpr-19148.9", 0xa14c86db, "mpr-19149.10", 0x94ef5849),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-19150.17", 0xe0ad870e, "mpr-19151.21", 0xe2a1b125),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-19153.27", 0x136adfd0, "mpr-19152.25", 0x363769a2),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "epr-19157.31", 0x8ffea0cf),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-19154.32", 0x7cd9e679),
};
#define SGT24H_SIZES [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, [ROM_POLYGONS] = 0x800000, [ROM_TEXTURES] = 0x400000, [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000

/* MAME's cpu4: no board here reads them. */
static const rom_part_t overrevb_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-19992b.15", 0x6d3e78d5, "epr-19993b.16", 0x765dc9ce),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-19996.11", 0x21928a00, "mpr-19997.12", 0x2a169cab),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "mpr-19994.9", 0xe691fbd5, "mpr-19995.10", 0x82a7828e),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-19998.17", 0x6a834574, "mpr-19999.21", 0xff590a2d),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-20001.27", 0x6ca236aa, "mpr-20000.25", 0x894d8ded),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "epr-20002.31", 0x7efb069e),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-20003.32", 0x149ac22b),
    ROM_SWAP16(ROM_SAMPLES, 0x0400000, "mpr-20004.34", 0x0b9c5410),
};
#define OVERREVB_SIZES [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, [ROM_POLYGONS] = 0x800000, [ROM_TEXTURES] = 0x400000, [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000

/* MAME's cpu4: no board here reads them. */
static const rom_part_t overrev_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-20124a.15", 0x74beb8d7, "epr-20125a.16", 0xdef64456),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-19996.11", 0x21928a00, "mpr-19997.12", 0x2a169cab),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "mpr-19994.9", 0xe691fbd5, "mpr-19995.10", 0x82a7828e),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-19998.17", 0x6a834574, "mpr-19999.21", 0xff590a2d),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-20001.27", 0x6ca236aa, "mpr-20000.25", 0x894d8ded),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "epr-20002.31", 0x7efb069e),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-20003.32", 0x149ac22b),
    ROM_SWAP16(ROM_SAMPLES, 0x0400000, "mpr-20004.34", 0x0b9c5410),
};
#define OVERREV_SIZES [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, [ROM_POLYGONS] = 0x800000, [ROM_TEXTURES] = 0x400000, [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000

/* With MAME's dsbz80:mpegcpu and dsbz80:mpeg, the Z80 music board
 * (board/dsb2.h); cpu4 and drive: no board here reads them. */
static const rom_part_t stcc_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-19570.15", 0x9d6a1965, "epr-19571.16", 0x97254d16),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-19257.11", 0xac28ee24, "mpr-19258.12", 0xf5ba7d78),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "epr-19270.9", 0x7bd1d04e, "epr-19271.10", 0xd2d74f85),
    ROM_MIRROR(ROM_MAIN_DATA, 0x0800000, 0x0900000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x0800000, 0x0a00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x0800000, 0x0b00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x0800000, 0x0c00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x0800000, 0x0d00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x0800000, 0x0e00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x0800000, 0x0f00000, 0x100000),
    ROM_PAIR32(ROM_COPRO_DATA, 0x0000000, "mpr-19255.29", 0xd78bf030, "mpr-19256.30", 0xcb2b2d9e),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-19251.17", 0xe06ff7ba, "mpr-19252.21", 0x68509993),
    ROM_PAIR32(ROM_POLYGONS, 0x0800000, "epr-19266.18", 0x41464ee2, "epr-19267.22", 0x780f994d),
    ROM_MIRROR(ROM_POLYGONS, 0x0800000, 0x0900000, 0x100000),
    ROM_MIRROR(ROM_POLYGONS, 0x0800000, 0x0a00000, 0x200000),
    ROM_MIRROR(ROM_POLYGONS, 0x0800000, 0x0c00000, 0x400000),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-19254.27", 0x1ec49c02, "mpr-19253.25", 0x41ba79fb),
    ROM_MIRROR(ROM_TEXTURES, 0x0000000, 0x0400000, 0x400000),
    ROM_PAIR32(ROM_TEXTURES, 0x0800000, "epr-19269.28", 0x01881121, "epr-19268.26", 0xbc4e081c),
    ROM_MIRROR(ROM_TEXTURES, 0x0800000, 0x0900000, 0x100000),
    ROM_MIRROR(ROM_TEXTURES, 0x0800000, 0x0a00000, 0x100000),
    ROM_MIRROR(ROM_TEXTURES, 0x0800000, 0x0b00000, 0x100000),
    ROM_MIRROR(ROM_TEXTURES, 0x0800000, 0x0c00000, 0x100000),
    ROM_MIRROR(ROM_TEXTURES, 0x0800000, 0x0d00000, 0x100000),
    ROM_MIRROR(ROM_TEXTURES, 0x0800000, 0x0e00000, 0x100000),
    ROM_MIRROR(ROM_TEXTURES, 0x0800000, 0x0f00000, 0x100000),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "epr-19274.31", 0x2dcc08ae),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-19259.32", 0x4d55dbfc),
    ROM_SWAP16(ROM_SAMPLES, 0x0400000, "mpr-19261.34", 0xb88878ff),
    ROM_BYTES(ROM_MPEGCPU, 0x0000000, "epr-19275.2s", 0xee809d3f),
    ROM_BYTES(ROM_MPEG, 0x0000000, "mpr-19262.57s", 0x922aed7a),
    ROM_BYTES(ROM_MPEG, 0x0200000, "mpr-19263.58s", 0xa256f4cd),
    ROM_BYTES(ROM_MPEG, 0x0400000, "mpr-19264.59s", 0xb6c51d0f),
    ROM_BYTES(ROM_MPEG, 0x0600000, "mpr-19265.60s", 0x7d98700a),
};
#define STCC_SIZES [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, [ROM_POLYGONS] = 0x1000000, [ROM_TEXTURES] = 0x1000000, [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000, [ROM_MPEGCPU] = 0x20000, [ROM_MPEG] = 0x800000

static const rom_part_t skisuprg_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-19489.15", 0x1df948a7, "epr-19490.16", 0xe6fc24d3),
    ROM_PAIR32(ROM_MAINCPU, 0x0100000, "epr-19551.13", 0x3ee8f0d5, "epr-19552.14", 0xbaa2e49a),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-19494.11", 0xf19cdb5c, "mpr-19495.12", 0xd42e5ef2),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "mpr-19492.9", 0x4805318f, "mpr-19493.10", 0x39daa909),
    ROM_PAIR32(ROM_COPRO_DATA, 0x0000000, "mpr-19502.29", 0x2212d8d6, "mpr-19503.30", 0x3c9cfc73),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-19496.17", 0x0e9aef4e, "mpr-19497.21", 0x5397efe9),
    ROM_PAIR32(ROM_POLYGONS, 0x0800000, "mpr-19498.18", 0x32e5ae60, "mpr-19499.22", 0x2b9f5b48),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-19501.27", 0x66d7b02e, "mpr-19500.25", 0x905f5798),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "epr-19491.31", 0x1c9b15fd),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-19504.32", 0x9419ec08),
    ROM_SWAP16(ROM_SAMPLES, 0x0400000, "mpr-19505.34", 0xeba7f41d),
};
#define SKISUPRG_SIZES [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, [ROM_POLYGONS] = 0x1000000, [ROM_TEXTURES] = 0x1000000, [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000

static const rom_part_t segawski_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-19963a.15", 0x89c9cb0d, "epr-19964a.16", 0xc382cefe),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-19980.11", 0x709804f2, "mpr-19981.12", 0xa1e8ec09),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "mpr-19982.9", 0x41c398bb, "mpr-19983.10", 0xe210dea2),
    ROM_PAIR32(ROM_MAIN_DATA, 0x1000000, "mpr-19984.7", 0x14b967d1, "mpr-19985.8", 0x57827677),
    ROM_PAIR32(ROM_MAIN_DATA, 0x1800000, "epr-19961.5", 0xcc34ecaf, "epr-19962.6", 0xe8a30e5e),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1800000, 0x1900000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1800000, 0x1a00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1800000, 0x1b00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1800000, 0x1c00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1800000, 0x1d00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1800000, 0x1e00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1800000, 0x1f00000, 0x100000),
    ROM_PAIR32(ROM_COPRO_DATA, 0x0000000, "mpr-19986.29", 0x4b8e26f8, "mpr-19987.30", 0x8d5b9d38),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-19972.17", 0x493c0f8f, "mpr-19973.21", 0x01b6f8e1),
    ROM_PAIR32(ROM_POLYGONS, 0x0800000, "mpr-19974.18", 0x2bd947d3, "mpr-19975.22", 0x07e6b699),
    ROM_PAIR32(ROM_POLYGONS, 0x1000000, "mpr-19976.19", 0xf9496566, "mpr-19977.23", 0xb3dbf54b),
    ROM_PAIR32(ROM_POLYGONS, 0x1800000, "mpr-19978.20", 0xc80f4ed7, "mpr-19979.24", 0x1c0db4d2),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-19969.27", 0xc0c33d39, "mpr-19968.25", 0xb8d2f04f),
    ROM_PAIR32(ROM_TEXTURES, 0x0800000, "mpr-19971.28", 0xc8708096, "mpr-19970.26", 0xc59d8d36),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "epr-19967.31", 0xc6b8ef3f),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-19988.32", 0x303732fb),
    ROM_SWAP16(ROM_SAMPLES, 0x0400000, "mpr-19989.34", 0x8074a4b3),
};
#define SEGAWSKI_SIZES [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, [ROM_POLYGONS] = 0x2000000, [ROM_TEXTURES] = 0x1000000, [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000

static const rom_part_t waverunr_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-19282a.15", 0x5df58604, "epr-19283a.16", 0xbca188e1),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-19291.11", 0x54431d67, "mpr-19292.12", 0x9152d979),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "mpr-19293.9", 0xb168bea9, "mpr-19294.10", 0xc731e659),
    ROM_PAIR32(ROM_MAIN_DATA, 0x1000000, "epr-19278.7", 0x29ed421d, "epr-19279.8", 0x6ae9f899),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1100000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1200000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1300000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1400000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1500000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1600000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1700000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1800000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1900000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1a00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1b00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1c00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1d00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1e00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA, 0x1000000, 0x1f00000, 0x100000),
    ROM_PAIR32(ROM_COPRO_DATA, 0x0000000, "epr-19280.29", 0xc6b59fb9, "epr-19281.30", 0x5a6110e7),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-19287.17", 0x203b9e45, "mpr-19288.21", 0x4a488e4d),
    ROM_PAIR32(ROM_POLYGONS, 0x0800000, "mpr-19289.18", 0x35361cb3, "mpr-19290.22", 0x67300826),
    ROM_PAIR32(ROM_POLYGONS, 0x1000000, "epr-19304.19", 0x1a7d9521, "epr-19305.23", 0x00412412),
    ROM_MIRROR(ROM_POLYGONS, 0x1000000, 0x1100000, 0x100000),
    ROM_MIRROR(ROM_POLYGONS, 0x1000000, 0x1200000, 0x100000),
    ROM_MIRROR(ROM_POLYGONS, 0x1000000, 0x1300000, 0x100000),
    ROM_MIRROR(ROM_POLYGONS, 0x1000000, 0x1400000, 0x100000),
    ROM_MIRROR(ROM_POLYGONS, 0x1000000, 0x1500000, 0x100000),
    ROM_MIRROR(ROM_POLYGONS, 0x1000000, 0x1600000, 0x100000),
    ROM_MIRROR(ROM_POLYGONS, 0x1000000, 0x1700000, 0x100000),
    ROM_MIRROR(ROM_POLYGONS, 0x1000000, 0x1800000, 0x100000),
    ROM_MIRROR(ROM_POLYGONS, 0x1000000, 0x1900000, 0x100000),
    ROM_MIRROR(ROM_POLYGONS, 0x1000000, 0x1a00000, 0x100000),
    ROM_MIRROR(ROM_POLYGONS, 0x1000000, 0x1b00000, 0x100000),
    ROM_MIRROR(ROM_POLYGONS, 0x1000000, 0x1c00000, 0x100000),
    ROM_MIRROR(ROM_POLYGONS, 0x1000000, 0x1d00000, 0x100000),
    ROM_MIRROR(ROM_POLYGONS, 0x1000000, 0x1e00000, 0x100000),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-19286.27", 0x3ccc33cb, "mpr-19285.25", 0xa070fbd0),
    ROM_PAIR32(ROM_TEXTURES, 0x0800000, "epr-19303.28", 0xfcffc8a0, "epr-19302.26", 0xbd00933a),
    ROM_MIRROR(ROM_TEXTURES, 0x0800000, 0x0900000, 0x100000),
    ROM_MIRROR(ROM_TEXTURES, 0x0800000, 0x0a00000, 0x100000),
    ROM_MIRROR(ROM_TEXTURES, 0x0800000, 0x0b00000, 0x100000),
    ROM_MIRROR(ROM_TEXTURES, 0x0800000, 0x0c00000, 0x100000),
    ROM_MIRROR(ROM_TEXTURES, 0x0800000, 0x0d00000, 0x100000),
    ROM_MIRROR(ROM_TEXTURES, 0x0800000, 0x0e00000, 0x100000),
    ROM_MIRROR(ROM_TEXTURES, 0x0800000, 0x0f00000, 0x100000),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "epr-19284.31", 0xefe5f0f3),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-19295.32", 0xb14eeb09),
    ROM_SWAP16(ROM_SAMPLES, 0x0400000, "mpr-19296.34", 0xb4b9faff),
};
#define WAVERUNR_SIZES [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, [ROM_POLYGONS] = 0x2000000, [ROM_TEXTURES] = 0x1000000, [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000

/* With MAME's dsb2:mpegcpu and dsb2:mpeg, the music board (board/dsb2.h).
 * The other three sets are its program pair on the same data and boards. */
#define TOPSKATR_DATA \
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-19735.11", 0x8e509266, "mpr-19736.12", 0x094e0a0d), \
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "mpr-19737.9", 0x281a7dde, "mpr-19738.10", 0xf688327e), \
    ROM_PAIR32(ROM_COPRO_DATA, 0x0000000, "mpr-19743.29", 0xd41a41bf, "mpr-19744.30", 0x84f203bf), \
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-19741.17", 0x111a6e29, "mpr-19742.21", 0x28510aff), \
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-19740.27", 0xb20f508b, "mpr-19739.25", 0x8120cfd8), \
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "mpr-19759.31", 0x573530f2), \
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-19745.32", 0x7082a0af), \
    ROM_SWAP16(ROM_SAMPLES, 0x0400000, "mpr-19746.34", 0x657b5977), \
    ROM_SWAP16(ROM_MPEGCPU, 0x0000000, "mpr-19760.2s", 0x2e41ca15), \
    ROM_BYTES(ROM_MPEG, 0x0000000, "mpr-19747.18s", 0x6e895aaa), \
    ROM_BYTES(ROM_MPEG, 0x0400000, "mpr-19748.20s", 0xfcd74de3), \
    ROM_BYTES(ROM_MPEG, 0x0800000, "mpr-19749.22s", 0x842ca1eb), \
    ROM_BYTES(ROM_MPEG, 0x0c00000, "mpr-19750.24s", 0xcd95d0bf)

static const rom_part_t topskatr_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-19755a.15", 0xb80633b9, "epr-19756a.16", 0x472046a2),
    TOPSKATR_DATA,
};
static const rom_part_t topskatru_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-19753a.15", 0x3b3028de, "epr-19754a.16", 0x17535b98),
    TOPSKATR_DATA,
};
static const rom_part_t topskatruo_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-19753.15", 0x179a0954, "epr-19754.16", 0xa4c62e01),
    TOPSKATR_DATA,
};
static const rom_part_t topskatrj_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-19751.15", 0xd615a15f, "epr-19752.16", 0x42f0ba8b),
    TOPSKATR_DATA,
};
#define TOPSKATR_SIZES [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, [ROM_POLYGONS] = 0x400000, [ROM_TEXTURES] = 0x800000, [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000, [ROM_MPEGCPU] = 0x20000, [ROM_MPEG] = 0x1000000

static const rom_part_t bel_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-20225.15", 0x4abc6b59, "epr-20226.16", 0x43e05b3a),
    ROM_PAIR32(ROM_MAINCPU, 0x0040000, "epr-20223.13", 0x61b1be98, "epr-20224.14", 0xeb2d7dbf),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0000000, "mpr-20233.11", 0x3e079a3b, "mpr-20234.12", 0x58bde826),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0400000, "mpr-20231.9", 0xb3393e93, "mpr-20232.10", 0xda4a2e11),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0800000, "mpr-20229.7", 0xcdec7bf4, "mpr-20230.8", 0xa166fa87),
    ROM_PAIR32(ROM_MAIN_DATA, 0x0c00000, "mpr-20227.5", 0x1277686e, "mpr-20228.6", 0x49cb5568),
    ROM_PAIR32(ROM_COPRO_DATA, 0x0000000, "mpr-20236.29", 0x8de9a3c2, "mpr-20235.30", 0x78fa11ef),
    ROM_PAIR32(ROM_POLYGONS, 0x0000000, "mpr-20244.17", 0x9d2a8660, "mpr-20240.21", 0x51615908),
    ROM_PAIR32(ROM_POLYGONS, 0x0400000, "mpr-20243.18", 0x48671f7c, "mpr-20239.22", 0x6cd8d8a5),
    ROM_PAIR32(ROM_POLYGONS, 0x0800000, "mpr-20242.19", 0xe7f86ac7, "mpr-20238.23", 0x0a480c7c),
    ROM_PAIR32(ROM_POLYGONS, 0x0c00000, "mpr-20241.20", 0x51974b98, "mpr-20237.24", 0x89b5d8b6),
    ROM_PAIR32(ROM_TEXTURES, 0x0000000, "mpr-20247.27", 0x00b0417d, "mpr-20245.25", 0x36490a08),
    ROM_PAIR32(ROM_TEXTURES, 0x0800000, "mpr-20248.28", 0x0ace6bef, "mpr-20246.26", 0x250d6ca1),
    ROM_SWAP16(ROM_AUDIOCPU, 0x0000000, "mpr-20249.31", 0xdc24f13d),
    ROM_SWAP16(ROM_SAMPLES, 0x0000000, "mpr-20250.32", 0x91b735d3),
    ROM_SWAP16(ROM_SAMPLES, 0x0200000, "mpr-20251.33", 0x703a947b),
    ROM_SWAP16(ROM_SAMPLES, 0x0400000, "mpr-20252.34", 0x8f48f375),
    ROM_SWAP16(ROM_SAMPLES, 0x0600000, "mpr-20253.35", 0xca6aa17c),
};
#define BEL_SIZES [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, [ROM_POLYGONS] = 0x1000000, [ROM_TEXTURES] = 0xc00000, [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000


/* ---- The sets: chip list, board and 315-5881 key ------------------------ */

M2LIB_SET(manxtt,    MANXTT_SIZES,    BOARD_MODEL2A_CRX, 0)
M2LIB_SET(skytargt,  SKYTARGT_SIZES,  BOARD_MODEL2A_CRX, 0)
M2LIB_SET(vcop2,     VCOP2_SIZES,     BOARD_MODEL2A_CRX, 0)
M2LIB_SET(zeroguna,  ZEROGUNA_SIZES,  BOARD_MODEL2A_CRX, ZEROGUNA_KEY)
M2LIB_SET(vstriker,  VSTRIKER_SIZES,  BOARD_MODEL2B_CRX, 0)
M2LIB_SET(vstrikero, VSTRIKERO_SIZES, BOARD_MODEL2B_CRX, 0)
M2LIB_SET(zerogun,   ZEROGUN_SIZES,   BOARD_MODEL2B_CRX, ZEROGUN_KEY)
M2LIB_SET(zerogunj,  ZEROGUNJ_SIZES,  BOARD_MODEL2B_CRX, ZEROGUNJ_KEY)
M2LIB_RECIPE(dynabb97, NULL, 0, DYNABB97_SIZES)
M2LIB_RECIPE(dynabb, NULL, 0, DYNABB_SIZES)
M2LIB_FUNCS(dynabb, BOARD_MODEL2B_CRX, 0, &dynabb97_recipe)
M2LIB_SET(gunblade,  GUNBLADE_SIZES,  BOARD_MODEL2B_CRX, 0)
M2LIB_SET(rchase2,   RCHASE2_SIZES,   BOARD_MODEL2B_CRX, 0)
M2LIB_RECIPE(indy500, indy500to_parts, M2LIB_N(indy500to_parts), INDY500_SIZES)
M2LIB_FUNCS(indy500, BOARD_MODEL2B_CRX, 0, NULL)
M2LIB_SET(indy500d,  INDY500D_SIZES,  BOARD_MODEL2B_CRX, 0)
M2LIB_SET(sgt24h,    SGT24H_SIZES,    BOARD_MODEL2B_CRX, 0)
M2LIB_SET(overrevb,  OVERREVB_SIZES,  BOARD_MODEL2B_CRX, 0)
M2LIB_SET(overrev,   OVERREV_SIZES,   BOARD_MODEL2C_CRX, 0)
M2LIB_RECIPE(stcc, stcca_parts, M2LIB_N(stcca_parts), STCC_SIZES)
M2LIB_FUNCS(stcc, BOARD_MODEL2C_CRX, 0, NULL)
M2LIB_SET(skisuprg,  SKISUPRG_SIZES,  BOARD_MODEL2C_CRX, 0)
M2LIB_SET(segawski,  SEGAWSKI_SIZES,  BOARD_MODEL2C_CRX, 0)
M2LIB_SET(waverunr,  WAVERUNR_SIZES,  BOARD_MODEL2C_CRX, 0)
M2LIB_SET(topskatr,  TOPSKATR_SIZES,  BOARD_MODEL2C_CRX, 0)
M2LIB_SET(topskatru, TOPSKATR_SIZES,  BOARD_MODEL2C_CRX, 0)
M2LIB_SET(topskatruo, TOPSKATR_SIZES, BOARD_MODEL2C_CRX, 0)
M2LIB_SET(topskatrj, TOPSKATR_SIZES,  BOARD_MODEL2C_CRX, 0)
M2LIB_SET(bel,       BEL_SIZES,       BOARD_MODEL2C_CRX, 0)

/* ---- Controls (MAME's INPUT_PORTS) --------------------------------------
 *
 * IN0 is held bits 0-7, IN1 8-15 and IN2 16-23, as MAME lays its ports out
 * (model2crx: COIN1 0x01, COIN2 0x02, TEST 0x04, SERVICE 0x08, START1 0x10,
 * START2 0x20; IN1 / IN2 a player's B1-B4 0x01-0x08, DOWN 0x10, UP 0x20,
 * RIGHT 0x40, LEFT 0x80). What turns a pot (io_5649.h's wheel, pedals, second
 * axis, gun sticks) is held bits 24-31, which no port reads. */
#define M2LIB_CABINET \
    [GAME_INPUT_P1_COIN]  = 0x00000001, [GAME_INPUT_P2_COIN]  = 0x00000002, \
    [GAME_INPUT_TEST]     = 0x00000004, [GAME_INPUT_SERVICE]  = 0x00000008
#define M2LIB_STICKS \
    [GAME_INPUT_P1_START] = 0x00000010, [GAME_INPUT_P2_START] = 0x00000020, \
    [GAME_INPUT_P1_DOWN]  = 0x00001000, [GAME_INPUT_P1_UP]    = 0x00002000, \
    [GAME_INPUT_P1_RIGHT] = 0x00004000, [GAME_INPUT_P1_LEFT]  = 0x00008000, \
    [GAME_INPUT_P2_DOWN]  = 0x00100000, [GAME_INPUT_P2_UP]    = 0x00200000, \
    [GAME_INPUT_P2_RIGHT] = 0x00400000, [GAME_INPUT_P2_LEFT]  = 0x00800000

/* Zero Gunner, Dynamite Baseball: the stick and two buttons a player. */
#define M2LIB_TWO_BUTTONS { .bits = { M2LIB_CABINET, M2LIB_STICKS, \
    [GAME_INPUT_P1_B1] = 0x00000100, [GAME_INPUT_P1_B2] = 0x00000200, \
    [GAME_INPUT_P2_B1] = 0x00010000, [GAME_INPUT_P2_B2] = 0x00020000, \
    /* Dynamite Baseball's bat swings (io_5649.h press); no port reads them. */ \
    [GAME_INPUT_P1_B3] = 0x01000000, [GAME_INPUT_P2_B3] = 0x02000000 } }

/* Virtua Striker: in play the buttons are 2-3-1 (MAME): short pass IN1 0x04,
 * long pass 0x01, shoot 0x02. */
#define VSTRIKER_INPUT { .bits = { M2LIB_CABINET, M2LIB_STICKS, \
    [GAME_INPUT_P1_B1] = 0x00000400, [GAME_INPUT_P1_B2] = 0x00000100, \
    [GAME_INPUT_P1_B3] = 0x00000200, \
    [GAME_INPUT_P2_B1] = 0x00040000, [GAME_INPUT_P2_B2] = 0x00010000, \
    [GAME_INPUT_P2_B3] = 0x00020000 } }

/* A wheel (indy500, sgt24h, overrev, stcc): LEFT / RIGHT steer, B1 the
 * accelerator, B2 the brake; UP / DOWN shift (IN1 0x10 / 0x20, two buttons,
 * no lever), B3 / B4 the views (IN1 0x01 / 0x02), START1 IN0 0x40. */
#define M2LIB_WHEEL_INPUT { .bits = { M2LIB_CABINET, \
    [GAME_INPUT_P1_START] = 0x00000040, \
    [GAME_INPUT_P1_UP]    = 0x00001000, [GAME_INPUT_P1_DOWN]  = 0x00002000, \
    [GAME_INPUT_P1_B3]    = 0x00000100, [GAME_INPUT_P1_B4]    = 0x00000200, \
    [GAME_INPUT_P1_B1]    = 0x10000000, [GAME_INPUT_P1_B2]    = 0x20000000, \
    [GAME_INPUT_P1_RIGHT] = 0x40000000, [GAME_INPUT_P1_LEFT]  = 0x80000000 } }
#define M2LIB_WHEEL      .left = 0x80000000, .right = 0x40000000, .accel = 0x10000000, .brake = 0x20000000, \
                         .shift_up = 0x1000, .shift_down = 0x2000
/* A pot LEFT / RIGHT turn that is no car's wheel: the plain keys, not the
 * driving ones (input.h input_keys_driving wants a brake). */
#define M2LIB_STEER      .left = 0x80000000, .right = 0x40000000
#define M2LIB_WHEEL_VIEW .view = { 0x100, 0x200 }
/* The pots that walk: LEFT / RIGHT the wheel, UP / DOWN the second axis. */
#define M2LIB_AXES_INPUT \
    [GAME_INPUT_P1_RIGHT] = 0x40000000, [GAME_INPUT_P1_LEFT] = 0x80000000, \
    [GAME_INPUT_P1_UP]    = 0x04000000, [GAME_INPUT_P1_DOWN] = 0x08000000
#define M2LIB_AXES .up = 0x04000000, .down = 0x08000000

/* Manx TT: Motor Raid's bike (io_5649.h bike): B1 the throttle, B2 the
 * brake, LEFT / RIGHT lean; B3 / B4 shift up and down (IN1 0x10 / 0x20),
 * START / VR IN0 0x40. */
#define MANXTT_INPUT { .bits = { M2LIB_CABINET, \
    [GAME_INPUT_P1_START] = 0x00000040, \
    [GAME_INPUT_P1_B3]    = 0x00001000, [GAME_INPUT_P1_B4]    = 0x00002000, \
    [GAME_INPUT_P1_B1]    = 0x00010000, [GAME_INPUT_P1_B2]    = 0x00020000, \
    [GAME_INPUT_P1_RIGHT] = 0x00040000, [GAME_INPUT_P1_LEFT]  = 0x00080000 } }

/* Sky Target: the stick (STICKY channel 0, STICKX channel 2 reversed), B1 the
 * machine gun (IN1 0x10), B2 missiles (0x20), B3 the view (IN0 0x20), START1
 * IN0 0x40. */
#define SKYTARGT_INPUT { .bits = { M2LIB_CABINET, M2LIB_AXES_INPUT, \
    [GAME_INPUT_P1_START] = 0x00000040, [GAME_INPUT_P1_B3] = 0x00000020, \
    [GAME_INPUT_P1_B1]    = 0x00001000, [GAME_INPUT_P1_B2] = 0x00002000 } }

/* Ski Super G: swing (channel 0) LEFT / RIGHT, inclining (1) UP / DOWN; B1
 * Select 2 (IN0 0x80), B2 Select 3 (0x10), B3 zoom in (0x20), B4 zoom out
 * (IN1 0x01), START Select 1 (0x40); the foot sensors (IN2, active high) on
 * P2 B1 (right, 0x0F) and B2 (left, 0xF0). */
#define SKISUPRG_INPUT { .bits = { M2LIB_CABINET, M2LIB_AXES_INPUT, \
    [GAME_INPUT_P1_START] = 0x00000040, \
    [GAME_INPUT_P1_B1]    = 0x00000080, [GAME_INPUT_P1_B2] = 0x00000010, \
    [GAME_INPUT_P1_B3]    = 0x00000020, [GAME_INPUT_P1_B4] = 0x00000100, \
    [GAME_INPUT_P2_B1]    = 0x000F0000, [GAME_INPUT_P2_B2] = 0x00F00000 } }

/* Water Ski: the slide (channel 0) LEFT / RIGHT; B1 Set (IN1 0x01), B2
 * Select up (0x02), B3 / B4 pitch left / right (0x04 / 0x08), START Select
 * down (IN0 0x40). */
#define SEGAWSKI_INPUT { .bits = { M2LIB_CABINET, M2LIB_AXES_INPUT, \
    [GAME_INPUT_P1_START] = 0x00000040, \
    [GAME_INPUT_P1_B1]    = 0x00000100, [GAME_INPUT_P1_B2] = 0x00000200, \
    [GAME_INPUT_P1_B3]    = 0x00000400, [GAME_INPUT_P1_B4] = 0x00000800 } }

/* Wave Runner: the handle bar and the roll (channels 0, 1) LEFT / RIGHT, the
 * throttle (2, reversed) B1, the pitch (3, reversed) UP / DOWN; B3 the view
 * (IN1 0x01), START1 IN0 0x40. */
#define WAVERUNR_INPUT { .bits = { M2LIB_CABINET, M2LIB_AXES_INPUT, \
    [GAME_INPUT_P1_START] = 0x00000040, [GAME_INPUT_P1_B3] = 0x00000100, \
    [GAME_INPUT_P1_B1]    = 0x10000000 } }

/* Top Skater: curving (channel 0) LEFT / RIGHT, the slide (1) UP / DOWN; B1
 * jump front (IN0 0x20), B2 jump tail (IN1 0x01), B3 / B4 select left / right
 * (IN0 0x80 / 0x10), START1 IN0 0x40. */
#define TOPSKATR_INPUT { .bits = { M2LIB_CABINET, M2LIB_AXES_INPUT, \
    [GAME_INPUT_P1_START] = 0x00000040, \
    [GAME_INPUT_P1_B1]    = 0x00000020, [GAME_INPUT_P1_B2] = 0x00000100, \
    [GAME_INPUT_P1_B3]    = 0x00000080, [GAME_INPUT_P1_B4] = 0x00000010 } }

/* Gunblade NY, Behind Enemy Lines: two gun sticks on the A/D (P1 X, P2 X, P1
 * Y, P2 Y), walked by each player's stick; B1 the trigger (IN1 0x01 / 0x02);
 * Behind Enemy Lines' B2 the missile (IN1 0x10 / 0x20), and its TEST and
 * SERVICE the other way round (IN0 0x08 / 0x04, MAME: "they reversed these"). */
#define M2LIB_GUNSTICK_INPUT(test, service, p1_b2, p2_b2) { .bits = { \
    [GAME_INPUT_P1_COIN]  = 0x00000001, [GAME_INPUT_P2_COIN]  = 0x00000002, \
    [GAME_INPUT_TEST]     = test,       [GAME_INPUT_SERVICE]  = service, \
    [GAME_INPUT_P1_START] = 0x00000010, [GAME_INPUT_P2_START] = 0x00000020, \
    [GAME_INPUT_P1_B1]    = 0x00000100, [GAME_INPUT_P2_B1]    = 0x00000200, \
    [GAME_INPUT_P1_B2]    = p1_b2,      [GAME_INPUT_P2_B2]    = p2_b2, \
    [GAME_INPUT_P1_DOWN]  = 0x01000000, [GAME_INPUT_P1_UP]    = 0x02000000, \
    [GAME_INPUT_P1_RIGHT] = 0x04000000, [GAME_INPUT_P1_LEFT]  = 0x08000000, \
    [GAME_INPUT_P2_DOWN]  = 0x10000000, [GAME_INPUT_P2_UP]    = 0x20000000, \
    [GAME_INPUT_P2_RIGHT] = 0x40000000, [GAME_INPUT_P2_LEFT]  = 0x80000000 } }
#define M2LIB_GUNSTICKS \
    .gun = { \
        { .up = 0x02000000, .down = 0x01000000, .left = 0x08000000, .right = 0x04000000, \
          .trigger = 0x100, .x_min = 0x69, .x_max = 0xff, .x_rest = 0xb1, \
          .y_min = 0x11, .y_max = 0xae, .y_rest = 0x5f }, \
        { .up = 0x20000000, .down = 0x10000000, .left = 0x80000000, .right = 0x40000000, \
          .trigger = 0x200, .x_min = 0x00, .x_max = 0x96, .x_rest = 0x50, \
          .y_min = 0x11, .y_max = 0xae, .y_rest = 0x5f }, \
    }, \
    .analog = { .ch = { IO5649_AN_GUN1_X, IO5649_AN_GUN2_X, IO5649_AN_GUN1_Y, IO5649_AN_GUN2_Y } }

/* Virtua Cop 2: Virtua Cop's guns on the 315-5649's serial mux (lightgun.h),
 * as The House of the Dead's; MAME `vcop2`'s ranges. */
#define VCOP2_INPUT { .bits = { M2LIB_CABINET, M2LIB_STICKS, \
    [GAME_INPUT_P1_B1] = 0x00000100, [GAME_INPUT_P2_B1] = 0x00000200, \
    [GAME_INPUT_P1_B2] = 0x00000400, [GAME_INPUT_P2_B2] = 0x00000800 } }
#define VCOP2_GUNS \
    .lightgun = true, \
    .gun = { \
        { .up = 0x2000, .down = 0x1000, .left = 0x8000, .right = 0x4000, \
          .trigger = 0x100, .reload = 0x400, \
          .x_min = 137, .x_max = 630, .x_rest = 0x17f, .y_min = 36, .y_max = 425, .y_rest = 0x0e6 }, \
        { .up = 0x200000, .down = 0x100000, .left = 0x800000, .right = 0x400000, \
          .trigger = 0x200, .reload = 0x800, \
          .x_min = 134, .x_max = 627, .x_rest = 0x17c, .y_min = 36, .y_max = 425, .y_rest = 0x0e6 }, \
    }

/* Every set: the program's own interrupt table, the board's vblank, the
 * sound board, and a country not looked for yet. */
#define M2LIB_BASE \
    .enable_68k_sound = true, \
    .irq_vectors      = true, \
    .board_vblank     = true, \
    .region           = { .kind = REGION_TODO, .why = "country setting not looked for yet" }

/* No pot: input.h serves IN0-IN2 as they are. */
#define M2LIB_PLAIN_IO .io_5649 = false

#define M2LIB_PROFILE(name, title, brd, in, ...) \
    static const game_profile_t name##_profile = { \
        .id = #name, .display_name = title, .board = brd, \
        .load_fn = name##_load, .install_fn = name##_install, \
        .input = in, .quirks = { M2LIB_BASE, __VA_ARGS__ }, \
    }
#define M2LIB_CLONE(name, title, brd, parent, in, ...) \
    static const game_profile_t name##_profile = { \
        .id = #name, .display_name = title, .board = brd, .parent_zip_name = parent, \
        .load_fn = name##_load, .install_fn = name##_install, \
        .input = in, .quirks = { M2LIB_BASE, __VA_ARGS__ }, \
    }

/* Model 2A. Manx TT is MAME's manxttdx machine: the DX's Model 1 sound board
 * (its second voice) is not here, the CRX's own sound is. */
M2LIB_PROFILE(manxtt, "Manx TT Superbike - DX/Twin (Revision D)", BOARD_MODEL2A_CRX, MANXTT_INPUT,
              .io_5649 = true, .wheel = { .left = 0x80000, .right = 0x40000, .accel = 0x10000,
                                          .brake = 0x20000, .bike = true });
M2LIB_PROFILE(skytargt, "Sky Target", BOARD_MODEL2A_CRX, SKYTARGT_INPUT,
              .io_5649 = true, .wheel = { M2LIB_STEER },
              .analog = { .ch = { IO5649_AN_AXIS2, 0, IO5649_AN_STEER_REV }, M2LIB_AXES });
M2LIB_PROFILE(vcop2, "Virtua Cop 2", BOARD_MODEL2A_CRX, VCOP2_INPUT, VCOP2_GUNS);
M2LIB_CLONE(zeroguna, "Zero Gunner (Export, Model 2A)", BOARD_MODEL2A_CRX, "zerogun.zip",
            M2LIB_TWO_BUTTONS, M2LIB_PLAIN_IO);

/* Model 2B. */
M2LIB_PROFILE(vstriker, "Virtua Striker (Revision A)", BOARD_MODEL2B_CRX, VSTRIKER_INPUT, M2LIB_PLAIN_IO);
static const game_profile_t vstrikero_profile = {
    .id = "vstrikero", .display_name = "Virtua Striker", .board = BOARD_MODEL2B_CRX,
    .rom_set_old = "vstrikro", .parent_zip_name = "vstriker.zip",
    .load_fn = vstrikero_load, .install_fn = vstrikero_install,
    .input = VSTRIKER_INPUT, .quirks = { M2LIB_BASE },
};
M2LIB_PROFILE(zerogun, "Zero Gunner (Export, Model 2B)", BOARD_MODEL2B_CRX, M2LIB_TWO_BUTTONS,
              M2LIB_PLAIN_IO);
M2LIB_CLONE(zerogunj, "Zero Gunner (Japan, Model 2B)", BOARD_MODEL2B_CRX, "zerogun.zip",
            M2LIB_TWO_BUTTONS, M2LIB_PLAIN_IO);
M2LIB_PROFILE(dynabb, "Dynamite Baseball", BOARD_MODEL2B_CRX, M2LIB_TWO_BUTTONS,
              .io_5649 = true,
              .analog = { .ch = { IO5649_AN_PRESS1, IO5649_AN_PRESS2 },
                          .press = { 0x01000000, 0x02000000 } });
M2LIB_PROFILE(gunblade, "Gunblade NY (Revision A)", BOARD_MODEL2B_CRX,
              M2LIB_GUNSTICK_INPUT(0x04, 0x08, 0, 0), .io_5649 = true, M2LIB_GUNSTICKS);
/* Rail Chase 2: the gun sticks of Gunblade NY's input, on other channels
 * (P2 X, P1 X, P2 Y, P1 Y) and every pot read the other way (MAME
 * PORT_REVERSE), MAME `rchase2`'s ranges. Port D is the seat board's reply
 * (seat_board.h), port E its commands: the seat's moves play on the host's
 * rumble (core/ffb.h). */
M2LIB_PROFILE(rchase2, "Rail Chase 2 (Revision A)", BOARD_MODEL2B_CRX,
              M2LIB_GUNSTICK_INPUT(0x04, 0x08, 0, 0), .io_5649 = true, .seat_board = true,
              .gun = {
                  { .up = 0x02000000, .down = 0x01000000, .left = 0x08000000, .right = 0x04000000,
                    .trigger = 0x100, .x_min = 0x3a, .x_max = 0xca, .x_rest = 0x82,
                    .y_min = 0x1c, .y_max = 0xcb, .y_rest = 0x73 },
                  { .up = 0x20000000, .down = 0x10000000, .left = 0x80000000, .right = 0x40000000,
                    .trigger = 0x200, .x_min = 0x34, .x_max = 0xc7, .x_rest = 0x7d,
                    .y_min = 0x1c, .y_max = 0xcb, .y_rest = 0x73 },
              },
              .analog = { .ch = { IO5649_AN_GUN2_X, IO5649_AN_GUN1_X, IO5649_AN_GUN2_Y, IO5649_AN_GUN1_Y },
                          .rev = 0x0F });
M2LIB_PROFILE(indy500, "INDY 500 Twin (Revision A, Newer)", BOARD_MODEL2B_CRX, M2LIB_WHEEL_INPUT,
              .io_5649 = true, .wheel = { M2LIB_WHEEL, M2LIB_WHEEL_VIEW },
              .analog = { .ch = { IO5649_AN_STEER, IO5649_AN_ACCEL, IO5649_AN_BRAKE } });
M2LIB_CLONE(indy500d, "INDY 500 Deluxe (Revision A)", BOARD_MODEL2B_CRX, "indy500.zip",
            M2LIB_WHEEL_INPUT, .io_5649 = true, .wheel = { M2LIB_WHEEL, M2LIB_WHEEL_VIEW },
            .analog = { .ch = { IO5649_AN_STEER, IO5649_AN_ACCEL, IO5649_AN_BRAKE } });
/* Super GT 24h and Over Rev: the pedals read the other way (PORT_REVERSE;
 * with no NVRAM Over Rev expects that). */
#define M2LIB_REV_PEDALS \
    .io_5649 = true, .wheel = { M2LIB_WHEEL, M2LIB_WHEEL_VIEW }, \
    .analog = { .ch = { IO5649_AN_STEER, IO5649_AN_ACCEL_REV, IO5649_AN_BRAKE_REV } }
M2LIB_PROFILE(sgt24h, "Super GT 24h", BOARD_MODEL2B_CRX, M2LIB_WHEEL_INPUT, M2LIB_REV_PEDALS);
M2LIB_CLONE(overrevb, "Over Rev (Model 2B, Revision B)", BOARD_MODEL2B_CRX, "overrev.zip",
            M2LIB_WHEEL_INPUT, M2LIB_REV_PEDALS);

/* Model 2C. Touring Car and Top Skater have a DSB MPEG board for their music
 * beside the CRX's own sound (board/dsb2.h). */
M2LIB_PROFILE(overrev, "Over Rev (Model 2C, Revision A)", BOARD_MODEL2C_CRX, M2LIB_WHEEL_INPUT,
              M2LIB_REV_PEDALS);
M2LIB_PROFILE(stcc, "Sega Touring Car Championship (newer)", BOARD_MODEL2C_CRX, M2LIB_WHEEL_INPUT,
              .io_5649 = true, .wheel = { M2LIB_WHEEL, M2LIB_WHEEL_VIEW },
              .analog = { .ch = { IO5649_AN_STEER, IO5649_AN_ACCEL, IO5649_AN_BRAKE } });
M2LIB_PROFILE(skisuprg, "Sega Ski Super G", BOARD_MODEL2C_CRX, SKISUPRG_INPUT,
              .io_5649 = true, .wheel = { M2LIB_STEER },
              .analog = { .ch = { IO5649_AN_STEER, IO5649_AN_AXIS2 }, .in_high = 0x00FF0000, M2LIB_AXES });
M2LIB_PROFILE(segawski, "Sega Water Ski (Japan, Revision A)", BOARD_MODEL2C_CRX, SEGAWSKI_INPUT,
              .io_5649 = true, .wheel = { M2LIB_STEER },
              .analog = { .ch = { IO5649_AN_STEER } });
M2LIB_PROFILE(waverunr, "Wave Runner (Japan, Revision A)", BOARD_MODEL2C_CRX, WAVERUNR_INPUT,
              .io_5649 = true, .wheel = { M2LIB_STEER, .accel = 0x10000000 },
              .analog = { .ch = { IO5649_AN_STEER, IO5649_AN_STEER, IO5649_AN_ACCEL_REV,
                                  IO5649_AN_AXIS2_REV }, .in_high = 0x00080000, M2LIB_AXES });
#define TOPSKATR_QUIRKS \
    .geo_vertex_matrix = true, .io_5649 = true, .wheel = { M2LIB_STEER }, \
    .analog = { .ch = { IO5649_AN_STEER, IO5649_AN_AXIS2 }, M2LIB_AXES }
M2LIB_PROFILE(topskatr, "Top Skater (Export, Revision A)", BOARD_MODEL2C_CRX, TOPSKATR_INPUT,
              TOPSKATR_QUIRKS);
/* MAME's topskatru; older dumps name its zip topskatu. */
static const game_profile_t topskatru_profile = {
    .id = "topskatru", .display_name = "Top Skater (USA, Revision A)", .board = BOARD_MODEL2C_CRX,
    .rom_set_old = "topskatu", .parent_zip_name = "topskatr.zip",
    .load_fn = topskatru_load, .install_fn = topskatru_install,
    .input = TOPSKATR_INPUT, .quirks = { M2LIB_BASE, TOPSKATR_QUIRKS },
};
M2LIB_CLONE(topskatruo, "Top Skater (USA)", BOARD_MODEL2C_CRX, "topskatr.zip", TOPSKATR_INPUT,
            TOPSKATR_QUIRKS);
M2LIB_CLONE(topskatrj, "Top Skater (Japan)", BOARD_MODEL2C_CRX, "topskatr.zip", TOPSKATR_INPUT,
            TOPSKATR_QUIRKS);
M2LIB_PROFILE(bel, "Behind Enemy Lines", BOARD_MODEL2C_CRX,
              M2LIB_GUNSTICK_INPUT(0x08, 0x04, 0x00001000, 0x00002000), .io_5649 = true, M2LIB_GUNSTICKS);

#endif /* PROFILES_M2_LIBRARY_H */
