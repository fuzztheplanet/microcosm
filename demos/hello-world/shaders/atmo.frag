#version 450 core
// The planet's atmosphere: the same sphere drawn 6% larger with its front
// faces culled, so we shade the far rim, and added on top as the halo around
// the disc. Each ray scatters 1 - e^-t of the light, where t is the Rayleigh
// depth along it. That depth falls off as e^(-50 h) with the height h at which
// the ray passes over the ground (in shell radii), so the halo hugs the limb.
// Near the limb the depth saturates every colour into a pale band, and higher
// up only blue saturates, giving a blue fringe that fades to black.
// The light scattered is the sunlight where the ray passes (air(), as on the
// disc). It's white on the day side, warmer toward the terminator and gone on
// the night side, so the disc and the halo meet at the limb. As on the disc,
// the tint only goes half-way, and the other half is the red's strength in
// grey, standing in for the sky's light scattered again. With the full tint
// the band near the limb took on the sun's own colour at each sun height. At
// the two ends of the terminator the halo ran white, yellow, orange and red
// along a short arc.
// Shares planet.vert (vObj goes unused here). Its sun turns with planet.frag's.
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

// Same as planet.frag's: the sunlight that gets through the air to a point
// where the sun's cosine is s.
vec3 air(float s){ s = max(s + 0.1, 0.005); return exp(-vec3(0.0073, 0.0176, 0.033) / (s * s)); }

void main(){
    vec3 V = normalize(vWorld - uCam), c = vWorld - V * dot(vWorld, V);   // c: the ray's closest point to the centre
    vec3 A = air(dot(normalize(c), normalize(uSun * mix(vec3(1.7, 1.5, 0.1), vec3(1.0), uAtmo))));
    o = vec4((A + A.r) * (1.0 - exp(-vec3(0.02, 0.04, 0.09) * 66.0 * exp((0.943 - length(c) / length(vWorld)) * 50.0))) * 0.3, 1.0);   // added on
}
