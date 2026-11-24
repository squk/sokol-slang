struct Params {
    float time;
};
ConstantBuffer<Params> params;
[format("rgba8")] WTexture2D<float4> outputImage;
Texture2D<float4> displayImage;
SamplerState displaySampler;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    outputImage.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) {
        return;
    }
    float2 p = (float2(id.xy) + 0.5) / float2(width, height) * 2.0 - 1.0;
    float radius = length(p);
    float angle = atan2(p.y, p.x);
    float wave = 0.5 + 0.5 * cos(24.0 * radius - 6.0 * angle + params.time * 3.0);
    float3 ink = float3(0.05, 0.08, 0.23);
    float3 mint = float3(0.16, 0.9, 0.65);
    float3 pink = float3(1.0, 0.35, 0.61);
    float3 color = lerp(mint, pink, saturate(radius));
    outputImage.Store(int2(id.xy), float4(lerp(ink, color, wave), 1.0));
}

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(float2 corner : POSITION) {
    VertexOutput output;
    output.position = float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = corner;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target0 {
    return displayImage.Sample(displaySampler, input.uv);
}
