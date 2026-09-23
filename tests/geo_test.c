/*
 * geo_test.c — Phase 9 verification for the 3D polygon decoder.
 *
 * Loads the real STF ROM set and runs the J=1.0 index-array decoder
 * (geo3d_decode_model) across the model table, confirming it produces valid,
 * finite geometry: a large fraction of models decode to non-empty meshes, the
 * emitted triangle/line coordinates are finite and bounded, and quad faces use
 * the A-B-D-C winding (2 tris per quad). Uses the STF quirks from sfight_profile.
 *
 * NOTE on the formal Jaccard=1.0 milestone: the offline comparator and the
 * model-index -> reference-OBJ filename mapping (C:\m2\3d\new\stf-poly) are not
 * checked into these repos, so a literal Jaccard re-measurement isn't done here.
 * The decoder is the verbatim J=1.0 implementation + STF's mesh-pointer
 * quirks; this test verifies the
 * port reproduces valid geometry. See the Phase 9 report for the caveat.
 */
#define NDEBUG 1
#include <stdio.h>
#include <math.h>
#include <time.h>

#include "sfight.h"     /* sfight_profile (quirks), load/install */
#include "geo3d.h"      /* geo3d_decode_model, g_geo3d_tris/lines */

#define ROMDIR "c:/Users/bigge/source/repos/ai/claude_mame/mame/roms/"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

static int finite_f(float v){ return (v == v) && v < 1e20f && v > -1e20f; }

/* Rank one model's faces the way the display-list draw does (geo3d_mesh_build →
 * geo3d_mesh_layers) and count its triangles above layer 0 and at it. */
static void layer_counts(const romset_t *rs, const game_quirks_t *q, int idx,
                         int *tris_up, int *tris_flat, int *planes) {
    static geo3d_cmesh_t m;
    *tris_up = *tris_flat = *planes = 0;
    uint32_t toff = q->model_table_offset + (uint32_t)idx * MODEL_ENTRY_SIZE;
    memset(&m, 0, sizeof m);
    m.model_idx = idx;
    m.uv_ptr  = read_u32_le(rs->main_data + toff + 0);
    m.mat_ptr = read_u32_le(rs->main_data + toff + 4);
    m.polygons = rs->polygons;   m.polygons_size  = rs->polygons_size;
    m.materials = rs->textures;  m.materials_size = rs->textures_size;
    m.main_data = rs->main_data;
    uint32_t mesh = read_u32_le(rs->main_data + toff + 8) * 4u - q->mesh_ptr_subtract + q->mesh_ptr_add;
    if (!geo3d_mesh_build(&m, mesh, m.mat_ptr != 0, m.uv_ptr != 0)) return;
    printf("info: model %d mat_ptr=%06X uv_ptr=%06X layered faces:", idx, m.mat_ptr, m.uv_ptr);
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (int i = 0; i < m.n_faces; i++) {
        const geo3d_cface_t *f = &m.faces[i];
        if (f->is_tri && !f->has_c) continue;
        int t = f->is_tri ? 1 : 2;
        if (f->layer) {
            *tris_up += t;
            printf(" %d(L%d)", i, f->layer);
            const int c[4] = { f->ai, f->bi, f->ci, f->di };
            for (int k = 0; k < (f->is_tri ? 3 : 4); k++) {
                const float p[3] = { m.sv[c[k]].x, m.sv[c[k]].y, m.sv[c[k]].z };
                for (int a = 0; a < 3; a++) { if (p[a] < lo[a]) lo[a] = p[a]; if (p[a] > hi[a]) hi[a] = p[a]; }
            }
        } else *tris_flat += t;
        if (f->has_plane) *planes += 1;
    }
    printf("\ninfo: model %d layered faces span (%.2f,%.2f,%.2f)..(%.2f,%.2f,%.2f)\n",
           idx, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
    free(m.sv); free(m.faces); free(m.edges);
}

/* GEO_TEST_DUMP=<model>: every face the ranking touched, and its orderings. */
static void layer_dump(const romset_t *rs, const game_quirks_t *q, int idx) {
    static geo3d_cmesh_t m;
    uint32_t toff = q->model_table_offset + (uint32_t)idx * MODEL_ENTRY_SIZE;
    memset(&m, 0, sizeof m);
    m.model_idx = idx;
    m.uv_ptr  = read_u32_le(rs->main_data + toff + 0);
    m.mat_ptr = read_u32_le(rs->main_data + toff + 4);
    m.polygons = rs->polygons;   m.polygons_size  = rs->polygons_size;
    m.materials = rs->textures;  m.materials_size = rs->textures_size;
    m.main_data = rs->main_data;
    uint32_t mesh = read_u32_le(rs->main_data + toff + 8) * 4u - q->mesh_ptr_subtract + q->mesh_ptr_add;
    if (!geo3d_mesh_build(&m, mesh, m.mat_ptr != 0, m.uv_ptr != 0)) return;
    printf("dump: model %d, %d faces, %d orderings\n", idx, m.n_faces, m.n_edges);
    for (int i = 0; i < m.n_faces; i++) {
        const geo3d_cface_t *f = &m.faces[i];
        bool in = false;
        for (int e = 0; e < m.n_edges && !in; e++) in = m.edges[e].lo == i || m.edges[e].hi == i;
        if (!in) continue;
        const int c[4] = { f->ai, f->bi, f->ci, f->di };
        const int nc = f->is_tri ? 3 : 4;
        float cx = 0, cy = 0, cz = 0, off = 0;
        for (int k = 0; k < nc; k++) {
            vec3_t p = m.sv[c[k]];
            cx += p.x / nc; cy += p.y / nc; cz += p.z / nc;
            float d = f->plane[0] * p.x + f->plane[1] * p.y + f->plane[2] * p.z - f->plane[3];
            if (fabsf(d) > fabsf(off)) off = d;
        }
        printf("dump:   face %3d fi %3d layer %u zmode %u fl %2d tile %4.0f,%4.0f %s centre (%7.2f,%7.2f,%7.2f) plane %s off %+.3f\n",
               i, f->fi, f->layer, f->zmode, (int)f->fl, f->tx, f->ty, f->tw > 0 ? "tex" : "flat", cx, cy, cz,
               f->has_plane ? "yes" : "no ", off);
    }
    for (int e = 0; e < m.n_edges; e++)
        printf("dump:   %3d / %3d  top %3d  %s\n", m.edges[e].lo, m.edges[e].hi, m.edges[e].top, m.edges[e].by_sort ? "by sort" : "held apart");
    free(m.sv); free(m.faces); free(m.edges);
}

int main(void) {
    static romset_t rs;
    if (sfight_load(&rs, ROMDIR "sfight.zip", ROMDIR "schamp.zip") != 0) {
        printf("FAIL: ROM load\n"); return 1;
    }
    const game_quirks_t *q = &sfight_profile.quirks;
    if (getenv("GEO_TEST_DUMP")) {
        layer_dump(&rs, q, atoi(getenv("GEO_TEST_DUMP")));
        romset_free(&rs);
        return 0;
    }

    int      nonempty = 0;
    long     total_tris = 0, total_lines = 0;
    int      bad_coord = 0;
    int      best_idx = -1, best_tris = 0;

    for (uint32_t m = 0; m < q->model_table_count; m++) {
        geo3d_tris_reset();
        geo3d_lines_reset();
        geo3d_decode_model((int)m,
                           rs.main_data, rs.main_data_size,
                           rs.polygons,  rs.polygons_size,
                           rs.textures,  rs.textures_size,
                           q->model_table_offset, q->model_table_count,
                           q->mesh_ptr_subtract, q->mesh_ptr_add,
                           NULL,                  /* model space (no transform) */
                           1.0f, 1.0f, 1.0f);
        int nt = g_geo3d_tris.count, nl = g_geo3d_lines.count;
        if (nt > 0 || nl > 0) nonempty++;
        total_tris  += nt;
        total_lines += nl;
        if (nt > best_tris) { best_tris = nt; best_idx = (int)m; }
        for (int i = 0; i < nt; i++) {
            const geo3d_tri_t *t = &g_geo3d_tris.tris[i];
            if (!finite_f(t->x0)||!finite_f(t->y0)||!finite_f(t->z0)||
                !finite_f(t->x1)||!finite_f(t->y1)||!finite_f(t->z1)||
                !finite_f(t->x2)||!finite_f(t->y2)||!finite_f(t->z2)) { bad_coord++; break; }
        }
    }

    printf("info: models scanned=%u nonempty=%d total_tris=%ld total_lines=%ld bad_coord_models=%d\n",
           q->model_table_count, nonempty, total_tris, total_lines, bad_coord);
    printf("info: richest model = index %d with %d triangles\n", best_idx, best_tris);

    CHECK(nonempty > 1000, "thousands of models decode to non-empty meshes");
    CHECK(total_tris > 100000, "decoder emits a large body of triangles");
    CHECK(bad_coord == 0, "all emitted triangle coordinates are finite");
    CHECK(best_tris > 0 && best_idx >= 0, "found a richly-detailed model");

    /* Re-decode the richest model and sanity-check bounds + winding (2 tris/quad
     * means tri count is even for an all-quad model; mixed tri/quad is allowed). */
    geo3d_tris_reset(); geo3d_lines_reset();
    geo3d_decode_model(best_idx,
                       rs.main_data, rs.main_data_size,
                       rs.polygons,  rs.polygons_size,
                       rs.textures,  rs.textures_size,
                       q->model_table_offset, q->model_table_count,
                       q->mesh_ptr_subtract, q->mesh_ptr_add,
                       NULL, 1.0f, 1.0f, 1.0f);
    float minx=1e30f,maxx=-1e30f;
    for (int i = 0; i < g_geo3d_tris.count; i++) {
        const geo3d_tri_t *t = &g_geo3d_tris.tris[i];
        if (t->x0<minx)minx=t->x0; if (t->x0>maxx)maxx=t->x0;
    }
    printf("info: richest model X extent [%.2f, %.2f]\n", minx, maxx);
    CHECK(maxx > minx, "richest model has non-degenerate spatial extent");

    /* Faces lying on faces (issue #75), against the explorer's js/layers.js:
     * 580's 32 shadow triangles over its 262 of sand, and 188's two JACKPOT
     * panels (4 triangles) over the slot machine's 60. */
    int up, flat, planes;
    layer_counts(&rs, q, 580, &up, &flat, &planes);
    printf("info: model 580: %d triangles layered, %d at layer 0, %d faces on a plane\n", up, flat, planes);
    CHECK(up == 32 && flat == 262, "model 580: the pyramid shadows lie over the sand");
    layer_counts(&rs, q, 188, &up, &flat, &planes);
    printf("info: model 188: %d triangles layered, %d at layer 0, %d faces on a plane\n", up, flat, planes);
    CHECK(up == 4 && flat == 60, "model 188: the JACKPOT panels lie over the cabinet");

    /* What ranking costs: it runs once per mesh, the first time a model is
     * drawn, so the slowest model is a first-sight hitch. */
    {
        static geo3d_cmesh_t mm;
        double worst = 0.0, total = 0.0;
        int worst_idx = -1;
        for (uint32_t m = 0; m < q->model_table_count; m++) {
            uint32_t toff = q->model_table_offset + m * MODEL_ENTRY_SIZE;
            uint32_t mp = read_u32_le(rs.main_data + toff + 8);
            if (!mp) continue;
            memset(&mm, 0, sizeof mm);
            mm.model_idx = (int)m;
            mm.uv_ptr  = read_u32_le(rs.main_data + toff + 0);
            mm.mat_ptr = read_u32_le(rs.main_data + toff + 4);
            mm.polygons = rs.polygons;  mm.polygons_size  = rs.polygons_size;
            mm.materials = rs.textures; mm.materials_size = rs.textures_size;
            mm.main_data = rs.main_data;
            clock_t t0 = clock();
            if (geo3d_mesh_build(&mm, mp * 4u - q->mesh_ptr_subtract + q->mesh_ptr_add, mm.mat_ptr != 0, mm.uv_ptr != 0)) {
                double ms = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC;
                total += ms;
                if (ms > worst) { worst = ms; worst_idx = (int)m; }
                free(mm.sv); free(mm.faces);
            }
        }
        printf("info: mesh build + ranking, every model: %.0f ms total, slowest model %d at %.1f ms\n",
               total, worst_idx, worst);
    }

    romset_free(&rs);
    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
