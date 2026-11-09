/*
 * scene3.c - Scene 3: the jungle. #included by main.c (one translation unit).
 *
 * Scene 2 burns out to white, and Scene 3 holds the white for a bar before
 * it opens on the floor of a rainforest. We stand among giant buttressed
 * trees, lianas and ferns, with shafts of sun coming down through the haze.
 * The camera looks round the clearing and up the trunks to the canopy, where
 * birds cross a gap in the leaves, then comes back down to a broad leaf right
 * beside us with a ladybug walking on it. We go in close, and the ladybug
 * opens its wing cases and takes off towards the light.
 *
 * The camera lets it go and glides off between the trunks until two of them
 * frame a view of a beach. A tyrannosaurus gradually emerges between their
 * trunks, stops and lowers its open jaws toward us before walking on in the
 * shade at the forest's edge. Once it has gone we step out to the last row
 * of trees. The blob from scenes 1 and 2, now with two tiny arms, comes out
 * of the surf and hops up the sand, and we walk out of the trees to meet it
 * while the lens lengthens. It waves, turns to the sea as a bigger wave runs
 * up, and hops back down to play in the swash. It jumps the foaming edge of
 * the next wave and stamps in the water, then comes back up, shivers itself
 * dry and sits to watch the waves. The camera rises back among the trees for
 * a last wide view as the last waves lap round it, and the scene fades out.
 *
 * Positions are in metres on a gently rolling floor (s3_ground), with the
 * viewer near the origin. The work is split into packages, one file and one
 * shader prefix each, which all draw through the same hooks and are lit the
 * same way:
 *
 *   s3_trees.c (jt_*)  the giant trees, their crowns, palms and lianas
 *   s3_under.c (ju_*)  the forest floor and the undergrowth: litter, grass, ferns
 *   s3_light.c (jl_*)  sun, sky, shadows, haze, light shafts, the post grade
 *   s3_bug.c   (jb_*)  the hero plant, its leaf and the ladybug
 *   s3_birds.c (jv_*)  the birds
 *   s3_beach.c (jw_*)  the beach and the sea (and the floor's sand, in ju_ground)
 *   s3_rex.c   (jr_*)  the tyrannosaurus
 *   s3_blob.c  (jo_*)  the blob on the beach
 *
 * Each package lists its programs in its own s3_<pkg>.def manifest. Every
 * jungle shader carries the same "s3 common" GLSL block: the Frame block, the
 * ground, the sun's shadow, the lighting and the haze. Scene 3 uses the Frame
 * block's fields like this:
 *   uViewProj / uLightVP   camera / sun (uViewProj = uLightVP in the shadow pass)
 *   uCam, uTime            eye position; scene-3 story seconds (c->t)
 *   uLightDir, uGlow       direction toward the sun; its radiance
 *   uSkyColor, uGroundColor ambient from above (sky through leaves) / below
 *   uFogColor, uFog        the haze colour and density (per metre)
 *   uTop, uBottom          the sky's zenith / horizon
 *   uWobble                the wind (0 calm .. 1 gusty)
 *   uClarity               the macro amount (0 wide .. 1 at the ladybug)
 *   uReal                  the beach part's progress (0 in the forest .. 1 from S3_GAP)
 *   uRim                   the beach's sun map: uv = clearing-map uv * x + yz
 *   uSun                   the characters' sun map: uv = clearing-map uv * x + yz
 *   uBlobFog, uGape        how much sun the character on show lets through (0.3
 *                          for the blob's jelly), and its middle's depth in that map
 *   uQ                     1 in the sun-map passes (shaders only need depth / cut-out)
 *   uRes                   render size
 */

/* The timeline. Scene 3 opens with a bar of white, then 54 bars of jungle.
 * The cues are in jungle seconds (time since S3_J0) and sit on bar lines.
 * The show plays them over 58 bars of the synth's fourth movement. The
 * forest part is stretched: the look round, the canopy, the ladybug and the
 * glide each get one extra bar (S3_SEG below, and stretch() in main.c). From
 * the beach's hush on, the cues run at their own pace, four bars behind. */
#define S3_J0      ((float)BAR_S)           /* ~1.9 s of white, then the jungle opens */
#define S3_REVEAL  2.4f                     /* white fades to the picture as eyes adjust */
#define S3_UP      ((float)(7 * BAR_S))     /* ~13.5: the gaze climbs into the canopy, and birds */
#define S3_FIND    ((float)(11 * BAR_S))    /* ~21.3: down and round to the hero plant */
#define S3_MACRO   ((float)(14 * BAR_S))    /* ~27.1: in close on the ladybug */
#define S3_FLY     ((float)(21 * BAR_S))    /* ~40.6: it opens its wings and takes off */
#define S3_ROAM    ((float)(23 * BAR_S))    /* ~44.5: the camera lets it go and glides off */
#define S3_GAP     ((float)(26 * BAR_S))    /* ~50.3: two trunks frame a glimpse of the beach */
#define S3_REX     ((float)(27 * BAR_S))    /* ~52.3: the tyrannosaurus walks past */
#define S3_SHORE   ((float)(31 * BAR_S))    /* ~60.0: it walks on behind the trunk, and we stay */
#define S3_HOLD    4                        /* crouched for four more bars. Every later cue */
                                            /* is pushed back by them, here and in jo_blob.vert, */
                                            /* jo_blob.frag, ju_ground.frag and the surf. */
#define S3_CLEAR   ((float)((31 + S3_HOLD) * BAR_S))   /* ~67.7: it has gone, and we step out */
#define S3_BLOB    ((float)((33 + S3_HOLD) * BAR_S))   /* ~71.6: the blob rises out of the surf */
#define S3_END     ((float)((50 + S3_HOLD) * BAR_S))   /* ~104.5: the end of Scene 3 (it waves from bar 42, plays from 44) */
#define S3_OUT     (S3_END - 2.0f)          /* the last fade to black */

/* Where things are. The viewer stands near the origin (see the camera keys).
 * At the start the hero plant grows about 1.3 m behind them to the right.
 * Yaw 0 looks down -z, and positive yaw turns right (toward +x). */
#define S3_HERO_X  1.27f                    /* the hero plant's base */
#define S3_HERO_Z  0.46f
#define S3_KEEP_R  0.55f                    /* nothing else grows this close to it */
/* The beach lies at yaw 60, a direction the look-round never faces.
 * s3_along() is the distance toward the sea, and s3_across() the distance to
 * the right when looking out. The forest stops at S3_EDGE, and the sand slopes
 * down through the waterline into the sea (s3_ground). The tyrannosaurus
 * walks the edge from left to right along S3_REX_S, putting a foot down every
 * two beats. The blob comes out of the sea at S3_BLOB_ACROSS. Before all
 * that, the camera glides out to S3_VANTAGE. */
#define S3_SEA_X   0.8660254f               /* toward the sea: s3_dir(60, 0) */
#define S3_SEA_Z   (-0.5f)
#define S3_EDGE    32.0f                    /* the forest's edge */
#define S3_SHORE_S 50.0f                    /* the waterline */
#define S3_SEA_Y   (-1.0f)                  /* sea level */
#define S3_REX_S   34.5f                    /* the tyrannosaurus's track */
#define S3_VANTAGE 13.5f                    /* where the glide stops, ~21 m back from the track */
#define S3_BLOB_ACROSS 1.5f
#define S3_STEP    ((float)(BAR_S * 0.5))   /* the tyrannosaurus's footfall period (two beats) */

/* The pass a draw hook is called for. S3_SHADOW draws the cached sun maps of
 * the still world, and S3_LIT the lit picture. S3_CHAR is the characters' own
 * sun map, redrawn every frame: the ladybug, the tyrannosaurus and the blob
 * draw their depth there, as in S3_SHADOW. S3_LATE comes after the sky, for
 * things that look through the finished picture. */
enum { S3_SHADOW, S3_LIT, S3_CHAR, S3_LATE };

/* Everything a package needs to draw a frame. */
typedef struct {
    float     t, j;       /* scene-3 story seconds; jungle seconds (t - S3_J0, < 0 in the white) */
    float     macro;      /* 0 wide .. 1 at the ladybug */
    float     beach;      /* 0 in the forest .. 1 from S3_GAP (the beach part) */
    float     focus;      /* focus distance (m) for the depth of field */
    mc_camera cam;
    mc_m4     vp;
    mc_v3     fwd;        /* unit view direction */
    mc_frustum fr;        /* the camera frustum (s3_visible) */
    int       pass;       /* S3_SHADOW, S3_LIT, S3_CHAR or S3_LATE */
} S3Ctx;

static float s3_along(float x, float z)  { return x * S3_SEA_X + z * S3_SEA_Z; }
static float s3_across(float x, float z) { return z * S3_SEA_X - x * S3_SEA_Z; }
static mc_v3 s3_beach_pt(float along, float across) {   /* a point on the floor */
    float x = along * S3_SEA_X - across * S3_SEA_Z, z = along * S3_SEA_Z + across * S3_SEA_X;
    return mc_vec3(x, 0.0f, z);
}

/* The floor's height at (x, z): the rolling forest floor, then the beach
 * sloping down into the sea. s3h() in the GLSL common block mirrors it
 * exactly, so geometry placed on the CPU and grass grown on the GPU agree. */
static float s3_ground(float x, float z) {
    float s = s3_along(x, z);
    return mc_lerpf(0.30f * sinf(0.11f * x + 0.7f) * sinf(0.13f * z + 0.3f)
                  + 0.12f * sinf(0.31f * x - 0.23f * z)
                  + 0.05f * sinf(0.83f * z + 0.57f * x + 1.3f),
                    (S3_SHORE_S - s) * 0.06f + S3_SEA_Y, mc_smoothstepf(S3_EDGE - 2.0f, S3_EDGE + 4.0f, s));
}

/* The trees. First the hero trunks, set where the look-round frames them,
 * then rings of jittered sites out into the haze, then the understory (small
 * palms and saplings close in). The beach part's own trees come last: the
 * pair framing the view out, the giants on the edge and the palms on the
 * sand. kind is 0 for a giant buttressed broadleaf, 1 for a palm, 2 for a
 * slender understory tree or sapling, and 3 for a tree that's gone (cleared
 * for the beach). Gone trees stay in the array so the others keep their
 * index, which is their seed. r is the trunk radius at chest height. */
typedef struct { mc_v3 p; float r, h; int kind; float lod; } S3Tree;   /* lod: s3_tree_lod */
#define S3_NFOREST 300     /* the forest's own sites ... */
#define S3_MAXTREE 320     /* ... and room for the beach part's */
static S3Tree s3_trees[S3_MAXTREE];
static int    s3_ntree, s3_nforest;   /* all trees; the ones before the beach part's own */

/* Sky window: the canopy stays open around this direction, and the birds
 * cross it while the camera looks up. Crowns keep out of a cone of
 * S3_GAP_COS. */
#define S3_GAP_YAW   (-140.0f)
#define S3_GAP_PITCH 56.0f
#define S3_GAP_COS   0.94f

static mc_v3 s3_dir(float yaw_deg, float pitch_deg) {
    float y = mc_radians(yaw_deg), p = mc_radians(pitch_deg);
    return mc_vec3(sinf(y) * cosf(p), sinf(p), -cosf(y) * cosf(p));
}

/* 1 if a disc of radius r at (x, z) keeps clear of every trunk, the hero plant
 * and the viewer's standing spot. */
static int s3_clear(float x, float z, float r) {
    for (int i = 0; i < s3_ntree; ++i) {
        if (s3_trees[i].kind > 2) continue;
        float dx = x - s3_trees[i].p.x, dz = z - s3_trees[i].p.z, rr = r + s3_trees[i].r * 1.6f;
        if (dx * dx + dz * dz < rr * rr) return 0;
    }
    float hx = x - S3_HERO_X, hz = z - S3_HERO_Z, hr = r + S3_KEEP_R;
    if (hx * hx + hz * hz < hr * hr) return 0;
    return x * x + (z - 1.5f) * (z - 1.5f) > (r + 0.35f) * (r + 0.35f) * 4.0f;   /* the camera's path */
}

static float s3_roam_dist(float x, float z);   /* the camera's glide (below) */

static void s3_tree_sites(void) {
    static const struct { float yaw, dist, r, h; int kind; } HERO[] = {
        { -20.0f, 13.0f, 1.15f, 44.0f, 0 },   /* the first giant, backlit in the haze */
        { -62.0f,  6.5f, 0.95f, 40.0f, 0 },   /* close, with big buttresses */
        {-105.0f, 10.0f, 0.60f, 34.0f, 2 },
        {-150.0f,  7.5f, 0.22f, 15.0f, 1 },   /* a palm */
        {  30.0f,  9.0f, 1.00f, 42.0f, 0 },
        {  72.0f,  5.0f, 0.20f, 13.0f, 1 },
        { 165.0f, 11.0f, 0.90f, 38.0f, 0 },
        {-200.0f,  8.0f, 0.45f, 26.0f, 2 },
    };
    s3_ntree = 0;
    for (int i = 0; i < MC_ARRAY_COUNT(HERO); ++i) {
        mc_v3 dir = s3_dir(HERO[i].yaw, 0.0f);
        float x = dir.x * HERO[i].dist, z = dir.z * HERO[i].dist;
        s3_trees[s3_ntree++] = (S3Tree){ mc_vec3(x, s3_ground(x, z), z), HERO[i].r, HERO[i].h, HERO[i].kind, 0.0f };
    }
    mc_rng rng = mc_rng_seed(0x7A6613u);
    for (int tries = 0; tries < 4000 && s3_ntree < 160; ++tries) {
        float a = mc_rng_range(&rng, 0.0f, MC_TAU), dist = 8.0f + 70.0f * powf(mc_rng_f(&rng), 1.3f);
        float x = cosf(a) * dist, z = sinf(a) * dist, u = mc_rng_f(&rng);
        int kind = u < 0.40f ? 0 : u < 0.62f ? 1 : 2;
        float r = kind == 0 ? mc_rng_range(&rng, 0.55f, 1.2f) : kind == 1 ? mc_rng_range(&rng, 0.15f, 0.24f)
                                                                         : mc_rng_range(&rng, 0.18f, 0.45f);
        int ok = 1;
        for (int k = 0; k < s3_ntree && ok; ++k) {
            float dx = x - s3_trees[k].p.x, dz = z - s3_trees[k].p.z, gap = (r + s3_trees[k].r) * 3.0f + 1.5f;
            ok = dx * dx + dz * dz > gap * gap;
        }
        if (!ok || dist < 4.5f) continue;
        float h = kind == 0 ? mc_rng_range(&rng, 30.0f, 46.0f) : kind == 1 ? mc_rng_range(&rng, 9.0f, 18.0f)
                                                                         : mc_rng_range(&rng, 14.0f, 28.0f);
        s3_trees[s3_ntree++] = (S3Tree){ mc_vec3(x, s3_ground(x, z), z), r, h, kind, 0.0f };
    }
    /* the understory: palms and saplings 2.5-11 m tall. They grow thick round
     * the viewer's small clearing (4.5 m) but stay clear of the hero plant and
     * the sky window. */
    mc_v3 gap = s3_dir(S3_GAP_YAW, S3_GAP_PITCH);
    for (int tries = 0; tries < 6000 && s3_ntree < S3_NFOREST; ++tries) {
        float a = mc_rng_range(&rng, 0.0f, MC_TAU), dist = 4.5f + 44.0f * powf(mc_rng_f(&rng), 1.5f);
        float x = cosf(a) * dist, z = sinf(a) * dist, r = mc_rng_range(&rng, 0.06f, 0.16f);
        int kind = 1 + (mc_rng_f(&rng) > 0.4f);
        float h = kind == 1 ? mc_rng_range(&rng, 2.5f, 8.0f) : mc_rng_range(&rng, 4.0f, 11.0f);
        float pz = fminf(fmaxf(z, 0.0f), 3.4f);                   /* the viewer's path: z 0..3.4 */
        int ok = (x - S3_HERO_X) * (x - S3_HERO_X) + (z - S3_HERO_Z) * (z - S3_HERO_Z) > 2.5f
              && x * x + (z - pz) * (z - pz) > 20.0f
              && mc_v3_dot(mc_v3_normalize(mc_vec3(x, h - 1.5f, z)), gap) < S3_GAP_COS - 0.06f;
        for (int k = 0; k < s3_ntree && ok; ++k) {
            float dx = x - s3_trees[k].p.x, dz = z - s3_trees[k].p.z, gp = (r + s3_trees[k].r) * 3.0f + 0.8f;
            ok = dx * dx + dz * dz > gp * gp;
        }
        if (ok) s3_trees[s3_ntree++] = (S3Tree){ mc_vec3(x, s3_ground(x, z), z), r, h, kind, 0.0f };
    }
    /* The beach part. The forest stops at the edge in a cove 48 m wide, enough
     * for every shot out of it. Nothing may stand in the camera's glide, in
     * its view out from the vantage, or on the tyrannosaurus's track (giants'
     * planks reach about 4.5 radii). The view is wider on the left, where the
     * tyrannosaurus is first seen between two trunks. Trees in the way are
     * only marked gone, and the hero trees all stay, so the shots before the
     * glide don't change. */
    mc_v3 side = s3_beach_pt(0.0f, 1.0f);
    for (int i = 8; i < s3_ntree; ++i) {
        S3Tree *t = &s3_trees[i];
        /* How far the tree reaches at eye height (1-2 m). Giants' planks,
         * small palms' fronds and the lowest saplings' sprays reach out, and
         * otherwise it's only the trunk. A tree in the glide is moved sideways
         * out of it, up to 5 m, and dropped if it's still in the way. */
        float room = t->kind == 0 ? t->r * 2.5f + 1.0f : t->h < (t->kind == 1 ? 6.0f : 5.0f) ? 1.4f + 0.3f * t->h
                                                                                          : t->r + 0.5f;
        if (s3_along(t->p.x, t->p.z) > 3.0f && s3_roam_dist(t->p.x, t->p.z) < room) {
            float sg = s3_roam_dist(t->p.x + side.x * 0.1f, t->p.z + side.z * 0.1f) > s3_roam_dist(t->p.x, t->p.z) ? 0.25f : -0.25f;
            for (int k = 0; k < 20 && s3_roam_dist(t->p.x, t->p.z) < room; ++k) { t->p.x += side.x * sg; t->p.z += side.z * sg; }
            t->p.y = s3_ground(t->p.x, t->p.z);
        }
        float a = s3_along(t->p.x, t->p.z), s = s3_across(t->p.x, t->p.z), c = fabsf(s);
        if (a > 46.0f || (a > (t->kind ? 30.5f : 27.5f) && c < 24.0f)
            || (a > S3_VANTAGE + 1.0f && c < 0.6f + (a - S3_VANTAGE) * (s < 0.0f ? 0.9f : 0.42f))
            || s3_roam_dist(t->p.x, t->p.z) < room - 0.01f)
            t->kind = 3;
    }
    s3_nforest = s3_ntree;
    /* along, across (half metres), trunk radius (cm), height (m). Thin ones are palms. */
    static const signed char EDGE[][4] = {
        { 50, -11,  95, 40 }, { 49,  10,  85, 36 },   /* the pair framing the glimpse from the vantage */
        { 59, -54, 110, 44 }, { 57, -78, 100, 40 },   /* up-sun on the edge: their crowns shade the track */
        { 54, -28,  90, 38 }, { 54,  30,  95, 40 },   /* ... and one down-sun */
        { 63, -18,  21, 13 }, { 64,  16,  20, 12 },   /* palms on the edge, leaning out but clear of the */
                                                      /* tyrannosaurus's head as it turns, then on the sand */
        { 78, -32,  20, 14 }, { 76, -17,  18, 12 }, { 80, 14, 19, 15 }, { 77, 26, 20, 11 }, { 82, 42, 18, 16 },
        { 56, -19,  70, 34 },                         /* left of the pair: the tyrannosaurus is first seen */
    };                                                /* between this one and the pair's left one */
    for (int e = 0; e < MC_ARRAY_COUNT(EDGE) && s3_ntree < S3_MAXTREE; ++e) {
        mc_v3 p = s3_beach_pt(EDGE[e][0] * 0.5f, EDGE[e][1] * 0.5f);
        float r = EDGE[e][2] * 0.01f;
        p.y = s3_ground(p.x, p.z);
        for (int i = 8; i < s3_nforest; ++i) {   /* it replaces any forest tree that stood too close */
            float dx = p.x - s3_trees[i].p.x, dz = p.z - s3_trees[i].p.z, sp = (r + s3_trees[i].r) * 3.0f + 1.0f;
            if (dx * dx + dz * dz < sp * sp) s3_trees[i].kind = 3;
        }
        s3_trees[s3_ntree++] = (S3Tree){ p, r, EDGE[e][3], r < 0.5f, 0.0f };
    }
}

/* Frustum test for the packages' culling. Profile builds can switch it off to
 * check that culling never changes the picture. */
static int s3_visible(Demo *d, const S3Ctx *c, mc_v3 centre, float radius) {
    (void)d;
#ifdef MC_PROFILE
    if (d->disable_culling) return 1;
#endif
    return c->pass == S3_SHADOW || c->pass == S3_CHAR || mc_frustum_sphere(&c->fr, centre, radius);
}

/* The packages' hooks. Each file defines its own make, free and draw, except
 * the ladybug's, which makes nothing and only has draw. make returns 0 on
 * failure, and draw is called once per pass. The light package also owns the
 * frame's lighting, the shadow pass, the sky, the volumetrics and the grade. */
static void s3_draw_world(Demo *d, const S3Ctx *c);
static void s3_setup(Demo *d, float t, S3Ctx *c);                    /* camera + matrices at time t */
static void s3_bug_at(float j, mc_v3 *pos, mc_v3 *fwd, mc_v3 *up);   /* the ladybug's pose (s3_bug.c) */
static void s3_rex_at(float j, mc_v3 *pos, mc_v3 *fwd);              /* the tyrannosaurus (s3_rex.c) */
static float s3_rex_step(float j);                                   /* 1 at each footfall, decaying */
static float s3_blob_at(float j, mc_v3 *pos);                       /* the blob (s3_blob.c) */
static float s3_roam_dist(float x, float z);                         /* distance to the camera's glide path */

#include "s3_trees.c"
#include "s3_under.c"
#include "s3_bug.c"
#include "s3_birds.c"
#include "s3_rex.c"
#include "s3_blob.c"
#include "s3_beach.c"
#include "s3_light.c"

static void s3_draw_world(Demo *d, const S3Ctx *c) {
    bug_draw(d, c);      /* first: up close, the hero leaf hides most of the frame */
    under_draw(d, c);
    trees_draw(d, c);
    birds_draw(d, c);
    rex_draw(d, c);
    blob_draw(d, c);
    beach_draw(d, c);    /* last: the sea is see-through */
}

/* The camera. In the wide shots it looks slowly round from where we stand,
 * then hands over to the macro rig that follows the ladybug. The keys below
 * give yaw and pitch in degrees, the fov, and the eye's x, height over the
 * ground and z. A faint handheld drift is added on top, settling to almost
 * nothing up close. */
typedef struct { float j, yaw, pitch, fov, x, y, z; } S3Key;
static const S3Key S3_KEYS[] = {
    { -2.0f,  -12.0f,   3.0f, 55.0f, 0.15f, 1.62f, 3.40f },
    {  0.0f,  -12.0f,   3.0f, 55.0f, 0.15f, 1.62f, 3.20f },
    {  4.0f,  -22.0f,   5.0f, 55.0f, 0.10f, 1.60f, 2.50f },
    {  8.0f,  -48.0f,   7.0f, 55.0f, 0.05f, 1.58f, 1.80f },
    { 12.0f,  -88.0f,   9.0f, 52.0f, 0.00f, 1.56f, 1.10f },
    { 15.5f, -118.0f,  38.0f, 58.0f, 0.00f, 1.55f, 0.60f },
    { 18.5f, -135.0f,  57.0f, 62.0f, 0.00f, 1.55f, 0.30f },
    { 21.5f, -150.0f,  50.0f, 60.0f, 0.00f, 1.55f, 0.10f },
    { 24.5f, -205.0f, -10.0f, 50.0f, 0.15f, 1.45f, 0.00f },
    { 27.5f, -248.0f, -30.0f, 42.0f, 0.55f, 1.25f, 0.15f },
    { 30.0f, -250.0f, -32.0f, 40.0f, 0.65f, 1.20f, 0.18f },
};
#define S3_NKEY MC_ARRAY_COUNT(S3_KEYS)
/* After the take-off comes the glide out toward the beach. Here x, y and z
 * are the eye's distance toward the sea (s3_along), its height over the floor
 * and its distance across (s3_across). Yaw and pitch are absolute. */
static const S3Key S3_ROAM_KEYS[] = {
    { 44.5f,  -6.0f,  62.0f, 55.0f,  1.0f, 1.00f,  0.9f },   /* still looking up after the ladybug */
    { 46.5f,  40.0f,  12.0f, 55.0f,  4.5f, 1.60f,  0.2f },   /* turning out between the trunks */
    { 48.5f,  62.0f,   3.0f, 54.0f,  9.0f, 1.80f, -0.5f },
    { 50.3f,  60.0f,   3.5f, 54.0f, 12.5f, 1.55f,  0.2f },   /* S3_GAP: the glimpse of the beach */
    { 52.3f,  60.0f,   5.0f, 50.0f, 13.0f, 1.00f,  0.0f },   /* S3_REX: its first footfall, and we crouch */
    { 57.0f,  70.0f,   8.0f, 29.0f, 13.2f, 0.48f,  1.6f },   /* low in the ferns; the tightening lens holds its head */
    { 60.0f,  66.0f,   7.0f, 31.0f, 12.9f, 0.48f,  1.4f },   /* as we recoil and wait for it to turn away. */
    { 65.0f,  61.0f,   6.0f, 34.0f, 13.4f, 0.60f,  1.4f },   /* We hold still as it goes behind the right trunk */
    { 69.24f, 59.0f,   3.5f, 54.0f, 13.6f, 1.50f, -0.1f },   /* and out of frame, and the look drifts back to the gap. */
    { 71.74f, 58.5f,   2.0f, 54.0f, 18.0f, 1.60f, -0.3f },   /* It has gone: out to the last row of trees as the blob rises, */
    { 74.24f, 63.0f,  1.75f, 46.0f, 22.0f, 1.65f, -0.3f },   /* then on as it hops up the beach toward us, the lens */
    { 77.74f, 64.5f,  1.5f,  32.0f, 27.5f, 1.25f,  0.0f },   /* lengthening. Out of the trees onto the top of the beach, */
    { 81.24f, 63.5f,  2.0f,  20.0f, 30.5f, 1.00f,  0.5f },   /* ~15 m from it as it waves. The closer framing gives */
    { 85.24f, 62.0f,  1.5f,  18.0f, 31.5f, 1.00f,  1.0f },   /* its face and the caught props room to read. The eye */
    { 89.24f, 56.5f,  1.75f, 18.0f, 32.0f, 1.00f,  2.5f },   /* stays 1 m up, looking over it toward the surf. As it */
    { 92.74f, 49.5f,  1.5f,  17.0f, 32.5f, 1.00f,  4.5f },   /* plays we move sideways along the top of the beach, */
    { 97.24f, 45.5f,  1.5f,  18.0f, 32.5f, 1.00f,  5.5f },   /* so the swash turns and the sun comes round beyond it. */
    { 100.24f, 44.0f,  -4.5f, 23.0f, 32.5f, 1.75f,  5.5f },   /* Then back among the trees, rising, with the lens widening */
    { 104.74f, 43.5f,  -3.5f, 54.0f, 26.5f, 3.25f,  7.5f },   /* again for the last wide view. */
};
#define S3_NROAM MC_ARRAY_COUNT(S3_ROAM_KEYS)

/* Distance from (x, z) to the glide's path. Trees and plants keep out of it. */
static float s3_roam_dist(float x, float z) {
    float a = s3_along(x, z), c = s3_across(x, z), best = 1e9f;
    for (int i = 0; i + 1 < S3_NROAM; ++i) {
        const S3Key *p = &S3_ROAM_KEYS[i], *q = &S3_ROAM_KEYS[i + 1];
        float dx = q->x - p->x, dz = q->z - p->z;
        float u = mc_saturatef(((a - p->x) * dx + (c - p->z) * dz) / (dx * dx + dz * dz + 1e-6f));
        float ex = a - p->x - u * dx, ez = c - p->z - u * dz;
        best = fminf(best, ex * ex + ez * ez);
    }
    return sqrtf(best);
}

static void s3_key_sample(const S3Key *keys, int n, float j, mc_v3 *ang, mc_v3 *pos) {
    int i = 0;
    while (i < n - 2 && j > keys[i + 1].j) ++i;
    float u = mc_saturatef((j - keys[i].j) / (keys[i + 1].j - keys[i].j));
    const S3Key *k[4] = { &keys[i > 0 ? i - 1 : 0], &keys[i], &keys[i + 1], &keys[i + 2 < n ? i + 2 : n - 1] };
    mc_v3 a[4], p[4];
    for (int q = 0; q < 4; ++q) {
        a[q] = mc_vec3(k[q]->yaw, k[q]->pitch, k[q]->fov);
        p[q] = mc_vec3(k[q]->x, k[q]->y, k[q]->z);
    }
    *ang = mc_catmull_rom(a[0], a[1], a[2], a[3], u);
    *pos = mc_catmull_rom(p[0], p[1], p[2], p[3], u);
}

static void s3_camera(float j, S3Ctx *c) {
    mc_v3 ang, off;
    s3_key_sample(S3_KEYS, S3_NKEY, j, &ang, &off);
    float hand = 1.0f - 0.9f * c->macro;
    ang.x += hand * (0.35f * sinf(j * 0.61f) + 0.20f * sinf(j * 1.37f + 1.0f));
    ang.y += hand * (0.30f * sinf(j * 0.53f + 2.0f) + 0.15f * sinf(j * 1.71f));
    mc_v3 eye = mc_vec3(off.x, s3_ground(off.x, off.z) + off.y + 0.006f * hand * sinf(j * 1.9f), off.z);
    mc_v3 aim = mc_v3_add(eye, s3_dir(ang.x, ang.y));
    float fov = ang.z;

    /* The macro rig circles in on the ladybug and slowly orbits it. When it
     * takes off, the rig stays where it was and tilts up to watch it go. */
    mc_v3 bp, bf, bu, anchor;
    s3_bug_at(j, &bp, &bf, &bu);
    s3_bug_at(fminf(j, S3_FLY), &anchor, &bf, &bu);
    float m = mc_smoothstepf(S3_FIND + 2.0f, S3_MACRO, j);
    if (m > 0.0f) {
        float jj = fminf(j, S3_FLY);
        float dist = mc_lerpf(0.34f, 0.045f, mc_smoothstepf(S3_FIND + 3.0f, S3_MACRO + 1.5f, jj))
                   + 0.006f * sinf(jj * 0.4f) + 0.05f * mc_smoothstepf(S3_FLY, (float)(40 * BAR_S), j);
        float az = mc_radians(-250.0f + 180.0f + 14.0f * (jj - S3_MACRO) / (S3_FLY - S3_MACRO));
        float el = mc_radians(mc_lerpf(34.0f, 20.0f, mc_smoothstepf(S3_MACRO, S3_FLY, jj)));
        mc_v3 rig = mc_v3_add(anchor, mc_vec3(sinf(az) * cosf(el) * dist, sinf(el) * dist, -cosf(az) * cosf(el) * dist));
        eye = mc_v3_lerp(eye, rig, m);
        aim = mc_v3_lerp(aim, bp, m);
        fov = mc_lerpf(fov, mc_lerpf(38.0f, 55.0f, mc_smoothstepf(S3_FLY + 0.5f, S3_ROAM + 1.0f, j)), m);
    }
    float focus = m > 0.0f ? mc_lerpf(6.0f, mc_v3_dist(bp, eye), m) : 6.0f;

    /* The glide out to the beach takes over from the macro rig and holds back
     * at the vantage while the tyrannosaurus passes. At its first footfall we
     * crouch into the ferns (eye below 0.5 m) and look up, so it stands against
     * the sky. The look follows it half-way, and the lens lengthens
     * (54 -> 29 deg) as it comes into the gap and the sun to look at us. Each
     * footfall jolts the eye down and dips the look (about 4 cm at 21 m). Its
     * first open-jaw breath makes us flinch up and back from its lowered
     * head. We hold still while it walks on behind the
     * right-hand trunk and out of frame. Only once it has gone do we stand and
     * step out between the trunks to the last row of trees, as the blob rises.
     * Then we go on out of the trees as it hops up the beach. The lens
     * lengthens (54 -> 17-20 deg) so its arms, its wave, its hops and the
     * caught props read clearly. Through the wave and the play the eye drops to 1 m and the
     * keys aim 1.5-2 deg over it. That puts it in the lower third against the
     * surf, with sky above and less of the dark, tree-shaded sand below (at
     * 0.7 m the rise of the sand in front hid its feet). The look also turns
     * half-way toward it. At the end we go back among the trees, wide again. */
    float r = mc_smoothstepf(S3_ROAM, S3_ROAM + 2.5f, j);
    if (r > 0.0f) {
        mc_v3 ra, rp, rx, rf, bx;
        s3_key_sample(S3_ROAM_KEYS, S3_NROAM, j, &ra, &rp);
        s3_rex_at(j, &rx, &rf);
        s3_blob_at(j, &bx);
        mc_v3 e2 = s3_beach_pt(rp.x, rp.z);
        e2.y = s3_ground(e2.x, e2.z) + rp.y;
        float wr = 0.5f * mc_smoothstepf(S3_REX + 1.0f, S3_REX + 3.0f, j)
                         * (1.0f - mc_smoothstepf(S3_SHORE, S3_SHORE + 1.0f, j));
        float wb = 0.5f * mc_smoothstepf(S3_BLOB, S3_BLOB + 2.5f, j);
        mc_v3 head = mc_v3_add(mc_v3_mad(rx, rf, 3.0f), mc_vec3(0.0f, 4.1f, 0.0f));
        mc_v3 d2 = mc_v3_normalize(mc_v3_lerp(s3_dir(ra.x, ra.y), mc_v3_dir(head, e2), wr));
        d2 = mc_v3_normalize(mc_v3_lerp(d2, mc_v3_dir(bx, e2), wb));
        float near = 1.0f / (1.0f + mc_v3_len2(mc_v3_sub(rx, e2)) / 225.0f);   /* footfalls shake us when it's close */
        float sh = 0.16f * near * s3_rex_step(j), sn = (j - 57.93f) / 0.24f;
        sn = expf(-sn * sn);
        sh -= 0.12f * sn;   /* its first breath (jr.vert and music): a brief upward and backward recoil */
        e2 = mc_v3_sub(e2, mc_v3_scale(s3_beach_pt(1.0f, 0.0f), 0.35f * sn));
        e2.y -= sh;
        d2.y -= sh * 0.15f;
        eye   = mc_v3_lerp(eye, e2, r);
        aim   = mc_v3_lerp(aim, mc_v3_add(e2, d2), r);
        fov   = mc_lerpf(fov, ra.z, r);
        focus = mc_lerpf(focus, mc_lerpf(mc_lerpf(10.0f, mc_v3_dist(head, e2), wr / 0.5f),
                                         mc_v3_dist(bx, e2), wb / 0.5f), r);
    }
    c->cam.pos    = eye;
    c->cam.target = aim;
    c->cam.up     = mc_vec3(0.0f, 1.0f, 0.0f);
    c->cam.fovy   = mc_radians(fov);
    c->cam.znear  = mc_lerpf(0.05f, 0.004f, c->macro);
    c->cam.zfar   = mc_lerpf(mc_lerpf(400.0f, 150.0f, c->macro), 2500.0f, r);
    c->fwd   = mc_v3_dir(aim, eye);
    c->focus = focus;
}

static void s3_setup(Demo *d, float t, S3Ctx *c) {
    memset(c, 0, sizeof *c);
    c->t = t;
    c->j = t - S3_J0;
    c->macro = mc_smoothstepf(S3_FIND + 2.0f, S3_MACRO, c->j) * (1.0f - mc_smoothstepf(S3_ROAM, S3_ROAM + 2.5f, c->j));
    c->beach = mc_smoothstepf(S3_ROAM + 2.0f, S3_GAP, c->j);
    s3_camera(c->j, c);
    c->vp   = camera_vp(d, &c->cam);
    c->fr   = mc_frustum_make(c->vp);
    c->pass = S3_LIT;
}

/* How near each tree is ever seen: the closest the eye comes to its foot
 * while it is in view, anywhere in the scene. The camera's path is fixed, so
 * this is known at load. A tree's detail is chosen from it once, so nothing
 * on the tree appears or vanishes as the eye moves. That covers jt.vert's
 * lianas, epiphytes, planks, limbs, cards and fronds, and s3_trees.c's finer
 * trunks and modelled crowns. */
static void s3_tree_lod(Demo *d) {
    S3Ctx c;
    for (int i = 0; i < s3_ntree; ++i) s3_trees[i].lod = 1e9f;
    for (float j = -0.5f; j < S3_END; j += 0.1f) {
        s3_setup(d, j + S3_J0, &c);
        for (int i = 0; i < s3_ntree; ++i) {
            S3Tree *t = &s3_trees[i];
            if (s3_visible(d, &c, mc_v3_add(t->p, mc_vec3(0, t->h * 0.6f, 0)), t->h * 0.7f + 5.0f))
                t->lod = fminf(t->lod, mc_v3_dist(t->p, c.cam.pos));
        }
    }
}

static int s3_make(Demo *d) {
    s3_tree_sites();
    s3_tree_lod(d);
    return trees_make(d) && under_make(d) && light_make(d) && birds_make(d)
        && rex_make(d) && blob_make(d) && beach_make(d);
}

static void s3_free(Demo *d) {
    trees_free(d); under_free(d); light_free(d); birds_free(d);
    rex_free(d); blob_free(d); beach_free(d);
#define MC_PROGRAM(field, vs, fs) mc_shader_free(&d->field);
#include "scene3.def"
#undef MC_PROGRAM
}

/* How the forest's parts are stretched (stretch() in main.c). Each pair is
 * bars of the show for bars of the story: the look round, the canopy, the way
 * down, the ladybug, its take-off and the glide. From the beach's hush on,
 * the story runs at its own pace. */
static const unsigned char S3_SEG[] = { 8, 7, 5, 4, 3, 3, 8, 7, 2, 2, 4, 3, 1, 1, 0 };

static void render_scene3(Demo *d, mc_window *w, double t) {
    int ow, oh;
    mc_window_size(w, &ow, &oh);
    t = S3_J0 + BAR_S * stretch((float)((t - S3_J0) / BAR_S), S3_SEG);   /* the show's seconds -> the story's */
    S3Ctx c;
    s3_setup(d, (float)t, &c);
    mc_post post = mc_post_defaults();
    post.time = (float)t;
    /* The picture comes up out of the white through an over-exposed glare,
     * like eyes adjusting to daylight. The white lifts first while the glare
     * and its bloom linger, so the trunks first show as silhouettes against
     * bright light. The exposure swells again as the ladybug flies up into
     * the light, and the scene fades out at the end. */
    float reveal = mc_smoothstepf(0.0f, S3_REVEAL, c.j);
    if (c.j < -0.5f) {                           /* pure white, nothing to draw yet (the last */
                                                 /* half second draws the jungle under it to warm up) */
        mc_renderer_begin(d->R, mc_vec3(1.0f, 1.0f, 1.0f));
        post.white = 1.0f;
        mc_renderer_end(d->R, mc_window_output_fbo(w), ow, oh, post);
        return;
    }
    light_frame(d, &c);                          /* lighting and the sun's matrix into the Frame block */
    mc_renderer_begin(d->R, d->frame.fog_color);
    light_shadow(d, &c);                         /* the sun's depth pass (casters: s3_draw_world) */
    mc_gl_frame_block(&d->frame, sizeof d->frame);
    c.pass = S3_LIT;
    mc_gl_switch(MC_ZTEST_ON | MC_ZWRITE_ON | MC_CULL_ON | MC_BLEND_OFF);
    glDepthFunc(GL_LESS);
    glCullFace(GL_BACK);
    s3_draw_world(d, &c);
    light_sky(d, &c);                            /* behind everything, at the far plane */
    c.pass = S3_LATE;                            /* then what must see the picture behind it: */
    blob_draw(d, &c);                            /* the blob's jelly, */
    beach_draw(d, &c);                           /* the reflections in the sea and the wet sand */
    light_volume(d, &c);                         /* shafts through the haze */
    light_post(d, &c, &post);                    /* the grade and depth of field */
    float glare = (1.0f - reveal) * (1.0f - reveal);
    float swell = mc_smoothstepf(S3_FLY + 1.0f, S3_ROAM, c.j) * (1.0f - mc_smoothstepf(S3_ROAM + 1.0f, S3_GAP, c.j));
    post.exposure += 5.0f * glare + 0.7f * swell;
    post.bloom    += 1.2f * glare + 0.3f * swell;
    post.white     = glare * glare * (1.0f - reveal);
    post.fade      = 1.0f - mc_smoothstepf(S3_OUT, S3_END, c.j);
    /* On the beach the lens is long, so the depth of field is shallow. With a
     * 1.5 cm aperture the blur at infinity is the aperture over the focus
     * distance, in pixels: about 5 px at 1080 lines while the blob waves,
     * which softens the surf behind it, and about 1.5 px in the wide view at
     * the end. */
    post.dof      += 0.015f * d->frame.res.y / (c.focus * post.projy) * mc_smoothstepf(S3_BLOB, S3_BLOB + 4.0f, c.j);
    mc_renderer_end(d->R, mc_window_output_fbo(w), ow, oh, post);
}
