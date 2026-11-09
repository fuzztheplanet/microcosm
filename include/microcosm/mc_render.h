/*
 * mc_render.h - the GPU side of the engine: shaders, meshes, render targets
 * and the post-process stack.
 *
 * A frame looks like this:
 *   mc_renderer_begin(R, clear);              // bind the HDR scene target, clear
 *   ... your shaders, uniforms and draws ...  // any OpenGL you like
 *   mc_renderer_end(R, output_fbo, ...);      // bloom + tonemap + grade -> out
 *
 * begin/end hide the HDR target and the post chain, and everything in between
 * is ordinary OpenGL. The lower-level pieces (shader compile, mesh upload,
 * targets, the fullscreen triangle) are public too, and the demo uses them
 * directly.
 */
#ifndef MICROCOSM_MC_RENDER_H
#define MICROCOSM_MC_RENDER_H

#include "mc.h"
#include "mc_gl.h"
#include "mc_math.h"
#include "mc_mesh.h"

/* shaders: compile a program, then set uniforms by cached location */
typedef struct { GLuint prog; } mc_shader;

mc_shader mc_shader_make(const char *vs_src, const char *fs_src);
void      mc_shader_free(mc_shader *s);
void      mc_shader_use(mc_shader s);
/* Uniform setters take a location: a layout(location = N) from the shader, or
 * one looked up once with mc_uniform. */
void mc_set_i (int loc, int v);
void mc_set_v2(int loc, mc_v2 v);
void mc_set_v4(int loc, mc_v4 v);
void mc_set_m4(int loc, mc_m4 v);
#ifndef MC_IMPORT_HASH   /* not in the payload, which resolves no GL call for them (mc_gl.h) */
int       mc_uniform(mc_shader s, const char *name);   /* -1 if absent */
void mc_set_f (int loc, float v);
void mc_set_v3(int loc, mc_v3 v);
#endif

/* meshes on the GPU, with an instanced draw path */
typedef struct {
    GLuint vao, vbo, ibo;   /* vbo 0: no vertex data (procedural); ibo 0: not indexed */
    GLuint inst_vbo;        /* per-instance buffer, 0 if unused */
    int    inst_cap;        /* instances the buffer is sized for */
    int    index_count;
    int    vertex_count;
    GLenum index_type;      /* upload uses uint16 if the indices fit, else uint32 */
    float  radius;          /* bounding radius: measured on upload, given for procedural */
} mc_gpu_mesh;

mc_gpu_mesh mc_mesh_upload(const mc_mesh *m);
/* A mesh whose vertex shader builds its vertices from gl_VertexID. There's no
 * vertex buffer, only a VAO, so instancing still works. With `indices` it
 * draws `count` vertex ids, three per triangle, uploaded as uint32; a vertex
 * shared by several triangles is then shaded only once. With NULL indices it
 * draws `count` vertices as a plain triangle list. `radius` is the distance
 * from the origin that bounds the mesh, used for culling. */
mc_gpu_mesh mc_mesh_procedural(int count, const mc_u32 *indices, float radius);
#ifndef MC_IMPORT_HASH   /* not in the payload, which just exits: call it inside MC_TIDY() */
void        mc_mesh_gpu_free(mc_gpu_mesh *g);
#endif
void        mc_mesh_draw(mc_gpu_mesh g);

/* Per-instance attribute layout used by the demo's instanced draw path.
 * Attributes 2..6 are fed from a tightly packed array of mc_instance. */
typedef struct {
    mc_m4 model;            /* attribs 2,3,4,5 (one vec4 per column) */
    mc_v4 color;            /* attrib 6 (rgb + emissive in .a)       */
} mc_instance;

/* Pre-size the instance buffer for up to `capacity` instances. It's optional
 * and avoids reallocating at runtime. Call it once after upload. */
void mc_mesh_reserve_instances(mc_gpu_mesh *g, int capacity);
/* (Re)upload instance data and set up the divisor attributes on the VAO.
 * The buffer only ever grows, and updates go through glBufferSubData. */
void mc_mesh_set_instances(mc_gpu_mesh *g, const mc_instance *data, int count);
void mc_mesh_draw_instanced(mc_gpu_mesh g, int count);
/* Draw instances first..first+count-1 from the instance VBO already uploaded. */
void mc_mesh_draw_instance_range(mc_gpu_mesh g, int first, int count);

/* offscreen render targets (FBOs) */
typedef struct {
    GLuint fbo, color, depth;
    int    w, h;
    int    hdr;             /* 1 for RGBA16F; zero for the other formats */
} mc_target;

mc_target mc_target_make(int w, int h, int hdr, int with_depth);
/* Color format: GL_RGBA8, GL_RGBA16F or GL_R8. Zero makes a depth-only FBO. */
mc_target mc_target_make_format(int w, int h, GLenum color_format, int with_depth);
int       mc_target_ok(const mc_target *t);       /* 1 if usable, 0 if creation failed */
void      mc_target_free(mc_target *t);
void      mc_target_bind(const mc_target *t);     /* NULL binds framebuffer 0 */
void      mc_target_bind_fbo(GLuint fbo, int w, int h);

/* Draw a fullscreen triangle with the current shader. There's no VBO: the
 * vertex shader builds the triangle from gl_VertexID. */
void mc_fullscreen_triangle(void);

/* Four GL switches in one value: the depth test, depth writes, face culling
 * and blending, in that order, two bits each. A pair of bits leaves its switch
 * alone (0), turns it on (1) or turns it off (2). For example,
 * mc_gl_switch(MC_ZTEST_OFF | MC_ZWRITE_OFF) does the same as
 * glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE). */
enum { MC_ZTEST_ON = 1, MC_ZTEST_OFF = 2, MC_ZWRITE_ON = 4, MC_ZWRITE_OFF = 8,
       MC_CULL_ON = 16, MC_CULL_OFF = 32, MC_BLEND_ON = 64, MC_BLEND_OFF = 128 };
void mc_gl_switch(int s);

/* the post stack: HDR scene in, graded image out */
/* Defaults (in parentheses) are what mc_post_defaults() returns. */
typedef struct {
    float exposure;    /* linear exposure before tonemap      (1.1)   */
    float bloom;       /* bloom mix amount                    (0.08)  */
    float bloom_thresh;/* luminance threshold for bright pass (1.0)   */
    float vignette;    /* 0..1                                (0.32)  */
    float grain;       /* film grain amount                   (0.012) */
    float chroma;      /* chromatic aberration                (0.35)  */
    float saturation;  /* colour saturation multiplier        (1.08)  */
    float fade;        /* 0 = black, 1 = full image (for fades) (1.0)  */
    float time;        /* seconds, animates grain             (0.0)   */
    /* Ambient occlusion and the white fade (default 0: off), the reach of
     * the motion blur (which can't be switched off), and the camera terms
     * used to rebuild depth and view position. The defaults for those don't
     * even match mc_camera_default: copy them from your camera every frame,
     * with projy = tanf(fovy / 2) and projx = projy * mc_renderer_aspect(). */
    float ssao;        /* ambient occlusion (GTAO) strength       (0)  */
    float blur;        /* max camera motion blur each way, as a fraction
                          of the screen (0 or less means 0.25%, which is
                          also the least there is)                 (0)  */
    float znear, zfar; /* scene camera near/far, for linear depth (0.1,400) */
    float projx, projy;/* tan(fov/2) in x,y, to rebuild view pos  (1,1)  */
    float white;       /* 0 = the image, 1 = pure white (for fades)  (0)   */
    /* Depth of field and added light (all default 0 / off). */
    float dof;         /* blur radius at infinity, full-res pixels (0)  */
    float focus;       /* focus distance, scene units               (0)  */
    GLuint vol_tex;    /* low-res HDR light to add, depth-aware (e.g.
                          volumetric shafts; blur it with mc_renderer_blur) */
    float vol;         /* how much of vol_tex to add                 (0)  */
    mc_v3 sun_dir;     /* direction to the sun (world space), for contact
                          shadows on the sun's share of each pixel's
                          light (zero vector: off)                      */
} mc_post;

mc_post mc_post_defaults(void);

typedef struct mc_renderer mc_renderer;
mc_renderer *mc_renderer_make(int w, int h);
void         mc_renderer_free(mc_renderer *r);
/* Returns 0 on failure; the previous target set stays usable. */
int          mc_renderer_resize(mc_renderer *r, int w, int h);
/* Set a fixed internal render scale of 25..200 percent. Above 100, the TAA
 * resolves the supersampled frame at its own size and the present pass
 * filters it down. On failure it returns 0 and nothing changes. The
 * presentation aspect is kept exact even when the scaled size rounds to odd
 * pixel counts. */
int          mc_renderer_set_scale(mc_renderer *r, int percent);
void         mc_renderer_size(const mc_renderer *r, int *w, int *h);

/* TAA. Call mc_renderer_frame once at the start of each frame with the
 * frame's time. The history carries on while time moves smoothly forward,
 * and starts over after a jump, a pause or a step back. Then build the
 * camera's view-projection with mc_renderer_camera, which offsets it by the
 * frame's sub-pixel jitter. The first camera built in a frame is the one the
 * history follows. */
void  mc_renderer_frame(mc_renderer *r, double t);
mc_m4 mc_renderer_camera(mc_renderer *r, mc_m4 proj, mc_m4 view);

/* Bind the HDR scene target and clear it to `clear` (linear RGB).
 * begin and end set the framebuffer, viewport, program, VAO, texture
 * bindings, blend, depth and cull state as they need, and don't restore what
 * the caller had. Callers must leave the scissor and stencil tests and
 * polygon offset disabled, all color channels writable, and the polygon mode
 * on fill. A context belongs to one thread; close it before opening another. */
void mc_renderer_begin(mc_renderer *r, mc_v3 clear);
/* Run the HDR scene through the post stack into `output_fbo` (0 = screen),
 * centred in its out_w x out_h. If that framebuffer is bigger than the
 * renderer's output size, the picture sits in the middle with opaque black
 * around it. It uploads its own data to the per-frame uniform block on
 * binding 0 (mc_gl_frame_block), so re-upload yours each frame before your
 * draws. */
void mc_renderer_end(mc_renderer *r, GLuint output_fbo, int out_w, int out_h,
                     mc_post params);
/* Presentation aspect ratio, unaffected by rounding the scaled scene size. */
float mc_renderer_aspect(const mc_renderer *r);
/* Re-bind the HDR scene target mid-frame (e.g. after a shadow-map pass drawn
 * into another FBO between begin() and end()). */
void  mc_renderer_bind_scene(mc_renderer *r);
/* The scene's depth texture, for the caller's own passes into other targets.
 * Both of these return a different texture every frame: end() swaps the
 * scene target with the last frame's, and a resize or set_scale replaces
 * them. Query them each frame between begin() and end(), and don't cache
 * them. */
GLuint mc_renderer_depth(const mc_renderer *r);
/* ...and its HDR colour, for passes into other targets that read the picture so far. */
GLuint mc_renderer_color(const mc_renderer *r);
/* Depth-aware separable blur of a reduced-resolution buffer, a -> b -> a.
 * a and b must be the same size. zn/zf are the near and far planes, used to
 * linearise the scene depth that weights the blur. */
void  mc_renderer_blur(mc_renderer *r, mc_target *a, mc_target *b, float zn, float zf);

#endif /* MICROCOSM_MC_RENDER_H */
