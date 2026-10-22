/**
 * @file FontAtlas.hpp
 * @brief Dynamic GPU Font Atlas powered by FreeType 2 and HarfBuzz text shaping.
 */
#pragma once

#include <string_view>
#include <vector>
#include <unordered_map>
#include <span>
#include <memory>
#include <glm/glm.hpp>
#include <webgpu/webgpu.h>
#include "core/Types.hpp"
#include "gpu/Handle.hpp"

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
    glm::vec2 size{0.0f};     ///< Glyph quad width and height in pixels
    glm::vec2 bearing{0.0f};  ///< Horizontal bearing X and vertical bearing Y in pixels
    float advance{0.0f};      ///< Horizontal advance in pixels
    glm::vec2 uvMin{0.0f};    ///< Top-left UV coordinates in atlas (0..1)
    glm::vec2 uvMax{0.0f};    ///< Bottom-right UV coordinates in atlas (0..1)
};

/**
 * @class FontAtlas
 * @brief Manages FreeType face loading, glyph rasterization, HarfBuzz font handles,
 * and dynamic packing into an RGBA8 WebGPU texture atlas.
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
     * @brief Initializes the font atlas from TrueType font binary data in memory.
     * @param fontData Span of font file bytes.
     * @param pixelSize Font size in pixels (e.g. 18).
     * @param atlasDim Atlas width and height in pixels (default 1024).
     */
    core::Result<void> InitializeFromMemory(std::span<const uint8_t> fontData, uint32_t pixelSize = 18, uint32_t atlasDim = 1024);

    /**
     * @brief Initializes the font atlas by reading a TrueType font file from disk.
     * @param filePath Path to the .ttf font file.
     * @param pixelSize Font size in pixels.
     * @param atlasDim Atlas dimension in pixels.
     */
    core::Result<void> InitializeFromFile(std::string_view filePath, uint32_t pixelSize = 18, uint32_t atlasDim = 1024);

    /**
     * @brief Retrieves glyph metrics and atlas UVs for a given glyph index.
     * If the glyph has not been rasterized yet, it will be rasterized and packed dynamically.
     */
    const GlyphInfo& GetGlyph(uint32_t glyphIndex);

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

    [[nodiscard]] WGPUTexture GetGPUTexture() const noexcept { return gpuTexture_.Get(); }
    [[nodiscard]] WGPUTextureView GetGPUTextureView() const noexcept { return gpuTextureView_.Get(); }
    [[nodiscard]] WGPUSampler GetGPUSampler() const noexcept { return gpuSampler_.Get(); }
    [[nodiscard]] glm::vec2 GetWhitePixelUV() const noexcept { return whitePixelUV_; }
    [[nodiscard]] uint32_t GetAtlasDimension() const noexcept { return atlasDim_; }

private:
    void Cleanup();
    void RasterizeAndPack(uint32_t glyphIndex, GlyphInfo& info);

    FT_Library ftLibrary_{nullptr};
    FT_Face ftFace_{nullptr};
    hb_font_t* hbFont_{nullptr};
    std::vector<uint8_t> fontDataBuffer_{}; // Keep in memory if loaded from file

    uint32_t pixelSize_{18};
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
};

} // namespace bgl::text

