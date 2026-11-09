#version 450 core
// One triangle that covers the screen, built from gl_VertexID with no vertex
// buffer. uv spans 0..1 across the visible area.
out vec2 uv;
void main(){
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
