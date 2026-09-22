/*
 * profiles/registry.h — list of all known game profiles.
 *
 * Including this file pulls in every per-game profile header and defines
 * g_profiles[] / g_profile_count + g_active_profile storage. Exactly ONE
 * translation unit may include this — the main.c TU. Other code refers to
 * the externs declared in game_profile.h.
 */
#ifndef PROFILES_REGISTRY_H
#define PROFILES_REGISTRY_H

#include "game_profile.h"
#include "sfight.h"
#include "sfight_console.h"
/* The web build (M2HLE_WEB) registers Sonic the Fighters and nothing else: it
 * exists to play that game over RPCN, and a set that cannot be played online
 * there is only download size. Left out, not hidden. See WEB-PORT.md. Both STF
 * profiles are there, since each is its own lobby space. */
#ifndef M2HLE_WEB
#include "fvipers.h"
#include "m2snake.h"
#endif
/* Future: vf2.h, daytona.h, vcop.h, ... */

/* Order matters: the first profile for a ROM set is its default
 * (profile_for_rom_set), so Console comes before Arcade. */
const game_profile_t *const g_profiles[] = {
    &sfight_console_profile,
    &sfight_profile,
#ifndef M2HLE_WEB
    &fvipers_profile,
    &m2snake_profile,
#endif
};

const size_t g_profile_count = sizeof(g_profiles) / sizeof(g_profiles[0]);

const game_profile_t *g_active_profile = NULL;

#endif /* PROFILES_REGISTRY_H */
