/**
 * @file FontLibrary.cpp
 * @brief Cross-platform font discovery, metadata indexing, and smart query resolution via FreeType 2.
 */
#include "text/FontLibrary.hpp"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <unordered_set>

namespace bgl::text {

namespace {

constexpr size_t kMaxScannedFontFiles = 4096u;
constexpr FT_Long kMaxCollectionFaces = 16;

struct DiscoveryCache {
    std::mutex mutex{};
    bool populated{false};
    std::vector<FontDescriptor> entries{};
};

DiscoveryCache& GetCache() {
    static DiscoveryCache cache{};
    return cache;
}

[[nodiscard]] std::string ToLowerAscii(std::string_view s) {
    std::string out(s.size(), '\0');
    for (size_t i = 0; i < s.size(); ++i) {
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    }
    return out;
}

[[nodiscard]] bool HasSupportedExtension(const std::filesystem::path& p) {
    const std::string ext = ToLowerAscii(p.extension().string());
    return ext == ".ttf" || ext == ".otf" || ext == ".ttc" || ext == ".otc";
}

[[nodiscard]] std::string NormalizePathKey(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::path canon = std::filesystem::weakly_canonical(p, ec);
    return (ec ? p : canon).lexically_normal().generic_string();
}

void AppendEnvDir(std::vector<std::filesystem::path>& out, const char* envName, const char* subPath = nullptr) {
    if (const char* val = std::getenv(envName); val != nullptr && val[0] != '\0') {
        std::filesystem::path p{val};
        if (subPath && subPath[0] != '\0') {
            p /= subPath;
        }
        out.push_back(std::move(p));
    }
}

void InspectPathWithLibrary(FT_Library ftLib, const std::filesystem::path& path, std::vector<FontDescriptor>& out,
                            FT_Long maxFaces = kMaxCollectionFaces) {
    const std::string pathStr = path.string();
    FT_Face face0{nullptr};
    if (FT_New_Face(ftLib, pathStr.c_str(), 0, &face0) != 0 || !face0) {
        return;
    }

    const FT_Long totalFaces = std::clamp<FT_Long>(face0->num_faces, 1, maxFaces);
    auto extractFace = [&](FT_Face face, FT_Long idx) {
        if (!face) {
            return;
        }
        FontDescriptor desc{};
        desc.family =
            (face->family_name && face->family_name[0] != '\0') ? std::string{face->family_name} : path.stem().string();
        desc.style = (face->style_name && face->style_name[0] != '\0') ? std::string{face->style_name} : "Regular";
        desc.path = pathStr;
        desc.faceIndex = static_cast<long>(idx);
        desc.isMonospace = (face->face_flags & FT_FACE_FLAG_FIXED_WIDTH) != 0;
        desc.isBold = (face->style_flags & FT_STYLE_FLAG_BOLD) != 0;
        desc.isItalic = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
        out.push_back(std::move(desc));
    };

    extractFace(face0, 0);
    FT_Done_Face(face0);

    for (FT_Long idx = 1; idx < totalFaces; ++idx) {
        FT_Face subFace{nullptr};
        if (FT_New_Face(ftLib, pathStr.c_str(), idx, &subFace) == 0 && subFace) {
            extractFace(subFace, idx);
            FT_Done_Face(subFace);
        }
    }
}

void ScanDirectories(std::span<const std::filesystem::path> dirs, std::vector<FontDescriptor>& out) {
    FT_Library ftLib{nullptr};
    if (FT_Init_FreeType(&ftLib) != 0 || !ftLib) {
        return;
    }

    std::unordered_set<std::string> seenFiles{};
    size_t filesScanned = 0;

    for (const auto& dir : dirs) {
        if (dir.empty()) {
            continue;
        }
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
            continue;
        }
        std::filesystem::recursive_directory_iterator it{
            dir, std::filesystem::directory_options::skip_permission_denied, ec};
        for (; it != std::filesystem::recursive_directory_iterator{}; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (filesScanned >= kMaxScannedFontFiles) {
                break;
            }
            if (!it->is_regular_file(ec) || !HasSupportedExtension(it->path())) {
                continue;
            }
            const std::string key = NormalizePathKey(it->path());
            if (!seenFiles.insert(key).second) {
                continue;
            }
            ++filesScanned;
            InspectPathWithLibrary(ftLib, it->path(), out);
        }
    }

    FT_Done_FreeType(ftLib);
}

void SortAndDeduplicate(std::vector<FontDescriptor>& list) {
    std::ranges::sort(list, [](const FontDescriptor& a, const FontDescriptor& b) {
        const std::string fa = ToLowerAscii(a.family);
        const std::string fb = ToLowerAscii(b.family);
        if (fa != fb) {
            return fa < fb;
        }
        if (a.isBold != b.isBold) {
            return !a.isBold && b.isBold;
        }
        if (a.isItalic != b.isItalic) {
            return !a.isItalic && b.isItalic;
        }
        return a.style < b.style;
    });
    std::unordered_set<std::string> uniqueKeys{};
    std::erase_if(list, [&](const FontDescriptor& d) {
        const std::string key = d.path + "#" + std::to_string(d.faceIndex);
        return !uniqueKeys.insert(key).second;
    });
}

[[nodiscard]] int ScoreFontCandidate(const FontDescriptor& desc, const FontQuery& query) {
    const std::string qLower = ToLowerAscii(query.nameOrPath);
    const std::string famLower = ToLowerAscii(desc.family);
    const std::string stemLower = ToLowerAscii(std::filesystem::path{desc.path}.stem().string());
    const std::string styleLower = ToLowerAscii(desc.style);

    int score = 0;
    const bool wantsDefaultOrUi = qLower.empty() || qLower == "default" || qLower == "ui";
    const bool wantsSans = wantsDefaultOrUi || qLower == "sans" || qLower == "sans-serif";
    const bool wantsMono = qLower == "mono" || qLower == "monospace" || qLower == "code";
    const bool wantsSerif = qLower == "serif";

    if (wantsSans) {
        static constexpr std::array<std::string_view, 11> kSansPrefs{
            "inter",     "roboto",   "noto sans", "dejavu sans", "liberation sans", "ubuntu",
            "cantarell", "segoe ui", "sf pro",    "arial",       "helvetica",
        };
        for (size_t i = 0; i < kSansPrefs.size(); ++i) {
            if (famLower == kSansPrefs[i]) {
                score += 600 - static_cast<int>(i * 25);
                break;
            }
            if (famLower.find(kSansPrefs[i]) != std::string::npos) {
                score += 380 - static_cast<int>(i * 20);
                break;
            }
        }
        if (!desc.isMonospace) {
            score += 40;
        }
    } else if (wantsMono) {
        static constexpr std::array<std::string_view, 9> kMonoPrefs{
            "jetbrains mono", "fira code", "cascadia", "liberation mono", "dejavu sans mono",
            "ubuntu mono",    "consolas",  "menlo",    "courier",
        };
        for (size_t i = 0; i < kMonoPrefs.size(); ++i) {
            if (famLower == kMonoPrefs[i]) {
                score += 650 - static_cast<int>(i * 25);
                break;
            }
            if (famLower.find(kMonoPrefs[i]) != std::string::npos) {
                score += 420 - static_cast<int>(i * 20);
                break;
            }
        }
        if (desc.isMonospace) {
            score += 240;
        }
    } else if (wantsSerif) {
        static constexpr std::array<std::string_view, 6> kSerifPrefs{
            "noto serif", "liberation serif", "dejavu serif", "georgia", "times new roman", "times",
        };
        for (size_t i = 0; i < kSerifPrefs.size(); ++i) {
            if (famLower == kSerifPrefs[i]) {
                score += 600 - static_cast<int>(i * 25);
                break;
            }
            if (famLower.find(kSerifPrefs[i]) != std::string::npos) {
                score += 360 - static_cast<int>(i * 20);
                break;
            }
        }
    } else {
        if (famLower == qLower || stemLower == qLower) {
            score += 1000;
        } else if (famLower.starts_with(qLower) || stemLower.starts_with(qLower)) {
            score += 700;
        } else if (famLower.find(qLower) != std::string::npos || stemLower.find(qLower) != std::string::npos) {
            score += 450;
        }
    }

    if (score <= 0) {
        return 0;
    }

    if (query.weight == FontWeight::Regular) {
        score += (!desc.isBold && styleLower.find("light") == std::string::npos) ? 140 : -60;
    } else if (query.weight == FontWeight::Bold) {
        score += desc.isBold ? 180 : -80;
    } else if (query.weight == FontWeight::Light) {
        score += (styleLower.find("light") != std::string::npos) ? 160 : -50;
    } else if (query.weight == FontWeight::Medium) {
        score += (styleLower.find("medium") != std::string::npos || styleLower.find("semibold") != std::string::npos)
                     ? 160
                     : 0;
    } else if (!desc.isBold) {
        score += 35; // Prefer regular faces slightly when weight is unspecified
    }

    if (query.style == FontStyle::Normal) {
        score += !desc.isItalic ? 120 : -80;
    } else if (query.style == FontStyle::Italic) {
        score += desc.isItalic ? 160 : -80;
    } else if (!desc.isItalic) {
        score += 20;
    }

    return score;
}

} // namespace

std::vector<std::filesystem::path> FontLibrary::GetDefaultSearchDirectories() {
    std::vector<std::filesystem::path> dirs;
    dirs.emplace_back("assets/fonts");
    dirs.emplace_back("../assets/fonts");
    dirs.emplace_back("fonts");

#if defined(_WIN32)
    AppendEnvDir(dirs, "LOCALAPPDATA", "Microsoft/Windows/Fonts");
    AppendEnvDir(dirs, "WINDIR", "Fonts");
#elif defined(__APPLE__)
    AppendEnvDir(dirs, "HOME", "Library/Fonts");
    dirs.emplace_back("/Library/Fonts");
    dirs.emplace_back("/System/Library/Fonts");
#else
    AppendEnvDir(dirs, "HOME", ".local/share/fonts");
    AppendEnvDir(dirs, "HOME", ".fonts");
    dirs.emplace_back("/usr/local/share/fonts");
    dirs.emplace_back("/usr/share/fonts");
#endif
    return dirs;
}

std::vector<FontDescriptor> FontLibrary::Discover(std::span<const std::filesystem::path> extraDirs, bool forceRescan) {
    std::vector<FontDescriptor> defaultEntries;
    {
        auto& cache = GetCache();
        std::scoped_lock lock{cache.mutex};
        if (!cache.populated || forceRescan) {
            cache.entries.clear();
            const auto defaults = GetDefaultSearchDirectories();
            ScanDirectories(defaults, cache.entries);
            SortAndDeduplicate(cache.entries);
            cache.populated = true;
        }
        defaultEntries = cache.entries;
    }

    if (extraDirs.empty()) {
        return defaultEntries;
    }

    std::vector<FontDescriptor> extraEntries;
    ScanDirectories(extraDirs, extraEntries);
    extraEntries.insert(extraEntries.end(), std::make_move_iterator(defaultEntries.begin()),
                        std::make_move_iterator(defaultEntries.end()));
    SortAndDeduplicate(extraEntries);
    return extraEntries;
}

core::Result<std::vector<FontDescriptor>> FontLibrary::InspectFile(const std::filesystem::path& filePath) {
    std::error_code ec;
    if (!std::filesystem::exists(filePath, ec) || !std::filesystem::is_regular_file(filePath, ec)) {
        return std::unexpected("Font file does not exist: " + filePath.string());
    }

    FT_Library ftLib{nullptr};
    if (FT_Init_FreeType(&ftLib) != 0 || !ftLib) {
        return std::unexpected("Failed to initialize FreeType 2 library");
    }

    std::vector<FontDescriptor> out;
    InspectPathWithLibrary(ftLib, filePath, out);
    FT_Done_FreeType(ftLib);

    if (out.empty()) {
        return std::unexpected("Unrecognized or unreadable font file: " + filePath.string());
    }
    return out;
}

core::Result<FontDescriptor> FontLibrary::Find(const FontQuery& query) {
    if (!query.nameOrPath.empty()) {
        const std::string qLower = ToLowerAscii(query.nameOrPath);
        if (qLower == "embedded" || qLower == "builtin") {
            return GetEmbeddedDefaultDescriptor();
        }
        const std::filesystem::path candidate{query.nameOrPath};
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            auto facesRes = InspectFile(candidate);
            if (!facesRes) {
                return std::unexpected(facesRes.error());
            }
            const auto& faces = *facesRes;
            const size_t idx = (query.faceIndex >= 0 && static_cast<size_t>(query.faceIndex) < faces.size())
                                   ? static_cast<size_t>(query.faceIndex)
                                   : size_t{0};
            return faces[idx];
        }

        for (const auto& extra : query.extraDirs) {
            const std::filesystem::path rel = extra / candidate;
            if (std::filesystem::is_regular_file(rel, ec)) {
                auto facesRes = InspectFile(rel);
                if (facesRes && !facesRes->empty()) {
                    return facesRes->front();
                }
            }
        }
    }

    const std::vector<FontDescriptor> catalog = Discover(query.extraDirs);
    int bestScore = 0;
    const FontDescriptor* bestMatch = nullptr;

    for (const auto& entry : catalog) {
        const int sc = ScoreFontCandidate(entry, query);
        if (sc > bestScore) {
            bestScore = sc;
            bestMatch = &entry;
        }
    }

    if (bestMatch) {
        return *bestMatch;
    }

    return std::unexpected("No installed font matched query '" + std::string{query.nameOrPath} + "'");
}

core::Result<FontDescriptor> FontLibrary::Find(std::string_view nameOrPath, FontWeight weight, FontStyle style) {
    return Find(FontQuery{.nameOrPath = nameOrPath, .weight = weight, .style = style});
}

core::Result<std::vector<uint8_t>> FontLibrary::ReadFontBytes(const std::filesystem::path& filePath) {
    constexpr uintmax_t kMaxFontFileBytes{64u * 1024u * 1024u};
    std::error_code ec{};
    if (!std::filesystem::is_regular_file(filePath, ec) || ec) {
        return std::unexpected("Font path is not a regular file: " + filePath.string());
    }
    const uintmax_t fileSize = std::filesystem::file_size(filePath, ec);
    if (ec || fileSize == 0 || fileSize > kMaxFontFileBytes) {
        return std::unexpected("Font file size invalid or exceeds 64 MiB limit: " + filePath.string());
    }

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        return std::unexpected("Cannot open font file: " + filePath.string());
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(fileSize));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(fileSize))) {
        return std::unexpected("Failed to read font file: " + filePath.string());
    }
    return bytes;
}

FontDescriptor FontLibrary::GetEmbeddedDefaultDescriptor() {
    return FontDescriptor{
        .family = "Liberation Mono",
        .style = "Regular",
        .path = {},
        .faceIndex = 0,
        .isMonospace = true,
        .isBold = false,
        .isItalic = false,
        .isEmbedded = true,
    };
}

void FontLibrary::ClearCache() {
    auto& cache = GetCache();
    std::scoped_lock lock{cache.mutex};
    cache.entries.clear();
    cache.populated = false;
}

} // namespace bgl::text
