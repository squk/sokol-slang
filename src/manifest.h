#pragma once
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <string>

using Json = nlohmann::json;

/*
    Manifest input
    ==============
    Version 1 describes a raster/compute program or program set. The JSON schema
    in schema/manifest.schema.json defines authored fields; resource discovery
    resolves them into the single-program form consumed by from_document.
    See docs/reference.md for the SPIR-V file contract and binding namespaces.
*/

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
    // Checks the version 1 schema; throws on invalid fields, types or values.
    static void validate_document(const Json &document);
    // Reads JSON and rejects duplicate keys, then validates the schema.
    static Json read(const std::filesystem::path &path);
    // Requires program, stages and resolved resources; checks binding collisions.
    static Manifest from_document(Json document, const std::filesystem::path &path);
    // Resolves a stage's file beside path (the manifest filename).
    std::filesystem::path stage_path(const std::string &stage) const;
};
