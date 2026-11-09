/*
 * s3_under.c - scene 3's forest floor and undergrowth. (#included by scene3.c.)
 *
 * All of it is grown on the GPU from vertex ids, with no meshes. The floor is
 * a polar grid (ju_ground), and every grass blade, fern frond, broad leaf and
 * heliconia bract is a bent strip (ju_plant). The CPU only decides where the
 * plants stand, clear of the trunks, the hero plant and the camera, and hands
 * the GPU that table. The shader scatters the grass itself.
 */
#define JU_RINGS   80        /* the floor grid: rings x 160 sectors */
#define JU_TUFTS   4500      /* grass tufts (16 blades each) */
#define JU_MAXP    1000      /* plant table entries (uP[] in ju_plant.vert) */

static float ju_pl[JU_MAXP][4];       /* x, z, size, seed: ferns, then broad leaves, then heliconias */
static int   ju_n[3], ju_cast[3];   /* per kind: all, within 25 m */
static float ju_tr[48][4];            /* trunks near the viewer, for the floor's moss and shade */
static int   ju_nt;

/* The camera's path over the floor (x, z of scene3.c's keys, ending at the
 * leaf). Tall plants keep their reach away from it. */
static const float JU_PATH[][2] = {
    { 0.15f, 3.4f }, { 0.05f, 1.8f }, { 0.0f, 0.1f }, { 0.15f, 0.0f }, { 0.65f, 0.18f }, { 1.1f, 0.41f },
};

static float hypot2(float x, float z) { return sqrtf(x * x + z * z); }

static float ju_path_gap(float x, float z) {
    float best = 1e9f;
    for (int i = 0; i + 1 < MC_ARRAY_COUNT(JU_PATH); ++i) {
        float ax = JU_PATH[i][0], az = JU_PATH[i][1], dx = JU_PATH[i + 1][0] - ax, dz = JU_PATH[i + 1][1] - az;
        float u = mc_saturatef(((x - ax) * dx + (z - az) * dz) / (dx * dx + dz * dz));
        best = fminf(best, hypot2(x - ax - u * dx, z - az - u * dz));
    }
    return best;
}

static int under_make(Demo *d) {
    (void)d; (void)s3_clear;   /* the shared rule's path circle is too wide for low plants */
    /* Per kind: how many, how far out they spread, and their size range (m).
     * Each kind's plants are laid out nearest first (rings outward, at any
     * free angle on each ring), so the draws can split them by distance. */
    static const struct { int n; float far, s0, s1; } KIND[3] = {
        { 420, 50.0f, 0.45f, 2.4f },   /* ferns, the biggest of them young palms */
        { 540, 42.0f, 0.08f, 1.6f },   /* broad leaves, from seedlings to elephant ears */
        { 32,  26.0f, 0.8f, 1.2f },    /* heliconias */
    };
    mc_rng rng = mc_rng_seed(0x7E57F0u);
    int np = 0;
    for (int k = 0; k < 3; ++k) {
        ju_n[k] = ju_cast[k] = 0;
        for (int i = 0; i < KIND[k].n; ++i) {
            float r = 0.6f + KIND[k].far * powf((i + mc_rng_f(&rng)) / KIND[k].n, 1.7f);
            float s = mc_lerpf(KIND[k].s0, KIND[k].s1, powf(mc_rng_f(&rng), k == 1 ? 2.5f : 2.0f));
            float reach = s * (k == 2 ? 1.2f : 0.85f), gap = s * (k == 2 ? 1.6f : 1.0f) > 0.8f ? reach + 0.3f : 0.2f;
            for (int tries = 0; tries < 24; ++tries) {
                float an = mc_rng_range(&rng, 0.0f, MC_TAU), x = 0.4f + cosf(an) * r, z = 1.0f + sinf(an) * r;
                int ok = ju_path_gap(x, z) > gap && hypot2(x - S3_HERO_X, z - S3_HERO_Z) > S3_KEEP_R + reach;
                for (int t = 0; t < s3_nforest && ok; ++t)
                    ok = hypot2(x - s3_trees[t].p.x, z - s3_trees[t].p.z) > s3_trees[t].r * 1.6f + reach * 0.5f;
                for (int t = 0; t < np && ok; ++t)        /* and a little room from the others */
                    ok = hypot2(x - ju_pl[t][0], z - ju_pl[t][1]) > 0.3f * (s + ju_pl[t][2]);
                if (!ok) continue;
                ju_pl[np][0] = x; ju_pl[np][1] = z; ju_pl[np][2] = s; ju_pl[np][3] = mc_rng_f(&rng);
                ++np; ++ju_n[k];
                ju_cast[k] += r < 25.0f;
                break;
            }
        }
    }
    /* The beach part, applied afterwards so the forest round the clearing
     * stays as it was. Nothing grows out on the sand, the undergrowth thins
     * toward the edge, and no tall plants stand in the camera's glide. Broad
     * leaves keep 4.2 m clear of where the camera crouches for the
     * tyrannosaurus: from 0.6 m up on a long lens, a seedling 4 m away sat
     * under the beast in the frame like a dark, two-faceted clam. Then low
     * creepers are added along the top of the beach. They're broad leaves, so
     * they go in that kind's block. */
    static float out[JU_MAXP][4];
    mc_v3 cr = s3_beach_pt(13.2f, 1.6f);   /* where the camera crouches for the tyrannosaurus */
    for (int k = 0, i = 0, kept = 0; k < 3; ++k) {
        int nk = 0, cs = 0;
        for (int e = 0; e < ju_n[k]; ++e, ++i) {
            float x = ju_pl[i][0], z = ju_pl[i][1], s = ju_pl[i][2], a = s3_along(x, z), r = hypot2(x - 0.4f, z - 1.0f);
            if (a > S3_EDGE - 0.5f - 0.4f * s || ju_pl[i][3] < (a - 25.0f) / 7.0f
                || (s > 0.9f && a > 3.0f && s3_roam_dist(x, z) < 0.85f * s + 0.4f)
                || (k == 1 && hypot2(x - cr.x, z - cr.z) < 4.2f)) continue;
            memcpy(out[kept++], ju_pl[i], sizeof out[0]);
            ++nk; cs += r < 25.0f;
        }
        for (int e = 0; k == 1 && e < 60; ++e) {
            mc_v3 p = s3_beach_pt(S3_EDGE + 0.5f + 0.09f * e, (float)((e * 37) % 60) - 30.0f);
            if (fabsf(s3_across(p.x, p.z) - 0.8f) < 2.0f) continue;   /* the camera walks out here */
            out[kept][0] = p.x; out[kept][1] = p.z; out[kept][2] = 0.12f + 0.004f * (e % 50); out[kept][3] = e * 0.0161f;
            ++kept; ++nk;
        }
        ju_n[k] = nk; ju_cast[k] = cs;
    }
    memcpy(ju_pl, out, sizeof ju_pl);
    for (int i = 0; i < s3_ntree && ju_nt < 48; ++i)
        if (s3_trees[i].kind < 3 && hypot2(s3_trees[i].p.x - 0.4f, s3_trees[i].p.z - 1.0f) < 45.0f) {
            float reach = s3_trees[i].r * 2.0f + 0.6f;
            ju_tr[ju_nt][0] = s3_trees[i].p.x; ju_tr[ju_nt][1] = s3_trees[i].p.z; ju_tr[ju_nt][3] = reach * reach;
            ++ju_nt;
        }
    return 1;
}
static void under_free(Demo *d) { (void)d; }

/* One kind's strips: uK = kind, segments, strips per plant, first plant. */
static void ju_strips(float kind, int seg, int per, int first, int n) {
    mc_set_v4(0, mc_vec4(kind, (float)seg, (float)per, (float)first));
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, seg * 2 + 2, n);
}

static void under_draw(Demo *d, const S3Ctx *c) {
    glBindVertexArray(mc_gl_fullscreen_vao());
    glDisable(GL_CULL_FACE);
    mc_shader_use(d->ju_plant);
    glUniform4fv(1, JU_MAXP, ju_pl[0]);   /* uP (uK is 0) */
    /* per kind: segments fine / coarse, half-strips per plant (two per leaf) */
    static const int SEG[3][3] = { { 12, 6, 20 }, { 10, 5, 10 }, { 10, 5, 26 } };
    for (int k = 0, first = 0, lit = c->pass == S3_LIT; k < 3; first += ju_n[k++])
        /* The picture gets every plant in fine strips for the whole scene. If
         * only the near ones were fine, the others would change shape as the
         * eye came by. The sun's map gets the coarse ones, out to 25 m. */
        ju_strips(k + 1.0f, SEG[k][!lit], SEG[k][2], first, (lit ? ju_n[k] : ju_cast[k]) * SEG[k][2]);
    if (c->pass == S3_LIT) {
        /* Four broad leaves shaken from the right canopy by the growl.
         * They enter from above and fall below the floor before this ends. */
        if (c->j > 58.9f && c->j < 63.0f) ju_strips(4.0f, 6, 2, 0, 8);
        ju_strips(0.0f, 4, 1, 0, JU_TUFTS * 16);
        /* The floor goes last, so whatever stands on it hides it early. It
         * receives shadows but casts none. */
        mc_shader_use(d->ju_ground);
        glUniform4fv(0, 48, ju_tr[0]);   /* uT */
        glDrawArrays(GL_TRIANGLES, 0, JU_RINGS * 160 * 6);
    }
    glEnable(GL_CULL_FACE);
}
