// Read-only scalar and vector storage buffers in vertex and fragment stages.
// Both stages use float4 elements, so they share one sokol_float4_record.
StructuredBuffer<float4> offsets;
StructuredBuffer<float> scales;
StructuredBuffer<float4> colors;

struct VertexInput {
    float2 position : POSITION;
    uint record : TEXCOORD0;
};
struct FragmentInput {
    float4 position : SV_Position;
    nointerpolation uint record : TEXCOORD0;
};
[shader("vertex")]
FragmentInput vertexMain(VertexInput input) {
    FragmentInput output;
    output.position = float4(input.position * scales[input.record], 0, 1) + offsets[input.record];
    output.record = input.record;
    return output;
}
[shader("fragment")]
float4 fragmentMain(FragmentInput input) : SV_Target0 {
    return colors[input.record];
}
