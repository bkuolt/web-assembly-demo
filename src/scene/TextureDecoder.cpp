/**
 * @file TextureDecoder.cpp
 * @brief Memory decoding for PNG, JPEG, and WebP using libpng, libjpeg-turbo, and libwebp.
 */
#include "scene/TextureDecoder.hpp"
#include "core/Log.hpp"
#include <cstring>

#if defined(BGL_PLATFORM_NATIVE)
#include <png.h>
#include <jpeglib.h>
#include <webp/decode.h>

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

namespace bgl::scene {

core::Result<DecodedImage> TextureDecoder::DecodePNG(std::span<const uint8_t> data) {
#if defined(BGL_PLATFORM_NATIVE)
    if (data.size() < 8 || !png_check_sig(data.data(), 8)) {
        return std::unexpected("Data is not a valid PNG signature");
    }

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr) return std::unexpected("Failed to create png read struct");

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_read_struct(&png_ptr, nullptr, nullptr);
        return std::unexpected("Failed to create png info struct");
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        return std::unexpected("Error occurred during PNG decoding");
    }

    PngMemoryReader reader{data.data(), data.size(), 8};
    png_set_read_fn(png_ptr, &reader, PngReadCallback);
    png_set_sig_bytes(png_ptr, 8);

    png_read_info(png_ptr, info_ptr);

    png_uint_32 width = 0, height = 0;
    int bit_depth = 0, color_type = 0;
    png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth, &color_type, nullptr, nullptr, nullptr);

    if (bit_depth == 16) png_set_strip_16(png_ptr);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png_ptr);
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png_ptr);
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY) png_set_add_alpha(png_ptr, 0xFF, PNG_FILLER_AFTER);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png_ptr);

    png_read_update_info(png_ptr, info_ptr);

    DecodedImage img;
    img.width = width;
    img.height = height;
    img.channels = 4;
    img.rgbaData.resize(width * height * 4);

    std::vector<png_bytep> row_pointers(height);
    for (size_t y = 0; y < height; ++y) {
        row_pointers[y] = &img.rgbaData[y * width * 4];
    }

    png_read_image(png_ptr, row_pointers.data());
    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);

    return img;
#else
    DecodedImage img;
    img.width = 2; img.height = 2;
    img.rgbaData = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    return img;
#endif
}

core::Result<DecodedImage> TextureDecoder::DecodeWebP(std::span<const uint8_t> data) {
#if defined(BGL_PLATFORM_NATIVE)
    int width = 0, height = 0;
    uint8_t* decoded = WebPDecodeRGBA(data.data(), data.size(), &width, &height);
    if (!decoded) {
        return std::unexpected("Failed to decode WebP stream");
    }
    DecodedImage img;
    img.width = static_cast<uint32_t>(width);
    img.height = static_cast<uint32_t>(height);
    img.channels = 4;
    img.rgbaData.assign(decoded, decoded + (width * height * 4));
    WebPFree(decoded);
    return img;
#else
    DecodedImage img;
    img.width = 2; img.height = 2;
    img.rgbaData = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    return img;
#endif
}

core::Result<DecodedImage> TextureDecoder::DecodeJPEG(std::span<const uint8_t> data) {
#if defined(BGL_PLATFORM_NATIVE)
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, const_cast<unsigned char*>(data.data()), data.size());

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return std::unexpected("Failed to read JPEG header");
    }

    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    DecodedImage img;
    img.width = cinfo.output_width;
    img.height = cinfo.output_height;
    img.channels = 4;
    img.rgbaData.resize(img.width * img.height * 4);

    std::vector<uint8_t> rowBuffer(img.width * 3);
    JSAMPROW rowPtr = rowBuffer.data();

    size_t y = 0;
    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, &rowPtr, 1);
        for (size_t x = 0; x < img.width; ++x) {
            img.rgbaData[(y * img.width + x) * 4 + 0] = rowBuffer[x * 3 + 0];
            img.rgbaData[(y * img.width + x) * 4 + 1] = rowBuffer[x * 3 + 1];
            img.rgbaData[(y * img.width + x) * 4 + 2] = rowBuffer[x * 3 + 2];
            img.rgbaData[(y * img.width + x) * 4 + 3] = 255;
        }
        y++;
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return img;
#else
    DecodedImage img;
    img.width = 2; img.height = 2;
    img.rgbaData = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    return img;
#endif
}

core::Result<DecodedImage> TextureDecoder::DecodeKTX2(std::span<const uint8_t> data) {
    DecodedImage img;
    img.width = 2; img.height = 2;
    img.rgbaData = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    return img;
}

core::Result<DecodedImage> TextureDecoder::DecodeAuto(std::span<const uint8_t> data, std::string_view mimeOrPath) {
    if (data.empty()) {
        return std::unexpected("Cannot decode empty image buffer");
    }

    // 1. Content-based magic byte detection (uniquely robust regardless of glTF naming)
    // PNG magic: 0x89 'P' 'N' 'G' 0x0D 0x0A 0x1A 0x0A
    if (data.size() >= 8 && png_check_sig(data.data(), 8)) {
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
    if (mimeOrPath.ends_with(".png") || mimeOrPath.find("png") != std::string_view::npos) {
        return DecodePNG(data);
    }
    if (mimeOrPath.ends_with(".jpg") || mimeOrPath.ends_with(".jpeg") || mimeOrPath.find("jpeg") != std::string_view::npos) {
        return DecodeJPEG(data);
    }
    if (mimeOrPath.ends_with(".webp") || mimeOrPath.find("webp") != std::string_view::npos) {
        return DecodeWebP(data);
    }
    if (mimeOrPath.ends_with(".ktx2") || mimeOrPath.find("ktx") != std::string_view::npos) {
        return DecodeKTX2(data);
    }

    // 3. Fallback attempts: try JPEG first, then WebP
    auto jpegRes = DecodeJPEG(data);
    if (jpegRes) return jpegRes;

    return DecodeWebP(data);
}

} // namespace bgl::scene
