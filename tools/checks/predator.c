/* CPU regression for the fitted chase. The reference poses were sampled from
 * the original steering simulation with release flags and fpround --rel 16,
 * before replacing it. They are not samples of the fitted control table.
 * The dense checks use the same three body-core spheres as the old steering
 * code, and the actual rendered heading (the +/-0.1 s path samples).
 * Prop clearance is checked separately by render.c's placement fixture. */
#define main demo_main
#include "../../demos/hello-world/main.c"
#undef main
#include <assert.h>
#include <stdint.h>

static const struct { unsigned short tick; mc_v3 p, heading; } reference[] = {
    {    0, { -6.039347f, 2.689911f, 79.57043f }, { -.04228912f, -.08531569f, -.9954523f } },
    {  512, { -10.30274f, 1.373194f, 71.46316f }, { -.6722974f, -.1522904f, -.7244421f } },
    { 1024, { -19.26672f, -.249486f, 62.73027f }, { -.6989632f, -.09954289f, -.7081907f } },
    { 1408, { -25.81726f, -.9836755f, 54.85692f }, { -.5484383f, -.04075648f, -.8351926f } },
    { 1613, { -28.31882f, -1.099859f, 50.26034f }, { -.3896504f, -.001893397f, -.9209567f } },
    { 1792, { -28.51899f, -1.023663f, 44.58691f }, { .270758f, .0186757f, -.9624624f } },
    { 1958, { -23.17039f, -.8223076f, 34.35627f }, { .5750487f, .01407515f, -.8179934f } },
    { 2048, { -17.80508f, -.7142181f, 27.69319f }, { .666818f, .01174248f, -.7451229f } },
    { 2080, { -15.77498f, -.6790533f, 25.45907f }, { .6724576f, .01164813f, -.740039f } },
    { 2118, { -13.53929f, -.640327f, 22.99869f }, { .6746331f, .0116512f, -.7380561f } },
    { 2163, { -10.83078f, -.5951562f, 20.24365f }, { .8110054f, .03413296f, -.5840356f } },
    { 2189, { -8.488502f, -.4577236f, 18.94446f }, { .9145824f, -.008525202f, -.4043001f } },
    { 2253, { -3.149859f, -1.049008f, 18.31857f }, { .9679189f, -.14523f, .2050203f } },
    { 2342, { 2.576713f, -2.311317f, 22.81737f }, { .425274f, -.1693842f, .8890688f } },
    { 2445, { 1.881369f, -3.595997f, 31.14406f }, { -.5687639f, -.114705f, .8144585f } },
    { 2560, { -6.776004f, -4.232323f, 34.56721f }, { -.9789461f, -.01156836f, -.2037716f } },
    { 2688, { -13.9758f, -3.991768f, 26.99594f }, { -.3192186f, .02301488f, -.9473976f } },
    { 2816, { -14.92627f, -3.84281f, 16.48891f }, { .1209572f, .01586668f, -.992527f } },
    { 2944, { -12.7797f, -3.599504f, 8.944735f }, { .4025188f, .04928213f, -.9140799f } },
    { 3072, { -9.157674f, -3.103326f, 2.495925f }, { .5097778f, .09091218f, -.8554846f } },
    { 3200, { -8.058883f, -2.38479f, -2.920846f }, { -.08954164f, .1286108f, -.9876406f } },
    { 3456, { -13.91279f, -1.572815f, -11.65824f }, { -.8677291f, .003125102f, -.4970199f } },
    { 3840, { -28.4432f, -1.572815f, -19.47508f }, { -.8897883f, 0, -.4563651f } },
    { 4608, { -58.6969f, -1.572815f, -32.6025f }, { -.9370584f, 0, -.3491616f } },
    { 5376, { -89.48783f, -1.572815f, -44.46017f }, { -.9170355f, 0, -.3987962f } },
    { 6144, { -118.6538f, -1.572815f, -59.83023f }, { -.8476385f, 0, -.530567f } },
};

static mc_v3 heading(const Demo *d, float u) {
    return mc_v3_normalize(mc_v3_sub(predator_center(d, u + .1f), predator_center(d, u - .1f)));
}

/* The terrain mesh is the CPU reference already checked against uwplace.comp.
 * Sample its actual triangles, including the clamped edge of the seabed. */
static float floor_at(const mc_mesh *m, float x, float z) {
    float gx = mc_clampf((x + UW_TERR_SIZE * .5f) * UW_TERR_SUB / UW_TERR_SIZE, 0, UW_TERR_SUB - .001f);
    float gz = mc_clampf((z + UW_TERR_SIZE * .5f) * UW_TERR_SUB / UW_TERR_SIZE, 0, UW_TERR_SUB - .001f);
    int ix = (int)gx, iz = (int)gz, a = ix * (UW_TERR_SUB + 1) + iz;
    float fx = gx - ix, fz = gz - iz, ya = m->verts[a].pos.y, yc = m->verts[a + UW_TERR_SUB + 2].pos.y;
    if (fz >= fx) {
        float yb = m->verts[a + 1].pos.y;
        return UW_FLOOR_BASE + ya + (yb - ya) * fz + (yc - yb) * fx;
    }
    float yd = m->verts[a + UW_TERR_SUB + 1].pos.y;
    return UW_FLOOR_BASE + ya + (yd - ya) * fx + (yc - yd) * fz;
}

int main(void) {
    Demo *d = calloc(1, sizeof *d);
    assert(d);
    pred_build(d);
    float worst = 0, angle = 0;
    for (int i = 0; i < MC_ARRAY_COUNT(reference); ++i) {
        float u = ENC_IN + reference[i].tick * (1.0f / 256.0f);
        worst = fmaxf(worst, mc_v3_dist(predator_center(d, u), reference[i].p));
        angle = fmaxf(angle, acosf(mc_clampf(mc_v3_dot(heading(d, u), reference[i].heading), -1, 1)));
    }
    assert(worst < .09f && angle < .035f);   /* 9 cm / 2 degrees, including the lunge and turn */
    assert(mc_v3_dist(predator_center(d, ENC_PEAK), reference[7].p) < .01f);
    assert(mc_v3_dist(predator_center(d, ENC_IN - 10), predator_center(d, ENC_IN)) == 0);
    assert(mc_v3_dist(predator_center(d, ENC_OUT + 10), predator_center(d, ENC_OUT)) == 0);

    mc_mesh ground = mc_mesh_terrain(UW_TERR_SIZE, UW_TERR_SUB, UW_TERR_AMP, UW_TERR_SEED);
    float blob_gap = 1e9f, camera_gap = 1e9f, floor_gap = 1e9f, speed = 0;
    const float z[3] = { .95f, 0, -1.45f }, radius[3] = { .45f, .55f, .65f };
    for (int i = 0; i <= 24 * 256; ++i) {
        float u = ENC_IN + i * (1.0f / 256.0f);
        mc_v3 p = predator_center(d, u), f = heading(d, u), b = blob2_center(uw_t(u));
        mc_camera c = camera_uw(uw_t(u));
        for (int k = 0; k < 3; ++k) {
            uint32_t bits;
            memcpy(&bits, (&p.x) + k, sizeof bits);
            assert((bits & 0x7f800000u) != 0x7f800000u);   /* works with -ffast-math */
            mc_v3 centre = mc_v3_mad(p, f, z[k] * PRED_SCALE);
            float r = radius[k] * PRED_SCALE;
            blob_gap = fminf(blob_gap, mc_v3_dist(centre, b) - UW_BLOB_R * 1.2f - r);
            camera_gap = fminf(camera_gap, mc_v3_dist(centre, c.pos) - r);
            floor_gap = fminf(floor_gap, centre.y - r - floor_at(&ground, centre.x, centre.z));
        }
        if (i) speed = fmaxf(speed, mc_v3_dist(p, predator_center(d, u - 1.0f / 256.0f)) * 256);
    }
    assert(blob_gap > .28f && camera_gap > 5.1f && floor_gap > 5.0f && speed < 30.0f);
    printf("Predator: reference poses within %.4f m / %.3f degrees; 256 Hz core-sphere gaps "
           "blob %.4f m, camera %.4f m, seabed %.4f m; peak speed %.3f m/s\n",
           worst, angle * MC_RAD2DEG, blob_gap, camera_gap, floor_gap, speed);
    mc_mesh_free(&ground);
    free(d);
    return 0;
}
