#version 450
layout(location=0) in vec2 varying_uv;
layout(location=0) out vec4 color;
layout(set=5, binding=9, std140) uniform FragmentParams {
    mat4 transform;
    vec4 tint;
} params;
layout(set=5, binding=11) uniform texture2D tex;
layout(set=5, binding=13) uniform sampler smp;
void main() {
    color = texture(sampler2D(tex, smp), varying_uv) * params.tint;
}
