/*
 * tas_window.h -- the desktop build's side of core/tas.h (Pinboard #1462): the
 * TAS menu, the window with the virtual pad, the transport, the branches and
 * the piano roll, the movie file dialogs and the hotkeys.
 *
 *   F10        frame advance (or pause, while it runs)
 *   F11        rewind a frame (held, it keeps going); Shift+F11 undo rewind
 *   F12        recording on / off
 *
 * Everything that touches g_tas takes the emu mutex (tas.h, Threads).
 */
#ifndef TAS_WINDOW_H
#define TAS_WINDOW_H

#include <stdio.h>
#include <string.h>

#include "cimgui.h"
#include "ImGuiFileDialog.h"
#include "emu_thread.h"
#include "tas.h"
#include "lightgun_ui.h"   /* the crosshair's settings (Pinboard #1496) */

static struct {
    bool show;
    char path[512];
    char error[300];
    int  seek_to;
    bool dragging;
} g_tas_ui;

static inline void tas_ui_lock(emu_thread_ctx_t *ctx)   { if (ctx->thread_alive) emu_mutex_lock(&ctx->mutex); }
static inline void tas_ui_unlock(emu_thread_ctx_t *ctx) { if (ctx->thread_alive) emu_mutex_unlock(&ctx->mutex); }

static inline void tas_ui_err(const char *err) {
    snprintf(g_tas_ui.error, sizeof g_tas_ui.error, "%s", err ? err : "");
}

/* A column's label in the piano roll, by GAME_INPUT_*. */
static const char *const s_tas_short[GAME_INPUT_COUNT] = {
    "1U", "1D", "1L", "1R", "1B1", "1B2", "1B3", "1B4", "1S", "1C",
    "2U", "2D", "2L", "2R", "2B1", "2B2", "2B3", "2B4", "2S", "2C",
    "Sv", "Ts", "GN", "G1", "G2", "G3", "G4",
};

/* The board's held bit for action a (0: this game has no such input). */
static inline uint32_t tas_ui_bit(int a) {
    return g_active_profile ? g_active_profile->input.bits[a] : 0;
}

/* ---- Actions --------------------------------------------------------------- */

static inline void tas_ui_start(emu_thread_ctx_t *ctx, bool power_on) {
    tas_ui_lock(ctx);
    const char *err = NULL;
    if (power_on) {
        err = tas__busy();
        if (!err) { g_tas.start_req = true; ctx->request_reset = 1; }
    } else {
        err = tas_start(ctx);
    }
    tas_ui_unlock(ctx);
    tas_ui_err(err);
    if (!err) g_tas_ui.show = true;
}

/* One frame on, from a stopped board; a running one stops. */
static inline void tas_ui_advance(emu_thread_ctx_t *ctx) {
    if (emu_is_running(ctx)) { emu_stop(ctx); return; }
    tas_ui_lock(ctx);
    bool seeking = g_tas.seeking || g_tas.seek_req >= 0;
    tas_ui_unlock(ctx);
    if (!seeking) emu_run_frames(ctx, 1);
}

static inline void tas_ui_rewind(emu_thread_ctx_t *ctx, uint32_t k) {
    tas_ui_lock(ctx);
    tas_rewind(k, false);
    tas_ui_unlock(ctx);
}

static inline void tas_ui_seek(emu_thread_ctx_t *ctx, uint32_t f) {
    tas_ui_lock(ctx);
    g_tas.rewinding = false;
    tas_seek(f, false);
    tas_ui_unlock(ctx);
}

/* The hotkeys; true when the key was TAS's. */
static inline bool tas_ui_hotkey(emu_thread_ctx_t *ctx, const sapp_event *ev) {
    if (!g_tas.on || ev->type != SAPP_EVENTTYPE_KEY_DOWN) return false;
    switch (ev->key_code) {
        case SAPP_KEYCODE_F10:
            if (!ev->key_repeat) tas_ui_advance(ctx);
            return true;
        case SAPP_KEYCODE_F11:
            if (ev->modifiers & SAPP_MODIFIER_SHIFT) {
                if (!ev->key_repeat) {
                    tas_ui_lock(ctx);
                    tas_undo_rewind(false);
                    tas_ui_unlock(ctx);
                }
            } else {
                if (emu_is_running(ctx)) emu_stop(ctx);
                tas_ui_rewind(ctx, 1);
            }
            return true;
        case SAPP_KEYCODE_F12:
            if (!ev->key_repeat) {
                tas_ui_lock(ctx);
                g_tas.recording = !g_tas.recording;
                tas_ui_unlock(ctx);
            }
            return true;
        default:
            return false;
    }
}

/* ---- Files ------------------------------------------------------------------ */

static inline void tas_ui_file_dialog(ImGuiFileDialog *fd, bool save) {
    struct IGFD_FileDialog_Config cfg = IGFD_FileDialog_Config_Get();
    cfg.path = ".";
    cfg.countSelectionMax = 1;
    cfg.flags = ImGuiFileDialogFlags_Modal | (save ? ImGuiFileDialogFlags_ConfirmOverwrite : 0);
    if (save) IGFD_OpenDialog(fd, "SaveTas", "Save the TAS movie", "TAS movies{.m2tas}", cfg);
    else      IGFD_OpenDialog(fd, "OpenTas", "Open a TAS movie", "TAS movies{.m2tas}", cfg);
}

static inline void tas_ui_dialog(emu_thread_ctx_t *ctx, ImGuiFileDialog *fd) {
    ImVec2 min_size = {600, 400}, max_size = {1280, 800};
    for (int save = 0; save < 2; save++) {
        if (!IGFD_DisplayDialog(fd, save ? "SaveTas" : "OpenTas", 0, min_size, max_size)) continue;
        if (IGFD_IsOk(fd)) {
            char *picked = IGFD_GetFilePathName(fd, IGFD_ResultMode_AddIfNoFileExt);
            if (picked) {
                tas_ui_lock(ctx);
                const char *err = save ? tas_save_file(picked) : tas_load_file(ctx, picked, true);
                tas_ui_unlock(ctx);
                tas_ui_err(err);
                snprintf(g_tas_ui.path, sizeof g_tas_ui.path, "%s", picked);
                if (!err) g_tas_ui.show = true;
                free(picked);
            }
        }
        IGFD_CloseDialog(fd);
    }
}

/* ---- The menu ---------------------------------------------------------------- */

static inline void tas_ui_menu(emu_thread_ctx_t *ctx, ImGuiFileDialog *fd, bool rom_loaded) {
    if (!igBeginMenu("TAS")) return;
    igMenuItemBoolPtr("TAS window", NULL, &g_tas_ui.show, true);
    igSeparator();
    if (igMenuItemEx("Start a movie here", NULL, false, rom_loaded)) tas_ui_start(ctx, false);
    if (igMenuItemEx("Start a movie from power-on", NULL, false, rom_loaded)) tas_ui_start(ctx, true);
    if (igMenuItemEx("Take the movie up again", NULL, false, rom_loaded && !g_tas.on && g_tas.anchor)) {
        tas_ui_lock(ctx);
        tas_ui_err(tas_resume(ctx));
        tas_ui_unlock(ctx);
    }
    if (igMenuItemEx("Stop TAS", NULL, false, g_tas.on)) {
        tas_ui_lock(ctx);
        tas_stop("stopped by the player");
        tas_ui_unlock(ctx);
    }
    igSeparator();
    if (igMenuItemEx("Open movie...", NULL, false, rom_loaded)) tas_ui_file_dialog(fd, false);
    if (igMenuItemEx("Save movie...", NULL, false, g_tas.anchor != NULL)) tas_ui_file_dialog(fd, true);
    igEndMenu();
}

/* ---- The window ---------------------------------------------------------------- */

/* The slow-motion pick: 1x, 1/2x, 1/4x, 1/8x. */
static inline void tas_ui_speed(void) {
    static const char *const speeds[] = { "1x", "1/2x", "1/4x", "1/8x" };
    int si = g_tas.slowdown >= 8 ? 3 : g_tas.slowdown >= 4 ? 2 : g_tas.slowdown >= 2 ? 1 : 0;
    igSetNextItemWidth(70);
    if (!igBeginCombo("##speed", speeds[si], 0)) return;
    for (int i = 0; i < 4; i++)
        if (igSelectableEx(speeds[i], i == si, 0, (ImVec2){ 0, 0 })) g_tas.slowdown = 1 << i;
    igEndCombo();
}

/* The seek bar: it seeks when let go. */
static inline void tas_ui_seek_bar(void) {
    int n = (int)g_tas.mv.n;
    if (!g_tas_ui.dragging) g_tas_ui.seek_to = (int)(g_tas.seeking ? g_tas.seek_to : g_tas.frame);
    igSetNextItemWidth(-1);
    igSliderIntEx("##seek", &g_tas_ui.seek_to, 0, n > 0 ? n : 1, "frame %d", 0);
    g_tas_ui.dragging = igIsItemActive();
    if (igIsItemDeactivatedAfterEdit()) {
        g_tas.rewinding = false;
        tas_seek((uint32_t)(g_tas_ui.seek_to > n ? n : g_tas_ui.seek_to), false);
    }
}

static inline void tas_ui_transport(emu_thread_ctx_t *ctx) {
    bool running = emu_is_running(ctx) && !g_tas.seeking;
    if (igButton("|<"))            { g_tas.rewinding = false; tas_seek(0, false); }
    igSetItemTooltip("To the movie's start");
    igSameLine();
    if (igButton("<"))             tas_rewind(1, false);
    igSetItemTooltip("Rewind a frame (F11)");
    igSameLine();
    if (igButton(running ? "Pause" : "Play")) {
        if (running) emu_stop(ctx); else emu_run(ctx);
    }
    igSameLine();
    if (igButton(">"))             { if (!running && !g_tas.seeking) emu_run_frames(ctx, 1); else emu_stop(ctx); }
    igSetItemTooltip("Frame advance (F10)");
    igSameLine();
    if (igButton(">|"))            { g_tas.rewinding = false; tas_seek(g_tas.mv.n, false); }
    igSetItemTooltip("To the movie's end");
    igSameLine();
    igBeginDisabled(g_tas.back_n == 0);
    if (igButton("Undo rewind"))   tas_undo_rewind(false);
    igSetItemTooltip("Back to where the last rewinds began (Shift+F11)");
    igEndDisabled();
    igSameLine();
    tas_ui_speed();
    tas_ui_seek_bar();
}

/* Action a's virtual-pad button: green held, amber the next frame, blue
 * held on the keys or a pad. */
static inline void tas_ui_pad_button(int a, uint32_t bit, bool hold, bool once, bool keys) {
    int colors = 0;
    if (hold || once || keys) {
        ImVec4 c = hold ? (ImVec4){ 0.15f, 0.55f, 0.20f, 1 } : once ? (ImVec4){ 0.70f, 0.55f, 0.10f, 1 }
                        : (ImVec4){ 0.25f, 0.35f, 0.55f, 1 };
        igPushStyleColorImVec4(ImGuiCol_Button, c);
        colors = 1;
    }
    igPushIDInt(a);
    if (igButtonEx(s_tas_short[a], (ImVec2){ 38, 0 })) g_tas.pad.hold ^= bit;
    if (igIsItemClickedEx(ImGuiMouseButton_Right))     g_tas.pad.once ^= bit;
    igSetItemTooltip("%s", g_input_action_names[a]);
    igPopID();
    if (colors) igPopStyleColor();
}

static inline void tas_ui_pad(void) {
    igSeparatorText("Virtual pad");
    igTextDisabled("Click: hold until clicked again. Right-click: the next frame only.");
    int shown = 0;
    for (int a = 0; a < GAME_INPUT_COUNT; a++) {
        uint32_t bit = tas_ui_bit(a);
        if (!bit) continue;
        bool hold = (g_tas.pad.hold & bit) != 0, once = (g_tas.pad.once & bit) != 0;
        bool keys = (g_input.held & bit) != 0;
        /* A row a player, and one for the cabinet's buttons. */
        int group = a < GAME_INPUT_P2_UP ? 0 : a < GAME_INPUT_SERVICE ? 1 : 2;
        static int last_group;
        if (shown && group == last_group) igSameLine();
        last_group = group;
        shown++;
        tas_ui_pad_button(a, bit, hold, once, keys);
    }
    if (igSmallButton("Release all")) { g_tas.pad.hold = 0; g_tas.pad.once = 0; }
    igSameLine();
    igCheckbox("Keys and pads too", &g_tas.keys_too);
    igSetItemTooltip("The keyboard's and the pads' buttons are recorded with the virtual pad's");
}

/* Light guns: where the pad aims each, on a small copy of the picture. */
static inline void tas_ui_aim(void) {
    if (!g_active_profile || !g_active_profile->quirks.lightgun) return;
    igSeparatorText("Gun aim");
    for (int p = 0; p < LIGHTGUN_PLAYERS; p++) {
        igPushIDInt(p);
        char label[32];
        snprintf(label, sizeof label, "Pad aims P%d", p + 1);
        igCheckbox(label, &g_tas.pad.aim_on[p]);
        igSameLine();
        igCheckbox("off the screen", &g_tas.pad.aim_off[p]);
        igSameLine();
        igText("%.3f, %.3f", g_tas.pad.aim_x[p], g_tas.pad.aim_y[p]);
        igPopID();
    }
    /* The off-screen border (lightgun.h): MAME's 5 % turns a shot at the
     * picture's edge into a reload. Recorded with each frame's aim. */
    float border = (float)g_lightgun_border / 10.0f;
    igSetNextItemWidth(160);
    if (igSliderFloatEx("Off-screen border", &border, 0.0f, LIGHTGUN_BORDER_DEF / 10.0f, "%.1f %%", 0))
        g_lightgun_border = lightgun_clamp((int)(border * 10.0f + 0.5f), 0, LIGHTGUN_BORDER_DEF);
    igSetItemTooltip("How near the picture's edge an aim counts as off the screen (a reload).\n"
                     "5 %% is MAME's; a real gun sees the picture to its edge. The border is\n"
                     "recorded with each frame's aim, so the movie plays back the same.");
    lightgun_ui_settings();

    ImVec2 size = { 248, 186 };
    ImVec2 o = igGetCursorScreenPos();
    igInvisibleButton("##aim", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    if (igIsItemActive()) {
        ImVec2 m = igGetMousePos();
        int p = igIsMouseDown(ImGuiMouseButton_Right) ? 1 : 0;
        float x = (m.x - o.x) / size.x, y = (m.y - o.y) / size.y;
        g_tas.pad.aim_x[p] = x < 0 ? 0 : x > 1 ? 1 : x;
        g_tas.pad.aim_y[p] = y < 0 ? 0 : y > 1 ? 1 : y;
        g_tas.pad.aim_on[p] = true;
    }
    igSetItemTooltip("Left: aim P1, right: aim P2");
    ImDrawList *dl = igGetWindowDrawList();
    ImDrawList_AddRectFilledEx(dl, o, (ImVec2){ o.x + size.x, o.y + size.y }, IM_COL32(20, 20, 30, 255), 0, 0);
    static const ImU32 col[LIGHTGUN_PLAYERS] = { IM_COL32(240, 80, 80, 255), IM_COL32(80, 160, 240, 255) };
    for (int p = 0; p < LIGHTGUN_PLAYERS; p++) {
        if (!g_tas.pad.aim_on[p]) continue;
        ImVec2 c = { o.x + g_tas.pad.aim_x[p] * size.x, o.y + g_tas.pad.aim_y[p] * size.y };
        ImDrawList_AddCircleEx(dl, c, 7, col[p], 0, 2);
        ImDrawList_AddLineEx(dl, (ImVec2){ c.x - 10, c.y }, (ImVec2){ c.x + 10, c.y }, col[p], 1);
        ImDrawList_AddLineEx(dl, (ImVec2){ c.x, c.y - 10 }, (ImVec2){ c.x, c.y + 10 }, col[p], 1);
    }
}

static inline void tas_ui_branches(void) {
    igSeparatorText("Branches");
    if (!igBeginTableEx("##branches", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV, (ImVec2){ 0, 0 }, 0))
        return;
    igTableSetupColumnEx("#", ImGuiTableColumnFlags_WidthFixed, 20, 0);
    igTableSetupColumnEx("Kept", ImGuiTableColumnFlags_WidthStretch, 0, 0);
    igTableSetupColumnEx("", ImGuiTableColumnFlags_WidthFixed, 44, 0);
    igTableSetupColumnEx("", ImGuiTableColumnFlags_WidthFixed, 44, 0);
    for (int i = 0; i < TAS_BRANCHES; i++) {
        tas_branch_t *b = &g_tas.branch[i];
        igPushIDInt(i);
        igTableNextRow();
        igTableNextColumn(); igText("%d", i + 1);
        igTableNextColumn();
        if (b->used) igText("frame %u of %u, %s", b->frame, b->track.n, b->when);
        else         igTextDisabled("empty");
        igTableNextColumn();
        if (igSmallButton("Save")) tas_branch_save(i);
        igSetItemTooltip("Keep the movie as it is, and where the board is in it");
        igTableNextColumn();
        igBeginDisabled(!b->used);
        if (igSmallButton("Load")) tas_branch_load(i);
        igEndDisabled();
        igPopID();
    }
    igEndTable();
}

/* The roll's buttons: undo, redo, insert, delete, cut. */
static inline void tas_ui_roll_buttons(void) {
    igBeginDisabled(g_tas.undo_n == 0);
    if (igSmallButton("Undo")) tas_undo(false);
    igEndDisabled();
    igSameLine();
    igBeginDisabled(g_tas.redo_n == 0);
    if (igSmallButton("Redo")) tas_undo(true);
    igEndDisabled();
    igSameLine();
    if (igSmallButton("Insert frame")) tas_insert(g_tas.frame, 1);
    igSetItemTooltip("A copy of the frame before, at the cursor");
    igSameLine();
    if (igSmallButton("Delete frame")) tas_delete(g_tas.frame, 1);
    igSameLine();
    if (igSmallButton("Cut here")) tas_truncate();
    igSetItemTooltip("Drop the frames after the cursor");
    igSameLine();
    igCheckbox("A new take cuts the rest", &g_tas.cut);
}

/* Row f's colour: the cursor's, a frame with no check, the desync's. */
static inline void tas_ui_row_colour(uint32_t f, bool have) {
    if (f == g_tas.frame)
        igTableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(60, 90, 40, 255), -1);
    else if (have && !g_tas.mv.check[f])
        igTableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(45, 35, 35, 255), -1);
    if (g_tas.desync == (int64_t)f)
        igTableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(150, 30, 30, 255), -1);
}

/* Frame f's gun 1: where it aims, or off. */
static inline void tas_ui_gun_cell(uint32_t f) {
    const uint8_t *gun = g_tas.mv.in[f].gun;
    if (!gun[0]) return;
    uint32_t x = 0, y = 0;
    for (int i = 0; i < 4; i++) {
        x |= (uint32_t)gun[4 + i] << (8 * i);
        y |= (uint32_t)gun[8 + i] << (8 * i);
    }
    igText(gun[1] ? "off" : "%.2f,%.2f", lightgun__bitsf(x), lightgun__bitsf(y));
}

/* Row f of the roll: its frame number (a click goes there), a cell an input. */
static inline void tas_ui_roll_row(uint32_t f, const int *act, int na, int cols, bool gun) {
    bool have = f < g_tas.mv.n;
    igTableNextRow();
    tas_ui_row_colour(f, have);
    igTableNextColumn();
    igPushIDInt((int)f);
    char lbl[24];
    snprintf(lbl, sizeof lbl, "%u%s", f, f == g_tas.frame ? " >" : "");
    if (igSelectableEx(lbl, false, 0, (ImVec2){ 0, 0 })) {
        g_tas.rewinding = false;
        tas_seek(f <= g_tas.mv.n ? f : g_tas.mv.n, false);
    }
    igSetItemTooltip("Go to this frame (the board about to run it)");
    for (int i = 0; i < na && i + 1 < cols; i++) {
        igTableNextColumn();
        uint32_t bit = tas_ui_bit(act[i]);
        bool on = have && (g_tas.mv.in[f].held & bit);
        igPushIDInt(i);
        if (igSelectableEx(on ? s_tas_short[act[i]] : ".", on, 0, (ImVec2){ 26, 0 }) && have)
            tas_edit_held(f, bit);
        igPopID();
    }
    if (gun) {
        igTableNextColumn();
        if (have) tas_ui_gun_cell(f);
    }
    igPopID();
}

/* Keep the cursor's row in view while the board moves. */
static inline void tas_ui_roll_follow(void) {
    static uint32_t follow_at = UINT32_MAX;
    if (follow_at == g_tas.frame) return;
    follow_at = g_tas.frame;
    float h = igGetTextLineHeightWithSpacing();
    float y = h * (float)g_tas.frame;
    float top = igGetScrollY(), view = igGetWindowHeight() - 2 * h;
    if (y < top || y > top + view) igSetScrollY(y - view / 2 > 0 ? y - view / 2 : 0);
}

/* The movie, a frame a row and an input a column; a click toggles a cell. */
static inline void tas_ui_roll(void) {
    igSeparatorText("Inputs");
    tas_ui_roll_buttons();

    int act[GAME_INPUT_COUNT], na = 0;
    for (int a = 0; a < GAME_INPUT_COUNT; a++) if (tas_ui_bit(a)) act[na++] = a;
    bool gun = g_active_profile && g_active_profile->quirks.lightgun;
    int cols = 1 + na + (gun ? 1 : 0);
    if (cols > 64) cols = 64;
    ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY
                       | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit;
    if (!igBeginTableEx("##roll", cols, fl, (ImVec2){ 0, 260 }, 0)) return;
    igTableSetupScrollFreeze(1, 1);
    igTableSetupColumnEx("Frame", 0, 0, 0);
    for (int i = 0; i < na && i + 1 < cols; i++) igTableSetupColumnEx(s_tas_short[act[i]], 0, 0, 0);
    if (gun) igTableSetupColumnEx("P1 gun", 0, 0, 0);
    igTableHeadersRow();

    uint32_t rows = g_tas.mv.n + 1;   /* the last row: the frame recording will add */
    ImGuiListClipper clipper;
    memset(&clipper, 0, sizeof clipper);
    ImGuiListClipper *clip = &clipper;
    ImGuiListClipper_Begin(clip, (int)rows, -1.0f);
    while (ImGuiListClipper_Step(clip))
        for (int r = clip->DisplayStart; r < clip->DisplayEnd; r++) tas_ui_roll_row((uint32_t)r, act, na, cols, gun);
    ImGuiListClipper_End(clip);
    tas_ui_roll_follow();
    igEndTable();
}

static inline void tas_ui_window(emu_thread_ctx_t *ctx) {
    /* A movie started anywhere (the menu, the bridge, a file) shows the window. */
    static bool was_on;
    if (g_tas.on && !was_on) g_tas_ui.show = true;
    was_on = g_tas.on;
    if (!g_tas_ui.show) return;
    igSetNextWindowSize((ImVec2){ 560, 760 }, ImGuiCond_FirstUseEver);
    if (!igBegin("TAS", &g_tas_ui.show, 0)) { igEnd(); return; }
    tas_ui_lock(ctx);
    if (!g_tas.on) {
        igTextWrapped("No movie is running. TAS > Start a movie here takes the board as it is; "
                      "from power-on resets it first. TAS > Open movie... plays a saved one.");
        if (g_tas.anchor) igText("A movie of %u frames is kept (save it, or take it up again).", g_tas.mv.n);
    } else {
        igText("Frame %u of %u%s   rerecords %u", g_tas.frame, g_tas.mv.n,
               g_tas.seeking ? " (seeking)" : "", g_tas.rerecords);
        igText("Greenzone: %u states, %.0f MB (one every %u frames)", g_tas.gz_n,
               (double)tas_gz_bytes() / (1 << 20), g_tas.gz_every_now);
        if (g_tas.desync >= 0)
            igTextColored((ImVec4){ 1, 0.35f, 0.35f, 1 }, "DESYNC at frame %lld: the same inputs ended on another board",
                          (long long)g_tas.desync);
        igCheckbox("Recording (F12)", &g_tas.recording);
        igSetItemTooltip("Off: the board plays the movie's inputs; on: the pad's go in");
        tas_ui_transport(ctx);
        tas_ui_pad();
        tas_ui_aim();
        tas_ui_branches();
        tas_ui_roll();
    }
    if (g_tas.msg[0]) igTextDisabled("%s", g_tas.msg);
    tas_ui_unlock(ctx);
    if (g_tas_ui.error[0]) igTextColored((ImVec4){ 1, 0.4f, 0.4f, 1 }, "%s", g_tas_ui.error);
    igEnd();
}

#endif /* TAS_WINDOW_H */
