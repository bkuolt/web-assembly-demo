/**
 * @file TextureDecoder.cpp
 * @brief Memory decoding for PNG, JPEG, WebP, and KTX2 textures using modular decoders.
 */
#include "scene/TextureDecoder.hpp"
#include "core/Log.hpp"
#include <array>
#include <cstring>
#include <memory>

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
#include <jpeglib.h>
#include <csetjmp>

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
static bool ReadPngStreamIsolateSetjmp(
    png_structp png_ptr,
    png_infop info_ptr,
    std::span<const uint8_t> data,
    DecodedImage& outImg,
    std::vector<png_bytep>& rowPointers) {

    if (setjmp(png_jmpbuf(png_ptr))) {
        return false;
    }

    png_set_user_limits(png_ptr, static_cast<png_uint_32>(kMaxTextureDimension), static_cast<png_uint_32>(kMaxTextureDimension));

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
    } else if (color_type == PNG_COLOR_TYPE_RGB ||
               color_type == PNG_COLOR_TYPE_GRAY ||
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
static bool ReadJpegStreamIsolateSetjmp(
    std::span<const uint8_t> data,
    DecodedImage& outImg,
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

    if (cinfo.output_width == 0 || cinfo.output_height == 0 ||
        cinfo.output_width > kMaxTextureDimension || cinfo.output_height > kMaxTextureDimension) {
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
    if (!png_ptr) return std::unexpected("Failed to create png read struct");

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
    img.width = 2; img.height = 2;
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
    std::unique_ptr<uint8_t, decltype(&WebPFree)> decoded{
        WebPDecodeRGBA(data.data(), data.size(), &width, &height),
        &WebPFree
    };
    if (!decoded) {
        return std::unexpected("Failed to decode WebP stream");
    }
    if (width <= 0 || height <= 0 ||
        static_cast<size_t>(width) > kMaxTextureDimension ||
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
    img.width = 2; img.height = 2;
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
    img.width = 2; img.height = 2;
    img.rgbaData = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    return img;
#endif
}

core::Result<DecodedImage> TextureDecoder::DecodeKTX2(std::span<const uint8_t> data) {
#if defined(BGL_PLATFORM_NATIVE) && defined(BGL_ENABLE_KTX)
    if (data.size() < 12) {
        return std::unexpected("Data too small to be a valid KTX texture");
    }

    ktxTexture* rawKtx{nullptr};
    KTX_error_code result{ktxTexture_CreateFromMemory(
        data.data(),
        data.size(),
        KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
        &rawKtx
    )};

    if (result != KTX_SUCCESS || !rawKtx) {
        return std::unexpected(std::string("ktxTexture_CreateFromMemory failed: error ") + std::to_string(result));
    }
    const auto ktxDeleter = [](ktxTexture* t) noexcept {
        if (t) ktxTexture_Destroy(t);
    };
    std::unique_ptr<ktxTexture, decltype(ktxDeleter)> kTexture{rawKtx, ktxDeleter};

    // Check if transcoding is needed for Basis Universal KTX2 textures (UASTC / ETC1S)
    if (kTexture->classId == ktxTexture2_c) {
        auto* ktx2{reinterpret_cast<ktxTexture2*>(kTexture.get())};
        if (ktxTexture2_NeedsTranscoding(ktx2)) {
            result = ktxTexture2_TranscodeBasis(ktx2, KTX_TTF_RGBA32, 0);
            if (result != KTX_SUCCESS) {
                return std::unexpected(std::string("ktxTexture2_TranscodeBasis to RGBA32 failed: error ") + std::to_string(result));
            }
        }
    }

    const uint32_t width{kTexture->baseWidth};
    const uint32_t height{kTexture->baseHeight};
    const ktx_size_t imageSize{ktxTexture_GetImageSize(kTexture.get(), 0)};
    ktx_size_t level0Offset{0};
    ktxTexture_GetImageOffset(kTexture.get(), 0, 0, 0, &level0Offset);
    const ktx_uint8_t* baseDataPtr{ktxTexture_GetData(kTexture.get())};

    if (!baseDataPtr || imageSize == 0 || width == 0 || height == 0 ||
        width > kMaxTextureDimension || height > kMaxTextureDimension) {
        return std::unexpected("KTX texture contains no image data or invalid dimensions");
    }
    const ktx_uint8_t* imageBytes{baseDataPtr + level0Offset};

    DecodedImage img{};
    img.width = width;
    img.height = height;
    img.channels = 4;

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
    return std::unexpected("KTX/KTX2 texture decoder disabled at build time (-DBGL_ENABLE_KTX=OFF)");
#else
    (void)data;
    DecodedImage img;
    img.width = 2; img.height = 2;
    img.channels = 4;
    img.rgbaData = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    return img;
#endif
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
    if (data.size() >= 12 &&
        data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
        data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P') {
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
    if (mimeOrPath.ends_with(".jpg") || mimeOrPath.ends_with(".jpeg") ||
        mimeOrPath.ends_with(".JPG") || mimeOrPath.ends_with(".JPEG") || mimeOrPath == "image/jpeg") {
        return DecodeJPEG(data);
    }
    if (mimeOrPath.ends_with(".webp") || mimeOrPath.ends_with(".WEBP") || mimeOrPath == "image/webp") {
        return DecodeWebP(data);
    }
    if (mimeOrPath.ends_with(".ktx2") || mimeOrPath.ends_with(".ktx") ||
        mimeOrPath.ends_with(".KTX2") || mimeOrPath.ends_with(".KTX") ||
        mimeOrPath == "image/ktx2" || mimeOrPath == "image/ktx") {
        return DecodeKTX2(data);
    }

    // 3. Fallback attempts
    auto jpegRes = DecodeJPEG(data);
    if (jpegRes) return jpegRes;

    return DecodeWebP(data);
}

std::vector<std::string_view> TextureDecoder::GetSupportedFormats() {
    std::vector<std::string_view> formats;
    if constexpr (IsPngSupported()) formats.push_back("PNG");
    if constexpr (IsJpegSupported()) formats.push_back("JPEG");
    if constexpr (IsWebpSupported()) formats.push_back("WebP");
    if constexpr (IsKtxSupported()) formats.push_back("KTX2 (Basis Universal)");
    return formats;
}

} // namespace bgl::scene

