/*
 * pad_lobby.h -- the RPCN netplay lobby for a host with a gamepad and no ImGui:
 * a menu drawn with sokol_debugtext over (or instead of) the game.
 *
 * Shared by the handheld frontend (main_sdl.c) and the libretro core
 * (main_libretro.c, "Online play: RPCN"). It reads netplay.h's status snapshot
 * and posts commands, and never touches netplay state itself; whoever runs the
 * board pumps the session (emu_netplay_pump), as everywhere.
 *
 * The host owns the input: it calls lobby_move / lobby_activate / lobby_show
 * from its own buttons, lobby_update once per rendered frame and lobby_draw
 * inside its swapchain pass, then sdtx_draw.
 *
 * There is no keyboard to type an account into, so sign-in is whatever the
 * settings file holds: in practice the PC's file copied across, whose Twitch
 * login token stands in for a password. RPCN allows one session per account, so
 * the PC's emulator and this device cannot both be signed in as the same one.
 */
#ifndef PAD_LOBBY_H
#define PAD_LOBBY_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sokol_gfx.h"
#include "sokol_debugtext.h"
#include "net/netplay.h"

#ifndef NETPLAY_DEFAULT_SERVER
#define NETPLAY_DEFAULT_SERVER "rpcn.sonicthefighte.rs"
#endif

typedef enum {
    LB_SIGN_IN, LB_TWITCH, LB_TWITCH_CANCEL, LB_CLOSE, LB_DISCONNECT, LB_HOST,
    LB_REFRESH, LB_JOIN, LB_START, LB_LEAVE_ROOM, LB_STOP,
} lobby_act_t;

typedef struct {
    lobby_act_t act;
    uint64_t    room;
    bool        enabled;
    char        text[64];
} lobby_row_t;

#define LOBBY_ROWS (RPCN_MAX_ROOMS + 8)

static struct {
    bool             open;
    int              sel;
    int              pressed;       /* A or B went down in the lobby; it acts on release */
    bool             have_cfg;      /* the settings file named a server or an account */
    netplay_config_t cfg;
    int              last_state;    /* -1 until the first snapshot */
    uint64_t           next_search;
    uint64_t           match_start;   /* when PLAYING began, for the "vs" banner */
    netplay_status_t st;
    lobby_row_t      rows[LOBBY_ROWS];
    int              nrows;
    /* Set by the host before lobby_init. */
    bool             net_host;      /* host a room as soon as the sign-in completes (once) */
    int              net_delay;     /* frames of input delay; 0 = the stored value */
    const char      *net_room_pass; /* lock the rooms hosted here with it */
    void           (*take_pad)(void);   /* the lobby opened: let go of what the game holds */
    /* The host signs in and out itself (the libretro core, from its core
     * options): the lobby only offers rooms, and says where signing in is. */
    bool             external_login;
    const char      *login_hint;    /* shown while signed out, with external_login */
} g_lobby = { .last_state = -1, .pressed = -1 };

static void lobby_move(int dir);

static void lobby_show(bool open) {
    if (open && !g_lobby.open && g_lobby.take_pad) g_lobby.take_pad();   /* the game sees the pad let go */
    g_lobby.open = open;
    g_lobby.sel  = 0;
}

static void lobby_init(void) {
    netplay_init();
    netplay_set_open_browser(false);   /* the code and the address are on screen */

    g_lobby.have_cfg = netplay_stored_settings(&g_lobby.cfg);
    netplay_config_t *c = &g_lobby.cfg;
    if (!c->server[0]) snprintf(c->server, sizeof c->server, "%s", NETPLAY_DEFAULT_SERVER);
    if (!c->port)        c->port = RPCN_DEFAULT_PORT;
    if (!c->frame_delay) c->frame_delay = 2;
    if (g_lobby.net_delay > 0) c->frame_delay = (uint32_t)g_lobby.net_delay;
    if (g_lobby.net_room_pass) snprintf(c->room_password, sizeof c->room_password, "%s", g_lobby.net_room_pass);
    lobby_show(true);
}

/* A stored login signs straight in: choosing netplay was the request. */
static bool lobby_can_sign_in(void) {
    return g_lobby.cfg.twitch_token[0] || (g_lobby.cfg.npid[0] && g_lobby.cfg.password[0]);
}

/* Sign in as the stored account: the Twitch login token when there is one (the
 * netplay window's Connect does the same), otherwise the stored password. */
static void lobby_sign_in(void) {
    netplay_config_t c = g_lobby.cfg;
    if (c.twitch_token[0]) {
        c.npid[0] = '\0';   /* the token's owner signs in, whoever else was stored */
        netplay_post(NETPLAY_CMD_TWITCH_START, &c);
    } else {
        netplay_post(NETPLAY_CMD_CONNECT, &c);
    }
}

static void lobby_row(lobby_act_t act, bool enabled, uint64_t room, const char *fmt, ...) {
    if (g_lobby.nrows >= LOBBY_ROWS) return;
    lobby_row_t *r = &g_lobby.rows[g_lobby.nrows++];
    r->act = act;
    r->room = room;
    r->enabled = enabled;
    va_list args;
    va_start(args, fmt);
    vsnprintf(r->text, sizeof r->text, fmt, args);
    va_end(args);
}

/* Once per rendered frame: take a snapshot, react to state changes and rebuild
 * the menu for the state the session is in. */
static void lobby_update(uint64_t now) {
    netplay_get_status(&g_lobby.st);
    const netplay_status_t *st = &g_lobby.st;

    if ((int)st->state != g_lobby.last_state) {
        int was = g_lobby.last_state;
        g_lobby.last_state = (int)st->state;
        g_lobby.sel = 0;
        if (st->state == NETPLAY_PLAYING) {
            g_lobby.match_start = now;
            lobby_show(false);
        } else if (was == NETPLAY_PLAYING) {
            lobby_show(true);   /* the match ended: say why, and what next */
        }
        if (st->state == NETPLAY_ONLINE) g_lobby.next_search = 0;   /* list the rooms now */
        if (st->state == NETPLAY_ONLINE && g_lobby.net_host) {
            g_lobby.net_host = false;   /* once: a later sign-in is the player's */
            netplay_post(NETPLAY_CMD_HOST, &g_lobby.cfg);
        }
    }
    if (st->state == NETPLAY_ONLINE && g_lobby.open && !st->search_pending
            && now >= g_lobby.next_search) {
        netplay_post(NETPLAY_CMD_SEARCH, &g_lobby.cfg);
        g_lobby.next_search = now + 5000000000ull;
    }

    g_lobby.nrows = 0;
    const netplay_config_t *c = &g_lobby.cfg;
    bool twitch_busy = st->twitch_state == RPCN_TWITCH_STARTING || st->twitch_state == RPCN_TWITCH_WAITING;
    switch (st->state) {
        case NETPLAY_OFF:
        case NETPLAY_FAILED:
            if (g_lobby.external_login) {
                lobby_row(LB_CLOSE, false, 0, "%s", g_lobby.login_hint ? g_lobby.login_hint : "Not signed in");
            } else if (twitch_busy) {
                lobby_row(LB_TWITCH_CANCEL, true, 0, "Cancel the Twitch sign-in");
            } else {
                if (c->twitch_token[0])
                    lobby_row(LB_SIGN_IN, true, 0, "Sign in as %s (Twitch)",
                              c->twitch_npid[0] ? c->twitch_npid : c->npid);
                else if (c->npid[0] && c->password[0])
                    lobby_row(LB_SIGN_IN, true, 0, "Sign in as %s", c->npid);
                if (!c->twitch_token[0])
                    lobby_row(LB_TWITCH, true, 0, "Sign in with Twitch");
            }
            lobby_row(LB_CLOSE, true, 0, "Play offline");
            break;
        case NETPLAY_CONNECTING:
            lobby_row(LB_DISCONNECT, true, 0, "Cancel");
            break;
        case NETPLAY_ONLINE:
            lobby_row(LB_HOST, true, 0, "Host a room (delay %u)", (unsigned)c->frame_delay);
            for (uint32_t i = 0; i < st->room_count; i++) {
                const rpcn_room_listing_t *r = &st->rooms[i];
                const char *why = netplay_room_reject_reason(r->flag_attr, g_active_profile);
                if (why)                              lobby_row(LB_JOIN, false, 0, "%.16s: other version", r->owner);
                else if (r->has_password)             lobby_row(LB_JOIN, false, 0, "%.16s: locked", r->owner);
                else if (r->cur_members >= r->max_slots) lobby_row(LB_JOIN, false, 0, "%.16s: full", r->owner);
                else lobby_row(LB_JOIN, true, r->room_id, "Join %.16s (delay %u)", r->owner,
                               (unsigned)((r->flag_attr >> NETPLAY_ROOM_DELAY_SHIFT) & NETPLAY_ROOM_DELAY_MASK));
            }
            lobby_row(LB_REFRESH, !st->search_pending, 0, st->search_pending ? "Searching..." : "Refresh the list");
            if (!g_lobby.external_login) lobby_row(LB_DISCONNECT, true, 0, "Sign out");
            lobby_row(LB_CLOSE, true, 0, "Back to the game");
            break;
        case NETPLAY_IN_ROOM:
            lobby_row(LB_START, st->peer_known, 0, st->peer_ready ? "Accept the match" : "Start the match");
            lobby_row(LB_LEAVE_ROOM, true, 0, "Leave the room");
            lobby_row(LB_CLOSE, true, 0, "Back to the game");
            break;
        case NETPLAY_SYNCING:
            lobby_row(LB_STOP, true, 0, "Cancel");
            lobby_row(LB_CLOSE, true, 0, "Back to the game");
            break;
        case NETPLAY_PLAYING:
            lobby_row(LB_CLOSE, true, 0, "Back to the match");
            lobby_row(LB_STOP, true, 0, "Leave the match");
            break;
    }
    if (g_lobby.sel >= g_lobby.nrows) g_lobby.sel = g_lobby.nrows - 1;
    if (g_lobby.sel < 0) g_lobby.sel = 0;
    /* Never rest on a row that cannot be picked ("Start" before anyone joins). */
    if (g_lobby.nrows && !g_lobby.rows[g_lobby.sel].enabled) lobby_move(+1);
}

static void lobby_move(int dir) {
    for (int k = 1; k <= g_lobby.nrows; k++) {
        int i = ((g_lobby.sel + dir * k) % g_lobby.nrows + g_lobby.nrows) % g_lobby.nrows;
        if (g_lobby.rows[i].enabled) { g_lobby.sel = i; return; }
    }
}

static void lobby_activate(void) {
    if (g_lobby.sel >= g_lobby.nrows || !g_lobby.rows[g_lobby.sel].enabled) return;
    const lobby_row_t *r = &g_lobby.rows[g_lobby.sel];
    netplay_config_t c = g_lobby.cfg;
    switch (r->act) {
        case LB_SIGN_IN:       lobby_sign_in(); break;
        case LB_TWITCH:        netplay_post(NETPLAY_CMD_TWITCH_START, &c); break;
        case LB_TWITCH_CANCEL: netplay_post(NETPLAY_CMD_TWITCH_CANCEL, &c); break;
        case LB_CLOSE:         lobby_show(false); break;
        case LB_DISCONNECT:    netplay_post(NETPLAY_CMD_DISCONNECT, &c); break;
        case LB_HOST:          netplay_post(NETPLAY_CMD_HOST, &c); break;
        case LB_REFRESH:       netplay_post(NETPLAY_CMD_SEARCH, &c); break;
        case LB_JOIN:          c.room_id = r->room; netplay_post(NETPLAY_CMD_JOIN, &c); break;
        case LB_START:         netplay_post(NETPLAY_CMD_START, &c); break;
        case LB_STOP:          netplay_post(NETPLAY_CMD_STOP, &c); break;
        case LB_LEAVE_ROOM:
            /* RPCN has no "leave" here: sign out and straight back in, in order. */
            netplay_post(NETPLAY_CMD_DISCONNECT, &c);
            lobby_sign_in();
            break;
    }
}

/* Text at cell (x, y), with the OSD's one-pixel shadow, wrapped at `cols`; with
 * draw false, only measured. Returns the row after the last one. */
static float lobby_text_ex(float x, float y, int cols, uint8_t r, uint8_t g, uint8_t b,
                           const char *s, bool draw) {
    char line[128];
    int width = cols - (int)x - 1;
    if (width < 8) width = 8;
    if (width > (int)sizeof line - 1) width = (int)sizeof line - 1;
    while (*s) {
        int n = (int)strlen(s);
        if (n > width) {
            n = width;
            for (int k = width; k > width / 2; k--) if (s[k] == ' ') { n = k; break; }
        }
        if (draw) {
            memcpy(line, s, (size_t)n);
            line[n] = '\0';
            sdtx_pos(x + 0.125f, y + 0.125f);
            sdtx_color3b(0, 0, 0);
            sdtx_puts(line);
            sdtx_pos(x, y);
            sdtx_color3b(r, g, b);
            sdtx_puts(line);
        }
        s += n;
        while (*s == ' ') s++;
        y += 1.0f;
    }
    return y;
}

static float lobby_text(float x, float y, int cols, uint8_t r, uint8_t g, uint8_t b, const char *s) {
    return lobby_text_ex(x, y, cols, r, g, b, s, true);
}

static void lobby_draw(int fb_w, int fb_h, uint64_t now_ns) {
    const netplay_status_t *st = &g_lobby.st;
    float scale = fb_h >= 720 ? 3.0f : 2.0f;
    int   cols  = (int)((float)fb_w / scale / 8.0f);
    char  buf[640];
    sg_apply_viewport(0, 0, fb_w, fb_h, true);
    sg_apply_scissor_rect(0, 0, fb_w, fb_h, true);
    sdtx_canvas((float)fb_w / scale, (float)fb_h / scale);
    sdtx_origin(0.0f, 0.0f);

    if (!g_lobby.open) {
        /* In a match: who against, for a few seconds, and a desync for good. */
        if (st->state == NETPLAY_PLAYING && now_ns - g_lobby.match_start < 5000000000ull) {
            snprintf(buf, sizeof buf, "P%d vs %s", st->local_player + 1, st->peer_npid);
            lobby_text(1.0f, 0.25f, cols, 120, 220, 255, buf);
        }
        if (st->state == NETPLAY_PLAYING && st->desync_frame != LOCKSTEP_NO_CHECK) {
            snprintf(buf, sizeof buf, "DESYNC at frame %u", st->desync_frame);
            lobby_text(1.0f, 1.25f, cols, 255, 90, 90, buf);
        }
        return;
    }

    float y = 1.0f;
    snprintf(buf, sizeof buf, "NETPLAY - %s", netplay_state_text(st->state));
    y = lobby_text(1.0f, y, cols, 255, 220, 120, buf);
    if (st->state >= NETPLAY_ONLINE && st->state != NETPLAY_FAILED) {
        snprintf(buf, sizeof buf, "Signed in as %s", g_lobby.cfg.twitch_token[0] && g_lobby.cfg.twitch_npid[0]
                 ? g_lobby.cfg.twitch_npid : g_lobby.cfg.npid);
        y = lobby_text(1.0f, y, cols, 170, 170, 170, buf);
    }
    y += 0.5f;

    if (!g_lobby.have_cfg && !g_lobby.external_login && st->state == NETPLAY_OFF && st->twitch_state == RPCN_TWITCH_IDLE) {
        snprintf(buf, sizeof buf, "No netplay settings. Copy netplay.cfg from the PC (%%APPDATA%%\\m2hle2) to %s",
                 netplay_cfg_path());
        y = lobby_text(1.0f, y, cols, 255, 255, 255, buf) + 0.5f;
    }
    if (st->twitch_state == RPCN_TWITCH_WAITING) {
        snprintf(buf, sizeof buf, "Twitch code: %s", st->twitch_user_code);
        y = lobby_text(1.0f, y, cols, 255, 255, 140, buf);
        snprintf(buf, sizeof buf, "Approve it at %s", st->twitch_uri);
        y = lobby_text(1.0f, y, cols, 255, 255, 255, buf);
        y = lobby_text(1.0f, y, cols, 170, 170, 170,
                       "(this signs the account's other devices out of Twitch)") + 0.5f;
    } else if (st->twitch_state == RPCN_TWITCH_STARTING) {
        y = lobby_text(1.0f, y, cols, 255, 255, 255, "Asking the server for a Twitch code...") + 0.5f;
    }
    if (st->state == NETPLAY_IN_ROOM || st->state == NETPLAY_SYNCING) {
        snprintf(buf, sizeof buf, "Room %llu - you are P%d (%s)", (unsigned long long)st->room_id,
                 st->local_player + 1, st->is_host ? "host" : "guest");
        y = lobby_text(1.0f, y, cols, 255, 255, 255, buf);
        snprintf(buf, sizeof buf, "Opponent: %s%s", st->peer_npid[0] ? st->peer_npid : "nobody yet",
                 !st->peer_npid[0] ? "" : st->peer_heard ? " (connected)" : " (connecting...)");
        y = lobby_text(1.0f, y, cols, 255, 255, 255, buf);
        if (st->peer_ready && st->state == NETPLAY_IN_ROOM) {
            snprintf(buf, sizeof buf, "%s is ready - accept to start", st->peer_npid[0] ? st->peer_npid : "The opponent");
            y = lobby_text(1.0f, y, cols, 120, 255, 140, buf);
        }
        if (st->state == NETPLAY_SYNCING)
            y = lobby_text(1.0f, y, cols, 255, 255, 140, "Waiting for the opponent to start...");
        y += 0.5f;
    }
    if (st->state == NETPLAY_ONLINE && st->room_count == 0 && !st->search_pending)
        y = lobby_text(1.0f, y, cols, 170, 170, 170, "No rooms yet - host one.") + 0.5f;

    for (int i = 0; i < g_lobby.nrows; i++) {
        const lobby_row_t *r = &g_lobby.rows[i];
        snprintf(buf, sizeof buf, "%s %s", i == g_lobby.sel ? ">" : " ", r->text);
        if (!r->enabled)            lobby_text(1.0f, y, cols, 110, 110, 110, buf);
        else if (i == g_lobby.sel)  lobby_text(1.0f, y, cols, 255, 255, 120, buf);
        else                        lobby_text(1.0f, y, cols, 230, 230, 230, buf);
        y += 1.0f;
    }

    /* The reason, when there is one, or the session's last word, just above the
     * key hint on the bottom row. */
    float rows_total = (float)fb_h / scale / 8.0f;
    float hint = rows_total - 1.5f;
    bool  failed = st->error[0] && (st->state == NETPLAY_FAILED || st->state == NETPLAY_OFF);
    const char *last = failed ? st->error
                     : st->log_count ? st->log[(st->log_count - 1) % NETPLAY_LOG_LINES] : "";
    float foot = hint - 0.5f - (lobby_text_ex(1.0f, 0.0f, cols, 0, 0, 0, last, false));
    if (foot < y + 0.5f) foot = y + 0.5f;
    if (failed) lobby_text(1.0f, foot, cols, 255, 110, 110, last);
    else        lobby_text(1.0f, foot, cols, 150, 150, 150, last);
    lobby_text(1.0f, hint, cols, 150, 150, 150, "A select  B close  L1+R1 lobby");
}

#endif /* PAD_LOBBY_H */
