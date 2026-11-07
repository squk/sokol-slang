#include "program_compile.h"
#include <iostream>
#include <algorithm>
#include <stdexcept>

using namespace sokol_slang;

static void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

int main() {
    try {
        ProgramRequest request;
        request.targets = "spirv_vk";
        require(!compile_programs(request).succeeded, "Empty family must fail");
        request.source.module_name = "family";
        request.source.source_path = "family.slang";
        request.source.source = R"(
            [shader("compute")]
            [numthreads(1, 1, 1)]
            void computeMain(uint3 id : SV_DispatchThreadID) {}
        )";
        request.programs["first"].entries = {{"compute", "computeMain"}};
        request.programs["second"].entries = {{"compute", "computeMain"}};
        auto result = compile_programs(request);
        for (const auto &diagnostic : result.source.diagnostics)
            std::cerr << diagnostic.phase << ": " << diagnostic.text << '\n';
        require(result.succeeded, "Valid family must compile");
        require(result.source.stages.size() == 1, "Shared entry must compile once");
        require(result.programs.size() == 2, "Both programs must be returned");
        require(!result.programs.at("first").stages.at(0).bytecode.empty(),
                "Vulkan artifact must own bytecode");

        request.programs["z_invalid_name!"].entries = {{"compute", "computeMain"}};
        result = compile_programs(request);
        require(result.source.succeeded, "Backend failure must preserve source result");
        require(!result.succeeded && result.programs.empty(),
                "Failure must discard partial family");
        require(result.source.diagnostics.back().phase == "program z_invalid_name!",
                "Backend failure must identify program");
        request.programs.erase("z_invalid_name!");
        request.programs["second"].entries = {{"fragment", "computeMain"}};
        result = compile_programs(request);
        require(!result.succeeded && result.source.stages.empty(),
                "Incomplete raster selection must fail before compilation");
        SourceRequest macros;
        macros.module_name = "macro_test";
        macros.source_path = "root.slang";
        macros.entry_points = {"main"};
        macros.source = R"(
                #include "select.slang"
                [shader("compute")]
                [numthreads(1, 1, 1)]
                void main(uint3 id : SV_DispatchThreadID) { int value = selectedValue(); }
            )";
        macros.files["select.slang"] = R"(
                #if MODE == 7
                import seven;
                #elif MODE == 9
                import nine;
                #else
                #error Missing session macro
                #endif
            )";
        macros.files["seven.slang"] = R"(
                #if MODE != 7
                #error Session macro did not reach imported module
                #endif
                int selectedValue() { return 7; }
            )";
        macros.files["nine.slang"] = R"(
                #if MODE != 9
                #error Session macro did not reach imported module
                #endif
                int selectedValue() { return 9; }
            )";
        for (const auto *mode : {"7", "9"}) {
            macros.defines = {{"MODE", mode}};
            auto compiled = compile_source(macros);
            require(compiled.succeeded, "Session macros must reach includes and imported modules");
            const std::string selected = std::string(mode) == "7" ? "seven.slang" : "nine.slang";
            const std::string other = std::string(mode) == "7" ? "nine.slang" : "seven.slang";
            require(std::find(compiled.dependency_paths.begin(), compiled.dependency_paths.end(),
                              selected) != compiled.dependency_paths.end(),
                    "Selected module must be a dependency");
            require(std::find(compiled.dependency_paths.begin(), compiled.dependency_paths.end(),
                              other) == compiled.dependency_paths.end(),
                    "Inactive module must not be a dependency");
        }
        macros.defines.clear();
        require(!compile_source(macros).succeeded, "Definitions must not leak across requests");
        for (const auto *name : {"", "1BAD", "BAD-NAME"}) {
            macros.defines = {{name, "1"}};
            const auto invalid = compile_source(macros);
            require(!invalid.succeeded && invalid.diagnostics.front().phase == "input",
                    "Invalid macro names must fail before session creation");
        }
        macros.defines = {{"MODE", std::string("7\0x", 3)}};
        const auto invalid = compile_source(macros);
        require(!invalid.succeeded && invalid.diagnostics.front().phase == "input",
                "Macro values must reject embedded null bytes");
        request.programs["second"].entries = {{"compute", "computeMain"}};
        request.source.defines = {{"FAMILY_MODE", "1"}};
        request.source.source =
            "#if FAMILY_MODE != 1\n#error Missing family macro\n#endif\n" + request.source.source;
        require(compile_programs(request).succeeded,
                "Program families must preserve source definitions");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
