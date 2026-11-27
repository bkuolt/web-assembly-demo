/**
 * @file FontAtlas.cpp
 * @brief Dynamic GPU Font Atlas powered by FreeType 2 and HarfBuzz text shaping.
 */
#include "text/FontAtlas.hpp"
#include "core/Log.hpp"
#include "text/EmbeddedFont.hpp"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <cstring>
#include <harfbuzz/hb-ft.h>
#include <harfbuzz/hb.h>
#include <utility>

namespace bgl::text {

FontAtlas::FontAtlas() = default;

FontAtlas::~FontAtlas() {
    Cleanup();
}

FontAtlas::FontAtlas(FontAtlas&& other) noexcept
    : ftLibrary_{std::exchange(other.ftLibrary_, nullptr)}, ftFace_{std::exchange(other.ftFace_, nullptr)},
      hbFont_{std::exchange(other.hbFont_, nullptr)}, fontDataBuffer_{std::move(other.fontDataBuffer_)},
      fallbackFaces_{std::move(other.fallbackFaces_)}, fallbackBuffers_{std::move(other.fallbackBuffers_)},
      familyName_{std::move(other.familyName_)}, styleName_{std::move(other.styleName_)}, pixelSize_{other.pixelSize_},
      useSdf_{other.useSdf_}, lineHeight_{other.lineHeight_}, ascender_{other.ascender_}, descender_{other.descender_},
      atlasDim_{other.atlasDim_}, cpuAtlas_{std::move(other.cpuAtlas_)}, isDirty_{other.isDirty_}, packX_{other.packX_},
      packY_{other.packY_}, rowHeight_{other.rowHeight_}, whitePixelUV_{other.whitePixelUV_},
      glyphCache_{std::move(other.glyphCache_)}, gpuTexture_{std::move(other.gpuTexture_)},
      gpuTextureView_{std::move(other.gpuTextureView_)}, gpuSampler_{std::move(other.gpuSampler_)},
      gpuGeneration_{other.gpuGeneration_} {
    other.fallbackFaces_.clear();
}

FontAtlas& FontAtlas::operator=(FontAtlas&& other) noexcept {
    if (this != &other) {
        Cleanup();
        ftLibrary_ = std::exchange(other.ftLibrary_, nullptr);
        ftFace_ = std::exchange(other.ftFace_, nullptr);
        hbFont_ = std::exchange(other.hbFont_, nullptr);
        fontDataBuffer_ = std::move(other.fontDataBuffer_);
        fallbackFaces_ = std::move(other.fallbackFaces_);
        other.fallbackFaces_.clear();
        fallbackBuffers_ = std::move(other.fallbackBuffers_);
        familyName_ = std::move(other.familyName_);
        styleName_ = std::move(other.styleName_);
        pixelSize_ = other.pixelSize_;
        useSdf_ = other.useSdf_;
        lineHeight_ = other.lineHeight_;
        ascender_ = other.ascender_;
        descender_ = other.descender_;
        atlasDim_ = other.atlasDim_;
        cpuAtlas_ = std::move(other.cpuAtlas_);
        isDirty_ = other.isDirty_;
        packX_ = other.packX_;
        packY_ = other.packY_;
        rowHeight_ = other.rowHeight_;
        whitePixelUV_ = other.whitePixelUV_;
        glyphCache_ = std::move(other.glyphCache_);
        gpuTexture_ = std::move(other.gpuTexture_);
        gpuTextureView_ = std::move(other.gpuTextureView_);
        gpuSampler_ = std::move(other.gpuSampler_);
        gpuGeneration_ = other.gpuGeneration_;
    }
    return *this;
}

void FontAtlas::Cleanup() noexcept {
    gpuSampler_.Reset();
    gpuTextureView_.Reset();
    gpuTexture_.Reset();
    if (hbFont_) {
        hb_font_destroy(hbFont_);
        hbFont_ = nullptr;
    }
    for (FT_Face fb : fallbackFaces_) {
        if (fb) {
            FT_Done_Face(fb);
        }
    }
    fallbackFaces_.clear();
    fallbackBuffers_.clear();
    if (ftFace_) {
        FT_Done_Face(ftFace_);
        ftFace_ = nullptr;
    }
    if (ftLibrary_) {
        FT_Done_FreeType(ftLibrary_);
        ftLibrary_ = nullptr;
    }
    familyName_.clear();
    styleName_.clear();
    glyphCache_.clear();
    cpuAtlas_.clear();
    fontDataBuffer_.clear();
    isDirty_ = false;
}

core::Result<void> FontAtlas::InitializeFromMemory(std::span<const uint8_t> fontData, uint32_t pixelSize,
                                                   uint32_t atlasDim, bool useSdf, long faceIndex) {
    if (fontData.empty()) {
        return std::unexpected("Cannot initialize FontAtlas from empty byte span");
    }

    // Copy memory into owned backing storage before Cleanup() so temporary caller buffers remain valid
    // throughout the FreeType FT_Face lifetime.
    std::vector<uint8_t> ownedCopy;
    if (fontData.data() != fontDataBuffer_.data()) {
        ownedCopy.assign(fontData.begin(), fontData.end());
    } else {
        ownedCopy = std::move(fontDataBuffer_);
    }

    Cleanup();
    fontDataBuffer_ = std::move(ownedCopy);
    pixelSize_ = std::max(4u, pixelSize);
    atlasDim_ = std::clamp(atlasDim, 128u, 4096u);
    useSdf_ = useSdf;

    if (FT_Init_FreeType(&ftLibrary_)) {
        fontDataBuffer_.clear();
        return std::unexpected("Failed to initialize FreeType 2 library");
    }

    FT_Error err = FT_New_Memory_Face(ftLibrary_, fontDataBuffer_.data(), static_cast<FT_Long>(fontDataBuffer_.size()),
                                      faceIndex, &ftFace_);
    if (err) {
        Cleanup();
        return std::unexpected(std::string("FreeType failed to load font face from memory: error ") +
                               std::to_string(err));
    }

    FT_Set_Pixel_Sizes(ftFace_, 0, pixelSize_);

    hbFont_ = hb_ft_font_create_referenced(ftFace_);
    if (!hbFont_) {
        Cleanup();
        return std::unexpected("Failed to create HarfBuzz font via hb_ft_font_create_referenced");
    }

    familyName_ = (ftFace_->family_name && ftFace_->family_name[0] != '\0') ? ftFace_->family_name : "Unknown";
    styleName_ = (ftFace_->style_name && ftFace_->style_name[0] != '\0') ? ftFace_->style_name : "Regular";

    if (ftFace_->size && ftFace_->size->metrics.height > 0) {
        lineHeight_ = static_cast<float>(ftFace_->size->metrics.height) / 64.0f;
        ascender_ = static_cast<float>(ftFace_->size->metrics.ascender) / 64.0f;
        descender_ = static_cast<float>(ftFace_->size->metrics.descender) / 64.0f;
    } else {
        lineHeight_ = static_cast<float>(pixelSize_) * 1.25f;
        ascender_ = static_cast<float>(pixelSize_);
        descender_ = -static_cast<float>(pixelSize_) * 0.25f;
    }

    cpuAtlas_.assign(static_cast<size_t>(atlasDim_) * atlasDim_ * 4u, 0);

    for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
            const size_t idx = (static_cast<size_t>(y) * atlasDim_ + x) * 4u;
            cpuAtlas_[idx + 0] = 255;
            cpuAtlas_[idx + 1] = 255;
            cpuAtlas_[idx + 2] = 255;
            cpuAtlas_[idx + 3] = 255;
        }
    }
    whitePixelUV_ = glm::vec2(4.0f / static_cast<float>(atlasDim_), 4.0f / static_cast<float>(atlasDim_));

    packX_ = 10;
    packY_ = 0;
    rowHeight_ = 10;
    isDirty_ = true;

    PrewarmAscii();

    BGL_LOG_INFO("FontAtlas initialized: '{}' ({}) size={}px, lineH={:.1f}px, atlas={}x{}", familyName_, styleName_,
                 pixelSize_, lineHeight_, atlasDim_, atlasDim_);
    return {};
}

core::Result<void> FontAtlas::InitializeFromFile(std::string_view filePath, uint32_t pixelSize, uint32_t atlasDim,
                                                 bool useSdf, long faceIndex) {
    auto bytesRes = FontLibrary::ReadFontBytes(std::filesystem::path{filePath});
    if (!bytesRes) {
        return std::unexpected(bytesRes.error());
    }
    return InitializeFromMemory(*bytesRes, pixelSize, atlasDim, useSdf, faceIndex);
}

core::Result<void> FontAtlas::InitializeWithQuery(const FontQuery& query, uint32_t pixelSize, uint32_t atlasDim,
                                                  bool useSdf, bool allowEmbeddedFallback) {
    if (auto found = FontLibrary::Find(query); found) {
        if (found->isEmbedded) {
            return InitializeFromMemory(GetEmbeddedFontData(), pixelSize, atlasDim, useSdf);
        }
        if (!found->path.empty()) {
            if (auto res = InitializeFromFile(found->path, pixelSize, atlasDim, useSdf, found->faceIndex); res) {
                return {};
            } else if (!allowEmbeddedFallback) {
                return std::unexpected(res.error());
            }
        } else if (!allowEmbeddedFallback) {
            return std::unexpected("Resolved font has empty path");
        }
    } else if (!allowEmbeddedFallback) {
        return std::unexpected(found.error());
    }

    return InitializeFromMemory(GetEmbeddedFontData(), pixelSize, atlasDim, useSdf);
}

core::Result<void> FontAtlas::AddFallbackFromFile(std::string_view filePath, long faceIndex) {
    auto bytesRes = FontLibrary::ReadFontBytes(std::filesystem::path{filePath});
    if (!bytesRes) {
        return std::unexpected(bytesRes.error());
    }
    return AddFallbackFromMemory(*bytesRes, faceIndex);
}

core::Result<void> FontAtlas::AddFallbackFromMemory(std::span<const uint8_t> fontData, long faceIndex) {
    if (!ftLibrary_ || !ftFace_) {
        return std::unexpected("Primary FontAtlas must be initialized before adding fallback fonts");
    }
    if (fallbackFaces_.size() >= kMaxFallbackFaces) {
        return std::unexpected("Maximum number of fallback fonts reached for this FontAtlas");
    }
    if (fontData.empty()) {
        return std::unexpected("Cannot register empty fallback font byte span");
    }

    std::vector<uint8_t> backing(fontData.begin(), fontData.end());
    FT_Face fbFace{nullptr};
    const FT_Error err =
        FT_New_Memory_Face(ftLibrary_, backing.data(), static_cast<FT_Long>(backing.size()), faceIndex, &fbFace);
    if (err || !fbFace) {
        return std::unexpected(std::string("FreeType failed to load fallback font: error ") + std::to_string(err));
    }

    FT_Set_Pixel_Sizes(fbFace, 0, pixelSize_);
    fallbackBuffers_.push_back(std::move(backing));
    fallbackFaces_.push_back(fbFace);
    return {};
}

const GlyphInfo& FontAtlas::GetGlyph(uint32_t glyphIndex) {
    const uint32_t key = glyphIndex & 0x00FFFFFFu;
    if (const auto it = glyphCache_.find(key); it != glyphCache_.end()) {
        return it->second;
    }

    GlyphInfo info;
    RasterizeAndPack(ftFace_, glyphIndex, key, info);
    auto [insertedIt, _] = glyphCache_.emplace(key, info);
    return insertedIt->second;
}

const GlyphInfo& FontAtlas::GetGlyphOrFallback(uint32_t primaryGlyphIndex, uint32_t utf32Codepoint,
                                               bool& usedFallback) {
    usedFallback = false;
    if (primaryGlyphIndex != 0 || utf32Codepoint == 0 || fallbackFaces_.empty()) {
        return GetGlyph(primaryGlyphIndex);
    }

    for (size_t slot = 0; slot < fallbackFaces_.size(); ++slot) {
        FT_Face fbFace = fallbackFaces_[slot];
        if (!fbFace) {
            continue;
        }
        const FT_UInt fbGlyph = FT_Get_Char_Index(fbFace, utf32Codepoint);
        if (fbGlyph == 0) {
            continue;
        }

        usedFallback = true;
        const uint32_t cacheKey =
            (static_cast<uint32_t>(slot + 1u) << 24u) | (static_cast<uint32_t>(fbGlyph) & 0x00FFFFFFu);
        if (const auto it = glyphCache_.find(cacheKey); it != glyphCache_.end()) {
            return it->second;
        }
        GlyphInfo info;
        RasterizeAndPack(fbFace, static_cast<uint32_t>(fbGlyph), cacheKey, info);
        auto [insertedIt, _] = glyphCache_.emplace(cacheKey, info);
        return insertedIt->second;
    }

    return GetGlyph(primaryGlyphIndex);
}

void FontAtlas::RasterizeAndPack(FT_Face face, uint32_t faceGlyphIndex, uint32_t cacheKey, GlyphInfo& info) {
    info.glyphIndex = cacheKey;
    if (!face) {
        return;
    }

    FT_Error err =
        FT_Load_Glyph(face, faceGlyphIndex, useSdf_ ? FT_LOAD_DEFAULT : (FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL));
    if (!err && useSdf_) {
        err = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_SDF);
        if (err) {
            err = FT_Load_Glyph(face, faceGlyphIndex, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL);
        }
    }
    if (err) {
        BGL_LOG_WARN("FreeType failed to render glyph index {}", faceGlyphIndex);
        info.size = glm::vec2{0.0f};
        info.uvMin = whitePixelUV_;
        info.uvMax = whitePixelUV_;
        return;
    }

    FT_GlyphSlot slot = face->glyph;
    const FT_Bitmap& bitmap = slot->bitmap;

    info.size = glm::vec2(static_cast<float>(bitmap.width), static_cast<float>(bitmap.rows));
    info.bearing = glm::vec2(static_cast<float>(slot->bitmap_left), static_cast<float>(slot->bitmap_top));
    info.advance = static_cast<float>(slot->advance.x >> 6);

    if (!bitmap.buffer || bitmap.width == 0 || bitmap.rows == 0) {
        info.uvMin = whitePixelUV_;
        info.uvMax = whitePixelUV_;
        return;
    }

    const uint32_t padding = useSdf_ ? 2u : 1u;
    const uint32_t glyphW = bitmap.width + 2u * padding;
    const uint32_t glyphH = bitmap.rows + 2u * padding;

    uint32_t nextX = packX_;
    uint32_t nextY = packY_;
    uint32_t nextRowH = rowHeight_;
    if (nextX + glyphW > atlasDim_) {
        nextX = 0;
        nextY += nextRowH;
        nextRowH = 0;
    }

    if (glyphW > atlasDim_ || nextY + glyphH > atlasDim_) {
        BGL_LOG_ERROR("Font Atlas ({}x{}) overflowed while packing glyph index {}", atlasDim_, atlasDim_,
                      faceGlyphIndex);
        info.size = glm::vec2{0.0f};
        info.uvMin = whitePixelUV_;
        info.uvMax = whitePixelUV_;
        return;
    }

    const uint32_t targetX = nextX + padding;
    const uint32_t targetY = nextY + padding;

    for (uint32_t y = 0; y < bitmap.rows; ++y) {
        const uint32_t srcRow = (bitmap.pitch >= 0) ? y : (bitmap.rows - 1u - y);
        const size_t absPitch = static_cast<size_t>(bitmap.pitch >= 0 ? bitmap.pitch : -bitmap.pitch);
        const uint8_t* rowPtr = bitmap.buffer + static_cast<size_t>(srcRow) * absPitch;
        for (uint32_t x = 0; x < bitmap.width; ++x) {
            uint8_t alpha = 0;
            if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
                if (x < absPitch) {
                    alpha = rowPtr[x];
                }
            } else if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
                const size_t byteIdx = static_cast<size_t>(x) >> 3u;
                if (byteIdx < absPitch) {
                    alpha = ((rowPtr[byteIdx] & (0x80u >> (x & 7u))) != 0u) ? 255u : 0u;
                }
            } else if (bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
                const size_t byteIdx = static_cast<size_t>(x) * 4u + 3u;
                if (byteIdx < absPitch) {
                    alpha = rowPtr[byteIdx];
                }
            }
            const size_t dstIdx = (static_cast<size_t>(targetY + y) * atlasDim_ + (targetX + x)) * 4u;
            cpuAtlas_[dstIdx + 0] = 255;
            cpuAtlas_[dstIdx + 1] = 255;
            cpuAtlas_[dstIdx + 2] = 255;
            cpuAtlas_[dstIdx + 3] = alpha;
        }
    }

    info.uvMin = glm::vec2(static_cast<float>(targetX) / static_cast<float>(atlasDim_),
                           static_cast<float>(targetY) / static_cast<float>(atlasDim_));
    info.uvMax = glm::vec2(static_cast<float>(targetX + bitmap.width) / static_cast<float>(atlasDim_),
                           static_cast<float>(targetY + bitmap.rows) / static_cast<float>(atlasDim_));

    packX_ = nextX + glyphW;
    packY_ = nextY;
    rowHeight_ = std::max(nextRowH, glyphH);
    isDirty_ = true;
}

void FontAtlas::PrewarmAscii() {
    if (!ftFace_) {
        return;
    }

    for (uint32_t c = 32; c <= 126; ++c) {
        const FT_UInt gIdx = FT_Get_Char_Index(ftFace_, c);
        if (gIdx != 0) {
            GetGlyph(gIdx);
        }
    }
}

void FontAtlas::SyncGPU(WGPUDevice device, WGPUQueue queue) {
    if (!device || !queue || cpuAtlas_.empty()) {
        return;
    }

    if (!gpuTexture_) {
        WGPUTextureDescriptor texDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        texDesc.size = WGPUExtent3D{atlasDim_, atlasDim_, 1};
        texDesc.mipLevelCount = 1;
        texDesc.sampleCount = 1;
        texDesc.dimension = WGPUTextureDimension_2D;
        texDesc.format = WGPUTextureFormat_RGBA8Unorm;
        texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        texDesc.label = gpu::ToWgpuStringView("FontAtlasTexture");
        gpuTexture_ = wgpuDeviceCreateTexture(device, &texDesc);
        if (!gpuTexture_) {
            BGL_LOG_ERROR("Failed to create GPU texture for FontAtlas ({}x{})", atlasDim_, atlasDim_);
            return;
        }

        WGPUTextureViewDescriptor viewDesc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        viewDesc.format = WGPUTextureFormat_RGBA8Unorm;
        viewDesc.dimension = WGPUTextureViewDimension_2D;
        viewDesc.baseMipLevel = 0;
        viewDesc.mipLevelCount = 1;
        viewDesc.baseArrayLayer = 0;
        viewDesc.arrayLayerCount = 1;
        viewDesc.aspect = WGPUTextureAspect_All;
        gpuTextureView_ = wgpuTextureCreateView(gpuTexture_, &viewDesc);

        WGPUSamplerDescriptor sampDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
        sampDesc.addressModeU = WGPUAddressMode_ClampToEdge;
        sampDesc.addressModeV = WGPUAddressMode_ClampToEdge;
        sampDesc.addressModeW = WGPUAddressMode_ClampToEdge;
        sampDesc.magFilter = WGPUFilterMode_Linear;
        sampDesc.minFilter = WGPUFilterMode_Linear;
        sampDesc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
        gpuSampler_ = wgpuDeviceCreateSampler(device, &sampDesc);
        if (!gpuTextureView_ || !gpuSampler_) {
            gpuSampler_.Reset();
            gpuTextureView_.Reset();
            gpuTexture_.Reset();
            return;
        }

        ++gpuGeneration_;
        isDirty_ = true;
    }

    if (isDirty_ && gpuTexture_) {
        WGPUTexelCopyTextureInfo copyDst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        copyDst.texture = gpuTexture_;
        copyDst.mipLevel = 0;
        copyDst.origin = WGPUOrigin3D{0, 0, 0};
        copyDst.aspect = WGPUTextureAspect_All;

        WGPUTexelCopyBufferLayout dataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        dataLayout.offset = 0;
        dataLayout.bytesPerRow = atlasDim_ * 4u;
        dataLayout.rowsPerImage = atlasDim_;

        const WGPUExtent3D writeSize{atlasDim_, atlasDim_, 1};
        wgpuQueueWriteTexture(queue, &copyDst, cpuAtlas_.data(), cpuAtlas_.size(), &dataLayout, &writeSize);
        isDirty_ = false;
    }
}

} // namespace bgl::text
