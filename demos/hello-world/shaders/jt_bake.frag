#version 450 core
// Bakes scene 3's materials once at load into mipmapped tiles that wrap with
// repeat (every channel is periodic). They keep their detail at any distance
// without shimmering, and cost one texture read instead of noise loops.
// uType picks the tile:
// 0 bark, a ~0.9 m square: (height, moss/lichen mask, slope across, slope
//   along). Ridges wandering up the trunk, flakes and lenticels.
// 1 leaves, four 256 px variants (across, along) in a 2 x 2 sheet: (veins,
//   tint, thickness, shape). Each has a drip tip, a finely toothed margin and
//   insect bites.
// 2 sand, a 2 m square: (height, grain tone, specks, -). Wind ripples over
//   swells, grains, and bits of shell and dark mineral.
// The vertex shader is sky.vert.
in vec2 uv; out vec4 o;
layout(location = 0) uniform int uType;

float hs(vec2 p){ p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }
// value noise and fbm, periodic over q cells
float pn(vec2 p, vec2 q){
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hs(mod(i, q)), hs(mod(i + vec2(1, 0), q)), f.x),
               mix(hs(mod(i + vec2(0, 1), q)), hs(mod(i + 1.0, q)), f.x), f.y);
}
float fb(vec2 p, vec2 q){
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; i++) { v += a * pn(p, q); p *= 2.0; q *= 2.0; a *= 0.5; }
    return v;
}

// Bark: shallow ridges wandering up the wood, covered in flakes and lenticels.
// The flakes are stretched Voronoi cells, each lifted by its own amount and
// cracked round its rim. Lenticels are short dark slits across the grain.
// Returns the height, 0..1.
float bark(vec2 p){
    float x = p.x * 10.0 + 1.5 * fb(p * vec2(2.0, 1.0), vec2(2.0, 1.0)) + 0.3 * pn(p * vec2(16.0, 6.0), vec2(16.0, 6.0));
    vec2 c = p * vec2(7.0, 4.0), ci = floor(c), e = vec2(9.0), id;
    for (int y = -1; y <= 1; y++) for (int z = -1; z <= 1; z++) {           // the flakes (F1, F2)
        vec2 g = ci + vec2(z, y), k = mod(g, vec2(7.0, 4.0));
        float l = length((g + vec2(hs(k), hs(k + 7.3)) - c) * vec2(1.0, 0.6));
        if (l < e.x) { e = vec2(l, e.x); id = k; } else e.y = min(e.y, l);
    }
    vec2 l = p * vec2(18.0, 46.0), li = mod(floor(l), vec2(18.0, 46.0)), lf = fract(l) - 0.5 - (vec2(hs(li), hs(li + 3.7)) - 0.5) * 0.5;
    return 0.45 * sqrt(1.0 - abs(fract(x) * 2.0 - 1.0)) + 0.4 * smoothstep(0.0, 0.12, e.y - e.x) * (0.6 + 0.4 * hs(id))
         + 0.15 * fb(p * vec2(40.0, 6.0), vec2(40.0, 6.0)) - 0.25 * smoothstep(0.3, 0.1, length(lf * vec2(1.0, 4.5)) * (hs(li + 9.1) > 0.55 ? 1.0 : 9.0));
}

void main(){
    vec2 p = uv;
    if (uType == 0) {
        // the height, and its slopes along the tile (packed: 1/100 per unit)
        float h = bark(p), e = 1.0 / 512.0;
        o = vec4(h, fb(p * 2.0, vec2(2.0)), 0.5 + 0.005 * (bark(p + vec2(e, 0.0)) - bark(p - vec2(e, 0.0))) / e,
                                            0.5 + 0.005 * (bark(p + vec2(0.0, e)) - bark(p - vec2(0.0, e))) / e);
    } else if (uType == 1) {
        // which variant (2 x 2), then (across 0..1 with the midrib at 0.5, along base..tip)
        vec2 v = floor(p * 2.0), q = fract(p * 2.0);
        float t = q.y * 1.08 - 0.04, a = abs(q.x * 2.0 - 1.0), k = hs(v + 0.5);
        float w = (0.44 + 0.18 * k) * pow(max(sin(3.1416 * pow(clamp(t, 0.0, 1.0), 0.8)), 0.0), 0.7 + 0.5 * k)
                * (1.0 - 0.45 * smoothstep(0.7, 1.0, t)) * (1.0 - 0.025 * pow(abs(sin(t * 150.0)), 4.0));
        float m = smoothstep(0.0, 0.012, w - a), e = m;
        for (int i = 0; i < 3; i++) {                                           // insect bites
            vec2 c = vec2(hs(v + float(i)), hs(v + float(i) + 0.3));
            vec2 d = vec2(q.x * 2.0 - 1.0 - sign(c.x - 0.5) * w * (0.7 + 0.5 * c.y), t - 0.2 - 0.6 * c.y);
            float b = 0.03 + 0.07 * hs(v - float(i)) * step(0.4, k + c.x * 0.5);
            m *= smoothstep(b, b + 0.012, length(d));
            e = min(e, smoothstep(b, b + 0.06, length(d)));
        }
        // veins: the midrib, and side veins every ~1/10 curving out to the margin
        float s = fract(t * 10.0 - a * 3.0 + 0.2 * k), r = a / max(w, 0.01);
        float vn = max(smoothstep(0.012 * (1.4 - t), 0.0, abs(q.x - 0.5)),
                       0.7 * smoothstep(0.1, 0.0, min(s, 1.0 - s)) * smoothstep(0.95, 0.5, r));
        // tint: mottling, and the margins and bites yellowed and browned
        o = vec4(vn, clamp(0.5 + 0.6 * (fb(q * 5.0 + v * 3.0, vec2(5.0)) - 0.5) + 0.35 * (1.0 - e) + 0.3 * smoothstep(0.85, 1.0, r), 0.0, 1.0),
                 1.0 - 0.6 * r + 0.3 * vn, m);
    } else {
        // ripples (8 cm, steep on their lee side) over swells, then grains
        float ph = 6.2832 * (7.0 * p.x + 24.0 * p.y + 1.2 * fb(p * 3.0, vec2(3.0)) + 0.3 * pn(p * 12.0, vec2(12.0)));
        float rp = pow(0.5 + 0.5 * sin(ph + 0.7 * sin(ph)), 1.5) * smoothstep(0.2, 0.7, fb(p * 4.0 + 5.0, vec2(4.0)));
        float g = hs(floor(p * 512.0) + 0.5);
        o = vec4(0.8 * rp + 0.2 * pn(p * 256.0, vec2(256.0)), 0.5 + 0.5 * pn(p * 512.0, vec2(512.0)) - 0.25 * g,
                 0.5 + 0.5 * step(0.993, g) - 0.5 * step(g, 0.01), 1.0);
    }
}
