/*
 * sky_eye.h — put Sonic the Fighters' camera where a noclip link says, on the
 * stage it says, and say when the picture is worth taking (Pinboard #265).
 *
 * STF's own debug menu has a page for this, SKY EYE (DEBUG_SKY_EYE, 0x4A0A4):
 * it sets debug_flag bit 5, which stops camera_work, and lets the pad move the
 * camera record fa_camera points at (0x500814):
 *
 *   +0x18 / +0x1C / +0x20   Xpos, Ypos, Zpos   float, board units
 *   +0x24 / +0x26 / +0x28   Xang, Yang, Zang   int16, 0x10000 a turn
 *
 * camera_control builds the view from those alone: ang_z(-Zang), ang_x(-Xang),
 * ang_y(-Yang), trans(-pos). Bit 5 is no use here, though: control_init,
 * collision, enemy_control, object_init, adv_movie_cont (the attract goes
 * blank) and a dozen more skip their work on it too. So this mode leaves
 * debug_flag alone and writes the record instead, at loc_1F3A0 in
 * camera_control (SKY_EYE_HOOK_PC) — the point camera_work's path and the
 * debug path meet, after camera_work has had its say and before area_check and
 * the view read the record. The rest of the game runs as it would.
 *
 * Getting to the stage is the other half. Holding stage_num is not arriving:
 * the game loads a stage on its next change_scene, and the stage it loaded is
 * the one whose texture-set words (stage record +0x0C, +0x0E) change_scene
 * copied to 0x504800 — the test tools/lib/capture.mjs (reachStage) makes. The
 * textures then stream in over many frames, so the picture is ready when the
 * texture sheet (texram0) stops changing (captureBoard's settle): the same
 * digest on two polls running, with the sheet more than lightly filled.
 *
 *   off -> loading (stage_num and the replay's stage held every frame)
 *       -> settling (the record's words match; camera held from here on)
 *       -> ready (texture RAM unchanged for SKY_EYE_STABLE_POLLS polls)
 *
 * The link is noclip's view link (js/viewlink.js): `game=sfight`, `stage=N`,
 * and the board camera noclip writes from its SKY EYE readout as
 * `eye=x,y,z` and `ang=xang,yang,zang` (js/skyeye.js). An older link without
 * them still carries the explorer's camera, `pos` and `look` (fly) or `target`
 * (orbit); that is the board's camera with Z negated as long as the explorer's
 * scene root is the identity, which is every stage but the ones that fly
 * (noclip's root is their inverse world prologue, unless `ride=1`).
 *
 * Everything here runs on the emu thread, or with its mutex held.
 */
#ifndef CORE_SKY_EYE_H
#define CORE_SKY_EYE_H

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "memory.h"
#include "i960.h"
#include "constants.h"
#include "game_profile.h"
#include "hle_hooks.h"   /* g_replay_stage_pin, g_active_profile */

/* STF's addresses (the arcade and console profiles run the same program). */
#define SKY_EYE_HOOK_PC      0x0001F3A0u  /* camera_control, loc_1F3A0 */
#define SKY_EYE_FA_CAMERA    0x00500814u  /* -> the camera record */
#define SKY_EYE_STAGE_NUM    0x00500064u
#define SKY_EYE_REPLAY_STAGE 0x0050005Bu  /* byte_50005B, the attract replay's */
#define SKY_EYE_STAGE_DATA   0x0008F3D0u  /* stage records, in the program ROM */
#define SKY_EYE_STAGE_STRIDE 256u
#define SKY_EYE_STAGE_LOADED 0x00504800u  /* the record change_scene copied */
#define SKY_EYE_STAGE_TEX    0x0Cu        /* its two texture-set words */
#define SKY_EYE_STAGES       16           /* stage records (identifyScene) */
#define SKY_EYE_PI           3.14159265358979323846

#define SKY_EYE_POLL_FRAMES   15          /* frames between texture digests */
#define SKY_EYE_STABLE_POLLS  2
#define SKY_EYE_MIN_NONZERO   0x10000u    /* capture.mjs MIN_NONZERO */

typedef enum {
    SKY_EYE_OFF = 0, SKY_EYE_LOADING, SKY_EYE_SETTLING, SKY_EYE_READY,
} sky_eye_phase_t;

static const char *const SKY_EYE_PHASE_NAME[] = { "off", "loading", "settling", "ready" };

typedef struct {
    int      stage;          /* stage_num; -1 = keep whatever is up */
    float    pos[3];         /* board frame */
    int      xang, yang, zang;
    int      from_explorer;  /* came from pos/look, not eye/ang */
} sky_eye_req_t;

static struct {
    volatile int  phase;
    sky_eye_req_t req;
    uint32_t      phase_frames;   /* frames spent in the current phase */
    uint32_t      total_frames;   /* frames since the request */
    uint64_t      digest;
    int           stable;
    uint32_t      nonzero;
    uint32_t      hook_calls;     /* times camera_control reached the hook */
    uint32_t      hook_hits;      /* the hook found the record this frame */
    int           camera_held;    /* ...and did last frame */
    int           saved_pin;      /* g_replay_stage_pin before the request */
} g_sky_eye;

/* ---- the camera ------------------------------------------------------------ */

static inline int sky_eye_profile_ok(void) {
    return g_active_profile && (!strcmp(g_active_profile->id, "sfight") ||
                                !strcmp(g_active_profile->id, "sfight_console"));
}

static inline uint32_t sky_eye_f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float    sky_eye_u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* The hook (SFIGHT_BASE_HOOKS): camera_control is about to read the record in
 * g13. Only fa_camera's record is the fight camera. */
static int sky_eye_hook_camera(i960_cpu_t *cpu, memory_bus_t *bus) {
    g_sky_eye.hook_calls++;
    if (g_sky_eye.phase < SKY_EYE_SETTLING) return 1;
    uint32_t rec = cpu->globals.g13;
    if (rec != mem_read32(bus, SKY_EYE_FA_CAMERA)) return 1;
    const sky_eye_req_t *r = &g_sky_eye.req;
    for (int i = 0; i < 3; i++) mem_write32(bus, rec + 0x18u + 4u * (uint32_t)i, sky_eye_f2u(r->pos[i]));
    mem_write16(bus, rec + 0x24u, (uint32_t)r->xang & 0xFFFFu);
    mem_write16(bus, rec + 0x26u, (uint32_t)r->yang & 0xFFFFu);
    mem_write16(bus, rec + 0x28u, (uint32_t)r->zang & 0xFFFFu);
    g_sky_eye.hook_hits++;
    return 1;
}

/* The game's camera as it stands: { pos, angles } off fa_camera's record. */
static inline int sky_eye_read_camera(memory_bus_t *bus, float pos[3], int ang[3]) {
    uint32_t rec = mem_read32(bus, SKY_EYE_FA_CAMERA);
    if (!rec) return 0;
    for (int i = 0; i < 3; i++) pos[i] = sky_eye_u2f(mem_read32(bus, rec + 0x18u + 4u * (uint32_t)i));
    for (int i = 0; i < 3; i++) ang[i] = (int16_t)mem_read16(bus, rec + 0x24u + 2u * (uint32_t)i);
    return 1;
}

/* ---- the stage ------------------------------------------------------------- */

static inline uint64_t sky_eye_digest(const uint8_t *p, size_t n, uint64_t h, uint32_t *nonzero) {
    const uint64_t *w = (const uint64_t *)p;
    for (size_t i = 0; i < n / 8; i++) {
        if (w[i]) (*nonzero)++;
        h = (h ^ w[i]) * 0x100000001B3ull;
    }
    return h;
}

static inline int sky_eye_stage_arrived(memory_bus_t *bus, int stage) {
    uint32_t want = mem_read32(bus, SKY_EYE_STAGE_DATA + (uint32_t)stage * SKY_EYE_STAGE_STRIDE + SKY_EYE_STAGE_TEX);
    return mem_read32(bus, SKY_EYE_STAGE_LOADED + SKY_EYE_STAGE_TEX) == want;
}

static inline void sky_eye_enter(int phase) {
    g_sky_eye.phase = phase;
    g_sky_eye.phase_frames = 0;
    LOG_INFO("sky_eye: %s (frame %u of the request)", SKY_EYE_PHASE_NAME[phase], g_sky_eye.total_frames);
}

/* The frame edge (emu_thread.h): hold the stage, watch it arrive and settle. */
static inline void sky_eye_edge(memory_bus_t *bus) {
    if (g_sky_eye.phase == SKY_EYE_OFF) return;
    if (!sky_eye_profile_ok()) { g_sky_eye.phase = SKY_EYE_OFF; return; }
    const int stage = g_sky_eye.req.stage;
    g_sky_eye.total_frames++;
    g_sky_eye.phase_frames++;
    g_sky_eye.camera_held = g_sky_eye.hook_hits != 0;
    g_sky_eye.hook_hits = 0;
    if (stage >= 0) {
        mem_write8(bus, SKY_EYE_STAGE_NUM, (uint32_t)stage);
        mem_write8(bus, SKY_EYE_REPLAY_STAGE, (uint32_t)stage);
        g_replay_stage_pin = stage;
    }
    switch (g_sky_eye.phase) {
    case SKY_EYE_LOADING:
        if (stage < 0 || sky_eye_stage_arrived(bus, stage)) {
            g_sky_eye.stable = 0;
            g_sky_eye.digest = 0;
            sky_eye_enter(SKY_EYE_SETTLING);
        }
        break;
    case SKY_EYE_SETTLING:
    case SKY_EYE_READY:
        if (stage >= 0 && !sky_eye_stage_arrived(bus, stage)) {   /* the game moved on */
            sky_eye_enter(SKY_EYE_LOADING);
            break;
        }
        if (g_sky_eye.phase_frames % SKY_EYE_POLL_FRAMES) break;
        if (!bus->texram0) break;
        uint32_t nz = 0;
        uint64_t h = sky_eye_digest(bus->texram0, TEXRAM0_SIZE, 0xCBF29CE484222325ull, &nz);
        g_sky_eye.nonzero = nz * 8u;
        int same = h == g_sky_eye.digest && g_sky_eye.nonzero > SKY_EYE_MIN_NONZERO;
        g_sky_eye.digest = h;
        g_sky_eye.stable = same ? g_sky_eye.stable + 1 : 0;
        if (g_sky_eye.phase == SKY_EYE_SETTLING && g_sky_eye.stable >= SKY_EYE_STABLE_POLLS)
            sky_eye_enter(SKY_EYE_READY);
        else if (g_sky_eye.phase == SKY_EYE_READY && !same)
            sky_eye_enter(SKY_EYE_SETTLING);
        break;
    default: break;
    }
}

static inline void sky_eye_start(const sky_eye_req_t *r) {
    if (g_sky_eye.phase == SKY_EYE_OFF) g_sky_eye.saved_pin = g_replay_stage_pin;
    g_sky_eye.req = *r;
    g_sky_eye.total_frames = 0;
    g_sky_eye.stable = 0;
    g_sky_eye.digest = 0;
    sky_eye_enter(SKY_EYE_LOADING);
}

static inline void sky_eye_stop(void) {
    if (g_sky_eye.phase != SKY_EYE_OFF && g_sky_eye.req.stage >= 0)
        g_replay_stage_pin = g_sky_eye.saved_pin;
    g_sky_eye.phase = SKY_EYE_OFF;
}

/* ---- the link -------------------------------------------------------------- */

/* Board angle (int16) from radians. */
static inline int sky_eye_angle(double rad) {
    long a = lround(rad / (2.0 * SKY_EYE_PI) * 65536.0) & 0xFFFF;
    return a >= 0x8000 ? (int)(a - 0x10000) : (int)a;
}

/* One `key=` value out of a link's fragment or query, %-decoded. */
static int sky_eye_param(const char *link, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    const char *s = strchr(link, '#');
    s = s ? s + 1 : (strchr(link, '?') ? strchr(link, '?') + 1 : link);
    while (*s) {
        const char *amp = strchr(s, '&');
        size_t len = amp ? (size_t)(amp - s) : strlen(s);
        if (len > kl && !strncmp(s, key, kl) && s[kl] == '=') {
            size_t o = 0;
            for (size_t i = kl + 1; i < len && o + 1 < cap; i++) {
                if (s[i] == '%' && i + 2 < len && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
                    char hx[3] = { s[i + 1], s[i + 2], 0 };
                    out[o++] = (char)strtol(hx, NULL, 16);
                    i += 2;
                } else out[o++] = s[i] == '+' ? ' ' : s[i];
            }
            out[o] = 0;
            return 1;
        }
        if (!amp) break;
        s = amp + 1;
    }
    return 0;
}

static int sky_eye_nums(const char *s, double *v, int max) {
    int n = 0;
    while (n < max && *s) {
        char *end;
        v[n] = strtod(s, &end);
        if (end == s || !isfinite(v[n])) return -1;
        n++;
        s = end;
        if (*s == ',') s++;
        else break;
    }
    return *s ? -1 : n;
}

/* The link's game and stage, when it names them. 0 and a reason in `err` when
 * either is not one this can use. */
static int sky_eye_link_game_stage(const char *link, sky_eye_req_t *r, char *err, size_t errcap) {
    char buf[256];
    if (sky_eye_param(link, "game", buf, sizeof buf) && strcmp(buf, "sfight")) {
        snprintf(err, errcap, "the link is for game '%s', not sfight", buf);
        return 0;
    }
    if (sky_eye_param(link, "stage", buf, sizeof buf)) {
        char *end;
        long s = strtol(buf, &end, 10);
        if (*end || s < 0 || s >= SKY_EYE_STAGES) { snprintf(err, errcap, "bad stage '%s'", buf); return 0; }
        r->stage = (int)s;
    }
    return 1;
}

/* The board's own camera, `eye` (in `eyebuf`) and `ang`. 0 and a reason in
 * `err` when they are not usable. */
static int sky_eye_link_eye(const char *link, const char *eyebuf, sky_eye_req_t *r,
                            char *err, size_t errcap) {
    char buf[256];
    double v[3], t[3];
    if (sky_eye_nums(eyebuf, v, 3) != 3) { snprintf(err, errcap, "bad eye '%s'", eyebuf); return 0; }
    int n;
    if (!sky_eye_param(link, "ang", buf, sizeof buf) || (n = sky_eye_nums(buf, t, 3)) < 2) {
        snprintf(err, errcap, "eye without a usable ang");
        return 0;
    }
    for (int i = 0; i < 3; i++) r->pos[i] = (float)v[i];
    r->xang = (int16_t)(long)t[0];
    r->yang = (int16_t)(long)t[1];
    r->zang = n > 2 ? (int16_t)(long)t[2] : 0;
    return 1;
}

/* The explorer's own camera: the board's, Z negated, on a stage that does
 * not fly. 0 and a reason in `err` when the link has none. */
static int sky_eye_link_explorer(const char *link, sky_eye_req_t *r, char *err, size_t errcap) {
    char buf[256];
    double v[3], t[3];
    if (!sky_eye_param(link, "pos", buf, sizeof buf) || sky_eye_nums(buf, v, 3) != 3) {
        snprintf(err, errcap, "no camera in the link (eye/ang or pos)");
        return 0;
    }
    r->pos[0] = (float)v[0]; r->pos[1] = (float)v[1]; r->pos[2] = (float)-v[2];
    r->from_explorer = 1;
    char cam[16] = "";
    sky_eye_param(link, "cam", cam, sizeof cam);
    if (!strcmp(cam, "fly") && sky_eye_param(link, "look", buf, sizeof buf) && sky_eye_nums(buf, t, 2) == 2) {
        r->yang = sky_eye_angle(t[0]);   /* look=yaw,pitch: the fly rig's Euler(pitch, yaw, 0, 'YXZ') */
        r->xang = sky_eye_angle(t[1]);
        return 1;
    }
    if (sky_eye_param(link, "target", buf, sizeof buf) && sky_eye_nums(buf, t, 3) == 3) {
        double fx = t[0] - v[0], fy = t[1] - v[1], fz = -(t[2] - v[2]);
        r->xang = sky_eye_angle(atan2(fy, hypot(fx, fz)));
        r->yang = sky_eye_angle(atan2(-fx, fz));
        return 1;
    }
    snprintf(err, errcap, "pos without look or target");
    return 0;
}

/* A noclip view link into a request. 0 and a reason in `err` when it is not
 * one this can use. */
static int sky_eye_parse_link(const char *link, sky_eye_req_t *r, char *err, size_t errcap) {
    char buf[256];
    memset(r, 0, sizeof *r);
    r->stage = -1;
    if (!sky_eye_link_game_stage(link, r, err, errcap)) return 0;
    if (sky_eye_param(link, "eye", buf, sizeof buf)) return sky_eye_link_eye(link, buf, r, err, errcap);
    return sky_eye_link_explorer(link, r, err, errcap);
}

/* The game's camera as a noclip link (a fragment; noclip's page goes before
 * it). The explorer's `pos`/`look` are filled in too, for a noclip that predates
 * eye/ang, and are right on every stage that does not fly. `lens=1` ticks the
 * explorer's board lens (focal 280 on 384 lines, the fov geo3d.h takes from
 * the same focal), or it opens the view at its own field of view. */
static int sky_eye_link(memory_bus_t *bus, char *out, size_t cap) {
    float p[3]; int a[3];
    if (!sky_eye_read_camera(bus, p, a)) return 0;
    int stage = (int)mem_read8(bus, SKY_EYE_STAGE_NUM);
    double pitch = a[0] * 2.0 * SKY_EYE_PI / 65536.0, yaw = a[1] * 2.0 * SKY_EYE_PI / 65536.0;
    double fx = -sin(yaw) * cos(pitch), fy = sin(pitch), fz = cos(yaw) * cos(pitch);
    int n = snprintf(out, cap,
        "#game=sfight&tab=stage&stage=%d&lens=1&eye=%.4f%%2C%.4f%%2C%.4f&ang=%d%%2C%d%%2C%d"
        "&cam=fly&pos=%.3f%%2C%.3f%%2C%.3f&target=%.3f%%2C%.3f%%2C%.3f&look=%.4f%%2C%.4f",
        stage, p[0], p[1], p[2], a[0], a[1], a[2],
        p[0], p[1], -p[2], p[0] + 5 * fx, p[1] + 5 * fy, -(p[2] + 5 * fz), yaw, pitch);
    return n > 0 && (size_t)n < cap;
}

#endif /* CORE_SKY_EYE_H */
