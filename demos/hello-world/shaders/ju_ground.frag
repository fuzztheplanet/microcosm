#version 450 core
// The forest floor: damp humus under drifts of fallen leaves and twigs. The
// leaves come in three sizes, from fresh ochre to rotting brown with the odd
// red one, and each piece is cut out, veined, curled and stacked over the
// ones below it. Moss covers the rises and the ground round the trunks' feet,
// where the floor is also in shade. The detail fades to its average colour
// once a leaf shrinks below a pixel.
// Past the forest's edge the litter thins out onto the beach, where the sand
// is pale and wind-rippled and glints grain by grain. A wrack line of weed
// and shells marks the top of the waves' reach. Below it the sand is dark
// and wet, each swash leaves a glossy film on it, and there are caustics on
// the seabed under the shallows. The tyrannosaurus's three-toed prints appear
// as its feet come down, and the blob leaves a wet trail.
in vec3 vWorld;
in vec3 vN;
in float vNear;
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
layout(binding = 12) uniform sampler2D uSand;   // jt_bake.frag's sand: (ripples, grain tone, specks, -), 2 m a tile
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

vec4 h4(vec2 p){
    vec4 q = fract(p.xyxy * vec4(.1031, .1030, .0973, .1099));
    q += dot(q, q.wzxy + 33.33);
    return fract((q.xxyz + q.yzzw) * q.zywx);
}
layout(binding = 1) uniform sampler3D uNoise;   // 32^3 random bytes, wrapping (main.c's noise_make)
// Value noise, 0..1. The hardware blends the lattice; each cell's fraction
// goes through the cubic first.
float vn3(vec3 p){ vec3 i = floor(p), f = p - i; return texture(uNoise, (i + f * f * (3.0 - 2.0 * f) + 0.5) / 32.0).r; }
float vn(vec2 p){ return vn3(vec3(p, 0.0)); }    // in 2D: the volume's first layer

// One scattered layer of litter in cells of sc metres. Each cell holds one
// leaf (now and then a twig instead), centred in the cell's middle half and
// at most 3/4 of a cell long, so only the four nearest cells can cover a
// point. Keeps the topmost piece in c (albedo, stacking height) and its
// surface in n (normal tilt xz, inside-ness, wetness).
void litter(vec2 w, float sc, float ty, inout vec4 c, inout vec4 n){
    vec2 p = w / sc, g = floor(p - 0.5);
    for (int j = 0; j < 4; j++) {
        vec2 cell = g + vec2(j & 1, j >> 1);
        vec4 h = h4(cell + ty * 41.0);
        float an = h.z * 6.283, cs = cos(an), sn = sin(an), L = 0.42 + 0.33 * h.w;
        bool twig = fract(h.w * 13.0) > 0.92 - 0.03 * ty;
        vec2 q = p - cell - 0.25 - 0.5 * h.xy;
        q = vec2(cs * q.x + sn * q.y, cs * q.y - sn * q.x);
        float x = q.x / L;
        float wd = L * (twig ? 0.04 : 0.2 + 0.2 * fract(h.y * 7.0)) * pow(max(1.0 - x * x, 1e-4), 0.6) * (1.0 - 0.3 * x);
        float e = 1.0 - abs(q.y) / wd, ht = ty + h.z;
        if (e > 0.0 && ht > c.w) {
            vec3 col = mix(mix(vec3(0.10, 0.05, 0.022), vec3(0.20, 0.12, 0.05), h.x * h.x),
                           vec3(0.045, 0.03, 0.018), smoothstep(0.3, 1.0, h.y));
            if (fract(h.w * 31.0) > 0.94) col = vec3(0.34, 0.09, 0.03);              // a red one
            if (twig) col = vec3(0.09, 0.07, 0.05);
            col *= (0.75 + 0.5 * fract(h.x * 17.0)) * (0.75 + 0.25 * smoothstep(0.0, 0.25, e));
            col *= 1.0 - 0.18 * step(fract(x * 7.0 + abs(q.y) / wd * 2.0), 0.12);   // veins
            col *= 1.0 + 0.3 * smoothstep(0.93, 1.0, e);                           // midrib
            c = vec4(col, ht);
            n = vec4((vec2(cs, sn) * (h.y - 0.5) + vec2(-sn, cs) * (-q.y / wd)) * 0.45, e, h.y);
        }
    }
}

// Value noise with its gradient: (value 0..1, d/dx, d/dy).
vec3 vnd(vec2 p){
    vec2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f), du = 6.0 * f * (1.0 - f);
    vec4 h = fract(sin(vec4(0.0, 1.0, 57.0, 58.0) + dot(i, vec2(1.0, 57.0))) * 43758.5);
    float k = h.x - h.y - h.z + h.w;
    return vec3(h.x + (h.y - h.x) * u.x + (h.z - h.x) * u.y + k * u.x * u.y, du * (vec2(h.y - h.x, h.z - h.x) + k * u.yx));
}
// A tile at u with its repeats hidden (iq's "texture repetition", way 3). k is
// a slow noise x 8, and picks one of eight randomly shifted copies of the tile.
// Where two copies meet, the relief (red) of one rises through the other's.
vec4 tnt(sampler2D t, vec2 u, vec2 x, vec2 y, float k){
    vec4 a = textureGrad(t, u + sin(vec2(3.0, 7.0) * floor(k)), x, y), b = textureGrad(t, u + sin(vec2(3.0, 7.0) * floor(k + 1.0)), x, y);
    return mix(a, b, smoothstep(0.3, 0.7, fract(k) + 0.3 * (b.r - a.r)));
}
// One three-toed print in its own frame (x along the stride, in metres): a
// heel pad and three tapering, clawed toes. Returns how far the sand is
// pressed in, 0..1.
float jwprint(vec2 q){
    float m = smoothstep(0.15, 0.07, length(q * vec2(1.0, 1.25)));
    for (int i = -1; i < 2; i++) {
        float a = float(i) * 0.42, L = 0.5 - 0.12 * abs(float(i));
        vec2 t = vec2(cos(a) * q.x + sin(a) * q.y, cos(a) * q.y - sin(a) * q.x);
        m = max(m, smoothstep(0.015, -0.015, abs(t.y) - 0.065 * (1.0 - 0.7 * t.x / L))
                 * smoothstep(-0.02, 0.05, t.x) * smoothstep(L, L - 0.06, t.x));
    }
    return m;
}

void main(){
    vec2 w = vWorld.xz;
    vec3 N = normalize(vN);
    float near = vNear, s = dot(w, s3sea);
    float f = length(fwidth(w));
    vec2 sx = dFdx(w) * 0.5, sy = dFdy(w) * 0.5;     // the sand tile's footprint (taken outside any branch)
    float bz = smoothstep(32.0, 35.5, s + 2.0 * vn(w * 0.3) - 1.0 + 0.6 * vn(w * 3.0));   // how much is beach
    // Subpixel grains and moss fibres average to their mean instead of
    // changing contrast at every camera step.
    vec4 c = vec4(vec3(0.05, 0.034, 0.02) * (0.6 + 0.8 * mix(h4(floor(w * 50.0)).x, 0.5, smoothstep(0.008, 0.03, f))), -1.0), n = vec4(0.0, 0.0, 0.0, 0.3);
    if (f < 0.12 && bz < 1.0) {
        if (f < 0.04) litter(w, 0.10, 0.0, c, n);
        litter(w, 0.17, 1.0, c, n);
        litter(w, 0.26, 2.0, c, n);
    }
    float lod = smoothstep(0.03, 0.12, f);
    vec3 alb = mix(c.rgb, vec3(0.10, 0.06, 0.03), lod);
    float ao = mix(mix(0.55, 1.0, smoothstep(0.0, 0.3, n.z)), 0.8, lod);
    // moss: on the rises, and thick round the trunks' feet
    float m = smoothstep(0.62, 0.85, vn(w * 0.4) * 0.6 + 0.3 * vn(w * 3.0) + near * 0.8) * (1.0 - bz);
    if (m > 0.0) alb = mix(alb, vec3(0.05, 0.09, 0.015) * (0.4 + 1.2 * mix(vn(w * 45.0), 0.5, smoothstep(0.01, 0.05, f))), m * vn(w * 7.0));
    float sand = bz * (1.0 - step(fract(c.w * 7.31), 1.0 - bz));            // the odd leaf still lies on it
    N = normalize(N + vec3(n.x, 0.0, n.y) * (1.0 - lod) * (1.0 - m) * (1.0 - sand));
    ao = mix(ao * (1.0 - 0.5 * near), 1.0, sand);
    float rough = mix(mix(0.3, 0.75, n.w), 0.9, m);
    vec3 add = vec3(0.0);
    float ad = 0.0;                                   // the sky the wet sand mirrors: ambient light
    if (bz > 0.0) {
        vec3 rw = jwr(w);
        float e = rw.x, j = uTime - s3j0, acr = dot(w, vec2(-s3sea.y, s3sea.x));
        // Moist sand is the lower beach and the sand the sheet has just left.
        // The film is what the sheet leaves as it drains, a mirror until it
        // soaks in.
        float dry = 1.0 - max(smoothstep(44.6, 46.6, s + 0.8 * vn(w * 0.4)), smoothstep(e - 1.5, e, s)),
              film = jwfilm(w) * step(s, e);
        float ba = 63.0 - 5.0 * smoothstep(71.613, 73.548, j) - 12.0 * clamp((j - 73.548) / 4.355, 0.0, 1.0);   // the blob's wet trail (s3_blob_at)
        dry *= 1.0 - (1.0 - smoothstep(0.2, 0.32, abs(acr - 1.5))) * step(ba, s) * step(s, 50.5);
        // The baked sand, 2 m per tile, with its repeats hidden. Within 8 m, a
        // short march along the view into the ripples' 1.5 cm depth finds where
        // the eye's ray meets them.
        vec2 su = w * 0.5, du = (vWorld.xz - uCam.xz) / max(uCam.y - vWorld.y, 0.3) * 0.002 * dry;
        float sk = vn(w * 0.07) * 8.0;                // which of its copies lies where
        vec4 g = tnt(uSand, su, sx, sy, sk);
        for (float z = 0.0; z < (length(uCam - vWorld) < 8.0 ? 1.0 : 0.0) && 1.0 - g.r > z; z += 0.25) { su += du; g = tnt(uSand, su, sx, sy, sk); }
        vec3 sa = vec3(0.56, 0.49, 0.37) * (0.78 + 0.35 * vn(w * 0.18)) * (0.82 + 0.12 * vn(w * 3.0) + 0.25 * g.g)
                * (1.0 - 0.4 * smoothstep(0.3, 0.1, g.b));                          // grains, dark mineral
        // the wrack line: strands of weed and twigs, bits of shell and coral
        float wr = smoothstep(0.8, 0.0, abs(s - 45.2 - 0.8 * vn(w * 0.15)));
        if (wr > 0.0 && f < 0.12) {
            vec4 c2 = vec4(0.0, 0.0, 0.0, -1.0), n2 = vec4(0.0);
            litter(w, 0.09, 3.0, c2, n2);
            if (c2.w > 2.5 && fract(c2.w * 3.7) < wr * 0.8) sa = c2.rgb * vec3(0.6, 0.75, 0.4);
        }
        sa = mix(sa, vec3(0.85, 0.8, 0.72), smoothstep(0.7, 0.9, g.b) * (0.3 + wr));   // bits of shell
        vec3 D3 = vec3(s3sea.x, 0.0, s3sea.y), L3 = vec3(-s3sea.y, 0.0, s3sea.x);
        // the sand's relief: wind-shaped swells and hollows, and the baked ripples
        // (~1.5 cm) on the dry sand
        vec2 r = vnd(w * 0.35).yz * 0.32 + vnd(w * 1.3).yz * 0.1 * dry
               - (vec2(tnt(uSand, su + vec2(0.002, 0.0), sx, sy, sk).r, tnt(uSand, su + vec2(0.0, 0.002), sx, sy, sk).r) - g.r) * 3.0 * dry;
        N = normalize(N + vec3(r.x, 0.0, r.y) * sand);
        // The tyrannosaurus's tracks: a print wherever a foot came down, at the
        // footholds jr.vert plants for its across() walk (which slows down to
        // sniff). Left feet are on the sea side.
        float pa = 1e3, kk = 0.0;
        for (float k = 0.0; k < 16.0; k++) {
            float tk = 52.258064 + (k + 0.55) * 0.9677419, x = clamp((tk - 58.1) / 1.3, -1.0, 1.0);
            float a = -16.8 + 3.0 * (tk - 52.258064) - 3.9 * (0.5 * (x + 1.0) + sin(3.1416 * x) / 6.2832);
            if (j > tk - 0.4 && abs(acr - a) < abs(acr - pa)) { pa = a; kk = k; }
        }
        if (abs(s - 34.5) < 1.2 && pa < 1e3) {
            vec2 q = vec2(acr - pa, s - 34.5 - (1.0 - mod(kk, 2.0) * 2.0) * 0.45);
            float pm = jwprint(q), hq = 0.3 * jwprint(q * 0.82) - pm;            // pressed in, a rim pushed up
            N = normalize(N - (L3 * (0.3 * jwprint((q + vec2(0.03, 0.0)) * 0.82) - jwprint(q + vec2(0.03, 0.0)) - hq)
                             + D3 * (0.3 * jwprint((q + vec2(0.0, 0.03)) * 0.82) - jwprint(q + vec2(0.0, 0.03)) - hq)) * 2.5);
            alb *= 1.0 - 0.2 * pm;
            sa *= 1.0 - 0.2 * pm;
            dry *= 1.0 - 0.3 * pm;
        }
        // seagrass out past the surf; caustics on the sand under the water
        sa = mix(sa, vec3(0.06, 0.09, 0.04), jwbed(w));
        if (s > e) {
            vec2 cq = w * 2.6 + uTime * vec2(0.3, 0.2);
            sa *= 1.0 + 2.0 * pow(clamp(1.0 - 2.5 * abs(vn(cq) - vn(cq * 1.37 + 4.0 - uTime * 0.3)), 0.0, 1.0), 6.0)
                      * smoothstep(e, e + 1.0, s) * exp(-max(-1.0 - vWorld.y, 0.0) * 0.5);
        }
        // foam stranded where the sheet stopped, as it drains
        sa = mix(sa, vec3(0.75), smoothstep(0.25, 0.0, abs(s - rw.y - 0.1)) * smoothstep(0.25, 0.35, rw.z) * (1.0 - smoothstep(0.45, 0.95, rw.z))
                                * smoothstep(0.5, 0.8, 1.0 - abs(2.0 * vn(vec2(acr * 2.0, s * 4.0)) - 1.0)));
        alb = mix(alb, sa * mix(0.45, 1.0, dry), sand);
        rough = mix(rough, mix(mix(0.45, 0.9, dry), 0.06, film), sand);
        // The film fills the ripples and mirrors the sky, and moist sand
        // mirrors it a little. Up close, dry grains glint in the sun.
        N = normalize(N + vec3(0.0, 3.0 * film * sand, 0.0));
        vec3 V = normalize(uCam - vWorld), R = reflect(-V, N);
        add = (0.15 * (1.0 - dry) + film) * sand * (0.02 + 0.98 * pow(1.0 - max(dot(N, V), 0.0), 5.0)) * s3sky(R);
        ad = add.r + add.g + add.b;
        if (f < 0.01 && s > 36.0) {                                        // out in the open sun
            vec4 gg = h4(floor(w * 300.0) + 7.0);
            add += uGlow * pow(max(dot(normalize(vec3(gg.x - 0.5, 1.2, gg.y - 0.5)), normalize(uLightDir + V)), 0.0), 300.0)
                 * step(0.99, gg.z) * dry * sand * (1.0 - smoothstep(0.002, 0.01, f)) * 2.0;
        }
    }
    vec3 l = s3light(vWorld, N, alb, rough, 0.0, ao);
    s3amb += ad;                                      // the occlusion darkens the mirrored sky too
    o = vec4(s3fog(l + add, vWorld), s3a);
}
