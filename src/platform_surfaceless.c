/*
 * platform_surfaceless.c - the headless backend, for CI, tests, and grabbing
 * screenshots on a machine with no display server.
 *
 * It brings up a surfaceless EGL context, with no window or X server, and
 * renders into its own RGBA8 FBO, which stands in for the window's default
 * framebuffer. mc_window_output_fbo() hands that FBO to the renderer to resolve
 * into, and read_pixels/save_png pull the result back out afterwards. A
 * program gets it by linking this file (with -lEGL -lm) instead of the
 * on-screen backend, platform_x11_egl.c.
 */
#include "microcosm/mc_platform.h"
#include "microcosm/mc_gl.h"
#include "microcosm/mc.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdlib.h>

void mc_pixels_flip(unsigned char *rgba, int w, int h); /* platform_common.c */

struct mc_window {
    EGLDisplay dpy;
    EGLContext ctx;
    GLuint     fbo, color, depth;
    int        w, h;
    int        frame;
};

/* Prefer Mesa's surfaceless platform if the extension is there. Otherwise take
 * whatever the default display is and hope it can render off-screen. */
static EGLDisplay open_display(void) {
    PFNEGLGETPLATFORMDISPLAYEXTPROC get =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get) {
        EGLDisplay d = get(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        if (d != EGL_NO_DISPLAY) return d;
    }
    return eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

mc_window *mc_window_open(const mc_window_desc *desc) {
    mc_window *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->w = desc->width;
    w->h = desc->height;

    w->dpy = open_display();
    if (w->dpy == EGL_NO_DISPLAY) { mc_log("egl: no display"); free(w); return NULL; }
    if (!eglInitialize(w->dpy, NULL, NULL)) { mc_log("egl: init failed"); free(w); return NULL; }
    eglBindAPI(EGL_OPENGL_API);

    const EGLint ctx_attr[] = {
        EGL_CONTEXT_MAJOR_VERSION, 4,
        EGL_CONTEXT_MINOR_VERSION, 5,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    w->ctx = eglCreateContext(w->dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attr);
    if (w->ctx == EGL_NO_CONTEXT) {
        mc_log("egl: context creation failed (0x%x)", eglGetError());
        eglTerminate(w->dpy); free(w); return NULL;
    }
    if (!eglMakeCurrent(w->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, w->ctx)) {
        mc_log("egl: makeCurrent failed (0x%x)", eglGetError());
        eglDestroyContext(w->dpy, w->ctx); eglTerminate(w->dpy); free(w); return NULL;
    }
    if (!mc_gl_load((mc_gl_proc (*)(const char *))eglGetProcAddress)) {
        mc_log("gl: required entry points missing (need OpenGL 3.3 core)");
        eglMakeCurrent(w->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(w->dpy, w->ctx);
        eglTerminate(w->dpy);
        free(w);
        return NULL;
    }

    /* Build the stand-in default framebuffer: an RGBA8 colour texture (so it can
     * be read back / sampled) plus a depth renderbuffer. */
    glGenFramebuffers(1, &w->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, w->fbo);
    glGenTextures(1, &w->color);
    glBindTexture(GL_TEXTURE_2D, w->color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w->w, w->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, w->color, 0);
    glGenRenderbuffers(1, &w->depth);
    glBindRenderbuffer(GL_RENDERBUFFER, w->depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w->w, w->h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, w->depth);
    GLenum fbs = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (fbs != GL_FRAMEBUFFER_COMPLETE) {
        mc_log("gl: output framebuffer incomplete (0x%04x)", (unsigned)fbs);
        mc_window_close(w);
        return NULL;
    }

    mc_log("headless: %s | %s", (const char *)glGetString(GL_RENDERER),
           (const char *)glGetString(GL_VERSION));
    return w;
}

void mc_window_close(mc_window *w) {
    if (!w) return;
    if (w->dpy != EGL_NO_DISPLAY) {
        mc_gl_context_release();
        eglMakeCurrent(w->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (w->ctx != EGL_NO_CONTEXT) eglDestroyContext(w->dpy, w->ctx);
        eglTerminate(w->dpy);
    }
    free(w);
}

int    mc_window_pump(mc_window *w) { w->frame++; return 1; }
void   mc_window_swap(mc_window *w) { (void)w; glFinish(); }
void   mc_window_size(mc_window *w, int *x, int *y) { if (x) *x = w->w; if (y) *y = w->h; }
GLuint mc_window_output_fbo(mc_window *w) { return w->fbo; }
int    mc_key_down(mc_window *w, mc_key k) { (void)w; (void)k; return 0; }

int mc_window_read_pixels(mc_window *w, unsigned char *rgba) {
    glBindFramebuffer(GL_FRAMEBUFFER, w->fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w->w, w->h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    mc_pixels_flip(rgba, w->w, w->h);
    return 1;
}
