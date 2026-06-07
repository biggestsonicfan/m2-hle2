/*
 * cop_window.h — COP/SHARC diagnostics window.
 *
 * Shows activity counters, the break-on-unknown trigger state, and a
 * deduplicated table of every unknown command seen this session with its
 * first call site and hit count.  Open via Debug → COP diagnostics.
 */
#ifndef COP_WINDOW_H
#define COP_WINDOW_H

#include <stdbool.h>
#include <stdio.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"

#include "cop.h"

static inline void cop_window_draw(bool *p_open) {
    igSetNextWindowSize((ImVec2){520, 420}, ImGuiCond_FirstUseEver);
    if (!igBegin("COP Diagnostics", p_open, 0)) { igEnd(); return; }

    /* Activity counters */
    igText("Writes:        %u", g_cop.writes);
    igText("Reads:         %u", g_cop.reads);
    igText("Transforms:    %u  (0x14802929)", g_sharc.transform_count);
    igText("Matrix reads:  %u  (0x02800505)", g_sharc.matrix_read_count);
    igText("Unknown cmds:  %u  (%d unique)",  g_sharc.unknown_cmds,
                                               g_sharc.unknown_log_count);

    igSeparator();

    /* Break-on-unknown controls */
    igCheckbox("Break on unknown cmd", (bool *)&g_sharc.break_on_unknown);
    if (g_sharc.unknown_triggered) {
        igSameLine();
        igTextColored((ImVec4){1.0f, 0.4f, 0.4f, 1.0f}, "TRIGGERED!");
        igText("  cmd=0x%08X  IP=0x%08X",
               g_sharc.unknown_trigger_cmd, g_sharc.unknown_trigger_ip);
        igSameLine();
        if (igButton("Clear")) g_sharc.unknown_triggered = 0;
    }

    igSeparator();

    /* Unknown command table */
    igText("Unknown commands seen this session (%d / %d):",
           g_sharc.unknown_log_count, SHARC_UNKNOWN_LOG_MAX);

    ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                       | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (igBeginTable("##unknown", 3, tf)) {
        igTableSetupScrollFreeze(0, 1);
        igTableSetupColumnEx("Command",   ImGuiTableColumnFlags_WidthFixed,   100.0f, 0);
        igTableSetupColumnEx("First IP",  ImGuiTableColumnFlags_WidthFixed,   100.0f, 0);
        igTableSetupColumnEx("Hit count", ImGuiTableColumnFlags_WidthFixed,    80.0f, 0);
        igTableHeadersRow();

        for (int i = 0; i < g_sharc.unknown_log_count; i++) {
            igTableNextRow();
            igTableSetColumnIndex(0);
            char buf[16];
            snprintf(buf, sizeof(buf), "0x%08X", g_sharc.unknown_log[i].cmd);
            igTextUnformatted(buf);
            igTableSetColumnIndex(1);
            snprintf(buf, sizeof(buf), "0x%08X", g_sharc.unknown_log[i].first_ip);
            igTextUnformatted(buf);
            igTableSetColumnIndex(2);
            snprintf(buf, sizeof(buf), "%u", g_sharc.unknown_log[i].count);
            igTextUnformatted(buf);
        }
        igEndTable();
    }

    igEnd();
}

#endif /* COP_WINDOW_H */
