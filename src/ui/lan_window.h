/*
 * lan_window.h — "Direct play": a GGPO match straight to another machine's
 * address, with no lobby and no RPCN (net/ggpo_lan.h, Pinboard #1891).
 *
 * Host on a port, or join an address: a LAN's, a Tailscale tailnet's
 * (100.x.y.z), a forwarded port's. Like the netplay window it draws from a
 * snapshot (ggl_status) and only posts commands (ggl_post) for the emu
 * thread's pump.
 */
#ifndef LAN_WINDOW_H
#define LAN_WINDOW_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"

#ifdef M2HLE_GGPO

static char g_lan_ui_port[8]   = "7000";
static char g_lan_ui_addr[256] = "";

static inline void lan_window_draw(bool *p_open) {
    igSetNextWindowSize((ImVec2){460, 300}, ImGuiCond_FirstUseEver);
    if (!igBegin("Direct play (LAN / Tailscale)", p_open, 0)) { igEnd(); return; }
    ggl_status_t st = ggl_status();
    bool lan_match = st.stage == GGL_MATCH && strcmp(st.match, "lan") == 0;
    bool busy = st.stage == GGL_LAN_HOST || st.stage == GGL_LAN_JOIN || lan_match;

    igTextWrapped("Rollback netplay straight to the other machine: no server. One player hosts, "
                  "the other joins the host's address. Over the internet, use a Tailscale "
                  "address (100.x.y.z) or forward the port (UDP).");
    igSeparator();

    if (st.stage == GGL_MATCH && !lan_match) {
        igTextDisabled("A lobby match is on; end it first.");
    } else if (!busy) {
        igSetNextItemWidth(80);
        igInputText("Port", g_lan_ui_port, sizeof g_lan_ui_port, ImGuiInputTextFlags_CharsDecimal);
        igSameLine();
        if (igButton("Host")) ggl_post(GGL_CMD_LAN_HOST, g_lan_ui_port, NULL);
        igSetNextItemWidth(260);
        igInputTextWithHint("##addr", "100.64.0.2:7000", g_lan_ui_addr, sizeof g_lan_ui_addr, 0);
        igSameLine();
        igBeginDisabled(!g_lan_ui_addr[0]);
        if (igButton("Join")) ggl_post(GGL_CMD_LAN_JOIN, g_lan_ui_addr, NULL);
        igEndDisabled();
    }

    if (st.stage == GGL_LAN_HOST) {
        igText("Hosting: the other player joins %s", st.url);
        igTextDisabled("Browsers join over ws:// on the same port (wss:// through tailscale serve).");
        if (igButton("Stop hosting")) ggl_post(GGL_CMD_DISCONNECT, NULL, NULL);
    } else if (st.stage == GGL_LAN_JOIN) {
        igText("Asking %s for a match...", st.url);
        if (igButton("Cancel")) ggl_post(GGL_CMD_DISCONNECT, NULL, NULL);
    } else if (lan_match) {
        igText("Playing %s as %s against %s", st.game, st.user, st.opponent);
        igTextDisabled("packets sent %u, received %u", st.sent_direct, st.got_direct);
        if (igButton("End match")) ggl_post(GGL_CMD_END, NULL, NULL);
        igSameLine();
        if (st.side == 0 && igButton("End and stop hosting")) ggl_post(GGL_CMD_DISCONNECT, NULL, NULL);
    }
    if (st.error[0] && (st.stage == GGL_FAILED || busy || st.stage == GGL_OFF))
        igTextColored((ImVec4){1.0f, 0.5f, 0.4f, 1.0f}, "%s", st.error);
    igEnd();
}

#else
static inline void lan_window_draw(bool *p_open) { (void)p_open; }
#endif

#endif /* LAN_WINDOW_H */
