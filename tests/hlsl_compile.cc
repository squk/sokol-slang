// Compile and reflect DXBC without requiring a graphics device. Rendering is
// checked separately by d3d11-proof; a compiler pass cannot stand in for it.
#include "hlsl_reflection.h"

static void compile_stage(const sg_shader_desc& descriptor, const sg_shader_function& function, sg_shader_stage stage) {
    HlslReflection compiled(function);
    auto* reflection = compiled.reflection.value;
    const auto& shader = compiled.shader;
    unsigned uniform_count = 0, texture_count = 0, sampler_count = 0;
    for (UINT i = 0; i < shader.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC binding = {};
        require(SUCCEEDED(reflection->GetResourceBindingDesc(i, &binding)), "binding reflection failed");
        require(binding.BindCount == 1, "unexpected resource array");
        bool matched = false;
        switch (binding.Type) {
            case D3D_SIT_CBUFFER:
                for (const auto& uniform : descriptor.uniform_blocks) {
                    if (uniform.stage == stage && uniform.hlsl_register_b_n == binding.BindPoint) {
                        D3D11_SHADER_BUFFER_DESC buffer = {};
                        require(SUCCEEDED(reflection->GetConstantBufferByName(binding.Name)->GetDesc(&buffer)),
                                "constant buffer reflection failed");
                        require(buffer.Size == uniform.size, "DXBC constant buffer size disagrees with Sokol descriptor");
                        matched = true;
                    }
                }
                ++uniform_count;
                break;
            case D3D_SIT_TEXTURE:
                for (const auto& view : descriptor.views) {
                    if (view.texture.stage == stage && view.texture.hlsl_register_t_n == binding.BindPoint) {
                        require(binding.Dimension == D3D_SRV_DIMENSION_TEXTURE2D, "unexpected texture dimension");
                        matched = true;
                    }
                }
                ++texture_count;
                break;
            case D3D_SIT_SAMPLER:
                for (const auto& sampler : descriptor.samplers) {
                    if (sampler.stage == stage && sampler.hlsl_register_s_n == binding.BindPoint) matched = true;
                }
                ++sampler_count;
                break;
            default: break;
        }
        require(matched, "DXBC resource register disagrees with Sokol descriptor");
    }
    // Both fixtures deliberately use one buffer per stage and a fragment texture/sampler.
    require(uniform_count == 1, "expected one active constant buffer");
    const unsigned fragment_resources = stage == SG_SHADERSTAGE_FRAGMENT ? 1 : 0;
    require(texture_count == fragment_resources && sampler_count == fragment_resources, "missing texture/sampler resources");
    if (stage == SG_SHADERSTAGE_VERTEX) {
        require(shader.InputParameters == 2, "expected two vertex attributes");
        for (UINT i = 0; i < shader.InputParameters; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC input = {};
            require(SUCCEEDED(reflection->GetInputParameterDesc(i, &input)), "vertex input reflection failed");
            bool matched = false;
            for (const auto& attr : descriptor.attrs) {
                if (attr.hlsl_sem_name && std::strcmp(attr.hlsl_sem_name, input.SemanticName) == 0 &&
                    attr.hlsl_sem_index == input.SemanticIndex) matched = true;
            }
            require(matched, "DXBC vertex semantics disagree with Sokol descriptor");
        }
    }
    std::printf("PASS: %s %s DXBC compilation, buffer sizes, resource registers and vertex semantics\n",
                SHADER_HEADER, function.d3d11_target);
}

int main() {
    try {
        const auto* descriptor = portable_draw_shader_desc(SG_BACKEND_D3D11);
        require(descriptor != nullptr, "D3D11 descriptor missing");
        compile_stage(*descriptor, descriptor->vertex_func, SG_SHADERSTAGE_VERTEX);
        compile_stage(*descriptor, descriptor->fragment_func, SG_SHADERSTAGE_FRAGMENT);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
