#version 450 core
// The planet's moon, in the foreground of the shot from orbit: dust over old
// lava plains, with broad, darker seas and brighter, mottled highlands. The
// only light is the planet's sun, turned as in planet.frag so the moon shows
// the same phase. With no air there's no haze, and the terminator is hard,
// which is where its relief shows. Shares planet.vert; vObj is its unit sphere.
in vec3 vObj;
in vec3 vWorld;
in vec3 vNormal;
out vec4 o;

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

layout(binding = 1) uniform sampler3D uNoise;   // 32^3 random bytes, wrapping (main.c's noise_make)
// Value noise, 0..1. The hardware blends the lattice; each cell's fraction
// goes through the cubic first.
float vn3(vec3 p){ vec3 i = floor(p), f = p - i; return texture(uNoise, (i + f * f * (3.0 - 2.0 * f) + 0.5) / 32.0).r; }
// Each octave is rotated (and scaled 2.02x) so the value noise's lattice never
// lines up.
float fbm3(vec3 p){ float v = 0.0, a = 0.5; for (int i = 0; i < 6; i++){ v += a * vn3(p); p = mat3(-1.2, 1.6, 0.4, -1.6, -1.0, -0.8, -0.4, -0.8, 1.8) * p; a *= 0.5; } return v; }


void main(){
    vec3 N = normalize(vNormal), dir = normalize(vObj), L = normalize(uSun * mix(vec3(1.7, 1.5, 0.1), vec3(1.0), uAtmo));
    // The seas (m) with mottled highlands around them. The mottling's own
    // field gives a gentle relief, turned into a normal by its screen-space
    // slopes as with planet.frag's ranges. From this far the moon shows mostly
    // through its albedo: relief strong enough to show as rock made it look
    // like an asteroid, and the post's colour fringes ran purple around it.
    float m = smoothstep(0.48, 0.56, fbm3(dir * 1.7 + 3.0)), e = fbm3(dir * 6.0);
    vec3 X = dFdx(vWorld), Y = dFdy(vWorld), a = cross(Y, N), b = cross(N, X);
    N = normalize(N - 0.5 * (dFdxFine(e) * a + dFdyFine(e) * b) / max(dot(X, a), 1e-9));
    o = vec4(vec3(0.42, 0.4, 0.38) * (1.0 - 0.5 * m) * (0.6 + 0.8 * e) * max(dot(N, L), 0.0) * 1.3 + 0.004, 1.0);
}
