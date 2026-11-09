#version 450 core
// The blob: a smooth ball wobbled along its normal by a 3D noise field scaled
// by uWobble. The ball is made here from gl_VertexID, as a grid of cells on
// each face of a cube pushed out onto the sphere. uWobble 1 is the soft, lumpy
// creature and ~0 a round sphere. After the encounter the demo lerps uWobble
// down and the shape rounds out, and a faint breathing on uTime keeps it
// moving. On the end card it grows two little arms that wave on the wink (D).
// Feeds blob.frag. It casts no shadow map, because under water the jelly is a
// lens (see uwsurf.frag).
layout(location = 2) in mat4 aModel;
layout(location = 6) in vec4 aColor;

// Per-frame uniforms shared by every scene shader: std140 on binding 0, kept
// identical in every file and mirrored by FrameUniforms in main.c.
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

out vec3 vWorld;
out vec3 vNormal;
out vec4 vColor;
out float vR;        // its radius (the model's scale)

layout(binding = 1) uniform sampler3D uNoise;   // 32^3 random bytes, wrapping (main.c's noise_make)
// Value noise, 0..1. The hardware blends the lattice; each cell's fraction
// goes through the cubic first.
float vn3(vec3 p){ vec3 i = floor(p), f = p - i; return texture(uNoise, (i + f * f * (3.0 - 2.0 * f) + 0.5) / 32.0).r; }
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
float disp(vec3 p){ return 0.24 * vn3(p * 1.6 + 11.0) + 0.1 * vn3(p * 3.3 + 4.0) - 0.17; }   // -0.17 .. 0.17

// How far out the surface lies along n, beyond the unit ball: the lumps,
// breathing by br, plus two little arms on the end card (g: round, uWobble
// 0.15, and not jelly, alpha 0). The arms are Scene 3's (jo_blob.vert) and
// swell out of its sides as one surface. Each is a paraboloid 0.42 of the
// radius high and 0.27 out to its foot (x is the chord from its axis over
// that), which makes a stubby arm with a round tip. A smoothstep flares the
// foot into the body with no crease. A pow 0.7 cone creased at the foot and
// its shading streaked, and an ellipse's flat top looked like a cut-off tube.
// The arms stand out of its sides, a little forward, and hang 29 deg down. As
// it puffs up for the wink (6.9-7.3 s on the card's uTime) they rise out to
// the sides. The left one goes to 17 deg up, cheering, and the right one
// waves between level and 26 deg up. Raised higher, beside its head, they
// looked like ears. When it turns over in the somersault they draw in (w, by
// how upright it stands), or the hanging arms would stand up like horns.
// That's spread over 120 deg of the turn, because over 35 deg they popped
// back in two frames.
float br, g;
float D(vec3 n){
    float r = uWobble * disp(n) * br, w = g * smoothstep(-0.5, 1.0, normalize(aModel[1].xyz).y);
    for (int i = 0; i < 2; i++) {
        float e = mix(-0.5, 0.3 - (0.075 - 0.225 * sin(uTime * 11.0)) * float(i), smoothstep(6.9, 7.3, uTime)),
              x = length(n - vec3(float(i + i - 1) * cos(e), sin(e), 0.35)) / 0.27;
        r += w * 0.42 * max(1.0 - x * x, 0.0) * smoothstep(1.0, 0.75, x);
    }
    return r;
}

void main(){
    // 48 x 48 cells a face. The end card's arms need that many; elsewhere
    // they're much finer than any lump and only smooth the outline.
    g  = float(uWobble < 0.5 && aColor.a == 0.0);
    vec3  N  = normalize(cube(gl_VertexID, 48, 0.75));    // the unit ball's point and normal
    br = 1.0 + 0.04 * sin(uTime * 1.5);                    // gentle breathing
    float d0 = D(N);
    vec3  t  = normalize(abs(N.y) < 0.9 ? cross(N, vec3(0,1,0)) : cross(N, vec3(1,0,0)));
    vec3  b  = cross(N, t);
    float e  = 0.10 - 0.08 * g;                            // (finer for the arms)
    float dt = D(N + t * e);
    float db = D(N + b * e);
    vec3  p  = N + N * d0;
    vec3  Np = normalize(N - 0.8 * (t * ((dt - d0) / e) + b * ((db - d0) / e)));
    vec4  wp = aModel * vec4(p, 1.0);
    vWorld  = wp.xyz;
    vNormal = mat3(aModel) * Np;
    vColor  = aColor;
    vR      = length(aModel[0].xyz);
    gl_Position = uViewProj * wp;
}
