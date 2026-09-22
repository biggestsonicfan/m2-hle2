/*
 * netplay_window.h — the netplay front-end: account, server, room browser, and
 * what the session is currently doing.
 *
 * Draws entirely from a snapshot (netplay_get_status) and never touches netplay
 * state directly: every button posts a command into the queue the emu thread
 * drains. That is the whole of the threading contract, and it is why nothing in
 * here needs a lock of its own.
 *
 * The room browser shows two lists. The first is this emulator's own lobby space
 * and its rooms are joinable. The second is YAMP's space for the same arcade
 * game, and it is display-only: those rooms are hosted by a different emulator
 * running a different build of the game, so a match could never stay in sync.
 * Showing them anyway is deliberate — "nobody is online" and "two people are
 * online in an emulator you cannot play against" are different answers, and only
 * one of them means there is no point waiting. See com_id.h.
 */
#ifndef NETPLAY_WINDOW_H
#define NETPLAY_WINDOW_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"

#include "netplay.h"

/*
 * The UI's own copy of the settings. Posted with every command, so the emu
 * thread never reads these buffers while ImGui is editing them.
 *
 * The server is prefilled with this project's own, so a player who just wants to
 * play types a name and a password and presses Connect. It is an ordinary
 * editable field — any RPCN server works, and pointing it elsewhere is one edit.
 */
#define NETPLAY_DEFAULT_SERVER "rpcn.sonicthefighte.rs"

static netplay_config_t g_np_ui = {
    .server      = NETPLAY_DEFAULT_SERVER,
    .port        = RPCN_DEFAULT_PORT,
    .frame_delay = 2,
    .browse_yamp = true,
};
static char     g_np_ui_email[128]  = "";
static char     g_np_ui_join_id[24] = "";
static bool     g_np_ui_show_signup = false;
static uint64_t g_np_ui_selected    = 0;

/*
 * Over the middle of the game: the room has emptied and the board is still in
 * its VS mode (netplay_empty_room_pump). Drawn whether or not the netplay window
 * is open, because the player is looking at the game, and it takes no input --
 * the button that answers it goes to the board.
 */
static inline void netplay_empty_room_overlay(void) {
    netplay_status_t st;
    netplay_get_status(&st);
    if (!st.empty_room) return;
    const ImGuiViewport *vp = igGetMainViewport();
    igSetNextWindowPosEx((ImVec2){ vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                   vp->WorkPos.y + vp->WorkSize.y * 0.5f },
                         ImGuiCond_Always, (ImVec2){ 0.5f, 0.5f });
    igSetNextWindowBgAlpha(0.8f);
    if (igBegin("##netplay_empty_room", NULL,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings)) {
        igTextUnformatted("There are no other players in the lobby.");
        igTextUnformatted("Press any button to restart the game.");
    }
    igEnd();
}

/* netplay_state_text lives in net/netplay.h: the MCP bridge names these
 * states too, and it is included before this window is. */

/*
 * The room: everybody in it in line order, what they are doing, and this
 * player's own choices. The line and the sides are the owner's (net/room.h);
 * the buttons here only change what THIS player has published.
 */
static inline void netplay_window_room(const netplay_status_t *st) {
    ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
    if (igBeginTable("##members", 5, tf)) {
        igTableSetupColumnEx("#",       ImGuiTableColumnFlags_WidthFixed,  24.0f, 0);
        igTableSetupColumnEx("Player",  ImGuiTableColumnFlags_WidthFixed, 150.0f, 0);
        igTableSetupColumnEx("Now",     ImGuiTableColumnFlags_WidthFixed,  90.0f, 0);
        igTableSetupColumnEx("W-L  pts", ImGuiTableColumnFlags_WidthFixed, 80.0f, 0);
        igTableSetupColumnEx("Link",    ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
        igTableHeadersRow();
        for (uint32_t i = 0; i < st->member_count; i++) {
            const netplay_member_status_t *m = &st->members[i];
            igTableNextRow();
            igTableSetColumnIndex(0);
            if (m->line_pos >= 0) igText("%d", m->line_pos + 1); else igTextDisabled("-");
            igTableSetColumnIndex(1);
            igText("%s%s%s", m->npid, m->is_me ? " (you)" : "", m->is_owner ? " *" : "");
            igTableSetColumnIndex(2);
            if (m->side == 0)                             igTextColored((ImVec4){0.5f, 0.8f, 1.0f, 1.0f}, "1P");
            else if (m->side == 1)                        igTextColored((ImVec4){1.0f, 0.6f, 0.5f, 1.0f}, "2P");
            else if (!m->known)                           igTextDisabled("...");
            else if (m->data.flags & ROOM_MEMBER_WATCH)   igTextDisabled("sitting out");
            else if (m->data.entry == ROOM_ENTRY_1P)      igText("wants 1P");
            else if (m->data.entry == ROOM_ENTRY_2P)      igText("wants 2P");
            else if (m->data.flags & ROOM_MEMBER_READY)   igText("ready");
            else                                          igTextDisabled("waiting");
            igTableSetColumnIndex(3);
            igText("%u-%u  %u", (unsigned)m->data.wins, (unsigned)(m->data.games - m->data.wins),
                   (unsigned)m->data.points);
            igTableSetColumnIndex(4);
            if (m->is_me)              igTextDisabled("-");
            else if (m->heard && m->rtt_ms >= 0) igText("reachable, %d ms", (int)m->rtt_ms);
            else if (m->heard)         igText("reachable");
            else if (m->addr_known)    igTextDisabled("punching...");
            else                       igTextDisabled("no address yet");
        }
        igEndTable();
    }
    igTextDisabled("* runs the room. Two fight; the winner stays on their side and the loser goes "
                   "to the back of the line.");

    bool running = netplay_state_running(st->state) || st->state == NETPLAY_SYNCING;
    if (running) {
        if (igButton(st->state == NETPLAY_WATCHING ? "Stop watching" : "Leave the match"))
            netplay_post(NETPLAY_CMD_STOP, &g_np_ui);
        igSameLine();
        igTextWrapped(st->state == NETPLAY_WATCHING
            ? "Every board in the room was reset for this match; yours runs the fighters' inputs."
            : "Every board in the room was reset for this match. Leaving calls it off.");
    } else if (st->me.flags & ROOM_MEMBER_READY) {
        if (igButton("Not ready")) netplay_post(NETPLAY_CMD_STOP, &g_np_ui);
    } else {
        igBeginDisabled(st->member_count < 2);
        if (igButton("Ready")) netplay_post(NETPLAY_CMD_START, &g_np_ui);
        igEndDisabled();
        igSameLine();
        igTextWrapped("The first match starts once every player is ready; after that the room "
                      "keeps going on its own.");
    }

    bool watch = (st->me.flags & ROOM_MEMBER_WATCH) != 0;
    if (igCheckbox("Sit out (watch only)", &watch)) {
        netplay_config_t c = g_np_ui;
        c.watch_only = watch;
        netplay_post(NETPLAY_CMD_WATCH, &c);
    }
    igSameLine();
    int entry = st->me.entry;
    igSetNextItemWidth(140.0f);
    if (igComboEx("Side##entry", &entry, "Either\0" "1P Entry\0" "2P Entry\0", -1)) {
        netplay_config_t c = g_np_ui;
        c.entry = (uint8_t)entry;
        netplay_post(NETPLAY_CMD_ENTRY, &c);
    }
    if (igIsItemHovered(0))
        igSetTooltip("Jump the line for that side of the cabinet. The first player asking for a "
                     "side gets it when the next match starts.");

    if (st->is_host && !running && st->room.phase == ROOM_PHASE_LOBBY) {
        igSameLine();
        if (igButton("Start now")) netplay_post(NETPLAY_CMD_FORCE_START, &g_np_ui);
    }
    if (igButton("Leave the room")) netplay_post(NETPLAY_CMD_LEAVE_ROOM, &g_np_ui);
}

static inline void netplay_window_draw(bool *p_open) {
    igSetNextWindowSize((ImVec2){620, 640}, ImGuiCond_FirstUseEver);
    if (!igBegin("Netplay (RPCN)", p_open, 0)) { igEnd(); return; }

    netplay_status_t st;
    netplay_get_status(&st);

    /* Adopt whatever was stored last run, once. netplay_init loaded it before the
     * UI existed, so this is the first chance to put it in the boxes. */
    static bool s_adopted = false;
    if (!s_adopted) {
        s_adopted = true;
        netplay_config_t stored;
        if (netplay_stored_settings(&stored)) {
            g_np_ui = stored;
            if (!g_np_ui.server[0])
                snprintf(g_np_ui.server, sizeof(g_np_ui.server), "%s", NETPLAY_DEFAULT_SERVER);
            if (!g_np_ui.port)        g_np_ui.port = RPCN_DEFAULT_PORT;
            if (!g_np_ui.frame_delay) g_np_ui.frame_delay = 2;
        }
        g_np_ui.vs_mode = g_vs_mode != 0;   /* --vs-mode */
    }

    /* ---- Status line ----------------------------------------------------- */
    igText("State: %s", netplay_state_text(st.state));
    if (st.com_id[0]) { igSameLine(); igText("   lobby %s", st.com_id); }
    if (st.error[0]) igTextColored((ImVec4){1.0f, 0.45f, 0.45f, 1.0f}, "%s", st.error);

    if (st.state >= NETPLAY_IN_ROOM && st.state != NETPLAY_FAILED) {
        const char *role = st.local_player == 0 ? "1P" : st.local_player == 1 ? "2P"
                         : st.local_player == 2 ? "watching" : "in line";
        igText("Room %llu   %u of %u   you: %s%s", (unsigned long long)st.room_id,
               st.member_count, st.max_slot, role, st.is_host ? "  (you run the room)" : "");
        if (st.room_known) {
            if (st.room.phase == ROOM_PHASE_MATCH)
                igText("Match %u in progress%s", st.room.match, st.room.vs_mode ? " (VS mode)" : "");
            else if (st.auto_start_s)
                igTextColored((ImVec4){0.45f, 0.95f, 0.55f, 1.0f}, "Next match in %u s", st.auto_start_s);
            else if (st.room.match)
                igText("Lobby - %u match%s played", st.room.match, st.room.match == 1 ? "" : "es");
            else
                igText("Lobby - the first match starts when every player is ready");
        }
        /* Somebody else is ready and this end is not. Worth saying outright: from
         * here it is otherwise indistinguishable from somebody who merely joined. */
        if (st.peer_ready)
            igTextColored((ImVec4){0.45f, 0.95f, 0.55f, 1.0f},
                          "Others are ready and waiting - press Ready to start");
    }
    if (netplay_state_running(st.state)) {
        igText("frame %u   stalls %u   match %u   seed 0x%08X",
               st.frame, st.stalls, st.generation, st.seed);
        if (st.desync_frame != LOCKSTEP_NO_CHECK)
            igTextColored((ImVec4){1.0f, 0.35f, 0.35f, 1.0f},
                          "DESYNC at frame %u - this board no longer agrees with the fighters'",
                          st.desync_frame);
    }

    igSeparator();

    /* ---- Server + account ------------------------------------------------ */
    if (igCollapsingHeader("Server and account",
                           st.state == NETPLAY_OFF || st.state == NETPLAY_FAILED
                               ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        igInputText("Server", g_np_ui.server, sizeof(g_np_ui.server), 0);
        int port = g_np_ui.port ? g_np_ui.port : RPCN_DEFAULT_PORT;
        if (igInputIntEx("Port", &port, 1, 10, 0)) {
            if (port < 1) port = 1;
            if (port > 65535) port = 65535;
            g_np_ui.port = (uint16_t)port;
        }
        /* ---- Twitch ------------------------------------------------------ */
        /*
         * Above the password boxes on purpose: for anyone who has a Twitch
         * account this is the whole of signing up and signing in, and the
         * classic fields below are the fallback rather than the main path.
         */
        if (st.twitch_signed_in && st.twitch_state != RPCN_TWITCH_WAITING) {
            igTextColored((ImVec4){0.6f, 0.8f, 1.0f, 1.0f}, "Signed in with Twitch as %s",
                          st.twitch_npid[0] ? st.twitch_npid : "(unknown)");
            igSameLine();
            if (igButton("Sign out")) netplay_post(NETPLAY_CMD_TWITCH_FORGET, &g_np_ui);
            if (igIsItemHovered(0))
                igSetTooltip("Forgets the stored login token. You will be sent back to twitch.tv "
                             "the next time you sign in.");
        } else if (st.twitch_state == RPCN_TWITCH_STARTING) {
            igText("Twitch: asking the server for a code...");
            igSameLine();
            if (igButton("Cancel")) netplay_post(NETPLAY_CMD_TWITCH_CANCEL, &g_np_ui);
        } else if (st.twitch_state == RPCN_TWITCH_WAITING) {
            igTextColored((ImVec4){0.6f, 0.8f, 1.0f, 1.0f}, "Enter this code on twitch.tv:");
            igSameLine();
            igTextColored((ImVec4){1.0f, 1.0f, 0.6f, 1.0f}, "%s", st.twitch_user_code);
            igTextWrapped("A browser should have opened at %s - the code is already filled in "
                          "there. Check it matches the one above before you approve it.",
                          st.twitch_uri);
            if (igButton("Open the page again")) netplay_open_url(st.twitch_uri);
            igSameLine();
            if (igButton("Copy code")) igSetClipboardText(st.twitch_user_code);
            igSameLine();
            if (igButton("Cancel")) netplay_post(NETPLAY_CMD_TWITCH_CANCEL, &g_np_ui);
        } else {
            if (igButton("Sign in with Twitch")) netplay_post(NETPLAY_CMD_TWITCH_START, &g_np_ui);
            if (igIsItemHovered(0))
                igSetTooltip("Approve it once in a browser and this machine is signed in for "
                             "good - the server hands back a login token that stands in for a "
                             "password from then on. No account name or password needed.");
            if (st.twitch_state == RPCN_TWITCH_FAILED && st.twitch_error[0]) {
                igTextColored((ImVec4){1.0f, 0.6f, 0.4f, 1.0f}, "%s", st.twitch_error);
            }
        }

        igSeparator();

        /* ---- Classic account --------------------------------------------- */
        igBeginDisabled(st.twitch_signed_in);
        igInputText("Account name", g_np_ui.npid, sizeof(g_np_ui.npid), 0);
        igInputText("Password", g_np_ui.password, sizeof(g_np_ui.password),
                    ImGuiInputTextFlags_Password);
        igInputText("Token", g_np_ui.token, sizeof(g_np_ui.token), 0);
        if (igIsItemHovered(0))
            igSetTooltip("The 16-character code the server e-mails when an account is made. "
                         "Leave it empty: most servers do not verify by e-mail, and one that "
                         "does will say so when the login is refused.");
        igEndDisabled();
        if (st.twitch_signed_in)
            igTextDisabled("The Twitch login token is used in place of a password.");

        igInputText("Certificate SHA-256", g_np_ui.fingerprint, sizeof(g_np_ui.fingerprint), 0);
        if (igIsItemHovered(0))
            igSetTooltip("Leave empty for a server with a real certificate - the chain and host "
                         "name are validated normally. A self-signed server (RPCN's own "
                         "--cert-gen) cannot pass that, so paste its fingerprint here: connect "
                         "once with this empty and the refusal names the value to paste.");

        igCheckbox("Also list YAMP's rooms (read-only)", &g_np_ui.browse_yamp);
        if (igIsItemHovered(0))
            igSetTooltip("YAMP plays the console port of the same arcade game. Its rooms cannot "
                         "be joined from here - they are listed so an empty lobby can be told "
                         "apart from an empty lobby with people next door.");

        bool connected = (st.state != NETPLAY_OFF && st.state != NETPLAY_FAILED);
        if (!connected) {
            if (igButton("Connect")) {
                if (st.twitch_signed_in) {
                    /* The account boxes above are greyed out, so what is in them
                     * is left over from before and is nobody's input. Signed in
                     * with Twitch, Connect means "as that login" - which
                     * TWITCH_START now is, without a browser, while the token
                     * holds (netplay_twitch_reuse). */
                    netplay_config_t as_twitch = g_np_ui;
                    as_twitch.npid[0] = '\0';
                    netplay_post(NETPLAY_CMD_TWITCH_START, &as_twitch);
                } else {
                    netplay_post(NETPLAY_CMD_CONNECT, &g_np_ui);
                }
            }
        } else {
            if (igButton("Disconnect")) netplay_post(NETPLAY_CMD_DISCONNECT, &g_np_ui);
        }
        igSameLine();
        if (igButton(g_np_ui_show_signup ? "Hide sign-up" : "No account yet?"))
            g_np_ui_show_signup = !g_np_ui_show_signup;

        if (g_np_ui_show_signup) {
            igIndentEx(0.0f);
            igTextWrapped("Registering runs on its own connection, before any login - so this "
                          "works on a server you have never logged into. The name and password "
                          "above are the ones it will create.");
            igInputText("E-mail", g_np_ui_email, sizeof(g_np_ui_email), 0);
            if (igButton("Create account")) {
                netplay_config_t c = g_np_ui;
                snprintf(c.email, sizeof(c.email), "%s", g_np_ui_email);
                netplay_post(NETPLAY_CMD_CREATE_ACCOUNT, &c);
            }
            igSameLine();
            if (igButton("E-mail my token again"))
                netplay_post(NETPLAY_CMD_RESEND_TOKEN, &g_np_ui);

            if (st.account_state == RPCN_ACCOUNT_WORKING) igText("working...");
            else if (st.account_state == RPCN_ACCOUNT_DONE)
                igTextColored((ImVec4){0.5f, 1.0f, 0.5f, 1.0f}, "%s",
                              st.account_job == RPCN_ACCOUNT_JOB_CREATE
                                  ? "account created" : "verification token e-mailed");
            else if (st.account_state == RPCN_ACCOUNT_FAILED)
                igTextColored((ImVec4){1.0f, 0.45f, 0.45f, 1.0f}, "%s", st.account_error);
            igUnindentEx(0.0f);
        }
    }

    /* ---- Rooms ----------------------------------------------------------- */
    if (igCollapsingHeader("Rooms", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool online = (st.state == NETPLAY_ONLINE);
        bool in_room = (st.state >= NETPLAY_IN_ROOM && st.state != NETPLAY_FAILED);

        int delay = (int)(g_np_ui.frame_delay ? g_np_ui.frame_delay : 2);
        if (igSliderIntEx("Frame delay", &delay, 0, 10, "%d frames", 0))
            g_np_ui.frame_delay = (uint32_t)delay;
        if (igIsItemHovered(0))
            igSetTooltip("How many frames of network latency are absorbed before either machine "
                         "has to stall. Higher is smoother and less responsive. 2-4 on a good "
                         "connection; raise it if the stall counter climbs.");
        int players = (int)(g_np_ui.max_players >= 2 ? g_np_ui.max_players : 2);
        if (igSliderIntEx("Room size", &players, 2, (int)ROOM_MAX_MEMBERS, "%d players", 0))
            g_np_ui.max_players = (uint32_t)players;
        if (igIsItemHovered(0))
            igSetTooltip("How many people a room you host can hold. Two fight at a time; the "
                         "rest wait in line and watch, and the winner stays on (the PS3 "
                         "port's Room Match).");
        igInputText("Room password", g_np_ui.room_password, sizeof(g_np_ui.room_password), 0);
        igCheckbox("VS mode", &g_np_ui.vs_mode);
        if (igIsItemHovered(0))
            igSetTooltip("For a room you host. After a match both players go straight back to "
                         "character select, already in, with no reboot. The winner does not stay on "
                         "against the CPU. With other players waiting in line, the line still moves "
                         "as usual.");

        igBeginDisabled(!online);
        if (igButton("Host a room")) netplay_post(NETPLAY_CMD_HOST, &g_np_ui);
        igSameLine();
        if (igButton("Refresh list")) netplay_post(NETPLAY_CMD_SEARCH, &g_np_ui);
        igEndDisabled();
        if (st.search_pending) { igSameLine(); igText("searching..."); }

        /* Its own row: with the two buttons above it this ran off the right edge
         * of the default window, which put the button doing the work off-screen. */
        igSetNextItemWidth(160.0f);
        igInputTextWithHint("##joinid", "room id", g_np_ui_join_id, sizeof(g_np_ui_join_id),
                            ImGuiInputTextFlags_CharsDecimal);
        igSameLine();
        igBeginDisabled(!online || !g_np_ui_join_id[0]);
        if (igButton("Join by id")) {
            netplay_config_t c = g_np_ui;
#ifdef _WIN32
            c.room_id = _strtoui64(g_np_ui_join_id, NULL, 10);
#else
            c.room_id = strtoull(g_np_ui_join_id, NULL, 10);
#endif
            if (c.room_id) netplay_post(NETPLAY_CMD_JOIN, &c);
        }
        igEndDisabled();

        ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                           | ImGuiTableFlags_SizingFixedFit;
        if (igBeginTable("##rooms", 5, tf)) {
            igTableSetupColumnEx("Room",    ImGuiTableColumnFlags_WidthFixed, 130.0f, 0);
            igTableSetupColumnEx("Host",    ImGuiTableColumnFlags_WidthFixed, 130.0f, 0);
            igTableSetupColumnEx("Players", ImGuiTableColumnFlags_WidthFixed,  60.0f, 0);
            igTableSetupColumnEx("Locked",  ImGuiTableColumnFlags_WidthFixed,  50.0f, 0);
            igTableSetupColumnEx("",        ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
            igTableHeadersRow();

            char buf[64];
            for (uint32_t i = 0; i < st.room_count; i++) {
                const rpcn_room_listing_t *r = &st.rooms[i];
                igTableNextRow();
                igTableSetColumnIndex(0);
                snprintf(buf, sizeof(buf), "%llu##r%u", (unsigned long long)r->room_id, i);
                if (igSelectableEx(buf, g_np_ui_selected == r->room_id, 0, (ImVec2){0, 0}))
                    g_np_ui_selected = r->room_id;
                igTableSetColumnIndex(1); igText("%s", r->owner);
                igTableSetColumnIndex(2); igText("%u/%u", r->cur_members, r->max_slots);
                igTableSetColumnIndex(3); igText("%s", r->has_password ? "yes" : "");
                igTableSetColumnIndex(4);
                {
                    const char *why = netplay_room_reject_reason(r->flag_attr, g_active_profile);
                    if (why) igTextColored((ImVec4){1.0f, 0.6f, 0.4f, 1.0f}, "%s", why);
                    else if (r->flag_attr)
                        igText("delay %u", (r->flag_attr >> NETPLAY_ROOM_DELAY_SHIFT)
                                            & NETPLAY_ROOM_DELAY_MASK);
                }
            }
            igEndTable();
        }
        if (st.room_count == 0)
            igTextDisabled(online ? "No rooms here yet - host one, and the other player joins it."
                                  : "Connect to see the rooms on this server.");

        igBeginDisabled(!online || !g_np_ui_selected);
        if (igButton("Join selected")) {
            netplay_config_t c = g_np_ui;
            c.room_id = g_np_ui_selected;
            netplay_post(NETPLAY_CMD_JOIN, &c);
        }
        igEndDisabled();

        /* The read-only neighbour list. */
        if (st.com_id_foreign[0] || st.foreign_room_count) {
            igSeparator();
            igTextDisabled("YAMP lobby %s - not joinable from here (%u room%s)",
                           st.com_id_foreign, st.foreign_room_count,
                           st.foreign_room_count == 1 ? "" : "s");
            for (uint32_t i = 0; i < st.foreign_room_count; i++) {
                const rpcn_room_listing_t *r = &st.foreign_rooms[i];
                igTextDisabled("    %llu   %s   %u/%u%s", (unsigned long long)r->room_id,
                               r->owner, r->cur_members, r->max_slots,
                               r->has_password ? "   locked" : "");
            }
        }

        if (in_room) {
            igSeparator();
            netplay_window_room(&st);
        }
    }

    /* ---- Log ------------------------------------------------------------- */
    if (igCollapsingHeader("Log", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (igBeginChild("##nplog", (ImVec2){0, 160}, ImGuiChildFlags_Borders, 0)) {
            uint32_t total = st.log_count;
            uint32_t shown = total < NETPLAY_LOG_LINES ? total : NETPLAY_LOG_LINES;
            for (uint32_t i = 0; i < shown; i++) {
                uint32_t n = total - shown + i;
                igTextWrapped("%s", st.log[n % NETPLAY_LOG_LINES]);
            }
            if (igGetScrollY() >= igGetScrollMaxY()) igSetScrollHereY(1.0f);
        }
        igEndChild();
    }

    igEnd();
}

#endif /* NETPLAY_WINDOW_H */
