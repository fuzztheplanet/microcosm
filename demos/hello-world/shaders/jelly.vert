#version 450 core
// Moon jellies, drifting in the light over the blob's route where the water
// is emptiest: story seconds 40-55, after the whale and before the predator.
// Each is built here from gl_VertexID. The bell is a flattened dome of 16 x 6
// cells whose rim draws in and out every ~1.6 s. Under it hang 12 strands of
// 10 segments, turned to face us: 8 fine tentacles from the rim and 4 broad,
// frilled oral arms from the middle, all swaying. Each jelly is tilted its own
// way, and its bell is 0.9-1.6 m in radius (r).
// gl_InstanceID places it over the point of the blob's path (uwplace.comp's,
// W[25977], one sample every 0.127595 story seconds) that the blob passes at
// the jelly's own moment. It sits 4-12 m to one side, with the bell's centre
// 3.9-5.9 m up, and rises 0.1 m a second (2.7-6.8 m over the window).
// Whatever the hash gives (GPU sines differ), its height keeps it clear of
// everything else. Its arms' tips (at most 2.5 m below it) stay at least 1 m
// above the camera, the blob and the schools' fish (none above -0.8 m from
// story second 34 to 58). Its crown stays 0.8 m under the surface (8.5 m).
// The whale's lane is far off and done with by then, and the props stand on
// the seabed. Drawn additively (jelly.frag), both sides, writing no depth.
layout(std430, binding = 0) readonly buffer U { vec4 W[]; };
layout(location = 3) uniform vec2 uStory;   // x: the act's story seconds (main.c's uw_u)

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
out vec2 vB;        // the bell: round it (radians), down it (0 crown .. 1 rim); a strand: (which, 1 + down it)

void main(){
    int i = gl_InstanceID, v = gl_VertexID, q = v / 6;
    float h = fract(sin(float(i) * 78.233) * 43758.547), g = fract(h * 13.7), u = uStory.x, r = 0.9 + 0.7 * g;
    vec3 c = W[25977 + int((40.0 + 15.0 * h) / .127595)].xyz * vec3(1.0, 0.0, 1.0) + vec3(sin(g * 40.0), 0.0, cos(g * 40.0)) * (4.0 + 8.0 * g)
           + vec3(0.0, 3.9 + 2.0 * fract(h * 7.1) + 0.1 * (u - 48.0), 0.0), p;
    // the cell's corner (as planet.vert's corner()); the pulse draws the rim in
    ivec2 k = ivec2(q % 16, q / 16) + (ivec2(22, 52) >> v % 6 & 1);
    float a = float(k.x) * 0.3927, t = float(k.y) / 6.0, pu = 0.5 + 0.5 * sin(u * 4.0 + h * 40.0), s = sin(t * 1.5) * (1.0 - 0.15 * t * pu);
    p = vec3(cos(a) * s, 0.55 * cos(t * 1.5), sin(a) * s);
    vNormal = p * vec3(1.0, 1.8, 1.0);
    vB = vec2(a, t);
    if (q >= 96){                                  // a strand: 0-7 the rim's tentacles, 8-11 the arms
        q -= 96;
        ivec2 e = ivec2(22, 52) >> v % 6 & 1;      // the corner: across it, down it
        float o = float(q < 80), z = float(q % 10 + e.y) * 0.1;
        a = float(q / 10) * (o > 0.0 ? 0.785 : 1.571) + 0.3;
        p = vec3(cos(a), 0.0, sin(a)) * mix(0.12, 0.98 - 0.15 * pu, o) + vec3(0.0, mix(0.3, 0.04, o) - z * mix(1.2, 0.7, o), 0.0)
          + vec3(sin(u * 1.3 + z * 5.0 + a), 0.0, cos(u * 1.1 + z * 4.0 + a)) * 0.15 * z
          + normalize(cross(vec3(0.0, 1.0, 0.0), c - uCam)) * (float(e.x) - 0.5) * mix(0.12 * (1.0 - 0.5 * z) * (1.0 + 0.4 * sin(z * 30.0)), 0.025, o);
        vNormal = c - uCam;
        vB = vec2(o, 1.0 + z);
    }
    // tilted its own way, up to ~15 degrees
    float x = (h - 0.5) * 0.5, y = (g - 0.5) * 0.5;
    p.xy = mat2(cos(x), sin(x), -sin(x), cos(x)) * p.xy;
    p.zy = mat2(cos(y), sin(y), -sin(y), cos(y)) * p.zy;
    vWorld = c + p * r;
    gl_Position = uViewProj * vec4(vWorld, 1.0);
}
