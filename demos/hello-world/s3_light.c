/*
 * s3_light.c - scene 3's light: the sun and the sky, the frame's lighting
 * terms, the sun's shadow map, the light shafts through the haze, and the
 * grade with its depth of field. (#included by scene3.c.)
 *
 * The viewer barely moves, so the sun's shadow map is a fixed box round the
 * clearing, with no fitting and so no swimming. Its 3.5 cm texels are sharp
 * enough for leaves close by, and s3shadow() (in the s3 common block) widens
 * its filter with the distance to the blocker, so the canopy far overhead
 * casts soft dapples. A second map of the same size covers the beach and the
 * forest's edge. It has the same sun and the same depths, only slid across,
 * so a scale and an offset (in uRim) take a point's clearing-map coordinates
 * to the beach map's. Both maps are drawn once. The characters that move
 * through them (the ladybug, the tyrannosaurus and the blob) get a third map
 * every frame, a tight box round whichever one is on show (scale and offset
 * in uSun) that is fine enough for the ladybug's feet. All three maps are
 * compared in hardware, with each tap filtered over 2 x 2 texels, and a
 * separate sampler reads the raw depths of the first two for the blocker
 * search. The shafts march the same map through the air at quarter
 * resolution (jl_vol.frag), and the engine blurs them and adds them in,
 * depth-aware.
 *
 * The open sky is a planet's atmosphere, not a gradient. Each frame
 * jl_atmo.frag marches it into a 64 x 32 table of radiance by direction,
 * which stays bound on unit 15. The sky, the reflections in the sea and the
 * sand, and the haze over the beach all read it (s3sky). The sky pass adds
 * the sun's disc and glare to the atmosphere and thin horizon clouds.
 */
#define S3_SHADOW_RES 2048
#define S3_SHADOW_R   36.0f     /* each map covers this radius (m): round the clearing, round the beach */
#define S3_CHAR_RES   1024
static mc_target jl_shadow, jl_beach, jl_char, jl_vol[2];   /* the sun's depth (clearing, beach, characters); quarter-res shafts and blur scratch */
static mc_target jl_atmo;                                    /* the open sky's radiance by direction (s3sky) */
static mc_m4     jl_beach_vp, jl_char_vp;                     /* the sun's view of the beach and of the character on show */
static float     jl_char_r;                                    /* that box's radius (m), 0 when no one is on show */
static GLuint    jl_raw;                                       /* a sampler reading the maps' raw depths */

static int jl_shadow_done;   /* the cached sun maps have been drawn (release) */

static int light_make(Demo *d) {
    jl_shadow_done = 0;
    (void)d;
    jl_shadow = mc_target_make_format(S3_SHADOW_RES, S3_SHADOW_RES, 0, 1);
    jl_beach  = mc_target_make_format(S3_SHADOW_RES, S3_SHADOW_RES, 0, 1);
    /* The characters' map has a colour target too (1 MB), so the ladybug,
     * the tyrannosaurus and the blob draw into it with the same single colour
     * output as in the main pass, and the driver reuses the shader variants
     * it built at load. With a depth-only target, Mesa's Intel driver
     * recompiled each of them the first time it appeared: 70-80 ms and a
     * dropped frame on a UHD 620 with a cold shader cache. The blob marks
     * there which parts let the sun through (uSunT: 1 for its jelly, 0.5 for
     * the parasol's cloth, 0 for the rest of what it carries). The two still
     * maps have no colour target, since they're drawn once, under Scene 3's
     * white. */
    jl_char   = mc_target_make_format(S3_CHAR_RES, S3_CHAR_RES, GL_R8, 1);
    GLuint t[3] = { jl_shadow.depth, jl_beach.depth, jl_char.depth };
    for (int i = 0; i < 3; ++i) {                    /* compared, 2 x 2 filtered */
        glBindTexture(GL_TEXTURE_2D, t[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    /* ...and a raw sampler (no compare, no mipmaps), nearest both ways. The
     * blocker search's taps are rotated per pixel, so their implicit level of
     * detail comes from the hash (minified, so nearest), and past a cut-out's
     * discard it's whatever the quad's discarded lanes held. With
     * magnification left linear, a few pixels of the cut-out plants switched
     * filter from frame to frame and run to run. Those were the scattered
     * pixels in Scene 3 that didn't repeat from one run to the next. */
    glGenSamplers(1, &jl_raw);
    glSamplerParameteri(jl_raw, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(jl_raw, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    jl_atmo = mc_target_make(64, 32, 1, 0);
    return mc_target_ok(&jl_shadow) && mc_target_ok(&jl_beach) && mc_target_ok(&jl_char) && mc_target_ok(&jl_atmo);
}

static void light_free(Demo *d) {
    (void)d;
    mc_target_free(&jl_shadow);
    mc_target_free(&jl_beach);
    mc_target_free(&jl_char);
    mc_target_free(&jl_atmo);
    for (int i = 0; i < MC_ARRAY_COUNT(jl_vol); ++i) mc_target_free(&jl_vol[i]);
}

/* The sun's view-projection of a 2r box round p. It's slid across the sun's
 * rays from `at` (the clearing's centre), so every map shares the same depths
 * (60..260 m). */
static mc_m4 jl_sun_vp(mc_v3 sun, mc_v3 at, mc_v3 p, float r) {
    mc_v3 b = mc_v3_sub(p, at);
    at = mc_v3_add(at, mc_v3_sub(b, mc_v3_scale(sun, mc_v3_dot(b, sun))));
    mc_m4 lview = mc_m4_look_at(mc_v3_mad(at, sun, 150.0f), at, mc_vec3(0.0f, 0.0f, -1.0f));
    return mc_m4_mul(mc_m4_ortho(-r, r, -r, r, 60.0f, 260.0f), lview);
}
/* The scale and offset that take a point's uv in the clearing map to its uv
 * in map vp: uv' = uv * s + o, where s is the clearing map's radius over
 * vp's. Both maps are orthographic. */
static mc_v3 jl_map_uv(const FrameUniforms *F, mc_m4 vp, mc_v3 at, float s) {
    mc_v4 p = mc_m4_mul_v4(F->light_vp, mc_vec4(at.x, at.y, at.z, 1.0f)), q = mc_m4_mul_v4(vp, mc_vec4(at.x, at.y, at.z, 1.0f));
    return mc_vec3(s, 0.5f * (1.0f - s + q.x - p.x * s), 0.5f * (1.0f - s + q.y - p.y * s));
}

/* Fills in the Frame block's lighting terms (see the top of scene3.c) for a
 * bright, humid tropical day. A warm sun stands high to the front-left of the
 * opening view, and the shade gets the cool green light of the canopy. Also
 * sets the sun's view-projections: the fixed boxes round the clearing and
 * the beach, and the box round the character on show. */
static void light_frame(Demo *d, S3Ctx *c) {
    FrameUniforms *F = &d->frame;
    frame_basics(d, c->vp, c->cam.pos, c->t);
    F->light_dir    = mc_v3_normalize(mc_vec3(-0.355f, 0.743f, -0.567f));   /* ~48 deg up */
    F->glow         = mc_vec3(5.8f, 5.0f, 3.7f);      /* the sun */
    F->sky_color    = mc_vec3(0.24f, 0.40f, 0.28f);   /* daylight filtered by the leaves */
    F->ground_color = mc_vec3(0.06f, 0.07f, 0.03f);   /* the floor's green-brown bounce */
    F->fog_color    = mc_vec3(0.09f, 0.19f, 0.17f);   /* the haze in the shade: teal-green (gold toward the sun, s3fog) */
    F->fog          = 0.011f;                          /* dense enough that the trunks recede in layers */
    F->top          = mc_v3_lerp(mc_vec3(0.22f, 0.48f, 1.25f), mc_vec3(0.06f, 0.27f, 1.25f), c->beach);   /* the open sky, */
    F->bottom       = mc_v3_lerp(mc_vec3(1.35f, 1.55f, 1.65f), mc_vec3(0.72f, 1.02f, 1.45f), c->beach);   /* bluer at the beach */
    F->wobble       = 0.5f + 0.5f * sinf(c->t * 0.21f);
    F->clarity      = c->macro;
    F->real         = c->beach;
    F->q            = 0.0f;
    mc_v3 at = mc_vec3(0.0f, 14.0f, -1.0f), p, f, n;
    F->light_vp = jl_sun_vp(F->light_dir, at, at, S3_SHADOW_R);
    jl_beach_vp = jl_sun_vp(F->light_dir, at, s3_beach_pt(45.0f, 0.0f), S3_SHADOW_R);
    F->rim      = jl_map_uv(F, jl_beach_vp, at, 1.0f);
    /* A tight box round the character on show. Its shadow lands where the
     * sun's rays through it do, which is inside its box in the map however
     * far below. With no one on show there's no map, and every lookup falls
     * outside it. The blob's jelly lets 0.3 of the sun through. At 0.5 the
     * shadow was so faint that the blob seemed to hover. The post's contact
     * shadows stay on and darken the sand at its foot. */
    float r = 0.0f;
    F->blob_fog = 0.0f;                              /* the character's shadow is opaque, */
    if (c->macro > 0.0f) { s3_bug_at(c->j, &p, &f, &n); r = 0.03f; }
    else if (c->j > S3_BLOB - 0.5f) { s3_blob_at(c->j, &p); r = c->j < JO_PROPS ? 1.2f : 1.8f; F->blob_fog = 0.3f; }   /* except the jelly's: 0.3 (and a wider box once it has the parasol) */
    else if (c->j > S3_REX - 4.0f) { s3_rex_at(c->j, &p, &f); p.y += 2.5f; r = 7.0f; }
    F->sun = mc_vec3(1.0f, -9.0f, -9.0f);
    if ((jl_char_r = r) > 0.0f) {
        jl_char_vp = jl_sun_vp(F->light_dir, at, p, r);
        F->sun     = jl_map_uv(F, jl_char_vp, at, S3_SHADOW_R / r);
        F->gape    = (90.0f - mc_v3_dot(mc_v3_sub(p, at), F->light_dir)) * 0.005f;   /* its middle's depth in the map */
    }
}

/* The sun's depth passes. Every package draws its casters with S3_SHADOW,
 * seen from the sun (uViewProj = uLightVP, uQ = 1), into the clearing's and
 * the beach's maps. The sun and those casters stand still, so a release build
 * draws them once, for one fixed moment, and the leaves' flutter would be
 * sub-texel anyway. Dev builds redraw them every frame so hot reloads show.
 * The characters move, so they draw themselves into their own map (S3_CHAR)
 * every frame. Then the maps go on texture units 4-8 for the lit passes
 * (s3 common block), with the characters' colour on unit 3 and the sky's
 * table on unit 15. */
static void light_shadow(Demo *d, S3Ctx *c) {
    FrameUniforms *F = &d->frame;
    mc_m4 sun = F->light_vp;
    mc_gl_frame_block(F, sizeof *F);                 /* first the sky's table: all the haze reads it */
    mc_target_bind(&jl_atmo);
    mc_gl_switch(MC_ZTEST_OFF | MC_ZWRITE_OFF);
    mc_shader_use(d->jl_atmo);
    mc_fullscreen_triangle();
    glDepthFunc(GL_LESS);
#ifdef MC_RELEASE
    if (!jl_shadow_done) {
#endif
    /* The still maps always show the world as it is half a second before the
     * jungle opens, whichever frame draws them. That pins down the wind
     * (uWobble and trees_draw's gusts both bend the trees), the eye (jt.frag
     * thins a curtain of lianas seen from below, and jt.vert turns cards
     * toward it) and the forest (uReal clears the beach part's trees). So the
     * maps are the same whatever draws them first: the show reaching Scene 3,
     * a still or a seek anywhere, or the warm-up at load. When they were
     * drawn with the current frame's state, the beach's shadows came from
     * whichever still was drawn first. */
    S3Ctx s;
    s3_setup(d, S3_J0 - 0.5f, &s);
    light_frame(d, &s);
    s.pass = S3_SHADOW;
    F->q = 1.0f;
    F->time = S3_J0;
    shadow_begin(d, &jl_shadow);
    s3_draw_world(d, &s);
    /* A gap in the canopy right over the hero plant. The map is cleared above
     * its leaf to a depth just under it, so a fleck of sun lands on the leaf
     * and the ladybug, and a shaft stands over them in the haze. The leaf
     * still shades what's below it. */
    mc_v3 h = mc_v3_sub(jb_leaf_at(0.42f, 0.12f), mc_v3_scale(F->light_dir, 0.3f));
    mc_v4 q = mc_m4_mul_v4(F->light_vp, mc_vec4(h.x, h.y, h.z, 1.0f));
    glEnable(GL_SCISSOR_TEST);
    glScissor((int)((q.x * 0.5f + 0.5f) * S3_SHADOW_RES) - 6, (int)((q.y * 0.5f + 0.5f) * S3_SHADOW_RES) - 6, 12, 12);
    glClearDepth(q.z * 0.5f + 0.5f);
    glClear(GL_DEPTH_BUFFER_BIT);
    glClearDepth(1.0);
    glDisable(GL_SCISSOR_TEST);
    /* the beach and the forest's edge */
    F->light_vp = jl_beach_vp;
    shadow_begin(d, &jl_beach);
    s3_draw_world(d, &s);
    light_frame(d, c);                               /* back to this frame's */
    jl_shadow_done = 1;
#ifdef MC_RELEASE
    }
#endif
    F->q = 1.0f;
    if (jl_char_r > 0.0f) {
        S3Ctx k = *c;
        k.pass = S3_CHAR;
        F->light_vp = jl_char_vp;
        shadow_begin(d, &jl_char);
        if (c->macro > 0.0f) bug_draw(d, &k);
        rex_draw(d, &k);
        blob_draw(d, &k);
    }
    F->light_vp = sun;
    shadow_end(d, c->vp);
    F->q = 0.0f;
    GLuint t[6] = { jl_char.color, jl_shadow.depth, jl_beach.depth, jl_char.depth, jl_shadow.depth, jl_beach.depth };
    for (int i = 0; i < 6; ++i) { glActiveTexture(GL_TEXTURE3 + i); glBindTexture(GL_TEXTURE_2D, t[i]); }
    glBindSampler(7, jl_raw);
    glBindSampler(8, jl_raw);
    glActiveTexture(GL_TEXTURE15);                   /* and the sky's table on 15 */
    glBindTexture(GL_TEXTURE_2D, jl_atmo.color);
    glActiveTexture(GL_TEXTURE0);
}

/* The clear sky, behind everything. The full-screen pass lies on the far
 * plane and comes after the world, so only the gaps in the leaves pay for
 * it. This also sizes the shafts' two quarter-resolution targets. */
static void light_sky(Demo *d, const S3Ctx *c) {
    FrameUniforms *F = &d->frame;
    int w, h;
    mc_renderer_size(d->R, &w, &h);
    int x = (w + 3) / 4, y = (h + 3) / 4;
    for (int i = 0; i < MC_ARRAY_COUNT(jl_vol); ++i) {
        if (jl_vol[i].w != x || jl_vol[i].h != y) { mc_target_free(&jl_vol[i]); jl_vol[i] = mc_target_make(x, y, 1, 0); }
    }
    F->view_proj = c->vp;
    mc_gl_frame_block(F, sizeof *F);
    MC_PASS(MC_P_SCENE);
    mc_renderer_bind_scene(d->R);
    glActiveTexture(GL_TEXTURE0);
    mc_gl_switch(MC_ZTEST_ON | MC_ZWRITE_OFF);
    glDepthFunc(GL_LEQUAL);
    mc_shader_use(d->jl_sky);
    mc_fullscreen_triangle();
    glDepthFunc(GL_LESS); glDepthMask(GL_TRUE);
}

/* The shafts: march the view rays through the haze at quarter resolution
 * against the sun's map, then blur them depth-aware. The engine adds them in
 * at the grade. */
static void light_volume(Demo *d, const S3Ctx *c) {
    MC_PASS(MC_P_RAYS);
    mc_gl_switch(MC_ZTEST_OFF | MC_ZWRITE_OFF);
    mc_target_bind(&jl_vol[0]);
    mc_shader_use(d->jl_vol);
    glBindTexture(GL_TEXTURE_2D, mc_renderer_depth(d->R));
    mc_fullscreen_triangle();
    mc_renderer_blur(d->R, &jl_vol[0], &jl_vol[1], c->cam.znear, c->cam.zfar);
    mc_renderer_bind_scene(d->R);
    MC_PASS(MC_P_SCENE);
    mc_gl_switch(MC_ZTEST_ON | MC_ZWRITE_ON | MC_CULL_ON | MC_BLEND_OFF);
}

/* The grade: filmic, with a little extra saturation. It adds ambient
 * occlusion in the wide shots, a shallow depth of field on the ladybug, and
 * the shafts. While we look out at the beach from the trees the exposure
 * stays set for the forest, so the sand glows past the dark trunks and the
 * tyrannosaurus is silhouetted against it. Once we step out into the sun the
 * exposure adapts, and a long lens softens what's behind the blob. */
static void light_post(Demo *d, const S3Ctx *c, mc_post *p) {
    int rw, rh;
    mc_renderer_size(d->R, &rw, &rh);
    float out = mc_smoothstepf(S3_CLEAR, S3_BLOB + 4.0f, c->j);
    p->exposure   = (1.0f - 0.2f * c->macro) * (1.0f - 0.2f * c->beach) * (1.0f - 0.48f * out);
    p->saturation = 1.12f - 0.1f * c->macro + 0.12f * out;
    p->bloom      = 0.08f;
    p->vignette   = 0.38f;
    p->chroma     = 0.2f;
    p->grain      = 0.008f;   /* keep the macro leaf and distant sand quiet */
    p->ssao       = 0.6f * (1.0f - c->macro);
    /* The forest closes in as the animal emerges. Keep its sunlit face
     * readable, and let the grade recover through the hush before the beach. */
    float tension = mc_smoothstepf(S3_GAP, S3_REX + 3.0f, c->j)
                  * (1.0f - mc_smoothstepf(S3_SHORE, S3_CLEAR, c->j));
    p->exposure   *= 1.0f - 0.18f * tension;
    p->saturation -= 0.13f * tension;
    p->vignette   += 0.18f * tension;
    post_depth(d, &c->cam, p);
    p->vol_tex    = jl_vol[0].color;
    p->vol        = 1.0f;
    /* Shallow focus on the ladybug. Once it has flown the canopy sharpens,
     * and the wide shots of the beach stay sharp from our feet to the
     * horizon. */
    p->dof        = 32.0f * c->macro * (1.0f - mc_smoothstepf(S3_FLY + 1.0f, S3_FLY + 3.5f, c->j)) * (float)rh / 720.0f;
    p->focus      = c->focus;
}
