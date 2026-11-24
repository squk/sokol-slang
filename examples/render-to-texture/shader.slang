struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct Params {
    float time;
};
ConstantBuffer<Params> params;
Texture2D<float4> pattern;
SamplerState filterSampler;

// One triangle covers the viewport. Its UVs span [0, 1] inside the viewport.
[shader("vertex")]
VertexOutput vertexMain(float2 corner : POSITION) {
    VertexOutput output;
    output.position = float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = corner;
    return output;
}

[shader("fragment")]
float4 patternFragment(VertexOutput input) : SV_Target0 {
    float2 p = abs(input.uv * 2.0 - 1.0);
    float radius = length(p);
    float rings = step(0.5, frac(4.0 * radius - params.time));
    float diamond = step(p.x + p.y, 0.87);
    float3 blue = float3(0.07, 0.19, 0.32);
    float3 cyan = float3(0.13, 0.81, 0.76);
    float3 orange = float3(1.0, 0.55, 0.18);
    return float4(lerp(blue, lerp(cyan, orange, diamond), rings), 1.0);
}

[shader("fragment")]
float4 displayFragment(VertexOutput input) : SV_Target0 {
    return pattern.Sample(filterSampler, input.uv);
}
