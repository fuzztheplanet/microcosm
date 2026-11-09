#version 450 core
// Instanced seaweed ribbons, built here from gl_VertexID. Each blade is a
// unit-tall vertical strip of 16 quads (17 x 2 vertices; triangle t uses t,
// t + 1 and t + 2) that tapers to a narrow tip. It's turned to face the eye
// and drawn two-sided. Blades sway in the current, bending more the higher up
// they go (local y), each offset by a phase packed in aColor.a. Tall blades
// become kelp fronds (K ramps in from 3 to 5 m tall): broad (0.9 m across the
// middle), golden-brown, with a rippling edge, and swaying three times as wide.
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

out vec3  vWorld;
out vec3  vNormal;
out vec4  vColor;
out float vH;

void main(){
    float phase = aColor.a;
    int v = gl_VertexID / 3 + gl_VertexID % 3;      // vertex (row, side) of the 17 x 2 strip
    float f = float(v / 2) / 16.0;
    // The spine sways in the current, and the width is laid across it square
    // to the eye (turned about the spine), so a blade is never seen edge-on.
    // A ribbon turned edge-on thins to a sub-pixel stick that crawls and
    // breaks up under the jitter.
    float K = smoothstep(3.0, 5.0, aModel[1].y);
    vec4 wp = aModel * vec4(sin(uTime * 1.3 + phase + f * 0.6) * 0.18 * f * (1.0 + 2.0 * K), f, cos(uTime * 1.1 + phase * 1.3 + f * 0.5) * 0.12 * f * (1.0 + 2.0 * K), 1.0);
    vec3 w = normalize(cross(wp.xyz - uCam, vec3(0.0, 1.0, 0.0)));
    wp.xyz += w * (0.12 * (1.0 - 0.7 * f) + K * (1.6 + 0.3 * sin(wp.y * 5.0)) * f * (1.0 - f)) * float(v % 2 * 2 - 1);
    vWorld  = wp.xyz;
    vNormal = cross(w, vec3(0.0, 1.0, 0.0));
    vColor  = vec4(mix(aColor.rgb, vec3(3.0, 0.8, 0.2), K), 0.0);
    vH      = f;
    gl_Position = uViewProj * wp;
}
