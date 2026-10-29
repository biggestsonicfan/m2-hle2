#ifndef M2HLE_CABINET_PICTURE_H
#define M2HLE_CABINET_PICTURE_H

/*
 * The Picture menu's cabinet (Pinboard #1056): a game whose colour tables
 * (colorxlat, 0x01810000) depend on the cabinet its test menu is set to shows
 * another cabinet's picture live, with no reboot. Daytona USA builds them once
 * at boot from its CABINET setting, and Deluxe's are darker in the shadows than
 * Twin's (profiles/daytona.h).
 *
 * Host only. The board's colorxlat stays as the game wrote it (the i960 never
 * reads it back); what draws it -- the 3D fill's tables (game_frame.h) and the
 * tile pens (video_window.h) -- takes cabinet_picture_xlat instead, and adds
 * cabinet_picture_gen to the bus's gen_lut so a switch is drawn at once.
 * Savestates, replays, det_digest and netplay never see it, and two players in
 * a session may each show their own.
 *
 * The choice is by the cabinet's id ("twin", "deluxe"), so a front end keeps
 * one string; a game that has no such cabinet shows its own tables. NULL or ""
 * is the board's own: whatever its test menu is set to.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "game_profile.h"
#include "hle_hooks.h"   /* g_active_profile */
#include "memory.h"

static struct {
    char                  want[16];   /* the cabinet's id; "" the board's own */
    uint32_t              gen;        /* bumped by every choice */
    const game_profile_t *built_for;  /* the profile xlat was built for, or NULL */
    uint32_t              built_gen;
    bool                  have;       /* xlat holds the chosen cabinet's tables */
    uint8_t               xlat[COLORXLAT_SIZE];
} g_cabinet_picture;

static inline void cabinet_picture_choose(const char *id) {
    if (!strcmp(g_cabinet_picture.want, id ? id : "")) return;
    snprintf(g_cabinet_picture.want, sizeof g_cabinet_picture.want, "%s", id ? id : "");
    g_cabinet_picture.gen++;
}

static inline const char *cabinet_picture_chosen(void) { return g_cabinet_picture.want; }

/* Added to bus->gen_lut by whatever caches the tables. */
static inline uint32_t cabinet_picture_gen(void) { return g_cabinet_picture.gen; }

/* The index of cabinet `id` in p's cabinets, or -1. */
static inline int cabinet_picture_index(const game_profile_t *p, const char *id) {
    if (!p || !p->cabinets || !p->cabinet_xlat_fn || !id || !id[0]) return -1;
    for (int i = 0; i < p->cabinet_count; i++)
        if (!strcmp(p->cabinets[i].id, id)) return i;
    return -1;
}

/* The colour tables to draw with: the chosen cabinet's, once the game has
 * loaded its own (until then the board's, all zero, which the tile pens take
 * as "show the palette as is"); else the board's. */
static inline const uint8_t *cabinet_picture_xlat(const memory_bus_t *bus) {
    const game_profile_t *p = g_active_profile;
    if (g_cabinet_picture.built_for != p || g_cabinet_picture.built_gen != g_cabinet_picture.gen) {
        const int i = cabinet_picture_index(p, g_cabinet_picture.want);
        g_cabinet_picture.have = i >= 0;
        if (i >= 0) p->cabinet_xlat_fn(i, g_cabinet_picture.xlat);
        g_cabinet_picture.built_for = p;
        g_cabinet_picture.built_gen = g_cabinet_picture.gen;
    }
    if (!g_cabinet_picture.have) return bus->colorxlat;
    /* Loaded: the same test as tile_renderer.h's pens, each channel's luma 0x40. */
    for (uint32_t c5 = 0; c5 < 32; c5++)
        for (uint32_t ch = 0; ch < 3; ch++)
            if (bus->colorxlat[(ch * 0x2000u + 0x40u + (c5 << 8)) * 2u]) return g_cabinet_picture.xlat;
    return bus->colorxlat;
}

#endif /* M2HLE_CABINET_PICTURE_H */
