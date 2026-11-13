#pragma once
#include "backend_compile.h"
#include "slang_compile.h"
#include <map>

namespace sokol_slang {

/*
    Program compilation
    ===================
    Select named vertex/fragment pairs or compute entries from one captured
    source module. Each distinct entry compiles once, then each program receives
    its own backend artifact. Supply explicit targets, e.g. "glsl410:wgsl".
    Raster policy must include {"clip_depth": "zero_to_one"}.

    Check ProgramResult::succeeded before consuming programs. Source compilation
    can succeed while a backend fails; source.succeeded alone is insufficient.
    See docs/library.md for a complete example and artifact layout contracts.
*/

struct ProgramSelection {
    // Exactly {vertex, fragment} or {compute}, with nonempty entry names.
    std::map<std::string, std::string> entries;
    // Manifest policy; module/program/stage data are supplied by compile_programs.
    Json policy;
};

// Captures one source module and a family of programs that share its entries.
// The caller keeps all input bytes unchanged until compile_programs returns.
struct ProgramRequest {
    SourceRequest source;
    std::map<std::string, ProgramSelection> programs;
    // Required colon-separated backend dialects, using the CLI target names.
    std::string targets;
    // Match BackendRequest: native host bytecode by default, source on request.
    bool source_only = false;
};

struct ProgramResult {
    bool succeeded = false;
    SourceResult source;
    // Empty on any failure. Diagnostics include the failing program name.
    std::map<std::string, ProgramArtifact> programs;
};

// Compiles each distinct entry once, then reuses compile_backend for each program.
// Owns all output and reads shader inputs from captured bytes. Backend tools
// may use temporary files. Creates no GPU objects.
// Caught std::exception failures return diagnostics and an empty program map.
ProgramResult compile_programs(const ProgramRequest &request);

// Reuses owned frontend SPIR-V and metadata without compiling source again.
// module_name supplies the same backend identity used by ProgramRequest.
// Each selected stage/entry must exist in source; source is retained in the result.
ProgramResult compile_programs(SourceResult source,
                               const std::map<std::string, ProgramSelection> &programs,
                               const std::string &targets, const std::string &module_name,
                               bool source_only = false);

} // namespace sokol_slang
