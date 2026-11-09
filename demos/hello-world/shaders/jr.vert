#version 450 core
// The tyrannosaurus. This poses jr.comp's sculpted mesh every frame, reading
// the rest pose straight from the buffer by gl_VertexID, and then draws a
// patch of the floor under it to darken. It walks along the forest's edge at
// s3sea * 34.5 and slows to a stop while it looks toward the camera. A foot
// comes down every STEP from REX, left first and then alternating, and the
// legs reach their footholds by two-bone IK. The body bobs, sways and rolls
// over each stance foot while the tail swings the other way.
// Each vertex follows its two bones. The trunk bends along its length (the
// tail's waves, and the neck turning with the head), the head and jaw turn
// with the look, and each leg's bones are moved rigidly from their rest pose
// onto the IK's. The walk matches s3_rex.c.
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
layout(std430, binding = 5) readonly buffer Jv { vec4 v[]; };   // jr.comp's vertices (uw_sculpt)
out vec3 vWorld;
out vec3 vNormal;
out vec3 vRest;    // the point on the creature at rest (for the skin's pattern, the eyes and the teeth)
out vec4 vPart;    // body: jaw share (for the mouth), -, 0, the jaw's drop; floor patch: -, -, 1, how dark
out vec3 vEye;     // from the eyeball's centre (on the point's side, 5 cm in under the skin) to the point

const float REX = 52.258064, STEP = .9677419;
const vec3 F = vec3(.5, 0., .8660254), U = vec3(0., 1., 0.);   // its heading; up
float j, tau, bob, sway, look, jaw, yaw, pitch, wb;
vec3 O, R, V2;       // the floor under its hips; its right (rolled); its up (rolled)
vec3 H, K, A, B, T;  // the current leg's hip, knee, ankle, ball of the foot; toe direction
float sw;            // the current foot's swing (0 planted .. 1 mid-air)
int i0, i1;          // the vertex's bones

// How far along the edge (s3_across) its hips are at jungle time t.
float across(float t){
    float x = clamp((t - 58.1) / 1.3, -1., 1.);
    return -16.5 + 3. * (t - REX) - 3.9 * (.5 * (x + 1.) + sin(3.1416 * x) / 6.2832);
}
// The floor under it at time t, offset sideways (right) by s.
vec3 ground(float t, float s){
    vec2 p = 34.5 * s3sea + across(t) * vec2(.5, .8660254) + s * vec2(-.8660254, .5);
    return vec3(p.x, s3h(p), p.y);
}
// A point of the body given in its own frame (x right, y up from the floor,
// z forward). The neck and head are turned by the fraction w of the head's
// look (yaw, then pitch).
vec3 body(vec3 q, float w){
    vec3 r = q - vec3(0., 3.75, 2.9);
    float c = cos(pitch * w), s = sin(pitch * w);
    r.yz = vec2(r.y * c - r.z * s, r.y * s + r.z * c);
    c = cos(yaw * w); s = sin(yaw * w);
    r.xz = vec2(r.x * c + r.z * s, r.z * c - r.x * s);
    r += vec3(0., 4.1 - .6 * bob * w, 2.9);                       // all of it 0.35 m up, which puts the hips at about 3.55 m
    return O + R * r.x + V2 * r.y + F * r.z;
}
// A world point back in the body's own frame (unturned).
vec3 own(vec3 w){ w -= O; return vec3(dot(w, R), dot(w, V2) - .35, dot(w, F)); }
// The leg on side s (-1 left, 1 right). Finds its foot's last landing and its
// swing, the ball of the foot, the ankle up and back from it, and the knee by
// IK.
void leg(float s){
    float f = s * .5 + .5, k = 2. * floor((tau - f) * .5) + f, ph = (tau - k) * .5;
    vec3 P0 = ground(REX + (k + .55) * STEP, s * .45) - F * .3, P1 = ground(REX + (k + 2.55) * STEP, s * .45) - F * .3;
    sw = smoothstep(.55, 1., ph);
    float m = sin(3.1416 * sw);
    B = mix(P0, P1, sw) + U * (.1 + .55 * m);
    H = body(vec3(s * .32, 3.2, -.55), 0.);
    A = B + normalize(F * mix(-.3, -.05, m) + U * mix(.7, .75, m)) * .78;
    vec3 d = A - H;
    float l = min(length(d), 3.36), a = (2.9584 - 2.7225 + l * l) / (2. * l);
    vec3 n = normalize(d);
    A = H + n * l;
    K = H + n * a + normalize(F - n * dot(F, n)) * sqrt(max(2.9584 - a * a, 0.));
    T = normalize(F - U * (.15 + .5 * sin(3.1416 * sw)));
}
// Rest point q carried rigidly from the bone at p0 along t0 to the bone at p1
// along t1 (both in the body's frame).
vec3 seg(vec3 q, vec3 p0, vec3 t0, vec3 p1, vec3 t1){
    vec3 x0 = normalize(vec3(1., 0., 0.) - t0 * t0.x), x1 = normalize(vec3(1., 0., 0.) - t1 * t1.x);
    q -= p0;
    return p1 + x1 * dot(q, x0) + t1 * dot(q, t0) + cross(t1, x1) * dot(q, cross(t0, x0));
}
// Where bone i takes rest point q.
vec3 X(int i, vec3 q){
    if (i > 2 && i < 11) {                                          // a leg: from its rest pose onto the IK's
        float s = i < 7 ? 1. : -1.;
        vec3 m = vec3(s, 1., 1.), h = vec3(.32, 3.2, -.55) * m, k = vec3(.39, 1.734, .347) * m,
             a = vec3(.45, .467, -.707) * m, b = vec3(.45, -.25, -.4) * m, K1 = own(K), A1 = own(A), B1 = own(B);
        int n = (i - 3) % 4;
        q = n == 0 ? seg(q, h, normalize(k - h), h, normalize(K1 - h)) :
            n == 1 ? seg(q, k, normalize(a - k), K1, normalize(A1 - K1)) :
            n == 2 ? seg(q, a, normalize(b - a), A1, normalize(B1 - A1)) :
                     seg(q, b, vec3(0., -.148, .989), B1, vec3(dot(T, R), dot(T, V2), dot(T, F)));
        return body(q, 0.);
    }
    if (i == 11) return body(q, 0.);                               // the arms
    if (i == 2) {                                                   // the jaw drops about its hinge
        vec2 r = q.yz - vec2(4.45, 4.05);
        float c = cos(jaw), s = sin(jaw);
        q.yz = vec2(4.45, 4.05) + vec2(r.x * c - r.y * s, r.x * s + r.y * c);
    }
    if (i > 0) return body(q, 1.);
    // The trunk. The tail swings against the hips and bounces with each step,
    // and both waves run down its length. The neck turns with the head.
    float z = q.z, tl = smoothstep(-1.5, -8., z);
    return body(q + tl * vec3(.55 * sin(3.1416 * tau - .5 * (-1.5 - z)), .2 * sin(6.2832 * tau - .7 * (-1.5 - z)) - .25 * tl * tl, 0.),
                smoothstep(2.6, 4., z));
}
vec3 Y(vec3 q){ return mix(X(i0, q), X(i1, q), wb); }
void main(){
    j = uTime - s3j0;
    tau = (j - REX) / STEP;
    float k = floor(tau), ph = tau - k, sg = mod(k, 2.) * 2. - 1.;  // the stance foot: -1 left, 1 right
    bob  = -.07 * cos(6.2832 * ph);
    sway = .08 * sg * sin(3.1416 * ph);
    float rl = .04 * sg * sin(3.1416 * ph);
    R  = vec3(-.8660254, 0., .5) * cos(rl) + U * sin(rl);
    V2 = cross(F, R) * -1.;
    O  = ground(j, 0.) + vec3(-.8660254, 0., .5) * sway + U * bob;
    // It notices us while slowing between the trunks, fixes its gaze almost
    // straight at the camera and lowers its head. The jaw opens on the two
    // breath accents before it slowly turns away and resumes its walk.
    look = smoothstep(56.2, 57.4, j) * (1. - smoothstep(59., 60.4, j));
    vec3 h = O + F * 5. + U * 4.8, toc = uCam - h;
    yaw   = look * clamp(atan(dot(toc, vec3(-.8660254, 0., .5)), dot(toc, F)), -1.65, 1.65);
    float breath = exp(-pow((j - 57.93) / .28, 2.));
    pitch = look * (.32 - .1 * breath);
    jaw   = .04 + look * (.14 + .35 * breath + .22 * exp(-pow((j - 58.54) / .22, 2.)));
    if (gl_VertexID >= 1 << 24) {                                   // the floor patch: dark under the body and the planted feet
        int Q = gl_VertexID - (1 << 24), n = Q % 6;
        Q /= 6;
        vec2 q = (vec2(Q % 40, Q / 40) + vec2(ivec2(22 >> n, 52 >> n) & 1)) / vec2(40., 24.);
        vWorld = ground(j, (q.y - .5) * 3.6) + F * (q.x * 6.4 - 3.2);
        vWorld.y = s3h(vWorld.xz) + .04;
        float dk = .45 * exp(-2. * (pow((q.x - .5) * 6.4 / 2.6, 2.) + pow((q.y - .5) * 3.6 / 1.1, 2.)));
        for (float s = -1.; s < 2.; s += 2.) {
            leg(s);
            dk += .5 * (1. - sw) * exp(-pow(length(vWorld - B) / .45, 2.));
        }
        vPart = vec4(0., 0., 1., min(dk, .7));
        vNormal = U;
        gl_Position = uViewProj * vec4(vWorld, 1.);
        return;
    }
    vec4 a = v[2 * gl_VertexID], b = v[2 * gl_VertexID + 1];
    int p = int(a.w);
    i0 = p & 15; i1 = p >> 4 & 15; wb = b.w;
    int l = i0 > 2 && i0 < 11 ? i0 : i1;                            // a leg vertex needs its leg's IK
    if (l > 2 && l < 11) leg(l < 7 ? 1. : -1.);
    // The normal comes from posing a point a short step out along the rest
    // normal. The sun's map doesn't need one.
    vec3 P = Y(a.xyz);
    vWorld  = P;
    vEye    = P - body(vec3(sign(a.x) * .15, 4.88, 4.58), 1.);   // carried by the head's bone, wherever it's read
    vNormal = uQ > .5 ? U : Y(a.xyz + b.xyz * .02) - P;
    vRest   = a.xyz;
    vPart   = vec4(i0 == 2 ? 1. - wb : i1 == 2 ? wb : 0., 0., 0., jaw);
    gl_Position = uViewProj * vec4(P, 1.);
}
