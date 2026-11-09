#version 450 core
// Lighting for the shapes: hemisphere ambient, the bang's glow at the origin
// acting as a light, one key light with its shadow, a sheen with a clear-coat
// glint, a fresnel rim, an emissive term that feeds the bloom, and distance
// fog. Everything stays in linear HDR, and the post stack does the
// tonemapping.
// Scene 1's crowd comes in three materials, encoded in the signs of the
// colour (see build_bodies): pastel plastic, iridescent plastic (green below
// 0) and tinted glass (red below 0). s1_draw draws the glass last, reading a
// copy of the picture behind it. Everything else drawn with this shader is
// the plain plastic.
in vec3 vWorld;
in vec3 vNormal;
in vec4 vColor;
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
layout(binding = 9) uniform sampler2D uScene;          // the picture behind the glass (main.c's jelly_copy)
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

void main(){
    // The key light's depth pass (s1_draw), rendered from the light. Solids
    // cast a full shadow and glass a third of one, by keeping one diagonal of
    // texels in three. keyShadow's 3 x 3 taps lie 2 texels apart, so their
    // diagonal offsets, 2 (i + j), land on each of the three diagonals three
    // times, which averages to a third wherever the taps fall. A pattern with
    // a period of 4 (2-on/2-off stripes, or a 2 x 2 checker) left a +-0.06
    // ripple that crawled as the light moved.
    if (uViewProj == uLightVP) {
        if (vColor.r < 0.0 && mod(gl_FragCoord.x + gl_FragCoord.y, 3.0) < 2.0) discard;
        return;
    }
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCam - vWorld);
    vec3 L = normalize(uLightDir);

    vec3  base     = abs(vColor.rgb);
    float emissive = vColor.a, nv = max(dot(N, V), 0.0);

    // Iridescence: a thin film over the plastic, about 0.4 um thick with index
    // 1.33, like a soap film or a beetle's shell. Light reflected off the
    // film's two faces comes back out of phase by the path through it,
    // 4 pi n d cos(theta_t) / lambda. Some wavelengths reinforce and others
    // cancel, so the hue shifts with the viewing angle: magenta face-on, then
    // gold and green, to cyan at the edge. It's lifted to 0.3-1 and mixed in
    // at 60% over the body's pastel, which keeps it in the palette.
    if (vColor.g < 0.0)
        base = mix(base, 0.65 - 0.35 * cos(6.7 * sqrt(1.0 - (1.0 - nv * nv) * 0.565) / vec3(0.65, 0.55, 0.45)), 0.6);

    // The key light's shadow, sampled a little off the surface along the
    // normal so a face doesn't shadow itself.
    float sh = keyShadow(vWorld + N * 0.04);

    // Ambient: the hemisphere, plus the glow at the origin (where the bang
    // happens), which lights the faces turned toward it by cos / (2 + r^2)
    // (the dot product isn't normalised, so its r cancels the one below).
    // The inner faces are brightest as the bang throws the shapes out, and as
    // the shapes fly off the glow fades to a faint warm light (uGlowAmt).
    float hemi    = 0.5 + 0.5 * N.y, r = length(vWorld);
    vec3  ambient = mix(uGroundColor, uSkyColor, hemi) * 0.65
                  + uGlow * uGlowAmt * 8.0 * max(dot(N, -vWorld), 0.0) / (r * (2.0 + r * r) + 0.01);

    float diff = max(dot(N, L), 0.0);
    // the highlight: a broad sheen with a clear coat's small, hot glint in it
    float nh   = max(dot(N, normalize(L + V)), 0.0);
    float spec = pow(nh, 56.0) * 0.5 + pow(nh, 600.0) * 3.0;
    float rim  = pow(1.0 - nv, 3.0);

    // The key's shadow isn't black: it keeps albedo^2 x 0.4 of the key light,
    // so the light seems to soak through the pastel plastic and the shade
    // stays coloured instead of going grey.
    vec3 col = base * (ambient + diff * vec3(1.0, 0.96, 0.9) * 1.25 * mix(base * 0.4, vec3(1.0), sh));
    // Glass. What's behind it is read from the copy along the ray refracted
    // into it (index 1.33), and the pastel tints it more the deeper the ray
    // goes in. A little of the plastic's own light is kept, so it looks
    // frosted, and by Fresnel the skin mirrors the sky, brightest at the rim.
    // The read point is 0.7 m x cos theta along the ray, over 1 + the surface
    // curvature k (1 / its radius, from the derivatives). The cos theta makes
    // the edge, which turns the ray fastest, bend it least (jo_blob.frag does
    // the same). Dividing by 1 + k does the same for a crease or a cone's
    // tip, so the picture behind is never magnified past a fold. With a fixed
    // 1.5 m, the tips and bevels focused the copy's single-frame pixels into
    // stair-steps 20 pixels high. The distance is capped at 0.7 m because k
    // is one value per 2 x 2 pixels, so the read point jumps from block to
    // block. At 1.2 m those jumps showed as steps in what a cone's
    // flank refracts. At 0.5 m the glass didn't bend the picture enough to
    // look like glass.
    float k = length(fwidth(N)) / length(fwidth(vWorld));
    if (vColor.r < 0.0) {
        vec4 q = uViewProj * vec4(vWorld + refract(-V, N, 0.75) * nv * 0.7 / (1.0 + k), 1.0);
        col = mix(col * 0.15 + texture(uScene, clamp(q.xy / q.w * 0.5 + 0.5, 0.0, 1.0)).rgb * exp(-(1.0 - base) * nv * 2.0),
                  mix(uFogColor * 0.5, uSkyColor * 3.0, smoothstep(-0.3, 0.9, reflect(-V, N).y)),
                  rim);
    }
    col += spec * sh;
    col += rim * uSkyColor * 0.9;
    // Emission goes above 1 so it spills into the bloom pass. It's scaled by
    // the albedo squared, so a glowing pastel (the logo, a flashing shape)
    // keeps its hue through the tonemap instead of burning to white.
    // The crowd's glowing third (emission below 0, set in animate) glows at
    // 3 x in the middle but less toward its silhouette (0.3 + 0.7 cos theta).
    // That way the light and shading still model the shape, and the bloom
    // carries the glow. At 4 x all over it clipped to a flat cut-out on the
    // beat, and at 2 x it looked like plain plastic with no sparkle. The
    // letters and the seed keep their own emission.
    col += base * base * (emissive < 0.0 ? -emissive * (0.3 + 0.7 * nv) * 3.0 : emissive * 4.0);

    // Underwater, a bubble: air in water. Bubbles have alpha -4, below
    // anything the crowd's glow uses (-1.6 at most), and render_scene2 draws
    // them blended, premultiplied. Past the critical angle, where the view
    // meets the skin at sin > 0.75 (the outer quarter of the radius), the
    // water's light is totally reflected. That makes the rim a silvery mirror
    // of the bright water above and around it. It's graded in from sin 0.6, so
    // a bubble only a few pixels across still shows. The inside is clear
    // apart from a sheen, so the picture behind shows through, and the sun
    // glints off it. Mirroring the dim water below at half strength, in a
    // rim a pixel or two wide, made the bubbles all but vanish at 1080p.
    float a = 1.0;
    vec3 R = reflect(-V, N);
    if (emissive < -3.0) {
        a = mix(0.2, 1.0, smoothstep(0.6, 0.8, sqrt(1.0 - nv * nv)));
        col = mix(uFogColor * 3.0, uSkyColor * 7.0, smoothstep(-0.6, 0.8, R.y)) * a + pow(max(dot(R, L), 0.0), 300.0) * 30.0;
    }

    float dist = length(uCam - vWorld);
    float f    = 1.0 - exp(-dist * uFog);
    col = mix(col, uFogColor * a, f);

    o = vec4(col, a);
}
