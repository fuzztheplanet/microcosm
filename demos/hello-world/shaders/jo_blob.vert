#version 450 core
// The blob comes ashore: the soft blue creature from scenes 1 and 2, with its
// two dark eyes and now two tiny arms as well. There's no vertex buffer, and
// gl_InstanceID picks the part:
//   0-1  what it carries, from its jump into the swash onwards
//   2-3  the two halves of the jelly, each with its arm
//   4-5  the eyes
//   6-7  the spray from its last two splashes
// gl_VertexID picks a corner of a 72 x 48 grid that the part bends into shape
// (or, for spray, one drop). The path and the hops on the beat come from
// s3_blob.c (JO_RUNS). The rest of the performance is timed here off uTime:
// rising out of the surf, shaking off the water, each hop's wind-up, stretch
// and squash with the arms flung up for balance, settling, blinks, a look
// around, the wave and the cheer at the end.
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
layout(location = 0) uniform vec4 uB;      // the blob's centre (s3_blob_at); w = seconds since its last run of hops ended
layout(location = 1) uniform vec4 uH;      // x = hops into the run (< 0: not hopping), y, z = each hop's height and reach (m), w = its turn (rad)
layout(location = 2) uniform vec4 uS[2];   // the last two splashes: position (x, z), how long ago, how hard
const float R = 0.48;   // the blob's radius (S3_BLOB_R); everything else is scaled to it
out vec3 vW;
out vec3 vN;
out vec3 vO;     // the point's direction in the blob's own squashed frame (for patterns); for a prop, its grid position around and along
flat out int vK; // 0 jelly, 2 eye, 3 spray drop; props: 4 straw, 5 parasol cloth, 6 wood, 7 canvas

vec3 C, Rt, U, F, S, sh, tp;   // centre, frame and squash; this side's shoulder and arm tip
float j;                       // the clock its cues are timed on (s)

float bump(float x, float a, float b){
    float m = (a + b) * 0.5;
    return smoothstep(a, m, x) * (1.0 - smoothstep(m, b, x));
}

// The jelly as a single surface (signed distance, in metres). The squashed
// ball of the body is melted together with this side's arm, a stubby round
// cone from the shoulder (0.21 R) to the tip (0.13 R). The arm swells out of
// the body like Ditto's, with a soft fillet and no seam.
float jelly(vec3 X){
    vec3 l = X - C, r = S * R, ba = tp - sh, pa = X - sh;
    l = vec3(dot(l, Rt), dot(l, U), dot(l, F));
    float k0 = length(l / r), b = k0 * (k0 - 1.0) / length(l / (r * r));
    float l2 = dot(ba, ba), rr = 0.08 * R, a2 = l2 - rr * rr, y = dot(pa, ba), z = y - l2;
    vec3 w = pa * l2 - ba * y;
    float x2 = dot(w, w), k = rr * rr * x2,
          a = (sign(z) * a2 * z * z * l2 > k ? sqrt(x2 + z * z * l2) / l2 - 0.13 * R :
               sign(y) * a2 * y * y * l2 < k ? sqrt(x2 + y * y * l2) / l2 - 0.21 * R :
               (sqrt(x2 * a2 / l2) + y * rr) / l2 - 0.21 * R);
    float h = max(0.14 * R - abs(a - b), 0.0) / (0.14 * R);
    return min(a, b) - h * h * 0.035 * R;
}

// A point turned around the axis Z through O: a is the angle (0..1 is a full
// turn), r the distance out from the axis and h the distance along it.
vec3 lathe(vec3 O, vec3 Z, float a, float r, float h){
    vec3 X = normalize(cross(Z, F)), Y = cross(Z, X);
    return O + Z * h + (X * cos(6.2832 * a) + Y * sin(6.2832 * a)) * r;
}

// The things it carries out of the water. When it lands from its jump into
// the swash (85.4), the splash throws them up out of the water and it catches
// them (see main). k 0 is a beach parasol in its right hand, with the pole
// leaning in over its head and the canopy springing open once it's caught,
// plus a straw sun hat. k 1 is a folded deckchair hanging from its left hand
// by the top bar. p is the part (its first grid row), a the position around
// it and t the position along it (0..1).
vec3 prop(int k, int p, float a, float t){
    vec3 D = normalize(C + U * (S.y * R + 0.8) + Rt * 0.3 - tp);
    float o = smoothstep(85.85, 86.3, j), l = 0.75 * min(t / 0.88, 1.0), s = pow(sin(12.566 * a), 2.0) * o, b = mix(0.12, 1.2, o);
    if (k < 1) {
        // The canopy: four ribs from the hub 1.2 m up the pole, bowing down
        // toward their tips. The cloth sags between them and is pulled in at
        // the tips. A valance hangs below (jo_blob.frag cuts its hem in
        // scallops).
        if (p < 1) return lathe(tp + D * 1.2, D, a, l * sin(b) - 0.07 * s * pow(l / 0.75, 3.0),
                                -l * cos(b) - (0.1 * l * l + 0.05 * s * l) * o - 0.6 * max(t - 0.88, 0.0));
        // The pole, pointed at both ends. The top end (the finial) pokes out
        // of the hub.
        if (p < 23) return lathe(tp - D * 0.35, D, a, 0.014 * min(12.0 * t * (1.0 - t), 1.0), 1.62 * t);
        // The hat sits tilted on top of the blob, with its brim clear of the
        // jelly. It has a round crown, a band, and a wide brim that droops and
        // waves.
        D = normalize(U - F * 0.07 - Rt * 0.05);
        l = min(t / 0.45, 1.0); o = max(t - 0.45, 0.0) / 0.55;
        return lathe(C + D * (S.y * R - 0.005), D, a, 0.15 * sin(1.5708 * l) + 0.26 * o,
                     0.11 * cos(1.5708 * l) - (0.05 - 0.015 * sin(18.85 * a)) * o * o);
    }
    // The deckchair hangs from the hand, turned a little out from the blob's
    // side, and swings when the hops stop. Each rail and bar of its two frames
    // is a tube (two grid rows), with the striped canvas folded between them.
    vec3 E = normalize(vec3(-Rt.x, 0.0, -Rt.z)), G = normalize(vec3(0.0, -1.0, 0.0) + E * (0.08 + 0.15 * sin(uB.w * 6.0) * exp(-uB.w * 2.0))),
         W = normalize(cross(E, G) + E * 0.7);
    if (p < 12) {
        int i = p / 2;
        float m = float(i == 2 || i == 3 || i == 5), x = float(i & 1) * 2.0 - 1.0;
        vec3 e = i < 4 ? vec3(x * (0.175 - 0.025 * m), 0.02 * m, 0.03 * m) : vec3(-0.19, 0.42 * m, 0.03 * m),
             f = i < 4 ? G * (0.46 - 0.06 * m) : W * 0.38;
        return lathe(tp + W * e.x + G * e.y + E * e.z, normalize(f), a, 0.013, t * length(f));
    }
    return tp + W * (a - 0.5) * 0.33 + G * (0.03 + 0.4 * t) + E * (0.015 + 0.01 * sin(3.14159 * a));
}

void main(){
    // The sun's shadow map gets a grid half as fine.
    int k = gl_InstanceID, n = 72 >> int(uQ), q = gl_VertexID / 6, c = gl_VertexID % 6;
    vec2 u = vec2(c == 1 || c == 2 || c == 4, c == 2 || c == 4 || c == 5), g = (vec2(q % n, q / n) + u) * float(72 / n);
    float th = g.x * 0.0872665, ph = g.y * 0.0654498 - 1.5708;
    vec3 o = vec3(cos(ph) * cos(th), sin(ph), cos(ph) * sin(th));
    if (k > 5) {
        // A drop of spray, thrown up and out from where the blob landed in the
        // water. Harder landings throw more drops, and throw them faster: up to
        // about 2,800, each 1-6 cm across, since bigger drops look like white
        // beads up close. Each flies until it falls back to the water or sand.
        // Drops are never drawn smaller than 1.5 pixels. A smaller drop is drawn
        // at 1.5 px with lower opacity instead, dithered, and the TAA smooths it.
        vec4 p = uS[k - 6], h = fract(sin(float(q) * vec4(12.9898, 78.233, 37.719, 93.989)) * 43758.5453);
        float t = p.z, a = h.y * 6.2832, v = (0.7 + 2.6 * h.z * h.z) * sqrt(p.w),
              z0 = s3h(p.xy) + 0.03, sz = mix(0.005, 0.03, h.z * h.w * h.w);
        vec3 X = vec3(p.x, z0, p.y) + vec3(cos(a), 0.0, sin(a)) * (R * 0.85 + (0.75 - 0.4 * h.w) * v * t)
               + vec3(0.0, mix(0.5, 1.4, h.w) * v * t - 4.9 * t * t, 0.0),
             W = vec3(cos(a) * (0.75 - 0.4 * h.w) * v, mix(0.5, 1.4, h.w) * v - 9.8 * t, sin(a) * (0.75 - 0.4 * h.w) * v),
             V = normalize(uCam - X), Dy = normalize(W - V * dot(W, V) + vec3(0.0, 1e-3, 0.0)), Dx = cross(V, Dy);
        // Each drop is stretched along its flight by the distance it covers in
        // a 60th of a second, with its coverage spread over the streak. Round
        // beads all of one size looked like bubbles, not spray.
        float r = max(sz, length(uCam - X) * 1.5 / uRes.y), e = length(W) * 0.016 / r,
              al = sz * sz / (r * r) * (1.0 - smoothstep(0.7, 1.2, t)) / (1.0 + e);
        if (h.x > 0.8 * p.w || X.y < min(z0, s3h(X.xz)) - 0.02) al = 0.0;
        vN = Dx * (u.x * 2.0 - 1.0) + Dy * (u.y * 2.0 - 1.0);
        vW = X + (vN + Dy * (u.y * 2.0 - 1.0) * e) * r;
        vO = vec3(u * 2.0 - 1.0, al);
        vK = 3;
        gl_Position = al > 0.0 ? uViewProj * vec4(vW, 1.0) : vec4(2.0);
        return;
    }

    // The hops land on the beat, run by run (s3_blob.c). Each one has a
    // wind-up, a stretch in the air and a squash on landing, which is softer
    // for the little hops.
    j = uTime - s3j0 - 7.7419355;
    float f = fract(uH.x), hop = step(0.0, uH.x), air = hop * clamp((f - 0.22) / 0.53, 0.0, 1.0);
    float lift = uH.y * sin(3.14159 * air), st = uB.w;
    float s = 1.0 + 0.02 * sin(j * 2.4)                                   // breathing
            + hop * min(sqrt(uH.y * 5.0), 1.0) * (0.14 * sin(3.14159 * air) * (1.0 - air) - 0.2 * bump(f, 0.0, 0.24) - 0.24 * bump(f, 0.72, 1.0))
            + 0.07 * sin(st * 17.0) * exp(-st * 4.0)                      // settling
            - 0.05 * smoothstep(91.8, 92.6, j);                           // sitting to watch the sea
    float shiver = (bump(j, 65.0, 65.7) + bump(j, 90.55, 91.25)) * sin(j * 70.0);   // it shivers as it rises, and again on the dry sand (no spray there)
    float gy = s3h(uB.xz), sea = 1.0 - smoothstep(-1.2, -0.9, gy);

    // Its frame faces the land and turns as the hops turn it. On top of that
    // come a look around, a tilt of the head while it waves, and another tilt
    // at the end.
    vec3 F0 = -vec3(s3sea.x, 0.0, s3sea.y);
    float ya = uH.w + 0.55 * (bump(j, 71.6, 72.6) - bump(j, 72.4, 73.4)),
          rl = 0.16 * (smoothstep(73.4, 74.0, j) - smoothstep(77.0, 77.5, j)) + 0.1 * smoothstep(93.6, 94.6, j);
    F = F0 * cos(ya) + vec3(-F0.z, 0.0, F0.x) * sin(ya);
    Rt = normalize(cross(F, vec3(0.0, 1.0, 0.0)));
    U = vec3(0.0, cos(rl), 0.0) + Rt * sin(rl);
    Rt = cross(U, F) * -1.0;
    C = uB.xyz - F0 * uH.z * hop * (smoothstep(0.22, 0.75, f) - f)
      + vec3(0.0, lift - R * (1.0 - s) + 0.03 * sea * sin(j * 2.7), 0.0);
    S = vec3((1.0 + 0.06 * shiver) / sqrt(s), s, (1.0 - 0.06 * shiver) / sqrt(s));   // squash, keeping the volume

    // This side's arm. It hangs down and out, paddles in the water and swings
    // up for balance on every hop. The right arm waves while the left one
    // cheers. Out of the water the arms hold what the blob carries, and both
    // go up as the last wave laps at it.
    float sd = (k & 1) == 1 ? -1.0 : 1.0;
    vec3 A = vec3(0.55, -0.75, 0.25) + vec3(0.0, 0.08 * sin(j * 2.0 + sd), 0.0);
    A = mix(A, vec3(0.75, 0.1 + 0.45 * sin(j * 9.0 + sd * 1.5), 0.35), sea * step(j, 66.0));
    A = mix(A, vec3(0.8, 0.5, 0.1), sin(3.14159 * air)) + hop * bump(f, 0.0, 0.24) * vec3(-0.2, 0.0, -0.5);
    A = mix(A, sd > 0.0 ? vec3(0.55 + 0.35 * sin(j * 10.0), 0.85, 0.25) : vec3(0.85, 0.45, 0.3), smoothstep(73.5, 74.0, j) - smoothstep(77.1, 77.5, j));
    if (j > 85.4) A = sd > 0.0 ? vec3(0.5, 0.35, 0.55) : vec3(0.85, 0.45, 0.1);
    A = mix(A, vec3(0.7, 0.75 + 0.1 * sin(j * 8.0 + sd), 0.25), bump(j, 92.9, 94.6));
    A = normalize(Rt * sd * A.x + U * A.y + F * A.z);
    sh = C + (Rt * sd * 0.86 * S.x + U * -0.06 * S.y + F * 0.18 * S.z) * R;
    tp = sh + A * 0.41 * R;

    vec3 P, N;
    vK = 0;
    if (k < 2) {
        // A prop (see prop()). The grid's rows are split into parts: the
        // canopy, pole and hat, or the chair's six tubes and its canvas. Each
        // part's normal is taken across its own surface.
        int r = q / n * (72 / n), p = k < 1 ? (r < 22 ? 0 : r < 26 ? 22 : 26) : min(r & 62, 12);
        float a = g.x / 72.0, t = clamp((g.y - float(p)) / float(k < 1 ? (r < 22 ? 22 : r < 26 ? 4 : 22) : p < 12 ? 2 : 6), 0.002, 0.996);
        if (j < 85.4 || k * r > 17) { gl_Position = vec4(2.0); return; }
        P = prop(k, p, a, t);
        o = vec3(a, t, 0.0);
        N = normalize(cross(prop(k, p, a + 0.002, t) - P, prop(k, p, a, t + 0.002) - P));
        vK = k < 1 ? (p < 1 ? 5 : p < 26 ? 6 : 4) : p < 12 ? 6 : 7;
        // The splash throws it up out of the water, from under the surf in
        // front of the blob, and the blob catches it.
        a = smoothstep(85.4, 85.85, j);
        P += (1.0 - a) * (F * 0.5 - vec3(0.0, 1.7, 0.0)) + vec3(0.0, 1.2 * a * (1.0 - a), 0.0);
    } else if (k < 4) {
        // Half of the jelly, on this arm's side. Rays start inside the
        // shoulder, which is inside both the body and the arm, so each ray
        // crosses the surface exactly once. They fan out from the outward
        // normal there to the seam down the middle, where the other half's
        // rays meet them at the same points.
        vec3 Q = C + (U * cos(th) * S.y + F * sin(th) * S.z) * R,
             D0 = normalize(sh - C), D1 = normalize(Q - sh);
        float w = acos(dot(D0, D1)), v = g.y / 48.0, t0 = 0.0, t1 = 2.6 * R;
        vec3 D = (sin((1.0 - v) * w) * D0 + sin(v * w) * D1) / sin(w);
        for (int i = 0; i < 15; ++i) {
            float t = 0.5 * (t0 + t1);
            if (jelly(sh + D * t) < 0.0) t0 = t; else t1 = t;
        }
        P = g.y > 47.5 ? Q : sh + D * t0;
        vec2 e = vec2(0.001, -0.001);
        N = normalize(e.xyy * jelly(P + e.xyy) + e.yyx * jelly(P + e.yyx) + e.yxy * jelly(P + e.yxy) + e.xxx * jelly(P + e.xxx));
        vec3 l = P - C;
        o = normalize(vec3(dot(l, Rt), dot(l, U), dot(l, F)) / S);
        P += N * 0.008 * sin(dot(o, vec3(3.0, 4.0, 2.0)) + j * 4.0);
        if (P.y < gy + 0.01) { P.y = gy + 0.01; N = vec3(0.0, -1.0, 0.0); }
    } else {                                        // an eye: a dark glossy lens set in the face
        vec3 e = normalize(vec3(sd * 0.30, 0.40, 0.80));
        vec3 ez = normalize(Rt * e.x / S.x + U * e.y / S.y + F * e.z / S.z);
        vec3 ex = normalize(cross(U, ez)), ey = cross(ez, ex);
        float open = 1.0 - 0.94 * (bump(j, 70.8, 71.05) + bump(j, 74.5, 74.75) + bump(j, 76.6, 76.85) + bump(j, 83.2, 83.45) + bump(j, 94.6, 94.85));
        vec3 l = vec3(0.26, 0.26 * open, 0.117) * R;
        P = C + (Rt * e.x * S.x + U * e.y * S.y + F * e.z * S.z) * R * 0.98 + ex * o.x * l.x + ey * o.y * l.y + ez * o.z * l.z;
        N = normalize(ex * o.x / l.x + ey * o.y / (l.y + 0.001) + ez * o.z / l.z);
        vK = 2;
    }
    vW = P;
    vN = N;
    vO = o;
    gl_Position = uViewProj * vec4(P, 1.0);
}
