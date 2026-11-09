#version 450 core
// The underwater backdrop, drawn behind everything (far.vert puts it on the
// far plane). It shows the water along each pixel's ray. The light the water
// scatters in gets brighter toward the surface and toward the sun, and the
// shafts the ripples cast down cross it (a broad version of the caustic web,
// sampled along the ray). Looking up, the ray meets the surface from below.
// Inside Snell's window the whole sky and the sun come through, squeezed into
// a disc 97 deg across with a thin rainbow at its rim, and outside it total
// internal reflection mirrors the dark water. Small ripples break both into
// moving facets, and specks drift up. uBright lets the demo fade the
// surface's light (splash flash, final fade-out).
in vec4 vH;
out vec4 o;
layout(location = 0) uniform vec4 uP;      // the eye's steadied place; 1 still .. 0 moving fast (main.c)

// Per-frame uniforms shared by every scene shader: std140 on binding 0, kept
// identical in every file and mirrored by FrameUniforms in main.c.
layout(std140) uniform Frame {
    mat4  uViewProj, uLightVP;          // camera (the sun's in the shadow pass); sun
    vec3  uCam;         float uTime;
    vec3  uLightDir;    float uReal;    // toward the key light; realism ramp
    vec3  uFogColor;    float uClarity;
    vec3  uSkyColor;    float uQ;       // ambient from above; renderer-evolution ramp
    vec3  uGroundColor; float uFog;     // ambient from below; fog density
    vec3  uRim;         float uWobble;  // the blob's rim colour and lumpiness
    vec3  uSun;         float uBlobFog; // toward the sun; the blob's fog density
    vec3  uTop;         float uGape;    // sky top colour; predator jaws
    vec3  uBottom;      float uGlowAmt; // sky bottom colour; glow intensity
    vec3  uGlow;        float uGridFade;
    vec3  uGridColor;   float uAtmo;    // floor grid; 0 = deep space, 1 = blue sky
    vec3  uSky;         float uBright;  // sky the sea reflects; underwater backdrop level
    vec2  uRes, uSunUV;                 // render size; the space sun's uv position
    float uTexScale, uAlgaeH;
};

// ---- the water (identical in every underwater shader) ----
// The sea's surface is S m up, and the camera stays under it. The water
// absorbs red first and then green, like Jerlov's oceanic water types. wx() is
// its extinction per metre, murky early on and clear later. The sunlight left
// at a given depth falls off at half that rate and is 1 at the blob's depth,
// 13 m down. uFogColor arrives already scaled to the eye's depth.
// cw() is the caustic web, the light the surface's ripples focus: the squared
// distance to the nearest of n rotated, drifting lattices, at q on the surface
// (scaled). It's read where the sun's ray left the surface, so the caustics on
// the seabed and the shafts in the water are one field.
// wp() is how much light the water scatters along a ray d. Half of it goes
// evenly and half forward, by Schlick's Henyey-Greenstein with g 0.4, which
// gives 1 looking across the light, about 3.4 toward the sun and 0.7 away.
const float S = 8.5;
const mat3 CM = mat3(-2.0,-1.0,2.0, 3.0,-2.0,1.0, 1.0,2.0,2.0);
vec3 wx(){ return mix(vec3(0.18,0.07,0.05), vec3(0.06,0.018,0.013), uClarity); }
float cw(vec2 q, int n){
    vec3 a = vec3(q, uTime * 0.3) * CM * 0.5, f;
    float m = 1.0;
    for (int i = 0; i < n; i++, a *= CM * (0.4 - 0.1 * float(i))) { f = 0.5 - fract(a); m = min(m, dot(f, f)); }
    return m;
}
float wp(vec3 d){ float k = 1.0 - 0.585 * dot(d, uLightDir); return 0.5 + 0.5 / (k * k); }
// ---- end water ----

float hash21(vec2 p){ p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }

void main(){
    vec3 d = normalize(vH.xyz / vH.w - uCam), c = wx(), e = c - 0.5 * c * d.y;
    float s = (S - uCam.y) / max(d.y, 1e-3), ph = wp(d);    // to the surface (far beyond it looking down)
    vec3 col = uFogColor * ph * c / e * (1.0 - exp(-e * s));  // in-scatter, as the surfaces' waterFog

    // The shafts are the caustic web's light carried down from the surface in
    // sheets. They're extruded along a ray at 1.2 times the sun's slant
    // (29 deg), so the beams fan down from above the frame. That's a lower sun
    // than the lighting's, because shafts show best when the eye looks along
    // them. They sway with the swell (its offset at the surface and 12 m
    // down, carried on linearly), stay sharp near the surface and blur with
    // depth, and get bolder toward the sun. The samples crowd the near metres,
    // where the eye sees the shafts best, and the nearest are faded because
    // they swept past the lens. There's no dither, since the TAA's jitter
    // would turn it into shimmer.
    // Where a sample's beam leaves the surface is linear along the ray,
    // q0 + t dq, taken from the eye's steadied place (uP.xyz) so the camera's
    // tremble doesn't shake them. The faster the eye goes (uP.w falls), the
    // more they give way to the same light four times broader and softer.
    // Sharp sheets strobed when the camera flew or swung through them at
    // speed, and the broad ones just drift by. They're kept gentle, at 28% of
    // the swing in light they once had.
    vec2 sl = 1.2 * uLightDir.xz / uLightDir.y, s0 = vec2(sin(uTime * 0.5 + 1.3), cos(uTime * 0.37 + 0.85)),
         s1 = vec2(sin(uTime * 0.5 - 0.5), cos(uTime * 0.37 - 0.35)),
         q0 = (uP.xz + sl * (S - uP.y) + mix(s0, s1, (S - uP.y) / 12.0)) * 0.12,
         dq = (d.xz - (sl + (s1 - s0) / 12.0) * d.y) * 0.12;
    float l = min(s, 20.0), sh = 0.0, sb = 0.0, ws = 0.0;
    for (int i = 0; i < 6; i++) {
        float u = (float(i) + 0.5) / 6.0, t = l * u * u, w = u / (1.0 + 20.0 * u * u * (0.2 + 0.4 * u * u)),
              m = cw(q0 + dq * t, 2), b = cw((q0 + dq * t) * 0.25, 2), k = clamp(1.0 - (S - uP.y - d.y * t) * 0.025, 0.4, 1.0);
        sh += mix(1.0, m * m * m * 91.4, k * min(t * 0.5, 1.0)) * w; ws += w;
        sb += mix(1.0, b * sqrt(b) * 12.06, k) * w;
    }
    col *= clamp(1.0 + ((sh / ws - 1.0) * uP.w + (sb / ws - 1.0) * (1.0 - uP.w) * 0.7) * (0.224 + 0.098 * ph) * (0.7 + 0.6 * uClarity), 0.3, 3.5);

    // The surface from below: many small ripples tilt it into facets, and
    // their curvature focuses the light through them into a moving web. Each
    // ripple fades out before it gets too small for a pixel, so nothing
    // shimmers, and far off the finer ones stop early.
    vec2 q = (uCam + d * s).xz, g = vec2(0.0);
    float fp = 0.0017 * s / max(d.y, 0.1), cv = 0.0;           // a pixel's footprint there; curvature
    for (int i = 0; i < 8; i++) {                              // 1.3 .. 19 rad/m, golden-angle headings
        vec2 r = vec2(cos(float(i) * 2.39996), sin(float(i) * 2.39996));
        float f = 1.3 * pow(1.47, float(i)), h = 0.045 / sqrt(f) * clamp(1.2 / (f * fp) - 1.0, 0.0, 1.0),
              b = dot(r, q) * f - uTime * sqrt(9.8 * f) + float(i) * 1.7;
        if (h <= 0.0) break;
        g += r * cos(b) * h;
        cv += f * sin(b) * h;
    }
    vec3 n = normalize(vec3(g.x, -1.0, g.y));
    float ci = -dot(d, n), fw = min(fwidth(ci), 0.04) + 0.002 + 0.03 * (1.0 - uP.w);   // (a soft rim while the eye swings)
    // Inside Snell's window the sky and the sun come through, the radiance n^2
    // brighter. Each colour's critical angle is a little different, which
    // puts a thin rainbow at the rim (dispersion, slightly exaggerated).
    // Outside the window, total internal reflection mirrors the water below,
    // which looks down and away from the light, so it's dark. This only
    // applies where the ray reaches the surface through the water.
    if (s < 300.0) {
        vec3 r = reflect(d, n), ref = uFogColor * exp(0.5 * c * (S - uCam.y)) * wp(r) / (1.0 - 0.5 * r.y),
             lt = ref * clamp(1.0 + 0.2 * cv * uP.w, 0.6, 1.6);
        if (ci > 0.61) {                                       // at the window or in it
            vec3 sun = refract(uLightDir, vec3(0.0, -1.0, 0.0), 1.333), a = refract(d, n, 1.333);
            float ct = sqrt(max(1.0 - 1.777 * (1.0 - ci * ci), 0.0)), fr = 0.02 + 0.98 * pow(1.0 - ct, 5.0),
                  sd = max(dot(a, sun), 0.0);
            vec3 sky = mix(vec3(0.85, 0.92, 1.0), vec3(0.12, 0.35, 0.8), sqrt(max(a.y, 0.0)))
                     + vec3(1.0, 0.95, 0.85) * (30.0 * uP.w * pow(sd, 2000.0) + 1.2 * pow(sd, 50.0) + 0.25 * pow(sd, 5.0));
            lt = mix(lt, sky * clamp(1.0 + 0.5 * cv * uP.w, 0.3, 2.5) * (1.0 - fr) * 1.78 + ref * fr,
                     smoothstep(vec3(0.655, 0.6619, 0.672) - fw, vec3(0.655, 0.6619, 0.672) + fw, vec3(ci)));
        }
        col += lt * exp(-c * s) * uBright;
    }

    // Suspended particles drifting up, fixed to the view's directions. When
    // the eye swings fast they'd sweep across the frame in jumps, so they fade
    // out. Each speck has its own size, and the larger ones are dimmer, as if
    // nearer and out of focus.
    vec2 pp = vec2(atan(d.x, d.z) * 60.0, d.y * 60.0 - uTime * 0.6);
    float h = hash21(floor(pp)) * 12.0 - 11.5;
    if (h > 0.0)
        col += vec3(0.5, 0.7, 0.7) * (1.0 - smoothstep(0.0, h, length(fract(pp) - 0.5))) * 0.03 / h * uP.w;

    o = vec4(col, 1.0);
}
