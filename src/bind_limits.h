#pragma once
#include "sokol_gfx.h"
#include <string>

// Bind slot maxima shared by backend_compile.cc, manifest.cc and
// program_bindings.cc. View and sampler counts come from the sokol_gfx.h this
// compiler was built against; the runtime must be built against the same
// header. The layout output repeats these values so a consumer can detect a
// mismatch.
constexpr int kMaxUniformBindslots = SG_MAX_UNIFORMBLOCK_BINDSLOTS;
constexpr int kMaxViewBindslots = SG_MAX_VIEW_BINDSLOTS;
constexpr int kMaxSamplerBindslots = SG_MAX_SAMPLER_BINDSLOTS;

// Sokol has no acceleration structure slot. This is the only definition of how
// many the compiler numbers, in a slot namespace of their own.
constexpr int kMaxAccelerationSlots = 4;

// Sokol allows storage images only in compute shaders. Fragment storage images
// get a slot namespace of their own; sokol-utils binds them
// (sg_make_shader_fragment_storage). This is the only definition of the count.
constexpr int kMaxFragmentStorageSlots = 4;

// Names the slot namespace of a resource. Sokol's view slots are global across
// stages; acceleration structures and fragment storage images each have their
// own namespace because Sokol has no bind slot for them.
inline std::string bind_space(const std::string &kind, const std::string &stage) {
    if (kind == "uniform" || kind == "sampler" || kind == "acceleration_structure")
        return kind;
    if (kind == "storage_image" && stage == "fragment")
        return "fragment_storage_image";
    return "view";
}

inline int bind_space_limit(const std::string &space) {
    return space == "uniform"                  ? kMaxUniformBindslots
           : space == "sampler"                ? kMaxSamplerBindslots
           : space == "acceleration_structure" ? kMaxAccelerationSlots
           : space == "fragment_storage_image" ? kMaxFragmentStorageSlots
                                               : kMaxViewBindslots;
}
