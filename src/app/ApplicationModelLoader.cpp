/**
 * @file ApplicationModelLoader.cpp
 * @brief Built-in glTF model preset catalog, async/sync scene loading, node selection, and camera framing.
 */
#include "app/Application.hpp"
#include "core/Log.hpp"
#include "core/NetworkFetcher.hpp"
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
    Shutdown();
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

void Application::ActivateScene(std::shared_ptr<scene::Scene> scene, std::string_view sourceUri) {
    if (!scene) {
        return;
    }
    ClearSelection();
    activeCameraIndex_ = -1;
    activeScene_ = std::move(scene);
    currentModelName_ = core::NetworkFetcher::ExtractDisplayName(sourceUri);
    SyncModelSelectionFromUri(sourceUri);
    CacheSceneMetrics();
    pipelineManager_.PrepareScene(*activeScene_);

    const glm::vec3 center{activeScene_->sceneAABB.Center()};
    const float radius{glm::length(activeScene_->sceneAABB.Extents())};

    if (sourceUri.find("Sponza") != std::string_view::npos || sourceUri.find("sponza") != std::string_view::npos) {
        const glm::vec3 courtyardCenter{center.x, 2.0f, center.z};
        constexpr float camDistance{10.0f};
        camera_ = core::Camera{courtyardCenter, camDistance};
        camera_.Orbit(1.2f, 0.15f);
        BGL_LOG_INFO("Sponza scene activated: courtyardCenter=[{:.2f}, {:.2f}, {:.2f}], camDistance={:.2f}",
                     courtyardCenter.x, courtyardCenter.y, courtyardCenter.z, camDistance);
    } else {
        const float targetDistance{std::max(radius * 2.5f, 0.15f)};
        camera_ = core::Camera{center, targetDistance};
        camera_.Orbit(0.8f, 0.4f);
        BGL_LOG_INFO("Scene '{}' activated: center=[{:.4f}, {:.4f}, {:.4f}], radius={:.4f}, camDistance={:.4f}",
                     currentModelName_, center.x, center.y, center.z, radius, targetDistance);
    }

    core::AABB worldBounds{activeScene_->sceneAABB};
    const glm::vec3 margin{glm::max(glm::vec3{10.0f}, worldBounds.Extents() * 0.1f)};
    worldBounds.minBound -= margin;
    worldBounds.maxBound += margin;
    quadtree_ = std::make_unique<scene::Quadtree>(worldBounds);
    quadtree_->InsertNodes(activeScene_->rootNodes);
    cachedQuadtreeNodeCount_ = quadtree_->GetNodeCount();
    lastQuadtreeTransformVersion_ = activeScene_->transformVersion_;

    if (!activeScene_->animations.empty()) {
        activeScene_->SetAnimation(0);
        BGL_LOG_INFO("Activated default glTF animation 0 ('{}', duration: {:.2f}s)",
                     activeScene_->animations[0].name.empty() ? "unnamed" : activeScene_->animations[0].name,
                     activeScene_->animations[0].duration);
    }
}

core::Result<void> Application::LoadScene(std::span<const uint8_t> data, std::string_view baseUri) {
    if (data.empty()) {
        return std::unexpected("Cannot load glTF scene from empty byte buffer");
    }
    if (!gltfLoader_) {
        gltfLoader_ = std::make_unique<scene::FastGltfLoader>(assetManager_);
    }
    auto sceneRes = gltfLoader_->LoadFromMemory(data, baseUri);
    if (!sceneRes) {
        return std::unexpected(sceneRes.error());
    }
    ActivateScene(*sceneRes, baseUri);
    return {};
}

core::Result<void> Application::LoadSceneFromUri(std::string_view uri) {
    const std::string sanitizedUri{core::NetworkFetcher::SanitizeUri(uri)};
    if (sanitizedUri.empty()) {
        return std::unexpected("Cannot load empty asset URI");
    }

    BGL_LOG_INFO("Loading glTF/GLB asset from {} '{}'...", core::NetworkFetcher::IsUrl(sanitizedUri) ? "URL" : "path",
                 sanitizedUri);
    auto fetchRes = core::NetworkFetcher::FetchSync(sanitizedUri);
    if (!fetchRes) {
        return std::unexpected("Failed to fetch glTF asset '" + sanitizedUri + "': " + fetchRes.error());
    }
    return LoadScene(*fetchRes, sanitizedUri);
}

void Application::SetLoadingSucceededState() {
    loadingTitle_ = std::format("Loaded {}", currentModelName_);
    loadingStatus_ = std::format("Ready: {} primitives, {} tex", totalPrimitives_,
                                 activeScene_ ? activeScene_->gpuTextures.size() : 0u);
    loadingTargetProgress_ = 1.0f;
    loadingVisualProgress_ = 1.0f;
    loadingHasError_ = false;
    loadingBannerTimer_ = 1.5f;
}

void Application::SetLoadingFailedState(std::string_view titleName, std::string_view errorMsg, bool revertSelection) {
    loadingTitle_ = std::format("Failed: {}", titleName);
    loadingStatus_ = std::string{errorMsg};
    loadingTargetProgress_ = 1.0f;
    loadingVisualProgress_ = 1.0f;
    loadingHasError_ = true;
    loadingBannerTimer_ = 4.0f;
    if (revertSelection) {
        selectedModelOption_ = lastCommittedModelOption_;
    }
}

void Application::OnFileDrop(std::string_view filename, std::span<const uint8_t> data) {
    BGL_LOG_INFO("Ingesting drag-and-dropped buffer '{}' ({} bytes)", filename, data.size());
    if (auto res = LoadScene(data, filename); res) {
        BGL_LOG_INFO("Successfully activated dropped glTF model '{}'", filename);
        SetLoadingSucceededState();
    } else {
        BGL_LOG_ERROR("Failed to load dropped glTF buffer '{}': {}", filename, res.error());
        SetLoadingFailedState(core::NetworkFetcher::ExtractDisplayName(filename), res.error(), false);
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
    if (activeLoad_) {
        if (activeLoad_->state) {
            activeLoad_->state->cancelRequested.store(true, std::memory_order_release);
        }
        cancellingLoads_.push_back(std::move(activeLoad_));
    }

    auto state = std::make_shared<AsyncLoadState>();
    state->sourceUri = sanitizedUri;
    state->displayLabel = resolvedTitle;
    state->SetProgress(0.02f, core::NetworkFetcher::IsHttpUrl(sanitizedUri) ? "Connecting to remote host..."
                                                                            : "Reading local glTF/GLB file...");

    loadingTitle_ = std::format("Loading {}...", resolvedTitle);
    loadingStatus_ = state->GetStatus();
    loadingTargetProgress_ = 0.02f;
    loadingVisualProgress_ = 0.02f;
    loadingBannerTimer_ = 0.0f;
    loadingHasError_ = false;

    auto task = std::make_unique<AsyncLoadTask>();
    task->state = state;
    // Worker captures only passive AsyncLoadState so std::jthread stays strictly owned by the main thread.
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

std::string Application::GetLoadingStatus() const {
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
        BGL_LOG_ERROR("Failed to load glTF model '{}': {}", state.sourceUri, errMsg);
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
        const bool matchLocal = !localName.empty() && (lowerName == localName || lowerUri.ends_with(localName));
        const bool matchRemote = !remoteName.empty() && (lowerName == remoteName || lowerUri.ends_with(remoteName));
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

void Application::CacheSceneMetrics() {
    totalPrimitives_ = 0;
    totalVertices_ = 0;
    totalIndices_ = 0;
    if (!activeScene_) {
        return;
    }

    size_t& primCount{totalPrimitives_};
    size_t& vertCount{totalVertices_};
    size_t& idxCount{totalIndices_};
    auto countStats = [&](this auto&& self, const scene::SceneNode& node) -> void {
        for (const auto& prim : node.GetPrimitives()) {
            primCount++;
            vertCount += prim.vertices.size();
            idxCount += prim.indices.size();
        }
        for (const auto& child : node.children) {
            if (child) {
                self(*child);
            }
        }
    };

    for (const auto& rootNode : activeScene_->rootNodes) {
        if (rootNode) {
            countStats(*rootNode);
        }
    }
    BGL_LOG_INFO("Scene '{}' metrics cached: {} primitives, {} vertices, {} indices", currentModelName_,
                 totalPrimitives_, totalVertices_, totalIndices_);
}

void Application::FocusSelection() noexcept {
    if (selectedNode_ && selectedNode_->worldAABB.minBound.x <= selectedNode_->worldAABB.maxBound.x) {
        camera_.FocusOnBounds(selectedNode_->worldAABB);
    } else if (activeScene_) {
        camera_.FocusOnBounds(activeScene_->sceneAABB);
    }
}

void Application::FrameAll() noexcept {
    if (activeScene_) {
        camera_.FocusOnBounds(activeScene_->sceneAABB);
    }
}

void Application::SelectNodeByIndex(int index) noexcept {
    if (!activeScene_ || index < 0 || static_cast<size_t>(index) >= activeScene_->allNodes.size()) {
        ClearSelection();
        return;
    }
    SelectNode(activeScene_->allNodes[index], index);
}

void Application::SelectNode(std::shared_ptr<scene::SceneNode> node, int index) noexcept {
    selectedNode_ = std::move(node);
    selectedNodeIndex_ = index;
    if (index < 0 && node && activeScene_) {
        for (size_t i = 0; i < activeScene_->allNodes.size(); ++i) {
            if (activeScene_->allNodes[i] == node) {
                selectedNodeIndex_ = static_cast<int>(i);
                break;
            }
        }
    }
    if (selectedNode_) {
        if (selectedNode_->worldAABB.minBound.x <= selectedNode_->worldAABB.maxBound.x) {
            const float distToNode = glm::length(camera_.GetPosition() - selectedNode_->worldAABB.Center());
            pipelineManager_.GetPostProcessPass().SetDoFFocusDistance(distToNode);
        }
        BGL_LOG_INFO("Selected Node #{} '{}' (AABB min=[{:.2f}, {:.2f}, {:.2f}], max=[{:.2f}, {:.2f}, {:.2f}])",
                     selectedNodeIndex_, selectedNode_->name, selectedNode_->worldAABB.minBound.x,
                     selectedNode_->worldAABB.minBound.y, selectedNode_->worldAABB.minBound.z,
                     selectedNode_->worldAABB.maxBound.x, selectedNode_->worldAABB.maxBound.y,
                     selectedNode_->worldAABB.maxBound.z);
    }
}

int Application::PickNodeAt(float screenX, float screenY, float screenWidth, float screenHeight) {
    if (!activeScene_ || screenWidth <= 0.0f || screenHeight <= 0.0f) {
        return -1;
    }
    const core::Ray pickRay = camera_.ScreenPointToRay(screenX, screenY, screenWidth, screenHeight);
    if (auto hit = scene::RayPicker::Pick(pickRay, activeScene_->rootNodes)) {
        if (hit->hitNode) {
            SelectNode(hit->hitNode, hit->hitNode->nodeIndex);
            if (std::filesystem::exists("assets/audio/click.wav")) {
                if (auto audioRes = soundEngine_.PlaySound3D("pick", "assets/audio/click.wav", hit->hitPoint);
                    !audioRes) {
                    BGL_LOG_DEBUG("Audio pick cue skipped: {}", audioRes.error());
                }
            }
            BGL_LOG_INFO("[RayPicker] Hit Node '{}' (index #{}, distance {:.3f}m)", hit->hitNode->name,
                         hit->hitNode->nodeIndex, hit->distance);
            return hit->hitNode->nodeIndex;
        }
    }
    ClearSelection();
    return -1;
}

void Application::SelectCamera(int cameraIndex) noexcept {
    if (!activeScene_ || cameraIndex < 0 || static_cast<size_t>(cameraIndex) >= activeScene_->cameras.size()) {
        activeCameraIndex_ = -1;
        BGL_LOG_INFO("Switched to default interactive orbit/fly camera");
        return;
    }
    activeCameraIndex_ = cameraIndex;
    const auto& sc = activeScene_->cameras[cameraIndex];
    camera_.SetFromTransform(sc.worldTransform, sc.yfov, sc.znear, sc.zfar);
    BGL_LOG_INFO("Activated glTF camera #{}: '{}' (fovY={:.1f}deg, near={:.2f}, far={:.2f})", cameraIndex, sc.name,
                 glm::degrees(sc.yfov), sc.znear, sc.zfar);
}

void Application::CycleCamera() noexcept {
    if (!activeScene_ || activeScene_->cameras.empty()) {
        return;
    }
    int next = activeCameraIndex_ + 1;
    if (static_cast<size_t>(next) >= activeScene_->cameras.size()) {
        next = -1;
    }
    SelectCamera(next);
}

int Application::GetCameraCount() const noexcept {
    return activeScene_ ? static_cast<int>(activeScene_->cameras.size()) : 0;
}

std::string_view Application::GetCameraName(int index) const noexcept {
    if (!activeScene_ || index < 0 || static_cast<size_t>(index) >= activeScene_->cameras.size()) {
        return "";
    }
    return activeScene_->cameras[index].name;
}

} // namespace bgl::app
