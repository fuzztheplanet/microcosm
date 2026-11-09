/*
 * mc_mesh.h - build geometry on the CPU, procedurally.
 *
 * A mesh is an indexed triangle list with interleaved vertices (position +
 * normal + UV). The usual flow is to build the shapes you need once at
 * startup, from the primitive constructors and optionally the modifiers, and
 * then upload them to the GPU with mc_mesh_upload (in mc_render.h).
 *
 * A mesh owns its two arrays and grows them as needed, so remember to
 * mc_mesh_free it. Every constructor returns a complete mesh that the caller
 * owns.
 *
 * A shape whose vertex shader makes its own vertices from gl_VertexID needs
 * none of this. mc_mesh_procedural, in mc_render.h, gives it a GPU mesh with
 * no vertex data at all.
 */
#ifndef MICROCOSM_MC_MESH_H
#define MICROCOSM_MC_MESH_H

#include "mc.h"
#include "mc_math.h"

typedef struct {
    mc_v3 pos;
    mc_v3 normal;
    mc_v2 uv;        /* texture coords; 0 unless a builder sets them (e.g. unwrapped fish/whale) */
} mc_vertex;

typedef struct {
    mc_vertex *verts;
    int        vert_count;
    int        vert_cap;
    mc_u32    *indices;     /* triangle list, 3 per face */
    int        index_count;
    int        index_cap;
} mc_mesh;

/* the low-level building blocks; most callers use the primitives below */
mc_mesh mc_mesh_new(void);
void    mc_mesh_free(mc_mesh *m);
/* Append one vertex / one triangle; arrays grow as needed. Returns index. */
int     mc_mesh_push_vertex(mc_mesh *m, mc_v3 pos, mc_v3 normal);
void    mc_mesh_push_tri(mc_mesh *m, mc_u32 a, mc_u32 b, mc_u32 c);
/* Recompute per-vertex normals by area-weighted face normals (smooth). */
void    mc_mesh_recompute_normals(mc_mesh *m);
/* Append src through xform, keeping its UVs. src must be a different mesh from
 * dst. xform must be rigid or a positive uniform scale for correct normals. For
 * nonuniform scale, first use mc_mesh_scale on the source, then append through
 * a rigid transform. */
void    mc_mesh_append(mc_mesh *dst, const mc_mesh *src, mc_m4 xform);
/* Triangulate a row-major (rows+1) x (cols+1) block of vertices starting at
 * `base`, where vertex (i,j) is base + i*(cols+1) + j. Each cell becomes two
 * triangles, wound CCW as long as (i,j) -> (i,j+1) -> (i+1,j) runs CCW seen
 * from outside. Every lofted or swept surface (tori, tubes, fins) is stitched
 * with this. */
void    mc_mesh_grid(mc_mesh *m, int base, int rows, int cols);

/* primitives, all centred on the origin unless noted */
mc_mesh mc_mesh_box(mc_v3 half_extents);
mc_mesh mc_mesh_plane(float size, int subdivisions);      /* XZ plane, +Y normal */
mc_mesh mc_mesh_uv_sphere(float radius, int slices, int stacks);
mc_mesh mc_mesh_icosphere(float radius, int subdivisions);
mc_mesh mc_mesh_torus(float major_r, float minor_r, int major_seg, int minor_seg);
mc_mesh mc_mesh_cylinder(float radius, float height, int segments, int caps);
mc_mesh mc_mesh_cone(float radius, float height, int segments);
/* A box with rounded edges and corners. `round` in [0,1] sets the rounding
 * radius as a fraction of the smallest half extent. */
mc_mesh mc_mesh_rounded_box(mc_v3 half_extents, float round, int subdivisions);
/* A fractal landscape: an XZ plane subdivided `subdivisions` times and raised
 * by fBm of amplitude `amp`, with the normals recomputed. It's textured by
 * triplanar projection, so it needs no UVs. */
mc_mesh mc_mesh_terrain(float size, int subdivisions, float amp, mc_u32 seed);
/* A (p,q) torus knot swept as a tube of radius `tube`. `curve_seg` is the
 * number of samples along the knot, and `tube_sides` the number around each
 * cross-section. */
mc_mesh mc_mesh_torus_knot(float scale, float tube, int p, int q,
                           int curve_seg, int tube_sides);

/* modifiers: mutate a mesh in place */
/* Split every triangle into 4, with no smoothing. UVs are interpolated
 * linearly. UV seams need distinct vertex indices, as in any indexed mesh. */
void mc_mesh_subdivide(mc_mesh *m, int iterations);
/* Push vertices along their normals by noise scaled by `amount`, for an
 * organic look. */
void mc_mesh_displace_noise(mc_mesh *m, float amount, float frequency, mc_u32 seed);
/* Spherify: blend each vertex toward a sphere of `radius` by factor t[0..1]. */
void mc_mesh_spherify(mc_mesh *m, float radius, float t);
/* Scale all positions. */
void mc_mesh_scale(mc_mesh *m, mc_v3 s);

#endif /* MICROCOSM_MC_MESH_H */
