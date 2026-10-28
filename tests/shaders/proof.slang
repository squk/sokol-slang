struct Params {
    float scale;
    float3 bias;
    float4x4 transform;
    float4 colors[2];
};
struct Record {
    float3 offset;
    float weight;
    float4 tint;
};
ConstantBuffer<Params> params;
StructuredBuffer<Record> records;
Texture2D<float4> tex;
SamplerState smp;

struct VertexInput {
    float2 position : POSITION;
    uint recordIndex : TEXCOORD0;
};
struct FragmentInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint recordIndex : TEXCOORD1;
};
[shader("vertex")]
FragmentInput vertexMain(VertexInput input, uint instance : SV_InstanceID) {
    FragmentInput output;
    Record record = records[input.recordIndex + instance];
    float3 position = float3(input.position * params.scale, 0) + params.bias + record.offset;
    output.position = mul(params.transform, float4(position, 1));
    output.uv = input.position * 0.5 + 0.5;
    output.recordIndex = input.recordIndex + instance;
    return output;
}
[shader("fragment")]
float4 fragmentMain(FragmentInput input) : SV_Target0 {
    Record record = records[input.recordIndex];
    return tex.Sample(smp, input.uv) * record.tint * record.weight * params.colors[1];
}
