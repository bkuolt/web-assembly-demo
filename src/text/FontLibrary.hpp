/**
 * @file FontLibrary.hpp
 * @brief Cross-platform font discovery, metadata indexing, and smart query resolution via FreeType 2.
 */
#ifndef BGL_TEXT_FONTLIBRARY_HPP
#define BGL_TEXT_FONTLIBRARY_HPP

#include "core/Types.hpp"
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bgl::text {

/**
 * @enum FontWeight
 * @brief Desired typographic weight filter used during font lookup.
 */
enum class FontWeight : uint8_t {
    Any = 0,
    Regular = 1,
    Bold = 2,
    Light = 3,
    Medium = 4,
};

/**
 * @enum FontStyle
 * @brief Desired typographic slant filter used during font lookup.
 */
enum class FontStyle : uint8_t {
    Any = 0,
    Normal = 1,
    Italic = 2,
};

/**
 * @struct FontDescriptor
 * @brief Metadata describing a discovered or resolved font face.
 */
struct FontDescriptor {
    std::string family{};    ///< Family name (e.g. "Liberation Sans", "DejaVu Sans Mono")
    std::string style{};     ///< Style name (e.g. "Regular", "Bold", "Italic")
    std::string path{};      ///< Filesystem path to font binary (empty for embedded built-in font)
    long faceIndex{0};       ///< Face index inside TrueType/OpenType collections (.ttc/.otc)
    bool isMonospace{false}; ///< True for fixed-width fonts
    bool isBold{false};      ///< True when the face reports bold style flags
    bool isItalic{false};    ///< True when the face reports italic/oblique style flags
    bool isEmbedded{false};  ///< True for the built-in zero-dependency Liberation Mono fallback
};

/**
 * @struct FontQuery
 * @brief Structured query supporting exact file paths, family names, generic aliases ("sans", "mono", "serif"),
 *        weight/style filters, and extra search directories.
 */
struct FontQuery {
    std::string_view nameOrPath{};                      ///< Family, stem, generic role, or filesystem path
    FontWeight weight{FontWeight::Any};                 ///< Preferred font weight
    FontStyle style{FontStyle::Any};                    ///< Preferred font slant
    long faceIndex{0};                                  ///< Explicit face index when opening collection files
    std::span<const std::filesystem::path> extraDirs{}; ///< Additional directories scanned prior to system paths
};

/**
 * @class FontLibrary
 * @brief Discovers TrueType/OpenType fonts across workspace assets and OS font directories
 *        and resolves font queries without external platform libraries like Fontconfig.
 */
class FontLibrary {
public:
    /**
     * @brief Returns default project and OS-standard directories searched for `.ttf`, `.otf`, `.ttc` files.
     */
    [[nodiscard]] static std::vector<std::filesystem::path> GetDefaultSearchDirectories();

    /**
     * @brief Scans default directories (cached) plus any `extraDirs` and returns sorted unique font faces.
     * @param extraDirs Optional caller-supplied directories to include ahead of cached OS defaults.
     * @param forceRescan When true, refreshes the cached default directory index from disk.
     */
    [[nodiscard]] static std::vector<FontDescriptor> Discover(std::span<const std::filesystem::path> extraDirs = {},
                                                              bool forceRescan = false);

    /**
     * @brief Inspects a single font file path via FreeType 2 without rasterizing glyphs.
     */
    [[nodiscard]] static core::Result<std::vector<FontDescriptor>> InspectFile(const std::filesystem::path& filePath);

    /**
     * @brief Resolves the best-matching font face for a structured query (file path, family, or generic alias).
     */
    [[nodiscard]] static core::Result<FontDescriptor> Find(const FontQuery& query);

    /**
     * @brief Convenience overload resolving by font name, alias ("sans", "mono", "serif"), or file path.
     */
    [[nodiscard]] static core::Result<FontDescriptor>
    Find(std::string_view nameOrPath, FontWeight weight = FontWeight::Any, FontStyle style = FontStyle::Any);

    /**
     * @brief Reads raw bytes from a resolved font file on disk cleanly into memory.
     */
    [[nodiscard]] static core::Result<std::vector<uint8_t>> ReadFontBytes(const std::filesystem::path& filePath);

    /**
     * @brief Returns metadata describing the built-in embedded Liberation Mono font fallback.
     */
    [[nodiscard]] static FontDescriptor GetEmbeddedDefaultDescriptor();

    /**
     * @brief Clears the cached directory scan results.
     */
    static void ClearCache();
};

} // namespace bgl::text

#endif // BGL_TEXT_FONTLIBRARY_HPP
