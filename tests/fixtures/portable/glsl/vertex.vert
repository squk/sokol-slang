#version 450
layout(location=0) in vec2 position;
layout(location=1) in vec2 uv;
layout(location=0) out vec2 varying_uv;
layout(set=3, binding=7, std140) uniform VertexParams {
    mat4 transform;
    vec4 tint;
} params;
void main() {
    gl_Position = params.transform * vec4(position, 0.25, 1.0);
    varying_uv = uv;
}
