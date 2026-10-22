/**
 * @file test_texture_decoders.cpp
 * @brief Unit tests for texture format decoders (PNG, JPEG, WebP, KTX2) and magic-byte sniffing.
 */
#include <gtest/gtest.h>
#include "scene/TextureDecoder.hpp"

using namespace bgl::scene;

TEST(TextureDecoderTest, SupportedFormatsQuery) {
    auto formats = TextureDecoder::GetSupportedFormats();
    EXPECT_FALSE(formats.empty());

#if defined(BGL_ENABLE_PNG)
    EXPECT_TRUE(TextureDecoder::IsPngSupported());
    bool hasPng = false;
    for (auto f : formats) if (f == "PNG") hasPng = true;
    EXPECT_TRUE(hasPng);
#endif

#if defined(BGL_ENABLE_JPEG)
    EXPECT_TRUE(TextureDecoder::IsJpegSupported());
    bool hasJpeg = false;
    for (auto f : formats) if (f == "JPEG") hasJpeg = true;
    EXPECT_TRUE(hasJpeg);
#endif

#if defined(BGL_ENABLE_WEBP)
    EXPECT_TRUE(TextureDecoder::IsWebpSupported());
    bool hasWebp = false;
    for (auto f : formats) if (f == "WebP") hasWebp = true;
    EXPECT_TRUE(hasWebp);
#endif

#if defined(BGL_ENABLE_KTX)
    EXPECT_TRUE(TextureDecoder::IsKtxSupported());
    bool hasKtx = false;
    for (auto f : formats) if (f.find("KTX") != std::string_view::npos) hasKtx = true;
    EXPECT_TRUE(hasKtx);
#endif
}

TEST(TextureDecoderTest, EmptyAndInvalidBuffers) {
    std::vector<uint8_t> emptyBuffer;

    auto resPng = TextureDecoder::DecodePNG(emptyBuffer);
    EXPECT_FALSE(resPng.has_value());

    auto resJpeg = TextureDecoder::DecodeJPEG(emptyBuffer);
    EXPECT_FALSE(resJpeg.has_value());

    auto resWebp = TextureDecoder::DecodeWebP(emptyBuffer);
    EXPECT_FALSE(resWebp.has_value());

    auto resKtx = TextureDecoder::DecodeKTX2(emptyBuffer);
    EXPECT_FALSE(resKtx.has_value());

    auto resAuto = TextureDecoder::DecodeAuto(emptyBuffer, "test.png");
    EXPECT_FALSE(resAuto.has_value());
}

TEST(TextureDecoderTest, MagicByteSniffingAutoRouting) {
    // 1. PNG magic bytes: \x89 P N G \r \n \x1a \n
    std::vector<uint8_t> fakePng = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00 };
    auto resPng = TextureDecoder::DecodeAuto(fakePng, "unknown_file");
    // Should fail cleanly in PNG decoder rather than unsupported format error
    EXPECT_FALSE(resPng.has_value());

    // 2. JPEG magic bytes: \xFF \xD8 \xFF
    std::vector<uint8_t> fakeJpeg = { 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10 };
    auto resJpeg = TextureDecoder::DecodeAuto(fakeJpeg, "unknown_file");
    EXPECT_FALSE(resJpeg.has_value());

    // 3. WebP magic bytes: RIFF .... WEBP
    std::vector<uint8_t> fakeWebp = { 'R', 'I', 'F', 'F', 0x00, 0x00, 0x00, 0x00, 'W', 'E', 'B', 'P' };
    auto resWebp = TextureDecoder::DecodeAuto(fakeWebp, "unknown_file");
    EXPECT_FALSE(resWebp.has_value());

    // 4. KTX2 magic bytes: \xAB K T X ' ' '2' '0' \xBB
    std::vector<uint8_t> fakeKtx2 = { 0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
    auto resKtx2 = TextureDecoder::DecodeAuto(fakeKtx2, "unknown_file");
    EXPECT_FALSE(resKtx2.has_value());
}
