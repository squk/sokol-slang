#include "storage_elements.h"
#include "spirv-tools/libspirv.hpp"
#include "spirv.hpp"
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace sokol_slang {
namespace {

struct Instruction {
    size_t offset;
    uint32_t count;
    spv::Op opcode;
};

std::string decode_string(const std::vector<uint32_t> &words, size_t begin, size_t end) {
    std::string text;
    for (size_t i = begin; i < end; ++i)
        for (int byte = 0; byte < 4; ++byte) {
            const char c = char((words[i] >> (8 * byte)) & 0xff);
            if (!c)
                return text;
            text.push_back(c);
        }
    return text;
}

std::vector<uint32_t> make(spv::Op opcode, std::vector<uint32_t> operands,
                           const std::string *text = nullptr) {
    if (text) {
        const size_t start = operands.size();
        operands.resize(start + text->size() / 4 + 1, 0);
        for (size_t i = 0; i < text->size(); ++i)
            operands[start + i / 4] |= uint32_t(uint8_t((*text)[i])) << (8 * (i % 4));
    }
    operands.insert(operands.begin(), uint32_t(operands.size() + 1) << 16 | opcode);
    return operands;
}

bool is_annotation(spv::Op opcode) {
    switch (opcode) {
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

// Opcodes whose result may be a pointer. Each stores the result type in word 1
// and the result id in word 2.
bool may_yield_pointer(spv::Op opcode) {
    switch (opcode) {
    case spv::OpUndef:
    case spv::OpConstantNull:
    case spv::OpFunctionParameter:
    case spv::OpFunctionCall:
    case spv::OpVariable:
    case spv::OpLoad:
    case spv::OpAccessChain:
    case spv::OpInBoundsAccessChain:
    case spv::OpPtrAccessChain:
    case spv::OpInBoundsPtrAccessChain:
    case spv::OpCopyObject:
    case spv::OpCopyLogical:
    case spv::OpSelect:
    case spv::OpPhi:
    case spv::OpBitcast:
        return true;
    default:
        return false;
    }
}

bool is_type(spv::Op opcode) {
    switch (opcode) {
    case spv::OpTypeBool:
    case spv::OpTypeInt:
    case spv::OpTypeFloat:
    case spv::OpTypeVector:
    case spv::OpTypeMatrix:
    case spv::OpTypeArray:
    case spv::OpTypeRuntimeArray:
    case spv::OpTypeStruct:
    case spv::OpTypePointer:
        return true;
    default:
        return false;
    }
}

} // namespace

std::vector<uint32_t> wrap_storage_buffer_elements(std::vector<uint32_t> words) {
    if (words.size() < 5)
        throw std::runtime_error("invalid SPIR-V header");
    std::vector<Instruction> instructions;
    for (size_t pos = 5; pos < words.size();) {
        const uint32_t count = words[pos] >> 16;
        if (!count || count > words.size() - pos)
            throw std::runtime_error("invalid SPIR-V instruction length");
        instructions.push_back({pos, count, spv::Op(words[pos] & 0xffff)});
        pos += count;
    }

    std::map<uint32_t, const Instruction *> types;
    std::map<uint32_t, uint32_t> pointees, result_types, strides;
    std::map<uint32_t, std::string> names;
    std::set<uint32_t> blocks, buffer_blocks;
    std::vector<const Instruction *> variables;
    std::vector<uint32_t> int32_constants;
    size_t names_at = 0, annotations_at = 0, functions_at = 0;
    for (const auto &ins : instructions) {
        const size_t o = ins.offset;
        if (ins.opcode == spv::OpName && ins.count >= 3)
            names.emplace(words[o + 1], decode_string(words, o + 2, o + ins.count));
        if (ins.opcode == spv::OpDecorate && ins.count >= 3) {
            if (words[o + 2] == spv::DecorationBlock)
                blocks.insert(words[o + 1]);
            if (words[o + 2] == spv::DecorationBufferBlock)
                buffer_blocks.insert(words[o + 1]);
            if (words[o + 2] == spv::DecorationArrayStride && ins.count >= 4)
                strides[words[o + 1]] = words[o + 3];
        }
        if (is_type(ins.opcode) && ins.count >= 2)
            types[words[o + 1]] = &ins;
        if (ins.opcode == spv::OpTypePointer && ins.count >= 4)
            pointees[words[o + 1]] = words[o + 3];
        if (may_yield_pointer(ins.opcode) && ins.count >= 3)
            result_types[words[o + 2]] = words[o + 1];
        if (ins.opcode == spv::OpVariable && ins.count >= 4)
            variables.push_back(&ins);
        if (ins.opcode == spv::OpConstant && ins.count == 4 && words[o + 3] == 0) {
            const auto type = types.find(words[o + 1]);
            if (type != types.end() && type->second->opcode == spv::OpTypeInt &&
                words[type->second->offset + 2] == 32)
                int32_constants.push_back(words[o + 2]);
        }
        if (!names_at && (ins.opcode == spv::OpModuleProcessed || is_annotation(ins.opcode)))
            names_at = o;
        if (!annotations_at && is_annotation(ins.opcode))
            annotations_at = o;
        if (!functions_at && ins.opcode == spv::OpFunction)
            functions_at = o;
    }
    auto type_of = [&](uint32_t id) -> const Instruction * {
        const auto found = types.find(id);
        return found == types.end() ? nullptr : found->second;
    };

    // A storage block is a BufferBlock struct in Uniform storage or a Block
    // struct in StorageBuffer storage. Only single-member blocks whose member is
    // a runtime array of non-struct elements are rewritten; other shapes reach
    // the existing record checks unchanged.
    std::map<uint32_t, uint32_t> block_arrays;   // block struct -> runtime array
    std::map<uint32_t, std::string> array_names; // runtime array -> buffer name
    for (const auto *variable : variables) {
        const uint32_t storage = words[variable->offset + 3];
        const auto pointee = pointees.find(words[variable->offset + 1]);
        if (pointee == pointees.end())
            continue;
        const uint32_t block = pointee->second;
        const bool storage_block =
            (storage == spv::StorageClassUniform && buffer_blocks.count(block)) ||
            (storage == spv::StorageClassStorageBuffer && blocks.count(block));
        const auto *block_type = type_of(block);
        if (!storage_block || !block_type || block_type->opcode != spv::OpTypeStruct ||
            block_type->count != 3)
            continue;
        const uint32_t array = words[block_type->offset + 2];
        const auto *array_type = type_of(array);
        if (!array_type || array_type->opcode != spv::OpTypeRuntimeArray)
            continue;
        const auto *element = type_of(words[array_type->offset + 2]);
        if (element && element->opcode == spv::OpTypeStruct)
            continue;
        block_arrays[block] = array;
        const auto name = names.find(words[variable->offset + 2]);
        array_names.emplace(array, name != names.end() && !name->second.empty()
                                       ? name->second
                                       : "%" + std::to_string(words[variable->offset + 2]));
    }
    // Slang shares one runtime array type between buffers of one element type.
    // Skip arrays that any other struct also contains.
    for (const auto &[id, type] : types) {
        if (type->opcode != spv::OpTypeStruct || block_arrays.count(id))
            continue;
        for (uint32_t i = 2; i < type->count; ++i)
            if (array_names.count(words[type->offset + i])) {
                const uint32_t array = words[type->offset + i];
                array_names.erase(array);
                for (auto it = block_arrays.begin(); it != block_arrays.end();)
                    it = it->second == array ? block_arrays.erase(it) : std::next(it);
            }
    }
    if (block_arrays.empty())
        return words;
    if (!names_at || !annotations_at || !functions_at)
        throw std::runtime_error("storage buffer rewrite needs debug, annotation and function "
                                 "sections in the SPIR-V module");

    uint32_t bound = words[3];
    std::map<size_t, std::vector<uint32_t>> inserts;
    std::vector<uint32_t> name_words, decoration_words;
    std::map<uint32_t, uint32_t> wrappers; // element type -> wrapper struct
    std::set<uint32_t> arrays;
    // Visit arrays in module order so each record precedes every array using it.
    std::map<size_t, uint32_t> ordered;
    for (const auto &item : array_names)
        ordered.emplace(type_of(item.first)->offset, item.first);
    for (const auto &item : ordered) {
        const uint32_t array = item.second;
        const auto &buffer = array_names.at(array);
        arrays.insert(array);
        const auto *array_type = type_of(array);
        const uint32_t element = words[array_type->offset + 2];
        const std::string prefix = "storage buffer '" + buffer + "': ";
        const auto *scalar = type_of(element);
        uint32_t components = 1;
        if (scalar && scalar->opcode == spv::OpTypeVector) {
            components = words[scalar->offset + 3];
            scalar = type_of(words[scalar->offset + 2]);
        }
        if (!scalar)
            throw std::runtime_error(prefix + "element type is not declared");
        switch (scalar->opcode) {
        case spv::OpTypeMatrix:
            throw std::runtime_error(prefix + "matrix elements are not supported; use a struct "
                                              "element with vector members");
        case spv::OpTypeArray:
        case spv::OpTypeRuntimeArray:
            throw std::runtime_error(prefix + "array elements are not supported; use a struct "
                                              "element");
        case spv::OpTypeBool:
            throw std::runtime_error(prefix + "bool elements are not supported; use uint");
        case spv::OpTypeInt:
        case spv::OpTypeFloat:
            break;
        default:
            throw std::runtime_error(prefix + "unsupported element type; use a 32-bit int, uint "
                                              "or float scalar or vector, or a struct");
        }
        if (words[scalar->offset + 2] != 32)
            throw std::runtime_error(prefix + "only 32-bit int, uint and float elements are "
                                              "supported");
        if (components < 1 || components > 4)
            throw std::runtime_error(prefix + "vector elements need 1 to 4 components");
        std::string type = scalar->opcode == spv::OpTypeFloat ? "float"
                           : words[scalar->offset + 3]        ? "int"
                                                              : "uint";
        if (components > 1)
            type += std::to_string(components);
        // std430: vec3 aligns to 16 bytes, so its array stride is 16. The
        // wrapper record then reflects the same 16-byte size with 4 bytes of
        // trailing padding, matching the C struct Sokol generates.
        const uint32_t expected = components == 3 ? 16 : 4 * components;
        const auto stride = strides.find(array);
        if (stride == strides.end())
            throw std::runtime_error(prefix + "runtime array has no ArrayStride");
        if (stride->second != expected)
            throw std::runtime_error(prefix + type + " elements need the std430 stride " +
                                     std::to_string(expected) + ", but the SPIR-V stride is " +
                                     std::to_string(stride->second));
        auto wrapper = wrappers.find(element);
        if (wrapper == wrappers.end()) {
            // The array follows its element type, so the record declared just
            // before the first such array is in scope for every later one.
            const uint32_t id = bound++;
            wrapper = wrappers.emplace(element, id).first;
            const auto record = make(spv::OpTypeStruct, {id, element});
            inserts[array_type->offset].insert(inserts[array_type->offset].end(), record.begin(),
                                               record.end());
            const std::string record_name = "sokol_" + type + "_record", member = "value";
            for (const auto &item :
                 {make(spv::OpName, {id}, &record_name), make(spv::OpMemberName, {id, 0}, &member)})
                name_words.insert(name_words.end(), item.begin(), item.end());
            const auto decoration = make(spv::OpMemberDecorate, {id, 0, spv::DecorationOffset, 0});
            decoration_words.insert(decoration_words.end(), decoration.begin(), decoration.end());
        }
        words[array_type->offset + 2] = wrapper->second;
    }
    // Insertion order within one offset is names, then decorations.
    inserts[names_at].insert(inserts[names_at].end(), name_words.begin(), name_words.end());
    inserts[annotations_at].insert(inserts[annotations_at].end(), decoration_words.begin(),
                                   decoration_words.end());

    uint32_t zero = int32_constants.empty() ? 0 : int32_constants.front();
    if (!zero) {
        uint32_t int_type = 0;
        for (const auto &[id, type] : types)
            if (type->opcode == spv::OpTypeInt && words[type->offset + 2] == 32) {
                int_type = id;
                break;
            }
        auto &tail = inserts[functions_at];
        if (!int_type) {
            int_type = bound++;
            const auto declaration = make(spv::OpTypeInt, {int_type, 32, 0});
            tail.insert(tail.end(), declaration.begin(), declaration.end());
        }
        zero = bound++;
        const auto constant = make(spv::OpConstant, {int_type, zero, 0});
        tail.insert(tail.end(), constant.begin(), constant.end());
    }

    // Pointer result types stay valid because the block and runtime array keep
    // their ids. A chain walking from a block or array pointer past the array
    // index now stops at the record, so add member index 0 to reach the value.
    auto pointer_state = [&](uint32_t pointer) {
        const auto type = result_types.find(pointer);
        if (type == result_types.end())
            return -1;
        const auto pointee = pointees.find(type->second);
        if (pointee == pointees.end())
            return -1;
        return block_arrays.count(pointee->second) ? 0 : arrays.count(pointee->second) ? 1 : -1;
    };
    std::map<size_t, size_t> chain_patches; // chain offset -> word after which 0 goes
    for (const auto &ins : instructions) {
        const bool pointer_chain =
            ins.opcode == spv::OpPtrAccessChain || ins.opcode == spv::OpInBoundsPtrAccessChain;
        if (ins.opcode != spv::OpAccessChain && ins.opcode != spv::OpInBoundsAccessChain &&
            !pointer_chain)
            continue;
        if (ins.count < 4)
            throw std::runtime_error("invalid SPIR-V access chain");
        int state = pointer_state(words[ins.offset + 3]);
        if (state < 0)
            continue;
        // Element indices of pointer chains step whole blocks or arrays.
        for (size_t i = ins.offset + (pointer_chain ? 5 : 4); i < ins.offset + ins.count; ++i) {
            if (state == 0) {
                state = 1;
                continue;
            }
            chain_patches[ins.offset] = i;
            break;
        }
    }

    std::vector<uint32_t> output(words.begin(), words.begin() + 5);
    output.reserve(words.size() + chain_patches.size() + 64);
    for (const auto &ins : instructions) {
        const auto insert = inserts.find(ins.offset);
        if (insert != inserts.end())
            output.insert(output.end(), insert->second.begin(), insert->second.end());
        const auto begin = words.begin() + ins.offset, end = begin + ins.count;
        const auto patch = chain_patches.find(ins.offset);
        if (patch == chain_patches.end()) {
            output.insert(output.end(), begin, end);
            continue;
        }
        const auto split = words.begin() + patch->second + 1;
        output.push_back((ins.count + 1) << 16 | ins.opcode);
        output.insert(output.end(), begin + 1, split);
        output.push_back(zero);
        output.insert(output.end(), split, end);
    }
    output[3] = bound;

    // Validate against the version the module declares. Ray query modules are 1.5.
    spv_target_env environment = SPV_ENV_UNIVERSAL_1_3;
    if (output[1] >= 0x00010600u)
        environment = SPV_ENV_UNIVERSAL_1_6;
    else if (output[1] >= 0x00010500u)
        environment = SPV_ENV_UNIVERSAL_1_5;
    else if (output[1] >= 0x00010400u)
        environment = SPV_ENV_UNIVERSAL_1_4;
    spvtools::SpirvTools validator(environment);
    std::string errors;
    validator.SetMessageConsumer(
        [&](spv_message_level_t, const char *, const spv_position_t &, const char *message) {
            errors += std::string(message) + "\n";
        });
    if (!validator.Validate(output))
        throw std::runtime_error("storage buffer element rewrite produced invalid SPIR-V: " +
                                 errors);
    return output;
}

} // namespace sokol_slang
