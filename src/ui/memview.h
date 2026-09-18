/*
 * memview.h — memory inspector window for the i960 address space, plus the
 * region-browser panel that m68k_memview.h reuses.
 *
 * The hex/ASCII grid itself is vendor/imgui_club's MemoryEditor, reached
 * through mem_edit.h: scrolling, in-place hex and ASCII editing, the options
 * popup (column count, HexII, grey-out-zeroes) and the data preview footer
 * (int8..int64 / float / double, either endianness, dec/hex/bin) all come
 * from there. This file adds what is ours: the region table, a jump box that
 * takes an address anywhere in the space and finds the region holding it, and
 * u32/f32 pokes.
 *
 * MemoryEditor addresses one contiguous block, so the panel shows one region
 * at a time — the i960's space is 4 GB of mostly nothing, and a flat view of
 * it would hand ImGui a scroll range no float can hold.
 *
 * All reads/writes go through the memory bus, so MMIO callbacks fire the same
 * way they would from CPU code. That is deliberate — it is how you watch a
 * register behave — but it also means browsing a FIFO region consumes it.
 *
 * Targets the dear_bindings cimgui API (defaulted-arg variants, no
 * suffixes like `_Vec4` / `_Str` / `_BoolPtr`).
 */
#ifndef MEMVIEW_H
#define MEMVIEW_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cimgui.h"
#include "mem_edit.h"
#include "memory.h"
#include "log.h"

/* ---- Region-browser panel (shared with the 68K window) -------------------- */

typedef struct {
    const char *name;
    uint32_t    base;
    uint32_t    size;
} memview_region_t;

/* Byte access drives the grid; the 32-bit pair drives the pokes, so that a
 * poke reaches an MMIO region as the one 4-byte access the hardware sees
 * rather than as four byte writes. */
typedef struct {
    uint8_t  (*read8)  (void *user, uint32_t addr);
    void     (*write8) (void *user, uint32_t addr, uint8_t val);
    uint32_t (*read32) (void *user, uint32_t addr);
    void     (*write32)(void *user, uint32_t addr, uint32_t val);
    void      *user;
} memview_bus_t;

typedef struct {
    mem_edit_t *edit;
    int         region;
    char        jump_buf[16];
    char        u32_buf[12];
    char        f32_buf[24];
} memview_panel_t;

static inline void memview_panel_draw(memview_panel_t *p,
                                      const memview_region_t *regions, int n_regions,
                                      const memview_bus_t *bus,
                                      int addr_digits) {
    if (n_regions <= 0) { igTextDisabled("no regions mapped"); return; }
    if (!p->edit) p->edit = mem_edit_create(addr_digits);
    mem_edit_set_bus(p->edit, bus->read8, bus->write8, bus->user);

    if (p->region < 0 || p->region >= n_regions) p->region = 0;
    const memview_region_t *r = &regions[p->region];

    igSetNextItemWidth(260);
    if (igBeginCombo("##region", r->name, 0)) {
        for (int i = 0; i < n_regions; i++) {
            char label[96];
            snprintf(label, sizeof(label), "%-16s %0*X..%0*X", regions[i].name,
                     addr_digits, regions[i].base,
                     addr_digits, regions[i].base + regions[i].size - 1);
            if (igSelectableEx(label, i == p->region, 0, (ImVec2){0, 0})) p->region = i;
        }
        igEndCombo();
    }

    /* Jump anywhere in the space: the region that holds the address comes
     * along with it. (MemoryEditor's own goto box, on the options line below,
     * stays inside the region on show.) */
    igSameLine();
    igSetNextItemWidth(addr_digits * 10.0f + 16.0f);
    if (igInputText("##jump", p->jump_buf, sizeof(p->jump_buf),
                    ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
        uint32_t addr = 0;
        if (sscanf(p->jump_buf, "%x", &addr) == 1) {
            int hit = -1;
            for (int i = 0; i < n_regions && hit < 0; i++)
                if (addr >= regions[i].base && addr - regions[i].base < regions[i].size) hit = i;
            if (hit >= 0) {
                p->region = hit;
                r = &regions[hit];
                mem_edit_goto(p->edit, addr);
            } else {
                LOG_WARN("memview: 0x%08X is not in any mapped region", addr);
            }
        }
    }
    igSameLine();
    igTextDisabled("jump (hex)");

    /* Poke row. The grid edits bytes in place; this is for the widths the
     * board's own state is written in. */
    uint32_t sel;
    if (mem_edit_selected(p->edit, &sel)) {
        uint32_t u32 = bus->read32 ? bus->read32(bus->user, sel & ~3u) : 0;
        float    f;  memcpy(&f, &u32, 4);

        igText("poke %0*X", addr_digits, sel & ~3u);
        igSameLine();
        igSetNextItemWidth(100);
        if (igInputText("##pu32", p->u32_buf, sizeof(p->u32_buf),
                        ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
            unsigned nv = 0;
            if (bus->write32 && sscanf(p->u32_buf, "%x", &nv) == 1) {
                bus->write32(bus->user, sel & ~3u, nv);
                LOG_INFO("memview: wrote u32 0x%08X to 0x%08X", nv, sel & ~3u);
            }
            p->u32_buf[0] = 0;
        }
        igSameLine();
        igTextDisabled("u32 (now %08X)", u32);

        igSameLine();
        igSetNextItemWidth(120);
        if (igInputText("##pf32", p->f32_buf, sizeof(p->f32_buf),
                        ImGuiInputTextFlags_EnterReturnsTrue)) {
            float nf = 0;
            if (bus->write32 && sscanf(p->f32_buf, "%f", &nf) == 1) {
                uint32_t nv; memcpy(&nv, &nf, 4);
                bus->write32(bus->user, sel & ~3u, nv);
                LOG_INFO("memview: wrote f32 %g (0x%08X) to 0x%08X", nf, nv, sel & ~3u);
            }
            p->f32_buf[0] = 0;
        }
        igSameLine();
        igTextDisabled("f32 (now %g)", f);
    } else {
        igTextDisabled("click a byte to poke a u32 / f32 at its word");
    }

    igSeparator();
    mem_edit_draw(p->edit, r->base, r->size);
}

/* ---- i960 window --------------------------------------------------------- */

static memview_panel_t g_memview;

static uint8_t memview_read8(void *user, uint32_t addr) {
    return (uint8_t)mem_read8((memory_bus_t *)user, addr);
}
static void memview_write8(void *user, uint32_t addr, uint8_t val) {
    mem_write8((memory_bus_t *)user, addr, val);
}
static uint32_t memview_read32(void *user, uint32_t addr) {
    return mem_read32((memory_bus_t *)user, addr);
}
static void memview_write32(void *user, uint32_t addr, uint32_t val) {
    mem_write32((memory_bus_t *)user, addr, val);
}

static inline void memview_draw(memory_bus_t *bus, bool *p_open) {
    igSetNextWindowSize((ImVec2){700, 520}, ImGuiCond_FirstUseEver);
    if (!igBegin("Memory viewer", p_open, 0)) { igEnd(); return; }

    /* The bus region table is the region list, in its declaration order —
     * which is also the order mem_find_region resolves overlaps in, so the
     * aliases (TILE_MIRROR, TEXRAM0_A/_M) show up as their own entries. */
    static memview_region_t regions[MEM_REGIONS_MAX];
    int n = 0;
    for (int i = 0; i < bus->region_count && n < MEM_REGIONS_MAX; i++) {
        if (bus->regions[i].size == 0) continue;
        regions[n].name = bus->regions[i].name;
        regions[n].base = bus->regions[i].base;
        regions[n].size = bus->regions[i].size;
        n++;
    }

    const memview_bus_t mb = {
        memview_read8, memview_write8, memview_read32, memview_write32, bus
    };
    memview_panel_draw(&g_memview, regions, n, &mb, 8);

    igEnd();
}

#endif /* MEMVIEW_H */
