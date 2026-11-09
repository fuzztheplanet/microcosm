#version 450 core
// Shared fullscreen vertex shader for all the post passes. One triangle covers
// the screen, with no vertex buffer, and uv spans 0..1 across the visible
// area. It also hands the TAA and the motion blur the reprojection from this
// frame's clip space to the last frame's. The inverse is taken here, three
// times a pass instead of once per pixel. A pass that doesn't read rp drops
// it, and the uniform block with it.
out vec2 uv;
flat out mat4 rp;
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
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    uv = p;
    rp = reproj * inverse(cvp);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
