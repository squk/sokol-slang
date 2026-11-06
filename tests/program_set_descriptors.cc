#include "sokol_gfx.h"

#include "program_set.h"
#include <cstdio>
#include <initializer_list>

// Including the complete header checks that shared stages, types and resources
// from independently generated programs have distinct C symbols.
static_assert(sizeof(programset_base_vertex_params_t) == 80);
static_assert(sizeof(programset_full_vertex_params_t) == 80);
static_assert(ATTR_programset_base_position == ATTR_programset_full_position);

int main() {
    for (auto backend :
         {SG_BACKEND_GLCORE, SG_BACKEND_GLES3, SG_BACKEND_D3D11, SG_BACKEND_METAL_MACOS,
          SG_BACKEND_METAL_IOS, SG_BACKEND_METAL_SIMULATOR, SG_BACKEND_WGPU, SG_BACKEND_VULKAN}) {
        const auto *base = programset_base_shader_desc(backend);
        const auto *full = programset_full_shader_desc(backend);
        const auto *work = programset_work_shader_desc(backend);
        if (!base || !full || !work || base == full || !work->compute_func.entry)
            return 1;
    }
    std::puts("PASS: combined raster/compute header, shared stages, public names and independent "
              "descriptors");
}
