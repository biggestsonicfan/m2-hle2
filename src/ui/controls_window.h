/*
 * controls_window.h -- the desktop's Controls window (Pinboard #1147): which
 * devices play besides the keyboard, the keyboard's keys, the gamepad's
 * buttons and the racing wheel's axes and buttons, kept per profile.
 *
 * Each profile's choice lives in controls/<profile id>.cfg beside the
 * per-user netplay settings (%APPDATA%\m2hle2, ~/.config/m2hle2), written on
 * every change; a profile without one plays the built-in map:
 *
 *   device=any|keyboard|gamepad|wheel  (keyboard: no pad or wheel plays)
 *   key.<action>=<key>[,<key>]         (p1_up .. p2_coin, service, test;
 *                                       input_keycode_by_name's names; none)
 *   pad.<button>=<actions>             (south east west north start back l1 r1
 *                                       l3 r3 guide; b1+b2, start, service, none)
 *   pad.device=<guid> <name>           (the gamepad that plays; absent: any)
 *   wheel.device=<guid> <name>         (the wheel; absent: one SDL calls a wheel)
 *   wheel.pedals=<guid> <name>         (pedals on their own lead; absent: the wheel's)
 *   wheel.axes=<steer>,<accel>,<brake> (joystick axis numbers)
 *   wheel.rest=<accel>,<brake>         (pedals released, -32768..32767 or auto)
 *   wheel.center=<n>                   (the wheel axis held straight; absent: 0)
 *   wheel.range=<degrees>              (a Logitech wheel's turn, lg4ff.h; absent:
 *                                       --g25-range's, 270)
 *   wheel.<n>=<actions>                (joystick button n, 0-31)
 *
 * The map is the host's only (input.h, sdl_pads.h): the board sees actions, so
 * nothing here changes what a session or a replay sends (the range and the
 * centre included: the board reads where the wheel is). --pad-map,
 * --wheel-axes and --wheel-buttons apply over the file for that run; the first
 * change made here drops them. --headless reads no file.
 */
#ifndef CONTROLS_WINDOW_H
#define CONTROLS_WINDOW_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cimgui.h"
#include "sokol_app.h"

#include "backup_ram.h"     /* backup_ram_user_dir */
#include "game_profile.h"
#include "input.h"
#include "log.h"
#if M2HLE_PADS
#include "sdl_pads.h"
#include "lg4ff.h"
#endif

#define CONTROLS_CLI_MAX 8

static struct {
    const game_profile_t *loaded;   /* whose controls are in; NULL: none yet */
    int  cap_action, cap_slot;      /* the key slot waiting for a key; -1: none */
    int  swallow;                   /* the captured key, whose release the game must not see */
    char status[160];
    struct { char opt[24], val[256]; } cli[CONTROLS_CLI_MAX];   /* --pad-map and the like */
    int  cli_n;
    int  det_role;                  /* the wheel axis a Detect button waits for (0 steer, 1 accel, 2 brake); -1: none */
    uint32_t det_id;                /* the device it watches */
    int  det_start[16];             /* its axes when Detect was clicked */
    int  learn_act;                 /* the action a wheel button is being set for; -1: none */
    uint64_t learn_held;            /* the wheel's buttons held then, which do not count */
    int  range_default;             /* the turn without a wheel.range line (--g25-range's) */
    bool range_known;
    int  range_drag;                /* the rotation slider's value while it is held */
    bool range_held;
} g_ctl = { .cap_action = -1, .det_role = -1, .learn_act = -1 };

/* The words the window shows for the actions. */
static const char *const g_ctl_action_label[GAME_INPUT_COUNT] = {
    "Up", "Down", "Left", "Right", "Button 1", "Button 2", "Button 3", "Button 4", "Start", "Coin",
    "Up", "Down", "Left", "Right", "Button 1", "Button 2", "Button 3", "Button 4", "Start", "Coin",
    "Service", "Test", "Gear N", "Gear 1", "Gear 2", "Gear 3", "Gear 4",
};

/* A pad option from the command line, applied again over each profile's file. */
static inline void controls_cli_note(const char *opt, const char *val) {
    if (g_ctl.cli_n >= CONTROLS_CLI_MAX) return;
    snprintf(g_ctl.cli[g_ctl.cli_n].opt, sizeof g_ctl.cli[0].opt, "%s", opt);
    snprintf(g_ctl.cli[g_ctl.cli_n].val, sizeof g_ctl.cli[0].val, "%s", val);
    g_ctl.cli_n++;
}

/* <user dir>/controls/<id>.cfg for profile p; false if there is no user dir. */
static inline bool controls_path(const game_profile_t *p, char *out, size_t cap) {
    char dir[512];
    if (!p || !backup_ram_user_dir(dir, sizeof dir)) return false;
    size_t n = strlen(dir);
    snprintf(dir + n, sizeof dir - n, BACKUP_RAM_SEP "controls");
    backup_ram_mkdir(dir);
    snprintf(out, cap, "%s" BACKUP_RAM_SEP "%s.cfg", dir, p->id);
    return true;
}

/* Profile p's built-in map: its default keys (input.h), the pads' binds. */
static inline void controls_defaults(const game_profile_t *p) {
    input_keys_default(p);
#if M2HLE_PADS
    sdl_pads_default();
    if (!g_ctl.range_known) { g_ctl.range_default = g_lg4ff_range; g_ctl.range_known = true; }
    g_lg4ff_range = g_ctl.range_default;
#endif
}

/* key.<action>=<key>,<key>...: the action's keys, the rest of its slots cleared. */
static inline void controls_key_line(int a, const char *v) {
    char buf[64];
    snprintf(buf, sizeof buf, "%s", v);
    int s = 0;
    for (char *tok = strtok(buf, ","); tok && s < INPUT_KEY_SLOTS; tok = strtok(NULL, ",")) {
        int kc = input_keycode_by_name(tok);
        if (kc > 0) g_input_keys[a][s++] = kc;
    }
    while (s < INPUT_KEY_SLOTS) g_input_keys[a][s++] = 0;
}

#if M2HLE_PADS
/* wheel.rest=<accel>,<brake>: each a raw axis value or auto. */
static inline void controls_wheel_rest(const char *v) {
    char buf[64];
    snprintf(buf, sizeof buf, "%s", v);
    char *b = strchr(buf, ',');
    if (!b) return;
    *b++ = '\0';
    const char *r[2] = { buf, b };
    for (int i = 0; i < 2; i++) {
        g_wheel_opt.rest_set[i] = strcmp(r[i], "auto") != 0;
        g_wheel_opt.rest[i] = g_wheel_opt.rest_set[i] ? atoi(r[i]) : 0;
    }
}

/* A wheel.* line: its device, pedals, axes, centre, turn, or a button's actions. */
static inline void controls_wheel_line(const char *k, const char *v) {
    if (!strcmp(k, "wheel.device")) {
        pad_pick_parse(&g_wheel_opt.dev, v);
    } else if (!strcmp(k, "wheel.pedals")) {
        pad_pick_parse(&g_wheel_opt.pedals, v);
    } else if (!strcmp(k, "wheel.rest")) {
        controls_wheel_rest(v);
    } else if (!strcmp(k, "wheel.axes")) {
        int st, ac, br;
        if (sscanf(v, "%d,%d,%d", &st, &ac, &br) == 3 && st >= 0 && ac >= 0 && br >= 0) {
            g_wheel_opt.steer = st; g_wheel_opt.accel = ac; g_wheel_opt.brake = br;
        }
    } else if (!strcmp(k, "wheel.center")) {
        g_wheel_opt.center = atoi(v);
        g_wheel_opt.center_set = true;
    } else if (!strcmp(k, "wheel.range")) {
        int r = atoi(v);
        g_lg4ff_range = r <= 0 ? 0 : lg4ff_clamp_range(r);
    } else {
        uint32_t acts;
        int b = atoi(k + 6);
        if (b >= 0 && b < WHEEL_BUTTONS && actions_by_name(v, &acts)) g_wheel_opt.button[b] = acts;
    }
}
#endif

/* One line of a controls file. */
static inline void controls_apply_line(char *line) {
    char *eq = strchr(line, '=');
    if (line[0] == '#' || !eq) return;
    *eq = '\0';
    const char *k = line, *v = eq + 1;
    if (!strncmp(k, "key.", 4)) {
        int a = input_action_by_name(k + 4);
        if (a >= 0) controls_key_line(a, v);
        return;
    }
#if M2HLE_PADS
    if (!strcmp(k, "device")) {
        for (int d = 0; d < PAD_DEVICE_COUNT; d++) if (!strcmp(v, g_pad_device_names[d])) g_pad_device = d;
    } else if (!strcmp(k, "pad.device")) {
        pad_pick_parse(&g_pad_pick, v);
    } else if (!strncmp(k, "pad.", 4)) {
        uint32_t acts;
        for (int b = 0; b < PAD_BIND_COUNT; b++)
            if (!strcmp(k + 4, g_pad_binds[b].name) && actions_by_name(v, &acts)) g_pad_binds[b].acts = acts;
    } else if (!strncmp(k, "wheel.", 6)) {
        controls_wheel_line(k, v);
    }
#endif
}

/* Profile p's controls in: the built-in map, its file over that, the command
 * line over both. */
static inline void controls_load(const game_profile_t *p) {
#if M2HLE_PADS
    int range = g_lg4ff_range;
#endif
    controls_defaults(p);
    g_ctl.loaded = p;
    char path[640], line[320];
    FILE *f = controls_path(p, path, sizeof path) ? fopen(path, "r") : NULL;
    if (f) {
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = '\0';
            controls_apply_line(line);
        }
        fclose(f);
        LOG_INFO("controls: %s", path);
    }
#if M2HLE_PADS
    for (int i = 0; i < g_ctl.cli_n; i++) sdl_pads_option(g_ctl.cli[i].opt, g_ctl.cli[i].val);
    wheel_preset_apply();   /* a G25's own layout, if nothing changed the built-in one */
    if (g_lg4ff_range && g_lg4ff_range != range) lg4ff_set_range(g_lg4ff_range);   /* this profile's turn */
#endif
}

static inline void controls_save_keys(FILE *f) {
    char name[16];
    for (int a = 0; a < GAME_INPUT_COUNT; a++) {
        fprintf(f, "key.%s=", g_input_action_names[a]);
        int n = 0;
        for (int s = 0; s < INPUT_KEY_SLOTS; s++)
            if (g_input_keys[a][s] && input_keycode_name(g_input_keys[a][s], name))
                fprintf(f, "%s%s", n++ ? "," : "", name);
        fprintf(f, "%s\n", n ? "" : "none");
    }
}

#if M2HLE_PADS
static inline void controls_save_pick(FILE *f, const char *key, const pad_pick_t *p) {
    char pick[112];
    if (!p->guid[0]) return;
    pad_pick_format(p, pick, sizeof pick);
    fprintf(f, "%s=%s\n", key, pick);
}

static inline void controls_save_wheel(FILE *f) {
    char acts[64];
    fprintf(f, "wheel.axes=%d,%d,%d\n", g_wheel_opt.steer, g_wheel_opt.accel, g_wheel_opt.brake);
    controls_save_pick(f, "pad.device", &g_pad_pick);
    controls_save_pick(f, "wheel.device", &g_wheel_opt.dev);
    controls_save_pick(f, "wheel.pedals", &g_wheel_opt.pedals);
    if (g_wheel_opt.rest_set[0] || g_wheel_opt.rest_set[1]) {
        fprintf(f, "wheel.rest=");
        for (int i = 0; i < 2; i++)
            if (g_wheel_opt.rest_set[i]) fprintf(f, "%s%d", i ? "," : "", g_wheel_opt.rest[i]);
            else                         fprintf(f, "%sauto", i ? "," : "");
        fprintf(f, "\n");
    }
    if (g_wheel_opt.center_set) fprintf(f, "wheel.center=%d\n", g_wheel_opt.center);
    if (g_lg4ff_range != g_ctl.range_default) fprintf(f, "wheel.range=%d\n", g_lg4ff_range);
    for (int b = 0; b < WHEEL_BUTTONS; b++)
        if (g_wheel_opt.button[b])
            fprintf(f, "wheel.%d=%s\n", b, input_combo_format(g_wheel_opt.button[b], acts, sizeof acts));
}
#endif

static inline void controls_save(void) {
    char path[640];
    if (!controls_path(g_ctl.loaded, path, sizeof path)) return;
    FILE *f = fopen(path, "w");
    if (!f) {
        LOG_WARN("controls: cannot write %s", path);
        snprintf(g_ctl.status, sizeof g_ctl.status, "Cannot write the controls file (see m2hle.log).");
        return;
    }
    fprintf(f, "# m2hle controls for %s\n", g_ctl.loaded->display_name);
#if M2HLE_PADS
    fprintf(f, "device=%s\n", g_pad_device_names[g_pad_device]);
#endif
    controls_save_keys(f);
#if M2HLE_PADS
    char acts[64];
    for (int b = 0; b < PAD_BIND_COUNT; b++)
        fprintf(f, "pad.%s=%s\n", g_pad_binds[b].name, input_combo_format(g_pad_binds[b].acts, acts, sizeof acts));
    controls_save_wheel(f);
#endif
    fclose(f);
}

/* Before the window changes anything: the command line's pad options were for
 * this run's start, and what is saved is the file's map with this change. */
static inline void controls_edit_begin(void) {
    if (!g_ctl.cli_n) return;
    g_ctl.cli_n = 0;
    controls_load(g_ctl.loaded);
}

/* The pads' presses again after a change, and the force off the ones that
 * stopped playing (ffb_update gives it back to the rest). */
static inline void controls_pads_changed(bool pads_on) {
#if M2HLE_PADS
    if (!pads_on) return;
    pad_refresh();
    ffb_stop_all();
#else
    (void)pads_on;
#endif
}

/* Once a frame: the active profile's controls, loaded when it changes. */
static inline void controls_frame(bool pads_on) {
    if (!g_active_profile || g_active_profile == g_ctl.loaded) return;
    g_ctl.cap_action = -1;
    controls_load(g_active_profile);
    controls_pads_changed(pads_on);
}

/* A key event: true when the window took it (a key being bound), and the game
 * and the shortcuts must not see it. */
static inline bool controls_key_event(const sapp_event *ev) {
    if (ev->type == SAPP_EVENTTYPE_KEY_UP && g_ctl.swallow && (int)ev->key_code == g_ctl.swallow) {
        g_ctl.swallow = 0;
        return true;
    }
    if (g_ctl.cap_action < 0) return false;
    if (ev->type != SAPP_EVENTTYPE_KEY_DOWN) return false;
    int kc = (int)ev->key_code, a = g_ctl.cap_action;
    g_ctl.swallow = kc;
    g_ctl.cap_action = -1;
    if (kc == SAPP_KEYCODE_ESCAPE) return true;
    char name[16];
    if (!input_keycode_name(kc, name)) {
        snprintf(g_ctl.status, sizeof g_ctl.status, "That key cannot be bound.");
        return true;
    }
    controls_edit_begin();
    g_ctl.status[0] = '\0';
    /* A key plays one action: it leaves the one it had. */
    for (int o = 0; o < GAME_INPUT_COUNT; o++)
        for (int s = 0; s < INPUT_KEY_SLOTS; s++)
            if (g_input_keys[o][s] == kc && !(o == a && s == g_ctl.cap_slot)) {
                g_input_keys[o][s] = 0;
                snprintf(g_ctl.status, sizeof g_ctl.status, "%s was %s%s's; it is not any more.", name,
                         o < GAME_INPUT_P2_UP ? "P1 " : o < GAME_INPUT_SERVICE ? "P2 " : "",
                         g_ctl_action_label[o]);
            }
    g_input_keys[a][g_ctl.cap_slot] = kc;
    controls_save();
    return true;
}

/* ---- The window ---------------------------------------------------------- */

/* Which of a driving game's view buttons (1-4) action a presses; 0 if none. */
static inline int controls_view_button(int a) {
    const game_profile_t *p = g_active_profile;
    if (!p || !p->input.bits[a]) return 0;
    for (int v = 0; v < 4; v++)
        if (p->quirks.wheel.view[v] && p->input.bits[a] == p->quirks.wheel.view[v]) return v + 1;
    return 0;
}

/* A player-2 action a pad or wheel button can press: only a view button
 * (Daytona's VR3 / VR4). */
static inline bool controls_p2_choice(uint32_t acts) {
    for (int a = GAME_INPUT_P2_UP; a <= GAME_INPUT_P2_COIN; a++)
        if ((acts & (1u << a)) && !controls_view_button(a)) return false;
    return true;
}

/* An action's row label, with what a driving game makes of it. */
static inline void controls_action_label(int a, char *buf, size_t cap) {
    const game_profile_t *p = g_active_profile;
    const char *what = NULL;
    if (p && (p->quirks.io_5649 || p->quirks.model1_io) && p->input.bits[a]) {
        uint32_t bit = p->input.bits[a];
        if (bit == p->quirks.wheel.accel)           what = "accelerator";
        else if (bit == p->quirks.wheel.brake)      what = "brake";
        else if (bit == p->quirks.wheel.shift_up)   what = "shift up";
        else if (bit == p->quirks.wheel.shift_down) what = "shift down";
        else if (bit == p->quirks.wheel.left)       what = "steer left";
        else if (bit == p->quirks.wheel.right)      what = "steer right";
        int v = controls_view_button(a);
        if (v) {
            snprintf(buf, cap, p->quirks.wheel.view[1] ? "%s (view %d)" : "%s (view)", g_ctl_action_label[a], v);
            return;
        }
    }
    if (what) snprintf(buf, cap, "%s (%s)", g_ctl_action_label[a], what);
    else      snprintf(buf, cap, "%s", g_ctl_action_label[a]);
}

static inline void controls_keys_rows(int first, int last) {
    char label[48], name[16], id[32];
    for (int a = first; a <= last; a++) {
        igTableNextRow();
        igTableSetColumnIndex(0);
        controls_action_label(a, label, sizeof label);
        bool unused = g_active_profile && !g_active_profile->input.bits[a];
        if (unused) igTextDisabled("%s", label); else igTextUnformatted(label);
        for (int s = 0; s < INPUT_KEY_SLOTS; s++) {
            igTableSetColumnIndex(1 + s);
            const char *shown = "-";
            if (g_ctl.cap_action == a && g_ctl.cap_slot == s) shown = "press a key...";
            else if (g_input_keys[a][s] && input_keycode_name(g_input_keys[a][s], name)) {
                for (char *c = name; *c; c++) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
                shown = name;
            }
            snprintf(id, sizeof id, "%s##k%d_%d", shown, a, s);
            if (igButtonEx(id, (ImVec2){ -1.0f, 0.0f })) { g_ctl.cap_action = a; g_ctl.cap_slot = s; g_ctl.status[0] = '\0'; }
            if (igIsItemClickedEx(ImGuiMouseButton_Right) && g_input_keys[a][s]) {
                controls_edit_begin();
                g_input_keys[a][s] = 0;
                controls_save();
            }
        }
    }
}

static inline void controls_tab_keyboard(void) {
    igTextWrapped("Click a key, then press the one you want (Esc cancels). Right-click clears it. "
                  "A gamepad's or a wheel's buttons are set in their own tabs.");
    if (!igBeginTableEx("keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV, (ImVec2){ 0, 0 }, 0.0f)) return;
    igTableSetupColumnEx("Action", ImGuiTableColumnFlags_WidthStretch, 1.4f, 0);
    igTableSetupColumnEx("Key", ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
    igTableSetupColumnEx("Second key", ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
    igTableHeadersRow();
    igTableNextRow(); igTableSetColumnIndex(0); igSeparatorText("Player 1");
    controls_keys_rows(GAME_INPUT_P1_UP, GAME_INPUT_P1_COIN);
    igTableNextRow(); igTableSetColumnIndex(0); igSeparatorText("Player 2");
    controls_keys_rows(GAME_INPUT_P2_UP, GAME_INPUT_P2_COIN);
    igTableNextRow(); igTableSetColumnIndex(0); igSeparatorText("Cabinet");
    controls_keys_rows(GAME_INPUT_SERVICE, GAME_INPUT_TEST);
    if (g_active_profile && g_active_profile->input.bits[GAME_INPUT_GEAR_N]) {
        igTableNextRow(); igTableSetColumnIndex(0); igSeparatorText("Gears");
        controls_keys_rows(GAME_INPUT_GEAR_N, GAME_INPUT_GEAR_4);
    }
    igEndTable();
}

#if M2HLE_PADS
/* What a pad button or wheel button can press: one player-1 action, the
 * cabinet's two, a racing cabinet's gears, or the usual button combos. */
static const uint32_t g_ctl_pad_choices[] = {
    0, ACT(GAME_INPUT_P1_B1), ACT(GAME_INPUT_P1_B2), ACT(GAME_INPUT_P1_B3), ACT(GAME_INPUT_P1_B4),
    ACT(GAME_INPUT_P1_START), ACT(GAME_INPUT_P1_COIN),
    ACT(GAME_INPUT_P1_UP), ACT(GAME_INPUT_P1_DOWN), ACT(GAME_INPUT_P1_LEFT), ACT(GAME_INPUT_P1_RIGHT),
    ACT(GAME_INPUT_SERVICE), ACT(GAME_INPUT_TEST),
    ACT(GAME_INPUT_GEAR_N), ACT(GAME_INPUT_GEAR_1), ACT(GAME_INPUT_GEAR_2), ACT(GAME_INPUT_GEAR_3), ACT(GAME_INPUT_GEAR_4),
    ACT(GAME_INPUT_P1_B1) | ACT(GAME_INPUT_P1_B2), ACT(GAME_INPUT_P1_B1) | ACT(GAME_INPUT_P1_B3),
    ACT(GAME_INPUT_P1_B2) | ACT(GAME_INPUT_P1_B3), ACT(GAME_INPUT_P1_B1) | ACT(GAME_INPUT_P1_B2) | ACT(GAME_INPUT_P1_B3),
    ACT(GAME_INPUT_P1_B3) | ACT(GAME_INPUT_P1_B4),
    ACT(GAME_INPUT_P2_B3), ACT(GAME_INPUT_P2_B4),   /* Daytona's VR3 / VR4 (controls_p2_choice) */
};
#define CTL_PAD_CHOICES (int)(sizeof g_ctl_pad_choices / sizeof g_ctl_pad_choices[0])

/* acts in words: "Button 1 + Button 2", "none". */
static inline const char *controls_acts_text(uint32_t acts, char *buf, size_t cap) {
    if (!acts) { snprintf(buf, cap, "none"); return buf; }
    size_t n = 0;
    buf[0] = '\0';
    for (int a = 0; a < GAME_INPUT_COUNT && n < cap; a++)
        if (acts & ACT(a)) {
            char label[48];
            controls_action_label(a, label, sizeof label);
            n += (size_t)snprintf(buf + n, cap - n, "%s%s", n ? " + " : "", label);
        }
    return buf;
}

/* The action picker of one pad or wheel button; true when it changed *acts. */
static inline bool controls_acts_combo(const char *id, uint32_t *acts) {
    char cur[96], item[96];
    bool changed = false;
    igSetNextItemWidth(-1.0f);
    if (!igBeginCombo(id, controls_acts_text(*acts, cur, sizeof cur), 0)) return false;
    for (int c = 0; c < CTL_PAD_CHOICES; c++) {
        if (!controls_p2_choice(g_ctl_pad_choices[c])) continue;
        if (igSelectableEx(controls_acts_text(g_ctl_pad_choices[c], item, sizeof item),
                           *acts == g_ctl_pad_choices[c], 0, (ImVec2){ 0, 0 })) {
            controls_edit_begin();
            *acts = g_ctl_pad_choices[c];
            changed = true;
        }
    }
    igEndCombo();
    return changed;
}

/* The first gamepad and the first wheel that play here, for the live rows. */
static inline SDL_Gamepad *controls_first_pad(void) {
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    SDL_Gamepad *pad = NULL;
    for (int i = 0, g = 0; ids && i < count && !pad; i++)
        if (gamepad_is(ids[i]) && pad_plays_here(g++)) pad = SDL_GetGamepadFromID(ids[i]);
    SDL_free(ids);
    return pad;
}

static inline SDL_Joystick *controls_first_wheel(void) {
    int count = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    SDL_Joystick *joy = NULL;
    for (int i = 0, w = 0; ids && i < count && !joy; i++)
        if (wheel_is(ids[i]) && pad_plays_here(w++)) joy = SDL_GetJoystickFromID(ids[i]);
    SDL_free(ids);
    return joy;
}

/* What SDL takes a joystick for. */
static inline const char *controls_joy_kind(SDL_JoystickID id) {
    if (SDL_IsGamepad(id)) return "gamepad";
    if (joy_is_racing_wheel(id)) return "wheel";
    switch (SDL_GetJoystickTypeForID(id)) {
        case SDL_JOYSTICK_TYPE_THROTTLE: return "throttle";
        case SDL_JOYSTICK_TYPE_FLIGHT_STICK: return "flight stick";
        default:                         return "joystick";
    }
}

/* A device picker: none_label, then the connected devices (gamepads only, or
 * every joystick), and the picked one while it is unplugged; true when *p
 * changed. */
static inline bool controls_pick_combo(const char *label, pad_pick_t *p, const char *none_label, bool gamepads) {
    char cur[112];
    SDL_JoystickID now = pad_pick_find(p);
    if (!p->guid[0]) snprintf(cur, sizeof cur, "%s", none_label);
    else snprintf(cur, sizeof cur, "%s%s", p->name, now ? "" : " (not connected)");
    bool changed = false;
    igSetNextItemWidth(300.0f);
    if (!igBeginCombo(label, cur, 0)) return false;
    if (igSelectableEx(none_label, !p->guid[0], 0, (ImVec2){ 0, 0 }) && p->guid[0]) {
        controls_edit_begin();
        memset(p, 0, sizeof *p);
        changed = true;
    }
    int count = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    for (int i = 0; ids && i < count; i++) {
        if (gamepads && !SDL_IsGamepad(ids[i])) continue;
        const char *n = SDL_GetJoystickNameForID(ids[i]);
        char item[128];
        snprintf(item, sizeof item, "%s (%s)##%u", n ? n : "?", controls_joy_kind(ids[i]), (unsigned)ids[i]);
        if (igSelectableEx(item, pad_pick_is(p, ids[i]), 0, (ImVec2){ 0, 0 }) && !pad_pick_is(p, ids[i])) {
            controls_edit_begin();
            pad_pick_set(p, ids[i]);
            changed = true;
        }
    }
    SDL_free(ids);
    if (p->guid[0] && !now) igSelectableEx(cur, true, ImGuiSelectableFlags_Disabled, (ImVec2){ 0, 0 });
    igEndCombo();
    return changed;
}

/* What a connected device does this run, for the Devices list. */
static inline const char *controls_joy_role(SDL_JoystickID id) {
    if (wheel_is(id)) return pad_device_takes(true) ? "the wheel" : "the wheel, not playing";
    if (pedals_is(id)) return pad_device_takes(true) ? "the pedals" : "the pedals, not playing";
    if (gamepad_is(id)) return pad_device_takes(false) ? "a gamepad" : "a gamepad, not playing";
    return SDL_IsGamepad(id) ? "a gamepad, not picked" : "unused: pick it in the Wheel tab";
}

static inline void controls_device_combo(bool pads_on) {
    static const char *const label[PAD_DEVICE_COUNT] = {
        "Keyboard + any gamepad or wheel", "Keyboard only", "Keyboard + gamepad", "Keyboard + racing wheel",
    };
    igSetNextItemWidth(260.0f);
    if (!igBeginCombo("Devices", label[g_pad_device], 0)) return;
    for (int d = 0; d < PAD_DEVICE_COUNT; d++)
        if (igSelectableEx(label[d], g_pad_device == d, 0, (ImVec2){ 0, 0 }) && g_pad_device != d) {
            controls_edit_begin();
            g_pad_device = d;
            controls_pads_changed(pads_on);
            controls_save();
        }
    igEndCombo();
}

static inline void controls_devices(bool pads_on) {
    controls_device_combo(pads_on);
    if (!pads_on) { igTextDisabled("No gamepads or wheels this run (--no-pads, capture mode, or SDL failed)."); return; }
    int count = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    if (!count) igTextDisabled("No gamepad or wheel is connected.");
    for (int i = 0; ids && i < count; i++) {
        SDL_JoystickID id = ids[i];
        const char *name = SDL_GetJoystickNameForID(id);
        igBulletText("%s (%s) - %s", name ? name : "?", controls_joy_kind(id), controls_joy_role(id));
    }
    SDL_free(ids);
}

static inline void controls_tab_gamepad(bool pads_on) {
    igTextWrapped("The d-pad and the left stick are player 1's directions; a driving game's wheel is "
                  "the stick, R2 the accelerator and L2 the brake. A held button lights its row.");
    if (pads_on && controls_pick_combo("Gamepad", &g_pad_pick, "Any gamepad", true)) {
        controls_pads_changed(pads_on);
        controls_save();
    }
    SDL_Gamepad *pad = pads_on ? controls_first_pad() : NULL;
    if (!igBeginTableEx("padbinds", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV, (ImVec2){ 0, 0 }, 0.0f)) return;
    igTableSetupColumnEx("Button", ImGuiTableColumnFlags_WidthFixed, 90.0f, 0);
    igTableSetupColumnEx("Presses", ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
    igTableHeadersRow();
    char id[32];
    for (int b = 0; b < PAD_BIND_COUNT; b++) {
        igTableNextRow();
        if (pad && SDL_GetGamepadButton(pad, g_pad_binds[b].button))
            igTableSetBgColor(ImGuiTableBgTarget_RowBg1, 0x6000C000u, -1);
        igTableSetColumnIndex(0);
        igTextUnformatted(g_pad_binds[b].name);
        igTableSetColumnIndex(1);
        snprintf(id, sizeof id, "##pad%d", b);
        if (controls_acts_combo(id, &g_pad_binds[b].acts)) { controls_pads_changed(pads_on); controls_save(); }
    }
    igEndTable();
}

/* A Detect button's wait: the axis of the watched device that moved
 * furthest from where it was when the button was clicked, once one has
 * moved past a sixth of its range; a pedal's released position is where it
 * was then. */
static inline void controls_detect_poll(bool pads_on) {
    if (g_ctl.det_role < 0) return;
    SDL_Joystick *joy = SDL_GetJoystickFromID((SDL_JoystickID)g_ctl.det_id);
    if (!joy) { g_ctl.det_role = -1; return; }
    int n = SDL_GetNumJoystickAxes(joy), best = -1, most = 12000;
    for (int k = 0; k < n && k < 16; k++) {
        int d = abs(SDL_GetJoystickAxis(joy, k) - g_ctl.det_start[k]);
        if (d > most) { most = d; best = k; }
    }
    if (best < 0) return;
    controls_edit_begin();
    int r = g_ctl.det_role;
    int *ax[3] = { &g_wheel_opt.steer, &g_wheel_opt.accel, &g_wheel_opt.brake };
    *ax[r] = best;
    if (r) { g_wheel_opt.rest[r - 1] = g_ctl.det_start[best]; g_wheel_opt.rest_set[r - 1] = true; }
    snprintf(g_ctl.status, sizeof g_ctl.status, "%s is axis %d.", r == 0 ? "The wheel" : r == 1 ? "The accelerator" : "The brake", best);
    g_ctl.det_role = -1;
    controls_pads_changed(pads_on);
    controls_save();
}

/* A Logitech wheel's rotation (lg4ff.h), sent to the wheel when it changes
 * and kept per profile. On Linux SDL's own driver gives it 900 degrees unless
 * m2hle can reach the wheel (Pinboard #1439). */
static inline void controls_wheel_range(SDL_Joystick *joy) {
    bool logi = (joy && SDL_GetJoystickVendor(joy) == LOGITECH_VID) || lg4ff_open_name() || lg4ff_locked_out();
    if (!logi) return;
    if (!g_lg4ff_on) { igTextDisabled("Logitech wheels are left alone this run (--g25 off)."); return; }
    if (!g_ctl.range_held) g_ctl.range_drag = g_lg4ff_range ? g_lg4ff_range : LG4FF_RANGE_MAX;
    igSetNextItemWidth(200.0f);
    igSliderInt("Rotation (degrees)", &g_ctl.range_drag, LG4FF_RANGE_MIN, LG4FF_RANGE_MAX);
    g_ctl.range_held = igIsItemActive();
    if (igIsItemDeactivatedAfterEdit()) {
        controls_edit_begin();
        if (lg4ff_set_range(g_ctl.range_drag))     snprintf(g_ctl.status, sizeof g_ctl.status, "The wheel turns %d degrees now.", g_lg4ff_range);
        else if (lg4ff_locked_out())  snprintf(g_ctl.status, sizeof g_ctl.status, "Kept, but m2hle cannot reach the wheel (see below).");
        else                          snprintf(g_ctl.status, sizeof g_ctl.status, "Kept: the wheel gets it when it comes.");
        controls_save();
    }
    igSameLine();
    igTextDisabled("the cabinets': 270");
    if (lg4ff_locked_out())
        igTextColored((ImVec4){ 1, 0.8f, 0.3f, 1 }, "m2hle cannot open the wheel's hidraw node, so its rotation stays\n"
                      "Linux's (900). Add the udev rule in LOGITECH.md, plug the wheel in\nagain and restart m2hle.");
}

/* What the board reads from the wheel and pedals now, and what can steer it
 * besides the wheel. */
static inline void controls_wheel_readout(void) {
    if (!wheel_host_on()) {
        igTextDisabled("This game has no analog wheel: turned past half way, the wheel presses Left or Right.");
        return;
    }
    if (!io5649_host_live()) { igTextDisabled("A session, a replay or a follow drives the board's wheel now."); return; }
    io5649_wheel_t w = io5649_wheel();
    int steer = g_io5649.steer;
    igTextWrapped("The game reads: wheel 0x%02X (straight 0x%02X, %+d), accelerator %d%%, brake %d%%", steer, w.center,
           steer - w.center, g_io5649_host.accel * 100 / IO5649_AXIS_MAX, g_io5649_host.brake * 100 / IO5649_AXIS_MAX);
    if (g_pad_held[GAME_INPUT_P1_LEFT] || g_pad_held[GAME_INPUT_P1_RIGHT])
        igTextColored((ImVec4){ 1, 0.8f, 0.3f, 1 }, "A pad or wheel button holds %s: with the wheel straight, that turns it.",
                      g_pad_held[GAME_INPUT_P1_LEFT] ? "Left" : "Right");
}

/* The actions a wheel's buttons are set for, one row each. */
static const int g_ctl_wheel_acts[] = {
    GAME_INPUT_P1_START, GAME_INPUT_P1_COIN, GAME_INPUT_P1_B1, GAME_INPUT_P1_B2, GAME_INPUT_P1_B3, GAME_INPUT_P1_B4,
    GAME_INPUT_P2_B3, GAME_INPUT_P2_B4,
    GAME_INPUT_P1_UP, GAME_INPUT_P1_DOWN, GAME_INPUT_P1_LEFT, GAME_INPUT_P1_RIGHT, GAME_INPUT_SERVICE, GAME_INPUT_TEST,
    GAME_INPUT_GEAR_N, GAME_INPUT_GEAR_1, GAME_INPUT_GEAR_2, GAME_INPUT_GEAR_3, GAME_INPUT_GEAR_4,
};

static inline uint64_t controls_wheel_held(SDL_Joystick *joy) {
    uint64_t m = 0;
    for (int b = 0; joy && b < WHEEL_BUTTONS; b++) if (wheel_button_down(joy, b)) m |= 1ull << b;
    return m;
}

/* "3", or the d-pad's "D-pad up" (WHEEL_HAT). */
static inline const char *controls_wheel_button_name(int b, char *buf, size_t cap) {
    static const char *const dir[4] = { "up", "right", "down", "left" };
    if (b < WHEEL_HAT) snprintf(buf, cap, "%d", b);
    else               snprintf(buf, cap, "D-pad %s", dir[b - WHEEL_HAT]);
    return buf;
}

/* A Set button's wait: the first wheel button pressed after it was clicked
 * plays its action, and only that. */
static inline void controls_wheel_learn_poll(SDL_Joystick *joy, bool pads_on) {
    if (g_ctl.learn_act < 0) return;
    if (!joy) { g_ctl.learn_act = -1; return; }
    uint64_t held = controls_wheel_held(joy), fresh = held & ~g_ctl.learn_held;
    g_ctl.learn_held &= held;   /* let go: it counts when pressed again */
    if (!fresh) return;
    int b = 0, a = g_ctl.learn_act;
    while (!(fresh & (1ull << b))) b++;
    char lab[48], was[96], bn[16];
    controls_wheel_button_name(b, bn, sizeof bn);
    controls_edit_begin();
    uint32_t old = g_wheel_opt.button[b];
    g_wheel_opt.button[b] = ACT(a);
    controls_action_label(a, lab, sizeof lab);
    if (old && old != ACT(a)) snprintf(g_ctl.status, sizeof g_ctl.status, "Button %s is %s now (it was %s).", bn, lab,
                                       controls_acts_text(old, was, sizeof was));
    else                      snprintf(g_ctl.status, sizeof g_ctl.status, "Button %s is %s now.", bn, lab);
    g_ctl.learn_act = -1;
    controls_pads_changed(pads_on);
    controls_save();
}

/* Action a's wheel buttons as text into list; *lit if one is held. */
static inline size_t controls_wheel_act_list(int a, uint64_t held, char *list, size_t cap, bool *lit) {
    char bn[16];
    size_t n = 0;
    *lit = false;
    list[0] = '\0';
    for (int b = 0; b < WHEEL_BUTTONS && n < cap; b++)
        if (g_wheel_opt.button[b] & ACT(a)) {
            n += (size_t)snprintf(list + n, cap - n, "%s%s", n ? ", " : "",
                                  controls_wheel_button_name(b, bn, sizeof bn));
            *lit |= (held >> b) & 1;
        }
    return n;
}

/* Action a's Set button (right-click clears its buttons; n of them). */
static inline void controls_wheel_act_set(SDL_Joystick *joy, int a, const char *label, uint64_t held, size_t n,
                                          bool pads_on) {
    if (g_ctl.learn_act == a) {
        if (igButtonEx("Press a button...", (ImVec2){ -1.0f, 0.0f })) g_ctl.learn_act = -1;
    } else if (igButtonEx("Set", (ImVec2){ -1.0f, 0.0f })) {
        if (joy) {
            g_ctl.learn_act = a;
            g_ctl.learn_held = held;
            g_ctl.det_role = -1;
            snprintf(g_ctl.status, sizeof g_ctl.status, "Press the wheel's button for %s.", label);
        } else snprintf(g_ctl.status, sizeof g_ctl.status, "No wheel is connected.");
    }
    if (igIsItemClickedEx(ImGuiMouseButton_Right) && n) {
        controls_edit_begin();
        for (int b = 0; b < WHEEL_BUTTONS; b++) g_wheel_opt.button[b] &= ~ACT(a);
        controls_pads_changed(pads_on);
        controls_save();
    }
}

/* One action's row: its name, its wheel buttons, Set. */
static inline void controls_wheel_act_row(SDL_Joystick *joy, int a, uint64_t held, bool pads_on) {
    char label[48], list[96];
    bool lit;
    size_t n = controls_wheel_act_list(a, held, list, sizeof list, &lit);
    igTableNextRow();
    if (lit) igTableSetBgColor(ImGuiTableBgTarget_RowBg1, 0x6000C000u, -1);
    igTableSetColumnIndex(0);
    controls_action_label(a, label, sizeof label);
    if (g_active_profile && !g_active_profile->input.bits[a]) igTextDisabled("%s", label);
    else igTextUnformatted(label);
    igTableSetColumnIndex(1);
    igTextUnformatted(n ? list : "-");
    igTableSetColumnIndex(2);
    igPushIDInt(a);
    controls_wheel_act_set(joy, a, label, held, n, pads_on);
    igPopID();
}

static inline void controls_wheel_actions(SDL_Joystick *joy, bool pads_on) {
    controls_wheel_learn_poll(joy, pads_on);
    igSeparatorText("Buttons");
    igTextWrapped("Set: click it, then press the wheel's button. Right-click clears the action's buttons.");
    if (!igBeginTableEx("wheelacts", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV, (ImVec2){ 0, 0 }, 0.0f)) return;
    igTableSetupColumnEx("Action", ImGuiTableColumnFlags_WidthStretch, 1.4f, 0);
    igTableSetupColumnEx("Wheel buttons", ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
    igTableSetupColumnEx("##set", ImGuiTableColumnFlags_WidthFixed, 150.0f, 0);
    igTableHeadersRow();
    uint64_t held = controls_wheel_held(joy);
    bool gears = g_active_profile && g_active_profile->input.bits[GAME_INPUT_GEAR_N];
    for (int i = 0; i < (int)(sizeof g_ctl_wheel_acts / sizeof g_ctl_wheel_acts[0]); i++) {
        int a = g_ctl_wheel_acts[i];
        if (a >= GAME_INPUT_GEAR_N && !gears) continue;
        if (a >= GAME_INPUT_P2_UP && a <= GAME_INPUT_P2_COIN && !controls_view_button(a)) continue;
        controls_wheel_act_row(joy, a, held, pads_on);
    }
    igEndTable();
}

/* The Wheel and Pedals pickers. */
static inline void controls_wheel_picks(bool pads_on) {
    if (controls_pick_combo("Wheel", &g_wheel_opt.dev, "Any device SDL calls a wheel", false)) {
        g_ctl.det_role = -1;
        controls_pads_changed(pads_on);
        controls_save();
    }
    if (controls_pick_combo("Pedals", &g_wheel_opt.pedals, "The wheel's own pedals", false)) {
        g_ctl.det_role = -1;
        g_wheel_opt.rest_set[0] = g_wheel_opt.rest_set[1] = false;
        controls_pads_changed(pads_on);
        controls_save();
    }
}

/* The wheel's straight: where it reads held straight, if that is not the
 * axis's middle (the car steers by itself otherwise). */
static inline void controls_wheel_centre(int raw, bool pads_on) {
    igSameLine();
    if (igButton(g_wheel_opt.center_set ? "Centre*" : "Centre")) {
        controls_edit_begin();
        g_wheel_opt.center = raw;
        g_wheel_opt.center_set = true;
        snprintf(g_ctl.status, sizeof g_ctl.status, "The wheel is straight at %d.", raw);
        controls_pads_changed(pads_on);
        controls_save();
    }
    if (igIsItemClickedEx(ImGuiMouseButton_Right) && g_wheel_opt.center_set) {
        controls_edit_begin();
        g_wheel_opt.center_set = false;
        g_wheel_opt.center = 0;
        controls_pads_changed(pads_on);
        controls_save();
    }
    if (igIsItemHovered(0))
        igSetTooltip("Hold the wheel straight and click: that is where it steers straight.\n"
                     "Right-click: back to the axis's middle.");
}

/* Axis k's meter (0 the wheel, 1 and 2 the pedals) on device dj, id did. */
static inline void controls_wheel_meter(int k, SDL_Joystick *dj, SDL_JoystickID did, int axis, bool pads_on) {
    igSameLine();
    int raw = SDL_GetJoystickAxis(dj, axis);
    float f;
    if (k == 0)
        f = ((g_wheel_opt.center_set ? wheel_host_steer_centred(raw, g_wheel_opt.center)
                                     : wheel_host_steer(raw, 0)) + 32767.0f) / 65534.0f;
    else {
        int rest = g_wheel_opt.rest_set[k - 1] ? g_wheel_opt.rest[k - 1] : joy_rest(did, axis);
        f = wheel_host_pedal(raw, rest) / (float)IO5649_AXIS_MAX;
    }
    igProgressBar(f, (ImVec2){ 140.0f, 0 }, "");
    if (k == 0) controls_wheel_centre(raw, pads_on);
}

/* One axis row: its number, Detect, the meter. */
static inline void controls_wheel_axis(int k, SDL_Joystick *dj, SDL_JoystickID did, bool pads_on) {
    int *ax[3] = { &g_wheel_opt.steer, &g_wheel_opt.accel, &g_wheel_opt.brake };
    static const char *const ax_name[3] = { "Wheel axis", "Accelerator axis", "Brake axis" };
    int v = *ax[k];
    igPushIDInt(k);
    igSetNextItemWidth(120.0f);
    if (igInputIntEx(ax_name[k], &v, 1, 1, 0) && v >= 0 && v < 16) {
        controls_edit_begin();
        *ax[k] = v;
        if (k) g_wheel_opt.rest_set[k - 1] = false;
        controls_pads_changed(pads_on);
        controls_save();
    }
    /* SameLine only before an item, or the next axis joins this row. */
    if (g_ctl.det_role == k) {
        igSameLine();
        if (igButton("Waiting... (cancel)")) g_ctl.det_role = -1;
    } else if (dj && (igSameLine(), igButton("Detect"))) {
        g_ctl.det_role = k;
        g_ctl.det_id = (uint32_t)did;
        int n = SDL_GetNumJoystickAxes(dj);
        for (int a = 0; a < 16; a++) g_ctl.det_start[a] = a < n ? SDL_GetJoystickAxis(dj, a) : 0;
    }
    if (dj && *ax[k] < SDL_GetNumJoystickAxes(dj)) controls_wheel_meter(k, dj, did, *ax[k], pads_on);
    igPopID();
}

/* Each button's actions, the d-pad's after them. */
static inline void controls_wheel_binds(SDL_Joystick *joy, int nbut, bool pads_on) {
    int rows = nbut > 12 ? nbut : 12;
    if (rows > WHEEL_HAT) rows = WHEEL_HAT;
    bool hat = joy && SDL_GetNumJoystickHats(joy) > 0;
    if (!igBeginTableEx("wheelbinds", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                        (ImVec2){ 0, 260.0f }, 0.0f)) return;
    igTableSetupColumnEx("Button", ImGuiTableColumnFlags_WidthFixed, 90.0f, 0);
    igTableSetupColumnEx("Presses", ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
    igTableHeadersRow();
    char id[32];
    char bn[16];
    for (int b = 0; b < (hat ? WHEEL_BUTTONS : rows); b++) {
        if (b >= rows && b < WHEEL_HAT) b = WHEEL_HAT;   /* then the d-pad */
        igTableNextRow();
        if (joy && wheel_button_down(joy, b))
            igTableSetBgColor(ImGuiTableBgTarget_RowBg1, 0x6000C000u, -1);
        igTableSetColumnIndex(0);
        controls_wheel_button_name(b, bn, sizeof bn);
        if (b < WHEEL_HAT) igText("Button %s", bn); else igTextUnformatted(bn);
        igTableSetColumnIndex(1);
        snprintf(id, sizeof id, "##wheel%d", b);
        if (controls_acts_combo(id, &g_wheel_opt.button[b])) { controls_pads_changed(pads_on); controls_save(); }
    }
    igEndTable();
}

static inline void controls_tab_wheel(bool pads_on) {
    igTextWrapped("Pick the wheel, and the pedals if they have their own USB lead (they are a second "
                  "device then). Detect: click it, then turn the wheel or press the pedal all the way. "
                  "Centre: hold the wheel straight and click it.");
    if (!pads_on) { igTextDisabled("No wheels this run."); return; }
    controls_wheel_picks(pads_on);
    SDL_Joystick *joy = controls_first_wheel();
    SDL_JoystickID wid = joy ? SDL_GetJoystickID(joy) : 0, pid = wid;
    if (g_wheel_opt.pedals.guid[0] && !pad_pick_is(&g_wheel_opt.pedals, wid)) pid = pad_pick_find(&g_wheel_opt.pedals);
    SDL_Joystick *pj = pid ? SDL_GetJoystickFromID(pid) : NULL;
    int naxes = joy ? SDL_GetNumJoystickAxes(joy) : 0, nbut = joy ? SDL_GetNumJoystickButtons(joy) : 0;
    if (joy) igText("Wheel: %s (%d axes, %d buttons)", SDL_GetJoystickName(joy), naxes, nbut);
    else     igTextDisabled("No wheel is connected.");
    if (g_wheel_opt.pedals.guid[0] && !pj) igTextDisabled("The pedals are not connected.");
    controls_detect_poll(pads_on);
    for (int k = 0; k < 3; k++) controls_wheel_axis(k, k ? pj : joy, k ? pid : wid, pads_on);
    controls_wheel_range(joy);
    controls_wheel_readout();
    controls_wheel_actions(joy, pads_on);
    igSpacing();
    if (igCollapsingHeader("Each button's actions (combos)", 0)) controls_wheel_binds(joy, nbut, pads_on);
}
#endif

static inline void controls_tabs(bool pads_on) {
    (void)pads_on;
    if (!igBeginTabBar("ctltabs", 0)) return;
    if (igBeginTabItem("Keyboard", NULL, 0)) { controls_tab_keyboard(); igEndTabItem(); }
#if M2HLE_PADS
    if (igBeginTabItem("Gamepad", NULL, 0)) { controls_tab_gamepad(pads_on); igEndTabItem(); }
    if (igBeginTabItem("Wheel", NULL, 0))   { controls_tab_wheel(pads_on); igEndTabItem(); }
#endif
    igEndTabBar();
}

/* Reset to defaults: p's file goes, its built-in controls come back. */
static inline void controls_reset(const game_profile_t *p, bool pads_on) {
    char path[640];
    g_ctl.cli_n = 0;
    g_ctl.det_role = -1;
    g_ctl.learn_act = -1;
    if (controls_path(p, path, sizeof path)) remove(path);
    controls_load(p);
    controls_pads_changed(pads_on);
    snprintf(g_ctl.status, sizeof g_ctl.status, "Back to the built-in controls.");
}

static inline void controls_window_draw(bool *open, bool pads_on) {
    if (!*open) { g_ctl.cap_action = -1; g_ctl.learn_act = -1; return; }
    igSetNextWindowSize((ImVec2){ 520.0f, 620.0f }, ImGuiCond_FirstUseEver);
    if (!igBegin("Controls", open, 0)) { igEnd(); return; }
    const game_profile_t *p = g_ctl.loaded;
    char path[640];
    igText("Controls for %s", p ? p->display_name : "(no profile)");
    if (controls_path(p, path, sizeof path)) igTextDisabled("Saved in %s", path);
    if (g_ctl.cli_n) igTextColored((ImVec4){ 1, 0.8f, 0.3f, 1 }, "Command-line pad options apply; a change here drops them.");
#if M2HLE_PADS
    controls_devices(pads_on);
#else
    (void)pads_on;
    igTextDisabled("This build has the keyboard alone.");
#endif
    if (g_ctl.status[0]) igTextColored((ImVec4){ 1, 0.8f, 0.3f, 1 }, "%s", g_ctl.status);
    igSeparator();
    controls_tabs(pads_on);
    igSeparator();
    if (igButton("Reset to defaults") && p) controls_reset(p, pads_on);
    igEnd();
}

#endif /* CONTROLS_WINDOW_H */
