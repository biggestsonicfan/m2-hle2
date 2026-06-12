/*
 * profiles/m2snake.h — "Snake" homebrew profile.
 *
 * Runs a homebrew i960 program ROM (built by the sibling m2-snake project) on
 * the shared Model 2B board, using Sonic The Fighters' DATA ROMs for everything
 * else (so the cabinet/romset is authentic STF — only the program ROM differs).
 *
 * Layout the loader expects, all in the directory of the picked .zip:
 *     m2snake.zip                      (marker file — selects this profile)
 *     sfight.zip  schamp.zip           (STF data ROMs, as MAME)
 *     m2snake/epr-19001.15             (homebrew program ROM, low half)
 *     m2snake/epr-19002.16             (homebrew program ROM, high half)
 *
 * m2snake_load reuses sfight_load to pull in every STF data region (and the STF
 * maincpu), then OVERWRITES the maincpu with the homebrew pair via the same
 * interleave_32_word the loader uses everywhere.
 *
 * No HLE hooks and no interrupt handlers: hook_count = 0 and irq_handler all
 * zero, so the emulator never injects STF-specific behaviour into our code. The
 * homebrew is self-contained — it polls the I/O ports and busy-waits for pacing
 * (the emu thread's no-frame-hook fallback paces it at 60 Hz).
 */
#ifndef PROFILES_M2SNAKE_H
#define PROFILES_M2SNAKE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "log.h"
#include "rom_loader.h"
#include "game_profile.h"
#include "memory.h"
#include "i960.h"
#include "irq_timer.h"  /* g_irqt — raise the vblank pending bit for the homebrew */
#include "sfight.h"     /* reuse sfight_load (STF data) + sfight_install (PRCB walk) */

/* Copy the directory portion (incl. trailing separator) of `path` into `out`.
 * Empty string if `path` has no separator (file in cwd). */
static inline void m2snake_dirname(const char *path, char *out, size_t out_sz) {
    const char *sep_f = strrchr(path, '/');
    const char *sep_b = strrchr(path, '\\');
    const char *sep   = sep_f > sep_b ? sep_f : sep_b;
    size_t n = sep ? (size_t)(sep - path + 1) : 0;
    if (n >= out_sz) n = out_sz - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}

/* ---- Loader ------------------------------------------------------------- */

static inline int m2snake_load(romset_t *rs, const char *primary_zip, const char *parent_zip) {
    (void)parent_zip;
    char dir[512];
    m2snake_dirname(primary_zip, dir, sizeof(dir));

    char sf[600], sc[600], hb1[600], hb2[600];
    snprintf(sf,  sizeof(sf),  "%ssfight.zip", dir);
    snprintf(sc,  sizeof(sc),  "%sschamp.zip", dir);
    snprintf(hb1, sizeof(hb1), "%sm2snake/epr-19001.15", dir);
    snprintf(hb2, sizeof(hb2), "%sm2snake/epr-19002.16", dir);

    /* Pull in all STF data regions (and the STF maincpu, which we then replace). */
    if (sfight_load(rs, sf, sc) != 0) {
        LOG_ERROR("m2snake: failed to load STF data ROMs from %s / %s", sf, sc);
        return -1;
    }

    /* Override the program ROM with the homebrew pair. */
    size_t s1 = 0, s2 = 0;
    uint8_t *h1 = file_load(hb1, &s1);
    uint8_t *h2 = file_load(hb2, &s2);
    if (!h1 || !h2) {
        LOG_ERROR("m2snake: homebrew program ROM not found (%s / %s) — build the m2-snake project", hb1, hb2);
        free(h1); free(h2);
        romset_free(rs);
        return -1;
    }
    memset(rs->maincpu, 0, rs->maincpu_size);
    interleave_32_word(rs->maincpu, rs->maincpu_size, h1, s1, h2, s2, 0);
    free(h1); free(h2);
    LOG_INFO("m2snake: program ROM overridden with homebrew (%zu + %zu bytes)", s1, s2);
    return 0;
}

/* ---- Installer ---------------------------------------------------------- */
/* Identical board bring-up to STF: copy regions, walk the PRCB for the reset
 * IP. The STF-specific XTRA_DATA mirror is harmless here (our ROM ignores it). */
static inline void m2snake_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    sfight_install(rs, cpu, bus);
}

/* ---- Profile object -----------------------------------------------------
 * No HLE hooks and no hardcoded homebrew addresses: the snake self-paces by
 * polling + ACKing the board vblank bit (intreq bit0), exactly as it does on
 * real hardware and in MAME. The `board_vblank` quirk makes the emu thread
 * raise that bit each 60 Hz slice; the fallback slice timing paces it. */

static const game_profile_t m2snake_profile = {
    .id               = "m2snake",
    .display_name     = "Snake (homebrew)",
    .parent_zip_name  = NULL,             /* loader derives the STF zips itself */
    .board            = BOARD_MODEL2B_CRX,
    .load_fn          = m2snake_load,
    .install_fn       = m2snake_install,
    .hook_count       = 0,                /* no HLE hooks — pure data profile    */
    .input = {
        /* Standard Model 2B board input bits (same layout STF assembles), so the
         * existing active-low IN0/IN1/IN2 port serving in input.h feeds the
         * homebrew's port reads directly. */
        .held_addr      = 0,
        .momentary_addr = 0,
        .bits = {
            [GAME_INPUT_P1_UP]    = 0x00002000,
            [GAME_INPUT_P1_DOWN]  = 0x00001000,
            [GAME_INPUT_P1_LEFT]  = 0x00008000,
            [GAME_INPUT_P1_RIGHT] = 0x00004000,
            [GAME_INPUT_P1_B1]    = 0x00000100,
            [GAME_INPUT_P1_B2]    = 0x00000200,
            [GAME_INPUT_P1_B3]    = 0x00000400,
            [GAME_INPUT_P1_START] = 0x00000010,
            [GAME_INPUT_P2_START] = 0x00000020,
            [GAME_INPUT_SERVICE]  = 0x00000004,
            [GAME_INPUT_P1_COIN]  = 0x00000001,
            [GAME_INPUT_P2_COIN]  = 0x00000002,
        },
    },
    .quirks = {
        /* Bring up the 68K sound block even though the homebrew is silent: the
         * emu thread sound_steps every slice regardless, so the 68K must be in a
         * valid reset state running a real driver. STF's sound 68K just idles
         * waiting for commands our i960 never sends — harmless, and avoids
         * stepping an uninitialised 68K. */
        .enable_68k_sound   = true,
        /* No interrupt handlers: the homebrew polls the pad once per frame
         * (315-5649 protocol, STF read_sw). m2-hle2 and MAME raise different
         * IRQs, so a per-frame poll is the portable equivalent. */
        .irq_handler        = { 0, 0, 0, 0 },
        .camera_struct_addr = 0,
        /* Enable the authentic 3D draw path (0x3C007878): the shared STF data
         * ROMs carry the 5103-entry model table + meshes, so the homebrew can
         * issue real object draws and geo3d decodes them from the COP stream. */
        .poly_connect_mask  = 0x45B4,
        .mesh_ptr_subtract  = 0x02000010,
        .mesh_ptr_add       = 0x10,
        .model_table_offset = 0x000E0004,
        .model_table_count  = 5103,
        .warning_skip_addr  = 0,
        /* Board raises the vblank pending bit each 60 Hz slice so the homebrew's
         * frame_wait poll advances one frame/slice with no HLE hook. */
        .board_vblank       = true,
        /* The homebrew drives the GEO directly (builds a display list in
         * bufferram), so decode that list instead of the STF COP bone stream. */
        .geo_displaylist    = true,
    },
};

#endif /* PROFILES_M2SNAKE_H */
