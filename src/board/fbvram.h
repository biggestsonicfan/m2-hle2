/*
 * fbvram.h — the 3D board's framebuffer, as the i960 sees it in test mode
 * (board-level; Pinboard #1517).
 *
 * MAME model2.cpp maps the two 512 KB frame buffers at 0x11600000 (bank A)
 * and 0x11680000 (bank B), 512 pixels a row, xRGB 1:5:5:5 with red in bits
 * 5-9, green 10-14, blue 0-4. With bit 0 of 0x10000000 set (render test mode,
 * "the host can access memories that are always being reloaded") the screen
 * shows the buffer in the 3D layer's place, between the tile layers, and the
 * rasterizer draws nothing (MAME screen_update / draw_framebuffer).
 *
 * Last Bronx draws its title picture this way: its decompressor (0x7A000)
 * stores each pixel in both banks, and its fades read bank A back. Bank A is
 * the one shown (stock MAME picks a bank by frame parity; both hold the same
 * picture). No other game is known to use it, so only a profile that needs it
 * installs the region, and every other game's bus, savestates and rollback
 * stay as they were.
 *
 * A pixel's colour is each channel's 5 bits through its colour translation
 * table at luma 0x40 and the monitor curve, as a tile pen's
 * (video_pen_channels): chan[0][r5], chan[1][g5], chan[2][b5].
 */
#ifndef FBVRAM_H
#define FBVRAM_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "constants.h"
#include "memory.h"
#include "savestate_reg.h"

#define FBVRAM_BASE   0x11600000u
#define FBVRAM_BANK   0x00080000u            /* bank B at FBVRAM_BASE + this */
#define FBVRAM_SIZE   (2u * FBVRAM_BANK)
#define FBVRAM_PITCH  512                    /* pixels a row */

typedef struct {
    uint8_t ram[FBVRAM_SIZE];               /* bank A, then bank B, little-endian words */
} fbvram_t;

static fbvram_t g_fbvram;
static volatile uint32_t g_fbvram_gen;       /* bumped by every write that changes it */

/* Clear both banks and map them. Called by a profile's install, after
 * mem_init; the savestate part is the profile's (id). */
static inline void fbvram_install(memory_bus_t *bus, const char *id) {
    memset(&g_fbvram, 0, sizeof g_fbvram);
    g_fbvram_gen++;
    mem_region_t *r = mem_add_region(bus, "FBVRAM", FBVRAM_BASE, FBVRAM_SIZE, g_fbvram.ram, 0);
    if (r) r->change_gen = &g_fbvram_gen;
    mem_regions_changed(bus);
    savestate_extra(id, "FBVRAM", &g_fbvram, sizeof g_fbvram);
}

/* Whether the screen shows the frame buffer in the 3D layer's place: test
 * mode on a bus the region is mapped on (mem_init drops it for the next game). */
static inline bool fbvram_shown(const memory_bus_t *bus) {
    if (!(bus->unknown_vid[0] & 0x01u)) return false;
    for (int i = 0; i < bus->region_count; i++)
        if (bus->regions[i].data == g_fbvram.ram) return true;
    return false;
}

/* Bank A as the screen shows it, into rgba (VIDEO_WIDTH x VIDEO_HEIGHT, row 0
 * top, opaque). The window follows the CRTC offsets as MAME's does: x - 84 -
 * H, y - 2 - V; what falls outside the buffer is black. */
static inline void fbvram_compose(const memory_bus_t *bus, const uint8_t chan[3][32], uint8_t *rgba) {
    int16_t hs = (int16_t)mem_read16((memory_bus_t *)bus, H_SYNC_BASE);
    int16_t vs = (int16_t)mem_read16((memory_bus_t *)bus, V_SYNC_BASE);
    int xoffs = -(84 + hs), yoffs = (512 - 384) - (130 + vs);
    for (int y = 0; y < VIDEO_HEIGHT; y++) {
        uint8_t *out = rgba + (size_t)y * VIDEO_WIDTH * 4;
        int row = y + yoffs;
        for (int x = 0; x < VIDEO_WIDTH; x++, out += 4) {
            int col = x + xoffs;
            uint16_t p = 0;
            if (row >= 0 && row < (int)(FBVRAM_BANK / 2 / FBVRAM_PITCH) && col >= 0 && col < FBVRAM_PITCH) {
                uint32_t o = ((uint32_t)row * FBVRAM_PITCH + (uint32_t)col) * 2u;
                p = (uint16_t)(g_fbvram.ram[o] | g_fbvram.ram[o + 1] << 8);
            }
            out[0] = chan[0][(p >> 5) & 31];
            out[1] = chan[1][(p >> 10) & 31];
            out[2] = chan[2][p & 31];
            out[3] = 255;
        }
    }
}

#endif /* FBVRAM_H */
