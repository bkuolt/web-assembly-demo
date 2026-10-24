/**
 * @file NetworkFetcher.cpp
 * @brief Implementation of HTTP/HTTPS (libcurl / Emscripten Fetch), data:, file://, and local file fetching.
 */
#include "core/NetworkFetcher.hpp"
#include "core/Log.hpp"
#include "core/Version.hpp"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <ranges>
#include <sstream>

#if defined(BGL_PLATFORM_WASM) && defined(__EMSCRIPTEN__)
#include <emscripten/fetch.h>
#elif defined(BGL_HAS_CURL)
#include <curl/curl.h>
#endif

namespace bgl::core {

namespace {

constexpr size_t kMaxAssetFileSize = 1024ULL * 1024ULL * 1024ULL; // 1 GiB safety limit against unbounded allocations

[[nodiscard]] bool StartsWithCaseInsensitive(std::string_view str, std::string_view prefix) noexcept {
    if (str.size() < prefix.size()) return false;
    return std::ranges::equal(
        str.substr(0, prefix.size()),
        prefix,
        [](unsigned char a, unsigned char b) noexcept {
            return std::tolower(a) == std::tolower(b);
        }
    );
}

[[nodiscard]] int HexVal(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

[[nodiscard]] Result<std::vector<uint8_t>> DecodeBase64(std::string_view encoded) {
    static constexpr std::array<int8_t, 256> kDecTable = []() constexpr {
        std::array<int8_t, 256> tab{};
        tab.fill(-1);
        for (int i{0}; i < 26; ++i) {
            tab[static_cast<size_t>('A' + i)] = static_cast<int8_t>(i);
            tab[static_cast<size_t>('a' + i)] = static_cast<int8_t>(i + 26);
        }
        for (int i{0}; i < 10; ++i) {
            tab[static_cast<size_t>('0' + i)] = static_cast<int8_t>(i + 52);
        }
        tab[static_cast<size_t>('+')] = 62;
        tab[static_cast<size_t>('/')] = 63;
        tab[static_cast<size_t>('-')] = 62;
        tab[static_cast<size_t>('_')] = 63;
        return tab;
    }();

    std::vector<uint8_t> out{};
    out.reserve((encoded.size() * 3) / 4);

    uint32_t val{0};
    int valb{-8};
    for (unsigned char c : encoded) {
        if (std::isspace(c)) continue;
        if (c == '=') break;
        const int8_t decoded{kDecTable[c]};
        if (decoded == -1) {
            return std::unexpected("Invalid character encountered in base64 data URI");
        }
        val = (val << 6) | static_cast<uint32_t>(decoded);
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFFu));
            valb -= 8;
        }
    }
    return out;
}

[[nodiscard]] std::string NormalizeUrlPath(std::string_view path) {
    std::vector<std::string_view> segments{};
    const bool leadingSlash{!path.empty() && path.front() == '/'};
    const bool trailingSlash{path.size() > 1 && path.back() == '/'};

    size_t start{0};
    while (start <= path.size()) {
        const size_t pos{path.find('/', start)};
        const size_t len{(pos == std::string_view::npos) ? (path.size() - start) : (pos - start)};
        const std::string_view seg{path.substr(start, len)};

        if (seg == "..") {
            if (!segments.empty() && segments.back() != "..") {
                segments.pop_back();
            }
        } else if (!seg.empty() && seg != ".") {
            segments.push_back(seg);
        }

        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }

    std::string result{};
    if (leadingSlash) result.push_back('/');
    for (size_t i{0}; i < segments.size(); ++i) {
        if (i > 0) result.push_back('/');
        result.append(segments[i]);
    }
    if (trailingSlash && (result.empty() || result.back() != '/')) {
        result.push_back('/');
    }
    if (result.empty() && leadingSlash) {
        return "/";
    }
    return result;
}

#if defined(BGL_HAS_CURL)
void EnsureCurlGlobalInitialized() {
    static std::once_flag initFlag{};
    std::call_once(initFlag, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    const size_t totalBytes{size * nmemb};
    auto* buffer{static_cast<std::vector<uint8_t>*>(userp)};
    if (buffer->size() + totalBytes > kMaxAssetFileSize) {
        return 0; // Exceeded limit; abort download safely with CURLE_WRITE_ERROR
    }
    const auto* bytePtr{static_cast<const uint8_t*>(contents)};
    buffer->insert(buffer->end(), bytePtr, bytePtr + totalBytes);
    return totalBytes;
}
#endif

[[nodiscard]] Result<std::vector<uint8_t>> ReadLocalFileBytes(const std::filesystem::path& filePath) {
    std::error_code ec{};
    if (!std::filesystem::exists(filePath, ec) || std::filesystem::is_directory(filePath, ec)) {
        return std::unexpected("File does not exist: " + filePath.string());
    }

    std::ifstream file{filePath, std::ios::binary | std::ios::ate};
    if (!file.is_open()) {
        return std::unexpected("Failed to open local file: " + filePath.string());
    }

    const std::streamsize size{file.tellg()};
    if (size < 0) {
        return std::unexpected("Failed to determine size of local file: " + filePath.string());
    }
    if (static_cast<uint64_t>(size) > kMaxAssetFileSize) {
        return std::unexpected("Local file exceeds maximum allowed asset size (1 GiB): " + filePath.string());
    }
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    if (size > 0 && !file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return std::unexpected("Failed to read local file: " + filePath.string());
    }
    return buffer;
}

} // namespace

bool NetworkFetcher::IsHttpUrl(std::string_view uri) noexcept {
    const std::string cleaned{SanitizeUri(uri)};
    return StartsWithCaseInsensitive(cleaned, "http://") ||
           StartsWithCaseInsensitive(cleaned, "https://");
}

bool NetworkFetcher::IsFileUrl(std::string_view uri) noexcept {
    const std::string cleaned{SanitizeUri(uri)};
    return StartsWithCaseInsensitive(cleaned, "file://");
}

bool NetworkFetcher::IsDataUri(std::string_view uri) noexcept {
    const std::string cleaned{SanitizeUri(uri)};
    return StartsWithCaseInsensitive(cleaned, "data:");
}

bool NetworkFetcher::IsUrl(std::string_view uri) noexcept {
    return IsHttpUrl(uri) || IsFileUrl(uri) || IsDataUri(uri);
}

bool NetworkFetcher::IsCurlSupported() noexcept {
#if defined(BGL_HAS_CURL) || (defined(BGL_PLATFORM_WASM) && defined(__EMSCRIPTEN__))
    return true;
#else
    return false;
#endif
}

std::string NetworkFetcher::SanitizeUri(std::string_view rawUri) {
    auto trim = [](std::string_view s) noexcept -> std::string_view {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
            s.remove_prefix(1);
        }
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
            s.remove_suffix(1);
        }
        return s;
    };

    std::string_view trimmed{trim(rawUri)};
    if (trimmed.empty()) return {};

    // Handle RFC 2483 text/uri-list payloads (may contain # comment lines and multiple CRLF-separated lines)
    if (trimmed.find('\n') != std::string_view::npos || trimmed.find('\r') != std::string_view::npos) {
        size_t pos{0};
        while (pos < trimmed.size()) {
            const size_t nextNl{trimmed.find_first_of("\r\n", pos)};
            const std::string_view line{trim(trimmed.substr(pos, (nextNl == std::string_view::npos) ? (trimmed.size() - pos) : (nextNl - pos)))};
            if (!line.empty() && line.front() != '#') {
                trimmed = line;
                break;
            }
            if (nextNl == std::string_view::npos) break;
            pos = nextNl + 1;
        }
    }

    // Strip surrounding quotes or angle brackets
    if (trimmed.size() >= 2) {
        if ((trimmed.front() == '"' && trimmed.back() == '"') ||
            (trimmed.front() == '\'' && trimmed.back() == '\'') ||
            (trimmed.front() == '<' && trimmed.back() == '>')) {
            trimmed = trim(trimmed.substr(1, trimmed.size() - 2));
        }
    }

    return std::string{trimmed};
}

std::string NetworkFetcher::DecodeUrlPercent(std::string_view input) {
    std::string out{};
    out.reserve(input.size());
    for (size_t i{0}; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()) {
            const int hi{HexVal(input[i + 1])};
            const int lo{HexVal(input[i + 2])};
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(input[i]);
    }
    return out;
}

std::string NetworkFetcher::ResolveRelativeUri(std::string_view baseUri, std::string_view relativeUri) {
    const std::string cleanRel{SanitizeUri(relativeUri)};
    if (cleanRel.empty()) return SanitizeUri(baseUri);

    // If relativeUri is already an absolute URL (http://, https://, file://, data:), return it directly
    if (IsUrl(cleanRel)) {
        return cleanRel;
    }

    const std::string cleanBase{SanitizeUri(baseUri)};
    if (cleanBase.empty() || IsDataUri(cleanBase)) {
        return cleanRel;
    }

    if (IsHttpUrl(cleanBase) || IsFileUrl(cleanBase)) {
        // Strip query and fragment from base URL before resolving relative path
        const size_t qPos{cleanBase.find_first_of("?#")};
        const std::string_view baseNoQuery{
            (qPos == std::string::npos) ? std::string_view{cleanBase} : std::string_view{cleanBase}.substr(0, qPos)
        };

        const size_t schemePos{baseNoQuery.find("://")};
        if (schemePos == std::string_view::npos) {
            return cleanRel;
        }
        const size_t authorityStart{schemePos + 3};
        const size_t pathStart{baseNoQuery.find('/', authorityStart)};

        const std::string_view origin{
            (pathStart == std::string_view::npos) ? baseNoQuery : baseNoQuery.substr(0, pathStart)
        };

        // Split relative URI into path and optional query/fragment
        const size_t relQueryPos{cleanRel.find_first_of("?#")};
        const std::string_view relPath{
            (relQueryPos == std::string::npos) ? std::string_view{cleanRel} : std::string_view{cleanRel}.substr(0, relQueryPos)
        };
        const std::string_view relSuffix{
            (relQueryPos == std::string::npos) ? std::string_view{} : std::string_view{cleanRel}.substr(relQueryPos)
        };

        if (!relPath.empty() && relPath.front() == '/') {
            return std::string{origin} + NormalizeUrlPath(relPath) + std::string{relSuffix};
        }

        std::string_view baseDirPath{"/"};
        if (pathStart != std::string_view::npos) {
            const size_t lastSlash{baseNoQuery.rfind('/')};
            if (lastSlash != std::string_view::npos && lastSlash >= pathStart) {
                baseDirPath = baseNoQuery.substr(pathStart, lastSlash - pathStart + 1);
            }
        }

        const std::string combinedPath{std::string{baseDirPath} + std::string{relPath}};
        return std::string{origin} + NormalizeUrlPath(combinedPath) + std::string{relSuffix};
    }

    // Local filesystem path resolution
    const std::filesystem::path relPath{cleanRel};
    if (relPath.is_absolute()) {
        return relPath.lexically_normal().string();
    }

    const std::filesystem::path basePath{cleanBase};
    const std::filesystem::path parentDir{basePath.parent_path()};
    if (parentDir.empty()) {
        return relPath.lexically_normal().string();
    }
    return (parentDir / relPath).lexically_normal().string();
}

std::string NetworkFetcher::ExtractDisplayName(std::string_view uri) {
    const std::string cleaned{SanitizeUri(uri)};
    if (cleaned.empty()) return "None";
    if (IsDataUri(cleaned)) return "EmbeddedData";

    const size_t qPos{cleaned.find_first_of("?#")};
    std::string_view withoutQuery{
        (qPos == std::string::npos) ? std::string_view{cleaned} : std::string_view{cleaned}.substr(0, qPos)
    };

    while (withoutQuery.size() > 1 && (withoutQuery.back() == '/' || withoutQuery.back() == '\\')) {
        withoutQuery.remove_suffix(1);
    }

    const size_t lastSlash{withoutQuery.find_last_of("/\\")};
    const std::string_view filename{
        (lastSlash == std::string_view::npos) ? withoutQuery : withoutQuery.substr(lastSlash + 1)
    };

    const std::string decoded{DecodeUrlPercent(filename)};
    return decoded.empty() ? cleaned : decoded;
}

Result<std::vector<uint8_t>> NetworkFetcher::FetchSync(std::string_view url) {
    const std::string cleanedUri{SanitizeUri(url)};
    if (cleanedUri.empty()) {
        return std::unexpected("Cannot fetch empty URI");
    }

    // 1. RFC 2397 inline data:[<mediatype>][;base64],<data>
    if (IsDataUri(cleanedUri)) {
        const size_t commaPos{cleanedUri.find(',')};
        if (commaPos == std::string::npos) {
            return std::unexpected("Malformed data: URI (missing comma separator)");
        }
        const std::string_view header{std::string_view{cleanedUri}.substr(5, commaPos - 5)};
        const std::string_view payload{std::string_view{cleanedUri}.substr(commaPos + 1)};

        const bool isBase64{header.find(";base64") != std::string_view::npos ||
                            header.find(";BASE64") != std::string_view::npos};
        if (isBase64) {
            return DecodeBase64(payload);
        }
        const std::string decoded{DecodeUrlPercent(payload)};
        return std::vector<uint8_t>{decoded.begin(), decoded.end()};
    }

    // 2. Local file:// URI
    if (IsFileUrl(cleanedUri)) {
        std::string_view pathPart{std::string_view{cleanedUri}.substr(7)};
        if (StartsWithCaseInsensitive(pathPart, "localhost/")) {
            pathPart.remove_prefix(9);
        }
        const std::string decodedPath{DecodeUrlPercent(pathPart)};
        return ReadLocalFileBytes(std::filesystem::path{decodedPath});
    }

    // 3. Remote HTTP / HTTPS URL
    if (IsHttpUrl(cleanedUri)) {
#if defined(BGL_PLATFORM_WASM) && defined(__EMSCRIPTEN__)
        emscripten_fetch_attr_t attr{};
        emscripten_fetch_attr_init(&attr);
        std::strcpy(attr.requestMethod, "GET");
        attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS;

        emscripten_fetch_t* fetch{emscripten_fetch(&attr, cleanedUri.c_str())};
        if (!fetch) {
            return std::unexpected("Emscripten fetch failed to initialize for URL: " + cleanedUri);
        }
        if (fetch->status >= 200 && fetch->status < 300 && fetch->data && fetch->numBytes > 0) {
            const auto* bytePtr{reinterpret_cast<const uint8_t*>(fetch->data)};
            std::vector<uint8_t> buffer{bytePtr, bytePtr + fetch->numBytes};
            emscripten_fetch_close(fetch);
            return buffer;
        }
        const uint16_t status{fetch->status};
        emscripten_fetch_close(fetch);
        return std::unexpected("HTTP request failed with status " + std::to_string(status) + " for URL: " + cleanedUri);

#elif defined(BGL_HAS_CURL)
        EnsureCurlGlobalInitialized();

        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl{curl_easy_init(), &curl_easy_cleanup};
        if (!curl) {
            return std::unexpected("Failed to initialize libcurl easy handle");
        }

        std::vector<uint8_t> responseBuffer{};
        responseBuffer.reserve(64 * 1024);

        std::array<char, CURL_ERROR_SIZE> errBuf{};
        errBuf.fill('\0');

        curl_easy_setopt(curl.get(), CURLOPT_URL, cleanedUri.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, CurlWriteCallback);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &responseBuffer);
        curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, errBuf.data());
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 10L);
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
        const std::string userAgent = std::string("bgl-engine/") + std::string(kEngineVersion) + " (libcurl)";
        curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, userAgent.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 120L);

        BGL_LOG_INFO("[NetworkFetcher] Downloading via libcurl: '{}'", cleanedUri);
        const CURLcode res{curl_easy_perform(curl.get())};
        if (res != CURLE_OK) {
            const std::string detail{errBuf[0] != '\0' ? errBuf.data() : curl_easy_strerror(res)};
            return std::unexpected("libcurl download failed for '" + cleanedUri + "': " + detail);
        }

        long httpCode{0};
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &httpCode);

        if (httpCode >= 400 || (httpCode != 0 && (httpCode < 200 || httpCode >= 300))) {
            return std::unexpected("HTTP error " + std::to_string(httpCode) + " when fetching URL: " + cleanedUri);
        }

        BGL_LOG_INFO("[NetworkFetcher] Downloaded {} bytes ({:.2f} MB) from '{}' (HTTP {})",
                     responseBuffer.size(), responseBuffer.size() / (1024.0 * 1024.0), cleanedUri, httpCode);
        return responseBuffer;
#else
        return std::unexpected("HTTP/HTTPS URL fetching is not enabled in this build: " + cleanedUri);
#endif
    }

    // 4. Local filesystem path (with automatic percent-decoding fallback)
    auto localRes = ReadLocalFileBytes(std::filesystem::path{cleanedUri});
    if (localRes) {
        return localRes;
    }
    if (cleanedUri.find('%') != std::string::npos) {
        const std::string decodedPath{DecodeUrlPercent(cleanedUri)};
        auto decodedRes = ReadLocalFileBytes(std::filesystem::path{decodedPath});
        if (decodedRes) {
            return decodedRes;
        }
    }
    return localRes;
}

void NetworkFetcher::FetchAsync(std::string_view url, FetchCallback callback) {
    auto res = FetchSync(url);
    if (callback) {
        callback(std::move(res));
    }
}

} // namespace bgl::core
