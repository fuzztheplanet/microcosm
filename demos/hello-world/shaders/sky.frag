#version 450 core
// The backdrop: a colour gradient, a soft glow where the bang happens, and a
// scatter of twinkling stars. It's all computed from each pixel's view ray
// (far.vert's vH), so it's fixed in the world, turns as the camera flies its
// loop and looks the same at any resolution.
in vec4 vH;
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

// (the jungle's per-point hash, spelled the same so the packer stores it once)
float hash(vec3 p){ return fract(sin(dot(p, vec3(12.9898, 78.233, 37.719))) * 43758.5); }

void main(){
    vec3 r = normalize(vH.xyz / vH.w - uCam), v = r + normalize(uCam);
    // The bang's shock front: a ring running out from the seed over the 0.4 s
    // after the kick (uAtmo goes 0 -> 1, set by render_scene1). v is the ray's
    // offset from the seed's direction. As the ring passes it bends the sky
    // like a lens, pulling each ray in toward the seed, so the glow inside it
    // is dragged out into a bright rim and the stars swim. It weakens as it
    // grows. The outer edge is sharp and the inner trail short, so it's a
    // bright band at the front rather than a disc. With a trail 3 x as long,
    // the whole inside lit up evenly. Outside the bang uAtmo sits at 1 or
    // above, where the sine isn't positive.
    float w = length(v) - 1.4 * uAtmo * (2.0 - uAtmo), e = max(sin(uAtmo * 3.14), 0.0) * (1.0 - uAtmo) * exp(-w * w * (w > 0.0 ? 600.0 : 250.0));
    r = normalize(r - v * e);
    // The bottom colour below the horizon and the top colour above it. The
    // level shot of the logo gets the same gradient a screen-space one gave,
    // and the shots looking down on the origin keep the horizon where it is.
    vec3 col = mix(uBottom, uTop, smoothstep(-0.3, 0.45, r.y));

    // Stars: hash a 3D lattice of cells 1/180 across (4 px at 720p), cut
    // through by the sphere of directions, and light one cell in 100. Each
    // star is a soft ball round its cell's centre and looks as wide as the
    // sphere's slice through it, so they come in many sizes, spread evenly in
    // every direction.
    vec3 q = r * 180.0, cell = floor(q);
    if (hash(cell) > 0.99) {
        float tw = 0.5 + 0.5 * sin(uTime * 3.0 + hash(cell + 1.3) * 6.2831);
        col += vec3(0.9, 0.95, 1.0) * tw * 0.7 * smoothstep(0.45, 0.25, length(q - cell - 0.5));
    }
    // Before the bang the whole sky, glow included, is dark, at a quarter
    // (f = 0.25), with the seed as a lone light in it, and the shock front
    // lights it up as it passes. Ahead of the front the light only creeps up
    // (0.6 uAtmo), and behind it (f = 1) it's back in full, so the light
    // outside the ring only ever grows. uAtmo reaches 1 once the front is past
    // the edge of the frame, and from then on floor(uAtmo) holds f at 1,
    // through the outro's 1 -> 2 as well.
    float f = min(smoothstep(0.0, -0.25, w) * 0.75 + 0.25 + 0.6 * uAtmo + floor(uAtmo), 1.0), a = uGlowAmt * f;

    // Radial glow round the origin, where the bang happens, measured by the
    // angle from it so it stays round (and stays on the dot before the bang).
    // It's two exponentials, a wide falloff plus a tight, brighter core, and
    // the wide one's width depends on how bright the glow is. The bang's flare
    // (a past 1.4) fills the sky. As it calms down (a from 0.6 down) it draws
    // in to a halo half as wide round the middle of the crowd. That covers the
    // hooks' glow, and the seed's in the dark before the bang, and leaves the
    // sky round the crowd its own colour instead of a milky haze. The faint
    // glows behind the title and the end card (a under 0.1) keep the wide one.
    float d = length(r + normalize(uCam));
    col = col * f + uGlow * a * (exp(-d * (4.0 + 4.0 * smoothstep(0.1, 0.25, a) * (1.0 - smoothstep(0.6, 1.4, a)))) + 0.25 * exp(-d * 12.0));
    // The finale's sun. It uses uSun, which Scene 1 has no other use for:
    // x is the centre's height over the horizon and y is how present it is.
    // It's a synthwave sunset straight past the origin, behind the title and
    // the end card. The disc is 0.2 in radius, gold at the top to hot pink at
    // the bottom, with its lower part cut by bars that thicken toward the
    // horizon, and the horizon hides whatever has set. A pink haze lies along
    // the horizon and round the disc. The sun rises as the title gathers and
    // sets as it scatters, and it sets through the end card too.
    vec2 s = normalize(-uCam.xz);
    vec3 u = (r - normalize(vec3(s.x, uSun.x, s.y))) / 0.2;
    col = mix(col, mix(vec3(0.7, 0.03, 0.3), vec3(0.8, 0.45, 0.05), u.y * 0.5 + 0.5), uSun.y * clamp((1.0 - length(u)) * 150.0, 0.0, 1.0)
              * step(0.0, r.y) * step((0.6 - u.y) * 0.5, fract(u.y * 4.0)))
        + uSun.y * vec3(0.5, 0.1, 0.3) * (0.4 * exp(-abs(r.y) * 10.0) + 0.3 * exp(-length(u)));
    // The shock's rim, the bang's flash sweeping across the frame, glows rose
    // white (the dawn act's colour, not the glow's beige). The outro takes
    // the sky down to black as uAtmo goes 1 -> 2 (render_scene1), while the
    // seed stays lit on its own.
    o = vec4((col + vec3(4.0, 2.8, 3.0) * e) * min(2.0 - uAtmo, 1.0), 1.0);
}
