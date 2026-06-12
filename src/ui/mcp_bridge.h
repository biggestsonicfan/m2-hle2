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
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
   typedef int mcp_sock_t;
#  define MCP_INVALID_SOCK  (-1)
#  define mcp_close(s)      close(s)
#  define mcp_sockerr()     errno
#endif

/* ---- Module state -------------------------------------------------------- */

typedef struct {
    int               enabled;
    int               port;
    emu_thread_ctx_t *emu;
    i960_cpu_t       *cpu;
    memory_bus_t     *bus;

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
             "\"ip\":\"0x%08X\",\"steps_per_second\":%u,\"profile\":\"%s\"}",
             running ? "true" : "false",
             halted  ? "true" : "false",
             ip, sps, profile_id);
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
                "\"bone\":%d}",
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
                cm->from_bone ? 1 : 0);
    }
    GAPPEND("]}");
#undef GAPPEND
}

static void mcp_cmd_sound_status(char *resp, int cap) {
    int active = 0;
    for (int i = 0; i < SCSP_VOICES; i++)
        if (g_scsp.voices[i].active) active++;
    /* 68K driver work RAM: a6 base = 0x1000, a5 = SCSP @ 0x100000.
     * Sequencer tracks at a6+0x2000 (= wave[0x3000]+n*0x10), active = bit7 of byte 0.
     * Timer reloads at a6+0x1440/0x1441 (= wave[0x2440/0x2441]); master flag a6+0x1406. */
    int seq_active = 0;
    for (int n = 0; n < 8; n++)
        if (g_sound.wave[0x3000 + n*0x10] & 0x80) seq_active++;
    unsigned tb = g_sound.wave[0x2440], ta = g_sound.wave[0x2441];
    unsigned master = g_sound.wave[0x2406];
    /* command ring ($1404 wptr / $1406 count / $1408 rptr, a6=0x1000) */
    unsigned ring_w = (g_sound.wave[0x2404]<<8)|g_sound.wave[0x2405];
    unsigned ring_c = g_sound.wave[0x2406];
    unsigned ring_r = (g_sound.wave[0x2408]<<8)|g_sound.wave[0x2409];
    /* Survey soundram (wave RAM) for any copied sample data, sampled every 256B.
     * The SCSP plays from soundram only, so samples must be copied here. */
    int wave_nz = 0;
    for (uint32_t k = 0; k < M68K_WAVE_SIZE; k += 256)
        if (g_sound.wave[k]) wave_nz++;
    /* IRQ delivery diagnostics */
    unsigned inten = (g_mcp.cpu && (g_mcp.cpu->sfr.pc & 0x2000)) ? 1 : 0;
    unsigned sqc = 0, sqs = 0xFF;
    if (g_mcp.bus && g_active_profile) {
        uint32_t ca = g_active_profile->quirks.sound_queue_count_addr;
        uint32_t sa = g_active_profile->quirks.sound_queue_state_addr;
        if (ca) sqc = mem_read8(g_mcp.bus, ca);
        if (sa) sqs = mem_read8(g_mcp.bus, sa);
    }
    snprintf(resp, (size_t)cap,
             "{\"ok\":true,"
             "\"i960_midi_writes\":%llu,"      /* i960 → 68K MIDI bytes sent       */
             "\"i960_comm_reads\":%llu,"
             "\"m68k_pc\":\"0x%06X\","          /* where the 68K driver is executing */
             "\"m68k_steps\":%u,"
             "\"rom_loaded\":%s,"
             "\"scsp_keyon_count\":%u,\"scsp_keyoff_count\":%u,"
             "\"noteon\":%u,\"noteoff\":%u,\"m68k_sr\":\"0x%04X\","
             "\"audio_cb_count\":%u,"           /* sokol_audio callback fires       */
             "\"active_voices\":%d,"            /* voices currently sounding        */
             "\"scsp_loaded\":%s,"
             "\"saudio_valid\":%s,"
             "\"sample_rate\":%d,"
             "\"sample_rom\":%s,"
             "\"seq_tracks_active\":%d,"        /* 68K sequencer tracks with a song   */
             "\"timer_b_reload\":%u,"
             "\"timer_a_reload\":%u,"
             "\"master_flag\":\"0x%02X\","      /* a6+0x1406; 0xFF = sound disabled    */
             "\"irq_intreq\":\"0x%X\",\"irq_intena\":\"0x%X\","
             "\"irq_delivered\":%llu,\"i960_inten\":%u,"
             "\"snd_q_count\":%u,\"snd_q_state\":\"0x%02X\","
             "\"scsp_tB_hi\":\"0x%02X\",\"scsp_tB_lo\":\"0x%02X\","
             "\"ko_oct\":%u,\"ko_fns\":%u,\"ko_tl\":%u,\"ko_disdl\":%u,\"ko_dipan\":%u,"
             "\"ko_step\":%.4f,\"ko_vol_l\":%.4f,\"ko_vol_r\":%.4f,"
             "\"sample_rom_reads\":%u,\"samples_size\":%u,\"wave_nonzero\":%d,"
             "\"dsc_count\":%u,\"dsc_b0\":%u,\"dsc_b1\":%u,"
             "\"loadsong\":%u,\"sc07\":%u,\"sc07_post\":%u,\"sc05\":%u,"
             "\"dsc7\":%u,\"sc07_d0\":%u,"
             "\"sc07_stk\":[\"%06X\",\"%06X\",\"%06X\",\"%06X\",\"%06X\",\"%06X\"],"
             "\"dsc_song\":%u,\"dsc_fade\":%u,\"dsc_zero\":%u,\"dsc_song_b0\":%u,\"dsc_song_b1\":%u,"
             "\"dsc_zero_b1\":%u,\"ring_w\":%u,\"ring_c\":%u,\"ring_r\":%u,"
             "\"enq\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u],\"c3\":%u,\"c4\":%u,"
             "\"d1hist\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u],"
             "\"sp\":\"%08X\",\"ssp\":\"%08X\",\"cmd_leak\":%d,\"chain_leak\":%d,"
             "\"cmd1_stk\":[\"%08X\",\"%08X\",\"%08X\",\"%08X\",\"%08X\",\"%08X\"],"
             "\"crash_pc\":\"%06X\",\"crash_target\":\"%06X\",\"crash_sp\":\"%08X\","
             "\"crash_stk\":[\"%08X\",\"%08X\",\"%08X\",\"%08X\",\"%08X\",\"%08X\",\"%08X\",\"%08X\"]}",
             (unsigned long long)g_sound.write_count,
             (unsigned long long)g_sound.read_count,
             g_sound.m68k.cpu.pc,
             g_sound_step_total,
             g_sound.rom_loaded ? "true" : "false",
             g_scsp.keyon_count, g_scsp.keyoff_count,
             g_sound_noteon, g_sound_noteoff, (unsigned)g_sound.m68k.cpu.sr,
             g_scsp.cb_count,
             active,
             g_scsp.loaded ? "true" : "false",
             saudio_isvalid() ? "true" : "false",
             saudio_isvalid() ? saudio_sample_rate() : 0,
             g_scsp.sample_rom ? "true" : "false",
             seq_active, tb, ta, master,
             g_irqt.intreq, g_irqt.intena,
             (unsigned long long)g_irqt.deliver_count, inten,
             sqc, sqs,
             g_sound.comm[0x41A], g_sound.comm[0x41B],
             g_scsp.dbg_oct, g_scsp.dbg_fns, g_scsp.dbg_tl,
             g_scsp.dbg_disdl, g_scsp.dbg_dipan,
             g_scsp.dbg_step, g_scsp.dbg_vol_l, g_scsp.dbg_vol_r,
             g_sound.sample_rom_reads, g_sound.samples_size, wave_nz,
             g_sound.dbg_dsc_count, g_sound.dbg_dsc_b0, g_sound.dbg_dsc_b1,
             g_sound.dbg_loadsong, g_sound.dbg_sc07, g_sound.dbg_sc07_post, g_sound.dbg_sc05,
             g_sound.dbg_dsc7, g_sound.dbg_sc07_d0,
             g_sound.dbg_sc07_stk[0], g_sound.dbg_sc07_stk[1], g_sound.dbg_sc07_stk[2],
             g_sound.dbg_sc07_stk[3], g_sound.dbg_sc07_stk[4], g_sound.dbg_sc07_stk[5],
             g_sound.dbg_dsc_song, g_sound.dbg_dsc_fade, g_sound.dbg_dsc_zero,
             g_sound.dbg_dsc_song_b0, g_sound.dbg_dsc_song_b1,
             g_sound.dbg_dsc_zero_b1, ring_w, ring_c, ring_r,
             g_sound.dbg_enq[0], g_sound.dbg_enq[1], g_sound.dbg_enq[2], g_sound.dbg_enq[3],
             g_sound.dbg_enq[4], g_sound.dbg_enq[5], g_sound.dbg_enq[6], g_sound.dbg_enq[7],
             g_sound.dbg_enq[8], g_sound.dbg_enq[9], g_sound.dbg_enq[10],
             g_sound.dbg_c3, g_sound.dbg_c4,
             g_sound.dbg_d1hist[0],g_sound.dbg_d1hist[1],g_sound.dbg_d1hist[2],g_sound.dbg_d1hist[3],
             g_sound.dbg_d1hist[4],g_sound.dbg_d1hist[5],g_sound.dbg_d1hist[6],g_sound.dbg_d1hist[7],
             g_sound.dbg_d1hist[8],g_sound.dbg_d1hist[9],g_sound.dbg_d1hist[10],g_sound.dbg_d1hist[11],
             g_sound.dbg_d1hist[12],g_sound.dbg_d1hist[13],g_sound.dbg_d1hist[14],g_sound.dbg_d1hist[15],
             g_sound.m68k.cpu.a[7], g_sound.m68k.cpu.ssp, g_sound.dbg_cmd_leak, g_sound.dbg_chain_leak,
             g_sound.dbg_cmd1_stk[0], g_sound.dbg_cmd1_stk[1], g_sound.dbg_cmd1_stk[2],
             g_sound.dbg_cmd1_stk[3], g_sound.dbg_cmd1_stk[4], g_sound.dbg_cmd1_stk[5],
             g_sound.dbg_crash_pc, g_sound.dbg_crash_target, g_sound.dbg_crash_sp,
             g_sound.dbg_crash_stk[0], g_sound.dbg_crash_stk[1], g_sound.dbg_crash_stk[2], g_sound.dbg_crash_stk[3],
             g_sound.dbg_crash_stk[4], g_sound.dbg_crash_stk[5], g_sound.dbg_crash_stk[6], g_sound.dbg_crash_stk[7]);
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

static void mcp_cmd_dump_slot_regs(char *resp, int cap) {
    char *p = resp; int left = cap, n;
#define SAPPEND(...) do { n = snprintf(p, (size_t)left, __VA_ARGS__); p += n; left -= n; } while(0)
    SAPPEND("{\"ok\":true,\"slot\":%u,\"sa\":\"0x%05X\",\"regs\":[",
            g_sound.dbg_slot_idx, g_sound.dbg_sa);
    for (int i = 0; i < 32; i++)
        SAPPEND("%s%u", i ? "," : "", g_sound.dbg_slot_regs[i]);
    SAPPEND("],\"wave_at_sa\":[");
    for (int i = 0; i < 16; i++)
        SAPPEND("%s%u", i ? "," : "", g_sound.dbg_wave_at_sa[i]);
    SAPPEND("]}");
#undef SAPPEND
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
    else if (strcmp(cmd, "sound_status")             == 0) mcp_cmd_sound_status(resp, cap);
    else if (strcmp(cmd, "dump_midi_log")            == 0) mcp_cmd_dump_midi_log(resp, cap);
    else if (strcmp(cmd, "read_wave")                == 0) {
        uint32_t addr=0,len=0; mcp_json_get_u32(req,"addr",&addr); mcp_json_get_u32(req,"len",&len);
        if (len>256) len=256;
        char *p=resp; int left=cap; int n;
        n=snprintf(p,(size_t)left,"{\"ok\":true,\"addr\":\"%06X\",\"b\":[",addr); p+=n; left-=n;
        for (uint32_t i=0;i<len && (addr+i)<M68K_WAVE_SIZE && left>6;i++){
            n=snprintf(p,(size_t)left,"%s%u",i?",":"",g_sound.wave[addr+i]); p+=n; left-=n;
        }
        snprintf(p,(size_t)left,"]}");
    }
    else if (strcmp(cmd, "dump_scsprd")              == 0) {
        char *p=resp; int left=cap; int n;
        n=snprintf(p,(size_t)left,"{\"ok\":true,\"r\":["); p+=n; left-=n;
        for (int i=0;i<g_sound.dbg_scsprd_n && left>40;i++){
            n=snprintf(p,(size_t)left,"%s{\"off\":\"%03X\",\"val\":%u,\"pc\":\"%06X\",\"cnt\":%u}",
                i?",":"", g_sound.dbg_scsprd_off[i], g_sound.dbg_scsprd_val[i],
                g_sound.dbg_scsprd_pc[i], g_sound.dbg_scsprd_cnt[i]); p+=n; left-=n;
        }
        snprintf(p,(size_t)left,"]}");
    }
    else if (strcmp(cmd, "read_comm")                == 0) {
        uint32_t addr=0,len=0; mcp_json_get_u32(req,"addr",&addr); mcp_json_get_u32(req,"len",&len);
        if (len>256) len=256;
        char *p=resp; int left=cap; int n;
        n=snprintf(p,(size_t)left,"{\"ok\":true,\"addr\":\"%03X\",\"b\":[",addr); p+=n; left-=n;
        for (uint32_t i=0;i<len && (addr+i)<M68K_SCSP_SIZE && left>6;i++){
            n=snprintf(p,(size_t)left,"%s%u",i?",":"",g_sound.comm[addr+i]); p+=n; left-=n;
        }
        snprintf(p,(size_t)left,"]}");
    }
    else if (strcmp(cmd, "dump_ctrl")                == 0) {
        char *p = resp; int left = cap; int n;
        n = snprintf(p, (size_t)left, "{\"ok\":true,\"w\":["); p += n; left -= n;
        for (int i = 0; i < 64 && left > 30; i++) {
            int idx = (g_sound.dbg_ctrl_pos + i) & 63;
            n = snprintf(p, (size_t)left, "%s[\"%06X\",%u,%u]", i?",":"",
                         g_sound.dbg_ctrl_pc[idx], (g_sound.dbg_ctrl_si[idx]>>8)&0xFF,
                         g_sound.dbg_ctrl_si[idx]&0xFF); p += n; left -= n;
        }
        snprintf(p, (size_t)left, "]}");
    }
    else if (strcmp(cmd, "dump_voices")              == 0) {
        char *p = resp; int left = cap; int n;
        n = snprintf(p, (size_t)left, "{\"ok\":true,\"v\":["); p += n; left -= n;
        int first = 1;
        for (int i = 0; i < SCSP_VOICES && left > 80; i++) {
            if (!g_scsp.voices[i].active) continue;
            uint32_t o = (uint32_t)i * 0x20u;
            unsigned w8  = ((unsigned)g_sound.comm[o+0x08]<<8)|g_sound.comm[o+0x09];
            unsigned wA  = ((unsigned)g_sound.comm[o+0x0A]<<8)|g_sound.comm[o+0x0B];
            unsigned d2r = (w8>>11)&0x1F, d1r=(w8>>6)&0x1F, ar=w8&0x1F;
            unsigned rr  = wA&0x1F, dl=(wA>>5)&0x1F;
            unsigned kyonb = (g_sound.comm[o+0x00]>>3)&1;
            n = snprintf(p, (size_t)left,
                "%s{\"s\":%d,\"age\":%u,\"loop\":%d,\"rel\":%d,\"tl\":%u,\"kyonb\":%u,"
                "\"ar\":%u,\"d1r\":%u,\"d2r\":%u,\"dl\":%u,\"rr\":%u}",
                first?"":",", i, g_scsp.voices[i].age, g_scsp.voices[i].loop?1:0,
                g_scsp.voices[i].releasing?1:0, g_sound.comm[o+0x0D], kyonb,
                ar, d1r, d2r, dl, rr);
            p += n; left -= n; first = 0;
        }
        snprintf(p, (size_t)left, "]}");
    }
    else if (strcmp(cmd, "dump_pcr")                 == 0) {
        char *p = resp; int left = cap; int n;
        n = snprintf(p, (size_t)left, "{\"ok\":true,\"snapped\":%d,\"trace\":[", g_sound.dbg_pcr_snapped); p += n; left -= n;
        for (int i = 0; i < 128 && left > 28; i++) {
            int idx = (g_sound.dbg_pcr_pos + i) % 128;  /* chronological from oldest */
            n = snprintf(p, (size_t)left, "%s[\"%06X\",\"%06X\"]", i?",":"",
                         g_sound.dbg_pcr_snap[idx], g_sound.dbg_pcr_sp[idx]); p += n; left -= n;
        }
        snprintf(p, (size_t)left, "]}");
    }
    else if (strcmp(cmd, "dump_slot_regs")           == 0) mcp_cmd_dump_slot_regs(resp, cap);
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
