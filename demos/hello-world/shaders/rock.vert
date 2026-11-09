#version 450 core
// Boulders with progressive geometry. The craggy rock they all start from is
// baked once at load by uwplace.comp, which leaves its vertices (position,
// normal) in that shader's buffer. The mesh's indices pick them by
// gl_VertexID, each LOD from its own range. Each boulder is squashed,
// stretched and dented in its own way (seeded from its position), so no two
// share a shape. On top of that, a per-instance 3D-noise displacement along
// the normal grows as uReal rises, its higher octaves fading in later, so
// the rocks gain crags and fine relief as Scene 2 goes on. The normal is
// re-derived from the noise gradient (a three-tap finite difference) so the
// new geometry lights correctly. Same instance layout as object.vert. Feeds
// uwsurf.frag (vWorld/vNormal/vColor).
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

layout(std430, binding = 0) buffer B { float H[40401]; vec4 V[]; };   // the rock: position, normal

layout(binding = 1) uniform sampler3D uNoise;   // 32^3 random bytes, wrapping (main.c's noise_make)
// Value noise, 0..1. The hardware blends the lattice; each cell's fraction
// goes through the cubic first.
float vn3(vec3 p){ vec3 i = floor(p), f = p - i; return texture(uNoise, (i + f * f * (3.0 - 2.0 * f) + 0.5) / 32.0).r; }
float disp(vec3 p, vec3 s){                            // octaves weighted by uReal, each -1..1 folded in
    return 0.14 * vn3(p * 1.3 + s * 4.0) - 0.2         // broad dents of its own (inward only)
         + 0.12 * vn3(p * 2.0 + s)
         + uReal * (0.18 * vn3(p * 4.5 + s * 1.7) - 0.09
         + uReal * (0.11 * vn3(p * 9.0 + s * 2.3) - 0.09
         + 0.07 * vn3(p * 17.0 + s * 3.1)));           // fine crags, finale only
}
void main(){
    vec3 s = aModel[3].xyz * 0.37;                     // unique per boulder
    // Each boulder gets its own shape: squashed and stretched along its own
    // axes (at most a tenth past the mesh, inside placement's UW_ROCK_VDISP),
    // and dented.
    vec3 q = 0.7 + 0.4 * fract(s * vec3(7.1, 3.3, 5.7)), a = V[2 * gl_VertexID].xyz * q;
    vec3 N = normalize(V[2 * gl_VertexID + 1].xyz / q);
    vec3 t = normalize(abs(N.y) < 0.9 ? cross(N, vec3(0,1,0)) : cross(N, vec3(1,0,0)));
    vec3 b = cross(N, t);
    float e  = 0.10;
    float d0 = disp(a,         s);
    float dt = disp(a + t * e, s);
    float db = disp(a + b * e, s);
    vec3 p  = a + N * d0;
    vec3 Np = normalize(N - 0.8 * (t * ((dt - d0) / e) + b * ((db - d0) / e)));
    vec4 wp = aModel * vec4(p, 1.0);
    vWorld  = wp.xyz;
    vNormal = mat3(aModel) * Np;
    // Darker low down, where it sinks into the sand (its centre stands 0.35 of
    // its scale above the sand). That's where the sand's shade, the silt and
    // the damp collect, and the dark contact seats the rock in the sand. The
    // GTAO's contact shadow is a metre wide and barely shows.
    vColor  = aColor;
    vColor.rgb *= smoothstep(-0.45, 0.25, (wp.y - aModel[3].y) / length(aModel[0].xyz)) * 0.7 + 0.3;
    gl_Position = uViewProj * wp;
}
