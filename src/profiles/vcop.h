/*
 * profiles/vcop.h — Virtua Cop (vcop, Revision B). Runs since Pinboard #1286:
 * attract, coin and start, and the game with two light guns, with sound.
 *
 * Pinboard #660 (parity with noclip) prepared it: the explorer reads this set
 * (vendor/noclip js/games.js, `vcop`), so the profile loads the same ROMs with
 * the same model table, and prepared_test still holds the decoder to noclip's
 * reads of it. ROM names, CRCs and offsets are MAME's
 * (src/mame/sega/model2.cpp, ROM_START(vcop)).
 *
 * The board is Daytona's: an original Model 2 (board/model2o.h, MAME model2o)
 * whose MB86234 TGP runs the game's program (board/tgp.h), and the Model 1
 * sound board (board/model1_sound.h). The I/O board is the Advanced Model 1
 * one (MAME model1io2, BIOS epr17181 in model1io2.zip): a TMPZ84C015 running
 * its own program, and an FPGA that reads the two guns (board/model1io.h,
 * model1io_install_advanced). The guns are lightgun.h's, as The House of the
 * Dead's: each pad walks its gun, B1 fires, B2 reloads (the shot off the
 * screen), and a host's pointer aims (ui/lightgun_host.h). No HLE hook.
 */
#ifndef PROFILES_VCOP_H
#define PROFILES_VCOP_H

#include "game_profile.h"
#include "rom_recipe.h"
#include "memory.h"
#include "model2o.h"
#include "model1io.h"
#include "lightgun.h"
#include "sound.h"
#include "i960.h"
#include "savestate_reg.h"

static const rom_part_t vcop_parts[] = {
    ROM_PAIR32(ROM_MAINCPU,   0x000000, "epr-17166b.12", 0xa5647c59, "epr-17167b.13", 0xf5dde26a),
    ROM_PAIR32(ROM_MAINCPU,   0x040000, "epr-17160a.14", 0x267f3242, "epr-17161a.15", 0xf7126876),
    ROM_PAIR32(ROM_MAIN_DATA, 0x000000, "mpr-17164.10",  0xac5fc501, "mpr-17165.11",  0x82296d00),
    ROM_PAIR32(ROM_MAIN_DATA, 0x400000, "mpr-17162.8",   0x60ddd41e, "mpr-17163.9",   0x8c1f9dc8),
    ROM_PAIR32(ROM_MAIN_DATA, 0x800000, "epr-17168a.6",  0x59091a37, "epr-17169a.7",  0x0495808d),
    ROM_PAIR32(ROM_POLYGONS,  0x000000, "mpr-17159.16",  0xe218727d, "mpr-17156.20",  0xc4f4aabf),
    ROM_PAIR32(ROM_TEXTURES,  0x000000, "mpr-17158.25",  0x1108d1ec, "mpr-17157.24",  0xcf31e33d),
    /* Model 1 sound board: the 68000's program, then MultiPCM 1 at 0 and
     * MultiPCM 2 at 0x400000 of `samples` (MAME keeps them as two regions). */
    ROM_SWAP16(ROM_AUDIOCPU,  0x000000, "epr-17170.7",   0x06a38ae2),
    ROM_SWAP16(ROM_AUDIOCPU,  0x020000, "epr-17171.8",   0xb5e436f8),
    ROM_BYTES(ROM_SAMPLES,    0x000000, "mpr-17172.32",  0xab22cac3),
    ROM_BYTES(ROM_SAMPLES,    0x200000, "mpr-17173.33",  0x3cb4005c),
    ROM_BYTES(ROM_SAMPLES,    0x400000, "mpr-17174.4",   0xa50369cc),
    ROM_BYTES(ROM_SAMPLES,    0x600000, "mpr-17175.5",   0x9136d43c),
    /* The TGP's tables (MAME ROM_REGION32_LE "copro_tgp_tables"). */
    ROM_PAIR32(ROM_TGP_TABLES, 0x000000, "opr-14742a.45", 0x90c6b117, "opr-14743a.46", 0xae7f446b),
    /* The Advanced Model 1 I/O board's program, MAME's model1io2.zip (BIOS epr17181). */
    ROM_BYTES(ROM_IOCPU,      0x000000, "epr-17181.6",   0x1add2b82),
};

static const rom_recipe_t vcop_recipe = {
    .set  = "vcop",
    .size = { [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000,
              [ROM_COPRO_DATA] = 0x800000,   /* empty on this board */
              [ROM_POLYGONS] = 0x1000000, [ROM_TEXTURES] = 0x1000000,
              [ROM_AUDIOCPU] = 0xc0000, [ROM_SAMPLES] = 0x800000,
              [ROM_TGP_TABLES] = 0x40000, [ROM_IOCPU] = 0x10000 },
    .parts = vcop_parts, .count = sizeof vcop_parts / sizeof vcop_parts[0],
};

static inline int vcop_load(romset_t *rs, const char *child, const char *parent) {
    return rom_recipe_load(rs, child, parent, &vcop_recipe);
}

/* The original Model 2 (model2o.h) with the Advanced I/O board (model1io.h). */
static inline void vcop_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!rs->loaded) { LOG_ERROR("vcop_install: romset not loaded"); return; }
    model2o_install_bus(rs, cpu, bus);
    model1io_install_advanced(bus, rs->iocpu, rs->iocpu_size);
    model2o_install_finish(rs, cpu, bus);
    savestate_extra("vcop", "MODEL1IO", &g_model1io, sizeof g_model1io);
    savestate_extra("vcop", "M1IO2", &g_model1io2, sizeof g_model1io2);
    savestate_extra("vcop", "GUN", &g_lightgun, sizeof g_lightgun);
    savestate_extra("vcop", "M2COMM", &g_m2comm, sizeof g_m2comm);
    savestate_extra("vcop", "TGP", &g_tgp, sizeof g_tgp);
    savestate_extra("vcop", "I960FRAC", &g_i960_cyc_frac, sizeof g_i960_cyc_frac);
    savestate_extra("vcop", "I960BT", &g_i960_bt, sizeof g_i960_bt);
    savestate_extra("vcop", "M1SOUND", &g_sound.m1, sizeof g_sound.m1);
}

/* MAME's `vcop` ports: IN0 the cabinet's (COIN1 0x01, COIN2 0x02, TEST 0x04,
 * SERVICE 0x08, START1 0x10, START2 0x20), IN1 bit 0 and 1 the triggers
 * (lightgun.h serves them). The rest of the word only works the guns: each
 * player's pad walks their gun and B2 reloads. The bits are HOTD_INPUT's. */
#define VCOP_INPUT { .bits = { \
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

static const game_profile_t vcop_profile = {
    .id           = "vcop",
    .display_name = "Virtua Cop (Rev B)",
    .parent_zip_name = "model1io2.zip",   /* the I/O board's program, as MAME finds it */
    .board        = BOARD_MODEL2,
    .load_fn      = vcop_load,
    .install_fn   = vcop_install,
    .input        = VCOP_INPUT,
    .quirks = {
        .irq_vectors       = true,
        .enable_68k_sound  = true,
        .model1_sound      = true,
        .board_vblank      = true,
        .i960_board_timing = true,
        .model1_io2        = true,
        .lightgun          = true,
        /* MAME INPUT_PORTS_START(vcop): P1_X, P1_Y, P2_X, P2_Y. */
        .gun = {
            { .up = 0x2000, .down = 0x1000, .left = 0x8000, .right = 0x4000,
              .trigger = 0x100, .reload = 0x400,
              .x_min = 0x083, .x_max = 0x276, .x_rest = 0x17c, .y_min = 0x024, .y_max = 0x1a9, .y_rest = 0x0e6 },
            { .up = 0x200000, .down = 0x100000, .left = 0x800000, .right = 0x400000,
              .trigger = 0x200, .reload = 0x800,
              .x_min = 0x080, .x_max = 0x273, .x_rest = 0x179, .y_min = 0x027, .y_max = 0x1a9, .y_rest = 0x0e8 },
        },
        .region            = { .kind = REGION_TODO,
                               .why  = "country setting not looked for yet" },
        /* noclip games.js `vcop`: 0x4E3 entries of 16 bytes; palette at 0x4C0000.
         * 0x4D2 on are read from the second table (0x28693F0[n*16]), their
         * headers in texture RAM, which the game fills at game start. */
        .model_table_offset = 0x4c8000,
        .model_table_count  = 0x4e3,
        .model_table_past_from   = 0x4d2,
        .model_table_past_offset = 0x8693f0,
        .model_layout       = MODEL_LAYOUT_VCOP,
        .mesh_ptr_subtract  = 0x02000010,
        .mesh_ptr_add       = 0x10,
    },
};

#endif /* PROFILES_VCOP_H */
