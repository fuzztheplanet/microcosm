/*
 * mc_audio.h - a sample-free software synth and a real-time audio backend.
 *
 * The synth (src/synth.c) generates the soundtrack procedurally, with no sample
 * data, and fills an interleaved stereo float buffer on demand. The backend
 * (src/audio.c) runs a thread that pulls audio through a callback and plays it
 * through ALSA, or falls back to paced silence if ALSA can't be opened.
 *
 * The demo syncs its visuals to mc_audio_time(), which estimates the playback
 * position from the ALSA device delay and interpolates between updates, within
 * bounds. The silent fallback follows absolute monotonic deadlines and doesn't
 * launch any external command.
 */
#ifndef MICROCOSM_MC_AUDIO_H
#define MICROCOSM_MC_AUDIO_H

#include "mc.h"

#define MC_SAMPLE_RATE 44100
#define MC_CHANNELS    2

/* the synth: renders the song on demand */
typedef struct mc_synth mc_synth;

/* Create the synth. `seed` lets you vary incidental details deterministically;
 * the song structure itself is fixed. */
mc_synth *mc_synth_make(mc_u32 seed);
void      mc_synth_free(mc_synth *s);
/* Render `frames` interleaved stereo frames (2*frames floats) in [-1,1].
 * Advances the synth's internal time. Real-time safe (no allocation/IO). */
void      mc_synth_render(mc_synth *s, float *out, int frames);
double    mc_synth_bpm(const mc_synth *s);
double    mc_synth_length(const mc_synth *s);   /* song length in seconds */

/* the backend: a thread that feeds the sound card */
/* Pull callback: fill `out` with `frames` interleaved stereo frames. Called
 * from the audio thread; must be real-time safe. */
typedef void (*mc_audio_cb)(float *out, int frames, void *user);

typedef struct mc_audio mc_audio;

/* Start playback. Returns NULL on failure. Spawns the audio thread. */
mc_audio *mc_audio_start(mc_audio_cb cb, void *user);
#ifndef MC_IMPORT_HASH   /* not in the payload, which just exits: call it inside MC_TIDY() */
void      mc_audio_stop(mc_audio *a);
#endif
/* Monotonic estimate of the playback position, in seconds. With ALSA it's the
 * audio submitted minus the device delay, interpolated but never past what has
 * been submitted. The silent fallback is paced against an absolute monotonic
 * deadline. */
double    mc_audio_time(mc_audio *a);

#endif /* MICROCOSM_MC_AUDIO_H */
