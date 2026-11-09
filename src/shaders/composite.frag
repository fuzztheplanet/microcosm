#version 450 core
// Composite: motion blur, depth of field, ambient occlusion and contact
// shadows, light shafts, exposure, a filmic tonemap, a split-tone grade,
// saturation, bloom, chroma, vignette, and fades to black and to white. The
// result is the LDR picture that the TAA then resolves, and the grain goes on
// after the TAA. aoTex/dofTex/volTex are always bound, and aoAmt, sunV.w and
// lens decide whether they're read.
in vec2 uv;
flat in mat4 rp;                        // clip space -> the last frame's (post.vert)
out vec4 o;
uniform sampler2D scene;
layout(binding = 1) uniform sampler2D bloom;
layout(binding = 2) uniform sampler2D aoTex;
layout(binding = 4) uniform sampler2D uDepth;
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
layout(binding = 5) uniform sampler2D dofTex;
layout(binding = 6) uniform sampler2D volTex;

// ACES's filmic curve (Narkowicz's fit), applied to colours that are first
// pulled a little toward grey and afterwards pushed back out by the inverse.
// That's the AgX trick, at half AgX's strength. Bright colours roll off to
// white and keep their hue, where the plain per-channel curve skewed the sky
// cyan and turned fire yellow. A bright sky or the jelly also keeps its colour
// a little further up before it whitens. Two alternatives were tried on all
// three scenes: full AgX flattened every scene to grey, and Khronos PBR
// Neutral (matched at mid grey) made Scene 1's pastels look like candy.
vec3 filmic(vec3 x){
    x = mat3(.921, .021, .021, .039, .939, .039, .04, .04, .94) * x;
    x = clamp((x * (2.51 * x + .03)) / (x * (2.43 * x + .59) + .14), 0., 1.);
    return mat3(1.088, -.023, -.023, -.043, 1.068, -.043, -.044, -.044, 1.067) * x;
}
float z(vec2 p){
    float d = texture(uDepth, p).r * 2.0 - 1.0;
    return 2.0 * zn * zf / (zf + zn - d * (zf - zn));
}
// Depth-aware bilinear upsample of the half-resolution AO / contact-shadow buffer.
vec2 ao(){
    vec2 sz = vec2(textureSize(aoTex, 0)), q = uv * sz - 0.5, f = fract(q), sum = vec2(0.0);
    float d = z(uv), ws = 0.0;
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++) {
            vec2 a = vec2(x, y), p = (floor(q) + a + 0.5) / sz;
            vec2 b = mix(1.0 - f, f, a);
            float w = b.x * b.y * exp(-abs(z(p) - d) * 8.0);
            sum += texture(aoTex, p).rg * w;
            ws += w;
        }
    return ws > 1e-5 ? sum / ws : vec2(1.0);
}
void main(){
    vec2 d = uv - 0.5;
    float ca = chroma * 0.003;
    vec4 c = texture(scene, uv);
    // Chromatic aberration. Red and blue are each spread over a range of
    // shifts, from half to all of the full shift, the way a lens spreads each
    // band of wavelengths. Shifting them whole put hard 1-pixel red and cyan
    // lines either side of the fine bark grooves, and the TAA's sharpening
    // then deepened them.
    vec3 col = vec3(texture(scene, uv + d * ca).r + texture(scene, uv + d * ca * 0.5).r, c.g * 2.0,
                    texture(scene, uv - d * ca).b + texture(scene, uv - d * ca * 0.5).b) * 0.5;
    // Motion blur. Find where this pixel was last frame (from its depth and
    // the camera's move) and smear over a third of that, like a 120 degree
    // shutter. The smear stops at 0.25% of the screen each way, so the seabed
    // rushing by stays readable, unless the caller asks for more with mblur
    // (a share of the screen). The reef flight needs that: capped at 0.25%,
    // its close kelp and coral, moving 30-45 pixels a frame, kept sharp edges
    // and stepped across the frame. There's one tap each way per pixel of
    // smear up to 16, then 16 spread over it. With no camera to come from
    // (the first frame, a cut) v is NaN, so there's no blur then. The loop is
    // bounded whatever arrives, because a NaN count looped forever on some
    // compilers and hung the GPU.
    vec4 q = rp * vec4(uv * 2.0 - 1.0, texture(uDepth, uv).r * 2.0 - 1.0, 1.0);
    vec2 v = (uv - q.xy / q.w * 0.5 - 0.5) * 0.17;
    v *= min(1.0, max(mblur, 0.0025) / max(length(v), 1e-6));
    if (any(isnan(v))) v = vec2(0.0);
    float n = min(floor(length(v / rcp)), 16.0);
    for (float i = 1.0; i <= 16.0; i++) {
        if (i > n) break;
        col += texture(scene, uv + v * i / n).rgb + texture(scene, uv - v * i / n).rgb;
    }
    col /= 1.0 + 2.0 * n;
    // Out of focus: blend in the quarter-resolution bokeh by this pixel's own
    // circle of confusion or by the blurred foreground's share of the bokeh
    // here (.a). That way what's out of focus in front spreads over a sharp
    // background. By its own circle alone, a blurred leaf kept a hard,
    // stepped outline on the ground. The bokeh is only read when the depth of
    // field ran, since otherwise its target holds whatever memory it was
    // given. After a resize that's half floats that aren't numbers, and NaN
    // times a zero weight is still NaN (black blotches over Scene 1).
    vec4 f = texture(dofTex, uv);
    float b = lens.y > 0.0 ? max(smoothstep(1.0, 3.0, abs(lens.y * (1.0 - lens.x / z(uv)))), min(f.a * 5.0, 1.0)) : 0.0;
    if (b > 0.0) col = mix(col, f.rgb, b);
    // The occlusion darkens the ambient share of the light (the scene's
    // alpha), and the contact shadows darken the rest, the sun's share. Both
    // are pixel-sharp, so the depth-of-field blur takes them away along with
    // the detail they belong to. Laid over the bokeh, they were sharp specks
    // on a soft background.
    if (aoAmt > 0.0 || sunV.w > 0.0) {
        vec2 a = mix(ao(), vec2(1.0), b);
        float s = clamp(c.a, 0.0, 1.0);
        col *= s * mix(1.0, a.x, aoAmt) + (1.0 - s) * a.y;
    }
    if (lens.z > 0.0) col += texture(volTex, uv).rgb * lens.z;   // shafts through the haze (smooth: bilinear will do)
    if (bloomAmt > 0.0) col += texture(bloom, uv).rgb * bloomAmt * 0.35;
    col = filmic(col * exposure);
    float l = dot(col, vec3(0.2126, 0.7152, 0.0722));
    col = mix(vec3(l), col, saturation) * (1.0 + mix(vec3(-0.03, 0.0, 0.04), vec3(0.03, 0.01, -0.03), l));   // cool shadows, warm lights
    float vig = (1.0 - smoothstep(0.3, 1.1, length(d) * 1.4));
    col *= mix(1.0, vig, vignette);
    col = pow(clamp(col, 0.0, 1.0), vec3(0.4545));                    // linear -> sRGB
    o = vec4(mix(col * fade, vec3(1.0), white), 1.0);
}
