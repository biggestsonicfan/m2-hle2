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
 *
 * Their reticle (Pinboard #1496: lightgun_host.h's style, size, a PNG of the
 * player's own, or none) is chosen under Video > Light gun crosshair or
 * in the TAS window, and kept in <user dir>/lightgun.cfg. An image is drawn
 * centred on the aim, its longer side the reticle's width, tinted the
 * player's colour unless asked not to (a white image takes it best).
 */
#ifndef M2HLE_LIGHTGUN_UI_H
#define M2HLE_LIGHTGUN_UI_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "cimgui.h"
#include "ImGuiFileDialog.h"
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_imgui.h"

#include "backup_ram.h"
#include "retro_shader.h"   /* stb_image, static to it */

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

/* The left button pulls P1's trigger, the right one reloads. */
static inline void lightgun_ui_click(const sapp_event *ev) {
    int b = ev->mouse_button == SAPP_MOUSEBUTTON_LEFT  ? LIGHTGUN_HOST_TRIGGER
          : ev->mouse_button == SAPP_MOUSEBUTTON_RIGHT ? LIGHTGUN_HOST_RELOAD : -1;
    if (b < 0) return;
    bool down = ev->type == SAPP_EVENTTYPE_MOUSE_DOWN;
    if (down && igGetIO()->WantCaptureMouse) return;    /* a click on a window */
    if (down) lightgun_ui_aim_at(ev->mouse_x, ev->mouse_y);
    lightgun_host_button(0, b, down);
}

/* P1's direction keys take the aim back from the mouse. */
static inline void lightgun_ui_key(const sapp_event *ev) {
    int a = input_keycode_to_action((int)ev->key_code);
    if (a == GAME_INPUT_P1_UP || a == GAME_INPUT_P1_DOWN || a == GAME_INPUT_P1_LEFT || a == GAME_INPUT_P1_RIGHT)
        lightgun_host_release_aim(0);
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
    case SAPP_EVENTTYPE_MOUSE_UP:
        lightgun_ui_click(ev);
        break;
    case SAPP_EVENTTYPE_KEY_DOWN:
        lightgun_ui_key(ev);
        break;
    case SAPP_EVENTTYPE_UNFOCUSED:
        lightgun_host_release_all();
        break;
    default: break;
    }
}

/* ---- The reticle's image ------------------------------------------------ */

static struct {
    sg_image img;
    sg_view  view;
    int      w, h;
    uint32_t gen;          /* g_lightgun_reticle_gen it was loaded for */
    char     err[160];     /* why the last one did not load */
} s_lightgun_tex;

static inline void lightgun_ui_drop_image(void) {
    if (s_lightgun_tex.view.id) sg_destroy_view(s_lightgun_tex.view);
    if (s_lightgun_tex.img.id)  sg_destroy_image(s_lightgun_tex.img);
    s_lightgun_tex.view.id = s_lightgun_tex.img.id = 0;
    s_lightgun_tex.w = s_lightgun_tex.h = 0;
}

/* The picture's texture, (re)loaded when its path changed; false when there
 * is none, or it would not decode (s_lightgun_tex.err says why). */
static inline bool lightgun_ui_image(void) {
    if (s_lightgun_tex.gen == g_lightgun_reticle_gen) return s_lightgun_tex.view.id != 0;
    s_lightgun_tex.gen = g_lightgun_reticle_gen;
    lightgun_ui_drop_image();
    s_lightgun_tex.err[0] = '\0';
    const char *path = g_lightgun_reticle_image;
    if (!path[0]) return false;
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(s_lightgun_tex.err, sizeof s_lightgun_tex.err, "the picture cannot be opened"); return false; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = len > 0 && len < (16L << 20) ? (uint8_t *)malloc((size_t)len) : NULL;
    bool got = buf && fread(buf, 1, (size_t)len, f) == (size_t)len;
    fclose(f);
    int w = 0, h = 0, comp;
    uint8_t *rgba = got ? stbi_load_from_memory(buf, (int)len, &w, &h, &comp, 4) : NULL;
    free(buf);
    if (!rgba || w <= 0 || h <= 0 || w > 1024 || h > 1024) {
        snprintf(s_lightgun_tex.err, sizeof s_lightgun_tex.err,
                 rgba ? "the picture is over 1024 x 1024" : "not a PNG, JPEG, BMP or TGA picture");
        if (rgba) stbi_image_free(rgba);
        LOG_WARN("lightgun: reticle %s: %s", path, s_lightgun_tex.err);
        return false;
    }
    s_lightgun_tex.img = sg_make_image(&(sg_image_desc){
        .width = w, .height = h, .pixel_format = SG_PIXELFORMAT_RGBA8,
        .data.mip_levels[0] = { rgba, (size_t)w * (size_t)h * 4u }, .label = "gun-reticle" });
    s_lightgun_tex.view = sg_make_view(&(sg_view_desc){ .texture.image = s_lightgun_tex.img, .label = "gun-reticle-tex" });
    s_lightgun_tex.w = w;
    s_lightgun_tex.h = h;
    stbi_image_free(rgba);
    return true;
}

/* ---- Drawing ------------------------------------------------------------- */

/* One gun's reticle at c, its arm s, in col (its shade under it first). */
static inline void lightgun_ui_reticle(ImDrawList *dl, ImVec2 c, float s, ImU32 col, float z) {
    int st = g_lightgun_reticle;
    if (st == LIGHTGUN_RETICLE_IMAGE && lightgun_ui_image()) {
        float sw = (float)s_lightgun_tex.w, sh = (float)s_lightgun_tex.h;
        float k = 2.0f * s / (sw > sh ? sw : sh);
        ImVec2 a = { c.x - sw * k * 0.5f, c.y - sh * k * 0.5f }, b = { c.x + sw * k * 0.5f, c.y + sh * k * 0.5f };
        ImDrawList_AddImageEx(dl, (ImTextureRef){ NULL, (ImTextureID)simgui_imtextureid(s_lightgun_tex.view) },
                              a, b, (ImVec2){ 0, 0 }, (ImVec2){ 1, 1 }, g_lightgun_reticle_tint ? col : 0xFFFFFFFFu);
        return;
    }
    if (st == LIGHTGUN_RETICLE_IMAGE) st = LIGHTGUN_RETICLE_CROSS;   /* no picture: the cross */
    ImU32 shade = 0xA0000000u;
    float w = z < 1.0f ? 1.0f : z;
    for (int k = 0; k < 2; k++) {
        ImU32 cc = k ? col : shade;
        float t = (k ? 2.0f : 4.0f) * w;
        switch (st) {
        case LIGHTGUN_RETICLE_DOT:
            ImDrawList_AddCircleFilled(dl, c, s * 0.18f + (k ? 0.0f : 1.5f), cc, 0);
            break;
        case LIGHTGUN_RETICLE_BOX: {
            float r = s * 0.7f;
            ImDrawList_AddRectEx(dl, (ImVec2){ c.x - r, c.y - r }, (ImVec2){ c.x + r, c.y + r }, cc, 0.0f, t, 0);
            ImDrawList_AddCircleFilled(dl, c, t * 0.5f, cc, 0);
            break;
        }
        case LIGHTGUN_RETICLE_RING:
            ImDrawList_AddCircleEx(dl, c, s * 0.6f, cc, 0, t);
            /* fallthrough: and the cross */
        default:
            ImDrawList_AddLineEx(dl, (ImVec2){ c.x - s, c.y }, (ImVec2){ c.x - s * 0.3f, c.y }, cc, t);
            ImDrawList_AddLineEx(dl, (ImVec2){ c.x + s * 0.3f, c.y }, (ImVec2){ c.x + s, c.y }, cc, t);
            ImDrawList_AddLineEx(dl, (ImVec2){ c.x, c.y - s }, (ImVec2){ c.x, c.y - s * 0.3f }, cc, t);
            ImDrawList_AddLineEx(dl, (ImVec2){ c.x, c.y + s * 0.3f }, (ImVec2){ c.x, c.y + s }, cc, t);
            break;
        }
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
    if (g_lightgun_reticle == LIGHTGUN_RETICLE_OFF) return;
    float z = (float)g_lightgun_reticle_size / 100.0f;
    float s = (float)h / 384.0f * 10.0f / dpi * z; /* the arm, ~10 board pixels */
    for (int p = 0; p < LIGHTGUN_PLAYERS; p++) {
        if (!lightgun_host_shown(p)) continue;
        float fx, fy;
        lightgun_aim(p, &fx, &fy);
        ImVec2 c = { ((float)ox + fx * (float)w) / dpi, ((float)oy + fy * (float)h) / dpi };
        lightgun_ui_reticle(dl, c, s, p == 0 ? 0xFF2020E0u : 0xFFFF6030u, z);   /* IM_COL32 order: red, blue */
    }
}

/* ---- The settings ---------------------------------------------------------- */

static inline bool lightgun_ui__cfg_path(char *out, size_t cap) {
    if (!backup_ram_user_dir(out, cap)) return false;
    size_t n = strlen(out);
    snprintf(out + n, cap - n, BACKUP_RAM_SEP "lightgun.cfg");
    return true;
}

/* <user dir>/lightgun.cfg: "reticle=ring", "size=100", "tint=1", "image=PATH".
 * Not when the command line chose (--gun-reticle, --gun-reticle-size). */
static inline void lightgun_ui_settings_load(void) {
    char path[512], line[600];
    if (g_lightgun_reticle_cli || !lightgun_ui__cfg_path(path, sizeof path)) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        int st;
        if (!strncmp(line, "reticle=", 8) && (st = lightgun_reticle_parse(line + 8)) >= 0) g_lightgun_reticle = st;
        else if (!strncmp(line, "size=", 5)) g_lightgun_reticle_size = lightgun_reticle_clamp_size(atoi(line + 5));
        else if (!strncmp(line, "tint=", 5)) g_lightgun_reticle_tint = atoi(line + 5) != 0;
        else if (!strncmp(line, "image=", 6)) lightgun_reticle_set_image(line + 6);
    }
    fclose(f);
}

static inline void lightgun_ui_settings_save(void) {
    char path[512], dir[512];
    if (!lightgun_ui__cfg_path(path, sizeof path) || !backup_ram_user_dir(dir, sizeof dir)) return;
    backup_ram_mkdir(dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "reticle=%s\nsize=%d\ntint=%d\nimage=%s\n", k_lightgun_reticle_names[g_lightgun_reticle],
            g_lightgun_reticle_size, g_lightgun_reticle_tint ? 1 : 0, g_lightgun_reticle_image);
    fclose(f);
}

static ImGuiFileDialog *s_lightgun_ui_fd;          /* the window's, from lightgun_ui_dialog */

static inline void lightgun_ui_pick_image(void) {
    if (!s_lightgun_ui_fd) return;
    struct IGFD_FileDialog_Config cfg = IGFD_FileDialog_Config_Get();
    cfg.path = ".";
    cfg.countSelectionMax = 1;
    cfg.flags = ImGuiFileDialogFlags_Modal;
    IGFD_OpenDialog(s_lightgun_ui_fd, "OpenReticle", "Choose a crosshair picture",
                    "Pictures{.png,.jpg,.jpeg,.bmp,.tga}", cfg);
}

/* The picker's dialog, once a frame. */
static inline void lightgun_ui_dialog(ImGuiFileDialog *fd) {
    s_lightgun_ui_fd = fd;
    if (!IGFD_DisplayDialog(fd, "OpenReticle", 0, (ImVec2){ 600, 400 }, (ImVec2){ 1280, 800 })) return;
    if (IGFD_IsOk(fd)) {
        char *picked = IGFD_GetFilePathName(fd, IGFD_ResultMode_AddIfNoFileExt);
        if (picked) {
            lightgun_reticle_set_image(picked);
            g_lightgun_reticle = LIGHTGUN_RETICLE_IMAGE;
            lightgun_ui_settings_save();
            free(picked);
        }
    }
    IGFD_CloseDialog(fd);
}

/* The crosshair's controls: shape, size, a picture of one's own. In a menu or
 * a window. */
static inline void lightgun_ui_settings(void) {
    static const char *const labels[LIGHTGUN_RETICLES] = {
        "Ring and cross", "Cross", "Dot", "Box", "Your own picture", "None",
    };
    bool changed = false;
    igSetNextItemWidth(160);
    changed |= igComboChar("Crosshair", &g_lightgun_reticle, labels, LIGHTGUN_RETICLES);
    igSetNextItemWidth(160);
    changed |= igSliderIntEx("Crosshair size", &g_lightgun_reticle_size, LIGHTGUN_RETICLE_SIZE_MIN,
                             LIGHTGUN_RETICLE_SIZE_MAX, "%d %%", ImGuiSliderFlags_AlwaysClamp);
    if (g_lightgun_reticle == LIGHTGUN_RETICLE_IMAGE) {
        if (igButton("Choose picture...")) lightgun_ui_pick_image();
        igSameLine();
        changed |= igCheckbox("Player's colour", &g_lightgun_reticle_tint);
        igSetItemTooltip("Tint the picture P1 red, P2 blue (a white picture takes it best)");
        if (!lightgun_ui_image())
            igTextDisabled("%s", s_lightgun_tex.err[0] ? s_lightgun_tex.err : "no picture chosen: the cross is drawn");
        else
            igTextDisabled("%s (%d x %d)", g_lightgun_reticle_image, s_lightgun_tex.w, s_lightgun_tex.h);
    }
    if (changed) lightgun_ui_settings_save();
}

/* Video > Light gun crosshair, for a game with guns. */
static inline void lightgun_ui_menu(void) {
    if (!lightgun_ui_on() || !igBeginMenu("Light gun crosshair")) return;
    lightgun_ui_settings();
    igEndMenu();
}

#endif /* M2HLE_LIGHTGUN_UI_H */
