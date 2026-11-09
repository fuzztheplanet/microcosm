#version 450 core
// Ground-truth ambient occlusion (GTAO, after Jimenez et al. 2016). For two
// slices through the view direction, find how high the depth buffer rises on
// either side within a metre. Then integrate the cosine-weighted sky this
// pixel can still see between those two horizons, around its own normal. Each
// frame rotates the slices and shifts the steps, so the TAA gathers many more.
// .g holds the contact shadows: a short march toward the sun through the
// depth buffer catches the small shadows the shadow maps are too coarse for
// (a foot on the sand, a bug on its leaf). They only run where the scene's
// alpha says some of the light is the sun's.
// This runs at half resolution. Each pixel reads one full-resolution depth
// texel at its centre and rebuilds its position from that same texel.
// Sampling at a texel boundary let whole rows flip to a neighbour, which drew
// lines on the floor. The normal comes from the texels on either side, taking
// on each axis the side whose depth is nearer the pixel's own. Derivatives
// across a 2 x 2 quad of half-resolution pixels spanned a silhouette in every
// other quad and put dark dashes down every trunk and liana and speckle
// through the foliage. At the screen's edge one side has no step at all, so
// the cross product is zero. Adding a trace of the position, too small to
// turn a real normal (0.5% at most, for a surface 4 mm away at 4K), makes it
// the eye ray instead. The normal is turned to face the eye ray (on ground
// seen edge-on, its z is about 0). The sky gets no occlusion.
in vec2 uv;
out vec4 o;
uniform sampler2D uDepth;
layout(binding = 1) uniform sampler2D scene;
// The post stack's parameters, std140 on binding 0. Once the scene is drawn,
// mc_renderer_end refills the frame's uniform buffer with them. Keep this block
// identical in every post shader; post_block in render.c mirrors it.
layout(std140) uniform Post {
    vec3  lens;  float zn;              // focus, blur at infinity, shafts mix; near plane
    vec2  uSpare, rcp, uRes;            // spare (0); scene texel; spare (0)
    float zf, exposure, bloomAmt, vignette, grain, chroma, saturation, fade, white, time,
          aoAmt, mblur, projx, projy, thresh,
          seed;                         // frames since the TAA history began (0: none)
    vec2  jit;                          // this frame's sub-pixel jitter (scene pixels)
    vec4  sunV;                         // toward the sun, view space; w: contact shadows on
    mat4  reproj, cvp;                  // last frame's view-projection; this one's
};
float lin(float d){
    d = d * 2.0 - 1.0;
    return 2.0 * zn * zf / (zf + zn - d * (zf - zn));
}
vec3 vpos(vec2 u){
    float z = lin(texture(uDepth, u).r);
    return vec3((u * 2.0 - 1.0) * vec2(projx, projy) * z, -z);
}
void main(){
    ivec2 i = ivec2(gl_FragCoord.xy) * 2;
    vec2 res = vec2(textureSize(uDepth, 0)), u = (vec2(i) + 0.5) / res;
    vec3 P = vpos(u), V = normalize(-P), e = vec3(1.0 / res, 0.0),
         a = vpos(u + e.xz) - P, b = P - vpos(u - e.xz),
         c = vpos(u + e.zy) - P, d = P - vpos(u - e.zy),
         N = normalize(cross(abs(a.z) < abs(b.z) ? a : b, abs(c.z) < abs(d.z) ? c : d) + P * 1e-12);
    if (dot(N, V) < 0.0) N = -N;
    o = vec4(1.0);
    if (texelFetch(uDepth, i, 0).r >= 1.0) return;
    // interleaved gradient noise, shifted by a golden-ratio step each frame
    float r = fract(52.9829 * fract(dot(gl_FragCoord.xy, vec2(0.06711, 0.00584))) + seed * 0.618),
          px = min(0.5 * res.y / (projy * -P.z), res.y * 0.15), vis = 0.0;   // a metre, in pixels here
    for (int k = 0; k < 2 && aoAmt > 0.0; k++) {
        float a = (r + float(k)) * 1.5708;
        vec3 D = vec3(cos(a), sin(a), 0.0), X = normalize(cross(D, V)), Np = N - X * dot(N, X);
        float L = length(Np), cn = clamp(dot(Np, V) / L, -1.0, 1.0), n = sign(dot(D - V * dot(D, V), Np)) * acos(cn), h[2];
        for (int sd = 0; sd < 2; sd++) {
            float lo = cos(n + (sd == 0 ? 1.5708 : -1.5708)), c = lo;
            for (int j = 0; j < 4; j++) {
                float t = (float(j) + fract(r * 7.37 + float(k) * 0.5)) / 4.0;
                vec3 w = vpos(u + D.xy * (sd == 0 ? 1.0 : -1.0) * max(t * t * px, 1.5) / res) - P;
                float l = length(w);
                c = max(c, mix(lo, dot(w, V) / l, clamp(2.5 - 2.5 * l, 0.0, 1.0)));
            }
            h[sd] = acos(clamp(c, -1.0, 1.0));
        }
        float h0 = n + max(-h[1] - n, -1.5708), h1 = n + min(h[0] - n, 1.5708);
        vis += L * 0.25 * (2.0 * cn + 2.0 * (h0 + h1) * sin(n) - cos(2.0 * h0 - n) - cos(2.0 * h1 - n));
    }
    if (aoAmt > 0.0) o.r = vis * 0.5;
    // Contact shadows: eight steps toward the sun over 4% of this pixel's
    // distance, starting just off a surface that faces the sun. A step that
    // ends up just behind what the depth buffer shows is in shadow. The window
    // is narrow because the depth buffer only knows front faces, and a ray
    // passing behind a whole trunk isn't in its shadow. A 24-bit depth
    // step at z is about z^2 / (near * 2^24). Past about 4000 near planes
    // that's more than the 0.1% a hit needs, and the far scene shadows itself
    // at random. With a 4 mm near plane, everything past a few metres speckled
    // black under the jitter, so the shadows fade out from 1000 to 2000 near
    // planes. A step off the screen ends the march, since there's nothing to
    // test there (the clamped edge depth shadowed the borders).
    if (sunV.w > 0.0 && dot(N, sunV.xyz) > 0.1 && texelFetch(scene, i, 0).a < 0.999 && -P.z < 2e3 * zn)
        for (int j = 0; j < 8; j++) {
            vec3 Q = P + (N * 0.004 + sunV.xyz * (float(j) + r) * 0.005) * -P.z;
            vec2 s = Q.xy / (-Q.z * vec2(projx, projy));
            if (abs(s.x) > 1.0 || abs(s.y) > 1.0) break;
            float e = -Q.z - lin(texture(uDepth, 0.5 + 0.5 * s).r);
            if (e > 0.001 * -Q.z && e < 0.012 * -Q.z) { o.g = smoothstep(1e3, 2e3, -P.z / zn); break; }
        }
}
