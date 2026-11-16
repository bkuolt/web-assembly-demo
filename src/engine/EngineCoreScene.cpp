/**
 * @file EngineCoreScene.cpp
 * @brief Scene loading, node selection/picking, camera cycling, and diurnal sun controls for EngineCore.
 */
#include "core/Log.hpp"
#include "core/NetworkFetcher.hpp"
#include "engine/EngineCore.hpp"
#include "scene/RayPicker.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <numbers>

namespace bgl::engine {

void EngineCore::ActivateScene(std::shared_ptr<scene::Scene> scene, std::string_view sourceUri) {
    if (!scene) {
        return;
    }
    ClearSelection();
    activeCameraIndex_ = -1;
    activeScene_ = std::move(scene);
    currentModelName_ = core::NetworkFetcher::ExtractDisplayName(sourceUri);
    OnSceneActivated(sourceUri);
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

core::Result<void> EngineCore::LoadScene(std::span<const uint8_t> data, std::string_view baseUri) {
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

core::Result<void> EngineCore::LoadSceneFromUri(std::string_view uri) {
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

void EngineCore::OnFileDrop(std::string_view filename, std::span<const uint8_t> data) {
    BGL_LOG_INFO("Ingesting drag-and-dropped buffer '{}' ({} bytes)", filename, data.size());
    if (auto res = LoadScene(data, filename); res) {
        BGL_LOG_INFO("Successfully activated dropped glTF model '{}'", filename);
    } else {
        BGL_LOG_ERROR("Failed to load dropped glTF buffer '{}': {}", filename, res.error());
    }
}

void EngineCore::CacheSceneMetrics() {
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

void EngineCore::FocusSelection() noexcept {
    if (selectedNode_ && selectedNode_->worldAABB.minBound.x <= selectedNode_->worldAABB.maxBound.x) {
        camera_.FocusOnBounds(selectedNode_->worldAABB);
    } else if (activeScene_) {
        camera_.FocusOnBounds(activeScene_->sceneAABB);
    }
}

void EngineCore::FrameAll() noexcept {
    if (activeScene_) {
        camera_.FocusOnBounds(activeScene_->sceneAABB);
    }
}

void EngineCore::SelectNodeByIndex(int index) noexcept {
    if (!activeScene_ || index < 0 || static_cast<size_t>(index) >= activeScene_->allNodes.size()) {
        ClearSelection();
        return;
    }
    SelectNode(activeScene_->allNodes[index], index);
}

void EngineCore::SelectNode(std::shared_ptr<scene::SceneNode> node, int index) noexcept {
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

int EngineCore::PickNodeAt(float screenX, float screenY, float screenWidth, float screenHeight) {
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

void EngineCore::SelectCamera(int cameraIndex) noexcept {
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

void EngineCore::CycleCamera() noexcept {
    if (!activeScene_ || activeScene_->cameras.empty()) {
        return;
    }
    int next = activeCameraIndex_ + 1;
    if (static_cast<size_t>(next) >= activeScene_->cameras.size()) {
        next = -1;
    }
    SelectCamera(next);
}

int EngineCore::GetCameraCount() const noexcept {
    return activeScene_ ? static_cast<int>(activeScene_->cameras.size()) : 0;
}

std::string_view EngineCore::GetCameraName(int index) const noexcept {
    if (!activeScene_ || index < 0 || static_cast<size_t>(index) >= activeScene_->cameras.size()) {
        return "";
    }
    return activeScene_->cameras[index].name;
}

void EngineCore::SetTimeOfDay(float hour) noexcept {
    float wrapped = std::fmod(hour, 24.0f);
    if (wrapped < 0.0f) {
        wrapped += 24.0f;
    }
    timeOfDay_ = wrapped;
    pipelineManager_.SetTimeOfDay(timeOfDay_);

    // Diurnal cycle: sunrise ~6:00, noon 12:00 (peak elevation), sunset ~18:00, midnight 0:00 (moon peak)
    const float solarSin = -std::cos((timeOfDay_ / 24.0f) * 2.0f * std::numbers::pi_v<float>);
    const float activeElev = (solarSin >= 0.0f) ? std::asin(std::clamp(solarSin, 0.0f, 1.0f))
                                                : std::asin(std::clamp(-solarSin * 0.75f, 0.0f, 1.0f));
    const float azimuth = (timeOfDay_ / 24.0f) * 2.0f * std::numbers::pi_v<float>;
    SetSunAzimuthElevation(azimuth, std::max(activeElev, 0.06f));
}

void EngineCore::SetSunAzimuthElevation(float azimuthRad, float elevationRad) noexcept {
    sunAzimuth_ = azimuthRad;
    sunElevation_ = std::clamp(elevationRad, 0.02f, 1.57f);
    UpdateSunDirection();
}

void EngineCore::UpdateSunDirection() noexcept {
    const float cosEl = std::cos(sunElevation_);
    sunDir_ = glm::normalize(
        glm::vec3{cosEl * std::sin(sunAzimuth_), std::sin(sunElevation_), cosEl * std::cos(sunAzimuth_)});
}

void EngineCore::SetKtxCompressionTarget(scene::KtxTranscodeTarget target) {
    if (!activeScene_) {
        return;
    }
    scene::KtxTranscodeTarget resolved = target;
    const bool unsupported = (resolved == scene::KtxTranscodeTarget::Bc7 && !gpuContext_.SupportsBC()) ||
                             (resolved == scene::KtxTranscodeTarget::Astc4x4 && !gpuContext_.SupportsASTC()) ||
                             (resolved == scene::KtxTranscodeTarget::Etc2Rgba && !gpuContext_.SupportsETC2());
    if (unsupported) {
        resolved = scene::KtxTranscodeTarget::Rgba32;
    }
    pipelineManager_.RetranscodeKtxTextures(*activeScene_, resolved);
}

} // namespace bgl::engine
