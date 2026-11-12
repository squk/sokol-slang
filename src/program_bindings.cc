#include "program_bindings.h"
#include <algorithm>
#include <map>
#include <stdexcept>
#include <tuple>
#include <vector>

Json resolve_program_bindings(Json &document, const std::filesystem::path &path,
                              const std::function<Json(const std::string &)> &inspect,
                              std::set<std::pair<std::string, std::string>> &used_overrides) {
    if (document.contains("resources")) {
        if (document.contains("policies") || document.contains("bindings"))
            throw std::runtime_error(
                "explicit resources cannot be combined with policies or binding overrides");
        return Json();
    }
    struct Resource {
        std::string name, stage;
        Json data;
    };
    std::vector<Resource> discovered;
    for (const auto &[stage, entry] : document.at("stages").items()) {
        const auto file =
            (path.parent_path() / entry.at("file").get<std::string>()).lexically_normal();
        const auto info = inspect(file.string());
        for (const auto &item : info.at("resources"))
            discovered.push_back({item.at("name"), stage, item});
    }
    std::sort(discovered.begin(), discovered.end(), [](const auto &a, const auto &b) {
        return std::tie(a.name, a.stage) < std::tie(b.name, b.stage);
    });
    const auto policies = document.value("policies", Json::object());
    std::map<std::pair<std::string, std::string>, int> overrides;
    for (const auto &binding : document.value("bindings", Json::array())) {
        const auto key = std::make_pair(binding.at("stage").get<std::string>(),
                                        binding.at("name").get<std::string>());
        if (!overrides.emplace(key, binding.at("slot")).second)
            throw std::runtime_error("duplicate binding override: " + key.first + ":" + key.second);
    }
    // Sokol has no acceleration structure bind slot, so these get their own
    // slot space. backend_compile.cc maps each slot to a Metal buffer index and
    // a Vulkan binding clear of every other resource.
    auto space_for = [](const std::string &kind) {
        return kind == "uniform" || kind == "sampler" ? kind
               : kind == "acceleration_structure"     ? "acceleration_structure"
                                                      : "view";
    };
    auto limit_for = [](const std::string &space) {
        return space == "uniform"                  ? 8
               : space == "sampler"                ? 12
               : space == "acceleration_structure" ? 4
                                                   : 32;
    };
    std::map<std::string, std::set<int>> occupied;
    // Reserve every explicit slot first, even if its resource sorts last.
    for (const auto &resource : discovered) {
        const auto found = overrides.find({resource.stage, resource.name});
        if (found == overrides.end())
            continue;
        used_overrides.insert(found->first);
        const auto space = space_for(resource.data.at("kind"));
        if (found->second < 0 || found->second >= limit_for(space))
            throw std::runtime_error(resource.name + ": slot must be in 0.." +
                                     std::to_string(limit_for(space) - 1));
        if (!occupied[space].insert(found->second).second)
            throw std::runtime_error("duplicate " + space +
                                     " slot; Sokol slots are global across stages");
    }
    Json resources = Json::object();
    Json bindings = Json::array();
    for (const auto &resource : discovered) {
        const auto &name = resource.name;
        const auto &stage = resource.stage;
        const std::string kind = resource.data.at("kind");
        const auto space = space_for(kind);
        Json policy = policies.value(name, Json::object());
        const std::string field = kind == "texture"   ? "sample_type"
                                  : kind == "sampler" ? "sampler_type"
                                                      : "";
        if (!field.empty()) {
            for (const auto &[key, value] : policy.items())
                if (key != field)
                    throw std::runtime_error(name + ": sampling policy does not match " + kind);
            if (!policy.contains(field))
                policy[field] = resource.data.at(field);
        } else if (!policy.empty()) {
            throw std::runtime_error(name +
                                     ": sampling policy is only valid for textures and samplers");
        }
        if (!resources.contains(name)) {
            resources[name] = {
                {"kind", kind}, {"names", Json::object()}, {"slots", Json::object()}};
            resources[name].update(policy);
        }
        auto &merged = resources[name];
        if (merged.at("kind") != kind || merged["slots"].contains(stage))
            throw std::runtime_error(name + ": duplicate or conflicting stage resource");
        for (const auto &[key, value] : policy.items())
            if (merged.at(key) != value)
                throw std::runtime_error(name + ": conflicting sampling policy across stages");
        const auto found = overrides.find({stage, name});
        int slot = found != overrides.end() ? found->second : 0;
        if (found == overrides.end()) {
            while (occupied[space].count(slot))
                ++slot;
            if (slot == limit_for(space))
                throw std::runtime_error(name + ": exceeds Sokol " + space + " binding limit");
            occupied[space].insert(slot);
        }
        merged["names"][stage] = name;
        merged["slots"][stage] = slot;
        for (auto key : {"access", "format"})
            if (resource.data.contains(key)) {
                if (merged.contains(key) && merged[key] != resource.data[key])
                    throw std::runtime_error(name + ": conflicting " + key + " across stages");
                merged[key] = resource.data[key];
            }
        Json binding = resource.data;
        binding.erase("sample_type");
        binding.erase("sampler_type");
        binding["stage"] = stage;
        binding["name"] = stage + "_" + name;
        binding["slot"] = slot;
        binding.update(policy);
        bindings.push_back(std::move(binding));
    }
    document["resources"] = std::move(bindings);
    document.erase("policies");
    document.erase("bindings");
    return resources;
}
