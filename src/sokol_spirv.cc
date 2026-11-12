#include "backend_internal.h"
#include "program_bindings.h"
#ifdef SOKOL_SLANG_SOURCE_COMPILER
#include "slang_compile.h"
#endif
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>

#ifdef SOKOL_SLANG_SOURCE_COMPILER
static int compile_source_request(const std::string &input, const std::string &output) {
    std::ifstream stream(input);
    if (!stream)
        throw std::runtime_error("Cannot open source request: " + input);
    const auto document = Json::parse(stream);
    const std::set<std::string> fields{"version",      "module_name", "source_path",  "source",
                                       "entry_points", "files",       "search_paths", "profile",
                                       "defines",      "capabilities"};
    if (!document.is_object() || document.at("version") != 1)
        throw std::runtime_error("Expected source request version 1");
    for (const auto &[name, value] : document.items())
        if (!fields.count(name))
            throw std::runtime_error("Unknown source request field: " + name);
    sokol_slang::SourceRequest request;
    request.module_name = document.at("module_name");
    request.source_path = document.at("source_path");
    request.source = document.at("source");
    request.entry_points = document.at("entry_points").get<std::vector<std::string>>();
    request.files = document.value("files", std::map<std::string, std::string>{});
    request.search_paths = document.value("search_paths", std::vector<std::string>{});
    request.defines = document.value("defines", std::map<std::string, std::string>{});
    request.profile = document.value("profile", request.profile);
    request.capabilities = document.value("capabilities", std::vector<std::string>{});
    auto result = sokol_slang::compile_source(request);
    Json report = {{"version", 1},
                   {"succeeded", result.succeeded},
                   {"compiler", result.compiler_identity},
                   {"dependencies", result.dependency_paths},
                   {"diagnostics", Json::array()},
                   {"stages", Json::array()}};
    for (const auto &diagnostic : result.diagnostics)
        report["diagnostics"].push_back({{"phase", diagnostic.phase}, {"text", diagnostic.text}});
    if (result.succeeded) {
        report["reflection"] = Json::parse(result.reflection_json);
        for (const auto &stage : result.stages)
            report["stages"].push_back(
                {{"stage", stage.stage}, {"entry", stage.entry_point}, {"spirv", stage.spirv}});
    }
    std::ofstream destination(output, std::ios::binary);
    destination << report.dump(2) << '\n';
    destination.close();
    if (!destination)
        throw std::runtime_error("Cannot write source result: " + output);
    return result.succeeded ? 0 : 1;
}
#endif

int main(int argc, char **argv) {
    std::filesystem::path native_work;
    try {
        std::map<std::string, std::string> options;
        std::vector<std::pair<std::string, std::string>> constraints;
        for (int i = 1; i < argc; ++i) {
            std::string key = argv[i];
            if (key == "-o")
                key = "--output";
            if (key == "-h")
                key = "--help";
            if (key == "--binding" || key == "--sample-type" || key == "--sampler-type") {
                if (++i == argc)
                    throw std::runtime_error("missing value: " + key);
                constraints.emplace_back(key, argv[i]);
                continue;
            }
            if (key == "--metal-arg") {
                if (++i == argc)
                    throw std::runtime_error("missing value: " + key);
                Json values = options.count("--metal-args")
                                  ? Json::parse(options.at("--metal-args"))
                                  : Json::array();
                values.push_back(argv[i]);
                options["--metal-args"] = values.dump();
                continue;
            }
            if (key == "--source") {
                if (!options.emplace(key, "").second)
                    throw std::runtime_error("duplicate option: " + key);
                continue;
            }
            if (key == "--help") {
                std::cout
                    << "sokol-spirv --vertex vertex.spv --fragment fragment.spv -o shader.h\n"
                       "sokol-spirv --compute compute.spv -o shader.h\n"
                       "sokol-spirv --manifest programs.json -o shader.h\n"
                       "sokol-spirv --inspect shader.spv\n"
                       "sokol-spirv --source-request request.json -o result.json\n"
                       "Optional: --module NAME --program NAME --targets DIALECTS\n"
                       "          --reflection FILE --depfile FILE\n"
                       "          --binding STAGE:NAME=SLOT --sample-type NAME=TYPE "
                       "--sampler-type NAME=TYPE\n"
                       "--source: emit native Metal/HLSL source instead of bytecode\n"
                       "--metal-arg ARG: pass an argument to the Metal compiler (repeatable)\n";
                return 0;
            }
            if (key != "--manifest" && key != "--output" && key != "--reflection" &&
                key != "--depfile" && key != "--targets" && key != "--inspect" &&
                key != "--vertex" && key != "--fragment" && key != "--compute" &&
                key != "--module" && key != "--program" && key != "--source-request")
                throw std::runtime_error("unknown option: " + key);
            if (++i == argc || !options.emplace(key, argv[i]).second)
                throw std::runtime_error("missing value or duplicate option: " + key);
        }
        if (options.count("--inspect")) {
            if (options.size() != 1 || !constraints.empty())
                throw std::runtime_error("--inspect cannot be combined with generation options");
            std::cout << inspect_spirv(options.at("--inspect")).dump(2) << '\n';
            return 0;
        }
        if (!options.count("--output"))
            throw std::runtime_error("missing --output");
        if (options.count("--source-request")) {
            if (options.size() != 2 || !constraints.empty())
                throw std::runtime_error("--source-request accepts only --output");
#ifdef SOKOL_SLANG_SOURCE_COMPILER
            return compile_source_request(options.at("--source-request"), options.at("--output"));
#else
            throw std::runtime_error("Source requests require -Dcompiler_library=true");
#endif
        }
        const auto output_path = std::filesystem::absolute(options.at("--output"));
        std::map<std::string, Json> inspections;
        auto inspect = [&](const std::string &path) -> Json {
            auto [item, inserted] = inspections.try_emplace(path);
            if (inserted)
                item->second = inspect_spirv(path);
            return item->second;
        };
        const auto manifest_path = options.count("--manifest")
                                       ? std::filesystem::absolute(options.at("--manifest"))
                                       : std::filesystem::current_path() / "command-line";
        Json document;
        if (options.count("--manifest")) {
            for (auto key : {"--vertex", "--fragment", "--compute", "--module", "--program"})
                if (options.count(key))
                    throw std::runtime_error("--manifest cannot be combined with " +
                                             std::string(key));
            if (!constraints.empty())
                throw std::runtime_error("supply manifest constraints inside the manifest");
            document = Manifest::read(manifest_path);
        } else {
            const bool compute = options.count("--compute");
            if (compute ? options.count("--vertex") || options.count("--fragment")
                        : !options.count("--vertex") || !options.count("--fragment"))
                throw std::runtime_error("supply --vertex and --fragment, or --compute");
            std::string module = output_path.stem().string();
            for (char &c : module)
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_'))
                    c = '_';
            if (module.empty() || (module[0] >= '0' && module[0] <= '9'))
                module = "_" + module;
            document = {{"version", 1},
                        {"module", options.count("--module") ? options.at("--module") : module},
                        {"program", options.count("--program") ? options.at("--program")
                                    : compute                  ? "run"
                                                               : "draw"},
                        {"stages", Json::object()}};
            if (!compute)
                document["clip_depth"] = "zero_to_one";
            for (const auto &stage : compute ? std::vector<std::string>{"compute"}
                                             : std::vector<std::string>{"vertex", "fragment"}) {
                const auto file =
                    std::filesystem::absolute(options.at("--" + stage)).lexically_normal();
                const auto info = inspect(file.string());
                if (info.at("entries").size() != 1 || info["entries"][0]["stage"] != stage)
                    throw std::runtime_error("expected one " + stage + " entry point in " +
                                             file.string());
                document["stages"][stage] = {{"file", file.string()},
                                             {"entry", info["entries"][0]["name"]}};
            }
            for (const auto &[key, value] : constraints) {
                const auto equal = value.find('=');
                if (equal == std::string::npos || equal == 0 || equal + 1 == value.size())
                    throw std::runtime_error(key + " expects NAME=VALUE");
                const auto name = value.substr(0, equal);
                const auto setting = value.substr(equal + 1);
                if (key == "--binding") {
                    const auto colon = name.find(':');
                    if (colon == std::string::npos || colon == 0 || colon + 1 == name.size() ||
                        setting.find_first_not_of("0123456789") != std::string::npos)
                        throw std::runtime_error("--binding expects STAGE:NAME=SLOT");
                    if (!document.contains("bindings"))
                        document["bindings"] = Json::array();
                    document["bindings"].push_back({{"stage", name.substr(0, colon)},
                                                    {"name", name.substr(colon + 1)},
                                                    {"slot", std::stoul(setting)}});
                } else {
                    const std::string field =
                        key == "--sample-type" ? "sample_type" : "sampler_type";
                    if (document.contains("policies") && document["policies"].contains(name))
                        throw std::runtime_error("duplicate sampling policy: " + name);
                    document["policies"][name] = {{field, setting}};
                }
            }
        }
        Manifest::validate_document(document);
        const bool batch = document.contains("programs");
        if (batch && (document.contains("program") || document.contains("stages") ||
                      document.contains("resources") || document.contains("clip_depth")))
            throw std::runtime_error("program sets cannot contain single-program fields");
        std::filesystem::create_directories(output_path.parent_path());
        native_work = output_path.string() + "." +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                      ".tmp";
        std::filesystem::create_directory(native_work);
        Json programs = batch ? document.at("programs") : Json::array({document});
        Json reports = Json::object();
        std::set<std::string> names;
        std::set<std::pair<std::string, std::string>> used_overrides;
        std::set<std::filesystem::path> dependencies;
        if (options.count("--manifest"))
            dependencies.insert(manifest_path);
        std::string combined;
        // Validate and render every program before publishing any output.
        for (auto program : programs) {
            const std::string name = program.at("program");
            if (!names.insert(name).second)
                throw std::runtime_error("duplicate program: " + name);
            if (batch) {
                program["version"] = document.at("version");
                program["module"] = document.at("module");
                for (auto field : {"policies", "bindings"})
                    if (document.contains(field))
                        program[field] = document[field];
            }
            const auto resources =
                resolve_program_bindings(program, manifest_path, inspect, used_overrides);
            auto manifest = Manifest::from_document(program, manifest_path);
            for (const auto &[stage, entry] : program.at("stages").items())
                dependencies.insert(manifest.stage_path(stage));
            auto generated = generate_program(manifest, options, native_work / name,
                                              output_path.filename().string(), batch);
            if (!resources.is_null())
                generated.report["resources"] = resources;
            reports[name] = std::move(generated.report);
            if (!combined.empty())
                combined += '\n';
            combined += generated.header;
        }
        for (const auto &binding : document.value("bindings", Json::array())) {
            const auto key = std::make_pair(binding.at("stage").get<std::string>(),
                                            binding.at("name").get<std::string>());
            if (!used_overrides.count(key))
                throw std::runtime_error("binding override names no reflected resource: " +
                                         key.first + ":" + key.second);
        }
        std::vector<std::pair<std::filesystem::path, std::string>> outputs;
        if (options.count("--reflection"))
            outputs.emplace_back(options.at("--reflection"),
                                 (batch ? reports : reports.begin().value()).dump(2) + '\n');
        auto escape = [](const std::string &path) {
            std::string result;
            for (char c : path) {
                if (c == ' ' || c == '#' || c == '\\')
                    result += '\\';
                if (c == '$')
                    result += '$';
                result += c;
            }
            return result;
        };
        if (options.count("--depfile")) {
            std::ostringstream depfile;
            depfile << escape(options.at("--output")) << ":";
            for (const auto &dependency : dependencies)
                depfile << ' ' << escape(dependency.string());
            depfile << '\n';
            outputs.emplace_back(options.at("--depfile"), depfile.str());
        }
        outputs.emplace_back(output_path, std::move(combined));
        // Stage all requested files before publishing. Publish the header last.
        for (size_t i = 0; i < outputs.size(); ++i) {
            std::ofstream stream(native_work / std::to_string(i), std::ios::binary);
            stream << outputs[i].second;
            stream.close();
            if (!stream)
                throw std::runtime_error("cannot write output: " + outputs[i].first.string());
        }
        for (size_t i = 0; i < outputs.size(); ++i)
            std::filesystem::copy_file(native_work / std::to_string(i), outputs[i].first,
                                       std::filesystem::copy_options::overwrite_existing);
        std::filesystem::remove_all(native_work);
        return 0;
    } catch (const std::exception &error) {
        if (!native_work.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(native_work, ignored);
        }
        std::cerr << "sokol-spirv: " << error.what() << '\n';
        return 1;
    }
}
