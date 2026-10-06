/*
 * dc_strips.h -- STRIPS.PAK, the 3D models made ready for the Tile
 * Accelerator offline (Pinboard #498).
 *
 * The draw's mesh cache (geo3d_mesh_get) walks a model's GEO stream once per
 * key (model, material and UV pointers) and keeps the faces; on the Dreamcast
 * that walk runs whenever a mesh comes back into the 1 MB arena, some 60
 * times a second in a fight. The pack holds each mesh the disc's recorded
 * frames draw (sfight.strips), already walked: geo3d_mesh_build's corners,
 * per face a geo3d_sface_t with the PVR texture's key found (and a tile over
 * 256 already windowed), and its corners' u, v in the texture's units. The
 * board's geometry is untouched: corners stay in model space, and the frame
 * still transforms them with the matrix the list gives the model
 * (calc_unit_mat's slot), culls, lights and sorts. What goes is the
 * stream walk, the attribute and texture header words, the UV words and the
 * window. A quad's four corners are one strip (A B C D; dp_face reorders them
 * to B A D C for the other diagonal); faces stay in the walk's order, as the
 * flat key and the diagonals carry from face to face, and the corners are
 * grouped opaque, punch-through, translucent.
 *
 * Why u, v and not whole pvr_vertex_t: the frame writes x, y, z, the colours
 * and the command word of every vertex it submits (the corners move with the
 * fighters' matrices), so only u, v survive from a stored one. As 32-byte
 * pvr_vertex_t the pack was 14.4 MB instead of 8.3, a packed mesh took more
 * of the arena, and the bench's page loads went from 357 to 908
 * (DREAMCAST-PORT.md #498).
 *
 * File: dcs_head_t; the index, dcs_index_t sorted by (model, mat, uv); at
 * data_off (a sector) the blobs, 32-byte aligned, in the order the recorded
 * frames first drew them. A blob is geo3d.h's geo3d_sp_head_t, sv padded to
 * 32, the faces, the corners' u, v. ROM-derived, so built by mkdisc.sh from the
 * ROM files (tools/dc_strips.c) and never committed.
 *
 * Host-clean: the converter includes it too (DCS_WRITER).
 */
#ifndef DC_STRIPS_H
#define DC_STRIPS_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    char     magic[4];                  /* "M2SP" */
    uint32_t n, data_off;               /* index entries; the blobs' offset, a sector multiple */
    /* the ROM the meshes were walked from: the pack is used only if they match */
    uint32_t polygons_size, textures_size, table_off, table_count, mesh_ptr_subtract, mesh_ptr_add;
    uint32_t verts, faces, bytes;       /* totals, for the log */
    uint32_t pad[4];
} dcs_head_t;

typedef struct {
    int32_t  model_idx;
    uint32_t mat_ptr, uv_ptr;
    uint32_t off, len;                  /* from data_off */
} dcs_index_t;

_Static_assert(sizeof(dcs_head_t) == 64 && sizeof(dcs_index_t) == 20, "STRIPS.PAK records");

static inline int dcs_key_cmp(int32_t ma, uint32_t ta, uint32_t ua, int32_t mb, uint32_t tb, uint32_t ub) {
    if (ma != mb) return ma < mb ? -1 : 1;
    if (ta != tb) return ta < tb ? -1 : 1;
    if (ua != ub) return ua < ub ? -1 : 1;
    return 0;
}

/* A tile wider or taller than the PVR's 256 (m2_sprite.h's 512x512 atlas):
 * the square window of it the face's coordinates fall in, a power of two at
 * most 256, with the coordinates moved into it. Aligned to its size when one
 * holds them (faces near each other share it), else from the face's own
 * corner (a sprite across a 256 line). False when they span more than 256
 * or leave the tile (a repeat). */
static bool dp_big_window(float *tx, float *ty, float *tw, float *th, float *u, float *v, int n) {
    float u0 = u[0], u1 = u[0], v0 = v[0], v1 = v[0];
    for (int i = 1; i < n; i++) {
        u0 = u[i] < u0 ? u[i] : u0; u1 = u[i] > u1 ? u[i] : u1;
        v0 = v[i] < v0 ? v[i] : v0; v1 = v[i] > v1 ? v[i] : v1;
    }
    if (u0 < 0.0f || v0 < 0.0f || u1 > *tw || v1 > *th) return false;
    for (float m = 8.0f; m <= 256.0f; m *= 2.0f) {
        float sx = floorf(u0 / m) * m, sy = floorf(v0 / m) * m;
        if (u1 > sx + m || v1 > sy + m || sx + m > *tw || sy + m > *th) continue;
        for (int i = 0; i < n; i++) { u[i] -= sx; v[i] -= sy; }
        *tx += sx; *ty += sy; *tw = *th = m;
        return true;
    }
    for (float m = 8.0f; m <= 256.0f; m *= 2.0f) {
        if (u1 - u0 > m || v1 - v0 > m || m > *tw || m > *th) continue;
        float sx = fminf(floorf(u0), *tw - m), sy = fminf(floorf(v0), *th - m);
        for (int i = 0; i < n; i++) { u[i] -= sx; v[i] -= sy; }
        *tx += sx; *ty += sy; *tw = *th = m;
        return true;
    }
    return false;
}

static inline uint32_t dcs_log2(uint32_t v) { uint32_t l = 0; while ((1u << l) < v) l++; return l; }

/* dp_tex_get's key for a tile, 0 when it cuts none (over 1024, or empty). */
static inline uint32_t dcs_tex_key(float ftx, float fty, float ftw, float fth, unsigned fl) {
    unsigned sheet = (fl & 4u /* GEO3D_FACE_SHEET1 */) ? 1 : 0;
    unsigned x0 = (unsigned)(int)ftx & 2047u, y0 = (unsigned)(int)fty & 1023u;
    unsigned tw = (unsigned)ftw, th = (unsigned)fth;
    if (tw > 1024 || th > 1024 || !tw || !th) return 0;
    return 0x80000000u | sheet << 29 | x0 << 18 | y0 << 8 | dcs_log2(tw) << 4 | dcs_log2(th);
}

#ifdef DCS_WRITER
/* A built mesh (geo3d_mesh_build, the host's faces) as a blob at out: its
 * length, 0 if it does not fit cap or the records' fields. */
static size_t dcs_blob(const geo3d_cmesh_t *m, uint8_t *out, size_t cap) {
    const size_t sv_bytes = ((size_t)m->n_sv * sizeof(vec3_t) + 31u) & ~(size_t)31u;
    if (m->n_sv > 0xFFFF || m->n_faces > 0xFFFF) return 0;
    size_t nverts = 0;
    for (int n = 0; n < m->n_faces; n++) nverts += m->faces[n].is_tri ? 3 : 4;
    const size_t len = sizeof(geo3d_sp_head_t) + sv_bytes + (size_t)m->n_faces * sizeof(geo3d_sface_t) +
                       nverts * sizeof(geo3d_svert_t);
    if (len > cap || nverts > 0xFFFF) return 0;
    memset(out, 0, len);
    geo3d_sp_head_t *h = (geo3d_sp_head_t *)out;
    h->model_idx = m->model_idx; h->mat_ptr = m->mat_ptr; h->uv_ptr = m->uv_ptr;
    h->n_sv = (uint16_t)m->n_sv; h->n_faces = (uint16_t)m->n_faces;
    h->bc[0] = m->bc.x; h->bc[1] = m->bc.y; h->bc[2] = m->bc.z; h->br = m->br;
    memcpy(out + sizeof *h, m->sv, (size_t)m->n_sv * sizeof(vec3_t));
    geo3d_sface_t *sf = (geo3d_sface_t *)(out + sizeof *h + sv_bytes);
    geo3d_svert_t *sv = (geo3d_svert_t *)(sf + m->n_faces);

    /* the faces, and each one's corners' texture coordinates (geo3d_dc_face's) */
    static float fu[65536][4], fv[65536][4];
    for (int n = 0; n < m->n_faces; n++) {
        const geo3d_cface_t *F = &m->faces[n];
        geo3d_sface_t *o = &sf[n];
        const int c[4] = { F->ai, F->bi, F->ci, F->di };
        const int nv = F->is_tri ? 3 : 4;
        for (int k = 0; k < 4; k++) {
            /* a corner the draw never reads (no C, a triangle's D) is kept as 0 */
            bool read = k < 2 || (k == 2 && F->has_c) || (k == 3 && !F->is_tri);
            if (read && (c[k] < 0 || c[k] >= m->n_sv)) return 0;
        }
        o->ai = (uint16_t)F->ai; o->bi = (uint16_t)F->bi;
        o->ci = F->has_c ? (uint16_t)F->ci : 0; o->di = F->is_tri ? 0 : (uint16_t)F->di;
        for (int z = 0; z < 4; z++) {
            if (F->zsrc[z] < 0 || F->zsrc[z] > 0xFFFF) return 0;
            o->zsrc[z] = (uint16_t)F->zsrc[z];
        }
        if (F->zmode > 0xFF || F->matidx > 0xFFFF) return 0;
        o->qn = F->qn; o->qa = F->qa;
        o->split_quad = F->split_quad; o->split_cut = F->split_cut;
        o->lb = F->lb;
        o->matidx = (uint16_t)F->matidx;
        unsigned f = (unsigned)(F->fl + 0.5f);
        o->fl = (uint16_t)f;
        o->zmode = (uint8_t)F->zmode;
        o->bits = (uint8_t)((F->is_tri ? GEO3D_SF_TRI : 0) | (F->has_c ? GEO3D_SF_HAS_C : 0) |
                            (F->has_qn ? GEO3D_SF_HAS_QN : 0) | (F->mat_ok ? GEO3D_SF_MAT_OK : 0) |
                            (F->tw > 0.0f ? GEO3D_SF_TEXTURED : 0));
        o->nv = (uint8_t)nv;
        o->list = (uint8_t)((f & 2u /* CHECKER */) ? GEO3D_SL_TR :
                            ((f & 1u /* TRANSPARENT */) && F->tw > 0.0f) ? GEO3D_SL_PT : GEO3D_SL_OP);

        float tx = F->tx, ty = F->ty, tw = F->tw, th = F->th;
        float wu[4], wv[4];
        memcpy(wu, F->uvu, sizeof wu); memcpy(wv, F->uvv, sizeof wv);
        if (tw > 256.0f || th > 256.0f) {
            if (!dp_big_window(&tx, &ty, &tw, &th, wu, wv, nv)) {
                memcpy(wu, F->uvu, sizeof wu); memcpy(wv, F->uvv, sizeof wv);
            }
        }
        o->tex = ((f & 2u) || !(tw > 0.0f)) ? 0u : dcs_tex_key(tx, ty, tw, th, f);
        /* dp_tex_get's su, sv: one over the PVR texture's side, at least 8 */
        float su = 0.0f, svv = 0.0f;
        if (o->tex) {
            unsigned W = 1u << ((o->tex >> 4) & 15u), H = 1u << (o->tex & 15u);
            W = W < 8 ? 8 : W; H = H < 8 ? 8 : H;
            su = 1.0f / (float)W; svv = 1.0f / (float)H;
        }
        for (int k = 0; k < 4; k++) { fu[n][k] = wu[k] * su; fv[n][k] = wv[k] * svv; }
    }

    /* the corners' u, v, list by list */
    size_t at = 0;
    for (int list = GEO3D_SL_OP; list <= GEO3D_SL_TR; list++)
        for (int n = 0; n < m->n_faces; n++) {
            geo3d_sface_t *o = &sf[n];
            if (o->list != list) continue;
            o->strip = (uint16_t)at;
            for (int k = 0; k < o->nv; k++) {
                geo3d_svert_t *v = &sv[at++];
                v->u = fu[n][k]; v->v = fv[n][k];
            }
        }
    return len;
}
#endif /* DCS_WRITER */

#endif /* DC_STRIPS_H */
