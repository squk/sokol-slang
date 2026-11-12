#pragma once
#include "sokol_gfx.h"

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
