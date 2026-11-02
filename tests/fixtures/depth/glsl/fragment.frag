#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(set=3, binding=12) uniform texture2D shadow;
layout(set=3, binding=7) uniform samplerShadow lessSmp;
layout(set=3, binding=1) uniform samplerShadow greaterSmp;
void main() {
    color = vec4(textureLod(sampler2DShadow(shadow, lessSmp), vec3(uv, uv.x), 0),
                 textureLod(sampler2DShadow(shadow, greaterSmp), vec3(uv, uv.y), 0), 0, 1);
}
