#pragma once
#include "bytecode.h"
#include <filesystem>

// Compile the build machine's native shader formats. Other platforms retain
// source so a single header can still describe every Sokol backend.
void compile_native_bytecode(shdc::Bytecode &bytecode, shdc::Spirvcross &sources,
                             shdc::Slang::Enum target, const std::filesystem::path &work);
