/*
 * gl_loader.c - fill in the GL function table from mc_gl.h.
 *
 * This file is why the binary imports no GL symbols: we ask the driver for
 * every entry point by name at runtime (via eglGetProcAddress) and keep the
 * pointers in mcgl. Nothing links against libGL, which is a large part of how
 * the binary fits in 64k.
 *
 * The table is a union of typed members and a plain array of generic function
 * pointers, both laid out by MC_GL_ALL. We fill the array and the call sites
 * read the typed members, which is fine in C, since each pointer is then
 * called through its matching PFN type.
 */
#include "microcosm/mc_gl.h"
#include "microcosm/mc.h"

mc_gl_api mcgl;
static GLuint fullscreen_vao, frame_ubo;

_Static_assert(sizeof mcgl == sizeof mcgl.slot, "mc_gl_api members must pack like the slot array");

GLuint mc_gl_fullscreen_vao(void) {
    if (!fullscreen_vao) glGenVertexArrays(1, &fullscreen_vao);
    return fullscreen_vao;
}

void mc_gl_frame_block(const void *data, GLsizeiptr bytes) {
    if (!frame_ubo) {
        glGenBuffers(1, &frame_ubo);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, frame_ubo);   /* also the generic binding */
    }
    glBufferData(GL_UNIFORM_BUFFER, bytes, data, GL_STREAM_DRAW);   /* orphan + refill */
}

#ifndef MC_IMPORT_HASH   /* the payload never closes its context: it exits (MC_TIDY) */
void mc_gl_context_release(void) {
    if (fullscreen_vao) glDeleteVertexArrays(1, &fullscreen_vao);
    if (frame_ubo) glDeleteBuffers(1, &frame_ubo);
    fullscreen_vao = frame_ubo = 0;
}
#endif

/* Every name without its "gl", NUL-separated, in table order. */
#define MC_GL_NAME_(name, type) #name "\0"
static const char names[] = MC_GL_ALL(MC_GL_NAME_);

int mc_gl_load(mc_gl_proc (*getproc)(const char *name)) {
    /* Rebuild each "glName" in place and look it up. We keep going on a miss,
     * so one run logs every missing entry point, not just the first. The copy
     * is a plain loop, so there are no libc string calls to import. */
    char name[32] = "gl";
    const char *s = names;
    int missing = 0;
    for (int i = 0; i < MC_GL_COUNT; ++i) {
        char *d = name + 2;
        while ((*d++ = *s++)) {}
        if (!(mcgl.slot[i] = getproc(name))) { mc_log("GL: missing %s", name); ++missing; }
    }
    return missing == 0;
}

/* Dev-only glGetError wrapper. Put MC_GL_CHECK() after a suspect call and it
 * names the offending enum and the function it fired in. Release builds leave
 * it out, since GL error checks aren't free. */
#if defined(MC_DEV)
void mc_gl_check_(const char *where) {
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        const char *name = "?";
        switch (e) {
        case GL_INVALID_ENUM:      name = "GL_INVALID_ENUM"; break;
        case GL_INVALID_VALUE:     name = "GL_INVALID_VALUE"; break;
        case GL_INVALID_OPERATION: name = "GL_INVALID_OPERATION"; break;
        case GL_OUT_OF_MEMORY:     name = "GL_OUT_OF_MEMORY"; break;
        case GL_INVALID_FRAMEBUFFER_OPERATION: name = "GL_INVALID_FRAMEBUFFER_OPERATION"; break;
        default: break;
        }
        mc_log("GL error 0x%04x (%s) at %s", (unsigned)e, name, where);
    }
}
#endif
