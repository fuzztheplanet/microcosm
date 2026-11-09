#version 450 core
// One step of the bloom's mip chain. Going down a level uses Jimenez's filter
// from Call of Duty: Advanced Warfare, thirteen bilinear taps over a 6 x 6
// block of the level above. The centre, inner four and outer ring are
// weighted to give a wide, smooth falloff that doesn't alias as things move.
// Coming back up (dir.x < 0), a 3 x 3 tent of the level below is added onto
// this one, made from four bilinear taps half a texel off on the diagonals.
in vec2 uv;
out vec4 o;
uniform sampler2D tex;
layout(location = 0) uniform vec2 dir;
vec3 f(float x, float y){ return texture(tex, uv + vec2(x, y) * abs(dir)).rgb; }
void main(){
    float r = dir.x < 0.0 ? 0.5 : 2.0;
    vec3 a = f(-r, -r) + f(r, -r) + f(-r, r) + f(r, r);
    o = vec4(dir.x < 0.0 ? a * 0.25
             : (a + (f(0.0, -r) + f(-r, 0.0) + f(r, 0.0) + f(0.0, r)) * 2.0 + f(0.0, 0.0) * 4.0
                + (f(-1.0, -1.0) + f(1.0, -1.0) + f(-1.0, 1.0) + f(1.0, 1.0)) * 4.0) / 32.0, 1.0);
}
