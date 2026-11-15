// Native uniform blocks preserve all unsigned bits; GL must reject its unsupported upload ABI.
struct UnsignedUniforms {
    uint scalar;
    uint2 pair;
    uint3 triple;
    uint4 vector;
    uint4 vectors[2];
};
ConstantBuffer<UnsignedUniforms> unsigned_params;
RWStructuredBuffer<uint4> write_buffer;

[shader("compute")]
[numthreads(32, 1, 1)]
void unsignedMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= 1u) return;
    write_buffer[0] = unsigned_params.vector + unsigned_params.vectors[1]
        + uint4(unsigned_params.pair, unsigned_params.scalar, unsigned_params.triple.z);
}
