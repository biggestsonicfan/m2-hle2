/*
 * link_check_pad.h — the question of core/link_check.h (Pinboard #1449) for a
 * frontend with only a pad and the PS3 menus' canvas: libretro. A box over the
 * paused game: up and down pick, cross (RetroPad B) or Start answers. The
 * virtual TOSlink cable is offered here and nowhere else, as on the desktop.
 *
 * Once a frame, on the thread that runs the board: link_check_pad_frame with
 * the pad in PS3UI_PAD_* bits (link_ask_frame says what its answer means).
 * link_check_pad_draw records the box on the canvas over the game.
 */
#ifndef UI_LINK_CHECK_PAD_H
#define UI_LINK_CHECK_PAD_H

#include "link_check.h"
#include "ps3ui_app.h"

#define LINK_PAD_ANSWER (PS3UI_PAD_CROSS | PS3UI_PAD_START)

/* The pad, in PS3UI_PAD_* bits, into link_ask_frame's. */
static inline uint32_t link_pad__keys(uint32_t pad) {
    return (pad & PS3UI_PAD_UP ? LINK_KEY_UP : 0u) | (pad & PS3UI_PAD_DOWN ? LINK_KEY_DOWN : 0u) |
           (pad & LINK_PAD_ANSWER ? LINK_KEY_ANSWER : 0u);
}

static inline link_ask_state_t link_check_pad_frame(uint32_t pad, uint32_t board_frame) {
    static uint32_t pad_was;
    uint32_t hit = pad & ~pad_was;
    pad_was = pad;
    return link_ask_frame(link_pad__keys(pad), link_pad__keys(hit), board_frame);
}

/* The box, in the canvas's 1920x1080 layout. True when it drew. */
static inline bool link_check_pad_draw(ps3ui_canvas_t *cv) {
    const link_ask_t *lp = &g_link_ask;
    if (lp->state != LINK_ASK_ASKING) return false;
    float black[3] = { 0.0f, 0.0f, 0.0f };
    ps3ui_fill_px(cv, 0, 0, cv->w, cv->h, black, 0.6f, PS3UI_BLEND_NORMAL);
    const ps3ui_box_t win = { 260.0f, 200.0f, 1660.0f, 880.0f };
    ps3ui_fill_rect(cv, win, 0x0B1A33, 0.95f, PS3UI_BLEND_NORMAL);
    ps3ui_fill_rect(cv, (ps3ui_box_t){ win.x0, win.y0, win.x1, win.y0 + 4.0f }, 0x4DD2FF, 1.0f, PS3UI_BLEND_NORMAL);

    ps3ui_text_style_t title = ps3ui_style_text(40.0f);
    title.rgb = 0xFFCC4D;
    ps3ui_text_left(cv, &title, win.x0 + 50.0f, win.y0 + 30.0f, LINK_CHECK_TITLE, 1.0f);
    ps3ui_text_style_t body = ps3ui_style_text(30.0f);
    ps3ui_text_box(cv, &body, (ps3ui_box_t){ win.x0 + 50.0f, win.y0 + 110.0f, win.x1 - 50.0f, win.y0 + 400.0f },
                   link_check_text(), 1.0f, 0);

    link_answer_t rows[3];
    int n = link_ask_rows(rows);
    float y = win.y1 - 70.0f - 56.0f * (float)n;
    ps3ui_text_style_t row = ps3ui_style_text(34.0f);
    for (int i = 0; i < n; i++, y += 56.0f) {
        bool on = i == lp->sel;
        if (on) ps3ui_fill_rect(cv, (ps3ui_box_t){ win.x0 + 40.0f, y - 6.0f, win.x0 + 760.0f, y + 50.0f },
                                0x4DD2FF, 0.25f, PS3UI_BLEND_NORMAL);
        row.rgb = on ? 0xFFFFFF : 0xA0B4C8;
        ps3ui_text_left(cv, &row, win.x0 + 70.0f, y, LINK_ANSWER_LABEL[rows[i]], 1.0f);
    }
    ps3ui_text_style_t hint = ps3ui_style_text(26.0f);
    hint.rgb = 0xA0B4C8;
    if (rows[lp->sel] == LINK_ANSWER_CABLE)
        ps3ui_text_box(cv, &hint, (ps3ui_box_t){ win.x0 + 800.0f, y - 56.0f * (float)n, win.x1 - 50.0f, y },
                       LINK_CHECK_CABLE_HINT, 1.0f, 0);
    ps3ui_text_left(cv, &hint, win.x0 + 70.0f, win.y1 - 56.0f, "Up / Down: choose     B or Start: answer", 1.0f);
    return true;
}

#endif /* UI_LINK_CHECK_PAD_H */
