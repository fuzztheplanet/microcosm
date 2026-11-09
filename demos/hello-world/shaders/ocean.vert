#version 450 core
// The ocean surface: a flat 900 m square of 140 x 140 cells, built here from
// gl_VertexID and pushed into waves by a few directional sines. The shading
// (ocean.frag) rebuilds the waves per pixel, so the 6 m triangles never show
// in the lighting or the foam.

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
layout(location = 0) uniform mat4  uModel;

out vec3 vWorld;

// The swell: a few directional sines (xy = direction, z = spatial freq,
// w = speed), summed with their analytic slopes. ocean.frag sums the same
// ones again per pixel, with finer ones on top.
float waveH(vec2 p, out vec2 slope){
    vec4 ws[4] = vec4[4](
        vec4( 1.0,  0.0, 0.16, 1.0),
        vec4( 0.6,  0.8, 0.28, 1.5),
        vec4(-0.7,  0.5, 0.52, 2.1),
        vec4( 0.2, -0.9, 0.95, 2.9));
    float h = 0.0, amp = 1.0;
    slope = vec2(0.0);
    for (int i = 0; i < 4; i++){
        vec2  d  = normalize(ws[i].xy);
        float f  = ws[i].z;
        float ph = dot(d, p) * f + uTime * ws[i].w;
        h     += amp * sin(ph);
        slope += amp * f * d * cos(ph);
        amp   *= 0.55;
    }
    return h;
}

// Corner v % 6 of quad v / 6 in a grid: (0,0) (1,0) (1,1), (0,0) (1,1) (0,1).
// That's counter-clockwise seen from the side the two steps' cross points to.
ivec2 corner(int v){ return ivec2(22, 52) >> v % 6 & 1; }

void main(){
    vec2 slope;
    int q = gl_VertexID / 6;
    vec3 pos = vec3((vec2(q / 140, q % 140) + vec2(corner(gl_VertexID).yx)) * 6.4285717 - 450.0, 0.0).xzy;
    pos.y += waveH(pos.xz, slope) * 1.1;
    vec4 wp = uModel * vec4(pos, 1.0);
    vWorld  = wp.xyz;
    gl_Position = uViewProj * wp;
}
