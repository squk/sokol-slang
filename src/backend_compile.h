#pragma once
#include "manifest.h"
#include <cstdint>
#include <vector>

namespace sokol_slang {
/*
    SPIR-V backend compilation
    ==========================
    Supply one compute stage or a vertex/fragment pair, manifest policy and
    explicit target dialects. Output owns backend code and native binding layout.
    The caller creates GPU objects using that layout. See docs/library.md for
    artifact fields and docs/reference.md for supported targets and limits.
*/

struct BackendStage {
    // Backend dialect, shader stage and native entry/profile, respectively.
    std::string target;
    std::string stage;
    std::string entry;
    std::string profile;
    // Translated source; bytecode is populated when native compilation is used.
    std::string source;
    std::vector<uint8_t> bytecode;
};

// A program owns backend code and reflection. No generated C++ declarations
// or Slang/Sokol pointers are needed to retain or serialize it.
struct ProgramArtifact {
    static constexpr uint32_t format_version = 2;
    // Source SPIR-V reflection and merged native layout serve different lookups.
    Json reflection;
    Json layout;
    std::vector<BackendStage> stages;
    // Serializes version, reflection, layout and all owned backend stages.
    Json to_json() const;
};

struct BackendRequest {
    // Uses the existing manifest's program, entry, binding and sampler policy
    // fields. Stage file fields are supplied internally from these owned words.
    Json manifest;
    // Keys are "vertex"/"fragment" or "compute"; values are SPIR-V words.
    std::map<std::string, std::vector<uint32_t>> stages;
    // Required colon-separated backend dialects, e.g. "glsl410:wgsl".
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
