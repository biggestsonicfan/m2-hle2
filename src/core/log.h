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
#  define log__strnicmp _strnicmp
#else
#  include <strings.h>
#  define log__strnicmp strncasecmp
#endif

#define LOG_MAX_LINES   1024
#define LOG_MAX_LINE    256
#define LOG_FILE_PATH   "m2hle.log"
/* The most one session writes to its log file. Past it the file gets one
 * notice and then only errors, for LOG_FILE_ERR_RESERVE more bytes: what a
 * long session is read for usually comes at its end. The log window keeps
 * everything. A normal session writes a few kilobytes, and nothing in it is
 * worth a full disk (issue #69: logs of 2.2 GB and 9.9 GB, and one that
 * filled C: under a Docker VM). */
#define LOG_FILE_CAP         (64ull * 1024ull * 1024ull)
#define LOG_FILE_ERR_RESERVE (1ull * 1024ull * 1024ull)
#define LOG_MAX_CHANNELS 16         /* --log-level CHANNEL=LEVEL entries */

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
    unsigned long long file_bytes;
    unsigned long long file_cap;  /* 0 = LOG_FILE_CAP; see log_set_file_cap */
    volatile int file_capped;     /* past the cap: errors only, until the reserve */
    char         path[512];     /* "" = LOG_FILE_PATH; see log_set_path */
    int          file_off;      /* --log off */
    int          min_rank;      /* --log-level default: lines below it are dropped */
    int          channel_count; /* --log-level CHANNEL=LEVEL overrides */
    char         channel_name[LOG_MAX_CHANNELS][24];
    int          channel_rank[LOG_MAX_CHANNELS];
} log_state_t;

static log_state_t g_log = {0};

/* Where the session log goes: a path, or "off" for no file at all. Call it
 * before the first log line, which opens (and truncates) the file. Instances
 * that share a working directory need one each: every open truncates while the
 * others keep their offsets, which leaves a file of NULs and interleaved lines. */
static inline void log_set_path(const char *path) {
    if (!path || !path[0]) { g_log.path[0] = '\0'; g_log.file_off = 0; return; }
    if (!strcmp(path, "off") || !strcmp(path, "none")) { g_log.file_off = 1; return; }
    g_log.file_off = 0;
    strncpy(g_log.path, path, sizeof(g_log.path) - 1);
    g_log.path[sizeof(g_log.path) - 1] = '\0';
}

/* The cap, for a test that wants to reach it without writing 64 MB. */
static inline void log_set_file_cap(unsigned long long bytes) {
    g_log.file_cap = bytes;
}

static inline const char *log_file_path(void) {
    return g_log.path[0] ? g_log.path : LOG_FILE_PATH;
}

/* Verbosity, per channel. A channel is the tag a message already starts with
 * ("mem: ...", "netplay: ...", "SHARC: ..."), matched without regard to case;
 * a line with no such tag is only subject to the default level. The spec is a
 * comma list of LEVEL or CHANNEL=LEVEL, e.g. "warn,mem=error,netplay=debug",
 * and LEVEL is debug | info | warn | error | off. The default keeps
 * everything, as before.
 *
 * The level enum is in no useful order (DEBUG was added last), so rank it. */

static inline int log__rank(log_level_t l) {
    switch (l) {
        case LOG_LVL_DEBUG: return 0;
        case LOG_LVL_INFO:  return 1;
        case LOG_LVL_WARN:  return 2;
        default:            return 3;
    }
}

static inline int log__rank_named(const char *s, size_t n) {
    static const char *names[] = { "debug", "info", "warn", "error", "off" };
    for (int i = 0; i < 5; i++)
        if (strlen(names[i]) == n && !log__strnicmp(s, names[i], n)) return i;
    return -1;
}

/* Returns 0, leaving the levels as they were, if any item does not parse. */
static inline int log_set_levels(const char *spec) {
    int  def = g_log.min_rank, nch = 0;
    char names[LOG_MAX_CHANNELS][24];
    int  ranks[LOG_MAX_CHANNELS];
    const char *p = spec ? spec : "";
    while (*p) {
        const char *e = strchr(p, ','); if (!e) e = p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(e - p));
        if (!eq) {
            if ((def = log__rank_named(p, (size_t)(e - p))) < 0) return 0;
        } else {
            size_t cl = (size_t)(eq - p);
            int r = log__rank_named(eq + 1, (size_t)(e - eq - 1));
            if (r < 0 || cl == 0 || cl >= sizeof names[0] || nch == LOG_MAX_CHANNELS) return 0;
            memcpy(names[nch], p, cl); names[nch][cl] = '\0';
            ranks[nch++] = r;
        }
        p = *e ? e + 1 : e;
    }
    g_log.min_rank = def;
    g_log.channel_count = nch;
    memcpy(g_log.channel_name, names, sizeof names);
    memcpy(g_log.channel_rank, ranks, sizeof ranks);
    return 1;
}

/* The threshold for a formatted line: its channel's, or the default. */
static inline int log__threshold(const char *line) {
    if (!g_log.channel_count) return g_log.min_rank;
    size_t n = 0;
    while (n < 24 && line[n] && line[n] != ':' && line[n] != ' ') n++;
    if (line[n] != ':') return g_log.min_rank;
    for (int i = 0; i < g_log.channel_count; i++)
        if (strlen(g_log.channel_name[i]) == n && !log__strnicmp(line, g_log.channel_name[i], n))
            return g_log.channel_rank[i];
    return g_log.min_rank;
}

static inline void log_file_ensure_open(void) {
    if (g_log.file || g_log.file_open_attempted) return;
    g_log.file_open_attempted = 1;
#ifdef __EMSCRIPTEN__
    /* No session log in a browser. The only file system there is MEMFS, which is
     * memory: a log written to it grows by a line at a time for as long as the
     * tab stays open. The browser console is the log (see log_msg). */
    return;
#endif
    if (g_log.file_off) return;
    g_log.file = fopen(log_file_path(), "w"); /* truncate per session */
    if (g_log.file) {
        g_log.file_bytes = 0;
        g_log.file_capped = 0;
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
    if (log__rank(level) < log__threshold(buf)) return;

    /* The file is never closed while the session runs. This takes no lock and
     * is called from the emu, MCP, A/V and main threads, so closing it at the
     * cap would let a thread that had already read the pointer write through a
     * closed FILE* (review on #69). Past the cap it stays open and takes only
     * errors, until the reserve is spent. Two threads racing over the cap can
     * each write the notice: a duplicate line, not a crash. */
    log_file_ensure_open();
    FILE *f = g_log.file;
    if (f) {
        unsigned long long cap = g_log.file_cap ? g_log.file_cap : LOG_FILE_CAP;
        int take = !g_log.file_capped
                || (level == LOG_LVL_ERROR
                    && g_log.file_bytes < cap + LOG_FILE_ERR_RESERVE);
        if (take) {
            int n = fprintf(f, "%s %s\n", prefixes[level], buf);
            if (n > 0) g_log.file_bytes += (unsigned)n;
            if (!g_log.file_capped && g_log.file_bytes >= cap) {
                g_log.file_capped = 1;
                fprintf(f, "[WARN] log: this file reached its cap (%llu bytes); from here "
                           "only errors are written, the log window has everything\n", cap);
            }
            fflush(f); /* per-line flush so `tail -f` works */
        }
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
