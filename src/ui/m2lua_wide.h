/*
 * m2lua_wide.h -- the picture a game script asks for (Pinboard #973, #976).
 *
 * Model2_SetWideScreen and the Stretch switches (core/m2lua.h) into
 * g_game_wide (ui/game_render.h): 16:9 with the 3D widened while the script has
 * widescreen on, the tile layers stretched by the Stretch switches (A or B: the
 * two chips' layers are drawn as one). The board's 4:3 with no script, scripts
 * off or a session holding the script. The sdl3 frontend and the web call it
 * once a picture, before they size anything by game_render_wide_width().
 * Drawing only: nothing the i960 reads changes.
 */
#ifndef M2LUA_WIDE_H
#define M2LUA_WIDE_H

#include "m2lua.h"
#include "game_render.h"

/* True when the picture's width changed. */
static inline bool m2lua_wide_apply(void) {
    const m2lua_video_flags_t *v = &g_m2lua.video;
    bool on = g_m2lua.loaded && !g_m2lua.disabled && !g_m2lua.suspended && v->widescreen;
    game_wide_t want = { 0 };
    if (on) want = (game_wide_t){
        .extra        = GAME_RENDER_WIDE_EXTRA,
        .stretch_low  = v->stretch_a_low  || v->stretch_b_low,
        .stretch_high = v->stretch_a_high || v->stretch_b_high,
    };
    bool changed = want.extra != g_game_wide.extra;
    g_game_wide = want;
    return changed;
}

#endif
