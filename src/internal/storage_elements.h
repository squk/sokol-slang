#pragma once
#include <cstdint>
#include <vector>

namespace sokol_slang {
// Sokol reflection requires each storage buffer to hold one unsized array of
// records. Rewrites buffers of 32-bit scalars or vectors so every element is a
// one-member record named sokol_<type>_record with member value. ArrayStride
// and all pointer result types are unchanged; access chains gain a constant 0
// member index after the array index. Throws on unsupported element types.
std::vector<uint32_t> wrap_storage_buffer_elements(std::vector<uint32_t> words);
} // namespace sokol_slang
