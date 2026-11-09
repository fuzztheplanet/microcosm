#version 450 core
// Instanced draw: the transform and colour come per instance, so one draw
// call covers hundreds of shapes. The meshes carry no vertex data, and every
// vertex is built here from gl_VertexID, both for Scene 1's solids
// (uK = SH_* + 1, balls 7) and for Scene 2's seabed (uK = 0), which comes
// from the heights uwplace.comp left in its buffer.
layout(location = 2) in mat4 aModel;   // a mat4 eats four attribute slots: 2,3,4,5
layout(location = 6) in vec4 aColor;   // rgb = albedo, a = how emissive it is

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

layout(location = 1) uniform int uK;
layout(std430, binding = 0) buffer B { float H[]; };   // the seabed's 201 x 201 heights

// The seabed's height at row j, column i of its grid (clamped to the grid).
float bed(int j, int i){ return H[clamp(j, 0, 200) * 201 + clamp(i, 0, 200)]; }

out vec3 vWorld;
out vec3 vNormal;
out vec4 vColor;

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
// Vertex v of a solid of revolution about +Y, in s steps. It's either a torus
// (uK 3: a 0.18 tube round a 0.45 ring, 14 bands round the tube) or a frustum
// made of three bands (base, side and top), with base radius c.x, top radius
// c.y and half height c.z. The frustum gives the cone (4), the cylinder (5)
// and an urchin's spike (6). The normals are the profile's, rotated with it.
void lathe(int v, int s, out vec3 p, out vec3 n){
    int q = v / 6, w = uK == 3 ? 14 : 3, b = min(q % w, 2);
    ivec2 k = corner(v);
    float a = 6.2831853 * float(q / w + k.y) / float(s), f = float(q % w + k.x) * 0.4487989;
    vec3 c = uK == 4 ? vec3(0.5, 0.0, 0.45) : uK == 5 ? vec3(0.4, 0.4, 0.4) : vec3(0.16, 0.0, 0.3);
    vec2 e[4] = vec2[](vec2(0.0, -c.z), vec2(c.x, -c.z), c.yz, vec2(0.0, c.z)), d = e[b + 1] - e[b];
    vec4 r = uK == 3 ? vec4(0.45 + 0.18 * cos(f), 0.18 * sin(f), cos(f), sin(f))
                     : vec4(mix(e[b], e[b + 1], float(k.x)), normalize(vec2(d.y, -d.x)));
    p = vec3(r.x * cos(a), r.y, r.x * sin(a));
    n = vec3(r.z * cos(a), r.w, r.z * sin(a));
}

void main(){
    vec3 p, n;
    int v = gl_VertexID - 2304, j = gl_VertexID / 201, i = gl_VertexID % 201;
    // Seabed vertex (j, i): x by row, z by column, 1.2 m apart. Its normal is
    // the area-weighted sum over its six triangles, as mc_mesh_recompute_normals
    // computes it, for cells split on the (j, i)-(j + 1, i + 1) diagonal.
    if (uK == 0) {
        float a = bed(j - 1, i - 1) - bed(j + 1, i + 1), x = bed(j - 1, i) - bed(j + 1, i), z = bed(j, i - 1) - bed(j, i + 1);
        p = vec3(float(j) * 1.2 - 120.0, H[gl_VertexID], float(i) * 1.2 - 120.0);
        n = normalize(vec3(x + x + a - z, 7.2, z + z + a - x));
    } else if (uK < 3 || uK > 6 || uK == 6 && v < 0) { // a ball of 0.6 (uK 1, 7), a rounded box (2) or an urchin's core (6)
        // Scene 1's ball (uK 7) has 12 x 12 cells a face, for a 48-sided
        // outline. At 6 x 6, a big one close to the lens showed its 24
        // corners. Scene 2's bubbles and specks keep 6 x 6 (uK 1). The rounded
        // box and the urchin core use 8 x 8, because the box's bevels at
        // 12 x 12 cost 5 ms on the UHD 620 with the end card's 350 letter
        // cubes. Spikes are 16-sided.
        vec3 c = cube(gl_VertexID, uK > 6 ? 12 : uK == 1 ? 6 : 8, uK == 2 ? 1.5 : 0.75) * 0.5, i = clamp(c, -0.3, 0.3) * float(uK == 2);
        n = normalize(c - i);                       // the box: a 0.6 core grown by 0.2; the urchin's core: a ball of 0.5
        p = i + (uK == 2 ? 0.2 : uK == 6 ? 0.5 : 0.6) * n;
    } else {
        lathe(uK == 6 ? v : gl_VertexID, uK == 6 ? 16 : 28, p, n);
        if (uK == 6) {                               // 18 spikes on a Fibonacci spiral round the core
            int k = v / 288;
            float y = 1.0 - float(2 * k + 1) / 18.0, s = sqrt(1.0 - y * y), h = float(k) * 2.3999632;
            vec3 d = vec3(s * cos(h), y, s * sin(h)), a = normalize(vec3(d.z, 0.0, -d.x));
            mat3 M = mat3(a, d, cross(a, d));        // +Y onto d
            p = M * p + d * 0.7;             // base sunk 0.1 into the core, so they meet in a curve, not at a lip
            n = M * n;
        }
    }
    vec4 wp = aModel * vec4(p, 1.0);
    vWorld  = wp.xyz;
    vNormal = mat3(aModel) * n;
    vColor  = aColor;
    gl_Position = uViewProj * wp;
}
