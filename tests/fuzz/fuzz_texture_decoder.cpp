/**
 * @file fuzz_texture_decoder.cpp
 * @brief libFuzzer target for robust parsing of corrupted / adversarial texture files.
 */
#include "scene/TextureDecoder.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0)
        return 0;
    std::span<const uint8_t> span(data, size);

    constexpr std::array<const char*, 6> kImageHints{
        "fuzz.png", "fuzz.jpg", "fuzz.webp", "fuzz.ktx2", "image/ktx2", "fuzz_unknown",
    };
    constexpr std::array<bgl::scene::KtxTranscodeTarget, 4> kKtxTargets{
        bgl::scene::KtxTranscodeTarget::Rgba32,
        bgl::scene::KtxTranscodeTarget::Bc7,
        bgl::scene::KtxTranscodeTarget::Astc4x4,
        bgl::scene::KtxTranscodeTarget::Etc2Rgba,
    };

    const uint8_t selector{data[0]};
    const auto target = kKtxTargets[selector % kKtxTargets.size()];
    const char* autoHint = kImageHints[selector % kImageHints.size()];
    const char* hdrHint = (selector & 1u) != 0u ? "fuzz.exr" : "fuzz.hdr";

    std::array<std::byte, 4096> scratchBuf{};
    std::pmr::monotonic_buffer_resource scratch{scratchBuf.data(), scratchBuf.size()};

    (void)bgl::scene::TextureDecoder::DecodeAuto(span, autoHint);
    (void)bgl::scene::TextureDecoder::DecodePNG(span);
    (void)bgl::scene::TextureDecoder::DecodeJPEG(span);
    (void)bgl::scene::TextureDecoder::DecodeWebP(span);
    (void)bgl::scene::TextureDecoder::DecodeKTX2(span, target);
    (void)bgl::scene::TextureDecoder::DecodeKTX2(span, bgl::scene::KtxTranscodeTarget::Rgba32, &scratch);
    (void)bgl::scene::TextureDecoder::DecodeHdrAuto(span, hdrHint);
    (void)bgl::scene::TextureDecoder::DecodeHDR(span);
    (void)bgl::scene::TextureDecoder::DecodeEXR(span);

    return 0;
}
