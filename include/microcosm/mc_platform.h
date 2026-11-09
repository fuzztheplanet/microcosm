/*
 * mc_platform.h - the thin layer between the engine and the OS: a window, a GL
 * context, input, a clock, and reading back the finished picture.
 *
 * Two backends implement this API, and you link exactly one:
 *   - src/platform_x11_egl.c      an on-screen window (X11 + EGL)
 *   - src/platform_surfaceless.c  off-screen EGL, for tests and screenshots
 * The choice is made only by which file you link (the surfaceless one needs
 * just -lEGL, no -lX11); no macro selects it.
 * Both expose the same functions, so the code above them doesn't know which
 * one it's using, and the same demo can render to a window or to a PNG on a
 * headless machine.
 *
 * The renderer works in its own HDR target and then resolves the final image
 * into an "output framebuffer". On the windowed backend that's framebuffer 0,
 * the one the driver shows. The headless backend has none, so it gives you an
 * RGBA8 FBO to read back or save. mc_window_output_fbo() returns whichever one
 * applies.
 */
#ifndef MICROCOSM_MC_PLATFORM_H
#define MICROCOSM_MC_PLATFORM_H

#include "mc.h"
#include "mc_gl.h"   /* for GLuint */

typedef struct mc_window mc_window;

/* Both flags are plain switches, so a zeroed desc means no vsync and no
 * fullscreen. The headless backend reads only width and height. */
typedef struct {
    int         width;
    int         height;
    const char *title;
    int         vsync;       /* 1: swaps wait for the display. 0: they don't */
    int         fullscreen;  /* 0: a window. 1 opens at the screen's size with
                                the EWMH fullscreen hint and a blank pointer */
} mc_window_desc;

/* open / close */
/* Open a window (or headless surface) and make a GL 4.5 core context current,
 * then load the GL function table. Returns NULL on failure. */
mc_window *mc_window_open(const mc_window_desc *desc);
#ifndef MC_IMPORT_HASH   /* not in the payload, which just exits: call it inside MC_TIDY() */
void       mc_window_close(mc_window *w);
#endif

/* per-frame */
/* Process events; returns 0 when the app should quit (window closed or ESC). */
int    mc_window_pump(mc_window *w);
/* Present the output framebuffer (swap buffers; glFinish when headless). */
void   mc_window_swap(mc_window *w);
void   mc_window_size(mc_window *w, int *width, int *height);
/* The framebuffer the final image must be resolved into: 0 on the windowed
 * backend, a readable RGBA8 FBO on the headless backend. */
GLuint mc_window_output_fbo(mc_window *w);

/* clock */
double mc_time_seconds(void);   /* monotonic seconds since first call */

/* input (just what the demo needs) */
typedef enum {
    MC_KEY_ESCAPE = 0,
    MC_KEY_SPACE,
    MC_KEY_LEFT, MC_KEY_RIGHT, MC_KEY_UP, MC_KEY_DOWN,
    MC_KEY_COUNT
} mc_key;
int  mc_key_down(mc_window *w, mc_key key);

/* headless: get the pixels out */
/* Read the output framebuffer as tightly-packed top-left-origin RGBA8.
 * `rgba` must hold width*height*4 bytes. Returns 1 on success. Not in the
 * payload, which doesn't load the GL readback calls (see mc_gl.h). */
#ifndef MC_IMPORT_HASH
int  mc_window_read_pixels(mc_window *w, unsigned char *rgba);
#endif
/* Save the output framebuffer to a PNG file. Returns 1 on success. Only dev
 * builds implement it; elsewhere it does nothing and returns 0. */
int  mc_window_save_png(mc_window *w, const char *path);

#endif /* MICROCOSM_MC_PLATFORM_H */
