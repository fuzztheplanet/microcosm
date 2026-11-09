/*
 * s3_beach.c - scene 3's beach and sea, seen out past the forest's edge.
 * (#included by scene3.c.)
 *
 * The sand belongs to the floor and is drawn by ju_ground. Past S3_EDGE the
 * litter thins out onto dry, rippled sand that glints, with a wrack line at
 * the top of the waves' reach. The swash darkens the sand it runs over, and
 * each sheet leaves a glossy wet film and a line of stranded foam as it
 * drains. ju_ground also throws caustics on the seabed, grows seagrass out
 * past the surf, and keeps the tyrannosaurus's tracks and the blob's wet
 * trail.
 *
 * This file draws the sea: one polar grid of vertices made in jw_sea.vert,
 * centred on the upper beach and reaching the horizon, with its rings 30 cm
 * apart through the surf. The swell comes from the surf block shared by the
 * sea, the sand and the blob. It slows and bunches up over the shelving sand,
 * comes in sets, and steepens and breaks into bores that run up the sand as
 * thin sheets. On top of it, jw_sea sums the wind's short-crested waves per
 * pixel. Detail the pixel can't resolve widens the sun's highlight and tilts
 * the reflection toward the sky overhead, so the water far out is deep blue.
 *
 * The sea is drawn after the floor, from the horizon in, with dual-source
 * blending. What it scatters and reflects is added, and what lies beneath is
 * seen through a per-channel transmittance: clear turquoise shallows over
 * sand and seagrass, deep blue beyond. Past the floor's reach, about 80 m
 * out, the sea draws its own bottom. The depth it writes lies up to 1.2 m
 * under its surface but never past the floor. That keeps it smooth for the
 * ambient occlusion and the depth of field, and leaves room for the blob's
 * body under the water, since the blob is drawn late and shades its own
 * underwater part. The water's reflection is the atmospheric sky
 * in jw_sea.frag; no screen-space reflection target or late pass is needed.
 */
#define JW_RINGS   190     /* the sea grid: rings x sectors over the half-plane seaward */
#define JW_SECTORS 96

static int beach_make(Demo *d) { (void)d; (void)s3_roam_dist; return 1; }   /* the forest keeps out of the glide */
static void beach_free(Demo *d) { (void)d; }

static void beach_draw(Demo *d, const S3Ctx *c) {
    if (c->beach <= 0.0f) return;
    if (c->pass != S3_LIT) return;
    mc_v3 b;
    mc_v4 sp[2];
    s3_blob_at(c->j, &b);
    jo_splashes(c->j, sp);
    glBindVertexArray(mc_gl_fullscreen_vao());
    mc_gl_switch(MC_CULL_OFF | MC_BLEND_ON);
    glBlendFunc(GL_ONE, GL_SRC1_COLOR);
    mc_shader_use(d->jw_sea);
    mc_set_v4(0, mc_vec4(b.x, b.y, b.z, S3_BLOB_R));            /* uB, uS[2]: as jo_blob's */
    glUniform4fv(2, 2, &sp[0].x);                               /* the rings its splashes leave */
    glDrawArrays(GL_TRIANGLES, 0, JW_RINGS * JW_SECTORS * 6);
    mc_gl_switch(MC_CULL_ON | MC_BLEND_OFF);
}
