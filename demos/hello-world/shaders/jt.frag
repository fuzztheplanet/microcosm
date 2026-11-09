#version 450 core
// The trees' surfaces. Bark uses the baked tile of ridged, flaking plates,
// whose relief tilts the normal and, within 6 m, adds parallax. Lichen and
// moss go on top, the moss gathering low down and on the tops of limbs and
// planks. Leaves are cut out of the cards and curtains with the baked leaf
// sheet: a jittered grid of leaves in two offset layers, each leaf turned,
// tilted and tinted on its own. Palm fronds are combs of drooping leaflets.
// The trees fill most of the frame, so they get their own, cheaper lighting,
// jlit(). Compared with s3light(), leaves get a plain glint in place of the
// GGX highlight and bark gets none, there's no sky sheen, and a 2-tap shadow
// replaces the penumbra search. The hardware compares and filters each tap
// over 2 x 2 texels, and the pair is turned at random from point to point,
// which gives the sun flecks soft edges.
in vec3  vW;
in vec3  vN;
in vec2  vU;
in float vS;          // the tree's kind + a random 0..0.9 per piece
out vec4 o;
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
layout(location = 0) uniform vec4 uP;      // part (see jt.vert)
layout(binding = 10) uniform sampler2D uBark;   // jt_bake.frag's tiles: bark (height, moss, tone),
layout(binding = 11) uniform sampler2D uLeaf;   // leaves (veins, tint, thickness, shape)
layout(binding = 13) uniform sampler2D uFarC;   // the far forest's impostor atlas: 8 views x 4 trees,
layout(binding = 14) uniform sampler2D uFarZ;   // lit colour and depth (1: nothing there)

float h3(vec3 p){
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}
layout(binding = 1) uniform sampler3D uNoise;   // 32^3 random bytes, wrapping (main.c's noise_make)
// Value noise, 0..1. The hardware blends the lattice; each cell's fraction
// goes through the cubic first.
float vn3(vec3 p){ vec3 i = floor(p), f = p - i; return texture(uNoise, (i + f * f * (3.0 - 2.0 * f) + 0.5) / 32.0).r; }
// One layer of leaves on a card. Each staggered cell holds one leaf, turned
// and shifted at random. Returns the baked leaf under the point (veins, tint,
// thickness, shape), from one of the sheet's four variants picked at random.
// l gets the position on the leaf (along it 0..1, across it) and two randoms.
// The caller works out the mip level lod from the card's own uv, outside any
// branch, so it stays smooth across the cells' seams.
vec4 leaf(vec2 uv, float g, float s, float lod, out vec4 l){
    vec2 c = uv * g;
    c.y += fract(floor(c.x) * 0.618);                 // stagger the columns so the leaves don't form rows
    vec2 id = floor(c);
    vec3 r = fract(h3(vec3(id, s)) * vec3(1.0, 17.13, 289.7));
    vec2 a = normalize(vec2(r.x, fract(r.x * 7.9)) - 0.5 + 1e-3), f = fract(c) - 0.5 - (r.yz - 0.5) * 0.2;
    f = vec2(dot(f, a), dot(f, vec2(-a.y, a.x)));
    l = vec4(f.x + 0.5, f.y * 4.0, r.yz);
    return r.z < 0.08 ? vec4(0.0) : textureLod(uLeaf, (floor(r.yz * 2.0) + clamp(vec2(0.5 + f.y * 1.15, (f.x + 0.54) / 1.08), 0.0, 1.0)) * 0.5,
                                                lod);
}
// Bark relief from the baked tile at u, 3 cm deep. The screen-space
// derivatives give the world gradients gu, gv of u along the surface, which
// carry the view direction into the tile. Within 6 m the relief is shifted
// one step along the view by the depth under it (parallax), and within 25 m
// the baked slopes also tilt N. The mipmaps average the slopes as they
// shrink, so they fade out instead of sparkling. When the slopes stopped at
// 10 m, trunks a little way off were lit flat and the plates looked painted
// on. Returns the texel, and moves u to where the eye's ray meets the relief.
vec4 relief(inout vec3 N, inout vec2 u, float d){
    vec3 dx = dFdx(vW), dy = dFdy(vW), r1 = cross(dy, N), r2 = cross(N, dx), V = normalize(uCam - vW);
    vec2 ux = dFdx(u), uy = dFdy(u);
    float k = 1.0 / dot(dx, r1);
    vec3 gu = (r1 * ux.x + r2 * uy.x) * k, gv = (r1 * ux.y + r2 * uy.y) * k;
    vec4 b = textureGrad(uBark, u, ux, uy);
    if (d < 25.0) {
        u -= vec2(dot(gu, V), dot(gv, V)) / max(dot(V, N), 0.3) * 0.03 * (1.0 - b.r) * (1.0 - smoothstep(4.0, 6.0, d));
        b = textureGrad(uBark, u, ux, uy);
        N = normalize(N - (gu * (b.b - 0.5) + gv * (b.a - 0.5)) * 3.0 * (1.0 - smoothstep(18.0, 25.0, d)));
    }
    return b;
}
vec3 jlit(vec3 N, vec3 alb, float gl, float tr, float ao){
    // The shadow map is read a little off the surface on the sun's side,
    // along N turned toward the sun (faceforward). Leaves face the eye, so a
    // leaf seen from below with the sun behind it has N pointing away from
    // the sun. Offsetting along that N put the read in the leaf's own shadow,
    // so the leaf shadowed its own backlit glow.
    vec3 L = uLightDir, s = (uLightVP * vec4(vW + faceforward(N, -L, N) * 0.03, 1.0)).xyz * 0.5 + 0.5, V = normalize(uCam - vW);
    float e = 1.0 - smoothstep(0.45, 0.5, max(abs(s.x - 0.5), abs(s.y - 0.5))), k = 0.0;
    vec2 u = s.xy * uRim.x + uRim.yz;
    if (e < 1.0 && max(abs(u.x - 0.5), abs(u.y - 0.5)) < 0.49) { k = 1.0; e = 1.0; }
    // The shadow: two taps 2.5 texels apart, along a direction turned at random
    // for each point. The jitter moves the point every frame, so the TAA blends
    // the turns into a soft, round edge. Two fixed taps showed the map's 3.5 cm
    // texels as stair-steps across the sun flecks on the trunks.
    float nl = dot(N, L), l = max(nl, 0.0), nv = max(dot(N, V), 0.001),
          a = fract(sin(dot(vW, vec3(12.9898, 78.233, 37.719))) * 43758.5) * 6.2832;
    u = vec2(cos(a), sin(a)) * 0.0006;
    float sh = s3pcf(s.xy + u, s.z - 0.0002, k) + s3pcf(s.xy - u, s.z - 0.0002, k);
    sh = mix(mix(0.2, 1.0, smoothstep(33.0, 37.0, dot(vW.xz, s3sea))), sh * 0.5, e);
    vec3 c = alb * l;
    if (tr == 0.0) c = s3on(alb, 0.45, l, nv, dot(L, V) - l * nv) * l;         // rough bark (waxy leaves use plain Lambert)
    float gloss = 60.0 / (1.0 + 60.0 * (dot(dFdx(N), dFdx(N)) + dot(dFdy(N), dFdy(N))));
    if (tr > 0.0)                                     // unresolved leaf normals spread the glint and lower its peak
        c += gl * (gloss / 60.0) * pow(max(dot(N, normalize(L + V)), 0.0), gloss) - c * tr * 0.3
           + alb * vec3(1.5, 1.15, 0.45) * tr * max(-nl, 0.0) * (0.5 + 2.0 * pow(max(dot(-V, L), 0.0), 4.0));
    // Ambient light: the canopy's from above, the floor's from below, skylight
    // through a leaf, and the haze's. The humid air glows gold toward the sun
    // (s3fog), so the haze term, uGlow x 0.06 x (0.5 + 0.5 N.L)^3, gives
    // surfaces facing that way a warm light the far side never gets. A trunk's
    // shaded flank goes from gold to green-grey instead of one flat tone, and
    // when the eye looks into the light the trunks' sunward edges glow.
    l = 0.5 + 0.5 * nl;
    return uGlow * sh * c + s3am(alb * (mix(uGroundColor, uSkyColor, N.y * 0.5 + 0.5) + tr * uSkyColor * (0.5 - 0.5 * N.y) + uGlow * 0.06 * l * l * l), alb, ao);
}

void main(){
    int part = int(uP.x);
    if (part == 9) {                                  // the far forest (jt.vert's farb)
        float r = floor(vS / 16.0), k = vS - r * 16.0, f = fract(k);
        if (r > 3.5) {                                // the wall: a canopy far off, lost in the haze
            float x = vU.x, top = 46.0 + 16.0 * vn3(vec3(x * 0.02, 0.5, 0.5)), c;
            for (float i = 1.0; i < 5.0; i *= 2.0) {      // crowns, big to small, uneven and of random heights,
                c = x * i / 12.0 + 1.5 * vn3(vec3(x * 0.04, i, 1.5));   // a few missing; the small ones are leaf clumps on the big
                float g = h3(vec3(floor(c), i, 3.0));       // each with its own width and height, so no row of arches
                top += 7.0 / i * sqrt(max(g - pow(2.0 * fract(c) - 1.0, 2.0), 0.0)) * (0.4 + g);
            }
            top += (vn3(vec3(x * 0.9, vW.y * 0.4, 4.5)) + vn3(vec3(x * 3.1, vW.y, 6.5))) * 1.2;   // and ragged with leaves
            if (vW.y > top) discard;
            // darker in the trunks' depths, lighter toward the sunlit crowns, mottled with foliage
            o = vec4(s3fog(vec3(0.03, 0.05, 0.03) * (0.4 + smoothstep(top - 25.0, top, vW.y)) * (0.7 + 0.6 * vn3(vec3(x * 0.4, vW.y * 0.4, 7.0))), vW), s3a);
            return;
        }
        // Blend the two baked views on either side, or use just one where only
        // one covers the pixel. Each view's coverage comes from its filtered
        // depth: a tree stands about 0.3-0.7 deep in its box, and 1 means
        // nothing is there. The cut is dithered between 0.1 and 0.9 and the TAA
        // blends it. An atlas texel is about 2 pixels here, and a hard
        // nearest-texel cut drew the far trees' outlines as stair-steps in the
        // haze.
        vec2 a = (vec2(floor(k), r) + vU) / vec2(8.0, 4.0), b = (vec2(mod(floor(k) + 1.0, 8.0), r) + vU) / vec2(8.0, 4.0);
        float wa = clamp(4.0 - 4.0 * texture(uFarZ, a).r, 0.0, 1.0) * (1.0 - f), wb = clamp(4.0 - 4.0 * texture(uFarZ, b).r, 0.0, 1.0) * f;
        if (wa + wb < 0.1 + 0.8 * fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233)) + uTime * 61.0) * 43758.5)) discard;
        o = vec4(s3fog((texture(uFarC, a).rgb * wa + texture(uFarC, b).rgb * wb) / (wa + wb), vW), s3a);
        return;
    }
    vec3 N = normalize(vN), alb;
    if (part < 4 ? !gl_FrontFacing : dot(N, uCam - vW) < 0.0) N = -N;   // leaves face the eye
    float tr = 0.0, gl = 0.0, ao = 1.0, gy = vW.y - s3h(vW.xz), kd = floor(vS), rs = fract(vS), d = length(vW - uCam);
    vec4 l = vec4(0.0), lt = vec4(0.0, 0.5, 1.0, 1.0);
    if (part == 4 || part == 7) {                     // crown cards, hanging curtains
        // A giant's crown, 20-40 m up, has 9 x 9 leaves per card here and 6 x 6
        // in the sun's map. At 6 x 6 it looked like scattered chips against
        // the sky.
        vec2 uv = part == 7 ? vU * vec2(1.0, 5.0) : vU;
        float g = part == 7 ? 4.0 : uQ > 0.5 ? 6.0 - kd : 9.0 - 2.5 * kd, lod = log2(length(fwidth(uv)) * g * 380.0);
        lt = leaf(uv, g, rs * 50.0, lod, l);
        // The heart of a giant crown clump is solid foliage in its own shade.
        // Only the picture gets it, and the sun's map keeps the gaps. Without
        // it, the crowns seen from below were see-through discs of dots.
        bool hf = part == 4 && kd < 0.5 && uQ < 0.5 && length(vU - 0.5) < 0.15 + 0.2 * vn3(vec3(vU * 5.0 + rs * 30.0, 2.5));
        if (length(fwidth(uv)) * g > 0.6) {           // leaves under ~2 px become steady, ragged clumps that don't shimmer
            if (vn3(vec3(uv * g * 0.8 + rs * 40.0, 0.5)) < 0.4) {
                if (!hf) discard;
                ao = 0.3; lt.b = -1.0;
            }
        } else if (lt.a < 0.5 && (lt = leaf(uv + 0.5 / g, g, rs * 50.0 + 9.0, lod, l)).a < 0.5) {
            if (!hf) discard;
            lt = leaf(uv * 1.6 + 0.3, g, rs * 50.0 + 17.0, lod + 0.7, l);   // more leaves, deeper in
            ao = 0.2 + 0.25 * lt.a; lt.b = -1.0;
        }
        if (part == 4 ? length(vU - 0.5) > 0.5
                      : abs(fract(vU.x * 3.0) - 0.5) > 0.45 - 0.2 * vU.y || vU.y > 0.8 + 0.2 * h3(vec3(floor(vU.x * 3.0), rs, 1.0)))
            discard;                                  // round crown clumps, ragged curtain strands
        if (part == 7 && (vW.y - uCam.y) / d > 0.35 + 0.4 * l.z) discard;   // seen from far below, a curtain thins out
    } else if (part == 8) {                           // a near crown's modelled leaf, cut out by the leaf sheet
        lt = texture(uLeaf, vU);
        if (lt.a < 0.5) discard;
        l = vec4(fract(vU.y * 2.0), 0.0, rs, fract(rs * 7.3));
    } else if (part == 5) {                           // palm leaflets: a comb of 44 per side
        // Where they'd come closer than ~3 px, the count is halved, and halved
        // again: every other leaflet is kept and the kept ones get wider.
        // Finer than that, they flicker under the jitter. The sun's map uses 15.
        float v = abs(vU.x * 2.0 - 1.0), f = fwidth(vU.y) * 44.0,
              n = uQ > 0.5 ? 15.0 : f < 0.33 ? 44.0 : f < 0.66 ? 22.0 : f < 1.33 ? 11.0 : 5.5,
              q = fract((vU.y + v * 0.05) * n) - 0.5;
        if (v > 0.06 && abs(q) > 0.28 * (1.0 - v * v) + 0.04) discard;
        l = vec4(v, q * 4.0, fract(vU.y * 44.0 * 0.37), rs);
    } else if (part == 6) {                           // epiphytes: bromeliad straps, or fern fronds
        float q = fract(vU.y * 16.0 + abs(vU.x * 2.0 - 1.0) * 0.8) - 0.5;
        if (rs > 0.55 && abs(vU.x * 2.0 - 1.0) > 0.12 && abs(q) > 0.3) discard;
        l = vec4(vU.y, vU.x * 2.0 - 1.0, rs, rs * 0.5);
    }
    if (uQ > 0.5) { o = vec4(0.0); return; }          // shadow pass: depth only
    if (part >= 4) {
        // a leaf (or an epiphyte's) gets its own tilt, tint and veins; it's glossy and glows when backlit
        vec3 j = vec3(l.zw, fract(l.z * 7.3 + l.w * 3.1)) - 0.5;
        N = normalize(N + j * (part == 8 ? 0.3 : 0.9) + N * 0.2);
        alb = mix(vec3(0.03, 0.065, 0.022), vec3(0.09, 0.15, 0.035), l.z) * (0.6 + 0.6 * l.w) * (1.15 - 0.3 * l.x);
        if (part == 5) alb = mix(vec3(0.06, 0.11, 0.022), vec3(0.15, 0.16, 0.045), l.z);
        if (part == 6) alb = mix(vec3(0.25, 0.04, 0.03), vec3(0.06, 0.13, 0.03), smoothstep(0.1, 0.45, l.x + rs));
        // veins, yellowed margins and bites from the baked sheet (palm leaflets
        // and the epiphytes paint their own)
        alb *= (part == 5 || part == 6 ? 1.0 + 0.5 * exp(-abs(l.y) * 25.0) + 0.15 * step(fract(l.x * 9.0 + abs(l.y) * 1.5), 0.12)
                                        : 1.0 + 0.8 * lt.r) * (0.75 + 0.5 * lt.g);
        alb = mix(alb, vec3(0.13, 0.11, 0.03), smoothstep(0.6, 1.0, lt.g) * 0.7);
        tr = 0.5 + 0.4 * lt.b; gl = 0.25;
    } else {
        // Bark: the baked tile of ridged plates, wrapped round the surface in
        // its own (round, along) parameters. The tile runs up a trunk by height
        // and out along a plank from the trunk, about 0.9-1 m per tile. The
        // 3 cm relief tilts the normal, and lichen and moss go on top.
        vec2 u = vU * (part == 0 ? vec2(kd == 0.0 ? 8.0 : 4.0, 0.0) : part == 1 ? vec2(4.0, 1.0) : part == 2 ? vec2(4.0, 10.0) : vec2(1.0, 20.0))
               + rs * vec2(3.0, 7.0);
        if (part == 0) u.y += vW.y * 1.1;
        // Stretch and shift the tile round the trunk a little more at each turn
        // up it, so it never repeats in step and there's no band every 90 cm.
        u += vec2(0.37 * sin(u.y * 1.3 + rs * 9.0), 0.4 * sin(u.y * 0.7 + rs * 5.0));
        vec4 b = relief(N, u, d);
        // Moss and lichen grow in patches ~2 m across, from the tile's own mask
        // at a quarter scale. Lichen is a pale crust, thicker on some trunks.
        float f = b.g, fr = b.r, m = 1.4 * texture(uBark, u * 0.25 + 0.5).g - 0.15, li = smoothstep(0.6, 0.7, m + 0.3 * f + 0.3 * rs - 0.1) * 0.8;
        m = smoothstep(0.6, 1.0, m + 0.45 * N.y - 0.08 * gy / (1.0 + 0.02 * gy * gy) + 0.25 + 0.2 * f - 0.3 * kd * (2.0 - kd));
        // The bark's tone and plates, plus long streaks where rain runs down the
        // trunk. The streaks are the mask again, stretched ~10 m up and ~1 m round.
        alb = mix(vec3(0.19, 0.16, 0.12), vec3(0.10, 0.10, 0.08), rs) * (0.3 + 0.7 * fr) * (0.7 + 0.6 * f)
            * (0.4 + 1.2 * texture(uBark, u * vec2(0.4, 0.04)).g)
            * (kd == 1.0 ? 0.6 + 0.4 * smoothstep(0.2, 0.4, fract(vW.y * 2.2)) : 1.0);   // palm rings
        if (kd == 2.0) alb = mix(alb, vec3(0.09, 0.10, 0.07), 0.6);                      // smoother, algae-stained
        alb = mix(alb, vec3(0.38, 0.40, 0.35), li);
        alb = mix(alb, mix(vec3(0.03, 0.06, 0.012), vec3(0.07, 0.12, 0.025), fr), m);
        if (part == 3) alb = mix(vec3(0.10, 0.08, 0.05), vec3(0.08, 0.12, 0.04), f);
        // Occlusion: the floor shades the bark low down, and the bark's cracks
        // shade themselves. A plank is also shaded by the trunk in the corner
        // where it leaves it (jt.vert's fin says how far out of the trunk it is).
        ao = (0.3 + 0.7 * smoothstep(0.0, 4.0, gy)) * (0.6 + 0.4 * fr) * (part == 1 ? 0.5 + 0.5 * smoothstep(0.0, 0.5, vU.y) : 1.0);
    }
    o = vec4(s3fog(jlit(N, alb, gl, tr, ao), vW), s3a);
}
