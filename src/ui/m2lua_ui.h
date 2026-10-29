/*
 * m2lua_ui.h -- the desktop window's side of the game scripts (core/m2lua.h,
 * Pinboard #958): paints what PostDraw drew over the game, gives the script's
 * Options a "Script" menu (M2emulator's Game menu entries), and turns the
 * keyboard into M2emulator's key numbers for Input_IsKeyPressed.
 *
 * Input_IsKeyPressed takes DirectInput's DIK_* codes, which are the PC's set-1
 * scan codes; k_m2lua_dik maps the ones a keyboard has onto sokol's key codes.
 * JOYn + button numbers have no gamepad behind them in this window and read 0.
 */
#ifndef M2LUA_UI_H
#define M2LUA_UI_H

#include "../core/m2lua.h"
#include "retro_shader.h"   /* stb_image, static to it */

/* DIK code -> sokol key code. */
static const struct { uint8_t dik; int16_t key; } k_m2lua_dik[] = {
    {0x01, SAPP_KEYCODE_ESCAPE}, {0x02, SAPP_KEYCODE_1}, {0x03, SAPP_KEYCODE_2},
    {0x04, SAPP_KEYCODE_3}, {0x05, SAPP_KEYCODE_4}, {0x06, SAPP_KEYCODE_5},
    {0x07, SAPP_KEYCODE_6}, {0x08, SAPP_KEYCODE_7}, {0x09, SAPP_KEYCODE_8},
    {0x0A, SAPP_KEYCODE_9}, {0x0B, SAPP_KEYCODE_0}, {0x0C, SAPP_KEYCODE_MINUS},
    {0x0D, SAPP_KEYCODE_EQUAL}, {0x0E, SAPP_KEYCODE_BACKSPACE}, {0x0F, SAPP_KEYCODE_TAB},
    {0x10, SAPP_KEYCODE_Q}, {0x11, SAPP_KEYCODE_W}, {0x12, SAPP_KEYCODE_E},
    {0x13, SAPP_KEYCODE_R}, {0x14, SAPP_KEYCODE_T}, {0x15, SAPP_KEYCODE_Y},
    {0x16, SAPP_KEYCODE_U}, {0x17, SAPP_KEYCODE_I}, {0x18, SAPP_KEYCODE_O},
    {0x19, SAPP_KEYCODE_P}, {0x1A, SAPP_KEYCODE_LEFT_BRACKET}, {0x1B, SAPP_KEYCODE_RIGHT_BRACKET},
    {0x1C, SAPP_KEYCODE_ENTER}, {0x1D, SAPP_KEYCODE_LEFT_CONTROL}, {0x1E, SAPP_KEYCODE_A},
    {0x1F, SAPP_KEYCODE_S}, {0x20, SAPP_KEYCODE_D}, {0x21, SAPP_KEYCODE_F},
    {0x22, SAPP_KEYCODE_G}, {0x23, SAPP_KEYCODE_H}, {0x24, SAPP_KEYCODE_J},
    {0x25, SAPP_KEYCODE_K}, {0x26, SAPP_KEYCODE_L}, {0x27, SAPP_KEYCODE_SEMICOLON},
    {0x28, SAPP_KEYCODE_APOSTROPHE}, {0x29, SAPP_KEYCODE_GRAVE_ACCENT}, {0x2A, SAPP_KEYCODE_LEFT_SHIFT},
    {0x2B, SAPP_KEYCODE_BACKSLASH}, {0x2C, SAPP_KEYCODE_Z}, {0x2D, SAPP_KEYCODE_X},
    {0x2E, SAPP_KEYCODE_C}, {0x2F, SAPP_KEYCODE_V}, {0x30, SAPP_KEYCODE_B},
    {0x31, SAPP_KEYCODE_N}, {0x32, SAPP_KEYCODE_M}, {0x33, SAPP_KEYCODE_COMMA},
    {0x34, SAPP_KEYCODE_PERIOD}, {0x35, SAPP_KEYCODE_SLASH}, {0x36, SAPP_KEYCODE_RIGHT_SHIFT},
    {0x37, SAPP_KEYCODE_KP_MULTIPLY}, {0x38, SAPP_KEYCODE_LEFT_ALT}, {0x39, SAPP_KEYCODE_SPACE},
    {0x3A, SAPP_KEYCODE_CAPS_LOCK}, {0x3B, SAPP_KEYCODE_F1}, {0x3C, SAPP_KEYCODE_F2},
    {0x3D, SAPP_KEYCODE_F3}, {0x3E, SAPP_KEYCODE_F4}, {0x3F, SAPP_KEYCODE_F5},
    {0x40, SAPP_KEYCODE_F6}, {0x41, SAPP_KEYCODE_F7}, {0x42, SAPP_KEYCODE_F8},
    {0x43, SAPP_KEYCODE_F9}, {0x44, SAPP_KEYCODE_F10}, {0x45, SAPP_KEYCODE_NUM_LOCK},
    {0x46, SAPP_KEYCODE_SCROLL_LOCK}, {0x47, SAPP_KEYCODE_KP_7}, {0x48, SAPP_KEYCODE_KP_8},
    {0x49, SAPP_KEYCODE_KP_9}, {0x4A, SAPP_KEYCODE_KP_SUBTRACT}, {0x4B, SAPP_KEYCODE_KP_4},
    {0x4C, SAPP_KEYCODE_KP_5}, {0x4D, SAPP_KEYCODE_KP_6}, {0x4E, SAPP_KEYCODE_KP_ADD},
    {0x4F, SAPP_KEYCODE_KP_1}, {0x50, SAPP_KEYCODE_KP_2}, {0x51, SAPP_KEYCODE_KP_3},
    {0x52, SAPP_KEYCODE_KP_0}, {0x53, SAPP_KEYCODE_KP_DECIMAL}, {0x57, SAPP_KEYCODE_F11},
    {0x58, SAPP_KEYCODE_F12}, {0x9C, SAPP_KEYCODE_KP_ENTER}, {0x9D, SAPP_KEYCODE_RIGHT_CONTROL},
    {0xB5, SAPP_KEYCODE_KP_DIVIDE}, {0xB8, SAPP_KEYCODE_RIGHT_ALT}, {0xC7, SAPP_KEYCODE_HOME},
    {0xC8, SAPP_KEYCODE_UP}, {0xC9, SAPP_KEYCODE_PAGE_UP}, {0xCB, SAPP_KEYCODE_LEFT},
    {0xCD, SAPP_KEYCODE_RIGHT}, {0xCF, SAPP_KEYCODE_END}, {0xD0, SAPP_KEYCODE_DOWN},
    {0xD1, SAPP_KEYCODE_PAGE_DOWN}, {0xD2, SAPP_KEYCODE_INSERT}, {0xD3, SAPP_KEYCODE_DELETE},
};

/* Video_CreateSurfaceFromFile's decoder: ui/retro_shader.h's stb_image. */
static uint8_t *m2lua_ui_decode(const uint8_t *buf, int len, int *w, int *h) {
    int comp;
    return stbi_load_from_memory(buf, len, w, h, &comp, 4);
}

static inline void m2lua_ui_key(int sapp_key, bool down) {
    for (size_t i = 0; i < sizeof k_m2lua_dik / sizeof k_m2lua_dik[0]; i++)
        if (k_m2lua_dik[i].key == sapp_key) m2lua_set_key(k_m2lua_dik[i].dik, down);
}

/* The window's key events, for Input_IsKeyPressed. */
static inline void m2lua_ui_event(const sapp_event *ev) {
    if (ev->type == SAPP_EVENTTYPE_KEY_DOWN || ev->type == SAPP_EVENTTYPE_KEY_UP)
        m2lua_ui_key((int)ev->key_code, ev->type == SAPP_EVENTTYPE_KEY_DOWN);
}

/* ---- Surfaces: a texture per loaded PNG ---------------------------------- */

static struct {
    sg_image img[M2LUA_SURFACE_MAX];
    sg_view  view[M2LUA_SURFACE_MAX];
    uint32_t gen[M2LUA_SURFACE_MAX];   /* the surface's gen it was made from */
} s_m2lua_tex;

static inline void m2lua_ui_drop_tex(int i) {
    if (s_m2lua_tex.view[i].id) sg_destroy_view(s_m2lua_tex.view[i]);
    if (s_m2lua_tex.img[i].id)  sg_destroy_image(s_m2lua_tex.img[i]);
    s_m2lua_tex.view[i].id = s_m2lua_tex.img[i].id = 0;
    s_m2lua_tex.gen[i] = 0;
}

static inline void m2lua_ui_make_tex(int i, const m2lua_surface_t *s) {
    s_m2lua_tex.img[i] = sg_make_image(&(sg_image_desc){
        .width = s->w, .height = s->h, .pixel_format = SG_PIXELFORMAT_RGBA8,
        .data.mip_levels[0] = { s->rgba, (size_t)s->w * (size_t)s->h * 4u },
        .label = "script-surface" });
    s_m2lua_tex.view[i] = sg_make_view(&(sg_view_desc){
        .texture.image = s_m2lua_tex.img[i], .label = "script-surface-tex" });
    s_m2lua_tex.gen[i] = s->gen;
}

/* Upload what the script loaded since, let go of what it released. */
static inline void m2lua_ui_sync_surfaces(void) {
    m2lua_init_once();
    emu_mutex_lock(&g_m2lua.draw_mutex);
    for (int i = 0; i < M2LUA_SURFACE_MAX; i++) {
        m2lua_surface_t *s = &g_m2lua.surfaces[i];
        if (!s->used) {
            if (s->rgba) { free(s->rgba); s->rgba = NULL; }
            if (s_m2lua_tex.img[i].id) m2lua_ui_drop_tex(i);
        } else if (s_m2lua_tex.gen[i] != s->gen) {
            m2lua_ui_drop_tex(i);
            m2lua_ui_make_tex(i, s);
        }
    }
    emu_mutex_unlock(&g_m2lua.draw_mutex);
}

/* ---- Painting the draw list ---------------------------------------------- */

static m2lua_draw_t s_m2lua_list[M2LUA_DRAW_MAX];

static inline ImU32 m2lua_ui_color(uint32_t argb) {
    uint32_t a = argb >> 24, r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
    return (a << 24) | (b << 16) | (g << 8) | r;
}

static inline void m2lua_ui_text(ImDrawList *dl, const m2lua_draw_t *d, ImVec2 p, float scale) {
    float size = M2LUA_TEXT_SIZE;
    bool  bold = false;
    if (d->font >= 0 && d->font < M2LUA_FONT_MAX) {
        size = g_m2lua.fonts[d->font].size;
        bold = g_m2lua.fonts[d->font].bold;
    }
    ImU32 col = m2lua_ui_color(d->color);
    ImDrawList_AddTextImFontPtr(dl, igGetFont(), size * scale, p, col, d->text);
    if (bold) ImDrawList_AddTextImFontPtr(dl, igGetFont(), size * scale, (ImVec2){ p.x + 1, p.y }, col, d->text);
}

static inline void m2lua_ui_surface(ImDrawList *dl, const m2lua_draw_t *d, ImVec2 p, float scale) {
    int i = d->surface;
    if (i < 0 || i >= M2LUA_SURFACE_MAX || !s_m2lua_tex.view[i].id) return;
    const m2lua_surface_t *s = &g_m2lua.surfaces[i];
    ImVec2 q = { p.x + (float)s->w * scale, p.y + (float)s->h * scale };
    ImDrawList_AddImage(dl, (ImTextureRef){ NULL, (ImTextureID)simgui_imtextureid(s_m2lua_tex.view[i]) }, p, q);
}

/* Paint PostDraw's list over the game at (ox, oy, w, h), window pixels: the
 * script's coordinates are the board's 496 x 384 picture. Under the ImGui
 * windows, over the game. */
static inline void m2lua_ui_draw(int ox, int oy, int w, int h) {
    m2lua_ui_sync_surfaces();
    int n;
    m2lua_take_draw(s_m2lua_list, &n);
    if (n == 0 || w <= 0 || h <= 0) return;
    float dpi = sapp_dpi_scale() > 0 ? sapp_dpi_scale() : 1.0f;
    float sx = (float)w / (float)VIDEO_WIDTH, sy = (float)h / (float)VIDEO_HEIGHT;
    ImDrawList *dl = igGetBackgroundDrawList();
    for (int i = 0; i < n; i++) {
        const m2lua_draw_t *d = &s_m2lua_list[i];
        ImVec2 p = { ((float)ox + d->x * sx) / dpi, ((float)oy + d->y * sy) / dpi };
        if (d->kind == M2LUA_DRAW_TEXT) m2lua_ui_text(dl, d, p, sy / dpi);
        else                            m2lua_ui_surface(dl, d, p, sy / dpi);
    }
}

/* ---- The Script menu ----------------------------------------------------- */

static inline void m2lua_ui_option(emu_mutex_t *mu, int i) {
    const m2lua_option_t *o = &g_m2lua.options[i];
    if (!igBeginMenu(o->name)) return;
    for (int v = 0; v < o->nvalues; v++) {
        if (!igMenuItemEx(o->values[v], NULL, o->value == v, true)) continue;
        emu_mutex_lock(mu);
        m2lua_set_option(i, v);
        emu_mutex_unlock(mu);
    }
    igEndMenu();
}

/* The loaded script's options, then a reload. mu: the emu mutex. */
static inline void m2lua_ui_menu(emu_mutex_t *mu) {
    if (!g_m2lua.set[0] || !igBeginMenu("Script")) return;
    if (!g_m2lua.loaded) igTextDisabled("no %s/%s.lua", g_m2lua.dir, g_m2lua.set);
    for (int i = 0; i < g_m2lua.noptions; i++) m2lua_ui_option(mu, i);
    if (g_m2lua.noptions) igSeparator();
    if (igMenuItemEx("Reload script", NULL, false, true)) {
        emu_mutex_lock(mu);
        m2lua_reload();
        emu_mutex_unlock(mu);
    }
    if (g_m2lua.error[0]) igTextDisabled("%s", g_m2lua.error);
    igEndMenu();
}

/* The window is going: the textures go with it. */
static inline void m2lua_ui_shutdown(void) {
    for (int i = 0; i < M2LUA_SURFACE_MAX; i++) m2lua_ui_drop_tex(i);
}

#endif /* M2LUA_UI_H */
