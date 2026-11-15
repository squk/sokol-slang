#include "internal/native_bytecode.h"
#include "internal/position_invariance.h"
#include "spirv-tools/libspirv.hpp"
#include "spirv_msl.hpp"
#include <algorithm>
#include <initializer_list>
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

static void emit(std::vector<uint32_t> &words, spv::Op op, std::initializer_list<uint32_t> args) {
    words.push_back(uint32_t(args.size() + 1) << 16 | op);
    words.insert(words.end(), args);
}
static void text(std::vector<uint32_t> &words, spv::Op op, std::initializer_list<uint32_t> args,
                 const std::string &value, std::initializer_list<uint32_t> tail = {}) {
    std::vector<uint32_t> instruction(args);
    const auto start = instruction.size();
    instruction.resize(start + value.size() / 4 + 1, 0);
    for (size_t i = 0; i < value.size(); ++i)
        instruction[start + i / 4] |= uint32_t(uint8_t(value[i])) << (8 * (i % 4));
    instruction.insert(instruction.end(), tail);
    words.push_back(uint32_t(instruction.size() + 1) << 16 | op);
    words.insert(words.end(), instruction.begin(), instruction.end());
}

// Slang normally emits a direct output; GLSL emits a gl_PerVertex block.
// Both fixtures write the same executable position without an Invariant mark.
static std::vector<uint32_t> vertex(bool block) {
    std::vector<uint32_t> words{spv::MagicNumber, 0x10000, 0, block ? 17u : 12u, 0};
    emit(words, spv::OpCapability, {spv::CapabilityShader});
    emit(words, spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
    text(words, spv::OpEntryPoint, {spv::ExecutionModelVertex, 7}, "main", {6});
    text(words, spv::OpName, {6}, block ? "vertex_output" : "gl_Position");
    if (block) {
        text(words, spv::OpMemberName, {12, 0}, "gl_Position");
        emit(words, spv::OpDecorate, {12, spv::DecorationBlock});
        emit(words, spv::OpMemberDecorate, {12, 0, spv::DecorationBuiltIn, spv::BuiltInPosition});
        emit(words, spv::OpMemberDecorate, {12, 1, spv::DecorationBuiltIn, spv::BuiltInPointSize});
    } else
        emit(words, spv::OpDecorate, {6, spv::DecorationBuiltIn, spv::BuiltInPosition});
    emit(words, spv::OpTypeVoid, {1});
    emit(words, spv::OpTypeFunction, {2, 1});
    emit(words, spv::OpTypeFloat, {3, 32});
    emit(words, spv::OpTypeVector, {4, 3, 4});
    if (block) {
        emit(words, spv::OpTypeStruct, {12, 4, 3});
        emit(words, spv::OpTypeInt, {13, 32, 0});
        emit(words, spv::OpConstant, {13, 14, 0});
        emit(words, spv::OpTypePointer, {15, spv::StorageClassOutput, 4});
    }
    emit(words, spv::OpTypePointer, {5, spv::StorageClassOutput, block ? 12u : 4u});
    emit(words, spv::OpConstant, {3, 9, 0});
    emit(words, spv::OpConstant, {3, 10, 0x3f800000});
    emit(words, spv::OpConstantComposite, {4, 11, 9, 9, 9, 10});
    emit(words, spv::OpVariable, {5, 6, spv::StorageClassOutput});
    emit(words, spv::OpFunction, {1, 7, 0, 2});
    emit(words, spv::OpLabel, {8});
    if (block)
        emit(words, spv::OpAccessChain, {15, 16, 6, 14});
    emit(words, spv::OpStore, {block ? 16u : 6u, 11});
    emit(words, spv::OpReturn, {});
    emit(words, spv::OpFunctionEnd, {});
    return words;
}

static void check_position(bool block) {
    const auto original = vertex(block);
    spvtools::SpirvTools validator(SPV_ENV_UNIVERSAL_1_0);
    validator.SetMessageConsumer([](spv_message_level_t, const char *, const spv_position_t &,
                                    const char *text) { std::cerr << text << '\n'; });
    require(validator.Validate(original), "Input fixture must be valid SPIR-V");
    const auto marked = sokol_slang::preserve_vertex_position_invariance(original);
    require(validator.Validate(marked), "Output decoration must preserve valid SPIR-V");
    require(sokol_slang::preserve_vertex_position_invariance(marked) == marked,
            "Repeated normalization must not duplicate Invariant");
    spirv_cross::CompilerMSL compiler(marked);
    if (block) {
        require(compiler.has_member_decoration(12, 0, spv::DecorationInvariant),
                "Block position member must be invariant in emitted Vulkan words");
        require(!compiler.has_member_decoration(12, 1, spv::DecorationInvariant),
                "PointSize must not inherit the position policy");
    } else
        require(compiler.has_decoration(6, spv::DecorationInvariant),
                "Direct position output must be invariant in emitted Vulkan words");
    spirv_cross::CompilerMSL::Options options;
    options.set_msl_version(2, 1);
    compiler.set_msl_options(options);
    const auto source = compiler.compile();
    require(source.find("gl_Position [[position, invariant]]") != std::string::npos,
            "Generated MSL must mark gl_Position, regardless of frontend field names");
    for (auto target : {shdc::Slang::METAL_MACOS, shdc::Slang::METAL_IOS, shdc::Slang::METAL_SIM}) {
        const auto args = metal_compile_arguments(target, source, "input shader.metal",
                                                  "output shader.air", {"-Wno-unused-variable"});
        require(std::count(args.begin(), args.end(), "-w") == 1,
                "Generated Metal commands must suppress warnings on every Apple target");
        require(std::count(args.begin(), args.end(), "-fpreserve-invariance") == 1,
                "The actual native command builder must preserve invariant positions");
        require(args.back() == "-fpreserve-invariance",
                "Native invariance policy follows user arguments");
        require(std::find(args.begin(), args.end(), "input shader.metal") != args.end(),
                "Native command arguments must retain paths containing spaces");
    }
}

int main() {
    try {
        check_position(false);
        check_position(true);
        const auto compute = metal_compile_arguments(
            shdc::Slang::METAL_MACOS, "kernel void main0() {}", "compute.metal", "compute.air", {});
        require(std::count(compute.begin(), compute.end(), "-w") == 1,
                "Generated compute commands must suppress warnings without caller arguments");
        require(std::find(compute.begin(), compute.end(), "-fpreserve-invariance") == compute.end(),
                "Compute shaders must keep their existing compiler policy");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
