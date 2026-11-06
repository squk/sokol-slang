#include "slang_compile.h"
#include <atomic>
#include <cstring>
#include <filesystem>
#include <set>
#include <slang-com-ptr.h>
#include <slang.h>

namespace sokol_slang {
namespace {
bool same_guid(const SlangUUID &a, const SlangUUID &b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

// Slang retains blobs and its filesystem through COM references. Neither
// object borrows the request, so backend work cannot outlive its source bytes.
class SourceBlob final : public ISlangBlob {
  public:
    explicit SourceBlob(std::string bytes) : bytes_(std::move(bytes)) {}
    SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(const SlangUUID &id,
                                                          void **out) override {
        *out = nullptr;
        if (!same_guid(id, ISlangUnknown::getTypeGuid()) &&
            !same_guid(id, ISlangBlob::getTypeGuid()))
            return SLANG_E_NO_INTERFACE;
        *out = static_cast<ISlangBlob *>(this);
        addRef();
        return SLANG_OK;
    }
    SLANG_NO_THROW uint32_t SLANG_MCALL addRef() override { return ++references_; }
    SLANG_NO_THROW uint32_t SLANG_MCALL release() override {
        const auto remaining = references_.fetch_sub(1) - 1;
        if (!remaining)
            delete this;
        return remaining;
    }
    SLANG_NO_THROW const void *SLANG_MCALL getBufferPointer() override { return bytes_.c_str(); }
    SLANG_NO_THROW size_t SLANG_MCALL getBufferSize() override { return bytes_.size(); }

  private:
    std::atomic<uint32_t> references_{1};
    std::string bytes_;
};

std::string source_path(const std::string &path) {
    return std::filesystem::path(path).lexically_normal().generic_string();
}

class SourceFiles final : public ISlangFileSystem {
  public:
    explicit SourceFiles(std::map<std::string, std::string> files) : files_(std::move(files)) {}
    SLANG_NO_THROW void *SLANG_MCALL castAs(const SlangUUID &id) override {
        if (same_guid(id, ISlangUnknown::getTypeGuid()) ||
            same_guid(id, ISlangCastable::getTypeGuid()) ||
            same_guid(id, ISlangFileSystem::getTypeGuid()))
            return static_cast<ISlangFileSystem *>(this);
        return nullptr;
    }
    SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(const SlangUUID &id,
                                                          void **out) override {
        *out = castAs(id);
        if (!*out)
            return SLANG_E_NO_INTERFACE;
        addRef();
        return SLANG_OK;
    }
    SLANG_NO_THROW uint32_t SLANG_MCALL addRef() override { return ++references_; }
    SLANG_NO_THROW uint32_t SLANG_MCALL release() override {
        const auto remaining = references_.fetch_sub(1) - 1;
        if (!remaining)
            delete this;
        return remaining;
    }
    SLANG_NO_THROW SlangResult SLANG_MCALL loadFile(const char *path, ISlangBlob **out) override {
        *out = nullptr;
        const auto found = files_.find(source_path(path));
        if (found == files_.end())
            return SLANG_E_NOT_FOUND;
        *out = new SourceBlob(found->second);
        return SLANG_OK;
    }

  private:
    std::atomic<uint32_t> references_{1};
    const std::map<std::string, std::string> files_;
};

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

    std::map<std::string, std::string> captured;
    for (const auto &[path, bytes] : request.files) {
        if (path.empty() || path.find('\0') != std::string::npos ||
            !captured.emplace(source_path(path), bytes).second)
            return fail("input",
                        "Captured source paths must be nonempty and unique after normalization");
    }
    if (request.source_path.empty() || request.source_path.find('\0') != std::string::npos ||
        !captured.emplace(source_path(request.source_path), request.source).second)
        return fail("input", "The root source needs a unique nonempty path");
    Slang::ComPtr<SourceFiles> files;
    files.attach(new SourceFiles(std::move(captured)));
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
    // Match the CLI's addressing and matrix contract before Sokol translation.
    slang::CompilerOptionEntry options[3] = {};
    options[0].name = slang::CompilerOptionName::VulkanUseEntryPointName;
    options[1].name = slang::CompilerOptionName::VulkanUseGLLayout;
    options[2].name = slang::CompilerOptionName::DebugInformation;
    for (auto &option : options)
        option.value.intValue0 = 1;
    slang::SessionDesc desc;
    desc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
    desc.compilerOptionEntries = options;
    desc.compilerOptionEntryCount = 3;
    desc.fileSystem = files.get();
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
        switch (layout->getEntryPointByIndex(i)->getStage()) {
        case SLANG_STAGE_VERTEX:
            stage.stage = "vertex";
            break;
        case SLANG_STAGE_FRAGMENT:
            stage.stage = "fragment";
            break;
        case SLANG_STAGE_COMPUTE:
            stage.stage = "compute";
            break;
        default:
            return fail("entry", "Sokol supports vertex, fragment and compute stages");
        }
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
