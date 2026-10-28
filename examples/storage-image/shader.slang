struct Params { int4 control; };
struct Record { uint value; uint marker; };
ConstantBuffer<Params> params;
[format("rgba8")] WTexture2D<float4> colors;
[format("r32ui")] RWTexture2D<uint> counters;
RWStructuredBuffer<Record> resultRecords;

[shader("compute")]
[numthreads(8, 4, 1)]
void computeMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(params.control.x) || id.y >= uint(params.control.y)) return;
    uint index = id.x + id.y * uint(params.control.x);
    if (params.control.z == 0) {
        counters[id.xy] = index * 3 + 7;
        colors.Store(int2(id.xy), float4(float(id.x) / 16, float(id.y) / 16, 0.5, 1));
    } else {
        uint value = counters[id.xy];
        counters[id.xy] = value + uint(params.control.w);
        Record result;
        result.value = value;
        result.marker = value + uint(params.control.w);
        resultRecords[index] = result;
    }
}
