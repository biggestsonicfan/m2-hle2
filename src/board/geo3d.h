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

/* Decode a Model 2 BGR555 colour word to normalized float RGB.
 *   bits 0-4 = R, bits 5-9 = G, bits 10-14 = B  (matches game's colpal()).
 * The material stream stores this as a little-endian uint16. */
static inline void geo3d_bgr555(uint16_t cw, float *r, float *g, float *b) {
    *r = (float)(((cw      ) & 0x1F) * 255 / 31) / 255.0f;
    *g = (float)(((cw >>  5) & 0x1F) * 255 / 31) / 255.0f;
    *b = (float)(((cw >> 10) & 0x1F) * 255 / 31) / 255.0f;
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
     * clip_win_x/y are the top-left origin of the window in game pixels.
     * clip_win_neutral_cam: true → portrait (neutral camera);
     *                       false → sub-window (game camera). */
    bool     has_clip_win;
    bool     clip_win_neutral_cam;
    /* True when this model was placed via the bone path (scan_active_bslot, i.e.
     * a fighter rob).  Bone geometry is in WORLD space; non-bone fight-stage
     * geometry comes through the FIFO already camera-relative (view space). */
    bool     from_bone;
    int16_t  clip_win_x;  /* left edge in game pixels */
    int16_t  clip_win_y;  /* top  edge in game pixels */
    int16_t  clip_win_w;  /* width  in game pixels */
    int16_t  clip_win_h;  /* height in game pixels */
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
    float x0, y0, z0,  u0, v0;
    float x1, y1, z1,  u1, v1;
    float x2, y2, z2,  u2, v2;
    float r, g, b;
} geo3d_tri_t;

typedef struct {
    geo3d_tri_t tris[GEO3D_MAX_TRIS];
    int         count;
} geo3d_tri_buf_t;

static geo3d_tri_buf_t g_geo3d_tris = {0};

static inline void geo3d_tris_reset(void) { g_geo3d_tris.count = 0; }

/* Emit a textured triangle: per-vertex position + atlas UV, plus a flat color. */
static inline void geo3d_emit_tri_uv(float x0, float y0, float z0, float u0, float v0,
                                      float x1, float y1, float z1, float u1, float v1,
                                      float x2, float y2, float z2, float u2, float v2,
                                      float r,  float g,  float b) {
    if (g_geo3d_tris.count >= GEO3D_MAX_TRIS) return;
    geo3d_tri_t *T = &g_geo3d_tris.tris[g_geo3d_tris.count++];
    T->x0=x0; T->y0=y0; T->z0=z0; T->u0=u0; T->v0=v0;
    T->x1=x1; T->y1=y1; T->z1=z1; T->u1=u1; T->v1=v1;
    T->x2=x2; T->y2=y2; T->z2=z2; T->u2=u2; T->v2=v2;
    T->r=r;   T->g=g;   T->b=b;
}

/* Backward-compatible: untextured triangle (UV = -1 → shader uses flat color). */
static inline void geo3d_emit_tri(float x0, float y0, float z0,
                                   float x1, float y1, float z1,
                                   float x2, float y2, float z2,
                                   float r,  float g,  float b) {
    geo3d_emit_tri_uv(x0,y0,z0,-1.0f,-1.0f, x1,y1,z1,-1.0f,-1.0f,
                      x2,y2,z2,-1.0f,-1.0f, r,g,b);
}

static inline void geo3d_emit_line(float x0, float y0, float z0,
                                    float x1, float y1, float z1,
                                    float r,  float g,  float b) {
    if (g_geo3d_lines.count >= GEO3D_MAX_LINES) return;
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

/* Portrait clip-window (chaos-portrait attract) framing tunables, dialed in via
 * the MCP bridge ("set_portrait_fov" / "set_portrait_cam_y").
 *   g_portrait_fov_deg : 0 = use the global FOV; >0 = override (smaller = zoom in).
 *   g_portrait_cam_y_frac : fraction of the character's own Y the neutral camera
 *     follows (1.0 = center on the char; 0.0 = origin camera). */
static float g_portrait_fov_deg    = 0.0f;  /* 0 = auto-derive from cell height (~23 deg) */
static float g_portrait_cam_y_frac = 0.0f;  /* 0 = no vertical centering (origin camera) */
static float g_portrait_row_shift  = 0.5f;  /* push top row down / bottom row up (toward screen center) */

/* Game-camera convention signs (dial live via the 3D window for the attract
 * "camera moving in wrong directions" bug). Multipliers applied in
 * geo3d_read_game_view to the camera struct's eye + angles. Defaults reproduce
 * the historical convention EXACTLY (z negated, yaw negated) so nothing changes
 * until toggled — once the right combo is found on-screen, bake it here. */
static float g_cam_sign_x  =  1.0f;
static float g_cam_sign_y  =  1.0f;
static float g_cam_sign_z  = -1.0f;  /* cam_z = -zpos */
static float g_cam_sign_rx =  1.0f;  /* pitch = +xang */
static float g_cam_sign_ry = -1.0f;  /* yaw   = -yang */

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
 * rects + the slow per-window render path (portrait neutral cams, sub-windows).
 * OFF: ignore windows entirely → every model renders full-screen through the
 * single game camera (fast path). Debug A/B for what the windows are doing. */
static int g_geo_windows_enabled = 1;

/* Non-portrait sub-windows: use the game camera (rotation) within the scissor
 * instead of the neutral camera. Fixes the Eggman-scene stage part (rotates
 * correctly) while keeping window support. Experimental A/B; default OFF. */
static int g_geo_subwin_game_cam = 0;

/* Texture UV orientation (dial in live via the geo3d window).  Defaults to the
 * user-observed correction: rotate 90 (swap u/v) + horizontal flip. */
static bool g_uv_swap   = false;
static bool g_uv_flip_u = false;
static bool g_uv_flip_v = false;
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
        /* Hook hasn't fired yet (first frame) — fall back to full ring. */
        total = g_cop.geo_capture_count;
        if (total > GEO_CAPTURE_SIZE) total = GEO_CAPTURE_SIZE;
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

    /* Clip window from GEO_WIN_SENTINEL — carries the clip rect for the
     * next draw call.  x/y/w/h are in game pixels (0–495, 0–383).
     * clip_win_is_portrait=true → portrait cell (neutral camera);
     * clip_win_is_portrait=false → sub-window (game camera). */
    bool   have_clip_win      = false;
    bool   clip_win_is_portrait = false;
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

    for (int i = 0; i < total && geo->captured_count < MAX_GEO_MODELS; i++) {
        int idx = (head - total + i + GEO_CAPTURE_SIZE) & (GEO_CAPTURE_SIZE - 1);
        uint32_t val = g_cop.geo_capture[idx];

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

        /* --- GEO_WIN_SENTINEL: clip window from set_window intercept (6 words) ---
         * FIFO words (after sentinel):
         *   word[0] = (x1<<16)|(511-y1)  — one corner of the clip rect
         *   word[1] = (x2<<16)|(511-y2)  — opposite corner
         *   word[2] = (cx<<16)|(511-cy)  — repeating center value (used for ID)
         *   words[3..5] = same as word[2]
         * All coordinates in game pixels (0–495 x, 0–383 y).
         *
         * If the center (cx,cy) matches one of the 8 STF chaos portrait cell
         * centers (off_9781C), apply a portrait-cell viewport + neutral camera.
         * Otherwise apply the rect as a sub-window viewport + game camera. */
        if (val == GEO_WIN_SENTINEL && i + 6 < total) {
            if (!g_geo_windows_enabled) { i += 6; continue; }  /* ignore windows (full-screen A/B) */
            /* Decode corners from FIFO words 0 and 1. */
            uint32_t fw0 = g_cop.geo_capture[(idx + 1) & (GEO_CAPTURE_SIZE-1)];
            uint32_t fw1 = g_cop.geo_capture[(idx + 2) & (GEO_CAPTURE_SIZE-1)];
            uint32_t fw2 = g_cop.geo_capture[(idx + 3) & (GEO_CAPTURE_SIZE-1)];
            int16_t xa   = (int16_t)(fw0 >> 16);
            int16_t ya   = (int16_t)(511 - (int16_t)(fw0 & 0xFFFF));
            int16_t xb   = (int16_t)(fw1 >> 16);
            int16_t yb   = (int16_t)(511 - (int16_t)(fw1 & 0xFFFF));
            int16_t x_cx = (int16_t)(fw2 >> 16);
            int16_t y_cy = (int16_t)(511 - (int16_t)(fw2 & 0xFFFF));
            /* Full rect from corners. */
            int16_t wl = (xa < xb) ? xa : xb;  /* left   */
            int16_t wr = (xa > xb) ? xa : xb;  /* right  */
            int16_t wt = (ya < yb) ? ya : yb;  /* top    */
            int16_t wb = (ya > yb) ? ya : yb;  /* bottom */
            /* Portrait centers (STF off_9781C). */
            static const int16_t px[4] = {60, 184, 308, 432};
            static const int16_t py[2] = {124, 260};
            bool is_portrait = false;
            for (int _xi = 0; _xi < 4 && !is_portrait; _xi++)
                for (int _yi = 0; _yi < 2 && !is_portrait; _yi++)
                    if (x_cx == px[_xi] && y_cy == py[_yi]) is_portrait = true;
            if (is_portrait) {
                /* Use the ACTUAL window rect — it is centered on the real portrait
                 * cell (py = 124 / 260), not the snapped VIDEO/4 x VIDEO/2 grid
                 * whose centers (96 / 288) put the top row ~28px high and forced
                 * an oversized 124x192 viewport (character looked small/far).
                 * Fall back to the snapped grid only if the rect is degenerate. */
                if (wr > wl && wb > wt) {
                    clip_win_x     = wl;
                    clip_win_y     = wt;
                    clip_win_w     = (int16_t)(wr - wl);
                    clip_win_h     = (int16_t)(wb - wt);
                } else {
                    int cell_w = VIDEO_WIDTH  / 4;
                    int cell_h = VIDEO_HEIGHT / 2;
                    clip_win_x = (int16_t)((x_cx / cell_w) * cell_w);
                    clip_win_y = (int16_t)((y_cy / cell_h) * cell_h);
                    clip_win_w = (int16_t)cell_w;
                    clip_win_h = (int16_t)cell_h;
                }
                have_clip_win      = true;
                clip_win_is_portrait = true;
            } else if (wr > wl && wb > wt) {
                /* Non-portrait sub-window: use actual rect, game camera. */
                clip_win_x         = wl;
                clip_win_y         = wt;
                clip_win_w         = (int16_t)(wr - wl);
                clip_win_h         = (int16_t)(wb - wt);
                have_clip_win      = true;
                clip_win_is_portrait = false;
            }
            i += 6;
            continue;
        }

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
            current_pos[1] = g_geo_shadow_floor_y;   /* ground plane (tunable) */
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
        cm->clip_win_neutral_cam = clip_win_is_portrait;
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
    static int    qt[GEO3D_IA_MAX_VPS];
    static int    idx[GEO3D_IA_MAX_IDX];

    int n_sv = 0, n_qt = 0, n_idx = 4;
    int vcount = 0;
    uint32_t mesh_offset;
    /* Material stream — one 8-byte record per face: [6-byte tex key][LE u16 BGR555].
     * Base address = (model-table[+0x04] pointer) * 2 into the textures ROM.
     * Format reverse-engineered from the Obj2StF converter (GophUndMe/Obj2StF). */
    uint32_t mat_base = 0;
    bool     have_mat = false;
    /* UV stream — model-table[+0x00] word index into the textures ROM; per polygon
     * NumVerts (pv,pu) 16-bit pairs, tile-relative texel = raw/8.  Vertex order in
     * the stream maps to decoder [A,B,D,C] (quad) / [A,B,C] (tri). */
    uint32_t uv_word = 0;     /* running word offset into materials[] for UVs */
    bool     have_uv = false;

    if (!main_data || !polygons) return;
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
            mat_base = mat_ptr_raw * 2u;
            have_mat = (mat_ptr_raw != 0);
            uint32_t uv_ptr_raw = read_u32_le(main_data + toff + 0);
            uv_word = uv_ptr_raw;          /* word index; byte = *2 */
            have_uv = (uv_ptr_raw != 0);
        }
        g_dbg_tex_models++;
        if (have_uv)  g_dbg_tex_models_uv++;
        if (have_mat) g_dbg_tex_models_mat++;
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

        if (matrix) { v1 = apply_matrix(v1, matrix); v2 = apply_matrix(v2, matrix); }

        sv[n_sv++] = v1;
        sv[n_sv++] = v2;
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
        bool textured = false;               /* texheader[0] bit14 = textured */
        if (have_mat) {
            uint32_t rec = mat_base + (uint32_t)fi * 8u;
            if ((size_t)rec + 8 <= materials_size) {
                uint16_t th0 = (uint16_t)materials[rec+0] | ((uint16_t)materials[rec+1]<<8);
                uint16_t th2 = (uint16_t)materials[rec+4] | ((uint16_t)materials[rec+5]<<8);
                uint16_t th3 = (uint16_t)materials[rec+6] | ((uint16_t)materials[rec+7]<<8);
                textured = (th0 & 0x4000) != 0;
                texw = 32u << (th0 & 0x7);
                texh = 32u << ((th0 >> 3) & 0x7);
                texx = 32u * (th2 & 0x3f);
                texy = 32u * ((th2 >> 6) & 0x1f);
                texsheet = (th2 >> 12) & 1u;            /* which texram bank */
                if      (g_uv_bank_mode == 1) texsheet = 0u;
                else if (g_uv_bank_mode == 2) texsheet = 1u;
                else if (g_uv_bank_mode == 3) texsheet ^= 1u;
                uint32_t matidx = (th3 >> 6) & 0x3ff;   /* colorbase → palette */
                uint32_t pal = GEO3D_PALETTE_OFF + matidx * 2u;
                if (main_data && (size_t)pal + 2 <= main_data_size) {
                    uint16_t cw = (uint16_t)main_data[pal] |
                                  ((uint16_t)main_data[pal + 1] << 8);
                    geo3d_bgr555(cw, &fr, &fg, &fb);
                }
            }
        }

        /* ---- UV stream: nv (pv,pu) pairs, mapped to [A,B,D,C]/[A,B,C] ---- */
        /* uvv[] indexed by vertex slot: 0=A 1=B 2=C 3=D */
        float uvu[4] = {-1,-1,-1,-1}, uvv[4] = {-1,-1,-1,-1};
        uint16_t cap_pu = 0, cap_pv = 0;
        if (have_uv) {
            static const int quad_slot[4] = {0,1,3,2};  /* stream k → A,B,D,C */
            static const int tri_slot[3]  = {0,1,2};    /* stream k → A,B,C   */
            const int *slot = tri_cnt ? tri_slot : quad_slot;
            for (int k = 0; k < nv; k++) {
                uint32_t b = (uv_word + (uint32_t)k * 2u) * 2u;  /* byte offset */
                if ((size_t)b + 4 > materials_size) break;
                uint16_t pv = (uint16_t)materials[b+0] | ((uint16_t)materials[b+1]<<8);
                uint16_t pu = (uint16_t)materials[b+2] | ((uint16_t)materials[b+3]<<8);
                if (k == 0) { cap_pu = pu; cap_pv = pv; }
                /* Only assign atlas UVs for textured faces; untextured faces
                 * keep uv=-1 so the shader uses the flat palette color.  Still
                 * advance the stream below to stay aligned with MAME. */
                if (!textured) continue;
                float tu = (float)pu / 8.0f, tv = (float)pv / 8.0f;
                if (g_uv_flip_u) tu = (float)texw - tu;
                if (g_uv_flip_v) tv = (float)texh - tv;
                if (g_uv_swap)   { float t = tu; tu = tv; tv = t; }
                float au = ((float)texx + tu) / (float)GEO3D_ATLAS_W;
                float av = ((float)(texsheet * GEO3D_SHEET_H) + (float)texy + tv)
                            / (float)GEO3D_ATLAS_H;
                uvu[slot[k]] = au; uvv[slot[k]] = av;
            }
        }
        uv_word += (uint32_t)nv * 2u;        /* advance for EVERY group */
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

        if (is_tri) {
            if (has_C) {
                geo3d_emit_line(A.x,A.y,A.z, B.x,B.y,B.z, fr,fg,fb);
                geo3d_emit_line(B.x,B.y,B.z, C.x,C.y,C.z, fr,fg,fb);
                geo3d_emit_line(C.x,C.y,C.z, A.x,A.y,A.z, fr,fg,fb);
                geo3d_emit_tri_uv(A.x,A.y,A.z, uvu[0],uvv[0],
                                  B.x,B.y,B.z, uvu[1],uvv[1],
                                  C.x,C.y,C.z, uvu[2],uvv[2], fr,fg,fb);
            } else {
                geo3d_emit_line(A.x,A.y,A.z, B.x,B.y,B.z, fr,fg,fb);
            }
        } else {
            /* Quad winding: A-B, B-D, D-C, C-A */
            geo3d_emit_line(A.x,A.y,A.z, B.x,B.y,B.z, fr,fg,fb);
            geo3d_emit_line(B.x,B.y,B.z, D.x,D.y,D.z, fr,fg,fb);
            geo3d_emit_line(D.x,D.y,D.z, C.x,C.y,C.z, fr,fg,fb);
            geo3d_emit_line(C.x,C.y,C.z, A.x,A.y,A.z, fr,fg,fb);
            /* Solid fill: two triangles ABD and ADC (slots 0,1,3 and 0,3,2) */
            if (has_D) {
                geo3d_emit_tri_uv(A.x,A.y,A.z, uvu[0],uvv[0],
                                  B.x,B.y,B.z, uvu[1],uvv[1],
                                  D.x,D.y,D.z, uvu[3],uvv[3], fr,fg,fb);
                geo3d_emit_tri_uv(A.x,A.y,A.z, uvu[0],uvv[0],
                                  D.x,D.y,D.z, uvu[3],uvv[3],
                                  C.x,C.y,C.z, uvu[2],uvv[2], fr,fg,fb);
            }
        }
    }
}

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

#endif /* GEO3D_H */
