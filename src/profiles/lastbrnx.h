/*
 * profiles/lastbrnx.h — Last Bronx, every build MAME knows (Pinboard #1517).
 *
 * Sega AM3's fighter on the Model 2B CRX, the board STF, Fighting Vipers and
 * Dead or Alive run on (MAME src/mame/sega/model2.cpp, ROM_START(lastbrnx)
 * and its clones):
 *
 *   lastbrnx    Export, Revision A   epr-19061a / 19062a
 *   lastbrnxu   USA,    Revision A   epr-19059a / 19060a
 *   lastbrnxj   Japan,  Revision A   epr-19046a / 19047a  (Tokyo Bangaichi;
 *                                    older MAME's lastbrnj)
 *
 * Only the program pair differs between builds; the others take every other
 * chip from lastbrnx.zip. The COP runs the program the game uploads
 * (board/model2b_cop.h): the i960 streams it into the SHARC's external port
 * (IOP EPB0 at 0x008C0010, DMA6 set up through IOP 0x40-0x42 and 0x1C) where
 * Virtual-On and Dead or Alive feed the FIFO. There is no protection chip.
 * The title picture is drawn straight into the frame buffer (board/fbvram.h).
 *
 * Controls: MAME INPUT_PORTS_START(vf2), an 8-way stick and PUNCH / KICK /
 * GUARD for each of two players: IN1 player 1, IN2 player 2, button 1 0x01,
 * 2 0x02, 3 0x04, down 0x10, up 0x20, right 0x40, left 0x80. IN0 the
 * cabinet's: COIN1 0x01, COIN2 0x02, TEST 0x04, SERVICE 0x08, START1 0x10,
 * START2 0x20.
 *
 * No model table in the quirks: noclip found it in the program ROM (0x189B0,
 * 2544 entries of 16 bytes, sent unchanged by the draws at 0x4370C and
 * 0xAD930), and the quirks' table is a main_data offset.
 */
#ifndef PROFILES_LASTBRNX_H
#define PROFILES_LASTBRNX_H

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
#include "model2b_cop.h"
#include "fbvram.h"
#include "savestate_reg.h"

#define LASTBRNX_COMMON \
    ROM_PAIR32(ROM_MAIN_DATA,  0x0000000, "mpr-19050.11", 0xe6af2b61, "mpr-19051.12", 0x14b88961), \
    ROM_PAIR32(ROM_MAIN_DATA,  0x0800000, "mpr-19048.9",  0x02180215, "mpr-19049.10", 0xdb7eecd6), \
    ROM_PAIR32(ROM_POLYGONS,   0x0000000, "mpr-19052.17", 0xd7f27216, "mpr-19053.21", 0x1f328465), \
    ROM_PAIR32(ROM_TEXTURES,   0x0000000, "mpr-19055.27", 0x85a57d49, "mpr-19054.25", 0x05366277), \
    ROM_SWAP16(ROM_AUDIOCPU,   0x0000000, "mpr-19056.31", 0x22a22918), \
    ROM_SWAP16(ROM_SAMPLES,    0x0000000, "mpr-19057.32", 0x64809438), \
    ROM_SWAP16(ROM_SAMPLES,    0x0400000, "mpr-19058.34", 0xe237c11c)

/* copro_data is empty (ROMREGION_ERASE00): the coprocessor's program comes
 * from the i960. */
#define LASTBRNX_SIZES \
    [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, \
    [ROM_POLYGONS] = 0x800000, [ROM_TEXTURES] = 0x800000, \
    [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000

static const rom_part_t lastbrnx_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-19061a.15", 0xc0aebab2, "epr-19062a.16", 0xcdf597e8),
    LASTBRNX_COMMON,
};
static const rom_part_t lastbrnxu_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-19059a.15", 0x25478257, "epr-19060a.16", 0xc48906b2),
    LASTBRNX_COMMON,
};
static const rom_part_t lastbrnxj_parts[] = {
    ROM_PAIR32(ROM_MAINCPU, 0x0000000, "epr-19046a.15", 0x75be7b7a, "epr-19047a.16", 0x1f5541e2),
    LASTBRNX_COMMON,
};

#define LASTBRNX_RECIPE(name) \
    static const rom_recipe_t name##_recipe = { \
        .set = #name, .size = { LASTBRNX_SIZES }, \
        .parts = name##_parts, .count = sizeof name##_parts / sizeof name##_parts[0], \
    }; \
    static inline int name##_load(romset_t *rs, const char *child, const char *parent) { \
        return rom_recipe_load(rs, child, parent, &name##_recipe); \
    }
LASTBRNX_RECIPE(lastbrnx)
LASTBRNX_RECIPE(lastbrnxu)
LASTBRNX_RECIPE(lastbrnxj)
#undef LASTBRNX_RECIPE

static inline uint32_t lastbrnx_read32(const uint8_t *buf, uint32_t off) {
    return (uint32_t)buf[off] | (uint32_t)buf[off + 1] << 8
         | (uint32_t)buf[off + 2] << 16 | (uint32_t)buf[off + 3] << 24;
}

/* The bus is mem_init's, the COP the uploaded program's; main_data at
 * 0x02000000; the processor's start is MAME's i960 device_reset (the PRCB's
 * interrupt stack, priority 31, interrupted, supervisor). */
static inline void lastbrnx_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!rs->loaded) { LOG_ERROR("lastbrnx_install: romset not loaded"); return; }
    i960_reset(cpu);
    mem_init(bus, rs->maincpu, rs->maincpu_size);
    model2b_cop_install(bus, rs->copro_data, rs->copro_data_size);
    savestate_extra("lastbrnx", "M2BCOP", &g_m2bc, sizeof g_m2bc);
    savestate_extra("lastbrnx", "SHARC", &g_shc, sizeof g_shc);
    fbvram_install(bus, "lastbrnx");   /* the title picture */

    if (rs->main_data && bus->main_data) {
        size_t n = rs->main_data_size < MAIN_DATA_SIZE ? rs->main_data_size : MAIN_DATA_SIZE;
        memcpy(bus->main_data, rs->main_data, n);
    }

    uint32_t prcb = lastbrnx_read32(rs->maincpu, 0x04);
    cpu->prcb = prcb;
    if (prcb + 0x2C < rs->maincpu_size) {
        uint32_t isp = lastbrnx_read32(rs->maincpu, prcb + PRCB_INTR_STACK);
        cpu->globals.fp = isp;
        cpu->locals.sp  = isp + 64;
        cpu->sfr.pc     = 0x001F2002u;
        uint32_t start_ip_ptr = lastbrnx_read32(rs->maincpu, prcb + PRCB_START_IP);
        if (start_ip_ptr < rs->maincpu_size) cpu->sfr.ip = lastbrnx_read32(rs->maincpu, start_ip_ptr);
    }
    LOG_INFO("lastbrnx_install: PRCB=0x%08X initial IP=0x%08X", prcb, cpu->sfr.ip);
}

#define LASTBRNX_INPUT { .bits = { \
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
 * no hooks: as Virtual-On and Dead or Alive. Each program pair is one
 * country (MAME has one set per country). */
#define LASTBRNX_QUIRKS { \
    .enable_68k_sound = true, \
    .irq_vectors      = true, \
    .board_vblank     = true, \
    .region           = { .kind = REGION_PROGRAM_ROM, .why = "one program pair per country (Export, USA, Japan)" }, \
}

static const game_profile_t lastbrnx_profile = {
    .id = "lastbrnx", .display_name = "Last Bronx (Export, Rev A)",
    .board = BOARD_MODEL2B_CRX, .load_fn = lastbrnx_load, .install_fn = lastbrnx_install,
    .input = LASTBRNX_INPUT, .quirks = LASTBRNX_QUIRKS,
};
static const game_profile_t lastbrnxu_profile = {
    .id = "lastbrnxu", .display_name = "Last Bronx (USA, Rev A)",
    .rom_set_old = "lastbrnu", .parent_zip_name = "lastbrnx.zip",
    .board = BOARD_MODEL2B_CRX, .load_fn = lastbrnxu_load, .install_fn = lastbrnx_install,
    .input = LASTBRNX_INPUT, .quirks = LASTBRNX_QUIRKS,
};
static const game_profile_t lastbrnxj_profile = {
    .id = "lastbrnxj", .display_name = "Last Bronx: Tokyo Bangaichi (Japan, Rev A)",
    .rom_set_old = "lastbrnj", .parent_zip_name = "lastbrnx.zip",
    .board = BOARD_MODEL2B_CRX, .load_fn = lastbrnxj_load, .install_fn = lastbrnx_install,
    .input = LASTBRNX_INPUT, .quirks = LASTBRNX_QUIRKS,
};

#endif /* PROFILES_LASTBRNX_H */
