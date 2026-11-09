#version 450 core
// The jungle's trees, built entirely here from gl_VertexID / gl_InstanceID,
// with no vertex buffers. Each draw covers one part, as a grid of quads
// (6 vertices each) for every piece, with several pieces per tree. The parts:
//   0 trunks (lobed, flaring giants, swollen palm feet)  1 buttress planks
//   2 limbs  3 lianas and strangler roots  4 crown leaf cards  5 palm fronds
//   6 epiphyte leaves (bromeliads, ferns) on the giants  7 curtains of hanging
//   leaves  8 the nearest crowns' leaves, modelled  9 the far forest:
//   billboards of these trees (and a wall of haze behind them)
// Lianas, epiphytes and planks drop out with distance, and far crowns swap
// half their cards for bigger ones, but only in the camera's passes. The
// sun's map is rendered once, with time frozen, and always gets the whole
// forest. The distance used is the nearest the tree is ever seen from
// (scene3.c's s3_tree_lod) rather than the current one, so a tree keeps one
// level of detail for the whole scene and nothing pops.
// Each surface comes from its own function, which also sets the normal (NM).
// uT holds the visible trees (s3_trees.c) as (x, y, z, trunk radius) and
// (height, kind, site index . gust, LOD distance). Kind 0 is a giant, 1 a
// palm, 2 a slender tree.
// ---- s3 common: keep byte-identical in every jungle shader (owner: s3_light) ----
// Per-frame uniforms shared by every scene shader. The block is std140 on
// binding 0, has to stay identical in every file, and mirrors FrameUniforms in
// main.c. The top of scene3.c lists what each field means in scene 3.
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
// The sun's depth maps, all seen through uLightVP. One covers the clearing
// and one the beach (uv = the clearing map's uv * uRim.x + uRim.yz). The
// third, redrawn every frame, covers whichever character is on show: the
// ladybug, the tyrannosaurus or the blob (uv = the clearing map's uv * uSun.x
// + uSun.yz). With one sun and one depth range, a point has the same depth in
// all three. The hardware compares depths with 2 x 2 filtering. The first two
// maps also have plain samplers for their raw depths, which feed the blocker
// search.
layout(binding = 4) uniform sampler2DShadow uShadow;
layout(binding = 5) uniform sampler2DShadow uShadow2;
layout(binding = 6) uniform sampler2DShadow uShadow3;
layout(binding = 7) uniform sampler2D uSunZ;
layout(binding = 8) uniform sampler2D uSunZ2;
// The character map's colour: 1 where the surface nearest the sun is the
// blob's jelly or its eyes, 0.5 for its parasol's cloth, 0 where it's opaque
// (the rest of what the blob carries).
layout(binding = 3) uniform sampler2D uSunT;
// The open sky's radiance by direction, drawn each frame by jl_atmo.frag into
// a 64 x 32 table. Its x is the angle round from the sun's bearing, and its y
// is the square root of the direction's height above the horizon, which gives
// finer steps near the horizon, where the haze changes fastest.
layout(binding = 15) uniform sampler2D uAir;

// The floor's height, mirroring s3_ground() in scene3.c. Inside the forest
// it's the rolling forest floor. Past the forest's edge (32 m out toward the
// sea, along s3sea) it's the beach, which slopes down through the waterline
// (50 m out, sea level -1) and on under the sea.
const vec2 s3sea = vec2(0.8660254, -0.5);
// The time the jungle opens, in scene-3 seconds (uTime), after one bar of
// white. It matches S3_J0 in scene3.c. The cues and the music count jungle
// seconds, j = uTime - s3j0.
const float s3j0 = 1.9354839;
float s3h(vec2 p){
    float s = dot(p, s3sea);
    return mix(0.30 * sin(0.11 * p.x + 0.7) * sin(0.13 * p.y + 0.3)
             + 0.12 * sin(0.31 * p.x - 0.23 * p.y)
             + 0.05 * sin(0.83 * p.y + 0.57 * p.x + 1.3),
               (50.0 - s) * 0.06 - 1.0, smoothstep(30.0, 36.0, s));
}
// s3sm() reads the raw depth at the clearing map's uv u, from the clearing's
// map, or from the beach's when k = 1. s3pcf() gives the share of u that's
// lit at depth z, filtered over 2 x 2 texels.
float s3sm(vec2 u, float k){
    return k > 0.5 ? texture(uSunZ2, u * uRim.x + uRim.yz).r : texture(uSunZ, u).r;
}
float s3pcf(vec2 u, float z, float k){
    return k > 0.5 ? texture(uShadow2, vec3(u * uRim.x + uRim.yz, z)) : texture(uShadow, vec3(u, z));
}
// Sunlight reaching P, from 0 (in shadow) to 1. The filter widens with the
// distance to the blocker, like a real penumbra from the sun's disc, so the
// canopy far overhead throws soft dapples and a leaf close by a sharp shadow.
// Where the clearing's map runs out, the beach's takes over. Beyond both, the
// forest canopy is taken to let a fifth of the sun through, and the open
// beach and sea get all of it.
// The character map is much finer (its offsets and bias are scaled to match)
// and adds the character's own shadow. The jelly blob lets uBlobFog of the
// sun through. Its shadow's edge blurs more the farther the point lies past
// the blob's middle along the sun's rays (uGape is that depth), because the
// light the jelly bends spreads out. The blur is a single tap, rotated by
// the same hash, which the TAA averages into a soft edge. The ladybug and
// the tyrannosaurus (uBlobFog 0) cast opaque shadows, and so does what the
// blob carries (uSunT 0), except its parasol's cloth (0.5), which lets 0.15
// of the sun through.
float s3shadow(vec3 P, vec3 N){
    vec3 s = (uLightVP * vec4(P + N * 0.03, 1.0)).xyz * 0.5 + 0.5;
    float e = 1.0 - smoothstep(0.45, 0.5, max(abs(s.x - 0.5), abs(s.y - 0.5))), b = 0.0, n = 0.0, r = 0.0, k = 0.0, h = 1.0;
    vec2 u = s.xy * uRim.x + uRim.yz;
    if (e < 1.0 && max(abs(u.x - 0.5), abs(u.y - 0.5)) < 0.49) { k = 1.0; e = 1.0; }
    float a = fract(sin(dot(P, vec3(12.9898, 78.233, 37.719))) * 43758.5) * 6.2832;
    for (int i = 0; i < 4; i++) {                      // blockers within ~25 cm
        float z = s3sm(s.xy + 0.003 * vec2(cos(a + float(i) * 1.5708), sin(a + float(i) * 1.5708)), k);
        if (z < s.z - 0.0002) { b += z; n += 1.0; }
    }
    if (n > 0.0) {
        float w = clamp((s.z - b / n) * 0.025, 0.0003, 0.004);   // penumbra (uv)
        for (int i = 0; i < 4; i++) {
            float q = a + float(i) * 2.39996, d = w * sqrt((float(i) + 0.5) / 4.0);
            r += s3pcf(s.xy + d * vec2(cos(q), sin(q)), s.z - 0.0002, k);
        }
        r *= 0.25;
    } else r = 1.0;
    s = (uLightVP * vec4(P + N * (0.1 / uSun.x), 1.0)).xyz * 0.5 + 0.5;
    u = s.xy * uSun.x + uSun.yz;
    if (max(abs(u.x - 0.5), abs(u.y - 0.5)) < 0.5) {
        u += max(s.z - uGape, 0.0) * 10.0 * uBlobFog * vec2(cos(a), sin(a));
        h = mix(texture(uShadow3, vec3(u, s.z - 0.0002 / uSun.x)), 1.0, uBlobFog * texture(uSunT, u).r);
    }
    return mix(mix(0.2, 1.0, smoothstep(33.0, 37.0, dot(P.xz, s3sea))), r, e) * h;
}
// s3lit() returns the lit colour of a surface. The sun gives rough diffuse, a
// GGX highlight with Smith's height-correlated visibility, and the light that
// comes through thin leaves from behind (yellower than their front), all
// times the shadow. The ambient added on top is sky and bounce light,
// skylight through leaves, and the sheen of what the surface mirrors. Under
// the canopy, what it mirrors is dark trunks around the horizon with lit
// leaves only high overhead, and the sheen is held back where the surface's
// own occlusion hides it. Past the forest's edge the ambient comes from the
// open sky, cool and bright, and the bounce from the sunlit sand.
// rough is 0..1, trans goes from 0 (opaque) to 1 (leaf), and ao is 0..1. v
// widens the highlight by how fast N turns across the pixel (specular
// anti-aliasing). The s3light() macro supplies v from N's screen-space
// derivatives, and it's a macro so that vertex shaders, which have no
// derivatives, never expand it.
// Energy is conserved. The highlight gets back what single scattering loses
// on rough surfaces, using Turquin's compensation 1 + F0 (1 / E - 1) with
// E ~ 1 - 0.55 rough from Karis' fit. For F0 0.04 that's ~ 1 + 0.04 rough^2.
// The diffuse gets whatever the highlight leaves.
// Afterwards s3amb holds the ambient part's sum, weighted for the
// screen-space occlusion, and s3sh the share of the sun that reaches the
// surface. s3fog() turns s3amb into s3a, the ambient's share of the final
// colour: the part the screen-space ambient occlusion may darken, which goes
// out as the scene's alpha.
float s3amb = 0.0, s3a = 0.0, s3sh = 1.0;
// Ambient light c on a surface of albedo alb under its own occlusion ao. Light
// that bounces between the surface and whatever occludes it comes back tinted
// by the surface (Jimenez's multi-bounce fit, per channel), so bright sand and
// lichen don't go as grey in their hollows. The screen-space occlusion knows
// nothing of albedo, so the share left for it to darken (s3amb) is lightened
// the same way: the fit's darkening shrinks by about x (0.6 + 0.4 x), where x
// is the albedo's luminance.
vec3 s3am(vec3 c, vec3 alb, float ao){
    float r = dot(alb, vec3(0.2126, 0.7152, 0.0722));
    c *= max(vec3(ao), ((ao * (2.0404 * alb - 0.3324) - 4.7951 * alb + 0.6417) * ao + 2.7552 * alb + 0.6903) * ao);
    s3amb = (c.r + c.g + c.b) * (1.0 - r * (0.6 + 0.4 * r));
    return c;
}
// Rough diffuse, using the energy-preserving Oren-Nayar model (EON, Portsmouth
// et al. 2024). It returns what a surface of albedo alb and roughness r sends
// toward the eye per unit of sunlight. l and nv are the cosines in and out,
// and s = L.V - l nv. The result is flatter than Lambert: brighter with the
// sun behind the eye and darker looking toward it. It also includes the light
// that scatters more than once, with a share of about 0.3 r alb l nv (the
// paper's multiple-scattering lobe, to first order in the albedo). The
// roughest surfaces in the scene (sand, moss, bark) use r ~ 0.45, because
// going higher would dim the sun's flecks on the trunks, which we see looking
// up toward the sun.
vec3 s3on(vec3 alb, float r, float l, float nv, float s){
    return alb * (1.0 + r * s / (s > 0.0 ? max(l, nv) : 1.0) + 0.3 * r * alb * l * nv) / (1.0 + 0.2878 * r);
}
vec3 s3lit(vec3 P, vec3 N, vec3 alb, float rough, float trans, float ao, float v){
    // Read the sun's map a little off the surface, on the sun's side. N faces
    // the eye, so a leaf or a wing lit from behind would read its own shadow.
    s3sh = s3shadow(P, N * sign(dot(N, uLightDir)));   // first, so little else is live across its fetches
    vec3 V = normalize(uCam - P), L = uLightDir, H = normalize(L + V);
    float nl = dot(N, L), l = max(nl, 0.0), nv = max(abs(dot(N, V)), 0.001), a = rough * rough, q = min(a * a + v, 1.0),
          nh = max(dot(N, H), 0.0), o = smoothstep(31.0, 37.0, dot(P.xz, s3sea)), y = reflect(-V, N).y;
    float D = q / (3.14159 * pow(nh * nh * (q - 1.0) + 1.0, 2.0)),
          F = 0.04 + 0.96 * pow(1.0 - max(dot(H, V), 0.0), 5.0),
          G = 0.5 / (l * sqrt(nv * nv * (1.0 - q) + q) + nv * sqrt(l * l * (1.0 - q) + q) + 0.0001),
          w = 1.0 - rough, z = min(w * w, exp2(-9.28 * nv)) * w + 0.0425 - 0.0275 * rough,     // the sheen: Karis' fit of
          E = (0.04 * (1.04 - 1.04 * z - 0.572 * rough) + 1.04 * z + 0.022 * rough - 0.04)     // the pre-integrated GGX,
            * clamp(pow(nv + ao, exp2(-16.0 * a - 1.0)) - 1.0 + ao, 0.0, 1.0),                // occluded (Lagarde)
          M = 1.0 + 0.04 * rough * rough;                                                      // multiple scattering
    vec3 sk = mix(uSkyColor, (uTop + uBottom * 1.6) * 0.22, o), gr = mix(uGroundColor, uGlow * 0.05, o),
         c = s3am(alb * (mix(gr, sk, N.y * 0.5 + 0.5) + trans * sk * (0.5 - 0.5 * N.y)) * (1.0 - E), alb, ao),
         e = E * M * mix(gr, sk, smoothstep(0.2 - 0.5 * o, 0.9 - 0.4 * o, y));
    s3amb += e.r + e.g + e.b;
    return uGlow * s3sh * ((s3on(alb, 0.5 * rough, l, nv, dot(L, V) - l * nv) * (1.0 - F) + D * G * F * M) * l
                + alb * vec3(1.5, 1.15, 0.45) * trans * max(-nl, 0.0) * (0.5 + 2.0 * pow(max(dot(-V, L), 0.0), 4.0)))
         + c + e;
}
#define s3light(P, N, alb, r, t, ao) s3lit(P, N, alb, r, t, ao, min(0.5 * (dot(dFdx(N), dFdx(N)) + dot(dFdy(N), dFdy(N))), 0.18))
// s3fog() adds the air between the eye and P. The part of the ray out over
// the beach and the sea crosses thin, bright sea air that fades into the
// horizon. It scatters the sky's own light, so the haze that way takes the
// sky's colour (aerial perspective, warmer and brighter toward the sun). The
// part inside the forest (short of its edge, 32 m toward the sea) crosses the
// humid haze, thicker near the ground. The canopy's green light falls on it
// from above, so it's brighter looking up. It glows gold toward the sun,
// whose light it scatters forward like any backlit air, and is a deep
// teal-green away from it. The light shafts add the sun's share in the near
// air. s3fog() also sets s3a.
// s3sky() is the open sky toward d (unit length), and the horizon's colour
// below the horizon. It has no sun disc, which jl_sky.frag adds. It's also
// what the sea and the wet sand reflect, and the light the sea air scatters
// toward the eye in s3fog().
vec3 s3sky(vec3 d){
    return textureLod(uAir, vec2(acos(clamp(dot(d.xz, normalize(uLightDir.xz)) / max(length(d.xz), 0.0001), -1.0, 1.0)) / 3.14159,
                               sqrt(max(d.y, 0.0))) * vec2(0.984375, 0.96875) + vec2(0.0078125, 0.015625), 0.0).rgb;
}
vec3 s3fog(vec3 c, vec3 P){
    vec3 v = P - uCam;
    float d = length(v), k = 0.12, e = exp(-k * uCam.y), a = dot(uCam.xz, s3sea), b = dot(P.xz, s3sea);
    float h = abs(v.y) > 0.01 ? (e - exp(-k * P.y)) / (k * v.y) : e;
    float f = clamp((32.0 - min(a, b)) / max(abs(b - a), 0.001), 0.0, 1.0);   // the forest's share of the ray
    float t = exp(-0.0008 * d * (1.0 - f)), g = exp(-uFog * d * (0.4 + h) * f);
    vec3 r = v / max(d, 0.001);
    vec3 hz = uFogColor * (0.6 + smoothstep(-0.1, 0.5, r.y)) + uGlow * 0.012 / pow(1.3 - dot(r, uLightDir), 2.0);
    c = mix(hz, t < 1.0 ? mix(s3sky(r), c, t) : c, g);
    s3a = clamp(s3amb * t * g / max(c.r + c.g + c.b, 0.0001), 0.0, 1.0);
    return c;
}
// ---- end s3 common ----
// The wind is a sea breeze blowing inland (-s3sea). Its gusts roll in as bent
// fronts about 30 m apart at 4 m/s, broken up across the wind into patches
// that drift along them. The canopy, the undergrowth and the hero plant share
// this one field, so you can see a gust travel through all of them. Returns
// the gust at p, l seconds ago, from 0 (calm) to 1 (gusty).
float gust(vec2 p, float l){
    float u = dot(p, s3sea), c = dot(p, vec2(0.5, 0.866)), t = uTime - l;
    return (0.35 + 0.65 * uWobble) * smoothstep(-0.4, 1.0, sin(0.2 * u + 0.8 * t + 1.5 * sin(0.06 * c + 0.2 * t)))
         * (0.55 + 0.45 * sin(0.13 * c + 0.05 * u + 0.5 * t));
}
layout(location = 2) uniform vec4 uT[640];
layout(location = 0) uniform vec4 uP;      // part, first tree, pieces per tree, grid columns + 128 x rows
layout(location = 1) uniform vec4 uGap;    // the sky window: direction from the viewer, cos of its radius
out vec3  vW;
out vec3  vN;
out vec2  vU;
out float vS;

const vec3 UP = vec3(0.0, 1.0, 0.0);

int   part, piece;
vec3  B, LA, LC, LB, NM, CT; // the tree's foot; the current limb or liana's curve; the normal; a card's tangent
float R, H, K, S, LR, SG, FAR, BP, CS; // trunk radius, height, kind, seed; the curve's radius, a liana's sag;
                                   // the tree's LOD distance (0 in the sun's pass); 1 for a palm on the beach;
                                   // a card's size
vec2  UV;                          // a modelled leaf's place in the baked leaf sheet, or a billboard's
float FV;                          // a billboard's tree (row) x 16 + the view it is seen from (eighths)

float hs(float n){ n = fract(n * 0.1031); n *= n + 33.33; return fract(n * (n + n)); }
vec4 h4(float n){ return vec4(hs(n), hs(n + 1.7), hs(n + 3.1), hs(n + 5.3)); }

// The trunk: its top (where the crown starts), centre line and radius.
float top(){ return K == 1.0 ? H : H * (K == 0.0 ? 0.68 : 0.85); }
vec3 tc(float s){
    float a = mix(S * 9.0, S - 1.02, BP);               // beach palms lean further, out to sea
    return B + vec3(cos(a), 0.0, sin(a)) * (K == 0.0 ? 0.3 * R * sin(s * 3.0 + S) : (0.1 + 0.18 * BP) * H * s * s)
             + vec3(sin(a), 0.0, -cos(a)) * (K - 1.0) * 0.07 * H * sin(s * 4.0 + S * 9.0)
             + UP * ((top() + 0.6) * s - 0.6);
}
float tr(float s){
    float m = s * top();
    return K == 0.0 ? R * (1.0 - 0.45 * s + 0.3 * exp(-m * 1.2))
         : K == 1.0 ? R * (1.0 - 0.3 * s + 0.6 * exp(-m * 2.0))
         : R * (1.0 - 0.7 * s);
}
vec3 bez(float t){ return mix(mix(LA, LC, t), mix(LC, LB, t), t); }

// Limb k: giants fork five main limbs off the top of the trunk, each with a
// second limb from its middle; slender trees grow tiers of near-level boughs.
void limb(int k){
    vec4 r = h4(S * 3.0 + float(k % 5) * 1.3);
    float az, el, L;
    if (K == 0.0) {
        az = S * 7.0 + float(k % 5) * 1.2566 + r.x * 0.5;
        el = 0.5 + 0.45 * r.y;
        L  = H * (0.22 + 0.1 * r.z);
        LA = tc(0.97);
        LR = R * 0.42;
    } else {
        float f = float(k) * 0.1, b = H < 12.0 ? 0.5 : 0.3;   // saplings branch from half way up
        az = S * 7.0 + float(k) * 2.4;
        el = 0.12 + 0.35 * r.y;
        L  = 1.2 + H * 0.2 * (1.0 - f) * (0.7 + 0.6 * r.z);
        LA = tc(b + (1.0 - b) * f);
        LR = tr(b + (1.0 - b) * f) * 0.6;
    }
    vec3 d = vec3(cos(az), 0.0, sin(az));
    LC = LA + d * L * 0.5 + UP * L * 0.15;
    LB = LA + (d * cos(el) + UP * (sin(el) - 0.15 * float(K))) * L;
    if (K == 0.0 && k > 4) {           // the second limb, off the first's middle
        LA = bez(0.55);
        az += (r.w - 0.5) * 2.0;
        d = vec3(cos(az), 0.0, sin(az));
        L *= 0.6;
        LC = LA + d * L * 0.5 + UP * L * 0.1;
        LB = LA + (d * 0.8 + UP * (0.3 + 0.5 * r.z)) * L;
        LR *= 0.5;
    }
}

// The centre line and radius of the tube parts at t (0..1).
vec3 ctr(float t){
    if (part == 0) return tc(t * sqrt(t));
    if (part == 3 && piece > 3) {      // strangler roots spiralling up the trunk
        float s = t * 0.9, a = LR * 40.0 + t * (7.0 + 9.0 * hs(LR));
        return tc(s) + vec3(cos(a), 0.0, sin(a)) * (tr(s) + LR);
    }
    vec3 p = bez(t);
    return part == 3 ? mix(LA, LB, t) - UP * SG * 4.0 * t * (1.0 - t)
                     + vec3(sin(t * 7.0 + S * 20.0), 0.0, cos(t * 5.0 + S * 30.0)) * SG * 0.25 * sin(t * 3.1416)
                     + vec3(sin(uTime * 0.8 + S + t * 3.0), 0.0, cos(uTime * 0.7 + S)) * 0.25 * uWobble * t
                     : p;
}
float rad(float t, float a){
    if (part == 0) {
        float s = t * sqrt(t);
        return tr(s) * (1.0 + (K == 0.0 ? 0.09 : 0.03) * sin(a * 5.0 + S + s * 7.0)
                            + 0.04 * sin(a * 11.0 + s * 23.0));
    }
    return part == 3 ? LR : LR * (1.0 - 0.75 * t);
}
vec3 tube(vec2 g){
    float a = -6.2832 * g.x, r = rad(g.y, a);
    vec3 p = ctr(g.y), d = normalize(ctr(g.y + 0.01) - p);
    vec3 n = normalize(cross(d, vec3(0.57, 0.62, 0.54))), m = cross(d, n), e = n * cos(a) + m * sin(a);
    // the normal follows the slope of the trunk's lobes and flutes round it
    NM = normalize(e + (n * sin(a) - m * cos(a)) * (rad(g.y, a + 0.01) - r) * 100.0 / r);
    return p + e * r;
}

// A buttress plank: a thick wall running out from the trunk, its top edge
// curving down to the ground. It grows out of the trunk's own centre line at
// each height, because a giant's trunk wanders up to 0.07 H off its foot.
// Planks placed round the foot stood clear of a wandering trunk like loose
// paper spikes, with their tops ~2 m out in the air.
// The cross-section runs round the grid's columns (g.x, with s the sine of
// the angle round it). It's a slab some 20-40 cm thick, thickest low down,
// with a rounded crest. Up to the shoulders (s ~ 0.7) the slab's middle line
// climbs to a radius below the top edge, and above them it stays there while
// the crest rounds over it. The normal is the angle's cosine across and
// s^3 + s / 2 up. It's level at mid-height, leans up on the upper flank (where
// moss gathers, as on the trunk), leans down on the lower flank (into the
// floor's shade) and turns up over the crest. The top catches the light from
// the canopy, and seen end on, a plank is a solid, rounded fin. A plank
// tapered to a wedge a few cm wide became a knife edge on the near trees'
// finer grid and looked like a cut-out sheet. There are eight columns round
// (16 up close), so a vertex sits on each shoulder and on the crest.
vec3 fin(inout vec2 g){
    vec4 r = h4(S * 7.0 + float(piece));
    float v = g.y, a = S * 5.0 + float(piece) * 0.8976 + r.x * 0.5 + (r.w - 0.5) * v * v;
    float L = R * (1.8 + 1.8 * r.y), hb = R * (2.4 + 2.8 * r.z), ph = 6.2832 * g.x, s = sin(ph),
          w = (R * 0.15 + 0.04) * (1.0 - 0.6 * v) * (1.1 - 0.4 * s),
          y = mix(-0.6, hb * pow(1.0 - v, 2.2) * (1.0 + 0.1 * sin(v * 11.0 + r.y * 6.0)) + 0.15 - w, min(0.6 + 0.6 * s, 1.0));
    vec3 d = vec3(cos(a), 0.0, sin(a)), sd = vec3(-d.z, 0.0, d.x);
    vec3 q = tc(y / top()) + d * (R * 0.5 + L * v) + sd * sin(v * 4.0 + r.x * 9.0) * 0.12 * R;
    q.y = mix(B.y, s3h(q.xz), v) + y;
    NM = normalize(sd * cos(ph) + UP * (s * s * s + 0.5 * s));
    // Replace v with how far the point stands out from the trunk's surface at
    // this height, in metres. jt.frag uses it to lay the bark along the plank
    // and to shade the corner where the plank leaves the trunk.
    g.y = R * 0.5 + L * v - tr(y / top());
    return q + NM * w;
}

// A crown leaf card: clustered round the ends of the limbs (giants) or along
// the boughs in flat sprays (slender trees), facing up-ish.
vec3 card(vec2 g){
    vec4 r = h4(S * 11.0 + float(piece) * 0.77);
    float f = (float(piece) + r.w) / 48.0;      // low to high, so from below the near leaves hide the far ones
    limb(int(f * 10.0));
    vec3 c, n = normalize(vec3(r.x - 0.5, 1.2 - r.w * 0.6, r.y - 0.5));
    float sz;
    if (K == 0.0) {                                   // spread evenly over the crown's dome (~1.7 layers)
        float a = float(piece) * 2.39996 + S * 9.0, e = sqrt((float(piece) + 0.5) / 48.0), L = H * 0.27;
        c  = tc(1.0) + vec3(cos(a) * e * L, L * (0.1 + 0.32 * (1.0 - e * e) + 0.1 * r.w), sin(a) * e * L);
        c  = mix(c, LB, 0.2);
        sz = (3.2 + 1.2 * r.z) * H * 0.027;
    } else {
        float k = clamp(H / 14.0, 0.55, 1.0);         // saplings: smaller, more upright sprays
        c  = bez(0.2 + 0.8 * r.z) + (r.xwy - 0.5) * vec3(1.6, 0.5, 1.6) * k;
        sz = (2.2 + 1.4 * r.w) * k;
        n.xz *= H < 12.0 ? 2.0 : 0.5;
    }
    if (FAR > 35.0) sz *= float(~piece & 1) * 1.4;    // far crowns: half the cards, larger
    if (FAR > 0.0 && piece >= (K == 0.0 ? 40 : H < 12.0 ? 10 : 22)) sz = 0.0;   // the camera sees fewer than the sun
    if (dot(normalize(c - vec3(0.0, 1.55, 0.3)), uGap.xyz) > uGap.w || K == 2.0 && float(piece) >= (H < 12.0 ? 24.0 : 36.0)
        || uQ > 0.5 && length(cross(c - vec3(29.7, 2.5, -17.5), uLightDir)) < 8.0)
        sz = 0.0;   // Keep the sky window open. Slender trees carry 36 cards, saplings 24.
                    // In the sun's maps (uQ), leave a gap in the crowns at the forest's edge,
                    // so a pool of sun falls on the tyrannosaurus's track where it stops to
                    // look at us (out of view above). No card goes within 8 m of the sun's
                    // ray through (29.7, 2.5, -17.5), which is roughly 2.5 m above
                    // s3_beach_pt(S3_REX_S, jr_across(58.1)).
    CT = normalize(cross(n, vec3(sin(r.z * 6.3), 0.0, cos(r.z * 6.3))));
    NM = n;
    CS = sz;
    return c + (CT * (g.x - 0.5) + cross(n, CT) * (g.y - 0.5)) * sz;
}

// A leaf of a near crown (part 8). The card's spray is grown as real leaves,
// 10 per card, and the card is piece / 10. Each leaf is a strip folded up
// along its midrib, scattered over the spray and drooping toward its tip.
// The baked sheet cuts it out.
vec3 leafm(vec2 g){
    int j = piece;
    piece /= 10;
    vec3 c = card(vec2(0.5)), n = NM, b = cross(n, CT);
    piece = j;
    vec4 r = h4(S * 29.0 + float(j) * 0.37);
    float a = r.y * 6.2832, L = CS * (0.19 + 0.09 * r.x), w = L * 0.335, s = g.y, v = g.x * 2.0 - 1.0, dr = 0.15 + 0.3 * r.z;
    vec3 T = CT * cos(a) + b * sin(a), Sd = cross(n, T);
    NM = normalize(cross(T - n * 2.0 * dr * s, Sd + n * sign(v) * 0.3));
    UV = (floor(fract(r.xy * 13.0) * 2.0) + vec2(0.5 + 0.31 * v, 0.037 + 0.926 * s)) * 0.5;
    return c + (CT * (r.z - 0.5) + b * (r.w - 0.5)) * CS * 0.8 - T * L * 0.3
             + T * s * L + Sd * v * w + n * (abs(v) * w * 0.3 - dr * s * s * L);
}

// A palm frond: an arching rachis with leaflets hanging off both sides.
vec3 rach(float u, vec4 r){
    float az = S * 3.0 + float(piece) * 2.4, el = mix(0.9, -0.5, float(piece) / 14.0) + 0.4 * (r.x - 0.5);
    float L = 1.2 + H * 0.3 * (0.8 + 0.4 * r.y);
    return tc(1.0) + (vec3(cos(az), 0.0, sin(az)) * cos(el) + UP * sin(el)) * L * u - UP * 0.4 * L * u * u;
}
vec3 frond(vec2 g){
    vec4 r = h4(S * 13.0 + float(piece));
    float u = g.y, v = g.x * 2.0 - 1.0;
    if (FAR > 0.0 && (piece >= (H < 8.0 ? 5 : 8) || FAR > 35.0 && (piece & 1) == 1)) u = 0.0;   // fewer fronds on small palms and far off
    vec3 p = rach(u, r), d = normalize(rach(u + 0.01, r) - p), sd = normalize(cross(d, UP));
    float l = (1.2 + H * 0.3) * 0.22 * sin(3.1416 * min(u * 0.85 + 0.12, 1.0));
    vec3 e = normalize(sd * sign(v) - UP * 0.9 + d * 0.4);
    NM = cross(e, d) * (v < 0.0 ? -1.0 : 1.0);         // (not sign(v): that gives a zero normal on the midrib, and a NaN pixel)
    return p + e * l * abs(v);
}

// An epiphyte leaf: rosettes of 8 straps (or fern fronds) perched on a giant's
// trunk and in its crown fork, arching out and drooping.
vec3 epi(vec2 g){
    vec4 r = h4(S * 19.0 + float(piece / 8));
    float s = piece < 40 ? 0.06 + 0.4 * r.x : 0.97, a = r.y * 6.2832 + float(piece) * 0.785;
    vec3 o = vec3(cos(r.y * 6.2832), 0.0, sin(r.y * 6.2832));
    vec3 ax = normalize(o + UP * 1.3);                                  // the rosette leans out
    vec3 rd = normalize(vec3(cos(a), 0.0, sin(a)) - ax * dot(vec3(cos(a), 0.0, sin(a)), ax));
    float L = 0.8 + 0.7 * r.z, u = g.y, v = g.x * 2.0 - 1.0;
    vec3 sd = cross(ax, rd), p = tc(s) + o * tr(s) + (rd * u + ax * (0.9 * u - 1.1 * u * u)) * L;
    NM = normalize(cross(sd, rd + ax * (0.9 - 2.2 * u)));
    return p + sd * v * L * 0.09 * (1.0 - u * u);
}

// A curtain of hanging leaves: a strip dropping from near a limb's end to a
// metre or two above the floor, turned toward the clearing.
vec3 curtain(vec2 g){
    vec4 r = h4(S * 23.0 + float(piece));
    limb(piece);
    if (FAR > 0.0 && piece > 2) return B;             // the camera sees 3 of the 8
    vec3 t = bez(0.6 + 0.4 * r.x);
    float len = min(t.y - B.y - 1.5 - 3.0 * r.z, 5.0 + 10.0 * r.y);
    NM = -normalize(vec3(t.x, 0.0, t.z));
    return t + vec3(-NM.z, 0.0, NM.x) * (g.x - 0.5) * len * 0.2 + NM * sin(g.y * 2.0 + r.w * 6.0) * 0.5 * g.y
             - UP * g.y * len;
}
// The far forest (part 9): JT_FAR billboards of the trees 46-110 m out,
// nearest first. Each is one of the four trees baked in s3_trees.c
// (trees_make), at its own size, upright and facing the eye. The fragment
// shader blends the two baked views on either side of the direction it's
// seen from. Behind them, a wall 118 m out (row 4, 9 degrees per quad) closes
// off the haze. No billboard stands in the cove the beach is seen through.
// Once the camera heads out to the beach, none stand on the sand, round the
// cove's flanks or out over the sea either, because the wide shots from the
// trees see past the flanks.
// A culled billboard or wall quad is dropped whole, with all its corners sent
// to one point: the origin, with NM zero so the leaves' flutter can't spread
// them. The wall is tested at the quad's middle. When it was tested corner by
// corner, a wall quad on the edge of the cove kept one side out at 118 m and
// pulled the other to the clearing's middle. NM wasn't set on that path, so
// the corner went wherever the compiler's leftovers pushed it, and a wedge of
// dark wall cut across the look round.
vec3 farb(vec2 g){
    float i = float(gl_InstanceID), a = hs(i) * 6.2832, r = floor(hs(i + 2.1) * 4.0);
    float s = (0.75 + 0.4 * hs(i + 1.3)) * (r < 2.0 ? 1.0 : r < 3.0 ? 0.8 : 0.45);
    vec3 P = vec3(cos(a), 0.0, sin(a)) * (46.0 + 64.0 * sqrt(i / 240.0));
    vec2 t = normalize(uCam.xz - P.xz);
    NM = vec3(0.0);
    if (i >= 240.0) {
        a = (i + 0.5) * 0.15708;
        P = vec3(cos(a), 0.0, sin(a)) * 118.0;
        r = 4.0;
    }
    float al = dot(P.xz, s3sea), ac = dot(P.xz, vec2(0.5, 0.8660254));
    if ((r < 4.0 ? al > 26.0 && abs(ac) < 34.0 || uReal > 0.0 && al > 30.0 : al > 59.0 && abs(ac) < 55.0)
        || uReal > 0.0 && (al > 10.0 && ac > -38.0 || al > 40.0)) return vec3(0.0);
    if (i >= 240.0) {                                 // the wall: this corner's own place
        a = (i + g.x) * 0.15708;
        P = vec3(cos(a), 0.0, sin(a)) * 118.0;
    }
    P.y = s3h(P.xz) - s;
    B = P;                                            // its own foot, seed and kind, so it sways in the wind
    S = a;                                            // like the trees, and never with another tree's data
    K = 0.0;
    UV = r < 4.0 ? g : vec2(a * 118.0, 0.0);
    FV = r * 16.0 + mod(atan(t.y, t.x) * 1.2732395, 8.0);
    return P + (r < 4.0 ? vec3(t.y, 0.0, -t.x) * (g.x - 0.5) * 24.0 * s + vec3(0.0, g.y * 44.0 * s, 0.0) : vec3(0.0, g.y * 80.0, 0.0));
}

vec3 surf(inout vec2 g){
    if (part == 9) return farb(g);
    if (part == 7) return curtain(g);
    if (part == 6) return epi(g);
    if (part == 1) return fin(g);
    if (part == 4) return card(g);
    if (part == 8) return leafm(g);
    if (part == 5) return frond(g);
    return tube(g);
}

void main(){
    part  = int(uP.x);
    int per = int(uP.z), cols = int(uP.w) & 127;
    int tree = int(uP.y) + gl_InstanceID / per;
    piece = gl_InstanceID % per;
    vec4 a = uT[2 * tree], b = uT[2 * tree + 1];
    B = a.xyz; R = a.w; H = b.x; K = b.y; S = hs(floor(b.z) * 0.618 + 0.1);   // b.z: its seed . the gust at its foot
    BP = K == 1.0 ? step(30.0, dot(B.xz, s3sea)) : 0.0;
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);                   // culled unless built below
    FAR = abs(uQ) > 0.5 ? 0.0 : b.w;                         // the sun's map and the impostors' bake (uQ -1) get everything
    if (part == 3 && FAR > 20.0 || part == 6 && FAR > 15.0 || part == 1 && (FAR > 25.0 || FAR > 12.0 && piece > 4)
        || part == 2 && FAR > 0.0 && piece > (FAR > 30.0 ? 4 : H < 12.0 ? 5 : 9)) return;   // limbs: fewer far off
    if (part == 2) limb(piece);
    else if (part == 3) limb(piece % 5);
    if (part == 3 && piece < 4) {      // a liana: from the limb down to the ground, or hanging free
        vec4 r = h4(S * 5.0 + float(piece) * 3.3);
        LA = bez(0.4 + 0.5 * r.x);
        vec3 e = vec3(cos(r.z * 6.3), 0.0, sin(r.z * 6.3));
        LB = r.y < 0.6 ? LA + e * (1.5 + 4.0 * r.w) : LA + e * r.w - UP * (5.0 + 10.0 * r.w);
        if (r.y < 0.6) LB.y = s3h(LB.xz) - 0.3;
        SG = (LA.y - LB.y) * 0.15 + 1.0;
        LR = 0.03 + 0.05 * r.w;
    } else if (part == 3) LR = 0.07 + 0.07 * hs(S + float(piece));
    int q = gl_VertexID / 6, c = gl_VertexID % 6;
    vec2 g = vec2(float(q % cols + int(c == 1 || c == 2 || c == 4)),
                  float(q / cols + int(c == 2 || c == 4 || c == 5))) / vec2(cols, int(uP.w) >> 7);
    vec3 p = surf(g), n = NM;
    // The wind. A tree bends downwind with the gust at its foot, taken from
    // s3_trees.c's jt_gust 0.6 s late (the far billboards work theirs out
    // here). It also sways on its own, more so in a gust. Leaves flutter on
    // top, harder in a gust, and fronds flutter more toward their tips. The sea
    // breeze blows harder toward the forest's edge, and the palms there and
    // out on the beach take it full on.
    float hgt = max(p.y - B.y - 2.0, 0.0), w = part == 9 ? gust(B.xz, 0.6) : fract(b.z) / 0.9,
          bp = 1.0 + (K == 1.0 ? 2.0 : 0.5) * smoothstep(20.0, 32.0, dot(B.xz, s3sea));
    p += (vec3(-s3sea.x, 0.0, -s3sea.y) * w + vec3(sin(uTime * 0.6 + S * 6.0), 0.0, cos(uTime * 0.5 + S * 4.0)) * (0.3 + 0.5 * w))
         * hgt * hgt / (hgt + 12.0) * 0.012 * bp;
    if (part > 3) p += n * sin(uTime * 3.0 + float(piece) * 1.7 + g.x * 2.0) * 0.05 * (0.3 + w) * bp * (part == 5 ? g.y : 1.0);
    vW = p;
    vN = n;
    vU = part > 7 ? UV : g;
    vS = part > 8 ? FV : K + 0.9 * hs(S * 17.0 + float(part == 1 ? 0 : piece) * 0.31);   // kind . random (a plank uses its trunk's)
    gl_Position = uViewProj * vec4(p, 1.0);
}
