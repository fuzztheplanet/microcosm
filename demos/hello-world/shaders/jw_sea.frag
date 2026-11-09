#version 450 core
// The sea's surface: what it reflects (the sky and the sun), what the water
// scatters (turquoise over the sand, deep blue farther out, milky in the
// surf, green through a wave's lip) and the white water. That's the breakers,
// the foam they leave, the lace at the edge of each swash, and the froth
// around the blob where it breaks the surface and where it splashes. Output 0
// is added to the scene and output 1 multiplies what lies beneath, so the
// seabed is seen through the water's per-channel transmittance.
in vec3 vW;
layout(location = 0, index = 0) out vec4 o;
layout(location = 0, index = 1) out vec4 oT;
layout(depth_greater) out float gl_FragDepth;   // only ever pushed further, so the early depth test still holds
layout(location = 0) uniform vec4 uB;      // the blob: centre, radius
layout(location = 2) uniform vec4 uS[2];   // its last two splashes: position (x, z), how long ago, how hard
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
// ---- surf: keep byte-identical in jw_sea, ju_ground, jo_blob (owner: s3_beach) ----
// The swell, shared by the sea, the sand and the blob. A wave comes in every
// 7 s and slows as the sand shelves up under it (c = sqrt(g d)). Crests 76 m
// apart far out bunch up to about 9 m apart at the waterline, and they bend
// round to face the beach. jwph() is the swell's phase, with crests at
// 0 mod 2 pi, rising seaward. x is metres out past the waterline (at 50 m)
// and a is the position along the beach. The waves keep time with the blob's
// play, so they also wait out the hold for the T-rex (S3_HOLD, 7.74 s).
float jwph(vec2 p){
    float x = max(dot(p, s3sea) - 50.0, 0.0), a = dot(p, vec2(-s3sea.y, s3sea.x));
    return 0.9 * uTime - 6.9677419 + (x < 198.0 ? 2.347 * sqrt(x + 3.0) : 0.0827 * x + 16.91) + 0.02 * a + 0.6 * sin(0.031 * a + 0.5 * sin(0.013 * a));
}
// The size of the wave at phase t. Waves come in sets.
float jwg(float t){ return 0.8 + 0.2 * sin(t * 0.16) + 0.12 * sin(t * 0.43 + 2.0); }
// The swash at p. Returns how far up the sand the water reaches now and how
// far this wave's sheet runs (both in metres out from the forest), and the
// fraction of a period since its bore reached the waterline. Each spent
// wave's sheet runs up the slope and drains back, and bigger waves run
// farther, in lobes along the beach.
vec3 jwr(vec2 p){
    float a = dot(p, vec2(-s3sea.y, s3sea.x)), q = jwph(p - s3sea * (dot(p, s3sea) - 50.0)) / 6.2832, n = floor(q), f = q - n,
          m = 49.8 - jwg(n * 6.2832) * (3.0 + 0.5 * sin(0.4 * a + n) + 0.3 * sin(1.1 * a - 2.0 * n));
    return vec3(mix(49.8, m, smoothstep(0.0, 0.25, f) * (1.0 - smoothstep(0.25, 0.9, f))), m, f);
}
// The film a draining sheet leaves on the sand, from its highest reach down
// to the retreating edge. It soaks in before the next sheet arrives.
float jwfilm(vec2 p){
    vec3 r = jwr(p);
    return smoothstep(r.y - 0.1, r.y + 0.4, dot(p, s3sea)) * smoothstep(0.2, 0.3, r.z) * (1.0 - smoothstep(0.3, 1.0, r.z));
}
// The swell at p. Returns the height over sea level, how far past breaking
// the wave is (its height minus 0.78 of the depth), its phase and the wave's
// height. A wave grows as the water gets shallower (Green's law) and is
// higher in some places along its crest. Before it breaks, its crest sharpens
// and its face steepens by m (0.5 is the sharpest the sea's grid can follow).
// It breaks once it's 0.78 of the depth, first at those high places and over
// the bars, so the break starts at its peaks and peels along. After that it
// runs in as a bore half the depth high, with a long back and a steep front.
vec4 jws(vec2 p, float m){
    float t = jwph(p), u = fract(t / 6.2832), s = dot(p, s3sea), a = dot(p, vec2(-s3sea.y, s3sea.x)),
          d = max(0.06 * (s - 50.0) * (1.0 + 0.2 * sin(0.045 * a + 1.0)), 0.0),
          H = 0.3 * jwg(t) * (1.0 + 0.3 * sin(0.13 * a + 0.33 * t) * (0.6 + 0.4 * sin(0.051 * a - 0.21 * t))) * pow(10.0 / (d + 0.3), 0.25),
          x = H - 0.78 * d, b = smoothstep(0.0, 0.1, x), k = m * smoothstep(0.3, 1.0, H / (0.78 * d + 0.01)), c = 6.2832 * u;
    H = mix(H, 0.5 * d + 0.03, b);
    return vec4(H * (mix(pow(0.5 + 0.5 * cos(c + 0.8 * k * cos(c)), 1.0 + 3.0 * k), mix(1.0 - u, 1.0, smoothstep(1.0 - 0.08 / m, 1.0, u)), b) - 0.3), x, t, H);
}
// The seabed out past the surf is sand with dark meadows of seagrass. This
// returns how much seagrass there is at p (0..1).
float jwbed(vec2 p){
    return smoothstep(0.35, 0.7, sin(0.11 * p.x + 1.3 * sin(0.07 * p.y)) * sin(0.13 * p.y + 1.1 * sin(0.09 * p.x)) + 0.3 * sin(0.37 * p.x + 0.29 * p.y))
         * smoothstep(55.0, 62.0, dot(p, s3sea));
}
// ---- end surf ----
float vn(vec2 p){                       // value noise, 0..1
    vec2 i = floor(p), f = fract(p);
    f *= f * (3.0 - 2.0 * f);
    vec4 h = fract(sin(vec4(0.0, 1.0, 57.0, 58.0) + dot(i, vec2(1.0, 57.0))) * 43758.5);
    return mix(mix(h.x, h.y, f.x), mix(h.z, h.w, f.x), f.y);
}
// The wind's waves on top of the swell: 17 trains from 18 m down to 8 cm,
// each moving at its own deep-water speed (w^2 = g k). The short ones are a
// little steeper. Their headings step by the golden angle, folded to within
// 90 degrees of the onshore breeze. They're short-crested, with each crest
// wandering along its length, and the short ones are stirred up in gusty
// patches (g). A train too fine for the pixel, whose phase turns more than
// about 1.5 rad across the pixel's footprint (dx, dy), gives up its slope to
// jwv, the variance of the slopes the pixel covers. jwu is the same for the
// four longest trains.
float jwv, jwj, jwa, jwu, jwl;
vec2 jwd;
vec2 jwc(vec2 p, vec2 dx, vec2 dy, float g, float z){
    vec2 r = vec2(0.0), d = -s3sea;
    float k = 0.35, w = 1.853;
    jwv = 0.0002;
    jwj = jwa = jwl = 0.0;
    jwd = vec2(0.0);
    for (int i = 0; i < 17; i++) {
        float n = float(i), m = mix(0.045, 0.06, smoothstep(0.35, 2.0, k)) * z * mix(1.0, g, smoothstep(1.5, 6.0, k)),
              f = smoothstep(1.6, 0.8, k * (abs(dot(d, dx)) + abs(dot(d, dy)))), e = m * m * (0.5 - 0.5 * f * f);
        if (f > 0.0) r += d * m * f * cos(k * dot(d, p) - w * uTime + n * 1.7 + 1.5 * sin(0.23 * k * dot(vec2(-d.y, d.x), p) + n * 2.3));
        jwv += e;
        if (i == 3) jwu = jwv - 0.0002;
        d = mat2(-0.737, 0.675, -0.675, -0.737) * d;
        d *= -sign(dot(d, s3sea));
        k *= 1.4;
        w *= 1.1832;
    }
    // The four longest trains again, the wind sea proper, in a loop of their
    // own so that both loops unroll. This works out how high they stand (jwa)
    // and how far their crests run together (jwj: the Jacobian of the choppy
    // sea's sideways sway is 1 - jwj). It also gives how fast jwj changes
    // across the surface (jwd), and jwj as it was 0.8 s ago (jwl, for the
    // foam a crest leaves where it broke).
    d = -s3sea;
    k = 0.35;
    w = 1.853;
    for (int i = 0; i < 4; i++) {
        float n = float(i), m = mix(0.045, 0.06, smoothstep(0.35, 2.0, k)) * z, f = smoothstep(2.0, 1.0, k * (abs(dot(d, dx)) + abs(dot(d, dy)))),
              a = k * dot(d, p) - w * uTime + n * 1.7 + 1.5 * sin(0.23 * k * dot(vec2(-d.y, d.x), p) + n * 2.3), c = m * f * cos(a), h = m * f * sin(a);
        jwj += h;
        jwl += h * cos(0.8 * w) + c * sin(0.8 * w);
        jwd += c * k * d;
        jwa += h / k;
        d = mat2(-0.737, 0.675, -0.675, -0.737) * d;
        d *= -sign(dot(d, s3sea));
        k *= 1.4;
        w *= 1.1832;
    }
    return r;
}
void main(){
    vec2 p = vW.xz, dx = dFdx(p), dy = dFdy(p);     // the pixel's footprint, taken before any discard
    vec2 A = vec2(-s3sea.y, s3sea.x);
    // Where the view ray, carried on through the water, meets the floor (at tf
    // along it; the floor reaches about 80 m out). The depth written is the
    // surface's depth pushed on along the ray, by up to 1.2 m and never past
    // the floor. That keeps it smooth for the occlusion and the depth of
    // field, and leaves room under the surface for the blob's body, which is
    // drawn after.
    vec3 D = vW - uCam;
    float s = dot(p, s3sea), ac = dot(p, A), sc = dot(uCam.xz, s3sea), ds = dot(D.xz, s3sea), dn = D.y + 0.06 * ds,
          tf = dn < 0.0 ? ((50.0 - sc) * 0.06 - 1.0 - uCam.y) / dn : 1e4, sf = sc + ds * tf, e = jwr(p).x, cov = smoothstep(e, e + 0.15, s);
    vec4 z = uViewProj * vec4(uCam + D * min(1.0 + 1.2 / length(D), max(tf - 0.02 / length(D), 1.0)), 1.0);
    gl_FragDepth = z.z / z.w * 0.5 + 0.5;
    if (cov <= 0.0) discard;
    vec3 V = uCam - vW, L = uLightDir;
    float d = length(V), ep = 0.05 + d * 0.002;
    V /= d;
    // The surface: the swell's slope with the wind's waves on top. On the thin
    // sheet of the swash there are only ripples.
    vec4 w = jws(p, 0.5);
    float gs = (jws(p + s3sea * ep, 0.5).x - w.x) / ep,  // the swell's slope seaward: > 0 on a face toward the beach
          gu = 0.3 + 1.4 * vn(p * 0.03 + uTime * vec2(-0.05, 0.03));
    vec2 sl = s3sea * gs + jwc(p, dx, dy, gu, mix(0.25, 1.0, smoothstep(50.0, 53.0, s)));
    // the blob: rings run out from where it cuts the surface
    vec2 bq = p - uB.xz;
    float br = length(bq), cut = uB.w * uB.w - (vW.y - uB.y) * (vW.y - uB.y);
    float wet = smoothstep(-0.4, 0.0, cut) * (1.0 - smoothstep(-0.05, 0.05, uB.y - uB.w - vW.y));
    float rr = br - sqrt(max(cut, 0.0));
    sl -= bq / (br + 1e-3) * sin(14.0 * rr - 9.0 * uTime) * 0.3 * exp(-2.0 * max(rr, 0.0)) * wet;
    // Where it lands in the water, a ring runs out over the sheet and the
    // middle is churned white, and both soon fade. The white is patchy lace
    // and froth, its share of the foam held under 0.55. At full strength it
    // made one flat, hard-edged disc of the foam's sky-lit lilac.
    float sk = 0.0;
    for (int i = 0; i < 2; i++) {
        vec2 sq = p - uS[i].xy;
        float sd = length(sq), ra = 0.5 + 1.3 * sqrt(uS[i].z), sw = uS[i].w * exp(-1.6 * uS[i].z);
        sl -= sq / (sd + 1e-3) * sin(12.0 * (sd - ra)) * 0.25 * sw * exp(-3.0 * abs(sd - ra));
        sk += sw * (0.4 * smoothstep(0.4, 0.0, abs(sd - ra)) + 0.5 * exp(-2.0 * uS[i].z) * smoothstep(ra, 0.3, sd));
    }
    vec3 N = normalize(vec3(-sl.x, 1.0, -sl.y));
    // At grazing angles the facets facing the camera outweigh the rest, so on
    // average the pixel mirrors sky from higher up, and less of it.
    vec3 M = normalize(N + vec3(V.x, 0.0, V.z) * sqrt(jwv)), R = reflect(-V, M), H = normalize(L + V);
    float F = 0.02 + 0.98 * pow(1.0 - max(dot(M, V), 0.0), 5.0), q = min(1.0, 2.0 * jwv + 0.25 * (dot(dFdx(N), dFdx(N)) + dot(dFdy(N), dFdy(N)))), nh = max(dot(N, H), 0.0);
    vec3 lit = uGlow * max(L.y, 0.0) + (uTop + uBottom) * 0.3;     // the open sea is all in sun
    // What lies beneath shows through the water, which absorbs the reds
    // first. It's clear turquoise over the sand and deep blue where the bottom
    // is lost. A wave's face is a wall of green water you see through (ff is
    // how much it faces the camera). Then there's the water's own colour,
    // greener in the shallows, the light through the thin lip of a wave about
    // to break, and the shade under it.
    float b = smoothstep(0.0, 0.1, w.y), ff = clamp(gs * 8.0, 0.0, 1.0), dep = max(vW.y - s3h(p), 0.0),
          ct = sqrt(1.0 - (1.0 - V.y * V.y) * 0.565), f = w.x / max(w.w, 0.01) + 0.3, ki = smoothstep(-0.15, 0.0, w.y) * (1.0 - b);
    vec3 T = exp(-vec3(0.45, 0.07, 0.04) * ((dep + 0.3 * max(s - 80.0, 0.0)) * (1.0 + 1.0 / ct) + 8.0 * ff * w.w));
    vec3 c = (1.0 - F) * lit * (mix(vec3(0.004, 0.03, 0.06), vec3(0.01, 0.07, 0.065), smoothstep(80.0, 55.0, s)) * (1.0 - T)
                                + vec3(0.06, 0.5, 0.42) * 0.4 * ki * smoothstep(0.6, 0.95, f) * ff)
           * (1.0 - 0.5 * ki * ff * smoothstep(0.85, 0.5, f))
           + F * s3sky(R)
           // the sun's highlight, as broad as the hidden slopes spread it (GGX)
           + uGlow * q / (3.1416 * pow(nh * nh * (q - 1.0) + 1.0, 2.0)) * (0.02 + 0.98 * pow(1.0 - max(dot(H, V), 0.0), 5.0))
             * 0.25 / max(dot(N, V), 0.05) * step(0.0, dot(N, L));
    // White water: the roller on a breaking wave's face and crest, and the
    // foam it leaves. The foam tears into streaks along the beach that open
    // into lace and melt away over the next few seconds. There's also the
    // frothy edge of each swash, the collar around the blob and the froth it
    // churns up.
    // The pattern is warped noise, streaky and lumpy, with a net of ridges for
    // the lace. It fades into its mean as it shrinks below the pixel (fp is
    // its footprint), and the cut-off widens with it.
    vec2 fq = vec2(ac * 0.25, s * 0.7 + uTime * 0.5);
    float fp = max(length(vec2(0.25 * dot(dx, A), 0.7 * dot(dx, s3sea))), length(vec2(0.25 * dot(dy, A), 0.7 * dot(dy, s3sea)))),
          aw = mix(0.2, 0.5, smoothstep(0.2, 1.5, fp)), u = fract(w.z / 6.2832), n = 0.5, fo = 0.0;
    if (b + wet + sk + smoothstep(0.35, 0.0, s - e) > 0.0) {   // only where there can be any
        fq += 0.8 * vec2(vn(fq * 0.5 + 3.0), vn(fq * 0.5 + 7.0));
        n = 0.35 * vn(fq) + 0.3 * vn(fq * 2.1) + mix(0.2 * vn(fq * vec2(5.0, 4.3)) + 0.15 * vn(fq * vec2(13.0, 9.0)), 0.175, smoothstep(0.1, 0.4, fp));
        // Whitest where a wave has just broken, thinning as its bore runs in,
        // and stronger in some places along the beach and from wave to wave.
        // The front is ragged and lumpy, and its puffs tumble along with it.
        float r = u + 0.05 * (n - 0.5), fr = smoothstep(0.8, 0.97, r), pf = mix(vn(vec2(ac * 1.6, w.z * 1.5)), 0.5, smoothstep(0.3, 1.0, fp * 6.4)),
              ro = b * (1.0 - 0.55 * smoothstep(0.15, 0.6, w.y)) * (0.35 + 0.65 * smoothstep(0.3, 0.7, vn(vec2(ac * 0.06, w.z * 0.16))))
                     * (max(0.7 * fr * (0.5 + 0.5 * pf), 0.7 * exp(-3.0 * max(r, 0.0))) + 0.12)
                 + wet * (0.5 * smoothstep(0.3, 0.0, abs(rr)) + 0.3 * exp(-1.5 * max(rr, 0.0))) + 0.8 * smoothstep(0.35, 0.0, s - e) + min(sk, 0.55);
        n = mix(n, pf, 0.5 * fr);
        fo = smoothstep(1.0 - ro - aw, 1.0 - ro + aw, mix(n, mix(1.0 - abs(2.0 * vn(fq * 2.3) - 1.0), 0.5, smoothstep(0.15, 0.5, fp)), 0.35));
    }
    // the water churned white behind a breaker: milky, pale green
    c += (1.0 - F) * lit * vec3(0.25, 0.5, 0.45) * 0.15 * b * exp(-2.0 * u);
    // Out past the surf, light comes through the thin crests of the swell and
    // the wind sea. The higher a crest stands, the greener the water lit
    // through it, and the troughs get a deeper blue. It's linear in the
    // height, so what the pixel can't resolve averages out instead of
    // shimmering.
    c += (1.0 - F) * lit * (1.0 - T) * vec3(0.0, 0.08, 0.055) * (w.x + 0.5 * jwa) * smoothstep(54.0, 62.0, s);
    // Whitecaps. Where the wind sea's crests run together and pinch (their
    // Jacobian, 1 - jwj, falls to about 0.8), a crest spills into a lumpy
    // patch of white and leaves foam that fades over the next second (jwl).
    // There are more of them in the gusts. Each patch is drawn soft over what
    // the pixel spans (jwd across its footprint), so it grows and fades
    // instead of blinking on. Where the pixel is too coarse to show those
    // waves, it draws the chance of a whitecap instead, from their hidden
    // variance. That whitens the far sea a touch, the way its whitecaps would.
    float ga = abs(dot(jwd, dx)) + abs(dot(jwd, dy)), wt = 0.22 - 0.045 * gu, wd = 1.7 / sqrt(jwu + 4e-4 + 0.25 * ga * ga),
          wf = smoothstep(0.3, 1.0, 1.9 * max(length(dx), length(dy))),
          wc = (1.0 - b) * smoothstep(56.0, 64.0, s) * max(1.0 / (1.0 + exp(wd * (wt - jwj))), 0.45 / (1.0 + exp(wd * (wt - jwl))));
    if (wc > 0.001) fo = max(fo, wc * mix(smoothstep(0.25, 0.65, vn(vec2(ac, s) * 1.9 + vec2(0.0, uTime * 0.4))), 0.6, wf));
    // Past the floor's reach, the bottom is drawn here: wet sand lit the way
    // the floor lights it, faded in over the last 15 m and dropping away past
    // 80 m.
    float vb = smoothstep(60.0, 75.0, sf);
    c += (1.0 - F) * T * vb * mix(vec3(0.23, 0.2, 0.15), vec3(0.025, 0.04, 0.02), jwbed(p)) * lit;
    // Foam: the sunlit tops of its lumps are bright, and the hollows between
    // them are in shade, lit by the sky and the white around them to a pale
    // blue-grey.
    c = mix(c, 0.7 * (uGlow * max(dot(N, L), 0.0) * smoothstep(0.35, 0.65, n) + (uTop + uBottom) * 0.3 + uGlow * 0.08), fo);
    T *= (1.0 - F) * (1.0 - fo) * (1.0 - vb);
    // the air in between: the forest's haze, then sea air melting into the horizon
    c = s3fog(c, vW);
    T *= exp(-0.0008 * d);
    o = vec4(c * cov, 0.0);   // what lies under it keeps its alpha (the ambient share)
    oT = vec4(mix(vec3(1.0), T, cov), 1.0);
}
