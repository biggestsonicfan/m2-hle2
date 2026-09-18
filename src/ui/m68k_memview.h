/*
 * m68k_memview.h — memory inspector for the 68K sound CPU address space.
 *
 * Same panel as the i960 window (memview.h): vendor/imgui_club's MemoryEditor
 * for the grid, our region list and jump box around it.
 *
 * Reads go through sound_m68k_peek, a debugger's read that leaves the SCSP
 * alone — the play position and the status bits are live state the driver
 * paces itself by, and a hex dump must not disturb them. Writes go through
 * sound_m68k_write (safe for wave RAM; SCSP register writes act exactly as
 * the driver's would, key-on included).
 *
 * The sample ROM's three windows are listed separately: the 68K sees the
 * first 2 MB at 0x800000 and the rest through banks 4 and 5 at 0xA00000 and
 * 0xE00000, and in STF most of the instruments live up there.
 */
#ifndef M68K_MEMVIEW_H
#define M68K_MEMVIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "cimgui.h"
#include "memview.h"
#include "sound.h"
#include "constants.h"

static memview_panel_t g_m68k_memview;

static uint8_t m68k_memview_read8(void *user, uint32_t addr) {
    return (uint8_t)sound_m68k_peek((sound_state_t *)user, addr, 1);
}
static void m68k_memview_write8(void *user, uint32_t addr, uint8_t val) {
    sound_m68k_write(user, addr, val, 1);
}
static uint32_t m68k_memview_read32(void *user, uint32_t addr) {
    return sound_m68k_peek((sound_state_t *)user, addr, 4);
}
static void m68k_memview_write32(void *user, uint32_t addr, uint32_t val) {
    sound_m68k_write(user, addr, val, 4);
}

static inline void m68k_memview_draw(bool *p_open) {
    igSetNextWindowSize((ImVec2){700, 520}, ImGuiCond_FirstUseEver);
    if (!igBegin("68K memory viewer", p_open, 0)) { igEnd(); return; }

    static const memview_region_t regions[] = {
        { "wave RAM",    M68K_WAVE_BASE,   M68K_WAVE_SIZE   },
        { "SCSP regs",   M68K_SCSP_BASE,   M68K_SCSP_SIZE   },
        { "sound ctrl",  M68K_SNDCTL_BASE, M68K_SNDCTL_SIZE },
        { "program ROM", M68K_ROM_BASE,    M68K_ROM_SIZE    },
        { "sample +0",   M68K_SAMPLE_BASE, M68K_SAMPLE_SIZE },
        { "sample bank4", 0x00A00000u,     0x00400000u      },
        { "sample bank5", 0x00E00000u,     0x00200000u      },
    };

    const memview_bus_t mb = {
        m68k_memview_read8, m68k_memview_write8,
        m68k_memview_read32, m68k_memview_write32, &g_sound
    };
    memview_panel_draw(&g_m68k_memview, regions,
                       (int)(sizeof regions / sizeof regions[0]), &mb, 6);

    igEnd();
}

#endif /* M68K_MEMVIEW_H */
