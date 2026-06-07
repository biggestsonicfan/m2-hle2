/*
 * memview.h — hex/ASCII memory inspector window.
 *
 * Jump-to-address, page nav, byte select, byte/u32/float edit, color
 * highlight (dim for 0x00, white for non-zero, yellow for selected).
 * All reads/writes go through the memory bus, so MMIO callbacks fire
 * the same way they would from CPU code.
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
#include "memory.h"
#include "log.h"

#define MEMVIEW_COLS 16
#define MEMVIEW_ROWS 16

typedef struct {
    uint32_t base_addr;
    char     jump_buf[16];
    int      selected_offset;   /* byte offset from base_addr, -1 = none */
    bool     editing;
    char     edit_buf[4];
    bool     u32_editing;
    char     u32_buf[12];
    bool     float_editing;
    char     float_buf[24];
} memview_state_t;

static memview_state_t g_memview = { .base_addr = 0, .selected_offset = -1 };

static inline void memview_reset(uint32_t base) {
    g_memview.base_addr = base & ~0xFu;
    g_memview.selected_offset = -1;
    g_memview.editing = false;
    g_memview.u32_editing = false;
    g_memview.float_editing = false;
    g_memview.jump_buf[0] = 0;
    g_memview.edit_buf[0] = 0;
    g_memview.u32_buf[0] = 0;
    g_memview.float_buf[0] = 0;
}

static inline void memview_draw(memory_bus_t *bus, bool *p_open) {
    igSetNextWindowSize((ImVec2){640, 440}, ImGuiCond_FirstUseEver);
    if (!igBegin("Memory viewer", p_open, 0)) { igEnd(); return; }

    igSetNextItemWidth(110);
    if (igInputText("##jump", g_memview.jump_buf, sizeof(g_memview.jump_buf),
                    ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
        uint32_t addr = 0;
        if (sscanf(g_memview.jump_buf, "%x", &addr) == 1) {
            g_memview.base_addr = addr & ~0xFu;
            g_memview.selected_offset = -1;
        }
    }
    igSameLine();
    igText("Jump (hex)");
    igSameLine();
    if (igButton("<<")) { g_memview.base_addr -= MEMVIEW_COLS * MEMVIEW_ROWS * 4; g_memview.selected_offset = -1; }
    igSameLine();
    if (igButton("<"))  { g_memview.base_addr -= MEMVIEW_COLS * MEMVIEW_ROWS;     g_memview.selected_offset = -1; }
    igSameLine();
    if (igButton(">"))  { g_memview.base_addr += MEMVIEW_COLS * MEMVIEW_ROWS;     g_memview.selected_offset = -1; }
    igSameLine();
    if (igButton(">>")) { g_memview.base_addr += MEMVIEW_COLS * MEMVIEW_ROWS * 4; g_memview.selected_offset = -1; }

    mem_region_t *r = mem_find_region(bus, g_memview.base_addr);
    igSameLineEx(0, 20);
    if (r) igText("region: %s [%08X+%X]", r->name, r->base, r->size);
    else   igText("region: <unmapped>");

    if (g_memview.selected_offset >= 0) {
        uint32_t addr = g_memview.base_addr + (uint32_t)g_memview.selected_offset;
        uint8_t  b   = (uint8_t)mem_read8(bus, addr);
        uint32_t u32 = mem_read32(bus, addr);
        float    f;  memcpy(&f, &u32, 4);

        igText("Addr 0x%08X   byte 0x%02X (%d)", addr, b, b);

        igSameLine();
        if (igSmallButton("edit byte")) {
            g_memview.editing = true;
            g_memview.u32_editing = false;
            g_memview.float_editing = false;
            snprintf(g_memview.edit_buf, sizeof(g_memview.edit_buf), "%02X", b);
        }
        if (g_memview.editing) {
            igSameLine();
            igSetNextItemWidth(48);
            if (igInputText("##eb", g_memview.edit_buf, sizeof(g_memview.edit_buf),
                            ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
                unsigned nv = 0;
                if (sscanf(g_memview.edit_buf, "%x", &nv) == 1) {
                    mem_write8(bus, addr, (uint8_t)(nv & 0xFF));
                    LOG_INFO("memview: wrote byte 0x%02X to 0x%08X", nv & 0xFF, addr);
                }
                g_memview.editing = false;
            }
        }

        igText("U32  0x%08X (%u)", u32, u32);
        igSameLine();
        if (igSmallButton("edit u32")) {
            g_memview.u32_editing = !g_memview.u32_editing;
            g_memview.float_editing = false;
            snprintf(g_memview.u32_buf, sizeof(g_memview.u32_buf), "%08X", u32);
        }
        if (g_memview.u32_editing) {
            igSameLine();
            igSetNextItemWidth(100);
            if (igInputText("##eu32", g_memview.u32_buf, sizeof(g_memview.u32_buf),
                            ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
                unsigned nv = 0;
                if (sscanf(g_memview.u32_buf, "%x", &nv) == 1) {
                    mem_write32(bus, addr, nv);
                    LOG_INFO("memview: wrote u32 0x%08X to 0x%08X", nv, addr);
                }
                g_memview.u32_editing = false;
            }
        }

        igText("f32  %g", f);
        igSameLine();
        if (igSmallButton("edit f32")) {
            g_memview.float_editing = !g_memview.float_editing;
            g_memview.u32_editing = false;
            snprintf(g_memview.float_buf, sizeof(g_memview.float_buf), "%g", f);
        }
        if (g_memview.float_editing) {
            igSameLine();
            igSetNextItemWidth(140);
            if (igInputText("##ef", g_memview.float_buf, sizeof(g_memview.float_buf),
                            ImGuiInputTextFlags_EnterReturnsTrue)) {
                float nf = 0;
                if (sscanf(g_memview.float_buf, "%f", &nf) == 1) {
                    uint32_t nv; memcpy(&nv, &nf, 4);
                    mem_write32(bus, addr, nv);
                    LOG_INFO("memview: wrote f32 %g (0x%08X) to 0x%08X", nf, nv, addr);
                }
                g_memview.float_editing = false;
            }
        }
    } else {
        igTextDisabled("click a byte to select");
    }

    igSeparator();

    igPushStyleColorImVec4(ImGuiCol_Text, (ImVec4){0.6f, 0.6f, 0.6f, 1.0f});
    igText("         ");
    for (int col = 0; col < MEMVIEW_COLS; col++) {
        igSameLine();
        igText("%02X", col);
    }
    igSameLine();
    igText("  ASCII");
    igPopStyleColor();

    igBeginChild("##scroll", (ImVec2){0, 0}, 0, ImGuiWindowFlags_HorizontalScrollbar);

    for (int row = 0; row < MEMVIEW_ROWS; row++) {
        uint32_t row_addr = g_memview.base_addr + (uint32_t)(row * MEMVIEW_COLS);
        char ascii[MEMVIEW_COLS + 1];

        igPushStyleColorImVec4(ImGuiCol_Text, (ImVec4){0.5f, 0.8f, 1.0f, 1.0f});
        igText("%08X:", row_addr);
        igPopStyleColor();

        for (int col = 0; col < MEMVIEW_COLS; col++) {
            int offset = row * MEMVIEW_COLS + col;
            uint32_t addr = g_memview.base_addr + (uint32_t)offset;
            uint8_t  val  = (uint8_t)mem_read8(bus, addr);
            ascii[col] = (val >= 0x20 && val < 0x7F) ? (char)val : '.';

            igSameLine();

            ImVec4 c;
            if (offset == g_memview.selected_offset) c = (ImVec4){1.0f, 1.0f, 0.2f, 1.0f};
            else if (val != 0)                       c = (ImVec4){0.9f, 0.9f, 0.9f, 1.0f};
            else                                     c = (ImVec4){0.35f, 0.35f, 0.35f, 1.0f};

            igPushStyleColorImVec4(ImGuiCol_Text, c);
            igText("%02X", val);
            igPopStyleColor();

            if (igIsItemClicked()) {
                g_memview.selected_offset = offset;
                g_memview.editing = false;
                g_memview.u32_editing = false;
                g_memview.float_editing = false;
            }
        }

        ascii[MEMVIEW_COLS] = 0;
        igSameLine();
        igPushStyleColorImVec4(ImGuiCol_Text, (ImVec4){0.5f, 0.9f, 0.5f, 1.0f});
        igText("  %s", ascii);
        igPopStyleColor();
    }

    igEndChild();
    igEnd();
}

#endif /* MEMVIEW_H */
