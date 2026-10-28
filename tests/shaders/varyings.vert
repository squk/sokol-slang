#version 450
layout(location=0) in vec2 position;
layout(location=1) in vec2 uv;
layout(location=0) out float uv_x;
layout(location=1) out vec2 unused_middle;
layout(location=2) out float uv_y;
layout(location=3) out vec2 unused_tail;
layout(set=3, binding=7, std140) uniform VertexParams {
    mat4 transform;
    vec4 tint;
} params;
void main() {
    gl_Position = params.transform * vec4(position, 0.25, 1.0);
    uv_x = uv.x;
    unused_middle = vec2(7, 11);
    uv_y = uv.y;
    unused_tail = vec2(13, 17);
}
