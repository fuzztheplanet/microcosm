#version 450 core
// Underwater surfaces: the seabed (object.vert) and the boulders (rock.vert).
// Per-instance material id (vColor.a): 0 = triplanar baked sand, 1 = baked rock,
// 2 = procedural rock. Lighting is a Cook-Torrance GGX BRDF plus an analytic
// underwater image-based light, with detail normals, the water's caustic web and
// its red-first extinction. The sand darkens round each boulder's foot (its
// contact shade, below). The uQ "evolution" ramp (0 early, 1 late) blends from
// a flat early look to the full PBR model, and uReal adds extra texture octaves
// and relief.
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
uniform sampler2D uTexSand;
layout(std430, binding = 0) buffer Bo { vec4 O[]; };   // uwplace.comp's buffer, in vec4s: the boulders by the sand
layout(binding = 2) uniform sampler2D uTexRock;
layout(location = 2) uniform bool uSandOnly;
layout(location = 3) uniform vec2 uStory;   // x: the act's story seconds (main.c's uw_u), for the sand

const float PI = 3.14159265;
layout(binding = 1) uniform sampler3D uNoise;   // 32^3 random bytes, wrapping (main.c's noise_make)
// Value noise, 0..1. The hardware blends the lattice; each cell's fraction
// goes through the cubic first.
float vn3(vec3 p){ vec3 i = floor(p), f = p - i; return texture(uNoise, (i + f * f * (3.0 - 2.0 * f) + 0.5) / 32.0).r; }
float vn(vec2 p){ return vn3(vec3(p, 0.0)); }    // in 2D: the volume's first layer
float D_GGX(float nh,float a){ a*=a; float d=nh*nh*(a-1.0)+1.0; return a/(PI*d*d+1e-7); }
float Gk(float x,float k){ return x/(x*(1.0-k)+k); }
float G_Smith(float nv,float nl,float r){ float k=(r+1.0); k=k*k*0.125; return Gk(nv,k)*Gk(nl,k); }
vec3 Fs(float c,vec3 F0){ return F0+(1.0-F0)*pow(1.0-c,5.0); }
vec3 FsR(float c,vec3 F0,float r){ return F0+(max(vec3(1.0-r),F0)-F0)*pow(1.0-c,5.0); }
// The light from below is the bright sand's, bounced back up. It lifts whatever
// faces down.
vec3 uwEnv(vec3 d,float clar){ float up=clamp(d.y*0.5+0.5,0.0,1.0);
    return mix(vec3(0.04,0.11,0.14), vec3(0.10,0.30,0.38)*(0.5+0.9*clar), up*up); }
vec3 pbrDirect(vec3 N,vec3 V,vec3 L,vec3 alb,float rough,vec3 F0,vec3 rad){
    vec3 H=normalize(V+L); float nl=max(dot(N,L),0.0),nv=max(dot(N,V),1e-3),nh=max(dot(N,H),0.0);
    float D=D_GGX(nh,rough), G=G_Smith(nv,nl,rough); vec3 F=Fs(max(dot(H,V),0.0),F0);
    vec3 spec=(D*G)*F/(4.0*nv*nl+1e-4); vec3 kd=(1.0-F);
    return (kd*alb/PI + spec)*rad*nl; }
vec3 pbrIBL(vec3 N,vec3 V,vec3 alb,float rough,vec3 F0,float clar,float q){
    vec3 R=reflect(-V,N); float nv=max(dot(N,V),1e-3); vec3 F=FsR(nv,F0,rough);
    vec3 irr=uwEnv(N,clar); vec3 pref=mix(uwEnv(R,clar),irr,rough);
    return ((1.0-F)*alb*irr + pref*F)*(0.35+0.65*q); }
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
// The water between the eye and this point. The colour gets the sunlight left
// at the point's depth and is dimmed along the way, and the water adds the
// light it scatters in. Both are integrated in closed form. The scattered light
// is brighter where the ray climbs toward the light than where it sinks, and
// looking toward the sun than away from it.
vec3 waterFog(vec3 col){
    vec3 c = wx(), d = vWorld - uCam;
    float s = max(length(d), 1e-3);
    vec3 e = c - 0.5 * c * d.y / s;
    return col * exp(c * (0.5 * vWorld.y + 2.25 - s)) + uFogColor * wp(d / s) * c / e * (1.0 - exp(-e * s));
}
// Caustics on a lit surface, with a mean of 1. Each colour channel of the web
// is a little wider or narrower than the next (red is widest), because the
// ripples split the light like a prism. Where a pixel spans several lines the
// web is smoothed toward its mean, and it's softer in murky water.
vec3 cst(vec3 p){
    float f = clamp(1.25 - 2.75 * length(fwidth(p.xz)), 0.0, 1.0);
    if (f <= 0.0) return vec3(1.0);                 // (far off it would only shimmer)
    float m = cw((p.xz + uLightDir.xz / uLightDir.y * (S - p.y)) * 0.45, 3), q = m * m;
    return mix(vec3(1.0), min(vec3(q * 37.1, q * sqrt(m) * 79.7, q * m * 166.0), vec3(8.0)), f * (0.35 + 0.55 * uClarity));
}
vec3 detailN(vec3 N,vec3 P,float hgt,float amt){
    vec3 dpx=dFdx(P),dpy=dFdy(P); float dhx=dFdx(hgt),dhy=dFdy(hgt);
    vec3 r1=cross(dpy,N),r2=cross(N,dpx); float det=dot(dpx,r1);
    vec3 g=(dhx*r1+dhy*r2)/(abs(det)+1e-6); return normalize(N-amt*g); }
// The sun's shadow map, compared in hardware by the texture's compare mode.
// Each tap is a 2 x 2 bilinear PCF, so the 3 x 3 taps give a smooth penumbra
// rather than steps of ninths.
layout(binding = 4) uniform sampler2DShadow uShadow;
float shadowVis(vec3 wp){
    if (uQ <= 0.0) return 1.0;
    float fade = smoothstep(35.0,40.0,length(wp-uCam));
    if (fade >= 1.0) return 1.0;
    vec4 lc = uLightVP * vec4(wp, 1.0); vec3 p = lc.xyz/lc.w * 0.5 + 0.5;
    if (p.z<0.0||p.z>1.0||p.x<0.0||p.x>1.0||p.y<0.0||p.y>1.0) return 1.0;
    float slope = 1.0-abs(dot(normalize(vNormal),normalize(uLightDir)));
    vec2 tx = 1.0/vec2(textureSize(uShadow,0));
    float v=0.0; p.z -= (0.025+0.12*slope)/149.0;
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x)
        v += texture(uShadow, vec3(p.xy+vec2(x,y)*tx, p.z));
    return mix(v/9.0,1.0,fade); }
// A texture wrapped round a boulder by the two planes it faces most (biplanar
// mapping, after iq). The least-facing plane gets weight 0 and its fetch is
// skipped, so it's two fetches where triplanar took three.
vec3 triplanar(sampler2D s,vec3 wp,vec3 N,float sc,vec3 dx,vec3 dy){
    vec3 n = abs(N), w = n - min(n.x, min(n.y, n.z)), c = vec3(0.0);
    w *= w;
    if (w.x > 0.0) c += textureGrad(s,wp.zy*sc,dx.zy*sc,dy.zy*sc).rgb*w.x;
    if (w.y > 0.0) c += textureGrad(s,wp.xz*sc,dx.xz*sc,dy.xz*sc).rgb*w.y;
    if (w.z > 0.0) c += textureGrad(s,wp.xy*sc,dx.xy*sc,dy.xy*sc).rgb*w.z;
    return c / (w.x + w.y + w.z + 1e-5); }
// A tile at u with its repeats hidden (iq's "texture repetition", way 3). k is
// a slow noise x 8, and picks one of eight randomly shifted copies of the tile.
// Where two copies meet, the relief (red) of one rises through the other's.
vec4 tnt(sampler2D t, vec2 u, vec2 x, vec2 y, float k){
    vec4 a = textureGrad(t, u + sin(vec2(3.0, 7.0) * floor(k)), x, y), b = textureGrad(t, u + sin(vec2(3.0, 7.0) * floor(k + 1.0)), x, y);
    return mix(a, b, smoothstep(0.3, 0.7, fract(k) + 0.3 * (b.r - a.r)));
}

// The reef bed at the centre of square c (uwplace.comp's pass 19), grown in by u.
float bd(ivec2 c, float u){ vec4 b = O[275418 + c.y * 68 + c.x]; return min(b.x, 1.0) * smoothstep(b.y, b.y + 4.0, u); }

void main(){
    vec3 N = normalize(vNormal), V = normalize(uCam - vWorld);
    vec3 dx = dFdx(vWorld), dy = dFdy(vWorld);
    // Alpha of the periodic sand/rock bake stores height, not roughness.
    // Evaluate height and its derivatives outside the per-instance branches.
    float hgt = (uSandOnly ? texture(uTexSand,vWorld.xz*0.25).a
                          : texture(uTexRock,vWorld.xz*0.25).a) * 1.5;
    int mat = int(vColor.a + 0.5);
    vec3 base; float rough;
    if (uSandOnly)     { base = tnt(uTexSand, vWorld.xz*uTexScale, dx.xz*uTexScale, dy.xz*uTexScale, vn(vWorld.xz*0.06)*8.0).rgb; rough = 0.92; }
    else if (mat == 0) { base = triplanar(uTexSand, vWorld, N, uTexScale, dx, dy); rough = 0.92; }
    else if (mat == 1) { base = triplanar(uTexRock, vWorld, N, uTexScale, dx, dy); rough = 0.62; }
    else { float n = vn(vWorld.xz*0.6) + 0.5*vn(vWorld.yz*1.3);
           base = mix(vec3(0.16,0.15,0.13), vec3(0.42,0.38,0.32), n); rough = 0.74; }
    if (mat <= 1 && uReal > 0.0){
        vec3 det  = uSandOnly ? texture(uTexSand,vWorld.xz*uTexScale*4.0).rgb
                  : (mat == 0) ? triplanar(uTexSand, vWorld, N, uTexScale*4.0, dx, dy)
                               : triplanar(uTexRock, vWorld, N, uTexScale*4.0, dx, dy);
        vec3 det2 = uSandOnly ? texture(uTexSand,vWorld.xz*uTexScale*11.0).rgb
                  : (mat == 0) ? triplanar(uTexSand, vWorld, N, uTexScale*11.0, dx, dy)
                               : triplanar(uTexRock, vWorld, N, uTexScale*11.0, dx, dy);
        base = mix(base, base*0.5 + det*0.5, uReal*0.72);
        base = mix(base, base*0.6 + det2*0.4, uReal*uReal*0.45);
        rough -= 0.18*uReal;
    }
    // sand settles on a boulder's upper faces, deepest in its hollows
    if (mat > 0) base = mix(base, vec3(0.5, 0.45, 0.35), smoothstep(0.6, 0.95, N.y - hgt * 0.3));
    base *= vColor.rgb;
    // Where the coral patches, the landmark reefs and the flight's last coral
    // stand, the sand turns to reef bed, so the heads sit in one reef rather
    // than on bare sand. For the centre of each 3.5 m square, uwplace.comp has
    // stored how much bed their heads lay down there, and from when (out of
    // sight, the way the props grow in). It's blended between the four nearest
    // squares, each capped at 1, and two octaves of noise fray its edge into
    // the sand so the squares never show. The bed is rubble and crusts of
    // growth in mottled ochre, olive and dusty rose, darker in its crevices
    // and in low relief.
    float u = uStory.x;
    if (uSandOnly) {
        vec2 g = clamp(vWorld.xz + 118.2, 0.0, 236.) / 3.5294;
        ivec2 a = ivec2(g);
        g -= vec2(a);
        float r = vn(vWorld.xz * 3.1), b = mix(mix(bd(a, u), bd(a + ivec2(1, 0), u), g.x), mix(bd(a + ivec2(0, 1), u), bd(a + 1, u), g.x), g.y);
        b = smoothstep(0.25, 0.55, b + vn(vWorld.xz * 0.5) * 0.7 + vn(vWorld.xz * 1.9) * 0.35 - 0.52);
        base = mix(base, mix(mix(vec3(0.5, 0.32, 0.12), vec3(0.3, 0.32, 0.1), vn(vWorld.xz * 0.9 + 7.0)), vec3(0.55, 0.2, 0.3),
                             smoothstep(0.55, 0.75, vn(vWorld.xz * 1.7))) * (0.2 + vn(vWorld.xz * 9.0) * r * 1.6), b);
        hgt = mix(hgt, r * 2.0, b);
    }

    /* detail normals: micro-relief from a procedural height field, growing with uQ */
    N = detailN(N, vWorld, hgt, (0.10 + 0.55*uQ));

    vec3  L = normalize(uLightDir);
    // The sand's contact shade round each boulder: the light the rock hides
    // from the sand at its foot. uwplace.comp lists the boulders near each
    // 3.5 m square of the seabed, starting at O[256922] in its buffer, 16
    // floats a square. Once grown in (by the smoothstep uw_prepare grows it
    // with), and until a landmark takes its place, each listed boulder darkens
    // the sand by how near the point lies in its own frame. That frame undoes
    // the boulder's rotation (Rodrigues) and divides out its size and the
    // squash rock.vert gives it (seeded from its position). The distance there
    // is 1 on the mean surface, so the shade follows the rock's outline. It
    // takes up to half the light at 0.6 (always inside the rock) and fades
    // smoothly to nothing by 1.8. It stays a little nearer, by up to 15%, on
    // the side away from the sun, where the cast shadow already darkens the
    // sand.
    // It darkens all of the sand's light, as the post's GTAO does in this act.
    // Taken from the ambient alone, it all but blackened the sand where little
    // else lit it (the web's dark cells and the cast shadow beside the rock).
    // The web's bright lines stayed, so the shade broke up into dark,
    // hard-edged blotches.
    float ao = 1.0;
    ivec2 c = ivec2(clamp(vWorld.xz + 120.0, 0.0, 239.9) / (240.0 / 68.0));
    for (int k = 0; k < 16 && uSandOnly; ++k) {
        int j = int(O[256922 + 4 * (c.y * 68 + c.x) + k / 4][k % 4]);
        if (j == 0) break;
        vec4 a = O[j], t = O[j + 1], e = O[j + 3];     // where + size, turn (axis, angle), (kind, birth, death, reach)
        if (u < e.y) continue;                         // (not born yet: most of them, early on)
        vec3 d = vWorld - a.xyz, w = d * cos(t.w) - cross(t.xyz, d) * sin(t.w) + t.xyz * dot(t.xyz, d) * (1.0 - cos(t.w));
        float h = length(w / (a.w * (0.7 + 0.4 * fract(a.xyz * 0.37 * vec3(7.1, 3.3, 5.7))))) * (1.0 - 0.3 * dot(d.xz, L.xz) / length(d));
        ao *= 1.0 - (e.z > 0.0 && u >= e.z ? 0.0 : smoothstep(e.y, e.y + 2.5, u)) * 0.5 * (1.0 - smoothstep(0.6, 1.8, h));
    }
    vec3 flatc = vec3(0.0);
    if (uQ < 1.0) flatc = base * (mix(vec3(0.05,0.09,0.12), vec3(0.12,0.26,0.30), 0.5+0.5*N.y)
                 + max(dot(N,L),0.0)*vec3(0.4,0.55,0.6)*(0.4+0.6*uClarity));
    if (uQ <= 0.0) {
        o = vec4(waterFog(flatc),1.0);
        return;
    }
    vec3  F0 = vec3(0.03);
    vec3  rad = vec3(0.60,0.85,0.95)*(0.5 + 1.2*uClarity)*cst(vWorld);
    rad *= mix(1.0, shadowVis(vWorld), uQ);          // sun shadowed, fading in with uQ
    // The jelly (centre uGlow, radius uGlowAmt) casts no shadow, because it's
    // a ball lens. The sunlight through it converges about 4.2 radii behind
    // its centre (n 1.136: f = n R / 2 (n - 1)), and a soft-bodied ball with a
    // wobbling skin focuses it loosely. So the sand under it gets a soft spot
    // of light, up to 2.65x the sun's (Gaussian, 1.2 radii across at 1/e^2).
    // Round the spot, out to the jelly's own radius, is a dim penumbra: the
    // light the spot gathered. At 1.5x the spot didn't show over the web and
    // the sky's light. Both fade in below the jelly, fade out again by the
    // time it's 7 radii above what it lights, and fade out beyond 20-45 m. A
    // hard-edged 3x spot inside a ring at a quarter looked like a white donut
    // painted on the sand.
    vec3 w = (vWorld - uGlow) / uGlowAmt;
    float z = -dot(w, L), r = length(w + L * z);
    rad *= 1.0 + uQ * smoothstep(0.0, 1.5, z) * (1.0 - smoothstep(3.0, 7.0, z)) * (1.0 - smoothstep(20.0, 45.0, length(uCam - vWorld)))
                   * (2.0 * exp(-6.0 * r * r) - 0.35 + 0.35 * smoothstep(0.5, 1.1, r));

    vec3  lit = pbrDirect(N, V, L, base, max(rough,0.05), F0, rad)
              + pbrIBL(N, V, base, max(rough,0.05), F0, uClarity, uQ);
    /* a flat, murky early look that the quality ramp dissolves into the PBR one */
    vec3  col = mix(flatc, lit, uQ) * ao;

    col = waterFog(col);
    o = vec4(col, 1.0);
}
