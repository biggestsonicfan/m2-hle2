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
#include "json_min.h"
#include "emu_thread.h"
#include "breakpoint.h"
#include "log.h"
#include "game_profile.h"
#include "rom_loader.h"   /* romset_t — the regions the model decoder reads */
#include "input.h"     /* g_input.held — drive the game's I/O ports over the bridge */
#include "objview_cmd.h"  /* the object viewer's commands, shared with the web build */
#include "av_stream.h"    /* the --av-port server, for the "av" block of get_status */
#include "overlay_host.h" /* ...and the "overlay" block: is the plugin actually running */
#include "frame_times.h"  /* ...and "render": where the host's frame time goes */
/* Before this header's own winsock block, and before anything else that could
 * reach <windows.h>: net_socket.h owns the include order and <winsock2.h> has
 * to precede it. main.c already includes this first, so here it is a no-op --
 * it is stated so the dependency is visible from the file that has it. */
#include "net/netplay.h"   /* the lobby, over the bridge — see the netplay section */

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

/* ---- Tiny JSON helpers ---------------------------------------------------
 *
 * The readers themselves live in core/json_min.h, shared with the browser
 * build's entry points so a command spelled one way works both ways. These are
 * the names the ~140 call sites below already use.
 */

#define mcp_json_u32hex   json_u32hex
#define mcp_json_get_str  json_get_str
#define mcp_json_get_u32  json_get_u32
#define mcp_json_get_f32  json_get_f32
#define mcp_json_get_int  json_get_int
#define mcp_json_escape   json_escape

/* ---- Command handlers ---------------------------------------------------- */

static void mcp_cmd_get_status(char *resp, int cap) {
    mcp_bridge_t *b = &g_mcp;
    int running = b->emu && emu_is_running(b->emu);
    int halted  = b->cpu && b->cpu->halted;
    uint32_t ip = 0;
    uint32_t sps = 0;
    uint64_t steps = 0;
    const char *profile_id = "none";

    if (b->emu && b->emu->thread_alive) {
        /* Read the double-buffered snapshot WITHOUT the mutex (like the UI does).
         * board_vblank profiles run slices back-to-back and never release the
         * mutex long enough, so locking here starves get_status. A slightly stale
         * ip/sps is fine for a status query. */
        ip  = b->emu->cpu_snapshot.sfr.ip;
        sps = b->emu->steps_per_second;
        /* Instructions since boot. A benchmark that times a game state divides
         * by it (tools/bench-state.mjs): a state is never entered from exactly
         * the same frame twice, so the window is never quite the same work, and
         * instructions a second is the comparable number where milliseconds are
         * not. */
        steps = b->emu->total_steps;
    }
    if (g_active_profile) profile_id = g_active_profile->id;

    char av[320];
    av_stream_status_json(av, (int)sizeof av);
    /* Whether an overlay plugin is loaded, and what it costs. A stream front
     * end cannot switch one on after the fact -- it is a command-line flag,
     * like --mcp -- so being able to SEE that it is missing is the difference
     * between a puzzled look at a blank column and a one-line message. */
    char ov[2048];
    overlay_host_status_json(ov, (int)sizeof ov);
    /* Where the host's rendering time has gone, cumulative: two readings some
     * seconds apart give the cost of each stage per rendered frame, which is
     * how a change to the renderer is measured (tools/bench-render.mjs). */
    const game_frame_times_t *ft = &g_game_frame_times;
    char rt[256];
    snprintf(rt, sizeof rt,
             "{\"frames\":%llu,\"compose_us\":%lld,\"scan_us\":%lld,\"upload_us\":%lld,"
             "\"draw3d_us\":%lld,\"tiles_us\":%lld}",
             (unsigned long long)ft->frames, (long long)ft->compose_us, (long long)ft->scan_us,
             (long long)ft->upload_us, (long long)ft->draw3d_us, (long long)ft->tiles_us);

    snprintf(resp, (size_t)cap,
             "{\"ok\":true,\"running\":%s,\"halted\":%s,"
             "\"ip\":\"0x%08X\",\"steps_per_second\":%u,\"steps\":%llu,\"profile\":\"%s\","
             "\"frames\":%u,\"rom_loaded\":%s,\"match_replay\":\"%s\",\"match_replay_frame\":%u,"
             "\"av\":%s,\"overlay\":%s,\"render\":%s}",
             running ? "true" : "false",
             halted  ? "true" : "false",
             ip, sps, (unsigned long long)steps, profile_id,
             g_emu_frames,
             (g_mcp.romset && g_mcp.romset->loaded) ? "true" : "false",
             g_match_replay == 1 ? "armed" : g_match_replay == 2 ? "done" : g_match_replay < 0 ? "unsupported" : "off",
             g_match_replay_frame, av, ov, rt);
}

/* {"cmd":"prof","on":1} arms the i960 address profiler (clearing it),
 * {"cmd":"prof","on":0} disarms, {"cmd":"prof_dump","path":"..."} writes
 * addr,count and its .frames.csv companion. Only an M2HLE_PROFILE build
 * counts anything; the rest answer ok:false so a driver can say why. */
static void mcp_cmd_prof(const char *req, char *resp, int cap) {
#ifdef M2HLE_PROFILE
    uint32_t on = 1;
    mcp_json_get_u32(req, "on", &on);
    pcprof_arm((int)on);
    snprintf(resp, (size_t)cap, "{\"ok\":true,\"on\":%s}", on ? "true" : "false");
#else
    (void)req;
    snprintf(resp, (size_t)cap,
             "{\"ok\":false,\"error\":\"this build has no profiler (cmake -DM2HLE_PROFILE=ON)\"}");
#endif
}

static void mcp_cmd_prof_dump(const char *req, char *resp, int cap) {
#ifdef M2HLE_PROFILE
    char path[512] = {0};
    if (!mcp_json_get_str(req, "path", path, sizeof path)) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"prof_dump needs a path\"}");
        return;
    }
    int n = pcprof_write(path);
    if (n < 0) { snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"nothing profiled\"}"); return; }
    snprintf(resp, (size_t)cap,
             "{\"ok\":true,\"addresses\":%d,\"steps\":%llu,\"frames\":%u}",
             n, (unsigned long long)g_pcprof.steps, g_pcprof.nframes);
#else
    (void)req;
    snprintf(resp, (size_t)cap,
             "{\"ok\":false,\"error\":\"this build has no profiler (cmake -DM2HLE_PROFILE=ON)\"}");
#endif
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
    /* The board's polygon z-sort (geo3d.h geo3d_sort_z) — 0 leaves every face
     * at the depth the projection gives it, which is what a before/after on a
     * co-planar decal wants. */
    if (mcp_json_get_str(req,"zsort",    v,sizeof v)) g_geo3d_zsort = (atoi(v) != 0);
    if (mcp_json_get_str(req,"zrecede",  v,sizeof v)) g_geo3d_zsort_recede = (float)atof(v);
    /* 0: the profile's standing models (geo3d_model_standing) recede like the rest. */
    if (mcp_json_get_str(req,"zstanding",v,sizeof v)) g_geo3d_zsort_standing = (atoi(v) != 0);
    /* 0: a far-corner face in front of one too deep to recede recedes anyway (geo3d_mesh_keep_depth). */
    if (mcp_json_get_str(req,"zkeep",    v,sizeof v)) g_geo3d_zsort_keep = (atoi(v) != 0);
    /* Faces lying on faces (geo3d_mesh_layers): 0 draws them as before. */
    if (mcp_json_get_str(req,"zlayers",  v,sizeof v)) g_geo3d_layers = (atoi(v) != 0);
    if (mcp_json_get_str(req,"zlayer_steps",v,sizeof v)) g_geo3d_layer_steps = (float)atof(v);
    /* zlayer_model: N, or LO-HI, or -1 for every model */
    if (mcp_json_get_str(req,"zlayer_model",v,sizeof v)) {
        const char *dash = v[0] ? strchr(v + 1, '-') : NULL;
        g_geo3d_layer_only = atoi(v);
        g_geo3d_layer_only_hi = dash ? atoi(dash + 1) : g_geo3d_layer_only;
    }
    if (mcp_json_get_str(req,"zlayer_board",v,sizeof v)) g_geo3d_layer_board = (atoi(v) != 0);
    if (mcp_json_get_str(req,"zlayer_plane",v,sizeof v)) g_geo3d_layer_plane = (atoi(v) != 0);
    /* 0: the texture filter wraps at every tile edge, ignoring the faces' wrap bits. */
    if (mcp_json_get_str(req,"texclamp",v,sizeof v)) g_geo3d_tex_clamp = (atoi(v) != 0);
    char models[GEO3D_LAYER_MODELS_MAX * 8] = "";
    for (int i = 0, o = 0; i < g_geo3d_layer_model_count && o < (int)sizeof models - 8; i++)
        o += snprintf(models + o, sizeof models - (size_t)o, "%s%d", i ? "," : "", g_geo3d_layer_models[i]);
    g_geo3d_layer_model_count = 0;
    snprintf(resp,(size_t)cap,
             "{\"ok\":true,\"cam\":[%.2f,%.2f,%.2f],\"rot\":[%.3f,%.3f],\"fov\":%.1f,"
             "\"lines\":%d,\"tris\":%d,\"test\":%d,\"zlayers\":%d,\"layer_faces\":%llu,\"layer_models\":[%s],\"zadjust\":\"0x%08X\"}",
             g_geo3d_state->cam_x,g_geo3d_state->cam_y,g_geo3d_state->cam_z,
             g_geo3d_state->rot_y,g_geo3d_state->rot_x,g_geo3d_state->fov_deg,
             g_geo3d_lines.count, g_geo3d_tris.count, g_geo3d_state->test_triangle ? 1 : 0,
             g_geo3d_layers, (unsigned long long)g_geo3d_layer_faces, models, g_geo3d_zadjust);
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
    uint8_t buf[4096];
    if (!mcp_json_get_u32(req, "addr", &addr) || !mcp_json_get_u32(req, "size", &size)) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"missing addr or size\"}"); return;
    }
    if (size > sizeof(buf)) size = (uint32_t)sizeof(buf);  /* cap to avoid huge responses */
    if (!g_mcp.bus) { snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"bus not ready\"}"); return; }

    /* UNDER THE EMU MUTEX, and not only for a consistent read: a netplay cold
     * boot re-runs mem_init on the emu thread, which frees and reallocates
     * every region. Touching the bus from this thread while that happens is a
     * use-after-free, and it killed the emulator the moment a challenger was
     * accepted. Copy under the lock and format outside it, so the critical
     * section is no longer than the read itself. */
    int locked = g_mcp.emu && g_mcp.emu->thread_alive;
    if (locked) emu_mutex_lock(&g_mcp.emu->mutex);
    for (uint32_t i = 0; i < size; i++) buf[i] = mem_read8(g_mcp.bus, addr + i);
    if (locked) emu_mutex_unlock(&g_mcp.emu->mutex);

    char *p = resp;
    int left = cap;
    int n;
    n = snprintf(p, (size_t)left, "{\"ok\":true,\"addr\":\"0x%08X\",\"data\":\"", addr);
    p += n; left -= n;
    for (uint32_t i = 0; i < size && left > 4; i++) {
        n = snprintf(p, (size_t)left, "%02X", buf[i]);
        p += n; left -= n;
    }
    snprintf(p, (size_t)left, "\"}");
}

/* Several ranges in one round trip, copied under ONE hold of the emu mutex.
 *
 *   {"cmd":"read_many","ranges":[["0x00500700",8],[5883784,1]]}
 *   -> {"ok":true,"frame":N,"data":["0102...","FF"]}
 *
 * The emu thread holds the mutex for a whole slice and a slice almost always
 * ends at the frame hook, so every range comes from the same moment between
 * two slices and the board never stops. That is the point: a caller that paused
 * the board around its reads (emu_stop, reads, emu_run) stalled BOTH machines
 * of a netplay session on every pause -- the fly's rounds ran at ~28 fps
 * against 60 at character select on the same link. `frame` is g_emu_frames
 * as the copy was taken.
 *
 * `ranges` is read as a flat run of numbers, addr then size, whatever the
 * nesting; decimal or 0x hex, quoted or bare. */
#define MCP_READ_MANY_RANGES 64
#define MCP_READ_MANY_BYTES  32768   /* hex doubles it, inside the 128 kB reply */
static void mcp_cmd_read_many(const char *req, char *resp, int cap) {
    static uint8_t buf[MCP_READ_MANY_BYTES];   /* one client at a time */
    uint32_t addr[MCP_READ_MANY_RANGES], size[MCP_READ_MANY_RANGES];
    int n = 0, half = 0, depth = 0;
    uint32_t total = 0;

    const char *p = json_value_at(req, "ranges");
    if (!p || *p != '[') {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"missing ranges\"}"); return;
    }
    for (; *p; p++) {
        if (*p == '[') { depth++; continue; }
        if (*p == ']') { if (--depth == 0) break; continue; }
        if (*p >= '0' && *p <= '9') {
            char *end;
            uint32_t v = (uint32_t)strtoul(p, &end, 0);
            p = end - 1;
            if (!half) {
                if (n == MCP_READ_MANY_RANGES) {
                    snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"more than %d ranges\"}",
                             MCP_READ_MANY_RANGES); return;
                }
                addr[n] = v;
            } else {
                size[n] = v;
                if (v > MCP_READ_MANY_BYTES - total) {
                    snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"more than %d bytes in all\"}",
                             MCP_READ_MANY_BYTES); return;
                }
                total += v;
                n++;
            }
            half ^= 1;
        }
    }
    if (half || n == 0) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"ranges must be [addr,size] pairs\"}"); return;
    }
    if (!g_mcp.bus) { snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"bus not ready\"}"); return; }

    /* Under the mutex for the same two reasons as mcp_cmd_read_memory, and held
     * across every range so they all come from one point between slices. */
    int locked = g_mcp.emu && g_mcp.emu->thread_alive;
    if (locked) emu_mutex_lock(&g_mcp.emu->mutex);
    unsigned frame = g_emu_frames;
    uint8_t *b = buf;
    for (int r = 0; r < n; r++)
        for (uint32_t i = 0; i < size[r]; i++) *b++ = mem_read8(g_mcp.bus, addr[r] + i);
    if (locked) emu_mutex_unlock(&g_mcp.emu->mutex);

    /* 32 kB of hex is 64 kB, well inside the bridge's 128 kB reply. */
    static const char hex[] = "0123456789ABCDEF";
    char *o = resp;
    o += snprintf(o, (size_t)cap, "{\"ok\":true,\"frame\":%u,\"data\":[", frame);
    b = buf;
    for (int r = 0; r < n; r++) {
        if (r) *o++ = ',';
        *o++ = '"';
        for (uint32_t i = 0; i < size[r]; i++, b++) {
            *o++ = hex[*b >> 4];
            *o++ = hex[*b & 15];
        }
        *o++ = '"';
    }
    *o++ = ']'; *o++ = '}'; *o = '\0';
}

static void mcp_cmd_write_memory(const char *req, char *resp, int cap) {
    uint32_t addr = 0;
    char hexdata[8192 + 1] = {0};
    if (!mcp_json_get_u32(req, "addr", &addr) ||
        !mcp_json_get_str(req, "data", hexdata, sizeof(hexdata))) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"missing addr or data\"}"); return;
    }
    if (!g_mcp.bus) { snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"bus not ready\"}"); return; }

    /* Same mutex as the read, for the same reason -- see mcp_cmd_read_memory.
     * A write also has to land as one piece: the i960 must not run between the
     * first byte and the last. */
    int count = 0;
    int locked = g_mcp.emu && g_mcp.emu->thread_alive;
    if (locked) emu_mutex_lock(&g_mcp.emu->mutex);
    for (int i = 0; hexdata[i*2] && hexdata[i*2+1]; i++) {
        char byte_str[3] = { hexdata[i*2], hexdata[i*2+1], 0 };
        uint8_t b = (uint8_t)strtoul(byte_str, NULL, 16);
        mem_write8(g_mcp.bus, addr + (uint32_t)i, b);
        count++;
    }
    if (locked) emu_mutex_unlock(&g_mcp.emu->mutex);
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
             "\"dsp_steps\":%d,\"out_fill\":%u,\"out_dropped\":%llu,\"midi_drops\":%u,\"midi_hi\":%u,\"midi_drains\":%llu,\"midi_holds\":%llu}",
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
             sc->mi_drops, sc->mi_hi, (unsigned long long)g_sound.midi_drains,
             (unsigned long long)g_sound.midi_holds);
}

/* quit: ask the process to come down.
 *
 * Not exit(): it raises a flag the run loop reads, so the board, the A/V
 * server and the netplay session are taken down in the order any other exit
 * uses -- the same reason the tray's Exit does not call exit() either (see
 * ui/kiosk.h). Answering before that happens is deliberate: the caller gets
 * its reply, and the socket closes because the process went away.
 *
 * A headless run needs this. Its only other way out is the tray icon, and
 * --no-tray takes that away -- which is exactly the case a script or a service
 * is in, so without this there would be no way to stop one at all short of
 * killing it out from under the A/V writer thread. */
static volatile int g_mcp_quit;
static inline bool mcp_quit_requested(void) { return g_mcp_quit != 0; }

static void mcp_cmd_quit(char *resp, int cap) {
    LOG_INFO("quit: asked over the bridge");
    g_mcp_quit = 1;
    snprintf(resp, (size_t)cap, "{\"ok\":true,\"quitting\":true}");
}

/* overlay_swap: put a new build of the overlay plugin on a live stream without
 * taking it down (ui/overlay_host.h, "SWAPPING TO A NEW BUILD").
 *
 *   {"cmd":"overlay_swap","path":"C:\\fly\\build-271\\flyoverlay.dll"}
 *
 * optional: "args" (replaces --overlay-args), "title" / "note" (the toast),
 * "card" (the standby card; \n between lines), "announce_s", "hold_s".
 *
 * `path` must be a NEW file: Windows locks a loaded DLL, so a build cannot be
 * written over the running one. Returns at once; get_status's overlay.swap
 * goes queued -> announce -> standby -> hold -> idle, and its "last" is ok,
 * rolled_back (the new one would not load; the old one is back) or failed. */
static void mcp_cmd_overlay_swap(const char *req, char *resp, int cap) {
    static overlay_swap_req_t r;       /* 2 KB: off the bridge thread's stack */
    memset(&r, 0, sizeof r);
    json_get_str_unescaped(req, "path",  r.path,  (int)sizeof r.path);
    r.has_args = json_get_str_unescaped(req, "args", r.args, (int)sizeof r.args) != 0;
    json_get_str_unescaped(req, "title", r.title, (int)sizeof r.title);
    json_get_str_unescaped(req, "note",  r.note,  (int)sizeof r.note);
    json_get_str_unescaped(req, "card",  r.card,  (int)sizeof r.card);
    float f;
    if (mcp_json_get_f32(req, "announce_s", &f)) r.announce_s = f;
    if (mcp_json_get_f32(req, "hold_s", &f))     r.hold_s     = f;

    char err[128];
    if (!overlay_host_request_swap(&r, err, (int)sizeof err)) {
        char err_js[256];
        mcp_json_escape(err_js, (int)sizeof err_js, err);
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"%s\"}", err_js);
        return;
    }
    snprintf(resp, (size_t)cap, "{\"ok\":true,\"queued\":true}");
}

/* snd_watch: arm or read the streaming watchdog (board/sound.h).
 *
 *   {"cmd":"snd_watch","on":1}   arm it, clearing the counters
 *   {"cmd":"snd_watch"}          read it
 *   {"cmd":"snd_watch","on":0}   disarm
 *
 * `late_up` of `refills` is the headline, NOT `late`: a refill pass the driver
 * began after the chip had already entered the chunk it was filling, counting
 * only the chunks a sample reload cannot explain (board/sound.h says why -- a
 * reload copies the new sample over a fixed window from offset 0 and is
 * indistinguishable from a late refill of chunk 0). `worst` is the furthest into
 * a chunk, in samples of 4096, the chip had got per slot, and `events` the first
 * few, with the board sample, the slot, and the 68000 PC that wrote the byte --
 * enough to disassemble the copy that lost the race. */
static void mcp_cmd_snd_watch(const char *req, char *resp, int cap) {
    uint32_t on = 2;                       /* 2 = not given: read without changing */
    mcp_json_get_u32(req, "on", &on);
    if (on != 2) snd_watch_arm((int)on);
    const snd_watch_t *w = &g_snd_watch;
    char *p = resp; int left = cap, n;
    n = snprintf(p, (size_t)left,
                 "{\"ok\":true,\"on\":%s,\"board_samples\":%llu,\"writes\":%llu,"
                 "\"refills\":%llu,\"late\":%llu,\"late_chunk0\":%llu,\"late_up\":%llu,"
                 "\"by_slot\":[",
                 w->on ? "true" : "false", (unsigned long long)g_sound.out_total,
                 (unsigned long long)w->total_writes, (unsigned long long)w->total_refills,
                 (unsigned long long)w->total_late, (unsigned long long)w->late_chunk0,
                 (unsigned long long)w->late_chunk_up);
    p += n; left -= n;
    for (int i = 0; i < 32 && left > 60; i++) {
        n = snprintf(p, (size_t)left, "%s[%llu,%llu,%u]", i ? "," : "",
                     (unsigned long long)w->refills[i], (unsigned long long)w->late[i],
                     w->worst[i]);
        p += n; left -= n;
    }
    n = snprintf(p, (size_t)left, "],\"events\":["); p += n; left -= n;
    for (uint32_t i = 0; i < w->nev && left > 160; i++) {
        n = snprintf(p, (size_t)left,
                     "%s{\"sample\":%llu,\"slot\":%u,\"chunk\":%u,\"addr\":\"0x%05X\","
                     "\"off\":\"0x%04X\",\"play\":\"0x%04X\",\"late\":%u,\"pc\":\"0x%06X\"}",
                     i ? "," : "", (unsigned long long)w->ev[i].sample, w->ev[i].slot,
                     w->ev[i].chunk, w->ev[i].addr, w->ev[i].off, w->ev[i].play,
                     w->ev[i].lateness, w->ev[i].pc);
        p += n; left -= n;
    }
    snprintf(p, (size_t)left, "]}");
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

/* sound_codes {since}: the commands the i960 has sent the sound board
 * (board/sound.h code_log), oldest first, from command number `since` on.
 * `next` is what to pass as `since` to read on (a reply that runs out of room
 * stops early and says so there); `lost` is how many asked for had already
 * left the ring. `sent` and `taken` are UART bytes the i960 wrote and bytes
 * the 68000 read out of the SCSP's MIDI buffer: equal once the driver has
 * caught up, and a gap that stays is bytes lost. */
static void mcp_cmd_sound_codes(const char *req, char *resp, int cap) {
    char *p = resp; int left = cap, n;
#define CAPPEND(...) do { n = snprintf(p, (size_t)left, __VA_ARGS__); p += n; left -= n; } while(0)
    uint32_t total = g_sound.code_n;
    uint32_t first = total > SOUND_CODE_LOG ? total - SOUND_CODE_LOG : 0;
    int asked = 0;
    mcp_json_get_int(req, "since", &asked);
    uint32_t since = asked < 0 ? 0 : (uint32_t)asked > total ? total : (uint32_t)asked;
    uint32_t lost = since < first ? first - since : 0;
    if (since < first) since = first;
    uint32_t end = since;
    while (end < total && end - since < 400) end++;     /* ~40 bytes a record */
    const scsp_t *sc = &g_sound.scsp;
    CAPPEND("{\"ok\":true,\"next\":%u,\"total\":%u,\"lost\":%u,\"sent\":%llu,\"taken\":%llu,"
            "\"midi_drops\":%u,\"midi_hi\":%u,\"midi_drains\":%llu,\"midi_holds\":%llu,\"queue_hi\":%u,\"sample\":%llu,\"codes\":[",
            end, total, lost, (unsigned long long)g_sound.write_count, (unsigned long long)sc->mi_taken,
            sc->mi_drops, sc->mi_hi, (unsigned long long)g_sound.midi_drains,
            (unsigned long long)g_sound.midi_holds, g_sound.queue_hi,
            (unsigned long long)g_sound.out_total);
    for (uint32_t i = since; i < end; i++) {
        uint32_t k = i & (SOUND_CODE_LOG - 1);
        CAPPEND("%s[\"0x%08X\",%llu]", i == since ? "" : ",", g_sound.code_log[k].code,
                (unsigned long long)g_sound.code_log[k].sample);
    }
    CAPPEND("]}");
#undef CAPPEND
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

/* Run exactly N game frames from a stopped board, then stop, in one request.
 *
 * emu_run + wait_frames + emu_stop overshoots by however many frames go by
 * while the stop is in flight: a mean of 8-15 unthrottled, and several at
 * 60 Hz on a loaded host (issue #98). Here the emu thread stops itself at
 * the edge of frame N (emu_slice_finish), so frame N+1 never begins, and the
 * reply comes once it has. reached:false is a breakpoint, watchpoint, halt,
 * a stop from elsewhere, or the timeout -- which stops the board, so it is
 * stopped whatever the answer. */
static void mcp_cmd_run_frames(const char *req, char *resp, int cap) {
    uint32_t count = 1, timeout_ms = 30000;
    mcp_json_get_u32(req, "count", &count);
    mcp_json_get_u32(req, "timeout_ms", &timeout_ms);
    if (count < 1) count = 1;
    if (timeout_ms > 300000) timeout_ms = 300000;
    if (!g_mcp.emu || !g_mcp.emu->thread_alive) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"emu not started\"}"); return;
    }
    if (emu_is_running(g_mcp.emu)) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"must be stopped to run_frames\"}"); return;
    }

    unsigned start = g_emu_frames;
    int64_t  t0    = emu_now_us();
    int64_t  limit = (int64_t)timeout_ms * 1000;
    g_bp.hit_addr = 0;   /* so a stop below can say whether a breakpoint made it */
    emu_run_frames(g_mcp.emu, count);
    bool timed_out = false;
    while (emu_is_running(g_mcp.emu)) {
        if (emu_now_us() - t0 >= limit) { timed_out = true; break; }
        emu_nap_us(250);
    }
    if (timed_out) {
        emu_stop(g_mcp.emu);
        while (emu_is_running(g_mcp.emu) && g_mcp.emu->thread_alive) emu_nap_us(250);
    }
    unsigned advanced = g_emu_frames - start;
    int halted = g_mcp.cpu && g_mcp.cpu->halted;
    const char *reason = advanced >= count ? "frames"
                       : timed_out         ? "timeout"
                       : halted            ? "halted"
                       : g_wp.hit          ? "watchpoint"
                       : g_bp.hit_addr     ? "breakpoint"
                       : "stopped";
    snprintf(resp, (size_t)cap,
             "{\"ok\":true,\"frames\":%u,\"advanced\":%u,\"reached\":%s,"
             "\"reason\":\"%s\",\"elapsed_ms\":%u}",
             g_emu_frames, advanced, advanced >= count ? "true" : "false",
             reason, (unsigned)((emu_now_us() - t0) / 1000));
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

/* ---- Netplay -------------------------------------------------------------
 *
 * The netplay window's buttons, as bridge commands. The reason they exist is
 * that a lobby somebody has to sit and watch is not a lobby a program can keep
 * open: stf-fly's broadcast wants to host a public room, notice that a
 * challenger has gone ready, accept, and play the session -- and none of that
 * is reachable from a process outside the emulator otherwise.
 *
 * THREADING. Every one of these is `netplay_post` or `netplay_get_status`, both
 * of which take the netplay mutex and nothing else. That is the same path the
 * UI thread uses and the reason that mutex exists; the bridge thread is one
 * more UI thread as far as netplay is concerned. Nothing here touches the emu
 * mutex, the session, or any socket -- the emu thread owns all three and pumps
 * them once a slice.
 *
 * A note on inputs. There is no netplay command for them and there does not
 * need to be: `set_input` already writes `g_input.held`, which is exactly what
 * `netplay_sample_local` reads and transmits. A session drives the board from
 * the composed mask in `net_held`, so what the bridge presses goes out on the
 * wire and comes back applied to both boards. What the bridge must NOT do
 * during a session is write memory -- see the warning on netplay_start.
 */

/* JSON string escaping, for the few fields here that carry text this process
 * did not write: account names, the server's error strings and the log. A stray
 * quote in one of those turns the whole reply into a parse error at the other
 * end, which is a bad way to find out that a server said something unexpected. */

/*
 * The config a netplay command runs with: whatever is already stored (which is
 * mostly the Twitch login token, the whole point of storing anything) with the
 * request's own fields laid over it. So {"cmd":"netplay_connect"} with no
 * arguments at all means "sign in as whoever signed in last", which is what a
 * scripted lobby wants -- the browser dance happens once, ever, by hand.
 */
static void mcp_netplay_cfg(const char *req, netplay_config_t *cfg) {
    uint32_t v = 0;
    char s[256];

    memset(cfg, 0, sizeof(*cfg));
    if (!netplay_stored_settings(cfg)) {
        cfg->port        = RPCN_DEFAULT_PORT;
        cfg->frame_delay = 2;
    }
    if (!cfg->frame_delay) cfg->frame_delay = 2;

    if (mcp_json_get_str(req, "server", s, sizeof(s)))
        snprintf(cfg->server, sizeof(cfg->server), "%s", s);
    if (mcp_json_get_str(req, "user", s, sizeof(s)))
        snprintf(cfg->npid, sizeof(cfg->npid), "%s", s);
    if (mcp_json_get_str(req, "pass", s, sizeof(s))) {
        snprintf(cfg->password, sizeof(cfg->password), "%s", s);
        /* The token is NOT dropped here any more. It used to be, so that a
         * password login did not go out carrying somebody's Twitch token --
         * but this cfg is copied wholesale over `g_netplay.cfg` and then
         * written to disk on the next successful login, so clearing it here
         * signed the Twitch account out of the machine every time an agent
         * logged in with a password. `netplay_twitch_is_for` now decides
         * which credential a given npid gets, and the token is simply kept. */
    }
    if (mcp_json_get_str(req, "token", s, sizeof(s)))
        snprintf(cfg->token, sizeof(cfg->token), "%s", s);
    if (mcp_json_get_str(req, "fingerprint", s, sizeof(s)))
        snprintf(cfg->fingerprint, sizeof(cfg->fingerprint), "%s", s);
    if (mcp_json_get_str(req, "room_pass", s, sizeof(s)))
        snprintf(cfg->room_password, sizeof(cfg->room_password), "%s", s);
    if (mcp_json_get_u32(req, "port", &v) && v)   cfg->port = (uint16_t)v;
    if (mcp_json_get_u32(req, "delay", &v))       cfg->frame_delay = v;
    if (mcp_json_get_u32(req, "p2p_port", &v))    cfg->local_p2p_port = (uint16_t)v;
    if (mcp_json_get_u32(req, "browse_yamp", &v)) cfg->browse_yamp = v != 0;
    if (mcp_json_get_u32(req, "ps3", &v))         cfg->ps3 = v != 0;
    if (mcp_json_get_str(req, "wire", s, sizeof(s)))
        snprintf(cfg->ps3_wire, sizeof(cfg->ps3_wire), "%s", s);
    if (mcp_json_get_u32(req, "max_players", &v)) cfg->max_players = v;
    /* VS mode for a room this hosts: a decided match goes back to character
     * select with both players in, and the same two play on without a reset.
     * Unset, it follows the board's own setting (--vs-mode). */
    cfg->vs_mode = g_vs_mode != 0;
    if (mcp_json_get_u32(req, "vs", &v))          cfg->vs_mode = v != 0;
    if (mcp_json_get_u32(req, "entry", &v))       cfg->entry = (uint8_t)v;
    if (mcp_json_get_u32(req, "watch", &v))       cfg->watch_only = v != 0;
    /* A room id is 64 bits and mcp_json_get_u32 is not, so it travels as a
     * string. Quoted or not: mcp_json_get_str finds the quoted form, and the
     * unquoted one is read straight out of the request. */
    if (mcp_json_get_str(req, "room_id", s, sizeof(s))) {
        cfg->room_id = strtoull(s, NULL, 0);
    } else {
        const char *q = strstr(req, "\"room_id\":");
        if (q) cfg->room_id = strtoull(q + 10, NULL, 0);
    }
}

static void mcp_netplay_reply(char *resp, int cap, const char *verb) {
    netplay_status_t st;
    netplay_get_status(&st);
    snprintf(resp, (size_t)cap,
             "{\"ok\":true,\"queued\":\"%s\",\"state\":\"%s\"}",
             verb, netplay_state_text(st.state));
}

static void mcp_cmd_netplay_connect(const char *req, char *resp, int cap) {
    netplay_config_t cfg;
    uint32_t twitch = 0;
    mcp_netplay_cfg(req, &cfg);
    mcp_json_get_u32(req, "twitch", &twitch);

    if (!cfg.server[0]) {
        snprintf(resp, (size_t)cap,
                 "{\"ok\":false,\"error\":\"no server: pass a server, or sign "
                 "in once in the netplay window so one is stored\"}");
        return;
    }
    if (!twitch && !cfg.npid[0]) {
        snprintf(resp, (size_t)cap,
                 "{\"ok\":false,\"error\":\"no account: pass user and pass, or "
                 "twitch:1 to run the device flow\"}");
        return;
    }
    /* The device flow signs in and then connects itself, so it REPLACES the
     * connect rather than preceding it (netplay_pump_twitch). Asking for both
     * would log in twice. */
    netplay_post(twitch ? NETPLAY_CMD_TWITCH_START : NETPLAY_CMD_CONNECT, &cfg);
    mcp_netplay_reply(resp, cap, twitch ? "twitch" : "connect");
}

static void mcp_cmd_netplay_host(const char *req, char *resp, int cap) {
    netplay_config_t cfg;
    mcp_netplay_cfg(req, &cfg);
    netplay_post(NETPLAY_CMD_HOST, &cfg);
    mcp_netplay_reply(resp, cap, "host");
}

static void mcp_cmd_netplay_join(const char *req, char *resp, int cap) {
    netplay_config_t cfg;
    mcp_netplay_cfg(req, &cfg);
    if (!cfg.room_id) {
        snprintf(resp, (size_t)cap,
                 "{\"ok\":false,\"error\":\"netplay_join needs a room_id\"}");
        return;
    }
    netplay_post(NETPLAY_CMD_JOIN, &cfg);
    mcp_netplay_reply(resp, cap, "join");
}

static void mcp_cmd_netplay_search(const char *req, char *resp, int cap) {
    netplay_config_t cfg;
    mcp_netplay_cfg(req, &cfg);
    netplay_post(NETPLAY_CMD_SEARCH, &cfg);
    mcp_netplay_reply(resp, cap, "search");
}

/*
 * Ready: this player wants to play. The room's owner starts the first match once
 * every player in the room is ready (net/room.h); after that the room rolls on
 * by itself, and this only matters again after netplay_stop.
 *
 * THE BOARD IS ABOUT TO COLD-BOOT. Every match starts from power-on on every
 * machine in the room -- both fighters and every watcher -- because that is the
 * only state copies with no savestates can be certain to share. Anything the
 * bridge had set up -- a fight in progress, a character written into a fighter
 * record, credits poked into RAM -- is gone the moment a match starts.
 *
 * AND MEMORY WRITES ARE DESYNCS. While a session is playing, write_memory
 * changes one of the two boards and not the other, which is precisely what the
 * frame check exists to catch. Everything a client does during a session has to
 * go through set_input: coins, START and the select screen's cursor included.
 * Reads are free.
 */
static void mcp_cmd_netplay_start(char *resp, int cap) {
    netplay_status_t st;
    netplay_get_status(&st);
    if (st.state < NETPLAY_IN_ROOM || st.state == NETPLAY_FAILED) {
        snprintf(resp, (size_t)cap,
                 "{\"ok\":false,\"error\":\"take a room first (state: %s)\"}",
                 netplay_state_text(st.state));
        return;
    }
    netplay_post(NETPLAY_CMD_START, NULL);
    mcp_netplay_reply(resp, cap, "start");
}

/* Not ready; and out of the match this board is running, if any. */
static void mcp_cmd_netplay_stop(char *resp, int cap) {
    netplay_post(NETPLAY_CMD_STOP, NULL);
    mcp_netplay_reply(resp, cap, "stop");
}

/* {"cmd":"netplay_entry","entry":0|1|2} -- ask for no side, 1P or 2P: the PS3
 * port's "1P Entry" / "2P Entry", which jumps the line for that side. */
static void mcp_cmd_netplay_entry(const char *req, char *resp, int cap) {
    netplay_config_t cfg;
    mcp_netplay_cfg(req, &cfg);
    netplay_post(NETPLAY_CMD_ENTRY, &cfg);
    mcp_netplay_reply(resp, cap, "entry");
}

/* {"cmd":"netplay_watch","watch":1|0} -- sit out (never picked to fight), or not. */
static void mcp_cmd_netplay_watch(const char *req, char *resp, int cap) {
    netplay_config_t cfg;
    mcp_netplay_cfg(req, &cfg);
    netplay_post(NETPLAY_CMD_WATCH, &cfg);
    mcp_netplay_reply(resp, cap, "watch");
}

/* The owner only: start the next match now, ready or not. */
static void mcp_cmd_netplay_force_start(char *resp, int cap) {
    netplay_post(NETPLAY_CMD_FORCE_START, NULL);
    mcp_netplay_reply(resp, cap, "force_start");
}

/* Leave the room, stay signed in. */
static void mcp_cmd_netplay_leave(char *resp, int cap) {
    netplay_post(NETPLAY_CMD_LEAVE_ROOM, NULL);
    mcp_netplay_reply(resp, cap, "leave");
}

static void mcp_cmd_netplay_disconnect(char *resp, int cap) {
    netplay_post(NETPLAY_CMD_DISCONNECT, NULL);
    mcp_netplay_reply(resp, cap, "disconnect");
}

/*
 * Everything the published snapshot holds, which is everything a client needs
 * to drive a lobby without a window.
 *
 * peer.ready is the one worth naming: it says somebody has joined this room AND
 * pressed start, and is sitting at the barrier waiting for this end to do the
 * same. That is a challenge, and answering it is netplay_start.
 *
 * rooms is only filled by netplay_search, and log carries the tail of the
 * emulator's own netplay log with a total count beside it, so a poller can tell
 * "nothing happened" from "I missed some lines".
 */
static void mcp_cmd_netplay_status(const char *req, char *resp, int cap) {
    netplay_status_t st;
    uint32_t want_log = 12, want_rooms = 0;
    char esc[512];
    char *p = resp;
    int left = cap, n;

    mcp_json_get_u32(req, "log", &want_log);
    mcp_json_get_u32(req, "rooms", &want_rooms);
    if (want_log > NETPLAY_LOG_LINES) want_log = NETPLAY_LOG_LINES;
    netplay_get_status(&st);

#define NP_APPEND(...) do { n = snprintf(p, (size_t)left, __VA_ARGS__);      \
                            if (n < 0 || n >= left) n = left > 0 ? left - 1 : 0; \
                            p += n; left -= n; } while (0)

    NP_APPEND("{\"ok\":true,\"state\":\"%s\",\"state_num\":%d,\"stage\":%d,"
              "\"is_host\":%s,\"player\":%d,\"room_id\":\"%llu\","
              "\"room_flags\":\"0x%08X\"",
              netplay_state_text(st.state), (int)st.state, (int)st.stage,
              st.is_host ? "true" : "false", st.local_player,
              (unsigned long long)st.room_id, st.room_flags);

    mcp_json_escape(esc, sizeof(esc), st.com_id);
    NP_APPEND(",\"com_id\":\"%s\"", esc);

    mcp_json_escape(esc, sizeof(esc), st.peer_npid);
    NP_APPEND(",\"peer\":{\"npid\":\"%s\",\"known\":%s,\"heard\":%s,"
              "\"ready\":%s,\"ready_gen\":%u,\"rtt_ms\":%d,\"addr\":\"",
              esc, st.peer_known ? "true" : "false",
              st.peer_heard ? "true" : "false",
              st.peer_ready ? "true" : "false", st.peer_ready_gen, (int)st.peer_rtt_ms);
    mcp_json_escape(esc, sizeof(esc), st.peer_addr);
    NP_APPEND("%s\"}", esc);

    /* The room (net/room.h): its phase, the match, and every member in line
     * order with the side they are on and what they have published. */
    NP_APPEND(",\"room\":{\"known\":%s,\"phase\":\"%s\",\"match\":%u,\"session\":%u,"
              "\"vs_mode\":%s,\"fighters\":[%u,%u],"
              "\"last_result\":%d,\"auto_start_s\":%u,\"max\":%u,\"me\":%u,\"members\":[",
              st.room_known ? "true" : "false",
              st.room.phase == ROOM_PHASE_MATCH ? "match" : "lobby",
              st.room.match, st.room.session ? st.room.session : st.room.match,
              st.room.vs_mode ? "true" : "false",
              st.room.fighter[0], st.room.fighter[1],
              st.room.last_result <= 1 ? (int)st.room.last_result : -1,
              st.auto_start_s, st.max_slot, st.my_member_id);
    for (uint32_t i = 0; i < st.member_count && left > 256; i++) {
        const netplay_member_status_t *m = &st.members[i];
        mcp_json_escape(esc, sizeof(esc), m->npid);
        NP_APPEND("%s{\"id\":%u,\"npid\":\"%s\",\"me\":%s,\"owner\":%s,\"line\":%d,\"side\":%d,"
                  "\"known\":%s,\"ready\":%s,\"watch\":%s,\"entry\":%u,\"playing\":%u,"
                  "\"result_match\":%u,\"games\":%u,\"wins\":%u,\"points\":%u,"
                  "\"addr_known\":%s,\"heard\":%s,\"rtt_ms\":%d}",
                  i ? "," : "", m->member_id, esc, m->is_me ? "true" : "false",
                  m->is_owner ? "true" : "false", m->line_pos, m->side,
                  m->known ? "true" : "false",
                  (m->data.flags & ROOM_MEMBER_READY) ? "true" : "false",
                  (m->data.flags & ROOM_MEMBER_WATCH) ? "true" : "false",
                  m->data.entry, m->data.playing, m->data.result_match,
                  m->data.games, m->data.wins, m->data.points,
                  m->addr_known ? "true" : "false", m->heard ? "true" : "false", (int)m->rtt_ms);
    }
    NP_APPEND("]}");

    NP_APPEND(",\"frame\":%u,\"stalls\":%u,\"generation\":%u,\"seed\":\"0x%08X\"",
              st.frame, st.stalls, st.generation, st.seed);
    /* PS3 cross-play (net/ps3_link.h): the PS3 room's phase, our side, the
     * lockstep's counters, and each member's signaling and RUDP channels
     * (0 idle, 1 SYN sent, 2 SYN received, 3 open, 4 closed). */
    if (st.ps3) {
        NP_APPEND(",\"ps3\":{\"room_known\":%s,\"phase\":%u,\"side\":%d,\"match\":%s,"
                  "\"gen\":%u,\"rgen\":%u,\"gen_ok\":%s,\"resp_done\":%s,\"passed\":%s,"
                  "\"sample\":%d,\"play\":%d,\"newest\":%d,\"delay\":%d,\"stalled\":%u,"
                  "\"seed\":\"0x%08X\",\"me_flags\":\"0x%08X\",\"peers\":[",
                  st.ps3_room_known ? "true" : "false", st.ps3_phase, st.ps3_side,
                  st.ps3_match ? "true" : "false", st.ps3_gen, st.ps3_rgen,
                  st.ps3_gen_ok ? "true" : "false", st.ps3_resp_done ? "true" : "false",
                  st.ps3_passed ? "true" : "false", st.ps3_sample, st.ps3_play, st.ps3_newest,
                  st.ps3_delay, st.ps3_stalled_frames, st.ps3_seed, st.ps3_me_flags);
        for (uint32_t i = 0; i < st.ps3_peer_count && left > 256; i++) {
            mcp_json_escape(esc, sizeof(esc), st.ps3_peers[i].npid);
            NP_APPEND("%s{\"id\":%u,\"npid\":\"%s\",\"sig\":%s,\"sig_peer\":%s,\"rtt_us\":%u,"
                      "\"ch\":[%u,%u,%u],\"addr\":\"%s\"}",
                      i ? "," : "", st.ps3_peers[i].member_id, esc,
                      st.ps3_peers[i].sig_active ? "true" : "false",
                      st.ps3_peers[i].sig_peer_active ? "true" : "false", st.ps3_peers[i].rtt_us,
                      st.ps3_peers[i].ch_state[0], st.ps3_peers[i].ch_state[1],
                      st.ps3_peers[i].ch_state[2], st.ps3_peers[i].addr);
        }
        NP_APPEND("]}");
    }
    /* The room emptied with the board still in its VS mode: any input restarts
     * the game (netplay_empty_room_pump). */
    NP_APPEND(",\"empty_room\":%s", st.empty_room ? "true" : "false");
    /* LOCKSTEP_NO_CHECK means the two boards have never disagreed. Reporting it
     * as a frame number would be a desync at frame 4294967295. */
    if (st.desync_frame != LOCKSTEP_NO_CHECK) NP_APPEND(",\"desync_frame\":%u", st.desync_frame);
    else                                      NP_APPEND(",\"desync_frame\":null");

    mcp_json_escape(esc, sizeof(esc), st.error);
    NP_APPEND(",\"error\":\"%s\"", esc);

    mcp_json_escape(esc, sizeof(esc), st.twitch_npid);
    NP_APPEND(",\"twitch\":{\"state\":%d,\"signed_in\":%s,\"npid\":\"%s\"",
              (int)st.twitch_state, st.twitch_signed_in ? "true" : "false", esc);
    mcp_json_escape(esc, sizeof(esc), st.twitch_user_code);
    NP_APPEND(",\"user_code\":\"%s\"", esc);
    mcp_json_escape(esc, sizeof(esc), st.twitch_uri);
    NP_APPEND(",\"uri\":\"%s\"", esc);
    mcp_json_escape(esc, sizeof(esc), st.twitch_error);
    NP_APPEND(",\"error\":\"%s\"}", esc);

    if (want_rooms) {
        NP_APPEND(",\"search_pending\":%s,\"rooms\":[",
                  st.search_pending ? "true" : "false");
        for (uint32_t i = 0; i < st.room_count && left > 128; i++) {
            mcp_json_escape(esc, sizeof(esc), st.rooms[i].owner);
            NP_APPEND("%s{\"room_id\":\"%llu\",\"owner\":\"%s\",\"members\":%u,"
                      "\"max\":%u,\"password\":%s,\"flags\":\"0x%08X\"}",
                      i ? "," : "", (unsigned long long)st.rooms[i].room_id, esc,
                      st.rooms[i].cur_members, st.rooms[i].max_slots,
                      st.rooms[i].has_password ? "true" : "false",
                      st.rooms[i].flag_attr);
        }
        NP_APPEND("]");
    }

    NP_APPEND(",\"log_count\":%u,\"log\":[", st.log_count);
    if (want_log) {
        uint32_t have  = st.log_count < NETPLAY_LOG_LINES ? st.log_count : NETPLAY_LOG_LINES;
        uint32_t take  = want_log < have ? want_log : have;
        uint32_t first = st.log_count - take;
        for (uint32_t i = 0; i < take && left > 64; i++) {
            mcp_json_escape(esc, sizeof(esc), st.log[(first + i) % NETPLAY_LOG_LINES]);
            NP_APPEND("%s\"%s\"", i ? "," : "", esc);
        }
    }
    NP_APPEND("]}");
#undef NP_APPEND
}

/*
 * {"cmd":"board_reset"} -- the cold boot a netplay session performs at the
 * barrier, with no session: re-install the ROM set, reset both CPUs, the sound
 * board, the interrupt controller, the input latch and the run loop's own
 * per-boot state. The run state is left alone (a stopped board stays stopped, at
 * the reset vector). The emu thread does it, so this waits for it to.
 *
 * It exists so the reset can be measured -- tools/grade-reset.mjs holds the boot
 * that follows against a first boot, byte for byte -- and is refused while a
 * session is at the barrier or playing, where it would reset one board of two.
 */
static void mcp_cmd_board_reset(char *resp, int cap) {
    if (!g_mcp.emu || !g_mcp.romset || !g_mcp.romset->loaded) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"no ROM set loaded\"}");
        return;
    }
    netplay_status_t st;
    netplay_get_status(&st);
    if (st.state == NETPLAY_SYNCING || st.state == NETPLAY_PLAYING || st.state == NETPLAY_WATCHING) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"a netplay session owns the board\"}");
        return;
    }
    uint32_t before = g_mcp.emu->reset_count;
    g_mcp.emu->request_reset = 1;
    for (int i = 0; i < 1000 && g_mcp.emu->reset_count == before; i++) emu_sleep_ms(10);
    if (g_mcp.emu->reset_count == before) {
        g_mcp.emu->request_reset = 0;
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"the board was not reset "
                 "(no reset hook, or a session began first)\"}");
        return;
    }
    snprintf(resp, (size_t)cap, "{\"ok\":true,\"resets\":%u}", (unsigned)g_mcp.emu->reset_count);
}

/* ---- The debug object viewer (objview_cmd.h) -----------------------------
 *
 * The commands themselves are in objview_cmd.h, shared with the browser build.
 * What is here is the waiting, which is all this transport adds: it has a
 * thread of its own, so it can block on the render thread and answer once.
 *
 * Two consequences worth knowing before debugging a timeout: a run with no
 * window (--headless) has no renderer at all and these commands say so rather
 * than hanging, and a minimised window may not be asked for frames by the OS,
 * which looks exactly like a stall.
 */

/* Wait for the render thread to run one service pass, so that what we report is
 * this request's result and not the previous one's. */
static int mcp_ov_settle(uint32_t timeout_ms) {
    unsigned before  = g_objview.serial;
    uint32_t elapsed = 0;
    g_objview.refresh = 1;
    while (g_objview.serial == before && elapsed < timeout_ms) {
        emu_sleep_ms(2);
        elapsed += 2;
    }
    return g_objview.serial != before;
}

static void mcp_cmd_objview_status(char *resp, int cap) {
    objview_cmd_reply(g_mcp.romset, g_mcp.bus, resp, cap, 1, NULL);
}

static void mcp_cmd_objview_set(const char *req, char *resp, int cap) {
    objview_cmd_apply(req);
    uint32_t settle_ms = 1500;
    mcp_json_get_u32(req, "settle_ms", &settle_ms);
    if (settle_ms > 60000) settle_ms = 60000;
    /* Render one pass, so the reply carries this object's triangle count, its
     * bounds and its auto-fit distance rather than the previous object's. */
    if (settle_ms && !mcp_ov_settle(settle_ms)) {
        objview_cmd_reply(g_mcp.romset, g_mcp.bus, resp, cap, 0,
                          "no render pass within settle_ms (no window, or minimised)");
        return;
    }
    /* The pass ran, which is not the same as the object having drawn: a model
     * past the table, an empty table entry and a ROM that is not loaded all
     * settle and then fail. Report what the renderer found, not that it looked. */
    objview_cmd_reply(g_mcp.romset, g_mcp.bus, resp, cap,
                      g_objview.last_ok, g_objview.last_err);
}

static void mcp_cmd_objview_shot(const char *req, char *resp, int cap) {
    char err[192];
    if (!objview_cmd_arm_shot(req, err, sizeof err)) {
        objview_cmd_reply(g_mcp.romset, g_mcp.bus, resp, cap, 0, err);
        return;
    }
    uint32_t timeout_ms = 30000;
    mcp_json_get_u32(req, "timeout_ms", &timeout_ms);
    if (timeout_ms > 300000) timeout_ms = 300000;

    objview_req_t *rq = &g_objview.req;
    uint32_t elapsed = 0;
    while (!rq->done && elapsed < timeout_ms) { emu_sleep_ms(2); elapsed += 2; }
    if (!rq->done) {
        rq->pending = 0;
        objview_cmd_reply(g_mcp.romset, g_mcp.bus, resp, cap, 0,
                          "the render thread did not take the shot in time "
                          "(--headless has no renderer; a minimised window gets no frames)");
        return;
    }
    objview_cmd_shot_reply(g_mcp.romset, g_mcp.bus, resp, cap, elapsed);
}

/* Block until the game has built the 3D state the viewer needs - objview_probe
 * says what "built" means and why counting frames will not do. */
static void mcp_cmd_objview_wait_ready(const char *req, char *resp, int cap) {
    uint32_t timeout_ms = 60000;
    mcp_json_get_u32(req, "timeout_ms", &timeout_ms);
    if (timeout_ms > 600000) timeout_ms = 600000;

    objview_ready_t rdy;
    uint32_t elapsed = 0;
    for (;;) {
        objview_probe(g_mcp.romset, g_mcp.bus, &rdy);
        if (rdy.ready || elapsed >= timeout_ms) break;
        emu_sleep_ms(20);
        elapsed += 20;
    }
    char *p = resp;
    int   left = cap, n;
    n = snprintf(p, (size_t)left, "{\"ok\":%s,\"elapsed_ms\":%u,",
                 rdy.ready ? "true" : "false", elapsed);
    p += n; left -= n;
    n = objview_cmd_state(g_mcp.romset, g_mcp.bus, p, left);
    p += n; left -= n;
    snprintf(p, (size_t)left, "}");
}

static void mcp_cmd_objview_list(const char *req, char *resp, int cap) {
    objview_cmd_list(req, g_mcp.romset, resp, cap);
}

static void mcp_dispatch(const char *req, char *resp, int cap) {
    char cmd[64] = {0};
    if (!mcp_json_get_str(req, "cmd", cmd, sizeof(cmd))) {
        snprintf(resp, (size_t)cap, "{\"ok\":false,\"error\":\"missing cmd\"}");
        return;
    }

    if      (strcmp(cmd, "get_status")       == 0) mcp_cmd_get_status(resp, cap);
    else if (strcmp(cmd, "set_input")        == 0) mcp_cmd_set_input(req, resp, cap);
    else if (strcmp(cmd, "prof")             == 0) mcp_cmd_prof(req, resp, cap);
    else if (strcmp(cmd, "prof_dump")        == 0) mcp_cmd_prof_dump(req, resp, cap);
    else if (strcmp(cmd, "set_camera")       == 0) mcp_cmd_set_camera(req, resp, cap);
    else if (strcmp(cmd, "get_registers")    == 0) mcp_cmd_get_registers(resp, cap);
    else if (strcmp(cmd, "read_memory")      == 0) mcp_cmd_read_memory(req, resp, cap);
    else if (strcmp(cmd, "read_many")        == 0) mcp_cmd_read_many(req, resp, cap);
    else if (strcmp(cmd, "write_memory")     == 0) mcp_cmd_write_memory(req, resp, cap);
    else if (strcmp(cmd, "dump_memory_file") == 0) mcp_cmd_dump_memory_file(req, resp, cap);
    else if (strcmp(cmd, "wait_frames")      == 0) mcp_cmd_wait_frames(req, resp, cap);
    else if (strcmp(cmd, "run_frames")       == 0) mcp_cmd_run_frames(req, resp, cap);
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
    else if (strcmp(cmd, "objview_status")           == 0) mcp_cmd_objview_status(resp, cap);
    else if (strcmp(cmd, "objview_set")              == 0) mcp_cmd_objview_set(req, resp, cap);
    else if (strcmp(cmd, "objview_shot")             == 0) mcp_cmd_objview_shot(req, resp, cap);
    else if (strcmp(cmd, "objview_wait_ready")       == 0) mcp_cmd_objview_wait_ready(req, resp, cap);
    else if (strcmp(cmd, "objview_list")             == 0) mcp_cmd_objview_list(req, resp, cap);
    else if (strcmp(cmd, "dump_bones")               == 0) mcp_cmd_dump_bones(resp, cap);
    else if (strcmp(cmd, "dump_tgp")                 == 0) mcp_cmd_dump_tgp(resp, cap);
    else if (strcmp(cmd, "cop_exec")                 == 0) mcp_cmd_cop_exec(req, resp, cap);
    else if (strcmp(cmd, "sound_status")             == 0) mcp_cmd_sound_status(resp, cap);
    else if (strcmp(cmd, "snd_watch")                == 0) mcp_cmd_snd_watch(req, resp, cap);
    else if (strcmp(cmd, "quit")                     == 0) mcp_cmd_quit(resp, cap);
    else if (strcmp(cmd, "overlay_swap")             == 0) mcp_cmd_overlay_swap(req, resp, cap);
    else if (strcmp(cmd, "reset_sound")              == 0) mcp_cmd_reset_sound(req, resp, cap);
    else if (strcmp(cmd, "dump_midi_log")            == 0) mcp_cmd_dump_midi_log(resp, cap);
    else if (strcmp(cmd, "sound_codes")              == 0) mcp_cmd_sound_codes(req, resp, cap);
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
    else if (strcmp(cmd, "netplay_status")           == 0) mcp_cmd_netplay_status(req, resp, cap);
    else if (strcmp(cmd, "netplay_connect")          == 0) mcp_cmd_netplay_connect(req, resp, cap);
    else if (strcmp(cmd, "netplay_host")             == 0) mcp_cmd_netplay_host(req, resp, cap);
    else if (strcmp(cmd, "netplay_join")             == 0) mcp_cmd_netplay_join(req, resp, cap);
    else if (strcmp(cmd, "netplay_search")           == 0) mcp_cmd_netplay_search(req, resp, cap);
    else if (strcmp(cmd, "netplay_start")            == 0) mcp_cmd_netplay_start(resp, cap);
    else if (strcmp(cmd, "netplay_stop")             == 0) mcp_cmd_netplay_stop(resp, cap);
    else if (strcmp(cmd, "netplay_entry")            == 0) mcp_cmd_netplay_entry(req, resp, cap);
    else if (strcmp(cmd, "netplay_watch")            == 0) mcp_cmd_netplay_watch(req, resp, cap);
    else if (strcmp(cmd, "netplay_force_start")      == 0) mcp_cmd_netplay_force_start(resp, cap);
    else if (strcmp(cmd, "netplay_leave")            == 0) mcp_cmd_netplay_leave(resp, cap);
    else if (strcmp(cmd, "netplay_disconnect")       == 0) mcp_cmd_netplay_disconnect(resp, cap);
    else if (strcmp(cmd, "board_reset")              == 0) mcp_cmd_board_reset(resp, cap);
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

/* There is no shutdown: the bridge thread lives until the process exits, and
 * every frontend lets the exit close the socket. */

#endif /* MCP_BRIDGE_H */
