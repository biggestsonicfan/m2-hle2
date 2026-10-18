/*
 * lightgun_ui.h — the desktop's mouse as P1's light gun, and the crosshairs
 * (Pinboard #1034).
 *
 * lightgun_host.h does the work every front end shares: the mouse over the
 * game's picture aims P1's gun, the left button is the trigger (P1 B1) and the
 * right button reloads (P1 B2); off the picture is off the screen. Here: a key
 * that walks P1's gun hands it back to the pad until the mouse moves again, and
 * clicks an ImGui window takes are not shots.
 *
 * The crosshairs are drawn where the board's guns point (lightgun_aim), not
 * where the mouse is: in a session or a replay that is the pad's aim.
 */
#ifndef M2HLE_LIGHTGUN_UI_H
#define M2HLE_LIGHTGUN_UI_H

#include <stdbool.h>

#include "cimgui.h"
#include "sokol_app.h"

#include "game_profile.h"
#include "input.h"
#include "lightgun.h"
#include "lightgun_host.h"

typedef struct { int x, y, w, h; } lightgun_ui_rect_t;

static lightgun_ui_rect_t s_lightgun_ui_rect;          /* the game's picture, last frame */

static inline bool lightgun_ui_on(void) { return lightgun_host_on(); }

/* Where the picture went this frame (window_game_rect), framebuffer pixels. */
static inline void lightgun_ui_set_rect(int x, int y, int w, int h) {
    s_lightgun_ui_rect = (lightgun_ui_rect_t){ x, y, w, h };
}

static inline void lightgun_ui_aim_at(float mx, float my) {
    const lightgun_ui_rect_t *r = &s_lightgun_ui_rect;
    lightgun_host_aim(0, mx, my, (float)r->x, (float)r->y, (float)r->w, (float)r->h);
}

static inline void lightgun_ui_event(const sapp_event *ev) {
    if (!lightgun_ui_on()) return;
    switch (ev->type) {
    case SAPP_EVENTTYPE_MOUSE_MOVE:
        lightgun_ui_aim_at(ev->mouse_x, ev->mouse_y);
        break;
    case SAPP_EVENTTYPE_MOUSE_LEAVE:
        lightgun_host_aim_away(0);
        break;
    case SAPP_EVENTTYPE_MOUSE_DOWN:
    case SAPP_EVENTTYPE_MOUSE_UP: {
        int b = ev->mouse_button == SAPP_MOUSEBUTTON_LEFT  ? LIGHTGUN_HOST_TRIGGER
              : ev->mouse_button == SAPP_MOUSEBUTTON_RIGHT ? LIGHTGUN_HOST_RELOAD : -1;
        if (b < 0) break;
        bool down = ev->type == SAPP_EVENTTYPE_MOUSE_DOWN;
        if (down && igGetIO()->WantCaptureMouse) break;    /* a click on a window */
        if (down) lightgun_ui_aim_at(ev->mouse_x, ev->mouse_y);
        lightgun_host_button(0, b, down);
        break;
    }
    case SAPP_EVENTTYPE_KEY_DOWN: {
        int a = input_keycode_to_action((int)ev->key_code);
        if (a == GAME_INPUT_P1_UP || a == GAME_INPUT_P1_DOWN || a == GAME_INPUT_P1_LEFT || a == GAME_INPUT_P1_RIGHT)
            lightgun_host_release_aim(0);
        break;
    }
    case SAPP_EVENTTYPE_UNFOCUSED:
        lightgun_host_release_all();
        break;
    default: break;
    }
}

/* A crosshair per gun: P1 red; P2 blue, once it has left its rest. */
static inline void lightgun_ui_draw(int ox, int oy, int w, int h) {
    if (!lightgun_ui_on() || w <= 0 || h <= 0) return;
    float dpi = sapp_dpi_scale() > 0 ? sapp_dpi_scale() : 1.0f;
    ImDrawList *dl = igGetBackgroundDrawList();
    if (lightgun_host_flash_tick())                /* the trigger's flash (lightgun_host.h) */
        ImDrawList_AddRectFilled(dl, (ImVec2){ (float)ox / dpi, (float)oy / dpi },
                                 (ImVec2){ (float)(ox + w) / dpi, (float)(oy + h) / dpi }, 0xFFFFFFFFu);
    float s = (float)h / 384.0f * 10.0f / dpi;     /* the arm, ~10 board pixels */
    for (int p = 0; p < LIGHTGUN_PLAYERS; p++) {
        if (!lightgun_host_shown(p)) continue;
        float fx, fy;
        lightgun_aim(p, &fx, &fy);
        ImVec2 c = { ((float)ox + fx * (float)w) / dpi, ((float)oy + fy * (float)h) / dpi };
        ImU32 col = p == 0 ? 0xFF2020E0u : 0xFFFF6030u;    /* IM_COL32 order: red, blue */
        ImU32 shade = 0xA0000000u;
        for (int k = 0; k < 2; k++) {
            ImU32 cc = k ? col : shade;
            float t = k ? 2.0f : 4.0f;
            ImDrawList_AddCircleEx(dl, c, s * 0.6f, cc, 0, t);
            ImDrawList_AddLineEx(dl, (ImVec2){ c.x - s, c.y }, (ImVec2){ c.x - s * 0.3f, c.y }, cc, t);
            ImDrawList_AddLineEx(dl, (ImVec2){ c.x + s * 0.3f, c.y }, (ImVec2){ c.x + s, c.y }, cc, t);
            ImDrawList_AddLineEx(dl, (ImVec2){ c.x, c.y - s }, (ImVec2){ c.x, c.y - s * 0.3f }, cc, t);
            ImDrawList_AddLineEx(dl, (ImVec2){ c.x, c.y + s * 0.3f }, (ImVec2){ c.x, c.y + s }, cc, t);
        }
    }
}

#endif /* M2HLE_LIGHTGUN_UI_H */
