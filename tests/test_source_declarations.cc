#include "slang_compile.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace sokol_slang;

static void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

static const SourceDeclaration &declaration(const SourceResult &result, std::string_view suffix) {
    const auto found = std::find_if(result.declarations.begin(), result.declarations.end(),
                                    [&](const auto &item) { return item.path.ends_with(suffix); });
    if (found == result.declarations.end())
        throw std::runtime_error("Missing declaration: " + std::string(suffix));
    return *found;
}

static const SourceAttribute &attribute(const SourceDeclaration &declaration,
                                        std::string_view name) {
    const auto found = std::find_if(
        declaration.attributes.begin(), declaration.attributes.end(), [&](const auto &item) {
            return item.name == name || item.name == std::string(name) + "Attribute";
        });
    if (found == declaration.attributes.end())
        throw std::runtime_error("Missing attribute: " + std::string(name));
    return *found;
}

static SourceRequest request() {
    SourceRequest request;
    request.module_name = "declarations_test";
    request.source_path = "/captured/material.slang";
    request.search_paths = {"/captured"};
    request.entry_points = {"main"};
    request.files["/captured/controls.slang"] = R"(
        module "controls";
        public enum Choice { First = 3, Second = 7 };
        [__AttributeUsage(_AttributeTargets.Var)]
        public struct MetadataAttribute {
            string label;
            int signedValue;
            uint count;
            bool enabled;
            float scale;
            Choice choice;
        };
        public struct Controls {
            [Metadata("a\0b", -7, 9u, true, 0.125f, Choice.Second)]
            float4 unused;
            float plain;
        };
    )";
    request.source = R"(
        import controls;
        [__AttributeUsage(_AttributeTargets.Var)]
        struct ImageControlAttribute {};
        [ImageControl] Texture2DArray<float4> tex_image;
        SamplerState smp_image;
        ConstantBuffer<Controls> ub_material;
        struct Local { float2 uv; };
        [shader("compute")]
        [numthreads(1, 1, 1)]
        void main(uint3 id : SV_DispatchThreadID) {}
    )";
    return request;
}

int main() {
    try {
        auto input = request();
        auto result = compile_source(input);
        for (const auto &diagnostic : result.diagnostics)
            std::cerr << diagnostic.phase << ": " << diagnostic.text << '\n';
        require(result.succeeded, "Declaration fixture must compile");
        require(SourceResult::declaration_format_version == 1, "Declaration DTO version changed");
        input.source.clear();
        input.files.clear();
        const auto &member = declaration(result, "::Controls::unused");
        require(member.module == "controls",
                "Imported declaration must retain its module identity");
        require(member.kind == SourceDeclaration::Kind::Variable,
                "Struct member must be a variable declaration");
        require(member.type.kind == SourceType::Kind::Vector && member.type.columns == 4,
                "Unused vector member type must survive declaration reflection");
        require(!member.type.qualified_name.empty(), "Type spelling must be owned");
        require(declaration(result, "::Controls::plain").attributes.empty(),
                "Plain members must also survive");
        const auto &metadata = attribute(member, "Metadata");
        require(metadata.arguments.size() == 6, "Attribute argument count must survive");
        using K = SourceAttributeArgument::Kind;
        require(metadata.arguments[0].kind == K::String &&
                    std::get<std::string>(metadata.arguments[0].value) == std::string("a\0b", 3),
                "Strings must preserve their explicit byte length");
        require(metadata.arguments[1].kind == K::Int32 &&
                    std::get<int32_t>(metadata.arguments[1].value) == -7,
                "Signed attribute values must keep their type");
        require(metadata.arguments[2].kind == K::UInt32 &&
                    std::get<uint32_t>(metadata.arguments[2].value) == 9,
                "Unsigned attribute values must keep their type");
        require(metadata.arguments[3].kind == K::Bool &&
                    std::get<bool>(metadata.arguments[3].value),
                "Bool attribute values must keep their type");
        require(metadata.arguments[4].kind == K::Float32 &&
                    std::get<float>(metadata.arguments[4].value) == 0.125f,
                "Float attribute values must keep their type");
        require(metadata.arguments[5].kind == K::EnumInt32 &&
                    metadata.arguments[5].type_name.find("Choice") != std::string::npos &&
                    std::get<int32_t>(metadata.arguments[5].value) == 7,
                "Enum values must retain their declared type identity");
        const auto &texture = declaration(result, "::tex_image");
        require(texture.type.kind == SourceType::Kind::Resource &&
                    texture.type.resource_shape == "texture_2d" && texture.type.resource_array &&
                    texture.type.resource_access == "read" &&
                    !texture.type.resource_result_type.empty(),
                "Resource identity must not depend on GPU liveness");
        require(attribute(texture, "ImageControl").arguments.empty(),
                "Marker attributes must retain explicit empty args");
        require(declaration(result, "::smp_image").type.kind == SourceType::Kind::Sampler,
                "Sampler identity must survive");
        require(declaration(result, "::ub_material").type.kind == SourceType::Kind::ConstantBuffer,
                "Uniform block identity must survive");
        require(declaration(result, "::Local::uv").type.columns == 2,
                "Root module members must survive");
        auto unsupported = request();
        unsupported.files.clear();
        unsupported.source = R"(
            [__AttributeUsage(_AttributeTargets.Var)]
            struct WideAttribute { double value; };
            struct Controls { [Wide(1.25)] float exposed; };
            [shader("compute")]
            [numthreads(1, 1, 1)]
            void main(uint3 id : SV_DispatchThreadID) {}
        )";
        const auto rejected = compile_source(unsupported);
        require(!rejected.succeeded, "Unsupported typed arguments must not be coerced");
        require(rejected.declarations.empty() && rejected.stages.empty(),
                "Failed extraction must not publish partial data");
        require(std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(),
                            [](const auto &diagnostic) {
                                return diagnostic.phase == "declarations" &&
                                       diagnostic.text.find("Wide") != std::string::npos &&
                                       diagnostic.text.find(
                                           "Unsupported attribute argument type") !=
                                           std::string::npos;
                            }),
                "Unsupported argument diagnostic must name its attribute and phase");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
