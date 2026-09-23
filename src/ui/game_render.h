/*
 * game_render.h — draws the game (tile composite + later 3D wireframes) into
 * the swapchain as a letterboxed full-screen quad.
 *
 * Phase 8: only the tile pipeline (textured fullscreen quad sampling the
 *          video_state_t sg_image).
 * Phase 9: will add a line pipeline on top for 3D wireframes (same viewport).
 *
 * Call game_render_init() once after sg_setup().  Each frame, inside an
 * active swapchain pass and BEFORE simgui_render(), call
 * game_render_draw_game(view, ox, oy, w, h) to draw the letterboxed quad.
 */
#ifndef GAME_RENDER_H
#define GAME_RENDER_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "sokol_gfx.h"

#include "constants.h"
#include "geo3d.h"
#include "log.h"

typedef struct {
    float x, y;
    float u, v;
} game_render_quad_vertex_t;

typedef struct {
    float x, y, z;
    float r, g, b, a;
} game_render_line_vertex_t;

/* Textured fill vertex: position + flat color + tile-relative texel UV + atlas
 * tile rect (tx,ty,tw,th in pixels; tw<=0 → untextured).  The shader wraps the
 * interpolated UV within the tile per-pixel before sampling the atlas. */
typedef struct {
    float x, y, z;
    float r, g, b, a;
    float u, v;
    float tx, ty, tw, th;
    float lb, pl;               /* lumabase + poly_luma for the colorxlat luma ramp */
    float fl;                   /* GEO3D_FACE_* flags */
    float texlod;               /* the board's texlod, or GEO3D_TEXLOD_NONE */
    float zs;                   /* the polygon's sort z, or GEO3D_ZSORT_NONE */
    float zl;                   /* its layer as a [0, 1] depth offset (geo3d_mesh_layers) */
} game_render_tex_vertex_t;

typedef struct {
    float mvp[16];
} game_render_vs_params_t;

typedef struct {
    /* Tile (fullscreen-quad) pipeline */
    sg_buffer   quad_vbuf;
    sg_shader   tile_shader;
    sg_pipeline tile_pipeline;
    sg_sampler  tile_sampler;
    /* The same quad for pen layers (GL backends): the colour is looked up.
     * indexed_opaque has no blending, for the layer behind the 3D: its alpha is
     * 1 everywhere, where blending gives the source colour exactly, but a blended
     * draw keeps the GPU from dropping the pixels the 3D then covers. */
    sg_shader   indexed_shader;
    sg_pipeline indexed_pipeline, indexed_opaque;

    /* Render-target blit: the tile shader without blending (a finished frame's
     * alpha is whatever the last layer left), a quad oriented for the backend's
     * render-target rows, and a bilinear sampler for scaling. */
    sg_buffer   target_vbuf;
    sg_pipeline target_pipeline;
    sg_sampler  target_sampler_linear;

    /* Overlay layers a plugin painted (ui/overlay_plugin.h): the same quad as
     * the tiles, premultiplied-alpha blended, with the plugin's BGRA swizzled
     * in the shader so the layer format is one thing on every backend. */
    sg_shader   overlay_shader;
    sg_pipeline overlay_pipeline;

    /* Line (3D wireframe) pipeline */
    sg_buffer   line_vbuf;
    sg_shader   line_shader;
    sg_pipeline line_pipeline;

    /* Fill (3D solid/textured triangle) pipeline.  Three variants for the
     * backface-cull toggle: none / cull-CW-front / cull-CCW-front. */
    sg_buffer   fill_vbuf;
    sg_shader   fill_shader;
    sg_pipeline fill_pipeline;        /* no cull */
    sg_pipeline fill_pipeline_cw;     /* cull back, CW = front */
    sg_pipeline fill_pipeline_ccw;    /* cull back, CCW = front */
    /* --verify-fill only: the reference fill shader, the same three variants. */
    sg_shader   fill_shader_ref;
    sg_pipeline fill_ref[3];
    /* The fill shader with its discards taken out (GL backends), for faces that
     * are neither transparent nor checkered and so never reach them: a shader
     * that can discard keeps the GPU from rejecting hidden fragments before
     * shading them (early depth test, Mali's forward pixel kill). Same three
     * variants; id 0 where there is none. */
    sg_shader   fill_shader_opaque;
    sg_pipeline fill_opaque[3];

    /* Texture luma atlas (decoded from texram0): 2048×1024 R8 */
    sg_image    atlas_image;
    sg_view     atlas_view;
    sg_sampler  atlas_sampler;

    /* Live luma-ramp LUTs (re-uploaded each frame from the bus): lumaram
     * (0x11400000, 256×512 R8) + colorxlat (0x01810000, 256×192 R8). */
    sg_image    luma_image;
    sg_view     luma_view;
    sg_image    cxlat_image;
    sg_view     cxlat_view;
    sg_sampler  lut_sampler;

    /* Colour ramps (GL backends): row r holds, for luma index 0..63, the screen
     * colour of the face colour ramp_key[r] (r5 << 10 | g5 << 5 | b5) through the
     * colorxlat snapshot and the monitor curve, as 8-bit RGB. A fill vertex names
     * its row in colour alpha (row + 2; below 1.5 means none), so the shader
     * fetches the finished colour once instead of three colorxlat texels. */
    bool        ramp_enabled;
    sg_image    ramp_image;
    sg_view     ramp_view;
    int         ramp_count;
    bool        ramp_dirty;
    bool        cxlat_valid;
    uint8_t     cxlat_snap[COLORXLAT_SIZE];
    /* Shade rows (GL backends, --fill-shade-rows) share that pool. A row holds
     * the finished colour of all 121 texel steps for one (luma band, poly_luma,
     * face colour): the whole per-pixel chain — lumaram, the poly multiply, the
     * colorxlat ramp and the monitor curve — depends on nothing else, so a
     * textured pixel fetches its colour once instead of a lumaram texel and then
     * a ramp texel. The tables behind it move in ~2% of frames; a face's key
     * recurs, so about six rows a frame are new. */
    bool        shade_enabled;
    bool        luma_valid;
    uint8_t     luma_snap[LUMA_SIZE];

    /* CPU scratch for line uploads — 2 verts per geo3d_line_t */
    game_render_line_vertex_t line_verts[GEO3D_MAX_LINES * 2];

    /* CPU scratch for fill uploads — 3 verts per geo3d_tri_t */
    game_render_tex_vertex_t  fill_verts[GEO3D_MAX_TRIS * 3];

    bool        initialized;
} game_render_t;

static game_render_t g_game_render = {0};

/* Colour ramp rows (see game_render_t ramp_*): which row a colour key has, the
 * key of each row, the texels, and whether they went up this frame. */
#define GAME_RENDER_RAMP_ROWS 1024
#define GAME_RENDER_RAMP_W    128   /* 64 luma steps of a colour ramp, 121 texel steps of a shade row */
static uint16_t g_ramp_row_of[0x8000];                     /* key -> row + 1; 0: none */
static uint16_t g_ramp_key[GAME_RENDER_RAMP_ROWS];
static uint8_t  g_ramp_px[GAME_RENDER_RAMP_ROWS * GAME_RENDER_RAMP_W * 4];
static bool     g_ramp_uploaded;                           /* this frame; cleared at commit */

/* Shade rows: (luma band, poly_luma, colour) -> row, open addressing. A row's
 * key is kept so the rows can be rebuilt when lumaram or colorxlat change. */
#define GAME_RENDER_SHADE_SLOTS 4096
typedef struct { uint32_t lb; uint16_t poly, col; } game_render_shade_key_t;
static game_render_shade_key_t g_shade_key[GAME_RENDER_RAMP_ROWS];
static uint16_t g_shade_slot_row[GAME_RENDER_SHADE_SLOTS];  /* row + 1; 0: free */
static uint8_t  g_shade_is_row[GAME_RENDER_RAMP_ROWS];      /* this pool row is a shade row */

static void game_render__ramp_commit(void *user) { (void)user; g_ramp_uploaded = false; }

/* --verify-fill: set before game_render_init to build the reference fill shader.
 * While on, every fill draw of the frame is logged (viewport, scissor, matrix,
 * cull variant, vertex range) so main_sdl can replay the frame's fills through
 * each shader into scratch targets and compare them. A frame whose vertex buffer
 * was uploaded more than once (a batch that overflowed) cannot be replayed:
 * earlier draws' vertices are gone. */
static int g_game_render_fill_verify = 0;
/* Draw with the reference fill shader instead (timing comparisons). */
static int g_game_render_fill_use_ref = 0;
/* Draw faces that cannot discard with the discard-free fill shader (default);
 * 0 draws every face with the one shader (timing comparisons). */
static int g_game_render_fill_split = 1;
/* Give textured faces shade rows: one fetch for the lumaram step, poly_luma and
 * colour ramp together. Off by default. Set before game_render_init. */
static int g_game_render_fill_shade = 0;
/* Read the bilinear 2x2 with textureGather where the taps are the atlas's own
 * (one texture operation instead of four). Needs an ES 3.1 context on GLES.
 * Off by default. Set before game_render_init. */
static int g_game_render_fill_gather = 0;
/* Give faces colour ramp rows (default); 0 keeps the shader's colorxlat lookup
 * for all of them (timing comparisons). Set before game_render_init. */
static int g_game_render_fill_ramp = 1;

/* src with every "discard;" turned into an empty statement, into buf. */
static inline const char *game_render_strip_discard(const char *src, char *buf, size_t cap) {
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < cap; ) {
        if (!strncmp(p, "discard;", 8)) { buf[o++] = ';'; p += 8; }
        else buf[o++] = *p++;
    }
    buf[o] = '\0';
    return buf;
}
#define GAME_RENDER_FILL_LOG_MAX 4096
typedef struct {
    int   vx, vy, vw, vh;
    int   sx, sy, sw, sh;
    float mvp[16];
    int   cull, first, count, can_discard;
} game_render_fill_draw_t;
static struct {
    int                     n, uploads;
    bool                    overflow;
    int                     vx, vy, vw, vh;   /* the viewport in force */
    game_render_fill_draw_t d[GAME_RENDER_FILL_LOG_MAX];
} g_fill_log;

/* ---- Shaders ------------------------------------------------------------- */

static const char *game_render_tile_vs_glsl =
    "#version 410\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "  uv = a_uv;\n"
    "}\n";

static const char *game_render_tile_fs_glsl =
    "#version 410\n"
    "uniform sampler2D tex_smp;\n"
    "in vec2 uv;\n"
    "out vec4 frag_color;\n"
    "void main() { frag_color = texture(tex_smp, uv); }\n";

/* A layer the GPU tile compositor drew holds pens (low byte red, high byte
 * green, alpha where it drew): look the colour up in the pen texture. Sampled
 * like the colour layers, nearest, so each screen pixel takes the same texel. */
static const char *game_render_indexed_fs_glsl =
    "#version 410\n"
    "uniform sampler2D tex_smp;\n"
    "uniform sampler2D pal_smp;\n"
    "in vec2 uv;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "  vec4 t = texture(tex_smp, uv);\n"
    "  int pen = int(t.r * 255.0 + 0.5) | (int(t.g * 255.0 + 0.5) << 8);\n"
    "  frag_color = vec4(texelFetch(pal_smp, ivec2(pen & 255, pen >> 8), 0).rgb, t.a);\n"
    "}\n";

static const char *game_render_tile_vs_hlsl =
    "struct vs_in { float2 pos : POSITION; float2 uv : TEXCOORD0; };\n"
    "struct vs_out { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "vs_out main(vs_in inp) {\n"
    "  vs_out outp;\n"
    "  outp.pos = float4(inp.pos, 0.0, 1.0);\n"
    "  outp.uv = inp.uv;\n"
    "  return outp;\n"
    "}\n";

static const char *game_render_tile_fs_hlsl =
    "Texture2D<float4> tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "struct fs_in { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "float4 main(fs_in inp) : SV_Target0 { return tex.Sample(smp, inp.uv); }\n";

/* Overlay layer: the plugin hands out PREMULTIPLIED BGRA8 and the image is an
 * ordinary RGBA8 texture, so the swizzle happens here rather than in a second
 * copy on the CPU. One shader, every backend, no pixel-format negotiation. */
static const char *game_render_overlay_fs_glsl =
    "#version 410\n"
    "uniform sampler2D tex_smp;\n"
    "in vec2 uv;\n"
    "out vec4 frag_color;\n"
    "void main() { frag_color = texture(tex_smp, uv).bgra; }\n";

static const char *game_render_overlay_fs_hlsl =
    "Texture2D<float4> tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "struct fs_in { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "float4 main(fs_in inp) : SV_Target0 { return tex.Sample(smp, inp.uv).bgra; }\n";

/* Line shaders */
static const char *game_render_line_vs_glsl =
    "#version 410\n"
    "uniform vec4 vs_params[4];\n"
    "layout(location=0) in vec3 a_pos;\n"
    "layout(location=1) in vec4 a_color;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "  mat4 mvp = mat4(vs_params[0], vs_params[1], vs_params[2], vs_params[3]);\n"
    "  gl_Position = mvp * vec4(a_pos, 1.0);\n"
    "  color = a_color;\n"
    "}\n";

static const char *game_render_line_fs_glsl =
    "#version 410\n"
    "in vec4 color;\n"
    "out vec4 frag_color;\n"
    "void main() { frag_color = color; }\n";

static const char *game_render_line_vs_hlsl =
    "cbuffer params : register(b0) { float4x4 mvp; };\n"
    "struct vs_in { float3 pos : POSITION; float4 color : COLOR0; };\n"
    "struct vs_out { float4 pos : SV_Position; float4 color : COLOR0; };\n"
    "vs_out main(vs_in inp) {\n"
    "  vs_out outp;\n"
    "  outp.pos = mul(mvp, float4(inp.pos, 1.0));\n"
    "  outp.color = inp.color;\n"
    "  return outp;\n"
    "}\n";

static const char *game_render_line_fs_hlsl =
    "struct fs_in { float4 pos : SV_Position; float4 color : COLOR0; };\n"
    "float4 main(fs_in inp) : SV_Target0 { return inp.color; }\n";

/* Textured fill shaders: flat color modulated by 4-bit luma atlas.
 * u<0 → untextured (flat color).  Luma (0..1) scaled ×2 so the tile average
 * (~mid luma) reproduces the base palette color; clamped to white. */
static const char *game_render_fill_vs_glsl =
    "#version 410\n"
    "uniform vec4 vs_params[4];\n"
    "layout(location=0) in vec3 a_pos;\n"
    "layout(location=1) in vec4 a_color;\n"
    "layout(location=2) in vec2 a_uv;\n"
    "layout(location=3) in vec4 a_tile;\n"
    "layout(location=4) in vec4 a_lbpl;\n"
    "layout(location=5) in vec2 a_zs;\n"   /* sort z, layer offset */
    "out vec4 color;\n"
    "out vec2 uv;\n"
    "out float ez;\n"
    "flat out vec4 tile;\n"
    "flat out vec4 lbpl;\n"
    /* Per-face integers the fill works out per pixel otherwise: the GEO3D_FACE_*
     * flags (plus 64 when both tile sides are powers of two) and the colour's
     * 5-bit channels. A face's three vertices carry the same values. */
    "flat out ivec4 face;\n"
    "flat out ivec2 tile_log2;\n"   /* log2 of the tile sides (GLSL ES 3.00 has no findMSB) */
    "flat out int ramp_row;\n"      /* colour alpha - 2: the face's colour ramp row, or -1 */
    /* A decal is its surface's own faces drawn again, exactly on top, and passes on
     * a depth TIE (LESS_EQUAL). Since the fill was split, the surface can go through
     * the discard-free program and its decal through the discarding one, and two
     * programs are only promised the same gl_Position for the same inputs if it is
     * declared invariant. Without this a driver may let them differ in the last
     * bit, and a mouth or an eye z-fights with the face it is painted on. */
    "invariant gl_Position;\n"
    "void main() {\n"
    "  mat4 mvp = mat4(vs_params[0], vs_params[1], vs_params[2], vs_params[3]);\n"
    "  gl_Position = mvp * vec4(a_pos, 1.0);\n"
    /* The board's polygon z-sort (geo3d.h geo3d_sort_z): the whole polygon is
     * given one z, so the vertex keeps its own x, y and w and takes that z
     * through the same two rows of the matrix. Carried as z/w rather than as a
     * depth, because the clipper interpolates z and w together and their ratio
     * is what survives. Only for a vertex the camera is in front of: behind the
     * lens w is negative and the clamp would hand back the near plane, which
     * tells the clipper to cut the edge at the vertex it should be keeping and
     * throws the polygon out whole. */
    "  if (a_zs.x < 1.0e29 && gl_Position.w > 0.0) {\n"
    "    float zc = mvp[0][2]*a_pos.x + mvp[1][2]*a_pos.y + mvp[2][2]*a_zs.x + mvp[3][2];\n"
    "    float zw = mvp[0][3]*a_pos.x + mvp[1][3]*a_pos.y + mvp[2][3]*a_zs.x + mvp[3][3];\n"
    "    gl_Position.z = clamp(zc / max(zw, 1e-6), -1.0, 1.0) * gl_Position.w;\n"
    "  }\n"
    /* A face lying on others in its plane is pulled in front of them by its layer
     * (geo3d_mesh_layers), in [0, 1] depth units: twice that in GL's [-1, 1]. */
    "  if (gl_Position.w > 0.0) gl_Position.z -= 2.0 * a_zs.y * gl_Position.w;\n"
    "  color = a_color; uv = a_uv; tile = a_tile; lbpl = a_lbpl; ez = -a_pos.z;\n"
    "  int fl = int(a_lbpl.z + 0.5), tw = int(a_tile.z), th = int(a_tile.w);\n"
    "  if (tw > 0 && th > 0 && (tw & (tw - 1)) == 0 && (th & (th - 1)) == 0) fl |= 64;\n"
    "  ramp_row = int(a_color.a + 0.5) - 2;\n"
    "  tile_log2 = ivec2(0);\n"
    "  for (int i = 1; i < 16; i++) {\n"
    "    if ((tw >> i) != 0) tile_log2.x = i;\n"
    "    if ((th >> i) != 0) tile_log2.y = i;\n"
    "  }\n"
    "  face = ivec4(fl, int(a_color.r*31.0+0.5), int(a_color.g*31.0+0.5), int(a_color.b*31.0+0.5));\n"
    "}\n";

/* model2rd.ipp fast_log2's fraction table: floor(256 * log2(1 + i/128)). */
#define GAME_RENDER_LOG2_TABLE \
    "0,2,5,8,11,14,16,19,22,25,27,30,33,35,38,40,43,46,48,51,53,56,58,61,63,65,68,70,73,75,77,80," \
    "82,84,87,89,91,93,96,98,100,102,104,106,109,111,113,115,117,119,121,123,125,127,129,132,134,136,138,140,141,143,145,147," \
    "149,151,153,155,157,159,161,162,164,166,168,170,172,173,175,177,179,181,182,184,186,188,189,191,193,194,196,198,200,201,203,205," \
    "206,208,209,211,213,214,216,218,219,221,222,224,225,227,229,230,232,233,235,236,238,239,241,242,244,245,247,248,250,251,253,254"

/* fast_log2 for the GLSL fills: the float's own exponent and the top seven bits
 * of its mantissa, which is what model2rd.ipp reads out of f2u(z). The same
 * value as floor(log2(z)) and z / 2^e for every normal z > 0, exact by
 * construction, and GLSL ES 3.00 (GLES 3, WebGL2) can compile it: that dialect
 * has floatBitsToInt and no ldexp. */
#define GAME_RENDER_FAST_LOG2_GLSL \
    "const int LOG2[128] = int[128](" GAME_RENDER_LOG2_TABLE ");\n" \
    "int fast_log2(float z) {\n" \
    "  if (z <= 0.0) return 0;\n" \
    "  int b = floatBitsToInt(z);\n" \
    "  return (((b >> 23) & 255) - 127) * 256 + LOG2[(b >> 16) & 127];\n" \
    "}\n"

/*
 * The fill — MAME's model2rd.ipp draw_scanline_tex, as the explorer ports it
 * (vendor/noclip js/viewer.js FRAG_SHADER), whose texel path this follows line
 * for line so the two can be held against each other.
 *
 *   texel     bilinear over the 4-bit sheet with the board's half-texel offset,
 *             each tap wrapped within the tile the way the index mask wraps,
 *             and mirrored in an odd copy when the face says so (u = ~u).
 *   level     the mip chain send_lod_data_q box-filters into texture RAM: level
 *             L of a tile sits at ((tx-2048)>>L)&2047, ((ty-1024)>>L)&1023 on
 *             the sheet that alternates with L (fetch_bilinear_texel). Picked
 *             the board's way (draw_scanline_tex): mml = fast_log2(z) - texlod
 *             in 1/128ths, level mml >> 7, and a blend (mml & 127) << 1 of the
 *             next level down. z is eye depth, so the choice does not depend on
 *             the window's size, and texlod comes from the geometrizer's LOD
 *             distance: a game sets how blurry a surface is per polygon. The
 *             title's Death Egg names tiny coefficients, and a screen-space
 *             pick there left its lone bright texels unfiltered, over the
 *             colorxlat step: twice MAME's white pixels. Faces with no texlod
 *             (not from a display list) fall back to screen-space derivatives.
 *   holes     on the transparent renderer a texel of 15 carries no colour: it
 *             borrows its pair's, and the four holes' weights blend into a
 *             coverage that must reach half a texel for the pixel to survive.
 *   checker   the half-transparency bit draws every other pixel.
 *   ramp      lumaram[lumabase + (t >> 1)] with t the *filtered* texel in 8.4,
 *             * poly_luma → luma6 → colorxlat[ch][(c5<<8)+luma6] → gamma
 *             max(c-64,0)*255/191. Indexing by the nearest of the sixteen
 *             texels instead quantises a blended pixel onto a neighbouring
 *             palette slot, a different colour rather than a nearby shade.
 *
 * Atlas texels are nibble*17, so a hole reads back exactly 1.0. The lod is
 * taken before any discard or branch: derivatives are undefined past either.
 * An untextured face goes through the same colorxlat ramp with luma
 * poly_luma >> 2 and no texel (model2rd.ipp draw_scanline_solid).
 * lb < 0 falls back to the old flat_color*luma path.
 */
static const char *game_render_fill_fs_ref_glsl =
    "#version 410\n"
    "uniform sampler2D atlas_smp;\n"
    "uniform sampler2D luma_smp;\n"
    "uniform sampler2D cxlat_smp;\n"
    "in vec4 color;\n"
    "in vec2 uv;\n"
    "in float ez;\n"
    "flat in vec4 tile;\n"
    "flat in vec4 lbpl;\n"
    "out vec4 frag_color;\n"
    GAME_RENDER_FAST_LOG2_GLSL
    "bool has(int bit) { return (int(lbpl.z + 0.5) & bit) != 0; }\n"
    "ivec4 level_tile(int L) {\n"
    "  int sheet = has(4) ? 1 : 0;\n"
    "  uint x = (uint(int(tile.x)) - 2048u) >> uint(L);\n"
    "  uint y = (uint(int(tile.y) - sheet * 1024) - 1024u) >> uint(L);\n"
    "  return ivec4(int(x & 2047u), int(y & 1023u) + ((sheet + L) & 1) * 1024,\n"
    "               max(int(tile.z) >> L, 1), max(int(tile.w) >> L, 1));\n"
    "}\n"
    "float tile_texel(ivec4 t, ivec2 p) {\n"
    "  ivec2 size = t.zw;\n"
    "  ivec2 s = p + size * 8;\n"
    "  ivec2 q = ivec2(uvec2(s) % uvec2(size));\n"
    "  ivec2 copy = ivec2(uvec2(s) / uvec2(size));\n"
    "  if (has(8) && (copy.x & 1) != 0) q.x = size.x - 1 - q.x;\n"
    "  if (has(16) && (copy.y & 1) != 0) q.y = size.y - 1 - q.y;\n"
    "  return texelFetch(atlas_smp, (t.xy + q) & 2047, 0).r;\n"
    "}\n"
    "vec2 sample_level(int L) {\n"
    "  ivec4 t = level_tile(L);\n"
    "  vec2 c = uv / pow(2.0, float(L)) - 0.5;\n"
    "  ivec2 i0 = ivec2(floor(c));\n"
    "  vec2 f = fract(c);\n"
    "  float t00 = tile_texel(t, i0);\n"
    "  float t10 = tile_texel(t, i0 + ivec2(1, 0));\n"
    "  float t01 = tile_texel(t, i0 + ivec2(0, 1));\n"
    "  float t11 = tile_texel(t, i0 + ivec2(1, 1));\n"
    "  float a = 1.0;\n"
    "  if (has(1)) {\n"
    "    vec4 cover = 1.0 - step(1.0, vec4(t00, t10, t01, t11));\n"
    "    a = mix(mix(cover.x, cover.y, f.x), mix(cover.z, cover.w, f.x), f.y);\n"
    "    if (t00 == 1.0) t00 = t10;\n"
    "    if (t10 == 1.0) t10 = t00;\n"
    "    if (t01 == 1.0) t01 = t11;\n"
    "    if (t11 == 1.0) t11 = t01;\n"
    "  }\n"
    "  float row0 = mix(t00, t10, f.x);\n"
    "  float row1 = mix(t01, t11, f.x);\n"
    "  if (has(1)) {\n"
    "    if (row0 == 1.0) row0 = row1;\n"
    "    if (row1 == 1.0) row1 = row0;\n"
    "  }\n"
    "  return vec2(mix(row0, row1, f.y), a);\n"
    "}\n"
    "void main() {\n"
    "  float lmax = floor(log2(max(min(tile.z, tile.w), 2.0)) + 0.5) - 1.0;\n"
    "  float lod = clamp(log2(max(length(dFdx(uv)), length(dFdy(uv)))), 0.0, lmax);\n"
    "  if (has(2) && ((int(gl_FragCoord.x) ^ int(gl_FragCoord.y)) & 1) == 0) discard;\n"
    "  vec3 rgb = color.rgb;\n"
    "  if (tile.z > 0.0) {\n"
    "    int L0 = int(floor(lod));\n"
    "    float blend = fract(lod);\n"
    "    if (lbpl.w < 100000.0) {\n"
    "      int mml = fast_log2(ez) - int(lbpl.w);\n"
    "      int maxl = int(lmax);\n"
    "      L0 = clamp(mml >> 7, 0, maxl);\n"
    "      blend = (mml > 0 && L0 < maxl) ? float((mml & 127) * 2) / 256.0 : 0.0;\n"
    "    }\n"
    "    vec2 tx = mix(sample_level(L0), sample_level(L0 + 1), blend);\n"
    "    if (has(1) && tx.y < 0.5) discard;\n"
    "    float al = tx.x;\n"
    "    if (lbpl.x < 0.0) {\n"
    "      if (al > 0.0) rgb = clamp(color.rgb * al * 2.0, 0.0, 1.0);\n"
    "    } else {\n"
    "      int lbyte = 2 * (int(lbpl.x) + int(al * 120.0));\n"
    "      float lram = texelFetch(luma_smp, ivec2(lbyte & 255, lbyte >> 8), 0).r * 255.0;\n"
    "      float poly = floor(clamp(lbpl.y, 0.0, 1.0) * 255.0 + 0.5);\n"
    "      int li = int(min(floor(lram * poly / 256.0), 63.0));\n"
    "      int r5 = int(color.r*31.0+0.5), g5 = int(color.g*31.0+0.5), b5 = int(color.b*31.0+0.5);\n"
    "      int br = ((r5<<8)+li)*2, bg = 0x4000+((g5<<8)+li)*2, bb = 0x8000+((b5<<8)+li)*2;\n"
    "      float cr = texelFetch(cxlat_smp, ivec2(br & 255, br >> 8), 0).r * 255.0;\n"
    "      float cg = texelFetch(cxlat_smp, ivec2(bg & 255, bg >> 8), 0).r * 255.0;\n"
    "      float cb = texelFetch(cxlat_smp, ivec2(bb & 255, bb >> 8), 0).r * 255.0;\n"
    "      vec3 c = max(vec3(cr,cg,cb) - 64.0, 0.0) * (255.0/191.0);\n"
    "      rgb = clamp(c / 255.0, 0.0, 1.0);\n"
    "    }\n"
    "  } else if (lbpl.x < 0.0) {\n"
    "    rgb = color.rgb * clamp(lbpl.y, 0.0, 1.0);\n"
    "  } else {\n"
    "    int li = min(int(floor(clamp(lbpl.y, 0.0, 1.0) * 255.0 + 0.5)) >> 2, 63);\n"
    "    int r5 = int(color.r*31.0+0.5), g5 = int(color.g*31.0+0.5), b5 = int(color.b*31.0+0.5);\n"
    "    int br = ((r5<<8)+li)*2, bg = 0x4000+((g5<<8)+li)*2, bb = 0x8000+((b5<<8)+li)*2;\n"
    "    float cr = texelFetch(cxlat_smp, ivec2(br & 255, br >> 8), 0).r * 255.0;\n"
    "    float cg = texelFetch(cxlat_smp, ivec2(bg & 255, bg >> 8), 0).r * 255.0;\n"
    "    float cb = texelFetch(cxlat_smp, ivec2(bb & 255, bb >> 8), 0).r * 255.0;\n"
    "    rgb = clamp(max(vec3(cr,cg,cb) - 64.0, 0.0) * (255.0/191.0) / 255.0, 0.0, 1.0);\n"
    "  }\n"
    "  frag_color = vec4(rgb, 1.0);\n"
    "}\n";

/*
 * The fill as drawn on GL: game_render_fill_fs_ref_glsl above, pixel for pixel
 * (main_sdl --verify-fill replays each frame's fills through both and compares
 * the bytes), with the work per pixel cut where the result cannot change:
 *   - the flags and colour channels come from the vertex stage as flat ints;
 *   - a tile side is a power of two (32 << n, halved per level), so the wrap's
 *     % and / are & and >> of values that are never negative there;
 *   - the second mip level is fetched only when it has weight: mix(a, b, 0.0)
 *     is a, so at lod 0 (every magnified texel) or a whole level, 4 fetches
 *     instead of 8.
 */
static const char *game_render_fill_fs_glsl =
    "#version 410\n"
    "uniform sampler2D atlas_smp;\n"
    "uniform sampler2D luma_smp;\n"
    "uniform sampler2D cxlat_smp;\n"
    "in vec4 color;\n"
    "in vec2 uv;\n"
    "in float ez;\n"
    "flat in vec4 tile;\n"
    "flat in vec4 lbpl;\n"
    "uniform sampler2D ramp_smp;\n"
    "flat in ivec4 face;\n"
    "flat in ivec2 tile_log2;\n"
    "flat in int ramp_row;\n"
    "out vec4 frag_color;\n"
    GAME_RENDER_FAST_LOG2_GLSL
    "ivec4 level_tile(int L) {\n"
    "  int sheet = (face.x & 4) != 0 ? 1 : 0;\n"
    "  uint x = (uint(int(tile.x)) - 2048u) >> uint(L);\n"
    "  uint y = (uint(int(tile.y) - sheet * 1024) - 1024u) >> uint(L);\n"
    "  return ivec4(int(x & 2047u), int(y & 1023u) + ((sheet + L) & 1) * 1024,\n"
    "               max(int(tile.z) >> L, 1), max(int(tile.w) >> L, 1));\n"
    "}\n"
    "float tile_texel(ivec4 t, ivec2 sh, ivec2 p) {\n"
    "  ivec2 s = p + t.zw * 8;\n"
    "  ivec2 q, copy;\n"
    "  if ((face.x & 64) != 0) { q = s & (t.zw - 1); copy = s >> sh; }\n"
    "  else                    { q = ivec2(uvec2(s) % uvec2(t.zw)); copy = ivec2(uvec2(s) / uvec2(t.zw)); }\n"
    "  if ((face.x & 8) != 0 && (copy.x & 1) != 0) q.x = t.z - 1 - q.x;\n"
    "  if ((face.x & 16) != 0 && (copy.y & 1) != 0) q.y = t.w - 1 - q.y;\n"
    "  return texelFetch(atlas_smp, (t.xy + q) & 2047, 0).r;\n"
    "}\n"
    "vec2 sample_level(int L) {\n"
    "  ivec4 t = level_tile(L);\n"
    "  ivec2 sh = max(tile_log2 - ivec2(L), ivec2(0));\n"   /* log2 of t.zw = max(side >> L, 1) */
    "  vec2 c = uv / pow(2.0, float(L)) - 0.5;\n"
    "  ivec2 i0 = ivec2(floor(c));\n"
    "  vec2 f = fract(c);\n"
    "  float t00, t10, t01, t11;\n"
    /* USE_GATHER: where the four taps are the atlas's own 2x2 — the tile does
     * not wrap or mirror between them and they do not cross the 2048 fold —
     * textureGather returns all four in one texture operation. Raw texels, no
     * filtering, so they are the texelFetch values exactly; P sits on their
     * shared corner, (i0 + 1) / 2048, which is exact in binary32. Anything else
     * keeps the four fetches. */
    "#ifdef USE_GATHER\n"
    "  ivec2 s0 = i0 + t.zw * 8;\n"
    "  ivec2 q0 = (face.x & 64) != 0 ? (s0 & (t.zw - 1)) : ivec2(uvec2(s0) % uvec2(t.zw));\n"
    "  ivec2 a0 = t.xy + q0;\n"
    "  if ((face.x & 24) == 0 && q0.x + 1 < t.z && q0.y + 1 < t.w && a0.x + 1 < 2048 && a0.y + 1 < 2048) {\n"
    "    vec4 g = textureGather(atlas_smp, (vec2(a0) + 1.0) / 2048.0, 0);\n"
    "    t00 = g.w; t10 = g.z; t01 = g.x; t11 = g.y;\n"
    "  } else {\n"
    "    t00 = tile_texel(t, sh, i0);\n"
    "    t10 = tile_texel(t, sh, i0 + ivec2(1, 0));\n"
    "    t01 = tile_texel(t, sh, i0 + ivec2(0, 1));\n"
    "    t11 = tile_texel(t, sh, i0 + ivec2(1, 1));\n"
    "  }\n"
    "#else\n"
    "  t00 = tile_texel(t, sh, i0);\n"
    "  t10 = tile_texel(t, sh, i0 + ivec2(1, 0));\n"
    "  t01 = tile_texel(t, sh, i0 + ivec2(0, 1));\n"
    "  t11 = tile_texel(t, sh, i0 + ivec2(1, 1));\n"
    "#endif\n"
    "  float a = 1.0;\n"
    "  if ((face.x & 1) != 0) {\n"
    "    vec4 cover = 1.0 - step(1.0, vec4(t00, t10, t01, t11));\n"
    "    a = mix(mix(cover.x, cover.y, f.x), mix(cover.z, cover.w, f.x), f.y);\n"
    "    if (t00 == 1.0) t00 = t10;\n"
    "    if (t10 == 1.0) t10 = t00;\n"
    "    if (t01 == 1.0) t01 = t11;\n"
    "    if (t11 == 1.0) t11 = t01;\n"
    "  }\n"
    "  float row0 = mix(t00, t10, f.x);\n"
    "  float row1 = mix(t01, t11, f.x);\n"
    "  if ((face.x & 1) != 0) {\n"
    "    if (row0 == 1.0) row0 = row1;\n"
    "    if (row1 == 1.0) row1 = row0;\n"
    "  }\n"
    "  return vec2(mix(row0, row1, f.y), a);\n"
    "}\n"
    "vec3 ramp(int li) {\n"
    "  int br = ((face.y<<8)+li)*2, bg = 0x4000+((face.z<<8)+li)*2, bb = 0x8000+((face.w<<8)+li)*2;\n"
    "  float cr = texelFetch(cxlat_smp, ivec2(br & 255, br >> 8), 0).r * 255.0;\n"
    "  float cg = texelFetch(cxlat_smp, ivec2(bg & 255, bg >> 8), 0).r * 255.0;\n"
    "  float cb = texelFetch(cxlat_smp, ivec2(bb & 255, bb >> 8), 0).r * 255.0;\n"
    "  return vec3(cr, cg, cb);\n"
    "}\n"
    /* The screen colour at luma index li: the face's colour ramp row when it has
     * one (the same bytes, worked out on the CPU), else the colorxlat texels. */
    "vec3 shade(int li) {\n"
    "  if (ramp_row >= 0) return texelFetch(ramp_smp, ivec2(li, ramp_row), 0).rgb;\n"
    "  return clamp(max(ramp(li) - 64.0, 0.0) * (255.0/191.0) / 255.0, 0.0, 1.0);\n"
    "}\n"
    "void main() {\n"
    "  float lmax = floor(log2(max(min(tile.z, tile.w), 2.0)) + 0.5) - 1.0;\n"
    "  float lod = clamp(log2(max(length(dFdx(uv)), length(dFdy(uv)))), 0.0, lmax);\n"
    "  if ((face.x & 2) != 0 && ((int(gl_FragCoord.x) ^ int(gl_FragCoord.y)) & 1) == 0) discard;\n"
    "  vec3 rgb = color.rgb;\n"
    "  if (tile.z > 0.0) {\n"
    "    int L0 = int(floor(lod));\n"
    "    float blend = fract(lod);\n"
    "    if (lbpl.w < 100000.0) {\n"
    "      int mml = fast_log2(ez) - int(lbpl.w);\n"
    "      int maxl = int(lmax);\n"
    "      L0 = clamp(mml >> 7, 0, maxl);\n"
    "      blend = (mml > 0 && L0 < maxl) ? float((mml & 127) * 2) / 256.0 : 0.0;\n"
    "    }\n"
    "    vec2 tx = sample_level(L0);\n"
    "    if (blend != 0.0) tx = mix(tx, sample_level(L0 + 1), blend);\n"
    "    if ((face.x & 1) != 0 && tx.y < 0.5) discard;\n"
    "    float al = tx.x;\n"
    "    if (lbpl.x < 0.0) {\n"
    "      if (al > 0.0) rgb = clamp(color.rgb * al * 2.0, 0.0, 1.0);\n"
    /* Flag 128: colour alpha names a shade row, which already holds this face's
     * lumaram step, poly_luma and colour ramp — one fetch for the two below. */
    "    } else if ((face.x & 128) != 0) {\n"
    "      rgb = texelFetch(ramp_smp, ivec2(int(al * 120.0), ramp_row), 0).rgb;\n"
    "    } else {\n"
    "      int lbyte = 2 * (int(lbpl.x) + int(al * 120.0));\n"
    "      float lram = texelFetch(luma_smp, ivec2(lbyte & 255, lbyte >> 8), 0).r * 255.0;\n"
    "      float poly = floor(clamp(lbpl.y, 0.0, 1.0) * 255.0 + 0.5);\n"
    "      int li = int(min(floor(lram * poly / 256.0), 63.0));\n"
    "      rgb = shade(li);\n"
    "    }\n"
    "  } else if (lbpl.x < 0.0) {\n"
    "    rgb = color.rgb * clamp(lbpl.y, 0.0, 1.0);\n"
    "  } else {\n"
    "    int li = min(int(floor(clamp(lbpl.y, 0.0, 1.0) * 255.0 + 0.5)) >> 2, 63);\n"
    "    rgb = shade(li);\n"
    "  }\n"
    "  frag_color = vec4(rgb, 1.0);\n"
    "}\n";

static const char *game_render_fill_vs_hlsl =
    "cbuffer params : register(b0) { float4x4 mvp; };\n"
    "struct vs_in { float3 pos : POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; float4 tile : TEXCOORD1; float4 lbpl : TEXCOORD2; float2 zs : TEXCOORD3; };\n"
    "struct vs_out { float4 pos : SV_Position; float4 color : COLOR0; float2 uv : TEXCOORD0; nointerpolation float4 tile : TEXCOORD1; nointerpolation float4 lbpl : TEXCOORD2; float ez : TEXCOORD3; };\n"
    "vs_out main(vs_in inp) {\n"
    "  vs_out outp;\n"
    "  outp.pos = mul(mvp, float4(inp.pos, 1.0));\n"
    /* The board's polygon z-sort — see the GLSL vertex shader above. */
    "  if (inp.zs.x < 1.0e29 && outp.pos.w > 0.0) {\n"
    "    float4 pz = float4(inp.pos.xy, inp.zs.x, 1.0);\n"
    "    outp.pos.z = clamp(dot(mvp[2], pz) / max(dot(mvp[3], pz), 1e-6), -1.0, 1.0) * outp.pos.w;\n"
    "  }\n"
    "  if (outp.pos.w > 0.0) outp.pos.z -= inp.zs.y * outp.pos.w;\n"
    "  outp.color = inp.color; outp.uv = inp.uv; outp.tile = inp.tile; outp.lbpl = inp.lbpl; outp.ez = -inp.pos.z;\n"
    "  return outp;\n"
    "}\n";

static const char *game_render_fill_fs_hlsl =
    "Texture2D<float4> atlas : register(t0);\n"
    "Texture2D<float4> lumat : register(t1);\n"
    "Texture2D<float4> cxlat : register(t2);\n"
    "SamplerState smp : register(s0);\n"
    "struct fs_in { float4 pos : SV_Position; float4 color : COLOR0; float2 uv : TEXCOORD0; nointerpolation float4 tile : TEXCOORD1; nointerpolation float4 lbpl : TEXCOORD2; float ez : TEXCOORD3; };\n"
    "static const int LOG2[128] = { " GAME_RENDER_LOG2_TABLE " };\n"
    "bool has(float fl, int bit) { return (((int)(fl + 0.5)) & bit) != 0; }\n"
    "int fast_log2(float z) {\n"
    "  if (z <= 0.0) return 0;\n"
    "  float e = floor(log2(z));\n"
    "  float m = z / ldexp(1.0, e);\n"
    "  if (m >= 2.0) { e += 1.0; m *= 0.5; }\n"
    "  if (m < 1.0) { e -= 1.0; m *= 2.0; }\n"
    "  return (int)e * 256 + LOG2[min((int)((m - 1.0) * 128.0), 127)];\n"
    "}\n"
    "int4 level_tile(float4 tile, float fl, int L) {\n"
    "  int sheet = has(fl, 4) ? 1 : 0;\n"
    "  uint x = ((uint)(int)tile.x - 2048u) >> (uint)L;\n"
    "  uint y = ((uint)((int)tile.y - sheet * 1024) - 1024u) >> (uint)L;\n"
    "  return int4((int)(x & 2047u), (int)(y & 1023u) + ((sheet + L) & 1) * 1024,\n"
    "              max(((int)tile.z) >> L, 1), max(((int)tile.w) >> L, 1));\n"
    "}\n"
    "float tile_texel(int4 t, float fl, int2 p) {\n"
    "  int2 size = t.zw;\n"
    "  int2 s = p + size * 8;\n"
    "  int2 q = (int2)((uint2)s % (uint2)size);\n"
    "  int2 copy = (int2)((uint2)s / (uint2)size);\n"
    "  if (has(fl, 8) && (copy.x & 1) != 0) q.x = size.x - 1 - q.x;\n"
    "  if (has(fl, 16) && (copy.y & 1) != 0) q.y = size.y - 1 - q.y;\n"
    "  return atlas.Load(int3((t.xy + q) & 2047, 0)).r;\n"
    "}\n"
    "float2 sample_level(float2 uv, float4 tile, float fl, int L) {\n"
    "  int4 t = level_tile(tile, fl, L);\n"
    "  float2 c = uv / pow(2.0, (float)L) - 0.5;\n"
    "  int2 i0 = (int2)floor(c);\n"
    "  float2 f = frac(c);\n"
    "  float t00 = tile_texel(t, fl, i0);\n"
    "  float t10 = tile_texel(t, fl, i0 + int2(1, 0));\n"
    "  float t01 = tile_texel(t, fl, i0 + int2(0, 1));\n"
    "  float t11 = tile_texel(t, fl, i0 + int2(1, 1));\n"
    "  float a = 1.0;\n"
    "  if (has(fl, 1)) {\n"
    "    float4 cover = 1.0 - step(1.0, float4(t00, t10, t01, t11));\n"
    "    a = lerp(lerp(cover.x, cover.y, f.x), lerp(cover.z, cover.w, f.x), f.y);\n"
    "    if (t00 == 1.0) t00 = t10;\n"
    "    if (t10 == 1.0) t10 = t00;\n"
    "    if (t01 == 1.0) t01 = t11;\n"
    "    if (t11 == 1.0) t11 = t01;\n"
    "  }\n"
    "  float row0 = lerp(t00, t10, f.x);\n"
    "  float row1 = lerp(t01, t11, f.x);\n"
    "  if (has(fl, 1)) {\n"
    "    if (row0 == 1.0) row0 = row1;\n"
    "    if (row1 == 1.0) row1 = row0;\n"
    "  }\n"
    "  return float2(lerp(row0, row1, f.y), a);\n"
    "}\n"
    "float4 main(fs_in inp) : SV_Target0 {\n"
    "  float fl = inp.lbpl.z;\n"
    "  float lmax = floor(log2(max(min(inp.tile.z, inp.tile.w), 2.0)) + 0.5) - 1.0;\n"
    "  float lod = clamp(log2(max(length(ddx(inp.uv)), length(ddy(inp.uv)))), 0.0, lmax);\n"
    "  if (has(fl, 2) && ((((int)inp.pos.x) ^ ((int)inp.pos.y)) & 1) == 0) discard;\n"
    "  float3 rgb = inp.color.rgb;\n"
    "  if (inp.tile.z > 0.0) {\n"
    "    int L0 = (int)floor(lod);\n"
    "    float blend = frac(lod);\n"
    "    if (inp.lbpl.w < 100000.0) {\n"
    "      int mml = fast_log2(inp.ez) - (int)inp.lbpl.w;\n"
    "      int maxl = (int)lmax;\n"
    "      L0 = clamp(mml >> 7, 0, maxl);\n"
    "      blend = (mml > 0 && L0 < maxl) ? (float)((mml & 127) * 2) / 256.0 : 0.0;\n"
    "    }\n"
    "    float2 tx = lerp(sample_level(inp.uv, inp.tile, fl, L0),\n"
    "                     sample_level(inp.uv, inp.tile, fl, L0 + 1), blend);\n"
    "    if (has(fl, 1) && tx.y < 0.5) discard;\n"
    "    float al = tx.x;\n"
    "    if (inp.lbpl.x < 0.0) {\n"
    "      if (al > 0.0) rgb = clamp(inp.color.rgb * al * 2.0, 0.0, 1.0);\n"
    "    } else {\n"
    "      int lbyte = 2 * ((int)inp.lbpl.x + (int)(al * 120.0));\n"
    "      float lram = lumat.Load(int3(lbyte & 255, lbyte >> 8, 0)).r * 255.0;\n"
    "      float poly = floor(clamp(inp.lbpl.y, 0.0, 1.0) * 255.0 + 0.5);\n"
    "      int li = (int)min(floor(lram * poly / 256.0), 63.0);\n"
    "      int r5 = (int)(inp.color.r*31.0+0.5), g5 = (int)(inp.color.g*31.0+0.5), b5 = (int)(inp.color.b*31.0+0.5);\n"
    "      int br = ((r5<<8)+li)*2, bg = 0x4000+((g5<<8)+li)*2, bb = 0x8000+((b5<<8)+li)*2;\n"
    "      float cr = cxlat.Load(int3(br & 255, br >> 8, 0)).r * 255.0;\n"
    "      float cg = cxlat.Load(int3(bg & 255, bg >> 8, 0)).r * 255.0;\n"
    "      float cb = cxlat.Load(int3(bb & 255, bb >> 8, 0)).r * 255.0;\n"
    "      float3 c = max(float3(cr,cg,cb) - 64.0, 0.0) * (255.0/191.0);\n"
    "      rgb = clamp(c / 255.0, 0.0, 1.0);\n"
    "    }\n"
    "  } else if (inp.lbpl.x < 0.0) {\n"
    "    rgb = inp.color.rgb * clamp(inp.lbpl.y, 0.0, 1.0);\n"
    "  } else {\n"
    "    int li = min(((int)floor(clamp(inp.lbpl.y, 0.0, 1.0) * 255.0 + 0.5)) >> 2, 63);\n"
    "    int r5 = (int)(inp.color.r*31.0+0.5), g5 = (int)(inp.color.g*31.0+0.5), b5 = (int)(inp.color.b*31.0+0.5);\n"
    "    int br = ((r5<<8)+li)*2, bg = 0x4000+((g5<<8)+li)*2, bb = 0x8000+((b5<<8)+li)*2;\n"
    "    float cr = cxlat.Load(int3(br & 255, br >> 8, 0)).r * 255.0;\n"
    "    float cg = cxlat.Load(int3(bg & 255, bg >> 8, 0)).r * 255.0;\n"
    "    float cb = cxlat.Load(int3(bb & 255, bb >> 8, 0)).r * 255.0;\n"
    "    rgb = clamp(max(float3(cr,cg,cb) - 64.0, 0.0) * (255.0/191.0) / 255.0, 0.0, 1.0);\n"
    "  }\n"
    "  return float4(rgb, 1.0);\n"
    "}\n";

/* GLES 3 compiles the same GLSL once "#version 410" becomes "#version 300 es"
 * with explicit default precisions: the fill shader's texel indices, luma ramp
 * and colorxlat offsets need highp, and ES would default float to mediump in the
 * fragment stage and sampler2D to lowp. sg_make_shader compiles synchronously,
 * so one buffer per stage only has to outlive that call. */
static inline const char *game_render_glsl(sg_backend backend, const char *src, int stage) {
    static const char head[] = "#version 410\n";
    /* textureGather needs ES 3.10 (it is core in GL 4.0), so the fills take that
     * header when the gather path is compiled in. */
    static const char es_head_300[] = "#version 300 es\n";
    static const char es_head_310[] = "#version 310 es\n";
    static const char es_prec[] =
        "precision highp float;\n"
        "precision highp int;\n"
        "precision highp sampler2D;\n"
        "precision highp usampler2D;\n";
    static const char gather_def[] = "#define USE_GATHER 1\n";
    static char buf[2][16384];
    if (strncmp(src, head, sizeof head - 1) != 0) return src;
    /* The whole program shares one version, so with the gather path compiled in
     * every ES shader takes the 3.10 header (3.00 sources are valid there); the
     * define goes only to the fill shader, which is the one that reads it. */
    bool gather = g_game_render_fill_gather != 0;
    bool define = gather && strstr(src, "USE_GATHER") != NULL;
    if (backend != SG_BACKEND_GLES3 && !define) return src;
    const char *body = src + sizeof head - 1;
    char *out = buf[stage], *end = out + sizeof buf[0];
    #define GAME_RENDER_GLSL_PUT(text) do { \
        size_t n_ = strlen(text); \
        if (out + n_ + 1 > end) { LOG_ERROR("game_render_glsl: shader too long"); return src; } \
        memcpy(out, (text), n_); out += n_; \
    } while (0)
    if (backend == SG_BACKEND_GLES3) {
        GAME_RENDER_GLSL_PUT(gather ? es_head_310 : es_head_300);
        GAME_RENDER_GLSL_PUT(es_prec);
    } else {
        GAME_RENDER_GLSL_PUT(head);
    }
    if (define) GAME_RENDER_GLSL_PUT(gather_def);
    GAME_RENDER_GLSL_PUT(body);
    #undef GAME_RENDER_GLSL_PUT
    *out = '\0';
    return buf[stage];
}

/* ---- Init ---------------------------------------------------------------- */

static inline void game_render_init(void) {
    if (g_game_render.initialized) return;

    sg_backend backend = sg_query_backend();
    LOG_INFO("game_render_init: backend=%d", (int)backend);

    /* Fullscreen quad in clip space (-1..1), UV flipped on V so the top-left
     * of the texture lands at the top-left of the quad. */
    game_render_quad_vertex_t quad[6] = {
        { -1.0f, -1.0f, 0.0f, 1.0f },
        {  1.0f, -1.0f, 1.0f, 1.0f },
        {  1.0f,  1.0f, 1.0f, 0.0f },
        { -1.0f, -1.0f, 0.0f, 1.0f },
        {  1.0f,  1.0f, 1.0f, 0.0f },
        { -1.0f,  1.0f, 0.0f, 0.0f },
    };
    g_game_render.quad_vbuf = sg_make_buffer(&(sg_buffer_desc){
        .usage = { .vertex_buffer = true, .immutable = true },
        .data  = SG_RANGE(quad),
        .label = "game-render-quad-vbuf",
    });

    {
        sg_shader_desc d;
        memset(&d, 0, sizeof(d));
        d.attrs[0].hlsl_sem_name  = "POSITION";
        d.attrs[0].hlsl_sem_index = 0;
        d.attrs[0].base_type      = SG_SHADERATTRBASETYPE_FLOAT;
        d.attrs[1].hlsl_sem_name  = "TEXCOORD";
        d.attrs[1].hlsl_sem_index = 0;
        d.attrs[1].base_type      = SG_SHADERATTRBASETYPE_FLOAT;

        d.views[0].texture.stage              = SG_SHADERSTAGE_FRAGMENT;
        d.views[0].texture.image_type         = SG_IMAGETYPE_2D;
        d.views[0].texture.sample_type        = SG_IMAGESAMPLETYPE_FLOAT;
        d.views[0].texture.hlsl_register_t_n  = 0;
        d.views[0].texture.msl_texture_n      = 0;
        d.views[0].texture.wgsl_group1_binding_n = 0;

        d.samplers[0].stage             = SG_SHADERSTAGE_FRAGMENT;
        d.samplers[0].sampler_type      = SG_SAMPLERTYPE_FILTERING;
        d.samplers[0].hlsl_register_s_n = 0;
        d.samplers[0].msl_sampler_n     = 0;
        d.samplers[0].wgsl_group1_binding_n = 0;

        d.texture_sampler_pairs[0].stage        = SG_SHADERSTAGE_FRAGMENT;
        d.texture_sampler_pairs[0].view_slot    = 0;
        d.texture_sampler_pairs[0].sampler_slot = 0;
        d.texture_sampler_pairs[0].glsl_name    = "tex_smp";

        d.label = "game-render-tile-shader";
        if (backend == SG_BACKEND_GLCORE || backend == SG_BACKEND_GLES3) {
            d.vertex_func.source   = game_render_glsl(backend, game_render_tile_vs_glsl, 0);
            d.fragment_func.source = game_render_glsl(backend, game_render_tile_fs_glsl, 1);
        } else if (backend == SG_BACKEND_D3D11) {
            d.vertex_func.source       = game_render_tile_vs_hlsl;
            d.vertex_func.d3d11_target = "vs_4_0";
            d.fragment_func.source     = game_render_tile_fs_hlsl;
            d.fragment_func.d3d11_target = "ps_4_0";
        }
        g_game_render.tile_shader = sg_make_shader(&d);
    }

    {
        sg_pipeline_desc p;
        memset(&p, 0, sizeof(p));
        p.shader                  = g_game_render.tile_shader;
        p.primitive_type          = SG_PRIMITIVETYPE_TRIANGLES;
        p.layout.attrs[0].format  = SG_VERTEXFORMAT_FLOAT2;
        p.layout.attrs[0].offset  = offsetof(game_render_quad_vertex_t, x);
        p.layout.attrs[1].format  = SG_VERTEXFORMAT_FLOAT2;
        p.layout.attrs[1].offset  = offsetof(game_render_quad_vertex_t, u);
        p.layout.buffers[0].stride = sizeof(game_render_quad_vertex_t);
        /* Alpha blend so the FG layer (alpha-keyed) composites over the 3D scene.
         * BG has alpha=255 everywhere, so blending leaves it fully opaque. */
        p.colors[0].blend.enabled        = true;
        p.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
        p.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        p.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
        p.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ZERO;
        p.label = "game-render-tile-pipeline";
        g_game_render.tile_pipeline = sg_make_pipeline(&p);

        if (backend == SG_BACKEND_GLCORE || backend == SG_BACKEND_GLES3) {
            sg_shader_desc d;
            memset(&d, 0, sizeof d);
            d.attrs[0].base_type = SG_SHADERATTRBASETYPE_FLOAT;
            d.attrs[1].base_type = SG_SHADERATTRBASETYPE_FLOAT;
            static const char *const names[2] = { "tex_smp", "pal_smp" };
            for (int i = 0; i < 2; i++) {
                d.views[i].texture.stage       = SG_SHADERSTAGE_FRAGMENT;
                d.views[i].texture.image_type  = SG_IMAGETYPE_2D;
                d.views[i].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
                d.texture_sampler_pairs[i].stage        = SG_SHADERSTAGE_FRAGMENT;
                d.texture_sampler_pairs[i].view_slot    = i;
                d.texture_sampler_pairs[i].sampler_slot = 0;
                d.texture_sampler_pairs[i].glsl_name    = names[i];
            }
            d.samplers[0].stage        = SG_SHADERSTAGE_FRAGMENT;
            d.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
            d.vertex_func.source   = game_render_glsl(backend, game_render_tile_vs_glsl, 0);
            d.fragment_func.source = game_render_glsl(backend, game_render_indexed_fs_glsl, 1);
            d.label = "game-render-indexed-shader";
            g_game_render.indexed_shader = sg_make_shader(&d);
            p.shader = g_game_render.indexed_shader;
            p.label  = "game-render-indexed-pipeline";
            g_game_render.indexed_pipeline = sg_make_pipeline(&p);
            p.colors[0].blend.enabled = false;
            p.label  = "game-render-indexed-opaque";
            g_game_render.indexed_opaque = sg_make_pipeline(&p);
            p.colors[0].blend.enabled = true;
        }
    }

    g_game_render.tile_sampler = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_NEAREST,
        .mag_filter = SG_FILTER_NEAREST,
        .wrap_u     = SG_WRAP_CLAMP_TO_EDGE,
        .wrap_v     = SG_WRAP_CLAMP_TO_EDGE,
        .label      = "game-render-tile-sampler",
    });

    /* ---- Render-target blit ----------------------------------------------- */
    {
        /* GL render targets store their bottom row first, so the blit quad
         * samples v=0 at the bottom; D3D's top row first, like the tile quads. */
        bool gl = backend == SG_BACKEND_GLCORE || backend == SG_BACKEND_GLES3;
        game_render_quad_vertex_t tq[6];
        memcpy(tq, quad, sizeof tq);
        if (gl) for (int i = 0; i < 6; i++) tq[i].v = 1.0f - tq[i].v;
        g_game_render.target_vbuf = sg_make_buffer(&(sg_buffer_desc){
            .usage = { .vertex_buffer = true, .immutable = true },
            .data  = SG_RANGE(tq),
            .label = "game-render-target-vbuf",
        });
        sg_pipeline_desc p;
        memset(&p, 0, sizeof(p));
        p.shader                   = g_game_render.tile_shader;
        p.primitive_type           = SG_PRIMITIVETYPE_TRIANGLES;
        p.layout.attrs[0].format   = SG_VERTEXFORMAT_FLOAT2;
        p.layout.attrs[0].offset   = offsetof(game_render_quad_vertex_t, x);
        p.layout.attrs[1].format   = SG_VERTEXFORMAT_FLOAT2;
        p.layout.attrs[1].offset   = offsetof(game_render_quad_vertex_t, u);
        p.layout.buffers[0].stride = sizeof(game_render_quad_vertex_t);
        p.label = "game-render-target-pipeline";
        g_game_render.target_pipeline = sg_make_pipeline(&p);
        g_game_render.target_sampler_linear = sg_make_sampler(&(sg_sampler_desc){
            .min_filter = SG_FILTER_LINEAR,
            .mag_filter = SG_FILTER_LINEAR,
            .wrap_u     = SG_WRAP_CLAMP_TO_EDGE,
            .wrap_v     = SG_WRAP_CLAMP_TO_EDGE,
            .label      = "game-render-target-sampler",
        });
    }

    /* ---- Overlay layer blit ----------------------------------------------- */
    {
        sg_shader_desc d;
        memset(&d, 0, sizeof(d));
        d.attrs[0].hlsl_sem_name  = "POSITION";
        d.attrs[0].hlsl_sem_index = 0;
        d.attrs[0].base_type      = SG_SHADERATTRBASETYPE_FLOAT;
        d.attrs[1].hlsl_sem_name  = "TEXCOORD";
        d.attrs[1].hlsl_sem_index = 0;
        d.attrs[1].base_type      = SG_SHADERATTRBASETYPE_FLOAT;
        d.views[0].texture.stage                 = SG_SHADERSTAGE_FRAGMENT;
        d.views[0].texture.image_type            = SG_IMAGETYPE_2D;
        d.views[0].texture.sample_type           = SG_IMAGESAMPLETYPE_FLOAT;
        d.views[0].texture.hlsl_register_t_n     = 0;
        d.views[0].texture.msl_texture_n         = 0;
        d.views[0].texture.wgsl_group1_binding_n = 0;
        d.samplers[0].stage                 = SG_SHADERSTAGE_FRAGMENT;
        d.samplers[0].sampler_type          = SG_SAMPLERTYPE_FILTERING;
        d.samplers[0].hlsl_register_s_n     = 0;
        d.samplers[0].msl_sampler_n         = 0;
        d.samplers[0].wgsl_group1_binding_n = 0;
        d.texture_sampler_pairs[0].stage        = SG_SHADERSTAGE_FRAGMENT;
        d.texture_sampler_pairs[0].view_slot    = 0;
        d.texture_sampler_pairs[0].sampler_slot = 0;
        d.texture_sampler_pairs[0].glsl_name    = "tex_smp";
        d.label = "game-render-overlay-shader";
        if (backend == SG_BACKEND_GLCORE || backend == SG_BACKEND_GLES3) {
            d.vertex_func.source   = game_render_glsl(backend, game_render_tile_vs_glsl, 0);
            d.fragment_func.source = game_render_glsl(backend, game_render_overlay_fs_glsl, 1);
        } else if (backend == SG_BACKEND_D3D11) {
            d.vertex_func.source         = game_render_tile_vs_hlsl;
            d.vertex_func.d3d11_target   = "vs_4_0";
            d.fragment_func.source       = game_render_overlay_fs_hlsl;
            d.fragment_func.d3d11_target = "ps_4_0";
        }
        g_game_render.overlay_shader = sg_make_shader(&d);

        sg_pipeline_desc p;
        memset(&p, 0, sizeof(p));
        p.shader                   = g_game_render.overlay_shader;
        p.primitive_type           = SG_PRIMITIVETYPE_TRIANGLES;
        p.layout.attrs[0].format   = SG_VERTEXFORMAT_FLOAT2;
        p.layout.attrs[0].offset   = offsetof(game_render_quad_vertex_t, x);
        p.layout.attrs[1].format   = SG_VERTEXFORMAT_FLOAT2;
        p.layout.attrs[1].offset   = offsetof(game_render_quad_vertex_t, u);
        p.layout.buffers[0].stride = sizeof(game_render_quad_vertex_t);
        /* Premultiplied source-over: ONE / ONE_MINUS_SRC_ALPHA on both. */
        p.colors[0].blend.enabled          = true;
        p.colors[0].blend.src_factor_rgb   = SG_BLENDFACTOR_ONE;
        p.colors[0].blend.dst_factor_rgb   = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        p.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
        p.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        p.label = "game-render-overlay-pipeline";
        g_game_render.overlay_pipeline = sg_make_pipeline(&p);
    }

    /* ---- Line pipeline ---------------------------------------------------- */

    g_game_render.line_vbuf = sg_make_buffer(&(sg_buffer_desc){
        .usage = { .vertex_buffer = true, .stream_update = true },
        .size  = sizeof(g_game_render.line_verts),
        .label = "game-render-line-vbuf",
    });

    {
        sg_shader_desc d;
        memset(&d, 0, sizeof(d));
        d.attrs[0].hlsl_sem_name  = "POSITION";
        d.attrs[0].hlsl_sem_index = 0;
        d.attrs[0].base_type      = SG_SHADERATTRBASETYPE_FLOAT;
        d.attrs[1].hlsl_sem_name  = "COLOR";
        d.attrs[1].hlsl_sem_index = 0;
        d.attrs[1].base_type      = SG_SHADERATTRBASETYPE_FLOAT;

        d.uniform_blocks[0].stage                    = SG_SHADERSTAGE_VERTEX;
        d.uniform_blocks[0].size                     = sizeof(game_render_vs_params_t);
        d.uniform_blocks[0].hlsl_register_b_n        = 0;
        d.uniform_blocks[0].msl_buffer_n             = 0;
        d.uniform_blocks[0].wgsl_group0_binding_n    = 0;
        d.uniform_blocks[0].glsl_uniforms[0].glsl_name   = "vs_params";
        d.uniform_blocks[0].glsl_uniforms[0].type        = SG_UNIFORMTYPE_FLOAT4;
        d.uniform_blocks[0].glsl_uniforms[0].array_count = 4;

        d.label = "game-render-line-shader";
        if (backend == SG_BACKEND_GLCORE || backend == SG_BACKEND_GLES3) {
            d.vertex_func.source   = game_render_glsl(backend, game_render_line_vs_glsl, 0);
            d.fragment_func.source = game_render_glsl(backend, game_render_line_fs_glsl, 1);
        } else if (backend == SG_BACKEND_D3D11) {
            d.vertex_func.source         = game_render_line_vs_hlsl;
            d.vertex_func.d3d11_target   = "vs_4_0";
            d.fragment_func.source       = game_render_line_fs_hlsl;
            d.fragment_func.d3d11_target = "ps_4_0";
        }
        g_game_render.line_shader = sg_make_shader(&d);
    }

    {
        sg_pipeline_desc p;
        memset(&p, 0, sizeof(p));
        p.shader                   = g_game_render.line_shader;
        p.primitive_type           = SG_PRIMITIVETYPE_LINES;
        p.layout.attrs[0].format   = SG_VERTEXFORMAT_FLOAT3;
        p.layout.attrs[0].offset   = offsetof(game_render_line_vertex_t, x);
        p.layout.attrs[1].format   = SG_VERTEXFORMAT_FLOAT4;
        p.layout.attrs[1].offset   = offsetof(game_render_line_vertex_t, r);
        p.layout.buffers[0].stride = sizeof(game_render_line_vertex_t);
        p.label = "game-render-line-pipeline";
        g_game_render.line_pipeline = sg_make_pipeline(&p);
    }

    /* ---- Fill (solid/textured triangle) pipeline -------------------------- */
    g_game_render.fill_vbuf = sg_make_buffer(&(sg_buffer_desc){
        .usage = { .vertex_buffer = true, .stream_update = true },
        .size  = GEO3D_MAX_TRIS * 3 * sizeof(game_render_tex_vertex_t),
        .label = "game-render-fill-vbuf",
    });
    {
        sg_shader_desc d;
        memset(&d, 0, sizeof(d));
        d.attrs[0].hlsl_sem_name  = "POSITION"; d.attrs[0].base_type = SG_SHADERATTRBASETYPE_FLOAT;
        d.attrs[1].hlsl_sem_name  = "COLOR";    d.attrs[1].base_type = SG_SHADERATTRBASETYPE_FLOAT;
        d.attrs[2].hlsl_sem_name  = "TEXCOORD"; d.attrs[2].hlsl_sem_index = 0; d.attrs[2].base_type = SG_SHADERATTRBASETYPE_FLOAT;
        d.attrs[3].hlsl_sem_name  = "TEXCOORD"; d.attrs[3].hlsl_sem_index = 1; d.attrs[3].base_type = SG_SHADERATTRBASETYPE_FLOAT;
        d.attrs[4].hlsl_sem_name  = "TEXCOORD"; d.attrs[4].hlsl_sem_index = 2; d.attrs[4].base_type = SG_SHADERATTRBASETYPE_FLOAT;
        d.attrs[5].hlsl_sem_name  = "TEXCOORD"; d.attrs[5].hlsl_sem_index = 3; d.attrs[5].base_type = SG_SHADERATTRBASETYPE_FLOAT;
        d.uniform_blocks[0].stage                 = SG_SHADERSTAGE_VERTEX;
        d.uniform_blocks[0].size                  = sizeof(game_render_vs_params_t);
        d.uniform_blocks[0].hlsl_register_b_n     = 0;
        d.uniform_blocks[0].msl_buffer_n          = 0;
        d.uniform_blocks[0].wgsl_group0_binding_n = 0;
        d.uniform_blocks[0].glsl_uniforms[0].glsl_name   = "vs_params";
        d.uniform_blocks[0].glsl_uniforms[0].type        = SG_UNIFORMTYPE_FLOAT4;
        d.uniform_blocks[0].glsl_uniforms[0].array_count = 4;
        d.views[0].texture.stage              = SG_SHADERSTAGE_FRAGMENT;
        d.views[0].texture.image_type         = SG_IMAGETYPE_2D;
        d.views[0].texture.sample_type        = SG_IMAGESAMPLETYPE_FLOAT;
        d.views[0].texture.hlsl_register_t_n  = 0;
        d.views[0].texture.msl_texture_n      = 0;
        d.views[0].texture.wgsl_group1_binding_n = 0;
        d.samplers[0].stage             = SG_SHADERSTAGE_FRAGMENT;
        d.samplers[0].sampler_type      = SG_SAMPLERTYPE_FILTERING;
        d.samplers[0].hlsl_register_s_n = 0;
        d.samplers[0].msl_sampler_n     = 0;
        d.samplers[0].wgsl_group1_binding_n = 0;
        d.texture_sampler_pairs[0].stage        = SG_SHADERSTAGE_FRAGMENT;
        d.texture_sampler_pairs[0].view_slot    = 0;
        d.texture_sampler_pairs[0].sampler_slot = 0;
        d.texture_sampler_pairs[0].glsl_name    = "atlas_smp";
        /* Luma-ramp LUTs: lumaram (t1) + colorxlat (t2), integer-fetched. */
        d.views[1].texture.stage              = SG_SHADERSTAGE_FRAGMENT;
        d.views[1].texture.image_type         = SG_IMAGETYPE_2D;
        d.views[1].texture.sample_type        = SG_IMAGESAMPLETYPE_FLOAT;
        d.views[1].texture.hlsl_register_t_n  = 1;
        d.views[1].texture.msl_texture_n      = 1;
        d.views[1].texture.wgsl_group1_binding_n = 2;
        d.views[2].texture.stage              = SG_SHADERSTAGE_FRAGMENT;
        d.views[2].texture.image_type         = SG_IMAGETYPE_2D;
        d.views[2].texture.sample_type        = SG_IMAGESAMPLETYPE_FLOAT;
        d.views[2].texture.hlsl_register_t_n  = 2;
        d.views[2].texture.msl_texture_n      = 2;
        d.views[2].texture.wgsl_group1_binding_n = 3;
        d.texture_sampler_pairs[1].stage        = SG_SHADERSTAGE_FRAGMENT;
        d.texture_sampler_pairs[1].view_slot    = 1;
        d.texture_sampler_pairs[1].sampler_slot = 0;
        d.texture_sampler_pairs[1].glsl_name    = "luma_smp";
        d.texture_sampler_pairs[2].stage        = SG_SHADERSTAGE_FRAGMENT;
        d.texture_sampler_pairs[2].view_slot    = 2;
        d.texture_sampler_pairs[2].sampler_slot = 0;
        d.texture_sampler_pairs[2].glsl_name    = "cxlat_smp";
        /* Colour ramps (t3): read by the GL fill shader only. */
        d.views[3].texture.stage              = SG_SHADERSTAGE_FRAGMENT;
        d.views[3].texture.image_type         = SG_IMAGETYPE_2D;
        d.views[3].texture.sample_type        = SG_IMAGESAMPLETYPE_FLOAT;
        d.views[3].texture.hlsl_register_t_n  = 3;
        d.views[3].texture.msl_texture_n      = 3;
        d.views[3].texture.wgsl_group1_binding_n = 4;
        d.texture_sampler_pairs[3].stage        = SG_SHADERSTAGE_FRAGMENT;
        d.texture_sampler_pairs[3].view_slot    = 3;
        d.texture_sampler_pairs[3].sampler_slot = 0;
        d.texture_sampler_pairs[3].glsl_name    = "ramp_smp";
        d.label = "game-render-fill-shader";
        if (backend == SG_BACKEND_GLCORE || backend == SG_BACKEND_GLES3) {
            d.vertex_func.source   = game_render_glsl(backend, game_render_fill_vs_glsl, 0);
            d.fragment_func.source = game_render_glsl(backend, g_game_render_fill_use_ref ? game_render_fill_fs_ref_glsl
                                                                                           : game_render_fill_fs_glsl, 1);
        } else if (backend == SG_BACKEND_D3D11) {
            d.vertex_func.source         = game_render_fill_vs_hlsl;
            d.vertex_func.d3d11_target   = "vs_4_0";
            d.fragment_func.source       = game_render_fill_fs_hlsl;
            d.fragment_func.d3d11_target = "ps_4_0";
        }
        g_game_render.fill_shader = sg_make_shader(&d);
        if (backend == SG_BACKEND_GLCORE || backend == SG_BACKEND_GLES3) {
            static char opaque_src[16384];
            d.vertex_func.source   = game_render_glsl(backend, game_render_fill_vs_glsl, 0);
            d.fragment_func.source = game_render_glsl(backend,
                game_render_strip_discard(g_game_render_fill_use_ref ? game_render_fill_fs_ref_glsl : game_render_fill_fs_glsl,
                                          opaque_src, sizeof opaque_src), 1);
            d.label = "game-render-fill-shader-opaque";
            g_game_render.fill_shader_opaque = sg_make_shader(&d);
        }
        if (g_game_render_fill_verify && (backend == SG_BACKEND_GLCORE || backend == SG_BACKEND_GLES3)) {
            d.vertex_func.source   = game_render_glsl(backend, game_render_fill_vs_glsl, 0);
            d.fragment_func.source = game_render_glsl(backend, game_render_fill_fs_ref_glsl, 1);
            d.label = "game-render-fill-shader-ref";
            g_game_render.fill_shader_ref = sg_make_shader(&d);
        }
    }
    {
        sg_pipeline_desc p;
        memset(&p, 0, sizeof(p));
        p.shader                   = g_game_render.fill_shader;
        p.primitive_type           = SG_PRIMITIVETYPE_TRIANGLES;
        p.layout.attrs[0].format   = SG_VERTEXFORMAT_FLOAT3;
        p.layout.attrs[0].offset   = offsetof(game_render_tex_vertex_t, x);
        p.layout.attrs[1].format   = SG_VERTEXFORMAT_FLOAT4;
        p.layout.attrs[1].offset   = offsetof(game_render_tex_vertex_t, r);
        p.layout.attrs[2].format   = SG_VERTEXFORMAT_FLOAT2;
        p.layout.attrs[2].offset   = offsetof(game_render_tex_vertex_t, u);
        p.layout.attrs[3].format   = SG_VERTEXFORMAT_FLOAT4;
        p.layout.attrs[3].offset   = offsetof(game_render_tex_vertex_t, tx);
        p.layout.attrs[4].format   = SG_VERTEXFORMAT_FLOAT4;
        p.layout.attrs[4].offset   = offsetof(game_render_tex_vertex_t, lb);
        p.layout.attrs[5].format   = SG_VERTEXFORMAT_FLOAT2;   /* zs, zl */
        p.layout.attrs[5].offset   = offsetof(game_render_tex_vertex_t, zs);
        p.layout.buffers[0].stride = sizeof(game_render_tex_vertex_t);
        p.depth.compare            = SG_COMPAREFUNC_LESS_EQUAL;
        p.depth.write_enabled      = true;
        p.label = "game-render-fill-pipeline";
        p.cull_mode = SG_CULLMODE_NONE;
        g_game_render.fill_pipeline = sg_make_pipeline(&p);
        p.cull_mode = SG_CULLMODE_BACK;
        p.face_winding = SG_FACEWINDING_CW;   p.label = "fill-cull-cw";
        g_game_render.fill_pipeline_cw = sg_make_pipeline(&p);
        p.face_winding = SG_FACEWINDING_CCW;  p.label = "fill-cull-ccw";
        g_game_render.fill_pipeline_ccw = sg_make_pipeline(&p);
        if (g_game_render.fill_shader_opaque.id) {
            p.shader = g_game_render.fill_shader_opaque;
            p.cull_mode = SG_CULLMODE_NONE;                                      p.label = "fill-opaque";
            g_game_render.fill_opaque[0] = sg_make_pipeline(&p);
            p.cull_mode = SG_CULLMODE_BACK; p.face_winding = SG_FACEWINDING_CW;  p.label = "fill-opaque-cw";
            g_game_render.fill_opaque[1] = sg_make_pipeline(&p);
            p.face_winding = SG_FACEWINDING_CCW;                                 p.label = "fill-opaque-ccw";
            g_game_render.fill_opaque[2] = sg_make_pipeline(&p);
        }
        if (g_game_render.fill_shader_ref.id) {
            p.shader = g_game_render.fill_shader_ref;
            p.cull_mode = SG_CULLMODE_NONE;                                      p.label = "fill-ref";
            g_game_render.fill_ref[0] = sg_make_pipeline(&p);
            p.cull_mode = SG_CULLMODE_BACK; p.face_winding = SG_FACEWINDING_CW;  p.label = "fill-ref-cw";
            g_game_render.fill_ref[1] = sg_make_pipeline(&p);
            p.face_winding = SG_FACEWINDING_CCW;                                 p.label = "fill-ref-ccw";
            g_game_render.fill_ref[2] = sg_make_pipeline(&p);
        }
    }

    /* ---- Texture luma atlas ---------------------------------------------- */
    g_game_render.atlas_image = sg_make_image(&(sg_image_desc){
        .width        = GEO3D_ATLAS_W,
        .height       = GEO3D_ATLAS_H,
        .pixel_format = SG_PIXELFORMAT_R8,
        .usage        = { .stream_update = true },
        .label        = "geo3d-atlas",
    });
    g_game_render.atlas_view = sg_make_view(&(sg_view_desc){
        .texture.image = g_game_render.atlas_image, .label = "geo3d-atlas-view" });
    g_game_render.atlas_sampler = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_NEAREST, .mag_filter = SG_FILTER_NEAREST,
        .wrap_u = SG_WRAP_REPEAT, .wrap_v = SG_WRAP_REPEAT,
        .label = "geo3d-atlas-sampler",
    });

    /* Luma-ramp LUTs: lumaram (0x20000 = 256×512) + colorxlat (0xC000 = 256×192). */
    g_game_render.luma_image = sg_make_image(&(sg_image_desc){
        .width = 256, .height = (int)(LUMA_SIZE / 256), .pixel_format = SG_PIXELFORMAT_R8,
        .usage = { .stream_update = true }, .label = "geo3d-lumaram" });
    g_game_render.luma_view = sg_make_view(&(sg_view_desc){
        .texture.image = g_game_render.luma_image, .label = "geo3d-lumaram-view" });
    g_game_render.cxlat_image = sg_make_image(&(sg_image_desc){
        .width = 256, .height = (int)(COLORXLAT_SIZE / 256), .pixel_format = SG_PIXELFORMAT_R8,
        .usage = { .stream_update = true }, .label = "geo3d-colorxlat" });
    g_game_render.cxlat_view = sg_make_view(&(sg_view_desc){
        .texture.image = g_game_render.cxlat_image, .label = "geo3d-colorxlat-view" });
    g_game_render.lut_sampler = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_NEAREST, .mag_filter = SG_FILTER_NEAREST,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE, .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
        .label = "geo3d-lut-sampler" });

    /* Colour ramps: the image exists on every backend (the fill shaders all
     * declare it); rows are only handed out where the GL fill shader reads them. */
    g_game_render.ramp_image = sg_make_image(&(sg_image_desc){
        .width = GAME_RENDER_RAMP_W, .height = GAME_RENDER_RAMP_ROWS, .pixel_format = SG_PIXELFORMAT_RGBA8,
        .usage = { .dynamic_update = true }, .label = "geo3d-colour-ramps" });
    g_game_render.ramp_view = sg_make_view(&(sg_view_desc){
        .texture.image = g_game_render.ramp_image, .label = "geo3d-colour-ramps-view" });
    g_game_render.ramp_enabled = g_game_render_fill_ramp && (backend == SG_BACKEND_GLCORE || backend == SG_BACKEND_GLES3);
    g_game_render.shade_enabled = g_game_render_fill_shade && g_game_render.ramp_enabled;
    memset(g_shade_slot_row, 0, sizeof g_shade_slot_row);
    memset(g_shade_is_row, 0, sizeof g_shade_is_row);
    g_game_render.ramp_count = 0;
    memset(g_ramp_row_of, 0, sizeof g_ramp_row_of);
    sg_add_commit_listener((sg_commit_listener){ .func = game_render__ramp_commit });

    g_game_render.initialized = true;
    LOG_INFO("game_render_init: complete");
}

static inline void game_render_shutdown(void) {
    if (!g_game_render.initialized) return;
    sg_destroy_sampler(g_game_render.atlas_sampler);
    sg_destroy_view(g_game_render.atlas_view);
    sg_destroy_image(g_game_render.atlas_image);
    sg_destroy_sampler(g_game_render.lut_sampler);
    sg_destroy_view(g_game_render.luma_view);
    sg_destroy_image(g_game_render.luma_image);
    sg_destroy_view(g_game_render.cxlat_view);
    sg_destroy_image(g_game_render.cxlat_image);
    sg_destroy_view(g_game_render.ramp_view);
    sg_destroy_image(g_game_render.ramp_image);
    sg_remove_commit_listener((sg_commit_listener){ .func = game_render__ramp_commit });
    sg_destroy_pipeline(g_game_render.fill_pipeline);
    sg_destroy_pipeline(g_game_render.fill_pipeline_cw);
    sg_destroy_pipeline(g_game_render.fill_pipeline_ccw);
    for (int i = 0; i < 3; i++) sg_destroy_pipeline(g_game_render.fill_ref[i]);   /* invalid ids are ignored */
    sg_destroy_shader(g_game_render.fill_shader_ref);
    for (int i = 0; i < 3; i++) sg_destroy_pipeline(g_game_render.fill_opaque[i]);
    sg_destroy_shader(g_game_render.fill_shader_opaque);
    sg_destroy_shader(g_game_render.fill_shader);
    sg_destroy_buffer(g_game_render.fill_vbuf);
    sg_destroy_pipeline(g_game_render.overlay_pipeline);
    sg_destroy_shader(g_game_render.overlay_shader);
    sg_destroy_pipeline(g_game_render.line_pipeline);
    sg_destroy_shader(g_game_render.line_shader);
    sg_destroy_buffer(g_game_render.line_vbuf);
    sg_destroy_sampler(g_game_render.target_sampler_linear);
    sg_destroy_pipeline(g_game_render.target_pipeline);
    sg_destroy_buffer(g_game_render.target_vbuf);
    sg_destroy_sampler(g_game_render.tile_sampler);
    sg_destroy_pipeline(g_game_render.tile_pipeline);
    sg_destroy_shader(g_game_render.tile_shader);
    sg_destroy_pipeline(g_game_render.indexed_pipeline);   /* invalid ids are ignored */
    sg_destroy_pipeline(g_game_render.indexed_opaque);
    sg_destroy_shader(g_game_render.indexed_shader);
    sg_destroy_buffer(g_game_render.quad_vbuf);
    g_game_render.initialized = false;
}

/* ---- mat4 helpers -------------------------------------------------------- */

static inline void gm_mat4_identity(float *m) {
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static inline void gm_mat4_mul(float *out, const float *a, const float *b) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += a[i*4 + k] * b[k*4 + j];
            out[i*4 + j] = s;
        }
}

static inline void gm_mat4_transpose(float *out, const float *m) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i*4 + j] = m[j*4 + i];
}

static inline void gm_mat4_perspective(float *m, float fov_rad, float aspect,
                                        float znear, float zfar) {
    float f = 1.0f / tanf(fov_rad * 0.5f);
    memset(m, 0, 64);
    m[0]  = f / aspect;
    m[5]  = f;
    m[10] = (zfar + znear) / (znear - zfar);
    m[11] = (2.0f * zfar * znear) / (znear - zfar);
    m[14] = -1.0f;
}

/* View = Rx(rx) * Ry(ry) * T(-cam) — right-handed for the host renderer. */
static inline void gm_mat4_view(float *m, float cx, float cy, float cz,
                                 float rot_y, float rot_x) {
    float cyy = cosf(rot_y), syy = sinf(rot_y);
    float cp  = cosf(rot_x), sp  = sinf(rot_x);
    m[0]  =  cyy;
    m[1]  =  0.0f;
    m[2]  = -syy;
    m[3]  = -(cyy*cx - syy*cz);
    m[4]  = -sp*syy;
    m[5]  =  cp;
    m[6]  = -sp*cyy;
    m[7]  = -cp*cy + sp*(syy*cx + cyy*cz);
    m[8]  =  cp*syy;
    m[9]  =  sp;
    m[10] =  cp*cyy;
    m[11] = -sp*cy - cp*(syy*cx + cyy*cz);
    m[12] = 0; m[13] = 0; m[14] = 0; m[15] = 1.0f;

    /* Rotation-only: drop the (−eye) translation for geometry that already has
     * the eye baked in (intro carnival flythrough etc.). */
    if (g_cam_rot_only) { m[3] = 0.0f; m[7] = 0.0f; m[11] = 0.0f; }
}

/* ---- Texture atlas upload ------------------------------------------------ */

/* The decoded atlas as last uploaded: 2048×2048 R8, sheet 0 over sheet 1. */
static uint8_t g_game_render_atlas_px[GEO3D_ATLAS_W * GEO3D_ATLAS_H];

/*
 * Decode the 4-bit luma texture sheet (texram0) into the R8 atlas and upload.
 * Logical layout 2048×1024 (per MAME model2rd.ipp get_texel): the sheet is
 * stored 1024×2048, so x>=1024 wraps to the other half via y^=1024.  Each
 * 32-bit word holds a 2×2 block of nibbles selected by (x&1,y&1).
 * Call once per frame (cheap; 2M texels) before drawing fills.
 *
 * Incremental: dirty0/dirty1 hold one flag per KB of each bank (memory.h
 * dirty_kb; NULL decodes that bank whole). A KB is one row of 256 words, and
 * word row q feeds exactly two atlas rows of the sheet, 2(q & 511) and the one
 * below, over x 0..1023 for q < 512 and x 1024..2047 above. Only flagged rows
 * are decoded again, into an atlas that persists between calls, so a texture
 * load that touches a few KB a frame no longer re-decodes all 4 million texels.
 * Each flag is cleared before its KB is read, so a write landing meanwhile
 * flags it again for the next call.
 */
static inline void game_render_upload_atlas(const uint8_t *texram0,
                                            const uint8_t *texram1,
                                            size_t sheet_size,
                                            volatile uint8_t *dirty0,
                                            volatile uint8_t *dirty1) {
    if (!g_game_render.initialized || !texram0) return;
    /* Skip the multi-megatexel decode while both texture banks are empty (early
     * boot, before the i960 uploads textures) — fills fall back to flat color.
     * The flags stay set, so the first decode covers everything written. */
    {
        size_t probe = 0;
        for (size_t k = 0; k < sheet_size && probe < 16; k += 0x1000) probe += texram0[k] ? 1 : 0;
        if (texram1)
            for (size_t k = 0; k < sheet_size && probe < 16; k += 0x1000) probe += texram1[k] ? 1 : 0;
        if (probe == 0) return;
    }
    uint8_t *atlas = g_game_render_atlas_px;
    const uint32_t *sheets[2] = { (const uint32_t *)texram0, (const uint32_t *)texram1 };
    volatile uint8_t *dirty[2] = { dirty0, dirty1 };
    size_t nwords = sheet_size / 4;
    uint32_t rows = (uint32_t)(sheet_size >> 10);          /* word rows (KB) per bank */
    bool any = false;
    for (int s = 0; s < 2; s++) {
        const uint32_t *sheet = sheets[s];
        if (!sheet) continue;
        for (uint32_t q = 0; q < rows && q < 1024u; q++) {
            if (dirty[s]) {
                if (!dirty[s][q]) continue;
                dirty[s][q] = 0;
            }
            any = true;
            int y0 = (int)((q & 511u) * 2u);
            int x0 = q < 512u ? 0 : 1024;
            for (int y = y0; y < y0 + 2; y++) {
                for (int x = x0; x < x0 + 1024; x++) {
                    int x2 = x, y2 = y;
                    if (x2 >= 1024) { x2 -= 1024; y2 ^= 1024; }
                    uint32_t off = ((uint32_t)(y2 / 2) * 512u) + (uint32_t)(x2 / 2);
                    uint32_t word = ((off >> 1) < nwords) ? sheet[off >> 1] : 0;
                    if (off & 1) word >>= 16;
                    if ((y & 1) == 0) word >>= 8;
                    if ((x & 1) == 0) word >>= 4;
                    atlas[(s * GEO3D_SHEET_H + y) * GEO3D_ATLAS_W + x] =
                        (uint8_t)((word & 0xf) * 17u);          /* 0..15 → 0..255 */
                }
            }
        }
    }
    if (!any) return;
    sg_update_image(g_game_render.atlas_image, &(sg_image_data){
        .mip_levels[0] = { .ptr = atlas, .size = sizeof g_game_render_atlas_px },
    });
}

/* ---- Colour ramps ---------------------------------------------------------- */

/* Row r's 64 colours from the colorxlat snapshot: the fill shader's ramp step,
 * colorxlat[ch][(c5 << 8) + li] then max(c - 64, 0) * 255 / 191, as the 8-bit
 * value the target stores. (c - 64) * 255 / 191 is never k + 0.5, so rounding
 * it here gives the byte the shader's float result is written as. */
/* The screen colour of face colour c5 at luma index li, as the shader's float
 * result is written: colorxlat then max(c - 64, 0) * 255 / 191. */
static inline void game_render__shade_px(const int c5[3], int li, uint8_t *px) {
    for (int ch = 0; ch < 3; ch++) {
        int v = g_game_render.cxlat_snap[ch * 0x4000 + ((c5[ch] << 8) + li) * 2];
        double x = v > 64 ? (double)(v - 64) * 255.0 / 191.0 : 0.0;
        px[ch] = (uint8_t)(x >= 255.0 ? 255 : (int)(x + 0.5));
    }
    px[3] = 255;
}

/* A shade row: the colour of every texel step 0..120, the way the fill shader
 * works it out — lumaram[2 * (band + step)], times poly_luma over 256 and capped
 * at 63, then that colour's ramp entry. */
static inline void game_render__shade_fill(int r) {
    const game_render_shade_key_t *k = &g_shade_key[r];
    int c5[3] = { (k->col >> 10) & 31, (k->col >> 5) & 31, k->col & 31 };
    for (int ai = 0; ai <= 120; ai++) {
        uint32_t lbyte = 2u * (k->lb + (uint32_t)ai);
        int lram = lbyte < LUMA_SIZE ? g_game_render.luma_snap[lbyte] : 0;
        int li = (lram * (int)k->poly) >> 8;
        if (li > 63) li = 63;
        game_render__shade_px(c5, li, &g_ramp_px[(r * GAME_RENDER_RAMP_W + ai) * 4]);
    }
}

static inline void game_render__ramp_fill(int r) {
    if (g_shade_is_row[r]) { game_render__shade_fill(r); return; }
    uint16_t key = g_ramp_key[r];
    int c5[3] = { (key >> 10) & 31, (key >> 5) & 31, key & 31 };
    for (int li = 0; li < 64; li++)
        game_render__shade_px(c5, li, &g_ramp_px[(r * GAME_RENDER_RAMP_W + li) * 4]);
}

/* Start a batch: once the rows are mostly used and none has gone up this frame,
 * forget them all, so rows follow what is on screen. */
static inline void game_render__ramp_begin(void) {
    if (!g_game_render.ramp_enabled || g_ramp_uploaded) return;
    if (g_game_render.ramp_count <= GAME_RENDER_RAMP_ROWS * 3 / 4) return;
    for (int r = 0; r < g_game_render.ramp_count; r++)
        if (!g_shade_is_row[r]) g_ramp_row_of[g_ramp_key[r]] = 0;
    memset(g_shade_slot_row, 0, sizeof g_shade_slot_row);
    memset(g_shade_is_row, 0, sizeof g_shade_is_row);
    g_game_render.ramp_count = 0;
}

/* The shade row for a face's (luma band, poly_luma, colour), made if new; -1
 * when the face keeps the shader's lumaram + ramp lookups. */
static inline int game_render__shade_row(float lb, float pl, float r, float g, float b) {
    if (!g_game_render.shade_enabled || !g_game_render.cxlat_valid || !g_game_render.luma_valid) return -1;
    if (!(lb >= 0.0f)) return -1;
    int r5 = (int)(r * 31.0f + 0.5f), g5 = (int)(g * 31.0f + 0.5f), b5 = (int)(b * 31.0f + 0.5f);
    if ((unsigned)r5 > 31u || (unsigned)g5 > 31u || (unsigned)b5 > 31u) return -1;
    float plc = pl < 0.0f ? 0.0f : (pl > 1.0f ? 1.0f : pl);
    game_render_shade_key_t k = { (uint32_t)lb, (uint16_t)(plc * 255.0f + 0.5f),
                                  (uint16_t)((r5 << 10) | (g5 << 5) | b5) };
    uint32_t h = (k.lb * 2654435761u) ^ ((uint32_t)k.poly * 40503u) ^ ((uint32_t)k.col * 2246822519u);
    for (uint32_t probe = 0; probe < 64u; probe++) {
        uint32_t s = (h + probe) & (GAME_RENDER_SHADE_SLOTS - 1u);
        uint16_t held = g_shade_slot_row[s];
        if (held) {
            const game_render_shade_key_t *e = &g_shade_key[held - 1];
            if (e->lb == k.lb && e->poly == k.poly && e->col == k.col) return held - 1;
            continue;
        }
        if (g_ramp_uploaded || g_game_render.ramp_count == GAME_RENDER_RAMP_ROWS) return -1;
        int row = g_game_render.ramp_count++;
        g_shade_key[row]     = k;
        g_shade_is_row[row]  = 1;
        g_shade_slot_row[s]  = (uint16_t)(row + 1);
        game_render__shade_fill(row);
        g_game_render.ramp_dirty = true;
        return row;
    }
    return -1;
}

/* The ramp row for a fill colour (as the shader rounds it to 5 bits a channel),
 * made if new; -1 when the face keeps the shader's own lookup: no ramps on this
 * backend or no tables yet, a channel out of range, all rows taken, or a new
 * colour after this frame's ramps went up. */
static inline int game_render__ramp_row(float r, float g, float b) {
    if (!g_game_render.ramp_enabled || !g_game_render.cxlat_valid) return -1;
    int r5 = (int)(r * 31.0f + 0.5f), g5 = (int)(g * 31.0f + 0.5f), b5 = (int)(b * 31.0f + 0.5f);
    if ((unsigned)r5 > 31u || (unsigned)g5 > 31u || (unsigned)b5 > 31u) return -1;
    int key = (r5 << 10) | (g5 << 5) | b5;
    if (g_ramp_row_of[key]) return g_ramp_row_of[key] - 1;
    if (g_ramp_uploaded || g_game_render.ramp_count == GAME_RENDER_RAMP_ROWS) return -1;
    int row = g_game_render.ramp_count++;
    g_ramp_row_of[key] = (uint16_t)(row + 1);
    g_ramp_key[row] = (uint16_t)key;
    game_render__ramp_fill(row);
    g_game_render.ramp_dirty = true;
    return row;
}

/* Upload the ramps if any row changed, at most once a frame. */
static inline void game_render__ramp_upload(void) {
    if (!g_game_render.ramp_enabled || !g_game_render.ramp_dirty || g_ramp_uploaded) return;
    sg_update_image(g_game_render.ramp_image, &(sg_image_data){
        .mip_levels[0] = { .ptr = g_ramp_px, .size = sizeof g_ramp_px } });
    g_ramp_uploaded = true;
    g_game_render.ramp_dirty = false;
}

/* Upload the live lumaram + colorxlat tables (raw bus bytes) to the LUT
 * textures the fill shader integer-fetches for the MAME luma ramp.  Cheap
 * (~180 KB/frame); call once per frame before drawing fills. colorxlat goes up
 * from a snapshot, which the colour ramps are rebuilt from, so both views of
 * the table always agree. */
static inline void game_render_upload_luts(const uint8_t *luma, const uint8_t *colorxlat) {
    if (!g_game_render.initialized) return;
    bool rebuild = false;
    if (luma) {
        /* Shade rows are built from lumaram, so they go up from a snapshot too:
         * both views of the table always agree, as colorxlat's do. */
        if (memcmp(g_game_render.luma_snap, luma, LUMA_SIZE) != 0) {
            memcpy(g_game_render.luma_snap, luma, LUMA_SIZE);
            rebuild = true;
        }
        g_game_render.luma_valid = true;
        sg_update_image(g_game_render.luma_image, &(sg_image_data){
            .mip_levels[0] = { .ptr = g_game_render.luma_snap, .size = LUMA_SIZE } });
    }
    if (colorxlat) {
        memcpy(g_game_render.cxlat_snap, colorxlat, COLORXLAT_SIZE);
        sg_update_image(g_game_render.cxlat_image, &(sg_image_data){
            .mip_levels[0] = { .ptr = g_game_render.cxlat_snap, .size = COLORXLAT_SIZE } });
        g_game_render.cxlat_valid = true;
        rebuild = true;
    }
    if (rebuild && g_game_render.ramp_count) {
        for (int r = 0; r < g_game_render.ramp_count; r++) game_render__ramp_fill(r);
        g_game_render.ramp_dirty = true;
    }
}

/* ---- Per-frame draw ------------------------------------------------------ */

/*
 * Draw the game's tile composite as a letterboxed fullscreen quad.
 * Call inside an active swapchain pass, BEFORE simgui_render().
 *
 *   tile_view : sg_view of the BGR-converted RGBA8 framebuffer
 *   ox, oy    : letterbox top-left in framebuffer pixels
 *   w,  h     : letterbox size in framebuffer pixels
 */
static inline void game_render_draw_game(sg_view tile_view,
                                          int ox, int oy, int w, int h) {
    if (!g_game_render.initialized) return;
    if (w <= 0 || h <= 0) return;

    sg_apply_viewport(ox, oy, w, h, true);

    sg_apply_pipeline(g_game_render.tile_pipeline);
    sg_apply_bindings(&(sg_bindings){
        .vertex_buffers[0] = g_game_render.quad_vbuf,
        .views[0]          = tile_view,
        .samplers[0]       = g_game_render.tile_sampler,
    });
    sg_draw(0, 6, 1);
}

/* game_render_draw_game for a layer of pens (the GPU tile compositor's targets):
 * each pixel's colour is pal_view's texel for its pen. GL backends only. */
static inline void game_render_draw_indexed(sg_view pen_view, sg_view pal_view, bool opaque,
                                             int ox, int oy, int w, int h) {
    if (!g_game_render.initialized) return;
    if (w <= 0 || h <= 0) return;

    sg_apply_viewport(ox, oy, w, h, true);
    sg_apply_pipeline(opaque ? g_game_render.indexed_opaque : g_game_render.indexed_pipeline);
    sg_apply_bindings(&(sg_bindings){
        .vertex_buffers[0] = g_game_render.quad_vbuf,
        .views[0]          = pen_view,
        .views[1]          = pal_view,
        .samplers[0]       = g_game_render.tile_sampler,
    });
    sg_draw(0, 6, 1);
}

/*
 * Draw a finished render target (a frame drawn offscreen at the board's own
 * resolution) into the current pass's viewport (ox, oy, w, h), opaque, with
 * nearest or bilinear filtering.
 */
static inline void game_render_draw_target(sg_view target_view, bool linear,
                                            int ox, int oy, int w, int h) {
    if (!g_game_render.initialized) return;
    if (w <= 0 || h <= 0) return;

    sg_apply_viewport(ox, oy, w, h, true);
    sg_apply_pipeline(g_game_render.target_pipeline);
    sg_apply_bindings(&(sg_bindings){
        .vertex_buffers[0] = g_game_render.target_vbuf,
        .views[0]          = target_view,
        .samplers[0]       = linear ? g_game_render.target_sampler_linear : g_game_render.tile_sampler,
    });
    sg_draw(0, 6, 1);
}

/*
 * Composite one overlay layer (ui/overlay_plugin.h) over what the pass has
 * already drawn. The view is an ordinary RGBA8 texture holding the plugin's
 * premultiplied BGRA bytes; the shader swaps the channels and the blend state
 * is premultiplied source-over, so a layer that is transparent everywhere
 * costs a quad and changes nothing.
 *
 * ox/oy/w/h is the layer's own rect in the composed frame, not a letterbox:
 * the image is 1:1 with it, so the nearest sampler never interpolates.
 */
static inline void game_render_draw_overlay(sg_view layer_view,
                                             int ox, int oy, int w, int h) {
    if (!g_game_render.initialized) return;
    if (w <= 0 || h <= 0) return;

    sg_apply_viewport(ox, oy, w, h, true);
    /* ...and the scissor with it. The 3D draw leaves the board's clip window
     * set (game_render_set_clip, and the per-run windows in the fill path), so
     * a layer that only set the viewport would be drawn into the columns and
     * then scissored away by the rect the last polygon happened to leave
     * behind. That is a black column and no error anywhere. */
    sg_apply_scissor_rect(ox, oy, w, h, true);
    sg_apply_pipeline(g_game_render.overlay_pipeline);
    sg_apply_bindings(&(sg_bindings){
        .vertex_buffers[0] = g_game_render.quad_vbuf,
        .views[0]          = layer_view,
        .samplers[0]       = g_game_render.tile_sampler,
    });
    sg_draw(0, 6, 1);
}

static inline void game_render_submit_lines(const game_render_vs_params_t *vs, int first, int vcount);
static inline void game_render_submit_fills(const game_render_vs_params_t *vs, int first, int vcount);
static inline void game_render_submit_fills_as(const game_render_vs_params_t *vs, int first, int vcount, int can_discard);

/*
 * Draw the 3D wireframe lines currently in g_geo3d_lines over the tile quad.
 * Call inside the same swapchain pass, after game_render_draw_game(), so the
 * viewport is already set for letterboxing.
 *
 *   cam_x/y/z, rot_y/x, fov_deg  — projection/view parameters
 */
static inline void game_render_draw_lines(float cam_x, float cam_y, float cam_z,
                                           float rot_y, float rot_x, float fov_deg,
                                           float aspect_override) {
    if (!g_game_render.initialized) return;
    if (!g_geo_wireframe || g_geo3d_lines.count <= 0) return;

    /* Pack the line buffer into the GPU vertex format. */
    int n = g_geo3d_lines.count;
    if (n > GEO3D_MAX_LINES) n = GEO3D_MAX_LINES;
    for (int i = 0; i < n; i++) {
        const geo3d_line_t *L = &g_geo3d_lines.lines[i];
        game_render_line_vertex_t *v = &g_game_render.line_verts[i * 2];
        v[0].x = L->x0; v[0].y = L->y0; v[0].z = L->z0;
        v[0].r = L->r;  v[0].g = L->g;  v[0].b = L->b;  v[0].a = 1.0f;
        v[1].x = L->x1; v[1].y = L->y1; v[1].z = L->z1;
        v[1].r = L->r;  v[1].g = L->g;  v[1].b = L->b;  v[1].a = 1.0f;
    }
    int vcount = n * 2;
    sg_update_buffer(g_game_render.line_vbuf, &(sg_range){
        .ptr  = g_game_render.line_verts,
        .size = (size_t)vcount * sizeof(game_render_line_vertex_t),
    });

    /* Build MVP — perspective × view, transposed for column-major GLSL. */
    float proj[16], view[16], mvp[16], mvp_t[16];
    float aspect  = (aspect_override > 0.0f) ? aspect_override
                                              : (float)VIDEO_WIDTH / (float)VIDEO_HEIGHT;
    float fov_rad = fov_deg * 3.14159265f / 180.0f;
    gm_mat4_perspective(proj, fov_rad, aspect, 0.1f, 5000.0f);
    gm_mat4_view(view, cam_x, cam_y, cam_z, rot_y, rot_x);
    gm_mat4_mul(mvp, proj, view);
    gm_mat4_transpose(mvp_t, mvp);

    game_render_vs_params_t vs_params;
    memcpy(vs_params.mvp, mvp_t, sizeof(mvp_t));
    game_render_submit_lines(&vs_params, 0, vcount);
}

static inline void game_render_submit_lines(const game_render_vs_params_t *vs, int first, int vcount) {
    const game_render_vs_params_t vs_params = *vs;
    sg_apply_pipeline(g_game_render.line_pipeline);
    sg_apply_bindings(&(sg_bindings){
        .vertex_buffers[0] = g_game_render.line_vbuf,
    });
    sg_apply_uniforms(0, &(sg_range){ .ptr = &vs_params, .size = sizeof(vs_params) });
    sg_draw(first, vcount, 1);
}

/*
 * Draw the solid-fill triangle buffer (g_geo3d_tris) over the tile quad.
 * Call BEFORE game_render_draw_lines so wireframes appear on top.
 * Same MVP parameters as draw_lines.
 */
static inline void game_render_draw_fills(float cam_x, float cam_y, float cam_z,
                                           float rot_y, float rot_x, float fov_deg,
                                           float aspect_override) {
    if (!g_game_render.initialized) return;
    if (g_geo3d_tris.count <= 0) return;

    int n = g_geo3d_tris.count;
    if (n > GEO3D_MAX_TRIS) n = GEO3D_MAX_TRIS;
    for (int i = 0; i < n; i++) {
        const geo3d_tri_t *T = &g_geo3d_tris.tris[i];
        game_render_tex_vertex_t *v = &g_game_render.fill_verts[i * 3];
        v[0].x=T->x0; v[0].y=T->y0; v[0].z=T->z0; v[0].r=T->r; v[0].g=T->g; v[0].b=T->b; v[0].a=1.f; v[0].u=T->u0; v[0].v=T->v0;
        v[1].x=T->x1; v[1].y=T->y1; v[1].z=T->z1; v[1].r=T->r; v[1].g=T->g; v[1].b=T->b; v[1].a=1.f; v[1].u=T->u1; v[1].v=T->v1;
        v[2].x=T->x2; v[2].y=T->y2; v[2].z=T->z2; v[2].r=T->r; v[2].g=T->g; v[2].b=T->b; v[2].a=1.f; v[2].u=T->u2; v[2].v=T->v2;
        float lb = g_luma_ramp ? T->lb : -1.0f;   /* -1 → old flat_color×luma path */
        for (int _k = 0; _k < 3; _k++) {
            v[_k].tx=T->tx; v[_k].ty=T->ty; v[_k].tw=T->tw; v[_k].th=T->th;
            v[_k].lb=lb;    v[_k].pl=T->pl; v[_k].fl=T->fl; v[_k].texlod=T->texlod;
            v[_k].zs = _k == 0 ? T->zs0 : (_k == 1 ? T->zs1 : T->zs2);
            v[_k].zl = T->zl;
        }
    }
    int vcount = n * 3;
    sg_update_buffer(g_game_render.fill_vbuf, &(sg_range){
        .ptr  = g_game_render.fill_verts,
        .size = (size_t)vcount * sizeof(game_render_tex_vertex_t),
    });

    float proj[16], view[16], mvp[16], mvp_t[16];
    float aspect  = (aspect_override > 0.0f) ? aspect_override
                                              : (float)VIDEO_WIDTH / (float)VIDEO_HEIGHT;
    float fov_rad = fov_deg * 3.14159265f / 180.0f;
    gm_mat4_perspective(proj, fov_rad, aspect, 0.1f, 5000.0f);
    gm_mat4_view(view, cam_x, cam_y, cam_z, rot_y, rot_x);
    gm_mat4_mul(mvp, proj, view);
    gm_mat4_transpose(mvp_t, mvp);

    game_render_vs_params_t vs_params;
    memcpy(vs_params.mvp, mvp_t, sizeof(mvp_t));
    game_render_submit_fills(&vs_params, 0, vcount);
}

/* The fill pipeline for a cull setting (g_backface_cull 0/1/2) and whether the
 * faces drawn can discard (0: none is transparent or checkered). */
static inline sg_pipeline game_render_fill_pip(int cull, int can_discard) {
    int v = cull == 1 ? 1 : cull == 2 ? 2 : 0;
    if (!can_discard && g_game_render_fill_split && g_game_render.fill_opaque[v].id) return g_game_render.fill_opaque[v];
    return v == 1 ? g_game_render.fill_pipeline_cw : v == 2 ? g_game_render.fill_pipeline_ccw : g_game_render.fill_pipeline;
}

static inline void game_render_submit_fills(const game_render_vs_params_t *vs, int first, int vcount) {
    game_render_submit_fills_as(vs, first, vcount, 1);
}

static inline void game_render_submit_fills_as(const game_render_vs_params_t *vs, int first, int vcount, int can_discard) {
    const game_render_vs_params_t vs_params = *vs;
    sg_apply_pipeline(game_render_fill_pip(g_backface_cull, can_discard));
    sg_apply_bindings(&(sg_bindings){
        .vertex_buffers[0] = g_game_render.fill_vbuf,
        .views[0]          = g_game_render.atlas_view,
        .views[1]          = g_game_render.luma_view,
        .views[2]          = g_game_render.cxlat_view,
        .views[3]          = g_game_render.ramp_view,
        .samplers[0]       = g_game_render.atlas_sampler,
    });
    sg_apply_uniforms(0, &(sg_range){ .ptr = &vs_params, .size = sizeof(vs_params) });
    sg_draw(first, vcount, 1);
}

/* Replay this frame's layer behind the 3D and its logged fill draws into the
 * current pass, the reference way (ref: back colour quad, blended pen layer,
 * the reference fill shader for every face) or the way the frame draws them
 * (opaque pen layer, fills split by whether they can discard). Vertex buffer and
 * textures are as the frame left them: nothing is uploaded. back_view: the back
 * colour; bg_view / pal_view: the GPU compositor's back layer and pens. */
static inline void game_render_replay_fills(bool ref, sg_view back_view, sg_view bg_view, sg_view pal_view) {
    if (g_fill_log.n > 0) {
        int x = g_fill_log.d[0].vx, y = g_fill_log.d[0].vy, w = g_fill_log.d[0].vw, h = g_fill_log.d[0].vh;
        if (ref) game_render_draw_game(back_view, x, y, w, h);
        game_render_draw_indexed(bg_view, pal_view, !ref, x, y, w, h);
    }
    for (int i = 0; i < g_fill_log.n; i++) {
        const game_render_fill_draw_t *e = &g_fill_log.d[i];
        sg_apply_viewport(e->vx, e->vy, e->vw, e->vh, true);
        sg_apply_pipeline(ref ? g_game_render.fill_ref[e->cull == 1 ? 1 : e->cull == 2 ? 2 : 0]
                              : game_render_fill_pip(e->cull, e->can_discard));
        sg_apply_scissor_rect(e->sx, e->sy, e->sw, e->sh, true);
        sg_apply_bindings(&(sg_bindings){
            .vertex_buffers[0] = g_game_render.fill_vbuf,
            .views[0]          = g_game_render.atlas_view,
            .views[1]          = g_game_render.luma_view,
            .views[2]          = g_game_render.cxlat_view,
            .views[3]          = g_game_render.ramp_view,
            .samplers[0]       = g_game_render.atlas_sampler,
        });
        game_render_vs_params_t vs;
        memcpy(vs.mvp, e->mvp, sizeof vs.mvp);
        sg_apply_uniforms(0, &(sg_range){ .ptr = &vs, .size = sizeof vs });
        sg_draw(e->first, e->count, 1);
    }
}

/*
 * Batched submission for the GEO display list. Every run of objects sharing a
 * projection and window decodes into the one tri/line buffer and records its
 * slice, matrix and scissor; the batch then uploads each vertex buffer ONCE and
 * issues the runs' draws in order. Updating a buffer between draws that still
 * reference it forces a GPU sync on some drivers (Mesa's panfrost took 60-260 ms
 * a frame doing that) and costs every driver a full buffer write per run.
 */
#define GAME_RENDER_MAX_RUNS 1024

typedef struct {
    int   tri_first, tri_count;
    int   line_first, line_count;
    float mvp_t[16];           /* column-major, as the shaders take it */
    int   sx, sy, sw, sh;      /* scissor, framebuffer pixels */
} game_render_run_t;

static struct {
    game_render_run_t runs[GAME_RENDER_MAX_RUNS];
    int               count;
} g_render_batch;

/* Upload the batch's geometry once and draw its runs in order, then empty it. */
static inline void game_render_batch_flush(bool lines_only) {
    const int nt = g_geo3d_tris.count  > GEO3D_MAX_TRIS  ? GEO3D_MAX_TRIS  : g_geo3d_tris.count;
    const int nl = g_geo3d_lines.count > GEO3D_MAX_LINES ? GEO3D_MAX_LINES : g_geo3d_lines.count;
    bool fills = !lines_only && nt > 0;
    bool lines = g_geo_wireframe && nl > 0;

    /* Per triangle: can its face discard (transparent or checkered)? Runs are
     * drawn as consecutive stretches of alike triangles, in order, so the ones
     * that cannot go through the discard-free shader. */
    static uint8_t can_discard[GEO3D_MAX_TRIS];
    if (fills) {
        game_render__ramp_begin();
        for (int i = 0; i < nt; i++) {
            const geo3d_tri_t *T = &g_geo3d_tris.tris[i];
            can_discard[i] = ((int)(T->fl + 0.5f) & (int)(GEO3D_FACE_TRANSPARENT | GEO3D_FACE_CHECKER)) != 0;
            game_render_tex_vertex_t *v = &g_game_render.fill_verts[i * 3];
            v[0].x=T->x0; v[0].y=T->y0; v[0].z=T->z0; v[0].u=T->u0; v[0].v=T->v0;
            v[1].x=T->x1; v[1].y=T->y1; v[1].z=T->z1; v[1].u=T->u1; v[1].v=T->v1;
            v[2].x=T->x2; v[2].y=T->y2; v[2].z=T->z2; v[2].u=T->u2; v[2].v=T->v2;
            float lb = g_luma_ramp ? T->lb : -1.0f;
            /* colour alpha: the face's row + 2 (1.0, as before, without one). A
             * textured face with a luma band takes a shade row where there is
             * one — the whole colour chain in a single fetch, flagged 128 — and
             * a colour ramp row otherwise. */
            float ramp = 1.0f, fl = T->fl;
            int row = -1;
            if (g_game_render.shade_enabled && T->tw > 0.0f)
                row = game_render__shade_row(lb, T->pl, T->r, T->g, T->b);
            if (row >= 0) {
                fl += 128.0f;
            } else if (g_game_render.ramp_enabled) {
                row = game_render__ramp_row(T->r, T->g, T->b);
            }
            if (row >= 0) ramp = (float)(row + 2);
            for (int k = 0; k < 3; k++) {
                v[k].r=T->r; v[k].g=T->g; v[k].b=T->b; v[k].a=ramp;
                v[k].tx=T->tx; v[k].ty=T->ty; v[k].tw=T->tw; v[k].th=T->th;
                v[k].lb=lb;    v[k].pl=T->pl; v[k].fl=fl;    v[k].texlod=T->texlod;
                v[k].zs = k == 0 ? T->zs0 : (k == 1 ? T->zs1 : T->zs2);
                v[k].zl = T->zl;
            }
        }
        sg_update_buffer(g_game_render.fill_vbuf, &(sg_range){
            .ptr = g_game_render.fill_verts, .size = (size_t)nt * 3 * sizeof(game_render_tex_vertex_t) });
        game_render__ramp_upload();
        g_fill_log.uploads++;
    }
    if (lines) {
        for (int i = 0; i < nl; i++) {
            const geo3d_line_t *L = &g_geo3d_lines.lines[i];
            game_render_line_vertex_t *v = &g_game_render.line_verts[i * 2];
            v[0].x = L->x0; v[0].y = L->y0; v[0].z = L->z0; v[0].r = L->r; v[0].g = L->g; v[0].b = L->b; v[0].a = 1.0f;
            v[1].x = L->x1; v[1].y = L->y1; v[1].z = L->z1; v[1].r = L->r; v[1].g = L->g; v[1].b = L->b; v[1].a = 1.0f;
        }
        sg_update_buffer(g_game_render.line_vbuf, &(sg_range){
            .ptr = g_game_render.line_verts, .size = (size_t)nl * 2 * sizeof(game_render_line_vertex_t) });
    }

    for (int r = 0; r < g_render_batch.count; r++) {
        const game_render_run_t *run = &g_render_batch.runs[r];
        game_render_vs_params_t vs;
        memcpy(vs.mvp, run->mvp_t, sizeof vs.mvp);
        sg_apply_scissor_rect(run->sx, run->sy, run->sw, run->sh, true);
        int tc = run->tri_first + run->tri_count > nt ? nt - run->tri_first : run->tri_count;
        bool split = g_game_render_fill_split && g_game_render.fill_opaque[0].id != 0;
        for (int s = run->tri_first, end = run->tri_first + (fills ? tc : 0); s < end; ) {
            int k = can_discard[s], e0 = s;
            if (split) while (e0 < end && can_discard[e0] == k) e0++;
            else       { e0 = end; k = 1; }
            game_render_submit_fills_as(&vs, s * 3, (e0 - s) * 3, k);
            if (g_game_render_fill_verify) {
                if (g_fill_log.n == GAME_RENDER_FILL_LOG_MAX) {
                    g_fill_log.overflow = true;
                } else {
                    game_render_fill_draw_t *e = &g_fill_log.d[g_fill_log.n++];
                    e->vx = g_fill_log.vx; e->vy = g_fill_log.vy; e->vw = g_fill_log.vw; e->vh = g_fill_log.vh;
                    e->sx = run->sx; e->sy = run->sy; e->sw = run->sw; e->sh = run->sh;
                    memcpy(e->mvp, vs.mvp, sizeof e->mvp);
                    e->cull = g_backface_cull; e->first = s * 3; e->count = (e0 - s) * 3; e->can_discard = k;
                }
            }
            s = e0;
        }
        int lc = run->line_first + run->line_count > nl ? nl - run->line_first : run->line_count;
        if (lines && lc > 0) {
            game_render_submit_lines(&vs, run->line_first * 2, lc * 2);
        }
    }
    g_render_batch.count = 0;
    geo3d_tris_reset();
    geo3d_lines_reset();
}

/*
 * The board's projection for eye-space geometry from the GEO display list
 * (geo3d_scan_geo_list): screen x = cx + fx*x/z, y = cy - fy*y/z over the
 * 496x384 screen, host eye space being the board's with z negated. Depth is an
 * ordinary perspective range squeezed into a slice per window, the last window
 * nearest: the board's rasterizer fills each pixel once, walking the windows
 * last to first and each window's polygons nearest first (model2_v.cpp
 * model2_3d_frame_end, the fillmap test in model2rd.ipp), so a later window
 * covers an earlier one while depth still sorts inside each window. Row-major. The slice is worked in a [0, 1] depth range and
 * stretched to [-1, 1] on GL: D3D11 clips clip-space z to [0, w], so a slice
 * placed below zero there is not drawn at all.
 */
static inline void gm_mat4_geo_projection(float *m, const float *gproj, int win, int windows) {
    const float W = (float)VIDEO_WIDTH, H = (float)VIDEO_HEIGHT;
    const float n = 0.05f, f = 20000.0f;
    float nwin = (float)(windows > 0 ? windows : 1);
    memset(m, 0, 64);
    m[0]  = 2.0f * gproj[0] / W;
    m[2]  = -(2.0f * gproj[2] / W - 1.0f);
    m[5]  = 2.0f * gproj[1] / H;
    m[6]  = -(1.0f - 2.0f * gproj[3] / H);
    m[14] = -1.0f;
    /* depth in [0, 1]: z = (f/(n-f))*zh + f*n/(n-f), w = -zh; then z/N + (win/N)*w */
    float slice = (float)((windows > 0 ? windows : 1) - 1 - win);
    float z10 = f / ((n - f) * nwin) - slice / nwin;
    float z11 = f * n / ((n - f) * nwin);
    sg_backend backend = sg_query_backend();
    if (backend == SG_BACKEND_GLCORE || backend == SG_BACKEND_GLES3) {
        m[10] = 2.0f * z10 + 1.0f;      /* 2*z - w, w's z term being -1 */
        m[11] = 2.0f * z11;
    } else {
        m[10] = z10;
        m[11] = z11;
    }
}

/*
 * Draw the frame's GEO display list: runs of objects that share a projection
 * and window are decoded together and drawn with that window's scissor.
 */
static inline void game_render_draw_geo_list(geo3d_state_t *geo,
                                              const uint8_t *main_data, size_t main_data_size,
                                              const uint8_t *polygons,  size_t polygons_size,
                                              const uint8_t *materials, size_t materials_size,
                                              uint32_t table_off, uint32_t table_count,
                                              uint32_t mesh_ptr_subtract, uint32_t mesh_ptr_add,
                                              int ox, int oy, int w, int h) {
    if (g_geo3d_dump_busy) return;
    const int count = geo->captured_count;
    float saved_light[3] = { g_light_dir[0], g_light_dir[1], g_light_dir[2] };
    sg_apply_viewport(ox, oy, w, h, true);
    g_fill_log.vx = ox; g_fill_log.vy = oy; g_fill_log.vw = w; g_fill_log.vh = h;
    g_render_batch.count = 0;
    geo3d_lines_reset();
    geo3d_tris_reset();
    for (int i = 0; i < count; ) {
        const captured_model_t *c0 = &geo->captured[i];
        int j = i;
        while (j < count) {
            const captured_model_t *cm = &geo->captured[j];
            if (cm->window != c0->window || memcmp(cm->gproj, c0->gproj, sizeof cm->gproj) != 0
                    || memcmp(cm->vp, c0->vp, sizeof cm->vp) != 0)
                break;
            j++;
        }
        int x0 = c0->vp[0] < 0 ? 0 : c0->vp[0], y0 = c0->vp[1] < 0 ? 0 : c0->vp[1];
        int x1 = c0->vp[2] > VIDEO_WIDTH ? VIDEO_WIDTH : c0->vp[2];
        int y1 = c0->vp[3] > VIDEO_HEIGHT ? VIDEO_HEIGHT : c0->vp[3];
        if (!(x1 > x0 && y1 > y0)) { i = j; continue; }   /* off-screen window: nothing drawn */

        if (g_render_batch.count == GAME_RENDER_MAX_RUNS) game_render_batch_flush(geo->lines_only);
        for (int attempt = 0; ; attempt++) {
            const int tri_first = g_geo3d_tris.count, line_first = g_geo3d_lines.count;
            for (int k = i; k < j; k++) {
                const captured_model_t *cm = &geo->captured[k];
                if (geo->isolate_index >= 0 && k != geo->isolate_index) continue;
                if (geo->filter_enabled && (k < geo->filter_min || k > geo->filter_max)) continue;
                g_light_dir[0] = cm->light[0]; g_light_dir[1] = cm->light[1]; g_light_dir[2] = cm->light[2];
                g_geo3d_obj_tpa = cm->tpa;
                g_geo3d_obj_tha = cm->tha;
                g_geo3d_board_luma = 1;
                g_geo3d_mode = cm->geo_mode;
                g_geo3d_zadjust = cm->zadjust;
                g_geo3d_lod  = cm->geo_lod;
                if (cm->model_idx < 0) {        /* polygon RAM: the mesh sits at the object address */
                    uint32_t word = cm->dbg_mesh_ptr & 0x7FFFu;
                    g_geo3d_obj_mesh      = (const uint8_t *)&g_geo_polyram[(cm->dbg_mesh_ptr & 0x01000000u) ? 1 : 0][word];
                    g_geo3d_obj_mesh_size = (0x8000u - word) * 4u;
                }
                geo3d_decode_model_cached(cm->model_idx, main_data, main_data_size, polygons, polygons_size,
                                          materials, materials_size, table_off, table_count,
                                          mesh_ptr_subtract, mesh_ptr_add,
                                          geo->use_matrix ? cm->matrix : NULL,
                                          cm->color[0], cm->color[1], cm->color[2]);
                g_geo3d_obj_tpa = g_geo3d_obj_tha = 0xFFFFFFFFu;
                g_geo3d_board_luma = 0;
                g_geo3d_obj_mesh = NULL;
            }
            /* The run filled the shared buffer after earlier runs: draw those and
             * decode it again into an empty buffer, where it gets the whole
             * capacity — exactly what drawing each run on its own gave it. */
            bool full = g_geo3d_tris.count >= GEO3D_MAX_TRIS || g_geo3d_lines.count >= GEO3D_MAX_LINES;
            if (full && attempt == 0 && (tri_first > 0 || line_first > 0)) {
                g_geo3d_tris.count  = tri_first;
                g_geo3d_lines.count = line_first;
                game_render_batch_flush(geo->lines_only);
                continue;
            }
            game_render_run_t *run = &g_render_batch.runs[g_render_batch.count++];
            run->tri_first  = tri_first;
            run->tri_count  = g_geo3d_tris.count - tri_first;
            run->line_first = line_first;
            run->line_count = g_geo3d_lines.count - line_first;
            break;
        }
        game_render_run_t *run = &g_render_batch.runs[g_render_batch.count - 1];
        run->sx = ox + x0 * w / VIDEO_WIDTH;
        run->sy = oy + y0 * h / VIDEO_HEIGHT;
        run->sw = (x1 - x0) * w / VIDEO_WIDTH;
        run->sh = (y1 - y0) * h / VIDEO_HEIGHT;
        float mvp[16];
        gm_mat4_geo_projection(mvp, c0->gproj, c0->window, geo->geo_windows);
        gm_mat4_transpose(run->mvp_t, mvp);
        i = j;
    }
    game_render_batch_flush(geo->lines_only);
    g_light_dir[0] = saved_light[0]; g_light_dir[1] = saved_light[1]; g_light_dir[2] = saved_light[2];
    sg_apply_scissor_rect(ox, oy, w, h, true);
}

/*
 * Draw all captured models with per-model clip windows applied as scissor rects.
 *
 * Replaces the separate geo3d_build_wireframes + game_render_draw_lines pair
 * when any model has a clip window (e.g. adv_movie_chaos portrait frames).
 * Falls back to a single-batch draw when no model has a clip window.
 *
 * ox/oy/w/h are the letterbox rect in framebuffer pixels (from game_render_letterbox).
 */
static inline void game_render_draw_captured_models(geo3d_state_t *geo,
                                                     const uint8_t *main_data, size_t main_data_size,
                                                     const uint8_t *polygons,  size_t polygons_size,
                                                     const uint8_t *materials, size_t materials_size,
                                                     uint32_t table_off, uint32_t table_count,
                                                     uint32_t mesh_ptr_subtract, uint32_t mesh_ptr_add,
                                                     int ox, int oy, int w, int h,
                                                     float cam_x, float cam_y, float cam_z,
                                                     float rot_y, float rot_x, float fov_deg,
                                                     float lerp_t) {
    if (!g_game_render.initialized) return;
    if (!geo->enabled) { geo3d_lines_reset(); return; }

    if (geo->use_captures && geo->captured_count > 0 && geo->captured[0].view_space && !geo->test_triangle) {
        game_render_draw_geo_list(geo, main_data, main_data_size, polygons, polygons_size,
                                  materials, materials_size, table_off, table_count,
                                  mesh_ptr_subtract, mesh_ptr_add, ox, oy, w, h);
        return;
    }

    /* Check whether any captured model carries a clip window. */
    bool any_clip = false;
    if (geo->use_captures) {
        for (int i = 0; i < geo->captured_count && !any_clip; i++)
            if (geo->captured[i].has_clip_win) any_clip = true;
    }

    if (!any_clip || geo->test_triangle || !geo->use_captures) {
        /* Fast path: single batch draw, no per-model scissor needed. */
        geo3d_build_wireframes(geo, main_data, main_data_size,
                               polygons, polygons_size,
                               materials, materials_size,
                               table_off, table_count,
                               mesh_ptr_subtract, mesh_ptr_add, lerp_t,
                               cam_x, cam_y, cam_z);
        if (!geo->lines_only)
            game_render_draw_fills(cam_x, cam_y, cam_z, rot_y, rot_x, fov_deg, 0.0f);
        game_render_draw_lines(cam_x, cam_y, cam_z, rot_y, rot_x, fov_deg, 0.0f);
        return;
    }

    /* Slow path: draw each model separately so we can scissor it. */
    for (int i = 0; i < geo->captured_count; i++) {
        if (geo->isolate_index >= 0 && i != geo->isolate_index) continue;
        if (geo->filter_enabled && (i < geo->filter_min || i > geo->filter_max)) continue;

        const captured_model_t *cm = &geo->captured[i];
        geo3d_lines_reset();
        geo3d_tris_reset();
        const float *mat = (geo->use_matrix && cm->has_matrix) ? cm->matrix : NULL;

        /* Interpolate translation with previous frame in the slow path too. */
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

        if (cm->has_clip_win) {
            /* Map clip rect (game pixels) to framebuffer pixels. */
            int sx = ox + (int)cm->clip_win_x * w / VIDEO_WIDTH;
            int sy = oy + (int)cm->clip_win_y * h / VIDEO_HEIGHT;
            int sw = (int)cm->clip_win_w * w / VIDEO_WIDTH;
            int sh = (int)cm->clip_win_h * h / VIDEO_HEIGHT;
            float cell_aspect = (sh > 0) ? (float)sw / (float)sh : 1.0f;
            /* Sub-window cells (character-select portraits, emeralds) are head-on
             * previews placed in VIEW-relative space — every cell's geometry sits
             * at the same ≈(0,-0.8,-4.0) in front, framed by its own cell viewport.
             * Render with an IDENTITY view (no scene camera): the scene's look-at
             * rotation (e.g. -90° Y) would swing them onto the camera plane and
             * scissor them away, which is what left the cells black. */
            sg_apply_viewport(sx, sy, sw, sh, true);
            sg_apply_scissor_rect(sx, sy, sw, sh, true);
            game_render_draw_fills(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, fov_deg, cell_aspect);
            game_render_draw_lines(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, fov_deg, cell_aspect);
        } else {
            sg_apply_viewport(ox, oy, w, h, true);
            sg_apply_scissor_rect(ox, oy, w, h, true);
            game_render_draw_fills(cam_x, cam_y, cam_z, rot_y, rot_x, fov_deg, 0.0f);
            game_render_draw_lines(cam_x, cam_y, cam_z, rot_y, rot_x, fov_deg, 0.0f);
        }
    }

    /* Restore full viewport and scissor for subsequent UI draws. */
    sg_apply_viewport(ox, oy, w, h, true);
    sg_apply_scissor_rect(ox, oy, w, h, true);
}

/*
 * Compute the letterboxed rectangle for the game inside (fb_w × fb_h).
 * Result respects VIDEO_WIDTH:VIDEO_HEIGHT aspect ratio.
 */
static inline void game_render_letterbox(int fb_w, int fb_h,
                                          int game_w, int game_h,
                                          int *ox, int *oy,
                                          int *out_w, int *out_h) {
    float game_aspect = (float)game_w / (float)game_h;
    float win_aspect  = (float)fb_w   / (float)fb_h;
    if (win_aspect > game_aspect) {
        *out_h = fb_h;
        *out_w = (int)(fb_h * game_aspect);
        *ox    = (fb_w - *out_w) / 2;
        *oy    = 0;
    } else {
        *out_w = fb_w;
        *out_h = (int)(fb_w / game_aspect);
        *ox    = 0;
        *oy    = (fb_h - *out_h) / 2;
    }
}

#endif /* GAME_RENDER_H */
