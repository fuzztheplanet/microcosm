#version 450 core
// A single non-instanced ball with an explicit model matrix, used for the
// planet and for the atmosphere shell. It's a cube with 48 x 48 cells a face,
// built here from gl_VertexID and pushed out onto the sphere. The
// object-space position is passed through so the fragment shader can grow
// continents from 3D noise that stays put as the planet turns.

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
layout(location = 0) uniform mat4 uModel;

out vec3 vObj;      // object-space unit position
out vec3 vWorld;
out vec3 vNormal;

// Corner v % 6 of quad v / 6 in a grid: (0,0) (1,0) (1,1), (0,0) (1,1) (0,1).
// That's counter-clockwise seen from the side the two steps' cross points to.
ivec2 corner(int v){ return ivec2(22, 52) >> v % 6 & 1; }
// Vertex v of a cube's six n x n grids (+X +Y +Z -X -Y -Z, wound to face out),
// on the surface of [-1,1]^3. Each grid is bent by u (e + (1 - e) u^2), which
// keeps 0 and the edges exactly where they were, so neighbouring faces meet
// without a crack. e = 0.75 evens out the cells once they're pushed onto a
// ball; e = 1.5 crowds them toward the edges.
vec3 cube(int v, int n, float e){
    int q = v / 6, f = q / (n * n), r = q % (n * n);
    vec2 u = vec2((ivec2(r % n, r / n) + corner(v)) * 2 - n) / float(n);
    u *= e + (1.0 - e) * u * u;
    vec3 p = f < 3 ? vec3(1.0, u) : vec3(-1.0, -u.x, u.y);
    return f % 3 == 0 ? p : f % 3 == 1 ? p.zxy : p.yzx;
}

void main(){
    vObj    = normalize(cube(gl_VertexID, 48, 0.75));
    vec4 wp = uModel * vec4(vObj, 1.0);
    vWorld  = wp.xyz;
    vNormal = normalize(mat3(uModel) * vObj);
    gl_Position = uViewProj * wp;
}
