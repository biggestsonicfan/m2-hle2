/*
 * geo3d_window.h — ImGui controls for the 3D pipeline.
 *
 * Lets you toggle rendering, switch between capture-list and manual
 * single-model browsing, scroll the camera, filter or isolate specific
 * captures, and force the test triangle to confirm the GPU pipeline is alive.
 */
#ifndef GEO3D_WINDOW_H
#define GEO3D_WINDOW_H

#include <stdbool.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"

#include "geo3d.h"

static inline void geo3d_window_draw(geo3d_state_t *geo, bool *p_open,
                                      const uint8_t *main_data, size_t main_data_size,
                                      uint32_t table_off, uint32_t table_count,
                                      uint32_t mesh_ptr_subtract) {
    igSetNextWindowPos((ImVec2){10, 460}, ImGuiCond_Once);
    igSetNextWindowSize((ImVec2){360, 420}, ImGuiCond_Once);

    if (!igBegin("3D", p_open, 0)) { igEnd(); return; }

    igCheckbox("Enabled",           &geo->enabled);
    igCheckbox("Use captures",      &geo->use_captures);
    igCheckbox("Use object matrix", &geo->use_matrix);
    igCheckbox("Use game view",     &geo->use_game_view);
    if (geo->use_game_view && geo->has_game_view) {
        igSameLine();
        igPushStyleColorImVec4(ImGuiCol_Text, (ImVec4){0.3f, 1.0f, 0.3f, 1.0f});
        igText("[reading]");
        igPopStyleColor();
    }
    igCheckbox("Test triangle",     &geo->test_triangle);

    igSeparator();
    igText("Captured: %d / %d   Lines: %d / %d",
           geo->captured_count, MAX_GEO_MODELS,
           g_geo3d_lines.count, GEO3D_MAX_LINES);

    igSeparator();
    if (geo->use_captures) {
        /* ---- Capture isolate ---- */
        igText("Isolate capture (-1 = off):");
        if (geo->isolate_index < 0) {
            if (igButton("Isolate")) geo->isolate_index = 0;
        } else {
            igSetNextItemWidth(120);
            int mx = geo->captured_count > 0 ? geo->captured_count - 1 : 0;
            igDragIntEx("##iso", &geo->isolate_index, 1.0f, 0, mx, "%d", 0);
            igSameLine();
            if (igButton("Prev##iso")) { if (geo->isolate_index > 0) geo->isolate_index--; }
            igSameLine();
            if (igButton("Next##iso")) { if (geo->isolate_index < mx) geo->isolate_index++; }
            igSameLine();
            if (igButton("Off##iso"))  { geo->isolate_index = -1; }

            if (geo->isolate_index >= 0 && geo->isolate_index < geo->captured_count) {
                const captured_model_t *cm = &geo->captured[geo->isolate_index];
                igText("  [%d] model_idx=%d  has_mat=%d",
                       geo->isolate_index, cm->model_idx, (int)cm->has_matrix);
                if (cm->has_matrix)
                    igText("  pos=(%.2f, %.2f, %.2f)",
                           cm->matrix[3], cm->matrix[7], cm->matrix[11]);
            }
        }

        igSeparator();
        igText("Filter range:");
        igCheckbox("Filter enabled", &geo->filter_enabled);
        if (geo->filter_enabled) {
            igDragIntEx("Filter min", &geo->filter_min, 1.0f, 0, MAX_GEO_MODELS - 1, "%d", 0);
            igDragIntEx("Filter max", &geo->filter_max, 1.0f, 0, MAX_GEO_MODELS - 1, "%d", 0);
        }
    } else {
        /* ---- Single-model browser ---- */
        igText("Single-model browser:");
        igSetNextItemWidth(120);
        int mi = geo->model_index;
        if (igInputIntEx("##midx", &mi, 1, 10, 0)) {
            if (mi < 0) mi = 0;
            if (table_count > 0 && (uint32_t)mi >= table_count) mi = (int)table_count - 1;
            geo->model_index = mi;
        }
        igSameLine();
        if (igButton("Prev##mb")) { if (geo->model_index > 0) geo->model_index--; }
        igSameLine();
        if (igButton("Next##mb")) {
            if (table_count == 0 || (uint32_t)geo->model_index + 1 < table_count)
                geo->model_index++;
        }

        /* Live model table entry readout */
        if (main_data && table_count > 0 && (uint32_t)geo->model_index < table_count) {
            uint32_t toff = table_off + (uint32_t)geo->model_index * MODEL_ENTRY_SIZE;
            if ((size_t)toff + MODEL_ENTRY_SIZE <= main_data_size) {
                uint32_t f1  = read_u32_le(main_data + toff + 0);
                uint32_t tex = read_u32_le(main_data + toff + 4);
                uint32_t pol = read_u32_le(main_data + toff + 8);
                uint32_t f4  = read_u32_le(main_data + toff + 12);
                uint32_t mesh_off = (pol >= (mesh_ptr_subtract >> 2)) ?
                                    pol * 4u - mesh_ptr_subtract : 0;
                igText("  f1=0x%08X  tex=0x%08X", f1, tex);
                igText("  pol=0x%08X -> mesh@0x%08X", pol, mesh_off);
                igText("  f4=0x%08X  Lines: %d", f4, g_geo3d_lines.count);
            }
        } else if (!main_data) {
            igText("  (no ROM loaded)");
        }
    }

    igSeparator();
    igText("Camera:");
    if (geo->use_game_view) {
        igText("  X=%.3f  Y=%.3f  Z=%.3f", geo->cam_x, geo->cam_y, geo->cam_z);
        igText("  rot_x=%.3f  rot_y=%.3f", geo->rot_x, geo->rot_y);
        igDragFloatEx("FOV", &geo->fov_deg, 1.0f, 15.0f, 120.0f, "%.1f deg", 0);
    } else {
        igDragFloatEx("cam X", &geo->cam_x, 0.1f, -1000.0f, 1000.0f, "%.2f", 0);
        igDragFloatEx("cam Y", &geo->cam_y, 0.1f, -1000.0f, 1000.0f, "%.2f", 0);
        igDragFloatEx("cam Z", &geo->cam_z, 0.1f, -1000.0f, 1000.0f, "%.2f", 0);
        igDragFloatEx("rot Y", &geo->rot_y, 0.01f, -6.28f, 6.28f, "%.3f rad", 0);
        igDragFloatEx("rot X", &geo->rot_x, 0.01f, -6.28f, 6.28f, "%.3f rad", 0);
        igDragFloatEx("FOV",   &geo->fov_deg, 1.0f, 15.0f, 120.0f, "%.1f deg", 0);
    }
    if (igButton("Reset Camera")) {
        geo->cam_x   = 0.0f;
        geo->cam_y   = 0.0f;
        geo->cam_z   = 5.0f;
        geo->rot_x   = 0.0f;
        geo->rot_y   = 0.0f;
        geo->fov_deg = 60.0f;
    }

    igSeparator();
    igText("Game-camera convention (attract dir):");
    {
        bool nx = g_cam_sign_x < 0, ny = g_cam_sign_y < 0, nz = g_cam_sign_z < 0;
        bool nrx = g_cam_sign_rx < 0, nry = g_cam_sign_ry < 0;
        if (igCheckbox("neg eye X",  &nx))  g_cam_sign_x  = nx  ? -1.0f : 1.0f;
        igSameLine(); if (igCheckbox("neg eye Y", &ny))  g_cam_sign_y  = ny  ? -1.0f : 1.0f;
        if (igCheckbox("neg eye Z",  &nz))  g_cam_sign_z  = nz  ? -1.0f : 1.0f;
        igSameLine(); if (igCheckbox("neg pitch", &nrx)) g_cam_sign_rx = nrx ? -1.0f : 1.0f;
        if (igCheckbox("neg yaw",    &nry)) g_cam_sign_ry = nry ? -1.0f : 1.0f;
        if (igButton("Reset convention")) {
            g_cam_sign_x = 1.0f; g_cam_sign_y = 1.0f; g_cam_sign_z = -1.0f;
            g_cam_sign_rx = 1.0f; g_cam_sign_ry = -1.0f;
        }
        bool ro = g_cam_rot_only != 0;
        if (igCheckbox("rotation only (eye baked)", &ro)) g_cam_rot_only = ro;
        bool ab = g_cam_auto_baked != 0;
        if (igCheckbox("auto eye-baked (SETPOS=-eye)", &ab)) g_cam_auto_baked = ab;
        igText("cam_mode = %d  (9=fight look-at, 0=attract movie)", g_cam_mode_value);
        bool ar = g_cam_auto_rot != 0;
        if (igCheckbox("auto rot-only when cam_mode != 9", &ar)) g_cam_auto_rot = ar;
        bool we = g_geo_windows_enabled != 0;
        if (igCheckbox("window (set_window) support", &we)) g_geo_windows_enabled = we;
        bool sg = g_geo_subwin_game_cam != 0;
        if (igCheckbox("sub-window uses game camera", &sg)) g_geo_subwin_game_cam = sg;
    }

    igSeparator();
    igText("Portrait cells (chaos attract):");
    igDragFloatEx("Portrait FOV", &g_portrait_fov_deg, 0.5f, 0.0f, 120.0f,
                  "%.1f deg (0=auto ~23)", 0);
    igDragFloatEx("Portrait camY", &g_portrait_cam_y_frac, 0.02f, -2.0f, 2.0f,
                  "%.2f (1=centered)", 0);
    igDragFloatEx("Portrait rowSnap", &g_portrait_row_shift, 0.02f, -2.0f, 2.0f,
                  "%.2f (top down/bot up)", 0);
    if (igButton("Reset Portrait")) {
        g_portrait_fov_deg    = 0.0f;
        g_portrait_cam_y_frac = 0.0f;
        g_portrait_row_shift  = 0.5f;
    }

    igSeparator();
    igText("Texture UV orientation:");
    igCheckbox("swap u/v (rotate 90)", &g_uv_swap);
    igCheckbox("flip u (horizontal)",  &g_uv_flip_u);
    igCheckbox("flip v (vertical)",    &g_uv_flip_v);
    igText("Texture bank:");
    igRadioButtonIntPtr("auto (bit12)",  &g_uv_bank_mode, 0);
    igRadioButtonIntPtr("force sheet 0", &g_uv_bank_mode, 1);
    igRadioButtonIntPtr("force sheet 1", &g_uv_bank_mode, 2);
    igRadioButtonIntPtr("swap banks",    &g_uv_bank_mode, 3);

    igEnd();
}

#endif /* GEO3D_WINDOW_H */
