/*
 * ggpo_compat.h — force-included ahead of every GGPO source file (CMake's
 * ggpo target: `-include` on gcc/clang, `/FI` on MSVC).
 *
 * Upstream GGPO (vendor/ggpo, pinned, never edited) is a Windows library: its
 * Linux platform file does not compile, and its socket and poll layers are
 * Winsock and WaitForMultipleObjects. This header takes the place of both
 * platform_*.h files, by defining their include guards first, and supplies the
 * handful of MSVC CRT names the portable sources use. ggpo_port.cpp replaces
 * udp.cpp, poll.cpp and platform_*.cpp.
 *
 * Nothing here is visible to the emulator: the C side includes ggpo_port.h and
 * ggponet.h only.
 */
#ifndef M2HLE_GGPO_COMPAT_H
#define M2HLE_GGPO_COMPAT_H

#define _GGPO_WINDOWS_H_
#define _GGPO_LINUX_H_
#if defined(_MSC_VER) && !defined(_WINDOWS)
#  define _WINDOWS     /* types.h picks a platform by this or __GNUC__ */
#endif

#include "../net_socket.h"   /* the one place winsock2.h / the BSD headers enter */
#include "ggpo_port.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
typedef int            BOOL;
typedef unsigned long  DWORD;
typedef void          *HANDLE;
typedef void          *HINSTANCE;
typedef void          *LPVOID;
typedef int            SOCKET;
typedef unsigned short u_short;   /* BSD's; strict C++11 (Emscripten) leaves it out */
#  define WINAPI
#  define FALSE 0
#  define TRUE  1
#  define INVALID_SOCKET (-1)
#  define SOCKET_ERROR   (-1)
#  define INFINITE       (-1)
#  define MAX_PATH       260
#endif

#ifndef _MSC_VER
#  include <stddef.h>
static inline int sprintf_s(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return (n < 0) ? 0 : ((size_t)n >= cap ? (int)cap - 1 : n);
}
static inline int vsprintf_s(char *buf, size_t cap, const char *fmt, va_list ap) {
    int n = vsnprintf(buf, cap, fmt, ap);
    return (n < 0) ? 0 : ((size_t)n >= cap ? (int)cap - 1 : n);
}
static inline int strcpy_s(char *dst, size_t cap, const char *src) {
    snprintf(dst, cap, "%s", src);
    return 0;
}
template <size_t N> static inline int strcpy_s(char (&dst)[N], const char *src) {
    return strcpy_s(dst, N, src);
}
/* strncat_s(dst, space left, src, count): GGPO passes what is LEFT, not the
 * buffer's size, so this appends at most space - 1 bytes past the current end. */
static inline int strncat_s(char *dst, size_t space, const char *src, size_t count) {
    size_t len = strlen(dst), n = strnlen(src, count);
    if (space == 0) return 0;
    if (n > space - 1) n = space - 1;
    memmove(dst + len, src, n);
    dst[len + n] = '\0';
    return 0;
}
#endif

/* GGPO's synctest writes synclogs\*.log on every frame it runs, and log.cpp
 * a log-<pid>.log when ggpo.log is set. The port decides what is written. */
#define fopen_s(fpp, name, mode) ggpo_port_fopen((fpp), (name), (mode))
#define CreateDirectoryA(name, sec) ((void)(name), (void)(sec), 1)
#define OutputDebugStringA(text) ggpo_port_debug_text(text)
#define DebugBreak() ((void)0)
#define Sleep(ms) ggpo_port_sleep_ms(ms)
/* Synctest prints a line for every frame it checks; the mismatches still reach
 * OutputDebugStringA above. */
static inline int ggpo_port_quiet(const char *fmt, ...) { (void)fmt; return 0; }
#define printf(...) ggpo_port_quiet(__VA_ARGS__)

class Platform {
public:
    typedef int ProcessID;
    static ProcessID GetProcessID();
    static void AssertFailed(char *msg);
    static unsigned int GetCurrentTimeMS();
    static int GetConfigInt(const char *name);
    static bool GetConfigBool(const char *name);
};

#endif
