#include "backend_internal.h"
#include "generators/sokolc.h"
#include "manifest.h"
#include "native_bytecode.h"
#include "program_bindings.h"
#include "sokol_gfx.h"
#include "spirv-tools/libspirv.hpp"
#include "spirv_hlsl.hpp"
#include "spirv_msl.hpp"
#include "spirv_reflect.hpp"
#include "tint/tint.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

using namespace shdc;
using namespace spirv_cross;
static_assert(MaxUniformBlocks == SG_MAX_UNIFORMBLOCK_BINDSLOTS);
static_assert(MaxViews == SG_MAX_VIEW_BINDSLOTS);
static_assert(MaxSamplers == SG_MAX_SAMPLER_BINDSLOTS);

// Reuse Sokol's reflection and C generator without its shader frontend.
namespace shdc {
bool Spirvcross::can_flatten_uniform_block(const Compiler &compiler, const Resource &resource) {
    const auto &block = compiler.get_type(resource.base_type_id);
    auto basic = SPIRType::Unknown;
    for (auto member : block.member_types) {
        const auto type = compiler.get_type(member).basetype;
        if (basic == SPIRType::Unknown) {
            basic = type;
            if (type != SPIRType::Float && type != SPIRType::Int)
                return false;
        } else if (type != basic)
            return false;
    }
    return true;
}
const SpirvcrossSource *Spirvcross::find_source_by_snippet_index(int index) const {
    for (const auto &source : sources)
        if (source.snippet_index == index)
            return &source;
    return nullptr;
}
const BytecodeBlob *Bytecode::find_blob_by_snippet_index(int index) const {
    for (const auto &blob : blobs)
        if (blob.snippet_index == index)
            return &blob;
    return nullptr;
}
} // namespace shdc

static void check(const ErrMsg &error) {
    if (error.valid()) {
        error.print(ErrMsg::GCC);
        throw std::runtime_error("Sokol rejected the shader interface");
    }
}

static std::string storage_image_format(const SPIRType &type) {
    switch (type.image.format) {
    case spv::ImageFormatRgba8:
        return "rgba8";
    case spv::ImageFormatRgba8Snorm:
        return "rgba8_snorm";
    case spv::ImageFormatRgba8ui:
        return "rgba8ui";
    case spv::ImageFormatRgba8i:
        return "rgba8i";
    case spv::ImageFormatRgba16ui:
        return "rgba16ui";
    case spv::ImageFormatRgba16i:
        return "rgba16i";
    case spv::ImageFormatRgba16f:
        return "rgba16f";
    case spv::ImageFormatR32ui:
        return "r32ui";
    case spv::ImageFormatR32i:
        return "r32i";
    case spv::ImageFormatR32f:
        return "r32f";
    case spv::ImageFormatRg32ui:
        return "rg32ui";
    case spv::ImageFormatRg32i:
        return "rg32i";
    case spv::ImageFormatRg32f:
        return "rg32f";
    case spv::ImageFormatRgba32ui:
        return "rgba32ui";
    case spv::ImageFormatRgba32i:
        return "rgba32i";
    case spv::ImageFormatRgba32f:
        return "rgba32f";
    default:
        throw std::runtime_error(
            "storage image requires an explicit Sokol-supported SPIR-V format");
    }
}

static std::string storage_image_access(const Compiler &compiler, const Resource &resource) {
    if (compiler.has_decoration(resource.id, spv::DecorationNonWritable))
        throw std::runtime_error("Sokol does not support read-only storage images; use a texture");
    return compiler.has_decoration(resource.id, spv::DecorationNonReadable) ? "write"
                                                                            : "read_write";
}

static void validate_spirv(const std::vector<uint32_t> &words, spv_target_env environment,
                           const std::string &prefix) {
    spvtools::SpirvTools validator(environment);
    std::string errors;
    validator.SetMessageConsumer(
        [&](spv_message_level_t, const char *, const spv_position_t &, const char *message) {
            errors += std::string(message) + "\n";
        });
    if (!validator.Validate(words))
        throw std::runtime_error(prefix + errors);
}

static std::vector<uint32_t> read_spirv(const char *path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        throw std::runtime_error("cannot open SPIR-V input");
    auto size = stream.tellg();
    if (size < 20 || size % 4)
        throw std::runtime_error("invalid SPIR-V size");
    std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
    stream.seekg(0);
    stream.read(reinterpret_cast<char *>(words.data()), size);
    if (!stream || words[0] != 0x07230203)
        throw std::runtime_error("invalid SPIR-V input");
    validate_spirv(words, SPV_ENV_UNIVERSAL_1_3, "invalid SPIR-V: ");
    return words;
}

// Public resource names come from metadata and do not depend on debug names.
static void name_resources(Compiler &compiler, const std::string &stage,
                           const std::map<std::string, Binding> &metadata) {
    const auto resources = compiler.get_shader_resources();
    std::set<uint32_t> types;
    std::set<std::pair<uint32_t, uint32_t>> descriptors;
    std::function<void(uint32_t)> name_members = [&](uint32_t id) {
        const auto &type = compiler.get_type(id);
        if (type.basetype != SPIRType::Struct)
            return;
        if (compiler.get_name(type.self).empty())
            compiler.set_name(type.self, "type_" + std::to_string(uint32_t(type.self)));
        for (uint32_t i = 0; i < type.member_types.size(); ++i) {
            if (compiler.get_member_name(type.self, i).empty())
                compiler.set_member_name(type.self, i, "member_" + std::to_string(i));
            name_members(type.member_types[i]);
        }
    };
    auto rename = [&](const auto &list, bool buffer) {
        for (const auto &resource : list) {
            if (!compiler.has_decoration(resource.id, spv::DecorationDescriptorSet) ||
                !compiler.has_decoration(resource.id, spv::DecorationBinding))
                throw std::runtime_error("resource has no descriptor set/binding");
            uint32_t set = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
            uint32_t binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
            if (!descriptors.emplace(set, binding).second)
                throw std::runtime_error("multiple resources use one SPIR-V descriptor");
            std::string name;
            for (const auto &[alias, item] : metadata)
                if (item.stage == stage && item.set == set && item.binding == binding)
                    name = alias;
            if (name.empty())
                throw std::runtime_error("missing or wrong metadata for " + stage + " set " +
                                         std::to_string(set) + " binding " +
                                         std::to_string(binding));
            compiler.set_name(resource.id, name);
            if (buffer) {
                if (!types.insert(resource.base_type_id).second)
                    throw std::runtime_error(
                        "multiple buffers with one block type are not supported yet");
                name_members(resource.base_type_id);
                compiler.set_name(resource.base_type_id, name);
            }
        }
    };
    rename(resources.uniform_buffers, true);
    rename(resources.storage_buffers, true);
    rename(resources.storage_images, false);
    rename(resources.separate_images, false);
    rename(resources.separate_samplers, false);
    auto name_attributes = [&](const auto &list, const std::string &prefix) {
        for (const auto &resource : list)
            if (compiler.get_name(resource.id).empty())
                compiler.set_name(resource.id, prefix + std::to_string(compiler.get_decoration(
                                                            resource.id, spv::DecorationLocation)));
    };
    name_attributes(resources.stage_inputs, "input_");
    name_attributes(resources.stage_outputs, "output_");
    // Stage links use locations; compiler-specific variable names need not match.
    const auto &varyings = stage == "vertex" ? resources.stage_outputs : resources.stage_inputs;
    for (const auto &varying : varyings) {
        if (!compiler.has_decoration(varying.id, spv::DecorationLocation))
            throw std::runtime_error("varying has no location");
        compiler.set_name(varying.id, "varying_" + std::to_string(compiler.get_decoration(
                                                       varying.id, spv::DecorationLocation)));
    }
}

// Sokol's generators assume the SPIR-V entry is main. Normalize that name in
// the binary so Vulkan and Tint receive exactly the entry used by reflection.
static std::vector<uint32_t> normalize_entry(std::vector<uint32_t> words,
                                             const std::string &entry) {
    Compiler inspect(words);
    auto entries = inspect.get_entry_points_and_stages();
    if (entries.size() != 1 || entries[0].name != entry)
        throw std::runtime_error("module must have exactly one entry point matching the manifest");
    std::vector<uint32_t> output(words.begin(), words.begin() + 5);
    for (size_t pos = 5; pos < words.size();) {
        uint32_t count = words[pos] >> 16;
        uint32_t opcode = words[pos] & 0xffff;
        if (opcode == spv::OpEntryPoint) {
            size_t name_words = entry.size() / 4 + 1;
            output.push_back(((count - name_words + 2) << 16) | opcode);
            output.push_back(words[pos + 1]);
            output.push_back(words[pos + 2]);
            output.push_back(0x6e69616d);
            output.push_back(0);
            output.insert(output.end(), words.begin() + pos + 3 + name_words,
                          words.begin() + pos + count);
        } else
            output.insert(output.end(), words.begin() + pos, words.begin() + pos + count);
        pos += count;
    }
    return output;
}

// Sokol's C fields stay flat when a buffer contains a one-member array wrapper.
// GL lookup names must retain the real member path in the emitted shader.
class SokolGenerator : public gen::SokolCGenerator {
  public:
    std::map<std::string, std::string> uniform_paths;
    std::string take_header() { return std::move(content); }
    Json describe_interface(const refl::ProgramReflection &program);
    std::string native_profile(Slang::Enum target, ShaderStage::Enum stage) {
        const char *profile = hlsl_target(target, stage);
        return profile ? profile : "";
    }
    std::string internal_program_prefix, public_program_prefix;

  private:
    ErrMsg end(const gen::GenInput &) override {
        if (!internal_program_prefix.empty()) {
            size_t position = 0;
            while ((position = content.find(internal_program_prefix, position)) !=
                   std::string::npos) {
                content.replace(position, internal_program_prefix.size(), public_program_prefix);
                position += public_program_prefix.size();
            }
        }
        for (const auto &[field, path] : uniform_paths) {
            const auto from = "glsl_name = \"" + field + "\"";
            const auto to = "glsl_name = \"" + path + "\"";
            size_t start = 0;
            while ((start = content.find(from, start)) != std::string::npos) {
                content.replace(start, from.size(), to);
                start += to.size();
            }
        }
        return {};
    }
};

Json SokolGenerator::describe_interface(const refl::ProgramReflection &program) {
    Json result = {{"name", program.name},
                   {"attributes", Json::array()},
                   {"uniform_blocks", Json::array()},
                   {"textures", Json::array()},
                   {"samplers", Json::array()},
                   {"storage_buffers", Json::array()},
                   {"storage_images", Json::array()},
                   {"texture_sampler_pairs", Json::array()},
                   {"fragment_outputs", Json::array()}};
    auto native_indices = [](Json &item, const auto &resource) {
        if constexpr (requires { resource.hlsl_register_b_n; })
            if (resource.hlsl_register_b_n >= 0)
                item["hlsl_register_b_n"] = resource.hlsl_register_b_n;
        if constexpr (requires { resource.hlsl_register_t_n; })
            if (resource.hlsl_register_t_n >= 0)
                item["hlsl_register_t_n"] = resource.hlsl_register_t_n;
        if constexpr (requires { resource.hlsl_register_u_n; })
            if (resource.hlsl_register_u_n >= 0)
                item["hlsl_register_u_n"] = resource.hlsl_register_u_n;
        if constexpr (requires { resource.hlsl_register_s_n; })
            if (resource.hlsl_register_s_n >= 0)
                item["hlsl_register_s_n"] = resource.hlsl_register_s_n;
        if constexpr (requires { resource.msl_buffer_n; })
            if (resource.msl_buffer_n >= 0)
                item["msl_buffer_n"] = resource.msl_buffer_n;
        if constexpr (requires { resource.msl_texture_n; })
            if (resource.msl_texture_n >= 0)
                item["msl_texture_n"] = resource.msl_texture_n;
        if constexpr (requires { resource.msl_sampler_n; })
            if (resource.msl_sampler_n >= 0)
                item["msl_sampler_n"] = resource.msl_sampler_n;
        if constexpr (requires { resource.wgsl_group0_binding_n; })
            if (resource.wgsl_group0_binding_n >= 0)
                item["wgsl_group0_binding_n"] = resource.wgsl_group0_binding_n;
        if constexpr (requires { resource.wgsl_group1_binding_n; })
            if (resource.wgsl_group1_binding_n >= 0)
                item["wgsl_group1_binding_n"] = resource.wgsl_group1_binding_n;
        if constexpr (requires { resource.spirv_set0_binding_n; })
            if (resource.spirv_set0_binding_n >= 0)
                item["spirv_set0_binding_n"] = resource.spirv_set0_binding_n;
        if constexpr (requires { resource.spirv_set1_binding_n; })
            if (resource.spirv_set1_binding_n >= 0)
                item["spirv_set1_binding_n"] = resource.spirv_set1_binding_n;
        if constexpr (requires { resource.glsl_binding_n; })
            if (resource.glsl_binding_n >= 0)
                item["glsl_binding_n"] = resource.glsl_binding_n;
    };
    if (program.has_vs()) {
        for (const auto &attribute : program.vs().inputs) {
            if (attribute.slot < 0)
                continue;
            result["attributes"].push_back(
                {{"slot", attribute.slot},
                 {"name", attribute.name},
                 {"base_type", attr_basetype(attribute.type_info.basetype())},
                 {"hlsl_sem_name", attribute.sem_name},
                 {"hlsl_sem_index", attribute.sem_index}});
        }
    }
    if (program.has_fs()) {
        for (const auto &output : program.fs().outputs)
            if (output.slot >= 0)
                result["fragment_outputs"].push_back({{"slot", output.slot},
                                                      {"name", output.name},
                                                      {"type", output.type_info.type_as_glsl()}});
    }
    if (program.has_cs())
        result["workgroup_size"] = program.cs().cs_workgroup_size;
    for (const auto &block : program.bindings.uniform_blocks) {
        Json item = {{"name", block.name},
                     {"slot", block.sokol_slot},
                     {"stage", shader_stage(block.stage)},
                     {"size", roundup(block.struct_info.size, 16)},
                     {"instance_name", block.inst_name},
                     {"flattened", block.flattened},
                     {"members", Json::array()}};
        native_indices(item, block);
        if (block.flattened && !block.struct_info.struct_items.empty()) {
            item["flattened_type"] = flattened_uniform_type(block.struct_info.struct_items[0].type);
            item["flattened_count"] = roundup(block.struct_info.size, 16) / 16;
        }
        for (const auto &member : block.struct_info.struct_items) {
            const auto path = block.inst_name + "." + member.name;
            const auto found = uniform_paths.find(path);
            item["members"].push_back(
                {{"name", member.name},
                 {"offset", member.offset},
                 {"type", uniform_type(member.type)},
                 {"array_count", member.array_count},
                 {"binding_name", found == uniform_paths.end() ? path : found->second}});
        }
        result["uniform_blocks"].push_back(std::move(item));
    }
    for (const auto &texture : program.bindings.textures) {
        Json item = {{"name", texture.name},
                     {"slot", texture.sokol_slot},
                     {"stage", shader_stage(texture.stage)},
                     {"image_type", image_type(texture.type)},
                     {"sample_type", image_sample_type(texture.sample_type)},
                     {"multisampled", texture.multisampled}};
        native_indices(item, texture);
        result["textures"].push_back(std::move(item));
    }
    for (const auto &sampler : program.bindings.samplers) {
        Json item = {{"name", sampler.name},
                     {"slot", sampler.sokol_slot},
                     {"stage", shader_stage(sampler.stage)},
                     {"sampler_type", sampler_type(sampler.type)}};
        native_indices(item, sampler);
        result["samplers"].push_back(std::move(item));
    }
    for (const auto &buffer : program.bindings.storage_buffers) {
        Json item = {{"name", buffer.name},
                     {"slot", buffer.sokol_slot},
                     {"stage", shader_stage(buffer.stage)},
                     {"readonly", buffer.readonly}};
        native_indices(item, buffer);
        result["storage_buffers"].push_back(std::move(item));
    }
    for (const auto &image : program.bindings.storage_images) {
        Json item = {{"name", image.name},
                     {"slot", image.sokol_slot},
                     {"stage", shader_stage(image.stage)},
                     {"image_type", image_type(image.type)},
                     {"access_format", storage_pixel_format(image.access_format)},
                     {"writeonly", image.writeonly}};
        native_indices(item, image);
        result["storage_images"].push_back(std::move(item));
    }
    for (const auto &pair : program.bindings.texture_samplers) {
        const auto *texture = program.bindings.find_texture_by_name(pair.texture_name);
        const auto *sampler = program.bindings.find_sampler_by_name(pair.sampler_name);
        if (!texture || !sampler)
            throw std::runtime_error("Texture/sampler pair has no resource");
        result["texture_sampler_pairs"].push_back({{"name", pair.name},
                                                   {"slot", pair.sokol_slot},
                                                   {"stage", shader_stage(pair.stage)},
                                                   {"view_slot", texture->sokol_slot},
                                                   {"sampler_slot", sampler->sokol_slot}});
    }
    return result;
}

static uint32_t prepare_glsl(CompilerGLSL &compiler) {
    for (const auto &resource : compiler.get_shader_resources().uniform_buffers)
        if (Spirvcross::can_flatten_uniform_block(compiler, resource))
            compiler.flatten_buffer_block(resource.id);
    const uint32_t dummy_sampler = compiler.build_dummy_sampler_for_combined_images();
    compiler.build_combined_image_samplers();
    for (const auto &pair : compiler.get_combined_image_samplers())
        compiler.set_name(pair.combined_id, compiler.get_name(pair.image_id) + "_" +
                                                compiler.get_name(pair.sampler_id));
    return dummy_sampler;
}

static SpirvcrossSource translate(const Input &input, int index, std::vector<uint32_t> words,
                                  const std::map<std::string, Binding> &metadata,
                                  std::set<std::string> &used, Json &report, Slang::Enum target,
                                  Bytecode &bytecode,
                                  std::map<std::string, std::string> &uniform_paths) {
    const auto shader_stage = ShaderStage::from_snippet_type(input.snippets[index].type);
    const auto stage_name = ShaderStage::to_str(shader_stage);
    const bool compute = shader_stage == ShaderStage::Compute;
    if (compute &&
        (target == Slang::GLSL410 || target == Slang::GLSL300ES || target == Slang::HLSL4))
        throw std::runtime_error(
            std::string(Slang::to_str(target)) +
            ": compute requires glsl430, glsl310es, hlsl5, Metal, WGSL or Vulkan");
    CompilerGLSL analysis(words);
    name_resources(analysis, stage_name, metadata);
    const auto expected_stage = compute                               ? spv::ExecutionModelGLCompute
                                : shader_stage == ShaderStage::Vertex ? spv::ExecutionModelVertex
                                                                      : spv::ExecutionModelFragment;
    const auto entries = analysis.get_entry_points_and_stages();
    if (entries.size() != 1 || entries[0].execution_model != expected_stage)
        throw std::runtime_error("expected one entry point with the requested stage");
    if (compute) {
        if (!analysis.get_execution_mode_bitset().get(spv::ExecutionModeLocalSize))
            throw std::runtime_error("compute requires a fixed workgroup size");
        uint64_t threads = 1;
        for (uint32_t axis = 0; axis < 3; ++axis) {
            const auto size =
                analysis.get_execution_mode_argument(spv::ExecutionModeLocalSize, axis);
            if (size == 0)
                throw std::runtime_error("compute workgroup dimensions must be positive");
            threads *= size;
            if (threads > std::numeric_limits<int>::max())
                throw std::runtime_error(
                    "compute workgroup size exceeds Sokol's signed integer range");
        }
    }
    auto resources = analysis.get_shader_resources();
    if (!resources.push_constant_buffers.empty() || !resources.sampled_images.empty() ||
        !resources.subpass_inputs.empty() || !resources.acceleration_structures.empty() ||
        !resources.atomic_counters.empty() || !resources.gl_plain_uniforms.empty() ||
        !resources.shader_record_buffers.empty())
        throw std::runtime_error("supported resources are flat uniforms, storage buffers/images, "
                                 "separate textures and samplers");
    auto check_locations = [&](const auto &list, uint32_t limit) {
        std::set<uint32_t> locations;
        for (const auto &resource : list) {
            auto location = analysis.get_decoration(resource.id, spv::DecorationLocation);
            const auto &type = analysis.get_type(resource.type_id);
            if (!analysis.has_decoration(resource.id, spv::DecorationLocation) ||
                location >= limit || !locations.insert(location).second || !type.array.empty() ||
                type.columns != 1)
                throw std::runtime_error("unsupported or out-of-range stage location: " +
                                         resource.name);
        }
    };
    check_locations(resources.stage_inputs, shader_stage == ShaderStage::Vertex
                                                ? SG_MAX_VERTEX_ATTRIBUTES
                                                : refl::StageAttr::Num);
    check_locations(resources.stage_outputs,
                    shader_stage == ShaderStage::Vertex ? refl::StageAttr::Num : 8);
    if (shader_stage == ShaderStage::Vertex) {
        // Sokol requires vertex attributes to start at zero without gaps.
        // Rewrite SPIR-V before translation so backend code and reflection use
        // the same slots. Stage varyings and fragment outputs keep their locations.
        auto attributes = resources.stage_inputs;
        std::sort(attributes.begin(), attributes.end(), [&](const auto &a, const auto &b) {
            return analysis.get_decoration(a.id, spv::DecorationLocation) <
                   analysis.get_decoration(b.id, spv::DecorationLocation);
        });
        for (uint32_t slot = 0; slot < attributes.size(); ++slot) {
            uint32_t offset;
            if (!analysis.get_binary_offset_for_decoration(attributes[slot].id,
                                                           spv::DecorationLocation, offset))
                throw std::runtime_error("SPIR-V vertex attribute has no location decoration");
            words.at(offset) = slot;
        }
    }
    for (const auto &resource : resources.storage_buffers) {
        const auto &block = analysis.get_type(resource.base_type_id);
        if (block.member_types.size() != 1)
            throw std::runtime_error("storage buffer must contain one array of records");
        const auto &record = analysis.get_type(block.member_types[0]);
        if (record.basetype != SPIRType::Struct || record.array.size() != 1 || record.array[0] != 0)
            throw std::runtime_error("storage buffer must contain one unsized array of records");
    }
    BindSlotMap slots;
    auto add = [&](const auto &list, const std::string &kind, BindSlot::Type type) {
        for (const auto &resource : list) {
            const auto found = metadata.find(resource.name);
            if (found == metadata.end() || found->second.kind != kind)
                throw std::runtime_error("missing or wrong metadata for resource: " +
                                         resource.name);
            if (!analysis.get_type(resource.type_id).array.empty())
                throw std::runtime_error("resource arrays are not supported: " + resource.name);
            int qualifier = 0;
            if (kind == "storage_buffer") {
                const bool readonly =
                    analysis.get_buffer_block_flags(resource.id).get(spv::DecorationNonWritable);
                if (readonly != (found->second.access == "read"))
                    throw std::runtime_error("storage buffer access does not match SPIR-V: " +
                                             resource.name);
                if (!readonly && shader_stage == ShaderStage::Vertex)
                    throw std::runtime_error(
                        "Sokol permits only read-only storage buffers in vertex shaders");
                qualifier = readonly ? BindSlot::ReadOnly : 0;
            } else if (kind == "storage_image") {
                if (!compute)
                    throw std::runtime_error(
                        "Sokol permits storage images only in compute shaders");
                const auto &image_type = analysis.get_type(resource.type_id);
                if (image_type.image.ms ||
                    (image_type.image.dim != spv::Dim2D && image_type.image.dim != spv::Dim3D) ||
                    (image_type.image.dim == spv::Dim3D && image_type.image.arrayed))
                    throw std::runtime_error(
                        "storage images support non-multisampled 2D, 2D array and 3D images");
                if (storage_image_format(image_type) != found->second.format)
                    throw std::runtime_error("storage image format does not match SPIR-V: " +
                                             resource.name);
                const auto access = storage_image_access(analysis, resource);
                if (access != found->second.access)
                    throw std::runtime_error("storage image access does not match SPIR-V: " +
                                             resource.name);
                qualifier = access == "write" ? BindSlot::WriteOnly : 0;
            }
            std::string error;
            if (!slots.add(BindSlot(found->second.slot, resource.name, type, qualifier), error))
                throw std::runtime_error(error);
            used.insert(resource.name);
        }
    };
    add(resources.uniform_buffers, "uniform", BindSlot::Type::UniformBlock);
    add(resources.storage_buffers, "storage_buffer", BindSlot::Type::StorageBuffer);
    add(resources.storage_images, "storage_image", BindSlot::Type::StorageImage);
    add(resources.separate_images, "texture", BindSlot::Type::Texture);
    add(resources.separate_samplers, "sampler", BindSlot::Type::Sampler);
    for (const auto &output : resources.stage_outputs)
        if (shader_stage == ShaderStage::Fragment &&
            analysis.has_decoration(output.id, spv::DecorationLocation))
            slots.track_fs_output_location(
                analysis.get_decoration(output.id, spv::DecorationLocation));
    if (resources.uniform_buffers.size() > 8 || resources.storage_buffers.size() > 8 ||
        resources.storage_images.size() > 8 || resources.separate_images.size() > 16 ||
        resources.separate_samplers.size() > 12)
        throw std::runtime_error("resource count exceeds the pinned Sokol stage limits");
    slots.allocate_backend_slots(shader_stage);
    auto remap = [&](Compiler &compiler) {
        auto assign = [&](const auto &list, int category) {
            for (const auto &resource : list) {
                int slot = category == 0 ? slots.find_uniformblock_slang_slot(resource.name, target)
                           : category == 1 ? slots.find_view_slang_slot(resource.name, target)
                                           : slots.find_sampler_slang_slot(resource.name, target);
                if (Slang::is_glsl(target) && category != 1)
                    continue;
                if (slot < 0)
                    continue;
                compiler.set_decoration(resource.id, spv::DecorationBinding, slot);
                compiler.set_decoration(
                    resource.id, spv::DecorationDescriptorSet,
                    (Slang::is_spirv(target) || Slang::is_wgsl(target)) && category != 0 ? 1 : 0);
            }
        };
        assign(resources.uniform_buffers, 0);
        assign(resources.storage_buffers, 1);
        assign(resources.storage_images, 1);
        if (!Slang::is_glsl(target))
            assign(resources.separate_images, 1);
        assign(resources.separate_samplers, 2);
    };
    auto patched_spirv = [&]() {
        auto patched = words;
        Compiler layout(words);
        name_resources(layout, stage_name, metadata);
        remap(layout);
        auto patch = [&](const auto &list) {
            for (const auto &resource : list) {
                for (auto decoration : {spv::DecorationBinding, spv::DecorationDescriptorSet}) {
                    uint32_t offset;
                    if (!layout.get_binary_offset_for_decoration(resource.id, decoration, offset))
                        throw std::runtime_error("SPIR-V resource has no binding/set decoration");
                    patched.at(offset) = layout.get_decoration(resource.id, decoration);
                }
            }
        };
        patch(resources.uniform_buffers);
        patch(resources.storage_buffers);
        patch(resources.storage_images);
        patch(resources.separate_images);
        patch(resources.separate_samplers);
        return patched;
    };
    if ((target == Slang::GLSL410 || target == Slang::GLSL300ES || target == Slang::HLSL4) &&
        !resources.storage_buffers.empty())
        throw std::runtime_error(std::string(Slang::to_str(target)) +
                                 " does not support this shader's storage buffers");
    SpirvcrossSource result;
    result.snippet_index = index;
    std::string native_entry = entries[0].name;
    if (Slang::is_msl(target)) {
        CompilerMSL compiler(words);
        name_resources(compiler, stage_name, metadata);
        remap(compiler);
        CompilerMSL::Options options;
        options.platform =
            target == Slang::METAL_MACOS ? CompilerMSL::Options::macOS : CompilerMSL::Options::iOS;
        options.set_msl_version(2, 1);
        // Sokol's Metal backend supports base-vertex and base-instance draws.
        options.ios_support_base_vertex_instance = true;
        options.enable_decoration_binding = true;
        compiler.set_msl_options(options);
        result.source_code = compiler.compile();
        native_entry = compiler.get_cleansed_entry_point_name(entries[0].name, expected_stage);
    } else if (Slang::is_hlsl(target)) {
        CompilerHLSL compiler(words);
        name_resources(compiler, stage_name, metadata);
        remap(compiler);
        CompilerGLSL::Options common;
        common.vertex.support_nonzero_base_instance = false;
        compiler.set_common_options(common);
        CompilerHLSL::Options options;
        options.shader_model = target == Slang::HLSL4 ? 40 : 50;
        compiler.set_hlsl_options(options);
        result.source_code = compiler.compile();
    } else if (Slang::is_glsl(target)) {
        CompilerGLSL compiler(words);
        name_resources(compiler, stage_name, metadata);
        remap(compiler);
        CompilerGLSL::Options options;
        options.es = target == Slang::GLSL300ES || target == Slang::GLSL310ES;
        options.version = target == Slang::GLSL300ES   ? 300
                          : target == Slang::GLSL310ES ? 310
                          : target == Slang::GLSL410   ? 410
                                                       : 430;
        options.enable_420pack_extension = false;
        options.emit_uniform_buffer_as_plain_uniforms = true;
        options.vertex.support_nonzero_base_instance = false;
        // The manifest contract uses 0..1 clip depth. GL expects -1..1.
        options.vertex.fixup_clipspace = true;
        compiler.set_common_options(options);
        prepare_glsl(compiler);
        result.source_code = compiler.compile();
    } else if (target == Slang::WGSL) {
        auto patched = patched_spirv();
        tint::spirv::reader::Options options;
        options.allow_non_uniform_derivatives = true;
        options.allowed_features.features = {
            tint::wgsl::LanguageFeature::kReadonlyAndReadwriteStorageTextures};
        auto program = tint::spirv::reader::Read(patched, options);
        if (program.Diagnostics().ContainsErrors())
            throw std::runtime_error(program.Diagnostics().Str());
        auto output = tint::wgsl::writer::Generate(program, {});
        if (output != tint::Success)
            throw std::runtime_error(output.Failure().reason);
        result.source_code = output.Get().wgsl;
    } else if (target == Slang::SPIRV_VK) {
        auto patched = patched_spirv();
        validate_spirv(patched, SPV_ENV_VULKAN_1_1, "Vulkan SPIR-V validation: ");
        BytecodeBlob blob;
        blob.valid = true;
        blob.snippet_index = index;
        const auto *data = reinterpret_cast<const uint8_t *>(patched.data());
        blob.data.assign(data, data + patched.size() * sizeof(uint32_t));
        bytecode.blobs.push_back(std::move(blob));
    }

    // Sokol detects depth textures and comparison samplers after GLSL analysis.
    // This analysis does not feed backend shader translation.
    CompilerGLSL reflect(words);
    name_resources(reflect, stage_name, metadata);
    CompilerGLSL::Options gl;
    gl.version = 430;
    gl.emit_uniform_buffer_as_plain_uniforms = true;
    reflect.set_common_options(gl);
    const uint32_t dummy_sampler = prepare_glsl(reflect);
    reflect.compile();
    BindSlotMap reflection_slots = slots;
    if (dummy_sampler) {
        // SPIRV-Cross adds a sampler for samplerless image fetches. Sokol's
        // parser needs a slot for it, though the shader has no such binding.
        int slot = 0;
        while (slot < MaxSamplers && !reflection_slots.samplers[slot].empty())
            ++slot;
        if (slot == MaxSamplers)
            throw std::runtime_error(
                "samplerless image fetch needs a free reflection sampler slot");
        std::string error;
        if (!reflection_slots.add(
                BindSlot(slot, "SPIRV_Cross_DummySampler", BindSlot::Type::Sampler), error))
            throw std::runtime_error(error);
        reflection_slots.allocate_backend_slots(shader_stage);
    }
    auto reflection_input = input;
    reflection_input.image_sample_type_tags.clear();
    reflection_input.sampler_type_tags.clear();
    result.stage_refl = refl::Reflection::parse_snippet_reflection(
        reflect, input.snippets[index], reflection_input, reflection_slots, result.error);
    check(result.error);
    if (dummy_sampler) {
        auto &samplers = result.stage_refl.bindings.samplers;
        samplers.erase(std::remove_if(samplers.begin(), samplers.end(),
                                      [](const auto &sampler) {
                                          return sampler.name == "SPIRV_Cross_DummySampler";
                                      }),
                       samplers.end());
        auto &pairs = result.stage_refl.bindings.texture_samplers;
        pairs.erase(std::remove_if(pairs.begin(), pairs.end(),
                                   [](const auto &pair) {
                                       return pair.sampler_name == "SPIRV_Cross_DummySampler";
                                   }),
                    pairs.end());
    }
    // Filterability is application metadata; scalar and comparison types must
    // agree with the shader code emitted above.
    for (auto &texture : result.stage_refl.bindings.textures) {
        const auto *tag = input.find_image_sample_type_tag(texture.name);
        using T = refl::ImageSampleType;
        if (tag) {
            if (tag->type != texture.sample_type &&
                !(texture.sample_type == T::FLOAT && tag->type == T::UNFILTERABLE_FLOAT))
                throw std::runtime_error(texture.name +
                                         ": sample_type does not match SPIR-V texture type");
            texture.sample_type = tag->type;
        }
    }
    for (auto &sampler : result.stage_refl.bindings.samplers) {
        const auto *tag = input.find_sampler_type_tag(sampler.name);
        using T = refl::SamplerType;
        if (tag) {
            if ((sampler.type == T::COMPARISON) != (tag->type == T::COMPARISON))
                throw std::runtime_error(sampler.name +
                                         ": sampler_type does not match SPIR-V comparison use");
            sampler.type = tag->type;
        }
    }
    for (const auto &pair : result.stage_refl.bindings.texture_samplers) {
        const auto *texture = result.stage_refl.bindings.find_texture_by_name(pair.texture_name);
        const auto *sampler = result.stage_refl.bindings.find_sampler_by_name(pair.sampler_name);
        if (!texture || !sampler)
            throw std::runtime_error(pair.name +
                                     ": texture/sampler pair has no reflected resource");
        using T = refl::ImageSampleType;
        if ((texture->sample_type == T::UINT || texture->sample_type == T::SINT ||
             texture->sample_type == T::UNFILTERABLE_FLOAT) &&
            sampler->type != refl::SamplerType::NONFILTERING)
            throw std::runtime_error(
                texture->name +
                ": integer and unfilterable textures require a nonfiltering sampler (" +
                sampler->name + ")");
    }
    // Flatten one-member array wrappers for host packing when their layouts match.
    for (auto &block : result.stage_refl.bindings.uniform_blocks) {
        for (auto &member : block.struct_info.struct_items) {
            if (member.type == refl::Type::Struct && member.struct_items.size() == 1) {
                auto array = member.struct_items[0];
                if (!array.is_array || array.offset != 0 || array.size != member.size)
                    throw std::runtime_error("unsupported array wrapper layout");
                uniform_paths[block.inst_name + "." + member.name] =
                    block.inst_name + "." + member.name + "." + array.name;
                array.name = member.name;
                array.offset = member.offset;
                member = array;
            }
            if (member.type == refl::Type::Struct)
                throw std::runtime_error("nested uniform structs are not supported yet");
            using T = refl::Type;
            const bool scalar_vector = (member.type >= T::Int && member.type <= T::Int4) ||
                                       (member.type >= T::Float && member.type <= T::Float4);
            if (!scalar_vector && member.type != T::Mat4x4)
                throw std::runtime_error("uniforms support int/float vectors and float4x4 only");
            if (member.is_array && member.type != T::Float4 && member.type != T::Int4 &&
                member.type != T::Mat4x4)
                throw std::runtime_error("uniform arrays support float4, int4 and float4x4 only");
            if (member.is_array && member.array_stride != (member.type == T::Mat4x4 ? 64 : 16))
                throw std::runtime_error("unsupported uniform array stride");
            if (member.is_matrix && member.matrix_stride != 16)
                throw std::runtime_error("unsupported uniform matrix stride");
        }
    }
    // Verify the generated descriptor uses the emitted backend entry name.
    result.stage_refl.entry_point = entries[0].name;
    if (result.stage_refl.entry_point_by_slang(target) != native_entry)
        throw std::runtime_error("backend entry name does not match Sokol's naming rule");
    result.valid = true;
    CompilerReflection raw(words);
    name_resources(raw, stage_name, metadata);
    report = {{"spirv", Json::parse(raw.compile())},
              {"entry", native_entry},
              {"bindings", Json::array()}};
    auto describe = [&](const auto &list, const char *kind, int category) {
        for (const auto &resource : list) {
            const auto &binding = metadata.at(resource.name);
            int native = category == 0   ? slots.find_uniformblock_slang_slot(resource.name, target)
                         : category == 1 ? slots.find_view_slang_slot(resource.name, target)
                                         : slots.find_sampler_slang_slot(resource.name, target);
            Json item = {{"name", resource.name},
                         {"kind", kind},
                         {"slot", binding.slot},
                         {"binding", native}};
            if (binding.kind == "storage_buffer" || binding.kind == "storage_image")
                item["access"] = binding.access;
            if (binding.kind == "storage_image")
                item["format"] = binding.format;
            report["bindings"].push_back(std::move(item));
        }
    };
    describe(resources.uniform_buffers, "uniform", 0);
    describe(resources.storage_buffers, "storage_buffer", 1);
    describe(resources.storage_images, "storage_image", 1);
    describe(resources.separate_images, "texture", 1);
    describe(resources.separate_samplers, "sampler", 2);
    if (compute)
        report["workgroup_size"] = result.stage_refl.cs_workgroup_size;
    return result;
}

// Reads texture and sampler use from SPIR-V so frontend callers need only
// supply policies, such as float filterability, that shader code cannot state.
class ResourceInspector : public Compiler {
  public:
    using Compiler::Compiler;
    void inspect_sampling() { analyze_image_and_sampler_usage(); }
    bool uses_comparison(uint32_t id) const { return comparison_ids.count(id) != 0; }
};

static Json inspect_words(const std::vector<uint32_t> &words) {
    ResourceInspector compiler(words);
    compiler.inspect_sampling();
    Json output = {{"entries", Json::array()}, {"resources", Json::array()}};
    for (const auto &entry : compiler.get_entry_points_and_stages())
        output["entries"].push_back(
            {{"name", entry.name},
             {"stage", entry.execution_model == spv::ExecutionModelVertex      ? "vertex"
                       : entry.execution_model == spv::ExecutionModelFragment  ? "fragment"
                       : entry.execution_model == spv::ExecutionModelGLCompute ? "compute"
                                                                               : "unsupported"}});
    auto resources = compiler.get_shader_resources();
    auto append = [&](const auto &list, const char *kind) {
        for (const auto &resource : list) {
            Json binding = {
                {"name", compiler.get_name(resource.id)},
                {"set", compiler.get_decoration(resource.id, spv::DecorationDescriptorSet)},
                {"binding", compiler.get_decoration(resource.id, spv::DecorationBinding)},
                {"kind", kind}};
            if (std::string(kind) == "texture") {
                const auto &image = compiler.get_type(resource.type_id).image;
                const auto scalar = compiler.get_type(image.type).basetype;
                binding["sample_type"] = image.depth || compiler.uses_comparison(resource.id)
                                             ? "depth"
                                         : scalar == SPIRType::Int  ? "sint"
                                         : scalar == SPIRType::UInt ? "uint"
                                                                    : "float";
            }
            if (std::string(kind) == "sampler")
                binding["sampler_type"] =
                    compiler.uses_comparison(resource.id) ? "comparison" : "filtering";
            if (std::string(kind) == "storage_buffer")
                binding["access"] =
                    compiler.get_buffer_block_flags(resource.id).get(spv::DecorationNonWritable)
                        ? "read"
                        : "read_write";
            if (std::string(kind) == "storage_image") {
                binding["access"] = storage_image_access(compiler, resource);
                binding["format"] = storage_image_format(compiler.get_type(resource.type_id));
            }
            output["resources"].push_back(binding);
        }
    };
    append(resources.uniform_buffers, "uniform");
    append(resources.storage_buffers, "storage_buffer");
    append(resources.storage_images, "storage_image");
    append(resources.separate_images, "texture");
    append(resources.separate_samplers, "sampler");
    return output;
}

Json inspect_spirv(const std::string &path) { return inspect_words(read_spirv(path.c_str())); }

GeneratedProgram generate_program(const Manifest &manifest,
                                  const std::map<std::string, std::string> &options,
                                  const std::filesystem::path &native_work,
                                  const std::string &output_name, bool batch,
                                  const std::map<std::string, std::vector<uint32_t>> *captured,
                                  sokol_slang::ProgramArtifact *artifact) {
    SokolGenerator generator;
    Input input;
    input.base_path = manifest.path.string();
    const std::string module = manifest.document["module"];
    const std::string program_name = manifest.document["program"];
    input.module = batch ? module + "_" + program_name : module;
    input.filenames.push_back(input.base_path);
    input.lines.emplace_back("manifest", 0, 0);
    const bool compute = manifest.document["stages"].contains("compute");
    const std::vector<std::string> stages = compute
                                                ? std::vector<std::string>{"compute"}
                                                : std::vector<std::string>{"vertex", "fragment"};
    for (int i = 0; i < int(stages.size()); ++i) {
        auto name = compute ? "cs" : i == 0 ? "vs" : "fs";
        Snippet snippet(compute ? Snippet::CS : i == 0 ? Snippet::VS : Snippet::FS, name);
        snippet.index = i;
        snippet.lines.push_back(0);
        input.snippets.push_back(snippet);
        input.snippet_map[name] = i;
        if (compute)
            input.cs_map[name] = i;
        else if (i == 0)
            input.vs_map[name] = i;
        else
            input.fs_map[name] = i;
    }
    input.programs[program_name] = compute ? Program::from_cs(program_name, "cs", 0)
                                           : Program::from_vs_fs(program_name, "vs", "fs", 0);
    for (const auto &resource : manifest.document["resources"]) {
        const std::string name = resource["name"];
        if (resource["kind"] == "texture")
            input.image_sample_type_tags[name] = ImageSampleTypeTag(
                name, refl::ImageSampleType::from_str(resource["sample_type"]), 0);
        if (resource["kind"] == "sampler")
            input.sampler_type_tags[name] =
                SamplerTypeTag(name, refl::SamplerType::from_str(resource["sampler_type"]), 0);
    }
    Args args;
    args.input = input.base_path;
    args.output = output_name;
    std::vector<Slang::Enum> targets;
    const bool modern_gl =
        compute ||
        std::any_of(manifest.bindings.begin(), manifest.bindings.end(), [](const auto &item) {
            return item.second.kind == "storage_buffer" || item.second.kind == "storage_image";
        });
    const auto target_list =
        options.count("--targets") ? options.at("--targets")
        : modern_gl ? "glsl430:glsl310es:hlsl5:metal_macos:metal_ios:metal_sim:wgsl:spirv_vk"
                    : "glsl410:glsl300es:hlsl5:metal_macos:metal_ios:metal_sim:wgsl:spirv_vk";
    if (target_list.empty() || target_list.back() == ':')
        throw std::runtime_error("empty target name");
    std::stringstream names(target_list);
    std::string target_name;
    std::set<std::string> backends;
    while (std::getline(names, target_name, ':')) {
        bool found = false;
        for (int i = 0; i < Slang::REFLECTION; ++i) {
            auto target = static_cast<Slang::Enum>(i);
            if (target_name == Slang::to_str(target)) {
                std::string backend = target_name;
                if (target == Slang::GLSL410 || target == Slang::GLSL430)
                    backend = "glcore";
                if (target == Slang::GLSL300ES || target == Slang::GLSL310ES)
                    backend = "gles";
                if (Slang::is_hlsl(target))
                    backend = "d3d11";
                if (!backends.insert(backend).second)
                    throw std::runtime_error("choose one target version per Sokol backend");
                args.slang |= Slang::bit(target);
                targets.push_back(target);
                found = true;
            }
        }
        if (!found)
            throw std::runtime_error("unknown target: " + target_name);
    }
    if (targets.empty())
        throw std::runtime_error("no targets requested");
    args.reflection = true;
    args.no_log_cmdline = true;
    std::array<Spirvcross, Slang::Num> translated;
    std::array<Bytecode, Slang::Num> bytecode;
    std::set<std::string> used;
    Json report = {{"version", 1}, {"targets", Json::object()}};
    std::vector<std::vector<uint32_t>> binaries;
    for (const auto &stage : stages)
        binaries.push_back(
            normalize_entry(captured ? captured->at(stage)
                                     : read_spirv(manifest.stage_path(stage).string().c_str()),
                            manifest.document["stages"][stage]["entry"]));
    for (auto target : targets) {
        auto &sources = translated[target].sources;
        for (int i = 0; i < int(stages.size()); ++i) {
            const auto &stage = stages[i];
            Json stage_json;
            sources.push_back(translate(input, i, binaries[i], manifest.bindings, used, stage_json,
                                        target, bytecode[target], generator.uniform_paths));
            report["targets"][Slang::to_str(target)][stage] = std::move(stage_json);
        }
        // Upstream Sokol checks location names. Also check types here.
        if (!compute)
            for (size_t i = 0; i < sources[1].stage_refl.inputs.size(); ++i) {
                const auto &in = sources[1].stage_refl.inputs[i];
                auto &out = sources[0].stage_refl.outputs[i];
                if (in.slot >= 0 && (out.slot < 0 || in.type_info.type != out.type_info.type))
                    throw std::runtime_error("vertex/fragment varying type mismatch at location " +
                                             std::to_string(i));
                // Independently compiled stages may retain vertex outputs that
                // the fragment shader does not consume. Sokol's linker expects
                // identical reflection arrays, so compare only consumed outputs.
                // Keep the actual backend interfaces and raw reflection intact.
                if (in.slot < 0)
                    out = refl::StageAttr{};
            }
        if (!options.count("--source"))
            compile_native_bytecode(bytecode[target], translated[target], target, native_work);
    }
    for (const auto &[resource, binding] : manifest.bindings)
        if (!used.count(resource))
            throw std::runtime_error("metadata resource is unused: " + resource);
    auto reflection = refl::Reflection::build(args, input, translated);
    check(reflection.error);
    if (artifact) {
        artifact->reflection = report;
        if (reflection.progs.size() != 1)
            throw std::runtime_error("Expected one reflected program");
        artifact->layout = generator.describe_interface(reflection.progs[0]);
        for (auto target : targets) {
            for (size_t i = 0; i < stages.size(); ++i) {
                const auto &source = translated[target].sources[i];
                sokol_slang::BackendStage stage;
                stage.target = Slang::to_str(target);
                stage.stage = stages[i];
                stage.entry = report["targets"][stage.target][stage.stage]["entry"];
                stage.profile = generator.native_profile(
                    target, ShaderStage::from_snippet_type(input.snippets[i].type));
                stage.source = source.source_code;
                if (const auto *blob = bytecode[target].find_blob_by_snippet_index(i))
                    stage.bytecode.assign(blob->data.begin(), blob->data.end());
                artifact->stages.push_back(std::move(stage));
            }
        }
        return {{}, std::move(report)};
    }
    if (reflection.progs.size() != 1)
        throw std::runtime_error("Expected one reflected program");
    report["layout"] = generator.describe_interface(reflection.progs[0]);
    gen::GenInput generation(args, input, translated, bytecode, reflection);
    if (batch) {
        generator.internal_program_prefix = input.module + "_" + program_name + "_";
        generator.public_program_prefix = module + "_" + program_name + "_";
    }
    check(generator.generate(generation));
    return {generator.take_header(), std::move(report)};
}

namespace sokol_slang {
Json ProgramArtifact::to_json() const {
    Json result = {{"version", format_version},
                   {"layout", layout},
                   {"reflection", reflection},
                   {"stages", Json::array()}};
    for (const auto &stage : stages)
        result["stages"].push_back({{"target", stage.target},
                                    {"stage", stage.stage},
                                    {"entry", stage.entry},
                                    {"profile", stage.profile},
                                    {"source", stage.source},
                                    {"bytecode", stage.bytecode}});
    return result;
}

ProgramArtifact compile_backend(const BackendRequest &request) {
    if (request.stages.empty())
        throw std::runtime_error("No SPIR-V stages supplied");
    if (request.targets.empty())
        throw std::runtime_error("Supply explicit backend targets");
    Json document = request.manifest;
    if (document.contains("programs"))
        throw std::runtime_error("compile_backend accepts one program at a time");
    document["stages"] = Json::object();
    std::map<std::string, Json> inspections;
    for (const auto &[stage, words] : request.stages) {
        if (words.size() < 5 || words[0] != 0x07230203)
            throw std::runtime_error("Invalid SPIR-V header");
        validate_spirv(words, SPV_ENV_UNIVERSAL_1_3, "invalid SPIR-V: ");
        auto info = inspect_words(words);
        if (info["entries"].size() != 1 || info["entries"][0]["stage"] != stage)
            throw std::runtime_error("Expected one matching SPIR-V stage entry");
        const std::string file = stage + ".spv";
        document["stages"][stage] = {{"file", file}, {"entry", info["entries"][0]["name"]}};
        inspections.emplace(file, std::move(info));
    }
    const std::filesystem::path path("program.json");
    Manifest::validate_document(document);
    std::set<std::pair<std::string, std::string>> used;
    const auto resources = resolve_program_bindings(
        document, path, [&](const std::string &file) { return inspections.at(file); }, used);
    for (const auto &binding : document.value("bindings", Json::array())) {
        const auto key = std::make_pair(binding.at("stage").get<std::string>(),
                                        binding.at("name").get<std::string>());
        if (!used.count(key))
            throw std::runtime_error("binding override names no reflected resource: " + key.first +
                                     ":" + key.second);
    }
    auto manifest = Manifest::from_document(document, path);
    std::map<std::string, std::string> options{{"--source", ""}};
    if (!request.targets.empty())
        options["--targets"] = request.targets;
    ProgramArtifact result;
    generate_program(manifest, options, {}, "", false, &request.stages, &result);
    if (!resources.is_null())
        result.reflection["resources"] = resources;
    return result;
}
} // namespace sokol_slang
