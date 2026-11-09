#version 450 core
// Bright pass, down to half resolution. Four bilinear taps cover a 4 x 4 block
// of the scene, each weighted by 1 / (1 + its luma), as Karis does it. That
// way a lone sub-pixel sun glint, on for one frame and off the next, can't
// throw a halo that flashes with it. Then we keep the energy above the
// threshold, with a soft knee.
in vec2 uv;
out vec4 o;
uniform sampler2D tex;
// The post stack's parameters, std140 on binding 0. Once the scene is drawn,
// mc_renderer_end refills the frame's uniform buffer with them. Keep this block
// identical in every post shader; post_block in render.c mirrors it.
layout(std140) uniform Post {
    vec3  lens;  float zn;              // focus, blur at infinity, shafts mix; near plane
    vec2  uSpare, rcp, uRes;            // spare (0); scene texel; spare (0)
    float zf, exposure, bloomAmt, vignette, grain, chroma, saturation, fade, white, time,
          aoAmt, mblur, projx, projy, thresh,
          seed;                         // frames since the TAA history began (0: none)
    vec2  jit;                          // this frame's sub-pixel jitter (scene pixels)
    vec4  sunV;                         // toward the sun, view space; w: contact shadows on
    mat4  reproj, cvp;                  // last frame's view-projection; this one's
};
void main(){
    vec3 c = vec3(0.0);
    float s = 0.0;
    for (int i = 0; i < 4; i++) {
        vec3 x = texture(tex, uv + vec2(i & 1, i >> 1) * 2.0 * rcp - rcp).rgb;
        float w = 1.0 / (1.0 + dot(x, vec3(0.2126, 0.7152, 0.0722)));
        c += x * w; s += w;
    }
    c /= s;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float k = max(0.0, l - thresh);
    o = vec4(l < 6e4 ? c * (k / (k + 0.5)) : vec3(0.0), 1.0);  // keep a NaN or Inf (a stray pixel) from spreading through the chain
}
