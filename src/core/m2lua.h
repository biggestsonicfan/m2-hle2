/*
 * m2lua.h -- game scripts in Lua 5.5, the way M2emulator 1.1a ran them
 * (Pinboard #958).
 *
 * M2emulator (ElSemi's Model 2 emulator) loads SCRIPTS\<romset>.lua when a game
 * starts and calls the functions it defines: Init, Reset, Frame, PostDraw, End.
 * The script reaches the board through helper functions (I960_ReadByte,
 * Romset_PatchDWord, Video_DrawText, Input_IsKeyPressed, Model2_SetWideScreen,
 * ...) and can publish named cheats and options in a global `Options` table.
 * Its own documentation is SCRIPTS\model2lua.txt; what that file leaves out was
 * read off EMULATOR.EXE (Lua 5.1.4 inside) with rea + Ghidra:
 *
 *   - the script loads once, at game load: package.path gets the SCRIPTS folder
 *     (LUA_PATH=...;.\SCRIPTS\?.lua), the helpers are registered, the chunk runs,
 *     and then the Options table is walked once with lua_next: name (default the
 *     key), values (default {"Off","On"}), and whether runfunc / changefunc are
 *     functions. A compile error disables the script (a message box there).
 *   - Frame runs after each emulated frame, under the script's critical section;
 *     right after it, every option whose value is not 0 and that has a runfunc
 *     gets runfunc(value), in Options order. (M2emulator skips the runfuncs when
 *     the script has no Frame at all; here they run either way.)
 *   - a Game-menu change sets Options.<key>.value and calls changefunc(value).
 *   - AND/OR/XOR/NOT/SHL/SHR round each argument to an integer, work on its low
 *     32 bits and return the result as an UNSIGNED 32-bit number; SHR is a
 *     logical shift and the count is taken mod 32.
 *   - a callback's runtime error raises a message box ("Script Frame error")
 *     and the script goes on; here it is logged, the first time and then every
 *     600th, and kept in m2lua_status.
 *
 * Lua 5.5 has had integers and the bitwise operators (& | ~ << >> and unary ~)
 * since 5.3, so a script can write `v & 0xF` where M2emulator's needed
 * AND(v, 0xF). The old helpers stay for the old scripts. Every value the board
 * hands a script (I960_Read*, Romset_Read*) is an integer, never a float, so
 * the operators take it as it is. Mind that Lua's integers are 64 bits: `~v`
 * and `v << 8` need `& 0xFFFFFFFF` to stay a 32-bit word; NOT() and SHL() do it.
 *
 * Threads: the Lua state is touched ONLY with the emu mutex held (the frame
 * edge, a reset, a load, the menu's option change, the bridge's eval), which is
 * M2emulator's critical section. PostDraw runs on the emu thread too, right
 * after Frame: what it draws is recorded and handed to the window as a list
 * (m2lua_take_draw), which ui/m2lua_ui.h paints over the game. Its coordinates
 * are the board's picture, 496 x 384, scaled with the picture.
 *
 * A script that writes the board would desync netplay and replays, so the run
 * loop does not call it while a session, a replay or a follow owns the board
 * (emu_script_may_run, emu_thread.h): its ROM patches come off, and it runs
 * again from its start when the session ends (docs/invariants/scripts.md).
 *
 * Video_CreateSurfaceFromFile decodes through the host's g_m2lua_decode_image
 * (the desktop's wraps ui/retro_shader.h's stb_image; the web has no files).
 */
#ifndef M2LUA_H
#define M2LUA_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "log.h"
#include "json_min.h"
#include "thread_mutex.h"
#include "rom_loader.h"
#include "../board/memory.h"
#include "../board/constants.h"

#define M2LUA_DRAW_MAX     256
#define M2LUA_TEXT_MAX     128
#define M2LUA_FONT_MAX      16
#define M2LUA_SURFACE_MAX   32
#define M2LUA_OPTION_MAX    32
#define M2LUA_VALUE_MAX     16
#define M2LUA_KEY_MAX      512   /* DIK codes below 0x100, JOYn+... from 0x100 */
#define M2LUA_TEXT_SIZE   13.0f  /* Video_DrawText's font, in board pixels */
#define M2LUA_ERROR_EVERY  600

typedef enum { M2LUA_DRAW_TEXT, M2LUA_DRAW_SURFACE } m2lua_draw_kind_t;

/* One thing PostDraw drew: text (font -1 = the default) or a surface. */
typedef struct {
    uint8_t  kind;
    int8_t   font;
    int8_t   surface;
    float    x, y;
    uint32_t color;        /* 0xAARRGGBB; a script's 0xRRGGBB is opaque */
    char     text[M2LUA_TEXT_MAX];
} m2lua_draw_t;

typedef struct { bool used; bool bold; float size; } m2lua_font_t;

/* A ROM byte as it was before a Romset_Patch*: the patches come off when the
 * script does, or when a session takes the board (m2lua_suspend). */
typedef struct { uint8_t area; uint8_t old; uint32_t off; } m2lua_patch_t;

/* A PNG a script loaded. The window uploads it when gen moves on, and lets
 * its texture go when used falls. */
typedef struct {
    bool     used;
    int      w, h;
    uint8_t *rgba;
    uint32_t gen;
} m2lua_surface_t;

typedef struct {
    char key[32];
    char name[64];
    char values[M2LUA_VALUE_MAX][32];
    int  nvalues;
    int  value;
    int  dflt;           /* the value the script gave it (RetroArch's cheats go back to it) */
    bool runfunc, changefunc;
} m2lua_option_t;

/* The Model2_Set* switches. The sdl3 frontend and the web draw them
 * (ui/m2lua_wide.h); the others keep and report them, so a script's view of
 * them holds. */
typedef struct {
    bool widescreen;
    bool stretch_a_low, stretch_b_low, stretch_a_high, stretch_b_high;
} m2lua_video_flags_t;

typedef struct {
    lua_State     *L;
    memory_bus_t  *bus;
    romset_t      *rom;
    char           dir[512];        /* the scripts folder */
    char           path[600];       /* the loaded script, "" for none */
    char           set[32];
    char           error[256];      /* the last error, "" for none */
    uint32_t       error_count;
    uint64_t       frames;          /* Frame calls */
    bool           loaded;
    bool           disabled;        /* --no-scripts */
    bool           suspended;       /* a session has the board: patches off, nothing runs */
    char          *source;          /* a script given as text (the web's scratchpad), NULL: the file */
    m2lua_patch_t *patches;         /* the undo log of every ROM byte patched */
    size_t         npatches, patch_cap;
    m2lua_video_flags_t video;
    m2lua_option_t options[M2LUA_OPTION_MAX];
    int            noptions;
    m2lua_font_t   fonts[M2LUA_FONT_MAX];
    m2lua_surface_t surfaces[M2LUA_SURFACE_MAX];
    /* PostDraw's list: built on the emu thread, published at its end. */
    m2lua_draw_t   build[M2LUA_DRAW_MAX];
    int            nbuild;
    m2lua_draw_t   shown[M2LUA_DRAW_MAX];
    int            nshown;
    uint32_t       shown_gen;
    emu_mutex_t    draw_mutex;
    bool           draw_mutex_ready;
    /* Held keys, by M2emulator's key number (DIK_*, JOYn + button), set by
     * the host's input (m2lua_set_key). */
    volatile uint8_t keys[M2LUA_KEY_MAX];
} m2lua_t;

static m2lua_t g_m2lua;

/* ---- Errors ------------------------------------------------------------- */

static inline void m2lua_error(const char *where) {
    lua_State *L = g_m2lua.L;
    const char *msg = lua_tostring(L, -1);
    snprintf(g_m2lua.error, sizeof g_m2lua.error, "%s: %s", where, msg ? msg : "(no message)");
    if (g_m2lua.error_count++ % M2LUA_ERROR_EVERY == 0)
        LOG_ERROR("script %s (%u so far)", g_m2lua.error, g_m2lua.error_count);
    lua_pop(L, 1);
}

/* Call the function pushed before its nargs arguments. False (and logged)
 * on an error. */
static inline bool m2lua_pcall(int nargs, const char *where) {
    if (lua_pcall(g_m2lua.L, nargs, 0, 0) == LUA_OK) return true;
    m2lua_error(where);
    return false;
}

/* Call a global function by name, if the script defines it. */
static inline void m2lua_call_global(const char *fn) {
    lua_State *L = g_m2lua.L;
    if (lua_getglobal(L, fn) != LUA_TFUNCTION) { lua_pop(L, 1); return; }
    m2lua_pcall(0, fn);
}

/* ---- Bit helpers (M2emulator's AND, OR, ...) ----------------------------- */

/* An argument as M2emulator took it: rounded, low 32 bits. */
static inline uint32_t m2lua_arg32(lua_State *L, int i) {
    if (lua_isinteger(L, i)) return (uint32_t)lua_tointeger(L, i);
    lua_Number n = luaL_checknumber(L, i);
    return (uint32_t)(int64_t)nearbyint(n);
}

static inline int m2lua_ret32(lua_State *L, uint32_t v) {
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}

static int m2lua_AND(lua_State *L) { return m2lua_ret32(L, m2lua_arg32(L, 1) & m2lua_arg32(L, 2)); }
static int m2lua_OR (lua_State *L) { return m2lua_ret32(L, m2lua_arg32(L, 1) | m2lua_arg32(L, 2)); }
static int m2lua_XOR(lua_State *L) { return m2lua_ret32(L, m2lua_arg32(L, 1) ^ m2lua_arg32(L, 2)); }
static int m2lua_NOT(lua_State *L) { return m2lua_ret32(L, ~m2lua_arg32(L, 1)); }
static int m2lua_SHL(lua_State *L) { return m2lua_ret32(L, m2lua_arg32(L, 1) << (m2lua_arg32(L, 2) & 31)); }
static int m2lua_SHR(lua_State *L) { return m2lua_ret32(L, m2lua_arg32(L, 1) >> (m2lua_arg32(L, 2) & 31)); }

/* ---- The i960's memory map ----------------------------------------------- */

static int m2lua_i960_read(lua_State *L, int size) {
    uint32_t a = m2lua_arg32(L, 1);
    memory_bus_t *bus = g_m2lua.bus;
    uint32_t v = 0;
    if (bus) v = size == 1 ? mem_read8(bus, a) : size == 2 ? mem_read16(bus, a) : mem_read32(bus, a);
    return m2lua_ret32(L, v);
}

static int m2lua_i960_write(lua_State *L, int size) {
    uint32_t a = m2lua_arg32(L, 1), v = m2lua_arg32(L, 2);
    memory_bus_t *bus = g_m2lua.bus;
    if (!bus) return 0;
    if (size == 1)      mem_write8(bus, a, v & 0xFF);
    else if (size == 2) mem_write16(bus, a, v & 0xFFFF);
    else                mem_write32(bus, a, v);
    return 0;
}

static int m2lua_I960_ReadByte  (lua_State *L) { return m2lua_i960_read(L, 1); }
static int m2lua_I960_ReadWord  (lua_State *L) { return m2lua_i960_read(L, 2); }
static int m2lua_I960_ReadDWord (lua_State *L) { return m2lua_i960_read(L, 4); }
static int m2lua_I960_WriteByte (lua_State *L) { return m2lua_i960_write(L, 1); }
static int m2lua_I960_WriteWord (lua_State *L) { return m2lua_i960_write(L, 2); }
static int m2lua_I960_WriteDWord(lua_State *L) { return m2lua_i960_write(L, 4); }

/* The host address of a RAM byte, for a cheat finder; 0 for MMIO, as there. */
static int m2lua_I960_GetRamPtr(lua_State *L) {
    uint32_t a = m2lua_arg32(L, 1);
    mem_region_t *r = g_m2lua.bus ? mem_find_region(g_m2lua.bus, a) : NULL;
    if (!r || !r->data || r->read_cb) { lua_pushinteger(L, 0); return 1; }
    lua_pushinteger(L, (lua_Integer)(uintptr_t)(r->data + (a - r->base)));
    return 1;
}

/* ---- ROM areas ----------------------------------------------------------- */

/* M2emulator's area numbers onto m2-hle2's regions (romset_t), each in the
 * byte order the board reads it (romset_save_dir's images). 7, 8 (DSB), 9
 * (comm board) and 11 (video board) have nothing here. */
static inline uint8_t *m2lua_area(int area, size_t *size) {
    romset_t *rs = g_m2lua.rom;
    *size = 0;
    if (!rs) return NULL;
    static const int8_t k_region[12] = { 0, 1, 2, 3, 4, 5, 6, -1, -1, -1, 7, -1 };
    if (area < 0 || area >= 12 || k_region[area] < 0) return NULL;
    size_t *z;
    uint8_t **r = romset_region(rs, k_region[area], &z);
    *size = *z;
    return *r;
}

/* Patch one byte, keeping what it was. */
static inline void m2lua_patch(int area, uint8_t *p, uint32_t off, uint8_t v) {
    if (p[off] == v) return;
    if (g_m2lua.npatches == g_m2lua.patch_cap) {
        size_t cap = g_m2lua.patch_cap ? g_m2lua.patch_cap * 2 : 256;
        m2lua_patch_t *n = realloc(g_m2lua.patches, cap * sizeof *n);
        if (!n) return;
        g_m2lua.patches = n;
        g_m2lua.patch_cap = cap;
    }
    g_m2lua.patches[g_m2lua.npatches++] = (m2lua_patch_t){ (uint8_t)area, p[off], off };
    p[off] = v;
}

/* Put every patched ROM byte back, newest first. */
static inline void m2lua_unpatch(void) {
    while (g_m2lua.npatches) {
        const m2lua_patch_t *q = &g_m2lua.patches[--g_m2lua.npatches];
        size_t n;
        uint8_t *p = m2lua_area(q->area, &n);
        if (p && q->off < n) p[q->off] = q->old;
    }
}

static int m2lua_rom_access(lua_State *L, int size, bool write) {
    int      area = (int)luaL_checkinteger(L, 1);
    uint32_t off  = m2lua_arg32(L, 2);
    size_t   n;
    uint8_t *p = m2lua_area(area, &n);
    if (!p || (size_t)off + (size_t)size > n)
        return write ? 0 : m2lua_ret32(L, 0);
    if (write) {
        uint32_t v = m2lua_arg32(L, 3);
        for (int i = 0; i < size; i++) m2lua_patch(area, p, off + (uint32_t)i, (uint8_t)(v >> (8 * i)));
        return 0;
    }
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)p[off + i] << (8 * i);
    return m2lua_ret32(L, v);
}

static int m2lua_Romset_ReadByte  (lua_State *L) { return m2lua_rom_access(L, 1, false); }
static int m2lua_Romset_ReadWord  (lua_State *L) { return m2lua_rom_access(L, 2, false); }
static int m2lua_Romset_ReadDWord (lua_State *L) { return m2lua_rom_access(L, 4, false); }
static int m2lua_Romset_PatchByte (lua_State *L) { return m2lua_rom_access(L, 1, true); }
static int m2lua_Romset_PatchWord (lua_State *L) { return m2lua_rom_access(L, 2, true); }
static int m2lua_Romset_PatchDWord(lua_State *L) { return m2lua_rom_access(L, 4, true); }

/* ---- Video: what PostDraw draws ------------------------------------------ */

static inline uint32_t m2lua_color(lua_State *L, int i) {
    uint32_t c = lua_isnoneornil(L, i) ? 0xFFFFFFu : m2lua_arg32(L, i);
    return (c >> 24) ? c : (c | 0xFF000000u);
}

static inline m2lua_draw_t *m2lua_draw_new(m2lua_draw_kind_t kind, float x, float y) {
    if (g_m2lua.nbuild >= M2LUA_DRAW_MAX) return NULL;
    m2lua_draw_t *d = &g_m2lua.build[g_m2lua.nbuild++];
    memset(d, 0, sizeof *d);
    d->kind = (uint8_t)kind;
    d->font = d->surface = -1;
    d->x = x;
    d->y = y;
    return d;
}

static inline int m2lua_draw_text(lua_State *L, int font, int at) {
    float x = (float)luaL_checknumber(L, at), y = (float)luaL_checknumber(L, at + 1);
    const char *s = luaL_tolstring(L, at + 2, NULL);   /* M2emulator printed numbers too */
    m2lua_draw_t *d = m2lua_draw_new(M2LUA_DRAW_TEXT, x, y);
    if (d) {
        d->font  = (int8_t)font;
        d->color = m2lua_color(L, at + 3);
        snprintf(d->text, sizeof d->text, "%s", s);
    }
    lua_pop(L, 1);
    return 0;
}

static int m2lua_Video_DrawText(lua_State *L) { return m2lua_draw_text(L, -1, 1); }

static int m2lua_Video_CreateFont(lua_State *L) {
    float size = (float)luaL_optnumber(L, 2, M2LUA_TEXT_SIZE);
    bool  bold = lua_toboolean(L, 3) && !(lua_isinteger(L, 3) && lua_tointeger(L, 3) == 0);
    for (int i = 0; i < M2LUA_FONT_MAX; i++) {
        m2lua_font_t *f = &g_m2lua.fonts[i];
        if (f->used) continue;
        *f = (m2lua_font_t){ true, bold, size > 1.0f ? size : M2LUA_TEXT_SIZE };
        lua_pushinteger(L, i + 1);
        return 1;
    }
    lua_pushinteger(L, 0);
    return 1;
}

static inline int m2lua_handle(lua_State *L, int i, int max) {
    lua_Integer h = luaL_optinteger(L, i, 0);
    return (h >= 1 && h <= max) ? (int)h - 1 : -1;
}

static int m2lua_Video_ReleaseFont(lua_State *L) {
    int f = m2lua_handle(L, 1, M2LUA_FONT_MAX);
    if (f >= 0) g_m2lua.fonts[f].used = false;
    return 0;
}

static int m2lua_Video_DrawTextWithFont(lua_State *L) {
    int f = m2lua_handle(L, 1, M2LUA_FONT_MAX);
    return m2lua_draw_text(L, (f >= 0 && g_m2lua.fonts[f].used) ? f : -1, 2);
}

/* A file name as a script gives it: relative ones from the scripts folder
 * when the file is there, else from the working directory. */
static inline void m2lua_resolve(const char *name, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s", g_m2lua.dir, name);
    FILE *f = fopen(out, "rb");
    if (f) { fclose(f); return; }
    snprintf(out, cap, "%s", name);
}

/* The host's image decoder (the window's stb_image): RGBA8, malloc'd, or NULL
 * when it cannot. NULL: a host with no window, and no surfaces. */
static uint8_t *(*g_m2lua_decode_image)(const uint8_t *buf, int len, int *w, int *h);

/* A whole file, malloc'd. */
static inline uint8_t *m2lua_read_file(const char *path, int *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 && n < (64L << 20) ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *len = (int)n;
    return buf;
}

static inline uint8_t *m2lua_load_image(const char *path, int *w, int *h) {
    int len = 0;
    uint8_t *buf = g_m2lua_decode_image ? m2lua_read_file(path, &len) : NULL;
    uint8_t *px = buf ? g_m2lua_decode_image(buf, len, w, h) : NULL;
    free(buf);
    return px;
}

static int m2lua_Video_CreateSurfaceFromFile(lua_State *L) {
    char path[700];
    m2lua_resolve(luaL_checkstring(L, 1), path, sizeof path);
    for (int i = 0; i < M2LUA_SURFACE_MAX; i++) {
        m2lua_surface_t *s = &g_m2lua.surfaces[i];
        if (s->used) continue;
        int w, h;
        uint8_t *px = m2lua_load_image(path, &w, &h);
        if (!px) { LOG_WARN("script: no image in %s", path); break; }
        emu_mutex_lock(&g_m2lua.draw_mutex);
        free(s->rgba);
        *s = (m2lua_surface_t){ true, w, h, px, s->gen + 1 };
        emu_mutex_unlock(&g_m2lua.draw_mutex);
        lua_pushinteger(L, i + 1);
        return 1;
    }
    lua_pushinteger(L, 0);
    return 1;
}

static int m2lua_Video_ReleaseSurface(lua_State *L) {
    int s = m2lua_handle(L, 1, M2LUA_SURFACE_MAX);
    if (s < 0) return 0;
    emu_mutex_lock(&g_m2lua.draw_mutex);
    g_m2lua.surfaces[s].used = false;   /* the window frees it with its texture */
    emu_mutex_unlock(&g_m2lua.draw_mutex);
    return 0;
}

static int m2lua_Video_DrawSurface(lua_State *L) {
    int s = m2lua_handle(L, 1, M2LUA_SURFACE_MAX);
    float x = (float)luaL_checknumber(L, 2), y = (float)luaL_checknumber(L, 3);
    if (s < 0 || !g_m2lua.surfaces[s].used) return 0;
    m2lua_draw_t *d = m2lua_draw_new(M2LUA_DRAW_SURFACE, x, y);
    if (d) d->surface = (int8_t)s;
    return 0;
}

static int m2lua_Video_GetScreenSize(lua_State *L) {
    lua_pushinteger(L, VIDEO_WIDTH);
    lua_pushinteger(L, VIDEO_HEIGHT);
    return 2;
}

/* ---- Input and the Model 2 switches -------------------------------------- */

static int m2lua_Input_IsKeyPressed(lua_State *L) {
    lua_Integer k = luaL_checkinteger(L, 1);
    lua_pushinteger(L, (k >= 0 && k < M2LUA_KEY_MAX && g_m2lua.keys[k]) ? 1 : 0);
    return 1;
}

static inline int m2lua_set_flag(lua_State *L, bool *flag) {
    *flag = lua_toboolean(L, 1) && !(lua_isnumber(L, 1) && lua_tonumber(L, 1) == 0);
    return 0;
}

static int m2lua_SetWideScreen  (lua_State *L) { return m2lua_set_flag(L, &g_m2lua.video.widescreen); }
static int m2lua_SetStretchALow (lua_State *L) { return m2lua_set_flag(L, &g_m2lua.video.stretch_a_low); }
static int m2lua_SetStretchBLow (lua_State *L) { return m2lua_set_flag(L, &g_m2lua.video.stretch_b_low); }
static int m2lua_SetStretchAHigh(lua_State *L) { return m2lua_set_flag(L, &g_m2lua.video.stretch_a_high); }
static int m2lua_SetStretchBHigh(lua_State *L) { return m2lua_set_flag(L, &g_m2lua.video.stretch_b_high); }

static const luaL_Reg k_m2lua_api[] = {
    { "AND", m2lua_AND }, { "OR", m2lua_OR }, { "XOR", m2lua_XOR },
    { "NOT", m2lua_NOT }, { "SHL", m2lua_SHL }, { "SHR", m2lua_SHR },
    { "Romset_ReadByte",  m2lua_Romset_ReadByte },  { "Romset_ReadWord",   m2lua_Romset_ReadWord },
    { "Romset_ReadDWord", m2lua_Romset_ReadDWord }, { "Romset_PatchByte",  m2lua_Romset_PatchByte },
    { "Romset_PatchWord", m2lua_Romset_PatchWord }, { "Romset_PatchDWord", m2lua_Romset_PatchDWord },
    { "I960_ReadByte",  m2lua_I960_ReadByte },  { "I960_ReadWord",   m2lua_I960_ReadWord },
    { "I960_ReadDWord", m2lua_I960_ReadDWord }, { "I960_WriteByte",  m2lua_I960_WriteByte },
    { "I960_WriteWord", m2lua_I960_WriteWord }, { "I960_WriteDWord", m2lua_I960_WriteDWord },
    { "I960_GetRamPtr", m2lua_I960_GetRamPtr },
    { "Video_DrawText",        m2lua_Video_DrawText },
    { "Video_CreateFont",      m2lua_Video_CreateFont },
    { "Video_ReleaseFont",     m2lua_Video_ReleaseFont },
    { "Video_DrawTextWithFont", m2lua_Video_DrawTextWithFont },
    { "Video_CreateSurfaceFromFile", m2lua_Video_CreateSurfaceFromFile },
    { "Video_DrawSurface",     m2lua_Video_DrawSurface },
    { "Video_ReleaseSurface",  m2lua_Video_ReleaseSurface },
    { "Video_GetScreenSize",   m2lua_Video_GetScreenSize },
    { "Input_IsKeyPressed",    m2lua_Input_IsKeyPressed },
    { "Model2_SetWideScreen",   m2lua_SetWideScreen },
    { "Model2_SetStretchALow",  m2lua_SetStretchALow },
    { "Model2_SetStretchBLow",  m2lua_SetStretchBLow },
    { "Model2_SetStretchAHigh", m2lua_SetStretchAHigh },
    { "Model2_SetStretchBHigh", m2lua_SetStretchBHigh },
    { NULL, NULL }
};

/* ---- Options ------------------------------------------------------------- */

/* Push Options[key] (nil when there is none). */
static inline int m2lua_push_option(const char *key) {
    lua_State *L = g_m2lua.L;
    if (lua_getglobal(L, "Options") != LUA_TTABLE) { lua_pop(L, 1); lua_pushnil(L); return LUA_TNIL; }
    int t = lua_getfield(L, -1, key);
    lua_remove(L, -2);
    return t;
}

/* The option's values list, or M2emulator's {"Off","On"}. Option table on top. */
static inline void m2lua_option_values(lua_State *L, m2lua_option_t *o) {
    o->nvalues = 0;
    if (lua_getfield(L, -1, "values") == LUA_TTABLE) {
        for (int i = 1; i <= M2LUA_VALUE_MAX; i++) {
            if (lua_geti(L, -1, i) == LUA_TNIL) { lua_pop(L, 1); break; }
            snprintf(o->values[o->nvalues++], sizeof o->values[0], "%s", luaL_tolstring(L, -1, NULL));
            lua_pop(L, 2);
        }
    }
    lua_pop(L, 1);
    if (o->nvalues == 0) {
        snprintf(o->values[0], sizeof o->values[0], "Off");
        snprintf(o->values[1], sizeof o->values[1], "On");
        o->nvalues = 2;
    }
}

static inline bool m2lua_field_is_fn(lua_State *L, const char *field) {
    bool fn = lua_getfield(L, -1, field) == LUA_TFUNCTION;
    lua_pop(L, 1);
    return fn;
}

/* One Options entry: key at -2, its table at -1. */
static inline void m2lua_option_read(lua_State *L) {
    if (g_m2lua.noptions >= M2LUA_OPTION_MAX || lua_type(L, -2) != LUA_TSTRING
        || lua_type(L, -1) != LUA_TTABLE) return;
    m2lua_option_t *o = &g_m2lua.options[g_m2lua.noptions++];
    memset(o, 0, sizeof *o);
    snprintf(o->key, sizeof o->key, "%s", lua_tostring(L, -2));
    lua_getfield(L, -1, "name");
    snprintf(o->name, sizeof o->name, "%s", lua_isstring(L, -1) ? lua_tostring(L, -1) : o->key);
    lua_pop(L, 1);
    m2lua_option_values(L, o);
    o->runfunc    = m2lua_field_is_fn(L, "runfunc");
    o->changefunc = m2lua_field_is_fn(L, "changefunc");
    lua_getfield(L, -1, "value");
    o->value = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : 0;
    o->dflt  = o->value;
    lua_pop(L, 1);
    lua_pushinteger(L, o->value);       /* Options.<key>.value is always there */
    lua_setfield(L, -2, "value");
}

static int m2lua_option_cmp(const void *a, const void *b) {
    return strcmp(((const m2lua_option_t *)a)->key, ((const m2lua_option_t *)b)->key);
}

/* Walk Options once, after the chunk ran, as M2emulator did. Sorted by key,
 * so the menu does not follow Lua's hash order. */
static inline void m2lua_options_scan(void) {
    lua_State *L = g_m2lua.L;
    g_m2lua.noptions = 0;
    if (lua_getglobal(L, "Options") == LUA_TTABLE) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            m2lua_option_read(L);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    qsort(g_m2lua.options, (size_t)g_m2lua.noptions, sizeof g_m2lua.options[0], m2lua_option_cmp);
}

/* Call Options[key][fn](value). */
static inline void m2lua_option_call(const m2lua_option_t *o, const char *fn) {
    lua_State *L = g_m2lua.L;
    if (m2lua_push_option(o->key) != LUA_TTABLE) { lua_pop(L, 1); return; }
    if (lua_getfield(L, -1, fn) != LUA_TFUNCTION) { lua_pop(L, 2); return; }
    lua_remove(L, -2);
    lua_pushinteger(L, o->value);
    m2lua_pcall(1, fn);
}

/* The menu's change: set Options.<key>.value, then changefunc(value). Call
 * with the emu mutex held. */
static inline void m2lua_set_option(int i, int value) {
    if (!g_m2lua.loaded || i < 0 || i >= g_m2lua.noptions) return;
    m2lua_option_t *o = &g_m2lua.options[i];
    if (value < 0 || value >= o->nvalues) return;
    o->value = value;
    lua_State *L = g_m2lua.L;
    if (m2lua_push_option(o->key) == LUA_TTABLE) {
        lua_pushinteger(L, value);
        lua_setfield(L, -2, "value");
    }
    lua_pop(L, 1);
    if (o->changefunc) m2lua_option_call(o, "changefunc");
}

static inline int m2lua_option_index(const char *key) {
    for (int i = 0; i < g_m2lua.noptions; i++)
        if (strcmp(g_m2lua.options[i].key, key) == 0) return i;
    return -1;
}

/* ---- Load, callbacks, unload -------------------------------------------- */

/* package.path: the scripts folder first, so require("model2") finds it. The
 * web has no files, and its fopen stub opens any name and then fails the read
 * (ENOSYS), which stops require at the first candidate: no paths there, so it
 * goes on to the built-ins below. */
static inline void m2lua_set_path(lua_State *L) {
    lua_getglobal(L, "package");
#ifdef __EMSCRIPTEN__
    lua_pushliteral(L, "");
    lua_setfield(L, -2, "cpath");
    lua_pushliteral(L, "");
#else
    lua_pushfstring(L, "%s/?.lua;%s/?/init.lua;./?.lua", g_m2lua.dir, g_m2lua.dir);
#endif
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);
}

/* M2emulator's scripts begin with require("model2"), which reads model2.lua and
 * common.lua from its SCRIPTS folder: the board's memory map, the HEX helpers
 * and the JOYn key numbers. The web has no folder, and the repo ships neither
 * file, so these stand in for them (written here, same names and values) when
 * the folder has no such file: a copy in the folder still wins. */
static const char k_m2lua_common_lua[] =
    "HEX2CHAR = \"0123456789ABCDEF\"\n"
    "local function hex(v, n) return string.format(\"%0\" .. n .. \"X\", math.floor(v) & ((1 << (4 * n)) - 1)) end\n"
    "function HEX4(v)  return hex(v, 1) end\n"
    "function HEX8(v)  return hex(v, 2) end\n"
    "function HEX16(v) return hex(v, 4) end\n"
    "function HEX32(v) return hex(v, 8) end\n"
    "JOY1, JOY2, JOY3, JOY4 = 0x100, 0x200, 0x300, 0x400\n"
    "JOY_LEFT, JOY_RIGHT, JOY_UP, JOY_DOWN = 0x0, 0x1, 0x2, 0x3\n"
    "JOY_LEFT2, JOY_RIGHT2, JOY_UP2, JOY_DOWN2 = 0x4, 0x5, 0x6, 0x7\n"
    "JOY_LEFT3, JOY_RIGHT3, JOY_UP3, JOY_DOWN3 = 0x8, 0x9, 0xA, 0xB\n"
    "for i = 1, 8 do _G[\"JOY_BUTTON\" .. i] = i << 4 end\n";

static const char k_m2lua_model2_lua[] =
    "require(\"common\")\n"
    "ROMBASE, RAM2BASE, RAMBASE = 0x00000000, 0x00200000, 0x00500000\n"
    "GEOBASE, COPBASE, DLISTBASE = 0x00800000, 0x00880000, 0x00900000\n"
    "TMAPBASE, TMAPGFXBASE, COLORXLAT = 0x01000000, 0x01080000, 0x01810000\n"
    "IOBASE, BACKUPBASE = 0x01C00000, 0x01D00000\n"
    "DATAROMBASE, XTRADATABASE = 0x02000000, 0x06000000\n"
    "TEX0BASE, TEX1BASE, LUMAXLAT, FRAMEBUFBASE = 0x11000000, 0x11200000, 0x11400000, 0x12C00000\n";

/* package.searchers' last: the stand-ins above, after the folder's files. */
static int m2lua_builtin_searcher(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    const char *src = !strcmp(name, "model2") ? k_m2lua_model2_lua
                    : !strcmp(name, "common") ? k_m2lua_common_lua : NULL;
    if (!src) { lua_pushfstring(L, "no built-in module '%s'", name); return 1; }
    if (luaL_loadbuffer(L, src, strlen(src), name) != LUA_OK) return lua_error(L);
    lua_pushliteral(L, ":built-in:");
    return 2;
}

static inline void m2lua_add_builtins(lua_State *L) {
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "searchers");
    lua_pushcfunction(L, m2lua_builtin_searcher);
    lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
    lua_pop(L, 2);
}

static inline lua_State *m2lua_new_state(void) {
    lua_State *L = luaL_newstate();
    if (!L) return NULL;
    luaL_openlibs(L);
    m2lua_set_path(L);
    m2lua_add_builtins(L);
    lua_pushglobaltable(L);
    luaL_setfuncs(L, k_m2lua_api, 0);
    lua_pop(L, 1);
    return L;
}

static inline void m2lua_clear_media(void) {
    memset(g_m2lua.fonts, 0, sizeof g_m2lua.fonts);
    emu_mutex_lock(&g_m2lua.draw_mutex);
    for (int i = 0; i < M2LUA_SURFACE_MAX; i++) g_m2lua.surfaces[i].used = false;
    g_m2lua.nbuild = g_m2lua.nshown = 0;
    g_m2lua.shown_gen++;
    emu_mutex_unlock(&g_m2lua.draw_mutex);
}

/* End(), then drop the state and take the ROM patches off. Call with the
 * emu mutex held, and before the ROM set the script patched is let go of. */
static inline void m2lua_unload(void) {
    m2lua_unpatch();
    if (g_m2lua.L) {
        if (g_m2lua.loaded && !g_m2lua.suspended) m2lua_call_global("End");
        lua_close(g_m2lua.L);
    }
    g_m2lua.suspended = false;
    g_m2lua.L = NULL;
    g_m2lua.loaded = false;
    g_m2lua.noptions = 0;
    g_m2lua.path[0] = 0;
    memset(&g_m2lua.video, 0, sizeof g_m2lua.video);
    m2lua_clear_media();
}

static inline void m2lua_set_dir(const char *dir) {
    snprintf(g_m2lua.dir, sizeof g_m2lua.dir, "%s", dir && dir[0] ? dir : "scripts");
}

static inline void m2lua_init_once(void) {
    if (g_m2lua.draw_mutex_ready) return;
    emu_mutex_init(&g_m2lua.draw_mutex);
    g_m2lua.draw_mutex_ready = true;
    if (!g_m2lua.dir[0]) m2lua_set_dir(NULL);
}

static inline bool m2lua_file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* Run the chunk at path (src NULL), or src, in a new state. False, with the
 * error kept, when it does not compile or its first run fails: the script is
 * off then. */
static inline bool m2lua_run_chunk(const char *path, const char *src) {
    g_m2lua.L = m2lua_new_state();
    if (!g_m2lua.L) { snprintf(g_m2lua.error, sizeof g_m2lua.error, "out of memory"); return false; }
    int rc = src ? luaL_loadbuffer(g_m2lua.L, src, strlen(src), path) : luaL_loadfile(g_m2lua.L, path);
    if (rc == LUA_OK) rc = lua_pcall(g_m2lua.L, 0, 0, 0);
    if (rc == LUA_OK) return true;
    m2lua_error("load");
    lua_close(g_m2lua.L);
    g_m2lua.L = NULL;
    return false;
}

static inline void m2lua_begin(const char *set, memory_bus_t *bus, romset_t *rom) {
    m2lua_init_once();
    m2lua_unload();
    g_m2lua.bus = bus;
    g_m2lua.rom = rom;
    g_m2lua.error[0] = 0;
    g_m2lua.error_count = 0;
    g_m2lua.frames = 0;
    snprintf(g_m2lua.set, sizeof g_m2lua.set, "%s", set ? set : "");
}

/* The chunk has run: read its Options and call Init(). */
static inline bool m2lua_finish(const char *path) {
    snprintf(g_m2lua.path, sizeof g_m2lua.path, "%s", path);
    g_m2lua.loaded = true;
    m2lua_options_scan();
    LOG_INFO("script: %s (%s, %d option%s)", path, LUA_RELEASE,
             g_m2lua.noptions, g_m2lua.noptions == 1 ? "" : "s");
    m2lua_call_global("Init");
    return true;
}

/* The game's script, <dir>/<set>.lua, for a game just loaded: load it, read
 * its Options and call Init(). Call with the emu mutex held. */
static inline bool m2lua_load(const char *set, memory_bus_t *bus, romset_t *rom) {
    m2lua_begin(set, bus, rom);
    free(g_m2lua.source);
    g_m2lua.source = NULL;
    if (g_m2lua.disabled || !set || !set[0]) return false;
    char path[sizeof g_m2lua.path];
    snprintf(path, sizeof path, "%s/%s.lua", g_m2lua.dir, set);
    if (!m2lua_file_exists(path)) return false;
    if (!m2lua_run_chunk(path, NULL)) return false;
    return m2lua_finish(path);
}

/* The game's script as text (the web page's scratchpad); "" or NULL: none.
 * now false (a session has the board): kept, and run when the session lets
 * go of it (m2lua_frame_hook). Emu mutex held. */
static inline bool m2lua_load_source(const char *set, const char *src, memory_bus_t *bus,
                                     romset_t *rom, bool now) {
    size_t n = src ? strlen(src) : 0;
    char *copy = n ? malloc(n + 1) : NULL;
    if (copy) memcpy(copy, src, n + 1);
    m2lua_begin(set, bus, rom);
    free(g_m2lua.source);
    g_m2lua.source = copy;
    if (!copy || g_m2lua.disabled || !set || !set[0]) return false;
    char name[64];
    snprintf(name, sizeof name, "=%s.lua", set);   /* errors read "sfight.lua:3: ..." */
    if (!now) {
        snprintf(g_m2lua.path, sizeof g_m2lua.path, "%s", name + 1);
        g_m2lua.loaded = g_m2lua.suspended = true;
        return true;
    }
    if (!m2lua_run_chunk(name, copy)) return false;
    return m2lua_finish(name + 1);
}

/* Publish what PostDraw drew for the window. */
static inline void m2lua_publish_draw(void) {
    emu_mutex_lock(&g_m2lua.draw_mutex);
    memcpy(g_m2lua.shown, g_m2lua.build, (size_t)g_m2lua.nbuild * sizeof g_m2lua.build[0]);
    g_m2lua.nshown = g_m2lua.nbuild;
    g_m2lua.shown_gen++;
    emu_mutex_unlock(&g_m2lua.draw_mutex);
}

/* The frame edge (emu thread, emu mutex held): Frame(), the runfuncs of the
 * options that are on, then PostDraw() into a new draw list. */
static inline void m2lua_frame(void) {
    if (!g_m2lua.loaded) return;
    g_m2lua.frames++;
    m2lua_call_global("Frame");
    for (int i = 0; i < g_m2lua.noptions; i++) {
        const m2lua_option_t *o = &g_m2lua.options[i];
        if (o->runfunc && o->value != 0) m2lua_option_call(o, "runfunc");
    }
    g_m2lua.nbuild = 0;
    m2lua_call_global("PostDraw");
    m2lua_publish_draw();
}

/* A board reset that is not the first boot (emu mutex held). */
static inline void m2lua_reset(void) {
    if (g_m2lua.loaded && !g_m2lua.suspended) m2lua_call_global("Reset");
}

/* A session (netplay, GGPO, a replay, a follow) has the board, and both ends
 * must run the same ROM: the patches come off and the script stops, with
 * nothing more drawn. M2emulator had no sessions; this is m2-hle2's. */
static inline void m2lua_suspend(void) {
    if (g_m2lua.suspended || !g_m2lua.loaded) return;
    g_m2lua.suspended = true;
    m2lua_unpatch();
    g_m2lua.nbuild = 0;
    m2lua_publish_draw();
    LOG_INFO("script: off while a session has the board");
}

/* The emu thread's hooks (emu_thread.h g_emu_script_*). may_run: no session
 * has the board. A session that lets go of it gets the script back from its
 * start, Init and patches and all. */
static inline bool m2lua_reload(void);

/* Back from a session: the script from its start, its options as they were. */
static inline void m2lua_resume(void) {
    int n = g_m2lua.noptions;
    m2lua_option_t *keep = n ? malloc((size_t)n * sizeof *keep) : NULL;
    if (keep) memcpy(keep, g_m2lua.options, (size_t)n * sizeof *keep);
    else n = 0;
    m2lua_reload();
    for (int k = 0; k < n; k++) {
        int i = m2lua_option_index(keep[k].key);
        if (i >= 0 && keep[k].value) m2lua_set_option(i, keep[k].value);
    }
    free(keep);
}

static inline void m2lua_frame_hook(bool may_run) {
    if (!may_run) { m2lua_suspend(); return; }
    if (g_m2lua.suspended) m2lua_resume();
    m2lua_frame();
}

/* before: the board is about to be reset; else it has been. */
static inline void m2lua_reset_hook(bool may_run, bool before) {
    if (!may_run) { if (before) m2lua_suspend(); return; }
    if (!before) m2lua_reset();
}

/* Reload the game's script from disk, as M2emulator's "reload LUA script". */
static inline bool m2lua_reload(void) {
    char set[sizeof g_m2lua.set];
    snprintf(set, sizeof set, "%s", g_m2lua.set);
    if (!g_m2lua.source) return m2lua_load(set, g_m2lua.bus, g_m2lua.rom);
    char *src = g_m2lua.source;
    g_m2lua.source = NULL;               /* m2lua_load_source takes a copy */
    bool ok = m2lua_load_source(set, src, g_m2lua.bus, g_m2lua.rom, true);
    free(src);
    return ok;
}

/* ---- RetroArch's cheats (the libretro core, Pinboard #967) ----------------
 *
 * RetroArch has no room for a core's own menu, so there a script's options are
 * cheats: one per value other than the option's default, code "<key>=<value>"
 * (the value's index or its name; several joined by '+'). The core writes them
 * as <set>.cht beside the script, the player loads that file in Quick Menu >
 * Cheats, and RetroArch hands the codes of the ones switched on to
 * retro_cheat_set. A cheat switched off puts its option back to the default. */

/* s with its double quotes made single, for a .cht string. */
static inline const char *m2lua_cht_str(const char *s, char *buf, size_t cap) {
    snprintf(buf, cap, "%s", s);
    for (char *p = buf; *p; p++) if (*p == '"') *p = '\'';
    return buf;
}

static inline void m2lua_cht_entry(char *out, int cap, int *at, int n, const m2lua_option_t *o, int v) {
    char name[64], value[32];
    if (*at < cap)
        *at += snprintf(out + *at, (size_t)(cap - *at),
                        "\ncheat%d_desc = \"%s: %s\"\ncheat%d_code = \"%s=%d\"\n"
                        "cheat%d_enable = \"false\"\ncheat%d_handler = \"0\"\n",
                        n, m2lua_cht_str(o->name, name, sizeof name), m2lua_cht_str(o->values[v], value, sizeof value),
                        n, o->key, v, n, n);
}

/* The loaded script's options as a RetroArch .cht file; its length, 0 for a
 * script with no options. */
static inline int m2lua_cht_text(char *out, int cap) {
    int n = 0;
    for (int i = 0; i < g_m2lua.noptions; i++)
        for (int v = 0; v < g_m2lua.options[i].nvalues; v++) n += v != g_m2lua.options[i].dflt;
    out[0] = 0;
    if (n <= 0 || cap <= 0) return 0;
    int at = snprintf(out, (size_t)cap,
                      "# The options of %s.lua as RetroArch cheats: m2-hle writes this file when it\n"
                      "# loads the script (edits are lost). Code \"<option>=<value>\"; a cheat\n"
                      "# switched off puts its option back to the script's default.\ncheats = \"%d\"\n",
                      g_m2lua.set, n);
    n = 0;
    for (int i = 0; i < g_m2lua.noptions; i++) {
        const m2lua_option_t *o = &g_m2lua.options[i];
        for (int v = 0; v < o->nvalues; v++)
            if (v != o->dflt) m2lua_cht_entry(out, cap, &at, n++, o, v);
    }
    return at < cap ? at : cap - 1;
}

/* One "<key>=<value>" of a cheat's code into want[]. */
static inline bool m2lua_cheat_one(const char *s, size_t len, int *want) {
    const char *eq = memchr(s, '=', len);
    if (!eq) return false;
    char key[sizeof g_m2lua.options[0].key], val[sizeof g_m2lua.options[0].values[0]];
    snprintf(key, sizeof key, "%.*s", (int)(eq - s), s);
    snprintf(val, sizeof val, "%.*s", (int)(s + len - eq - 1), eq + 1);
    int i = m2lua_option_index(key);
    if (i < 0) return false;
    const m2lua_option_t *o = &g_m2lua.options[i];
    char *end;
    long v = strtol(val, &end, 10);
    if (end == val || *end)
        for (v = o->nvalues - 1; v >= 0 && strcmp(o->values[v], val) != 0; v--) {}
    if (v < 0 || v >= o->nvalues) return false;
    want[i] = (int)v;
    return true;
}

/* A cheat's code into want[] (one value per option); how many it set. */
static inline int m2lua_cheat_code(const char *code, int *want) {
    int n = 0;
    while (code && *code) {
        size_t len = strcspn(code, "+");
        n += m2lua_cheat_one(code, len, want);
        code += len;
        if (*code) code++;
    }
    return n;
}

/* want[] back to every option's default. */
static inline void m2lua_cheat_defaults(int *want) {
    for (int i = 0; i < g_m2lua.noptions; i++) want[i] = g_m2lua.options[i].dflt;
}

/* Every option whose value differs from want[] set to it (and its changefunc
 * called). Emu mutex held, the script running. */
static inline void m2lua_cheat_apply(const int *want) {
    for (int i = 0; i < g_m2lua.noptions; i++)
        if (want[i] != g_m2lua.options[i].value) m2lua_set_option(i, want[i]);
}

/* ---- The script's state as JSON (the bridge's script_status, the web page) - */

static inline void m2lua_json_put(char *out, int cap, int *at, const char *fmt, const char *s) {
    char esc[1024];
    if (s) json_escape(esc, (int)sizeof esc, s);
    if (*at < cap) *at += snprintf(out + *at, (size_t)(cap - *at), fmt, s ? esc : "");
}

static inline void m2lua_json_option(char *out, int cap, int *at, const m2lua_option_t *o) {
    m2lua_json_put(out, cap, at, "{\"key\":\"%s\"", o->key);
    m2lua_json_put(out, cap, at, ",\"name\":\"%s\"", o->name);
    if (*at < cap) *at += snprintf(out + *at, (size_t)(cap - *at), ",\"value\":%d,\"values\":[", o->value);
    for (int v = 0; v < o->nvalues; v++) m2lua_json_put(out, cap, at, v ? ",\"%s\"" : "\"%s\"", o->values[v]);
    m2lua_json_put(out, cap, at, "]}", NULL);
}

/* {"ok":true,"lua":..,"loaded":..,"running":..,"set":..,"path":..,"error":..,
 * "options":[{"key","name","value","values":[..]}]}. running: no session has
 * the board. Emu mutex held. */
static inline void m2lua_status_json(char *out, int cap, bool running) {
    int at = snprintf(out, (size_t)cap, "{\"ok\":true,\"lua\":\"%s\",\"loaded\":%s,\"running\":%s,"
                      "\"frames\":%llu,\"draws\":%d", LUA_RELEASE, g_m2lua.loaded ? "true" : "false",
                      running && !g_m2lua.suspended ? "true" : "false",
                      (unsigned long long)g_m2lua.frames, g_m2lua.nshown);
    m2lua_json_put(out, cap, &at, ",\"set\":\"%s\"", g_m2lua.set);
    m2lua_json_put(out, cap, &at, ",\"path\":\"%s\"", g_m2lua.path);
    m2lua_json_put(out, cap, &at, ",\"error\":\"%s\"", g_m2lua.error);
    m2lua_json_put(out, cap, &at, ",\"options\":[", NULL);
    for (int i = 0; i < g_m2lua.noptions; i++) {
        if (i) m2lua_json_put(out, cap, &at, ",", NULL);
        m2lua_json_option(out, cap, &at, &g_m2lua.options[i]);
    }
    m2lua_json_put(out, cap, &at, "]}", NULL);
}

/* Copy the newest draw list out for the window. Returns its generation. */
static inline uint32_t m2lua_take_draw(m2lua_draw_t *out, int *n) {
    m2lua_init_once();
    emu_mutex_lock(&g_m2lua.draw_mutex);
    memcpy(out, g_m2lua.shown, (size_t)g_m2lua.nshown * sizeof out[0]);
    *n = g_m2lua.nshown;
    uint32_t gen = g_m2lua.shown_gen;
    emu_mutex_unlock(&g_m2lua.draw_mutex);
    return gen;
}

static inline void m2lua_set_key(int key, bool down) {
    if (key >= 0 && key < M2LUA_KEY_MAX) g_m2lua.keys[key] = down ? 1 : 0;
}

/* Run a chunk in the script's state and hand back what it returns, as text
 * (the bridge's script_eval). Emu mutex held. A game with no script gets a
 * state of its own, so the helpers can be tried on any set. */
static inline bool m2lua_eval(const char *code, char *out, size_t cap) {
    m2lua_init_once();
    if (!g_m2lua.L) g_m2lua.L = m2lua_new_state();
    lua_State *L = g_m2lua.L;
    int top = lua_gettop(L);
    int rc = luaL_loadstring(L, code);
    if (rc == LUA_OK) rc = lua_pcall(L, 0, LUA_MULTRET, 0);
    size_t used = 0;
    out[0] = 0;
    for (int i = top + 1; i <= lua_gettop(L) && used + 1 < cap; i++) {
        const char *s = luaL_tolstring(L, i, NULL);
        used += (size_t)snprintf(out + used, cap - used, "%s%s", i > top + 1 ? "\t" : "", s);
        lua_pop(L, 1);
        if (used >= cap) used = cap - 1;
    }
    lua_settop(L, top);
    return rc == LUA_OK;
}

#endif /* M2LUA_H */
