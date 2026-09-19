/*
 * objview_window.h — ImGui controls for the debug object viewer (objview.h).
 *
 * Everything the MCP bridge's objview_* commands can set is here too, and both
 * drive the same state: a session that starts by hand can be taken over by a
 * script mid-way and the other way round, which is the point — an artifact
 * found by eye is then handed to a sweep without retyping the numbers.
 *
 * The preview is the very render target the screenshots are read out of, so
 * what this window shows is what a shot would write.
 */
#ifndef OBJVIEW_WINDOW_H
#define OBJVIEW_WINDOW_H

#include <stdbool.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"
#include "sokol_imgui.h"

#include "objview.h"

static inline void objview_window_draw(bool *p_open, const romset_t *rs, memory_bus_t *bus) {
    /* The viewer only renders while something is looking at it — this flag is
     * what stops a closed window from costing a decode and a pass per frame. */
    g_objview.preview_open = (p_open && *p_open) ? 1 : 0;
    if (!p_open || !*p_open) return;

    igSetNextWindowPos((ImVec2){ 390, 60 }, ImGuiCond_Once);
    igSetNextWindowSize((ImVec2){ 620, 720 }, ImGuiCond_Once);
    if (!igBegin("Object viewer", p_open, 0)) { igEnd(); return; }

    objview_t *v = &g_objview.v;
    bool active = v->active != 0;
    if (igCheckbox("Active", &active)) v->active = active;
    igSameLine();
    if (igIsItemHovered(0))
        igSetTooltip("Decode and draw the selected model offscreen, on its own.\n"
                     "The game's own frame is untouched either way.");

    /* ---- Readiness ------------------------------------------------------- */
    {
        objview_ready_t rdy;
        objview_probe(rs, bus, &rdy);
        if (rdy.ready) {
            igPushStyleColorImVec4(ImGuiCol_Text, (ImVec4){ 0.3f, 1.0f, 0.3f, 1.0f });
            igText("ready");
        } else {
            igPushStyleColorImVec4(ImGuiCol_Text, (ImVec4){ 1.0f, 0.7f, 0.2f, 1.0f });
            igText("not ready - run until attract starts");
        }
        igPopStyleColor();
        igText("rom=%d profile=%d frames=%u tex=%d%% pal=%d%% models=%u captures=%d 3d=%d",
               rdy.rom_loaded, rdy.profile, rdy.frames, rdy.tex_pct, rdy.pal_pct,
               rdy.models, rdy.captures, rdy.saw_3d);
    }

    igSeparator();

    /* ---- Preview --------------------------------------------------------- */
    if (g_objview.color_tex.id && g_objview.rt_w > 0) {
        /* Fit the window's width, but never so tall that the controls below are
         * pushed out of reach on a short window. */
        float avail_w = igGetContentRegionAvail().x;
        if (avail_w < 64.0f) avail_w = 64.0f;
        float scale = avail_w / (float)g_objview.rt_w;
        float max_h = 300.0f;
        if (scale * (float)g_objview.rt_h > max_h) scale = max_h / (float)g_objview.rt_h;
        if (scale > 1.0f) scale = 1.0f;
        igImage((ImTextureRef){ NULL, (ImTextureID)simgui_imtextureid(g_objview.color_tex) },
                (ImVec2){ (float)g_objview.rt_w * scale, (float)g_objview.rt_h * scale });
    } else if (v->active) {
        igTextDisabled("(no preview yet)");
    } else {
        igTextDisabled("(tick Active to render)");
    }

    igSeparator();

    /* ---- What to draw ---------------------------------------------------- */
    igText("Object:");
    igRadioButtonIntPtr("model table", &v->source, 0);
    igSameLine();
    igRadioButtonIntPtr("this frame's capture", &v->source, 1);

    if (v->source == 0) {
        igSetNextItemWidth(140);
        igInputIntEx("model##ov", &v->model, 1, 25, 0);
        if (v->model < 0) v->model = 0;
    } else {
        int n = g_geo3d_state ? g_geo3d_state->captured_count : 0;
        igSetNextItemWidth(140);
        igDragIntEx("capture##ov", &v->capture, 1.0f, 0, n > 0 ? n - 1 : 0, "%d", 0);
        bool ucm = v->use_capture_matrix != 0;
        if (igCheckbox("use the board's matrix (ignores pos/rot/scale)", &ucm))
            v->use_capture_matrix = ucm;
    }
    if (v->have_result)
        igText("  drawn model %d - %d tris, %d lines", v->drawn_model, v->tris, v->lines);
    if (!g_objview.last_ok && g_objview.last_err[0]) {
        igPushStyleColorImVec4(ImGuiCol_Text, (ImVec4){ 1.0f, 0.4f, 0.4f, 1.0f });
        igTextWrapped("  %s", g_objview.last_err);
        igPopStyleColor();
    }

    igSeparator();

    /* ---- Placement ------------------------------------------------------- */
    igText("Placement in world space:");
    igDragFloat3Ex("pos##ov",   v->pos, 0.05f, -5000.0f, 5000.0f, "%.3f", 0);
    igDragFloat3Ex("rot deg##ov", v->rot, 0.5f,  -360.0f,  360.0f, "%.1f", 0);
    igDragFloatEx("scale##ov", &v->scale, 0.01f, 0.001f, 100.0f, "%.3f", 0);

    igSeparator();

    /* ---- Camera ---------------------------------------------------------- */
    igText("Camera (orbit about the target):");
    igDragFloatEx("yaw deg##ov",   &v->yaw,   0.5f, -3600.0f, 3600.0f, "%.1f", 0);
    igDragFloatEx("pitch deg##ov", &v->pitch, 0.5f,   -89.0f,   89.0f, "%.1f", 0);
    igDragFloatEx("fov deg##ov",   &v->fov,   0.5f,     5.0f,  150.0f, "%.1f", 0);
    bool fit = v->autofit != 0;
    if (igCheckbox("auto-fit (centre + range on the model's bounds)", &fit)) v->autofit = fit;
    if (fit) {
        igDragFloatEx("fit margin##ov", &v->fit_margin, 0.01f, 0.5f, 5.0f, "%.2f", 0);
        igText("  dist %.3f   target (%.2f, %.2f, %.2f)",
               v->dist, v->target[0], v->target[1], v->target[2]);
    } else {
        igDragFloatEx("dist##ov", &v->dist, 0.05f, 0.05f, 5000.0f, "%.3f", 0);
        igDragFloat3Ex("target##ov", v->target, 0.05f, -5000.0f, 5000.0f, "%.3f", 0);
    }
    if (v->have_result) {
        igText("  bounds (%.2f, %.2f, %.2f) .. (%.2f, %.2f, %.2f)",
               v->bmin[0], v->bmin[1], v->bmin[2], v->bmax[0], v->bmax[1], v->bmax[2]);
        igText("  eye (%.2f, %.2f, %.2f)", v->cam[0], v->cam[1], v->cam[2]);
    }

    igSeparator();

    /* ---- Presentation ---------------------------------------------------- */
    igText("Image:");
    igSetNextItemWidth(90);
    igInputIntEx("w##ov", &v->width, 0, 0, 0);
    igSameLine();
    igSetNextItemWidth(90);
    igInputIntEx("h##ov", &v->height, 0, 0, 0);
    igColorEdit3("background##ov", v->bg, 0);
    { bool b = v->wireframe != 0; if (igCheckbox("wireframe overlay##ov", &b)) v->wireframe = b; }
    igSameLine();
    { bool b = v->textured != 0;  if (igCheckbox("textured##ov", &b))          v->textured = b; }
    igText("Backface cull:"); igSameLine();
    igRadioButtonIntPtr("off##ovc", &v->cull, 0); igSameLine();
    igRadioButtonIntPtr("CW##ovc",  &v->cull, 1); igSameLine();
    igRadioButtonIntPtr("CCW##ovc", &v->cull, 2);

    igSeparator();

    igEnd();
}

#endif /* OBJVIEW_WINDOW_H */
