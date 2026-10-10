/*
 * profiles/sfight_rng.h — Sonic the Fighters - Arcade on the real board's clock
 * (Pinboard #604). An option while it is measured, not the default.
 *
 * STF's rand (0x66B0) adds the four board timers into its state, so the CPU
 * opponent's choices depend on how far the timers count between frames. A
 * real Model 2B's capture (#603, the rng-serial probe) puts the frame at
 * 434,557 +- 71 timer counts; MAME's screen, 16 MHz / (656 x 424), is 434,600,
 * 57.5241 Hz. The Arcade profile keeps its 60 Hz frame (416,667 cycles).
 *
 * Only the vblank period differs from sfight so far. The i960's cycle table is
 * the same, and the board still counts 2-2.8x more between a timer's re-arm
 * and the idle wait than we charge (tools/stf_ai/board_capture.py).
 */
#ifndef PROFILES_SFIGHT_RNG_H
#define PROFILES_SFIGHT_RNG_H

#include "sfight.h"

#define SFIGHT_RNG_FRAME_CYCLES 434600   /* 25 MHz x 656 x 424 / 16 MHz */

static inline void sfight_rng_install(const romset_t *rs, i960_cpu_t *cpu, memory_bus_t *bus) {
    sfight_install(rs, cpu, bus);
    irqt_set_frame(SFIGHT_RNG_FRAME_CYCLES, 1);
}

static const game_profile_t sfight_rng_profile = {
    .id               = "sfight_rng",
    .display_name     = "Sonic the Fighters - Arcade (board clock)",
    .rom_set          = "sfight",
    .parent_zip_name  = "schamp.zip",
    .board            = BOARD_MODEL2B_CRX,
    .load_fn      = sfight_load,
    .install_fn   = sfight_rng_install,
    .hook_count   = SFIGHT_BASE_HOOK_COUNT,
    .hooks        = { SFIGHT_BASE_HOOKS },
    .input        = { SFIGHT_INPUT_MAP },
    .quirks       = { SFIGHT_QUIRKS },
    .match_info   = sfight_match_info,
};

#endif /* PROFILES_SFIGHT_RNG_H */
