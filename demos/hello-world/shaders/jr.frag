#version 450 core
// The tyrannosaurus's hide: dark grey-brown and rough, mottled and paler
// underneath, with near-black bands across the back, pebbled with scales and
// creased with folds. The pattern comes from the rest-pose position, so it
// rides with the body. The head has a wet amber eye under the brow, a row of
// teeth along the lip and the dark of the mouth as the jaw drops. The eye's
// cornea mirrors the bright sky as a catchlight. The shared model lights it
// at the forest's edge, where it walks in the shade of the crowns and then
// stops in a pool of sun (jt.vert opens the crowns there for the sun only).
// The sun there lights its back and head from above and behind. The bright
// beach and sky behind it give it a rim, reflected at grazing angles. The
// floor patch only darkens.
in vec3 vWorld;
in vec3 vNormal;
in vec3 vRest;
in vec4 vPart;
in vec3 vEye;
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
layout(binding = 1) uniform sampler3D uNoise;   // 32^3 random bytes, wrapping (main.c's noise_make)
// Value noise, 0..1. The hardware blends the lattice; each cell's fraction
// goes through the cubic first.
float vn3(vec3 p){ vec3 i = floor(p), f = p - i; return texture(uNoise, (i + f * f * (3.0 - 2.0 * f) + 0.5) / 32.0).r; }
void main(){
    if (uQ > .5) { o = vec4(0.); return; }                         // a sun map: depth only
    if (vPart.z > .5) { o = vec4(vec3(1. - vPart.w), 1.); return; }
    vec3 V = normalize(uCam - vWorld), N = normalize(vNormal), q = vRest;
    if (dot(N, V) < 0.) N = -N;
    float rough = .65, y = 0.;                                    // y: a wet cornea's reflectance head-on (below)
    // the relief: pebbly scales, larger plates, folds running round the trunk
    float fp = length(fwidth(q)), n = vn3(q * 5.), h = .35 * mix(vn3(q * 16.), .5, smoothstep(.025, .12, fp)) + .35 * n + .3 * sin(q.z * 7. + 2. * sin(q.y * 2.3 + q.x)) * smoothstep(2., 2.4, q.y) * smoothstep(3.9, 3.5, q.z);
    vec3 dx = dFdx(vWorld), dy = dFdy(vWorld), r1 = cross(dy, N), r2 = cross(N, dx);
    float det = dot(dx, r1), f = .06 * (1. - smoothstep(.04, .2, fp));   // unresolved scales become smooth hide
    N = normalize(abs(det) * N - f * sign(det) * (dFdx(h) * r1 + dFdy(h) * r2));
    // Dark grey-brown, paler underneath and mottled, with near-black bands
    // across the back and tail that fade down the flanks. A brighter olive
    // looked like a khaki clay toy in the shade.
    float sb = smoothstep(.45, .8, .5 + .5 * sin(q.z * 4.5 + 2.5 * n + 1.5 * sin(q.y * 1.7))) * smoothstep(-.3, .5, N.y);
    vec3 c = mix(vec3(.05, .038, .026), vec3(.11, .085, .06), smoothstep(-.2, -.8, N.y)) * (1. - .85 * sb)
           * (.95 + .25 * sin(q.z * 1.7 + 2. * sin(q.y * 2.1)) + .2 * n) * (.85 + .3 * h);
    // The eye, the dark socket under the brow, the teeth along the lip and the
    // mouth. The eye's surface is a dome bulging from its socket. Its normal
    // points out from the eyeball's centre (vEye, carried by the head's bone)
    // and is about 40 degrees off the eye's axis at the rim, so a few of its
    // pixels always face halfway between the camera and the sky.
    float e = length(vec2(q.y - 4.88, q.z - 4.58)), t = fract(q.z * 16.);
    if (abs(q.x) > .12) {
        if (e < .045) { c = e < .02 ? vec3(.005) : vec3(.32, .16, .02); rough = .12; N = normalize(vEye); y = .2; }
        c *= .55 + .45 * smoothstep(.04, .09, e);
    }
    if (q.z > 4.6 && q.z < 5.35 && vPart.x > .1 && vPart.x < .45 - .35 * abs(2. * t - 1.)) { c = vec3(.4, .36, .27); rough = .3; }
    c = mix(c, vec3(.02, .006, .005), smoothstep(.25, .4, vPart.x) * (1. - smoothstep(.6, .75, vPart.x)) * smoothstep(.05, .09, vPart.w) * step(4.3, q.z));
    // In the shade at the forest's edge it's open to the sky over the beach,
    // so its back gets the sky's light and its belly very little. The
    // occlusion runs from 0.3 to 0.9 so its form shows. At 0.2-0.45 over the
    // whole hide it was a flat black cut-out. Between the thighs under the
    // belly (w, from the rest pose) it's a fifth of that, because the legs and
    // the belly close that area in. Without it, the inner thighs and the
    // crotch, the skin the stride stretches most, would pick up the sunlit
    // sand and sky there.
    float w = 1. - .8 * smoothstep(.5, .15, abs(q.x)) * smoothstep(3.1, 2.6, q.y);
    vec3 l = s3light(vWorld, N, c, rough, 0., (.3 + .6 * smoothstep(-.9, .5, N.y)) * w);
    // Broad light from the open beach reveals the face without tracing its
    // silhouette. It belongs to the ambient share, so creases stay shaded.
    vec3 fill = c * uBottom * .6 * smoothstep(3.4, 4.2, q.z) * (.35 + .65 * max(N.y, 0.)) * w;
    l += fill;
    s3amb += fill.r + fill.g + fill.b;
    // Thin skin with the sun behind it (the arms, the claws, the tip of the
    // tail) lets light through, reddened by the blood under it
    // (Barre-Brisebois' translucency). The thickness is taken as twice the
    // radius the skin curves around, from how fast the normal turns across
    // the pixel. Only convex curvature counts, since a crease is no thinner.
    // A metre of body lets nothing through and a 10 cm finger about a third.
    // The sun reaching the far side is taken to be as much as reaches this
    // side (s3sh).
    vec3 g = normalize(vNormal);
    float k = max(dot(dFdx(g), dx) / (dot(dx, dx) + 1e-9), dot(dFdy(g), dy) / (dot(dy, dy) + 1e-9));
    l += uGlow * s3sh * c * vec3(5., 1.6, .9) * exp(-12. / max(k, .01)) * max(-dot(N, uLightDir), 0.)
       * (.3 + 2. * pow(max(dot(-V, uLightDir), 0.), 3.));
    // At grazing angles the hide mirrors what lies behind it, the bright sky
    // and beach (Schlick's Fresnel, softened for its roughness). It mirrors
    // less where the reflected ray finds the shaded floor. The result is a rim
    // of light round its back, head and tail against the bright background.
    // The eye's wet cornea mirrors the sky at every angle, so the open sky
    // over the beach shows as a catchlight on its upper part. In the shade the
    // sun itself is hidden from it by the brow and head. A real cornea
    // reflects about 3% head-on; here 0.2 keeps the clear sky's catchlight
    // visible at 21 m (about 8 px at 150%) and steady from frame to frame. A
    // cornea bulging further (72 degrees) at 0.04 lit only its grazing rim,
    // just as brightly but as a sliver that sparkled as it walked.
    // Where the hide faces down, the reflected ray only grazes the horizon
    // from under the body, hence the + N.y. Without it the belly, the
    // undersides of the thighs and the crotch glowed, with a cyan rim along
    // the thighs.
    // This is sky light, so it joins the ambient share (s3amb, the alpha).
    // The screen-space occlusion darkens it in the folds, and the contact
    // shadows (the sun's share) leave it alone. Marching toward the sun from
    // the flank behind the near thigh, they would hit the thigh in front and
    // black out the rim in a slit where it meets the body.
    vec3 R = reflect(-V, N), r = s3sky(R) * (pow(1. - max(dot(N, V), 0.), 5.) * .6 + y) * smoothstep(-.4, .3, R.y + N.y) * w;
    // Where the pool of sun lights it from behind, there's also a rim of sun
    // along its back, neck and head, so the silhouette stands out against the
    // bright sky.
    l += r + uGlow * s3sh * pow(1. - max(dot(N, V), 0.), 4.) * max(dot(N, uLightDir), 0.);
    s3amb += r.r + r.g + r.b;
    o = vec4(mix(l, s3fog(l, vWorld), .5), s3a);                 // only half the haze, so it stays a dark shape against the beach
}
