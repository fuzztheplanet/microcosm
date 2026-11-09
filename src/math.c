/*
 * math.c - the guts of mc_math.h.
 *
 * Plain float math on top of libm. Two things to keep in mind: matrices are
 * column-major (OpenGL's convention, so we can hand them straight to
 * glUniformMatrix4fv), and the noise is value noise with a cheap integer hash.
 * It only wobbles positions and drives motion, so it doesn't need to be Perlin.
 */
#include "microcosm/mc_math.h"
#include <math.h>
#include "microcosm/mc_imports.h"   /* payload calls by hash; keep after system headers */

/* constructors */
mc_v2 mc_vec2(float x, float y)                   { return (mc_v2){x, y}; }
mc_v3 mc_vec3(float x, float y, float z)          { return (mc_v3){x, y, z}; }
mc_v4 mc_vec4(float x, float y, float z, float w) { return (mc_v4){x, y, z, w}; }
mc_v3 mc_v3_splat(float s)                        { return (mc_v3){s, s, s}; }

/* scalar helpers */
float mc_absf(float x)  { return x < 0.0f ? -x : x; }
float mc_signf(float x) { return x < 0.0f ? -1.0f : (x > 0.0f ? 1.0f : 0.0f); }
float mc_minf(float a, float b) { return a < b ? a : b; }
float mc_maxf(float a, float b) { return a > b ? a : b; }
float mc_clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
float mc_saturatef(float x) { return mc_clampf(x, 0.0f, 1.0f); }
float mc_lerpf(float a, float b, float t) { return a + (b - a) * t; }
float mc_fractf(float x) { return x - floorf(x); }
float mc_modf_pos(float x, float m) { float r = fmodf(x, m); return r < 0.0f ? r + m : r; }
float mc_radians(float d) { return d * MC_DEG2RAD; }
float mc_degrees(float r) { return r * MC_RAD2DEG; }

float mc_smoothstepf(float e0, float e1, float x) {
    float t = mc_saturatef((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}
float mc_remapf(float x, float a0, float a1, float b0, float b1) {
    return b0 + (x - a0) * (b1 - b0) / (a1 - a0);
}

/* vec2 */
mc_v2 mc_v2_add(mc_v2 a, mc_v2 b)   { return (mc_v2){a.x + b.x, a.y + b.y}; }
mc_v2 mc_v2_sub(mc_v2 a, mc_v2 b)   { return (mc_v2){a.x - b.x, a.y - b.y}; }
mc_v2 mc_v2_scale(mc_v2 a, float s) { return (mc_v2){a.x * s, a.y * s}; }
float mc_v2_dot(mc_v2 a, mc_v2 b)   { return a.x * b.x + a.y * b.y; }
float mc_v2_len(mc_v2 a)            { return sqrtf(mc_v2_dot(a, a)); }
mc_v2 mc_v2_normalize(mc_v2 a) {
    float l = mc_v2_len(a);
    return l > 1e-8f ? mc_v2_scale(a, 1.0f / l) : (mc_v2){0, 0};
}

/* vec3, the workhorse */
mc_v3 mc_v3_add(mc_v3 a, mc_v3 b)   { return (mc_v3){a.x + b.x, a.y + b.y, a.z + b.z}; }
mc_v3 mc_v3_sub(mc_v3 a, mc_v3 b)   { return (mc_v3){a.x - b.x, a.y - b.y, a.z - b.z}; }
mc_v3 mc_v3_mul(mc_v3 a, mc_v3 b)   { return (mc_v3){a.x * b.x, a.y * b.y, a.z * b.z}; }
mc_v3 mc_v3_scale(mc_v3 a, float s) { return (mc_v3){a.x * s, a.y * s, a.z * s}; }
mc_v3 mc_v3_neg(mc_v3 a)            { return (mc_v3){-a.x, -a.y, -a.z}; }
float mc_v3_dot(mc_v3 a, mc_v3 b)   { return a.x * b.x + a.y * b.y + a.z * b.z; }
float mc_v3_len2(mc_v3 a)           { return mc_v3_dot(a, a); }
float mc_v3_len(mc_v3 a)            { return sqrtf(mc_v3_len2(a)); }
float mc_v3_dist(mc_v3 a, mc_v3 b)  { return mc_v3_len(mc_v3_sub(a, b)); }
/* a + b * s in one call. The demo does this add-and-scale pair all the time. */
mc_v3 mc_v3_mad(mc_v3 a, mc_v3 b, float s) { return (mc_v3){a.x + b.x * s, a.y + b.y * s, a.z + b.z * s}; }
mc_v3 mc_v3_cross(mc_v3 a, mc_v3 b) {
    return (mc_v3){a.y * b.z - a.z * b.y,
                   a.z * b.x - a.x * b.z,
                   a.x * b.y - a.y * b.x};
}
mc_v3 mc_v3_normalize(mc_v3 a) {
    float l = mc_v3_len(a);
    return l > 1e-8f ? mc_v3_scale(a, 1.0f / l) : (mc_v3){0, 0, 0};
}
mc_v3 mc_v3_dir(mc_v3 a, mc_v3 b) { return mc_v3_normalize(mc_v3_sub(a, b)); }
mc_v3 mc_v3_lerp(mc_v3 a, mc_v3 b, float t) {
    return (mc_v3){mc_lerpf(a.x, b.x, t), mc_lerpf(a.y, b.y, t), mc_lerpf(a.z, b.z, t)};
}
mc_v3 mc_v3_min(mc_v3 a, mc_v3 b) { return (mc_v3){mc_minf(a.x,b.x), mc_minf(a.y,b.y), mc_minf(a.z,b.z)}; }
mc_v3 mc_v3_max(mc_v3 a, mc_v3 b) { return (mc_v3){mc_maxf(a.x,b.x), mc_maxf(a.y,b.y), mc_maxf(a.z,b.z)}; }
mc_v3 mc_v3_abs(mc_v3 a) { return (mc_v3){mc_absf(a.x), mc_absf(a.y), mc_absf(a.z)}; }
mc_v3 mc_v3_reflect(mc_v3 i, mc_v3 n) { return mc_v3_sub(i, mc_v3_scale(n, 2.0f * mc_v3_dot(i, n))); }
mc_v3 mc_v3_clamp(mc_v3 v, float lo, float hi) {
    return (mc_v3){mc_clampf(v.x,lo,hi), mc_clampf(v.y,lo,hi), mc_clampf(v.z,lo,hi)};
}

/* vec4 (just the handful we actually use) */
mc_v4 mc_v4_add(mc_v4 a, mc_v4 b)   { return (mc_v4){a.x+b.x, a.y+b.y, a.z+b.z, a.w+b.w}; }
mc_v4 mc_v4_scale(mc_v4 a, float s) { return (mc_v4){a.x*s, a.y*s, a.z*s, a.w*s}; }
float mc_v4_dot(mc_v4 a, mc_v4 b)   { return a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w; }

/* mat4, column-major: element (row r, col c) lives at m[c*4 + r]. */
mc_m4 mc_m4_identity(void) {
    mc_m4 r = {{0}};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}
mc_m4 mc_m4_mul(mc_m4 a, mc_m4 b) {
    mc_m4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k)
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}
mc_m4 mc_m4_transpose(mc_m4 a) {
    mc_m4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            r.m[c * 4 + row] = a.m[row * 4 + c];
    return r;
}
mc_m4 mc_m4_translate(mc_v3 t) {
    mc_m4 r = mc_m4_identity();
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}
mc_m4 mc_m4_scale(mc_v3 s) {
    mc_m4 r = {{0}};
    r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z; r.m[15] = 1.0f;
    return r;
}
mc_m4 mc_m4_scale1(float s) { return mc_m4_scale(mc_v3_splat(s)); }

mc_m4 mc_m4_rotate(mc_v3 axis, float angle) {
    mc_v3 a = mc_v3_normalize(axis);
    float c = cosf(angle), s = sinf(angle), ic = 1.0f - c;
    mc_m4 r = mc_m4_identity();
    r.m[0]  = c + a.x * a.x * ic;
    r.m[1]  = a.y * a.x * ic + a.z * s;
    r.m[2]  = a.z * a.x * ic - a.y * s;
    r.m[4]  = a.x * a.y * ic - a.z * s;
    r.m[5]  = c + a.y * a.y * ic;
    r.m[6]  = a.z * a.y * ic + a.x * s;
    r.m[8]  = a.x * a.z * ic + a.y * s;
    r.m[9]  = a.y * a.z * ic - a.x * s;
    r.m[10] = c + a.z * a.z * ic;
    return r;
}
mc_m4 mc_m4_from_quat(mc_quat q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float xx = x*x, yy = y*y, zz = z*z;
    float xy = x*y, xz = x*z, yz = y*z;
    float wx = w*x, wy = w*y, wz = w*z;
    mc_m4 r = mc_m4_identity();
    r.m[0]  = 1 - 2*(yy + zz); r.m[1]  = 2*(xy + wz);     r.m[2]  = 2*(xz - wy);
    r.m[4]  = 2*(xy - wz);     r.m[5]  = 1 - 2*(xx + zz); r.m[6]  = 2*(yz + wx);
    r.m[8]  = 2*(xz + wy);     r.m[9]  = 2*(yz - wx);     r.m[10] = 1 - 2*(xx + yy);
    return r;
}
mc_m4 mc_m4_trs(mc_v3 t, mc_quat rot, mc_v3 s) {
    return mc_m4_mul(mc_m4_translate(t), mc_m4_mul(mc_m4_from_quat(rot), mc_m4_scale(s)));
}
mc_m4 mc_m4_look_at(mc_v3 eye, mc_v3 center, mc_v3 up) {
    mc_v3 f = mc_v3_dir(center, eye);
    mc_v3 s = mc_v3_normalize(mc_v3_cross(f, up));
    mc_v3 u = mc_v3_cross(s, f);
    mc_m4 r = mc_m4_identity();
    r.m[0] = s.x; r.m[4] = s.y; r.m[8]  = s.z;
    r.m[1] = u.x; r.m[5] = u.y; r.m[9]  = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -mc_v3_dot(s, eye);
    r.m[13] = -mc_v3_dot(u, eye);
    r.m[14] =  mc_v3_dot(f, eye);
    return r;
}
mc_m4 mc_m4_perspective(float fovy, float aspect, float znear, float zfar) {
    float f = 1.0f / tanf(fovy * 0.5f);
    mc_m4 r = {{0}};
    r.m[0]  = f / aspect;
    r.m[5]  = f;
    r.m[10] = (zfar + znear) / (znear - zfar);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * zfar * znear) / (znear - zfar);
    return r;
}
mc_m4 mc_m4_ortho(float l, float rr, float b, float t, float n, float f) {
    mc_m4 r = mc_m4_identity();
    r.m[0]  = 2.0f / (rr - l);
    r.m[5]  = 2.0f / (t - b);
    r.m[10] = -2.0f / (f - n);
    r.m[12] = -(rr + l) / (rr - l);
    r.m[13] = -(t + b) / (t - b);
    r.m[14] = -(f + n) / (f - n);
    return r;
}
mc_v4 mc_m4_mul_v4(mc_m4 a, mc_v4 v) {
    return (mc_v4){
        a.m[0]*v.x + a.m[4]*v.y + a.m[8]*v.z  + a.m[12]*v.w,
        a.m[1]*v.x + a.m[5]*v.y + a.m[9]*v.z  + a.m[13]*v.w,
        a.m[2]*v.x + a.m[6]*v.y + a.m[10]*v.z + a.m[14]*v.w,
        a.m[3]*v.x + a.m[7]*v.y + a.m[11]*v.z + a.m[15]*v.w};
}
mc_v3 mc_m4_transform_point(mc_m4 a, mc_v3 p) {
    mc_v4 r = mc_m4_mul_v4(a, (mc_v4){p.x, p.y, p.z, 1.0f});
    float iw = (r.w != 0.0f) ? 1.0f / r.w : 1.0f;
    return (mc_v3){r.x * iw, r.y * iw, r.z * iw};
}
mc_v3 mc_m4_transform_dir(mc_m4 a, mc_v3 d) {
    mc_v4 r = mc_m4_mul_v4(a, (mc_v4){d.x, d.y, d.z, 0.0f});
    return (mc_v3){r.x, r.y, r.z};
}
/* Full 4x4 inverse by cofactors. Not pretty, but it's branch-free and we only
 * call it off the hot path. A singular matrix returns identity instead of inf. */
mc_m4 mc_m4_inverse(mc_m4 a) {
    const float *m = a.m;
    float inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];
    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    mc_m4 r;
    if (det == 0.0f) return mc_m4_identity();
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) r.m[i] = inv[i] * det;
    return r;
}

/* quaternions (x,y,z,w), with w the scalar part */
mc_quat mc_quat_identity(void) { return (mc_quat){0, 0, 0, 1}; }
mc_quat mc_quat_axis_angle(mc_v3 axis, float angle) {
    mc_v3 a = mc_v3_normalize(axis);
    float h = angle * 0.5f, s = sinf(h);
    return (mc_quat){a.x * s, a.y * s, a.z * s, cosf(h)};
}
mc_quat mc_quat_euler(float x, float y, float z) {
    mc_quat qx = mc_quat_axis_angle((mc_v3){1,0,0}, x);
    mc_quat qy = mc_quat_axis_angle((mc_v3){0,1,0}, y);
    mc_quat qz = mc_quat_axis_angle((mc_v3){0,0,1}, z);
    return mc_quat_mul(qz, mc_quat_mul(qy, qx));
}
mc_quat mc_quat_mul(mc_quat a, mc_quat b) {
    return (mc_quat){
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z};
}
mc_quat mc_quat_normalize(mc_quat q) {
    float l = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if (l < 1e-8f) return mc_quat_identity();
    float i = 1.0f / l;
    return (mc_quat){q.x*i, q.y*i, q.z*i, q.w*i};
}
mc_quat mc_quat_slerp(mc_quat a, mc_quat b, float t) {
    float d = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
    if (d < 0.0f) { b = (mc_quat){-b.x, -b.y, -b.z, -b.w}; d = -d; }
    if (d > 0.9995f) { /* nearly the same orientation: a plain lerp will do */
        mc_quat r = {mc_lerpf(a.x,b.x,t), mc_lerpf(a.y,b.y,t),
                     mc_lerpf(a.z,b.z,t), mc_lerpf(a.w,b.w,t)};
        return mc_quat_normalize(r);
    }
    float th0 = acosf(d), th = th0 * t;
    float s0 = cosf(th) - d * sinf(th) / sinf(th0);
    float s1 = sinf(th) / sinf(th0);
    return (mc_quat){a.x*s0 + b.x*s1, a.y*s0 + b.y*s1,
                     a.z*s0 + b.z*s1, a.w*s0 + b.w*s1};
}
mc_v3 mc_quat_rotate(mc_quat q, mc_v3 v) {
    mc_v3 u = {q.x, q.y, q.z};
    float s = q.w;
    mc_v3 t = mc_v3_scale(mc_v3_cross(u, v), 2.0f);
    return mc_v3_add(v, mc_v3_mad(mc_v3_cross(u, t), t, s));
}

/* hashing + RNG. The hash is the lowbias32 integer finalizer, and the
 * generator is a 32-bit xorshift. Both are deterministic, because every run of
 * the demo has to look the same. */
mc_u32 mc_hash_u32(mc_u32 x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
mc_u32 mc_hash2(mc_u32 x, mc_u32 y) { return mc_hash_u32(x ^ (mc_hash_u32(y) + 0x9e3779b9U)); }

mc_rng mc_rng_seed(mc_u32 seed) { return (mc_rng){seed ? seed : 0x1234567u}; }
mc_u32 mc_rng_next(mc_rng *r) {
    mc_u32 x = r->s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    r->s = x;
    return x;
}
float mc_rng_f(mc_rng *r) { return (float)(mc_rng_next(r) >> 8) / (float)(1u << 24); }
float mc_rng_range(mc_rng *r, float lo, float hi) { return lo + (hi - lo) * mc_rng_f(r); }
mc_v3 mc_rng_unit_v3(mc_rng *r) {
    /* Pick z uniformly, then an azimuth. Archimedes' hat-box trick gives an
     * even spread over the sphere without rejection sampling. */
    float z = mc_rng_range(r, -1.0f, 1.0f);
    float a = mc_rng_range(r, 0.0f, MC_TAU);
    float s = sqrtf(mc_maxf(0.0f, 1.0f - z * z));
    return (mc_v3){s * cosf(a), s * sinf(a), z};
}

/* value noise + fBm */
static float hash_to_unit(mc_u32 h) { return (float)(h >> 8) / (float)(1u << 24) * 2.0f - 1.0f; }
static float vhash3(int x, int y, int z) {
    mc_u32 h = mc_hash_u32((mc_u32)x * 374761393u + (mc_u32)y * 668265263u + (mc_u32)z * 2147483647u + 0x9e3779b9u);
    return hash_to_unit(h);
}
/* Perlin's quintic fade. Its 1st and 2nd derivatives are flat at 0 and 1, so
 * cells blend without the usual value-noise grid creases. */
static float smooth5(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

float mc_noise3(float x, float y, float z) {
    float fx = floorf(x), fy = floorf(y), fz = floorf(z);
    int ix = (int)fx, iy = (int)fy, iz = (int)fz;
    float tx = smooth5(x - fx), ty = smooth5(y - fy), tz = smooth5(z - fz);
    float c000 = vhash3(ix,   iy,   iz  ), c100 = vhash3(ix+1, iy,   iz  );
    float c010 = vhash3(ix,   iy+1, iz  ), c110 = vhash3(ix+1, iy+1, iz  );
    float c001 = vhash3(ix,   iy,   iz+1), c101 = vhash3(ix+1, iy,   iz+1);
    float c011 = vhash3(ix,   iy+1, iz+1), c111 = vhash3(ix+1, iy+1, iz+1);
    float x00 = mc_lerpf(c000, c100, tx), x10 = mc_lerpf(c010, c110, tx);
    float x01 = mc_lerpf(c001, c101, tx), x11 = mc_lerpf(c011, c111, tx);
    float y0  = mc_lerpf(x00, x10, ty),   y1  = mc_lerpf(x01, x11, ty);
    return mc_lerpf(y0, y1, tz);
}
float mc_noise2(float x, float y) { return mc_noise3(x, y, 0.0f); }

float mc_fbm3(mc_v3 p, int octaves, float lacunarity, float gain) {
    float sum = 0.0f, amp = 0.5f, freq = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum  += amp * mc_noise3(p.x * freq, p.y * freq, p.z * freq);
        norm += amp;
        freq *= lacunarity;
        amp  *= gain;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}
float mc_fbm2(float x, float y, int octaves, float lacunarity, float gain) {
    return mc_fbm3((mc_v3){x, y, 0.0f}, octaves, lacunarity, gain);
}
/* Curl of a noise field. Finite-difference the three partials and combine them
 * the way curl does. The result is divergence-free, so curl noise looks like
 * swirling flow rather than random drift. The odd constant offsets
 * decorrelate the three component fields. */
mc_v3 mc_curl3(mc_v3 p) {
    const float e = 0.1f;
    mc_v3 dx = {e, 0, 0}, dy = {0, e, 0}, dz = {0, 0, e};
    #define N(pp, o) mc_noise3((pp).x + (o), (pp).y - (o), (pp).z + (o))
    float x1 = N(mc_v3_add(p, dy), 0.0f) - N(mc_v3_sub(p, dy), 0.0f);
    float x2 = N(mc_v3_add(p, dz), 1.7f) - N(mc_v3_sub(p, dz), 1.7f);
    float y1 = N(mc_v3_add(p, dz), 3.1f) - N(mc_v3_sub(p, dz), 3.1f);
    float y2 = N(mc_v3_add(p, dx), 0.0f) - N(mc_v3_sub(p, dx), 0.0f);
    float z1 = N(mc_v3_add(p, dx), 5.3f) - N(mc_v3_sub(p, dx), 5.3f);
    float z2 = N(mc_v3_add(p, dy), 1.7f) - N(mc_v3_sub(p, dy), 1.7f);
    #undef N
    float inv = 1.0f / (2.0f * e);
    return mc_v3_normalize((mc_v3){(x1 - x2) * inv, (y1 - y2) * inv, (z1 - z2) * inv});
}

/* easing curves, the usual Penner/easings.net family */
static float ease_out_back(float t)   { const float c1 = 1.70158f, c3 = c1 + 1.0f;
                                        float u = t - 1.0f; return 1.0f + c3*u*u*u + c1*u*u; }
static float ease_out_elastic(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const float c4 = MC_TAU / 3.0f;
    return powf(2.0f, -10.0f * t) * sinf((t * 10.0f - 0.75f) * c4) + 1.0f;
}
static float ease_out_bounce(float t) {
    const float n1 = 7.5625f, d1 = 2.75f;
    if (t < 1.0f/d1)      return n1*t*t;
    else if (t < 2.0f/d1) { t -= 1.5f/d1;  return n1*t*t + 0.75f; }
    else if (t < 2.5f/d1) { t -= 2.25f/d1; return n1*t*t + 0.9375f; }
    else                  { t -= 2.625f/d1; return n1*t*t + 0.984375f; }
}
float mc_ease(mc_ease_kind k, float t) {
    t = mc_saturatef(t);
    switch (k) {
    case MC_EASE_IN_QUAD:    return t * t;
    case MC_EASE_OUT_QUAD:   return t * (2.0f - t);
    case MC_EASE_INOUT_QUAD: return t < 0.5f ? 2*t*t : -1.0f + (4.0f - 2.0f*t) * t;
    case MC_EASE_IN_CUBIC:   return t * t * t;
    case MC_EASE_OUT_CUBIC:  { float u = t - 1.0f; return u*u*u + 1.0f; }
    case MC_EASE_INOUT_CUBIC:return t < 0.5f ? 4*t*t*t : 1.0f + (t-1.0f)*(2.0f*t-2.0f)*(2.0f*t-2.0f);
    case MC_EASE_OUT_BACK:   return ease_out_back(t);
    case MC_EASE_OUT_ELASTIC:return ease_out_elastic(t);
    case MC_EASE_OUT_BOUNCE: return ease_out_bounce(t);
    case MC_EASE_INOUT_SINE: return -(cosf(MC_PI * t) - 1.0f) * 0.5f;
    case MC_EASE_LINEAR:
    default:                 return t;
    }
}

/* splines. Catmull-Rom passes through its control points, which is why the demo
 * camera uses it: you place waypoints and the curve goes through them. */
mc_v3 mc_catmull_rom(mc_v3 p0, mc_v3 p1, mc_v3 p2, mc_v3 p3, float t) {
    float t2 = t * t, t3 = t2 * t;
    mc_v3 a = mc_v3_scale(p1, 2.0f);
    mc_v3 b = mc_v3_scale(mc_v3_sub(p2, p0), t);
    mc_v3 c = mc_v3_scale(mc_v3_add(mc_v3_sub(mc_v3_scale(p0, 2.0f), mc_v3_scale(p1, 5.0f)),
                                    mc_v3_sub(mc_v3_scale(p2, 4.0f), p3)), t2);
    mc_v3 d = mc_v3_scale(mc_v3_add(mc_v3_sub(mc_v3_scale(p1, 3.0f), p0),
                                    mc_v3_sub(p3, mc_v3_scale(p2, 3.0f))), t3);
    return mc_v3_scale(mc_v3_add(a, mc_v3_add(b, mc_v3_add(c, d))), 0.5f);
}
mc_v3 mc_bezier3(mc_v3 p0, mc_v3 p1, mc_v3 p2, mc_v3 p3, float t) {
    float u = 1.0f - t;
    float b0 = u*u*u, b1 = 3*u*u*t, b2 = 3*u*t*t, b3 = t*t*t;
    return (mc_v3){
        p0.x*b0 + p1.x*b1 + p2.x*b2 + p3.x*b3,
        p0.y*b0 + p1.y*b1 + p2.y*b2 + p3.y*b3,
        p0.z*b0 + p1.z*b1 + p2.z*b2 + p3.z*b3};
}
mc_v3 mc_spline_sample(const mc_v3 *pts, int n, float u, int loop) {
    if (n <= 0) return (mc_v3){0, 0, 0};
    if (n == 1) return pts[0];
    u = mc_saturatef(u);
    int segs = loop ? n : (n - 1);
    float f = u * (float)segs;
    int i = (int)f;
    if (i >= segs) i = segs - 1;
    float t = f - (float)i;
    int i0 = loop ? (i - 1 + n) % n : (i - 1 < 0 ? 0 : i - 1);
    int i1 = loop ? i % n : i;
    int i2 = loop ? (i + 1) % n : (i + 1 >= n ? n - 1 : i + 1);
    int i3 = loop ? (i + 2) % n : (i + 2 >= n ? n - 1 : i + 2);
    return mc_catmull_rom(pts[i0], pts[i1], pts[i2], pts[i3], t);
}
