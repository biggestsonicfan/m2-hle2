/*
 * profiles/sfight_homebrew.h — a program that is not Sonic the Fighters, on
 * Sonic the Fighters' board.
 *
 * Model 2 homebrew ships as the stock `sfight` set with its program EPROMs
 * swapped (m2-pacman: epr-19001.15, epr-19002.16 and the sound EPROM
 * epr-19021.31, as MAME and a real board run it). Every address in STF's
 * profile belongs to STF's program: its HLE hooks, its interrupt handlers
 * (0xC40, ...), its sound queue and warning flag. In another program they land
 * in the middle of its functions, or in its interrupt table.
 *
 * So this profile runs any program on the set with none of them: no hooks,
 * interrupts through the program's own interrupt table (irq_vectors), and the
 * vblank raised by the board (board_vblank). It is not picked by the set's
 * name; profile_for_program picks it when the program's interrupt table does
 * not name the handlers STF's profile does. `--profile sfight_homebrew` picks
 * it outright.
 */
#ifndef PROFILES_SFIGHT_HOMEBREW_H
#define PROFILES_SFIGHT_HOMEBREW_H

#include "game_profile.h"
#include "sfight.h"

static const game_profile_t sfight_homebrew_profile = {
    .id               = "sfight_homebrew",
    .display_name     = "Homebrew on Sonic the Fighters' board",
    .rom_set          = "sfight",
    .parent_zip_name  = "schamp.zip",
    .board            = BOARD_MODEL2B_CRX,
    .load_fn          = sfight_load,
    .install_fn       = sfight_install,
    .hook_count       = 0,
    .input            = { SFIGHT_INPUT_MAP },
    .any_program      = true,
    .quirks = {
        .enable_68k_sound   = true,
        .irq_vectors        = true,
        .board_vblank       = true,
        /* STF's data ROMs are on the board: a program can draw its models. */
        .mesh_ptr_subtract  = 0x02000010,
        .mesh_ptr_add       = 0x10,
        .model_table_offset = 0x000E0004,
        .model_table_count  = 5103,
        .geo_displaylist    = true,
    },
};

#endif /* PROFILES_SFIGHT_HOMEBREW_H */
