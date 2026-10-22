/**
 * @file AssetManager.cpp
 * @brief Implementation of AssetManager caching and hash deduplication.
 */
#include "scene/AssetManager.hpp"
#include "core/NetworkFetcher.hpp"
#include "core/Log.hpp"

namespace bgl::scene {

AssetManager::AssetManager() : scratchArena_(1024 * 1024 * 8) {}

core::Result<std::shared_ptr<DecodedImage>> AssetManager::LoadTexture(std::string_view path) {
    uint64_t hash = Fnv1a64(path);
    auto it = textureCache_.find(hash);
    if (it != textureCache_.end()) {
        BGL_LOG_DEBUG("AssetManager: Texture cache hit for '{}' (hash: {:x})", path, hash);
        return it->second;
    }

    auto dataResult = core::NetworkFetcher::FetchSync(path);
    if (!dataResult) {
        return std::unexpected("AssetManager failed to load: " + dataResult.error());
    }

    auto decodedResult = TextureDecoder::DecodeAuto(*dataResult, path);
    if (!decodedResult) {
        return std::unexpected("AssetManager failed to decode: " + decodedResult.error());
    }

    auto sharedImg = std::make_shared<DecodedImage>(std::move(*decodedResult));
    textureCache_[hash] = sharedImg;
    BGL_LOG_INFO("AssetManager: Cached new texture '{}' ({}x{}) with hash {:x}", path, sharedImg->width, sharedImg->height, hash);
    return sharedImg;
}

void AssetManager::InsertTexture(uint64_t hash, std::shared_ptr<DecodedImage> texture) {
    textureCache_[hash] = std::move(texture);
}

bool AssetManager::HasTexture(uint64_t hash) const noexcept {
    return textureCache_.contains(hash);
}

void AssetManager::Clear() {
    textureCache_.clear();
    scratchArena_.Reset();
}

} // namespace bgl::scene

