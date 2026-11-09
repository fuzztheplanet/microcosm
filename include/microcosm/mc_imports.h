/*
 * mc_imports.h - the shipped payload imports nothing by name.
 *
 * A normally linked executable carries a symbol table: the names of every
 * libc/libm/X11/ALSA/EGL function it calls, their glibc/ALSA version tags and a
 * relocation for each. Packed, that's over a kilobyte, and the version tags tie
 * the binary to the build machine's glibc. The packed payload keeps only
 * DT_NEEDED, so the dynamic loader still maps the five libraries. Its own entry
 * point (src/mc_start.c) then finds every function below by the low
 * MC_IMPORT_HASH_BITS of a hash of its name. It walks the loader's list of
 * mapped libraries, scans each one's dynamic symbol table, takes the default
 * (non-hidden) version of each match and stores the address in mc_imp. Every
 * call then goes through that table.
 *
 * Only the payload build defines MC_IMPORT_HASH (the Makefile's PAYLOAD_DEFS).
 * Dev, headless and verification builds see an empty header and make ordinary
 * calls. Include it AFTER the system headers: each name below becomes a macro,
 * typed from the header's own declaration. tools/genimports.py hashes this list
 * for the resolver, so the list's order is the table's order.
 */
#ifndef MICROCOSM_MC_IMPORTS_H
#define MICROCOSM_MC_IMPORTS_H
#ifdef MC_IMPORT_HASH

/* The import hash width: how many low bits of each name's GNU hash the payload
 * keeps, in whole bytes. 24 means 3 bytes per import, which saves about a byte
 * of packed stream per import compared with 32. At 24, no import shares its
 * hash with any other name the build machine's libraries export
 * (tools/importcheck.py fails the link if one does). Another system's libraries
 * could still have a clash, at odds of a few in a hundred (see
 * docs/pipeline.org). 32 keeps the whole hash if you ever need to go back. */
#define MC_IMPORT_HASH_BITS 24

#define MC_IMPORTS(X) \
    X(malloc) X(calloc) X(getenv) X(strtol) \
    X(clock_gettime) X(clock_nanosleep) \
    X(pthread_create) X(pthread_mutex_init) X(pthread_mutex_lock) \
    X(pthread_mutex_unlock) \
    X(sinf) X(cosf) X(tanf) X(tanhf) X(expf) X(exp2f) X(powf) X(floorf) X(ceilf) \
    X(acosf) X(atan2f) \
    X(XOpenDisplay) X(XCreateColormap) X(XCreateWindow) \
    X(XFree) X(XGetVisualInfo) X(XInternAtom) X(XLookupKeysym) \
    X(XMapWindow) X(XNextEvent) X(XPending) X(XSetWMProtocols) X(XStoreName) X(XChangeProperty) \
    X(XCreateBitmapFromData) X(XCreatePixmapCursor) \
    X(eglGetDisplay) X(eglInitialize) X(eglBindAPI) X(eglChooseConfig) \
    X(eglGetConfigAttrib) X(eglCreateWindowSurface) X(eglCreateContext) \
    X(eglMakeCurrent) X(eglSwapInterval) X(eglSwapBuffers) X(eglGetProcAddress) \
    X(snd_pcm_open) X(snd_pcm_close) X(snd_pcm_prepare) \
    X(snd_pcm_wait) X(snd_pcm_writei) X(snd_pcm_delay) X(snd_pcm_state) \
    X(snd_pcm_set_params)

#define MC_IMPORT_SLOT(name) void *name;
extern struct mc_imports { MC_IMPORTS(MC_IMPORT_SLOT) } mc_imp;
#undef MC_IMPORT_SLOT

/* name -> the table's slot, cast back to the declared function type. */
#define MC_IMP(name) ((__typeof__(&name))mc_imp.name)
#define malloc          MC_IMP(malloc)
#define calloc          MC_IMP(calloc)
#define getenv          MC_IMP(getenv)
#define strtol          MC_IMP(strtol)
/* exit is not imported. The payload leaves through the exit_group system call
 * (mc_exit in src/mc_start.c) and runs no library's exit handlers. */
__attribute__((noreturn)) void mc_exit(int code);
#define exit            mc_exit
#define clock_gettime   MC_IMP(clock_gettime)
#define clock_nanosleep MC_IMP(clock_nanosleep)
#define pthread_create        MC_IMP(pthread_create)
#define pthread_mutex_init    MC_IMP(pthread_mutex_init)
#define pthread_mutex_lock    MC_IMP(pthread_mutex_lock)
#define pthread_mutex_unlock  MC_IMP(pthread_mutex_unlock)
#define sinf   MC_IMP(sinf)
#define cosf   MC_IMP(cosf)
#define tanf   MC_IMP(tanf)
#define tanhf  MC_IMP(tanhf)
#define expf   MC_IMP(expf)
#define exp2f  MC_IMP(exp2f)
#define powf   MC_IMP(powf)
#define floorf MC_IMP(floorf)
#define ceilf  MC_IMP(ceilf)
#define acosf  MC_IMP(acosf)
#define atan2f MC_IMP(atan2f)
#define XOpenDisplay    MC_IMP(XOpenDisplay)
#define XCreateColormap MC_IMP(XCreateColormap)
#define XCreateWindow   MC_IMP(XCreateWindow)
#define XFree           MC_IMP(XFree)
#define XGetVisualInfo  MC_IMP(XGetVisualInfo)
#define XInternAtom     MC_IMP(XInternAtom)
#define XLookupKeysym   MC_IMP(XLookupKeysym)
#define XMapWindow      MC_IMP(XMapWindow)
#define XNextEvent      MC_IMP(XNextEvent)
#define XPending        MC_IMP(XPending)
#define XSetWMProtocols MC_IMP(XSetWMProtocols)
#define XStoreName      MC_IMP(XStoreName)
#define XChangeProperty MC_IMP(XChangeProperty)
#define XCreateBitmapFromData MC_IMP(XCreateBitmapFromData)
#define XCreatePixmapCursor   MC_IMP(XCreatePixmapCursor)
#define eglGetDisplay          MC_IMP(eglGetDisplay)
#define eglInitialize          MC_IMP(eglInitialize)
#define eglBindAPI             MC_IMP(eglBindAPI)
#define eglChooseConfig        MC_IMP(eglChooseConfig)
#define eglGetConfigAttrib     MC_IMP(eglGetConfigAttrib)
#define eglCreateWindowSurface MC_IMP(eglCreateWindowSurface)
#define eglCreateContext       MC_IMP(eglCreateContext)
#define eglMakeCurrent         MC_IMP(eglMakeCurrent)
#define eglSwapInterval        MC_IMP(eglSwapInterval)
#define eglSwapBuffers         MC_IMP(eglSwapBuffers)
#define eglGetProcAddress      MC_IMP(eglGetProcAddress)
#define snd_pcm_open     MC_IMP(snd_pcm_open)
#define snd_pcm_close    MC_IMP(snd_pcm_close)
#define snd_pcm_prepare  MC_IMP(snd_pcm_prepare)
#define snd_pcm_wait     MC_IMP(snd_pcm_wait)
#define snd_pcm_writei   MC_IMP(snd_pcm_writei)
#define snd_pcm_delay    MC_IMP(snd_pcm_delay)
#define snd_pcm_state    MC_IMP(snd_pcm_state)
#define snd_pcm_set_params MC_IMP(snd_pcm_set_params)

#endif /* MC_IMPORT_HASH */
#endif /* MICROCOSM_MC_IMPORTS_H */
