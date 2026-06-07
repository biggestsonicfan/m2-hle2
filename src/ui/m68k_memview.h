/*
 * m68k_memview.h — hex memory inspector for the 68K sound CPU address space.
 *
 * Reads through sound_m68k_read so every region (wave RAM, SCSP regs,
 * program ROM, sample ROM) is accessible at its 68K-side address.
 * Writes go through sound_m68k_write (safe for wave RAM; SCSP register
 * writes will trigger key-on/off intercepts as normal).
 */
#ifndef M68K_MEMVIEW_H
#define M68K_MEMVIEW_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cimgui.h"
#include "sound.h"
#include "constants.h"

#define M68K_MEMVIEW_COLS 16
#define M68K_MEMVIEW_ROWS 16

typedef struct {
    uint32_t base_addr;
    char     jump_buf[16];
    int      selected_offset;
    bool     editing;
    char     edit_buf[4];
} m68k_memview_state_t;

static m68k_memview_state_t g_m68k_memview = { .base_addr = 0, .selected_offset = -1 };

static inline const char *m68k_region_name(uint32_t addr) {
    if (addr < M68K_WAVE_SIZE)                                        return "wave RAM";
    if (addr >= M68K_SCSP_BASE   && addr < M68K_SCSP_BASE   + M68K_SCSP_SIZE)   return "SCSP regs";
    if (addr >= M68K_SNDCTL_BASE && addr < M68K_SNDCTL_BASE + M68K_SNDCTL_SIZE) return "sound ctrl";
    if (addr >= M68K_ROM_BASE    && addr < M68K_ROM_BASE    + M68K_ROM_SIZE)     return "program ROM";
    if (addr >= M68K_SAMPLE_BASE && addr < M68K_SAMPLE_BASE + M68K_SAMPLE_SIZE)  return "sample ROM";
    return "<unmapped>";
}

static inline void m68k_memview_draw(bool *p_open) {
    igSetNextWindowSize((ImVec2){660, 460}, ImGuiCond_FirstUseEver);
    if (!igBegin("68K memory viewer", p_open, 0)) { igEnd(); return; }

    /* Jump bar */
    igSetNextItemWidth(110);
    if (igInputText("##jmp", g_m68k_memview.jump_buf, sizeof(g_m68k_memview.jump_buf),
                    ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
        uint32_t addr = 0;
        if (sscanf(g_m68k_memview.jump_buf, "%x", &addr) == 1) {
            g_m68k_memview.base_addr    = addr & ~0xFu;
            g_m68k_memview.selected_offset = -1;
        }
    }
    igSameLine();
    igText("Jump (hex)");
    igSameLine();
    if (igButton("<<")) { g_m68k_memview.base_addr -= M68K_MEMVIEW_COLS * M68K_MEMVIEW_ROWS * 4; g_m68k_memview.selected_offset = -1; }
    igSameLine();
    if (igButton("<"))  { g_m68k_memview.base_addr -= M68K_MEMVIEW_COLS * M68K_MEMVIEW_ROWS;     g_m68k_memview.selected_offset = -1; }
    igSameLine();
    if (igButton(">"))  { g_m68k_memview.base_addr += M68K_MEMVIEW_COLS * M68K_MEMVIEW_ROWS;     g_m68k_memview.selected_offset = -1; }
    igSameLine();
    if (igButton(">>")) { g_m68k_memview.base_addr += M68K_MEMVIEW_COLS * M68K_MEMVIEW_ROWS * 4; g_m68k_memview.selected_offset = -1; }

    igSameLineEx(0, 20);
    igText("region: %s", m68k_region_name(g_m68k_memview.base_addr));

    /* Quick-jump buttons for each region */
    if (igSmallButton("wave"))   { g_m68k_memview.base_addr = M68K_WAVE_BASE;   g_m68k_memview.selected_offset = -1; }
    igSameLine();
    if (igSmallButton("SCSP"))   { g_m68k_memview.base_addr = M68K_SCSP_BASE;   g_m68k_memview.selected_offset = -1; }
    igSameLine();
    if (igSmallButton("ROM"))    { g_m68k_memview.base_addr = M68K_ROM_BASE;     g_m68k_memview.selected_offset = -1; }
    igSameLine();
    if (igSmallButton("sample")) { g_m68k_memview.base_addr = M68K_SAMPLE_BASE;  g_m68k_memview.selected_offset = -1; }
    igSameLine();
    if (igSmallButton("vec"))    { g_m68k_memview.base_addr = 0x60u & ~0xFu;     g_m68k_memview.selected_offset = -1; } /* IRQ vectors at 0x60+ */

    /* Selected byte info + edit */
    if (g_m68k_memview.selected_offset >= 0) {
        uint32_t addr = g_m68k_memview.base_addr + (uint32_t)g_m68k_memview.selected_offset;
        uint8_t  b    = (uint8_t)sound_m68k_read(&g_sound, addr, 1);
        uint32_t u32  = sound_m68k_read(&g_sound, addr & ~3u, 4);

        igText("Addr 0x%06X   byte 0x%02X (%3d)   u32 0x%08X", addr, b, b, u32);
        igSameLine();
        if (igSmallButton("edit byte")) {
            g_m68k_memview.editing = true;
            snprintf(g_m68k_memview.edit_buf, sizeof(g_m68k_memview.edit_buf), "%02X", b);
        }
        if (g_m68k_memview.editing) {
            igSameLine();
            igSetNextItemWidth(48);
            if (igInputText("##eb", g_m68k_memview.edit_buf, sizeof(g_m68k_memview.edit_buf),
                            ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
                unsigned nv = 0;
                if (sscanf(g_m68k_memview.edit_buf, "%x", &nv) == 1)
                    sound_m68k_write(&g_sound, addr, nv & 0xFF, 1);
                g_m68k_memview.editing = false;
            }
        }
    } else {
        igTextDisabled("click a byte to select");
    }

    igSeparator();

    /* Column header */
    igPushStyleColorImVec4(ImGuiCol_Text, (ImVec4){0.6f, 0.6f, 0.6f, 1.0f});
    igText("         ");
    for (int col = 0; col < M68K_MEMVIEW_COLS; col++) { igSameLine(); igText("%02X", col); }
    igSameLine();
    igText("  ASCII");
    igPopStyleColor();

    igBeginChild("##scroll", (ImVec2){0, 0}, 0, ImGuiWindowFlags_HorizontalScrollbar);

    for (int row = 0; row < M68K_MEMVIEW_ROWS; row++) {
        uint32_t row_addr = g_m68k_memview.base_addr + (uint32_t)(row * M68K_MEMVIEW_COLS);
        char ascii[M68K_MEMVIEW_COLS + 1];

        igPushStyleColorImVec4(ImGuiCol_Text, (ImVec4){0.5f, 0.8f, 1.0f, 1.0f});
        igText("%06X:", row_addr);
        igPopStyleColor();

        for (int col = 0; col < M68K_MEMVIEW_COLS; col++) {
            int      offset = row * M68K_MEMVIEW_COLS + col;
            uint32_t addr   = g_m68k_memview.base_addr + (uint32_t)offset;
            uint8_t  val    = (uint8_t)sound_m68k_read(&g_sound, addr, 1);
            ascii[col] = (val >= 0x20 && val < 0x7F) ? (char)val : '.';

            igSameLine();

            ImVec4 c;
            if (offset == g_m68k_memview.selected_offset) c = (ImVec4){1.0f, 1.0f, 0.2f, 1.0f};
            else if (val != 0)                             c = (ImVec4){0.9f, 0.9f, 0.9f, 1.0f};
            else                                           c = (ImVec4){0.35f, 0.35f, 0.35f, 1.0f};

            igPushStyleColorImVec4(ImGuiCol_Text, c);
            igText("%02X", val);
            igPopStyleColor();

            if (igIsItemClicked()) {
                g_m68k_memview.selected_offset = offset;
                g_m68k_memview.editing         = false;
            }
        }

        ascii[M68K_MEMVIEW_COLS] = 0;
        igSameLine();
        igPushStyleColorImVec4(ImGuiCol_Text, (ImVec4){0.5f, 0.9f, 0.5f, 1.0f});
        igText("  %s", ascii);
        igPopStyleColor();
    }

    igEndChild();
    igEnd();
}

#endif /* M68K_MEMVIEW_H */
