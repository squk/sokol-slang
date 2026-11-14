Texture2D<float4> _spatial_vertex_texture;
SamplerState _spatial_vertex_sampler;
struct VertexOutput {
    float4 position : SV_Position;
    float4 color : COLOR0;
};
[shader("vertex")]
VertexOutput vertexMain(uint index : SV_VulkanVertexID) {
    VertexOutput result;
    float2 position = float2(index == 1 ? 3.0 : -1.0, index == 2 ? 3.0 : -1.0);
    result.position = float4(position, 0.0, 1.0);
    result.color = _spatial_vertex_texture.SampleLevel(_spatial_vertex_sampler, position * 0.5 + 0.5, 0.0);
    return result;
}
[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target0 { return input.color; }
