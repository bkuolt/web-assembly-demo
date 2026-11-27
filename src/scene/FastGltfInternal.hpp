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
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace bgl::scene::detail {

[[nodiscard]] inline std::span<const uint8_t> ExtractContiguousSourceBytes(const fastgltf::DataSource& data) noexcept {
    if (std::holds_alternative<fastgltf::sources::Array>(data)) {
        const auto& arr = std::get<fastgltf::sources::Array>(data);
        return {reinterpret_cast<const uint8_t*>(arr.bytes.data()), arr.bytes.size()};
    }
    if (std::holds_alternative<fastgltf::sources::ByteView>(data)) {
        const auto& bv = std::get<fastgltf::sources::ByteView>(data);
        return {reinterpret_cast<const uint8_t*>(bv.bytes.data()), bv.bytes.size()};
    }
    if (std::holds_alternative<fastgltf::sources::Vector>(data)) {
        const auto& vec = std::get<fastgltf::sources::Vector>(data);
        return {reinterpret_cast<const uint8_t*>(vec.bytes.data()), vec.bytes.size()};
    }
    return {};
}

[[nodiscard]] inline constexpr size_t AccessorElementByteSize(fastgltf::AccessorType type,
                                                              fastgltf::ComponentType comp) noexcept {
    size_t compBytes{0};
    switch (comp) {
    case fastgltf::ComponentType::Byte:
    case fastgltf::ComponentType::UnsignedByte:
        compBytes = 1u;
        break;
    case fastgltf::ComponentType::Short:
    case fastgltf::ComponentType::UnsignedShort:
        compBytes = 2u;
        break;
    case fastgltf::ComponentType::Int:
    case fastgltf::ComponentType::UnsignedInt:
    case fastgltf::ComponentType::Float:
        compBytes = 4u;
        break;
    case fastgltf::ComponentType::Double:
        compBytes = 8u;
        break;
    default:
        return 0u;
    }

    size_t compCount{0};
    switch (type) {
    case fastgltf::AccessorType::Scalar:
        compCount = 1u;
        break;
    case fastgltf::AccessorType::Vec2:
        compCount = 2u;
        break;
    case fastgltf::AccessorType::Vec3:
        compCount = 3u;
        break;
    case fastgltf::AccessorType::Vec4:
    case fastgltf::AccessorType::Mat2:
        compCount = 4u;
        break;
    case fastgltf::AccessorType::Mat3:
        compCount = 9u;
        break;
    case fastgltf::AccessorType::Mat4:
        compCount = 16u;
        break;
    default:
        return 0u;
    }
    return compBytes * compCount;
}

[[nodiscard]] inline bool ValidateBufferViewSlice(const fastgltf::Asset& asset, size_t bvIdx, size_t accByteOffset,
                                                  size_t elemBytes, size_t count, bool respectStride) noexcept {
    if (bvIdx >= asset.bufferViews.size() || elemBytes == 0 || count == 0) {
        return false;
    }
    const auto& bv = asset.bufferViews[bvIdx];
    if (bv.bufferIndex >= asset.buffers.size()) {
        return false;
    }
    const auto& buf = asset.buffers[bv.bufferIndex];
    if (bv.byteOffset > buf.byteLength || bv.byteLength > (buf.byteLength - bv.byteOffset)) {
        return false;
    }
    const auto backingBytes = ExtractContiguousSourceBytes(buf.data);
    if (backingBytes.empty() || bv.byteOffset > backingBytes.size() ||
        bv.byteLength > (backingBytes.size() - bv.byteOffset)) {
        return false;
    }
    if (accByteOffset > bv.byteLength) {
        return false;
    }
    const size_t availBytes{bv.byteLength - accByteOffset};
    if (elemBytes > availBytes) {
        return false;
    }
    size_t stride{elemBytes};
    if (respectStride && bv.byteStride.has_value() && *bv.byteStride != 0u) {
        if (*bv.byteStride < elemBytes) {
            return false;
        }
        stride = *bv.byteStride;
    }
    return (count - 1u) <= ((availBytes - elemBytes) / stride);
}

[[nodiscard]] inline bool ValidateAccessor(const fastgltf::Asset& asset, size_t accIdx, size_t minElemBytes,
                                           size_t maxAllowedCount,
                                           std::optional<fastgltf::AccessorType> requiredType = std::nullopt) noexcept {
    if (accIdx >= asset.accessors.size()) {
        return false;
    }
    const auto& acc = asset.accessors[accIdx];
    if (acc.count == 0 || acc.count > maxAllowedCount) {
        return false;
    }
    if (requiredType.has_value() && acc.type != *requiredType) {
        return false;
    }
    const size_t elemBytes{AccessorElementByteSize(acc.type, acc.componentType)};
    if (elemBytes == 0 || elemBytes < minElemBytes) {
        return false;
    }
    if (acc.sparse.has_value()) {
        const auto& sp = *acc.sparse;
        if (sp.count == 0 || sp.count > acc.count) {
            return false;
        }
        size_t spIdxBytes{0u};
        if (sp.indexComponentType == fastgltf::ComponentType::UnsignedByte) {
            spIdxBytes = 1u;
        } else if (sp.indexComponentType == fastgltf::ComponentType::UnsignedShort) {
            spIdxBytes = 2u;
        } else if (sp.indexComponentType == fastgltf::ComponentType::UnsignedInt) {
            spIdxBytes = 4u;
        } else {
            return false;
        }
        if (!ValidateBufferViewSlice(asset, sp.indicesBufferView, sp.indicesByteOffset, spIdxBytes, sp.count, false) ||
            !ValidateBufferViewSlice(asset, sp.valuesBufferView, sp.valuesByteOffset, elemBytes, sp.count, false)) {
            return false;
        }
        if (acc.bufferViewIndex.has_value()) {
            return ValidateBufferViewSlice(asset, *acc.bufferViewIndex, acc.byteOffset, elemBytes, acc.count, true);
        }
        return true;
    }
    if (!acc.bufferViewIndex.has_value()) {
        return false;
    }
    return ValidateBufferViewSlice(asset, *acc.bufferViewIndex, acc.byteOffset, elemBytes, acc.count, true);
}

struct ProgressReporter {
    LoadProgressCallback callback{};
    float base{0.0f};
    float span{1.0f};
    std::mutex mutex{};
    float maxReportedFrac{0.0f};
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
        if (IsCancelled()) {
            return false;
        }
        maxReportedFrac = std::max(maxReportedFrac, globalFrac);
        if (!callback(LoadProgressInfo{.fraction = maxReportedFrac, .status = std::move(status)})) {
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
