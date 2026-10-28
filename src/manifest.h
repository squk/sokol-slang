#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <nlohmann/json.hpp>

using Json = nlohmann::json;

// A Binding links one SPIR-V descriptor to a public name and Sokol slot.
// Manifest owns these records; the compiler borrows them during generation.
struct Binding {
    std::string kind;
    int slot;
    std::string stage;
    uint32_t set;
    uint32_t binding;
    std::string access;
    std::string format;
};

// Manifest validates the embedded schema and cross-resource rules before the
// compiler opens any SPIR-V files. File paths resolve beside the manifest.
struct Manifest {
    Json document;
    std::filesystem::path path;
    std::map<std::string, Binding> bindings;
    static Manifest load(const std::filesystem::path& path);
    std::filesystem::path stage_path(const std::string& stage) const;
};
