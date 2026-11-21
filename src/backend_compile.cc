#include "generators/sokolc.h"
#include "internal/backend_cli.h"
#include "internal/bind_limits.h"
#include "internal/compiler_stack.h"
#include "internal/native_bytecode.h"
#include "internal/position_invariance.h"
#include "internal/program_bindings.h"
#include "internal/storage_elements.h"
#include "manifest.h"
#include "sokol_gfx.h"
#include "spirv-tools/libspirv.hpp"
#include "spirv_hlsl.hpp"
#include "spirv_msl.hpp"
#include "spirv_reflect.hpp"
#include "tint/tint.h"
#include <algorithm>
#include <atomic>
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
static_assert(MaxUniformBlocks == kMaxUniformBindslots,
              "sokol-shdc uniform block slot limit differs from sokol_gfx.h "
              "SG_MAX_UNIFORMBLOCK_BINDSLOTS");
static_assert(MaxViews == kMaxViewBindslots,
              "sokol-shdc view slot limit differs from sokol_gfx.h SG_MAX_VIEW_BINDSLOTS");
static_assert(MaxSamplers == kMaxSamplerBindslots,
              "sokol-shdc sampler slot limit differs from sokol_gfx.h SG_MAX_SAMPLER_BINDSLOTS");

// Compiler keeps its parsed module protected; this names it for read-only scans.
struct ParsedModule : Compiler {
    using Compiler::ir;
};

static bool is_runtime_array(const SPIRType &type) {
    return type.array.size() == 1 && type.array[0] == 0 && type.array_size_literal[0];
}

// Sokol's parser only knows the shared view namespace. After GLSL analysis,
// leave fragment storage images out of its variable list and reflect them from
// the validated module instead. Never emit shader code from this filtered copy.
struct ReflectionCompiler : CompilerGLSL {
    using CompilerGLSL::CompilerGLSL;
    // Inspect the selected entry and its calls, not unused functions or declared
    // capabilities that another program in the source module may need.
    bool uses_uint64_atomic_minmax() const {
        struct Handler final : OpcodeHandler {
            const Compiler &compiler;
            bool found = false;
            explicit Handler(const Compiler &compiler) : compiler(compiler) {}
            bool handle(spv::Op opcode, const uint32_t *args, uint32_t) override {
                if (opcode == spv::OpAtomicUMax || opcode == spv::OpAtomicUMin) {
                    const auto &type = compiler.get_type(args[0]);
                    found = type.basetype == SPIRType::UInt64 && type.width == 64;
                }
                return !found;
            }
        } handler(*this);
        traverse_all_reachable_opcodes(get<SPIRFunction>(ir.default_entry_point), handler);
        return handler.found;
    }
    void omit_resource(uint32_t id) {
        auto &variables = ir.ids_for_type[TypeVariable];
        variables.erase(std::remove(variables.begin(), variables.end(), ID(id)), variables.end());
    }
};

static refl::StoragePixelFormat::Enum reflection_storage_format(const SPIRType &type) {
    using Format = refl::StoragePixelFormat;
    switch (type.image.format) {
    case spv::ImageFormatRgba8:
        return Format::RGBA8;
    case spv::ImageFormatRgba8Snorm:
        return Format::RGBA8SN;
    case spv::ImageFormatRgba8ui:
        return Format::RGBA8UI;
    case spv::ImageFormatRgba8i:
        return Format::RGBA8SI;
    case spv::ImageFormatRgba16ui:
        return Format::RGBA16UI;
    case spv::ImageFormatRgba16i:
        return Format::RGBA16SI;
    case spv::ImageFormatRgba16f:
        return Format::RGBA16F;
    case spv::ImageFormatR16ui:
        return Format::R16UI;
    case spv::ImageFormatR11fG11fB10f:
        return Format::RG11B10F;
    case spv::ImageFormatR32ui:
        return Format::R32UI;
    case spv::ImageFormatR32i:
        return Format::R32SI;
    case spv::ImageFormatR32f:
        return Format::R32F;
    case spv::ImageFormatRg32ui:
        return Format::RG32UI;
    case spv::ImageFormatRg32i:
        return Format::RG32SI;
    case spv::ImageFormatRg32f:
        return Format::RG32F;
    case spv::ImageFormatRgba32ui:
        return Format::RGBA32UI;
    case spv::ImageFormatRgba32i:
        return Format::RGBA32SI;
    case spv::ImageFormatRgba32f:
        return Format::RGBA32F;
    default:
        throw std::runtime_error("unsupported fragment storage image format");
    }
}

// Base type shared by every scalar inside a type, or Unknown when they differ.
static SPIRType::BaseType leaf_basetype(const Compiler &compiler, TypeID id) {
    const auto &type = compiler.get_type(id);
    if (type.basetype != SPIRType::Struct)
        return type.basetype;
    auto basic = SPIRType::Unknown;
    for (auto member : type.member_types) {
        const auto leaf = leaf_basetype(compiler, member);
        if (leaf == SPIRType::Unknown || (basic != SPIRType::Unknown && leaf != basic))
            return SPIRType::Unknown;
        basic = leaf;
    }
    return basic;
}

static bool holds_array(const Compiler &compiler, TypeID id) {
    const auto &type = compiler.get_type(id);
    if (!type.array.empty())
        return true;
    if (type.basetype == SPIRType::Struct)
        for (auto member : type.member_types)
            if (holds_array(compiler, member))
                return true;
    return false;
}

// SPIRV-Cross turns each member access of a flattened block into vec4 reads, but
// throws "Access chains that result in an array can not be flattened" when code
// forms a pointer to an array, or to a struct holding one, such as an array passed
// to a function. Such blocks stay plain uniforms. Pointers into the block that
// leave through copies or calls count as whole loads.
static bool loads_whole_array(const Compiler &compiler, uint32_t block_variable) {
    const auto &words = (compiler.*(&ParsedModule::ir)).spirv;
    std::set<uint32_t> pointers = {block_variable};
    for (size_t pos = 5; pos < words.size();) {
        const uint32_t count = words[pos] >> 16, opcode = words[pos] & 0xffff;
        if (count == 0)
            break;
        const uint32_t *op = &words[pos];
        switch (opcode) {
        case spv::OpAccessChain:
        case spv::OpInBoundsAccessChain:
        case spv::OpPtrAccessChain:
        case spv::OpCopyObject:
            if (pointers.count(op[3])) {
                // SPIRV-Cross also emits each intermediate chain, so a pointer to an
                // array fails even when a later chain indexes it.
                if (opcode != spv::OpCopyObject && holds_array(compiler, op[1]))
                    return true;
                pointers.insert(op[2]);
            }
            break;
        case spv::OpLoad:
            if (pointers.count(op[3]) && holds_array(compiler, op[1]))
                return true;
            break;
        case spv::OpCopyMemory:
        case spv::OpCopyMemorySized:
            if (pointers.count(op[2]))
                return true;
            break;
        case spv::OpFunctionCall:
            for (uint32_t i = 4; i < count; ++i)
                if (pointers.count(op[i]))
                    return true;
            break;
        default:
            break;
        }
        pos += count;
    }
    return false;
}

// Reuse Sokol's reflection and C generator without its shader frontend.
namespace shdc {
// Sokol's GL backend uploads a flattened block as one vec4 or ivec4 array, so every
// scalar in it, struct array elements included, must share float or int.
bool Spirvcross::can_flatten_uniform_block(const Compiler &compiler, const Resource &resource) {
    const auto basic = leaf_basetype(compiler, resource.base_type_id);
    return (basic == SPIRType::Float || basic == SPIRType::Int) &&
           !loads_whole_array(compiler, resource.id);
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
    case spv::ImageFormatR16ui:
        return "r16ui";
    case spv::ImageFormatR11fG11fB10f:
        return "rg11b10f";
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

// Sokol has no bind slot for acceleration structures. A manifest slot (0..3)
// maps to a native index placed clear of every other resource. Metal buffers
// 0..7 hold uniform blocks and storage buffers follow from 8 in declaration
// order (sokol-shdc's BindSlotMap::allocate_backend_slots), so these start after
// the program's last compute storage buffer, and never below 16; they must end
// before the bindless argument buffers at MslBindlessBufferBase. Vulkan set 1
// uses one binding per view slot, then one per sampler slot, so the base is the
// view count plus the sampler count of the sokol_gfx.h used here.
// The layout reports the final numbers per slot; runtimes must use those.
// Ray query is available only in compute shaders on Metal and Vulkan.
static constexpr int MslStorageBufferBase = 8;
static constexpr int MslAccelerationBufferBase = 16;
static constexpr int MslBindlessBufferBase = 23;
static constexpr int SpirvAccelerationBindingBase = kMaxViewBindslots + kMaxSamplerBindslots;

static bool supports_ray_query(Slang::Enum target) {
    return target == Slang::METAL_MACOS || target == Slang::METAL_IOS || target == Slang::SPIRV_VK;
}

// The Metal buffer index of acceleration structure slot 0 for a program's bindings.
static int msl_acceleration_base(const std::map<std::string, Binding> &bindings) {
    int storage_buffers = 0;
    for (const auto &[name, binding] : bindings)
        if (binding.kind == "storage_buffer" && binding.stage == "compute")
            ++storage_buffers;
    return std::max(MslAccelerationBufferBase, MslStorageBufferBase + storage_buffers);
}

// Returns -1 for targets that cannot bind an acceleration structure.
static int acceleration_native_index(Slang::Enum target, int slot,
                                     const std::map<std::string, Binding> &bindings) {
    if (Slang::is_msl(target)) {
        const int index = msl_acceleration_base(bindings) + slot;
        if (index >= MslBindlessBufferBase)
            throw std::runtime_error("acceleration structure slot " + std::to_string(slot) +
                                     " needs Metal buffer " + std::to_string(index) +
                                     ", past the compute storage buffers and into the "
                                     "bindless argument buffers at " +
                                     std::to_string(MslBindlessBufferBase) +
                                     "; use fewer storage buffers");
        return index;
    }
    if (target == Slang::SPIRV_VK)
        return SpirvAccelerationBindingBase + slot;
    return -1;
}

// Sokol allows storage images only in compute shaders. A fragment storage image
// has a manifest slot (0..3) of its own and a native index clear of every Sokol
// view: Metal fragment textures use indices below SG_MAX_VIEW_BINDSLOTS, so
// these start there (Apple GPUs bind 128 textures per stage); Vulkan set 1
// places them after the acceleration structure bindings. sokol-utils binds
// them (sg_make_shader_fragment_storage, sg_fragment_storage_bind) from the
// layout's fragment_storage_images list.
static constexpr int MslFragmentStorageTextureBase = kMaxViewBindslots;
static constexpr int SpirvFragmentStorageBindingBase =
    SpirvAccelerationBindingBase + kMaxAccelerationSlots;

static bool supports_fragment_storage(Slang::Enum target) {
    return Slang::is_msl(target) || target == Slang::SPIRV_VK;
}

// Returns -1 for targets that cannot bind a fragment storage image.
static int fragment_storage_native_index(Slang::Enum target, int slot) {
    if (Slang::is_msl(target))
        return MslFragmentStorageTextureBase + slot;
    if (target == Slang::SPIRV_VK)
        return SpirvFragmentStorageBindingBase + slot;
    return -1;
}

// Bindless texture and sampler tables use SPIR-V runtime-sized arrays
// (OpTypeRuntimeArray) in a program-wide slot namespace (0..3):
// - Metal fragment and compute buffers 0..7 hold uniform blocks and 8..22 hold
//   storage buffers (compute acceleration structures use 16..19), so bindless
//   argument buffers use 23..26.
// - Metal vertex buffers 23..30 hold vertex attributes, so vertex bindless
//   argument buffers use 19..22 and cap vertex storage buffers at 11 (8..18).
// - Vulkan places slot s in descriptor set (2 + s), binding 0, clear of set 0
//   (uniform blocks) and set 1 (views, samplers, accel, fragment storage).
static constexpr int MslVertexBindlessBufferBase = 19;
static constexpr int SpirvBindlessSetBase = 2;

static bool supports_bindless(Slang::Enum target) {
    return Slang::is_msl(target) || target == Slang::SPIRV_VK;
}

static int bindless_msl_buffer_index(ShaderStage::Enum stage, int slot) {
    return (stage == ShaderStage::Vertex ? MslVertexBindlessBufferBase : MslBindlessBufferBase) +
           slot;
}

static int bindless_spirv_set_index(int slot) { return SpirvBindlessSetBase + slot; }

struct ReflectedBindlessTexture {
    std::string name;
    ShaderStage::Enum stage;
    int slot;
    refl::ImageType::Enum type;
    refl::ImageSampleType::Enum sample_type;
    bool multisampled;
};

struct ReflectedBindlessSampler {
    std::string name;
    ShaderStage::Enum stage;
    int slot;
    refl::SamplerType::Enum type;
};

// Slang lowers RasterizerOrdered* resources to a fragment shader interlock.
// SPIRV-Cross turns the interlocked resources into [[raster_order_group(0)]]
// for Metal; Vulkan needs VK_EXT_fragment_shader_interlock at run time.
static bool uses_interlock(const Compiler &compiler) {
    for (auto capability : compiler.get_declared_capabilities())
        if (capability == spv::CapabilityFragmentShaderPixelInterlockEXT ||
            capability == spv::CapabilityFragmentShaderSampleInterlockEXT ||
            capability == spv::CapabilityFragmentShaderShadingRateInterlockEXT)
            return true;
    return false;
}

static bool uses_vulkan_glsl_capabilities(const Compiler &compiler) {
    for (auto capability : compiler.get_declared_capabilities())
        if (capability == spv::CapabilityShaderNonUniform ||
            capability == spv::CapabilityRuntimeDescriptorArray)
            return true;
    return false;
}

// Ray query needs SPIR-V 1.4 or later. Validate against the version the module
// declares instead of a fixed environment.
static spv_target_env spirv_environment(const std::vector<uint32_t> &words, bool vulkan) {
    const uint32_t version = words.size() > 1 ? words[1] : 0;
    if (version >= 0x10600)
        return vulkan ? SPV_ENV_VULKAN_1_3 : SPV_ENV_UNIVERSAL_1_6;
    if (version >= 0x10500)
        return vulkan ? SPV_ENV_VULKAN_1_2 : SPV_ENV_UNIVERSAL_1_5;
    if (version >= 0x10400)
        return vulkan ? SPV_ENV_VULKAN_1_2 : SPV_ENV_UNIVERSAL_1_4;
    return vulkan ? SPV_ENV_VULKAN_1_1 : SPV_ENV_UNIVERSAL_1_3;
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
    validate_spirv(words, spirv_environment(words, false), "invalid SPIR-V: ");
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
                // SPIRV-Cross unifies struct types with identical layouts, so
                // two buffers with the same shape share base_type_id. The first
                // resource claims the type name; subsequent resources keep
                // their variable name but reuse the shared struct typedef
                // downstream. One C typedef, multiple bind slots.
                if (types.insert(resource.base_type_id).second) {
                    name_members(resource.base_type_id);
                    compiler.set_name(resource.base_type_id, name);
                }
            }
        }
    };
    rename(resources.uniform_buffers, true);
    rename(resources.storage_buffers, true);
    rename(resources.storage_images, false);
    rename(resources.separate_images, false);
    rename(resources.separate_samplers, false);
    rename(resources.acceleration_structures, false);
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
    Json describe_fragment_storage(const std::vector<refl::StorageImage> &images);
    Json describe_bindless_textures(const std::vector<ReflectedBindlessTexture> &textures);
    Json describe_bindless_samplers(const std::vector<ReflectedBindlessSampler> &samplers);
    std::string native_profile(Slang::Enum target, ShaderStage::Enum stage) {
        const char *profile = hlsl_target(target, stage);
        return profile ? profile : "";
    }
    std::string internal_program_prefix, public_program_prefix;

  private:
    // Native-only uniform types have no GL upload enum. translate() checks the
    // target before the descriptor uses these members.
    std::string uniform_type(refl::Type::Enum type) override {
        // Native backends upload the block as bytes. GL has no unsigned Sokol
        // uniform upload type, so translate() rejects unsigned blocks for GL.
        return type == refl::Type::Struct || (type >= refl::Type::UInt && type <= refl::Type::UInt4)
                   ? "SG_UNIFORMTYPE_INVALID"
                   : gen::SokolCGenerator::uniform_type(type);
    }
    std::string flattened_uniform_type(refl::Type::Enum type) override {
        if (type >= refl::Type::UInt && type <= refl::Type::UInt4)
            return "SG_UNIFORMTYPE_INVALID";
        return type == refl::Type::Struct ? "SG_UNIFORMTYPE_FLOAT4"
                                          : gen::SokolCGenerator::flattened_uniform_type(type);
    }
    // Sokol's version emits only plain uniforms; this one adds struct arrays, whose
    // fields translate() limited to float4, int4 and float4x4 at std140 offsets.
    void gen_uniform_block_decl(const gen::GenInput &gen, const refl::UniformBlock &ub) override;
    void gen_uniform_decl(const gen::GenInput &gen, const refl::Type &uniform);
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

void SokolGenerator::gen_uniform_decl(const gen::GenInput &gen, const refl::Type &uniform) {
    using T = refl::Type;
    const std::string count =
        uniform.array_count ? fmt::format("[{}]", uniform.array_count) : std::string();
    if (uniform.type == T::Struct) {
        l_open("struct {{\n");
        for (const auto &field : uniform.struct_items)
            gen_uniform_decl(gen, field);
        l_close("}} {}{};\n", uniform.name, count);
        return;
    }
    const auto custom = gen.inp.ctype_map.find(uniform.type_as_glsl());
    if (custom != gen.inp.ctype_map.end()) {
        l("{} {}{};\n", custom->second, uniform.name, count);
        return;
    }
    switch (uniform.type) {
    case T::Float:
        l("float {}{};\n", uniform.name, count);
        break;
    case T::Float2:
        l("float {}{}[2];\n", uniform.name, count);
        break;
    case T::Float3:
        l("float {}{}[3];\n", uniform.name, count);
        break;
    case T::Float4:
        l("float {}{}[4];\n", uniform.name, count);
        break;
    case T::Int:
        l("int {}{};\n", uniform.name, count);
        break;
    case T::Int2:
        l("int {}{}[2];\n", uniform.name, count);
        break;
    case T::Int3:
        l("int {}{}[3];\n", uniform.name, count);
        break;
    case T::Int4:
        l("int {}{}[4];\n", uniform.name, count);
        break;
    case T::UInt:
        l("uint32_t {}{};\n", uniform.name, count);
        break;
    case T::UInt2:
        l("uint32_t {}{}[2];\n", uniform.name, count);
        break;
    case T::UInt3:
        l("uint32_t {}{}[3];\n", uniform.name, count);
        break;
    case T::UInt4:
        l("uint32_t {}{}[4];\n", uniform.name, count);
        break;
    case T::Mat4x4:
        l("float {}{}[16];\n", uniform.name, count);
        break;
    default:
        throw std::runtime_error(uniform.name + ": unsupported uniform type");
    }
}

void SokolGenerator::gen_uniform_block_decl(const gen::GenInput &gen,
                                            const refl::UniformBlock &ub) {
    l("#pragma pack(push,1)\n");
    int offset = 0;
    l_open("SOKOL_SHDC_ALIGN({}) typedef struct {} {{\n", ub.struct_info.align,
           struct_name(ub.name));
    for (const auto &uniform : ub.struct_info.struct_items) {
        if (uniform.offset > offset) {
            l("uint8_t _pad_{}[{}];\n", offset, uniform.offset - offset);
            offset = uniform.offset;
        }
        gen_uniform_decl(gen, uniform);
        // Reflection gives a struct array the size of one element.
        offset += uniform.type == refl::Type::Struct ? uniform.array_stride * uniform.array_count
                                                     : uniform.size;
    }
    if (offset < roundup(offset, 16))
        l("uint8_t _pad_{}[{}];\n", offset, roundup(offset, 16) - offset);
    l_close("}} {};\n", struct_name(ub.name));
    l("#pragma pack(pop)\n");
}

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
            Json described = {
                {"name", member.name},
                {"offset", member.offset},
                {"type", uniform_type(member.type)},
                {"array_count", member.array_count},
                {"binding_name", found == uniform_paths.end() ? path : found->second}};
            if (member.type == refl::Type::Struct) {
                // Field offsets are relative to each element, which is array_stride bytes.
                described["array_stride"] = member.array_stride;
                described["fields"] = Json::array();
                for (const auto &field : member.struct_items)
                    described["fields"].push_back({{"name", field.name},
                                                   {"offset", field.offset},
                                                   {"type", uniform_type(field.type)}});
            }
            item["members"].push_back(std::move(described));
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
        // The record type and size match the generated C struct and the array stride.
        const auto &records = buffer.struct_info.struct_items.at(0);
        item["struct_name"] = records.struct_typename;
        item["record_size"] = records.size;
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

// Sokol's layout has no fragment storage image list, so this adds one with the
// same fields as storage_images. Native indices are the ones translate()
// assigned for Metal and Vulkan.
Json SokolGenerator::describe_fragment_storage(const std::vector<refl::StorageImage> &images) {
    Json list = Json::array();
    for (const auto &image : images)
        list.push_back(
            {{"name", image.name},
             {"slot", image.sokol_slot},
             {"stage", shader_stage(ShaderStage::Fragment)},
             {"image_type", image_type(image.type)},
             {"access_format", storage_pixel_format(image.access_format)},
             {"writeonly", image.writeonly},
             {"msl_texture_n", fragment_storage_native_index(Slang::METAL_MACOS, image.sokol_slot)},
             {"spirv_set1_binding_n",
              fragment_storage_native_index(Slang::SPIRV_VK, image.sokol_slot)}});
    return list;
}

Json SokolGenerator::describe_bindless_textures(
    const std::vector<ReflectedBindlessTexture> &textures) {
    Json list = Json::array();
    for (const auto &texture : textures)
        list.push_back({{"name", texture.name},
                        {"slot", texture.slot},
                        {"stage", shader_stage(texture.stage)},
                        {"image_type", image_type(texture.type)},
                        {"sample_type", image_sample_type(texture.sample_type)},
                        {"multisampled", texture.multisampled},
                        {"msl_buffer_n", bindless_msl_buffer_index(texture.stage, texture.slot)},
                        {"spirv_set_n", bindless_spirv_set_index(texture.slot)},
                        {"spirv_binding_n", 0}});
    return list;
}

Json SokolGenerator::describe_bindless_samplers(
    const std::vector<ReflectedBindlessSampler> &samplers) {
    Json list = Json::array();
    for (const auto &sampler : samplers)
        list.push_back({{"name", sampler.name},
                        {"slot", sampler.slot},
                        {"stage", shader_stage(sampler.stage)},
                        {"sampler_type", sampler_type(sampler.type)},
                        {"msl_buffer_n", bindless_msl_buffer_index(sampler.stage, sampler.slot)},
                        {"spirv_set_n", bindless_spirv_set_index(sampler.slot)},
                        {"spirv_binding_n", 0}});
    return list;
}

// Reads texture and sampler use from SPIR-V so frontend callers need only
// supply policies, such as float filterability, that shader code cannot state.
class ResourceInspector : public Compiler {
  public:
    using Compiler::Compiler;
    void inspect_sampling() { analyze_image_and_sampler_usage(); }
    bool uses_comparison(uint32_t id) const { return comparison_ids.count(id) != 0; }
};

static uint32_t prepare_glsl(CompilerGLSL &compiler, bool combine_image_samplers = true) {
    for (const auto &resource : compiler.get_shader_resources().uniform_buffers)
        if (Spirvcross::can_flatten_uniform_block(compiler, resource))
            compiler.flatten_buffer_block(resource.id);
    if (!combine_image_samplers)
        return 0;
    const uint32_t dummy_sampler = compiler.build_dummy_sampler_for_combined_images();
    compiler.build_combined_image_samplers();
    for (const auto &pair : compiler.get_combined_image_samplers())
        compiler.set_name(pair.combined_id, compiler.get_name(pair.image_id) + "_" +
                                                compiler.get_name(pair.sampler_id));
    return dummy_sampler;
}

static SpirvcrossSource translate(const Input &input, int index, std::vector<uint32_t> words,
                                  const std::map<std::string, Binding> &metadata,
                                  std::set<std::string> &used, Json &report, Json &stage_spirv,
                                  Slang::Enum target, bool last_target, Bytecode &bytecode,
                                  std::map<std::string, std::string> &uniform_paths,
                                  std::vector<refl::StorageImage> &fragment_images,
                                  std::vector<ReflectedBindlessTexture> &bindless_textures,
                                  std::vector<ReflectedBindlessSampler> &bindless_samplers) {
    const auto shader_stage = ShaderStage::from_snippet_type(input.snippets[index].type);
    const auto stage_name = ShaderStage::to_str(shader_stage);
    const bool compute = shader_stage == ShaderStage::Compute;
    if (shader_stage == ShaderStage::Vertex)
        words = sokol_slang::preserve_vertex_position_invariance(std::move(words));
    if (compute &&
        (target == Slang::GLSL410 || target == Slang::GLSL300ES || target == Slang::HLSL4))
        throw std::runtime_error(
            std::string(Slang::to_str(target)) +
            ": compute requires glsl430, glsl310es, hlsl5, Metal, WGSL or Vulkan");
    ReflectionCompiler analysis(words);
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
        !resources.subpass_inputs.empty() || !resources.atomic_counters.empty() ||
        !resources.gl_plain_uniforms.empty() || !resources.shader_record_buffers.empty())
        throw std::runtime_error("supported resources are flat uniforms, storage buffers/images, "
                                 "separate textures, samplers and compute acceleration structures");
    const bool ray_query = !resources.acceleration_structures.empty();
    if (ray_query) {
        if (!compute)
            throw std::runtime_error(
                "acceleration structures are only supported in compute shaders");
        if (!supports_ray_query(target))
            throw std::runtime_error(std::string(Slang::to_str(target)) +
                                     ": ray query requires Metal (macOS or iOS) or Vulkan");
        if (resources.acceleration_structures.size() > kMaxAccelerationSlots)
            throw std::runtime_error("too many acceleration structures");
        std::set<int> accelerations;
        for (const auto &resource : resources.acceleration_structures) {
            const auto found = metadata.find(resource.name);
            if (found == metadata.end() || found->second.kind != "acceleration_structure")
                throw std::runtime_error("missing or wrong metadata for resource: " +
                                         resource.name);
            if (!analysis.get_type(resource.type_id).array.empty())
                throw std::runtime_error("resource arrays are not supported: " + resource.name);
            if (!accelerations.insert(found->second.slot).second)
                throw std::runtime_error("duplicate acceleration structure slot: " + resource.name);
            used.insert(resource.name);
        }
    }
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
            throw std::runtime_error("storage buffer must contain one unsized array of records "
                                     "or 32-bit scalars/vectors");
    }
    BindSlotMap slots;
    // Fragment storage images and bindless tables stay out of Sokol's slot map;
    // sokol-utils binds them from the layout lists.
    struct FragmentStorage {
        uint32_t id;
        std::string name;
        int slot;
        int qualifier;
    };
    struct BindlessResource {
        uint32_t id;
        uint32_t type_id;
        std::string name;
        std::string kind;
        int slot;
    };
    std::vector<FragmentStorage> fragment_storage;
    std::vector<BindlessResource> bindless_resources;
    std::set<int> bindless_slots;
    size_t bindless_texture_count = 0;
    size_t bindless_sampler_count = 0;
    auto add = [&](const auto &list, const std::string &kind, BindSlot::Type type) {
        for (const auto &resource : list) {
            const auto &resource_type = analysis.get_type(resource.type_id);
            if (is_runtime_array(resource_type)) {
                if (kind != "texture" && kind != "sampler")
                    throw std::runtime_error("resource arrays are not supported: " + resource.name);
                const std::string expected_kind =
                    kind == "texture" ? "bindless_texture" : "bindless_sampler";
                const auto found = metadata.find(resource.name);
                if (found == metadata.end() || found->second.kind != expected_kind)
                    throw std::runtime_error("missing or wrong metadata for resource: " +
                                             resource.name);
                if (!supports_bindless(target))
                    throw std::runtime_error(std::string(Slang::to_str(target)) +
                                             ": bindless tables require Metal or Vulkan");
                if (!bindless_slots.insert(found->second.slot).second)
                    throw std::runtime_error("duplicate bindless slot: " + resource.name);
                bindless_resources.push_back({resource.id, resource.type_id, resource.name,
                                              expected_kind, found->second.slot});
                if (expected_kind == "bindless_texture")
                    ++bindless_texture_count;
                else
                    ++bindless_sampler_count;
                used.insert(resource.name);
                continue;
            }
            const auto found = metadata.find(resource.name);
            if (found == metadata.end() || found->second.kind != kind)
                throw std::runtime_error("missing or wrong metadata for resource: " +
                                         resource.name);
            if (!resource_type.array.empty())
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
                if (shader_stage == ShaderStage::Vertex)
                    throw std::runtime_error(
                        "storage images are allowed in compute and fragment shaders only");
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
                if (shader_stage == ShaderStage::Fragment) {
                    if (!supports_fragment_storage(target))
                        throw std::runtime_error(
                            std::string(Slang::to_str(target)) +
                            ": fragment storage images require Metal or Vulkan");
                    fragment_storage.push_back(
                        {resource.id, resource.name, found->second.slot, qualifier});
                    used.insert(resource.name);
                    continue;
                }
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
    const bool has_bindless = !bindless_resources.empty();
    if (bindless_resources.size() > kMaxBindlessSlots)
        throw std::runtime_error("too many bindless tables");
    for (const auto &output : resources.stage_outputs)
        if (shader_stage == ShaderStage::Fragment &&
            analysis.has_decoration(output.id, spv::DecorationLocation))
            slots.track_fs_output_location(
                analysis.get_decoration(output.id, spv::DecorationLocation));
    // Native backends can use every Sokol view slot. The portable minimum is
    // not their maximum; the runtime checks the device's actual stage limits.
    const bool native_views = target == Slang::METAL_MACOS || target == Slang::METAL_IOS ||
                              target == Slang::METAL_SIM || target == Slang::SPIRV_VK ||
                              target == Slang::HLSL5;
    const size_t texture_limit =
        native_views ? MaxViews : SG_MAX_PORTABLE_TEXTURE_BINDINGS_PER_STAGE;
    // Metal has 31 buffer indices. Sokol reserves eight each for vertex
    // buffers and uniform blocks, leaving fifteen for storage buffers (eleven
    // in a vertex stage that also binds bindless argument buffers at 19..22).
    const bool metal =
        target == Slang::METAL_MACOS || target == Slang::METAL_IOS || target == Slang::METAL_SIM;
    const size_t storage_buffer_limit =
        metal ? 31 - SG_MAX_VERTEXBUFFER_BINDSLOTS - SG_MAX_UNIFORMBLOCK_BINDSLOTS -
                    (shader_stage == ShaderStage::Vertex && has_bindless ? kMaxBindlessSlots : 0)
              : SG_MAX_PORTABLE_STORAGEBUFFER_BINDINGS_PER_STAGE;
    const size_t regular_textures = resources.separate_images.size() - bindless_texture_count;
    const size_t regular_samplers = resources.separate_samplers.size() - bindless_sampler_count;
    if (resources.uniform_buffers.size() > MaxUniformBlocks ||
        resources.storage_buffers.size() > storage_buffer_limit ||
        resources.storage_images.size() > 8 || regular_textures > texture_limit ||
        regular_samplers > MaxSamplers)
        throw std::runtime_error(
            std::string(Slang::to_str(target)) + " " + stage_name +
            ": resource count exceeds stage limits (uniforms " +
            std::to_string(resources.uniform_buffers.size()) + "/8, storage buffers " +
            std::to_string(resources.storage_buffers.size()) + "/" +
            std::to_string(storage_buffer_limit) + ", storage images " +
            std::to_string(resources.storage_images.size()) + "/8, textures " +
            std::to_string(regular_textures) + "/" + std::to_string(texture_limit) + ", samplers " +
            std::to_string(regular_samplers) + "/" + std::to_string(MaxSamplers) + ")");
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
        for (const auto &resource : resources.acceleration_structures) {
            compiler.set_decoration(
                resource.id, spv::DecorationBinding,
                acceleration_native_index(target, metadata.at(resource.name).slot, metadata));
            compiler.set_decoration(resource.id, spv::DecorationDescriptorSet,
                                    Slang::is_spirv(target) ? 1 : 0);
        }
        for (const auto &image : fragment_storage) {
            compiler.set_decoration(image.id, spv::DecorationBinding,
                                    fragment_storage_native_index(target, image.slot));
            compiler.set_decoration(image.id, spv::DecorationDescriptorSet,
                                    Slang::is_spirv(target) ? 1 : 0);
        }
        for (const auto &bindless : bindless_resources) {
            if (Slang::is_msl(target)) {
                compiler.set_decoration(bindless.id, spv::DecorationBinding,
                                        bindless_msl_buffer_index(shader_stage, bindless.slot));
                compiler.set_decoration(bindless.id, spv::DecorationDescriptorSet, 0);
            } else if (Slang::is_spirv(target)) {
                compiler.set_decoration(bindless.id, spv::DecorationBinding, 0);
                compiler.set_decoration(bindless.id, spv::DecorationDescriptorSet,
                                        bindless_spirv_set_index(bindless.slot));
            }
        }
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
        patch(resources.acceleration_structures);
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
        // Ray queries, bindless descriptor arrays, and unsigned 64-bit atomic
        // buffer min/max need MSL 2.4. Other programs retain MSL 2.1.
        options.set_msl_version(
            2, ray_query || has_bindless || analysis.uses_uint64_atomic_minmax() ? 4 : 1);
        if (has_bindless)
            options.argument_buffers_tier = CompilerMSL::Options::ArgumentBuffersTier::Tier2;
        // Sokol's Metal backend supports base-vertex and base-instance draws.
        options.ios_support_base_vertex_instance = true;
        options.enable_decoration_binding = true;
        compiler.set_msl_options(options);
        result.source_code = compiler.compile();
        if (compiler.needs_buffer_size_buffer()) {
            throw std::runtime_error(std::string(Slang::to_str(target)) +
                                     ": storage buffer GetDimensions() requires Metal "
                                     "spvBufferSizeConstants, which sokol_gfx does not bind; pass "
                                     "buffer capacity in a uniform or header word instead");
        }
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
        validate_spirv(patched, spirv_environment(patched, true), "Vulkan SPIR-V validation: ");
        BytecodeBlob blob;
        blob.valid = true;
        blob.snippet_index = index;
        const auto *data = reinterpret_cast<const uint8_t *>(patched.data());
        blob.data.assign(data, data + patched.size() * sizeof(uint32_t));
        bytecode.blobs.push_back(std::move(blob));
    }

    // Sokol detects depth textures and comparison samplers after GLSL analysis.
    // This analysis does not feed backend shader translation.
    ReflectionCompiler reflect(words);
    name_resources(reflect, stage_name, metadata);
    CompilerGLSL::Options gl;
    gl.version = 430;
    // SPIRV-Cross rejects ray query, runtime descriptor arrays and non-uniform
    // indexing unless it targets Vulkan GLSL 460. Shared Slang modules can emit
    // CapabilityShaderNonUniform in stages that do not touch the bindless table.
    // This copy feeds reflection only; no backend consumes its text.
    if (ray_query || has_bindless || uses_vulkan_glsl_capabilities(reflect)) {
        gl.version = 460;
        gl.vulkan_semantics = true;
    }
    gl.emit_uniform_buffer_as_plain_uniforms = true;
    // BindSlot access comes from the validated SPIR-V above. GLSL's optional
    // qualifier deduction narrows write-only uses of a read/write image during
    // compile(), which would make reflection disagree with those same slots.
    gl.enable_storage_image_qualifier_deduction = false;
    reflect.set_common_options(gl);
    // Bindless programs run only on Metal and Vulkan, which never consume GL
    // combined image/sampler pairs; skipping pair construction keeps omitted
    // bindless images out of combined sampler pairs.
    const uint32_t dummy_sampler = prepare_glsl(reflect, !has_bindless);
    reflect.compile();
    // CompilerGLSL can rename identifiers reserved by GLSL. BindSlotMap uses
    // the original image and sampler names; only the combined sampler names
    // must retain their emitted spelling for runtime GLSL uniform lookup.
    const auto original_resources = analysis.get_shader_resources();
    for (const auto &image : original_resources.separate_images)
        reflect.set_name(image.id, analysis.get_name(image.id));
    for (const auto &sampler : original_resources.separate_samplers)
        reflect.set_name(sampler.id, analysis.get_name(sampler.id));
    BindSlotMap reflection_slots = slots;
    for (const auto &storage : fragment_storage) {
        const auto &type = analysis.get_type_from_variable(storage.id);
        refl::StorageImage image;
        image.name = storage.name;
        image.stage = shader_stage;
        image.sokol_slot = storage.slot;
        image.type = refl::Reflection::spirtype_to_image_type(type);
        image.access_format = reflection_storage_format(type);
        image.writeonly = (storage.qualifier & BindSlot::WriteOnly) != 0;
        image.msl_texture_n = fragment_storage_native_index(Slang::METAL_MACOS, storage.slot);
        image.spirv_set1_binding_n = fragment_storage_native_index(Slang::SPIRV_VK, storage.slot);
        fragment_images.push_back(image);
        reflect.omit_resource(storage.id);
    }
    if (has_bindless) {
        ResourceInspector inspector(words);
        inspector.inspect_sampling();
        for (const auto &bindless : bindless_resources) {
            if (bindless.kind == "bindless_texture") {
                const auto &type = analysis.get_type(bindless.type_id);
                const auto image_type = refl::Reflection::spirtype_to_image_type(type);
                if (image_type == refl::ImageType::INVALID)
                    throw std::runtime_error(bindless.name +
                                             ": unsupported bindless texture dimension");
                const auto scalar = analysis.get_type(type.image.type).basetype;
                auto sample_type = (type.image.depth || inspector.uses_comparison(bindless.id))
                                       ? refl::ImageSampleType::DEPTH
                                   : scalar == SPIRType::Int  ? refl::ImageSampleType::SINT
                                   : scalar == SPIRType::UInt ? refl::ImageSampleType::UINT
                                                              : refl::ImageSampleType::FLOAT;
                if (const auto *tag = input.find_image_sample_type_tag(bindless.name)) {
                    using T = refl::ImageSampleType;
                    if (tag->type != sample_type &&
                        !(sample_type == T::FLOAT && tag->type == T::UNFILTERABLE_FLOAT))
                        throw std::runtime_error(
                            bindless.name + ": sample_type does not match SPIR-V texture type");
                    sample_type = tag->type;
                }
                bindless_textures.push_back({bindless.name, shader_stage, bindless.slot, image_type,
                                             sample_type, type.image.ms});
            } else {
                auto sampler_type = inspector.uses_comparison(bindless.id)
                                        ? refl::SamplerType::COMPARISON
                                        : refl::SamplerType::FILTERING;
                if (const auto *tag = input.find_sampler_type_tag(bindless.name)) {
                    using T = refl::SamplerType;
                    if ((sampler_type == T::COMPARISON) != (tag->type == T::COMPARISON))
                        throw std::runtime_error(
                            bindless.name + ": sampler_type does not match SPIR-V comparison use");
                    sampler_type = tag->type;
                }
                bindless_samplers.push_back(
                    {bindless.name, shader_stage, bindless.slot, sampler_type});
            }
            reflect.omit_resource(bindless.id);
        }
    }
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
    // Report the image name before Sokol's parser loses it in its error path.
    for (const auto &image : reflect.get_shader_resources().separate_images)
        if (reflection_slots.find_view_index(image.name) < 0)
            throw std::runtime_error("texture binding lookup failed for '" + image.name +
                                     "' (SPIR-V resource '" + analysis.get_name(image.id) +
                                     "', stage " + stage_name + ")");
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
    // Slang wraps each std140 array in a struct with one `data` array member. Replace the
    // wrapper with that array so the C struct and Sokol's uniform list name the member as
    // written; GL lookups keep the real path through uniform_paths.
    using T = refl::Type;
    // Uniform arrays hold float4, int4, uint4 or float4x4 elements, whose std140 strides
    // match C. std140 pads float, float2 and float3 array elements to 16 bytes.
    const auto check_unsigned = [target](const std::string &name, const refl::Type &item) {
        if (item.type >= T::UInt && item.type <= T::UInt4 && Slang::is_glsl(target))
            throw std::runtime_error(name + ": Sokol GL uniform uploads do not support unsigned "
                                            "integers; use signed integers for GL targets");
    };
    auto check_array_element = [&](const std::string &name, const refl::Type &item) {
        check_unsigned(name, item);
        if (item.type != T::Float4 && item.type != T::Int4 && item.type != T::UInt4 &&
            item.type != T::Mat4x4)
            throw std::runtime_error(
                name +
                ": uniform arrays hold float4, int4, uint4 or float4x4 elements only; std140 "
                "pads float, float2 and float3 elements to 16 bytes, so pack them into "
                "float4, int4 or uint4 elements instead");
        if (item.is_matrix && item.matrix_stride != 16)
            throw std::runtime_error(name + ": uniform matrices need a 16 byte column stride");
    };
    for (auto &block : result.stage_refl.bindings.uniform_blocks) {
        const std::string block_name = block.name;
        for (auto &member : block.struct_info.struct_items) {
            const std::string name = block_name + "." + member.name;
            if (member.type == T::Struct && !member.is_array && member.struct_items.size() == 1 &&
                member.struct_items[0].is_array) {
                auto array = member.struct_items[0];
                const int bytes =
                    array.type == T::Struct ? array.array_stride * array.array_count : array.size;
                if (array.offset != 0 || bytes != member.size)
                    throw std::runtime_error(name + ": array wrapper has padding Sokol cannot "
                                                    "express");
                uniform_paths[block.inst_name + "." + member.name] =
                    block.inst_name + "." + member.name + "." + array.name;
                array.name = member.name;
                array.offset = member.offset;
                member = array;
            }
            if (member.is_array && member.array_count == 0)
                throw std::runtime_error(name + ": uniform arrays need a fixed size");
            if (member.type == T::Struct) {
                if (!member.is_array)
                    throw std::runtime_error(name + ": uniform structs are supported only as "
                                                    "array elements");
                // Struct arrays map to one C array of structs; each field is a full std140
                // slot, so the C fields follow each other without padding.
                int offset = 0;
                for (const auto &field : member.struct_items) {
                    const std::string field_name = name + "." + field.name;
                    if (field.is_array || field.type == T::Struct)
                        throw std::runtime_error(field_name +
                                                 ": struct array elements hold "
                                                 "float4, int4, uint4 or float4x4 fields only");
                    check_array_element(field_name, field);
                    if (field.offset != offset)
                        throw std::runtime_error(field_name + ": unexpected std140 offset");
                    offset += field.size;
                }
                if (member.array_stride != offset)
                    throw std::runtime_error(name + ": unexpected std140 struct array stride");
                // Sokol's GL backend binds uniforms by name and has no struct uniforms, so
                // GL reads struct arrays only through a flattened vec4 block.
                // SokolGenerator::flattened_uniform_type reports such blocks as FLOAT4.
                if (Slang::is_glsl(target) &&
                    (!block.flattened || member.struct_items[0].type == T::Int4))
                    throw std::runtime_error(
                        name + ": GL needs uniform blocks holding struct arrays to use only "
                               "float4 and float4x4 members, and to index those arrays instead "
                               "of copying them whole");
                continue;
            }
            check_unsigned(name, member);
            const bool scalar_vector = (member.type >= T::Int && member.type <= T::UInt4) ||
                                       (member.type >= T::Float && member.type <= T::Float4);
            if (!scalar_vector && member.type != T::Mat4x4)
                throw std::runtime_error(name + ": uniforms support int/uint/float vectors and "
                                                "float4x4 only");
            if (member.is_array) {
                check_array_element(name, member);
                if (member.array_stride != (member.type == T::Mat4x4 ? 64 : 16))
                    throw std::runtime_error(name + ": unexpected uniform array stride");
            }
            if (member.is_matrix && member.matrix_stride != 16)
                throw std::runtime_error(name + ": unsupported uniform matrix stride");
        }
    }
    // Verify the generated descriptor uses the emitted backend entry name.
    result.stage_refl.entry_point = entries[0].name;
    if (result.stage_refl.entry_point_by_slang(target) != native_entry)
        throw std::runtime_error("backend entry name does not match Sokol's naming rule");
    result.valid = true;
    if (stage_spirv.is_null()) {
        CompilerReflection raw(words);
        name_resources(raw, stage_name, metadata);
        stage_spirv = Json::parse(raw.compile());
    }
    report = {{"entry", native_entry}, {"bindings", Json::array()}};
    if (last_target)
        report["spirv"] = std::move(stage_spirv);
    else
        report["spirv"] = stage_spirv;
    auto describe = [&](const auto &list, const char *kind, int category) {
        for (const auto &resource : list) {
            const auto &binding = metadata.at(resource.name);
            int native = category == 0   ? slots.find_uniformblock_slang_slot(resource.name, target)
                         : category == 1 ? slots.find_view_slang_slot(resource.name, target)
                                         : slots.find_sampler_slang_slot(resource.name, target);
            if (binding.kind == "storage_image" && binding.stage == "fragment")
                native = fragment_storage_native_index(target, binding.slot);
            if (binding.kind == "bindless_texture" || binding.kind == "bindless_sampler")
                native = Slang::is_msl(target)
                             ? bindless_msl_buffer_index(shader_stage, binding.slot)
                             : 0;
            Json item = {
                {"name", resource.name},
                {"kind", binding.kind == "bindless_texture" || binding.kind == "bindless_sampler"
                             ? binding.kind
                             : std::string(kind)},
                {"slot", binding.slot},
                {"binding", native}};
            if (Slang::is_spirv(target) &&
                (binding.kind == "bindless_texture" || binding.kind == "bindless_sampler"))
                item["set"] = bindless_spirv_set_index(binding.slot);
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
    for (const auto &resource : resources.acceleration_structures)
        report["bindings"].push_back(
            {{"name", resource.name},
             {"kind", "acceleration_structure"},
             {"slot", metadata.at(resource.name).slot},
             {"binding",
              acceleration_native_index(target, metadata.at(resource.name).slot, metadata)}});
    if (compute)
        report["workgroup_size"] = result.stage_refl.cs_workgroup_size;
    if (shader_stage == ShaderStage::Fragment)
        report["raster_ordered"] = uses_interlock(analysis);
    return result;
}

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
    auto append = [&](const auto &list, const char *base_kind) {
        for (const auto &resource : list) {
            const auto &type = compiler.get_type(resource.type_id);
            std::string kind = base_kind;
            if (is_runtime_array(type)) {
                if (kind == "texture")
                    kind = "bindless_texture";
                else if (kind == "sampler")
                    kind = "bindless_sampler";
                else
                    throw std::runtime_error("runtime-sized resource arrays are only supported for "
                                             "separate textures and samplers");
            }
            Json binding = {
                {"name", compiler.get_name(resource.id)},
                {"set", compiler.get_decoration(resource.id, spv::DecorationDescriptorSet)},
                {"binding", compiler.get_decoration(resource.id, spv::DecorationBinding)},
                {"kind", kind}};
            if (kind == "texture" || kind == "bindless_texture") {
                const auto &image = type.image;
                const auto scalar = compiler.get_type(image.type).basetype;
                binding["sample_type"] = image.depth || compiler.uses_comparison(resource.id)
                                             ? "depth"
                                         : scalar == SPIRType::Int  ? "sint"
                                         : scalar == SPIRType::UInt ? "uint"
                                                                    : "float";
            }
            if (kind == "sampler" || kind == "bindless_sampler")
                binding["sampler_type"] =
                    compiler.uses_comparison(resource.id) ? "comparison" : "filtering";
            if (kind == "storage_buffer")
                binding["access"] =
                    compiler.get_buffer_block_flags(resource.id).get(spv::DecorationNonWritable)
                        ? "read"
                        : "read_write";
            if (kind == "storage_image") {
                binding["access"] = storage_image_access(compiler, resource);
                binding["format"] = storage_image_format(type);
            }
            output["resources"].push_back(binding);
        }
    };
    append(resources.uniform_buffers, "uniform");
    append(resources.storage_buffers, "storage_buffer");
    append(resources.storage_images, "storage_image");
    append(resources.separate_images, "texture");
    append(resources.separate_samplers, "sampler");
    append(resources.acceleration_structures, "acceleration_structure");
    return output;
}

Json inspect_spirv(const std::string &path) { return inspect_words(read_spirv(path.c_str())); }

// Sokol's layout has no acceleration structure list, so this adds one. The
// host binds each structure by slot; native indices let a backend skip a lookup.
static Json describe_acceleration_structures(const Manifest &manifest) {
    Json list = Json::array();
    for (const auto &[name, binding] : manifest.bindings)
        if (binding.kind == "acceleration_structure")
            list.push_back(
                {{"name", name},
                 {"slot", binding.slot},
                 {"stage", "SG_SHADERSTAGE_COMPUTE"},
                 {"msl_buffer_n",
                  acceleration_native_index(Slang::METAL_MACOS, binding.slot, manifest.bindings)},
                 {"spirv_set1_binding_n",
                  acceleration_native_index(Slang::SPIRV_VK, binding.slot, manifest.bindings)}});
    return list;
}

// Adds the lists of resources Sokol has no slot for and the slot maxima this
// compiler used. A consumer compares the maxima with its own sokol_gfx.h to
// catch a mismatch. raster_ordered marks a fragment stage with a shader
// interlock; the runtime must check sg_fragment_storage_raster_order_supported.
static void add_binding_contract(Json &layout, const Manifest &manifest,
                                 Json fragment_storage_images, Json bindless_textures,
                                 Json bindless_samplers, bool raster_ordered) {
    layout["acceleration_structures"] = describe_acceleration_structures(manifest);
    layout["max_acceleration_slots"] = kMaxAccelerationSlots;
    layout["fragment_storage_images"] = std::move(fragment_storage_images);
    layout["max_fragment_storage_slots"] = kMaxFragmentStorageSlots;
    layout["bindless_textures"] = std::move(bindless_textures);
    layout["bindless_samplers"] = std::move(bindless_samplers);
    layout["max_bindless_slots"] = kMaxBindlessSlots;
    layout["raster_ordered"] = raster_ordered;
    layout["max_view_bindslots"] = kMaxViewBindslots;
    layout["max_sampler_bindslots"] = kMaxSamplerBindslots;
}

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
        if (resource["kind"] == "texture" || resource["kind"] == "bindless_texture")
            input.image_sample_type_tags[name] = ImageSampleTypeTag(
                name, refl::ImageSampleType::from_str(resource["sample_type"]), 0);
        if (resource["kind"] == "sampler" || resource["kind"] == "bindless_sampler")
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
    // Every target reflects the same fragment storage and bindless lists; keep
    // the first target's stage reflections.
    std::vector<refl::StorageImage> fragment_images;
    std::vector<ReflectedBindlessTexture> bindless_textures;
    std::vector<ReflectedBindlessSampler> bindless_samplers;
    bool raster_ordered = false;
    std::vector<std::vector<uint32_t>> binaries;
    // Every backend, reflection and Vulkan output sees scalar and vector
    // storage buffer elements as one-member records.
    for (const auto &stage : stages)
        binaries.push_back(sokol_slang::wrap_storage_buffer_elements(
            normalize_entry(captured ? captured->at(stage)
                                     : read_spirv(manifest.stage_path(stage).string().c_str()),
                            manifest.document["stages"][stage]["entry"])));
    // Raw SPIR-V reflection uses the same normalized stage for every target.
    std::vector<Json> stage_spirv(stages.size());
    for (auto target : targets) {
        auto &sources = translated[target].sources;
        for (int i = 0; i < int(stages.size()); ++i) {
            const auto &stage = stages[i];
            Json stage_json;
            std::vector<refl::StorageImage> images;
            std::vector<ReflectedBindlessTexture> stage_bindless_textures;
            std::vector<ReflectedBindlessSampler> stage_bindless_samplers;
            sources.push_back(translate(input, i, binaries[i], manifest.bindings, used, stage_json,
                                        stage_spirv[i], target, target == targets.back(),
                                        bytecode[target], generator.uniform_paths, images,
                                        stage_bindless_textures, stage_bindless_samplers));
            if (target == targets.front()) {
                fragment_images.insert(fragment_images.end(), images.begin(), images.end());
                bindless_textures.insert(bindless_textures.end(), stage_bindless_textures.begin(),
                                         stage_bindless_textures.end());
                bindless_samplers.insert(bindless_samplers.end(), stage_bindless_samplers.begin(),
                                         stage_bindless_samplers.end());
            }
            raster_ordered = raster_ordered || stage_json.value("raster_ordered", false);
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
            compile_native_bytecode(
                bytecode[target], translated[target], target, native_work,
                options.count("--metal-args")
                    ? Json::parse(options.at("--metal-args")).get<std::vector<std::string>>()
                    : std::vector<std::string>{});
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
        add_binding_contract(
            artifact->layout, manifest, generator.describe_fragment_storage(fragment_images),
            generator.describe_bindless_textures(bindless_textures),
            generator.describe_bindless_samplers(bindless_samplers), raster_ordered);
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
    add_binding_contract(report["layout"], manifest,
                         generator.describe_fragment_storage(fragment_images),
                         generator.describe_bindless_textures(bindless_textures),
                         generator.describe_bindless_samplers(bindless_samplers), raster_ordered);
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

// Preserve compiler source identity separately from backend binding names.
// Locations join attributes after remapping; descriptors join resource aliases.
static void source_names(ProgramArtifact &artifact, const Manifest &manifest,
                         const std::map<std::string, Json> &inspections) {
    std::map<int, std::string> attribute_names;
    bool first = true;
    for (const auto &[target, stages] : artifact.reflection.at("targets").items()) {
        if (!stages.contains("vertex"))
            continue;
        std::map<int, std::string> names;
        for (const auto &input : stages.at("vertex").at("spirv").value("inputs", Json::array())) {
            const std::string name = input.at("name");
            if (name.empty() || !names.emplace(input.at("location").get<int>(), name).second)
                throw std::runtime_error("Invalid source attribute identity for " + target);
        }
        if (!first && names != attribute_names)
            throw std::runtime_error("Source attributes disagree across backend targets");
        attribute_names = std::move(names);
        first = false;
    }
    for (auto &attribute : artifact.layout.at("attributes")) {
        const auto found = attribute_names.find(attribute.at("slot").get<int>());
        if (found == attribute_names.end())
            throw std::runtime_error("Native attribute has no source identity");
        attribute["source_name"] = found->second;
    }
    if (attribute_names.size() != artifact.layout.at("attributes").size())
        throw std::runtime_error("Native and source attribute locations disagree");
    const std::pair<const char *, const char *> lists[] = {
        {"uniform_blocks", "uniform"},
        {"textures", "texture"},
        {"samplers", "sampler"},
        {"storage_buffers", "storage_buffer"},
        {"storage_images", "storage_image"},
        {"acceleration_structures", "acceleration_structure"},
        {"fragment_storage_images", "storage_image"},
        {"bindless_textures", "bindless_texture"},
        {"bindless_samplers", "bindless_sampler"}};
    for (const auto &[list, kind] : lists)
        for (auto &resource : artifact.layout.at(list)) {
            const std::map<std::string, std::string> stage_names = {
                {"SG_SHADERSTAGE_VERTEX", "vertex"},
                {"SG_SHADERSTAGE_FRAGMENT", "fragment"},
                {"SG_SHADERSTAGE_COMPUTE", "compute"}};
            const std::string stage = stage_names.at(resource.at("stage").get<std::string>());
            const int slot = resource.at("slot");
            const Binding *binding = nullptr;
            for (const auto &[alias, candidate] : manifest.bindings)
                if (candidate.stage == stage && candidate.kind == kind && candidate.slot == slot) {
                    if (binding)
                        throw std::runtime_error("Ambiguous native resource identity");
                    binding = &candidate;
                }
            if (!binding)
                throw std::runtime_error("Native resource has no descriptor identity");
            std::string name;
            for (const auto &source : inspections.at(stage + ".spv").at("resources"))
                if (source.at("kind") == kind && source.at("set") == binding->set &&
                    source.at("binding") == binding->binding) {
                    if (!name.empty())
                        throw std::runtime_error("Ambiguous source descriptor identity");
                    name = source.at("name").get<std::string>();
                }
            if (name.empty())
                throw std::runtime_error("Native resource has no source name");
            resource["source_name"] = std::move(name);
        }
}

// Each backend call owns the native compiler's files. Atomic directory creation
// keeps concurrent calls and processes separate; cleanup never throws.
class NativeBytecodeDirectory {
  public:
    explicit NativeBytecodeDirectory(bool enabled) {
        if (!enabled)
            return;
        static std::atomic<uint64_t> sequence = 0;
        const auto root = std::filesystem::temp_directory_path();
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt != 128; ++attempt) {
            auto candidate = root / ("sokol-slang-native-" + std::to_string(nonce) + "-" +
                                     std::to_string(++sequence));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = std::move(candidate);
                return;
            }
            if (error && error != std::errc::file_exists)
                throw std::filesystem::filesystem_error("Cannot create native shader directory",
                                                        candidate, error);
        }
        throw std::runtime_error("Cannot reserve a native shader directory");
    }
    ~NativeBytecodeDirectory() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }
    NativeBytecodeDirectory(const NativeBytecodeDirectory &) = delete;
    NativeBytecodeDirectory &operator=(const NativeBytecodeDirectory &) = delete;
    const std::filesystem::path &path() const { return path_; }

  private:
    std::filesystem::path path_;
};

static ProgramArtifact compile_backend_impl(const BackendRequest &request) {
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
        validate_spirv(words, spirv_environment(words, false), "invalid SPIR-V: ");
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
    std::map<std::string, std::string> options;
    if (!request.metal_args.empty())
        options["--metal-args"] = Json(request.metal_args).dump();
    if (request.source_only)
        options["--source"] = "";
    if (!request.targets.empty())
        options["--targets"] = request.targets;
    NativeBytecodeDirectory work(!request.source_only);
    ProgramArtifact result;
    generate_program(manifest, options, work.path(), "", false, &request.stages, &result);
    source_names(result, manifest, inspections);
    if (!resources.is_null())
        result.reflection["resources"] = resources;
    return result;
}
ProgramArtifact compile_backend(const BackendRequest &request) {
    ProgramArtifact result;
    run_with_compiler_stack([&] { result = compile_backend_impl(request); });
    return result;
}

} // namespace sokol_slang
