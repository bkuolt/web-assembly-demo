/**
 * @file TextureDecoder.hpp
 * @brief Memory decoding for PNG, JPEG, WebP, KTX2 (RGBA32/BC7/ASTC/ETC2), Radiance HDR (.hdr), and OpenEXR (.exr) textures.
 */
#pragma once

#include <vector>
#include <cstdint>
#include <span>
#include <string_view>
#include "core/Types.hpp"

namespace bgl::scene {

/**
 * @enum KtxTranscodeTarget
 * @brief Target GPU texture format for KTX2 Basis Universal (ETC1S / UASTC) transcoding.
 */
enum class KtxTranscodeTarget : uint32_t {
    Rgba32   = 0, ///< Uncompressed RGBA8 (32 bits/pixel)
    Bc7      = 1, ///< Desktop BC7 RGBA block-compressed (8 bits/pixel, 4x4 blocks, 16 bytes/block)
    Astc4x4  = 2, ///< Mobile/Modern ASTC 4x4 RGBA block-compressed (8 bits/pixel, 4x4 blocks, 16 bytes/block)
    Etc2Rgba = 3  ///< Mobile/WebGL2 ETC2 EAC RGBA8 block-compressed (8 bits/pixel, 4x4 blocks, 16 bytes/block)
};

/**
 * @struct DecodedMipLevel
 * @brief Single mipmap level payload for block-compressed or pre-filtered KTX2 textures.
 */
struct DecodedMipLevel {
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> data{};
};

/**
 * @struct DecodedImage
 * @brief Decoded 2D texture (either uncompressed RGBA8 or native GPU block-compressed BC7/ASTC/ETC2 mip chain).
 */
struct DecodedImage {
    uint32_t width{0};
    uint32_t height{0};
    uint32_t channels{4};
    std::vector<uint8_t> rgbaData{};
    bool isCompressed{false};
    KtxTranscodeTarget compressionFormat{KtxTranscodeTarget::Rgba32};
    std::vector<DecodedMipLevel> mipLevels{};
};

/**
 * @struct DecodedHdrImage
 * @brief High Dynamic Range linear floating-point RGBA texture (4 floats per pixel) decoded from .hdr or .exr.
 */
struct DecodedHdrImage {
    uint32_t width{0};
    uint32_t height{0};
    std::vector<float> rgbaFloat{};

    [[nodiscard]] bool IsValid() const noexcept {
        return width > 0 && height > 0 && rgbaFloat.size() >= static_cast<size_t>(width) * height * 4;
    }
};

/**
 * @class TextureDecoder
 * @brief Modular multi-format texture decoder supporting LDR, GPU block-compressed KTX2, and HDR (.hdr/.exr) formats.
 */
class TextureDecoder {
public:
    static core::Result<DecodedImage> DecodePNG(std::span<const uint8_t> data);
    static core::Result<DecodedImage> DecodeJPEG(std::span<const uint8_t> data);
    static core::Result<DecodedImage> DecodeWebP(std::span<const uint8_t> data);
    static core::Result<DecodedImage> DecodeKTX2(std::span<const uint8_t> data, KtxTranscodeTarget target);
    static core::Result<DecodedImage> DecodeKTX2(std::span<const uint8_t> data);
    static core::Result<DecodedImage> DecodeAuto(std::span<const uint8_t> data, std::string_view mimeOrPath);

    static core::Result<DecodedHdrImage> DecodeHDR(std::span<const uint8_t> data);
    static core::Result<DecodedHdrImage> DecodeEXR(std::span<const uint8_t> data);
    static core::Result<DecodedHdrImage> DecodeHdrAuto(std::span<const uint8_t> data, std::string_view mimeOrPath = {});

    static void SetDefaultKtxTarget(KtxTranscodeTarget target) noexcept;
    [[nodiscard]] static KtxTranscodeTarget GetDefaultKtxTarget() noexcept;
    [[nodiscard]] static constexpr std::string_view GetKtxTargetName(KtxTranscodeTarget target) noexcept {
        switch (target) {
            case KtxTranscodeTarget::Bc7:      return "BC7_RGBA";
            case KtxTranscodeTarget::Astc4x4:  return "ASTC_4x4_RGBA";
            case KtxTranscodeTarget::Etc2Rgba: return "ETC2_RGBA8";
            case KtxTranscodeTarget::Rgba32:
            default:                           return "RGBA32";
        }
    }

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

    static constexpr bool IsHdrSupported() noexcept { return true; }
    static constexpr bool IsExrSupported() noexcept { return true; }

    static std::vector<std::string_view> GetSupportedFormats();
};

} // namespace bgl::scene

