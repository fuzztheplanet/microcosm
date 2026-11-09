/*
 * s3_trees.c - scene 3's giant trees, their crowns, palms and lianas.
 * (#included by scene3.c.)
 *
 * There are no meshes. jt.vert grows every part from vertex and instance
 * IDs: lobed trunks, buttress planks, limbs, lianas and strangler roots, leaf
 * cards on the crowns, palm fronds, bromeliads and ferns perched on the
 * giants, and curtains of hanging leaves. Every gap at eye level ends in the
 * far forest, which is billboards of the same trees. This file only picks
 * the trees in view (nearest first, grouped by kind), hands them over as a
 * uniform array and issues one instanced draw per part and kind.
 */

/* The parts, in draw order (jt.vert's part numbers). Each has a grid of
 * cols x rows quads per piece, a number of pieces per tree, and a bit mask of
 * the kinds that grow it. 16 in the mask means the part also casts sun
 * shadows. Those go into the cached sun maps, which are drawn once, so a
 * caster costs nothing per frame. The planks shade each other, the floor and
 * the trunk they grow from. Leaves (cards, fronds, epiphytes, the near
 * crowns' modelled leaves) are two-sided. Slender trunks and palms get 8
 * sides, giants 14. */
static const struct { unsigned char cols, rows, per, kinds; } JT_PART[] = {
    { 14, 18,   1, 7 | 16 },   /* trunks */
    {  8, 10,   7, 1 | 16 },   /* buttress planks: giants (a vertex on each shoulder and the crest) */
    {  5,  5,  10, 5 | 16 },   /* limbs: giants, slender */
    {  4, 16,   6, 1 },        /* lianas and strangler roots: giants */
    {  1,  1,  48, 5 | 16 },   /* crown leaf cards */
    {  4, 10,  14, 2 | 16 },   /* palm fronds */
    {  2,  4,  48, 1 },        /* epiphytes: 6 rosettes of 8 leaves, giants */
    {  1,  4,   8, 1 | 16 },   /* curtains of hanging leaves, giants */
    {  2,  4, 240, 4 },        /* the nearest crowns' modelled leaves: 10 for each of 24 cards */
};
static float jt_tree[2 * S3_MAXTREE][4];
static GLuint    jt_tex[3];   /* the baked bark, leaves and sand (jt_bake.frag), on texture units 10-12 */
static mc_target jt_far;      /* the far forest's impostor atlas, colour and depth on units 13 and 14 */
static int       jt_baking;   /* set while drawing the impostors' trees (no far forest then) */
#define JT_FAR   240          /* the far forest's billboards (jt.vert part 9, plus 40 for its wall) */
#define JT_FAR_W 512          /* one baked view of a tree: 21 px/m for the giants (at 10.7 the */
                              /* holes in the leaves were stair-stepped blocks in the haze) */
#define JT_FAR_H 768

static void light_frame(Demo *d, S3Ctx *c);
static void trees_draw(Demo *d, const S3Ctx *c);

/* Bake the jungle's materials once and leave each bound on its own unit for
 * good (the shaders name their units with layout(binding)). Then bake the
 * far forest's impostors. The first four hero trees (two giants, a slender
 * tree and a palm) are moved far out into the forest, outside the sun's maps
 * and under the canopy. Their own shaders draw each one whole, from eight
 * sides, into an atlas that holds colour, lit by scene 3's sun and sky, and
 * depth, which the cut-outs follow. The sun and sky never move, so every
 * side stays correctly lit. The haze is added live. */
static int trees_make(Demo *d) {
    for (int k = 0; k < 3; ++k) {
        glActiveTexture(GL_TEXTURE10 + k);
        if (!(jt_tex[k] = bake_tex(d->jt_bake, k))) return 0;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, 4);   /* sharp at grazing angles */
    }
    jt_far = mc_target_make(8 * JT_FAR_W, 4 * JT_FAR_H, 1, 1);
    glActiveTexture(GL_TEXTURE13); glBindTexture(GL_TEXTURE_2D, jt_far.color);
    glActiveTexture(GL_TEXTURE14); glBindTexture(GL_TEXTURE_2D, jt_far.depth);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);   /* filtered: the cut-out's */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);   /* coverage (jt.frag) */
    glActiveTexture(GL_TEXTURE0);
    mc_target_bind(&jt_far);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    mc_gl_switch(MC_ZTEST_ON | MC_ZWRITE_ON | MC_CULL_ON | MC_BLEND_OFF); glDepthFunc(GL_LESS);
    mc_v3 keep[4];
    FrameUniforms frame = d->frame;
    int n = s3_ntree;
    for (int v = 0; v < 4; ++v) { keep[v] = s3_trees[v].p; s3_trees[v].p = mc_vec3(-300.0f - 400.0f * v, 0.0f, 300.0f); }
    s3_ntree = 4; jt_baking = 1;
    S3Ctx c;
    s3_setup(d, S3_J0, &c);
    light_frame(d, &c);
    d->frame.fog = d->frame.wobble = 0.0f;
    d->frame.q = -1.0f;                            /* jt.vert: every limb and leaf, as if seen close up */
    for (int v = 0; v < 32; ++v) {                 /* a box fitting each tree: 24 x 44 x 26 m for the giants */
        float k = v < 16 ? 1.0f : v < 24 ? 0.8f : 0.45f;
        mc_v3 at = mc_v3_add(s3_trees[v / 8].p, mc_vec3(0.0f, 21.0f * k, 0.0f));
        c.cam.pos = d->frame.cam = mc_v3_add(at, mc_vec3(60.0f * cosf((float)v * MC_TAU / 8.0f), 0.0f, 60.0f * sinf((float)v * MC_TAU / 8.0f)));
        c.fr = mc_frustum_make(d->frame.view_proj = mc_m4_mul(mc_m4_ortho(-12.0f * k, 12.0f * k, -22.0f * k, 22.0f * k, 60.0f - 13.0f * k, 60.0f + 13.0f * k),
                                                               mc_m4_look_at(c.cam.pos, at, mc_vec3(0.0f, 1.0f, 0.0f))));
        mc_gl_frame_block(&d->frame, sizeof d->frame);
        glViewport(v % 8 * JT_FAR_W, v / 8 * JT_FAR_H, JT_FAR_W, JT_FAR_H);
        trees_draw(d, &c);
    }
    for (int v = 0; v < 4; ++v) s3_trees[v].p = keep[v];
    s3_ntree = n; jt_baking = 0;
    d->frame = frame;
    mc_target_bind(NULL);
    return mc_target_ok(&jt_far);
}
static void trees_free(Demo *d) { (void)d; glDeleteTextures(3, jt_tex); mc_target_free(&jt_far); }

/* jt.vert's gust(), the wind field, at a tree's foot 0.6 s ago. The tree
 * bends with it as a whole, so it's worked out once per tree rather than per
 * vertex. */
static float jt_gust(const Demo *d, mc_v3 p) {
    float u = p.x * S3_SEA_X + p.z * S3_SEA_Z, c = 0.5f * p.x + 0.866f * p.z, t = d->frame.time - 0.6f;
    return (0.35f + 0.65f * d->frame.wobble) * mc_smoothstepf(-0.4f, 1.0f, sinf(0.2f * u + 0.8f * t + 1.5f * sinf(0.06f * c + 0.2f * t)))
         * (0.55f + 0.45f * sinf(0.13f * c + 0.05f * u + 0.5f * t));
}

static void trees_draw(Demo *d, const S3Ctx *c) {
    int first[4], n = 0;   /* each kind's first tree in jt_tree */
    for (int k = 0; k < 3; ++k) {
        first[k] = n;
        for (int i = 0; i < s3_ntree; ++i) {
            const S3Tree *t = &s3_trees[i];
            if (t->kind != k || (c->pass == S3_SHADOW ? t->p.x * t->p.x + t->p.z * t->p.z > 3600.0f
                                 : !s3_visible(d, c, mc_v3_add(t->p, mc_vec3(0, t->h * 0.6f, 0)), t->h * 0.7f + 5.0f)))
                continue;   /* the sun's map reaches ~60 m out */
            /* Nearest first, so near crowns hide far ones before those get
             * shaded. The key is how near the tree is ever seen, which also
             * sets its detail (s3_tree_lod), or in the impostors' bake how
             * near it is now. */
            float dist = jt_baking ? mc_v3_dist(t->p, c->cam.pos) : t->lod, *q;
            int at = n++;
            for (; at > first[k] && jt_tree[2 * at - 1][3] > dist; --at)
                memcpy(jt_tree[2 * at], jt_tree[2 * at - 2], sizeof(float[8]));
            q = jt_tree[2 * at];
            q[0] = t->p.x; q[1] = t->p.y; q[2] = t->p.z; q[3] = t->r;
            q[4] = t->h;   q[5] = (float)k;  q[6] = (float)i + 0.9f * jt_gust(d, t->p); q[7] = dist;   /* seed (whole part) and gust (fraction); its LOD */
        }
    }
    first[3] = n;
    mc_shader_use(d->jt);
    glUniform4fv(2, 2 * n, jt_tree[0]);                         /* uP 0, uGap 1, uT 2.. */
    mc_v3 gap = s3_dir(S3_GAP_YAW, S3_GAP_PITCH);
    glUniform4f(1, gap.x, gap.y, gap.z, S3_GAP_COS);
    glBindVertexArray(mc_gl_fullscreen_vao());
    for (int p = 0; p < MC_ARRAY_COUNT(JT_PART); ++p) {
        if (p == 4) glDisable(GL_CULL_FACE);
        for (int k = 0; k < 3; ++k) {
            /* Trunks and planks ever seen within 12 m get twice the sides and
             * more than twice the rings, so they show no facets close up.
             * Slender crowns ever seen within 10 m get real leaves instead of
             * cards. */
            int at = first[k], near = at, end = first[k + 1], crown = (p == 4 || p == 8) && k == 2;
            while ((p < 2 || crown) && c->pass == S3_LIT && near < end && jt_tree[2 * near + 1][3] < (crown ? 10.0f : 12.0f)) ++near;
            if (p == 4) at = near;
            if (p == 8) end = near;
            for (int fine = p < 2 && near > at; at < end; fine = 0) {
                int count = (fine ? near : end) - at, cols = (p || !k ? JT_PART[p].cols : 8) << fine,
                    rows = JT_PART[p].rows * (fine + 1) + 4 * fine;
                if (!(JT_PART[p].kinds >> k & 1) || (c->pass == S3_SHADOW && JT_PART[p].kinds < 16)) break;
                glUniform4f(0, (float)p, (float)at, (float)JT_PART[p].per, (float)(cols + 128 * rows));
                glDrawArraysInstanced(GL_TRIANGLES, 0, cols * rows * 6, count * JT_PART[p].per);
                at += count;
            }
        }
    }
    if (c->pass == S3_LIT && !jt_baking) {                    /* the far forest: billboards and a wall of haze */
        glUniform4f(0, 9.0f, 0.0f, 1.0f, 129.0f);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, JT_FAR + 40);
    }
    glEnable(GL_CULL_FACE);
}
