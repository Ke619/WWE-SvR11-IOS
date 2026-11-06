#version 450
layout(location = 0) out vec3 c;
void main() {
    vec2 p[3] = vec2[](vec2(0.0, -0.7), vec2(0.7, 0.7), vec2(-0.7, 0.7));
    vec3 k[3] = vec3[](vec3(1, 0, 0), vec3(0, 1, 0), vec3(0, 0, 1));
    gl_Position = vec4(p[gl_VertexIndex], 0, 1);
    c = k[gl_VertexIndex];
}
