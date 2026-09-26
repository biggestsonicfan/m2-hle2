/*
 * crash_trace.h -- a backtrace in the log when the process dies of a signal.
 *
 * The fly runs emulators for days, headless, with nobody watching, and a
 * process that dies of SIGSEGV used to leave nothing behind but a restart
 * line in the fly's own log. With this, m2hle.log (and stderr) end with the
 * signal, the faulting address and the stack, which addr2line or gdb turn
 * into lines when the binary has symbols (tools/build-debug.sh).
 *
 * glibc hosts only (the Linux desktop and handheld builds); elsewhere
 * crash_trace_install() does nothing. A sanitizer build keeps its own handler,
 * which reports far more, so this one stands aside there.
 */
#ifndef CRASH_TRACE_H
#define CRASH_TRACE_H

#include "log.h"

#ifndef M2HLE_BUILD_FLAVOR
#define M2HLE_BUILD_FLAVOR "unknown"
#endif

#if defined(__SANITIZE_ADDRESS__)
#  define CRASH_TRACE_SANITIZED 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define CRASH_TRACE_SANITIZED 1
#  endif
#endif

#if defined(__GLIBC__) && !defined(__EMSCRIPTEN__) && !defined(CRASH_TRACE_SANITIZED)
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void crash_trace__put(int fd, const char *s) {
    size_t n = strlen(s);
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

static void crash_trace__handler(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    void *frames[64];
    int n = backtrace(frames, 64);
    char line[160];
    /* snprintf is not on the async-signal-safe list; glibc's does not allocate
     * for these conversions, and the process is going down either way. */
    snprintf(line, sizeof line, "\n=== m2hle %s (%s): fatal signal %d (%s) at %p ===\n",
             M2HLE_VERSION, M2HLE_BUILD_FLAVOR, sig, strsignal(sig), si ? si->si_addr : NULL);
    int fds[2] = { STDERR_FILENO, g_log.file ? fileno(g_log.file) : -1 };
    for (int i = 0; i < 2; i++) {
        if (fds[i] < 0) continue;
        crash_trace__put(fds[i], line);
        backtrace_symbols_fd(frames, n, fds[i]);
        crash_trace__put(fds[i], "=== end of backtrace ===\n");
    }
    /* SA_RESETHAND put the default action back: re-raise, so the exit status
     * and any core dump are the signal's own. */
    raise(sig);
}

static inline void crash_trace_install(void) {
    static char alt[64 * 1024];            /* a stack overflow has none left */
    stack_t ss = { .ss_sp = alt, .ss_size = sizeof alt, .ss_flags = 0 };
    sigaltstack(&ss, NULL);
    void *warm[2];
    backtrace(warm, 2);                    /* loads libgcc now, not in the handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_trace__handler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        sigaction(sigs[i], &sa, NULL);
}
#else
static inline void crash_trace_install(void) {}
#endif

#endif /* CRASH_TRACE_H */
