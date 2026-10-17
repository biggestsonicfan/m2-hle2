/*
 * profiles/vcop.h — Virtua Cop (vcop, Revision B). PREPARED, not runnable.
 *
 * Pinboard #660 (parity with noclip): the explorer already reads this set
 * (vendor/noclip js/games.js, `vcop`), so the profile is here to load the same
 * ROMs, with the same model table, ready for the board work. ROM names, CRCs
 * and offsets are MAME's (src/mame/sega/model2.cpp, ROM_START(vcop)).
 *
 * What the board lacks (`preliminary`): Virtua Cop is an original Model 2
 * (MAME model2o). Its coprocessor is the MB86234 TGP that Sega Rally's board/
 * tgp.h already runs, but the memory map differs from 2A's, the I/O is the
 * Model 1 I/O board (two light guns), and the sound board is Model 1's (68000
 * with two MultiPCMs, fed over the i8251 UART), which sound.h does not have.
 */
#ifndef PROFILES_VCOP_H
#define PROFILES_VCOP_H

#include "game_profile.h"
#include "rom_recipe.h"

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
};

static const rom_recipe_t vcop_recipe = {
    .set  = "vcop",
    .size = { [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000,
              [ROM_COPRO_DATA] = 0x800000,   /* empty on this board */
              [ROM_POLYGONS] = 0x1000000, [ROM_TEXTURES] = 0x1000000,
              [ROM_AUDIOCPU] = 0xc0000, [ROM_SAMPLES] = 0x800000 },
    .parts = vcop_parts, .count = sizeof vcop_parts / sizeof vcop_parts[0],
};

static inline int vcop_load(romset_t *rs, const char *child, const char *parent) {
    return rom_recipe_load(rs, child, parent, &vcop_recipe);
}

static const game_profile_t vcop_profile = {
    .id           = "vcop",
    .display_name = "Virtua Cop (Rev B)",
    .board        = BOARD_MODEL2,
    .load_fn      = vcop_load,
    .preliminary  = "original Model 2 board (model2o memory map), Model 1 I/O board "
                    "with light guns, Model 1 sound board (68000 + 2x MultiPCM)",
    .quirks = {
        /* noclip games.js `vcop`: 0x4D2 entries of 16 bytes; palette at 0x4C0000. */
        .model_table_offset = 0x4c8000,
        .model_table_count  = 0x4d2,
        .model_layout       = MODEL_LAYOUT_VCOP,
        .mesh_ptr_subtract  = 0x02000010,
        .mesh_ptr_add       = 0x10,
    },
};

#endif /* PROFILES_VCOP_H */
