/**
 * @file fuzz_texture_decoder.cpp
 * @brief libFuzzer target for robust parsing of corrupted / adversarial texture files.
 */
#include "scene/TextureDecoder.hpp"
#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0)
        return 0;
    std::span<const uint8_t> span(data, size);

    // Fuzz all decoders safely
    (void)bgl::scene::TextureDecoder::DecodeAuto(span, "fuzz_input");
    (void)bgl::scene::TextureDecoder::DecodePNG(span);
    (void)bgl::scene::TextureDecoder::DecodeJPEG(span);
    (void)bgl::scene::TextureDecoder::DecodeWebP(span);
    (void)bgl::scene::TextureDecoder::DecodeKTX2(span);

    return 0;
}
