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

    /* Line (3D wireframe) pipeline */
    sg_buffer   line_vbuf;
    sg_shader   line_shader;
    sg_pipeline line_pipeline;

    /* Fill (3D solid/textured triangle) pipeline */
    sg_buffer   fill_vbuf;
    sg_shader   fill_shader;
    sg_pipeline fill_pipeline;

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
    "layout(location=4) in vec2 a_lbpl;\n"
    "out vec4 color;\n"
    "out vec2 uv;\n"
    "out vec4 tile;\n"
    "out vec2 lbpl;\n"
    "void main() {\n"
    "  mat4 mvp = mat4(vs_params[0], vs_params[1], vs_params[2], vs_params[3]);\n"
    "  gl_Position = mvp * vec4(a_pos, 1.0);\n"
    "  color = a_color; uv = a_uv; tile = a_tile; lbpl = a_lbpl;\n"
    "}\n";

/* MAME luma ramp: texel(4-bit) → lumaram[2*lumabase + texel*16] (even-byte
 * layout) → luma6 = min(lram*poly_luma/256, 63) → colorxlat[ch][(c5<<8)+luma6]
 * → gamma max(c-64,0)*255/191.  lumaram=256x512, colorxlat=256x192 (3 ch @
 * byte 0/0x4000/0x8000).  lb<0 falls back to the old flat_color*luma path. */
static const char *game_render_fill_fs_glsl =
    "#version 410\n"
    "uniform sampler2D atlas_smp;\n"
    "uniform sampler2D luma_smp;\n"
    "uniform sampler2D cxlat_smp;\n"
    "in vec4 color;\n"
    "in vec2 uv;\n"
    "in vec4 tile;\n"
    "in vec2 lbpl;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "  vec3 rgb = color.rgb;\n"
    "  if (tile.z > 0.0) {\n"
    "    vec2 w = uv - tile.zw * floor(uv / tile.zw);\n"
    "    vec2 auv = (tile.xy + w) / vec2(2048.0, 2048.0);\n"
    "    float al = texture(atlas_smp, auv).r;\n"
    "    if (lbpl.x < 0.0) {\n"
    "      if (al > 0.0) rgb = clamp(color.rgb * al * 2.0, 0.0, 1.0);\n"
    "    } else {\n"
    "      int texel4 = int(al * 15.0 + 0.5);\n"
    "      int lbyte = 2 * int(lbpl.x) + texel4 * 16;\n"
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
    "  } else {\n"
    "    rgb = color.rgb * clamp(lbpl.y, 0.0, 1.0);\n"
    "  }\n"
    "  frag_color = vec4(rgb, 1.0);\n"
    "}\n";

static const char *game_render_fill_vs_hlsl =
    "cbuffer params : register(b0) { float4x4 mvp; };\n"
    "struct vs_in { float3 pos : POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; float4 tile : TEXCOORD1; float2 lbpl : TEXCOORD2; };\n"
    "struct vs_out { float4 pos : SV_Position; float4 color : COLOR0; float2 uv : TEXCOORD0; float4 tile : TEXCOORD1; float2 lbpl : TEXCOORD2; };\n"
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
    "struct fs_in { float4 pos : SV_Position; float4 color : COLOR0; float2 uv : TEXCOORD0; float4 tile : TEXCOORD1; float2 lbpl : TEXCOORD2; };\n"
    "float4 main(fs_in inp) : SV_Target0 {\n"
    "  float3 rgb = inp.color.rgb;\n"
    "  if (inp.tile.z > 0.0) {\n"
    "    float2 w = inp.uv - inp.tile.zw * floor(inp.uv / inp.tile.zw);\n"
    "    float2 auv = (inp.tile.xy + w) / float2(2048.0, 2048.0);\n"
    "    float al = atlas.Sample(smp, auv).r;\n"
    "    if (inp.lbpl.x < 0.0) {\n"
    "      if (al > 0.0) rgb = clamp(inp.color.rgb * al * 2.0, 0.0, 1.0);\n"
    "    } else {\n"
    "      int texel4 = (int)(al * 15.0 + 0.5);\n"
    "      int lbyte = 2 * (int)inp.lbpl.x + texel4 * 16;\n"
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
    "  } else {\n"
    "    rgb = inp.color.rgb * clamp(inp.lbpl.y, 0.0, 1.0);\n"
    "  }\n"
    "  return float4(rgb, 1.0);\n"
    "}\n";

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
            d.vertex_func.source   = game_render_tile_vs_glsl;
            d.fragment_func.source = game_render_tile_fs_glsl;
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
    }

    g_game_render.tile_sampler = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_NEAREST,
        .mag_filter = SG_FILTER_NEAREST,
        .wrap_u     = SG_WRAP_CLAMP_TO_EDGE,
        .wrap_v     = SG_WRAP_CLAMP_TO_EDGE,
        .label      = "game-render-tile-sampler",
    });

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
            d.vertex_func.source   = game_render_line_vs_glsl;
            d.fragment_func.source = game_render_line_fs_glsl;
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
            d.vertex_func.source   = game_render_fill_vs_glsl;
            d.fragment_func.source = game_render_fill_fs_glsl;
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
        p.layout.attrs[4].format   = SG_VERTEXFORMAT_FLOAT2;
        p.layout.attrs[4].offset   = offsetof(game_render_tex_vertex_t, lb);
        p.layout.buffers[0].stride = sizeof(game_render_tex_vertex_t);
        p.depth.compare            = SG_COMPAREFUNC_LESS_EQUAL;
        p.depth.write_enabled      = true;
        p.label = "game-render-fill-pipeline";
        g_game_render.fill_pipeline = sg_make_pipeline(&p);
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
    sg_destroy_shader(g_game_render.fill_shader);
    sg_destroy_buffer(g_game_render.fill_vbuf);
    sg_destroy_pipeline(g_game_render.line_pipeline);
    sg_destroy_shader(g_game_render.line_shader);
    sg_destroy_buffer(g_game_render.line_vbuf);
    sg_destroy_sampler(g_game_render.tile_sampler);
    sg_destroy_pipeline(g_game_render.tile_pipeline);
    sg_destroy_shader(g_game_render.tile_shader);
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

/*
 * Decode the 4-bit luma texture sheet (texram0) into the R8 atlas and upload.
 * Logical layout 2048×1024 (per MAME model2rd.ipp get_texel): the sheet is
 * stored 1024×2048, so x>=1024 wraps to the other half via y^=1024.  Each
 * 32-bit word holds a 2×2 block of nibbles selected by (x&1,y&1).
 * Call once per frame (cheap; 2M texels) before drawing fills.
 */
static inline void game_render_upload_atlas(const uint8_t *texram0,
                                            const uint8_t *texram1,
                                            size_t sheet_size) {
    if (!g_game_render.initialized || !texram0) return;
    /* Skip the multi-megatexel decode while both texture banks are empty (early
     * boot, before the i960 uploads textures) — fills fall back to flat color. */
    {
        size_t probe = 0;
        for (size_t k = 0; k < sheet_size && probe < 16; k += 0x1000) probe += texram0[k] ? 1 : 0;
        if (texram1)
            for (size_t k = 0; k < sheet_size && probe < 16; k += 0x1000) probe += texram1[k] ? 1 : 0;
        if (probe == 0) return;
    }
    static uint8_t atlas[GEO3D_ATLAS_W * GEO3D_ATLAS_H];   /* 2048×2048 (two sheets) */
    const uint32_t *sheets[2] = { (const uint32_t *)texram0, (const uint32_t *)texram1 };
    size_t nwords = sheet_size / 4;
    for (int s = 0; s < 2; s++) {
        const uint32_t *sheet = sheets[s];
        if (!sheet) continue;
        for (int y = 0; y < GEO3D_SHEET_H; y++) {
            for (int x = 0; x < GEO3D_ATLAS_W; x++) {
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
    sg_update_image(g_game_render.atlas_image, &(sg_image_data){
        .mip_levels[0] = { .ptr = atlas, .size = sizeof(atlas) },
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

    sg_apply_pipeline(g_game_render.line_pipeline);
    sg_apply_bindings(&(sg_bindings){
        .vertex_buffers[0] = g_game_render.line_vbuf,
    });
    sg_apply_uniforms(0, &(sg_range){ .ptr = &vs_params, .size = sizeof(vs_params) });
    sg_draw(0, vcount, 1);
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
            v[_k].lb=lb;    v[_k].pl=T->pl;
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

    sg_apply_pipeline(g_game_render.fill_pipeline);
    sg_apply_bindings(&(sg_bindings){
        .vertex_buffers[0] = g_game_render.fill_vbuf,
        .views[0]          = g_game_render.atlas_view,
        .views[1]          = g_game_render.luma_view,
        .views[2]          = g_game_render.cxlat_view,
        .samplers[0]       = g_game_render.atlas_sampler,
    });
    sg_apply_uniforms(0, &(sg_range){ .ptr = &vs_params, .size = sizeof(vs_params) });
    sg_draw(0, vcount, 1);
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
            /* Viewport maps NDC (-1..1) to this sub-window — objects project
             * relative to the sub-window center rather than the full screen. */
            sg_apply_viewport(sx, sy, sw, sh, true);
            sg_apply_scissor_rect(sx, sy, sw, sh, true);
            /* All clip-window scenes (portrait chars, adv_movie rooms, Egg Robo
             * sequences) are fixed-view: the i960 encodes positions in world
             * space relative to a neutral camera.  The fighting camera tracks
             * players that don't exist in these scenes and produces wrong
             * geometry.  clip_win_neutral_cam distinguishes portrait aspect
             * but both branches use the neutral camera.
             * For portrait cells (each cell = one single-model character), aim the
             * neutral camera at the character's own Y so it sits centered in the cell.
             * The revealed-state set_pos uses y=-0.8 (ndc_y ~ -0.35 → low, spilling out
             * the bottom); matching the camera Y re-centers it (intro state y=0 is
             * unaffected).  Scoped to portrait cells so multi-part scenes (Eggman lab)
             * keep the origin camera.
             * g_portrait_cam_y_frac scales how much of the char Y the camera
             * follows; g_portrait_fov_deg (if >0) overrides the FOV to zoom in. */
            float pcam_y = 0.0f;
            if (cm->clip_win_neutral_cam) {
                pcam_y = cm->matrix[7] * g_portrait_cam_y_frac;
                /* Per-row snap: top row sits low in its cell, bottom row high —
                 * both pulled toward the screen-center label band where the
                 * circles cluster.  Camera up (+y) renders the model lower. */
                pcam_y += (cm->clip_win_y < VIDEO_HEIGHT / 2)
                        ? g_portrait_row_shift : -g_portrait_row_shift;
            }
            float pfov = fov_deg;
            if (cm->clip_win_neutral_cam) {
                if (g_portrait_fov_deg > 0.0f) {
                    pfov = g_portrait_fov_deg;          /* manual dial override */
                } else {
                    /* Auto: the real hardware renders the full frame at the global
                     * FOV and the window crops a slice.  The cell covers clip_win_h
                     * of the VIDEO_HEIGHT-tall frame, so its vertical angular extent
                     * is 2*atan(tan(fov/2) * clip_win_h / VIDEO_HEIGHT).  A 136-tall
                     * cell at fov 60 → ~23 deg. */
                    float t = tanf(fov_deg * 0.5f * 3.14159265f / 180.0f)
                            * (float)cm->clip_win_h / (float)VIDEO_HEIGHT;
                    pfov = 2.0f * atanf(t) * 180.0f / 3.14159265f;
                }
            }
            if (!cm->clip_win_neutral_cam && g_geo_subwin_game_cam) {
                /* Non-portrait sub-window (e.g. Eggman stage): use the game camera
                 * within the scissor so it rotates, instead of the neutral cam. */
                game_render_draw_fills(cam_x, cam_y, cam_z, rot_y, rot_x, fov_deg, cell_aspect);
                game_render_draw_lines(cam_x, cam_y, cam_z, rot_y, rot_x, fov_deg, cell_aspect);
            } else {
                game_render_draw_fills(0.0f, pcam_y, 0.0f, 0.0f, 0.0f, pfov, cell_aspect);
                game_render_draw_lines(0.0f, pcam_y, 0.0f, 0.0f, 0.0f, pfov, cell_aspect);
            }
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
