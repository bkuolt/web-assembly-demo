/**
 * @file NetworkFetcher.hpp
 * @brief Unified HTTP/HTTPS (libcurl / Emscripten Fetch), file://, data:, and local filesystem asset fetcher.
 */
#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <span>
#include "core/Types.hpp"

namespace bgl::core {

using FetchCallback = std::function<void(Result<std::vector<uint8_t>>)>;

/**
 * @class NetworkFetcher
 * @brief Thread-safe URI resolver and synchronous/asynchronous asset downloader for glTF/GLB models,
 *        external binary buffers (.bin), and external textures (PNG, JPEG, WebP, KTX2).
 */
class NetworkFetcher {
public:
    /**
     * @brief Check if the URI uses HTTP or HTTPS protocol (`http://` or `https://`).
     */
    [[nodiscard]] static bool IsHttpUrl(std::string_view uri) noexcept;

    /**
     * @brief Check if the URI uses the file URI scheme (`file://`).
     */
    [[nodiscard]] static bool IsFileUrl(std::string_view uri) noexcept;

    /**
     * @brief Check if the URI is an RFC 2397 inline data URI (`data:`).
     */
    [[nodiscard]] static bool IsDataUri(std::string_view uri) noexcept;

    /**
     * @brief Check if the string is any supported URI scheme (`http://`, `https://`, `file://`, `data:`).
     */
    [[nodiscard]] static bool IsUrl(std::string_view uri) noexcept;

    /**
     * @brief Check whether native HTTP/HTTPS fetching via libcurl (or Emscripten Fetch) is compiled in.
     */
    [[nodiscard]] static bool IsCurlSupported() noexcept;

    /**
     * @brief Clean raw drag-and-drop or CLI URI strings (handles text/uri-list comments, CRLF, quotes).
     */
    [[nodiscard]] static std::string SanitizeUri(std::string_view rawUri);

    /**
     * @brief Decode percent-encoded URI characters (e.g. `%20` -> space).
     */
    [[nodiscard]] static std::string DecodeUrlPercent(std::string_view input);

    /**
     * @brief Resolve a relative URI (such as `scene.bin` or `../textures/albedo.png`) against a base model URI
     *        (which may be an `http://`/`https://` URL, `file://` URL, or local filesystem path).
     */
    [[nodiscard]] static std::string ResolveRelativeUri(std::string_view baseUri, std::string_view relativeUri);

    /**
     * @brief Extract a clean display filename from a URL or local file path (stripping query/fragment).
     */
    [[nodiscard]] static std::string ExtractDisplayName(std::string_view uri);

    /**
     * @brief Asynchronously fetch a local path or remote URL and invoke callback with the byte buffer.
     */
    static void FetchAsync(std::string_view url, FetchCallback callback);

    /**
     * @brief Synchronously fetch a local file, `file://` URI, `data:` URI, or `http://`/`https://` URL.
     *        Thread-safe for concurrent invocation from worker threads.
     */
    [[nodiscard]] static Result<std::vector<uint8_t>> FetchSync(std::string_view url);
};

} // namespace bgl::core
