/**
 * @file NetworkFetcher.cpp
 * @brief Implementation of HTTP/HTTPS (libcurl / Emscripten Fetch), data:, file://, and local file fetching.
 */
#include "core/NetworkFetcher.hpp"
#include "core/Log.hpp"
#include "core/ThreadPool.hpp"
#include "core/Version.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <ranges>
#include <sstream>

#if defined(BGL_PLATFORM_WASM) && defined(__EMSCRIPTEN__)
#include <emscripten/fetch.h>
#elif defined(BGL_HAS_CURL)
#include <curl/curl.h>
#if defined(__linux__) || defined(__unix__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#endif

namespace bgl::core {

namespace {

#if defined(BGL_PLATFORM_WASM)
constexpr size_t kMaxAssetFileSize = 48ULL * 1024ULL * 1024ULL; // 48 MiB WASM safety limit
#else
constexpr size_t kMaxAssetFileSize = 256ULL * 1024ULL * 1024ULL; // 256 MiB native safety limit
#endif

thread_local bool t_allowLoopbackHttpForTests{false};

[[nodiscard]] bool StartsWithCaseInsensitive(std::string_view str, std::string_view prefix) noexcept {
    if (str.size() < prefix.size())
        return false;
    return std::ranges::equal(str.substr(0, prefix.size()), prefix, [](unsigned char a, unsigned char b) noexcept {
        return std::tolower(a) == std::tolower(b);
    });
}

[[nodiscard]] int HexVal(char c) noexcept {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

[[nodiscard]] bool ContainsControlOrNul(std::string_view text) noexcept {
    for (unsigned char ch : text) {
        if (ch < 0x20u || ch == 0x7Fu) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] Result<std::vector<uint8_t>> DecodeBase64(std::string_view encoded) {
    static constexpr std::array<int8_t, 256> kDecTable = []() constexpr {
        std::array<int8_t, 256> tab{};
        tab.fill(-1);
        for (int i{0}; i < 26; ++i) {
            tab[static_cast<size_t>('A') + static_cast<size_t>(i)] = static_cast<int8_t>(i);
            tab[static_cast<size_t>('a') + static_cast<size_t>(i)] = static_cast<int8_t>(i + 26);
        }
        for (int i{0}; i < 10; ++i) {
            tab[static_cast<size_t>('0') + static_cast<size_t>(i)] = static_cast<int8_t>(i + 52);
        }
        tab[static_cast<size_t>('+')] = 62;
        tab[static_cast<size_t>('/')] = 63;
        tab[static_cast<size_t>('-')] = 62;
        tab[static_cast<size_t>('_')] = 63;
        return tab;
    }();

    constexpr size_t kMaxBase64EncodedBytes{4u * ((kMaxAssetFileSize / 3u) + 2u)};
    if (encoded.size() > kMaxBase64EncodedBytes) {
        return std::unexpected("Base64 data URI exceeds maximum allowed asset size");
    }

    std::vector<uint8_t> out{};
    out.reserve((encoded.size() / 4u + 1u) * 3u);

    uint32_t val{0};
    int valb{-8};
    for (unsigned char c : encoded) {
        if (std::isspace(c))
            continue;
        if (c == '=')
            break;
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

        if (pos == std::string_view::npos)
            break;
        start = pos + 1;
    }

    std::string result{};
    if (leadingSlash)
        result.push_back('/');
    for (size_t i{0}; i < segments.size(); ++i) {
        if (i > 0)
            result.push_back('/');
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
        std::atexit([]() { curl_global_cleanup(); });
    });
}

#if defined(__linux__) || defined(__unix__)
[[nodiscard]] bool IsBlockedIpv4HostOrder(uint32_t ip, bool allowLoopback) noexcept {
    if ((ip & 0xFF000000u) == 0x7F000000u) { // 127.0.0.0/8 (Loopback)
        return !allowLoopback;
    }
    return (ip & 0xFF000000u) == 0x00000000u || // 0.0.0.0/8 (this network)
           (ip & 0xFF000000u) == 0x0A000000u || // 10.0.0.0/8 (RFC1918)
           (ip & 0xFFC00000u) == 0x64400000u || // 100.64.0.0/10 (CGNAT)
           (ip & 0xFFFF0000u) == 0xA9FE0000u || // 169.254.0.0/16 (Link-local / IMDS)
           (ip & 0xFFF00000u) == 0xAC100000u || // 172.16.0.0/12 (RFC1918)
           (ip & 0xFFFF0000u) == 0xC0000000u || // 192.0.0.0/24 (IETF)
           (ip & 0xFFFF0000u) == 0xC0A80000u || // 192.168.0.0/16 (RFC1918)
           (ip & 0xFFFE0000u) == 0xC6120000u || // 198.18.0.0/15 (Benchmark)
           (ip >= 0xE0000000u);                 // 224.0.0.0/4 multicast + 240.0.0.0/4 reserved
}

[[nodiscard]] bool IsBlockedSocketAddress(const struct sockaddr* addr, bool allowLoopback) noexcept {
    if (!addr) {
        return true;
    }
    if (addr->sa_family == AF_INET) {
        const auto* sin{reinterpret_cast<const struct sockaddr_in*>(addr)};
        return IsBlockedIpv4HostOrder(ntohl(sin->sin_addr.s_addr), allowLoopback);
    }
    if (addr->sa_family == AF_INET6) {
        const auto* sin6{reinterpret_cast<const struct sockaddr_in6*>(addr)};
        const uint8_t* b{sin6->sin6_addr.s6_addr};
        bool allZero{true};
        for (size_t i{0}; i < 16u; ++i) {
            if (b[i] != 0u) {
                allZero = false;
                break;
            }
        }
        if (allZero) {
            return true; // ::
        }
        bool isLoopback{b[15] == 1u};
        for (size_t i{0}; i < 15u; ++i) {
            if (b[i] != 0u) {
                isLoopback = false;
                break;
            }
        }
        if (isLoopback) {
            return !allowLoopback; // ::1
        }
        if ((b[0] & 0xFEu) == 0xFCu ||                    // fc00::/7 ULA
            (b[0] == 0xFEu && (b[1] & 0xC0u) == 0x80u) || // fe80::/10 link-local
            b[0] == 0xFFu) {                              // ff00::/8 multicast
            return true;
        }
        // IPv4-mapped IPv6 ::ffff:0:0/96
        bool v4Mapped{b[10] == 0xFFu && b[11] == 0xFFu};
        for (size_t i{0}; i < 10u; ++i) {
            if (b[i] != 0u) {
                v4Mapped = false;
                break;
            }
        }
        if (v4Mapped) {
            const uint32_t v4Host{(static_cast<uint32_t>(b[12]) << 24u) | (static_cast<uint32_t>(b[13]) << 16u) |
                                  (static_cast<uint32_t>(b[14]) << 8u) | static_cast<uint32_t>(b[15])};
            return IsBlockedIpv4HostOrder(v4Host, allowLoopback);
        }
        return false;
    }
    return true;
}

curl_socket_t CurlOpenSocketCallback(void* clientp, curlsocktype purpose, struct curl_sockaddr* address) {
    const bool allowLoopback = clientp != nullptr && *static_cast<const bool*>(clientp);
    if (purpose != CURLSOCKTYPE_IPCXN || !address || IsBlockedSocketAddress(&address->addr, allowLoopback)) {
        return CURL_SOCKET_BAD;
    }
    return ::socket(address->family, address->socktype, address->protocol);
}
#endif

[[nodiscard]] bool IsLoopbackHttpLiteral(std::string_view url) noexcept {
    if (!StartsWithCaseInsensitive(url, "http://")) {
        return false;
    }
    std::string_view host = url.substr(7);
    const size_t slashPos = host.find_first_of("/?#");
    if (slashPos != std::string_view::npos) {
        host = host.substr(0, slashPos);
    }
    const size_t colonPos = host.rfind(':');
    if (colonPos != std::string_view::npos && host.find(']') == std::string_view::npos) {
        host = host.substr(0, colonPos);
    }
    return (host.size() == 9u && StartsWithCaseInsensitive(host, "127.0.0.1")) ||
           (host.size() == 9u && StartsWithCaseInsensitive(host, "localhost")) || host == "[::1]";
}

size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* buffer{static_cast<std::vector<uint8_t>*>(userp)};
    const size_t remaining{buffer->size() < kMaxAssetFileSize ? (kMaxAssetFileSize - buffer->size()) : 0};
    if (nmemb > 0 && size > remaining / nmemb) {
        return 0; // Exceeded limit or multiplication overflow; abort download safely with CURLE_WRITE_ERROR
    }
    const size_t totalBytes{size * nmemb};
    const auto* bytePtr{static_cast<const uint8_t*>(contents)};
    buffer->insert(buffer->end(), bytePtr, bytePtr + totalBytes);
    return totalBytes;
}

struct CurlProgressContext {
    const FetchProgressCallback* callback{nullptr};
    bool cancelled{false};
    std::chrono::steady_clock::time_point lastReport{};
};

int CurlXferInfoCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t /*ultotal*/,
                         curl_off_t /*ulnow*/) {
    auto* ctx{static_cast<CurlProgressContext*>(clientp)};
    if (!ctx || !ctx->callback || !(*ctx->callback)) {
        return 0;
    }
    const auto now = std::chrono::steady_clock::now();
    const bool finished = (dltotal > 0 && dlnow >= dltotal);
    if (!finished && (now - ctx->lastReport) < std::chrono::milliseconds(30)) {
        return 0;
    }
    ctx->lastReport = now;
    const size_t received = (dlnow > 0) ? static_cast<size_t>(dlnow) : 0u;
    const size_t total = (dltotal > 0) ? static_cast<size_t>(dltotal) : 0u;
    if (!(*ctx->callback)(FetchProgress{.bytesReceived = received, .totalBytes = total})) {
        ctx->cancelled = true;
        return 1;
    }
    return 0;
}
#endif

[[nodiscard]] Result<std::vector<uint8_t>> ReadLocalFileBytes(const std::filesystem::path& filePath,
                                                              const FetchProgressCallback& progressCb = {}) {
    std::error_code ec{};
    if (!std::filesystem::exists(filePath, ec) || !std::filesystem::is_regular_file(filePath, ec)) {
        return std::unexpected("File does not exist or is not a regular file: " + filePath.string());
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

    const size_t totalBytes{static_cast<size_t>(size)};
    std::vector<uint8_t> buffer(totalBytes);
    if (totalBytes == 0) {
        if (progressCb && !progressCb(FetchProgress{.bytesReceived = 0, .totalBytes = 0})) {
            return std::unexpected("Loading cancelled");
        }
        return buffer;
    }

    if (!progressCb) {
        if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
            return std::unexpected("Failed to read local file: " + filePath.string());
        }
        return buffer;
    }

    constexpr size_t kChunkBytes = 512ULL * 1024ULL;
    size_t offset{0};
    while (offset < totalBytes) {
        const size_t toRead{std::min(kChunkBytes, totalBytes - offset)};
        if (!file.read(reinterpret_cast<char*>(buffer.data() + offset), static_cast<std::streamsize>(toRead))) {
            return std::unexpected("Failed to read local file: " + filePath.string());
        }
        offset += toRead;
        if (!progressCb(FetchProgress{.bytesReceived = offset, .totalBytes = totalBytes})) {
            return std::unexpected("Loading cancelled");
        }
    }
    return buffer;
}

[[nodiscard]] std::string_view TrimUriViewNoAlloc(std::string_view s) noexcept {
    auto trimWs = [](std::string_view v) noexcept -> std::string_view {
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r' || v.front() == '\n')) {
            v.remove_prefix(1);
        }
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n')) {
            v.remove_suffix(1);
        }
        return v;
    };
    std::string_view cur = trimWs(s);
    if (cur.find('\n') != std::string_view::npos || cur.find('\r') != std::string_view::npos) {
        size_t pos{0};
        bool found{false};
        while (pos < cur.size()) {
            const size_t nl = cur.find_first_of("\r\n", pos);
            const std::string_view line =
                trimWs(cur.substr(pos, nl == std::string_view::npos ? cur.size() - pos : nl - pos));
            if (!line.empty() && line.front() != '#') {
                cur = line;
                found = true;
                break;
            }
            if (nl == std::string_view::npos) {
                break;
            }
            pos = nl + 1;
        }
        if (!found) {
            return {};
        }
    }
    if (cur.size() >= 2 && ((cur.front() == '"' && cur.back() == '"') || (cur.front() == '\'' && cur.back() == '\'') ||
                            (cur.front() == '<' && cur.back() == '>'))) {
        cur = trimWs(cur.substr(1, cur.size() - 2));
    }
    return cur;
}

[[nodiscard]] Result<std::string> ParseLocalFileUrlPath(std::string_view fileUrl) {
    if (!StartsWithCaseInsensitive(fileUrl, "file://")) {
        return std::string{fileUrl};
    }
    std::string_view rest = fileUrl.substr(7);
    if (!rest.empty() && rest.front() != '/') {
        const size_t slashPos = rest.find('/');
        if (slashPos == std::string_view::npos || !StartsWithCaseInsensitive(rest.substr(0, slashPos), "localhost") ||
            slashPos != 9u) {
            return std::unexpected("Rejected file:// URI with non-local authority");
        }
        rest.remove_prefix(slashPos);
    }
    std::string decodedPath{NetworkFetcher::DecodeUrlPercent(rest)};
    if (ContainsControlOrNul(decodedPath)) {
        return std::unexpected("Rejected file:// URI with control or NUL characters");
    }
    return decodedPath;
}

} // namespace

NetworkFetcher::ScopedAllowLoopbackHttpForTests::ScopedAllowLoopbackHttpForTests() noexcept
    : prev_{t_allowLoopbackHttpForTests} {
    t_allowLoopbackHttpForTests = true;
}

NetworkFetcher::ScopedAllowLoopbackHttpForTests::~ScopedAllowLoopbackHttpForTests() {
    t_allowLoopbackHttpForTests = prev_;
}

bool NetworkFetcher::IsHttpUrl(std::string_view uri) noexcept {
    const std::string_view cleaned{TrimUriViewNoAlloc(uri)};
    return StartsWithCaseInsensitive(cleaned, "http://") || StartsWithCaseInsensitive(cleaned, "https://");
}

bool NetworkFetcher::IsFileUrl(std::string_view uri) noexcept {
    const std::string_view cleaned{TrimUriViewNoAlloc(uri)};
    return StartsWithCaseInsensitive(cleaned, "file://");
}

bool NetworkFetcher::IsDataUri(std::string_view uri) noexcept {
    const std::string_view cleaned{TrimUriViewNoAlloc(uri)};
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
    return std::string{TrimUriViewNoAlloc(rawUri)};
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
    std::string cleanRel{SanitizeUri(relativeUri)};
    if (cleanRel.empty())
        return SanitizeUri(baseUri);

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
        const std::string_view baseNoQuery{(qPos == std::string::npos) ? std::string_view{cleanBase}
                                                                       : std::string_view{cleanBase}.substr(0, qPos)};

        const size_t schemePos{baseNoQuery.find("://")};
        if (schemePos == std::string_view::npos) {
            return cleanRel;
        }
        const size_t authorityStart{schemePos + 3};
        const size_t pathStart{baseNoQuery.find('/', authorityStart)};

        const std::string_view origin{(pathStart == std::string_view::npos) ? baseNoQuery
                                                                            : baseNoQuery.substr(0, pathStart)};

        // Split relative URI into path and optional query/fragment
        const size_t relQueryPos{cleanRel.find_first_of("?#")};
        const std::string_view relPath{(relQueryPos == std::string::npos)
                                           ? std::string_view{cleanRel}
                                           : std::string_view{cleanRel}.substr(0, relQueryPos)};
        const std::string_view relSuffix{
            (relQueryPos == std::string::npos) ? std::string_view{} : std::string_view{cleanRel}.substr(relQueryPos)};

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

Result<std::string> NetworkFetcher::ResolveExternalAssetUri(std::string_view baseUri, std::string_view relativeUri) {
    const std::string cleanRel{SanitizeUri(relativeUri)};
    if (cleanRel.empty()) {
        return std::unexpected("Empty external subresource URI");
    }
    if (ContainsControlOrNul(cleanRel)) {
        return std::unexpected("External URI contains invalid control or NUL characters");
    }

    const std::string cleanBase{SanitizeUri(baseUri)};
    std::string resolved{ResolveRelativeUri(cleanBase, cleanRel)};
    if (resolved.empty() || ContainsControlOrNul(resolved)) {
        return std::unexpected("Resolved external URI is invalid or contains control/NUL characters");
    }
    if (!IsDataUri(resolved) && ContainsControlOrNul(DecodeUrlPercent(resolved))) {
        return std::unexpected("Percent-decoded external URI contains invalid control or NUL characters");
    }

    if (IsHttpUrl(cleanBase)) {
        if (!IsHttpUrl(resolved) && !IsDataUri(resolved)) {
            return std::unexpected("Remote asset may only reference http(s):// or data: subresources");
        }
        if (StartsWithCaseInsensitive(cleanBase, "https://") && StartsWithCaseInsensitive(resolved, "http://")) {
            return std::unexpected("HTTPS asset may not downgrade subresource to plaintext http://");
        }
        return resolved;
    }

    if (cleanBase.empty() || IsDataUri(cleanBase) || StartsWithCaseInsensitive(cleanBase, "memory://")) {
        if (!IsDataUri(resolved)) {
            return std::unexpected("Inline data or memory-buffered asset may only reference data: subresources");
        }
        return resolved;
    }

    if (IsDataUri(resolved)) {
        return resolved;
    }

    if (IsHttpUrl(cleanRel) || IsHttpUrl(resolved)) {
        return std::unexpected("Local asset may not reference remote http(s):// subresources");
    }

    auto stripFileUrl = [](std::string_view s) -> Result<std::string> {
        if (!StartsWithCaseInsensitive(s, "file://")) {
            return std::string{s};
        }
        std::string_view rest = s.substr(7);
        if (!rest.empty() && rest.front() != '/') {
            const size_t slashPos = rest.find('/');
            if (slashPos != 9u || !StartsWithCaseInsensitive(rest.substr(0, slashPos), "localhost")) {
                return std::unexpected("Rejected file:// URI with non-local authority");
            }
            rest.remove_prefix(slashPos);
        }
        return NetworkFetcher::DecodeUrlPercent(rest);
    };

    if (!IsFileUrl(cleanBase) && IsFileUrl(cleanRel)) {
        return std::unexpected("Local filesystem asset may not use explicit file:// subresource");
    }
    if (!IsFileUrl(cleanBase) && std::filesystem::path{cleanRel}.is_absolute()) {
        return std::unexpected("Local filesystem asset subresource path must be relative");
    }

    auto baseFsRes = stripFileUrl(cleanBase);
    auto targetFsRes = stripFileUrl(resolved);
    if (!baseFsRes || !targetFsRes) {
        return std::unexpected(!baseFsRes ? baseFsRes.error() : targetFsRes.error());
    }

    std::error_code ec{};
    const std::filesystem::path rawParent{std::filesystem::path{*baseFsRes}.parent_path()};
    const std::filesystem::path baseDir{
        std::filesystem::weakly_canonical(rawParent.empty() ? std::filesystem::current_path(ec) : rawParent, ec)};
    const std::filesystem::path targetPath{std::filesystem::weakly_canonical(std::filesystem::path{*targetFsRes}, ec)};
    if (ec || baseDir.empty() || targetPath.empty()) {
        return std::unexpected("Failed to resolve canonical subresource path safely");
    }
    const std::filesystem::path relToBase{targetPath.lexically_relative(baseDir)};
    if (relToBase.empty() || relToBase == "." || *relToBase.begin() == ".." || relToBase.is_absolute()) {
        return std::unexpected("Rejected external subresource escaping asset base directory");
    }

    return resolved;
}

std::string NetworkFetcher::ExtractDisplayName(std::string_view uri) {
    const std::string cleaned{SanitizeUri(uri)};
    if (cleaned.empty())
        return "None";
    if (IsDataUri(cleaned))
        return "EmbeddedData";

    const size_t qPos{cleaned.find_first_of("?#")};
    std::string_view withoutQuery{(qPos == std::string::npos) ? std::string_view{cleaned}
                                                              : std::string_view{cleaned}.substr(0, qPos)};

    while (withoutQuery.size() > 1 && (withoutQuery.back() == '/' || withoutQuery.back() == '\\')) {
        withoutQuery.remove_suffix(1);
    }

    const size_t lastSlash{withoutQuery.find_last_of("/\\")};
    const std::string_view filename{(lastSlash == std::string_view::npos) ? withoutQuery
                                                                          : withoutQuery.substr(lastSlash + 1)};

    const std::string decoded{DecodeUrlPercent(filename)};
    return decoded.empty() ? cleaned : decoded;
}

Result<std::vector<uint8_t>> NetworkFetcher::FetchSync(std::string_view url) {
    return FetchSync(url, FetchProgressCallback{});
}

Result<std::vector<uint8_t>> NetworkFetcher::FetchSync(std::string_view url, const FetchProgressCallback& progressCb) {
    const std::string cleanedUri{SanitizeUri(url)};
    if (cleanedUri.empty()) {
        return std::unexpected("Cannot fetch empty URI");
    }
    if (ContainsControlOrNul(cleanedUri)) {
        return std::unexpected("Rejected URI containing control or NUL characters");
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
        if (decoded.size() > kMaxAssetFileSize) {
            return std::unexpected("Decoded data: URI exceeds maximum allowed asset size");
        }
        return std::vector<uint8_t>{decoded.begin(), decoded.end()};
    }

    // 2. Local file:// URI
    if (IsFileUrl(cleanedUri)) {
        auto parsedFile = ParseLocalFileUrlPath(cleanedUri);
        if (!parsedFile) {
            return std::unexpected(parsedFile.error());
        }
        return ReadLocalFileBytes(std::filesystem::path{*parsedFile}, progressCb);
    }

    // 3. Remote HTTP / HTTPS URL (HTTPS required for remote transfers)
    if (IsHttpUrl(cleanedUri)) {
        const bool allowLoopbackTest = t_allowLoopbackHttpForTests;
        const bool isAllowedTestHttp = allowLoopbackTest && IsLoopbackHttpLiteral(cleanedUri);
        if (!StartsWithCaseInsensitive(cleanedUri, "https://") && !isAllowedTestHttp) {
            return std::unexpected("Rejected insecure cleartext http:// URL (https:// required): " + cleanedUri);
        }
#if defined(BGL_PLATFORM_WASM) && defined(__EMSCRIPTEN__)
        emscripten_fetch_attr_t attr{};
        emscripten_fetch_attr_init(&attr);
        std::strcpy(attr.requestMethod, "GET");
        attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS;

        emscripten_fetch_t* fetch{emscripten_fetch(&attr, cleanedUri.c_str())};
        if (!fetch) {
            return std::unexpected("Emscripten fetch failed to initialize for URL: " + cleanedUri);
        }
        if (fetch->status >= 200 && fetch->status < 300 && fetch->data && fetch->numBytes > 0 &&
            static_cast<uint64_t>(fetch->numBytes) <= kMaxAssetFileSize) {
            const auto* bytePtr{reinterpret_cast<const uint8_t*>(fetch->data)};
            std::vector<uint8_t> buffer{bytePtr, bytePtr + fetch->numBytes};
            emscripten_fetch_close(fetch);
            return buffer;
        }
        const uint16_t status{fetch->status};
        emscripten_fetch_close(fetch);
        return std::unexpected("HTTP request failed or exceeded size cap (status " + std::to_string(status) +
                               ") for URL: " + cleanedUri);

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
        curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 5L);
#if defined(__linux__) || defined(__unix__)
        bool sockAllowLoopback = allowLoopbackTest;
        curl_easy_setopt(curl.get(), CURLOPT_OPENSOCKETFUNCTION, CurlOpenSocketCallback);
        curl_easy_setopt(curl.get(), CURLOPT_OPENSOCKETDATA, &sockAllowLoopback);
#endif
#if defined(CURLOPT_PROTOCOLS_STR) && defined(CURLOPT_REDIR_PROTOCOLS_STR)
        curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, isAllowedTestHttp ? "http,https" : "https");
        curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
#elif defined(CURLOPT_PROTOCOLS) && defined(CURLOPT_REDIR_PROTOCOLS)
        curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS,
                         isAllowedTestHttp ? (CURLPROTO_HTTP | CURLPROTO_HTTPS) : CURLPROTO_HTTPS);
        curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl.get(), CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
        curl_easy_setopt(curl.get(), CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(kMaxAssetFileSize));
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
        const std::string userAgent = std::string("bgl-engine/") + std::string(kEngineVersion) + " (libcurl)";
        curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, userAgent.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 120L);
        curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 30L);

        CurlProgressContext progressCtx{&progressCb, false, std::chrono::steady_clock::now()};
        if (progressCb) {
            if (!progressCb(FetchProgress{.bytesReceived = 0, .totalBytes = 0})) {
                return std::unexpected("Download cancelled");
            }
            curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, CurlXferInfoCallback);
            curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &progressCtx);
        }

        BGL_LOG_INFO("[NetworkFetcher] Downloading via libcurl: '{}'", SanitizeForLog(cleanedUri));
        const CURLcode res{curl_easy_perform(curl.get())};
        if (res != CURLE_OK) {
            if (progressCtx.cancelled || res == CURLE_ABORTED_BY_CALLBACK) {
                return std::unexpected("Download cancelled");
            }
            const std::string detail{errBuf[0] != '\0' ? errBuf.data() : curl_easy_strerror(res)};
            return std::unexpected("libcurl download failed for '" + cleanedUri + "': " + detail);
        }

        long httpCode{0};
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &httpCode);

        if (httpCode < 200 || httpCode >= 300) {
            return std::unexpected("HTTP error " + std::to_string(httpCode) + " when fetching URL: " + cleanedUri);
        }

        if (progressCb) {
            (void)progressCb(
                FetchProgress{.bytesReceived = responseBuffer.size(), .totalBytes = responseBuffer.size()});
        }

        BGL_LOG_INFO("[NetworkFetcher] Downloaded {} bytes ({:.2f} MB) from '{}' (HTTP {})", responseBuffer.size(),
                     static_cast<double>(responseBuffer.size()) / (1024.0 * 1024.0), SanitizeForLog(cleanedUri),
                     httpCode);
        return responseBuffer;
#else
        return std::unexpected("HTTP/HTTPS URL fetching is not enabled in this build: " + cleanedUri);
#endif
    }

    // 4. Local filesystem path (with automatic percent-decoding fallback)
    auto localRes = ReadLocalFileBytes(std::filesystem::path{cleanedUri}, progressCb);
    if (localRes) {
        return localRes;
    }
    if (cleanedUri.find('%') != std::string::npos) {
        const std::string decodedPath{DecodeUrlPercent(cleanedUri)};
        if (ContainsControlOrNul(decodedPath)) {
            return std::unexpected("Rejected local path containing control or NUL characters");
        }
        auto decodedRes = ReadLocalFileBytes(std::filesystem::path{decodedPath}, progressCb);
        if (decodedRes) {
            return decodedRes;
        }
    }
    return localRes;
}

void NetworkFetcher::FetchAsync(std::string_view url, FetchCallback callback) {
    std::string urlStr{url};
    ThreadPool::Instance().Enqueue([url = std::move(urlStr), cb = std::move(callback)]() mutable {
        auto res = FetchSync(url);
        if (cb) {
            cb(std::move(res));
        }
    });
}

} // namespace bgl::core
