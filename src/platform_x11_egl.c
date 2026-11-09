/*
 * platform_x11_egl.c - the on-screen backend: a plain X11 window with an EGL
 * OpenGL context on it.
 *
 * It draws to the default framebuffer (FBO 0), which the renderer's last post
 * pass resolves straight into. It also owns the window lifecycle, resize
 * handling and the handful of keys the demo listens for. This is the backend in
 * dev/release/dist builds. A program linked with the headless twin
 * (platform_surfaceless.c) instead renders with no window.
 *
 * It uses EGL rather than the older GLX because that's the call sequence the
 * headless backend uses too, so the two stay in step, and nothing here needs
 * libGL.
 */
#include "microcosm/mc_platform.h"
#include "microcosm/mc_gl.h"
#include "microcosm/mc.h"

#include <EGL/egl.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <stdlib.h>
#include <string.h>
#include "microcosm/mc_imports.h"   /* payload calls by hash; keep after system headers */

void mc_pixels_flip(unsigned char *rgba, int w, int h); /* platform_common.c */

struct mc_window {
    Display   *xdpy;
    Window     win;
    Atom       wm_delete;
    EGLDisplay dpy;
    EGLContext ctx;
    EGLSurface surf;
    int        w, h;
    int        should_close;
    unsigned char keys[MC_KEY_COUNT];
};

static int map_key(KeySym ks, mc_key *out) {
    switch (ks) {
    case XK_Escape: *out = MC_KEY_ESCAPE; return 1;
    case XK_space:  *out = MC_KEY_SPACE;  return 1;
    case XK_Left:   *out = MC_KEY_LEFT;   return 1;
    case XK_Right:  *out = MC_KEY_RIGHT;  return 1;
    case XK_Up:     *out = MC_KEY_UP;     return 1;
    case XK_Down:   *out = MC_KEY_DOWN;   return 1;
    default: return 0;
    }
}

mc_window *mc_window_open(const mc_window_desc *desc) {
    mc_window *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->w = desc->width;
    w->h = desc->height;

    w->xdpy = XOpenDisplay(NULL);
    if (!w->xdpy) { mc_log("x11: cannot open display"); MC_TIDY(free(w)); return NULL; }

    w->dpy = eglGetDisplay((EGLNativeDisplayType)w->xdpy);
    if (w->dpy == EGL_NO_DISPLAY || !eglInitialize(w->dpy, NULL, NULL)) {
        mc_log("egl: init failed"); MC_TIDY(XCloseDisplay(w->xdpy); free(w)); return NULL;
    }
    eglBindAPI(EGL_OPENGL_API);

    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
        EGL_NONE
    };
    EGLConfig cfg; EGLint ncfg = 0;
    if (!eglChooseConfig(w->dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg == 0) {
        mc_log("egl: no matching config"); goto fail;
    }

    /* X needs a visual whose pixel format matches the EGL config, so ask EGL
     * which native visual id it chose and look that up. If the lookup comes up
     * empty, fall back to the screen's default visual and hope for the best. */
    EGLint vid = 0;
    eglGetConfigAttrib(w->dpy, cfg, EGL_NATIVE_VISUAL_ID, &vid);
    XVisualInfo tmpl; memset(&tmpl, 0, sizeof tmpl);
    tmpl.visualid = (VisualID)vid;
    int nvi = 0;
    XVisualInfo *vi = XGetVisualInfo(w->xdpy, VisualIDMask, &tmpl, &nvi);
    Visual *visual;
    int depth;
    if (vi && nvi > 0) { visual = vi->visual; depth = vi->depth; }
    else { visual = DefaultVisual(w->xdpy, DefaultScreen(w->xdpy));
           depth = DefaultDepth(w->xdpy, DefaultScreen(w->xdpy)); }

    Window root = RootWindow(w->xdpy, DefaultScreen(w->xdpy));
    Colormap cmap = XCreateColormap(w->xdpy, root, visual, AllocNone);
    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof swa);
    swa.colormap = cmap;
    swa.background_pixel = 0;
    swa.border_pixel = 0;
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | StructureNotifyMask;

    /* Fullscreen: the window is created at the screen's size, so it's right
     * from the first frame, even on a server with no window manager to honour
     * the hint below. The pointer over it is blank: an empty 1 x 1 bitmap is
     * both its picture and its mask. (With cursor None, a window would show its
     * parent's cursor.) */
    if (desc->fullscreen) {
        Screen *s = DefaultScreenOfDisplay(w->xdpy);
        w->w = WidthOfScreen(s);
        w->h = HeightOfScreen(s);
        XColor black = {0};
        Pixmap blank = XCreateBitmapFromData(w->xdpy, root, (const char *)&black, 1, 1);
        swa.cursor = XCreatePixmapCursor(w->xdpy, blank, blank, &black, &black, 0, 0);
    }

    w->win = XCreateWindow(w->xdpy, root, 0, 0, (unsigned)w->w, (unsigned)w->h, 0,
                           depth, InputOutput, visual,
                           CWColormap | CWEventMask | CWBackPixel | CWBorderPixel | CWCursor, &swa);
    if (vi) XFree(vi);
    if (!w->win) { mc_log("x11: cannot create window"); goto fail; }

    XStoreName(w->xdpy, w->win, desc->title ? desc->title : "microcosm");
    w->wm_delete = XInternAtom(w->xdpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(w->xdpy, w->win, &w->wm_delete, 1);

    /* Fullscreen also sets that hint (_NET_WM_STATE_FULLSCREEN) and asks a
     * compositing window manager (Cinnamon on the compo machine's Mint) to
     * leave the window unredirected. The frames then go to the screen as
     * drawn, without being copied through the compositor's own frame. That
     * means less latency and no judder from two clocks, and variable refresh
     * can kick in. */
    if (desc->fullscreen) {
        static const long bypass = 1;
        Atom wm_state = XInternAtom(w->xdpy, "_NET_WM_STATE", False);
        Atom fs = XInternAtom(w->xdpy, "_NET_WM_STATE_FULLSCREEN", False);
        XChangeProperty(w->xdpy, w->win, wm_state, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)&fs, 1);
        XChangeProperty(w->xdpy, w->win, XInternAtom(w->xdpy, "_NET_WM_BYPASS_COMPOSITOR", False), XA_CARDINAL, 32,
                        PropModeReplace, (const unsigned char *)&bypass, 1);
    }

    XMapWindow(w->xdpy, w->win);

    w->surf = eglCreateWindowSurface(w->dpy, cfg, (EGLNativeWindowType)w->win, NULL);
    if (w->surf == EGL_NO_SURFACE) { mc_log("egl: no window surface"); goto fail; }

    const EGLint ctx_attr[] = {
        EGL_CONTEXT_MAJOR_VERSION, 4,
        EGL_CONTEXT_MINOR_VERSION, 5,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    w->ctx = eglCreateContext(w->dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (w->ctx == EGL_NO_CONTEXT) { mc_log("egl: no context (0x%x)", eglGetError()); goto fail; }
    if (!eglMakeCurrent(w->dpy, w->surf, w->surf, w->ctx)) { mc_log("egl: makeCurrent failed"); goto fail; }

    eglSwapInterval(w->dpy, desc->vsync ? 1 : 0);

    if (!mc_gl_load((mc_gl_proc (*)(const char *))eglGetProcAddress)) {
        mc_log("gl: required entry points missing (need OpenGL 4.5 core)");
        goto fail;
    }

    mc_log("window: %s | %s", (const char *)glGetString(GL_RENDERER),
           (const char *)glGetString(GL_VERSION));
    return w;

fail:   /* unwind whatever we managed to create, in reverse */
    MC_TIDY(if (w->ctx) eglDestroyContext(w->dpy, w->ctx);
            if (w->surf) eglDestroySurface(w->dpy, w->surf);
            if (w->win) XDestroyWindow(w->xdpy, w->win);
            if (w->dpy != EGL_NO_DISPLAY) eglTerminate(w->dpy);
            if (w->xdpy) XCloseDisplay(w->xdpy);
            free(w));
    return NULL;
}

#ifndef MC_IMPORT_HASH   /* the payload never closes its window: it exits (MC_TIDY) */
void mc_window_close(mc_window *w) {
    if (!w) return;
    mc_gl_context_release();
    eglMakeCurrent(w->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (w->ctx)  eglDestroyContext(w->dpy, w->ctx);
    if (w->surf) eglDestroySurface(w->dpy, w->surf);
    eglTerminate(w->dpy);
    if (w->win)  XDestroyWindow(w->xdpy, w->win);
    XCloseDisplay(w->xdpy);
    free(w);
}
#endif

int mc_window_pump(mc_window *w) {
    while (XPending(w->xdpy)) {
        XEvent e;
        XNextEvent(w->xdpy, &e);
        switch (e.type) {
        case ClientMessage:
            if ((Atom)e.xclient.data.l[0] == w->wm_delete) w->should_close = 1;
            break;
        case ConfigureNotify:
            w->w = e.xconfigure.width;
            w->h = e.xconfigure.height;
            break;
        case KeyPress:
        case KeyRelease: {
            KeySym ks = XLookupKeysym(&e.xkey, 0);
            mc_key k;
            if (map_key(ks, &k)) {
                w->keys[k] = (e.type == KeyPress);
                if (k == MC_KEY_ESCAPE && e.type == KeyPress) w->should_close = 1;
            }
            break;
        }
        default: break;
        }
    }
    return !w->should_close;
}

void   mc_window_swap(mc_window *w) { eglSwapBuffers(w->dpy, w->surf); }
void   mc_window_size(mc_window *w, int *x, int *y) { if (x) *x = w->w; if (y) *y = w->h; }
GLuint mc_window_output_fbo(mc_window *w) { (void)w; return 0; }
int    mc_key_down(mc_window *w, mc_key k) { return (k < MC_KEY_COUNT) ? w->keys[k] : 0; }

#ifndef MC_IMPORT_HASH   /* the payload reads nothing back (mc_gl.h) */
int mc_window_read_pixels(mc_window *w, unsigned char *rgba) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w->w, w->h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    mc_pixels_flip(rgba, w->w, w->h);
    return 1;
}
#endif
