/*
 * mc_scene.h - a camera, keyframe tracks and a small timeline that knows the
 * beat.
 *
 * None of this allocates, and it's all small pure functions. The idea is that
 * one clock (ideally the audio clock) drives everything. The camera, the
 * animated values and the beat then stay locked to the same t, and the
 * visuals can't drift away from the music.
 */
#ifndef MICROCOSM_MC_SCENE_H
#define MICROCOSM_MC_SCENE_H

#include "mc.h"
#include "mc_math.h"

/* camera: pos/target/up plus the usual projection params */
typedef struct {
    mc_v3 pos;
    mc_v3 target;
    mc_v3 up;
    float fovy;     /* radians */
    float znear;
    float zfar;
} mc_camera;

/* Eye at (0, 1.5, 6) looking at the origin, +Y up, a 55 degree fovy, near
 * 0.05 and far 200. mc_post_defaults() doesn't match these: give the post
 * stack your camera's near, far and fov yourself (see mc_render.h). */
mc_camera mc_camera_default(void);
mc_m4     mc_camera_view(const mc_camera *c);
mc_m4     mc_camera_proj(const mc_camera *c, float aspect);

/* Frustum culling. mc_frustum_make takes the six clip planes (left, right,
 * bottom, top, near, far) of a view-projection matrix, for GL's -w..w clip
 * volume, normalised so distances come out in world units. mc_frustum_sphere
 * returns 0 when the sphere is wholly outside one plane and 1 otherwise. It's
 * conservative: a sphere just off a corner can pass, a visible one never
 * fails. */
typedef struct { mc_v4 plane[6]; } mc_frustum;
mc_frustum mc_frustum_make(mc_m4 view_proj);
int mc_frustum_sphere(const mc_frustum *f, mc_v3 center, float radius);

/* keyframe tracks: hand-authored value-over-time curves */
typedef struct { float t; float        v; int ease; } mc_key_f;
typedef struct { float t; mc_v3        v; int ease; } mc_key_v3;

/* Sample a track of any length at time `t` (keys must be sorted by .t).
 * At duplicate timestamps, the last key at that time wins.
 * `ease` on each key selects the interpolation into that key. */
float mc_track_f (const mc_key_f  *keys, int n, float t);
mc_v3 mc_track_v3(const mc_key_v3 *keys, int n, float t);

/* timeline: beats, pulses, and scene gating */
typedef struct {
    double t;         /* current time in seconds   */
    double duration;  /* total length in seconds   */
    double bpm;       /* beats per minute          */
} mc_timeline;

double mc_beats(const mc_timeline *tl);          /* t in beats */
/* A 0..1 pulse that jumps to 1 on each beat and decays until the next one,
 * handy for "bounce". A higher `sharpness` decays faster. */
float  mc_beat_pulse(const mc_timeline *tl, float sharpness);
/* 1 while t is within [start, start+len), else 0 (scene gating). */
int    mc_window_active(const mc_timeline *tl, double start, double len);
/* Normalised 0..1 progress within [start, start+len], clamped. */
float  mc_window_phase(const mc_timeline *tl, double start, double len);

#endif /* MICROCOSM_MC_SCENE_H */
