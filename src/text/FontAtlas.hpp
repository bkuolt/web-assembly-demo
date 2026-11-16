/**
 * @file FontAtlas.hpp
 * @brief Dynamic GPU Font Atlas powered by FreeType 2 and HarfBuzz text shaping.
 */
#ifndef BGL_TEXT_FONTATLAS_HPP
#define BGL_TEXT_FONTATLAS_HPP

#include "core/Types.hpp"
#include "gpu/Handle.hpp"
#include "text/FontLibrary.hpp"
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <webgpu/webgpu.h>

// Forward declarations for third-party C types
typedef struct FT_LibraryRec_* FT_Library;
typedef struct FT_FaceRec_* FT_Face;
typedef struct hb_font_t hb_font_t;

namespace bgl::text {

/**
 * @struct GlyphInfo
 * @brief Metrics and texture atlas coordinates for a single rasterized glyph.
 */
struct GlyphInfo {
    uint32_t glyphIndex{0};
    glm::vec2 size{0.0f};    ///< Glyph quad width and height in pixels
    glm::vec2 bearing{0.0f}; ///< Horizontal bearing X and vertical bearing Y in pixels
    float advance{0.0f};     ///< Horizontal advance in pixels
    glm::vec2 uvMin{0.0f};   ///< Top-left UV coordinates in atlas (0..1)
    glm::vec2 uvMax{0.0f};   ///< Bottom-right UV coordinates in atlas (0..1)
};

/**
 * @class FontAtlas
 * @brief Manages FreeType face loading (including fallback faces), glyph rasterization,
 * HarfBuzz font handles, and dynamic packing into an RGBA8 WebGPU texture atlas.
 */
class FontAtlas {
public:
    FontAtlas();
    ~FontAtlas();

    FontAtlas(const FontAtlas&) = delete;
    FontAtlas& operator=(const FontAtlas&) = delete;
    FontAtlas(FontAtlas&& other) noexcept;
    FontAtlas& operator=(FontAtlas&& other) noexcept;

    /**
     * @brief Initializes the font atlas from TrueType/OpenType font binary data in memory.
     * @param fontData Span of font file bytes.
     * @param pixelSize Font size in pixels (e.g. 18 or 24 for SDF).
     * @param atlasDim Atlas width and height in pixels (default 1024).
     * @param useSdf Enable FreeType Signed Distance Field (SDF) rasterization for crisp scaling.
     * @param faceIndex Collection face index (default 0).
     */
    core::Result<void> InitializeFromMemory(std::span<const uint8_t> fontData, uint32_t pixelSize = 18,
                                            uint32_t atlasDim = 1024, bool useSdf = false, long faceIndex = 0);

    /**
     * @brief Initializes the font atlas by reading a TrueType/OpenType font file from disk.
     * @param filePath Path to the font file.
     * @param pixelSize Font size in pixels.
     * @param atlasDim Atlas dimension in pixels.
     * @param useSdf Enable FreeType Signed Distance Field (SDF) rasterization.
     * @param faceIndex Collection face index (default 0).
     */
    core::Result<void> InitializeFromFile(std::string_view filePath, uint32_t pixelSize = 18, uint32_t atlasDim = 1024,
                                          bool useSdf = false, long faceIndex = 0);

    /**
     * @brief Resolves a font via FontLibrary::Find(query) and initializes the atlas cleanly,
     *        optionally falling back to the built-in embedded font when unavailable.
     */
    core::Result<void> InitializeWithQuery(const FontQuery& query, uint32_t pixelSize = 18, uint32_t atlasDim = 1024,
                                           bool useSdf = false, bool allowEmbeddedFallback = true);

    /**
     * @brief Registers a secondary font file used automatically when primary glyphs are missing.
     */
    core::Result<void> AddFallbackFromFile(std::string_view filePath, long faceIndex = 0);

    /**
     * @brief Registers an in-memory secondary font used automatically when primary glyphs are missing.
     */
    core::Result<void> AddFallbackFromMemory(std::span<const uint8_t> fontData, long faceIndex = 0);

    [[nodiscard]] bool HasFallbackFaces() const noexcept { return !fallbackFaces_.empty(); }
    [[nodiscard]] bool IsSdf() const noexcept { return useSdf_; }

    /**
     * @brief Retrieves glyph metrics and atlas UVs for a primary face glyph index.
     */
    const GlyphInfo& GetGlyph(uint32_t glyphIndex);

    /**
     * @brief Retrieves primary glyph or resolves `utf32Codepoint` from registered fallback faces into the same atlas.
     * @param primaryGlyphIndex Glyph index produced by HarfBuzz for the primary face (`0` indicates missing glyph).
     * @param utf32Codepoint Unicode scalar value used to query fallback faces when `primaryGlyphIndex == 0`.
     * @param[out] usedFallback Set to `true` when resolved via a fallback face (use `info.advance` for stepping).
     */
    const GlyphInfo& GetGlyphOrFallback(uint32_t primaryGlyphIndex, uint32_t utf32Codepoint, bool& usedFallback);

    /**
     * @brief Pre-warms the atlas with all standard ASCII printable characters (32 to 126).
     */
    void PrewarmAscii();

    /**
     * @brief Synchronizes the CPU pixel buffer to the WebGPU texture if dirty.
     */
    void SyncGPU(WGPUDevice device, WGPUQueue queue);

    [[nodiscard]] hb_font_t* GetHarfBuzzFont() const noexcept { return hbFont_; }
    [[nodiscard]] FT_Face GetFTFace() const noexcept { return ftFace_; }
    [[nodiscard]] uint32_t GetPixelSize() const noexcept { return pixelSize_; }
    [[nodiscard]] float GetLineHeight() const noexcept { return lineHeight_; }
    [[nodiscard]] float GetAscender() const noexcept { return ascender_; }
    [[nodiscard]] float GetDescender() const noexcept { return descender_; }
    [[nodiscard]] std::string_view GetFamilyName() const noexcept { return familyName_; }
    [[nodiscard]] std::string_view GetStyleName() const noexcept { return styleName_; }
    [[nodiscard]] uint64_t GetGpuTextureGeneration() const noexcept { return gpuGeneration_; }

    [[nodiscard]] WGPUTexture GetGPUTexture() const noexcept { return gpuTexture_.Get(); }
    [[nodiscard]] WGPUTextureView GetGPUTextureView() const noexcept { return gpuTextureView_.Get(); }
    [[nodiscard]] WGPUSampler GetGPUSampler() const noexcept { return gpuSampler_.Get(); }
    [[nodiscard]] glm::vec2 GetWhitePixelUV() const noexcept { return whitePixelUV_; }
    [[nodiscard]] uint32_t GetAtlasDimension() const noexcept { return atlasDim_; }

    /** @brief Releases GPU texture/sampler resources as well as FreeType and HarfBuzz handles. */
    void Shutdown() noexcept { Cleanup(); }

private:
    static constexpr size_t kMaxFallbackFaces = 4;

    void Cleanup() noexcept;
    void RasterizeAndPack(FT_Face face, uint32_t faceGlyphIndex, uint32_t cacheKey, GlyphInfo& info);

    FT_Library ftLibrary_{nullptr};
    FT_Face ftFace_{nullptr};
    hb_font_t* hbFont_{nullptr};
    std::vector<uint8_t> fontDataBuffer_{};

    std::vector<FT_Face> fallbackFaces_{};
    std::vector<std::vector<uint8_t>> fallbackBuffers_{};

    std::string familyName_{};
    std::string styleName_{};
    uint32_t pixelSize_{18};
    bool useSdf_{false};
    float lineHeight_{22.0f};
    float ascender_{16.0f};
    float descender_{-4.0f};

    uint32_t atlasDim_{1024};
    std::vector<uint8_t> cpuAtlas_{};
    bool isDirty_{false};

    // Shelf / Skyline packing state
    uint32_t packX_{0};
    uint32_t packY_{0};
    uint32_t rowHeight_{0};
    glm::vec2 whitePixelUV_{0.0f};

    std::unordered_map<uint32_t, GlyphInfo> glyphCache_{};

    // WebGPU RAII resources
    gpu::TextureHandle gpuTexture_{};
    gpu::TextureViewHandle gpuTextureView_{};
    gpu::SamplerHandle gpuSampler_{};
    uint64_t gpuGeneration_{0};
};

} // namespace bgl::text

#endif // BGL_TEXT_FONTATLAS_HPP
