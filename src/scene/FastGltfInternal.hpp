/**
 * @file FastGltfInternal.hpp
 * @brief Internal stage declarations and progress reporter shared by FastGltfLoader translation units.
 */
#ifndef BGL_SCENE_FASTGLTFINTERNAL_HPP
#define BGL_SCENE_FASTGLTFINTERNAL_HPP

#include "scene/FastGltfLoader.hpp"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>

namespace bgl::scene::detail {

struct ProgressReporter {
    LoadProgressCallback callback{};
    float base{0.0f};
    float span{1.0f};
    std::mutex mutex{};
    std::atomic<bool> cancelled{false};

    [[nodiscard]] bool IsCancelled() const noexcept { return cancelled.load(std::memory_order_acquire); }

    bool ReportLocal(float localFraction, std::string status) {
        if (IsCancelled()) {
            return false;
        }
        if (!callback) {
            return true;
        }
        const float globalFrac = std::clamp(base + std::clamp(localFraction, 0.0f, 1.0f) * span, 0.0f, 0.96f);
        std::scoped_lock lock(mutex);
        if (!callback(LoadProgressInfo{.fraction = globalFrac, .status = std::move(status)})) {
            cancelled.store(true, std::memory_order_release);
            return false;
        }
        return true;
    }
};

void DecodeImagesAndTextures(const fastgltf::Asset& asset, std::string_view baseUri, AssetManager& assetManager,
                             Scene& scene, ProgressReporter* reporter = nullptr);
void ParseMaterialsAndLogStructure(const fastgltf::Asset& asset, Scene& scene);
void ParsePunctualLights(const fastgltf::Asset& asset, Scene& scene);
void ParseMeshesAndGenerateLods(const fastgltf::Asset& asset, Scene& scene, ProgressReporter* reporter = nullptr);
void BuildSceneHierarchy(const fastgltf::Asset& asset, Scene& scene);
void ParseCamerasSkinsAndAnimations(const fastgltf::Asset& asset, Scene& scene);

} // namespace bgl::scene::detail

#endif // BGL_SCENE_FASTGLTFINTERNAL_HPP
