#version 450 core
// Scene 2's backdrop. Deep space (black, the galaxy's band, steady stars and a
// hard sun disc in its glare) blends into a bright blue sky as uAtmo climbs,
// so one shader covers the way from orbit down through the atmosphere.
// Full-screen, sharing sky.vert.
in vec2 uv;
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

float hash21(vec2 p){ p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }
layout(binding = 1) uniform sampler3D uNoise;   // 32^3 random bytes, wrapping (main.c's noise_make)
// Value noise, 0..1. The hardware blends the lattice; each cell's fraction
// goes through the cubic first.
float vn3(vec3 p){ vec3 i = floor(p), f = p - i; return texture(uNoise, (i + f * f * (3.0 - 2.0 * f) + 0.5) / 32.0).r; }
float fbm(vec2 p){ float v = 0.0, a = 0.5; for (int i = 0; i < 5; i++){ v += a * vn3(vec3(p, 0.0)); p *= 2.03; a *= 0.5; } return v; }   // 2D: the volume's first layer

void main(){
    // Over the sea (the full sky, uAtmo = 1, from the cloud break on), the sky
    // is laid out by the direction each pixel looks in (d, through the
    // camera's inverse). It's pale at the horizon and deeper overhead, with a
    // glow round the sea's sun (uSun), the same sky ocean.frag mirrors, so
    // sky and sea meet at the horizon in one colour. In space and on the
    // plunge it's laid out on the screen round the backdrop's sun (uSunUV).
    // sd is the distance from the sun's centre, in screen heights (or
    // radians, over the sea).
    vec4 r = inverse(uViewProj) * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    vec3 d = normalize(r.xyz / r.w - uCam);
    float k = floor(uAtmo), sd = k > 0.0 ? length(d - normalize(uSun)) : length((uv - uSunUV) * uRes) / uRes.y;

    // Deep space is black, apart from the galaxy's disc seen edge-on and its
    // many separate stars (below). The disc is a clumpy band of unresolved
    // stars across the sky, warm at its heart and bluer at its edges, split
    // along the middle by dark lanes of dust.
    vec2 p = uv * uRes / uRes.y;
    float b = dot(p, vec2(0.42, 0.9)) - 0.75, mw = exp(-b * b * 18.0);
    vec2 g = vec2(dot(p, vec2(0.9, -0.42)), b);    // along the band, across it
    vec3 space = vec3(0.002, 0.003, 0.007) + mix(vec3(0.35, 0.5, 1.0), vec3(1.0, 0.8, 0.6), mw * mw) * mw * fbm(g * vec2(3.0, 6.0)) * 0.13
               * (1.0 - 0.9 * smoothstep(0.45, 0.65, fbm(g * vec2(4.0, 16.0) + 3.0)) * exp(-b * b * 80.0));
    // along the band, knots of pink glow from the gas its young stars light
    space += vec3(0.6, 0.05, 0.2) * mw * mw * smoothstep(0.55, 0.85, fbm(g * 12.0 + 7.0)) * 0.05;

    // Stars: one in 40 cells of a fine grid (~5 px, and one in 15 in the
    // band's heart), each placed somewhere in its cell. They're steady, since
    // there's no air to twinkle through. Brightness is a steep power of a
    // uniform hash, so many are faint and a few bright, and the colour
    // temperature runs from red-orange to blue-white.
    vec2 sp = p * 200.0, si = floor(sp);
    float h = hash21(si), e = 0.975 - 0.04 * mw;
    if (h > e){
        h = (h - e) / (1.0 - e);
        sp -= si + fract(h * vec2(31.0, 57.0)) * 0.6 + 0.2;
        space += mix(vec3(1.0, 0.78, 0.6), vec3(0.7, 0.82, 1.0), fract(h * 7.0)) * (0.02 + 4.0 * pow(h, 24.0)) * exp(-dot(sp, sp) * 9.0);
    }

    // The few bright ones: one in 30 cells of a grid 8 x coarser, each with a
    // glow and the lens's four spikes round it.
    sp = p * 25.0; si = floor(sp); h = hash21(si + 9.0);
    if (h > 0.967){
        sp -= si + 0.5;
        space += mix(vec3(1.0, 0.8, 0.6), vec3(0.7, 0.85, 1.0), fract(h * 9.0)) * (h - 0.967) * 90.0
               * (exp(-dot(sp, sp) * 900.0) + 0.04 * exp(-length(sp) * 25.0) + 0.1 * exp(-min(abs(sp.x), abs(sp.y)) * 400.0 - length(sp) * 9.0));
    }

    // The sun: a hard white disc (14 px across at 1080p), darker toward its
    // limb. Around it are a tight glare from the lens's scatter, falling off
    // with the square of the angle, and a faint veiling glow rather than a
    // fog.
    space += vec3(1.0, 0.96, 0.9) * (40.0 * sqrt(max(1.0 - sd * sd / 4e-5, 0.0)) + 2e-5 / (sd * sd + 1e-5) + 0.03 * exp(-sd * 10.0));

    // the blue sky we fade into as we enter the atmosphere, with its glow
    // round the sun (the haze's forward scattering)
    vec3 sky = mix(uSky, vec3(0.2, 0.4, 0.85), k > 0.0 ? sqrt(clamp(d.y * 3.0, 0.0, 1.0)) : uv.y)
             + vec3(1.0, 0.95, 0.8) * (1.0 - smoothstep(0.0, 0.4, sd)) * 0.5;

    o = vec4(mix(space, sky, uAtmo), 1.0);
}
