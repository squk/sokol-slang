#include "program_compile.h"
#include <iostream>
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
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
