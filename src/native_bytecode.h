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
