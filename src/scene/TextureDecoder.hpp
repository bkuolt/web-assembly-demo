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
    /**
     * @brief Decodes an in-memory PNG image byte buffer into uncompressed 32-bit RGBA pixels.
     * @param data Byte span containing the PNG file data.
     * @return DecodedImage structure on success, or an error string.
     */
    static core::Result<DecodedImage> DecodePNG(std::span<const uint8_t> data);

    /**
     * @brief Decodes an in-memory JPEG image byte buffer into uncompressed 32-bit RGBA pixels.
     * @param data Byte span containing the JPEG file data.
     * @return DecodedImage structure on success, or an error string.
     */
    static core::Result<DecodedImage> DecodeJPEG(std::span<const uint8_t> data);

    /**
     * @brief Decodes an in-memory WebP image byte buffer into uncompressed 32-bit RGBA pixels.
     * @param data Byte span containing the WebP file data.
     * @return DecodedImage structure on success, or an error string.
     */
    static core::Result<DecodedImage> DecodeWebP(std::span<const uint8_t> data);

    /**
     * @brief Transcodes KTX2 Basis Universal texture data to a specified target GPU compression format.
     * @param data Byte span containing the KTX2 file data.
     * @param target Desired transcoding target (RGBA32, BC7, ASTC, or ETC2).
     * @return DecodedImage structure with transcoded mip chain on success, or an error string.
     */
    static core::Result<DecodedImage> DecodeKTX2(std::span<const uint8_t> data, KtxTranscodeTarget target);

    /**
     * @brief Transcodes KTX2 Basis Universal texture data to the active default GPU compression format.
     * @param data Byte span containing the KTX2 file data.
     * @return DecodedImage structure on success, or an error string.
     */
    static core::Result<DecodedImage> DecodeKTX2(std::span<const uint8_t> data);

    /**
     * @brief Automatically detects format by magic bytes/MIME/extension and decodes LDR or KTX2 texture data.
     * @param data Byte span containing the image file data.
     * @param mimeOrPath Filename, path, or MIME type hint for format resolution.
     * @return DecodedImage structure on success, or an error string.
     */
    static core::Result<DecodedImage> DecodeAuto(std::span<const uint8_t> data, std::string_view mimeOrPath);

    /**
     * @brief Decodes Radiance HDR (.hdr / RGBE) linear floating-point environment map data.
     * @param data Byte span containing raw .hdr file bytes.
     * @return DecodedHdrImage structure with linear 32-bit float RGBA data on success, or error string.
     */
    static core::Result<DecodedHdrImage> DecodeHDR(std::span<const uint8_t> data);

    /**
     * @brief Decodes an uncompressed or scanline OpenEXR (.exr) linear floating-point image.
     * @param data Byte span containing raw .exr file bytes.
     * @return DecodedHdrImage structure on success, or an error string.
     */
    static core::Result<DecodedHdrImage> DecodeEXR(std::span<const uint8_t> data);

    /**
     * @brief Automatically detects format and decodes an HDR image (.hdr or .exr).
     * @param data Byte span containing raw HDR/EXR file bytes.
     * @param mimeOrPath Filename, path, or MIME type hint.
     * @return DecodedHdrImage on success, or an error string.
     */
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

