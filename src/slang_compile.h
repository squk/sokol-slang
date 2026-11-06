#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sokol_slang {

// One call owns a Slang session. Callers may compile separate requests on
// separate threads; no Slang pointers or renderer objects cross this boundary.
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
};

struct StageCode {
    std::string entry_point;
    std::vector<uint32_t> spirv;
};

struct CompilerDiagnostic {
    // Slang's text retains source paths, line numbers and diagnostic IDs.
    std::string phase;
    std::string text;
};

// Owns every byte after the Slang session ends. This is frontend SPIR-V and
// Slang reflection, before Sokol binding allocation and backend translation.
// It is not a cooked Sokol program artifact.
struct SourceResult {
    bool succeeded = false;
    std::string compiler_identity;
    std::vector<StageCode> stages;
    std::string reflection_json;
    std::vector<std::string> dependency_paths;
    std::vector<CompilerDiagnostic> diagnostics;
};

SourceResult compile_source(const SourceRequest &request);

} // namespace sokol_slang
