/*
 * synth.c - the softsynth behind "Hello World". It uses no samples: every
 * value it outputs is computed.
 *
 * It's built from the standard parts: polyBLEP saw oscillators (band-limited,
 * so they don't alias), ADSR envelopes, a Zavalishin state-variable filter
 * with a cutoff envelope per note, a tempo-synced ping-pong delay and a stereo
 * feedback-delay-network reverb. The finale adds a 7-saw supersaw and a
 * noise+body snare, and the master bus has a kick-driven sidechain pump and a
 * peak limiter.
 *
 * The song is a handful of small tables (the chord progressions and the
 * drum/bass/arp step patterns) that a sample-accurate sequencer reads as the
 * clock ticks. An arrangement curve on top fades instruments in and out and
 * sweeps the pad filter from section to section. It has four movements, all
 * at 124 BPM. Scene 1 has a bright A-major dance tune, and scene 2 a dark,
 * sparse D-minor descent followed by a triumphant C-major finale. Scene 3 gets
 * a warm D-major jungle piece with kalimba, hand percussion, cicadas, wind and
 * bird calls. A low dissonant pulse and drone warn of the tyrannosaurus, then lift into
 * A major for the blob's return on the beach, resolves and rings out. A coda
 * plays under the end card: scene 1's tune once more, with the blob's landings
 * plucked on top.
 *
 * mc_synth_render does no allocation and no I/O, so it's safe to call from the
 * audio thread; mc_synth_make allocates everything up front. The seed only
 * jitters incidental detail (start phases, detune) and never the structure,
 * and a given seed always renders the same song.
 */
#include "microcosm/mc_audio.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "microcosm/mc_imports.h"   /* payload calls by hash; keep after system headers */

/* Song layout, counted in bars and 16th-note steps. */
#define SONG_BPM      124.0
/* Scene 1 runs 26 bars (~50.3 s). The chord progression restarts with every
 * section, so each one opens on the tonic whatever its length. */
#define BARS_INTRO    4
#define BARS_BUILD    4
#define BARS_HOOKA    4
#define BARS_BREAK    4
#define BARS_HOOKB    7
#define BARS_OUTRO    3
/* The second movement scores scene 2's descent from orbit to the ocean floor.
 * It keeps the 124 BPM 16th grid but has darker chords (see PROG) and a sparse
 * arrangement soaked in reverb, so it sounds like a different piece rather
 * than more of the same tune. Its four parts follow the picture: space, the
 * dive, the splash and the deep. */
#define BARS_S2_SPACE  5
#define BARS_S2_DIVE   5
#define BARS_S2_SPLASH 2
#define BARS_S2_DEEP   6
#define BARS_SCENE2   (BARS_S2_SPACE + BARS_S2_DIVE + BARS_S2_SPLASH + BARS_S2_DEEP) /* 18 */
/* The third movement, the finale, scores the back half of Scene 2's underwater
 * journey, from the point where the murky water clears and the world turns
 * realistic. It's the climax, in bright C major, with a four-on-the-floor
 * groove under the sidechain pump and a fat supersaw lead over heavy pads. Its
 * three parts are a build, a long peak, and an outro that resolves and tails
 * off, ending on the burn-out to white. (The S3_/SCENE3 names mean this
 * movement, not the demo's scene 3.) */
#define BARS_S3_BUILD  7
#define BARS_S3_PEAK   26   /* the long finale: the chase, the blob's transformation, and the flight */
#define BARS_S3_OUTRO  3
#define BARS_SCENE3   (BARS_S3_BUILD + BARS_S3_PEAK + BARS_S3_OUTRO)   /* 36 */
/* The fourth movement is the jungle (the demo's scene 3), 59 bars long. Its
 * first bar is a shimmer under the white picture. The forest follows: a look
 * round, the canopy with birds overhead, the way back down, and the close-up on
 * a ladybug as it takes off. The camera then glides out to the beach. The music
 * darkens at the first sight of the sea; a sour drone and tightening pulse
 * follow the tyrannosaurus through a hushed forest until it has left the picture. After a short relief the
 * blob returns in A major, waves and plays in the swash to the groove, and the
 * movement closes on a IV V7 I cadence and rings out. */
#define BARS_WHITE    1
#define BARS_JUNGLE   (BARS_WHITE + 58)
/* The coda plays under the end card: five bars of scene 1's dance tune (I V vi
 * IV, then home to I), with a kalimba note wherever the blob lands (main.c's
 * END_LAND). */
#define BARS_END      5
#define TOTAL_BARS    (BARS_INTRO + BARS_BUILD + BARS_HOOKA + BARS_BREAK + BARS_HOOKB + BARS_OUTRO + \
                       BARS_SCENE2 + BARS_SCENE3 + BARS_JUNGLE + BARS_END)  /* 144 */
#define STEPS_PER_BAR 16
#define TOTAL_STEPS   (TOTAL_BARS * STEPS_PER_BAR)

/* Section boundaries, in bars. */
#define BAR_BUILD  (BARS_INTRO)
#define BAR_HOOKA  (BAR_BUILD + BARS_BUILD)
#define BAR_BREAK  (BAR_HOOKA + BARS_HOOKA)
#define BAR_HOOKB  (BAR_BREAK + BARS_BREAK)
#define BAR_OUTRO  (BAR_HOOKB + BARS_HOOKB)
#define BAR_SCENE2    (BAR_OUTRO + BARS_OUTRO)          /* 26: scene 1 ends, scene 2 begins */
#define BAR_S2_DIVE   (BAR_SCENE2 + BARS_S2_SPACE)      /* 31 */
#define BAR_S2_SPLASH (BAR_S2_DIVE + BARS_S2_DIVE)      /* 36 */
#define BAR_S2_DEEP   (BAR_S2_SPLASH + BARS_S2_SPLASH)  /* 38 */
#define BAR_SCENE3    (BAR_SCENE2 + BARS_SCENE2)        /* 44: third movement begins (Scene 2's finale) */
#define BAR_S3_PEAK   (BAR_SCENE3 + BARS_S3_BUILD)      /* 51 */
#define BAR_S3_OUTRO  (BAR_S3_PEAK + BARS_S3_PEAK)      /* 77 */
#define BAR_STALK     (BAR_S3_PEAK + 9)                 /* 60: the predator's stalk, lunge and near miss, */
                                                        /* four bars inside the peak (see arrange) */
#define BAR_JUNGLE    (BAR_S3_OUTRO + BARS_S3_OUTRO)    /* 80: the white, then the jungle */
#define BAR_WOODS     (BAR_JUNGLE + BARS_WHITE)         /* 81: the forest, scene 3's S3_J0. Its cues */
                                                        /* count bars from here, as below */
#define BAR_END       (BAR_JUNGLE + BARS_JUNGLE)        /* 139: the end card */

#define SRF        ((float)MC_SAMPLE_RATE)
#define DLEN       (1 << 16)    /* delay ring size, power of two (~1.49 s) */
#define REST       127         /* pattern sentinel: no note on this step */

/* The chord progressions, one chord per bar, wrapping every 4 bars and
 * restarting at each section (see on_step). Each entry is a root note (MIDI,
 * octave 3) and the triad's semitone offsets. Scene 1 has the I-V-vi-IV "four
 * chords of pop" in A major. Scene 2's descent is a darker D minor
 * i-VI-III-VII, and its finale resolves with I-V-vi-IV in C major. The jungle
 * plays open, rootless sevenths over its bass (Dmaj7, Bm7, Gmaj7, A add9). The
 * tyrannosaurus gets one low, sour cluster (D Eb Ab), and the jungle's ending
 * comes home to A by way of D and E7. */
typedef struct { signed char root; signed char n[3]; } chord_def;
static const chord_def PROG[6][4] = {
    { { 57, { 0, 4, 7 } }, { 52, { 0, 4, 7 } }, { 54, { 0, 3, 7 } }, { 50, { 0, 4, 7 } } },  /* A  E  F#m D  */
    { { 50, { 0, 3, 7 } }, { 46, { 0, 4, 7 } }, { 53, { 0, 4, 7 } }, { 48, { 0, 4, 7 } } },  /* Dm Bb F   C  */
    { { 48, { 0, 4, 7 } }, { 55, { 0, 4, 7 } }, { 57, { 0, 3, 7 } }, { 53, { 0, 4, 7 } } },  /* C  G  Am  F  */
    { { 50, { 4, 7, 11 } }, { 47, { 3, 7, 10 } }, { 43, { 4, 7, 11 } }, { 45, { 7, 14, 16 } } }, /* D  Bm  G  A */
    { { 38, { 0, 1, 6 } } },                                                                    /* the T-rex */
    { { 50, { 0, 4, 7 } }, { 52, { 0, 4, 10 } }, { 57, { 0, 4, 7 } }, { 57, { 0, 4, 7 } } }, /* D  E7 A   A  */
};

/* One-bar, 16-step patterns per movement: kick, hat velocity (0..7), open hat,
 * snare, bass (semitones over the root, REST = none) and the arpeggio (indices
 * into the chord's extended tone set, see chord_ext).
 *   1: the dance groove: four on the floor, offbeat 16th hats, a bouncy bass.
 *   2: thinned right out: a half-time heartbeat kick, no hats, held roots and
 *      a slow bell line with big gaps for the reverb.
 *   3: the finale: a backbeat snare, driving hats, pumping eighths and a rising
 *      line for the supersaw.
 *   4: the jungle: a soft frame drum, a shaker on the 16ths, claves in a 3-2
 *      son clave (the snare row), a deep bass and the kalimba's syncopated line. */
enum { PT_KICK, PT_HAT, PT_OPEN, PT_SNARE, PT_BASS, PT_ARP };
static const unsigned char PAT[4][6][16] = {
    { { 1,0,0,0, 1,0,0,0, 1,0,0,0, 1,0,0,0 },
      { 2,3,7,3, 2,3,7,3, 2,3,7,3, 2,3,7,4 },
      { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,1,0 },
      { 0 },
      { 0,REST,0,12, 0,REST,12,0, 0,REST,0,12, 0,REST,12,0 },
      { 0,1,2,3, 4,3,2,1, 0,1,2,3, 4,5,3,1 } },
    { { 1,0,0,0, 0,0,0,0, 1,0,0,0, 0,0,0,0 },
      { 0 },
      { 0 },
      { 0 },
      { 0,REST,REST,REST, REST,REST,REST,REST, 0,REST,REST,REST, REST,REST,REST,REST },
      { 0,REST,REST,REST, 2,REST,REST,REST, 4,REST,REST,REST, 3,REST,REST,REST } },
    { { 1,0,0,0, 1,0,0,0, 1,0,0,0, 1,0,0,0 },
      { 3,4,6,4, 3,4,6,4, 3,4,6,4, 3,5,7,5 },
      { 0,0,0,0, 0,0,1,0, 0,0,0,0, 0,0,1,0 },
      { 0,0,0,0, 1,0,0,0, 0,0,0,0, 1,0,0,0 },
      { 0,REST,0,REST, 0,REST,0,REST, 0,REST,0,REST, 0,REST,12,REST },
      { 0,2,4,5, 4,2,0,2, 4,5,4,2, 0,2,4,5 } },
    { { 1,0,0,0, 0,0,0,0, 0,0,1,0, 0,0,0,0 },
      { 4,2,3,2, 5,2,3,2, 4,2,3,2, 5,2,3,3 },
      { 0 },
      { 1,0,0,1, 0,0,1,0, 0,0,1,0, 1,0,0,0 },
      { 12,REST,REST,REST, REST,REST,REST,REST, REST,REST,12,REST, REST,REST,REST,REST },
      { 0,REST,2,REST, 4,REST,3,1, REST,2,REST,4, 5,REST,3,REST } },
};

/* Note envelopes per movement (attack, decay, sustain, release, in seconds).
 * ENV_LEAD is the lead and its octave layer: a pluck in scene 1, a bell in
 * scene 2's descent and the sustained supersaw in the finale. The bass rings
 * longer in the descent, and longest in the jungle. */
static const float ENV_BFILT[4]   = { 0.001f, 0.09f, 0.0f, 0.05f };   /* the bass's filter sweep */
static const float ENV_BASS[4][4] = { { 0.003f, 0.14f, 0.0f, 0.05f }, { 0.003f, 0.30f, 0.0f, 0.14f }, { 0.003f, 0.14f, 0.0f, 0.05f },
                                     { 0.02f, 0.9f, 0.0f, 0.3f } };
static const float ENV_LEAD[3][2][4] = {
    { { 0.002f, 0.12f, 0.0f, 0.08f }, { 0.002f, 0.14f, 0.0f, 0.08f } },
    { { 0.010f, 0.60f, 0.0f, 0.45f }, { 0.010f, 0.70f, 0.0f, 0.45f } },
    { { 0.008f, 0.45f, 0.55f, 0.30f }, { 0 } },
};

/* A private xorshift, so the synth can be linked without math.c. */
typedef struct { mc_u32 s; } rng_t;
static mc_u32 rng_u32(rng_t *r) {
    mc_u32 x = r->s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    r->s = x;
    return x;
}
static float rng_f(rng_t *r)  { return (float)(rng_u32(r) >> 8) * (1.0f / 16777216.0f); }
static float rng_bi(rng_t *r) { return rng_f(r) * 2.0f - 1.0f; }   /* [-1,1) noise */

/* Scalar odds and ends. midi_hz is the equal-temperament note-to-frequency. */
static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
static float sstep(float x)   { x = clamp01(x); return x * x * (3.0f - 2.0f * x); }
static float midi_hz(float m) { return 440.0f * exp2f((m - 69.0f) * (1.0f / 12.0f)); }
static float chord_ext(const chord_def *c, int idx) {
    /* idx 0..5 walks the triad across two octaves: n0 n1 n2 n0+12 n1+12 n2+12 */
    int oct = (idx >= 3) ? 12 : 0;
    return (float)(c->n[idx % 3] + oct);
}

/* polyBLEP sawtooth. poly_blep is a small correction polynomial spread over
 * the discontinuity, which removes most of the aliasing. The phase is in [0,1)
 * and advances by dt = freq/samplerate each sample. */
static float poly_blep(float t, float dt) {
    if (t < dt)            { t /= dt;             return t + t - t * t - 1.0f; }
    else if (t > 1.0f - dt){ t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}
static float osc_saw(float *phase, float dt) {
    float t = *phase;
    float v = 2.0f * t - 1.0f - poly_blep(t, dt);
    t += dt;
    if (t >= 1.0f) t -= 1.0f;
    *phase = t;
    return v;
}

/* Zavalishin's topology-preserving state-variable filter. One svf_run computes
 * the lowpass, bandpass and highpass outputs together. */
typedef struct { float ic1, ic2, lp, bp, hp; } svf;
static void svf_run(svf *f, float in, float cut_hz, float q) {
    float fc = cut_hz * (1.0f / SRF);
    if (fc > 0.49f) fc = 0.49f;
    if (fc < 1.0e-5f) fc = 1.0e-5f;
    float g  = tanf(MC_PI * fc);
    float k  = 1.0f / q;
    float a1 = 1.0f / (1.0f + g * (g + k));
    float a2 = g * a1;
    float a3 = g * a2;
    float v3 = in - f->ic2;
    float v1 = a1 * f->ic1 + a2 * v3;
    float v2 = f->ic2 + a2 * f->ic1 + a3 * v3;
    f->ic1 = 2.0f * v1 - f->ic1;
    f->ic2 = 2.0f * v2 - f->ic2;
    f->lp = v2;
    f->bp = v1;
    f->hp = in - k * v1 - v2;
}

/* A plain ADSR. stage: 0 idle, 1 attack, 2 decay/sustain, 3 release. The decay
 * and release are one-pole exponentials, which is why they get a coefficient
 * rather than a linear rate. */
typedef struct { int stage; float level, a_inc, d_coef, s, r_coef; } env;
static float decay_coef(float seconds) {
    if (seconds < 1.0e-4f) seconds = 1.0e-4f;
    return expf(-1.0f / (seconds * SRF));
}
static void env_trig(env *e, const float *adsr) {   /* attack, decay, sustain, release */
    float a = adsr[0];
    e->a_inc  = (a > 1.0e-5f) ? 1.0f / (a * SRF) : 1.0f;
    e->d_coef = decay_coef(adsr[1]);
    e->s      = adsr[2];
    e->r_coef = decay_coef(adsr[3]);
    e->level  = 0.0f;
    e->stage  = 1;
}
static float env_run(env *e) {
    switch (e->stage) {
    case 1: e->level += e->a_inc; if (e->level >= 1.0f) { e->level = 1.0f; e->stage = 2; } break;
    case 2: e->level = e->s + (e->level - e->s) * e->d_coef;
            if (e->s == 0.0f && e->level < 1.0e-6f) { e->level = 0.0f; e->stage = 0; } break;
    case 3: e->level *= e->r_coef; if (e->level < 1.0e-5f) { e->level = 0.0f; e->stage = 0; } break;
    default: e->level = 0.0f; break;
    }
    return e->level;
}

/* The instruments, each holding only the state its synthesis needs. The render
 * loop runs each one by name, with no dispatch table. */
typedef struct {                       /* kick: sine + pitch drop + click */
    int   active;
    float phase, amp, click, pitch;
    float amp_c, click_c, pitch_c;
} kick_voice;

typedef struct {                       /* hihat: highpassed noise, short decay */
    int   active;
    float amp, amp_c, pan;
    svf   hp;
} hat_voice;

typedef struct {                       /* bass: saw -> enveloped lowpass */
    float phase, freq;
    env   amp, fenv;
    svf   filt;
} bass_voice;

typedef struct {                       /* lead: 3 detuned saws -> lowpass pluck */
    float phase[3], freq;
    env   amp;
    svf   filt;
} lead_voice;
/* The lead and its octave layer: their unison detune and filter cutoff. */
static const float LEAD_DET[2][3] = { { 1.0f, 1.006f, 0.994f }, { 1.0f, 1.007f, 0.993f } };
static const float LEAD_CUT[2]    = { 4200.0f, 5200.0f };

typedef struct {                       /* pad: triad x 2 detuned saws, stereo */
    float phase[3][2], freq[3];
    svf   filt[2];
} pad_voice;

typedef struct {                       /* supersaw lead: 7 detuned saws, stereo spread */
    float phase[7], freq;
    env   amp;
    svf   filt[2];
} super_voice;

typedef struct {                       /* snare: noise burst + a short tonal body */
    int   active;
    float amp, amp_c, tone, tone_c, tphase;
    svf   bp;
} snare_voice;

typedef struct {                       /* kalimba tine: two damped resonators (the note and */
    float re[2], im[2], c[2], sn[2], d[2], pan;   /* its fast-fading 6th partial) as rotating */
} kal_voice;                                      /* phasors, so there's no sinf per sample */

typedef struct {                       /* bird call: a gliding FM sine, a soft attack-decay, a trill */
    float phase, freq, sweep, a, a_c, b, b_c, fm, trill, trill_dt, depth, pan;
} bird_voice;

/* The jungle's kalimba, per section (white, look round, canopy, descent, ladybug,
 * take-off, resolve, glide, hush, tyrannosaurus, relief, blob, wave, play, end):
 * which 16ths of the arp row it plays, its octave over the chord, and how long
 * it rings (tenths of a second). */
static const struct { unsigned short mask; unsigned char oct, ring; } KAL4[15] = {
    { 0x1111, 12, 24 }, { 0x5555, 0, 12 }, { 0xFFFF, 0, 10 }, { 0x5555, 0, 14 },
    { 0xFFFF, 12, 16 }, { 0xFFFF, 12, 12 }, { 0x0101, 12, 30 }, { 0x5555, 0, 10 },
    { 0 },              { 0 },              { 0x1010, 12, 24 }, { 0xFFFF, 0, 12 },
    { 0x001F, 12, 30 }, { 0x5555, 12, 10 }, { 0x1111, 12, 26 },
};
/* ... and, from the take-off on, its harmony: the PROG row (high nibble), the
 * chord it starts on (low bits), and whether it holds that chord (4) instead of
 * moving on. In the blob's sections (the jungle's last four) the kalimba also
 * plays scene 1's arpeggio, the hook. The last entry is for the coda, which
 * uses scene 1's own progression. */
static const unsigned char JHARM[11] = { 0x37, 0x34, 0x30, 0x44, 0x44, 0x37, 0x00, 0x04, 0x00, 0x50, 0x00 };

/* Supersaw detune spread and per-oscillator stereo pan (JP-8000-ish). */
static const float SUPER_DET[7] = { -0.019f, -0.011f, -0.0045f, 0.0f, 0.0045f, 0.011f, 0.019f };
static const float SUPER_PAN[7] = { -1.0f, -0.6f, -0.3f, 0.0f, 0.3f, 0.6f, 1.0f };

/* Time-domain effects: a stereo ping-pong delay and a reverb. Their buffers are
 * part of the synth struct (see mc_synth_make).
 *
 * The reverb is a feedback delay network of eight delay lines, 32 to 61 ms long
 * (prime lengths). Each line is read at its end through a one-pole lowpass,
 * standing in for the air's damping so the highs die first, and a gain. An 8x8
 * Hadamard matrix (three butterfly passes, scaled by 1/sqrt 8) mixes the eight
 * outputs, and they're written back with the send added. The matrix is
 * orthogonal, so the mixing loses no energy, and since every line feeds every
 * other the echoes multiply eightfold on each trip. Each line's gain is
 * 10^(-3 len / (rate rt)), so all eight fall 60 dB in the same rt seconds
 * whatever their length. Each length also wanders by +-6 samples on a slow sine
 * of its own (a fraction of a cent of pitch) and is read with linear
 * interpolation, which keeps the tail from ringing metallic. The even lines sum
 * to the left output and the odd ones to the right, giving two different mixes
 * of one tail: decorrelated stereo from a mono send. The shortest line (32 ms)
 * doubles as the pre-delay. Before the network, the send loses its lows below
 * about 150 Hz so the tail doesn't boom, and two allpasses smear its attacks.
 * Each movement sets the reverb its own way (RVV). It's big for scene 1 and the
 * end card, long and dark in space and in the deep, a little shorter for the
 * finale's groove, and short and airy in the jungle. */
#define RV_LEN 4096             /* each line's ring, power of two (~93 ms) */
typedef struct { float l[DLEN], r[DLEN]; int len, wi; } delay_fx;   /* stereo ping-pong */
typedef struct { float buf[556]; int len, i; } allpass_fx;
typedef struct {
    float lp[8], g[8], at[8];   /* each line's damping state, gain and (wandering) length */
    float damp, hp;             /* the damping's one-pole coefficient; the send's lowpass */
    int   w;
    allpass_fx ap[2];
    float line[8][RV_LEN];      /* the delay lines, one write position for all */
} reverb_fx;
static const short RV_TAP[8] = { 1427, 1559, 1747, 1907, 2063, 2251, 2459, 2707 };
/* Per movement (numbered as in on_step): rt in tenths of a second, and the
 * damping 1 - e^(-2 pi fc / rate) in hundredths, for fc of 8, 2.2, 3 and 11 kHz. */
static const unsigned char RVV[4][2] = { { 26, 68 }, { 45, 27 }, { 30, 35 }, { 14, 80 } };

static void delay_run(delay_fx *d, float in, float fb, float *outL, float *outR) {
    int ri = (d->wi - d->len) & (DLEN - 1);
    float rl = d->l[ri];
    float rr = d->r[ri];
    d->l[d->wi] = in + rr * fb;        /* dry goes left; the echo taps cross over for the bounce */
    d->r[d->wi] = rl * fb;
    d->wi = (d->wi + 1) & (DLEN - 1);
    *outL = rl;
    *outR = rr;
}
static float allpass_run(allpass_fx *a, float in, float fb) {
    float bufout = a->buf[a->i];
    float out = -in + bufout;
    a->buf[a->i] = in + bufout * fb;
    if (++a->i >= a->len) a->i = 0;
    return out;
}
/* One sample of the reverb: the diffused send in, the left and right tails out. */
static void reverb_run(reverb_fx *rv, float in, float *out) {
    float x[8];
    in -= rv->hp += (in - rv->hp) * 0.021f;
    in = allpass_run(&rv->ap[1], allpass_run(&rv->ap[0], in, 0.5f), 0.5f);
    for (int i = 0; i < 8; ++i) {
        float p = (float)(rv->w + RV_LEN) - rv->at[i], *b = rv->line[i];
        int   j = (int)p;
        float y = b[j & (RV_LEN - 1)];
        y += (b[(j + 1) & (RV_LEN - 1)] - y) * (p - (float)j);
        out[i & 1] += y;
        rv->lp[i] += (y - rv->lp[i]) * rv->damp;
        x[i] = rv->lp[i] * rv->g[i];
    }
    for (int h = 1; h < 8; h += h)
        for (int i = 0; i < 8; ++i)
            if (!(i & h)) { float a = x[i], c = x[i + h]; x[i] = a + c; x[i + h] = a - c; }
    for (int i = 0; i < 8; ++i) rv->line[i][rv->w] = x[i] * 0.35355339f + in;
    rv->w = (rv->w + 1) & (RV_LEN - 1);
}

/* The arrangement. Each of the song's 29 sections (start bars below) sets a
 * target level per instrument, the pad's filter cutoff, the reverb send and the
 * sidechain amount. Most of these ramp across the section: a parameter runs
 * from v0 by d while the section's progress t sweeps a .. a+w (clamped outside
 * that range), either linearly or along a smoothstep. The intro opening up and
 * the outros closing down are ramps like this. The render loop smooths toward
 * these targets, so the steps between sections don't click. The pad lies under
 * everything and sets each section's loudness floor. The quiet sections (the
 * intro, space, the deep and the tyrannosaurus) hold it at 0.23 to 0.55,
 * against 0.65 to 1.0 elsewhere. That way the loudness follows the story, and
 * the sections span about 9 LU rather than sitting within 5. */
enum { P_KICK, P_HAT, P_BASS, P_LEAD, P_LEAD2, P_PAD, P_CUT, P_RISER, P_SPACE,
       P_SUPER, P_SNARE, P_SUB, P_PUMP, P_KAL, P_BIRD, P_AMB, P_WIND, P_SURF, P_COUNT };
enum { SEC_COUNT = 29 };
static const unsigned char SEC_BAR[SEC_COUNT + 1] = {
    0, BAR_BUILD, BAR_HOOKA, BAR_BREAK, BAR_HOOKB, BAR_OUTRO,       /* scene 1     */
    BAR_SCENE2, BAR_S2_DIVE, BAR_S2_SPLASH, BAR_S2_DEEP,            /* the descent */
    BAR_SCENE3, BAR_S3_PEAK, BAR_S3_OUTRO,                          /* the finale  */
    BAR_JUNGLE, BAR_WOODS, BAR_WOODS + 8, BAR_WOODS + 13,           /* white, look round, canopy, descent, */
    BAR_WOODS + 16, BAR_WOODS + 24, BAR_WOODS + 25,                 /* ladybug, take-off, resolve, */
    BAR_WOODS + 26, BAR_WOODS + 30, BAR_WOODS + 31,                 /* glide, the beach's hush, the T-rex, */
    BAR_WOODS + 39, BAR_WOODS + 41, BAR_WOODS + 46,                 /* relief, the blob, its wave, */
    BAR_WOODS + 48, BAR_WOODS + 54, BAR_END, TOTAL_BARS             /* its play in the swash, the end, */
};                                                                  /* the end card */
/* The arrangement, one ramp per entry: section, parameter (| SM for a
 * smoothstep), a and w in hundredths of the section, and v0 and d in
 * thousandths (in Hz for the pad cutoff; both are stored coarser, see RAMP_Q_).
 * LEVEL is a constant level for the whole section. The list is expanded once
 * per field into RAMP_SEC, RAMP_PAR and so on, one array per field, because the
 * packer compresses those far better than interleaved records. */
#define SM 0x80
#define LEVEL(F, sec, par, v) F(sec, par, 0, 100, v, 0)
#define RAMPS(F) \
    /* INTRO: sparse. The filter opens and the hats fade in, then the lead. The                                            \
     * pad rises from 0.26 to 0.48 and holds there, leaving room for the bang */                                           \
    F(0, P_PAD|SM, 0, 40, 260, 220) F(0, P_CUT|SM, 0, 100, 260, 2340) F(0, P_HAT|SM, 40, 60, 0, 450)                       \
    F(0, P_LEAD|SM, 60, 40, 0, 350) LEVEL(F, 0, P_SPACE, 500)                                                              \
    F(0, P_RISER|SM, 86, 14, 0, 800)   /* a swell into the bang, cut on its kick */                                        \
    /* BUILD: the kick and bass come in, the pump eases in, and a riser leads into the hook */                             \
    F(1, P_KICK, 0, 100, 900, 100) F(1, P_BASS, 0, 100, 550, 450) F(1, P_HAT, 0, 100, 600, 400)                            \
    F(1, P_LEAD, 0, 100, 550, 350) LEVEL(F, 1, P_PAD, 750) LEVEL(F, 1, P_CUT, 2900) LEVEL(F, 1, P_SPACE, 300)              \
    F(1, P_PUMP, 0, 100, 0, 450) F(1, P_RISER|SM, 75, 25, 0, 1000)                                                         \
    /* HOOK A: the full groove */                                                                                          \
    LEVEL(F, 2, P_KICK, 1000) LEVEL(F, 2, P_BASS, 1000) LEVEL(F, 2, P_HAT, 1000) LEVEL(F, 2, P_LEAD, 1000)                 \
    LEVEL(F, 2, P_PAD, 780) LEVEL(F, 2, P_CUT, 3200) LEVEL(F, 2, P_SPACE, 200) LEVEL(F, 2, P_PUMP, 600)                    \
    /* BREAKDOWN: a breather while the filter sweeps slowly back up */                                                     \
    LEVEL(F, 3, P_HAT, 250) LEVEL(F, 3, P_LEAD, 800) LEVEL(F, 3, P_PAD, 700) F(3, P_CUT|SM, 0, 100, 500, 2500)             \
    LEVEL(F, 3, P_SPACE, 600) LEVEL(F, 3, P_PUMP, 200) F(3, P_RISER|SM, 70, 30, 0, 1000)                                   \
    /* HOOK B: the bigger reprise, with the octave lead */                                                                 \
    LEVEL(F, 4, P_KICK, 1000) LEVEL(F, 4, P_BASS, 1000) LEVEL(F, 4, P_HAT, 1000) LEVEL(F, 4, P_LEAD, 1000)                 \
    LEVEL(F, 4, P_LEAD2, 600) LEVEL(F, 4, P_PAD, 880) LEVEL(F, 4, P_CUT, 3400) LEVEL(F, 4, P_SPACE, 260)                   \
    LEVEL(F, 4, P_PUMP, 650)                                                                                               \
    /* OUTRO: collapse back toward the intro */                                                                            \
    F(5, P_KICK|SM, 0, 50, 1000, -1000) F(5, P_BASS|SM, 25, 50, 1000, -1000) F(5, P_HAT|SM, 0, 100, 1000, -1000)           \
    F(5, P_LEAD|SM, 10, 60, 1000, -1000) LEVEL(F, 5, P_PAD, 850) F(5, P_CUT|SM, 0, 100, 2600, -2320)                       \
    LEVEL(F, 5, P_SPACE, 500) F(5, P_PUMP, 0, 100, 600, -600)                                                              \
    /* SCENE 2 / SPACE: dark and drowned in reverb. A slow swell, with the pad held low */                                 \
    LEVEL(F, 6, P_PAD, 500) F(6, P_CUT|SM, 0, 100, 500, 1200) LEVEL(F, 6, P_LEAD, 450) LEVEL(F, 6, P_SPACE, 1000)          \
    /* SCENE 2 / DIVE: tension. The pad and the heartbeat rise toward the hit */                                           \
    F(7, P_PAD|SM, 0, 100, 500, 380) F(7, P_CUT|SM, 0, 100, 1700, 1700) LEVEL(F, 7, P_LEAD, 550)                           \
    F(7, P_BASS, 0, 100, 300, 550) F(7, P_KICK|SM, 0, 100, 0, 850) F(7, P_SPACE, 0, 100, 900, -450)                        \
    F(7, P_RISER|SM, 55, 45, 0, 1000)                                                                                      \
    /* SCENE 2 / SPLASH: the hit, then the kick and bass fall away and the filter closes down */                           \
    F(8, P_KICK|SM, 0, 50, 1000, -1000) F(8, P_BASS|SM, 0, 50, 600, -600) F(8, P_LEAD, 0, 100, 550, -250)                  \
    LEVEL(F, 8, P_PAD, 900) F(8, P_CUT|SM, 0, 100, 3200, -2700) F(8, P_SPACE, 0, 100, 450, 550)                            \
    F(8, P_RISER, 0, 18, 1000, -1000)                                                                                      \
    /* SCENE 2 / DEEP: muffled and calm, the pad low again, and the bell fades out */                                      \
    F(9, P_PAD|SM, 60, 40, 500, -270) F(9, P_CUT|SM, 0, 100, 650, 300) F(9, P_LEAD|SM, 70, 30, 420, -420)                  \
    LEVEL(F, 9, P_BASS, 220) LEVEL(F, 9, P_SPACE, 1000) F(9, P_RISER|SM, 82, 18, 0, 600)   /* a swell as it clears */      \
    /* FINALE / BUILD: the kick comes in, then the hats and the snare, over a rising sub and a riser */                    \
    LEVEL(F, 10, P_PAD, 850) F(10, P_CUT|SM, 0, 100, 1200, 2400) F(10, P_SUPER, 0, 100, 400, 550)                          \
    F(10, P_BASS, 0, 100, 500, 500) F(10, P_KICK|SM, 10, 40, 0, 1000) F(10, P_HAT|SM, 30, 50, 0, 900)                      \
    F(10, P_SNARE|SM, 50, 50, 0, 1000) F(10, P_SUB, 0, 100, 0, 400) LEVEL(F, 10, P_PUMP, 800)                              \
    LEVEL(F, 10, P_SPACE, 300) F(10, P_RISER|SM, 70, 30, 0, 1000)                                                          \
    /* FINALE / PEAK: everything in. Over the back half, through the chase and the                                         \
     * flight, the filter opens on the pad and the supersaw with a riser under them.                                       \
     * From bar 71, 2 s before the blob takes off, the supersaw itself swells from                                         \
     * 1.0 to 1.4 by the outro's downbeat, so the music lifts with the flight */                                           \
    LEVEL(F, 11, P_KICK, 1000) LEVEL(F, 11, P_BASS, 1000) LEVEL(F, 11, P_HAT, 1000) LEVEL(F, 11, P_SNARE, 1000)            \
    F(11, P_SUPER, 77, 23, 1000, 400) LEVEL(F, 11, P_PAD, 900) F(11, P_CUT, 50, 50, 4200, 1800) LEVEL(F, 11, P_SUB, 700)   \
    LEVEL(F, 11, P_PUMP, 850) LEVEL(F, 11, P_SPACE, 220) F(11, P_RISER|SM, 50, 50, 0, 700)                                 \
    /* FINALE / OUTRO: resolve and tail off, with the reverb opening out at the end */                                     \
    F(12, P_KICK|SM, 30, 50, 1000, -1000) F(12, P_BASS|SM, 30, 60, 1000, -1000) F(12, P_HAT|SM, 0, 100, 1000, -1000)       \
    F(12, P_SNARE|SM, 20, 40, 1000, -1000) F(12, P_SUPER|SM, 40, 60, 1000, -1000) LEVEL(F, 12, P_PAD, 900)                 \
    F(12, P_CUT|SM, 0, 100, 4200, -2700) F(12, P_SUB|SM, 0, 100, 700, -700) F(12, P_PUMP, 0, 100, 500, -500)               \
    F(12, P_SPACE, 0, 100, 400, 300) F(12, P_RISER|SM, 57, 43, 0, 1000)   /* the burn-out to white (2.5 s) */              \
    /* THE JUNGLE / WHITE: the pad opens, high kalimba notes ring out, the whoosh                                          \
     * dies away, and we hear the forest before we see it. (From here on P_SNARE                                           \
     * is the claves, P_KICK a frame drum and P_HAT a shaker.) */                                                          \
    LEVEL(F, 13, P_PAD, 900) F(13, P_CUT|SM, 0, 100, 1500, 3500) LEVEL(F, 13, P_SPACE, 1000)                               \
    F(13, P_RISER|SM, 0, 60, 1000, -1000) LEVEL(F, 13, P_KAL, 450) F(13, P_WIND|SM, 55, 45, 0, 350)                        \
    F(13, P_AMB|SM, 70, 30, 0, 250)                                                                                        \
    /* LOOK ROUND: the filter settles lower, the cicadas swell, and the bass,                                              \
     * shaker, frame drum and claves come in one by one */                                                                 \
    F(14, P_PAD, 0, 100, 900, -200) F(14, P_CUT|SM, 0, 30, 5000, -2800) F(14, P_SPACE, 0, 30, 1000, -550)                  \
    F(14, P_AMB|SM, 0, 25, 250, 450) F(14, P_WIND, 0, 100, 350, 150) LEVEL(F, 14, P_BIRD, 450)                             \
    LEVEL(F, 14, P_KAL, 700) F(14, P_SUB|SM, 0, 30, 0, 600) F(14, P_BASS|SM, 0, 30, 0, 300)                                \
    F(14, P_HAT|SM, 15, 30, 0, 700) F(14, P_KICK|SM, 25, 30, 0, 550) F(14, P_SNARE|SM, 40, 30, 0, 500)                     \
    /* CANOPY: up into the light, with the birds overhead */                                                               \
    LEVEL(F, 15, P_PAD, 800) F(15, P_CUT|SM, 0, 100, 2200, 1800) LEVEL(F, 15, P_SPACE, 450) LEVEL(F, 15, P_AMB, 650)       \
    LEVEL(F, 15, P_WIND, 550) LEVEL(F, 15, P_BIRD, 1000) LEVEL(F, 15, P_KAL, 850) LEVEL(F, 15, P_SUB, 600)                 \
    LEVEL(F, 15, P_BASS, 330) LEVEL(F, 15, P_HAT, 800) LEVEL(F, 15, P_KICK, 600) LEVEL(F, 15, P_SNARE, 550)                \
    /* DESCENT: the groove thins out */                                                                                    \
    LEVEL(F, 16, P_PAD, 800) F(16, P_CUT|SM, 0, 100, 4000, -2600) F(16, P_SPACE, 0, 100, 450, 200)                         \
    F(16, P_AMB, 0, 100, 650, -200) LEVEL(F, 16, P_WIND, 450) F(16, P_BIRD, 0, 100, 700, -500)                             \
    LEVEL(F, 16, P_KAL, 750) F(16, P_SUB|SM, 0, 100, 600, -300) F(16, P_BASS|SM, 0, 70, 330, -330)                         \
    F(16, P_HAT|SM, 0, 80, 800, -800) F(16, P_KICK|SM, 20, 70, 600, -600) F(16, P_SNARE|SM, 0, 60, 550, -550)              \
    /* LADYBUG: the kalimba almost alone over a soft pad */                                                                \
    LEVEL(F, 17, P_PAD, 650) LEVEL(F, 17, P_CUT, 1400) LEVEL(F, 17, P_SPACE, 650) LEVEL(F, 17, P_AMB, 420)                 \
    LEVEL(F, 17, P_WIND, 280) LEVEL(F, 17, P_BIRD, 180) LEVEL(F, 17, P_KAL, 950) LEVEL(F, 17, P_SUB, 300)                  \
    /* TAKE-OFF: everything lifts on the dominant */                                                                       \
    F(18, P_PAD|SM, 0, 100, 650, 350) F(18, P_CUT|SM, 0, 100, 1400, 3600) LEVEL(F, 18, P_SPACE, 700)                       \
    LEVEL(F, 18, P_AMB, 420) F(18, P_WIND|SM, 0, 100, 280, 520) LEVEL(F, 18, P_BIRD, 350) LEVEL(F, 18, P_KAL, 950)         \
    F(18, P_SUB|SM, 0, 100, 300, 450) F(18, P_RISER|SM, 0, 100, 0, 800)                                                    \
    /* RESOLVE: home to D as the ladybug vanishes into the light, then the groove starts back */                           \
    F(19, P_PAD|SM, 0, 100, 1000, -200) F(19, P_CUT|SM, 0, 100, 5000, -1600) LEVEL(F, 19, P_SPACE, 750)                    \
    F(19, P_RISER|SM, 0, 40, 800, -800) F(19, P_AMB|SM, 0, 100, 420, 180) F(19, P_WIND|SM, 0, 100, 800, -350)              \
    F(19, P_BIRD, 0, 100, 350, 300) LEVEL(F, 19, P_KAL, 900) F(19, P_SUB|SM, 0, 100, 750, -250)                            \
    F(19, P_BASS|SM, 50, 50, 0, 300) F(19, P_HAT|SM, 40, 60, 0, 500) F(19, P_KICK|SM, 70, 30, 0, 450)                      \
    /* GLIDE: travelling between the trunks. The drums drop out near the forest's edge */                                  \
    LEVEL(F, 20, P_PAD, 800) F(20, P_CUT|SM, 0, 100, 3400, 1200) LEVEL(F, 20, P_SPACE, 500) LEVEL(F, 20, P_AMB, 600)       \
    LEVEL(F, 20, P_WIND, 450) LEVEL(F, 20, P_BIRD, 650) LEVEL(F, 20, P_KAL, 850) LEVEL(F, 20, P_SUB, 500)                  \
    F(20, P_BASS|SM, 75, 25, 300, -300) F(20, P_HAT|SM, 70, 30, 700, -700) F(20, P_KICK|SM, 70, 30, 550, -550)             \
    /* THE BEACH'S HUSH: the first glimpses through the trunks arrive over a                                              \
     * low D/Eb/Ab warning. Melody, birds and percussion fall away. */                                                     \
    F(21, P_PAD|SM, 0, 100, 450, 200) F(21, P_CUT|SM, 0, 100, 800, -350) LEVEL(F, 21, P_SPACE, 1000)                      \
    F(21, P_AMB|SM, 0, 50, 600, -550) F(21, P_WIND|SM, 0, 100, 450, -350) F(21, P_BIRD, 0, 20, 650, -650)                \
    F(21, P_KAL|SM, 0, 20, 850, -850) F(21, P_SUB|SM, 0, 100, 300, 250) F(21, P_BASS|SM, 0, 100, 0, 500)                 \
    LEVEL(F, 21, P_SURF, 180)                                                                                              \
    /* THE T-REX: the dissonant drone and low pulse thicken. arrange swells                                                \
     * their pressure into the first snort; on_step tightens the pulse as the                                             \
     * head turns toward us. The dark chord holds through the occluded exit. */                                           \
    F(22, P_PAD|SM, 0, 25, 650, 250) LEVEL(F, 22, P_CUT, 450) LEVEL(F, 22, P_SPACE, 1000)                                   \
    F(22, P_AMB|SM, 0, 10, 50, -50) F(22, P_WIND|SM, 0, 50, 100, -50) LEVEL(F, 22, P_SURF, 180)                           \
    F(22, P_SUB|SM, 0, 25, 550, 450) F(22, P_BASS|SM, 0, 25, 500, 400)                                                     \
    /* RELIEF: the T-rex has gone. The forest wakes and the waves come up */                                               \
    F(23, P_PAD|SM, 0, 100, 900, -150) F(23, P_CUT|SM, 0, 100, 450, 2550) LEVEL(F, 23, P_SPACE, 650)                       \
    F(23, P_AMB|SM, 20, 80, 0, 500) F(23, P_WIND|SM, 0, 100, 50, 400) F(23, P_BIRD|SM, 30, 70, 0, 600)                     \
    F(23, P_KAL|SM, 20, 50, 0, 650) F(23, P_SUB|SM, 0, 100, 1000, -650) F(23, P_BASS|SM, 0, 100, 900, -650)                \
    F(23, P_SURF|SM, 0, 100, 180, 620) F(23, P_HAT|SM, 60, 40, 0, 450)                                                     \
    /* THE BLOB: out of the surf. Scene 1's hook comes back on the kalimba, in A */                                        \
    LEVEL(F, 24, P_PAD, 780) F(24, P_CUT|SM, 0, 100, 3000, 1200) LEVEL(F, 24, P_SPACE, 450) LEVEL(F, 24, P_AMB, 450)       \
    LEVEL(F, 24, P_WIND, 400) LEVEL(F, 24, P_BIRD, 600) LEVEL(F, 24, P_KAL, 950) LEVEL(F, 24, P_SUB, 450)                  \
    LEVEL(F, 24, P_BASS, 380) LEVEL(F, 24, P_HAT, 650) F(24, P_KICK|SM, 0, 30, 300, 250) F(24, P_SNARE|SM, 20, 40, 0, 500) \
    LEVEL(F, 24, P_SURF, 650)                                                                                              \
    /* THE WAVE: home in A as the blob waves, with a rising run of high notes each bar and a bright pad */                 \
    LEVEL(F, 25, P_PAD, 900) LEVEL(F, 25, P_CUT, 5200) LEVEL(F, 25, P_SPACE, 900) LEVEL(F, 25, P_AMB, 450)                 \
    LEVEL(F, 25, P_WIND, 400) LEVEL(F, 25, P_BIRD, 600) LEVEL(F, 25, P_KAL, 950) LEVEL(F, 25, P_SUB, 450)                  \
    LEVEL(F, 25, P_BASS, 380) LEVEL(F, 25, P_HAT, 650) LEVEL(F, 25, P_KICK, 550) LEVEL(F, 25, P_SNARE, 500)                \
    LEVEL(F, 25, P_SURF, 650)                                                                                              \
    /* THE PLAY: down into the swash. The groove comes back up and the sea gets louder */                                  \
    LEVEL(F, 26, P_PAD, 780) F(26, P_CUT|SM, 0, 100, 3600, 1400) LEVEL(F, 26, P_SPACE, 500) LEVEL(F, 26, P_AMB, 400)       \
    LEVEL(F, 26, P_WIND, 400) LEVEL(F, 26, P_BIRD, 500) LEVEL(F, 26, P_KAL, 950) LEVEL(F, 26, P_SUB, 500)                  \
    LEVEL(F, 26, P_BASS, 450) LEVEL(F, 26, P_HAT, 750) LEVEL(F, 26, P_KICK, 650) LEVEL(F, 26, P_SNARE, 600)                \
    LEVEL(F, 26, P_SURF, 800)                                                                                              \
    /* THE END: the blob comes back up and sits to watch the sea. IV, V7, then home                                        \
     * on I as the last wave laps at it. The last chord rings on, and the waves and                                        \
     * the forest fade out with the picture */                                                                             \
    F(27, P_PAD|SM, 40, 60, 900, -900) F(27, P_CUT|SM, 0, 95, 5200, -4000) LEVEL(F, 27, P_SPACE, 900)                      \
    F(27, P_AMB|SM, 30, 65, 450, -450) F(27, P_WIND|SM, 25, 70, 400, -400) F(27, P_BIRD, 0, 60, 600, -600)                 \
    F(27, P_KAL|SM, 55, 40, 950, -950) F(27, P_SUB|SM, 30, 55, 450, -450) F(27, P_BASS|SM, 30, 45, 380, -380)              \
    F(27, P_HAT|SM, 0, 45, 650, -650) F(27, P_KICK|SM, 0, 40, 550, -550) F(27, P_SNARE|SM, 0, 40, 500, -500)               \
    F(27, P_SURF|SM, 40, 58, 650, -650) F(27, P_RISER|SM, 88, 12, 0, 800)   /* a swell into the end card */                \
    /* THE END CARD: scene 1's groove, straight in on the first kick, with the blob's                                      \
     * landings on the kalimba. The last bar sounds I once and dies away with the picture */                               \
    F(28, P_KICK|SM, 80, 10, 1000, -1000) F(28, P_HAT|SM, 80, 12, 900, -900) F(28, P_BASS|SM, 80, 12, 900, -900)           \
    F(28, P_LEAD|SM, 80, 14, 900, -900) F(28, P_LEAD2|SM, 80, 14, 500, -500) F(28, P_PAD|SM, 80, 16, 850, -850)            \
    LEVEL(F, 28, P_CUT, 3200) LEVEL(F, 28, P_SPACE, 300) F(28, P_PUMP, 80, 12, 600, -600) F(28, P_KAL|SM, 84, 12, 950, -950) \

#define RAMP_SEC_(s, p, a, w, v, d) s,
#define RAMP_PAR_(s, p, a, w, v, d) p,
#define RAMP_A_(s, p, a, w, v, d) a,
#define RAMP_W_(s, p, a, w, v, d) w,
/* v0 and d are stored in a signed byte each. Levels are stored in hundredths,
 * which is exact because every such value in the list is a multiple of 10
 * thousandths. The cutoff is stored in steps of 50 Hz, rounded to the nearest,
 * so it's at most 25 Hz off, which is inaudible. */
#define RAMP_Q_(p, v) (signed char)(((p) & 0x7f) == P_CUT ? ((v) + ((v) < 0 ? -25 : 25)) / 50 : (v) / 10)
#define RAMP_V0_(s, p, a, w, v, d) RAMP_Q_(p, v),
#define RAMP_D_(s, p, a, w, v, d) RAMP_Q_(p, d),
static const unsigned char RAMP_SEC[] = { RAMPS(RAMP_SEC_) }, RAMP_PAR[] = { RAMPS(RAMP_PAR_) },
                           RAMP_A[] = { RAMPS(RAMP_A_) }, RAMP_W[] = { RAMPS(RAMP_W_) };
static const signed char RAMP_V0[] = { RAMPS(RAMP_V0_) }, RAMP_D[] = { RAMPS(RAMP_D_) };
#undef LEVEL

/* The section at a position in bars. */
static int section_at(float bars) {
    int sec = 0;
    while (sec < SEC_COUNT - 1 && bars >= (float)SEC_BAR[sec + 1]) ++sec;
    return sec;
}

/* Evaluate the targets at a position in bars (parameters a section does not
 * mention are 0). The ramps are sorted by section; first[] indexes them (see
 * mc_synth_make). */
static void arrange(float *m, const unsigned short *first, float bars) {
    int sec = section_at(bars);
    float t = (bars - (float)SEC_BAR[sec]) / (float)(SEC_BAR[sec + 1] - SEC_BAR[sec]);
    memset(m, 0, P_COUNT * sizeof *m);
    for (int i = first[sec]; i < first[sec + 1]; ++i) {
        int par = RAMP_PAR[i] & 0x7f;
        float v = (float)RAMP_V0[i];
        if (RAMP_D[i]) {
            float x = clamp01((t - (float)RAMP_A[i] * 0.01f) / ((float)RAMP_W[i] * 0.01f));
            if (RAMP_PAR[i] & SM) x = sstep(x);
            v += (float)RAMP_D[i] * x;
        }
        m[par] = v * (par == P_CUT ? 50.0f : 0.01f);
    }
    /* The whale reveal, show 88..97 s: let the held chord and its room swell
     * while the busy lead and percussion step back. No extra voice or notes;
     * the same build resumes as the camera settles behind the blob. */
    if (sec == 10) {
        float q = sstep((bars - 45.5f) * 0.5f) * (1.0f - sstep((bars - 48.0f) * 0.5f));
        m[P_PAD] += q * 0.2f;
        m[P_SPACE] += q * 0.4f;
        m[P_SUPER] *= 1.0f - q * 0.65f;
        m[P_KICK] *= 1.0f - q * 0.35f;
        m[P_HAT] *= 1.0f - q * 0.5f;
    }
    /* The stalk: for four bars inside the peak, from the predator's first sight
     * to the near miss, the supersaw drops out and the pad's filter closes to
     * 500 Hz. Meanwhile on_step plays the descent's pattern rows: a half-time
     * heartbeat kick (the one on bar 63's third beat lands on the near miss)
     * and short roots, with no hats and no snare. The snare fill at the
     * stalk's end brings the chase back in. The stalk isn't a section of its
     * own, so it doesn't shift the section numbers, anything indexed by them
     * or the peak's percentages. */
    if ((unsigned)((int)bars - BAR_STALK) < 4) m[P_SUPER] = 0.0f, m[P_CUT] = 500.0f;
    /* A pressure swell into the first snort (46/128 of this section), then
     * out under the growl. Reuse the pad filter and noise riser, so the threat
     * has a rising edge without bringing the cheerful rhythm back. */
    if (sec == 22) {
        float q = sstep((t - 0.12f) / 0.24f) * (1.0f - sstep((t - 0.36f) / 0.08f));
        m[P_PAD] += q * 0.35f;
        m[P_CUT] += q * 1800.0f;
        m[P_RISER] = q * 0.55f;
    }
}

/* The whole synth: song clock, RNG, the instrument voices, the effects, and the
 * smoothed copies of the arrangement targets. */
struct mc_synth {
    mc_u32  seed;
    double  step_len;        /* samples per 16th step (fractional) */
    mc_u64  total_samples;   /* song length in frames */
    mc_u64  frame;           /* running render position */
    int     prev_step;       /* last step index, for edge-triggered events */
    int     mov;             /* the movement playing (on_step's numbering) */
    rng_t   rng;

    chord_def cur;
    float   pad_det[2];       /* the pad's left (flat) and right (sharp) detune */

    /* the arrangement's targets and the smoothed levels actually used (removes
     * zipper noise at section edges) */
    float   mix[P_COUNT], sm[P_COUNT];
    unsigned short ramp0[SEC_COUNT + 1];  /* each section's first ramp */

    kick_voice  kick;
    hat_voice   hat;
    bass_voice  bass;
    lead_voice  lead[2];      /* the lead and its octave layer */
    pad_voice   pad;
    super_voice super;        /* the finale's supersaw lead */
    snare_voice snare;        /* the finale's backbeat snare */
    float       sub_phase;    /* sub-bass sine (the finale and the jungle) */
    kal_voice   kal[6];       /* the jungle: kalimba tines (a new note takes the quietest) */
    bird_voice  bird[3];      /* ... bird calls (likewise) */
    mc_u64      wild_at;      /* the jungle's first frame: the leads and supersaw rest until the end card */
    float       clave_phase, clave_amp, clave_c;   /* ... claves */
    float       amb_phase;    /* ... the cicadas' buzz */
    float       slow[5];      /* ... slow swells: the wind's gusts, the cicadas' L/R choruses, the waves L/R */
    svf         cicada, wind, surf[2];
    float       thud_ph, thud_hz, thud_amp, thud_cr, thud_lp, thud_t;   /* ... the tyrannosaurus's footfalls */
    float       thud_c[3];    /* ... their pitch, body and crunch decays */
    float       br_amp, br_c, br_cut, br_t, br_ph;                       /* ... and its breath */
    rng_t       rex_rng;      /* ... their own noise, so they don't reshuffle the birds and the wind */
    svf         br_f;

    delay_fx   delay;
    reverb_fx  reverb;
    svf        riser_bp;

    float   sc_env;           /* sidechain envelope, retriggered on every kick */
    float   sc_coef;          /* its release coefficient */
    float   lim_g;            /* master limiter's current gain */
    float   dcx[2], dcy[2];   /* DC blocker state, per channel */
};

/* Note-ons. These set the one-shot state a voice needs; the render loop takes
 * it from there. */
static void kick_trig(kick_voice *k) {
    k->active  = 1;
    k->phase   = 0.0f;
    k->amp     = 1.0f;
    k->click   = 1.0f;
    k->pitch   = 150.0f;
    k->amp_c   = decay_coef(0.15f);
    k->click_c = decay_coef(0.004f);
    k->pitch_c = decay_coef(0.028f);
}
static void hat_trig(hat_voice *h, int vel, int open, int step) {
    h->active = 1;
    h->amp    = (float)vel * (1.0f / 7.0f);
    h->amp_c  = decay_coef(open ? 0.18f : 0.04f);
    h->pan    = (step & 2) ? 0.3f : -0.3f;
}
static void snare_trig(snare_voice *s) {
    s->active = 1;
    s->amp    = 1.0f;  s->amp_c  = decay_coef(0.13f);   /* noise body */
    s->tone   = 1.0f;  s->tone_c = decay_coef(0.09f);   /* tonal thump */
    s->tphase = 0.0f;
}

/* A bird somewhere in the trees: a whistle gliding up or down, a trill, or a
 * macaw's harsh, falling squawk. Squawks are likelier while the birds are
 * overhead. */
static void bird_call(mc_synth *s) {
    bird_voice *b = s->bird;
    for (int i = 1; i < 3; ++i) if (s->bird[i].a < b->a) b = &s->bird[i];
    float u = rng_f(&s->rng), r = rng_f(&s->rng), len, ratio;
    b->phase = b->trill = b->depth = b->fm = 0.0f;
    b->a = b->b = 1.0f;
    b->pan = rng_bi(&s->rng) * 0.8f;
    if (u < 0.3f * s->sm[P_BIRD]) {          /* squawk */
        b->freq = 600.0f + 250.0f * r; b->fm = 3.2f; len = 0.3f; ratio = 0.8f;
    } else if (u < 0.6f) {                   /* trill */
        b->freq = 3000.0f + 1500.0f * r; b->fm = 0.3f; len = 0.45f; ratio = 1.1f;
        b->depth = 1.0f; b->trill_dt = (24.0f + 14.0f * r) / SRF;
    } else {                                 /* whistle */
        b->freq = 1700.0f + 2000.0f * r; len = 0.12f + 0.25f * r; ratio = 2.75f * u - 1.05f;   /* 0.6 .. 1.7 */
    }
    b->sweep = powf(ratio, 1.0f / (len * SRF));
    b->a_c = decay_coef(len * 0.5f);
    b->b_c = decay_coef(0.012f);
}

/* Pluck the quietest kalimba tine. Its resonators keep whatever they still hold
 * and the new note (amplitude amp, ringing for ring seconds) is added on top, so
 * nothing clicks. */
static void kal_pluck(mc_synth *s, float note, float ring, float pan, float amp) {
    kal_voice *k = s->kal;
    for (int i = 1; i < 6; ++i)
        if (s->kal[i].re[0] * s->kal[i].re[0] + s->kal[i].im[0] * s->kal[i].im[0]
            < k->re[0] * k->re[0] + k->im[0] * k->im[0]) k = &s->kal[i];
    float w = MC_TAU * midi_hz(note) / SRF;
    for (int p = 0; p < 2; ++p) {      /* the note, then its 6th partial at 0.3 */
        k->c[p]  = cosf(w * (float)(1 + 5 * p));
        k->sn[p] = sinf(w * (float)(1 + 5 * p));
        k->d[p]  = decay_coef(ring / (float)(1 + 4 * p));
        k->re[p] += amp * (1.0f - 0.7f * (float)p);
    }
    k->pan = pan;
}

/* Fire the events scheduled on a given absolute step of the song. */
static void on_step(mc_synth *s, int step) {
    int bar = step / STEPS_PER_BAR;
    int st  = step % STEPS_PER_BAR;
    /* The movement: 0 = scene 1 (and the coda), 1 = scene 2's descent, 2 = its
     * finale, 3 = the jungle. Each reads its own tables. The progression
     * restarts with every movement and with every section of scene 1 and the
     * jungle, so each one opens on its home chord whatever its length. From
     * the jungle's take-off on, JHARM picks each section's chords instead. */
    int sec  = section_at((float)bar);
    int mov  = sec < 6 || sec == SEC_COUNT - 1 ? 0 : sec < 10 ? 1 : sec < 13 ? 2 : 3;   /* the coda: scene 1's */
    int from = SEC_BAR[sec < 6 || sec > 12 ? sec : sec < 10 ? 6 : 10];
    const unsigned char (*pat)[16] = PAT[(unsigned)(bar - BAR_STALK) < 4 ? 1 : mov];   /* the stalk: see arrange */
    s->mov = mov;

    if (st == 0) {
        int ci = (bar - from) & 3, row = mov;
        if (sec >= 18) {
            int h = JHARM[sec - 18];
            row = h >> 4;
            ci  = h & 4 ? h & 3 : (ci + h) & 3;
        }
        s->cur = PROG[row][ci];
        for (int i = 0; i < 3; ++i)
            s->pad.freq[i] = midi_hz((float)(s->cur.root + s->cur.n[i]));
    }
    /* Fills, in the bar before a few section changes, adding hits to the kick
     * (frame drum) and snare (claves) rows. A snare roll swelling from eighths
     * into sixteenths leads into the finale's peak. One beat of sixteenths
     * after the stalk's heartbeat drops the chase back in as the blob bolts
     * from the predator (from 0.23 s after the picture's PANIC_IN). Two beats
     * of sixteenths lead into the outro as the riser tops out. Into the canopy
     * and into the play, the frame drum and the claves answer each other. */
    static const struct { unsigned char bar; unsigned short k, sn; } FILL[5] = {
        { BAR_S3_PEAK - 1, 0, 0xFF55 }, { BAR_S3_PEAK + 12, 0, 0xF000 }, { BAR_S3_OUTRO - 1, 0, 0xFF00 },
        { BAR_WOODS + 7, 0x5000, 0xA000 }, { BAR_WOODS + 47, 0x5100, 0xA800 },
    };
    int fk = 0, fsn = 0;
    for (int i = 0; i < 5; ++i)
        if (bar == FILL[i].bar) { fk = FILL[i].k >> st & 1; fsn = FILL[i].sn >> st & 1; }
    if (pat[PT_KICK][st] | fk) {                /* every kick retriggers the sidechain */
        kick_trig(&s->kick); s->sc_env = 1.0f;
        if (mov == 3) { s->kick.pitch = 95.0f; s->kick.click = 0.15f; }   /* a soft frame drum */
    }
    if (pat[PT_HAT][st]) hat_trig(&s->hat, pat[PT_HAT][st], pat[PT_OPEN][st], st);
    if (pat[PT_SNARE][st] | fsn) {
        if (mov == 3) { s->clave_phase = 0.0f; s->clave_amp = 1.0f; }
        else {
            snare_trig(&s->snare);
            if (fsn) s->snare.amp = s->snare.tone = 0.3f + 0.1f * (float)st;   /* a roll swells past a plain hit */
        }
    }
    if (mov == 3 && rng_f(&s->rng) < s->sm[P_BIRD] * 0.16f) bird_call(s);
    /* the tyrannosaurus: thirteen footfalls two beats apart, starting on the
     * T-rex section's first step. They're heaviest as it passes closest (the
     * fifth and sixth). It snorts twice at the camera (at j 57.93 and 58.54 in
     * the picture; the attack is 0.1 s) and growls as it turns away (j 58.9). */
    int foot = step - (BAR_WOODS + 31) * STEPS_PER_BAR;
    if (foot >= 0 && foot < 104 && !(foot & 7)) {
        float q = (float)(foot >> 3) - 4.8f;
        s->thud_amp = 0.45f + 0.55f * expf(-q * q * (1.0f / 6.0f));
        s->thud_cr  = s->thud_amp;
        s->thud_hz  = 95.0f;
        s->thud_ph  = s->thud_t = 0.0f;
    }
    if (foot == 46 || foot == 51) { s->br_amp = 5.0f; s->br_cut = 750.0f; s->br_c = decay_coef(0.24f); s->br_t = 0.0f; }
    /* the blob's hops (s3_blob.c's JO_RUNS), each on a beat from jungle bar 42
     * (the story's 38). Each landing gets a soft, low pluck 0.36 s into the
     * beat. Landings in the swash also splash: a hiss through the breath's
     * filter (its shiver on the dry sand gets none). SPLASH holds each
     * splash's step counted from bar 48, its level in tenths and its cutoff in
     * hundreds of Hz. */
    static const unsigned char SPLASH[7][3] = {
        { 27, 12, 30 }, { 66, 30, 26 }, { 75, 18, 30 }, { 79, 18, 30 },
        { 87, 12, 32 }, { 91, 12, 32 }, { 131, 10, 34 },
    };
    int hop = step - (BAR_WOODS + 42) * STEPS_PER_BAR - 3;
    if (hop >= 0 && hop < 256 && !(hop & 3) && (0x1276c14730001ffull >> (hop >> 2) & 1))
        kal_pluck(s, (float)s->cur.root, 0.5f, 0.2f, 0.6f);
    /* the end card's blob, on the beat: one note for each syllable it lands on
     * (as if singing "HEL-LO MI-CRO-COS-MIC WORLD"), one for the
     * somersault's landing, and an A on the last downbeat */
    static const unsigned char END_MEL[20] = { 0, 0, 0, 0, 71, 76, 80, 76, 78, 73, 69, 0, 0, 0, 74, 0, 81 };
    int eb = step - BAR_END * STEPS_PER_BAR;
    if (eb >= 0 && !(eb & 3) && END_MEL[eb >> 2]) kal_pluck(s, (float)END_MEL[eb >> 2], 1.2f, 0.0f, 1.0f);
    for (int i = 0; i < 7; ++i)
        if (step - (BAR_WOODS + 48) * STEPS_PER_BAR == SPLASH[i][0]) {
            s->br_amp = SPLASH[i][1] * 0.1f; s->br_cut = SPLASH[i][2] * 100.0f; s->br_c = decay_coef(0.2f); s->br_t = 0.0f;
        }
    if (foot == 55) { s->br_amp = 3.0f; s->br_cut = 220.0f; s->br_c = decay_coef(1.35f); s->br_t = 0.0f; }
    int bn = pat[PT_BASS][st];
    /* Two-beat D/Eb pulses, tightening to one beat during the approach and
     * head turn. The jungle's syncopated bass resumes only after the exit. */
    if (sec == 22) bn = st & (foot >= 24 && foot < 56 ? 3 : 7) ? REST : 12 + ((foot >> 3) & 1);
    if (bn != REST) {
        s->bass.freq = midi_hz((float)(s->cur.root - 12 + bn));
        env_trig(&s->bass.amp,  ENV_BASS[mov]);
        env_trig(&s->bass.fenv, ENV_BFILT);
    }
    const unsigned char *arp = sec >= 24 ? PAT[0][PT_ARP] : pat[PT_ARP];
    if (arp[st] != REST) {
        float base = (float)(s->cur.root + 12) + chord_ext(&s->cur, arp[st]);
        if (mov == 3) {   /* the jungle: a kalimba tine, on the steps its section lets through */
            if (KAL4[sec - 13].mask >> st & 1)
                kal_pluck(s, base + (float)KAL4[sec - 13].oct, (float)KAL4[sec - 13].ring * 0.1f,
                          rng_bi(&s->rng) * 0.6f, 1.0f);
        } else if (mov == 2) {   /* the finale: the supersaw plays the line */
            s->super.freq = midi_hz(base);
            env_trig(&s->super.amp, ENV_LEAD[2][0]);
        } else {          /* the lead and its octave layer */
            for (int v = 0; v < 2; ++v) {
                s->lead[v].freq = midi_hz(base + 12.0f * (float)v);
                env_trig(&s->lead[v].amp, ENV_LEAD[mov][v]);
            }
        }
    }
}

mc_synth *mc_synth_make(mc_u32 seed) {
    mc_synth *s = calloc(1, sizeof *s);
    if (!s) return NULL;

    s->seed          = seed ? seed : 0x1234567u;
    s->rng.s         = s->seed;
    s->step_len      = (double)MC_SAMPLE_RATE * 60.0 / SONG_BPM / 4.0;
    s->total_samples = (mc_u64)(s->step_len * (double)TOTAL_STEPS + 0.5);
    s->prev_step     = -1;
    s->cur           = PROG[0][0];

    for (int i = 0; i < 3; ++i) {
        s->pad.freq[i]     = midi_hz((float)(s->cur.root + s->cur.n[i]));
        s->pad.phase[i][0] = rng_f(&s->rng);
        s->pad.phase[i][1] = rng_f(&s->rng);
    }
    s->pad_det[1] = 1.0040f + rng_f(&s->rng) * 0.0010f;   /* seed: incidental detune */
    s->pad_det[0] = 0.9960f - rng_f(&s->rng) * 0.0010f;
    s->bass.freq  = 110.0f;
    s->lead[0].freq = 440.0f;
    s->lead[1].freq = 880.0f;
    s->super.freq = 440.0f;

    /* Sidechain release (~160 ms pump) and the limiter starting wide open. */
    s->sc_coef = decay_coef(0.16f);
    s->lim_g   = 1.0f;
    s->clave_c = decay_coef(0.028f);
    s->rex_rng.s = s->seed ^ 0x7E5u;
    s->thud_c[0] = decay_coef(0.06f); s->thud_c[1] = decay_coef(0.25f); s->thud_c[2] = decay_coef(0.04f);
    s->wild_at = (mc_u64)(s->step_len * (double)(BAR_JUNGLE * STEPS_PER_BAR));

    /* Prime the smoothers at the first sample's targets. */
    for (int i = MC_ARRAY_COUNT(RAMP_SEC); i-- > 0;) s->ramp0[RAMP_SEC[i]] = (unsigned short)i;
    s->ramp0[SEC_COUNT] = MC_ARRAY_COUNT(RAMP_SEC);
    arrange(s->mix, s->ramp0, 0.0f);
    memcpy(s->sm, s->mix, sizeof s->sm);

    s->delay.len = (int)(3.0 * s->step_len + 0.5);        /* dotted-eighth echo */

    s->reverb.ap[0].len = 556;                             /* Freeverb's allpass tunings */
    s->reverb.ap[1].len = 441;
    return s;
}

void mc_synth_free(mc_synth *s) { free(s); }

double mc_synth_bpm(const mc_synth *s)    { (void)s; return SONG_BPM; }
double mc_synth_length(const mc_synth *s) { return (double)s->total_samples / (double)MC_SAMPLE_RATE; }

void mc_synth_render(mc_synth *s, float *out, int frames) {
    const float smc = 0.004f;   /* automation smoothing coefficient (~5 ms) */

    for (int n = 0; n < frames; ++n) {
        /* the song position. Past the end, the sequencer holds its last step
         * and the arrangement its last values, so whatever is still sounding
         * rings out. (Looping back to the start would let the song's first
         * 0.1 s, the audio queued as the show ends, play at the exit.) */
        mc_u64 sf = s->frame;

        /* ---- sequencer: fire step events on each 16th boundary ---- */
        int step = (int)((double)sf / s->step_len);
        if (step >= TOTAL_STEPS) step = TOTAL_STEPS - 1;
        if (step != s->prev_step) { on_step(s, step); s->prev_step = step; }

        /* ---- arrangement: targets, then their smoothed copies ---- */
        arrange(s->mix, s->ramp0, (float)((double)sf / (s->step_len * (double)STEPS_PER_BAR)));
        float *sm = s->sm;
        for (int i = 0; i < P_COUNT; ++i) sm[i] += (s->mix[i] - sm[i]) * smc;

        /* Two buses: the drums (dry, never ducked) and the musical bus (bass,
         * lead, pad, supersaw, reverb) which the sidechain pumps. */
        float drumL = 0.0f, drumR = 0.0f;
        float busL  = 0.0f, busR  = 0.0f;
        float drumC = 0.0f, busC  = 0.0f;   /* mono sends, split to both sides below */
        float hit   = 0.0f;                 /* the snare or the claves, also sent to the reverb */

        /* ---- kick ---- */
        if (s->kick.active) {
            float k = sinf(s->kick.phase * MC_TAU) * s->kick.amp;
            k += rng_bi(&s->rng) * s->kick.click * 0.6f;     /* transient click */
            s->kick.phase += s->kick.pitch / SRF;
            if (s->kick.phase >= 1.0f) s->kick.phase -= 1.0f;
            s->kick.pitch = 48.0f + (s->kick.pitch - 48.0f) * s->kick.pitch_c;
            s->kick.amp   *= s->kick.amp_c;
            s->kick.click *= s->kick.click_c;
            if (s->kick.amp < 1.0e-4f) s->kick.active = 0;
            drumC += k * sm[P_KICK] * 0.95f;
        }

        /* ---- hihat ---- */
        if (s->hat.active) {
            svf_run(&s->hat.hp, rng_bi(&s->rng), 9000.0f, 0.8f);
            float h = s->hat.hp.hp * s->hat.amp * sm[P_HAT] * 0.35f;
            s->hat.amp *= s->hat.amp_c;
            if (s->hat.amp < 1.0e-4f) s->hat.active = 0;
            drumL += h * (0.5f - 0.5f * s->hat.pan);
            drumR += h * (0.5f + 0.5f * s->hat.pan);
        }

        /* ---- snare (the finale): a noise body plus a short tonal thump ---- */
        if (s->snare.active) {
            svf_run(&s->snare.bp, rng_bi(&s->rng) * s->snare.amp, 1800.0f, 1.1f);
            s->snare.tphase += 190.0f / SRF;
            if (s->snare.tphase >= 1.0f) s->snare.tphase -= 1.0f;
            float body = sinf(s->snare.tphase * MC_TAU) * s->snare.tone;
            hit = (s->snare.bp.bp * 0.9f + body * 0.5f) * sm[P_SNARE] * 0.6f;
            s->snare.amp  *= s->snare.amp_c;
            s->snare.tone *= s->snare.tone_c;
            if (s->snare.amp < 1.0e-4f) s->snare.active = 0;
        }

        /* ---- bass (+ a sub sine an octave down in the finale and the jungle) ---- */
        float ae_bass;
        {
            float o   = osc_saw(&s->bass.phase, s->bass.freq / SRF);
            float fe  = env_run(&s->bass.fenv);
            ae_bass   = env_run(&s->bass.amp);
            svf_run(&s->bass.filt, o, 130.0f + fe * 2600.0f, 1.3f);
            busC += s->bass.filt.lp * ae_bass * sm[P_BASS] * 0.70f;
        }
        {
            float sub = sinf(s->sub_phase * MC_TAU);
            s->sub_phase += (s->bass.freq * 0.5f) / SRF;
            if (s->sub_phase >= 1.0f) s->sub_phase -= 1.0f;
            busC += sub * ae_bass * sm[P_SUB] * 0.7f;     /* 0 outside the finale and the jungle */
        }

        /* ---- lead + octave layer ---- */
        int tame = sf < s->wild_at || step >= BAR_END * STEPS_PER_BAR;   /* the leads and the supersaw rest in the jungle */
        float lead_dry = 0.0f;
        if (tame) {
            for (int v = 0; v < 2; ++v) {
                lead_voice *L = &s->lead[v];
                float dt = L->freq / SRF;
                float o  = 0.0f;
                for (int k = 0; k < 3; ++k) o += osc_saw(&L->phase[k], dt * LEAD_DET[v][k]);
                svf_run(&L->filt, o * 0.333f, LEAD_CUT[v], 1.0f);
                lead_dry += L->filt.lp * env_run(&L->amp) * sm[P_LEAD + v];
            }
        }
        /* ---- the jungle (fourth movement): kalimba tines, bird calls, claves,
         * cicadas and wind. They're all idle before it and draw no noise. ---- */
        float kal = 0.0f, birds = 0.0f, jung[2] = { 0.0f, 0.0f };
        for (int i = 0; i < 6; ++i) {
            kal_voice *k = &s->kal[i];
            if (k->re[0] * k->re[0] + k->im[0] * k->im[0] < 1.0e-6f) continue;
            float o = (k->im[0] + k->im[1]) * sm[P_KAL] * 0.16f;
            for (int p = 0; p < 2; ++p) {
                float r = (k->re[p] * k->c[p] - k->im[p] * k->sn[p]) * k->d[p];
                k->im[p] = (k->im[p] * k->c[p] + k->re[p] * k->sn[p]) * k->d[p];
                k->re[p] = r;
            }
            kal += o;
            jung[0] += o * (1.0f - k->pan); jung[1] += o * (1.0f + k->pan);
        }
        for (int i = 0; i < 3; ++i) {
            bird_voice *b = &s->bird[i];
            if (b->a < 1.0e-4f) continue;
            float o = sinf(MC_TAU * b->phase + b->fm * sinf(MC_TAU * b->phase)) * (b->a - b->b)
                    * (1.0f - b->depth * (0.5f + 0.5f * sinf(MC_TAU * b->trill))) * sm[P_BIRD] * 0.28f;
            b->phase += b->freq / SRF;
            if (b->phase >= 1.0f) b->phase -= 1.0f;
            b->trill += b->trill_dt;
            if (b->trill >= 1.0f) b->trill -= 1.0f;
            b->freq *= b->sweep;
            b->a *= b->a_c; b->b *= b->b_c;
            birds += o;
            jung[0] += o * (1.0f - b->pan); jung[1] += o * (1.0f + b->pan);
        }
        if (s->clave_amp > 1.0e-4f) {
            hit += sinf(MC_TAU * s->clave_phase) * s->clave_amp * sm[P_SNARE] * 0.22f;
            s->clave_phase += 2550.0f / SRF;
            if (s->clave_phase >= 1.0f) s->clave_phase -= 1.0f;
            s->clave_amp *= s->clave_c;
        }
        drumC += hit;
        /* ---- the tyrannosaurus: each footfall is a sub sine falling from 95 to
         * 36 Hz under a crunch of litter. The second block is its breath: two
         * snorts and a rasping growl ---- */
        if (s->thud_amp > 1.0e-4f) {
            s->thud_lp += (rng_bi(&s->rex_rng) * s->thud_cr - s->thud_lp) * 0.2f;
            drumC += (sinf(MC_TAU * s->thud_ph) * s->thud_amp * fminf(s->thud_t, 1.0f) + s->thud_lp * 1.5f) * 0.9f;
            s->thud_t  += 1.0f / 130.0f;                    /* a ~3 ms onset */
            s->thud_ph += s->thud_hz / SRF;
            if (s->thud_ph >= 1.0f) s->thud_ph -= 1.0f;
            s->thud_hz   = 36.0f + (s->thud_hz - 36.0f) * s->thud_c[0];
            s->thud_amp *= s->thud_c[1];
            s->thud_cr  *= s->thud_c[2];
        }
        if (s->br_amp > 1.0e-4f) {
            float src = rng_bi(&s->rex_rng);
            if (s->br_cut < 500.0f) src += osc_saw(&s->br_ph, 48.0f / SRF) * 0.8f;   /* the growl's throat */
            svf_run(&s->br_f, src, s->br_cut, 1.6f);
            drumC += (s->br_cut < 500.0f ? s->br_f.lp : s->br_f.bp) * s->br_amp * fminf(s->br_t * 10.0f, 1.0f)
                   * (0.6f + 0.4f * sinf(MC_TAU * 23.0f * s->br_t)) * 0.8f;
            s->br_t   += 1.0f / SRF;
            s->br_amp *= s->br_c;
        }
        if ((sf & 63) == 0) {                     /* the slow swells, every 64 samples */
            float tt = (float)sf * (1.0f / SRF);
            s->slow[0] = (0.5f + 0.5f * sinf(tt * 0.53f)) * (0.6f + 0.4f * sinf(tt * 0.29f + 1.3f));
            for (int c = 0; c < 2; ++c) {
                s->slow[1 + c] = 0.55f + 0.45f * sinf(tt * (0.41f + 0.23f * (float)c) + 2.0f * (float)c);
                float x = tt / (7.3f + 1.6f * (float)c) + 0.37f * (float)c;   /* a wave: it rushes up, draws back */
                x -= floorf(x);
                s->slow[3 + c] = sstep(x * 5.0f) * (1.0f - x) * (1.0f - x);
            }
            /* the reverb: the movement's voicing, each line's length wandering,
             * and the gain for its length */
            reverb_fx *rv = &s->reverb;
            rv->damp = RVV[s->mov][1] * 0.01f;
            for (int i = 0; i < 8; ++i) {
                rv->at[i] = (float)RV_TAP[i] + 6.0f * sinf(tt * (0.7f + 0.29f * (float)i) + (float)i);
                rv->g[i]  = exp2f(rv->at[i] * (-99.66f / SRF) / (float)RVV[s->mov][0]);
            }
        }
        if (sm[P_AMB] + sm[P_WIND] > 1.0e-4f) {
            /* a cicada chorus buzzing at ~87 Hz, swelling at its own slow rate on
             * each side; wind in the leaves, in gusts */
            float ph = s->amb_phase, buzz = 4.0f * ph * (1.0f - ph), g = s->slow[0];
            s->amb_phase += 87.0f / SRF;
            if (s->amb_phase >= 1.0f) s->amb_phase -= 1.0f;
            svf_run(&s->wind, rng_bi(&s->rng), 500.0f + 1400.0f * g, 0.7f);
            svf_run(&s->cicada, rng_bi(&s->rng), 5400.0f, 5.0f);
            for (int c = 0; c < 2; ++c)
                jung[c] += s->cicada.bp * buzz * buzz * s->slow[1 + c] * sm[P_AMB] * 0.35f
                         + s->wind.bp * g * sm[P_WIND] * 0.3f;
        }
        if (sm[P_SURF] > 1.0e-4f)                 /* the surf, each side on its own waves */
            for (int c = 0; c < 2; ++c) {
                float w = s->slow[3 + c];
                svf_run(&s->surf[c], rng_bi(&s->rng), 300.0f + 1500.0f * w, 0.7f);
                jung[c] += s->surf[c].lp * (0.25f + w) * sm[P_SURF] * 0.5f;
            }
        busL += jung[0] * 0.5f; busR += jung[1] * 0.5f;

        float dl, dr;
        delay_run(&s->delay, lead_dry * 0.5f + kal * 0.35f, 0.42f, &dl, &dr);
        busC += lead_dry * 0.55f;
        busL += dl * 0.5f; busR += dr * 0.5f;

        /* ---- pad (stereo, detuned) and the supersaw lead (the finale: 7
         * detuned saws, stereo-spread), each through a lowpass per side. Both
         * use the pad's cutoff, so the supersaw opens and closes with the
         * arrangement, from 1.2 kHz as the build starts to 6 kHz at the height
         * of the flight. Even there its aliases stay about 55 dB under its
         * harmonics ---- */
        float pad[2] = { 0.0f, 0.0f }, sup[2] = { 0.0f, 0.0f };
        {
            for (int i = 0; i < 3; ++i) {
                float dt = s->pad.freq[i] / SRF;
                for (int c = 0; c < 2; ++c) pad[c] += osc_saw(&s->pad.phase[i][c], dt * s->pad_det[c]);
            }
            float ae = tame ? env_run(&s->super.amp) : 0.0f;
            float base = s->super.freq / SRF;
            for (int i = 0; tame && i < 7; ++i) {
                float o = osc_saw(&s->super.phase[i], base * (1.0f + SUPER_DET[i]));
                sup[0] += o * (0.5f - 0.5f * SUPER_PAN[i]);
                sup[1] += o * (0.5f + 0.5f * SUPER_PAN[i]);
            }
            for (int c = 0; c < 2; ++c) {
                svf_run(&s->pad.filt[c], pad[c] * 0.2f, sm[P_CUT], 0.9f);
                if (tame) svf_run(&s->super.filt[c], sup[c] * (1.0f / 7.0f), sm[P_CUT], 0.9f);
                pad[c] = s->pad.filt[c].lp * sm[P_PAD] * 0.9f;
                sup[c] = s->super.filt[c].lp * ae * sm[P_SUPER] * 1.2f;   /* 0 outside scene 2's finale */
            }
            busL += pad[0]; busR += pad[1];
            busL += sup[0]; busR += sup[1];
        }

        /* ---- riser (noise sweep into the drops), on the unducked drum bus ---- */
        if (sm[P_RISER] > 1.0e-4f) {
            svf_run(&s->riser_bp, rng_bi(&s->rng), 400.0f + sm[P_RISER] * 6000.0f, 2.0f);
            drumC += s->riser_bp.bp * sm[P_RISER] * 0.5f;
        }

        /* ---- reverb: the pad, lead, supersaw, kalimba, birds and the snare or
         * claves go in (as much as the section asks for), a stereo tail comes
         * out ---- */
        {
            float rin = (lead_dry * 0.25f + (pad[0] + pad[1]) * 0.20f
                       + (sup[0] + sup[1]) * 0.10f) * sm[P_SPACE];
            rin += (kal * 0.35f + birds * 0.6f + hit) * sm[P_SPACE];
            float wet[2] = { 0.0f, 0.0f };
            reverb_run(&s->reverb, rin, wet);
            busL += wet[0] * 0.14f; busR += wet[1] * 0.14f;
        }

        drumL += drumC * 0.5f; drumR += drumC * 0.5f;
        busL  += busC * 0.5f;  busR  += busC * 0.5f;

        /* ---- sidechain pump: duck the musical bus off the kick envelope ---- */
        float duck = 1.0f - sm[P_PUMP] * 0.8f * s->sc_env;
        busL *= duck; busR *= duck;
        s->sc_env *= s->sc_coef;

        float ch[2] = { drumL + busL, drumR + busR };

        /* ---- master limiter: fast-attack / slow-release peak control, so the
         * fuller finale mix doesn't drive the soft-clip hard ---- */
        float peak = fabsf(ch[0]); { float ar = fabsf(ch[1]); if (ar > peak) peak = ar; }
        float tgt = (peak > 1.1f) ? (1.1f / peak) : 1.0f;
        if (tgt < s->lim_g) s->lim_g = tgt;                    /* catch transients at once */
        else s->lim_g += (tgt - s->lim_g) * 0.0006f;           /* ease back up (~40 ms) */

        /* ---- master: DC block then tanh soft-clip safety with headroom ---- */
        for (int c = 0; c < 2; ++c) {
            float x = ch[c] * s->lim_g;
            float y = x - s->dcx[c] + 0.9975f * s->dcy[c];
            s->dcx[c] = x; s->dcy[c] = y;
            out[2 * n + c] = tanhf(y * 1.1f) * 0.92f;
        }

        s->frame++;
    }
}
