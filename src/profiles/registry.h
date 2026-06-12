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
#include "fvipers.h"
#include "m2snake.h"
/* Future: vf2.h, daytona.h, vcop.h, ... */

const game_profile_t *const g_profiles[] = {
    &sfight_profile,
    &fvipers_profile,
    &m2snake_profile,
};

const size_t g_profile_count = sizeof(g_profiles) / sizeof(g_profiles[0]);

const game_profile_t *g_active_profile = NULL;

#endif /* PROFILES_REGISTRY_H */
