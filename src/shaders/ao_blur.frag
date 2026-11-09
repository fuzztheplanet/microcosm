#version 450 core
// Bilateral filter for the half-resolution buffers (AO, light shafts). The depth
// texture stays at scene resolution.
in vec2 uv;
out vec4 o;
uniform sampler2D tex;
layout(binding = 1) uniform sampler2D uDepth;
layout(location = 0) uniform vec4 dir;                       // the step (xy); the camera's near and far planes (zw)
float z(vec2 p){
    float d = texture(uDepth, p).r * 2.0 - 1.0;
    return 2.0 * dir.z * dir.w / (dir.w + dir.z - d * (dir.w - dir.z));
}
void main(){
    vec3 c = vec3(0.0);
    float w = 0.0, d = z(uv);
    for (int i = -2; i <= 2; i++) {
        vec2 p = uv + float(i) * dir.xy;
        float k = exp(-float(i * i) * 0.5 - abs(z(p) - d) * 4.0);
        c += texture(tex, p).rgb * k;
        w += k;
    }
    o = vec4(c / w, 1.0);
}
