/*
 * mcp_bridge.h — local TCP JSON bridge for the MCP server.
 *
 * Started when m2hle.exe is launched with --mcp [--mcp-port N].
 * Listens on localhost:port, accepts one client at a time, exchanges
 * newline-delimited JSON commands:
 *
 *   → {"cmd":"get_status"}
 *   ← {"ok":true,"running":false,"halted":false,"ip":"0x00074E4","steps_per_second":0}
 *
 * All reads from CPU state go through the emu mutex snapshot so the bridge
 * thread never races the emu thread.
 */
#ifndef MCP_BRIDGE_H
#define MCP_BRIDGE_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "memory.h"
#include "i960.h"
#include "emu_thread.h"
#include "breakpoint.h"
#include "log.h"
#include "game_profile.h"
#include "rom_loader.h"   /* romset_t — the regions the model decoder reads */
#include "input.h"     /* g_input.held — drive the game's I/O ports over the bridge */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  ifndef _WINSOCK2API_
#    include <winsock2.h>
#  endif
#  pragma comment(lib, "ws2_32.lib")
   typedef SOCKET mcp_sock_t;
#  define MCP_INVALID_SOCK  INVALID_SOCKET
#  define mcp_close(s)      closesocket(s)
#  define mcp_sockerr()     WSAGetLastError()
#else
#  include <errno.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
   typedef int mcp_sock_t;
#  define MCP_INVALID_SOCK  (-1)
#  define mcp_close(s)      close(s)
#  define mcp_sockerr()     errno
#endif

/* Largest range dump_memory_file will copy in one call — enough for a whole
 * 1 MB texture sheet or the 32 MB main-data region, and a bound on how much the
 * bridge will allocate for a single request. */
#define MCP_DUMP_MAX_BYTES  (32u * 1024u * 1024u)

/* How long wait_frames will sit on a stopped emulator before concluding no
 * frame is coming. Long enough to cover a ROM load — the bridge accepts a
 * client well before --run has taken effect. */
#define MCP_STOPPED_GRACE_MS  4000u

/* ---- Module state -------------------------------------------------------- */

typedef struct {
    int               enabled;
    int               port;
    emu_thread_ctx_t *emu;
    i960_cpu_t       *cpu;
    memory_bus_t     *bus;
    /* The assembled ROM regions, for commands that decode straight out of them
     * rather than out of the running machine's RAM. Set by main.c after a load;
     * null until then, and every such command has to check. */
    const romset_t   *romset;

#ifdef _WIN32
    HANDLE            thread;
#else
    pthread_t         thread;
#endif
    volatile int      alive;
    mcp_sock_t        listen_sock;
} mcp_bridge_t;

static mcp_bridge_t g_mcp = {0};

/* ---- Tiny JSON helpers --------------------------------------------------- */

/* Write a hex uint32 JSON field.  buf must be large enough. */
static inline int mcp_json_u32hex(char *buf, int cap, const char *key, uint32_t v) {
    return snprintf(buf, (size_t)cap, "\"%s\":\"0x%08X\"", key, v);
}

/* Extract string value from `"key":"value"` — returns 1 on success. */
static int mcp_json_get_str(const char *json, const char *key, char *out, int out_cap) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < out_cap - 1) out[i++] = *p++;
    out[i] = '\0';
    return 1;
}

/* Extract uint32 (decimal or 0x hex) from `"key":value`. */
static int mcp_json_get_u32(const char *json, const char *key, uint32_t *out) {
    char vstr[32];
    /* Try quoted hex first, then unquoted. */
    if (mcp_json_get_str(json, key, vstr, sizeof(vstr))) {
        *out = (uint32_t)strtoul(vstr, NULL, 0);
        return 1;
    }
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    *out = (uint32_t)strtoul(p, NULL, 0);
    return 1;
}

/* ---- Command handlers ---------------------------------------------------- */

static void mcp_cmd_get_status(char *resp, int cap) {
    mcp_bridge_t *b = &g_mcp;
    int running = b->emu && emu_is_running(b->emu);
    int halted  = b->cpu && b->cpu->halted;
    uint32_t ip = 0;
    uint32_t sps = 0;
    const char *profile_id = "none";

    if (b->emu && b->emu->thread_alive) {
        /* Read the double-buffered snapshot WITHOUT the mutex (like the UI does).
         * board_vblank profiles run slices back-to-back and never release the
         * mutex long enough, so locking here starves get_status. A slightly stale
         * ip/sps is fine for a status query. */
        ip  = b->emu->cpu_snapshot.sfr.ip;
        sps = b->emu->steps_per_second;
    }
    if (g_active_profile) profile_id = g_active_profile->id;

    snprintf(resp, (size_t)cap,
             "{\"ok\":true,\"running\":%s,\"halted\":%s,"
             "\"ip\":\"0x%08X\",\"steps_per_second\":%u,\"profile\":\"%s\","
             "\"frames\":%u,\"rom_loaded\":%s,\"match_replay\":\"%s\",\"match_replay_frame\":%u}",
             running ? "true" : "false",
             halted  ? "true" : "false",
             ip, sps, profile_id,
             g_emu_frames,
             (g_mcp.romset && g_mcp.romset->loaded) ? "true" : "false",
             g_match_replay == 1 ? "armed" : g_match_replay == 2 ? "done" : g_match_replay < 0 ? "unsupported" : "off",
             g_match_replay_frame);
}

/* match_replay: arm the jump from attract mode's intro movie straight to its
 * preprogrammed replay fight (game_quirks_t.attract_replay). It happens at the
 * next frame edge the profile's movie step is reached, so arm it before attract
 * gets there -- --match-replay on the command line arms it from boot. */
static void mcp_cmd_match_replay(char *resp, int cap) {
    if (!g_active_profile || !g_active_profile->quirks.attract_replay.step_addr) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"this game profile has no attract replay\"}");
        return;
    }
    if (g_match_replay != 2) g_match_replay = 1;
    snprintf(resp, (size_t)cap, "{\"ok\":true,\"match_replay\":\"%s\"}", g_match_replay == 2 ? "done" : "armed");
}

/* Drive game input: set the active-high held mask (0x500700 layout, same bits the
 * profile's input.bits use). The I/O read callback serves these to the game's
 * read_sw. e.g. {"cmd":"set_input","held":"0x1000"} holds P1 DOWN. */
static void mcp_cmd_set_input(const char *req, char *resp, int cap) {
    uint32_t held = 0;
    mcp_json_get_u32(req, "held", &held);
    g_input.held = held;
    snprintf(resp, (size_t)cap, "{\"ok\":true,\"held\":\"0x%08X\"}", held);
}

/* Live-tune the 3D camera (for the geo_displaylist render path) without rebuilding.
 * Any omitted field keeps its current value. e.g.
 *   {"cmd":"set_camera","cam_z":"0","fov":"65","rot_y":"0"} */
static void mcp_cmd_set_camera(const char *req, char *resp, int cap) {
    if (!g_geo3d_state) { snprintf(resp,(size_t)cap,"{\"ok\":false,\"error\":\"geo3d not ready\"}"); return; }
    char v[32];
    if (mcp_json_get_str(req,"cam_x",v,sizeof v)) g_geo3d_state->cam_x   = (float)atof(v);
    if (mcp_json_get_str(req,"cam_y",v,sizeof v)) g_geo3d_state->cam_y   = (float)atof(v);
    if (mcp_json_get_str(req,"cam_z",v,sizeof v)) g_geo3d_state->cam_z   = (float)atof(v);
    if (mcp_json_get_str(req,"rot_y",v,sizeof v)) g_geo3d_state->rot_y   = (float)atof(v);
    if (mcp_json_get_str(req,"rot_x",v,sizeof v)) g_geo3d_state->rot_x   = (float)atof(v);
    if (mcp_json_get_str(req,"fov",  v,sizeof v)) g_geo3d_state->fov_deg = (float)atof(v);
    if (mcp_json_get_str(req,"test", v,sizeof v)) g_geo3d_state->test_triangle = (atoi(v) != 0);
    if (mcp_json_get_str(req,"lines_only",v,sizeof v)) g_geo3d_state->lines_only = (atoi(v) != 0);
    snprintf(resp,(size_t)cap,
             "{\"ok\":true,\"cam\":[%.2f,%.2f,%.2f],\"rot\":[%.3f,%.3f],\"fov\":%.1f,"
             "\"lines\":%d,\"tris\":%d,\"test\":%d}",
             g_geo3d_state->cam_x,g_geo3d_state->cam_y,g_geo3d_state->cam_z,
             g_geo3d_state->rot_y,g_geo3d_state->rot_x,g_geo3d_state->fov_deg,
             g_geo3d_lines.count, g_geo3d_tris.count, g_geo3d_state->test_triangle ? 1 : 0);
}

static void mcp_cmd_get_registers(char *resp, int cap) {
    if (!g_mcp.emu || !g_mcp.emu->thread_alive) { snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"emu not started\"}"); return; }

    i960_cpu_t snap;
    snap = g_mcp.emu->cpu_snapshot;   /* lock-free double-buffered read (see get_status) */

    /* Build JSON manually — no alloc, bounded output. */
    char *p = resp;
    int   left = cap;
    int   n;

#define APPEND(...) do { n = snprintf(p, (size_t)left, __VA_ARGS__); p += n; left -= n; } while(0)

    APPEND("{\"ok\":true,\"globals\":{");
    for (int i = 0; i < 16; i++) {
        APPEND("\"g%d\":\"0x%08X\"%s", i, snap.globals.g[i], i < 15 ? "," : "");
    }
    APPEND("},\"locals\":{");
    static const char *local_names[16] = {
        "pfp","sp","rip","r3","r4","r5","r6","r7",
        "r8","r9","r10","r11","r12","r13","r14","r15"
    };
    for (int i = 0; i < 16; i++) {
        APPEND("\"%s\":\"0x%08X\"%s", local_names[i], snap.locals.r[i], i < 15 ? "," : "");
    }
    APPEND("},\"sfr\":{");
    APPEND("\"ip\":\"0x%08X\",", snap.sfr.ip);
    APPEND("\"ac\":\"0x%08X\",", snap.sfr.ac);
    APPEND("\"pc\":\"0x%08X\",", snap.sfr.pc);
    APPEND("\"tc\":\"0x%08X\"",  snap.sfr.tc);
    APPEND("},\"fp_regs\":[");
    for (int i = 0; i < 4; i++) {
        APPEND("%g%s", snap.fp_regs[i], i < 3 ? "," : "");
    }
    APPEND("],\"halted\":%s,\"frame_depth\":%d}",
           snap.halted ? "true" : "false", snap.frame_depth);
#undef APPEND
}

static void mcp_cmd_read_memory(const char *req, char *resp, int cap) {
    uint32_t addr = 0, size = 0;
    if (!mcp_json_get_u32(req, "addr", &addr) || !mcp_json_get_u32(req, "size", &size)) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"missing addr or size\"}"); return;
    }
    if (size > 4096) size = 4096;  /* cap to avoid huge responses */
    if (!g_mcp.bus) { snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"bus not ready\"}"); return; }

    char *p = resp;
    int left = cap;
    int n;
    n = snprintf(p, (size_t)left, "{\"ok\":true,\"addr\":\"0x%08X\",\"data\":\"", addr);
    p += n; left -= n;
    for (uint32_t i = 0; i < size && left > 4; i++) {
        uint8_t b = mem_read8(g_mcp.bus, addr + i);
        n = snprintf(p, (size_t)left, "%02X", b);
        p += n; left -= n;
    }
    snprintf(p, (size_t)left, "\"}");
}

static void mcp_cmd_write_memory(const char *req, char *resp, int cap) {
    uint32_t addr = 0;
    char hexdata[8192 + 1] = {0};
    if (!mcp_json_get_u32(req, "addr", &addr) ||
        !mcp_json_get_str(req, "data", hexdata, sizeof(hexdata))) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"missing addr or data\"}"); return;
    }
    if (!g_mcp.bus) { snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"bus not ready\"}"); return; }

    int count = 0;
    for (int i = 0; hexdata[i*2] && hexdata[i*2+1]; i++) {
        char byte_str[3] = { hexdata[i*2], hexdata[i*2+1], 0 };
        uint8_t b = (uint8_t)strtoul(byte_str, NULL, 16);
        mem_write8(g_mcp.bus, addr + (uint32_t)i, b);
        count++;
    }
    snprintf(resp, (size_t)cap, "{\"ok\":true,\"bytes_written\":%d}", count);
}

static void mcp_cmd_emu_run(char *resp, int cap) {
    if (!g_mcp.emu || !g_mcp.emu->thread_alive) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"emu not started\"}"); return;
    }
    emu_run(g_mcp.emu);
    snprintf(resp, (size_t)cap, "{\"ok\":true}");
}

static void mcp_cmd_emu_stop(char *resp, int cap) {
    if (!g_mcp.emu || !g_mcp.emu->thread_alive) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"emu not started\"}"); return;
    }
    emu_stop(g_mcp.emu);
    snprintf(resp, (size_t)cap, "{\"ok\":true}");
}

static void mcp_cmd_emu_step(const char *req, char *resp, int cap) {
    uint32_t count = 1;
    mcp_json_get_u32(req, "count", &count);
    if (count < 1) count = 1;
    if (count > 1000000) count = 1000000;
    if (!g_mcp.emu || !g_mcp.emu->thread_alive) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"emu not started\"}"); return;
    }
    if (emu_is_running(g_mcp.emu)) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"must be stopped to step\"}"); return;
    }
    emu_step(g_mcp.emu, (int)count);
    snprintf(resp, (size_t)cap, "{\"ok\":true,\"steps\":%u}", count);
}

static void mcp_cmd_set_breakpoint(const char *req, char *resp, int cap) {
    uint32_t addr = 0;
    char label[64] = {0};
    if (!mcp_json_get_u32(req, "addr", &addr)) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"missing addr\"}"); return;
    }
    mcp_json_get_str(req, "label", label, sizeof(label));
    bp_add(addr, label[0] ? label : NULL);
    snprintf(resp, (size_t)cap, "{\"ok\":true,\"addr\":\"0x%08X\"}", addr);
}

static void mcp_cmd_clear_breakpoint(const char *req, char *resp, int cap) {
    uint32_t addr = 0;
    if (!mcp_json_get_u32(req, "addr", &addr)) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"missing addr\"}"); return;
    }
    int found = 0;
    for (int i = 0; i < BP_MAX; i++) {
        if (g_bp.list[i].active && g_bp.list[i].addr == addr) {
            bp_remove(i);
            found++;
        }
    }
    snprintf(resp, (size_t)cap, "{\"ok\":true,\"removed\":%d}", found);
}

static void mcp_cmd_list_breakpoints(char *resp, int cap) {
    char *p = resp;
    int left = cap;
    int n;
    n = snprintf(p, (size_t)left, "{\"ok\":true,\"breakpoints\":[");
    p += n; left -= n;
    int first = 1;
    for (int i = 0; i < BP_MAX && left > 8; i++) {
        if (!g_bp.list[i].active) continue;
        n = snprintf(p, (size_t)left, "%s{\"addr\":\"0x%08X\",\"label\":\"%s\",\"enabled\":%s}",
                     first ? "" : ",",
                     g_bp.list[i].addr,
                     g_bp.list[i].label,
                     g_bp.list[i].enabled ? "true" : "false");
        p += n; left -= n;
        first = 0;
    }
    snprintf(p, (size_t)left, "]}");
}

/* ---- Watchpoints (data breakpoints) ------------------------------------- */

static void mcp_cmd_set_watchpoint(const char *req, char *resp, int cap) {
    uint32_t addr = 0, size = 1;
    char label[64] = {0}, type[8] = {0};
    if (!mcp_json_get_u32(req, "addr", &addr)) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"missing addr\"}"); return;
    }
    mcp_json_get_u32(req, "size", &size);
    if (size == 0) size = 1;
    mcp_json_get_str(req, "label", label, sizeof(label));
    mcp_json_get_str(req, "type", type, sizeof(type));   /* "w" | "r" | "rw" (default w) */
    bool on_w = (type[0] == 0) || strchr(type, 'w') || strchr(type, 'W');
    bool on_r = strchr(type, 'r') || strchr(type, 'R');
    int idx = wp_add(addr, addr + size, on_w, on_r, label[0] ? label : NULL);
    snprintf(resp, (size_t)cap, "{\"ok\":%s,\"addr\":\"0x%08X\",\"size\":%u,\"slot\":%d}",
             idx >= 0 ? "true" : "false", addr, size, idx);
}

static void mcp_cmd_clear_watchpoint(const char *req, char *resp, int cap) {
    uint32_t addr = 0;
    if (!mcp_json_get_u32(req, "addr", &addr)) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"missing addr\"}"); return;
    }
    int removed = wp_remove_addr(addr);
    snprintf(resp, (size_t)cap, "{\"ok\":true,\"removed\":%d}", removed);
}

static void mcp_cmd_clear_all_watchpoints(char *resp, int cap) {
    wp_clear_all();
    snprintf(resp, (size_t)cap, "{\"ok\":true}");
}

static void mcp_cmd_list_watchpoints(char *resp, int cap) {
    char *p = resp; int left = cap;
    int n = snprintf(p, (size_t)left, "{\"ok\":true,\"watchpoints\":["); p += n; left -= n;
    int first = 1;
    for (int i = 0; i < WP_MAX && left > 8; i++) {
        if (!g_wp.list[i].active) continue;
        n = snprintf(p, (size_t)left,
                     "%s{\"lo\":\"0x%08X\",\"hi\":\"0x%08X\",\"w\":%s,\"r\":%s,\"label\":\"%s\"}",
                     first ? "" : ",", g_wp.list[i].lo, g_wp.list[i].hi,
                     g_wp.list[i].on_write ? "true" : "false",
                     g_wp.list[i].on_read ? "true" : "false", g_wp.list[i].label);
        p += n; left -= n; first = 0;
    }
    snprintf(p, (size_t)left, "]}");
}

static void mcp_cmd_get_cop_diagnostics(char *resp, int cap) {
    char *p = resp;
    int   left = cap;
    int   n;

#define APPEND(...) do { n = snprintf(p, (size_t)left, __VA_ARGS__); p += n; left -= n; } while(0)

    APPEND("{\"ok\":true,"
           "\"writes\":%u,\"reads\":%u,"
           "\"transforms\":%u,\"matrix_reads\":%u,"
           "\"unknown_cmds\":%u,\"unknown_unique\":%d,"
           "\"break_on_unknown\":%s,\"unknown_triggered\":%s,"
           "\"trigger_cmd\":\"0x%08X\",\"trigger_ip\":\"0x%08X\","
           "\"cmd_ips\":{"
           "\"set_pos\":\"0x%08X\","
           "\"set_ang_x\":\"0x%08X\","
           "\"set_ang_y\":\"0x%08X\","
           "\"set_ang_z\":\"0x%08X\","
           "\"read_matrix\":\"0x%08X\","
           "\"rot_transform\":\"0x%08X\","
           "\"full_transform\":\"0x%08X\","
           "\"sin_scale\":\"0x%08X\","
           "\"cos_scale\":\"0x%08X\","
           "\"atan2\":\"0x%08X\""
           "},"
           "\"unknown_log\":[",
           g_cop.writes, g_cop.reads,
           g_sharc.transform_count, g_sharc.matrix_read_count,
           g_sharc.unknown_cmds, g_sharc.unknown_log_count,
           g_sharc.break_on_unknown  ? "true" : "false",
           g_sharc.unknown_triggered ? "true" : "false",
           g_sharc.unknown_trigger_cmd, g_sharc.unknown_trigger_ip,
           g_sharc.ip_set_pos,      g_sharc.ip_set_ang_x,
           g_sharc.ip_set_ang_y,    g_sharc.ip_set_ang_z,
           g_sharc.ip_read_matrix,  g_sharc.ip_rot_transform,
           g_sharc.ip_full_transform, g_sharc.ip_sin_scale,
           g_sharc.ip_cos_scale,    g_sharc.ip_atan2);

    for (int i = 0; i < g_sharc.unknown_log_count && left > 8; i++) {
        APPEND("%s{\"cmd\":\"0x%08X\",\"first_ip\":\"0x%08X\",\"count\":%u}",
               i ? "," : "",
               g_sharc.unknown_log[i].cmd,
               g_sharc.unknown_log[i].first_ip,
               g_sharc.unknown_log[i].count);
    }

    APPEND("]}");
#undef APPEND
}

static void mcp_cmd_get_geo_captures(char *resp, int cap) {
    if (!g_geo3d_state) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"geo3d not initialized\"}");
        return;
    }
    char *p = resp;
    int   left = cap;
    int   n;
#define GAPPEND(...) do { n = snprintf(p, (size_t)left, __VA_ARGS__); p += n; left -= n; } while(0)
    GAPPEND("{\"ok\":true,\"count\":%d,\"captures\":[", g_geo3d_state->captured_count);
    for (int i = 0; i < g_geo3d_state->captured_count && left > 64; i++) {
        const captured_model_t *cm = &g_geo3d_state->captured[i];
        /* Per-column scale = length of each rotation column (detects squish). */
        float scx = sqrtf(cm->matrix[0]*cm->matrix[0]+cm->matrix[4]*cm->matrix[4]+cm->matrix[8]*cm->matrix[8]);
        float scy = sqrtf(cm->matrix[1]*cm->matrix[1]+cm->matrix[5]*cm->matrix[5]+cm->matrix[9]*cm->matrix[9]);
        float scz = sqrtf(cm->matrix[2]*cm->matrix[2]+cm->matrix[6]*cm->matrix[6]+cm->matrix[10]*cm->matrix[10]);
        GAPPEND("%s{\"idx\":%d,\"model\":%d,\"mesh\":\"0x%X\","
                "\"pos\":[%.3f,%.3f,%.3f],"
                "\"ang\":[%.2f,%.2f,%.2f],"
                "\"have_pos\":%d,\"have_ang\":%d,\"have_mat\":%d,"
                "\"has_matrix\":%d,\"xyz\":[%.3f,%.3f,%.3f],"
                "\"scale\":[%.3f,%.3f,%.3f],"
                "\"up\":[%.2f,%.2f,%.2f],"
                "\"clip\":%d,\"cx\":%d,\"cy\":%d,\"cw\":%d,\"ch\":%d,"
                "\"bone\":%d,\"vs\":%d,\"win\":%d,\"vp\":[%d,%d,%d,%d],\"gp\":[%.1f,%.1f,%.1f,%.1f],\"tpa\":\"0x%X\",\"tha\":\"0x%X\",\"matptr\":\"0x%X\",\"m\":[",
                i ? "," : "",
                i, cm->model_idx, cm->dbg_mesh_ptr,
                cm->dbg_pos[0], cm->dbg_pos[1], cm->dbg_pos[2],
                cm->dbg_ang_deg[0], cm->dbg_ang_deg[1], cm->dbg_ang_deg[2],
                cm->dbg_have_pos, cm->dbg_have_ang, cm->dbg_have_mat,
                cm->has_matrix ? 1 : 0,
                cm->matrix[3], cm->matrix[7], cm->matrix[11],
                scx, scy, scz,
                cm->matrix[1], cm->matrix[5], cm->matrix[9],
                cm->has_clip_win ? 1 : 0, cm->clip_win_x, cm->clip_win_y,
                cm->clip_win_w, cm->clip_win_h,
                cm->from_bone ? 1 : 0, cm->view_space ? 1 : 0, cm->window,
                cm->vp[0], cm->vp[1], cm->vp[2], cm->vp[3],
                cm->gproj[0], cm->gproj[1], cm->gproj[2], cm->gproj[3],
                cm->tpa, cm->tha, cm->material_ptr);
        for (int k = 0; k < 12; k++) GAPPEND("%s%.5g", k ? "," : "", cm->matrix[k]);
        GAPPEND("]}");
    }
    GAPPEND("]}");
#undef GAPPEND
}

/* sound_status: the sound board at a glance — the 68000, the SCSP's interrupt
 * and timer state, which slots are sounding, and the host output ring. */
static void mcp_cmd_sound_status(char *resp, int cap) {
    const scsp_t *sc = &g_sound.scsp;
    uint32_t keyed = 0, active = 0;
    for (int i = 0; i < 32; i++) {
        if (sc->slot[i].r[0] & 0x0800) keyed |= 1u << i;
        if (sc->slot[i].active)        active |= 1u << i;
    }
    uint32_t fill = (g_sound.out_w - g_sound.out_r) & (SOUND_OUT_FRAMES - 1);
    snprintf(resp, (size_t)cap,
             "{\"ok\":true,\"rom_loaded\":%s,\"samples_size\":%u,\"m68k_pc\":\"0x%06X\",\"m68k_sr\":\"0x%04X\","
             "\"cycles\":%llu,\"samples\":%llu,\"irqs\":[%llu,%llu,%llu,%llu,%llu,%llu,%llu],"
             "\"midi_writes\":%llu,\"midi_fifo\":%u,\"scieb\":\"0x%03X\",\"scipd\":\"0x%03X\",\"lines\":\"0x%02X\","
             "\"levels\":[%u,%u,%u],\"timers\":[\"0x%04X\",\"0x%04X\",\"0x%04X\"],\"keyed\":\"0x%08X\",\"active\":\"0x%08X\","
             "\"dsp_steps\":%d,\"out_fill\":%u,\"out_dropped\":%llu,\"midi_drops\":%u,\"midi_hi\":%u,\"midi_drains\":%llu}",
             g_sound.rom_loaded ? "true" : "false", g_sound.samples_size,
             g_sound.m68k.cpu.pc, (unsigned)g_sound.m68k.cpu.sr,
             (unsigned long long)g_sound.m68k.cpu.cycles, (unsigned long long)sc->samples,
             (unsigned long long)g_sound.irqs[1], (unsigned long long)g_sound.irqs[2], (unsigned long long)g_sound.irqs[3],
             (unsigned long long)g_sound.irqs[4], (unsigned long long)g_sound.irqs[5], (unsigned long long)g_sound.irqs[6],
             (unsigned long long)g_sound.irqs[7],
             (unsigned long long)g_sound.write_count, (unsigned)((sc->mi_w - sc->mi_r) & 31),
             sc->c[0x0F], sc->c[0x10], sc->lines, sc->lvl_ta, sc->lvl_tbc, sc->lvl_midi,
             sc->c[0x0C], sc->c[0x0D], sc->c[0x0E], keyed, active,
             sc->dsp.stopped ? -1 : sc->dsp.last_step, fill, (unsigned long long)g_sound.out_dropped,
             sc->mi_drops, sc->mi_hi, (unsigned long long)g_sound.midi_drains);
}

/* reset_sound: reboot the sound board, and/or push raw bytes at its MIDI input.
 *
 *   {"cmd":"reset_sound"}                      reboot the 68000 + SCSP
 *   {"cmd":"reset_sound","restart":0,"midi":"8A0102"}   just send the bytes
 *   {"cmd":"reset_sound","midi":"8A0102"}      reboot, then send them
 *
 * The recovery path for a driver that has lost its command stream: the music
 * dies and stays dead, and this brings it back without dropping the session.
 * After a reboot the driver is silent until the game's next music cue, so
 * `midi` is there to kick a track by hand. */
static void mcp_cmd_reset_sound(const char *req, char *resp, int cap) {
    char midi[512] = {0};
    uint32_t restart = 1;
    mcp_json_get_u32(req, "restart", &restart);
    if (restart) emu_sound_restart(g_mcp.emu);

    int n = 0;
    if (mcp_json_get_str(req, "midi", midi, (int)sizeof midi)) {
        size_t len = strlen(midi);
        if (len == 0 || (len % 2u) != 0u) {
            snprintf(resp, (size_t)cap,
                     "{\"ok\":false,\"error\":\"midi is %u hex chars; want 2 per byte\"}",
                     (unsigned)len);
            return;
        }
        uint8_t bytes[256];
        for (size_t i = 0; i + 2u <= len && n < (int)sizeof bytes; i += 2u) {
            char h[3] = { midi[i], midi[i + 1], 0 };
            char *end = NULL;
            unsigned long v = strtoul(h, &end, 16);
            if (end != h + 2) {
                snprintf(resp, (size_t)cap,
                         "{\"ok\":false,\"error\":\"midi has a non-hex byte at %u\"}", (unsigned)i);
                return;
            }
            bytes[n++] = (uint8_t)v;
        }
        emu_sound_midi(g_mcp.emu, bytes, n);
    }
    const scsp_t *sc = &g_sound.scsp;
    snprintf(resp, (size_t)cap,
             "{\"ok\":true,\"restarted\":%s,\"midi_bytes\":%d,\"m68k_pc\":\"0x%06X\","
             "\"midi_drops\":%u,\"midi_hi\":%u}",
             restart ? "true" : "false", n, g_sound.m68k.cpu.pc, sc->mi_drops, sc->mi_hi);
}

static void mcp_cmd_dump_geo_stream(char *resp, int cap) {
    char *p = resp; int left = cap, n;
    int fe = g_cop.geo_frame_end, fs = g_cop.geo_frame_start;
    int total = fe - fs;
    if (total <= 0 || total > GEO_CAPTURE_SIZE) {
        total = g_cop.geo_capture_count;
        if (total > GEO_CAPTURE_SIZE) total = GEO_CAPTURE_SIZE;
        fe = g_cop.geo_capture_head;
    }
    int head = fe;
#define DAPPEND(...) do { n = snprintf(p, (size_t)left, __VA_ARGS__); p += n; left -= n; } while(0)
    DAPPEND("{\"ok\":true,\"total\":%d,\"cmds\":[", total);
    int first = 1;
    for (int i = 0; i < total && left > 96; ) {
        int idx = (head - total + i + GEO_CAPTURE_SIZE) & (GEO_CAPTURE_SIZE - 1);
        uint32_t cmd = g_cop.geo_capture[idx];
        int na = sharc_args_for_cmd(cmd);
        if (na < 0) na = 0;
        DAPPEND("%s{\"c\":\"0x%08X\",\"a\":[", first ? "" : ",", cmd); first = 0;
        for (int j = 0; j < na && j < 8; j++) {
            uint32_t a = g_cop.geo_capture[(idx + 1 + j) & (GEO_CAPTURE_SIZE - 1)];
            float f; memcpy(&f, &a, 4);
            if (f == f && fabsf(f) > 1e-5f && fabsf(f) < 1e6f) DAPPEND("%s%.3f", j ? "," : "", f);
            else DAPPEND("%s\"0x%X\"", j ? "," : "", a);
        }
        DAPPEND("]}");
        i += 1 + na;
    }
    DAPPEND("]}");
#undef DAPPEND
}

static void mcp_cmd_dump_midi_log(char *resp, int cap) {
    char *p = resp; int left = cap, n;
#define MAPPEND(...) do { n = snprintf(p, (size_t)left, __VA_ARGS__); p += n; left -= n; } while(0)
    MAPPEND("{\"ok\":true,\"count\":%u,\"bytes\":[", g_sound.midi_log_n);
    for (uint32_t i = 0; i < g_sound.midi_log_n && left > 48; i++)
        MAPPEND("%s{\"v\":\"0x%02X\",\"ip\":\"0x%06X\"}",
                i ? "," : "", g_sound.midi_log[i], g_sound.midi_log_ip[i]);
    MAPPEND("]}");
#undef MAPPEND
}

static void mcp_cmd_dump_bones(char *resp, int cap) {
    char *p = resp; int left = cap, n;
#define BAPPEND(...) do { n = snprintf(p, (size_t)left, __VA_ARGS__); p += n; left -= n; } while(0)
    BAPPEND("{\"ok\":true,\"cur_pos\":[%.3f,%.3f,%.3f],\"slots\":[",
            g_sharc.pos[0], g_sharc.pos[1], g_sharc.pos[2]);
    for (int s = 0; s < 4; s++) {           /* pid0 slots 0-3 */
        const float *rc = g_sharc.rot_cache[s];
        const float *tb = g_sharc.tgp_bone[s];
        BAPPEND("%s{\"slot\":%d,"
                "\"rot_cache_T\":[%.3f,%.3f,%.3f],"
                "\"tgp_bone_T\":[%.3f,%.3f,%.3f],"
                "\"rot_cache_R\":[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f]}",
                s ? "," : "", s,
                rc[9], rc[10], rc[11], tb[9], tb[10], tb[11],
                rc[0],rc[1],rc[2],rc[3],rc[4],rc[5],rc[6],rc[7],rc[8]);
    }
    BAPPEND("]}");
#undef BAPPEND
}

/* cop_exec — hand the coprocessor a stream of words, exactly as the i960 does.
 *
 * `dump_model` runs the emulator's own polygon decoder over ROM the running
 * game never has to reach. This is that idea for the coprocessor: a caller
 * hands it the words a real board was captured sending — a set_body and the
 * four IK chains that follow it — and reads the transforms back with
 * `dump_tgp`, so the COP handlers can be measured without driving the game to
 * a fight first.
 *
 * The words go in through cop_write(), which is the MMIO path itself: argument
 * counting, dispatch and all the running matrix state included. An argument
 * count that is wrong by one desyncs here exactly as it would in a game, which
 * is the point of not calling sharc_exec() directly.
 *
 * "reset" clears COP and SHARC state first, so one call cannot inherit the
 * matrix another left behind.
 */
static void mcp_cmd_cop_exec(const char *req, char *resp, int cap) {
    char words[8192];
    uint32_t reset = 0;
    mcp_json_get_u32(req, "reset", &reset);
    if (!mcp_json_get_str(req, "words", words, (int)sizeof(words))) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"missing words\"}");
        return;
    }
    size_t len = strlen(words);
    if (len == 0 || (len % 8u) != 0u) {
        snprintf(resp, (size_t)cap,
                 "{\"ok\":false,\"error\":\"words is %u hex chars; want 8 per 32-bit word\"}",
                 (unsigned)len);
        return;
    }
    int locked = g_mcp.emu && g_mcp.emu->thread_alive;
    if (locked) emu_mutex_lock(&g_mcp.emu->mutex);
    if (reset) cop_reset();
    int n = 0;
    for (size_t i = 0; i + 8u <= len; i += 8u) {
        char w[9];
        memcpy(w, words + i, 8); w[8] = '\0';
        cop_write((uint32_t)strtoul(w, NULL, 16));
        n++;
    }
    if (locked) emu_mutex_unlock(&g_mcp.emu->mutex);
    snprintf(resp, (size_t)cap, "{\"ok\":true,\"words\":%d}", n);
}

/* dump_tgp — the bone slots the geometry decoder draws a fighter from.
 *
 * `dump_bones` is a four-slot summary at three decimal places, for a human
 * reading a debug window. This is the whole 32-slot table — P1 on 0..15, P2 on
 * 16..31, each a column-major 3x4 — with the current matrix beside it, printed
 * with enough digits to be differenced against another implementation rather
 * than eyeballed.
 */
static void mcp_cmd_dump_tgp(char *resp, int cap) {
    char *p = resp; int left = cap, n;
#define TAPPEND(...) do { n = snprintf(p, (size_t)left, __VA_ARGS__); \
                          if (n < 0 || n >= left) n = left - 1; \
                          p += n; left -= n; } while (0)
    TAPPEND("{\"ok\":true,\"pos\":[%.9g,%.9g,%.9g],\"rot\":[",
            g_sharc.pos[0], g_sharc.pos[1], g_sharc.pos[2]);
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++)
            TAPPEND("%s%.9g", (c || r) ? "," : "", g_sharc.rot[c][r]);
    TAPPEND("],\"tgp\":[");
    for (int s = 0; s < 32; s++) {
        TAPPEND("%s[", s ? "," : "");
        for (int k = 0; k < 12; k++)
            TAPPEND("%s%.9g", k ? "," : "", g_sharc.tgp_bone[s][k]);
        TAPPEND("]");
    }
    TAPPEND("]}");
#undef TAPPEND
}

static void mcp_cmd_set_break_on_unknown_cop(const char *req, char *resp, int cap) {
    uint32_t enable = 1;
    mcp_json_get_u32(req, "enable", &enable);
    g_sharc.break_on_unknown  = (int)enable;
    g_sharc.unknown_triggered = 0;
    snprintf(resp, (size_t)cap, "{\"ok\":true,\"break_on_unknown_cop\":%s}",
             enable ? "true" : "false");
}

static void mcp_cmd_enable_breakpoint(const char *req, char *resp, int cap) {
    uint32_t addr = 0;
    if (!mcp_json_get_u32(req, "addr", &addr)) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"missing addr\"}"); return;
    }
    int found = 0;
    for (int i = 0; i < BP_MAX; i++) {
        if (g_bp.list[i].active && g_bp.list[i].addr == addr) {
            g_bp.list[i].enabled = true; found++;
        }
    }
    snprintf(resp, (size_t)cap, "{\"ok\":true,\"updated\":%d}", found);
}

static void mcp_cmd_disable_breakpoint(const char *req, char *resp, int cap) {
    uint32_t addr = 0;
    if (!mcp_json_get_u32(req, "addr", &addr)) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"missing addr\"}"); return;
    }
    int found = 0;
    for (int i = 0; i < BP_MAX; i++) {
        if (g_bp.list[i].active && g_bp.list[i].addr == addr) {
            g_bp.list[i].enabled = false; found++;
        }
    }
    snprintf(resp, (size_t)cap, "{\"ok\":true,\"updated\":%d}", found);
}

static void mcp_cmd_clear_all_breakpoints(char *resp, int cap) {
    int removed = 0;
    for (int i = 0; i < BP_MAX; i++) {
        if (g_bp.list[i].active) { bp_remove(i); removed++; }
    }
    snprintf(resp, (size_t)cap, "{\"ok\":true,\"removed\":%d}", removed);
}

/*
 * Block until the emulator stops running (breakpoint, halt, or manual stop),
 * or until timeout_ms elapses.  Returns the stop reason and final IP.
 * If the emulator is already stopped on entry the response is immediate.
 */
static void mcp_cmd_wait_for_stop(const char *req, char *resp, int cap) {
    uint32_t timeout_ms = 30000;
    mcp_json_get_u32(req, "timeout_ms", &timeout_ms);
    if (timeout_ms > 300000) timeout_ms = 300000;  /* 5-min hard cap */
    if (!g_mcp.emu || !g_mcp.emu->thread_alive) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"emu not started\"}"); return;
    }

    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        if (!emu_is_running(g_mcp.emu)) break;
        emu_sleep_ms(10);
        elapsed += 10;
    }

    int running  = emu_is_running(g_mcp.emu);
    int halted   = g_mcp.cpu && g_mcp.cpu->halted;
    uint32_t ip  = 0;
    if (g_mcp.emu && g_mcp.emu->thread_alive) {
        emu_mutex_lock(&g_mcp.emu->mutex);
        ip = g_mcp.emu->cpu_snapshot.sfr.ip;
        emu_mutex_unlock(&g_mcp.emu->mutex);
    }
    const char *reason = running                  ? "timeout"
                       : halted                   ? "halted"
                       : g_wp.hit                 ? "watchpoint"
                       : g_bp.hit_addr            ? "breakpoint"
                       : g_sharc.unknown_trigger_cmd ? "cop_unknown"
                       : "stopped";
    snprintf(resp, (size_t)cap,
             "{\"ok\":true,\"stopped\":%s,\"reason\":\"%s\","
             "\"ip\":\"0x%08X\",\"elapsed_ms\":%u,"
             "\"cop_cmd\":\"0x%08X\",\"cop_ip\":\"0x%08X\","
             "\"wp_addr\":\"0x%08X\",\"wp_val\":\"0x%08X\",\"wp_ip\":\"0x%08X\",\"wp_write\":%s}",
             running ? "false" : "true", reason, ip, elapsed,
             g_sharc.unknown_trigger_cmd, g_sharc.unknown_trigger_ip,
             g_wp.hit_addr, g_wp.hit_val, g_wp.hit_ip, g_wp.hit_write ? "true" : "false");
}

/* ---- Command dispatch ---------------------------------------------------- */

/* Dump a bus range straight to a file.
 *
 * read_memory caps at 4096 bytes to keep a response inside the 128 kB reply
 * buffer, so pulling texture RAM through it is 512 round trips a sheet. The
 * grading tools want whole regions — two 1 MB sheets, luma RAM, colorxlat —
 * and want them as bytes rather than as hex, so this writes the range out
 * from inside the emulator and hands back only the count. Same reason MAME's
 * capture Lua writes its own files rather than shipping words over the bridge.
 *
 * The read goes through mem_read8 rather than at the region's backing store so
 * a range that spans regions, or one behind an MMIO callback, dumps the same
 * bytes the i960 would see. */
/* dump_geo_list — the GEO display list the renderer walks: the copy published
 * when the game last set the read pointer. Writes <path> as u32 read pointer,
 * u32 publish count, u16 H-sync, u16 V-sync, then bufferram's words. */
/* set_geo_isolate: draw only captured object `index` (-1 for all), to find which
 * object puts a given thing on screen. */
static void mcp_cmd_set_geo_isolate(const char *req, char *resp, int cap) {
    uint32_t idx = 0xFFFFFFFFu;
    mcp_json_get_u32(req, "index", &idx);
    if (!g_geo3d_state) { snprintf(resp, (size_t)cap, "{\"ok\":false}"); return; }
    g_geo3d_state->isolate_index = (idx == 0xFFFFFFFFu) ? -1 : (int)idx;
    { uint32_t dm = 0xFFFFFFFFu; if (mcp_json_get_u32(req, "dump_tex", &dm)) g_dump_model_tex = (dm == 0xFFFFFFFFu) ? -1 : (int)dm; }
    {   /* from/to: draw only captures in [from, to]; to < from turns the range off */
        uint32_t lo = 0, hi = 0;
        if (mcp_json_get_u32(req, "from", &lo) && mcp_json_get_u32(req, "to", &hi)) {
            g_geo3d_state->filter_enabled = hi >= lo;
            g_geo3d_state->filter_min = (int)lo;
            g_geo3d_state->filter_max = (int)hi;
        }
    }
    snprintf(resp, (size_t)cap, "{\"ok\":true,\"isolate\":%d}", g_geo3d_state->isolate_index);
}

static void mcp_cmd_dump_geo_list(const char *req, char *resp, int cap) {
    char path[512] = {0};
    if (!mcp_json_get_str(req, "path", path, sizeof(path))) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"missing path\"}"); return;
    }
    if (!g_mcp.bus || !g_geodl_snap_ready) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"no display list published yet\"}"); return;
    }
    static uint32_t words[BUFF_RAM_SIZE / 4];
    uint32_t head[2]; uint16_t sync[2];
    int locked = g_mcp.emu && g_mcp.emu->thread_alive;
    if (locked) emu_mutex_lock(&g_mcp.emu->mutex);
    memcpy(words, g_geodl_snap, sizeof words);
    head[0] = g_geodl_snap_rstart; head[1] = (uint32_t)g_geodl_snap_seq;
    sync[0] = (uint16_t)mem_read16(g_mcp.bus, H_SYNC_BASE);
    sync[1] = (uint16_t)mem_read16(g_mcp.bus, V_SYNC_BASE);
    if (locked) emu_mutex_unlock(&g_mcp.emu->mutex);
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(head, 4, 2, f) == 2 && fwrite(sync, 2, 2, f) == 2
               && fwrite(words, 4, BUFF_RAM_SIZE / 4, f) == BUFF_RAM_SIZE / 4;
    if (f) fclose(f);
    snprintf(resp, (size_t)cap, "{\"ok\":%s,\"read_start\":%u,\"seq\":%u,\"hsync\":%d,\"vsync\":%d}",
             ok ? "true" : "false", head[0], head[1], (int16_t)sync[0], (int16_t)sync[1]);
}

static void mcp_cmd_dump_memory_file(const char *req, char *resp, int cap) {
    uint32_t addr = 0, size = 0;
    char path[512] = {0};
    if (!mcp_json_get_u32(req, "addr", &addr) || !mcp_json_get_u32(req, "size", &size) ||
        !mcp_json_get_str(req, "path", path, sizeof(path))) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"missing addr, size or path\"}");
        return;
    }
    if (!g_mcp.bus) { snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"bus not ready\"}"); return; }
    if (size > MCP_DUMP_MAX_BYTES) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"size above %u\"}", (unsigned)MCP_DUMP_MAX_BYTES);
        return;
    }

    uint8_t *buf = (uint8_t *)malloc(size ? size : 1);
    if (!buf) { snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"out of memory\"}"); return; }

    /* One consistent snapshot: the i960 must not write half of texture RAM
     * between the first byte and the last. Copy under the emu mutex, write the
     * file outside it — disk I/O inside the critical section stalls the UI. */
    int locked = g_mcp.emu && g_mcp.emu->thread_alive;
    if (locked) emu_mutex_lock(&g_mcp.emu->mutex);
    uint32_t nonzero = 0;
    for (uint32_t i = 0; i < size; i++) {
        uint8_t b = mem_read8(g_mcp.bus, addr + i);
        buf[i] = b;
        if (b) nonzero++;
    }
    if (locked) emu_mutex_unlock(&g_mcp.emu->mutex);

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(buf);
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"cannot open file\"}");
        return;
    }
    size_t wrote = fwrite(buf, 1, size, f);
    fclose(f);
    free(buf);

    snprintf(resp, (size_t)cap,
             "{\"ok\":%s,\"addr\":\"0x%08X\",\"bytes\":%u,\"nonzero\":%u,\"frames\":%u}",
             wrote == size ? "true" : "false", addr, (unsigned)wrote, nonzero, g_emu_frames);
}

/* Block until the game has advanced N frames, then report the frame clock.
 *
 * The capture drivers pace by game frame, not by wall clock: "let it run four
 * frames and dump" has to mean four of the board's frames however fast the host
 * is. A stalled game (no frame hook firing) returns advanced < count rather
 * than hanging, so a caller can tell "slow" from "stopped". */
static void mcp_cmd_wait_frames(const char *req, char *resp, int cap) {
    uint32_t count = 1, timeout_ms = 30000;
    mcp_json_get_u32(req, "count", &count);
    mcp_json_get_u32(req, "timeout_ms", &timeout_ms);
    if (timeout_ms > 300000) timeout_ms = 300000;

    unsigned start = g_emu_frames;
    uint32_t elapsed = 0;
    /* A stopped emulator produces no frames, so give up on one rather than sit
     * out the whole timeout — but only after it has been stopped a while. On
     * startup the bridge is listening before the ROM has finished loading and
     * before --run takes effect, and an immediate bail there would hand every
     * caller reached:false the moment it connected. */
    uint32_t idle_ms = 0;
    while (elapsed < timeout_ms && (g_emu_frames - start) < count) {
        if (g_mcp.emu && !emu_is_running(g_mcp.emu)) {
            idle_ms += 2;
            if (idle_ms >= MCP_STOPPED_GRACE_MS) break;
        } else {
            idle_ms = 0;
        }
        emu_sleep_ms(2);
        elapsed += 2;
    }
    unsigned advanced = g_emu_frames - start;
    snprintf(resp, (size_t)cap,
             "{\"ok\":true,\"frames\":%u,\"advanced\":%u,\"reached\":%s,\"elapsed_ms\":%u}",
             g_emu_frames, advanced, advanced >= count ? "true" : "false", elapsed);
}

/* Decode model-table entries and write the triangles out as a file.
 *
 * This is the emulator's own index-array polygon decoder — the one in geo3d.h,
 * with the connectivity rules CLAUDE.md calls load-bearing — run over a range
 * of the model table with no matrix, so its output can be held against another
 * implementation of the same format.
 *
 * Positions, the texture coordinates and tile rectangle each corner carries,
 * and the face's fill flags. All of it is a pure function of the ROM — the UV
 * stream and the texture headers sit in the texture ROM beside the material
 * records. What the running game has uploaded decides which *texels* are in
 * that rectangle, and none of that is written here.
 *
 * The emit sink is redirected for the duration so the sweep does not fight the
 * render thread for the buffer the current frame is being built in, and
 * geo3d_build_wireframes stands down while it is (see g_geo3d_dump_busy).
 *
 * Format, little-endian throughout:
 *   magic "M2MD" | u32 version=2 | u32 first | u32 count
 *   then per model: u32 index | u32 tris | tris * 20 * f32, each triangle
 *   (x,y,z) * 3 | (u,v) * 3 | tile x,y,w,h (w = 0 untextured) | GEO3D_FACE_* flags
 */
static void mcp_cmd_dump_model(const char *req, char *resp, int cap) {
    uint32_t first = 0, count = 1;
    char path[512] = {0};
    mcp_json_get_u32(req, "model", &first);
    mcp_json_get_u32(req, "count", &count);
    if (!mcp_json_get_str(req, "path", path, sizeof(path))) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"missing path\"}"); return;
    }
    /* `loaded` and not merely allocated. The regions are calloc'd before they
     * are filled, and a profile resolves in between — decoding out of the gap
     * reads zero mesh pointers and reports every model empty, which looks like
     * a decoder that agrees about nothing rather than like a race. */
    if (!g_mcp.romset || !g_mcp.romset->loaded ||
        !g_mcp.romset->main_data || !g_mcp.romset->polygons) {
        snprintf(resp, (size_t)cap,
                 "{\"ok\":false,\"error\":\"no ROM loaded yet\"}"); return;
    }
    if (!g_active_profile) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"no game profile\"}"); return;
    }
    const game_quirks_t *q = &g_active_profile->quirks;
    if (first >= q->model_table_count) {
        snprintf(resp, (size_t)cap,
                 "{\"ok\":false,\"error\":\"model %u is past the table's %u\"}",
                 first, q->model_table_count);
        return;
    }
    if (count > q->model_table_count - first) count = q->model_table_count - first;

    FILE *f = fopen(path, "wb");
    if (!f) { snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"cannot open file\"}"); return; }

    /* One private sink for the whole sweep. It is the same size as the shared
     * one, which is why it is static rather than on the bridge thread's stack. */
    static geo3d_tri_buf_t dump_buf;
    geo3d_tri_buf_t *saved = g_geo3d_tri_sink;
    g_geo3d_dump_busy = 1;
    g_geo3d_tri_sink  = &dump_buf;

    uint32_t hdr[4];
    memcpy(hdr, "M2MD", 4);
    hdr[1] = 2; hdr[2] = first; hdr[3] = count;
    fwrite(hdr, 4, 4, f);

    uint32_t nonempty = 0, total_tris = 0;
    for (uint32_t m = first; m < first + count; m++) {
        dump_buf.count = 0;
        geo3d_decode_model((int)m,
                           g_mcp.romset->main_data, g_mcp.romset->main_data_size,
                           g_mcp.romset->polygons,  g_mcp.romset->polygons_size,
                           g_mcp.romset->textures,  g_mcp.romset->textures_size,
                           q->model_table_offset, q->model_table_count,
                           q->mesh_ptr_subtract, q->mesh_ptr_add,
                           NULL, 1.0f, 1.0f, 1.0f);
        uint32_t n = (uint32_t)dump_buf.count;
        uint32_t rec[2] = { m, n };
        fwrite(rec, 4, 2, f);
        for (uint32_t i = 0; i < n; i++) {
            const geo3d_tri_t *T = &dump_buf.tris[i];
            float v[20] = { T->x0, T->y0, T->z0, T->x1, T->y1, T->z1, T->x2, T->y2, T->z2,
                            T->u0, T->v0, T->u1, T->v1, T->u2, T->v2,
                            T->tx, T->ty, T->tw, T->th, T->fl };
            fwrite(v, 4, 20, f);
        }
        if (n) { nonempty++; total_tris += n; }
    }

    g_geo3d_tri_sink  = saved;
    g_geo3d_dump_busy = 0;
    fclose(f);

    snprintf(resp, (size_t)cap,
             "{\"ok\":true,\"first\":%u,\"count\":%u,\"nonempty\":%u,\"tris\":%u}",
             first, count, nonempty, total_tris);
}

/* Record the display list: every write to the geometry processor and the
 * coprocessor for `frames` whole frames, in write order. See the display-list
 * tap in memory.h for what is recorded and why.
 *
 * Request: frames, path, probes ("addr:size,addr:size,..." read at each frame
 * edge, size 1/2/4), optional max_words and timeout_ms, and optional lo / hi
 * to record only part of the window (the coprocessor FIFO alone is a fraction
 * of the words, which is what makes a long capture of the rig affordable).
 *
 * Writes the explorer toolkit's MAME capture format, little-endian:
 *   <path>.bin    (u32 address, u32 value) per write
 *   <path>.json   {"words", "frames", "overflow", "probes", "marks"} where each
 *                 mark is [frame, word index, probe values...] and a pair of
 *                 consecutive marks brackets exactly one frame
 * The tools lay the probes out in whichever order a given check reads them. */
/* capture_snd {path, frames, timeout_ms, async}: the sound board's side of the
 * next `frames` game frames, in tools/mame/snd-capture.lua's format (sound.h).
 * With async:1 it only arms (so a driver can arm before emu_run and catch
 * power-on); capture_snd_finish then waits for it and writes the index. */
static char     g_sndcap_path[512];
static uint32_t (*g_sndcap_marks)[4];

static void mcp_cmd_capture_snd_finish(const char *req, char *resp, int cap) {
    uint32_t timeout_ms = 600000;
    mcp_json_get_u32(req, "timeout_ms", &timeout_ms);
    if (!g_sndcap_marks) { snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"no capture armed\"}"); return; }
    uint32_t elapsed = 0, idle_ms = 0;
    while (!g_sndcap.done && elapsed < timeout_ms) {
        if (!emu_is_running(g_mcp.emu)) { idle_ms += 5; if (idle_ms >= MCP_STOPPED_GRACE_MS) break; }
        else idle_ms = 0;
        emu_sleep_ms(5);
        elapsed += 5;
    }
    emu_mutex_lock(&g_mcp.emu->mutex);
    sndcap_stop();
    uint32_t n = g_sndcap.n, nmarks = g_sndcap.nmarks;
    uint32_t (*marks)[4] = g_sndcap_marks;
    g_sndcap.marks = NULL; g_sndcap_marks = NULL;
    emu_mutex_unlock(&g_mcp.emu->mutex);

    char file[600];
    snprintf(file, sizeof file, "%s.json", g_sndcap_path);
    FILE *m = fopen(file, "w");
    if (m) {
        fprintf(m, "{\"source\":\"m2hle-snd\",\"records\":%u,\"ram_base\":4096,\"ram_size\":16384,\"regs_words\":536,\"marks\":[", n);
        for (uint32_t i = 0; i < nmarks; i++)
            fprintf(m, "%s[%u,%u,%u,%u]", i ? "," : "", marks[i][0], marks[i][1], marks[i][2], marks[i][3]);
        fprintf(m, "]}\n");
        fclose(m);
    }
    free(marks);
    snprintf(resp, (size_t)cap, "{\"ok\":%s,\"records\":%u,\"frames\":%u}", m ? "true" : "false", n, nmarks);
}

static void mcp_cmd_capture_snd(const char *req, char *resp, int cap) {
    uint32_t frames = 600, async = 0;
    char path[512] = {0};
    mcp_json_get_u32(req, "frames", &frames);
    mcp_json_get_u32(req, "async", &async);
    if (!mcp_json_get_str(req, "path", path, sizeof(path))) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"missing path\"}"); return;
    }
    if (!g_mcp.emu) { snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"emulator not ready\"}"); return; }
    if (g_sndcap_marks) { snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"a capture is already armed\"}"); return; }
    if (frames == 0) frames = 1;
    char file[600];
    uint32_t (*marks)[4] = calloc((size_t)frames + 2, sizeof *marks);
    snprintf(file, sizeof file, "%s.bin", path);      FILE *f  = fopen(file, "wb");
    snprintf(file, sizeof file, "%s.ram.bin", path);  FILE *fr = fopen(file, "wb");
    snprintf(file, sizeof file, "%s.regs.bin", path); FILE *fg = fopen(file, "wb");
    if (!marks || !f || !fr || !fg) {
        if (f) fclose(f);
        if (fr) fclose(fr);
        if (fg) fclose(fg);
        free(marks);
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"cannot open output\"}"); return;
    }
    snprintf(g_sndcap_path, sizeof g_sndcap_path, "%s", path);
    g_sndcap_marks = marks;
    emu_mutex_lock(&g_mcp.emu->mutex);
    memset(&g_sndcap, 0, sizeof g_sndcap);
    g_sndcap.f = f; g_sndcap.ramf = fr; g_sndcap.regsf = fg;
    g_sndcap.marks = marks; g_sndcap.want = frames;
    g_sndcap.active = 1;
    emu_mutex_unlock(&g_mcp.emu->mutex);
    if (async) { snprintf(resp, (size_t)cap, "{\"ok\":true,\"armed\":true}"); return; }
    mcp_cmd_capture_snd_finish(req, resp, cap);
}

static void mcp_cmd_capture_dl(const char *req, char *resp, int cap) {
    uint32_t frames = 60, max_words = 8u * 1024u * 1024u, timeout_ms = 120000;
    uint32_t lo = DL_TAP_LO, hi = DL_TAP_HI, want_tgp = 0, want_slots = 0, want_unit = 0, want_cop = 0;
    char path[512] = {0}, probes[2048] = {0}, blockspec[512] = {0};
    mcp_json_get_str(req, "blocks", blockspec, sizeof(blockspec));
    mcp_json_get_u32(req, "tgp", &want_tgp);
    mcp_json_get_u32(req, "slots", &want_slots);
    mcp_json_get_u32(req, "unit", &want_unit);
    /* cop: the coprocessor conversation in a MAME SHARC-side capture's format
     * (tests/cop_replay), with <path>.bufram.bin and <path>.dm.bin beside it. */
    mcp_json_get_u32(req, "cop", &want_cop);
    mcp_json_get_u32(req, "lo", &lo);
    mcp_json_get_u32(req, "hi", &hi);
    if (lo < DL_TAP_LO) lo = DL_TAP_LO;
    if (hi > DL_TAP_HI || hi <= lo) hi = DL_TAP_HI;
    mcp_json_get_u32(req, "frames", &frames);
    mcp_json_get_u32(req, "max_words", &max_words);
    mcp_json_get_u32(req, "timeout_ms", &timeout_ms);
    mcp_json_get_str(req, "probes", probes, sizeof(probes));
    if (!mcp_json_get_str(req, "path", path, sizeof(path))) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"missing path\"}"); return;
    }
    if (!g_mcp.bus || !g_mcp.emu) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"emulator not ready\"}"); return;
    }
    if (frames == 0 || frames > 3600) frames = frames ? 3600 : 1;
    if (max_words > 64u * 1024u * 1024u) max_words = 64u * 1024u * 1024u;

    uint32_t paddr[DL_MAX_PROBES]; uint8_t psize[DL_MAX_PROBES]; int np = 0;
    for (char *tok = strtok(probes, ","); tok && np < DL_MAX_PROBES; tok = strtok(NULL, ",")) {
        char *colon = strchr(tok, ':');
        paddr[np] = (uint32_t)strtoul(tok, NULL, 16);
        uint32_t sz = colon ? (uint32_t)strtoul(colon + 1, NULL, 10) : 4;
        psize[np++] = (uint8_t)(sz == 1 || sz == 2 ? sz : 4);
    }

    /* blocks: "hexaddr:hexlen,..." -- whole ranges copied at every mark into
     * <path>.blocks.bin, capmarks x (sum of lengths) bytes. */
    uint32_t baddr[DL_MAX_BLOCKS], blen[DL_MAX_BLOCKS], bbytes = 0; int nb = 0;
    for (char *tok = strtok(blockspec, ","); tok && nb < DL_MAX_BLOCKS; tok = strtok(NULL, ",")) {
        char *colon = strchr(tok, ':');
        if (!colon) continue;
        uint32_t len = (uint32_t)strtoul(colon + 1, NULL, 16);
        if (!len || bbytes + len > DL_BLOCK_BYTES) continue;
        baddr[nb] = (uint32_t)strtoul(tok, NULL, 16); blen[nb++] = len; bbytes += len;
    }

    dl_rec_t  *recs  = (dl_rec_t *)malloc((size_t)max_words * sizeof(dl_rec_t));
    dl_mark_t *marks = (dl_mark_t *)calloc((size_t)frames + 2, sizeof(dl_mark_t));
    const size_t tgp_per_mark = sizeof g_sharc.tgp_bone / sizeof(float);
    float     *tgp   = want_tgp ? (float *)calloc(((size_t)frames + 2) * tgp_per_mark, sizeof(float)) : NULL;
    uint32_t  *slots = want_slots ? (uint32_t *)calloc(((size_t)frames + 2) * DL_SLOT_WORDS, sizeof(uint32_t)) : NULL;
    const size_t unit_per_mark = sizeof g_sharc.rot_cache / sizeof(float);
    float     *unit  = want_unit ? (float *)calloc(((size_t)frames + 2) * unit_per_mark, sizeof(float)) : NULL;
    uint8_t   *blocks = nb ? (uint8_t *)calloc(((size_t)frames + 2), bbytes) : NULL;
    uint8_t   *cop_bufram = want_cop ? (uint8_t *)calloc(BUFF_RAM_SIZE, 1) : NULL;
    uint32_t  *cop_dm = want_cop ? (uint32_t *)calloc(0x1000, sizeof(uint32_t)) : NULL;
    if (!recs || !marks || (want_tgp && !tgp) || (want_slots && !slots) || (want_unit && !unit) || (nb && !blocks)
        || (want_cop && (!cop_bufram || !cop_dm))) {
        free(recs); free(marks); free(tgp); free(slots); free(unit); free(blocks); free(cop_bufram); free(cop_dm);
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"out of memory\"}"); return;
    }

    emu_mutex_lock(&g_mcp.emu->mutex);
    memset(&g_dl, 0, sizeof g_dl);
    g_dl.recs = recs;   g_dl.cap = max_words;
    g_dl.marks = marks; g_dl.capmarks = (size_t)frames + 2;
    g_dl.want = frames;
    g_dl.lo = lo; g_dl.hi = hi;
    g_dl.tgp = tgp;
    g_dl.slots = slots;
    g_dl.unit = unit;
    g_dl.nblocks = nb; g_dl.block_bytes = bbytes; g_dl.blocks = blocks;
    memcpy(g_dl.block_addr, baddr, sizeof(uint32_t) * (size_t)nb);
    memcpy(g_dl.block_len, blen, sizeof(uint32_t) * (size_t)nb);
    g_dl.cop = want_cop != 0; g_dl.cop_bufram = cop_bufram; g_dl.cop_dm = cop_dm;
    g_cop_tap = want_cop ? dl_cop_tap : NULL;
    g_dl.nprobes = np;
    memcpy(g_dl.probe_addr, paddr, sizeof(uint32_t) * (size_t)np);
    memcpy(g_dl.probe_size, psize, (size_t)np);
    g_dl.armed = 1;
    emu_mutex_unlock(&g_mcp.emu->mutex);

    /* Wait out the frames without holding the mutex, as wait_frames does. */
    uint32_t elapsed = 0, idle_ms = 0;
    while (!g_dl.done && elapsed < timeout_ms) {
        if (!emu_is_running(g_mcp.emu)) {
            idle_ms += 2;
            if (idle_ms >= MCP_STOPPED_GRACE_MS) break;
        } else {
            idle_ms = 0;
        }
        emu_sleep_ms(2);
        elapsed += 2;
    }

    emu_mutex_lock(&g_mcp.emu->mutex);
    g_dl.armed = 0; g_dl.active = 0;
    size_t n = g_dl.n, nmarks = g_dl.nmarks;
    int overflow = g_dl.overflow, done = g_dl.done;
    g_dl.recs = NULL; g_dl.marks = NULL; g_dl.tgp = NULL; g_dl.slots = NULL; g_dl.unit = NULL; g_dl.cap = g_dl.capmarks = 0;
    g_dl.blocks = NULL; g_dl.nblocks = 0; g_dl.block_bytes = 0;
    g_dl.cop = 0; g_dl.cop_bufram = NULL; g_dl.cop_dm = NULL; g_cop_tap = NULL;
    emu_mutex_unlock(&g_mcp.emu->mutex);

    char file[600];
    snprintf(file, sizeof file, "%s.bin", path);
    FILE *f = fopen(file, "wb");
    int wrote_ok = f != NULL;
    if (f) {
        for (size_t i = 0; i < n; i++) {
            uint32_t pair[2] = { recs[i].addr, recs[i].val };
            if (fwrite(pair, 4, 2, f) != 2) { wrote_ok = 0; break; }
        }
        fclose(f);
    }
    snprintf(file, sizeof file, "%s.json", path);
    f = wrote_ok ? fopen(file, "w") : NULL;
    if (f) {
        fprintf(f, "{\"words\":%zu,\"frames\":%zu,\"overflow\":%s,\"probes\":[",
                n, nmarks ? nmarks - 1 : 0, overflow ? "true" : "false");
        for (int i = 0; i < np; i++)
            fprintf(f, "%s\"0x%06X:%u\"", i ? "," : "", paddr[i], psize[i]);
        fprintf(f, "],\"marks\":[");
        for (size_t m = 0; m < nmarks; m++) {
            fprintf(f, "%s[%u,%u", m ? "," : "", marks[m].frame, marks[m].index);
            for (int i = 0; i < np; i++) fprintf(f, ",%u", marks[m].probe[i]);
            fprintf(f, "]");
        }
        fprintf(f, "]}\n");
        fclose(f);
    } else {
        wrote_ok = 0;
    }
    if (tgp && wrote_ok) {
        /* <path>.tgp.bin: one record per mark, 32 slots × 12 f32 in the HLE's
         * tgp_bone order (P1 slots 0..15, then P2's). */
        snprintf(file, sizeof file, "%s.tgp.bin", path);
        f = fopen(file, "wb");
        if (f) {
            if (fwrite(tgp, sizeof(float) * tgp_per_mark, nmarks, f) != nmarks) wrote_ok = 0;
            fclose(f);
        } else {
            wrote_ok = 0;
        }
    }
    /* <path>.slots.bin: DL_SLOT_WORDS bufferram words per mark (a MAME capture's
     * TGP layout); <path>.unit.bin: the unit-matrix cache, 32 × 12 f32 per mark. */
    if (slots && wrote_ok) {
        snprintf(file, sizeof file, "%s.slots.bin", path);
        f = fopen(file, "wb");
        if (!f || fwrite(slots, sizeof(uint32_t) * DL_SLOT_WORDS, nmarks, f) != nmarks) wrote_ok = 0;
        if (f) fclose(f);
    }
    if (unit && wrote_ok) {
        snprintf(file, sizeof file, "%s.unit.bin", path);
        f = fopen(file, "wb");
        if (!f || fwrite(unit, sizeof(float) * unit_per_mark, nmarks, f) != nmarks) wrote_ok = 0;
        if (f) fclose(f);
    }
    if (blocks && wrote_ok) {
        snprintf(file, sizeof file, "%s.blocks.bin", path);
        f = fopen(file, "wb");
        if (!f || fwrite(blocks, bbytes, nmarks, f) != nmarks) wrote_ok = 0;
        if (f) fclose(f);
    }
    if (cop_bufram && wrote_ok) {
        snprintf(file, sizeof file, "%s.bufram.bin", path);
        f = fopen(file, "wb");
        if (!f || fwrite(cop_bufram, 1, BUFF_RAM_SIZE, f) != BUFF_RAM_SIZE) wrote_ok = 0;
        if (f) fclose(f);
        snprintf(file, sizeof file, "%s.dm.bin", path);
        f = fopen(file, "wb");
        if (!f || fwrite(cop_dm, sizeof(uint32_t), 0x1000, f) != 0x1000) wrote_ok = 0;
        if (f) fclose(f);
    }
    free(cop_bufram);
    free(cop_dm);
    free(recs);
    free(marks);
    free(tgp);
    free(slots);
    free(unit);
    free(blocks);

    snprintf(resp, (size_t)cap,
             "{\"ok\":%s,\"words\":%zu,\"frames\":%zu,\"complete\":%s,\"overflow\":%s%s}",
             wrote_ok ? "true" : "false", n, nmarks ? nmarks - 1 : 0,
             done ? "true" : "false", overflow ? "true" : "false",
             wrote_ok ? "" : ",\"error\":\"cannot write capture\"");
}

static void mcp_dispatch(const char *req, char *resp, int cap) {
    char cmd[64] = {0};
    if (!mcp_json_get_str(req, "cmd", cmd, sizeof(cmd))) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"missing cmd\"}");
        return;
    }

    if      (strcmp(cmd, "get_status")       == 0) mcp_cmd_get_status(resp, cap);
    else if (strcmp(cmd, "set_input")        == 0) mcp_cmd_set_input(req, resp, cap);
    else if (strcmp(cmd, "set_camera")       == 0) mcp_cmd_set_camera(req, resp, cap);
    else if (strcmp(cmd, "get_registers")    == 0) mcp_cmd_get_registers(resp, cap);
    else if (strcmp(cmd, "read_memory")      == 0) mcp_cmd_read_memory(req, resp, cap);
    else if (strcmp(cmd, "write_memory")     == 0) mcp_cmd_write_memory(req, resp, cap);
    else if (strcmp(cmd, "dump_memory_file") == 0) mcp_cmd_dump_memory_file(req, resp, cap);
    else if (strcmp(cmd, "wait_frames")      == 0) mcp_cmd_wait_frames(req, resp, cap);
    else if (strcmp(cmd, "dump_model")       == 0) mcp_cmd_dump_model(req, resp, cap);
    else if (strcmp(cmd, "capture_dl")       == 0) mcp_cmd_capture_dl(req, resp, cap);
    else if (strcmp(cmd, "match_replay")     == 0) mcp_cmd_match_replay(resp, cap);
    else if (strcmp(cmd, "capture_snd")      == 0) mcp_cmd_capture_snd(req, resp, cap);
    else if (strcmp(cmd, "capture_snd_finish") == 0) mcp_cmd_capture_snd_finish(req, resp, cap);
    else if (strcmp(cmd, "dump_geo_list")    == 0) mcp_cmd_dump_geo_list(req, resp, cap);
    else if (strcmp(cmd, "set_geo_isolate")  == 0) mcp_cmd_set_geo_isolate(req, resp, cap);
    else if (strcmp(cmd, "emu_run")          == 0) mcp_cmd_emu_run(resp, cap);
    else if (strcmp(cmd, "emu_stop")         == 0) mcp_cmd_emu_stop(resp, cap);
    else if (strcmp(cmd, "emu_step")         == 0) mcp_cmd_emu_step(req, resp, cap);
    else if (strcmp(cmd, "set_breakpoint")   == 0) mcp_cmd_set_breakpoint(req, resp, cap);
    else if (strcmp(cmd, "clear_breakpoint")    == 0) mcp_cmd_clear_breakpoint(req, resp, cap);
    else if (strcmp(cmd, "enable_breakpoint")   == 0) mcp_cmd_enable_breakpoint(req, resp, cap);
    else if (strcmp(cmd, "disable_breakpoint")  == 0) mcp_cmd_disable_breakpoint(req, resp, cap);
    else if (strcmp(cmd, "clear_all_breakpoints")== 0) mcp_cmd_clear_all_breakpoints(resp, cap);
    else if (strcmp(cmd, "list_breakpoints")    == 0) mcp_cmd_list_breakpoints(resp, cap);
    else if (strcmp(cmd, "set_watchpoint")      == 0) mcp_cmd_set_watchpoint(req, resp, cap);
    else if (strcmp(cmd, "clear_watchpoint")    == 0) mcp_cmd_clear_watchpoint(req, resp, cap);
    else if (strcmp(cmd, "clear_all_watchpoints")== 0) mcp_cmd_clear_all_watchpoints(resp, cap);
    else if (strcmp(cmd, "list_watchpoints")    == 0) mcp_cmd_list_watchpoints(resp, cap);
    else if (strcmp(cmd, "wait_for_stop")            == 0) mcp_cmd_wait_for_stop(req, resp, cap);
    else if (strcmp(cmd, "set_break_on_unknown_cop") == 0) mcp_cmd_set_break_on_unknown_cop(req, resp, cap);
    else if (strcmp(cmd, "get_cop_diagnostics")      == 0) mcp_cmd_get_cop_diagnostics(resp, cap);
    else if (strcmp(cmd, "get_geo_captures")         == 0) mcp_cmd_get_geo_captures(resp, cap);
    else if (strcmp(cmd, "dump_bones")               == 0) mcp_cmd_dump_bones(resp, cap);
    else if (strcmp(cmd, "dump_tgp")                 == 0) mcp_cmd_dump_tgp(resp, cap);
    else if (strcmp(cmd, "cop_exec")                 == 0) mcp_cmd_cop_exec(req, resp, cap);
    else if (strcmp(cmd, "sound_status")             == 0) mcp_cmd_sound_status(resp, cap);
    else if (strcmp(cmd, "reset_sound")              == 0) mcp_cmd_reset_sound(req, resp, cap);
    else if (strcmp(cmd, "dump_midi_log")            == 0) mcp_cmd_dump_midi_log(resp, cap);
    else if (strcmp(cmd, "read_wave")                == 0) {   /* sound RAM bytes */
        uint32_t addr=0,len=0; mcp_json_get_u32(req,"addr",&addr); mcp_json_get_u32(req,"len",&len);
        if (len>256) len=256;
        char *p=resp; int left=cap; int n;
        n=snprintf(p,(size_t)left,"{\"ok\":true,\"addr\":\"%06X\",\"b\":[",addr); p+=n; left-=n;
        for (uint32_t i=0;i<len && (addr+i)<SOUND_RAM_SIZE && left>6;i++){
            n=snprintf(p,(size_t)left,"%s%u",i?",":"",g_sound.ram[addr+i]); p+=n; left-=n;
        }
        snprintf(p,(size_t)left,"]}");
    }
    else if (strcmp(cmd, "read_comm")                == 0) {   /* SCSP register bytes, no side effects */
        uint32_t addr=0,len=0; mcp_json_get_u32(req,"addr",&addr); mcp_json_get_u32(req,"len",&len);
        if (len>256) len=256;
        char *p=resp; int left=cap; int n;
        n=snprintf(p,(size_t)left,"{\"ok\":true,\"addr\":\"%03X\",\"b\":[",addr); p+=n; left-=n;
        for (uint32_t i=0;i<len && (addr+i)<M68K_SCSP_SIZE && left>6;i++){
            uint16_t w = scsp_peek16(&g_sound.scsp, (addr+i) & ~1u);
            n=snprintf(p,(size_t)left,"%s%u",i?",":"",((addr+i)&1) ? (w&0xFF) : (w>>8)); p+=n; left-=n;
        }
        snprintf(p,(size_t)left,"]}");
    }
    else if (strcmp(cmd, "dump_geo_stream")          == 0) mcp_cmd_dump_geo_stream(resp, cap);
    else if (strcmp(cmd, "set_shadow_floor")         == 0) {
        char ystr[32] = {0};
        if (mcp_json_get_str(req, "y", ystr, sizeof(ystr)))
            g_geo_shadow_floor_y = (float)atof(ystr);
        snprintf(resp, (size_t)cap, "{\"ok\":true,\"shadow_floor_y\":%.3f}", g_geo_shadow_floor_y);
    }
    else if (strcmp(cmd, "dump_face_uv")              == 0) {
        char *p = resp; int left = cap; int n;
        n = snprintf(p, (size_t)left, "{\"ok\":true,\"faces\":["); p += n; left -= n;
        for (int i = 0; i < g_dbg_face_uv_n && left > 200; i++) {
            dbg_face_uv_t *d = &g_dbg_face_uv[i];
            n = snprintf(p, (size_t)left,
                "%s{\"m\":%d,\"texx\":%d,\"texy\":%d,\"texw\":%d,\"texh\":%d,"
                "\"sheet\":%d,\"tri\":%d,\"pu0\":%d,\"pv0\":%d,"
                "\"au\":[%.4f,%.4f,%.4f,%.4f],\"av\":[%.4f,%.4f,%.4f,%.4f]}",
                i ? "," : "", d->model, d->texx, d->texy, d->texw, d->texh,
                d->texsheet, d->tri, d->pu0, d->pv0,
                d->au[0],d->au[1],d->au[2],d->au[3], d->av[0],d->av[1],d->av[2],d->av[3]);
            p += n; left -= n;
        }
        snprintf(p, (size_t)left, "]}");
    }
    else if (strcmp(cmd, "dump_tex_stats")            == 0) {
        snprintf(resp, (size_t)cap,
            "{\"ok\":true,\"models\":%ld,\"models_uv\":%ld,\"models_mat\":%ld,"
            "\"faces\":%ld,\"textured\":%ld,\"uv_faces\":%ld}",
            g_dbg_tex_models, g_dbg_tex_models_uv, g_dbg_tex_models_mat,
            g_dbg_tex_faces, g_dbg_tex_textured, g_dbg_tex_uv_faces);
    }
    else snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"unknown cmd: %s\"}", cmd);
}

/* ---- Bridge thread -------------------------------------------------------- */

static void mcp_bridge_serve(mcp_sock_t client) {
    char req_buf[8192];
    char resp_buf[131072];
    int  req_len = 0;

    while (g_mcp.alive) {
        char chunk[512];
#ifdef _WIN32
        int r = recv(client, chunk, (int)sizeof(chunk) - 1, 0);
#else
        int r = (int)recv(client, chunk, sizeof(chunk) - 1, 0);
#endif
        if (r <= 0) break;
        chunk[r] = '\0';

        /* Accumulate until we have a newline-terminated command. */
        int chunk_i = 0;
        while (chunk_i < r) {
            char c = chunk[chunk_i++];
            if (c == '\n' || c == '\r') {
                if (req_len > 0) {
                    req_buf[req_len] = '\0';
                    mcp_dispatch(req_buf, resp_buf, (int)sizeof(resp_buf));

                    /* Append newline terminator for the Python side. */
                    int resp_len = (int)strlen(resp_buf);
                    resp_buf[resp_len]     = '\n';
                    resp_buf[resp_len + 1] = '\0';
#ifdef _WIN32
                    send(client, resp_buf, resp_len + 1, 0);
#else
                    send(client, resp_buf, (size_t)(resp_len + 1), 0);
#endif
                    req_len = 0;
                }
            } else {
                if (req_len < (int)sizeof(req_buf) - 1)
                    req_buf[req_len++] = c;
            }
        }
    }
    mcp_close(client);
}

#ifdef _WIN32
static DWORD WINAPI mcp_thread_proc(LPVOID arg) {
    (void)arg;
#else
static void *mcp_thread_proc(void *arg) {
    (void)arg;
#endif
    while (g_mcp.alive) {
        struct sockaddr_in client_addr;
#ifdef _WIN32
        int addr_len = sizeof(client_addr);
#else
        socklen_t addr_len = sizeof(client_addr);
#endif
        mcp_sock_t client = accept(g_mcp.listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client == MCP_INVALID_SOCK) {
            if (g_mcp.alive) emu_sleep_ms(10);
            continue;
        }
        LOG_INFO("mcp: client connected");
        mcp_bridge_serve(client);
        LOG_INFO("mcp: client disconnected");
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ---- Public API ---------------------------------------------------------- */

static inline void mcp_bridge_init(emu_thread_ctx_t *emu, i960_cpu_t *cpu, memory_bus_t *bus) {
    g_mcp.emu = emu;
    g_mcp.cpu = cpu;
    g_mcp.bus = bus;
}

/* The assembled ROM regions, once a set has been loaded. dump_model decodes
 * straight out of these rather than out of the running machine. */
static inline void mcp_bridge_set_romset(const romset_t *rs) { g_mcp.romset = rs; }

static inline int mcp_bridge_start(int port) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOG_ERROR("mcp: WSAStartup failed (%d)", WSAGetLastError());
        return -1;
    }
#endif
    mcp_sock_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == MCP_INVALID_SOCK) {
        LOG_ERROR("mcp: socket() failed (%d)", mcp_sockerr());
        return -1;
    }

    int opt = 1;
#ifdef _WIN32
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#else
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_ERROR("mcp: bind() failed on port %d (%d)", port, mcp_sockerr());
        mcp_close(sock);
        return -1;
    }
    if (listen(sock, 1) != 0) {
        LOG_ERROR("mcp: listen() failed (%d)", mcp_sockerr());
        mcp_close(sock);
        return -1;
    }

    g_mcp.listen_sock = sock;
    g_mcp.port        = port;
    g_mcp.alive       = 1;

#ifdef _WIN32
    g_mcp.thread = CreateThread(NULL, 0, mcp_thread_proc, NULL, 0, NULL);
#else
    pthread_create(&g_mcp.thread, NULL, mcp_thread_proc, NULL);
#endif
    LOG_INFO("mcp: bridge listening on 127.0.0.1:%d", port);
    return 0;
}

static inline void mcp_bridge_shutdown(void) {
    if (!g_mcp.alive) return;
    g_mcp.alive = 0;
    mcp_close(g_mcp.listen_sock);
#ifdef _WIN32
    if (g_mcp.thread) { WaitForSingleObject(g_mcp.thread, 1000); CloseHandle(g_mcp.thread); g_mcp.thread = NULL; }
    WSACleanup();
#else
    pthread_join(g_mcp.thread, NULL);
#endif
    LOG_INFO("mcp: bridge stopped");
}

#endif /* MCP_BRIDGE_H */
