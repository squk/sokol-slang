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
    // Host Metal/D3D targets compile native bytecode unless source is requested.
    bool source_only = false;
    // Arguments passed verbatim to the host Metal compiler, in order.
    std::vector<std::string> metal_args;
};

// Throws std::runtime_error on invalid SPIR-V, policy, limits or interfaces.
// Runs CPU translation synchronously on a private 8 MiB stack and joins before
// returning. The caller bounds concurrent calls; this function creates no GPU data.
// Native tools use a private directory removed before return, including failures.
// Thread setup failures throw std::system_error.
ProgramArtifact compile_backend(const BackendRequest &request);
} // namespace sokol_slang
