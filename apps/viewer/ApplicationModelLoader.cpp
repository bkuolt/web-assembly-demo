/**
 * @file ApplicationModelLoader.cpp
 * @brief Built-in glTF model preset catalog, async/sync scene loading, node selection, and camera framing.
 */
#include "core/Log.hpp"
#include "core/NetworkFetcher.hpp"
#include "viewer/Application.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <format>
#include <mutex>
#include <thread>

namespace bgl::app {

namespace {

constexpr std::array<ModelPresetEntry, 19> kBuiltinModelPresets{{
    {"ToyCar (5.1 MB)", "assets/ToyCar.glb",
     "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/ToyCar/glTF-Binary/ToyCar.glb"},
    {"Sponza (52.6 MB)", "assets/Sponza.glb",
     "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Sponza/glTF/Sponza.gltf"},
    {"RobotExpressive (0.5 MB)", "assets/RobotExpressive.glb",
     "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/RobotExpressive/glTF-Binary/"
     "RobotExpressive.glb"},
    {"Soldier (2.1 MB)", "assets/Soldier.glb",
     "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Soldier/glTF-Binary/Soldier.glb"},
    {"DamagedHelmet (3.6 MB)", nullptr,
     "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/DamagedHelmet/glTF-Binary/"
     "DamagedHelmet.glb"},
    {"AntiqueCamera (16.7 MB)", nullptr,
     "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/AntiqueCamera/glTF-Binary/"
     "AntiqueCamera.glb"},
    {"BoomBox (10.1 MB)", nullptr,
     "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/BoomBox/glTF-Binary/BoomBox.glb"},
    {"WaterBottle (8.6 MB)", nullptr,
     "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/WaterBottle/glTF-Binary/"
     "WaterBottle.glb"},
    {"MaterialsVariantsShoe (7.5 MB)", nullptr,
     "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/MaterialsVariantsShoe/glTF-Binary/"
     "MaterialsVariantsShoe.glb"},
    {"Avocado (7.7 MB)", nullptr,
     "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Avocado/glTF-Binary/Avocado.glb"},
    {"BarramundiFish (11.9 MB)", nullptr,
     "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/BarramundiFish/glTF-Binary/"
     "BarramundiFish.glb"},
    {"Lantern (1.2 MB)", nullptr,
     "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Lantern/glTF-Binary/Lantern.glb"},
    {"Corset (0.3 MB)", nullptr,
     "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Corset/glTF-Binary/Corset.glb"},
    {"Fox (0.16 MB)", nullptr,
     "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Fox/glTF-Binary/Fox.glb"},
    {"CesiumMan (0.42 MB)", nullptr,
     "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/CesiumMan/glTF-Binary/"
     "CesiumMan.glb"},
    {"BrainStem (3.05 MB)", nullptr,
     "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/BrainStem/glTF-Binary/"
     "BrainStem.glb"},
    {"ClearCoatTest (0.25 MB)", nullptr,
     "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/ClearCoatTest/glTF-Binary/"
     "ClearCoatTest.glb"},
    {"MorphPrimitivesTest (0.05 MB)", nullptr,
     "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/MorphPrimitivesTest/glTF-Binary/"
     "MorphPrimitivesTest.glb"},
    {"Duck (0.11 MB)", nullptr,
     "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Duck/glTF-Binary/Duck.glb"},
}};

[[nodiscard]] std::string ToLowerAscii(std::string_view input) {
    std::string out{input};
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

[[nodiscard]] bool MatchesPresetFilename(std::string_view lowerUri, std::string_view lowerDisplayName,
                                         std::string_view lowerPresetFile) noexcept {
    if (lowerPresetFile.empty()) {
        return false;
    }
    if (lowerDisplayName == lowerPresetFile || lowerUri == lowerPresetFile) {
        return true;
    }
    if (lowerUri.size() > lowerPresetFile.size() && lowerUri.ends_with(lowerPresetFile)) {
        const char sep{lowerUri[lowerUri.size() - lowerPresetFile.size() - 1]};
        return sep == '/' || sep == '\\' || sep == ':';
    }
    return false;
}

} // namespace

struct AsyncLoadState {
    std::string sourceUri{};
    std::string displayLabel{};
    std::atomic<float> progress{0.0f};
    std::atomic<bool> cancelRequested{false};
    std::atomic<bool> finished{false};
    std::mutex stateMutex{};
    std::string statusText{"Preparing..."};
    core::Result<std::shared_ptr<scene::Scene>> result{std::unexpected("Loading...")};

    void SetProgress(float p, std::string text) {
        progress.store(std::clamp(p, 0.0f, 1.0f), std::memory_order_release);
        std::scoped_lock lock(stateMutex);
        statusText = std::move(text);
    }

    [[nodiscard]] std::string GetStatus() {
        std::scoped_lock lock(stateMutex);
        return statusText;
    }
};

struct Application::AsyncLoadTask {
    std::shared_ptr<AsyncLoadState> state{};
#if defined(BGL_PLATFORM_NATIVE)
    std::jthread worker{};
#endif
};

Application::Application() = default;

Application::~Application() {
    ShutdownImpl();
}

std::span<const ModelPresetEntry> Application::GetBuiltinModelPresets() noexcept {
    return kBuiltinModelPresets;
}

std::string Application::ResolveModelPresetUri(const ModelPresetEntry& preset) {
    if (preset.localRelPath && preset.localRelPath[0] != '\0') {
        const std::string_view rel{preset.localRelPath};
        const std::array<std::string, 6> candidates{
            std::string{rel},
            std::string{"web/"} + std::string{rel},
            std::string{"../"} + std::string{rel},
            std::string{"../web/"} + std::string{rel},
            std::string{"../../"} + std::string{rel},
            std::string{"../../web/"} + std::string{rel},
        };
        std::error_code ec{};
        for (const auto& candidate : candidates) {
            if (std::filesystem::exists(candidate, ec) && std::filesystem::is_regular_file(candidate, ec)) {
                return candidate;
            }
        }
        if (!preset.remoteUrl || preset.remoteUrl[0] == '\0') {
            return std::string{rel};
        }
    }
    return preset.remoteUrl ? std::string{preset.remoteUrl} : std::string{};
}

void Application::SetLoadingSucceededState() {
    loadingTitle_ = std::format("Loaded {}", core::SanitizeForLog(currentModelName_));
    loadingStatus_ = std::format("Ready: {} primitives, {} tex", totalPrimitives_,
                                 activeScene_ ? activeScene_->gpuTextures.size() : 0u);
    loadingTargetProgress_ = 1.0f;
    loadingVisualProgress_ = 1.0f;
    loadingHasError_ = false;
    loadingBannerTimer_ = 1.5f;
}

void Application::SetLoadingFailedState(std::string_view titleName, std::string_view errorMsg, bool revertSelection) {
    loadingTitle_ = std::format("Failed: {}", core::SanitizeForLog(titleName));
    loadingStatus_ = core::SanitizeForLog(errorMsg);
    loadingTargetProgress_ = 1.0f;
    loadingVisualProgress_ = 1.0f;
    loadingHasError_ = true;
    loadingBannerTimer_ = 4.0f;
    if (revertSelection) {
        selectedModelOption_ = lastCommittedModelOption_;
    }
}

void Application::RetireActiveLoadAsync() noexcept {
#if defined(BGL_PLATFORM_NATIVE)
    constexpr size_t kMaxCancellingLoads{4u};
    std::erase_if(cancellingLoads_, [](const std::unique_ptr<AsyncLoadTask>& t) {
        return !t || !t->state || t->state->finished.load(std::memory_order_acquire);
    });
    if (!activeLoad_) {
        return;
    }
    if (activeLoad_->state) {
        activeLoad_->state->cancelRequested.store(true, std::memory_order_release);
    }
    while (cancellingLoads_.size() >= kMaxCancellingLoads) {
        cancellingLoads_.erase(cancellingLoads_.begin());
    }
    cancellingLoads_.push_back(std::move(activeLoad_));
#endif
}

void Application::OnFileDrop(std::string_view filename, std::span<const uint8_t> data) {
    RetireActiveLoadAsync();
    const std::string displayName{core::NetworkFetcher::ExtractDisplayName(filename)};
    const std::string memoryBaseUri{"memory://" + displayName};
    BGL_LOG_INFO("Ingesting drag-and-dropped buffer '{}' ({} bytes)", core::SanitizeForLog(displayName), data.size());
    if (auto res = LoadScene(data, memoryBaseUri); res) {
        BGL_LOG_INFO("Successfully activated dropped glTF model '{}'", core::SanitizeForLog(displayName));
        SetLoadingSucceededState();
    } else {
        BGL_LOG_ERROR("Failed to load dropped glTF buffer '{}': {}", core::SanitizeForLog(displayName), res.error());
        SetLoadingFailedState(displayName, res.error(), false);
    }
}

void Application::LoadSceneFromUriAsync(std::string_view uri, std::string_view displayLabel) {
    const std::string sanitizedUri{core::NetworkFetcher::SanitizeUri(uri)};
    if (sanitizedUri.empty()) {
        SetLoadingFailedState("Empty URI", "Cannot load empty asset URI", false);
        return;
    }

    const std::string resolvedTitle =
        displayLabel.empty() ? core::NetworkFetcher::ExtractDisplayName(sanitizedUri) : std::string{displayLabel};

#if defined(BGL_PLATFORM_NATIVE)
    RetireActiveLoadAsync();

    auto state = std::make_shared<AsyncLoadState>();
    state->sourceUri = sanitizedUri;
    state->displayLabel = resolvedTitle;
    state->SetProgress(0.02f, core::NetworkFetcher::IsHttpUrl(sanitizedUri) ? "Connecting to remote host..."
                                                                            : "Reading local glTF/GLB file...");

    loadingTitle_ = std::format("Loading {}...", core::SanitizeForLog(resolvedTitle));
    loadingStatus_ = state->GetStatus();
    loadingTargetProgress_ = 0.02f;
    loadingVisualProgress_ = 0.02f;
    loadingBannerTimer_ = 0.0f;
    loadingHasError_ = false;

    auto task = std::make_unique<AsyncLoadTask>();
    task->state = state;
    // Main thread joins or retires worker via std::jthread before Shutdown destroys `this` or `assetManager_`.
    task->worker = std::jthread([this, state, sanitizedUri]() {
        scene::FastGltfLoader workerLoader{assetManager_};
        auto res = workerLoader.LoadFromFile(sanitizedUri, [state](const scene::LoadProgressInfo& info) -> bool {
            if (state->cancelRequested.load(std::memory_order_acquire)) {
                return false;
            }
            state->SetProgress(info.fraction, info.status);
            return true;
        });
        {
            std::scoped_lock lock(state->stateMutex);
            state->result = std::move(res);
        }
        state->finished.store(true, std::memory_order_release);
    });
    activeLoad_ = std::move(task);
#else
    if (auto loadRes = LoadSceneFromUri(sanitizedUri); !loadRes) {
        SetLoadingFailedState(resolvedTitle, loadRes.error(), true);
    } else {
        SetLoadingSucceededState();
    }
#endif
}

bool Application::IsLoadingScene() const noexcept {
    return activeLoad_ != nullptr;
}

float Application::GetLoadingProgress() const noexcept {
    return activeLoad_ ? loadingVisualProgress_ : loadingTargetProgress_;
}

const std::string& Application::GetLoadingStatus() const noexcept {
    return loadingStatus_;
}

void Application::PollAsyncSceneLoad(float deltaTime) {
    std::erase_if(cancellingLoads_, [](const std::unique_ptr<AsyncLoadTask>& t) {
        return !t || !t->state || t->state->finished.load(std::memory_order_acquire);
    });

    if (!activeLoad_ || !activeLoad_->state) {
        if (loadingBannerTimer_ > 0.0f) {
            loadingVisualProgress_ +=
                (loadingTargetProgress_ - loadingVisualProgress_) * std::min(1.0f, deltaTime * 18.0f);
            loadingBannerTimer_ = std::max(0.0f, loadingBannerTimer_ - deltaTime);
        }
        return;
    }

    const float actual = activeLoad_->state->progress.load(std::memory_order_acquire);
    loadingTargetProgress_ = std::max(loadingTargetProgress_, actual);
    loadingStatus_ = activeLoad_->state->GetStatus();
    loadingVisualProgress_ += (loadingTargetProgress_ - loadingVisualProgress_) * std::min(1.0f, deltaTime * 14.0f);

    if (!activeLoad_->state->finished.load(std::memory_order_acquire)) {
        return;
    }

    auto task = std::move(activeLoad_);
    auto& state = *task->state;
    if (state.cancelRequested.load(std::memory_order_acquire)) {
        return;
    }

    core::Result<std::shared_ptr<scene::Scene>> res{std::unexpected("Cancelled")};
    {
        std::scoped_lock lock(state.stateMutex);
        res = std::move(state.result);
    }

    if (res && *res) {
        loadingTargetProgress_ = 0.98f;
        loadingStatus_ = "Uploading GPU buffers & textures...";
        ActivateScene(*res, state.sourceUri);
        SetLoadingSucceededState();
    } else {
        const std::string errMsg = res ? std::string{"Loaded glTF scene is empty"} : res.error();
        BGL_LOG_ERROR("Failed to load glTF model '{}': {}", core::SanitizeForLog(state.sourceUri),
                      core::SanitizeForLog(errMsg));
        SetLoadingFailedState(core::NetworkFetcher::ExtractDisplayName(state.sourceUri), errMsg, true);
    }
}

void Application::CancelPendingSceneLoads() noexcept {
    if (activeLoad_ && activeLoad_->state) {
        activeLoad_->state->cancelRequested.store(true, std::memory_order_release);
    }
    for (const auto& t : cancellingLoads_) {
        if (t && t->state) {
            t->state->cancelRequested.store(true, std::memory_order_release);
        }
    }
    activeLoad_.reset();
    cancellingLoads_.clear();
}

void Application::SyncModelSelectionFromUri(std::string_view sourceUri) {
    const std::string lowerUri = ToLowerAscii(sourceUri);
    const std::string lowerName = ToLowerAscii(currentModelName_);
    for (size_t i = 0; i < kBuiltinModelPresets.size(); ++i) {
        const auto& preset = kBuiltinModelPresets[i];
        const std::string remoteName =
            preset.remoteUrl ? ToLowerAscii(core::NetworkFetcher::ExtractDisplayName(preset.remoteUrl)) : std::string{};
        const std::string localName = preset.localRelPath
                                          ? ToLowerAscii(core::NetworkFetcher::ExtractDisplayName(preset.localRelPath))
                                          : std::string{};
        const bool matchLocal = MatchesPresetFilename(lowerUri, lowerName, localName);
        const bool matchRemote = MatchesPresetFilename(lowerUri, lowerName, remoteName);
        if (matchLocal || matchRemote) {
            selectedModelOption_ = i;
            lastCommittedModelOption_ = i;
            customModelLabel_.clear();
            return;
        }
    }
    if (!currentModelName_.empty() && currentModelName_ != "None") {
        customModelLabel_ = std::format("Custom: {}", currentModelName_);
        selectedModelOption_ = kBuiltinModelPresets.size();
        lastCommittedModelOption_ = selectedModelOption_;
    }
}

} // namespace bgl::app
