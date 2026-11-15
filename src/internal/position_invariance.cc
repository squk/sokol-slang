#include "internal/position_invariance.h"
#include "spirv_cross.hpp"
#include <iterator>
#include <set>
#include <stdexcept>

namespace sokol_slang {
namespace {
// SPIR-V puts annotations after module declarations and debug names, before
// types and executable instructions. No IDs or instruction operands change.
bool before_types(spv::Op op) {
    switch (op) {
    case spv::OpCapability:
    case spv::OpExtension:
    case spv::OpExtInstImport:
    case spv::OpMemoryModel:
    case spv::OpEntryPoint:
    case spv::OpExecutionMode:
    case spv::OpExecutionModeId:
    case spv::OpString:
    case spv::OpSource:
    case spv::OpSourceContinued:
    case spv::OpSourceExtension:
    case spv::OpName:
    case spv::OpMemberName:
    case spv::OpModuleProcessed:
    case spv::OpDecorate:
    case spv::OpMemberDecorate:
    case spv::OpDecorationGroup:
    case spv::OpGroupDecorate:
    case spv::OpGroupMemberDecorate:
    case spv::OpDecorateId:
    case spv::OpDecorateString:
    case spv::OpMemberDecorateString:
        return true;
    default:
        return false;
    }
}
} // namespace

std::vector<uint32_t> preserve_vertex_position_invariance(std::vector<uint32_t> words) {
    spirv_cross::Compiler compiler(words);
    bool vertex = false;
    for (const auto &entry : compiler.get_entry_points_and_stages())
        vertex |= entry.execution_model == spv::ExecutionModelVertex;
    if (!vertex)
        return words;
    std::vector<uint32_t> annotations;
    std::set<std::pair<uint32_t, uint32_t>> marked;
    for (const auto &output : compiler.get_shader_resources().builtin_outputs) {
        if (output.builtin != spv::BuiltInPosition)
            continue;
        const auto &resource = output.resource;
        if (compiler.has_decoration(resource.id, spv::DecorationBuiltIn)) {
            if (!compiler.has_decoration(resource.id, spv::DecorationInvariant) &&
                marked.emplace(resource.id, UINT32_MAX).second)
                annotations.insert(annotations.end(), {uint32_t(3u << 16) | spv::OpDecorate,
                                                       resource.id, spv::DecorationInvariant});
        } else {
            const auto &type = compiler.get_type(resource.base_type_id);
            for (uint32_t member = 0; member < type.member_types.size(); ++member) {
                if (!compiler.has_member_decoration(resource.base_type_id, member,
                                                    spv::DecorationBuiltIn) ||
                    compiler.get_member_decoration(resource.base_type_id, member,
                                                   spv::DecorationBuiltIn) !=
                        spv::BuiltInPosition ||
                    compiler.has_member_decoration(resource.base_type_id, member,
                                                   spv::DecorationInvariant) ||
                    !marked.emplace(resource.base_type_id, member).second)
                    continue;
                annotations.insert(annotations.end(),
                                   {uint32_t(4u << 16) | spv::OpMemberDecorate,
                                    resource.base_type_id, member, spv::DecorationInvariant});
            }
        }
    }
    if (annotations.empty())
        return words;
    size_t offset = 5;
    while (offset < words.size()) {
        const uint32_t count = words[offset] >> 16;
        if (!count || count > words.size() - offset)
            throw std::runtime_error("invalid SPIR-V instruction length");
        if (!before_types(spv::Op(words[offset] & 0xffff)))
            break;
        offset += count;
    }
    words.insert(words.begin() + offset, annotations.begin(), annotations.end());
    return words;
}
} // namespace sokol_slang
