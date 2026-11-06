#pragma once
#include "manifest.h"
#include <functional>
#include <set>

// Resolve inferred resources and caller constraints before backend translation.
// Explicit resource manifests keep their authored public names and slots.
Json resolve_program_bindings(Json &document, const std::filesystem::path &path,
                              const std::function<Json(const std::string &)> &inspect,
                              std::set<std::pair<std::string, std::string>> &used_overrides);
