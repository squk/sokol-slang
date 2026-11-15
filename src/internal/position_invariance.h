#pragma once
#include <cstdint>
#include <vector>

namespace sokol_slang {
// Marks vertex BuiltIn Position outputs invariant, including interface-block
// members. Returns owned SPIR-V words; leaves other stages and existing marks
// unchanged. Every backend must translate or emit these same words.
std::vector<uint32_t> preserve_vertex_position_invariance(std::vector<uint32_t> words);
} // namespace sokol_slang
