/*
 * net_socket.h — the one place in the tree that owns the platform socket headers.
 *
 * Every other net/ module includes this FIRST and never reaches for <winsock2.h>
 * itself. On Windows that ordering is load-bearing: <windows.h> pulls in the
 * original <winsock.h> unless WIN32_LEAN_AND_MEAN was defined before it, and a
 * translation unit that gets winsock.h before winsock2.h fails with a few dozen
 * redefinition errors. sokol_app.h and emu_thread.h both define the macro before
 * including windows.h, so the two coexist — but only as long as this header is
 * the single point where winsock2.h enters, and main.c includes net/ before
 * anything else (see the note there).
 *
 * The abstraction is deliberately thin. Everything above it speaks in terms of
 *   - net_sock_t          an opaque socket handle, NET_SOCK_INVALID when unset
 *   - IPv4 in NETWORK byte order (uint32_t) and ports in HOST order (uint16_t)
 * because that is the shape RPCN's own signaling payloads arrive in, and
 * converting twice is how a port ends up byte-swapped in exactly one code path.
 *
 * Winsock needs process-wide startup, refcounted here because the netplay
 * session is opened and closed repeatedly (connect, fail, reconnect, host, join)
 * and a WSACleanup under a live socket breaks the next connect for no visible
 * reason. One counter for the whole program.
 *
 * THE WEB BUILD (Emscripten) has no sockets at all. There, the address and UDP
 * functions below are replaced by ones that speak to the gateway over a
 * WebSocket (web_socket.h), and a net_sock_t is a WebSocket id rather than a
 * file descriptor. The TCP helpers are left compiled and never called: tls.h's
 * web backend does not use them.
 */
#ifndef NET_SOCKET_H
#define NET_SOCKET_H

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/ioctl.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <time.h>
#  include <unistd.h>
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#  include "web_socket.h"
#endif

/* ---- Handle -------------------------------------------------------------- */

#ifdef _WIN32
typedef SOCKET net_sock_t;
#  define NET_SOCK_INVALID INVALID_SOCKET
#else
typedef int net_sock_t;
#  define NET_SOCK_INVALID (-1)
#endif

static inline bool net_sock_valid(net_sock_t s) { return s != NET_SOCK_INVALID; }

/* ---- Startup / shutdown -------------------------------------------------- */

static int g_net_refs = 0;

static inline bool net_startup(void) {
#ifdef _WIN32
    if (g_net_refs++ == 0) {
        WSADATA data;
        memset(&data, 0, sizeof(data));
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) { g_net_refs = 0; return false; }
    }
#else
    g_net_refs++;
#endif
    return true;
}

static inline void net_shutdown_lib(void) {
#ifdef _WIN32
    if (g_net_refs > 0 && --g_net_refs == 0) WSACleanup();
#else
    if (g_net_refs > 0) g_net_refs--;
#endif
}

/* ---- Errors -------------------------------------------------------------- */

static inline int net_errno(void) {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static inline bool net_would_block(int err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

/* ---- Socket lifetime ----------------------------------------------------- */

static inline void net_close(net_sock_t *s) {
    if (!s || !net_sock_valid(*s)) return;
#ifdef _WIN32
    closesocket(*s);
#elif defined(__EMSCRIPTEN__)
    m2ws_close(*s);   /* the only sockets the web build ever opens */
#else
    close(*s);
#endif
    *s = NET_SOCK_INVALID;
}

static inline bool net_set_nonblocking(net_sock_t s) {
#ifdef _WIN32
    u_long nb = 1;
    return ioctlsocket(s, FIONBIO, &nb) == 0;
#else
    int fl = fcntl(s, F_GETFL, 0);
    if (fl < 0) return false;
    return fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0;
#endif
}

/* Bytes waiting on the socket, without reading them. Used by the TLS layer so a
 * per-frame poll never blocks the emu thread on a quiet connection. */
static inline bool net_pending(net_sock_t s, uint32_t *out) {
#ifdef _WIN32
    u_long n = 0;
    if (ioctlsocket(s, FIONREAD, &n) != 0) return false;
#else
    int n = 0;
    if (ioctl(s, FIONREAD, &n) != 0) return false;
#endif
    if (out) *out = (uint32_t)n;
    return true;
}

/* ---- Addresses ----------------------------------------------------------- */

/* "a.b.c.d:port" into `buf`. `ip` is network byte order. */
static inline const char *net_addr_text(char *buf, size_t cap, uint32_t ip, uint16_t port) {
    const uint8_t *o = (const uint8_t *)&ip;
    snprintf(buf, cap, "%u.%u.%u.%u:%u", o[0], o[1], o[2], o[3], port);
    return buf;
}

#ifndef __EMSCRIPTEN__

/* First IPv4 address for `host`, in network byte order. 0 on failure. */
static inline uint32_t net_resolve_ipv4(const char *host) {
    if (!host || !*host) return 0;
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    uint32_t out = 0;
    if (getaddrinfo(host, NULL, &hints, &res) == 0 && res) {
        out = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    }
    if (res) freeaddrinfo(res);
    return out;
}

/*
 * The LAN address this machine would use to reach `dest`. A UDP socket bound to
 * INADDR_ANY reports 0.0.0.0 from getsockname, which is useless to a peer, so ask
 * the routing table instead: "connecting" a throwaway datagram socket picks the
 * source address without putting a single packet on the wire.
 *
 * RPCN's signaling keepalive carries this so the server can hand two peers behind
 * one public IP each other's local address instead of hairpinning through the NAT.
 */
static inline uint32_t net_local_ipv4_towards(uint32_t dest_be) {
    net_sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!net_sock_valid(s)) return 0;

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(53);
    dst.sin_addr.s_addr = dest_be ? dest_be : htonl(0x08080808u);

    uint32_t out = 0;
    if (connect(s, (struct sockaddr *)&dst, sizeof(dst)) == 0) {
        struct sockaddr_in self;
        memset(&self, 0, sizeof(self));
#ifdef _WIN32
        int len = (int)sizeof(self);
#else
        socklen_t len = sizeof(self);
#endif
        if (getsockname(s, (struct sockaddr *)&self, &len) == 0) out = self.sin_addr.s_addr;
    }
    net_close(&s);
    return out;
}

/* ---- UDP ----------------------------------------------------------------- */

/* Non-blocking UDP socket bound to `port` on every interface. */
static inline bool net_udp_open(net_sock_t *out, uint16_t port) {
    net_sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!net_sock_valid(s)) return false;
    net_set_nonblocking(s);

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port        = htons(port);
    if (bind(s, (struct sockaddr *)&local, sizeof(local)) != 0) { net_close(&s); return false; }

    *out = s;
    return true;
}

static inline bool net_udp_send(net_sock_t s, uint32_t ip_be, uint16_t port,
                                const void *data, uint32_t len) {
    if (!net_sock_valid(s) || !ip_be || !port) return false;
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(port);
    dst.sin_addr.s_addr = ip_be;
    int sent = (int)sendto(s, (const char *)data, (int)len, 0,
                           (struct sockaddr *)&dst, sizeof(dst));
    return sent == (int)len;
}

/* Bytes received, or 0 when nothing is pending. Fills the sender so callers can
 * route by source — safer than sniffing content, whose leading bytes collide. */
static inline int net_udp_recv(net_sock_t s, void *buf, uint32_t cap,
                               uint32_t *out_ip_be, uint16_t *out_port) {
    if (!net_sock_valid(s)) return 0;
    struct sockaddr_in from;
    memset(&from, 0, sizeof(from));
#ifdef _WIN32
    int from_len = (int)sizeof(from);
#else
    socklen_t from_len = sizeof(from);
#endif
    int got = (int)recvfrom(s, (char *)buf, (int)cap, 0, (struct sockaddr *)&from, &from_len);
    if (got <= 0) return 0;
    if (out_ip_be) *out_ip_be = from.sin_addr.s_addr;
    if (out_port)  *out_port  = ntohs(from.sin_port);
    return got;
}

#else /* __EMSCRIPTEN__ */

/*
 * The web build's versions (see the top of this file). The netcode's view is
 * unchanged: a UDP socket that sends to and receives from IPv4 addresses. What
 * it cannot know is that every datagram crosses the gateway, which is why two
 * of these answer for the gateway rather than for this machine.
 */

/* The signaling helper is the only thing ever resolved (rpcn_connect), and a
 * browser cannot resolve names: answer with the tag the gateway maps to the
 * real helper, whatever the name. */
static inline uint32_t net_resolve_ipv4(const char *host) {
    (void)host;
    static const uint8_t tag[4] = WEB_SIGNALING_TAG_BYTES;
    uint32_t out;
    memcpy(&out, tag, 4);
    return out;
}

/* This tab has no address of its own that anyone could use. The gateway writes
 * the session's virtual address into each signaling keepalive instead, which is
 * the field this feeds (rpcn_send_signaling_ping). */
static inline uint32_t net_local_ipv4_towards(uint32_t dest_be) {
    (void)dest_be;
    return 0;
}

/* `port` means nothing here: the gateway picks the public port. */
static inline bool net_udp_open(net_sock_t *out, uint16_t port) {
    (void)port;
    char url[300];
    m2ws_url(url, sizeof(url), "dgram");
    int id = m2ws_open(url, 1);
    if (m2ws_state(id) == M2WS_CLOSED) { m2ws_close(id); return false; }
    *out = id;
    return true;
}

#define NET_WEB_DGRAM_MAX 1400

static inline bool net_udp_send(net_sock_t s, uint32_t ip_be, uint16_t port,
                                const void *data, uint32_t len) {
    if (!net_sock_valid(s) || !ip_be || !port || len > NET_WEB_DGRAM_MAX) return false;
    uint8_t frame[6 + NET_WEB_DGRAM_MAX];
    memcpy(frame, &ip_be, 4);                 /* already network order */
    frame[4] = (uint8_t)(port >> 8);
    frame[5] = (uint8_t)port;
    memcpy(frame + 6, data, len);
    return m2ws_send(s, frame, (int)(6 + len)) != 0;
}

static inline int net_udp_recv(net_sock_t s, void *buf, uint32_t cap,
                               uint32_t *out_ip_be, uint16_t *out_port) {
    if (!net_sock_valid(s)) return 0;
    uint8_t frame[6 + NET_WEB_DGRAM_MAX];
    int got = m2ws_recv_msg(s, frame, (int)sizeof(frame));
    if (got < 6) return 0;
    uint32_t n = (uint32_t)got - 6;
    if (n > cap) return 0;
    if (out_ip_be) memcpy(out_ip_be, frame, 4);
    if (out_port)  *out_port = (uint16_t)((frame[4] << 8) | frame[5]);
    memcpy(buf, frame + 6, n);
    return (int)n;
}

#endif /* __EMSCRIPTEN__ */

/* ---- TCP ----------------------------------------------------------------- */

/* Blocking connect with a timeout, so an unreachable server costs a couple of
 * seconds rather than the OS's own (30 s+) retry schedule — this runs on the emu
 * thread and the player is looking at a frozen game while it happens. */
static inline bool net_tcp_connect(net_sock_t *out, const char *host, uint16_t port,
                                   uint32_t timeout_ms) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints;
    struct addrinfo *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host, port_str, &hints, &result) != 0 || !result) return false;

    net_sock_t s = NET_SOCK_INVALID;
    for (struct addrinfo *ai = result; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (!net_sock_valid(s)) continue;

        /* Connect non-blocking, then select() with our own deadline. */
        net_set_nonblocking(s);
        int rc = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
        if (rc != 0) {
            int err = net_errno();
#ifdef _WIN32
            bool in_progress = (err == WSAEWOULDBLOCK);
#else
            bool in_progress = (err == EINPROGRESS);
#endif
            if (!in_progress) { net_close(&s); continue; }

            fd_set wr;
            FD_ZERO(&wr);
            FD_SET(s, &wr);
            struct timeval tv;
            tv.tv_sec  = (long)(timeout_ms / 1000);
            tv.tv_usec = (long)((timeout_ms % 1000) * 1000);
            if (select((int)s + 1, NULL, &wr, NULL, &tv) <= 0) { net_close(&s); continue; }

            /* select() reporting writable is not the same as connected. */
            int so_err = 0;
#ifdef _WIN32
            int so_len = (int)sizeof(so_err);
#else
            socklen_t so_len = sizeof(so_err);
#endif
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&so_err, &so_len) != 0 || so_err != 0) {
                net_close(&s);
                continue;
            }
        }
        break;
    }
    freeaddrinfo(result);

    if (!net_sock_valid(s)) return false;
    *out = s;
    return true;
}

/* Blocking send of exactly `len` bytes over a socket left in non-blocking mode by
 * net_tcp_connect: the handshake writes small flights and must not lose a partial
 * write, so spin on WOULDBLOCK with a select rather than failing. */
static inline bool net_tcp_send_all(net_sock_t s, const void *data, uint32_t len) {
    const char *p = (const char *)data;
    uint32_t left = len;
    while (left) {
        int sent = (int)send(s, p, (int)left, 0);
        if (sent > 0) { p += sent; left -= (uint32_t)sent; continue; }
        if (sent < 0 && net_would_block(net_errno())) {
            fd_set wr;
            FD_ZERO(&wr);
            FD_SET(s, &wr);
            struct timeval tv;
            tv.tv_sec  = 5;
            tv.tv_usec = 0;
            if (select((int)s + 1, NULL, &wr, NULL, &tv) <= 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

/* Blocking receive with a deadline, for the handshake only. Returns bytes, or
 * <= 0 on close/error/timeout. */
static inline int net_tcp_recv_timeout(net_sock_t s, void *buf, uint32_t cap,
                                       uint32_t timeout_ms) {
    for (;;) {
        int got = (int)recv(s, (char *)buf, (int)cap, 0);
        if (got >= 0) return got;
        if (!net_would_block(net_errno())) return -1;

        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        struct timeval tv;
        tv.tv_sec  = (long)(timeout_ms / 1000);
        tv.tv_usec = (long)((timeout_ms % 1000) * 1000);
        if (select((int)s + 1, &rd, NULL, NULL, &tv) <= 0) return -1;
    }
}

/* ---- Monotonic milliseconds ---------------------------------------------- */

static inline uint64_t net_now_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

/* The same clock in microseconds, for timing a round trip. net_now_ms is
 * GetTickCount64 on Windows, which moves in 15.6 ms steps: coarser than the
 * thing being measured. */
static inline uint64_t net_now_us(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)(now.QuadPart / freq.QuadPart) * 1000000ull
         + (uint64_t)(now.QuadPart % freq.QuadPart) * 1000000ull / (uint64_t)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
#endif
}

#endif /* NET_SOCKET_H */
