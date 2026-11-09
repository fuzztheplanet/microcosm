#version 450 core
// The blue-and-green planet. Continents, oceans, ice caps and drifting cloud
// are all fbm noise over the object-space direction, with one broad field
// warping the rest. That gives wandering coasts, forest and desert, ice caps
// with a ragged edge, and cloud in fronts and swirls that casts soft shadows.
// A single sun lights it from the side the space backdrop's sun is on. There's
// a day/night terminator, with the sunlight reddening through the air toward
// it (air(), which atmo.frag's halo shares), and a Fresnel glint off the sea
// only. The air between the ground and the eye dims the surface
// and adds the light it scatters: a blue haze that thickens to a pale limb,
// warm at the terminator and absent at night.
in vec3 vObj;
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

layout(binding = 1) uniform sampler3D uNoise;   // 32^3 random bytes, wrapping (main.c's noise_make)
// Value noise, 0..1. The hardware blends the lattice; each cell's fraction
// goes through the cubic first.
float vn3(vec3 p){ vec3 i = floor(p), f = p - i; return texture(uNoise, (i + f * f * (3.0 - 2.0 * f) + 0.5) / 32.0).r; }
// Each octave is rotated (and scaled 2.02x) so the value noise's lattice never
// lines up.
float fbm3(vec3 p){ float v = 0.0, a = 0.5; for (int i = 0; i < 6; i++){ v += a * vn3(p); p = mat3(-1.2, 1.6, 0.4, -1.6, -1.0, -0.8, -0.4, -0.8, 1.8) * p; a *= 0.5; } return v; }

// Sunlight after its way down through the air, where the sun stands s high
// (s is the cosine of its angle from straight up). Rayleigh extinction takes
// blue first. Ozone adds its own: its Chappuis band takes green and red and
// almost no blue, a fifth to a quarter more in those channels (the column
// ratios of Bruneton's 2017 model). The air mass grows as 1 / s^2 from
// s + 0.1. The planet's curve keeps it finite at the horizon, and past that
// the ground is in the planet's shadow anyway. The light is white overhead,
// faintly warm at 30 degrees, orange low down and red at the terminator, and
// then it's dark. Without the ozone, the low sun went through yellow and
// then, against the air's bluish scatter, green, which added one more band of
// rainbow on the way down. Shared with atmo.frag.
vec3 air(float s){ s = max(s + 0.1, 0.005); return exp(-vec3(0.0073, 0.0176, 0.033) / (s * s)); }

void main(){
    // The sun. In space it comes from the side the backdrop's sun is on, about
    // 68 degrees off the eye, so a third of the disc is night. uSun, the sea's
    // sun, stands nearly behind the eye and would light the disc full-face.
    // On the plunge, as the sky turns blue (uAtmo), the sun swings back round
    // behind the eye, so the night side doesn't hang in a blue sky and we
    // fall into a sunny noon. atmo.frag turns it the same way.
    vec3 N = normalize(vNormal), dir = normalize(vObj), L = normalize(uSun * mix(vec3(1.7, 1.5, 0.1), vec3(1.0), uAtmo)), V = normalize(uCam - vWorld);

    // Continents: the terrain noise is warped by a broader one, so the coasts
    // wander and break up. The broad one also decides which land is dry.
    float w = fbm3(dir * 1.6 + 7.0), h = fbm3(dir * 2.3 + w + 5.0), sea = 0.54;
    float land = smoothstep(sea, sea + 0.012, h), NL = dot(N, L), nv = max(dot(N, V), 0.0);

    vec3 ocean = mix(vec3(0.004, 0.025, 0.09), vec3(0.02, 0.13, 0.22), smoothstep(sea - 0.04, sea, h));   // deep, shelf
    vec3 grn   = mix(vec3(0.07, 0.19, 0.05), vec3(0.43, 0.35, 0.21), smoothstep(0.42, 0.6, w));          // forest, desert
    grn = mix(grn, vec3(0.30, 0.27, 0.22), smoothstep(sea + 0.1, sea + 0.2, h));                         // bare uplands
    vec3 base  = mix(ocean, grn, land);

    // ice caps with a ragged edge, reaching further down over the land and the
    // high ground than over the deep sea
    float ice = smoothstep(0.8, 0.85, abs(dir.y) + 0.4 * (w - 0.5) + 0.6 * (h - sea));
    base = mix(base, vec3(0.85, 0.9, 0.95), ice);

    // Drifting cloud. Its field is squeezed north-south so it streams in
    // bands. The land's two fields push it about, two of its own swirl it
    // (fronts and eddies), and one six times finer frays it. The ground is
    // shaded where the cloud stands between it and the sun.
    vec3 q = dir * vec3(3.0, 5.0, 3.0) + vec3(uTime * 0.015, 0.0, 9.0) + 1.5 * vec3(w, h, w);
    q += 0.8 * vec3(fbm3(q * 2.0 + 3.0), 0.0, fbm3(q * 2.0 - 4.0));
    float c = fbm3(q * 6.0) * 0.3, cloud = smoothstep(0.64, 0.82, fbm3(q) + c), shade = smoothstep(0.64, 0.82, fbm3(q + 0.06 * L) + c);
    base *= 1.0 - 0.5 * shade;
    base = mix(base, vec3(1.0 - 0.6 * max(shade - cloud, 0.0)), cloud * 0.85);   // cloud, darker on the side away from the sun

    // The sun through the air (air() reddens it toward the terminator), and
    // the sea's glint, Fresnel-weighted. The glint is a broad sheen, the way
    // wind-roughened water looks from orbit, rather than a tight white spot.
    // Relief: the land's height above the sea becomes a normal through its
    // screen-space slopes (the surface gradient), which lights the ground
    // wherever there's no cloud. The ranges catch the sun on one side and are
    // in shade on the other, most strongly toward the terminator. Edge-on at
    // the limb the footprint's area can reach 0, and 0 / 0 is a NaN the TAA
    // would keep, so it's floored.
    float e = max(h - sea, 0.0);
    vec3 X = dFdx(vWorld), Y = dFdy(vWorld), a = cross(Y, N), b = cross(N, X);
    a = mix(normalize(N - 4.0 * (dFdxFine(e) * a + dFdyFine(e) * b) / max(dot(X, a), 1e-9)), N, cloud);
    vec3 A = air(NL), sun = A * max(dot(a, L), 0.0), E = exp(-vec3(0.02, 0.04, 0.09) / max(nv, 0.03));
    vec3 col = base * (0.015 + sun * 1.1);
    col += (1.0 - land) * (1.0 - cloud) * (1.0 - ice) * sun * (0.02 + 0.98 * pow(1.0 - nv, 5.0)) * (0.3 + 5.0 * pow(max(dot(N, normalize(L + V)), 0.0), 60.0));

    // The air between the surface and the eye. E is what it lets through over
    // about 1 / nv air masses, a thinner layer than the sun's path above. It
    // dims what lies below and adds the sunlight it scatters, as single
    // scattering in a flat layer, where m / (m + nv) is the share of the path
    // the sun reaches. That light is blue where the path is short, whiter
    // toward the limb where it's long, faintly warm in the terminator's
    // reddened light, and nothing on the night side.
    // The scattered light takes only half the sun's tint. The other half
    // keeps the red's strength but not its colour, as the sky's own light
    // would after scattering again. A.r is used because red is the last to
    // go, so it still fades out in the same place. With the full tint, the
    // limb ran white, yellow, orange and red in a band a few pixels wide at
    // each end of the terminator, as saturated as a rainbow. The halo and
    // the disc share the half tint, so they still meet.
    float m = max(NL + 0.1, 0.005);
    o = vec4(col * E + (A + A.r) * (1.0 - E) * 0.3 * m / (m + nv), 1.0);
}
