#version 450 core
// Bakes four tiled materials once at load, so some objects are textured
// instead of shaded procedurally every frame. uType 0 = rippled sand and
// 1 = mottled cracked rock (the seabed's two), 2 = fish scales, 3 = whale
// skin. For sand and rock alpha stores height, and for the skins it stores
// roughness. Four shifted evaluations blended with smooth weights make every
// channel periodic. Shares sky.vert.
in vec2 uv; out vec4 o;
layout(location = 0) uniform int uType;

float hash21(vec2 p){ p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }
layout(binding = 1) uniform sampler3D uNoise;   // 32^3 random bytes, wrapping (main.c's noise_make)
// Value noise, 0..1. The hardware blends the lattice; each cell's fraction
// goes through the cubic first.
float vn3(vec3 p){ vec3 i = floor(p), f = p - i; return texture(uNoise, (i + f * f * (3.0 - 2.0 * f) + 0.5) / 32.0).r; }
float fbm(vec2 p){ float v = 0.0, a = 0.5; for (int i = 0; i < 6; i++){ v += a * vn3(vec3(p, 0.0)); p = p*2.03 + 1.7; a *= 0.5; } return v; }   // 2D: the volume's first layer
float cells(vec2 p){
    vec2 ip = floor(p), fp = fract(p); float md = 1.0;
    for (int y=-1;y<=1;y++) for (int x=-1;x<=1;x++){
        vec2 g = vec2(x,y), o2 = vec2(hash21(ip+g), hash21(ip+g+3.1));
        md = min(md, length(g + o2 - fp));
    }
    return md;
}
vec4 material(vec2 uv){
    vec4 result;
    float n = fbm(uv * 10.0), n2 = fbm(uv * 32.0);
    if (uType == 0){                                    // sand: ripples + grain
        float rip = sin(uv.x * 55.0 + fbm(uv * 4.0) * 9.0) * 0.5 + 0.5;
        vec3 c = mix(vec3(0.52,0.47,0.36), vec3(0.72,0.66,0.52), n);
        c *= 0.82 + 0.18 * rip;
        c += (n2 - 0.5) * 0.06;
        result = vec4(c, (n + 0.5*n2)/1.5);
    } else if (uType == 1){                             // rock: grey, cracked
        float cr = smoothstep(0.03, 0.11, cells(uv * 7.0));
        vec3 c = mix(vec3(0.2,0.18,0.15), vec3(0.5,0.46,0.4), n);
        c *= 0.55 + 0.45 * cr;
        c += (n2 - 0.5) * 0.05;
        result = vec4(c, (n + 0.5*n2)/1.5);
    } else if (uType == 2){                             // fish scales (tileable brick)
        vec2 g = uv * vec2(8.0, 8.0);
        g.x += 0.5 * mod(floor(g.y), 2.0);              // brick offset, period 2 -> seamless
        vec2 cell = fract(g) - 0.5;
        float d = length(cell * vec2(1.0, 1.3));
        float scale = (1.0 - smoothstep(0.30, 0.52, d));        // 1 on the scale, 0 in the gap
        float edge  = smoothstep(0.30, 0.47, d) * scale;
        float cid = hash21(floor(g) + 3.3);
        vec3 silver = mix(vec3(0.40,0.55,0.70), vec3(0.64,0.76,0.88), cid);
        vec3 c = mix(vec3(0.08,0.14,0.20), silver, scale);
        c *= 1.0 - 0.40 * edge;                         // rim shading -> height for the frag
        c += (n2 - 0.5) * 0.04;
        result = vec4(c, 0.32);                        // .a = roughness (glossy scales)
    } else {                                            // whale skin: mottled grey + speckle
        float mo = fbm(uv * 3.0);
        vec3 c = mix(vec3(0.16,0.19,0.23), vec3(0.34,0.37,0.40), fbm(uv * 7.0));
        c = mix(c, c * 1.30, smoothstep(0.55, 0.80, mo) * 0.5);
        c = mix(c, c * 0.70, (1.0 - smoothstep(0.00, 0.20, fbm(uv * 3.0 + 5.0))) * 0.4);
        float bump = (1.0 - smoothstep(0.02, 0.12, cells(uv * 14.0)));   // barnacles / tubercles
        c = mix(c, vec3(0.58,0.56,0.50), bump * 0.5);
        c += (n2 - 0.5) * 0.03;
        result = vec4(c, 0.55);
    }
    return result;
}
void main(){
    vec2 w = uv*uv*(3.0-2.0*uv);
    o = mix(mix(material(uv),material(uv-vec2(1,0)),w.x),
            mix(material(uv-vec2(0,1)),material(uv-vec2(1,1)),w.x),w.y);
}
