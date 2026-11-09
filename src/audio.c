/* The audio thread holds on to each rendered block until the device has
 * accepted all of it. The clock it publishes is the frames submitted so far
 * minus the device delay, and it only interpolates up to what has been
 * submitted. ALSA is opened nonblocking, so shutdown and underrun handling
 * can't leave the worker stuck. Without an ALSA device, the thread sleeps to
 * absolute deadlines instead, so the clock still runs in real time. */
#include "microcosm/mc_audio.h"
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include "microcosm/mc_imports.h"   /* payload calls by hash; keep after system headers */

#define BLOCK 1024

struct mc_audio {
    mc_audio_cb cb;
    void *user;
    pthread_t thread;
    int thread_ok;
    atomic_int running;
    pthread_mutex_t lock;
    mc_u64 submitted;                 /* owned by the audio thread */
    double position, ceiling, stamp;  /* published under lock, in seconds */
    double last_time;
    int advancing;
    double silent_origin;
    snd_pcm_t *pcm;
};

static double audio_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void publish(mc_audio *a, double played, int advancing) {
    pthread_mutex_lock(&a->lock);
    a->position = played / MC_SAMPLE_RATE;
    a->ceiling = (double)a->submitted / MC_SAMPLE_RATE;
    a->stamp = audio_now();
    a->advancing = advancing;
    pthread_mutex_unlock(&a->lock);
}

static void publish_device(mc_audio *a) {
    snd_pcm_sframes_t delay = 0;
    /* A failed delay query is not evidence that the whole queue has played. */
    if (snd_pcm_delay(a->pcm, &delay) < 0) return;
    if (delay < 0) delay = 0;
    if ((mc_u64)delay > a->submitted) delay = (snd_pcm_sframes_t)a->submitted;
    publish(a, (double)(a->submitted - (mc_u64)delay),
            snd_pcm_state(a->pcm) == SND_PCM_STATE_RUNNING);
}

static snd_pcm_t *alsa_open(void) {
    snd_pcm_t *pcm = NULL;
    if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0)
        return NULL;
    /* Interleaved float stereo at the synth's rate, resampled if the device
     * runs at another rate. snd_pcm_set_params is one import where the
     * hw_params calls would be nine. The latency is 100 ms, a whole number of
     * frames (4410), with periods a quarter of that (1102 frames, about one
     * block). Through a resampling plug, snd_pcm_set_params fails if the
     * latency isn't a whole number of frames, and four blocks (92879 us)
     * isn't. It leaves the stream prepared, and playback starts once the
     * buffer is full. */
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_FLOAT_LE, SND_PCM_ACCESS_RW_INTERLEAVED, MC_CHANNELS,
                           MC_SAMPLE_RATE, 1, 100000) < 0) {
        snd_pcm_close(pcm);
        return NULL;
    }
    return pcm;
}

static void silent_write(mc_audio *a, int frames) {
    double previous = (double)a->submitted;
    a->submitted += (mc_u64)frames;
    if (a->silent_origin == 0.0)
        a->silent_origin = audio_now() - previous / MC_SAMPLE_RATE;
    publish(a, previous, 1);
    double deadline = a->silent_origin + (double)a->submitted / MC_SAMPLE_RATE;
    struct timespec ts = { (time_t)deadline, (long)((deadline - (time_t)deadline) * 1e9) };
    while (atomic_load_explicit(&a->running, memory_order_relaxed) &&
           clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
    publish(a, (double)a->submitted, 0);
}

/* Delivers one callback block, which may take several device writes. After a
 * recovery it carries on with the rest of the same block and never asks the
 * synth to render that tail again. The fake-sink regression test
 * (tools/checks/audio.c) calls it directly. */
static void audio_deliver(mc_audio *a, const float *buf, int frames) {
    int offset = 0;
    while (offset < frames && atomic_load_explicit(&a->running, memory_order_relaxed)) {
        if (a->pcm) {
            snd_pcm_sframes_t n = snd_pcm_writei(a->pcm, buf + offset * MC_CHANNELS,
                                                (snd_pcm_uframes_t)(frames - offset));
            if (n > 0) {
                offset += (int)n;
                a->submitted += (mc_u64)n;
                publish_device(a);
            } else if (n == 0 || n == -EAGAIN) {
                publish_device(a);
                snd_pcm_wait(a->pcm, 10); /* bounded, so the worker notices a stop */
            } else if (n == -EINTR) {
                continue;
            } else {
                /* snd_pcm_prepare is enough to recover from an xrun.
                 * snd_pcm_recover isn't used, because its suspend/resume
                 * retry loop may never end. */
                if ((n != -EPIPE && n != -ESTRPIPE) || snd_pcm_prepare(a->pcm) < 0) {
                    snd_pcm_close(a->pcm);
                    a->pcm = NULL;
                } else {
                    publish(a, (double)a->submitted, 0);
                }
            }
        } else {
            silent_write(a, frames - offset);
            offset = frames;
        }
    }
}

static void *audio_thread(void *arg) {
    mc_audio *a = arg;
    float buf[BLOCK * MC_CHANNELS];
    while (atomic_load_explicit(&a->running, memory_order_relaxed)) {
        a->cb(buf, BLOCK, a->user);
        audio_deliver(a, buf, BLOCK);
    }
    return NULL;
}

mc_audio *mc_audio_start(mc_audio_cb cb, void *user) {
    if (!cb) return NULL;
    mc_audio *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->cb = cb; a->user = user;
    atomic_init(&a->running, 1);
    if (pthread_mutex_init(&a->lock, NULL) != 0) { MC_TIDY(free(a)); return NULL; }
    a->pcm = alsa_open();
    publish(a, 0.0, 0);
    if (pthread_create(&a->thread, NULL, audio_thread, a) != 0) {
        MC_TIDY(if (a->pcm) snd_pcm_close(a->pcm); pthread_mutex_destroy(&a->lock); free(a));
        return NULL;
    }
    a->thread_ok = 1;
    return a;
}

#ifndef MC_IMPORT_HASH   /* the payload never stops its music; it exits (MC_TIDY) */
void mc_audio_stop(mc_audio *a) {
    if (!a) return;
    atomic_store_explicit(&a->running, 0, memory_order_relaxed);
    if (a->thread_ok) pthread_join(a->thread, NULL);
    if (a->pcm) { snd_pcm_drop(a->pcm); snd_pcm_close(a->pcm); }
    pthread_mutex_destroy(&a->lock);
    free(a);
}
#endif

double mc_audio_time(mc_audio *a) {
    if (!a) return 0.0;
    pthread_mutex_lock(&a->lock);
    double t = a->position + (a->advancing ? audio_now() - a->stamp : 0.0);
    if (t > a->ceiling) t = a->ceiling;
    if (t < a->last_time) t = a->last_time;
    a->last_time = t;
    pthread_mutex_unlock(&a->lock);
    return t;
}
