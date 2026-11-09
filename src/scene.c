/*
 * scene.c - the small stuff that turns a clock into a shot: camera math,
 * keyframe tracks, and a couple of beat/timeline helpers. None of it
 * allocates, and tracks live on the caller's stack. The header has the
 * contract.
 */
#include "microcosm/mc_scene.h"
#include <math.h>
#include "microcosm/mc_imports.h"   /* payload calls by hash; keep after system headers */

/* camera */
mc_camera mc_camera_default(void) {
    mc_camera c;
    c.pos    = mc_vec3(0.0f, 1.5f, 6.0f);
    c.target = mc_vec3(0.0f, 0.0f, 0.0f);
    c.up     = mc_vec3(0.0f, 1.0f, 0.0f);
    c.fovy   = mc_radians(55.0f);
    c.znear  = 0.05f;
    c.zfar   = 200.0f;
    return c;
}
mc_m4 mc_camera_view(const mc_camera *c) { return mc_m4_look_at(c->pos, c->target, c->up); }
mc_m4 mc_camera_proj(const mc_camera *c, float aspect) {
    return mc_m4_perspective(c->fovy, aspect, c->znear, c->zfar);
}

/* frustum culling, then the keyframe tracks */

mc_frustum mc_frustum_make(mc_m4 m) {
    mc_frustum f;
    for (int i = 0; i < 6; ++i) {
        int row = i / 2;
        float sign = (i & 1) ? -1.0f : 1.0f;
        mc_v4 p = mc_vec4(m.m[3] + sign * m.m[row],
                          m.m[7] + sign * m.m[4 + row],
                          m.m[11] + sign * m.m[8 + row],
                          m.m[15] + sign * m.m[12 + row]);
        float len = sqrtf(p.x*p.x + p.y*p.y + p.z*p.z);
        if (len > 0.0f) { p.x /= len; p.y /= len; p.z /= len; p.w /= len; }
        f.plane[i] = p;
    }
    return f;
}

int mc_frustum_sphere(const mc_frustum *f, mc_v3 c, float radius) {
    for (int i = 0; i < 6; ++i) {
        mc_v4 p = f->plane[i];
        if (p.x*c.x + p.y*c.y + p.z*c.z + p.w < -radius) return 0;
    }
    return 1;
}

/* Which segment is t in? Returns i with keys[i].t <= t < keys[i+1].t, and the
 * 0..1 position within that segment in *local. Clamps at both ends. */
static float key_time(const void *keys, size_t stride, int i) {
    /* Both public key types start with a float time. */
    return *(const float *)((const char *)keys + (size_t)i * stride);
}
static int track_segment(const void *keys, size_t stride, int n, float t, float *local) {
    if (t < key_time(keys, stride, 0)) { *local = 0.0f; return 0; }
    if (t >= key_time(keys, stride, n - 1)) { *local = 1.0f; return n - 2; }
    int i = 0;
    while (i < n - 2 && t >= key_time(keys, stride, i + 1)) ++i;
    float start = key_time(keys, stride, i);
    float span = key_time(keys, stride, i + 1) - start;
    *local = span > 0.0f ? (t - start) / span : 0.0f;
    return i;
}

float mc_track_f(const mc_key_f *keys, int n, float t) {
    if (n <= 0) return 0.0f;
    if (n == 1) return keys[0].v;
    float u;
    int i = track_segment(keys, sizeof *keys, n, t, &u);
    float e = mc_ease((mc_ease_kind)keys[i + 1].ease, u);
    return mc_lerpf(keys[i].v, keys[i + 1].v, e);
}

mc_v3 mc_track_v3(const mc_key_v3 *keys, int n, float t) {
    if (n <= 0) return mc_vec3(0, 0, 0);
    if (n == 1) return keys[0].v;
    float u;
    int i = track_segment(keys, sizeof *keys, n, t, &u);
    float e = mc_ease((mc_ease_kind)keys[i + 1].ease, u);
    return mc_v3_lerp(keys[i].v, keys[i + 1].v, e);
}

/* timeline: seconds in, musical quantities out */
double mc_beats(const mc_timeline *tl) { return tl->t * tl->bpm / 60.0; }

/* A pulse that's 1 on the beat and decays to 0. A bigger sharpness makes it
 * decay faster. The demo uses a plain sine of mc_beats instead, because a hard
 * pulse makes the motion stutter, but this is here when you want a thump. */
float mc_beat_pulse(const mc_timeline *tl, float sharpness) {
    float phase = (float)(mc_beats(tl) - floor(mc_beats(tl))); /* fraction into the current beat */
    float decay = powf(1.0f - phase, sharpness < 1.0f ? 1.0f : sharpness);
    return mc_saturatef(decay);
}

int mc_window_active(const mc_timeline *tl, double start, double len) {
    return (tl->t >= start && tl->t < start + len) ? 1 : 0;
}

float mc_window_phase(const mc_timeline *tl, double start, double len) {
    if (len <= 0.0) return 0.0f;
    double p = (tl->t - start) / len;
    return mc_saturatef((float)p);
}
