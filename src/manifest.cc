#include "manifest.h"
#include "manifest_schema.h"
#include <fstream>
#include <regex>
#include <set>
#include <stdexcept>
#include <tuple>

// Implements the schema keywords used by our fixed manifest schema. This is
// internal validation, not an API for accepting arbitrary JSON Schemas.
static void validate(const Json& value, const Json& schema, const std::string& path) {
    auto fail = [&](const std::string& message) { throw std::runtime_error(path + ": " + message); };
    if (schema.contains("const") && value != schema["const"]) fail("unexpected constant value");
    if (schema.contains("enum")) {
        bool match = false;
        for (const auto& allowed : schema["enum"]) match |= value == allowed;
        if (!match) fail("value is not in the allowed enum");
    }
    if (schema.contains("type")) {
        const auto type = schema["type"].get<std::string>();
        bool valid = type == "object" ? value.is_object() : type == "array" ? value.is_array() :
                     type == "string" ? value.is_string() : type == "integer" && value.is_number_integer();
        if (!valid) fail("expected " + type);
    }
    if (value.is_object()) {
        if (schema.contains("maxProperties") && value.size() > schema["maxProperties"].get<size_t>()) fail("too many fields");
        for (const auto& required : schema.value("required", Json::array()))
            if (!value.contains(required.get<std::string>())) fail("missing field " + required.get<std::string>());
        const auto properties = schema.value("properties", Json::object());
        for (const auto& [key, item] : value.items()) {
            if (properties.contains(key)) validate(item, properties[key], path + "." + key);
            else if (!schema.value("additionalProperties", true)) fail("unknown field " + key);
        }
    }
    if (value.is_array()) {
        if (schema.contains("maxItems") && value.size() > schema["maxItems"].get<size_t>()) fail("too many items");
        for (size_t i = 0; i < value.size(); ++i) validate(value[i], schema.at("items"), path + "[" + std::to_string(i) + "]");
    }
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        if (text.find('\0') != std::string::npos) fail("NUL is not allowed");
        if (schema.contains("minLength") && text.size() < schema["minLength"].get<size_t>()) fail("string is too short");
        if (schema.contains("maxLength") && text.size() > schema["maxLength"].get<size_t>()) fail("string is too long");
        if (schema.contains("pattern") && !std::regex_match(text, std::regex(schema["pattern"].get<std::string>()))) fail("invalid identifier");
    }
    if (value.is_number()) {
        if (schema.contains("minimum") && value < schema["minimum"]) fail("value is below minimum");
        if (schema.contains("maximum") && value > schema["maximum"]) fail("value is above maximum");
    }
    if (schema.contains("oneOf")) {
        int matches = 0;
        for (const auto& alternative : schema["oneOf"]) {
            try { validate(value, alternative, path); ++matches; }
            catch (const std::runtime_error&) { }
        }
        if (matches != 1) fail("expected vertex and fragment stages, or a compute stage");
    }
}

Manifest Manifest::load(const std::filesystem::path& input) {
    Manifest manifest;
    manifest.path = std::filesystem::absolute(input);
    std::ifstream stream(input);
    if (!stream) throw std::runtime_error("cannot read manifest: " + input.string());
    // Reject duplicate JSON keys instead of silently keeping the last value.
    std::vector<std::set<std::string>> keys;
    auto callback = [&](int, Json::parse_event_t event, Json& parsed) {
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key && !keys.back().insert(parsed.get<std::string>()).second)
            throw std::runtime_error("duplicate JSON key: " + parsed.get<std::string>());
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    manifest.document = Json::parse(stream, callback);
    validate(manifest.document, Json::parse(manifest_schema), "manifest");
    if (!manifest.document["stages"].contains("compute") && !manifest.document.contains("clip_depth"))
        throw std::runtime_error("missing field clip_depth for a raster program");
    std::set<std::pair<std::string, int>> slots;
    std::set<std::tuple<std::string, uint32_t, uint32_t>> descriptors;
    for (const auto& resource : manifest.document["resources"]) {
        Binding binding{resource["kind"], resource["slot"], resource["stage"], resource["set"], resource["binding"], resource.value("access", "read"), resource.value("format", "")};
        const std::string name = resource["name"];
        const auto space = binding.kind == "uniform" || binding.kind == "sampler" ? binding.kind : "view";
        const int limit = binding.kind == "uniform" ? 8 : binding.kind == "sampler" ? 12 : 32;
        if (binding.slot >= limit) throw std::runtime_error(name + ": slot must be in 0.." + std::to_string(limit - 1));
        if (!slots.emplace(space, binding.slot).second) throw std::runtime_error("duplicate " + space + " slot");
        if (!descriptors.emplace(binding.stage, binding.set, binding.binding).second) throw std::runtime_error("duplicate SPIR-V descriptor");
        if (!manifest.bindings.emplace(name, binding).second) throw std::runtime_error("duplicate public resource name: " + name);
        if (resource.contains("sample_type") != (binding.kind == "texture")) throw std::runtime_error(name + ": sample_type is required only for textures");
        if (resource.contains("sampler_type") != (binding.kind == "sampler")) throw std::runtime_error(name + ": sampler_type is required only for samplers");
        if (resource.contains("access") && binding.kind != "storage_buffer" && binding.kind != "storage_image")
            throw std::runtime_error(name + ": access is only valid for storage buffers and images");
        if (resource.contains("format") != (binding.kind == "storage_image"))
            throw std::runtime_error(name + ": format is required only for storage images");
        if (binding.kind == "storage_buffer" && binding.access == "write")
            throw std::runtime_error(name + ": storage buffer access must be read or read_write");
        if (binding.kind == "storage_image") {
            if (!resource.contains("access") || binding.access == "read")
                throw std::runtime_error(name + ": storage image access must be explicit write or read_write");
            if (binding.stage != "compute")
                throw std::runtime_error(name + ": Sokol permits storage images only in compute shaders");
        }
        if (!manifest.document["stages"].contains(binding.stage)) throw std::runtime_error(name + ": resource stage is not in this program");
    }
    return manifest;
}

std::filesystem::path Manifest::stage_path(const std::string& stage) const {
    return (path.parent_path() / document["stages"][stage]["file"].get<std::string>()).lexically_normal();
}
