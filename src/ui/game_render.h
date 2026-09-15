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
    /* The same quad for pen layers (GL backends): the colour is looked up. */
    sg_shader   indexed_shader;
    sg_pipeline indexed_pipeline;

    /* Render-target blit: the tile shader without blending (a finished frame's
     * alpha is whatever the last layer left), a quad oriented for the backend's
     * render-target rows, and a bilinear sampler for scaling. */
    sg_buffer   target_vbuf;
    sg_pipeline target_pipeline;
    sg_sampler  target_sampler_linear;

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

    /* CPU scratch for line uploads — 2 verts per geo3d_line_t */
    game_render_line_vertex_t line_verts[GEO3D_MAX_LINES * 2];

    /* CPU scratch for fill uploads — 3 verts per geo3d_tri_t */
    game_render_tex_vertex_t  fill_verts[GEO3D_MAX_TRIS * 3];

    bool        initialized;
} game_render_t;

static game_render_t g_game_render = {0};

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
    "layout(location=4) in vec3 a_lbpl;\n"
    "out vec4 color;\n"
    "out vec2 uv;\n"
    "flat out vec4 tile;\n"
    "flat out vec3 lbpl;\n"
    "void main() {\n"
    "  mat4 mvp = mat4(vs_params[0], vs_params[1], vs_params[2], vs_params[3]);\n"
    "  gl_Position = mvp * vec4(a_pos, 1.0);\n"
    "  color = a_color; uv = a_uv; tile = a_tile; lbpl = a_lbpl;\n"
    "}\n";

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
 *             from screen-space derivatives, since the board's texlod is
 *             calibrated to 496x384 and this draws at the window's size.
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
static const char *game_render_fill_fs_glsl =
    "#version 410\n"
    "uniform sampler2D atlas_smp;\n"
    "uniform sampler2D luma_smp;\n"
    "uniform sampler2D cxlat_smp;\n"
    "in vec4 color;\n"
    "in vec2 uv;\n"
    "flat in vec4 tile;\n"
    "flat in vec3 lbpl;\n"
    "out vec4 frag_color;\n"
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
    "  ivec2 q = s % size;\n"
    "  ivec2 copy = s / size;\n"
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
    "  float lmax = max(log2(max(min(tile.z, tile.w), 2.0)) - 1.0, 0.0);\n"
    "  float lod = clamp(log2(max(length(dFdx(uv)), length(dFdy(uv)))), 0.0, lmax);\n"
    "  if (has(2) && ((int(gl_FragCoord.x) ^ int(gl_FragCoord.y)) & 1) == 0) discard;\n"
    "  vec3 rgb = color.rgb;\n"
    "  if (tile.z > 0.0) {\n"
    "    int L0 = int(floor(lod));\n"
    "    vec2 tx = mix(sample_level(L0), sample_level(L0 + 1), fract(lod));\n"
    "    if (has(1) && tx.y < 0.5) discard;\n"
    "    float al = tx.x;\n"
    "    if (lbpl.x < 0.0) {\n"
    "      if (al > 0.0) rgb = clamp(color.rgb * al * 2.0, 0.0, 1.0);\n"
    "    } else {\n"
    "      int lbyte = 2 * (int(lbpl.x) + int(al * 120.0));\n"
    "      float lram = texelFetch(luma_smp, ivec2(lbyte & 255, lbyte >> 8), 0).r * 255.0;\n"
    "      float poly = clamp(lbpl.y, 0.0, 1.0) * 255.0;\n"
    "      int li = int(min(lram * poly / 256.0, 63.0) + 0.5);\n"
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
    "    int li = min(int(clamp(lbpl.y, 0.0, 1.0) * 255.0 + 0.5) >> 2, 63);\n"
    "    int r5 = int(color.r*31.0+0.5), g5 = int(color.g*31.0+0.5), b5 = int(color.b*31.0+0.5);\n"
    "    int br = ((r5<<8)+li)*2, bg = 0x4000+((g5<<8)+li)*2, bb = 0x8000+((b5<<8)+li)*2;\n"
    "    float cr = texelFetch(cxlat_smp, ivec2(br & 255, br >> 8), 0).r * 255.0;\n"
    "    float cg = texelFetch(cxlat_smp, ivec2(bg & 255, bg >> 8), 0).r * 255.0;\n"
    "    float cb = texelFetch(cxlat_smp, ivec2(bb & 255, bb >> 8), 0).r * 255.0;\n"
    "    rgb = clamp(max(vec3(cr,cg,cb) - 64.0, 0.0) * (255.0/191.0) / 255.0, 0.0, 1.0);\n"
    "  }\n"
    "  frag_color = vec4(rgb, 1.0);\n"
    "}\n";

static const char *game_render_fill_vs_hlsl =
    "cbuffer params : register(b0) { float4x4 mvp; };\n"
    "struct vs_in { float3 pos : POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; float4 tile : TEXCOORD1; float3 lbpl : TEXCOORD2; };\n"
    "struct vs_out { float4 pos : SV_Position; float4 color : COLOR0; float2 uv : TEXCOORD0; nointerpolation float4 tile : TEXCOORD1; nointerpolation float3 lbpl : TEXCOORD2; };\n"
    "vs_out main(vs_in inp) {\n"
    "  vs_out outp;\n"
    "  outp.pos = mul(mvp, float4(inp.pos, 1.0));\n"
    "  outp.color = inp.color; outp.uv = inp.uv; outp.tile = inp.tile; outp.lbpl = inp.lbpl;\n"
    "  return outp;\n"
    "}\n";

static const char *game_render_fill_fs_hlsl =
    "Texture2D<float4> atlas : register(t0);\n"
    "Texture2D<float4> lumat : register(t1);\n"
    "Texture2D<float4> cxlat : register(t2);\n"
    "SamplerState smp : register(s0);\n"
    "struct fs_in { float4 pos : SV_Position; float4 color : COLOR0; float2 uv : TEXCOORD0; nointerpolation float4 tile : TEXCOORD1; nointerpolation float3 lbpl : TEXCOORD2; };\n"
    "bool has(float fl, int bit) { return (((int)(fl + 0.5)) & bit) != 0; }\n"
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
    "  int2 q = s % size;\n"
    "  int2 copy = s / size;\n"
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
    "  float lmax = max(log2(max(min(inp.tile.z, inp.tile.w), 2.0)) - 1.0, 0.0);\n"
    "  float lod = clamp(log2(max(length(ddx(inp.uv)), length(ddy(inp.uv)))), 0.0, lmax);\n"
    "  if (has(fl, 2) && ((((int)inp.pos.x) ^ ((int)inp.pos.y)) & 1) == 0) discard;\n"
    "  float3 rgb = inp.color.rgb;\n"
    "  if (inp.tile.z > 0.0) {\n"
    "    int L0 = (int)floor(lod);\n"
    "    float2 tx = lerp(sample_level(inp.uv, inp.tile, fl, L0),\n"
    "                     sample_level(inp.uv, inp.tile, fl, L0 + 1), frac(lod));\n"
    "    if (has(fl, 1) && tx.y < 0.5) discard;\n"
    "    float al = tx.x;\n"
    "    if (inp.lbpl.x < 0.0) {\n"
    "      if (al > 0.0) rgb = clamp(inp.color.rgb * al * 2.0, 0.0, 1.0);\n"
    "    } else {\n"
    "      int lbyte = 2 * ((int)inp.lbpl.x + (int)(al * 120.0));\n"
    "      float lram = lumat.Load(int3(lbyte & 255, lbyte >> 8, 0)).r * 255.0;\n"
    "      float poly = clamp(inp.lbpl.y, 0.0, 1.0) * 255.0;\n"
    "      int li = (int)(min(lram * poly / 256.0, 63.0) + 0.5);\n"
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
    "    int li = min(((int)(clamp(inp.lbpl.y, 0.0, 1.0) * 255.0 + 0.5)) >> 2, 63);\n"
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
    static const char es_head[] =
        "#version 300 es\n"
        "precision highp float;\n"
        "precision highp int;\n"
        "precision highp sampler2D;\n"
        "precision highp usampler2D;\n";
    static char buf[2][8192];
    if (backend != SG_BACKEND_GLES3 || strncmp(src, head, sizeof head - 1) != 0) return src;
    size_t body = strlen(src) - (sizeof head - 1);
    if (sizeof es_head - 1 + body + 1 > sizeof buf[0]) {
        LOG_ERROR("game_render_glsl: shader too long for the ES buffer");
        return src;
    }
    memcpy(buf[stage], es_head, sizeof es_head - 1);
    memcpy(buf[stage] + sizeof es_head - 1, src + sizeof head - 1, body + 1);
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
        d.label = "game-render-fill-shader";
        if (backend == SG_BACKEND_GLCORE || backend == SG_BACKEND_GLES3) {
            d.vertex_func.source   = game_render_glsl(backend, game_render_fill_vs_glsl, 0);
            d.fragment_func.source = game_render_glsl(backend, game_render_fill_fs_glsl, 1);
        } else if (backend == SG_BACKEND_D3D11) {
            d.vertex_func.source         = game_render_fill_vs_hlsl;
            d.vertex_func.d3d11_target   = "vs_4_0";
            d.fragment_func.source       = game_render_fill_fs_hlsl;
            d.fragment_func.d3d11_target = "ps_4_0";
        }
        g_game_render.fill_shader = sg_make_shader(&d);
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
        p.layout.attrs[4].format   = SG_VERTEXFORMAT_FLOAT3;
        p.layout.attrs[4].offset   = offsetof(game_render_tex_vertex_t, lb);
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
    sg_destroy_pipeline(g_game_render.fill_pipeline);
    sg_destroy_pipeline(g_game_render.fill_pipeline_cw);
    sg_destroy_pipeline(g_game_render.fill_pipeline_ccw);
    sg_destroy_shader(g_game_render.fill_shader);
    sg_destroy_buffer(g_game_render.fill_vbuf);
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

/* Upload the live lumaram + colorxlat tables (raw bus bytes) to the LUT
 * textures the fill shader integer-fetches for the MAME luma ramp.  Cheap
 * (~180 KB/frame); call once per frame before drawing fills. */
static inline void game_render_upload_luts(const uint8_t *luma, const uint8_t *colorxlat) {
    if (!g_game_render.initialized) return;
    if (luma)
        sg_update_image(g_game_render.luma_image, &(sg_image_data){
            .mip_levels[0] = { .ptr = luma, .size = LUMA_SIZE } });
    if (colorxlat)
        sg_update_image(g_game_render.cxlat_image, &(sg_image_data){
            .mip_levels[0] = { .ptr = colorxlat, .size = COLORXLAT_SIZE } });
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
static inline void game_render_draw_indexed(sg_view pen_view, sg_view pal_view,
                                             int ox, int oy, int w, int h) {
    if (!g_game_render.initialized) return;
    if (w <= 0 || h <= 0) return;

    sg_apply_viewport(ox, oy, w, h, true);
    sg_apply_pipeline(g_game_render.indexed_pipeline);
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

static inline void game_render_submit_lines(const game_render_vs_params_t *vs, int first, int vcount);
static inline void game_render_submit_fills(const game_render_vs_params_t *vs, int first, int vcount);

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
            v[_k].lb=lb;    v[_k].pl=T->pl; v[_k].fl=T->fl;
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

static inline void game_render_submit_fills(const game_render_vs_params_t *vs, int first, int vcount) {
    const game_render_vs_params_t vs_params = *vs;
    sg_apply_pipeline(g_backface_cull == 1 ? g_game_render.fill_pipeline_cw
                    : g_backface_cull == 2 ? g_game_render.fill_pipeline_ccw
                    :                        g_game_render.fill_pipeline);
    sg_apply_bindings(&(sg_bindings){
        .vertex_buffers[0] = g_game_render.fill_vbuf,
        .views[0]          = g_game_render.atlas_view,
        .views[1]          = g_game_render.luma_view,
        .views[2]          = g_game_render.cxlat_view,
        .samplers[0]       = g_game_render.atlas_sampler,
    });
    sg_apply_uniforms(0, &(sg_range){ .ptr = &vs_params, .size = sizeof(vs_params) });
    sg_draw(first, vcount, 1);
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

    if (fills) {
        for (int i = 0; i < nt; i++) {
            const geo3d_tri_t *T = &g_geo3d_tris.tris[i];
            game_render_tex_vertex_t *v = &g_game_render.fill_verts[i * 3];
            v[0].x=T->x0; v[0].y=T->y0; v[0].z=T->z0; v[0].u=T->u0; v[0].v=T->v0;
            v[1].x=T->x1; v[1].y=T->y1; v[1].z=T->z1; v[1].u=T->u1; v[1].v=T->v1;
            v[2].x=T->x2; v[2].y=T->y2; v[2].z=T->z2; v[2].u=T->u2; v[2].v=T->v2;
            float lb = g_luma_ramp ? T->lb : -1.0f;
            for (int k = 0; k < 3; k++) {
                v[k].r=T->r; v[k].g=T->g; v[k].b=T->b; v[k].a=1.0f;
                v[k].tx=T->tx; v[k].ty=T->ty; v[k].tw=T->tw; v[k].th=T->th;
                v[k].lb=lb;    v[k].pl=T->pl; v[k].fl=T->fl;
            }
        }
        sg_update_buffer(g_game_render.fill_vbuf, &(sg_range){
            .ptr = g_game_render.fill_verts, .size = (size_t)nt * 3 * sizeof(game_render_tex_vertex_t) });
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
        if (fills && tc > 0) {
            game_render_submit_fills(&vs, run->tri_first * 3, tc * 3);
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
