#pragma once
#include "manifest.h"
#include <cstdint>
#include <vector>

namespace sokol_slang {
struct BackendStage {
    std::string target;
    std::string stage;
    std::string entry;
    std::string profile;
    std::string source;
    std::vector<uint8_t> bytecode;
};

// A program owns backend code and reflection. No generated C++ declarations
// or Slang/Sokol pointers are needed to retain or serialize it.
struct ProgramArtifact {
    static constexpr uint32_t format_version = 2;
    Json reflection;
    Json layout;
    std::vector<BackendStage> stages;
    Json to_json() const;
};

struct BackendRequest {
    // Uses the existing manifest's program, entry, binding and sampler policy
    // fields. Stage file fields are supplied internally from these owned words.
    Json manifest;
    std::map<std::string, std::vector<uint32_t>> stages;
    std::string targets;
};

// Throws std::runtime_error on invalid SPIR-V, policy, limits or interfaces.
// Reuses the CLI's binding allocator and backend translator; creates no GPU data.
ProgramArtifact compile_backend(const BackendRequest &request);
} // namespace sokol_slang
