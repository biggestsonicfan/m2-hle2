/*
 * ggpo_port.cpp — GGPO's platform, poll and UDP layers, for every host we
 * build. Replaces upstream's platform_*.cpp, poll.cpp and network/udp.cpp
 * (CMakeLists.txt leaves those out), against the same headers.
 *
 * The second C++ translation unit in the tree (the first is ui/mem_edit.cpp),
 * for the same reason: it implements a C++ library's classes. Keep it to that.
 *
 * Poll: GGPO's backends pump it with a timeout of 0 from ggpo_idle and from
 * every input call, and register no handles but the UDP socket's loop sink.
 * So this one never waits: it runs the message, periodic and loop sinks as
 * upstream's did after its wait.
 */
#include "types.h"
#include "poll.h"
#include "network/udp.h"

#ifndef _WIN32
#  include <time.h>
#endif

/* ---- Platform ----------------------------------------------------------- */

Platform::ProcessID Platform::GetProcessID() {
#ifdef _WIN32
    return (ProcessID)GetCurrentProcessId();
#else
    return (ProcessID)getpid();
#endif
}

void Platform::AssertFailed(char *msg) {
    fprintf(stderr, "GGPO: %s\n", msg);
    fflush(stderr);
}

unsigned int Platform::GetCurrentTimeMS() {
    return (unsigned int)net_now_ms();
}

int Platform::GetConfigInt(const char *name) {
    const char *v = getenv(name);
    return v ? atoi(v) : 0;
}

bool Platform::GetConfigBool(const char *name) {
    const char *v = getenv(name);
    if (!v) return false;
    return atoi(v) != 0 || strcmp(v, "true") == 0 || strcmp(v, "TRUE") == 0;
}

/* ---- What the compat header routes here --------------------------------- */

static ggpo_transport_t s_transport;
static bool             s_have_transport;
static uint16_t         s_bound;
static uint32_t         s_sync_errors;
static char             s_last_error[256];
static char             s_synclog_dir[240];

extern "C" void ggpo_port_set_transport(const ggpo_transport_t *t) {
    s_have_transport = t != NULL;
    if (t) s_transport = *t;
}

extern "C" uint16_t ggpo_port_bound(void) { return s_bound; }
extern "C" uint32_t ggpo_port_sync_errors(void) { return s_sync_errors; }
extern "C" const char *ggpo_port_last_error(void) { return s_last_error; }

extern "C" void ggpo_port_reset_errors(void) {
    s_sync_errors = 0;
    s_last_error[0] = '\0';
}

extern "C" void ggpo_port_set_synclog_dir(const char *dir) {
    snprintf(s_synclog_dir, sizeof(s_synclog_dir), "%s", dir ? dir : "");
}

/* Synctest names its files "synclogs\\..."; those go to the synclog directory
 * or nowhere. Only its state-* files, the two snapshots of a mismatch: the
 * log-* ones are a file every frame. Anything else (log.cpp's ggpo.log) opens
 * as named. */
extern "C" int ggpo_port_fopen(FILE **fp, const char *name, const char *mode) {
    static const char prefix[] = "synclogs\\";
    char path[512];
    *fp = NULL;
    if (strncmp(name, prefix, sizeof(prefix) - 1) == 0) {
        if (!s_synclog_dir[0] || strncmp(name + sizeof(prefix) - 1, "state-", 6) != 0) return 1;
        snprintf(path, sizeof(path), "%s/%s", s_synclog_dir, name + sizeof(prefix) - 1);
        name = path;
    }
    *fp = fopen(name, mode);
    return *fp ? 0 : 1;
}

extern "C" void ggpo_port_debug_text(const char *text) {
    s_sync_errors++;
    snprintf(s_last_error, sizeof(s_last_error), "%s", text);
}

extern "C" void ggpo_port_sleep_ms(unsigned ms) {
#ifdef _WIN32
    (Sleep)(ms);
#else
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

/* ---- Poll --------------------------------------------------------------- */

Poll::Poll(void) : _start_time(0), _handle_count(0) {}

void Poll::RegisterHandle(IPollSink *sink, HANDLE h, void *cookie) {
    ASSERT(_handle_count < MAX_POLLABLE_HANDLES - 1);
    _handles[_handle_count] = h;
    _handle_sinks[_handle_count] = PollSinkCb(sink, cookie);
    _handle_count++;
}

void Poll::RegisterMsgLoop(IPollSink *sink, void *cookie) {
    _msg_sinks.push_back(PollSinkCb(sink, cookie));
}

void Poll::RegisterLoop(IPollSink *sink, void *cookie) {
    _loop_sinks.push_back(PollSinkCb(sink, cookie));
}

void Poll::RegisterPeriodic(IPollSink *sink, int interval, void *cookie) {
    _periodic_sinks.push_back(PollPeriodicSinkCb(sink, cookie, interval));
}

void Poll::Run() {
    while (Pump(100)) continue;
}

bool Poll::Pump(int timeout) {
    (void)timeout;
    bool finished = false;
    if (_start_time == 0) _start_time = (int)Platform::GetCurrentTimeMS();
    int elapsed = (int)Platform::GetCurrentTimeMS() - _start_time;

    for (int i = 0; i < _msg_sinks.size(); i++) {
        PollSinkCb &cb = _msg_sinks[i];
        finished = !cb.sink->OnMsgPoll(cb.cookie) || finished;
    }
    for (int i = 0; i < _periodic_sinks.size(); i++) {
        PollPeriodicSinkCb &cb = _periodic_sinks[i];
        if (cb.interval + cb.last_fired <= elapsed) {
            cb.last_fired = (elapsed / cb.interval) * cb.interval;
            finished = !cb.sink->OnPeriodicPoll(cb.cookie, cb.last_fired) || finished;
        }
    }
    for (int i = 0; i < _loop_sinks.size(); i++) {
        PollSinkCb &cb = _loop_sinks[i];
        finished = !cb.sink->OnLoopPoll(cb.cookie) || finished;
    }
    return finished;
}

int Poll::ComputeWaitTime(int elapsed) {
    (void)elapsed;
    return 0;
}

/* ---- Udp ---------------------------------------------------------------- */

static bool own_send(void *ctx, uint32_t ip_be, uint16_t port, const void *data, uint32_t len) {
    return net_udp_send(*(net_sock_t *)ctx, ip_be, port, data, len);
}

static int own_recv(void *ctx, void *buf, uint32_t cap, uint32_t *ip_be, uint16_t *port) {
    return net_udp_recv(*(net_sock_t *)ctx, buf, cap, ip_be, port);
}

Udp::Udp() : _socket(INVALID_SOCKET), _callbacks(NULL), _poll(NULL) {}

Udp::~Udp(void) {
    if (_socket != INVALID_SOCKET) {
        net_sock_t s = (net_sock_t)_socket;
        net_close(&s);
        _socket = INVALID_SOCKET;
        net_shutdown_lib();
    }
    s_bound = 0;
}

void Udp::Init(uint16 port, Poll *poll, Callbacks *callbacks) {
    _callbacks = callbacks;
    _poll = poll;
    _poll->RegisterLoop(this);
    s_bound = 0;
    if (s_have_transport) return;

    net_sock_t s;
    if (!net_startup()) {
        Log("winsock startup failed.\n");
        return;
    }
    if (!net_udp_open(&s, port)) {
        Log("could not bind udp port %d (%d).\n", port, net_errno());
        net_shutdown_lib();
        return;
    }
    _socket = (SOCKET)s;
    s_bound = port;
    Log("udp bound to port %d.\n", port);
}

void Udp::SendTo(char *buffer, int len, int flags, struct sockaddr *dst, int destlen) {
    (void)flags;
    (void)destlen;
    struct sockaddr_in *to = (struct sockaddr_in *)dst;
    uint32_t ip = to->sin_addr.s_addr;
    uint16_t port = ntohs(to->sin_port);
    net_sock_t s = (net_sock_t)_socket;
    bool ok = s_have_transport
            ? s_transport.send(s_transport.ctx, ip, port, buffer, (uint32_t)len)
            : own_send(&s, ip, port, buffer, (uint32_t)len);
    if (!ok) Log("send of %d bytes to port %d failed.\n", len, port);
}

bool Udp::OnLoopPoll(void *cookie) {
    (void)cookie;
    uint8 buf[MAX_UDP_PACKET_SIZE];
    net_sock_t s = (net_sock_t)_socket;
    if (!s_have_transport && !net_sock_valid(s)) return true;
    for (;;) {
        uint32_t ip = 0;
        uint16_t port = 0;
        int len = s_have_transport
                ? s_transport.recv(s_transport.ctx, buf, sizeof(buf), &ip, &port)
                : own_recv(&s, buf, sizeof(buf), &ip, &port);
        if (len <= 0) break;
        sockaddr_in from;
        memset(&from, 0, sizeof(from));
        from.sin_family = AF_INET;
        from.sin_addr.s_addr = ip;
        from.sin_port = htons(port);
        _callbacks->OnMsg(from, (UdpMsg *)buf, len);
    }
    return true;
}

void Udp::Log(const char *fmt, ...) {
    char buf[1024];
    va_list args;
    int off = snprintf(buf, sizeof(buf), "udp | ");
    va_start(args, fmt);
    vsnprintf(buf + off, sizeof(buf) - (size_t)off, fmt, args);
    va_end(args);
    ::Log("%s", buf);
}

/* ---- The session, for C ------------------------------------------------- */

#include "ggponet.h"

static GGPOSession     *s_session;
static ggpo_port_cb_t   s_cb;
static GGPOPlayerHandle s_local = GGPO_INVALID_HANDLE;
static GGPOPlayerHandle s_remote = GGPO_INVALID_HANDLE;

static bool __cdecl cb_begin_game(const char *game) { (void)game; return true; }

static bool __cdecl cb_save(unsigned char **buffer, int *len, int *checksum, int frame) {
    *buffer = (unsigned char *)s_cb.save(s_cb.ud, len, checksum, frame);
    return *buffer != NULL;
}

static bool __cdecl cb_load(unsigned char *buffer, int len) { return s_cb.load(s_cb.ud, buffer, len); }

static bool __cdecl cb_log(char *filename, unsigned char *buffer, int len) {
    if (s_cb.log_state) s_cb.log_state(s_cb.ud, filename, buffer, len);
    return true;
}

static void __cdecl cb_free(void *buffer) { s_cb.free_buf(s_cb.ud, buffer); }

static bool __cdecl cb_advance(int flags) { (void)flags; return s_cb.advance(s_cb.ud); }

static bool __cdecl cb_event(GGPOEvent *e) {
    int player = -1, a = 0, b = 0;
    switch (e->code) {
    case GGPO_EVENTCODE_CONNECTED_TO_PEER:       player = e->u.connected.player; break;
    case GGPO_EVENTCODE_SYNCHRONIZING_WITH_PEER:
        player = e->u.synchronizing.player;
        a = e->u.synchronizing.count;
        b = e->u.synchronizing.total;
        break;
    case GGPO_EVENTCODE_SYNCHRONIZED_WITH_PEER:  player = e->u.synchronized.player; break;
    case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:  player = e->u.disconnected.player; break;
    case GGPO_EVENTCODE_TIMESYNC:                a = e->u.timesync.frames_ahead; break;
    case GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
        player = e->u.connection_interrupted.player;
        a = e->u.connection_interrupted.disconnect_timeout;
        break;
    case GGPO_EVENTCODE_CONNECTION_RESUMED:      player = e->u.connection_resumed.player; break;
    default: break;
    }
    if (s_cb.event) s_cb.event(s_cb.ud, (int)e->code, player, a, b);
    return true;
}

static GGPOSessionCallbacks port_callbacks(const ggpo_port_cb_t *cb) {
    s_cb = *cb;
    GGPOSessionCallbacks c;
    memset(&c, 0, sizeof(c));
    c.begin_game = cb_begin_game;
    c.save_game_state = cb_save;
    c.load_game_state = cb_load;
    c.log_game_state = cb_log;
    c.free_buffer = cb_free;
    c.advance_frame = cb_advance;
    c.on_event = cb_event;
    return c;
}

static int add_player(GGPOPlayerType type, int num, const char *ip, uint16_t port, GGPOPlayerHandle *h) {
    GGPOPlayer p;
    memset(&p, 0, sizeof(p));
    p.size = sizeof(p);
    p.type = type;
    p.player_num = num;
    if (ip) {
        snprintf(p.u.remote.ip_address, sizeof(p.u.remote.ip_address), "%s", ip);
        p.u.remote.port = port;
    }
    return (int)ggpo_add_player(s_session, &p, h);
}

extern "C" int ggpo_port_start_p2p(const ggpo_port_cb_t *cb, int input_size, uint16_t local_port,
                                   int local_player, const char *remote_ip, uint16_t remote_port) {
    ggpo_port_close();
    GGPOSessionCallbacks c = port_callbacks(cb);
    char name[] = "m2hle";
    int err = (int)ggpo_start_session(&s_session, &c, name, 2, input_size, local_port);
    if (err != GGPO_OK) { s_session = NULL; return err; }
    if (!s_bound && !s_have_transport) { ggpo_port_close(); return GGPO_ERRORCODE_GENERAL_FAILURE; }
    int remote_player = local_player == 1 ? 2 : 1;
    err = add_player(GGPO_PLAYERTYPE_LOCAL, local_player, NULL, 0, &s_local);
    if (err == GGPO_OK) err = add_player(GGPO_PLAYERTYPE_REMOTE, remote_player, remote_ip, remote_port, &s_remote);
    if (err != GGPO_OK) ggpo_port_close();
    return err;
}

/* Synctest: both players are this machine's, each added through its own handle. */
extern "C" int ggpo_port_start_synctest(const ggpo_port_cb_t *cb, int input_size, int frames) {
    ggpo_port_close();
    GGPOSessionCallbacks c = port_callbacks(cb);
    char name[] = "m2hle";
    int err = (int)ggpo_start_synctest(&s_session, &c, name, 2, input_size, frames);
    if (err != GGPO_OK) { s_session = NULL; return err; }
    err = add_player(GGPO_PLAYERTYPE_LOCAL, 1, NULL, 0, &s_local);
    if (err == GGPO_OK) err = add_player(GGPO_PLAYERTYPE_LOCAL, 2, NULL, 0, &s_remote);
    if (err != GGPO_OK) ggpo_port_close();
    return err;
}

extern "C" void ggpo_port_close(void) {
    if (s_session) ggpo_close_session(s_session);
    s_session = NULL;
    s_local = s_remote = GGPO_INVALID_HANDLE;
}

extern "C" bool ggpo_port_active(void) { return s_session != NULL; }

extern "C" int ggpo_port_idle(int timeout_ms) {
    return s_session ? (int)ggpo_idle(s_session, timeout_ms) : GGPO_ERRORCODE_INVALID_SESSION;
}

extern "C" int ggpo_port_add_local_input(int which, const void *values, int size) {
    if (!s_session) return GGPO_ERRORCODE_INVALID_SESSION;
    return (int)ggpo_add_local_input(s_session, which ? s_remote : s_local, (void *)values, size);
}

extern "C" int ggpo_port_sync_input(void *values, int size, int *disconnect_flags) {
    if (!s_session) return GGPO_ERRORCODE_INVALID_SESSION;
    return (int)ggpo_synchronize_input(s_session, values, size, disconnect_flags);
}

extern "C" int ggpo_port_advance_frame(void) {
    return s_session ? (int)ggpo_advance_frame(s_session) : GGPO_ERRORCODE_INVALID_SESSION;
}

extern "C" int ggpo_port_set_frame_delay(int frames) {
    if (!s_session) return GGPO_ERRORCODE_INVALID_SESSION;
    return (int)ggpo_set_frame_delay(s_session, s_local, frames);
}

extern "C" int ggpo_port_set_disconnect_timeout(int ms) {
    if (!s_session) return GGPO_ERRORCODE_INVALID_SESSION;
    return (int)ggpo_set_disconnect_timeout(s_session, ms);
}

extern "C" int ggpo_port_stats(int *ping, int *kbps_sent, int *local_behind, int *remote_behind) {
    if (!s_session || s_remote == GGPO_INVALID_HANDLE) return GGPO_ERRORCODE_INVALID_SESSION;
    GGPONetworkStats st;
    memset(&st, 0, sizeof(st));
    int err = (int)ggpo_get_network_stats(s_session, s_remote, &st);
    if (err != GGPO_OK) return err;
    *ping = st.network.ping;
    *kbps_sent = st.network.kbps_sent;
    *local_behind = st.timesync.local_frames_behind;
    *remote_behind = st.timesync.remote_frames_behind;
    return GGPO_OK;
}
