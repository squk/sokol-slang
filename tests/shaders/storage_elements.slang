// Storage buffers whose elements are scalars or vectors rather than structs.
// The core wraps each element type in a sokol_<type>_record struct.
StructuredBuffer<float4> colors;
StructuredBuffer<float3> offsets;
StructuredBuffer<float> weights;
StructuredBuffer<int2> pairs;
ByteAddressBuffer rawInput;
RWByteAddressBuffer rawOutput;
RWStructuredBuffer<uint> counts;
// Metal cannot query a storage buffer's length under sokol_gfx (no spvBufferSizeConstants), so the
// element count comes in as a uniform (signed: GL uniform uploads take no unsigned integers).
cbuffer params {
    int colorCount;
};

[shader("compute")]
[numthreads(32, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(colorCount))
        return;
    float4 color = colors[id.x];
    float3 offset = offsets[id.x];
    int2 pair = pairs[id.x];
    uint4 raw = rawInput.Load4(id.x * 16);
    rawOutput.Store(id.x * 8, raw.x + uint(pair.x));
    rawOutput.Store(id.x * 8 + 4, raw.w + uint(pair.y));
    InterlockedAdd(counts[0], 1);
    counts[id.x + 1] = uint(color.y * weights[id.x] + offset.z);
}
