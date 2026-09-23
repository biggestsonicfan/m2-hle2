/*
 * ps3ui_app.h -- the PS3-style online menus, driven by a pad.
 *
 * The PS3 port runs its network play through one task (TaskSession, see
 * tools/ps3ui/README.md) whose screens are AET windows with text in their
 * placeholders. This is that task over our netplay (net/netplay.h): the same
 * screens, the same buttons, the same timers, with RPCN rooms underneath.
 *
 *   Online Battle        -> sign in (ours: the PS3 is always signed in)
 *   PLAYER MATCH menu    -> Quick Match / Custom Match / Create Match / Controls
 *   RULE MENU            -> the room to create
 *   connect_win          -> "Accessing..." / "Searching for sessions..."
 *   search_rslt          -> up to six rooms, joined with the cross button
 *   connect_win          -> "Waiting for an opponent." (a room of two)
 *   search_rslt + timer  -> the ROOM MATCH list (a room of three or more)
 *   z_base               -> the VS lobby: both fighters, READY, the countdown
 *   vs_end_menu_PS3      -> the result
 *
 * The frontend feeds it the pad once a frame (ps3ui_app_frame), draws the
 * canvas while ps3ui_app_visible() is true, and gives the game the pad only
 * when it is not. Everything goes to netplay through `ps3ui_backend_t`, so a
 * test can run every screen off a made-up status.
 */
#ifndef PS3UI_APP_H
#define PS3UI_APP_H

#include <stdio.h>
#include <string.h>
#include "ps3ui_screens.h"
#include "netplay.h"

/* The official RPCN server unless the player picks ours on the sign-in screen
 * (netplay.h, NETPLAY_SERVER_*). Twitch sign-in is ours only. */
#define PS3UI_DEFAULT_SERVER NETPLAY_SERVER_OFFICIAL

/* ---- the pad ----------------------------------------------------------------- */

enum {
    PS3UI_PAD_UP = 1u << 0,
    PS3UI_PAD_DOWN = 1u << 1,
    PS3UI_PAD_LEFT = 1u << 2,
    PS3UI_PAD_RIGHT = 1u << 3,
    PS3UI_PAD_CROSS = 1u << 4,      /* confirm (Western consoles) */
    PS3UI_PAD_CIRCLE = 1u << 5,     /* back */
    PS3UI_PAD_TRIANGLE = 1u << 6,
    PS3UI_PAD_SQUARE = 1u << 7,
    PS3UI_PAD_START = 1u << 8,
    PS3UI_PAD_SELECT = 1u << 9,
    PS3UI_PAD_L1 = 1u << 10,
    PS3UI_PAD_R1 = 1u << 11,
};

/* Held-button repeat on the d-pad: the first repeat after 20 frames, then
 * every 6. */
#define PS3UI_REPEAT_DELAY 20
#define PS3UI_REPEAT_RATE  6

/* ---- windows: the PS3's AetWin ------------------------------------------------ */

enum { PS3UI_WIN_CLOSED, PS3UI_WIN_OPENING, PS3UI_WIN_IDLE, PS3UI_WIN_CLOSING };

typedef struct {
    const ps3ui_scene_t *scene;
    const ps3ui_window_def_t *def;
    int state;
    float t;
} ps3ui_win_t;

static void ps3ui_win_open(ps3ui_win_t *w, const ps3ui_scene_t *scene, const char *name)
{
    const ps3ui_window_def_t *def = NULL;
    for (int i = 0; i < scene->nwins; i++)
        if (strcmp(scene->wins[i].name, name) == 0)
            def = &scene->wins[i];
    if (!def) {
        fprintf(stderr, "ps3ui: no window %s\n", name);
        return;
    }
    if (w->def == def && w->state != PS3UI_WIN_CLOSED && w->state != PS3UI_WIN_CLOSING)
        return;
    w->scene = scene;
    w->def = def;
    w->state = def->sta_s >= 0.0f ? PS3UI_WIN_OPENING : PS3UI_WIN_IDLE;
    w->t = def->sta_s >= 0.0f ? def->sta_s : def->neu_s;
}

static void ps3ui_win_close(ps3ui_win_t *w)
{
    if (w->state == PS3UI_WIN_OPENING || w->state == PS3UI_WIN_IDLE) {
        if (w->def->end_s >= 0.0f) {
            w->state = PS3UI_WIN_CLOSING;
            w->t = w->def->end_s;
        } else {
            w->state = PS3UI_WIN_CLOSED;
        }
    }
}

static int ps3ui_win_is(const ps3ui_win_t *w, const char *name)
{
    return w->def && w->state != PS3UI_WIN_CLOSED && w->state != PS3UI_WIN_CLOSING && strcmp(w->def->name, name) == 0;
}

static void ps3ui_win_tick(ps3ui_win_t *w)
{
    switch (w->state) {
    case PS3UI_WIN_OPENING:
        if (++w->t > w->def->sta_e) {
            w->state = PS3UI_WIN_IDLE;
            w->t = w->def->neu_s;
        }
        break;
    case PS3UI_WIN_IDLE:
        if (++w->t > w->def->neu_e)
            w->t = w->def->neu_s;
        break;
    case PS3UI_WIN_CLOSING:
        if (++w->t > w->def->end_e)
            w->state = PS3UI_WIN_CLOSED;
        break;
    default:
        break;
    }
}

static void ps3ui_win_draw(ps3ui_canvas_t *cv, const ps3ui_win_t *w, float dx, float dy, ps3ui_slots_t *slots)
{
    if (!w->def || w->state == PS3UI_WIN_CLOSED)
        return;
    ps3ui_play(cv, w->scene, w->def->name, w->t, ps3ui_mat_translate(dx, dy), slots);
}

/* Text fades with its window: the placeholder's opacity, as the PS3 does. */
static float ps3ui_slot_alpha(const ps3ui_slots_t *s, const char *name)
{
    const ps3ui_slot_t *t = ps3ui_slot(s, name);
    return t ? t->opacity : 0.0f;
}

static int ps3ui_slot_xy(const ps3ui_slots_t *s, const char *name, float u, float v, float *x, float *y)
{
    const ps3ui_slot_t *t = ps3ui_slot(s, name);
    if (!t)
        return 0;
    ps3ui_slot_at(t, u * (float)t->w, v * (float)t->h, x, y);
    return 1;
}

/* ---- text helpers --------------------------------------------------------- */

/* Centre a line on x (flag 8) with its cap-top at y (a 46 cell's row 5). */
static void ps3ui_text_centre(ps3ui_canvas_t *cv, const ps3ui_text_style_t *st, float x, float top, const char *s,
                              float a)
{
    ps3ui_rich(cv, st, x - ps3ui_rich_width(st, s) * 0.5f, top + 42.0f * st->cap / 37.0f, s, a);
}

static void ps3ui_text_left(ps3ui_canvas_t *cv, const ps3ui_text_style_t *st, float x, float top, const char *s, float a)
{
    ps3ui_rich(cv, st, x, top + 42.0f * st->cap / 37.0f, s, a);
}

static void ps3ui_text_right(ps3ui_canvas_t *cv, const ps3ui_text_style_t *st, float x, float top, const char *s,
                             float a)
{
    ps3ui_rich(cv, st, x - ps3ui_rich_width(st, s), top + 42.0f * st->cap / 37.0f, s, a);
}

/* Word-wrap `s` into the box [x0, x1] and centre the block vertically in
 * [y0, y1] (connect_win's flags 0x800 + vertical centring). Lines centred. */
static void ps3ui_text_box(ps3ui_canvas_t *cv, const ps3ui_text_style_t *st, float x0, float y0, float x1, float y1,
                           const char *s, float a, int centre)
{
    char lines[8][160];
    int n = 0;
    const char *p = s;
    while (*p && n < 8) {
        char line[160] = "";
        int len = 0;
        const char *q = p;
        const char *brk = NULL;
        while (*q && *q != '\n') {
            char test[160];
            int tl = (int)(q - p) + 1;
            if (tl >= 159)
                break;
            memcpy(test, p, (size_t)tl);
            test[tl] = 0;
            if (ps3ui_rich_width(st, test) > x1 - x0 && brk)
                break;
            if (*q == ' ')
                brk = q;
            q++;
        }
        const char *end = q;
        if (*q && *q != '\n' && brk)
            end = brk;
        len = (int)(end - p);
        if (len > 159)
            len = 159;
        memcpy(line, p, (size_t)len);
        line[len] = 0;
        snprintf(lines[n++], sizeof lines[0], "%s", line);
        p = end;
        while (*p == ' ')
            p++;
        if (*p == '\n')
            p++;
    }
    float lh = 46.0f * st->cap / 37.0f + 8.0f;
    float top = (y0 + y1) * 0.5f - (lh * (float)n - 8.0f) * 0.5f;
    for (int i = 0; i < n; i++) {
        if (centre)
            ps3ui_text_centre(cv, st, (x0 + x1) * 0.5f, top + lh * (float)i, lines[i], a);
        else
            ps3ui_text_left(cv, st, x0, top + lh * (float)i, lines[i], a);
    }
}


static void ps3ui_draw_cursor(ps3ui_canvas_t *cv, const ps3ui_scene_t *scene, const char *comp, float x, float y,
                              float t);
#define ps3ui_draw_cursor_fwd ps3ui_draw_cursor

/* ---- the PS3's info window: OK and Yes/No dialogs ----------------------------
 *
 * TaskWindowInformation (0x77754): the message is wrapped in the width of the
 * small window's text box and the window picked by how many lines it takes --
 * two or fewer cmn_win_s_*, three cmn_win_m_*, more cmn_win_l_*; _02 is the OK
 * variant, _03 the Yes/No one. Yes/No starts on No. Up/down move (no wrap), the
 * confirm button answers, and the cancel button does nothing: to say no you
 * move to No. The hint bar says only "x:Enter". */

typedef struct {
    int open, yesno, sel, result;       /* result: -1 open, 0 Yes/OK, 1 No */
    char msg[320];
    ps3ui_win_t win;
} ps3ui_dialog_t;

static int ps3ui_dialog_lines(const char *msg)
{
    ps3ui_text_style_t st = ps3ui_style_text(37.0f);
    const float width = 1512.0f - 408.0f;  /* cmn_win_s_01: p_txt_01_lt .. p_txt_02_rb */
    int lines = 1;
    float x = 0.0f;
    char word[128];
    for (const char *p = msg; *p;) {
        if (*p == '\n') {
            lines++;
            x = 0.0f;
            p++;
            continue;
        }
        int n = 0;
        while (p[n] && p[n] != ' ' && p[n] != '\n' && n < 126)
            n++;
        memcpy(word, p, (size_t)n);
        word[n] = 0;
        float w = ps3ui_rich_width(&st, word) + ps3ui_text_width(&st, " ");
        if (x > 0.0f && x + w > width) {
            lines++;
            x = 0.0f;
        }
        x += w;
        p += n;
        while (*p == ' ')
            p++;
    }
    return lines;
}

static void ps3ui_dialog_ask(ps3ui_dialog_t *d, const char *msg, int yesno)
{
    snprintf(d->msg, sizeof d->msg, "%s", msg);
    d->yesno = yesno;
    d->sel = yesno ? 1 : 0;             /* every Yes/No the game opens starts on No */
    d->result = -1;
    d->open = 1;
    int lines = ps3ui_dialog_lines(msg);
    const char *name = lines <= 2 ? (yesno ? "cmn_win_s_03" : "cmn_win_s_02")
                     : lines == 3 ? (yesno ? "cmn_win_m_03" : "cmn_win_m_02")
                                  : (yesno ? "cmn_win_l_03" : "cmn_win_l_02");
    d->win.state = PS3UI_WIN_CLOSED;
    ps3ui_win_open(&d->win, &ps3ui_n_cmn_base, name);
}

/* One frame: `pressed` in PS3UI_PAD_* bits. Returns the answer once, when the
 * window has closed; -1 until then. */
static int ps3ui_dialog_update(ps3ui_dialog_t *d, uint32_t pressed)
{
    ps3ui_win_tick(&d->win);
    if (!d->open) {
        if (d->result >= 0 && d->win.state == PS3UI_WIN_CLOSED) {
            int r = d->result;
            d->result = -1;
            return r;
        }
        return -1;
    }
    if (d->win.state != PS3UI_WIN_IDLE)
        return -1;
    if (d->yesno && (pressed & PS3UI_PAD_UP))
        d->sel = 0;
    if (d->yesno && (pressed & PS3UI_PAD_DOWN))
        d->sel = 1;
    if (pressed & PS3UI_PAD_CROSS) {
        d->result = d->sel;
        d->open = 0;
        ps3ui_win_close(&d->win);
    }
    return -1;
}

static int ps3ui_dialog_showing(const ps3ui_dialog_t *d)
{
    return d->open || (d->win.def && d->win.state != PS3UI_WIN_CLOSED);
}

static void ps3ui_dialog_draw(ps3ui_canvas_t *cv, const ps3ui_dialog_t *d, float cursor_t)
{
    if (!ps3ui_dialog_showing(d))
        return;
    ps3ui_slots_t s = { 0 };
    ps3ui_win_draw(cv, &d->win, 0, 0, &s);
    float x0, y0, x1, y1, ex, ey;
    if (!ps3ui_slot_xy(&s, "p_txt_01_lt", 0, 0, &x0, &y0) || !ps3ui_slot_xy(&s, "p_txt_02_rb", 1, 1, &x1, &y1))
        return;
    float alpha = ps3ui_slot_alpha(&s, "p_txt_01_lt");
    ps3ui_text_style_t st = ps3ui_style_text(37.0f);
    /* the message, wrapped from the top of its box */
    char line[320];
    const char *p = d->msg;
    float y = y0;
    while (*p) {
        int n = 0, brk = -1;
        while (p[n] && p[n] != '\n') {
            char t[320];
            memcpy(t, p, (size_t)n + 1);
            t[n + 1] = 0;
            if (brk >= 0 && ps3ui_rich_width(&st, t) > x1 - x0)
                break;
            if (p[n] == ' ')
                brk = n;
            n++;
        }
        int take = (p[n] && p[n] != '\n' && brk >= 0) ? brk : n;
        memcpy(line, p, (size_t)take);
        line[take] = 0;
        ps3ui_text_left(cv, &st, x0, y, line, alpha);
        y += 54.0f;
        p += take;
        while (*p == ' ')
            p++;
        if (*p == '\n')
            p++;
    }
    const char *a = d->yesno ? "p_txt_yes_01_lt" : "p_txt_ok_01_lt";
    const char *b = d->yesno ? "p_txt_yes_02_rb" : "p_txt_ok_02_rb";
    float ax, ay, bx, by;
    if (!ps3ui_slot_xy(&s, a, 0, 0, &ax, &ay) || !ps3ui_slot_xy(&s, b, 1, 1, &bx, &by))
        return;
    if (d->win.state == PS3UI_WIN_IDLE && ps3ui_slot_xy(&s, "p_win_edg_lt", 0, 0, &ex, &ey))
        ps3ui_draw_cursor_fwd(cv, &ps3ui_n_cmn_base, "cursor_cmn01_46", ex, ay - 4.0f + 54.0f * (float)d->sel, cursor_t);
    ps3ui_text_centre(cv, &st, (ax + bx) * 0.5f, ay, d->yesno ? "Yes" : "OK", alpha);
    if (d->yesno)
        ps3ui_text_centre(cv, &st, (ax + bx) * 0.5f, ay + 54.0f, "No", alpha);
}

/* ---- the backend: netplay, or a test's stand-in --------------------------------- */

typedef struct {
    void (*get_status)(netplay_status_t *out);
    void (*post)(netplay_cmd_kind_t kind, const netplay_config_t *cfg);
    bool (*stored_settings)(netplay_config_t *out);
} ps3ui_backend_t;

/* ---- the task ------------------------------------------------------------------- */

typedef enum {
    PS3UI_SCR_NONE,             /* hidden: the game has the screen */
    PS3UI_SCR_SIGNIN,           /* ours: how to sign in */
    PS3UI_SCR_TWITCH,           /* ours: the Twitch code */
    PS3UI_SCR_OSK,              /* ours: the on-screen keyboard */
    PS3UI_SCR_MENU,             /* PLAYER MATCH: Quick / Custom / Create / Controls */
    PS3UI_SCR_RULE,             /* RULE MENU: the room to create */
    PS3UI_SCR_CONNECT,          /* "Accessing..." / "Searching for sessions..." */
    PS3UI_SCR_SEARCH,           /* the session list */
    PS3UI_SCR_ROOM,             /* waiting, or the ROOM MATCH list */
    PS3UI_SCR_VS,               /* the VS lobby */
    PS3UI_SCR_RESULT,           /* after a match */
} ps3ui_screen_t;

enum { PS3UI_DLG_NONE, PS3UI_DLG_LEAVE, PS3UI_DLG_EXIT, PS3UI_DLG_ERROR, PS3UI_DLG_SIGNOUT };

#define PS3UI_ROWS 6

typedef struct {
    ps3ui_backend_t be;
    netplay_status_t st;
    netplay_config_t cfg;
    int have_cfg;

    int open;                   /* Online Battle was chosen */
    ps3ui_screen_t scr;
    int cursor;
    int dialog, dialog_cursor;
    char dialog_text[256];
    ps3ui_dialog_t dlg;

    /* the pad */
    uint32_t held, pressed;
    uint32_t repeat_held;
    int repeat_frames;

    /* windows */
    ps3ui_win_t main, sub, msg, timer, bar;
    float bg_t;
    float cursor_t;
    uint32_t frame;

    int default_delay;          /* frames of input delay a room hosted here asks for ("Auto") */
    int last_state;             /* netplay's state last frame */

    /* the menu we are in */
    int quick;                  /* Quick Match: join what the search finds, or host */
    int searching;              /* a search is out */
    uint32_t search_sent;
    int rule_players, rule_vs, rule_delay;
    int rule_damage_real;       /* DAMAGE REAL (no catch-up); ours only, NORMAL by default */
    int list_n;
    int list_idx[PS3UI_ROWS];

    /* the room */
    float countdown;            /* frames */
    int ready_sent;
    float ready_age[2];
    int last_ready[2];
    uint16_t last_match;
    int result_side;            /* 0/1 = winner side, -1 = none */
    float result_t;
    char result_names[2][20];

    /* the on-screen keyboard (ours) */
    int osk_field;              /* 0 = name, 1 = password, 2 = e-mail token */
    char osk_text[3][128];   /* [1] holds RPCS3's 64-character derived key too */
    int fail_shown;             /* this failure's error has been shown */
    int osk_row, osk_col, osk_shift;
    ps3ui_screen_t osk_back;
} ps3ui_app_t;

static ps3ui_app_t g_ps3ui_app;

static void ps3ui_app_ask(ps3ui_app_t *a, int kind, const char *msg);

/* ---- strings, as the PS3 prints them (string_array ids in brackets) -------- */

static const char *const ps3ui_str_title = "PLAYER MATCH";                          /* 0x158 */
static const char *const ps3ui_str_menu[5] = { "Quick Match", "Custom Match", "Create Match", "Controls",
                                               "Sign out" };                /* ours */

/* ---- the server --------------------------------------------------------------- */

static int ps3ui_on_community(const ps3ui_app_t *a) { return netplay_server_is_community(a->cfg.server); }

/* Sign in on `server` from here on. An RPCN account is the server's own, so a
 * password typed for the other one is not carried across, and neither is a
 * certificate pin. */
static void ps3ui_set_server(ps3ui_app_t *a, const char *server)
{
    if (netplay_same_name(a->cfg.server, server))
        return;
    snprintf(a->cfg.server, sizeof a->cfg.server, "%s", server);
    a->cfg.port = RPCN_DEFAULT_PORT;
    a->cfg.fingerprint[0] = 0;
    a->cfg.password[0] = 0;
    a->cfg.token[0] = 0;
}

static const char *ps3ui_server_label(const ps3ui_app_t *a)
{
    if (netplay_server_is_official(a->cfg.server))
        return "RPCN (official)";
    if (ps3ui_on_community(a))
        return "sonicthefighte.rs";
    return a->cfg.server;                   /* a settings file that names another */
}

/* The rule menu's rows: DAMAGE is a setting of our server's rooms. */
static int ps3ui_rule_rows(const ps3ui_app_t *a) { return ps3ui_on_community(a) ? 4 : 3; }

/* ---- the task's life ----------------------------------------------------------- */

static void ps3ui_app_defaults(ps3ui_app_t *a)
{
    netplay_config_t *c = &a->cfg;
    if (!c->server[0])   snprintf(c->server, sizeof c->server, "%s", PS3UI_DEFAULT_SERVER);
    if (!c->port)        c->port = RPCN_DEFAULT_PORT;
    if (!c->frame_delay) c->frame_delay = (uint32_t)a->default_delay;
    if (!c->max_players) c->max_players = 2;
}

static void ps3ui_app_init(ps3ui_app_t *a, ps3ui_backend_t be)
{
    memset(a, 0, sizeof *a);
    a->be = be;
    a->rule_players = 2;
    a->rule_delay = 0;
    a->result_side = -1;
    a->default_delay = 2;
    a->last_state = -1;
    if (be.stored_settings)
        a->have_cfg = be.stored_settings(&a->cfg) ? 1 : 0;
    ps3ui_app_defaults(a);
    /* a zeroed status is not "no session": its desync_frame says frame 0 */
    a->st.desync_frame = LOCKSTEP_NO_CHECK;
    if (be.get_status)
        be.get_status(&a->st);
}

/* The real thing: netplay.h behind the task. The caller has set the settings
 * file's path (netplay_set_config_path) if it keeps one of its own. */
static void ps3ui_app_netplay(ps3ui_app_t *a, int delay)
{
    netplay_init();
    netplay_set_open_browser(false);   /* the Twitch code and address go on screen */
    ps3ui_backend_t be = { netplay_get_status, netplay_post, netplay_stored_settings };
    ps3ui_app_init(a, be);
    if (delay > 0) {
        a->default_delay = delay;
        a->cfg.frame_delay = (uint32_t)delay;
    }
}

static int ps3ui_app_visible(const ps3ui_app_t *a) { return a->open && a->scr != PS3UI_SCR_NONE; }

static void ps3ui_app_go(ps3ui_app_t *a, ps3ui_screen_t s)
{
    if (a->scr == s)
        return;
    a->scr = s;
    a->cursor = 0;
    ps3ui_win_close(&a->sub);
    ps3ui_win_close(&a->msg);
    ps3ui_win_close(&a->timer);
}

static void ps3ui_post(ps3ui_app_t *a, netplay_cmd_kind_t k)
{
    if (k == NETPLAY_CMD_CONNECT || k == NETPLAY_CMD_TWITCH_START)
        a->fail_shown = 0;              /* a new attempt: its failure is news */
    if (a->be.post)
        a->be.post(k, &a->cfg);
}

/* Open the task (the main menu's "Online Battle"). */
static void ps3ui_app_open(ps3ui_app_t *a)
{
    a->open = 1;
    ps3ui_win_open(&a->bar, &ps3ui_n_cmn_base, "sousa_win");
    a->scr = PS3UI_SCR_NONE;
}

static void ps3ui_app_close(ps3ui_app_t *a)
{
    a->open = 0;
    a->scr = PS3UI_SCR_NONE;
}

/* ---- input ------------------------------------------------------------------- */

static void ps3ui_app_pad(ps3ui_app_t *a, uint32_t held)
{
    a->pressed = held & ~a->held;
    a->held = held;
    uint32_t dirs = held & (PS3UI_PAD_UP | PS3UI_PAD_DOWN | PS3UI_PAD_LEFT | PS3UI_PAD_RIGHT);
    if (dirs && dirs == a->repeat_held) {
        if (++a->repeat_frames >= PS3UI_REPEAT_DELAY &&
            (a->repeat_frames - PS3UI_REPEAT_DELAY) % PS3UI_REPEAT_RATE == 0)
            a->pressed |= dirs;
    } else {
        a->repeat_held = dirs;
        a->repeat_frames = 0;
    }
}

static int ps3ui_hit(const ps3ui_app_t *a, uint32_t b) { return (a->pressed & b) != 0; }

/* A vertical cursor over n rows; wrap as the list does, or clamp. */
static int ps3ui_move(ps3ui_app_t *a, int *cur, int n, int wrap)
{
    int old = *cur;
    if (n <= 0)
        return 0;
    if (ps3ui_hit(a, PS3UI_PAD_UP))
        *cur = *cur > 0 ? *cur - 1 : wrap ? n - 1 : 0;
    if (ps3ui_hit(a, PS3UI_PAD_DOWN))
        *cur = *cur < n - 1 ? *cur + 1 : wrap ? 0 : n - 1;
    if (*cur >= n)
        *cur = n - 1;
    return *cur != old;
}

/* ---- the room, read off the status --------------------------------------------- */

static int ps3ui_in_room(const netplay_status_t *st)
{
    return st->state == NETPLAY_IN_ROOM || st->state == NETPLAY_SYNCING;
}

/* The two fighters of the next match: the room's fighters when a match is
 * under way, else the front of the line (1P/2P entries first), as room.h
 * picks them. Returns member indices, -1 for an empty side. */
static void ps3ui_fighters(const netplay_status_t *st, int out[2])
{
    out[0] = out[1] = -1;
    for (uint32_t i = 0; i < st->member_count; i++) {
        int side = st->members[i].side;
        if (side == 0 || side == 1)
            out[side] = (int)i;
    }
    if (out[0] >= 0 || out[1] >= 0)
        return;
    /* nobody on a side yet: by entry, then by line */
    for (int want = 1; want <= 2; want++)
        for (uint32_t i = 0; i < st->member_count; i++)
            if (st->members[i].data.entry == want && out[want - 1] < 0)
                out[want - 1] = (int)i;
    for (int pos = 0; pos < (int)ROOM_MAX_MEMBERS; pos++)
        for (uint32_t i = 0; i < st->member_count; i++) {
            if (st->members[i].line_pos != pos || (int)i == out[0] || (int)i == out[1])
                continue;
            if (st->members[i].data.flags & ROOM_MEMBER_WATCH)
                continue;
            if (out[0] < 0)
                out[0] = (int)i;
            else if (out[1] < 0)
                out[1] = (int)i;
        }
}

/* ---- screens: update ------------------------------------------------------------ */

static void ps3ui_start_search(ps3ui_app_t *a)
{
    a->searching = 1;
    a->search_sent = a->frame;
    ps3ui_post(a, NETPLAY_CMD_SEARCH);
    ps3ui_app_go(a, PS3UI_SCR_CONNECT);
}

/* The rooms a join could work on, in the list's order: not ours, not full,
 * not locked, playable by this build. */
static void ps3ui_collect_rooms(ps3ui_app_t *a)
{
    a->list_n = 0;
    for (uint32_t i = 0; i < a->st.room_count && a->list_n < PS3UI_ROWS; i++) {
        const rpcn_room_listing_t *r = &a->st.rooms[i];
        if (r->cur_members == 0 || r->room_id == a->st.room_id)
            continue;
        a->list_idx[a->list_n++] = (int)i;
    }
}

static int ps3ui_room_joinable(const ps3ui_app_t *a, const rpcn_room_listing_t *r)
{
    return !r->has_password && r->cur_members < r->max_slots
        && !netplay_room_reject_reason(r->flag_attr, g_active_profile);
}

static void ps3ui_host(ps3ui_app_t *a, int players)
{
    a->cfg.max_players = (uint32_t)players;
    a->cfg.vs_mode = a->rule_vs != 0;
    a->cfg.damage_real = ps3ui_on_community(a) && a->rule_damage_real;
    a->cfg.frame_delay = (uint32_t)(a->rule_delay ? a->rule_delay : a->default_delay);
    ps3ui_post(a, NETPLAY_CMD_HOST);
    ps3ui_app_go(a, PS3UI_SCR_CONNECT);
}

static void ps3ui_update_signin(ps3ui_app_t *a)
{
    /* rows: Twitch, account, server, back */
    ps3ui_move(a, &a->cursor, 4, 1);
    if (ps3ui_hit(a, PS3UI_PAD_CIRCLE)) {
        ps3ui_app_close(a);
        return;
    }
    if (a->cursor == 2 && ps3ui_hit(a, PS3UI_PAD_LEFT | PS3UI_PAD_RIGHT | PS3UI_PAD_CROSS)) {
        ps3ui_set_server(a, ps3ui_on_community(a) ? NETPLAY_SERVER_OFFICIAL : NETPLAY_SERVER_COMMUNITY);
        return;
    }
    if (!ps3ui_hit(a, PS3UI_PAD_CROSS))
        return;
    if (a->cursor == 0) {
        /* Twitch sign-in is our server's: the official one has none. A stored
         * Twitch login is reused; the device flow (and its code) runs only when
         * there is none (netplay_twitch_reuse). */
        ps3ui_set_server(a, NETPLAY_SERVER_COMMUNITY);
        a->cfg.npid[0] = 0;
        ps3ui_post(a, NETPLAY_CMD_TWITCH_START);
        ps3ui_app_go(a, PS3UI_SCR_TWITCH);
    } else if (a->cursor == 1) {
        a->osk_field = 0;
        snprintf(a->osk_text[0], sizeof a->osk_text[0], "%s", a->cfg.npid);
        a->osk_text[1][0] = 0;
        a->osk_back = PS3UI_SCR_SIGNIN;
        ps3ui_app_go(a, PS3UI_SCR_OSK);
    } else if (a->cursor == 3) {
        ps3ui_app_close(a);
    }
}

/* The on-screen keyboard: a 10x4 grid, then a row of actions. */
static const char *const ps3ui_osk_rows[2][4] = {
    { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_.@" },
    { "!\"#$%&'()=", "QWERTYUIOP", "ASDFGHJKL+", "ZXCVBNM,/?" },
};
enum { PS3UI_OSK_ACT_SHIFT, PS3UI_OSK_ACT_SPACE, PS3UI_OSK_ACT_BACK, PS3UI_OSK_ACT_DONE, PS3UI_OSK_ACTS };
static const char *const ps3ui_osk_act[PS3UI_OSK_ACTS] = { "Shift", "Space", "Delete", "Done" };

static void ps3ui_update_osk(ps3ui_app_t *a)
{
    char *t = a->osk_text[a->osk_field];
    size_t len = strlen(t);
    int cols = a->osk_row == 4 ? PS3UI_OSK_ACTS : 10;
    if (ps3ui_hit(a, PS3UI_PAD_UP)) a->osk_row = (a->osk_row + 4) % 5;
    if (ps3ui_hit(a, PS3UI_PAD_DOWN)) a->osk_row = (a->osk_row + 1) % 5;
    cols = a->osk_row == 4 ? PS3UI_OSK_ACTS : 10;
    if (a->osk_col >= cols) a->osk_col = cols - 1;
    if (ps3ui_hit(a, PS3UI_PAD_LEFT)) a->osk_col = (a->osk_col + cols - 1) % cols;
    if (ps3ui_hit(a, PS3UI_PAD_RIGHT)) a->osk_col = (a->osk_col + 1) % cols;
    if (ps3ui_hit(a, PS3UI_PAD_SQUARE) && len)   /* delete */
        t[len - 1] = 0;
    if (ps3ui_hit(a, PS3UI_PAD_TRIANGLE) && len + 1 < 20)
        t[len] = ' ', t[len + 1] = 0;
    if (ps3ui_hit(a, PS3UI_PAD_L1) || ps3ui_hit(a, PS3UI_PAD_R1))
        a->osk_shift ^= 1;
    if (ps3ui_hit(a, PS3UI_PAD_CIRCLE)) {
        ps3ui_app_go(a, a->osk_back);
        return;
    }
    int done = ps3ui_hit(a, PS3UI_PAD_START);
    if (ps3ui_hit(a, PS3UI_PAD_CROSS)) {
        if (a->osk_row < 4) {
            if (len + 1 < (a->osk_field == 0 ? 17u : (uint32_t)sizeof a->osk_text[1])) {
                t[len] = ps3ui_osk_rows[a->osk_shift][a->osk_row][a->osk_col];
                t[len + 1] = 0;
            }
        } else if (a->osk_col == PS3UI_OSK_ACT_SHIFT) {
            a->osk_shift ^= 1;
        } else if (a->osk_col == PS3UI_OSK_ACT_SPACE) {
            if (len + 1 < (uint32_t)sizeof a->osk_text[1]) t[len] = ' ', t[len + 1] = 0;
        } else if (a->osk_col == PS3UI_OSK_ACT_BACK) {
            if (len) t[len - 1] = 0;
        } else {
            done = 1;
        }
    }
    if (!done)
        return;
    if (a->osk_field == 0) {
        a->osk_field = 1;               /* on to the password */
        a->osk_row = a->osk_col = 0;
        return;
    }
    if (a->osk_field == 2) {
        /* the e-mail token, for the account and password already typed */
        snprintf(a->cfg.token, sizeof a->cfg.token, "%s", a->osk_text[2]);
    } else {
        snprintf(a->cfg.npid, sizeof a->cfg.npid, "%s", a->osk_text[0]);
        snprintf(a->cfg.password, sizeof a->cfg.password, "%s", a->osk_text[1]);
    }
    a->cfg.twitch_token[0] = 0;
    ps3ui_post(a, NETPLAY_CMD_CONNECT);
    ps3ui_app_go(a, PS3UI_SCR_CONNECT);
}

static void ps3ui_update_menu(ps3ui_app_t *a)
{
    ps3ui_move(a, &a->cursor, 5, 1);
    if (ps3ui_hit(a, PS3UI_PAD_CIRCLE)) {
        ps3ui_app_ask(a, PS3UI_DLG_EXIT, "Do you want to exit Player Match?");
        return;
    }
    if (!ps3ui_hit(a, PS3UI_PAD_CROSS))
        return;
    switch (a->cursor) {
    case 0:                                 /* Quick Match */
        a->quick = 1;
        ps3ui_start_search(a);
        break;
    case 1:                                 /* Custom Match */
        a->quick = 0;
        ps3ui_start_search(a);
        break;
    case 2:                                 /* Create Match */
        ps3ui_app_go(a, PS3UI_SCR_RULE);
        break;
    case 4:                                 /* Sign out (ours) */
        ps3ui_app_ask(a, PS3UI_DLG_SIGNOUT, "Do you want to sign out? The sign-in will be forgotten.");
        break;
    default:                                /* Controls: the game's own */
        break;
    }
}

/* RULE MENU rows: players, game type, frame delay, and on our server damage. */
static void ps3ui_update_rule(ps3ui_app_t *a)
{
    ps3ui_move(a, &a->cursor, ps3ui_rule_rows(a), 1);
    int d = ps3ui_hit(a, PS3UI_PAD_RIGHT) ? 1 : ps3ui_hit(a, PS3UI_PAD_LEFT) ? -1 : 0;
    if (d) {
        if (a->cursor == 0) a->rule_players = 2 + (a->rule_players - 2 + d + 7) % 7;
        if (a->cursor == 1) a->rule_vs ^= 1;
        if (a->cursor == 2) a->rule_delay = (a->rule_delay + d + 9) % 9;
        if (a->cursor == 3) a->rule_damage_real ^= 1;
    }
    if (ps3ui_hit(a, PS3UI_PAD_CIRCLE))
        ps3ui_app_go(a, PS3UI_SCR_MENU);
    else if (ps3ui_hit(a, PS3UI_PAD_CROSS))
        ps3ui_host(a, a->rule_players);
}

static void ps3ui_update_search(ps3ui_app_t *a)
{
    ps3ui_move(a, &a->cursor, a->list_n, 1);
    if (ps3ui_hit(a, PS3UI_PAD_CIRCLE)) {
        ps3ui_app_go(a, PS3UI_SCR_MENU);
    } else if (ps3ui_hit(a, PS3UI_PAD_SQUARE)) {
        ps3ui_start_search(a);
    } else if (ps3ui_hit(a, PS3UI_PAD_CROSS) && a->list_n) {
        const rpcn_room_listing_t *r = &a->st.rooms[a->list_idx[a->cursor]];
        if (ps3ui_room_joinable(a, r)) {
            a->cfg.room_id = r->room_id;
            ps3ui_post(a, NETPLAY_CMD_JOIN);
            ps3ui_app_go(a, PS3UI_SCR_CONNECT);
        }
    }
}

static void ps3ui_update_connect(ps3ui_app_t *a)
{
    if (a->searching && !a->st.search_pending && a->frame - a->search_sent > 30) {
        a->searching = 0;
        ps3ui_collect_rooms(a);
        if (a->quick) {
            /* Quick Match: a random room that will have us, else our own */
            int pick[PS3UI_ROWS], n = 0;
            for (int i = 0; i < a->list_n; i++)
                if (ps3ui_room_joinable(a, &a->st.rooms[a->list_idx[i]]))
                    pick[n++] = a->list_idx[i];
            if (n) {
                a->cfg.room_id = a->st.rooms[pick[a->frame % (uint32_t)n]].room_id;
                ps3ui_post(a, NETPLAY_CMD_JOIN);
            } else {
                ps3ui_host(a, 2);
            }
        } else {
            ps3ui_app_go(a, PS3UI_SCR_SEARCH);
        }
        return;
    }
    if (a->searching && ps3ui_hit(a, PS3UI_PAD_CIRCLE)) {
        a->searching = 0;
        ps3ui_app_go(a, PS3UI_SCR_MENU);
    }
}

static int ps3ui_me(const netplay_status_t *st)
{
    for (uint32_t i = 0; i < st->member_count; i++)
        if (st->members[i].is_me)
            return (int)i;
    return -1;
}

static void ps3ui_update_room(ps3ui_app_t *a)
{
    const netplay_status_t *st = &a->st;
    int n = (int)st->member_count;
    if (a->countdown > 0.0f)
        a->countdown -= 1.0f;
    if (n >= 3) {
        /* ROOM MATCH: the owner's timer runs the room, so everybody is ready */
        if (!a->ready_sent && !(st->me.flags & ROOM_MEMBER_READY)) {
            ps3ui_post(a, NETPLAY_CMD_START);
            a->ready_sent = 1;
        }
        ps3ui_move(a, &a->cursor, n, 0);
        if (ps3ui_hit(a, PS3UI_PAD_CROSS)) {
            a->cfg.entry = (uint8_t)((st->me.entry + 1) % 3);
            ps3ui_post(a, NETPLAY_CMD_ENTRY);
        }
        if (ps3ui_hit(a, PS3UI_PAD_SQUARE) && st->is_host)
            ps3ui_post(a, NETPLAY_CMD_FORCE_START);
    }
    if (ps3ui_hit(a, PS3UI_PAD_CIRCLE))
        ps3ui_app_ask(a, PS3UI_DLG_LEAVE, "Do you want to exit this session?");
}

static void ps3ui_update_vs(ps3ui_app_t *a)
{
    const netplay_status_t *st = &a->st;
    int ready = (st->me.flags & ROOM_MEMBER_READY) != 0;
    if (a->countdown > 0.0f && st->state == NETPLAY_IN_ROOM) {
        a->countdown -= 1.0f;
        if (a->countdown <= 0.0f && !ready && !a->ready_sent) {
            ps3ui_post(a, NETPLAY_CMD_START);     /* the PS3 readies you at 0 */
            a->ready_sent = 1;
        }
    }
    if (!ready && !a->ready_sent && ps3ui_hit(a, PS3UI_PAD_CROSS)) {
        ps3ui_post(a, NETPLAY_CMD_START);
        a->ready_sent = 1;
    }
    if (!ready && ps3ui_hit(a, PS3UI_PAD_CIRCLE))
        ps3ui_app_ask(a, PS3UI_DLG_LEAVE, "Do you want to exit this session?");
}

static void ps3ui_update_result(ps3ui_app_t *a)
{
    a->result_t += 1.0f;
    if (a->result_t >= 600.0f || ps3ui_hit(a, PS3UI_PAD_CROSS) || ps3ui_hit(a, PS3UI_PAD_CIRCLE))
        ps3ui_app_go(a, PS3UI_SCR_ROOM);
}

static void ps3ui_app_ask(ps3ui_app_t *a, int kind, const char *msg)
{
    a->dialog = kind;
    ps3ui_dialog_ask(&a->dlg, msg, kind != PS3UI_DLG_ERROR);
}

static void ps3ui_update_dialog(ps3ui_app_t *a)
{
    int r = ps3ui_dialog_update(&a->dlg, a->pressed);
    if (r < 0)
        return;
    int kind = a->dialog;
    a->dialog = PS3UI_DLG_NONE;
    if (kind == PS3UI_DLG_ERROR || r != 0)
        return;
    if (kind == PS3UI_DLG_LEAVE) {
        ps3ui_post(a, NETPLAY_CMD_LEAVE_ROOM);
        ps3ui_app_go(a, PS3UI_SCR_MENU);
    } else if (kind == PS3UI_DLG_SIGNOUT) {
        ps3ui_post(a, NETPLAY_CMD_SIGN_OUT);
        /* our copy too, or the sign-in screen signs straight back in */
        a->cfg.password[0] = 0;
        a->cfg.token[0] = 0;
        a->cfg.twitch_token[0] = 0;
        a->cfg.twitch_npid[0] = 0;
        a->cfg.twitch_server[0] = 0;
        ps3ui_app_go(a, PS3UI_SCR_SIGNIN);
    } else {
        ps3ui_app_close(a);
    }
}

/* Follow netplay: which screen the state puts us on. */
static void ps3ui_follow(ps3ui_app_t *a)
{
    const netplay_status_t *st = &a->st;
    if ((int)st->state != a->last_state) {
        a->last_state = (int)st->state;
        /* The login the session now holds -- a Twitch token the flow just
         * landed, an account typed in -- is the one we work with from here on;
         * the room settings stay ours. */
        netplay_config_t fresh;
        if (st->state == NETPLAY_ONLINE && a->be.stored_settings && a->be.stored_settings(&fresh)) {
            fresh.frame_delay = a->cfg.frame_delay;
            fresh.max_players = a->cfg.max_players;
            fresh.vs_mode = a->cfg.vs_mode;
            a->cfg = fresh;
            a->have_cfg = 1;
            ps3ui_app_defaults(a);
        }
    }
    switch (st->state) {
    case NETPLAY_OFF:
        if (a->scr != PS3UI_SCR_OSK && a->scr != PS3UI_SCR_TWITCH && a->scr != PS3UI_SCR_SIGNIN) {
            if (a->have_cfg && (netplay_twitch_here(&a->cfg) || a->cfg.password[0]) && a->scr == PS3UI_SCR_NONE) {
                ps3ui_post(a, NETPLAY_CMD_CONNECT);   /* a stored login: straight in */
                ps3ui_app_go(a, PS3UI_SCR_CONNECT);
            } else if (a->scr != PS3UI_SCR_CONNECT || a->frame % 60 == 0) {
                ps3ui_app_go(a, PS3UI_SCR_SIGNIN);
            }
        }
        if (a->scr == PS3UI_SCR_TWITCH && st->twitch_state == RPCN_TWITCH_DONE)
            ps3ui_app_go(a, PS3UI_SCR_CONNECT);
        break;
    case NETPLAY_CONNECTING:
        if (a->scr != PS3UI_SCR_TWITCH)
            ps3ui_app_go(a, PS3UI_SCR_CONNECT);
        break;
    case NETPLAY_ONLINE:
        if (a->scr == PS3UI_SCR_ROOM || a->scr == PS3UI_SCR_VS || a->scr == PS3UI_SCR_RESULT)
            ps3ui_app_go(a, PS3UI_SCR_MENU);          /* the room went away */
        if (a->scr == PS3UI_SCR_NONE || a->scr == PS3UI_SCR_SIGNIN || a->scr == PS3UI_SCR_TWITCH
            || a->scr == PS3UI_SCR_OSK || (a->scr == PS3UI_SCR_CONNECT && !a->searching))
            ps3ui_app_go(a, PS3UI_SCR_MENU);
        break;
    case NETPLAY_IN_ROOM:
    case NETPLAY_SYNCING: {
        if (a->scr == PS3UI_SCR_RESULT)
            break;
        /* a result has come in: show it first */
        if (st->room.match != a->last_match) {
            uint16_t prev = a->last_match;
            a->last_match = st->room.match;
            if (prev && st->room.phase == ROOM_PHASE_LOBBY && st->room.last_result <= 1) {
                a->result_side = st->room.last_result;
                a->result_t = 0.0f;
                ps3ui_app_go(a, PS3UI_SCR_RESULT);
                break;
            }
        }
        int two = st->member_count <= 2 || st->max_slot <= 2;
        ps3ui_screen_t want = two && st->member_count >= 2 ? PS3UI_SCR_VS : PS3UI_SCR_ROOM;
        if (st->state == NETPLAY_SYNCING)
            want = two ? PS3UI_SCR_VS : PS3UI_SCR_ROOM;
        if (a->scr != want) {
            ps3ui_app_go(a, want);
            a->countdown = want == PS3UI_SCR_VS ? (two ? 1800.0f : 300.0f) : 1800.0f;
            a->ready_sent = 0;
            a->ready_age[0] = a->ready_age[1] = 0.0f;
            a->last_ready[0] = a->last_ready[1] = 0;
        }
        break;
    }
    case NETPLAY_PLAYING:
    case NETPLAY_WATCHING:
        a->scr = PS3UI_SCR_NONE;                     /* the game has the screen */
        break;
    case NETPLAY_FAILED:
        /* Once per failure. Netplay stays FAILED until the next attempt, so
         * asking again whenever the dialog is closed brings it straight back,
         * and the player can never get past it to try anything else. */
        if (!a->fail_shown && st->error[0]) {
            a->fail_shown = 1;
            if (st->need_email_token) {
                /* The password was right; the server verifies accounts by
                 * e-mail. There is no token box on the sign-in screen, so the
                 * keyboard asks for it and signs in again with it. */
                a->osk_field = 2;
                snprintf(a->osk_text[2], sizeof a->osk_text[2], "%s", a->cfg.token);
                a->osk_row = a->osk_col = 0;
                a->osk_back = PS3UI_SCR_SIGNIN;
                ps3ui_app_go(a, PS3UI_SCR_OSK);
                snprintf(a->dialog_text, sizeof a->dialog_text, "%s",
                         a->cfg.token[0] ? "The server refused that e-mail token. Check it against the sign-up e-mail and enter it again."
                                         : "This server verifies accounts by e-mail. Enter the token from the sign-up e-mail.");
            } else {
                snprintf(a->dialog_text, sizeof a->dialog_text, "%s", st->error);
            }
            ps3ui_app_ask(a, PS3UI_DLG_ERROR, a->dialog_text);
        }
        if (a->scr != PS3UI_SCR_SIGNIN && a->scr != PS3UI_SCR_OSK)
            ps3ui_app_go(a, PS3UI_SCR_SIGNIN);
        break;
    }
}

/* Open the windows the current screen shows. Windows run on the task's clock,
 * not the draw's: a frontend that skips a draw must still see them open. */
static const char *const ps3ui_choice_win[9] = { NULL, NULL, "choice_win_02", "choice_win_03", "choice_win_04",
                                                 "choice_win_05", "choice_win_06", "choice_win_07", "choice_win_08" };

static void ps3ui_app_windows(ps3ui_app_t *a)
{
    switch (a->scr) {
    case PS3UI_SCR_SIGNIN:
        ps3ui_win_open(&a->main, &ps3ui_n_cmn_base, ps3ui_choice_win[4]);
        ps3ui_win_open(&a->msg, &ps3ui_n_cmn_base, "cmn_win_b_01");
        break;
    case PS3UI_SCR_OSK:
        ps3ui_win_open(&a->main, &ps3ui_n_cmn_base, ps3ui_choice_win[7]);
        break;
    case PS3UI_SCR_MENU:
        ps3ui_win_open(&a->main, &ps3ui_n_cmn_base, ps3ui_choice_win[5]);
        break;
    case PS3UI_SCR_RULE:
        ps3ui_win_open(&a->main, &ps3ui_n_cmn_base, ps3ui_choice_win[ps3ui_rule_rows(a)]);
        break;
    case PS3UI_SCR_TWITCH:
    case PS3UI_SCR_CONNECT:
        ps3ui_win_open(&a->main, &ps3ui_n_cmn_screen, "connect_win");
        break;
    case PS3UI_SCR_SEARCH:
        ps3ui_win_open(&a->main, &ps3ui_n_cmn_online, "search_rslt");
        ps3ui_win_open(&a->msg, &ps3ui_n_cmn_base, "cmn_win_b_01");
        break;
    case PS3UI_SCR_ROOM:
        if (a->st.member_count >= 3) {
            ps3ui_win_open(&a->main, &ps3ui_n_cmn_online, "search_rslt");
            ps3ui_win_open(&a->timer, &ps3ui_n_cmn_online, "timer");
            ps3ui_win_open(&a->msg, &ps3ui_n_cmn_base, "cmn_win_b_01");
        } else {
            ps3ui_win_open(&a->main, &ps3ui_n_cmn_screen, "connect_win");
            ps3ui_win_close(&a->timer);
            ps3ui_win_close(&a->msg);
        }
        break;
    case PS3UI_SCR_VS:
        ps3ui_win_open(&a->main, &ps3ui_n_cmn_online, "z_base");
        break;
    case PS3UI_SCR_RESULT:
        ps3ui_win_open(&a->main, &ps3ui_n_cmn_online, "vs_end_menu_PS3");
        ps3ui_win_open(&a->timer, &ps3ui_n_cmn_online, "timer");
        break;
    default:
        break;
    }
}

/* One frame of the task. `held` is the pad in PS3UI_PAD_* bits. */
static void ps3ui_app_frame(ps3ui_app_t *a, uint32_t held)
{
    a->frame++;
    ps3ui_app_pad(a, held);
    if (a->be.get_status)
        a->be.get_status(&a->st);       /* read even while closed: the host reports from it */
    if (!a->open)
        return;
    ps3ui_follow(a);
    if (a->dialog || ps3ui_dialog_showing(&a->dlg))
        ps3ui_update_dialog(a);
    else
        switch (a->scr) {
        case PS3UI_SCR_SIGNIN: ps3ui_update_signin(a); break;
        case PS3UI_SCR_TWITCH:
            if (ps3ui_hit(a, PS3UI_PAD_CIRCLE)) {
                ps3ui_post(a, NETPLAY_CMD_TWITCH_CANCEL);
                ps3ui_app_go(a, PS3UI_SCR_SIGNIN);
            }
            break;
        case PS3UI_SCR_OSK: ps3ui_update_osk(a); break;
        case PS3UI_SCR_MENU: ps3ui_update_menu(a); break;
        case PS3UI_SCR_RULE: ps3ui_update_rule(a); break;
        case PS3UI_SCR_CONNECT: ps3ui_update_connect(a); break;
        case PS3UI_SCR_SEARCH: ps3ui_update_search(a); break;
        case PS3UI_SCR_ROOM: ps3ui_update_room(a); break;
        case PS3UI_SCR_VS: ps3ui_update_vs(a); break;
        case PS3UI_SCR_RESULT: ps3ui_update_result(a); break;
        default: break;
        }
    /* READY effects start when a fighter's flag goes up */
    if (a->scr == PS3UI_SCR_VS) {
        int f[2];
        ps3ui_fighters(&a->st, f);
        for (int i = 0; i < 2; i++) {
            int r = f[i] >= 0 && (a->st.members[f[i]].data.flags & ROOM_MEMBER_READY);
            if (r && !a->last_ready[i])
                a->ready_age[i] = 0.0f;
            else if (r)
                a->ready_age[i] += 1.0f;
            a->last_ready[i] = r;
        }
    }
    ps3ui_app_windows(a);
    ps3ui_win_tick(&a->main);
    ps3ui_win_tick(&a->sub);
    ps3ui_win_tick(&a->msg);
    ps3ui_win_tick(&a->timer);
    ps3ui_win_tick(&a->bar);
    a->bg_t = a->bg_t + 1.0f > 249.0f ? 10.0f : a->bg_t + 1.0f;
    a->cursor_t = a->cursor_t + 1.0f >= 180.0f ? 0.0f : a->cursor_t + 1.0f;
}

/* ---- screens: draw --------------------------------------------------------------- */

/* The row cursor (cursor_cmn01_46 / cursor_search) at a window edge. */
static void ps3ui_draw_cursor(ps3ui_canvas_t *cv, const ps3ui_scene_t *scene, const char *comp, float x, float y,
                              float t)
{
    ps3ui_slots_t none = { 0 };
    ps3ui_play(cv, scene, comp, t, ps3ui_mat_translate(x, y), &none);
}

/* A choice_win menu: title, centred rows, the cursor on the current row. A row
 * with a value (`values` set, and non-NULL for that row) is "label:"
 * right-aligned at the centre and its value left of centre + 64, with arrows,
 * as the RULE MENU and Matching range rows are drawn. */
static void ps3ui_draw_menu(ps3ui_canvas_t *cv, ps3ui_app_t *a, const char *title, const char *const *rows, int n,
                            const char *const *values)
{
    ps3ui_slots_t s = { 0 };
    ps3ui_win_draw(cv, &a->main, 0, 0, &s);
    float ex, ey, lx, ly, rx, ry, tx, ty;
    if (!ps3ui_slot_xy(&s, "p_win_edg_lt", 0, 0, &ex, &ey) || !ps3ui_slot_xy(&s, "p_txt_01_lt", 0, 0, &lx, &ly)
        || !ps3ui_slot_xy(&s, "p_txt_03_rt", 1, 0, &rx, &ry))
        return;
    float alpha = ps3ui_slot_alpha(&s, "p_txt_01_lt");
    if (a->main.state == PS3UI_WIN_IDLE)
        ps3ui_draw_cursor(cv, &ps3ui_n_cmn_base, "cursor_cmn01_46", ex, ey + 54.0f * (float)a->cursor, a->cursor_t);
    ps3ui_text_style_t st = ps3ui_style_text(37.0f);
    float cx = (lx + rx) * 0.5f;
    for (int i = 0; i < n; i++) {
        float y = ly + 54.0f * (float)i;
        if (values && values[i]) {
            char label[80];
            snprintf(label, sizeof label, "%s:", rows[i]);
            ps3ui_text_right(cv, &st, cx, y, label, alpha);
            char v[80];
            snprintf(v, sizeof v, i == a->cursor ? "< %s >" : "%s", values[i]);
            ps3ui_text_left(cv, &st, cx + 64.0f - (i == a->cursor ? ps3ui_text_width(&st, "< ") : 0.0f), y, v,
                            alpha);
        } else {
            ps3ui_text_centre(cv, &st, cx, y, rows[i], alpha);
        }
    }
    if (title && ps3ui_slot_xy(&s, "head_tit_ct", 0.5f, 0, &tx, &ty)) {
        ps3ui_text_style_t ts = ps3ui_style_title(40.0f);
        ps3ui_text(cv, &ts, tx - ps3ui_text_width(&ts, title) * 0.5f, ty + 47.0f, title,
                   ps3ui_slot_alpha(&s, "head_tit_ct"));
    }
}

/* The bottom message box (cmn_win_b_01). */
static void ps3ui_draw_message(ps3ui_canvas_t *cv, ps3ui_app_t *a, const char *text)
{
    ps3ui_slots_t s = { 0 };
    ps3ui_win_draw(cv, &a->msg, 0, 0, &s);
    float x0, y0, x1, y1;
    if (!ps3ui_slot_xy(&s, "p_txt_01_lt", 0, 0, &x0, &y0) || !ps3ui_slot_xy(&s, "p_txt_02_rb", 1, 1, &x1, &y1))
        return;
    ps3ui_text_style_t st = ps3ui_style_text(37.0f);
    ps3ui_text_box(cv, &st, x0, y0, x1, y1, text, ps3ui_slot_alpha(&s, "p_txt_01_lt"), 0);
}

/* The status window (connect_win), its text wrapped and centred in the box. */
static void ps3ui_draw_status(ps3ui_canvas_t *cv, ps3ui_app_t *a, const char *text)
{
    ps3ui_slots_t s = { 0 };
    ps3ui_win_draw(cv, &a->main, 0, 0, &s);
    float x0, y0, x1, y1;
    if (!ps3ui_slot_xy(&s, "p_connect_01_lt", 0, 0, &x0, &y0) || !ps3ui_slot_xy(&s, "p_connect_02_rb", 1, 1, &x1, &y1))
        return;
    ps3ui_text_style_t st = ps3ui_style_text(37.0f);
    ps3ui_text_box(cv, &st, x0, y0, x1, y1, text, ps3ui_slot_alpha(&s, "p_connect_01_lt"), 1);
}

static void ps3ui_draw_search(ps3ui_canvas_t *cv, ps3ui_app_t *a)
{
    ps3ui_slots_t s = { 0 };
    ps3ui_win_draw(cv, &a->main, 0, 0, &s);
    float ex, ey, px, py, tx, ty;
    float alpha = ps3ui_slot_alpha(&s, "p_player1_lt");
    if (ps3ui_slot_xy(&s, "head_tit_ct", 0.5f, 0, &tx, &ty)) {
        ps3ui_text_style_t ts = ps3ui_style_title(40.0f);
        ps3ui_text(cv, &ts, tx - ps3ui_text_width(&ts, ps3ui_str_title) * 0.5f, ty + 47.0f, ps3ui_str_title,
                   ps3ui_slot_alpha(&s, "head_tit_ct"));
    }
    if (ps3ui_slot_xy(&s, "p_match_area_c", 0.5f, 0.5f, &tx, &ty)) {
        ps3ui_text_style_t st = ps3ui_style_text(26.0f);
        ps3ui_text_centre(cv, &st, tx, ty - 34.0f, "Rooms", alpha);
        char n[16];
        snprintf(n, sizeof n, "%d", a->list_n);
        ps3ui_text_centre(cv, &st, tx, ty + 4.0f, n, alpha);
    }
    if (a->list_n && a->main.state == PS3UI_WIN_IDLE && ps3ui_slot_xy(&s, "p_win_edg_lt", 0, 0, &ex, &ey))
        ps3ui_draw_cursor(cv, &ps3ui_n_cmn_online, "cursor_search", ex, ey + 74.0f * (float)a->cursor, a->cursor_t);
    ps3ui_text_style_t name = ps3ui_style_name(), st = ps3ui_style_text(37.0f);
    for (int i = 0; i < a->list_n; i++) {
        char slot[16];
        snprintf(slot, sizeof slot, "p_player%d_lt", i + 1);
        if (!ps3ui_slot_xy(&s, slot, 0, 0, &px, &py))
            continue;
        const rpcn_room_listing_t *r = &a->st.rooms[a->list_idx[i]];
        int ping = r->relay_ms ? (int)r->relay_ms : PS3UI_PING_UNKNOWN;
        ps3ui_draw_image(cv, ps3ui_sprite(ps3ui_net_icon(ping)), ps3ui_mat_translate(px, py), alpha,
                         PS3UI_BLEND_NORMAL, NULL);
        ps3ui_text(cv, &name, px + 240.0f, py + 2.0f + 42.0f, r->owner, alpha);
        char count[16];
        snprintf(count, sizeof count, "%u / %u", r->cur_members, r->max_slots);
        ps3ui_text_right(cv, &st, px + 800.0f, py + 2.0f, count, alpha);
    }
    /* details of the highlighted room */
    if (a->list_n && ps3ui_slot_xy(&s, "p_txt_01_lt", 0, 0, &px, &py) && ps3ui_slot_xy(&s, "p_txt_03_rt", 1, 0, &tx, &ty)) {
        const rpcn_room_listing_t *r = &a->st.rooms[a->list_idx[a->cursor]];
        ps3ui_text_style_t lab = ps3ui_style_text(26.0f), val = ps3ui_style_text(29.0f);
        const char *why = netplay_room_reject_reason(r->flag_attr, g_active_profile);
        char delay[16], players[16];
        snprintf(delay, sizeof delay, "%u", (r->flag_attr >> NETPLAY_ROOM_DELAY_SHIFT) & NETPLAY_ROOM_DELAY_MASK);
        snprintf(players, sizeof players, "%u", r->max_slots);
        const char *labels[4] = { "Players", "Frame delay", "Game type", "Entry" };
        const char *values[4] = { players, delay, why ? "Other version" : "Sonic the Fighters",
                                  r->has_password ? "Private" : "Open" };
        for (int i = 0; i < 4; i++) {
            ps3ui_text_left(cv, &lab, px, py + 72.0f * (float)i, labels[i], alpha);
            ps3ui_text_right(cv, &val, tx, ty + 68.0f * (float)i, values[i], alpha);
        }
    }
}

/* The ROOM MATCH list: members in the search list's rows. */
static void ps3ui_draw_room_list(ps3ui_canvas_t *cv, ps3ui_app_t *a)
{
    const netplay_status_t *st = &a->st;
    ps3ui_slots_t s = { 0 }, ts = { 0 };
    ps3ui_win_draw(cv, &a->main, 0, 0, &s);
    ps3ui_win_draw(cv, &a->timer, 43.0f, 27.0f, &ts);
    float alpha = ps3ui_slot_alpha(&s, "p_player1_lt"), tx, ty, px, py, ex, ey;
    if (ps3ui_slot_xy(&s, "head_tit_ct", 0.5f, 0, &tx, &ty)) {
        ps3ui_text_style_t t = ps3ui_style_title(40.0f);
        ps3ui_text(cv, &t, tx - ps3ui_text_width(&t, ps3ui_str_title) * 0.5f, ty + 47.0f, ps3ui_str_title,
                   ps3ui_slot_alpha(&s, "head_tit_ct"));
    }
    if (ps3ui_slot_xy(&ts, "p_timer_c", 0.5f, 0.5f, &tx, &ty) && st->auto_start_s) {
        char buf[8];
        snprintf(buf, sizeof buf, "%02u", st->auto_start_s > 99 ? 99 : st->auto_start_s);
        ps3ui_text_style_t t = ps3ui_style_title(40.0f);
        ps3ui_text(cv, &t, tx - ps3ui_text_width(&t, buf) * 0.5f, ty + 20.0f, buf, ps3ui_slot_alpha(&ts, "p_timer_c"));
    }
    if (a->main.state == PS3UI_WIN_IDLE && ps3ui_slot_xy(&s, "p_win_edg_lt", 0, 0, &ex, &ey))
        ps3ui_draw_cursor(cv, &ps3ui_n_cmn_online, "cursor_search", ex, ey + 74.0f * (float)a->cursor, a->cursor_t);
    ps3ui_text_style_t name = ps3ui_style_name(), tag = ps3ui_style_text(26.0f);
    int f[2];
    ps3ui_fighters(st, f);
    for (int i = 0; i < PS3UI_ROWS; i++) {
        char slot[16];
        snprintf(slot, sizeof slot, "p_player%d_lt", i + 1);
        if (!ps3ui_slot_xy(&s, slot, 0, 0, &px, &py))
            continue;
        if ((uint32_t)i >= st->max_slot) {
            /* a row past the room's size: covered */
            ps3ui_fill_rect(cv, px - 37.0f, py - 11.0f, px - 37.0f + 880.0f, py - 11.0f + 70.0f, 0x000000, 0.5f * alpha,
                            PS3UI_BLEND_NORMAL);
            continue;
        }
        if ((uint32_t)i >= st->member_count)
            continue;
        const netplay_member_status_t *m = &st->members[i];
        if (!m->is_me)
            ps3ui_draw_image(cv, ps3ui_sprite(ps3ui_net_icon(m->rtt_ms)), ps3ui_mat_translate(px, py), alpha,
                             PS3UI_BLEND_NORMAL, NULL);
        ps3ui_text(cv, &name, px + 240.0f, py + 2.0f + 42.0f, m->npid, alpha);
        const char *t = NULL;
        uint32_t rgb = 0xFF0000;
        if (i == f[0] || i == f[1]) {
            t = i == f[0] ? "1P" : "2P";
            rgb = i == f[0] ? 0xFF0000 : 0x0000FF;
        } else if (m->data.entry) {
            t = m->data.entry == 1 ? "ENTRY 1P" : "ENTRY 2P";
            rgb = m->data.entry == 1 ? 0xFF0000 : 0x0000FF;
        }
        if (t) {
            tag.rgb = rgb;
            ps3ui_text_centre(cv, &tag, px + 750.0f, py + 24.0f - 21.0f, t, alpha);
        }
    }
    const char *msg = st->member_count >= 2 ? "Now accepting match entries. The top players in 1P Entry and 2P Entry get priority."
                                            : "Please wait.";
    ps3ui_draw_message(cv, a, msg);
}

static const char *ps3ui_hints(const ps3ui_app_t *a);

static void ps3ui_draw_vs(ps3ui_canvas_t *cv, ps3ui_app_t *a)
{
    const netplay_status_t *st = &a->st;
    int f[2];
    ps3ui_fighters(st, f);
    ps3ui_vs_lobby_t v = { 0 };
    v.title = ps3ui_str_title;
    for (int i = 0; i < 2; i++) {
        v.ping_ms[i] = PS3UI_PING_NONE;
        if (f[i] < 0)
            continue;
        const netplay_member_status_t *m = &st->members[f[i]];
        v.name[i] = m->npid;
        v.ping_ms[i] = m->is_me ? PS3UI_PING_NONE : m->rtt_ms;
        v.ready[i] = (m->data.flags & ROOM_MEMBER_READY) != 0;
        v.ready_age[i] = a->ready_age[i];
    }
    v.countdown = st->state == NETPLAY_IN_ROOM ? (int)(a->countdown / 60.0f) : 0;
    v.left_hint = "SELECT button:Controls";
    v.hints = ps3ui_hints(a);
    v.frame = a->main.t;
    ps3ui_draw_vs_lobby(cv, &v);    /* background, bar and hints included */
}

/* The result (vs_end_menu_PS3): WINNER / LOSER over each side, the names. */
static void ps3ui_draw_result(ps3ui_canvas_t *cv, ps3ui_app_t *a)
{
    ps3ui_slots_t s = { 0 }, ts = { 0 };
    ps3ui_win_draw(cv, &a->main, 0, 0, &s);
    ps3ui_win_draw(cv, &a->timer, 0, 0, &ts);
    int f[2];
    ps3ui_fighters(&a->st, f);
    static const char *const head[2] = { "p_end1p_head_ct", "p_end2p_head_ct" };
    static const char *const txt[2] = { "p_end1p_txt_lt", "p_end2p_txt_lt" };
    for (int i = 0; i < 2; i++) {
        float x, y;
        float al = ps3ui_slot_alpha(&s, head[i]);
        if (a->result_side >= 0 && ps3ui_slot_xy(&s, head[i], 0.5f, 0, &x, &y)) {
            const char *w = a->result_side == i ? "WINNER" : "LOSER";
            ps3ui_text_style_t t = ps3ui_style_title(40.0f);
            float tw = ps3ui_text_width(&t, w);
            ps3ui_text(cv, &t, x - tw * 0.5f, y + 47.0f + 3.0f, w, al);    /* white, 3 lower */
            t.rgb = a->result_side == i ? 0x792323 : 0x0A4A84;
            ps3ui_text(cv, &t, x - tw * 0.5f, y + 47.0f, w, al);
        }
        if (f[i] >= 0 && ps3ui_slot_xy(&s, txt[i], 0, 0, &x, &y)) {
            ps3ui_text_style_t n = ps3ui_style_name();
            ps3ui_text(cv, &n, x + 84.0f, y + 42.0f, a->st.members[f[i]].npid, ps3ui_slot_alpha(&s, txt[i]));
        }
    }
    float x, y;
    if (ps3ui_slot_xy(&ts, "p_timer_c", 0.5f, 0.5f, &x, &y)) {
        char buf[8];
        int left = (int)((600.0f - a->result_t) / 60.0f);
        snprintf(buf, sizeof buf, "%02d", left < 0 ? 0 : left);
        ps3ui_text_style_t t = ps3ui_style_title(40.0f);
        ps3ui_text(cv, &t, x - ps3ui_text_width(&t, buf) * 0.5f, y + 20.0f, buf, ps3ui_slot_alpha(&ts, "p_timer_c"));
    }
    float lx, ly, rx, ry;
    if (ps3ui_slot_xy(&s, "p_txt_01_lt", 0, 0, &lx, &ly) && ps3ui_slot_xy(&s, "p_txt_02_rb", 1, 1, &rx, &ry)) {
        ps3ui_text_style_t st = ps3ui_style_text(37.0f);
        float ex, ey;
        if (ps3ui_slot_xy(&s, "p_win_edg_lt", 0, 0, &ex, &ey) && a->main.state == PS3UI_WIN_IDLE)
            ps3ui_draw_cursor(cv, &ps3ui_n_cmn_base, "cursor_cmn01_46", ex, ey, a->cursor_t);
        ps3ui_text_centre(cv, &st, (lx + rx) * 0.5f, ly, "Return to setup screen", ps3ui_slot_alpha(&s, "p_txt_01_lt"));
    }
}

/* Our on-screen keyboard, in the same window language: a large common window,
 * the field being typed as its title line, keys laid on the menu grid. */
static void ps3ui_draw_osk(ps3ui_canvas_t *cv, ps3ui_app_t *a)
{
    ps3ui_slots_t s = { 0 };
    ps3ui_win_draw(cv, &a->main, 0, 0, &s);
    float ex, ey, lx, ly, rx, ry, tx, ty;
    if (!ps3ui_slot_xy(&s, "p_win_edg_lt", 0, 0, &ex, &ey) || !ps3ui_slot_xy(&s, "p_txt_01_lt", 0, 0, &lx, &ly)
        || !ps3ui_slot_xy(&s, "p_txt_03_rt", 1, 0, &rx, &ry))
        return;
    float alpha = ps3ui_slot_alpha(&s, "p_txt_01_lt");
    ps3ui_text_style_t st = ps3ui_style_text(37.0f);
    /* row 0: the field */
    char shown[sizeof a->osk_text[1]];
    if (a->osk_field == 1) {
        size_t n = strlen(a->osk_text[1]);
        memset(shown, '*', n);
        shown[n] = 0;
    } else {
        snprintf(shown, sizeof shown, "%s", a->osk_text[a->osk_field]);
    }
    static const char *const label[3] = { "Online ID", "Password", "E-mail token" };
    char line[sizeof shown + 32];
    snprintf(line, sizeof line, "%s: %s_", label[a->osk_field], shown);
    ps3ui_text_centre(cv, &st, (lx + rx) * 0.5f, ly, line, alpha);
    /* rows 1..4: keys; row 5: actions */
    float cell = (rx - lx) / 10.0f;
    if (a->main.state == PS3UI_WIN_IDLE) {
        float y = ey + 54.0f * (float)(a->osk_row + 1);
        int cols = a->osk_row == 4 ? PS3UI_OSK_ACTS : 10;
        float w = (rx - lx) / (float)cols;
        ps3ui_fill_rect(cv, lx + w * (float)a->osk_col, y + 2.0f, lx + w * (float)(a->osk_col + 1), y + 52.0f,
                        0x00BAFF, 0.3f * alpha, PS3UI_BLEND_ADD);
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 10; c++) {
            char k[2] = { ps3ui_osk_rows[a->osk_shift][r][c], 0 };
            ps3ui_text_centre(cv, &st, lx + cell * ((float)c + 0.5f), ly + 54.0f * (float)(r + 1), k, alpha);
        }
    float aw = (rx - lx) / (float)PS3UI_OSK_ACTS;
    for (int c = 0; c < PS3UI_OSK_ACTS; c++)
        ps3ui_text_centre(cv, &st, lx + aw * ((float)c + 0.5f), ly + 54.0f * 5.0f, ps3ui_osk_act[c], alpha);
    if (ps3ui_slot_xy(&s, "head_tit_ct", 0.5f, 0, &tx, &ty)) {
        ps3ui_text_style_t ts = ps3ui_style_title(40.0f);
        const char *t = "SIGN IN";
        ps3ui_text(cv, &ts, tx - ps3ui_text_width(&ts, t) * 0.5f, ty + 47.0f, t, ps3ui_slot_alpha(&s, "head_tit_ct"));
    }
}

/* The hints for the screen we are on (the PS3's help strings). */
static const char *ps3ui_hints(const ps3ui_app_t *a)
{
    if (a->dialog || ps3ui_dialog_showing(&a->dlg))
        return "\x02:Enter";                  /* the info window's own hint */
    int ready = (a->st.me.flags & ROOM_MEMBER_READY) != 0;
    switch (a->scr) {
    case PS3UI_SCR_SIGNIN:
    case PS3UI_SCR_MENU:
        return "\x01:Back  \x02:Enter";
    case PS3UI_SCR_RULE:
        return "\x01:Back  \x02:Create";
    case PS3UI_SCR_OSK:
        return "\x04:Delete  \x03:Space  \x01:Back  \x02:Enter";
    case PS3UI_SCR_TWITCH:
        return "\x01:Back";
    case PS3UI_SCR_CONNECT:
        return a->searching ? "\x01:Back" : NULL;
    case PS3UI_SCR_SEARCH:
        return a->list_n ? "\x04:Update list  \x01:Back  \x02:Enter" : "\x04:Update list  \x01:Back";
    case PS3UI_SCR_ROOM:
        return a->st.member_count >= 3 ? (a->st.is_host ? "\x04:Skip  \x01:Exit  \x02:Match entry"
                                                        : "\x01:Exit  \x02:Match entry")
                                       : "\x01:Exit";
    case PS3UI_SCR_VS:
        return ready ? "" : "\x01:Exit  \x02:Ready";
    case PS3UI_SCR_RESULT:
        return "\x02:Enter";
    default:
        return NULL;
    }
}

static void ps3ui_app_draw(ps3ui_app_t *a, ps3ui_canvas_t *cv)
{
    if (!ps3ui_app_visible(a))
        return;
    if (a->scr == PS3UI_SCR_VS) {
        ps3ui_draw_vs(cv, a);
    } else {
        ps3ui_draw_bg(cv, a->bg_t);
        switch (a->scr) {
        case PS3UI_SCR_SIGNIN: {
            static const char *const rows[4] = { "Sign in with Twitch", "Sign in with an RPCN account", "Server",
                                                 "Back" };
            const char *values[4] = { NULL, NULL, ps3ui_server_label(a), NULL };
            ps3ui_draw_menu(cv, a, "ONLINE BATTLE", rows, 4, values);
            ps3ui_draw_message(cv, a, a->cursor == 0
                ? "Twitch sign-in is on the sonicthefighte.rs server."
                : ps3ui_on_community(a)
                ? "sonicthefighte.rs: this emulator's own rooms, with Twitch sign-in."
                : netplay_server_is_official(a->cfg.server)
                ? "The official RPCN server (np.rpcs3.net), shared with RPCS3's players."
                : "Sign in to RPCN to play online.");
            break;
        }
        case PS3UI_SCR_TWITCH: {
            char t[512];
            if (a->st.twitch_user_code[0])
                snprintf(t, sizeof t, "Go to %s and enter the code\n%s", a->st.twitch_uri[0] ? a->st.twitch_uri
                                                                              : "twitch.tv/activate",
                         a->st.twitch_user_code);
            else
                snprintf(t, sizeof t, "Accessing...");
            ps3ui_draw_status(cv, a, t);
            break;
        }
        case PS3UI_SCR_OSK: ps3ui_draw_osk(cv, a); break;
        case PS3UI_SCR_MENU: ps3ui_draw_menu(cv, a, ps3ui_str_title, ps3ui_str_menu, 5, NULL); break;
        case PS3UI_SCR_RULE: {
            static const char *const rows[4] = { "Players", "Game type", "Frame delay", "Damage" };
            char p[8], d[16];
            snprintf(p, sizeof p, "%d", a->rule_players);
            if (a->rule_delay)
                snprintf(d, sizeof d, "%d", a->rule_delay);
            else
                snprintf(d, sizeof d, "Auto");
            const char *values[4] = { p, a->rule_vs ? "VS (rematch)" : "Arcade", d,
                                      a->rule_damage_real ? "REAL" : "NORMAL" };
            ps3ui_draw_menu(cv, a, "RULE MENU", rows, ps3ui_rule_rows(a), values);
            break;
        }
        case PS3UI_SCR_CONNECT:
            ps3ui_draw_status(cv, a, a->searching ? "Searching for sessions..." : "Accessing...");
            break;
        case PS3UI_SCR_SEARCH:
            ps3ui_draw_search(cv, a);
            ps3ui_draw_message(cv, a, a->list_n ? "Select a session." : "No sessions were found.");
            break;
        case PS3UI_SCR_ROOM:
            if (a->st.member_count >= 3)
                ps3ui_draw_room_list(cv, a);
            else
                ps3ui_draw_status(cv, a, "Waiting for an opponent.");
            break;
        case PS3UI_SCR_RESULT: ps3ui_draw_result(cv, a); break;
        default: break;
        }
        ps3ui_slot_t bar = ps3ui_draw_hint_bar(cv, a->bar.t);
        ps3ui_draw_hint_text(cv, &bar, ps3ui_hints(a));
    }
    if (ps3ui_dialog_showing(&a->dlg)) {
        ps3ui_dialog_draw(cv, &a->dlg, a->cursor_t);
        ps3ui_slot_t bar = ps3ui_draw_hint_bar(cv, a->bar.t);
        ps3ui_draw_hint_text(cv, &bar, ps3ui_hints(a));
    }
}

#endif /* PS3UI_APP_H */
