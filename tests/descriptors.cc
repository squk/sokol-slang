#include "sokol_gfx.h"
#include SHADER_HEADER
#include <cstdio>
#include <cstring>
#include <initializer_list>

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } } while (0)

int main() {
    const sg_backend backends[] = {SG_BACKEND_GLCORE, SG_BACKEND_GLES3, SG_BACKEND_D3D11,
        SG_BACKEND_METAL_MACOS, SG_BACKEND_METAL_IOS, SG_BACKEND_METAL_SIMULATOR,
        SG_BACKEND_WGPU, SG_BACKEND_VULKAN};
    for (auto backend : backends) {
        auto desc = portable_draw_shader_desc(backend);
        CHECK(desc);
        CHECK(desc->uniform_blocks[UB_portable_vertex_params].size == sizeof(portable_vertex_params_t));
        CHECK(desc->uniform_blocks[UB_portable_fragment_params].size == sizeof(portable_fragment_params_t));
        CHECK(sizeof(portable_vertex_params_t) == 80);
        CHECK(desc->views[VIEW_portable_fragment_tex].texture.stage == SG_SHADERSTAGE_FRAGMENT);
        CHECK(desc->samplers[SMP_portable_fragment_smp].sampler_type == SG_SAMPLERTYPE_FILTERING);
        CHECK(desc->texture_sampler_pairs[0].view_slot == VIEW_portable_fragment_tex);
        CHECK(desc->texture_sampler_pairs[0].sampler_slot == SMP_portable_fragment_smp);
        if (backend == SG_BACKEND_VULKAN) {
            for (auto* fn : {&desc->vertex_func, &desc->fragment_func}) {
                CHECK(fn->bytecode.ptr && fn->bytecode.size >= 20);
                uint32_t magic; std::memcpy(&magic, fn->bytecode.ptr, sizeof(magic));
                CHECK(magic == 0x07230203);
                CHECK(std::strcmp(fn->entry, "main") == 0);
            }
        } else {
            CHECK(desc->vertex_func.source && *desc->vertex_func.source);
            CHECK(desc->fragment_func.source && *desc->fragment_func.source);
        }
        if (backend == SG_BACKEND_WGPU) {
            CHECK(desc->uniform_blocks[UB_portable_vertex_params].wgsl_group0_binding_n == UB_portable_vertex_params);
            CHECK(desc->views[VIEW_portable_fragment_tex].texture.wgsl_group1_binding_n == VIEW_portable_fragment_tex);
            CHECK(desc->samplers[SMP_portable_fragment_smp].wgsl_group1_binding_n == 32 + SMP_portable_fragment_smp);
        }
        if (backend == SG_BACKEND_D3D11) {
            CHECK(std::strcmp(desc->attrs[0].hlsl_sem_name, "TEXCOORD") == 0);
            CHECK(desc->attrs[0].hlsl_sem_index == 0);
        }
    }
    CHECK(portable_draw_shader_desc(SG_BACKEND_DUMMY) == nullptr);
    std::puts("PASS: descriptors for every Sokol backend");
}
