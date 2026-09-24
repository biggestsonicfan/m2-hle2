/*
 * ps3ui_shell.h -- the PS3 port's offline shell: title, menus, pause.
 *
 * The PS3 port wraps the arcade board in a menu system (its flow machine,
 * Flow_Update 0xab7c8; see tools/ps3ui/README.md). This is that shell over this
 * emulator's board, so a player never needs a keyboard:
 *
 *   TITLE        the board's attract, which shows its own PUSH START BUTTON --
 *                the prompt the PS3 draws, in the same arcade lettering, since
 *                the PS3's board is not running yet at its title
 *   MAIN MENU    Arcade / Offline Versus / Online Battle / Help & Options
 *   ARCADE       difficulty, rounds, time, attack, barriers, game type
 *   VERSUS       the same without difficulty; needs a second pad (START on it)
 *   ONLINE       ps3ui_app.h, the online lobby
 *   GAME         the board, alone; SELECT opens the pause menu
 *   PAUSE        Resume Game / Help & Options / Exit Game
 *   OPTIONS      Controls (the PS3's six button presets) / Settings (volume) /
 *                Credits
 *   CREDITS      the open-source code and fonts the builds are made of: licence,
 *                copyright and where each comes from
 *
 * Left out on purpose: the logo reel and the title logo (Sega's artwork; the
 * board shows its own), Scoreboards and Save Data (the PS3's network and
 * storage), How to Play, the PS3's own Credits and the Command List (Sega's
 * text).
 *
 * The frontend feeds both pads once a frame, asks ps3ui_shell_view() what to
 * show, gives the game the pad only when ps3ui_shell_game_pad() says so, and
 * does not step the board while ps3ui_shell_board_paused().
 */
#ifndef PS3UI_SHELL_H
#define PS3UI_SHELL_H

#include "ps3ui_app.h"

/* What the shell needs from the host. */
typedef struct {
    void (*reset_board)(void *user);                               /* Exit Game: back to attract */
    void (*apply_settings)(void *user, const uint8_t s[8], int versus); /* ARCADE / VERSUS, at start */
    void (*set_volume)(void *user, int music, int se);             /* 0..20 each */
    void *user;
} ps3ui_host_t;

typedef enum {
    PS3UI_SH_TITLE, PS3UI_SH_MAIN, PS3UI_SH_ARCADE, PS3UI_SH_VERSUS, PS3UI_SH_OPTIONS,
    PS3UI_SH_CONTROLS, PS3UI_SH_SETTINGS, PS3UI_SH_CREDITS, PS3UI_SH_ONLINE, PS3UI_SH_GAME, PS3UI_SH_PAUSE,
} ps3ui_sh_screen_t;

typedef enum {
    PS3UI_VIEW_GAME,        /* the board's picture only */
    PS3UI_VIEW_OVERLAY,     /* the board's picture with the shell over it */
    PS3UI_VIEW_FULL,        /* the shell's own 16:9 frame */
} ps3ui_view_t;

/* Button codes for the Controls screen (the i960's input codes, in order). */
enum { PS3UI_BTN_UNUSED, PS3UI_BTN_P, PS3UI_BTN_K, PS3UI_BTN_G, PS3UI_BTN_PG, PS3UI_BTN_PK, PS3UI_BTN_KG,
       PS3UI_BTN_PKG, PS3UI_BTN_CODES };
/* The rows of the Controls screen, as the PS3 lists the pad's buttons. */
enum { PS3UI_KEY_TRIANGLE, PS3UI_KEY_CIRCLE, PS3UI_KEY_SQUARE, PS3UI_KEY_CROSS, PS3UI_KEY_L1, PS3UI_KEY_L2,
       PS3UI_KEY_R1, PS3UI_KEY_R2, PS3UI_KEYS };

#define PS3UI_TYPES 6

/* The PS3's presets (table 0x375d50): Standard, then Arcade stick 1..5. */
static const uint8_t ps3ui_presets[PS3UI_TYPES][PS3UI_KEYS] = {
    { PS3UI_BTN_P, PS3UI_BTN_K, PS3UI_BTN_G, PS3UI_BTN_P, PS3UI_BTN_PG, PS3UI_BTN_PKG, PS3UI_BTN_PK, PS3UI_BTN_KG },
    { PS3UI_BTN_K, 0, PS3UI_BTN_P, PS3UI_BTN_G, 0, 0, 0, 0 },
    { 0, PS3UI_BTN_P, PS3UI_BTN_K, PS3UI_BTN_G, 0, 0, 0, 0 },
    { 0, 0, PS3UI_BTN_K, 0, PS3UI_BTN_P, PS3UI_BTN_G, 0, 0 },
    { 0, 0, PS3UI_BTN_K, 0, 0, PS3UI_BTN_P, 0, PS3UI_BTN_G },
    { PS3UI_BTN_P, PS3UI_BTN_K, PS3UI_BTN_G, 0, 0, 0, 0, 0 },
};

/* ARCADE / VERSUS rows (the PS3's value lists and defaults). */
enum { PS3UI_SET_DIFFICULTY, PS3UI_SET_ROUNDS, PS3UI_SET_TIME, PS3UI_SET_ATTACK, PS3UI_SET_BARRIERS,
       PS3UI_SET_TYPE, PS3UI_SETS };
static const char *const ps3ui_set_label[PS3UI_SETS] = { "Difficulty", "Round count", "Time limit",
                                                         "Attack power", "Number of barriers", "Game type" };
static const char *const ps3ui_set_values[PS3UI_SETS][10] = {
    { "Easy", "Normal", "Hard", "Hardest" },
    { "2", "3", "4", "5" },
    { "10", "30", "60", "99" },
    { "-1", "Normal", "+1", "+2", "+3" },
    { "1", "2", "3", "4", "5", "6", "7", "8", "9", "10" },
    { "Type A", "Type B", "Type C", "Type D" },
};
static const uint8_t ps3ui_set_max[PS3UI_SETS] = { 3, 3, 3, 4, 9, 3 };
static const uint8_t ps3ui_set_arcade_def[PS3UI_SETS] = { 1, 0, 1, 1, 4, 0 };
static const uint8_t ps3ui_set_versus_def[PS3UI_SETS] = { 1, 1, 1, 1, 4, 0 };

static const char *const ps3ui_btn_names[PS3UI_BTN_CODES] = { "Unused", "P", "K", "G", "P+G", "P+K", "K+G", "P+K+G" };
static const char *const ps3ui_type_names[PS3UI_TYPES] = { "Standard", "Arcade stick 1", "Arcade stick 2",
                                                           "Arcade stick 3", "Arcade stick 4", "Arcade stick 5" };

/* CREDITS: every third-party part of a build -- vendor/ (the submodules),
 * src/libretro/libretro.h, licenses/ (the fonts), and the libraries the SDL3
 * and Linux builds load. Copyright lines as the licence files give them. */
typedef struct {
    const char *name, *licence, *copyright, *url;
} ps3ui_credit_t;

static const ps3ui_credit_t ps3ui_credits[] = {
    { "Sokol", "zlib", "Copyright (c) 2018 Andre Weissflog", "https://github.com/floooh/sokol" },
    { "Dear ImGui", "MIT", "Copyright (c) 2014-2026 Omar Cornut", "https://github.com/ocornut/imgui" },
    { "Dear Bindings", "MIT", "Copyright (c) 2021-2022 Ben Carter", "https://github.com/dearimgui/dear_bindings" },
    { "imgui_club", "MIT", "Copyright (c) 2017-2024 Omar Cornut", "https://github.com/ocornut/imgui_club" },
    { "ImGuiFileDialog", "MIT", "Copyright (c) 2018-2025 Stephane Cuillerdier (Aiekick)",
      "https://github.com/aiekick/ImGuiFileDialog" },
    { "miniz", "MIT", "Copyright 2013-2014 RAD Game Tools and Valve Software; 2010-2014 Rich Geldreich",
      "https://github.com/richgel999/miniz" },
    { "stb_truetype", "MIT / Public Domain", "Copyright (c) 2017 Sean Barrett", "https://github.com/nothings/stb" },
    { "libretro API", "MIT", "Copyright (C) 2010-2024 The RetroArch team", "https://github.com/libretro/RetroArch" },
    { "SDL 3", "zlib", "Copyright (C) 1997-2025 Sam Lantinga", "https://github.com/libsdl-org/SDL" },
    { "OpenSSL", "Apache 2.0", "Copyright (c) 1998-2025 The OpenSSL Project Authors",
      "https://github.com/openssl/openssl" },
    { "Anybody (font)", "SIL OFL 1.1", "Copyright 2020 The Anybody Project Authors",
      "https://github.com/Etcetera-Type-Co/Anybody" },
    { "M PLUS 1p (font)", "SIL OFL 1.1", "Copyright 2016 The M+ Project Authors",
      "https://github.com/coz-m/MPLUS_FONTS" },
    { "M PLUS 1 Code (font)", "SIL OFL 1.1", "Copyright 2021 The M+ FONTS Project Authors",
      "https://github.com/coz-m/MPLUS_FONTS" },
};
#define PS3UI_CREDITS ((int)(sizeof ps3ui_credits / sizeof ps3ui_credits[0]))
#define PS3UI_CREDITS_ROWS 6    /* choice_win_06 */

enum { PS3UI_SHDLG_NONE, PS3UI_SHDLG_EXIT_GAME };

typedef struct {
    ps3ui_host_t host;
    ps3ui_app_t *online;
    ps3ui_sh_screen_t scr;
    int cursor, main_cursor, options_cursor, pause_cursor;
    int options_from_pause;
    uint8_t arcade[PS3UI_SETS], versus[PS3UI_SETS];
    int ctl_type;
    uint8_t ctl[PS3UI_TYPES][PS3UI_KEYS];
    int music, se;
    int credits_top;            /* CREDITS: the first row on screen */
    int two_p;                  /* VERSUS: a second pad has pressed START */
    int resume_wait;            /* PAUSE -> GAME takes 10 frames, as the PS3's */
    int netplay;                /* a session is running: no pause */
    int no_controls;            /* the host maps buttons itself (the web page): no Controls row */

    uint32_t held, pressed, held2, pressed2;
    uint32_t repeat_held;
    int repeat_frames;

    ps3ui_win_t main, msg, bar;
    ps3ui_dialog_t dlg;
    int dlg_kind;
    float cursor_t, t;
    uint32_t frame;
} ps3ui_shell_t;

static ps3ui_shell_t g_ps3ui_shell;

static void ps3ui_shell_init(ps3ui_shell_t *sh, ps3ui_host_t host, ps3ui_app_t *online)
{
    memset(sh, 0, sizeof *sh);
    sh->host = host;
    sh->online = online;
    sh->scr = PS3UI_SH_TITLE;
    memcpy(sh->arcade, ps3ui_set_arcade_def, PS3UI_SETS);
    memcpy(sh->versus, ps3ui_set_versus_def, PS3UI_SETS);
    memcpy(sh->ctl, ps3ui_presets, sizeof sh->ctl);
    sh->music = sh->se = 5;
    ps3ui_win_open(&sh->bar, &ps3ui_n_cmn_base, "sousa_win");
}

/* The pad's buttons as the current Controls type sets them. */
static const uint8_t *ps3ui_shell_buttons(const ps3ui_shell_t *sh) { return sh->ctl[sh->ctl_type]; }

static ps3ui_view_t ps3ui_shell_view(const ps3ui_shell_t *sh)
{
    switch (sh->scr) {
    case PS3UI_SH_PAUSE:
        return PS3UI_VIEW_OVERLAY;
    case PS3UI_SH_TITLE:
    case PS3UI_SH_GAME:
        return PS3UI_VIEW_GAME;
    case PS3UI_SH_ONLINE:
        return ps3ui_app_visible(sh->online) ? PS3UI_VIEW_FULL : PS3UI_VIEW_GAME;
    case PS3UI_SH_OPTIONS:
    case PS3UI_SH_CONTROLS:
    case PS3UI_SH_SETTINGS:
    case PS3UI_SH_CREDITS:
        return sh->options_from_pause ? PS3UI_VIEW_OVERLAY : PS3UI_VIEW_FULL;
    default:
        return PS3UI_VIEW_FULL;
    }
}

/* The game has the pad only when nothing of the shell is on screen -- and not
 * at the title, where START is the shell's. */
static int ps3ui_shell_game_pad(const ps3ui_shell_t *sh)
{
    return sh->scr != PS3UI_SH_TITLE && ps3ui_shell_view(sh) == PS3UI_VIEW_GAME;
}

/* The board is held still under the menus, as the PS3 suspends it -- but never
 * while netplay runs it (the barrier and the match need it stepped). */
static int ps3ui_shell_board_paused(const ps3ui_shell_t *sh)
{
    if (sh->netplay || sh->scr == PS3UI_SH_ONLINE)
        return 0;
    return sh->scr != PS3UI_SH_GAME && sh->scr != PS3UI_SH_TITLE;
}

static void ps3ui_shell_go(ps3ui_shell_t *sh, ps3ui_sh_screen_t s)
{
    if (sh->scr == PS3UI_SH_MAIN) sh->main_cursor = sh->cursor;
    if (sh->scr == PS3UI_SH_OPTIONS) sh->options_cursor = sh->cursor;
    sh->scr = s;
    sh->cursor = s == PS3UI_SH_MAIN ? sh->main_cursor : s == PS3UI_SH_OPTIONS ? sh->options_cursor : 0;
    ps3ui_win_close(&sh->msg);
    if (s == PS3UI_SH_VERSUS)
        sh->two_p = 0;
    if (s == PS3UI_SH_CREDITS)
        sh->credits_top = 0;
}

static void ps3ui_shell_pad(ps3ui_shell_t *sh, uint32_t held, uint32_t held2)
{
    sh->pressed = held & ~sh->held;
    sh->held = held;
    sh->pressed2 = held2 & ~sh->held2;
    sh->held2 = held2;
    uint32_t dirs = held & (PS3UI_PAD_UP | PS3UI_PAD_DOWN | PS3UI_PAD_LEFT | PS3UI_PAD_RIGHT);
    if (dirs && dirs == sh->repeat_held) {
        if (++sh->repeat_frames >= PS3UI_REPEAT_DELAY
            && (sh->repeat_frames - PS3UI_REPEAT_DELAY) % PS3UI_REPEAT_RATE == 0)
            sh->pressed |= dirs;
    } else {
        sh->repeat_held = dirs;
        sh->repeat_frames = 0;
    }
}

static int ps3ui_sh_hit(const ps3ui_shell_t *sh, uint32_t b) { return (sh->pressed & b) != 0; }

/* Vertical cursor over n rows, clamped (every shell menu clamps). */
static void ps3ui_sh_move(ps3ui_shell_t *sh, int n)
{
    if (ps3ui_sh_hit(sh, PS3UI_PAD_UP) && sh->cursor > 0) sh->cursor--;
    if (ps3ui_sh_hit(sh, PS3UI_PAD_DOWN) && sh->cursor < n - 1) sh->cursor++;
}

/* ←/→ on a value, clamped, as the PS3's value rows. */
static int ps3ui_sh_value(ps3ui_shell_t *sh, int v, int max)
{
    if (ps3ui_sh_hit(sh, PS3UI_PAD_LEFT) && v > 0) v--;
    if (ps3ui_sh_hit(sh, PS3UI_PAD_RIGHT) && v < max) v++;
    return v;
}

/* The 8 bytes NetGameMode_Set takes: difficulty, !trial, 0, time, rounds,
 * attack, barriers, type. */
static void ps3ui_shell_settings_bytes(const uint8_t v[PS3UI_SETS], uint8_t s[8])
{
    s[0] = v[PS3UI_SET_DIFFICULTY];
    s[1] = 1;
    s[2] = 0;
    s[3] = v[PS3UI_SET_TIME];
    s[4] = v[PS3UI_SET_ROUNDS];
    s[5] = v[PS3UI_SET_ATTACK];
    s[6] = v[PS3UI_SET_BARRIERS];
    s[7] = v[PS3UI_SET_TYPE];
}

static void ps3ui_shell_start_game(ps3ui_shell_t *sh, int versus)
{
    uint8_t s[8];
    ps3ui_shell_settings_bytes(versus ? sh->versus : sh->arcade, s);
    if (sh->host.apply_settings)
        sh->host.apply_settings(sh->host.user, s, versus);
    ps3ui_shell_go(sh, PS3UI_SH_GAME);
}

/* ---- update --------------------------------------------------------------------- */

static const char *const ps3ui_main_rows[4] = { "Arcade", "Offline Versus", "Online Battle", "Help & Options" };
static const char *const ps3ui_main_explain[4] = {
    "Fight your way through every opponent on your own.",
    "Two players, two controllers, head to head.",
    "Play other people over the internet on RPCN.",
    "Change the controls and the volume, and see the credits.",
};
static const char *const ps3ui_option_rows[3] = { "Controls", "Settings", "Credits" };
static const char *const ps3ui_option_rows_nc[2] = { "Settings", "Credits" };
static const char *const ps3ui_pause_rows[4] = { "Resume Game", "Help & Options", "", "Exit Game" };

static void ps3ui_sh_update_settings_menu(ps3ui_shell_t *sh, int versus)
{
    uint8_t *v = versus ? sh->versus : sh->arcade;
    int first = versus ? 1 : 0;                  /* VERSUS has no Difficulty row */
    int rows = PS3UI_SETS - first;
    if (versus && !sh->two_p) {
        if (sh->pressed2 & (PS3UI_PAD_START | PS3UI_PAD_CROSS))
            sh->two_p = 1;                       /* the second pad claims 2P */
        if (ps3ui_sh_hit(sh, PS3UI_PAD_CIRCLE))
            ps3ui_shell_go(sh, PS3UI_SH_MAIN);
        return;
    }
    ps3ui_sh_move(sh, rows);
    int r = sh->cursor + first;
    v[r] = (uint8_t)ps3ui_sh_value(sh, v[r], ps3ui_set_max[r]);
    if (ps3ui_sh_hit(sh, PS3UI_PAD_CIRCLE))
        ps3ui_shell_go(sh, PS3UI_SH_MAIN);
    else if (ps3ui_sh_hit(sh, PS3UI_PAD_CROSS))
        ps3ui_shell_start_game(sh, versus);      /* x starts from any row */
}

static void ps3ui_sh_update_controls(ps3ui_shell_t *sh)
{
    ps3ui_sh_move(sh, 1 + PS3UI_KEYS);
    if (sh->cursor == 0) {
        sh->ctl_type = ps3ui_sh_value(sh, sh->ctl_type, PS3UI_TYPES - 1);
    } else {
        uint8_t *b = &sh->ctl[sh->ctl_type][sh->cursor - 1];
        /* the button rows wrap */
        if (ps3ui_sh_hit(sh, PS3UI_PAD_LEFT)) *b = (uint8_t)((*b + PS3UI_BTN_CODES - 1) % PS3UI_BTN_CODES);
        if (ps3ui_sh_hit(sh, PS3UI_PAD_RIGHT)) *b = (uint8_t)((*b + 1) % PS3UI_BTN_CODES);
    }
    if (ps3ui_sh_hit(sh, PS3UI_PAD_CROSS))       /* x saves and leaves; o does nothing */
        ps3ui_shell_go(sh, PS3UI_SH_OPTIONS);
}

static void ps3ui_sh_update_volume(ps3ui_shell_t *sh)
{
    ps3ui_sh_move(sh, 2);
    int *v = sh->cursor == 0 ? &sh->music : &sh->se;
    int was = *v;
    *v = ps3ui_sh_value(sh, *v, 20);
    if (*v != was && sh->host.set_volume)
        sh->host.set_volume(sh->host.user, sh->music, sh->se);   /* live, as the PS3's */
    if (ps3ui_sh_hit(sh, PS3UI_PAD_CROSS))
        ps3ui_shell_go(sh, PS3UI_SH_OPTIONS);
}

/* CREDITS: a list longer than its window, scrolled to keep the cursor on it. */
static void ps3ui_sh_update_credits(ps3ui_shell_t *sh)
{
    ps3ui_sh_move(sh, PS3UI_CREDITS);
    if (sh->cursor < sh->credits_top)
        sh->credits_top = sh->cursor;
    if (sh->cursor >= sh->credits_top + PS3UI_CREDITS_ROWS)
        sh->credits_top = sh->cursor - PS3UI_CREDITS_ROWS + 1;
    if (ps3ui_sh_hit(sh, PS3UI_PAD_CIRCLE | PS3UI_PAD_CROSS))
        ps3ui_shell_go(sh, PS3UI_SH_OPTIONS);
}

static void ps3ui_shell_frame(ps3ui_shell_t *sh, uint32_t pad, uint32_t pad2, int netplay_session)
{
    sh->frame++;
    sh->netplay = netplay_session;
    ps3ui_shell_pad(sh, pad, pad2);
    sh->t += 1.0f;
    sh->cursor_t = sh->cursor_t + 1.0f >= 180.0f ? 0.0f : sh->cursor_t + 1.0f;

    if (ps3ui_dialog_showing(&sh->dlg)) {
        int r = ps3ui_dialog_update(&sh->dlg, sh->pressed);
        if (r == 0 && sh->dlg_kind == PS3UI_SHDLG_EXIT_GAME) {
            if (sh->host.reset_board)
                sh->host.reset_board(sh->host.user);
            ps3ui_shell_go(sh, PS3UI_SH_MAIN);
        }
        goto windows;
    }

    switch (sh->scr) {
    case PS3UI_SH_TITLE:
        if (ps3ui_sh_hit(sh, PS3UI_PAD_START | PS3UI_PAD_CROSS))
            ps3ui_shell_go(sh, PS3UI_SH_MAIN);
        break;
    case PS3UI_SH_MAIN:
        ps3ui_sh_move(sh, 4);                    /* no wrap; o does nothing here */
        if (ps3ui_sh_hit(sh, PS3UI_PAD_CROSS)) {
            if (sh->cursor == 0) ps3ui_shell_go(sh, PS3UI_SH_ARCADE);
            if (sh->cursor == 1) ps3ui_shell_go(sh, PS3UI_SH_VERSUS);
            if (sh->cursor == 2) {
                ps3ui_shell_go(sh, PS3UI_SH_ONLINE);
                ps3ui_app_open(sh->online);
            }
            if (sh->cursor == 3) {
                sh->options_from_pause = 0;
                ps3ui_shell_go(sh, PS3UI_SH_OPTIONS);
            }
        }
        break;
    case PS3UI_SH_ARCADE: ps3ui_sh_update_settings_menu(sh, 0); break;
    case PS3UI_SH_VERSUS: ps3ui_sh_update_settings_menu(sh, 1); break;
    case PS3UI_SH_OPTIONS: {
        static const ps3ui_sh_screen_t to[3] = { PS3UI_SH_CONTROLS, PS3UI_SH_SETTINGS, PS3UI_SH_CREDITS };
        int first = sh->no_controls ? 1 : 0;     /* the web page has no Controls row */
        ps3ui_sh_move(sh, 3 - first);
        if (ps3ui_sh_hit(sh, PS3UI_PAD_CIRCLE))
            ps3ui_shell_go(sh, sh->options_from_pause ? PS3UI_SH_PAUSE : PS3UI_SH_MAIN);
        else if (ps3ui_sh_hit(sh, PS3UI_PAD_CROSS))
            ps3ui_shell_go(sh, to[sh->cursor + first]);
        break;
    }
    case PS3UI_SH_CONTROLS: ps3ui_sh_update_controls(sh); break;
    case PS3UI_SH_SETTINGS: ps3ui_sh_update_volume(sh); break;
    case PS3UI_SH_CREDITS: ps3ui_sh_update_credits(sh); break;
    case PS3UI_SH_ONLINE:
        if (!sh->online->open)
            ps3ui_shell_go(sh, PS3UI_SH_MAIN);   /* the lobby was left */
        break;
    case PS3UI_SH_GAME:
        if (!sh->netplay && ps3ui_sh_hit(sh, PS3UI_PAD_SELECT)) {
            ps3ui_shell_go(sh, PS3UI_SH_PAUSE);  /* SELECT pauses; START is the arcade's */
            sh->pause_cursor = 0;
        }
        break;
    case PS3UI_SH_PAUSE:
        if (sh->resume_wait) {
            if (--sh->resume_wait == 0)
                ps3ui_shell_go(sh, PS3UI_SH_GAME);
            break;
        }
        sh->cursor = sh->pause_cursor;
        ps3ui_sh_move(sh, 4);
        if (sh->cursor == 2)                     /* the blank row above Exit Game is skipped */
            sh->cursor = ps3ui_sh_hit(sh, PS3UI_PAD_UP) ? 1 : 3;
        sh->pause_cursor = sh->cursor;
        if (ps3ui_sh_hit(sh, PS3UI_PAD_SELECT)) {
            sh->pause_cursor = 0;                /* SELECT again: Resume */
            sh->resume_wait = 10;
            ps3ui_win_close(&sh->main);
        } else if (ps3ui_sh_hit(sh, PS3UI_PAD_CROSS)) {
            if (sh->cursor == 0) {
                sh->resume_wait = 10;
                ps3ui_win_close(&sh->main);
            } else if (sh->cursor == 1) {
                sh->options_from_pause = 1;
                ps3ui_shell_go(sh, PS3UI_SH_OPTIONS);
            } else {
                sh->dlg_kind = PS3UI_SHDLG_EXIT_GAME;
                ps3ui_dialog_ask(&sh->dlg, "Do you want to end the game? Game progress will be lost.", 1);
            }
        }
        break;
    }

windows:
    /* the windows each screen shows */
    switch (sh->scr) {
    case PS3UI_SH_MAIN: ps3ui_win_open(&sh->main, &ps3ui_n_cmn_base, "choice_win_04");
        ps3ui_win_open(&sh->msg, &ps3ui_n_cmn_base, "cmn_win_b_01"); break;
    case PS3UI_SH_ARCADE: ps3ui_win_open(&sh->main, &ps3ui_n_cmn_base, "choice_win_06");
        ps3ui_win_open(&sh->msg, &ps3ui_n_cmn_base, "cmn_win_b_01"); break;
    case PS3UI_SH_VERSUS: ps3ui_win_open(&sh->main, &ps3ui_n_cmn_base, "choice_win_05");
        ps3ui_win_open(&sh->msg, &ps3ui_n_cmn_base, "cmn_win_b_01"); break;
    case PS3UI_SH_OPTIONS:
        ps3ui_win_open(&sh->main, &ps3ui_n_cmn_base, sh->no_controls ? "choice_win_02" : "choice_win_03");
        break;
    case PS3UI_SH_CREDITS: ps3ui_win_open(&sh->main, &ps3ui_n_cmn_base, "choice_win_06");
        ps3ui_win_open(&sh->msg, &ps3ui_n_cmn_base, "cmn_win_b_01"); break;
    case PS3UI_SH_CONTROLS: ps3ui_win_open(&sh->main, &ps3ui_n_cmn_screen, "controls_win_ps3"); break;
    case PS3UI_SH_SETTINGS: ps3ui_win_open(&sh->main, &ps3ui_n_cmn_base, "choice_win_02"); break;
    case PS3UI_SH_PAUSE: if (!sh->resume_wait) ps3ui_win_open(&sh->main, &ps3ui_n_cmn_base, "pause_win_s"); break;
    default: ps3ui_win_close(&sh->main); ps3ui_win_close(&sh->msg); break;
    }
    ps3ui_win_tick(&sh->main);
    ps3ui_win_tick(&sh->msg);
    ps3ui_win_tick(&sh->bar);
}

/* ---- draw ------------------------------------------------------------------------- */

/* A value list (ARCADE / VERSUS / Settings): label left, value right, the
 * value green when it is the default, arrows around the current row's. */
static void ps3ui_sh_draw_values(ps3ui_canvas_t *cv, ps3ui_shell_t *sh, const char *title, const char *const *labels,
                                 const char *const *values, const int *is_default, int n, int dim)
{
    ps3ui_slots_t s = { 0 };
    ps3ui_win_draw(cv, &sh->main, 0, 0, &s);
    float lx, ly, rx, ry, ex, ey, tx, ty;
    if (!ps3ui_slot_xy(&s, "p_txt_01_lt", 0, 0, &lx, &ly) || !ps3ui_slot_xy(&s, "p_txt_03_rt", 1, 0, &rx, &ry))
        return;
    float alpha = ps3ui_slot_alpha(&s, "p_txt_01_lt") * (dim ? 0.5f : 1.0f);
    if (!dim && sh->main.state == PS3UI_WIN_IDLE && ps3ui_slot_xy(&s, "p_win_edg_lt", 0, 0, &ex, &ey))
        ps3ui_draw_cursor(cv, &ps3ui_n_cmn_base, "cursor_cmn01_46", ex, ey + 54.0f * (float)sh->cursor, sh->cursor_t);
    ps3ui_text_style_t st = ps3ui_style_text(37.0f);
    for (int i = 0; i < n; i++) {
        float y = ly + 54.0f * (float)i;
        ps3ui_text_left(cv, &st, lx, y, labels[i], alpha);
        ps3ui_text_style_t vs = st;
        vs.rgb = is_default[i] ? 0x00F040 : 0xFFFFFF;
        ps3ui_text_right(cv, &vs, rx - (i == sh->cursor && !dim ? 34.0f : 0.0f), y, values[i], alpha);
        if (i == sh->cursor && !dim) {
            float vw = ps3ui_text_width(&vs, values[i]);
            ps3ui_text_right(cv, &st, rx, y, ">", alpha);
            ps3ui_text_right(cv, &st, rx - 34.0f - vw - 12.0f, y, "<", alpha);
        }
    }
    if (ps3ui_slot_xy(&s, "head_tit_ct", 0.5f, 0, &tx, &ty)) {
        ps3ui_text_style_t ts = ps3ui_style_title(40.0f);
        ps3ui_text(cv, &ts, tx - ps3ui_text_width(&ts, title) * 0.5f, ty + 47.0f, title,
                   ps3ui_slot_alpha(&s, "head_tit_ct"));
    }
}

static void ps3ui_sh_draw_list(ps3ui_canvas_t *cv, ps3ui_shell_t *sh, const char *title, const char *const *rows,
                               int n, int cursor)
{
    ps3ui_slots_t s = { 0 };
    ps3ui_win_draw(cv, &sh->main, 0, 0, &s);
    float lx, ly, rx, ry, ex, ey, tx, ty;
    if (!ps3ui_slot_xy(&s, "p_txt_01_lt", 0, 0, &lx, &ly) || !ps3ui_slot_xy(&s, "p_txt_02_rb", 1, 1, &rx, &ry))
        return;
    float alpha = ps3ui_slot_alpha(&s, "p_txt_01_lt");
    if (sh->main.state == PS3UI_WIN_IDLE && ps3ui_slot_xy(&s, "p_win_edg_lt", 0, 0, &ex, &ey))
        ps3ui_draw_cursor(cv, &ps3ui_n_cmn_base, "cursor_cmn01_46", ex, ey + 54.0f * (float)cursor, sh->cursor_t);
    ps3ui_text_style_t st = ps3ui_style_text(37.0f);
    for (int i = 0; i < n; i++)
        if (rows[i][0])
            ps3ui_text_centre(cv, &st, (lx + rx) * 0.5f, ly + 54.0f * (float)i, rows[i], alpha);
    if (title && ps3ui_slot_xy(&s, "head_tit_ct", 0.5f, 0, &tx, &ty)) {
        ps3ui_text_style_t ts = ps3ui_style_title(40.0f);
        ps3ui_text(cv, &ts, tx - ps3ui_text_width(&ts, title) * 0.5f, ty + 47.0f, title,
                   ps3ui_slot_alpha(&s, "head_tit_ct"));
    }
}

static void ps3ui_sh_draw_explain(ps3ui_canvas_t *cv, ps3ui_shell_t *sh, const char *text)
{
    ps3ui_slots_t s = { 0 };
    ps3ui_win_draw(cv, &sh->msg, 0, 0, &s);
    float x0, y0, x1, y1;
    if (!ps3ui_slot_xy(&s, "p_txt_01_lt", 0, 0, &x0, &y0) || !ps3ui_slot_xy(&s, "p_txt_02_rb", 1, 1, &x1, &y1))
        return;
    ps3ui_text_style_t st = ps3ui_style_text(37.0f);
    ps3ui_text_box(cv, &st, x0, y0, x1, y1, text, ps3ui_slot_alpha(&s, "p_txt_01_lt"), 0);
}

static void ps3ui_sh_draw_controls(ps3ui_canvas_t *cv, ps3ui_shell_t *sh)
{
    ps3ui_slots_t s = { 0 };
    ps3ui_win_draw(cv, &sh->main, 0, 0, &s);
    float x, y, lx, ly, rx, ry, ex, ey;
    ps3ui_text_style_t st = ps3ui_style_text(37.0f);
    float alpha = ps3ui_slot_alpha(&s, "p_txt_01_lt");
    if (ps3ui_slot_xy(&s, "head_tit_ct", 0.5f, 0, &x, &y)) {
        ps3ui_text_style_t ts = ps3ui_style_title(40.0f);
        ps3ui_text(cv, &ts, x - ps3ui_text_width(&ts, "CONTROLS") * 0.5f, y + 47.0f, "CONTROLS",
                   ps3ui_slot_alpha(&s, "head_tit_ct"));
    }
    if (sh->main.state == PS3UI_WIN_IDLE) {
        const char *edge = sh->cursor == 0 ? "p_ctgry_edg_lt" : "p_win_edg_lt";
        if (ps3ui_slot_xy(&s, edge, 0, 0, &ex, &ey)) {
            float cy = sh->cursor == 0 ? ey : ey + 54.0f * (float)(sh->cursor - 1);
            /* the list is narrower than a menu: the same cursor, squeezed */
            float wdt = sh->cursor == 0 ? 1640.0f - 295.0f : 1128.0f - 295.0f;
            ps3ui_slots_t none = { 0 };
            ps3ui_play(cv, &ps3ui_n_cmn_base, "cursor_cmn01_46", sh->cursor_t,
                       ps3ui_mat_mul(ps3ui_mat_translate(ex, cy), ps3ui_mat_scale(wdt / 1155.0f, 1.0f)), &none);
        }
    }
    if (ps3ui_slot_xy(&s, "p_ctrl_typesel_ct", 0.5f, 0, &x, &y)) {
        char t[64];
        snprintf(t, sizeof t, sh->cursor == 0 ? "< %s >" : "%s", ps3ui_type_names[sh->ctl_type]);
        ps3ui_text_centre(cv, &st, x, y, t, alpha);
    }
    static const char *const keys[PS3UI_KEYS] = { "\x03", "\x01", "\x04", "\x02", "L1", "L2", "R1", "R2" };
    if (ps3ui_slot_xy(&s, "p_txt_01_lt", 0, 0, &lx, &ly) && ps3ui_slot_xy(&s, "p_txt_03_rt", 1, 0, &rx, &ry))
        for (int i = 0; i < PS3UI_KEYS; i++) {
            float yy = ly + 54.0f * (float)i;
            ps3ui_text_left(cv, &st, lx, yy, keys[i], alpha);
            ps3ui_text_right(cv, &st, rx, yy, ps3ui_btn_names[sh->ctl[sh->ctl_type][i]], alpha);
        }
    /* the pad diagram's two labels */
    if (ps3ui_slot_xy(&s, "p_controls_txt_01_c", 0.5f, 0.5f, &x, &y))
        ps3ui_text_centre(cv, &st, x, y - 21.0f, "Pause Menu", alpha);
    if (ps3ui_slot_xy(&s, "p_controls_txt_02_c", 0.5f, 0.5f, &x, &y))
        ps3ui_text_centre(cv, &st, x, y - 21.0f, "Move", alpha);
}

/* CREDITS: name left, licence right, the rows from credits_top; the explain
 * window below gives the copyright and origin of the row under the cursor. */
static void ps3ui_sh_draw_credits(ps3ui_canvas_t *cv, ps3ui_shell_t *sh)
{
    ps3ui_slots_t s = { 0 };
    ps3ui_win_draw(cv, &sh->main, 0, 0, &s);
    float lx, ly, rx, ry, ex, ey, tx, ty;
    if (!ps3ui_slot_xy(&s, "p_txt_01_lt", 0, 0, &lx, &ly) || !ps3ui_slot_xy(&s, "p_txt_03_rt", 1, 0, &rx, &ry))
        return;
    float alpha = ps3ui_slot_alpha(&s, "p_txt_01_lt");
    if (sh->main.state == PS3UI_WIN_IDLE && ps3ui_slot_xy(&s, "p_win_edg_lt", 0, 0, &ex, &ey))
        ps3ui_draw_cursor(cv, &ps3ui_n_cmn_base, "cursor_cmn01_46", ex,
                          ey + 54.0f * (float)(sh->cursor - sh->credits_top), sh->cursor_t);
    ps3ui_text_style_t st = ps3ui_style_text(37.0f);
    ps3ui_text_style_t ls = st;
    ls.rgb = 0x00F040;
    for (int i = 0; i < PS3UI_CREDITS_ROWS && sh->credits_top + i < PS3UI_CREDITS; i++) {
        const ps3ui_credit_t *c = &ps3ui_credits[sh->credits_top + i];
        float y = ly + 54.0f * (float)i;
        ps3ui_text_left(cv, &st, lx, y, c->name, alpha);
        ps3ui_text_right(cv, sh->credits_top + i == sh->cursor ? &st : &ls, rx, y, c->licence, alpha);
    }
    if (ps3ui_slot_xy(&s, "head_tit_ct", 0.5f, 0, &tx, &ty)) {
        char title[32];
        snprintf(title, sizeof title, "CREDITS  %d/%d", sh->cursor + 1, PS3UI_CREDITS);
        ps3ui_text_style_t ts = ps3ui_style_title(40.0f);
        ps3ui_text(cv, &ts, tx - ps3ui_text_width(&ts, title) * 0.5f, ty + 47.0f, title,
                   ps3ui_slot_alpha(&s, "head_tit_ct"));
    }
    const ps3ui_credit_t *c = &ps3ui_credits[sh->cursor];
    char about[256];
    snprintf(about, sizeof about, "%s\n%s", c->copyright, c->url);
    ps3ui_sh_draw_explain(cv, sh, about);
}

static const char *ps3ui_shell_hints(const ps3ui_shell_t *sh)
{
    if (ps3ui_dialog_showing(&sh->dlg))
        return "\x02:Enter";
    switch (sh->scr) {
    case PS3UI_SH_MAIN: return "\x02:Enter";
    case PS3UI_SH_ARCADE: return "\x01:Back  \x02:Enter";
    case PS3UI_SH_VERSUS: return sh->two_p ? "\x01:Back  \x02:Enter" : "\x01:Back";
    case PS3UI_SH_OPTIONS: return "\x01:Back  \x02:Enter";
    case PS3UI_SH_CREDITS: return "\x01:Back";
    case PS3UI_SH_CONTROLS:
    case PS3UI_SH_SETTINGS:
    case PS3UI_SH_PAUSE: return "\x02:Enter";
    default: return NULL;
    }
}

static void ps3ui_shell_draw(ps3ui_shell_t *sh, ps3ui_canvas_t *cv)
{
    ps3ui_view_t view = ps3ui_shell_view(sh);
    if (view == PS3UI_VIEW_GAME)
        return;
    if (sh->scr == PS3UI_SH_ONLINE) {
        ps3ui_app_draw(sh->online, cv);
        return;
    }
    if (view == PS3UI_VIEW_FULL)
        ps3ui_draw_bg(cv, 100.0f);
    switch (sh->scr) {
    case PS3UI_SH_MAIN:
        ps3ui_sh_draw_list(cv, sh, "MAIN MENU", ps3ui_main_rows, 4, sh->cursor);
        ps3ui_sh_draw_explain(cv, sh, ps3ui_main_explain[sh->cursor]);
        break;
    case PS3UI_SH_ARCADE:
    case PS3UI_SH_VERSUS: {
        int versus = sh->scr == PS3UI_SH_VERSUS, first = versus ? 1 : 0, n = PS3UI_SETS - first;
        const uint8_t *v = versus ? sh->versus : sh->arcade;
        const uint8_t *def = versus ? ps3ui_set_versus_def : ps3ui_set_arcade_def;
        const char *labels[PS3UI_SETS], *values[PS3UI_SETS];
        int is_def[PS3UI_SETS];
        for (int i = 0; i < n; i++) {
            labels[i] = ps3ui_set_label[i + first];
            values[i] = ps3ui_set_values[i + first][v[i + first]];
            is_def[i] = v[i + first] == def[i + first];
        }
        ps3ui_sh_draw_values(cv, sh, versus ? "OFFLINE VERSUS" : "ARCADE", labels, values, is_def, n,
                             versus && !sh->two_p);
        ps3ui_sh_draw_explain(cv, sh, versus && !sh->two_p
                                  ? "Two controllers are needed. Press the START button on Player 2's controller."
                                  : "Press the cross button to start.");
        break;
    }
    case PS3UI_SH_OPTIONS:
        ps3ui_sh_draw_list(cv, sh, "HELP & OPTIONS", sh->no_controls ? ps3ui_option_rows_nc : ps3ui_option_rows,
                           sh->no_controls ? 2 : 3, sh->cursor);
        break;
    case PS3UI_SH_CREDITS:
        ps3ui_sh_draw_credits(cv, sh);
        break;
    case PS3UI_SH_CONTROLS:
        ps3ui_sh_draw_controls(cv, sh);
        break;
    case PS3UI_SH_SETTINGS: {
        static const char *const labels[2] = { "Volume: Music", "Volume: Sound Effects" };
        char m[8], e[8];
        snprintf(m, sizeof m, "%d", sh->music);
        snprintf(e, sizeof e, "%d", sh->se);
        const char *values[2] = { m, e };
        int is_def[2] = { sh->music == 5, sh->se == 5 };
        ps3ui_sh_draw_values(cv, sh, "SETTINGS", labels, values, is_def, 2, 0);
        break;
    }
    case PS3UI_SH_PAUSE:
        ps3ui_sh_draw_list(cv, sh, "1P PAUSE", ps3ui_pause_rows, 4, sh->pause_cursor);
        break;
    default:
        break;
    }
    ps3ui_dialog_draw(cv, &sh->dlg, sh->cursor_t);
    ps3ui_slot_t bar = ps3ui_draw_hint_bar(cv, sh->bar.t);
    ps3ui_draw_hint_text(cv, &bar, ps3ui_shell_hints(sh));
}

#endif /* PS3UI_SHELL_H */
