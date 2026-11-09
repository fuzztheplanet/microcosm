#version 450 core
// Fish, lit with Cook-Torrance GGX and an analytic underwater IBL. The body is
// countershaded, with faint banding, detail-normal micro-scales and an
// iridescent thin-film sheen, all strengthening with the uQ evolution ramp.
// The fog uses per-wavelength extinction.
// The predator (uK & 8) goes through the same shader but is painted as a dark
// slate hunter, pale-bellied, scarred and fine-scaled, with no sheen. It has
// white fangs in a dark, wet gullet and pale, glowing slit-pupilled eyes that
// stand out through the murk.
in vec3  vWorld;
in vec3  vNormal;
in vec4  vColor;
in float vBodyT;
in vec2  vUV;
in vec3  vObj;
in float vTooth;
in float vIn;
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
layout(binding = 3) uniform sampler2D uSkin;      // baked scale skin, sampled by the mesh's UVs (Path B)
layout(location = 1) uniform int uK;   // the draw's UW_ flags: 8 the predator

const float PI = 3.14159265;
layout(binding = 1) uniform sampler3D uNoise;   // 32^3 random bytes, wrapping (main.c's noise_make)
// Value noise, 0..1. The hardware blends the lattice; each cell's fraction
// goes through the cubic first.
float vn3(vec3 p){ vec3 i = floor(p), f = p - i; return texture(uNoise, (i + f * f * (3.0 - 2.0 * f) + 0.5) / 32.0).r; }
float fbm2(vec2 p){ float s=0.0,a=0.5; for(int i=0;i<4;i++){ s+=a*vn3(vec3(p,0.0)); p=p*2.03+1.7; a*=0.5; } return s; }   // 2D, from the noise volume's first layer
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

// A fin's rays, fanning out from its root: the tail stock for the tail fin, near
// the middle of the body for the others. 1 on a ray, 0 on the membrane between.
float rays(){
    vec3 d = vObj - vec3(0.0, 0.0, vObj.z < -0.75 ? -0.85 : 0.05);
    return smoothstep(0.5, 1.0, abs(sin(atan(length(d.xy), d.z) * 30.0)));
}

// Sunlight through a fin's thin membrane, seen from the side away from the
// sun (N faces the eye). It's spread out, and brightest where the eye looks up
// through the fin toward the sun, since most of the light carries on forward
// (Barre-Brisebois).
float fin(vec3 N, vec3 V, vec3 L){ return max(-dot(N, L), 0.0) * (0.4 + 2.5 * pow(max(dot(-V, L), 0.0), 4.0)); }

// The baked scales at u (8 to a tile each way), eased to their mean (the
// texture's last mip) as a scale shrinks from 6 to 3 pixels across (k 8). The
// predator's (k 24) fade from 18 to 9 pixels: at those sizes its fine, even
// net of scales looked like chain mail and crawled as it swept past the lens.
// Any finer and the rows beat against the pixels as moire, faint in the albedo
// but strong in the relief detailN takes from them, since the slopes grow with
// the frequency. Mip-mapping alone leaves the moire in. Up close the scales
// stay sharp. sF is how far they're faded.
float sF;
vec4 scales(vec2 u, float k){
    vec2 w = fwidth(u) * k;
    sF = smoothstep(0.17, 0.33, max(w.x, w.y));
    return mix(texture(uSkin, u), textureLod(uSkin, u, 9.0), sF);
}

vec4 hunter(vec3 N, vec3 V, vec3 L){
    float back = smoothstep(-0.45, 0.25, N.y);
    vec3 alb = mix(vec3(0.34,0.35,0.33), vec3(0.040,0.052,0.058), back);
    float sc = fbm2(vec2(vObj.z * 9.0, vObj.y * 26.0 + abs(vObj.x) * 26.0));
    alb *= 1.0 - 0.45 * smoothstep(0.62, 0.70, sc) * (1.0 - smoothstep(0.70, 0.74, sc));   // old scars
    alb *= 0.85 + 0.30 * fbm2(vObj.zy * vec2(14.0, 30.0) + 3.0);                   // blotches
    vec4 skin = scales(vUV * 3.0, 24.0);                                            // fine scales
    alb *= mix(1.0, 0.8 + 0.4 * dot(skin.rgb, vec3(0.333)), uReal);
    float rough = 0.55;
    if (vTooth > 0.5) { alb = vec3(0.86,0.83,0.74); rough = 0.3; }
    else if (vIn > 0.5) { alb = vec3(0.05,0.008,0.012); rough = 0.25; }          // the gullet: near-black, wet
    if (vFin > 0.5) { alb = mix(vec3(0.12,0.14,0.15), vec3(0.05), rays()); N = faceforward(N, -V, N); }   // dark membranes over spines, lit from either side
    N = detailN(N, vWorld, dot(skin.rgb, vec3(0.333)) * 1.2, (0.02 + 0.04 * uQ) * (1.0 - vTooth - vFin));
    vec3  F0 = vec3(0.04);
    vec3  rad = vec3(0.60,0.85,0.95) * (0.5 + 1.1 * uClarity) * mix(1.0, shadowVis(vWorld), uQ) * cst(vWorld);
    vec3  lit = pbrDirect(N, V, L, alb, rough, F0, rad) + pbrIBL(N, V, alb, rough, F0, uClarity, uQ);
    // The wet rim. Where the scales are faded we add the rim light their
    // relief would catch on average (measured where they resolve), so the skin
    // keeps its brightness as the scales come and go.
    lit += (pow(1.0 - max(dot(N,V),0.0), 4.0) + 0.1 * sF * (1.0 - vTooth)) * vec3(0.10,0.16,0.18) * (0.4 + 0.6 * uClarity)
         + vFin * alb * rad * fin(N, V, L);                                           // light through the fins
    vec3 flatc = alb * (mix(vec3(0.05,0.09,0.12), vec3(0.12,0.26,0.30), 0.5+0.5*N.y)
                 + max(dot(N,L),0.0)*vec3(0.5,0.6,0.65)*(0.4+0.6*uClarity));
    // The jaws and the throat shut almost all the water's light out of the
    // gullet. So inside the mouth the sheen, the rim and the light from the
    // surrounding water go too, leaving a black maw when the jaws open wide.
    // Lit like the hide, it looked like a dim blue-grey box behind the fangs.
    vec3 col = mix(flatc, lit, uQ) * (1.0 - 0.85 * vIn);
    // the eye: a pale, faintly glowing iris with a black vertical slit
    float e = length(vec2(vObj.z - 0.60, (vObj.y - 0.11) * 1.15));
    if (e < 0.062 && abs(vObj.x) > 0.06 && vTooth + vIn < 0.5) {
        float slit = (1.0 - smoothstep(0.010, 0.016, abs(vObj.z - 0.60))) * step(abs(vObj.y - 0.11), 0.045);
        col = mix(vec3(0.95,0.82,0.30) * (0.7 + 0.8 * (1.0 - smoothstep(0.02, 0.062, e))), vec3(0.0), slit);
    }
    return vec4(col, 1.0);
}

void main(){
    vec3 N = normalize(vNormal), V = normalize(uCam - vWorld), L = normalize(uLightDir);
    if ((uK & 8) != 0) {
        o = vec4(waterFog(hunter(N, V, L).rgb), 1.0);
        return;
    }
    float belly = smoothstep(-0.3, 0.3, N.y);
    vec3 alb = mix(mix(vColor.rgb, vec3(0.95), 0.6), vColor.rgb, belly);
    float band = step(0.5, 0.5 + 0.5 * sin(vBodyT * 16.0));
    alb = mix(alb, alb * 0.6, band * (0.25 + 0.2 * uReal));

    /* UV-mapped scale skin (Path B), which moves with the body. It takes over
     * the albedo, drives the scale relief and sets the roughness, all fading in
     * with the evolution ramps, so early fish stay procedural and late fish are
     * textured. */
    vec4 skin = scales(vUV, 8.0);
    alb = mix(alb, alb * 0.35 + skin.rgb * 0.95, uReal);
    float hgt = dot(skin.rgb, vec3(0.333)) * 1.6, r = rays();
    if (vFin > 0.5) { alb = (vColor.rgb * 0.8 + 0.15) * (1.0 - 0.4 * r); hgt = 0.0; N = faceforward(N, -V, N); }   // a clear membrane over its rays, lit from either side
    if (uReal < 1.0) hgt = mix(fbm2(vWorld.xz * 6.0 + vWorld.y * 3.0), hgt, uReal);
    N = detailN(N, vWorld, hgt, 0.05 + 0.32 * uQ);

    float rough = mix(mix(0.45, 0.26, uQ), skin.a, 0.7 * uReal);
    vec3  F0 = vec3(0.04);
    vec3  rad = vec3(0.60,0.85,0.95) * (0.5 + 1.1 * uClarity) * cst(vWorld);
    rad *= mix(1.0, shadowVis(vWorld), uQ);          // sun shadowed, fading in with uQ
    vec3  lit = pbrDirect(N, V, L, alb, rough, F0, rad)
              + pbrIBL(N, V, alb, rough, F0, uClarity, uQ);
    float fres = pow(1.0 - max(dot(N,V),0.0), 3.0);        // iridescent thin-film sheen
    vec3  irid = 0.5 + 0.5 * cos(vec3(0.0,2.1,4.2) + fres*8.0 + vBodyT*3.0);
    lit += fres * irid * (0.15 + 0.5 * uQ) * (0.4 + 0.6 * uClarity) * (1.0 - vFin)
         + vFin * alb * rad * fin(N, V, L);                                           // light through the fins

    vec3 flatc = alb * (mix(vec3(0.05,0.09,0.12), vec3(0.12,0.26,0.30), 0.5+0.5*N.y)
                 + max(dot(N,L),0.0)*vec3(0.5,0.6,0.65)*(0.4+0.6*uClarity));
    vec3 col = mix(flatc, lit, uQ);
    col = waterFog(col);
    o = vec4(col, 1.0);
}
