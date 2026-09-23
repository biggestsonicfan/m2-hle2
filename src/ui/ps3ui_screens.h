/*
 * ps3ui_screens.h -- the PS3-style screens, drawn with ps3ui.h.
 *
 * Each screen is a function of a small state struct: which AET compositions
 * it plays, at which frame, and the text the PS3's program would draw into
 * their placeholders, placed by the rules read out of the PS3 wrapper
 * (Text_Printf at 0x430ac and its callers; see tools/ps3ui/README.md).
 *
 * Button glyphs travel in strings as control characters: PS3UI_BTN_CIRCLE
 * and friends, which the text drawer renders as the pad's face buttons.
 */
#ifndef PS3UI_SCREENS_H
#define PS3UI_SCREENS_H

#include <stdio.h>
#include "ps3ui.h"
#include "ps3ui_layout.h"
#include "ps3ui_sprites.h"

/* ---- text styles ----------------------------------------------------------- */

/* The PS3's font 2 (titles, numbers) at its native size. */
static ps3ui_text_style_t ps3ui_style_title(float cap)
{
    ps3ui_text_style_t s = { PS3UI_FONT_TITLE, cap, 0, 0, 0xFFFFFF, 0, ps3ui_ps3_metrics[1], 40.0f };
    return s;
}

/* The PS3's font 1 (everything else), proportional. */
static ps3ui_text_style_t ps3ui_style_text(float cap)
{
    ps3ui_text_style_t s = { PS3UI_FONT_TEXT, cap, 0, 0, 0xFFFFFF, 0, ps3ui_ps3_metrics[0], 37.0f };
    return s;
}

/* Names: font 1 on a fixed 23-unit pitch. */
static ps3ui_text_style_t ps3ui_style_name(void)
{
    ps3ui_text_style_t s = { PS3UI_FONT_MONO, 37.0f, 0, 23.0f, 0xFFFFFF, 0, NULL, 37.0f };
    return s;
}

/* ---- text with button glyphs ------------------------------------------------ */

/* Width of a string whose control characters are button glyphs. */
static float ps3ui_rich_width(const ps3ui_text_style_t *st, const char *s)
{
    float w = 0.0f;
    char run[256];
    int n = 0;
    for (const char *p = s;; p++) {
        if (*p == 0 || (*p >= PS3UI_BTN_CIRCLE && *p <= PS3UI_BTN_SQUARE)) {
            run[n] = 0;
            if (n)
                w += ps3ui_text_width(st, run) + (st->metrics ? 4.0f * st->cap / st->ps3_cap : 0.0f);
            n = 0;
            if (*p == 0)
                break;
            w += ps3ui_button_adv(st->cap);
        } else if (n < 255) {
            run[n++] = *p;
        }
    }
    return w;
}

/* Draw a string with button glyphs, baseline-left at (x, y). */
static void ps3ui_rich(ps3ui_canvas_t *cv, const ps3ui_text_style_t *st, float x, float y, const char *s,
                       float opacity)
{
    char run[256];
    int n = 0;
    for (const char *p = s;; p++) {
        if (*p == 0 || (*p >= PS3UI_BTN_CIRCLE && *p <= PS3UI_BTN_SQUARE)) {
            run[n] = 0;
            if (n) {
                ps3ui_text(cv, st, x, y, run, opacity);
                x += ps3ui_text_width(st, run) + (st->metrics ? 4.0f * st->cap / st->ps3_cap : 0.0f);
            }
            n = 0;
            if (*p == 0)
                break;
            ps3ui_draw_button(cv, *p, x, y, st->cap, opacity);
            x += ps3ui_button_adv(st->cap);
        } else if (n < 255) {
            run[n++] = *p;
        }
    }
}

/* ---- placeholders ------------------------------------------------------------ */

#define PS3UI_MAX_SLOTS 48

typedef struct {
    const char *name;
    ps3ui_mat_t m;
    float opacity;
    int w, h;
} ps3ui_slot_t;

typedef struct {
    ps3ui_slot_t slot[PS3UI_MAX_SLOTS];
    int n;
} ps3ui_slots_t;

static void ps3ui_slots_hook(void *user, ps3ui_canvas_t *cv, const char *name, ps3ui_mat_t m, float opacity,
                             int w, int h)
{
    (void)cv;
    ps3ui_slots_t *s = (ps3ui_slots_t *)user;
    if (s->n < PS3UI_MAX_SLOTS) {
        ps3ui_slot_t *o = &s->slot[s->n++];
        o->name = name;
        o->m = m;
        o->opacity = opacity;
        o->w = w;
        o->h = h;
    }
}

static const ps3ui_slot_t *ps3ui_slot(const ps3ui_slots_t *s, const char *name)
{
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->slot[i].name, name) == 0)
            return &s->slot[i];
    return NULL;
}

/* A slot's box corner (u,v in its own w x h) in layout units. */
static void ps3ui_slot_at(const ps3ui_slot_t *s, float u, float v, float *x, float *y)
{
    ps3ui_mat_apply(s->m, u, v, x, y);
}

/* Play composition `comp` of `scene` at frame f, collecting placeholders. */
static void ps3ui_play(ps3ui_canvas_t *cv, const ps3ui_scene_t *scene, const char *comp, float f,
                       ps3ui_mat_t m, ps3ui_slots_t *slots)
{
    int ci = ps3ui_comp_find(scene, comp);
    if (ci < 0) {
        fprintf(stderr, "ps3ui: no composition %s\n", comp);
        return;
    }
    ps3ui_play_t p = { scene, ps3ui_slots_hook, slots, NULL };
    ps3ui_play_comp(&p, cv, ci, f, m, 1.0f, PS3UI_BLEND_NORMAL);
}

/* ---- common chrome --------------------------------------------------------- */

/* The background every menu sits on (n_cmn_base "bg"). */
static void ps3ui_draw_bg(ps3ui_canvas_t *cv, float f)
{
    ps3ui_slots_t slots = { 0 };
    /* the 16:9 frame's margins, if the output is not 16:9 */
    ps3ui_clear(cv, 0x001735);
    ps3ui_play(cv, &ps3ui_n_cmn_base, "bg", f, ps3ui_mat_id(), &slots);
}

/* The button-hint bar along the bottom (n_cmn_base "sousa_win"). Its text
 * goes in last, right-aligned against the bar's bottom-right placeholder, so
 * the bar returns that slot for ps3ui_draw_hint_text. */
static ps3ui_slot_t ps3ui_draw_hint_bar(ps3ui_canvas_t *cv, float f)
{
    ps3ui_slots_t slots = { 0 };
    ps3ui_slot_t none = { 0 };
    ps3ui_play(cv, &ps3ui_n_cmn_base, "sousa_win", f, ps3ui_mat_id(), &slots);
    const ps3ui_slot_t *rb = ps3ui_slot(&slots, "p_sousa_win_02_rb");
    return rb ? *rb : none;
}

static void ps3ui_draw_hint_text(ps3ui_canvas_t *cv, const ps3ui_slot_t *rb, const char *hints)
{
    if (!rb->name || !hints || !*hints)
        return;
    ps3ui_text_style_t st = ps3ui_style_text(37.0f);
    float x, y;
    ps3ui_slot_at(rb, (float)rb->w, (float)rb->h, &x, &y);
    ps3ui_rich(cv, &st, x - ps3ui_rich_width(&st, hints), y - 8.0f, hints, rb->opacity);
}

/* Play a one-shot effect and hold its last frame, as the program does: the
 * last frame of the layer that places the composition in its scene. */
static float ps3ui_hold(const ps3ui_scene_t *scene, const char *comp, float f)
{
    int ci = ps3ui_comp_find(scene, comp);
    for (int c = 0; ci >= 0 && c < scene->ncomps; c++)
        for (int i = 0; i < scene->comps[c].n; i++) {
            const ps3ui_layer_t *l = &scene->layers[scene->comps[c].first + i];
            if (l->type == PS3UI_LAYER_COMP && l->item == ci)
                return f < l->end - 1.0f ? f : l->end - 1.0f;
        }
    return f;
}

/* ---- the VS lobby (n_cmn_online "z_base") ---------------------------------- */

typedef struct {
    const char *title;          /* "PLAYER MATCH" / "RANKED MATCH" */
    const char *name[2];        /* 1P, 2P */
    int ping_ms[2];             /* signal bars; PS3UI_PING_NONE on your own row */
    int ready[2];
    float ready_age[2];         /* frames since READY went up */
    int countdown;              /* seconds; < 0 hides it */
    const char *left_hint;      /* the strip above the bar: "SELECT button:Controls" */
    const char *hints;          /* right-aligned in the bar */
    float frame;                /* z_base's own frame: 0..9 opens it, 10..69 loops */
} ps3ui_vs_lobby_t;

#define PS3UI_PING_NONE (-2)
#define PS3UI_PING_UNKNOWN (-1)

/* The PS3's signal icon for a ping (TaskSession, see ps3-lobby-ui.md 3.8). */
static int ps3ui_net_icon(int ping_ms)
{
    if (ping_ms < 0) return PS3UI_SPR_X_NET_XX;
    if (ping_ms < 80) return PS3UI_SPR_X_NET_03;
    if (ping_ms < 142) return PS3UI_SPR_X_NET_02;
    if (ping_ms < 204) return PS3UI_SPR_X_NET_01;
    return PS3UI_SPR_X_NET_00;
}

static void ps3ui_draw_vs_lobby(ps3ui_canvas_t *cv, const ps3ui_vs_lobby_t *s)
{
    float f = s->frame;
    ps3ui_draw_bg(cv, 100.0f);
    ps3ui_slots_t slots = { 0 };
    ps3ui_play(cv, &ps3ui_n_cmn_online, "z_base", f, ps3ui_mat_id(), &slots);
    /* the hint bar goes over z_base: its top rim covers the SELECT strip's foot */
    ps3ui_slot_t bar = ps3ui_draw_hint_bar(cv, f);

    const ps3ui_slot_t *t;
    float x, y;
    if ((t = ps3ui_slot(&slots, "head_tit_ct")) && s->title) {
        ps3ui_text_style_t st = ps3ui_style_title(40.0f);
        ps3ui_slot_at(t, (float)t->w * 0.5f, 0.0f, &x, &y);
        ps3ui_text(cv, &st, x - ps3ui_text_width(&st, s->title) * 0.5f, y + 47.0f, s->title, t->opacity);
    }
    if ((t = ps3ui_slot(&slots, "p_timer_c")) && s->countdown >= 0) {
        char buf[16];
        snprintf(buf, sizeof buf, "%d", s->countdown);
        ps3ui_text_style_t st = ps3ui_style_title(40.0f);
        ps3ui_slot_at(t, (float)t->w * 0.5f, (float)t->h * 0.5f, &x, &y);
        ps3ui_text(cv, &st, x - ps3ui_text_width(&st, buf) * 0.5f, y + 20.0f, buf, t->opacity);
    }
    static const char *const tag[2] = { "p_tag_1p_lt", "p_tag_2p_lt" };
    for (int i = 0; i < 2; i++) {
        if (!(t = ps3ui_slot(&slots, tag[i])))
            continue;
        ps3ui_slot_at(t, 0, 0, &x, &y);
        if (s->ping_ms[i] != PS3UI_PING_NONE) {
            const ps3ui_image_t *im = ps3ui_sprite(ps3ui_net_icon(s->ping_ms[i]));
            ps3ui_draw_image(cv, im, ps3ui_mat_translate(x + 2.0f, y + 3.0f), t->opacity, PS3UI_BLEND_NORMAL, NULL);
        }
        if (s->name[i]) {
            ps3ui_text_style_t st = ps3ui_style_name();
            ps3ui_text(cv, &st, x + 121.0f, y + 45.0f, s->name[i], t->opacity);
        }
        if (s->ready[i]) {
            /* eff_ready, placed by the program 318 right and 78 down of the tag */
            ps3ui_slots_t none = { 0 };
            ps3ui_mat_t m = ps3ui_mat_mul(ps3ui_mat_translate(x + 318.0f, y + 79.0f), ps3ui_mat_translate(-191.0f, -31.0f));
            ps3ui_play(cv, &ps3ui_n_cmn_online, "eff_ready",
                       ps3ui_hold(&ps3ui_n_cmn_online, "eff_ready", s->ready_age[i]), m, &none);
        }
    }
    if ((t = ps3ui_slot(&slots, "p_sousa2_win_01_lt")) && s->left_hint) {
        ps3ui_text_style_t st = ps3ui_style_text(33.0f);
        ps3ui_slot_at(t, 0, 0, &x, &y);
        ps3ui_rich(cv, &st, x + 1.0f, y + 37.0f, s->left_hint, t->opacity);
    }
    ps3ui_draw_hint_text(cv, &bar, s->hints);
}

#endif /* PS3UI_SCREENS_H */
