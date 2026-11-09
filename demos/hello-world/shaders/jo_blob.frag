#version 450 core
// The blob on the beach. It's a glossy, translucent blue jelly lit by the
// jungle's lighting model, with the sea and the sky behind it bent through
// its body. It glows from within where the sun comes through, and its coat
// mirrors the sky. It's wet, with water streaming off it, when it comes out
// of the sea and again out of the swash, and while it's still in the sea it's
// drawn under the surf. The eyes are glossy black lenses that catch the sky,
// and each drop of its splashes is a tiny lens of sea water. This shader also
// draws what it carries out of the water: a straw hat, a striped parasol and
// a deckchair.
in vec3 vW;
in vec3 vN;
in vec3 vO;
flat in int vK;
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
layout(binding = 9) uniform sampler2D uScene;   // the finished picture behind it (main.c's jelly_copy)
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
void main(){
    // Its colour in the sun's map (uSunT) marks what lets the sun through.
    // That's the jelly, and the parasol's cloth at half as much (0.15 of the
    // sun), because the shade under a beach parasol is a light one.
    if (uQ > 0.5) { o = vec4(vK < 4 ? 1.0 : vK == 5 ? 0.5 : 0.0); return; }
    float j = uTime - s3j0 - 7.7419355;   // its cues wait out the hold for the T-rex (S3_HOLD)
    vec3 V = normalize(uCam - vW);
    if (vK == 3) {
        // A drop is a dithered disc. It refracts (and flips) what's behind it,
        // reflects the sky at its rim and catches a glint of the sun. Tiny drops
        // (under 1.5 px, vO.z < 1) are mostly white mist. Big drops are clear,
        // with only a faint sky reflection at the rim, since a darker middle
        // inside a bright ring looked like a soap bubble. Drops are at most 0.7
        // opaque. The background copy is taken before the blob is drawn, so a
        // solid drop over the jelly would show the sea in place of the jelly.
        float r2 = dot(vO.xy, vO.xy);
        if (r2 > 1.0 || fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233)) + uTime * 61.0) * 43758.5) > min(vO.z, 0.7)) discard;
        vec3 N = normalize(vN + V * sqrt(1.0 - r2)), R = reflect(-V, N);
        vec4 q = uViewProj * vec4(vW - N * 0.4, 1.0);
        float F = 0.02 + 0.98 * pow(1.0 - dot(N, V), 5.0);
        vec3 c = texture(uScene, q.xy / q.w * 0.5 + 0.5).rgb * (1.0 - F) + s3sky(R) * F * 0.4
               + uGlow * pow(max(dot(R, uLightDir), 0.0), 120.0) + (0.3 - 0.2 * vO.z) * (uGlow * max(uLightDir.y, 0.0) + (uTop + uBottom) * 0.3);
        o = vec4(s3fog(c, vW), 1.0);
        return;
    }
    vec3 N = normalize(vN), R = reflect(-V, N);
    float nv = max(dot(N, V), 0.0), F = 0.04 + 0.96 * pow(1.0 - nv, 5.0);
    vec3 env = s3sky(R) * mix(0.3, 1.0, smoothstep(-0.15, 0.05, R.y));
    vec3 c;
    if (vK > 3) {
        // What it carries is lit like the rest of the world and drawn with it,
        // before the sky, so the jelly bends it too. The hat is straw braided
        // in rows around it, with a groove between rows and each row its own
        // shade, under a navy band. The cloth is red and white and the canvas
        // yellow and white. Seen from the shaded side, the sun glows through
        // the cloth, the canvas and the straw. The wood is varnished.
        if (vK == 5 && vO.y > 0.94 + 0.06 * sqrt(1.0 - pow(fract(vO.x * 16.0) * 2.0 - 1.0, 2.0))) discard;   // the valance's scallops
        N = faceforward(N, -V, N);
        float b = fract(vO.y * 40.0);
        vec3 alb = vK == 5 ? (fract(vO.x * 4.0) < 0.5 ? vec3(0.72, 0.05, 0.04) : vec3(0.8, 0.78, 0.72))
                 : vK == 7 ? (fract(vO.x * 3.5) < 0.5 ? vec3(0.85, 0.6, 0.05) : vec3(0.8, 0.78, 0.72))
                 : vK == 6 ? vec3(0.42, 0.26, 0.14)
                 : vO.y > 0.34 && vO.y < 0.45 ? vec3(0.02, 0.025, 0.05)
                 : vec3(0.86, 0.66, 0.34) * (0.9 + 0.2 * fract(sin(floor(vO.y * 40.0) * 78.233) * 43758.5))
                   * (0.55 + 0.45 * smoothstep(0.0, 0.15, b) * smoothstep(1.0, 0.85, b));
        c = s3light(vW, N, alb, vK == 6 ? 0.4 : 0.7, 0.0, 1.0) + alb * uGlow * s3sh * max(-dot(N, uLightDir), 0.0) * (vK == 6 ? 0.0 : 0.35);
    } else if (vK == 2)                             // an eye
        c = s3light(vW, N, vec3(0.008, 0.008, 0.014), 0.05, 0.0, 1.0) + env * (0.35 * F + 0.006) * (0.4 + smoothstep(0.88, 0.97, R.y) * 3.0);
    else {
        float wet = 1.0 - 0.5 * min(smoothstep(66.0, 76.0, j) * (1.0 - smoothstep(80.3, 80.7, j)) + smoothstep(89.0, 99.0, j), 1.0);
        vec3 alb = vec3(0.15, 0.4, 1.0);            // the blue of scenes 1 and 2
        // The coat gets diffuse light and a sharp sun highlight. On top go the
        // soft glow it had in scenes 1 and 2, brightest where it faces us, and
        // its pale glowing edge. The sunlight it gathers glows through it and
        // out of its far side, and the wet skin mirrors the sky.
        c = (s3light(vW, N, alb * 0.45, mix(0.3, 0.08, wet), 0.0, 1.0)
          + alb * (0.35 + 0.55 * nv) * 0.55 + vec3(0.49, 0.64, 1.0) * pow(1.0 - nv, 2.0) * 1.1
          + alb * uGlow * 0.1 * (0.3 + pow(max(dot(-V, uLightDir), 0.0), 3.0)) * (0.3 + 0.7 * (1.0 - nv))
          + uGlow * vec3(0.1, 0.45, 1.0) * 0.15 * pow(max(dot(N, -uLightDir), 0.0), 3.0)) * 0.6
          + env * F * (0.5 + 0.5 * wet);
        // The jelly. The sea, sand and sky behind it are bent through the ball,
        // each colour a little differently, and seen through its blue, which
        // absorbs more of the reds and greens the thicker it is. Thicker parts
        // are also blurred more, by a three-tap vertical tent filter. The ball
        // flips a tall stretch of the long lens's streaky sea and squeezes it
        // into itself, and without the blur it looked striped.
        vec3 bg;
        for (int i = 0; i < 3; i++) {
            vec4 q = uViewProj * vec4(vW + refract(-V, N, 0.85 - 0.01 * float(i)) * nv * 1.1, 1.0);
            vec2 u = q.xy / q.w * 0.5 + 0.5, e = vec2(0.0, 0.02 * nv);
            bg[i] = (texture(uScene, u + e) + 2.0 * texture(uScene, u) + texture(uScene, u - e))[i] * 0.25;
        }
        c += bg * exp(-(1.0 - alb) * nv * 2.5) * (0.8 - F);
        // sea water runs off it in drips as it comes out of the sea, and again
        // out of the swash
        float a = atan(vO.z, vO.x) * 3.0, h = fract(sin(floor(a) * 78.233) * 43758.5), y = 1.0 - fract(j * 0.5 + h) * 2.4;
        c += env * 0.4 * step(0.5, h) * smoothstep(0.8, 1.0, 1.0 - 2.0 * abs(fract(a) - 0.5))
           * step(y, vO.y) * (1.0 - smoothstep(y, y + 0.45, vO.y)) * (1.0 - smoothstep(66.5, 69.5, j) + smoothstep(88.4, 89.0, j) - smoothstep(90.6, 92.6, j));
    }
    // Under the surf, the blob is seen the way jw_sea sees the seabed: through
    // the water along the view, which absorbs the reds first and scatters its
    // own blue-green, under a surface that mirrors the sky at grazing angles.
    // There's a ring of foam where the swell laps at it.
    float y = jws(vW.xz, 0.5).x - 1.0;
    if (vW.y < y && dot(vW.xz, s3sea) > jwr(vW.xz).x) {
        vec3 lit = uGlow * max(uLightDir.y, 0.0) + (uTop + uBottom) * 0.3;
        float f = 0.02 + 0.98 * pow(1.0 - V.y, 5.0);
        c = mix(mix(vec3(0.015, 0.16, 0.17) * lit, c, exp(-vec3(0.84, 0.15, 0.11) * (y - vW.y) / max(V.y, 0.1))),
                s3sky(V * vec3(-1.0, 1.0, -1.0)), f);
        c = mix(c, vec3(0.85) * lit, 0.6 * smoothstep(0.06, 0.0, y - vW.y));
    }
    // Alpha 1 makes the post pass's contact shadows skip the jelly and its
    // spray. Their march took the drops and the ball for solid blockers and
    // blackened a patch of the jelly at each splash, and a speck under each arm.
    o = vec4(s3fog(c, vW), vK > 3 ? s3a : 1.0);   // props pass their ambient share, like the rest of the world
}
