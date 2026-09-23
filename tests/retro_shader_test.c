/*
 * retro_shader_test.c -- libretro GLSL presets (ui/retro_shader.h) without a GPU:
 * the parts that decide WHAT runs, which are also the parts a player's preset
 * can get wrong. The GL runner is not under test here; the web build and the
 * Linux desktop run it.
 *
 *  (A) Paths: normalisation, joining, the page's file table found by suffix.
 *  (B) A single .glsl: one pass, straight to the screen, its parameters.
 *  (C) A two-pass .glslp: scale types, filters, wrap, alias, framebuffers,
 *      frame_count_mod, a LUT decoded from a PNG, parameter overrides, and a
 *      #reference whose own paths resolve against its own folder.
 *  (D) What is refused, with a sentence: slang, Cg, a missing shader, a pass count.
 */
#define NDEBUG 1
#include <math.h>
#include <stdio.h>

#define RS_NO_GL 1
#include "retro_shader.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

static void add_text(const char *path, const char *text) {
    rs_vfs_add(path, (const uint8_t *)text, strlen(text));
}

static const char *k_crt =
    "#pragma parameter SCAN \"Scanline strength\" 0.5 0.0 1.0 0.05\n"
    "#pragma parameter MASK \"Mask\" 1.0 0.0 3.0\n"
    "#if defined(VERTEX)\nvoid main() {}\n#elif defined(FRAGMENT)\nvoid main() {}\n#endif\n";

static const char *k_blur =
    "#pragma parameter SCAN \"Scanline strength (again)\" 0.9 0.0 1.0 0.1\n"
    "#pragma parameter BLUR \"Blur\" 2.0 1.0 4.0 1.0\n"
    "#if defined(VERTEX)\nvoid main() {}\n#elif defined(FRAGMENT)\nvoid main() {}\n#endif\n";

int main(void) {
    char buf[RS_PATH], err[RS_ERR];

    /* ---- (A) ---- */
    snprintf(buf, sizeof buf, "a\\b\\.\\c\\..\\d.glsl");
    rs_path_normalize(buf);
    CHECK(!strcmp(buf, "a/b/d.glsl"), "normalize: backslashes, . and .. go");
    snprintf(buf, sizeof buf, "../../x/y.png");
    rs_path_normalize(buf);
    CHECK(!strcmp(buf, "../../x/y.png"), "normalize: a leading .. that cannot pop stays");
    rs_path_join("presets/crt/", "../../shaders/crt.glsl", buf, sizeof buf);
    CHECK(!strcmp(buf, "shaders/crt.glsl"), "join: relative to the preset's folder");
    rs_path_join("presets/", "/abs/x.glsl", buf, sizeof buf);
    CHECK(!strcmp(buf, "/abs/x.glsl"), "join: an absolute path stands alone");
    CHECK(rs_path_suffix_match("my/shaders/crt/crt-geom.glsl", "shaders/crt/crt-geom.glsl") == 3,
          "suffix: three trailing components shared");
    CHECK(rs_path_suffix_match("a/CRT.GLSL", "b/crt.glsl") == 1, "suffix: file names match whatever the case");

    add_text("pick/crt-geom.glsl", k_crt);
    add_text("pick/other/crt-geom.glsl", k_blur);
    add_text("elsewhere/shaders/crt/crt-geom.glsl", k_crt);
    const rs_vfs_file_t *f = rs_vfs_find("x/shaders/crt/crt-geom.glsl");
    CHECK(f && !strcmp(f->path, "elsewhere/shaders/crt/crt-geom.glsl"), "table: the longest shared tail wins");
    f = rs_vfs_find("pick/crt-geom.glsl");
    CHECK(f && !strcmp(f->path, "pick/crt-geom.glsl"), "table: an exact path wins outright");
    CHECK(rs_vfs_find("nothing.glsl") == NULL, "table: an unknown name is not found");
    rs_vfs_clear();

    /* ---- (B) ---- */
    static rs_preset_t p;
    add_text("crt.glsl", k_crt);
    bool ok = rs_preset_load(&p, "crt.glsl", err, sizeof err);
    CHECK(ok && p.n_passes == 1, "glsl: one pass");
    CHECK(ok && p.pass[0].type_x == RS_SCALE_VIEWPORT && !p.pass[0].scale_set, "glsl: drawn straight to the screen");
    CHECK(ok && p.n_params == 2 && !strcmp(p.param[0].name, "SCAN") && p.param[0].value == 0.5f &&
          fabsf(p.param[0].step - 0.05f) < 1e-6f, "glsl: #pragma parameter read, step included");
    CHECK(ok && fabsf(p.param[1].step - 0.03f) < 1e-6f, "glsl: a missing step is a hundredth of the range");
    CHECK(ok && !strcmp(p.param[0].desc, "Scanline strength"), "glsl: the description without its quotes");
    rs_preset_free(&p);
    rs_vfs_clear();

    /* ---- (C) ---- */
    uint8_t px[2 * 2 * 4] = { 255, 0, 0, 255,  0, 255, 0, 255,  0, 0, 255, 255,  255, 255, 255, 255 };
    int png_len = 0;
    unsigned char *png = stbi_write_png_to_mem(px, 2 * 4, 2, 2, 4, &png_len);
    CHECK(png && png_len > 0, "fixture: a 2x2 PNG");
    rs_vfs_add("pack/textures/mask.png", png, (size_t)png_len);
    STBIW_FREE(png);
    add_text("pack/shaders/crt.glsl", k_crt);
    add_text("pack/shaders/blur.glsl", k_blur);
    /* The base preset lives in its own folder, and its paths are its own. */
    add_text("pack/base/base.glslp",
             "shaders = 2\n"
             "shader0 = ../shaders/blur.glsl\n"
             "shader1 = ../shaders/crt.glsl\n"
             "BLUR = 3.0\n");
    add_text("pack/presets/mine.glslp",
             "#reference \"../base/base.glslp\"\n"
             "# a comment\n"
             "filter_linear0 = true\n"
             "wrap_mode0 = repeat\n"
             "scale_type0 = source\n"
             "scale0 = 2.0\n"
             "alias0 = \"BlurPass\"\n"
             "float_framebuffer0 = true\n"
             "mipmap_input1 = true\n"
             "frame_count_mod1 = 60\n"
             "scale_type_x1 = viewport\n"
             "scale_x1 = 1.0\n"
             "scale_type_y1 = absolute\n"
             "scale_y1 = 720\n"
             "textures = \"mask_grille\"\n"
             "mask_grille = ../textures/mask.png\n"
             "mask_grille_linear = true\n"
             "mask_grille_wrap_mode = repeat\n"
             "parameters = \"SCAN\"\n"
             "SCAN = 0.25\n");
    ok = rs_preset_load(&p, "pack/presets/mine.glslp", err, sizeof err);
    if (!ok) printf("      (%s)\n", err);
    CHECK(ok && p.n_passes == 2, "glslp: two passes through #reference");
    CHECK(ok && !strcmp(p.pass[0].path, "pack/shaders/blur.glsl"), "glslp: the referenced preset's paths are its own");
    CHECK(ok && p.pass[0].filter_set && p.pass[0].filter_linear && p.pass[0].wrap == RS_WRAP_REPEAT,
          "glslp: filter and wrap");
    CHECK(ok && p.pass[0].scale_set && p.pass[0].type_x == RS_SCALE_SOURCE && p.pass[0].scale_x == 2.0f,
          "glslp: source x2");
    CHECK(ok && !strcmp(p.pass[0].alias, "BlurPass") && p.pass[0].float_fb, "glslp: alias and float framebuffer");
    CHECK(ok && p.pass[1].type_x == RS_SCALE_VIEWPORT && p.pass[1].type_y == RS_SCALE_ABSOLUTE &&
          p.pass[1].scale_y == 720.0f && p.pass[1].scale_set, "glslp: per-axis scale types");
    CHECK(ok && p.pass[1].mipmap_input && p.pass[1].frame_count_mod == 60, "glslp: mipmap_input, frame_count_mod");
    CHECK(ok && !p.pass[1].filter_set && p.pass[1].wrap == RS_WRAP_BORDER, "glslp: RetroArch's defaults when unset");
    CHECK(ok && p.n_luts == 1 && !strcmp(p.lut[0].name, "mask_grille") && p.lut[0].w == 2 && p.lut[0].h == 2 &&
          p.lut[0].linear && p.lut[0].wrap == RS_WRAP_REPEAT, "glslp: the LUT, decoded");
    CHECK(ok && p.lut[0].rgba && p.lut[0].rgba[0] == 255 && p.lut[0].rgba[5] == 255 && p.lut[0].rgba[10] == 255,
          "glslp: the LUT's top row comes first");
    rs_param_t *scan = ok ? rs_preset_param(&p, "SCAN") : NULL;
    rs_param_t *blur = ok ? rs_preset_param(&p, "BLUR") : NULL;
    CHECK(ok && p.n_params == 3, "glslp: parameters from both passes, a repeated name once");
    CHECK(scan && scan->value == 0.25f && scan->initial == 0.25f, "glslp: the preset's value overrides the shader's");
    CHECK(scan && !strcmp(scan->desc, "Scanline strength (again)"), "glslp: the first pass to name it describes it");
    CHECK(blur && blur->value == 3.0f, "glslp: a referenced preset's override carries");
    rs_preset_free(&p);

    /* A last pass with no scale draws to the screen; the passes before it are source x1. */
    add_text("pack/two.glslp", "shaders = 2\nshader0 = shaders/blur.glsl\nshader1 = shaders/crt.glsl\n");
    ok = rs_preset_load(&p, "pack/two.glslp", err, sizeof err);
    CHECK(ok && !p.pass[0].scale_set && p.pass[0].type_x == RS_SCALE_SOURCE && p.pass[0].scale_x == 1.0f,
          "defaults: an unscaled middle pass is source x1");
    CHECK(ok && !p.pass[1].scale_set && p.pass[1].type_x == RS_SCALE_VIEWPORT, "defaults: an unscaled last pass is the viewport");
    rs_preset_free(&p);

    /* ---- (D) ---- */
    CHECK(!rs_preset_load(&p, "x.slangp", err, sizeof err) && strstr(err, "GLSL"), "refused: slang, pointing at GLSL");
    CHECK(!rs_preset_load(&p, "x.cgp", err, sizeof err) && strstr(err, "Cg"), "refused: Cg");
    add_text("pack/missing.glslp", "shaders = 1\nshader0 = shaders/nowhere.glsl\n");
    CHECK(!rs_preset_load(&p, "pack/missing.glslp", err, sizeof err) && strstr(err, "nowhere.glsl"),
          "refused: a missing shader, by name");
    add_text("pack/zero.glslp", "shaders = 0\n");
    CHECK(!rs_preset_load(&p, "pack/zero.glslp", err, sizeof err), "refused: no passes");
    add_text("pack/stage.glsl", "#version 450\n#pragma stage vertex\nvoid main() {}\n");
    CHECK(!rs_preset_load(&p, "pack/stage.glsl", err, sizeof err) && strstr(err, "slang"),
          "refused: a slang shader renamed .glsl");
    add_text("pack/nolut.glslp", "shaders = 1\nshader0 = shaders/crt.glsl\ntextures = \"lut\"\n");
    CHECK(!rs_preset_load(&p, "pack/nolut.glslp", err, sizeof err) && strstr(err, "lut"),
          "refused: a texture with no file");
    rs_vfs_clear();

    /* ---- (E) ---- */
    {
        const char *src =
            "uniform float PARAM;\n"                                       /* 1 */
            "const float K = 2.0;\n"                                       /* 2 */
            "#ifdef FRAGMENT\n"                                            /* 3 */
            "static const float x = PARAM * K; // scaled\n"                /* 4 */
            "#endif\n"                                                     /* 5 */
            "vec3 arr[2] = vec3[](vec3(PARAM),\n"                          /* 6 */
            "                     vec3(1.0));\n"                           /* 7 */
            "float a = 1.0, b = PARAM;\n"                                  /* 8 */
            "struct S { float f; };\n"                                     /* 9 */
            "float f(float v) { float y = v * PARAM; return y; }\n"        /* 10 */
            "void main()\n{\n  gl_FragColor = vec4(x);\n}\n";
        int lines[] = { 4, 7, 8, 10 };
        int counter = 0;
        char *out = rs_hoist_globals(src, lines, 4, &counter);
        CHECK(out != NULL && counter == 2, "hoist: two statements moved (a declarator list and a local are not)");
        if (out) {
            CHECK(strstr(out, "float x;\n#define RS_INIT_0 x =  PARAM * K;") != NULL,
                  "hoist: const and static gone, the initialiser in a macro, its comment cut");
            CHECK(strstr(out, "#ifdef FRAGMENT\n  float x;") != NULL || strstr(out, "#ifdef FRAGMENT\n float x;") != NULL ||
                  strstr(out, "#ifdef FRAGMENT\nfloat x;") != NULL, "hoist: the declaration stays inside its #if");
            CHECK(strstr(out, "vec3 arr[2];\n#define RS_INIT_1 arr =") != NULL, "hoist: an array keeps its size, the name is found");
            CHECK(strstr(out, "vec3(PARAM),                      vec3(1.0))") != NULL, "hoist: a two-line initialiser on one line");
            CHECK(strstr(out, "const float K = 2.0;") != NULL, "hoist: a line the compiler did not name is untouched");
            CHECK(strstr(out, "float a = 1.0, b = PARAM;") != NULL, "hoist: a declarator list is left alone");
            CHECK(strstr(out, "float y = v * PARAM;") != NULL, "hoist: a function's own locals are left alone");
            const char *m = strstr(out, "void main()\n{");
            CHECK(m && strstr(m, "#ifdef RS_INIT_0\nRS_INIT_0\n#endif\n#ifdef RS_INIT_1\nRS_INIT_1\n#endif") != NULL,
                  "hoist: the assignments open main(), in source order, each guarded");
            /* A second round appends after the first. */
            int more[] = { 2 };
            char *out2 = rs_hoist_globals(out, more, 1, &counter);
            CHECK(out2 && counter == 3 && strstr(out2, "RS_INIT_1\n#endif\n#ifdef RS_INIT_2\nRS_INIT_2\n#endif\n/*rs_inits*/"),
                  "hoist: a later round's assignments come after the earlier ones");
            free(out2);
            free(out);
        }
        int got[4];
        int n = rs_glsl_global_init_errors(
            "ERROR: 0:9: '=' : global variable initializers must be constant expressions\n"
            "ERROR: 0:12: 'x' : undeclared identifier\n"
            "ERROR: 0:40: '=' : global variable initializers must be constant expressions\n", 5, got, 4);
        CHECK(n == 2 && got[0] == 4 && got[1] == 35, "hoist: the compiler's lines, less the prologue");
    }

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
