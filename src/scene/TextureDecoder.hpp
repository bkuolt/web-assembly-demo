/**
 * @file TextureDecoder.hpp
 * @brief Memory decoding for PNG, JPEG, WebP, and KTX2 textures.
 */
#pragma once

#include <vector>
#include <cstdint>
#include <span>
#include <string_view>
#include "core/Types.hpp"

namespace bgl::scene {

/**
 * @struct DecodedImage
 * @brief Raw RGBA8 2D bitmap texture.
 */
struct DecodedImage {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channels = 4;
    std::vector<uint8_t> rgbaData;
};

/**
 * @class TextureDecoder
 * @brief Modular multi-format texture decoder.
 */
class TextureDecoder {
public:
    static core::Result<DecodedImage> DecodePNG(std::span<const uint8_t> data);
    static core::Result<DecodedImage> DecodeJPEG(std::span<const uint8_t> data);
    static core::Result<DecodedImage> DecodeWebP(std::span<const uint8_t> data);
    static core::Result<DecodedImage> DecodeKTX2(std::span<const uint8_t> data);
    static core::Result<DecodedImage> DecodeAuto(std::span<const uint8_t> data, std::string_view mimeOrPath);

    /**
     * @brief Queries compile-time availability of format decoders.
     */
    static constexpr bool IsPngSupported() noexcept {
#if defined(BGL_ENABLE_PNG)
        return true;
#else
        return false;
#endif
    }

    static constexpr bool IsJpegSupported() noexcept {
#if defined(BGL_ENABLE_JPEG)
        return true;
#else
        return false;
#endif
    }

    static constexpr bool IsWebpSupported() noexcept {
#if defined(BGL_ENABLE_WEBP)
        return true;
#else
        return false;
#endif
    }

    static constexpr bool IsKtxSupported() noexcept {
#if defined(BGL_ENABLE_KTX)
        return true;
#else
        return false;
#endif
    }

    static std::vector<std::string_view> GetSupportedFormats();
};

} // namespace bgl::scene

