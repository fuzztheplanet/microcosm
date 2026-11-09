#version 450 core
// The blob's surface, with no real lighting. Under water (alpha 1, Scene 2) it
// starts out brighter where it faces you, with a hot fresnel rim at the edges,
// so it looks soft and glowing rather than solid. As the renderer grows up (uQ)
// it turns into jelly and you see the water behind through it. In Scene 1 it's
// a deeper, glossy blue, and on the end card it's a glossy deep blue shaded
// like the beach's. Paired with blob.vert (noise wobble).
in vec3 vWorld;
in vec3 vNormal;
in vec4 vColor;      // rgb = blue body colour, a = 1 for the jelly
in float vR;
out vec4 o;
layout(binding = 9) uniform sampler2D uScene;   // what lies behind it (the jelly's copy)

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
// Caustics on a lit surface, with a mean of 1. Each colour channel of the web
// is a little wider or narrower than the next (red is widest), because the
// ripples split the light like a prism. Where a pixel spans several lines the
// web is smoothed toward its mean, and it's softer in murky water.
vec3 cst(vec3 p){
    float f = clamp(1.25 - 2.75 * length(fwidth(p.xz)), 0.0, 1.0);
    if (f <= 0.0) return vec3(1.0);                 // (far off it would only shimmer)
    float m = cw((p.xz + uLightDir.xz / uLightDir.y * (S - p.y)) * 0.45, 3), q = m * m;
    return mix(vec3(1.0), min(vec3(q * 37.1, q * sqrt(m) * 79.7, q * m * 166.0), vec3(8.0)), f * (0.35 + 0.55 * uClarity));
}

void main(){
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCam - vWorld);
    float ndv  = max(dot(N, V), 0.0);
    float fres = pow(1.0 - ndv, 2.0);

    vec3 col = vColor.rgb * (0.35 + 0.55 * ndv);  // facing the camera = brighter
    col += uRim * fres * 1.3;                      // luminous edge
    col += vColor.rgb * 0.25;                       // keep the dark side from going flat black
    // In Scene 1 (alpha 0) it's a deeper, glossy blue, brightest facing us and
    // darker toward the edge to bring out its round body and lumps. The bright
    // window of sky above catches on its crown. A pale glow with a pale edge at
    // three-quarters opacity washed out to a matte lilac-blue on the title's
    // pale sky, unlike the end card's glossy blob.
    if (vColor.a == 0.0) col = vColor.rgb * (0.25 + 0.65 * ndv) + smoothstep(0.97, 0.985, dot(reflect(-V, N), vec3(0.0, 0.96, -0.28))) * 2.0;
    // The end card's blob (round, uWobble 0.15, and not jelly: alpha 0) is
    // shaded like the beach's in jo_blob.frag, so the card doesn't follow
    // Scene 3's glossy blue jelly with the title's pale, matte glow. Its soft
    // light is at a third and its pale edge at half. The violet sky behind it
    // shows through its blue body, which absorbs more red and green the
    // thicker it is. Fresnel mirrors the sky in its skin, and the window of
    // sky straight above makes a soft-edged glint on its crown.
    if (uWobble < 0.5 && vColor.a == 0.0) {
        vec3 R = reflect(-V, N);
        col = vColor.rgb * (0.35 + 0.55 * ndv) * 0.33 + uRim * fres * 0.66
            + uTop * 1.5 * exp((vColor.rgb - 1.0) * ndv * 2.5)
            + mix(uBottom, uTop * 4.0, smoothstep(-0.2, 0.8, R.y)) * (0.04 + 0.96 * pow(1.0 - ndv, 5.0))
            + smoothstep(0.97, 0.985, dot(R, vec3(0.0, 0.96, -0.28))) * 2.0;
    }
    // How much of it is jelly, eased in as a square. At half strength the dark
    // seabed seen through the lumps' lenses blotched the milky body like
    // bruises.
    float J = vColor.a * smoothstep(0.1, 0.5, uQ) * smoothstep(0.1, 0.5, uQ);
    vec3 cs = cst(vWorld);                         // outside the branch: it takes derivatives
    // The normal that bends its rays: half-way between the lumps' normal and
    // the view while it's lumpy (see below).
    vec3 B = normalize(mix(V, N, 1.0 - 0.5 * min(uWobble, 1.0)));
    // Under water (alpha 1) the depth it writes lies inside it: its centre's
    // once it's jelly, and half its radius in while it's milky. Using the
    // centre's depth for the milky blob smeared a ghost of its bright rim
    // inside its edge as the eye moved. The eyes are drawn after it, 0.9 of
    // its radius out, so they show through however its lumps lie.
    vec4 q = uViewProj * vec4(vWorld - N * vR * (J > 0.0 ? 1.0 : 0.5), 1.0);
    float z = vColor.a > 0.0 ? q.z / q.w * 0.5 + 0.5 : gl_FragCoord.z;
    if (J > 0.0) {
        // The jelly: a clear body you see the water and the seabed through.
        // Each colour's ray is bent into the ball, crosses it along a chord of
        // 2 R cos and is bent again on the way out (the exit normal is the
        // entry normal reflected about the ray). It's then read from the copy
        // of the picture behind, as a ball lens would show it: magnified, and
        // upright this close. The three colours' indices drift apart only
        // toward the edges, by the rim's fres, for a faint spectrum there.
        // Spread apart across its face, they would split each caustic line
        // the lumps magnify into red, green and blue contours like an oil
        // slick.
        // While it's lumpy (uWobble 1) the rays bend by a normal half-way
        // between its own and the view's, and once it has rounded out, by its
        // own. Each lump is a little lens, and at full strength they magnified
        // the seabed's caustic web into concentric rings inside it, like
        // thumbprints.
        // The blue body absorbs red and green along that chord (Beer-Lambert,
        // (1 - its colour) per diameter; at 0.75 it stayed nearly colourless).
        // Fresnel mirrors the bright surface overhead in its skin, with the
        // sun's glint, and the caustics ripple over its top.
        // While its face is turned to us (uGridFade: how much of its eyes
        // shows, see main.c), the middle of the ball as seen from here takes
        // no glint. That's within ~40-50 deg of us around its centre (uGlow),
        // where its eyes are. A glint parked there looked like a third eye or
        // a white nose, in the flight's glances too.
        vec3 bg, R = reflect(-V, N), L = normalize(uLightDir), T;
        for (int i = 0; i < 3; i++) {
            float e = 0.88 - 0.004 * float(i) * fres;
            T = refract(-V, B, e);
            q = uViewProj * vec4(vWorld - T * 2.0 * vR * dot(T, B) + refract(T, -reflect(B, T), 1.0 / e) * vR * 1.2, 1.0);
            bg[i] = texture(uScene, q.xy / q.w * 0.5 + 0.5)[i];
        }
        float F = 0.02 + 0.98 * pow(1.0 - ndv, 5.0);
        // A little more body colour separates the clear jelly from the cyan
        // water. A broader, dimmer sun glint stays steady as the skin moves.
        col = col * (1.0 - 0.9 * J) + J * (bg * exp((1.0 - vColor.rgb) * dot(T, N) * 1.2) * (1.0 - F)
            + mix(uFogColor * 0.5, uSkyColor * 2.6, smoothstep(-0.3, 0.9, R.y)) * F
            + pow(max(dot(R, L), 0.0), 300.0) * 4.0 * (1.0 - uGridFade * smoothstep(0.6, 0.8, dot(normalize(vWorld - uGlow), V))))
            + J * (cs - 1.0) * max(dot(N, L), 0.0) * vec3(0.06, 0.12, 0.14);   // the web over its top
    }
    gl_FragDepth = z;

    float dist = length(uCam - vWorld);
    float f = 1.0 - exp(-dist * uBlobFog);
    col = mix(col, uFogColor, f * 0.35);
    o = vec4(col, 1.0);
}
