/*
 * s3_bug.c - scene 3's hero plant, its broad leaf and the ladybug on it.
 * (#included by scene3.c.)
 *
 * The plant is a clump of big, glossy elephant-ear leaves on long stalks,
 * grown entirely in jb_leaf.vert from gl_VertexID / gl_InstanceID. The
 * ladybug walks on the hero leaf (instance 0), so its shape is mirrored here
 * (jb_leaf_at). The ladybug is about 8 mm long. jb_bug.vert builds it in
 * millimetres from one small sphere that it also generates (no mesh). Each
 * part is that sphere squashed or folded: wing cases, shield, head, eyes,
 * legs, antennae, and a few dew drops on the leaf. Its hind wings are drawn
 * last as blurred, translucent membranes.
 */
#define JB_LEN    0.40f     /* the hero leaf's length (m) ... */
#define JB_YAW    (-0.26f)  /* ... and heading (down -z, a little toward the viewer) */
#define JB_RISE   0.8f      /* its root is this high over the ground (as LEAF[0] in jb_leaf.vert) */
#define JB_PARTS  32        /* opaque parts (jb_bug.vert) ... */
#define JB_WINGS  16        /* ... then the wings, 8 blurred copies each */
#define JB_VERTS  (40 * 16 * 6)   /* one leaf or stalk: a 40 x 16 quad grid */
#define JB_BALL   (6 * 12 * 12 * 6) /* the unit sphere every ladybug part is made from (jb_bug.vert) */


/* A point on the hero leaf: u along the midrib (0 stalk .. 1 tip), v across
 * (-1..1 edge to edge). The blade is arched, droops to the tip, is folded a
 * little along the midrib and wavy at the margin. This mirrors shape() and
 * frame() in the jb_ shaders, and they must stay in step. */
static mc_v3 jb_leaf_at(float u, float v) {
    float w = 0.36f * powf(sinf(MC_PI * powf(u, 0.6f)), 0.85f), x = 0.06f + JB_LEN * u, y = JB_LEN * v * w;
    float z = JB_LEN * (0.2f * u - 0.3f * u * u + (0.1f * fabsf(v) - 0.05f * v * v) * w
                        + 0.007f * sinf(u * 17.0f + v * 2.0f + 6.0f) * v * v);
    float sy = sinf(JB_YAW), cy = cosf(JB_YAW);
    return mc_vec3(S3_HERO_X + sy * x - cy * y, s3_ground(S3_HERO_X, S3_HERO_Z) + JB_RISE + z, S3_HERO_Z - cy * x - sy * y);
}

/* How far along its walk the ladybug is (0..1). It sets off as we come in,
 * stops midway for a look round, and arrives as it gets ready to fly. The
 * walk is 78 mm long and weaves a little beside the midrib (u 0.3 .. 0.485).
 * It goes at most 7 mm/s before the look round and 12 mm/s after it. Its feet
 * take 39 strides of 2 mm over the walk (jb_bug.vert). The gait phase is the
 * distance walked, 39 x 2 pi rad over the walk, so each foot stays put on the
 * leaf the whole time it's down. A whole number of strides fits up to the
 * look round and up to the end, so it stands on all six feet at both. */
static float jb_walk(float j) {
    return 0.5f * (mc_smoothstepf(S3_MACRO - 2.0f, 33.0f, j) + mc_smoothstepf(35.0f, S3_FLY - 0.8f, j));
}

/* a point on its path, w along it, dv off it across the blade */
static mc_v3 jb_path(float w, float dv) {
    return jb_leaf_at(0.3f + 0.185f * w, 0.13f + 0.046f * sinf(w * 5.0f) + dv);
}

static void s3_bug_at(float j, mc_v3 *pos, mc_v3 *fwd, mc_v3 *up) {
    float w = jb_walk(j), t = j - S3_FLY - 1.7f;
    /* heading the way it walks, following its weave, not crabbing sideways */
    mc_v3 p = jb_path(w, 0.0f), f = mc_v3_sub(jb_path(w + 0.01f, 0.0f), p);
    *up  = mc_v3_normalize(mc_v3_cross(f, mc_v3_sub(jb_path(w, 0.01f), p)));
    *fwd = mc_v3_normalize(f);
    /* The take-off: up off the leaf, then away into the light, nose high and
     * weaving a little. (The shaders square the frame up again.) */
    if (t > 0.0f) {
        mc_v3 dir = mc_vec3(-0.045f, 0.91f, -0.41f);
        float k = mc_smoothstepf(0.0f, 0.7f, t);
        p = mc_v3_add(p, mc_v3_mad(mc_v3_scale(*up, 0.008f * k), dir, t * t * (0.07f + 0.05f * t)));
        p.x += 0.012f * k * sinf(t * 7.0f);
        *fwd = mc_v3_lerp(*fwd, dir, k * 0.85f);
    }
    *pos = p;
}


static void bug_draw(Demo *d, const S3Ctx *c) {
    /* The ladybug for the shaders: where it is, its heading and up vector, and
     * how far it is through its walk, the opening of its wing cases, the
     * unfolding of its wings and its flight. */
    float j = c->j;
    mc_v3 p, f, n;
    s3_bug_at(j, &p, &f, &n);
    mc_m4 bug = {{ p.x, p.y, p.z, jb_walk(j) * 245.0f,  f.x, f.y, f.z, mc_smoothstepf(S3_FLY, S3_FLY + 0.9f, j),
                   n.x, n.y, n.z, mc_smoothstepf(S3_FLY + 0.5f, S3_FLY + 1.3f, j),
                   mc_smoothstepf(S3_FLY + 1.2f, S3_FLY + 1.7f, j), 0, 0, 0 }};
    /* the plant: blades and stalks, two-sided, in the lit pass and the sun's map */
    if (c->pass != S3_CHAR) {
        mc_shader_use(d->jb_leaf);
        mc_set_m4(0, bug);   /* uBug */
        glDisable(GL_CULL_FACE);
        glBindVertexArray(mc_gl_fullscreen_vao());
        glDrawArraysInstanced(GL_TRIANGLES, 0, JB_VERTS, 14);
    }
    if (c->pass != S3_SHADOW && s3_visible(d, c, p, 0.08f)) {   /* the bug and its dew drops (S3_CHAR: its shadow) */
        mc_shader_use(d->jb_bug);
        mc_set_m4(0, bug);   /* uBug; uFirst is 1 */
        glEnable(GL_CULL_FACE);
        glBindVertexArray(mc_gl_fullscreen_vao());
        glDrawArraysInstanced(GL_TRIANGLES, 0, JB_BALL, JB_PARTS);
        /* the hind wings, last: translucent, over everything else */
        if (c->pass == S3_CHAR) return;
        mc_set_i(1, JB_PARTS);
        mc_gl_switch(MC_ZWRITE_OFF | MC_CULL_OFF | MC_BLEND_ON);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDrawArraysInstanced(GL_TRIANGLES, 0, JB_BALL, JB_WINGS);
        mc_set_i(1, 0);
        mc_gl_switch(MC_ZWRITE_ON | MC_BLEND_OFF);
    }
    glEnable(GL_CULL_FACE);
}
