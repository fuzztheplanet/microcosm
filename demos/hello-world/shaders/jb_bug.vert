#version 450 core
// The seven-spot ladybug, about 8 mm long. It's built in millimetres (x right,
// y up, z forward, y = 0 on the leaf) out of one unit sphere, with one part
// per instance:
//  0-1   the wing cases. Each is a quarter shell: the sphere folded onto
//        x, y > 0, with the folded-over parts pulled 0.5% inward to make the
//        underside. They lift and spread about their hinges as they open (A.y).
//  2     the shield (a half shell), 3 the head, 4-5 the eyes, 6 the belly.
//  7-24  six legs of three segments each, the knees bent up and out between
//        hip and foot. The feet step in a tripod gait (A.x) and are tucked up
//        in flight (A.w). A foot that's down slides back under the body as
//        fast as the body moves forward, so it holds its place on the leaf,
//        and then swings forward in an arch half a millimetre high. The
//        stride is a triangle wave 1 mm from front to back, driven by A.x,
//        the distance walked (s3_bug.c).
//  25-28 the clubbed antennae, which twitch now and then.
//  29-31 dew drops on the hero leaf (in world space, placed with shape()).
//  32-47 the hind wings. They unfold (A.z) and then beat. Each is drawn as
//        eight faint copies spread over the stroke, which is how the eye sees
//        an 85 Hz beat.
// The unit sphere is made here from gl_VertexID: 12 x 12 cells on each face of
// a cube, pushed out onto the sphere. Its grid lines run along x = 0 and
// y = 0, where the wing cases fold.
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
// The ladybug. Columns 0-2 .xyz hold its position, heading and up vector. A
// holds its gait phase, then how far the wing cases are open, how far the
// wings are unfolded and how much it's flying (each 0..1, except the gait).
layout(location = 0) uniform mat4 uBug;
layout(location = 1) uniform int  uFirst;  // the first part in this draw
out vec3 vWorld;
out vec3 vNormal;
out vec3 vLoc;        // the part's own unit-sphere point (for its markings)
flat out int vPart;

vec3 P, Nn;
vec4 A = vec4(uBug[0].w, uBug[1].w, uBug[2].w, uBug[3].x);
vec3 shape(float u, float v, vec2 k){
    float w = 0.36 * pow(sin(3.14159 * pow(u, 0.6)), 0.85);
    return vec3(u, v * w, k.x * u - k.y * u * u + (0.1 * abs(v) - 0.05 * v * v) * w + 0.007 * sin(u * 17.0 + v * 2.0 + k.y * 20.0) * v * v);
}
// a point on the hero leaf (jb_leaf_at() in s3_bug.c)
vec3 hero(float u, float v){
    vec3 s = shape(u, v, vec2(0.2, 0.3)) * 0.4;
    return vec3(1.27, s3h(vec2(1.27, 0.46)) + 0.8 + s.z, 0.46) + vec3(-0.257, 0.0, -0.966) * (0.06 + s.x) + vec3(-0.966, 0.0, 0.257) * s.y;
}
void part(vec3 c, mat3 B, vec3 r, vec3 p){ P = c + B * (p * r); Nn = B * (p / r); }
// shell(), below, folds the sphere onto y > 0 (and x * s > 0 when s != 0) to
// make a thin shell.
// Corner v % 6 of quad v / 6 in a grid: (0,0) (1,0) (1,1), (0,0) (1,1) (0,1).
// That's counter-clockwise seen from the side the two steps' cross points to.
ivec2 corner(int v){ return ivec2(22, 52) >> v % 6 & 1; }
// Vertex v of a cube's six n x n grids (+X +Y +Z -X -Y -Z, wound to face out),
// on the surface of [-1,1]^3. Each grid is bent by u (e + (1 - e) u^2), which
// keeps 0 and the edges exactly where they were, so neighbouring faces meet
// without a crack. e = 0.75 evens out the cells once they're pushed onto a
// ball; e = 1.5 crowds them toward the edges.
vec3 cube(int v, int n, float e){
    int q = v / 6, f = q / (n * n), r = q % (n * n);
    vec2 u = vec2((ivec2(r % n, r / n) + corner(v)) * 2 - n) / float(n);
    u *= e + (1.0 - e) * u * u;
    vec3 p = f < 3 ? vec3(1.0, u) : vec3(-1.0, -u.x, u.y);
    return f % 3 == 0 ? p : f % 3 == 1 ? p.zxy : p.yzx;
}
vec3 shell(vec3 p, float s){ return vec3(s == 0.0 ? p.x : s * abs(p.x), abs(p.y), p.z) * (p.y < 0.0 || p.x * s < 0.0 ? 0.995 : 1.0); }
mat3 along(vec3 d){
    vec3 y = normalize(d), x = normalize(cross(y, vec3(0.3, 0.2, 1.0)));
    return mat3(x, y, cross(x, y));
}
void seg(vec3 a, vec3 b, float r, vec3 p){ part((a + b) * 0.5, along(b - a), vec3(r, length(b - a) * 0.5, r), p); }
mat3 rx(float a){ return mat3(1, 0, 0, 0, cos(a), sin(a), 0, -sin(a), cos(a)); }
mat3 rz(float a){ return mat3(cos(a), sin(a), 0, -sin(a), cos(a), 0, 0, 0, 1); }

void main(){
    int k = gl_InstanceID + uFirst;
    vec3 p = normalize(cube(gl_VertexID, 12, 0.75));
    float s = (k & 1) == 0 ? -1.0 : 1.0;
    mat3 I = mat3(1.0);
    if (k < 2) {
        p = shell(p, s);
        mat3 R = rx(0.45 * A.y) * rz(s * 0.95 * A.y);
        vec3 h = vec3(s * 0.3, 2.5, 1.6);
        part(h + R * (vec3(0.0, 0.95, -0.55) - h), R, vec3(2.75, 2.0, 2.95), p);
    } else if (k == 2) { p = shell(p, 0.0); part(vec3(0.0, 1.0, 1.75), I, vec3(2.05, 1.4, 1.15), p); }
    else if (k == 3) part(vec3(0.0, 0.95, 2.75), I, vec3(1.05, 0.75, 0.62), p);
    else if (k < 6) part(vec3(s * 0.7, 1.12, 2.95), I, vec3(0.33), p);
    else if (k == 6) part(vec3(0.0, 1.0, -0.3), I, vec3(2.2, 1.0, 2.9), p);
    else if (k < 25) {
        int l = (k - 7) / 3, g = (k - 7) % 3, pr = l % 3;
        s = l < 3 ? -1.0 : 1.0;
        float c = A.x + 3.14159 * float((pr + l / 3) & 1);
        vec3 h = vec3(s * 0.8, 0.7, 1.3 - 1.1 * float(pr)),
             f = mix(vec3(s * 2.7, 0.5 * max(-sin(c), 0.0), 2.4 - 2.2 * float(pr) + 0.317 * asin(cos(c))),
                     h + vec3(s * 0.7, -0.35, -0.9 + 0.2 * float(pr)), A.w),
             d = f - h, kn = (h + f) * 0.5 + vec3(s * 0.4, 0.9, 0.0) * sqrt(max(2.56 - dot(d, d) * 0.25, 0.04));
        if (g == 0) seg(h, kn, 0.22, p);
        else if (g == 1) seg(kn, f, 0.15, p);
        else seg(f, f + vec3(s * 0.25, 0.08, pr == 2 ? -0.55 : 0.5), 0.1, p);
    } else if (k < 29) {
        s = k < 27 ? -1.0 : 1.0;
        float tw = 0.25 * sin(uTime * 6.0 + s) * smoothstep(0.6, 1.0, sin(uTime * 0.9 + s * 1.7));
        vec3 b = vec3(s * 0.42, 1.2, 3.2), d = normalize(vec3(s * (0.7 + tw), 0.5 + tw * 0.5, 1.0));
        if ((k & 1) == 1) seg(b, b + d * 1.2, 0.07, p);
        else part(b + d * 1.45, along(d), vec3(0.16, 0.3, 0.16), p);
    } else if (k < 32) {
        vec3 dr = vec3[](vec3(0.4, -0.06, 2.6), vec3(0.47, 0.33, 3.4), vec3(0.355, 0.3, 1.9))[k - 29];
        vec3 x = hero(dr.x, dr.y), t = normalize(hero(dr.x + 0.01, dr.y) - x), n = normalize(cross(t, hero(dr.x, dr.y + 0.01) - x));
        p = shell(p, 0.0);
        vec3 r = dr.z * vec3(1e-3, 0.55e-3, 1e-3);
        P = x + mat3(t, n, cross(t, n)) * (p * r) - n * 1e-4;
        Nn = mat3(t, n, cross(t, n)) * (p / r);
    } else {
        int w = k - 32;
        s = w < 8 ? -1.0 : 1.0;
        mat3 R = rz(s * A.w * 1.1 * cos(3.14159 * (float(w & 7) + 0.5) / 8.0 + uTime * 40.0));
        vec3 dw = R * normalize(mix(vec3(s * 0.15, 0.05, -1.0), vec3(s, 0.15, -0.4), A.z));
        float sp = 2.8 * (0.3 + 0.7 * A.z);
        part(vec3(s * 0.9, 2.2, 1.0) + dw * sp, mat3(cross(dw, R[1]), dw, R[1]), vec3(1.1, sp, 0.03), p);
    }
    vLoc = p;
    vPart = k;
    if (k < 29 || k > 31) {
        vec3 z = normalize(uBug[1].xyz), x = normalize(cross(uBug[2].xyz, z));
        mat3 B = mat3(x, cross(z, x), z);
        P = uBug[0].xyz + B * P * 1e-3;
        Nn = B * Nn;
    }
    vWorld = P;
    vNormal = Nn;
    gl_Position = uViewProj * vec4(P, 1.0);
}
