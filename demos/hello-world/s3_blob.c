/*
 * s3_blob.c - scene 3's blob on the beach. It's the creature from scenes 1
 * and 2, now with two tiny arms. From its jump into the swash onwards it
 * carries a straw hat, a beach parasol and a folded deckchair. (#included by
 * scene3.c.) Everything except its path is generated and timed in
 * jo_blob.vert, in one instanced draw: its shape, its hops, arms and eyes,
 * and what it carries. It also draws itself into the characters' sun map
 * (S3_CHAR), so it casts a real shadow and shades itself, and what it
 * carries shades it too.
 *
 * It's drawn late (S3_LATE), after the sea and the sky, so its jelly can look
 * through a copy of the finished picture behind it. Where it's still under
 * the surf, jo_blob.frag uses the depth to show the sea from that copy
 * instead. What it carries is opaque and drawn with the world (S3_LIT), so
 * it's in that copy.
 */
#define S3_BLOB_R 0.48f                         /* its radius (jo_blob.vert's R), so it's a metre tall */
#define JO_HOP0   ((34.0f + S3_HOLD) * (float)BAR_S)   /* bar 38: the first of nine hops, one a beat */
#define JO_BEAT   ((float)BAR_S * 0.25f)
#define JO_PROPS  (192.5f * JO_BEAT)            /* it lands from its jump into the swash and carries the props from here on */
#define JO_VERTS  (72 * 48 * 6)                 /* a 72 x 48 grid per part: half the jelly, an eye */

/* Its hops, run by run, on the beat. A run gives:
 *   beat    the jungle beat it starts on (beats since S3_J0, so bar 38 is 152)
 *   n, len  how many hops, and how many beats each takes
 *   step    how far each hop carries it seaward (1/60 m, < 0 is up the beach)
 *   lift    how high each hop goes (cm)
 *   turn    how far each hop turns it (1/50 rad, to its right)
 *   splash  how hard each landing splashes, if it lands in the water
 * The music's plucks and splashes (synth.c) land with them. */
typedef struct { unsigned char beat, n, len; signed char step, lift, turn, splash; } JoRun;
static const JoRun JO_RUNS[] = {
    { 152, 9, 1, -80, 20,   0,   0 },   /* ashore: nine hops up the beach */
    { 176, 2, 1,   0, 12,  79,   0 },   /* round to the sea as a bigger wave runs up toward it */
    { 180, 2, 1,  52, 20,   0,   0 },   /* down after it as it drains, */
    { 182, 1, 1,  52, 20,   0,  30 },   /* into the last of it */
    { 186, 1, 1,   0,  8,   0,   0 },   /* bouncing as the next one comes */
    { 188, 1, 1,   0,  8,   0,   0 },
    { 191, 1, 2,  18, 35,   0, 100 },   /* a jump over its foaming edge, into the water */
    { 194, 2, 1,   0, 10,   0,  60 },   /* stamping in it */
    { 197, 2, 1,   0, 12, -79,  35 },   /* round again as it drains */
    { 200, 3, 1, -50, 20,   0,   0 },   /* up the beach, */
    { 205, 1, 1,  15, 10, 120,   0 },   /* half round to watch the sea, a step down toward it */
    { 208, 1, 1,   0,  8,   0,  30 },   /* a bounce as the last wave laps round it */
};
#define JO_NRUNS MC_ARRAY_COUNT(JO_RUNS)

/* Where its centre is at jungle time j, and how far it has turned from facing
 * the land (the camera), in radians to its right. At S3_BLOB it's just under
 * the surface about 13 m out. It swims in and rises out of the sea, hops up
 * the beach and stops on the wet sand about 4 m above the waterline, then
 * waves, plays in the swash and comes back up (JO_RUNS). The shader does the
 * hops themselves, around this steady line. */
static float s3_blob_at(float j, mc_v3 *pos) {
    float a = 63.0f - 5.0f * mc_smoothstepf(S3_BLOB, JO_HOP0, j), y = 0.0f;
    for (const JoRun *r = JO_RUNS; r < JO_RUNS + JO_NRUNS; ++r) {
        float k = mc_clampf((j / JO_BEAT - r->beat) / r->len, 0.0f, r->n), h = floorf(k);
        a += k * r->step * (1.0f / 60.0f);
        y += (h + (k < r->n ? mc_smoothstepf(0.22f, 0.75f, k - h) : 0.0f)) * r->turn * 0.02f;
    }
    mc_v3 p = s3_beach_pt(a, S3_BLOB_ACROSS);
    p.y = mc_lerpf(S3_SEA_Y - 1.1f * S3_BLOB_R, s3_ground(p.x, p.z) + S3_BLOB_R,
                   mc_smoothstepf(S3_BLOB + 0.3f, S3_BLOB + 2.3f, j));
    *pos = p;
    return y;
}

/* The two latest splashes by jungle time j, newest first: where (x, z), how
 * long ago and how hard. They drive the spray (jo_blob) and the rings left on
 * the water (jw_sea). Only landings in the water splash, so nothing flies off
 * it when it shivers on the dry sand after the play (jo_blob.vert). A ring of
 * spray there, 3 m up from the water, looked like water out of nowhere. */
static void jo_splashes(float j, mc_v4 s[2]) {
    mc_v3 p;
    s[0] = s[1] = mc_vec4(0.0f, 0.0f, 9.0f, 0.0f);
    for (const JoRun *r = JO_RUNS; r < JO_RUNS + JO_NRUNS; ++r)
        for (int i = 0; i < r->n && r->splash; ++i) {
            float t = (r->beat + (i + 0.75f) * r->len) * JO_BEAT;   /* it lands */
            if (t > j) break;
            s3_blob_at(t + 0.25f * r->len * JO_BEAT, &p);        /* where that hop ends */
            s[1] = s[0];
            s[0] = mc_vec4(p.x, p.z, j - t, r->splash * 0.01f);
        }
}

static int blob_make(Demo *d) { (void)d; return 1; }
static void blob_free(Demo *d) { (void)d; }

static void blob_draw(Demo *d, const S3Ctx *c) {
    mc_v3 p;
    mc_v4 sp[2], h = mc_vec4(-1.0f, 0.0f, 0.0f, s3_blob_at(c->j, &p));
    if (c->pass == S3_SHADOW || (c->pass == S3_LIT && c->j < JO_PROPS) || c->j < S3_BLOB - 0.5f || !s3_visible(d, c, p, 2.5f)) return;
    /* The run it's in: how far into it (in hops), and how high and how far
     * out each hop goes. Also its turn, and how long since a run last ended,
     * for the wobble that follows. */
    float b = c->j / JO_BEAT, st = 9.0f;
    for (const JoRun *r = JO_RUNS; r < JO_RUNS + JO_NRUNS; ++r) {
        float k = (b - r->beat) / r->len;
        if (k >= 0.0f && k < r->n) h = mc_vec4(k, r->lift * 0.01f, r->step * (1.0f / 60.0f), h.w);
        if (k >= r->n) st = (k - r->n) * r->len * JO_BEAT;
    }
    jo_splashes(c->j, sp);
    int n = sp[0].z < 1.6f ? 8 : 6;                                  /* the spray, while it flies */
    if (c->pass == S3_LATE) jelly_copy(d, p, n > 6 ? 2.5f : 1.0f);  /* the sea and the sky behind it */
    mc_shader_use(d->jo_blob);
    mc_set_v4(0, mc_vec4(p.x, p.y, p.z, st));                       /* uB, uH, uS[2]: locations 0-3 */
    mc_set_v4(1, h);
    glUniform4fv(2, 2, &sp[0].x);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(mc_gl_fullscreen_vao());
    /* What it carries, its two halves, its eyes and the spray. S3_LIT draws
     * only what it carries. S3_LATE draws the lot, and the depth test passes
     * none of what it carries this second time. S3_CHAR draws its shadow from
     * a coarser grid. */
    glDrawArraysInstanced(GL_TRIANGLES, 0, c->pass == S3_CHAR ? JO_VERTS / 4 : JO_VERTS, c->pass == S3_LATE ? n : c->pass == S3_LIT ? 2 : 6);
    glEnable(GL_CULL_FACE);
}
