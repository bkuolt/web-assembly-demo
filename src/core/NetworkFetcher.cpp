/**
 * @file NetworkFetcher.cpp
 * @brief Implementation of network fetcher with local fallback.
 */
#include "core/NetworkFetcher.hpp"
#include "core/Log.hpp"
#include <fstream>
#include <filesystem>

#if defined(BGL_PLATFORM_WASM) && defined(__EMSCRIPTEN__)
#include <emscripten/fetch.h>
#endif

namespace bgl::core {

Result<std::vector<uint8_t>> NetworkFetcher::FetchSync(std::string_view url) {
    std::filesystem::path p(url);
    if (std::filesystem::exists(p)) {
        std::ifstream file(p, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return std::unexpected("Failed to open local file: " + std::string(url));
        }
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> buffer(size);
        if (file.read(reinterpret_cast<char*>(buffer.data()), size)) {
            return buffer;
        }
        return std::unexpected("Failed to read file: " + std::string(url));
    }
    return std::unexpected("File does not exist: " + std::string(url));
}

void NetworkFetcher::FetchAsync(std::string_view url, FetchCallback callback) {
    auto res = FetchSync(url);
    callback(std::move(res));
}

} // namespace bgl::core

