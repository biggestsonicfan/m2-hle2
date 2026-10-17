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
/* The web build (M2HLE_WEB) registers Sonic the Fighters, which it exists to
 * play over RPCN, Sega Rally, Virtual-On, Dynamite Cop and Fighting Vipers
 * (the page's ?game=, web/site/m2hle-games.js). Any other set is only
 * download size: left out, not hidden. See WEB-PORT.md. Both STF profiles are there, since
 * each is its own lobby space. */
#include "srallyc.h"
#include "vonj.h"
#include "dynamcop.h"
#include "fvipers.h"
#ifndef M2HLE_WEB
#include "m2snake.h"
#include "sfight_homebrew.h"
#endif
/* Prepared, not runnable (Pinboard #660): the sets noclip reads that the
 * board cannot run yet. A few hundred bytes of chip lists, so the web build
 * has them too. */
#include "vcop.h"
#include "daytona.h"
#include "hotd.h"

/* Order matters: the first profile for a ROM set is its default
 * (profile_for_rom_set), so Console comes before Arcade. */
const game_profile_t *const g_profiles[] = {
    &sfight_console_profile,
    &sfight_profile,
    &srallyc_profile,
    &vonj_profile,
    &dynamcopb_profile,
    &dyndeka2b_profile,
    &dynamcop_profile,
    &dyndeka2_profile,
    &dynamcopc_profile,
    &fvipers_profile,
#ifndef M2HLE_WEB
    &m2snake_profile,
    &sfight_homebrew_profile,
#endif
};

const size_t g_profile_count = sizeof(g_profiles) / sizeof(g_profiles[0]);

const game_profile_t *g_active_profile = NULL;

const game_profile_t *const g_prepared_profiles[] = {
    &vcop_profile,
    &daytona_profile,
    &hotd_profile,
};

const size_t g_prepared_count = sizeof(g_prepared_profiles) / sizeof(g_prepared_profiles[0]);

#endif /* PROFILES_REGISTRY_H */
