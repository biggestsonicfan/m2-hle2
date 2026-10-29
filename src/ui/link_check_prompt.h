/*
 * link_check_prompt.h — the desktop's question when a linked game set to
 * MASTER finds no other cabinet on the link (core/link_check.h, Pinboard
 * #1449). The game is paused while it is up and runs again on the answer, if
 * it was running. The virtual TOSlink cable is offered here and nowhere else.
 */
#ifndef UI_LINK_CHECK_PROMPT_H
#define UI_LINK_CHECK_PROMPT_H

#include "cimgui.h"
#include "emu_thread.h"
#include "link_check.h"

static inline void link_check_prompt(emu_thread_ctx_t *emu) {
    static bool showing, resume;
    link_check_frame(g_emu_frames);
    if (!showing) {
        if (!link_check_due()) return;
        showing = true;
        resume  = emu_is_running(emu);
        if (resume) emu_stop(emu);
        igOpenPopup("##link_check", 0);
    }
    const ImGuiViewport *vp = igGetMainViewport();
    igSetNextWindowPosEx((ImVec2){ vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                   vp->WorkPos.y + vp->WorkSize.y * 0.5f },
                         ImGuiCond_Always, (ImVec2){ 0.5f, 0.5f });
    if (!igBeginPopupModal("##link_check", NULL,
                           ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoSavedSettings)) {
        igOpenPopup("##link_check", 0);   /* closed by something else: keep asking */
        return;
    }
    igTextColored((ImVec4){ 1.0f, 0.8f, 0.3f, 1.0f }, LINK_CHECK_TITLE);
    igSeparator();
    igTextUnformatted(link_check_text());
    igSpacing();
    int pick = -1;
    if (link_check_has_test(g_active_profile)) {
        if (igButton(LINK_ANSWER_LABEL[LINK_ANSWER_TEST_MENU])) pick = LINK_ANSWER_TEST_MENU;
        igSameLine();
    }
    if (igButton(LINK_ANSWER_LABEL[LINK_ANSWER_CONTINUE])) pick = LINK_ANSWER_CONTINUE;
    if (link_check_has_cable(g_active_profile)) {
        igSameLine();
        if (igButton(LINK_ANSWER_LABEL[LINK_ANSWER_CABLE])) pick = LINK_ANSWER_CABLE;
        if (igIsItemHovered(0))
            igSetTooltip("Wires this comm board's output to its own input: a link of one\n"
                         "cabinet, so the MASTER finds itself and the game goes on.\n"
                         "Until another game is loaded.");
    }
    if (pick >= 0) {
        link_check_answer((link_answer_t)pick, g_emu_frames);
        igCloseCurrentPopup();
        showing = false;
        if (resume) emu_run(emu);
    }
    igEndPopup();
}

#endif /* UI_LINK_CHECK_PROMPT_H */
