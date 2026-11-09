/*
 * mesh.c - the CPU side of mc_mesh.h. Everything here builds geometry, and
 * nothing here talks to the GPU (that's render.c's job).
 *
 * A mesh is an indexed triangle list of interleaved (position, normal, UV)
 * vertices, kept in two growable arrays. Two rules hold throughout, and the
 * lighting depends on them:
 *   - front faces are wound CCW seen from outside, and
 *   - normals point outward.
 * Get the winding backwards on a new primitive and it'll light inside-out.
 * Primitives are centred on the origin. A constructor hands you a finished
 * mesh that you own and free with mc_mesh_free.
 */
#include "microcosm/mc_mesh.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "microcosm/mc_imports.h"   /* payload calls by hash; keep after system headers */

/* Growable arrays with the usual doubling. A failed realloc is fatal: these
 * meshes are built once at startup (or in offline tooling), so there's nothing
 * useful to do about running out of memory here except stop with a message. */
static void mesh_reserve_verts(mc_mesh *m, int need) {
    if (need <= m->vert_cap) return;
    int cap = m->vert_cap ? m->vert_cap : 16;
    while (cap < need) cap *= 2;
    mc_vertex *p = realloc(m->verts, (size_t)cap * sizeof *p);
    if (!p) mc_fatal("mesh: out of memory (verts)");
    m->verts = p;
    m->vert_cap = cap;
}
static void mesh_reserve_indices(mc_mesh *m, int need) {
    if (need <= m->index_cap) return;
    int cap = m->index_cap ? m->index_cap : 32;
    while (cap < need) cap *= 2;
    mc_u32 *p = realloc(m->indices, (size_t)cap * sizeof *p);
    if (!p) mc_fatal("mesh: out of memory (indices)");
    m->indices = p;
    m->index_cap = cap;
}

/* the raw building blocks everything else is made of */
mc_mesh mc_mesh_new(void) {
    mc_mesh m;
    memset(&m, 0, sizeof m);
    return m;
}
void mc_mesh_free(mc_mesh *m) {
    if (!m) return;
    free(m->verts);
    free(m->indices);
    memset(m, 0, sizeof *m);        /* leave a clean, reusable zeroed struct */
}
int mc_mesh_push_vertex(mc_mesh *m, mc_v3 pos, mc_v3 normal) {
    mesh_reserve_verts(m, m->vert_count + 1);
    int idx = m->vert_count++;
    m->verts[idx].pos = pos;
    m->verts[idx].normal = normal;
    m->verts[idx].uv = mc_vec2(0.0f, 0.0f);   /* builders that need UVs set them afterward */
    return idx;
}
void mc_mesh_push_tri(mc_mesh *m, mc_u32 a, mc_u32 b, mc_u32 c) {
    mesh_reserve_indices(m, m->index_count + 3);
    m->indices[m->index_count++] = a;
    m->indices[m->index_count++] = b;
    m->indices[m->index_count++] = c;
}

/* Smooth per-vertex normals. Zero them, add each face's unnormalized cross
 * product to its three corners, then normalize. The raw cross product weights
 * each face by its area for free. Degenerate triangles add a near-zero vector
 * and drop out on their own, and mc_v3_normalize tolerates a zero input, so
 * there's nothing to special-case. */
void mc_mesh_recompute_normals(mc_mesh *m) {
    for (int i = 0; i < m->vert_count; ++i)
        m->verts[i].normal = mc_vec3(0.0f, 0.0f, 0.0f);
    for (int t = 0; t + 2 < m->index_count; t += 3) {
        mc_u32 a = m->indices[t], b = m->indices[t + 1], c = m->indices[t + 2];
        mc_v3 fn = mc_v3_cross(mc_v3_sub(m->verts[b].pos, m->verts[a].pos),
                               mc_v3_sub(m->verts[c].pos, m->verts[a].pos));
        m->verts[a].normal = mc_v3_add(m->verts[a].normal, fn);
        m->verts[b].normal = mc_v3_add(m->verts[b].normal, fn);
        m->verts[c].normal = mc_v3_add(m->verts[c].normal, fn);
    }
    for (int i = 0; i < m->vert_count; ++i)
        m->verts[i].normal = mc_v3_normalize(m->verts[i].normal);
}

/* Stamp src into dst through xform. Positions get the full affine transform,
 * and normals get the direction transform and a renormalize. That's exact for
 * rigid transforms and uniform scale. For nonuniform scaling, use
 * mc_mesh_scale first and then a rigid append. */
void mc_mesh_append(mc_mesh *dst, const mc_mesh *src, mc_m4 xform) {
    int base = dst->vert_count;
    for (int i = 0; i < src->vert_count; ++i) {
        mc_v3 p = mc_m4_transform_point(xform, src->verts[i].pos);
        mc_v3 n = mc_v3_normalize(mc_m4_transform_dir(xform, src->verts[i].normal));
        mc_mesh_push_vertex(dst, p, n);
        dst->verts[dst->vert_count - 1].uv = src->verts[i].uv;
    }
    for (int t = 0; t + 2 < src->index_count; t += 3)
        mc_mesh_push_tri(dst, (mc_u32)base + src->indices[t],
                              (mc_u32)base + src->indices[t + 1],
                              (mc_u32)base + src->indices[t + 2]);
}

/* Stitch a row-major (rows+1) x (cols+1) block of vertices starting at `base`
 * into two triangles per cell. Vertex (i,j) sits at base + i*(cols+1) + j. If
 * (i,j) -> (i,j+1) -> (i+1,j) runs CCW seen from outside, every cell comes
 * out front-facing. The tori, tubes, lofts and fins all use this. */
void mc_mesh_grid(mc_mesh *m, int base, int rows, int cols) {
    for (int i = 0; i < rows; ++i)
        for (int j = 0; j < cols; ++j) {
            mc_u32 a = (mc_u32)(base + i * (cols + 1) + j), c = a + (mc_u32)(cols + 1);
            mc_mesh_push_tri(m, a, a + 1, c);
            mc_mesh_push_tri(m, a + 1, c + 1, c);
        }
}

/* Tessellate face f (0..5 = +X -X +Y -Y +Z -Z) of a box with half extents he
 * into an nseg x nseg grid of triangles. The face starts at the -X-Y-Z corner
 * (moved to the + side along its own axis for a + face) and spans the next two
 * axes in cyclic order, swapped for a - face. That keeps cross(du, dv) along
 * the outward normal, so the face lights from the right side. */
static void add_box_face(mc_mesh *m, mc_v3 he, int f, int nseg) {
    if (nseg < 1) nseg = 1;
    const float h[3] = { he.x, he.y, he.z };
    int a = f >> 1, neg = f & 1, u = (a + 1 + neg) % 3, v = (a + 2 - neg) % 3;
    float o[3] = { -h[0], -h[1], -h[2] }, e[3] = {0}, g[3] = {0}, nn[3] = {0};
    if (!neg) o[a] = h[a];
    e[u] = 2.0f * h[u];
    g[v] = 2.0f * h[v];
    nn[a] = neg ? -1.0f : 1.0f;
    mc_v3 origin = mc_vec3(o[0], o[1], o[2]), du = mc_vec3(e[0], e[1], e[2]);
    mc_v3 dv = mc_vec3(g[0], g[1], g[2]), n = mc_vec3(nn[0], nn[1], nn[2]);
    int base = m->vert_count;
    int stride = nseg + 1;
    for (int j = 0; j <= nseg; ++j) {
        float tv = (float)j / (float)nseg;
        for (int i = 0; i <= nseg; ++i) {
            float tu = (float)i / (float)nseg;
            mc_v3 p = mc_v3_add(origin,
                                mc_v3_mad(mc_v3_scale(du, tu), dv, tv));
            mc_mesh_push_vertex(m, p, n);
        }
    }
    for (int j = 0; j < nseg; ++j)
        for (int i = 0; i < nseg; ++i) {
            mc_u32 a0 = (mc_u32)(base + j * stride + i);
            mc_u32 b0 = (mc_u32)(base + j * stride + i + 1);
            mc_u32 c0 = (mc_u32)(base + (j + 1) * stride + i + 1);
            mc_u32 d0 = (mc_u32)(base + (j + 1) * stride + i);
            mc_mesh_push_tri(m, a0, b0, c0);
            mc_mesh_push_tri(m, a0, c0, d0);
        }
}

/* A box as six grid faces with flat per-face normals. */
static void add_cube_faces(mc_mesh *m, mc_v3 he, int nseg) {
    for (int f = 0; f < 6; ++f) add_box_face(m, he, f, nseg);
}

/* primitives */
mc_mesh mc_mesh_box(mc_v3 half_extents) {
    mc_mesh m = mc_mesh_new();
    add_cube_faces(&m, half_extents, 1);
    return m;
}

mc_mesh mc_mesh_plane(float size, int subdivisions) {
    if (subdivisions < 1) subdivisions = 1;
    mc_mesh m = mc_mesh_new();
    float h = size * 0.5f;
    /* XZ plane, +Y normal: the top face of a box with no height. */
    add_box_face(&m, mc_vec3(h, 0.0f, h), 2, subdivisions);
    return m;
}

mc_mesh mc_mesh_uv_sphere(float radius, int slices, int stacks) {
    if (slices < 3) slices = 3;
    if (stacks < 2) stacks = 2;
    mc_mesh m = mc_mesh_new();
    for (int i = 0; i <= stacks; ++i) {
        float phi = MC_PI * (float)i / (float)stacks;   /* 0 = +Y pole */
        float sp = sinf(phi), cp = cosf(phi);
        for (int j = 0; j <= slices; ++j) {
            float th = MC_TAU * (float)j / (float)slices;
            mc_v3 n = mc_vec3(sp * cosf(th), cp, sp * sinf(th)); /* already unit */
            mc_mesh_push_vertex(&m, mc_v3_scale(n, radius), n);
        }
    }
    int stride = slices + 1;
    for (int i = 0; i < stacks; ++i)
        for (int j = 0; j < slices; ++j) {
            mc_u32 v00 = (mc_u32)(i * stride + j);
            mc_u32 v01 = (mc_u32)(i * stride + j + 1);
            mc_u32 v10 = (mc_u32)((i + 1) * stride + j);
            mc_u32 v11 = (mc_u32)((i + 1) * stride + j + 1);
            if (i != 0)          mc_mesh_push_tri(&m, v00, v01, v10); /* skip N pole fan */
            if (i != stacks - 1) mc_mesh_push_tri(&m, v01, v11, v10); /* skip S pole fan */
        }
    return m;
}

/* Icosphere: start from the 12-vertex icosahedron, subdivide, then push every
 * vertex onto the sphere. The triangles come out much more even than a UV
 * sphere's, with no pinch at the poles, which is why it's the default ball
 * around here. */
mc_mesh mc_mesh_icosphere(float radius, int subdivisions) {
    if (subdivisions < 0) subdivisions = 0;
    mc_mesh m = mc_mesh_new();
    const float t = (1.0f + sqrtf(5.0f)) * 0.5f;        /* golden ratio */
    const mc_v3 base[12] = {
        {-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0},
        {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t},
        {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1},
    };
    static const int faces[20][3] = {
        {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11},
        {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
        {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9},
        {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1},
    };
    for (int i = 0; i < 12; ++i) {
        mc_v3 n = mc_v3_normalize(base[i]);
        mc_mesh_push_vertex(&m, n, n);                  /* unit sphere, R applied later */
    }
    for (int f = 0; f < 20; ++f)
        mc_mesh_push_tri(&m, (mc_u32)faces[f][0], (mc_u32)faces[f][1], (mc_u32)faces[f][2]);
    mc_mesh_subdivide(&m, subdivisions);
    /* Project every vertex onto the sphere; normal is the normalized position. */
    for (int i = 0; i < m.vert_count; ++i) {
        mc_v3 n = mc_v3_normalize(m.verts[i].pos);
        m.verts[i].pos = mc_v3_scale(n, radius);
        m.verts[i].normal = n;
    }
    return m;
}

mc_mesh mc_mesh_torus(float major_r, float minor_r, int major_seg, int minor_seg) {
    if (major_seg < 3) major_seg = 3;
    if (minor_seg < 3) minor_seg = 3;
    mc_mesh m = mc_mesh_new();
    for (int i = 0; i <= major_seg; ++i) {
        float th = MC_TAU * (float)i / (float)major_seg;
        float ct = cosf(th), st = sinf(th);
        for (int j = 0; j <= minor_seg; ++j) {
            float ph = MC_TAU * (float)j / (float)minor_seg;
            float cph = cosf(ph), sph = sinf(ph);
            mc_v3 n = mc_vec3(cph * ct, sph, cph * st);  /* already unit */
            mc_v3 p = mc_vec3((major_r + minor_r * cph) * ct,
                              minor_r * sph,
                              (major_r + minor_r * cph) * st);
            mc_mesh_push_vertex(&m, p, n);
        }
    }
    mc_mesh_grid(&m, 0, major_seg, minor_seg);
    return m;
}

/* A flat disc at height y facing ny (+1 up, -1 down), built as a fan from a
 * centre vertex to a ring of segments+1 rim vertices. Cylinders and cones use
 * it for their caps. */
static void add_disc(mc_mesh *m, float radius, float y, float ny, int segments) {
    mc_u32 c = (mc_u32)mc_mesh_push_vertex(m, mc_vec3(0.0f, y, 0.0f), mc_vec3(0.0f, ny, 0.0f));
    for (int j = 0; j <= segments; ++j) {
        float th = MC_TAU * (float)j / (float)segments;
        mc_mesh_push_vertex(m, mc_vec3(radius * cosf(th), y, radius * sinf(th)), mc_vec3(0.0f, ny, 0.0f));
    }
    for (mc_u32 j = 0; j < (mc_u32)segments; ++j) {
        if (ny > 0.0f) mc_mesh_push_tri(m, c, c + j + 2, c + j + 1);
        else           mc_mesh_push_tri(m, c, c + j + 1, c + j + 2);
    }
}

mc_mesh mc_mesh_cylinder(float radius, float height, int segments, int caps) {
    if (segments < 3) segments = 3;
    mc_mesh m = mc_mesh_new();
    float hy = height * 0.5f;
    /* Side wall: paired bottom/top ring vertices with radial normals. */
    for (int j = 0; j <= segments; ++j) {
        float th = MC_TAU * (float)j / (float)segments;
        float ct = cosf(th), st = sinf(th);
        mc_v3 n = mc_vec3(ct, 0.0f, st);
        mc_mesh_push_vertex(&m, mc_vec3(radius * ct, -hy, radius * st), n);
        mc_mesh_push_vertex(&m, mc_vec3(radius * ct,  hy, radius * st), n);
    }
    mc_mesh_grid(&m, 0, segments, 1);
    if (caps) {
        add_disc(&m, radius,  hy,  1.0f, segments);
        add_disc(&m, radius, -hy, -1.0f, segments);
    }
    return m;
}

mc_mesh mc_mesh_cone(float radius, float height, int segments) {
    if (segments < 3) segments = 3;
    mc_mesh m = mc_mesh_new();
    float hy = height * 0.5f;
    /* Side-face normal tilts up by radius/height; apex gets the per-face mean. */
    float slope = (height != 0.0f) ? radius / height : 0.0f;
    /* One triangle per segment: rim, apex (at the mid angle), next rim. */
    for (int j = 0; j < segments; ++j) {
        mc_u32 b = (mc_u32)m.vert_count;
        for (int k = 0; k < 3; ++k) {
            float th = MC_TAU * ((float)j + 0.5f * (float)k) / (float)segments;
            float ct = cosf(th), st = sinf(th);
            mc_mesh_push_vertex(&m, k == 1 ? mc_vec3(0.0f, hy, 0.0f) : mc_vec3(radius * ct, -hy, radius * st),
                                mc_v3_normalize(mc_vec3(ct, slope, st)));
        }
        mc_mesh_push_tri(&m, b, b + 1, b + 2);
    }
    add_disc(&m, radius, -hy, -1.0f, segments);     /* base cap (-Y) */
    return m;
}

/* A rounded box, as the Minkowski sum of an inner box core and a sphere of
 * radius r = round * min(half extents). Build a subdivided cube, then push each
 * surface point out to distance r from the nearest point on the core. Flat
 * faces stay flat, with the axis as their normal, and edges and corners round
 * off with the outward direction as the normal. round == 1 shrinks the core
 * to a point, giving a sphere of radius min(he). */
mc_mesh mc_mesh_rounded_box(mc_v3 half_extents, float round, int subdivisions) {
    if (subdivisions < 1) subdivisions = 1;
    round = mc_saturatef(round);
    mc_mesh m = mc_mesh_new();
    add_cube_faces(&m, half_extents, subdivisions);
    float r = round * mc_minf(half_extents.x, mc_minf(half_extents.y, half_extents.z));
    if (r > 1e-6f) {
        mc_v3 core = mc_v3_max(mc_v3_sub(half_extents, mc_v3_splat(r)), mc_v3_splat(0.0f));
        for (int i = 0; i < m.vert_count; ++i) {
            mc_v3 p = m.verts[i].pos;
            mc_v3 inner = mc_vec3(mc_clampf(p.x, -core.x, core.x),
                                  mc_clampf(p.y, -core.y, core.y),
                                  mc_clampf(p.z, -core.z, core.z));
            mc_v3 d = mc_v3_sub(p, inner);
            float dl = mc_v3_len(d);
            mc_v3 n = (dl > 1e-6f) ? mc_v3_scale(d, 1.0f / dl) : m.verts[i].normal;
            m.verts[i].pos = mc_v3_mad(inner, n, r);
            m.verts[i].normal = n;
        }
    }
    return m;
}

/* A fractal landscape. Start from a flat grid, raise each vertex by fBm of its
 * XZ position, shaped by a smoothstep so the valleys come out flat and the
 * ridges stay sharp, then recompute the normals. Scene 2's seabed is this
 * heightfield: uwplace.comp computes the same thing on the GPU, and the
 * checks compare the two. */
mc_mesh mc_mesh_terrain(float size, int subdivisions, float amp, mc_u32 seed) {
    mc_mesh m = mc_mesh_plane(size, subdivisions);
    mc_rng r = mc_rng_seed(seed);
    /* Draw the two offsets in a fixed order. C leaves argument evaluation order
     * unspecified, so mc_vec2(rng(), rng()) could swap them. Callers that
     * replay this height field (to seat props on the seabed, say) must match
     * it. */
    float ofx = mc_rng_range(&r, -64.0f, 64.0f);
    float ofz = mc_rng_range(&r, -64.0f, 64.0f);
    mc_v2 off = mc_vec2(ofx, ofz);
    float freq = 3.0f / (size > 1e-3f ? size : 1.0f);
    for (int i = 0; i < m.vert_count; ++i) {
        mc_v3 p = m.verts[i].pos;
        float h = mc_fbm2(p.x * freq + off.x, p.z * freq + off.y, 6, 2.0f, 0.5f);
        h = mc_saturatef(h * 0.5f + 0.5f);
        h = h * h * (3.0f - 2.0f * h);              /* flatten the lows, keep the highs */
        m.verts[i].pos.y += amp * (h - 0.35f);
    }
    mc_mesh_recompute_normals(&m);
    return m;
}

/* A point on the (p,q) torus knot at parameter u. */
static mc_v3 knot_point(float u, int p, int q, float s) {
    float pu = (float)p * u, qu = (float)q * u;
    float rr = 2.0f + cosf(qu);
    return mc_vec3(s * rr * cosf(pu) * 0.5f, s * sinf(qu) * 0.5f, s * rr * sinf(pu) * 0.5f);
}

/* Sweep a circular tube around the knot curve. The cross-section normal is the
 * outward radial direction, which is exact for a round tube. That lets us set
 * the normals at build time and skip the recompute, which would depend on
 * winding. */
mc_mesh mc_mesh_torus_knot(float scale, float tube, int p, int q, int cseg, int sides) {
    if (cseg < 8) cseg = 8;
    if (sides < 3) sides = 3;
    mc_mesh m = mc_mesh_new();
    for (int i = 0; i <= cseg; ++i) {
        float u = MC_TAU * (float)i / (float)cseg;
        mc_v3 c  = knot_point(u, p, q, scale);
        mc_v3 c2 = knot_point(u + 0.01f, p, q, scale);
        mc_v3 T  = mc_v3_dir(c2, c);
        mc_v3 N  = mc_v3_cross(T, mc_vec3(0.0f, 1.0f, 0.0f));
        if (mc_v3_len2(N) < 1e-4f) N = mc_vec3(1.0f, 0.0f, 0.0f);
        N = mc_v3_normalize(N);
        mc_v3 B = mc_v3_normalize(mc_v3_cross(T, N));
        for (int j = 0; j <= sides; ++j) {
            float v = MC_TAU * (float)j / (float)sides;
            mc_v3 dir = mc_v3_mad(mc_v3_scale(N, cosf(v)), B, sinf(v));
            mc_mesh_push_vertex(&m, mc_v3_mad(c, dir, tube), dir);
        }
    }
    int stride = sides + 1;
    for (int i = 0; i < cseg; ++i)
        for (int j = 0; j < sides; ++j) {
            mc_u32 a = (mc_u32)(i * stride + j),       b = (mc_u32)(i * stride + j + 1);
            mc_u32 c = (mc_u32)((i + 1) * stride + j), d = (mc_u32)((i + 1) * stride + j + 1);
            mc_mesh_push_tri(&m, a, b, d);
            mc_mesh_push_tri(&m, a, d, c);
        }
    return m;
}

/* modifiers */

/* When we subdivide, each edge gets split once, and both triangles sharing it
 * must reuse the same midpoint vertex, or the mesh cracks apart along every
 * edge. This hash map (undirected edge -> midpoint index) keeps it welded.
 * Open addressing, power-of-two size, linear probing. */
#define MP_EMPTY (~(mc_u64)0)
typedef struct {
    mc_u64 *keys;
    mc_u32 *vals;
    int     cap;        /* power of two */
} mp_cache;

static int next_pow2(int x) {
    int p = 1;
    while (p < x) p <<= 1;
    return p;
}
static mc_u32 edge_midpoint(mp_cache *c, mc_mesh *m, mc_u32 a, mc_u32 b) {
    mc_u32 lo = a < b ? a : b, hi = a < b ? b : a;
    mc_u64 key = ((mc_u64)lo << 32) | (mc_u64)hi;
    mc_u32 mask = (mc_u32)c->cap - 1u;
    mc_u32 h = (mc_hash_u32((mc_u32)key) ^ mc_hash_u32((mc_u32)(key >> 32))) & mask;
    for (;;) {
        if (c->keys[h] == MP_EMPTY) {
            /* Create the midpoint: average position, normalized average normal. */
            mc_v3 pos = mc_v3_scale(mc_v3_add(m->verts[a].pos, m->verts[b].pos), 0.5f);
            mc_v3 nrm = mc_v3_normalize(mc_v3_add(m->verts[a].normal, m->verts[b].normal));
            mc_v2 uv = mc_vec2((m->verts[a].uv.x + m->verts[b].uv.x) * 0.5f,
                               (m->verts[a].uv.y + m->verts[b].uv.y) * 0.5f);
            mc_u32 idx = (mc_u32)mc_mesh_push_vertex(m, pos, nrm);
            m->verts[idx].uv = uv;
            c->keys[h] = key;
            c->vals[h] = idx;
            return idx;
        }
        if (c->keys[h] == key) return c->vals[h];
        h = (h + 1u) & mask;
    }
}

void mc_mesh_subdivide(mc_mesh *m, int iterations) {
    for (int it = 0; it < iterations; ++it) {
        int old_ic = m->index_count;
        if (old_ic < 3) break;
        int ntris = old_ic / 3;

        /* Snapshot the current index list, since we rebuild it in place. */
        mc_u32 *old = malloc((size_t)old_ic * sizeof *old);
        if (!old) mc_fatal("mesh: out of memory (subdivide)");
        memcpy(old, m->indices, (size_t)old_ic * sizeof *old);
        m->index_count = 0;

        mp_cache c;
        c.cap = next_pow2(2 * 3 * ntris + 16);          /* <= 0.5 load factor */
        c.keys = malloc((size_t)c.cap * sizeof *c.keys);
        c.vals = malloc((size_t)c.cap * sizeof *c.vals);
        if (!c.keys || !c.vals) mc_fatal("mesh: out of memory (subdivide cache)");
        for (int i = 0; i < c.cap; ++i) c.keys[i] = MP_EMPTY;

        for (int t = 0; t < ntris; ++t) {
            mc_u32 i0 = old[t * 3], i1 = old[t * 3 + 1], i2 = old[t * 3 + 2];
            mc_u32 m0 = edge_midpoint(&c, m, i0, i1);
            mc_u32 m1 = edge_midpoint(&c, m, i1, i2);
            mc_u32 m2 = edge_midpoint(&c, m, i2, i0);
            mc_mesh_push_tri(m, i0, m0, m2);
            mc_mesh_push_tri(m, m0, i1, m1);
            mc_mesh_push_tri(m, m2, m1, i2);
            mc_mesh_push_tri(m, m0, m1, m2);
        }
        free(c.keys);
        free(c.vals);
        free(old);
    }
}

void mc_mesh_displace_noise(mc_mesh *m, float amount, float frequency, mc_u32 seed) {
    /* A seed-derived offset decorrelates the noise field between calls. */
    mc_rng r = mc_rng_seed(seed);
    mc_v3 off = mc_vec3(mc_rng_range(&r, -128.0f, 128.0f),
                        mc_rng_range(&r, -128.0f, 128.0f),
                        mc_rng_range(&r, -128.0f, 128.0f));
    for (int i = 0; i < m->vert_count; ++i) {
        mc_v3 sp = mc_v3_mad(off, m->verts[i].pos, frequency);
        float d = amount * mc_fbm3(sp, 5, 2.0f, 0.5f);
        m->verts[i].pos = mc_v3_mad(m->verts[i].pos, m->verts[i].normal, d);
    }
    mc_mesh_recompute_normals(m);               /* keep lighting correct */
}

void mc_mesh_spherify(mc_mesh *m, float radius, float t) {
    for (int i = 0; i < m->vert_count; ++i) {
        mc_v3 p = m->verts[i].pos;
        mc_v3 s = mc_v3_scale(mc_v3_normalize(p), radius);
        m->verts[i].pos = mc_v3_lerp(p, s, t);
    }
    mc_mesh_recompute_normals(m);               /* shape changed: renormalize */
}

void mc_mesh_scale(mc_mesh *m, mc_v3 s) {
    /* Positions scale directly. Normals transform by the inverse-transpose of
     * the diagonal scale, which comes down to normalize(normal / s). That's
     * exact even for non-uniform scale, and it keeps hard edges because
     * nothing gets re-averaged. */
    mc_v3 inv = mc_vec3(s.x != 0.0f ? 1.0f / s.x : 0.0f,
                        s.y != 0.0f ? 1.0f / s.y : 0.0f,
                        s.z != 0.0f ? 1.0f / s.z : 0.0f);
    for (int i = 0; i < m->vert_count; ++i) {
        m->verts[i].pos = mc_v3_mul(m->verts[i].pos, s);
        m->verts[i].normal = mc_v3_normalize(mc_v3_mul(m->verts[i].normal, inv));
    }
}
