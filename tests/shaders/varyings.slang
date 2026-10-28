// Keep the portable host interface so every backend can reuse its pixel oracle.
struct Params { float4x4 transform; float4 tint; };
ConstantBuffer<Params> params;
Texture2D<float4> tex;
SamplerState smp;
struct VertexInput {
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
};
struct FragmentInput {
    float4 position : SV_Position;
    float uvX : TEXCOORD0;
    float2 unusedMiddle : TEXCOORD1;
    float uvY : TEXCOORD2;
    float2 unusedTail : TEXCOORD3;
};
[shader("vertex")]
FragmentInput vertexMain(VertexInput input) {
    FragmentInput output;
    output.position = mul(params.transform, float4(input.position, 0.25, 1));
    output.uvX = input.uv.x;
    output.unusedMiddle = float2(7, 11);
    output.uvY = input.uv.y;
    output.unusedTail = float2(13, 17);
    return output;
}
[shader("fragment")]
float4 fragmentMain(FragmentInput input) : SV_Target0 {
    return tex.Sample(smp, float2(input.uvX, input.uvY)) * params.tint;
}
