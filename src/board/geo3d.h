/*
 * geo3d.h — board-level 3D state for Model 2.
 *
 * Owns the captured-model list, the model-table → pol-pointer lookup, and
 * the COP capture-stream scanner that pulls draw-commands out of the
 * geo_capture ring in cop.h.  The actual GPU draw + index-array polygon
 * decoder lives in src/ui/game_render.h (Phase 9 step 2).
 *
 * Per-game knobs (model table offset/count, mesh-pointer encoding) come from
 * game_profile_t.quirks.  Algorithm is board-level — same code drives every
 * Model 2 game once its quirks are filled in.
 *
 * STF-specific tuning (orbital objects, flying-carpet child offsets) is NOT
 * here — that lived in stf-hle's monolithic scanner and will move to a
 * per-profile callback if/when needed.  The basic scanner handles:
 *   0x02000404 + 12 floats  — set matrix (3x4 row-major)
 *   0x03000606 + 3  floats  — set position
 *   0x3C007878 + 6  words   — object draw header  (word[5] = mesh ptr)
 */
#ifndef GEO3D_H
#define GEO3D_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "constants.h"
#include "cop.h"            /* g_cop.geo_capture[], g_sharc.tgp_bone[] */
#include "log.h"
#include "memory.h"         /* mem_read8 / mem_read32 */

/* ---- Capacities ---------------------------------------------------------- */

#define MAX_GEO_MODELS     512
#define GEO3D_MAX_LINES    32768
#define MODEL_LOOKUP_SIZE  8192
#define MODEL_ENTRY_SIZE   16
#define VERTEX_PAIR_SIZE   40

/* Index-array decoder buffers.  Sized for the largest meshes across games:
 * sfight's longest is ~700 vertex pairs, but Fighting Vipers' "FIGHTING VIPERS"
 * logo (model 2411) is ~2527 pairs (5054 verts) — at 2001 the decoder truncated
 * it mid-mesh and the "rs" was cut off.  Bumped to 4096 with headroom. */
#define GEO3D_IA_MAX_VPS   4096
#define GEO3D_IA_MAX_VERTS (GEO3D_IA_MAX_VPS * 2)
#define GEO3D_IA_MAX_IDX   (4 + GEO3D_IA_MAX_VPS * 4)

/* Upper bound on words scanned when the per-frame geo window is unusable (first
 * frame, or a board_vblank marking race). Caps the cost of accumulated draws ×
 * the O(model_table_count) lookup so a raced frame can't freeze the render. One
 * real game frame is far smaller than this. */
#define GEO3D_SCAN_FALLBACK_MAX 8192

/* Global face-color palette in main_data (STF): BGR555 LE u16 per entry,
 * indexed by the per-face material index.  color = pal[main_data + OFF + matidx*2].
 * STF-specific; move to game_quirks_t if another ROMset places it elsewhere. */
#define GEO3D_PALETTE_OFF  0x00100000u

/* Texture atlas (logical layout per MAME get_texel): each sheet is 2048×1024
 * 4-bit luma.  STF has two texture banks (texram0 / texram1); the texsheet bit
 * (texheader[2] bit12) selects.  We stack both sheets into one atlas: texram0 in
 * the top half (v 0..0.5), texram1 in the bottom half (v 0.5..1.0). */
#define GEO3D_ATLAS_W      2048
#define GEO3D_SHEET_H      1024   /* one sheet */
#define GEO3D_ATLAS_H      2048   /* two sheets stacked */

/* ---- Small math types ---------------------------------------------------- */

typedef struct { float x, y, z; } vec3_t;

static inline float read_float_le(const uint8_t *p) {
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static inline uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline float u32_as_float(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* Reject NaN/inf/insane-magnitude bit patterns when sniffing for floats in
 * the COP stream — used to gate matrix/position commands. */
static inline bool is_sane_float(uint32_t u) {
    float f = u32_as_float(u);
    if (f != f) return false;
    if (f > 1.0e6f || f < -1.0e6f) return false;
    return true;
}

/* Apply a 3x4 row-major matrix (9 rotation + 3 translation) to a vec3. */
static inline vec3_t apply_matrix(vec3_t v, const float *m) {
    vec3_t r;
    r.x = m[0]*v.x + m[1]*v.y + m[2]*v.z  + m[3];
    r.y = m[4]*v.x + m[5]*v.y + m[6]*v.z  + m[7];
    r.z = m[8]*v.x + m[9]*v.y + m[10]*v.z + m[11];
    return r;
}

/* ---- Decal quad cut ------------------------------------------------------
 * A decal on this board is not a face floating in front of a surface: it is
 * the surface's own faces emitted a second time with a cut-out texture, so the
 * holes let the first copy show (Sonic's shouting head, model 3544, is six such
 * quads on six of the muzzle's; 504 models carry at least one). The board never
 * compares the two — both take their z from the same corners and the later
 * submission keeps the bucket — and a LESS_EQUAL depth test reproduces that for
 * free, but only while the copies are the same triangles.
 *
 * They are not always. A quad is filled as two triangles along a diagonal that
 * comes from where its strip anchored, and the copy anchors elsewhere: a quad
 * with any warp in it bulges one way under one cut and the other way under the
 * other, and the half that bulges behind the original is drawn behind it. So a
 * quad whose four corners were emitted before in this model is cut the way that
 * one was — same corners, same winding, same UVs on the same corners.
 *
 * The corner key is noclip's `cornerKey` (vendor/noclip js/model.js) bit for
 * bit — untransformed Z-negated position, rounded to 1/4096 the way
 * Math.round rounds, FNV over two 16-bit halves — so tools/grade-models.mjs
 * holds the two decoders to J = 1 on exactly the same triangles. The whole of
 * the J = 0.990 that tool first measured was this rule and nothing else. */
#define GEO3D_SPLIT_SLOTS 8192u   /* power of two, > 2 × GEO3D_IA_MAX_VPS */

static inline uint32_t geo3d_corner_key(vec3_t p) {
    const float c[3] = { p.x, p.y, p.z };
    uint32_t h = 2166136261u;
    for (int a = 0; a < 3; a++) {
        uint32_t q = (uint32_t)(int32_t)floor((double)c[a] * 4096.0 + 0.5);
        h = (h ^ (q & 0xffffu)) * 16777619u;
        h = (h ^ ((q >> 16) & 0xffffu)) * 16777619u;
    }
    return h;
}

static uint32_t g_geo3d_split_gen;
static uint32_t g_geo3d_split_stamp[GEO3D_SPLIT_SLOTS];
static uint32_t g_geo3d_split_quad[GEO3D_SPLIT_SLOTS];
static uint32_t g_geo3d_split_cut[GEO3D_SPLIT_SLOTS];

/* Forget every quad seen so far — once per model. */
static inline void geo3d_split_reset(void) {
    if (++g_geo3d_split_gen == 0) {
        memset(g_geo3d_split_stamp, 0, sizeof g_geo3d_split_stamp);
        g_geo3d_split_gen = 1;
    }
}

/* True when a quad with these corners was already cut along the other diagonal.
 * The first sighting records its cut and answers false. */
static inline bool geo3d_split_other_way(uint32_t quad, uint32_t cut) {
    uint32_t i = (quad * 2654435761u) & (GEO3D_SPLIT_SLOTS - 1u);
    while (g_geo3d_split_stamp[i] == g_geo3d_split_gen) {
        if (g_geo3d_split_quad[i] == quad) return g_geo3d_split_cut[i] != cut;
        i = (i + 1u) & (GEO3D_SPLIT_SLOTS - 1u);
    }
    g_geo3d_split_stamp[i] = g_geo3d_split_gen;
    g_geo3d_split_quad[i]  = quad;
    g_geo3d_split_cut[i]   = cut;
    return false;
}

/* Decode a Model 2 BGR555 colour word to normalized float RGB.
 *   bits 0-4 = R, bits 5-9 = G, bits 10-14 = B  (matches game's colpal()).
 * The material stream stores this as a little-endian uint16. */
#define GEO3D_C5(i) ((float)((i) * 255 / 31) / 255.0f)
static const float g_geo3d_c5[32] = {   /* the three divisions per channel below, folded */
    GEO3D_C5(0),  GEO3D_C5(1),  GEO3D_C5(2),  GEO3D_C5(3),  GEO3D_C5(4),  GEO3D_C5(5),  GEO3D_C5(6),  GEO3D_C5(7),
    GEO3D_C5(8),  GEO3D_C5(9),  GEO3D_C5(10), GEO3D_C5(11), GEO3D_C5(12), GEO3D_C5(13), GEO3D_C5(14), GEO3D_C5(15),
    GEO3D_C5(16), GEO3D_C5(17), GEO3D_C5(18), GEO3D_C5(19), GEO3D_C5(20), GEO3D_C5(21), GEO3D_C5(22), GEO3D_C5(23),
    GEO3D_C5(24), GEO3D_C5(25), GEO3D_C5(26), GEO3D_C5(27), GEO3D_C5(28), GEO3D_C5(29), GEO3D_C5(30), GEO3D_C5(31),
};
#undef GEO3D_C5

static inline void geo3d_bgr555(uint16_t cw, float *r, float *g, float *b) {
    *r = g_geo3d_c5[(cw      ) & 0x1F];   /* (c5 * 255 / 31) / 255.0f */
    *g = g_geo3d_c5[(cw >>  5) & 0x1F];
    *b = g_geo3d_c5[(cw >> 10) & 0x1F];
}

/* Material pointer → stable distinct RGB.  Same material always maps to the
 * same colour; different materials get different hues. */
static inline void material_ptr_to_color(uint32_t mat_ptr, float *r, float *g, float *b) {
    uint32_t h = mat_ptr;
    h ^= h >> 16; h *= 0x45D9F3Bu; h ^= h >> 16;
    float hue = (float)(h & 0xFFFF) / 65536.0f;
    float s = 0.85f, v = 1.0f;
    float hh = hue * 6.0f;
    int   seg = (int)hh;
    float ff  = hh - (float)seg;
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * ff);
    float t = v * (1.0f - s * (1.0f - ff));
    switch (seg % 6) {
        case 0:  *r = v; *g = t; *b = p; break;
        case 1:  *r = q; *g = v; *b = p; break;
        case 2:  *r = p; *g = v; *b = t; break;
        case 3:  *r = p; *g = q; *b = v; break;
        case 4:  *r = t; *g = p; *b = v; break;
        default: *r = v; *g = p; *b = q; break;
    }
}

/* ---- Captured model ------------------------------------------------------ */

typedef struct {
    int      model_idx;
    bool     has_matrix;
    float    matrix[12];     /* 3x4 row-major */
    uint32_t material_ptr;
    float    color[3];
    /* Clip window (screen-space, game coords 0–495 × 0–383).
     * Set when a GEO_WIN_SENTINEL precedes this model in the capture stream.
     * clip_win_x/y are the top-left origin of the window in game pixels. */
    bool     has_clip_win;
    /* True when this model was placed via the bone path (scan_active_bslot, i.e.
     * a fighter rob).  Bone geometry is in WORLD space; non-bone fight-stage
     * geometry comes through the FIFO already camera-relative (view space). */
    bool     from_bone;
    int16_t  clip_win_x;  /* left edge in game pixels */
    int16_t  clip_win_y;  /* top  edge in game pixels */
    int16_t  clip_win_w;  /* width  in game pixels */
    int16_t  clip_win_h;  /* height in game pixels */
    /* Placed by the GEO display list (geo3d_scan_geo_list): `matrix` is already
     * the full model-to-eye transform the COP laid down, so the renderer skips
     * the host camera and projects the board's way instead — gproj holds focal
     * x, focal y and the projection centre in screen pixels (y from the top),
     * vp the window's scissor in screen pixels (x0, y0, x1, y1), window which
     * window the object belongs to (earlier windows paint over later ones), and
     * light the GEO light vector in host eye space. */
    bool     view_space;
    float    gproj[4];
    int16_t  vp[4];
    uint16_t window;
    float    light[3];
    uint32_t geo_mode;      /* the list's mode word (bit 0: specular) and LOD scale */
    float    geo_lod;       /* when the object was drawn (model2_v.cpp commands 07, 16) */
    uint32_t tpa, tha;      /* the object command's texture point / header addresses */
    /* Debug fields populated by the scanner */
    uint32_t dbg_mesh_ptr;
    float    dbg_pos[3];
    float    dbg_ang_deg[3]; /* Euler angles in degrees (X, Y, Z) */
    int      dbg_have_pos;
    int      dbg_have_ang;
    int      dbg_have_mat;
} captured_model_t;

/* ---- 3D line (for the GPU pipeline to consume) -------------------------- */

typedef struct {
    float x0, y0, z0;
    float x1, y1, z1;
    float r, g, b;
} geo3d_line_t;

typedef struct {
    geo3d_line_t lines[GEO3D_MAX_LINES];
    int          count;
} geo3d_line_buf_t;

static geo3d_line_buf_t g_geo3d_lines = {0};

static inline void geo3d_lines_reset(void) { g_geo3d_lines.count = 0; }

/* ---- Solid triangle buffer ----------------------------------------------- */

#define GEO3D_MAX_TRIS GEO3D_MAX_LINES

typedef struct {
    float x0, y0, z0,  u0, v0;   /* u,v = tile-relative texel (may run past the tile) */
    float x1, y1, z1,  u1, v1;
    float x2, y2, z2,  u2, v2;
    float r, g, b;
    float tx, ty, tw, th;        /* atlas tile rect (pixels); tw<=0 → untextured */
    float lb, pl;                /* lumabase (lumaram band) + poly_luma (0..1 lighting) */
    float fl;                    /* GEO3D_FACE_* bits, carried as a float to the shader */
    float texlod;                /* the board's per-polygon texlod, or GEO3D_TEXLOD_NONE */
    float zs0, zs1, zs2;         /* the substituted depth per corner, or NONE */
} geo3d_tri_t;

/* A polygon's texture LOD as the rasterizer takes it (model2_v.cpp, raster
 * command 01): log2 of the geometrizer's LOD distance in 1/128ths, the integer
 * part from the float's exponent and the fraction out of log RAM. The fill
 * picks the mip level per pixel as (log2(z^2) - texlod) >> 7. Faces not drawn
 * from a display list carry NONE, and the shader falls back to screen-space
 * derivatives for them. */
#define GEO3D_TEXLOD_NONE 1.0e6f
static float g_geo3d_emit_texlod = GEO3D_TEXLOD_NONE;

/*
 * The board's polygon z-sort, which this renderer's depth buffer has to be told
 * about (model2_v.cpp model2_3d_process_polygon; the explorer's port is
 * vendor/noclip js/viewer.js VERT_SHADER, and js/model.js carries the corners).
 *
 * The board has no depth buffer. It gives a WHOLE POLYGON one z — the nearest
 * or the farthest of its corners, whichever bits 10..11 of the attribute word
 * name — sorts on that into 65536 buckets drawn near-first, and the fill writes
 * a pixel only where nothing has. So a polygon wins a pixel outright, whatever
 * the geometry does between it and the next, and the artists used that: a big
 * backing plane asks to be sorted by its far corner and stands out of the way
 * of the small faces painted on it.
 *
 * Mode 0 is not a mode. It means "keep the previous polygon's corners", which
 * lands the face in that polygon's bucket — the board's own way of declaring a
 * decal — so the corners and the last real mode are carried forward across the
 * face loop exactly as the ROM emits them.
 *
 *   1  the nearest corner (min_z)      2  the farthest (max_z)
 *   3  the board's error case: infinitely far
 *
 * *Symptom that surfaced this in STF:* the Flying Carpet's pyramid shadows.
 * Model 580's sand is 262 triangles at mode 2 and the shadows painted on it are
 * 32 at mode 1, flat in the same y = -3.0 plane and carrying the checker bit.
 * Ignoring both instructions leaves the two at identical depth and the GPU's
 * tie-break decides per pixel. South Island's 507 is the same fault one step
 * worse: its plate is modelled 0.01 from the plane under it, which no depth
 * buffer resolves at that range, and it speckled.
 *
 * Only half of the rule is taken, and the half is the point (the explorer
 * reasons this out at length; its answer is reproduced here). A polygon may
 * take the sorted depth when that pushes it BACK, never when it pulls it
 * forward. Pushing back is what the artists used it for. Pulling forward is the
 * same instruction read the other way — a surface claiming every pixel it
 * covers at its nearest corner — which the board can afford because it has no
 * depth buffer to contradict and this renderer cannot. So the shader clamps the
 * substitute into [own z - GEO3D_ZSORT_RECEDE, own z]: a vertex may recede, by
 * its own polygon's depth and no further.
 *
 * And the bound alone is not enough, because what it bounds is still a sink: a
 * ground plane pushed back twelve units passes below anything modelled under it
 * within them. So whether to recede at all is decided by the polygon's own
 * depth — how far its near corner stands in front of its far one along the
 * view. Two faces lying flat against each other are shallow together, so the
 * backing one recedes in full and the decal keeps the pixel, which is the whole
 * of what the rule was for. A face raked along the view is deep, and stepping
 * it back sinks its near end through whatever stands under it, so it keeps the
 * depth the projection gave it.
 *
 * *Symptom that surfaced the bound in STF:* Aurora Icefield. Its ground is four
 * wedges hundreds of units deep at a grazing angle, and receding them in full
 * put the ice behind the walruses' reflection and the lower half of the cage,
 * both of which hang under it — half the rink went black.
 *
 * Faces that never went through the geometry decoder (the homebrew HUD, the
 * wireframe, the object viewer's free camera) carry NONE and keep the depth the
 * projection gives them.
 */
#define GEO3D_ZSORT_NONE   1.0e30f
static float g_geo3d_emit_zs        = GEO3D_ZSORT_NONE;
static int   g_geo3d_zsort          = 1;      /* 0: every face keeps its own depth */
static float g_geo3d_zsort_recede   = 12.0f;  /* how far back a vertex may be taken */

/* The sort z of one polygon, from the four corners it is sorted by, or NONE for
 * a polygon too deep to recede. Camera z runs negative into the screen, so the
 * board's min_z is the greatest of the four and its max_z the least. */
static inline float geo3d_sort_z(const vec3_t *sv, const int *zsrc, uint32_t zmode) {
    if (!g_geo3d_zsort) return GEO3D_ZSORT_NONE;
    if (zmode == 3u) return -1.0e10f;            /* the board's error case */
    float near_ = sv[zsrc[0]].z, far_ = near_;
    for (int i = 1; i < 4; i++) {
        float c = sv[zsrc[i]].z;
        if (c > near_) near_ = c;
        if (c < far_)  far_  = c;
    }
    if (near_ - far_ > g_geo3d_zsort_recede) return GEO3D_ZSORT_NONE;
    return zmode == 2u ? far_ : near_;
}

/* Models the game profile says stand on a floor (game_quirks_t.zsort_standing):
 * every face keeps the depth the projection gives it, as HUD faces do.
 *
 * *Symptom that surfaced this in STF (issue #78):* Aurora Icefield's ice
 * pillars (4278) and walrus statues (1601) had their bases cut off flat at the
 * ice, and the pillars' lower panels streaked with the far wall's texture.
 * Every face of both is sorted by its farthest corner and is only a few units
 * deep, so each receded in full, through the ice (1602), which is too deep to
 * recede and must not (see above). On the board 1602 sorts by a corner out at
 * its tip, so both solids win every pixel they cover; the depth buffer gives
 * the same answer for a closed solid over the ice. The explorer does the same
 * for the draws aurora_disp marks `standing` (noclip#7, ZSORT_KEEP). */
static const uint16_t *g_geo3d_standing;
static int             g_geo3d_standing_count;
static int             g_geo3d_zsort_standing = 1;   /* 0: standing models recede like the rest */
static inline bool geo3d_model_standing(int model_idx) {
    if (!g_geo3d_zsort_standing) return false;
    for (int i = 0; i < g_geo3d_standing_count; i++)
        if (g_geo3d_standing[i] == model_idx) return true;
    return false;
}

/* The bound, applied per vertex so the slope of a polygon lying along the view
 * survives: the vertex may recede to its polygon's sorted depth, no further
 * than the bound, and is never pulled forward. Done here rather than in the
 * shader so the bound stays an ordinary variable. */
static inline float geo3d_zs_vertex(float zb, float z) {
    if (zb > 1.0e29f) return GEO3D_ZSORT_NONE;
    float lo = z - g_geo3d_zsort_recede;
    return zb < lo ? lo : (zb > z ? z : zb);
}

/* Carry the z-sort state across one face of the index-array walk, the way
 * model.js does: a face naming a mode replaces both the mode and the corners,
 * and a face naming none keeps what stands. */
static inline void geo3d_zsort_step(uint32_t at, bool is_tri, bool has_C,
                                    int ai, int bi, int ci, int di,
                                    int *zsrc, uint32_t *zmode, bool *zset) {
    uint32_t zm = (at >> 10) & 3u;
    if (zm == 0u && *zset) return;
    if (zm != 0u) *zmode = zm;
    *zset = true;
    zsrc[0] = ai; zsrc[1] = bi;
    if (is_tri) { zsrc[2] = has_C ? ci : ai; zsrc[3] = zsrc[2]; }
    else        { zsrc[2] = di;              zsrc[3] = ci; }
}

/* Per-face fill flags out of the texture header — the same bits, in the same
 * places, as the explorer's face flags (vendor/noclip js/model.js), so
 * tools/grade-models.mjs compares them as they stand.
 *   TRANSPARENT  texheader[0] bit 13 on a textured face: the board's
 *                transparent renderer, where a texel of 15 is a hole
 *                (model2rd.ipp, the Translucent draw_scanline_tex). Cuts the
 *                palm fronds, billboard trees, clouds and ring ropes out.
 *   CHECKER      bit 15: drawn on every other screen pixel — the board's
 *                half transparency (South Island's water planes, waterfall).
 *   SHEET1       the tile's full-size level is on texram1; the mip chain
 *                alternates sheets from there.
 *   MIRROR_X/Y   bits 8 / 9: a coordinate that runs into an odd copy of the
 *                tile is inverted (fetch_bilinear_texel `u = ~u`) instead of
 *                repeating. */
#define GEO3D_FACE_TRANSPARENT 1u
#define GEO3D_FACE_CHECKER     2u
#define GEO3D_FACE_SHEET1      4u
#define GEO3D_FACE_MIRROR_X    8u
#define GEO3D_FACE_MIRROR_Y    16u

typedef struct {
    geo3d_tri_t tris[GEO3D_MAX_TRIS];
    int         count;
} geo3d_tri_buf_t;

static geo3d_tri_buf_t g_geo3d_tris = {0};

/* Where emitted triangles land.
 *
 * Normally the buffer above, which the renderer draws. A tool that wants to
 * decode a model without drawing it — the MCP bridge's model dump, which sweeps
 * every entry in the table — points this at a buffer of its own instead, so it
 * is not competing with the render thread for the one the frame is built in.
 * While it is redirected g_geo3d_dump_busy is set, and geo3d_build_wireframes
 * leaves the scene alone rather than resetting a buffer it no longer owns. */
static geo3d_tri_buf_t *g_geo3d_tri_sink   = &g_geo3d_tris;
static volatile int     g_geo3d_dump_busy  = 0;

/* Flat-colour override: ignore the model's ROM material (colour + texture) and
 * shade every face with the caller's flat colour. Used by the homebrew display
 * list, where geometry is instanced from a placeholder ROM quad (model 456) whose
 * material is meaningless — the real colour comes from the per-object colorbase.
 * Also gates the near-plane vertex reject in the emit helpers (the homebrew emits
 * camera-space geometry; STF does not). */
static int   g_geo_flat_color = 0;
/* Luma brightening for the homebrew flat path: approximates the GEO colorxlat ramp
 * that brightens lit faces above their dark base hue (live-tunable). */
static float g_geo_flat_boost = 2.0f;
/* Near-plane cull distance (camera-space units) for the homebrew flat-colour path.
 * Must stay below the closest legitimate geometry — the HUD lives icons sit at
 * HUD_LZ=5.0 (z=-5) — while still rejecting the near-camera streak stars that cross
 * z≈0. 4.0 keeps the lives with margin and kills the explosive crossers. */
#define GEO3D_NEAR_CULL 4.0f

static inline void geo3d_tris_reset(void) { g_geo3d_tri_sink->count = 0; }

/* Emit a textured triangle: per-vertex position + tile-relative texel UV, the
 * atlas tile rect (tx,ty,tw,th in pixels), plus a flat color.  The shader wraps
 * the interpolated UV within [0,tw)x[0,th) per-pixel before sampling the atlas;
 * tw<=0 means untextured (flat color). */
static inline void geo3d_emit_tri_uv(float x0, float y0, float z0, float u0, float v0,
                                      float x1, float y1, float z1, float u1, float v1,
                                      float x2, float y2, float z2, float u2, float v2,
                                      float r,  float g,  float b,
                                      float tx, float ty, float tw, float th,
                                      float lb, float pl, float fl) {
    if (g_geo3d_tri_sink->count >= GEO3D_MAX_TRIS) return;
    /* Homebrew (camera-space) near-plane reject: a face with any vertex closer than
     * GEO3D_NEAR_CULL blows up into a huge filled wedge across the screen.  Drop it —
     * legitimate scene geometry sits well in front (claw at z≈−9). Gated to the
     * flat-colour homebrew path so it can't clip close STF geometry. */
    if (g_geo_flat_color &&
        (z0 > -GEO3D_NEAR_CULL || z1 > -GEO3D_NEAR_CULL || z2 > -GEO3D_NEAR_CULL)) return;
    /* Degenerate close-quad reject: the homebrew's small HUD geo_obj_line elements
     * (lives icons at z≈-5) occasionally decode with a bad col0 that stretches one
     * thin-quad edge across the screen (a vertex collapses toward x≈0). A genuine
     * HUD claw spans well under a unit; reject a near-camera tri whose planar extent
     * is implausibly large (the streak) while keeping the small real glyphs. */
    if (g_geo_flat_color) {
        float zc = (z0 + z1 + z2) * (1.0f / 3.0f);
        if (zc > -8.0f) {                       /* HUD / near-camera region only */
            float mnx = fminf(x0, fminf(x1, x2)), mxx = fmaxf(x0, fmaxf(x1, x2));
            float mny = fminf(y0, fminf(y1, y2)), mxy = fmaxf(y0, fmaxf(y1, y2));
            if ((mxx - mnx) > 1.5f || (mxy - mny) > 1.5f) return;
        }
    }
    geo3d_tri_t *T = &g_geo3d_tri_sink->tris[g_geo3d_tri_sink->count++];
    T->x0=x0; T->y0=y0; T->z0=z0; T->u0=u0; T->v0=v0;
    T->x1=x1; T->y1=y1; T->z1=z1; T->u1=u1; T->v1=v1;
    T->x2=x2; T->y2=y2; T->z2=z2; T->u2=u2; T->v2=v2;
    T->r=r;   T->g=g;   T->b=b;
    T->tx=tx; T->ty=ty; T->tw=tw; T->th=th;
    T->lb=lb; T->pl=pl; T->fl=fl;
    T->texlod = g_geo3d_emit_texlod;
    T->zs0 = geo3d_zs_vertex(g_geo3d_emit_zs, z0);
    T->zs1 = geo3d_zs_vertex(g_geo3d_emit_zs, z1);
    T->zs2 = geo3d_zs_vertex(g_geo3d_emit_zs, z2);
}

/* Backward-compatible: untextured triangle (tw=0 → shader uses flat color). */
static inline void geo3d_emit_tri(float x0, float y0, float z0,
                                   float x1, float y1, float z1,
                                   float x2, float y2, float z2,
                                   float r,  float g,  float b) {
    geo3d_emit_tri_uv(x0,y0,z0,0.0f,0.0f, x1,y1,z1,0.0f,0.0f,
                      x2,y2,z2,0.0f,0.0f, r,g,b, 0.0f,0.0f,0.0f,0.0f, 0.0f,1.0f, 0.0f);
}

static inline void geo3d_emit_line(float x0, float y0, float z0,
                                    float x1, float y1, float z1,
                                    float r,  float g,  float b) {
    if (g_geo3d_lines.count >= GEO3D_MAX_LINES) return;
    /* Near-plane reject (homebrew camera-space geometry): the host camera looks down
     * −z, so any endpoint closer than GEO3D_NEAR_CULL makes the perspective divide
     * blow up into a streak/wedge from a screen edge.  The homebrew's nearest real
     * geometry is the claw at z≈−9 (Z_NEAR_W 10 − CLAW_FWD), so −6 culls only the
     * near-camera stragglers (stars/effects passing the eye). Gated to the flat-colour
     * homebrew path so it can't clip close STF geometry. */
    if (g_geo_flat_color && (z0 > -GEO3D_NEAR_CULL || z1 > -GEO3D_NEAR_CULL)) return;
    geo3d_line_t *L = &g_geo3d_lines.lines[g_geo3d_lines.count++];
    L->x0 = x0; L->y0 = y0; L->z0 = z0;
    L->x1 = x1; L->y1 = y1; L->z1 = z1;
    L->r  = r;  L->g  = g;  L->b  = b;
}

/* ---- 3D state ------------------------------------------------------------ */

typedef struct {
    captured_model_t captured[MAX_GEO_MODELS];
    int              captured_count;

    /* Previous frame's capture list — used for sub-frame position interpolation. */
    captured_model_t captured_prev[MAX_GEO_MODELS];
    int              captured_prev_count;
    /* g_cop.geo_frame_end value at the last scan — used to detect a new (vblank)
     * frame for the captured_prev interpolation snapshot. */
    int              last_frame_end;

    /* Camera + projection (used by the GPU pass in game_render.h) */
    float            cam_x, cam_y, cam_z;
    float            rot_x, rot_y;
    float            fov_deg;

    /* Manual single-model browser (when use_captures = false) */
    int              model_index;

    /* Render toggles */
    bool             enabled;         /* draw 3D wireframes at all */
    bool             use_captures;    /* draw the live capture list */
    bool             use_matrix;      /* apply captured per-object transform */
    bool             use_game_view;   /* read view matrix from game RAM */
    bool             test_triangle;   /* draw a hard-coded triangle (sanity) */
    bool             lines_only;      /* skip filled tris (homebrew wireframe; no overdraw) */

    /* Capture filter */
    bool             filter_enabled;
    int              filter_min;
    int              filter_max;
    int              isolate_index;   /* >=0: show only this single capture */

    /* Polygon connectivity (cached from the active profile each scan). */
    uint32_t         connect_when;
    bool             simple_connect;

    /* View matrix read from game RAM, if has_game_view. */
    float            game_view[12];
    bool             has_game_view;

    /* Windows in the display list last walked (geo3d_scan_geo_list). */
    int              geo_windows;
} geo3d_state_t;

static inline void geo3d_init(geo3d_state_t *geo) {
    memset(geo, 0, sizeof(*geo));
    geo->cam_z          = 5.0f;
    geo->fov_deg        = 60.0f;
    geo->model_index    = 3;
    geo->enabled        = true;
    geo->use_captures   = true;
    geo->use_matrix     = true;
    geo->use_game_view  = true;
    geo->isolate_index  = -1;
    geo->connect_when   = 0xFFFF;
    geo->simple_connect = false;
}

/* Global pointer set by main.c so mcp_bridge.h can read captured models. */
static geo3d_state_t *g_geo3d_state = NULL;

/* ---- Model-table lookup -------------------------------------------------- */

/* Maps a polygon-ROM pointer (as it appears in the COP capture stream) to
 * the model-table index that references it.  Built lazily on first scan;
 * call geo3d_lookup_invalidate() if main_data is swapped (new ROM load). */
typedef struct {
    uint32_t pol_ptrs[MODEL_LOOKUP_SIZE];
    int      model_idx[MODEL_LOOKUP_SIZE];
    int      count;
    bool     built;
} geo3d_lookup_t;

static geo3d_lookup_t g_geo3d_lookup = {0};

/* Ground-plane Y for character shadows (0x3A007474).  Tunable at runtime via the
 * MCP bridge ("set_shadow_floor") so the floor height can be dialed in visually
 * until the proper stage-floor source is wired. */
static float g_geo_shadow_floor_y = 0.0f;


/* Game-camera convention signs (dial live via the 3D window for the attract
 * "camera moving in wrong directions" bug). Multipliers applied in
 * geo3d_read_game_view to the camera struct's eye + angles. Defaults reproduce
 * the historical convention EXACTLY (z negated, yaw negated) so nothing changes
 * until toggled — once the right combo is found on-screen, bake it here. */
static float g_cam_sign_x  =  1.0f;
static float g_cam_sign_y  =  1.0f;
static float g_cam_sign_z  = -1.0f;  /* cam_z = -zpos */
static float g_cam_sign_rx =  1.0f;  /* pitch = +xang */
static float g_cam_sign_ry =  1.0f;  /* yaw   = +yang (neg yaw off by default) */

/* Debug: dump the RAW camera struct (eye + angle word) once per game frame to
 * cam_ours.csv, keyed by the STF frame counter (0x500020). Attract is
 * deterministic from boot, so this aligns frame-for-frame with a MAME capture
 * → compare to localise wrong-camera-direction (values vs our render convention). */
static int g_cam_log = 0;

/* Rotation-only view: apply the camera ROTATION but NOT its (−eye) translation.
 * Some scenes (e.g. the STF intro carnival flythrough) bake the eye-translation
 * into the captured COP geometry (it = world−eye already), so the normal
 * R·(p−eye) view subtracts the eye a second time → double-translation. With this
 * set, the view is R·p, correct for already-eye-baked geometry. */
static int g_cam_rot_only = 0;

/* cam_mode (camera struct +0x28; STF 0x519EC0 = g13+0x40). The fight's camera_init
 * sets cam_mode=9 (look-at, world-space geometry); ADV_MOVIE attract scenes set
 * cam_mode=0 (scripted, candidate eye-baked). Auto-select rotation-only when
 * cam_mode != 9 to test cam_mode as the per-scene baking discriminator. */
static int g_cam_mode_value = -1;   /* last read; -1 = unknown */
static int g_cam_auto_rot   = 0;    /* if set: g_cam_rot_only := (cam_mode != 9) */

/* Raw camera eye (0x519E98 xpos,ypos,zpos) for the eye-bake auto-detector. */
static float g_cam_eye_raw[3] = {0.0f, 0.0f, 0.0f};
/* Auto eye-baked detection: a scene that emits a base SETPOS ≈ (−eye.x, −eye.y,
 * eye.z) bakes the camera translation into its geometry (STF intro carnival),
 * so that frame must render rotation-only.  When set, the scanner detects this
 * per frame and drives g_cam_rot_only.  Confirmed from the COP stream:
 * SETPOS [-17.4,-30.6,-7.5] ≈ −eye precedes the carnival objects.
 * Default ON: improves the carnival/eye-baked attract scenes; toggle off to debug. */
static int g_cam_auto_baked = 1;

/* Clip-window ("set_window") support. ON: GEO_WIN_SENTINEL sets per-model clip
 * rects + the slow per-window render path (scissor + game camera per window).
 * OFF: ignore windows entirely → every model renders full-screen through the
 * single game camera (fast path). Debug A/B for what the windows are doing. */
static int g_geo_windows_enabled = 1;

/* Texture UV orientation debug dials (geo3d window). The board needs none of
 * them: with the stream order below the raw coordinates are already right. */
static bool g_uv_swap   = false;
static bool g_uv_flip_u = false;
static bool g_uv_flip_v = false;
/* UV stream → corner order. The stream walks each face's loop the opposite way
 * from the index array, because negating Z on read reverses the winding: a
 * quad's corners come out A,B,D,C and its stream runs B,A,C,D (slots
 * {1,0,2,3}); a triangle's runs B,A,C ({1,0,2}).
 *
 * The strips settle it without a reference image. A vertex shared by two faces
 * of one strip carries one UV in ROM, so the right order is the one that agrees
 * with itself across shared corners: over every STF model, 96.5% for this order
 * against 75.5% for the previous A,B,D,C-with-both-flips, which read forwards and
 * mirrored every face whose texture axis runs along it. The explorer found and
 * documents the same thing (vendor/noclip TECHNICAL.md, "the UV stream runs
 * against the reconstructed winding") and tools/grade-models.mjs holds this
 * decoder to its coordinates corner for corner.
 *
 * 1 = the previous A,B,D,C / A,B,C reading, kept as a debug A/B. */
static int  g_uv_quad_order = 0;

/* Flat shading ("definition"): MAME shades each polygon by luminance =
 * |normal·light|*diffuse + ambient (model2_v.cpp geo_parse), which modulates
 * the texel luma.  We don't compute the game's lighting, so models read flat.
 * Approximate it: per-face geometric normal (cross of transformed edges) · a
 * tunable light dir, modulating the face color.  Tunable live in the 3D window. */
static int   g_light_enable  = 1;
/* MAME per-pixel colorxlat luma ramp (lumaram + colorxlat LUTs in the shader).
 * Default ON; off falls back to flat_color×luma. */
static int   g_luma_ramp     = 1;
/* 3D wireframe overlay (the line pass over the solid fills). Default OFF —
 * the lines clutter the shaded fills. */
static int   g_geo_wireframe = 0;
/* Backface culling: 0 = none, 1 = cull back (CW front), 2 = cull back (CCW front).
 * MAME backface-tests via normal·viewdir; winding cull is simpler — flip 1/2 to
 * match the model winding (whichever shows outer shells, not interiors). */
static int   g_backface_cull = 0;
static float g_light_dir[3]  = {0.3f, 0.5f, 1.0f};
static float g_light_ambient = 0.45f;
static float g_light_diffuse = 0.55f;
/* Debug: when == a model index, dump that model's per-face texture tiles
 * (sheet,texx,texy,texw,texh) to model_tex.txt while it is decoded. -1 = off.
 * Test it with GEO3D_DUMP_TEX(), never with a bare ==: a polygon-RAM object
 * decodes as model -1 too, so "off" matched every one of them. On the desktop
 * that quietly rewrote model_tex.txt per face; on the web, with no file
 * system, the stub fopen answered fd 0 and the write threw out of the frame
 * (the Death Egg screens in attract, adv_movie_egg). */
static int  g_dump_model_tex = -1;
/* The debug dumps (this one, the camera CSV, the texture extractor, the COP
 * stream) exist only in the desktop build, the one with a UI and a bridge to
 * ask for them (CMake defines M2HLE_DEBUG_DUMPS on that target alone). The
 * handheld, libretro and web builds write no debug files at all: before
 * 74eab22 this one fired for every polygon-RAM object, and a ROCKNIX core
 * rewrote model_tex.txt on the SD card for every face of them, every frame --
 * STF's win screen crawled on it. Compiled out, a bug like that cannot write. */
#ifdef M2HLE_DEBUG_DUMPS
#define GEO3D_DUMP_TEX(model_idx) (g_dump_model_tex >= 0 && (model_idx) == g_dump_model_tex)
#else
#define GEO3D_DUMP_TEX(model_idx) ((void)(model_idx), 0)
#endif
/* Texture bank override: 0=auto (texsheet bit12), 1=force sheet0, 2=force sheet1,
 * 3=swap (invert the bit12 selection). */
static int  g_uv_bank_mode = 0;

/* Fight stage camera-relative fix: the fight stage geometry comes through the
 * FIFO already camera-relative (world − camera) while bone fighters are world.
 * When a fighter (bone model) is on screen, add the camera back to non-bone
 * matrices so the uniform game camera doesn't subtract it twice. */
static bool g_stage_camera_fix = true;

/* Texture-path debug counters (cumulative across decode calls).  Read via the
 * MCP bridge "dump_tex_stats" to see whether faces actually come out textured. */
static long g_dbg_tex_models = 0, g_dbg_tex_models_uv = 0, g_dbg_tex_models_mat = 0;
static long g_dbg_tex_faces  = 0, g_dbg_tex_textured  = 0, g_dbg_tex_uv_faces  = 0;
/* First N textured faces' UVs, for offline cross-check against the atlas dump. */
typedef struct { int model; uint16_t texx, texy, texsheet, tri;
                 float au[4], av[4]; uint16_t texw, texh, pu0, pv0; } dbg_face_uv_t;
static dbg_face_uv_t g_dbg_face_uv[24];
static int g_dbg_face_uv_n = 0;

static inline void geo3d_lookup_invalidate(void) {
    g_geo3d_lookup.built = false;
    g_geo3d_lookup.count = 0;
}

static inline void geo3d_lookup_build(const uint8_t *main_data, size_t main_data_size,
                                       uint32_t table_off, uint32_t table_count) {
    if (g_geo3d_lookup.built) return;
    g_geo3d_lookup.count = 0;
    if (!main_data) return;
    if ((size_t)table_off + (size_t)table_count * MODEL_ENTRY_SIZE > main_data_size) return;

    for (uint32_t m = 0; m < table_count && g_geo3d_lookup.count < MODEL_LOOKUP_SIZE; m++) {
        uint32_t toff = table_off + m * MODEL_ENTRY_SIZE;
        uint32_t pol  = read_u32_le(main_data + toff + 8);
        if (pol != 0) {
            g_geo3d_lookup.pol_ptrs[g_geo3d_lookup.count]  = pol;
            g_geo3d_lookup.model_idx[g_geo3d_lookup.count] = (int)m;
            g_geo3d_lookup.count++;
        }
    }
    g_geo3d_lookup.built = true;
    LOG_INFO("geo3d_lookup_build: %d entries (table_off=0x%X count=%u)",
             g_geo3d_lookup.count, table_off, table_count);
}

static inline int geo3d_lookup_by_pol(uint32_t pol_ptr) {
    for (int i = 0; i < g_geo3d_lookup.count; i++) {
        if (g_geo3d_lookup.pol_ptrs[i] == pol_ptr) return g_geo3d_lookup.model_idx[i];
    }
    return -1;
}

/* ---- Capture-stream scanner --------------------------------------------- */

/*
 * Walk the COP capture ring (g_cop.geo_capture) and extract draw calls.
 * Per-object state carried forward across the loop:
 *   current_pos[3]   — last position command  (0x03000606)
 *   current_ang[3]   — last Euler angle setters (0x04000808/04800909/05000A0A)
 *   current_mat[12]  — last explicit matrix     (0x02000404)
 *   have_pos / have_ang / have_mat — reset after each captured object
 *
 * Inputs:
 *   main_data, polygons   — already-loaded ROM regions
 *   table_off, table_count — model table location (from profile quirks)
 *   mesh_ptr_subtract/add  — mesh pointer encoding (from profile quirks)
 *
 * Output: geo->captured[] populated, geo->captured_count set.
 */
static inline void geo3d_scan_captures(geo3d_state_t *geo,
                                        const uint8_t *main_data, size_t main_data_size,
                                        size_t polygons_size,
                                        uint32_t table_off, uint32_t table_count,
                                        uint32_t mesh_ptr_subtract, uint32_t mesh_ptr_add) {
    if (!main_data) { geo->captured_count = 0; return; }

    geo3d_lookup_build(main_data, main_data_size, table_off, table_count);

    /* Use the per-frame ring range recorded by the frame-pace hook.
     * This limits the scan to exactly one game frame's draw commands,
     * eliminating ghosting from stale entries in the circular ring.
     * Fall back to the full ring if the frame hook hasn't fired yet. */
    int frame_end   = g_cop.geo_frame_end;
    int frame_start = g_cop.geo_frame_start;
    int total = frame_end - frame_start;
    if (total <= 0 || total > GEO_CAPTURE_SIZE) {
        /* Hook hasn't fired yet (first frame), OR the board_vblank marking raced
         * with this UI-thread read and produced an empty/inverted window. Scan a
         * BOUNDED recent slice — never the whole ring. The full ring can hold
         * thousands of accumulated object draws, and each 0x3C007878 costs an
         * O(model_table_count) lookup; scanning all of them would freeze the
         * render for seconds (the m2snake "hang"). One game frame is far smaller
         * than this bound, so a real first frame is unaffected; a raced frame
         * just scans a little extra and self-corrects next frame. */
        total = g_cop.geo_capture_count;
        if (total > GEO3D_SCAN_FALLBACK_MAX) total = GEO3D_SCAN_FALLBACK_MAX;
        frame_end = g_cop.geo_capture_head;
    }
    int head = frame_end;

    /* When a new game frame arrives — detected via vblank (g_cop.geo_frame_end
     * advances once per vsync, set by the frame-pace hook) rather than the game's
     * RAM frame counter — snapshot the current captured list into captured_prev
     * for interpolation. */
    if (g_cop.geo_frame_end != geo->last_frame_end) {
        memcpy(geo->captured_prev, geo->captured,
               (size_t)geo->captured_count * sizeof(captured_model_t));
        geo->captured_prev_count = geo->captured_count;
        geo->last_frame_end      = g_cop.geo_frame_end;
    }

    geo->captured_count = 0;

    float current_pos[3]  = {0.0f, 0.0f, 0.0f};
    float current_mat[12] = {0.0f};
    bool  have_pos = false;
    bool  have_mat = false;
    /* have_ang: set by any ang command, saved/restored with push/pop, NOT reset
     * after a draw.  Allows models drawn inside a push block with only ang
     * commands (no set_pos) to get the correct rotated matrix — e.g. model 3351
     * (moustache) drawn after ang_x inside PUSH B where have_pos was already
     * consumed by the preceding model 3545 draw.  scan_rot's push/pop scoping
     * prevents stale angle state from earlier capture-ring entries from leaking. */
    bool  have_ang = false;

    /* Local rotation matrix: mirrors g_sharc.rot convention (column-major [col][row]).
     * Updated by push/pop/identity/ang commands so the have_pos/have_ang matrix
     * path captures the full transform, not just identity. */
    float scan_rot[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
    float scan_rot_stk[16][3][3];
    float scan_T_stk[16][3];   /* saved current_pos per push level */
    bool  scan_hp_stk[16];     /* saved have_pos per push level */
    bool  scan_ha_stk[16];     /* saved have_ang per push level */
    int   scan_bs_stk[16];     /* saved scan_active_bslot per push level */
    int   scan_stk_top = 0;

    /* Active clip window — set/cleared by the set_window event cursor (below) as
     * the scan passes each event's stream position; stays active across draws
     * until the next event. x/y/w/h in game pixels (0–495, 0–383); full-screen
     * windows clear it (no clip). */
    bool   have_clip_win      = false;
    int16_t clip_win_x = 0, clip_win_y = 0, clip_win_w = 0, clip_win_h = 0;

    /* Bone transform state (0x35806B6B + 0x1B803737).
     * cop.h's 0x35806B6B handler writes the IK result into g_cop.tgp_bone[].
     * 0x1B803737 selects which slot (pid*16+bone_idx) for the next mesh.
     * Indexed: P1 bone N → tgp_bone[N], P2 bone N → tgp_bone[16+N]. */
    int     scan_active_bslot = -1;

    /* Eye-bake auto-detect: a base SETPOS ≈ (−eye.x, −eye.y, eye.z) means this
     * frame's geometry is already world−eye (carnival intro) → render rot-only. */
    int   frame_eye_baked = 0;
    /* The eye-bake base SETPOS ≈ −raw_eye on ALL axes (raw zpos is positive;
     * the camera readout shows −z only because cam_z = −zpos). */
    float eb_x = -g_cam_eye_raw[0], eb_y = -g_cam_eye_raw[1], eb_z = -g_cam_eye_raw[2];
    bool  eb_valid = (fabsf(g_cam_eye_raw[0]) + fabsf(g_cam_eye_raw[1])
                      + fabsf(g_cam_eye_raw[2])) > 5.0f;

    /* Clip-window cursor: replay the discrete set_window events (cop.h
     * g_win_events[]) in stream order alongside the draws. Each event is tagged
     * with the geo_capture_head at which it took effect; this frame spans
     * [head-total, head). Advance the cursor to the oldest event still in range. */
    int win_ev = g_win_event_head - GEO_WIN_EVENTS_MAX; if (win_ev < 0) win_ev = 0;
    {   int win_frame_start = head - total;
        while (win_ev < g_win_event_head &&
               g_win_events[win_ev & (GEO_WIN_EVENTS_MAX-1)].head_pos < win_frame_start) win_ev++;
    }

    for (int i = 0; i < total && geo->captured_count < MAX_GEO_MODELS; i++) {
        int idx = (head - total + i + GEO_CAPTURE_SIZE) & (GEO_CAPTURE_SIZE - 1);
        uint32_t val = g_cop.geo_capture[idx];

        /* Apply any set_window events that take effect at or before this stream
         * position. Decode the two corner words (X = high16, Y = 511 - low16,
         * verified against captured words); a full-screen window CLEARS the clip
         * (so fights, which only set full-screen, stay on the fast path). */
        if (g_geo_windows_enabled) {
            int abs_pos = head - total + i;
            while (win_ev < g_win_event_head &&
                   g_win_events[win_ev & (GEO_WIN_EVENTS_MAX-1)].head_pos <= abs_pos) {
                const geo_win_event_t *we = &g_win_events[win_ev & (GEO_WIN_EVENTS_MAX-1)];
                int xa=(int)(int16_t)(we->w0>>16), ya=511-(int)(int16_t)(we->w0 & 0xFFFF);
                int xb=(int)(int16_t)(we->w1>>16), yb=511-(int)(int16_t)(we->w1 & 0xFFFF);
                int wl=xa<xb?xa:xb, wr=xa>xb?xa:xb, wt=ya<yb?ya:yb, wb=ya>yb?ya:yb;
                if (wl<=0 && wt<=0 && wr>=495 && wb>=383) {
                    have_clip_win = false;                  /* full-screen → no clip */
                } else if (wr>wl && wb>wt) {
                    have_clip_win = true;
                    clip_win_x=(int16_t)wl; clip_win_y=(int16_t)wt;
                    clip_win_w=(int16_t)(wr-wl); clip_win_h=(int16_t)(wb-wt);
                }
                win_ev++;
            }
        }

        /* --- Matrix stack: push / identity / pop ----------------------------- */
        if (val == 0x00800101) {
            if (scan_stk_top < 16) {
                memcpy(scan_rot_stk[scan_stk_top], scan_rot,    sizeof(scan_rot));
                memcpy(scan_T_stk[scan_stk_top],   current_pos, sizeof(current_pos));
                scan_hp_stk[scan_stk_top] = have_pos;
                scan_ha_stk[scan_stk_top] = have_ang;
                scan_bs_stk[scan_stk_top] = scan_active_bslot;
                scan_stk_top++;
            }
            have_ang          = false;
            scan_active_bslot = -1;
            continue;
        }
        if (val == 0x01800303) {
            /* SHARC identity reset: rot → I (scale baked into rot is also reset), T → (0,0,0). */
            memset(scan_rot, 0, sizeof(scan_rot));
            scan_rot[0][0] = scan_rot[1][1] = scan_rot[2][2] = 1.0f;
            current_pos[0] = current_pos[1] = current_pos[2] = 0.0f;
            have_pos = false;
            have_ang = false;
            continue;
        }
        if (val == 0x01000202) {
            if (scan_stk_top > 0) {
                --scan_stk_top;
                memcpy(scan_rot,    scan_rot_stk[scan_stk_top], sizeof(scan_rot));
                memcpy(current_pos, scan_T_stk[scan_stk_top],   sizeof(current_pos));
                have_pos          = scan_hp_stk[scan_stk_top];
                have_ang          = scan_ha_stk[scan_stk_top];
                scan_active_bslot = scan_bs_stk[scan_stk_top];
            }
            continue;
        }

        /* --- Angle setters: update scan_rot with the same post-multiply as
         *     sharc_postmul_ry/rz/rx in sharc.h.  The arg is the raw 32-bit
         *     FIFO word (int32_t fixed-point, 0x10000 = 360°). -------------- */
        if ((val == 0x04000808 || val == 0x04800909 || val == 0x05000A0A) && i + 1 < total) {
            if (scan_stk_top == 0) { i += 1; continue; }
            uint32_t aw = g_cop.geo_capture[(idx + 1) & (GEO_CAPTURE_SIZE - 1)];
            /* Only the low 16 bits are meaningful (0x10000 = 360°); mask before
             * converting so large accumulating angles (e.g. the attract portrait
             * spin am_cntr*0xFFF0) don't blow up cosf/sinf precision. Matches the
             * live SHARC sharc_angle_to_rad(). */
            float a = ((float)(int16_t)(aw & 0xFFFF) / 65536.0f) * (2.0f * 3.14159265f);
            float c = cosf(a), s = sinf(a);
            float (*r)[3] = scan_rot;
            int _ri;
            if (val == 0x04800909) {          /* ang_y: col0,col2 */
                for (_ri = 0; _ri < 3; _ri++) {
                    float c0 =  c*r[0][_ri] + s*r[2][_ri];
                    float c2 = -s*r[0][_ri] + c*r[2][_ri];
                    r[0][_ri] = c0; r[2][_ri] = c2;
                }
            } else if (val == 0x04000808) {   /* ang_x: col1,col2 */
                for (_ri = 0; _ri < 3; _ri++) {
                    float c1 = c*r[1][_ri] - s*r[2][_ri];
                    float c2 = s*r[1][_ri] + c*r[2][_ri];
                    r[1][_ri] = c1; r[2][_ri] = c2;
                }
            } else {                           /* ang_z (0x05000A0A): col0,col1 */
                for (_ri = 0; _ri < 3; _ri++) {
                    float c0 = c*r[0][_ri] - s*r[1][_ri];
                    float c1 = s*r[0][_ri] + c*r[1][_ri];
                    r[0][_ri] = c0; r[1][_ri] = c1;
                }
            }
            have_ang = true;
            i++; continue;
        }

        /* --- 0x35806B6B: bone IK (17 args) — cop.h writes result to tgp_bone --- */
        if (val == 0x35806B6B && i + 17 < total) {
            i += 17; continue;
        }

        /* --- 0x1B003636: plain bone-cache load → current matrix (2 args) ----
         * SHARC copies rot_cache[player*16+slot] into rot[] and pos[].
         * Mirror that here so subsequent ang/set_pos commands stack correctly. */
        if (val == 0x1B003636 && i + 2 < total) {
            uint32_t a0 = g_cop.geo_capture[(idx+1) & (GEO_CAPTURE_SIZE-1)];
            uint32_t a1 = g_cop.geo_capture[(idx+2) & (GEO_CAPTURE_SIZE-1)];
            int player   = ((a0 & 0xFF) == 1) ? 1 : 0;
            int slot_idx = (int)a1 / 12;
            if ((unsigned)slot_idx < 16u) {
                const float *B = g_sharc.rot_cache[player * 16 + slot_idx];
                for (int _c = 0; _c < 3; _c++)
                    for (int _r = 0; _r < 3; _r++)
                        scan_rot[_c][_r] = B[_c * 3 + _r];
                current_pos[0] = B[9];
                current_pos[1] = B[10];
                current_pos[2] = B[11];
                have_pos = true;
            }
            i += 2; continue;
        }

        /* --- 0x1B803737: bone slot selector (3 words: cmd + pid + bone_idx*0xC) ---
         * arg0 = player_id (0=P1, 1=P2)
         * arg1 = bone_idx * 0xC  → bone_idx = arg1/0xC
         * tgp_bone index = player_id*16 + bone_idx */
        if (val == 0x1B803737 && i + 2 < total) {
            uint32_t pid_w = g_cop.geo_capture[(idx+1) & (GEO_CAPTURE_SIZE-1)];
            uint32_t boff  = g_cop.geo_capture[(idx+2) & (GEO_CAPTURE_SIZE-1)];
            int pid      = (int)(pid_w & 1);
            int bone_idx = (int)((boff & 0xFF) / 0xC);
            int tgp_idx  = pid * 16 + bone_idx;
            scan_active_bslot = (tgp_idx >= 0 && tgp_idx < 32) ? tgp_idx : -1;
            i += 2; continue;
        }

        /* Clip windows are no longer spliced into this command stream as a
         * synthetic sentinel. set_window now records discrete events (cop.h
         * g_win_events[]) tagged with the stream position; the cursor at the top
         * of this loop activates/clears have_clip_win as the scan passes them. */

        /* --- 0x02000404 / 0x05800B0B: matrix (12 floats follow) ---
         * 0x02000404: written directly by the i960 with a freshly-built transform.
         * 0x05800B0B: written by the i960 after reading the current matrix via
         *   0x02800505, applying a derived bone transform (e.g. neck attachment),
         *   and sending it back.  Both carry the same row-major display-format matrix
         *   (Z-negation already applied, m[2][3] = +pos[2] before have_mat negates it). */
        if ((val == 0x02000404 || val == 0x05800B0B) && i + 12 < total) {
            bool  all_sane = true;
            float m[12];
            for (int j = 0; j < 12; j++) {
                uint32_t fv = g_cop.geo_capture[(idx + 1 + j) & (GEO_CAPTURE_SIZE - 1)];
                if (!is_sane_float(fv) && fv != 0) { all_sane = false; break; }
                m[j] = u32_as_float(fv);
            }
            if (all_sane) {
                memcpy(current_mat, m, sizeof(m));
                have_mat = true;
                have_ang  = false;
                scan_active_bslot = -1;
                if (!have_pos) {
                    current_pos[0] = m[3];
                    current_pos[1] = m[7];
                    current_pos[2] = m[11];
                    have_pos = true;
                }
            }
            i += 12; continue;
        }

        /* --- 0x03000606: position (3 floats follow) --- */
        if (val == 0x03000606 && i + 3 < total) {
            bool  all_sane = true;
            float p[3];
            for (int j = 0; j < 3; j++) {
                uint32_t fv = g_cop.geo_capture[(idx + 1 + j) & (GEO_CAPTURE_SIZE - 1)];
                if (!is_sane_float(fv)) { all_sane = false; break; }
                p[j] = u32_as_float(fv);
            }
            if (all_sane) {
                /* Eye-bake marker: a SETPOS whose raw args ≈ (−eye.x,−eye.y,eye.z)
                 * is the base translation that makes subsequent geometry world−eye. */
                if (eb_valid && fabsf(p[0]-eb_x) < 3.0f && fabsf(p[1]-eb_y) < 3.0f
                             && fabsf(p[2]-eb_z) < 3.0f)
                    frame_eye_baked = 1;
                /* SHARC: T += rot × args (additive, rot-relative offset → world).
                 * scan_rot is column-major [col][row]; result[r] = Σ_c rot[c][r]*p[c]. */
                float (*r)[3] = scan_rot;
                current_pos[0] += r[0][0]*p[0] + r[1][0]*p[1] + r[2][0]*p[2];
                current_pos[1] += r[0][1]*p[0] + r[1][1]*p[1] + r[2][1]*p[2];
                current_pos[2] += r[0][2]*p[0] + r[1][2]*p[1] + r[2][2]*p[2];
                have_pos = true;
            }
            i += 3; continue;
        }

        /* --- 0x3A007474: shadow matrix setup (7 args) ---
         * Called from rob_kage_disp_test immediately before the shadow model draw.
         * Args: [pid, bone*0xC, flag, off.x, off.y, off.z, g5].  The firmware (PM
         * 0x20D0A) loads the anchor bone (_L204D6) then multiplies by a SHARC
         * shadow-projection matrix (PM 0x21F20, built by make_kage_matrix) that
         * projects the bone onto the stage GROUND PLANE.  We don't yet capture that
         * projection's ground-plane translation (g_sharc.shadow_rot is only the 3×3
         * flatten), so for now we use the bone's X/Z (so the shadow tracks the
         * character) and pin Y to the floor.  TODO: capture the full projection /
         * stage floor height (word_5019AC) so the shadow sits exactly on the ground. */
        if (val == 0x3A007474 && i + 7 < total) {
            uint32_t pid_w = g_cop.geo_capture[(idx + 1) & (GEO_CAPTURE_SIZE-1)];
            uint32_t boff  = g_cop.geo_capture[(idx + 2) & (GEO_CAPTURE_SIZE-1)];
            uint32_t offy_w= g_cop.geo_capture[(idx + 5) & (GEO_CAPTURE_SIZE-1)]; /* off.y */
            int pid  = (int)(pid_w & 1);
            int bidx = (int)((boff & 0xFF) / 0xC);
            int slot = pid * 16 + bidx;
            if (slot >= 0 && slot < 32) {
                const float *tb = g_sharc.tgp_bone[slot];
                float rsum = 0.0f;
                for (int _k = 0; _k < 9; _k++) rsum += tb[_k] * tb[_k];
                if (rsum > 1e-6f) {            /* bone is populated → use its X/Z */
                    current_pos[0] = tb[9];
                    current_pos[2] = tb[11];
                    have_pos = true;
                }
            }
            for (int _c = 0; _c < 3; _c++)
                for (int _r = 0; _r < 3; _r++)
                    scan_rot[_c][_r] = g_sharc.shadow_rot[_c][_r];
            /* off.y is 0 (game-frame floor); our render places fighters at negative Y,
             * so the shadow floor must be the character's FEET in our frame = the
             * extreme bone Y. Find the populated-bone Y range for this player. */
            (void)offy_w;
            float _ymin = 1e9f;
            for (int _bi = 0; _bi < 16; _bi++) {
                const float *bb = g_sharc.tgp_bone[pid * 16 + _bi];
                float _brs = 0.0f; for (int _k = 0; _k < 9; _k++) _brs += bb[_k]*bb[_k];
                if (_brs > 1e-6f && bb[10] < _ymin) _ymin = bb[10];
            }
            /* The arena floor = the lowest the feet ever reach (standing); jumps only
             * raise the feet ABOVE it. So track a running min of the feet Y → a fixed
             * ground plane the shadow stays on when a fighter jumps. Slow upward drift
             * re-levels on stage/match changes. */
            static float s_floor_y = 1e9f;
            if (_ymin < 1e8f) {
                if (_ymin < s_floor_y) s_floor_y = _ymin;                /* grab the floor */
                else                   s_floor_y += (_ymin - s_floor_y) * 0.003f; /* re-level slowly */
                current_pos[1] = s_floor_y;
            } else {
                current_pos[1] = g_geo_shadow_floor_y;
            }
            have_ang = true;
            have_pos = true;
            i += 7; continue;
        }

        /* --- 0x1F803F3F: set_ang_xyz — args[0]=ang_z, args[1]=ang_y, args[2]=ang_x --- */
        if (val == 0x1F803F3F && i + 3 < total) {
            if (scan_stk_top == 0) { i += 3; continue; }
            /* Mirrors sharc_exec.h 0x1F803F3F: apply z, then y, then x post-multiply. */
            float (*r)[3] = scan_rot;
            int _ai, _ri;
            for (_ai = 0; _ai < 3; _ai++) {
                uint32_t aw = g_cop.geo_capture[(idx + 1 + _ai) & (GEO_CAPTURE_SIZE-1)];
                /* Only the low 16 bits are meaningful (0x10000 = 360°); mask before
             * converting so large accumulating angles (e.g. the attract portrait
             * spin am_cntr*0xFFF0) don't blow up cosf/sinf precision. Matches the
             * live SHARC sharc_angle_to_rad(). */
            float a = ((float)(int16_t)(aw & 0xFFFF) / 65536.0f) * (2.0f * 3.14159265f);
                float c = cosf(a), s = sinf(a);
                if (_ai == 0) {        /* arg[0] = ang_z: col0, col1 */
                    for (_ri = 0; _ri < 3; _ri++) {
                        float c0 = c*r[0][_ri] - s*r[1][_ri];
                        float c1 = s*r[0][_ri] + c*r[1][_ri];
                        r[0][_ri] = c0; r[1][_ri] = c1;
                    }
                } else if (_ai == 1) { /* arg[1] = ang_y: col0, col2 */
                    for (_ri = 0; _ri < 3; _ri++) {
                        float c0 =  c*r[0][_ri] + s*r[2][_ri];
                        float c2 = -s*r[0][_ri] + c*r[2][_ri];
                        r[0][_ri] = c0; r[2][_ri] = c2;
                    }
                } else {               /* arg[2] = ang_x: col1, col2 */
                    for (_ri = 0; _ri < 3; _ri++) {
                        float c1 = c*r[1][_ri] - s*r[2][_ri];
                        float c2 = s*r[1][_ri] + c*r[2][_ri];
                        r[1][_ri] = c1; r[2][_ri] = c2;
                    }
                }
            }
            have_ang = true;
            i += 3; continue;
        }

        /* --- 0x3800707: set_scale (3 float args: sx, sy, sz) ---
         * The SHARC firmware multiplies scale directly into the current rotation
         * columns (col0 *= sx, col1 *= sy, col2 *= sz) at the time the command
         * executes — it does NOT store it for later application.  This means a
         * second set_scale call compounds with the first rather than replacing it,
         * which is exactly how consecutive draws get placed on opposite sides:
         *   set_scale(-1, 0.5, 1) → col0 negated (left side), col1 halved
         *   ang_x r5              → spin baked in
         *   draw 3351             → left side, Y-compressed
         *   set_scale(-1, 1, 1)   → col0 *= -1 again → back to +X (right side)
         *   draw 3351             → right side, same spin direction
         * Push/pop saves/restores scan_rot, so scale effects are naturally scoped. */
        if (val == 0x03800707 && i + 3 < total) {
            uint32_t fx = g_cop.geo_capture[(idx+1) & (GEO_CAPTURE_SIZE-1)];
            uint32_t fy = g_cop.geo_capture[(idx+2) & (GEO_CAPTURE_SIZE-1)];
            uint32_t fz = g_cop.geo_capture[(idx+3) & (GEO_CAPTURE_SIZE-1)];
            float sx = is_sane_float(fx) ? u32_as_float(fx) : 1.0f;
            float sy = is_sane_float(fy) ? u32_as_float(fy) : 1.0f;
            float sz = is_sane_float(fz) ? u32_as_float(fz) : 1.0f;
            int _r;
            for (_r = 0; _r < 3; _r++) {
                scan_rot[0][_r] *= sx;
                scan_rot[1][_r] *= sy;
                scan_rot[2][_r] *= sz;
            }
            /* Scale baked into scan_rot — flag as transformed so draws after
             * a scale-only setup (stage_dsp pattern: PUSH+set_scale+draw+POP)
             * are captured with the correct scale matrix and not skipped by the
             * no-placement guard. */
            have_ang = true;
            i += 3; continue;
        }

        /* --- Generic arg skip for all recognized non-draw commands -----------
         * Any command whose args we don't specifically process above may have
         * float args whose bit patterns accidentally match 0x3C007878 (≈ 1/128)
         * or another sentinel.  Skip them to prevent false triggers.
         * 0x3C007878 itself is excluded so its handler below still runs. */
        if (val != 0x3C007878) {
            int _nargs = sharc_args_for_cmd(val);
            if (_nargs > 0) i += _nargs;
            continue;
        }

        /* --- 0x3C007878: object marker, 8 args, mesh ptr at arg[4] (idx+5) --- */
        if (i + 8 >= total) continue;

        uint32_t w_mesh = g_cop.geo_capture[(idx + 5) & (GEO_CAPTURE_SIZE - 1)];

        /* Skip no-placement models that appear inside a clipped sub-window.
         * These are typically stale data or background art that was deliberately
         * drawn inside a window region (e.g. adv_movie_egg model 3333 after
         * set_window(win_down)) and must not bleed into other viewports.
         * Background models WITHOUT a clip window (e.g. adv_movie_snc terrain)
         * ARE legitimately at world origin and must render — don't skip them.
         * have_ang guards angle-only placed models (Egg Robo uses ang_y/z/x inside
         * push/pop without set_pos after the push clears it). */
        if (scan_active_bslot < 0 && !have_mat && !have_pos && !have_ang && have_clip_win) {
            i += 8;
            continue;
        }

        if (w_mesh == 0 || w_mesh < 0x800000) { i += 8; continue; }

        uint32_t mesh_off = w_mesh * 4u - mesh_ptr_subtract;
        if (mesh_off >= polygons_size) { i += 8; continue; }

        int model_idx = geo3d_lookup_by_pol(w_mesh);
        if (model_idx < 0) { i += 8; continue; }

        uint32_t toff = table_off + (uint32_t)model_idx * MODEL_ENTRY_SIZE;
        if ((size_t)toff + MODEL_ENTRY_SIZE > main_data_size) { i += 8; continue; }
        uint32_t entry_pol = read_u32_le(main_data + toff + 8);
        if (entry_pol < 0x800000) { i += 8; continue; }

        captured_model_t *cm = &geo->captured[geo->captured_count];
        cm->model_idx    = model_idx;
        cm->material_ptr = read_u32_le(main_data + toff + 4);
        material_ptr_to_color(cm->material_ptr,
                              &cm->color[0], &cm->color[1], &cm->color[2]);

        if (scan_active_bslot >= 0 && scan_active_bslot < 32) {
            /* Bone object: cop.h wrote the IK result to g_cop.tgp_bone[slot].
             * Format: [0..8]=col-major 3×3, [9..11]=T[xyz].
             * Convert to row-major 3×4 for cm->matrix; negate Z translation. */
            float *tb = g_sharc.tgp_bone[scan_active_bslot];
            int _r, _c;
            for (_r = 0; _r < 3; _r++) {
                for (_c = 0; _c < 3; _c++)
                    cm->matrix[_r*4+_c] = tb[_c*3+_r];
                cm->matrix[_r*4+3] = tb[9+_r];
            }
            cm->matrix[2]  = -cm->matrix[2];
            cm->matrix[6]  = -cm->matrix[6];
            cm->matrix[11] = -cm->matrix[11];
            cm->has_matrix = true;
        } else if (have_mat) {
            memcpy(cm->matrix, current_mat, sizeof(cm->matrix));
            cm->matrix[11] = -current_mat[11];
            if (have_pos) {
                cm->matrix[3]  =  current_pos[0];
                cm->matrix[7]  =  current_pos[1];
                cm->matrix[11] = -current_pos[2];
            }
            cm->has_matrix = true;
        } else if (have_pos || have_ang) {
            /* Build row-major 3×4 from scan_rot (column-major [col][row]) +
             * current_pos.  Scale is already baked into scan_rot columns by the
             * 0x3800707 handler, so no separate scale multiply needed here.
             * have_ang covers models drawn after ang commands but without set_pos
             * (e.g. model 3351 moustache, where have_pos was consumed by the
             * preceding model draw).  scan_rot's push/pop scoping prevents stale
             * angle state from leaking across unrelated command groups. */
            float (*r)[3] = scan_rot;
            cm->matrix[0]  =  r[0][0]; cm->matrix[1]  =  r[1][0]; cm->matrix[2]  = -r[2][0]; cm->matrix[3]  =  current_pos[0];
            cm->matrix[4]  =  r[0][1]; cm->matrix[5]  =  r[1][1]; cm->matrix[6]  = -r[2][1]; cm->matrix[7]  =  current_pos[1];
            cm->matrix[8]  =  r[0][2]; cm->matrix[9]  =  r[1][2]; cm->matrix[10] =  r[2][2]; cm->matrix[11] = -current_pos[2];
            cm->has_matrix = true;
        } else {
            cm->has_matrix = false;
        }

        cm->has_clip_win         = have_clip_win;
        cm->from_bone            = (scan_active_bslot >= 0 && scan_active_bslot < 32);
        cm->clip_win_x           = clip_win_x;
        cm->clip_win_y           = clip_win_y;
        cm->clip_win_w           = clip_win_w;
        cm->clip_win_h           = clip_win_h;

        cm->dbg_mesh_ptr    = w_mesh;
        cm->dbg_pos[0]      = current_pos[0];
        cm->dbg_pos[1]      = current_pos[1];
        cm->dbg_pos[2]      = current_pos[2];
        cm->dbg_ang_deg[0]  = 0.0f;
        cm->dbg_ang_deg[1]  = 0.0f;
        cm->dbg_ang_deg[2]  = 0.0f;
        cm->dbg_have_pos    = (int)have_pos;
        cm->dbg_have_ang    = (int)have_ang;
        cm->dbg_have_mat    = (int)have_mat;

        geo->captured_count++;
        i += 8;
        have_pos = false;
        have_mat = false;
        /* scan_active_bslot intentionally NOT reset here: one 0x1B803737 command
         * may precede multiple set_obj calls in the same push/pop scope (e.g.
         * Rocket Metal body + jet exhaust + regular exhaust all share bone 12).
         * It is reset on push (entering a new scope) and restored on pop. */
        /* have_clip_win persists until the next GEO_WIN_SENTINEL — all models
         * in a scene share the same clip window, not just the first one after
         * set_window (fixes adv_movie_egg phase 2/3/4 missing scissor rect). */
        (void)mesh_ptr_add;
    }

    /* Auto eye-baked: if this frame established a base SETPOS ≈ −eye, its
     * geometry is world−eye → render rotation-only (no second −eye subtract). */
    if (g_cam_auto_baked)
        g_cam_rot_only = frame_eye_baked;
}

/* ---- GEO display-list scanner (authentic hardware path) ----------------- *
 * Non-STF games and the m2-snake homebrew drive the GEO directly: the i960
 * builds a display list in bufferram and points the GEO read pointer at it.
 * (STF is reconstructed from the COP bone stream by geo3d_scan_captures above.)
 * This walks that list with the geo_parse command grammar and turns each
 * (MATRIX, OBJECT) pair into a captured_model_t — reusing the same mesh+matrix
 * render path. Only object_data (cmd 1) is emitted; direct_data (cmd 2) inline
 * geometry is not yet decoded (warned once and stops the walk).               */
static inline int geo3d__dl_args(const uint32_t *L, uint32_t nw, uint32_t p, uint32_t cmd) {
    switch (cmd) {
        case 1: case 0x11: return 4;                          /* object_data        */
        case 3: case 0x13: return 6;                          /* window/clip        */
        case 7: case 0x17: case 8: case 0x18:
        case 0x10: case 0x16: case 0x1e: return 1;            /* mode/zsort/lod/...  */
        case 9: case 0x19: return 2;                          /* focal              */
        case 0xa: case 0x1a: case 0xc: case 0x1c: return 3;   /* light              */
        case 0xb: case 0x1b: return 12;                       /* matrix             */
        case 0xd: return 2;
        case 4: case 0x14: case 5: case 0x15:                 /* texdata: 2 + count  */
            return 2 + (int)(p + 2 < nw ? L[p + 2] : 0);
        case 6: return 2 + 2 * (int)(p + 2 < nw ? L[p + 2] : 0);  /* texparam        */
        default: return 0;
    }
}

static inline void geo3d_scan_displaylist(geo3d_state_t *geo,
        const uint32_t *buff_ram, uint32_t buff_words, uint32_t rstart,
        const uint8_t *main_data, size_t main_data_size,
        uint32_t table_off, uint32_t table_count,
        const uint8_t *palette, size_t palette_size) {
    if (!main_data || !buff_ram) { geo->captured_count = 0; return; }
    geo3d_lookup_build(main_data, main_data_size, table_off, table_count);

    /* Snapshot the prev list for interpolation, once per vblank frame (same
     * cadence the COP-stream scanner uses). */
    if (g_cop.geo_frame_end != geo->last_frame_end) {
        memcpy(geo->captured_prev, geo->captured,
               (size_t)geo->captured_count * sizeof(captured_model_t));
        geo->captured_prev_count = geo->captured_count;
        geo->last_frame_end      = g_cop.geo_frame_end;
    }
    geo->captured_count = 0;

    float cur_mat[12];
    bool  have_mat = false;
    static int warned_direct = 0;
    uint32_t p = (rstart & (buff_words * 4u - 1u)) / 4u;   /* wrap within bufferram */
    for (uint32_t guard = 0; guard < buff_words && p < buff_words
                             && geo->captured_count < MAX_GEO_MODELS; guard++) {
        uint32_t op = buff_ram[p];
        if (op & 0x80000000u) break;                          /* JUMP -> flat list ends */
        uint32_t cmd = (op >> 23) & 0x1f;
        if (cmd == 0xf || cmd == 0x1f) break;                 /* END */

        if ((cmd == 0xb || cmd == 0x1b) && p + 12u < buff_words) {
            /* MATRIX: 12 floats stored column-major (col0,col1,col2,T) — convert
             * to the captured_model_t row-major 3x4 layout. */
            float m[12];
            for (int k = 0; k < 12; k++) memcpy(&m[k], &buff_ram[p + 1 + (uint32_t)k], 4);
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 3; c++) cur_mat[r * 4 + c] = m[c * 3 + r];
                cur_mat[r * 4 + 3] = m[9 + r];
            }
            have_mat = true;
        } else if ((cmd == 1 || cmd == 0x11) && p + 4u < buff_words) {
            /* OBJECT: args = tpa, tha, oba(mesh ptr), obc. The homebrew's oba is a
             * model-table mesh pointer, so reverse-map it to a model index and reuse
             * the STF mesh+matrix renderer. */
            uint32_t oba = buff_ram[p + 3];
            int model_idx = geo3d_lookup_by_pol(oba);
            if (model_idx >= 0) {
                uint32_t toff = table_off + (uint32_t)model_idx * MODEL_ENTRY_SIZE;
                if ((size_t)toff + MODEL_ENTRY_SIZE <= main_data_size) {
                    captured_model_t *cm = &geo->captured[geo->captured_count];
                    memset(cm, 0, sizeof(*cm));
                    cm->model_idx    = model_idx;
                    cm->material_ptr = read_u32_le(main_data + toff + 4);
                    /* Per-object flat colour: the homebrew encodes the colorbase in
                     * the object_data `tha` (= GEO_TEXRAM_BIT 0x800000 | colorbase*4)
                     * and stores the hue at palram[colorbase + 0x1000] (BGR555), set
                     * via m2_setcolor. Read it live from palette RAM. */
                    cm->color[0] = cm->color[1] = cm->color[2] = 1.0f;  /* fallback white */
                    {
                        uint32_t tha = buff_ram[p + 2];
                        uint32_t cb  = (tha & 0x007FFFFFu) >> 2;
                        uint32_t poff = (cb + 0x1000u) * 2u;
                        if (palette && poff + 1u < palette_size) {
                            uint16_t bgr = (uint16_t)(palette[poff] | (palette[poff + 1] << 8));
                            cm->color[0] = ( bgr        & 0x1F) / 31.0f;   /* R = bits[4:0]  */
                            cm->color[1] = ((bgr >> 5)  & 0x1F) / 31.0f;   /* G = bits[9:5]  */
                            cm->color[2] = ((bgr >> 10) & 0x1F) / 31.0f;   /* B = bits[14:10] */
                        }
                    }
                    cm->dbg_mesh_ptr = oba;
                    if (have_mat) {
                        memcpy(cm->matrix, cur_mat, sizeof(cm->matrix));
                        /* Homebrew geometry is camera space with +z forward (GEO
                         * projects screen = focal*x/z); the GL renderer looks down -z,
                         * so negate the whole Z-output row (row 2 = matrix[8..11]). */
                        cm->matrix[8]  = -cur_mat[8];
                        cm->matrix[9]  = -cur_mat[9];
                        cm->matrix[10] = -cur_mat[10];
                        cm->matrix[11] = -cur_mat[11];
                        cm->has_matrix = true;
                        cm->dbg_have_mat = 1;
                        cm->dbg_pos[0] = cur_mat[3];
                        cm->dbg_pos[1] = cur_mat[7];
                        cm->dbg_pos[2] = -cur_mat[11];
                    }
                    /* Near-plane cull: objects at/behind the camera (z >= ~0) project
                     * to infinity and streak across the screen (e.g. the starfield
                     * passing the camera). Keep only those safely in front. */
                    if (!have_mat || cm->matrix[11] <= -1.0f)
                        geo->captured_count++;
                }
            }
        } else if (cmd == 0xa && p + 3u < buff_words) {
            /* LIGHT (0x05000A0A): 3 floats (x,y,z). The GEO lights each face by
             * normal·light; mirror it into the renderer's light dir so the flat
             * panels shade like the HLE. Camera space — negate z to match the
             * Z-row negation the host (−z forward) applies to the geometry. */
            float lx, ly, lz;
            memcpy(&lx, &buff_ram[p + 1], 4);
            memcpy(&ly, &buff_ram[p + 2], 4);
            memcpy(&lz, &buff_ram[p + 3], 4);
            g_light_dir[0] = lx; g_light_dir[1] = ly; g_light_dir[2] = -lz;
        } else if (cmd == 2 || cmd == 0x12) {
            if (!warned_direct) {
                LOG_WARN("geo3d displaylist: direct_data (cmd 2) not yet decoded; stopping walk");
                warned_direct = 1;
            }
            break;   /* variable-length inline geometry — can't skip reliably yet */
        }
        p += 1u + (uint32_t)geo3d__dl_args(buff_ram, buff_words, p, cmd);
    }
}

/* ---- Texture words and colours, as the rasterizer reads them ------------- *
 * A face's texture header (4 words) and texture points (a (v,u) pair a corner)
 * are read from the address its object command carries: texture ROM, or with
 * bit 23 set the rasterizer's own 64K-word texture RAM, which the display
 * list's texture-data command writes (model2_v.cpp, raster command 04). A
 * face's colour is palette RAM at colorbase + 0x1000 (model2rd.ipp), which
 * games reload per scene; the ROM colour table is only what a scene starts with.
 *
 * The object-command addresses stand in for the model table's while a caller
 * sets them (GEO display list draws); 0xFFFFFFFF leaves the table's. */
/* The 32 material slots the display list's texture-parameter command sets
 * (model2_v.cpp geo_texture_parameters): per slot diffuse and ambient, 0..255.
 * A polygon's attribute word names its slot, bits 18..22. While
 * g_geo3d_board_luma is set the decoder lights faces with them the way the
 * geometrizer does (geo_parse_np_ns) instead of its fixed approximation:
 *   luminance = (N.L * N.P < 0) ? 0 : |N.L|,  luma = clamp(luminance*diffuse + ambient + specular, 0, 255)
 * N the polygon's own normal from ROM, L the list's light vector, P a corner,
 * all in eye space. Specular is the z of L reflected about N, raised by the
 * slot's control and scaled, and only in a mode with bit 0 set (geo_parse_np_s).
 * The same pass gives each face the rasterizer's texlod from the slot's
 * distance coefficient, |N.P| and the list's LOD scale. */
static int            g_geo3d_board_luma  = 0;
static uint32_t       g_geo3d_mode        = 0;
static float          g_geo3d_lod         = 0.0f;
/* A mesh in polygon RAM rather than ROM (an object address without bit 23):
 * while set, the decoder reads it from here, with the texture addresses above. */
static const uint8_t *g_geo3d_obj_mesh      = NULL;
static size_t         g_geo3d_obj_mesh_size = 0;
static uint32_t       g_geo3d_obj_tpa     = 0xFFFFFFFFu;
static uint32_t       g_geo3d_obj_tha     = 0xFFFFFFFFu;
static const uint8_t *g_geo3d_palram      = NULL;
static size_t         g_geo3d_palram_size = 0;

static inline bool geo3d_tex_word(const uint8_t *rom, size_t rom_size, uint32_t addr, uint16_t *out) {
    if (addr & 0x800000u) { *out = g_geo_texram_words[addr & 0xFFFFu]; return true; }
    size_t b = (size_t)addr * 2u;
    if (!rom || b + 2 > rom_size) return false;
    *out = (uint16_t)(rom[b] | (rom[b + 1] << 8));
    return true;
}

/* ---- GEO display list, as the geometrizer walks it ----------------------- *
 * The board's own draw list (memory.h, "GEO display list"): the i960 and the
 * COP lay it in bufferram and the GEO walks it once a frame. Walked here the way
 * MAME's geo_parse does (model2_v.cpp), every object comes out with the matrix
 * the COP put down with it — model to eye, nothing to reconstruct — and the
 * window, focal lengths and light the list had set by then.
 *
 * Projection, per model2_3d_project: an eye-space point (x, y, z), z forward,
 * lands at screen x = xoff + cx + fx*x/z, y = (384 - cy) + yoff - fy*y/z, with
 * xoff = 84 + H-sync and yoff = 130 + V-sync (the CRTC registers) and (cx, cy)
 * the window's centre for the object's eye mode. The window's viewport clips the
 * same way. STF's full-screen window is (0,127)-(496,511) centred (248,319), and
 * its H/V sync of -84/-3 put that centre at (248,192).
 *
 * Returns false (and captures nothing) when the list does not reach END — the
 * caller can fall back to the COP-stream scanner. */
static int g_geo_use_list = 1;   /* 0: the old COP-stream reconstruction */

static inline bool geo3d_scan_geo_list(geo3d_state_t *geo,
        const uint32_t *words, uint32_t nw, uint32_t rstart,
        int16_t hsync, int16_t vsync,
        const uint8_t *main_data, size_t main_data_size,
        uint32_t table_off, uint32_t table_count) {
    if (!main_data || !words) return false;
    geo3d_lookup_build(main_data, main_data_size, table_off, table_count);

    if (g_cop.geo_frame_end != geo->last_frame_end) {
        memcpy(geo->captured_prev, geo->captured,
               (size_t)geo->captured_count * sizeof(captured_model_t));
        geo->captured_prev_count = geo->captured_count;
        geo->last_frame_end      = g_cop.geo_frame_end;
    }

    #define GEOL_S12(v) ((int16_t)(((v) & 0x800) ? (int)(v) - 0x1000 : (int)(v)))
    float    raw[12] = { 1,0,0, 0,1,0, 0,0,1, 0,0,0 };   /* column-major, as the COP wrote it */
    float    fx = 280.0f, fy = 280.0f;
    float    light[3] = { 0.0f, 0.0f, 1.0f };
    /* Mode and LOD are the geometrizer's own state and outlive a list. */
    static uint32_t mode = 0;
    static float    lod  = 0.0f;
    int16_t  wvp[4]  = { 0, 0, 496, 384 };                /* viewport, list coordinates */
    int16_t  wc[4][2] = { {248, 192}, {248, 192}, {248, 192}, {248, 192} };
    int      window = 0;
    float    xoff = 84.0f + hsync, yoff = 130.0f + vsync;
    int      count = 0;
    bool     ended = false;

    uint32_t p = (rstart & 0x1FFFFu) >> 2;
    for (uint32_t guard = 0; guard < 0x8000u && p < nw && count < MAX_GEO_MODELS; guard++) {
        uint32_t op = words[p];
        if (op & 0x80000000u) { p = (op & 0x1FFFFu) >> 2; continue; }   /* jump */
        uint32_t cmd = (op >> 23) & 0x1F;
        #define A(k) (p + 1u + (k) < nw ? words[p + 1u + (k)] : 0u)
        uint32_t len = 0;
        switch (cmd) {
            case 0x00: len = 0; break;
            case 0x01: case 0x11: {                            /* object */
                len = 4;
                uint32_t oba = A(2);
                int model_idx = -1;
                if (oba & 0x00800000u) {                       /* polygon ROM: a model-table entry */
                    model_idx = geo3d_lookup_by_pol(oba);
                    if (model_idx < 0) break;
                    uint32_t toff = table_off + (uint32_t)model_idx * MODEL_ENTRY_SIZE;
                    if ((size_t)toff + MODEL_ENTRY_SIZE > main_data_size) break;
                }
                captured_model_t *cm = &geo->captured[count++];
                memset(cm, 0, sizeof *cm);
                cm->model_idx    = model_idx;                  /* -1: polygon RAM, see oba */
                if (model_idx >= 0) {
                    cm->material_ptr = read_u32_le(main_data + table_off + (uint32_t)model_idx * MODEL_ENTRY_SIZE + 4);
                    material_ptr_to_color(cm->material_ptr, &cm->color[0], &cm->color[1], &cm->color[2]);
                } else {
                    cm->color[0] = cm->color[1] = cm->color[2] = 0.7f;
                }
                /* The decoder reads vertices as (x, y, -z) and the host looks down
                 * -z: undo the first negation in column 2, apply the second to row 2,
                 * so what reaches the screen is the board's M * (x, y, z). */
                const float *M = raw;
                float *H = cm->matrix;
                H[0] =  M[0]; H[1] =  M[3]; H[2]  = -M[6]; H[3]  =  M[9];
                H[4] =  M[1]; H[5] =  M[4]; H[6]  = -M[7]; H[7]  =  M[10];
                H[8] = -M[2]; H[9] = -M[5]; H[10] =  M[8]; H[11] = -M[11];
                cm->has_matrix = true;
                cm->view_space = true;
                int sel = (int)((op >> 29) & 3u);
                cm->gproj[0] = fx;
                cm->gproj[1] = fy;
                cm->gproj[2] = xoff + wc[sel][0];
                cm->gproj[3] = (384.0f - wc[sel][1]) + yoff;
                cm->vp[0] = (int16_t)(wvp[0] + xoff);
                cm->vp[1] = (int16_t)((384 - wvp[3]) + yoff);
                cm->vp[2] = (int16_t)(wvp[2] + xoff);
                cm->vp[3] = (int16_t)((384 - wvp[1]) + yoff);
                cm->window   = (uint16_t)window;
                cm->light[0] = light[0]; cm->light[1] = light[1]; cm->light[2] = -light[2];
                cm->geo_mode = mode;
                cm->geo_lod  = lod;
                cm->dbg_mesh_ptr = A(2);
                cm->tpa = A(0);
                cm->tha = A(1);
                cm->dbg_pos[0] = M[9]; cm->dbg_pos[1] = M[10]; cm->dbg_pos[2] = M[11];
                cm->dbg_have_mat = 1;
                break;
            }
            case 0x02: case 0x12:                              /* direct data: inline polygons */
                geo->captured_count = count;
                geo->geo_windows = window + 1;
                return false;
            case 0x03: case 0x13: {                            /* window */
                len = 6;
                uint32_t w0 = A(0), w1 = A(1);
                wvp[0] = GEOL_S12((w0 >> 16) & 0xFFF); wvp[1] = GEOL_S12(w0 & 0xFFF);
                wvp[2] = GEOL_S12((w1 >> 16) & 0xFFF); wvp[3] = GEOL_S12(w1 & 0xFFF);
                for (int k = 0; k < 4; k++) {
                    uint32_t c = A(2 + (uint32_t)k);
                    wc[k][0] = GEOL_S12((c >> 16) & 0xFFF);
                    wc[k][1] = GEOL_S12(c & 0xFFF);
                }
                window++;
                break;
            }
            /* texture data / polygon data / texture parameters: memory.h
             * (geodl_apply_state) applied them when the list was published */
            case 0x04: case 0x14: case 0x05: case 0x15: len = 2 + A(1); break;
            case 0x06: len = 2 + 2 * A(1); break;
            case 0x07: case 0x17: len = 1; mode = A(0); break;
            case 0x16:            len = 1; lod = u32_as_float(A(0)); break;
            case 0x08: case 0x18: case 0x10: case 0x1E: len = 1; break;
            case 0x09: case 0x19:                              /* focal lengths */
                len = 2;
                fx = u32_as_float(A(0)); fy = u32_as_float(A(1));
                break;
            case 0x0A: case 0x1A:                              /* light vector */
                len = 3;
                light[0] = u32_as_float(A(0)); light[1] = u32_as_float(A(1)); light[2] = u32_as_float(A(2));
                break;
            case 0x0B: case 0x1B:                              /* matrix */
                len = 12;
                for (uint32_t k = 0; k < 12; k++) raw[k] = u32_as_float(A(k));
                break;
            case 0x0C: case 0x1C:                              /* translation only */
                len = 3;
                for (uint32_t k = 0; k < 3; k++) raw[9 + k] = u32_as_float(A(k));
                break;
            case 0x0D: len = 2; break;
            case 0x1D: len = 2 + 3 * A(1); break;
            case 0x0F: case 0x1F: ended = true; break;
            default: break;
        }
        #undef A
        if (ended) break;
        p += 1u + len;
    }
    #undef GEOL_S12
    geo->captured_count = count;
    geo->geo_windows    = window + 1;
    return ended;
}

/* ---- J=1.0 index-array polygon decoder ---------------------------------- */

/*
 * Decode one model from the polygon ROM into wire-line segments.
 *
 * Validated J=1.0 on STF (4402/4405 models) AND on Daytona (2377 models) —
 * cross-game validation is what makes this algorithm load-bearing rather
 * than STF-specific.
 *
 *   iFlag = vp[25] & 0x03:
 *     0 — sentinel: previous group ended; start fresh strip
 *     1 — carry far edge of previous face (Index[-4], Index[-2])
 *     2 — plain new quad group
 *     3 — anchor new strip off previous corner
 *
 *   Face loop runs 2 groups behind the tail; f1==2 means triangle, else
 *   quad with A-B-D-C winding.
 *
 *   Vertex convention: (x, y, -z).
 */
static inline void geo3d_decode_model(int model_idx,
                                       const uint8_t *main_data, size_t main_data_size,
                                       const uint8_t *polygons,  size_t polygons_size,
                                       const uint8_t *materials, size_t materials_size,
                                       uint32_t table_off, uint32_t table_count,
                                       uint32_t mesh_ptr_subtract, uint32_t mesh_ptr_add,
                                       const float *matrix,
                                       float cr, float cg, float cb) {
    static vec3_t sv[GEO3D_IA_MAX_VERTS];
    static uint32_t svk[GEO3D_IA_MAX_VERTS];   /* geo3d_corner_key, pre-transform */
    static int    qt[GEO3D_IA_MAX_VPS];
    static vec3_t qn[GEO3D_IA_MAX_VPS];         /* the record's polygon normal (z negated like the points) */
    static uint32_t qa[GEO3D_IA_MAX_VPS];       /* the record's attribute word */
    static int    idx[GEO3D_IA_MAX_IDX];

    int n_sv = 0, n_qt = 0, n_idx = 4;
    int vcount = 0;
    uint32_t mesh_offset;
    /* Material stream — one 8-byte record per face: [6-byte tex key][LE u16 BGR555].
     * Base address = (model-table[+0x04] pointer) * 2 into the textures ROM.
     * Format reverse-engineered from the Obj2StF converter (GophUndMe/Obj2StF). */
    uint32_t mat_base = 0;
    uint32_t mat_word = 0;
    bool     have_mat = false;
    /* UV stream — model-table[+0x00] word index into the textures ROM; per polygon
     * NumVerts (pv,pu) 16-bit pairs, tile-relative texel = raw/8.  Vertex order in
     * the stream maps to decoder [A,B,D,C] (quad) / [A,B,C] (tri). */
    uint32_t uv_word = 0;     /* running word offset into materials[] for UVs */
    bool     have_uv = false;

    if (!main_data || !polygons) return;
    if (g_geo3d_obj_mesh) {
        /* A polygon-RAM object: the mesh starts at the object address, and its
         * texture addresses come from the object command. */
        polygons      = g_geo3d_obj_mesh;
        polygons_size = g_geo3d_obj_mesh_size;
        mesh_offset   = 0;
        if (materials && g_geo3d_obj_tha != 0xFFFFFFFFu) {
            mat_word = g_geo3d_obj_tha; mat_base = mat_word * 2u; have_mat = mat_word != 0;
            uv_word  = g_geo3d_obj_tpa; have_uv = uv_word != 0;
        }
    } else {
    if (model_idx < 0 || (uint32_t)model_idx >= table_count) return;

    {
        uint32_t toff = table_off + (uint32_t)model_idx * MODEL_ENTRY_SIZE;
        if ((size_t)toff + MODEL_ENTRY_SIZE > main_data_size) return;
        uint32_t mesh_ptr_raw = read_u32_le(main_data + toff + 8);
        if (mesh_ptr_raw == 0) return;
        mesh_offset = mesh_ptr_raw * 4u - mesh_ptr_subtract;
        mesh_offset += mesh_ptr_add;

        if (materials) {
            uint32_t mat_ptr_raw = read_u32_le(main_data + toff + 4);
            uint32_t uv_ptr_raw  = read_u32_le(main_data + toff + 0);
            if (g_geo3d_obj_tha != 0xFFFFFFFFu) mat_ptr_raw = g_geo3d_obj_tha;
            if (g_geo3d_obj_tpa != 0xFFFFFFFFu) uv_ptr_raw  = g_geo3d_obj_tpa;
            mat_word = mat_ptr_raw;        /* word address (bit 23: texture RAM) */
            mat_base = mat_ptr_raw * 2u;
            have_mat = (mat_ptr_raw != 0);
            uv_word = uv_ptr_raw;          /* word index; byte = *2 */
            have_uv = (uv_ptr_raw != 0);
        }
        g_dbg_tex_models++;
        if (have_uv)  g_dbg_tex_models_uv++;
        if (have_mat) g_dbg_tex_models_mat++;
    }
    }

    /* Initial Index: placeholder group that becomes the first face. */
    idx[0] = 0; idx[1] = 1; idx[2] = 2; idx[3] = 3;

    while (vcount < GEO3D_IA_MAX_VPS) {
        if ((size_t)mesh_offset + VERTEX_PAIR_SIZE > polygons_size) break;
        if (n_sv + 2 > GEO3D_IA_MAX_VERTS || n_idx + 4 > GEO3D_IA_MAX_IDX) break;

        const uint8_t *vp = polygons + mesh_offset;
        bool is_end = (vp[24] == 0 && vp[25] == 0 && vp[26] == 0 && vp[27] == 0);

        vec3_t v1, v2;
        v1.x =  read_float_le(vp +  0);
        v1.y =  read_float_le(vp +  4);
        v1.z = -read_float_le(vp +  8);
        v2.x =  read_float_le(vp + 12);
        v2.y =  read_float_le(vp + 16);
        v2.z = -read_float_le(vp + 20);

        uint8_t f1    = vp[24];
        uint8_t iflag = vp[25] & 0x03;

        svk[n_sv]     = geo3d_corner_key(v1);
        svk[n_sv + 1] = geo3d_corner_key(v2);

        if (matrix) { v1 = apply_matrix(v1, matrix); v2 = apply_matrix(v2, matrix); }

        sv[n_sv++] = v1;
        sv[n_sv++] = v2;
        qn[n_qt] = (vec3_t){ read_float_le(vp + 28), read_float_le(vp + 32), -read_float_le(vp + 36) };
        qa[n_qt] = read_u32_le(vp + 24);
        qt[n_qt++] = f1;

        int lvc   = vcount + 1;
        int new_a = 2 * (lvc + 1);   /* vertex index of current VP's v1 */

        switch (iflag) {
            case 0:
                idx[n_idx - 4] = -1; idx[n_idx - 3] = -1;
                idx[n_idx - 2] = -1; idx[n_idx - 1] = -1;
                idx[n_idx++] = new_a - 2; idx[n_idx++] = new_a - 1;
                idx[n_idx++] = new_a;     idx[n_idx++] = new_a + 1;
                break;
            case 1: {
                int p3 = idx[n_idx - 4], p2 = idx[n_idx - 2];
                idx[n_idx++] = p3;    idx[n_idx++] = p2;
                idx[n_idx++] = new_a; idx[n_idx++] = new_a + 1;
                break;
            }
            case 2:
                idx[n_idx++] = new_a - 2; idx[n_idx++] = new_a - 1;
                idx[n_idx++] = new_a;     idx[n_idx++] = new_a + 1;
                break;
            case 3: {
                int anc_a = (f1 == 1) ? idx[n_idx - 1] : idx[n_idx - 2];
                int anc_b = idx[n_idx - 3];
                idx[n_idx++] = anc_a; idx[n_idx++] = anc_b;
                idx[n_idx++] = new_a; idx[n_idx++] = new_a + 1;
                break;
            }
        }

        if (is_end) break;
        mesh_offset += VERTEX_PAIR_SIZE;
        vcount++;
    }

    /* Face loop — emit wireframe edges, stopping 2 groups before the tail. */
    /* Material records are written per EMITTED face (sentinel/degenerate strip
     * groups get none), while the UV stream stores 4 (pv,pu) pairs per face-loop
     * ITERATION (incl. sentinels). So material is indexed by emitted-count (efi)
     * and the UV stream advances 8 words every iteration. Verified on model 3351:
     * 13 iterations, 3 sentinels → 10 material records, 104-word UV stream. */
    int efi = 0;
    /* The board's z-sort, carried across the walk (see geo3d_sort_z). */
    int zsrc[4] = {0,0,0,0}; uint32_t zmode = 0u; bool zset = false;
    const bool standing = geo3d_model_standing(model_idx);
    geo3d_split_reset();
    for (int i = 0; i < n_idx - 8; i += 4) {
        int fi = i / 4;

        int ai = idx[i], bi = idx[i + 1], ci = idx[i + 2], di = idx[i + 3];

        /* NumVerts drives BOTH the UV-stream advance and the fill topology.
         * Must be computed for every group (even skipped ones) to keep the UV
         * stream aligned, exactly like MAME advances command_buffer[0]. */
        bool tri_cnt = (fi < n_qt && qt[fi] == 2);
        int  nv = tri_cnt ? 3 : 4;

        /* ---- texture header (tile rect + palette index) ---- */
        float fr = cr, fg = cg, fb = cb;     /* flat color (palette) */
        uint32_t texx = 0, texy = 0, texw = 32, texh = 32, texsheet = 0;
        uint32_t lumabase = 0;               /* texheader[1] low byte << 7 (lumaram band) */
        bool textured = false;               /* texheader[0] bit14 = textured */
        uint32_t fflags = 0;                 /* GEO3D_FACE_* */
        bool untex_trans = false;            /* untextured + transparent: the board draws nothing */
        if (have_mat) {
            uint32_t hw = mat_word + (uint32_t)efi * 4u;
            uint16_t th0 = 0, th1 = 0, th2 = 0, th3 = 0;
            if (geo3d_tex_word(materials, materials_size, hw,      &th0) &&
                geo3d_tex_word(materials, materials_size, hw + 1u, &th1) &&
                geo3d_tex_word(materials, materials_size, hw + 2u, &th2) &&
                geo3d_tex_word(materials, materials_size, hw + 3u, &th3)) {
                lumabase = (uint32_t)(th1 & 0xff) << 7;
                textured = (th0 & 0x4000) != 0;
                texw = 32u << (th0 & 0x7);
                texh = 32u << ((th0 >> 3) & 0x7);
                texx = 32u * (th2 & 0x3f);
                texy = 32u * ((th2 >> 6) & 0x1f);
                texsheet = (th2 >> 12) & 1u;            /* which texram bank */
                if      (g_uv_bank_mode == 1) texsheet = 0u;
                else if (g_uv_bank_mode == 2) texsheet = 1u;
                else if (g_uv_bank_mode == 3) texsheet ^= 1u;
                if (textured && (th0 & 0x2000)) fflags |= GEO3D_FACE_TRANSPARENT;
                untex_trans = !textured && (th0 & 0x2000);
                if (th0 & 0x8000)               fflags |= GEO3D_FACE_CHECKER;
                if (texsheet)                   fflags |= GEO3D_FACE_SHEET1;
                if ((th0 >> 8) & 1)             fflags |= GEO3D_FACE_MIRROR_X;
                if ((th0 >> 9) & 1)             fflags |= GEO3D_FACE_MIRROR_Y;
                uint32_t matidx = (th3 >> 6) & 0x3ff;   /* colorbase → palette */
                uint32_t pal = GEO3D_PALETTE_OFF + matidx * 2u;
                uint32_t ram = (matidx + 0x1000u) * 2u;
                if (g_geo3d_palram && (size_t)ram + 2 <= g_geo3d_palram_size) {
                    uint16_t cw = (uint16_t)(g_geo3d_palram[ram] | (g_geo3d_palram[ram + 1] << 8)) & 0x7FFF;
                    geo3d_bgr555(cw, &fr, &fg, &fb);
                } else if (main_data && (size_t)pal + 2 <= main_data_size) {
                    uint16_t cw = (uint16_t)main_data[pal] |
                                  ((uint16_t)main_data[pal + 1] << 8);
                    geo3d_bgr555(cw, &fr, &fg, &fb);
                }
                /* Debug dump of this model's per-face texture tiles. */
                if (GEO3D_DUMP_TEX(model_idx)) {
                    static FILE *mtf = NULL;
                    if (fi == 0) { if (mtf) fclose(mtf); mtf = fopen("model_tex.txt", "w");
                        if (mtf) {
                            fprintf(mtf, "# model %d texture tiles  th0 th2 th3 -> sheet (texx,texy) texw x texh\n", model_idx);
                            /* Neighbouring model-table entries: uv_ptr(+0)/mat_ptr(+4)/mesh_ptr(+8).
                             * UV-stream length for this model = uv_ptr[next] - uv_ptr[this]. */
                            for (int mi = model_idx - 1; mi <= model_idx + 2; mi++) {
                                if (mi < 0 || (uint32_t)mi >= table_count) continue;
                                uint32_t te = table_off + (uint32_t)mi * MODEL_ENTRY_SIZE;
                                if ((size_t)te + MODEL_ENTRY_SIZE > main_data_size) continue;
                                fprintf(mtf, "# table[%d]: uv_ptr=%u mat_ptr=%u mesh_ptr=%u\n", mi,
                                        read_u32_le(main_data + te + 0), read_u32_le(main_data + te + 4),
                                        read_u32_le(main_data + te + 8));
                            }
                        } }
                    int _skip = (ai < 0 || ai >= n_sv || bi < 0 || bi >= n_sv);
                    if (mtf) { fprintf(mtf,
                        "face %3d: th0=%04X th2=%04X th3=%04X  textured=%d nv=%d f1=%d ai=%d bi=%d SKIP=%d have_uv=%d uv_word=%u sheet=%u tile=(%4u,%4u) %ux%u colorbase=%u efi=%d rgb=(%.2f,%.2f,%.2f) lb=%u fl=%u A=(%.2f,%.2f,%.2f) D=(%.2f,%.2f,%.2f)\n",
                        fi, th0, th2, th3, textured?1:0, nv, (fi < n_qt ? qt[fi] : -1),
                        ai, bi, _skip, have_uv?1:0, uv_word, texsheet, texx, texy, texw, texh, matidx, efi, fr, fg, fb, lumabase, fflags,
                        (ai >= 0 && ai < n_sv) ? sv[ai].x : 0.0f, (ai >= 0 && ai < n_sv) ? sv[ai].y : 0.0f, (ai >= 0 && ai < n_sv) ? sv[ai].z : 0.0f,
                        (di >= 0 && di < n_sv) ? sv[di].x : 0.0f, (di >= 0 && di < n_sv) ? sv[di].y : 0.0f, (di >= 0 && di < n_sv) ? sv[di].z : 0.0f);
                        /* Raw UV-stream window around the first cone face, to find the
                         * real (pv,pu) pairs and the correct per-face stride. */
                        if (fi == 0) {
                            fprintf(mtf, "  -- raw UV stream u16 (pv,pu) from model start uv_word=%u --\n", uv_word);
                            for (int w = 0; w < 48; w += 2) {
                                long bo = ((long)uv_word + w) * 2;
                                if (bo >= 0 && (size_t)bo + 4 <= materials_size) {
                                    uint16_t v0 = (uint16_t)materials[bo]   | ((uint16_t)materials[bo+1] << 8);
                                    uint16_t v1 = (uint16_t)materials[bo+2] | ((uint16_t)materials[bo+3] << 8);
                                    fprintf(mtf, "  word %+3d (off %u): pv=%5u pu=%5u\n", w, uv_word + w, v0, v1);
                                }
                            }
                        }
                        fflush(mtf); }
                }
            }
        }

        /* Flat-colour override (homebrew display list): keep the caller's colour,
         * force untextured — model 456's ROM material/texture is meaningless here. */
        if (g_geo_flat_color) { textured = false; fr = cr; fg = cg; fb = cb; fflags = 0; }
        float ffl = (float)fflags;

        /* Atlas tile rect (pixels) for this face — passed to the shader for the
         * per-pixel wrap. tw=0 → untextured (flat color). */
        float ftx = (float)texx;
        float fty = (float)((uint32_t)texsheet * GEO3D_SHEET_H + texy);
        float ftw = textured ? (float)texw : 0.0f;
        float fth = (float)texh;

        /* ---- UV stream: nv (pv,pu) pairs, mapped to [A,B,D,C]/[A,B,C] ---- */
        /* uvv[] indexed by vertex slot: 0=A 1=B 2=C 3=D.  Stored as tile-relative
         * texel coords (may run past the tile); the shader wraps per-pixel. */
        float uvu[4] = {0,0,0,0}, uvv[4] = {0,0,0,0};
        uint16_t cap_pu = 0, cap_pv = 0;
        if (have_uv) {
            static const int quad_slot[4]     = {1,0,2,3};  /* stream k → B,A,C,D */
            static const int tri_slot[3]      = {1,0,2};    /* stream k → B,A,C   */
            static const int quad_slot_fwd[4] = {0,1,3,2};  /* old: A,B,D,C */
            static const int tri_slot_fwd[3]  = {0,1,2};    /* old: A,B,C   */
            const int *slot = g_uv_quad_order ? (tri_cnt ? tri_slot_fwd : quad_slot_fwd)
                                              : (tri_cnt ? tri_slot     : quad_slot);
            for (int k = 0; k < nv; k++) {
                uint32_t tw_ = uv_word + (uint32_t)k * 2u;
                uint16_t pv = 0, pu = 0;
                if (!geo3d_tex_word(materials, materials_size, tw_, &pv) ||
                    !geo3d_tex_word(materials, materials_size, tw_ + 1u, &pu)) break;
                if (k == 0) { cap_pu = pu; cap_pv = pv; }
                /* Only assign atlas UVs for textured faces; untextured faces
                 * keep uv=-1 so the shader uses the flat palette color.  Still
                 * advance the stream below to stay aligned with MAME. */
                if (!textured) continue;
                float tu = (float)pu / 8.0f, tv = (float)pv / 8.0f;
                if (g_uv_flip_u) tu = (float)texw - tu;
                if (g_uv_flip_v) tv = (float)texh - tv;
                if (g_uv_swap)   { float t = tu; tu = tv; tv = t; }
                uvu[slot[k]] = tu; uvv[slot[k]] = tv;   /* tile-texel; shader wraps */
                if (GEO3D_DUMP_TEX(model_idx) && fi <= 12) {
                    static FILE *uf = NULL;
                    if (fi == 0 && k == 0) { if (uf) fclose(uf); uf = fopen("model_uv.txt", "w"); }
                    if (!uf) uf = fopen("model_uv.txt", "a");
                    if (uf) { fprintf(uf, "face %2d k=%d nv=%d pu=%u pv=%u -> tu=%.1f tv=%.1f tile=(%.0f,%.0f) %.0fx%.0f\n",
                                      fi, k, nv, pu, pv, tu, tv, ftx, fty, ftw, fth); fflush(uf); }
                }
            }
        }
        uv_word += (uint32_t)nv * 2u;   /* nv (pv,pu) pairs per iteration (3 tri / 4 quad) */
        g_dbg_tex_faces++;
        if (textured) g_dbg_tex_textured++;
        if (uvu[0] >= 0.0f || uvu[1] >= 0.0f) g_dbg_tex_uv_faces++;
        /* Capture faces that sample the EYES tile region (sheet0, top-right) to
         * find which models reference it (should include the Death Egg). */
        if (textured && texsheet == 0 && texx >= 1600 && texx <= 1920
                && texy <= 340 && g_dbg_face_uv_n < 24) {
            dbg_face_uv_t *d = &g_dbg_face_uv[g_dbg_face_uv_n++];
            d->model = model_idx; d->texx = (uint16_t)texx; d->texy = (uint16_t)texy;
            d->texsheet = (uint16_t)texsheet; d->tri = (uint16_t)(tri_cnt ? 1 : 0);
            for (int s = 0; s < 4; s++) { d->au[s] = uvu[s]; d->av[s] = uvv[s]; }
            d->texw = (uint16_t)texw; d->texh = (uint16_t)texh;
            d->pu0 = cap_pu; d->pv0 = cap_pv;
        }

        if (ai < 0 || ai >= n_sv) continue;
        if (bi < 0 || bi >= n_sv) continue;

        vec3_t A = sv[ai], B = sv[bi];
        bool has_C = (ci >= 0 && ci < n_sv);
        bool has_D = (di >= 0 && di < n_sv);
        vec3_t C = has_C ? sv[ci] : (vec3_t){0,0,0};
        vec3_t D = has_D ? sv[di] : (vec3_t){0,0,0};

        bool is_tri = tri_cnt || !has_C || !has_D;

        geo3d_zsort_step((fi < n_qt) ? qa[fi] : 0u, is_tri, has_C, ai, bi, ci, di,
                         zsrc, &zmode, &zset);
        g_geo3d_emit_zs = standing ? GEO3D_ZSORT_NONE : geo3d_sort_z(sv, zsrc, zmode);

        /* Per-face luminance (poly_luma) = |normal·light|*diffuse + ambient,
         * approximating MAME's per-polygon lighting (model2_v.cpp geo_parse).
         * NOT folded into the color — it's passed through so the colorxlat luma
         * ramp can use it (luma6 = lumaram[lumabase+texel]*poly_luma/256).
         * Normal = cross of the transformed edges (two-sided via |dot|). */
        float pl = 1.0f;
        g_geo3d_emit_texlod = GEO3D_TEXLOD_NONE;
        bool board_cull = false;             /* the geometrizer would not draw this face */
        vec3_t fn = (fi < n_qt) ? qn[fi] : (vec3_t){0, 0, 0};
        /* No fallback to the approximation for a zero normal: the board lights
         * it too, to luminance 0, so the face gets its ambient term. */
        if (g_geo3d_board_luma && matrix) {
            float nx = matrix[0]*fn.x + matrix[1]*fn.y + matrix[2]*fn.z;
            float ny = matrix[4]*fn.x + matrix[5]*fn.y + matrix[6]*fn.z;
            float nz = matrix[8]*fn.x + matrix[9]*fn.y + matrix[10]*fn.z;
            float dotl = nx*g_light_dir[0] + ny*g_light_dir[1] + nz*g_light_dir[2];
            float dotp = nx*A.x + ny*A.y + nz*A.z;
            float lum  = (dotl * dotp < 0.0f) ? 0.0f : fabsf(dotl);
            /* check_culling: a face whose attribute word lacks the double-sided
             * bit 17 is dropped when N.P < 0 (the rear), and a link of type 0
             * (bits 8..9) is never drawn. */
            uint32_t at = (fi < n_qt) ? qa[fi] : 0u;
            board_cull = (((at >> 17) & 1u) == 0 && dotp < 0.0f) || ((at >> 8) & 3u) == 0;
            const float *tp = g_geo_texparam[(at >> 18) & 0x1F];
            float spec = 0.0f;
            if (g_geo3d_mode & 1u) {
                /* Board z is this space's -z, for the normal and the light alike. */
                uint32_t ctl = (uint32_t)tp[3];
                spec = g_light_dir[2] - 2.0f * dotl * nz;
                if (spec < 0.0f || ctl == 0) spec = 0.0f;
                if ((ctl >> 1) != 0) spec *= spec;
                if ((ctl >> 2) != 0) spec *= spec;
                if (((ctl + 1) >> 3) != 0) spec *= spec;
                spec *= tp[2];
            }
            float luma = lum * tp[0] + tp[1] + spec;
            if (luma < 0.0f) luma = 0.0f;
            if (luma > 255.0f) luma = 255.0f;
            pl = (float)(int)luma / 255.0f;
            /* The rasterizer gets f2u(distance) >> 8 and splits it: exponent
             * (bits 23-30) as the integer part, the next 15 bits' log from log
             * RAM as the fraction. A zero distance gives texlod 0 (the oracle's
             * model2_v.cpp; stock MAME would pick the coarsest level). */
            float dist = g_geo_coef[at >> 27] * fabsf(dotp) * g_geo3d_lod;
            uint32_t db;
            memcpy(&db, &dist, 4);
            g_geo3d_emit_texlod = (db >> 8) == 0 ? 0.0f
                : (float)((int)((db >> 16) & 0x7F80u) - 0x3F80 + (int)g_geo_logram[(db >> 8) & 0x7FFFu]);
        } else if (g_light_enable && has_C) {
            float e1x=B.x-A.x, e1y=B.y-A.y, e1z=B.z-A.z;
            float e2x=C.x-A.x, e2y=C.y-A.y, e2z=C.z-A.z;
            float nx=e1y*e2z-e1z*e2y, ny=e1z*e2x-e1x*e2z, nz=e1x*e2y-e1y*e2x;
            float nl=sqrtf(nx*nx+ny*ny+nz*nz);
            float ll=sqrtf(g_light_dir[0]*g_light_dir[0]+g_light_dir[1]*g_light_dir[1]+g_light_dir[2]*g_light_dir[2]);
            float shade=g_light_ambient;
            if (nl>1e-6f && ll>1e-6f) {
                float d=(nx*g_light_dir[0]+ny*g_light_dir[1]+nz*g_light_dir[2])/(nl*ll);
                if (d<0) d=-d;
                shade += g_light_diffuse*d;
            }
            if (shade>1.0f) shade=1.0f;
            pl = shade;
        }

        /* Homebrew flat panels: the GEO brightens lit faces via the colorxlat luma
         * ramp (dark base hue + lit normal → bright panel). The shader's flat path
         * only darkens (×pl≤1), so the dark C_TUBE base never brightens. Approximate
         * the ramp by scaling the flat colour up with poly-luma, then neutralise the
         * shader's own ×pl so it isn't applied twice. */
        if (g_geo_flat_color) {
            float boost = pl * g_geo_flat_boost;
            fr = fminf(fr * boost, 1.0f);
            fg = fminf(fg * boost, 1.0f);
            fb = fminf(fb * boost, 1.0f);
            pl = 1.0f;
        }

        /* The untextured transparent renderer returns without writing a pixel
         * (model2rd.ipp draw_scanline_solid<true>). Only on the display-list
         * path, so the model tools keep comparing every face with the explorer. */
        if (g_geo3d_board_luma && (untex_trans || board_cull)) { efi++; continue; }
        float lbv = g_geo_flat_color ? -1.0f : (float)lumabase;

        if (is_tri) {
            if (has_C) {
                geo3d_emit_line(A.x,A.y,A.z, B.x,B.y,B.z, fr,fg,fb);
                geo3d_emit_line(B.x,B.y,B.z, C.x,C.y,C.z, fr,fg,fb);
                geo3d_emit_line(C.x,C.y,C.z, A.x,A.y,A.z, fr,fg,fb);
                geo3d_emit_tri_uv(A.x,A.y,A.z, uvu[0],uvv[0],
                                  B.x,B.y,B.z, uvu[1],uvv[1],
                                  C.x,C.y,C.z, uvu[2],uvv[2], fr,fg,fb, ftx,fty,ftw,fth, lbv,pl, ffl);
            } else {
                geo3d_emit_line(A.x,A.y,A.z, B.x,B.y,B.z, fr,fg,fb);
            }
        } else {
            /* Quad winding: A-B, B-D, D-C, C-A */
            geo3d_emit_line(A.x,A.y,A.z, B.x,B.y,B.z, fr,fg,fb);
            geo3d_emit_line(B.x,B.y,B.z, D.x,D.y,D.z, fr,fg,fb);
            geo3d_emit_line(D.x,D.y,D.z, C.x,C.y,C.z, fr,fg,fb);
            geo3d_emit_line(C.x,C.y,C.z, A.x,A.y,A.z, fr,fg,fb);
            /* Solid fill: ABD + ADC (slots 0,1,3 / 0,3,2), or ABC + BDC along
             * the other diagonal when a decal copy must match an earlier cut. */
            uint32_t kA = svk[ai], kB = svk[bi], kC = svk[ci], kD = svk[di];
            if (geo3d_split_other_way(kA ^ kB ^ kC ^ kD, kA ^ kD)) {
                geo3d_emit_tri_uv(A.x,A.y,A.z, uvu[0],uvv[0],
                                  B.x,B.y,B.z, uvu[1],uvv[1],
                                  C.x,C.y,C.z, uvu[2],uvv[2], fr,fg,fb, ftx,fty,ftw,fth, lbv,pl, ffl);
                geo3d_emit_tri_uv(B.x,B.y,B.z, uvu[1],uvv[1],
                                  D.x,D.y,D.z, uvu[3],uvv[3],
                                  C.x,C.y,C.z, uvu[2],uvv[2], fr,fg,fb, ftx,fty,ftw,fth, lbv,pl, ffl);
            } else {
                geo3d_emit_tri_uv(A.x,A.y,A.z, uvu[0],uvv[0],
                                  B.x,B.y,B.z, uvu[1],uvv[1],
                                  D.x,D.y,D.z, uvu[3],uvv[3], fr,fg,fb, ftx,fty,ftw,fth, lbv,pl, ffl);
                geo3d_emit_tri_uv(A.x,A.y,A.z, uvu[0],uvv[0],
                                  D.x,D.y,D.z, uvu[3],uvv[3],
                                  C.x,C.y,C.z, uvu[2],uvv[2], fr,fg,fb, ftx,fty,ftw,fth, lbv,pl, ffl);
            }
        }
        efi++;   /* this face was emitted → consumes one material record */
    }
    g_geo3d_emit_texlod = GEO3D_TEXLOD_NONE;
    g_geo3d_emit_zs     = GEO3D_ZSORT_NONE;
}

/* ---- Mesh cache -------------------------------------------------------------
 * Most of geo3d_decode_model's work for a display-list object does not depend
 * on the instance: the vertex pairs out of ROM, the strip topology, the corner
 * keys that pick a quad's cut, and — while the material and UV streams live in
 * ROM — every face's texture header and UVs, and with them which faces the
 * board skips. That part is decoded once per (model, material, UV) and kept.
 * Drawing an instance replays the faces that can emit: the matrix transform,
 * the lighting and the board's back-face / link-type cull, the face colour
 * from live palette RAM and the quad cuts, in the original order with the
 * original arithmetic, so the triangles come out bit for bit the same
 * (arc_bench --draw-digest with and without the cache).
 *
 * Not cached (the decoder runs as before): meshes in polygon RAM, materials or
 * UVs in texture RAM, no matrix, the homebrew flat-colour mode, the UV debug
 * dials and the texture dump. The wireframe lines are only built when they
 * will be drawn. */

#define GEO3D_MESH_CACHE_SLOTS 4096u   /* power of two */

typedef struct {
    int32_t  ai, bi, ci, di;
    uint8_t  is_tri, has_c, has_qn, mat_ok;
    uint32_t qa;                 /* attribute word: texparam slot in bits 18..22 */
    int32_t  zsrc[4];            /* the corners the board sorts this polygon by */
    uint32_t zmode;              /* attribute bits 10..11, carried (geo3d_sort_z) */
    uint32_t split_quad, split_cut;
    uint32_t matidx;             /* colorbase, when mat_ok */
    vec3_t   qn;                 /* the record's normal, Z negated */
    float    tx, ty, tw, th, lb, fl;
    float    uvu[4], uvv[4];
} geo3d_cface_t;

typedef struct {
    bool           used;
    int            model_idx;
    uint32_t       mat_ptr, uv_ptr;
    const uint8_t *polygons, *materials, *main_data;
    size_t         polygons_size, materials_size;
    uint32_t       table_off, table_count, mesh_ptr_subtract, mesh_ptr_add;
    int            n_sv, n_faces;
    vec3_t        *sv;           /* untransformed, Z negated */
    geo3d_cface_t *faces;        /* only the faces that emit, in decode order */
} geo3d_cmesh_t;

static int           g_geo3d_mesh_cache = 1;   /* 0: always run the full decoder */
static geo3d_cmesh_t g_geo3d_meshes[GEO3D_MESH_CACHE_SLOTS];
static unsigned      g_geo3d_mesh_count;
static uint64_t      g_geo3d_mesh_hits, g_geo3d_mesh_builds;

static inline void geo3d_mesh_cache_clear(void) {
    for (unsigned i = 0; i < GEO3D_MESH_CACHE_SLOTS; i++) {
        free(g_geo3d_meshes[i].sv);
        free(g_geo3d_meshes[i].faces);
    }
    memset(g_geo3d_meshes, 0, sizeof g_geo3d_meshes);
    g_geo3d_mesh_count = 0;
}

/* The static half of geo3d_decode_model for one (model, material, UV): same
 * loops, same limits, no matrix. Returns false if out of memory. */
static inline bool geo3d_mesh_build(geo3d_cmesh_t *m, uint32_t mesh_offset,
                                    bool have_mat, bool have_uv) {
    static vec3_t   sv[GEO3D_IA_MAX_VERTS];
    static uint32_t svk[GEO3D_IA_MAX_VERTS];
    static int      qt[GEO3D_IA_MAX_VPS];
    static vec3_t   qn[GEO3D_IA_MAX_VPS];
    static uint32_t qa[GEO3D_IA_MAX_VPS];
    static int      idx[GEO3D_IA_MAX_IDX];
    static geo3d_cface_t faces[GEO3D_IA_MAX_IDX / 4];
    const uint8_t *polygons = m->polygons, *materials = m->materials;

    int n_sv = 0, n_qt = 0, n_idx = 4, vcount = 0;
    idx[0] = 0; idx[1] = 1; idx[2] = 2; idx[3] = 3;
    while (vcount < GEO3D_IA_MAX_VPS) {
        if ((size_t)mesh_offset + VERTEX_PAIR_SIZE > m->polygons_size) break;
        if (n_sv + 2 > GEO3D_IA_MAX_VERTS || n_idx + 4 > GEO3D_IA_MAX_IDX) break;
        const uint8_t *vp = polygons + mesh_offset;
        bool is_end = (vp[24] == 0 && vp[25] == 0 && vp[26] == 0 && vp[27] == 0);
        vec3_t v1 = { read_float_le(vp + 0),  read_float_le(vp + 4),  -read_float_le(vp + 8) };
        vec3_t v2 = { read_float_le(vp + 12), read_float_le(vp + 16), -read_float_le(vp + 20) };
        uint8_t f1 = vp[24], iflag = vp[25] & 0x03;
        svk[n_sv] = geo3d_corner_key(v1);
        svk[n_sv + 1] = geo3d_corner_key(v2);
        sv[n_sv++] = v1;
        sv[n_sv++] = v2;
        qn[n_qt] = (vec3_t){ read_float_le(vp + 28), read_float_le(vp + 32), -read_float_le(vp + 36) };
        qa[n_qt] = read_u32_le(vp + 24);
        qt[n_qt++] = f1;
        int new_a = 2 * (vcount + 2);
        switch (iflag) {
            case 0:
                idx[n_idx - 4] = -1; idx[n_idx - 3] = -1; idx[n_idx - 2] = -1; idx[n_idx - 1] = -1;
                idx[n_idx++] = new_a - 2; idx[n_idx++] = new_a - 1; idx[n_idx++] = new_a; idx[n_idx++] = new_a + 1;
                break;
            case 1: {
                int p3 = idx[n_idx - 4], p2 = idx[n_idx - 2];
                idx[n_idx++] = p3; idx[n_idx++] = p2; idx[n_idx++] = new_a; idx[n_idx++] = new_a + 1;
                break;
            }
            case 2:
                idx[n_idx++] = new_a - 2; idx[n_idx++] = new_a - 1; idx[n_idx++] = new_a; idx[n_idx++] = new_a + 1;
                break;
            case 3: {
                int anc_a = (f1 == 1) ? idx[n_idx - 1] : idx[n_idx - 2];
                int anc_b = idx[n_idx - 3];
                idx[n_idx++] = anc_a; idx[n_idx++] = anc_b; idx[n_idx++] = new_a; idx[n_idx++] = new_a + 1;
                break;
            }
        }
        if (is_end) break;
        mesh_offset += VERTEX_PAIR_SIZE;
        vcount++;
    }

    uint32_t mat_word = m->mat_ptr, uv_word = m->uv_ptr;
    int n_faces = 0, efi = 0;
    int zsrc[4] = {0,0,0,0}; uint32_t zmode = 0u; bool zset = false;
    for (int i = 0; i < n_idx - 8; i += 4) {
        int fi = i / 4;
        int ai = idx[i], bi = idx[i + 1], ci = idx[i + 2], di = idx[i + 3];
        bool tri_cnt = (fi < n_qt && qt[fi] == 2);
        int  nv = tri_cnt ? 3 : 4;

        uint32_t texx = 0, texy = 0, texw = 32, texh = 32, texsheet = 0, lumabase = 0, fflags = 0, matidx = 0;
        bool textured = false, untex_trans = false, mat_ok = false;
        if (have_mat) {
            uint32_t hw = mat_word + (uint32_t)efi * 4u;
            uint16_t th0 = 0, th1 = 0, th2 = 0, th3 = 0;
            if (geo3d_tex_word(materials, m->materials_size, hw, &th0) &&
                geo3d_tex_word(materials, m->materials_size, hw + 1u, &th1) &&
                geo3d_tex_word(materials, m->materials_size, hw + 2u, &th2) &&
                geo3d_tex_word(materials, m->materials_size, hw + 3u, &th3)) {
                lumabase = (uint32_t)(th1 & 0xff) << 7;
                textured = (th0 & 0x4000) != 0;
                texw = 32u << (th0 & 0x7);
                texh = 32u << ((th0 >> 3) & 0x7);
                texx = 32u * (th2 & 0x3f);
                texy = 32u * ((th2 >> 6) & 0x1f);
                texsheet = (th2 >> 12) & 1u;
                if (textured && (th0 & 0x2000)) fflags |= GEO3D_FACE_TRANSPARENT;
                untex_trans = !textured && (th0 & 0x2000);
                if (th0 & 0x8000)               fflags |= GEO3D_FACE_CHECKER;
                if (texsheet)                   fflags |= GEO3D_FACE_SHEET1;
                if ((th0 >> 8) & 1)             fflags |= GEO3D_FACE_MIRROR_X;
                if ((th0 >> 9) & 1)             fflags |= GEO3D_FACE_MIRROR_Y;
                matidx = (th3 >> 6) & 0x3ff;
                mat_ok = true;
            }
        }
        float uvu[4] = {0,0,0,0}, uvv[4] = {0,0,0,0};
        if (have_uv) {
            static const int quad_slot[4] = {1,0,2,3};
            static const int tri_slot[3]  = {1,0,2};
            const int *slot = tri_cnt ? tri_slot : quad_slot;
            for (int k = 0; k < nv; k++) {
                uint32_t tw_ = uv_word + (uint32_t)k * 2u;
                uint16_t pv = 0, pu = 0;
                if (!geo3d_tex_word(materials, m->materials_size, tw_, &pv) ||
                    !geo3d_tex_word(materials, m->materials_size, tw_ + 1u, &pu)) break;
                if (!textured) continue;
                uvu[slot[k]] = (float)pu / 8.0f;
                uvv[slot[k]] = (float)pv / 8.0f;
            }
        }
        uv_word += (uint32_t)nv * 2u;

        if (ai < 0 || ai >= n_sv) continue;
        if (bi < 0 || bi >= n_sv) continue;
        bool has_c = (ci >= 0 && ci < n_sv), has_d = (di >= 0 && di < n_sv);
        /* Before the skip below, not after: the z-sort state is carried across
         * every face of the walk, including the ones nothing is drawn for. */
        geo3d_zsort_step((fi < n_qt) ? qa[fi] : 0u, tri_cnt || !has_c || !has_d, has_c,
                         ai, bi, ci, di, zsrc, &zmode, &zset);
        if (untex_trans) { efi++; continue; }   /* the board draws nothing for it */

        geo3d_cface_t *f = &faces[n_faces++];
        memset(f, 0, sizeof *f);
        f->ai = ai; f->bi = bi; f->ci = ci; f->di = di;
        f->is_tri = tri_cnt || !has_c || !has_d;
        f->has_c  = has_c;
        f->has_qn = fi < n_qt;
        f->qn     = f->has_qn ? qn[fi] : (vec3_t){0, 0, 0};
        f->qa     = f->has_qn ? qa[fi] : 0;
        f->zmode  = zmode;
        for (int z = 0; z < 4; z++) f->zsrc[z] = zsrc[z];
        f->mat_ok = mat_ok;
        f->matidx = matidx;
        if (!f->is_tri) { f->split_quad = svk[ai] ^ svk[bi] ^ svk[ci] ^ svk[di]; f->split_cut = svk[ai] ^ svk[di]; }
        f->tx = (float)texx;
        f->ty = (float)((uint32_t)texsheet * GEO3D_SHEET_H + texy);
        f->tw = textured ? (float)texw : 0.0f;
        f->th = (float)texh;
        f->lb = (float)lumabase;
        f->fl = (float)fflags;
        memcpy(f->uvu, uvu, sizeof uvu);
        memcpy(f->uvv, uvv, sizeof uvv);
        efi++;
    }

    m->sv    = malloc((size_t)(n_sv ? n_sv : 1) * sizeof(vec3_t));
    m->faces = malloc((size_t)(n_faces ? n_faces : 1) * sizeof(geo3d_cface_t));
    if (!m->sv || !m->faces) { free(m->sv); free(m->faces); m->sv = NULL; m->faces = NULL; return false; }
    memcpy(m->sv, sv, (size_t)n_sv * sizeof(vec3_t));
    memcpy(m->faces, faces, (size_t)n_faces * sizeof(geo3d_cface_t));
    m->n_sv = n_sv;
    m->n_faces = n_faces;
    return true;
}

/* geo3d_decode_model through the cache when the object allows it. */
static inline void geo3d_decode_model_cached(int model_idx,
                                             const uint8_t *main_data, size_t main_data_size,
                                             const uint8_t *polygons,  size_t polygons_size,
                                             const uint8_t *materials, size_t materials_size,
                                             uint32_t table_off, uint32_t table_count,
                                             uint32_t mesh_ptr_subtract, uint32_t mesh_ptr_add,
                                             const float *matrix,
                                             float cr, float cg, float cb) {
    #define GEO3D_FULL_DECODE() geo3d_decode_model(model_idx, main_data, main_data_size, polygons, polygons_size, \
        materials, materials_size, table_off, table_count, mesh_ptr_subtract, mesh_ptr_add, matrix, cr, cg, cb)
    if (!g_geo3d_mesh_cache || !g_geo3d_board_luma || !matrix || g_geo3d_obj_mesh || g_geo_flat_color ||
            g_uv_bank_mode || g_uv_quad_order || g_uv_swap || g_uv_flip_u || g_uv_flip_v ||
            GEO3D_DUMP_TEX(model_idx)) {
        GEO3D_FULL_DECODE();
        return;
    }
    if (!main_data || !polygons) return;
    if (model_idx < 0 || (uint32_t)model_idx >= table_count) return;
    uint32_t toff = table_off + (uint32_t)model_idx * MODEL_ENTRY_SIZE;
    if ((size_t)toff + MODEL_ENTRY_SIZE > main_data_size) return;
    uint32_t mesh_ptr_raw = read_u32_le(main_data + toff + 8);
    if (mesh_ptr_raw == 0) return;
    uint32_t mesh_offset = mesh_ptr_raw * 4u - mesh_ptr_subtract + mesh_ptr_add;
    uint32_t mat_ptr = 0, uv_ptr = 0;
    if (materials) {
        mat_ptr = read_u32_le(main_data + toff + 4);
        uv_ptr  = read_u32_le(main_data + toff + 0);
        if (g_geo3d_obj_tha != 0xFFFFFFFFu) mat_ptr = g_geo3d_obj_tha;
        if (g_geo3d_obj_tpa != 0xFFFFFFFFu) uv_ptr  = g_geo3d_obj_tpa;
    }
    bool have_mat = mat_ptr != 0, have_uv = uv_ptr != 0;
    /* Streams in texture RAM change under the cache: decode those every time. */
    if ((have_mat && (mat_ptr & 0x800000u)) || (have_uv && (uv_ptr & 0x800000u))) {
        GEO3D_FULL_DECODE();
        return;
    }
    #undef GEO3D_FULL_DECODE

    uint32_t h = ((uint32_t)model_idx * 2654435761u) ^ (mat_ptr * 40503u) ^ (uv_ptr * 2246822519u);
    geo3d_cmesh_t *m = NULL;
    for (uint32_t probe = 0; probe < GEO3D_MESH_CACHE_SLOTS; probe++) {
        geo3d_cmesh_t *e = &g_geo3d_meshes[(h + probe) & (GEO3D_MESH_CACHE_SLOTS - 1u)];
        if (!e->used) { m = e; break; }
        if (e->model_idx == model_idx && e->mat_ptr == mat_ptr && e->uv_ptr == uv_ptr &&
                e->polygons == polygons && e->materials == materials && e->main_data == main_data &&
                e->polygons_size == polygons_size && e->materials_size == materials_size &&
                e->table_off == table_off && e->table_count == table_count &&
                e->mesh_ptr_subtract == mesh_ptr_subtract && e->mesh_ptr_add == mesh_ptr_add) {
            m = e;
            break;
        }
    }
    if (!m || !m->used) {
        if (!m || g_geo3d_mesh_count >= GEO3D_MESH_CACHE_SLOTS * 3u / 4u) {
            geo3d_mesh_cache_clear();
            m = &g_geo3d_meshes[h & (GEO3D_MESH_CACHE_SLOTS - 1u)];
        }
        *m = (geo3d_cmesh_t){ .model_idx = model_idx, .mat_ptr = mat_ptr, .uv_ptr = uv_ptr,
                              .polygons = polygons, .materials = materials, .main_data = main_data,
                              .polygons_size = polygons_size, .materials_size = materials_size,
                              .table_off = table_off, .table_count = table_count,
                              .mesh_ptr_subtract = mesh_ptr_subtract, .mesh_ptr_add = mesh_ptr_add };
        if (!geo3d_mesh_build(m, mesh_offset, have_mat, have_uv)) {
            m->used = false;
            geo3d_decode_model(model_idx, main_data, main_data_size, polygons, polygons_size, materials,
                               materials_size, table_off, table_count, mesh_ptr_subtract, mesh_ptr_add,
                               matrix, cr, cg, cb);
            return;
        }
        m->used = true;
        g_geo3d_mesh_count++;
        g_geo3d_mesh_builds++;
    } else {
        g_geo3d_mesh_hits++;
    }

    static vec3_t tv[GEO3D_IA_MAX_VERTS];
    for (int i = 0; i < m->n_sv; i++) tv[i] = apply_matrix(m->sv[i], matrix);

    geo3d_split_reset();
    bool lines = g_geo_wireframe != 0;
    const bool standing = geo3d_model_standing(model_idx);
    for (int n = 0; n < m->n_faces; n++) {
        const geo3d_cface_t *f = &m->faces[n];
        vec3_t A = tv[f->ai], B = tv[f->bi];
        vec3_t C = f->has_c ? tv[f->ci] : (vec3_t){0, 0, 0};
        vec3_t D = f->is_tri ? (vec3_t){0, 0, 0} : tv[f->di];

        g_geo3d_emit_zs = standing ? GEO3D_ZSORT_NONE : geo3d_sort_z(tv, f->zsrc, f->zmode);

        float fr = cr, fg = cg, fb = cb;
        if (f->mat_ok) {
            uint32_t pal = GEO3D_PALETTE_OFF + f->matidx * 2u;
            uint32_t ram = (f->matidx + 0x1000u) * 2u;
            if (g_geo3d_palram && (size_t)ram + 2 <= g_geo3d_palram_size) {
                uint16_t cw = (uint16_t)(g_geo3d_palram[ram] | (g_geo3d_palram[ram + 1] << 8)) & 0x7FFF;
                geo3d_bgr555(cw, &fr, &fg, &fb);
            } else if ((size_t)pal + 2 <= main_data_size) {
                uint16_t cw = (uint16_t)main_data[pal] | ((uint16_t)main_data[pal + 1] << 8);
                geo3d_bgr555(cw, &fr, &fg, &fb);
            }
        }

        /* The board's lighting and culling, as geo3d_decode_model does them on
         * the display-list path (board luma with a matrix: always this branch). */
        vec3_t fn = f->qn;
        float nx = matrix[0]*fn.x + matrix[1]*fn.y + matrix[2]*fn.z;
        float ny = matrix[4]*fn.x + matrix[5]*fn.y + matrix[6]*fn.z;
        float nz = matrix[8]*fn.x + matrix[9]*fn.y + matrix[10]*fn.z;
        float dotl = nx*g_light_dir[0] + ny*g_light_dir[1] + nz*g_light_dir[2];
        float dotp = nx*A.x + ny*A.y + nz*A.z;
        float lum  = (dotl * dotp < 0.0f) ? 0.0f : fabsf(dotl);
        uint32_t at = f->has_qn ? f->qa : 0u;
        if ((((at >> 17) & 1u) == 0 && dotp < 0.0f) || ((at >> 8) & 3u) == 0) continue;   /* board_cull */
        const float *tp = g_geo_texparam[(at >> 18) & 0x1F];
        /* Specular, the truncated luma and the texlod belong to the instance
         * (the list's mode word and LOD scale, the eye-space normal), so they
         * are worked out here per draw and never kept in the mesh. */
        float spec = 0.0f;
        if (g_geo3d_mode & 1u) {
            uint32_t ctl = (uint32_t)tp[3];
            spec = g_light_dir[2] - 2.0f * dotl * nz;
            if (spec < 0.0f || ctl == 0) spec = 0.0f;
            if ((ctl >> 1) != 0) spec *= spec;
            if ((ctl >> 2) != 0) spec *= spec;
            if (((ctl + 1) >> 3) != 0) spec *= spec;
            spec *= tp[2];
        }
        float luma = lum * tp[0] + tp[1] + spec;
        if (luma < 0.0f) luma = 0.0f;
        if (luma > 255.0f) luma = 255.0f;
        float pl = (float)(int)luma / 255.0f;
        float dist = g_geo_coef[at >> 27] * fabsf(dotp) * g_geo3d_lod;
        uint32_t db;
        memcpy(&db, &dist, 4);
        g_geo3d_emit_texlod = (db >> 8) == 0 ? 0.0f
            : (float)((int)((db >> 16) & 0x7F80u) - 0x3F80 + (int)g_geo_logram[(db >> 8) & 0x7FFFu]);

        if (f->is_tri) {
            if (f->has_c) {
                if (lines) {
                    geo3d_emit_line(A.x,A.y,A.z, B.x,B.y,B.z, fr,fg,fb);
                    geo3d_emit_line(B.x,B.y,B.z, C.x,C.y,C.z, fr,fg,fb);
                    geo3d_emit_line(C.x,C.y,C.z, A.x,A.y,A.z, fr,fg,fb);
                }
                geo3d_emit_tri_uv(A.x,A.y,A.z, f->uvu[0],f->uvv[0],
                                  B.x,B.y,B.z, f->uvu[1],f->uvv[1],
                                  C.x,C.y,C.z, f->uvu[2],f->uvv[2], fr,fg,fb, f->tx,f->ty,f->tw,f->th, f->lb,pl, f->fl);
            } else if (lines) {
                geo3d_emit_line(A.x,A.y,A.z, B.x,B.y,B.z, fr,fg,fb);
            }
        } else {
            if (lines) {
                geo3d_emit_line(A.x,A.y,A.z, B.x,B.y,B.z, fr,fg,fb);
                geo3d_emit_line(B.x,B.y,B.z, D.x,D.y,D.z, fr,fg,fb);
                geo3d_emit_line(D.x,D.y,D.z, C.x,C.y,C.z, fr,fg,fb);
                geo3d_emit_line(C.x,C.y,C.z, A.x,A.y,A.z, fr,fg,fb);
            }
            if (geo3d_split_other_way(f->split_quad, f->split_cut)) {
                geo3d_emit_tri_uv(A.x,A.y,A.z, f->uvu[0],f->uvv[0],
                                  B.x,B.y,B.z, f->uvu[1],f->uvv[1],
                                  C.x,C.y,C.z, f->uvu[2],f->uvv[2], fr,fg,fb, f->tx,f->ty,f->tw,f->th, f->lb,pl, f->fl);
                geo3d_emit_tri_uv(B.x,B.y,B.z, f->uvu[1],f->uvv[1],
                                  D.x,D.y,D.z, f->uvu[3],f->uvv[3],
                                  C.x,C.y,C.z, f->uvu[2],f->uvv[2], fr,fg,fb, f->tx,f->ty,f->tw,f->th, f->lb,pl, f->fl);
            } else {
                geo3d_emit_tri_uv(A.x,A.y,A.z, f->uvu[0],f->uvv[0],
                                  B.x,B.y,B.z, f->uvu[1],f->uvv[1],
                                  D.x,D.y,D.z, f->uvu[3],f->uvv[3], fr,fg,fb, f->tx,f->ty,f->tw,f->th, f->lb,pl, f->fl);
                geo3d_emit_tri_uv(A.x,A.y,A.z, f->uvu[0],f->uvv[0],
                                  D.x,D.y,D.z, f->uvu[3],f->uvv[3],
                                  C.x,C.y,C.z, f->uvu[2],f->uvv[2], fr,fg,fb, f->tx,f->ty,f->tw,f->th, f->lb,pl, f->fl);
            }
        }
    }
    g_geo3d_emit_texlod = GEO3D_TEXLOD_NONE;
    g_geo3d_emit_zs     = GEO3D_ZSORT_NONE;
}

#ifdef M2HLE_DEBUG_DUMPS   /* desktop only; see GEO3D_DUMP_TEX */
/* ---- Programmatic per-model texture extractor -------------------------------
 * Pulls a model's texture data straight from ROM — no MAME memory capture for
 * the descriptors.  Walks the per-face material records (8 bytes each) at
 * mat_ptr*2 in the textures ROM, decoding the SAME fields the renderer uses:
 * tile rect, texsheet (bank), colorbase, and the flat BGR555 colour from the
 * main_data palette (GEO3D_PALETTE_OFF + colorbase*2).  Each distinct textured
 * tile is copied from the CORRECT bank — texsheet 0 → texram0, 1 → texram1.
 * Face count comes from the model-table mat_ptr delta to the next entry
 * (8-byte records → (mat_next - mat_ptr)/4 faces).
 *   Writes  model_<N>_tex.txt  (manifest: per-face + tile list + colours)
 *           model_<N>_tile_<i>_s<bank>_<W>x<H>_<X>_<Y>.bin  (4-bit luma tiles) */
static int g_extract_model   = -1;   /* set via --extract N; run from the main loop */
static int g_extract_seq     = -1;   /* >=0: append seqNNN to filenames (map cycle) */
static int g_extract_rombank = -1;   /* --rombank N: read texels from the static 16MB
                                      * textures ROM bank N (N*0x100000) instead of
                                      * runtime texram. -1 = use live texram0/1. */

static void geo3d_extract_model_texture(int model_idx,
        const uint8_t *main_data, size_t main_data_size,
        const uint8_t *materials, size_t materials_size,
        const uint8_t *texram0,   const uint8_t *texram1,
        uint32_t table_off, uint32_t table_count) {
    if (!main_data || !materials || model_idx < 0 ||
        (uint32_t)(model_idx + 1) >= table_count) return;
    uint32_t toff = table_off + (uint32_t)model_idx * MODEL_ENTRY_SIZE;
    if ((size_t)toff + 2u * MODEL_ENTRY_SIZE > main_data_size) return;
    uint32_t uv_ptr   = read_u32_le(main_data + toff + 0);
    uint32_t mat_ptr  = read_u32_le(main_data + toff + 4);
    uint32_t mesh_ptr = read_u32_le(main_data + toff + 8);
    uint32_t mat_next = read_u32_le(main_data + toff + MODEL_ENTRY_SIZE + 4);
    if (mat_next <= mat_ptr) return;
    uint32_t nfaces = (mat_next - mat_ptr) / 4u;   /* 8-byte (4 u16) records/face */
    if (nfaces > 8192u) nfaces = 8192u;
    uint32_t mat_base = mat_ptr * 2u;

    struct { uint32_t s, x, y, w, h; } tiles[256]; int nt = 0;
    char path[160];
    snprintf(path, sizeof path, "model_%d_tex.txt", model_idx);
    FILE *mf = fopen(path, "w");
    if (mf) fprintf(mf, "# model %d  uv_ptr=%u mat_ptr=%u mesh_ptr=%u  faces=%u\n",
                    model_idx, uv_ptr, mat_ptr, mesh_ptr, nfaces);

    for (uint32_t f = 0; f < nfaces; f++) {
        uint32_t rec = mat_base + f * 8u;
        if ((size_t)rec + 8 > materials_size) break;
        uint16_t th0 = (uint16_t)materials[rec+0] | ((uint16_t)materials[rec+1] << 8);
        uint16_t th2 = (uint16_t)materials[rec+4] | ((uint16_t)materials[rec+5] << 8);
        uint16_t th3 = (uint16_t)materials[rec+6] | ((uint16_t)materials[rec+7] << 8);
        int      textured = (th0 & 0x4000) != 0;
        uint32_t w  = 32u << (th0 & 7u), h = 32u << ((th0 >> 3) & 7u);
        uint32_t x  = 32u * (th2 & 0x3fu), y = 32u * ((th2 >> 6) & 0x1fu);
        uint32_t s  = (th2 >> 12) & 1u;          /* texture bank (texsheet) */
        if      (g_uv_bank_mode == 1) s = 0u;    /* --bank override: force sheet 0 */
        else if (g_uv_bank_mode == 2) s = 1u;    /*                  force sheet 1 */
        else if (g_uv_bank_mode == 3) s ^= 1u;   /*                  swap banks    */
        uint32_t cb = (th3 >> 6) & 0x3ffu;       /* colorbase */
        uint32_t pal = GEO3D_PALETTE_OFF + cb * 2u;
        uint16_t col = ((size_t)pal + 2 <= main_data_size)
                       ? ((uint16_t)main_data[pal] | ((uint16_t)main_data[pal+1] << 8)) : 0;
        if (mf) fprintf(mf,
            "face %4u th0=%04X th2=%04X th3=%04X textured=%d bank=%u tile=(%u,%u) %ux%u colorbase=%u color=%04X\n",
            f, th0, th2, th3, textured, s, x, y, w, h, cb, col);
        if (textured) {
            int hit = -1;
            for (int t = 0; t < nt; t++)
                if (tiles[t].s==s && tiles[t].x==x && tiles[t].y==y &&
                    tiles[t].w==w && tiles[t].h==h) { hit = t; break; }
            if (hit < 0 && nt < 256) {
                tiles[nt].s=s; tiles[nt].x=x; tiles[nt].y=y; tiles[nt].w=w; tiles[nt].h=h; nt++;
            }
        }
    }
    if (mf) fprintf(mf, "# %d distinct textured tiles\n", nt);

    for (int t = 0; t < nt; t++) {
        const uint8_t *bank;
        if (g_extract_rombank >= 0) {
            /* static source: bank N of the 16MB textures ROM (1MB sheet each) */
            size_t bo = (size_t)g_extract_rombank * 0x100000u;
            bank = (bo + 0x100000u <= materials_size) ? (materials + bo) : NULL;
        } else {
            bank = tiles[t].s ? texram1 : texram0;   /* live texram, correct bank */
        }
        if (!bank) continue;
        const uint32_t *sheet = (const uint32_t *)bank;
        uint32_t tx = tiles[t].x, ty = tiles[t].y, tw = tiles[t].w, th = tiles[t].h;
        /* Binary PGM (P5) grayscale: each 4-bit luma texel -> 0..255, decoded with
         * the EXACT texram swizzle the renderer uses (16-bit halfword = 2x2 nibble
         * block; x>=1024 folds to y^=1024). Directly viewable / convertible. */
        if (g_extract_seq >= 0)
            snprintf(path, sizeof path, "model_%d_seq%03d_tile_%d_s%u_%ux%u_%u_%u.pgm",
                     model_idx, g_extract_seq, t, tiles[t].s, tw, th, tx, ty);
        else
            snprintf(path, sizeof path, "model_%d_tile_%d_s%u_%ux%u_%u_%u.pgm",
                     model_idx, t, tiles[t].s, tw, th, tx, ty);
        FILE *tf = fopen(path, "wb");
        if (!tf) continue;
        fprintf(tf, "P5\n%u %u\n255\n", tw, th);
        for (uint32_t y = ty; y < ty + th; y++) {
            for (uint32_t x = tx; x < tx + tw; x++) {
                uint32_t x2 = x, y2 = y;
                if (x2 >= 1024u) { x2 -= 1024u; y2 ^= 1024u; }
                uint32_t off  = (y2 / 2u) * 512u + (x2 / 2u);
                uint32_t word = sheet[off >> 1];
                if (off & 1u)       word >>= 16;
                if ((y & 1u) == 0u) word >>= 8;
                if ((x & 1u) == 0u) word >>= 4;
                fputc((int)((word & 0xfu) * 17u), tf);
            }
        }
        fclose(tf);
        if (mf) fprintf(mf, "# tile %d bank=%u (%u,%u) %ux%u -> %s\n",
                        t, tiles[t].s, tx, ty, tw, th, path);
    }
    if (mf) fclose(mf);
    LOG_INFO("geo3d_extract_model_texture: model %d  %u faces  %d tiles", model_idx, nfaces, nt);
}
#endif /* M2HLE_DEBUG_DUMPS */

/* ---- Build wireframes for the current capture list --------------------- */

/*
 * Clears the line buffer and decodes every captured model into it (respecting
 * the geo->filter_enabled and geo->isolate_index filters).  If
 * geo->test_triangle is set, emits a single hard-coded triangle for sanity.
 */
/* Compute a lerped 3×4 matrix from cur+prev, blending only translation
 * (columns 0–2 are rotation/scale; column 3 is translation Tx/Ty/Tz).
 * lerp_t = 0 → previous frame position, 1 → current frame position. */
static inline void geo3d_lerp_matrix(float *out, const float *cur, const float *prev,
                                      float lerp_t) {
    memcpy(out, cur, 12 * sizeof(float));
    out[3]  = prev[3]  + (cur[3]  - prev[3])  * lerp_t;
    out[7]  = prev[7]  + (cur[7]  - prev[7])  * lerp_t;
    out[11] = prev[11] + (cur[11] - prev[11]) * lerp_t;
}

static inline void geo3d_build_wireframes(geo3d_state_t *geo,
                                           const uint8_t *main_data, size_t main_data_size,
                                           const uint8_t *polygons,  size_t polygons_size,
                                           const uint8_t *materials, size_t materials_size,
                                           uint32_t table_off, uint32_t table_count,
                                           uint32_t mesh_ptr_subtract, uint32_t mesh_ptr_add,
                                           float lerp_t,
                                           float cam_x, float cam_y, float cam_z) {
    /* A bridge model dump owns the emit sink for the moment; leave the scene as
     * it stands rather than resetting a buffer this frame will not refill. */
    if (g_geo3d_dump_busy) return;
    geo3d_lines_reset();
    geo3d_tris_reset();
    if (!geo->enabled) return;
    (void)cam_x; (void)cam_y; (void)cam_z;

    if (geo->test_triangle) {
        geo3d_emit_line( 0.0f,  1.0f, 0.0f,  -1.0f, -1.0f, 0.0f,  1,1,1);
        geo3d_emit_line(-1.0f, -1.0f, 0.0f,   1.0f, -1.0f, 0.0f,  1,1,1);
        geo3d_emit_line( 1.0f, -1.0f, 0.0f,   0.0f,  1.0f, 0.0f,  1,1,1);
        return;
    }

    if (geo->use_captures) {
        for (int i = 0; i < geo->captured_count; i++) {
            if (geo->isolate_index >= 0) {
                if (i != geo->isolate_index) continue;
            } else if (geo->filter_enabled) {
                if (i < geo->filter_min || i > geo->filter_max) continue;
            }
            const captured_model_t *cm = &geo->captured[i];
            const float *mat = (geo->use_matrix && cm->has_matrix) ? cm->matrix : NULL;

            /* Interpolate translation with the previous frame if available. */
            float lerped[12];
            if (mat && lerp_t > 0.0f && lerp_t < 1.0f
                    && i < geo->captured_prev_count
                    && geo->captured_prev[i].has_matrix
                    && geo->captured_prev[i].model_idx == cm->model_idx) {
                geo3d_lerp_matrix(lerped, mat, geo->captured_prev[i].matrix, lerp_t);
                mat = lerped;
            }

            geo3d_decode_model(cm->model_idx,
                                main_data, main_data_size,
                                polygons, polygons_size,
                                materials, materials_size,
                                table_off, table_count,
                                mesh_ptr_subtract, mesh_ptr_add,
                                mat, cm->color[0], cm->color[1], cm->color[2]);
        }
    } else {
        /* Single-model browser: derive colour from the model-table material ptr. */
        float cr = 0.0f, cg = 1.0f, cb = 0.0f;
        uint32_t toff = table_off + (uint32_t)geo->model_index * MODEL_ENTRY_SIZE;
        if (main_data && (size_t)toff + MODEL_ENTRY_SIZE <= main_data_size) {
            uint32_t mat_ptr = read_u32_le(main_data + toff + 4);
            material_ptr_to_color(mat_ptr, &cr, &cg, &cb);
        }
        geo3d_decode_model(geo->model_index,
                            main_data, main_data_size,
                            polygons, polygons_size,
                            materials, materials_size,
                            table_off, table_count,
                            mesh_ptr_subtract, mesh_ptr_add,
                            NULL, cr, cg, cb);
    }
}

/* ---- Game camera --------------------------------------------------------- */

/*
 * Read the game's camera struct from RAM and update geo->cam_x/y/z/rot_x/rot_y.
 * Camera struct layout (STF, board-level convention per CLAUDE.md):
 *   +0x00  float  xpos
 *   +0x04  float  ypos
 *   +0x08  float  zpos
 *   +0x0C  int16  xang16  (fixed-point, 0x10000 = 360 deg)
 *   +0x0E  int16  yang16  (negated — hardware stores with opposite sign)
 *
 * cam_z is negated (Z-negation convention).  yang16 is negated (left-handed
 * hardware stores Y-rotation with opposite sign vs. our right-handed renderer).
 */
static inline void geo3d_read_game_view(geo3d_state_t *geo,
                                         memory_bus_t *bus,
                                         uint32_t cam_addr,
                                         uint32_t ang_addr) {
    if (!cam_addr || !bus) return;
    /* The camera angle word layout differs per game. STF packs it at eye+0xC;
     * FV keeps it separate (yaw = high16 @0x515584). ang_addr from the profile;
     * 0 falls back to the STF-style eye+0xC. */
    if (!ang_addr) ang_addr = cam_addr + 0x0C;

#define GEO3D_READ_F32(addr) do { \
        uint32_t _u = mem_read32(bus, (addr)); \
        float _f; memcpy(&_f, &_u, 4); \
        _result_f = _f; \
    } while(0)

    float _result_f;

    GEO3D_READ_F32(cam_addr + 0x00); float xpos = _result_f;
    GEO3D_READ_F32(cam_addr + 0x04); float ypos = _result_f;
    GEO3D_READ_F32(cam_addr + 0x08); float zpos = _result_f;

    uint32_t ang_word = mem_read32(bus, ang_addr);
    int16_t xang16 = (int16_t)(ang_word & 0xFFFF);
    int16_t yang16 = (int16_t)((ang_word >> 16) & 0xFFFF);

#undef GEO3D_READ_F32

    static const float TWO_PI = 6.28318530717959f;
    geo->cam_x = g_cam_sign_x * xpos;
    geo->cam_y = g_cam_sign_y * ypos;
    geo->cam_z = g_cam_sign_z * zpos;
    geo->rot_x = g_cam_sign_rx * ((float)xang16 / 65536.0f) * TWO_PI;
    geo->rot_y = g_cam_sign_ry * ((float)yang16 / 65536.0f) * TWO_PI;
    geo->has_game_view = true;

    /* Raw eye for the eye-bake auto-detector (scanner compares base SETPOS to −eye). */
    g_cam_eye_raw[0] = xpos; g_cam_eye_raw[1] = ypos; g_cam_eye_raw[2] = zpos;

    /* cam_mode = camera_struct +0x28 (g13+0x40). Drives the per-scene baking
     * heuristic: cam_mode 9 = fight look-at (world); others = attract (eye-baked). */
    g_cam_mode_value = (int)(mem_read32(bus, cam_addr + 0x28) & 0xFF);
    if (g_cam_auto_rot)
        g_cam_rot_only = (g_cam_mode_value != 9) ? 1 : 0;

    /* Debug: dump the raw camera struct per game frame for MAME comparison
     * (MAME = ground truth). Keyed by the STF frame counter so the two
     * deterministic-from-boot attract runs align frame-for-frame. */
#ifdef M2HLE_DEBUG_DUMPS
    if (g_cam_log) {
        static FILE *cf = NULL; static uint32_t prevf = 0xFFFFFFFFu;
        uint32_t fr = mem_read32(bus, 0x00500020);
        if (!cf) { cf = fopen("cam_ours.csv", "w");
                   if (cf) fprintf(cf, "frame,ex,ey,ez,xang,yang\n"); }
        if (cf && fr != prevf) { prevf = fr;
            fprintf(cf, "%u,%.4f,%.4f,%.4f,%d,%d\n",
                    fr, xpos, ypos, zpos, (int)xang16, (int)yang16);
            fflush(cf); }
    }
#endif
}

/* ---- Debug: log a summary of the current capture list ------------------- */

static inline void geo3d_log_captures(const geo3d_state_t *geo) {
    LOG_INFO("=== geo3d captures: %d / %d ===", geo->captured_count, MAX_GEO_MODELS);
    int n = geo->captured_count;
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) {
        const captured_model_t *cm = &geo->captured[i];
        if (cm->has_matrix) {
            LOG_INFO("  [%2d] model=%4d mat=0x%08X pos=(%.2f, %.2f, %.2f)",
                     i, cm->model_idx, cm->material_ptr,
                     cm->matrix[3], cm->matrix[7], cm->matrix[11]);
        } else {
            LOG_INFO("  [%2d] model=%4d mat=0x%08X (no transform)",
                     i, cm->model_idx, cm->material_ptr);
        }
    }
    if (geo->captured_count > 16)
        LOG_INFO("  ... %d more", geo->captured_count - 16);
}

#ifdef M2HLE_DEBUG_DUMPS   /* desktop only; see GEO3D_DUMP_TEX */
/* ---- Raw COP capture-stream dump (for per-pass view-base analysis) -------- */

/* Walk the current frame's geo_capture ring and write an annotated, decoded
 * dump (push/pop/identity/matrix/set_pos/scale/ang/bone/window/obj) to
 * cop_stream_<N>.txt.  N increments each call so consecutive dumps (e.g.
 * carnival then console) land in separate files for diffing.  Used to find
 * whether a pass establishes a view-base matrix before its objects. */
static inline void geo3d_dump_capture_stream(void) {
    static int dump_n = 0;
    char path[64];
    snprintf(path, sizeof(path), "cop_stream_%d.txt", dump_n++);
    FILE *f = fopen(path, "w");
    if (!f) { LOG_WARN("cop dump: cannot open %s", path); return; }

    int total = g_cop.geo_capture_count;
    int head  = g_cop.geo_capture_head;
    if (total > GEO_CAPTURE_SIZE) total = GEO_CAPTURE_SIZE;
    fprintf(f, "# COP capture stream: %d words\n", total);

    for (int i = 0; i < total; i++) {
        int idx = (head - total + i + GEO_CAPTURE_SIZE) & (GEO_CAPTURE_SIZE - 1);
        uint32_t v = g_cop.geo_capture[idx];
        #define GC(o) g_cop.geo_capture[(idx + (o)) & (GEO_CAPTURE_SIZE - 1)]
        #define F(o)  (is_sane_float(GC(o)) ? u32_as_float(GC(o)) : 0.0f)

        if (v == 0x00800101)      { fprintf(f, "%5d  PUSH\n", i); }
        else if (v == 0x01000202) { fprintf(f, "%5d  POP\n", i); }
        else if (v == 0x01800303) { fprintf(f, "%5d  IDENTITY\n", i); }
        else if (v == 0x02000404 || v == 0x05800B0B) {
            fprintf(f, "%5d  MATRIX(%08X)  R0[%.3f %.3f %.3f] R1[%.3f %.3f %.3f] R2[%.3f %.3f %.3f] T[%.3f %.3f %.3f]\n",
                    i, v, F(1),F(2),F(3), F(4),F(5),F(6), F(7),F(8),F(9), F(10),F(11),F(12));
            i += 12;
        }
        else if (v == 0x03000606) { fprintf(f, "%5d  SETPOS [%.3f %.3f %.3f]\n", i, F(1),F(2),F(3)); i += 3; }
        else if (v == 0x03800707) { fprintf(f, "%5d  SCALE  [%.3f %.3f %.3f]\n", i, F(1),F(2),F(3)); i += 3; }
        else if (v == 0x04000808) { fprintf(f, "%5d  ANG_X  %d\n", i, (int)(int16_t)(GC(1)&0xFFFF)); i += 1; }
        else if (v == 0x04800909) { fprintf(f, "%5d  ANG_Y  %d\n", i, (int)(int16_t)(GC(1)&0xFFFF)); i += 1; }
        else if (v == 0x05000A0A) { fprintf(f, "%5d  ANG_Z  %d\n", i, (int)(int16_t)(GC(1)&0xFFFF)); i += 1; }
        else if (v == 0x1F803F3F) { fprintf(f, "%5d  ANG_XYZ z=%d y=%d x=%d\n", i,
                                            (int)(int16_t)(GC(1)&0xFFFF),(int)(int16_t)(GC(2)&0xFFFF),(int)(int16_t)(GC(3)&0xFFFF)); i += 3; }
        else if (v == 0x1B003636) { fprintf(f, "%5d  BONE_LOAD pid=%u slot=%u\n", i, GC(1)&0xFF, (GC(2))/12); i += 2; }
        else if (v == 0x1B803737) { fprintf(f, "%5d  BONE_SEL  pid=%u bone=%u\n", i, GC(1)&1, (GC(2)&0xFF)/0xC); i += 2; }
        else if (v == GEO_WIN_SENTINEL) {
            fprintf(f, "%5d  WINDOW  c0=%08X c1=%08X c2=%08X\n", i, GC(1),GC(2),GC(3)); i += 6;
        }
        else if (v == 0x3C007878) {
            fprintf(f, "%5d  OBJ  mesh=%08X\n", i, GC(5)); i += 8;
        }
        else {
            int na = sharc_args_for_cmd(v);
            if (na > 0) { fprintf(f, "%5d  cmd %08X (+%d args)\n", i, v, na); i += na; }
            else        { fprintf(f, "%5d  word %08X\n", i, v); }
        }
        #undef GC
        #undef F
    }
    fclose(f);
    LOG_INFO("cop dump -> %s (%d words)", path, total);
}
#endif /* M2HLE_DEBUG_DUMPS */

#endif /* GEO3D_H */
