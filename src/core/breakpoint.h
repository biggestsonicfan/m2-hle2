/*
 * breakpoint.h — software breakpoint state + IP-match check.
 *
 * Called by the emu thread (bp_check) before stepping each instruction;
 * the UI window in src/ui/breakpoint_window.h handles add/remove/toggle.
 */
#ifndef BREAKPOINT_H
#define BREAKPOINT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "log.h"

#define BP_MAX 64

typedef struct {
    uint32_t addr;
    char     label[64];
    bool     enabled;
    bool     active;       /* slot in use */
} breakpoint_t;

typedef struct {
    breakpoint_t list[BP_MAX];
    int          count;
    volatile int hit;
    uint32_t     hit_addr;
} bp_state_t;

static bp_state_t g_bp = {0};

static inline void bp_init(void) { memset(&g_bp, 0, sizeof(g_bp)); }

static inline void bp_add(uint32_t addr, const char *label) {
    for (int i = 0; i < BP_MAX; i++) {
        if (!g_bp.list[i].active) {
            g_bp.list[i].addr = addr;
            g_bp.list[i].enabled = true;
            g_bp.list[i].active = true;
            strncpy(g_bp.list[i].label, label ? label : "", 63);
            g_bp.count++;
            LOG_INFO("breakpoint added: 0x%08X (%s)", addr, g_bp.list[i].label);
            return;
        }
    }
    LOG_WARN("breakpoint table full");
}

static inline void bp_remove(int index) {
    if (index >= 0 && index < BP_MAX && g_bp.list[index].active) {
        LOG_INFO("breakpoint removed: 0x%08X (%s)", g_bp.list[index].addr, g_bp.list[index].label);
        g_bp.list[index].active = false;
        g_bp.count--;
    }
}

/* Called from the emu thread before each instruction. */
static inline int bp_check(uint32_t ip) {
    for (int i = 0; i < BP_MAX; i++) {
        if (g_bp.list[i].active && g_bp.list[i].enabled && g_bp.list[i].addr == ip) {
            g_bp.hit = 1;
            g_bp.hit_addr = ip;
            return 1;
        }
    }
    return 0;
}

#endif /* BREAKPOINT_H */
