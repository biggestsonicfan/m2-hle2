/*
 * profiles/daytona.h — Daytona USA (daytona, Japan, Revision A). PREPARED, not
 * runnable.
 *
 * Pinboard #660 (parity with noclip): the explorer reads this set and its
 * clones (vendor/noclip js/games.js, `daytona`), and the polygon decoder was
 * first worked out on it (CLAUDE.md, "3D Polygon Decoder"). The profile loads
 * the same ROMs with the same model table, ready for the board work. ROM names,
 * CRCs and offsets are MAME's (src/mame/sega/model2.cpp, ROM_START(daytona)).
 *
 * What the board lacks (`preliminary`): Daytona is an original Model 2 (MAME
 * model2o), like Virtua Cop: the MB86234 TGP (board/tgp.h runs Sega Rally's),
 * the model2o memory map, the Model 1 I/O board (wheel, pedals, the drive
 * board's force feedback), the Model 1 sound board, and M2COMM for linked
 * cabinets. The comm board's Z80 program (epr-16726) and the drive board's
 * ROM are not loaded: nothing here would run them.
 */
#ifndef PROFILES_DAYTONA_H
#define PROFILES_DAYTONA_H

#include "game_profile.h"
#include "rom_recipe.h"

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
};

static const rom_recipe_t daytona_recipe = {
    .set  = "daytona",
    .size = { [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000,
              [ROM_COPRO_DATA] = 0x800000,
              [ROM_POLYGONS] = 0x1000000, [ROM_TEXTURES] = 0x1000000,
              [ROM_AUDIOCPU] = 0xc0000, [ROM_SAMPLES] = 0x800000 },
    .parts = daytona_parts, .count = sizeof daytona_parts / sizeof daytona_parts[0],
};

static inline int daytona_load(romset_t *rs, const char *child, const char *parent) {
    return rom_recipe_load(rs, child, parent, &daytona_recipe);
}

static const game_profile_t daytona_profile = {
    .id           = "daytona",
    .display_name = "Daytona USA (Japan, Rev A)",
    .board        = BOARD_MODEL2,
    .load_fn      = daytona_load,
    .preliminary  = "original Model 2 board (model2o memory map), Model 1 I/O board "
                    "with wheel and drive board, Model 1 sound board, M2COMM link",
    .quirks = {
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
