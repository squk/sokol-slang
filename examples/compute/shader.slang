struct Params { int4 control; };
struct Record { uint value; uint marker; };
ConstantBuffer<Params> params;
StructuredBuffer<Record> sourceRecords;
RWStructuredBuffer<Record> resultRecords;

[shader("compute")]
[numthreads(32, 2, 1)]
void computeMain(uint3 id : SV_DispatchThreadID) {
    uint index = id.x + id.y * 64;
    if (index < uint(params.control.x)) {
        Record result;
        result.value = sourceRecords[index].value * uint(params.control.y) + uint(params.control.z);
        result.marker = sourceRecords[index].marker ^ (id.x + (id.y << 16));
        resultRecords[index] = result;
    }
}
