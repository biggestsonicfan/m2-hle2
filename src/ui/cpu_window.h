/*
 * cpu_window.h — i960 register inspector.
 *
 * SFRs, globals (g0..g15/fp), locals (pfp/sp/rip/r3..r15), frame-stack depth.
 * Changed-since-last-snapshot register cells render in yellow.
 */
#ifndef CPU_WINDOW_H
#define CPU_WINDOW_H

#include <stdbool.h>

#include "i960.h"
#include "cimgui.h"

static const char *cpu_local_reg_names[] = {
    "pfp", "sp", "rip", "r3", "r4", "r5", "r6", "r7",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
};

static const char *cpu_global_reg_names[] = {
    "g0", "g1", "g2",  "g3",  "g4",  "g5",  "g6",  "g7",
    "g8", "g9", "g10", "g11", "g12", "g13", "g14", "fp"
};

static inline void cpu_window_reg_row(const char *name, uint32_t value, uint32_t prev_value) {
    igTableNextRow();
    igTableNextColumn();
    igText("%s", name);
    igTableNextColumn();
    if (value != prev_value) {
        igPushStyleColorImVec4(ImGuiCol_Text, (ImVec4){1.0f, 1.0f, 0.0f, 1.0f});
        igText("0x%08X", value);
        igPopStyleColor();
    } else {
        igText("0x%08X", value);
    }
}

static inline void cpu_window_draw(const i960_cpu_t *cpu, const i960_cpu_t *prev, bool *p_open) {
    igSetNextWindowSize((ImVec2){360, 480}, ImGuiCond_FirstUseEver);
    if (!igBegin("CPU registers", p_open, 0)) { igEnd(); return; }

    if (cpu->halted) {
        igPushStyleColorImVec4(ImGuiCol_Text, (ImVec4){1.0f, 0.3f, 0.3f, 1.0f});
        igText("HALTED");
        igPopStyleColor();
    } else {
        igPushStyleColorImVec4(ImGuiCol_Text, (ImVec4){0.3f, 1.0f, 0.3f, 1.0f});
        igText("RUNNING");
        igPopStyleColor();
    }
    igSeparator();

    if (igCollapsingHeaderBoolPtr("Special registers", NULL, ImGuiTreeNodeFlags_DefaultOpen)) {
        if (igBeginTable("sfr_table", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            igTableSetupColumn("Register", 0);
            igTableSetupColumn("Value", 0);
            igTableHeadersRow();
            cpu_window_reg_row("IP", cpu->sfr.ip, prev->sfr.ip);
            cpu_window_reg_row("AC", cpu->sfr.ac, prev->sfr.ac);
            cpu_window_reg_row("PC", cpu->sfr.pc, prev->sfr.pc);
            cpu_window_reg_row("TC", cpu->sfr.tc, prev->sfr.tc);
            igEndTable();
        }
    }

    if (igCollapsingHeaderBoolPtr("Global registers (g0..fp)", NULL, ImGuiTreeNodeFlags_DefaultOpen)) {
        if (igBeginTable("global_table", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            igTableSetupColumn("Register", 0);
            igTableSetupColumn("Value", 0);
            igTableHeadersRow();
            for (int i = 0; i < 16; i++) {
                cpu_window_reg_row(cpu_global_reg_names[i], cpu->globals.g[i], prev->globals.g[i]);
            }
            igEndTable();
        }
    }

    if (igCollapsingHeaderBoolPtr("Local registers (pfp..r15)", NULL, ImGuiTreeNodeFlags_DefaultOpen)) {
        if (igBeginTable("local_table", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            igTableSetupColumn("Register", 0);
            igTableSetupColumn("Value", 0);
            igTableHeadersRow();
            for (int i = 0; i < 16; i++) {
                cpu_window_reg_row(cpu_local_reg_names[i], cpu->locals.r[i], prev->locals.r[i]);
            }
            igEndTable();
        }
    }

    if (igCollapsingHeader("Frame stack", 0)) {
        igText("depth: %d / %d", cpu->frame_depth, FRAME_STACK_DEPTH);
    }

    igEnd();
}

#endif /* CPU_WINDOW_H */
