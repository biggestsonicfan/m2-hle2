/*
 * retro_shader.h -- libretro GLSL shader presets (.glslp / .glsl) over the game's
 * picture, for the GL builds (Linux desktop, the web build).
 *
 * The format is RetroArch's "GLSL" shader format, the one in libretro's
 * glsl-shaders repository: a preset names up to 26 passes, each one source file
 * holding both stages behind `#if defined(VERTEX)` / `#elif defined(FRAGMENT)`,
 * plus lookup textures and parameter overrides. The runner follows RetroArch's GL
 * driver (gfx/drivers_shader/shader_glsl.c) closely enough that presets written
 * for it run unchanged:
 *
 *   - the uniforms and attributes it names: MVPMatrix, FrameCount, FrameDirection,
 *     OutputSize, TextureSize, InputSize, Texture, Orig*, Pass#*, PassPrev#*,
 *     Prev* / Prev#* (frame history), each pass's alias, the LUTs by name, every
 *     `#pragma parameter` as a float uniform; the old `ruby` prefix too;
 *   - its orientation: texture coordinate v = 0 is the TOP of the picture in
 *     every pass, and only the last pass flips onto the screen;
 *   - its scale rules (source / viewport / absolute), float and sRGB
 *     framebuffers, mipmap_input, frame_count_mod, `#reference`.
 *
 * Slang presets (.slangp) are not handled: they are Vulkan GLSL and need a
 * compiler (glslang + SPIRV-Cross) that this project does not carry. Neither is
 * D3D11, for the same reason -- the Windows build offers the built-in CRT only.
 *
 * Two halves. The PRESET half (parsing, files, parameters) is plain C and builds
 * anywhere, which is what tests/retro_shader_test.c holds. The RUNNER half is raw
 * GL, compiled where sokol_gfx is on a GL backend. It leaves GL state dirty on
 * purpose; the caller (post_shader.h) hands control back to sokol with
 * sg_reset_state_cache().
 *
 * Files come from disk, or -- in the browser, which has no file system -- from a
 * small in-memory table the page fills with the files the player picked
 * (rs_vfs_add). A preset's relative paths resolve against that table by path,
 * then by trailing path components, then by file name, so picking a preset and
 * its shaders out of different folders still finds them.
 */
#ifndef RETRO_SHADER_H
#define RETRO_SHADER_H

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

/* LUTs: PNG above all, and the few JPEG/BMP/TGA ones. Memory only: the files
 * are read through rs_read. Static, so a second stb_image elsewhere cannot clash. */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#define RS_MAX_PASSES  26     /* RetroArch's GFX_MAX_SHADERS */
#define RS_MAX_LUTS    16
#define RS_MAX_PARAMS  160
#define RS_MAX_HISTORY 7      /* Prev .. Prev6 */
#define RS_NAME        64
#define RS_PATH        512
#define RS_ERR         2048

typedef enum { RS_SCALE_SOURCE, RS_SCALE_VIEWPORT, RS_SCALE_ABSOLUTE } rs_scale_type_t;
typedef enum { RS_WRAP_BORDER, RS_WRAP_EDGE, RS_WRAP_REPEAT, RS_WRAP_MIRROR } rs_wrap_t;

typedef struct {
    char            path[RS_PATH];
    char           *source;          /* the file as read, NUL-terminated */
    bool            filter_set, filter_linear;
    rs_wrap_t       wrap;
    bool            scale_set;       /* the preset gave this pass a scale */
    rs_scale_type_t type_x, type_y;
    float           scale_x, scale_y;
    bool            float_fb, srgb_fb, mipmap_input;
    unsigned        frame_count_mod;
    char            alias[RS_NAME];
} rs_pass_t;

typedef struct {
    char      name[RS_NAME];
    char      path[RS_PATH];
    bool      linear, mipmap;
    rs_wrap_t wrap;
    uint8_t  *rgba;                  /* decoded, top row first */
    int       w, h;
} rs_lut_t;

typedef struct {
    char  name[RS_NAME];
    char  desc[96];
    float value, initial, min, max, step;
} rs_param_t;

typedef struct {
    char       name[RS_NAME];        /* the preset's file name, for the UI */
    int        n_passes;
    rs_pass_t  pass[RS_MAX_PASSES];
    int        n_luts;
    rs_lut_t   lut[RS_MAX_LUTS];
    int        n_params;
    rs_param_t param[RS_MAX_PARAMS];
} rs_preset_t;

/* ---- Paths ------------------------------------------------------------------ */

static inline bool rs_path_absolute(const char *p) {
    return p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':');
}

/* Forward slashes, no "." and no "dir/.." (a leading ".." that cannot be popped stays). */
static inline void rs_path_normalize(char *p) {
    for (char *c = p; *c; c++) if (*c == '\\') *c = '/';
    char out[RS_PATH];
    size_t n = 0;
    const char *s = p;
    bool abs = *s == '/';
    if (abs) { out[n++] = '/'; s++; }
    size_t root = n;
    while (*s) {
        const char *e = strchr(s, '/');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        if (len == 0 || (len == 1 && s[0] == '.')) {
            /* skip */
        } else if (len == 2 && s[0] == '.' && s[1] == '.') {
            /* Pop the last component unless there is none, or it is itself "..". */
            size_t k = n;
            while (k > root && out[k - 1] == '/') k--;
            size_t start = k;
            while (start > root && out[start - 1] != '/') start--;
            bool poppable = k > start && !(k - start == 2 && out[start] == '.' && out[start + 1] == '.');
            if (poppable) n = start;
            else if (n + 3 < sizeof out) { memcpy(out + n, "../", 3); n += 3; }
        } else if (n + len + 1 < sizeof out) {
            memcpy(out + n, s, len); n += len;
            out[n++] = '/';
        }
        if (!e) break;
        s = e + 1;
    }
    if (n > root && out[n - 1] == '/') n--;
    out[n] = '\0';
    memcpy(p, out, n + 1);
}

static inline void rs_path_dir(const char *path, char *out, size_t cap) {
    snprintf(out, cap, "%s", path);
    char *a = strrchr(out, '/'), *b = strrchr(out, '\\');
    char *s = a > b ? a : b;
    if (s) s[1] = '\0'; else out[0] = '\0';
}

static inline void rs_path_join(const char *dir, const char *rel, char *out, size_t cap) {
    if (rs_path_absolute(rel) || !dir[0]) snprintf(out, cap, "%s", rel);
    else snprintf(out, cap, "%s%s%s", dir,
                  (dir[strlen(dir) - 1] == '/' || dir[strlen(dir) - 1] == '\\') ? "" : "/", rel);
    rs_path_normalize(out);
}

static inline const char *rs_path_base(const char *p) {
    const char *a = strrchr(p, '/'), *b = strrchr(p, '\\');
    const char *s = a > b ? a : b;
    return s ? s + 1 : p;
}

static inline bool rs_ext_is(const char *path, const char *ext) {
    size_t n = strlen(path), e = strlen(ext);
    if (n < e) return false;
    for (size_t i = 0; i < e; i++)
        if (tolower((unsigned char)path[n - e + i]) != tolower((unsigned char)ext[i])) return false;
    return true;
}

/* ---- Files: the page's table, or the disk ------------------------------------ */

/* `used`: read by the last rs_preset_load, so the page can keep just those. */
typedef struct { char *path; uint8_t *data; size_t len; bool used; } rs_vfs_file_t;
static rs_vfs_file_t *g_rs_vfs;
static int            g_rs_vfs_n, g_rs_vfs_cap;

static inline void rs_vfs_clear(void) {
    for (int i = 0; i < g_rs_vfs_n; i++) { free(g_rs_vfs[i].path); free(g_rs_vfs[i].data); }
    free(g_rs_vfs);
    g_rs_vfs = NULL;
    g_rs_vfs_n = g_rs_vfs_cap = 0;
}

/* Copies both. A later file under the same path replaces the earlier one. */
static inline bool rs_vfs_add(const char *path, const uint8_t *data, size_t len) {
    char norm[RS_PATH];
    snprintf(norm, sizeof norm, "%s", path);
    rs_path_normalize(norm);
    for (int i = 0; i < g_rs_vfs_n; i++) {
        if (strcmp(g_rs_vfs[i].path, norm) == 0) {
            uint8_t *d = (uint8_t *)malloc(len + 1);
            if (!d) return false;
            memcpy(d, data, len); d[len] = 0;
            free(g_rs_vfs[i].data);
            g_rs_vfs[i].data = d; g_rs_vfs[i].len = len;
            return true;
        }
    }
    if (g_rs_vfs_n == g_rs_vfs_cap) {
        int cap = g_rs_vfs_cap ? g_rs_vfs_cap * 2 : 32;
        rs_vfs_file_t *n = (rs_vfs_file_t *)realloc(g_rs_vfs, (size_t)cap * sizeof *n);
        if (!n) return false;
        g_rs_vfs = n; g_rs_vfs_cap = cap;
    }
    rs_vfs_file_t *f = &g_rs_vfs[g_rs_vfs_n];
    f->path = (char *)malloc(strlen(norm) + 1);
    f->data = (uint8_t *)malloc(len + 1);
    if (!f->path || !f->data) { free(f->path); free(f->data); return false; }
    strcpy(f->path, norm);
    memcpy(f->data, data, len); f->data[len] = 0;
    f->len = len;
    f->used = false;
    g_rs_vfs_n++;
    return true;
}

/* The table's paths the last load read, one per line. */
static inline const char *rs_vfs_used(void) {
    static char out[RS_PATH * (RS_MAX_PASSES + RS_MAX_LUTS + 8)];
    size_t n = 0;
    out[0] = '\0';
    for (int i = 0; i < g_rs_vfs_n; i++) {
        if (!g_rs_vfs[i].used) continue;
        size_t len = strlen(g_rs_vfs[i].path);
        if (n + len + 2 > sizeof out) break;
        memcpy(out + n, g_rs_vfs[i].path, len);
        n += len;
        out[n++] = '\n';
        out[n] = '\0';
    }
    return out;
}

/* How many trailing path components two paths share (case-insensitive). */
static inline int rs_path_suffix_match(const char *a, const char *b) {
    const char *ea = a + strlen(a), *eb = b + strlen(b);
    int comps = 0;
    while (ea > a && eb > b) {
        const char *sa = ea, *sb = eb;
        while (sa > a && sa[-1] != '/') sa--;
        while (sb > b && sb[-1] != '/') sb--;
        size_t la = (size_t)(ea - sa), lb = (size_t)(eb - sb);
        if (la != lb || la == 0 || (lb == 2 && sb[0] == '.' && sb[1] == '.')) break;
        for (size_t k = 0; k < la; k++)
            if (tolower((unsigned char)sa[k]) != tolower((unsigned char)sb[k])) return comps;
        comps++;
        if (sa == a || sb == b) break;
        ea = sa - 1; eb = sb - 1;
    }
    return comps;
}

static inline const rs_vfs_file_t *rs_vfs_find(const char *path) {
    char norm[RS_PATH];
    snprintf(norm, sizeof norm, "%s", path);
    rs_path_normalize(norm);
    const rs_vfs_file_t *best = NULL;
    int best_n = 0;
    for (int i = 0; i < g_rs_vfs_n; i++) {
        if (strcmp(g_rs_vfs[i].path, norm) == 0) return &g_rs_vfs[i];
        int n = rs_path_suffix_match(g_rs_vfs[i].path, norm);
        if (n > best_n) { best_n = n; best = &g_rs_vfs[i]; }
    }
    return best;   /* at least the file name matched, or NULL */
}

/* The whole file, NUL-terminated, malloc'd. */
static inline bool rs_read(const char *path, uint8_t **data, size_t *len) {
    *data = NULL; *len = 0;
    if (g_rs_vfs_n > 0) {
        rs_vfs_file_t *f = (rs_vfs_file_t *)rs_vfs_find(path);
        if (!f) return false;
        f->used = true;
        *data = (uint8_t *)malloc(f->len + 1);
        if (!*data) return false;
        memcpy(*data, f->data, f->len + 1);
        *len = f->len;
        return true;
    }
#ifdef __EMSCRIPTEN__
    return false;
#else
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n < 0 || n > 64 * 1024 * 1024) { fclose(fp); return false; }
    uint8_t *d = (uint8_t *)malloc((size_t)n + 1);
    if (!d) { fclose(fp); return false; }
    size_t got = fread(d, 1, (size_t)n, fp);
    fclose(fp);
    d[got] = 0;
    *data = d; *len = got;
    return true;
#endif
}

/* ---- The preset ---------------------------------------------------------------- */

typedef struct { char key[96]; char val[RS_PATH]; char base[RS_PATH]; } rs_kv_t;
typedef struct { rs_kv_t *kv; int n, cap; } rs_kvs_t;

static inline void rs_kvs_set(rs_kvs_t *t, const char *key, const char *val, const char *base) {
    for (int i = 0; i < t->n; i++) {
        if (strcmp(t->kv[i].key, key) == 0) {
            snprintf(t->kv[i].val, sizeof t->kv[i].val, "%s", val);
            snprintf(t->kv[i].base, sizeof t->kv[i].base, "%s", base);
            return;
        }
    }
    if (t->n == t->cap) {
        int cap = t->cap ? t->cap * 2 : 64;
        rs_kv_t *n = (rs_kv_t *)realloc(t->kv, (size_t)cap * sizeof *n);
        if (!n) return;
        t->kv = n; t->cap = cap;
    }
    rs_kv_t *e = &t->kv[t->n++];
    snprintf(e->key, sizeof e->key, "%s", key);
    snprintf(e->val, sizeof e->val, "%s", val);
    snprintf(e->base, sizeof e->base, "%s", base);
}

static inline const rs_kv_t *rs_kvs_get(const rs_kvs_t *t, const char *key) {
    for (int i = 0; i < t->n; i++) if (strcmp(t->kv[i].key, key) == 0) return &t->kv[i];
    return NULL;
}

static inline const char *rs_kvs_str(const rs_kvs_t *t, const char *key) {
    const rs_kv_t *e = rs_kvs_get(t, key);
    return e ? e->val : NULL;
}

static inline char *rs_trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

/* A value with its quotes off and any trailing comment gone. */
static inline char *rs_unquote(char *v) {
    v = rs_trim(v);
    if (*v == '"') {
        char *e = strchr(v + 1, '"');
        if (e) *e = '\0';
        return v + 1;
    }
    char *hash = strchr(v, '#');
    if (hash) *hash = '\0';
    return rs_trim(v);
}

static inline bool rs_parse_file(rs_kvs_t *t, const char *path, int depth, char *err, size_t errlen) {
    if (depth > 8) { snprintf(err, errlen, "%s: #reference nests too deeply", rs_path_base(path)); return false; }
    uint8_t *data; size_t len;
    if (!rs_read(path, &data, &len)) { snprintf(err, errlen, "Could not read %s", path); return false; }
    char base[RS_PATH];
    rs_path_dir(path, base, sizeof base);
    char *text = (char *)data, *line = text;
    bool ok = true;
    while (line && *line && ok) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) { *nl = '\0'; nl++; while (*nl == '\r' || *nl == '\n') nl++; }
        char *l = rs_trim(line);
        if (strncmp(l, "#reference", 10) == 0) {
            char ref[RS_PATH];
            rs_path_join(base, rs_unquote(l + 10), ref, sizeof ref);
            ok = rs_parse_file(t, ref, depth + 1, err, errlen);
        } else if (*l && *l != '#' && !(l[0] == '/' && l[1] == '/')) {
            char *eq = strchr(l, '=');
            if (eq) {
                *eq = '\0';
                rs_kvs_set(t, rs_trim(l), rs_unquote(eq + 1), base);
            }
        }
        line = nl;
    }
    free(data);
    return ok;
}

static inline bool rs_bool(const char *v) {
    return v && (!strcmp(v, "true") || !strcmp(v, "1") || !strcmp(v, "TRUE") || !strcmp(v, "True"));
}

static inline rs_wrap_t rs_wrap(const char *v) {
    if (!v) return RS_WRAP_BORDER;   /* RetroArch's default */
    if (!strcmp(v, "clamp_to_edge"))   return RS_WRAP_EDGE;
    if (!strcmp(v, "repeat"))          return RS_WRAP_REPEAT;
    if (!strcmp(v, "mirrored_repeat")) return RS_WRAP_MIRROR;
    return RS_WRAP_BORDER;
}

static inline bool rs_scale_type(const char *v, rs_scale_type_t *out) {
    if (!v) return false;
    if (!strcmp(v, "source"))   { *out = RS_SCALE_SOURCE;   return true; }
    if (!strcmp(v, "viewport")) { *out = RS_SCALE_VIEWPORT; return true; }
    if (!strcmp(v, "absolute")) { *out = RS_SCALE_ABSOLUTE; return true; }
    return false;
}

/* `#pragma parameter NAME "Description" initial min max [step]`, from every pass.
 * A name seen in an earlier pass is not added again. */
static inline void rs_scan_params(rs_preset_t *p, const char *src) {
    const char *s = src;
    while ((s = strstr(s, "#pragma parameter")) != NULL) {
        s += 17;
        const char *e = strpbrk(s, "\r\n");
        size_t n = e ? (size_t)(e - s) : strlen(s);
        char line[320];
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, s, n); line[n] = '\0';
        char name[RS_NAME] = "", desc[96] = "";
        char *c = rs_trim(line);
        int k = 0;
        while (*c && !isspace((unsigned char)*c) && k < RS_NAME - 1) name[k++] = *c++;
        name[k] = '\0';
        c = rs_trim(c);
        if (*c == '"') {
            c++;
            char *q = strchr(c, '"');
            if (!q) continue;
            size_t dl = (size_t)(q - c);
            if (dl >= sizeof desc) dl = sizeof desc - 1;
            memcpy(desc, c, dl); desc[dl] = '\0';
            c = q + 1;
        }
        float v[4] = { 0, 0, 1, 0 };
        int got = sscanf(c, "%f %f %f %f", &v[0], &v[1], &v[2], &v[3]);
        if (!name[0] || got < 3) continue;
        bool dup = false;
        for (int i = 0; i < p->n_params; i++) if (!strcmp(p->param[i].name, name)) dup = true;
        if (dup || p->n_params >= RS_MAX_PARAMS) continue;
        rs_param_t *pr = &p->param[p->n_params++];
        snprintf(pr->name, sizeof pr->name, "%s", name);
        snprintf(pr->desc, sizeof pr->desc, "%s", desc[0] ? desc : name);
        pr->initial = pr->value = v[0];
        pr->min = v[1]; pr->max = v[2];
        pr->step = got >= 4 && v[3] > 0.0f ? v[3] : (v[2] - v[1]) / 100.0f;
    }
}

static inline void rs_preset_free(rs_preset_t *p) {
    for (int i = 0; i < p->n_passes; i++) free(p->pass[i].source);
    for (int i = 0; i < p->n_luts; i++) stbi_image_free(p->lut[i].rgba);
    memset(p, 0, sizeof *p);
}

static inline bool rs_load_pass_source(rs_pass_t *ps, char *err, size_t errlen) {
    uint8_t *data; size_t len;
    if (!rs_read(ps->path, &data, &len)) {
        snprintf(err, errlen, "Could not read the shader %s. Add it with the preset.", ps->path);
        return false;
    }
    if (strstr((const char *)data, "#pragma stage")) {
        free(data);
        snprintf(err, errlen, "%s is a slang shader. Use the GLSL version of it.", rs_path_base(ps->path));
        return false;
    }
    ps->source = (char *)data;
    return true;
}

static inline bool rs_load_lut(rs_lut_t *l, char *err, size_t errlen) {
    uint8_t *data; size_t len;
    if (!rs_read(l->path, &data, &len)) {
        snprintf(err, errlen, "Could not read the texture %s (%s). Add it with the preset.", l->path, l->name);
        return false;
    }
    int w, h, comp;
    l->rgba = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 4);
    free(data);
    if (!l->rgba) {
        snprintf(err, errlen, "Could not decode the texture %s: %s", l->path, stbi_failure_reason());
        return false;
    }
    l->w = w; l->h = h;
    return true;
}

/*
 * Load a .glslp (or one .glsl as a single pass). On failure `err` says why in a
 * sentence a player can act on, and `p` is left empty.
 */
static inline bool rs_preset_load(rs_preset_t *p, const char *path, char *err, size_t errlen) {
    memset(p, 0, sizeof *p);
    err[0] = '\0';
    snprintf(p->name, sizeof p->name, "%s", rs_path_base(path));
    for (int i = 0; i < g_rs_vfs_n; i++) g_rs_vfs[i].used = false;

    if (rs_ext_is(path, ".slangp") || rs_ext_is(path, ".slang")) {
        snprintf(err, errlen, "Slang shaders (.slang/.slangp) need a shader compiler this build doesn't have. "
                              "Use the GLSL version (.glslp/.glsl) from libretro's glsl-shaders.");
        return false;
    }
    if (rs_ext_is(path, ".cgp") || rs_ext_is(path, ".cg")) {
        snprintf(err, errlen, "Cg shaders (.cg/.cgp) are not supported. Use the GLSL version (.glslp/.glsl).");
        return false;
    }
    if (rs_ext_is(path, ".glsl")) {
        rs_pass_t *ps = &p->pass[0];
        snprintf(ps->path, sizeof ps->path, "%s", path);
        rs_path_normalize(ps->path);
        ps->wrap = RS_WRAP_BORDER;
        ps->type_x = ps->type_y = RS_SCALE_VIEWPORT;
        ps->scale_x = ps->scale_y = 1.0f;
        if (!rs_load_pass_source(ps, err, errlen)) { rs_preset_free(p); return false; }
        p->n_passes = 1;
        rs_scan_params(p, ps->source);
        return true;
    }
    if (!rs_ext_is(path, ".glslp")) {
        snprintf(err, errlen, "Pick a .glslp preset or a .glsl shader.");
        return false;
    }

    rs_kvs_t t = { 0 };
    char norm[RS_PATH];
    snprintf(norm, sizeof norm, "%s", path);
    rs_path_normalize(norm);
    if (!rs_parse_file(&t, norm, 0, err, errlen)) { free(t.kv); return false; }

    const char *nstr = rs_kvs_str(&t, "shaders");
    int n = nstr ? atoi(nstr) : 0;
    if (n < 1 || n > RS_MAX_PASSES) {
        snprintf(err, errlen, "%s names %d shader passes (1 to %d work).", p->name, n, RS_MAX_PASSES);
        free(t.kv);
        return false;
    }
    char key[96];
    for (int i = 0; i < n; i++) {
        rs_pass_t *ps = &p->pass[i];
        snprintf(key, sizeof key, "shader%d", i);
        const rs_kv_t *sh = rs_kvs_get(&t, key);
        if (!sh) { snprintf(err, errlen, "%s has no %s.", p->name, key); goto fail; }
        rs_path_join(sh->base, sh->val, ps->path, sizeof ps->path);
        #define RS_KEY(fmt) (snprintf(key, sizeof key, fmt, i), rs_kvs_str(&t, key))
        const char *v;
        if ((v = RS_KEY("filter_linear%d")) != NULL) { ps->filter_set = true; ps->filter_linear = rs_bool(v); }
        v = RS_KEY("wrap_mode%d");
        if (!v) v = RS_KEY("texture_wrap_mode%d");
        ps->wrap = rs_wrap(v);
        ps->type_x = ps->type_y = RS_SCALE_SOURCE;
        ps->scale_x = ps->scale_y = 1.0f;
        rs_scale_type_t st;
        if (rs_scale_type(RS_KEY("scale_type%d"), &st))   { ps->type_x = ps->type_y = st; ps->scale_set = true; }
        if (rs_scale_type(RS_KEY("scale_type_x%d"), &st)) { ps->type_x = st; ps->scale_set = true; }
        if (rs_scale_type(RS_KEY("scale_type_y%d"), &st)) { ps->type_y = st; ps->scale_set = true; }
        if ((v = RS_KEY("scale%d")) != NULL)   ps->scale_x = ps->scale_y = (float)atof(v);
        if ((v = RS_KEY("scale_x%d")) != NULL) ps->scale_x = (float)atof(v);
        if ((v = RS_KEY("scale_y%d")) != NULL) ps->scale_y = (float)atof(v);
        ps->float_fb     = rs_bool(RS_KEY("float_framebuffer%d"));
        ps->srgb_fb      = rs_bool(RS_KEY("srgb_framebuffer%d"));
        ps->mipmap_input = rs_bool(RS_KEY("mipmap_input%d"));
        if ((v = RS_KEY("frame_count_mod%d")) != NULL) ps->frame_count_mod = (unsigned)atoi(v);
        if ((v = RS_KEY("alias%d")) != NULL) snprintf(ps->alias, sizeof ps->alias, "%s", v);
        #undef RS_KEY
        /* The last pass without a scale of its own draws straight to the screen. */
        if (i == n - 1 && !ps->scale_set) { ps->type_x = ps->type_y = RS_SCALE_VIEWPORT; }
        if (!rs_load_pass_source(ps, err, errlen)) goto fail;
        p->n_passes = i + 1;
        rs_scan_params(p, ps->source);
    }

    const char *tex = rs_kvs_str(&t, "textures");
    if (tex && *tex) {
        char list[RS_PATH];
        snprintf(list, sizeof list, "%s", tex);
        for (char *name = strtok(list, ";"); name; name = strtok(NULL, ";")) {
            name = rs_trim(name);
            if (!*name) continue;
            if (p->n_luts >= RS_MAX_LUTS) { snprintf(err, errlen, "%s has more than %d textures.", p->name, RS_MAX_LUTS); goto fail; }
            rs_lut_t *l = &p->lut[p->n_luts];
            snprintf(l->name, sizeof l->name, "%s", name);
            const rs_kv_t *e = rs_kvs_get(&t, name);
            if (!e) { snprintf(err, errlen, "%s names texture %s but gives no file for it.", p->name, name); goto fail; }
            rs_path_join(e->base, e->val, l->path, sizeof l->path);
            snprintf(key, sizeof key, "%s_linear", name);
            l->linear = rs_bool(rs_kvs_str(&t, key));
            snprintf(key, sizeof key, "%s_mipmap", name);
            l->mipmap = rs_bool(rs_kvs_str(&t, key));
            snprintf(key, sizeof key, "%s_wrap_mode", name);
            l->wrap = rs_wrap(rs_kvs_str(&t, key));
            if (!rs_load_lut(l, err, errlen)) goto fail;
            p->n_luts++;
        }
    }

    /* A preset overrides a parameter by naming it, listed in "parameters" or not. */
    for (int i = 0; i < p->n_params; i++) {
        const char *v = rs_kvs_str(&t, p->param[i].name);
        if (v) p->param[i].value = p->param[i].initial = (float)atof(v);
    }
    free(t.kv);
    return true;

fail:
    free(t.kv);
    rs_preset_free(p);
    return false;
}

static inline rs_param_t *rs_preset_param(rs_preset_t *p, const char *name) {
    for (int i = 0; i < p->n_params; i++) if (!strcmp(p->param[i].name, name)) return &p->param[i];
    return NULL;
}

/* ---- Global initialisers GLSL ES will not take ------------------------------------
 *
 * Desktop GLSL lets a global be initialised from a uniform or a function call
 * (`float x = PARAM * 2.0;`); GLSL ES, and so WebGL, allows only constant
 * expressions there. crt-royale, mame_hlsl and others are written that way. The
 * compiler names each offending line; each is rewritten as a plain declaration
 * and an assignment at the top of main():
 *
 *     float x = PARAM * 2.0;      ->   float x;
 *                                      #define RS_INIT_3 x = PARAM * 2.0;
 *     void main() {               ->   void main() {
 *                                      #ifdef RS_INIT_3
 *                                      RS_INIT_3
 *                                      #endif
 *                                      (a marker comment for the next round)
 *
 * The macro is defined where the declaration was, so a declaration inside an
 * #if branch that is not taken leaves no assignment behind. A `const` that
 * loses its initialiser loses the const too; whatever was initialised from it
 * is then named by the next compile and moved the same way.
 */

typedef struct { size_t start, end; } rs_span_t;

/* Statements at file scope, [start, end) with the ';', skipping comments,
 * preprocessor lines and the bodies of functions and structs. */
static inline int rs_glsl_globals(const char *s, rs_span_t *out, int cap) {
    const size_t n = strlen(s);
    size_t i = 0, st = (size_t)-1;
    int count = 0, depth = 0;
    bool line_start = true;
    while (i < n) {
        const char c = s[i];
        if (c == '/' && s[i + 1] == '/') { while (i < n && s[i] != '\n') i++; continue; }
        if (c == '/' && s[i + 1] == '*') {
            i += 2;
            while (i < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i = i < n ? i + 2 : n;
            continue;
        }
        if (c == '\n') { line_start = true; i++; continue; }
        if (line_start && (c == ' ' || c == '\t' || c == '\r')) { i++; continue; }
        if (line_start && c == '#') {
            while (i < n && s[i] != '\n') {
                if (s[i] == '\\' && s[i + 1] == '\n') i++;
                else if (s[i] == '\\' && s[i + 1] == '\r' && s[i + 2] == '\n') i += 2;
                i++;
            }
            continue;
        }
        line_start = false;
        if (depth == 0) {
            if (st == (size_t)-1 && !isspace((unsigned char)c)) st = i;
            if (c == ';') {
                if (st != (size_t)-1 && count < cap) { out[count].start = st; out[count].end = i + 1; count++; }
                st = (size_t)-1;
            } else if (c == '{') {
                depth = 1;
            }
        } else if (c == '{') {
            depth++;
        } else if (c == '}' && --depth == 0) {
            st = (size_t)-1;
        }
        i++;
    }
    return count;
}

static inline bool rs_word_at(const char *s, size_t i, const char *w) {
    size_t k = strlen(w);
    if (i > 0 && (isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_')) return false;
    return !strncmp(s + i, w, k) && !(isalnum((unsigned char)s[i + k]) || s[i + k] == '_');
}

/* One statement's replacement, or NULL to leave it alone. */
static inline char *rs_hoist_one(const char *stmt, size_t len, int k) {
    /* The initialiser's '=': the first at bracket depth 0 that is not ==, <=, >=, !=. */
    size_t eq = (size_t)-1;
    int depth = 0;
    for (size_t i = 0; i < len; i++) {
        char c = stmt[i];
        if (c == '(' || c == '[') depth++;
        else if (c == ')' || c == ']') depth--;
        else if (c == ',' && depth == 0) return NULL;     /* more than one declarator */
        else if (c == '\n' && eq == (size_t)-1) {
            size_t j = i + 1;
            while (j < len && (stmt[j] == ' ' || stmt[j] == '\t')) j++;
            if (j < len && stmt[j] == '#') return NULL;    /* a directive inside the statement */
        }
        else if (c == '=' && depth == 0 && eq == (size_t)-1 && i + 1 < len && stmt[i + 1] != '=' &&
                 (i == 0 || (stmt[i - 1] != '<' && stmt[i - 1] != '>' && stmt[i - 1] != '!' && stmt[i - 1] != '=')))
            eq = i;
    }
    if (eq == (size_t)-1) return NULL;
    for (size_t i = eq; i < len; i++) {                   /* nor after the '=' */
        if (stmt[i] != '\n') continue;
        size_t j = i + 1;
        while (j < len && (stmt[j] == ' ' || stmt[j] == '\t')) j++;
        if (j < len && stmt[j] == '#') return NULL;
    }
    static const char *const keep_out[] = { "uniform", "in", "out", "attribute", "varying", "layout",
                                            "precision", "struct", "inout", "buffer", "shared" };
    for (size_t i = 0; i < eq; i++)
        for (size_t w = 0; w < sizeof keep_out / sizeof keep_out[0]; w++)
            if (rs_word_at(stmt, i, keep_out[w])) return NULL;

    /* The declaration without const or static, and the name it declares. */
    char *out = (char *)malloc(len * 2 + 128);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < eq; ) {
        if (rs_word_at(stmt, i, "const"))  { i += 5; continue; }
        if (rs_word_at(stmt, i, "static")) { i += 6; continue; }
        out[o++] = stmt[i++];
    }
    while (o > 0 && isspace((unsigned char)out[o - 1])) o--;
    size_t e = o;
    if (e > 0 && out[e - 1] == ']') {                     /* an array: the name is before its size */
        int d = 0;
        while (e > 0) { char c = out[--e]; if (c == ']') d++; else if (c == '[' && --d == 0) break; }
        while (e > 0 && isspace((unsigned char)out[e - 1])) e--;
    }
    size_t nb = e;
    while (nb > 0 && (isalnum((unsigned char)out[nb - 1]) || out[nb - 1] == '_')) nb--;
    if (nb == e) { free(out); return NULL; }
    char name[RS_NAME];
    size_t nl_ = e - nb < sizeof name - 1 ? e - nb : sizeof name - 1;
    memcpy(name, out + nb, nl_); name[nl_] = '\0';

    o += (size_t)sprintf(out + o, ";\n#define RS_INIT_%d %s = ", k, name);
    /* The initialiser on one line: line comments cut, line breaks made spaces. */
    for (size_t i = eq + 1; i < len; i++) {
        if (stmt[i] == '/' && i + 1 < len && stmt[i + 1] == '/') { while (i < len && stmt[i] != '\n') i++; out[o++] = ' '; continue; }
        out[o++] = stmt[i] == '\n' || stmt[i] == '\r' ? ' ' : stmt[i];
    }
    o += (size_t)sprintf(out + o, ";\n");
    out[o] = '\0';
    return out;
}

/* `lines` are 1-based lines of `text` the compiler rejected. Returns the new
 * text, or NULL when none of them was a statement this can move. *counter
 * numbers the macros across calls. */
static inline char *rs_hoist_globals(const char *text, const int *lines, int n_lines, int *counter) {
    enum { MAX_GLOBALS = 4096 };
    rs_span_t *spans = (rs_span_t *)malloc(MAX_GLOBALS * sizeof *spans);
    if (!spans) return NULL;
    int n_spans = rs_glsl_globals(text, spans, MAX_GLOBALS);
    /* Which statements the lines fall in, in source order, each once. */
    bool *pick = (bool *)calloc((size_t)(n_spans ? n_spans : 1), sizeof *pick);
    if (!pick) { free(spans); return NULL; }
    for (int l = 0; l < n_lines; l++) {
        size_t off = 0;
        for (int line = 1; line < lines[l] && text[off]; off++) if (text[off] == '\n') line++;
        size_t le = off;
        while (text[le] && text[le] != '\n') le++;
        for (int g = 0; g < n_spans; g++)
            if (spans[g].start <= le && spans[g].end > off) { pick[g] = true; break; }
    }
    size_t len = strlen(text), cap = len * 2 + 4096, o = 0;
    char *out = (char *)malloc(cap);
    int first = *counter, made = 0;
    size_t at = 0;
    for (int g = 0; out && g < n_spans; g++) {
        if (!pick[g]) continue;
        char *rep_ = rs_hoist_one(text + spans[g].start, spans[g].end - 1 - spans[g].start, *counter);
        if (!rep_) continue;
        size_t pre = spans[g].start - at, rl = strlen(rep_);
        if (o + pre + rl + 1 > cap) { cap = (o + pre + rl + 1) * 2; char *gr = (char *)realloc(out, cap); if (!gr) { free(out); out = NULL; free(rep_); break; } out = gr; }
        memcpy(out + o, text + at, pre); o += pre;
        memcpy(out + o, rep_, rl); o += rl;
        at = spans[g].end;
        free(rep_);
        (*counter)++;
        made++;
    }
    free(spans);
    free(pick);
    if (!out || !made) { free(out); return NULL; }
    if (o + (len - at) + 1 > cap) { cap = o + (len - at) + 1; char *gr = (char *)realloc(out, cap); if (!gr) { free(out); return NULL; } out = gr; }
    memcpy(out + o, text + at, len - at); o += len - at;
    out[o] = '\0';

    /* The assignments, at the top of every main(), after any from earlier calls. */
    size_t blk_cap = (size_t)made * 64 + 32, b = 0;
    char *blk = (char *)malloc(blk_cap);
    if (!blk) { free(out); return NULL; }
    for (int k = first; k < first + made; k++)
        b += (size_t)snprintf(blk + b, blk_cap - b, "\n#ifdef RS_INIT_%d\nRS_INIT_%d\n#endif", k, k);
    static const char marker[] = "/*rs_inits*/";
    bool has_marker = strstr(out, marker) != NULL;
    size_t total = strlen(out);
    size_t cap2 = total + 1 + (b + sizeof marker + 8) * 8;
    char *res = (char *)malloc(cap2);
    if (!res) { free(blk); free(out); return NULL; }
    size_t r = 0;
    for (size_t i = 0; i < total; ) {
        if (has_marker && !strncmp(out + i, marker, sizeof marker - 1)) {
            if (r + b + sizeof marker + 2 > cap2) break;
            memcpy(res + r, blk + 1, b - 1); r += b - 1;   /* the marker already starts a line */
            res[r++] = '\n';
            memcpy(res + r, marker, sizeof marker - 1); r += sizeof marker - 1;
            i += sizeof marker - 1;
            continue;
        }
        if (!has_marker && rs_word_at(out, i, "main")) {
            size_t j = i + 4;
            while (isspace((unsigned char)out[j])) j++;
            if (out[j] == '(') {
                while (out[j] && out[j] != ')') j++;
                if (out[j] == ')') j++;
                while (isspace((unsigned char)out[j])) j++;
                if (out[j] == '{' && r + (j + 1 - i) + b + sizeof marker + 2 <= cap2) {
                    memcpy(res + r, out + i, j + 1 - i); r += j + 1 - i;
                    memcpy(res + r, blk, b); r += b;
                    res[r++] = '\n';
                    memcpy(res + r, marker, sizeof marker - 1); r += sizeof marker - 1;
                    i = j + 1;
                    continue;
                }
            }
        }
        if (r + 2 > cap2) break;
        res[r++] = out[i++];
    }
    res[r] = '\0';
    free(blk);
    free(out);
    return res;
}

/* The lines an ES compiler rejected for a non-constant global initialiser, as
 * lines of the body: `head` lines of prologue came before it. ANGLE and Mesa
 * both say "ERROR: 0:<line>: ... global variable initializers must be constant". */
static inline int rs_glsl_global_init_errors(const char *log, int head, int *lines, int cap) {
    int n = 0;
    for (const char *l = log; l && *l && n < cap; ) {
        const char *e = strchr(l, '\n');
        size_t len = e ? (size_t)(e - l) : strlen(l);
        char line[512];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, l, len); line[len] = '\0';
        int src = 0, ln = 0;
        const char *er = strstr(line, "ERROR:");
        if (er && strstr(line, "global variable initializers") && sscanf(er, "ERROR: %d:%d", &src, &ln) == 2 && ln > head)
            lines[n++] = ln - head;
        l = e ? e + 1 : NULL;
    }
    return n;
}

/* ---- The runner (GL) -------------------------------------------------------------- */

#if (defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)) && !defined(_WIN32) && !defined(RS_NO_GL)
#define RS_HAVE_GL 1

#if defined(__EMSCRIPTEN__) || defined(SOKOL_GLES3)
#include <GLES3/gl3.h>
#define RS_GLES 1
#else
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#include <GL/gl.h>
#include <GL/glext.h>
#endif

/* What a sampler uniform reads. */
enum { RS_SRC_INPUT, RS_SRC_ORIG, RS_SRC_PASS, RS_SRC_PREV, RS_SRC_LUT };

typedef struct {
    uint8_t kind, index, unit;
    GLint   loc_tex_size, loc_input_size;
} rs_bind_t;

#define RS_MAX_BINDS 16

typedef struct {
    GLuint    prog;
    GLint     a_pos, a_color;
    GLint     a_tex[8];
    int       n_tex_attr;
    GLint     u_mvp, u_frame_count, u_frame_dir, u_output_size;
    GLenum    t_frame_count, t_frame_dir;
    rs_bind_t bind[RS_MAX_BINDS];
    int       n_bind;
    GLint     u_param[RS_MAX_PARAMS];
    /* The pass's output. The last pass, drawn to the screen, has none. */
    GLuint    fbo, tex;
    int       w, h;
    bool      mips;          /* the NEXT pass reads this with mipmap_input */
    GLenum    ifmt;
} rs_gl_pass_t;

typedef struct { GLuint tex, fbo; int w, h; bool mips; } rs_gl_target_t;

typedef struct {
    bool           ready;
    rs_preset_t   *preset;
    int            n;                      /* passes, including a stock blit when the last is scaled */
    bool           stock_tail;
    rs_gl_pass_t   pass[RS_MAX_PASSES + 1];
    GLuint         lut_tex[RS_MAX_LUTS];
    GLuint         vao, vbo, read_fbo;
    GLuint         samplers[2][4][2];      /* [linear][wrap][mip] */
    rs_gl_target_t orig;
    rs_gl_target_t hist[RS_MAX_HISTORY];
    int            history, hist_head, hist_filled;
    uint32_t       frame;
    int            src_w, src_h, vp_w, vp_h;
    bool           float_ok;
    char           err[RS_ERR];
} rs_gl_t;

static rs_gl_t g_rs_gl;

/* RetroArch's stock pass, for a preset whose last pass has a scale of its own. */
static const char rs_stock_glsl[] =
    "#if defined(VERTEX)\n"
    "#if __VERSION__ >= 130\n#define COMPAT_VARYING out\n#define COMPAT_ATTRIBUTE in\n"
    "#else\n#define COMPAT_VARYING varying\n#define COMPAT_ATTRIBUTE attribute\n#endif\n"
    "COMPAT_ATTRIBUTE vec4 VertexCoord;\nCOMPAT_ATTRIBUTE vec4 TexCoord;\n"
    "COMPAT_VARYING vec4 TEX0;\nuniform mat4 MVPMatrix;\n"
    "void main() { gl_Position = MVPMatrix * VertexCoord; TEX0 = TexCoord; }\n"
    "#elif defined(FRAGMENT)\n"
    "#if __VERSION__ >= 130\n#define COMPAT_VARYING in\n#define COMPAT_TEXTURE texture\nout vec4 FragColor;\n"
    "#else\n#define COMPAT_VARYING varying\n#define FragColor gl_FragColor\n#define COMPAT_TEXTURE texture2D\n#endif\n"
    "#ifdef GL_ES\nprecision mediump float;\n#endif\n"
    "uniform sampler2D Texture;\nCOMPAT_VARYING vec4 TEX0;\n"
    "void main() { FragColor = COMPAT_TEXTURE(Texture, TEX0.xy); }\n"
    "#endif\n";

static inline void rs_gl_err(const char *fmt, const char *a, const char *b) {
    snprintf(g_rs_gl.err, sizeof g_rs_gl.err, fmt, a, b);
}

/* The source with its own #version taken out (returned in `ver`) and its
 * parameter pragmas blanked, which some GLSL ES compilers reject. */
static inline char *rs_gl_strip(const char *src, char *ver, size_t vercap) {
    size_t n = strlen(src);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, src, n + 1);
    ver[0] = '\0';
    char *v = strstr(out, "#version");
    if (v && (v == out || v[-1] == '\n' || v[-1] == '\r' || v[-1] == ' ' || v[-1] == '\t')) {
        char *e = v + 8;
        while (*e && *e != '\n' && *e != '\r') e++;
        size_t len = (size_t)(e - (v + 8));
        char tmp[64];
        if (len >= sizeof tmp) len = sizeof tmp - 1;
        memcpy(tmp, v + 8, len); tmp[len] = '\0';
        snprintf(ver, vercap, "%s", rs_trim(tmp));
        memset(v, ' ', (size_t)(e - v));
    }
    for (char *p = out; (p = strstr(p, "#pragma parameter")) != NULL; ) {
        while (*p && *p != '\n' && *p != '\r') *p++ = ' ';
    }
    return out;
}

/* Lines of prologue rs_gl_compile puts ahead of a body. */
static inline int rs_gl_head_lines(const char *body, bool es) {
    return 3 + (es && !strstr(body, "#extension") ? 2 : 0);
}

static inline GLuint rs_gl_compile(GLenum stage, const char *version, const char *body, bool es, char *log, size_t logcap) {
    char head[256];
    bool has_ext = strstr(body, "#extension") != NULL;
    snprintf(head, sizeof head, "#version %s\n#define %s\n#define PARAMETER_UNIFORM\n%s",
             version, stage == GL_VERTEX_SHADER ? "VERTEX" : "FRAGMENT",
             es && !has_ext ? "precision highp float;\nprecision highp int;\n" : "");
    const char *parts[2] = { head, body };
    GLuint sh = glCreateShader(stage);
    glShaderSource(sh, 2, parts, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLsizei got = 0;
        glGetShaderInfoLog(sh, (GLsizei)logcap, &got, log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/* `src` with whole-word tokens replaced: from[i] becomes to[i]. */
static inline char *rs_gl_rewrite(const char *src, const char *const *from, const char *const *to, int n) {
    size_t len = strlen(src), cap = len + len / 2 + 64, o = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    for (size_t i = 0; i < len; ) {
        const bool edge = i == 0 || !(isalnum((unsigned char)src[i - 1]) || src[i - 1] == '_');
        int hit = -1;
        size_t k = 0;
        for (int t = 0; edge && t < n && hit < 0; t++) {
            k = strlen(from[t]);
            if (!strncmp(src + i, from[t], k) && !(isalnum((unsigned char)src[i + k]) || src[i + k] == '_')) hit = t;
        }
        const char *put = hit >= 0 ? to[hit] : NULL;
        size_t need = put ? strlen(put) : 1;
        if (o + need + 1 >= cap) {
            cap = cap * 2 + need;
            char *g = (char *)realloc(out, cap);
            if (!g) { free(out); return NULL; }
            out = g;
        }
        if (put) { memcpy(out + o, put, need); o += need; i += k; }
        else out[o++] = src[i++];
    }
    out[o] = '\0';
    return out;
}

/* How an attempt treats the source. */
enum {
    RS_AS_IS,
    /* Every lowp and mediump made highp. For a shader that declares a uniform at
     * one precision in its vertex stage and another in its fragment stage: GLSL
     * ES says that cannot link, WebGL enforces it, and many phone drivers let it
     * pass, which is how such shaders got into libretro's collection
     * (zfast_crt_geo is one). */
    RS_AS_HIGHP,
    /* GLSL 1.10 spelled as ES 3.00: attribute/varying/texture2D/gl_FragColor.
     * WebGL2 compiles ES 1.00 too, but gives it no derivatives (fwidth, dFdx),
     * which old shaders use. A shader can be both: gizmo-crt declares its
     * output through the compat macros (FragColor) and then writes gl_FragColor
     * once, so that write goes to the shader's own output. */
    RS_AS_LEGACY,
};

/* The shader declares an ES 3.00 output of its own under that name. */
static inline bool rs_gl_has_fragcolor(const char *body) {
    return strstr(body, "vec4 FragColor;") != NULL;
}

static inline char *rs_gl_stage_source(const char *body, int how, bool fragment) {
    static const char *const hp_from[] = { "lowp", "mediump" };
    static const char *const hp_to[]   = { "highp", "highp" };
    static const char *const lv_from[] = { "attribute", "varying", "texture2D", "texture2DProj", "texture2DLod", "highp", "mediump", "lowp" };
    static const char *const lv_to[]   = { "in", "out", "texture", "textureProj", "textureLod", "highp", "highp", "highp" };
    static const char *const lf_from[] = { "varying", "gl_FragColor", "texture2D", "texture2DProj", "texture2DLod", "highp", "mediump", "lowp" };
    static const char *const lf_to[]   = { "in", "rs_FragColor", "texture", "textureProj", "textureLod", "highp", "highp", "highp" };
    static const char *const lo_to[]   = { "in", "FragColor", "texture", "textureProj", "textureLod", "highp", "highp", "highp" };
    if (how == RS_AS_HIGHP) return rs_gl_rewrite(body, hp_from, hp_to, 2);
    if (how == RS_AS_LEGACY && !fragment) return rs_gl_rewrite(body, lv_from, lv_to, 8);
    if (how == RS_AS_LEGACY) return rs_gl_rewrite(body, lf_from, rs_gl_has_fragcolor(body) ? lo_to : lf_to, 8);
    size_t n = strlen(body);
    char *out = (char *)malloc(n + 1);
    if (out) memcpy(out, body, n + 1);
    return out;
}

/* Compile and link one pass, trying GLSL versions until one takes: the shader's
 * own first, then the ones libretro's shaders are written for. When none takes,
 * the error reported is the first attempt's, which is the version the shader
 * was written for; a later fallback's complaints are about the fallback. */
static inline GLuint rs_gl_program(const char *src, const char *name) {
    char ver[64], log[1024] = "", first[1024] = "";
    const char *first_ver = NULL;
    int first_hoisted = 0;
    char *body = rs_gl_strip(src, ver, sizeof ver);
    if (!body) return 0;
#ifdef RS_GLES
    const bool es = true;
    const char *tries[6] = { NULL, "300 es", "300 es", "300 es", "100", "100" };
    const int   how[6]   = { RS_AS_IS, RS_AS_IS, RS_AS_HIGHP, RS_AS_LEGACY, RS_AS_IS, RS_AS_HIGHP };
    if (strstr(ver, "es") || !strcmp(ver, "100")) tries[0] = ver;
    /* A legacy rewrite declares its output ahead of the body, where an
     * #extension line would then be out of place. */
    const bool has_ext = strstr(body, "#extension") != NULL;
#else
    const bool es = false;
    const char *tries[6] = { NULL, "130", "140", "330", "120", NULL };
    const int   how[6]   = { RS_AS_IS };
    if (ver[0] && !strstr(ver, "es") && strcmp(ver, "100")) tries[0] = ver;
    const bool has_ext = false;
#endif
    GLuint prog = 0;
    int hoisted = 0;
    for (size_t t = 0, round = 0; t < sizeof tries / sizeof tries[0] && !prog; ) {
        if (!tries[t] || (how[t] == RS_AS_LEGACY && has_ext)) { t++; continue; }
        char *vtext = rs_gl_stage_source(body, how[t], false);
        char *ftext = rs_gl_stage_source(body, how[t], true);
        char *fdecl = NULL;
        if (ftext && how[t] == RS_AS_LEGACY && !rs_gl_has_fragcolor(body)) {
            size_t n = strlen(ftext);
            static const char decl[] = "out highp vec4 rs_FragColor;\n";
            fdecl = (char *)malloc(n + sizeof decl);
            if (fdecl) { memcpy(fdecl, decl, sizeof decl - 1); memcpy(fdecl + sizeof decl - 1, ftext, n + 1); }
            free(ftext);
            ftext = fdecl;
        }
        log[0] = '\0';
        GLuint vs = vtext ? rs_gl_compile(GL_VERTEX_SHADER, tries[t], vtext, es, log, sizeof log) : 0;
        GLuint fs = vs && ftext ? rs_gl_compile(GL_FRAGMENT_SHADER, tries[t], ftext, es, log, sizeof log) : 0;
        free(vtext);
        free(ftext);
        if (vs && fs) {
            GLuint p = glCreateProgram();
            glAttachShader(p, vs);
            glAttachShader(p, fs);
            glLinkProgram(p);
            GLint ok = 0;
            glGetProgramiv(p, GL_LINK_STATUS, &ok);
            if (ok) {
                LOG_INFO("shader: %s compiled as GLSL %s%s", name, tries[t],
                         how[t] == RS_AS_HIGHP ? ", all highp" : how[t] == RS_AS_LEGACY ? ", rewritten from GLSL 1.10" : "");
                prog = p;
            } else {
                GLsizei got = 0;
                glGetProgramInfoLog(p, (GLsizei)sizeof log, &got, log);
                glDeleteProgram(p);
            }
        }
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        /* Globals GLSL ES will not initialise: move them into main() and try
         * the same version again, a few times, as each move can expose the next. */
        if (!prog && es && how[t] != RS_AS_LEGACY && round < 12) {
            int lines[128];
            int n = rs_glsl_global_init_errors(log, rs_gl_head_lines(body, es), lines, 128);
            char *next = n ? rs_hoist_globals(body, lines, n, &hoisted) : NULL;
            if (next) {
                free(body);
                body = next;
                round++;
                continue;
            }
        }
        /* The error to report is where the first attempt ended up, moves and all. */
        if (!prog && !first_ver) { first_ver = tries[t]; first_hoisted = hoisted; snprintf(first, sizeof first, "%s", log); }
        round = 0;
        t++;
    }
    if (prog && hoisted) LOG_INFO("shader: %s: %d global initialiser%s moved into main()", name, hoisted, hoisted == 1 ? "" : "s");
    free(body);
    if (!prog) {
        char what[192];
        if (first_hoisted)
            snprintf(what, sizeof what, "%s did not compile (as GLSL %s, with %d global initialiser%s moved into main())",
                     name, first_ver, first_hoisted, first_hoisted == 1 ? "" : "s");
        else
            snprintf(what, sizeof what, "%s did not compile (as GLSL %s)", name, first_ver ? first_ver : "?");
        rs_gl_err("%s: %s", what, first);
    }
    return prog;
}

/* A uniform under its own name or the old ruby-prefixed one. */
static inline GLint rs_gl_loc(GLuint prog, const char *name) {
    GLint l = glGetUniformLocation(prog, name);
    if (l < 0) {
        char r[RS_NAME + 8];
        snprintf(r, sizeof r, "ruby%s", name);
        l = glGetUniformLocation(prog, r);
    }
    return l;
}

static inline GLenum rs_gl_uniform_type(GLuint prog, GLint loc) {
    if (loc < 0) return 0;
    GLint count = 0;
    glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &count);
    for (GLint i = 0; i < count; i++) {
        char name[128]; GLsizei len; GLint size; GLenum type;
        glGetActiveUniform(prog, (GLuint)i, sizeof name, &len, &size, &type, name);
        if (glGetUniformLocation(prog, name) == loc) return type;
    }
    return 0;
}

/* One sampler uniform named `prefix`+"Texture", with its sizes, if the program has it. */
static inline void rs_gl_bind_add(rs_gl_pass_t *gp, const char *prefix, int kind, int index) {
    if (gp->n_bind >= RS_MAX_BINDS) return;
    char n[RS_NAME + 16];
    if (kind == RS_SRC_LUT) snprintf(n, sizeof n, "%s", prefix);
    else                    snprintf(n, sizeof n, "%sTexture", prefix);
    GLint loc = rs_gl_loc(gp->prog, n);
    if (loc < 0) return;
    rs_bind_t *b = &gp->bind[gp->n_bind];
    b->kind = (uint8_t)kind;
    b->index = (uint8_t)index;
    b->unit = (uint8_t)gp->n_bind;
    snprintf(n, sizeof n, "%sTextureSize", prefix);
    b->loc_tex_size = kind == RS_SRC_LUT ? -1 : rs_gl_loc(gp->prog, n);
    snprintf(n, sizeof n, "%sInputSize", prefix);
    b->loc_input_size = kind == RS_SRC_LUT ? -1 : rs_gl_loc(gp->prog, n);
    glUniform1i(loc, b->unit);
    gp->n_bind++;
}

static inline void rs_gl_introspect(rs_gl_pass_t *gp, int index, const rs_preset_t *p) {
    GLuint prog = gp->prog;
    glUseProgram(prog);
    gp->a_pos = gp->a_color = -1;
    gp->n_tex_attr = 0;
    GLint count = 0;
    glGetProgramiv(prog, GL_ACTIVE_ATTRIBUTES, &count);
    for (GLint i = 0; i < count; i++) {
        char name[128]; GLsizei len; GLint size; GLenum type;
        glGetActiveAttrib(prog, (GLuint)i, sizeof name, &len, &size, &type, name);
        GLint loc = glGetAttribLocation(prog, name);
        if (loc < 0) continue;
        const char *bare = strncmp(name, "ruby", 4) == 0 ? name + 4 : name;
        if (!strcmp(bare, "VertexCoord") || !strcmp(bare, "Vertex")) gp->a_pos = loc;
        else if (!strcmp(bare, "COLOR") || !strcmp(bare, "Color"))    gp->a_color = loc;
        else if (gp->n_tex_attr < 8) gp->a_tex[gp->n_tex_attr++] = loc;   /* TexCoord, OrigTexCoord, ... */
    }
    gp->u_mvp         = rs_gl_loc(prog, "MVPMatrix");
    gp->u_frame_count = rs_gl_loc(prog, "FrameCount");
    gp->u_frame_dir   = rs_gl_loc(prog, "FrameDirection");
    gp->u_output_size = rs_gl_loc(prog, "OutputSize");
    gp->t_frame_count = rs_gl_uniform_type(prog, gp->u_frame_count);
    gp->t_frame_dir   = rs_gl_uniform_type(prog, gp->u_frame_dir);

    gp->n_bind = 0;
    rs_gl_bind_add(gp, "", RS_SRC_INPUT, 0);
    rs_gl_bind_add(gp, "Orig", RS_SRC_ORIG, 0);
    char n[RS_NAME + 16];
    for (int k = 0; k < index && k < p->n_passes; k++) {
        snprintf(n, sizeof n, "Pass%d", k + 1);
        rs_gl_bind_add(gp, n, RS_SRC_PASS, k);
        snprintf(n, sizeof n, "PassPrev%d", index - k);
        rs_gl_bind_add(gp, n, RS_SRC_PASS, k);
        if (p->pass[k].alias[0]) rs_gl_bind_add(gp, p->pass[k].alias, RS_SRC_PASS, k);
    }
    snprintf(n, sizeof n, "PassPrev%d", index + 1);
    rs_gl_bind_add(gp, n, RS_SRC_ORIG, 0);
    rs_gl_bind_add(gp, "Prev", RS_SRC_PREV, 0);
    for (int h = 1; h < RS_MAX_HISTORY; h++) {
        snprintf(n, sizeof n, "Prev%d", h);
        rs_gl_bind_add(gp, n, RS_SRC_PREV, h);
    }
    for (int l = 0; l < p->n_luts; l++) rs_gl_bind_add(gp, p->lut[l].name, RS_SRC_LUT, l);
    for (int i = 0; i < p->n_params; i++) gp->u_param[i] = glGetUniformLocation(prog, p->param[i].name);
    for (int i = 0; i < gp->n_bind; i++)
        if (gp->bind[i].kind == RS_SRC_PREV && gp->bind[i].index + 1 > g_rs_gl.history)
            g_rs_gl.history = gp->bind[i].index + 1;
}

static inline GLenum rs_gl_wrap(rs_wrap_t w) {
    switch (w) {
        case RS_WRAP_EDGE:   return GL_CLAMP_TO_EDGE;
        case RS_WRAP_REPEAT: return GL_REPEAT;
        case RS_WRAP_MIRROR: return GL_MIRRORED_REPEAT;
        default:
#if defined(GL_CLAMP_TO_BORDER) && !defined(RS_GLES)
            return GL_CLAMP_TO_BORDER;
#else
            return GL_CLAMP_TO_EDGE;   /* GLES 3.0 has no border */
#endif
    }
}

static inline GLuint rs_gl_sampler(bool linear, rs_wrap_t wrap, bool mip) {
    GLuint *s = &g_rs_gl.samplers[linear][wrap][mip];
    if (*s) return *s;
    glGenSamplers(1, s);
    GLenum mag = linear ? GL_LINEAR : GL_NEAREST;
    GLenum min = mip ? (linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST) : mag;
    glSamplerParameteri(*s, GL_TEXTURE_MIN_FILTER, (GLint)min);
    glSamplerParameteri(*s, GL_TEXTURE_MAG_FILTER, (GLint)mag);
    glSamplerParameteri(*s, GL_TEXTURE_WRAP_S, (GLint)rs_gl_wrap(wrap));
    glSamplerParameteri(*s, GL_TEXTURE_WRAP_T, (GLint)rs_gl_wrap(wrap));
    return *s;
}

static inline void rs_gl_target_free(rs_gl_target_t *t) {
    if (t->fbo) glDeleteFramebuffers(1, &t->fbo);
    if (t->tex) glDeleteTextures(1, &t->tex);
    memset(t, 0, sizeof *t);
}

/* (Re)make a render target. Returns the format it really got. */
static inline GLenum rs_gl_target(GLuint *tex, GLuint *fbo, int w, int h, GLenum want) {
    if (*fbo) glDeleteFramebuffers(1, fbo);
    if (*tex) glDeleteTextures(1, tex);
    glGenTextures(1, tex);
    glGenFramebuffers(1, fbo);
    GLenum ifmt = want;
    for (int attempt = 0; attempt < 2; attempt++) {
        glBindTexture(GL_TEXTURE_2D, *tex);
        if (ifmt == GL_RGBA16F)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
        else
            glTexImage2D(GL_TEXTURE_2D, 0, (GLint)ifmt, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) break;
        if (ifmt == GL_RGBA8) break;
        LOG_WARN("shader: a %s framebuffer is not renderable here; using 8-bit",
                 ifmt == GL_RGBA16F ? "float" : "sRGB");
        ifmt = GL_RGBA8;
    }
    return ifmt;
}

static inline void rs_gl_destroy(void) {
    for (int i = 0; i < RS_MAX_PASSES + 1; i++) {
        rs_gl_pass_t *gp = &g_rs_gl.pass[i];
        if (gp->prog) glDeleteProgram(gp->prog);
        if (gp->fbo) glDeleteFramebuffers(1, &gp->fbo);
        if (gp->tex) glDeleteTextures(1, &gp->tex);
    }
    for (int i = 0; i < RS_MAX_LUTS; i++) if (g_rs_gl.lut_tex[i]) glDeleteTextures(1, &g_rs_gl.lut_tex[i]);
    rs_gl_target_free(&g_rs_gl.orig);
    for (int i = 0; i < RS_MAX_HISTORY; i++) rs_gl_target_free(&g_rs_gl.hist[i]);
    if (g_rs_gl.vao) glDeleteVertexArrays(1, &g_rs_gl.vao);
    if (g_rs_gl.vbo) glDeleteBuffers(1, &g_rs_gl.vbo);
    if (g_rs_gl.read_fbo) glDeleteFramebuffers(1, &g_rs_gl.read_fbo);
    for (int a = 0; a < 2; a++) for (int b = 0; b < 4; b++) for (int c = 0; c < 2; c++)
        if (g_rs_gl.samplers[a][b][c]) glDeleteSamplers(1, &g_rs_gl.samplers[a][b][c]);
    char err[RS_ERR];
    memcpy(err, g_rs_gl.err, sizeof err);
    memset(&g_rs_gl, 0, sizeof g_rs_gl);
    memcpy(g_rs_gl.err, err, sizeof err);
}

/* Build the programs and upload the LUTs for `p`, which must outlive the runner. */
static inline bool rs_gl_create(rs_preset_t *p) {
    rs_gl_destroy();
    g_rs_gl.err[0] = '\0';
    g_rs_gl.preset = p;
    const rs_pass_t *last = &p->pass[p->n_passes - 1];
    g_rs_gl.stock_tail = last->scale_set;
    g_rs_gl.n = p->n_passes + (g_rs_gl.stock_tail ? 1 : 0);

#ifdef RS_GLES
    const char *exts = NULL;
    GLint next = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &next);
    for (GLint i = 0; i < next; i++) {
        exts = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (exts && (strstr(exts, "color_buffer_float") || strstr(exts, "color_buffer_half_float"))) g_rs_gl.float_ok = true;
    }
#else
    g_rs_gl.float_ok = true;
#endif

    for (int i = 0; i < g_rs_gl.n; i++) {
        bool stock = i == p->n_passes;
        const char *src = stock ? rs_stock_glsl : p->pass[i].source;
        const char *name = stock ? "stock" : rs_path_base(p->pass[i].path);
        g_rs_gl.pass[i].prog = rs_gl_program(src, name);
        if (!g_rs_gl.pass[i].prog) { rs_gl_destroy(); return false; }
        rs_gl_introspect(&g_rs_gl.pass[i], i, p);
    }
    glUseProgram(0);

    for (int l = 0; l < p->n_luts; l++) {
        glGenTextures(1, &g_rs_gl.lut_tex[l]);
        glBindTexture(GL_TEXTURE_2D, g_rs_gl.lut_tex[l]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, p->lut[l].w, p->lut[l].h, 0, GL_RGBA, GL_UNSIGNED_BYTE, p->lut[l].rgba);
        if (p->lut[l].mipmap) glGenerateMipmap(GL_TEXTURE_2D);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    /* VertexCoord then TexCoord, 0..1. The first four vertices are the
     * offscreen passes' quad; the second four put v = 0 at the TOP of the
     * screen, for the pass that draws there. */
    static const GLfloat quad[] = {
        0, 0, 0, 0,   1, 0, 1, 0,   0, 1, 0, 1,   1, 1, 1, 1,
        0, 1, 0, 0,   1, 1, 1, 0,   0, 0, 0, 1,   1, 0, 1, 1,
    };
    glGenVertexArrays(1, &g_rs_gl.vao);
    glGenBuffers(1, &g_rs_gl.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_rs_gl.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glGenFramebuffers(1, &g_rs_gl.read_fbo);
    g_rs_gl.ready = true;
    LOG_INFO("shader: %s ready, %d pass%s, %d texture%s, %d parameter%s", p->name,
             p->n_passes, p->n_passes == 1 ? "" : "es", p->n_luts, p->n_luts == 1 ? "" : "s",
             p->n_params, p->n_params == 1 ? "" : "s");
    return true;
}

static inline int rs_gl_dim(rs_scale_type_t t, float s, int src, int vp) {
    float v = t == RS_SCALE_SOURCE ? (float)src * s : t == RS_SCALE_VIEWPORT ? (float)vp * s : s;
    int n = (int)(v + 0.5f);
    return n < 1 ? 1 : n > 8192 ? 8192 : n;
}

/* Sizes for this frame; targets remade where they changed. */
static inline void rs_gl_layout(int sw, int sh, int vw, int vh) {
    rs_preset_t *p = g_rs_gl.preset;
    bool orig_mips = p->pass[0].mipmap_input;
    if (g_rs_gl.orig.w != sw || g_rs_gl.orig.h != sh || g_rs_gl.orig.mips != orig_mips || !g_rs_gl.orig.tex) {
        rs_gl_target(&g_rs_gl.orig.tex, &g_rs_gl.orig.fbo, sw, sh, GL_RGBA8);
        g_rs_gl.orig.w = sw; g_rs_gl.orig.h = sh; g_rs_gl.orig.mips = orig_mips;
        for (int i = 0; i < RS_MAX_HISTORY; i++) rs_gl_target_free(&g_rs_gl.hist[i]);
        g_rs_gl.hist_filled = 0;
    }
    for (int i = 0; i < g_rs_gl.history; i++) {
        rs_gl_target_t *h = &g_rs_gl.hist[i];
        if (!h->tex) { rs_gl_target(&h->tex, &h->fbo, sw, sh, GL_RGBA8); h->w = sw; h->h = sh; }
    }
    int w = sw, h = sh;
    for (int i = 0; i < g_rs_gl.n - 1; i++) {
        rs_gl_pass_t *gp = &g_rs_gl.pass[i];
        const rs_pass_t *ps = &p->pass[i];
        int ow = rs_gl_dim(ps->type_x, ps->scale_x, w, vw);
        int oh = rs_gl_dim(ps->type_y, ps->scale_y, h, vh);
        GLenum want = ps->float_fb && g_rs_gl.float_ok ? GL_RGBA16F : ps->srgb_fb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
        bool mips = i + 1 < p->n_passes && p->pass[i + 1].mipmap_input;
        if (gp->w != ow || gp->h != oh || !gp->tex || gp->mips != mips) {
            gp->ifmt = rs_gl_target(&gp->tex, &gp->fbo, ow, oh, want);
            gp->w = ow; gp->h = oh; gp->mips = mips;
        }
        w = ow; h = oh;
    }
    g_rs_gl.src_w = sw; g_rs_gl.src_h = sh;
    g_rs_gl.vp_w = vw;  g_rs_gl.vp_h = vh;
}

static inline void rs_gl_size(GLint loc, int w, int h) {
    if (loc >= 0) glUniform2f(loc, (GLfloat)w, (GLfloat)h);
}

static inline void rs_gl_set_int(GLint loc, GLenum type, int v) {
    if (loc < 0) return;
    if (type == GL_FLOAT) glUniform1f(loc, (GLfloat)v);
    else                  glUniform1i(loc, v);
}

/* Pass i, drawn into whatever framebuffer and viewport are bound. */
static inline void rs_gl_draw_pass(int i, bool to_screen, int out_w, int out_h) {
    rs_preset_t *p = g_rs_gl.preset;
    rs_gl_pass_t *gp = &g_rs_gl.pass[i];
    const bool stock = i >= p->n_passes;
    const rs_pass_t *ps = stock ? NULL : &p->pass[i];

    /* The pass's input: the previous output, or the original frame. */
    GLuint in_tex = g_rs_gl.orig.tex;
    int in_w = g_rs_gl.orig.w, in_h = g_rs_gl.orig.h;
    if (i > 0) { in_tex = g_rs_gl.pass[i - 1].tex; in_w = g_rs_gl.pass[i - 1].w; in_h = g_rs_gl.pass[i - 1].h; }
    const bool in_mips = i == 0 ? g_rs_gl.orig.mips : g_rs_gl.pass[i - 1].mips;
    /* Unset means RetroArch's own smoothing: off, but the stock tail smooths. */
    const bool linear = stock ? true : ps->filter_set ? ps->filter_linear : false;
    const rs_wrap_t wrap = stock ? RS_WRAP_EDGE : ps->wrap;

    glUseProgram(gp->prog);
    if (gp->u_mvp >= 0) {
        static const GLfloat mvp[16] = { 2, 0, 0, 0,  0, 2, 0, 0,  0, 0, -1, 0,  -1, -1, 0, 1 };
        glUniformMatrix4fv(gp->u_mvp, 1, GL_FALSE, mvp);
    }
    unsigned fc = g_rs_gl.frame;
    if (ps && ps->frame_count_mod) fc %= ps->frame_count_mod;
    rs_gl_set_int(gp->u_frame_count, gp->t_frame_count, (int)fc);
    rs_gl_set_int(gp->u_frame_dir, gp->t_frame_dir, 1);
    rs_gl_size(gp->u_output_size, out_w, out_h);
    for (int k = 0; k < p->n_params; k++)
        if (gp->u_param[k] >= 0) glUniform1f(gp->u_param[k], p->param[k].value);

    for (int b = 0; b < gp->n_bind; b++) {
        const rs_bind_t *bd = &gp->bind[b];
        GLuint tex = 0; int w = 0, h = 0; bool mip = false, lin = linear; rs_wrap_t wr = wrap;
        switch (bd->kind) {
            case RS_SRC_INPUT: tex = in_tex; w = in_w; h = in_h; mip = in_mips && ps && ps->mipmap_input; break;
            case RS_SRC_ORIG:  tex = g_rs_gl.orig.tex; w = g_rs_gl.orig.w; h = g_rs_gl.orig.h; break;
            case RS_SRC_PASS:  tex = g_rs_gl.pass[bd->index].tex; w = g_rs_gl.pass[bd->index].w; h = g_rs_gl.pass[bd->index].h; break;
            case RS_SRC_PREV: {
                /* Prev is the frame before this one: fall back to this frame
                 * until the ring has filled that far. */
                int back = bd->index + 1, H = g_rs_gl.history;
                if (H > 0 && back <= g_rs_gl.hist_filled)
                    tex = g_rs_gl.hist[(g_rs_gl.hist_head - back + H) % H].tex;
                if (!tex) tex = g_rs_gl.orig.tex;
                w = g_rs_gl.orig.w; h = g_rs_gl.orig.h;
                break;
            }
            case RS_SRC_LUT:
                tex = g_rs_gl.lut_tex[bd->index];
                lin = p->lut[bd->index].linear; wr = p->lut[bd->index].wrap; mip = p->lut[bd->index].mipmap;
                break;
        }
        glActiveTexture(GL_TEXTURE0 + bd->unit);
        glBindTexture(GL_TEXTURE_2D, tex);
        glBindSampler(bd->unit, rs_gl_sampler(lin, wr, mip));
        rs_gl_size(bd->loc_tex_size, w, h);
        rs_gl_size(bd->loc_input_size, w, h);
    }

    glBindVertexArray(g_rs_gl.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_rs_gl.vbo);
    const GLsizei stride = 4 * sizeof(GLfloat);
    const size_t base = to_screen ? 16 * sizeof(GLfloat) : 0;
    if (gp->a_pos >= 0) {
        glEnableVertexAttribArray((GLuint)gp->a_pos);
        glVertexAttribPointer((GLuint)gp->a_pos, 2, GL_FLOAT, GL_FALSE, stride, (const void *)base);
    }
    for (int t = 0; t < gp->n_tex_attr; t++) {
        glEnableVertexAttribArray((GLuint)gp->a_tex[t]);
        glVertexAttribPointer((GLuint)gp->a_tex[t], 2, GL_FLOAT, GL_FALSE, stride, (const void *)(base + 2 * sizeof(GLfloat)));
    }
    if (gp->a_color >= 0) {
        glDisableVertexAttribArray((GLuint)gp->a_color);
        glVertexAttrib4f((GLuint)gp->a_color, 1.0f, 1.0f, 1.0f, 1.0f);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (gp->a_pos >= 0) glDisableVertexAttribArray((GLuint)gp->a_pos);
    for (int t = 0; t < gp->n_tex_attr; t++) glDisableVertexAttribArray((GLuint)gp->a_tex[t]);
    for (int b = 0; b < gp->n_bind; b++) {
        glActiveTexture(GL_TEXTURE0 + gp->bind[b].unit);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindSampler(gp->bind[b].unit, 0);
    }
    glActiveTexture(GL_TEXTURE0);
}

static inline void rs_gl_state(void) {
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

/*
 * Everything but the last pass, outside any sokol pass: the source (a GL texture
 * holding the game, bottom row first, as a GL render target does) is copied
 * upright into Orig, then the offscreen passes run. vw x vh is the rectangle the
 * last pass will fill.
 */
static inline void rs_gl_prepare(GLuint src_tex, int sw, int sh, int vw, int vh) {
    if (!g_rs_gl.ready) return;
    rs_gl_state();
    rs_gl_layout(sw, sh, vw, vh);

    /* Flipped on the way in, so v = 0 is the top of the picture, as RetroArch has it. */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_rs_gl.read_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src_tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_rs_gl.orig.fbo);
    glBlitFramebuffer(0, 0, sw, sh, 0, sh, sw, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    if (g_rs_gl.orig.mips) {
        glBindTexture(GL_TEXTURE_2D, g_rs_gl.orig.tex);
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    for (int i = 0; i < g_rs_gl.n - 1; i++) {
        rs_gl_pass_t *gp = &g_rs_gl.pass[i];
        glBindFramebuffer(GL_FRAMEBUFFER, gp->fbo);
        glViewport(0, 0, gp->w, gp->h);
#if defined(GL_FRAMEBUFFER_SRGB) && !defined(RS_GLES)
        if (gp->ifmt == GL_SRGB8_ALPHA8) glEnable(GL_FRAMEBUFFER_SRGB);
#endif
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        rs_gl_draw_pass(i, false, gp->w, gp->h);
#if defined(GL_FRAMEBUFFER_SRGB) && !defined(RS_GLES)
        glDisable(GL_FRAMEBUFFER_SRGB);
#endif
        if (gp->mips) {
            glBindTexture(GL_TEXTURE_2D, gp->tex);
            glGenerateMipmap(GL_TEXTURE_2D);
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    glBindVertexArray(0);
}

/* The last pass, into the framebuffer bound now, over (x, y_gl, w, h) with GL's
 * origin at the bottom left. Then the frame goes into the history ring. */
static inline void rs_gl_final(int x, int y_gl, int w, int h) {
    if (!g_rs_gl.ready || !g_rs_gl.orig.tex) return;
    GLint fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    rs_gl_state();
    glViewport(x, y_gl, w, h);
    rs_gl_draw_pass(g_rs_gl.n - 1, true, w, h);
    glUseProgram(0);
    glBindVertexArray(0);

    if (g_rs_gl.history > 0) {
        rs_gl_target_t *hs = &g_rs_gl.hist[g_rs_gl.hist_head];
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_rs_gl.orig.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, hs->fbo);
        glBlitFramebuffer(0, 0, g_rs_gl.orig.w, g_rs_gl.orig.h, 0, 0, hs->w, hs->h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        g_rs_gl.hist_head = (g_rs_gl.hist_head + 1) % g_rs_gl.history;
        if (g_rs_gl.hist_filled < g_rs_gl.history) g_rs_gl.hist_filled++;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fb);
    g_rs_gl.frame++;
}

#endif /* GL runner */

#endif /* RETRO_SHADER_H */
