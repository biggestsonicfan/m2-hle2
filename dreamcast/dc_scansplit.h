/* dc_scansplit.h -- the 3D scan's time by function (make SCANSPLIT=1, Pinboard #535).
 *
 * The bench's "sc" is dp_decode (dc_pvr.h): the GEO walk and every model's
 * decode. With DC_SCAN_SPLIT each GEO3D_LAP(part) mark in dp_decode and in
 * geo3d.h's cached path charges the time since the last mark to its part, so
 * the parts add up to "sc". A mark is one read of TMU2's counter (KOS's
 * clock, 80 ns a tick) and a subtraction, not timer_us_gettime64's divide.
 * The bench reports the parts' times and calls at its last frame. Off by
 * default: the marks are then nothing and the disc is the same. */
#ifndef DC_SCANSPLIT_H
#define DC_SCANSPLIT_H

#include <stdint.h>

#ifndef DC_SCAN_SPLIT
#define DC_SCAN_SPLIT 0
#endif

enum {
    DC_LAP_WALK,     /* dp_decode's setup and geo3d_scan_geo_list */
    DC_LAP_RUN,      /* dp_decode's runs: grouping, dp_cull_planes, the per-model state */
    DC_LAP_MESH,     /* geo3d_mesh_for_draw: the model's streams, geo3d_mesh_get (geo3d_strips_load) */
    DC_LAP_FULL,     /* geo3d_decode_model: a model the cache does not take */
    DC_LAP_DIRECT,   /* geo3d_decode_direct */
    DC_LAP_SPHERE,   /* geo3d_cached_sphere, and geo3d_cached_gone_carry for a model wholly out */
    DC_LAP_XFORM,    /* the corners through the matrix (ftrv) */
    DC_LAP_CODES,    /* geo3d_cached_cull_codes */
    DC_LAP_PROJ,     /* geo3d_dc_verts: the corners projected into the pool */
    DC_LAP_FACES,    /* geo3d_cached_draw_dc's face loop (calls: faces walked) */
    DC_LAPS
};

typedef struct {
    uint32_t last;            /* TMU2's count at the last mark */
    uint32_t t[DC_LAPS];      /* ticks charged to each part, since boot */
    uint32_t n[DC_LAPS];      /* the marks (or faces) counted to each */
} dc_split_t;

#if DC_SCAN_SPLIT
static dc_split_t g_split;

#define DC_TMU2_TCOR (*(volatile uint32_t *)0xFFD80020u)   /* TMU2's reload: ticks in KOS's 1 s */
#define DC_TMU2_TCNT (*(volatile uint32_t *)0xFFD80024u)   /* counts down, reloads at 0 */

static inline void dc_lap_start(void) { g_split.last = DC_TMU2_TCNT; }

static inline void dc_lap(unsigned part, uint32_t n) {
    uint32_t now = DC_TMU2_TCNT, d = g_split.last - now;
    if (now > g_split.last) d += DC_TMU2_TCOR + 1u;   /* reloaded since */
    g_split.last = now;
    g_split.t[part] += d;
    g_split.n[part] += n;
}

/* ticks to ms */
static inline uint32_t dc_split_ms(uint32_t ticks) {
    return (uint32_t)((uint64_t)ticks * 1000u / (DC_TMU2_TCOR + 1u));
}

#define GEO3D_LAP_START()    dc_lap_start()
#define GEO3D_LAP_N(p, cnt)  dc_lap(DC_LAP_##p, (uint32_t)(cnt))
#endif

#endif
