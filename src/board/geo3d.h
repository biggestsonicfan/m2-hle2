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
#include "cop.h"            /* g_cop.geo_capture[] */
#include "log.h"
#include "memory.h"         /* mem_read8 / mem_read32 */

/* ---- Capacities ---------------------------------------------------------- */

#define MAX_GEO_MODELS     512
#define GEO3D_DIRECT_WORDS 16384u
/* The line and triangle buffers a frame is decoded into. A small target sets
 * both lower (the Dreamcast draws no lines and flushes per run). */
#ifndef GEO3D_MAX_LINES
#define GEO3D_MAX_LINES    32768
#endif
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

/* A host whose ROM is paged in software (the Dreamcast) maps a pointer into its
 * window to the bytes there; the result is good until the next call. */
#ifndef GEO3D_ROM
#define GEO3D_ROM(p, n) ((const uint8_t *)(p))
#endif
static inline uint32_t geo3d_rom16(const uint8_t *p) { p = GEO3D_ROM(p, 2); return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

static inline float u32_as_float(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* Apply a 3x4 row-major matrix (9 rotation + 3 translation) to a vec3. */
static inline vec3_t apply_matrix(vec3_t v, const float *m) {
    vec3_t r;
    r.x = m[0]*v.x + m[1]*v.y + m[2]*v.z  + m[3];
    r.y = m[4]*v.x + m[5]*v.y + m[6]*v.z  + m[7];
    r.z = m[8]*v.x + m[9]*v.y + m[10]*v.z + m[11];
    return r;
}

#if defined(GEO3D_DC_SINK) && (defined(__SH4__) || defined(__SH4_SINGLE__) || defined(__SH4_SINGLE_ONLY__))
/* The SH-4's FTRV: XMTRX (the back bank) times fv0 in one instruction, for the
 * corners and the view planes. Nothing between the load and the last FTRV may
 * call out: the drawing only, the board's state never sees these. */
#define GEO3D_FTRV 1
/* c: 16 floats, column by column, 8-byte aligned. */
static inline void geo3d_xmtrx_load(const float *c) {
    __asm__ __volatile__(
        "fschg\n\t"
        "fmov.d @%0+, xd0\n\t"  "fmov.d @%0+, xd2\n\t"  "fmov.d @%0+, xd4\n\t"  "fmov.d @%0+, xd6\n\t"
        "fmov.d @%0+, xd8\n\t"  "fmov.d @%0+, xd10\n\t" "fmov.d @%0+, xd12\n\t" "fmov.d @%0+, xd14\n\t"
        "fschg\n"
        : "+r"(c) : : "memory");
}
/* XMTRX * (x, y, z, w) into o[0..3]. */
static inline void geo3d_ftrv(float x, float y, float z, float w, float *o) {
    register float f0 __asm__("fr0") = x;
    register float f1 __asm__("fr1") = y;
    register float f2 __asm__("fr2") = z;
    register float f3 __asm__("fr3") = w;
    __asm__ __volatile__("ftrv xmtrx, fv0" : "+f"(f0), "+f"(f1), "+f"(f2), "+f"(f3));
    o[0] = f0; o[1] = f1; o[2] = f2; o[3] = f3;
}
/* A board matrix (3 rows of 4) as XMTRX. */
static inline void geo3d_xmtrx_board(const float *m) {
    float c[16] __attribute__((aligned(8))) = {
        m[0], m[4], m[8],  0.0f,  m[1], m[5], m[9],  0.0f,
        m[2], m[6], m[10], 0.0f,  m[3], m[7], m[11], 1.0f };
    geo3d_xmtrx_load(c);
}
#endif

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

/* floor(c * 4096 + 0.5) as int32, without the double floor (a library call on
 * the SH-4, a tenth of its frame there). c * 4096 is exact in float; its
 * integer part truncates and the fraction left is exact, so rounding half up
 * from those two is the double expression's value. Out of int32's range (and
 * NaN) it takes the double path as before. */
static inline int32_t geo3d_round4096(float c) {
    const float y = c * 4096.0f;
    if (fabsf(y) < 1073741824.0f) {
        const int32_t i = (int32_t)y;
        const float fr = y - (float)i;
        return i + (fr >= 0.5f) - (fr < -0.5f);
    }
    return (int32_t)floor((double)c * 4096.0 + 0.5);
}

static inline uint32_t geo3d_corner_key(vec3_t p) {
    const float c[3] = { p.x, p.y, p.z };
    uint32_t h = 2166136261u;
    for (int a = 0; a < 3; a++) {
        uint32_t q = (uint32_t)geo3d_round4096(c[a]);
        h = (h ^ (q & 0xffffu)) * 16777619u;
        h = (h ^ ((q >> 16) & 0xffffu)) * 16777619u;
    }
    return h;
}

/* A slot's three words side by side: as three arrays 32 KB apart they fell on
 * the same line of the SH-4's direct-mapped cache, three misses a lookup. */
typedef struct { uint32_t stamp, quad, cut; } geo3d_split_slot_t;
static uint32_t g_geo3d_split_gen;
static geo3d_split_slot_t g_geo3d_split[GEO3D_SPLIT_SLOTS];

/* Forget every quad seen so far — once per model. */
static inline void geo3d_split_reset(void) {
    if (++g_geo3d_split_gen == 0) {
        for (uint32_t i = 0; i < GEO3D_SPLIT_SLOTS; i++) g_geo3d_split[i].stamp = 0;
        g_geo3d_split_gen = 1;
    }
}

/* True when a quad with these corners was already cut along the other diagonal.
 * The first sighting records its cut and answers false. */
static inline bool geo3d_split_other_way(uint32_t quad, uint32_t cut) {
    uint32_t i = (quad * 2654435761u) & (GEO3D_SPLIT_SLOTS - 1u);
    while (g_geo3d_split[i].stamp == g_geo3d_split_gen) {
        if (g_geo3d_split[i].quad == quad) return g_geo3d_split[i].cut != cut;
        i = (i + 1u) & (GEO3D_SPLIT_SLOTS - 1u);
    }
    g_geo3d_split[i].stamp = g_geo3d_split_gen;
    g_geo3d_split[i].quad  = quad;
    g_geo3d_split[i].cut   = cut;
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
    uint32_t zadjust;       /* the z-sort mode (command 08) then: the raster's z_adjust */
    uint32_t tpa, tha;      /* the object command's texture point / header addresses */
    /* Direct data (GEO 0x02/0x12) instead of an object: its words after the
     * command, at direct_off in geo3d_state_t.direct_words. 0: an object. */
    uint32_t direct_off, direct_len;
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

#ifndef GEO3D_MAX_TRIS
#define GEO3D_MAX_TRIS GEO3D_MAX_LINES
#endif

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
    float zl;                    /* the face's layer as a [0, 1] depth offset (geo3d_mesh_layers) */
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
 * A game frame takes the whole rule (geo3d_flat_depth, below). It used to take
 * half of it, the explorer's: a polygon could take its sorted depth only to
 * recede, by at most GEO3D_ZSORT_RECEDE, and only when it was shallow along the
 * view. The explorer needs that because its camera flies anywhere. With the
 * board's camera that half rule needed seven patches: the recede bound, a list
 * of models standing on a floor, keep-depth for far-corner pairs, the
 * coplanar layers and their planes, decal ties across draws, same-matrix runs
 * ranked as one mesh, and held pairs. Each patch came from a picture that MAME
 * got right and the half rule got wrong. With the whole rule, none of them
 * apply. tools/grade-zsort.mjs --toggle zflat
 * held all fifteen stages' replays against MAME (Pinboard #247), the Aurora
 * rink that once went half black included.
 *
 * What is left of the half rule is geo3d_sort_z and the recede per vertex: the
 * object viewer's free camera and set_camera zflat=0 still use it.
 *
 * Faces that never went through the geometry decoder (the homebrew HUD, the
 * wireframe, the object viewer's free camera) carry NONE and keep the depth the
 * projection gives them.
 */
#define GEO3D_ZSORT_NONE   1.0e30f
/* The host projection's near plane (gm_mat4_geo_projection), in camera units. */
#define GEO3D_NEAR         0.05f
/* The sort z of a polygon the board gives key 0 (geo3d_board_zkey): just
 * behind the near plane, the nearest depth there is, and the same for every
 * such polygon, so the later one wins the tie as on the board. */
#define GEO3D_ZSORT_KEY0   (-GEO3D_NEAR * 1.01f)
static float g_geo3d_emit_zs        = GEO3D_ZSORT_NONE;
static int   g_geo3d_zsort          = 1;      /* 0: every face keeps its own depth */
static float g_geo3d_zsort_recede   = 12.0f;  /* how far back a vertex may be taken */
static int   g_geo3d_zsort_key0     = 1;      /* 0: key-0 polygons keep their own sort z */
static inline uint32_t geo3d_board_zkey(float z);

/* MAME's float_to_zval gives key 0 to a polygon whose sort corner is behind
 * the eye or more than 12 binary orders nearer than z_adjust, and the board
 * has no near plane: it clips against the four sides through the eye. All of
 * those polygons tie, and a tie goes to the later one. So they all get one
 * depth at the near plane (GEO3D_ZSORT_KEY0, which the depth test's
 * LESS_EQUAL hands to the later draw) and no layer, and the vertex shader
 * clips them at the eye, not at the near plane.
 *  *Symptom that surfaced this in STF (Pinboard #242):* in the Death Egg II
 * cutscene's space shot the camera sits inside the Lunar Fox, drawn at scale
 * 0.01 with z_adjust 4.0, and 727 of its hull's 759 faces, the canopy (231)
 * and Sonic's head (3027) are key 0. The near plane cut the hull open, and the
 * canopy's checker covered Sonic's head, which MAME draws over it.
 * Under the flat key (below) this is the half rule's (zflat 0) only: key 0
 * is the nearest flat depth there, and the vertex shader puts every vertex of
 * a flat face at clip z 0, so it too is cut at the eye alone. */

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
    float z = zmode == 2u ? far_ : near_;
    if (g_geo3d_zsort_key0 && geo3d_board_zkey(-z) == 0u) return GEO3D_ZSORT_KEY0;
    return z;
}

/* ---- The board's sort, taken whole (set_camera zflat) ------------------------
 * MAME's model2_3d_process_polygon gives every polygon ONE z-sort key and the
 * renderer fills the nearest key first, the later polygon winning a tie. That
 * is a depth buffer whose depth is the key, flat over the polygon, drawn in
 * submission order with LEQUAL; the fill shader writes it per fragment from
 * the face's key (geo3d_tri_t.zl < 0: -(1 + (key + 0.5) / 65536)). Within a
 * window the key is the whole of the depth test, so two faces lying on each
 * other are settled the way the board settles them, whichever objects they
 * belong to and however far apart they stand.
 *
 * Mode 0 is "the previous polygon's z", and on the board that carries across
 * objects: raster->polygon_z is reset once a frame and set by every polygon,
 * culled ones included. g_geo3d_flat_prev_z is that register. */
static int   g_geo3d_zflat       = 1;   /* 0: the half rule alone (geo3d_sort_z), for an A/B */
static int   g_geo3d_flat_list   = 0;   /* set while game_render_draw_geo_list decodes */
static float g_geo3d_flat_prev_z = 1.0e10f;
static float g_geo3d_emit_flat   = -1.0f;   /* this face's depth in its window's slice, or < 0 */

/* Faces that cannot reach the window are not emitted (game_render_draw_geo_list
 * sets the planes per run; $M2HLE_VIEW_CULL=0 turns it off for an A/B). The
 * planes are the run's projection rows taken into eye space: the window's four
 * sides, a few board pixels further out, and the eye plane. A face is dropped
 * only when every corner is strictly outside the same one, which a linear
 * function over the triangle carries to every point of it: no pixel, however
 * the GPU rounds. Its key, carry and diagonal are still worked out. */
static int   g_geo3d_view_cull = -1;   /* -1: read the environment once */
static int   g_geo3d_cull_on;          /* planes set for the run being decoded */
static float g_geo3d_cull_plane[5][4];
static float g_geo3d_cull_nlen[5];       /* |xyz| of each plane */

#ifdef GEO3D_FTRV
/* geo3d_cull_code with planes 0-3's distances from FTRV (XMTRX: those planes,
 * row by row), in d. */
static inline unsigned geo3d_cull_bit(const float *q, vec3_t p, float d, unsigned k) {
    if (!(d < 0.0f)) return 0;
    float m = 1.0e-5f * (fabsf(q[0] * p.x) + fabsf(q[1] * p.y) + fabsf(q[2] * p.z) + fabsf(q[3]));
    return d < -m ? 1u << k : 0u;
}
/* Unrolled: the loop and its select were most of a corner's cost. */
static inline uint8_t geo3d_cull_code_d(vec3_t p, const float *d4) {
    const float (*q)[4] = g_geo3d_cull_plane;
    return (uint8_t)(geo3d_cull_bit(q[0], p, d4[0], 0) | geo3d_cull_bit(q[1], p, d4[1], 1) |
                     geo3d_cull_bit(q[2], p, d4[2], 2) | geo3d_cull_bit(q[3], p, d4[3], 3) |
                     geo3d_cull_bit(q[4], p, q[4][0] * p.x + q[4][1] * p.y + q[4][2] * p.z + q[4][3], 4));
}
#endif

static inline uint8_t geo3d_cull_code(vec3_t p) {
    uint8_t code = 0;
    for (int k = 0; k < 5; k++) {
        const float *q = g_geo3d_cull_plane[k];
        float d = q[0] * p.x + q[1] * p.y + q[2] * p.z + q[3];
        if (!(d < 0.0f)) continue;   /* m >= 0, so only a negative d can be out */
        float m = 1.0e-5f * (fabsf(q[0] * p.x) + fabsf(q[1] * p.y) + fabsf(q[2] * p.z) + fabsf(q[3]));
        if (d < -m) code |= (uint8_t)(1u << k);
    }
    return code;
}

static inline uint32_t geo3d_board_zkey(float z);
/* The depth the board keys the face by, carried in g_geo3d_flat_prev_z: every
 * face in order must pass through here, drawn or not. */
static inline float geo3d_flat_z(const vec3_t *sv, const int *zsrc, uint32_t zmode) {
    float z;
    if (zmode == 0u)      z = g_geo3d_flat_prev_z;
    else if (zmode == 3u) z = 1.0e10f;
    else {   /* view z runs negative ahead: mode 2 takes the farthest corner, mode 1 the nearest */
        float a = sv[zsrc[0]].z, b = sv[zsrc[1]].z, c = sv[zsrc[2]].z, d = sv[zsrc[3]].z;
        if (zmode == 2u) { if (b < a) a = b; if (c < a) a = c; if (d < a) a = d; }
        else             { if (b > a) a = b; if (c > a) a = c; if (d > a) a = d; }
        z = -a;
    }
    g_geo3d_flat_prev_z = z;
    return z;
}
static inline float geo3d_flat_depth(const vec3_t *sv, const int *zsrc, uint32_t zmode) {
    return ((float)geo3d_board_zkey(geo3d_flat_z(sv, zsrc, zmode)) + 0.5f) * (1.0f / 65536.0f);
}

/* The bound, applied per vertex so the slope of a polygon lying along the view
 * survives: the vertex may recede to its polygon's sorted depth, no further
 * than the bound, and is never pulled forward. Done here rather than in the
 * shader so the bound stays an ordinary variable. */
static inline float geo3d_zs_vertex(float zb, float z) {
    if (zb > 1.0e29f) return GEO3D_ZSORT_NONE;
    if (zb == GEO3D_ZSORT_KEY0) return zb;
    float lo = z - g_geo3d_zsort_recede;
    return zb < lo ? lo : (zb > z ? z : zb);
}

/* Faces lying on each other in one plane (geo3d_mesh_layers): the face being
 * emitted, its layer, and the plane of its group in camera space (n.p = d),
 * which it takes its depth from instead of the sorted depth. A face with a
 * layer and no plane keeps its own depth. Set per face for the object viewer
 * (g_geo3d_decode_layers); a game frame takes the board's key instead
 * (geo3d_flat_depth), and everything else leaves them at 0. */
static int   g_geo3d_layers         = 1;      /* 0: no face is layered */
static float g_geo3d_layer_steps    = 4.0f;   /* 24-bit depth steps a layer is pulled forward */
static float g_geo3d_emit_layer     = 0.0f;
static uint64_t g_geo3d_layer_faces;          /* faces drawn layered or on a plane (set_camera reports it) */
/* geo3d_decode_model takes the layers from the mesh cache only when its caller
 * is on the render thread, which owns the cache, and says so (the object
 * viewer). The bridge's model dump decodes on its own thread. */
static int   g_geo3d_decode_layers  = 0;

/* One face's layer and plane, by face-loop index (geo3d_mesh_layers_for). */
typedef struct {
    uint16_t layer;
    uint8_t  has_plane;
    float    plane[4];
} geo3d_face_layer_t;
static int   g_geo3d_emit_has_plane = 0;
static float g_geo3d_emit_plane[4];

/* A model-space plane (n.p = d, |n| = 1) through a draw's 3x4 row-major matrix
 * into camera space. The normal goes through the cofactor matrix — the inverse
 * transpose up to the determinant, which scales n and d alike — so a scaled or
 * sheared draw keeps its plane. */
static inline bool geo3d_plane_to_view(const float *pl, const float *m, float *out) {
    const float r0[3] = { m[0], m[1], m[2] }, r1[3] = { m[4], m[5], m[6] }, r2[3] = { m[8], m[9], m[10] };
    const float c0[3] = { r1[1]*r2[2] - r1[2]*r2[1], r1[2]*r2[0] - r1[0]*r2[2], r1[0]*r2[1] - r1[1]*r2[0] };
    const float c1[3] = { r2[1]*r0[2] - r2[2]*r0[1], r2[2]*r0[0] - r2[0]*r0[2], r2[0]*r0[1] - r2[1]*r0[0] };
    const float c2[3] = { r0[1]*r1[2] - r0[2]*r1[1], r0[2]*r1[0] - r0[0]*r1[2], r0[0]*r1[1] - r0[1]*r1[0] };
    float n[3] = { c0[0]*pl[0] + c0[1]*pl[1] + c0[2]*pl[2],
                   c1[0]*pl[0] + c1[1]*pl[1] + c1[2]*pl[2],
                   c2[0]*pl[0] + c2[1]*pl[1] + c2[2]*pl[2] };
    if (n[0] == 0.0f && n[1] == 0.0f && n[2] == 0.0f) return false;
    vec3_t p = apply_matrix((vec3_t){ pl[0] * pl[3], pl[1] * pl[3], pl[2] * pl[3] }, m);
    out[0] = n[0]; out[1] = n[1]; out[2] = n[2];
    out[3] = n[0] * p.x + n[1] * p.y + n[2] * p.z;
    return true;
}

/* The depth one corner is drawn at: where its view ray meets the face's group
 * plane, or the sorted depth. A plane's depth is affine across the screen, so
 * corners put on it fill the face at the plane's depth pixel for pixel, and
 * every face of a group lands on the same values; the layer then need only
 * beat the rounding (the explorer does this per pixel, FACE_LAYERS in
 * js/viewer.js, and pays for gl_FragDepth). */
static inline float geo3d_zs_corner(float x, float y, float z) {
    if (g_geo3d_emit_zs == GEO3D_ZSORT_KEY0) return GEO3D_ZSORT_KEY0;
    if (g_geo3d_emit_has_plane) {
        const float *P = g_geo3d_emit_plane;
        float den = P[0] * x + P[1] * y + P[2] * z;
        if (den != 0.0f) {
            float zp = z * P[3] / den;
            /* Forward onto the plane, never back: a face laid in front of its
             * group's plane keeps its own depth, or a face held apart from it
             * with its own depth (a far-corner face takes no plane) can end up
             * in front. The Tails lab's monitor pictures went behind their own
             * screens that way (issue #85). */
            if (zp < 0.0f && zp >= z) return zp;
        }
        return GEO3D_ZSORT_NONE;
    }
    if (g_geo3d_emit_layer > 0.0f) return GEO3D_ZSORT_NONE;
    return geo3d_zs_vertex(g_geo3d_emit_zs, z);
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
 *                repeating.
 *   WRAP_X/Y     bits 6 / 7: the bilinear pair at the tile's last texel
 *                blends into the next copy's first. Without the bit the board
 *                clamps there, taking the nearer texel of the pair unblended
 *                (fetch_bilinear_texel, `!tex_wrap_x && u1 == 0`), which is
 *                what keeps STF's sky ring free of a seam at every segment
 *                join. MAME clears wrap on a mirrored axis; the mirrored tap is
 *                the edge texel again there, so the flag needs no masking.
 *                Bits 64 and 128 belong to the renderer, hence 256 / 512 (the
 *                explorer's too). */
#define GEO3D_FACE_TRANSPARENT 1u
#define GEO3D_FACE_CHECKER     2u
#define GEO3D_FACE_SHEET1      4u
#define GEO3D_FACE_MIRROR_X    8u
#define GEO3D_FACE_MIRROR_Y    16u
#define GEO3D_FACE_WRAP_X      256u
#define GEO3D_FACE_WRAP_Y      512u
#define GEO3D_FACE_CHECKER_ODD 1024u  /* the checker's other phase (set_camera "checker":0) */

/* 0: every face filters as if it set both wrap bits, the fill from before they
 * were read (set_camera "texclamp", for a before/after). Applied where the
 * renderer packs its vertices, since cached meshes hold the decoded flags. */
static int g_geo3d_tex_clamp = 1;
/* 0: checkers draw the other pixel of each pair, the phase the GL fill had
 * when it took gl_FragCoord's bottom-up rows (set_camera "checker"). */
static int g_geo3d_checker_phase = 1;
static inline float geo3d_face_fill_flags(float fl) {
    unsigned f = (unsigned)(fl + 0.5f);
    if (!g_geo3d_tex_clamp) f |= GEO3D_FACE_WRAP_X | GEO3D_FACE_WRAP_Y;
    if (!g_geo3d_checker_phase && (f & GEO3D_FACE_CHECKER)) f |= GEO3D_FACE_CHECKER_ODD;
    return (float)f;
}

/* The buffer's size, GEO3D_MAX_TRIS unless a port takes its faces elsewhere
 * (GEO3D_DC_SINK) and needs none of it. */
#ifndef GEO3D_TRI_BUF
#define GEO3D_TRI_BUF GEO3D_MAX_TRIS
#endif

typedef struct {
    geo3d_tri_t tris[GEO3D_TRI_BUF];
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

#ifdef GEO3D_DC_SINK
/* The Dreamcast's faces go straight into its own compact records
 * (dreamcast/dc_pvr.h), not geo3d_tri_t: a mesh's corners are projected once
 * into a pool (geo3d_dc_verts, -1 when it is full) and a face is three of
 * them (geo3d_dc_tri); geo3d_dc_tri_xyz is a face with corners of its own. */
static int  geo3d_dc_verts(const vec3_t *tv, int n);
static void geo3d_dc_tri(int a, int b, int c, float ua, float va, float ub, float vb, float uc, float vc,
                         float r, float g, float b_, float tx, float ty, float tw, float th,
                         float lb, float pl, float fl);
static void geo3d_dc_tri_xyz(float x0, float y0, float z0, float u0, float v0,
                             float x1, float y1, float z1, float u1, float v1,
                             float x2, float y2, float z2, float u2, float v2,
                             float r, float g, float b, float tx, float ty, float tw, float th,
                             float lb, float pl, float fl);
#endif

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
    if (g_geo3d_tri_sink->count >= GEO3D_TRI_BUF) return;
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
#ifdef GEO3D_DC_SINK
    if (g_geo3d_tri_sink == &g_geo3d_tris) {
        geo3d_dc_tri_xyz(x0, y0, z0, u0, v0, x1, y1, z1, u1, v1, x2, y2, z2, u2, v2,
                         r, g, b, tx, ty, tw, th, lb, pl, fl);
        return;
    }
#endif
    geo3d_tri_t *T = &g_geo3d_tri_sink->tris[g_geo3d_tri_sink->count++];
    T->x0=x0; T->y0=y0; T->z0=z0; T->u0=u0; T->v0=v0;
    T->x1=x1; T->y1=y1; T->z1=z1; T->u1=u1; T->v1=v1;
    T->x2=x2; T->y2=y2; T->z2=z2; T->u2=u2; T->v2=v2;
    T->r=r;   T->g=g;   T->b=b;
    T->tx=tx; T->ty=ty; T->tw=tw; T->th=th;
    T->lb=lb; T->pl=pl; T->fl=fl;
    T->texlod = g_geo3d_emit_texlod;
    if (g_geo3d_emit_flat >= 0.0f) {
        T->zs0 = T->zs1 = T->zs2 = GEO3D_ZSORT_NONE;
        T->zl  = -(1.0f + g_geo3d_emit_flat);
        return;
    }
    T->zs0 = geo3d_zs_corner(x0, y0, z0);
    T->zs1 = geo3d_zs_corner(x1, y1, z1);
    T->zs2 = geo3d_zs_corner(x2, y2, z2);
    T->zl  = g_geo3d_emit_layer * g_geo3d_layer_steps * (1.0f / 16777216.0f);
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
    /* Take fov_deg from the display list's focal command (a geo_displaylist
     * profile; geo3d_scan_displaylist). Moving the FOV by hand clears it. */
    bool             fov_auto;

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

    /* A view matrix was read from game RAM this frame. */
    bool             has_game_view;

    /* Windows in the display list last walked (geo3d_scan_geo_list). */
    int              geo_windows;

    /* That list's direct data, copied out of the snapshot: the emu thread
     * reuses a snapshot two publishes on, while the frame may still draw. */
    uint32_t         direct_words[GEO3D_DIRECT_WORDS];
    uint32_t         direct_used;
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
    /* Open-addressed index into the arrays above, keyed by pol_ptr; -1 is
     * empty. A pointer keeps the slot of its first model, as the linear scan
     * it replaced did. The scan cost ~80 samples a fight frame on the A55. */
    int16_t  hash[MODEL_LOOKUP_SIZE * 2];
} geo3d_lookup_t;

static geo3d_lookup_t g_geo3d_lookup = {0};

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
 * decoder to its coordinates corner for corner. */

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
/* The debug dumps (this one, the texture extractor, the COP
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

static inline void geo3d_lookup_invalidate(void) {
    g_geo3d_lookup.built = false;
    g_geo3d_lookup.count = 0;
}

_Static_assert(MODEL_LOOKUP_SIZE * 2 == 1 << 14, "geo3d_lookup_hash takes 14 bits");
static inline uint32_t geo3d_lookup_hash(uint32_t pol_ptr) {
    return (pol_ptr * 0x9E3779B1u) >> (32 - 14);
}

static inline void geo3d_lookup_build(const uint8_t *main_data, size_t main_data_size,
                                       uint32_t table_off, uint32_t table_count) {
    if (g_geo3d_lookup.built) return;
    g_geo3d_lookup.count = 0;
    if (!main_data) return;
    if ((size_t)table_off + (size_t)table_count * MODEL_ENTRY_SIZE > main_data_size) return;

    for (uint32_t m = 0; m < table_count && g_geo3d_lookup.count < MODEL_LOOKUP_SIZE; m++) {
        uint32_t toff = table_off + m * MODEL_ENTRY_SIZE;
        uint32_t pol  = read_u32_le(GEO3D_ROM(main_data + toff + 8, 4));
        if (pol != 0) {
            g_geo3d_lookup.pol_ptrs[g_geo3d_lookup.count]  = pol;
            g_geo3d_lookup.model_idx[g_geo3d_lookup.count] = (int)m;
            g_geo3d_lookup.count++;
        }
    }
    memset(g_geo3d_lookup.hash, 0xFF, sizeof g_geo3d_lookup.hash);
    for (int i = 0; i < g_geo3d_lookup.count; i++) {
        uint32_t h = geo3d_lookup_hash(g_geo3d_lookup.pol_ptrs[i]);
        while (g_geo3d_lookup.hash[h] >= 0 &&
               g_geo3d_lookup.pol_ptrs[g_geo3d_lookup.hash[h]] != g_geo3d_lookup.pol_ptrs[i])
            h = (h + 1) & (MODEL_LOOKUP_SIZE * 2 - 1);
        if (g_geo3d_lookup.hash[h] < 0) g_geo3d_lookup.hash[h] = (int16_t)i;
    }
    g_geo3d_lookup.built = true;
    LOG_INFO("geo3d_lookup_build: %d entries (table_off=0x%X count=%u)",
             g_geo3d_lookup.count, table_off, table_count);
}

static inline int geo3d_lookup_by_pol(uint32_t pol_ptr) {
    if (!g_geo3d_lookup.built) return -1;
    for (uint32_t h = geo3d_lookup_hash(pol_ptr);; h = (h + 1) & (MODEL_LOOKUP_SIZE * 2 - 1)) {
        int i = g_geo3d_lookup.hash[h];
        if (i < 0) return -1;
        if (g_geo3d_lookup.pol_ptrs[i] == pol_ptr) return g_geo3d_lookup.model_idx[i];
    }
}

/* ---- GEO display-list scanner (authentic hardware path) ----------------- *
 * Non-STF games and the m2-snake homebrew drive the GEO directly: the i960
 * builds a display list in bufferram and points the GEO read pointer at it.
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

/* What geo3d_scan_displaylist's walk carries from one command to the next. */
typedef struct {
    geo3d_state_t  *geo;
    const uint32_t *buff_ram;
    uint32_t        buff_words;
    const uint8_t  *main_data;
    size_t          main_data_size;
    uint32_t        table_off;
    const uint8_t  *palette;
    size_t          palette_size;
    float           cur_mat[12];
    bool            have_mat;
} geo3d_dl_walk_t;

/* Snapshot the prev list for interpolation, once per vblank frame. */
static inline void geo3d__dl_snapshot_prev(geo3d_state_t *geo) {
    if (g_cop.geo_frame_end != geo->last_frame_end) {
        memcpy(geo->captured_prev, geo->captured,
               (size_t)geo->captured_count * sizeof(captured_model_t));
        geo->captured_prev_count = geo->captured_count;
        geo->last_frame_end      = g_cop.geo_frame_end;
    }
}

/* MATRIX: 12 floats stored column-major (col0,col1,col2,T) — convert
 * to the captured_model_t row-major 3x4 layout. */
static inline void geo3d__dl_matrix(geo3d_dl_walk_t *w, uint32_t p) {
    float m[12];
    for (int k = 0; k < 12; k++) memcpy(&m[k], &w->buff_ram[p + 1 + (uint32_t)k], 4);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) w->cur_mat[r * 4 + c] = m[c * 3 + r];
        w->cur_mat[r * 4 + 3] = m[9 + r];
    }
    w->have_mat = true;
}

/* Per-object flat colour: the homebrew encodes the colorbase in
 * the object_data `tha` (= GEO_TEXRAM_BIT 0x800000 | colorbase*4)
 * and stores the hue at palram[colorbase + 0x1000] (BGR555), set
 * via m2_setcolor. Read it live from palette RAM. */
static inline void geo3d__dl_object_color(const geo3d_dl_walk_t *w, uint32_t p, captured_model_t *cm) {
    cm->color[0] = cm->color[1] = cm->color[2] = 1.0f;  /* fallback white */
    {
        uint32_t tha = w->buff_ram[p + 2];
        uint32_t cb  = (tha & 0x007FFFFFu) >> 2;
        uint32_t poff = (cb + 0x1000u) * 2u;
        if (w->palette && poff + 1u < w->palette_size) {
            uint16_t bgr = (uint16_t)(w->palette[poff] | (w->palette[poff + 1] << 8));
            cm->color[0] = ( bgr        & 0x1F) / 31.0f;   /* R = bits[4:0]  */
            cm->color[1] = ((bgr >> 5)  & 0x1F) / 31.0f;   /* G = bits[9:5]  */
            cm->color[2] = ((bgr >> 10) & 0x1F) / 31.0f;   /* B = bits[14:10] */
        }
    }
}

/* OBJECT: args = tpa, tha, oba(mesh ptr), obc. The homebrew's oba is a
 * model-table mesh pointer, so reverse-map it to a model index and reuse
 * the STF mesh+matrix renderer. */
static inline void geo3d__dl_object(geo3d_dl_walk_t *w, uint32_t p) {
    geo3d_state_t *geo = w->geo;
    const float *cur_mat = w->cur_mat;
    uint32_t oba = w->buff_ram[p + 3];
    int model_idx = geo3d_lookup_by_pol(oba);
    if (model_idx < 0) return;
    uint32_t toff = w->table_off + (uint32_t)model_idx * MODEL_ENTRY_SIZE;
    if ((size_t)toff + MODEL_ENTRY_SIZE > w->main_data_size) return;
    captured_model_t *cm = &geo->captured[geo->captured_count];
    memset(cm, 0, sizeof(*cm));
    cm->model_idx    = model_idx;
    cm->material_ptr = read_u32_le(GEO3D_ROM(w->main_data + toff + 4, 4));
    geo3d__dl_object_color(w, p, cm);
    cm->dbg_mesh_ptr = oba;
    if (w->have_mat) {
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
    if (!w->have_mat || cm->matrix[11] <= -1.0f)
        geo->captured_count++;
}

/* FOCAL: fx, fy in pixels; the GEO puts y at fy*y/z from the window's
 * centre. The host's perspective puts it at 192*cot(fov/2)*y/z on the
 * 384-line screen, and x at the same scale (aspect 496/384), so this
 * fov is the board's projection when fx == fy (m2-sdk's geo_focal(280,
 * 280): 68.9 degrees). */
static inline void geo3d__dl_focal(geo3d_dl_walk_t *w, uint32_t p) {
    float fy;
    memcpy(&fy, &w->buff_ram[p + 2], 4);
    if (w->geo->fov_auto && fy > 1.0f)
        w->geo->fov_deg = 2.0f * atanf(192.0f / fy) * (180.0f / 3.14159265f);
}

/* LIGHT (0x05000A0A): 3 floats (x,y,z). The GEO lights each face by
 * normal·light; mirror it into the renderer's light dir so the flat
 * panels shade like the HLE. Camera space — negate z to match the
 * Z-row negation the host (−z forward) applies to the geometry. */
static inline void geo3d__dl_light(const geo3d_dl_walk_t *w, uint32_t p) {
    float lx, ly, lz;
    memcpy(&lx, &w->buff_ram[p + 1], 4);
    memcpy(&ly, &w->buff_ram[p + 2], 4);
    memcpy(&lz, &w->buff_ram[p + 3], 4);
    g_light_dir[0] = lx; g_light_dir[1] = ly; g_light_dir[2] = -lz;
}

/* One command of the walk; false where the walk stops. */
static inline bool geo3d__dl_command(geo3d_dl_walk_t *w, uint32_t p, uint32_t cmd) {
    static int warned_direct = 0;
    const uint32_t buff_words = w->buff_words;
    if ((cmd == 0xb || cmd == 0x1b) && p + 12u < buff_words) {
        geo3d__dl_matrix(w, p);
    } else if ((cmd == 1 || cmd == 0x11) && p + 4u < buff_words) {
        geo3d__dl_object(w, p);
    } else if ((cmd == 9 || cmd == 0x19) && p + 2u < buff_words) {
        geo3d__dl_focal(w, p);
    } else if (cmd == 0xa && p + 3u < buff_words) {
        geo3d__dl_light(w, p);
    } else if (cmd == 2 || cmd == 0x12) {
        if (!warned_direct) {
            LOG_WARN("geo3d displaylist: direct_data (cmd 2) not yet decoded; stopping walk");
            warned_direct = 1;
        }
        return false;   /* variable-length inline geometry — can't skip reliably yet */
    }
    return true;
}

static inline void geo3d_scan_displaylist(geo3d_state_t *geo,
        const uint32_t *buff_ram, uint32_t buff_words, uint32_t rstart,
        const uint8_t *main_data, size_t main_data_size,
        uint32_t table_off, uint32_t table_count,
        const uint8_t *palette, size_t palette_size) {
    if (!main_data || !buff_ram) { geo->captured_count = 0; return; }
    geo3d_lookup_build(main_data, main_data_size, table_off, table_count);

    geo3d__dl_snapshot_prev(geo);
    geo->captured_count = 0;

    geo3d_dl_walk_t w = { .geo = geo, .buff_ram = buff_ram, .buff_words = buff_words,
                          .main_data = main_data, .main_data_size = main_data_size,
                          .table_off = table_off, .palette = palette, .palette_size = palette_size,
                          .have_mat = false };
    uint32_t p = (rstart & (buff_words * 4u - 1u)) / 4u;   /* wrap within bufferram */
    for (uint32_t guard = 0; guard < buff_words && p < buff_words
                             && geo->captured_count < MAX_GEO_MODELS; guard++) {
        uint32_t op = buff_ram[p];
        if (op & 0x80000000u) break;                          /* JUMP -> flat list ends */
        uint32_t cmd = (op >> 23) & 0x1f;
        if (cmd == 0xf || cmd == 0x1f) break;                 /* END */

        if (!geo3d__dl_command(&w, p, cmd)) break;
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
/* 0: a mode with bit 1 set lights and culls with the ROM normal anyway (geo3d_board_normal). */
static int            g_geo3d_nn_normals  = 1;

/* The eye-space normal the geometrizer lights and culls a face with, from its
 * transformed corners A, B, C (host space, z negated). In a mode with bit 1 set
 * (geo_parse_nn_*) the ROM normal is skipped and the normal is worked out from
 * the corners instead: (B - A) x (C - A), normalized, the two corners the strip
 * carries and the link's first new point. Otherwise it is the ROM normal fn
 * through the matrix. The cross is negated because the z flip mirrors it.
 *
 * *Symptom that surfaced this in STF:* fighter shadows came out shredded.
 * rob_kage_disp_test draws them in mode 2 (set_mmode), under a matrix that
 * flattens the fighter onto the floor. A ROM normal put through that matrix
 * says nothing about which way the flattened face points, so the rear test
 * dropped faces at random, most visibly Knuckles' dreadlocks at select. */
#ifndef GEO3D_RSQRTF
#define GEO3D_RSQRTF(x) (1.0f / sqrtf(x))
#endif
static inline vec3_t geo3d_board_normal(const float *matrix, vec3_t fn, bool has_c,
                                        vec3_t A, vec3_t B, vec3_t C) {
    vec3_t n;
    if ((g_geo3d_mode & 2u) && has_c && g_geo3d_nn_normals) {
        float e1x = B.x - A.x, e1y = B.y - A.y, e1z = B.z - A.z;
        float e2x = C.x - A.x, e2y = C.y - A.y, e2z = C.z - A.z;
        n.x = -(e1y * e2z - e1z * e2y);
        n.y = -(e1z * e2x - e1x * e2z);
        n.z = -(e1x * e2y - e1y * e2x);
        float l2 = n.x * n.x + n.y * n.y + n.z * n.z;
        if (l2 != 0.0f) { float k = GEO3D_RSQRTF(l2); n.x *= k; n.y *= k; n.z *= k; }
        return n;
    }
    n.x = matrix[0]*fn.x + matrix[1]*fn.y + matrix[2]*fn.z;
    n.y = matrix[4]*fn.x + matrix[5]*fn.y + matrix[6]*fn.z;
    n.z = matrix[8]*fn.x + matrix[9]*fn.y + matrix[10]*fn.z;
    return n;
}
/* The object command's texture point and header addresses, over the model
 * table's (0xFFFFFFFF: none). */
static uint32_t       g_geo3d_obj_tpa     = 0xFFFFFFFFu;
static uint32_t       g_geo3d_obj_tha     = 0xFFFFFFFFu;
static const uint8_t *g_geo3d_palram      = NULL;
static size_t         g_geo3d_palram_size = 0;

static inline bool geo3d_tex_word(const uint8_t *rom, size_t rom_size, uint32_t addr, uint16_t *out) {
    if (addr & 0x800000u) { *out = g_geo_rs->texram[addr & 0xFFFFu]; return true; }
    size_t b = (size_t)addr * 2u;
    if (!rom || b + 2 > rom_size) return false;
    *out = (uint16_t)geo3d_rom16(rom + b);
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
 * Returns false (and captures nothing) when the list does not reach END: the
 * frame then draws no 3D, as there is nothing the board would draw either. */

static inline bool geo3d_scan_geo_list(geo3d_state_t *geo,
        const uint32_t *words, uint32_t nw, uint32_t rstart,
        int16_t hsync, int16_t vsync,
        const uint8_t *main_data, size_t main_data_size,
        uint32_t table_off, uint32_t table_count) {
    if (!main_data || !words) return false;
    geo3d_lookup_build(main_data, main_data_size, table_off, table_count);

#ifndef GEO3D_DC_SINK   /* the Dreamcast draws no in-between frames */
    if (g_cop.geo_frame_end != geo->last_frame_end) {
        memcpy(geo->captured_prev, geo->captured,
               (size_t)geo->captured_count * sizeof(captured_model_t));
        geo->captured_prev_count = geo->captured_count;
        geo->last_frame_end      = g_cop.geo_frame_end;
    }
#endif

    #define GEOL_S12(v) ((int16_t)(((v) & 0x800) ? (int)(v) - 0x1000 : (int)(v)))
    float    raw[12] = { 1,0,0, 0,1,0, 0,0,1, 0,0,0 };   /* column-major, as the COP wrote it */
    float    fx = 280.0f, fy = 280.0f;
    float    light[3] = { 0.0f, 0.0f, 1.0f };
    /* Mode and LOD are the geometrizer's own state and outlive a list. */
    static uint32_t mode = 0;
    static float    lod  = 0.0f;
    static uint32_t zadj = 0;
    int16_t  wvp[4]  = { 0, 0, 496, 384 };                /* viewport, list coordinates */
    int16_t  wc[4][2] = { {248, 192}, {248, 192}, {248, 192}, {248, 192} };
    int      window = 0;
    float    xoff = 84.0f + hsync, yoff = 130.0f + vsync;
    int      count = 0;
    bool     ended = false;
    geo->direct_used = 0;
    /* Where a draw lands: the projection, the window's scissor and the state
     * the geometrizer holds for it. */
    #define GEOL_PLACE(cm, sel) do {                                       \
        (cm)->view_space = true;                                           \
        (cm)->gproj[0] = fx;                                               \
        (cm)->gproj[1] = fy;                                               \
        (cm)->gproj[2] = xoff + wc[sel][0];                                \
        (cm)->gproj[3] = (384.0f - wc[sel][1]) + yoff;                     \
        (cm)->vp[0] = (int16_t)(wvp[0] + xoff);                            \
        (cm)->vp[1] = (int16_t)((384 - wvp[3]) + yoff);                    \
        (cm)->vp[2] = (int16_t)(wvp[2] + xoff);                            \
        (cm)->vp[3] = (int16_t)((384 - wvp[1]) + yoff);                    \
        (cm)->window   = (uint16_t)window;                                 \
        (cm)->light[0] = light[0]; (cm)->light[1] = light[1]; (cm)->light[2] = -light[2]; \
        (cm)->geo_mode = mode;                                             \
        (cm)->geo_lod  = lod;                                              \
        (cm)->zadjust  = zadj;                                             \
        (cm)->tpa = A(0);                                                  \
        (cm)->tha = A(1);                                                  \
    } while (0)

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
                    cm->material_ptr = read_u32_le(GEO3D_ROM(main_data + table_off + (uint32_t)model_idx * MODEL_ENTRY_SIZE + 4, 4));
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
                GEOL_PLACE(cm, (op >> 29) & 3u);
                cm->dbg_mesh_ptr = A(2);
                cm->dbg_pos[0] = M[9]; cm->dbg_pos[1] = M[10]; cm->dbg_pos[2] = M[11];
                cm->dbg_have_mat = 1;
                break;
            }
            case 0x02: case 0x12: {                            /* direct data: polygons in the list */
                len = geodl_direct_len(words, nw, p);
                if (p + 1u + len > nw || geo->direct_used + len > GEO3D_DIRECT_WORDS) break;
                captured_model_t *cm = &geo->captured[count++];
                memset(cm, 0, sizeof *cm);
                cm->model_idx  = -1;
                cm->direct_off = geo->direct_used;
                cm->direct_len = len;
                memcpy(geo->direct_words + geo->direct_used, words + p + 1u, len * 4u);
                geo->direct_used += len;
                /* The raster command is (opcode >> 23) - 1, whose centre select
                 * (bits 6..7) is always 0 (MAME geo_direct_data). */
                GEOL_PLACE(cm, 0u);
                break;
            }
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
            /* z-sort mode: the raster keeps (word >> 8) << 8 as its z_adjust
             * (model2_v.cpp geo_zsort_mode, raster command 08) */
            case 0x08: case 0x18: len = 1; zadj = A(0) & 0xFFFFFF00u; break;
            case 0x10: case 0x1E: len = 1; break;
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
    #undef GEOL_PLACE
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

/* What models are decoded from: the three ROMs and where the game keeps its
 * model table (the profile's quirks). The renderer builds one a frame
 * (game_render.h, geo3d_models_of). */
typedef struct {
    const uint8_t *main_data;  size_t main_data_size;
    const uint8_t *polygons;   size_t polygons_size;
    const uint8_t *materials;  size_t materials_size;   /* the textures ROM */
    uint32_t table_off, table_count;
    uint32_t mesh_ptr_subtract, mesh_ptr_add;
    /* A mesh in polygon RAM rather than ROM (an object address without bit 23):
     * when set, the decoder reads it from here, at offset 0, in place of
     * model_idx's, with the object command's texture addresses. */
    const uint8_t *obj_mesh;   size_t obj_mesh_size;
} geo3d_models_t;

/* Where a model's mesh, face records and UV stream start: its model-table
 * entry, with the object command's texture addresses over the two streams.
 * mat_ptr and uv_ptr are 0 without a textures ROM. False when the model has
 * no mesh. */
static inline bool geo3d_model_streams(const geo3d_models_t *md, int model_idx,
                                       uint32_t *mesh_offset, uint32_t *mat_ptr, uint32_t *uv_ptr) {
    if (model_idx < 0 || (uint32_t)model_idx >= md->table_count) return false;
    uint32_t toff = md->table_off + (uint32_t)model_idx * MODEL_ENTRY_SIZE;
    if ((size_t)toff + MODEL_ENTRY_SIZE > md->main_data_size) return false;
    uint32_t mesh_ptr_raw = read_u32_le(GEO3D_ROM(md->main_data + toff + 8, 4));
    if (mesh_ptr_raw == 0) return false;
    *mesh_offset = mesh_ptr_raw * 4u - md->mesh_ptr_subtract + md->mesh_ptr_add;
    *mat_ptr = *uv_ptr = 0;
    if (md->materials) {
        *mat_ptr = read_u32_le(GEO3D_ROM(md->main_data + toff + 4, 4));   /* word address (bit 23: texture RAM) */
        *uv_ptr  = read_u32_le(GEO3D_ROM(md->main_data + toff + 0, 4));   /* word index; byte = *2 */
        if (g_geo3d_obj_tha != 0xFFFFFFFFu) *mat_ptr = g_geo3d_obj_tha;
        if (g_geo3d_obj_tpa != 0xFFFFFFFFu) *uv_ptr  = g_geo3d_obj_tpa;
    }
    return true;
}

static bool geo3d_mesh_layers_for(const geo3d_models_t *md, int model_idx,
                                  uint32_t mat_ptr, uint32_t uv_ptr, uint32_t mesh_offset,
                                  geo3d_face_layer_t *out, int cap);

/* The texture header walk (MAME model2_3d_process_polygon): a polygon reads its
 * header at the current address, then moves the address by the signed record
 * count in bits 12-16 of its attribute, every polygon, culled ones included.
 * Sonic The Fighters and Fighting Vipers store 1 on every face that draws and 0
 * on the groups that do not, which is one record per emitted face; The House of
 * the Dead reuses a header for a run of faces and steps back as well (the
 * explorer's model.js, from the HOTD prototype). */
static inline uint32_t geo3d_tho_step(uint32_t attr) {
    int32_t tho = (int32_t)((attr >> 12) & 0x1Fu);
    if (tho & 0x10) tho -= 0x20;
    return (uint32_t)(tho * 4);
}

/* A triangle's record holds one new point, and the geometrizer takes the slot
 * for a second with the first (MAME geo_parse_*, the raster's `rope of P1(n)`),
 * which is what a polygon linking off it gets. STF and FV store the first point
 * again there; HOTD stores zeros, which taken literally reach back to the
 * part's origin. The record a triangle's attribute describes is the next one. */
static inline void geo3d_relink_triangles(vec3_t *sv, uint32_t *svk, int n_sv,
                                          const int *qt, int n_qt) {
    for (int k = 0; k + 1 < n_qt; k++) {
        if (qt[k] != 2) continue;
        int a = 2 * (k + 1);
        if (a + 1 >= n_sv) break;
        sv[a + 1] = sv[a];
        svk[a + 1] = svk[a];
    }
}

/* The index-array walk of one mesh (CLAUDE.md, "3D Polygon Decoder"): the
 * corners, the per-record triangle flag, normal and attribute, and the index
 * array the face loop reads four at a time. With a matrix the corners come out
 * transformed; their keys (geo3d_corner_key) are always taken before. */
typedef struct {
    vec3_t   sv[GEO3D_IA_MAX_VERTS];
    uint32_t svk[GEO3D_IA_MAX_VERTS];   /* geo3d_corner_key, pre-transform */
    int      qt[GEO3D_IA_MAX_VPS];      /* the record's f1 (2: a triangle) */
    vec3_t   qn[GEO3D_IA_MAX_VPS];      /* the record's polygon normal (z negated like the points) */
    uint32_t qa[GEO3D_IA_MAX_VPS];      /* the record's attribute word */
    int      idx[GEO3D_IA_MAX_IDX];
    int      n_sv, n_qt, n_idx;
} geo3d_ia_t;

static inline void geo3d_ia_walk(geo3d_ia_t *ia, const uint8_t *polygons, size_t polygons_size,
                                 uint32_t mesh_offset, const float *matrix) {
    vec3_t *sv = ia->sv;
    uint32_t *svk = ia->svk;
    int *idx = ia->idx;
    int n_sv = 0, n_qt = 0, n_idx = 4, vcount = 0;

    /* Initial Index: placeholder group that becomes the first face. */
    idx[0] = 0; idx[1] = 1; idx[2] = 2; idx[3] = 3;

    while (vcount < GEO3D_IA_MAX_VPS) {
        if ((size_t)mesh_offset + VERTEX_PAIR_SIZE > polygons_size) break;
        if (n_sv + 2 > GEO3D_IA_MAX_VERTS || n_idx + 4 > GEO3D_IA_MAX_IDX) break;

        const uint8_t *vp = GEO3D_ROM(polygons + mesh_offset, VERTEX_PAIR_SIZE);
        bool is_end = (vp[24] == 0 && vp[25] == 0 && vp[26] == 0 && vp[27] == 0);

        vec3_t v1 = { read_float_le(vp + 0),  read_float_le(vp + 4),  -read_float_le(vp + 8) };
        vec3_t v2 = { read_float_le(vp + 12), read_float_le(vp + 16), -read_float_le(vp + 20) };
        uint8_t f1    = vp[24];
        uint8_t iflag = vp[25] & 0x03;

        svk[n_sv]     = geo3d_corner_key(v1);
        svk[n_sv + 1] = geo3d_corner_key(v2);
        if (matrix) { v1 = apply_matrix(v1, matrix); v2 = apply_matrix(v2, matrix); }
        sv[n_sv++] = v1;
        sv[n_sv++] = v2;
        ia->qn[n_qt] = (vec3_t){ read_float_le(vp + 28), read_float_le(vp + 32), -read_float_le(vp + 36) };
        ia->qa[n_qt] = read_u32_le(vp + 24);
        ia->qt[n_qt++] = f1;

        int new_a = 2 * (vcount + 2);   /* vertex index of this record's v1 */

        switch (iflag) {
            case 0:
                /* The board culls a polygon whose own link type is 0 (MAME
                 * check_culling), so the group standing here is wiped, and at the
                 * head of a mesh that is the mesh's first polygon too. AM2's
                 * one-link 2x2 shadow card (70 STF entries) is exactly that and
                 * draws nothing. */
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

    geo3d_relink_triangles(sv, svk, n_sv, ia->qt, n_qt);
    ia->n_sv = n_sv; ia->n_qt = n_qt; ia->n_idx = n_idx;
}

/* Face fi's attribute word, 0 past the records. */
static inline uint32_t geo3d_ia_attr(const geo3d_ia_t *ia, int fi) {
    return (fi < ia->n_qt) ? ia->qa[fi] : 0u;
}

/* One texture header (MAME model2_3d_process_polygon): the tile rect, the sheet,
 * the lumaram band, the colour index and the face flags the fill reads. */
typedef struct {
    uint32_t texx, texy, texw, texh, sheet;
    uint32_t lumabase;      /* texheader[1] low byte << 7 (lumaram band) */
    uint32_t matidx;        /* colorbase → palette */
    uint32_t fflags;        /* GEO3D_FACE_* */
    bool     textured;      /* texheader[0] bit14 */
    bool     untex_trans;   /* untextured + transparent: the board draws nothing */
} geo3d_texhdr_t;

#define GEO3D_TEXHDR_NONE ((geo3d_texhdr_t){ .texw = 32, .texh = 32 })

static inline geo3d_texhdr_t geo3d_texhdr_decode(const uint16_t th[4]) {
    const uint16_t th0 = th[0], th2 = th[2];
    geo3d_texhdr_t h = GEO3D_TEXHDR_NONE;
    h.lumabase = (uint32_t)(th[1] & 0xff) << 7;
    h.textured = (th0 & 0x4000) != 0;
    h.texw  = 32u << (th0 & 0x7);
    h.texh  = 32u << ((th0 >> 3) & 0x7);
    h.texx  = 32u * (th2 & 0x3f);
    h.texy  = 32u * ((th2 >> 6) & 0x1f);
    h.sheet = (th2 >> 12) & 1u;                 /* which texram bank */
    if (h.textured && (th0 & 0x2000)) h.fflags |= GEO3D_FACE_TRANSPARENT;
    h.untex_trans = !h.textured && (th0 & 0x2000);
    if (th0 & 0x8000)               h.fflags |= GEO3D_FACE_CHECKER;
    if (h.sheet)                    h.fflags |= GEO3D_FACE_SHEET1;
    if ((th0 >> 8) & 1)             h.fflags |= GEO3D_FACE_MIRROR_X;
    if ((th0 >> 9) & 1)             h.fflags |= GEO3D_FACE_MIRROR_Y;
    if ((th0 >> 6) & 1)             h.fflags |= GEO3D_FACE_WRAP_X;
    if ((th0 >> 7) & 1)             h.fflags |= GEO3D_FACE_WRAP_Y;
    h.matidx = (th[3] >> 6) & 0x3ff;
    return h;
}

/* The header's four words at word address hw; false (and th untouched past the
 * first word missing) when any is out of the ROM. */
static inline bool geo3d_texhdr_words(const uint8_t *materials, size_t materials_size,
                                      uint32_t hw, uint16_t th[4]) {
    return geo3d_tex_word(materials, materials_size, hw,      &th[0]) &&
           geo3d_tex_word(materials, materials_size, hw + 1u, &th[1]) &&
           geo3d_tex_word(materials, materials_size, hw + 2u, &th[2]) &&
           geo3d_tex_word(materials, materials_size, hw + 3u, &th[3]);
}

/* The atlas tile rect (pixels) handed to the shader for the per-pixel wrap;
 * w = 0 means untextured (flat colour). */
static inline void geo3d_texhdr_tile(const geo3d_texhdr_t *h, float *tx, float *ty, float *tw, float *th) {
    *tx = (float)h->texx;
    *ty = (float)(h->sheet * GEO3D_SHEET_H + h->texy);
    *tw = h->textured ? (float)h->texw : 0.0f;
    *th = (float)h->texh;
}

/* A colour index's flat colour: palette RAM when the board has one, else the
 * ROM palette. Leaves the colour alone when neither holds the index. */
static inline void geo3d_palette_color(uint32_t matidx, const uint8_t *main_data, size_t main_data_size,
                                       float *r, float *g, float *b) {
    uint32_t pal = GEO3D_PALETTE_OFF + matidx * 2u;
    uint32_t ram = (matidx + 0x1000u) * 2u;
    if (g_geo3d_palram && (size_t)ram + 2 <= g_geo3d_palram_size) {
        uint16_t cw = (uint16_t)(g_geo3d_palram[ram] | (g_geo3d_palram[ram + 1] << 8)) & 0x7FFF;
        geo3d_bgr555(cw, r, g, b);
    } else if (main_data && (size_t)pal + 2 <= main_data_size) {
        uint16_t cw = (uint16_t)geo3d_rom16(main_data + pal);
        geo3d_bgr555(cw, r, g, b);
    }
}

/* One face's texture points: nv (pv,pu) pairs from word uv_word, put in slots
 * 0=A 1=B 2=C 3=D as tile-relative texels (they may run past the tile; the
 * shader wraps). The stream runs B,A,C,D for a quad and B,A,C for a triangle.
 * Untextured faces keep 0 so the shader uses the flat colour. Returns the pairs
 * the ROM held. The caller advances the stream by nv pairs whatever this read,
 * to stay aligned with MAME. */
static inline int geo3d_uv_read(const uint8_t *materials, size_t materials_size, uint32_t uv_word,
                                bool tri, bool textured, float uvu[4], float uvv[4]) {
    static const int quad_slot[4] = {1,0,2,3};  /* stream k → B,A,C,D */
    static const int tri_slot[3]  = {1,0,2};    /* stream k → B,A,C   */
    const int *slot = tri ? tri_slot : quad_slot;
    const int nv = tri ? 3 : 4;
    int k;
    for (k = 0; k < nv; k++) {
        uint32_t tw_ = uv_word + (uint32_t)k * 2u;
        uint16_t pv = 0, pu = 0;
        if (!geo3d_tex_word(materials, materials_size, tw_, &pv) ||
            !geo3d_tex_word(materials, materials_size, tw_ + 1u, &pu)) break;
        if (!textured) continue;
        uvu[slot[k]] = (float)pu / 8.0f;
        uvv[slot[k]] = (float)pv / 8.0f;
    }
    return k;
}

/* The geometrizer's view of a face (MAME geo_parse): its normal, N·L and N·P
 * with the link's first new point, C, the one the geometrizer reads after the
 * normal. A warped quad's ROM normal does not hold all four corners (B and C
 * sit off its plane in about one STF face in seven), so the corner matters. */
typedef struct { float nz, dotl, dotp; uint32_t at; } geo3d_lit_t;

/* Fills lt and answers check_culling: a face whose attribute word lacks the
 * double-sided bit 17 is dropped when N·P < 0 (the rear), and a link of type 0
 * (bits 8..9) is never drawn. */
static inline bool geo3d_board_cull(const float *matrix, vec3_t fn, uint32_t at, bool has_c,
                                    vec3_t A, vec3_t B, vec3_t C, geo3d_lit_t *lt) {
    const vec3_t N = geo3d_board_normal(matrix, fn, has_c, A, B, C);
    float nx = N.x, ny = N.y, nz = N.z;
    const vec3_t P = has_c ? C : A;
    lt->nz   = nz;
    lt->dotl = nx*g_light_dir[0] + ny*g_light_dir[1] + nz*g_light_dir[2];
    lt->dotp = nx*P.x + ny*P.y + nz*P.z;
    lt->at   = at;
    return (((at >> 17) & 1u) == 0 && lt->dotp < 0.0f) || ((at >> 8) & 3u) == 0;
}

/* The face's poly luma (0..1) from the list's texture parameters, with
 * specular when the mode word asks for it, and its texture LOD into
 * g_geo3d_emit_texlod. Both belong to the instance (the mode word, the LOD
 * scale, the eye-space normal), so the mesh cache never keeps them. */
static inline float geo3d_board_luma(const geo3d_lit_t *lt) {
    const uint32_t at = lt->at;
    const float dotl = lt->dotl, dotp = lt->dotp;
    float lum  = (dotl * dotp < 0.0f) ? 0.0f : fabsf(dotl);
    const float *tp = g_geo_rs->texparam[(at >> 18) & 0x1F];
    float spec = 0.0f;
    if (g_geo3d_mode & 1u) {
        /* Board z is this space's -z, for the normal and the light alike. */
        uint32_t ctl = (uint32_t)tp[3];
        spec = g_light_dir[2] - 2.0f * dotl * lt->nz;
        if (spec < 0.0f || ctl == 0) spec = 0.0f;
        if ((ctl >> 1) != 0) spec *= spec;
        if ((ctl >> 2) != 0) spec *= spec;
        if (((ctl + 1) >> 3) != 0) spec *= spec;
        spec *= tp[2];
    }
    float luma = lum * tp[0] + tp[1] + spec;
    if (luma < 0.0f) luma = 0.0f;
    if (luma > 255.0f) luma = 255.0f;
    /* The rasterizer gets f2u(distance) >> 8 and splits it: exponent
     * (bits 23-30) as the integer part, the next 15 bits' log from log
     * RAM as the fraction. A zero distance gives texlod 0 (the oracle's
     * model2_v.cpp; stock MAME would pick the coarsest level). */
    float dist = g_geo_rs->coef[at >> 27] * fabsf(dotp) * g_geo3d_lod;
    uint32_t db;
    memcpy(&db, &dist, 4);
    g_geo3d_emit_texlod = (db >> 8) == 0 ? 0.0f
        : (float)((int)((db >> 16) & 0x7F80u) - 0x3F80 + (int)g_geo_rs->logram[(db >> 8) & 0x7FFFu]);
    return (float)(int)luma / 255.0f;
}

/* The object viewer's light: ambient plus |N·L| of the face's own edges. */
static inline float geo3d_approx_light(vec3_t A, vec3_t B, vec3_t C) {
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
    return shade;
}

/* What a face is filled with: flat colour, atlas tile, lumaram band, poly luma,
 * face flags and the four corners' texels (slots A, B, C, D). */
typedef struct {
    float r, g, b;
    float tx, ty, tw, th;
    float lb, pl, fl;
    const float *u, *v;
} geo3d_paint_t;

static inline void geo3d_emit_paint_tri(vec3_t P, int i, vec3_t Q, int j, vec3_t R, int k,
                                        const geo3d_paint_t *p) {
    geo3d_emit_tri_uv(P.x,P.y,P.z, p->u[i],p->v[i],
                      Q.x,Q.y,Q.z, p->u[j],p->v[j],
                      R.x,R.y,R.z, p->u[k],p->v[k], p->r,p->g,p->b, p->tx,p->ty,p->tw,p->th, p->lb,p->pl, p->fl);
}

/* One face: its edges (with lines) and its fill. A quad is ABD + ADC (slots
 * 0,1,3 / 0,3,2), or ABC + BDC along the other diagonal when a decal copy must
 * match an earlier cut (geo3d_split_other_way, with the corner keys' sums). A
 * "triangle" with no C is one edge and no fill. */
static inline void geo3d_emit_face(vec3_t A, vec3_t B, vec3_t C, vec3_t D, bool is_tri, bool has_c,
                                   bool lines, uint32_t split_quad, uint32_t split_cut,
                                   const geo3d_paint_t *p) {
    const float r = p->r, g = p->g, b = p->b;
    if (is_tri) {
        if (has_c) {
            if (lines) {
                geo3d_emit_line(A.x,A.y,A.z, B.x,B.y,B.z, r,g,b);
                geo3d_emit_line(B.x,B.y,B.z, C.x,C.y,C.z, r,g,b);
                geo3d_emit_line(C.x,C.y,C.z, A.x,A.y,A.z, r,g,b);
            }
            geo3d_emit_paint_tri(A, 0, B, 1, C, 2, p);
        } else if (lines) {
            geo3d_emit_line(A.x,A.y,A.z, B.x,B.y,B.z, r,g,b);
        }
        return;
    }
    if (lines) {
        /* Quad winding: A-B, B-D, D-C, C-A */
        geo3d_emit_line(A.x,A.y,A.z, B.x,B.y,B.z, r,g,b);
        geo3d_emit_line(B.x,B.y,B.z, D.x,D.y,D.z, r,g,b);
        geo3d_emit_line(D.x,D.y,D.z, C.x,C.y,C.z, r,g,b);
        geo3d_emit_line(C.x,C.y,C.z, A.x,A.y,A.z, r,g,b);
    }
    if (geo3d_split_other_way(split_quad, split_cut)) {
        geo3d_emit_paint_tri(A, 0, B, 1, C, 2, p);
        geo3d_emit_paint_tri(B, 1, D, 3, C, 2, p);
    } else {
        geo3d_emit_paint_tri(A, 0, B, 1, D, 3, p);
        geo3d_emit_paint_tri(A, 0, D, 3, C, 2, p);
    }
}

/* The emit globals back to "no face" after a model. */
static inline void geo3d_emit_state_reset(void) {
    g_geo3d_emit_texlod    = GEO3D_TEXLOD_NONE;
    g_geo3d_emit_zs        = GEO3D_ZSORT_NONE;
    g_geo3d_emit_flat      = -1.0f;
    g_geo3d_emit_layer     = 0.0f;
    g_geo3d_emit_has_plane = 0;
}

/* A face's depth for the full decoder: the board's sort z, the flat key on a
 * game frame (flat), and the object viewer's coplanar layer and plane (lay,
 * NULL when there are none; far-corner faces keep the recede alone). */
static inline void geo3d_face_depth(const vec3_t *sv, const int zsrc[4], uint32_t zmode, bool flat,
                                    const geo3d_face_layer_t *lay, const float *matrix) {
    g_geo3d_emit_zs = geo3d_sort_z(sv, zsrc, zmode);
    g_geo3d_emit_flat = flat ? geo3d_flat_depth(sv, zsrc, zmode) : -1.0f;
    const bool lay_face = lay && zmode != 2u && zmode != 3u;
    g_geo3d_emit_layer     = lay_face ? (float)lay->layer : 0.0f;
    g_geo3d_emit_has_plane = 0;
    if (lay_face && lay->has_plane) {
        if (matrix) g_geo3d_emit_has_plane = geo3d_plane_to_view(lay->plane, matrix, g_geo3d_emit_plane);
        else { memcpy(g_geo3d_emit_plane, lay->plane, sizeof g_geo3d_emit_plane); g_geo3d_emit_has_plane = 1; }
    }
    if (g_geo3d_emit_zs == GEO3D_ZSORT_KEY0) { g_geo3d_emit_layer = 0.0f; g_geo3d_emit_has_plane = 0; }
    if (g_geo3d_emit_layer > 0.0f || g_geo3d_emit_has_plane) g_geo3d_layer_faces++;
}

/* model_tex.txt's head, for face 0: the model and its neighbouring
 * model-table entries: uv_ptr(+0)/mat_ptr(+4)/mesh_ptr(+8). UV-stream length
 * for this model = uv_ptr[next] - uv_ptr[this]. */
static inline void geo3d_dump_tex_head(FILE *mtf, const geo3d_models_t *md, int model_idx) {
    fprintf(mtf, "# model %d texture tiles  th0 th2 th3 -> sheet (texx,texy) texw x texh\n", model_idx);
    for (int mi = model_idx - 1; mi <= model_idx + 2; mi++) {
        if (mi < 0 || (uint32_t)mi >= md->table_count) continue;
        uint32_t te = md->table_off + (uint32_t)mi * MODEL_ENTRY_SIZE;
        if ((size_t)te + MODEL_ENTRY_SIZE > md->main_data_size) continue;
        fprintf(mtf, "# table[%d]: uv_ptr=%u mat_ptr=%u mesh_ptr=%u\n", mi,
                read_u32_le(GEO3D_ROM(md->main_data + te + 0, 4)), read_u32_le(GEO3D_ROM(md->main_data + te + 4, 4)),
                read_u32_le(GEO3D_ROM(md->main_data + te + 8, 4)));
    }
}

/* Raw UV-stream window around the first cone face, to find the real (pv,pu)
 * pairs and the correct per-face stride. */
static inline void geo3d_dump_tex_raw(FILE *mtf, const geo3d_models_t *md, uint32_t uv_word) {
    const uint8_t *materials = md->materials;
    fprintf(mtf, "  -- raw UV stream u16 (pv,pu) from model start uv_word=%u --\n", uv_word);
    for (int w = 0; w < 48; w += 2) {
        long bo = ((long)uv_word + w) * 2;
        if (bo >= 0 && (size_t)bo + 4 <= md->materials_size) {
            uint16_t v0 = (uint16_t)geo3d_rom16(materials + bo);
            uint16_t v1 = (uint16_t)geo3d_rom16(materials + bo + 2);
            fprintf(mtf, "  word %+3d (off %u): pv=%5u pu=%5u\n", w, uv_word + w, v0, v1);
        }
    }
}

/* A corner for the dump: the vertex, or zeros when the index is out of range. */
static inline vec3_t geo3d_dump_corner(const geo3d_ia_t *ia, int k) {
    return (k >= 0 && k < ia->n_sv) ? ia->sv[k] : (vec3_t){ 0.0f, 0.0f, 0.0f };
}

/* Debug dump of one model's per-face texture tiles (GEO3D_DUMP_TEX), to
 * model_tex.txt; face 0 starts the file with the neighbouring model-table
 * entries and a raw window of the UV stream. */
static inline void geo3d_dump_face_tex(const geo3d_models_t *md, int model_idx, const geo3d_ia_t *ia,
                                       int fi, int ai, int bi, int di, int nv, uint32_t hw,
                                       const uint16_t th[4], const geo3d_texhdr_t *h,
                                       bool have_uv, uint32_t uv_word, float fr, float fg, float fb) {
    static FILE *mtf = NULL;
    const int n_sv = ia->n_sv;
    if (fi == 0) { if (mtf) fclose(mtf); mtf = fopen("model_tex.txt", "w");
        if (mtf) geo3d_dump_tex_head(mtf, md, model_idx); }
    if (!mtf) return;
    int _skip = (ai < 0 || ai >= n_sv || bi < 0 || bi >= n_sv);
    const vec3_t A = geo3d_dump_corner(ia, ai), D = geo3d_dump_corner(ia, di);
    fprintf(mtf,
        "face %3d: th0=%04X th2=%04X th3=%04X  textured=%d nv=%d f1=%d ai=%d bi=%d SKIP=%d have_uv=%d uv_word=%u sheet=%u tile=(%4u,%4u) %ux%u colorbase=%u hdr=%u rgb=(%.2f,%.2f,%.2f) lb=%u fl=%u A=(%.2f,%.2f,%.2f) D=(%.2f,%.2f,%.2f)\n",
        fi, th[0], th[2], th[3], h->textured?1:0, nv, (fi < ia->n_qt ? ia->qt[fi] : -1),
        ai, bi, _skip, have_uv?1:0, uv_word, h->sheet, h->texx, h->texy, h->texw, h->texh, h->matidx, hw, fr, fg, fb, h->lumabase, h->fflags,
        A.x, A.y, A.z, D.x, D.y, D.z);
    if (fi == 0) geo3d_dump_tex_raw(mtf, md, uv_word);
    fflush(mtf);
}

/* Debug dump of a face's texture points (GEO3D_DUMP_TEX), to model_uv.txt. */
static inline void geo3d_dump_face_uv(int fi, int n_read, bool tri, const float uvu[4], const float uvv[4],
                                      float ftx, float fty, float ftw, float fth) {
    static FILE *uf = NULL;
    static const int quad_slot[4] = {1,0,2,3}, tri_slot[3] = {1,0,2};
    const int *slot = tri ? tri_slot : quad_slot;
    const int nv = tri ? 3 : 4;
    for (int k = 0; k < n_read; k++) {
        float tu = uvu[slot[k]], tv = uvv[slot[k]];
        if (fi == 0 && k == 0) { if (uf) fclose(uf); uf = fopen("model_uv.txt", "w"); }
        if (!uf) uf = fopen("model_uv.txt", "a");
        if (uf) { fprintf(uf, "face %2d k=%d nv=%d pu=%u pv=%u -> tu=%.1f tv=%.1f tile=(%.0f,%.0f) %.0fx%.0f\n",
                          fi, k, nv, (unsigned)(tu * 8.0f), (unsigned)(tv * 8.0f), tu, tv, ftx, fty, ftw, fth); fflush(uf); }
    }
}

/* One model's walk in geo3d_decode_model: what it reads, where the texture
 * header and UV streams stand, the faces' layers, and the board's z-sort,
 * carried from face to face. */
typedef struct {
    const geo3d_models_t *md;
    int            model_idx;
    const float   *matrix;
    float          cr, cg, cb;
    const geo3d_ia_t *ia;
    const uint8_t *polygons;
    size_t         polygons_size;
    uint32_t       mesh_offset;
    /* Texture header stream: word address (bit 23: texture RAM), walked by each
     * polygon's own step (geo3d_tho_step). */
    uint32_t       mat_word, mat_rec;
    bool           have_mat;
    /* UV stream — model-table[+0x00] word index into the textures ROM; per polygon
     * NumVerts (pv,pu) 16-bit pairs, tile-relative texel = raw/8 (geo3d_uv_read). */
    uint32_t       uv_word;
    bool           have_uv;
    bool           flat, have_lay;
    const geo3d_face_layer_t *lay;
    /* The board's z-sort, carried across the walk (see geo3d_sort_z). */
    int            zsrc[4];
    uint32_t       zmode;
    bool           zset;
} geo3d_decode_walk_t;

/* Where the model's mesh and its texture streams are: the model table's, or for
 * a polygon-RAM object the object's own. False when there is nothing to walk. */
static inline bool geo3d_decode_streams(geo3d_decode_walk_t *w) {
    const geo3d_models_t *md = w->md;
    w->mat_word = 0;
    w->have_mat = false;
    w->uv_word = 0;
    w->have_uv = false;
    w->polygons      = md->polygons;
    w->polygons_size = md->polygons_size;

    if (!md->main_data || !w->polygons) return false;
    if (md->obj_mesh) {
        /* A polygon-RAM object: the mesh starts at the object address, and its
         * texture addresses come from the object command. */
        w->polygons      = md->obj_mesh;
        w->polygons_size = md->obj_mesh_size;
        w->mesh_offset   = 0;
        if (md->materials && g_geo3d_obj_tha != 0xFFFFFFFFu) {
            w->mat_word = g_geo3d_obj_tha; w->have_mat = w->mat_word != 0;
            w->uv_word  = g_geo3d_obj_tpa; w->have_uv = w->uv_word != 0;
        }
    } else {
        if (!geo3d_model_streams(md, w->model_idx, &w->mesh_offset, &w->mat_word, &w->uv_word)) return false;
        w->have_mat = w->mat_word != 0;
        w->have_uv  = w->uv_word != 0;
    }
    return true;
}

/* The faces' layers, from the mesh cache, for a caller that may use it. A game
 * frame's draw takes the board's key instead (or, with zflat off, the half
 * rule alone, as the cached draw does) and has no use for them. */
static inline bool geo3d_decode_layers(const geo3d_decode_walk_t *w, bool game_draw,
                                       geo3d_face_layer_t *lay) {
    const geo3d_models_t *md = w->md;
    return !game_draw && g_geo3d_decode_layers && g_geo3d_layers && !md->obj_mesh && !g_geo_flat_color &&
           !(w->have_mat && (w->mat_word & 0x800000u)) && !(w->have_uv && (w->uv_word & 0x800000u)) &&
           geo3d_mesh_layers_for(md, w->model_idx, w->have_mat ? w->mat_word : 0u, w->have_uv ? w->uv_word : 0u,
                                 w->mesh_offset, lay, GEO3D_IA_MAX_IDX / 4);
}

/* A face's texture header (tile rect + palette index) and flat colour, from
 * the header stream, which it moves on by the polygon's own step. */
static inline geo3d_texhdr_t geo3d_decode_face_header(geo3d_decode_walk_t *w, int fi, uint32_t at,
                                                      int ai, int bi, int di, int nv,
                                                      float *fr, float *fg, float *fb) {
    const geo3d_models_t *md = w->md;
    geo3d_texhdr_t h = GEO3D_TEXHDR_NONE;
    if (w->have_mat) {
        uint32_t hw = w->mat_rec;
        w->mat_rec += geo3d_tho_step(at);
        uint16_t th[4] = { 0, 0, 0, 0 };
        if (geo3d_texhdr_words(md->materials, md->materials_size, hw, th)) {
            h = geo3d_texhdr_decode(th);
            geo3d_palette_color(h.matidx, md->main_data, md->main_data_size, fr, fg, fb);
            if (GEO3D_DUMP_TEX(w->model_idx))
                geo3d_dump_face_tex(md, w->model_idx, w->ia, fi, ai, bi, di, nv, hw, th, &h,
                                    w->have_uv, w->uv_word, *fr, *fg, *fb);
        }
    }

    /* Flat-colour override (homebrew display list): keep the caller's colour,
     * force untextured — model 456's ROM material/texture is meaningless here. */
    if (g_geo_flat_color) { h.textured = false; *fr = w->cr; *fg = w->cg; *fb = w->cb; h.fflags = 0; }
    return h;
}

/* A face's texture points, and the UV stream moved on past them. */
static inline void geo3d_decode_face_uv(geo3d_decode_walk_t *w, int fi, bool tri_cnt, int nv,
                                        const geo3d_texhdr_t *h, const geo3d_paint_t *paint,
                                        float uvu[4], float uvv[4]) {
    if (w->have_uv) {
        int n_read = geo3d_uv_read(w->md->materials, w->md->materials_size, w->uv_word, tri_cnt, h->textured, uvu, uvv);
        if (GEO3D_DUMP_TEX(w->model_idx) && fi <= 12 && h->textured)
            geo3d_dump_face_uv(fi, n_read, tri_cnt, uvu, uvv, paint->tx, paint->ty, paint->tw, paint->th);
    }
    w->uv_word += (uint32_t)nv * 2u;   /* nv (pv,pu) pairs per iteration (3 tri / 4 quad) */
}

/* Per-face luminance (poly_luma), passed through so the colorxlat luma
 * ramp can use it (luma6 = lumaram[lumabase+texel]*poly_luma/256), not
 * folded into the colour. No fallback to the approximation for a zero
 * normal: the board lights it too, to luminance 0, so the face gets
 * its ambient term. */
static inline float geo3d_decode_face_luma(const geo3d_decode_walk_t *w, int fi, uint32_t at, bool has_C,
                                           vec3_t A, vec3_t B, vec3_t C, bool *board_cull) {
    const geo3d_ia_t *ia = w->ia;
    float pl = 1.0f;
    g_geo3d_emit_texlod = GEO3D_TEXLOD_NONE;
    *board_cull = false;             /* the geometrizer would not draw this face */
    if (g_geo3d_board_luma && w->matrix) {
        geo3d_lit_t lt;
        vec3_t fn = (fi < ia->n_qt) ? ia->qn[fi] : (vec3_t){0, 0, 0};
        *board_cull = geo3d_board_cull(w->matrix, fn, at, has_C, A, B, C, &lt);
        pl = geo3d_board_luma(&lt);
    } else if (g_light_enable && has_C) {
        pl = geo3d_approx_light(A, B, C);
    }
    return pl;
}

/* Homebrew flat panels: the GEO brightens lit faces via the colorxlat luma
 * ramp (dark base hue + lit normal → bright panel). The shader's flat path
 * only darkens (×pl≤1), so the dark C_TUBE base never brightens. Approximate
 * the ramp by scaling the flat colour up with poly-luma, then neutralise the
 * shader's own ×pl so it isn't applied twice. */
static inline void geo3d_flat_color_boost(float *fr, float *fg, float *fb, float *pl) {
    if (g_geo_flat_color) {
        float boost = *pl * g_geo_flat_boost;
        *fr = fminf(*fr * boost, 1.0f);
        *fg = fminf(*fg * boost, 1.0f);
        *fb = fminf(*fb * boost, 1.0f);
        *pl = 1.0f;
    }
}

/* One iteration of the face loop: the face at index-array position i. The
 * header and UV streams move on whether or not it draws. */
static inline void geo3d_decode_face(geo3d_decode_walk_t *w, int i) {
    const geo3d_ia_t *ia = w->ia;
    const vec3_t *sv = ia->sv;
    const int n_sv = ia->n_sv;
    int fi = i / 4;
    int ai = ia->idx[i], bi = ia->idx[i + 1], ci = ia->idx[i + 2], di = ia->idx[i + 3];
    const uint32_t at = geo3d_ia_attr(ia, fi);
    bool tri_cnt = (fi < ia->n_qt && ia->qt[fi] == 2);
    int  nv = tri_cnt ? 3 : 4;

    /* ---- texture header (tile rect + palette index) ---- */
    float fr = w->cr, fg = w->cg, fb = w->cb;     /* flat color (palette) */
    geo3d_texhdr_t h = geo3d_decode_face_header(w, fi, at, ai, bi, di, nv, &fr, &fg, &fb);

    geo3d_paint_t paint;
    geo3d_texhdr_tile(&h, &paint.tx, &paint.ty, &paint.tw, &paint.th);

    float uvu[4] = {0,0,0,0}, uvv[4] = {0,0,0,0};
    geo3d_decode_face_uv(w, fi, tri_cnt, nv, &h, &paint, uvu, uvv);

    if (ai < 0 || ai >= n_sv) return;
    if (bi < 0 || bi >= n_sv) return;

    vec3_t A = sv[ai], B = sv[bi];
    bool has_C = (ci >= 0 && ci < n_sv);
    bool has_D = (di >= 0 && di < n_sv);
    vec3_t C = has_C ? sv[ci] : (vec3_t){0,0,0};
    vec3_t D = has_D ? sv[di] : (vec3_t){0,0,0};

    bool is_tri = tri_cnt || !has_C || !has_D;

    geo3d_zsort_step(at, is_tri, has_C, ai, bi, ci, di, w->zsrc, &w->zmode, &w->zset);
    geo3d_face_depth(sv, w->zsrc, w->zmode, w->flat, w->have_lay ? &w->lay[fi] : NULL, w->matrix);

    bool board_cull;
    float pl = geo3d_decode_face_luma(w, fi, at, has_C, A, B, C, &board_cull);
    geo3d_flat_color_boost(&fr, &fg, &fb, &pl);

    /* The untextured transparent renderer returns without writing a pixel
     * (model2rd.ipp draw_scanline_solid<true>). Only on the display-list
     * path, so the model tools keep comparing every face with the explorer. */
    if (g_geo3d_board_luma && (h.untex_trans || board_cull)) return;

    paint.r = fr; paint.g = fg; paint.b = fb;
    paint.lb = g_geo_flat_color ? -1.0f : (float)h.lumabase;
    paint.pl = pl;
    paint.fl = (float)h.fflags;
    paint.u = uvu; paint.v = uvv;
    uint32_t split_quad = 0, split_cut = 0;
    if (!is_tri) {
        uint32_t kA = ia->svk[ai], kB = ia->svk[bi], kC = ia->svk[ci], kD = ia->svk[di];
        split_quad = kA ^ kB ^ kC ^ kD;
        split_cut  = kA ^ kD;
    }
    geo3d_emit_face(A, B, C, D, is_tri, has_C, true, split_quad, split_cut, &paint);
}

static inline void geo3d_decode_model(const geo3d_models_t *md, int model_idx,
                                       const float *matrix,
                                       float cr, float cg, float cb) {
    static geo3d_ia_t ia;
    geo3d_decode_walk_t w = { .md = md, .model_idx = model_idx, .matrix = matrix,
                              .cr = cr, .cg = cg, .cb = cb, .ia = &ia };
    if (!geo3d_decode_streams(&w)) return;

    static geo3d_face_layer_t lay[GEO3D_IA_MAX_IDX / 4];
    const bool game_draw = g_geo3d_flat_list && matrix;
    w.flat = g_geo3d_zflat && game_draw;
    w.lay = lay;
    w.have_lay = geo3d_decode_layers(&w, game_draw, lay);

    geo3d_ia_walk(&ia, w.polygons, w.polygons_size, w.mesh_offset, matrix);

    /* Face loop, stopping 2 groups before the tail. The header stream moves by
     * each polygon's own step; the UV stream by NumVerts pairs every iteration,
     * sentinels included, exactly like MAME advances command_buffer[0]. */
    w.mat_rec = w.mat_word;
    geo3d_split_reset();
    for (int i = 0; i < ia.n_idx - 8; i += 4) geo3d_decode_face(&w, i);
    geo3d_emit_state_reset();
}

/* ---- Direct data -------------------------------------------------------------
 * GEO command 0x02/0x12 carries its polygons in the list itself (MAME
 * geo_direct_data): texture point and header addresses, two corners, then a
 * link per polygon of attribute, luma, distance and one more corner (two for a
 * quad), until an attribute whose low two bits are 0. The geometrizer passes
 * them to the raster untouched apart from dropping each word's low byte, so the
 * corners are already in view space with the focus applied (x * fx, y * fy, z),
 * the luma word holds the polygon's luma (bits 30..23) and its rear flag (bit
 * 31), and the distance word is the texture LOD's float. What the raster does
 * with them is model2_3d_process_polygon, as for an object's polygons: P0 and P1
 * of the previous link and the new corners make the polygon, the texture points
 * run on NumVerts pairs a polygon, the header moves by the attribute's signed
 * offset, and the link type says which corners the next polygon keeps. */
/* A corner as the raster holds it, put where geo3d_decode_model's are: the
 * host looks down -z. */
static inline vec3_t geo3d_direct_pt(const uint32_t *w, uint32_t q, float fx, float fy) {
    return (vec3_t){ u32_as_float(w[(q)] & 0xFFFFFF00u) / fx,
                     u32_as_float(w[(q) + 1u] & 0xFFFFFF00u) / fy,
                     -u32_as_float(w[(q) + 2u] & 0xFFFFFF00u) };
}

/* texture header and points, both advanced whether or not it draws */
static inline bool geo3d_direct_tex(const uint8_t *materials, size_t materials_size,
                                    uint32_t *tpa, uint32_t *tha, uint32_t attr, int nv,
                                    uint16_t th[4], uint16_t pv[4], uint16_t pu[4]) {
    bool have_th = true;
    for (uint32_t k = 0; k < 4u; k++)
        have_th = geo3d_tex_word(materials, materials_size, *tha + k, &th[k]) && have_th;
    for (int k = 0; k < nv; k++) {
        geo3d_tex_word(materials, materials_size, *tpa + 2u * (uint32_t)k,      &pv[k]);
        geo3d_tex_word(materials, materials_size, *tpa + 2u * (uint32_t)k + 1u, &pu[k]);
    }
    *tpa += 2u * (uint32_t)nv;
    *tha += geo3d_tho_step(attr);
    return have_th;
}

/* z-sort: every polygon sets the register, culled or not. Answers the
 * farthest corner's depth. */
static inline float geo3d_direct_zsort(const vec3_t v[4], int nv, uint32_t attr) {
    static float prev_zs = GEO3D_ZSORT_NONE;
    uint32_t zmode = (attr >> 10) & 3u;
    static const int zsrc[4] = { 0, 1, 2, 3 };
    g_geo3d_emit_flat = (g_geo3d_zflat && g_geo3d_flat_list) ? geo3d_flat_depth(v, zsrc, zmode) : -1.0f;
    if (zmode != 0u) prev_zs = geo3d_sort_z(v, zsrc, zmode);
    g_geo3d_emit_zs = prev_zs;
    float max_z = -v[0].z;
    for (int k = 1; k < nv; k++) if (-v[k].z > max_z) max_z = -v[k].z;
    return max_z;
}

/* One polygon that is drawn: its colour, texture and fill. */
static inline void geo3d_direct_emit(const vec3_t v[4], int nv, const uint16_t th[4],
                                     const uint16_t pv[4], const uint16_t pu[4],
                                     uint32_t lw, uint32_t dw,
                                     const uint8_t *main_data, size_t main_data_size) {
    const geo3d_texhdr_t h = geo3d_texhdr_decode(th);
    float fr = 0.7f, fg = 0.7f, fb = 0.7f;
    geo3d_palette_color(h.matidx, main_data, main_data_size, &fr, &fg, &fb);
    /* the untextured transparent renderer writes nothing */
    if (h.untex_trans) return;
    float uu[4], vv[4];
    for (int k = 0; k < 4; k++) { uu[k] = h.textured ? (float)pu[k] / 8.0f : 0.0f;
                                  vv[k] = h.textured ? (float)pv[k] / 8.0f : 0.0f; }
    float ftx, fty, ftw, fth;
    geo3d_texhdr_tile(&h, &ftx, &fty, &ftw, &fth);
    float pl = (float)((lw >> 15) & 0xFFu) / 255.0f;
    float lb = (float)h.lumabase, ffl = (float)h.fflags;
    g_geo3d_emit_texlod = dw == 0 ? 0.0f
        : (float)((int)((dw >> 8) & 0x7F80u) - 0x3F80 + (int)g_geo_rs->logram[dw & 0x7FFFu]);
    g_geo3d_emit_layer = 0.0f;
    g_geo3d_emit_has_plane = 0;
    /* the raster fills the polygon v0, v1, v2(, v3) */
    for (int k = 1; k + 1 < nv; k++)
        geo3d_emit_tri_uv(v[0].x, v[0].y, v[0].z, uu[0], vv[0],
                          v[k].x, v[k].y, v[k].z, uu[k], vv[k],
                          v[k + 1].x, v[k + 1].y, v[k + 1].z, uu[k + 1], vv[k + 1],
                          fr, fg, fb, ftx, fty, ftw, fth, lb, pl, ffl);
}

/* linking: which corners the next polygon starts from */
static inline void geo3d_direct_link(uint32_t attr, vec3_t *p0, vec3_t *p1, vec3_t c2, vec3_t c3) {
    switch ((attr >> 8) & 3u) {
        case 0: case 2: *p0 = c2; *p1 = c3; break;
        case 1:         *p1 = c2;           break;
        case 3:         *p0 = c3;           break;
    }
}

static inline void geo3d_decode_direct(const uint32_t *w, uint32_t n,
                                        const uint8_t *materials, size_t materials_size,
                                        const uint8_t *main_data, size_t main_data_size,
                                        float fx, float fy) {
    if (n < 8u || fx == 0.0f || fy == 0.0f) return;
    uint32_t tpa = w[0], tha = w[1];
    vec3_t p0 = geo3d_direct_pt(w, 2u, fx, fy), p1 = geo3d_direct_pt(w, 5u, fx, fy);   /* P0(n-1), P1(n-1) */
    uint32_t q = 8u;
    while (q < n) {
        uint32_t attr = w[q] & 0x00FFFFFFu;
        if ((attr & 3u) == 0 || q + 6u > n) break;
        const bool quad = (attr & 1u) != 0;
        if (quad && q + 9u > n) break;
        uint32_t lw = w[q + 1u] >> 8, dw = w[q + 2u] >> 8;
        vec3_t c2 = geo3d_direct_pt(w, q + 3u, fx, fy);                         /* P0(n) */
        vec3_t c3 = quad ? geo3d_direct_pt(w, q + 6u, fx, fy) : c2;             /* P1(n) */
        q += quad ? 9u : 6u;
        const int nv = quad ? 4 : 3;
        vec3_t v[4] = { p1, p0, c2, c3 };                    /* the raster's object.v[] */

        uint16_t th[4] = { 0, 0, 0, 0 };
        uint16_t pv[4] = { 0, 0, 0, 0 }, pu[4] = { 0, 0, 0, 0 };
        bool have_th = geo3d_direct_tex(materials, materials_size, &tpa, &tha, attr, nv, th, pv, pu);

        float max_z = geo3d_direct_zsort(v, nv, attr);

        /* check_culling: the rear without the double-sided bit, link type 0,
         * and a polygon wholly behind the eye */
        bool cull = (((attr >> 17) & 1u) == 0 && (lw & 0x00800000u)) || ((attr >> 8) & 3u) == 0 || max_z < 0.0f;
        if (have_th && !cull)
            geo3d_direct_emit(v, nv, th, pv, pu, lw, dw, main_data, main_data_size);

        geo3d_direct_link(attr, &p0, &p1, c2, c3);
    }
    g_geo3d_emit_texlod = GEO3D_TEXLOD_NONE;
    g_geo3d_emit_zs     = GEO3D_ZSORT_NONE;
    g_geo3d_emit_flat   = -1.0f;
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

#ifndef GEO3D_MESH_CACHE_SLOTS
#define GEO3D_MESH_CACHE_SLOTS 4096u   /* power of two */
#endif
/* The heap the cached meshes may hold before the cache starts over; 0: no
 * bound but the slots. A desktop never reaches it; the Dreamcast's 16 MB does. */
#ifndef GEO3D_MESH_CACHE_BYTES
#define GEO3D_MESH_CACHE_BYTES 0u
#endif
/* Bytes of a block of its own that the cached meshes live in, in place of the
 * heap; 0: the heap. A full block starts the cache over. On the Dreamcast the
 * heap is what the ROM pager leaves, and a mesh cache on it ran out: the
 * meshes that did not fit were decoded in full every draw, and the draws took
 * twice as long (Pinboard #394). */
#ifndef GEO3D_MESH_ARENA
#define GEO3D_MESH_ARENA 0u
#endif

typedef struct {
    int32_t  ai, bi, ci, di;
    uint8_t  is_tri, has_c, has_qn, mat_ok;
    uint32_t qa;                 /* attribute word: texparam slot in bits 18..22 */
    int      zsrc[4];            /* the corners the board sorts this polygon by */
    uint32_t zmode;              /* attribute bits 10..11, carried (geo3d_sort_z) */
    uint32_t split_quad, split_cut;
    uint32_t matidx;             /* colorbase, when mat_ok */
    vec3_t   qn;                 /* the record's normal, Z negated */
    float    tx, ty, tw, th, lb, fl;
    float    uvu[4], uvv[4];
    uint16_t fi;                 /* its index in the face loop */
#ifndef GEO3D_DC_SINK            /* the Dreamcast has no object viewer: 24 bytes a face less in its arena */
    uint16_t layer;              /* faces lying under this one in its plane (geo3d_mesh_layers) */
    uint8_t  has_plane;          /* plane is set: the face takes its depth from it */
    float    plane[4];           /* its group's plane in model space, n.p = d, |n| = 1 */
#endif
} geo3d_cface_t;

#ifdef GEO3D_DC_SINK
/* A cached face's triangles from the corner pool at v0 (cut 0: ABC; 1: ABC and
 * BDC; 2: ABD and ADC), sharing one texture and colour; key: the board's flat
 * key, or < 0 for each triangle's nearest corner. */
static void geo3d_dc_face(int v0, const geo3d_cface_t *f, int cut, float r, float g, float b, float pl, int32_t key);
#endif

/* ---- Strip pack ------------------------------------------------------------------
 * A cached mesh made offline (GEO3D_STRIPS; the Dreamcast's STRIPS.PAK, see
 * dreamcast/dc_strips.h): the corners as geo3d_mesh_build leaves them, a
 * smaller face per drawn face with its texture already found, and per face
 * its corners' texture coordinates, already in the PVR texture's units, in
 * strip order (A B C D). The draw still transforms, culls, lights and sorts;
 * it reads no GEO stream, attribute, texture header or UV word. Blob:
 * geo3d_sp_head_t, then sv (padded to 32), the faces, the corners' u, v. */
#define GEO3D_SF_TRI      1u
#define GEO3D_SF_HAS_C    2u
#define GEO3D_SF_HAS_QN   4u
#define GEO3D_SF_MAT_OK   8u
#define GEO3D_SF_TEXTURED 16u
enum { GEO3D_SL_OP, GEO3D_SL_PT, GEO3D_SL_TR };   /* opaque, punch-through (texel holes), translucent */
typedef struct {
    int32_t  model_idx;
    uint32_t mat_ptr, uv_ptr;
    uint16_t n_sv, n_faces;
    float    bc[3], br;
} geo3d_sp_head_t;
typedef struct {
    uint16_t ai, bi, ci, di;
    uint16_t zsrc[4];
    vec3_t   qn;
    uint32_t qa;
    uint32_t split_quad, split_cut;
    uint32_t tex;                /* the PVR texture's key (dp_tex_get), after any 256 window; 0: none */
    float    lb;
    uint16_t matidx, fl;
    uint16_t strip;              /* its first vertex: A B C (D), the strip for cut 1 */
    uint8_t  bits, zmode, list, nv;
    uint8_t  pad[6];
} geo3d_sface_t;
typedef struct {
    float    u, v;               /* in the texture's [0, 1] */
} geo3d_svert_t;
_Static_assert(sizeof(geo3d_sp_head_t) == 32 && sizeof(geo3d_sface_t) == 64 && sizeof(geo3d_svert_t) == 8,
               "strip pack records");
#if defined(GEO3D_STRIPS) && GEO3D_MESH_ARENA
struct geo3d_cmesh;
/* The port's: the mesh m names from its pack, into the arena at *at (at most
 * room bytes). 1: loaded; 0: not in the pack; -1: no room. */
static int geo3d_strips_load(struct geo3d_cmesh *m, uint8_t *at, size_t room);
static void geo3d_dc_sface(int v0, const geo3d_sface_t *f, const geo3d_svert_t *s, int cut,
                           float r, float g, float b, float pl, int32_t key);
#endif

typedef struct geo3d_cmesh {
    bool           used;
    int            model_idx;
    uint32_t       mat_ptr, uv_ptr;
    geo3d_models_t md;           /* what it was built from (never a polygon-RAM mesh) */
    int            n_sv, n_faces;
    vec3_t        *sv;           /* untransformed, Z negated */
    vec3_t         bc;           /* a sphere round sv: centre and radius */
    float          br;
    geo3d_cface_t *faces;        /* only the faces that emit, in decode order */
    const geo3d_sface_t *sfaces; /* or these, from the strip pack (faces NULL) */
    const geo3d_svert_t *strips; /* their vertices */
    bool           ranked;       /* geo3d_mesh_layers has run (the object viewer asks for it) */
    bool           failed;       /* out of memory when built: decoded in full until the cache starts over */
    uint32_t       used_at;      /* g_geo3d_mesh_epoch when last drawn (GEO3D_MESH_ARENA) */
    uint32_t       arena_len;    /* its bytes in the arena, sv first */
} geo3d_cmesh_t;

static int           g_geo3d_mesh_cache = 1;   /* 0: always run the full decoder */
static geo3d_cmesh_t g_geo3d_meshes[GEO3D_MESH_CACHE_SLOTS];
static unsigned      g_geo3d_mesh_count;
static size_t        g_geo3d_mesh_bytes;
static uint64_t      g_geo3d_mesh_hits, g_geo3d_mesh_builds, g_geo3d_mesh_packed;
#if GEO3D_MESH_ARENA
static uint8_t       g_geo3d_arena[GEO3D_MESH_ARENA] __attribute__((aligned(32)));
static size_t        g_geo3d_arena_used;
#endif

static unsigned g_geo3d_mesh_clears, g_geo3d_mesh_evicts;   /* times the cache started over, was thinned */
static uint32_t g_geo3d_mesh_epoch;    /* a frame count, if the port keeps one (geo3d_mesh_cache_evict) */
static inline void geo3d_mesh_cache_clear(void) {
    g_geo3d_mesh_clears++;
#if GEO3D_MESH_ARENA
    g_geo3d_arena_used = 0;
#else
    for (unsigned i = 0; i < GEO3D_MESH_CACHE_SLOTS; i++) {
        free(g_geo3d_meshes[i].sv);
        free(g_geo3d_meshes[i].faces);
    }
#endif
    memset(g_geo3d_meshes, 0, sizeof g_geo3d_meshes);
    g_geo3d_mesh_count = 0;
    g_geo3d_mesh_bytes = 0;
}

static inline uint32_t geo3d_mesh_hash(int model_idx, uint32_t mat_ptr, uint32_t uv_ptr) {
    return ((uint32_t)model_idx * 2654435761u) ^ (mat_ptr * 40503u) ^ (uv_ptr * 2246822519u);
}

static inline geo3d_cmesh_t *geo3d_mesh_free_slot(uint32_t h) {
    for (uint32_t probe = 0; probe < GEO3D_MESH_CACHE_SLOTS; probe++) {
        geo3d_cmesh_t *e = &g_geo3d_meshes[(h + probe) & (GEO3D_MESH_CACHE_SLOTS - 1u)];
        if (!e->used) return e;
    }
    return NULL;
}

/* A full arena (or table) drops the meshes not drawn this frame or the last
 * and slides the rest down, rather than starting over: the frame's own meshes
 * stay built. Needs the port to count frames (g_geo3d_mesh_epoch); false when
 * nothing could go, and the caller starts over. */
static bool geo3d_mesh_cache_evict(void) {
#if GEO3D_MESH_ARENA
    static geo3d_cmesh_t keep[GEO3D_MESH_CACHE_SLOTS];
    unsigned n = 0, total = 0;
    for (unsigned i = 0; i < GEO3D_MESH_CACHE_SLOTS; i++) {
        const geo3d_cmesh_t *e = &g_geo3d_meshes[i];
        if (!e->used) continue;
        total++;
        if (!e->failed && e->sv && e->used_at + 1u >= g_geo3d_mesh_epoch) {
            /* in arena order (insertion, a few hundred at most) */
            unsigned k = n++;
            while (k && keep[k - 1].sv > e->sv) { keep[k] = keep[k - 1]; k--; }
            keep[k] = *e;
        }
    }
    if (n == total) return false;
    memset(g_geo3d_meshes, 0, sizeof g_geo3d_meshes);
    size_t at = 0, bytes = 0;
    for (unsigned k = 0; k < n; k++) {
        geo3d_cmesh_t *m = &keep[k];
        uint8_t *src = (uint8_t *)m->sv;
        ptrdiff_t d = (g_geo3d_arena + at) - src;
        if (d) memmove(g_geo3d_arena + at, src, m->arena_len);
        m->sv = (vec3_t *)(g_geo3d_arena + at);
        if (m->faces)  m->faces  = (geo3d_cface_t *)((uint8_t *)m->faces + d);
        if (m->sfaces) m->sfaces = (const geo3d_sface_t *)((const uint8_t *)m->sfaces + d);
        if (m->strips) m->strips = (const geo3d_svert_t *)((const uint8_t *)m->strips + d);
        at += m->arena_len;
        bytes += m->sfaces ? m->arena_len : (size_t)m->n_sv * sizeof(vec3_t) + (size_t)m->n_faces * sizeof(geo3d_cface_t);
        *geo3d_mesh_free_slot(geo3d_mesh_hash(m->model_idx, m->mat_ptr, m->uv_ptr)) = *m;
    }
    g_geo3d_arena_used = at;
    g_geo3d_mesh_count = n;
    g_geo3d_mesh_bytes = bytes;
    g_geo3d_mesh_evicts++;
    return true;
#else
    return false;
#endif
}

/* ---- Faces lying on faces ---------------------------------------------------
 * Which of two faces in one plane the board shows, ported from the explorer's
 * js/layers.js (vendor/noclip), whose header reasons it out in full. In short:
 * the board gives a polygon one depth (geo3d_sort_z) and fills near buckets
 * first, so of two faces in a plane one wins the whole polygon; a depth buffer
 * finds the same depth on both and hands each pixel to rounding. The recede
 * above settles that only while the backing face is shallow and seen at an
 * angle. It is not always:
 *
 * *Symptoms that surfaced this in STF (issue #75):* model 580's sand is radial
 * triangles up to ~160 units long, deeper than the recede bound from a low
 * camera, so they kept their own depth and tied with the pyramid shadows laid
 * on them, which broke into shards. Model 188's JACKPOT panels stand 0.02 in
 * front of a slot-machine face seen nearly square on, where receding to the far
 * corner moves nothing and 0.02 is under one step of the depth buffer at fight
 * distance: the cabinet's orange showed through.
 *
 * Faces that overlap, face the same way to within a couple of degrees and stand
 * no more than half a unit apart are put in the order the board's sort gives
 * them from a spread of view directions. Each face gets a layer (0 for a face
 * nothing lies under, else one more than the highest face under it) and the
 * faces that were ordered at all share their group's plane, the largest face's.
 * Only the model's own faces are ranked, in model space, once per cached mesh.
 * A layered face does not recede (geo3d_zs_corner).
 *
 * A game frame no longer uses any of this: it draws with the board's own key
 * (geo3d_flat_depth), which settles every such pair as the board does. The
 * layers remain for the object viewer, whose free camera has no board sort to
 * copy. There a face sorted by its farthest corner keeps the recede and takes no
 * layer or plane, and pairs held apart by more than the tie with the same
 * corner are left to the depth buffer (below). */
#define GEO3D_LAYER_GAP    0.5    /* how far apart two faces may stand and be ordered */
#define GEO3D_LAYER_TIE    0.02   /* how far apart counts as one plane */
#define GEO3D_LAYER_COSINE 0.999  /* least cosine between their normals */
#define GEO3D_LAYER_POLY   16     /* a clipped polygon's vertices: 4 + 4 at most */

typedef struct {
    double pts[4][3];
    int    npts, face, cut, zmode;
    double n[3], area, lo[3], hi[3];
    double hull[3][GEO3D_LAYER_POLY][2];
    int    nhull[3];              /* -1 until worked out for that axis */
} geo3d_lface_t;

static const int g_geo3d_layer_other[3][2] = { {1, 2}, {0, 2}, {0, 1} };

#ifndef GEO3D_DC_SINK   /* the ranking below: the object viewer's, not the Dreamcast's */
/* Andrew's monotone chain, counter-clockwise, as layers.js hull(). */
static int geo3d_layer_hull(double (*p)[2], int n, double (*out)[2]) {
    for (int i = 1; i < n; i++) {                /* insertion sort: n <= 4 */
        double a = p[i][0], b = p[i][1];
        int j = i - 1;
        while (j >= 0 && (p[j][0] > a || (p[j][0] == a && p[j][1] > b))) { p[j + 1][0] = p[j][0]; p[j + 1][1] = p[j][1]; j--; }
        p[j + 1][0] = a; p[j + 1][1] = b;
    }
    if (n < 3) { for (int i = 0; i < n; i++) { out[i][0] = p[i][0]; out[i][1] = p[i][1]; } return n; }
    #define GEO3D_CROSS(o, a, b) (((a)[0] - (o)[0]) * ((b)[1] - (o)[1]) - ((a)[1] - (o)[1]) * ((b)[0] - (o)[0]))
    double lower[8][2], upper[8][2];
    int nl = 0, nu = 0;
    for (int i = 0; i < n; i++) {
        while (nl >= 2 && GEO3D_CROSS(lower[nl - 2], lower[nl - 1], p[i]) <= 1e-12) nl--;
        lower[nl][0] = p[i][0]; lower[nl][1] = p[i][1]; nl++;
    }
    for (int i = n - 1; i >= 0; i--) {
        while (nu >= 2 && GEO3D_CROSS(upper[nu - 2], upper[nu - 1], p[i]) <= 1e-12) nu--;
        upper[nu][0] = p[i][0]; upper[nu][1] = p[i][1]; nu++;
    }
    #undef GEO3D_CROSS
    int k = 0;
    for (int i = 0; i < nl - 1; i++) { out[k][0] = lower[i][0]; out[k][1] = lower[i][1]; k++; }
    for (int i = 0; i < nu - 1; i++) { out[k][0] = upper[i][0]; out[k][1] = upper[i][1]; k++; }
    return k;
}

static double geo3d_layer_poly_area(double (*p)[2], int n) {
    double a = 0.0;
    for (int i = 0; i < n; i++) {
        const double *u = p[i], *v = p[(i + 1) % n];
        a += u[0] * v[1] - v[0] * u[1];
    }
    return fabs(a) / 2.0;
}

/* The intersection of two convex counter-clockwise polygons (Sutherland-Hodgman,
 * clipping the first by each edge of the second); 0 when under three points. */
static int geo3d_layer_intersect(double (*s)[2], int ns, double (*c)[2], int nc, double (*out)[2]) {
    double buf[2][GEO3D_LAYER_POLY][2];
    int n = ns, cur = 0;
    for (int i = 0; i < ns; i++) { buf[0][i][0] = s[i][0]; buf[0][i][1] = s[i][1]; }
    for (int i = 0; i < nc && n; i++) {
        const double *a = c[i], *b = c[(i + 1) % nc];
        double (*in)[2] = buf[cur], (*o)[2] = buf[cur ^ 1];
        int m = 0;
        for (int j = 0; j < n; j++) {
            const double *p = in[j], *q = in[(j + 1) % n];
            double sp = (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]);
            double sq = (b[0] - a[0]) * (q[1] - a[1]) - (b[1] - a[1]) * (q[0] - a[0]);
            if (sp >= 0.0 && m < GEO3D_LAYER_POLY) { o[m][0] = p[0]; o[m][1] = p[1]; m++; }
            if ((sp >= 0.0) != (sq >= 0.0) && m < GEO3D_LAYER_POLY) {
                double t = sp / (sp - sq);
                o[m][0] = p[0] + t * (q[0] - p[0]); o[m][1] = p[1] + t * (q[1] - p[1]); m++;
            }
        }
        n = m; cur ^= 1;
    }
    if (n < 3) return 0;
    for (int i = 0; i < n; i++) { out[i][0] = buf[cur][i][0]; out[i][1] = buf[cur][i][1]; }
    return n;
}

static int geo3d_layer_flat_hull(geo3d_lface_t *f, int ax) {
    if (f->nhull[ax] < 0) {
        const int p = g_geo3d_layer_other[ax][0], q = g_geo3d_layer_other[ax][1];
        double flat[4][2];
        for (int i = 0; i < f->npts; i++) { flat[i][0] = f->pts[i][p]; flat[i][1] = f->pts[i][q]; }
        f->nhull[ax] = geo3d_layer_hull(flat, f->npts, f->hull[ax]);
    }
    return f->nhull[ax];
}

/* The point of a face's plane over (u, v) in the plane of the other two axes. */
static void geo3d_layer_lift(const geo3d_lface_t *f, int ax, const double *uv, double *out) {
    const int p = g_geo3d_layer_other[ax][0], q = g_geo3d_layer_other[ax][1];
    const double *o = f->pts[0];
    out[p] = uv[0]; out[q] = uv[1];
    out[ax] = o[ax] - (f->n[p] * (uv[0] - o[p]) + f->n[q] * (uv[1] - o[q])) / f->n[ax];
}

/* Directions to look from, (u, v, w) with w the side a face is drawn from: a
 * golden-angle spiral over the cap within 80 degrees of it. */
static double g_geo3d_layer_views[64][3];
static int    g_geo3d_layer_views_ready;

/* The share of the views from which the board draws g over f: each is keyed by
 * its near or far corner along the view, the nearer key fills first, and a key
 * shared to within a bucket goes to g, the later polygon. */
static double geo3d_layer_later_share(const geo3d_lface_t *f, const geo3d_lface_t *g) {
    if (!g_geo3d_layer_views_ready) {
        const double cap = 1.0 - cos(80.0 * 3.14159265358979323846 / 180.0);
        for (int k = 0; k < 64; k++) {
            double w = 1.0 - ((k + 0.5) / 64.0) * cap;
            double r = sqrt(1.0 - w * w), a = k * 2.399963;
            g_geo3d_layer_views[k][0] = r * cos(a);
            g_geo3d_layer_views[k][1] = r * sin(a);
            g_geo3d_layer_views[k][2] = w;
        }
        g_geo3d_layer_views_ready = 1;
    }
    const double w[3] = { -f->n[0], -f->n[1], -f->n[2] };
    const double t[3] = { fabs(w[0]) < 0.9 ? 1.0 : 0.0, fabs(w[0]) < 0.9 ? 0.0 : 1.0, 0.0 };
    double u[3] = { w[1] * t[2] - w[2] * t[1], w[2] * t[0] - w[0] * t[2], w[0] * t[1] - w[1] * t[0] };
    double ul = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    u[0] /= ul; u[1] /= ul; u[2] /= ul;
    const double v[3] = { w[1] * u[2] - w[2] * u[1], w[2] * u[0] - w[0] * u[2], w[0] * u[1] - w[1] * u[0] };
    int wins = 0;
    for (int k = 0; k < 64; k++) {
        const double *V = g_geo3d_layer_views[k];
        double d[3];
        for (int a = 0; a < 3; a++) d[a] = V[0] * u[a] + V[1] * v[a] + V[2] * w[a];
        double key[2];
        for (int h = 0; h < 2; h++) {
            const geo3d_lface_t *F = h ? g : f;
            double near_ = 1e300, far_ = -1e300;
            for (int i = 0; i < F->npts; i++) {
                double z = -(F->pts[i][0] * d[0] + F->pts[i][1] * d[1] + F->pts[i][2] * d[2]);
                if (z < near_) near_ = z;
                if (z > far_)  far_  = z;
            }
            key[h] = F->zmode == 1 ? near_ : far_;
        }
        if (key[1] <= key[0] + 1e-3) wins++;
    }
    return wins / 64.0;
}

static const geo3d_lface_t *g_geo3d_layer_sorting;   /* qsort has no context argument */
static int geo3d_layer_by_lo(const void *a, const void *b) {
    const geo3d_lface_t *F = g_geo3d_layer_sorting;
    double x = F[*(const int *)a].lo[0], y = F[*(const int *)b].lo[0];
    return x < y ? -1 : x > y ? 1 : *(const int *)a - *(const int *)b;
}

/* One cached face as the ranking sees it: its corners, the normal of the
 * larger of the triangles the fill draws, turned to agree with the ROM's, its
 * area and its box. False for a line or a face with no area, which is not
 * ranked. */
static bool geo3d_layer_face(const geo3d_cmesh_t *m, int k, geo3d_lface_t *f) {
    const geo3d_cface_t *c = &m->faces[k];
    if (c->is_tri && !c->has_c) return false;   /* a line */
    memset(f, 0, sizeof *f);
    const int corner[4] = { c->ai, c->bi, c->ci, c->di };
    f->npts = c->is_tri ? 3 : 4;
    for (int i = 0; i < f->npts; i++) {
        vec3_t p = m->sv[corner[i]];
        f->pts[i][0] = p.x; f->pts[i][1] = p.y; f->pts[i][2] = p.z;
    }
    /* The triangles the fill draws: ABC, or ABD + ADC. */
    const int tri[2][3] = { {0, 1, c->is_tri ? 2 : 3}, {0, 3, 2} };
    double best = 0.0;
    for (int t = 0; t < (c->is_tri ? 1 : 2); t++) {
        const double *a = f->pts[tri[t][0]], *b = f->pts[tri[t][1]], *e = f->pts[tri[t][2]];
        double e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        double e2[3] = { e[0] - a[0], e[1] - a[1], e[2] - a[2] };
        double x[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        double len = sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
        f->area += len / 2.0;
        if (len > best) { best = len; for (int a2 = 0; a2 < 3; a2++) f->n[a2] = x[a2] / len; }
    }
    if (best <= 0.0 || f->area <= 1e-4) return false;
    /* Turned to agree with the ROM's normal, so "further along the normal"
     * is "further behind" for either face of a pair, and two faces back to
     * back in one plane are never compared. */
    if (c->qn.x * f->n[0] + c->qn.y * f->n[1] + c->qn.z * f->n[2] < 0.0)
        for (int a2 = 0; a2 < 3; a2++) f->n[a2] = -f->n[a2];
    for (int a2 = 0; a2 < 3; a2++) {
        f->lo[a2] = f->hi[a2] = f->pts[0][a2];
        for (int i = 1; i < f->npts; i++) {
            if (f->pts[i][a2] < f->lo[a2]) f->lo[a2] = f->pts[i][a2];
            if (f->pts[i][a2] > f->hi[a2]) f->hi[a2] = f->pts[i][a2];
        }
    }
    f->face  = k;
    f->cut   = ((int)(c->fl + 0.5f) & (int)GEO3D_FACE_TRANSPARENT) != 0;
    f->zmode = c->zmode ? (int)c->zmode : 1;   /* the explorer's walk starts at 1 */
    f->nhull[0] = f->nhull[1] = f->nhull[2] = -1;
    return true;
}

/* Whether two ranked faces overlap face to face: near-parallel, their boxes
 * within the gap, and their outlines on the axis plane (ax) sharing more than
 * a sliver. The shared outline goes to common. */
static bool geo3d_layer_pair_overlap(geo3d_lface_t *f, geo3d_lface_t *g, int *ax_out,
                                     double common[GEO3D_LAYER_POLY][2], int *nc_out) {
    const double gap = GEO3D_LAYER_GAP;
    if (f->n[0] * g->n[0] + f->n[1] * g->n[1] + f->n[2] * g->n[2] < GEO3D_LAYER_COSINE) return false;
    if (f->lo[1] > g->hi[1] + gap || g->lo[1] > f->hi[1] + gap ||
        f->lo[2] > g->hi[2] + gap || g->lo[2] > f->hi[2] + gap) return false;
    const int ax = fabs(f->n[0]) >= fabs(f->n[1]) && fabs(f->n[0]) >= fabs(f->n[2]) ? 0
                 : fabs(f->n[1]) >= fabs(f->n[2]) ? 1 : 2;
    int nfh = geo3d_layer_flat_hull(f, ax), ngh = geo3d_layer_flat_hull(g, ax);
    int nc = geo3d_layer_intersect(f->hull[ax], nfh, g->hull[ax], ngh, common);
    double lim = 0.01 * (f->area < g->area ? f->area : g->area);
    *ax_out = ax;
    *nc_out = nc;
    return nc && geo3d_layer_poly_area(common, nc) > (lim > 1e-3 ? lim : 1e-3);
}

/* How far g stands behind f across the overlap, on average. False when some
 * corner of it is more than the gap away. */
static bool geo3d_layer_pair_behind(const geo3d_lface_t *f, const geo3d_lface_t *g, int ax,
                                    double common[GEO3D_LAYER_POLY][2], int nc, double *behind) {
    double most = 0.0, sum = 0.0;
    for (int k = 0; k < nc; k++) {
        double pf[3], pg[3];
        geo3d_layer_lift(f, ax, common[k], pf);
        geo3d_layer_lift(g, ax, common[k], pg);
        double s = f->n[0] * (pg[0] - pf[0]) + f->n[1] * (pg[1] - pf[1]) + f->n[2] * (pg[2] - pf[2]);
        if (fabs(s) > most) most = fabs(s);
        sum += s;
    }
    if (most > GEO3D_LAYER_GAP) return false;
    *behind = sum / nc;
    return true;
}

/* Of a pair held within the tie, or asking for different corners: which is on
 * top, i or j. */
static int geo3d_layer_pair_pick(const geo3d_lface_t *f, const geo3d_lface_t *g, int i, int j,
                                 double behind) {
    const double tie = GEO3D_LAYER_TIE;
    /* g over a smaller solid f in one plane is a window: a pane of light
     * and over it the frame with holes cut for the glass. */
    const bool window = fabs(behind) <= tie && !f->cut && g->cut && f->area < g->area * (1.0 - 1e-3);
    const double share = geo3d_layer_later_share(f, g);
    if (window || share >= 0.75) return j;
    if (share <= 0.25)           return i;
    if (behind > tie)            return i;
    if (behind < -tie)           return j;
    if (fabs(f->area - g->area) > 1e-3 * (f->area > g->area ? f->area : g->area))
                                 return f->area < g->area ? i : j;
    return j;
}

/* Which of two ranked faces lies on top, i or j (j the later polygon), or -1
 * when the pair is not ordered: turned apart, not overlapping, or held apart
 * by more than the tie while asking for the same corner. */
static int geo3d_layer_pair_top(geo3d_lface_t *L, int i, int j) {
    geo3d_lface_t *f = &L[i], *g = &L[j];
    double common[GEO3D_LAYER_POLY][2];
    int ax = 0, nc = 0;
    if (!geo3d_layer_pair_overlap(f, g, &ax, common, &nc)) return -1;
    double behind;
    if (!geo3d_layer_pair_behind(f, g, ax, common, nc, &behind)) return -1;
    /* The sort has the last word only where the two are in one plane to
     * within the tie, or where one asks for a different corner. */
    /* Held apart by more than the tie and asking for the same corner, the
     * two are what they look like: the nearer is in front for the depth
     * buffer as for the board, so they are left to it and join no group.
     * The explorer orders them too, and then puts both on one plane; on a
     * model a few tenths across (a fighter's glove, 1813/1818) that moved
     * faces 0.15 apart onto each other, away from MAME. */
    if (!(fabs(behind) <= GEO3D_LAYER_TIE || f->zmode != g->zmode)) return -1;
    return geo3d_layer_pair_pick(f, g, i, j, behind);
}

/* The orderings found, as edges from the face underneath to the face on top. */
typedef struct {
    int *bottom, *top;
    int  n, cap;
} geo3d_layer_edges_t;

static bool geo3d_layer_edge_add(geo3d_layer_edges_t *E, int bottom, int top) {
    if (E->n == E->cap) {
        int nc2 = E->cap ? E->cap * 2 : 64;
        int *nb = realloc(E->bottom, (size_t)nc2 * sizeof *nb);
        if (!nb) return false;
        E->bottom = nb;
        int *nt = realloc(E->top, (size_t)nc2 * sizeof *nt);
        if (!nt) return false;
        E->top = nt;
        E->cap = nc2;
    }
    E->bottom[E->n] = bottom;
    E->top[E->n]    = top;
    E->n++;
    return true;
}

/* Order every candidate pair: sweep along x over boxes grown by the gap.
 * Marks each face that was ordered at all. False when memory runs out. */
static bool geo3d_layer_order(geo3d_lface_t *L, int *ord, int n, uint8_t *ordered,
                              geo3d_layer_edges_t *E) {
    g_geo3d_layer_sorting = L;
    qsort(ord, (size_t)n, sizeof *ord, geo3d_layer_by_lo);
    for (int a = 0; a < n; a++) {
        const geo3d_lface_t *A = &L[ord[a]];
        for (int b = a + 1; b < n && L[ord[b]].lo[0] <= A->hi[0] + GEO3D_LAYER_GAP; b++) {
            const int i = ord[a] < ord[b] ? ord[a] : ord[b], j = ord[a] < ord[b] ? ord[b] : ord[a];
            const int top = geo3d_layer_pair_top(L, i, j);
            if (top < 0) continue;
            if (!geo3d_layer_edge_add(E, top == i ? j : i, top)) return false;
            ordered[i] = ordered[j] = 1;
        }
    }
    return true;
}

/* Longest path from the faces nothing lies under. A cycle is broken where it
 * is met: whatever is still waiting keeps the layer it has reached. below,
 * start, adj and queue are scratch (n, n + 1, E->n and n ints). */
static void geo3d_layer_depths(int n, const geo3d_layer_edges_t *E, int *layer,
                               int *below, int *start, int *adj, int *queue) {
    for (int e = 0; e < E->n; e++) { start[E->bottom[e] + 1]++; below[E->top[e]]++; }
    for (int i = 0; i < n; i++) start[i + 1] += start[i];
    int *fill = queue;                     /* borrowed as a cursor per face */
    for (int i = 0; i < n; i++) fill[i] = start[i];
    for (int e = 0; e < E->n; e++) adj[fill[E->bottom[e]]++] = E->top[e];
    int qn = 0;
    for (int i = 0; i < n; i++) if (!below[i]) queue[qn++] = i;
    for (int h = 0; h < qn; h++) {
        const int u = queue[h];
        for (int e = start[u]; e < start[u + 1]; e++) {
            const int v = adj[e];
            if (layer[u] + 1 > layer[v]) layer[v] = layer[u] + 1;
            if (--below[v] == 0) queue[qn++] = v;
        }
    }
}

/* The group a face is in (union-find with path halving). */
static int geo3d_layer_root(int *root, int r) {
    while (root[r] != r) r = root[r] = root[root[r]];
    return r;
}

/* The groups: faces joined by any ordering, each taking the plane of its
 * largest face that takes a plane at all (largest[root], -1 for none). A face
 * sorted by its farthest corner keeps its own depth (the cached draw), so its
 * plane would only move the faces laid on it: the Tails lab's monitor pictures
 * (model 3473, faces 682/683) stand 0.17 in front of the wall (face 6, mode 2),
 * and on the wall's plane they went behind their own screen (680, mode 2, 0.03
 * behind them), which then covered them (issue #85). */
static void geo3d_layer_groups(const geo3d_cmesh_t *m, const geo3d_lface_t *L, int n,
                               const geo3d_layer_edges_t *E, int *root, int *largest) {
    for (int i = 0; i < n; i++) { root[i] = i; largest[i] = -1; }
    for (int e = 0; e < E->n; e++) {
        int ra = geo3d_layer_root(root, E->bottom[e]), rb = geo3d_layer_root(root, E->top[e]);
        root[ra] = rb;
    }
    for (int i = 0; i < n; i++) {
        int r = geo3d_layer_root(root, i);
        const int zm = m->faces[L[i].face].zmode;
        if (zm == 2 || zm == 3) continue;
        if (largest[r] < 0 || L[i].area > L[largest[r]].area) largest[r] = i;
    }
}

/* An ordered face takes its group's plane, unless it strayed off that plane
 * through a tilt: then it keeps its depth. */
static void geo3d_layer_plane(geo3d_cface_t *c, const geo3d_lface_t *f, const geo3d_lface_t *ref) {
    const double *nn = ref->n;
    const double d = nn[0] * ref->pts[0][0] + nn[1] * ref->pts[0][1] + nn[2] * ref->pts[0][2];
    for (int k = 0; k < f->npts; k++)
        if (fabs(nn[0] * f->pts[k][0] + nn[1] * f->pts[k][1] + nn[2] * f->pts[k][2] - d) > GEO3D_LAYER_GAP)
            return;
    c->has_plane = 1;
    c->plane[0] = (float)nn[0]; c->plane[1] = (float)nn[1];
    c->plane[2] = (float)nn[2]; c->plane[3] = (float)d;
}

/* Rank a cached mesh's faces (sets layer / has_plane / plane on each). Faces are
 * left unlayered if memory runs out.
 *
 * Render thread only, like the mesh cache it serves: qsort's context rides in
 * g_geo3d_layer_sorting and the view directions are filled in on first use,
 * neither of them locked. A caller on another thread (a dump tool, a worker)
 * has to rank under a lock or with its own copies of both. */
static void geo3d_mesh_layers(geo3d_cmesh_t *m) {
    const int nf = m->n_faces;
    geo3d_lface_t *L   = malloc((size_t)(nf ? nf : 1) * sizeof *L);
    int           *ord = malloc((size_t)(nf ? nf : 1) * sizeof *ord);
    uint8_t       *ordered = NULL;
    geo3d_layer_edges_t E = { 0 };
    int           *scratch = NULL;
    if (!L || !ord) goto done;

    int n = 0;
    for (int k = 0; k < nf; k++)
        if (geo3d_layer_face(m, k, &L[n])) { ord[n] = n; n++; }

    ordered = calloc((size_t)(n ? n : 1), 1);
    if (!ordered || !geo3d_layer_order(L, ord, n, ordered, &E) || !E.n) goto done;

    /* below, layer, queue, root and largest (n each), start (n + 1), adj (E.n) */
    scratch = calloc((size_t)n * 6 + 1 + (size_t)E.n, sizeof *scratch);
    if (!scratch) goto done;
    int *below = scratch, *layer = below + n, *queue = layer + n, *root = queue + n;
    int *largest = root + n, *start = largest + n, *adj = start + n + 1;

    geo3d_layer_depths(n, &E, layer, below, start, adj, queue);
    geo3d_layer_groups(m, L, n, &E, root, largest);
    for (int i = 0; i < n; i++) {
        geo3d_cface_t *c = &m->faces[L[i].face];
        c->layer = (uint16_t)(layer[i] > 0xFFFF ? 0xFFFF : layer[i]);
        if (!ordered[i]) continue;
        const int r = geo3d_layer_root(root, i);
        if (largest[r] < 0) continue;   /* none of the group takes a plane */
        geo3d_layer_plane(c, &L[i], &L[largest[r]]);
    }
done:
    free(scratch);
    free(L); free(ord); free(ordered); free(E.bottom); free(E.top);
}
#endif

/* The z-sort mode in force for the object being drawn (the display list's
 * command 08, captured_model_t.zadjust); set by the draw loop like g_geo3d_mode. */
static uint32_t g_geo3d_zadjust = 0;

/* The board's sort key for a polygon depth (board z, positive into the screen):
 * MAME's float_to_zval, z_adjust and all. The exponent is taken relative to
 * z_adjust's and the mantissa rounded to 12 bits, so depths closer than that
 * share a bucket; below the range the key denormalises and then clamps to 0,
 * above it clamps to 0xFFFF, and there the board ties where depth would not. */
static inline uint32_t geo3d_board_zkey(float z) {
    uint32_t u;
    memcpy(&u, &z, 4);
    if ((int32_t)u < 0) return 0;
    int32_t  e = (int32_t)((u >> 23) & 0xffu) - (int32_t)((g_geo3d_zadjust >> 23) & 0xffu);
    uint32_t mant = (u & 0x7fffffu) + 0x400u;
    if (mant > 0x7fffffu) { e++; mant = (mant & 0x7fffffu) >> 1; }
    mant >>= 11;
    if (e < -12) return 0;
    if (e < 0)   return (mant | 0x1000u) >> -e;
    if (e < 15)  return ((uint32_t)(e + 1) << 12) | mant;
    return 0xffffu;
}

/* A face of the static mesh as the walk found it: corners, z-sort, material
 * and texture points. */
static inline void geo3d_cface_fill(geo3d_cface_t *f, const geo3d_ia_t *ia, int fi, bool tri_cnt,
                                    int ai, int bi, int ci, int di, bool has_c, bool has_d,
                                    const int zsrc[4], uint32_t zmode, bool mat_ok,
                                    const geo3d_texhdr_t *h, const float uvu[4], const float uvv[4]) {
    memset(f, 0, sizeof *f);
    f->ai = ai; f->bi = bi; f->ci = ci; f->di = di;
    f->fi = (uint16_t)fi;
    f->is_tri = tri_cnt || !has_c || !has_d;
    f->has_c  = has_c;
    f->has_qn = fi < ia->n_qt;
    f->qn     = f->has_qn ? ia->qn[fi] : (vec3_t){0, 0, 0};
    f->qa     = f->has_qn ? ia->qa[fi] : 0;
    f->zmode  = zmode;
    for (int z = 0; z < 4; z++) f->zsrc[z] = zsrc[z];
    f->mat_ok = mat_ok;
    f->matidx = h->matidx;
    if (!f->is_tri) {
        const uint32_t *svk = ia->svk;
        f->split_quad = svk[ai] ^ svk[bi] ^ svk[ci] ^ svk[di];
        f->split_cut  = svk[ai] ^ svk[di];
    }
    geo3d_texhdr_tile(h, &f->tx, &f->ty, &f->tw, &f->th);
    f->lb = (float)h->lumabase;
    f->fl = (float)h->fflags;
    memcpy(f->uvu, uvu, 4 * sizeof *uvu);
    memcpy(f->uvv, uvv, 4 * sizeof *uvv);
}

/* The mesh's corners into m->sv (already allocated, as m->faces is), and its
 * bounding sphere. */
static inline bool geo3d_mesh_keep(geo3d_cmesh_t *m, const vec3_t *sv, int n_sv,
                                   geo3d_cface_t *faces, int n_faces) {
    memcpy(m->sv, sv, (size_t)n_sv * sizeof(vec3_t));
    m->faces = faces;
    m->n_sv = n_sv;
    m->n_faces = n_faces;
    vec3_t lo = n_sv ? sv[0] : (vec3_t){ 0 }, hi = lo;
    for (int i = 1; i < n_sv; i++) {
        lo.x = fminf(lo.x, sv[i].x); lo.y = fminf(lo.y, sv[i].y); lo.z = fminf(lo.z, sv[i].z);
        hi.x = fmaxf(hi.x, sv[i].x); hi.y = fmaxf(hi.y, sv[i].y); hi.z = fmaxf(hi.z, sv[i].z);
    }
    m->bc = (vec3_t){ 0.5f * (lo.x + hi.x), 0.5f * (lo.y + hi.y), 0.5f * (lo.z + hi.z) };
    m->br = 0.0f;
    for (int i = 0; i < n_sv; i++) {
        float dx = sv[i].x - m->bc.x, dy = sv[i].y - m->bc.y, dz = sv[i].z - m->bc.z;
        m->br = fmaxf(m->br, sqrtf(dx * dx + dy * dy + dz * dz));
    }
    m->br = m->br * 1.001f + 1.0e-6f;
    return true;
}

/* The static half of geo3d_decode_model for one (model, material, UV): same
 * walk, same face loop, no matrix. Returns false if out of memory. */
static inline bool geo3d_mesh_build(geo3d_cmesh_t *m, uint32_t mesh_offset,
                                    bool have_mat, bool have_uv) {
    static geo3d_ia_t ia;
    const uint8_t *materials = m->md.materials;
    const size_t   materials_size = m->md.materials_size;

    geo3d_ia_walk(&ia, m->md.polygons, m->md.polygons_size, mesh_offset, NULL);
    const int n_sv = ia.n_sv;

    /* The faces go straight to the heap, a block for one per index quad, cut
     * to the ones that emit below: no 4096-face scratch in BSS (600 KB, which
     * the Dreamcast's heap needs more). */
    const int max_faces = ia.n_idx > 8 ? (ia.n_idx - 8 + 3) / 4 : 0;
#if GEO3D_MESH_ARENA
    /* the faces last, so that cutting them to the ones that emit gives the rest back */
    const size_t sv_bytes = ((size_t)n_sv * sizeof(vec3_t) + 31u) & ~(size_t)31u;
    if (g_geo3d_arena_used + sv_bytes + (size_t)max_faces * sizeof(geo3d_cface_t) > GEO3D_MESH_ARENA) return false;
    m->sv = (vec3_t *)(g_geo3d_arena + g_geo3d_arena_used);
    geo3d_cface_t *faces = (geo3d_cface_t *)(g_geo3d_arena + g_geo3d_arena_used + sv_bytes);
#else
    m->sv = malloc((size_t)(n_sv ? n_sv : 1) * sizeof(vec3_t));
    geo3d_cface_t *faces = malloc((size_t)(max_faces ? max_faces : 1) * sizeof(geo3d_cface_t));
    if (!m->sv || !faces) { free(m->sv); free(faces); m->sv = NULL; return false; }
#endif

    uint32_t mat_word = m->mat_ptr, uv_word = m->uv_ptr, mat_rec = mat_word;
    int n_faces = 0;
    int zsrc[4] = {0,0,0,0}; uint32_t zmode = 0u; bool zset = false;
    for (int i = 0; i < ia.n_idx - 8; i += 4) {
        int fi = i / 4;
        int ai = ia.idx[i], bi = ia.idx[i + 1], ci = ia.idx[i + 2], di = ia.idx[i + 3];
        const uint32_t at = geo3d_ia_attr(&ia, fi);
        bool tri_cnt = (fi < ia.n_qt && ia.qt[fi] == 2);
        int  nv = tri_cnt ? 3 : 4;

        geo3d_texhdr_t h = GEO3D_TEXHDR_NONE;
        bool mat_ok = false;
        if (have_mat) {
            uint32_t hw = mat_rec;
            mat_rec += geo3d_tho_step(at);
            uint16_t th[4] = { 0, 0, 0, 0 };
            if (geo3d_texhdr_words(materials, materials_size, hw, th)) {
                h = geo3d_texhdr_decode(th);
                mat_ok = true;
            }
        }
        float uvu[4] = {0,0,0,0}, uvv[4] = {0,0,0,0};
        if (have_uv) geo3d_uv_read(materials, materials_size, uv_word, tri_cnt, h.textured, uvu, uvv);
        uv_word += (uint32_t)nv * 2u;

        if (ai < 0 || ai >= n_sv) continue;
        if (bi < 0 || bi >= n_sv) continue;
        bool has_c = (ci >= 0 && ci < n_sv), has_d = (di >= 0 && di < n_sv);
        /* Before the skip below, not after: the z-sort state is carried across
         * every face of the walk, including the ones nothing is drawn for. */
        geo3d_zsort_step(at, tri_cnt || !has_c || !has_d, has_c,
                         ai, bi, ci, di, zsrc, &zmode, &zset);
        if (h.untex_trans) continue;   /* the board draws nothing for it */

        geo3d_cface_fill(&faces[n_faces++], &ia, fi, tri_cnt, ai, bi, ci, di, has_c, has_d,
                         zsrc, zmode, mat_ok, &h, uvu, uvv);
    }
#if GEO3D_MESH_ARENA
    m->arena_len = (uint32_t)(sv_bytes + (((size_t)n_faces * sizeof(geo3d_cface_t) + 31u) & ~(size_t)31u));
    g_geo3d_arena_used += m->arena_len;
#else
    geo3d_cface_t *fit = n_faces < max_faces ? realloc(faces, (size_t)(n_faces ? n_faces : 1) * sizeof(geo3d_cface_t)) : NULL;
    if (fit) faces = fit;
#endif
    return geo3d_mesh_keep(m, ia.sv, n_sv, faces, n_faces);
}

/* A model's static mesh from the cache, built on first sight. mesh_offset,
 * mat_ptr and uv_ptr as geo3d_decode_model works them out. NULL when out of
 * memory. Render thread only: the cache is not locked. */
static inline bool geo3d_models_same(const geo3d_models_t *a, const geo3d_models_t *b) {
    return a->polygons == b->polygons && a->materials == b->materials && a->main_data == b->main_data &&
           a->polygons_size == b->polygons_size && a->materials_size == b->materials_size &&
           a->table_off == b->table_off && a->table_count == b->table_count &&
           a->mesh_ptr_subtract == b->mesh_ptr_subtract && a->mesh_ptr_add == b->mesh_ptr_add;
}

static geo3d_cmesh_t *geo3d_mesh_get(const geo3d_models_t *md, int model_idx,
                                     uint32_t mat_ptr, uint32_t uv_ptr, uint32_t mesh_offset) {
    uint32_t h = geo3d_mesh_hash(model_idx, mat_ptr, uv_ptr);
    geo3d_cmesh_t *m = NULL;
    for (uint32_t probe = 0; probe < GEO3D_MESH_CACHE_SLOTS; probe++) {
        geo3d_cmesh_t *e = &g_geo3d_meshes[(h + probe) & (GEO3D_MESH_CACHE_SLOTS - 1u)];
        if (!e->used) { m = e; break; }
        if (e->model_idx == model_idx && e->mat_ptr == mat_ptr && e->uv_ptr == uv_ptr &&
                geo3d_models_same(&e->md, md)) {
            m = e;
            break;
        }
    }
    if (m && m->used) {
        if (m->failed) return NULL;
        g_geo3d_mesh_hits++;
        m->used_at = g_geo3d_mesh_epoch;
        return m;
    }
    if (!m || g_geo3d_mesh_count >= GEO3D_MESH_CACHE_SLOTS * 3u / 4u ||
            (GEO3D_MESH_CACHE_BYTES && g_geo3d_mesh_bytes >= GEO3D_MESH_CACHE_BYTES)) {
        if (geo3d_mesh_cache_evict() && g_geo3d_mesh_count < GEO3D_MESH_CACHE_SLOTS * 3u / 4u) {
            m = geo3d_mesh_free_slot(h);
        } else {
            geo3d_mesh_cache_clear();
            m = &g_geo3d_meshes[h & (GEO3D_MESH_CACHE_SLOTS - 1u)];
        }
    }
    for (int retry = 0;; retry++) {
        *m = (geo3d_cmesh_t){ .model_idx = model_idx, .mat_ptr = mat_ptr, .uv_ptr = uv_ptr, .md = *md };
        m->md.obj_mesh = NULL;
        m->md.obj_mesh_size = 0;
        m->used = true;
        m->used_at = g_geo3d_mesh_epoch;
        g_geo3d_mesh_count++;
#if defined(GEO3D_STRIPS) && GEO3D_MESH_ARENA
        /* made offline, if the pack has it: a copy, not a walk */
        int packed = geo3d_strips_load(m, g_geo3d_arena + g_geo3d_arena_used, GEO3D_MESH_ARENA - g_geo3d_arena_used);
        if (packed > 0) {
            g_geo3d_arena_used += m->arena_len;
            g_geo3d_mesh_bytes += m->arena_len;
            g_geo3d_mesh_packed++;
            return m;
        }
        if (packed == 0 && geo3d_mesh_build(m, mesh_offset, mat_ptr != 0, uv_ptr != 0)) break;
#else
        if (geo3d_mesh_build(m, mesh_offset, mat_ptr != 0, uv_ptr != 0)) break;
#endif
        /* Out of memory (or arena), which the cache's own meshes may be
         * holding: thin it, then start over. A mesh that still does not fit
         * stays a failed entry, not a build every frame: a failed build costs
         * most of a full decode, which the draw runs too. */
        if (retry == 2 || g_geo3d_mesh_count <= 1) { m->failed = true; return NULL; }
        m->used = false;
        g_geo3d_mesh_count--;
        if (retry == 0 && geo3d_mesh_cache_evict()) {
            m = geo3d_mesh_free_slot(h);
        } else {
            retry = 1;
            geo3d_mesh_cache_clear();
            m = &g_geo3d_meshes[h & (GEO3D_MESH_CACHE_SLOTS - 1u)];
        }
    }
    g_geo3d_mesh_bytes += (size_t)m->n_sv * sizeof(vec3_t) + (size_t)m->n_faces * sizeof(geo3d_cface_t);
    g_geo3d_mesh_builds++;
    return m;
}

/* The layers of a model's faces by face-loop index, for geo3d_decode_model
 * (declared before it). False when the cache cannot answer. */
static bool geo3d_mesh_layers_for(const geo3d_models_t *md, int model_idx,
                                  uint32_t mat_ptr, uint32_t uv_ptr, uint32_t mesh_offset,
                                  geo3d_face_layer_t *out, int cap) {
#ifdef GEO3D_DC_SINK
    (void)md; (void)model_idx; (void)mat_ptr; (void)uv_ptr; (void)mesh_offset; (void)out; (void)cap;
    return false;
#else
    geo3d_cmesh_t *m = geo3d_mesh_get(md, model_idx, mat_ptr, uv_ptr, mesh_offset);
    if (!m) return false;
    if (!m->ranked) { geo3d_mesh_layers(m); m->ranked = true; }
    memset(out, 0, (size_t)cap * sizeof *out);
    for (int n = 0; n < m->n_faces; n++) {
        const geo3d_cface_t *f = &m->faces[n];
        if (f->fi >= cap) continue;
        out[f->fi].layer     = f->layer;
        out[f->fi].has_plane = f->has_plane;
        memcpy(out[f->fi].plane, f->plane, sizeof f->plane);
    }
    return true;
#endif
}

/* Whether a draw goes through the mesh cache, and its mesh if so: NONE draws
 * nothing, FULL is the full decoder's (see geo3d_decode_model_cached). */
enum { GEO3D_DRAW_NONE, GEO3D_DRAW_FULL, GEO3D_DRAW_CACHED };
static int geo3d_mesh_for_draw(const geo3d_models_t *md, int model_idx,
                               const float *matrix, geo3d_cmesh_t **out) {
    *out = NULL;
    if (!g_geo3d_mesh_cache || !g_geo3d_board_luma || !matrix || md->obj_mesh || g_geo_flat_color ||
            GEO3D_DUMP_TEX(model_idx))
        return GEO3D_DRAW_FULL;
    if (!md->main_data || !md->polygons) return GEO3D_DRAW_NONE;
    uint32_t mesh_offset, mat_ptr, uv_ptr;
    if (!geo3d_model_streams(md, model_idx, &mesh_offset, &mat_ptr, &uv_ptr)) return GEO3D_DRAW_NONE;
    bool have_mat = mat_ptr != 0, have_uv = uv_ptr != 0;
    /* Streams in texture RAM change under the cache: decode those every time. */
    if ((have_mat && (mat_ptr & 0x800000u)) || (have_uv && (uv_ptr & 0x800000u)))
        return GEO3D_DRAW_FULL;
    *out = geo3d_mesh_get(md, model_idx, mat_ptr, uv_ptr, mesh_offset);
    return *out ? GEO3D_DRAW_CACHED : GEO3D_DRAW_FULL;
}

/* The model's sphere against the window, before any corner is moved: 0 when
 * no face can be dropped (culling off, or wholly inside every plane), the bit of
 * a plane it is wholly outside, or -1 across a side (its corners need codes). */
static inline int geo3d_cached_sphere(const geo3d_cmesh_t *m, const float *matrix, bool lines) {
    if (!g_geo3d_cull_on || lines) return 0;
    const float *mx = matrix;
    float s2 = fmaxf(fmaxf(mx[0] * mx[0] + mx[4] * mx[4] + mx[8] * mx[8],
                           mx[1] * mx[1] + mx[5] * mx[5] + mx[9] * mx[9]),
                     mx[2] * mx[2] + mx[6] * mx[6] + mx[10] * mx[10]);
    vec3_t c = apply_matrix(m->bc, matrix);
    float r = m->br * sqrtf(s2) * 1.001f;
    uint8_t in = 0, all_out = 0;
    for (int k = 0; k < 5; k++) {
        const float *q = g_geo3d_cull_plane[k];
        float d = q[0] * c.x + q[1] * c.y + q[2] * c.z + q[3];
        float e = r * g_geo3d_cull_nlen[k] + 1.0e-3f * (fabsf(q[0] * c.x) + fabsf(q[1] * c.y) + fabsf(q[2] * c.z) + fabsf(q[3]));
        if (d > e) in++;
        else if (d < -e && !all_out) all_out = (uint8_t)(1u << k);
    }
    return in == 5 ? 0 : all_out ? all_out : -1;
}

/* The cached draw's window test, sph from geo3d_cached_sphere. False when no
 * face can be dropped. Otherwise oc holds each corner's codes: wholly outside
 * one plane, every corner has that plane's, and *gone is set. */
static inline bool geo3d_cached_cull_codes(const geo3d_cmesh_t *m, int sph,
                                           const vec3_t *tv, uint8_t *oc, bool *gone) {
    *gone = false;
    if (sph == 0) return false;
    if (sph > 0) { memset(oc, sph, (size_t)m->n_sv); *gone = true; }
#ifdef GEO3D_FTRV
    else {
        const float (*q)[4] = g_geo3d_cull_plane;
        float c[16] __attribute__((aligned(8))) = {
            q[0][0], q[1][0], q[2][0], q[3][0],  q[0][1], q[1][1], q[2][1], q[3][1],
            q[0][2], q[1][2], q[2][2], q[3][2],  q[0][3], q[1][3], q[2][3], q[3][3] };
        geo3d_xmtrx_load(c);
        for (int i = 0; i < m->n_sv; i++) {
            float d[4];
            geo3d_ftrv(tv[i].x, tv[i].y, tv[i].z, 1.0f, d);
            oc[i] = geo3d_cull_code_d(tv[i], d);
        }
    }
#else
    else for (int i = 0; i < m->n_sv; i++) oc[i] = geo3d_cull_code(tv[i]);
#endif
    return true;
}

#ifdef GEO3D_DC_SINK
/* A model wholly out of the window leaves only the flat key's carry behind:
 * that of the last face (in order) with a mode other than "the previous". */
static inline void geo3d_cached_gone_carry(const geo3d_cmesh_t *m, const float *matrix) {
    if (!(g_geo3d_zflat && g_geo3d_flat_list)) return;
    int zsrc[4];
    uint32_t zmode = 0;
    /* the walk takes the packed faces first */
    for (int n = m->faces ? m->n_faces - 1 : -1; n >= 0 && !zmode; n--)
        if ((zmode = m->faces[n].zmode) != 0u)
            for (int k = 0; k < 4; k++) zsrc[k] = m->faces[n].zsrc[k];
#if defined(GEO3D_STRIPS) && GEO3D_MESH_ARENA
    for (int n = m->sfaces ? m->n_faces - 1 : -1; n >= 0 && !zmode; n--)
        if ((zmode = m->sfaces[n].zmode) != 0u)
            for (int k = 0; k < 4; k++) zsrc[k] = m->sfaces[n].zsrc[k];
#endif
    if (!zmode) return;
    vec3_t tv[4];
    const int idx[4] = { 0, 1, 2, 3 };
    if (zmode != 3u) {   /* moved as geo3d_decode_model_cached moves them, to the bit */
#ifdef GEO3D_FTRV
        geo3d_xmtrx_board(matrix);
        for (int k = 0; k < 4; k++) {
            const vec3_t p = m->sv[zsrc[k]];
            float o[4];
            geo3d_ftrv(p.x, p.y, p.z, 1.0f, o);
            tv[k].x = o[0]; tv[k].y = o[1]; tv[k].z = o[2];
        }
#else
        for (int k = 0; k < 4; k++) tv[k] = apply_matrix(m->sv[zsrc[k]], matrix);
#endif
    }
    geo3d_flat_z(tv, idx, zmode);
}

/* The cached draw's walk in the order the Dreamcast needs: the flat key's carry
 * and the diagonals for every face, the colour and light only for one that is
 * drawn, and the key only as a number. gone: every face out, so its corners
 * need no projecting. */
static inline void geo3d_cached_draw_dc(const geo3d_models_t *md, const geo3d_cmesh_t *m,
                                        const vec3_t *tv, const uint8_t *oc, bool cull, bool gone,
                                        const float *matrix, float cr, float cg, float cb) {
    const int dcv = gone ? -1 : geo3d_dc_verts(tv, m->n_sv);
    const bool flat = g_geo3d_zflat && g_geo3d_flat_list;
#if defined(GEO3D_STRIPS) && GEO3D_MESH_ARENA
    /* The same walk over a packed mesh's faces (geo3d_strips_load). */
    for (int n = 0; m->sfaces && n < m->n_faces; n++) {
        const geo3d_sface_t *f = &m->sfaces[n];
        /* every face missed the cache: ask for the one after next (two lines) */
        __builtin_prefetch(f + 2);
        __builtin_prefetch((const char *)(f + 2) + 32);
        const bool is_tri = (f->bits & GEO3D_SF_TRI) != 0;
        const int zsrc[4] = { f->zsrc[0], f->zsrc[1], f->zsrc[2], f->zsrc[3] };
        const float z = flat ? geo3d_flat_z(tv, zsrc, f->zmode) : 0.0f;
        if (is_tri && !(f->bits & GEO3D_SF_HAS_C)) continue;
        bool out = false;
        if (cull)
            out = (oc[f->ai] & oc[f->bi] & oc[f->ci] & (is_tri ? 0xFFu : oc[f->di])) != 0;
        if (out && is_tri) continue;
        geo3d_lit_t lt;
        if (geo3d_board_cull(matrix, f->qn, (f->bits & GEO3D_SF_HAS_QN) ? f->qa : 0u, true,
                             tv[f->ai], tv[f->bi], tv[f->ci], &lt)) continue;
        if (out || dcv < 0) { if (!is_tri) geo3d_split_other_way(f->split_quad, f->split_cut); continue; }
        float fr = cr, fg = cg, fb = cb;
        if (f->bits & GEO3D_SF_MAT_OK) geo3d_palette_color(f->matidx, md->main_data, md->main_data_size, &fr, &fg, &fb);
        const float pl = geo3d_board_luma(&lt);
        const int cut = is_tri ? 0 : geo3d_split_other_way(f->split_quad, f->split_cut) ? 1 : 2;
        geo3d_dc_sface(dcv, f, m->strips + f->strip, cut, fr, fg, fb, pl, flat ? (int32_t)geo3d_board_zkey(z) : -1);
    }
#endif
    for (int n = 0; m->faces && n < m->n_faces; n++) {
        const geo3d_cface_t *f = &m->faces[n];
        const float z = flat ? geo3d_flat_z(tv, f->zsrc, f->zmode) : 0.0f;
        if (f->is_tri && !f->has_c) continue;
        bool out = false;
        if (cull)
            out = (oc[f->ai] & oc[f->bi] & oc[f->ci] & (f->is_tri ? 0xFFu : oc[f->di])) != 0;
        if (out && f->is_tri) continue;
        geo3d_lit_t lt;
        if (geo3d_board_cull(matrix, f->qn, f->has_qn ? f->qa : 0u, true,
                             tv[f->ai], tv[f->bi], tv[f->ci], &lt)) continue;
        if (out || dcv < 0) { if (!f->is_tri) geo3d_split_other_way(f->split_quad, f->split_cut); continue; }
        float fr = cr, fg = cg, fb = cb;
        if (f->mat_ok) geo3d_palette_color(f->matidx, md->main_data, md->main_data_size, &fr, &fg, &fb);
        const float pl = geo3d_board_luma(&lt);
        const int cut = f->is_tri ? 0 : geo3d_split_other_way(f->split_quad, f->split_cut) ? 1 : 2;
        geo3d_dc_face(dcv, f, cut, fr, fg, fb, pl, flat ? (int32_t)geo3d_board_zkey(z) : -1);
    }
}
#endif

/* One face of a cached mesh, drawn at the instance's matrix. */
static inline void geo3d_cached_face(const geo3d_models_t *md, const geo3d_cface_t *f,
                                     const vec3_t *tv, const uint8_t *oc, bool cull, bool lines,
                                     const float *matrix, float cr, float cg, float cb) {
    vec3_t A = tv[f->ai], B = tv[f->bi];
    vec3_t C = f->has_c ? tv[f->ci] : (vec3_t){0, 0, 0};
    vec3_t D = f->is_tri ? (vec3_t){0, 0, 0} : tv[f->di];

    /* The board's key, flat over the face; with zflat off, the half rule
     * alone (the recede), for an A/B. */
    const bool flat = g_geo3d_zflat && g_geo3d_flat_list;
    g_geo3d_emit_flat = flat ? geo3d_flat_depth(tv, f->zsrc, f->zmode) : -1.0f;
    g_geo3d_emit_zs   = flat ? GEO3D_ZSORT_NONE : geo3d_sort_z(tv, f->zsrc, f->zmode);
    bool out = false;
    if (cull && f->has_c)
        out = (oc[f->ai] & oc[f->bi] & oc[f->ci] & (f->is_tri ? 0xFFu : oc[f->di])) != 0;
    /* Out of the window: a triangle has nothing left to do; a quad still
     * records its diagonal below, if the board would have drawn it. */
    if (out && f->is_tri) return;

    /* The board's lighting and culling, as geo3d_decode_model does them on
     * the display-list path (board luma with a matrix: always this branch).
     * Specular, the truncated luma and the texlod belong to the instance, so
     * they are worked out here per draw and never kept in the mesh. */
    geo3d_lit_t lt;
    if (geo3d_board_cull(matrix, f->qn, f->has_qn ? f->qa : 0u, f->has_c, A, B, C, &lt)) return;
    if (out) { geo3d_split_other_way(f->split_quad, f->split_cut); return; }
    float fr = cr, fg = cg, fb = cb;   /* only a face that is drawn needs its colour */
    if (f->mat_ok) geo3d_palette_color(f->matidx, md->main_data, md->main_data_size, &fr, &fg, &fb);
    const float pl = geo3d_board_luma(&lt);
    const geo3d_paint_t paint = {
        .r = fr, .g = fg, .b = fb, .tx = f->tx, .ty = f->ty, .tw = f->tw, .th = f->th,
        .lb = f->lb, .pl = pl, .fl = f->fl, .u = f->uvu, .v = f->uvv,
    };
    geo3d_emit_face(A, B, C, D, f->is_tri, f->has_c, lines, f->split_quad, f->split_cut, &paint);
}

static inline void geo3d_decode_model_cached(const geo3d_models_t *md, int model_idx,
                                             const float *matrix,
                                             float cr, float cg, float cb) {
    geo3d_cmesh_t *m;
    const int how = geo3d_mesh_for_draw(md, model_idx, matrix, &m);
    if (how == GEO3D_DRAW_NONE) return;
    if (how == GEO3D_DRAW_FULL) {
        geo3d_decode_model(md, model_idx, matrix, cr, cg, cb);
        return;
    }

    bool lines = g_geo_wireframe != 0;
    const int sph = geo3d_cached_sphere(m, matrix, lines);
#ifdef GEO3D_DC_SINK
    /* Wholly outside the window: no face is drawn, and of all the walk does
     * only the flat key's carry outlives the model (the diagonals are the
     * model's own). The carry is the last face's that sets one: move its
     * corners alone. */
    if (sph > 0) {
        geo3d_cached_gone_carry(m, matrix);
        geo3d_emit_state_reset();
        return;
    }
#endif

    static vec3_t tv[GEO3D_IA_MAX_VERTS];
#ifdef GEO3D_FTRV
    geo3d_xmtrx_board(matrix);
    for (int i = 0; i < m->n_sv; i++) {
        float o[4];
        __builtin_prefetch(&m->sv[i + 8]);   /* the corners stream in: a line ahead */
        geo3d_ftrv(m->sv[i].x, m->sv[i].y, m->sv[i].z, 1.0f, o);
        tv[i].x = o[0]; tv[i].y = o[1]; tv[i].z = o[2];
    }
#else
    for (int i = 0; i < m->n_sv; i++) tv[i] = apply_matrix(m->sv[i], matrix);
#endif
    geo3d_split_reset();
    static uint8_t oc[GEO3D_IA_MAX_VERTS];
    bool gone;
    const bool cull = geo3d_cached_cull_codes(m, sph, tv, oc, &gone);
#ifdef GEO3D_DC_SINK
    geo3d_cached_draw_dc(md, m, tv, oc, cull, gone, matrix, cr, cg, cb);
#else
    for (int n = 0; n < m->n_faces; n++)
        geo3d_cached_face(md, &m->faces[n], tv, oc, cull, lines, matrix, cr, cg, cb);
#endif
    geo3d_emit_state_reset();
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

/* A tile the extractor writes: its sheet and rectangle. */
typedef struct { uint32_t s, x, y, w, h; } geo3d_extract_tile_t;

/* One face's material record: its manifest line, and its tile added to the
 * list when it is textured and new. */
static void geo3d_extract_face(FILE *mf, uint32_t f, const uint8_t *rp,
                               const uint8_t *main_data, size_t main_data_size,
                               geo3d_extract_tile_t *tiles, int *nt) {
    uint16_t th0 = (uint16_t)geo3d_rom16(rp + 0);
    uint16_t th2 = (uint16_t)geo3d_rom16(rp + 4);
    uint16_t th3 = (uint16_t)geo3d_rom16(rp + 6);
    int      textured = (th0 & 0x4000) != 0;
    uint32_t w  = 32u << (th0 & 7u), h = 32u << ((th0 >> 3) & 7u);
    uint32_t x  = 32u * (th2 & 0x3fu), y = 32u * ((th2 >> 6) & 0x1fu);
    uint32_t s  = (th2 >> 12) & 1u;          /* texture bank (texsheet) */
    uint32_t cb = (th3 >> 6) & 0x3ffu;       /* colorbase */
    uint32_t pal = GEO3D_PALETTE_OFF + cb * 2u;
    uint16_t col = ((size_t)pal + 2 <= main_data_size)
                   ? (uint16_t)geo3d_rom16(main_data + pal) : 0;
    if (mf) fprintf(mf,
        "face %4u th0=%04X th2=%04X th3=%04X textured=%d bank=%u tile=(%u,%u) %ux%u colorbase=%u color=%04X\n",
        f, th0, th2, th3, textured, s, x, y, w, h, cb, col);
    if (!textured) return;
    for (int t = 0; t < *nt; t++)
        if (tiles[t].s==s && tiles[t].x==x && tiles[t].y==y &&
            tiles[t].w==w && tiles[t].h==h) return;
    if (*nt < 256) tiles[(*nt)++] = (geo3d_extract_tile_t){ s, x, y, w, h };
}

/* Where a tile's texels come from: bank N of the 16MB textures ROM (1MB sheet
 * each) with --rombank, else the live texram of the tile's own bank. */
static const uint8_t *geo3d_extract_bank(const geo3d_extract_tile_t *t,
                                         const uint8_t *materials, size_t materials_size,
                                         const uint8_t *texram0, const uint8_t *texram1) {
    if (g_extract_rombank >= 0) {
        size_t bo = (size_t)g_extract_rombank * 0x100000u;
        return (bo + 0x100000u <= materials_size) ? (materials + bo) : NULL;
    }
    return t->s ? texram1 : texram0;
}

/* Binary PGM (P5) grayscale: each 4-bit luma texel -> 0..255, decoded with the
 * EXACT texram swizzle the renderer uses (16-bit halfword = 2x2 nibble block;
 * x>=1024 folds to y^=1024). Directly viewable / convertible. */
static void geo3d_extract_pgm(FILE *tf, const uint32_t *sheet, const geo3d_extract_tile_t *t) {
    uint32_t tx = t->x, ty = t->y, tw = t->w, th = t->h;
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
}

/* One tile's .pgm, and its line in the manifest. */
static void geo3d_extract_tile(FILE *mf, int model_idx, int t, const geo3d_extract_tile_t *tl,
                               const uint8_t *bank) {
    char path[160];
    uint32_t tx = tl->x, ty = tl->y, tw = tl->w, th = tl->h;
    if (g_extract_seq >= 0)
        snprintf(path, sizeof path, "model_%d_seq%03d_tile_%d_s%u_%ux%u_%u_%u.pgm",
                 model_idx, g_extract_seq, t, tl->s, tw, th, tx, ty);
    else
        snprintf(path, sizeof path, "model_%d_tile_%d_s%u_%ux%u_%u_%u.pgm",
                 model_idx, t, tl->s, tw, th, tx, ty);
    FILE *tf = fopen(path, "wb");
    if (!tf) return;
    geo3d_extract_pgm(tf, (const uint32_t *)bank, tl);
    fclose(tf);
    if (mf) fprintf(mf, "# tile %d bank=%u (%u,%u) %ux%u -> %s\n",
                    t, tl->s, tx, ty, tw, th, path);
}

static void geo3d_extract_model_texture(int model_idx,
        const uint8_t *main_data, size_t main_data_size,
        const uint8_t *materials, size_t materials_size,
        const uint8_t *texram0,   const uint8_t *texram1,
        uint32_t table_off, uint32_t table_count) {
    if (!main_data || !materials || model_idx < 0 ||
        (uint32_t)(model_idx + 1) >= table_count) return;
    uint32_t toff = table_off + (uint32_t)model_idx * MODEL_ENTRY_SIZE;
    if ((size_t)toff + 2u * MODEL_ENTRY_SIZE > main_data_size) return;
    uint32_t uv_ptr   = read_u32_le(GEO3D_ROM(main_data + toff + 0, 4));
    uint32_t mat_ptr  = read_u32_le(GEO3D_ROM(main_data + toff + 4, 4));
    uint32_t mesh_ptr = read_u32_le(GEO3D_ROM(main_data + toff + 8, 4));
    uint32_t mat_next = read_u32_le(GEO3D_ROM(main_data + toff + MODEL_ENTRY_SIZE + 4, 4));
    if (mat_next <= mat_ptr) return;
    uint32_t nfaces = (mat_next - mat_ptr) / 4u;   /* 8-byte (4 u16) records/face */
    if (nfaces > 8192u) nfaces = 8192u;
    uint32_t mat_base = mat_ptr * 2u;

    geo3d_extract_tile_t tiles[256]; int nt = 0;
    char path[160];
    snprintf(path, sizeof path, "model_%d_tex.txt", model_idx);
    FILE *mf = fopen(path, "w");
    if (mf) fprintf(mf, "# model %d  uv_ptr=%u mat_ptr=%u mesh_ptr=%u  faces=%u\n",
                    model_idx, uv_ptr, mat_ptr, mesh_ptr, nfaces);

    for (uint32_t f = 0; f < nfaces; f++) {
        uint32_t rec = mat_base + f * 8u;
        if ((size_t)rec + 8 > materials_size) break;
        geo3d_extract_face(mf, f, materials + rec, main_data, main_data_size, tiles, &nt);
    }
    if (mf) fprintf(mf, "# %d distinct textured tiles\n", nt);

    for (int t = 0; t < nt; t++) {
        const uint8_t *bank = geo3d_extract_bank(&tiles[t], materials, materials_size, texram0, texram1);
        if (bank) geo3d_extract_tile(mf, model_idx, t, &tiles[t], bank);
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

/* One capture of the live list, unless the filter leaves it out. */
static inline void geo3d_wire_capture(const geo3d_state_t *geo, const geo3d_models_t *md,
                                      int i, float lerp_t) {
    if (geo->isolate_index >= 0) {
        if (i != geo->isolate_index) return;
    } else if (geo->filter_enabled) {
        if (i < geo->filter_min || i > geo->filter_max) return;
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

    geo3d_decode_model(md, cm->model_idx, mat, cm->color[0], cm->color[1], cm->color[2]);
}

/* Single-model browser: derive colour from the model-table material ptr. */
static inline void geo3d_wire_browser(const geo3d_state_t *geo, const geo3d_models_t *md) {
    float cr = 0.0f, cg = 1.0f, cb = 0.0f;
    uint32_t toff = md->table_off + (uint32_t)geo->model_index * MODEL_ENTRY_SIZE;
    if (md->main_data && (size_t)toff + MODEL_ENTRY_SIZE <= md->main_data_size) {
        uint32_t mat_ptr = read_u32_le(GEO3D_ROM(md->main_data + toff + 4, 4));
        material_ptr_to_color(mat_ptr, &cr, &cg, &cb);
    }
    geo3d_decode_model(md, geo->model_index, NULL, cr, cg, cb);
}

static inline void geo3d_build_wireframes(geo3d_state_t *geo, const geo3d_models_t *md,
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
        for (int i = 0; i < geo->captured_count; i++)
            geo3d_wire_capture(geo, md, i, lerp_t);
    } else {
        geo3d_wire_browser(geo, md);
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
    geo->cam_x =  xpos;
    geo->cam_y =  ypos;
    geo->cam_z = -zpos;
    geo->rot_x = ((float)xang16 / 65536.0f) * TWO_PI;
    geo->rot_y = ((float)yang16 / 65536.0f) * TWO_PI;
    geo->has_game_view = true;
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

#endif /* GEO3D_H */
