#version 450 core
// Per-pixel shading for the swell. The swell's sines are summed again here,
// with their slopes, and five finer octaves of chop go on top. Each octave of
// chop fades out where it would alias, and its slope is counted as roughness
// instead. The foam follows the crests, streaked by the wind, rather than the
// grid's triangles. The colour is a depth-tinted body, the sky mirrored by
// Fresnel, and the sun's glint by GGX with that roughness, all in linear HDR
// so the glint blooms.
in vec3 vWorld;
out vec4 o;

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

float waveH(vec2 p, out vec2 slope){
    vec4 ws[4] = vec4[4](
        vec4( 1.0,  0.0, 0.16, 1.0),
        vec4( 0.6,  0.8, 0.28, 1.5),
        vec4(-0.7,  0.5, 0.52, 2.1),
        vec4( 0.2, -0.9, 0.95, 2.9));
    float h = 0.0, amp = 1.0;
    slope = vec2(0.0);
    for (int i = 0; i < 4; i++){
        vec2  d  = normalize(ws[i].xy);
        float f  = ws[i].z;
        float ph = dot(d, p) * f + uTime * ws[i].w;
        h     += amp * sin(ph);
        slope += amp * f * d * cos(ph);
        amp   *= 0.55;
    }
    return h;
}
layout(binding = 1) uniform sampler3D uNoise;   // 32^3 random bytes, wrapping (main.c's noise_make)
// Value noise, 0..1. The hardware blends the lattice; each cell's fraction
// goes through the cubic first.
float vn3(vec3 p){ vec3 i = floor(p), f = p - i; return texture(uNoise, (i + f * f * (3.0 - 2.0 * f) + 0.5) / 32.0).r; }
float vn(vec2 p){ return vn3(vec3(p, 0.0)); }    // in 2D: the volume's first layer

void main(){
    vec2 p = vWorld.xz, s, c;
    float h = waveH(p, s), w = length(fwidth(p)), r = 0.003, k;
    // The chop: five octaves, each rotated and faster, dropped as they shrink
    // below a pixel. The tilt a dropped octave would have given the surface
    // (its slope variance) goes into the roughness instead, the way LEAN and
    // Toksvig filter normal maps by footprint. r is the mean squared slope
    // the pixel can't hold, on top of the capillaries' own, and the cat's-paw
    // below adds to it too.
    for (int i = 1; i < 6; i++){
        float f = 0.95 * pow(1.9, float(i)), ph = dot(vec2(cos(float(i) * 2.4), sin(float(i) * 2.4)), p) * f
                + uTime * sqrt(f * 9.8) * 0.5;
        vec2 g = 0.1 * pow(0.6, float(i)) * f * vec2(cos(float(i) * 2.4), sin(float(i) * 2.4));
        k = smoothstep(0.3, 1.2, f * w);
        s += g * cos(ph) * (1.0 - k);
        r += dot(g, g) * 0.5 * k;
    }
    // A cat's-paw of noise, so no lattice shows. It's gone by the time its
    // 0.6 m cells shrink to about a pixel. Kept down to half a pixel, it
    // looked like grains of salt over the mid-distance.
    c = p * 1.7 + uTime * vec2(0.3, -0.5);
    k = smoothstep(0.25, 0.8, 1.7 * w);
    s += (vec2(vn(c), vn(c.yx + 7.0)) - 0.5) * 0.3 * (1.0 - k);
    r += 0.008 * k;
    vec3 N = normalize(vec3(-s.x * 0.8, 1.0, -s.y * 0.8)), V = normalize(uCam - vWorld), L = normalize(uSun), H = normalize(L + V);
    float nv = max(dot(N, V), 0.01), F = 0.02 + 0.98 * pow(1.0 - nv, 5.0);

    // The body is deep blue in the troughs, with the thin crests lit through
    // to turquoise (more so looking toward the sun). The sky it mirrors goes
    // over that by Fresnel (Schlick, 2% for water): pale at the horizon and
    // deeper blue overhead, with the glow round the sun. It's the same sky
    // space.frag draws over the sea, so the two meet at the horizon.
    vec3 col = mix(vec3(0.006, 0.045, 0.1), vec3(0.02, 0.2, 0.27) * (1.0 + 2.0 * pow(max(dot(-V, L), 0.0), 4.0)), clamp(0.4 + 0.4 * h, 0.0, 1.0));
    vec3 R = reflect(-V, N);
    col = mix(col, mix(uSky, vec3(0.2, 0.4, 0.85), sqrt(clamp(R.y * 3.0, 0.0, 1.0)))
                 + vec3(1.0, 0.95, 0.8) * (1.0 - smoothstep(0.0, 0.4, length(R - L))) * 0.5, F);

    // The sun's glint: GGX (D F / 4 N.V, times N.L), with Fresnel at the half
    // vector. Its width is the surface's own roughness plus every slope the
    // pixel averages over (r), so it's a sharp sparkle up close and a smooth
    // glitter path far off.
    float a = r * 0.64, d = pow(max(dot(N, H), 0.0), 2.0) * (a - 1.0) + 1.0;
    col += vec3(1.0, 0.96, 0.85) * a / (12.566 * d * d * nv) * (0.02 + 0.98 * pow(1.0 - max(dot(H, V), 0.0), 5.0)) * max(dot(N, L), 0.0) * 1.5;

    // Foam on the breaking crests, torn into streaks along the wind. A third,
    // finer octave frays its edges into lace and thins it inside, and evens
    // out where the streaks shrink below a pixel. That gives broken-up
    // patches a metre or two across rather than one 5 m blob per crest. Foam
    // is bubbles, so a fourth octave, round and about 15 cm across, drifts
    // with the foam and packs or thins them: brighter where they're packed,
    // with the water showing through where they're thin. The slope the foam
    // lies on turns it toward the sun or away, because flat white foam looked
    // like paint laid on the water.
    c = p * vec2(0.35, 1.2) + uTime * vec2(0.6, 0.0);
    float fo = mix(vn(c) * 0.45 + vn(c * 3.1) * 0.3 + vn(c * 9.7) * 0.25, 0.5, smoothstep(0.7, 2.5, w));
    c *= vec2(20.0, 5.8);
    float fb = mix(vn(c) + vn(mat2(0.8, 0.6, -0.6, 0.8) * c + 3.7), 1.0, smoothstep(0.1, 0.4, w * 7.0));
    col = mix(col, vec3(0.9, 0.95, 1.0) * (0.35 + 0.35 * fb) * (0.5 + dot(N, L)), smoothstep(1.4, 1.7, h + 0.6 * fo) * smoothstep(0.45, 0.75, fo + 0.2 * fb - 0.2));
    // the far sea fades into the horizon haze before the plane's edge shows
    o = vec4(mix(col, uSky, smoothstep(200.0, 440.0, length(vWorld - uCam))), 1.0);
}
