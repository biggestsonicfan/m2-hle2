/*
 * breakpoint_window.h — breakpoint list + add/remove UI.
 */
#ifndef BREAKPOINT_WINDOW_H
#define BREAKPOINT_WINDOW_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "cimgui.h"
#include "breakpoint.h"

static inline void bp_window_draw(bool *p_open) {
    igSetNextWindowSize((ImVec2){420, 300}, ImGuiCond_FirstUseEver);
    if (!igBegin("Breakpoints", p_open, 0)) { igEnd(); return; }

    static char addr_buf[16] = "";
    static char label_buf[64] = "";

    igSetNextItemWidth(110);
    igInputText("##addr", addr_buf, sizeof(addr_buf), ImGuiInputTextFlags_CharsHexadecimal);
    igSameLine();
    igSetNextItemWidth(160);
    igInputText("##label", label_buf, sizeof(label_buf), 0);
    igSameLine();
    if (igButton("Add")) {
        uint32_t addr = 0;
        if (sscanf(addr_buf, "%x", &addr) == 1) {
            bp_add(addr, label_buf[0] ? label_buf : NULL);
            addr_buf[0] = 0;
            label_buf[0] = 0;
        }
    }

    igSeparator();

    if (igBeginTable("bp_table", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        igTableSetupColumn("On", 0);
        igTableSetupColumn("Address", 0);
        igTableSetupColumn("Label", 0);
        igTableSetupColumn("", 0);
        igTableHeadersRow();

        for (int i = 0; i < BP_MAX; i++) {
            if (!g_bp.list[i].active) continue;

            igTableNextRow();

            igTableNextColumn();
            char chk_id[16];
            snprintf(chk_id, sizeof(chk_id), "##bp_%d", i);
            igCheckbox(chk_id, &g_bp.list[i].enabled);

            igTableNextColumn();
            if (g_bp.hit_addr == g_bp.list[i].addr && g_bp.hit) {
                igPushStyleColorImVec4(ImGuiCol_Text, (ImVec4){1.0f, 0.35f, 0.35f, 1.0f});
                igText("0x%08X", g_bp.list[i].addr);
                igPopStyleColor();
            } else {
                igText("0x%08X", g_bp.list[i].addr);
            }

            igTableNextColumn();
            igText("%s", g_bp.list[i].label);

            igTableNextColumn();
            char del_id[16];
            snprintf(del_id, sizeof(del_id), "X##d_%d", i);
            if (igButton(del_id)) bp_remove(i);
        }

        igEndTable();
    }

    igEnd();
}

#endif /* BREAKPOINT_WINDOW_H */
