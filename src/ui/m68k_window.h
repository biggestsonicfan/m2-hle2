/*
 * m68k_window.h — MC68000 register inspector for the sound CPU.
 *
 * Shows D0-D7, A0-A7, PC, and SR with decoded flag bits.
 * Changed-since-last-snapshot cells render in yellow (same style as
 * cpu_window.h for the i960).
 */
#ifndef M68K_WINDOW_H
#define M68K_WINDOW_H

#include <stdbool.h>
#include "m68k.h"
#include "cimgui.h"

static inline void m68k_window_reg_row(const char *name, uint32_t v, uint32_t pv) {
    igTableNextRow();
    igTableNextColumn(); igText("%s", name);
    igTableNextColumn();
    if (v != pv) {
        igPushStyleColorImVec4(ImGuiCol_Text, (ImVec4){1.f, 1.f, 0.f, 1.f});
        igText("0x%08X", v);
        igPopStyleColor();
    } else {
        igText("0x%08X", v);
    }
}

/* Status banner */
static inline void m68k_window_banner(const m68k_cpu_t *c) {
    if (c->halted) {
        igPushStyleColorImVec4(ImGuiCol_Text, (ImVec4){1.f, .3f, .3f, 1.f});
        igText("HALTED");
        igPopStyleColor();
    } else if (c->stopped) {
        igPushStyleColorImVec4(ImGuiCol_Text, (ImVec4){1.f, .8f, .2f, 1.f});
        igText("STOPPED (waiting interrupt)");
        igPopStyleColor();
    } else {
        igPushStyleColorImVec4(ImGuiCol_Text, (ImVec4){.3f, 1.f, .3f, 1.f});
        igText("RUNNING");
        igPopStyleColor();
    }
}

/* SR: show raw value + decoded flags */
static inline void m68k_window_sr_row(uint16_t sr, uint16_t psr) {
    igTableNextRow();
    igTableNextColumn(); igText("SR");
    igTableNextColumn();
    const char *T = (sr & M68K_SR_T1) ? "T" : "-";
    const char *S = (sr & M68K_SR_S)  ? "S" : "U";
    int   ipl = (sr >> M68K_SR_IPL_SHIFT) & 7;
    const char *X = (sr & M68K_SR_X) ? "X" : "-";
    const char *N = (sr & M68K_SR_N) ? "N" : "-";
    const char *Z = (sr & M68K_SR_Z) ? "Z" : "-";
    const char *V = (sr & M68K_SR_V) ? "V" : "-";
    const char *C = (sr & M68K_SR_C) ? "C" : "-";
    if (sr != psr) {
        igPushStyleColorImVec4(ImGuiCol_Text, (ImVec4){1.f, 1.f, 0.f, 1.f});
        igText("0x%04X  %s%s%d%s%s%s%s%s", sr, T, S, ipl, X, N, Z, V, C);
        igPopStyleColor();
    } else {
        igText("0x%04X  %s%s%d%s%s%s%s%s", sr, T, S, ipl, X, N, Z, V, C);
    }
}

/* PC and SR */
static inline void m68k_window_status(const m68k_cpu_t *c, const m68k_cpu_t *cp) {
    if (igCollapsingHeaderBoolPtr("Status", NULL, ImGuiTreeNodeFlags_DefaultOpen)) {
        if (igBeginTable("sr_tbl", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            igTableSetupColumn("Register", 0); igTableSetupColumn("Value", 0);
            igTableHeadersRow();
            m68k_window_reg_row("PC", c->pc, cp->pc);
            m68k_window_sr_row(c->sr, cp->sr);
            igEndTable();
        }
    }
}

/* Data registers D0–D7 */
static inline void m68k_window_data_regs(const m68k_cpu_t *c, const m68k_cpu_t *cp) {
    if (igCollapsingHeaderBoolPtr("Data registers (D0-D7)", NULL, ImGuiTreeNodeFlags_DefaultOpen)) {
        if (igBeginTable("d_tbl", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            igTableSetupColumn("Reg", 0); igTableSetupColumn("Value", 0);
            igTableHeadersRow();
            for (int i = 0; i < 8; i++) {
                char name[4]; name[0]='D'; name[1]='0'+i; name[2]=0;
                m68k_window_reg_row(name, c->d[i], cp->d[i]);
            }
            igEndTable();
        }
    }
}

/* Address registers A0–A7 */
static inline void m68k_window_addr_regs(const m68k_cpu_t *c, const m68k_cpu_t *cp) {
    if (igCollapsingHeaderBoolPtr("Address registers (A0-A7)", NULL, ImGuiTreeNodeFlags_DefaultOpen)) {
        if (igBeginTable("a_tbl", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            igTableSetupColumn("Reg", 0); igTableSetupColumn("Value", 0);
            igTableHeadersRow();
            for (int i = 0; i < 7; i++) {
                char name[4]; name[0]='A'; name[1]='0'+i; name[2]=0;
                m68k_window_reg_row(name, c->a[i], cp->a[i]);
            }
            /* A7 is the active SP; show USP/SSP bank too */
            m68k_window_reg_row("A7(SP)", c->a[7], cp->a[7]);
            m68k_window_reg_row("USP",   c->usp,   cp->usp);
            m68k_window_reg_row("SSP",   c->ssp,   cp->ssp);
            igEndTable();
        }
    }
}

static inline void m68k_window_draw(const m68k_state_t *s,
                                    const m68k_state_t *prev,
                                    bool *p_open)
{
    const m68k_cpu_t *c  = &s->cpu;
    const m68k_cpu_t *cp = &prev->cpu;

    igSetNextWindowSize((ImVec2){380, 520}, ImGuiCond_FirstUseEver);
    if (!igBegin("68K Sound CPU", p_open, 0)) { igEnd(); return; }

    m68k_window_banner(c);
    igSeparator();

    m68k_window_status(c, cp);
    m68k_window_data_regs(c, cp);
    m68k_window_addr_regs(c, cp);

    igText("Cycles: %llu", (unsigned long long)c->cycles);
    igEnd();
}

#endif /* M68K_WINDOW_H */
