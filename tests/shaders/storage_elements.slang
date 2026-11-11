// Storage buffers whose elements are scalars or vectors rather than structs.
// The core wraps each element type in a sokol_<type>_record struct.
StructuredBuffer<float4> colors;
StructuredBuffer<float3> offsets;
StructuredBuffer<float> weights;
StructuredBuffer<int2> pairs;
ByteAddressBuffer rawInput;
RWByteAddressBuffer rawOutput;
RWStructuredBuffer<uint> counts;

[shader("compute")]
[numthreads(32, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID) {
    uint count, stride;
    colors.GetDimensions(count, stride);
    if (id.x >= count)
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
