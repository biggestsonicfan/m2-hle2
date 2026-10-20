/*
 * m2lua_mcp.h -- the bridge's script_* commands (MCP_GUIDE.md, "Game
 * scripts"; Pinboard #958), for the hosts that run core/m2lua.h. The bridge
 * hands them over through g_mcp_script_cmd, so a host without Lua answers
 * "unknown cmd".
 *
 *   script_status                      the script, its options, the last error
 *   script_option {"key":K,"value":N}  set option K to its Nth value (changefunc runs)
 *   script_reload                      read the script from disk again
 *   script_eval   {"code":"..."}       run a chunk in the script's state
 *
 * Setting an option and eval change the board: they are refused while a
 * session (netplay, GGPO, a replay, a follow) owns it, as the frame hook is.
 */
#ifndef M2LUA_MCP_H
#define M2LUA_MCP_H

#include "../core/m2lua.h"
#include "mcp_bridge.h"

static inline void m2lua_mcp_lock(bool lock) {
    if (!g_mcp.emu || !g_mcp.emu->thread_alive) return;
    if (lock) emu_mutex_lock(&g_mcp.emu->mutex);
    else      emu_mutex_unlock(&g_mcp.emu->mutex);
}

static inline void m2lua_mcp_status(char *resp, int cap) {
    m2lua_mcp_lock(true);
    m2lua_status_json(resp, cap, emu_script_may_run());
    m2lua_mcp_lock(false);
}

static inline bool m2lua_mcp_refused(char *resp, int cap) {
    if (emu_script_may_run()) return false;
    snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"a session owns the board\"}");
    return true;
}

static inline void m2lua_mcp_set_option(const char *req, char *resp, int cap) {
    char key[64] = {0};
    int value = 0;
    if (!json_get_str_unescaped(req, "key", key, sizeof key) || !mcp_json_get_int(req, "value", &value)) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"missing key or value\"}");
        return;
    }
    if (m2lua_mcp_refused(resp, cap)) return;
    m2lua_mcp_lock(true);
    int i = m2lua_option_index(key);
    bool ok = i >= 0 && value >= 0 && value < g_m2lua.options[i].nvalues;
    if (ok) m2lua_set_option(i, value);
    m2lua_mcp_lock(false);
    if (ok) m2lua_mcp_status(resp, cap);
    else    snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"no such option or value\"}");
}

static inline void m2lua_mcp_eval(const char *req, char *resp, int cap) {
    static char code[8192], out[4096], esc[8192];
    if (!json_get_str_unescaped(req, "code", code, sizeof code)) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"missing code\"}");
        return;
    }
    if (m2lua_mcp_refused(resp, cap)) return;
    m2lua_mcp_lock(true);
    bool ok = m2lua_eval(code, out, sizeof out);
    m2lua_mcp_lock(false);
    mcp_json_escape(esc, (int)sizeof esc, out);
    snprintf(resp, (size_t)cap, "{\"ok\":%s,\"%s\":\"%s\"}", ok ? "true" : "false",
             ok ? "result" : "error", esc);
}

static inline void m2lua_mcp_reload(char *resp, int cap) {
    if (m2lua_mcp_refused(resp, cap)) return;
    m2lua_mcp_lock(true);
    m2lua_reload();
    m2lua_mcp_lock(false);
    m2lua_mcp_status(resp, cap);
}

static void m2lua_mcp_cmd(const char *cmd, const char *req, char *resp, int cap) {
    if      (strcmp(cmd, "script_status") == 0) m2lua_mcp_status(resp, cap);
    else if (strcmp(cmd, "script_option") == 0) m2lua_mcp_set_option(req, resp, cap);
    else if (strcmp(cmd, "script_eval")   == 0) m2lua_mcp_eval(req, resp, cap);
    else if (strcmp(cmd, "script_reload") == 0) m2lua_mcp_reload(resp, cap);
    else snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"unknown cmd: %s\"}", cmd);
}

#endif /* M2LUA_MCP_H */
