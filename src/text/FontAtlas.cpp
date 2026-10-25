/**
 * @file FontAtlas.cpp
 * @brief Dynamic GPU Font Atlas powered by FreeType 2 and HarfBuzz text shaping.
 */
#include "text/FontAtlas.hpp"
#include "core/Log.hpp"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>
#include <fstream>
#include <algorithm>
#include <cstring>

namespace bgl::text {

FontAtlas::FontAtlas() = default;

FontAtlas::~FontAtlas() {
    Cleanup();
}

FontAtlas::FontAtlas(FontAtlas&& other) noexcept
    : ftLibrary_{std::exchange(other.ftLibrary_, nullptr)}
    , ftFace_{std::exchange(other.ftFace_, nullptr)}
    , hbFont_{std::exchange(other.hbFont_, nullptr)}
    , fontDataBuffer_{std::move(other.fontDataBuffer_)}
    , pixelSize_{other.pixelSize_}
    , lineHeight_{other.lineHeight_}
    , ascender_{other.ascender_}
    , descender_{other.descender_}
    , atlasDim_{other.atlasDim_}
    , cpuAtlas_{std::move(other.cpuAtlas_)}
    , isDirty_{other.isDirty_}
    , packX_{other.packX_}
    , packY_{other.packY_}
    , rowHeight_{other.rowHeight_}
    , whitePixelUV_{other.whitePixelUV_}
    , glyphCache_{std::move(other.glyphCache_)}
    , gpuTexture_{std::move(other.gpuTexture_)}
    , gpuTextureView_{std::move(other.gpuTextureView_)}
    , gpuSampler_{std::move(other.gpuSampler_)} {}

FontAtlas& FontAtlas::operator=(FontAtlas&& other) noexcept {
    if (this != &other) {
        Cleanup();
        ftLibrary_ = std::exchange(other.ftLibrary_, nullptr);
        ftFace_ = std::exchange(other.ftFace_, nullptr);
        hbFont_ = std::exchange(other.hbFont_, nullptr);
        fontDataBuffer_ = std::move(other.fontDataBuffer_);
        pixelSize_ = other.pixelSize_;
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
    }
    return *this;
}

void FontAtlas::Cleanup() {
    gpuSampler_.Reset();
    gpuTextureView_.Reset();
    gpuTexture_.Reset();
    if (hbFont_) {
        hb_font_destroy(hbFont_);
        hbFont_ = nullptr;
    }
    if (ftFace_) {
        FT_Done_Face(ftFace_);
        ftFace_ = nullptr;
    }
    if (ftLibrary_) {
        FT_Done_FreeType(ftLibrary_);
        ftLibrary_ = nullptr;
    }
    glyphCache_.clear();
    cpuAtlas_.clear();
}

core::Result<void> FontAtlas::InitializeFromMemory(std::span<const uint8_t> fontData, uint32_t pixelSize, uint32_t atlasDim) {
    Cleanup();
    pixelSize_ = pixelSize;
    atlasDim_ = atlasDim;

    if (FT_Init_FreeType(&ftLibrary_)) {
        return std::unexpected("Failed to initialize FreeType 2 library");
    }

    FT_Error err = FT_New_Memory_Face(ftLibrary_, fontData.data(), static_cast<FT_Long>(fontData.size()), 0, &ftFace_);
    if (err) {
        Cleanup();
        return std::unexpected(std::string("FreeType failed to load font face from memory: error ") + std::to_string(err));
    }

    FT_Set_Pixel_Sizes(ftFace_, 0, pixelSize_);

    hbFont_ = hb_ft_font_create_referenced(ftFace_);
    if (!hbFont_) {
        Cleanup();
        return std::unexpected("Failed to create HarfBuzz font via hb_ft_font_create_referenced");
    }

    if (ftFace_->size && ftFace_->size->metrics.height > 0) {
        lineHeight_ = static_cast<float>(ftFace_->size->metrics.height) / 64.0f;
        ascender_ = static_cast<float>(ftFace_->size->metrics.ascender) / 64.0f;
        descender_ = static_cast<float>(ftFace_->size->metrics.descender) / 64.0f;
    } else {
        lineHeight_ = static_cast<float>(pixelSize_) * 1.25f;
        ascender_ = static_cast<float>(pixelSize_);
        descender_ = -static_cast<float>(pixelSize_) * 0.25f;
    }

    // Allocate RGBA8 CPU pixel buffer initialized to 0
    cpuAtlas_.assign(atlasDim_ * atlasDim_ * 4, 0);

    // Reserve 8x8 white pixel block at (0, 0)
    for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
            uint32_t idx = (y * atlasDim_ + x) * 4;
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

    BGL_LOG_INFO("FontAtlas initialized: size={}px, lineH={:.1f}px, ascender={:.1f}px, descender={:.1f}px, atlas={}x{}",
                 pixelSize_, lineHeight_, ascender_, descender_, atlasDim_, atlasDim_);
    return {};
}

core::Result<void> FontAtlas::InitializeFromFile(std::string_view filePath, uint32_t pixelSize, uint32_t atlasDim) {
    std::ifstream file(std::string(filePath), std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return std::unexpected(std::string("Cannot open font file: ") + std::string(filePath));
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    fontDataBuffer_.resize(size);
    if (!file.read(reinterpret_cast<char*>(fontDataBuffer_.data()), size)) {
        return std::unexpected(std::string("Failed to read font file: ") + std::string(filePath));
    }

    return InitializeFromMemory(fontDataBuffer_, pixelSize, atlasDim);
}

const GlyphInfo& FontAtlas::GetGlyph(uint32_t glyphIndex) {
    auto it = glyphCache_.find(glyphIndex);
    if (it != glyphCache_.end()) {
        return it->second;
    }

    GlyphInfo info;
    RasterizeAndPack(glyphIndex, info);
    auto [insertedIt, _] = glyphCache_.emplace(glyphIndex, info);
    return insertedIt->second;
}

void FontAtlas::RasterizeAndPack(uint32_t glyphIndex, GlyphInfo& info) {
    info.glyphIndex = glyphIndex;
    if (!ftFace_) return;

    FT_Error err = FT_Load_Glyph(ftFace_, glyphIndex, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL);
    if (err) {
        BGL_LOG_WARN("FreeType failed to render glyph index {}", glyphIndex);
        info.uvMin = whitePixelUV_;
        info.uvMax = whitePixelUV_;
        return;
    }

    FT_GlyphSlot slot = ftFace_->glyph;
    const FT_Bitmap& bitmap = slot->bitmap;

    info.size = glm::vec2(static_cast<float>(bitmap.width), static_cast<float>(bitmap.rows));
    info.bearing = glm::vec2(static_cast<float>(slot->bitmap_left), static_cast<float>(slot->bitmap_top));
    info.advance = static_cast<float>(slot->advance.x >> 6);

    if (bitmap.width == 0 || bitmap.rows == 0) {
        info.uvMin = whitePixelUV_;
        info.uvMax = whitePixelUV_;
        return;
    }

    constexpr uint32_t padding = 1;
    const uint32_t glyphW = bitmap.width + 2 * padding;
    const uint32_t glyphH = bitmap.rows + 2 * padding;

    if (packX_ + glyphW > atlasDim_) {
        packX_ = 0;
        packY_ += rowHeight_;
        rowHeight_ = 0;
    }

    if (packY_ + glyphH > atlasDim_) {
        BGL_LOG_ERROR("Font Atlas ({}x{}) overflowed while packing glyph index {}", atlasDim_, atlasDim_, glyphIndex);
        info.uvMin = whitePixelUV_;
        info.uvMax = whitePixelUV_;
        return;
    }

    uint32_t targetX = packX_ + padding;
    uint32_t targetY = packY_ + padding;

    for (uint32_t y = 0; y < bitmap.rows; ++y) {
        for (uint32_t x = 0; x < bitmap.width; ++x) {
            uint8_t alpha = bitmap.buffer[y * bitmap.pitch + x];
            uint32_t dstIdx = ((targetY + y) * atlasDim_ + (targetX + x)) * 4;
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

    packX_ += glyphW;
    rowHeight_ = std::max(rowHeight_, glyphH);
    isDirty_ = true;
}

void FontAtlas::PrewarmAscii() {
    if (!ftFace_) return;

    for (uint32_t c = 32; c <= 126; ++c) {
        FT_UInt gIdx = FT_Get_Char_Index(ftFace_, c);
        if (gIdx != 0) {
            GetGlyph(gIdx);
        }
    }
}

void FontAtlas::SyncGPU(WGPUDevice device, WGPUQueue queue) {
    if (!device || !queue) return;

    if (!gpuTexture_) {
        WGPUTextureDescriptor texDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        texDesc.size = WGPUExtent3D{ atlasDim_, atlasDim_, 1 };
        texDesc.mipLevelCount = 1;
        texDesc.sampleCount = 1;
        texDesc.dimension = WGPUTextureDimension_2D;
        texDesc.format = WGPUTextureFormat_RGBA8Unorm;
        texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        texDesc.label = gpu::ToWgpuStringView("FontAtlasTexture");
        gpuTexture_ = wgpuDeviceCreateTexture(device, &texDesc);

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

        isDirty_ = true;
    }

    if (isDirty_ && gpuTexture_) {
        WGPUTexelCopyTextureInfo copyDst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        copyDst.texture = gpuTexture_;
        copyDst.mipLevel = 0;
        copyDst.origin = WGPUOrigin3D{ 0, 0, 0 };
        copyDst.aspect = WGPUTextureAspect_All;

        WGPUTexelCopyBufferLayout dataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        dataLayout.offset = 0;
        dataLayout.bytesPerRow = atlasDim_ * 4;
        dataLayout.rowsPerImage = atlasDim_;

        WGPUExtent3D writeSize = { atlasDim_, atlasDim_, 1 };
        wgpuQueueWriteTexture(queue, &copyDst, cpuAtlas_.data(), cpuAtlas_.size(), &dataLayout, &writeSize);
        isDirty_ = false;
    }
}

} // namespace bgl::text

