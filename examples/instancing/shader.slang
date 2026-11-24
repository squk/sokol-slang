struct Params {
    float2 scale;
    float time;
};
ConstantBuffer<Params> params;

struct VertexInput {
    float2 position : POSITION;
    float2 center : TEXCOORD0;
    float3 color : COLOR0;
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 color : COLOR0;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
    float angle = params.time + 3.0 * input.center.x + 2.0 * input.center.y;
    float s = sin(angle);
    float c = cos(angle);
    float2 rotated = float2(c * input.position.x - s * input.position.y,
                            s * input.position.x + c * input.position.y);
    VertexOutput output;
    output.position = float4((rotated + input.center) * params.scale, 0.0, 1.0);
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target0 {
    return float4(input.color, 1.0);
}
