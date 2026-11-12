#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace sokol_slang {

// Each call owns a fresh Slang session and captured files. The calling thread
// reuses its own global session for standard-library startup; globals never
// cross threads. No Slang pointers or renderer objects cross this boundary.
struct SourceRequest {
    std::string module_name;
    std::string source_path;
    std::string source;
    std::vector<std::string> entry_points;
    // Search paths address this captured file map, never the host filesystem.
    // The caller keeps the request unchanged until compile_source returns.
    std::map<std::string, std::string> files;
    std::vector<std::string> search_paths;
    std::string profile = "spirv_1_3";
    // Slang capability names added to the session, such as spvRayQueryKHR.
    std::vector<std::string> capabilities;
    // Session macros apply to the root, captured includes and imported modules.
    std::map<std::string, std::string> defines;
};

struct StageCode {
    std::string stage;
    std::string entry_point;
    std::vector<uint32_t> spirv;
};

struct CompilerDiagnostic {
    // Slang's text retains source paths, line numbers and diagnostic IDs.
    std::string phase;
    std::string text;
};

// Engine-neutral declaration metadata copied while the Slang session lives.
// Unknown type kinds retain their qualified spelling; no SDK pointers escape.
struct SourceType {
    enum class Kind {
        Unknown,
        Scalar,
        Vector,
        Matrix,
        Struct,
        Array,
        ConstantBuffer,
        ParameterBlock,
        Resource,
        Sampler,
        Enum,
        Interface,
        Pointer
    };
    Kind kind = Kind::Unknown;
    std::string qualified_name, scalar_type, element_type, resource_result_type;
    std::string resource_shape, resource_access;
    uint32_t rows = 0, columns = 0;
    bool resource_array = false, resource_multisampled = false;
};

struct SourceAttributeArgument {
    enum class Kind { Int32, UInt32, Bool, Float32, String, EnumInt32 };
    Kind kind = Kind::Int32;
    std::string type_name;
    std::variant<int32_t, uint32_t, bool, float, std::string> value = int32_t{0};
};
struct SourceAttribute {
    std::string name;
    std::vector<SourceAttributeArgument> arguments;
};
struct SourceDeclaration {
    enum class Kind { Unknown, Struct, Variable, Function, Namespace, Enum, Generic };
    Kind kind = Kind::Unknown;
    // path includes module and enclosing declaration names. Function paths
    // include parameter types so overloaded declarations remain distinct.
    std::string module, path, name;
    SourceType type;
    std::vector<SourceAttribute> attributes;
};

// Owns every byte after the Slang session ends. This is frontend SPIR-V and
// Slang reflection, before Sokol binding allocation and backend translation.
// It is not a cooked Sokol program artifact.
struct SourceResult {
    static constexpr uint32_t declaration_format_version = 1;
    bool succeeded = false;
    std::string compiler_identity;
    std::vector<StageCode> stages;
    std::string reflection_json;
    // Includes unreferenced declarations exposed by module reflection. This is
    // not a per-entry reachability or optimized instance-buffer access report.
    std::vector<SourceDeclaration> declarations;
    std::vector<std::string> dependency_paths;
    std::vector<CompilerDiagnostic> diagnostics;
};

SourceResult compile_source(const SourceRequest &request);

} // namespace sokol_slang
