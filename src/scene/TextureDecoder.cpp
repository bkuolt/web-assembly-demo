/**
 * @file TextureDecoder.cpp
 * @brief Memory decoding for PNG, JPEG, WebP, and KTX2 textures using modular decoders.
 */
#include "scene/TextureDecoder.hpp"
#include "core/Log.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

#if defined(BGL_PLATFORM_NATIVE)

#if defined(BGL_ENABLE_PNG)
#include <png.h>
struct PngMemoryReader {
    const uint8_t* data;
    size_t size;
    size_t offset;
};

static void PngReadCallback(png_structp png_ptr, png_bytep outBytes, png_size_t byteCountToRead) {
    auto* reader = static_cast<PngMemoryReader*>(png_get_io_ptr(png_ptr));
    if (reader->offset + byteCountToRead > reader->size) {
        png_error(png_ptr, "PNG read EOF reached unexpectedly");
        return;
    }
    std::memcpy(outBytes, reader->data + reader->offset, byteCountToRead);
    reader->offset += byteCountToRead;
}
#endif

#if defined(BGL_ENABLE_JPEG)
#include <csetjmp>
#include <jpeglib.h>

struct CustomJpegErrorMgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void JpegErrorExit(j_common_ptr cinfo) {
    auto* myerr = reinterpret_cast<CustomJpegErrorMgr*>(cinfo->err);
    longjmp(myerr->setjmp_buffer, 1);
}
#endif

#if defined(BGL_ENABLE_WEBP)
#include <webp/decode.h>
#endif

#if defined(BGL_ENABLE_KTX)
#include <ktx.h>
#endif

#endif

namespace bgl::scene {

static constexpr size_t kMaxTextureDimension{16384};

#if defined(BGL_PLATFORM_NATIVE) && defined(BGL_ENABLE_PNG)
static bool ReadPngStreamIsolateSetjmp(png_structp png_ptr, png_infop info_ptr, std::span<const uint8_t> data,
                                       DecodedImage& outImg, std::vector<png_bytep>& rowPointers) {
    if (setjmp(png_jmpbuf(png_ptr))) {
        return false;
    }

    png_set_user_limits(png_ptr, static_cast<png_uint_32>(kMaxTextureDimension),
                        static_cast<png_uint_32>(kMaxTextureDimension));

    PngMemoryReader reader{data.data(), data.size(), 8};
    png_set_read_fn(png_ptr, &reader, PngReadCallback);
    png_set_sig_bytes(png_ptr, 8);

    png_read_info(png_ptr, info_ptr);

    png_uint_32 width{0};
    png_uint_32 height{0};
    int bit_depth{0};
    int color_type{0};
    png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth, &color_type, nullptr, nullptr, nullptr);

    if (width == 0 || height == 0 || width > kMaxTextureDimension || height > kMaxTextureDimension) {
        return false;
    }

    if (bit_depth == 16) {
        png_set_strip_16(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    }
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png_ptr);
    } else if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY ||
               color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_add_alpha(png_ptr, 0xFF, PNG_FILLER_AFTER);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png_ptr);
    }

    png_read_update_info(png_ptr, info_ptr);

    const size_t totalBytes{static_cast<size_t>(width) * static_cast<size_t>(height) * 4u};
    outImg.width = width;
    outImg.height = height;
    outImg.channels = 4;
    outImg.rgbaData.resize(totalBytes);

    rowPointers.resize(height);
    for (size_t y{0}; y < height; ++y) {
        rowPointers[y] = &outImg.rgbaData[y * static_cast<size_t>(width) * 4u];
    }

    png_read_image(png_ptr, rowPointers.data());
    return true;
}
#endif

#if defined(BGL_PLATFORM_NATIVE) && defined(BGL_ENABLE_JPEG)
static bool ReadJpegStreamIsolateSetjmp(std::span<const uint8_t> data, DecodedImage& outImg,
                                        std::vector<uint8_t>& rowBuffer) {
    struct jpeg_decompress_struct cinfo{};
    CustomJpegErrorMgr jerr{};

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = JpegErrorExit;

    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, const_cast<unsigned char*>(data.data()), data.size());

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    if (cinfo.output_width == 0 || cinfo.output_height == 0 || cinfo.output_width > kMaxTextureDimension ||
        cinfo.output_height > kMaxTextureDimension) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    const size_t width{cinfo.output_width};
    const size_t height{cinfo.output_height};
    outImg.width = cinfo.output_width;
    outImg.height = cinfo.output_height;
    outImg.channels = 4;
    outImg.rgbaData.resize(width * height * 4u);

    rowBuffer.resize(width * 3u);
    JSAMPROW rowPtr{rowBuffer.data()};

    size_t y{0};
    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, &rowPtr, 1);
        for (size_t x{0}; x < width; ++x) {
            outImg.rgbaData[(y * width + x) * 4u + 0u] = rowBuffer[x * 3u + 0u];
            outImg.rgbaData[(y * width + x) * 4u + 1u] = rowBuffer[x * 3u + 1u];
            outImg.rgbaData[(y * width + x) * 4u + 2u] = rowBuffer[x * 3u + 2u];
            outImg.rgbaData[(y * width + x) * 4u + 3u] = 255;
        }
        ++y;
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}
#endif

core::Result<DecodedImage> TextureDecoder::DecodePNG(std::span<const uint8_t> data) {
#if defined(BGL_PLATFORM_NATIVE) && defined(BGL_ENABLE_PNG)
    static constexpr std::array<uint8_t, 8> kPngMagic{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (data.size() < kPngMagic.size() || std::memcmp(data.data(), kPngMagic.data(), kPngMagic.size()) != 0) {
        return std::unexpected("Data is not a valid PNG signature");
    }

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr)
        return std::unexpected("Failed to create png read struct");

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_read_struct(&png_ptr, nullptr, nullptr);
        return std::unexpected("Failed to create png info struct");
    }

    DecodedImage img{};
    std::vector<png_bytep> rowPointers{};
    const bool ok{ReadPngStreamIsolateSetjmp(png_ptr, info_ptr, data, img, rowPointers)};
    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);

    if (!ok) {
        return std::unexpected("Error occurred during PNG decoding");
    }
    return img;
#elif defined(BGL_PLATFORM_NATIVE)
    (void)data;
    return std::unexpected("PNG texture decoder disabled at build time (-DBGL_ENABLE_PNG=OFF)");
#else
    (void)data;
    DecodedImage img;
    img.width = 2;
    img.height = 2;
    img.rgbaData = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    return img;
#endif
}

core::Result<DecodedImage> TextureDecoder::DecodeWebP(std::span<const uint8_t> data) {
#if defined(BGL_PLATFORM_NATIVE) && defined(BGL_ENABLE_WEBP)
    if (data.empty()) {
        return std::unexpected("WebP data is empty");
    }
    int width{0};
    int height{0};
    std::unique_ptr<uint8_t, decltype(&WebPFree)> decoded{WebPDecodeRGBA(data.data(), data.size(), &width, &height),
                                                          &WebPFree};
    if (!decoded) {
        return std::unexpected("Failed to decode WebP stream");
    }
    if (width <= 0 || height <= 0 || static_cast<size_t>(width) > kMaxTextureDimension ||
        static_cast<size_t>(height) > kMaxTextureDimension) {
        return std::unexpected("WebP image dimensions exceed maximum allowed limits");
    }
    const size_t totalBytes{static_cast<size_t>(width) * static_cast<size_t>(height) * 4u};
    DecodedImage img{};
    img.width = static_cast<uint32_t>(width);
    img.height = static_cast<uint32_t>(height);
    img.channels = 4;
    img.rgbaData.assign(decoded.get(), decoded.get() + totalBytes);
    return img;
#elif defined(BGL_PLATFORM_NATIVE)
    (void)data;
    return std::unexpected("WebP texture decoder disabled at build time (-DBGL_ENABLE_WEBP=OFF)");
#else
    (void)data;
    DecodedImage img;
    img.width = 2;
    img.height = 2;
    img.rgbaData = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    return img;
#endif
}

core::Result<DecodedImage> TextureDecoder::DecodeJPEG(std::span<const uint8_t> data) {
#if defined(BGL_PLATFORM_NATIVE) && defined(BGL_ENABLE_JPEG)
    if (data.size() < 4) {
        return std::unexpected("JPEG data too small or empty");
    }

    DecodedImage img{};
    std::vector<uint8_t> rowBuffer{};
    if (!ReadJpegStreamIsolateSetjmp(data, img, rowBuffer)) {
        return std::unexpected("libjpeg decompression error");
    }
    return img;
#elif defined(BGL_PLATFORM_NATIVE)
    (void)data;
    return std::unexpected("JPEG texture decoder disabled at build time (-DBGL_ENABLE_JPEG=OFF)");
#else
    (void)data;
    DecodedImage img;
    img.width = 2;
    img.height = 2;
    img.rgbaData = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    return img;
#endif
}

namespace {

std::atomic<KtxTranscodeTarget> g_defaultKtxTarget{KtxTranscodeTarget::Rgba32};

[[nodiscard]] float HalfToFloat(uint16_t h) noexcept {
    const uint32_t sign{static_cast<uint32_t>(h & 0x8000u) << 16};
    uint32_t exp{static_cast<uint32_t>(h >> 10) & 0x1Fu};
    uint32_t mant{static_cast<uint32_t>(h & 0x03FFu)};

    uint32_t bits{0};
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            exp = 1;
            while ((mant & 0x0400u) == 0) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x03FFu;
            bits = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
        }
    } else if (exp == 0x1Fu) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
    }
    return std::bit_cast<float>(bits);
}

} // namespace

void TextureDecoder::SetDefaultKtxTarget(KtxTranscodeTarget target) noexcept {
    g_defaultKtxTarget.store(target, std::memory_order_relaxed);
}

KtxTranscodeTarget TextureDecoder::GetDefaultKtxTarget() noexcept {
    return g_defaultKtxTarget.load(std::memory_order_relaxed);
}

core::Result<DecodedImage> TextureDecoder::DecodeKTX2(std::span<const uint8_t> data) {
    return DecodeKTX2(data, GetDefaultKtxTarget());
}

core::Result<DecodedImage> TextureDecoder::DecodeKTX2(std::span<const uint8_t> data, KtxTranscodeTarget target) {
#if defined(BGL_PLATFORM_NATIVE) && defined(BGL_ENABLE_KTX)
    static constexpr std::array<uint8_t, 12> kKtx2Ident{0xAB, 'K',  'T',  'X',  ' ',  '2',
                                                        '0',  0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    static constexpr std::array<uint8_t, 12> kKtx1Ident{0xAB, 'K',  'T',  'X',  ' ',  '1',
                                                        '1',  0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    if (data.size() < 12) {
        return std::unexpected("Data too small to be a valid KTX texture");
    }
    if (std::memcmp(data.data(), kKtx2Ident.data(), 12) == 0) {
        if (data.size() < 80) {
            return std::unexpected("KTX2 data is truncated (header smaller than 80 bytes)");
        }
    } else if (std::memcmp(data.data(), kKtx1Ident.data(), 12) == 0) {
        if (data.size() < 64) {
            return std::unexpected("KTX1 data is truncated (header smaller than 64 bytes)");
        }
    } else {
        return std::unexpected("Data is not a valid KTX1 or KTX2 texture signature");
    }

    ktxTexture* rawKtx{nullptr};
    KTX_error_code result{
        ktxTexture_CreateFromMemory(data.data(), data.size(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &rawKtx)};

    if (result != KTX_SUCCESS || !rawKtx) {
        return std::unexpected(std::string("ktxTexture_CreateFromMemory failed: error ") + std::to_string(result));
    }
    const auto ktxDeleter = [](ktxTexture* t) noexcept {
        if (t)
            ktxTexture_Destroy(t);
    };
    std::unique_ptr<ktxTexture, decltype(ktxDeleter)> kTexture{rawKtx, ktxDeleter};

    const uint32_t width{kTexture->baseWidth};
    const uint32_t height{kTexture->baseHeight};
    if (width == 0 || height == 0 || width > kMaxTextureDimension || height > kMaxTextureDimension) {
        return std::unexpected("KTX texture contains invalid dimensions");
    }

    // Block-compressed formats require 4x4 block alignment on base mip dimensions in WebGPU
    KtxTranscodeTarget effectiveTarget{target};
    if (effectiveTarget != KtxTranscodeTarget::Rgba32 && ((width % 4u) != 0u || (height % 4u) != 0u)) {
        effectiveTarget = KtxTranscodeTarget::Rgba32;
    }

    // Check if transcoding is needed for Basis Universal KTX2 textures (UASTC / ETC1S)
    bool transcodedCompressed{false};
    if (kTexture->classId == ktxTexture2_c) {
        auto* ktx2{reinterpret_cast<ktxTexture2*>(kTexture.get())};
        if (ktxTexture2_NeedsTranscoding(ktx2)) {
            ktx_transcode_fmt_e ktxFmt{KTX_TTF_RGBA32};
            switch (effectiveTarget) {
            case KtxTranscodeTarget::Bc7:
                ktxFmt = KTX_TTF_BC7_RGBA;
                break;
            case KtxTranscodeTarget::Astc4x4:
                ktxFmt = KTX_TTF_ASTC_4x4_RGBA;
                break;
            case KtxTranscodeTarget::Etc2Rgba:
                ktxFmt = KTX_TTF_ETC2_RGBA;
                break;
            case KtxTranscodeTarget::Rgba32:
            default:
                ktxFmt = KTX_TTF_RGBA32;
                break;
            }
            result = ktxTexture2_TranscodeBasis(ktx2, ktxFmt, 0);
            if (result != KTX_SUCCESS && ktxFmt != KTX_TTF_RGBA32) {
                // Fallback to uncompressed RGBA32 if compressed transcode fails
                effectiveTarget = KtxTranscodeTarget::Rgba32;
                result = ktxTexture2_TranscodeBasis(ktx2, KTX_TTF_RGBA32, 0);
            }
            if (result != KTX_SUCCESS) {
                return std::unexpected(std::string("ktxTexture2_TranscodeBasis failed: error ") +
                                       std::to_string(result));
            }
            transcodedCompressed = (effectiveTarget != KtxTranscodeTarget::Rgba32);
        } else {
            effectiveTarget = KtxTranscodeTarget::Rgba32;
        }
    } else {
        effectiveTarget = KtxTranscodeTarget::Rgba32;
    }

    const ktx_size_t imageSize{ktxTexture_GetImageSize(kTexture.get(), 0)};
    ktx_size_t level0Offset{0};
    ktxTexture_GetImageOffset(kTexture.get(), 0, 0, 0, &level0Offset);
    const ktx_uint8_t* baseDataPtr{ktxTexture_GetData(kTexture.get())};

    if (!baseDataPtr || imageSize == 0) {
        return std::unexpected("KTX texture contains no image data");
    }
    const ktx_uint8_t* imageBytes{baseDataPtr + level0Offset};

    DecodedImage img{};
    img.width = width;
    img.height = height;
    img.channels = 4;
    img.isCompressed = transcodedCompressed;
    img.compressionFormat = effectiveTarget;

    // Extract mip chain for compressed or multi-mip KTX2 textures
    const uint32_t numLevels{std::max(1u, kTexture->numLevels)};
    img.mipLevels.reserve(numLevels);
    for (uint32_t level{0}; level < numLevels; ++level) {
        const uint32_t mipW{std::max(1u, width >> level)};
        const uint32_t mipH{std::max(1u, height >> level)};
        if (transcodedCompressed && ((mipW < 4u) || (mipH < 4u) || (mipW % 4u != 0u) || (mipH % 4u != 0u))) {
            break;
        }
        const ktx_size_t mipBytesSize{ktxTexture_GetImageSize(kTexture.get(), level)};
        ktx_size_t mipOffset{0};
        if (ktxTexture_GetImageOffset(kTexture.get(), level, 0, 0, &mipOffset) != KTX_SUCCESS || mipBytesSize == 0) {
            break;
        }
        DecodedMipLevel mip{};
        mip.width = mipW;
        mip.height = mipH;
        mip.data.assign(baseDataPtr + mipOffset, baseDataPtr + mipOffset + mipBytesSize);
        img.mipLevels.push_back(std::move(mip));
    }

    if (transcodedCompressed) {
        img.rgbaData.assign(imageBytes, imageBytes + imageSize);
        return img;
    }

    const size_t expectedRgbaSize{static_cast<size_t>(width) * static_cast<size_t>(height) * 4u};
    const size_t expectedRgbSize{static_cast<size_t>(width) * static_cast<size_t>(height) * 3u};

    if (imageSize >= expectedRgbaSize) {
        img.rgbaData.resize(expectedRgbaSize);
        std::memcpy(img.rgbaData.data(), imageBytes, expectedRgbaSize);
    } else if (imageSize == expectedRgbSize) {
        img.rgbaData.resize(expectedRgbaSize);
        for (size_t i{0}; i < static_cast<size_t>(width) * height; ++i) {
            img.rgbaData[i * 4 + 0] = imageBytes[i * 3 + 0];
            img.rgbaData[i * 4 + 1] = imageBytes[i * 3 + 1];
            img.rgbaData[i * 4 + 2] = imageBytes[i * 3 + 2];
            img.rgbaData[i * 4 + 3] = 255;
        }
    } else {
        img.rgbaData.resize(imageSize);
        std::memcpy(img.rgbaData.data(), imageBytes, imageSize);
    }

    return img;
#elif defined(BGL_PLATFORM_NATIVE)
    (void)data;
    (void)target;
    return std::unexpected("KTX/KTX2 texture decoder disabled at build time (-DBGL_ENABLE_KTX=OFF)");
#else
    (void)data;
    (void)target;
    DecodedImage img{};
    img.width = 2;
    img.height = 2;
    img.channels = 4;
    img.rgbaData = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    return img;
#endif
}

core::Result<DecodedHdrImage> TextureDecoder::DecodeHDR(std::span<const uint8_t> data) {
    if (data.size() < 16) {
        return std::unexpected("HDR data buffer is too small");
    }

    const std::string_view textView{reinterpret_cast<const char*>(data.data()), data.size()};
    if (!textView.starts_with("#?RADIANCE") && !textView.starts_with("#?RGBE") && !textView.starts_with("#?")) {
        return std::unexpected("Data does not start with Radiance .hdr signature (#?RADIANCE / #?RGBE)");
    }

    size_t pos{0};
    auto readLine = [&]() -> std::string_view {
        if (pos >= textView.size())
            return {};
        const size_t nl{textView.find('\n', pos)};
        if (nl == std::string_view::npos) {
            const std::string_view line{textView.substr(pos)};
            pos = textView.size();
            return line;
        }
        std::string_view line{textView.substr(pos, nl - pos)};
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        pos = nl + 1;
        return line;
    };

    // 1. Read header lines until empty line
    while (pos < textView.size()) {
        const std::string_view line{readLine()};
        if (line.empty())
            break;
    }

    // 2. Read resolution line: "-Y <height> +X <width>"
    const std::string_view resLine{readLine()};
    if (resLine.empty()) {
        return std::unexpected("Malformed Radiance .hdr file: missing resolution string");
    }

    uint32_t width{0};
    uint32_t height{0};
    const size_t yPos{resLine.find('Y')};
    const size_t xPos{resLine.find('X')};
    if (yPos == std::string_view::npos || xPos == std::string_view::npos) {
        return std::unexpected("Malformed Radiance .hdr resolution string");
    }

    try {
        const std::string ySub{resLine.substr(yPos + 1, xPos - (yPos + 1))};
        const std::string xSub{resLine.substr(xPos + 1)};
        height = static_cast<uint32_t>(std::stoul(ySub));
        width = static_cast<uint32_t>(std::stoul(xSub));
    } catch (...) {
        return std::unexpected("Failed to parse Radiance .hdr width/height");
    }

    if (width == 0 || height == 0 || width > kMaxTextureDimension || height > kMaxTextureDimension) {
        return std::unexpected("Radiance .hdr dimensions out of valid range");
    }

    DecodedHdrImage hdr{};
    hdr.width = width;
    hdr.height = height;
    hdr.rgbaFloat.resize(static_cast<size_t>(width) * height * 4u, 0.0f);

    auto rgbeToFloat = [](uint8_t r, uint8_t g, uint8_t b, uint8_t e, float* outRgba) noexcept {
        if (e == 0) {
            outRgba[0] = 0.0f;
            outRgba[1] = 0.0f;
            outRgba[2] = 0.0f;
            outRgba[3] = 1.0f;
        } else {
            const float scale{std::ldexp(1.0f, static_cast<int>(e) - (128 + 8))};
            outRgba[0] = (static_cast<float>(r) + 0.5f) * scale;
            outRgba[1] = (static_cast<float>(g) + 0.5f) * scale;
            outRgba[2] = (static_cast<float>(b) + 0.5f) * scale;
            outRgba[3] = 1.0f;
        }
    };

    std::vector<uint8_t> scanline(static_cast<size_t>(width) * 4u);

    for (uint32_t y{0}; y < height; ++y) {
        if (pos + 4 > data.size()) {
            return std::unexpected("Unexpected EOF while reading .hdr scanline");
        }

        const uint8_t b0{data[pos]};
        const uint8_t b1{data[pos + 1]};
        const uint8_t b2{data[pos + 2]};
        const uint8_t b3{data[pos + 3]};

        // Adaptive RLE scanline check
        if (b0 == 2 && b1 == 2 && (b2 & 0x80u) == 0 && width >= 8 && width <= 0x7FFFu) {
            const uint32_t scanW{(static_cast<uint32_t>(b2) << 8) | static_cast<uint32_t>(b3)};
            if (scanW != width) {
                return std::unexpected("Radiance .hdr RLE scanline width mismatch");
            }
            pos += 4;
            for (uint32_t ch{0}; ch < 4; ++ch) {
                uint32_t x{0};
                while (x < width) {
                    if (pos >= data.size()) {
                        return std::unexpected("Unexpected EOF in .hdr RLE channel stream");
                    }
                    const uint8_t code{data[pos++]};
                    if (code > 128) {
                        const uint32_t runCount{static_cast<uint32_t>(code - 128)};
                        if (pos >= data.size() || x + runCount > width) {
                            return std::unexpected("Corrupt RLE run in .hdr scanline");
                        }
                        const uint8_t val{data[pos++]};
                        for (uint32_t i{0}; i < runCount; ++i) {
                            scanline[(x + i) * 4u + ch] = val;
                        }
                        x += runCount;
                    } else {
                        const uint32_t litCount{static_cast<uint32_t>(code)};
                        if (litCount == 0 || pos + litCount > data.size() || x + litCount > width) {
                            return std::unexpected("Corrupt literal run in .hdr scanline");
                        }
                        for (uint32_t i{0}; i < litCount; ++i) {
                            scanline[(x + i) * 4u + ch] = data[pos++];
                        }
                        x += litCount;
                    }
                }
            }
        } else {
            // Uncompressed RGBE scanline
            const size_t rowBytes{static_cast<size_t>(width) * 4u};
            if (pos + rowBytes > data.size()) {
                return std::unexpected("Unexpected EOF in uncompressed .hdr scanline");
            }
            std::memcpy(scanline.data(), data.data() + pos, rowBytes);
            pos += rowBytes;
        }

        float* dstRow{&hdr.rgbaFloat[static_cast<size_t>(y) * width * 4u]};
        for (uint32_t x{0}; x < width; ++x) {
            rgbeToFloat(scanline[x * 4u + 0], scanline[x * 4u + 1], scanline[x * 4u + 2], scanline[x * 4u + 3],
                        &dstRow[x * 4u]);
        }
    }

    return hdr;
}

core::Result<DecodedHdrImage> TextureDecoder::DecodeEXR(std::span<const uint8_t> data) {
    // OpenEXR magic: 0x76, 0x2F, 0x31, 0x01
    static constexpr std::array<uint8_t, 4> kExrMagic{0x76, 0x2F, 0x31, 0x01};
    if (data.size() < 32 || std::memcmp(data.data(), kExrMagic.data(), kExrMagic.size()) != 0) {
        return std::unexpected("Data is not a valid OpenEXR (.exr) stream");
    }

    size_t pos{8}; // Skip 4-byte magic + 4-byte version field
    auto readU32 = [&](size_t offset) noexcept -> uint32_t {
        uint32_t v{0};
        std::memcpy(&v, data.data() + offset, sizeof(uint32_t));
        return v;
    };
    auto readI32 = [&](size_t offset) noexcept -> int32_t {
        int32_t v{0};
        std::memcpy(&v, data.data() + offset, sizeof(int32_t));
        return v;
    };
    auto readU64 = [&](size_t offset) noexcept -> uint64_t {
        uint64_t v{0};
        std::memcpy(&v, data.data() + offset, sizeof(uint64_t));
        return v;
    };
    auto readCString = [&](size_t& cur, size_t limit) -> std::string_view {
        const size_t start{cur};
        while (cur < limit && data[cur] != 0) {
            ++cur;
        }
        if (cur >= limit)
            return {};
        const std::string_view s{reinterpret_cast<const char*>(data.data() + start), cur - start};
        ++cur; // Skip null terminator
        return s;
    };

    struct ExrChannel {
        std::string name{};
        int32_t pixelType{1}; // 0=UINT, 1=HALF, 2=FLOAT
        size_t byteSize{2};
        int rgbaComp{-1}; // 0=R, 1=G, 2=B, 3=A
    };

    std::vector<ExrChannel> channels{};
    uint8_t compression{0};
    int32_t minX{0}, minY{0}, maxX{-1}, maxY{-1};

    // Parse OpenEXR header attributes until null byte
    while (pos < data.size()) {
        if (data[pos] == 0) {
            ++pos;
            break;
        }
        const std::string_view attrName{readCString(pos, data.size())};
        const std::string_view attrType{readCString(pos, data.size())};
        if (pos + 4 > data.size()) {
            return std::unexpected("Truncated OpenEXR attribute header");
        }
        const uint32_t attrSize{readU32(pos)};
        pos += 4;
        if (pos + attrSize > data.size()) {
            return std::unexpected("Truncated OpenEXR attribute payload");
        }

        const size_t attrEnd{pos + attrSize};
        if (attrName == "channels" && attrType == "chlist") {
            size_t chPos{pos};
            while (chPos < attrEnd && data[chPos] != 0) {
                const std::string_view chName{readCString(chPos, attrEnd)};
                if (chPos + 16 > attrEnd)
                    break;
                const int32_t pType{readI32(chPos)};
                chPos += 16; // pixelType(4) + pLinear(1) + reserved(3) + xSampling(4) + ySampling(4)

                ExrChannel ch{};
                ch.name = std::string{chName};
                ch.pixelType = pType;
                ch.byteSize = (pType == 1) ? 2u : 4u;
                if (chName == "R" || chName == "r")
                    ch.rgbaComp = 0;
                else if (chName == "G" || chName == "g")
                    ch.rgbaComp = 1;
                else if (chName == "B" || chName == "b")
                    ch.rgbaComp = 2;
                else if (chName == "A" || chName == "a")
                    ch.rgbaComp = 3;
                channels.push_back(std::move(ch));
            }
        } else if (attrName == "compression" && attrSize >= 1) {
            compression = data[pos];
        } else if (attrName == "dataWindow" && attrSize >= 16) {
            minX = readI32(pos + 0);
            minY = readI32(pos + 4);
            maxX = readI32(pos + 8);
            maxY = readI32(pos + 12);
        }
        pos = attrEnd;
    }

    if (maxX < minX || maxY < minY || channels.empty()) {
        return std::unexpected("Invalid OpenEXR dataWindow or channel list");
    }
    if (compression != 0) {
        return std::unexpected("OpenEXR compression mode " + std::to_string(compression) +
                               " not supported (use uncompressed EXR or Radiance .hdr)");
    }

    const uint32_t width{static_cast<uint32_t>(maxX - minX + 1)};
    const uint32_t height{static_cast<uint32_t>(maxY - minY + 1)};
    if (width == 0 || height == 0 || width > kMaxTextureDimension || height > kMaxTextureDimension) {
        return std::unexpected("OpenEXR dimensions exceed limits");
    }

    // Skip scanline offset table (height * uint64_t for NO_COMPRESSION)
    if (pos + static_cast<size_t>(height) * sizeof(uint64_t) > data.size()) {
        return std::unexpected("Truncated OpenEXR scanline offset table");
    }

    DecodedHdrImage hdr{};
    hdr.width = width;
    hdr.height = height;
    hdr.rgbaFloat.resize(static_cast<size_t>(width) * height * 4u, 0.0f);
    for (size_t i{0}; i < static_cast<size_t>(width) * height; ++i) {
        hdr.rgbaFloat[i * 4u + 3u] = 1.0f;
    }

    for (uint32_t lineIdx{0}; lineIdx < height; ++lineIdx) {
        const uint64_t chunkOffset{readU64(pos + static_cast<size_t>(lineIdx) * sizeof(uint64_t))};
        if (chunkOffset + 8 > data.size()) {
            return std::unexpected("OpenEXR scanline chunk offset out of bounds");
        }
        const int32_t yCoord{readI32(chunkOffset)};
        const uint32_t dataSize{readU32(chunkOffset + 4)};
        const int32_t relY{yCoord - minY};
        if (relY < 0 || static_cast<uint32_t>(relY) >= height || chunkOffset + 8 + dataSize > data.size()) {
            return std::unexpected("OpenEXR scanline payload out of bounds");
        }

        size_t cPtr{chunkOffset + 8};
        float* dstRow{&hdr.rgbaFloat[static_cast<size_t>(relY) * width * 4u]};
        for (const auto& ch : channels) {
            const size_t chRowBytes{static_cast<size_t>(width) * ch.byteSize};
            if (cPtr + chRowBytes > chunkOffset + 8 + dataSize)
                break;
            if (ch.rgbaComp >= 0 && ch.rgbaComp < 4) {
                for (uint32_t x{0}; x < width; ++x) {
                    float val{0.0f};
                    if (ch.pixelType == 1) {
                        uint16_t h{0};
                        std::memcpy(&h, data.data() + cPtr + x * 2u, sizeof(uint16_t));
                        val = HalfToFloat(h);
                    } else if (ch.pixelType == 2) {
                        std::memcpy(&val, data.data() + cPtr + x * 4u, sizeof(float));
                    }
                    dstRow[x * 4u + static_cast<uint32_t>(ch.rgbaComp)] = val;
                }
            }
            cPtr += chRowBytes;
        }
    }

    return hdr;
}

core::Result<DecodedHdrImage> TextureDecoder::DecodeHdrAuto(std::span<const uint8_t> data,
                                                            std::string_view mimeOrPath) {
    if (data.empty()) {
        return std::unexpected("Cannot decode empty HDR image buffer");
    }
    static constexpr std::array<uint8_t, 4> kExrMagic{0x76, 0x2F, 0x31, 0x01};
    if (data.size() >= 4 && std::memcmp(data.data(), kExrMagic.data(), 4) == 0) {
        return DecodeEXR(data);
    }
    const std::string_view textPrefix{reinterpret_cast<const char*>(data.data()), std::min<size_t>(data.size(), 16)};
    if (textPrefix.starts_with("#?RADIANCE") || textPrefix.starts_with("#?RGBE") || textPrefix.starts_with("#?")) {
        return DecodeHDR(data);
    }
    if (mimeOrPath.ends_with(".exr") || mimeOrPath.ends_with(".EXR")) {
        return DecodeEXR(data);
    }
    return DecodeHDR(data);
}

core::Result<DecodedImage> TextureDecoder::DecodeAuto(std::span<const uint8_t> data, std::string_view mimeOrPath) {
    if (data.empty()) {
        return std::unexpected("Cannot decode empty image buffer");
    }

    // 1. Content-based magic byte detection
    // PNG magic: 0x89 'P' 'N' 'G' 0x0D 0x0A 0x1A 0x0A
    static constexpr std::array<uint8_t, 8> kPngMagic{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (data.size() >= kPngMagic.size() && std::memcmp(data.data(), kPngMagic.data(), kPngMagic.size()) == 0) {
        return DecodePNG(data);
    }
    // JPEG magic: 0xFF 0xD8 0xFF
    if (data.size() >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return DecodeJPEG(data);
    }
    // WebP magic: 'RIFF' .... 'WEBP'
    if (data.size() >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' && data[8] == 'W' &&
        data[9] == 'E' && data[10] == 'B' && data[11] == 'P') {
        return DecodeWebP(data);
    }
    // KTX2 magic: 0xAB 'K' 'T' 'X' ' ' '2' '0' 0xBB 0x0D 0x0A 0x1A 0x0A
    if (data.size() >= 12 && data[0] == 0xAB && data[1] == 'K' && data[2] == 'T' && data[3] == 'X') {
        return DecodeKTX2(data);
    }

    // 2. MIME type or file extension hints
    if (mimeOrPath.ends_with(".png") || mimeOrPath.ends_with(".PNG") || mimeOrPath == "image/png") {
        return DecodePNG(data);
    }
    if (mimeOrPath.ends_with(".jpg") || mimeOrPath.ends_with(".jpeg") || mimeOrPath.ends_with(".JPG") ||
        mimeOrPath.ends_with(".JPEG") || mimeOrPath == "image/jpeg") {
        return DecodeJPEG(data);
    }
    if (mimeOrPath.ends_with(".webp") || mimeOrPath.ends_with(".WEBP") || mimeOrPath == "image/webp") {
        return DecodeWebP(data);
    }
    if (mimeOrPath.ends_with(".ktx2") || mimeOrPath.ends_with(".ktx") || mimeOrPath.ends_with(".KTX2") ||
        mimeOrPath.ends_with(".KTX") || mimeOrPath == "image/ktx2" || mimeOrPath == "image/ktx") {
        return DecodeKTX2(data);
    }

    // 3. Fallback attempts
    auto jpegRes = DecodeJPEG(data);
    if (jpegRes)
        return jpegRes;

    return DecodeWebP(data);
}

std::vector<std::string_view> TextureDecoder::GetSupportedFormats() {
    std::vector<std::string_view> formats;
    if constexpr (IsPngSupported())
        formats.push_back("PNG");
    if constexpr (IsJpegSupported())
        formats.push_back("JPEG");
    if constexpr (IsWebpSupported())
        formats.push_back("WebP");
    if constexpr (IsKtxSupported())
        formats.push_back("KTX2 (BasisU -> BC7/ASTC/ETC2/RGBA32)");
    if constexpr (IsHdrSupported())
        formats.push_back("HDR (Radiance RGBE)");
    if constexpr (IsExrSupported())
        formats.push_back("EXR (OpenEXR FP16/FP32)");
    return formats;
}

} // namespace bgl::scene
