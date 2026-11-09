#version 450 core
// The whale, lit with Cook-Torrance GGX plus analytic underwater IBL. Its skin
// is a blue whale's, painted from the rest point (vObj) so it moves with the
// animal. It's laid out by the angle round the body and by s, which runs from
// the root of the flukes to the snout. vFin marks the fins. The skin is
// countershaded blue-grey, a dark back fading to a pale throat, and mottled.
// The long throat grooves run back from the chin, the mouth line sweeps down
// to a small eye, and the blowhole sits on top. Fine skin detail, detail
// normals and a wet fresnel rim grow with the uQ/uReal evolution ramps, and
// the fog uses wavelength-selective extinction.
in vec3  vWorld;
in vec3  vNormal;
in vec4  vColor;
in float vBodyT;
in vec3  vObj;
in float vFin;
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
layout(binding = 3) uniform sampler2D uSkin;      // baked UV skin (Path B)

const float PI = 3.14159265;
layout(binding = 1) uniform sampler3D uNoise;   // 32^3 random bytes, wrapping (main.c's noise_make)
// Value noise, 0..1. The hardware blends the lattice; each cell's fraction
// goes through the cubic first.
float vn3(vec3 p){ vec3 i = floor(p), f = p - i; return texture(uNoise, (i + f * f * (3.0 - 2.0 * f) + 0.5) / 32.0).r; }
float fbm2(vec2 p){ float s=0.0,a=0.5; for(int i=0;i<4;i++){ s+=a*vn3(vec3(p,0.0)); p=p*2.03+1.7; a*=0.5; } return s; }   // 2D: the volume's first layer
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
    vec3 spec=(D*G)*F/(4.0*nv*nl+1e-4); return ((1.0-F)*alb/PI + spec)*rad*nl; }
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

void main(){
    vec3 N = normalize(vNormal), V = normalize(uCam - vWorld), L = normalize(uLightDir);
    float body = 1.0 - vFin;
    float s   = vObj.z * 0.5 + 0.5;                      // 0 where the flukes join .. 1 at the snout
    /* the angle round the body, about the section's centre and squashed as the
     * section is: -pi/2 at the belly to pi/2 at the back, on both flanks */
    float af  = atan((vObj.y + 0.035 * smoothstep(0.0, 0.7, vObj.z) * (1.0 - smoothstep(0.76, 1.0, vObj.z)))
                     / mix(1.0, 0.68, smoothstep(0.0, 0.75, vObj.z)), abs(vObj.x));
    float ventral = mix(1.0 - smoothstep(-0.6, 0.2, N.y), 1.0 - smoothstep(-0.55, 0.10, af), body);
    float crisp = body * (1.0 - smoothstep(0.015, 0.05, fwidth(af)));  // markings finer than a pixel fade out
    vec3 alb = mix(vec3(0.13,0.17,0.22), vec3(0.42,0.45,0.47), smoothstep(0.30, 0.80, ventral));

    /* mottling: lighter spots and darker blotches over the back and flanks */
    vec2 mc = mix(vObj.xz * 40.0, vec2(s * 70.0, af * 9.0), body);
    float m1 = fbm2(mc), m2 = fbm2(mc * 0.45 + 7.0);
    alb = mix(alb, alb * 1.55 + 0.03, smoothstep(0.58, 0.72, m1) * 0.7 * (1.0 - 0.6 * ventral));
    alb *= 1.0 - 0.25 * smoothstep(0.55, 0.70, m2);

    /* throat grooves: fine pleats along the belly from the chin back to the navel.
     * They fade out as they shrink toward 4 pixels apart, so they never shimmer.
     * Faded out any later, they crawled like a grid of dashes. */
    float gx = af * 14.0;
    float region = crisp * smoothstep(0.47, 0.56, s) * (1.0 - smoothstep(0.93, 0.985, s))
                 * (1.0 - smoothstep(-0.45, -0.15, af)) * (1.0 - smoothstep(0.1, 0.25, fwidth(gx)));
    float groove = (1.0 - smoothstep(0.0, 0.45, abs(fract(gx) - 0.5) * 2.0)) * region;
    alb *= 1.0 - 0.22 * groove;
    float hgt = -0.15 * groove;
    /* mouth line, from the snout tip down and back to the corner of the mouth */
    float aL = -0.10 - 0.30 * (1.0 - smoothstep(0.80, 0.995, s));
    float mouth = crisp * (1.0 - smoothstep(0.012, 0.035, abs(af - aL)))
                * smoothstep(0.795, 0.81, s) * (1.0 - smoothstep(0.985, 0.995, s));
    alb *= 1.0 - 0.75 * mouth;
    hgt -= 0.4 * mouth;
    /* a small dark eye just above the corner of the mouth, and the blowhole on top */
    float eye  = 1.0 - smoothstep(0.006, 0.010, length(vec2(2.0 * (s - 0.795), 0.16 * (af + 0.30))));
    float blow = (1.0 - smoothstep(0.003, 0.006, abs(0.16 * (1.5708 - af) - 0.012)))
               * (1.0 - smoothstep(0.008, 0.016, abs(2.0 * (s - 0.875))));
    alb = mix(alb, vec3(0.012), crisp * max(eye, 0.7 * blow));

    /* fine skin (Path B, the baked periodic skin): subtle relief and roughness */
    vec4 skin = texture(uSkin, mix(vObj.xz * 8.0, vec2(af * 1.91, s * 36.0), body));
    float fine = dot(skin.rgb, vec3(0.333));
    alb *= mix(1.0, 0.85 + 0.4 * fine, uReal);
    hgt += fine * 1.2;
    N = detailN(N, vWorld, hgt, 0.04 + 0.22 * uQ);

    float rough = mix(mix(0.50, 0.30, uQ), skin.a, 0.4 * uReal);
    vec3  F0 = vec3(0.035);
    // The caustic web at 0.4 strength, and none on the fins. At full strength
    // its cells lined up along the back of a 30 m whale seen from below into
    // evenly spaced dark bars, like a mackerel's.
    vec3  rad = vec3(0.55,0.80,0.90) * (0.5 + 1.1 * uClarity) * mix(vec3(1.0), cst(vWorld), 0.4 * body);
    rad *= mix(1.0, shadowVis(vWorld), uQ);          // sun shadowed, fading in with uQ
    vec3  lit = pbrDirect(N, V, L, alb, rough, F0, rad)
              + pbrIBL(N, V, alb, rough, F0, uClarity, uQ);
    float fres = pow(1.0 - max(dot(N,V),0.0), 4.0);
    lit += fres * vec3(0.25,0.45,0.55) * (0.3 + 0.6 * uQ) * (0.4 + 0.6 * uClarity);

    vec3 flatc = alb * (mix(vec3(0.05,0.09,0.12), vec3(0.12,0.24,0.28), 0.5+0.5*N.y)
                 + max(dot(N,L),0.0)*vec3(0.5,0.6,0.65)*(0.4+0.6*uClarity));
    // The fins keep the flat diffuse look. With the IBL's grazing sheen and
    // the rim, the root of the near flipper, seen end-on from below, showed
    // as a pale, stair-stepped teardrop on the flank.
    vec3 col = mix(flatc, lit, uQ * body);
    col = waterFog(col);
    o = vec4(col, 1.0);
}
