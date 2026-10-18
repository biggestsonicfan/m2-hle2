/*
 * profiles/daytona.h — Daytona USA (daytona, Japan, Revision A). Runs on the
 * desktop builds (Pinboard #1035) and the web (#1047): attract, the test menu, coin to race, with
 * the wheel, pedals and lever through the Model 1 I/O board, and the Model 1
 * sound board's music and effects (board/model1_sound.h).
 *
 * Pinboard #660 (parity with noclip): the explorer reads this set and its
 * clones (vendor/noclip js/games.js, `daytona`), and the polygon decoder was
 * first worked out on it (CLAUDE.md, "3D Polygon Decoder"). The profile loads
 * the same ROMs with the same model table, ready for the board work. ROM names,
 * CRCs and offsets are MAME's (src/mame/sega/model2.cpp, ROM_START(daytona)).
 *
 * The board: an original Model 2 (MAME model2o), like Virtua Cop. The i960
 * talks to an MB86234 TGP running the game's program (board/tgp.h, as Sega
 * Rally's; Daytona sends some arguments with halfword stores, model2a.h), the
 * model2o memory map is daytona_install's, the Model 1 I/O board is
 * board/model1io.h (its Z80 runs model1io.zip's program), and M2COMM answers
 * as a link of one. The sound board is MAME's segam1audio: a 68000, two
 * MultiPCMs and a YM3438 whose timers run but whose FM voices are not made
 * (the game keys none on in attract or a race; a key-on is logged). The
 * drive board's force feedback is its commands decoded (board/drive_board.h)
 * and played on the host's pad or wheel (core/ffb.h, Pinboard #1047). What it
 * lacks: a second cabinet on the link. The comm board's Z80 program
 * (epr-16726) and the drive board's ROM are not loaded: nothing here would
 * run them.
 */
#ifndef PROFILES_DAYTONA_H
#define PROFILES_DAYTONA_H

#include "game_profile.h"
#include "rom_recipe.h"
#include "memory.h"
#include "model2a.h"
#include "model1io.h"
#include "sound.h"
#include "m2comm.h"
#include "i960.h"
#include "io_5649.h"
#include "savestate_reg.h"

static const rom_part_t daytona_parts[] = {
    ROM_PAIR32(ROM_MAINCPU,    0x000000, "epr-16722a.12",  0x48b94318, "epr-16723a.13",  0x8af8b32d),
    ROM_PAIR32(ROM_MAIN_DATA,  0x000000, "mpr-16528.10",   0x9ce591f6, "mpr-16529.11",   0xf7095eaf),
    ROM_PAIR32(ROM_MAIN_DATA,  0x400000, "mpr-16808.8",    0x44f1f5a0, "mpr-16809.9",    0x37a2dd12),
    ROM_PAIR32(ROM_MAIN_DATA,  0x800000, "epr-16724a.6",   0x469f10fd, "epr-16725a.7",   0xba0df8db),
    ROM_MIRROR(ROM_MAIN_DATA,  0x800000, 0x900000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x800000, 0xa00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x800000, 0xb00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x800000, 0xc00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x800000, 0xd00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x800000, 0xe00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x800000, 0xf00000, 0x100000),
    /* Course collision and height, read by the TGP (as Sega Rally's). */
    ROM_PAIR32(ROM_COPRO_DATA, 0x000000, "mpr-16537.ic28", 0x36b7c35a, "mpr-16536.ic29", 0x6d6afed9),
    ROM_PAIR32(ROM_POLYGONS,   0x000000, "mpr-16523.ic16", 0x2f484d42, "mpr-16518.ic20", 0xdf683bf7),
    ROM_PAIR32(ROM_POLYGONS,   0x400000, "mpr-16524.ic17", 0x34658bd7, "mpr-16519.ic21", 0xfacd1c81),
    ROM_PAIR32(ROM_POLYGONS,   0x800000, "mpr-16525.ic18", 0xfb517521, "mpr-16520.ic22", 0xd66bd9bd),
    ROM_PAIR32(ROM_POLYGONS,   0xc00000, "mpr-16772.ic19", 0x770ed912, "mpr-16771.ic23", 0xa2205124),
    ROM_PAIR32(ROM_TEXTURES,   0x000000, "mpr-16522.25",   0x55d39a57, "mpr-16521.24",   0xaf1934fb),
    ROM_PAIR32(ROM_TEXTURES,   0x800000, "mpr-16770.27",   0xf9fa7bfb, "mpr-16769.26",   0xe57429e9),
    /* Model 1 sound board: MultiPCM 1 at 0, MultiPCM 2 at 0x400000 of `samples`. */
    ROM_SWAP16(ROM_AUDIOCPU,   0x000000, "epr-16720.7",    0x8e73cffd),
    ROM_SWAP16(ROM_AUDIOCPU,   0x020000, "epr-16721.8",    0x1bb3b7b7),
    ROM_BYTES(ROM_SAMPLES,     0x000000, "mpr-16491.32",   0x89920903),
    ROM_BYTES(ROM_SAMPLES,     0x200000, "mpr-16492.33",   0x459e701b),
    ROM_BYTES(ROM_SAMPLES,     0x400000, "mpr-16493.4",    0x9990db15),
    ROM_BYTES(ROM_SAMPLES,     0x600000, "mpr-16494.5",    0x600e1d6c),
    /* The TGP's tables (MAME ROM_REGION32_LE "copro_tgp_tables"). */
    ROM_PAIR32(ROM_TGP_TABLES, 0x000000, "opr-14742a.45",  0x90c6b117, "opr-14743a.46",  0xae7f446b),
    /* The Model 1 I/O board's program, MAME's model1io.zip (BIOS epr14869c). */
    ROM_BYTES(ROM_IOCPU,       0x000000, "epr-14869c.25",  0x24b68e64),
};

static const rom_recipe_t daytona_recipe = {
    .set  = "daytona",
    .size = { [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000,
              [ROM_COPRO_DATA] = 0x800000,
              [ROM_POLYGONS] = 0x1000000, [ROM_TEXTURES] = 0x1000000,
              [ROM_AUDIOCPU] = 0xc0000, [ROM_SAMPLES] = 0x800000,
              [ROM_TGP_TABLES] = 0x40000, [ROM_IOCPU] = 0x10000 },
    .parts = daytona_parts, .count = sizeof daytona_parts / sizeof daytona_parts[0],
};

static inline int daytona_load(romset_t *rs, const char *child, const char *parent) {
    return rom_recipe_load(rs, child, parent, &daytona_recipe);
}

static inline uint32_t daytona_read32(const uint8_t *buf, uint32_t off) {
    return (uint32_t)buf[off] | (uint32_t)buf[off + 1] << 8
         | (uint32_t)buf[off + 2] << 16 | (uint32_t)buf[off + 3] << 24;
}

/* The settings block (Pinboard #1035): the test menu's GAME ASSIGNMENTS, 128
 * bytes the boot (ROM 0x28A60) takes from the first copy that is valid -- the
 * I/O board's EEPROM, read into backup RAM at 0x1D00000, then backup RAM's own
 * at 0x1D00080 -- and else from the ROM's factory block at 0x39D60. A copy is
 * valid when its first 8 bytes are the factory block's ("SEGA" ...) and the
 * u16 at +8 is the CRC (ROM 0x292D8: table at 0x39DE0, seed 0) of +0x0A..+0x7F.
 * The byte at +0x0B is LINK ID: 0 SINGLE, 1 MASTER, 2 SLAVE.
 *
 * The factory value is MASTER: this is the twin cabinet's set, and a board alone
 * waits for its other half, prints CANCELLED and reboots (ROM 0x1B5F4), forever.
 * An operator sets SINGLE in the test menu; a blank board here gets the same
 * block that leaves: the factory one, SINGLE, with its CRC. A block the player's
 * backup RAM already holds is left alone. */
#define DAYTONA_SETTINGS      0x80u
#define DAYTONA_FACTORY       0x39D60u
#define DAYTONA_CRC_TABLE     0x39DE0u
#define DAYTONA_SETTINGS_LINK 0x0Bu

static inline uint16_t daytona_settings_crc(const uint8_t *rom, const uint8_t *p, uint32_t n) {
    uint32_t c = 0;
    while (n--) {
        uint32_t i = ((c >> 8) ^ *p++) & 0xFFu;
        c = (c << 8) ^ ((uint32_t)rom[DAYTONA_CRC_TABLE + i * 2u] | (uint32_t)rom[DAYTONA_CRC_TABLE + i * 2u + 1u] << 8);
    }
    return (uint16_t)c;
}

static inline void daytona_settings_single(memory_bus_t *bus, const uint8_t *rom, size_t rom_size) {
    if (!bus->back || rom_size < DAYTONA_CRC_TABLE + 0x200u) return;
    uint8_t *b = bus->back + DAYTONA_SETTINGS;
    if (memcmp(b, rom + DAYTONA_FACTORY, 8) == 0
        && (uint16_t)(b[8] | b[9] << 8) == daytona_settings_crc(rom, b + 0x0A, 0x76)) return;
    memcpy(b, rom + DAYTONA_FACTORY, 0x80);
    b[DAYTONA_SETTINGS_LINK] = 0;
    uint16_t crc = daytona_settings_crc(rom, b + 0x0A, 0x76);
    b[8] = (uint8_t)crc;
    b[9] = (uint8_t)(crc >> 8);
}

/* The original Model 2 (MAME model2o_mem): model2_tgp_mem, the TGP's map,
 * with RAM at 0x200000-0x21FFFF, the program's second 128 KB mirrored at
 * 0x220000, and the I/O board's dual-port RAM at 0x01C00000. */
static inline void daytona_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!rs->loaded) { LOG_ERROR("daytona_install: romset not loaded"); return; }

    i960_reset(cpu);
    mem_init(bus, rs->maincpu, rs->maincpu_size);
    daytona_settings_single(bus, rs->maincpu, rs->maincpu_size);
    model2a_install(bus, rs->tgp_tables, rs->copro_data, rs->copro_data_size);

    mem_region_t *r = model2a_region_named(bus, "RAM2");
    if (r) r->size = 0x20000u;
    if (rs->maincpu_size >= 0x40000u)
        mem_add_region(bus, "ROM_MIRROR", 0x00220000u, 0x20000u, rs->maincpu + 0x20000u, 1);
    model1io_install(bus, rs->iocpu, rs->iocpu_size);
    /* The comm board (MAME model2o: M2COMM), alone: the boot's network check
     * waits for it to answer. */
    m2comm_install(bus, 0x1C0u);

    if (rs->main_data && bus->main_data) {
        size_t n = rs->main_data_size < MAIN_DATA_SIZE ? rs->main_data_size : MAIN_DATA_SIZE;
        memcpy(bus->main_data, rs->main_data, n);
    }

    uint32_t prcb = daytona_read32(rs->maincpu, 0x04);
    if (prcb + 0x2C < rs->maincpu_size) {
        uint32_t isp = daytona_read32(rs->maincpu, prcb + PRCB_INTR_STACK);
        cpu->globals.fp = isp;
        cpu->locals.sp  = isp + 64;
        uint32_t start_ip_ptr = daytona_read32(rs->maincpu, prcb + PRCB_START_IP);
        if (start_ip_ptr < rs->maincpu_size) cpu->sfr.ip = daytona_read32(rs->maincpu, start_ip_ptr);
    }
    io5649_reset();
    savestate_extra("daytona", "IO5649", &g_io5649, sizeof g_io5649);
    savestate_extra("daytona", "MODEL1IO", &g_model1io, sizeof g_model1io);
    savestate_extra("daytona", "M2COMM", &g_m2comm, sizeof g_m2comm);
    savestate_extra("daytona", "TGP", &g_tgp, sizeof g_tgp);
    savestate_extra("daytona", "I960FRAC", &g_i960_cyc_frac, sizeof g_i960_cyc_frac);
    savestate_extra("daytona", "I960BT", &g_i960_bt, sizeof g_i960_bt);
    savestate_extra("daytona", "M1SOUND", &g_sound.m1, sizeof g_sound.m1);
    LOG_INFO("daytona_install: PRCB=0x%08X initial IP=0x%08X", prcb, cpu->sfr.ip);
}

static const game_profile_t daytona_profile = {
    .id           = "daytona",
    .display_name = "Daytona USA (Japan, Rev A)",
    .parent_zip_name = "model1io.zip",   /* the I/O board's program, as MAME finds it */
    .board        = BOARD_MODEL2,
    .load_fn      = daytona_load,
    .install_fn   = daytona_install,
    .input = {
        /* Byte 0 is IN0 as the I/O board reads it (MAME INPUT_PORTS_START(daytona)):
         * COIN1 0x01, COIN2 0x02, TEST 0x04, SERVICE 0x08, START 0x10 and the
         * view buttons VR1-3 0x20-0x80. Byte 1 is io_5649.h's wheel, pedals and
         * lever (quirks.wheel), and VR4. VR3 and VR4 take P2's B3/B4: P1's
         * four buttons are the pedals and VR1-2. */
        .bits = {
            [GAME_INPUT_P1_COIN]  = 0x00000001,
            [GAME_INPUT_TEST]     = 0x00000004,
            [GAME_INPUT_SERVICE]  = 0x00000008,
            [GAME_INPUT_P1_START] = 0x00000010,
            [GAME_INPUT_P1_B3]    = 0x00000020,
            [GAME_INPUT_P1_B4]    = 0x00000040,
            [GAME_INPUT_P2_B3]    = 0x00000080,
            [GAME_INPUT_P1_B1]    = 0x00000100,
            [GAME_INPUT_P1_B2]    = 0x00000200,
            [GAME_INPUT_P2_B4]    = 0x00000400,
            [GAME_INPUT_P1_DOWN]  = 0x00001000,
            [GAME_INPUT_P1_UP]    = 0x00002000,
            [GAME_INPUT_P1_RIGHT] = 0x00004000,
            [GAME_INPUT_P1_LEFT]  = 0x00008000,
        },
    },
    .quirks = {
        .irq_vectors       = true,
        .enable_68k_sound  = true,
        .model1_sound      = true,
        .board_vblank      = true,
        .i960_board_timing = true,
        .model1_io         = true,
        .model1_io_vr4     = 0x0400,
        .region            = { .kind = REGION_TODO,
                               .why  = "country setting not looked for yet" },
        .wheel = { .left = 0x8000, .right = 0x4000, .accel = 0x0100, .brake = 0x0200,
                   .shift_up = 0x2000, .shift_down = 0x1000 },
        /* noclip games.js `daytona` (Rev A): 3190 entries of 20 bytes (the
         * explorer also reads oba/tpa/tha there; 16 bytes on STF), palette at
         * 0x84F4EC. The polygon decoder was confirmed on this set first. */
        .model_table_offset = 0x83fbb4,
        .model_table_count  = 3190,
        .model_layout       = MODEL_LAYOUT_DAYTONA,
        .mesh_ptr_subtract  = 0x02000010,
        .mesh_ptr_add       = 0x10,
    },
};

#endif /* PROFILES_DAYTONA_H */
