#version 450 core
// The synthwave floor grid, drawn additively: the gaps between lines are black
// and add nothing, so the sky shows straight through and only the lines glow.
// They dim with distance so the horizon fades out. Where the shapes shade the
// floor from the key light, the lines dim and the floor darkens. The pass adds
// a negative amount there, as though the floor took the light, which works
// because the HDR target is float and its blending is unclamped.
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

layout(binding = 4) uniform sampler2DShadow uShadow;   // the key light's depth (s1_draw)
// The key light's shadow at p, 1 lit .. 0 in shadow. It takes 3 x 3 compared,
// bilinear taps of the depth map (s1_draw), 2 texels apart, so each tap's
// 2-texel filter meets the next and the ~9 cm penumbra ramps smoothly. Spaced
// wider, an edge would break up into steps. 4 x 4 looked the same and cost
// 1.8 ms more on the UHD 620. The code is the same in object.frag and
// ground.frag, so it packs once.
float keyShadow(vec3 p){
    vec3 s = (uLightVP * vec4(p, 1.0)).xyz * 0.5 + 0.5;
    float sh = 0.0;
    for (int k = 0; k < 9; k++)
        sh += texture(uShadow, vec3(s.xy + (vec2(k % 3, k / 3) - 1.0) / 1024.0, s.z)) / 9.0;
    return sh;
}

// Coverage of the nearest grid line in x or y, 1 on a line and 0 off it. It's
// anti-aliased with fwidth so the lines stay sharp at any distance instead of
// shimmering.
float grid_line(vec2 p, float w){
    vec2 d = fwidth(p);
    vec2 g = abs(fract(p) - 0.5);
    vec2 l = (1.0 - smoothstep(w - max(d, vec2(1e-6)), w + max(d, vec2(1e-6)), g));
    return max(l.x, l.y);
}

void main(){
    // The bang's shock front (uAtmo 0 -> 1 over 0.4 s from the kick, as in
    // sky.frag) runs out over the floor seconds before the grid fades in. The
    // lines light up as it passes, brightest at the front, all the way out to
    // the horizon. x is how far a point lies beyond the front (negative once
    // the front has passed it). Outside the bang uAtmo sits at 1 or past it.
    vec2 p = vWorld.xz;
    float e = max(sin(uAtmo * 3.14), 0.0) * (1.0 - uAtmo);
    float x = length(p) - 60.0 * uAtmo * (2.0 - uAtmo);
    e *= smoothstep(1.0, -1.0, x) * exp(x * 0.12);
    float fine   = grid_line(p * 0.5, 0.02);       // tight cells
    float coarse = grid_line(p * 0.1, 0.03) * 1.6; // brighter major lines over them
    float pulse  = 0.7 + 0.3 * sin(uTime * 2.0 - length(p) * 0.3); // ripple out from the origin
    float lines  = (fine + coarse) * pulse;

    float dist = length(uCam - vWorld);
    float fade = exp(-dist * 0.05);

    float sh = uGridFade > 0.0 ? keyShadow(vWorld) : 1.0;   // no taps when only the shock shows

    vec3 col = ((uGridColor * lines * sh - uBottom * (1.0 - sh) * 0.6) * uGridFade + uGridColor * lines * e * 3.0) * fade;
    // The finale's sun (sky.frag) mirrored in the floor, as on a wet road: a
    // narrow streak running down from the horizon below it, fading toward us.
    vec3 v = reflect(normalize(vWorld - uCam), vec3(0.0, 1.0, 0.0));
    vec2 s = normalize(-uCam.xz);
    col += uSun.y * vec3(0.8, 0.2, 0.35) * exp(-abs(dot(v.xz, vec2(s.y, -s.x))) * 25.0 - abs(v.y - uSun.x) * 5.0);
    o = vec4(col, 1.0);                       // black between lines; only the glow adds
}
