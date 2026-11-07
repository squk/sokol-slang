#include "program_compile.h"
#include <set>
#include <stdexcept>
#include <utility>

namespace sokol_slang {

namespace {
std::set<std::string> selected_entries(const std::map<std::string, ProgramSelection> &programs,
                                       const std::string &targets) {
    if (programs.empty())
        throw std::runtime_error("Supply at least one program");
    if (targets.empty())
        throw std::runtime_error("Supply explicit backend targets");
    std::set<std::string> entries;
    for (const auto &[name, program] : programs) {
        if (name.empty())
            throw std::runtime_error("Program name is empty");
        const auto &selected = program.entries;
        const bool compute = selected.size() == 1 && selected.contains("compute");
        const bool raster =
            selected.size() == 2 && selected.contains("vertex") && selected.contains("fragment");
        if (!compute && !raster)
            throw std::runtime_error(name + ": select compute or vertex and fragment entries");
        for (const auto &[stage, entry] : selected) {
            if (entry.empty())
                throw std::runtime_error(name + ": empty " + stage + " entry");
            entries.insert(entry);
        }
    }
    return entries;
}
} // namespace

ProgramResult compile_programs(const ProgramRequest &request) {
    ProgramResult result;
    try {
        SourceRequest source = request.source;
        const auto entries = selected_entries(request.programs, request.targets);
        // Selections define the exact entry set; no discovery or extra stages.
        source.entry_points.assign(entries.begin(), entries.end());
        return compile_programs(compile_source(source), request.programs, request.targets,
                                source.module_name, request.source_only);
    } catch (const std::exception &error) {
        result.source.diagnostics.push_back({"request", error.what()});
        return result;
    }
}

ProgramResult compile_programs(SourceResult source,
                               const std::map<std::string, ProgramSelection> &programs,
                               const std::string &targets, const std::string &module_name,
                               bool source_only) {
    ProgramResult result;
    result.source = std::move(source);
    std::string phase = "request";
    try {
        selected_entries(programs, targets);
        if (!result.source.succeeded)
            return result;
        if (module_name.empty())
            throw std::runtime_error("Supply an explicit module name");
        std::map<std::pair<std::string, std::string>, const StageCode *> available;
        for (const auto &stage : result.source.stages)
            if (!available.emplace(std::make_pair(stage.stage, stage.entry_point), &stage).second)
                throw std::runtime_error("Duplicate compiled entry: " + stage.entry_point);
        for (const auto &[name, program] : programs) {
            phase = "program " + name;
            BackendRequest backend;
            backend.manifest = program.policy;
            if (backend.manifest.is_null())
                backend.manifest = Json::object();
            if (!backend.manifest.is_object())
                throw std::runtime_error("Program policy must be an object");
            backend.manifest["version"] = 1;
            backend.manifest["module"] = module_name;
            backend.manifest["program"] = name;
            backend.targets = targets;
            backend.source_only = source_only;
            for (const auto &[stage, entry] : program.entries) {
                const auto found = available.find({stage, entry});
                if (found == available.end())
                    throw std::runtime_error("Missing " + stage + " entry: " + entry);
                backend.stages.emplace(stage, found->second->spirv);
            }
            result.programs.emplace(name, compile_backend(backend));
        }
        result.succeeded = true;
    } catch (const std::exception &error) {
        result.programs.clear();
        result.source.diagnostics.push_back({phase, error.what()});
    }
    return result;
}

} // namespace sokol_slang
