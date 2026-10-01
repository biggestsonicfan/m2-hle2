#ifndef M2HLE_BACKUP_RAM_H
#define M2HLE_BACKUP_RAM_H

/*
 * Backup RAM that outlives the process: the board's battery-backed SRAM at
 * 0x01D00000, where a game keeps its test-menu settings (STF: GAME and COIN
 * ASSIGNMENTS), its bookkeeping and its rankings.
 *
 * The board has 16 KB there (MAME model2.cpp maps 0x01D00000-0x01D03FFF to the
 * "backup1" NVRAM), and that is what is kept: the file is MAME's
 * nvram/<set>/backup1 byte for byte, so either emulator reads the other's.
 *
 * Three rules make it safe to keep:
 *
 *  - A reset does not clear it, as on the board: mem_init keeps the contents
 *    while the battery is attached (backup_ram_before_reset / _after_reset).
 *  - A netplay session's board is not the player's. A session is a cold boot of
 *    the same blank board on every machine, so backup RAM there must start empty
 *    whatever either player has saved, and what the session writes (a room's
 *    rules, its bookkeeping) must not reach the player's file. Every board reset
 *    netplay performs, and a PS3 match, detaches the battery first
 *    (backup_ram_detach); only loading a ROM set attaches it again.
 *  - Nothing keeps it unless the frontend asks (backup_ram_open). The graders,
 *    the tests, det_digest and the fly's headless runs never do, so they boot
 *    blank exactly as before.
 *
 * Where it is kept is the frontend's business: a file in the per-user settings
 * directory on the desktop and the handheld, localStorage in the browser, and
 * the frontend's own .srm in the libretro core (RETRO_MEMORY_SAVE_RAM points at
 * g_backup.image, which the core hands the board on its first run).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"

#ifdef __EMSCRIPTEN__
#  include <emscripten.h>
#elif defined(_WIN32)
#  include <direct.h>
#else
#  include <sys/stat.h>
#endif

#define BACKUP_RAM_KEEP 0x4000u    /* the board's SRAM, MAME's backup1 */
/* Look for changes once a minute. STF bumps a counter at 0x1D03319 every few
 * seconds for as long as it runs, so a shorter wait is a file write every few
 * seconds -- an SD card's worth of wear on a handheld. Exit flushes. */
#define BACKUP_RAM_CHECK_FRAMES 3600u

typedef struct {
    bool     attached;     /* the board's backup RAM is the player's */
    bool     fresh;        /* image holds a newly opened set's: do not take the old board's */
    bool     dirty;        /* image differs from what was last stored */
    bool     to_file;      /* store to path (desktop, handheld, web); else the frontend owns it */
    char     set[64];      /* the ROM set it belongs to */
    char     path[512];    /* the file, or the localStorage key on the web */
    uint32_t frames;
    const uint8_t *board;  /* the bus's backup RAM, as of the last mem_init */
    uint8_t  image[BACKUP_RAM_KEEP];
} backup_ram_t;

static backup_ram_t g_backup;

/* --no-nvram / --nvram-dir. -1: the frontend's default. */
static int  g_backup_want = -1;
static char g_backup_dir[480];

/* ---- Where it is kept ------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
/* localStorage takes text: hex is 32 KB a set, well inside its quota. */
EM_JS(void, backup_ram_web_store, (const char *key, const uint8_t *p, int n), {
    let s = '';
    for (let i = 0; i < n; i++) s += (HEAPU8[p + i] | 256).toString(16).slice(1);
    try { localStorage.setItem(UTF8ToString(key), s); } catch (e) {}
});
EM_JS(int, backup_ram_web_fetch, (const char *key, uint8_t *p, int n), {
    let s = null;
    try { s = localStorage.getItem(UTF8ToString(key)); } catch (e) {}
    if (s === null || s.length != n * 2) return 0;
    for (let i = 0; i < n; i++) HEAPU8[p + i] = parseInt(s.substr(i * 2, 2), 16);
    return 1;
});
#else
static inline void backup_ram_mkdir(const char *dir) {
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0700);
#endif
}

/* The per-user directory netplay.cfg lives in (netplay_cfg_path): %APPDATA%\m2hle2,
 * $XDG_CONFIG_HOME/m2hle2 or ~/.config/m2hle2. Created here. */
static inline bool backup_ram_user_dir(char *dir, size_t cap) {
    dir[0] = '\0';
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (!appdata || !appdata[0]) return false;
    snprintf(dir, cap, "%s\\m2hle2", appdata);
#else
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && xdg[0])        snprintf(dir, cap, "%s", xdg);
    else if (home && home[0]) snprintf(dir, cap, "%s/.config", home);
    else return false;
    backup_ram_mkdir(dir);
    size_t n = strlen(dir);
    snprintf(dir + n, cap - n, "/m2hle2");
#endif
    backup_ram_mkdir(dir);
    return true;
}
#endif

#ifdef _WIN32
#  define BACKUP_RAM_SEP "\\"
#else
#  define BACKUP_RAM_SEP "/"
#endif

/* Write the image where it is kept. Emu thread (or with the board stopped). */
static inline void backup_ram_store(void) {
    if (!g_backup.to_file || !g_backup.path[0]) { g_backup.dirty = false; return; }
#ifdef __EMSCRIPTEN__
    backup_ram_web_store(g_backup.path, g_backup.image, (int)BACKUP_RAM_KEEP);
    g_backup.dirty = false;
#else
    /* Written whole and renamed into place, so a crash mid-write leaves the old
     * file rather than half of a new one (the game would throw that away). */
    char tmp[sizeof g_backup.path + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_backup.path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(g_backup.image, 1, BACKUP_RAM_KEEP, f) == BACKUP_RAM_KEEP;
    if (f) ok = (fclose(f) == 0) && ok;
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp, g_backup.path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    ok = ok && rename(tmp, g_backup.path) == 0;
#endif
    if (!ok) { remove(tmp); LOG_WARN("backup RAM: could not write %s", g_backup.path); return; }
    g_backup.dirty = false;
    LOG_INFO("backup RAM: saved %s", g_backup.path);
#endif
}

/* ---- The frontend's side --------------------------------------------------- */

/* Store now if it changed (exit, ROM switch). Board stopped. */
static inline void backup_ram_flush(void) {
    const uint8_t *back = g_backup.board;
    if (g_backup.attached && !g_backup.fresh && back &&
        memcmp(g_backup.image, back, BACKUP_RAM_KEEP)) {
        memcpy(g_backup.image, back, BACKUP_RAM_KEEP);
        g_backup.dirty = true;
    }
    if (g_backup.dirty) backup_ram_store();
}

/*
 * Attach the battery for a newly loaded ROM set, before its install_fn: the
 * previous set's contents are stored first, then this set's are read (blank if
 * there are none yet). to_file false keeps nothing on disk: the frontend owns
 * g_backup.image (libretro). Board stopped.
 */
static inline void backup_ram_open(const char *set, bool to_file) {
    backup_ram_flush();
    memset(g_backup.image, 0, sizeof g_backup.image);
    snprintf(g_backup.set, sizeof g_backup.set, "%s", set ? set : "");
    g_backup.path[0] = '\0';
    g_backup.to_file = to_file;
    g_backup.dirty   = false;
    g_backup.frames  = 0;
    g_backup.attached = g_backup.fresh = true;
    if (!to_file || !g_backup.set[0]) return;
#ifdef __EMSCRIPTEN__
    snprintf(g_backup.path, sizeof g_backup.path, "m2hle2.nvram.%s", g_backup.set);
    if (backup_ram_web_fetch(g_backup.path, g_backup.image, (int)BACKUP_RAM_KEEP))
        LOG_INFO("backup RAM: %s from this browser", g_backup.set);
#else
    char dir[480];
    if (g_backup_dir[0]) {
        snprintf(dir, sizeof dir, "%s", g_backup_dir);
        backup_ram_mkdir(dir);
    } else if (backup_ram_user_dir(dir, sizeof dir)) {
        size_t n = strlen(dir);
        snprintf(dir + n, sizeof dir - n, BACKUP_RAM_SEP "nvram");
        backup_ram_mkdir(dir);
    } else {
        LOG_WARN("backup RAM: no per-user directory; it will not be kept");
        return;
    }
    size_t n = strlen(dir);
    snprintf(dir + n, sizeof dir - n, BACKUP_RAM_SEP "%s", g_backup.set);
    backup_ram_mkdir(dir);
    snprintf(g_backup.path, sizeof g_backup.path, "%s" BACKUP_RAM_SEP "backup1", dir);
    FILE *f = fopen(g_backup.path, "rb");
    if (!f) { LOG_INFO("backup RAM: %s starts blank (%s)", g_backup.set, g_backup.path); return; }
    size_t got = fread(g_backup.image, 1, BACKUP_RAM_KEEP, f);
    fclose(f);
    if (got != BACKUP_RAM_KEEP) {
        LOG_WARN("backup RAM: %s is %zu bytes, not %u; starting blank", g_backup.path, got, BACKUP_RAM_KEEP);
        memset(g_backup.image, 0, sizeof g_backup.image);
    } else {
        LOG_INFO("backup RAM: %s from %s", g_backup.set, g_backup.path);
    }
#endif
}

/* The name a set's backup RAM is kept under: the ROM set's, as MAME's
 * nvram/<set>/ is, unless the program is not the set's own game (homebrew on
 * a stock set's board, profile_adopt_program): that gets one of its own, by
 * its program ROM, so it cannot write over the game's settings. */
static inline void backup_ram_key(char *out, size_t cap, const char *set,
                                  bool homebrew, const uint8_t *prog, size_t size) {
    if (!homebrew || !prog) { snprintf(out, cap, "%s", set ? set : ""); return; }
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; i++) h = (h ^ prog[i]) * 16777619u;
    snprintf(out, cap, "%s-%08x", set ? set : "", (unsigned)h);
}

/* The board is no longer the player's (a netplay session, a PS3 match): store
 * what it holds now and stop following it. The next mem_init boots blank. */
static inline void backup_ram_detach(void) {
    if (!g_backup.attached) return;
    backup_ram_flush();
    g_backup.attached = false;
    LOG_INFO("backup RAM: a netplay board; %s is kept as it was", g_backup.set);
}

/* The player's game again after a session (netplay_restart_alone): the image
 * still holds what the detach flushed, and the reset that follows puts it back
 * on the board. */
static inline void backup_ram_reattach(void) {
    if (g_backup.attached || !g_backup.set[0]) return;
    g_backup.attached = g_backup.fresh = true;
    g_backup.frames   = 0;
    LOG_INFO("backup RAM: %s is back", g_backup.set);
}

/* Hand the image to the board again, for a frontend that filled g_backup.image
 * after the board was installed (libretro loads the .srm after load_game). */
static inline void backup_ram_reload(uint8_t *back) {
    if (g_backup.attached && back) memcpy(back, g_backup.image, BACKUP_RAM_KEEP);
}

/* ---- The board's side ------------------------------------------------------ */

/* mem_init, before it clears the bus: a reset keeps what the board holds. */
static inline void backup_ram_before_reset(const uint8_t *back) {
    if (g_backup.attached && !g_backup.fresh) {
        if (memcmp(g_backup.image, back, BACKUP_RAM_KEEP)) g_backup.dirty = true;
        memcpy(g_backup.image, back, BACKUP_RAM_KEEP);
    }
}

/* mem_init, after: the battery's contents go back in. */
static inline void backup_ram_after_reset(uint8_t *back) {
    g_backup.board = back;
    if (!g_backup.attached) return;
    memcpy(back, g_backup.image, BACKUP_RAM_KEEP);
    g_backup.fresh = false;
}

/* Every frame edge (emu thread): once a minute, store it if the game changed it. */
static inline void backup_ram_frame(const uint8_t *back) {
    if (!g_backup.attached || g_backup.fresh) return;
    if (++g_backup.frames < (g_backup.to_file ? BACKUP_RAM_CHECK_FRAMES : 60u)) return;
    g_backup.frames = 0;
    if (memcmp(g_backup.image, back, BACKUP_RAM_KEEP)) {
        memcpy(g_backup.image, back, BACKUP_RAM_KEEP);
        g_backup.dirty = true;
    }
    if (g_backup.dirty) backup_ram_store();
}

#endif
