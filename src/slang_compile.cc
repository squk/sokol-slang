#include "slang_compile.h"
#include <cstring>
#include <set>
#include <slang-com-ptr.h>
#include <slang.h>

namespace sokol_slang {
namespace {
std::string blob_text(ISlangBlob *blob) {
    if (!blob || !blob->getBufferSize())
        return {};
    return {static_cast<const char *>(blob->getBufferPointer()), blob->getBufferSize()};
}
} // namespace

SourceResult compile_source(const SourceRequest &request) {
    SourceResult result;
    auto fail = [&](const char *phase, const char *text) {
        result.diagnostics.push_back({phase, text});
        return result;
    };
    auto diagnostic = [&](const char *phase, ISlangBlob *blob) {
        if (blob && blob->getBufferSize())
            result.diagnostics.push_back({phase, blob_text(blob)});
    };
    if (request.module_name.empty() || request.entry_points.empty() ||
        request.source.find('\0') != std::string::npos)
        return fail("input",
                    "Supply a module name, entry points and source without embedded null bytes");
    std::set<std::string> names;
    for (const auto &name : request.entry_points)
        if (name.empty() || !names.insert(name).second)
            return fail("input", "Entry point names must be nonempty and unique");

    Slang::ComPtr<slang::IGlobalSession> global;
    if (SLANG_FAILED(slang::createGlobalSession(global.writeRef())))
        return fail("session", "Cannot create Slang global session");
    result.compiler_identity = global->getBuildTagString();
    slang::TargetDesc target;
    target.format = SLANG_SPIRV;
    target.profile = global->findProfile(request.profile.c_str());
    if (target.profile == SLANG_PROFILE_UNKNOWN)
        return fail("input", "Unknown Slang target profile");
    std::vector<const char *> paths;
    for (const auto &path : request.search_paths)
        paths.push_back(path.c_str());
    slang::SessionDesc desc;
    desc.targets = &target;
    desc.targetCount = 1;
    desc.searchPaths = paths.data();
    desc.searchPathCount = paths.size();
    Slang::ComPtr<slang::ISession> session;
    if (SLANG_FAILED(global->createSession(desc, session.writeRef())))
        return fail("session", "Cannot create Slang compilation session");
    Slang::ComPtr<ISlangBlob> messages;
    auto *module = session->loadModuleFromSourceString(request.module_name.c_str(),
                                                       request.source_path.c_str(),
                                                       request.source.c_str(), messages.writeRef());
    diagnostic("load", messages);
    if (!module)
        return result;
    for (int i = 0; i < module->getDependencyFileCount(); ++i)
        result.dependency_paths.emplace_back(module->getDependencyFilePath(i));
    std::vector<Slang::ComPtr<slang::IEntryPoint>> entries;
    std::vector<slang::IComponentType *> components{module};
    for (const auto &name : request.entry_points) {
        Slang::ComPtr<slang::IEntryPoint> entry;
        if (SLANG_FAILED(module->findEntryPointByName(name.c_str(), entry.writeRef())))
            return fail("entry", "Requested entry point does not exist");
        components.push_back(entry.get());
        entries.push_back(entry);
    }
    Slang::ComPtr<slang::IComponentType> composite;
    messages.setNull();
    auto status = session->createCompositeComponentType(components.data(), components.size(),
                                                        composite.writeRef(), messages.writeRef());
    diagnostic("compose", messages);
    if (SLANG_FAILED(status))
        return result;
    Slang::ComPtr<slang::IComponentType> linked;
    messages.setNull();
    status = composite->link(linked.writeRef(), messages.writeRef());
    diagnostic("link", messages);
    if (SLANG_FAILED(status))
        return result;
    auto *layout = linked->getLayout(0);
    Slang::ComPtr<ISlangBlob> reflection;
    if (!layout || SLANG_FAILED(layout->toJson(reflection.writeRef())))
        return fail("reflection", "Cannot serialize Slang program reflection");
    result.reflection_json = blob_text(reflection);
    std::vector<StageCode> stages;
    for (size_t i = 0; i < request.entry_points.size(); ++i) {
        Slang::ComPtr<ISlangBlob> code;
        messages.setNull();
        status = linked->getEntryPointCode(i, 0, code.writeRef(), messages.writeRef());
        diagnostic("code", messages);
        if (SLANG_FAILED(status))
            return result;
        if (!code || !code->getBufferSize() || code->getBufferSize() % sizeof(uint32_t))
            return fail("code", "Slang returned invalid SPIR-V byte length");
        StageCode stage;
        stage.entry_point = request.entry_points[i];
        stage.spirv.resize(code->getBufferSize() / sizeof(uint32_t));
        std::memcpy(stage.spirv.data(), code->getBufferPointer(), code->getBufferSize());
        stages.push_back(std::move(stage));
    }
    result.stages = std::move(stages);
    result.succeeded = true;
    return result;
}
} // namespace sokol_slang
