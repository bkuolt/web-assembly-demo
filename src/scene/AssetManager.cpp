/**
 * @file AssetManager.cpp
 * @brief Implementation of AssetManager caching and hash deduplication.
 */
#include "scene/AssetManager.hpp"
#include "core/Log.hpp"
#include "core/NetworkFetcher.hpp"

namespace bgl::scene {

AssetManager::AssetManager() : scratchArena_(1024 * 1024 * 8) {}

core::Result<std::shared_ptr<DecodedImage>> AssetManager::LoadTexture(std::string_view path) {
    uint64_t pathHash = Fnv1a64(path);
    {
        std::shared_lock lock(mutex_);
        auto it = textureCache_.find(pathHash);
        if (it != textureCache_.end()) {
            BGL_LOG_DEBUG("AssetManager: Texture cache hit for '{}' (hash: {:x})", path, pathHash);
            return it->second;
        }
    }

    auto dataResult = core::NetworkFetcher::FetchSync(path);
    if (!dataResult) {
        return std::unexpected("AssetManager failed to load: " + dataResult.error());
    }

    // True binary content hashing over loaded payload
    const uint64_t contentHash = Fnv1a64(*dataResult);
    {
        std::shared_lock lock(mutex_);
        auto it = textureCache_.find(contentHash);
        if (it != textureCache_.end()) {
            BGL_LOG_DEBUG("AssetManager: Content hash deduplication hit for '{}' (hash: {:x})", path, contentHash);
            std::unique_lock writeLock(mutex_);
            textureCache_[pathHash] = it->second;
            return it->second;
        }
    }

    auto decodedResult = TextureDecoder::DecodeAuto(*dataResult, path);
    if (!decodedResult) {
        return std::unexpected("AssetManager failed to decode: " + decodedResult.error());
    }

    auto sharedImg = std::make_shared<DecodedImage>(std::move(*decodedResult));
    {
        std::unique_lock lock(mutex_);
        textureCache_[pathHash] = sharedImg;
        textureCache_[contentHash] = sharedImg;
    }
    BGL_LOG_INFO("AssetManager: Cached new texture '{}' ({}x{}) with content hash {:x}", path, sharedImg->width,
                 sharedImg->height, contentHash);
    return sharedImg;
}

void AssetManager::InsertTexture(uint64_t hash, std::shared_ptr<DecodedImage> texture) {
    std::unique_lock lock(mutex_);
    textureCache_[hash] = std::move(texture);
}

bool AssetManager::HasTexture(uint64_t hash) const noexcept {
    std::shared_lock lock(mutex_);
    return textureCache_.contains(hash);
}

std::shared_ptr<DecodedImage> AssetManager::GetCachedTexture(uint64_t hash) const noexcept {
    std::shared_lock lock(mutex_);
    auto it = textureCache_.find(hash);
    if (it != textureCache_.end()) {
        return it->second;
    }
    return nullptr;
}

void AssetManager::Clear() {
    std::unique_lock lock(mutex_);
    textureCache_.clear();
    scratchArena_.Reset();
}

} // namespace bgl::scene
