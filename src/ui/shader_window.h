/*
 * shader_window.h -- the desktop build's side of post_shader.h: the Video menu,
 * the custom preset's parameter sliders, and remembering the choice.
 *
 * The choice lives in video.cfg beside the per-user netplay settings
 * (netplay_cfg_path's directory: %APPDATA%\m2hle2, ~/.config/m2hle2):
 *
 *   filter=off|crt|custom
 *   preset=<path to the .glslp or .glsl>
 *   input_scale=0..4          (0 = automatic)
 *   param.<NAME>=<value>      (the custom preset's parameters)
 *
 * The command line (--crt, --shader, --shader-scale, --no-shader) applies over
 * it for that run and is not written back.
 */
#ifndef SHADER_WINDOW_H
#define SHADER_WINDOW_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cimgui.h"
#include "ImGuiFileDialog.h"
#include "post_shader.h"
#include "log.h"

static struct {
    bool show_params;
    char cfg_path[640];
} g_shader_ui;

static inline const char *shader_ui_cfg_path(void) {
    if (!g_shader_ui.cfg_path[0]) {
        char dir[512];
        snprintf(dir, sizeof dir, "%s", netplay_cfg_path());
        char *a = strrchr(dir, '/'), *b = strrchr(dir, '\\');
        char *s = a > b ? a : b;
        if (s) s[1] = '\0'; else dir[0] = '\0';
        snprintf(g_shader_ui.cfg_path, sizeof g_shader_ui.cfg_path, "%svideo.cfg", dir);
    }
    return g_shader_ui.cfg_path;
}

static inline void shader_ui_save(void) {
    const char *path = shader_ui_cfg_path();
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { LOG_WARN("shader: cannot write %s", tmp); return; }
    const post_shader_mode_t m = post_shader_mode();
    fprintf(f, "filter=%s\n", m == POST_SHADER_CRT ? "crt" : m == POST_SHADER_CUSTOM ? "custom" : "off");
    if (post_shader_custom_path()[0]) fprintf(f, "preset=%s\n", post_shader_custom_path());
    fprintf(f, "input_scale=%d\n", post_shader_input_scale());
    for (int i = 0; i < post_shader_param_count(); i++) {
        const rs_param_t *p = post_shader_param(i);
        if (p->value != p->initial) fprintf(f, "param.%s=%g\n", p->name, p->value);
    }
    fclose(f);
    remove(path);
    if (rename(tmp, path) != 0) LOG_WARN("shader: cannot replace %s", path);
}

/* After post_shader_init, with the GL context current. */
static inline void shader_ui_load(void) {
    FILE *f = fopen(shader_ui_cfg_path(), "r");
    if (!f) return;
    char line[1024], filter[16] = "off", preset[RS_PATH] = "";
    struct { char name[RS_NAME]; float v; } params[RS_MAX_PARAMS];
    int n_params = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "filter"))           snprintf(filter, sizeof filter, "%s", v);
        else if (!strcmp(k, "preset"))      snprintf(preset, sizeof preset, "%s", v);
        else if (!strcmp(k, "input_scale")) post_shader_set_input_scale(atoi(v));
        else if (!strncmp(k, "param.", 6) && n_params < RS_MAX_PARAMS) {
            snprintf(params[n_params].name, sizeof params[n_params].name, "%s", k + 6);
            params[n_params++].v = (float)atof(v);
        }
    }
    fclose(f);
    if (preset[0] && post_shader_custom_supported()) {
        if (post_shader_load(preset)) {
            for (int i = 0; i < n_params; i++) post_shader_set_param(params[i].name, params[i].v);
        } else {
            LOG_WARN("shader: the saved preset %s did not load: %s", preset, post_shader_error());
        }
    }
    if (!strcmp(filter, "crt")) post_shader_set_mode(POST_SHADER_CRT);
    else if (!strcmp(filter, "custom") && post_shader_custom_loaded()) post_shader_set_mode(POST_SHADER_CUSTOM);
    else post_shader_set_mode(POST_SHADER_OFF);
}

static inline void shader_ui_open_dialog(ImGuiFileDialog *fd) {
    struct IGFD_FileDialog_Config cfg = IGFD_FileDialog_Config_Get();
    cfg.path = ".";
    cfg.countSelectionMax = 1;
    cfg.flags = ImGuiFileDialogFlags_Modal;
    IGFD_OpenDialog(fd, "OpenShader", "Load a libretro GLSL shader", "Libretro GLSL{.glslp,.glsl}", cfg);
}

/* The Video menu, in the main menu bar. */
static inline void shader_ui_menu(ImGuiFileDialog *fd) {
    if (!igBeginMenu("Video")) return;
    const post_shader_mode_t m = post_shader_mode();
    if (igMenuItemEx("No filter", NULL, m == POST_SHADER_OFF, true)) {
        post_shader_set_mode(POST_SHADER_OFF);
        shader_ui_save();
    }
    if (igMenuItemEx("CRT filter (Lost Judgment)", NULL, m == POST_SHADER_CRT, true)) {
        post_shader_set_mode(POST_SHADER_CRT);
        shader_ui_save();
    }
    igSetItemTooltip("Scanlines and an aperture grille, as Lost Judgment's arcade cabinets show "
                     "Sonic the Fighters (ported from YAMP). Costs GPU time.");
    if (post_shader_custom_loaded()) {
        char label[RS_NAME + 32];
        snprintf(label, sizeof label, "Custom: %s", post_shader_custom_name());
        if (igMenuItemEx(label, NULL, m == POST_SHADER_CUSTOM, true)) {
            post_shader_set_mode(POST_SHADER_CUSTOM);
            shader_ui_save();
        }
    }
    igSeparator();
    const bool can = post_shader_custom_supported();
    if (igMenuItemEx("Load libretro shader (.glslp / .glsl)...", NULL, false, can)) shader_ui_open_dialog(fd);
    if (!can) igSetItemTooltip("Custom shaders need the OpenGL renderer (the Linux and web builds). "
                               "This build draws with Direct3D 11.");
    else      igSetItemTooltip("A preset from libretro's glsl-shaders. Slang (.slangp) presets are not supported.");
    if (igMenuItemEx("Shader parameters...", NULL, g_shader_ui.show_params, post_shader_param_count() > 0))
        g_shader_ui.show_params = !g_shader_ui.show_params;
    if (post_shader_custom_loaded() && igMenuItem("Unload custom shader")) {
        post_shader_unload();
        g_shader_ui.show_params = false;
        shader_ui_save();
    }
    if (igBeginMenu("Filter input")) {
        static const char *const labels[5] = {
            "Automatic", "1x (496x384, the board's own)", "2x (992x768)", "3x (1488x1152)", "4x (1984x1536)",
        };
        for (int i = 0; i < 5; i++) {
            if (igMenuItemEx(labels[i], NULL, post_shader_input_scale() == i, true)) {
                post_shader_set_input_scale(i);
                shader_ui_save();
            }
        }
        igEndMenu();
    }
    igSetItemTooltip("The resolution the game is drawn at before the filter reads it. Automatic is the "
                     "window's size for the CRT filter and 1x for a custom shader.");
    igSeparator();
    igTextDisabled("Filters can cut performance. Turn one off if the game slows down.");
    if (post_shader_error()[0]) {
        igPushTextWrapPos(igGetFontSize() * 30.0f);
        igTextWrapped("%s", post_shader_error());
        igPopTextWrapPos();
    }
    igEndMenu();
}

/* The file dialog and the parameters window. Every frame, with the other windows. */
static inline void shader_ui_draw(ImGuiFileDialog *fd) {
    ImVec2 min_size = { 600, 400 }, max_size = { 1280, 800 };
    if (IGFD_DisplayDialog(fd, "OpenShader", 0, min_size, max_size)) {
        if (IGFD_IsOk(fd)) {
            char *picked = IGFD_GetFilePathName(fd, IGFD_ResultMode_AddIfNoFileExt);
            if (picked) {
                if (post_shader_load(picked)) {
                    g_shader_ui.show_params = post_shader_param_count() > 0;
                    shader_ui_save();
                }
                free(picked);
            }
        }
        IGFD_CloseDialog(fd);
    }

    if (!g_shader_ui.show_params || post_shader_param_count() == 0) return;
    igSetNextWindowSize((ImVec2){ 460, 420 }, ImGuiCond_FirstUseEver);
    if (igBegin("Shader parameters", &g_shader_ui.show_params, 0)) {
        igTextDisabled("%s", post_shader_custom_name());
        if (igButton("Reset all")) {
            for (int i = 0; i < post_shader_param_count(); i++) {
                rs_param_t *p = post_shader_param(i);
                p->value = p->initial;
            }
            shader_ui_save();
        }
        igSeparator();
        for (int i = 0; i < post_shader_param_count(); i++) {
            rs_param_t *p = post_shader_param(i);
            igPushIDInt(i);
            if (p->max > p->min) {
                igSliderFloat(p->desc, &p->value, p->min, p->max);
                /* The preset's step, as RetroArch's menu has it. */
                if (p->step > 0.0f) p->value = p->min + (float)(int)((p->value - p->min) / p->step + 0.5f) * p->step;
                if (igIsItemDeactivatedAfterEdit()) shader_ui_save();
            } else {
                igTextDisabled("%s: %g", p->desc, p->value);
            }
            igPopID();
        }
    }
    igEnd();
}

#endif /* SHADER_WINDOW_H */
