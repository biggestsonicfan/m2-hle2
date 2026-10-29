/*
 * profiles/dynamcop.h — Dynamite Cop / Dynamite Deka 2 (Pinboard #965).
 *
 * One game on all three CRX boards, the same data ROMs on each (MAME
 * src/mame/sega/model2.cpp, ROM_START(dynamcop) and its clones):
 *
 *   dynamcop   Export, Model 2A   TGP running the program the game uploads
 *   dyndeka2   Japan,  Model 2A   (board/tgp.h, board/model2a.h)
 *   dynamcopb  Export, Model 2B   SHARC running the uploaded program
 *   dyndeka2b  Japan,  Model 2B   (board/model2b_cop.h)
 *   dynamcopc  USA,    Model 2C   TGPx4 running the uploaded program
 *                                 (board/tgpx4.h, board/model2c_cop.h)
 *
 * Only the first program pair differs between them. Older MAME named three of
 * them dynmcopb, dyndek2b and dynmcopc (rom_set_old), and the dynmcopb dump
 * names its program chips progrom0.15 / progrom1.16; the chips are found by
 * CRC, so either loads.
 *
 * Every board has the 315-5881 decryption chip (board/crypt_5881.h, key
 * 0x2c2a4a93, the security part 317-0236-COM): the game writes enciphered
 * data into RAM at 0x01D80000 and reads it back plain from 0x01D90000.
 *
 * Controls: an 8-way stick and PUNCH / KICK / JUMP for each of two players
 * (MAME INPUT_PORTS_START(dynamcop)): IN1 player 1, IN2 player 2, button 1
 * 0x01, 2 0x02, 3 0x04, down 0x10, up 0x20, right 0x40, left 0x80. IN0 the
 * cabinet's: COIN1 0x01, COIN2 0x02, TEST 0x04, SERVICE 0x08, START1 0x10,
 * START2 0x20.
 */
#ifndef PROFILES_DYNAMCOP_H
#define PROFILES_DYNAMCOP_H

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
#include "savestate_reg.h"

/* ROM_PARAMETER( ":315_5881:key", "2c2a4a93" ), 317-0236-COM. */
#define DYNAMCOP_KEY 0x2c2a4a93u

/* The chips every board has: the program's second pair, data, polygons,
 * textures, sound. The program's first pair is the board's and region's. */
#define DYNAMCOP_PROGRAM_HI(lo, hi) \
    ROM_PAIR32(ROM_MAINCPU,    0x0100000, lo,              0x618a68bf, hi,              0x13abe49c)
#define DYNAMCOP_COMMON \
    ROM_PAIR32(ROM_MAIN_DATA,  0x0000000, "mpr-20797.10",  0x87bab1e4, "mpr-20798.11",  0x40dd752b), \
    ROM_PAIR32(ROM_MAIN_DATA,  0x0800000, "mpr-20795.8",   0x0ef85e12, "mpr-20796.9",   0x870139cb), \
    ROM_PAIR32(ROM_MAIN_DATA,  0x1000000, "mpr-20793.6",   0x42ea08f8, "mpr-20794.7",   0x8e5cd1db), \
    ROM_PAIR32(ROM_MAIN_DATA,  0x1800000, "mpr-20791.4",   0x4883d0df, "mpr-20792.5",   0x47becfa2), \
    ROM_PAIR32(ROM_POLYGONS,   0x0000000, "mpr-20799.16",  0x424571bf, "mpr-20803.20",  0x61a8ad52), \
    ROM_PAIR32(ROM_POLYGONS,   0x0800000, "mpr-20800.17",  0x3c2ee808, "mpr-20804.21",  0x03b35cb8), \
    ROM_PAIR32(ROM_POLYGONS,   0x1000000, "mpr-20801.18",  0xc6914173, "mpr-20805.22",  0xf6605ede), \
    ROM_PAIR32(ROM_POLYGONS,   0x1800000, "mpr-20802.19",  0xd11b5267, "mpr-20806.23",  0x0c942073), \
    ROM_PAIR32(ROM_TEXTURES,   0x0000000, "mpr-20809.25",  0x3b7b4622, "mpr-20807.24",  0x1241e0f2), \
    ROM_PAIR32(ROM_TEXTURES,   0x0800000, "mpr-20810.27",  0x838a10a7, "mpr-20808.26",  0x706bd495), \
    ROM_SWAP16(ROM_AUDIOCPU,   0x0000000, "epr-20811.30",  0xa154b83e), \
    ROM_SWAP16(ROM_SAMPLES,    0x0000000, "mpr-20812.31",  0x695b6088), \
    ROM_SWAP16(ROM_SAMPLES,    0x0200000, "mpr-20813.32",  0x1908679c), \
    ROM_SWAP16(ROM_SAMPLES,    0x0400000, "mpr-20814.36",  0xe8ebc74c), \
    ROM_SWAP16(ROM_SAMPLES,    0x0600000, "mpr-20815.37",  0x1b5aaae4)
/* MODEL2_CPU_BOARD: the TGP's tables, on the 2A's CPU board. */
#define DYNAMCOP_TGP_TABLES \
    ROM_PAIR32(ROM_TGP_TABLES, 0x0000000, "opr-14742a.45", 0x90c6b117, "opr-14743a.46", 0xae7f446b)

/* copro_data is empty on every board (ROMREGION_ERASE00): the coprocessor's
 * program comes from the i960. */
#define DYNAMCOP_SIZES_2B \
    [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, \
    [ROM_POLYGONS] = 0x2000000, [ROM_TEXTURES] = 0x1000000, \
    [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000
#define DYNAMCOP_SIZES_2A DYNAMCOP_SIZES_2B, [ROM_TGP_TABLES] = 0x40000

static const rom_part_t dynamcop_parts[] = {
    ROM_PAIR32(ROM_MAINCPU,    0x0000000, "epr-20930.12",  0xb8fc8ff7, "epr-20931.13",  0x89d13f88),
    DYNAMCOP_PROGRAM_HI("epr-20932.14", "epr-20933.15"), DYNAMCOP_COMMON, DYNAMCOP_TGP_TABLES,
};
static const rom_part_t dyndeka2_parts[] = {
    ROM_PAIR32(ROM_MAINCPU,    0x0000000, "epr-20922.12",  0x0a8b5604, "epr-20923.13",  0x83be73d4),
    DYNAMCOP_PROGRAM_HI("epr-20924.14", "epr-20925.15"), DYNAMCOP_COMMON, DYNAMCOP_TGP_TABLES,
};
static const rom_part_t dynamcopb_parts[] = {
    ROM_PAIR32(ROM_MAINCPU,    0x0000000, "epr-20944.15",  0x29b142f2, "epr-20945.16",  0xc495912e),
    DYNAMCOP_PROGRAM_HI("epr-20942.13", "epr-20943.14"), DYNAMCOP_COMMON,
};
static const rom_part_t dyndeka2b_parts[] = {
    ROM_PAIR32(ROM_MAINCPU,    0x0000000, "epr-20936.15",  0x23ef98f2, "epr-20937.16",  0x25a14e00),
    DYNAMCOP_PROGRAM_HI("epr-20934.13", "epr-20935.14"), DYNAMCOP_COMMON,
};
static const rom_part_t dynamcopc_parts[] = {
    ROM_PAIR32(ROM_MAINCPU,    0x0000000, "epr-20952.15",  0xec8bc896, "epr-20953.16",  0xa8276ffd),
    DYNAMCOP_PROGRAM_HI("epr-20950.13", "epr-20951.14"), DYNAMCOP_COMMON,
};

#define DYNAMCOP_RECIPE(name, sizes) \
    static const rom_recipe_t name##_recipe = { \
        .set = #name, .size = { sizes }, \
        .parts = name##_parts, .count = sizeof name##_parts / sizeof name##_parts[0], \
    }; \
    static inline int name##_load(romset_t *rs, const char *child, const char *parent) { \
        return rom_recipe_load(rs, child, parent, &name##_recipe); \
    }
DYNAMCOP_RECIPE(dynamcop,  DYNAMCOP_SIZES_2A)
DYNAMCOP_RECIPE(dyndeka2,  DYNAMCOP_SIZES_2A)
DYNAMCOP_RECIPE(dynamcopb, DYNAMCOP_SIZES_2B)
DYNAMCOP_RECIPE(dyndeka2b, DYNAMCOP_SIZES_2B)
DYNAMCOP_RECIPE(dynamcopc, DYNAMCOP_SIZES_2B)
#undef DYNAMCOP_RECIPE

static inline uint32_t dynamcop_read32(const uint8_t *buf, uint32_t off) {
    return (uint32_t)buf[off] | (uint32_t)buf[off + 1] << 8
         | (uint32_t)buf[off + 2] << 16 | (uint32_t)buf[off + 3] << 24;
}

/* What every board's install shares: the data windows, the 315-5881, the
 * processor's start (MAME i960 device_reset: the PRCB's interrupt stack,
 * priority 31, interrupted, supervisor). The bus is mem_init's already. */
static inline void dynamcop_install_common(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    /* main_data at 0x02000000; the "extra" window at 0x06000000 is main_data
     * from 0x1000000 (MAME model2_base_mem). */
    if (rs->main_data && bus->main_data) {
        size_t n = rs->main_data_size < MAIN_DATA_SIZE ? rs->main_data_size : MAIN_DATA_SIZE;
        memcpy(bus->main_data, rs->main_data, n);
    }
    if (rs->main_data && bus->xtra_data && rs->main_data_size > 0x1000000u) {
        size_t n = rs->main_data_size - 0x1000000u;
        memcpy(bus->xtra_data, rs->main_data + 0x1000000u, n < XTRA_DATA_SIZE ? n : XTRA_DATA_SIZE);
    }
    crypt_5881_install(bus, DYNAMCOP_KEY);
    savestate_extra("dynamcop", "C5881", &g_c5881, sizeof g_c5881);

    uint32_t prcb = dynamcop_read32(rs->maincpu, 0x04);
    cpu->prcb = prcb;
    if (prcb + 0x2C < rs->maincpu_size) {
        uint32_t isp = dynamcop_read32(rs->maincpu, prcb + PRCB_INTR_STACK);
        cpu->globals.fp = isp;
        cpu->locals.sp  = isp + 64;
        cpu->sfr.pc     = 0x001F2002u;
        uint32_t start_ip_ptr = dynamcop_read32(rs->maincpu, prcb + PRCB_START_IP);
        if (start_ip_ptr < rs->maincpu_size) cpu->sfr.ip = dynamcop_read32(rs->maincpu, start_ip_ptr);
    }
    LOG_INFO("dynamcop_install: PRCB=0x%08X initial IP=0x%08X", prcb, cpu->sfr.ip);
}

static inline void dynamcop_install_2a(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!rs->loaded) { LOG_ERROR("dynamcop_install: romset not loaded"); return; }
    i960_reset(cpu);
    mem_init(bus, rs->maincpu, rs->maincpu_size);
    model2a_install(bus, rs->tgp_tables, rs->copro_data, rs->copro_data_size);
    savestate_extra("dynamcop", "TGP", &g_tgp, sizeof g_tgp);
    dynamcop_install_common(rs, cpu, bus);
}

static inline void dynamcop_install_2b(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!rs->loaded) { LOG_ERROR("dynamcop_install: romset not loaded"); return; }
    i960_reset(cpu);
    mem_init(bus, rs->maincpu, rs->maincpu_size);
    model2b_cop_install(bus, rs->copro_data, rs->copro_data_size);
    savestate_extra("dynamcop", "M2BCOP", &g_m2bc, sizeof g_m2bc);
    savestate_extra("dynamcop", "SHARC", &g_shc, sizeof g_shc);
    dynamcop_install_common(rs, cpu, bus);
}

static inline void dynamcop_install_2c(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!rs->loaded) { LOG_ERROR("dynamcop_install: romset not loaded"); return; }
    i960_reset(cpu);
    mem_init(bus, rs->maincpu, rs->maincpu_size);
    model2c_cop_install(bus, rs->copro_data, rs->copro_data_size);
    savestate_extra("dynamcop", "TGPX4", &g_tgx, sizeof g_tgx);
    dynamcop_install_common(rs, cpu, bus);
}

#define DYNAMCOP_INPUT { .bits = { \
    [GAME_INPUT_P1_COIN]  = 0x00000001, [GAME_INPUT_P2_COIN]  = 0x00000002, \
    [GAME_INPUT_TEST]     = 0x00000004, [GAME_INPUT_SERVICE]  = 0x00000008, \
    [GAME_INPUT_P1_START] = 0x00000010, [GAME_INPUT_P2_START] = 0x00000020, \
    [GAME_INPUT_P1_B1]    = 0x00000100, [GAME_INPUT_P1_B2]    = 0x00000200, \
    [GAME_INPUT_P1_B3]    = 0x00000400, \
    [GAME_INPUT_P1_DOWN]  = 0x00001000, [GAME_INPUT_P1_UP]    = 0x00002000, \
    [GAME_INPUT_P1_RIGHT] = 0x00004000, [GAME_INPUT_P1_LEFT]  = 0x00008000, \
    [GAME_INPUT_P2_B1]    = 0x00010000, [GAME_INPUT_P2_B2]    = 0x00020000, \
    [GAME_INPUT_P2_B3]    = 0x00040000, \
    [GAME_INPUT_P2_DOWN]  = 0x00100000, [GAME_INPUT_P2_UP]    = 0x00200000, \
    [GAME_INPUT_P2_RIGHT] = 0x00400000, [GAME_INPUT_P2_LEFT]  = 0x00800000, \
} }

/* Interrupts through the program's own table, the frame edge at the vblank,
 * no hooks: as Virtual-On and Sega Rally. */
#define DYNAMCOP_QUIRKS { \
    .enable_68k_sound = true, \
    .irq_vectors      = true, \
    .board_vblank     = true, \
    /* Export and Japan are two program ROMs; whether the test menu has a */ \
    /* country setting as well is not looked at yet. */ \
    .region           = { .kind = REGION_TODO, \
                          .why  = "country setting not looked for yet" }, \
    /* The model table, in the data ROMs every build shares: 19432 entries */ \
    /* of 16 bytes at data 0xD40000, the 1995 layout, read by the draw */ \
    /* routines as 0x2D40000[n*16]. 50 entries are empty (the table is laid */ \
    /* out in 51 banks); the other 19382 meshes are all in the polygon ROM. */ \
    /* prepared_test holds it to the explorer's reads (NOCLIP-SYNC.md §8.2). */ \
    .model_table_offset = 0xD40000, \
    .model_table_count  = 19432, \
    .mesh_ptr_subtract  = 0x02000010, \
    .mesh_ptr_add       = 0x10, \
}

static const game_profile_t dynamcop_profile = {
    .id = "dynamcop", .display_name = "Dynamite Cop (Export, Model 2A)",
    .board = BOARD_MODEL2A_CRX, .load_fn = dynamcop_load, .install_fn = dynamcop_install_2a,
    .input = DYNAMCOP_INPUT, .quirks = DYNAMCOP_QUIRKS,
};
static const game_profile_t dyndeka2_profile = {
    .id = "dyndeka2", .display_name = "Dynamite Deka 2 (Japan, Model 2A)",
    .parent_zip_name = "dynamcop.zip",
    .board = BOARD_MODEL2A_CRX, .load_fn = dyndeka2_load, .install_fn = dynamcop_install_2a,
    .input = DYNAMCOP_INPUT, .quirks = DYNAMCOP_QUIRKS,
};
static const game_profile_t dynamcopb_profile = {
    .id = "dynamcopb", .display_name = "Dynamite Cop (Export, Model 2B)",
    .rom_set_old = "dynmcopb", .parent_zip_name = "dynamcop.zip",
    .board = BOARD_MODEL2B_CRX, .load_fn = dynamcopb_load, .install_fn = dynamcop_install_2b,
    .input = DYNAMCOP_INPUT, .quirks = DYNAMCOP_QUIRKS,
};
static const game_profile_t dyndeka2b_profile = {
    .id = "dyndeka2b", .display_name = "Dynamite Deka 2 (Japan, Model 2B)",
    .rom_set_old = "dyndek2b", .parent_zip_name = "dynamcop.zip",
    .board = BOARD_MODEL2B_CRX, .load_fn = dyndeka2b_load, .install_fn = dynamcop_install_2b,
    .input = DYNAMCOP_INPUT, .quirks = DYNAMCOP_QUIRKS,
};
static const game_profile_t dynamcopc_profile = {
    .id = "dynamcopc", .display_name = "Dynamite Cop (USA, Model 2C)",
    .rom_set_old = "dynmcopc", .parent_zip_name = "dynamcop.zip",
    .board = BOARD_MODEL2C_CRX, .load_fn = dynamcopc_load, .install_fn = dynamcop_install_2c,
    .input = DYNAMCOP_INPUT, .quirks = DYNAMCOP_QUIRKS,
};

#endif /* PROFILES_DYNAMCOP_H */
