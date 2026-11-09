#version 450 core
// Puts the TAA's history on the screen, with the film grain on top. The grain
// goes on after the TAA because the TAA would smooth it away. It hashes the
// output pixel (with a sine-free hash), or above 1080 lines a cell the size
// of a 1080-line screen's pixel, so it stays noise at any resolution instead
// of beating into a striped moire. It's high-passed (a pixel's hash minus the
// mean of its right and upper neighbours'), so it has no blotches and gives
// the even texture of a fine-grain film stock. Like film, it changes 24 times
// a second, not every frame. The 8-bit write is then dithered, whether the
// picture is fading or not. The grain fades with the picture, and without the
// dither a glow fading out to black broke up into rings, one per step.
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
float hash(vec2 p){
    vec3 q = fract(p.xyx * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}
void main(){
    // The grain is added in the display's own steps, like film grain:
    // strongest in the midtones, only a trace in the shadows. Added to linear
    // light, it boiled in the shadows, a dozen steps either way, frame after
    // frame.
    // The history is the output's size, or the frame's size when the frame is
    // supersampled and so larger. Then this is its downsampling filter: four
    // bilinear taps around the pixel's centre, offset by 0.6 of a, the amount
    // (in uv) by which an output pixel is wider than a history pixel. With the
    // TAA's own Gaussian (0.47 of a frame pixel) that makes a filter of
    // 0.43-0.46 output pixels from 150% to 200%, as wide as the TAA's at 100%.
    // So the finer samples give better edges and shading, not a sharper,
    // aliasing picture.
    vec2 a = fwidth(uv) - 1.0 / vec2(textureSize(tex, 0));
    vec3 c = texture(tex, uv).rgb;
    if (a.x > 1e-5) {
        a *= 0.6;
        c = (texture(tex, uv + a).rgb + texture(tex, uv - a).rgb + texture(tex, uv + vec2(a.x, -a.y)).rgb
           + texture(tex, uv - vec2(a.x, -a.y)).rgb) * 0.25;
    }
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    // The grain is hashed in cells of whole pixels, as many pixels a side as
    // the output has multiples of 1080 lines. A 4K screen's grain is in 2 x 2
    // cells, the size it has at 1080 (hashed per pixel, it had a quarter of
    // that area). A 1440-line screen keeps single pixels. Cells of 4/3 pixel
    // there made five rows and columns in every 21 two pixels wide, which
    // drew a fine plaid in flat areas. The line count is one over uv's step
    // down a pixel, rounded, since its last bits are off. At 1080 lines the
    // grain matches the old one to the bit (Mesa, UHD 620). That's why the
    // sums are in an odd order, the order the compiler had given the old
    // gl_FragCoord.xy + w. It's also why fract(y) is there: it's a 0.5 the
    // compiler can't fold into w, and folding it moved two rows.
    vec2 g = floor(gl_FragCoord.xy / max(floor(1.0 / 1080.0 / abs(dFdy(uv.y)) + 0.01), 1.0)),
         w = fract(floor(time * 24.0) * 0.618) * vec2(419.0, 173.0), p = vec2(g.x + (w.x + 0.5), (g.y + fract(gl_FragCoord.y)) + w.y);
    c += (hash(p) - 0.5 * (hash(p + vec2(1.0, 0.0)) + hash(p + vec2(0.0, 1.0))))
       * 0.82 * grain * fade * (1.0 - white) * (0.35 + 2.6 * l * (1.0 - l));
    // The dither: triangular, one step either way, from two more hashes of
    // the grain's seed. There's none at pure black or pure white, so those
    // stay pure.
    c += (hash(p + 0.37) + hash(p + 0.71) - 1.0) * min(min(c, 1.0 - c), 0.004);
    o = vec4(clamp(c, 0.0, 1.0), 1.0);
}
