struct Params {
    float4x4 transform;
    float4 tint;
};
ConstantBuffer<Params> params;
Texture2D<float4> tex;
SamplerState smp;
struct VertexInput {
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
};
struct FragmentInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};
[shader("vertex")]
FragmentInput vertexMain(VertexInput input) {
    FragmentInput output;
    output.position = mul(params.transform, float4(input.position, 0.25, 1));
    output.uv = input.uv;
    return output;
}
[shader("fragment")]
float4 fragmentMain(FragmentInput input) : SV_Target0 {
    return tex.Sample(smp, input.uv) * params.tint;
}
