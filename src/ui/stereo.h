/*
 * stereo.h -- side-by-side 3D for a phone in a Cardboard viewer (Pinboard #1436).
 *
 * One board, two eyes. The board is run once, as ever; what changes is only how
 * its frame is drawn: game_frame_draw runs once per eye into that eye's half of
 * the screen, and the GEO projection (gm_mat4_geo_projection) moves the eye
 * sideways for each. Nothing the i960 can read changes, so netplay, replays and
 * det_digest see the same board with 3D on or off.
 *
 * The shift is an off-axis pair, the usual stereo camera: each eye moves by s
 * along x in eye space, and its picture slides back by the same amount at the
 * focus distance D, so a point at D lands on the same pixel in both eyes (the
 * screen's depth), nearer ones stand out and farther ones sink in. Put as the
 * projection's x row:
 *
 *     clip x += m0 * s + k * w,  w = -zh (host eye z),  k = -m0 * s / D
 *
 * k is the parallax at infinity in NDC, which is what the Depth setting picks
 * (a fraction of an eye's picture), so the felt depth does not depend on the
 * game's units; D is the scene's own distance (stereo_note_scene: the median
 * eye distance of the frame's objects, smoothed) times the Focus setting.
 *
 * The tile layers are flat: the background is slid by the infinity parallax
 * (it is the sky, the farthest thing there is), the foreground (HUD) comes
 * out a little in front of the screen (STEREO_HUD of the infinity parallax,
 * crossed), so a fighter at the focus does not stand in front of the bars that
 * cover it.
 */
#ifndef STEREO_H
#define STEREO_H

#include <math.h>
#include <stdbool.h>


typedef struct {
    int      on;
    float    depth;    /* parallax at infinity per eye, a fraction of the eye's picture width */
    float    focus;    /* the screen's depth, a multiple of the scene's distance */
    float    spacing;  /* between the two pictures' centres, a fraction of the screen width */
    float    size;     /* each picture's width, a fraction of its half of the screen */
    int      swap;     /* left picture for the right eye: cross-eyed viewing without a viewer */
    unsigned gen;      /* bumped by every change, so a held picture is drawn again */

    /* per draw */
    int   eye;         /* -1 left, +1 right, 0 one picture */
    float scene_z;     /* the scene's distance, smoothed (0: none seen yet) */
} stereo_state_t;

#define STEREO_DEPTH_MAX   0.06f
#define STEREO_FOCUS_MIN   0.25f
#define STEREO_FOCUS_MAX   4.0f
#define STEREO_SPACING_MIN 0.30f
#define STEREO_SPACING_MAX 0.70f
#define STEREO_SIZE_MIN    0.50f
#define STEREO_HUD         0.35f

static stereo_state_t g_stereo = {
    .on = 0, .depth = 0.02f, .focus = 1.0f, .spacing = 0.5f, .size = 1.0f,
};

static inline float stereo__clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Any value out of range is brought into it; NaN takes the default. */
static inline void stereo_set(int on, float depth, float focus, float spacing, float size, int swap) {
    if (!(depth == depth))     depth = 0.02f;
    if (!(focus == focus))     focus = 1.0f;
    if (!(spacing == spacing)) spacing = 0.5f;
    if (!(size == size))       size = 1.0f;
    g_stereo.on      = on != 0;
    g_stereo.depth   = stereo__clamp(depth, 0.0f, STEREO_DEPTH_MAX);
    g_stereo.focus   = stereo__clamp(focus, STEREO_FOCUS_MIN, STEREO_FOCUS_MAX);
    g_stereo.spacing = stereo__clamp(spacing, STEREO_SPACING_MIN, STEREO_SPACING_MAX);
    g_stereo.size    = stereo__clamp(size, STEREO_SIZE_MIN, 1.0f);
    g_stereo.swap    = swap != 0;
    g_stereo.gen++;
}

/* The eye being drawn: -1 left, +1 right, 0 none (one picture). */
static inline int stereo_eye(void) { return g_stereo.on ? g_stereo.eye : 0; }

/* The parallax at infinity of the eye being drawn, in NDC (a picture is 2 wide). */
static inline float stereo_infinity_ndc(void) {
    return (float)stereo_eye() * 2.0f * g_stereo.depth;
}

/* The distance drawn at the screen's depth, in the board's eye units. */
static inline float stereo_focus_z(void) {
    float z = g_stereo.scene_z > 0.0f ? g_stereo.scene_z : 50.0f;
    return z * g_stereo.focus;
}

/* The frame's objects (their model-to-eye translations), once per host frame:
 * the median distance, eased in over a few frames so a cut does not jolt the
 * eyes. Objects behind the eye or nearer than one unit are left out: a
 * shadow card under the camera or a HUD model says nothing about the scene. */
static inline void stereo_note_scene(const float *zs, int n) {
    if (n <= 0) return;
    /* insertion sort of up to 64 samples, spread over the list */
    float s[64];
    int m = 0;
    const int step = n > 64 ? n / 64 : 1;
    for (int i = 0; i < n && m < 64; i += step) {
        float z = zs[i];
        if (!(z > 1.0f && z < 1.0e5f)) continue;
        int j = m++;
        while (j > 0 && s[j - 1] > z) { s[j] = s[j - 1]; j--; }
        s[j] = z;
    }
    if (m == 0) return;
    float med = s[m / 2];
    if (g_stereo.scene_z <= 0.0f) g_stereo.scene_z = med;
    else g_stereo.scene_z *= powf(med / g_stereo.scene_z, 0.08f);   /* eased in log space */
}

/* Fold the eye into the board's projection (row-major, x row m[0..3], w = -z). */
static inline void stereo_apply_projection(float *m) {
    const float k = stereo_infinity_ndc();
    if (k == 0.0f) return;
    const float D = stereo_focus_z();
    m[2] -= k;       /* k * w, w being -zh */
    m[3] -= k * D;   /* m0 * s, s = -k * D / m0 */
}

/* Which picture an eye gets, after the Swap setting: the left half shows the
 * left eye unless swapped. */
static inline int stereo_eye_of_half(int half_eye) { return g_stereo.swap ? -half_eye : half_eye; }

#endif /* STEREO_H */
