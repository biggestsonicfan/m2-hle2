/*
 * profiles/airwlkrs.h — Air Walkers (Data East, 1997), Model 2A (Pinboard #1063).
 *
 * The only build MAME knows (src/mame/sega/model2.cpp, ROM_START(airwlkrs),
 * model2a_airwlkrs_state): a CRX board, the TGP running the program the i960
 * uploads (board/tgp.h, board/model2a.h), no coprocessor data ROMs. The second
 * data pair is 1 MB and MAME copies it up to 0xFFFFFF (ROM_COPY), so every
 * mirror the program might read is filled.
 *
 * Controls (MAME INPUT_PORTS_START(airwlkrs)): an 8-way stick and three
 * buttons for each player, IN1 player 1 and IN2 player 2 as every two-player
 * Model 2 game; button 1 0x01, 2 0x02, 3 0x04, down 0x10, up 0x20, right 0x40,
 * left 0x80. IN0 is the cabinet's: COIN1 0x01, COIN2 0x02, TEST 0x04, SERVICE
 * 0x08, START1 0x10, START2 0x20, COIN3 0x40, COIN4 0x80.
 *
 * The cabinet takes four players: port F bit 7 switches IN1 / IN2 and the
 * start bits to players 3 and 4 (quirks.four_player_sides, input.h), and the
 * game only raises it with GAME OPTIONS -> PLAYER SELECTION at 4P SIMULTANEOUS
 * (the default is 2P). Players 3 and 4 play from g_input.far_held: the third
 * and fourth pad (libretro ports, web and SDL3 pads) or p3:/p4: macros; a
 * session (netplay, replays, follow) carries two players, so they read
 * released there.
 *
 * MAME marks the set MACHINE_NOT_WORKING | MACHINE_IMPERFECT_SOUND.
 */
#ifndef PROFILES_AIRWLKRS_H
#define PROFILES_AIRWLKRS_H

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
#include "savestate_reg.h"

static const rom_part_t airwlkrs_parts[] = {
    ROM_PAIR32(ROM_MAINCPU,    0x000000, "j_2-14_ic12_fe7e.12", 0x8851a8d7, "j_2-14_ic13_d539.13", 0x72287ee0),
    ROM_PAIR32(ROM_MAIN_DATA,  0x000000, "mpr-19236.10",    0x3c26e978, "mpr-19237.11",    0x961328b1),
    ROM_PAIR32(ROM_MAIN_DATA,  0x800000, "11-7_ic8_d400.8", 0x37f300bd, "11-7_ic9_6e4a.9", 0x454e4a09),
    ROM_MIRROR(ROM_MAIN_DATA,  0x800000, 0x900000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x800000, 0xa00000, 0x200000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x800000, 0xc00000, 0x400000),
    ROM_PAIR32(ROM_POLYGONS,   0x000000, "mpr-19235.16", 0xeaad8f92, "mpr-19232.20", 0xfd153001),
    ROM_PAIR32(ROM_TEXTURES,   0x000000, "mpr-19234.25", 0xd7d69493, "mpr-19233.24", 0x7a2e51f1),
    ROM_SWAP16(ROM_AUDIOCPU,   0x000000, "10-18_ic30_30f2.30", 0xde335a79),
    ROM_SWAP16(ROM_SAMPLES,    0x000000, "mpr-19243.31", 0x10f530c0),
    ROM_SWAP16(ROM_SAMPLES,    0x200000, "mpr-19242.32", 0xc0772a28),
    ROM_SWAP16(ROM_SAMPLES,    0x400000, "mpr-19241.36", 0x226fa430),
    ROM_SWAP16(ROM_SAMPLES,    0x600000, "mpr-19240.37", 0xfb6edae7),
    /* MODEL2_CPU_BOARD: the TGP's tables, on the 2A's CPU board. */
    ROM_PAIR32(ROM_TGP_TABLES, 0x000000, "opr-14742a.45", 0x90c6b117, "opr-14743a.46", 0xae7f446b),
};

/* copro_data is empty (ROMREGION_ERASE00): the TGP's program comes from the i960. */
static const rom_recipe_t airwlkrs_recipe = {
    .set = "airwlkrs",
    .size = {
        [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2400000, [ROM_COPRO_DATA] = 0x800000,
        [ROM_POLYGONS] = 0x2000000, [ROM_TEXTURES] = 0x1000000,
        [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000, [ROM_TGP_TABLES] = 0x40000,
    },
    .parts = airwlkrs_parts, .count = sizeof airwlkrs_parts / sizeof airwlkrs_parts[0],
};

static inline int airwlkrs_load(romset_t *rs, const char *child, const char *parent) {
    return rom_recipe_load(rs, child, parent, &airwlkrs_recipe);
}

static inline uint32_t airwlkrs_read32(const uint8_t *buf, uint32_t off) {
    return (uint32_t)buf[off] | (uint32_t)buf[off + 1] << 8
         | (uint32_t)buf[off + 2] << 16 | (uint32_t)buf[off + 3] << 24;
}

static inline void airwlkrs_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!rs->loaded) { LOG_ERROR("airwlkrs_install: romset not loaded"); return; }
    i960_reset(cpu);
    mem_init(bus, rs->maincpu, rs->maincpu_size);
    model2a_install(bus, rs->tgp_tables, rs->copro_data, rs->copro_data_size);
    savestate_extra("airwlkrs", "TGP", &g_tgp, sizeof g_tgp);

    /* main_data at 0x02000000; the "extra" window at 0x06000000 is main_data
     * from 0x1000000, empty here (MAME model2_base_mem). */
    if (rs->main_data && bus->main_data) {
        size_t n = rs->main_data_size < MAIN_DATA_SIZE ? rs->main_data_size : MAIN_DATA_SIZE;
        memcpy(bus->main_data, rs->main_data, n);
    }
    if (rs->main_data && bus->xtra_data && rs->main_data_size > 0x1000000u) {
        size_t n = rs->main_data_size - 0x1000000u;
        memcpy(bus->xtra_data, rs->main_data + 0x1000000u, n < XTRA_DATA_SIZE ? n : XTRA_DATA_SIZE);
    }

    /* MAME i960 device_reset: the PRCB's interrupt stack, priority 31,
     * interrupted, supervisor. */
    uint32_t prcb = airwlkrs_read32(rs->maincpu, 0x04);
    cpu->prcb = prcb;
    if (prcb + 0x2C < rs->maincpu_size) {
        uint32_t isp = airwlkrs_read32(rs->maincpu, prcb + PRCB_INTR_STACK);
        cpu->globals.fp = isp;
        cpu->locals.sp  = isp + 64;
        cpu->sfr.pc     = 0x001F2002u;
        uint32_t start_ip_ptr = airwlkrs_read32(rs->maincpu, prcb + PRCB_START_IP);
        if (start_ip_ptr < rs->maincpu_size) cpu->sfr.ip = airwlkrs_read32(rs->maincpu, start_ip_ptr);
    }
    LOG_INFO("airwlkrs_install: PRCB=0x%08X initial IP=0x%08X", prcb, cpu->sfr.ip);
}

static const game_profile_t airwlkrs_profile = {
    .id = "airwlkrs", .display_name = "Air Walkers",
    .board = BOARD_MODEL2A_CRX, .load_fn = airwlkrs_load, .install_fn = airwlkrs_install,
    .input = { .bits = {
        [GAME_INPUT_P1_COIN]  = 0x00000001, [GAME_INPUT_P2_COIN]  = 0x00000002,
        [GAME_INPUT_TEST]     = 0x00000004, [GAME_INPUT_SERVICE]  = 0x00000008,
        [GAME_INPUT_P1_START] = 0x00000010, [GAME_INPUT_P2_START] = 0x00000020,
        [GAME_INPUT_P1_B1]    = 0x00000100, [GAME_INPUT_P1_B2]    = 0x00000200,
        [GAME_INPUT_P1_B3]    = 0x00000400,
        [GAME_INPUT_P1_DOWN]  = 0x00001000, [GAME_INPUT_P1_UP]    = 0x00002000,
        [GAME_INPUT_P1_RIGHT] = 0x00004000, [GAME_INPUT_P1_LEFT]  = 0x00008000,
        [GAME_INPUT_P2_B1]    = 0x00010000, [GAME_INPUT_P2_B2]    = 0x00020000,
        [GAME_INPUT_P2_B3]    = 0x00040000,
        [GAME_INPUT_P2_DOWN]  = 0x00100000, [GAME_INPUT_P2_UP]    = 0x00200000,
        [GAME_INPUT_P2_RIGHT] = 0x00400000, [GAME_INPUT_P2_LEFT]  = 0x00800000,
    } },
    /* Interrupts through the program's own table, the frame edge at the
     * vblank, no hooks: as Dead or Alive's 2A builds. */
    .quirks = {
        .enable_68k_sound = true,
        .irq_vectors      = true,
        .board_vblank     = true,
        .four_player_sides = true,
        /* The program ROM is Data East's AOU show build ("Ver 0.99.99a JAPAN
         * 97-01-23 AOU SHOW version" at 0xC6A50): the warning names only
         * Japan, and the test menu's COIN and GAME OPTIONS have no country. */
        .region           = { .kind = REGION_PROGRAM_ROM,
                              .why  = "the program ROM is Japan's AOU show build, with no country setting" },
    },
};

#endif /* PROFILES_AIRWLKRS_H */
