/*
 * multi_view.h — the desktop window's side of `--boards N` (Pinboard #1910,
 * experimental): every board of core/emu_multi.h drawn, side by side.
 *
 * Each board has a picture of its own: its own tile layers (video_state_t),
 * its own 3D state (geo3d_state_t, the main one's settings), its own render
 * bank (the texture atlas and the LUT textures, game_render_use_bank) and an
 * offscreen target the size of the board's screen. A round (emu_multi_round)
 * calls multi_view__drawn with each board in the bus just after its frame,
 * and that draws the board into its target; the window then shows the
 * targets in a grid, the one in focus framed. A click on a board, or
 * Ctrl+1..8, gives it the focus: the keyboard and the sound. Each gamepad
 * and wheel plays a board of its own (multi_pads.h, Pinboard #1914).
 * Emulation > Boards picks the count from the window, as `--boards` does.
 *
 * The board frames are paced here, at 60 a second of the host's clock, at
 * most one round a host frame (the emu thread only naps meanwhile).
 * Colour ramps and shade rows are off while it runs: they are built from one
 * board's tables. Sub-frame interpolation is off (each picture is its board's
 * frame as it ended). Desktop only; UI thread.
 */
#ifndef M2HLE_MULTI_VIEW_H
#define M2HLE_MULTI_VIEW_H

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "sokol_gfx.h"
#include "emu_thread.h"
#include "game_frame.h"
#include "game_render.h"
#include "post_shader.h"   /* the swapchain's colour and depth formats */
#include "video_window.h"

typedef struct {
    bool               made;
    video_state_t     *video;
    geo3d_state_t     *geo3d;
    game_render_bank_t bank;
    sg_image           color, depth;
    sg_view            att, datt, tex;
    uint64_t           drawn;           /* frames drawn into the target */
} multi_view_board_t;

static struct {
    multi_view_board_t b[EMU_MULTI_MAX];
    bool               started;         /* tried for this load of the set */
    bool               ramp, shade;     /* the renderer's, while multi is on */
    int64_t            last_us;
    int64_t            due_us;          /* host time owed a round */
    memory_bus_t      *bus;
    const romset_t    *rs;
    sg_pass_action     action;
    struct { int x, y, w, h; } cell[EMU_MULTI_MAX];
    int                cells;
    char               seat[EMU_MULTI_MAX][48];   /* the pads that play each board (multi_pads.h) */
    bool               show_pads;                 /* the Board controllers window */
} g_mview;

static inline bool multi_view__make(multi_view_board_t *v, const geo3d_state_t *base) {
    memset(v, 0, sizeof *v);
    v->video = (video_state_t *)calloc(1, sizeof *v->video);
    v->geo3d = (geo3d_state_t *)malloc(sizeof *v->geo3d);
    if (!v->video || !v->geo3d || !game_render_bank_make(&v->bank)) {
        free(v->video); free(v->geo3d);
        return false;
    }
    video_init(v->video);
    memcpy(v->geo3d, base, sizeof *v->geo3d);   /* the main window's 3D settings */
    v->geo3d->captured_count = 0;
    v->color = sg_make_image(&(sg_image_desc){
        .usage = { .color_attachment = true }, .width = VIDEO_WIDTH, .height = VIDEO_HEIGHT,
        .pixel_format = post_color_format(), .sample_count = 1, .label = "multi-board" });
    v->depth = sg_make_image(&(sg_image_desc){
        .usage = { .depth_stencil_attachment = true }, .width = VIDEO_WIDTH, .height = VIDEO_HEIGHT,
        .pixel_format = post_depth_format(), .sample_count = 1, .label = "multi-board-depth" });
    v->att  = sg_make_view(&(sg_view_desc){ .color_attachment.image = v->color });
    v->datt = sg_make_view(&(sg_view_desc){ .depth_stencil_attachment.image = v->depth });
    v->tex  = sg_make_view(&(sg_view_desc){ .texture.image = v->color });
    v->made = true;
    return true;
}

static inline void multi_view__free(multi_view_board_t *v) {
    if (!v->made) return;
    sg_destroy_view(v->tex); sg_destroy_view(v->att); sg_destroy_view(v->datt);
    sg_destroy_image(v->color); sg_destroy_image(v->depth);
    game_render_bank_free(&v->bank);
    video_shutdown(v->video);
    free(v->video);
    free(v->geo3d);
    memset(v, 0, sizeof *v);
}

static inline void multi_view_free_all(void) {
    for (int b = 0; b < EMU_MULTI_MAX; b++) multi_view__free(&g_mview.b[b]);
    if (g_game_render.initialized) {
        g_game_render.ramp_enabled  = g_mview.ramp;
        g_game_render.shade_enabled = g_mview.shade;
    }
    /* The main picture's bank missed what the boards wrote: decode it whole. */
    g_game_render_bank0.atlas_valid = g_game_render_bank0.luts_valid = false;
}

/* emu_multi_round's callback: board b is in the bus, its frame just ended. */
static void multi_view__drawn(void *user, int b) {
    (void)user;
    multi_view_board_t *v = &g_mview.b[b];
    if (!v->made) return;
    game_frame_prepare(v->video, v->geo3d, g_mview.bus, g_mview.rs, true);
    game_render_use_bank(&v->bank);
    sg_begin_pass(&(sg_pass){
        .action      = g_mview.action,
        .attachments = { .colors[0] = v->att, .depth_stencil = v->datt },
    });
    game_frame_draw(v->video, v->geo3d, g_mview.bus, g_mview.rs,
                    (game_rect_t){ 0, 0, VIDEO_WIDTH, VIDEO_HEIGHT }, 1.0f);
    sg_end_pass();
    game_render_use_bank(NULL);
    v->drawn++;
}

/* The boards made, once the set is loaded and running (`--boards N`). */
static inline void multi_view__start(emu_thread_ctx_t *ctx, const geo3d_state_t *base) {
    g_mview.started = true;
    const game_profile_t *p = g_active_profile;
    int n = emu_multi_count(p, g_multi_want);
    if (g_multi_want > n)
        LOG_WARN("multi: a %s ring holds %d cabinets; %d boards, not %d", p->id, n, n, g_multi_want);
    for (int b = 0; b < n; b++)
        if (!multi_view__make(&g_mview.b[b], base)) {
            LOG_ERROR("multi: no GPU resources for board %d", b + 1);
            multi_view_free_all();
            return;
        }
    emu_mutex_lock(&ctx->mutex);
    bool ok = emu_multi_start(ctx, p, n, true);
    emu_mutex_unlock(&ctx->mutex);
    if (!ok) { LOG_ERROR("multi: %d boards of %s could not start", n, p ? p->id : "?"); multi_view_free_all(); return; }
    g_mview.ramp  = g_game_render.ramp_enabled;
    g_mview.shade = g_game_render.shade_enabled;
    g_game_render.ramp_enabled = g_game_render.shade_enabled = false;
    g_mview.last_us = emu_now_us();
    g_mview.due_us  = 0;
}

/* A new set loaded: the boards are made again for it. */
static inline void multi_view_set_loaded(void) {
    multi_view_free_all();
    g_mview.started = false;
}

/* The menu's choice of 2..8: the boards running stop, and n of them start
 * from power-on at the next host frame. */
static inline void multi_view_want(emu_thread_ctx_t *ctx, int n) {
    emu_mutex_lock(&ctx->mutex);
    emu_multi_stop(ctx);
    emu_mutex_unlock(&ctx->mutex);
    multi_view_set_loaded();
    g_multi_want = n;
}

/* Emulation > Boards: one board, or 2..the most the loaded game takes (a
 * linked game's ring, else EMU_MULTI_MAX). Returns 0 when One board was
 * picked: the frontend loads the set again, as it starts alone (Power
 * Sled's ride has its Main beside it then, SOLO); -1 otherwise. */
static inline int multi_view_menu(emu_thread_ctx_t *ctx, bool loaded) {
    if (!igBeginMenuEx("Boards", loaded && g_active_profile)) return -1;
    const game_profile_t *p = g_active_profile;
    int ring = emu_multi_count(p, EMU_MULTI_MAX);
    bool linked = p->ring_role || p->link_role;
    int now = emu_multi_on() ? g_multi.n : g_multi_want ? emu_multi_count(p, g_multi_want) : 1;
    int picked = -1;
    if (igMenuItemEx("One board", NULL, now == 1, true) && now != 1) { g_multi_want = 0; picked = 0; }
    for (int n = 2; n <= ring; n++) {
        char label[48];
        snprintf(label, sizeof label, "%d boards%s", n, n == ring && linked ? " (all cabinets)" : "");
        if (igMenuItemEx(label, NULL, now == n, true) && now != n) multi_view_want(ctx, n);
    }
    igSeparator();
    igTextDisabled("%s", linked ? "Linked in a ring, board 1 the MASTER" : "Side by side, not linked");
    igTextDisabled("A new count starts every board from power-on");
    igTextDisabled("Click a board or Ctrl+1..8: the keyboard plays it");
    igMenuItemBoolPtr("Controllers...", NULL, &g_mview.show_pads, true);
    igEndMenu();
    return picked;
}

/* Once a host frame, before any pass: the boards made when due, then the
 * rounds owed, each board drawn into its target. True while the boards run:
 * the window then draws multi_view_draw, not its one picture. */
static inline bool multi_view_frame(emu_thread_ctx_t *ctx, memory_bus_t *bus, const romset_t *rs,
                                    const geo3d_state_t *base, bool running) {
    if (!g_multi_want) return false;
    if (!g_mview.started && running && rs->loaded && g_active_profile) multi_view__start(ctx, base);
    if (!emu_multi_on()) return false;
    g_mview.bus = bus;
    g_mview.rs  = rs;
    g_mview.action = (sg_pass_action){
        .colors[0] = { .load_action = SG_LOADACTION_CLEAR, .clear_value = { 0, 0, 0, 1 } },
        .depth     = { .load_action = SG_LOADACTION_CLEAR, .clear_value = 1.0f },
    };
    int64_t now = emu_now_us();
    g_mview.due_us += now - g_mview.last_us;
    g_mview.last_us = now;
    if (!running) { g_mview.due_us = 0; return true; }
    if (g_mview.due_us > 4 * EMU_SLICE_US) g_mview.due_us = 4 * EMU_SLICE_US;   /* a stall is not caught up */
    if (g_mview.due_us < EMU_SLICE_US) return true;
    g_mview.due_us -= EMU_SLICE_US;
    emu_mutex_lock(&ctx->mutex);
    emu_multi_round(ctx, multi_view__drawn, NULL);
    emu_mutex_unlock(&ctx->mutex);
    if (g_multi.rounds % 600 == 0) emu_multi_say();
    return true;
}

/* The grid for n pictures of aspect a in w x h: the column count that gives
 * the largest picture. */
static inline int multi_view__cols(int n, int w, int h, float a) {
    int best = 1;
    float best_w = 0.0f;
    for (int c = 1; c <= n; c++) {
        int r = (n + c - 1) / c;
        float cw = (float)w / (float)c, ch = (float)h / (float)r;
        float pw = cw / ch > a ? ch * a : cw;
        if (pw > best_w) { best_w = pw; best = c; }
    }
    return best;
}

/* Board b's label: the profile's name for that cabinet of the ring, or its
 * role; nothing unlinked. */
static inline const char *multi_view__name(int b) {
    if (!g_multi.linked) return "";
    const char *const *names = g_active_profile ? g_active_profile->ring_names : NULL;
    for (int i = 0; names && names[i]; i++)
        if (i == b) return names[i];
    return b ? "SLAVE" : "MASTER";
}

/* Where each board's picture goes in ox, oy, w, h, into g_mview.cell. A
 * ring whose MASTER plays itself (ring_op: Power Sled's Main) has it on top,
 * centred over one row of its slaves; any other ring is one row (Daytona's
 * cabinets, Virtual-On's pair); unlinked boards take the grid with the
 * largest pictures. The pictures keep the screen's aspect and the block of
 * them is centred. */
static inline void multi_view__place(int n, int ox, int oy, int w, int h) {
    float a = (float)VIDEO_WIDTH / (float)VIDEO_HEIGHT;
    bool main_above = g_multi.linked && n > 1 && g_active_profile && g_active_profile->ring_op;
    int cols, rows;
    if (main_above)          { cols = n - 1; rows = 2; }
    else if (g_multi.linked) { cols = n;     rows = 1; }
    else                     { cols = multi_view__cols(n, w, h, a); rows = (n + cols - 1) / cols; }
    int cw = w / cols, ch = h / rows;
    game_rect_t pic = game_render_letterbox(cw - 4, ch - 4, VIDEO_WIDTH, VIDEO_HEIGHT);
    int pw = pic.w + 4, ph = pic.h + 4;                 /* a cell, its 2-pixel gutter */
    int y0 = oy + (h - rows * ph) / 2;
    for (int b = 0; b < n; b++) {
        int r, c, in_row;
        if (main_above) { r = b ? 1 : 0; c = b ? b - 1 : 0; in_row = b ? n - 1 : 1; }
        else            { r = b / cols;  c = b % cols;      in_row = r < rows - 1 ? cols : n - r * cols; }
        int x0 = ox + (w - in_row * pw) / 2;            /* each row centred, a short last one too */
        g_mview.cell[b].x = x0 + c * pw + 2;
        g_mview.cell[b].y = y0 + r * ph + 2;
        g_mview.cell[b].w = pic.w;
        g_mview.cell[b].h = pic.h;
    }
}

/* The boards' pictures over ox, oy, w, h of the swapchain pass
 * (multi_view__place), and their numbers over them. */
static inline void multi_view_draw(int ox, int oy, int w, int h) {
    int n = g_multi.n;
    multi_view__place(n, ox, oy, w, h);
    float dpi = sapp_dpi_scale() > 0 ? sapp_dpi_scale() : 1.0f;
    ImDrawList *dl = igGetBackgroundDrawList();
    for (int b = 0; b < n; b++) {
        game_rect_t pic = { g_mview.cell[b].x, g_mview.cell[b].y, g_mview.cell[b].w, g_mview.cell[b].h };
        if (g_mview.b[b].drawn) game_render_draw_target(g_mview.b[b].tex, true, pic.x, pic.y, pic.w, pic.h);
        ImVec2 p0 = { (float)pic.x / dpi, (float)pic.y / dpi };
        ImVec2 p1 = { (float)(pic.x + pic.w) / dpi, (float)(pic.y + pic.h) / dpi };
        bool mine = b == g_multi.focus;
        if (mine) ImDrawList_AddRectEx(dl, p0, p1, 0xFF30D0FFu, 0.0f, 3.0f, 0);
        else if (g_multi.pad_held[b])   /* a pad's button held: which board it plays */
            ImDrawList_AddRectEx(dl, p0, p1, 0xFF40FF60u, 0.0f, 2.0f, 0);
        char label[128];
        snprintf(label, sizeof label, "%d %s%s%s%s", b + 1, multi_view__name(b),
                 g_mview.seat[b][0] ? "  " : "", g_mview.seat[b], mine ? "  (keys, sound)" : "");
        ImVec2 t = { p0.x + 6.0f, p0.y + 4.0f };
        ImDrawList_AddTextImFontPtr(dl, igGetFont(), igGetFontSize(), (ImVec2){ t.x + 1, t.y + 1 }, 0xC0000000u, label);
        ImDrawList_AddTextImFontPtr(dl, igGetFont(), igGetFontSize(), t, mine ? 0xFF30D0FFu : 0xFFE0E0E0u, label);
    }
    g_mview.cells = n;
}

/* A click on a board's picture, or Ctrl+1..8: that board takes the focus.
 * True when the event was this. */
static inline bool multi_view_event(const sapp_event *ev) {
    if (!emu_multi_on()) return false;
    if (ev->type == SAPP_EVENTTYPE_MOUSE_DOWN && ev->mouse_button == SAPP_MOUSEBUTTON_LEFT
            && !igGetIO()->WantCaptureMouse) {
        for (int b = 0; b < g_mview.cells; b++)
            if (ev->mouse_x >= g_mview.cell[b].x && ev->mouse_x < g_mview.cell[b].x + g_mview.cell[b].w
                    && ev->mouse_y >= g_mview.cell[b].y && ev->mouse_y < g_mview.cell[b].y + g_mview.cell[b].h) {
                emu_multi_set_focus(b);
                return true;
            }
    }
    if (ev->type == SAPP_EVENTTYPE_KEY_DOWN && (ev->modifiers & SAPP_MODIFIER_CTRL)
            && ev->key_code >= SAPP_KEYCODE_1 && ev->key_code <= SAPP_KEYCODE_8) {
        emu_multi_set_focus((int)(ev->key_code - SAPP_KEYCODE_1));
        return true;
    }
    return false;
}

#endif
