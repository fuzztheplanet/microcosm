#version 450 core
// Depth of field, step 2: scatter-as-gather over a golden-angle spiral, after
// Dennis Gustafsson's single-pass bokeh. A tap counts if its own circle of
// confusion reaches this texel. Taps behind a sharper centre can't spread over
// it, so in-focus edges stay clean and bright points open into round bokeh.
// .a, gathered the same way, holds how much of the result is blurred
// foreground (taps in front of the focus by over 2 pixels of blur). The
// composite blends by this as well as by the pixel's own circle, so a blurred
// stem in front spreads over the sharp ground behind it instead of standing
// on it with a hard edge.
// Where the buffer two mip levels down is bright (a point of HDR light
// nearby), a tap takes its colour from there instead, the mean of 4 x 4
// texels. The spiral's taps are about 3 texels apart however far it reaches,
// so a small bright point like the sun in a dew drop could fall between
// them. Its bokeh then came out as the spiral's own pattern of dots instead
// of a disc. This is only done for bright points, because reading the mips
// everywhere blurred the half-blurred middle distance and bled the colour of
// in-focus edges (the ladybug's red) into the bokeh behind them.
// Where a tap's own circle is large (ramping in from 9 to 18 frame pixels,
// which is 2.25 to 4.5 texels here), that mean is read through four bilinear
// taps half a texel off on the diagonals instead of one. With one tap, its
// tents over the 4 x 4-texel blocks drew a far drop's highlight as a lumpy
// square of overlapping blocks. The wider, smoother footprint draws an even
// disc with round contours. The centre tap is read the same way there.
// Otherwise a bright point at the centre stays a sharp dot in the middle of
// its own bokeh. Starting this at 12 pixels instead of 9 left that pinprick
// on a near drop whose circle was 9-12. Smaller circles keep the single tap
// and their plain centre for the beach's long lens: its circles stay under
// 8.5 pixels, and a wider mean smeared its bright surf into blotches.
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
// The mean two levels down, smoothed: four bilinear taps half a texel off on
// the diagonals.
vec3 bs(vec2 u){
    vec2 e = 0.5 / vec2(textureSize(tex, 2));
    return (textureLod(tex, u + e, 2.0).rgb + textureLod(tex, u - e, 2.0).rgb + textureLod(tex, u + vec2(e.x, -e.y), 2.0).rgb
          + textureLod(tex, u - vec2(e.x, -e.y), 2.0).rgb) * 0.25;
}
// One tap. A bright point takes its colour from the mean two levels down.
// m = 1 does this for small circles too, while the centre tap passes 0 and so
// only gets it under a large circle. The mean is only smoothed where that can
// count: the circle is large and the plain mean is already bright. Under a
// large circle a dimmer mean counts too, starting at 0.15 and fully by 0.45.
// A mote or a glint a few pixels across only averages to that much over
// 4 x 4 texels, so without this each ring tap that landed on the point itself
// still printed the spiral as a rosette of dots. Such taps are far from
// focus, so no in-focus colour bleeds through them.
vec4 tap(vec2 q, float m){
    vec4 s = textureLod(tex, q, 0.0);
    vec3 b = textureLod(tex, q, 2.0).rgb;
    float k = smoothstep(9.0, 18.0, abs(s.a));
    if (k > 0.0 && dot(b, vec3(0.2126, 0.7152, 0.0722)) > 0.3) b = mix(b, bs(q), k);
    s.rgb = mix(s.rgb, b, smoothstep(0.3 - 0.15 * k, 1.0 - 0.55 * k, dot(b, vec3(0.2126, 0.7152, 0.0722))) * max(m, k));
    return s;
}
void main(){
    vec2 px = 1.0 / vec2(textureSize(tex, 0));
    vec4 c = tap(uv, 0.0), acc = vec4(c.rgb, 0.0);
    float cs = abs(c.a) * 0.25, n = 1.0, r = 0.5;         // radii in quarter-res texels
    // Go out to the largest circle there is (lens.y, in the frame's pixels),
    // so the bokeh's size on screen stays the same at any internal resolution.
    for (float a = 0.0; r < lens.y * 0.25 + 0.5; a += 2.39996) {
        vec2 q = uv + vec2(cos(a), sin(a)) * r * px;
        vec4 s = tap(q, 1.0);
        float ss = abs(s.a) * 0.25;
        if (s.a > c.a) ss = min(ss, cs * 2.0);
        acc += mix(acc / n, vec4(s.rgb, step(s.a, -2.0)), smoothstep(r - 0.5, r + 0.5, ss));
        n += 1.0;
        r += 1.4 / r;
    }
    o = acc / n;
}
