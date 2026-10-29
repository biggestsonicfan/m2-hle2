/*
 * profiles/hotd.h — The House of the Dead (hotd: Revision A, or the first
 * release, MAME's hotdo). Runs since Pinboard #1034.
 *
 * Pinboard #660 (parity with noclip) prepared it: the explorer reads this set
 * (vendor/noclip js/games.js, `hotd` / `hotdo`), so the profile loads the same
 * ROMs with the same model table, and prepared_test still holds the decoder to
 * noclip's reads of it. ROM names, CRCs and offsets are MAME's
 * (src/mame/sega/model2.cpp, ROM_START(hotd)).
 *
 * The board is a Model 2C: the i960 uploads a program to its MB86235 TGPx4,
 * which board/tgpx4.h runs as for Dynamite Cop's 2C (board/model2c_cop.h).
 * The sound board is STF's SCSP + 68000, with the UART at 0x01C80000. The two
 * light guns are on the I/O chip's second serial channel (board/lightgun.h;
 * docs/invariants/memory-bus.md): the pad's directions walk a gun, B1 fires,
 * B2 reloads, and a host's pointer aims a gun (ui/lightgun_host.h). The only
 * HLE hooks are the region's two (below).
 */
#ifndef PROFILES_HOTD_H
#define PROFILES_HOTD_H

#include <stdint.h>
#include <string.h>

#include "constants.h"
#include "log.h"
#include "rom_loader.h"
#include "rom_recipe.h"
#include "game_profile.h"
#include "memory.h"
#include "i960.h"
#include "model2c_cop.h"
#include "lightgun.h"
#include "savestate_reg.h"
#include "hle_hooks.h"     /* g_region */

/* Everything but the first program pair, which is the only one hotdo's differs in. */
#define HOTD_COMMON \
    ROM_PAIR32(ROM_MAINCPU,    0x0100000, "epr-19694.13",  0xe85ca1a3, "epr-19695.14",  0xcd52b461), \
    ROM_PAIR32(ROM_MAIN_DATA,  0x0000000, "mpr-19704.11",  0xaa80dbb0, "mpr-19705.12",  0xf906843b), \
    ROM_PAIR32(ROM_MAIN_DATA,  0x0800000, "mpr-19702.9",   0xfc8aa3b7, "mpr-19703.10",  0x208d993d), \
    ROM_PAIR32(ROM_MAIN_DATA,  0x1000000, "mpr-19700.7",   0x0558cfd3, "mpr-19701.8",   0x224a8929), \
    ROM_PAIR32(ROM_MAIN_DATA,  0x1800000, "epr-19698.5",   0xe7a7b6ea, "epr-19699.6",   0x8160b3d9), \
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1900000, 0x100000), \
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1a00000, 0x100000), \
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1b00000, 0x100000), \
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1c00000, 0x100000), \
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1d00000, 0x100000), \
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1e00000, 0x100000), \
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1f00000, 0x100000), \
    /* The TGPx4's program. */ \
    ROM_PAIR32(ROM_COPRO_DATA, 0x0000000, "epr-19707.29",  0x384fd133, "epr-19706.30",  0x1277531c), \
    ROM_PAIR32(ROM_POLYGONS,   0x0000000, "mpr-19715.17",  0x3ff7dda7, "mpr-19711.21",  0x080d13f1), \
    ROM_PAIR32(ROM_POLYGONS,   0x0800000, "mpr-19714.18",  0x3e55ab49, "mpr-19710.22",  0x80df1036), \
    ROM_PAIR32(ROM_POLYGONS,   0x1000000, "mpr-19713.19",  0x4d092cd3, "mpr-19709.23",  0xd08937bf), \
    ROM_PAIR32(ROM_POLYGONS,   0x1800000, "mpr-19712.20",  0x41577943, "mpr-19708.24",  0x5cb790f2), \
    ROM_PAIR32(ROM_TEXTURES,   0x0000000, "mpr-19718.27",  0xa9de5924, "mpr-19716.25",  0x45c7dcce), \
    ROM_PAIR32(ROM_TEXTURES,   0x0800000, "mpr-19719.28",  0x838f8343, "mpr-19717.26",  0x393e440b), \
    ROM_SWAP16(ROM_AUDIOCPU,   0x0000000, "epr-19720.31",  0xb367d21d), \
    ROM_SWAP16(ROM_SAMPLES,    0x0000000, "mpr-19721.32",  0xf5d8fa9a), \
    ROM_SWAP16(ROM_SAMPLES,    0x0400000, "mpr-19722.34",  0xa56fa539)

static const rom_part_t hotd_parts[] = {
    ROM_PAIR32(ROM_MAINCPU,    0x0000000, "epr-19696a.15", 0x42adc32e, "epr-19697a.16", 0x1e247cd5),
    HOTD_COMMON,
};

/* MAME's hotdo, the first release: only the first program pair differs. */
static const rom_part_t hotd_older_parts[] = {
    ROM_PAIR32(ROM_MAINCPU,    0x0000000, "epr-19696.15",  0x03da5623, "epr-19697.16",  0xa9722d87),
};

static const rom_recipe_t hotd_recipe = {
    .set  = "hotd",
    .size = { [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000,
              [ROM_COPRO_DATA] = 0x800000,
              [ROM_POLYGONS] = 0x2000000, [ROM_TEXTURES] = 0x1000000,
              [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000 },
    .parts = hotd_parts, .count = sizeof hotd_parts / sizeof hotd_parts[0],
    .older = hotd_older_parts, .older_count = sizeof hotd_older_parts / sizeof hotd_older_parts[0],
};

static inline int hotd_load(romset_t *rs, const char *child, const char *parent) {
    return rom_recipe_load(rs, child, parent, &hotd_recipe);
}

/* MAME's hotdo as a set of its own (Pinboard #1286): hotd's chips with the
 * first release's program pair, from hotdo.zip or its parent hotd.zip. */
static const rom_part_t hotdo_parts[] = {
    ROM_PAIR32(ROM_MAINCPU,    0x0000000, "epr-19696.15",  0x03da5623, "epr-19697.16",  0xa9722d87),
    HOTD_COMMON,
};
#undef HOTD_COMMON

static const rom_recipe_t hotdo_recipe = {
    .set  = "hotdo",
    .size = { [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000,
              [ROM_COPRO_DATA] = 0x800000,
              [ROM_POLYGONS] = 0x2000000, [ROM_TEXTURES] = 0x1000000,
              [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000 },
    .parts = hotdo_parts, .count = sizeof hotdo_parts / sizeof hotdo_parts[0],
};

static inline int hotdo_load(romset_t *rs, const char *child, const char *parent) {
    return rom_recipe_load(rs, child, parent, &hotdo_recipe);
}

/* The prototype (hotdp, Pinboard #1286): flash modules, not mask ROMs, so
 * every chip is its own, and a 1 MB program. MAME ROM_START(hotdp); the
 * mirror of 0x1800000 copies a window no chip fills, as MAME's does. */
static const rom_part_t hotdp_parts[] = {
    ROM_PAIR32(ROM_MAINCPU,    0x0000000, "prg0.15",    0x548ed10a, "prg1.16",    0xf43bb51f),
    ROM_PAIR32(ROM_MAIN_DATA,  0x0000000, "dat0.11",    0x8d40fc82, "dat1.12",    0x63e04c15),
    ROM_PAIR32(ROM_MAIN_DATA,  0x0800000, "dat2.9",     0x2aa9e4b9, "dat3.10",    0x356d348b),
    ROM_PAIR32(ROM_MAIN_DATA,  0x1000000, "dat4.7",     0x7ec403f6, "dat5.8",     0x592fac50),
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1900000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1a00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1b00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1c00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1d00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1e00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1f00000, 0x100000),
    ROM_PAIR32(ROM_COPRO_DATA, 0x0000000, "copro0.29",  0xfc2380f5, "copro1.30",  0xe6ae8f3c),
    ROM_PAIR32(ROM_POLYGONS,   0x0000000, "tgp0.17",    0xb458ec9b, "tgp1.21",    0x4b250500),
    ROM_PAIR32(ROM_POLYGONS,   0x0800000, "tgp2.18",    0x17f68d25, "tgp3.22",    0xcaff1d48),
    ROM_PAIR32(ROM_POLYGONS,   0x1000000, "tgp4.19",    0x8854f204, "tgp5.23",    0x29f311f3),
    ROM_PAIR32(ROM_TEXTURES,   0x0000000, "tex1.27",    0xeea00bdf, "tex0.25",    0xfb10366a),
    ROM_PAIR32(ROM_TEXTURES,   0x0800000, "tex3.28",    0x9a61d7e8, "tex2.26",    0x84ec2923),
    ROM_SWAP16(ROM_AUDIOCPU,   0x0000000, "sndpgm0.31", 0x30accd2e),
    ROM_SWAP16(ROM_SAMPLES,    0x0000000, "sound1.32",  0xe0a8dd56),
    ROM_SWAP16(ROM_SAMPLES,    0x0200000, "sound2.33",  0xa517834f),
    ROM_SWAP16(ROM_SAMPLES,    0x0400000, "sound3.34",  0xf0c529bb),
    ROM_SWAP16(ROM_SAMPLES,    0x0600000, "sound4.35",  0x3ad48d53),
};

static const rom_recipe_t hotdp_recipe = {
    .set  = "hotdp",
    .size = { [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000,
              [ROM_COPRO_DATA] = 0x800000,
              [ROM_POLYGONS] = 0x2000000, [ROM_TEXTURES] = 0x1000000,
              [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000 },
    .parts = hotdp_parts, .count = sizeof hotdp_parts / sizeof hotdp_parts[0],
};

static inline int hotdp_load(romset_t *rs, const char *child, const char *parent) {
    return rom_recipe_load(rs, child, parent, &hotdp_recipe);
}

static inline uint32_t hotd_read32(const uint8_t *buf, uint32_t off) {
    return (uint32_t)buf[off] | (uint32_t)buf[off + 1] << 8
         | (uint32_t)buf[off + 2] << 16 | (uint32_t)buf[off + 3] << 24;
}

/*
 * The region (Pinboard #1040). The settings are in the 93C46 EEPROM, as Sega
 * Rally's, not in backup RAM. At boot 0x1DF90 reads it to 0x51F090: the magic
 * "S32A", version 0x6655, then a sum at 0x51F098 over 38 bytes from 0x51F09A
 * and a mirror of both at 0x51F0C0. COUNTRY is 0x51F0A9 (EEPROM byte 0x19;
 * mirror 0x51F0D1): 0 Japan, 1 USA, 2 Export, the test menu's (0x9DC8) "JAPAN",
 * "USA", "EXPORT" at 0xA91D0; get_country (0x1F050) takes the ROM's 0xAA531,
 * Japan, over anything above 2. After the load, 0x13CD0 copies it to
 * 0x51F111, which the game reads.
 *
 * country_default (0x1E0A8): the first store after 0x1E1D0 has written the
 * factory settings (0x1E0A0 runs only on the default paths of 0x1DF90). Put
 * g_region in COUNTRY; 0x1E0C0 then sums it, mirrors it and writes the EEPROM.
 * Japan is the ROM's default and MAME's, so --region japan leaves the board as
 * it was.
 *
 * settings_loaded (0x168C8, `call 0x13CD0` in the boot after 0x1DF90): with
 * g_region_over_saved, put g_region in COUNTRY over the one a saved EEPROM
 * brought, before 0x13CD0 copies it on. The EEPROM keeps its own until the
 * test menu next saves (0x8764).
 *
 * Both are hotdo's addresses (the container's hotd.zip; checked in MAME). The
 * Revision A program is another build: install checks the two instructions and
 * leaves the hooks idle on any other program.
 */
#define HOTD_HOOK_ADDR_COUNTRY_DEFAULT  0x0001E0A8u
#define HOTD_HOOK_ADDR_SETTINGS_LOADED  0x000168C8u
#define HOTD_COUNTRY                    0x0051F0A9u

/*
 * The prototype (hotdp, Pinboard #1286) keeps the same block at 0x51E480
 * ("S32A", version 0x8708, 22 words): 0x180B0 writes the factory settings
 * out of ROM 0x82990 (Japan), the store after it at 0x17F28 (`st r4,
 * 0x51E48C`) would be country_default, and COUNTRY is 0x51E499, which the
 * game reads itself (0xF720, 0xFB38, 0x15C28); 0x11DE8, the boot's call
 * after the EEPROM read, would be settings_loaded. It takes no hooks: only
 * Japan reaches the title. With USA (1) or Export (2) the boot's state
 * (0x51F218, set at 0xF75C) is the FBI notice or the export warning, and
 * neither ever moves on (their object at 0x53DEF0 never ticks). MAME boots it
 * Japanese, and marks it MACHINE_NOT_WORKING.
 */

/* What install checks the program for, and the COUNTRY byte its hooks write. */
typedef struct {
    uint32_t country_default, country_default_word;
    uint32_t settings_loaded, settings_loaded_word;
    uint32_t country;
} hotd_build_t;

static const hotd_build_t k_hotdo_build = {
    HOTD_HOOK_ADDR_COUNTRY_DEFAULT, 0x92203000u,   /* st r4, 0x51F09C */
    HOTD_HOOK_ADDR_SETTINGS_LOADED, 0x09FFD408u,   /* call 0x13CD0 */
    HOTD_COUNTRY,
};

static bool     g_hotd_region_idle;   /* the program is not the build's: the hooks do nothing */
static uint32_t g_hotd_country = HOTD_COUNTRY;

static int hotd_hook_country_default(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    if (!g_hotd_region_idle) mem_write8(bus, g_hotd_country, (uint8_t)g_region);
    return 1;
}

static uint32_t hotd_country(memory_bus_t *bus) { (void)bus; return g_hotd_country; }

static int hotd_hook_settings_loaded(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    if (g_region_over_saved && !g_hotd_region_idle) mem_write8(bus, hotd_country(bus), (uint8_t)g_region);
    return 1;
}

/* main_data at 0x02000000; the "extra" window at 0x06000000 is main_data
 * from 0x1000000 (MAME model2_base_mem). */
static inline void hotd__data_windows(const romset_t *rs, memory_bus_t *bus) {
    if (rs->main_data && bus->main_data) {
        size_t n = rs->main_data_size < MAIN_DATA_SIZE ? rs->main_data_size : MAIN_DATA_SIZE;
        memcpy(bus->main_data, rs->main_data, n);
    }
    if (rs->main_data && bus->xtra_data && rs->main_data_size > 0x1000000u) {
        size_t n = rs->main_data_size - 0x1000000u;
        memcpy(bus->xtra_data, rs->main_data + 0x1000000u, n < XTRA_DATA_SIZE ? n : XTRA_DATA_SIZE);
    }
}

/* Whether the program is not build k's: its two hooked instructions differ. */
static inline bool hotd__not_build(const romset_t *rs, const hotd_build_t *k) {
    return rs->maincpu_size <= k->country_default + 4u || rs->maincpu_size <= k->settings_loaded + 4u
        || hotd_read32(rs->maincpu, k->settings_loaded) != k->settings_loaded_word
        || hotd_read32(rs->maincpu, k->country_default) != k->country_default_word;
}

/* The 2C board (model2c_cop.h), the data windows, and the processor's start
 * (MAME i960 device_reset: the PRCB's interrupt stack, priority 31,
 * interrupted, supervisor), as Dynamite Cop's 2C less the 315-5881. */
static inline void hotd_install_build(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus,
                                      const hotd_build_t *k) {
    if (!rs->loaded) { LOG_ERROR("hotd_install: romset not loaded"); return; }
    i960_reset(cpu);
    mem_init(bus, rs->maincpu, rs->maincpu_size);
    model2c_cop_install(bus, rs->copro_data, rs->copro_data_size);
    savestate_extra("hotd", "TGPX4", &g_tgx, sizeof g_tgx);
    savestate_extra("hotd", "GUN", &g_lightgun, sizeof g_lightgun);
    hotd__data_windows(rs, bus);
    /* The two hooked instructions: the build's program, whose region hooks
     * these are. A build with no hooks (hotdp) passes NULL. */
    g_hotd_country = k ? k->country : HOTD_COUNTRY;
    g_hotd_region_idle = !k || hotd__not_build(rs, k);
    if (k && g_hotd_region_idle) LOG_WARN("hotd_install: not the build's program; the region hooks stay idle");
    uint32_t prcb = hotd_read32(rs->maincpu, 0x04);
    cpu->prcb = prcb;
    if (prcb + 0x2C < rs->maincpu_size) {
        uint32_t isp = hotd_read32(rs->maincpu, prcb + PRCB_INTR_STACK);
        cpu->globals.fp = isp;
        cpu->locals.sp  = isp + 64;
        cpu->sfr.pc     = 0x001F2002u;
        uint32_t start_ip_ptr = hotd_read32(rs->maincpu, prcb + PRCB_START_IP);
        if (start_ip_ptr < rs->maincpu_size) cpu->sfr.ip = hotd_read32(rs->maincpu, start_ip_ptr);
    }
    LOG_INFO("hotd_install: PRCB=0x%08X initial IP=0x%08X", prcb, cpu->sfr.ip);
}

static inline void hotd_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    hotd_install_build(rs, cpu, bus, &k_hotdo_build);
}
static inline void hotdp_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    hotd_install_build(rs, cpu, bus, NULL);
}

/* MAME's `vcop2` ports, as `hotd` modifies them: IN0 the cabinet's, IN1
 * bit 0 and 1 the triggers (lightgun.h serves them, and the guns). Each
 * player's pad walks their gun and B2 reloads; P2's trigger is IN1 bit 1. */
#define HOTD_INPUT { .bits = { \
    [GAME_INPUT_P1_COIN]  = 0x00000001, [GAME_INPUT_P2_COIN]  = 0x00000002, \
    [GAME_INPUT_TEST]     = 0x00000004, [GAME_INPUT_SERVICE]  = 0x00000008, \
    [GAME_INPUT_P1_START] = 0x00000010, [GAME_INPUT_P2_START] = 0x00000020, \
    [GAME_INPUT_P1_B1]    = 0x00000100, [GAME_INPUT_P2_B1]    = 0x00000200, \
    [GAME_INPUT_P1_B2]    = 0x00000400, [GAME_INPUT_P2_B2]    = 0x00000800, \
    [GAME_INPUT_P1_DOWN]  = 0x00001000, [GAME_INPUT_P1_UP]    = 0x00002000, \
    [GAME_INPUT_P1_RIGHT] = 0x00004000, [GAME_INPUT_P1_LEFT]  = 0x00008000, \
    [GAME_INPUT_P2_DOWN]  = 0x00100000, [GAME_INPUT_P2_UP]    = 0x00200000, \
    [GAME_INPUT_P2_RIGHT] = 0x00400000, [GAME_INPUT_P2_LEFT]  = 0x00800000, \
} }

/* The light guns, the region and the model table: noclip games.js `hotd` /
 * `hotdo`, 7477 entries of 16 bytes, `hotdp` 10097; the board's mesh-pointer
 * encoding, as STF's. */
#define HOTD_REGION_HOOKED { .kind = REGION_HOOKED, .factory_default = "country_default", \
                             .over_saved = "settings_loaded", .country = hotd_country }
#define HOTD_REGION_JAPAN  { .kind = REGION_PROGRAM_ROM, \
                             .why = "the prototype reaches its title only in Japan (hotd.h)" }
#define HOTD_QUIRKS(table, count, region_use) { \
    .lightgun = true, \
    .gun = { \
        { .up = 0x2000, .down = 0x1000, .left = 0x8000, .right = 0x4000, \
          .trigger = 0x100, .reload = 0x400, \
          .x_min = 173, .x_max = 596, .x_rest = 0x180, .y_min = 87, .y_max = 380, .y_rest = 0x0e9 }, \
        { .up = 0x200000, .down = 0x100000, .left = 0x800000, .right = 0x400000, \
          .trigger = 0x200, .reload = 0x800, \
          .x_min = 163, .x_max = 596, .x_rest = 0x17b, .y_min = 87, .y_max = 380, .y_rest = 0x0e9 }, \
    }, \
    .irq_vectors      = true, \
    .board_vblank     = true, \
    .region           = region_use, \
    .model_table_offset = table, \
    .model_table_count  = count, \
    .mesh_ptr_subtract  = 0x02000010, \
    .mesh_ptr_add       = 0x10, \
    .enable_68k_sound   = true, \
}

/* The game's mode word (Pinboard #1415): 4 from START (the opening scene
 * included), 5 the continue count, 7 game over; then 1, 9 and attract's 2, 3
 * and 8 (its demo plays a game too). A local game is recorded while it is 4-7. */
#define HOTD_MODE 0x0051EE14u
static bool hotd_in_play(memory_bus_t *bus) {
    uint16_t m = mem_read16(bus, HOTD_MODE);
    return m >= 4 && m <= 7;
}

#define HOTD_HOOKS(country_default, settings_loaded) .hook_count = 2, .hooks = { \
    { country_default, hotd_hook_country_default, "country_default" }, \
    { settings_loaded, hotd_hook_settings_loaded, "settings_loaded" }, \
}

static const game_profile_t hotd_profile = {
    .id           = "hotd",
    .display_name = "The House of the Dead (Rev A)",
    .board        = BOARD_MODEL2C_CRX,
    .load_fn      = hotd_load,
    .install_fn   = hotd_install,
    HOTD_HOOKS(HOTD_HOOK_ADDR_COUNTRY_DEFAULT, HOTD_HOOK_ADDR_SETTINGS_LOADED),
    .input        = HOTD_INPUT,
    .in_play      = hotd_in_play,
    .quirks       = HOTD_QUIRKS(0xe73530, 7477, HOTD_REGION_HOOKED),
};

/* The clones (Pinboard #1286), picked by their own zip names. hotdo is the
 * program the hooks above were found on; hotdp runs Japan only (above). */
static const game_profile_t hotdo_profile = {
    .id           = "hotdo",
    .display_name = "The House of the Dead",
    .parent_zip_name = "hotd.zip",
    .board        = BOARD_MODEL2C_CRX,
    .load_fn      = hotdo_load,
    .install_fn   = hotd_install,
    HOTD_HOOKS(HOTD_HOOK_ADDR_COUNTRY_DEFAULT, HOTD_HOOK_ADDR_SETTINGS_LOADED),
    .input        = HOTD_INPUT,
    .in_play      = hotd_in_play,
    .quirks       = HOTD_QUIRKS(0xe73530, 7477, HOTD_REGION_HOOKED),
};

static const game_profile_t hotdp_profile = {
    .id           = "hotdp",
    .display_name = "The House of the Dead (prototype)",
    .parent_zip_name = "hotd.zip",
    .board        = BOARD_MODEL2C_CRX,
    .load_fn      = hotdp_load,
    .install_fn   = hotdp_install,
    .input        = HOTD_INPUT,
    .quirks       = HOTD_QUIRKS(0xc5d1f0, 10097, HOTD_REGION_JAPAN),
};

#endif /* PROFILES_HOTD_H */
