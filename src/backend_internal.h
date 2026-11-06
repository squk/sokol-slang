#pragma once
#include "backend_compile.h"

// File-based CLI adapters. Runtime callers use compile_backend instead.
struct GeneratedProgram {
    std::string header;
    Json report;
};
Json inspect_spirv(const std::string &path);
GeneratedProgram
generate_program(const Manifest &manifest, const std::map<std::string, std::string> &options,
                 const std::filesystem::path &native_work, const std::string &output_name,
                 bool batch, const std::map<std::string, std::vector<uint32_t>> *captured = nullptr,
                 sokol_slang::ProgramArtifact *artifact = nullptr);
