/*
 * retro_lobby.h -- the RPCN lobby as libretro core options.
 *
 * The libretro core's netplay menu. RetroArch draws it -- its own theme, its
 * own font, its own controls, its own on-screen keyboard -- from the option
 * definitions the core hands it, and the core itself draws nothing at all.
 * ui/pad_lobby.h is the other half of the pair: that one is for a frontend that
 * HAS no menu (the SDL3 handheld build), and it is what used to be drawn here,
 * in the handheld's own bitmap font, over the top of a RetroArch that looks
 * nothing like it.
 *
 * The whole lobby is Quick Menu > Core Options > Online play:
 *
 *   Status        one line: what the session is doing, and as whom
 *   Room to join  the rooms RPCN last listed; the Join action takes this one
 *   Players       the line in the room we are in, read only
 *   Do            the actions this state allows. It fires once the value has
 *                 SETTLED (left and right walk a value list one step at a time
 *                 and every step is a value change, so acting at once would
 *                 fire every action passed on the way to the wanted one), and
 *                 is put back to "(nothing)" with SET_VARIABLE.
 *   Room size / Side to play / Take part    ordinary settings
 *
 * THE NUMBER OF OPTIONS NEVER CHANGES. A core may re-send its option table as
 * often as it likes -- which is the only way a room list reaches a frontend's
 * menu -- but only "as long as the number of options doesn't change from the
 * number given in the first call" (libretro.h, SET_CORE_OPTIONS_V2). So the
 * table is registered in full before a ROM is loaded, and an option with
 * nothing to say in this state is HIDDEN (SET_CORE_OPTIONS_DISPLAY), never
 * dropped.
 *
 * RE-SENDING THE TABLE WRITES THE FRONTEND'S OPTIONS FILE: RetroArch answers
 * SET_CORE_OPTIONS_V2 by deinitialising the option manager, which flushes the
 * .opt file, and initialising a new one from what we sent. So the table goes
 * out only when one of its strings actually changed (rl_signature), and nothing
 * that TICKS -- a countdown, a frame number, "searching..." -- may go into a
 * label, or a handheld's card would take a write a second. Those go out as
 * notifications (retro_message_ext) instead.
 *
 * RetroArch pauses the core while its menu is open, so the session is not
 * serviced there and what is picked in the menu happens when the menu closes.
 * That is said in the "Do" option's own description rather than worked around:
 * a core cannot ask a frontend to keep running it.
 *
 * It reads netplay.h's status snapshot and posts commands, and never touches
 * netplay state itself; whoever runs the board pumps the session
 * (emu_netplay_pump), as everywhere.
 */
#ifndef RETRO_LOBBY_H
#define RETRO_LOBBY_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro/libretro.h"
#include "net/netplay.h"

#ifndef NETPLAY_DEFAULT_SERVER
#define NETPLAY_DEFAULT_SERVER "rpcn.sonicthefighte.rs"
#endif

/* The core's own sign-in option: "Stop signing in" puts it back to signed out,
 * so the core's one sign-in path (lr_rpcn_apply_login) stays the only one. */
#define RL_LOGIN_KEY "m2hle_rpcn_login"

enum {
    RL_STATUS, RL_ROOM, RL_PLAYERS, RL_DO, RL_SIZE, RL_SIDE, RL_WATCH,
    RL_OPTION_COUNT
};

#define RL_MAX_VALUES (RPCN_MAX_ROOMS + 4)
#define RL_KEY_LEN    24
#define RL_LABEL_LEN  96
#define RL_INFO_LEN   320
/* How long a "Do" value must sit still before it is acted on. */
#define RL_SETTLE_US  400000
/* How often the room list is refreshed while signed in and out of a room. */
#define RL_SEARCH_US  5000000
/* The shortest gap between two sends of the table. Every send writes the
 * frontend's options file, and a room that fills, empties and is joined changes
 * several rows at once; this coalesces the burst, and puts a floor under what a
 * server answering a search in a different order each time can cost. */
#define RL_PUSH_US    500000

static const char *const rl_key_name[RL_OPTION_COUNT] = {
    "m2hle_rpcn_status", "m2hle_rpcn_room", "m2hle_rpcn_players", "m2hle_rpcn_do",
    "m2hle_room_size", "m2hle_room_side", "m2hle_room_watch",
};
static const char *const rl_desc[RL_OPTION_COUNT] = {
    "RPCN status", "RPCN room to join", "RPCN players in the room", "RPCN lobby action",
    "RPCN room size (hosting)", "RPCN side to play", "RPCN take part",
};
static const char *const rl_desc_cat[RL_OPTION_COUNT] = {
    "Status", "Room to join", "Players", "Do", "Room size (hosting)", "Side to play", "Take part",
};
static const char *const rl_info_text[RL_OPTION_COUNT] = {
    "What the RPCN session is doing. Sign in with \"RPCN sign-in\" above.",
    "The room \"Do: Join the room above\" takes. The list refreshes by itself while the game runs.",
    "Who is in the room, front of the line first, with the side they are on and their record.",
    "What to do next in the lobby. RetroArch pauses the game while this menu is open, so what you "
    "pick here happens when you close the menu; a message on screen then says what came of it.",
    "How many players a room hosted from this machine holds. Two fight, the rest wait in line and "
    "watch, and after every match the winner stays on.",
    "Which side to ask for when your turn comes. Either side is the shortest wait.",
    "Sit out to watch the room's matches without ever being picked to fight.",
};

static struct retro_core_option_v2_definition g_rlobby_defs[RL_OPTION_COUNT];

static struct {
    retro_environment_t env;
    void              (*say)(const char *msg, unsigned ms);   /* the core's notification */

    netplay_config_t cfg;
    netplay_status_t st;
    bool     have_cfg;      /* the settings file named a server or an account */
    bool     started;       /* rlobby_init has run: RPCN is the chosen online play */
    int      last_state;    /* -1 until the first snapshot */
    int64_t  next_search_us;
    uint64_t sig;           /* the strings as last sent to the frontend */
    bool     dirty;         /* ...and they have changed since */
    int64_t  push_at_us;    /* not before this, whatever has changed */
    int      shown;         /* the visibility mask last applied; -1 = none yet */

    char     act[RL_KEY_LEN];   /* the "Do" value waiting to settle */
    int64_t  act_at_us;
    uint64_t room_pick;         /* the room "Room to join" names; 0 = none */
    uint8_t  want_entry;        /* the menu's side and take-part, as last read */
    bool     want_watch;
    bool     read_once;         /* ...and those have been read at least once */

    char  key[RL_OPTION_COUNT][RL_MAX_VALUES][RL_KEY_LEN];
    char  label[RL_OPTION_COUNT][RL_MAX_VALUES][RL_LABEL_LEN];
    int   nval[RL_OPTION_COUNT];
    char  info[RL_INFO_LEN];    /* the status row's sublabel, when it has more to say */
} g_rlobby = { .last_state = -1, .shown = -1 };

/* ---- The frontend ------------------------------------------------------------ */

static const char *rl_var(const char *key) {
    struct retro_variable v = { key, NULL };
    return g_rlobby.env && g_rlobby.env(RETRO_ENVIRONMENT_GET_VARIABLE, &v) ? v.value : NULL;
}

static void rl_set_var(const char *key, const char *val) {
    struct retro_variable v = { key, val };
    if (g_rlobby.env) g_rlobby.env(RETRO_ENVIRONMENT_SET_VARIABLE, &v);
}

static void rl_say(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_rlobby.say) g_rlobby.say(buf, 4000);
}

/* ---- Building the table ------------------------------------------------------ */

static void rl_begin(int opt) { g_rlobby.nval[opt] = 0; }

static void rl_value(int opt, const char *key, const char *fmt, ...) {
    int n = g_rlobby.nval[opt];
    if (n >= RL_MAX_VALUES) return;
    snprintf(g_rlobby.key[opt][n], RL_KEY_LEN, "%s", key);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_rlobby.label[opt][n], RL_LABEL_LEN, fmt, ap);
    va_end(ap);
    g_rlobby.nval[opt] = n + 1;
}

/* Point the definition at the values just built and terminate it. The first
 * value is the default, so it is always the neutral one. */
static void rl_commit(int opt) {
    struct retro_core_option_v2_definition *d = &g_rlobby_defs[opt];
    int n = g_rlobby.nval[opt];
    if (n == 0) { rl_value(opt, "-", "-"); n = 1; }
    for (int i = 0; i < n; i++) {
        d->values[i].value = g_rlobby.key[opt][i];
        d->values[i].label = g_rlobby.label[opt][i];
    }
    d->values[n].value  = NULL;
    d->values[n].label  = NULL;
    d->key              = rl_key_name[opt];
    d->desc             = rl_desc[opt];
    d->desc_categorized = rl_desc_cat[opt];
    d->info             = rl_info_text[opt];
    d->info_categorized = NULL;
    d->category_key     = "online";
    d->default_value    = g_rlobby.key[opt][0];
}

static uint64_t rl_hash(uint64_t h, const char *s) {
    if (!s) return (h ^ 0xFFu) * 1099511628211ull;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
    return h * 1099511628211ull;
}

static uint64_t rl_signature(void) {
    uint64_t h = 1469598103934665603ull;
    for (int o = 0; o < RL_OPTION_COUNT; o++) {
        h = rl_hash(h, g_rlobby_defs[o].info);
        for (int i = 0; i < g_rlobby.nval[o]; i++) {
            h = rl_hash(h, g_rlobby.key[o][i]);
            h = rl_hash(h, g_rlobby.label[o][i]);
        }
    }
    return h;
}

/*
 * The table as it stands before any session exists -- what the core registers
 * in retro_set_environment, where the online mode is not known yet and the
 * count has to be right for good. The three plain settings are built here and
 * never rebuilt; only the four rows that follow a session move.
 */
static void rlobby_defaults(retro_environment_t env) {
    g_rlobby.env = env;   /* before any ROM: the rows are hidden from the first ask */
    rl_begin(RL_STATUS);  rl_value(RL_STATUS,  "status", "Signed out");
    rl_begin(RL_ROOM);    rl_value(RL_ROOM,    "-", "(none)");
    rl_begin(RL_PLAYERS); rl_value(RL_PLAYERS, "-", "(not in a room)");
    rl_begin(RL_DO);      rl_value(RL_DO,      "-", "(nothing)");

    rl_begin(RL_SIZE);
    for (unsigned n = 2; n <= ROOM_MAX_MEMBERS; n++) {
        char k[RL_KEY_LEN];
        snprintf(k, sizeof k, "%u", n);
        rl_value(RL_SIZE, k, "%u players", n);
    }
    rl_begin(RL_SIDE);
    rl_value(RL_SIDE, "either", "Whichever is free");
    rl_value(RL_SIDE, "1p",     "1P");
    rl_value(RL_SIDE, "2p",     "2P");

    rl_begin(RL_WATCH);
    rl_value(RL_WATCH, "play",  "Play");
    rl_value(RL_WATCH, "watch", "Watch only");

    for (int o = 0; o < RL_OPTION_COUNT; o++) rl_commit(o);
    g_rlobby.sig = rl_signature();
}

/* The four rows that follow the session. Called once a frame; it only marks the
 * table dirty when a string it built actually differs from the one sent. */
static void rl_build(void) {
    const netplay_status_t *st = &g_rlobby.st;
    const netplay_config_t *c  = &g_rlobby.cfg;
    const char *who = c->twitch_token[0] && c->twitch_npid[0] ? c->twitch_npid : c->npid;
    if (!who[0]) who = "?";

    /* ---- Status: one value, so the row reads as a line of text ---- */
    g_rlobby.info[0] = '\0';
    rl_begin(RL_STATUS);
    switch (st->state) {
        case NETPLAY_OFF:
            if (st->twitch_state == RPCN_TWITCH_WAITING)
                rl_value(RL_STATUS, "status", "Twitch: enter the code %s", st->twitch_user_code);
            else if (st->twitch_state == RPCN_TWITCH_STARTING)
                rl_value(RL_STATUS, "status", "Asking Twitch for a code");
            else
                rl_value(RL_STATUS, "status", "Signed out");
            break;
        case NETPLAY_CONNECTING:
            rl_value(RL_STATUS, "status", "Signing in");
            break;
        case NETPLAY_ONLINE:
            rl_value(RL_STATUS, "status", "%s: %u room%s", who,
                     (unsigned)st->room_count, st->room_count == 1 ? "" : "s");
            break;
        case NETPLAY_IN_ROOM:
            rl_value(RL_STATUS, "status", "Room %llu: %u of %u%s",
                     (unsigned long long)st->room_id, (unsigned)st->member_count,
                     (unsigned)st->max_slot, st->is_host ? ", you run it" : "");
            break;
        case NETPLAY_SYNCING:
            rl_value(RL_STATUS, "status", "Starting the match");
            break;
        case NETPLAY_PLAYING:
            rl_value(RL_STATUS, "status", "Playing as %dP against %s", st->local_player + 1,
                     st->peer_npid[0] ? st->peer_npid : "?");
            break;
        case NETPLAY_WATCHING:
            rl_value(RL_STATUS, "status", "Watching the room's match");
            break;
        case NETPLAY_FAILED:
            rl_value(RL_STATUS, "status", "Not signed in");
            break;
    }
    if (st->twitch_state == RPCN_TWITCH_WAITING)
        snprintf(g_rlobby.info, RL_INFO_LEN,
                 "Go to %s and enter the code %s. This signs the account's other devices out of Twitch.",
                 st->twitch_uri, st->twitch_user_code);
    else if (st->error[0] && (st->state == NETPLAY_FAILED || st->state == NETPLAY_OFF))
        snprintf(g_rlobby.info, RL_INFO_LEN, "%s", st->error);
    else if (!g_rlobby.have_cfg && st->state == NETPLAY_OFF)
        snprintf(g_rlobby.info, RL_INFO_LEN,
                 "No account yet. Sign in using Twitch above, or enter one as a cheat code.");

    /* ---- The rooms RPCN last listed ---- */
    rl_begin(RL_ROOM);
    rl_value(RL_ROOM, "-", "(none)");
    for (uint32_t i = 0; i < st->room_count; i++) {
        const rpcn_room_listing_t *r = &st->rooms[i];
        char k[RL_KEY_LEN];
        snprintf(k, sizeof k, "%llu", (unsigned long long)r->room_id);
        if (netplay_room_reject_reason(r->flag_attr, g_active_profile))
            rl_value(RL_ROOM, k, "%.16s - another version", r->owner);
        else if (r->has_password)
            rl_value(RL_ROOM, k, "%.16s - locked", r->owner);
        else if (r->cur_members >= r->max_slots)
            rl_value(RL_ROOM, k, "%.16s - full", r->owner);
        else
            rl_value(RL_ROOM, k, "%.16s - %u of %u, delay %u", r->owner,
                     (unsigned)r->cur_members, (unsigned)r->max_slots,
                     (unsigned)((r->flag_attr >> NETPLAY_ROOM_DELAY_SHIFT) & NETPLAY_ROOM_DELAY_MASK));
    }

    /* ---- The line in the room ---- */
    rl_begin(RL_PLAYERS);
    if (st->member_count == 0) rl_value(RL_PLAYERS, "-", "(not in a room)");
    for (uint32_t i = 0; i < st->member_count; i++) {
        const netplay_member_status_t *m = &st->members[i];
        const char *role = m->side == 0 ? "1P" : m->side == 1 ? "2P"
                         : (m->data.flags & ROOM_MEMBER_WATCH) ? "watching"
                         : (m->data.flags & ROOM_MEMBER_READY) ? "ready"
                         : (!m->is_me && !m->heard) ? "connecting" : "waiting";
        char k[RL_KEY_LEN];
        snprintf(k, sizeof k, "m%u", (unsigned)m->member_id);
        rl_value(RL_PLAYERS, k, "%d. %.16s%s  %s  %u-%u", m->line_pos + 1, m->npid,
                 m->is_me ? " (you)" : "", role,
                 (unsigned)m->data.wins, (unsigned)(m->data.games - m->data.wins));
    }

    /* ---- What this state allows ---- */
    rl_begin(RL_DO);
    rl_value(RL_DO, "-", "(nothing)");
    switch (st->state) {
        case NETPLAY_OFF:
        case NETPLAY_FAILED:
            break;
        case NETPLAY_CONNECTING:
            rl_value(RL_DO, "cancel", "Stop signing in");
            break;
        case NETPLAY_ONLINE:
            rl_value(RL_DO, "host", "Host a room for %u",
                     (unsigned)(c->max_players ? c->max_players : 2));
            rl_value(RL_DO, "join", "Join the room above");
            rl_value(RL_DO, "refresh", "Refresh the room list");
            break;
        case NETPLAY_IN_ROOM:
            if (st->me.flags & ROOM_MEMBER_READY)
                rl_value(RL_DO, "unready", "Not ready after all");
            else if (!(st->me.flags & ROOM_MEMBER_WATCH))
                rl_value(RL_DO, "ready", "Ready to play");
            if (st->is_host && st->room.phase == ROOM_PHASE_LOBBY)
                rl_value(RL_DO, "startnow", "Start the next match now");
            rl_value(RL_DO, "leave", "Leave the room");
            break;
        case NETPLAY_SYNCING:
            rl_value(RL_DO, "quit",  "Stop waiting");
            rl_value(RL_DO, "leave", "Leave the room");
            break;
        case NETPLAY_PLAYING:
        case NETPLAY_WATCHING:
            rl_value(RL_DO, "quit",  st->state == NETPLAY_WATCHING ? "Stop watching" : "Leave the match");
            rl_value(RL_DO, "leave", "Leave the room");
            break;
    }

    for (int o = RL_STATUS; o <= RL_DO; o++) rl_commit(o);
    if (g_rlobby.info[0]) g_rlobby_defs[RL_STATUS].info = g_rlobby.info;

    uint64_t sig = rl_signature();
    if (sig != g_rlobby.sig) {
        g_rlobby.sig   = sig;
        g_rlobby.dirty = true;
    }
}

/* ---- Which rows are shown ---------------------------------------------------- */

static void rl_show(int opt, bool on) {
    struct retro_core_option_display d = { rl_key_name[opt], on };
    if (g_rlobby.env) g_rlobby.env(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &d);
}

/* A row with nothing to say is hidden rather than dropped: the option count is
 * fixed for the life of the core. `force` says it all again whatever was said
 * last -- for the frontend's own update-display callback, which asks because it
 * is about to draw the list, and which may run while the core is paused. */
static bool rlobby_apply_visibility(bool force) {
    const netplay_status_t *st = &g_rlobby.st;
    bool on      = g_rlobby.started;
    bool online  = on && st->state == NETPLAY_ONLINE;
    bool in_room = on && (st->state == NETPLAY_IN_ROOM || st->state == NETPLAY_SYNCING
                          || netplay_state_running(st->state));
    int mask = (on ? 1 : 0) | (online ? 2 : 0) | (in_room ? 4 : 0);
    if (!g_rlobby.env) return false;   /* nothing said yet, so nothing to remember */
    if (mask == g_rlobby.shown && !force) return false;
    g_rlobby.shown = mask;
    rl_show(RL_STATUS,  on);
    rl_show(RL_DO,      on);
    rl_show(RL_ROOM,    online);
    rl_show(RL_SIZE,    online);
    rl_show(RL_PLAYERS, in_room);
    rl_show(RL_SIDE,    in_room);
    rl_show(RL_WATCH,   in_room);
    return true;
}

/* ---- Acting ------------------------------------------------------------------ */

static void rl_fire(const char *act) {
    netplay_config_t c = g_rlobby.cfg;
    if (!strcmp(act, "refresh")) {
        netplay_post(NETPLAY_CMD_SEARCH, &c);
    } else if (!strcmp(act, "host")) {
        netplay_post(NETPLAY_CMD_HOST, &c);
        rl_say("RPCN: hosting a room for %u", (unsigned)(c.max_players ? c.max_players : 2));
    } else if (!strcmp(act, "join")) {
        if (!g_rlobby.room_pick) {
            rl_say("RPCN: pick one in \"Room to join\" first");
            return;
        }
        c.room_id = g_rlobby.room_pick;
        netplay_post(NETPLAY_CMD_JOIN, &c);
        rl_say("RPCN: joining room %llu", (unsigned long long)c.room_id);
    } else if (!strcmp(act, "ready")) {
        netplay_post(NETPLAY_CMD_START, &c);
        rl_say("RPCN: ready - the match starts once everyone is");
    } else if (!strcmp(act, "unready")) {
        netplay_post(NETPLAY_CMD_STOP, &c);
    } else if (!strcmp(act, "startnow")) {
        netplay_post(NETPLAY_CMD_FORCE_START, &c);
    } else if (!strcmp(act, "leave")) {
        netplay_post(NETPLAY_CMD_LEAVE_ROOM, &c);
        rl_say("RPCN: left the room");
    } else if (!strcmp(act, "quit")) {
        netplay_post(NETPLAY_CMD_STOP, &c);
    } else if (!strcmp(act, "cancel")) {
        /* Through the core's own sign-in option, so the core does not go on
         * thinking it has asked for a session that is no longer being set up. */
        rl_set_var(RL_LOGIN_KEY, "off");
    }
}

/* ---- The core's calls --------------------------------------------------------- */

/* Read what the menu holds. Called when the frontend says an option changed.
 * `now_us` is the core's clock; a "Do" value is only acted on once it has been
 * left alone (rlobby_update). */
static void rlobby_read_options(int64_t now_us) {
    const char *v;
    if (!g_rlobby.started) return;

    if ((v = rl_var(rl_key_name[RL_ROOM])))
        g_rlobby.room_pick = strcmp(v, "-") ? strtoull(v, NULL, 10) : 0;

    if ((v = rl_var(rl_key_name[RL_SIZE]))) {
        unsigned n = (unsigned)atoi(v);
        g_rlobby.cfg.max_players = n < 2 ? 2 : n > ROOM_MAX_MEMBERS ? ROOM_MAX_MEMBERS : n;
    }
    /* Side and take-part are settings, so they act on the CHANGE: asking again
     * every frame would argue with a room that turned the request down. */
    if ((v = rl_var(rl_key_name[RL_SIDE]))) {
        uint8_t e = !strcmp(v, "1p") ? (uint8_t)ROOM_ENTRY_1P
                  : !strcmp(v, "2p") ? (uint8_t)ROOM_ENTRY_2P : (uint8_t)ROOM_ENTRY_NONE;
        if (g_rlobby.read_once && e != g_rlobby.want_entry) {
            netplay_config_t c = g_rlobby.cfg;
            c.entry = e;
            netplay_post(NETPLAY_CMD_ENTRY, &c);
        }
        g_rlobby.want_entry = e;
    }
    if ((v = rl_var(rl_key_name[RL_WATCH]))) {
        bool w = !strcmp(v, "watch");
        if (g_rlobby.read_once && w != g_rlobby.want_watch) {
            netplay_config_t c = g_rlobby.cfg;
            c.watch_only = w;
            netplay_post(NETPLAY_CMD_WATCH, &c);
        }
        g_rlobby.want_watch = w;
    }
    if ((v = rl_var(rl_key_name[RL_DO]))) {
        if (!strcmp(v, "-")) {
            g_rlobby.act[0] = '\0';
        } else if (!g_rlobby.read_once) {
            /* Left in the frontend's options file by a run that ended before it
             * could put the row back: an action, not a setting, so it is not
             * replayed on the next launch. */
            rl_set_var(rl_key_name[RL_DO], "-");
        } else if (strcmp(v, g_rlobby.act)) {
            snprintf(g_rlobby.act, RL_KEY_LEN, "%s", v);
            g_rlobby.act_at_us = now_us + RL_SETTLE_US;
        }
    }
    g_rlobby.read_once = true;
}

/* Once per retro_run: snapshot the session, act on a settled choice, and rebuild
 * the rows. True when the table has changed and the core should send it again --
 * which the core does itself, because the table is the core's; the lobby owns
 * only the last seven entries of it. */
static bool rlobby_update(int64_t now_us) {
    if (!g_rlobby.started) return false;
    netplay_get_status(&g_rlobby.st);
    const netplay_status_t *st = &g_rlobby.st;

    if ((int)st->state != g_rlobby.last_state) {
        g_rlobby.last_state = (int)st->state;
        if (st->state == NETPLAY_ONLINE) {
            g_rlobby.next_search_us = 0;   /* list the rooms now */
            /* The login the session now holds -- a Twitch token the flow just
             * landed, or an account from a cheat -- is the one this lobby works
             * with from here on. What the menu set stays the menu's. */
            netplay_config_t fresh;
            if (netplay_stored_settings(&fresh)) {
                fresh.frame_delay = g_rlobby.cfg.frame_delay;
                fresh.max_players = g_rlobby.cfg.max_players;
                snprintf(fresh.room_password, sizeof fresh.room_password, "%s",
                         g_rlobby.cfg.room_password);
                g_rlobby.cfg      = fresh;
                g_rlobby.have_cfg = true;
            }
        }
    }
    if (st->state == NETPLAY_ONLINE && !st->search_pending && now_us >= g_rlobby.next_search_us) {
        netplay_post(NETPLAY_CMD_SEARCH, &g_rlobby.cfg);
        g_rlobby.next_search_us = now_us + RL_SEARCH_US;
    }
    if (g_rlobby.act[0] && now_us >= g_rlobby.act_at_us) {
        char act[RL_KEY_LEN];
        snprintf(act, sizeof act, "%s", g_rlobby.act);
        g_rlobby.act[0] = '\0';
        rl_set_var(rl_key_name[RL_DO], "-");
        rl_fire(act);
    }
    rl_build();
    rlobby_apply_visibility(false);
    if (!g_rlobby.dirty || now_us < g_rlobby.push_at_us) return false;
    g_rlobby.dirty      = false;
    g_rlobby.push_at_us = now_us + RL_PUSH_US;
    return true;
}

/* The ROM is being unloaded, or online play is no longer RPCN: the rows have
 * nothing behind them, so they go out of the frontend's menu. */
static void rlobby_stop(void) {
    if (!g_rlobby.started) return;
    g_rlobby.started = false;
    rlobby_apply_visibility(true);
}

/* The input delay a room hosted here asks for, from the core's own option. */
static void rlobby_set_delay(int frames) {
    if (frames > 0) g_rlobby.cfg.frame_delay = (uint32_t)frames;
}

/*
 * RPCN is the chosen online play and a ROM is loaded. The caller sets the
 * settings path first: netplay_init reads the file.
 */
static void rlobby_init(retro_environment_t env, void (*say)(const char *, unsigned), int delay) {
    g_rlobby.env = env;
    g_rlobby.say = say;

    netplay_init();
    netplay_set_open_browser(false);   /* the code and the address go on screen */

    g_rlobby.have_cfg = netplay_stored_settings(&g_rlobby.cfg);
    netplay_config_t *c = &g_rlobby.cfg;
    if (!c->server[0])   snprintf(c->server, sizeof c->server, "%s", NETPLAY_DEFAULT_SERVER);
    if (!c->port)        c->port = RPCN_DEFAULT_PORT;
    if (!c->frame_delay) c->frame_delay = 2;
    if (!c->max_players) c->max_players = 2;
    if (delay > 0)       c->frame_delay = (uint32_t)delay;

    /* A second load in the same process starts the lobby over: the old
     * session's state would swallow the new one's first transitions. */
    g_rlobby.started   = true;
    g_rlobby.shown     = -1;           /* the rows have not been shown or hidden yet */
    g_rlobby.last_state = -1;
    g_rlobby.read_once = false;        /* ...so a "Do" left in the menu is cleared, not run */
    g_rlobby.act[0]    = '\0';
    g_rlobby.room_pick = 0;
    netplay_get_status(&g_rlobby.st);
    rl_build();
    rlobby_apply_visibility(true);
}

#endif /* RETRO_LOBBY_H */
