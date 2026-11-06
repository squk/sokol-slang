#include "program_compile.h"
#include <set>
#include <stdexcept>

namespace sokol_slang {

ProgramResult compile_programs(const ProgramRequest &request) {
    ProgramResult result;
    std::string phase = "request";
    try {
        if (request.programs.empty())
            throw std::runtime_error("Supply at least one program");
        if (request.targets.empty())
            throw std::runtime_error("Supply explicit backend targets");
        SourceRequest source = request.source;
        std::set<std::string> entries;
        for (const auto &[name, program] : request.programs) {
            if (name.empty())
                throw std::runtime_error("Program name is empty");
            const auto &selected = program.entries;
            const bool compute = selected.size() == 1 && selected.contains("compute");
            const bool raster = selected.size() == 2 && selected.contains("vertex") &&
                                selected.contains("fragment");
            if (!compute && !raster)
                throw std::runtime_error(name + ": select compute or vertex and fragment entries");
            for (const auto &[stage, entry] : selected) {
                if (entry.empty())
                    throw std::runtime_error(name + ": empty " + stage + " entry");
                entries.insert(entry);
            }
        }
        // Selections define the exact entry set; no discovery or extra stages.
        source.entry_points.assign(entries.begin(), entries.end());
        result.source = compile_source(source);
        if (!result.source.succeeded)
            return result;
        std::map<std::pair<std::string, std::string>, const StageCode *> available;
        for (const auto &stage : result.source.stages)
            if (!available.emplace(std::make_pair(stage.stage, stage.entry_point), &stage).second)
                throw std::runtime_error("Duplicate compiled entry: " + stage.entry_point);
        for (const auto &[name, program] : request.programs) {
            phase = "program " + name;
            BackendRequest backend;
            backend.manifest = program.policy;
            if (backend.manifest.is_null())
                backend.manifest = Json::object();
            if (!backend.manifest.is_object())
                throw std::runtime_error("Program policy must be an object");
            backend.manifest["version"] = 1;
            backend.manifest["module"] = source.module_name;
            backend.manifest["program"] = name;
            backend.targets = request.targets;
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
