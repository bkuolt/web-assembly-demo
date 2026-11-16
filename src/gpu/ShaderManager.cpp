/**
 * @file ShaderManager.cpp
 * @brief Implementation of modular WGSL shader `#embed` storage, `#include` preprocessor, and hot-reload tracker.
 */
#include "gpu/ShaderManager.hpp"
#include "core/Log.hpp"
#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <ranges>
#include <sstream>
#include <unordered_set>
#if defined(BGL_PLATFORM_NATIVE)
#include <webgpu/wgpu.h>
#endif

namespace bgl::gpu {

namespace {

#if defined(__has_embed)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wc++26-extensions"
#endif

constexpr char kEmbCommonUniforms[] = {
#embed "shaders/common/uniforms.wgsl"
};
constexpr char kEmbCommonGgxSample[] = {
#embed "shaders/common/ggx_sample.wgsl"
};
constexpr char kEmbCommonWeather[] = {
#embed "shaders/common/weather.wgsl"
};
constexpr char kEmbCommonGpuBounds[] = {
#embed "shaders/common/gpu_bounds.wgsl"
};
constexpr char kEmbBrdfLut[] = {
#embed "shaders/brdf_lut.wgsl"
};
constexpr char kEmbGgxPrefilter[] = {
#embed "shaders/ggx_prefilter.wgsl"
};
constexpr char kEmbTonemap[] = {
#embed "shaders/tonemap.wgsl"
};
constexpr char kEmbShadow[] = {
#embed "shaders/shadow.wgsl"
};
constexpr char kEmbPbr[] = {
#embed "shaders/pbr.wgsl"
};
constexpr char kEmbAabb[] = {
#embed "shaders/aabb.wgsl"
};
constexpr char kEmbWireframe[] = {
#embed "shaders/wireframe.wgsl"
};
constexpr char kEmbMipGen[] = {
#embed "shaders/mipgen.wgsl"
};
constexpr char kEmbSkybox[] = {
#embed "shaders/skybox.wgsl"
};
constexpr char kEmbGpuCull[] = {
#embed "shaders/gpu_cull.wgsl"
};
constexpr char kEmbGpuRayPick[] = {
#embed "shaders/gpu_ray_pick.wgsl"
};

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

struct EmbeddedEntry {
    std::string_view path;
    std::string_view bytes;
};

constexpr std::array<EmbeddedEntry, 15> kEmbeddedShaders{{
    {"common/uniforms.wgsl", {kEmbCommonUniforms, sizeof(kEmbCommonUniforms)}},
    {"common/ggx_sample.wgsl", {kEmbCommonGgxSample, sizeof(kEmbCommonGgxSample)}},
    {"common/weather.wgsl", {kEmbCommonWeather, sizeof(kEmbCommonWeather)}},
    {"common/gpu_bounds.wgsl", {kEmbCommonGpuBounds, sizeof(kEmbCommonGpuBounds)}},
    {"brdf_lut.wgsl", {kEmbBrdfLut, sizeof(kEmbBrdfLut)}},
    {"ggx_prefilter.wgsl", {kEmbGgxPrefilter, sizeof(kEmbGgxPrefilter)}},
    {"tonemap.wgsl", {kEmbTonemap, sizeof(kEmbTonemap)}},
    {"shadow.wgsl", {kEmbShadow, sizeof(kEmbShadow)}},
    {"pbr.wgsl", {kEmbPbr, sizeof(kEmbPbr)}},
    {"aabb.wgsl", {kEmbAabb, sizeof(kEmbAabb)}},
    {"wireframe.wgsl", {kEmbWireframe, sizeof(kEmbWireframe)}},
    {"mipgen.wgsl", {kEmbMipGen, sizeof(kEmbMipGen)}},
    {"skybox.wgsl", {kEmbSkybox, sizeof(kEmbSkybox)}},
    {"gpu_cull.wgsl", {kEmbGpuCull, sizeof(kEmbGpuCull)}},
    {"gpu_ray_pick.wgsl", {kEmbGpuRayPick, sizeof(kEmbGpuRayPick)}},
}};
#endif

[[nodiscard]] std::string TrimView(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r')) {
        sv.remove_suffix(1);
    }
    return std::string{sv};
}

[[nodiscard]] std::string NormalizeVirtualPath(std::string_view path) {
    std::filesystem::path p{path};
    std::string norm{p.lexically_normal().generic_string()};
    while (norm.starts_with("./")) {
        norm.erase(0, 2);
    }
    return norm;
}

} // namespace

ShaderManager::ShaderManager() : shaderDir_{DetectDefaultShaderDirectory()} {}

ShaderManager& ShaderManager::Instance() {
    static ShaderManager instance{};
    return instance;
}

std::string_view ShaderManager::GetEmbeddedRaw(std::string_view virtualPath) noexcept {
#if defined(__has_embed)
    const std::string norm{NormalizeVirtualPath(virtualPath)};
    for (const auto& entry : kEmbeddedShaders) {
        if (entry.path == norm) {
            return entry.bytes;
        }
    }
#else
    (void)virtualPath;
#endif
    return {};
}

std::string_view ShaderManager::GetEmbeddedPreprocessed(std::string_view virtualPath) {
    static std::mutex cacheMutex{};
    static std::unordered_map<std::string, std::string> cache{};

    const std::string norm{NormalizeVirtualPath(virtualPath)};
    std::lock_guard lock{cacheMutex};
    if (const auto it = cache.find(norm); it != cache.end()) {
        return it->second;
    }

    auto res = PreprocessWithLookup(norm, [](std::string_view p) -> core::Result<std::string> {
        const std::string_view raw{GetEmbeddedRaw(p)};
        if (raw.empty()) {
            return std::unexpected("Embedded WGSL shader not found: " + std::string{p});
        }
        return std::string{raw};
    });
    if (!res) {
        BGL_LOG_ERROR("Failed to preprocess embedded WGSL shader '{}': {}", norm, res.error());
        return {};
    }
    auto [insertedIt, ok] = cache.emplace(norm, std::move(res->code));
    (void)ok;
    return insertedIt->second;
}

std::filesystem::path ShaderManager::DetectDefaultShaderDirectory() const {
#if defined(BGL_PLATFORM_NATIVE)
    static constexpr std::array<std::string_view, 4> kCandidates{
        "src/gpu/shaders",
        "../src/gpu/shaders",
        "../../src/gpu/shaders",
        "shaders",
    };
    std::error_code ec;
    for (std::string_view candidate : kCandidates) {
        std::filesystem::path p{candidate};
        if (std::filesystem::exists(p / "pbr.wgsl", ec) && std::filesystem::is_directory(p, ec)) {
            return p;
        }
    }
#endif
    return {};
}

void ShaderManager::SetShaderDirectory(std::filesystem::path dir) {
    std::lock_guard lock{mutex_};
    shaderDir_ = std::move(dir);
    trackedEntries_.clear();
}

std::filesystem::path ShaderManager::GetShaderDirectory() const {
    std::lock_guard lock{mutex_};
    return shaderDir_;
}

std::string ShaderManager::ResolveIncludePath(std::string_view includingVirtualPath, std::string_view includeTarget) {
    std::string targetNorm{NormalizeVirtualPath(includeTarget)};
    // First prefer path relative to the directory of `includingVirtualPath` if target has ".." or "./" or isn't rooted
    const std::filesystem::path baseDir{std::filesystem::path{includingVirtualPath}.parent_path()};
    if (!baseDir.empty() && !targetNorm.starts_with(baseDir.generic_string() + "/")) {
        const std::string relCandidate{NormalizeVirtualPath((baseDir / includeTarget).generic_string())};
        if (!relCandidate.starts_with("..")) {
            if (!GetEmbeddedRaw(relCandidate).empty() || targetNorm.starts_with("..")) {
                return relCandidate;
            }
        }
    }
    return targetNorm;
}

core::Result<std::string> ShaderManager::ReadSourceFile(std::string_view virtualPath) const {
    const std::string norm{NormalizeVirtualPath(virtualPath)};
    if (norm.starts_with("..")) {
        return std::unexpected("Invalid WGSL shader path traversal outside shader root: " + norm);
    }

    std::filesystem::path dirCopy{};
    {
        std::lock_guard lock{mutex_};
        dirCopy = shaderDir_;
    }

#if defined(BGL_PLATFORM_NATIVE)
    if (!dirCopy.empty()) {
        std::error_code ec;
        const std::filesystem::path fullPath{dirCopy / norm};
        if (std::filesystem::exists(fullPath, ec) && std::filesystem::is_regular_file(fullPath, ec)) {
            std::ifstream ifs{fullPath, std::ios::binary};
            if (ifs) {
                std::ostringstream ss;
                ss << ifs.rdbuf();
                return ss.str();
            }
        }
    }
#endif

    const std::string_view embedded{GetEmbeddedRaw(norm)};
    if (!embedded.empty()) {
        return std::string{embedded};
    }
    return std::unexpected("WGSL shader source not found on disk or in #embed bundle: " + norm);
}

core::Result<PreprocessedShader> ShaderManager::PreprocessWithLookup(std::string_view entryVirtualPath,
                                                                     const SourceLookupFn& lookup, size_t maxDepth) {
    PreprocessedShader result{};
    std::unordered_set<std::string> pragmaOnceSet{};
    std::vector<std::string> activeStack{};
    std::unordered_set<std::string> recordedDeps{};
    std::unordered_map<std::string, std::string> sourceCache{};

    auto cachedLookup = [&](const std::string& path) -> core::Result<std::string_view> {
        if (auto it = sourceCache.find(path); it != sourceCache.end()) {
            return std::string_view{it->second};
        }
        auto res = lookup(path);
        if (!res) {
            return std::unexpected(res.error());
        }
        auto [insertedIt, _] = sourceCache.emplace(path, std::move(*res));
        return std::string_view{insertedIt->second};
    };

    auto processFile = [&](auto& self, std::string_view currentPath, size_t depth) -> core::Result<void> {
        if (depth > maxDepth) {
            return std::unexpected("WGSL #include recursion depth exceeded (" + std::to_string(maxDepth) + ") at " +
                                   std::string{currentPath});
        }

        const std::string normPath{NormalizeVirtualPath(currentPath)};
        if (std::ranges::find(activeStack, normPath) != activeStack.end()) {
            return std::unexpected("Circular WGSL #include detected: " + normPath);
        }
        if (pragmaOnceSet.contains(normPath)) {
            return {};
        }

        auto srcRes = cachedLookup(normPath);
        if (!srcRes) {
            return std::unexpected(srcRes.error());
        }

        if (recordedDeps.insert(normPath).second) {
            result.dependencyVirtualPaths.push_back(normPath);
        }

        activeStack.push_back(normPath);

        std::string_view remaining{*srcRes};
        while (!remaining.empty()) {
            const size_t nlPos{remaining.find('\n')};
            std::string_view line{nlPos == std::string_view::npos ? remaining : remaining.substr(0, nlPos)};
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            remaining = (nlPos == std::string_view::npos) ? std::string_view{} : remaining.substr(nlPos + 1);

            const std::string trimmed{TrimView(line)};
            if (trimmed == "#pragma once") {
                pragmaOnceSet.insert(normPath);
                continue;
            }

            if (trimmed.starts_with("#include")) {
                std::string_view rest{std::string_view{trimmed}.substr(8)};
                const std::string arg{TrimView(rest)};
                if (arg.size() < 3 || arg.front() != '"' || arg.back() != '"') {
                    activeStack.pop_back();
                    return std::unexpected(
                        std::format("Malformed WGSL #include directive in '{}': {}", normPath, trimmed));
                }
                const std::string target{arg.substr(1, arg.size() - 2)};
                std::string resolved{ResolveIncludePath(normPath, target)};
                // If relative resolution fails when in a subdirectory, fall back to shader root using cachedLookup.
                if (resolved.find('/') != std::string::npos && !cachedLookup(resolved).has_value()) {
                    std::string rootCandidate{NormalizeVirtualPath(target)};
                    if (cachedLookup(rootCandidate).has_value()) {
                        resolved = std::move(rootCandidate);
                    }
                }
                if (auto subRes = self(self, resolved, depth + 1); !subRes) {
                    activeStack.pop_back();
                    return std::unexpected(subRes.error());
                }
                result.code.push_back('\n');
                continue;
            }

            result.code.append(line);
            result.code.push_back('\n');
        }

        activeStack.pop_back();
        return {};
    };

    if (auto res = processFile(processFile, entryVirtualPath, 0); !res) {
        return std::unexpected(res.error());
    }
    return result;
}

core::Result<PreprocessedShader> ShaderManager::Preprocess(std::string_view virtualPath) const {
    return PreprocessWithLookup(virtualPath, [this](std::string_view path) { return this->ReadSourceFile(path); });
}

core::Result<std::string> ShaderManager::LoadShader(std::string_view virtualPath) {
    const std::string normEntry{NormalizeVirtualPath(virtualPath)};
    auto res = Preprocess(normEntry);
    if (!res) {
        return std::unexpected(res.error());
    }

    std::lock_guard lock{mutex_};
    std::vector<TrackedDep> deps{};
    if (!shaderDir_.empty()) {
        std::error_code ec;
        deps.reserve(res->dependencyVirtualPaths.size());
        for (const auto& depVirtual : res->dependencyVirtualPaths) {
            TrackedDep td{};
            td.diskPath = shaderDir_ / depVirtual;
            td.existedOnDisk =
                std::filesystem::exists(td.diskPath, ec) && std::filesystem::is_regular_file(td.diskPath, ec);
            if (td.existedOnDisk) {
                td.lastWriteTime = std::filesystem::last_write_time(td.diskPath, ec);
            }
            deps.push_back(std::move(td));
        }
    }
    trackedEntries_[normEntry] = std::move(deps);
    return std::move(res->code);
}

core::Result<ShaderModuleHandle> ShaderManager::LoadAndCompile(WGPUDevice device, std::string_view virtualPath,
                                                               std::string_view label, bool validateScope) {
    auto codeRes = LoadShader(virtualPath);
    if (!codeRes) {
        return std::unexpected(codeRes.error());
    }
    return CompileWgslModule(device, *codeRes, label, validateScope);
}

core::Result<ShaderModuleHandle> ShaderManager::CompileWgslModule(WGPUDevice device, std::string_view code,
                                                                  std::string_view label, bool validateScope) {
    if (!device) {
        return std::unexpected("Cannot compile WGSL shader module on null WebGPU device");
    }
    if (code.empty()) {
        return std::unexpected("Cannot compile empty WGSL shader source for '" + std::string{label} + "'");
    }

#if defined(BGL_PLATFORM_NATIVE)
    struct PopScopeState {
        bool done{false};
        WGPUErrorType errorType{WGPUErrorType_NoError};
        std::string message{};
    } popState{};

    if (validateScope) {
        wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
    }
#else
    (void)validateScope;
#endif

    WGPUShaderSourceWGSL wgslDesc = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgslDesc.chain.next = nullptr;
    wgslDesc.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgslDesc.code = ToWgpuStringView(code);

    WGPUShaderModuleDescriptor smDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    smDesc.nextInChain = &wgslDesc.chain;
    smDesc.label = ToWgpuStringView(label);

    ShaderModuleHandle shaderModule{wgpuDeviceCreateShaderModule(device, &smDesc)};

#if defined(BGL_PLATFORM_NATIVE)
    if (validateScope) {
        WGPUPopErrorScopeCallbackInfo popInfo{};
        popInfo.mode = WGPUCallbackMode_AllowProcessEvents;
        popInfo.userdata1 = &popState;
        popInfo.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView msg, void* userdata1,
                              void* userdata2) {
            (void)userdata2;
            auto* state = static_cast<PopScopeState*>(userdata1);
            state->done = true;
            if (status == WGPUPopErrorScopeStatus_Success && type != WGPUErrorType_NoError) {
                state->errorType = type;
                if (msg.data) {
                    const size_t len = (msg.length == WGPU_STRLEN) ? std::string_view{msg.data}.size() : msg.length;
                    state->message.assign(msg.data, len);
                }
            }
        };
        wgpuDevicePopErrorScope(device, popInfo);
        wgpuDevicePoll(device, WGPU_TRUE, nullptr);
        if (popState.errorType != WGPUErrorType_NoError) {
            return std::unexpected("WGSL validation error in '" + std::string{label} + "': " + popState.message);
        }
    }
#endif

    if (!shaderModule) {
        return std::unexpected("Failed to create WGSL shader module '" + std::string{label} + "'");
    }
    return shaderModule;
}

std::vector<std::string> ShaderManager::PollModifiedShaders() {
    std::vector<std::string> modified{};
#if defined(BGL_PLATFORM_NATIVE)
    std::lock_guard lock{mutex_};
    if (shaderDir_.empty()) {
        return modified;
    }

    std::error_code ec;
    for (auto& [entryName, deps] : trackedEntries_) {
        bool changed = false;
        for (auto& dep : deps) {
            const bool nowExists =
                std::filesystem::exists(dep.diskPath, ec) && std::filesystem::is_regular_file(dep.diskPath, ec);
            if (nowExists != dep.existedOnDisk) {
                dep.existedOnDisk = nowExists;
                if (nowExists) {
                    dep.lastWriteTime = std::filesystem::last_write_time(dep.diskPath, ec);
                }
                changed = true;
            } else if (nowExists) {
                const auto currentTime = std::filesystem::last_write_time(dep.diskPath, ec);
                if (!ec && currentTime != dep.lastWriteTime) {
                    dep.lastWriteTime = currentTime;
                    changed = true;
                }
            }
        }
        if (changed) {
            modified.push_back(entryName);
        }
    }
#endif
    return modified;
}

} // namespace bgl::gpu
