/*
 * tiny64.h - the quick way to get one fragment shader on screen with
 * MICROCOSM.
 *
 * The rest of the engine is explicit: you open a window, build a renderer,
 * upload meshes, cache uniform locations and drive a post stack. The demo
 * needs that much control, but it's too much when all you want is to try a
 * fragment shader on the screen.
 *
 * tiny64 wraps the public mc_* API in a handful of functions, so a complete
 * windowed, animated single-shader intro looks like this:
 *
 *     #define TINY64_IMPL          // in exactly ONE .c file
 *     #include <microcosm/tiny64.h>
 *
 *     static const char *FRAG =
 *         "#version 450 core\n"
 *         "in vec2 uv; out vec4 o; uniform float t;"
 *         "void main(){ o = vec4(0.5+0.5*cos(t+uv.xyx+vec3(0,2,4)), 1.0); }";
 *
 *     int main(void) {
 *         t64_open(1280, 720, "hello");
 *         t64_shader(FRAG);
 *         while (t64_running()) {
 *             t64_set1f("t", (float)t64_time());
 *             t64_quad();
 *             t64_swap();
 *         }
 *         t64_close();
 *     }
 *
 * Your fragment shader runs over the whole window and gets an `in vec2 uv` in
 * [0,1], with (0,0) at the bottom left as in GL, plus whatever uniforms you
 * set by name. There's no HDR, bloom or mesh path here. Once you outgrow a
 * single pass, move to the full mc_render.h API.
 *
 * It's header-only. Define TINY64_IMPL before the include in one translation
 * unit to pull in the implementation, and include it bare everywhere else.
 * The implementation only calls the public engine API (mc_window_*,
 * mc_shader_*, mc_fullscreen_triangle, ...) and the GL loader table, so you
 * link it like any other program that uses the engine.
 *
 * It's for windowed and dev builds, and it can't be packed into the 64k
 * payload. The payload's compile (MC_IMPORT_HASH) doesn't declare
 * mc_uniform, mc_set_f or mc_set_v3, which the uniform setters below call,
 * or mc_window_close, which t64_close calls. A demo that will be packed uses
 * the full API and sets its uniforms by layout(location).
 */
#ifndef MICROCOSM_TINY64_H
#define MICROCOSM_TINY64_H

#include "mc_platform.h"
#include "mc_render.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- lifecycle ---------------------------------------------------------- */
/* Open a window and a GL 4.5 context. Returns 1 on success and 0 on failure,
 * in which case don't call anything else. Call it once. */
int    t64_open(int width, int height, const char *title);
void   t64_close(void);
/* Pump events and report whether to keep going; 0 after a close/ESC request. */
int    t64_running(void);
/* Present the frame (buffer swap, vsync-paced on the windowed backend). */
void   t64_swap(void);
/* Seconds since t64_open(), monotonic: the clock to animate your shader with. */
double t64_time(void);
/* Current framebuffer size, for an aspect-ratio or resolution uniform. */
void   t64_size(int *width, int *height);

/* ---- the single full-screen shader -------------------------------------- */
/* Compile `fragment_src` against a built-in full-screen-triangle vertex shader
 * and make it current. It's safe to call again to swap shaders. On a compile
 * error the previous shader (if any) is kept, so a typo doesn't blank the
 * screen. */
void   t64_shader(const char *fragment_src);
/* The same, but with `glsl_lib` (helper functions, #defines, an SDF kit, ...)
 * put in front of `fragment_src`. The compiler sees both as one source, so if
 * you pass a lib, put the `#version` line in it and leave it out of
 * `fragment_src`. */
void   t64_shader_lib(const char *glsl_lib, const char *fragment_src);
/* Draw the full-screen triangle with the current shader into the window. */
void   t64_quad(void);

/* ---- uniforms by name (location looked up once, then cached) ------------ */
void   t64_set1f(const char *name, float v);
void   t64_set2f(const char *name, float a, float b);
void   t64_set3f(const char *name, float a, float b, float c);
void   t64_set4f(const char *name, float a, float b, float c, float d);
void   t64_set1i(const char *name, int v);

/*
 * Opt-in short names: running(), swap(), shader(), fullscreen_quad() and
 * uniform1f(). They're off by default because the bare names collide with
 * everyday code. time() would clash with <time.h>, so there's no macro for
 * it; use t64_time().
 */
#ifdef TINY64_SHORT_NAMES
#define running()         t64_running()
#define swap()            t64_swap()
#define shader(src)       t64_shader(src)
#define fullscreen_quad() t64_quad()
#define uniform1f(n, v)   t64_set1f((n), (v))
#endif

#ifdef __cplusplus
}
#endif

/* implementation - compiled into the one TU that defines TINY64_IMPL */
#ifdef TINY64_IMPL

#include <stdlib.h>   /* malloc, free */
#include <string.h>   /* strcmp       */

/* A full-screen triangle with no vertex buffer. gl_VertexID picks the three
 * corners that cover the viewport, and uv comes out in [0,1]. The engine's
 * post passes use the same trick. */
static const char *t64__vs =
    "#version 450 core\n"
    "out vec2 uv;"
    "void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
    "uv=p;gl_Position=vec4(p*2.0-1.0,0.0,1.0);}";

/* A tiny name->location cache so setting uniforms by name every frame doesn't
 * hit glGetUniformLocation each time. 32 slots is plenty for a single pass. */
#define T64__MAX_UNIFORMS 32

static struct {
    mc_window *win;
    mc_shader  sh;
    double     t0;
    int        running;
    struct { char name[32]; int loc; } cache[T64__MAX_UNIFORMS];
    int        ncache;
} t64__;

int t64_open(int width, int height, const char *title) {
    mc_window_desc desc = { width, height, title ? title : "tiny64", 1, 0 };
    t64__.win = mc_window_open(&desc);
    if (!t64__.win) return 0;
    t64__.t0 = mc_time_seconds();
    t64__.running = 1;
    t64__.ncache = 0;
    return 1;
}

void t64_close(void) {
    mc_shader_free(&t64__.sh);
    if (t64__.win) mc_window_close(t64__.win);
    t64__.win = NULL;
}

int t64_running(void) {
    if (!t64__.win) return 0;
    if (!mc_window_pump(t64__.win)) t64__.running = 0;
    return t64__.running;
}

void t64_swap(void) { if (t64__.win) mc_window_swap(t64__.win); }

double t64_time(void) { return mc_time_seconds() - t64__.t0; }

void t64_size(int *width, int *height) { mc_window_size(t64__.win, width, height); }

void t64_shader_lib(const char *glsl_lib, const char *fragment_src) {
    /* Concatenate lib + fragment into one source string for the compiler. */
    const char *lib = glsl_lib ? glsl_lib : "";
    const char *frg = fragment_src ? fragment_src : "";
    size_t la = strlen(lib), lb = strlen(frg);
    char *buf = (char *)malloc(la + lb + 1);
    if (!buf) { mc_log("tiny64: out of memory building shader source"); return; }
    memcpy(buf, lib, la);
    memcpy(buf + la, frg, lb);
    buf[la + lb] = 0;

    mc_shader ns = mc_shader_make(t64__vs, buf);
    free(buf);
    if (!ns.prog) {
        mc_log("tiny64: shader failed to build - keeping the previous one");
        return;
    }
    mc_shader_free(&t64__.sh);
    t64__.sh = ns;
    t64__.ncache = 0;   /* locations belong to the old program; drop them */
}

void t64_shader(const char *fragment_src) { t64_shader_lib("", fragment_src); }

/* Look up (and remember) a uniform location by name in the current program. */
static int t64__loc(const char *name) {
    for (int i = 0; i < t64__.ncache; ++i)
        if (!strcmp(t64__.cache[i].name, name)) return t64__.cache[i].loc;
    int loc = mc_uniform(t64__.sh, name);
    if (t64__.ncache < T64__MAX_UNIFORMS) {
        /* names too long for a slot still work, they just aren't cached */
        size_t k = 0;
        for (; name[k] && k < sizeof t64__.cache[0].name - 1; ++k)
            t64__.cache[t64__.ncache].name[k] = name[k];
        if (!name[k]) {
            t64__.cache[t64__.ncache].name[k] = 0;
            t64__.cache[t64__.ncache].loc = loc;
            ++t64__.ncache;
        }
    }
    return loc;
}

void t64_set1f(const char *name, float v)                               { mc_shader_use(t64__.sh); mc_set_f(t64__loc(name), v); }
void t64_set2f(const char *name, float a, float b)                      { mc_shader_use(t64__.sh); mc_set_v2(t64__loc(name), mc_vec2(a, b)); }
void t64_set3f(const char *name, float a, float b, float c)             { mc_shader_use(t64__.sh); mc_set_v3(t64__loc(name), mc_vec3(a, b, c)); }
void t64_set4f(const char *name, float a, float b, float c, float d)    { mc_shader_use(t64__.sh); mc_set_v4(t64__loc(name), mc_vec4(a, b, c, d)); }
void t64_set1i(const char *name, int v)                                 { mc_shader_use(t64__.sh); mc_set_i(t64__loc(name), v); }

void t64_quad(void) {
    int w, h;
    mc_window_size(t64__.win, &w, &h);
    mc_target_bind_fbo(mc_window_output_fbo(t64__.win), w, h);
    mc_shader_use(t64__.sh);
    mc_fullscreen_triangle();
}

#endif /* TINY64_IMPL */
#endif /* MICROCOSM_TINY64_H */
