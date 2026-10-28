Texture2D<float> shadow;
SamplerComparisonState lessSmp;
SamplerComparisonState greaterSmp;

struct FragmentInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
FragmentInput vertexMain(uint vertex : SV_VulkanVertexID) {
    float2 position = float2(vertex == 1 ? 3.0 : -1.0, vertex == 2 ? 3.0 : -1.0);
    FragmentInput output;
    output.position = float4(position, 0, 1);
    output.uv = position * 0.5 + 0.5;
    return output;
}

[shader("fragment")]
float4 fragmentMain(FragmentInput input) : SV_Target0 {
    return float4(shadow.SampleCmpLevelZero(lessSmp, input.uv, input.uv.x),
                  shadow.SampleCmpLevelZero(greaterSmp, input.uv, input.uv.y), 0, 1);
}
