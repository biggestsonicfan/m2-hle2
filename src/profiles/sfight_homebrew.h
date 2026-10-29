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
 * interrupts through the program's own interrupt table (irq_vectors), and no
 * frame hook (board_vblank: the run loop marks its frames at the vblank). Its
 * idle loops are found in the program at install, as every game's are
 * (m2_spin_install). It is not
 * picked by the set's name; profile_for_program picks it when the program's interrupt table does
 * not name the handlers STF's profile does. `--profile sfight_homebrew` picks
 * it outright.
 *
 * The board has its comm board, as MAME's sfight does (m2comm.h, the frame
 * offset 0x1C0 of every ROM but Power Sled's). STF never starts it; homebrew
 * can link two cabinets over it (Pinboard #1632: Fighters Megamix's board
 * build, m2-megamix board/src/link.c). Its role is where that build keeps it,
 * in backup RAM (sfight_homebrew_link_role).
 */
#ifndef PROFILES_SFIGHT_HOMEBREW_H
#define PROFILES_SFIGHT_HOMEBREW_H

#include "game_profile.h"
#include "sfight.h"
#include "m2comm.h"
#include "savestate_reg.h"

static const game_profile_t sfight_homebrew_profile;

/* STF's boot and the comm board. */
static inline void sfight_homebrew_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    sfight_install(rs, cpu, bus);
    m2comm_install(bus, 0x1C0u);
    savestate_extra("sfight_homebrew", "M2COMM", &g_m2comm, sizeof g_m2comm);

}

/* A linked cabinet's role, where Fighters Megamix's board build reads it at
 * boot (m2-megamix board/src/link.c, link_save): past STF's settings in backup
 * RAM, "MMX1", the role (0 alone, 1 1P's cabinet, the master; 2 2P's, the
 * slave) and its complement. A program that keeps nothing there never reads
 * it; a link session's backup RAM is not the player's (comm_role.h). */
#define SFIGHT_HOMEBREW_LINK     0x01D03FF0u
#define SFIGHT_HOMEBREW_LINK_TAG 0x4D4D5831u   /* "MMX1" */

static inline bool sfight_homebrew_link_role(memory_bus_t *bus, game_link_role_t role) {
    uint32_t r = role == GAME_LINK_MASTER ? 1u : role == GAME_LINK_SLAVE ? 2u : 0u;
    mem_write32(bus, SFIGHT_HOMEBREW_LINK,      SFIGHT_HOMEBREW_LINK_TAG);
    mem_write32(bus, SFIGHT_HOMEBREW_LINK + 4u, r);
    mem_write32(bus, SFIGHT_HOMEBREW_LINK + 8u, ~r);
    return true;
}

static const game_profile_t sfight_homebrew_profile = {
    .id               = "sfight_homebrew",
    .display_name     = "Homebrew on Sonic the Fighters' board",
    .rom_set          = "sfight",
    .parent_zip_name  = "schamp.zip",
    .board            = BOARD_MODEL2B_CRX,
    .load_fn          = sfight_load,
    .install_fn       = sfight_homebrew_install,
    .hook_count       = 0,
    .input            = { SFIGHT_INPUT_MAP },
    .any_program      = true,
    .link_role        = sfight_homebrew_link_role,
    .quirks = {
        .enable_68k_sound   = true,
        .irq_vectors        = true,
        .board_vblank       = true,
        /* STF's data ROMs are on the board: a program can draw its models. */
        .mesh_ptr_subtract  = 0x02000010,
        .mesh_ptr_add       = 0x10,
        .model_table_offset = 0x000E0004,
        .model_table_count  = 5103,
        /* No geo_displaylist: the program's list is drawn by the board's GEO
         * walk (geo3d_scan_geo_list), as STF's is. m2-snake's own format and
         * its walker (geo3d_scan_displaylist) stop at direct data, which
         * m2-sdk's m2_sprite.h draws every sprite with. */
        .region             = { .kind = REGION_HOMEBREW, .why = "homebrew: no country setting" },
    },
};

#endif /* PROFILES_SFIGHT_HOMEBREW_H */
