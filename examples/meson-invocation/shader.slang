struct Uniforms {
    float4 tint;
    float scale;
};
ConstantBuffer<Uniforms> uniforms;

struct VertexInput {
    float2 position : POSITION;
    float3 color : COLOR0;
};

struct VertexOutput {
    float4 position : SV_Position;
    float4 color : COLOR0;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
    VertexOutput output;
    output.position = float4(input.position * uniforms.scale, 0.0, 1.0);
    output.color = float4(input.color, 1.0) * uniforms.tint;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target0 {
    return input.color;
}
