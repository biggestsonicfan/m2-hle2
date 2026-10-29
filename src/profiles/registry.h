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
 * play over RPCN, Sega Rally, Virtual-On, Dynamite Cop, Fighting Vipers,
 * The House of the Dead, Daytona USA, Virtua Fighter 2, Dead or Alive and
 * Air Walkers, Power Sled, Virtua Cop, Last Bronx, Pilot Kids and Motor Raid, with the clones of #1286 (the page's ?game=, web/site/m2hle-games.js). Any other set is only
 * download size: left out, not hidden. See WEB-PORT.md. Both STF profiles are there, since
 * each is its own lobby space. */
#include "srallyc.h"
#include "vonj.h"
#include "dynamcop.h"
#include "fvipers.h"
#include "hotd.h"      /* runs since Pinboard #1034, on the web since #1040 */
#include "daytona.h"   /* runs since Pinboard #1035, on the web since #1047 */
#include "schamp.h"    /* Pinboard #1286, on the web too */
#include "doa.h"       /* runs since Pinboard #1050, on the web too */
#include "vf2.h"       /* runs since Pinboard #1046, on the web since #1055 */
#include "airwlkrs.h"  /* runs since Pinboard #1063, on the site since #1063 */
#include "powsled.h"   /* runs since Pinboard #1052, on the site since #1089 */
#include "vcop.h"      /* runs since Pinboard #1286 */
#include "lastbrnx.h"  /* runs since Pinboard #1517, on the site since #1535 */
#include "pltkids.h"   /* runs since Pinboard #1538, on the site too */
#include "motoraid.h"  /* runs since Pinboard #1627, on the site since #1628 */
#ifndef M2HLE_WEB
#include "m2_library.h" /* the rest of MAME's library, Pinboard #1872 */
#include "m2snake.h"
#include "sfight_homebrew.h"
#endif
/* Prepared, not runnable (Pinboard #660): the sets noclip reads that the
 * board cannot run yet. A few hundred bytes of chip lists, so the web build
 * has them too. None today: Virtua Cop, the last, runs since #1286. */

/* Order matters: the first profile for a ROM set is its default
 * (profile_for_rom_set), so Console comes before Arcade. */
const game_profile_t *const g_profiles[] = {
    &sfight_console_profile,
    &sfight_profile,
    &srallyc_profile,
    &vonj_profile,
    &von_profile,
    &dynamcopb_profile,
    &dyndeka2b_profile,
    &dynamcop_profile,
    &dyndeka2_profile,
    &dynamcopc_profile,
    &fvipers_profile,
    &hotd_profile,
    &daytona_profile,
    &doa_profile,
    &doab_profile,
    &doaa_profile,
    &doaab_profile,
    &doaae_profile,
    &vf2_profile,
    &vf2b_profile,
    &vf2a_profile,
    &vf2o_profile,
    &airwlkrs_profile,
    &powsled_profile,
    &vcop_profile,
    &lastbrnx_profile,
    &lastbrnxu_profile,
    &lastbrnxj_profile,
    &pltkids_profile,
    &pltkidsa_profile,
    &motoraid_profile,
    /* The clones of #1286, on the site under their game (web/site/m2hle-games.js). */
    &schamp_profile,
    &hotdo_profile,
    &hotdp_profile,
    &daytona93_profile,
    &daytonase_profile,
    &daytonas_profile,
    &daytonat_profile,
    &daytonata_profile,
    &daytonagtx_profile,
#ifndef M2HLE_WEB
    &manxtt_profile,
    &skytargt_profile,
    &vcop2_profile,
    &zeroguna_profile,
    &vstriker_profile,
    &vstrikero_profile,
    &zerogun_profile,
    &zerogunj_profile,
    &dynabb_profile,
    &gunblade_profile,
    &indy500_profile,
    &indy500d_profile,
    &sgt24h_profile,
    &overrevb_profile,
    &overrev_profile,
    &stcc_profile,
    &skisuprg_profile,
    &segawski_profile,
    &waverunr_profile,
    &topskatr_profile,
    &bel_profile,
    &m2snake_profile,
    &sfight_homebrew_profile,
#endif
};

const size_t g_profile_count = sizeof(g_profiles) / sizeof(g_profiles[0]);

const game_profile_t *g_active_profile = NULL;

const game_profile_t *const g_prepared_profiles[] = { NULL };

const size_t g_prepared_count = 0;

#endif /* PROFILES_REGISTRY_H */
