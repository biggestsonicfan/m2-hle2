/*
 * profiles/doa.h — Dead or Alive, every build MAME knows (Pinboard #1050).
 *
 * Tecmo's game on two CRX boards, the same data, model, texture and sound
 * chips on each (MAME src/mame/sega/model2.cpp, ROM_START(doa) and its
 * clones; noclip js/games.js, `doab` and the builds relocated from it):
 *
 *   doa     Model 2B, Revision C (Jan 10 1997)   SHARC running the program
 *   doab    Model 2B, Revision B (Dec  6 1996)   the game uploads
 *                                                (board/model2b_cop.h)
 *   doaa    Model 2A, Revision A (Dec  4 1996)   TGP running the uploaded
 *   doaab   Model 2A             (Nov  3 1996)   program (board/tgp.h,
 *   doaae   Model 2A, Export, Revision A         board/model2a.h)
 *
 * Only the program pair differs between builds. The 2A seats the other chips
 * in other sockets, so MAME's 2A sets name them by those (noclip's
 * DOA_2A_SOCKETS); the chips are found by CRC, so a merged set loads either way.
 *
 * Every board has the 317-0229 compression chip (board/crypt_0229.h): the
 * game checks a string it reads from it at boot.
 *
 * Controls: an 8-way stick and HOLD / PUNCH / KICK for each of two players
 * (MAME INPUT_PORTS_START(doa)): IN1 player 1, IN2 player 2, button 1 0x01,
 * 2 0x02, 3 0x04, down 0x10, up 0x20, right 0x40, left 0x80. IN0 the
 * cabinet's: COIN1 0x01, COIN2 0x02, TEST 0x04, SERVICE 0x08, START1 0x10,
 * START2 0x20.
 *
 * The model table is noclip's (`modelTable`: 19330 entries of 16 bytes at data
 * 0x3A160), the board's mesh-pointer encoding, as STF's.
 */
#ifndef PROFILES_DOA_H
#define PROFILES_DOA_H

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
#include "crypt_0229.h"
#include "savestate_reg.h"

/* The chips every build has, in the 2B's sockets and the 2A's. The last data
 * pair is 2 MB a half, and MAME copies it up to 0x1C00000 as well (ROM_COPY). */
#define DOA_COMMON(d11, d12, d9, d10, d7, d8, d5, d6, p17, p21, p18, p22, p19, p23, \
                   t27, t25, s31, s32, s33, s34, s35) \
    ROM_PAIR32(ROM_MAIN_DATA,  0x0000000, d11, 0xab431bfe, d12, 0xc5cb694d), \
    ROM_PAIR32(ROM_MAIN_DATA,  0x0800000, d9,  0x2d2d1b1a, d10, 0x96b17bcf), \
    ROM_PAIR32(ROM_MAIN_DATA,  0x1000000, d7,  0xa8d963fb, d8,  0x90ae5682), \
    ROM_PAIR32(ROM_MAIN_DATA,  0x1800000, d5,  0x1dcedb10, d6,  0x8c63055e), \
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1c00000, 0x400000), \
    ROM_PAIR32(ROM_POLYGONS,   0x0000000, p17, 0xd0e6ecf0, p21, 0x7cbe432d), \
    ROM_PAIR32(ROM_POLYGONS,   0x0800000, p18, 0x453d3f4a, p22, 0xb976da02), \
    ROM_PAIR32(ROM_POLYGONS,   0x1000000, p19, 0x0d6bf454, p23, 0x6a75634c), \
    ROM_PAIR32(ROM_TEXTURES,   0x0000000, t27, 0x9c49e845, t25, 0x190c017f), \
    ROM_SWAP16(ROM_AUDIOCPU,   0x0000000, s31, 0x400bdbfb), \
    ROM_SWAP16(ROM_SAMPLES,    0x0000000, s32, 0x8fd2708a), \
    ROM_SWAP16(ROM_SAMPLES,    0x0200000, s33, 0x0c69787d), \
    ROM_SWAP16(ROM_SAMPLES,    0x0400000, s34, 0xc18ea0b8), \
    ROM_SWAP16(ROM_SAMPLES,    0x0600000, s35, 0x2877f96f)
#define DOA_COMMON_2B DOA_COMMON( \
    "mpr-19318.11", "mpr-19319.12", "mpr-19316.9",  "mpr-19317.10", \
    "mpr-19314.7",  "mpr-19315.8",  "mpr-19312.5",  "mpr-19313.6", \
    "mpr-19322.17", "mpr-19325.21", "mpr-19323.18", "mpr-19326.22", \
    "mpr-19324.19", "mpr-19327.23", "mpr-19321.27", "mpr-19320.25", \
    "epr-19328.31", "mpr-19329.32", "mpr-19330.33", "mpr-19331.34", "mpr-19332.35")
#define DOA_COMMON_2A DOA_COMMON( \
    "mpr-19318.10", "mpr-19319.11", "mpr-19316.8",  "mpr-19317.9", \
    "mpr-19314.6",  "mpr-19315.7",  "mpr-19312.4",  "mpr-19313.5", \
    "mpr-19322.16", "mpr-19325.20", "mpr-19323.17", "mpr-19326.21", \
    "mpr-19324.18", "mpr-19327.22", "mpr-19321.25", "mpr-19320.24", \
    "epr-19328.30", "mpr-19329.31", "mpr-19330.32", "mpr-19331.36", "mpr-19332.37"), \
    /* MODEL2_CPU_BOARD: the TGP's tables, on the 2A's CPU board. */ \
    ROM_PAIR32(ROM_TGP_TABLES, 0x0000000, "opr-14742a.45", 0x90c6b117, "opr-14743a.46", 0xae7f446b)

/* copro_data is empty on both boards (ROMREGION_ERASE00): the coprocessor's
 * program comes from the i960. */
#define DOA_SIZES_2B \
    [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, \
    [ROM_POLYGONS] = 0x2000000, [ROM_TEXTURES] = 0x800000, \
    [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000
#define DOA_SIZES_2A DOA_SIZES_2B, [ROM_TGP_TABLES] = 0x40000

static const rom_part_t doa_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-19379c.15", 0x5cc62fbe, "epr-19380c.16", 0x58cfeaa9),
    DOA_COMMON_2B,
};
static const rom_part_t doab_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-19379b.15", 0x8a10a944, "epr-19380b.16", 0x766c1ec8),
    DOA_COMMON_2B,
};
static const rom_part_t doaa_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-19310a.12", 0x06486f7a, "epr-19311a.13", 0x1be62912),
    DOA_COMMON_2A,
};
static const rom_part_t doaab_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-19310.12",  0x3c571e75, "epr-19311.13",  0x8818363d),
    DOA_COMMON_2A,
};
static const rom_part_t doaae_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-19383a.12", 0x42e61481, "epr-19384a.13", 0x034a3ab9),
    DOA_COMMON_2A,
};

#define DOA_RECIPE(name, sizes) \
    static const rom_recipe_t name##_recipe = { \
        .set = #name, .size = { sizes }, \
        .parts = name##_parts, .count = sizeof name##_parts / sizeof name##_parts[0], \
    }; \
    static inline int name##_load(romset_t *rs, const char *child, const char *parent) { \
        return rom_recipe_load(rs, child, parent, &name##_recipe); \
    }
DOA_RECIPE(doa,   DOA_SIZES_2B)
DOA_RECIPE(doab,  DOA_SIZES_2B)
DOA_RECIPE(doaa,  DOA_SIZES_2A)
DOA_RECIPE(doaab, DOA_SIZES_2A)
DOA_RECIPE(doaae, DOA_SIZES_2A)
#undef DOA_RECIPE

static inline uint32_t doa_read32(const uint8_t *buf, uint32_t off) {
    return (uint32_t)buf[off] | (uint32_t)buf[off + 1] << 8
         | (uint32_t)buf[off + 2] << 16 | (uint32_t)buf[off + 3] << 24;
}

/* What both boards' installs share: the data windows, the 317-0229, the
 * processor's start (MAME i960 device_reset: the PRCB's interrupt stack,
 * priority 31, interrupted, supervisor). The bus is mem_init's already. */
static inline void doa_install_common(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
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
    crypt_0229_install(bus);
    savestate_extra("doa", "C0229", &g_c0229, sizeof g_c0229);

    uint32_t prcb = doa_read32(rs->maincpu, 0x04);
    cpu->prcb = prcb;
    if (prcb + 0x2C < rs->maincpu_size) {
        uint32_t isp = doa_read32(rs->maincpu, prcb + PRCB_INTR_STACK);
        cpu->globals.fp = isp;
        cpu->locals.sp  = isp + 64;
        cpu->sfr.pc     = 0x001F2002u;
        uint32_t start_ip_ptr = doa_read32(rs->maincpu, prcb + PRCB_START_IP);
        if (start_ip_ptr < rs->maincpu_size) cpu->sfr.ip = doa_read32(rs->maincpu, start_ip_ptr);
    }
    LOG_INFO("doa_install: PRCB=0x%08X initial IP=0x%08X", prcb, cpu->sfr.ip);
}

static inline void doa_install_2a(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!rs->loaded) { LOG_ERROR("doa_install: romset not loaded"); return; }
    i960_reset(cpu);
    mem_init(bus, rs->maincpu, rs->maincpu_size);
    model2a_install(bus, rs->tgp_tables, rs->copro_data, rs->copro_data_size);
    savestate_extra("doa", "TGP", &g_tgp, sizeof g_tgp);
    doa_install_common(rs, cpu, bus);
}

static inline void doa_install_2b(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!rs->loaded) { LOG_ERROR("doa_install: romset not loaded"); return; }
    i960_reset(cpu);
    mem_init(bus, rs->maincpu, rs->maincpu_size);
    model2b_cop_install(bus, rs->copro_data, rs->copro_data_size);
    savestate_extra("doa", "M2BCOP", &g_m2bc, sizeof g_m2bc);
    savestate_extra("doa", "SHARC", &g_shc, sizeof g_shc);
    doa_install_common(rs, cpu, bus);
}

#define DOA_INPUT { .bits = { \
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
 * no hooks: as Virtual-On, Sega Rally and Dynamite Cop. */
#define DOA_QUIRKS(region_kind, region_why) { \
    .enable_68k_sound   = true, \
    .irq_vectors        = true, \
    .board_vblank       = true, \
    .region             = { .kind = region_kind, .why = region_why }, \
    .model_table_offset = 0x3a160, \
    .model_table_count  = 19330, \
    .mesh_ptr_subtract  = 0x02000010, \
    .mesh_ptr_add       = 0x10, \
}
/* MAME: every build but doaae defaults to Japan and offers Japan, USA and
 * Export in the test menu; where it keeps that is not looked at yet. */
#define DOA_QUIRKS_MENU   DOA_QUIRKS(REGION_TODO, "a test-menu country; where it is kept not looked for yet")
#define DOA_QUIRKS_EXPORT DOA_QUIRKS(REGION_PROGRAM_ROM, "the program ROM is locked to Export")

static const game_profile_t doa_profile = {
    .id = "doa", .display_name = "Dead or Alive (Model 2B, Rev C)",
    .board = BOARD_MODEL2B_CRX, .load_fn = doa_load, .install_fn = doa_install_2b,
    .input = DOA_INPUT, .quirks = DOA_QUIRKS_MENU,
};
static const game_profile_t doab_profile = {
    .id = "doab", .display_name = "Dead or Alive (Model 2B, Rev B)",
    .parent_zip_name = "doa.zip",
    .board = BOARD_MODEL2B_CRX, .load_fn = doab_load, .install_fn = doa_install_2b,
    .input = DOA_INPUT, .quirks = DOA_QUIRKS_MENU,
};
static const game_profile_t doaa_profile = {
    .id = "doaa", .display_name = "Dead or Alive (Model 2A, Rev A)",
    .parent_zip_name = "doa.zip",
    .board = BOARD_MODEL2A_CRX, .load_fn = doaa_load, .install_fn = doa_install_2a,
    .input = DOA_INPUT, .quirks = DOA_QUIRKS_MENU,
};
static const game_profile_t doaab_profile = {
    .id = "doaab", .display_name = "Dead or Alive (Model 2A)",
    .parent_zip_name = "doa.zip",
    .board = BOARD_MODEL2A_CRX, .load_fn = doaab_load, .install_fn = doa_install_2a,
    .input = DOA_INPUT, .quirks = DOA_QUIRKS_MENU,
};
static const game_profile_t doaae_profile = {
    .id = "doaae", .display_name = "Dead or Alive (Export, Model 2A, Rev A)",
    .parent_zip_name = "doa.zip",
    .board = BOARD_MODEL2A_CRX, .load_fn = doaae_load, .install_fn = doa_install_2a,
    .input = DOA_INPUT, .quirks = DOA_QUIRKS_EXPORT,
};

#endif /* PROFILES_DOA_H */
