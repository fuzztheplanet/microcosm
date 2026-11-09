/*
 * MICROCOSM - a 64 KB demo engine for x86_64 Linux, OpenGL 4.5 core.
 *
 * This is the base header that the other mc_*.h headers include: the shared
 * scalar types, a few macros and the dev/release switch. It doesn't pull in
 * the rest of the API, so include the mc_*.h headers you need. Everything is
 * C11 and prefixed mc_, and nothing here allocates or touches global state on
 * its own.
 *
 * Two build flavours, picked by the Makefile:
 *   MC_DEV      asserts, logging, GL error checks and PNG dumps, for
 *               development.
 *   MC_RELEASE  silent: the logging and asserts compile to nothing, so no
 *               stdio or abort code ends up in the 64k binary.
 * If neither is defined we fall back to MC_DEV, so the headers still make sense
 * when you include them on their own.
 */
#ifndef MICROCOSM_MC_H
#define MICROCOSM_MC_H

#if !defined(MC_RELEASE) && !defined(MC_DEV)
#define MC_DEV 1
#endif

#include <stddef.h>
#include <stdint.h>

/* short integer names, used everywhere */
typedef uint8_t  mc_u8;
typedef uint32_t mc_u32;
typedef int32_t  mc_i32;
typedef uint64_t mc_u64;

/* handy constants */
#define MC_PI      3.14159265358979323846f
#define MC_TAU     6.28318530717958647692f
#define MC_DEG2RAD (MC_PI / 180.0f)
#define MC_RAD2DEG (180.0f / MC_PI)

/* the usual small macros (the min/max ones double-evaluate, so mind side effects) */
#define MC_ARRAY_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define MC_MIN(a, b)      ((a) < (b) ? (a) : (b))
#define MC_MAX(a, b)      ((a) > (b) ? (a) : (b))
#define MC_CLAMP(x, lo, hi) MC_MIN(MC_MAX((x), (lo)), (hi))
#define MC_UNUSED(x)      ((void)(x))

/*
 * Logging and asserts. In dev they go to stderr (and mc_assert aborts); in
 * release they expand to nothing, so they can't drag stdio or abort into the
 * binary. mc_fatal is the exception: it always exits non-zero, just quietly
 * in release.
 */
#if defined(MC_DEV)
#include <stdio.h>
#include <stdlib.h>
#define mc_log(...)  do { fprintf(stderr, "[mc] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define mc_assert(c) do { if (!(c)) { fprintf(stderr, "[mc] assert failed: %s (%s:%d)\n", \
                          #c, __FILE__, __LINE__); abort(); } } while (0)
#else
#define mc_log(...)  ((void)0)
#define mc_assert(c) ((void)0)
#endif

/* unrecoverable: say what happened (dev) and exit non-zero either way */
#if defined(MC_DEV)
#define mc_fatal(...) do { fprintf(stderr, "[mc] FATAL: " __VA_ARGS__); fputc('\n', stderr); \
                           exit(1); } while (0)
#else
#include <stdlib.h>
#define mc_fatal(...) do { exit(1); } while (0)
#endif

/* Cleanup that a failed start, or the end of the show, does before it returns
 * or exits. The shipped payload (MC_IMPORT_HASH) skips it, since the process
 * ends right after and the OS releases everything. Every other build frees
 * what it made: dev keeps the full cleanup, and the checks exercise it.
 * mc_window_close, mc_audio_stop, mc_mesh_gpu_free and mc_gl_context_release
 * aren't declared in the payload, so in code that goes into it, call them
 * only inside MC_TIDY(...). */
#ifdef MC_IMPORT_HASH
#define MC_TIDY(...) ((void)0)
#else
#define MC_TIDY(...) do { __VA_ARGS__; } while (0)
#endif

#endif /* MICROCOSM_MC_H */
