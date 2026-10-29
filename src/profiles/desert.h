/*
 * profiles/desert.h — Desert Tank (desert; Sega / Martin Marietta, 1994).
 * Pinboard #1890: the last of MAME's Model 2 sets m2-hle2 lacked.
 *
 * The board is Daytona's (profiles/daytona.h): an original Model 2
 * (board/model2o.h, MAME model2o) whose MB86234 TGP runs the game's program
 * (board/tgp.h), the Model 1 sound board (board/model1_sound.h) and the Model 1
 * I/O board (board/model1io.h, its Z80 running model1io.zip's epr-14869c).
 * ROM names, CRCs and offsets are MAME's (src/mame/sega/model2.cpp,
 * ROM_START(desert)); MAME's mpr-16964.20 / mpr-16965.21 are .21 / .20 in the
 * zip (the sockets swapped, the CRCs MAME's), so the recipe names the zip's.
 * Unlike Daytona, MAME copies nothing over main_data past the 0x800000 pair,
 * and the 68000 has one program chip.
 *
 * The cabinet (MAME INPUT_PORTS_START(desert); the MiSTer core's notes,
 * MISTER-MODEL2.md): IN0 the cabinet's buttons and VR1-VR3, IN1 bit 0 the
 * forward / reverse lever (high is REVERSE, checked on a cabinet), 0x10 the
 * machine gun, 0x20 the cannon. The A/D's channel 0 is the handle, 1 the
 * accelerator and 2 the turret's elevation (MAME's "BRAKE"; centred, lower
 * aims up). The pots are io_5649.h's model (quirks.analog): LEFT / RIGHT turn
 * the handle, UP / DOWN walk the turret, B1 floors the accelerator. The lever
 * is held for reverse (in_high), so the tank drives forward when nothing is
 * held. No HLE hook.
 */
#ifndef PROFILES_DESERT_H
#define PROFILES_DESERT_H

#include "game_profile.h"
#include "rom_recipe.h"
#include "memory.h"
#include "model2o.h"
#include "model1io.h"
#include "io_5649.h"
#include "sound.h"
#include "m2comm.h"
#include "i960.h"
#include "savestate_reg.h"

static const rom_part_t desert_parts[] = {
    ROM_PAIR32(ROM_MAINCPU,    0x000000, "epr-16976.12",  0xd036dff0, "epr-16977.13",  0xe91194bd),
    ROM_PAIR32(ROM_MAINCPU,    0x040000, "epr-16970.14",  0x4ea12d1f, "epr-16971.15",  0xd630b220),
    ROM_PAIR32(ROM_MAIN_DATA,  0x000000, "mpr-16974.10",  0x2ab491c5, "mpr-16975.11",  0xe24fe7d3),
    ROM_PAIR32(ROM_MAIN_DATA,  0x400000, "mpr-16972.8",   0x23e53748, "mpr-16973.9",   0x77d6f509),
    ROM_PAIR32(ROM_MAIN_DATA,  0x800000, "epr-16978.6",   0x38b3e574, "epr-16979.7",   0xc314eb8b),
    /* Collision and height, read by the TGP (as Daytona's course data). */
    ROM_PAIR32(ROM_COPRO_DATA, 0x000000, "epr-16981.28",  0xae847571, "epr-16980.29",  0x5239b864),
    ROM_PAIR32(ROM_POLYGONS,   0x000000, "mpr-16968.16",  0x4a16f465, "mpr-16964.21",  0xd4a769b6),
    ROM_PAIR32(ROM_POLYGONS,   0x400000, "mpr-16969.17",  0x887380ac, "mpr-16965.20",  0x9ba7645f),
    ROM_PAIR32(ROM_TEXTURES,   0x000000, "mpr-16967.25",  0xb8b84c9d, "mpr-16966.24",  0x7484efe9),
    /* Model 1 sound board: the 68000's program, MultiPCM 1 at 0 and
     * MultiPCM 2 at 0x400000 of `samples` (MAME keeps them as two regions). */
    ROM_SWAP16(ROM_AUDIOCPU,   0x000000, "epr-16985.7",   0x8c4d9056),
    ROM_BYTES(ROM_SAMPLES,     0x000000, "mpr-16986.32",  0x559612f9),
    ROM_BYTES(ROM_SAMPLES,     0x400000, "mpr-16988.4",   0xbc705875),
    ROM_BYTES(ROM_SAMPLES,     0x600000, "mpr-16989.5",   0x1b616b31),
    /* The TGP's tables (MAME ROM_REGION32_LE "copro_tgp_tables"). */
    ROM_PAIR32(ROM_TGP_TABLES, 0x000000, "opr-14742a.45", 0x90c6b117, "opr-14743a.46", 0xae7f446b),
    /* The Model 1 I/O board's program, MAME's model1io.zip (BIOS epr14869c). */
    ROM_BYTES(ROM_IOCPU,       0x000000, "epr-14869c.25", 0x24b68e64),
};

static const rom_recipe_t desert_recipe = {
    .set  = "desert",
    .size = { [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000,
              [ROM_COPRO_DATA] = 0x800000,
              [ROM_POLYGONS] = 0x800000, [ROM_TEXTURES] = 0x1000000,
              [ROM_AUDIOCPU] = 0xc0000, [ROM_SAMPLES] = 0x800000,
              [ROM_TGP_TABLES] = 0x40000, [ROM_IOCPU] = 0x10000 },
    .parts = desert_parts, .count = sizeof desert_parts / sizeof desert_parts[0],
};

static inline int desert_load(romset_t *rs, const char *child, const char *parent) {
    return rom_recipe_load(rs, child, parent, &desert_recipe);
}

/* The original Model 2 (model2o.h) with the Model 1 I/O board (model1io.h).
 * The turret is the layout's second axis (g_io5649_ax). */
static inline void desert_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    if (!rs->loaded) { LOG_ERROR("desert_install: romset not loaded"); return; }
    model2o_install_bus(rs, cpu, bus);
    model1io_install(bus, rs->iocpu, rs->iocpu_size);
    model2o_install_finish(rs, cpu, bus);
    io5649_reset();
    savestate_extra("desert", "IO5649", &g_io5649, sizeof g_io5649);
    savestate_extra("desert", "IO5649AX", &g_io5649_ax, sizeof g_io5649_ax);
    savestate_extra("desert", "MODEL1IO", &g_model1io, sizeof g_model1io);
    savestate_extra("desert", "M2COMM", &g_m2comm, sizeof g_m2comm);
    savestate_extra("desert", "TGP", &g_tgp, sizeof g_tgp);
    savestate_extra("desert", "I960FRAC", &g_i960_cyc_frac, sizeof g_i960_cyc_frac);
    savestate_extra("desert", "I960BT", &g_i960_bt, sizeof g_i960_bt);
    savestate_extra("desert", "M1SOUND", &g_sound.m1, sizeof g_sound.m1);
}

/* Byte 0 is IN0 (COIN1 0x01, COIN2 0x02, TEST 0x04, SERVICE 0x08, START
 * 0x10, VR1-VR3 0x20-0x80), byte 1 IN1 (the lever 0x01, machine gun 0x10,
 * cannon 0x20). The pots' bits are none of the ports' (M2LIB_AXES_INPUT's). */
#define DESERT_INPUT { .bits = { \
    [GAME_INPUT_P1_COIN]  = 0x00000001, [GAME_INPUT_P2_COIN]  = 0x00000002, \
    [GAME_INPUT_TEST]     = 0x00000004, [GAME_INPUT_SERVICE]  = 0x00000008, \
    [GAME_INPUT_P1_START] = 0x00000010, \
    [GAME_INPUT_P2_B1]    = 0x00000020, [GAME_INPUT_P2_B2]    = 0x00000040, \
    [GAME_INPUT_P2_B3]    = 0x00000080, \
    [GAME_INPUT_P1_B4]    = 0x00000100, \
    [GAME_INPUT_P1_B2]    = 0x00001000, [GAME_INPUT_P1_B3]    = 0x00002000, \
    [GAME_INPUT_P1_B1]    = 0x10000000, \
    [GAME_INPUT_P1_RIGHT] = 0x40000000, [GAME_INPUT_P1_LEFT]  = 0x80000000, \
    [GAME_INPUT_P1_UP]    = 0x04000000, [GAME_INPUT_P1_DOWN]  = 0x08000000, \
} }

static const game_profile_t desert_profile = {
    .id           = "desert",
    .display_name = "Desert Tank",
    .parent_zip_name = "model1io.zip",   /* the I/O board's program, as MAME finds it */
    .board        = BOARD_MODEL2,
    .load_fn      = desert_load,
    .install_fn   = desert_install,
    .input        = DESERT_INPUT,
    .quirks = {
        .irq_vectors       = true,
        .enable_68k_sound  = true,
        .model1_sound      = true,
        .board_vblank      = true,
        .i960_board_timing = true,
        .model1_io         = true,
        .region            = { .kind = REGION_TODO,
                               .why  = "country setting not looked for yet" },
        /* LEFT / RIGHT the handle; B1 the accelerator (no brake: the plain
         * keys, input_keys_driving). VR1-VR3 are the view buttons. */
        .wheel = { .left = 0x80000000, .right = 0x40000000, .accel = 0x10000000,
                   .view = { 0x0020, 0x0040, 0x0080 } },
        /* Channel 2 is the turret, centred: `up` walks it to the low end,
         * which aims the cannon up. B4 holds the lever in reverse. */
        .analog = { .ch = { IO5649_AN_STEER, IO5649_AN_ACCEL, IO5649_AN_AXIS2 },
                    .in_high = 0x00000100, .up = 0x04000000, .down = 0x08000000 },
    },
};

#endif /* PROFILES_DESERT_H */
