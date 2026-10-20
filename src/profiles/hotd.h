/*
 * profiles/hotd.h — The House of the Dead (hotd: Revision A, or the first release, MAME's hotdo). PREPARED, not
 * runnable.
 *
 * Pinboard #660 (parity with noclip): the explorer reads this set (vendor/
 * noclip js/games.js, `hotd` / `hotdo`), so the profile loads the same ROMs
 * with the same model table, ready for the board work. ROM names, CRCs and
 * offsets are MAME's (src/mame/sega/model2.cpp, ROM_START(hotd)).
 *
 * What the board lacks (`preliminary`): HOTD is a Model 2C. Its coprocessor is
 * the MB86235 TGPx4, whose program is copro_data here. board/tgpx4.h runs it
 * for Dynamite Cop's 2C set (board/model2c_cop.h), which uploads its program;
 * this set is not wired to it yet. The sound board is the
 * same SCSP + 68000 as STF's, so sound.h should take it as it is, and the
 * light guns go through the 2C's I/O.
 */
#ifndef PROFILES_HOTD_H
#define PROFILES_HOTD_H

#include "game_profile.h"
#include "rom_recipe.h"

static const rom_part_t hotd_parts[] = {
    ROM_PAIR32(ROM_MAINCPU,    0x0000000, "epr-19696a.15", 0x42adc32e, "epr-19697a.16", 0x1e247cd5),
    ROM_PAIR32(ROM_MAINCPU,    0x0100000, "epr-19694.13",  0xe85ca1a3, "epr-19695.14",  0xcd52b461),
    ROM_PAIR32(ROM_MAIN_DATA,  0x0000000, "mpr-19704.11",  0xaa80dbb0, "mpr-19705.12",  0xf906843b),
    ROM_PAIR32(ROM_MAIN_DATA,  0x0800000, "mpr-19702.9",   0xfc8aa3b7, "mpr-19703.10",  0x208d993d),
    ROM_PAIR32(ROM_MAIN_DATA,  0x1000000, "mpr-19700.7",   0x0558cfd3, "mpr-19701.8",   0x224a8929),
    ROM_PAIR32(ROM_MAIN_DATA,  0x1800000, "epr-19698.5",   0xe7a7b6ea, "epr-19699.6",   0x8160b3d9),
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1900000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1a00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1b00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1c00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1d00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1e00000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x1800000, 0x1f00000, 0x100000),
    /* The TGPx4's program. */
    ROM_PAIR32(ROM_COPRO_DATA, 0x0000000, "epr-19707.29",  0x384fd133, "epr-19706.30",  0x1277531c),
    ROM_PAIR32(ROM_POLYGONS,   0x0000000, "mpr-19715.17",  0x3ff7dda7, "mpr-19711.21",  0x080d13f1),
    ROM_PAIR32(ROM_POLYGONS,   0x0800000, "mpr-19714.18",  0x3e55ab49, "mpr-19710.22",  0x80df1036),
    ROM_PAIR32(ROM_POLYGONS,   0x1000000, "mpr-19713.19",  0x4d092cd3, "mpr-19709.23",  0xd08937bf),
    ROM_PAIR32(ROM_POLYGONS,   0x1800000, "mpr-19712.20",  0x41577943, "mpr-19708.24",  0x5cb790f2),
    ROM_PAIR32(ROM_TEXTURES,   0x0000000, "mpr-19718.27",  0xa9de5924, "mpr-19716.25",  0x45c7dcce),
    ROM_PAIR32(ROM_TEXTURES,   0x0800000, "mpr-19719.28",  0x838f8343, "mpr-19717.26",  0x393e440b),
    ROM_SWAP16(ROM_AUDIOCPU,   0x0000000, "epr-19720.31",  0xb367d21d),
    ROM_SWAP16(ROM_SAMPLES,    0x0000000, "mpr-19721.32",  0xf5d8fa9a),
    ROM_SWAP16(ROM_SAMPLES,    0x0400000, "mpr-19722.34",  0xa56fa539),
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

static const game_profile_t hotd_profile = {
    .id           = "hotd",
    .display_name = "The House of the Dead (Rev A)",
    .board        = BOARD_MODEL2C_CRX,
    .load_fn      = hotd_load,
    .preliminary  = "Model 2C board: its MB86235 TGPx4 is not wired for this set, "
                    "and no light-gun I/O",
    .quirks = {
        /* noclip games.js `hotd` / `hotdo`: 7477 entries of 16 bytes; the
         * board's mesh-pointer encoding, as STF's. */
        .model_table_offset = 0xe73530,
        .model_table_count  = 7477,
        .mesh_ptr_subtract  = 0x02000010,
        .mesh_ptr_add       = 0x10,
        .enable_68k_sound   = false,
    },
};

#endif /* PROFILES_HOTD_H */
