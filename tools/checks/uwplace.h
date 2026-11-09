/* Validate the actual GPU placement output, independently of its population
 * recipe. Included after main.c by render.c; never part of the shipped demo. */
static mc_v3 placement_xyz(mc_v4 v) { return mc_vec3(v.x, v.y, v.z); }

static void placement_checks(Demo *d) {
    const int cells = 34 * 34 * 48;
    int kinds[5] = {0}, landmarks[3] = {0}, timed = 0;
    float path_gap = 1e9f, camera_gap = 1e9f;
    for (int i = 0; i < UW_NCAND; ++i) {
        const UwProp *p = &uw_in.o[i];
        int kind = (int)p->k.x;
        if (!kind || p->k.y >= 999) continue;
        assert(kind >= 1 && kind <= 4 && p->at.w > 0 && p->k.w >= 0);
        assert(fabsf(p->at.x) <= 120 - p->k.w + .001f && fabsf(p->at.z) <= 120 - p->k.w + .001f);
        ++kinds[kind];
        if (i >= cells) ++landmarks[(i - cells) / (6 * 96)];
        mc_v3 q = placement_xyz(p->at);
        float reach = p->k.w;
        if (kind == 4) { q.y += p->at.w * .5f; reach = p->at.w * .5f + .3f; }
        /* Crabs lie below the paths; the shader deliberately exempts them. */
        if (kind != 3) {
            for (int k = 0; k < UW_PATH_SAMP; ++k)
                path_gap = fminf(path_gap, mc_v3_dist(q, placement_xyz(uw_in.path[k])) - reach - 4.6f);
            for (int k = 0; k < UW_NSCHOOL - 1; ++k)
                assert(mc_v3_dist(q, placement_xyz(uw_in.school[k])) >= reach + uw_in.school[k].w + 2.9f - .002f);
            mc_v4 school = uw_in.school[UW_NSCHOOL - 1];
            float dx = q.x - school.x, dz = q.z - school.z;
            assert(sqrtf(dx * dx + dz * dz) >= reach + school.w - .002f || fabsf(q.y - school.y) >= reach + 3.5f - .002f);
        }
        if (kind == 1) {
            for (int k = 0; k < UW_CAM_SAMP; ++k)
                camera_gap = fminf(camera_gap, mc_v3_dist(q, placement_xyz(uw_in.cam[2 * k])) - reach - 2.5f);
            for (int k = 0; k <= PRED_N / 4; ++k)
                assert(mc_v3_dist(q, placement_xyz(uw_in.pred[k])) >= reach + 4.6f - .002f);
            /* Cell rocks must not intersect; the authored landmark outcrops
             * deliberately pack their rocks together and are excluded. */
            for (int k = 0; i < cells && k < i; ++k) {
                const UwProp *other = &uw_in.o[k];
                if (other->k.x == 1 && other->k.y < 999)
                    assert(mc_v3_dist(q, placement_xyz(other->at)) >= reach + other->k.w + .3f - .002f);
            }
        }
        if (i < cells && i % 48 > 10) assert(fabsf(p->at.z + 42) >= p->k.w + 7.8f - .002f);
        /* Late cell births must have the whole grow-in outside the camera's
         * view. Early births include the established already-grown and
         * fallback cases, and landmarks use a larger shared-site envelope. */
        if (i < cells && p->k.y >= 50) {
            int first = (int)lroundf(p->k.y * 4), count = kind == 1 ? 11 : 17;
            float e = kind == 4 ? p->at.w : p->k.w;
            assert(first >= 0 && first + count <= UW_CAM_SAMP);
            for (int k = first; k < first + count; ++k) {
                mc_v3 v = mc_v3_sub(placement_xyz(p->at), placement_xyz(uw_in.cam[2 * k]));
                float a = mc_v3_dot(v, placement_xyz(uw_in.cam[2 * k + 1]));
                assert(mc_v3_len(v) >= 70 - .002f || sqrtf(fmaxf(mc_v3_len2(v) - a * a, 0)) * .5f - a * .866f >= e + 4 - .002f);
            }
            ++timed;
        }
    }
    assert(path_gap >= -.002f && camera_gap >= -.002f && timed > 0);
    for (int k = 1; k <= 4; ++k) assert(kinds[k] > 0);
    for (int k = 0; k < 3; ++k) assert(landmarks[k] > 0);
    assert(kinds[1] + kinds[2] + kinds[3] + kinds[4] == d->uw_np);
    printf("Placement: %d rocks, %d coral, %d crabs, %d weeds; landmark members %d/%d/%d; "
           "%d late growth intervals clear; path/camera gaps %.4f/%.4f m\n",
           kinds[1], kinds[2], kinds[3], kinds[4], landmarks[0], landmarks[1], landmarks[2], timed, path_gap, camera_gap);
}
