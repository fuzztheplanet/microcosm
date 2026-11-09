#version 450 core
// TAA. Each frame the camera is shifted by a sub-pixel step, so over eight
// frames every pixel gets sampled at eight spots. This pass folds the new
// samples into the history, which is kept at the output size (so it also
// upsamples a reduced scene). A 3 x 3 Gaussian over the jittered frame
// rebuilds this pixel's centre, sharpened a touch, and the history converges
// on that, so the picture stays sharp. The history is fetched where this
// point was last frame (from its depth and the camera's move) with a sharp
// Catmull-Rom filter. It's then pulled back inside the colours around this
// pixel, to within 1.5 standard deviations of their mean, which stops
// ghosting. A still picture blends in only 7% of each new frame, so the
// jittered samples along its edges average out instead of shimmering.
// There are no motion vectors for things that move on their own, so where
// this point's depth falls outside last frame's depths around it, the
// history lets go fast. A bird flying by does that, but an edge under the
// jitter doesn't. The new frame also counts for more where the history had
// to be pulled in a long way (it was wrong, so something moved), and the
// faster the picture moves, so a rushing seabed stays sharp. The result is
// the new history, which present.frag puts on screen. A frame with no
// history (seed 0: after a jump, a pause, or the first frame) is just its
// own samples.
in vec2 uv;
flat in mat4 rp;                        // clip space -> the last frame's (post.vert)
out vec4 o;
uniform sampler2D tex;
layout(binding = 1) uniform sampler2D hist;
layout(binding = 2) uniform sampler2D uDepth;
layout(binding = 3) uniform sampler2D uLast;
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
vec3 catrom(vec2 u){
    vec2 sz = vec2(textureSize(hist, 0)), p = u * sz - 0.5, f = fract(p), t = floor(p) + 0.5;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f),
         w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f), w12 = w1 + w2;
    vec2 a = (t - 1.0) / sz, b = (t + w2 / w12) / sz, c = (t + 2.0) / sz;
    return (texture(hist, vec2(b.x, a.y)).rgb * w12.x * w0.y + texture(hist, vec2(a.x, b.y)).rgb * w0.x * w12.y
          + texture(hist, b).rgb * w12.x * w12.y + texture(hist, vec2(c.x, b.y)).rgb * w3.x * w12.y
          + texture(hist, vec2(b.x, c.y)).rgb * w12.x * w3.y) / (w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y);
}
void main(){
    vec2 s = vec2(textureSize(tex, 0)), p = uv * s + jit;     // this pixel's centre in the jittered frame
    vec3 c = vec3(0.0), m = c, m2 = c, lo = vec3(9.0), hi = c;
    float ws = 0.0, wm = 0.0;
    for (int i = 0; i < 9; i++) {
        vec2 e = floor(p) + vec2(i % 3 - 1, i / 3 - 1) + 0.5;
        vec3 x = texture(tex, e / s).rgb;
        float w = exp(-2.3 * dot(e - p, e - p));
        c += x * w; ws += w; wm = max(wm, w);
        m += x; m2 += x * x; lo = min(lo, x); hi = max(hi, x);
    }
    vec4 q = rp * vec4(uv * 2.0 - 1.0, texture(uDepth, p / s).r * 2.0 - 1.0, 1.0);
    vec2 h = q.xy / q.w * 0.5 + 0.5;
    c /= ws; m /= 9.0;
    c = clamp(c + dot(c - m, vec3(0.2126, 0.7152, 0.0722)) * 0.5, lo, hi);     // sharpen a touch, luma only (per channel it deepened every colour fringe)
    if (seed > 0.0 && h == clamp(h, 0.0, 1.0)) {
        float z0 = q.z / q.w, tz = 0.02 * (1.0 - z0), za = 1.0, zb = 0.0;
        for (int i = 0; i < 4; i++) {                  // last frame's depths around it: 4 x 4
            vec4 g = textureGatherOffset(uLast, h, ivec2(i & 1, i >> 1) * 2 - 1);
            za = min(za, min(min(g.x, g.y), min(g.z, g.w))); zb = max(zb, max(max(g.x, g.y), max(g.z, g.w)));
        }
        za = za * 2.0 - 1.0; zb = zb * 2.0 - 1.0;
        vec3 e = sqrt(max(m2 / 9.0 - m * m, 0.0)), y0 = catrom(h), y = clamp(y0, max(lo, m - 1.5 * e), min(hi, m + 1.5 * e));
        c = mix(y, c, z0 < za - tz || z0 > zb + tz ? 0.8 : max(0.07 * wm + 0.5 * min(length(y0 - y) / (length(e) + 0.01), 1.0), min(length((uv - h) * s) * 0.03, 0.4)));
    }
    o = vec4(c, 1.0);
}
