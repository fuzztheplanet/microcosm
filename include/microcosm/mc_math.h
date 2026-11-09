/*
 * mc_math.h - the math a 3D demo needs: vectors, 4x4 matrices, quaternions,
 * noise, easing curves and splines.
 *
 * Conventions:
 *   - Everything is a small value type, passed and returned by value, with no
 *     pointers-to-float and no hidden allocation.
 *   - Matrices are column-major in m[16], as OpenGL expects: m[0..3] is
 *     column 0, m[4..7] column 1, and so on. Read them row-major and your
 *     transforms come out transposed.
 *   - Angles are in radians unless the name says degrees.
 *   - Coordinates are right-handed. mc_m4_look_at's camera looks down -Z, and
 *     mc_m4_perspective and mc_m4_ortho map depth to GL's -1..1 clip range.
 * The implementations are in src/math.c. With LTO the hot ones can be inlined.
 */
#ifndef MICROCOSM_MC_MATH_H
#define MICROCOSM_MC_MATH_H

#include "mc.h"

typedef struct { float x, y; }       mc_v2;
typedef struct { float x, y, z; }    mc_v3;
typedef struct { float x, y, z, w; } mc_v4;
typedef struct { float m[16]; }      mc_m4;   /* column-major */
typedef struct { float x, y, z, w; } mc_quat; /* (x,y,z,w), w is the scalar */

/* constructors */
mc_v2 mc_vec2(float x, float y);
mc_v3 mc_vec3(float x, float y, float z);
mc_v4 mc_vec4(float x, float y, float z, float w);
mc_v3 mc_v3_splat(float s);

/* scalar helpers, mostly float versions of things <math.h> has in double */
float mc_absf(float x);
float mc_signf(float x);
float mc_minf(float a, float b);
float mc_maxf(float a, float b);
float mc_clampf(float x, float lo, float hi);
float mc_saturatef(float x);                 /* clamp to [0,1] */
float mc_lerpf(float a, float b, float t);
float mc_fractf(float x);
float mc_modf_pos(float x, float m);         /* positive modulo */
float mc_smoothstepf(float e0, float e1, float x);
float mc_radians(float deg);
float mc_degrees(float rad);
float mc_remapf(float x, float a0, float a1, float b0, float b1);

/* vectors */
mc_v2 mc_v2_add(mc_v2 a, mc_v2 b);
mc_v2 mc_v2_sub(mc_v2 a, mc_v2 b);
mc_v2 mc_v2_scale(mc_v2 a, float s);
float mc_v2_dot(mc_v2 a, mc_v2 b);
float mc_v2_len(mc_v2 a);
mc_v2 mc_v2_normalize(mc_v2 a);

mc_v3 mc_v3_add(mc_v3 a, mc_v3 b);
mc_v3 mc_v3_sub(mc_v3 a, mc_v3 b);
mc_v3 mc_v3_mul(mc_v3 a, mc_v3 b);           /* component-wise */
mc_v3 mc_v3_scale(mc_v3 a, float s);
mc_v3 mc_v3_neg(mc_v3 a);
float mc_v3_dot(mc_v3 a, mc_v3 b);
mc_v3 mc_v3_cross(mc_v3 a, mc_v3 b);
float mc_v3_len(mc_v3 a);
float mc_v3_len2(mc_v3 a);                    /* squared length */
float mc_v3_dist(mc_v3 a, mc_v3 b);
mc_v3 mc_v3_mad(mc_v3 a, mc_v3 b, float s);   /* a + b * s */
mc_v3 mc_v3_dir(mc_v3 a, mc_v3 b);            /* normalize(a - b): the way from b to a */
mc_v3 mc_v3_normalize(mc_v3 a);              /* safe: returns {0,0,0} if ~zero */
mc_v3 mc_v3_lerp(mc_v3 a, mc_v3 b, float t);
mc_v3 mc_v3_min(mc_v3 a, mc_v3 b);
mc_v3 mc_v3_max(mc_v3 a, mc_v3 b);
mc_v3 mc_v3_abs(mc_v3 a);
mc_v3 mc_v3_reflect(mc_v3 i, mc_v3 n);
mc_v3 mc_v3_clamp(mc_v3 v, float lo, float hi);

mc_v4 mc_v4_add(mc_v4 a, mc_v4 b);
mc_v4 mc_v4_scale(mc_v4 a, float s);
float mc_v4_dot(mc_v4 a, mc_v4 b);

/* 4x4 matrices (column-major) */
mc_m4 mc_m4_identity(void);
mc_m4 mc_m4_mul(mc_m4 a, mc_m4 b);           /* a * b */
mc_m4 mc_m4_transpose(mc_m4 a);
mc_m4 mc_m4_inverse(mc_m4 a);                /* general inverse */
mc_m4 mc_m4_translate(mc_v3 t);
mc_m4 mc_m4_scale(mc_v3 s);
mc_m4 mc_m4_scale1(float s);
mc_m4 mc_m4_rotate(mc_v3 axis, float angle);
mc_m4 mc_m4_from_quat(mc_quat q);
mc_m4 mc_m4_trs(mc_v3 t, mc_quat r, mc_v3 s);/* translate * rotate * scale */
mc_m4 mc_m4_look_at(mc_v3 eye, mc_v3 center, mc_v3 up);
mc_m4 mc_m4_perspective(float fovy, float aspect, float znear, float zfar);
mc_m4 mc_m4_ortho(float l, float r, float b, float t, float n, float f);
mc_v4 mc_m4_mul_v4(mc_m4 a, mc_v4 v);
mc_v3 mc_m4_transform_point(mc_m4 a, mc_v3 p);
mc_v3 mc_m4_transform_dir(mc_m4 a, mc_v3 d);

/* quaternions, for rotations that interpolate smoothly */
mc_quat mc_quat_identity(void);
mc_quat mc_quat_axis_angle(mc_v3 axis, float angle);
mc_quat mc_quat_euler(float x, float y, float z); /* radians, XYZ order */
mc_quat mc_quat_mul(mc_quat a, mc_quat b);
mc_quat mc_quat_normalize(mc_quat q);
mc_quat mc_quat_slerp(mc_quat a, mc_quat b, float t);
mc_v3   mc_quat_rotate(mc_quat q, mc_v3 v);

/* hashing and a throwaway PRNG */
mc_u32 mc_hash_u32(mc_u32 x);
mc_u32 mc_hash2(mc_u32 x, mc_u32 y);

/* A tiny deterministic PRNG (xorshift-ish). The caller holds its state. */
typedef struct { mc_u32 s; } mc_rng;
mc_rng mc_rng_seed(mc_u32 seed);
mc_u32 mc_rng_next(mc_rng *r);
float  mc_rng_f(mc_rng *r);                   /* [0,1) */
float  mc_rng_range(mc_rng *r, float lo, float hi);
mc_v3  mc_rng_unit_v3(mc_rng *r);             /* uniform point on unit sphere */

/* value/gradient noise */
/* Noise in [-1,1]. fbm sums octaves of it, and curl gives divergence-free flow. */
float mc_noise2(float x, float y);
float mc_noise3(float x, float y, float z);
float mc_fbm2(float x, float y, int octaves, float lacunarity, float gain);
float mc_fbm3(mc_v3 p, int octaves, float lacunarity, float gain);
mc_v3 mc_curl3(mc_v3 p);

/* easing curves, t in [0,1] */
typedef enum {
    MC_EASE_LINEAR = 0,
    MC_EASE_IN_QUAD, MC_EASE_OUT_QUAD, MC_EASE_INOUT_QUAD,
    MC_EASE_IN_CUBIC, MC_EASE_OUT_CUBIC, MC_EASE_INOUT_CUBIC,
    MC_EASE_OUT_BACK, MC_EASE_OUT_ELASTIC, MC_EASE_OUT_BOUNCE,
    MC_EASE_INOUT_SINE,
    MC_EASE_COUNT
} mc_ease_kind;

float mc_ease(mc_ease_kind kind, float t);   /* t clamped to [0,1] */

/* splines */
mc_v3 mc_catmull_rom(mc_v3 p0, mc_v3 p1, mc_v3 p2, mc_v3 p3, float t);
mc_v3 mc_bezier3(mc_v3 p0, mc_v3 p1, mc_v3 p2, mc_v3 p3, float t);
/* Sample a Catmull-Rom path through `n` control points at u in [0,1].
 * If loop != 0 the path wraps. Otherwise the endpoints are duplicated. */
mc_v3 mc_spline_sample(const mc_v3 *pts, int n, float u, int loop);

#endif /* MICROCOSM_MC_MATH_H */
