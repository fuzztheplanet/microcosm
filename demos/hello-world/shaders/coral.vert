#version 450 core
// The coral, built here from gl_VertexID (the mesh is blank). There are four
// forms, picked by the instance's aColor.a.
// Form 0 is a branching staghorn. A short trunk forks four ways and each fork
// splits twice more, giving 29 branches. Each is a tapered six-sided tube of
// 36 vertices, and the whole head is about a unit tall and half as wide. A
// branch's children start at its tip with the tip's radius, so the joints
// overlap and no seam shows. They're spread around it at even (jittered)
// angles, tilted out from it and pulled up, and each generation is a little
// shorter and thinner.
// The other three forms are one surface of revolution, 16 sides around by 10
// rings from pole to pole, using the same 174 quads with the last few folded
// onto the lower pole. It's an ellipsoid with radii K.xyz and its centre K.w
// up. Its lower half is pulled down onto the sand (by the sixth power of how
// far below the middle a ring lies), and two waves around the rim, offset by
// the hue, make it irregular. That gives a brain coral's half-sunk dome, a
// table coral's flat plate on the cone of its stalk and a sea fan's thin
// upright disc.
// All the forms stay within the unit reach the placement gives a head
// (uwplace.comp's uR.y). They read across the reef at flight speed, where a
// staghorn alone looked like a sprig. Every head is turned and scaled by its
// instance and drawn two-sided. It's lit by coral.frag, which reads the form
// and how high up the head a point is from vColor.a, and shadow.frag casts its
// shadow.
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

out vec3 vWorld;
out vec3 vNormal;
out vec4 vColor;

float h(int n){ return fract(sin(float(n) * 78.233) * 43758.547); }
const vec4 K[3] = vec4[](vec4(.75, .5, .75, 0.), vec4(.76, .1, .76, .5), vec4(.7, .5, .04, .45));

void main(){
    int v = gl_VertexID, b = v / 36, q = v / 6, c = v % 6, n = 0, F = int(aColor.a);
    // the quad's corner: round (i) and along (j)
    int i = int(c == 1 || c == 2 || c == 4), j = int(c == 2 || c == 4 || c == 5);
    // the path to branch b: the trunk (0), then the child index at each level
    ivec3 w = b < 1 ? ivec3(-1) : b < 5 ? ivec3(b - 1, -1, -1) : b < 13 ? ivec3((b - 5) / 2, (b - 5) % 2, -1)
                                 : ivec3((b - 13) / 4, (b - 13) / 2 % 2, (b - 13) % 2);
    vec3 p = vec3(0.), d = vec3(0., 1., 0.), u = vec3(1., 0., 0.), e = vec3(0., 0., 1.);
    float l = .3, r = .085, t = .085 * .85;
    for (int k = 0; k < 3 && w[k] >= 0; ++k) {
        bool f = k == 0;                              // forking off the trunk: four ways, spread wider, no pull
        float a = h(n * 7);                           // the parent's turn, then this child's place round it
        n = n * 4 + w[k] + 1;
        a = 6.2832 * (a + float(w[k]) / (f ? 4. : 2.)) + (f ? .3 : .4) * (2. * h(n * 7 + 1) - 1.);
        float g = mix(.42, f ? .62 : .72, h(n * 7 + 2));
        p += d * l;
        d = normalize(d * cos(g) + (u * cos(a) + e * sin(a)) * sin(g) + vec3(0., f ? 0. : .22, 0.));
        u = normalize(cross(d, abs(d.y) > .9 ? vec3(1., 0., 0.) : vec3(0., 1., 0.)));
        e = cross(d, u);
        l *= f ? mix(.8667, 1.1333, h(n * 7 + 3)) : mix(.66, .82, h(n * 7 + 3));
        r = t;
        t *= f ? .5882 : .62;
    }
    // the corner: round the tube (six sides) and along it (base, tip)
    float s = float(q % 6 + i) * 1.0472;
    vec3 o = u * cos(s) + e * sin(s);
    p += d * l * float(j) + o * mix(r, t, float(j));
    vec3 C = aColor.rgb;
    float sh = 1.;
    if (F > 0) {
        // the other forms: ring j of 10 from the top pole, side i of 16 round it
        vec4 k = K[F - 1];
        t = min(float(q / 16 + j), 10.) * .31416;
        s = float(q % 16 + i) * .3927;
        o = vec3(sin(t) * cos(s), cos(t), sin(t) * sin(s));
        // A table's colour is muted to olive, ochre, dusty purple or rose (a
        // dome's less so), with a paler growing rim. Whatever faces down (a
        // table's underside and stalk, a dome's foot) is in its own shade.
        C = mix(vec3(dot(C, vec3(.3, .5, .2))) * vec3(.9, .8, .6), C, F == 2 ? .45 : F == 1 ? .8 : 1.);
        if (F == 2) C = mix(C, vec3(.85, .8, .65), smoothstep(.75, .95, sin(t)) * step(0., o.y) * .6);
        sh = mix(.3, 1., smoothstep(-.2, .4, o.y));
        p = o * k.xyz;
        p.xz *= 1. + .1 * sin(s * 2. + aColor.g * 9.) + .06 * sin(s * 3. + aColor.r * 9.);   // its rim irregular
        p.y += k.w * (1. - pow(max(-o.y, 0.), 6.));
        o /= k.xyz;
    }
    vec4 wp = aModel * vec4(p, 1.);
    vWorld  = wp.xyz;
    vNormal = mat3(aModel) * o;
    // rgb: its hue, darker toward the sand, each head a little lighter or
    // darker than the next. a: its form plus how high up the head this is.
    vColor  = vec4(C * sh * (.4 + .6 * smoothstep(0., .5, p.y)) * (.7 + .5 * fract(sin(dot(aModel[3].xz, vec2(12.9898, 78.233))) * 43758.5)),
                   aColor.a + min(p.y, .99));
    gl_Position = uViewProj * wp;
}
