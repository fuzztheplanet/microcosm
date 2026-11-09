#include <alsa/asoundlib.h>
#include <time.h>
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
static snd_pcm_sframes_t sink_write(snd_pcm_t*,const void*,snd_pcm_uframes_t);
static int sink_delay(snd_pcm_t*,snd_pcm_sframes_t*);
static snd_pcm_state_t sink_state(snd_pcm_t*);
static int sink_prepare(snd_pcm_t*);
static int sink_wait(snd_pcm_t*,int);
static int sink_close(snd_pcm_t*);
static int fake_clock(clockid_t,struct timespec*);
static int fake_sleep(clockid_t,int,const struct timespec*,struct timespec*);
#define snd_pcm_writei sink_write
#define snd_pcm_delay sink_delay
#define snd_pcm_state sink_state
#define snd_pcm_prepare sink_prepare
#define snd_pcm_wait sink_wait
#define snd_pcm_close sink_close
#define clock_gettime fake_clock
#define clock_nanosleep fake_sleep
#include "../../src/audio.c"
static double now=100;
static int script[32], count, at, received, recoveries, closed, fail_delay, stop_wait;
static float samples[4096];
static mc_audio *owner;
static snd_pcm_sframes_t delay_frames;
static snd_pcm_state_t state=SND_PCM_STATE_RUNNING;
static int fake_clock(clockid_t id,struct timespec *ts) { (void)id; ts->tv_sec=(time_t)now; ts->tv_nsec=(long)((now-ts->tv_sec)*1e9); return 0; }
static int fake_sleep(clockid_t id,int flags,const struct timespec *ts,struct timespec *unused) {
    (void)id;(void)flags;(void)unused; now=ts->tv_sec+ts->tv_nsec*1e-9; return 0;
}
static snd_pcm_sframes_t sink_write(snd_pcm_t *pcm,const void *buf,snd_pcm_uframes_t frames) {
    (void)pcm; int n=at<count ? script[at++] : (int)frames;
    if(n>0) { if(n>(int)frames)n=frames; memcpy(samples+received*2,buf,n*2*sizeof(float)); received+=n; }
    return n;
}
static int sink_delay(snd_pcm_t *p,snd_pcm_sframes_t *d) { (void)p; *d=delay_frames; return fail_delay ? -EIO : 0; }
static snd_pcm_state_t sink_state(snd_pcm_t *p) { (void)p; return state; }
static int sink_prepare(snd_pcm_t *p) { (void)p; ++recoveries; return 0; }
static int sink_wait(snd_pcm_t *p,int timeout) { (void)p; assert(timeout<=10); if(stop_wait)atomic_store(&owner->running,0); return 0; }
static int sink_close(snd_pcm_t *p) { (void)p; ++closed; return 0; }
int main(void) {
    mc_audio a={0}; owner=&a; pthread_mutex_init(&a.lock,NULL); atomic_init(&a.running,1); a.pcm=(snd_pcm_t*)1;
    float input[2048]; for(int i=0;i<2048;++i)input[i]=(float)i;
    int steps[]={13,-EAGAIN,0,-EINTR,101,-EPIPE,7,-ESTRPIPE,44};
    memcpy(script,steps,sizeof steps); count=sizeof steps/sizeof *steps;
    audio_deliver(&a,input,1024);
    assert(received==1024 && a.submitted==1024 && recoveries==2);
    assert(!memcmp(input,samples,sizeof input));
    delay_frames=512; publish_device(&a); double t=mc_audio_time(&a);
    assert(fabs(t-512.0/MC_SAMPLE_RATE)<1e-7);
    now+=.005; assert(mc_audio_time(&a)>t);
    now+=1; assert(mc_audio_time(&a)==1024.0/MC_SAMPLE_RATE);
    a.last_time=0; state=SND_PCM_STATE_PREPARED; publish_device(&a); t=mc_audio_time(&a); now+=1;
    assert(mc_audio_time(&a)==t); fail_delay=1; publish_device(&a); assert(mc_audio_time(&a)==t);
    fail_delay=0; delay_frames=-10; publish_device(&a); assert(mc_audio_time(&a)<=a.ceiling);
    at=0; count=1; script[0]=-EAGAIN; stop_wait=1; received=0; audio_deliver(&a,input,1024); assert(received==0);
    stop_wait=0; atomic_store(&a.running,1); at=0; script[0]=-EIO;
    audio_deliver(&a,input,1024); assert(!a.pcm && closed==1 && a.submitted==2048);
    assert(mc_audio_time(&a)==2048.0/MC_SAMPLE_RATE);
    pthread_mutex_destroy(&a.lock);
    puts("Audio: partial/zero writes, EAGAIN/EINTR, xrun/suspend recovery, stop and bounded clock passed");
}
