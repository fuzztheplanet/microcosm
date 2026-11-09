/*
 * render.c - the GL-facing half of the engine: shader and program helpers,
 * mesh and instance buffers, render targets, and the post chain that turns a
 * raw HDR scene into the final graded frame.
 *
 * The renderer keeps these targets:
 *   - an RGBA16F scene target with a sampleable depth texture, at the internal
 *     scale (80% by default in the demo)
 *   - a six-level bloom chain, from half resolution down
 *   - half-res AO / contact-shadow buffers
 *   - a quarter-res depth-of-field pair
 *   - an LDR target for the composite
 *   - the TAA's two history targets, at output size
 *
 * You draw your scene into the HDR target between begin() and end(), using
 * the camera from mc_renderer_camera, which is jittered for the TAA. end()
 * then runs:
 *   GTAO + contact shadows (optional) -> depth of field (optional) ->
 *   bright pass -> bloom chain -> motion blur, tonemap and grade (with the AO
 *   and any caller light folded in) -> TAA
 * The TAA upsamples to the output size, and the present pass writes the
 * result into whatever framebuffer you pass (0 = the screen). The optional
 * passes are switched on by their strengths in mc_post, so they cost nothing
 * when off, and the demo ramps them in over Scenes 2 and 3.
 *
 * The only shaders here are the engine's post passes in src/shaders. Scene
 * shaders are the caller's business. GL 4.5 core throughout.
 */
#include "microcosm/mc_render.h"
#include "microcosm/mc_profile.h"
#include "microcosm/mc.h"
#include <stdlib.h>
#include <string.h>
#include "microcosm/mc_imports.h"   /* payload calls by hash; keep after system headers */

/* ---- shaders and programs ---- */

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
#if defined(MC_DEV)
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        mc_log("shader compile error:\n%s", log);
#endif
        MC_TIDY(glDeleteShader(s));
        return 0;
    }
    return s;
}

mc_shader mc_shader_make(const char *vs_src, const char *fs_src) {
    mc_shader sh = {0};
    /* the same source for both stages means a compute program with just that shader */
    int cs = vs_src == fs_src;
    GLuint vs = compile(cs ? GL_COMPUTE_SHADER : GL_VERTEX_SHADER, vs_src);
    GLuint fs = cs ? vs : compile(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) { MC_TIDY(if (vs) glDeleteShader(vs); if (fs && !cs) glDeleteShader(fs)); return sh; }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    if (!cs) glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
#if defined(MC_DEV)
        char log[1024];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        mc_log("program link error:\n%s", log);
#endif
        MC_TIDY(glDeleteProgram(p));
        p = 0;
    }
    /* the shaders now go when the program does (the payload frees neither) */
    MC_TIDY(glDeleteShader(vs); if (!cs) glDeleteShader(fs));
    sh.prog = p;
    return sh;
}

void mc_shader_free(mc_shader *s) { if (s && s->prog) { glDeleteProgram(s->prog); s->prog = 0; } }
void mc_shader_use(mc_shader s)   { glUseProgram(s.prog); }

void mc_set_i (int loc, int v)     { if (loc >= 0) glUniform1i(loc, v); }
void mc_set_v2(int loc, mc_v2 v)   { if (loc >= 0) glUniform2f(loc, v.x, v.y); }
void mc_set_v4(int loc, mc_v4 v)   { if (loc >= 0) glUniform4f(loc, v.x, v.y, v.z, v.w); }
void mc_set_m4(int loc, mc_m4 v)   { if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, v.m); }
#ifndef MC_IMPORT_HASH   /* the payload looks nothing up by name and sets no lone float or vec3 */
int  mc_uniform(mc_shader s, const char *name) { return glGetUniformLocation(s.prog, name); }
void mc_set_f (int loc, float v)   { if (loc >= 0) glUniform1f(loc, v); }
void mc_set_v3(int loc, mc_v3 v)   { if (loc >= 0) glUniform3f(loc, v.x, v.y, v.z); }
#endif

/* ---- GPU meshes and instancing ---- */

mc_gpu_mesh mc_mesh_upload(const mc_mesh *m) {
    mc_gpu_mesh g = {0};
    g.index_count  = m->index_count;
    g.vertex_count = m->vert_count;
    for (int i = 0; i < m->vert_count; ++i) {
        float radius = mc_v3_len(m->verts[i].pos);
        if (radius > g.radius) g.radius = radius;
    }
    glGenVertexArrays(1, &g.vao);
    glBindVertexArray(g.vao);

    glGenBuffers(1, &g.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferData(GL_ARRAY_BUFFER, (long)(m->vert_count * sizeof(mc_vertex)), m->verts, GL_STATIC_DRAW);

    glGenBuffers(1, &g.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ibo);
    mc_u32 max_index = 0;
    for (int i = 0; i < m->index_count; ++i) {
        mc_assert(m->indices[i] < (mc_u32)m->vert_count);
        if (m->indices[i] > max_index) max_index = m->indices[i];
    }
    uint16_t *small = max_index <= UINT16_MAX && m->index_count > 0
                   ? malloc((size_t)m->index_count * sizeof *small) : NULL;
    g.index_type = small ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
    if (small) {
        for (int i = 0; i < m->index_count; ++i) small[i] = (uint16_t)m->indices[i];
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)m->index_count * sizeof *small, small, GL_STATIC_DRAW);
        free(small);
    } else {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)m->index_count * sizeof(mc_u32), m->indices, GL_STATIC_DRAW);
    }

    /* attrib 0 = position, attrib 1 = normal, attrib 7 = uv (2-5 are the instance
     * model matrix, 6 is instance colour). Shaders that don't read uv ignore it. */
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(mc_vertex), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(mc_vertex), (void *)offsetof(mc_vertex, normal));
    glEnableVertexAttribArray(7);
    glVertexAttribPointer(7, 2, GL_FLOAT, GL_FALSE, sizeof(mc_vertex), (void *)offsetof(mc_vertex, uv));

    glBindVertexArray(0);
    return g;
}

mc_gpu_mesh mc_mesh_procedural(int count, const mc_u32 *indices, float radius) {
    mc_gpu_mesh g = {0};
    g.radius = radius;
    glGenVertexArrays(1, &g.vao);
    if (!indices) { g.vertex_count = count; return g; }
    glBindVertexArray(g.vao);
    glGenBuffers(1, &g.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)count * (GLsizeiptr)sizeof *indices, indices, GL_STATIC_DRAW);
    glBindVertexArray(0);
    g.index_count = count;
    g.index_type = GL_UNSIGNED_INT;
    return g;
}

#ifndef MC_IMPORT_HASH   /* the payload frees no mesh: it exits (MC_TIDY) */
void mc_mesh_gpu_free(mc_gpu_mesh *g) {
    if (!g) return;
    if (g->inst_vbo) glDeleteBuffers(1, &g->inst_vbo);
    if (g->vbo) glDeleteBuffers(1, &g->vbo);
    if (g->ibo) glDeleteBuffers(1, &g->ibo);
    if (g->vao) glDeleteVertexArrays(1, &g->vao);
    memset(g, 0, sizeof *g);
}
#endif

/* Draw `count` instances of the mesh: by index if it has an index buffer,
 * otherwise its vertex_count vertices as a plain triangle list. */
static void mesh_draw(const mc_gpu_mesh *g, int count) {
    glBindVertexArray(g->vao);
    if (g->ibo) glDrawElementsInstanced(GL_TRIANGLES, g->index_count, g->index_type, (void *)0, count);
    else        glDrawArraysInstanced(GL_TRIANGLES, 0, g->vertex_count, count);
}

void mc_mesh_draw(mc_gpu_mesh g) { mesh_draw(&g, 1); }

/* The VAO keeps its buffer binding when the buffer's storage is replaced, so
 * the attributes only need repointing to attach a buffer or to select an
 * instance range. (GL 4.5 could select the range with a baseInstance draw,
 * but nothing here uses one.) */
static void setup_instance_attribs(int first) {
    size_t base = (size_t)first * sizeof(mc_instance);
    for (int i = 0; i < 4; ++i) {
        int loc = 2 + i;
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, sizeof(mc_instance),
                              (void *)(base + (size_t)i * sizeof(mc_v4)));
        glVertexAttribDivisor(loc, 1);
    }
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, sizeof(mc_instance),
                          (void *)(base + offsetof(mc_instance, color)));
    glVertexAttribDivisor(6, 1);
}

void mc_mesh_reserve_instances(mc_gpu_mesh *g, int capacity) {
    if (capacity <= g->inst_cap) return;
    glBindVertexArray(g->vao);
    if (!g->inst_vbo) glGenBuffers(1, &g->inst_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g->inst_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)capacity * (GLsizeiptr)sizeof(mc_instance),
                 NULL, GL_DYNAMIC_DRAW);
    g->inst_cap = capacity;
    setup_instance_attribs(0);
    glBindVertexArray(0);
}

void mc_mesh_set_instances(mc_gpu_mesh *g, const mc_instance *data, int count) {
    if (count <= 0) return;
    glBindVertexArray(g->vao);
    if (!g->inst_vbo) glGenBuffers(1, &g->inst_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g->inst_vbo);
    /* The buffer object remains attached to the VAO when storage grows. */
    if (count > g->inst_cap) {
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)count * (GLsizeiptr)sizeof(mc_instance),
                     NULL, GL_DYNAMIC_DRAW);
        g->inst_cap = count;
        setup_instance_attribs(0);
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)count * (GLsizeiptr)sizeof(mc_instance), data);
    glBindVertexArray(0);
}

void mc_mesh_draw_instanced(mc_gpu_mesh g, int count) { mesh_draw(&g, count); }

void mc_mesh_draw_instance_range(mc_gpu_mesh g, int first, int count) {
    if (count <= 0) return;
    glBindVertexArray(g.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g.inst_vbo);
    if (first) setup_instance_attribs(first);
    mesh_draw(&g, count);
    /* Point back at instance zero for ordinary draws. */
    if (first) setup_instance_attribs(0);
}

/* ---- render targets (FBOs) ---- */

mc_target mc_target_make(int w, int h, int hdr, int with_depth) {
    return mc_target_make_format(w, h, hdr ? GL_RGBA16F : GL_RGBA8, with_depth);
}

mc_target mc_target_make_format(int w, int h, GLenum format, int with_depth) {
    mc_target t = {0};
    /* A size past GL_MAX_TEXTURE_SIZE leaves the framebuffer incomplete,
     * which is caught below like any other failure. */
    if (w <= 0 || h <= 0 ||
        (!format && !with_depth) ||
        (format && format != GL_R8 && format != GL_RGBA8 && format != GL_RGBA16F)) return t;
    t.w = w; t.h = h; t.hdr = format == GL_RGBA16F;
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);

    if (format) {
    glGenTextures(1, &t.color);
    glBindTexture(GL_TEXTURE_2D, t.color);
    glTexImage2D(GL_TEXTURE_2D, 0, format, w, h, 0, format == GL_R8 ? GL_RED : GL_RGBA,
                 t.hdr ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.color, 0);

    } else {
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    }
    if (with_depth) {
        /* A depth texture rather than a renderbuffer, so the post passes (AO,
         * depth of field, TAA) can sample scene depth. Depth testing works the
         * same either way. */
        glGenTextures(1, &t.depth);
        glBindTexture(GL_TEXTURE_2D, t.depth);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, t.depth, 0);
    }
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        mc_log("target incomplete (%dx%d hdr=%d status=0x%04x)", w, h, t.hdr, (unsigned)status);
        mc_target_free(&t);   /* leaves t zeroed; t.fbo == 0 signals failure */
        return t;
    }
    return t;
}

/* A target is usable iff framebuffer creation succeeded (color or depth-only). */
int mc_target_ok(const mc_target *t) { return t && t->fbo != 0; }

void mc_target_free(mc_target *t) {
    if (!t) return;
    if (t->depth) glDeleteTextures(1, &t->depth);
    if (t->color) glDeleteTextures(1, &t->color);
    if (t->fbo)   glDeleteFramebuffers(1, &t->fbo);
    memset(t, 0, sizeof *t);
}

void mc_target_bind_fbo(GLuint fbo, int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
}
void mc_target_bind(const mc_target *t) {
    if (t) mc_target_bind_fbo(t->fbo, t->w, t->h);
    else   glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* The depth test, depth writes, culling and blending, two bits each (mc_render.h). */
void mc_gl_switch(int s) {
    static const GLenum cap[4] = { GL_DEPTH_TEST, 0, GL_CULL_FACE, GL_BLEND };
    for (int i = 0; i < 4; ++i, s >>= 2)
        if (s & 3) {
            if (i == 1) glDepthMask(s & 1);
            else if (s & 1) glEnable(cap[i]);
            else glDisable(cap[i]);
        }
}

/* One oversized triangle covers the screen with no vertex data: the vertex
 * shader builds the corners from gl_VertexID. The core profile still wants a
 * VAO bound, so we keep an empty one around. */
void mc_fullscreen_triangle(void) {
    glBindVertexArray(mc_gl_fullscreen_vao());
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* ---- post-processing ---- */

mc_post mc_post_defaults(void) {
    mc_post p = {0};
    p.exposure = 1.1f;
    p.bloom = 0.08f;
    p.bloom_thresh = 1.0f;
    p.vignette = 0.32f;
    p.grain = 0.012f;   /* fine texture without masking small silhouettes or material detail */
    p.chroma = 0.35f;
    p.saturation = 1.08f;
    p.fade = 1.0f;
    p.time = 0.0f;
    p.ssao = 0.0f;
    p.znear = 0.1f; p.zfar = 400.0f;
    p.projx = 1.0f; p.projy = 1.0f;
    return p;
}

/* The post-pass shaders live in src/shaders: post.vert plus one .frag per
 * pass, and the GTAO and TAA shaders explain how they work in their own
 * comments. genshaders.py bakes them into this generated header as POST_VS,
 * BRIGHT_FS and so on: minified in release builds, sharing the demo shaders'
 * rename mapping, and verbatim in dev builds. */
#include "mc_engine_shaders.h"

#define MC_BLOOM_LEVELS 6         /* half resolution down to 1/64 */

struct mc_renderer {
    int       w, h;
    int       output_w, output_h, scale, texture_limit;
    union {                       /* every target, by name or as one array (for make/check/free loops) */
        struct {
            mc_target scene, last;        /* HDR + depth texture, and last frame's (the TAA reads its depth) */
            mc_target bloom[MC_BLOOM_LEVELS]; /* the bright pass, then its mip chain (half res down) */
            mc_target ao, ao_b;           /* half-res AO (.r) + contact shadows (.g); ao_b: bilateral blur scratch */
            mc_target dof_a, dof_b;       /* quarter-res depth of field: scene + CoC, then bokeh */
            mc_target ldr;                /* the composite at the scene's size, pre-TAA (RGBA16F) */
            mc_target hist[2];            /* TAA history, ping-ponged; output size, or the frame's if larger */
        };
        mc_target all[MC_BLOOM_LEVELS + 9];
    };
    union {                       /* the post passes' programs, by name or as one array */
        struct { mc_shader bright, blur, composite, taa, present, gtao, ao_blur, dofdown, dofgather; };
        mc_shader pass[9];
    };
    /* TAA state. t is the frame's time and n the frame count since the
     * history last started over. vp and view are the frame's first camera,
     * pvp is the last frame's view-projection, and jit is this frame's
     * sub-pixel jitter in scene pixels. */
    double    t;
    int       n, have, flip;
    mc_m4     vp, view, pvp;
    mc_v2     jit;
};

/* The post passes' parameters. This mirrors the std140 Post block that every
 * post shader declares, and is uploaded once per frame through the frame's
 * uniform buffer. */
typedef struct {
    mc_v3 lens; float zn;
    mc_v2 spare, rcp, res;        /* spare, res: 0 */
    float zf, exposure, bloom, vignette, grain, chroma, saturation, fade, white, time,
          ao, blur, projx, projy, thresh, seed;
    mc_v2 jit;
    mc_v4 sunv;                   /* toward the sun, view space; w = 1: contact shadows on */
    mc_m4 reproj, cvp;            /* last frame's view-projection, then this frame's */
} post_block;
_Static_assert(sizeof(post_block) == 256, "post_block must match the std140 Post block");
_Static_assert(offsetof(mc_renderer, hist) + 2 * sizeof(mc_target) == offsetof(mc_renderer, all) + sizeof(((mc_renderer *)0)->all) &&
               offsetof(mc_renderer, dofgather) + sizeof(mc_shader) == offsetof(mc_renderer, pass) + sizeof(((mc_renderer *)0)->pass),
               "the target and program arrays must cover every named one");

static int renderer_make_targets(mc_renderer *r, int w, int h, int ow, int oh) {
    r->w = w; r->h = h;
    r->scene   = mc_target_make(w, h, 1, 1);
    r->last    = mc_target_make(w, h, 1, 1);
    int bw = (w + 1) / 2, bh = (h + 1) / 2, ok = 1;
    for (int i = 0; i < MC_BLOOM_LEVELS; ++i)
        r->bloom[i] = mc_target_make((w + (2 << i) - 1) >> (i + 1), (h + (2 << i) - 1) >> (i + 1), 1, 0);
    r->ao      = mc_target_make(bw, bh, 0, 0);
    r->ao_b    = mc_target_make(bw, bh, 0, 0);
    r->dof_a   = mc_target_make((w + 3) / 4, (h + 3) / 4, 1, 0);
    r->dof_b   = mc_target_make((w + 3) / 4, (h + 3) / 4, 1, 0);
    r->ldr     = mc_target_make(w, h, 1, 0);   /* half floats, so the TAA keeps detail finer than 8-bit steps */
    /* The history is the output's size, or the frame's when that is larger.
     * When supersampling, the TAA resolves the frame at its own size and the
     * present pass filters it down. */
    if (w > ow) { ow = w; oh = h; }
    r->hist[0] = mc_target_make(ow, oh, 1, 0);
    r->hist[1] = mc_target_make(ow, oh, 1, 0);
    r->t = -1e9;                  /* new history: start over */
    for (int i = 0; i < MC_ARRAY_COUNT(r->all); ++i) ok &= mc_target_ok(&r->all[i]);
    return ok;
}

mc_renderer *mc_renderer_make(int w, int h) {
    GLint limit = 0; glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
    if (w <= 0 || h <= 0 || w > limit || h > limit) return NULL;
    mc_renderer *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->output_w = w; r->output_h = h; r->scale = 100; r->texture_limit = limit;
    if (!renderer_make_targets(r, w, h, w, h)) { MC_TIDY(mc_renderer_free(r)); return NULL; }

    r->bright    = mc_shader_make(POST_VS, BRIGHT_FS);
    r->blur      = mc_shader_make(POST_VS, BLUR_FS);
    r->composite = mc_shader_make(POST_VS, COMPOSITE_FS);
    r->taa       = mc_shader_make(POST_VS, TAA_FS);
    r->present   = mc_shader_make(POST_VS, PRESENT_FS);
    r->gtao      = mc_shader_make(POST_VS, GTAO_FS);
    r->ao_blur   = mc_shader_make(POST_VS, AO_BLUR_FS);
    r->dofdown   = mc_shader_make(POST_VS, DOFDOWN_FS);
    r->dofgather = mc_shader_make(POST_VS, DOFGATHER_FS);
    for (int i = 0; i < MC_ARRAY_COUNT(r->pass); ++i)
        if (!r->pass[i].prog) {
            mc_log("renderer: post shader compilation failed");
            MC_TIDY(mc_renderer_free(r));
            return NULL;
        }

    return r;   /* nothing more to set: the texture units and the blurs' dir are fixed in the shaders */
}

static void renderer_free_targets(mc_renderer *r) {
    for (int i = 0; i < MC_ARRAY_COUNT(r->all); ++i) mc_target_free(&r->all[i]);
}

void mc_renderer_free(mc_renderer *r) {
    if (!r) return;
    renderer_free_targets(r);
    for (int i = 0; i < MC_ARRAY_COUNT(r->pass); ++i) mc_shader_free(&r->pass[i]);
    free(r);
}

static int renderer_resize(mc_renderer *r, int w, int h, int scale) {
    if (!r || w <= 0 || h <= 0 || scale < 25 || scale > 200) return 0;
    if (w == r->output_w && h == r->output_h && scale == r->scale) return 1;
    int rw = (int)(((int64_t)w * scale + 50) / 100);
    int rh = (int)(((int64_t)h * scale + 50) / 100);
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    if (rw > r->texture_limit || rh > r->texture_limit) return 0;
    mc_renderer next = {0};
    if (!renderer_make_targets(&next, rw, rh, w, h)) {
        renderer_free_targets(&next);
        return 0;
    }
    renderer_free_targets(r);
    r->w = next.w; r->h = next.h;
    r->output_w = w; r->output_h = h; r->scale = scale;
    memcpy(r->all, next.all, sizeof r->all);
    r->t = next.t;
    return 1;
}

int mc_renderer_resize(mc_renderer *r, int w, int h) {
    return r ? renderer_resize(r, w, h, r->scale) : 0;
}
int mc_renderer_set_scale(mc_renderer *r, int percent) {
    return r ? renderer_resize(r, r->output_w, r->output_h, percent) : 0;
}
void mc_renderer_size(const mc_renderer *r, int *w, int *h) {
    if (w) *w = r->w;
    if (h) *h = r->h;
}
float mc_renderer_aspect(const mc_renderer *r) { return (float)r->output_w / (float)r->output_h; }

/* Re-bind the HDR scene target and its viewport after the caller has drawn
 * into another framebuffer mid-frame, such as a shadow map pass between
 * begin() and end(). Doesn't clear. */
void mc_renderer_bind_scene(mc_renderer *r) { mc_target_bind(&r->scene); }
GLuint mc_renderer_depth(const mc_renderer *r) { return r->scene.depth; }
GLuint mc_renderer_color(const mc_renderer *r) { return r->scene.color; }

/* TAA. The history carries on while the clock runs smoothly forward (by less
 * than 0.25 s a frame). A jump, a pause, a step back or a resize starts it
 * over, so a frame rendered on its own, or twice at the same time, comes out
 * as the same plain frame. */
void mc_renderer_frame(mc_renderer *r, double t) {
    double dt = t - r->t;
    r->n = dt > 0.0 && dt < 0.25 ? r->n + 1 : 0;
    r->t = t;
    r->have = 0;
}

/* The camera's view-projection, offset by this frame's sub-pixel step so that
 * eight frames sample eight different spots inside each pixel. The frame's
 * first camera is the one the history follows. A camera cut needs nothing
 * extra: the TAA clamps the history to the colours around each pixel in the
 * new frame, and drops it quickly where the depth has changed. */
mc_m4 mc_renderer_camera(mc_renderer *r, mc_m4 proj, mc_m4 view) {
    if (!r->have) {
        r->have = 1;
        r->vp = mc_m4_mul(proj, view);
        r->view = view;
        /* the first eight points of the Halton (2, 3) sequence, in sixteenths of a pixel from its centre */
        static const signed char spot[16] = { 0, -3, -4, 3, 4, -6, -6, -1, 2, 4, -2, -4, 6, 1, -7, 6 };
        int i = (r->n - 1) & 7;
        r->jit = r->n ? mc_vec2(spot[2 * i] / 16.0f, spot[2 * i + 1] / 16.0f) : mc_vec2(0.0f, 0.0f);
    }
    proj.m[8] -= 2.0f * r->jit.x / (float)r->w;
    proj.m[9] -= 2.0f * r->jit.y / (float)r->h;
    return mc_m4_mul(proj, view);
}

void mc_renderer_begin(mc_renderer *r, mc_v3 clear) {
    MC_PASS(MC_P_SCENE);
    mc_target_bind(&r->scene);
    glDisable(GL_BLEND);
    /* Depth writes MUST be enabled before clearing, or a masked depth clear is
     * a no-op and we inherit stale depth from the previous frame. */
    glDepthMask(GL_TRUE);
    glClearColor(clear.x, clear.y, clear.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    mc_gl_switch(MC_ZTEST_ON | MC_ZWRITE_ON | MC_CULL_ON);
    glDepthFunc(GL_LEQUAL);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
}

static void bind_tex(GLuint tex, int unit) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex);
}

void mc_renderer_blur(mc_renderer *r, mc_target *a, mc_target *b, float zn, float zf) {
    mc_shader_use(r->ao_blur);
    bind_tex(r->scene.depth, 1);
    mc_target_bind(b); bind_tex(a->color, 0);
    mc_set_v4(0, mc_vec4(1.0f / (float)a->w, 0.0f, zn, zf)); mc_fullscreen_triangle();
    mc_target_bind(a); bind_tex(b->color, 0);
    mc_set_v4(0, mc_vec4(0.0f, 1.0f / (float)a->h, zn, zf)); mc_fullscreen_triangle();
}

void mc_renderer_end(mc_renderer *r, GLuint output_fbo, int out_w, int out_h, mc_post p) {
    MC_PASS_END();
    mc_gl_switch(MC_ZTEST_OFF | MC_ZWRITE_OFF | MC_CULL_OFF | MC_BLEND_OFF);
    mc_v4 sv = mc_m4_mul_v4(r->view, mc_vec4(p.sun_dir.x, p.sun_dir.y, p.sun_dir.z, 0.0f));
    sv.w = mc_v3_len(p.sun_dir) > 0.5f;
    post_block pb = { mc_vec3(p.focus, p.dof, p.vol_tex ? p.vol : 0.0f), p.znear,
                      mc_vec2(0.0f, 0.0f), mc_vec2(1.0f / (float)r->w, 1.0f / (float)r->h),
                      mc_vec2(0.0f, 0.0f),
                      p.zfar, p.exposure, p.bloom, p.vignette, p.grain, p.chroma, p.saturation, p.fade,
                      p.white, p.time, p.ssao, p.blur, p.projx, p.projy, p.bloom_thresh, (float)r->n,
                      r->jit, sv, r->n ? r->pvp : r->vp, r->vp };
    mc_gl_frame_block(&pb, sizeof pb);

    /* Ambient occlusion (GTAO) from the scene depth into r->ao, with the
     * contact shadows in .g, then a separable depth-aware filter. This runs
     * only if p.ssao is above 0 or there's a sun, and with neither, nothing is
     * computed or sampled. */
    if (p.ssao > 0.0f || sv.w > 0.0f) {
        MC_PASS(MC_P_AO);
        mc_target_bind(&r->ao);
        mc_shader_use(r->gtao);
        bind_tex(r->scene.depth, 0);
        bind_tex(r->scene.color, 1);
        mc_fullscreen_triangle();
        MC_PASS(MC_P_AO_FILTER);
        mc_renderer_blur(r, &r->ao, &r->ao_b, p.znear, p.zfar);
    }

    /* Depth of field. The scene and its circles of confusion go down to
     * quarter resolution, then the bokeh gather runs on that. The composite
     * blends the result in per pixel, so sharp pixels keep full resolution.
     * It only runs when p.dof > 0. */
    if (p.dof > 0.0f) {
        MC_PASS(MC_P_DOF);
        mc_target_bind(&r->dof_a);
        mc_shader_use(r->dofdown);
        bind_tex(r->scene.color, 0);
        bind_tex(r->scene.depth, 1);
        mc_fullscreen_triangle();
        mc_target_bind(&r->dof_b);
        mc_shader_use(r->dofgather);
        bind_tex(r->dof_a.color, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glGenerateMipmap(GL_TEXTURE_2D);   /* the gather reads bright points two levels down */
        mc_fullscreen_triangle();
    }

    /* Bright pass: copy the parts of the scene above the threshold into
     * bloom[0], already at half resolution. Each pixel is a Karis-weighted
     * average of a 4 x 4 block, so a single glint can't flash a halo. */
    /* Bloom: halve the bright pass down a chain of six levels, with Jimenez's
     * thirteen taps at each step, then come back up, adding each level onto
     * the one above through a 3 x 3 tent. That gives a tight core with a wide,
     * soft halo. */
    if (p.bloom > 0.0f) {
    MC_PASS(MC_P_BRIGHT);
    mc_target_bind(&r->bloom[0]);
    mc_shader_use(r->bright);
    bind_tex(r->scene.color, 0);
    mc_fullscreen_triangle();
    MC_PASS(MC_P_BLOOM);
    mc_shader_use(r->blur);
    for (int i = 1; i < 2 * MC_BLOOM_LEVELS - 1; ++i) {
        int up = i >= MC_BLOOM_LEVELS, k = up ? 2 * MC_BLOOM_LEVELS - 2 - i : i;   /* draw into level k */
        mc_target *s = &r->bloom[up ? k + 1 : k - 1];
        if (up) { glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE); }
        mc_target_bind(&r->bloom[k]);
        bind_tex(s->color, 0);
        mc_set_v2(0, mc_vec2((up ? -1.0f : 1.0f) / (float)s->w, 1.0f / (float)s->h));   /* dir.x < 0: going up, with the tent */
        mc_fullscreen_triangle();
    }
    glDisable(GL_BLEND);
    }

    /* Composite into the LDR target, still at the scene's size: motion blur,
     * exposure, tonemap, grade and the bloom mix. */
    MC_PASS(MC_P_COMPOSITE);
    mc_target_bind(&r->ldr);
    mc_shader_use(r->composite);
    bind_tex(r->scene.color, 0);
    bind_tex(r->bloom[0].color, 1);
    bind_tex(r->ao.color, 2);
    bind_tex(r->scene.depth, 4);
    bind_tex(r->dof_b.color, 5);
    bind_tex(p.vol_tex, 6);
    mc_fullscreen_triangle();

    /* Finally the TAA. This frame's jittered samples are folded into the
     * history, which is kept at the output size, so this also upsamples a
     * reduced scene. The present pass then draws the history into the
     * output framebuffer with the film grain on top. It's centred there, with
     * black around it if the framebuffer is a different size (the demo's
     * letterbox). */
    MC_PASS(MC_P_TAA);
    mc_shader_use(r->taa);
    bind_tex(r->ldr.color, 0);
    bind_tex(r->hist[r->flip].color, 1);
    bind_tex(r->scene.depth, 2);
    bind_tex(r->last.depth, 3);
    mc_target_bind(&r->hist[r->flip ^= 1]);
    mc_fullscreen_triangle();
    glBindFramebuffer(GL_FRAMEBUFFER, output_fbo);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport((out_w - r->output_w) / 2, (out_h - r->output_h) / 2, r->output_w, r->output_h);
    mc_shader_use(r->present);
    bind_tex(r->hist[r->flip].color, 0);
    mc_fullscreen_triangle();
    r->pvp = r->vp;
    mc_target t = r->scene; r->scene = r->last; r->last = t;

    glActiveTexture(GL_TEXTURE0);
    glDepthMask(GL_TRUE);   /* leave depth writable for the next frame's clear */
    MC_GL_CHECK();
    MC_PASS_END();
}
