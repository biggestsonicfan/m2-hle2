/*
 * multi_pads.h — who plays which board while several run (`--boards N`,
 * core/emu_multi.h; Pinboard #1914, experimental).
 *
 * Every gamepad and racing wheel is a board's own (sdl_pads.h reads each
 * alone while multi runs, g_pad_multi): its buttons and its wheel and pedals
 * go to that board only (g_multi.pad_held / pad_an), so two or eight people
 * can play side by side in one window. The keyboard plays the board in focus
 * (a click on it, or Ctrl+1..8), with the sound.
 *
 * A device's seat is Auto, a board, or None. The Auto devices take the
 * boards nobody chose, in order, in the order SDL lists them (the wheels,
 * then the gamepads): on Virtual-On the first pad plays board 1 and the
 * second board 2; on Daytona pads 1..8 the cabinets 1..8. A Main the
 * profile's operator works (Power Sled's) is nobody's seat: the rides are.
 * Seats are for this run, kept by the device (SDL's id) while it stays
 * plugged in. Emulation > Boards > Controllers... sets them.
 *
 * Desktop only, UI thread; the window's pads (main.c's M2HLE_PADS).
 */
#ifndef M2HLE_MULTI_PADS_H
#define M2HLE_MULTI_PADS_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdl_pads.h"
#include "multi_view.h"

#define MULTI_SEAT_AUTO (-2)
#define MULTI_SEAT_NONE (-1)

typedef struct { SDL_JoystickID id; int board; } multi_pads_seat_t;
static struct {
    multi_pads_seat_t chosen[PAD_MULTI_DEVS];
    int n;
    int board[PAD_MULTI_DEVS];   /* g_pad_multi.dev[k]'s board this frame, or -1 */
} g_mpads;

/* The first board a player sits at: a Main with an operator works itself. */
static inline int multi_pads__first(void) {
    return g_multi.linked && g_multi.op ? 1 : 0;
}

static inline int multi_pads__chosen(SDL_JoystickID id) {
    for (int i = 0; i < g_mpads.n; i++)
        if (g_mpads.chosen[i].id == id) return g_mpads.chosen[i].board;
    return MULTI_SEAT_AUTO;
}

static inline void multi_pads__choose(SDL_JoystickID id, int board) {
    for (int i = 0; i < g_mpads.n; i++)
        if (g_mpads.chosen[i].id == id) { g_mpads.chosen[i].board = board; return; }
    if (g_mpads.n < PAD_MULTI_DEVS) g_mpads.chosen[g_mpads.n++] = (multi_pads_seat_t){ id, board };
}

/* Each device's board: the chosen ones, then the Auto ones on the boards
 * left, in order. */
static inline void multi_pads__seat(void) {
    bool taken[EMU_MULTI_MAX] = {0};
    for (int k = 0; k < g_pad_multi.n; k++) {
        int b = multi_pads__chosen(g_pad_multi.dev[k].id);
        g_mpads.board[k] = b >= 0 && b < g_multi.n ? b : -1;
        if (g_mpads.board[k] >= 0) taken[b] = true;
    }
    int next = multi_pads__first();
    for (int k = 0; k < g_pad_multi.n; k++) {
        if (multi_pads__chosen(g_pad_multi.dev[k].id) != MULTI_SEAT_AUTO) continue;
        while (next < g_multi.n && taken[next]) next++;
        if (next < g_multi.n) g_mpads.board[k] = next++;
    }
}

/* A device's short name: Pad 2, Wheel 1 (its place among its kind). */
static inline void multi_pads__short(int k, char *buf, size_t cap) {
    int nth = 1;
    for (int j = 0; j < k; j++) nth += g_pad_multi.dev[j].wheel == g_pad_multi.dev[k].wheel;
    snprintf(buf, cap, "%s %d", g_pad_multi.dev[k].wheel ? "Wheel" : "Pad", nth);
}

/* Multi on or off: the pads go to the boards, or back to the shared actions.
 * True when pad_refresh has to run. Once a host frame, before the pads'. */
static inline bool multi_pads_switch(void) {
    bool on = emu_multi_on();
    if (on == g_pad_multi.on) return false;
    g_pad_multi.on = on;
    g_pad_multi.n = 0;
    return true;
}

/* The pads' bits and analog to their boards, and the grid's labels. Once a
 * host frame, before multi_view_frame. */
static inline void multi_pads_frame(void) {
    memset(g_multi.pad_held, 0, sizeof g_multi.pad_held);
    memset((void *)g_multi.pad_an, 0, sizeof g_multi.pad_an);
    memset(g_mview.seat, 0, sizeof g_mview.seat);
    if (!emu_multi_on() || !g_pad_multi.on) return;
    multi_pads__seat();
    for (int k = 0; k < g_pad_multi.n; k++) {
        int b = g_mpads.board[k];
        if (b < 0) continue;
        const pad_multi_dev_t *d = &g_pad_multi.dev[k];
        g_multi.pad_held[b] |= d->held;
        if (d->an.on && !g_multi.pad_an[b].on) g_multi.pad_an[b] = d->an;
        char s[24];
        multi_pads__short(k, s, sizeof s);
        size_t len = strlen(g_mview.seat[b]);
        snprintf(g_mview.seat[b] + len, sizeof g_mview.seat[b] - len, "%s%s", len ? ", " : "", s);
    }
}

/* A board's name in the seat combo: "2 SLAVE", "3 RED ride". */
static inline void multi_pads__board_label(int b, char *buf, size_t cap) {
    const char *name = multi_view__name(b);
    snprintf(buf, cap, "Board %d%s%s", b + 1, name[0] ? " " : "", name);
}

static inline void multi_pads__combo(int k) {
    SDL_JoystickID id = g_pad_multi.dev[k].id;
    int chosen = multi_pads__chosen(id), at = g_mpads.board[k];
    char cur[80], item[80];
    if (chosen == MULTI_SEAT_AUTO) {
        char b[56];
        if (at >= 0) multi_pads__board_label(at, b, sizeof b);
        snprintf(cur, sizeof cur, "Auto: %s", at >= 0 ? b : "no board left");
    } else if (at >= 0) multi_pads__board_label(at, cur, sizeof cur);
    else snprintf(cur, sizeof cur, "None");
    igSetNextItemWidth(220.0f);
    char cid[24];
    snprintf(cid, sizeof cid, "##seat%u", (unsigned)id);
    if (!igBeginCombo(cid, cur, 0)) return;
    if (igSelectableEx("Auto", chosen == MULTI_SEAT_AUTO, 0, (ImVec2){ 0, 0 })) multi_pads__choose(id, MULTI_SEAT_AUTO);
    for (int b = multi_pads__first(); b < g_multi.n; b++) {
        multi_pads__board_label(b, item, sizeof item);
        if (igSelectableEx(item, chosen == b, 0, (ImVec2){ 0, 0 })) multi_pads__choose(id, b);
    }
    if (igSelectableEx("None", chosen == MULTI_SEAT_NONE, 0, (ImVec2){ 0, 0 })) multi_pads__choose(id, MULTI_SEAT_NONE);
    igEndCombo();
}

/* Emulation > Boards > Controllers...: each device and its board. */
static inline void multi_pads_window(bool pads_on) {
    if (!g_mview.show_pads) return;
    if (!igBegin("Board controllers", &g_mview.show_pads, ImGuiWindowFlags_AlwaysAutoResize)) { igEnd(); return; }
    if (!emu_multi_on()) {
        igTextWrapped("One board runs: every pad and the keyboard play it. Pick 2 boards or more "
                      "in Emulation > Boards to give each player a board.");
        igEnd();
        return;
    }
    char b[56];
    multi_pads__board_label(g_multi.focus, b, sizeof b);
    igText("Keyboard");
    igSameLineEx(160.0f, -1.0f);
    igText("%s (the board in focus: click one, or Ctrl+1..%d)", b, g_multi.n);
    if (multi_pads__first()) {
        multi_pads__board_label(0, b, sizeof b);
        igTextDisabled("%s works itself (the operator): any player's buttons reach it", b);
    }
    igSeparator();
    if (!pads_on) igTextDisabled("No gamepad support in this build or session (SDL did not start).");
    else if (!g_pad_multi.n) igTextDisabled("No gamepad or wheel plugged in (or Controls > Devices is keyboard only).");
    for (int k = 0; k < g_pad_multi.n; k++) {
        const pad_multi_dev_t *d = &g_pad_multi.dev[k];
        char s[24];
        multi_pads__short(k, s, sizeof s);
        igPushIDInt(k);
        bool live = d->held || (d->an.on && (d->an.accel > 4000 || d->an.brake > 4000 || abs(d->an.steer) > 8000));
        igTextColored(live ? (ImVec4){ 0.3f, 1.0f, 0.4f, 1.0f } : (ImVec4){ 0.5f, 0.5f, 0.5f, 1.0f }, "%s", live ? "(*)" : "( )");
        igSameLine();
        igText("%s", s);
        igSameLineEx(160.0f, -1.0f);
        multi_pads__combo(k);
        igSameLine();
        const char *n = d->wheel ? SDL_GetJoystickNameForID(d->id) : SDL_GetGamepadNameForID(d->id);
        igTextDisabled("%s", n ? n : "?");
        igPopID();
    }
    igSeparator();
    igTextDisabled("Press a button: its row lights, and its board's frame turns green.");
    igTextDisabled("Auto pads take the boards nobody chose, in order. Kept while plugged in.");
    igEnd();
}

#endif
