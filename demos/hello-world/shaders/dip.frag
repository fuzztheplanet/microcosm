#version 450 core
// The camera going under, as the waterline sweeps up the lens. Above the line
// is the sea seen from just above the swell (copied to unit 9 before the sea
// below was drawn), lifted a little by the meniscus near the line. On the line
// is a bright film of light with the meniscus's shadowed lip under it. The sea
// we're coming into, below the line, is already in the picture, and this pass
// blends over it.
in vec2 uv;
out vec4 o;
layout(binding = 9) uniform sampler2D uAbove;
layout(location = 0) uniform vec4 uL;           // the waterline's height on the lens; time
void main(){
    float d = uv.y - uL.x - 0.035 * sin(uv.x * 7.0 + uL.y * 3.0) * sin(uv.x * 3.0 - uL.y),
          a = smoothstep(-0.003, 0.003, d), f = exp(-abs(d) * 300.0);
    vec3 c = mix(vec3(0.0, 0.04, 0.07), texture(uAbove, vec2(uv.x, uv.y + 0.03 * exp(-abs(d) * 40.0))).rgb, a);
    o = vec4(mix(c, vec3(1.3, 1.45, 1.5), f), max(max(a, f), 0.5 * exp(-abs(d) * 70.0)));
}
