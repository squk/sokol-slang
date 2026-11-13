#include "slang_compile.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <slang-com-ptr.h>
#include <slang.h>
#include <stdexcept>

namespace sokol_slang {
namespace {
bool same_guid(const SlangUUID &a, const SlangUUID &b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

// Slang retains blobs and its filesystem through COM references. Neither
// object borrows the request, so backend work cannot outlive its source bytes.
class SourceBlob final : public ISlangBlob {
  public:
    explicit SourceBlob(std::string bytes) : bytes_(std::move(bytes)) {}
    SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(const SlangUUID &id,
                                                          void **out) override {
        *out = nullptr;
        if (!same_guid(id, ISlangUnknown::getTypeGuid()) &&
            !same_guid(id, ISlangBlob::getTypeGuid()))
            return SLANG_E_NO_INTERFACE;
        *out = static_cast<ISlangBlob *>(this);
        addRef();
        return SLANG_OK;
    }
    SLANG_NO_THROW uint32_t SLANG_MCALL addRef() override { return ++references_; }
    SLANG_NO_THROW uint32_t SLANG_MCALL release() override {
        const auto remaining = references_.fetch_sub(1) - 1;
        if (!remaining)
            delete this;
        return remaining;
    }
    SLANG_NO_THROW const void *SLANG_MCALL getBufferPointer() override { return bytes_.c_str(); }
    SLANG_NO_THROW size_t SLANG_MCALL getBufferSize() override { return bytes_.size(); }

  private:
    std::atomic<uint32_t> references_{1};
    std::string bytes_;
};

std::string source_path(const std::string &path) {
    return std::filesystem::path(path).lexically_normal().generic_string();
}

// SourceFiles serves captured bytes to Slang and records the source keys it loads.
// Loads may overlap; the dependency snapshot uses the same lock as loadFile.
class SourceFiles final : public ISlangFileSystem {
  public:
    explicit SourceFiles(std::map<std::string, std::string> files) : files_(std::move(files)) {}
    SLANG_NO_THROW void *SLANG_MCALL castAs(const SlangUUID &id) override {
        if (same_guid(id, ISlangUnknown::getTypeGuid()) ||
            same_guid(id, ISlangCastable::getTypeGuid()) ||
            same_guid(id, ISlangFileSystem::getTypeGuid()))
            return static_cast<ISlangFileSystem *>(this);
        return nullptr;
    }
    SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(const SlangUUID &id,
                                                          void **out) override {
        *out = castAs(id);
        if (!*out)
            return SLANG_E_NO_INTERFACE;
        addRef();
        return SLANG_OK;
    }
    SLANG_NO_THROW uint32_t SLANG_MCALL addRef() override { return ++references_; }
    SLANG_NO_THROW uint32_t SLANG_MCALL release() override {
        const auto remaining = references_.fetch_sub(1) - 1;
        if (!remaining)
            delete this;
        return remaining;
    }
    bool contains(const char *path) const { return path && files_.contains(source_path(path)); }
    std::vector<std::string> loaded_paths() const {
        const std::lock_guard lock(loaded_mutex_);
        return {loaded_.begin(), loaded_.end()};
    }
    SLANG_NO_THROW SlangResult SLANG_MCALL loadFile(const char *path, ISlangBlob **out) override {
        *out = nullptr;
        const auto found = files_.find(source_path(path));
        if (found == files_.end())
            return SLANG_E_NOT_FOUND;
        {
            const std::lock_guard lock(loaded_mutex_);
            loaded_.insert(found->first);
        }
        *out = new SourceBlob(found->second);
        return SLANG_OK;
    }

  private:
    std::atomic<uint32_t> references_{1};
    const std::map<std::string, std::string> files_;
    mutable std::mutex loaded_mutex_;
    std::set<std::string> loaded_;
};

std::string blob_text(ISlangBlob *blob) {
    if (!blob || !blob->getBufferSize())
        return {};
    return {static_cast<const char *>(blob->getBufferPointer()), blob->getBufferSize()};
}

std::string type_name(slang::TypeReflection *type) {
    if (!type)
        return {};
    Slang::ComPtr<ISlangBlob> name;
    if (SLANG_SUCCEEDED(type->getFullName(name.writeRef())) && name)
        return blob_text(name.get());
    return type->getName() ? type->getName() : "";
}

SourceType describe_type(slang::TypeReflection *type) {
    SourceType result;
    if (!type)
        return result;
    result.qualified_name = type_name(type);
    using K = slang::TypeReflection::Kind;
    using O = SourceType::Kind;
    switch (type->getKind()) {
    case K::Scalar:
        result.kind = O::Scalar;
        break;
    case K::Vector:
        result.kind = O::Vector;
        break;
    case K::Matrix:
        result.kind = O::Matrix;
        break;
    case K::Struct:
        result.kind = O::Struct;
        break;
    case K::Array:
        result.kind = O::Array;
        break;
    case K::ConstantBuffer:
        result.kind = O::ConstantBuffer;
        break;
    case K::ParameterBlock:
        result.kind = O::ParameterBlock;
        break;
    case K::Resource:
        result.kind = O::Resource;
        break;
    case K::SamplerState:
        result.kind = O::Sampler;
        break;
    case K::Enum:
        result.kind = O::Enum;
        break;
    case K::Interface:
        result.kind = O::Interface;
        break;
    case K::Pointer:
        result.kind = O::Pointer;
        break;
    default:
        break;
    }
    using S = slang::TypeReflection::ScalarType;
    switch (type->getScalarType()) {
    case S::Void:
        result.scalar_type = "void";
        break;
    case S::Bool:
        result.scalar_type = "bool";
        break;
    case S::Int8:
        result.scalar_type = "int8";
        break;
    case S::UInt8:
        result.scalar_type = "uint8";
        break;
    case S::Int16:
        result.scalar_type = "int16";
        break;
    case S::UInt16:
        result.scalar_type = "uint16";
        break;
    case S::Int32:
        result.scalar_type = "int32";
        break;
    case S::UInt32:
        result.scalar_type = "uint32";
        break;
    case S::Int64:
        result.scalar_type = "int64";
        break;
    case S::UInt64:
        result.scalar_type = "uint64";
        break;
    case S::Float16:
        result.scalar_type = "float16";
        break;
    case S::Float32:
        result.scalar_type = "float32";
        break;
    case S::Float64:
        result.scalar_type = "float64";
        break;
    default:
        break;
    }
    if (type->getKind() == K::Scalar || type->getKind() == K::Vector ||
        type->getKind() == K::Matrix) {
        result.rows = type->getRowCount();
        result.columns = type->getColumnCount();
    }
    if (type->getKind() == K::Array || type->getKind() == K::ConstantBuffer ||
        type->getKind() == K::ParameterBlock || type->getKind() == K::Pointer)
        result.element_type = type_name(type->getElementType());
    if (type->getKind() == K::Resource) {
        result.resource_result_type = type_name(type->getResourceResultType());
        const auto shape = type->getResourceShape();
        switch (shape & SLANG_RESOURCE_BASE_SHAPE_MASK) {
        case SLANG_TEXTURE_1D:
            result.resource_shape = "texture_1d";
            break;
        case SLANG_TEXTURE_2D:
            result.resource_shape = "texture_2d";
            break;
        case SLANG_TEXTURE_3D:
            result.resource_shape = "texture_3d";
            break;
        case SLANG_TEXTURE_CUBE:
            result.resource_shape = "texture_cube";
            break;
        case SLANG_TEXTURE_BUFFER:
            result.resource_shape = "texture_buffer";
            break;
        case SLANG_STRUCTURED_BUFFER:
            result.resource_shape = "structured_buffer";
            break;
        case SLANG_BYTE_ADDRESS_BUFFER:
            result.resource_shape = "byte_address_buffer";
            break;
        case SLANG_ACCELERATION_STRUCTURE:
            result.resource_shape = "acceleration_structure";
            break;
        case SLANG_TEXTURE_SUBPASS:
            result.resource_shape = "texture_subpass";
            break;
        default:
            result.resource_shape = "unknown";
            break;
        }
        result.resource_array = (shape & SLANG_TEXTURE_ARRAY_FLAG) != 0;
        result.resource_multisampled = (shape & SLANG_TEXTURE_MULTISAMPLE_FLAG) != 0;
        switch (type->getResourceAccess()) {
        case SLANG_RESOURCE_ACCESS_READ:
            result.resource_access = "read";
            break;
        case SLANG_RESOURCE_ACCESS_READ_WRITE:
            result.resource_access = "read_write";
            break;
        case SLANG_RESOURCE_ACCESS_RASTER_ORDERED:
            result.resource_access = "raster_ordered";
            break;
        case SLANG_RESOURCE_ACCESS_APPEND:
            result.resource_access = "append";
            break;
        case SLANG_RESOURCE_ACCESS_CONSUME:
            result.resource_access = "consume";
            break;
        case SLANG_RESOURCE_ACCESS_WRITE:
            result.resource_access = "write";
            break;
        case SLANG_RESOURCE_ACCESS_FEEDBACK:
            result.resource_access = "feedback";
            break;
        default:
            result.resource_access = "unknown";
            break;
        }
    }
    return result;
}

// These types borrow the Slang session and exist only while copying declarations.
// Imported enum constants can lack an argument type; the attribute's declared
// fields still give the compiler's type, without guessing from its value getter.
using AttributeSignatures = std::map<std::string, std::set<slang::TypeReflection *>>;

AttributeSignatures attribute_signatures(const std::vector<slang::IModule *> &modules) {
    AttributeSignatures signatures;
    for (auto *module : modules) {
        std::set<slang::DeclReflection *> visited;
        std::function<void(slang::DeclReflection *, const std::string &)> walk;
        walk = [&](slang::DeclReflection *decl, const std::string &parent) {
            if (!decl || !visited.insert(decl).second)
                return;
            const std::string name = decl->getName() ? decl->getName() : "";
            const std::string path = parent.empty() ? name : parent + "::" + name;
            if (decl->getKind() == slang::DeclReflection::Kind::Struct &&
                name.ends_with("Attribute")) {
                if (auto *type = decl->getType()) {
                    for (const auto &alias : {name, path, type_name(type)}) {
                        if (alias.empty())
                            continue;
                        signatures[alias].insert(type);
                        if (alias.ends_with("Attribute"))
                            signatures[alias.substr(0, alias.size() - 9)].insert(type);
                    }
                }
            }
            for (unsigned i = 0; i < decl->getChildrenCount(); ++i)
                walk(decl->getChild(i),
                     decl->getKind() == slang::DeclReflection::Kind::Module
                         ? std::string(module->getName() ? module->getName() : "")
                         : path);
        };
        walk(module->getModuleReflection(), "");
    }
    return signatures;
}

slang::TypeReflection *declared_argument_type(slang::Attribute *attribute, uint32_t index,
                                              const AttributeSignatures &signatures) {
    const auto found = signatures.find(attribute->getName());
    if (found == signatures.end())
        throw std::runtime_error("Attribute argument type and declaration are unavailable");
    if (found->second.size() != 1)
        throw std::runtime_error("Attribute declaration name is ambiguous");
    auto *type = *found->second.begin();
    if (index >= type->getFieldCount())
        throw std::runtime_error("Attribute argument has no declared field");
    auto *field = type->getFieldByIndex(index);
    if (!field || !field->getType())
        throw std::runtime_error("Attribute field type is unavailable");
    return field->getType();
}

SourceAttributeArgument attribute_argument(slang::Attribute *attribute, uint32_t index,
                                           const AttributeSignatures &signatures) {
    auto *type = attribute->getArgumentType(index);
    if (!type)
        type = declared_argument_type(attribute, index, signatures);
    SourceAttributeArgument result;
    result.type_name = type_name(type);
    // This getter returns null for every non-string argument; it does not
    // stringify numbers. Copy the explicit length, including embedded nulls.
    size_t length = 0;
    if (const char *value = attribute->getArgumentValueString(index, &length)) {
        result.kind = SourceAttributeArgument::Kind::String;
        result.value = std::string(value, length);
        return result;
    }
    using S = slang::TypeReflection::ScalarType;
    const auto scalar = type->getScalarType();
    const bool enumeration = type->getKind() == slang::TypeReflection::Kind::Enum;
    if (enumeration && scalar == S::UInt32)
        throw std::runtime_error("Unsigned enum attribute arguments are unsupported: " +
                                 result.type_name);
    if (type->getKind() != slang::TypeReflection::Kind::Scalar && !enumeration)
        throw std::runtime_error("Unsupported attribute argument type: " + result.type_name);
    if (scalar == S::Float32 && !enumeration) {
        float value;
        if (SLANG_FAILED(attribute->getArgumentValueFloat(index, &value)))
            throw std::runtime_error("Cannot read float32 attribute argument");
        result.kind = SourceAttributeArgument::Kind::Float32;
        result.value = value;
        return result;
    }
    if (scalar != S::Bool && scalar != S::Int32 && scalar != S::UInt32 &&
        !(enumeration && scalar == S::None))
        throw std::runtime_error("Unsupported attribute argument type: " + result.type_name);
    int value;
    if (SLANG_FAILED(attribute->getArgumentValueInt(index, &value)))
        throw std::runtime_error("Cannot read int32 attribute argument");
    if (enumeration) {
        result.kind = SourceAttributeArgument::Kind::EnumInt32;
        result.value = int32_t(value);
    } else if (scalar == S::Bool) {
        if (value != 0 && value != 1)
            throw std::runtime_error("Invalid bool attribute argument");
        result.kind = SourceAttributeArgument::Kind::Bool;
        result.value = value != 0;
    } else if (scalar == S::UInt32) {
        result.kind = SourceAttributeArgument::Kind::UInt32;
        result.value = uint32_t(value);
    } else {
        result.kind = SourceAttributeArgument::Kind::Int32;
        result.value = int32_t(value);
    }
    return result;
}

template <typename Reflection>
std::vector<SourceAttribute> describe_attributes(Reflection *reflection, const std::string &path,
                                                 const AttributeSignatures &signatures) {
    std::vector<SourceAttribute> result;
    for (unsigned index = 0; index < reflection->getUserAttributeCount(); ++index) {
        auto *attribute = reflection->getUserAttributeByIndex(index);
        if (!attribute || !attribute->getName())
            throw std::runtime_error(path + ": invalid user attribute");
        SourceAttribute output;
        output.name = attribute->getName();
        try {
            for (uint32_t argument = 0; argument < attribute->getArgumentCount(); ++argument)
                output.arguments.push_back(attribute_argument(attribute, argument, signatures));
        } catch (const std::exception &error) {
            throw std::runtime_error(path + " [" + output.name + "]: " + error.what());
        }
        result.push_back(std::move(output));
    }
    return result;
}

std::vector<SourceDeclaration> source_declarations(slang::ISession *session, slang::IModule *root,
                                                   const SourceFiles &files) {
    std::vector<slang::IModule *> captured_modules{root};
    for (SlangInt i = 0; i < session->getLoadedModuleCount(); ++i) {
        auto *module = session->getLoadedModule(i);
        if (module && module != root && files.contains(module->getFilePath()) &&
            std::find(captured_modules.begin(), captured_modules.end(), module) ==
                captured_modules.end())
            captured_modules.push_back(module);
    }
    // Imported modules can report a resolved path unlike their captured key.
    // Their declarations still supply authoritative attribute parameter types.
    std::vector<slang::IModule *> signature_modules = captured_modules;
    for (SlangInt i = 0; i < session->getLoadedModuleCount(); ++i) {
        auto *module = session->getLoadedModule(i);
        if (module && std::find(signature_modules.begin(), signature_modules.end(), module) ==
                          signature_modules.end())
            signature_modules.push_back(module);
    }
    const auto signatures = attribute_signatures(signature_modules);
    std::vector<SourceDeclaration> output;
    std::set<std::string> variables;
    const auto variable = [&](slang::VariableReflection *value, const std::string &module,
                              const std::string &parent) {
        if (!value || !value->getName())
            return;
        SourceDeclaration declaration;
        declaration.kind = SourceDeclaration::Kind::Variable;
        declaration.module = module;
        declaration.name = value->getName();
        declaration.path = parent + "::" + declaration.name;
        if (!variables.insert(declaration.path).second)
            return;
        declaration.type = describe_type(value->getType());
        declaration.attributes = describe_attributes(value, declaration.path, signatures);
        output.push_back(std::move(declaration));
    };
    std::set<slang::IModule *> modules;
    const auto inspect = [&](slang::IModule *module) {
        if (!module || !modules.insert(module).second)
            return;
        if (module != root && !files.contains(module->getFilePath()))
            return;
        const std::string module_name = module->getName() ? module->getName() : "";
        std::set<slang::DeclReflection *> visited;
        std::function<void(slang::DeclReflection *, const std::string &)> walk;
        walk = [&](slang::DeclReflection *decl, const std::string &parent) {
            if (!decl || !visited.insert(decl).second)
                return;
            const auto kind = decl->getKind();
            if (kind == slang::DeclReflection::Kind::Module) {
                for (unsigned i = 0; i < decl->getChildrenCount(); ++i)
                    walk(decl->getChild(i), parent);
                return;
            }
            if (auto *value = decl->asVariable()) {
                variable(value, module_name, parent);
                return;
            }
            SourceDeclaration declaration;
            declaration.module = module_name;
            declaration.name = decl->getName() ? decl->getName() : "";
            if (declaration.name.empty())
                return;
            declaration.path = parent + "::" + declaration.name;
            if (auto *function = decl->asFunction()) {
                declaration.kind = SourceDeclaration::Kind::Function;
                declaration.type = describe_type(function->getReturnType());
                declaration.path += "(";
                for (unsigned i = 0; i < function->getParameterCount(); ++i) {
                    if (i)
                        declaration.path += ",";
                    declaration.path += type_name(function->getParameterByIndex(i)->getType());
                }
                declaration.path += ")";
                declaration.attributes =
                    describe_attributes(function, declaration.path, signatures);
            } else if (kind == slang::DeclReflection::Kind::Namespace) {
                declaration.kind = SourceDeclaration::Kind::Namespace;
            } else if (kind == slang::DeclReflection::Kind::Generic) {
                declaration.kind = SourceDeclaration::Kind::Generic;
            } else if (auto *type = decl->getType()) {
                declaration.type = describe_type(type);
                declaration.attributes = describe_attributes(type, declaration.path, signatures);
                if (kind == slang::DeclReflection::Kind::Struct)
                    declaration.kind = SourceDeclaration::Kind::Struct;
                if (kind == slang::DeclReflection::Kind::Enum)
                    declaration.kind = SourceDeclaration::Kind::Enum;
            }
            const auto path = declaration.path;
            output.push_back(std::move(declaration));
            for (unsigned i = 0; i < decl->getChildrenCount(); ++i)
                walk(decl->getChild(i), path);
            if (kind == slang::DeclReflection::Kind::Struct)
                if (auto *type = decl->getType())
                    for (unsigned i = 0; i < type->getFieldCount(); ++i)
                        variable(type->getFieldByIndex(i), module_name, path);
        };
        walk(module->getModuleReflection(), module_name);
    };
    inspect(root);
    for (SlangInt i = 0; i < session->getLoadedModuleCount(); ++i)
        inspect(session->getLoadedModule(i));
    std::sort(output.begin(), output.end(),
              [](const auto &a, const auto &b) { return a.path < b.path; });
    return output;
}

} // namespace

SourceResult compile_source(const SourceRequest &request) {
    SourceResult result;
    auto fail = [&](const char *phase, const char *text) {
        result.diagnostics.push_back({phase, text});
        return result;
    };
    auto diagnostic = [&](const char *phase, ISlangBlob *blob) {
        if (blob && blob->getBufferSize())
            result.diagnostics.push_back({phase, blob_text(blob)});
    };
    if (request.module_name.empty() || request.source.find('\0') != std::string::npos)
        return fail("input", "Supply a module name and source without embedded null bytes");
    if (request.defines.size() > static_cast<size_t>(std::numeric_limits<SlangInt>::max()))
        return fail("input", "Too many preprocessor definitions");
    std::vector<slang::PreprocessorMacroDesc> macros;
    macros.reserve(request.defines.size());
    for (const auto &[name, value] : request.defines) {
        const auto letter = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        };
        bool valid = !name.empty() && letter(name.front());
        for (char c : name)
            valid &= letter(c) || (c >= '0' && c <= '9');
        if (!valid) {
            result.diagnostics.push_back(
                {"input", "Invalid preprocessor definition name: " + name});
            return result;
        }
        if (value.find('\0') != std::string::npos) {
            result.diagnostics.push_back(
                {"input", "Preprocessor definition contains a null byte: " + name});
            return result;
        }
        macros.push_back({name.c_str(), value.c_str()});
    }
    std::set<std::string> names;
    for (const auto &name : request.entry_points)
        if (name.empty() || !names.insert(name).second)
            return fail("input", "Entry point names must be nonempty and unique");

    std::map<std::string, std::string> captured;
    for (const auto &[path, bytes] : request.files) {
        if (path.empty() || path.find('\0') != std::string::npos ||
            !captured.emplace(source_path(path), bytes).second)
            return fail("input",
                        "Captured source paths must be nonempty and unique after normalization");
    }
    if (request.source_path.empty() || request.source_path.find('\0') != std::string::npos ||
        !captured.emplace(source_path(request.source_path), request.source).second)
        return fail("input", "The root source needs a unique nonempty path");
    Slang::ComPtr<SourceFiles> files;
    files.attach(new SourceFiles(std::move(captured)));
    // Slang's global session retains the standard library. The SDK requires
    // separate globals for concurrent threads; request sessions stay fresh.
    thread_local Slang::ComPtr<slang::IGlobalSession> global;
    if (!global && SLANG_FAILED(slang::createGlobalSession(global.writeRef())))
        return fail("session", "Cannot create Slang global session");
    result.compiler_identity = global->getBuildTagString();
    slang::TargetDesc target;
    target.format = SLANG_SPIRV;
    target.profile = global->findProfile(request.profile.c_str());
    if (target.profile == SLANG_PROFILE_UNKNOWN)
        return fail("input", "Unknown Slang target profile");
    std::vector<const char *> paths;
    for (const auto &path : request.search_paths)
        paths.push_back(path.c_str());
    // Match the CLI's addressing and matrix contract before Sokol translation.
    std::vector<slang::CompilerOptionEntry> options(3);
    options[0].name = slang::CompilerOptionName::VulkanUseEntryPointName;
    options[1].name = slang::CompilerOptionName::VulkanUseGLLayout;
    options[2].name = slang::CompilerOptionName::DebugInformation;
    for (auto &option : options)
        option.value.intValue0 = 1;
    // Capabilities such as spvRayQueryKHR enable SPIR-V extensions that the
    // target profile alone does not grant. Slang takes capability IDs, not names.
    for (const auto &capability : request.capabilities) {
        const auto id = capability.find('\0') == std::string::npos
                            ? global->findCapability(capability.c_str())
                            : SLANG_CAPABILITY_UNKNOWN;
        if (id == SLANG_CAPABILITY_UNKNOWN)
            return fail("input", ("Unknown Slang capability: " + capability).c_str());
        slang::CompilerOptionEntry option = {};
        option.name = slang::CompilerOptionName::Capability;
        option.value.intValue0 = static_cast<int32_t>(id);
        options.push_back(option);
    }
    slang::SessionDesc desc;
    desc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
    desc.compilerOptionEntries = options.data();
    desc.compilerOptionEntryCount = static_cast<uint32_t>(options.size());
    desc.fileSystem = files.get();
    desc.targets = &target;
    desc.targetCount = 1;
    desc.preprocessorMacros = macros.data();
    desc.preprocessorMacroCount = static_cast<SlangInt>(macros.size());
    desc.searchPaths = paths.data();
    desc.searchPathCount = paths.size();
    Slang::ComPtr<slang::ISession> session;
    if (SLANG_FAILED(global->createSession(desc, session.writeRef())))
        return fail("session", "Cannot create Slang compilation session");
    Slang::ComPtr<ISlangBlob> messages;
    auto *module = session->loadModuleFromSourceString(request.module_name.c_str(),
                                                       request.source_path.c_str(),
                                                       request.source.c_str(), messages.writeRef());
    diagnostic("load", messages);
    if (!module)
        return result;
    try {
        result.declarations = source_declarations(session.get(), module, *files.get());
    } catch (const std::exception &error) {
        return fail("declarations", error.what());
    }
    auto entry_names = request.entry_points;
    if (entry_names.empty()) {
        for (int i = 0; i < module->getDefinedEntryPointCount(); ++i) {
            Slang::ComPtr<slang::IEntryPoint> entry;
            if (SLANG_FAILED(module->getDefinedEntryPoint(i, entry.writeRef())))
                return fail("entry", "Cannot inspect defined entry point");
            entry_names.emplace_back(entry->getFunctionReflection()->getName());
        }
        if (entry_names.empty())
            return fail("entry", "Module defines no shader entry points");
    }
    std::vector<Slang::ComPtr<slang::IEntryPoint>> entries;
    std::vector<slang::IComponentType *> components{module};
    for (const auto &name : entry_names) {
        Slang::ComPtr<slang::IEntryPoint> entry;
        if (SLANG_FAILED(module->findEntryPointByName(name.c_str(), entry.writeRef())))
            return fail("entry", "Requested entry point does not exist");
        components.push_back(entry.get());
        entries.push_back(entry);
    }
    Slang::ComPtr<slang::IComponentType> composite;
    messages.setNull();
    auto status = session->createCompositeComponentType(components.data(), components.size(),
                                                        composite.writeRef(), messages.writeRef());
    diagnostic("compose", messages);
    if (SLANG_FAILED(status))
        return result;
    Slang::ComPtr<slang::IComponentType> linked;
    messages.setNull();
    status = composite->link(linked.writeRef(), messages.writeRef());
    diagnostic("link", messages);
    if (SLANG_FAILED(status))
        return result;
    auto *layout = linked->getLayout(0);
    Slang::ComPtr<ISlangBlob> reflection;
    if (!layout || SLANG_FAILED(layout->toJson(reflection.writeRef())))
        return fail("reflection", "Cannot serialize Slang program reflection");
    result.reflection_json = blob_text(reflection);
    std::vector<StageCode> stages;
    for (size_t i = 0; i < entry_names.size(); ++i) {
        Slang::ComPtr<ISlangBlob> code;
        messages.setNull();
        status = linked->getEntryPointCode(i, 0, code.writeRef(), messages.writeRef());
        diagnostic("code", messages);
        if (SLANG_FAILED(status))
            return result;
        if (!code || !code->getBufferSize() || code->getBufferSize() % sizeof(uint32_t))
            return fail("code", "Slang returned invalid SPIR-V byte length");
        StageCode stage;
        switch (layout->getEntryPointByIndex(i)->getStage()) {
        case SLANG_STAGE_VERTEX:
            stage.stage = "vertex";
            break;
        case SLANG_STAGE_FRAGMENT:
            stage.stage = "fragment";
            break;
        case SLANG_STAGE_COMPUTE:
            stage.stage = "compute";
            break;
        default:
            return fail("entry", "Sokol supports vertex, fragment and compute stages");
        }
        stage.entry_point = entry_names[i];
        stage.spirv.resize(code->getBufferSize() / sizeof(uint32_t));
        std::memcpy(stage.spirv.data(), code->getBufferPointer(), code->getBufferSize());
        stages.push_back(std::move(stage));
    }
    // Slang may report hash-suffixed virtual identities for captured includes.
    // Build dependencies need the actual source paths loaded by SourceFiles.
    result.dependency_paths = files->loaded_paths();
    const auto root_path = source_path(request.source_path);
    if (std::find(result.dependency_paths.begin(), result.dependency_paths.end(), root_path) ==
        result.dependency_paths.end())
        result.dependency_paths.push_back(root_path);
    result.stages = std::move(stages);
    result.succeeded = true;
    return result;
}
} // namespace sokol_slang
