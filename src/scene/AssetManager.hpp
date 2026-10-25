/**
 * @file AssetManager.hpp
 * @brief 64-bit FNV-1a content hashing, asset deduplication, and PMR staging management.
 */
#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <memory>
#include <span>
#include <cstdint>
#include <shared_mutex>
#include "core/Types.hpp"
#include "core/MemoryPool.hpp"
#include "scene/TextureDecoder.hpp"

namespace bgl::scene {

/**
 * @brief Compute 64-bit FNV-1a hash over binary data span.
 */
[[nodiscard]] constexpr uint64_t Fnv1a64(std::span<const uint8_t> data) noexcept {
    uint64_t hash{14695981039346656037ULL};
    for (uint8_t byte : data) {
        hash ^= static_cast<uint64_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

/**
 * @brief Compute 64-bit FNV-1a hash over string view.
 */
[[nodiscard]] constexpr uint64_t Fnv1a64(std::string_view str) noexcept {
    return Fnv1a64(std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(str.data()), str.size()});
}

/**
 * @class AssetManager
 * @brief High-performance asset cache with deduplication and scratch arena.
 */
class AssetManager {
public:
    AssetManager();
    ~AssetManager() = default;

    /**
     * @brief Load or retrieve a cached texture by URI / path.
     */
    core::Result<std::shared_ptr<DecodedImage>> LoadTexture(std::string_view path);

    /**
     * @brief Store a decoded texture indexed by its 64-bit FNV-1a content hash.
     */
    void InsertTexture(uint64_t hash, std::shared_ptr<DecodedImage> texture);

    /**
     * @brief Query whether a texture with the given hash is already resident in cache.
     */
    [[nodiscard]] bool HasTexture(uint64_t hash) const noexcept;

    /**
     * @brief Retrieve cached texture by hash, or nullptr if absent.
     */
    [[nodiscard]] std::shared_ptr<DecodedImage> GetCachedTexture(uint64_t hash) const noexcept;

    /**
     * @brief Access the scratch arena for per-frame staging allocations.
     */
    [[nodiscard]] core::ScratchArena& GetScratchArena() noexcept { return scratchArena_; }

    void Clear();

private:
    mutable std::shared_mutex mutex_{};
    core::ScratchArena scratchArena_;
    std::unordered_map<uint64_t, std::shared_ptr<DecodedImage>> textureCache_;
};

} // namespace bgl::scene

