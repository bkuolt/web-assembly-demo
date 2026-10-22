/**
 * @file NetworkFetcher.hpp
 * @brief Asynchronous network asset streaming for Wasm (Emscripten Fetch) and Native.
 */
#pragma once

#include <string>
#include <vector>
#include <functional>
#include <span>
#include "core/Types.hpp"

namespace bgl::core {

using FetchCallback = std::function<void(Result<std::vector<uint8_t>>)>;

/**
 * @class NetworkFetcher
 * @brief HTTP/HTTPS fetcher wrapper for downloading glb/gltf and textures.
 */
class NetworkFetcher {
public:
    static void FetchAsync(std::string_view url, FetchCallback callback);
    static Result<std::vector<uint8_t>> FetchSync(std::string_view url);
};

} // namespace bgl::core

