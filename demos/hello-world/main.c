/*
 * main.c - "Hello World", the demo that shows off what the engine can do.
 *
 * A single bright dot goes bang and opens into a small universe of procedural
 * shapes that orbit, drift and now and then crash into each other. Cubes spell
 * out MICROCOSM, and then everything collapses back to a dot. That's Scene 1.
 * Two more scenes and an end card follow (see below), and the show plays once.
 * Nothing here is loaded from disk: the engine generates the meshes, the
 * motion, the post-processing and the music at runtime.
 *
 * There's one render_frame() and three main()s to drive it, picked at compile
 * time:
 *   - windowed (default) : real-time, locked to the soundtrack, ESC to quit.
 *                          The release build quits when the show ends, and
 *                          the dev build holds on black for the scrubber.
 *   - MC_HEADLESS        : render a fixed set of frames to build/frame_*.png
 *                          and exit. The checks (tools/checks) also build this
 *                          file headless, but with their own main().
 *   - MC_CAPTURE         : (with MC_HEADLESS) stream the whole show as raw
 *                          RGBA to stdout for ffmpeg (make capture).
 */
#include "microcosm/mc_platform.h"
#include "microcosm/mc_render.h"
#include "microcosm/mc_profile.h"
#include "microcosm/mc_mesh.h"
#include "microcosm/mc_scene.h"
#include "microcosm/mc_math.h"
#include "microcosm/mc.h"
#if !defined(MC_HEADLESS) || defined(MC_CAPTURE)   /* the capture writes the soundtrack too */
#include "microcosm/mc_audio.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "microcosm/mc_imports.h"   /* payload calls by hash; keep after system headers */

/* The knobs: resolutions, body count and the timeline anchors. This is what
 * you'd reach for first if you wanted to retune the thing. */

#define WIN_W   1280            /* the dev build's window; the release takes the screen */
#define WIN_H   720
#define RENDER_W 960            /* the headless build's PNG size */
#define RENDER_H 540
#define NBODY   182            /* how many shapes scene 1 has */
/* Every time below is in seconds and tied to the 124 BPM soundtrack, so the
 * visuals land on the music. Scene 1 is the song's first movement, 26 bars
 * (~50.3 s). The section starts are counted in bars and must match synth.c:
 *   intro 0-4 | build 4-8 | hook A 8-12 | breakdown 12-16 | hook B 16-23 |
 *   outro 23-26  (bars)
 * If you retime the song, move these with it. The bang sits on the first
 * kick drop. */
#define BPM     124.0
#define BAR_S   (240.0 / BPM)   /* one 4/4 bar, ~1.935 s */
#define T_BANG  (4 * BAR_S)     /* ~7.7 s: dot -> universe, on the first kick (build) */
#define T_HOOKA (8 * BAR_S)     /* ~15.5 s */
#define T_BREAK (12 * BAR_S)    /* ~23.2 s */
#define T_HOOKB (16 * BAR_S)    /* ~31.0 s */
#define T_OUTRO (23 * BAR_S)    /* ~44.5 s */
#define DUR     (26 * BAR_S)    /* ~50.3 s: scene 1 ends on the music's change */
#define T_LOGO_IN  (T_HOOKB + 1.7)  /* the crowd gathers into the logo (hook B) */
#define T_FORM_IN  (T_HOOKB - BAR_S) /* a spiral resolves on hook B's first downbeat */
#define T_FORM_OUT (T_HOOKB + BAR_S / 4) /* hold one beat, release before the title */
#define T_LOGO_OUT (T_OUTRO - 1.5)  /* ...and scatters again, as the outro comes */
#define T_COLLAPSE (T_OUTRO + 0.1)  /* outro: everything falls back to a dot */
#define T_BLACK    (T_OUTRO + 4.7)  /* fully black here; it holds black until DUR */
#define T_WINK     (T_HOOKB + 8.4)  /* the blob winks during the finale */
#define MAX_LOGO 512           /* the title's 142 cubes and the end card's 323 */
#define LOGO_CELL 0.22f         /* world size of one glyph cell */
#define LOGO_Y    1.6f          /* where the logo plane sits vertically */
#define SIM_DT    (1.0 / 120.0) /* collision sim runs at a fixed 120 Hz */

/* The demo is three scenes and an end card, each played once, and then it
 * stops. Scene 1 is the big bang described above. Scene 2 is a plunge from
 * orbit, through a blue-green planet's sky and into the sea, where the blob is
 * waiting, and it ends by burning out to white. Scene 3 opens on that white
 * and becomes a rainforest (scene3.c). When scene 3 goes black, the end card
 * says hello in scene 1's cubes and the blob bounces along the words
 * (render_end). It all runs off one monotonic clock timed to the synth's
 * movements (synth.c), so picture and music stay in step with no other
 * syncing. Scene beats are in scene-local seconds. */
#define SCENE1_DUR     DUR          /* scene 1 is all 26 bars of DUR (~50.3 s) */
#define SCENE2_DUR     (54 * BAR_S) /* scene 2 = 54 bars (~104.5 s): descent, journey, scare, transformation, flight */
#define SCENE3_DUR     (59 * BAR_S) /* scene 3 = 59 bars (~114.2 s): the white, the jungle, the beach */
#define SCENE2_START   (SCENE1_DUR)
#define SCENE4_DUR     (5 * BAR_S)  /* the end card = 5 bars (~9.7 s) */
#define SCENE3_START   (SCENE2_START + SCENE2_DUR)
#define SCENE4_START   (SCENE3_START + SCENE3_DUR)
#define TOTAL_DUR      (SCENE4_START + SCENE4_DUR)
#define END_TEXT       "HEL-LO/MI-CRO-COS-MIC/WORLD"   /* the end card: a line per '/', '-' between syllables */
#define END_SYL        7            /* ... its syllables, one blob landing each */
#define S2_SPACE_END   ((float)(5 * BAR_S))   /* ~9.7 s: orbit drift ends, the dive begins */
#define S2_CLOUDFLASH  (S2_SPACE_END + 4.86f) /* ~14.5 s: punch through the cloud deck (planet -> sea cut) */
#define S2_BREACH      ((float)(10 * BAR_S))  /* ~19.4 s: hit the water and go under */
#define S2_DIP         0.17f        /* the lens takes this long either side of S2_BREACH to go under */
#define S2_END_FADE    (SCENE2_DUR - 2.5)     /* the burn-out to white begins */
#define PLANET_R       20.0f        /* planet radius, scene-2 world units */

/* The underwater journey runs for ~85 scene-local seconds after the breach.
 * The seabed is a big fBm heightfield sunk well below the blob, so nothing
 * ever clips the floor, and the props on it are placed so nothing collides. */
/* The underwater act is choreographed on a story clock u = 0..UW_SPAN since
 * the breach. The blob's path, the scatter, the predator, the cameras and
 * every cue below are in story time: 40 story bars of UW_BAR seconds. The real
 * clock runs through them in 44 bars (SCENE2_DUR - S2_BREACH, ~85 s), with
 * each part stretched at its own pace (S2_SEG). uw_u() and uw_t() convert
 * between the two. */
#define UW_SPAN        104.5f       /* story seconds of underwater travel */
#define UW_BAR         (UW_SPAN / 40.0f)   /* story seconds per story bar (~2.61) */
/* The seabed is a 240 m square of 200 x 200 cells, raised by fBm of amplitude
 * 9 and sunk to UW_FLOOR_BASE. It's built by mc_mesh_terrain(UW_TERR_SIZE,
 * UW_TERR_SUB, UW_TERR_AMP, UW_TERR_SEED), and uwplace.comp's sand() computes
 * the same thing on the GPU. The checks make sure the two agree. */
#define UW_TERR_SIZE   240.0f
#define UW_TERR_SUB    200          /* seabed heightfield subdivisions */
#define UW_TERR_AMP    9.0f
#define UW_TERR_SEED   0x5EAB3Du
#define UW_FLOOR_BASE  (-16.0f)     /* seabed sits here; blob swims around y=-4.5 */
#define UW_BAKE_RES    512          /* baked material texture resolution */
#define UW_SHADOW_RES  2048         /* sun shadow-map resolution */
/* The boulder is baked by uwplace.comp: a ball of 6 x 20 x 20 cells (plus two
 * coarser LODs of 10 and 5) pushed out by three octaves of noise. UW_ROCK_R is
 * its radius for placement, and rock.vert's crags go on top (UW_ROCK_VDISP).
 * The baked rock's farthest vertex goes 0.0004 past it, inside the crags'
 * headroom. Every prop is placed against this value, so changing it moves
 * them all. */
#define UW_ROCK_N      20
#define UW_ROCK_R      1.3664165f
#define UW_ACC_START   34.0f        /* unused: the blob speeds up from FLIGHT_IN (blob_phase) */
#define UW_BLOB_R      3.0f         /* blob radius used for collision-free placement */
#define UW_MAXF        190          /* fish budget (the circling schools) */
#define BIG_U          92.5f        /* where on its flight the blob bolts through the big school */
#define UW_MAXP        8192         /* the seabed's props: boulders, weed, coral and crabs (uwplace.comp) */
#define UW_NCAND       (34 * 34 * 48 + 18 * 96)   /* uwplace.comp's candidates: cell slots plus landmark slots */
#define UW_BATCH_CAP   UW_MAXP      /* instances per draw batch */
#define UW_NSCHOOL     26           /* fish schools */
#define UW_PATH_SAMP   820          /* samples of the blob path, for clearance tests (dense: the flight is fast) */
#define UW_ROCK_VDISP  0.30f        /* headroom (unit space) for rock.vert's progressive crags */
#define WHALE_IN       4.0f         /* a whale glides past as the water clears (show ~73-107 s; seen ~76.5-96 s) */
#define WHALE_OUT      44.0f
#define WHALE_Z        (-42.0f)     /* its lane, kept well clear of the blob (z >= -27) */
#define WHALE_SCALE    14.0f        /* ~30 units long, the size of a blue whale */
#define WHALE_ZHALF    6.8f         /* whale half-extent across its lane (fluke span) */
#define UW_LOOK        26.0f        /* the whale reveal's held upward gaze, bright water behind it (show ~91.8 s) */

/* The encounter. A big predator comes out of the murk ahead and swims
 * straight at the blob, which hasn't noticed it. It lunges with its jaws open
 * and misses by a hair at ENC_PEAK, and only then does the blob panic: it
 * flinches and bolts (blob_phase's flee term). The predator overshoots, turns
 * to give chase, falls behind and breaks off into the deep. A heartbeat in the
 * music runs through the stalk, with its kick on the near miss (the stalk in
 * synth.c, bars 60-63), and a snare fill brings the anthem back for the chase.
 * Its fixed chase spline is reconstructed at startup (pred_build); the
 * seabed's prop placement keeps rock and coral clear of that path. */
#define ENC_IN         56.0f        /* the predator emerges from the murk ahead */
#define ENC_LUNGE      62.3f        /* stalking glide turns into the full-speed lunge */
#define ENC_PEAK       64.0f        /* the near miss: it shoots past, a hair from the blob */
#define PANIC_IN       64.35f       /* ...and only now the blob panics: flinch, then bolt */
#define ENC_GIVEUP     68.0f        /* outrun, the predator breaks off the chase */
#define ENC_OUT        80.0f        /* gone into the deep (not drawn after this) */
/* After the scare the blob changes. The camera circles it while its eyes grow
 * and its shape rounds out, and then the blob speeds off across the ocean. */
#define TRANS_IN       72.0f        /* the camera starts orbiting the (slowed) blob */
#define TRANS_END      84.0f        /* 0.88 of a lap (camera_at2); transformation complete */
#define FLIGHT_IN      84.0f        /* then it accelerates across the ocean, revealing scenery */
#define PRED_SCALE     4.5f         /* ~13 units long */
#define PRED_SIDE      (-1.0f)      /* -1: it strikes past the blob's left, away from the hairpin
                                       turn (to the right) that the blob makes as it bolts */
#define PRED_HZ        60           /* the reconstructed path's sample rate */
#define PRED_N         (((int)ENC_OUT - (int)ENC_IN) * PRED_HZ + 2)   /* whole seconds */
#define UW_GRID        (81 * 101 * 209)    /* the largest animal's sculpting grid (jr.comp's GS: the predator's) */

enum { SH_ICO, SH_BOX, SH_TORUS, SH_CONE, SH_CYL, SH_SPIKE, SH_COUNT };

typedef struct {
    /* who this body is - decided once in build_bodies and never touched again */
    int    base_type;
    mc_v3  base_color;
    mc_v3  dir;        /* unit direction out from the origin */
    float  orbit;      /* how far out it orbits once fully expanded */
    float  phase;      /* per-body time offset, so they don't move in lockstep */
    mc_v3  axis;       /* spin axis */
    float  spin;       /* spin speed */
    float  emissive;   /* 0..1 base emissive */
    float  base_scale;

    /* what collisions have done to it - reset by reseed_loop */
    int    type;       /* current shape (a combine can swap it)     */
    mc_v3  color;      /* current colour (ditto)                    */
    int    alive;      /* goes 0 when another body absorbs it       */
    mc_v3  poff;       /* offset from the analytic orbit, from collisions */
    mc_v3  voff;       /* and its velocity                          */
    float  cool;       /* seconds until this body may collide again */
    float  pop;        /* seconds left on the post-combine scale pop */
} Body;

/* Scene 2's seabed props. uwplace.comp places them once at load, on the GPU,
 * and after that they're just drawn each frame. Each one grows in at its birth
 * time, out of the camera's sight, so the count grows as the blob explores.
 * None of them ever intersects the blob's path, the camera's, the predator's,
 * the fish or each other. A prop is uwplace.comp's record: position and size
 * (for weed, a blade's height), rotation (axis, angle), colour, and k = (kind:
 * 1 boulder, 2 coral, 3 crab, 4 weed; birth; death, when a landmark grows over
 * it, or 0 for never; reach). */
typedef struct { mc_v4 at, turn, color, k; } UwProp;
typedef struct { mc_v3 c; float R, om; int n0, n; } UwSchool;
typedef struct { int school; float alpha, dr, sc, ph, tint, ybob; } UwFish;
/* uwplace.comp's buffer (binding 0, std430), which stays bound there while
 * the frames are drawn. The shader's first pass fills in the seabed heights
 * (world y). These are the seabed's vertices in object.vert, and fy() in the
 * shader replays their triangles so the props sit on the sand that's drawn.
 * It also fills in the boulder's vertices (position and normal, for
 * rock.vert), each LOD in a slot of 6 x 21 x 21, the finest one's size. Next
 * come the things the props are placed against: the blob's path, the main
 * camera (camera_uw) every 0.25 s (position and unit forward), every 4th
 * sample of the predator's path, and the fish schools (centre and ring
 * radius). The sand shader reads the answer too, since it lists the boulders
 * on each 3.5 m square of seabed for their contact shading. */
#define UW_CAM_SAMP 419             /* UW_SPAN (104.5 s) at 4 a second, plus one */
#define UW_ROCK_VERTS (3 * 6 * 21 * 21)   /* the boulder's three LODs' slots */
static struct UwIn {
    float h[(UW_TERR_SUB + 1) * (UW_TERR_SUB + 1)], pad[3];
    mc_v4 rock[2 * UW_ROCK_VERTS];
    mc_v4 path[UW_PATH_SAMP], cam[2 * UW_CAM_SAMP], pred[PRED_N / 4 + 1], school[UW_NSCHOOL];
    UwProp o[UW_NCAND + 9 + 68 * 68 + 68 * 17];   /* the answer: the candidates, the landmark sites' records,
                                           * then the boulders on each 3.5 m square of sand and of the
                                           * reef bed (uwsurf.frag) */
} uw_in;
_Static_assert(offsetof(struct UwIn, o) == 28022 * 16, "uwplace.comp's answer starts on a vec4 (std430): "
               "vec4 28022 of the buffer, as its pass 19 and uwsurf.frag count");

enum { UW_BED, UW_ROCK0, UW_ROCK1, UW_ROCK2, UW_ALGA, UW_FISH,
       UW_CRAB, UW_CORAL, UW_WHALE, UW_PRED, UW_BLOB, UW_BATCHES };
typedef struct {
    mc_instance items[UW_BATCH_CAP];
    unsigned char mask[UW_BATCH_CAP];
    int n, both, color, shadow;
} UwBatch;

enum {
#define MC_PROGRAM(field, vs, fs) MC_PROG_##field,
#define MC_BAKE(vs, fs) MC_PROG_BAKE,
#include "programs.def"
#undef MC_PROGRAM
#undef MC_BAKE
    MC_PROGRAM_COUNT
};

/* The per-frame uniforms, mirroring the std140 `Frame` block every scene shader
 * declares (a vec3 is 12 bytes and the float after it packs into its 16-byte
 * slot). Each scene fills what it draws with and uploads it once; the
 * underwater shadow pass uploads it again with uViewProj = uLightVP. */
typedef struct {
    mc_m4 view_proj, light_vp;              /* uViewProj, uLightVP */
    mc_v3 cam;          float time;         /* uCam, uTime */
    mc_v3 light_dir;    float real;         /* uLightDir, uReal */
    mc_v3 fog_color;    float clarity;      /* uFogColor, uClarity */
    mc_v3 sky_color;    float q;            /* uSkyColor, uQ */
    mc_v3 ground_color; float fog;          /* uGroundColor, uFog */
    mc_v3 rim;          float wobble;       /* uRim, uWobble */
    mc_v3 sun;          float blob_fog;     /* uSun, uBlobFog */
    mc_v3 top;          float gape;         /* uTop, uGape */
    mc_v3 bottom;       float glow_amt;     /* uBottom, uGlowAmt */
    mc_v3 glow;         float grid_fade;    /* uGlow, uGridFade */
    mc_v3 grid_color;   float atmo;         /* uGridColor, uAtmo (scene 1: the bang's shock, the outro's fade) */
    mc_v3 sea_sky;      float bright;       /* uSky, uBright */
    mc_v2 res, sun_uv;                      /* uRes, uSunUV */
    float tex_scale, algae_h, pad_[2];      /* uTexScale, uAlgaeH */
} FrameUniforms;
_Static_assert(sizeof(FrameUniforms) == 352, "FrameUniforms must match the std140 Frame block");

/* The few per-draw uniforms that live outside the Frame block, at locations
 * the shaders fix with layout(location). They are the backdrops' model matrix,
 * the object shader's shape (uK, which fish.vert reads as its flags), the
 * seabed's sand-only switch and the bakes' material. */
enum { U_MODEL = 0, U_K = 1, U_SAND_ONLY = 2, U_STORY = 3, U_TYPE = 0 };

typedef struct {
    mc_gpu_mesh shapes[SH_COUNT];
    mc_gpu_mesh ground;
    mc_gpu_mesh blob;      /* the round blue blob */
    mc_gpu_mesh planet;    /* scene 2: the planet (and, scaled up, its atmosphere) */
    mc_gpu_mesh ocean;     /* scene 2: the wavy sea surface */
    /* scene 2 underwater journey */
    mc_gpu_mesh uw_seabed, uw_rock[3], uw_fish, uw_algae, uw_whale;   /* the boulder's three LODs */
    UwBatch uw_batch[UW_BATCHES];
    unsigned char rock_level[UW_MAXP];
    float uw_wobble;
    mc_v3 pred_path[PRED_N];      /* the predator's fitted path, ENC_IN.. at PRED_HZ */
#ifdef MC_PROFILE
    int disable_culling, disable_lod, legacy_order; /* private A/B controls */
#endif
    mc_gpu_mesh uw_pred, uw_crab, uw_coral;   /* predator, crabs, coral */
    mc_gpu_mesh uw_jelly;                     /* a moon jelly: bell, tentacles, arms (jelly.vert) */
    GLuint      uw_kit;               /* the sculpted animals' vertices (uw_sculpt) */
    GLuint      uw_buf;               /* uwplace.comp's buffer (demo_free frees it; the payload just exits) */
    mc_gpu_mesh jr_body;              /* scene 3's tyrannosaurus, sculpted with them */
    GLuint      tex_sand, tex_rock;   /* baked seabed materials */
    GLuint      noise;                /* the 32^3 value noise on unit 1 (noise_make) */
    GLuint      tex_fishskin, tex_whaleskin;   /* baked UV skins for the creatures */
    mc_target   uw_shadow;            /* sun shadow map (depth texture) */
    mc_m4       uw_lightvp;           /* light view-proj for the current frame */
    float       uw_shadow_span;
    UwProp   uw_props[UW_MAXP]; int uw_np;   /* the seabed props, placed at load */
    UwSchool uw_schools[UW_NSCHOOL]; int uw_ns;
    UwFish   uw_fish_s[UW_MAXF];   int uw_nf;

    mc_shader sky, obj, grnd, blobsh;
    mc_shader space, planet_sh, atmo, moon_sh, ocean_sh, water, dip;   /* scene 2 */
    mc_shader uwsurf, uwrock, fishsh, algaesh, whalesh, coralsh, uwplace;  /* scene 2 underwater */
    mc_shader shadowrock, shadowfish, shadowcoral, shadowalgae;
    mc_shader shadowsh;                                          /* sun shadow-map depth pass */
    mc_shader jellysh;                                           /* the moon jellies */
    /* scene 3: one field per program in the jungle packages' manifests */
#define MC_PROGRAM(field, vs, fs) mc_shader field;
#include "scene3.def"
#undef MC_PROGRAM
    FrameUniforms frame;   /* this frame's Frame block */

    Body         bodies[NBODY];
    mc_instance *inst[SH_COUNT];
    int          inst_count[SH_COUNT];

    /* finale logo and end card (instanced cubes spelling the title, then the
     * end card's greeting from logo_n to text_n) */
    mc_v3 logo_pos[MAX_LOGO];    /* target position in the logo plane */
    mc_v3 logo_start[MAX_LOGO];  /* scattered origin it flies in from  */
    mc_v3 logo_col[MAX_LOGO];
    unsigned char logo_syl[MAX_LOGO];   /* the end card's syllable it is in */
    int   logo_n, text_n;
    mc_v3 syl[END_SYL];          /* each syllable's top centre, where the blob lands */

    int    loop;          /* scene 1's loop number (always 0)    */
    mc_v3  blob_color;    /* the blob's colour for that loop     */
    mc_rng crng;          /* RNG for collision outcomes          */
    double sim_t;         /* time the collision sim has reached  */
    int    alive_count;   /* live bodies (combines reduce this)  */

#ifndef MC_RELEASE
    mc_u64 shader_hashes[MC_PROGRAM_COUNT][2];
    mc_u64 shader_attempted[MC_PROGRAM_COUNT][2];
    double shader_poll_at;
#endif
    mc_renderer *R;
} Demo;

#ifdef MC_RELEASE   /* release: the sources come by index (shaders_gen.h's mc_program_shaders) */
static const struct { size_t offset; } programs[] = {
#define MC_PROGRAM(field, vs, fs) {offsetof(Demo, field)},
#define MC_BAKE(vs, fs) {SIZE_MAX},
#else
static const struct { size_t offset; const char *vs, *fs; } programs[] = {
#define MC_PROGRAM(field, vs, fs) {offsetof(Demo, field), vs, fs},
#define MC_BAKE(vs, fs) {SIZE_MAX, vs, fs},
#endif
#include "programs.def"
#undef MC_PROGRAM
#undef MC_BAKE
};

/* Neon pastels for the shapes and the letters. */
static const mc_v3 PALETTE[] = {
    {1.00f, 0.42f, 0.62f}, /* pink    */
    {1.00f, 0.72f, 0.30f}, /* orange  */
    {1.00f, 0.95f, 0.45f}, /* yellow  */
    {0.45f, 0.92f, 0.70f}, /* mint    */
    {0.40f, 0.78f, 1.00f}, /* sky     */
    {0.72f, 0.55f, 1.00f}, /* violet  */
    {1.00f, 0.55f, 0.90f}, /* magenta */
};
#define PALETTE_N MC_ARRAY_COUNT(PALETTE)

/* Four acts, each with its own pace and colour, with the boundaries on the
 * music's section changes. Each act runs the motion clock at a different
 * speed, but only the clock's rate changes at a boundary (see motion_time),
 * so nothing jumps. */
#define NSEQ 4
static const double SEQ_START[NSEQ + 1] = { 0.0, T_HOOKA, T_BREAK, T_HOOKB, DUR };
static const float  SEQ_SPEED[NSEQ]     = { 0.90f, 1.15f, 0.70f, 1.10f };
/* The first two acts have deep, dark skies so the pastel crowd stands out
 * against them, instead of being a pastel cloud on a pastel sky. The far side
 * of the crowd sinks into the fog (uFogColor is the bottom colour). */
static const mc_v3  SEQ_TOP[NSEQ] = {       /* sky colour above the horizon   */
    {0.45f, 0.25f, 0.28f},  /* I  dawn    - dusky rose   */
    {0.19f, 0.32f, 0.45f},  /* II play    - deep blue    */
    {0.30f, 0.64f, 0.58f},  /* III deep   - calm teal    */
    {0.40f, 0.20f, 0.85f},  /* IV finale  - deep violet  */
};
static const mc_v3  SEQ_BOT[NSEQ] = {       /* sky colour below it            */
    {0.10f, 0.04f, 0.07f},
    {0.03f, 0.05f, 0.09f},
    {0.04f, 0.11f, 0.10f},
    {0.11f, 0.05f, 0.17f},
};

/* The warped motion clock: real seconds, each scaled by its act's speed and
 * summed. It keeps climbing without stepping back or jumping, and only its
 * rate changes at an act boundary. Most of scene 1's motion runs off this
 * instead of raw t. */
static double motion_time(double t) {
    double tw = 0.0;
    for (int i = 0; i < NSEQ; ++i) {
        if (t <= SEQ_START[i]) break;
        double end = (t < SEQ_START[i + 1]) ? t : SEQ_START[i + 1];
        tw += (end - SEQ_START[i]) * (double)SEQ_SPEED[i];
        if (t < SEQ_START[i + 1]) break;
    }
    return tw;
}

/* Sky palette for time t. It cross-fades into the next act's over the 2.5 s
 * before the boundary, so the colours never jump. */
static void atmosphere(double t, mc_v3 *top, mc_v3 *bot) {
    int i = 0;
    while (i < NSEQ - 1 && t >= SEQ_START[i + 1]) ++i;
    mc_v3 tp = SEQ_TOP[i], bt = SEQ_BOT[i];
    if (i < NSEQ - 1) {
        float nb = (float)SEQ_START[i + 1];
        float g = mc_smoothstepf(nb - 2.5f, nb, (float)t);
        tp = mc_v3_lerp(tp, SEQ_TOP[i + 1], g);
        bt = mc_v3_lerp(bt, SEQ_BOT[i + 1], g);
    }
    *top = tp; *bot = bt;
}

/* Shader loading. A release build has the shaders, minified, baked into the
 * executable. A dev build reads the .glsl files off disk so they can be
 * hot-reloaded while you edit them. */

#ifdef MC_RELEASE
#include "shaders_gen.h"   /* generated by genshaders.py: the blob and each program's sources */
static mc_shader program_source(int i) {
    return mc_shader_make(mc_shader_at(mc_program_shaders[i][0]), mc_shader_at(mc_program_shaders[i][1]));
}
#else
#include <stdio.h>
#ifndef MC_SHADER_ROOT
#define MC_SHADER_ROOT "demos/hello-world/shaders"
#endif
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { mc_log("cannot open shader %s", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { mc_log("ftell failed for %s", path); fclose(f); return NULL; }
    char *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) {   /* short/failed read */
        mc_log("short read on %s", path);
        free(b); fclose(f); return NULL;
    }
    b[n] = 0;
    fclose(f);
    return b;
}
static mc_shader build_program(const char *vs, const char *fs) {
    char p1[256], p2[256];
    snprintf(p1, sizeof p1, "%s/%s", MC_SHADER_ROOT, vs);
    snprintf(p2, sizeof p2, "%s/%s", MC_SHADER_ROOT, fs);
    char *a = read_file(p1), *b = strcmp(vs, fs) ? read_file(p2) : a;   /* same file: a compute shader */
    mc_shader s = mc_shader_make(a ? a : "", b ? b : "");
    if (b != a) free(b);
    free(a);
    return s;
}
#endif

static mc_shader *program_at(Demo *d, size_t offset) { return (mc_shader *)((char *)d + offset); }

/* After every (re)link, set the one thing that never changes: the seabed
 * program's sand-only switch. There's nothing else to look up or bind. The
 * sampler units and per-draw uniform locations are fixed in the shaders
 * (layout(binding), layout(location)), everything per frame lives in the
 * Frame block, and GL puts every block back on binding 0 whenever a program
 * links. */
static void fetch_uniforms(Demo *d) {
    mc_shader_use(d->uwsurf);
    mc_set_i(U_SAND_ONLY, 1);
}

static int load_shaders(Demo *d) {
    for (int i = 0; i < MC_PROGRAM_COUNT; ++i) {
        if (programs[i].offset == SIZE_MAX) continue;
        mc_shader *slot = (mc_shader *)((char *)d + programs[i].offset);
#ifdef MC_RELEASE
        *slot = program_source(i);
#else
        *slot = build_program(programs[i].vs, programs[i].fs);
#endif
        if (!slot->prog) return 0;
    }
    fetch_uniforms(d);
    /* the Frame block's constants */
    FrameUniforms *F = &d->frame;
    F->grid_color = mc_vec3(0.35f, 0.75f, 1.0f);
    F->sea_sky    = mc_vec3(0.55f, 0.72f, 1.0f);   /* the sky the sea reflects */
    F->sun_uv     = mc_vec2(0.68f, 0.72f);         /* the sun in the space backdrop */
    F->tex_scale  = 0.12f;                         /* seabed/rock texture tiling */
    F->algae_h    = 1.0f;
    return 1;
}

/* Bake a tiled material (uwbake.frag's seabed and skins, jt_bake.frag's jungle)
 * into a repeat-wrapped, mipmapped 2D texture by rendering the bake into it once.
 * Types 0 and 1 (the seabed's sand and rock, or the bark and leaves) get 4x
 * anisotropic filtering, which keeps them sharp where the floor and the rocks
 * slope away. Types 2 and 3 don't, because on the fish scales it would only
 * sharpen the moire. The jungle's sand is its type 2, and it gets its 4x in
 * trees_make. */
static GLuint bake_tex(mc_shader bake, int type) {
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, UW_BAKE_RES, UW_BAKE_RES, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, 4 >> (type & 2));
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &tex); return 0;
    }
    glViewport(0, 0, UW_BAKE_RES, UW_BAKE_RES);
    glDisable(GL_DEPTH_TEST);
    mc_shader_use(bake);
    mc_set_i(U_TYPE, type);
    mc_fullscreen_triangle();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGenerateMipmap(GL_TEXTURE_2D);
    return tex;
}
static int bake_underwater(Demo *d) {
#ifdef MC_RELEASE
    mc_shader bake = program_source(MC_PROG_BAKE);
#else
    mc_shader bake = build_program("sky.vert", "uwbake.frag");
#endif
    if (!bake.prog) return 0;
    d->tex_sand = bake_tex(bake, 0);
    d->tex_rock = bake_tex(bake, 1);
    d->tex_fishskin  = bake_tex(bake, 2);
    d->tex_whaleskin = bake_tex(bake, 3);
    mc_shader_free(&bake);
    return d->tex_sand && d->tex_rock && d->tex_fishskin && d->tex_whaleskin;
}

/* Dev hot reload. Content hashes catch rapid edits, deletions and atomic file
 * replacement. It builds every affected program and material first, then
 * swaps them all in together, or keeps the old ones if any fails. */
#ifdef MC_RELEASE
static void maybe_reload_shaders(Demo *d) { (void)d; }
#else
static void scan_shaders(mc_u64 hashes[MC_PROGRAM_COUNT][2]) {
    for (int i = 0; i < MC_PROGRAM_COUNT; ++i) for (int stage = 0; stage < 2; ++stage) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", MC_SHADER_ROOT, stage ? programs[i].fs : programs[i].vs);
        char *text = read_file(path);
        mc_u64 hash = 14695981039346656037ULL;
        if (text) {
            for (const unsigned char *p = (unsigned char *)text; *p; ++p)
                hash = (hash ^ *p) * 1099511628211ULL;
            free(text);
        } else hash = 0;
        hashes[i][stage] = hash;
    }
}

static void maybe_reload_shaders(Demo *d) {
    double now = mc_time_seconds();
    if (now < d->shader_poll_at) return;
    d->shader_poll_at = now + 0.25;
    mc_u64 hashes[MC_PROGRAM_COUNT][2];
    scan_shaders(hashes);
    if (!memcmp(hashes, d->shader_attempted, sizeof hashes)) return;
    memcpy(d->shader_attempted, hashes, sizeof hashes);
    mc_shader replacements[MC_PROGRAM_COUNT] = {{0}};
    GLuint textures[4] = {0};
    int changed = 0, ok = 1, rebake = 0;
    for (int i = 0; i < MC_PROGRAM_COUNT; ++i) {
        if (!memcmp(hashes[i], d->shader_hashes[i], sizeof hashes[i])) continue;
        changed = 1;
        replacements[i] = build_program(programs[i].vs, programs[i].fs);
        if (!replacements[i].prog) { ok = 0; break; }
        if (programs[i].offset == SIZE_MAX) {
            rebake = 1;
            for (int k = 0; k < 4; ++k) {
                textures[k] = bake_tex(replacements[i], k);
                if (!textures[k]) ok = 0;
            }
        }
    }
    if (changed && ok) {
        memcpy(d->shader_hashes, hashes, sizeof hashes);
        for (int i = 0; i < MC_PROGRAM_COUNT; ++i) {
            if (!replacements[i].prog || programs[i].offset == SIZE_MAX) continue;
            mc_shader *slot = (mc_shader *)((char *)d + programs[i].offset);
            mc_shader_free(slot); *slot = replacements[i]; replacements[i].prog = 0;
        }
        if (rebake) {
            GLuint previous[] = {d->tex_sand, d->tex_rock, d->tex_fishskin, d->tex_whaleskin};
            glDeleteTextures(4, previous);
            d->tex_sand = textures[0]; d->tex_rock = textures[1];
            d->tex_fishskin = textures[2]; d->tex_whaleskin = textures[3];
            memset(textures, 0, sizeof textures);
        }
        fetch_uniforms(d);
        mc_log("affected shaders/materials reloaded");
    } else if (changed) mc_log("shader reload failed; previous programs/materials retained");
    for (int i = 0; i < MC_PROGRAM_COUNT; ++i) mc_shader_free(&replacements[i]);
    glDeleteTextures(4, textures);
}
#endif

/* One-time setup: build the meshes, the bodies, the logo. */

/* Every mesh in the demo is built in its vertex shader from gl_VertexID, so
 * none of them carries vertex data (mc_mesh_procedural). Scenes 1 and 2's
 * solids, the blob, the planet, the sea, the coral and the seaweed are just `n`
 * vertices drawn as a plain triangle list. `radius` is the shape's reach, used
 * for culling. */
static void vs_mesh(mc_gpu_mesh *g, int n, float radius) { *g = mc_mesh_procedural(n, NULL, radius); }

/* The seabed and the boulder are grids whose vertices uwplace.comp computes
 * once, and their shaders read them back by gl_VertexID. They're drawn by
 * index so each vertex is shaded only once. The index list covers `faces`
 * square grids of n x n cells, each grid's (n + 1)^2 vertices numbered row by
 * row from `base`. Each cell is split into two triangles the way the shaders'
 * corner() expects. */
static void grid_mesh(mc_gpu_mesh *g, int faces, int n, int base, float radius) {
    static mc_u32 ix[UW_TERR_SUB * UW_TERR_SUB * 6];
    int k = 0, m = n + 1;
    for (int r = 0; r < faces * m; ++r)
        for (int c = 0; c < n && r % m < n; ++c)
            for (int v = 0; v < 6; ++v)
                ix[k++] = (mc_u32)(base + (r + (52 >> v & 1)) * m + c + (22 >> v & 1));
    *g = mc_mesh_procedural(k, ix, radius);
}

/* The animals are sculpted at load with the creature kit (jr.comp). Scene 3's
 * tyrannosaurus comes first, then scene 2's fish, crab, predator and whale,
 * all into one shared vertex buffer that jr.vert and fish.vert read on binding
 * 5. Each animal gets its own triangles, which become its mesh's index buffer
 * (scene 2's animals are drawn in the usual instance batches). The sculpting
 * grid's two buffers are deleted once they're done. The culling radii cover
 * each sea animal's whole swing. */
static void uw_sculpt(Demo *d) {
    static const float radius[5] = { 0.0f, 1.8f, 1.45f, 1.95f, 1.25f };
    mc_gpu_mesh *g[5] = { &d->jr_body, &d->uw_fish, &d->uw_crab, &d->uw_pred, &d->uw_whale };
    GLuint b[4], zero[6] = {0};
    const GLsizeiptr size[4] = { UW_GRID * 4, UW_GRID * 4, 200000 * 32, sizeof zero };
    glGenBuffers(4, b);
    for (int i = 0; i < 4; ++i) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, b[i]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, size[i], i == 3 ? zero : NULL, GL_STATIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, (GLuint)(i + i / 3), b[i]);   /* the counter goes on 4 */
    }
    mc_shader_use(d->jr_mesh);
    for (int k = 0; k < 5; ++k) {
        mc_gpu_mesh *m = g[k];
        glGenVertexArrays(1, &m->vao);
        glBindVertexArray(m->vao);
        glGenBuffers(1, &m->ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, 1200000 * 4, NULL, GL_STATIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m->ibo);
        for (int p = 0; p < 3; ++p) {
            glUniform1i(0, 4 * k + p);
            glDispatchCompute(21, 31, 70);
            glMemoryBarrier(GL_ALL_BARRIER_BITS);
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, b[3]);               /* read its index count, then reset it for the next */
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, 4, &m->index_count);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, 4, zero);
        m->index_type = GL_UNSIGNED_INT;
        m->radius = radius[k];
    }
    glBindVertexArray(0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, d->uw_kit = b[2]);
    glDeleteBuffers(2, b);
    glDeleteBuffers(1, b + 3);
}

static void build_shapes(Demo *d) {
    /* Scene 1's solids are a ball, a rounded box, a torus, a cone, a cylinder
     * and a sea urchin (a ball with 18 cone spikes on a Fibonacci spiral).
     * object.vert builds them from gl_VertexID, with uK picking which one, and
     * ground.vert builds the floor. So their meshes are nothing but these
     * vertex counts. A ball of n x n grids takes 6 x n x n x 6 vertices. n is 6
     * for the ball as Scene 2's bubbles and specks draw it, and 12 for Scene
     * 1's (s1_shapes draws it with 4x the vertices). It's 8 for the box and the
     * urchin's core, and the urchin's 18 spikes take 16 x 3 x 6 each. */
    static const short SHAPE_VERTS[SH_COUNT] = { 1296, 2304, 2352, 504, 504, 7488 };
    for (int s = 0; s < SH_COUNT; ++s) vs_mesh(&d->shapes[s], SHAPE_VERTS[s], 0.0f);
    vs_mesh(&d->ground, 6, 0.0f);

    /* The blob: a smooth ball made in blob.vert (48 x 48 cells a face, for the
     * end card's arms), which also does the noise wobble (scaled by uWobble)
     * so the transformation can round it out at runtime. */
    vs_mesh(&d->blob, 82944, 0x1.000002p+0f);

    /* Scene 2. A smooth high-detail ball for the planet (all its surface detail
     * is in the shader), and a big square of 140 x 140 cells for the sea. The
     * sea has enough vertices that the vertex-shader waves look like a real
     * swell. Both are built in their vertex shaders too. */
    vs_mesh(&d->planet, 82944, 0x1.000002p+0f);
    vs_mesh(&d->ocean, 117600, 0.0f);

    /* Scene 2's underwater journey. There's a rolling sand seabed (one grid of
     * 200 x 200 cells) and craggy boulders. A boulder is a ball of 6 x 20 x 20
     * cells displaced by three noise octaves (big lumps, medium bumps and fine
     * crags), with two coarser LODs of the same rock at 10 and 5 cells a side.
     * Then a seaweed blade and a coral, which coral.vert grows. The animals
     * are sculpted in uw_sculpt. */
    grid_mesh(&d->uw_seabed, 1, UW_TERR_SUB, 0, 0.0f);
    for (int level = 0; level < 3; ++level)
        grid_mesh(&d->uw_rock[level], 6, UW_ROCK_N >> level, level * UW_ROCK_VERTS / 3, UW_ROCK_R);
    vs_mesh(&d->uw_algae, 96, 0x1.002a74p+0f);
    vs_mesh(&d->uw_coral, 29 * 36, 1.03f);
    vs_mesh(&d->uw_jelly, (16 * 6 + 12 * 10) * 6, 0.0f);
}

/* A tiny 5x7 bitmap font - only the letters in "MICROCOSM" and the end card's
 * "HELLO MICROCOSMIC WORLD", because that's all we spell. One byte per row,
 * 7 rows. The low 5 bits are the pixels, and bit 4 is the leftmost column. */
static const char FONT_CHARS[] = "MICROSHELWD";
static const unsigned char FONT[][7] = {
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F},
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},
};

/* Lay out a text in cubes, from line y downwards, one centred line per '/'. A
 * '-' starts the next syllable, and so does a new line. Each lit pixel is a
 * cube's target, paired with a scattered origin it flies in from. Each
 * syllable's top centre goes into syl (at most END_SYL of them) for the end
 * card's blob. */
static void build_text(Demo *d, const char *s, float y, mc_rng *rng) {
    const float cell = LOGO_CELL;
    int n = d->text_n, syl = 0;
    for (;;) {
        int len = 0, k = 0, k0 = 0;
        for (const char *p = s; *p && *p != '/'; ++p) len += *p != '-';
        float x0 = -(float)(6 * len - 1) * 0.5f * cell;
        for (;; ++s) {
            if (!*s || *s == '-' || *s == '/') {            /* a syllable ends: letters k0 .. k-1 */
                d->syl[syl++] = mc_vec3(x0 + (float)(3 * (k0 + k) - 1) * cell, y + 3.0f * cell, 0.0f);
                k0 = k;
                if (*s == '-') continue;
                break;
            }
            int f = 0;
            while (FONT_CHARS[f] != *s) ++f;
            for (int r = 0; r < 7; ++r)
                for (int c = 0; c < 5; ++c) {
                    if (!(FONT[f][r] & (1 << (4 - c)))) continue;
                    d->logo_pos[n]   = mc_vec3(x0 + (float)(6 * k + c) * cell, (3.0f - (float)r) * cell + y, 0.0f);
                    d->logo_start[n] = mc_v3_scale(mc_rng_unit_v3(rng), mc_rng_range(rng, 8.0f, 16.0f));
                    d->logo_col[n]   = PALETTE[(k + c) % PALETTE_N];
                    d->logo_syl[n++] = (unsigned char)syl;
                }
            ++k;
        }
        if (!*s++) break;
        y -= 12.0f * cell;
    }
    d->text_n = n;
}

static void build_logo(Demo *d) {
    mc_rng rng = mc_rng_seed(0x109017u);
    build_text(d, "MICROCOSM", LOGO_Y, &rng);
    d->logo_n = d->text_n;
    build_text(d, END_TEXT, LOGO_Y + 12.0f * LOGO_CELL, &rng);
}

static void build_bodies(Demo *d) {
    mc_rng rng = mc_rng_seed(0xB16BA75u);
    for (int i = 0; i < NBODY; ++i) {
        Body *b = &d->bodies[i];
        b->base_type  = (int)(mc_rng_next(&rng) % SH_COUNT);
        b->dir   = mc_rng_unit_v3(&rng);
        b->orbit = mc_rng_range(&rng, 2.0f, 8.5f);
        b->phase = mc_rng_range(&rng, 0.0f, MC_TAU);
        b->axis  = mc_rng_unit_v3(&rng);
        b->spin  = mc_rng_range(&rng, -1.6f, 1.6f);
        b->base_color = PALETTE[mc_rng_next(&rng) % PALETTE_N];
        b->emissive  = (mc_rng_f(&rng) < 0.33f) ? mc_rng_range(&rng, -0.8f, -0.3f) : 0.0f;   /* (below 0: object.frag) */
        b->base_scale = mc_rng_range(&rng, 0.35f, 0.9f);
        /* the material is coded in the colour's signs (object.frag). Every
         * fourth body is glass (red below 0) and every fourth from the third on
         * is iridescent (green below 0). The rest, and whatever a combine
         * makes, are pastel plastic. */
        if (i & 1) (&b->base_color.x)[i >> 1 & 1] *= -1.0f;
    }
}

/* The blob's colour for each loop of scene 1. The show plays once, so only
 * loop 0's blue is ever used: "the round blue blob" in the notes. */
static const mc_v3 BLOB_COLORS[] = {
    {0.15f, 0.40f, 1.00f},  /* blue    */
    {1.00f, 0.30f, 0.55f},  /* rose    */
    {0.20f, 0.95f, 0.70f},  /* teal    */
    {1.00f, 0.65f, 0.15f},  /* amber   */
    {0.75f, 0.35f, 1.00f},  /* purple  */
    {0.20f, 0.85f, 1.00f},  /* cyan    */
};
#define BLOB_COLORS_N MC_ARRAY_COUNT(BLOB_COLORS)

/* Reset scene 1's collision state. Every body goes back to its original shape,
 * colour and position, the cooldowns clear, and the collision RNG and the blob
 * colour are re-derived from the loop number. The same loop number always
 * gives the same run, which is what lets the dev scrubber seek backwards
 * (sim_seek). */
static void reseed_loop(Demo *d, int loop) {
    d->loop = loop;
    d->blob_color = BLOB_COLORS[((unsigned)loop) % BLOB_COLORS_N];
    d->crng = mc_rng_seed(0x5EED0017u ^ ((mc_u32)loop * 2654435761u));
    d->sim_t = 0.0;
    d->alive_count = NBODY;

    for (int i = 0; i < NBODY; ++i) {
        Body *b = &d->bodies[i];
        b->type  = b->base_type;
        b->color = b->base_color;
        b->alive = 1;
        b->poff  = mc_vec3(0, 0, 0);
        b->voff  = mc_vec3(0, 0, 0);
        b->cool  = 0.0f;
        b->pop   = 0.0f;
    }
}

/* Per-frame animation. Everything below maps a time t to some scalar the frame
 * needs; render_frame stitches the results together. */

/* How open the universe is, 0 (a dot) to 1 (fully expanded). */
static float expansion(double t) {
    if (t < T_BANG) {
        /* a barely-there ramp so the pre-bang frame isn't dead still */
        return 0.02f * (float)t / (float)T_BANG;
    }
    if (t < T_COLLAPSE) {
        float u = (float)((t - T_BANG) / 2.2);        /* the burst takes ~2.2s */
        return mc_ease(MC_EASE_OUT_ELASTIC, mc_saturatef(u)) * 1.0f;  /* overshoot, then settle */
    }
    float u = (float)((t - T_COLLAPSE) / (T_BLACK - T_COLLAPSE));
    return (1.0f - mc_ease(MC_EASE_INOUT_CUBIC, mc_saturatef(u)));
}

/* Overall body size: springs in at the bang (with a little overshoot from the
 * back-ease), holds, then shrinks away at the collapse. */
static float body_scale(double t) {
    float in = mc_ease(MC_EASE_OUT_BACK, mc_saturatef((float)((t - T_BANG) / 1.2)));
    float out = 1.0f;
    if (t >= T_COLLAPSE)
        out = 1.0f - mc_ease(MC_EASE_IN_CUBIC, mc_saturatef((float)((t - T_COLLAPSE) / (T_BLACK - T_COLLAPSE))));
    return in * out;
}

/* A 0..1 "energy" curve that follows the song's dynamics. A lot of brightness
 * and motion amounts are multiplied by this so the whole scene leans into the
 * drops and eases off in the breakdown. */
static float energy(double t) {
    float e = mc_smoothstepf((float)T_BANG, (float)T_BANG + 5.0f, (float)t);  /* rise after the drop */
    if (t > T_BREAK && t < T_HOOKB)                                           /* breakdown dip */
        e *= mc_lerpf(1.0f, 0.5f, mc_smoothstepf((float)T_BREAK, (float)T_BREAK + 2.5f, (float)t));
    if (t > T_HOOKB)                                                          /* hook B reprise */
        e = mc_maxf(e, mc_smoothstepf((float)T_HOOKB, (float)T_HOOKB + 2.0f, (float)t));
    if (t >= T_COLLAPSE) e *= (1.0f - mc_window_phase(&(mc_timeline){t, DUR, BPM}, T_COLLAPSE, T_BLACK - T_COLLAPSE));
    return mc_saturatef(e);
}

/* The blob. A smooth hump: 0 at the edges of a time window, 1 through the
 * middle, so the blob can fade in and back out without any hard edges. */
static float hump(double t, double start, double len) {
    float u = mc_window_phase(&(mc_timeline){t, DUR, BPM}, start, len);
    return mc_smoothstepf(0.0f, 0.22f, u) * (1.0f - mc_smoothstepf(0.78f, 1.0f, u));
}
/* How present the blob is right now, 0..1. It's the max of three humps, so
 * the blob makes three appearances, each stronger than the last, and the last
 * one is over the logo. */
static float blob_presence(double t) {
    float p = 0.35f * hump(t, T_HOOKA - 1.5, 3.0);          /* a shy first glimpse (build)  */
    p = mc_maxf(p, 0.62f * hump(t, T_BREAK + 0.5, 6.5));    /* drifts past in the breakdown */
    p = mc_maxf(p, 0.85f * hump(t, T_HOOKB + 2.0, 10.0));   /* and hangs over the finale    */
    return p;
}
/* Where the blob sits in the world. It's parked far back and low, drifts on a
 * couple of slow sines, and rises as its presence grows. For the finale it
 * hangs higher and further back, over the title rather than behind it,
 * because glowing right behind "ROC" it washed the letters out. It moves up
 * there at T_LOGO_IN while it's out of sight, and doesn't come back after. */
static mc_v3 blob_center(double t) {
    float pr = blob_presence(t), f = t > T_LOGO_IN ? 1.0f : 0.0f;
    return mc_vec3(3.0f * sinf((float)t * 0.05f),
                   -4.0f + 6.0f * pr + 12.5f * f,
                   -28.0f + 2.5f * cosf((float)t * 0.04f) - 14.0f * f);
}

static mc_camera camera_at(double t) {
    /* The camera flies a closed Catmull-Rom loop through these points, picked by
     * hand to vary the distance and height so it never feels like a turntable. */
    static const mc_v3 path[] = {
        { 0.0f, 1.2f,  4.0f}, { 6.0f, 3.0f,  6.0f}, { 2.0f, 6.0f, -7.0f},
        {-7.0f, 2.0f, -5.0f}, {-6.0f, 4.5f,  5.0f}, {-1.0f, 1.0f,  9.0f},
        { 5.0f, 2.0f,  7.0f}, { 8.0f, 5.0f, -2.0f},
    };
    mc_camera c = mc_camera_default();
    float mt = (float)motion_time(t);
    float u = (float)(motion_time(t) / motion_time(DUR));  /* re-paced per sequence */
    float ex = expansion(t);
    /* Hug the origin while it's just a dot, then pull back as things expand. */
    mc_v3 p = mc_spline_sample(path, MC_ARRAY_COUNT(path), mc_fractf(u), 1);
    p = mc_v3_scale(p, 0.35f + 0.65f * ex);
    p.y += 0.3f * sinf(mt * 0.5f);
    c.pos = p;
    c.target = mc_vec3(0.0f, 0.4f * sinf(mt * 0.3f), 0.0f);
    c.fovy = mc_radians(53.0f + 2.0f * sinf(mt * 0.15f));   /* gentle smooth breath, no beat punch */

    /* During the finale, lerp the free-flying camera toward a fixed head-on
     * shot of the logo, then hand it back once the logo is gone. */
    float cb = mc_smoothstepf((float)T_LOGO_IN + 1.0f, (float)T_LOGO_IN + 4.0f, (float)t)
             * (1.0f - mc_smoothstepf((float)T_LOGO_OUT - 1.0f, (float)T_LOGO_OUT + 1.0f, (float)t));
    if (cb > 0.001f) {
        mc_v3 eye = mc_vec3(0.0f, LOGO_Y, 13.5f);
        mc_v3 tgt = mc_vec3(0.0f, LOGO_Y + 1.0f, 0.0f);
        c.pos = mc_v3_lerp(c.pos, eye, cb);
        c.target = mc_v3_lerp(c.target, tgt, cb);
        c.fovy = mc_radians(52.0f);
    }

    /* The collapse pulls back from the seed until it's a star up to the
     * right, where Scene 2's sun will rise (uSunUV, within 3 px). The eye
     * ends up 25 m out, looking 15 deg down past it. */
    float cp = mc_smoothstepf((float)T_COLLAPSE, (float)T_BLACK, (float)t);
    if (cp > 0.0f) {
        mc_v3 eye = mc_vec3(-7.5f, 1.0f, 24.0f);
        c.pos = mc_v3_lerp(c.pos, eye, cp);
        c.target = mc_v3_lerp(c.target, mc_vec3(-7.5f, -5.0f, 1.5f), cp);
    }

    /* A jolt on the bang. The lens is knocked a few degrees off its aim, up
     * first, and shudders back over half a second. */
    float b = (float)(t - T_BANG), shake = b > 0.0f ? expf(-b * 7.0f) : 0.0f;
    c.target = mc_v3_mad(c.target, mc_vec3(sinf(b * 55.0f), cosf(b * 70.0f), 0.0f), 0.06f * shake * mc_v3_dist(c.pos, c.target));
    return c;
}

static void push_instance(Demo *d, int type, mc_m4 model, mc_v4 color) {
    int n = d->inst_count[type]++;
    d->inst[type][n].model = model;
    d->inst[type][n].color = color;
}

/* Where a body would be from orbit alone, before collisions nudge it. */
static mc_v3 body_orbit_pos(const Body *b, float mt, float ex) {
    float ang = mt * (0.15f + 0.1f * b->spin) + b->phase;
    mc_v3 dir = mc_quat_rotate(mc_quat_axis_angle(mc_vec3(0, 1, 0), ang), b->dir);
    mc_v3 pos = mc_v3_scale(dir, b->orbit * ex);
    pos.y += sinf(mt * 0.8f + b->phase) * 0.6f * ex;
    return pos;
}
static float body_radius(const Body *b, float gs) { return b->base_scale * gs * 0.42f; }
#define MIN_ALIVE 105   /* floor on live bodies - once we're this low, stop merging */

/* One tick of the collision sim, at the fixed SIM_DT.
 *
 * The orbits themselves stay analytic (body_orbit_pos + the expansion
 * envelope). On top of that, this keeps a small offset per body (poff/voff)
 * that a damped spring keeps pulling back to zero. A body that gets shoved
 * flies off and then eases home, and the scene never drifts into mush. When
 * two bodies overlap we roll a die: 25% they merge into one body of a new
 * random shape and colour, 25% they bounce apart, and the rest of the time
 * they pass through each other. */
static void sim_step(Demo *d, float dt, double t) {
    float ex = expansion(t), gs = body_scale(t), mt = (float)motion_time(t);
    float damp = expf(-dt * 1.6f);
    for (int i = 0; i < NBODY; ++i) {
        Body *b = &d->bodies[i];
        if (!b->alive) continue;
        b->voff = mc_v3_sub(b->voff, mc_v3_scale(b->poff, 3.0f * dt)); /* spring back toward the orbit */
        b->voff = mc_v3_scale(b->voff, damp);                          /* ...with damping, so it settles */
        b->poff = mc_v3_mad(b->poff, b->voff, dt);
        if (b->cool > 0.0f) b->cool -= dt;
        if (b->pop  > 0.0f) b->pop  -= dt;
    }

    /* Only look for collisions while the universe is actually open - no point
     * during the dot, the bang itself, or the collapse. */
    if (t < (double)T_BANG + 2.0 || t > (double)T_COLLAPSE) return;

    static mc_v3 wp[NBODY];
    static float rad[NBODY];
    for (int i = 0; i < NBODY; ++i) {
        if (!d->bodies[i].alive) continue;
        wp[i]  = mc_v3_add(body_orbit_pos(&d->bodies[i], mt, ex), d->bodies[i].poff);
        rad[i] = body_radius(&d->bodies[i], gs);
    }
    for (int i = 0; i < NBODY; ++i) {
        Body *A = &d->bodies[i];
        if (!A->alive || A->cool > 0.0f) continue;
        for (int j = i + 1; j < NBODY; ++j) {
            Body *B = &d->bodies[j];
            if (!B->alive || B->cool > 0.0f) continue;
            mc_v3 delta = mc_v3_sub(wp[i], wp[j]);
            float sr = rad[i] + rad[j];
            if (mc_v3_len2(delta) >= sr * sr) continue;   /* spheres don't overlap - skip */

            float roll = mc_rng_f(&d->crng);
            A->cool = B->cool = 1.6f;                      /* both just reacted; don't re-roll for a bit */
            if (roll < 0.25f && d->alive_count > MIN_ALIVE) { /* merge: A becomes something new, B vanishes */
                A->type  = (int)(mc_rng_next(&d->crng) % SH_COUNT);
                A->color = PALETTE[mc_rng_next(&d->crng) % PALETTE_N];
                A->pop   = 0.45f;
                B->alive = 0;
                d->alive_count--;
                break;                                     /* A is done for this tick */
            } else if (roll < 0.50f) {                     /* bounce: push them apart */
                mc_v3 n = mc_v3_normalize(delta);
                if (mc_v3_len2(n) < 0.5f) n = mc_vec3(0, 1, 0);  /* dead-centre overlap: pick any axis */
                A->voff = mc_v3_mad(A->voff, n, 6.0f);
                B->voff = mc_v3_sub(B->voff, mc_v3_scale(n, 6.0f));
            }
            /* the other 50%: nothing happens, they drift straight through */
        }
    }
}

/* Catch the sim up to `target` in fixed SIM_DT steps. Because the step size is
 * fixed, the result is the same whether we got here at 30fps or 300, which
 * keeps the real-time loop and the headless capture in agreement. The guard
 * stops a huge jump from pinning the CPU. */
static void sim_advance(Demo *d, double target) {
    int guard = 0;
    while (d->sim_t + SIM_DT <= target && guard++ < 200000) {
        d->sim_t += SIM_DT;
        sim_step(d, (float)SIM_DT, d->sim_t);
    }
}

/* Put the sim at an arbitrary time. Forwards is just sim_advance. Going
 * backwards, which the dev scrubber does when you drag the timeline left,
 * means replaying from the deterministic seed, since the sim only runs one
 * way. Windowed only: the headless builds always step forward. */
#if !defined(MC_HEADLESS) && !defined(MC_CAPTURE)
static void sim_seek(Demo *d, double t) {
    if (t + 1e-6 < d->sim_t) reseed_loop(d, d->loop);
    sim_advance(d, t);
}
#endif

/* One cube of a title (d->logo_*), ph (0..1) of the way in from its scattered
 * start. It tumbles until it lands and grows as it comes in. dy lifts it and
 * emi lights it. */
static void push_cube(Demo *d, int i, float ph, float dy, float emi) {
    float e  = mc_ease(MC_EASE_OUT_CUBIC, ph);
    mc_v3 pos = mc_v3_lerp(d->logo_start[i], d->logo_pos[i], e);
    pos.y += dy;
    float sc = LOGO_CELL * 0.95f * mc_smoothstepf(0.0f, 0.3f, ph);
    mc_quat q = mc_quat_axis_angle(mc_vec3(0.3f, 1.0f, 0.2f), (1.0f - e) * 4.0f);
    mc_v3 c = d->logo_col[i];
    push_instance(d, SH_BOX, mc_m4_trs(pos, q, mc_v3_splat(sc)), mc_vec4(c.x, c.y, c.z, emi));
}

/* The blob's two eyes. The body is drawn later (back-to-front needs it after
 * the opaque shapes), but the eyes go in with the shapes as ordinary little
 * flattened spheres, so whatever is in front of the blob hides them properly.
 * The blob has radius r and centre ctr, and is turned by q and squashed by sq
 * along its own axes. The eyes are k times their base size and set apart by
 * the same factor. The left eye is open by wl and the right by wr, where 1
 * is wide open and a sliver is shut. The wink shuts the right one.
 *
 * Each eye has a highlight: a white bead 0.22 of the eye across, sunk into
 * its front up and to the left so only its cap shows. That keeps it on the
 * eye from any side. Set on the front, it floated off the eye when seen from
 * below or the side. The bead shuts with the eye. As an eye shuts, a lid
 * grows round it, (1 - openness) x 0.22 of the eye high, unlit and a darker
 * blue than the body. A shut eye is then a dark lash line with a faint rim,
 * which still shows on the jelly's dark patches. A bare hairline got lost
 * there, and a pale, glowing lid looked like the white slits of a visor. The
 * wink closes the same way.
 *
 * The eyes sit low on the face and well out, clear of the body. Higher and
 * further in, their lower halves sank into it, and they looked like two sleepy
 * crescents under the skin on the end card and dull purple domes on the
 * title's lumps. */
static void blob_eyes(Demo *d, mc_v3 ctr, float r, mc_quat q, mc_v3 sq, float wl, float wr, float k) {
    float es = 0.26f * k * r;                            /* eyes scale with the blob */
    for (int s = -1; s <= 1; s += 2) {
        float w = s > 0 ? wr : wl;
        mc_v3 ep = mc_v3_add(ctr, mc_quat_rotate(q, mc_v3_mul(mc_vec3((float)s * 0.30f * k * r, 0.37f * r, 0.96f * r), sq)));
        for (int j = 0; j < 2; ++j)   /* the eye, its lid */
            push_instance(d, SH_ICO, mc_m4_trs(ep, q, mc_v3_mul(mc_vec3(es, es * (j ? 0.22f - 0.22f * w : w), es * (0.45f - 0.05f * (float)j)), sq)),
                          j ? mc_vec4(0.25f, 0.42f, 0.75f, 0.0f) : mc_vec4(0.02f, 0.02f, 0.05f, 0.0f));
        push_instance(d, SH_ICO, mc_m4_trs(mc_v3_add(ep, mc_quat_rotate(q, mc_v3_mul(mc_vec3(-0.25f * es, 0.25f * es * w, 0.27f * es), sq))),
                                           q, mc_v3_splat(0.22f * es * w)), mc_vec4(1.0f, 1.0f, 1.0f, 0.6f));
    }
}
/* The wink, w seconds in: the eye's height squashed to a sliver and back over
 * half a second, peaking shut at w = 0.22. */
static float wink(float w) {
    float k = (w > 0.0f && w < 0.5f) ? 1.0f - mc_absf(w - 0.22f) / 0.22f : 0.0f;
    return mc_lerpf(1.0f, 0.06f, mc_saturatef(k));
}

static void animate(Demo *d, double t) {
    for (int i = 0; i < SH_COUNT; ++i) d->inst_count[i] = 0;

    mc_timeline tl = {t, DUR, BPM};
    float ex = expansion(t);
    float gs = body_scale(t);
    float en = energy(t);
    float mt = (float)motion_time(t);   /* the warped clock, not raw t */
    /* A soft beat: a sine on the beat phase rather than a spike, so everything
     * it drives pulses musically instead of stuttering. */
    float sb = 0.5f + 0.5f * sinf((float)mc_beats(&tl) * MC_TAU);
    /* How assembled the logo is. The orbiting crowd shrinks away by this amount
     * so the letters have room to read. */
    float logo_vis = mc_smoothstepf((float)T_LOGO_IN + 1.0f, (float)T_LOGO_IN + 4.0f, (float)t)
                   * (1.0f - mc_smoothstepf((float)T_LOGO_OUT - 1.0f, (float)T_LOGO_OUT + 1.0f, (float)t));

    /* A camera-facing five-turn spiral gathers on the hook's downbeat. Only
     * the drawn positions change: collision offsets and the seekable sim keep
     * running underneath, ready for the smooth release before the title. */
    float form = mc_smoothstepf((float)T_FORM_IN, (float)T_HOOKB, (float)t)
               * (1.0f - mc_smoothstepf((float)T_FORM_OUT, (float)T_LOGO_IN, (float)t));
    mc_v3 form_center, form_right, form_up;
    if (form > 0.0f) {
        mc_camera c = camera_at(t);
        mc_v3 f = mc_v3_dir(c.target, c.pos);
        form_right = mc_v3_normalize(mc_v3_cross(f, mc_vec3(0.0f, 1.0f, 0.0f)));
        form_up = mc_v3_cross(form_right, f);
        form_center = mc_v3_mad(c.pos, f, 14.0f);
    }

    /* The seed is the bright little core that's all there is before the bang,
     * and it's what everything shrinks back into at the collapse. */
    float seed = 0.16f * (1.0f - mc_saturatef((float)((t - T_BANG) / 0.8)));
    if (t >= T_COLLAPSE)
        seed = mc_maxf(seed, 0.16f * mc_window_phase(&tl, T_COLLAPSE, T_BLACK - T_COLLAPSE));
    if (seed > 0.002f) {
        float s = seed * (1.0f + 0.12f * sb);
        mc_m4 model = mc_m4_trs(mc_vec3(0, 0, 0), mc_quat_axis_angle(mc_vec3(0, 1, 0), mt), mc_v3_splat(s));
        push_instance(d, SH_ICO, model, mc_vec4(1.0f, 0.92f, 0.78f, 1.0f));
    }

    for (int i = 0; i < NBODY; ++i) {
        Body *b = &d->bodies[i];
        /* skip it if a combine absorbed it, or if no body has a size yet
         * (before the bang) or any more (after the collapse). A zero-size shape
         * would still cost the depth and lit passes its thousands of vertices. */
        if (!b->alive || gs <= 0.0f) continue;

        /* final position = analytic orbit + whatever the collision sim nudged */
        mc_v3 pos = mc_v3_add(body_orbit_pos(b, mt, ex), b->poff);
        float breathe = 1.0f + 0.05f * sinf(mt * 1.4f + b->phase);
        float sc = b->base_scale * gs * breathe * (1.0f - 0.85f * logo_vis);
        /* fresh-merge flourish: a quick scale pop and emissive flash, 1 -> 0 */
        float popf = (b->pop > 0.0f) ? b->pop / 0.45f : 0.0f;
        sc *= 1.0f + 0.4f * popf;

        if (form > 0.0f) {
            /* Fixed birth indices leave holes for merged bodies without
             * reassigning survivors. sqrt distributes the slots almost
             * uniformly along the spiral: >0.60 apart for radius <0.29.
             * The urchins get a smaller scale because their spikes reach 1. */
            float r = sqrtf(1.0f + 35.0f * (float)i / NBODY);
            float a = r * MC_TAU + 0.6f * ((float)t - (float)T_HOOKB);
            mc_v3 p = mc_v3_mad(mc_v3_mad(form_center, form_right, r * cosf(a)), form_up, r * sinf(a));
            pos = mc_v3_lerp(pos, p, form);
            sc = mc_lerpf(sc, b->type == SH_SPIKE ? 0.28f : 0.40f, form);
        }

        mc_quat spin = mc_quat_axis_angle(b->axis, mt * b->spin + b->phase);
        mc_m4 model = mc_m4_trs(pos, spin, mc_v3_splat(sc));

        /* (the emission is negative, as build_bodies set it up, so object.frag
         * glows the crowd softer than the letters) */
        float emi = b->emissive * (0.45f + 0.55f * sb) * (0.5f + 0.5f * en) - popf * 0.8f;
        push_instance(d, b->type, model, mc_vec4(b->color.x, b->color.y, b->color.z, emi));
    }

    /* The logo. Each lit pixel is a cube that flies in from a scattered start
     * to its slot in the letters, staggered so they arrive as a swarm. */
    float la = mc_smoothstepf((float)T_LOGO_IN, (float)T_LOGO_IN + 4.0f, (float)t);
    if (t >= (double)T_LOGO_OUT)
        la *= 1.0f - mc_smoothstepf((float)T_LOGO_OUT, (float)T_LOGO_OUT + 3.0f, (float)t);
    /* Once it's complete, on the next downbeat (bar 19), a light sweeps across
     * it in one beat, left to right on a slant, and the cubes it passes flare
     * and hop a little. At rest the letters glow at 0.6. Much brighter and
     * they bloom to near white, and you can barely see their pastels. */
    float sw = ((float)t - (float)(T_HOOKB + 3 * BAR_S)) * 24.0f - 6.5f;
    if (la > 0.01f)
        for (int i = 0; i < d->logo_n; ++i) {
            float g = d->logo_pos[i].x + 0.5f * d->logo_pos[i].y - sw;
            g = expf(-g * g * 3.0f);
            push_cube(d, i, mc_saturatef(la * 1.4f - (float)i / (float)d->logo_n * 0.35f), 0.12f * g, 0.6f * la + 7.0f * g);
        }

    /* The blob's eyes, sized on the same growth curve as the body. The right
     * one winks once in the finale. They're the end card's eyes, 1.2 times the
     * base size. */
    float bp = blob_presence(t);
    if (bp > 0.45f)
        blob_eyes(d, blob_center(t), 7.0f * (0.7f + 0.3f * bp), mc_quat_identity(), mc_v3_splat(1.0f),
                  1.0f, wink((float)t - (float)T_WINK), 1.2f);
}

/* Scene 1. Build the matrices and the per-moment look, then lay down sky, grid,
 * bodies and blob, and hand it all to the post stack. Draw order matters here,
 * so read the comments on each pass before reordering. */

/* Helpers shared by the three scenes. camera_vp is the camera's
 * view-projection, jittered for the TAA. frame_basics fills in the Frame
 * block's per-frame basics. post_depth sets the depth terms the AO rebuilds
 * view positions from, and the sun direction its contact shadows march
 * toward. shadow_begin starts a sun depth pass: it uploads the Frame block as
 * seen from the light (every caster's vertex shader reads uViewProj), then
 * binds and clears the depth target. shadow_end puts the camera back. */
static mc_m4 camera_vp(Demo *d, const mc_camera *c) {
    return mc_renderer_camera(d->R, mc_camera_proj(c, mc_renderer_aspect(d->R)), mc_camera_view(c));
}
static void frame_basics(Demo *d, mc_m4 vp, mc_v3 eye, float t) {
    FrameUniforms *F = &d->frame;
    int rw, rh;
    mc_renderer_size(d->R, &rw, &rh);
    F->view_proj = vp;
    F->cam       = eye;
    F->time      = t;
    F->res       = mc_vec2((float)rw, (float)rh);
}
static void post_depth(Demo *d, const mc_camera *c, mc_post *p) {
    p->znear = c->znear;
    p->zfar  = c->zfar;
    p->projy = tanf(c->fovy * 0.5f);
    p->projx = p->projy * mc_renderer_aspect(d->R);
    p->sun_dir = d->frame.light_dir;
}
static void shadow_begin(Demo *d, const mc_target *t) {
    FrameUniforms *F = &d->frame;
    F->view_proj = F->light_vp;
    mc_gl_frame_block(F, sizeof *F);
    MC_PASS(MC_P_SHADOW);
    mc_target_bind(t);
    mc_gl_switch(MC_ZTEST_ON | MC_ZWRITE_ON | MC_CULL_ON);
    glCullFace(GL_BACK);
    glClear(GL_DEPTH_BUFFER_BIT);
}
static void shadow_end(Demo *d, mc_m4 camera) {
    mc_renderer_bind_scene(d->R);
    MC_PASS(MC_P_SCENE);
    d->frame.view_proj = camera;
}

/* Scene 1's shapes, one instanced draw call per shape type. The solids are
 * drawn in passes 0 (the key light's depth map, with shadowsh) and 1, and the
 * glass in passes 2 and 3 (the depth map). Every pass but 0 goes through
 * object.frag. Each type's glass (red below 0, see build_bodies) is first
 * sorted to the end of its list, and the function returns how many there are.
 * Pass 0 is always the frame's first (s1_draw) and uploads the lists. The
 * later passes' sorts only shuffle the glass among itself, so the solids'
 * count, which is where each range starts, stays the same. (shadowsh is
 * Scene 1's alone. Scene 2's seabed casts no shadow, so nothing there reads
 * the uK that pass 0 leaves behind.) */
static int s1_shapes(Demo *d, int pass) {
    int glass = 0;
    mc_shader_use(pass ? d->obj : d->shadowsh);
    for (int s = 0; s < SH_COUNT; ++s) {
        mc_instance *in = d->inst[s], x;
        int n = d->inst_count[s], g = n;
        for (int i = 0; i < g;)
            if (in[i].color.x < 0.0f) x = in[i], in[i] = in[--g], in[g] = x;
            else ++i;
        glass += n - g;
        /* these balls are finer than Scene 2's bubbles and specks, which share
         * the mesh: uK 7 and four times the vertices (object.vert) */
        mc_gpu_mesh m = d->shapes[s];
        if (!s) m.vertex_count *= 4;
        mc_set_i(U_K, s ? s + 1 : 7);
        if (!pass) mc_mesh_set_instances(&d->shapes[s], in, n);
        mc_mesh_draw_instance_range(m, pass > 1 ? g : 0, pass > 1 ? n - g : g);
    }
    return glass;
}
static void jelly_copy(Demo *d, mc_v3 p, float r);
static void bind_uw_textures(Demo *d);
static mc_m4 jl_sun_vp(mc_v3 sun, mc_v3 at, mc_v3 p, float r);   /* (s3_light.c) */
/* Scene 1's world, which the end card shares. First comes the key light's
 * shadow map of the shapes that animate (or the end card) queued. Then the
 * Frame block for camera c at time t, with the act's sky colours (atop, abot)
 * brightened by energy en, the origin glow and the grid. Then it draws the
 * sky, the grid, the shapes and the blob, which is present by a and lumpy by
 * wob. The caller adds the post. */
static void s1_draw(Demo *d, const mc_camera *c, float t, float en, mc_v3 atop, mc_v3 abot,
                    float glowamt, float grid_fade, mc_m4 blob, float a, float wob) {
    mc_v3 sky_top    = mc_v3_scale(atop, 0.30f + 0.22f * en);
    mc_v3 sky_bottom = mc_v3_scale(abot, 0.7f + 0.5f * en);

    /* The key light circles slowly overhead and casts the shapes' shadows.
     * Its depth pass comes first: an orthographic 32 m box round the
     * origin, where every shape and letter lives (Scene 3's jl_sun_vp makes
     * it). It renders into Scene 2's sun map, since the two scenes never share
     * a frame, and object.frag reads it on unit 4 (bind_uw_textures).
     * mc_renderer_begin hands the target back. */
    FrameUniforms *F = &d->frame;
    F->light_dir    = mc_v3_normalize(mc_vec3(cosf(t * 0.2f), 0.8f, sinf(t * 0.2f)));
    F->light_vp     = jl_sun_vp(F->light_dir, mc_vec3(0.0f, 0.0f, 0.0f), mc_vec3(0.0f, 0.0f, 0.0f), 16.0f);
    shadow_begin(d, &d->uw_shadow);
    s1_shapes(d, 0);
    s1_shapes(d, 3);

    /* Everything the shaders need this frame, in one upload. */
    frame_basics(d, camera_vp(d, c), c->pos, t);
    F->fog_color    = sky_bottom;
    F->fog          = F->blob_fog = 0.012f + 0.01f * (1.0f - en);
    F->sky_color    = mc_v3_add(sky_top, mc_v3_splat(0.1f));
    F->ground_color = mc_v3_scale(sky_bottom, 1.5f);
    F->top          = sky_top;
    F->bottom       = sky_bottom;
    F->glow         = mc_vec3(1.0f, 0.85f, 0.65f);            /* (scene 3 makes it its sun) */
    F->glow_amt     = glowamt;
    F->grid_fade    = grid_fade;
    F->rim          = mc_v3_clamp(mc_v3_mad(mc_v3_splat(0.4f), d->blob_color, 0.6f), 0.0f, 1.0f);
    F->wobble       = wob;
    mc_gl_frame_block(F, sizeof *F);

    mc_renderer_begin(d->R, sky_bottom);
    bind_uw_textures(d);

    /* Sky first, as a background: depth test and writes off so it never
     * occludes anything and everything draws over it. */
    mc_gl_switch(MC_ZTEST_OFF | MC_ZWRITE_OFF);
    mc_shader_use(d->sky);
    mc_fullscreen_triangle();

    mc_gl_switch(MC_ZTEST_ON | MC_ZWRITE_ON);

    if (grid_fade > 0.01f || F->atmo * (1.0f - F->atmo) > 0.0f) {   /* (or the bang's shock runs over it) */
        /* additive, depth-tested but with no depth write. Bodies hide it, but
         * it doesn't punch holes in the depth buffer for the blob later. */
        mc_gl_switch(MC_ZWRITE_OFF | MC_BLEND_ON);
        glBlendFunc(GL_ONE, GL_ONE);
        mc_shader_use(d->grnd);
        mc_set_m4(U_MODEL, mc_m4_translate(mc_vec3(0.0f, -4.5f, 0.0f)));
        mc_mesh_draw(d->ground);
        mc_gl_switch(MC_ZWRITE_ON | MC_BLEND_OFF);
    }

    /* The bodies, one instanced draw call per shape type: the solids now, the
     * glass at the end. */
    int glass = s1_shapes(d, 1);

    /* The blob, drawn after the solids so the ones in front of it win. Its
     * opacity is a, through constant-alpha blending, and like the grid it's
     * depth-tested but doesn't write depth, so near geometry still hides it. */
    if (a > 0.01f) {
        mc_v3 bc = d->blob_color;                               /* loop 0's blue; rim in F */
        mc_instance mi = { blob, mc_vec4(bc.x, bc.y, bc.z, 0.0f) };
        mc_shader_use(d->blobsh);
        mc_gl_switch(MC_ZWRITE_OFF | MC_BLEND_ON);
        glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
        glBlendColor(0.0f, 0.0f, 0.0f, a);
        mc_mesh_set_instances(&d->blob, &mi, 1);
        mc_mesh_draw_instanced(d->blob, 1);
        mc_gl_switch(MC_ZWRITE_ON | MC_BLEND_OFF);
    }

    /* The glass bodies, last: like Scene 2's jelly they look through a copy of
     * the picture drawn so far (unit 9), the blob and the solids behind them
     * included. */
    if (glass) {
        jelly_copy(d, c->pos, 0.0f);
        s1_shapes(d, 2);
    }
}

static void render_scene1(Demo *d, mc_window *w, double t) {
    int ow, oh;
    mc_window_size(w, &ow, &oh);

    mc_timeline tl = {t, DUR, BPM};
    float en = energy(t);
    float sb = 0.5f + 0.5f * sinf((float)mc_beats(&tl) * MC_TAU);  /* smooth beat */

    animate(d, t);

    mc_camera cam = camera_at(t);

    /* The look for this instant: sky colours from the current act, brightened by
     * energy, plus the origin glow. */
    mc_v3 atop, abot;
    atmosphere(t, &atop, &abot);
    /* the origin flares on the bang and settles in under a second, so the
     * crowd's colours show within half a second. With a 3 s fade, the whole
     * frame stayed a beige wash and the crowd milky long after the shock. */
    float glowamt = 1.4f * (1.0f - mc_saturatef((float)((t - T_BANG) / 0.8f))) + 0.25f + 0.35f * sb * en;
    if (t >= T_COLLAPSE) {
        /* one last flare on the collapse, eased up and back down so it swells
         * rather than blowing out to white */
        float cp = mc_window_phase(&tl, T_COLLAPSE, T_BLACK - T_COLLAPSE);
        glowamt += 0.5f * mc_ease(MC_EASE_OUT_QUAD, cp) * (1.0f - 0.6f * cp);
    }
    /* pull the glow down while the logo is up, or it washes the letters out.
     * The finale's sun (sky.frag) rises behind the logo as it gathers and sets
     * as it scatters. */
    float lv = mc_smoothstepf((float)T_LOGO_IN + 1.0f, (float)T_LOGO_IN + 4.0f, (float)t)
             * (1.0f - mc_smoothstepf((float)T_LOGO_OUT - 1.0f, (float)T_LOGO_OUT + 1.0f, (float)t));
    glowamt *= (1.0f - 0.9f * lv);
    d->frame.sun = mc_vec3(0.32f * lv - 0.24f, lv, 0.0f);

    /* The synthwave floor grid: fades in a few seconds after the bang and back
     * out at the collapse. */
    float grid_fade = mc_smoothstepf((float)T_BANG + 2.0f, (float)T_BANG + 8.0f, (float)t) * en;
    if (t >= T_COLLAPSE) grid_fade *= (1.0f - mc_window_phase(&tl, T_COLLAPSE, T_BLACK - T_COLLAPSE));

    /* uAtmo does two jobs here. Going 0 -> 1 in 0.4 s from the kick, it's the
     * bang's shock front running out over the sky and the floor (sky.frag,
     * ground.frag). Going 1 -> 2 over the 3 s before T_BLACK, it's the outro
     * taking the world down to black (sky.frag), all except the seed, which
     * object.frag draws. The end card keeps the 1 that Scene 2 leaves there,
     * so it gets no shock and no fade. */
    d->frame.atmo = mc_saturatef((float)(t - T_BANG) * 2.5f) + mc_smoothstepf((float)T_BLACK - 3.0f, (float)T_BLACK, (float)t);

    /* The blob, swelling in by its presence, with a gentle wobble. */
    float mp = blob_presence(t), mt = (float)motion_time(t);
    s1_draw(d, &cam, (float)t, en, atop, abot, glowamt, grid_fade,
            mc_m4_trs(blob_center(t), mc_quat_axis_angle(mc_vec3(0.0f, 1.0f, 0.1f), mt * 0.05f),
                      mc_v3_splat(7.0f * (1.0f + 0.06f * sinf(mt * 0.9f)) * (0.7f + 0.3f * mp))),
            mp * 1.15f, 1.0f);   /* scene 1: full wobble; all but opaque at its fullest (0.85 presence) */

    /* Post. Start from the defaults and push the knobs around with the beat,
     * the energy and the big moments. */
    mc_post post = mc_post_defaults();
    post.time = (float)t;
    post.bloom = 0.07f + 0.04f * sb * en + 0.25f * (1.0f - mc_saturatef((float)((t - T_BANG) / 1.0f)));
    /* The bang is a kick of the exposure before the tonemap, so the seed and
     * the burst flare while the dark only lifts a little. The flash itself is
     * the shock's rim sweeping the frame (sky.frag). A white flash after the
     * tonemap looked like a strobe: a grey veil over the dark for a few frames
     * before the ring brought the light, then dimmer, then blown out. */
    post.exposure = 1.05f + 0.15f * en;
    if (t >= T_BANG) post.exposure += 1.5f * expf(-(float)(t - T_BANG) * 12.0f);
    /* Fade up from black at the start. At the end, the world has gone black
     * by T_BLACK (uAtmo, above), all except the seed, which by then is a star
     * up to the right (camera_at). The seed lasts a beat longer and is going
     * out as the scene cuts to Scene 2, whose sun rises in its place. */
    post.fade = mc_smoothstepf(0.0f, 2.5f, (float)t)
              * (1.0f - mc_smoothstepf((float)(T_BLACK + 60.0 / BPM), (float)DUR + 0.3f, (float)t));
    /* while the blob's around, lift the bloom and ease off the vignette for a
     * softer frame */
    post.bloom     += 0.05f * mp;
    post.vignette   = mc_maxf(0.0f, post.vignette - 0.08f * mp);
    mc_renderer_end(d->R, mc_window_output_fbo(w), ow, oh, post);
}

/* The end card: 5 bars, coming out of scene 3's black. It spells "HELLO
 * MICROCOSMIC WORLD" on three lines in scene 1's cubes, over scene 1's sky
 * and grid in the finale's violet. The cubes fly together on the first beats.
 * The blob drops in from above and bounces along the words karaoke-style, one
 * syllable a beat from beat 4. Each landing squashes the blob and makes the
 * letters under it dip and flash, and they stay lit once sung. Then it
 * crouches and somersaults up onto the middle of HELLO on beat 14, and a flash
 * runs out through all the letters. It puffs up and winks, and the card fades
 * out over its last three beats as the music resolves (synth.c). Beats count
 * from the card's start. Hop k lands on syllable k at END_LAND(k), and the
 * last hop, the somersault, lands on top of HELLO. Each hop takes off 0.3 of
 * a beat after the one before it landed. */
#define END_LAND(k) ((k) < END_SYL ? 4.0f + (float)(k) : 14.0f)
#define END_R       0.7f        /* the blob's radius here: about a letter's height across */

static void render_end(Demo *d, mc_window *w, double t) {
    int ow, oh;
    mc_window_size(w, &ow, &oh);
    float b = (float)(t * (BPM / 60.0));                         /* beats into the card */
    float sb = 0.5f + 0.5f * sinf(b * MC_TAU);                   /* smooth beat */

    /* The hop under way (or the last one, once it has landed). It starts
     * where the one before landed; the first drops from 7 m above, starting on
     * beat 2. p is how far through the hop we are, and a is beats since the
     * latest landing. The path is a parabola whose top is h above the straight
     * line, arcing toward the camera. The blob stretches in the air and
     * squashes through each landing, and its bottom stays on the letters. */
    int k = 0;
    while (k < END_SYL && b >= END_LAND(k)) ++k;
    float l0 = k ? END_LAND(k - 1) : 2.0f, l1 = END_LAND(k);
    float p = mc_saturatef((b - l0 - 0.3f) / (l1 - l0 - 0.3f)), a = b - (b < l1 ? l0 : l1);
    mc_v3 p1 = k < END_SYL ? d->syl[k] : mc_vec3(0.0f, d->syl[0].y, 0.0f);
    mc_v3 p0 = k ? d->syl[k - 1] : mc_v3_add(p1, mc_vec3(0.0f, 7.0f, 0.0f));
    float h  = !k ? 1.75f : k == END_SYL ? 3.0f : 0.5f + 0.12f * mc_absf(p1.x - p0.x);
    float sy = 1.0f + 0.15f * sinf(MC_PI * p) - 0.3f * sinf(MC_PI * mc_saturatef(a / 0.3f));
    float r  = END_R * (1.0f + 0.5f * mc_smoothstepf(14.3f, 15.0f, b));    /* it puffs up at the top */
    mc_v3 sq = mc_vec3(1.5f - 0.5f * sy, sy, 1.5f - 0.5f * sy);
    mc_v3 ctr = mc_v3_lerp(p0, p1, p);
    ctr.y += 4.0f * p * (1.0f - p) * h + r * sy;
    ctr.z  = 0.3f + 3.2f * p * (1.0f - p);
    mc_quat q = mc_quat_axis_angle(mc_vec3(1.0f, 0.0f, 0.0f), k == END_SYL ? MC_TAU * p : 0.0f);

    for (int i = 0; i < SH_COUNT; ++i) d->inst_count[i] = 0;
    /* the letters fly in, staggered, by beat 3. Each syllable dips and
     * flashes as the blob lands on it, and stays lit after. The last landing's
     * flash runs out through them all. */
    float la = mc_smoothstepf(0.0f, 3.0f, b);
    for (int i = d->logo_n; i < d->text_n; ++i) {
        float ai = b - END_LAND(d->logo_syl[i]);
        float aw = b - 14.0f - 0.1f * mc_v3_dist(d->logo_pos[i], p1);
        push_cube(d, i, mc_saturatef(la * 1.4f - (float)(i - d->logo_n) / (float)(d->text_n - d->logo_n) * 0.35f),
                  -0.1f * sinf(MC_PI * mc_saturatef(ai / 0.3f)),
                  la * (ai > 0.0f ? 0.45f : 0.12f) + 1.5f * (ai > 0.0f ? expf(-3.0f * ai) : 0.0f)
                     + 1.5f * (aw > 0.0f ? expf(-3.0f * aw) : 0.0f));
    }
    blob_eyes(d, ctr, r, q, sq, 1.0f, wink((b - 15.2f) * (float)(60.0 / BPM)), 1.2f);   /* big and bright, as on the beach */

    /* a head-on shot, easing in a little */
    mc_camera cam = mc_camera_default();
    cam.pos    = mc_vec3(0.4f * sinf(b * 0.15f), LOGO_Y + 1.0f, 15.5f - 0.1f * b);
    cam.target = mc_vec3(0.0f, LOGO_Y + 1.0f, 0.0f);
    cam.fovy   = mc_radians(44.0f);
    d->frame.atmo = 1.0f;   /* uAtmo: no shock, no outro dark (scene 2 leaves 1; a seek may not) */
    d->frame.sun  = mc_vec3(0.1f - 0.012f * b, 1.0f, 0.0f);   /* the finale's sun, setting (sky.frag) */
    s1_draw(d, &cam, (float)t, 1.0f, mc_v3_scale(SEQ_TOP[NSEQ - 1], 0.55f), mc_v3_scale(SEQ_BOT[NSEQ - 1], 0.8f),
            0.04f + 0.04f * sb, 1.0f,
            mc_m4_trs(ctr, q, mc_v3_scale(sq, r)), 1.0f, 0.15f);       /* rounded, as scene 2 left it; opaque, glossy (blob.frag) */

    mc_post post = mc_post_defaults();
    post.time     = (float)t;
    post.bloom    = 0.09f + 0.03f * sb;
    post.exposure = 1.1f;
    post.vignette -= 0.08f;
    post.fade     = mc_smoothstepf(0.0f, 1.0f, b) * (1.0f - mc_smoothstepf(17.0f, 20.0f, b));
    mc_renderer_end(d->R, mc_window_output_fbo(w), ow, oh, post);
}

/* Scene 2: orbit -> planet -> sea -> the blob, with its own world and camera.
 * It has two coordinate frames, cut between on a bright flash so the seam
 * never shows. The planet frame (a sphere at the origin, camera diving in from
 * far off) runs up to the cloud break. The flat-sea frame (plane at y=0,
 * camera falling onto it and then sinking below) takes the splash and the
 * deep. */

/* A stretched clock. An act choreographed on a story clock, in bars, plays
 * stretched, with each part spread over more bars of the show. seg lists the
 * parts in order as pairs (bars of the show, bars of the story), ending in 0,
 * and the last pace holds on after that. stretch() turns bars of the show into
 * the act into bars of its story. Each change of pace is eased over half a
 * bar, and the story clock is the integral of its eased pace, so no speed on
 * screen ever jumps. A quarter bar away from the changes, each part's bar
 * lines match its story's, so whatever is locked to the music stays locked. */
static float stretch(float y, const unsigned char *seg) {
    float r = (float)seg[1] / seg[0], x = r * y, at = 0.0f;
    for (; *seg; seg += 2) {
        float d = (float)seg[1] / seg[0] - r, z = y - at + 0.25f;
        x += d * (z < 0.0f ? 0.0f : z < 0.5f ? z * z : z - 0.25f);
        r += d;
        at += seg[0];
    }
    return x;
}
/* The underwater act's parts, split at the music's section changes: the deep
 * (8 bars for its 7), the clearing (7 for 6), the anthem up to the predator
 * (9 for 8), the stalk and the chase (6 for 6, so the heartbeat stays on the
 * near miss), the transformation (5 for 4), and the flight and the burn-out
 * (9, unstretched). */
static const unsigned char S2_SEG[] = { 8, 7, 7, 6, 9, 8, 6, 6, 5, 4, 1, 1, 0 };
static float  uw_u(double t) { return stretch((float)((t - S2_BREACH) / BAR_S), S2_SEG) * UW_BAR; }   /* scene time -> story */
/* ... and back: bars of the story to bars of the show (by bisection) */
static float unstretch(float x, const unsigned char *seg) {
    float lo = -8.0f, hi = 64.0f;
    for (int i = 0; i < 30; ++i) {
        float m = 0.5f * (lo + hi);
        if (stretch(m, seg) < x) lo = m; else hi = m;
    }
    return lo;
}
static double uw_t(float u)  { return S2_BREACH + unstretch(u / UW_BAR, S2_SEG) * BAR_S; }           /* story -> scene time */
static mc_v3 blob2_center(double t);   /* fwd decl: the blob's travelling position */
static void  blob_frame(float u, mc_v3 *fwd, mc_v3 *right);

/* The encounter camera's frame: the blob's heading averaged over the last
 * second, so the hairpin its panicked bolt makes swings the camera round gently. */
static void enc_frame(float u, mc_v3 *fwd, mc_v3 *right) {
    mc_v3 sum = mc_vec3(0.0f, 0.0f, 0.0f), f, r;
    for (int i = 0; i < 4; ++i) {
        blob_frame(u - 0.35f * (float)i, &f, &r);
        sum = mc_v3_add(sum, f);
    }
    if (mc_v3_len2(sum) < 1e-6f) sum = f;
    *fwd = mc_v3_normalize(sum);
    *right = mc_v3_cross(*fwd, mc_vec3(0, 1, 0));
}
/* Where the encounter camera sits relative to the blob: sw = 0 while the
 * predator stalks (back on its side, at eye level), 1 for the high side-on
 * chase shot that frames the blob and its pursuer. */
static mc_v3 enc_offset(mc_v3 f, mc_v3 r, float sw) {
    float phi = mc_lerpf(1.0f, 1.3f, sw), rad = mc_lerpf(17.0f, 15.0f, sw);
    mc_v3 dir = mc_v3_mad(mc_v3_scale(f, -cosf(phi)), r, sinf(phi) * PRED_SIDE);
    return mc_v3_mad(mc_vec3(0.0f, mc_lerpf(1.0f, 9.0f, sw), 0.0f), dir, rad);
}

/* The swell's height along the line x = 0 at scene time t. The camera falls
 * along that line, and it's one of the ocean grid's lines, so the surface
 * drawn there is a straight blend of the two vertices either side. This
 * evaluates ocean.vert's sines (x 1.1) at those two and blends them. */
static float sea_level(float z, float t) {
    const float g = 900.0f / 140.0f;
    float h[2], f = (z + 450.0f) / g, k = floorf(f);
    for (int i = 0; i < 2; ++i) {
        float y = (k + (float)i) * g - 450.0f;
        h[i] = 1.1f * (sinf(t) + 0.55f * sinf(0.224f * y + 1.5f * t) + 0.3025f * sinf(0.30224f * y + 2.1f * t)
                       + 0.166375f * sinf(2.9f * t - 0.92738f * y));
    }
    return mc_lerpf(h[0], h[1], f - k);
}

/* Above the sea, from the cloud deck to the moment the lens goes under. It's
 * a long fall that slows as it comes down and levels out over the swell, then
 * glides in to skim the water at S2_BREACH, when the waterline sweeps up the
 * lens. Over its last few metres it rides the waves' rise and fall, ending
 * about 12 cm above the surface drawn there. */
static mc_camera camera_sea(double t) {
    mc_camera c = mc_camera_default();
    float u = (float)(t - S2_CLOUDFLASH) / (S2_BREACH - S2_CLOUDFLASH);
    float hgt = fmaxf(55.0f * (1.0f - u) * (1.0f - 0.74f * u), 0.12f), z = (22.0f + 8.0f * u) * u;   /* (the sea above stays above it) */
    float pitch = mc_radians(-41.0f + 34.0f * mc_smoothstepf(0.35f, 1.0f, u));
    c.pos = mc_vec3(0.0f, hgt + sea_level(z, (float)t) * (1.0f - mc_smoothstepf(0.0f, 6.0f, hgt)), z);
    c.target = mc_v3_add(c.pos, mc_vec3(0.0f, sinf(pitch), cosf(pitch)));
    c.fovy = mc_radians(62.0f);
    c.zfar = 1200.0f;
    return c;
}

static mc_camera camera_at2(double t) {
    mc_camera c = mc_camera_default();
    if (t < (double)S2_CLOUDFLASH) {
        /* planet frame: drift in slowly, then accelerate straight down at it */
        float drift = (float)t * 0.03f;
        mc_v3 dir = mc_v3_normalize(mc_vec3(0.25f + 0.10f * sinf(drift),
                                            0.18f + 0.05f * cosf(drift * 0.7f), 1.0f));
        float dist, wide = 0.0f;
        if (t < (double)S2_SPACE_END) {
            float u = (float)t / S2_SPACE_END;
            dist = mc_lerpf(150.0f, 110.0f, mc_ease(MC_EASE_INOUT_SINE, u));
        } else {
            float u = (float)(t - S2_SPACE_END) / (S2_CLOUDFLASH - S2_SPACE_END);
            dist = mc_lerpf(110.0f, PLANET_R + 2.5f, mc_ease(MC_EASE_IN_CUBIC, u));
            wide = u;                                   /* widen the lens on the plunge */
        }
        c.pos = mc_v3_scale(dir, dist);
        c.target = mc_vec3(0, 0, 0);
        c.fovy = mc_radians(50.0f + 10.0f * wide);
        c.znear = 0.3f; c.zfar = 500.0f;
    } else if (t < (double)S2_BREACH) {
        c = camera_sea(t);                              /* sea frame, above water */
    } else {
        /* underwater: trail the wandering blob from behind its direction of
         * travel, at a distance that drifts gently in and out, a touch above
         * and framed a little ahead. As the blob turns, we follow it. */
        float u = uw_u(t);
        mc_v3 b0 = blob2_center(t);
        mc_v3 fwd, r;   /* level, or where the blob nearly stops at the far end of a loop,
                         * its bob would swing the trail up to 11 m above it */
        blob_frame(u, &fwd, &r);
        /* the flight pulls the camera back and widens the lens, so the speed-up
         * shows more of the scenery */
        float fw = mc_smoothstepf(FLIGHT_IN, FLIGHT_IN + 5.0f, u);
        float dist = 11.0f + 3.0f * sinf(u * 0.3f) + 7.0f * fw;
        c.pos = mc_v3_add(b0, mc_v3_mad(mc_vec3(0.0f, 3.0f + 2.5f * fw, 0.0f), fwd, -dist));
        c.target = mc_v3_mad(b0, fwd, 3.0f);
        c.fovy = mc_radians(55.0f + 10.0f * fw);
        c.znear = 0.1f; c.zfar = 400.0f;
        /* Now and then, swing the trail off to one side instead of sitting
         * straight behind, so we catch the blob at a 3/4 angle (and, once it has
         * them, its eyes). It's gentle and occasional on the early journey and
         * stronger during the flight, and off through the scare and the
         * transformation orbit, which own the camera. */
        {
            mc_v3 right = mc_v3_cross(fwd, mc_vec3(0.0f, 1.0f, 0.0f));
            if (mc_v3_len2(right) < 1e-5f) right = mc_vec3(1.0f, 0.0f, 0.0f);
            right = mc_v3_normalize(right);
            float early      = 1.0f - mc_smoothstepf(ENC_IN - 8.0f, ENC_IN, u);  /* 1 before the scare */
            float occasional = mc_smoothstepf(0.40f, 0.90f, sinf(u * 0.13f + 2.0f)); /* comes and goes */
            float sway       = sinf(u * 0.40f + 0.5f);                            /* which side, how far */
            float side       = sway * (early * occasional * 3.0f + fw * 5.0f);
            c.pos    = mc_v3_mad(c.pos, right, side);
            c.target = mc_v3_mad(c.target, right, side * 0.2f);
        }
        /* The encounter. The camera pulls back to the predator's side of the
         * unaware blob, framing the open water ahead that the predator will
         * come out of. It holds steady while the lens slowly tightens as the
         * predator closes in, and the near miss jolts it. Then, as the blob
         * bolts (its path whips round in a tight hairpin), the camera cranes up
         * into a high side-on chase shot, looking down between the blob and the
         * predator turning after it. Its frame follows the blob's heading
         * smoothed over a second, so the hairpin turns it gently. (Angles: 0 is
         * straight behind the blob, swinging toward the predator's side.) */
        float enc = mc_smoothstepf(ENC_IN - 1.5f, ENC_IN + 2.5f, u)
                  * (1.0f - mc_smoothstepf(TRANS_IN + 2.0f, TRANS_IN + 2.2f, u));
        if (enc > 0.001f) {
            mc_v3 f, right;
            enc_frame(u, &f, &right);
            float sw   = mc_smoothstepf(PANIC_IN + 0.15f, PANIC_IN + 1.8f, u);   /* stalk -> chase */
            mc_v3 epos = mc_v3_add(b0, enc_offset(f, right, sw));
            mc_v3 etgt = mc_v3_add(b0, mc_v3_lerp(mc_v3_mad(mc_vec3(0.0f, 2.0f, 0.0f), f, 6.0f),
                                                  mc_v3_scale(f, -5.5f), sw));
            float tight = mc_smoothstepf(ENC_IN, ENC_PEAK - 0.6f, u);
            float efov  = mc_lerpf(mc_lerpf(50.0f, 45.0f, tight), 60.0f, sw);
            c.pos    = mc_v3_lerp(c.pos, epos, enc);
            c.target = mc_v3_lerp(c.target, etgt, enc);
            c.fovy   = mc_radians(mc_lerpf(mc_degrees(c.fovy), efov, enc));
            /* the blow of the near miss, then a panicked tremble while it flees */
            float hit = expf(-(u - ENC_PEAK) * (u - ENC_PEAK) * 9.0f);
            float amp = 1.1f * hit + 0.32f * mc_smoothstepf(ENC_PEAK, PANIC_IN + 0.3f, u)
                                           * (1.0f - mc_smoothstepf(PANIC_IN + 2.5f, TRANS_IN, u));
            mc_v3 jit = mc_vec3(sinf(u * 37.0f) + 0.6f * sinf(u * 71.0f),
                                sinf(u * 41.0f + 1.3f),
                                sinf(u * 43.0f + 2.1f) + 0.6f * sinf(u * 67.0f));
            c.pos    = mc_v3_mad(c.pos, jit, amp * enc);
            c.target = mc_v3_mad(c.target, jit, amp * 0.4f * enc);
            c.fovy  += mc_radians(9.0f) * hit * enc;   /* a lurch wider on the blow */
        }
        /* The transformation. The camera breaks off and circles the blob,
         * which is drifting slowly now, while its eyes grow and it rounds out,
         * and then eases back behind it for the flight. It swings from the
         * bearing where the chase shot left it round to the bearing the trail
         * takes at TRANS_END, easing in and out. That's 0.5-1.5 laps (0.88
         * here). With 1-2 laps the reef whirled past at ~50 px a frame. The
         * pose is blended round the blob in bearing, distance and height,
         * because a straight-line blend cuts a chord through the swing and
         * swoops in until the blob fills the frame. */
        float tw = mc_smoothstepf(TRANS_IN, TRANS_IN + 2.0f, u)
                 * (1.0f - mc_smoothstepf(TRANS_END - 2.5f, TRANS_END, u));
        if (tw > 0.001f) {
            mc_v3 f0, r0, q = mc_v3_sub(c.pos, b0);
            enc_frame(TRANS_IN, &f0, &r0);
            mc_v3 d0 = enc_offset(f0, r0, 1.0f);
            blob_frame(TRANS_END, &f0, &r0);
            float a0 = atan2f(d0.z, d0.x), a1 = atan2f(q.z, q.x), ang = atan2f(-f0.z, -f0.x) - a0 - MC_PI;
            ang = a0 + (ang - MC_TAU * floorf(ang / MC_TAU) + MC_PI) * mc_smoothstepf(TRANS_IN, TRANS_END, u) - a1;
            ang = a1 + tw * (ang - MC_TAU * floorf(ang / MC_TAU + 0.5f));
            float h = mc_lerpf(sqrtf(q.x * q.x + q.z * q.z), 8.5f, tw);
            c.pos    = mc_v3_add(b0, mc_vec3(h * cosf(ang), mc_lerpf(q.y, 2.8f, tw), h * sinf(ang)));
            c.target = mc_v3_lerp(c.target, b0, tw);
            c.fovy   = mc_lerpf(c.fovy, mc_radians(50.0f), tw);
        }
    }
    return c;
}

/* The whale reveal: take five story seconds to lift the gaze, hold for the
 * mother and calf, then settle back over four. The slow return also keeps
 * the window's bright rim from flashing past. */
static float uw_look(float u) {
    return mc_smoothstepf(UW_LOOK - 5.0f, UW_LOOK, u) * (1.0f - mc_smoothstepf(UW_LOOK + 2.0f, UW_LOOK + 6.0f, u));
}

/* The underwater camera as the picture uses it. The props are placed against
 * it too (uw_world's samples): the boulders keep clear of it, and every prop
 * grows in out of its sight. Measured every 5 ms of the act, it stays 2.7 m
 * or more from every boulder, coral head, blade and fish.
 * - For the dive's last metres, just under the surface, the lens comes in high
 *   and level, then sinks into its place behind the blob, looking down at it.
 * - Until the encounter draws near (ENC_IN - 8) it stands 4 m further back,
 *   turned 17 degrees and tilted a little up. That puts the blob low on the
 *   right third, with the seabed and the open water (and the whale's pass)
 *   beside and above it. Trailing the blob dead centre at half the frame's
 *   height made it a pale lump in the murk for the act's first 20 s.
 * - The whale reveal eases 10 m back and a little up along the trail from the
 *   blob. The smaller foreground blob gives the mother and calf their scale
 *   as the gaze lifts and the lens narrows. It stops just under Snell's
 *   window. At 7 degrees higher, the window's rim flickered in along the top
 *   as green chips.
 * - The flight's chase starts just after take-off. It's wide, 11 m behind the
 *   blob and 3 m above it, looking down past it at the reef, so the kelp, the
 *   coral and the big school stream past close below. From 18 m back and
 *   5.5 m up, the reef looked like a carpet and you could hardly tell the
 *   speed. The distance is fixed. The direction back is the mean direction
 *   to the points 8-16 m back along the path (by arc length), so a bend or a
 *   burst of speed turns it smoothly. Taking the path point that many metres
 *   back in straight-line distance made it surge by 1.6x. */
static mc_v3 whale_center(float uwU);
static mc_camera camera_uw(double t) {
    mc_camera c = camera_at2(t);
    float u = uw_u(t), e = 1.0f - mc_smoothstepf(0.0f, 3.5f, (float)(t - S2_BREACH));
    mc_v3 f = mc_v3_sub(c.target, c.pos), b = blob2_center(t), p = b;
    c.pos.y += 6.5f * e;
    c.target = mc_v3_lerp(c.target, mc_v3_add(c.pos, mc_vec3(f.x, -1.2f, f.z)), e);
    e = 1.0f - mc_smoothstepf(ENC_IN - 12.0f, ENC_IN - 8.0f, u);
    f = mc_v3_normalize(mc_vec3(f.x, 0.0f, f.z));
    c.pos = mc_v3_mad(c.pos, f, -4.0f * e);
    e *= mc_v3_dist(c.target, c.pos);
    c.target = mc_v3_mad(c.target, mc_v3_cross(f, mc_vec3(0.0f, 1.0f, 0.0f)), -0.31f * e);
    c.target.y += 0.06f * e;
    if ((e = mc_smoothstepf(FLIGHT_IN + 2.0f, FLIGHT_IN + 5.0f, u)) > 0.0f) {
        /* the direction back along the blob's path (see above). It swings
         * 23 degrees either side, in step with the blob's glances back. */
        mc_v3 q = b, g = mc_vec3(0.0f, 0.0f, 0.0f);
        float s = 0.0f;
        for (int i = 1; i < 200 && s < 16.0f; ++i) {
            p = blob2_center(t - i * 0.03);
            s += mc_v3_dist(p, q);
            q = p;
            if (s > 8.0f) g = mc_v3_add(g, mc_v3_dir(p, b));
        }
        g.y = 0.0f;
        g = mc_v3_normalize(g);
        g = mc_v3_normalize(mc_v3_mad(g, mc_v3_cross(g, mc_vec3(0.0f, 1.0f, 0.0f)), 0.42f * sinf(u * 0.4f + 0.5f)));
        p = mc_v3_mad(mc_v3_add(b, mc_vec3(0.0f, 3.0f, 0.0f)), g, 11.0f);
        c.pos = mc_v3_lerp(c.pos, p, e);
        c.target = mc_v3_lerp(c.target, mc_v3_sub(b, mc_vec3(0.0f, 2.2f, 0.0f)), e);
        c.fovy = mc_lerpf(c.fovy, mc_radians(75.0f), e);
    }
    if ((e = uw_look(u)) > 0.0f) {
        c.pos = mc_v3_mad(c.pos, mc_v3_dir(c.pos, b), 10.0f * e);
        f = mc_v3_lerp(mc_v3_dir(whale_center(u), c.pos), mc_v3_dir(b, c.pos), 0.4f);
        f.y += 0.03f;
        c.target = mc_v3_lerp(c.target, mc_v3_mad(c.pos, mc_v3_normalize(f), mc_v3_dist(c.target, c.pos)), e);
        c.fovy = mc_lerpf(c.fovy, mc_radians(48.0f), e);
    }
    return c;
}

/* The blob wanders the seabed on a looping path, and blob_phase warps its
 * progress along it. It cruises at ~1x, bolts after the near miss and from
 * FLIGHT_IN speeds up quadratically, so through the finale it passes more and
 * more fish, rock and weed per second. blob2_center takes scene-local t. */
static float blob_phase(float u) {
    /* 1x while it cruises, unaware even as the predator closes in. Just after
     * the near miss it bolts in a panic: it hits full speed (~9x) within a
     * second and runs out of breath by TRANS_IN. The curve 1-(1-x)^6(1+6x)
     * rises fast, then flattens, and adds 22 units of progress. Through the
     * transformation orbit it's back to ~1x and drifts while the camera
     * circles it. Then the flight across the ocean keeps speeding up
     * (quadratic plus a cubic kick), so the longer the blob runs, the further
     * and faster it goes. */
    float x = mc_saturatef((u - PANIC_IN) / (TRANS_IN - PANIC_IN));
    float y = 1.0f - x, y2 = y * y;
    float flee = 1.0f - y2 * y2 * y2 * (1.0f + 6.0f * x);
    float fl = u - FLIGHT_IN;
    if (fl < 0.0f) fl = 0.0f;
    return u + 22.0f * flee + 0.075f * fl * fl + 0.0016f * fl * fl * fl;
}
static mc_v3 blob2_center(double t) {
    float uu = uw_u(t);
    float u  = blob_phase(uu);
    /* During the flight the wander widens too (the Lissajous amplitude grows),
     * so the blob spirals outward over new seabed instead of covering the
     * same ground faster. */
    float fl = uu - FLIGHT_IN; if (fl < 0.0f) fl = 0.0f;
    float ex = 1.0f + 0.95f * mc_smoothstepf(0.0f, UW_SPAN - FLIGHT_IN, fl);
    return mc_vec3(ex * (28.0f * sinf(u * 0.07f) + 6.0f * sinf(u * 0.23f)),
                   -4.5f + 0.7f * sinf(u * 0.5f),
                   ex * (22.0f * cosf(u * 0.09f) + 5.0f * cosf(u * 0.19f)));
}

/* The whale, in its own cleared lane at WHALE_Z, well clear of the blob (whose
 * z never drops below -27). It swims 150 m along +X in 40 story seconds
 * (about 4.4 m/s of show time), high in the water: 3 m up at the top of its
 * gentle arc, 5.5 m under the surface. It comes out of the clearing murk
 * about 90 m ahead of the camera, which trails the blob toward its lane, at
 * ~76.5 s of the show. At the glance up (UW_LOOK) it crosses over side-on,
 * about 40 m off, dark against the bright water overhead. It's left behind
 * as the blob turns away and leaves the frame at ~96 s. It comes and goes out
 * of sight (and out of the sun's map). Props and schools stay out of the lane
 * at all times, and before story second 60 no fill prop stands in it either.
 * uwU is story seconds since the breach. */
static mc_v3 whale_center(float uwU) {
    float s = (uwU - WHALE_IN) / (WHALE_OUT - WHALE_IN);   /* 0..1 across the window */
    return mc_vec3(mc_lerpf(-57.5f, 92.5f, s), 3.0f * sinf(s * 3.14159f), WHALE_Z);
}

/* The blob's level heading and its right-hand side at scene-local u. */
static void blob_frame(float u, mc_v3 *fwd, mc_v3 *right) {
    mc_v3 b = blob2_center(uw_t(u));
    mc_v3 f = mc_v3_sub(blob2_center(uw_t(u) + 0.12), b);
    f.y = 0.0f;
    if (mc_v3_len2(f) < 1e-8f) f = mc_vec3(0, 0, 1);
    *fwd = mc_v3_normalize(f);
    *right = mc_v3_cross(*fwd, mc_vec3(0, 1, 0));
}

/* How wide the predator's jaws gape, from 0 (shut) to 1.55 (wide open).
 * fish.vert swings each jaw 0.75 rad per unit about its hinge. While it
 * stalks, the jaws hang ajar at 0.55, the pose the kit sculpted them in, with
 * the lower fangs clear of the upper lip. At 0.35 the fangs crossed outside
 * the lip and the mouth looked like a slit with the teeth outside the snout.
 * From a second before the lunge they open to 66 degrees each, a dark mouth
 * ringed with fangs. Stopping at 1 (43 degrees) looked like a toothy box.
 * They snap shut on the miss, at its kick at ENC_PEAK, as it sweeps past the
 * lens with its mouth wide in the frame. Shutting 0.12 earlier closed them
 * before they filled the frame. The jaws never come nearer than 2 m to the
 * blob, and they work again through the chase. */
static float pred_gape(float u) {
    float open  = 0.55f + mc_smoothstepf(ENC_LUNGE - 1.0f, ENC_PEAK - 0.35f, u);
    float snap  = mc_smoothstepf(ENC_PEAK, ENC_PEAK + 0.15f, u);
    float chase = 0.6f * mc_smoothstepf(ENC_PEAK + 0.5f, ENC_PEAK + 1.5f, u)
                * (1.0f - 0.6f * mc_smoothstepf(ENC_GIVEUP, ENC_GIVEUP + 2.0f, u));
    return open * (1.0f - snap) + chase;
}

/* The predator, at scene-local u: the path pred_build reconstructed at startup. */
static mc_v3 predator_center(const Demo *d, float u) {
    float x = (u - ENC_IN) * (float)PRED_HZ;
    x = mc_clampf(x, 0.0f, (float)(PRED_N - 2));
    int i = (int)x;
    return mc_v3_lerp(d->pred_path[i], d->pred_path[i + 1], x - (float)i);
}

/* Lays out the fish schools: rings of fish (fixed centres, members evenly
 * spaced so they never collide), clear of the blob's whole path, the whale's
 * lane and each other. The seabed's props keep clear of them in turn. It also
 * fills in the blob's path and the camera samples that uwplace.comp places
 * the props against. It runs once at startup, before audio playback. */
static void uw_world(Demo *d) {
    for (int i = 0; i < UW_PATH_SAMP; ++i) {
        mc_v3 p = blob2_center(uw_t((float)i * (UW_SPAN / (UW_PATH_SAMP - 1))));
        uw_in.path[i] = mc_vec4(p.x, p.y, p.z, 0.0f);
    }
    for (int k = 0; k < UW_CAM_SAMP; ++k) {
        mc_camera c = camera_uw(uw_t((float)k * 0.25f));
        mc_v3 f = mc_v3_dir(c.target, c.pos);
        uw_in.cam[2 * k] = mc_vec4(c.pos.x, c.pos.y, c.pos.z, 0.0f);
        uw_in.cam[2 * k + 1] = mc_vec4(f.x, f.y, f.z, 0.0f);
    }
    mc_rng fr = mc_rng_seed(0xF15E00u);
    for (int tries = 0; tries < UW_NSCHOOL * 240 && d->uw_ns < UW_NSCHOOL - 1; ++tries) {
        mc_v3 c = mc_vec3(mc_rng_range(&fr, -66.0f, 66.0f),
                          mc_rng_range(&fr, -9.0f, -3.0f),
                          mc_rng_range(&fr, -56.0f, 56.0f));
        float R  = mc_rng_range(&fr, 3.0f, 5.5f);     /* smaller, tighter schools place more easily */
        float om = mc_rng_range(&fr, 0.15f, 0.40f) * (mc_rng_f(&fr) < 0.5f ? -1.0f : 1.0f);
        float need = R + 2.4f;                         /* the ring, plus a fish length */
        int ok = fabsf(c.z - WHALE_Z) >= need + WHALE_ZHALF + 0.5f;
        for (int i = 0; i < UW_PATH_SAMP && ok; ++i)
            ok = mc_v3_dist(c, mc_vec3(uw_in.path[i].x, uw_in.path[i].y, uw_in.path[i].z)) >= need + UW_BLOB_R + 1.6f;
        for (int i = 0; i < d->uw_ns && ok; ++i)
            ok = mc_v3_dist(c, d->uw_schools[i].c) >= need + d->uw_schools[i].R + 2.5f;
        if (!ok) continue;
        int n = (int)(6.2831f * R / 3.8f);           /* even spacing >= ~3.8 units of arc (> a fish length) */
        if (n < 5) n = 5;
        if (n > 14) n = 14;
        if (d->uw_nf + n > UW_MAXF) n = UW_MAXF - d->uw_nf;
        if (n <= 0) continue;
        UwSchool *S = &d->uw_schools[d->uw_ns];
        S->c = c; S->R = R; S->om = om; S->n0 = d->uw_nf; S->n = n;
        uw_in.school[d->uw_ns] = mc_vec4(c.x, c.y, c.z, R);
        for (int k = 0; k < n; ++k) {
            UwFish *F = &d->uw_fish_s[d->uw_nf++];
            F->school = d->uw_ns;
            F->alpha  = 6.2831f * (float)k / (float)n;
            F->dr     = mc_rng_range(&fr, -0.35f, 0.35f);
            F->sc     = mc_rng_range(&fr, 0.6f, 1.1f);
            F->ph     = mc_rng_range(&fr, 0.0f, 20.0f);
            F->tint   = mc_rng_f(&fr);
            F->ybob   = mc_rng_range(&fr, -0.5f, 0.5f);
        }
        /* A school the camera passes through (within R + 1 of its centre)
         * keeps its ring, which the props stay out of, but shows no fish, or
         * one of them would cross the lens. Measured every 5 ms, no other
         * fish comes within 2.7 m of the lens. */
        for (int k = 0; k < UW_CAM_SAMP; ++k)
            if (mc_v3_dist(c, mc_vec3(uw_in.cam[2 * k].x, uw_in.cam[2 * k].y, uw_in.cam[2 * k].z)) < R + 1.0f) S->n = 0;
        d->uw_ns++;
    }
    for (int i = d->uw_ns; i < UW_NSCHOOL; ++i) uw_in.school[i].w = -1e4f;   /* none */
    /* the last slot is the big school on the flight's path at BIG_U
     * (fish.vert). The props keep out of it as a disc 12.5 m in radius and
     * 3.5 m above and below its centre (uwplace.comp). */
    mc_v3 c = blob2_center(uw_t(BIG_U));
    uw_in.school[UW_NSCHOOL - 1] = mc_vec4(c.x, -5, c.z, 12.5f);
}

/* Bind the underwater textures for the lit passes: the sun shadow map (unit 4,
 * for every shadowVis()) and the baked sand and rock materials (units 0 and
 * 2). Unit 1 is skipped because its 3D slot holds the noise, and a program
 * can't read that beside a 2D texture on the same unit. They stay bound until
 * the post stack runs. uLightVP is in the Frame block. */
static void bind_uw_textures(Demo *d) {
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, d->uw_shadow.depth);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, d->tex_rock);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d->tex_sand);
}

/* Bind a baked UV skin (unit 3) on a creature program (fish/whale), for Path-B
 * texturing sampled by the mesh's UVs. */
static void set_skin(GLuint tex) {
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, tex);
    glActiveTexture(GL_TEXTURE0);
}

/* The mesh behind each underwater batch. Each frame uw_prepare gathers the
 * instances the camera or the light can see, once. Each batch's buffer is
 * split into shared, camera-only and light-only ranges, so even a caster
 * behind the camera is there for the light pass without a re-upload. */
static mc_gpu_mesh *uw_mesh(Demo *d, int id) {
    switch (id) {
    case UW_BED: return &d->uw_seabed;
    case UW_ROCK0: case UW_ROCK1: case UW_ROCK2: return &d->uw_rock[id - UW_ROCK0];
    case UW_ALGA: return &d->uw_algae;
    case UW_FISH: return &d->uw_fish;
    case UW_CRAB: return &d->uw_crab;
    case UW_CORAL: return &d->uw_coral;
    case UW_WHALE: return &d->uw_whale;
    case UW_PRED: return &d->uw_pred;
    default: return &d->blob;
    }
}

static int uw_visible(Demo *d, const mc_frustum *camera, const mc_frustum *light,
                      mc_v3 pos, float radius, int casts) {
#ifdef MC_PROFILE
    if (d->disable_culling) return casts ? 3 : 1;
#else
    (void)d;
#endif
    return mc_frustum_sphere(camera, pos, radius) |
           (casts && mc_frustum_sphere(light, pos, radius) ? 2 : 0);
}

static void uw_add(Demo *d, int id, mc_instance item, int mask) {
    if (!mask) return;
    UwBatch *b = &d->uw_batch[id];
    mc_assert(b->n < UW_BATCH_CAP);
    b->items[b->n] = item; b->mask[b->n++] = (unsigned char)mask;
}

static int uw_reveal(int total, int initial, float start, float end, float t) {
    int n = initial + (int)(mc_smoothstepf(start, end, t) * (float)(total - initial));
    return n < 0 ? 0 : n > total ? total : n;
}

/* The fixed chase, fitted to the original steering simulation: positions in
 * 1/128 m and velocities in 1/4 m/s, with the near miss pinned at story 64 s.
 * Knot spans are 60 Hz ticks. Hermite tangents become Catmull-Rom's outside
 * points, so the existing spline code fills the same path table used by both
 * the swimmer and the seabed's keep-out pass. */
static const short PRED_POINT[3][33] = {
    { -773, -1233, -2863, -3519, -3574, -3657, -3171, -2279, -1683, -1455, -1252, -1089, -867, -405, 181, 432, 54, -913, -1433, -1684, -1927, -1686, -1172, -1057, -1026, -1393, -1925, -2357, -3976, -5421, -8152, -10610, -15188 },
    { 344, 196, -83, -138, -140, -132, -111, -91, -81, -77, -69, -59, -77, -134, -249, -365, -489, -542, -533, -518, -494, -467, -397, -362, -317, -215, -201, -201, -201, -201, -201, -201, -201 },
    { 10185, 9242, 7601, 6656, 6545, 5732, 4707, 3545, 2888, 2652, 2504, 2426, 2349, 2344, 2692, 3326, 4191, 4414, 4110, 3717, 2273, 1263, 319, 42, -281, -1192, -1572, -1809, -2662, -3338, -4409, -5337, -7658 },
};
static const signed char PRED_VELOCITY[3][33] = {
    { 0, -14, -18, -12, -11, 12, 45, 67, 59, 65, 98, 89, 85, 85, 56, 4, -67, -85, -61, -37, 6, 23, 31, 9, -1, -30, -39, -39, -39, -40, -41, -41, -37 },
    { -1, -3, -2, 0, 0, 1, 1, 1, 1, 1, 8, -1, -8, -13, -15, -14, -8, -1, 3, 3, 1, 3, 5, 6, 6, 3, 0, 0, 0, 0, 0, 0, 0 },
    { -17, -16, -21, -23, -23, -47, -71, -74, -65, -60, -52, -40, -20, 18, 66, 87, 57, -22, -64, -80, -79, -56, -47, -43, -44, -32, -21, -21, -20, -18, -15, -16, -23 },
};
static const unsigned char PRED_SPAN[] = { 109, 171, 80, 9, 50, 32, 29, 18, 7, 5, 3, 5, 10, 15, 15, 21, 22, 13, 10, 32, 30, 34, 12, 14, 43, 28, 21, 78, 68, 125, 112, 219 };
static void pred_build(Demo *d) {
    mc_v3 p[4];
    int at = 0;
    for (int k = 0; k < MC_ARRAY_COUNT(PRED_SPAN); ++k) {
        int n = PRED_SPAN[k];
        for (int c = 0; c < 3; ++c) {
            float a = PRED_POINT[c][k] * (1.0f / 128.0f), b = PRED_POINT[c][k + 1] * (1.0f / 128.0f);
            (&p[0].x)[c] = b - PRED_VELOCITY[c][k] * ((float)n / 120.0f);
            (&p[1].x)[c] = a;
            (&p[2].x)[c] = b;
            (&p[3].x)[c] = a + PRED_VELOCITY[c][k + 1] * ((float)n / 120.0f);
        }
        for (int j = 0; j < n; ++j)
            d->pred_path[at++] = mc_catmull_rom(p[0], p[1], p[2], p[3], (float)j / (float)n);
    }
    for (; at < PRED_N; ++at) d->pred_path[at] = p[2];
}

/* uwplace.comp runs 21 passes. The first makes the seabed's heights and the
 * boulder's vertices, which stay in its buffer, bound while the frames draw
 * (object.vert and rock.vert read them). The rest place the seabed's props
 * against what uw_world and pred_build found, and keep the ones that fit:
 * boulders, coral heads, crabs and weed, each with its birth time (and death
 * time, where a landmark grows over it). The last pass lists the boulders on
 * each patch of sand, for the sand's contact shading (uwsurf.frag). */
static void uw_place(Demo *d) {
    GLuint b;
    for (int i = 0; i <= PRED_N / 4; ++i)
        uw_in.pred[i] = mc_vec4(d->pred_path[4 * i].x, d->pred_path[4 * i].y, d->pred_path[4 * i].z, 0.0f);
    glGenBuffers(1, &b);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, b);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)sizeof uw_in, &uw_in, GL_STATIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, b);
    MC_TIDY(d->uw_buf = b);
    mc_shader_use(d->uwplace);
    glUniform4f(1, UW_ROCK_R + UW_ROCK_VDISP, d->uw_coral.radius, d->uw_crab.radius + 0.18f, 0.0f);
    for (int p = -1; p < 20; ++p) {
        glUniform1i(0, p);
        glDispatchCompute(UW_NCAND / 64, 1, 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
    }
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, (char *)uw_in.o - (char *)&uw_in, (GLsizeiptr)sizeof uw_in.o, uw_in.o);
    for (int i = 0; i < UW_NCAND && d->uw_np < UW_MAXP; ++i)
        if (uw_in.o[i].k.x > 0.0f && uw_in.o[i].k.y < 999.0f) d->uw_props[d->uw_np++] = uw_in.o[i];
}

/* A creature's model matrix from three path samples h seconds apart. The head
 * (+Z) faces the direction of travel, so it pitches as it climbs or dives, and
 * `bank` rolls its back into turns (the lateral acceleration tilts its up axis
 * toward the centre of the turn). */
static mc_m4 swim_matrix(mc_v3 p0, mc_v3 p, mc_v3 p1, float h, float bank, float sc) {
    mc_v3 f = mc_v3_sub(p1, p0);
    f = mc_v3_len2(f) > 1e-10f ? mc_v3_normalize(f) : mc_vec3(0.0f, 0.0f, 1.0f);
    mc_v3 a = mc_v3_scale(mc_v3_add(mc_v3_sub(p1, mc_v3_scale(p, 2.0f)), p0), bank / (h * h));
    a = mc_v3_sub(a, mc_v3_scale(f, mc_v3_dot(a, f)));
    float al = mc_v3_len(a);
    if (al > 0.8f) a = mc_v3_scale(a, 0.8f / al);
    mc_v3 x = mc_v3_cross(mc_v3_add(mc_vec3(0.0f, 1.0f, 0.0f), a), f);
    x = mc_v3_len2(x) > 1e-10f ? mc_v3_normalize(x) : mc_vec3(1.0f, 0.0f, 0.0f);
    mc_v3 y = mc_v3_cross(f, x);
    mc_m4 m = mc_m4_identity();
    m.m[0] = x.x * sc; m.m[1] = x.y * sc; m.m[2]  = x.z * sc;
    m.m[4] = y.x * sc; m.m[5] = y.y * sc; m.m[6]  = y.z * sc;
    m.m[8] = f.x * sc; m.m[9] = f.y * sc; m.m[10] = f.z * sc;
    m.m[12] = p.x; m.m[13] = p.y; m.m[14] = p.z;
    return m;
}

static void uw_prepare(Demo *d, float t, float u, const mc_camera *cam,
                       mc_m4 vp, mc_v3 blob, float bsz, int height) {
    mc_frustum camera = mc_frustum_make(vp), light = mc_frustum_make(d->uw_lightvp);
    for (int id = 0; id < UW_BATCHES; ++id) d->uw_batch[id].n = 0;
    mc_v3 y = mc_vec3(0,1,0);
    mc_instance bed = {mc_m4_identity(), mc_vec4(1,.98f,.95f,0)};   /* its heights are the world's */
    uw_add(d, UW_BED, bed, 3);
    /* The seabed's props. Each grows in from its birth (a boulder swelling up
     * out of the sand, weed rising, coral opening out) and stays until a
     * landmark grows over it. Boulders pick a mesh LOD by their size on
     * screen, or in the sun's map if that's bigger, so a far camera can't
     * coarsen a near shadow caster, with 20% hysteresis against chatter. Far
     * boulders, crabs and all the weed except the kelp (over 3 m tall) skip
     * the shadow pass, whose shadows fade out by 40 units anyway. */
    for (int i = 0; i < d->uw_np; ++i) {
        const UwProp *p = &d->uw_props[i];
        int k = (int)p->k.x, id = k == 1 ? UW_ROCK0 : k == 2 ? UW_CORAL : k == 3 ? UW_CRAB : UW_ALGA;
        float g = mc_smoothstepf(p->k.y, p->k.y + (k == 1 ? 2.5f : 4.0f), u), sc = p->at.w * g;
        if (g < .02f || (p->k.z > 0 && u >= p->k.z)) continue;
        mc_v3 pos = mc_vec3(p->at.x, p->at.y - (k == 1 ? p->at.w * .35f * (1 - g) : 0), p->at.z);
        float dist = mc_v3_dist(cam->pos, pos), radius = k == 4 ? d->uw_algae.radius * 1.3f * sc : p->k.w * g;
        int mask = uw_visible(d, &camera, &light, pos, radius, k == 2 || (k == 1 && dist < 45) || (k > 3 && sc > 3));
        if (!mask) continue;
        if (k == 1) {
            float pixels = radius * height / (2 * tanf(cam->fovy * .5f) * mc_maxf(dist - radius, .01f));
            if (mask & 2) pixels = mc_maxf(pixels, radius * UW_SHADOW_RES / d->uw_shadow_span);
            int level = d->rock_level[i];
            if (level == 0 && pixels < 64) level = 1;
            if (level == 1 && pixels > 96) level = 0;
            if (level == 1 && pixels < 24) level = 2;
            if (level == 2 && pixels > 36) level = 1;
#ifdef MC_PROFILE
            if (d->disable_lod) level = 0;
#endif
            d->rock_level[i] = (unsigned char)level;
            id += level;
        }
        mc_instance item = {mc_m4_trs(pos, mc_quat_axis_angle(mc_vec3(p->turn.x, p->turn.y, p->turn.z), p->turn.w),
                                      k == 4 ? mc_vec3(1, sc, 1) : mc_v3_splat(sc)), p->color};
        uw_add(d, id, item, mask);
    }
    int nf = uw_reveal(d->uw_nf, 5, 3, 98, u);
    for (int si = 0; si < d->uw_ns; ++si) {
        UwSchool *s = &d->uw_schools[si];
        float reach = 0;
        for (int i = s->n0; i < s->n0 + s->n && i < nf; ++i) {
            UwFish *f = &d->uw_fish_s[i];
            reach = mc_maxf(reach, s->R + fabsf(f->dr) + fabsf(f->ybob) + .3f + (d->uw_fish.radius + .18f) * f->sc);
        }
        if (!uw_visible(d, &camera, &light, s->c, reach, 0)) continue;
        for (int i = s->n0; i < s->n0 + s->n && i < nf; ++i) {
            UwFish *f = &d->uw_fish_s[i];
            float r = s->R + f->dr, a0 = s->om * u + f->alpha, a1 = s->om * (u + .1f) + f->alpha;
            mc_v3 p = mc_vec3(s->c.x + r * cosf(a0), s->c.y + f->ybob + .3f * sinf(u * .8f + f->ph), s->c.z + r * sinf(a0));
            int mask = uw_visible(d, &camera, &light, p, (d->uw_fish.radius + .18f) * f->sc, 0);
            if (!mask) continue;
            float dx = r * (cosf(a1) - cosf(a0)), dz = r * (sinf(a1) - sinf(a0));
            float yaw = dx * dx + dz * dz > 1e-9f ? atan2f(dx, dz) : 0;
            mc_instance item = {mc_m4_trs(p, mc_quat_axis_angle(y, yaw), mc_v3_splat(f->sc)),
                               mc_vec4(.6f + .4f * f->tint, .7f, .9f, f->ph)};
            uw_add(d, UW_FISH, item, mask);
        }
    }
    /* the whale (0), its calf (2) and the predator (1). The calf is 0.4 of its
     * mother's size and swims at her near flank, 5 m below her, since any
     * closer the two looked like one body. It drifts forward and back along
     * her, inside her lane (2.7 m of fluke either side of z = WHALE_Z + 3.5)
     * and clear of the schools' rings. Its flukes beat out of step with hers
     * (its phase, aColor.a, is 2). */
    for (int animal = 0; animal < 3; ++animal) {
        int whale = animal != 1;
        if (!(u > (whale ? WHALE_IN : ENC_IN) && u < (whale ? WHALE_OUT : ENC_OUT))) continue;
        int id = whale ? UW_WHALE : UW_PRED;
        mc_v3 o = animal == 2 ? mc_vec3(-3.0f + 2.0f * sinf(u * 0.25f), -5.0f, 3.5f) : mc_vec3(0, 0, 0), q[3];
        for (int j = 0; j < 3; ++j)   /* 0.1 s before, now, 0.1 s after */
            q[j] = mc_v3_add(whale ? whale_center(u + .1f * (j - 1)) : predator_center(d, u + .1f * (j - 1)), o);
        float sc = animal == 2 ? WHALE_SCALE * 0.4f : whale ? WHALE_SCALE : PRED_SCALE;
        int mask = uw_visible(d, &camera, &light, q[1], (uw_mesh(d,id)->radius + .18f) * sc, 1);
        if (!mask) continue;
        mc_instance item = {swim_matrix(q[0], q[1], q[2], .1f, whale ? 0.0f : .03f, sc),
                           whale ? mc_vec4(0,0,0,(float)animal) : mc_vec4(.10f,.13f,.12f,7.3f)};
        uw_add(d, id, item, mask);
    }
    /* the panic: a violent quiver right after the near miss, settling as it bolts */
    float startle = u > PANIC_IN ? 1.6f * mc_smoothstepf(PANIC_IN, PANIC_IN + 0.15f, u) * expf(-(u - PANIC_IN) * 1.4f)
                                 * (0.65f + 0.35f * sinf(u * 47.0f)) : 0;
    d->uw_wobble = mc_lerpf(1, .15f, mc_smoothstepf(TRANS_IN, TRANS_END, u)) + startle;
    mc_instance bi = {mc_m4_trs(blob, mc_quat_axis_angle(mc_vec3(0,1,.1f), t * .1f), mc_v3_splat(bsz)), mc_vec4(.15f,.40f,1,1)};   /* a = 1: the jelly */
    uw_add(d, UW_BLOB, bi, uw_visible(d, &camera, &light, blob, (d->blob.radius + .18f * mc_maxf(1, d->uw_wobble)) * bsz, 1));
    for (int id = 0; id < UW_BATCHES; ++id) {
        UwBatch *b = &d->uw_batch[id];
        static mc_instance ordered[UW_BATCH_CAP];
        int n = 0;
        int bins = 4;
#ifdef MC_PROFILE
        if (d->legacy_order) bins = 1;
#endif
        /* Four distance buckets within each visibility range (shared,
         * camera-only, light-only), sorted by one stable counting sort on the
         * 12 keys. It adds no draws, and off-camera casters still go up in the
         * same upload. */
        static unsigned char key[UW_BATCH_CAP];
        int start[13] = {0};
        for (int i = 0; i < b->n; ++i) {
            mc_m4 *m = &b->items[i].model;
            float dist2 = mc_v3_len2(mc_v3_sub(mc_vec3(m->m[12],m->m[13],m->m[14]), cam->pos));
            int bucket = bins == 1 ? 0 : dist2 < 256 ? 0 : dist2 < 1024 ? 1 : dist2 < 4096 ? 2 : 3;
            key[i] = (unsigned char)((b->mask[i] == 3 ? 0 : b->mask[i] == 1 ? 4 : 8) + bucket);
            ++start[key[i] + 1];
        }
        for (int k = 0; k < 12; ++k) start[k + 1] += start[k];
        n = b->n;
        b->both = start[4];
        b->color = start[8];
        for (int i = 0; i < b->n; ++i) ordered[start[key[i]]++] = b->items[i];
        b->shadow = b->both + n - b->color;
        if (n) mc_mesh_set_instances(uw_mesh(d, id), ordered, n);
    }
}

static void uw_draw(Demo *d, int id, int shadow) {
    UwBatch *b = &d->uw_batch[id];
    mc_gpu_mesh g = *uw_mesh(d, id);
    if (!shadow) mc_mesh_draw_instance_range(g, 0, b->color);
    else {
        mc_mesh_draw_instance_range(g, 0, b->both);
        mc_mesh_draw_instance_range(g, b->color, b->shadow - b->both);
    }
}

/* The underwater draws as data: a program, a batch and how to draw it. The
 * animals' draws hand fish.vert their flags (uK, location 1), which say which
 * animal it is and how it swims, and bind their skin. Everything else they
 * need is in the Frame block. */
enum { UW_TWO_SIDED = 1, UW_FISHY = 2, UW_WHALE_SKIN = 4, UW_PREDATOR = 8, UW_BARE = 16 };
typedef struct { unsigned char prog, batch, flags; } UwDraw;
static const UwDraw uw_shadow_draws[] = {
    {MC_PROG_shadowcoral, UW_CORAL, UW_TWO_SIDED},
    {MC_PROG_shadowalgae, UW_ALGA, UW_TWO_SIDED},
    {MC_PROG_shadowrock, UW_ROCK0, 0},    {MC_PROG_shadowrock, UW_ROCK1, 0},
    {MC_PROG_shadowrock, UW_ROCK2, 0},    /* not the blob: a lens (uwsurf.frag) */
    {MC_PROG_shadowfish, UW_WHALE, UW_WHALE_SKIN},
    {MC_PROG_shadowfish, UW_PRED, UW_PREDATOR},
};
static const UwDraw uw_lit_draws[] = {     /* the seabed and blob draw separately */
    {MC_PROG_uwrock, UW_ROCK0, 0},        {MC_PROG_uwrock, UW_ROCK1, 0},
    {MC_PROG_uwrock, UW_ROCK2, 0},        {MC_PROG_algaesh, UW_ALGA, UW_TWO_SIDED},
    {MC_PROG_fishsh, UW_FISH, UW_FISHY},  /* the animals are closed: cull */
    {MC_PROG_fishsh, UW_CRAB, UW_FISHY | UW_BARE},   /* crabs: no scales */
    {MC_PROG_coralsh, UW_CORAL, UW_TWO_SIDED},
    {MC_PROG_whalesh, UW_WHALE, UW_WHALE_SKIN},
    {MC_PROG_fishsh, UW_PRED, UW_FISHY | UW_PREDATOR},
};

static void uw_draw_list(Demo *d, const UwDraw *list, int n, int shadow) {
    for (const UwDraw *u = list; u < list + n; ++u) {
        UwBatch *b = &d->uw_batch[u->batch];
        if (!(shadow ? b->shadow : b->color)) continue;
        mc_shader_use(*program_at(d, programs[u->prog].offset));
        if (u->flags > 1) glUniform1i(1, u->flags);
        if (u->flags & UW_FISHY) set_skin(d->tex_fishskin);
        if (u->flags & UW_WHALE_SKIN) set_skin(d->tex_whaleskin);
        if (u->flags & UW_TWO_SIDED) glDisable(GL_CULL_FACE);
        uw_draw(d, u->batch, shadow);
        glEnable(GL_CULL_FACE);
    }
}

/* The sun's depth pass. It leaves the camera's matrix in the Frame block for
 * the caller to upload for the lit passes. */
static void uw_shadow_pass(Demo *d) {
    mc_m4 camera = d->frame.view_proj;
    shadow_begin(d, &d->uw_shadow);
    uw_draw_list(d, uw_shadow_draws, (int)(sizeof uw_shadow_draws / sizeof *uw_shadow_draws), 1);
    shadow_end(d, camera);
}

static mc_m4 uw_light_vp(Demo *d, const mc_camera *cam) {
    /* Fit the first 40 world units of the authored camera frustum. A bounding
     * sphere using the widest authored FOV keeps the scale constant through
     * camera shake and orbits. Casters along the light's 149-unit depth span
     * are included. Receivers fade their shadows out over 35..40 units, inside
     * this conservative fit. */
    float aspect = mc_renderer_aspect(d->R), far = 40.0f;
    float half = far * tanf(mc_radians(65.0f) * .5f);
    float radius = ceilf((sqrtf(20*20 + half*half*(1+aspect*aspect)) + 2) / 4) * 4;
    d->uw_shadow_span = radius * 2;
    mc_v3 forward = mc_v3_dir(cam->target, cam->pos);
    mc_v3 center = mc_v3_mad(cam->pos, forward, 20);
    mc_v3 sdir = mc_v3_normalize(mc_vec3(0.5f, 1.0f, 0.25f));   /* the sun, from above */
    mc_v3 eye  = mc_v3_mad(center, sdir, 70.0f);
    mc_m4 lview = mc_m4_look_at(eye, center, mc_vec3(0.0f, 0.0f, 1.0f));
    float texel = d->uw_shadow_span / UW_SHADOW_RES;
    lview.m[12] = floorf(lview.m[12] / texel + .5f) * texel;
    lview.m[13] = floorf(lview.m[13] / texel + .5f) * texel;
    mc_m4 lproj = mc_m4_ortho(-radius, radius, -radius, radius, 1.0f, 150.0f);
    return mc_m4_mul(lproj, lview);
}

/* The picture drawn so far, copied for a jelly to look through, since it can't
 * read the target it draws into. It's the whole HDR scene at render size, on
 * texture unit 9. Copying only the rectangle round the ball isn't worth it.
 * The lens reads up to ~2.3 radii off its centre, the crossing needs the
 * whole frame anyway, and the whole frame is ~150 bytes cheaper than working
 * the rectangle out. It's a 1536 x 864 half-float copy, ~0.3 ms on a UHD 620
 * and next to nothing on the target. */
static GLuint jelly_tex;
static int    jelly_size;
static void jelly_copy(Demo *d, mc_v3 p, float r) {   /* (p, r: the ball, unused) */
    int w, h;
    (void)p; (void)r;
    mc_renderer_size(d->R, &w, &h);
    glActiveTexture(GL_TEXTURE9);
    if (!jelly_tex) glGenTextures(1, &jelly_tex);
    glBindTexture(GL_TEXTURE_2D, jelly_tex);
    if (jelly_size != w * 65536 + h) {
        jelly_size = w * 65536 + h;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    glActiveTexture(GL_TEXTURE0);
}

static void uw_draw_seabed(Demo *d) {
    mc_shader_use(d->uwsurf);
    glUniform2f(U_STORY, uw_u(d->frame.time), 0.0f);   /* the story's seconds: when the reef bed lies down */
    uw_draw(d, UW_BED, 0);
}

static void render_scene2(Demo *d, mc_window *w, double t) {
    int ow, oh;
    mc_window_size(w, &ow, &oh);

    /* Going under takes 2 S2_DIP. The waterline sweeps up the lens, with the
     * sea above it (drawn first, then copied) and the sea below it (the
     * underwater act, held at its first moment until the breach), so there's
     * no cut. */
    float dip = (float)(t - S2_BREACH) / S2_DIP;            /* -1 .. 1 while it goes under */
    int   up  = dip < 1.0f, down = dip > -1.0f;             /* the worlds this frame shows */
    double tu = fmax(t, (double)S2_BREACH);                 /* the underwater act's clock */
    mc_camera cam = t < (double)S2_CLOUDFLASH ? camera_at2(t) : up ? camera_sea(t) : camera_uw(t);
    if (up && down) {   /* the history (and the depth the post sees) follow the sea below */
        mc_camera cu = camera_uw(tu);
        camera_vp(d, &cu);
    }
    mc_m4 vp = camera_vp(d, &cam);
    mc_v3 sun = mc_v3_normalize(mc_vec3(0.5f, 0.35f, 0.55f));

    int   planetCtx   = (t < (double)S2_CLOUDFLASH);
    /* a white-out through the cloud deck hides the cut from the planet to the sea */
    float cloudFlash  = expf(-(float)((t - S2_CLOUDFLASH) * (t - S2_CLOUDFLASH)) * 6.0f);
    float uwU    = uw_u(tu);                                 /* story seconds since the breach */
    /* A brief darker passage lets the jellyfish's luminous canals read.
     * The same story-time envelope lives in jelly.frag. */
    float bio = mc_smoothstepf(38.0f, 43.0f, uwU) * (1.0f - mc_smoothstepf(51.0f, 56.0f, uwU));
    float uwClar = mc_smoothstepf(0.0f, 9.0f, uwU);          /* the water clears (by show ~77.3 s) */
    float qual   = mc_smoothstepf(0.0f, 30.0f, uwU);         /* renderer-evolution ramp (shared with uQ) */
    int ordered = 1;
#ifdef MC_PROFILE
    ordered = !d->legacy_order;
#endif

    /* The Frame block for the backdrops and the surface shots. The underwater
     * act adds its own terms (and the shadow pass's view) further down. */
    FrameUniforms *F = &d->frame;
    frame_basics(d, vp, cam.pos, (float)t);
    F->sun       = sun;
    /* space turns to sky in the plunge's last 1.2 s, as the planet comes to
     * fill the frame. Over the last 4.2 s, the sky would turn blue round a
     * ball still a third of the frame high, like a toy planet in a blue sky. */
    F->atmo      = (t < (double)S2_CLOUDFLASH)
                 ? mc_smoothstepf(S2_CLOUDFLASH - 1.2f, S2_CLOUDFLASH, (float)t)
                 : 1.0f;                                                       /* full sky over the sea */
    /* just under the surface the water is bright and clear, darkening as we sink */
    float sf     = 1.0f - mc_smoothstepf(0.0f, 3.5f, (float)(t - S2_BREACH));
    F->bright    = 0.22f + 0.70f * uwClar + 0.8f * sf;
    mc_gl_frame_block(F, sizeof *F);

    mc_v3 clear = planetCtx ? mc_vec3(0.01f, 0.014f, 0.03f) : mc_vec3(0.0f, 0.05f, 0.12f);
    mc_renderer_begin(d->R, clear);
    /* The jelly's copy follows the renderer's size. It's re-made here, on the
     * cut's near-black first frame after Scene 1's scale switch, instead of at
     * its first use. That's the dip, in full view, where the new 37 MB texture
     * cost a frame an extra 30 ms on the UHD 620. What it copies now is
     * overwritten before anything reads it. */
    {
        int rw, rh;
        mc_renderer_size(d->R, &rw, &rh);
        if (jelly_size != rw * 65536 + rh) jelly_copy(d, cam.pos, 0.0f);
    }

    /* the backdrop above water (full-screen, no depth): space fading to sky */
    mc_gl_switch(MC_ZTEST_OFF | MC_ZWRITE_OFF);
    if (up) {
        mc_shader_use(d->space);
        mc_fullscreen_triangle();
    }
    mc_gl_switch(MC_ZTEST_ON | MC_ZWRITE_ON);

    if (planetCtx) {
        /* the planet, turning slowly */
        mc_shader_use(d->planet_sh);
        mc_m4 model = mc_m4_mul(mc_m4_scale1(PLANET_R),
                                mc_m4_rotate(mc_vec3(0.1f, 1.0f, 0.0f), (float)t * 0.04f));
        mc_set_m4(U_MODEL, model);
        mc_mesh_draw(d->planet);

        /* its moon, a fifth of its size, 65 units out toward where we come
         * from and 23 off our line. It sits low to the right of the planet as
         * we drift in, then swells and slides out past the frame's right edge
         * on the plunge, never coming nearer the lens than 23. */
        mc_shader_use(d->moon_sh);
        mc_set_m4(U_MODEL, mc_m4_trs(mc_vec3(36.9f, 3.2f, 56.8f), mc_quat_axis_angle(mc_vec3(0.0f, 1.0f, 0.0f), (float)t * 0.02f), mc_v3_splat(4.0f)));
        mc_mesh_draw(d->planet);

        /* its atmosphere: the same sphere, a little larger, added on, far rim
         * only (front-face culled) so it shows as a halo round the disc */
        mc_shader_use(d->atmo);
        mc_set_m4(U_MODEL, mc_m4_scale1(PLANET_R * 1.06f));
        mc_gl_switch(MC_ZWRITE_OFF | MC_BLEND_ON);
        glBlendFunc(GL_ONE, GL_ONE);
        glCullFace(GL_FRONT);
        mc_mesh_draw(d->planet);
        mc_gl_switch(MC_ZWRITE_ON | MC_BLEND_OFF);
        glCullFace(GL_BACK);
    } else if (up) {
        /* the sea surface coming up to meet us */
        mc_shader_use(d->ocean_sh);
        mc_set_m4(U_MODEL, mc_m4_identity());
        mc_mesh_draw(d->ocean);
    }
    if (down) {
        if (up) {   /* both: keep the sea above, start again below */
            jelly_copy(d, cam.pos, 1e9f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            cam = camera_uw(tu);
            vp = camera_vp(d, &cam);
            frame_basics(d, vp, cam.pos, (float)t);
        }
        /* ---- the underwater journey (the long, accelerating act) ---- *
         * The blob swims a wandering path, speeding up through the finale, and
         * the camera trails it past fish, boulders and weed, all placed
         * collision-free (uwplace.comp) so nothing ever intersects. Three
         * things ramp up as it goes. One is the water's `clarity`. Another is
         * the `real` push, uReal, which gives every underwater shader finer
         * detail, relief, sharper speculars and more caustics. The third is
         * the amount on screen, as more rocks, fish and weed appear while the
         * blob explores. The seabed and half the rocks are textured (baked,
         * triplanar). The other rocks and the weed are procedural, and the
         * fish take on a baked skin as uReal rises. */
        float clarity = mc_smoothstepf(0.0f, 9.0f, uwU);
        float real    = mc_smoothstepf(8.0f, 48.0f, uwU);              /* until the encounter draws near */
        /* qual (the renderer-evolution knob, 0 early -> 1 late) is computed at the
         * top of render_scene2 and fed to every underwater shader as uQ. */
        mc_v3  blob    = blob2_center(tu);
        mc_v3  bfwd    = mc_v3_sub(blob2_center(tu + 0.12), blob);     /* blob's heading */
        if (mc_v3_len2(bfwd) < 1e-5f) bfwd = mc_vec3(0, 0, 1);
        bfwd = mc_v3_normalize(bfwd);
        mc_v3  bright  = mc_v3_cross(bfwd, mc_vec3(0, 1, 0));
        if (mc_v3_len2(bright) < 1e-5f) bright = mc_vec3(1, 0, 0);
        bright = mc_v3_normalize(bright);
        mc_v3  ldir    = mc_v3_normalize(mc_vec3(0.5f, 1.0f, 0.25f));   /* light from the surface */
        mc_v3  fogc    = mc_v3_lerp(mc_vec3(0.02f, 0.10f, 0.16f),
                                    mc_vec3(0.04f, 0.22f, 0.37f), clarity);
        float  appear  = mc_smoothstepf((float)S2_BREACH + 1.0f, (float)S2_BREACH + 5.0f, (float)t);
        float  bsz     = UW_BLOB_R * (0.6f + 0.4f * appear);          /* blob radius (used below too) */
        if (uwU > ENC_PEAK && uwU < ENC_PEAK + 0.6f)                    /* it flinches at the near miss */
            bsz *= 1.0f - 0.14f * sinf((uwU - ENC_PEAK) / 0.6f * MC_PI);

        /* The sun's shadow map. Its view is fitted here, before uw_prepare
         * culls against it. The casters' depth is drawn further down
         * (uw_shadow_pass), and the lit passes sample it, faded in by uQ. */
        d->uw_lightvp = uw_light_vp(d, &cam);
        MC_PASS_END();
#ifdef MC_PROFILE
        double prepare_start = mc_time_seconds();
#endif
        uw_prepare(d, (float)tu, uwU, &cam, vp, blob, bsz, (int)F->res.y);
#ifdef MC_PROFILE
        mc_profile_cpu(1, (mc_time_seconds() - prepare_start) * 1000);
#endif
        /* The underwater terms of the Frame block: the light, the water and the
         * blob/predator animation, shared by the shadow and lit passes. */
        F->light_vp     = d->uw_lightvp;
        F->light_dir    = ldir;
        F->real         = real;
        F->fog_color    = mc_v3_scale(mc_v3_lerp(fogc, mc_vec3(0.04f, 0.22f, 0.37f), sf), 1.0f - 0.3f * bio);
        /* ...as the water scatters it in at the eye's depth. That's the
         * sunlight left at that depth, with red going first (the water
         * shaders' wx(), half of it per metre of depth, 1 at the blob's depth). */
        {
            mc_v3 wx = mc_v3_lerp(mc_vec3(0.18f, 0.07f, 0.05f), mc_vec3(0.06f, 0.018f, 0.013f), clarity);
            float y = 0.5f * (cam.pos.y + 4.5f);
            F->fog_color = mc_v3_mul(F->fog_color, mc_vec3(fminf(expf(wx.x * y), 1.4f), fminf(expf(wx.y * y), 1.4f), fminf(expf(wx.z * y), 1.4f)));
        }
        F->clarity      = clarity;
        F->q            = qual;
        F->sky_color    = mc_vec3(0.10f, 0.40f, 0.50f);   /* bubbles/eyes: blue underwater light */
        F->ground_color = mc_vec3(0.0f, 0.06f, 0.12f);
        F->fog          = mc_lerpf(0.03f, 0.012f, clarity);
        F->blob_fog     = mc_lerpf(0.02f, 0.008f, clarity);
        F->rim          = mc_v3_clamp(mc_v3_mad(mc_v3_splat(0.4f), mc_vec3(0.15f, 0.40f, 1.00f), 0.6f), 0.0f, 1.0f);
        F->wobble       = d->uw_wobble;                     /* set by uw_prepare */
        F->glow         = blob;                             /* the jelly's lens (uwsurf.frag): */
        F->glow_amt     = bsz;                              /* its centre and radius */
        F->gape         = pred_gape(uwU);
        /* the blob's face (below, with its eyes). y is how far it's turned from
         * the camera, which sets how much of its eyes shows (uGridFade,
         * borrowed from Scene 1's grid). The jelly's glint keeps clear of the
         * eyes (blob.frag). */
        float face = 1.0f - mc_smoothstepf(TRANS_END - 1.0f, TRANS_END + 3.0f, uwU),
              sway = sinf(uwU * 0.40f + 0.5f),
              y    = (1.0f - face) * (1.0f - 0.9f * mc_smoothstepf(FLIGHT_IN, FLIGHT_IN + 5.0f, uwU) * mc_smoothstepf(0.3f, 1.0f, fabsf(sway)))
                   * (sway < 0.0f ? -MC_PI : MC_PI);
        F->grid_fade    = mc_smoothstepf(0.65f, 0.92f, cosf(y));
        /* the shadow pass runs on every frame the sea below is drawn, including
         * the crossing and frames where uQ is 0. The lit shaders fade their
         * shadows in with uQ, but the object shader (the bubbles, the eyes,
         * the marine snow) reads the map whatever uQ is. Without this pass the
         * map would still hold Scene 1's key-light depths. */
        uw_shadow_pass(d);
        mc_gl_frame_block(F, sizeof *F);
        bind_uw_textures(d);

        /* The backdrop's light shafts hang from the eye's path averaged over
         * the half second round now, so the camera's tremble doesn't shake
         * them. The faster that path runs (past ~12 m/s) or the eye swings
         * (~60 deg/s), the more they give way to broad, soft shafts, as a
         * shutter would smear them. This starts sooner near the surface, where
         * they're sharpest. Sharp shafts flown or swung through at speed
         * strobed. The specks and the glint of Snell's window calm down with
         * them. Location 0 carries the averaged eye (xyz) and the sharpness
         * (w). */
        {
            mc_camera p = camera_uw(fmax(tu - 0.25, (double)S2_BREACH)), c = p;
            mc_v3 m = p.pos, f = mc_v3_dir(p.target, p.pos);
            float k = 0.0f;
            for (int i = 1; i <= 30; ++i) {
                c = camera_uw(fmax(tu - 0.25 + i / 60.0, (double)S2_BREACH));
                mc_v3 g = mc_v3_dir(c.target, c.pos);
                k += acosf(fminf(1.0f, mc_v3_dot(f, g))) * 2.5f;
                m = mc_v3_add(m, c.pos);
                f = g;
            }
            m = mc_v3_scale(m, 1.0f / 31.0f);
            k = fmaxf((k + mc_v3_dist(c.pos, p.pos) / 6.0f) * (1.0f + 0.1f * fmaxf(m.y, 0.0f)) - 1.1f, 0.0f) * 3.0f;
            mc_shader_use(d->water);
            mc_set_v4(0, mc_vec4(m.x, m.y, m.z, 1.0f / (1.0f + k * k)));
        }
        if (!ordered) {   /* the legacy order: the backdrop (it needs the water's terms) and the seabed first */
            mc_gl_switch(MC_ZTEST_OFF | MC_ZWRITE_OFF);
            mc_shader_use(d->water);
            mc_fullscreen_triangle();
            mc_gl_switch(MC_ZTEST_ON | MC_ZWRITE_ON);
            uw_draw_seabed(d);
        }
        /* Then the lit batches, in uw_lit_draws' order:
         * - the boulders (uwrock): rock.vert adds geometric crags as uReal
         *   rises, over the same uwsurf.frag material. More of the pre-placed
         *   field appears as the blob explores.
         * - the weed: each pre-placed blade sprouts at its own time and eases
         *   up to full height (the unit-tall mesh is scaled by its model). A
         *   tall one is a broad, golden kelp frond that sways wide and casts.
         * - the fish: more of the pre-placed schools as the act goes on. Each
         *   fish circles its school's ring nose first.
         * - the crabs on the sand, on the fish PBR shader.
         * - the coral heads, which grow in, each with its own hue and one of
         *   four forms: staghorn, brain-coral dome, table, sea fan.
         * - the whale, a slow pass across the far field in its own lane.
         * - the predator, fish.vert/.frag with uK's 8. It stalks the blob,
         *   lunges with its jaws wide (uGape), misses by a hair
         *   (collision-checked), snaps them shut and gives chase. */
        uw_draw_list(d, uw_lit_draws, (int)(sizeof uw_lit_draws / sizeof *uw_lit_draws), 0);
        /* The big school the blob bolts through in its flight. fish.vert (uK's
         * 32) places all 300 fish, parting round the blob. It's drawn from
         * u 60 on (it appears far behind the camera) whenever its disc is in
         * sight. It casts no shadow, which would cost its whole draw again. */
        {
            mc_frustum fr = mc_frustum_make(vp);
            mc_v4 *c = &uw_in.school[UW_NSCHOOL - 1];
            if (uwU > 60 && mc_frustum_sphere(&fr, mc_vec3(c->x, c->y, c->z), 14)) {
                mc_shader_use(d->fishsh);
                glUniform1i(1, 34);
                glUniform2f(U_STORY, uwU, 0.0f);
                set_skin(d->tex_fishskin);
                mc_mesh_draw_instanced(d->uw_fish, 300);
            }
        }

        /* the blob, while it isn't jelly yet (see below). Its rim, fog and
         * wobble are in the Frame block. */
        if (qual <= 0.1f) {
            mc_shader_use(d->blobsh);
            uw_draw(d, UW_BLOB, 0);
        }

        /* a few bubbles rising out of the blob, plus the blob's eyes (instanced
         * SH_ICO). A bubble is air (alpha -4 picks object.frag's bubble, drawn
         * blended). They rise out of the blob's top, off the line to the lens.
         * When they trailed up to 3 m behind it, they got between it and the
         * camera. */
        d->inst_count[SH_ICO] = 0;
        {
            mc_rng rng = mc_rng_seed(0x6B0BBu);
            for (int i = 0; i < 14; ++i) {
                float off = mc_rng_range(&rng, -1.0f, 1.0f);
                float sp  = mc_rng_range(&rng, 0.6f, 1.6f);
                float ph  = mc_rng_range(&rng, 0.0f, 20.0f);
                float sz  = mc_rng_range(&rng, 0.03f, 0.13f);
                float jit = mc_rng_range(&rng, -1.5f, 1.5f);
                mc_v3 bp = mc_v3_add(blob,
                               mc_v3_mad(mc_v3_mad(mc_vec3(0.0f, mc_modf_pos((float)t * sp + ph, 6.0f), 0.0f), bright, jit), bfwd, off + 0.5f * sinf((float)t + ph)));
                if (qual > 0.1f || mc_v3_dist(bp, blob) > 1.2f * bsz)   /* not inside it while it is milky: it writes its centre's depth (blob.frag) */
                    push_instance(d, SH_ICO, mc_m4_trs(bp, mc_quat_identity(), mc_v3_splat(sz)),
                                  mc_vec4(0.7f, 0.85f, 0.95f, -4.0f));
            }
            /* and the dive's own: a burst of bubbles around the lens as it goes
             * under, rising past it and away while it sinks. They're 0.7-1.7 cm
             * across per metre of distance. At 0.4-1 cm, drawn as air, they
             * were just rings of 2-4 px. */
            float tb = (float)(t - S2_BREACH) + S2_DIP, g = 1.0f - mc_smoothstepf(1.0f, 3.0f, tb);
            if (g > 0.0f) {
                mc_camera c0 = camera_uw(S2_BREACH);
                mc_v3 f = mc_v3_dir(c0.target, c0.pos);
                mc_v3 r = mc_v3_normalize(mc_v3_cross(f, mc_vec3(0.0f, 1.0f, 0.0f)));
                for (int i = 0; i < 40; ++i) {
                    float z = mc_rng_range(&rng, 0.8f, 4.0f), sp = mc_rng_range(&rng, 0.5f, 1.5f);
                    mc_v3 bp = mc_v3_add(c0.pos, mc_v3_mad(mc_v3_scale(f, z), r, mc_rng_range(&rng, -0.9f, 0.9f) * z));
                    bp.y += mc_rng_range(&rng, -0.5f, 0.1f) * z + (sp + 0.6f * tb) * tb;
                    push_instance(d, SH_ICO, mc_m4_trs(bp, mc_quat_identity(), mc_v3_splat(mc_rng_range(&rng, 0.007f, 0.017f) * z * g)),
                                  mc_vec4(0.7f, 0.85f, 0.95f, -4.0f));
                }
            }
            /* Marine snow: specks of silt hanging in a 10 m box that wraps round
             * the eye. They sit in front of everything, with parallax, smeared
             * by the motion blur when the eye flies and soft where the depth of
             * field takes them. The backdrop's own specks only show on open
             * water. A speck shrinks away inside 1.5 m of the lens, because one
             * passing the lens swelled to a white egg 120 px across. */
            rng = mc_rng_seed(0x5110u);   /* its own draws: the dive's burst comes and goes */
            for (int i = 0; i < 500; ++i) {
                mc_v3 q;
                for (int j = 0; j < 3; ++j)
                    (&q.x)[j] = (&cam.pos.x)[j] + mc_modf_pos(mc_rng_range(&rng, 0.0f, 10.0f) - (&cam.pos.x)[j], 10.0f) - 5.0f;
                float sz = mc_rng_range(&rng, 0.005f, 0.018f) * mc_smoothstepf(0.4f, 1.5f, mc_v3_dist(q, cam.pos));
                if (qual > 0.1f || mc_v3_dist(q, blob) > 1.2f * bsz)       /* (as the bubbles) */
                    push_instance(d, SH_ICO, mc_m4_trs(q, mc_quat_identity(), mc_v3_splat(sz)),
                                  mc_vec4(0.35f, 0.45f, 0.45f, 0.1f));
            }
        }
        /* The blob's face: Scene 1's eyes (blob_eyes), a size smaller. It has
         * no eyes through the journey or the encounter, so the stalk, the near
         * miss and the chase play on its body alone. Once it has survived the
         * scare it grows them, wide open, from nothing to Scene 1's size,
         * steadily through the orbit as the camera circles it. They sit 0.9 of
         * its radius out and draw after the blob. The blob writes its centre's
         * depth (blob.frag), so the eyes show through its front half whatever
         * its lumps do, milky or clear.
         * The face's direction is a turn and a tip, with no roll, both taken
         * from the direction to the camera c. Until the flight it faces the
         * camera, tipped to its height. Then it turns away by y (set with the
         * Frame block, above), round to the side the flight's camera swings out
         * to (the sign of camera_uw's sway). It ends up looking where it's
         * going, and we see the back of its head. Now and then, as that camera
         * swings out, it glances back over its shoulder, 0.1 of the way round,
         * to a 3/4 face. The turn follows the camera, never the blob's heading,
         * which weaves faster than the camera follows. When it was measured
         * from the heading, the glance swung across the back of its head in a
         * fifth of a second, flipping sides as the heading crossed the camera's
         * line. As the face turns from 23 to 50 deg off the camera, the eyes
         * shrink away (over ~0.3 s) so neither pokes out of the outline. The
         * camera is close, so the outline it sees lies ~75 deg round from it,
         * and the eyes sit ~25 deg out from the middle of the face. */
        {
            mc_v3 c = mc_v3_dir(cam.pos, blob);
            float grow = mc_smoothstepf(TRANS_IN, TRANS_IN + 9.0f, uwU),
                  a = 0.5f * (atan2f(c.x, c.z) - y), b = -0.5f * face * atan2f(c.y, sqrtf(c.x * c.x + c.z * c.z));
            if (grow > 0.0f)
                blob_eyes(d, blob, 0.9f * bsz, (mc_quat){cosf(a) * sinf(b), sinf(a) * cosf(b), -sinf(a) * sinf(b), cosf(a) * cosf(b)},
                          mc_v3_splat(1.0f), 1.0f, 1.0f, 1.11f * grow * F->grid_fade);
        }

        if (ordered) uw_draw_seabed(d);
    }

    if (ordered && down) {
        mc_gl_switch(MC_ZTEST_ON | MC_ZWRITE_OFF | MC_CULL_OFF | MC_BLEND_OFF);
        glDepthFunc(GL_LEQUAL);
        mc_shader_use(d->water);
        mc_fullscreen_triangle();
        glDepthMask(GL_TRUE);
    }
    /* Once uQ passes 0.1 the blob turns to jelly (blob.frag). It's drawn last
     * then, after everything that can be seen through it, and it looks through
     * a copy of the picture so far. That includes the moon jellies
     * (jelly.vert), added on over the water from story second 36 to 57. */
    if (down) {
        if (uwU > 36.0f && uwU < 57.0f) {
            mc_shader_use(d->jellysh);
            glUniform2f(U_STORY, uwU, 0.0f);
            mc_gl_switch(MC_ZWRITE_OFF | MC_CULL_OFF | MC_BLEND_ON);
            glBlendFunc(GL_ONE, GL_ONE);
            mc_mesh_draw_instanced(d->uw_jelly, 56);
            mc_gl_switch(MC_ZWRITE_ON | MC_BLEND_OFF);
        }
        if (F->q > 0.1f && d->uw_batch[UW_BLOB].color) {
            jelly_copy(d, cam.pos, 1e9f);
            glEnable(GL_CULL_FACE);
            mc_shader_use(d->blobsh);
            uw_draw(d, UW_BLOB, 0);
            glDisable(GL_CULL_FACE);
        }
        /* draw the bubbles and eyes (object shader, blue underwater light) with
         * no glow at the origin. That glow is Scene 1's light, and down here
         * uGlow / uGlowAmt hold the jelly's centre and radius (for the seabed's
         * caustic), which would tint them by wherever the blob happens to be. */
        F->glow_amt = 0.0f;
        mc_gl_frame_block(F, sizeof *F);
        mc_shader_use(d->obj);
        mc_set_i(U_K, SH_ICO + 1);
        mc_mesh_set_instances(&d->shapes[SH_ICO], d->inst[SH_ICO], d->inst_count[SH_ICO]);
        mc_gl_switch(MC_BLEND_ON);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        mc_mesh_draw_instanced(d->shapes[SH_ICO], d->inst_count[SH_ICO]);
        mc_gl_switch(MC_BLEND_OFF);
    }
    /* the waterline on the lens, the sea above it laid over the sea below */
    if (up && down) {
        mc_gl_switch(MC_ZTEST_OFF | MC_ZWRITE_OFF | MC_BLEND_ON);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        mc_shader_use(d->dip);
        mc_set_v4(0, mc_vec4(0.5f + 0.5f * dip, (float)t, 0.0f, 0.0f));
        mc_fullscreen_triangle();
        mc_gl_switch(MC_ZTEST_ON | MC_ZWRITE_ON | MC_BLEND_OFF);
    }

    /* post: a whiteout through the clouds, and a fade up from black at the
     * top. At the end the picture over-exposes, blooms and burns out to pure
     * white, and scene 3 starts from that white. The fade up is the eye
     * adjusting. The exposure opens over the first 2 s (e^-6 to 1) under a
     * short fade, so the sun, by far the brightest thing there, comes up
     * first, where Scene 1's last star went out. The stars and the planet
     * follow. */
    mc_post post = mc_post_defaults();
    post.time = (float)t;
    float burn  = mc_smoothstepf((float)S2_END_FADE, (float)SCENE2_DUR, (float)t);
    post.exposure = 1.05f + 2.4f * cloudFlash + (planetCtx ? 0.10f : 0.0f) + 2.5f * burn * burn;
    post.bloom    = 0.09f + 0.5f * cloudFlash + 0.6f * burn;
    post.fade  = mc_smoothstepf(-0.3f, 0.4f, (float)t);
    float u = fminf((float)t - 2.0f, 0.0f);
    post.exposure *= expf(-1.5f * u * u);
    post.white = burn * burn * (3.0f - 2.0f * burn) + cloudFlash * cloudFlash;
    /* the lens's colour fringe fades out as (1 - uAtmo)^2 while the sky behind
     * the planet turns blue. Against the blue, the dark limb picked up a
     * 1-pixel red line. */
    if (planetCtx) post.chroma *= 1.0f - F->atmo * (2.0f - F->atmo);
    if (down) {                              /* murky and soft early, clear and saturated late */
        float k = mc_saturatef(0.5f + 0.5f * dip);   /* (coming in as the waterline sweeps up) */
        post.bloom      += 0.06f * (1.0f - uwClar) * k;
        post.saturation += (0.04f + 0.10f * uwClar) * k;
        post.vignette   += 0.08f * (1.0f - uwClar) * k;
        post.exposure   += 0.10f * uwClar - 0.35f * uw_look(uwU) - 0.10f * bio;   /* the eye stops down for the light overhead */
        /* the scare: as the predator closes in, the frame darkens and loses
         * colour and the vignette tightens, worst at the near miss, which also
         * gets a jolt of chromatic aberration. It eases off as the predator
         * falls behind. */
        if (uwU > ENC_IN && uwU < ENC_OUT) {
            float dread = mc_smoothstepf(ENC_IN + 1.0f, ENC_PEAK, uwU)
                        * (1.0f - 0.65f * mc_smoothstepf(ENC_PEAK + 0.5f, ENC_GIVEUP, uwU))
                        * (1.0f - mc_smoothstepf(ENC_GIVEUP, TRANS_IN + 1.0f, uwU));
            float hit = expf(-(uwU - ENC_PEAK) * (uwU - ENC_PEAK) * 6.0f);
            post.exposure   -= 0.45f * dread;
            post.saturation -= 0.35f * dread;
            post.vignette   += 0.30f * dread;
            post.chroma     += 0.25f * dread + 0.9f * hit;
        }
        /* The ambient occlusion ramps in with the evolution knob, from none
         * early (the flat look) to full by the end. The engine's god rays
         * aren't used: the cameras never look up at the surface itself, so
         * all they would draw is a dashed streak below the blob's highlight.
         * The light in the water is the backdrop's own shafts (water.frag). */
        post.ssao   = qual * 0.70f;
        /* and through the flight the motion blur can reach further, 1% of the
         * screen instead of 0.25%. At 0.25% the close kelp and coral, moving
         * 30-45 px a frame, kept sharp edges and strobed past. */
        post.blur   = 0.01f * mc_smoothstepf(FLIGHT_IN, FLIGHT_IN + 5.0f, uwU);
        post_depth(d, &cam, &post);
    }
    mc_renderer_end(d->R, mc_window_output_fbo(w), ow, oh, post);
}

#include "scene3.c"   /* the jungle: its world, camera and packages */

/* Pick the scene for the global clock and draw it. */
static void render_frame(Demo *d, mc_window *w, double gt) {
    mc_renderer_frame(d->R, gt);
    if (gt < (double)SCENE1_DUR)        render_scene1(d, w, gt);
    else if (gt < (double)SCENE3_START) render_scene2(d, w, gt - (double)SCENE2_START);
    else if (gt < (double)SCENE4_START) render_scene3(d, w, gt - (double)SCENE3_START);
    else                                render_end(d, w, gt - (double)SCENE4_START);
}

/* The renderer sized for the frame at gt in window w: the largest 16:9
 * rectangle in it, at `scale` percent if given (MC_RENDER_SCALE), else the
 * scene's own supersampling (see main). 0 if the targets could not be made. */
static int demo_fit(Demo *d, mc_window *w, double gt, int scale) {
    int ww, hh;
    mc_window_size(w, &ww, &hh);
    int vh = MC_MIN(hh, ww * 9 / 16);
    return mc_renderer_set_scale(d->R, scale ? scale : (gt < SCENE1_DUR || gt >= SCENE4_START ? 216000 : 162000) / MC_MAX(vh, 1080))
        && mc_renderer_resize(d->R, MC_MIN(ww, hh * 16 / 9), vh);
}

/* The load's last step, before the music starts: draw eight frames of the
 * show that are never shown, at the size the show draws them. Every program
 * then draws once into every kind of target it will draw into, where it can't
 * cost a frame anyone sees, and everything sized to the picture gets made. A
 * driver may finish a program only at its first draw, and Mesa's Intel driver
 * builds one again there for a different kind of target. The glass drawing
 * into the key light's map dropped a frame at the bang on a UHD 620 with a
 * cold shader cache. A first use costs too, for example the glass's 66 MB
 * copy of the picture at 4K and the targets sized to it.
 *
 * The times are in half bars:
 *  - Scene 2's planet and its moon
 *  - the dip (the sea above, the waterline, the sea below, the milky blob, its
 *    bubbles, the ambient occlusion)
 *  - the whale overhead
 *  - the reef (weed, fish, coral, boulders, their sun map, the jelly, the moon
 *    jellies over it)
 *  - Scene 3's first light (the forest, its sun maps, the shafts, the birds,
 *    the ladybug)
 *  - the tyrannosaurus on the beach (the sea, its reflections)
 *  - the blob ashore (its sun map, the depth of field)
 *  - and last Scene 1 (its shapes, the glass and its copy of the picture, the
 *    grid, the blob, the key light's map), which leaves the renderer at the
 *    size the show opens with.
 * That covers the show's program, target and blend combinations. Fewer frames
 * won't do, because the planet, the dip, the whale in view and the coral each
 * have a moment to themselves,
 * and so do the birds, the T-rex and the blob's depth of field.
 *
 * Afterwards, whatever those frames leave for later frames is put back the
 * way the load left it, so the show draws the same pictures it would have
 * drawn without them. That means the Frame block (Scene 1 sets only its own
 * terms) and the boulders' detail levels (their hysteresis). Scene 3's still
 * sun maps are kept. light_shadow draws them with the world as it is in the
 * white's last half second, whichever frame draws them, so these are the
 * show's own. The anti-aliasing history needs nothing here.
 * The show's first frame (t ~ 0) and its first frame of Scene 3 (under the
 * white) come before the last ones drawn here, and a step back in the clock
 * starts the history over. A caller starting just after 36.8 s, or entering
 * Scene 3 just after 238.1 s, would carry it on. Scene 1's sim isn't run, so its
 * shapes stand where they start.
 *
 * The cost is about a second of load time on a UHD 620 with a warm shader
 * cache (a few tens of ms on a big GPU). About 0.2 s of that is won straight
 * back, because the show's first frame doesn't have to make the renderer's
 * targets. */
static void demo_warm(Demo *d, mc_window *w, int scale) {
    static const unsigned char at[] = { 64, 72, 95, 105, 162, 227, 246, 38 };
    FrameUniforms keep = d->frame;
    for (int i = 0; i < MC_ARRAY_COUNT(at); ++i) {
        double t = at[i] * (BAR_S / 2.0);
        demo_fit(d, w, t, scale);
        render_frame(d, w, t);
    }
    d->frame = keep;
    memset(d->rock_level, 0, sizeof d->rock_level);
}

/* Setup and teardown. */

static void demo_free(Demo *d);

/* The demo's value noise: random bytes on a wrapping 32^3 lattice, filtered by
 * the hardware, so noise costs one fetch. It lives on unit 1's 3D slot, which
 * nothing else binds. It's made first so every load-time pass (the bakes, the
 * seabed's placement) can read it, as well as Scene 2's planet. */
static GLuint noise_make(void) {
    static unsigned char v[32 * 32 * 32];
    GLuint t;
    mc_rng g = mc_rng_seed(0xC10D5u);
    for (int i = 0; i < 32 * 32 * 32; ++i) v[i] = (unsigned char)(mc_rng_next(&g) >> 24);
    glActiveTexture(GL_TEXTURE1);
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_3D, t);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_R8, 32, 32, 32, 0, GL_RED, GL_UNSIGNED_BYTE, v);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glActiveTexture(GL_TEXTURE0);
    return t;
}

static Demo *demo_make(int render_w, int render_h) {
    Demo *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    MC_PRECALC_START();
    d->R = mc_renderer_make(render_w, render_h);
    if (!d->R || !mc_renderer_set_scale(d->R, 80)) { mc_log("renderer creation failed"); MC_TIDY(free(d)); return NULL; }   /* the TAA upsamples */
    MC_PRECALC_DONE("renderer CPU setup");
    d->noise = noise_make();
    build_shapes(d);
    MC_PRECALC_DONE("mesh generation/upload");
    build_bodies(d);
    build_logo(d);
    MC_PRECALC_DONE("bodies/logo");
    if (!load_shaders(d)) { demo_free(d); return NULL; }
    MC_PRECALC_DONE("demo shader compile/link");
    uw_sculpt(d);
    MC_PRECALC_DONE("the animals sculpted");
    if (!bake_underwater(d)) { demo_free(d); return NULL; }
    MC_PRECALC_DONE("material bake submission");
    uw_world(d);
    MC_PRECALC_DONE("schools");
    pred_build(d);
    MC_PRECALC_DONE("predator path");
    uw_place(d);
    MC_PRECALC_DONE("seabed props");
    if (!s3_make(d)) { demo_free(d); return NULL; }
    MC_PRECALC_DONE("jungle");
    reseed_loop(d, 0);     /* loop 0, the show's only one: the blue blob and its collisions */
#ifndef MC_RELEASE
    scan_shaders(d->shader_hashes);
    memcpy(d->shader_attempted, d->shader_hashes, sizeof d->shader_hashes);
#endif
    /* Size every instance buffer for the bodies plus the logo cubes up front
     * (SH_BOX holds both), so nothing reallocates mid-animation. That wastes a
     * little on the other shape types. */
    for (int i = 0; i < SH_COUNT; ++i) {
        d->inst[i] = malloc(sizeof(mc_instance) * (NBODY + MAX_LOGO));
        if (!d->inst[i]) mc_fatal("out of memory allocating instance buffer");
        mc_mesh_reserve_instances(&d->shapes[i], NBODY + MAX_LOGO);
    }
    mc_mesh_reserve_instances(&d->blob, 1);
    mc_mesh_reserve_instances(&d->uw_seabed, 1);
    for (int i = 0; i < 3; ++i) mc_mesh_reserve_instances(&d->uw_rock[i], UW_MAXP);
    mc_mesh_reserve_instances(&d->uw_fish, 300);   /* the circling schools' UW_MAXF, or the big school's 300 (fish.vert) */
    mc_mesh_reserve_instances(&d->uw_algae, UW_MAXP);
    mc_mesh_reserve_instances(&d->uw_whale, 1);
    mc_mesh_reserve_instances(&d->uw_pred, 1);
    mc_mesh_reserve_instances(&d->uw_crab, UW_MAXP);
    mc_mesh_reserve_instances(&d->uw_coral, UW_MAXP);
    d->uw_shadow = mc_target_make_format(UW_SHADOW_RES, UW_SHADOW_RES, 0, 1);   /* sun shadow map */
    if (!mc_target_ok(&d->uw_shadow)) { demo_free(d); return NULL; }
    glBindTexture(GL_TEXTURE_2D, d->uw_shadow.depth);   /* compared, 2 x 2 filtered (shadowVis) */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return d;
}

static void demo_free(Demo *d) {
#ifdef MC_IMPORT_HASH
    (void)d;      /* the shipped payload: exiting the process releases it all */
#else
    s3_free(d);
    for (int i = 0; i < 3; ++i) mc_mesh_gpu_free(&d->uw_rock[i]);
    for (int i = 0; i < SH_COUNT; ++i) { mc_mesh_gpu_free(&d->shapes[i]); free(d->inst[i]); }
    mc_mesh_gpu_free(&d->ground);
    mc_mesh_gpu_free(&d->blob);
    mc_mesh_gpu_free(&d->planet);
    mc_mesh_gpu_free(&d->ocean);
    mc_mesh_gpu_free(&d->uw_seabed); glDeleteBuffers(1, &d->uw_buf);
    mc_mesh_gpu_free(&d->uw_fish);   mc_mesh_gpu_free(&d->uw_algae);
    mc_mesh_gpu_free(&d->uw_whale);
    mc_mesh_gpu_free(&d->uw_pred); mc_mesh_gpu_free(&d->uw_crab); mc_mesh_gpu_free(&d->uw_coral);
    glDeleteBuffers(1, &d->uw_kit); mc_mesh_gpu_free(&d->jr_body);
    mc_target_free(&d->uw_shadow);
    { GLuint t[] = { d->tex_sand, d->tex_rock, d->tex_fishskin, d->tex_whaleskin, d->noise };
      glDeleteTextures(5, t); }
    mc_shader_free(&d->sky); mc_shader_free(&d->obj);
    mc_shader_free(&d->grnd); mc_shader_free(&d->blobsh);
    mc_shader_free(&d->space); mc_shader_free(&d->planet_sh);
    mc_shader_free(&d->atmo); mc_shader_free(&d->moon_sh); mc_shader_free(&d->ocean_sh); mc_shader_free(&d->water); mc_shader_free(&d->dip);
    mc_shader_free(&d->uwsurf); mc_shader_free(&d->uwrock); mc_shader_free(&d->fishsh);
    mc_shader_free(&d->algaesh); mc_shader_free(&d->whalesh); mc_shader_free(&d->coralsh); mc_shader_free(&d->uwplace);
    mc_shader_free(&d->shadowsh); mc_shader_free(&d->jellysh);
    mc_mesh_gpu_free(&d->uw_jelly);
    mc_shader_free(&d->shadowrock);
    mc_shader_free(&d->shadowfish); mc_shader_free(&d->shadowcoral); mc_shader_free(&d->shadowalgae);
    mc_renderer_free(d->R);
    free(d);
#endif
}

/* The audio thread calls this to get its next block of samples. */
#if !defined(MC_HEADLESS) && !defined(MC_CAPTURE)
static void audio_pull(float *out, int frames, void *user) {
    mc_synth_render((mc_synth *)user, out, frames);
}
#endif

/* Dev transport: a small scrubber for moving round the timeline while working
 * on the demo. None of it is in the release binary. The first key press takes
 * the clock away from the audio. SPACE pauses, LEFT/RIGHT jump 5 s back or
 * forward, and UP/DOWN double or halve the speed. */
#if !defined(MC_RELEASE) && !defined(MC_HEADLESS) && !defined(MC_CAPTURE)
/* The timeline's named sections. On each key press the scrubber prints the
 * one the new position falls in. They line up with the T_* constants at the
 * top of the file. */
static const struct { double t; const char *name; } SECTIONS[] = {
    { 0.0,                        "s1 intro"      },
    { T_BANG,                     "s1 bang"       },
    { T_HOOKA,                    "s1 hook A"     },
    { T_BREAK,                    "s1 breakdown"  },
    { T_HOOKB,                    "s1 hook B"     },
    { T_LOGO_IN,                  "s1 logo"       },
    { T_COLLAPSE,                 "s1 collapse"   },
    { SCENE1_DUR,                 "s2 space"      },
    { SCENE1_DUR + S2_SPACE_END,  "s2 dive"       },
    { SCENE1_DUR + S2_CLOUDFLASH, "s2 fall"       },
    { SCENE1_DUR + S2_BREACH,     "s2 underwater" },
    { SCENE1_DUR + S2_BREACH + 24.4 * BAR_S,     "s2 predator"   },   /* uw_t(ENC_IN) ... */
    { SCENE1_DUR + S2_BREACH + 27.5 * BAR_S,     "s2 near miss"  },
    { SCENE1_DUR + S2_BREACH + 30.7 * BAR_S,     "s2 transform"  },
    { SCENE1_DUR + S2_BREACH + 36.15 * BAR_S,    "s2 flight"     },
    { SCENE1_DUR + S2_END_FADE,   "s2 burn-out"   },
    { SCENE3_START,               "s3 white"      },
    { SCENE3_START + S3_J0,       "s3 jungle"     },
    { SCENE3_START + S3_J0 + 8 * BAR_S,  "s3 canopy"  },   /* the story's S3_UP, ... stretched (S3_SEG) */
    { SCENE3_START + S3_J0 + 13 * BAR_S, "s3 leaf"    },
    { SCENE3_START + S3_J0 + 16 * BAR_S, "s3 ladybug" },
    { SCENE3_START + S3_J0 + 24 * BAR_S, "s3 take-off"},
    { SCENE3_START + S3_J0 + 26 * BAR_S, "s3 glide"   },
    { SCENE3_START + S3_J0 + 30 * BAR_S, "s3 beach"   },
    { SCENE3_START + S3_J0 + 31 * BAR_S, "s3 t-rex"   },
    { SCENE3_START + S3_J0 + 39 * BAR_S, "s3 edge"    },
    { SCENE3_START + S3_J0 + 41 * BAR_S, "s3 blob"    },
    { SCENE4_START,               "end card"      },
};
static const char *section_name(double t) {
    const char *n = SECTIONS[0].name;
    for (int i = 0; i < MC_ARRAY_COUNT(SECTIONS); ++i)
        if (t >= SECTIONS[i].t) n = SECTIONS[i].name;
    return n;
}

typedef struct {
    int    engaged;              /* 1 once the user has grabbed the timeline */
    int    paused;
    double timeline;             /* manual position (seconds) */
    double speed;
    double wall_prev;            /* last wall-clock sample, for dt            */
    int    prev[MC_KEY_COUNT];   /* previous key state, for edge detection    */
} Transport;

static int key_edge(mc_window *w, Transport *tr, mc_key k) {
    int now = mc_key_down(w, k), edge = now && !tr->prev[k];
    tr->prev[k] = now;
    return edge;
}

/* Turn the transport keys into a timeline position in seconds. Until a key is
 * pressed it returns `live` (the real audio or wall clock) unchanged, so a
 * normal run isn't affected. */
static double dev_transport(mc_window *w, Transport *tr, double live) {
    double now = mc_time_seconds();
    double wdt = now - tr->wall_prev;
    tr->wall_prev = now;

    int sp = key_edge(w, tr, MC_KEY_SPACE);
    int sl = key_edge(w, tr, MC_KEY_LEFT);
    int sr = key_edge(w, tr, MC_KEY_RIGHT);
    int su = key_edge(w, tr, MC_KEY_UP);
    int sd = key_edge(w, tr, MC_KEY_DOWN);

    if ((sp || sl || sr || su || sd) && !tr->engaged) {
        tr->engaged  = 1;
        tr->timeline = live;     /* grab the clock wherever it happens to be */
    }
    if (!tr->engaged) return live;

    if (sp) tr->paused = !tr->paused;
    if (su) tr->speed  = tr->speed < 4.0   ? tr->speed * 2.0 : 4.0;
    if (sd) tr->speed  = tr->speed > 0.125 ? tr->speed * 0.5 : 0.125;
    if (sl) tr->timeline -= 5.0;
    if (sr) tr->timeline += 5.0;
    if (!tr->paused) tr->timeline += wdt * tr->speed;
    if (tr->timeline < 0.0) tr->timeline = 0.0;

    if (sp || sl || sr || su || sd) {
        double gt = tr->timeline > TOTAL_DUR ? TOTAL_DUR : tr->timeline;
        mc_log("transport: %6.1fs  x%-5.3g %s  [%s]",
               gt, tr->speed, tr->paused ? "paused" : "play  ", section_name(gt));
    }
    return tr->timeline;
}
#endif /* dev transport */

/* main(). Three of them, picked at compile time: one that streams the show as
 * video, one that dumps PNGs and quits, one that opens a window and runs it. */

#define SONG_SEED 0xB16BA75u   /* the synth's seed: the song as it plays */

#if defined(MC_CAPTURE)
/* Video-capture main. It renders the whole show offline at 1920x1080 and 60
 * fps and streams every frame as raw RGBA to stdout, so it can be piped
 * straight into ffmpeg (make capture: tools/capture.sh). All progress goes to
 * stderr so stdout stays a clean pixel stream. Two environment variables:
 * MC_CAP_DUR (seconds, default the whole show) and MC_CAP_SCALE (the internal
 * resolution for the whole show, 25-200 percent of the frame, default 200).
 * At 200 every frame is rendered from 2160 lines, as the release renders
 * Scene 1: the TAA resolves at that size and the present pass filters it
 * down, which gives a sharp video. 100 renders about twice as fast, for a
 * preview. With MC_CAP_AUDIO=<file> it writes the soundtrack there instead
 * and quits. The soundtrack is raw float32 stereo at MC_SAMPLE_RATE, as long
 * as the frames, and silent past the show's end, like the release.
 * It's built headless (surfaceless EGL). */
#include <stdlib.h>
static int   cap_envi(const char *k, int d)    { const char *v = getenv(k); return v ? atoi(v) : d; }
static double cap_envf(const char *k, double d){ const char *v = getenv(k); return v ? atof(v) : d; }
static int cap_audio(const char *path, double dur) {
    mc_synth *s = mc_synth_make(SONG_SEED);
    FILE *f = s ? fopen(path, "wb") : NULL;
    if (!f) { fprintf(stderr, "[capture] could not write the soundtrack to %s\n", path); if (s) mc_synth_free(s); return 1; }
    static float buf[MC_CHANNELS * 4096];
    long total = (long)(dur * MC_SAMPLE_RATE), song = (long)(TOTAL_DUR * MC_SAMPLE_RATE), at = 0;
    int ok = 1;
    while (ok && at < total) {
        int n = total - at < 4096 ? (int)(total - at) : 4096;
        if (at < song) mc_synth_render(s, buf, n);
        for (int i = 0; i < n; ++i)                       /* the music stops with the show */
            if (at + i >= song) buf[MC_CHANNELS * i] = buf[MC_CHANNELS * i + 1] = 0.0f;
        ok = fwrite(buf, sizeof(float) * MC_CHANNELS, (size_t)n, f) == (size_t)n;
        at += n;
    }
    ok = fclose(f) == 0 && ok;
    mc_synth_free(s);
    if (ok) fprintf(stderr, "[capture] soundtrack: %.1f s -> %s\n", dur, path);
    else    fprintf(stderr, "[capture] soundtrack: writing %s failed\n", path);
    return !ok;
}
int main(void) {
    const int    W = 1920, H = 1080;
    const double fps = 60.0;
    double dur = cap_envf("MC_CAP_DUR", (double)TOTAL_DUR);
    int    sc  = cap_envi("MC_CAP_SCALE", 200);
    if (dur < 0.0 || sc < 25 || sc > 200) {
        fprintf(stderr, "[capture] MC_CAP_DUR or MC_CAP_SCALE (25-200) out of range\n");
        return 2;
    }
    if (getenv("MC_CAP_AUDIO")) return cap_audio(getenv("MC_CAP_AUDIO"), dur);
    mc_window_desc desc = { .width = W, .height = H, .title = "microcosm capture", .vsync = 0 };
    mc_window *w = mc_window_open(&desc);
    if (!w) { fprintf(stderr, "[capture] could not open %dx%d surface\n", W, H); return 1; }
    Demo *d = demo_make(W, H);
    if (!d) { mc_window_close(w); return 1; }
    if (!mc_renderer_set_scale(d->R, sc)) {             /* once: no scene switches it */
        fprintf(stderr, "[capture] could not render at %d%% of %dx%d\n", sc, W, H);
        demo_free(d); mc_window_close(w); return 1;
    }
    int rw, rh;
    mc_renderer_size(d->R, &rw, &rh);
    size_t nbytes = (size_t)W * (size_t)H * 4u;
    unsigned char *buf = malloc(nbytes);
    if (!buf) { demo_free(d); mc_window_close(w); return 1; }
    long frames = (long)(dur * fps);
    int result = 0;
    fprintf(stderr, "[capture] %dx%d @ %.3g fps, %.1f s -> %ld frames (raw rgba on stdout), rendered at %dx%d (%d%%)\n",
            W, H, fps, dur, frames, rw, rh, sc);
    for (long i = 0; i < frames; ++i) {
        double t = (double)i / fps;
        if (t < (double)SCENE1_DUR) sim_advance(d, t);   /* collision sim is scene 1 only */
        render_frame(d, w, t);
        mc_window_swap(w);
        mc_window_read_pixels(w, buf);                   /* top-left-origin RGBA8 */
        if (fwrite(buf, 1, nbytes, stdout) != nbytes) { fprintf(stderr, "[capture] stdout write failed\n"); result = 1; break; }
        if ((i & 63) == 0) fprintf(stderr, "[capture] %ld/%ld (t=%.1f)\n", i, frames, t);
    }
    free(buf);
    demo_free(d);
    mc_window_close(w);
    return result;
}
#elif defined(MC_HEADLESS)
int main(void) {
    mc_window_desc desc = { .width = RENDER_W, .height = RENDER_H, .title = "microcosm", .vsync = 0 };
    mc_window *w = mc_window_open(&desc);
    if (!w) return 1;
    Demo *d = demo_make(RENDER_W, RENDER_H);
    if (!d) { mc_window_close(w); return 1; }


    /* One grab per beat of the show. Scene 1: the bang, the busy middle, the
     * logo and the tail. Scene 2: space, the fall and the underwater journey
     * from its murky start to the finale. Then scene 3's beats and the end
     * card. */
    #define S2U(u) (SCENE2_START + uw_t(u))              /* underwater story time -> global */
    #define S3J(j) (SCENE3_START + S3_J0 + BAR_S * unstretch((j) / (float)BAR_S, S3_SEG))   /* jungle story time -> global */
    const double times[] = {
        3.0, T_BANG + 0.7, T_HOOKA + 4.0, T_LOGO_IN + 4.2, T_COLLAPSE + 0.6,   /* scene 1 */
        SCENE2_START + 1.5, SCENE2_START + S2_CLOUDFLASH + 1.4,   /* s2 space + fall */
        S2U(4.3), S2U(22.3), S2U(40.3),                     /* s2 underwater: murky -> clearing (+whale) */
        S2U(ENC_LUNGE), S2U(ENC_PEAK),                      /* the encounter: lunge, near miss */
        S2U(ENC_PEAK + 3.0),                                /* the panicked bolt, the predator in pursuit */
        S2U(76.3), S2U(81.3),                               /* the transformation: camera orbits, eyes grow, shape rounds */
        S2U(89.3), S2U(97.3),                               /* the flight: more and more scenery rushes past */
        S3J(3.0f), S3J(12.0f),                                       /* s3: the jungle, the look round */
        S3J(S3_UP + 4.0f),                                           /* the canopy, birds overhead */
        S3J(S3_MACRO + 4.0f),                                        /* the ladybug on its leaf */
        S3J(S3_FLY + 1.5f),                                          /* ... taking off */
        S3J(S3_GAP + 0.5f),                                          /* the beach between two trunks */
        S3J(S3_REX + 3.5f),                                          /* the tyrannosaurus passing */
        S3J(S3_BLOB + 6.0f),                                         /* the blob coming ashore */
        SCENE4_START + 9.4 * BAR_S / 4.0,                            /* the end card: the blob on MIC */
    };
    #undef S2U
    #undef S3J
    for (int i = 0; i < MC_ARRAY_COUNT(times); ++i) {
        if (times[i] < SCENE1_DUR) sim_advance(d, times[i]);  /* sim is scene 1 only */
        render_frame(d, w, times[i]);
        mc_window_swap(w);
        char path[64];
        snprintf(path, sizeof path, "build/frame_%02d.png", i);
        mc_window_save_png(w, path);
        mc_log("saved %s (t=%.1f)", path, times[i]);
    }
    demo_free(d);
    mc_window_close(w);
    return 0;
}
#else
int main(void) {
    /* The shipped build is the show: fullscreen at the desktop's resolution,
     * no pointer. The dev build stays a window. */
    mc_window_desc desc = { .width = WIN_W, .height = WIN_H, .title = "MICROCOSM - Hello World", .vsync = 1,
#if defined(MC_RELEASE)
                            .fullscreen = 1
#endif
    };
    mc_window *w = mc_window_open(&desc);
    if (!w) return 1;
    /* Show opaque black through the load (shader compiles, bakes, the
     * sculpting, the seabed's placement), not whatever the driver or the
     * compositor shows in a window nothing has been drawn into yet. */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    mc_window_swap(w);
    Demo *d = demo_make(WIN_W, WIN_H);     /* the loop below sizes it to the window */
    if (!d) { MC_TIDY(mc_window_close(w)); return 1; }
    /* The internal resolution: MC_RENDER_SCALE for the whole show if it's
     * set. Otherwise it's 0 here and demo_fit picks it per scene (below). */
    long scale = 0;
    const char *scale_text = getenv("MC_RENDER_SCALE");
    if (scale_text) {
        char *end;
        scale = strtol(scale_text, &end, 10);
        if (end == scale_text || *end || scale < 25 || scale > 200) {
            mc_log("MC_RENDER_SCALE must be an integer from 25 to 200");
            MC_TIDY(demo_free(d); mc_window_close(w)); return 1;
        }
    }

    demo_warm(d, w, (int)scale);           /* the show's first draws, still in the load (the music waits) */

    /* Audio is optional: if the synth can't be created we fall back to the
     * wall clock, and mc_audio_start already tolerates a NULL backend. */
    mc_synth *synth = mc_synth_make(SONG_SEED);
    mc_audio *audio = synth ? mc_audio_start(audio_pull, synth) : NULL;

    double start = mc_time_seconds();
    int audio_stopped = 0;
    int result = 0;
#if !defined(MC_RELEASE) && !defined(MC_HEADLESS)
    Transport tr = { .speed = 1.0 };
    tr.wall_prev = start;
#endif
    while (mc_window_pump(w)) {
        /* One monotonic clock that never wraps. The show runs scene 1, scene
         * 2, scene 3 and the end card, then holds on a black frame. Every cut
         * is hidden. Scene 1 fades to black and scene 2 fades up from it, and
         * scene 2 burns out to white and scene 3 opens out of the white. The
         * end card comes up out of scene 3's black. */
        double raw = audio ? mc_audio_time(audio) : (mc_time_seconds() - start);
#if !defined(MC_RELEASE) && !defined(MC_HEADLESS)
        raw = dev_transport(w, &tr, raw);   /* pause / seek / speed while debugging */
#endif
        double gt = raw;
        if (gt > TOTAL_DUR) gt = TOTAL_DUR;   /* past the end: freeze on black */
        if (gt < SCENE1_DUR) sim_seek(d, gt); /* the collision sim is scene 1 only */

        /* Match the render resolution to the (possibly resized) window so the
         * image is pixel-sharp, but keep it 16:9, the shape every shot is
         * composed for. On a screen of another shape the picture keeps 16:9,
         * centred between black bars (mc_renderer_end centres the renderer's
         * output in the window), and so does the camera's aspect. It's
         * supersampled for the party's RTX 5080 target: its expected headroom
         * at 1080p is extrapolated, not measured here. MC_RENDER_SCALE=80 is
         * the setting for an integrated GPU. Scene 1 is the cheap one (see
         * docs/making-of.org), so
         * it and the end card, which is the same kind of frame, render 2160
         * lines, and Scenes 2 and 3 render 1620. That's 200% and 150% of a
         * 1080p picture and less of a larger one, so a 4K desktop gets 100%
         * and 75%, the same frames rather than 4x the work. It's never more
         * than 200%. The scale switches fall in the blacks either side, and a
         * resize starts the TAA's history over anyway. */
        if (!demo_fit(d, w, gt, (int)scale)) {
            mc_log("renderer resize failed"); result = 1; break;
        }
        maybe_reload_shaders(d);      /* dev hot-reload, a no-op in release */
        render_frame(d, w, gt);
        mc_window_swap(w);

        /* At the end, stop the music once, so the final black frame is silent
         * and the song doesn't loop under it. */
        if (gt >= TOTAL_DUR && audio && !audio_stopped) {
            MC_TIDY(mc_audio_stop(audio));   /* (the payload exits right after) */
            audio = NULL;
            audio_stopped = 1;
        }
        /* The show has played once. The shipping build exits here, so the
         * program ends with the show instead of holding a black frame for
         * ever. The dev build keeps running, frozen on black, so the scrubber
         * still works for looking at the ending. */
#if defined(MC_RELEASE)
        if (raw >= TOTAL_DUR) break;   /* release auto-exits when the show ends */
#endif
    }

    /* the shipped payload just exits: the OS releases it all */
    MC_TIDY(if (audio) mc_audio_stop(audio); if (synth) mc_synth_free(synth); demo_free(d); mc_window_close(w));
    return result;
}
#endif
