/*
 * schamp.h - Sonic Championship, the export build of Sonic The Fighters
 * (Pinboard #1286).
 *
 * MAME ROM_START(schamp), the parent of sfight in MAME. The mask ROMs are
 * sfight's chip for chip; the program pair (epr-19141/19142) and the
 * XTRA_DATA pair (epr-19143/19144, the English text) are its own.
 *
 * The program is sfight's with code put in or taken out in a few places, so
 * most of it sits a little later (noclip games.js SCHAMP_RELOCATION). The
 * hooks below are the sfight hooks whose 16 bytes are the same at the moved
 * address, and whose RAM (0x500000 on) the game keeps where sfight does:
 * every literal the hooks read is referenced as often in both programs.
 *
 * Not carried over: the VS rematch, the enemy rank tables, the cross-play
 * hooks, replay_stage, sky_eye, the texture loader rows and the native
 * replays. Each reads or skips code at sfight addresses no one has checked
 * in this program yet. Without them the game plays as the board does.
 */
#ifndef PROFILES_SCHAMP_H
#define PROFILES_SCHAMP_H

#include "sfight.h"
#include "../core/rom_recipe.h"

static const rom_part_t schamp_parts[] = {
    ROM_PAIR32(ROM_MAINCPU,    0x0000000, "epr-19141.15", 0xb942ef21, "epr-19142.16", 0x2d54bd76),

    ROM_PAIR32(ROM_MAIN_DATA,  0x0000000, "mpr-19007.11", 0x8b8ff751, "mpr-19008.12", 0xa94654f5),
    ROM_PAIR32(ROM_MAIN_DATA,  0x0800000, "mpr-19005.9",  0x98cd1127, "mpr-19006.10", 0xe79f0a26),
    ROM_PAIR32(ROM_MAIN_DATA,  0x1000000, "epr-19143.7",  0xf97176fd, "epr-19144.8",  0xd040202a),
    ROM_MIRROR(ROM_MAIN_DATA,  0x1000000, 0x1100000, 0x100000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x1000000, 0x1200000, 0x200000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x1000000, 0x1400000, 0x400000),
    ROM_MIRROR(ROM_MAIN_DATA,  0x1000000, 0x1800000, 0x800000),

    ROM_PAIR32(ROM_COPRO_DATA, 0x0000000, "mpr-19015.29", 0xc74d99e3, "mpr-19016.30", 0x746ae931),

    ROM_PAIR32(ROM_POLYGONS,   0x0000000, "mpr-19009.17", 0xfd410350, "mpr-19012.21", 0x9bb7b5b6),
    ROM_PAIR32(ROM_POLYGONS,   0x0800000, "mpr-19010.18", 0x6fd94187, "mpr-19013.22", 0x9e232fe5),

    ROM_PAIR32(ROM_TEXTURES,   0x0000000, "mpr-19019.27", 0x59121896, "mpr-19017.25", 0x7b298379),
    ROM_PAIR32(ROM_TEXTURES,   0x0800000, "mpr-19020.28", 0x9540dba0, "mpr-19018.26", 0x3b7e7a12),

    ROM_SWAP16(ROM_AUDIOCPU,   0x0000000, "epr-19021.31", 0x0b9f7583),

    ROM_SWAP16(ROM_SAMPLES,    0x0000000, "mpr-19022.32", 0x4381869b),
    ROM_SWAP16(ROM_SAMPLES,    0x0200000, "mpr-19023.33", 0x07c67f88),
    ROM_SWAP16(ROM_SAMPLES,    0x0400000, "mpr-19024.34", 0x15ff76d3),
    ROM_SWAP16(ROM_SAMPLES,    0x0600000, "mpr-19025.35", 0x6ad8fb70),
};

static const rom_recipe_t schamp_recipe = {
    .set  = "schamp",
    .size = { [ROM_MAINCPU] = 0x200000, [ROM_MAIN_DATA] = 0x2000000,
              [ROM_COPRO_DATA] = 0x800000,
              [ROM_POLYGONS] = 0x1000000, [ROM_TEXTURES] = 0x1000000,
              [ROM_AUDIOCPU] = 0x80000, [ROM_SAMPLES] = 0x800000 },
    .parts = schamp_parts, .count = sizeof schamp_parts / sizeof schamp_parts[0],
};

static inline int schamp_load(romset_t *rs, const char *child, const char *parent) {
    return rom_recipe_load(rs, child, parent, &schamp_recipe);
}

/* sfight's install without the native replay recorder. */
static inline void schamp_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    sfight_install_board(rs, cpu, bus);
}

/* sfight's address in the comment, schamp's in the entry (+ the run's offset). */
#define SCHAMP_HOOK_COUNT 8
#define SCHAMP_HOOKS                                                                \
    { 0x00011A30, sfight_hook_frame_pace,      "frame_pace"              }, /* 0x11A04 */ \
    { 0x000077F8, sfight_hook_cop_err_hang,    "co_processor_error_hang" }, /* 0x77F8 */  \
    { 0x0000DC68, sfight_hook_versus_result,   "versus_result"           }, /* 0xDC3C */  \
    { 0x00062714, sfight_hook_country_default, "country_default"         }, /* 0x62688 */ \
    { 0x00006974, sfight_hook_settings_loaded, "settings_loaded"         }, /* 0x6974 */  \
    { 0x00062700, sfight_hook_damage_default,  "damage_default"          }, /* 0x62674 */ \
    { 0x00062584, sfight_hook_rounds_default,  "rounds_default"          }, /* 0x624F8 */ \
    { 0x0001163C, sfight_hook_idle_spin,       "_idle spin"              }, /* 0x11610 */

_Static_assert(sizeof((hle_hook_entry_t[]){ SCHAMP_HOOKS }) ==
               SCHAMP_HOOK_COUNT * sizeof(hle_hook_entry_t),
               "SCHAMP_HOOK_COUNT does not match SCHAMP_HOOKS");

/* SFIGHT_QUIRKS without the rematch, the rank tables and the attract replay
 * (its state is sfight's movie controller, not checked here). The model
 * table is in the shared mask ROMs; the interrupt handlers and RAM are
 * where sfight has them. */
static const game_profile_t schamp_profile = {
    .id               = "schamp",
    .display_name     = "Sonic Championship",
    .board            = BOARD_MODEL2B_CRX,
    .load_fn      = schamp_load,
    .install_fn   = schamp_install,
    .hook_count   = SCHAMP_HOOK_COUNT,
    .hooks        = { SCHAMP_HOOKS },
    .input        = { SFIGHT_INPUT_MAP },
    .quirks       = {
        .mesh_ptr_subtract  = 0x02000010,
        .mesh_ptr_add       = 0x10,
        .model_table_offset = 0x000E0004,
        .model_table_count  = 5103,
        .camera_struct_addr = 0x00519E98,
        .enable_68k_sound   = true,
        .irq_handler        = { 0x00000C40, 0x00000D10, 0x00000D30, 0x00000DF0 },
        .sound_queue_count_addr = 0x00504001,
        .warning_skip_addr      = 0x00500410,
        .region = { .kind = REGION_HOOKED, .factory_default = "country_default",
                    .over_saved = "settings_loaded", .country = sfight_country },
    },
    .match_info   = sfight_match_info,
};

#endif /* PROFILES_SCHAMP_H */
