/*
 * log.h — ring-buffer logger + on-disk session log.
 *
 * Severity API: log_msg(level, fmt, ...) or the LOG_* macros below.
 * Unknown COP commands and unhandled MMIO writes should LOG_WARN so
 * new-game support work surfaces them automatically (per CLAUDE.md).
 */
#ifndef LOG_H
#define LOG_H

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#define LOG_MAX_LINES   1024
#define LOG_MAX_LINE    256
#define LOG_FILE_PATH   "m2hle.log"

typedef enum {
    LOG_LVL_INFO,
    LOG_LVL_WARN,
    LOG_LVL_ERROR,
    LOG_LVL_DEBUG
} log_level_t;

typedef struct {
    char         lines[LOG_MAX_LINES][LOG_MAX_LINE];
    log_level_t  levels[LOG_MAX_LINES];
    int          count;
    int          scroll_to_bottom;
    int          break_on_warn;
    volatile int warn_triggered;
    FILE        *file;
    int          file_open_attempted;
} log_state_t;

static log_state_t g_log = {0};

static inline void log_file_ensure_open(void) {
    if (g_log.file || g_log.file_open_attempted) return;
    g_log.file_open_attempted = 1;
#ifdef __EMSCRIPTEN__
    /* No session log in a browser. The only file system there is MEMFS, which is
     * memory: a log written to it grows by a line at a time for as long as the
     * tab stays open. The browser console is the log (see log_msg). */
    return;
#endif
    g_log.file = fopen(LOG_FILE_PATH, "w"); /* truncate per session */
    if (g_log.file) {
        fprintf(g_log.file, "=== m2-hle session log ===\n");
        fflush(g_log.file);
    }
}

static inline void log_file_close(void) {
    if (g_log.file) { fclose(g_log.file); g_log.file = NULL; }
}

static inline void log_init(void) {
#if !defined(NDEBUG) && defined(_WIN32)
    if (AllocConsole()) {
        freopen("CONOUT$", "w", stderr);
        freopen("CONOUT$", "w", stdout);
    }
#endif
}

static inline void log_msg(log_level_t level, const char *fmt, ...) {
    static const char *prefixes[] = {
        "[INFO]", "[WARN]", "[ERR ]", "[DBG ]"
    };
    char buf[LOG_MAX_LINE];

    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    log_file_ensure_open();
    if (g_log.file) {
        fprintf(g_log.file, "%s %s\n", prefixes[level], buf);
        fflush(g_log.file); /* per-line flush so `tail -f` works */
    }

    int idx = g_log.count % LOG_MAX_LINES;
    strncpy(g_log.lines[idx], buf, LOG_MAX_LINE - 1);
    g_log.lines[idx][LOG_MAX_LINE - 1] = '\0';
    g_log.levels[idx] = level;
    g_log.count++;
    g_log.scroll_to_bottom = 1;

    if (level == LOG_LVL_WARN && g_log.break_on_warn) {
        g_log.warn_triggered = 1;
    }

    /* A release build is silent on a desktop, where m2hle.log is the record. In a
     * browser there is no file, so the lines go to the console in every build. */
#if !defined(NDEBUG) || defined(__EMSCRIPTEN__)
    fprintf(stderr, "%s %s\n", prefixes[level], buf);
    fflush(stderr);
#endif
}

#define LOG_INFO(fmt, ...)  log_msg(LOG_LVL_INFO,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  log_msg(LOG_LVL_WARN,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_msg(LOG_LVL_ERROR, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) log_msg(LOG_LVL_DEBUG, fmt, ##__VA_ARGS__)

#endif /* LOG_H */
