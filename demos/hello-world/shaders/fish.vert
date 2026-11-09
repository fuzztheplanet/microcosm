#version 450 core
// Scene 2's animals: the fish, the crabs, the predator and the whale. They're
// all sculpted once at load by the creature kit (jr.comp, kinds 1-4) into one
// vertex buffer (binding 5). Each vertex holds its rest point, its two bones
// and the second bone's share, and its normal. Instanced like object.vert
// (model at 2-5, tint plus a per-animal phase at 6).
// uK is the draw's UW_ flags. The fish and crabs swim with a side-to-side wag
// that travels down +Z (crabs, 16, have no scales). The predator (8) also
// works its jaws, which are modelled at gape 0.55: uGape swings the upper one
// (bone 1, its fangs 3) up and the lower one (2, 4) down about their hinges.
// A vertex the kit marked (+256) lies inside the mouth. The whale (4) beats up
// and down instead, a slow wave from head to flukes. Fins are bone 5.
layout(location = 2) in mat4 aModel;
layout(location = 6) in vec4 aColor;
layout(std430, binding = 5) readonly buffer Cv { vec4 V[]; };
layout(std430, binding = 0) readonly buffer U { vec4 W[]; };   // uwplace.comp's: the blob's path from W[25977], the schools' from W[27996]
layout(location = 1) uniform int uK;
layout(location = 3) uniform vec2 uStory;   // x: the act's story seconds (main.c's uw_u), for the big school

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

out vec3  vWorld;
out vec3  vNormal;
out vec4  vColor;
out float vBodyT;
out vec2  vUV;
out vec3  vObj;
out float vTooth;
out float vFin;    // 1 on a fin
out float vIn;     // 1 inside the predator's mouth

// Bone k's swing of p: the jaws turn about their hinges. For a normal, h is 0
// and there's no hinge.
vec3 jaw(vec3 p, int k, float h){
    if (k < 1 || k > 4) return p;
    float s = k == 1 || k == 3 ? 1.0 : -1.0, a = (uGape - 0.55) * 0.75 * s;
    vec2 q = p.zy - h * vec2(0.72, 0.06 * s);
    q = mat2(cos(a), sin(a), -sin(a), cos(a)) * q + h * vec2(0.72, 0.06 * s);
    return vec3(p.x, q.y, q.x);
}

// The big school the blob bolts through in its flight (uK's 32). Instance i is
// slot c around main.c's last school (W[28021]). There are four layers 1.1 m
// apart (jittered by 0.5) of 75 fish each, laid out on Vogel's spiral (golden
// angle around, square root out) over a disc 16 m across. That spreads them
// evenly, at least 1.4 m apart and without rows, all under the camera's path.
// A fish holds its slot, bobbing, until the blob comes by. Around its closest
// approach (t: the path's nearest sample plus how far along the next segment
// it lies) it darts out sideways, square to the path and away from it (q).
// How far it darts, D, is 5.5 m less a fifth of its distance from the path.
// The dart starts 1.5 s before, and 0.15 s sooner for every metre further
// out, so an inner fish never catches up with an outer one. It drifts back
// from 2.5 s after, once the camera following the blob has flown over too.
// This is an analytic flow around the known path rather than a simulated
// flock. A channel opens 5.5 m either side of the blob, the nearest fish dart
// furthest, and none crosses another. They all swim one way on the spot
// (1.7 m/s), so a fish turns toward where it darts and back.
// Measured over the act, their centres stay at least 5.5 m from the blob's and
// 5 m from the camera, and as ellipsoids of their length and height they stay
// clear of each other. The props keep out of their disc (uwplace.comp).
vec3 c, q;
float t, D, h;
vec3 at(float w){ return c + q * D * (smoothstep(-1.5, -.3, w - t + 4.1 - D * .75) - smoothstep(2.5, 4.5, w - t)) + vec3(0., .15 * sin(w * 1.7 + h), 0.); }

void main(){
    vec4 a = V[2 * gl_VertexID], b = V[2 * gl_VertexID + 1];
    int k = int(a.w) & 15, l = int(a.w) >> 4 & 15, i = gl_InstanceID, m = 0;
    vec3 p = a.xyz, n = b.xyz;
    mat4 M = aModel;
    vec4 C = aColor;
    if ((uK & 32) != 0) {
        h = fract(sin(float(i) * 78.233) * 43758.547);
        float u = uStory.x, x = float(i) * 2.39996, e = 1e9;
        c = vec3(cos(x), 0., sin(x)) * sqrt((float(i % 75) + .5) / 75.) * 8. + vec3(0., float(i / 75) * 1.1 - 1.65 + h * .5, 0.);
        c += W[28021].xyz;
        for (int j = 714; j < 740; ++j) if (distance(c.xz, W[25977 + j].xz) < e) e = distance(c.xz, W[25977 + j].xz), m = j;
        vec4 o = W[25977 + m], g = W[25978 + m] - o;
        q = normalize(vec3(-g.z, 0., g.x));
        e = dot(c - o.xyz, q);
        q *= sign(e + 1e-6);
        t = (float(m) + dot(c.xz - o.xz, g.xz) / dot(g.xz, g.xz)) * .127595;
        D = max(5.5 - .2 * abs(e), 0.);
        vec3 f = normalize(at(u + .05) - at(u - .05) + vec3(.14, 0., .1)), r = normalize(cross(vec3(0., 1., 0.), f));
        M = mat4(vec4(r, 0.), vec4(cross(f, r), 0.), vec4(f, 0.), vec4(at(u) / .34, 1. / .34)) * .34;
        C = vec4(.12, .2, .4, h * 20.);
    }
    float phase = C.a;
    vFin = float(k == 5);
    vTooth = float(k == 3 || k == 4);
    vIn = float(int(a.w) >> 8);
    vUV = vec2((atan(p.y, p.x) / 6.2831853 + 0.5) * 4.0 + 8.0 * vFin, 2.3 * (p.z + 1.7)) * float((uK & 16) == 0);   // the scale skin, wrapped round (crabs: bare)
    if ((uK & 8) != 0) {
        p = mix(jaw(p, k, 1.0), jaw(p, l, 1.0), b.w);
        n = jaw(n, k, 0.0);
    }
    vObj = p;
    vBodyT = p.z;
    if ((uK & 4) != 0) {
        float s   = (1.0 - p.z) * 0.5;                    // 0 at the snout .. 1 at the flukes' root
        float env = smoothstep(0.30, 1.05, s);
        float amp = 0.006 + 0.085 * env * env;            // the flukes travel ~+-1.2 m at scale
        float arg = uTime * 1.35 + phase - 2.4 * s;       // ~4.7 s per beat, head -> tail
        float dA  = 0.17 * env * (s > 0.30 && s < 1.05 ? 6.0 * (s - 0.30) * (1.05 - s) / 0.4219 : 0.0);
        p.y += amp * sin(arg);
        n.z += 0.5 * (dA * sin(arg) - amp * 2.4 * cos(arg)) * n.y;   // dz/ds = -2: tilt with the slope
    } else
        p.x += sin(p.z * 2.2 - uTime * 7.0 + phase * 6.2831) * 0.18 * (1.0 - smoothstep(-1.4, 0.2, p.z));   // 0 at nose, 1 at tail
    vec4 wp = M * vec4(p, 1.0);
    vWorld  = wp.xyz;
    vNormal = normalize(mat3(M) * normalize(n));
    vColor  = C;
    gl_Position = uViewProj * wp;
}
