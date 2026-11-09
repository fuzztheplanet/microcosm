/*
 * s3_birds.c - scene 3's birds (and two butterflies). (#included by scene3.c.)
 *
 * Scarlet macaws cross the gap in the canopy while we look up, and a flock of
 * parrots goes over higher up, the other way. A pair flies far off between
 * the trunks during the look round, and two blue morphos flutter past near
 * the eye. Routes, shapes and wingbeats are all generated in jv_bird.vert
 * from gl_InstanceID / gl_VertexID, so the lot takes one instanced draw.
 */
#define JV_VERTS (4 * 64 * 6)   /* an 8x8 quad grid each for the body, two wings and the tail */
#define JV_COUNT 17         /* 4 macaws, 9 parrots, the far pair, 2 morphos */

static int birds_make(Demo *d) { (void)d; return 1; }
static void birds_free(Demo *d) { (void)d; }

static void birds_draw(Demo *d, const S3Ctx *c) {
    if (c->pass == S3_SHADOW || c->j > S3_FIND + 1.5f) return;
    glDisable(GL_CULL_FACE);
    mc_shader_use(d->jv_bird);
    glBindVertexArray(mc_gl_fullscreen_vao());
    glDrawArraysInstanced(GL_TRIANGLES, 0, JV_VERTS, JV_COUNT);
    glEnable(GL_CULL_FACE);
}
