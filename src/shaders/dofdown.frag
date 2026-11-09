#version 450 core
// Depth of field, step 1: the scene at quarter resolution, as a 4 x 4 box made
// from four bilinear taps. Each texel's signed circle of confusion goes in .a,
// in full-resolution pixels and negative in front of the focus.
in vec2 uv;
out vec4 o;
uniform sampler2D tex;
layout(binding = 1) uniform sampler2D uDepth;
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
    float d = texture(uDepth, uv).r * 2.0 - 1.0;
    d = 2.0 * zn * zf / (zf + zn - d * (zf - zn));
    vec2 e = 1.0 / vec2(textureSize(tex, 0));
    o = vec4((texture(tex, uv + e).rgb + texture(tex, uv - e).rgb + texture(tex, uv + vec2(e.x, -e.y)).rgb
            + texture(tex, uv - vec2(e.x, -e.y)).rgb) * 0.25, clamp(lens.y * (1.0 - lens.x / d), -lens.y, lens.y));
}
