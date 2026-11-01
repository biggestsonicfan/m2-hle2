/*
 * game_fullscreen.h — fullscreen game mode (Pinboard #1939): the window covers
 * its monitor, shows the game and nothing else, and takes the keyboard.
 *
 * It is capture mode's picture (no menu bar, no debug windows) for someone
 * playing rather than recording: where --kiosk parks the window off the
 * desktop for OBS, this puts it in front of everything and makes it the
 * foreground window, so the pads and the keys reach the game without a click.
 *
 *   Alt+Enter or F11   in or out (F11 is TAS's own while its window is on)
 *   Esc                out, rather than quit
 *   File > Fullscreen  in; "Start in fullscreen" remembers it (display.cfg)
 *   --fullscreen       in from the start, for this run
 *
 * The cursor goes after two seconds without a move and comes back with one.
 * The prompts a player has to answer (a linked cabinet alone, play again, an
 * empty room) still draw over the game; main.c keeps those.
 *
 * Board-independent host shell code: nothing here knows what is emulated.
 */
#ifndef GAME_FULLSCREEN_H
#define GAME_FULLSCREEN_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cimgui.h"
#include "sokol_app.h"

#include "backup_ram.h"
#include "kiosk.h"
#include "log.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#define GAME_FULLSCREEN_CURSOR_IDLE 2.0f   /* seconds still before the cursor goes */

static struct {
    bool  on;              /* fullscreen game mode */
    bool  start_on;        /* "Start in fullscreen" (display.cfg) */
    bool  cursor_hidden;
    float still;           /* seconds since the mouse last moved */
} g_gfs;

static inline bool game_fullscreen_on(void) { return g_gfs.on; }

/* Make the window the foreground one. Windows refuses SetForegroundWindow to
 * a process that does not own the foreground, which is the case for one
 * started from a launcher; sharing the foreground thread's input state for
 * the call is the accepted way past that. Elsewhere the window manager gives
 * a new fullscreen window the focus itself. */
static inline void game_fullscreen__take_focus(void) {
#if defined(_WIN32)
    HWND h = (HWND)sapp_win32_get_hwnd();
    if (!h) return;
    const DWORD me = GetCurrentThreadId();
    HWND fg = GetForegroundWindow();
    const DWORD fg_thread = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    const bool attach = fg_thread && fg_thread != me && AttachThreadInput(fg_thread, me, TRUE);
    ShowWindow(h, SW_SHOW);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    SetActiveWindow(h);
    SetFocus(h);
    if (attach) AttachThreadInput(fg_thread, me, FALSE);
#endif
}

static inline void game_fullscreen__cursor(bool shown) {
    if (g_gfs.cursor_hidden == !shown) return;
    g_gfs.cursor_hidden = !shown;
    sapp_show_mouse(shown);
}

static inline void game_fullscreen_set(bool on) {
    if (on && kiosk_active()) {
        LOG_WARN("fullscreen: not in capture mode; leave it from the tray first");
        return;
    }
    if (g_gfs.on == on && sapp_is_fullscreen() == on) return;
    g_gfs.on = on;
    if (sapp_is_fullscreen() != on) sapp_toggle_fullscreen();
    g_gfs.still = 0.0f;
    game_fullscreen__cursor(true);
    if (on) game_fullscreen__take_focus();
    LOG_INFO("fullscreen %s", on ? "on: Esc, Alt+Enter or F11 to leave" : "off");
}

static inline void game_fullscreen_toggle(void) { game_fullscreen_set(!g_gfs.on); }

/* A move brings the cursor back. */
static inline void game_fullscreen_mouse(const sapp_event *ev) {
    if (ev->type != SAPP_EVENTTYPE_MOUSE_MOVE) return;
    g_gfs.still = 0.0f;
    if (g_gfs.on) game_fullscreen__cursor(true);
}

/* The keys; true when the key was fullscreen's. Called after TAS has had its
 * F11. */
static inline bool game_fullscreen_key(const sapp_event *ev) {
    if (ev->type != SAPP_EVENTTYPE_KEY_DOWN || ev->key_repeat || kiosk_active()) return false;
    const bool alt_enter = (ev->key_code == SAPP_KEYCODE_ENTER || ev->key_code == SAPP_KEYCODE_KP_ENTER) &&
                           (ev->modifiers & SAPP_MODIFIER_ALT);
    if (alt_enter || ev->key_code == SAPP_KEYCODE_F11) { game_fullscreen_toggle(); return true; }
    if (ev->key_code == SAPP_KEYCODE_ESCAPE && g_gfs.on) { game_fullscreen_set(false); return true; }
    return false;
}

/* Once a frame: the idle cursor, and the window leaving fullscreen some other
 * way (the window manager's own key) taking the mode with it. */
static inline void game_fullscreen_tick(float dt) {
    if (!g_gfs.on) return;
    if (!sapp_is_fullscreen()) { game_fullscreen_set(false); return; }
    g_gfs.still += (dt > 0.0f && dt < 1.0f) ? dt : 0.0f;
    if (g_gfs.still >= GAME_FULLSCREEN_CURSOR_IDLE) game_fullscreen__cursor(false);
}

/* <user dir>/display.cfg: "fullscreen=1". */
static inline bool game_fullscreen__cfg_path(char *out, size_t cap) {
    if (!backup_ram_user_dir(out, cap)) return false;
    size_t n = strlen(out);
    snprintf(out + n, cap - n, BACKUP_RAM_SEP "display.cfg");
    return true;
}

static inline void game_fullscreen_settings_load(void) {
    char path[512], line[128];
    if (!game_fullscreen__cfg_path(path, sizeof path)) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "fullscreen=", 11)) g_gfs.start_on = line[11] == '1';
    fclose(f);
}

static inline void game_fullscreen_settings_save(void) {
    char path[512], dir[512];
    if (!game_fullscreen__cfg_path(path, sizeof path) || !backup_ram_user_dir(dir, sizeof dir)) return;
    backup_ram_mkdir(dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "fullscreen=%d\n", g_gfs.start_on ? 1 : 0);
    fclose(f);
}

/* File menu: go fullscreen now, and whether to start that way. */
static inline void game_fullscreen_menu(void) {
    if (igMenuItemEx("Fullscreen", "Alt+Enter", false, !kiosk_active())) game_fullscreen_set(true);
    if (igMenuItemBoolPtr("Start in fullscreen", NULL, &g_gfs.start_on, true))
        game_fullscreen_settings_save();
}

#endif /* GAME_FULLSCREEN_H */
