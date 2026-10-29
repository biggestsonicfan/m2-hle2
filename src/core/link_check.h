/*
 * link_check.h — a linked game set to MASTER with no other cabinet on the link
 * (Pinboard #1449).
 *
 * Daytona USA, Virtual-On, Power Sled and the other linked sets keep their
 * cabinet's role on the link in the test menu (Daytona: GAME SYSTEM, LINK ID;
 * Virtual-On: GAME ASSIGNMENTS, NETWORK LINK ATTRIBUTE). A board set to MASTER
 * starts its comm board as the master and waits for the other cabinets; alone,
 * it waits for nobody. Daytona shows NETWORK
 * CHECKING, then CANCELLED, reboots and does it again, forever, and only the
 * test menu gets out of it. The board counts each start of its comm board as a
 * master alone (m2comm_lone_masters, board/m2comm.h); the host pauses the game
 * at the next one and asks what to do:
 *
 *   LINK_ANSWER_TEST_MENU  back to the test menu, to set the cabinet SINGLE:
 *                          Test is held for 20 frames (Daytona answers it in
 *                          NETWORK CHECKING too)
 *   LINK_ANSWER_CONTINUE   go on as it is; not asked again for this set
 *   LINK_ANSWER_CABLE      plug in a virtual TOSlink cable: the comm board's
 *                          output into its own input, a ring of one cabinet
 *                          (m2comm_set_cable). Offered only here, and only
 *                          to a game that plays on a ring of one
 *                          (game_profile_t link_cable): Daytona does;
 *                          Virtual-On counts one node as "Illegal Nodes: 1"
 *                          and checks again, for ever.
 *
 * Not asked of a board that is not the player's: a netplay session's or a
 * replay's (backup RAM detached), which boots blank, so SINGLE, anyway; a
 * session's own link takes the cable's place. A headless run never asks:
 * get_status's link_pace reports the count and the cable, and the bridge's
 * link_check answers. Every frontend asks: the desktop in ImGui
 * (ui/link_check_prompt.h), the web page in HTML (web_link_check), libretro and
 * SDL3 with the pad, through link_ask_frame below (ui/link_check_pad.h, main_sdl.c).
 *
 * UI thread. The count is the emu thread's, read without the mutex: a new
 * start shows up a frame later at worst.
 */
#ifndef CORE_LINK_CHECK_H
#define CORE_LINK_CHECK_H

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

#include "backup_ram.h"
#include "game_profile.h"
#include "input.h"
#include "log.h"
#include "m2comm.h"

#define LINK_CHECK_TEST_FRAMES 20u   /* board frames Test is held for */

typedef enum { LINK_ANSWER_TEST_MENU, LINK_ANSWER_CONTINUE, LINK_ANSWER_CABLE } link_answer_t;

typedef struct {
    uint32_t handled;     /* lone-master starts answered */
    bool     quiet;       /* LINK_ANSWER_CONTINUE: not asked again for this set */
    bool     test_held;   /* Test is held, until board frame test_until */
    uint32_t test_until;
} link_check_t;

static link_check_t g_link_check;

/* The question on a frontend that draws it itself (libretro, SDL3). */
typedef enum {
    LINK_ASK_NONE,      /* nothing asked: the game runs and has the pad */
    LINK_ASK_ASKING,    /* the question is up: the board waits, the game gets no pad */
    LINK_ASK_RELEASE,   /* answered, button still down: the board runs, no pad yet */
} link_ask_state_t;

typedef struct {
    link_ask_state_t state;
    int  sel;           /* the row picked */
    bool test;          /* the profile has a Test button: the test menu row */
    bool cable;         /* the game plays on a ring of one: the cable row */
} link_ask_t;

static link_ask_t g_link_ask;

/* A set was loaded: nothing asked yet, the cable out. */
static inline void link_check_reset(void) {
    if (g_link_check.test_held) input_action_up(GAME_INPUT_TEST);
    g_link_check.handled   = m2comm_lone_masters();
    g_link_check.quiet     = false;
    g_link_check.test_held = false;
    g_link_ask.state       = LINK_ASK_NONE;
    m2comm_set_cable(false);
}

/* Whether the profile has a Test button to go back to the test menu with. */
static inline bool link_check_has_test(const game_profile_t *p) {
    return p && p->input.bits[GAME_INPUT_TEST] != 0;
}

/* Whether the question offers the profile's game the virtual TOSlink cable. */
static inline bool link_check_has_cable(const game_profile_t *p) {
    return p && p->link_cable;
}

/* True while a start of the board as a master alone waits for an answer. */
static inline bool link_check_due(void) {
    uint32_t n = m2comm_lone_masters();
    if (n == g_link_check.handled) return false;
    if (g_link_check.quiet || (g_backup.set[0] && !g_backup.attached)) {
        g_link_check.handled = n;
        return false;
    }
    return true;
}

/* board_frame: the board's frame count now (g_emu_frames). */
static inline void link_check_answer(link_answer_t a, uint32_t board_frame) {
    g_link_check.handled = m2comm_lone_masters();
    switch (a) {
        case LINK_ANSWER_TEST_MENU:
            if (!g_link_check.test_held) input_action_down(GAME_INPUT_TEST);
            g_link_check.test_held  = true;
            g_link_check.test_until = board_frame + LINK_CHECK_TEST_FRAMES;
            break;
        case LINK_ANSWER_CONTINUE: g_link_check.quiet = true; break;
        case LINK_ANSWER_CABLE:    m2comm_set_cable(true);    break;
    }
}

/* The Test bits a frontend that sets the board's held bits itself each frame
 * (libretro) adds to them while Test is held for the answer. */
static inline uint32_t link_check_test_bits(void) {
    return g_link_check.test_held && g_active_profile ? g_active_profile->input.bits[GAME_INPUT_TEST] : 0;
}

/* Once a host frame: lets go of Test once the board has run its frames. Until
 * then Test stays down whatever else lets go of it: on the web the answering
 * key's release reaches the game, and a key up clears an action it never
 * pressed (input_action_up). */
static inline void link_check_frame(uint32_t board_frame) {
    g_input.held |= link_check_test_bits();   /* not a second down: one up lets go */
    if (g_link_check.test_held && (int32_t)(board_frame - g_link_check.test_until) >= 0) {
        g_link_check.test_held = false;
        input_action_up(GAME_INPUT_TEST);
    }
}

/* The question, for every frontend that asks it. */
#define LINK_CHECK_TITLE "No other cabinet on the link"

/* Where the loaded game's test menu sets it to play alone (game_profile_t
 * link_setting), or NULL. */
static inline const char *link_check_setting(void) {
    return g_active_profile ? g_active_profile->link_setting : NULL;
}

/* The question's text, for the loaded game. */
static inline const char *link_check_text(void) {
    static char text[384];
    const char *where = link_check_setting();
    if (where)
        snprintf(text, sizeof text,
                 "This cabinet is set to be the link's MASTER, but no other cabinet is linked to it.\n"
                 "It will wait for one, give up and restart, over and over.\n"
                 "Set it to play alone in the test menu: %s.", where);
    else
        snprintf(text, sizeof text, "%s",
                 "This cabinet is set to be the link's MASTER, but no other cabinet is linked to it.\n"
                 "It will wait for one, give up and restart, over and over.\n"
                 "Set it to play alone in the test menu's game settings.");
    return text;
}

/* The question in one line, for a log. */
static inline const char *link_check_line(void) {
    static char line[256];
    const char *where = link_check_setting();
    snprintf(line, sizeof line, "No other cabinet on the link: this MASTER waits for nobody. %s%s%s",
             where ? "Set it to play alone in the test menu: " : "Set it to play alone in the test menu.",
             where ? where : "", where ? "." : "");
    return line;
}

/* The answers' names, by link_answer_t, and what the cable is. */
static const char *const LINK_ANSWER_LABEL[] = { "Back to the test menu", "Continue anyway",
                                                 "Insert a virtual TOSlink cable" };
#define LINK_CHECK_CABLE_HINT \
    "Wires this comm board's output to its own input: a link of one cabinet, " \
    "so the MASTER finds itself. Until another game is loaded."

/* The rows a frontend that draws the question shows, top to bottom. */
static inline int link_ask_rows(link_answer_t rows[3]) {
    int n = 0;
    if (g_link_ask.test) rows[n++] = LINK_ANSWER_TEST_MENU;
    rows[n++] = LINK_ANSWER_CONTINUE;
    if (g_link_ask.cable) rows[n++] = LINK_ANSWER_CABLE;
    return n;
}

/* Buttons for link_ask_frame, from whatever the frontend reads. */
#define LINK_KEY_UP     1u
#define LINK_KEY_DOWN   2u
#define LINK_KEY_ANSWER 4u

/* Once a host frame, on the frontend's thread: held, the LINK_KEY_* bits down
 * now; hit, those pressed since the last call. While it says ASKING the board
 * must not run and the game gets none of the input; after an answer
 * (RELEASE) the game still gets none until the answering button is let go,
 * so its press does not reach the game. */
static inline link_ask_state_t link_ask_frame(uint32_t held, uint32_t hit, uint32_t board_frame) {
    link_ask_t *a = &g_link_ask;
    if (a->state == LINK_ASK_RELEASE && !(held & LINK_KEY_ANSWER)) a->state = LINK_ASK_NONE;
    if (a->state == LINK_ASK_NONE) {
        if (!link_check_due()) return LINK_ASK_NONE;
        a->state = LINK_ASK_ASKING;
        a->sel   = 0;
        a->test  = link_check_has_test(g_active_profile);
        a->cable = link_check_has_cable(g_active_profile);
        LOG_WARN("link: this cabinet is the link's MASTER and no other cabinet is linked; asking");
        return a->state;   /* a button down now was not pressed at the question */
    }
    if (a->state != LINK_ASK_ASKING) return a->state;
    link_answer_t rows[3];
    int n = link_ask_rows(rows);
    if ((hit & LINK_KEY_UP) && a->sel > 0) a->sel--;
    if ((hit & LINK_KEY_DOWN) && a->sel < n - 1) a->sel++;
    if (hit & LINK_KEY_ANSWER) {
        link_check_answer(rows[a->sel], board_frame);
        a->state = LINK_ASK_RELEASE;
    }
    return a->state;
}

static inline bool link_ask_up(void) { return g_link_ask.state == LINK_ASK_ASKING; }

#endif /* CORE_LINK_CHECK_H */
