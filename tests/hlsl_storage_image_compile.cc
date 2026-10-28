#include "hlsl_reflection.h"

int main() {
    try {
        const auto* descriptor = storage_image_run_shader_desc(SG_BACKEND_D3D11);
        require(descriptor != nullptr, "storage image descriptor missing");
        HlslReflection compiled(descriptor->compute_func);
        auto* reflection = compiled.reflection.value;
        UINT x = 0, y = 0, z = 0;
        require(reflection->GetThreadGroupSize(&x, &y, &z) == 32 && x == 8 && y == 4 && z == 1,
                "storage image workgroup mismatch");
        unsigned uniforms = 0, buffers = 0, images = 0;
        for (UINT i = 0; i < compiled.shader.BoundResources; ++i) {
            D3D11_SHADER_INPUT_BIND_DESC binding = {};
            check_hr(reflection->GetResourceBindingDesc(i, &binding), "storage image binding reflection");
            require(binding.BindCount == 1, "unexpected resource array");
            bool matched = false;
            if (binding.Type == D3D_SIT_CBUFFER) {
                for (const auto& uniform : descriptor->uniform_blocks) {
                    if (uniform.stage == SG_SHADERSTAGE_COMPUTE && uniform.hlsl_register_b_n == binding.BindPoint) {
                        D3D11_SHADER_BUFFER_DESC buffer = {};
                        check_hr(reflection->GetConstantBufferByName(binding.Name)->GetDesc(&buffer), "constant buffer reflection");
                        require(buffer.Size == uniform.size, "storage image uniform size mismatch");
                        matched = true;
                    }
                }
                ++uniforms;
            } else if (binding.Type == D3D_SIT_UAV_RWBYTEADDRESS) {
                for (const auto& view : descriptor->views) {
                    const auto& buffer = view.storage_buffer;
                    if (buffer.stage == SG_SHADERSTAGE_COMPUTE && !buffer.readonly && buffer.hlsl_register_u_n == binding.BindPoint)
                        matched = true;
                }
                ++buffers;
            } else if (binding.Type == D3D_SIT_UAV_RWTYPED) {
                require(binding.Dimension == D3D_SRV_DIMENSION_TEXTURE2D, "storage image dimension mismatch");
                for (const auto& view : descriptor->views) {
                    const auto& image = view.storage_image;
                    if (image.stage == SG_SHADERSTAGE_COMPUTE && image.hlsl_register_u_n == binding.BindPoint) {
                        const auto expected = image.access_format == SG_PIXELFORMAT_R32UI ? D3D_RETURN_TYPE_UINT : D3D_RETURN_TYPE_UNORM;
                        require(binding.ReturnType == expected, "storage image texel type mismatch");
                        matched = true;
                    }
                }
                ++images;
            }
            require(matched, "storage image DXBC resource disagrees with Sokol descriptor");
        }
        require(uniforms == 1 && buffers == 1 && images == 2, "missing image/buffer resources");
        std::printf("PASS: %s storage-image DXBC, workgroup, typed/raw UAV registers and texel types\n", SHADER_HEADER);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
