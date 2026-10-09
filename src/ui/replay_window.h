/*
 * replay_window.h -- the desktop build's side of core/replay.h: the Replays
 * menu, the file dialog that opens a replay, and the window that shows its
 * label, asks whether to play it, and then drives the playback.
 *
 * Everything that touches the board takes the emu mutex, as the MCP bridge's
 * "replay" command does: replay_play_start resets the board and loads a state.
 */
#ifndef REPLAY_WINDOW_H
#define REPLAY_WINDOW_H

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cimgui.h"
#include "ImGuiFileDialog.h"
#include "emu_thread.h"
#include "replay.h"

static struct {
    bool show;
    bool hide_result;     /* no spoilers: the label leaves out who won */
    char path[512];       /* the open replay's file */
    char error[400];
    int  seek_to;         /* the progress bar while it is dragged */
    bool dragging;
} g_replay_ui;

static inline void replay_ui_lock(emu_thread_ctx_t *ctx)   { if (ctx->thread_alive) emu_mutex_lock(&ctx->mutex); }
static inline void replay_ui_unlock(emu_thread_ctx_t *ctx) { if (ctx->thread_alive) emu_mutex_unlock(&ctx->mutex); }

/* Open a file and show its label; nothing plays until the player says so. */
static inline void replay_ui_open(emu_thread_ctx_t *ctx, const char *path) {
    replay_ui_lock(ctx);
    const char *err = replay_open_file(path);
    replay_ui_unlock(ctx);
    snprintf(g_replay_ui.path, sizeof g_replay_ui.path, "%s", path);
    snprintf(g_replay_ui.error, sizeof g_replay_ui.error, "%s", err ? err : "");
    g_replay_ui.show = true;
}

static inline void replay_ui_play(emu_thread_ctx_t *ctx) {
    replay_ui_lock(ctx);
    const char *err = replay_play_start(ctx);
    replay_ui_unlock(ctx);
    snprintf(g_replay_ui.error, sizeof g_replay_ui.error, "%s", err ? err : "");
    if (!err && !emu_is_running(ctx)) emu_run(ctx);
}

static inline void replay_ui_open_dialog(ImGuiFileDialog *fd) {
    static char dir[512];
    struct IGFD_FileDialog_Config cfg = IGFD_FileDialog_Config_Get();
    cfg.path = replay_dir(dir, sizeof dir) ? dir : ".";
    cfg.countSelectionMax = 1;
    cfg.flags = ImGuiFileDialogFlags_Modal;
    IGFD_OpenDialog(fd, "OpenReplay", "Open a replay", "Replays{.m2replay}", cfg);
}

static inline void replay_ui_menu(emu_thread_ctx_t *ctx, ImGuiFileDialog *fd) {
    if (!igBeginMenu("Replays")) return;
    bool rec = g_replay_rec.want;
    if (igMenuItemBoolPtr("Record online matches", NULL, &rec, true)) {
        g_replay_rec.want = rec;
        replay_settings_save();
    }
    if (igMenuItem("Play a replay...")) replay_ui_open_dialog(fd);
    igMenuItemBoolPtr("Replay window", NULL, &g_replay_ui.show, true);
    if (replay_playing() && igMenuItem("Stop the replay")) {
        replay_ui_lock(ctx);
        replay_play_stop();
        replay_ui_unlock(ctx);
    }
    igSeparator();
    char dir[512];
    if (replay_dir(dir, sizeof dir)) {
        igTextDisabled("saved in %s", dir);
        if (igMenuItem("Copy the folder's path")) igSetClipboardText(dir);
    }
    if (replay_recording())     igTextDisabled("recording %s vs %s", g_replay_rec.meta.p1, g_replay_rec.meta.p2);
    if (g_replay_rec.error[0])  igTextDisabled("%s", g_replay_rec.error);
    igEndMenu();
}

static inline void replay_ui_dialog(emu_thread_ctx_t *ctx, ImGuiFileDialog *fd) {
    ImVec2 min_size = {600, 400};
    ImVec2 max_size = {1280, 800};
    if (!IGFD_DisplayDialog(fd, "OpenReplay", 0, min_size, max_size)) return;
    if (IGFD_IsOk(fd)) {
        char *picked = IGFD_GetFilePathName(fd, IGFD_ResultMode_AddIfNoFileExt);
        if (picked) {
            replay_ui_open(ctx, picked);
            free(picked);
        }
    }
    IGFD_CloseDialog(fd);
}

/* The label: who, with whom, where, when, and (unless hidden) how it ended. */
static inline void replay_ui_label(const replay_label_t *l) {
    time_t t = (time_t)l->timestamp;
    struct tm *tm = localtime(&t);
    char when[64] = "";
    if (tm) strftime(when, sizeof when, "%Y-%m-%d %H:%M", tm);
    igText("%s (%s)  vs  %s (%s)", l->p1, l->c1[0] ? l->c1 : "?", l->p2, l->c2[0] ? l->c2 : "?");
    igText("Stage: %s", l->stage[0] ? l->stage : "?");
    igText("Played %s, %u:%02u long", when, l->seconds / 60, l->seconds % 60);
    igTextDisabled("recorded by %s%s; %s, %s", l->recorded_by,
                   strcmp(l->netcode, "ggpo") == 0 ? " over GGPO" : "", l->romset, l->profile);
    igCheckbox("Hide the result", &g_replay_ui.hide_result);
    if (g_replay_ui.hide_result) return;
    if (l->winner == 0 || l->winner == 1)
        igText("%s beat %s, %u-%u", l->winner_name, l->loser_name,
               l->winner == 0 ? l->r1 : l->r2, l->winner == 0 ? l->r2 : l->r1);
    else
        igText("No result: %s", l->ended[0] ? l->ended : "it ended early");
}

/* While it plays: the time line, and the buttons. */
static inline void replay_ui_controls(emu_thread_ctx_t *ctx) {
    replay_play_t *P = &g_replay_play;
    uint32_t frame = g_follow.frame;
    int last = P->frames ? (int)P->frames : 1;
    if (!g_replay_ui.dragging) g_replay_ui.seek_to = (int)frame;
    char lbl[48];
    snprintf(lbl, sizeof lbl, "%u:%02u / %u:%02u", frame / 3600, frame / 60 % 60,
             P->frames / 3600, P->frames / 60 % 60);
    igSliderIntEx("##time", &g_replay_ui.seek_to, 0, last, lbl, 0);
    g_replay_ui.dragging = igIsItemActive();
    if (igIsItemDeactivatedAfterEdit()) {
        replay_ui_lock(ctx);
        replay_play_seek(ctx, (uint32_t)g_replay_ui.seek_to);
        replay_ui_unlock(ctx);
    }
    bool running = emu_is_running(ctx);
    if (igButton(running ? "Pause" : "Resume")) {
        if (running) emu_stop(ctx); else emu_run(ctx);
    }
    igSameLine();
    if (igButton("Restart")) {
        replay_ui_lock(ctx);
        replay_play_seek(ctx, 0);
        replay_ui_unlock(ctx);
    }
    igSameLine();
    igCheckbox("Fast forward", &P->fast);
    igSameLine();
    if (igButton("Stop")) {
        replay_ui_lock(ctx);
        replay_play_stop();
        replay_ui_unlock(ctx);
    }
    if (g_follow.split) igTextColored((ImVec4){1, 0.4f, 0.4f, 1}, "The replay went off its record: %s", g_follow.why);
    else if (g_follow.ended) igTextDisabled("The end. Restart, seek back, or Stop to go back to your game.");
}

static inline void replay_ui_window(emu_thread_ctx_t *ctx, ImGuiFileDialog *fd) {
    if (!g_replay_ui.show) return;
    igSetNextWindowSize((ImVec2){460, 0}, ImGuiCond_FirstUseEver);
    if (!igBegin("Replay", &g_replay_ui.show, 0)) { igEnd(); return; }
    replay_play_t *P = &g_replay_play;
    if (!P->loaded) {
        igTextDisabled("No replay is open.");
        if (igButton("Open a replay...")) replay_ui_open_dialog(fd);
    } else {
        const char *slash = strrchr(g_replay_ui.path, '/');
        const char *bslash = strrchr(g_replay_ui.path, '\\');
        if (bslash > slash) slash = bslash;
        igTextDisabled("%s", slash ? slash + 1 : g_replay_ui.path);
        replay_label_t l;
        replay_label_get(P->json, &l);
        replay_ui_label(&l);
        igSeparator();
        if (P->on) {
            replay_ui_controls(ctx);
        } else if (P->unplayable[0]) {
            igTextColored((ImVec4){1, 0.6f, 0.3f, 1}, "Cannot play here: %s", P->unplayable);
        } else {
            igText("Play this replay? It takes over the board until you stop it.");
            if (igButton("Play")) replay_ui_play(ctx);
            igSameLine();
            if (igButton("Not now")) g_replay_ui.show = false;
        }
    }
    if (g_replay_ui.error[0]) igTextColored((ImVec4){1, 0.4f, 0.4f, 1}, "%s", g_replay_ui.error);
    igEnd();
}

#endif /* REPLAY_WINDOW_H */
