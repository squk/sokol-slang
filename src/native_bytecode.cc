#include "native_bytecode.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

#if defined(__APPLE__)
#include <cerrno>
#include <cstdio>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

static void run_tool(std::vector<std::string> args) {
    std::vector<char *> argv;
    for (auto &arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid;
    const int error = posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ);
    if (error)
        throw std::runtime_error("cannot run Metal tool: " + args[0]);
    int status;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR)
            throw std::runtime_error("cannot wait for Metal tool: " + args[0]);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("Metal tool failed: " + args[0]);
}

static std::filesystem::path metal_compiler() {
    FILE *pipe = popen("/usr/bin/xcrun --find metal", "r");
    if (!pipe)
        throw std::runtime_error("cannot find Apple Metal compiler");
    std::string path;
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe))
        path += buffer;
    const int status = pclose(pipe);
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
        path.pop_back();
    if (status || path.empty())
        throw std::runtime_error(
            "Apple Metal compiler unavailable; install the Xcode Metal Toolchain or use --source");
    // Recent Xcode versions discover metal through the downloadable toolchain,
    // while xcrun may fail to discover its sibling metallib executable.
    return std::filesystem::canonical(path);
}

static std::vector<uint8_t> compile_metal(const std::string &source, shdc::Slang::Enum target,
                                          const std::filesystem::path &path) {
    static const auto compiler = metal_compiler();
    const auto linker = compiler.parent_path() / "metallib";
    auto input = path;
    input += ".metal";
    auto object = path;
    object += ".air";
    auto library = path;
    library += ".metallib";
    std::ofstream stream(input);
    stream << source;
    stream.close();
    if (!stream)
        throw std::runtime_error("cannot write Metal source: " + input.string());
    const bool macos = target == shdc::Slang::METAL_MACOS;
    const std::string triple = macos                              ? "air64-apple-macos10.14"
                               : target == shdc::Slang::METAL_IOS ? "air64-apple-ios12.0"
                                                                  : "air64-apple-ios13.0-simulator";
    // SPIRV-Cross can leave unused constants in each entry point's generated source.
    run_tool({compiler.string(), "-target", triple,
              macos ? "-std=macos-metal2.1" : "-std=ios-metal2.1", "-Wno-unused-const-variable",
              "-c", input.string(), "-o", object.string()});
    run_tool({linker.string(), object.string(), "-o", library.string()});
    std::ifstream binary(library, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(binary)), {});
    if (binary.bad() || bytes.size() < 4 || std::string(bytes.begin(), bytes.begin() + 4) != "MTLB")
        throw std::runtime_error("invalid Metal library: " + library.string());
    return bytes;
}
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3dcompiler.h>
#include <windows.h>

static std::vector<uint8_t> compile_dxbc(const shdc::SpirvcrossSource &source,
                                         shdc::Slang::Enum target) {
    HMODULE library = LoadLibraryA("d3dcompiler_47.dll");
    if (!library)
        throw std::runtime_error("d3dcompiler_47.dll unavailable; install it or use --source");
    auto compile = reinterpret_cast<pD3DCompile>(GetProcAddress(library, "D3DCompile"));
    if (!compile) {
        FreeLibrary(library);
        throw std::runtime_error("D3DCompile unavailable in d3dcompiler_47.dll");
    }
    const auto stage = source.stage_refl.stage;
    const std::string profile = std::string(stage == shdc::ShaderStage::Vertex     ? "vs"
                                            : stage == shdc::ShaderStage::Fragment ? "ps"
                                                                                   : "cs") +
                                (target == shdc::Slang::HLSL4 ? "_4_0" : "_5_0");
    ID3DBlob *output = nullptr;
    ID3DBlob *diagnostics = nullptr;
    const HRESULT result =
        compile(source.source_code.data(), source.source_code.size(), nullptr, nullptr, nullptr,
                source.stage_refl.entry_point.c_str(), profile.c_str(),
                D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &output,
                &diagnostics);
    std::string message;
    if (diagnostics) {
        message.assign(static_cast<const char *>(diagnostics->GetBufferPointer()),
                       diagnostics->GetBufferSize());
        diagnostics->Release();
    }
    std::vector<uint8_t> bytes;
    if (output) {
        const auto *data = static_cast<const uint8_t *>(output->GetBufferPointer());
        bytes.assign(data, data + output->GetBufferSize());
        output->Release();
    }
    FreeLibrary(library);
    if (FAILED(result) || bytes.size() < 4 ||
        std::string(bytes.begin(), bytes.begin() + 4) != "DXBC")
        throw std::runtime_error("DXBC compilation failed (" + profile + "): " + message);
    if (!message.empty())
        std::cerr << message;
    return bytes;
}
#endif

void compile_native_bytecode(shdc::Bytecode &bytecode, shdc::Spirvcross &sources,
                             shdc::Slang::Enum target, const std::filesystem::path &work) {
    for (auto &source : sources.sources) {
        shdc::BytecodeBlob blob;
#if defined(__APPLE__)
        if (shdc::Slang::is_msl(target)) {
            std::filesystem::create_directories(work);
            blob.data = compile_metal(source.source_code, target,
                                      work / (std::string(shdc::Slang::to_str(target)) + "_" +
                                              std::to_string(source.snippet_index)));
        }
#elif defined(_WIN32)
        if (shdc::Slang::is_hlsl(target))
            blob.data = compile_dxbc(source, target);
#endif
        if (!blob.data.empty()) {
            blob.valid = true;
            blob.snippet_index = source.snippet_index;
            bytecode.blobs.push_back(std::move(blob));
            source.source_code.clear();
        }
    }
}
