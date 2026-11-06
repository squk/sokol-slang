#pragma once
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <string>

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

// Read validates the input contract; from_document validates the core's resolved
// resources. File paths resolve beside the manifest.
struct Manifest {
    Json document;
    std::filesystem::path path;
    std::map<std::string, Binding> bindings;
    static void validate_document(const Json &document);
    static Json read(const std::filesystem::path &path);
    static Manifest from_document(Json document, const std::filesystem::path &path);
    std::filesystem::path stage_path(const std::string &stage) const;
};
