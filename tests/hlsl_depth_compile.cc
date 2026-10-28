#include "hlsl_reflection.h"

int main() {
    try {
        const auto* descriptor = depth_draw_shader_desc(SG_BACKEND_D3D11);
        require(descriptor != nullptr, "D3D11 depth descriptor missing");
        HlslReflection vertex(descriptor->vertex_func);
        require(vertex.shader.InputParameters == 1, "expected vertex ID input");
        D3D11_SIGNATURE_PARAMETER_DESC input = {};
        check_hr(vertex.reflection.value->GetInputParameterDesc(0, &input), "vertex input reflection");
        require(input.SystemValueType == D3D_NAME_VERTEX_ID, "vertex ID semantic missing");
        HlslReflection fragment(descriptor->fragment_func);
        unsigned textures = 0, samplers = 0;
        for (UINT i = 0; i < fragment.shader.BoundResources; ++i) {
            D3D11_SHADER_INPUT_BIND_DESC binding = {};
            check_hr(fragment.reflection.value->GetResourceBindingDesc(i, &binding), "fragment binding reflection");
            bool matched = false;
            if (binding.Type == D3D_SIT_TEXTURE) {
                require(binding.Dimension == D3D_SRV_DIMENSION_TEXTURE2D && binding.ReturnType == D3D_RETURN_TYPE_FLOAT,
                        "expected floating-point 2D depth texture");
                for (const auto& view : descriptor->views) {
                    if (view.texture.stage == SG_SHADERSTAGE_FRAGMENT &&
                        view.texture.hlsl_register_t_n == binding.BindPoint &&
                        view.texture.sample_type == SG_IMAGESAMPLETYPE_DEPTH) matched = true;
                }
                ++textures;
            } else if (binding.Type == D3D_SIT_SAMPLER) {
                require(binding.uFlags & D3D_SIF_COMPARISON_SAMPLER, "expected comparison sampler bytecode");
                for (const auto& sampler : descriptor->samplers) {
                    if (sampler.stage == SG_SHADERSTAGE_FRAGMENT && sampler.hlsl_register_s_n == binding.BindPoint &&
                        sampler.sampler_type == SG_SAMPLERTYPE_COMPARISON) matched = true;
                }
                ++samplers;
            }
            require(matched && binding.BindCount == 1, "depth resource register/type disagrees with Sokol descriptor");
        }
        require(textures == 1 && samplers == 2, "expected one depth texture and two comparison samplers");
        std::puts("PASS: DXBC depth sampling, comparison sampler registers and vertex ID");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
