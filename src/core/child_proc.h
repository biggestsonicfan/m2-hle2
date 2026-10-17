/*
 * child_proc.h — this program again, as a child that dies with it
 * (Pinboard #987: a Versus's second cabinet, src/net/comm_local.h).
 *
 *   child_proc_start  run this executable with argv; the child ends when this
 *                     process does, however it ends (Linux: PR_SET_PDEATHSIG;
 *                     Windows: a job object that kills on close)
 *   child_proc_stop   end it now and reap it
 *   child_proc_alive  whether it still runs
 *
 * Desktop hosts only (Linux, Windows): the web and libretro builds have no
 * second process to start.
 */
#ifndef CORE_CHILD_PROC_H
#define CORE_CHILD_PROC_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
typedef struct { HANDLE process, job; } child_proc_t;
#elif defined(__linux__)
#  include <signal.h>
#  include <stdlib.h>
#  include <sys/prctl.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
typedef struct { pid_t pid; } child_proc_t;
#endif

#if defined(_WIN32) || defined(__linux__)

#ifdef _WIN32
/* One byte into out at at, if it fits. */
static inline size_t child_proc__put(char *out, size_t cap, size_t at, char ch) {
    if (at < cap) out[at++] = ch;
    return at;
}

/* a in quotes: a run of backslashes is doubled before a quote or the end. */
static inline size_t child_proc__quoted(char *out, size_t cap, size_t at, const char *a) {
    at = child_proc__put(out, cap, at, '"');
    for (const char *p = a; ; p++) {
        size_t slashes = 0;
        while (*p == '\\') { slashes++; p++; }
        size_t n = *p == '"' ? slashes * 2 + 1 : !*p ? slashes * 2 : slashes;
        while (n--) at = child_proc__put(out, cap, at, '\\');
        if (!*p) break;
        at = child_proc__put(out, cap, at, *p);
    }
    return child_proc__put(out, cap, at, '"');
}

/* One argument, quoted as CommandLineToArgvW reads it back. */
static inline size_t child_proc__quote(char *out, size_t cap, size_t at, const char *a) {
    if (at) at = child_proc__put(out, cap, at, ' ');
    if (*a && !strpbrk(a, " \t\"")) {
        if (at < cap) at += (size_t)snprintf(out + at, cap - at, "%s", a);
    } else {
        at = child_proc__quoted(out, cap, at, a);
    }
    if (at < cap) out[at] = '\0';
    return at;
}
#endif

/* argv[0] is ignored: the child is this executable. False if it did not start. */
static inline bool child_proc_start(child_proc_t *c, char *const argv[]) {
#ifdef _WIN32
    char exe[MAX_PATH], line[8192];
    if (!GetModuleFileNameA(NULL, exe, sizeof exe)) return false;
    size_t at = child_proc__quote(line, sizeof line, 0, exe);
    for (int i = 1; argv[i]; i++) at = child_proc__quote(line, sizeof line, at, argv[i]);
    if (at >= sizeof line - 1) return false;
    c->job = CreateJobObjectA(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
    memset(&lim, 0, sizeof lim);
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (c->job) SetInformationJobObject(c->job, JobObjectExtendedLimitInformation, &lim, sizeof lim);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(exe, line, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) return false;
    if (c->job) AssignProcessToJobObject(c->job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    c->process = pi.hProcess;
    return true;
#else
    char exe[4096];   /* its real path, so the child is named as this one is */
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return false;
    exe[n] = '\0';
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) _exit(0);   /* the parent went before the line above */
        execv(exe, argv);
        _exit(127);
    }
    c->pid = pid;
    return true;
#endif
}

static inline bool child_proc_alive(child_proc_t *c) {
#ifdef _WIN32
    return c->process && WaitForSingleObject(c->process, 0) == WAIT_TIMEOUT;
#else
    if (c->pid <= 0) return false;
    if (waitpid(c->pid, NULL, WNOHANG) == 0) return true;
    c->pid = 0;   /* reaped */
    return false;
#endif
}

static inline void child_proc_stop(child_proc_t *c) {
#ifdef _WIN32
    if (c->process) {
        TerminateProcess(c->process, 0);
        WaitForSingleObject(c->process, 5000);
        CloseHandle(c->process);
    }
    if (c->job) CloseHandle(c->job);
    c->process = c->job = NULL;
#else
    if (c->pid > 0) {
        kill(c->pid, SIGTERM);
        waitpid(c->pid, NULL, 0);
    }
    c->pid = 0;
#endif
}

#endif /* _WIN32 || __linux__ */

#endif /* CORE_CHILD_PROC_H */
