#include "program_compile.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <utility>

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

        const auto frontend = result.source;
        auto captured = frontend;
        captured.compiler_identity = "owned frontend identity";
        captured.dependency_paths.push_back("owned-only.slang");
        const auto reused = compile_programs(std::move(captured), request.programs, request.targets,
                                             request.source.module_name);
        require(reused.succeeded && reused.programs.size() == 2,
                "Owned frontend result must compile a backend family");
        require(reused.source.compiler_identity == "owned frontend identity" &&
                    reused.source.dependency_paths.back() == "owned-only.slang",
                "Backend reuse must preserve owned frontend metadata");
        require(reused.source.stages.front().spirv == frontend.stages.front().spirv,
                "Backend reuse must preserve frontend SPIR-V");
        require(reused.programs.at("first").to_json() == result.programs.at("first").to_json(),
                "Reused frontend must produce the same program artifact");
        SourceResult failed_source;
        failed_source.diagnostics.push_back({"frontend", "original failure"});
        auto failed_reuse = compile_programs(std::move(failed_source), request.programs,
                                             request.targets, request.source.module_name);
        require(!failed_reuse.succeeded && failed_reuse.programs.empty() &&
                    failed_reuse.source.diagnostics.size() == 1 &&
                    failed_reuse.source.diagnostics.front().text == "original failure",
                "Failed frontend results must retain diagnostics and skip backend programs");
        auto duplicate_source = frontend;
        duplicate_source.stages.push_back(duplicate_source.stages.front());
        failed_reuse = compile_programs(std::move(duplicate_source), request.programs,
                                        request.targets, request.source.module_name);
        require(!failed_reuse.succeeded && failed_reuse.programs.empty() &&
                    failed_reuse.source.diagnostics.back().text.find("Duplicate compiled entry") !=
                        std::string::npos,
                "Backend reuse must reject duplicate compiled entries");
        auto missing_selection = request.programs;
        missing_selection.at("first").entries = {{"compute", "notCompiled"}};
        failed_reuse = compile_programs(frontend, missing_selection, request.targets,
                                        request.source.module_name);
        require(!failed_reuse.succeeded && failed_reuse.programs.empty() &&
                    failed_reuse.source.diagnostics.back().phase == "program first",
                "Backend reuse must reject missing selected entries and clear the family");
        missing_selection.at("first").entries = {{"fragment", "computeMain"}};
        failed_reuse = compile_programs(frontend, missing_selection, request.targets,
                                        request.source.module_name);
        require(!failed_reuse.succeeded && failed_reuse.programs.empty() &&
                    failed_reuse.source.diagnostics.back().phase == "request",
                "Backend reuse must reject incomplete raster selections");
        require(!compile_programs(frontend, request.programs, request.targets, "").succeeded,
                "Backend reuse needs an explicit module name");
        require(
            !compile_programs(frontend, request.programs, "", request.source.module_name).succeeded,
            "Backend reuse needs explicit targets");

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
        ProgramRequest raster;
        raster.targets = "glsl430:spirv_vk";
        raster.source.module_name = "source_identity";
        raster.source.source_path = "source_identity.slang";
        raster.source.source = R"(
            struct Parameters { float scale; };
            ConstantBuffer<Parameters> ub_material;
            Texture2D<float4> tex_color;
            SamplerState smp_tex_color;
            struct StorageRecord { float4 value; };
            StructuredBuffer<StorageRecord> instance_parameter_data;
            struct VertexInput { float3 in_pos : POSITION; };
            struct Varyings { float4 position : SV_Position; float2 uv : TEXCOORD0; };
            [shader("vertex")]
            Varyings vertexMain(VertexInput input) {
                Varyings output;
                output.position = float4(input.in_pos * (ub_material.scale + instance_parameter_data[0].value.x), 1.0);
                output.uv = input.in_pos.xy;
                return output;
            }
            [shader("fragment")]
            float4 fragmentMain(Varyings input) : SV_Target0 {
                return tex_color.Sample(smp_tex_color, input.uv) *
                    (ub_material.scale + instance_parameter_data[0].value.x);
            }
        )";
        raster.programs["draw"].entries = {{"vertex", "vertexMain"}, {"fragment", "fragmentMain"}};
        raster.programs["draw"].policy = {{"policies",
                                           {{"tex_color", {{"sample_type", "float"}}},
                                            {"smp_tex_color", {{"sampler_type", "filtering"}}}}}};
        const auto raster_result = compile_programs(raster);
        require(raster_result.succeeded, "Struct-input raster family must compile");
        const auto &artifact = raster_result.programs.at("draw");
        require(artifact.to_json().at("version") == 2, "Artifact schema must be version 2");
        const auto &attribute = artifact.layout.at("attributes").at(0);
        require(attribute.at("source_name") == "input.in_pos", "Attribute must retain source path");
        require(attribute.at("name") == "input_in_pos",
                "Native attribute must retain GLSL spelling");
        require(artifact.layout.at("uniform_blocks").size() == 2,
                "Both stages must retain material blocks");
        for (const auto &block : artifact.layout.at("uniform_blocks")) {
            const std::string stage =
                block.at("stage") == "SG_SHADERSTAGE_VERTEX" ? "vertex" : "fragment";
            require(block.at("source_name") == "ub_material",
                    "Uniform source identity must be unprefixed");
            require(block.at("name") == stage + "_ub_material",
                    "Uniform native identity must retain prefix");
        }
        const auto &texture = artifact.layout.at("textures").at(0);
        require(texture.at("source_name") == "tex_color",
                "Texture source identity must be unprefixed");
        require(texture.at("name") == "fragment_tex_color",
                "Texture native identity must retain prefix");
        require(texture.at("sample_type") == "SG_IMAGESAMPLETYPE_FLOAT",
                "Texture sampling policy must remain intact");
        const auto &sampler = artifact.layout.at("samplers").at(0);
        require(sampler.at("source_name") == "smp_tex_color",
                "Sampler source identity must be unprefixed");
        require(sampler.at("name") == "fragment_smp_tex_color",
                "Sampler native identity must retain prefix");
        require(artifact.layout.at("storage_buffers").size() == 2,
                "Both stages must retain shared storage");
        for (const auto &buffer : artifact.layout.at("storage_buffers")) {
            const std::string stage =
                buffer.at("stage") == "SG_SHADERSTAGE_VERTEX" ? "vertex" : "fragment";
            require(buffer.at("source_name") == "instance_parameter_data",
                    "Storage source identity must not change");
            require(buffer.at("name") == stage + "_instance_parameter_data",
                    "Storage native identity must retain prefix");
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
