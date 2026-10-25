/**
 * @file test_texture_decoders.cpp
 * @brief Unit tests for texture format decoders (PNG, JPEG, WebP, KTX2) and magic-byte sniffing.
 */
#include "gpu/Pipeline.hpp"
#include "scene/TextureDecoder.hpp"
#include <gtest/gtest.h>

using namespace bgl::scene;

TEST(TextureDecoderTest, SupportedFormatsQuery) {
    auto formats = TextureDecoder::GetSupportedFormats();
    EXPECT_FALSE(formats.empty());

#if defined(BGL_ENABLE_PNG)
    EXPECT_TRUE(TextureDecoder::IsPngSupported());
    bool hasPng = false;
    for (auto f : formats)
        if (f == "PNG")
            hasPng = true;
    EXPECT_TRUE(hasPng);
#endif

#if defined(BGL_ENABLE_JPEG)
    EXPECT_TRUE(TextureDecoder::IsJpegSupported());
    bool hasJpeg = false;
    for (auto f : formats)
        if (f == "JPEG")
            hasJpeg = true;
    EXPECT_TRUE(hasJpeg);
#endif

#if defined(BGL_ENABLE_WEBP)
    EXPECT_TRUE(TextureDecoder::IsWebpSupported());
    bool hasWebp = false;
    for (auto f : formats)
        if (f == "WebP")
            hasWebp = true;
    EXPECT_TRUE(hasWebp);
#endif

#if defined(BGL_ENABLE_KTX)
    EXPECT_TRUE(TextureDecoder::IsKtxSupported());
    bool hasKtx = false;
    for (auto f : formats)
        if (f.find("KTX") != std::string_view::npos)
            hasKtx = true;
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
    std::vector<uint8_t> fakePng = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00};
    auto resPng = TextureDecoder::DecodeAuto(fakePng, "unknown_file");
    // Should fail cleanly in PNG decoder rather than unsupported format error
    EXPECT_FALSE(resPng.has_value());

    // 2. JPEG magic bytes: \xFF \xD8 \xFF
    std::vector<uint8_t> fakeJpeg = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10};
    auto resJpeg = TextureDecoder::DecodeAuto(fakeJpeg, "unknown_file");
    EXPECT_FALSE(resJpeg.has_value());

    // 3. WebP magic bytes: RIFF .... WEBP
    std::vector<uint8_t> fakeWebp = {'R', 'I', 'F', 'F', 0x00, 0x00, 0x00, 0x00, 'W', 'E', 'B', 'P'};
    auto resWebp = TextureDecoder::DecodeAuto(fakeWebp, "unknown_file");
    EXPECT_FALSE(resWebp.has_value());

    // 4. KTX2 magic bytes: \xAB K T X ' ' '2' '0' \xBB
    std::vector<uint8_t> fakeKtx2 = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    auto resKtx2 = TextureDecoder::DecodeAuto(fakeKtx2, "unknown_file");
    EXPECT_FALSE(resKtx2.has_value());
}

TEST(TextureDecoderTest, RadianceHdrAndOpenExrSignatures) {
    // 1. Empty buffer rejection
    std::vector<uint8_t> emptyHdr;
    auto resEmpty = TextureDecoder::DecodeHdrAuto(emptyHdr, "sky.hdr");
    EXPECT_FALSE(resEmpty.has_value());

    // 2. Minimal valid uncompressed Radiance HDR payload
    std::string validHdrStr = "#?RADIANCE\n"
                              "FORMAT=32-bit_rle_rgbe\n\n"
                              "-Y 1 +X 2\n";
    // 2 uncompressed pixels: black (0,0,0,0) and unit white (128,128,128,128)
    std::vector<uint8_t> hdrBytes(validHdrStr.begin(), validHdrStr.end());
    hdrBytes.insert(hdrBytes.end(), {0, 0, 0, 0, 128, 128, 128, 128});

    auto resHdr = TextureDecoder::DecodeHDR(hdrBytes);
    ASSERT_TRUE(resHdr.has_value()) << (resHdr.has_value() ? "" : resHdr.error());
    EXPECT_EQ(resHdr->width, 2u);
    EXPECT_EQ(resHdr->height, 1u);
    ASSERT_EQ(resHdr->rgbaFloat.size(), 8u);     // 2 pixels * 4 components
    EXPECT_FLOAT_EQ(resHdr->rgbaFloat[0], 0.0f); // pixel 0 R
    EXPECT_FLOAT_EQ(resHdr->rgbaFloat[3], 1.0f); // pixel 0 A
    EXPECT_GT(resHdr->rgbaFloat[4], 0.0f);       // pixel 1 R > 0

    // 3. OpenEXR magic signature detection (0x76, 0x2F, 0x31, 0x01)
    std::vector<uint8_t> fakeExr = {0x76, 0x2F, 0x31, 0x01, 0x02, 0x00, 0x00, 0x00};
    auto resExr = TextureDecoder::DecodeEXR(fakeExr);
    // Signature detected but payload truncated: cleanly returns error result
    EXPECT_FALSE(resExr.has_value());

    // 4. Auto-routing by extension
    auto autoRes = TextureDecoder::DecodeHdrAuto(hdrBytes, "environment.hdr");
    EXPECT_TRUE(autoRes.has_value());
}

TEST(TextureDecoderTest, KtxTranscodeTargetNamesAndDefaultTarget) {
    EXPECT_EQ(TextureDecoder::GetKtxTargetName(KtxTranscodeTarget::Rgba32), "RGBA32");
    EXPECT_EQ(TextureDecoder::GetKtxTargetName(KtxTranscodeTarget::Bc7), "BC7_RGBA");
    EXPECT_EQ(TextureDecoder::GetKtxTargetName(KtxTranscodeTarget::Astc4x4), "ASTC_4x4_RGBA");
    EXPECT_EQ(TextureDecoder::GetKtxTargetName(KtxTranscodeTarget::Etc2Rgba), "ETC2_RGBA8");

    // Setting and getting default KTX target
    TextureDecoder::SetDefaultKtxTarget(KtxTranscodeTarget::Bc7);
    EXPECT_EQ(TextureDecoder::GetDefaultKtxTarget(), KtxTranscodeTarget::Bc7);
    TextureDecoder::SetDefaultKtxTarget(KtxTranscodeTarget::Rgba32);
    EXPECT_EQ(TextureDecoder::GetDefaultKtxTarget(), KtxTranscodeTarget::Rgba32);
}

TEST(TextureDecoderTest, MipLevelCalculationsForOddAndNonPowerOfTwo) {
    // PipelineManager::CalculateMipLevels tests across POT, NPOT, and 1D aspect textures
    EXPECT_EQ(bgl::gpu::PipelineManager::CalculateMipLevels(1, 1), 1u);
    EXPECT_EQ(bgl::gpu::PipelineManager::CalculateMipLevels(2, 2), 2u);
    EXPECT_EQ(bgl::gpu::PipelineManager::CalculateMipLevels(3, 1), 2u);
    EXPECT_EQ(bgl::gpu::PipelineManager::CalculateMipLevels(1, 512), 10u);
    EXPECT_EQ(bgl::gpu::PipelineManager::CalculateMipLevels(33, 17), 6u);
    EXPECT_EQ(bgl::gpu::PipelineManager::CalculateMipLevels(1920, 1080), 11u);
    EXPECT_EQ(bgl::gpu::PipelineManager::CalculateMipLevels(4096, 2048), 13u);
}

TEST(TextureDecoderTest, RadianceHdrMultiPixelDecodedValues) {
    // 3 pixels in uncompressed Radiance HDR format:
    // Pixel 0: RGBE (0, 0, 0, 0) -> Black (0, 0, 0, 1)
    // Pixel 1: RGBE (255, 0, 0, 129) -> Red: (255/256)*2^(129-128) = (255/256)*2 = ~1.992
    // Pixel 2: RGBE (0, 255, 0, 128) -> Green: (255/256)*2^0 = ~0.996
    std::string hdrHeader = "#?RADIANCE\n"
                            "FORMAT=32-bit_rle_rgbe\n\n"
                            "-Y 1 +X 3\n";
    std::vector<uint8_t> hdrData(hdrHeader.begin(), hdrHeader.end());
    hdrData.insert(hdrData.end(), {0, 0, 0, 0, 255, 0, 0, 129, 0, 255, 0, 128});

    auto res = TextureDecoder::DecodeHDR(hdrData);
    ASSERT_TRUE(res.has_value()) << (res.has_value() ? "" : res.error());
    EXPECT_EQ(res->width, 3u);
    EXPECT_EQ(res->height, 1u);
    ASSERT_EQ(res->rgbaFloat.size(), 12u); // 3 * 4

    // Pixel 0 (Black)
    EXPECT_FLOAT_EQ(res->rgbaFloat[0], 0.0f);
    EXPECT_FLOAT_EQ(res->rgbaFloat[1], 0.0f);
    EXPECT_FLOAT_EQ(res->rgbaFloat[2], 0.0f);
    EXPECT_FLOAT_EQ(res->rgbaFloat[3], 1.0f);

    // Pixel 1 (Bright Red > 1.9, green and blue near zero)
    EXPECT_NEAR(res->rgbaFloat[4], 1.992f, 0.05f);
    EXPECT_NEAR(res->rgbaFloat[5], 0.0f, 0.005f);
    EXPECT_NEAR(res->rgbaFloat[6], 0.0f, 0.005f);

    // Pixel 2 (Unit Green ~1.0, red and blue near zero)
    EXPECT_NEAR(res->rgbaFloat[8], 0.0f, 0.005f);
    EXPECT_NEAR(res->rgbaFloat[9], 0.996f, 0.05f);
    EXPECT_NEAR(res->rgbaFloat[10], 0.0f, 0.005f);
}

TEST(TextureDecoderTest, InvalidExtensionAndUnsupportedPayload) {
    std::vector<uint8_t> dummyData = {0x01, 0x02, 0x03, 0x04};
    auto resUnknown = TextureDecoder::DecodeAuto(dummyData, "texture.unsupported_extension");
    EXPECT_FALSE(resUnknown.has_value());
}
