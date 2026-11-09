#version 450 core
// A moon jelly (jelly.vert), added over the water behind it. The bell's tissue
// is clear and shows where it's seen edge-on, at its outline and its rim, and
// four pink horseshoe gonads sit around its crown. It's lit by the sunlight
// left at its depth and fades into the water with distance like everything
// else. Only the fog's extinction applies here, though, because the light the
// water scatters is already drawn in what lies behind. The jelly fades in and
// out over its window in the act. During the brief darker passage, turquoise
// light travels slowly along the bell's radial canals and its oral arms.
in vec3 vWorld;
in vec3 vNormal;
in vec2 vB;
out vec4 o;
layout(location = 3) uniform vec2 uStory;

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

// ---- the water (identical in every underwater shader) ----
// The sea's surface is S m up, and the camera stays under it. The water
// absorbs red first and then green, like Jerlov's oceanic water types. wx() is
// its extinction per metre, murky early on and clear later. The sunlight left
// at a given depth falls off at half that rate and is 1 at the blob's depth,
// 13 m down. uFogColor arrives already scaled to the eye's depth.
// cw() is the caustic web, the light the surface's ripples focus: the squared
// distance to the nearest of n rotated, drifting lattices, at q on the surface
// (scaled). It's read where the sun's ray left the surface, so the caustics on
// the seabed and the shafts in the water are one field.
// wp() is how much light the water scatters along a ray d. Half of it goes
// evenly and half forward, by Schlick's Henyey-Greenstein with g 0.4, which
// gives 1 looking across the light, about 3.4 toward the sun and 0.7 away.
const float S = 8.5;
const mat3 CM = mat3(-2.0,-1.0,2.0, 3.0,-2.0,1.0, 1.0,2.0,2.0);
vec3 wx(){ return mix(vec3(0.18,0.07,0.05), vec3(0.06,0.018,0.013), uClarity); }
float cw(vec2 q, int n){
    vec3 a = vec3(q, uTime * 0.3) * CM * 0.5, f;
    float m = 1.0;
    for (int i = 0; i < n; i++, a *= CM * (0.4 - 0.1 * float(i))) { f = 0.5 - fract(a); m = min(m, dot(f, f)); }
    return m;
}
float wp(vec3 d){ float k = 1.0 - 0.585 * dot(d, uLightDir); return 0.5 + 0.5 / (k * k); }
// ---- end water ----

void main(){
    vec3 d = vWorld - uCam;
    float s = length(d), e = 1.0 - abs(dot(normalize(vNormal), d / s)), a = (floor(vB.x / 1.5708) + 0.5) * 1.5708;
    // a gonad: a ring of radius 0.12 round each of 4 points 0.3 out on the crown
    float r = abs(length(vB.y * vec2(cos(vB.x), sin(vB.x)) - 0.3 * vec2(cos(a), sin(a))) - 0.12);
    vec3 col = vB.y > 1.0 ? vec3(0.45, 0.65, 0.8) * (0.25 - 0.15 * vB.x) * (2.0 - vB.y)   // a strand, fading to its tip
             : vec3(0.45, 0.65, 0.8) * (0.03 + 0.6 * e * e * e + 0.25 * smoothstep(0.8, 1.0, vB.y)) + vec3(0.9, 0.4, 0.6) * 0.3 * smoothstep(0.04, 0.0, r);
    float bio = smoothstep(38.0, 43.0, uStory.x) * (1.0 - smoothstep(51.0, 56.0, uStory.x));
    float vein = abs(sin(4.0 * vB.x + 0.15 * sin(9.0 * vB.y - uStory.x * 0.6)));
    float width = fwidth(vein);     // derivatives must run before the bell/strand branch
    float light = vB.y > 1.0 ? (0.12 - 0.08 * vB.x) * (2.0 - vB.y)
                : (1.0 - smoothstep(0.06, 0.14 + width, vein)) * smoothstep(0.1, 0.5, vB.y);
    col += vec3(0.12, 0.9, 1.0) * bio * light * (0.65 + 0.35 * sin(uStory.x * 0.8 - vB.y * 5.0 + vWorld.x * 0.2));
    o = vec4(col * exp(wx() * (0.5 * vWorld.y + 2.25 - s)) * smoothstep(36.0, 39.0, uStory.x) * smoothstep(57.0, 54.0, uStory.x), 0.0);
}
