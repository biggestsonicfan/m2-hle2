/*
 * ps3ui_render -- render the PS3-style UI to PNG files, no GPU needed.
 *
 *   ps3ui_render sprites <dir>              every painted sprite, at its own size
 *   ps3ui_render vs <out.png> [w h [frame]] the VS lobby as in the reference capture
 *   ps3ui_render app <dir> [w h]            every online screen, off a made-up
 *                                           netplay status (no network)
 *   ps3ui_render shell <dir> [w h]          the offline shell: title, menus, pause
 *
 * tools/ps3ui/grade.py holds these against the PS3's own sprites and a capture
 * of the real screen (both kept outside the repo). Not a ctest.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NDEBUG 1
#include "net/netplay.h"
#include "miniz.h"
#include "registry.h"
#include "ps3ui_app.h"
#include "ps3ui_shell.h"

static int write_png(const char *path, const uint8_t *rgba, int w, int h)
{
    size_t len = 0;
    void *png = tdefl_write_image_to_png_file_in_memory_ex(rgba, w, h, 4, &len, 6, 0);
    if (!png)
        return -1;
    FILE *f = fopen(path, "wb");
    if (!f) {
        mz_free(png);
        return -1;
    }
    fwrite(png, 1, len, f);
    fclose(f);
    mz_free(png);
    return 0;
}

static int dump_sprites(const char *dir)
{
    for (int id = 0; id < PS3UI_SPR_ALL; id++) {
        const ps3ui_image_t *im = ps3ui_sprite(id);
        if (!im || !im->px)
            continue;
        uint8_t *rgba = (uint8_t *)malloc((size_t)im->w * (size_t)im->h * 4);
        for (int i = 0; i < im->w * im->h * 4; i++) {
            float v = im->px[i];
            v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
            rgba[i] = (uint8_t)(v * 255.0f + 0.5f);
        }
        char path[1024];
        const char *name = id < PS3UI_SPR_COUNT ? ps3ui_sprite_info[id].name : NULL;
        static const char *const extra[] = { "n_cmn_net_00", "n_cmn_net_01", "n_cmn_net_02", "n_cmn_net_03",
                                             "n_cmn_net_xx", "btn_circle", "btn_cross", "btn_triangle",
                                             "btn_square" };
        if (!name)
            name = extra[id - PS3UI_SPR_COUNT];
        snprintf(path, sizeof path, "%s/%s.png", dir, name);
        if (write_png(path, rgba, im->w, im->h))
            fprintf(stderr, "cannot write %s\n", path);
        free(rgba);
    }
    return 0;
}


/* ---- a fake netplay for the app screens -------------------------------------- */

static netplay_status_t g_fake;
static netplay_cmd_kind_t g_last_cmd;

static void fake_status(netplay_status_t *out) { *out = g_fake; }
static void fake_post(netplay_cmd_kind_t k, const netplay_config_t *cfg) { (void)cfg; g_last_cmd = k; }

static void add_member(const char *npid, int me, int8_t side, uint8_t flags, uint8_t entry, int rtt)
{
    netplay_member_status_t *m = &g_fake.members[g_fake.member_count];
    memset(m, 0, sizeof *m);
    m->member_id = (uint16_t)(16 + g_fake.member_count);
    snprintf(m->npid, sizeof m->npid, "%s", npid);
    m->is_me = me != 0;
    m->side = side;
    m->line_pos = (int8_t)g_fake.member_count;
    m->known = true;
    m->data.flags = flags;
    m->data.entry = entry;
    m->rtt_ms = rtt;
    if (me)
        g_fake.me = m->data;
    g_fake.member_count++;
}

static void run(ps3ui_app_t *a, int frames, uint32_t pad)
{
    for (int i = 0; i < frames; i++)
        ps3ui_app_frame(a, i == 0 ? pad : 0);
    if (pad && frames == 1)
        ps3ui_app_frame(a, 0);
}

static int shot(ps3ui_app_t *a, ps3ui_canvas_t *cv, const char *dir, const char *name)
{
    char path[1024];
    ps3ui_app_draw(a, cv);
    ps3ui_canvas_resolve(cv);
    snprintf(path, sizeof path, "%s/%s.png", dir, name);
    printf("%s\n", path);
    return write_png(path, cv->rgba, cv->w, cv->h);
}

static int dump_app(const char *dir, int w, int h)
{
    ps3ui_canvas_t cv = { 0 };
    ps3ui_canvas_size(&cv, w, h);
    ps3ui_backend_t be = { fake_status, fake_post, NULL };
    ps3ui_app_t *a = &g_ps3ui_app;
    ps3ui_app_init(a, be);
    memset(&g_fake, 0, sizeof g_fake);

    g_fake.state = NETPLAY_OFF;
    ps3ui_app_open(a);
    run(a, 40, 0);
    shot(a, &cv, dir, "01_signin");

    run(a, 1, PS3UI_PAD_DOWN);
    run(a, 1, PS3UI_PAD_DOWN);
    run(a, 1, PS3UI_PAD_RIGHT);             /* Server: from the official one to ours */
    run(a, 10, 0);
    shot(a, &cv, dir, "01b_signin_server");
    run(a, 1, PS3UI_PAD_UP);
    run(a, 1, PS3UI_PAD_UP);

    run(a, 1, PS3UI_PAD_DOWN);
    run(a, 1, PS3UI_PAD_CROSS);
    run(a, 40, 0);
    for (const char *p = "stftest"; *p; p++) {
        a->osk_text[0][strlen(a->osk_text[0]) + 1] = 0;
        a->osk_text[0][strlen(a->osk_text[0])] = *p;
    }
    shot(a, &cv, dir, "02_osk");

    g_fake.state = NETPLAY_CONNECTING;
    run(a, 40, 0);
    shot(a, &cv, dir, "03_accessing");

    g_fake.state = NETPLAY_ONLINE;
    run(a, 40, 0);
    shot(a, &cv, dir, "04_menu");

    run(a, 1, PS3UI_PAD_DOWN);
    run(a, 1, PS3UI_PAD_CROSS);             /* Custom Match -> search */
    g_fake.search_pending = true;
    run(a, 40, 0);
    shot(a, &cv, dir, "05_searching");

    static const char *const owners[] = { "saltyfreeman", "biggestsonic", "m2hletest", "stftest2" };
    for (int i = 0; i < 4; i++) {
        rpcn_room_listing_t *r = &g_fake.rooms[i];
        r->room_id = (uint64_t)(100 + i);
        r->cur_members = (uint16_t)(1 + i % 3);
        r->max_slots = (uint16_t)(i == 2 ? 8 : 2);
        snprintf(r->owner, sizeof r->owner, "%s", owners[i]);
        r->relay_ms = (uint32_t)(30 + 60 * i);
    }
    g_fake.room_count = 4;
    g_fake.search_pending = false;
    run(a, 40, 0);
    run(a, 1, PS3UI_PAD_DOWN);
    run(a, 20, 0);
    shot(a, &cv, dir, "06_search");

    run(a, 1, PS3UI_PAD_CIRCLE);
    run(a, 20, 0);
    run(a, 1, PS3UI_PAD_DOWN);
    run(a, 1, PS3UI_PAD_DOWN);
    run(a, 1, PS3UI_PAD_CROSS);             /* Create Match -> rule menu */
    run(a, 40, 0);
    shot(a, &cv, dir, "07_rule");

    g_fake.state = NETPLAY_IN_ROOM;
    g_fake.max_slot = 2;
    g_fake.is_host = true;
    add_member("stftest2", 1, -1, 0, 0, -1);
    run(a, 40, 0);
    shot(a, &cv, dir, "08_waiting");

    add_member("m2hletest", 0, -1, ROOM_MEMBER_READY, 0, 30);
    run(a, 40, 0);
    a->countdown = 24 * 60 + 30;
    run(a, 30, 0);
    shot(a, &cv, dir, "09_vs");

    g_fake.member_count = 0;
    g_fake.max_slot = 6;
    add_member("stftest2", 1, -1, ROOM_MEMBER_READY, 1, -1);
    add_member("m2hletest", 0, -1, ROOM_MEMBER_READY, 2, 30);
    add_member("saltyfreeman", 0, -1, ROOM_MEMBER_READY, 0, 150);
    add_member("biggestsonic", 0, -1, ROOM_MEMBER_READY, 0, 250);
    g_fake.auto_start_s = 17;
    run(a, 40, 0);
    shot(a, &cv, dir, "10_room_match");

    g_fake.member_count = 0;
    g_fake.max_slot = 2;
    add_member("stftest2", 1, 0, ROOM_MEMBER_READY, 0, -1);
    add_member("m2hletest", 0, 1, ROOM_MEMBER_READY, 0, 30);
    g_fake.room.match = 1;
    run(a, 2, 0);
    g_fake.room.match = 2;
    g_fake.room.phase = ROOM_PHASE_LOBBY;
    g_fake.room.last_result = 0;
    run(a, 40, 0);
    shot(a, &cv, dir, "11_result");

    run(a, 1, PS3UI_PAD_CROSS);
    g_fake.members[0].data.flags = 0;       /* not ready: the Exit button works */
    g_fake.me.flags = 0;
    run(a, 30, 0);
    run(a, 1, PS3UI_PAD_CIRCLE);           /* leave? */
    run(a, 20, 0);
    shot(a, &cv, dir, "12_dialog");

    /* what a frame costs: the VS lobby, drawn and resolved */
    run(a, 1, PS3UI_PAD_CIRCLE);
    g_fake.room.last_result = ROOM_RESULT_NONE;
    run(a, 60, 0);
    clock_t t0 = clock();
    for (int i = 0; i < 120; i++) {
        ps3ui_app_frame(a, 0);
        ps3ui_app_draw(a, &cv);
        ps3ui_canvas_resolve(&cv);
    }
    printf("frame: %.2f ms at %dx%d\n", (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 120.0, w, h);
    return 0;
}


/* ---- the offline shell ------------------------------------------------------------ */

static int g_resets, g_applied_versus = -1;
static uint8_t g_applied[8];
static void host_reset(void *u) { (void)u; g_resets++; }
static void host_apply(void *u, const uint8_t v[8], int versus)
{
    (void)u;
    memcpy(g_applied, v, 8);
    g_applied_versus = versus;
}

/* A stand-in for the board's picture under the overlays: a 4:3 checker. */
static void fake_game(ps3ui_canvas_t *cv)
{
    for (int y = 0; y < cv->h; y++)
        for (int x = 0; x < cv->w; x++) {
            float *p = cv->rgb + ((size_t)y * (size_t)cv->w + (size_t)x) * 3;
            int c = ((x / 40) + (y / 40)) & 1;
            p[0] = c ? 0.55f : 0.35f;
            p[1] = c ? 0.40f : 0.25f;
            p[2] = c ? 0.20f : 0.15f;
        }
}

/* A press is one frame down and at least one up: two presses in a row are two. */
static void srun(ps3ui_shell_t *sh, int frames, uint32_t pad, uint32_t pad2)
{
    for (int i = 0; i < frames; i++)
        ps3ui_shell_frame(sh, i == 0 ? pad : 0, i == 0 ? pad2 : 0, 0);
    if ((pad || pad2) && frames == 1)
        ps3ui_shell_frame(sh, 0, 0, 0);
}

static void sshot(ps3ui_shell_t *sh, ps3ui_canvas_t *cv, const char *dir, const char *name)
{
    char path[1024];
    if (ps3ui_shell_view(sh) != PS3UI_VIEW_FULL)
        fake_game(cv);
    ps3ui_shell_draw(sh, cv);
    ps3ui_canvas_resolve(cv);
    snprintf(path, sizeof path, "%s/%s.png", dir, name);
    printf("%s view=%d paused=%d gamepad=%d\n", path, ps3ui_shell_view(sh), ps3ui_shell_board_paused(sh),
           ps3ui_shell_game_pad(sh));
    write_png(path, cv->rgba, cv->w, cv->h);
}

static int dump_shell(const char *dir, int w, int h)
{
    ps3ui_canvas_t cv = { 0 };
    ps3ui_canvas_size(&cv, w, h);
    ps3ui_backend_t be = { fake_status, fake_post, NULL };
    ps3ui_app_init(&g_ps3ui_app, be);
    memset(&g_fake, 0, sizeof g_fake);
    ps3ui_shell_t *sh = &g_ps3ui_shell;
    ps3ui_shell_init(sh, (ps3ui_host_t){ host_reset, host_apply, NULL, NULL }, &g_ps3ui_app);

    srun(sh, 20, 0, 0);
    sshot(sh, &cv, dir, "s01_title");
    srun(sh, 1, PS3UI_PAD_START, 0);
    srun(sh, 40, 0, 0);
    sshot(sh, &cv, dir, "s02_main");
    srun(sh, 1, PS3UI_PAD_CROSS, 0);        /* Arcade */
    srun(sh, 30, 0, 0);
    srun(sh, 1, PS3UI_PAD_DOWN, 0);
    srun(sh, 1, PS3UI_PAD_RIGHT, 0);        /* rounds 2 -> 3 */
    srun(sh, 20, 0, 0);
    sshot(sh, &cv, dir, "s03_arcade");
    srun(sh, 1, PS3UI_PAD_CIRCLE, 0);
    srun(sh, 20, 0, 0);
    srun(sh, 1, PS3UI_PAD_DOWN, 0);
    srun(sh, 1, PS3UI_PAD_CROSS, 0);        /* Offline Versus */
    srun(sh, 30, 0, 0);
    sshot(sh, &cv, dir, "s04_versus_wait");
    srun(sh, 1, 0, PS3UI_PAD_START);        /* 2P claims */
    srun(sh, 20, 0, 0);
    sshot(sh, &cv, dir, "s05_versus");
    srun(sh, 1, PS3UI_PAD_CIRCLE, 0);
    srun(sh, 20, 0, 0);
    srun(sh, 1, PS3UI_PAD_DOWN, 0);
    srun(sh, 1, PS3UI_PAD_DOWN, 0);
    srun(sh, 1, PS3UI_PAD_CROSS, 0);        /* Help & Options */
    srun(sh, 30, 0, 0);
    sshot(sh, &cv, dir, "s06_options");
    srun(sh, 1, PS3UI_PAD_CROSS, 0);        /* Controls */
    srun(sh, 30, 0, 0);
    srun(sh, 1, PS3UI_PAD_RIGHT, 0);        /* Arcade stick 1 */
    srun(sh, 10, 0, 0);
    sshot(sh, &cv, dir, "s07_controls");
    srun(sh, 1, PS3UI_PAD_CROSS, 0);
    srun(sh, 20, 0, 0);
    srun(sh, 1, PS3UI_PAD_DOWN, 0);
    srun(sh, 1, PS3UI_PAD_CROSS, 0);        /* Settings */
    srun(sh, 30, 0, 0);
    srun(sh, 1, PS3UI_PAD_RIGHT, 0);
    srun(sh, 10, 0, 0);
    sshot(sh, &cv, dir, "s08_settings");
    srun(sh, 1, PS3UI_PAD_CROSS, 0);
    srun(sh, 20, 0, 0);
    srun(sh, 1, PS3UI_PAD_CIRCLE, 0);       /* back to main */
    srun(sh, 20, 0, 0);
    srun(sh, 1, PS3UI_PAD_UP, 0);
    srun(sh, 1, PS3UI_PAD_UP, 0);
    srun(sh, 1, PS3UI_PAD_UP, 0);
    srun(sh, 1, PS3UI_PAD_CROSS, 0);        /* Arcade */
    srun(sh, 20, 0, 0);
    srun(sh, 1, PS3UI_PAD_CROSS, 0);        /* start */
    srun(sh, 20, 0, 0);
    printf("applied versus=%d bytes %u %u %u %u %u %u %u %u\n", g_applied_versus, g_applied[0], g_applied[1],
           g_applied[2], g_applied[3], g_applied[4], g_applied[5], g_applied[6], g_applied[7]);
    sshot(sh, &cv, dir, "s09_game");
    srun(sh, 1, PS3UI_PAD_SELECT, 0);       /* pause */
    srun(sh, 30, 0, 0);
    sshot(sh, &cv, dir, "s10_pause");
    srun(sh, 1, PS3UI_PAD_DOWN, 0);
    srun(sh, 1, PS3UI_PAD_DOWN, 0);         /* Exit Game (the blank row skipped) */
    srun(sh, 1, PS3UI_PAD_CROSS, 0);
    srun(sh, 30, 0, 0);
    sshot(sh, &cv, dir, "s11_exit_confirm");
    srun(sh, 1, PS3UI_PAD_UP, 0);
    srun(sh, 1, PS3UI_PAD_CROSS, 0);        /* Yes */
    srun(sh, 30, 0, 0);
    printf("resets=%d screen=%d\n", g_resets, sh->scr);
    sshot(sh, &cv, dir, "s12_back_to_main");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "shell") == 0)
        return dump_shell(argv[2], argc >= 5 ? atoi(argv[3]) : 1280, argc >= 5 ? atoi(argv[4]) : 720);
    if (argc >= 3 && strcmp(argv[1], "app") == 0)
        return dump_app(argv[2], argc >= 5 ? atoi(argv[3]) : 1280, argc >= 5 ? atoi(argv[4]) : 720);
    if (argc >= 3 && strcmp(argv[1], "sprites") == 0)
        return dump_sprites(argv[2]);
    if (argc >= 3 && strcmp(argv[1], "vs") == 0) {
        int w = argc >= 5 ? atoi(argv[3]) : 1280, h = argc >= 5 ? atoi(argv[4]) : 720;
        float frame = argc >= 6 ? (float)atof(argv[5]) : 40.0f;
        ps3ui_canvas_t cv = { 0 };
        ps3ui_canvas_size(&cv, w, h);
        ps3ui_vs_lobby_t s = { 0 };
        s.title = "PLAYER MATCH";
        s.name[0] = "stftest2";
        s.name[1] = "m2hletest";
        s.ping_ms[0] = PS3UI_PING_NONE;
        s.ping_ms[1] = 30;
        s.ready[1] = 1;
        s.ready_age[1] = 60.0f;
        s.countdown = 24;
        s.left_hint = "SELECT button:Controls";
        s.hints = "\x03:Opponent's Profile  \x01:Exit  \x02:Ready";
        s.frame = frame;
        ps3ui_draw_vs_lobby(&cv, &s);
        ps3ui_canvas_resolve(&cv);
        return write_png(argv[2], cv.rgba, w, h) ? 1 : 0;
    }
    fprintf(stderr, "usage: ps3ui_render sprites <dir> | vs <out.png> [w h [frame]]\n");
    return 2;
}
