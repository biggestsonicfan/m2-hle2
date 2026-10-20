/*
 * profiles/srallyc.h — Sega Rally Championship (srallyc, Revision C).
 *
 * The first Model 2A profile: the coprocessor is a TGP running the program the
 * i960 uploads at boot (board/tgp.h, board/model2a.h), not the SHARC.
 * ROM names and CRCs are MAME's (src/mame/sega/model2.cpp, ROM_START(srallyc));
 * the files are found by CRC, so a set that names them differently loads too.
 *
 * Status: boots to attract with sound, the TGP drawing the 3D and the course
 * textures loaded. Interrupts go through the program's own table (irq_vectors):
 * the vblank handler at 0x5A0490 clears the texture-upload budget flag at
 * 0x202004 and arms timer 2, and without it the course and title textures were
 * never uploaded. Hooks: the region (srallyc_hook_country_default) and the
 * native replay's two (srallyc_native.h).
 *
 * Controls: the 315-5649 I/O board's wheel, pedals and four-speed lever
 * (board/io_5649.h). LEFT / RIGHT turn the wheel, B1 is the accelerator, B2
 * the brake, UP / DOWN shift a gear, B3 is VIEW CHANGE (the VR button). The
 * wheel's pot is adjustable (--wheel-center / --wheel-range / --wheel-speed);
 * the default is the owner's manual's spec.
 *
 * Replays: each course raced is kept as the clip the game records of it, and
 * played back by the game's own ending (srallyc_native.h); --replay-format
 * inputs keeps the state and the inputs too. Labelled with the car and the
 * course (srallyc_match_info below).
 */
#ifndef PROFILES_SRALLYC_H
#define PROFILES_SRALLYC_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "log.h"
#include "rom_loader.h"
#include "game_profile.h"
#include "memory.h"
#include "model2a.h"
#include "i960.h"
#include "hle_hooks.h"
#include "io_5649.h"
#include "savestate_reg.h"
#include "srallyc_native.h" /* each course's clip in the game's replay format (Pinboard #997) */

#define SRALLYC_PAIR_32(dest, dest_sz, lo, crc_lo, hi, crc_hi, half_sz, offset) \
    do { \
        tmp1 = zip_extract_from_set(child, parent, lo, &fsize, crc_lo); \
        tmp2 = zip_extract_from_set(child, parent, hi, &fsize, crc_hi); \
        if (!tmp1 || !tmp2) { ok = 0; goto done; } \
        interleave_32_word(dest, dest_sz, tmp1, half_sz, tmp2, half_sz, offset); \
        free(tmp1); tmp1 = NULL; \
        free(tmp2); tmp2 = NULL; \
    } while (0)

#define SRALLYC_SAMPLE(dest, dest_sz, name, crc, offset) \
    do { \
        tmp1 = zip_extract_from_set(child, parent, name, &fsize, crc); \
        if (!tmp1) { ok = 0; goto done; } \
        load_16_word_swap(dest, dest_sz, tmp1, fsize, offset); \
        free(tmp1); tmp1 = NULL; \
    } while (0)

static inline uint8_t *srallyc_region(size_t size) { return (uint8_t *)calloc(1, size); }

static inline int srallyc_load(romset_t *rs, const char *child, const char *parent) {
    romset_free(rs);
    size_t   fsize = 0;
    uint8_t *tmp1 = NULL, *tmp2 = NULL;
    int      ok = 1;

    rs->maincpu_size = 0x200000;
    rs->maincpu = srallyc_region(rs->maincpu_size);
    SRALLYC_PAIR_32(rs->maincpu, rs->maincpu_size,
        "epr-17888c.12", 0x3d6808aa, "epr-17889c.13", 0xf43c7802, 0x80000, 0x000000);

    rs->main_data_size = 0x2400000;
    rs->main_data = srallyc_region(rs->main_data_size);
    SRALLYC_PAIR_32(rs->main_data, rs->main_data_size,
        "mpr-17746.10", 0x8fe311f4, "mpr-17747.11", 0x543593fd, 0x200000, 0x000000);
    SRALLYC_PAIR_32(rs->main_data, rs->main_data_size,
        "mpr-17744.8",  0x71fed098, "mpr-17745.9",  0x8ecca705, 0x200000, 0x400000);
    SRALLYC_PAIR_32(rs->main_data, rs->main_data_size,
        "mpr-17884.6",  0x4cfc95e1, "mpr-17885.7",  0xa08d2467, 0x200000, 0x800000);

    /* Course collision and height, read by the TGP through its bank window. */
    rs->copro_data_size = 0x800000;
    rs->copro_data = srallyc_region(rs->copro_data_size);
    SRALLYC_PAIR_32(rs->copro_data, rs->copro_data_size,
        "mpr-17754.28", 0x81a84f67, "mpr-17755.29", 0x2a6e7da4, 0x200000, 0x000000);

    rs->polygons_size = 0x1000000;
    rs->polygons = srallyc_region(rs->polygons_size);
    SRALLYC_PAIR_32(rs->polygons, rs->polygons_size,
        "mpr-17748.16", 0x3148a2b2, "mpr-17750.20", 0x232aec29, 0x200000, 0x000000);
    SRALLYC_PAIR_32(rs->polygons, rs->polygons_size,
        "mpr-17749.17", 0x0838d184, "mpr-17751.21", 0xed87ac62, 0x200000, 0x400000);

    rs->textures_size = 0x1000000;
    rs->textures = srallyc_region(rs->textures_size);
    SRALLYC_PAIR_32(rs->textures, rs->textures_size,
        "mpr-17753.25", 0x6db0eb36, "mpr-17752.24", 0xd6aa86ce, 0x200000, 0x000000);

    rs->audiocpu_size = 0x80000;
    rs->audiocpu = srallyc_region(rs->audiocpu_size);
    tmp1 = zip_extract_from_set(child, parent, "epr-17890a.30", &fsize, 0x5bac3fa1);
    if (!tmp1) { ok = 0; goto done; }
    load_16_word_swap(rs->audiocpu, rs->audiocpu_size, tmp1, fsize, 0);
    free(tmp1); tmp1 = NULL;

    rs->samples_size = 0x800000;
    rs->samples = srallyc_region(rs->samples_size);
    SRALLYC_SAMPLE(rs->samples, rs->samples_size, "mpr-17756.31", 0x7725f111, 0x000000);
    SRALLYC_SAMPLE(rs->samples, rs->samples_size, "mpr-17757.32", 0x1616e649, 0x200000);
    SRALLYC_SAMPLE(rs->samples, rs->samples_size, "mpr-17886.36", 0x54a72923, 0x400000);
    SRALLYC_SAMPLE(rs->samples, rs->samples_size, "mpr-17887.37", 0x38c31fdd, 0x600000);

    /* MODEL2_CPU_BOARD: the TGP's sine / atan / 1/x / 1/sqrt(x) tables. */
    rs->tgp_tables_size = 0x40000;
    rs->tgp_tables = srallyc_region(rs->tgp_tables_size);
    SRALLYC_PAIR_32(rs->tgp_tables, rs->tgp_tables_size,
        "opr-14742a.45", 0x90c6b117, "opr-14743a.46", 0xae7f446b, 0x20000, 0x000000);

    rs->loaded = true;
    LOG_INFO("srallyc: ROM set loaded");

done:
    free(tmp1);
    free(tmp2);
    if (!ok) { LOG_ERROR("srallyc: ROM set load failed"); romset_free(rs); return -1; }
    return 0;
}

#undef SRALLYC_PAIR_32
#undef SRALLYC_SAMPLE

static inline uint32_t srallyc_read32(const uint8_t *buf, uint32_t off) {
    return (uint32_t)buf[off] | (uint32_t)buf[off + 1] << 8
         | (uint32_t)buf[off + 2] << 16 | (uint32_t)buf[off + 3] << 24;
}

static inline void srallyc_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!rs->loaded) { LOG_ERROR("srallyc_install: romset not loaded"); return; }

    i960_reset(cpu);
    mem_init(bus, rs->maincpu, rs->maincpu_size);
    model2a_install(bus, rs->tgp_tables, rs->copro_data, rs->copro_data_size);

    /* main_data at 0x02000000; "extra" data at 0x06000000 is main_data from
     * 0x1000000, which Sega Rally leaves empty (MAME model2_base_mem). */
    if (rs->main_data && bus->main_data) {
        size_t n = rs->main_data_size < MAIN_DATA_SIZE ? rs->main_data_size : MAIN_DATA_SIZE;
        memcpy(bus->main_data, rs->main_data, n);
    }
    if (rs->main_data && bus->xtra_data && rs->main_data_size > 0x1000000u) {
        size_t n = rs->main_data_size - 0x1000000u;
        memcpy(bus->xtra_data, rs->main_data + 0x1000000u, n < XTRA_DATA_SIZE ? n : XTRA_DATA_SIZE);
    }

    uint32_t prcb = srallyc_read32(rs->maincpu, 0x04);
    if (prcb + 0x2C < rs->maincpu_size) {
        /* FP on the PRCB's interrupt stack (MAME i960 device_reset): the boot
         * calls 0x5C0, which flushes the frames and returns through memory, so
         * the caller's frame has to land in RAM, not at 0 in ROM. */
        uint32_t isp = srallyc_read32(rs->maincpu, prcb + PRCB_INTR_STACK);
        cpu->globals.fp = isp;
        cpu->locals.sp  = isp + 64;
        uint32_t start_ip_ptr = srallyc_read32(rs->maincpu, prcb + PRCB_START_IP);
        if (start_ip_ptr < rs->maincpu_size) cpu->sfr.ip = srallyc_read32(rs->maincpu, start_ip_ptr);
    }
    io5649_reset();
    savestate_extra("srallyc", "IO5649", &g_io5649, sizeof g_io5649);
    /* The TGP is board state outside the parts savestate.h lists: without it a
     * loaded state left the i960 reading an empty reply FIFO and drew no 3D. */
    savestate_extra("srallyc", "TGP", &g_tgp, sizeof g_tgp);
    savestate_extra("srallyc", "I960FRAC", &g_i960_cyc_frac, sizeof g_i960_cyc_frac);
    savestate_extra("srallyc", "I960BT", &g_i960_bt, sizeof g_i960_bt);
    LOG_INFO("srallyc_install: PRCB=0x%08X initial IP=0x%08X", prcb, cpu->sfr.ip);
}

/* Replays (core/replay.h, Pinboard #877). Work RAM at 0x200000, read off the
 * board in attract and a player's game:
 *   0x202098  the main mode: 2 attract, 3 a player's game (from the coin-up's
 *             select screens to game over), 4 the test menu
 *   0x20209C  the game's sub-state (3 mode select, 4/5 championship, 6/7 practice)
 *   0x20A7DC  the course: 0 forest, 1 desert, 2 mountain (attract's three demos
 *             and the first championship race)
 *   0x20A8C4  the car-select cursor, kept into the race: 0 Celica MT,
 *             1 Celica AT, 2 Delta AT, 3 Delta MT (left to right on the wheel)
 * The game's own replay, each course's highlight clip, is srallyc_native.h's. */
#define SRALLYC_MODE   0x00202098u
#define SRALLYC_COURSE 0x0020A7DCu
#define SRALLYC_CAR    0x0020A8C4u

static bool srallyc_in_play(memory_bus_t *bus) {
    return mem_read8(bus, SRALLYC_MODE) == 3;
}

static void srallyc_match_info(memory_bus_t *bus, game_match_info_t *out) {
    static const char *const cars[4] = {
        "Toyota Celica GT-Four (MT)", "Toyota Celica GT-Four (AT)",
        "Lancia Delta HF Integrale (AT)", "Lancia Delta HF Integrale (MT)",
    };
    static const char *const courses[3] = { "Forest", "Desert", "Mountain" };
    int car = (int)mem_read8(bus, SRALLYC_CAR), course = (int)mem_read8(bus, SRALLYC_COURSE);
    out->fighting      = srallyc_in_play(bus);
    out->players       = 1;
    out->chara[0]      = car;
    out->chara_name[0] = car < 4 ? cars[car] : NULL;
    out->chara[1]      = -1;
    out->stage         = course;
    out->stage_name    = course < 3 ? courses[course] : NULL;
}

/*
 * The region (Pinboard #942). The program runs from RAM, copied from ROM
 * 0x1000 to 0x5A0000 at boot. Its settings are a 0x24-byte block in work RAM
 * at 0x202010: CRC16 (+0), length 0x24 (+2), save count (+4), then the GAME
 * ASSIGNMENTS; COUNTRY is the byte at 0x202019 (0 JPN, 1 USA, 2 EXPORT: the
 * strings at 0x5A5C68, and the test menu at 0x5A5F80 keeps it at 2 or less).
 * They are kept in the 315-5649's serial EEPROM (a 93C46 on port 0x1C00000,
 * DO at 0x1C00002 bit 5), not in the backup SRAM at 0x1D00000.
 *
 * At boot 0x5A1C80 reads the block from the EEPROM (0x5A1200 -> 0x5A1180) and
 * checks its CRC and length. A block that fails gets the factory defaults
 * (0x5A1A20 -> 0x5A1978) and is saved back (0x5A1930 -> 0x5A1220); either way
 * the boot goes on at 0x5A1CD0.
 *
 * country_default (0x5A19A4, in the factory defaults): the instruction before
 * it, `stob g14, 0x202019` (g14 zeroed at the routine's start), stores JPN.
 * Put g_region there and let this one run. Japan (0) is the ROM's default and
 * MAME's, so --region japan (the graders') leaves the board as it was.
 *
 * settings_checked (0x5A1CD0, where the boot goes on): with g_region_over_saved
 * (the web's --region=, or --region-over-saved), put g_region in the working
 * block's COUNTRY over the one a saved block brought. The EEPROM keeps its own
 * until the game next saves. This board has no EEPROM yet (io_5649.h: DO reads
 * 1), so today every boot fails the CRC and takes the defaults, and this
 * writes what country_default already did; an emulated EEPROM changes that.
 */
#define SRALLYC_COUNTRY 0x00202019u

static int srallyc_hook_country_default(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    mem_write8(bus, SRALLYC_COUNTRY, (uint8_t)g_region);
    return 1;
}

static uint32_t srallyc_country(memory_bus_t *bus) { (void)bus; return SRALLYC_COUNTRY; }

static int srallyc_hook_settings_checked(i960_cpu_t *cpu, memory_bus_t *bus) {
    (void)cpu;
    if (g_region_over_saved) mem_write8(bus, srallyc_country(bus), (uint8_t)g_region);
    return 1;
}

static const game_profile_t srallyc_profile = {
    .id              = "srallyc",
    .display_name    = "Sega Rally Championship (Rev C)",
    .parent_zip_name = NULL,
    .board           = BOARD_MODEL2A_CRX,
    .load_fn         = srallyc_load,
    .install_fn      = srallyc_install,
    .hook_count      = 2 + SRALLYC_NATIVE_HOOK_COUNT,
    .hooks = {
        { 0x005A19A4, srallyc_hook_country_default,  "country_default" },
        { 0x005A1CD0, srallyc_hook_settings_checked, "settings_checked" },
        SRALLYC_NATIVE_HOOKS
    },
    .match_info      = srallyc_match_info,
    .native_take     = srallyc_native_take,
    .native_put      = srallyc_native_put,
    .native_play     = srallyc_native_play,
    .in_play         = srallyc_in_play,
    .input = {
        /* IN0 is the cabinet's: COIN1 0x01, TEST 0x04, SERVICE 0x08, VR 0x20,
         * START 0x40 (MAME INPUT_PORTS_START(srallyc)). Byte 1 is not a port
         * here: io_5649.h reads it as the wheel, pedals and lever (quirks.wheel)
         * and serves IN1 as the gearbox. */
        .bits = {
            [GAME_INPUT_P1_COIN]  = 0x00000001,
            [GAME_INPUT_TEST]     = 0x00000004,
            [GAME_INPUT_SERVICE]  = 0x00000008,
            [GAME_INPUT_P1_B3]    = 0x00000020,
            [GAME_INPUT_P1_START] = 0x00000040,
            [GAME_INPUT_P1_B1]    = 0x00000100,
            [GAME_INPUT_P1_B2]    = 0x00000200,
            [GAME_INPUT_P1_DOWN]  = 0x00001000,
            [GAME_INPUT_P1_UP]    = 0x00002000,
            [GAME_INPUT_P1_RIGHT] = 0x00004000,
            [GAME_INPUT_P1_LEFT]  = 0x00008000,
            /* A gear's own button (io_5649.h): bits no port reads. */
            [GAME_INPUT_GEAR_N]   = 0x00010000,
            [GAME_INPUT_GEAR_1]   = 0x00020000,
            [GAME_INPUT_GEAR_2]   = 0x00040000,
            [GAME_INPUT_GEAR_3]   = 0x00080000,
            [GAME_INPUT_GEAR_4]   = 0x00100000,
        },
    },
    .quirks = {
        .enable_68k_sound = true,
        /* The program's own interrupt table: its vblank handler (0x5A0490)
         * clears the texture-upload budget flag at 0x202004 and arms timer 2. */
        .irq_vectors      = true,
        .board_vblank     = true,
        .io_5649          = true,
        .region           = { .kind = REGION_HOOKED, .factory_default = "country_default",
                              .over_saved = "settings_checked", .country = srallyc_country },
        /* The grid's LOD budget is a timer read (0x5BADE0: timer 3 into
         * 0x20AAC0, 0x5BB2A0 picks a quota row from it). At MAME's table the
         * frame ends early, the row is 8 and the third car ahead of the player
         * goes to physics on the grid, so at GO it lags the pack (MAME, issue
         * #305). The board's i960 is slower than the table (i-cache misses,
         * load-use stalls, wait states; i960_timing.h, measured on a Model 2B,
         * Pinboard #611-#738), which gives rows 5-7: the car stays on its
         * path, as on the board and in the PS2 port (FUN_0012bf70 picks row
         * 7). Pinboard #667, #744. */
        .i960_board_timing = true,
        .wheel = { .left = 0x8000, .right = 0x4000, .accel = 0x0100, .brake = 0x0200,
                   .shift_up = 0x2000, .shift_down = 0x1000 },
    },
};

#endif /* PROFILES_SRALLYC_H */
