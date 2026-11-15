#pragma once
#include "bytecode.h"
#include <filesystem>
#include <string>
#include <vector>

// Compile the build machine's native shader formats. Other platforms retain
// source so a single header can still describe every Sokol backend.
void compile_native_bytecode(shdc::Bytecode &bytecode, shdc::Spirvcross &sources,
                             shdc::Slang::Enum target, const std::filesystem::path &work,
                             const std::vector<std::string> &metal_args);

// Builds the arguments used by the Metal tool, without its executable path.
// Invariant vertex positions require the matching native compiler option;
// stage names and position field names do not affect this rule.
std::vector<std::string> metal_compile_arguments(shdc::Slang::Enum target,
                                                 const std::string &source,
                                                 const std::filesystem::path &input,
                                                 const std::filesystem::path &object,
                                                 const std::vector<std::string> &extra);
