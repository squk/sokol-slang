// Compiler/reflection check independent of the D3D11 dispatch/readback fixture.
#include "hlsl_reflection.h"

int main() {
    try {
        const auto* descriptor = compute_run_shader_desc(SG_BACKEND_D3D11);
        require(descriptor != nullptr, "D3D11 compute descriptor missing");
        HlslReflection compiled(descriptor->compute_func);
        auto* reflection = compiled.reflection.value;
        UINT x = 0, y = 0, z = 0;
        const UINT threads = reflection->GetThreadGroupSize(&x, &y, &z);
        require(x == 32 && y == 2 && z == 1 && threads == 64, "DXBC compute workgroup dimensions disagree with authored shader");
        unsigned uniforms = 0, readers = 0, writers = 0;
        for (UINT i = 0; i < compiled.shader.BoundResources; ++i) {
            D3D11_SHADER_INPUT_BIND_DESC binding = {};
            check_hr(reflection->GetResourceBindingDesc(i, &binding), "compute binding reflection");
            require(binding.BindCount == 1, "unexpected compute resource array");
            bool matched = false;
            if (binding.Type == D3D_SIT_CBUFFER) {
                for (const auto& uniform : descriptor->uniform_blocks) {
                    if (uniform.stage == SG_SHADERSTAGE_COMPUTE && uniform.hlsl_register_b_n == binding.BindPoint) {
                        D3D11_SHADER_BUFFER_DESC buffer = {};
                        check_hr(reflection->GetConstantBufferByName(binding.Name)->GetDesc(&buffer), "compute constant buffer reflection");
                        require(buffer.Size == uniform.size, "DXBC compute uniform size disagrees with Sokol descriptor");
                        matched = true;
                    }
                }
                ++uniforms;
            } else if (binding.Type == D3D_SIT_BYTEADDRESS || binding.Type == D3D_SIT_UAV_RWBYTEADDRESS) {
                const bool readonly = binding.Type == D3D_SIT_BYTEADDRESS;
                require(binding.Dimension == D3D_SRV_DIMENSION_BUFFER, "unexpected compute storage dimension");
                for (const auto& view : descriptor->views) {
                    const auto& buffer = view.storage_buffer;
                    const auto reg = readonly ? buffer.hlsl_register_t_n : buffer.hlsl_register_u_n;
                    if (buffer.stage == SG_SHADERSTAGE_COMPUTE && buffer.readonly == readonly && reg == binding.BindPoint)
                        matched = true;
                }
                if (readonly) ++readers; else ++writers;
            }
            if (!matched) std::fprintf(stderr, "unmatched resource %s: type %u, register %u, dimension %u\n",
                binding.Name, unsigned(binding.Type), binding.BindPoint, unsigned(binding.Dimension));
            require(matched, "DXBC compute resource access/register disagrees with Sokol descriptor");
        }
        require(uniforms == 1 && readers == 1 && writers == 1, "missing compute uniform/SRV/UAV resources");
        std::printf("PASS: %s cs_5_0 DXBC compilation, 32x2 workgroup, uniform size and SRV/UAV registers\n", SHADER_HEADER);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
