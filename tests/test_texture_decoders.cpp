/**
 * @file test_texture_decoders.cpp
 * @brief Unit tests for texture format decoders (PNG, JPEG, WebP, KTX2) and magic-byte sniffing.
 */
#include "gpu/Pipeline.hpp"
#include "scene/Scene.hpp"
#include "scene/TextureDecoder.hpp"
#include <gtest/gtest.h>
#include <memory_resource>
#if defined(BGL_ENABLE_KTX)
#include <ktx.h>
#endif

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

TEST(TextureDecoderTest, IsKtx2DataAndPmrDecodeOverload) {
    const std::vector<uint8_t> ktxMagic = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A, 0x00};
    const std::vector<uint8_t> pngMagic = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x00};
    const std::vector<uint8_t> tooShort = {0xAB, 'K', 'T', 'X'};

    EXPECT_TRUE(TextureDecoder::IsKtx2Data(ktxMagic));
    EXPECT_FALSE(TextureDecoder::IsKtx2Data(pngMagic));
    EXPECT_FALSE(TextureDecoder::IsKtx2Data(tooShort));
    EXPECT_FALSE(TextureDecoder::IsKtx2Data({}));

    std::array<std::byte, 4096> scratchBuf{};
    std::pmr::monotonic_buffer_resource arena(scratchBuf.data(), scratchBuf.size());

    auto resTruncated = TextureDecoder::DecodeKTX2(ktxMagic, KtxTranscodeTarget::Bc7, &arena);
    EXPECT_FALSE(resTruncated.has_value());

    TextureGpuResource texRes;
    EXPECT_EQ(texRes.vramBytes, 0u);
    EXPECT_EQ(texRes.format, KtxTranscodeTarget::Rgba32);
}

TEST(TextureDecoderTest, OpenExrOverflowAndCraftedHeaderHardening) {
    // Build minimal uncompressed EXR with crafted INT32_MIN..INT32_MAX dataWindow (would UB without int64 math)
    std::vector<uint8_t> exrOverflow{0x76, 0x2F, 0x31, 0x01, 0x02, 0x00, 0x00, 0x00};
    auto appendStr = [&](std::string_view s) {
        exrOverflow.insert(exrOverflow.end(), s.begin(), s.end());
        exrOverflow.push_back(0);
    };
    auto appendU32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i)
            exrOverflow.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFFu));
    };
    auto appendI32 = [&](int32_t v) { appendU32(static_cast<uint32_t>(v)); };

    // channels attribute ("R", HALF)
    appendStr("channels");
    appendStr("chlist");
    appendU32(19);
    appendStr("R");
    appendI32(1); // HALF
    for (int i = 0; i < 12; ++i)
        exrOverflow.push_back(0);
    exrOverflow.push_back(0); // end chlist

    // compression = 0
    appendStr("compression");
    appendStr("compression");
    appendU32(1);
    exrOverflow.push_back(0);

    // dataWindow with signed overflow span
    appendStr("dataWindow");
    appendStr("box2i");
    appendU32(16);
    appendI32(-2000000000);
    appendI32(0);
    appendI32(2000000000);
    appendI32(0);
    exrOverflow.push_back(0); // end header

    EXPECT_FALSE(TextureDecoder::DecodeEXR(exrOverflow).has_value());

    // Minimal valid 1x1 EXR header followed by wraparound chunkOffset (UINT64_MAX - 2)
    std::vector<uint8_t> exrBadOffset{0x76, 0x2F, 0x31, 0x01, 0x02, 0x00, 0x00, 0x00};
    auto appendTo = [&](std::vector<uint8_t>& dst, std::string_view s) {
        dst.insert(dst.end(), s.begin(), s.end());
        dst.push_back(0);
    };
    auto appendU32To = [&](std::vector<uint8_t>& dst, uint32_t v) {
        for (int i = 0; i < 4; ++i)
            dst.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFFu));
    };
    appendTo(exrBadOffset, "channels");
    appendTo(exrBadOffset, "chlist");
    appendU32To(exrBadOffset, 19);
    appendTo(exrBadOffset, "R");
    appendU32To(exrBadOffset, 1);
    for (int i = 0; i < 12; ++i)
        exrBadOffset.push_back(0);
    exrBadOffset.push_back(0);
    appendTo(exrBadOffset, "compression");
    appendTo(exrBadOffset, "compression");
    appendU32To(exrBadOffset, 1);
    exrBadOffset.push_back(0);
    appendTo(exrBadOffset, "dataWindow");
    appendTo(exrBadOffset, "box2i");
    appendU32To(exrBadOffset, 16);
    appendU32To(exrBadOffset, 0);
    appendU32To(exrBadOffset, 0);
    appendU32To(exrBadOffset, 0);
    appendU32To(exrBadOffset, 0);
    exrBadOffset.push_back(0);
    // Offset table entry: 0xFFFFFFFFFFFFFFFD (would wrap +8 without safe bounds check)
    for (int i = 0; i < 8; ++i)
        exrBadOffset.push_back(i == 0 ? 0xFDu : 0xFFu);

    EXPECT_FALSE(TextureDecoder::DecodeEXR(exrBadOffset).has_value());

    // Invalid oversized/corrupt WebP header rejected prior to full pixel allocation
    std::vector<uint8_t> corruptWebp{'R', 'I', 'F', 'F', 20, 0, 0, 0, 'W', 'E', 'B', 'P',
                                     'V', 'P', '8', ' ', 8,  0, 0, 0, 0,   0,   0,   0};
    EXPECT_FALSE(TextureDecoder::DecodeWebP(corruptWebp).has_value());

    // Extreme HDR resolution banner rejected cleanly without OOM
    const std::string craftedHdr{"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 100000000 +X 100000000\n"};
    const std::vector<uint8_t> hdrBytes(craftedHdr.begin(), craftedHdr.end());
    EXPECT_FALSE(TextureDecoder::DecodeHDR(hdrBytes).has_value());

    // Oversized PNG IHDR dimensions rejected cleanly
    const std::vector<uint8_t> craftedPng{0x89, 'P',  'N',  'G',  0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00,
                                          0x0D, 'I',  'H',  'D',  'R',  0x7F, 0xFF, 0xFF, 0xFF, 0x7F, 0xFF,
                                          0xFF, 0xFF, 0x08, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    EXPECT_FALSE(TextureDecoder::DecodePNG(craftedPng).has_value());

    // Truncated / corrupt KTX2 header rejected cleanly
    const std::vector<uint8_t> craftedKtx2{0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB,
                                           0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x00};
    EXPECT_FALSE(TextureDecoder::DecodeKTX2(craftedKtx2, KtxTranscodeTarget::Rgba32).has_value());

    // Valid minimal 1x1 Radiance HDR produces finite non-negative RGBA channels
    const std::string validHdrStr{"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n"};
    std::vector<uint8_t> validHdr(validHdrStr.begin(), validHdrStr.end());
    validHdr.insert(validHdr.end(), {128u, 128u, 128u, 128u});
    const auto hdrOk = TextureDecoder::DecodeHDR(validHdr);
    ASSERT_TRUE(hdrOk.has_value());
    EXPECT_EQ(hdrOk->width, 1u);
    EXPECT_EQ(hdrOk->height, 1u);
    ASSERT_EQ(hdrOk->rgbaFloat.size(), 4u);
    for (float c : hdrOk->rgbaFloat) {
        EXPECT_TRUE(std::isfinite(c));
        EXPECT_GE(c, 0.0f);
    }

    // OpenEXR non-zero version flags (e.g., tiled bit 0x200) rejected cleanly as unsupported
    std::vector<uint8_t> exrTiledFlags = {0x76, 0x2F, 0x31, 0x01, 0x02, 0x02, 0x00, 0x00, 0x00};
    EXPECT_FALSE(TextureDecoder::DecodeEXR(exrTiledFlags).has_value());

#if defined(BGL_ENABLE_KTX)
    {
        // Uncompressed 1x1 VK_FORMAT_B8G8R8A8_UNORM (44) KTX2 swaps B and R into RGBA
        ktxTextureCreateInfo ci{};
        ci.vkFormat = 44; // VK_FORMAT_B8G8R8A8_UNORM
        ci.baseWidth = 1;
        ci.baseHeight = 1;
        ci.baseDepth = 1;
        ci.numDimensions = 2;
        ci.numLevels = 1;
        ci.numLayers = 1;
        ci.numFaces = 1;
        ktxTexture2* ktxBgra{nullptr};
        if (ktxTexture2_Create(&ci, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &ktxBgra) == KTX_SUCCESS && ktxBgra) {
            const uint8_t bgra[4]{10u, 20u, 30u, 255u};
            if (ktxTexture_SetImageFromMemory(ktxTexture(ktxBgra), 0, 0, 0, bgra, sizeof(bgra)) == KTX_SUCCESS) {
                ktx_uint8_t* rawPtr{nullptr};
                ktx_size_t rawSize{0};
                if (ktxTexture_WriteToMemory(ktxTexture(ktxBgra), &rawPtr, &rawSize) == KTX_SUCCESS && rawPtr) {
                    const auto dec = TextureDecoder::DecodeKTX2({rawPtr, rawSize});
                    ASSERT_TRUE(dec.has_value()) << dec.error();
                    ASSERT_EQ(dec->rgbaData.size(), 4u);
                    EXPECT_EQ(dec->rgbaData[0], 30u); // R
                    EXPECT_EQ(dec->rgbaData[1], 20u); // G
                    EXPECT_EQ(dec->rgbaData[2], 10u); // B
                    EXPECT_EQ(dec->rgbaData[3], 255u);
                    free(rawPtr);
                }
            }
            ktxTexture_Destroy(ktxTexture(ktxBgra));
        }

        // Array KTX2 (numLayers == 2) must be rejected cleanly
        ktxTextureCreateInfo arrCi = ci;
        arrCi.numLayers = 2;
        arrCi.isArray = KTX_TRUE;
        ktxTexture2* ktxArr{nullptr};
        if (ktxTexture2_Create(&arrCi, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &ktxArr) == KTX_SUCCESS && ktxArr) {
            ktx_uint8_t* rawPtr{nullptr};
            ktx_size_t rawSize{0};
            if (ktxTexture_WriteToMemory(ktxTexture(ktxArr), &rawPtr, &rawSize) == KTX_SUCCESS && rawPtr) {
                EXPECT_FALSE(TextureDecoder::DecodeKTX2({rawPtr, rawSize}).has_value());
                free(rawPtr);
            }
            ktxTexture_Destroy(ktxTexture(ktxArr));
        }
    }
#endif
}
