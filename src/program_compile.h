#pragma once
#include "backend_compile.h"
#include "slang_compile.h"
#include <map>

namespace sokol_slang {

struct ProgramSelection {
    // Stage kind to entry name. Policy uses compile_backend's manifest schema.
    std::map<std::string, std::string> entries;
    Json policy;
};

// Captures one source module and a family of programs that share its entries.
// The caller keeps all input bytes unchanged until compile_programs returns.
struct ProgramRequest {
    SourceRequest source;
    std::map<std::string, ProgramSelection> programs;
    std::string targets;
};

struct ProgramResult {
    bool succeeded = false;
    SourceResult source;
    // Empty on any failure. Diagnostics include the failing program name.
    std::map<std::string, ProgramArtifact> programs;
};

// Compiles each distinct entry once, then reuses compile_backend for each program.
// Owns all output, performs no file I/O and creates no GPU objects.
ProgramResult compile_programs(const ProgramRequest &request);

} // namespace sokol_slang
