/*
 * s3_rex.c - scene 3's tyrannosaurus, stalking past along the forest's edge.
 * (#included by scene3.c.)
 *
 * It's sculpted once at load along with scene 2's animals (main.c's
 * uw_sculpt). jr.comp evaluates its smooth union of round cones and
 * ellipsoids on a 5 cm grid and meshes the surface on the GPU with surface
 * nets, tagging each vertex with the two bones that hold it (d->jr_body, its
 * vertices on binding 5). Every frame jr.vert poses that mesh: the walk, the
 * legs' IK, the tail, the head's look and the jaw. It also draws a patch of
 * the floor under it, multiplied in, as its contact shadow. Its sun shadow
 * comes from the characters' map, which it draws itself into every frame
 * (S3_CHAR: depth only, no patch). The walk here and in the shader must
 * agree.
 */
#define JR_PATCH (40 * 24 * 6)        /* the floor patch's vertices (jr.vert) */

/* How far across (s3_across) its hips are at jungle time j. It walks at
 * 3 m/s from S3_REX and comes to a stop at j 58.1 in the gap between the two
 * trunks, where it looks at us. Then it walks on and away, off to the right
 * well before the blob comes out. Mirrored by across() in jr.vert and by the
 * footprints in ju_ground.frag. */
static float jr_across(float j) {
    float x = fminf(fmaxf((j - 58.1f) / 1.3f, -1.0f), 1.0f);
    return -16.5f + 3.0f * (j - S3_REX) - 3.9f * (0.5f * (x + 1.0f) + sinf(MC_PI * x) / MC_TAU);
}

/* Where it is at jungle time j (the floor under its hips), and which way it
 * faces. It walks the edge from left to right (as seen looking out to sea)
 * along S3_REX_S. The camera glances after it and shakes at each footfall
 * (s3_rex_step). */
static void s3_rex_at(float j, mc_v3 *pos, mc_v3 *fwd) {
    mc_v3 p = s3_beach_pt(S3_REX_S, jr_across(j));
    p.y = s3_ground(p.x, p.z);
    *pos = p;
    *fwd = s3_beach_pt(0.0f, 1.0f);
}

/* 1 just after a foot comes down, decaying fast, and 0 once it has gone. A
 * foot comes down every S3_STEP from S3_REX, left foot first, thirteen times,
 * in time with the music. The rise to 1 takes 40 ms, because a drop within
 * one frame looked like a skipped frame at 60 Hz rather than a thump. It
 * runs to the end of the thirteenth step. Stopping at S3_SHORE + 1, 30 ms
 * into the tenth, gave a one-frame drop and snap back. */
static float s3_rex_step(float j) {
    float k = (j - S3_REX) / S3_STEP, f = k - floorf(k);
    if (k < 0.0f || k >= 13.0f) return 0.0f;
    return fminf(f * 24.0f, 1.0f) * expf(-8.0f * f);
}

static int rex_make(Demo *d) { (void)d; return 1; }   /* it is sculpted with the sea's animals (uw_sculpt) */
static void rex_free(Demo *d) { (void)d; }

static void rex_draw(Demo *d, const S3Ctx *c) {
    mc_v3 p, f;
    s3_rex_at(c->j, &p, &f);
    /* Start while the entire animal is still beyond the left edge. Its head,
     * shoulders and tail then cross the real trunks continuously; there is no
     * visibility fade or cut when the first footfall reaches the camera. */
    if (c->pass == S3_SHADOW || c->j < S3_REX - 4.0f || c->j > S3_BLOB
        || !s3_visible(d, c, mc_v3_add(p, mc_vec3(0.0f, 3.0f, 0.0f)), 9.0f)) return;
    mc_shader_use(d->jr_rex);
    mc_mesh_draw(d->jr_body);                                         /* closed: its back faces culled */
    if (c->pass == S3_LIT) {                                          /* the floor patch */
        mc_gl_switch(MC_ZWRITE_OFF | MC_CULL_OFF | MC_BLEND_ON);
        glBlendFunc(GL_ZERO, GL_SRC_COLOR);
        glDrawArrays(GL_TRIANGLES, 1 << 24, JR_PATCH);
        mc_gl_switch(MC_ZWRITE_ON | MC_CULL_ON | MC_BLEND_OFF);
    }
}
