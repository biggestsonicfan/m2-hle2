/*
 * ps3ui_ggpo.h -- the "go again?" prompt for a GGPO match (Pinboard #579).
 *
 * An RPCN match asks after every result (ps3ui_app.h, PS3UI_SCR_AGAIN); a
 * match from the GGPO lobby asks the same, in the same window. A lobby match
 * plays in VS mode (emu_ggpo_start), so a decided match goes back to
 * character select on both boards: Play again (or the timer) carries on
 * there, Exit ends the match for both players (GGL_CMD_END tells the lobby,
 * which tells the other end).
 *
 * The result is the board's, counted by emu_ggpo.h once no rollback can take
 * it back, so both machines ask about the same match.
 *
 * The frontend calls ps3ui_ggpo_prompt once a frame after the session's tick,
 * on the thread that runs it, and gives the game none of the pad while it
 * says the prompt is up.
 */
#ifndef PS3UI_GGPO_H
#define PS3UI_GGPO_H

#include "ps3ui_app.h"

#ifdef M2HLE_GGPO

/* True while the prompt is on screen and has the pad. */
static bool ps3ui_ggpo_prompt(ps3ui_app_t *a, uint32_t pad)
{
    static uint32_t seen;
    ggl_status_t st = ggl_status();
    if (st.stage != GGL_MATCH) {
        ps3ui_app_ext_cancel(a);        /* the match ended under it */
        seen = 0;
        return false;
    }
    int winner = 0;
    uint32_t n = emu_ggpo_results(&winner);
    if (n != seen) {
        seen = n;
        /* not over the RPCN lobby's own screens, should both be open */
        if (n && winner && (!a->open || a->ext)) {
            int me = st.side == 0 ? 0 : 1;
            const char *name[2];
            name[me] = st.user;
            name[1 - me] = st.opponent;
            ps3ui_app_ask_ext(a, winner - 1, name[0], name[1]);
        }
    }
    if (!a->ext)
        return false;
    ps3ui_app_frame(a, pad);
    if (ps3ui_app_ext_answer(a) == 1)
        ggl_post(GGL_CMD_END, "", "");
    return true;
}

#else

static bool ps3ui_ggpo_prompt(ps3ui_app_t *a, uint32_t pad) { (void)a; (void)pad; return false; }

#endif

#endif
