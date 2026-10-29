/*
 * profiles/pltkids.h — Pilot Kids, both builds MAME knows (Pinboard #1538).
 *
 * Psikyo's shoot-'em-up on the CRX boards (MAME src/mame/sega/model2.cpp,
 * ROM_START(pltkids) and ROM_START(pltkidsa)):
 *
 *   pltkids    Model 2B, Revision A   epr-21285a / 21286a   SHARC running the
 *                                     uploaded program (board/model2b_cop.h)
 *   pltkidsa   Model 2A               epr-21281 / 21282     TGP running the
 *                                     uploaded program (board/tgp.h, model2a.h)
 *
 * Only the program pair differs; pltkidsa takes every other chip from
 * pltkids.zip, and the 2A CPU board's TGP tables from its own zip. Both have
 * the 315-5881 decryption chip (board/crypt_5881.h, key 0x042e2dc1, the
 * security part 317-5044-COM).
 *
 * The program copies its PRCB to 0x500000, re-initialises the processor on it
 * (IAC 0x93), then clears work RAM 0x500000-0x54FFFF (summing it on the way,
 * the loop at 0x72C) and never writes the PRCB again. The processor keeps the
 * interrupt table and stack it read at the IAC (i960_prcb_cache), so the game
 * runs on unpatched. MAME's init_pltkids patches the clear's store at 0x730
 * out instead, because its i960 reads the PRCB again at every interrupt.
 *
 * Controls: MAME INPUT_PORTS_START(pltkids), the CRX stick and two buttons,
 * SHOT and BOMB, for each of two players: IN1 player 1, IN2 player 2, button 1
 * 0x01, 2 0x02, down 0x10, up 0x20, right 0x40, left 0x80. IN0 the cabinet's:
 * COIN1 0x01, COIN2 0x02, TEST 0x04, SERVICE 0x08, START1 0x10, START2 0x20.
 */
#ifndef PROFILES_PLTKIDS_H
#define PROFILES_PLTKIDS_H

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
#include "crypt_5881.h"
#include "savestate_reg.h"

/* ROM_PARAMETER( ":315_5881:key", "042e2dc1" ), 317-5044-COM. */
#define PLTKIDS_KEY 0x042e2dc1u

#define PLTKIDS_COMMON \
    ROM_PAIR32(ROM_MAIN_DATA,  0x0000000, "mpr-21262.da0", 0xaa71353e, "mpr-21263.da1", 0xd55d4509), \
    ROM_PAIR32(ROM_POLYGONS,   0x0000000, "mpr-21264.tp0", 0x6b35204d, "mpr-21268.tp1", 0x16ce2147), \
    ROM_PAIR32(ROM_POLYGONS,   0x0800000, "mpr-21265.tp2", 0xf061e639, "mpr-21269.tp3", 0x8c06255e), \
    ROM_PAIR32(ROM_POLYGONS,   0x1000000, "mpr-21266.tp4", 0xf9c32021, "mpr-21270.tp5", 0xb61f81c3), \
    ROM_PAIR32(ROM_POLYGONS,   0x1800000, "mpr-21267.tp6", 0xc42cc938, "mpr-21271.tp7", 0xa5325c75), \
    ROM_PAIR32(ROM_TEXTURES,   0x0000000, "mpr-21274.tx1", 0xf045e3d1, "mpr-21272.tx0", 0xdd605c21), \
    ROM_PAIR32(ROM_TEXTURES,   0x0800000, "mpr-21275.tx3", 0xc4870b7c, "mpr-21273.tx2", 0x722ec8a2), \
    ROM_SWAP16(ROM_AUDIOCPU,   0x0000000, "epr-21276.sd0", 0x8f415bc3), \
    ROM_SWAP16(ROM_SAMPLES,    0x0000000, "mpr-21277.sd1", 0xbfba0ff6), \
    ROM_SWAP16(ROM_SAMPLES,    0x0200000, "mpr-21278.sd2", 0x27e18e08), \
    ROM_SWAP16(ROM_SAMPLES,    0x0400000, "mpr-21279.sd3", 0x3a8dcf68), \
    ROM_SWAP16(ROM_SAMPLES,    0x0600000, "mpr-21280.sd4", 0xaa548124)

/* copro_data is empty on both boards (ROMREGION_ERASE00): the coprocessor's
 * program comes from the i960. */
#define PLTKIDS_SIZES_2B \
    [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000, [ROM_COPRO_DATA] = 0x800000, \
    [ROM_POLYGONS] = 0x2000000, [ROM_TEXTURES] = 0x1000000, \
    [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000
#define PLTKIDS_SIZES_2A PLTKIDS_SIZES_2B, [ROM_TGP_TABLES] = 0x40000

static const rom_part_t pltkids_parts[] = {
    ROM_PAIR32(ROM_MAINCPU,    0x0000000, "epr-21285a.15", 0xbdde5b41, "epr-21286a.16", 0xc8092e0e),
    PLTKIDS_COMMON,
};
static const rom_part_t pltkidsa_parts[] = {
    ROM_PAIR32(ROM_MAINCPU,    0x0000000, "epr-21281.pr0", 0x293ead5d, "epr-21282.pr1", 0xed0e7b9e),
    PLTKIDS_COMMON,
    /* MODEL2_CPU_BOARD: the TGP's tables, on the 2A's CPU board. */
    ROM_PAIR32(ROM_TGP_TABLES, 0x0000000, "opr-14742a.45", 0x90c6b117, "opr-14743a.46", 0xae7f446b),
};

#define PLTKIDS_RECIPE(name, sizes) \
    static const rom_recipe_t name##_recipe = { \
        .set = #name, .size = { sizes }, \
        .parts = name##_parts, .count = sizeof name##_parts / sizeof name##_parts[0], \
    }; \
    static inline int name##_load(romset_t *rs, const char *child, const char *parent) { \
        return rom_recipe_load(rs, child, parent, &name##_recipe); \
    }
PLTKIDS_RECIPE(pltkids,  PLTKIDS_SIZES_2B)
PLTKIDS_RECIPE(pltkidsa, PLTKIDS_SIZES_2A)
#undef PLTKIDS_RECIPE

static inline uint32_t pltkids_read32(const uint8_t *buf, uint32_t off) {
    return (uint32_t)buf[off] | (uint32_t)buf[off + 1] << 8
         | (uint32_t)buf[off + 2] << 16 | (uint32_t)buf[off + 3] << 24;
}

/* What both boards' installs share: the data windows, the 315-5881, the processor's start (MAME i960 device_reset: the PRCB's
 * interrupt stack, priority 31, interrupted, supervisor). */
static inline void pltkids_install_common(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    /* main_data at 0x02000000; the "extra" window at 0x06000000 is main_data
     * from 0x1000000, which this game's 8 MB of data does not reach. */
    if (rs->main_data && bus->main_data) {
        size_t n = rs->main_data_size < MAIN_DATA_SIZE ? rs->main_data_size : MAIN_DATA_SIZE;
        memcpy(bus->main_data, rs->main_data, n);
    }
    if (rs->main_data && bus->xtra_data && rs->main_data_size > 0x1000000u) {
        size_t n = rs->main_data_size - 0x1000000u;
        memcpy(bus->xtra_data, rs->main_data + 0x1000000u, n < XTRA_DATA_SIZE ? n : XTRA_DATA_SIZE);
    }
    crypt_5881_install(bus, PLTKIDS_KEY);
    savestate_extra("pltkids", "C5881", &g_c5881, sizeof g_c5881);

    uint32_t prcb = pltkids_read32(rs->maincpu, 0x04);
    cpu->prcb = prcb;
    if (prcb + 0x2C < rs->maincpu_size) {
        uint32_t isp = pltkids_read32(rs->maincpu, prcb + PRCB_INTR_STACK);
        cpu->globals.fp = isp;
        cpu->locals.sp  = isp + 64;
        cpu->sfr.pc     = 0x001F2002u;
        uint32_t start_ip_ptr = pltkids_read32(rs->maincpu, prcb + PRCB_START_IP);
        if (start_ip_ptr < rs->maincpu_size) cpu->sfr.ip = pltkids_read32(rs->maincpu, start_ip_ptr);
    }
    LOG_INFO("pltkids_install: PRCB=0x%08X initial IP=0x%08X", prcb, cpu->sfr.ip);
}

static inline void pltkids_install_2a(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!rs->loaded) { LOG_ERROR("pltkids_install: romset not loaded"); return; }
    i960_reset(cpu);
    mem_init(bus, rs->maincpu, rs->maincpu_size);
    model2a_install(bus, rs->tgp_tables, rs->copro_data, rs->copro_data_size);
    savestate_extra("pltkids", "TGP", &g_tgp, sizeof g_tgp);
    pltkids_install_common(rs, cpu, bus);
}

static inline void pltkids_install_2b(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!rs->loaded) { LOG_ERROR("pltkids_install: romset not loaded"); return; }
    i960_reset(cpu);
    mem_init(bus, rs->maincpu, rs->maincpu_size);
    model2b_cop_install(bus, rs->copro_data, rs->copro_data_size);
    savestate_extra("pltkids", "M2BCOP", &g_m2bc, sizeof g_m2bc);
    savestate_extra("pltkids", "SHARC", &g_shc, sizeof g_shc);
    pltkids_install_common(rs, cpu, bus);
}

#define PLTKIDS_INPUT { .bits = { \
    [GAME_INPUT_P1_COIN]  = 0x00000001, [GAME_INPUT_P2_COIN]  = 0x00000002, \
    [GAME_INPUT_TEST]     = 0x00000004, [GAME_INPUT_SERVICE]  = 0x00000008, \
    [GAME_INPUT_P1_START] = 0x00000010, [GAME_INPUT_P2_START] = 0x00000020, \
    [GAME_INPUT_P1_B1]    = 0x00000100, [GAME_INPUT_P1_B2]    = 0x00000200, \
    [GAME_INPUT_P1_DOWN]  = 0x00001000, [GAME_INPUT_P1_UP]    = 0x00002000, \
    [GAME_INPUT_P1_RIGHT] = 0x00004000, [GAME_INPUT_P1_LEFT]  = 0x00008000, \
    [GAME_INPUT_P2_B1]    = 0x00010000, [GAME_INPUT_P2_B2]    = 0x00020000, \
    [GAME_INPUT_P2_DOWN]  = 0x00100000, [GAME_INPUT_P2_UP]    = 0x00200000, \
    [GAME_INPUT_P2_RIGHT] = 0x00400000, [GAME_INPUT_P2_LEFT]  = 0x00800000, \
} }

/* Interrupts through the program's own table, the frame edge at the vblank,
 * no hooks: as Dynamite Cop. */
#define PLTKIDS_QUIRKS { \
    .enable_68k_sound = true, \
    .irq_vectors      = true, \
    .board_vblank     = true, \
    .region           = { .kind = REGION_TODO, \
                          .why  = "country setting not looked for yet" }, \
}

static const game_profile_t pltkids_profile = {
    .id = "pltkids", .display_name = "Pilot Kids (Model 2B, Revision A)",
    .board = BOARD_MODEL2B_CRX, .load_fn = pltkids_load, .install_fn = pltkids_install_2b,
    .input = PLTKIDS_INPUT, .quirks = PLTKIDS_QUIRKS,
};
static const game_profile_t pltkidsa_profile = {
    .id = "pltkidsa", .display_name = "Pilot Kids (Model 2A)",
    .parent_zip_name = "pltkids.zip",
    .board = BOARD_MODEL2A_CRX, .load_fn = pltkidsa_load, .install_fn = pltkids_install_2a,
    .input = PLTKIDS_INPUT, .quirks = PLTKIDS_QUIRKS,
};

#endif /* PROFILES_PLTKIDS_H */
