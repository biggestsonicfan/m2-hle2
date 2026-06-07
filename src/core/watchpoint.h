/*
 * watchpoint.h — memory read/write watchpoints (data breakpoints).
 *
 * wp_check_write / wp_check_read are called from the memory bus (memory.h) on
 * every access; a match sets g_wp.hit, which the emu thread polls to stop just
 * like a code breakpoint.  Used by the MCP bridge to locate the code that
 * touches a given address (e.g. texture-RAM uploads).
 */
#ifndef WATCHPOINT_H
#define WATCHPOINT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "log.h"

#define WP_MAX 16

typedef struct {
    uint32_t lo, hi;        /* watched byte range [lo, hi)  */
    char     label[64];
    bool     on_write;
    bool     on_read;
    bool     enabled;
    bool     active;        /* slot in use */
} watchpoint_t;

typedef struct {
    watchpoint_t list[WP_MAX];
    int          count;
    volatile int hit;       /* set by wp_check, cleared by the emu thread */
    uint32_t     hit_addr;  /* address that triggered */
    uint32_t     hit_val;   /* value written (0 for reads) */
    uint32_t     hit_ip;    /* i960 IP at the access */
    bool         hit_write; /* true = write, false = read */
} wp_state_t;

static wp_state_t g_wp = {0};

static inline void wp_init(void) { memset(&g_wp, 0, sizeof(g_wp)); }

static inline int wp_add(uint32_t lo, uint32_t hi, bool on_write, bool on_read,
                         const char *label) {
    if (hi <= lo) hi = lo + 1;
    for (int i = 0; i < WP_MAX; i++) {
        if (!g_wp.list[i].active) {
            g_wp.list[i].lo = lo;  g_wp.list[i].hi = hi;
            g_wp.list[i].on_write = on_write;
            g_wp.list[i].on_read  = on_read;
            g_wp.list[i].enabled  = true;
            g_wp.list[i].active   = true;
            strncpy(g_wp.list[i].label, label ? label : "", 63);
            g_wp.list[i].label[63] = 0;
            g_wp.count++;
            LOG_INFO("watchpoint added: [0x%08X,0x%08X) %s%s (%s)", lo, hi,
                     on_write ? "W" : "", on_read ? "R" : "", g_wp.list[i].label);
            return i;
        }
    }
    LOG_WARN("watchpoint table full");
    return -1;
}

static inline int wp_remove_addr(uint32_t addr) {
    int removed = 0;
    for (int i = 0; i < WP_MAX; i++) {
        if (g_wp.list[i].active && addr >= g_wp.list[i].lo && addr < g_wp.list[i].hi) {
            g_wp.list[i].active = false; g_wp.count--; removed++;
        }
    }
    return removed;
}

static inline void wp_clear_all(void) {
    for (int i = 0; i < WP_MAX; i++) g_wp.list[i].active = false;
    g_wp.count = 0;
}

/* Called from the memory bus on each access.  `ip` is the i960 IP performing it.
 * Records the FIRST match per slice; the emu thread clears g_wp.hit after stop. */
static inline void wp_check(uint32_t addr, uint32_t val, bool is_write, uint32_t ip) {
    if (g_wp.count == 0 || g_wp.hit) return;
    for (int i = 0; i < WP_MAX; i++) {
        const watchpoint_t *w = &g_wp.list[i];
        if (!w->active || !w->enabled) continue;
        if (addr < w->lo || addr >= w->hi) continue;
        if (is_write ? !w->on_write : !w->on_read) continue;
        g_wp.hit = 1;
        g_wp.hit_addr = addr;
        g_wp.hit_val = val;
        g_wp.hit_ip = ip;
        g_wp.hit_write = is_write;
        return;
    }
}

#endif /* WATCHPOINT_H */
