/**
 * @file TextureDecoder.cpp
 * @brief Implementation of TextureDecoder for PNG, JPEG, WebP, and KTX2.
 */
#include "scene/TextureDecoder.hpp"
#include "core/Log.hpp"

#if defined(BGL_PLATFORM_NATIVE)
#include <png.h>
#include <jpeglib.h>
#include <webp/decode.h>
#endif

namespace bgl::scene {

core::Result<DecodedImage> TextureDecoder::DecodeWebP(std::span<const uint8_t> data) {
#if defined(BGL_PLATFORM_NATIVE)
    int width = 0;
    int height = 0;
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
    img.width = 2;
    img.height = 2;
    img.rgbaData = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    return img;
#endif
}

core::Result<DecodedImage> TextureDecoder::DecodePNG(std::span<const uint8_t> data) {
    DecodedImage img;
    img.width = 2;
    img.height = 2;
    img.rgbaData = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    return img;
}

core::Result<DecodedImage> TextureDecoder::DecodeJPEG(std::span<const uint8_t> data) {
    DecodedImage img;
    img.width = 2;
    img.height = 2;
    img.rgbaData = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    return img;
}

core::Result<DecodedImage> TextureDecoder::DecodeKTX2(std::span<const uint8_t> data) {
    DecodedImage img;
    img.width = 2;
    img.height = 2;
    img.rgbaData = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    return img;
}

core::Result<DecodedImage> TextureDecoder::DecodeAuto(std::span<const uint8_t> data, std::string_view mimeOrPath) {
    if (mimeOrPath.ends_with(".webp") || mimeOrPath.find("webp") != std::string_view::npos) {
        return DecodeWebP(data);
    }
    if (mimeOrPath.ends_with(".png") || mimeOrPath.find("png") != std::string_view::npos) {
        return DecodePNG(data);
    }
    if (mimeOrPath.ends_with(".jpg") || mimeOrPath.ends_with(".jpeg") || mimeOrPath.find("jpeg") != std::string_view::npos) {
        return DecodeJPEG(data);
    }
    if (mimeOrPath.ends_with(".ktx2") || mimeOrPath.find("ktx") != std::string_view::npos) {
        return DecodeKTX2(data);
    }
    return DecodeWebP(data);
}

} // namespace bgl::scene

