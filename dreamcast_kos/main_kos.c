/*
 * main_kos.c -- B of the Dreamcast A/B (Pinboard #520): the board, Gems C,
 * AOT, the pager and the ADX sound exactly as dreamcast/ (A) has them, but the
 * picture drawn the way KallistiOS's examples draw (examples/dreamcast/gldc):
 * GLdc, kos-ports' OpenGL 1.x, instead of the PVR's lists by hand.
 *
 *   - geo3d decodes into its own triangle buffer (no GEO3D_DC_SINK), in eye
 *     space; GL projects, clips at the near plane and twiddles.
 *   - One draw order, back to front by the board's sort key, everything in
 *     the translucent list with autosort off (GLdc's "presorted" mode).
 *   - A face's texture is its tile at 8 bits a texel against a shared grey
 *     palette (opaque, or texel 15 a hole), modulated by one colour: texel
 *     15's shade. GL 1.x has no offset colour, so the knee and pool banks
 *     A uses are not reachable from here (DC-AB.md).
 *   - The tile layers come from the board's own CPU compositor
 *     (tile_renderer.h) as two 512x512 ARGB1555 textures.
 *   - assert and exception handlers, as KOS's assert/stacktrace example has.
 *
 * Measured against A in DC-AB.md.
 */
#include "net/netplay.h"

#include <kos.h>
#include <dc/maple/controller.h>
#include <dc/biosfont.h>
#include <arch/stack.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glkos.h>

#ifndef DC_FPS_CAP
#define DC_FPS_CAP 60
#endif
#ifndef DC_HASH_FRAME
#define DC_HASH_FRAME 0
#endif
#ifndef DC_BENCH_F0
#define DC_BENCH_F0 3500u
#define DC_BENCH_F1 3900u
#endif

#include "constants.h"
#include "log.h"
#include "memory.h"
#include "i960.h"
#include "i960_exec.h"
#include "rom_loader.h"
#include "emu_thread.h"
#include "input.h"
#include "registry.h"
#include "tile_renderer.h"
#include "gems.h"
#include "geo3d.h"

#include "dc_pager.h"
#include "dc_sound.h"

KOS_INIT_FLAGS(INIT_IRQ | INIT_CONTROLLER | INIT_CDROM);

#define SCR_W 640
#define SCR_H 480
#define SX ((float)SCR_W / VIDEO_WIDTH)    /* the view stretched over the frame, as A's FILL=1 */
#define SY ((float)SCR_H / VIDEO_HEIGHT)

static memory_bus_t     bus;
static i960_cpu_t       cpu;
static emu_thread_ctx_t ctx;
static romset_t         rs;
static tile_cpu_t       tiles;
static geo3d_state_t    geo;

/* ---- Robustness: what KOS's assert and stacktrace examples set up ---------- */

#ifndef NDEBUG   /* the bench builds with NDEBUG, as A does: no asserts, no handler */
static void kb_assert(const char *file, int line, const char *expr, const char *msg, const char *func) {
    printf("assert %s:%d %s: %s %s\n", file, line, func ? func : "", expr, msg ? msg : "");
    arch_stk_trace(1);
    arch_abort();
}
#endif

/* One staging buffer for every upload (half a tile layer, half the text, a
 * cut texture): GL copies it into VRAM before it returns, and RAM is what B
 * is shortest of. The layers and the text are GL_ARGB1555_KOS, allocated once
 * and refilled with glTexSubImage2D: GLdc maps GL_RGB5_A1 to twiddled ARGB4444,
 * which halves the colour and converts every upload through a temporary
 * buffer the size of the whole texture (512 KB, on a heap with ~570 KB left). */
static uint16_t g_stage[512 * 256] __attribute__((aligned(32)));

/* ---- Text: the BIOS font into one texture (bfont_draw_str) ------------------ */

#define TX_ROWS 10
#define TX_W    1024
#define TX_H    256
static char     g_text_rows[TX_ROWS][88];
static bool     g_text_dirty;
static GLuint   g_text_tex;

static void kt_row(int row, const char *s) {
    if (row < 0 || row >= TX_ROWS || !strncmp(g_text_rows[row], s, 87)) return;
    snprintf(g_text_rows[row], sizeof g_text_rows[row], "%s", s);
    g_text_dirty = true;
}

/* ---- Textures --------------------------------------------------------------- */

#define KT_SLOTS 1024u
typedef struct { uint32_t key; GLuint id; uint16_t q0, q1; uint8_t sheet; float su, sv; } kt_tex_t;
static kt_tex_t g_tex[KT_SLOTS];
static unsigned g_tex_n;
static uint32_t g_gen_tex = ~0u;
#define g_cut ((uint8_t *)g_stage)   /* 8bpp: the largest tile B cuts is 512x512; a bigger one draws untextured */
static struct { uint64_t us_tex, us_tiles, us_decode, us_sort, us_submit; unsigned made, dropped, fails, tris, tmax; } g_k;

static uint32_t kt_hash(uint32_t k) { k ^= k >> 16; k *= 0x7FEB352Du; k ^= k >> 15; return k; }
static unsigned kt_log2(unsigned v) { unsigned l = 0; while ((1u << l) < v) l++; return l; }

static void kt_drop_all(void) {
    for (unsigned i = 0; i < KT_SLOTS; i++)
        if (g_tex[i].key) { glDeleteTextures(1, &g_tex[i].id); g_tex[i].key = 0; g_k.dropped++; }
    g_tex_n = 0;
}

/* Texture RAM changed: drop every texture cut from a row the game wrote. A
 * dropped slot breaks a probe chain, so the rest are put back. */
static void kt_invalidate(memory_bus_t *b) {
    if (b->gen_tex == g_gen_tex) return;
    g_gen_tex = b->gen_tex;
    static uint8_t dirty[2][1024];
    for (int s = 0; s < 2; s++)
        for (int q = 0; q < 1024; q++) { dirty[s][q] = b->tex_dirty[s][q]; b->tex_dirty[s][q] = 0; }
    bool gone = false;
    for (unsigned i = 0; i < KT_SLOTS; i++) {
        kt_tex_t *t = &g_tex[i];
        if (!t->key) continue;
        for (unsigned q = t->q0; q <= t->q1; q++)
            if (dirty[t->sheet][q]) { glDeleteTextures(1, &t->id); t->key = 0; g_tex_n--; g_k.dropped++; gone = true; break; }
    }
    if (!gone) return;
    static kt_tex_t old[KT_SLOTS];
    memcpy(old, g_tex, sizeof old);
    memset(g_tex, 0, sizeof g_tex);
    for (unsigned i = 0; i < KT_SLOTS; i++) {
        if (!old[i].key) continue;
        unsigned h = kt_hash(old[i].key), p = 0;
        while (g_tex[(h + p) & (KT_SLOTS - 1u)].key) p++;
        g_tex[(h + p) & (KT_SLOTS - 1u)] = old[i];
    }
}

/* The GL texture of a face's tile: its texels a byte each against shared
 * palette 0 (opaque) or 1 (texel 15 clear). */
static kt_tex_t *kt_get(float ftx, float fty, float ftw, float fth, unsigned fl) {
    unsigned sheet = (fl & GEO3D_FACE_SHEET1) ? 1 : 0, trans = fl & GEO3D_FACE_TRANSPARENT ? 1 : 0;
    unsigned x0 = (unsigned)(int)ftx & 2047u, y0 = (unsigned)(int)fty & 1023u, tw = (unsigned)ftw, th = (unsigned)fth;
    if (!tw || !th || tw > 512 || th > 512) { g_k.fails++; return NULL; }
    unsigned lw = kt_log2(tw), lh = kt_log2(th);
    uint32_t key = 0x80000000u | sheet << 29 | trans << 28 | x0 << 17 | y0 << 7 | lw << 3 | lh;
    unsigned h = kt_hash(key);
    kt_tex_t *t = NULL;
    for (unsigned p = 0; p < KT_SLOTS; p++) {
        kt_tex_t *e = &g_tex[(h + p) & (KT_SLOTS - 1u)];
        if (e->key == key) return e;
        if (!e->key) { t = e; break; }
    }
    if (!t || g_tex_n >= KT_SLOTS * 3 / 4) { g_k.fails++; return NULL; }
    uint64_t t0 = timer_us_gettime64();
    unsigned W = 1u << lw, H = 1u << lh;
    W = W < 8 ? 8 : W; H = H < 8 ? 8 : H;
    const uint32_t *src = (const uint32_t *)(sheet ? bus.texram1 : bus.texram0);
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = 0; x < W; x++)
            g_cut[y * W + x] = (uint8_t)dct_texel(src, (x0 + (x & (tw - 1))) & 2047u, (y0 + (y & (th - 1))) & 1023u);
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_COLOR_INDEX8_EXT, (GLsizei)W, (GLsizei)H, 0, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, g_cut);
    if (glGetError() != GL_NO_ERROR) { glDeleteTextures(1, &id); g_k.fails++; return NULL; }
    glTexParameteri(GL_TEXTURE_2D, GL_SHARED_TEXTURE_BANK_KOS, (GLint)trans);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, fl & GEO3D_FACE_MIRROR_X ? GL_MIRRORED_REPEAT : GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, fl & GEO3D_FACE_MIRROR_Y ? GL_MIRRORED_REPEAT : GL_REPEAT);
    *t = (kt_tex_t){ key, id, (uint16_t)((y0 >> 1) + (x0 >= 1024 ? 512 : 0)),
                     (uint16_t)(((y0 + th - 1) >> 1) + (x0 >= 1024 ? 512 : 0)), (uint8_t)sheet,
                     1.0f / (float)W, 1.0f / (float)H };
    g_tex_n++; g_k.made++;
    g_k.us_tex += timer_us_gettime64() - t0;
    return t;
}

/* ---- Colour: the fill shader's chain at one texel ---------------------------- */

static uint8_t g_cx[256];
static void k_shade(const int c5[3], int li, int out[3]) {
    for (int ch = 0; ch < 3; ch++) out[ch] = g_cx[bus.colorxlat[ch * 0x4000 + ((c5[ch] << 8) + li) * 2]];
}
static int k_clamp(float v) { return v <= 0.0f ? 0 : v >= 1.0f ? 255 : (int)(v * 255.0f + 0.5f); }

typedef struct { float r, g, b, lb, pl, fl; } k_col_in_t;
static uint32_t k_colour(const k_col_in_t *T, bool tex) {
    int c5[3] = { (int)(T->r * 31.0f + 0.5f), (int)(T->g * 31.0f + 0.5f), (int)(T->b * 31.0f + 0.5f) };
    for (int k = 0; k < 3; k++) c5[k] = c5[k] < 0 ? 0 : c5[k] > 31 ? 31 : c5[k];
    float pl = T->pl < 0.0f ? 0.0f : T->pl > 1.0f ? 1.0f : T->pl;
    int poly = (int)(pl * 255.0f + 0.5f), c[3];
    if (!tex) {
        if (T->lb < 0.0f) { c[0] = k_clamp(T->r * pl); c[1] = k_clamp(T->g * pl); c[2] = k_clamp(T->b * pl); }
        else k_shade(c5, poly >> 2 > 63 ? 63 : poly >> 2, c);
    } else if (T->lb < 0.0f) {
        c[0] = k_clamp(T->r * 2.0f); c[1] = k_clamp(T->g * 2.0f); c[2] = k_clamp(T->b * 2.0f);
    } else {
        uint32_t lbyte = 2u * ((uint32_t)T->lb + 120u);
        int lram = lbyte < LUMA_SIZE ? bus.luma[lbyte] : 0, li = (lram * poly) >> 8;
        k_shade(c5, li > 63 ? 63 : li, c);
    }
    unsigned a = ((unsigned)(T->fl + 0.5f) & GEO3D_FACE_CHECKER) ? 128u : 255u;
    return (uint32_t)c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16 | a << 24;   /* RGBA bytes */
}

/* ---- The 3D scene --------------------------------------------------------------- */

typedef struct { float ax, bx, ay, by; uint8_t slice; } k_run_t;
#define K_RUNS 256
static k_run_t  g_run[K_RUNS];
static int      g_runs;

/* The frame's faces, as geo3d hands them over (GEO3D_DC_SINK, as A): a mesh's
 * corners go into the pool once, already in GL's clip terms for their run
 * (x' = kx x + cx z, y' = ky y + cy z, z), and a face is three of them with
 * its texture, coordinates and colour worked out. */
/* GLdc's TR list, in vertices (64 bytes each, a draw's header takes one).
 * It is reserved once: past it GLdc grows the list by memalign + copy, and a
 * failed memalign is a NULL write (its assert is compiled out), so a frame
 * that would overrun it loses its farthest faces instead (k_frame). */
#ifndef K_TR_CAP
#define K_TR_CAP 10240
#endif
#define K_TR_RESERVE 384   /* the two layers, the text, and what the near clip adds */
typedef struct { float x, y, z; } k_pv_t;
#define K_MAX_VERTS 8192
static k_pv_t   g_pv[K_MAX_VERTS];
static int      g_pv_n;
typedef struct { uint16_t v[3]; uint16_t pad; kt_tex_t *tex; float u[3], t[3]; uint32_t c; } k_face_t;
static k_face_t g_face[GEO3D_MAX_TRIS];
static int      g_face_n;
static unsigned g_face_dropped;
static uint32_t g_key[GEO3D_MAX_TRIS], g_order[2][GEO3D_MAX_TRIS];
static float    g_cur_kx, g_cur_cx, g_cur_ky, g_cur_cy;
static int      g_cur_slice;

static int geo3d_dc_verts(const vec3_t *tv, int n) {
    if (g_pv_n + n > K_MAX_VERTS) { g_face_dropped++; return -1; }
    k_pv_t *o = &g_pv[g_pv_n];
    for (int i = 0; i < n; i++, o++) {
        float x = tv[i].x, y = tv[i].y, z = tv[i].z;
        o->x = g_cur_kx * x + g_cur_cx * z;
        o->y = g_cur_ky * y + g_cur_cy * z;
        o->z = z;
    }
    int base = g_pv_n;
    g_pv_n += n;
    return base;
}

/* The last face's texture and colour: a quad's two halves and a model's
 * faces share them. */
static struct { float tx, ty, tw, th, fl; kt_tex_t *tex; k_col_in_t c; bool ctex; uint32_t col; } g_memo;
static void k_memo_reset(void) { memset(&g_memo, 0, sizeof g_memo); g_memo.tw = g_memo.c.r = -1.0f; }

static void k_face_put(int a, int b, int c, const float *u, const float *v, int i, int j, int k,
                       float tx, float ty, float tw, float th, float r, float g, float bb,
                       float lb, float pl, float fl, int32_t key) {
    if (g_face_n >= GEO3D_MAX_TRIS) { g_face_dropped++; return; }
    unsigned f = (unsigned)(fl + 0.5f);
    if (!(tx == g_memo.tx && ty == g_memo.ty && tw == g_memo.tw && th == g_memo.th && fl == g_memo.fl)) {
        g_memo.tex = tw > 0.0f && !(f & GEO3D_FACE_CHECKER) ? kt_get(tx, ty, tw, th, f) : NULL;
        g_memo.tx = tx; g_memo.ty = ty; g_memo.tw = tw; g_memo.th = th; g_memo.fl = fl;
    }
    kt_tex_t *tex = g_memo.tex;
    if (r != g_memo.c.r || g != g_memo.c.g || bb != g_memo.c.b || lb != g_memo.c.lb || pl != g_memo.c.pl ||
            fl != g_memo.c.fl || (tex != NULL) != g_memo.ctex) {
        g_memo.c = (k_col_in_t){ r, g, bb, lb, pl, fl };
        g_memo.ctex = tex != NULL;
        g_memo.col = k_colour(&g_memo.c, tex != NULL);
    }
    const int t = g_face_n++;
    k_face_t *F = &g_face[t];
    F->v[0] = (uint16_t)a; F->v[1] = (uint16_t)b; F->v[2] = (uint16_t)c;
    F->tex = tex;
    F->c = g_memo.col;
    float su = tex ? tex->su : 0.0f, sv = tex ? tex->sv : 0.0f;
    F->u[0] = u[i] * su; F->t[0] = v[i] * sv;
    F->u[1] = u[j] * su; F->t[1] = v[j] * sv;
    F->u[2] = u[k] * su; F->t[2] = v[k] * sv;
    uint32_t q = key >= 0 ? (uint32_t)key
               : geo3d_board_zkey(fminf(-g_pv[a].z, fminf(-g_pv[b].z, -g_pv[c].z)));
    q = q > 0xFFFFu ? 0xFFFFu : q;
    g_key[t] = (uint32_t)(7 - g_cur_slice) << 29 | (0xFFFFu - q) << 13 | (uint32_t)t;
}

static void geo3d_dc_tri(int a, int b, int c, float ua, float va, float ub, float vb, float uc, float vc,
                         float r, float g, float b_, float tx, float ty, float tw, float th,
                         float lb, float pl, float fl) {
    float u[3] = { ua, ub, uc }, v[3] = { va, vb, vc };
    int32_t key = -1;
    if (g_geo3d_emit_flat >= 0.0f) {
        uint32_t k = (uint32_t)(g_geo3d_emit_flat * 65536.0f);
        key = (int32_t)(k > 0xFFFFu ? 0xFFFFu : k);
    }
    k_face_put(a, b, c, u, v, 0, 1, 2, tx, ty, tw, th, r, g, b_, lb, pl, fl, key);
}

static void geo3d_dc_face(int v0, const geo3d_cface_t *F, int cut, float r, float g, float b, float pl, int32_t key) {
    const int a = v0 + F->ai, bb = v0 + F->bi, c = v0 + F->ci, d = v0 + F->di;
    if (cut == 0) {
        k_face_put(a, bb, c, F->uvu, F->uvv, 0, 1, 2, F->tx, F->ty, F->tw, F->th, r, g, b, F->lb, pl, F->fl, key);
    } else if (cut == 1) {
        k_face_put(a, bb, c, F->uvu, F->uvv, 0, 1, 2, F->tx, F->ty, F->tw, F->th, r, g, b, F->lb, pl, F->fl, key);
        k_face_put(bb, d, c, F->uvu, F->uvv, 1, 3, 2, F->tx, F->ty, F->tw, F->th, r, g, b, F->lb, pl, F->fl, key);
    } else {
        k_face_put(a, bb, d, F->uvu, F->uvv, 0, 1, 3, F->tx, F->ty, F->tw, F->th, r, g, b, F->lb, pl, F->fl, key);
        k_face_put(a, d, c, F->uvu, F->uvv, 0, 3, 2, F->tx, F->ty, F->tw, F->th, r, g, b, F->lb, pl, F->fl, key);
    }
}

static void geo3d_dc_tri_xyz(float x0, float y0, float z0, float u0, float v0,
                             float x1, float y1, float z1, float u1, float v1,
                             float x2, float y2, float z2, float u2, float v2,
                             float r, float g, float b, float tx, float ty, float tw, float th,
                             float lb, float pl, float fl) {
    const vec3_t c[3] = { { x0, y0, z0 }, { x1, y1, z1 }, { x2, y2, z2 } };
    int v = geo3d_dc_verts(c, 3);
    if (v >= 0) geo3d_dc_tri(v, v + 1, v + 2, u0, v0, u1, v1, u2, v2, r, g, b, tx, ty, tw, th, lb, pl, fl);
}

/* GLdc copies what glDrawArrays is given, so the vertices go through a small
 * staging array a batch at a time. */
typedef struct { float x, y, z, u, v; uint32_t c; } k_vtx_t;
#define K_VTX 384
static k_vtx_t  g_vtx[K_VTX] __attribute__((aligned(32)));

/* A's dp_cull_planes: the faces outside a run's window never come out. */
static void k_cull_planes(const float *gp, int x0, int y0, int x1, int y1) {
    const float W = (float)VIDEO_WIDTH, H = (float)VIDEO_HEIGHT, M = 4.0f;
    const float r0[4] = { 2.0f * gp[0] / W, 0.0f, -(2.0f * gp[2] / W - 1.0f), 0.0f };
    const float r1[4] = { 0.0f, 2.0f * gp[1] / H, -(1.0f - 2.0f * gp[3] / H), 0.0f };
    const float r3[4] = { 0.0f, 0.0f, -1.0f, 0.0f };
    const float aL = 2.0f * ((float)x0 - M) / W - 1.0f, aR = 2.0f * ((float)x1 + M) / W - 1.0f;
    const float bT = 1.0f - 2.0f * ((float)y0 - M) / H, bB = 1.0f - 2.0f * ((float)y1 + M) / H;
    for (int k = 0; k < 4; k++) {
        g_geo3d_cull_plane[0][k] = r0[k] - aL * r3[k];
        g_geo3d_cull_plane[1][k] = aR * r3[k] - r0[k];
        g_geo3d_cull_plane[2][k] = bT * r3[k] - r1[k];
        g_geo3d_cull_plane[3][k] = r1[k] - bB * r3[k];
        g_geo3d_cull_plane[4][k] = r3[k];
    }
    for (int k = 0; k < 5; k++) {
        const float *q = g_geo3d_cull_plane[k];
        g_geo3d_cull_nlen[k] = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    }
    g_geo3d_cull_on = 1;
}

/* The display list into the sink, run by run (A's dp_decode). */
static void k_decode(void) {
    const game_quirks_t *q = &g_active_profile->quirks;
    geo3d_tris_reset();
    geo3d_lines_reset();
    g_runs = 0;
    g_pv_n = g_face_n = 0;
    k_memo_reset();
    g_geo3d_mesh_epoch++;
    if (!g_geodl_snap_ready || !rs.main_data || !rs.polygons) { geo.captured_count = 0; return; }
    const uint32_t *snap = g_geodl_snap;
    g_geo_rs = geodl_raster_for(snap);
    if (!geo3d_scan_geo_list(&geo, snap, BUFF_RAM_SIZE / 4, g_geodl_snap_rstart,
                             (int16_t)mem_read16(&bus, H_SYNC_BASE), (int16_t)mem_read16(&bus, V_SYNC_BASE),
                             rs.main_data, rs.main_data_size, q->model_table_offset, q->model_table_count)) {
        geo.captured_count = 0;
        return;
    }
    g_geo3d_palram = bus.palette;
    g_geo3d_palram_size = PALETTE_SIZE;
    g_geo3d_flat_prev_z = 1.0e10f;
    g_geo3d_flat_list = 1;
    const int count = geo.captured_count, windows = geo.geo_windows > 0 ? geo.geo_windows : 1;
    for (int i = 0; i < count; ) {
        const captured_model_t *c0 = &geo.captured[i];
        int j = i;
        while (j < count && geo.captured[j].window == c0->window &&
               !memcmp(geo.captured[j].gproj, c0->gproj, sizeof c0->gproj) &&
               !memcmp(geo.captured[j].vp, c0->vp, sizeof c0->vp)) j++;
        int x0 = c0->vp[0] < 0 ? 0 : c0->vp[0], y0 = c0->vp[1] < 0 ? 0 : c0->vp[1];
        int x1 = c0->vp[2] > VIDEO_WIDTH ? VIDEO_WIDTH : c0->vp[2], y1 = c0->vp[3] > VIDEO_HEIGHT ? VIDEO_HEIGHT : c0->vp[3];
        if (!(x1 > x0 && y1 > y0) || g_runs >= K_RUNS) { i = j; continue; }
        k_cull_planes(c0->gproj, x0, y0, x1, y1);
        int slice = windows - 1 - (int)c0->window;
        const k_run_t *R = &g_run[g_runs];
        g_run[g_runs++] = (k_run_t){ c0->gproj[0] * SX, c0->gproj[2] * SX, -c0->gproj[1] * SY, c0->gproj[3] * SY,
                                     (uint8_t)(slice < 0 ? 0 : slice > 7 ? 7 : slice) };
        g_cur_kx = 2.0f * R->ax / SCR_W; g_cur_cx = 1.0f - 2.0f * R->bx / SCR_W;
        g_cur_ky = -2.0f * R->ay / SCR_H; g_cur_cy = 2.0f * R->by / SCR_H - 1.0f;
        g_cur_slice = R->slice;
        for (int k = i; k < j; k++) {
            const captured_model_t *cm = &geo.captured[k];
            g_light_dir[0] = cm->light[0]; g_light_dir[1] = cm->light[1]; g_light_dir[2] = cm->light[2];
            g_geo3d_obj_tpa = cm->tpa;
            g_geo3d_obj_tha = cm->tha;
            g_geo3d_board_luma = 1;
            g_geo3d_mode = cm->geo_mode;
            g_geo3d_zadjust = cm->zadjust;
            g_geo3d_lod = cm->geo_lod;
            if (cm->direct_len) {
                geo3d_decode_direct(geo.direct_words + cm->direct_off, cm->direct_len,
                                    rs.textures, rs.textures_size, rs.main_data, rs.main_data_size,
                                    cm->gproj[0], cm->gproj[1]);
            } else {
                geo3d_models_t from = {
                    rs.main_data, rs.main_data_size, rs.polygons, rs.polygons_size,
                    rs.textures, rs.textures_size, q->model_table_offset, q->model_table_count,
                    q->mesh_ptr_subtract, q->mesh_ptr_add, NULL, 0 };
                if (cm->model_idx < 0) {
                    uint32_t word = cm->dbg_mesh_ptr & 0x7FFFu;
                    from.obj_mesh      = (const uint8_t *)&g_geo_rs->polyram[(cm->dbg_mesh_ptr & 0x01000000u) ? 1 : 0][word];
                    from.obj_mesh_size = (0x8000u - word) * 4u;
                }
                geo3d_decode_model_cached(&from, cm->model_idx, cm->matrix, cm->color[0], cm->color[1], cm->color[2]);
            }
            g_geo3d_obj_tpa = g_geo3d_obj_tha = 0xFFFFFFFFu;
            g_geo3d_board_luma = 0;
        }
        i = j;
    }
    g_geo3d_flat_list = 0;
    g_geo3d_cull_on = 0;
    g_geo3d_palram = NULL;
}

/* Far to near: the window's slice, then the board's key, a tie to the later
 * face. Key = (7 - slice) << 29 | (0xFFFF - zkey) << 13 | index, ascending:
 * a two-pass radix sort of the top 19 bits, stable, as A's. */
static void k_sort(int n) {
    static unsigned lo[1025], hi[513];
    uint32_t *a = g_key, *b = g_order[1], *o = g_order[0];
    memset(lo, 0, sizeof lo); memset(hi, 0, sizeof hi);
    for (int t = 0; t < n; t++) { uint32_t k = a[t]; lo[((k >> 13) & 1023u) + 1]++; hi[(k >> 23) + 1]++; }
    for (int i = 1; i <= 1024; i++) lo[i] += lo[i - 1];
    for (int i = 1; i <= 512; i++) hi[i] += hi[i - 1];
    for (int t = 0; t < n; t++) b[lo[(a[t] >> 13) & 1023u]++] = a[t];
    for (int t = 0; t < n; t++) o[hi[b[t] >> 23]++] = b[t] & 0x1FFFu;
}

static void k_layer(GLuint id, float z) {
    glBindTexture(GL_TEXTURE_2D, id);
    glEnable(GL_TEXTURE_2D);
    const float u1 = (float)VIDEO_WIDTH / 512.0f, v1 = (float)VIDEO_HEIGHT / 512.0f;
    /* the same projection as the faces: x' and y' at w = 1 (z = -1) */
    glBegin(GL_QUADS);
    glColor4ub(255, 255, 255, 255);
    glTexCoord2f(0, 0);   glVertex3f(-1.0f,  1.0f, z);
    glTexCoord2f(u1, 0);  glVertex3f( 1.0f,  1.0f, z);
    glTexCoord2f(u1, v1); glVertex3f( 1.0f, -1.0f, z);
    glTexCoord2f(0, v1);  glVertex3f(-1.0f, -1.0f, z);
    glEnd();
}

/* ---- Tiles: the board's CPU compositor, uploaded as two textures ------------- */

static GLuint   g_bg_tex, g_fg_tex;
static bool     g_fg_any;

static void k_tiles(void) {
    static uint32_t s_tile = ~0u, s_gfx = ~0u, s_pal = ~0u, s_lut = ~0u;
    bool redraw = bus.gen_tile != s_tile || bus.gen_gfx != s_gfx || !tiles.valid;
    bool recolour = bus.gen_pal != s_pal || bus.gen_lut != s_lut || !tiles.valid;
    if (!redraw && !recolour) return;
    uint64_t t0 = timer_us_gettime64();
    if (redraw) {
        static tile_dirty_t d;
        static int16_t x0[VIDEO_HEIGHT], x1[VIDEO_HEIGHT];
        tile_cpu_snapshot(&tiles, &bus, &d, bus.gen_gfx != s_gfx, bus.gen_tile != s_tile);
        for (int k = 0; k < TILE_SNAP_WORDS / 512; k++) bus.tile_dirty[k] = 0;
        tile_cpu_lines(&d, x0, x1);
        if (d.full || d.count) tile_cpu_draw(&tiles, bus.tmapgfx, x0, x1);
    }
    if (recolour) {
        uint8_t chan[3][32];
        video_pen_channels(&bus, chan);
        tile_cpu_pens(&tiles, &bus, chan);
    }
    tiles.valid = true;
    s_tile = bus.gen_tile; s_gfx = bus.gen_gfx; s_pal = bus.gen_pal; s_lut = bus.gen_lut;
    static uint16_t pen[TILE_PEN_NONE + 1];
    for (int p = 0; p <= TILE_PEN_NONE; p++) {
        const uint8_t *c = tiles.pencol[p];
        pen[p] = c[3] ? (uint16_t)(0x8000u | (c[0] >> 3) << 10 | (c[1] >> 3) << 5 | (c[2] >> 3)) : 0;
    }
    g_fg_any = false;
    for (int i = 0; i < VIDEO_WIDTH * VIDEO_HEIGHT && !g_fg_any; i++) g_fg_any = tiles.fg[i] != TILE_PEN_NONE;
    for (int L = 0; L < 2; L++) {
        if (L && !g_fg_any) break;   /* an empty front layer is neither uploaded nor drawn */
        const uint16_t *src = L ? tiles.fg : tiles.bg;
        glBindTexture(GL_TEXTURE_2D, L ? g_fg_tex : g_bg_tex);
        for (int y0 = 0; y0 < VIDEO_HEIGHT; y0 += 256) {   /* in halves: g_stage is 512x256 */
            int h = VIDEO_HEIGHT - y0 < 256 ? VIDEO_HEIGHT - y0 : 256;
            for (int y = 0; y < h; y++) {
                const uint16_t *s = &src[(y0 + y) * VIDEO_WIDTH];
                uint16_t *d = &g_stage[y * 512];
                for (int x = 0; x < VIDEO_WIDTH; x++) d[x] = pen[s[x]];
                for (int x = VIDEO_WIDTH; x < 512; x++) d[x] = 0;
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y0, 512, h, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, g_stage);
        }
    }
    g_k.us_tiles += timer_us_gettime64() - t0;
}

static void k_init(void) {
    GLdcConfig cfg;
    glKosInitConfig(&cfg);
    cfg.autosort_enabled = GL_FALSE;              /* we sort: the board's order */
    cfg.internal_palette_format = GL_RGBA8;
    cfg.texture_twiddle = GL_TRUE;
    cfg.initial_tr_capacity = K_TR_CAP;
    cfg.initial_op_capacity = 64;    /* nothing goes in the opaque or punch-through lists */
    cfg.initial_pt_capacity = 64;
    cfg.initial_immediate_capacity = 512;
    glKosInitEx(&cfg);
    for (int v = 0; v < 256; v++) g_cx[v] = (uint8_t)(v > 64 ? (v - 64) * 255 / 191 : 0);
    static uint8_t pal[2][16][4];
    for (int i = 0; i < 16; i++)
        for (int p = 0; p < 2; p++) {
            pal[p][i][0] = pal[p][i][1] = pal[p][i][2] = (uint8_t)(i * 17);
            pal[p][i][3] = p && i == 15 ? 0 : 255;
        }
    glEnable(GL_SHARED_TEXTURE_PALETTE_EXT);
    glColorTableEXT(GL_SHARED_TEXTURE_PALETTE_0_KOS, GL_RGBA8, 16, GL_RGBA, GL_UNSIGNED_BYTE, pal[0]);
    glColorTableEXT(GL_SHARED_TEXTURE_PALETTE_1_KOS, GL_RGBA8, 16, GL_RGBA, GL_UNSIGNED_BYTE, pal[1]);
    GLuint ids[3];
    glGenTextures(3, ids);
    g_bg_tex = ids[0]; g_fg_tex = ids[1]; g_text_tex = ids[2];
    for (int i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_2D, ids[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    for (int i = 0; i < 3; i++) {   /* allocated once, zeroed: k_tiles and the text refill them */
        glBindTexture(GL_TEXTURE_2D, ids[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_ARGB1555_KOS, i == 2 ? TX_W : 512, i == 2 ? TX_H : 512, 0,
                     GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, NULL);
    }
    glBindTexture(GL_TEXTURE_2D, g_text_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glViewport(0, 0, SCR_W, SCR_H);
    glClearColor(0.0f, 0.0f, 0.3f, 1.0f);   /* blue until the board is up: GL itself is working */
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glShadeModel(GL_FLAT);
    /* w = -z, the faces' x and y premixed per run (k_frame), z only for the
     * near clip: n = 0.001, f = 1e5 */
    const float n = 0.001f, f = 100000.0f;
    const GLfloat P[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, -(f + n) / (f - n), -1,  0, 0, -2 * f * n / (f - n), 0 };
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(P);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnable(GL_NEARZ_CLIPPING_KOS);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
}

static void k_text_quads(void) {
    if (g_text_dirty) {
        glBindTexture(GL_TEXTURE_2D, g_text_tex);
        for (int r0 = 0; r0 < TX_ROWS; r0 += TX_ROWS / 2) {   /* five 24-px rows at a time fit g_stage */
            memset(g_stage, 0, TX_W * 24 * (TX_ROWS / 2) * 2);
            for (int r = r0; r < r0 + TX_ROWS / 2; r++)
                if (g_text_rows[r][0]) bfont_draw_str(g_stage + (r - r0) * 24 * TX_W, TX_W, true, g_text_rows[r]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, r0 * 24, TX_W, 24 * (TX_ROWS / 2), GL_BGRA,
                            GL_UNSIGNED_SHORT_1_5_5_5_REV, g_stage);
        }
        g_text_dirty = false;
    }
    bool any = false;
    for (int r = 0; r < TX_ROWS; r++) any |= g_text_rows[r][0] != 0;
    if (!any) return;
    glBindTexture(GL_TEXTURE_2D, g_text_tex);
    glEnable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glColor4ub(255, 255, 255, 255);
    for (int r = 0; r < TX_ROWS; r++) {
        int n = (int)strlen(g_text_rows[r]);
        if (!n) continue;
        float w = (float)(n * 12) * 0.5f, y0 = (float)(r * 24), y1 = y0 + 24.0f;   /* 12-px cells at half width */
        float nx1 = w / (SCR_W * 0.5f) - 1.0f, ny0 = 1.0f - y0 / (SCR_H * 0.5f), ny1 = 1.0f - y1 / (SCR_H * 0.5f);
        float u1 = (float)(n * 12) / TX_W, v0 = y0 / TX_H, v1 = y1 / TX_H;
        glTexCoord2f(0, v0);  glVertex3f(-1.0f, ny0, -1.0f);
        glTexCoord2f(u1, v0); glVertex3f(nx1, ny0, -1.0f);
        glTexCoord2f(u1, v1); glVertex3f(nx1, ny1, -1.0f);
        glTexCoord2f(0, v1);  glVertex3f(-1.0f, ny1, -1.0f);
    }
    glEnd();
}

static void k_text_frame(void) {
    glClear(GL_COLOR_BUFFER_BIT);
    k_text_quads();
    glKosSwapBuffers();
}

/* One frame: tiles behind, the faces back to front, tiles in front, the text. */
static void k_frame(void) {
    uint64_t t0 = timer_us_gettime64();
    k_tiles();
    kt_invalidate(&bus);
    if (g_tex_n >= KT_SLOTS * 3 / 4) kt_drop_all();
    uint64_t ta = timer_us_gettime64();
    k_decode();
    uint64_t tb = timer_us_gettime64();
    const int n = g_face_n;
    k_sort(n);
    uint64_t tc = timer_us_gettime64();

    glClear(GL_COLOR_BUFFER_BIT);
    k_layer(g_bg_tex, -1.0f);
    /* The faces, in batches of one texture. */
    glVertexPointer(3, GL_FLOAT, sizeof(k_vtx_t), &g_vtx[0].x);
    glTexCoordPointer(2, GL_FLOAT, sizeof(k_vtx_t), &g_vtx[0].u);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(k_vtx_t), &g_vtx[0].c);
    /* What fits the reserved list, counted from the nearest face: a triangle
     * is 3 vertices and a draw 1 header. The near clip adds a vertex to a
     * triangle it cuts, which few are; K_TR_RESERVE covers those. */
    int start = 0;
    {
        int cost = 0, run = 0;
        kt_tex_t *pc = (kt_tex_t *)1;
        for (int k = n - 1; k >= 0; k--) {
            const k_face_t *F = &g_face[g_order[0][k]];
            int c = 3;
            if (F->tex != pc || run + 3 > K_VTX) { c++; pc = F->tex; run = 0; }
            run += 3;
            if (cost + c > K_TR_CAP - K_TR_RESERVE) { start = k + 1; break; }
            cost += c;
        }
        g_face_dropped += (unsigned)start;
    }
    int nv = 0;
    kt_tex_t *cur = (kt_tex_t *)1;
    for (int k = start; k < n; k++) {
        const k_face_t *F = &g_face[g_order[0][k]];
        if (F->tex != cur || nv + 3 > K_VTX) {
            if (nv) glDrawArrays(GL_TRIANGLES, 0, nv);
            nv = 0;
            if (F->tex != cur) {
                if (F->tex) { glBindTexture(GL_TEXTURE_2D, F->tex->id); glEnable(GL_TEXTURE_2D); }
                else glDisable(GL_TEXTURE_2D);
                cur = F->tex;
            }
        }
        for (int v = 0; v < 3; v++) {
            const k_pv_t *P = &g_pv[F->v[v]];
            k_vtx_t *V = &g_vtx[nv++];
            V->x = P->x; V->y = P->y; V->z = P->z;
            V->u = F->u[v]; V->v = F->t[v];
            V->c = F->c;
        }
    }
    if (nv) glDrawArrays(GL_TRIANGLES, 0, nv);
    g_k.tris = (unsigned)n;
    if (g_k.tris > g_k.tmax) g_k.tmax = g_k.tris;
    if (g_fg_any) k_layer(g_fg_tex, -1.0f);
    k_text_quads();
    glKosSwapBuffers();
    uint64_t t1 = timer_us_gettime64();
    g_k.us_decode += tb - ta;
    g_k.us_sort += tc - tb;
    g_k.us_submit += t1 - tc;
    (void)t0;
}

/* ---- The board, as A's main_dc.c sets it up ----------------------------------- */

static uint8_t *dc_window(const char *name, size_t size) {
    if (!strcmp(name, "MAIN_DATA") && rs.main_data_size >= size) return rs.main_data;
    if (!strcmp(name, "XTRA_DATA") && rs.main_data_size >= 0x1000000u + size) return rs.main_data + 0x1000000u;
    if (!strcmp(name, "VID_EXT_RAM")) return pg_anon(0, (uint32_t)size);
    return NULL;
}

static int dc_romset(void) {
    static const struct { const char *name; size_t off_p, off_s; } map[] = {
        { "maincpu",   offsetof(romset_t, maincpu),    offsetof(romset_t, maincpu_size) },
        { "main_data", offsetof(romset_t, main_data),  offsetof(romset_t, main_data_size) },
        { "copro",     offsetof(romset_t, copro_data), offsetof(romset_t, copro_data_size) },
        { "polygons",  offsetof(romset_t, polygons),   offsetof(romset_t, polygons_size) },
        { "textures",  offsetof(romset_t, textures),   offsetof(romset_t, textures_size) },
        { "audiocpu",  offsetof(romset_t, audiocpu),   offsetof(romset_t, audiocpu_size) },
        { "samples",   offsetof(romset_t, samples),    offsetof(romset_t, samples_size) },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        size_t size = 0;
        uint8_t *p = pg_region(map[i].name, &size);
        *(uint8_t **)((char *)&rs + map[i].off_p) = size ? p : NULL;
        *(size_t *)((char *)&rs + map[i].off_s)   = size;
    }
    rs.loaded = rs.maincpu != NULL;
    for (size_t i = 0; i < g_profile_count; i++)
        if (!strcmp(g_profiles[i]->id, g_pg.lay->profile)) g_active_profile = g_profiles[i];
    if (!g_active_profile) return -1;
    if (rs.loaded) profile_adopt_program(rs.maincpu, rs.maincpu_size);
    return rs.loaded ? 0 : -1;
}

static int dc_hook_sound(i960_cpu_t *c, memory_bus_t *b) {
    (void)b;
    ds_code(c->globals.g[0]);
    hle_ret(c);
    return 0;
}

static game_profile_t dc_profile;
static void dc_add_sound_hook(void) {
    if (strncmp(g_active_profile->id, "sfight", 6) || g_active_profile->any_program ||
         g_active_profile->hook_count >= HLE_HOOK_TABLE_MAX) return;
    dc_profile = *g_active_profile;
    dc_profile.hooks[dc_profile.hook_count++] =
        (hle_hook_entry_t){ 0x0003F268, dc_hook_sound, "sound_request_special (dc_sound)" };
    g_active_profile = &dc_profile;
}

static uint32_t dc_uart_read(mem_region_t *r, uint32_t addr, int size) {
    (void)r; (void)size;
    return addr - MIDI_BASE == 4 ? 0x05u : 0u;
}

static void dc_install_board(void) {
    pg_rom_revert();
    pg_anon_clear(0);
    g_active_profile->install_fn(&rs, &cpu, &bus);
    for (int i = 0; i < bus.region_count; i++)
        if (bus.regions[i].base == MIDI_BASE) { bus.regions[i].read_cb = dc_uart_read; mem_regions_changed(&bus); }
    irqt_reset();
    input_reset();
    input_attach(&bus);
    emu_board_reset_state();
}

static void dc_pad(void) {
    static const struct { uint32_t mask; int act; } map[] = {
        { CONT_DPAD_UP,    GAME_INPUT_P1_UP },   { CONT_DPAD_DOWN,  GAME_INPUT_P1_DOWN },
        { CONT_DPAD_LEFT,  GAME_INPUT_P1_LEFT }, { CONT_DPAD_RIGHT, GAME_INPUT_P1_RIGHT },
        { CONT_A, GAME_INPUT_P1_B1 }, { CONT_B, GAME_INPUT_P1_B2 },
        { CONT_X, GAME_INPUT_P1_B3 }, { CONT_Y, GAME_INPUT_P1_B4 },
        { CONT_START, GAME_INPUT_P1_START },
    };
    static uint32_t was;
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t *st = dev ? (cont_state_t *)maple_dev_status(dev) : NULL;
    uint32_t now = 0;
    if (st) {
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (st->buttons & map[i].mask) now |= 1u << map[i].act;
        if (st->ltrig > 128) now |= 1u << GAME_INPUT_P1_COIN;
    }
    for (int a = 0; a < GAME_INPUT_COUNT; a++) {
        uint32_t b = 1u << a;
        if ((now & b) && !(was & b)) input_action_down(a);
        if (!(now & b) && (was & b)) input_action_up(a);
    }
    was = now;
}

static void k_stop(const char *why) {
    kt_row(1, why);
    for (;;) { k_text_frame(); thd_sleep(500); }
}

/* The largest block malloc can still hand out, in KB. */
static unsigned k_free_kb(void) {
    unsigned lo = 0, hi = 16384;
    while (lo < hi) {
        unsigned mid = (lo + hi + 1) / 2;
        void *volatile p = memalign(32, (size_t)mid << 10);   /* volatile: GCC drops a malloc freed unused */
        if (p) { free(p); lo = mid; } else hi = mid - 1;
    }
    return lo;
}
static char g_heap_line[96];
#define K_HEAP(tag) do { size_t l_ = strlen(g_heap_line); snprintf(g_heap_line + l_, sizeof g_heap_line - l_, " %s %u", tag, k_free_kb()); kt_row(9, g_heap_line); } while (0)

#define K_STEP(s) do { kt_row(1, s); k_text_frame(); } while (0)   /* boot progress: Flycast's libretro core has no serial console */

int main(int argc, char **argv) {
    (void)argc; (void)argv;
#ifndef NDEBUG
    assert_set_handler(kb_assert);
#endif
    strcpy(g_heap_line, "heap KB:");
    K_HEAP("start");
    k_init();
    K_HEAP("gl");
    kt_row(0, "m2-hle2 (GLdc build) for Dreamcast: finding the ROM files");
    k_text_frame();

    K_STEP("sound");
    bool sound = ds_init() == 0;
    K_STEP("sincos");
    {
        uint32_t size = 0, fad = pg_find_file("SINCOS.BIN", &size);
        uint32_t *t = fad && size == 0x20000u * 4u ? memalign(32, size) : NULL;
        if (t && cdrom_read_sectors(t, fad, size / 2048) == ERR_OK) g_sharc_sincos = t;
        else free(t);
    }
    K_HEAP("snd");
    K_STEP("pager");
    const uint32_t keep = TEXRAM0_SIZE + TEXRAM1_SIZE + FRAMEBUFFER_SIZE + (512u << 10);
    uint32_t cache = 8u << 20;
    for (void *p; cache > (1u << 20); cache -= 256u << 10)
        if ((p = memalign(16384, cache + keep))) { free(p); break; }
    if (pg_init(&dc_layout_sfight, cache, VID_EXT_RAM_SIZE) != 0 || dc_romset() != 0)
        k_stop("the disc lacks a ROM file (dc_layout.h)");
    K_STEP("gems");
    g_mem_window = dc_window;
    if (sound) dc_add_sound_hook();
    g_gems_i960 = g_gems_cop = 1;
    bool gems = gems_apply(profile_rom_set(g_active_profile));
    char line[128];
#if defined(I960_AOT) && I960_AOT
    const char *aot = " +aot";
#else
    const char *aot = "";
#endif
    snprintf(line, sizeof line, "GLdc: profile %s%s%s, cache %u KB", g_active_profile->id, gems ? " +gems" : "", aot,
             (unsigned)(cache >> 10));
    kt_row(2, line);
    K_HEAP("pg");
    K_STEP("board RAM");
    dbgio_dev_select("null");
    if (!mem_init(&bus, NULL, 0)) k_stop("out of memory for the board's RAM");
    K_STEP("board");
    i960_reset(&cpu);
    dc_install_board();
    emu_ctx_init(&ctx, &cpu, &bus);
    geo3d_init(&geo);
    ctx.run_state = EMU_RUNNING;
    K_STEP("running");
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    uint64_t t_last = timer_us_gettime64(), t_boot = t_last, us_all = 0, us_dall = 0, n_drawn = 0;
    uint32_t f_last = g_emu_frames, shown = 0, slices = 0;
    uint64_t us_slice = 0;
    int hashed = 0;
    static char hashed_line[96], b_line[96], b_line2[96];
    while (!cpu.halted) {
        dc_pad();
        uint64_t t0 = timer_us_gettime64();
        if (g_emu_frames < 4) { snprintf(line, sizeof line, "slice %u", (unsigned)g_emu_frames); K_STEP(line); }
        emu_slice_body(&ctx);
        emu_slice_finish(&ctx);
        uint64_t t1 = timer_us_gettime64();
        if (g_emu_frames < 4) { snprintf(line, sizeof line, "frame %u", (unsigned)g_emu_frames); K_STEP(line); }
        k_frame();
        shown++; n_drawn++;
#if DC_FPS_CAP
        {   /* KOS's way to wait: thd_sleep gives the time to the sound thread */
            static uint64_t cap_t0;
            static uint32_t cap_f0;
            uint64_t now = timer_us_gettime64();
            uint64_t due = cap_t0 + (uint64_t)(g_emu_frames - cap_f0) * 1000000u / DC_FPS_CAP;
            if (!cap_t0 || now > due + 100000u) { cap_t0 = now; cap_f0 = g_emu_frames; }
            else if (due > now + 1000u) thd_sleep((int)((due - now) / 1000u));
        }
#endif
        if (DC_HASH_FRAME && g_emu_frames >= DC_HASH_FRAME && !hashed) {
            uint32_t h = 2166136261u;
            for (uint32_t a = 0x500000u; a < 0x600000u; a += 4) h = (h ^ mem_read32(&bus, a)) * 16777619u;
            for (int r = 0; r < 32; r++) h = (h ^ ((uint32_t *)&cpu.globals)[r]) * 16777619u;
            h = (h ^ (uint32_t)cpu.cycles) * 16777619u;
            snprintf(hashed_line, sizeof hashed_line, "f%u %08lx ip %lx sl %lu dr %lu/%lu all %lu ms", (unsigned)g_emu_frames,
                     (unsigned long)h, (unsigned long)cpu.sfr.ip, (unsigned long)(us_all / 1000),
                     (unsigned long)(us_dall / 1000), (unsigned long)n_drawn, (unsigned long)((t1 - t_boot) / 1000));
            kt_row(6, hashed_line);
            hashed = 1;
        }
        {   /* the same fixed stretch of the fight as A's bench line */
            static uint64_t b_t0, b_d0, b_sl, b_p0[5];
            const uint64_t b_p[5] = { g_k.us_tiles, g_k.us_decode, g_k.us_sort, g_k.us_submit, g_k.us_tex };
            if (!b_t0 && g_emu_frames > DC_BENCH_F0) { b_t0 = t0; b_d0 = us_dall; memcpy(b_p0, b_p, sizeof b_p); }
            if (b_t0 && !b_line[0]) {
                b_sl += t1 - t0;
                if (g_emu_frames >= DC_BENCH_F1) {
                    snprintf(b_line, sizeof b_line, "f%u-%u %lu ms: sl %lu dr %lu", DC_BENCH_F0, (unsigned)g_emu_frames,
                             (unsigned long)((t1 - b_t0) / 1000), (unsigned long)(b_sl / 1000),
                             (unsigned long)((us_dall - b_d0) / 1000));
                    snprintf(b_line2, sizeof b_line2, "ti %lu dec %lu so %lu su %lu tx %lu",
                             (unsigned long)((b_p[0] - b_p0[0]) / 1000), (unsigned long)((b_p[1] - b_p0[1]) / 1000),
                             (unsigned long)((b_p[2] - b_p0[2]) / 1000), (unsigned long)((b_p[3] - b_p0[3]) / 1000),
                             (unsigned long)((b_p[4] - b_p0[4]) / 1000));
                    kt_row(3, b_line);
                    kt_row(4, b_line2);
                }
            }
        }
        ds_pump();
        uint64_t t2 = timer_us_gettime64();
        us_slice += t1 - t0; us_all += t1 - t0; us_dall += t2 - t1; slices++;
        if (t2 - t_last >= 2000000) {
            uint32_t fr = g_emu_frames - f_last;
            double sec = (double)(t2 - t_last) / 1e6;
            snprintf(line, sizeof line, "GLdc frame %u %.1f fps (shown %.1f) slice %u ms",
                     (unsigned)g_emu_frames, fr / sec, shown / sec, (unsigned)(us_slice / 1000 / (slices ? slices : 1)));
            kt_row(0, line);
            snprintf(line, sizeof line, "tex %u new %u drop %u fail %u tris %u max %u lost %u heap %u", g_tex_n,
                     g_k.made, g_k.dropped, g_k.fails, g_k.tris, g_k.tmax, g_face_dropped, k_free_kb());
            kt_row(8, line);
            t_last = t2; f_last = g_emu_frames; shown = 0; slices = 0; us_slice = 0;
        }
    }
    k_stop("the board halted");
    return 0;
}
