#include "core/MemoryTracker.hpp"
#include "core/NetworkFetcher.hpp"
#include <filesystem>
#include <future>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using bgl::core::FetchProgress;
using bgl::core::MemoryTracker;
using bgl::core::NetworkFetcher;

namespace {

TEST(NetworkFetcherTest, SchemeRecognitionHandlesCaseAndSanitization) {
    EXPECT_TRUE(NetworkFetcher::IsHttpUrl("http://example.com/a.glb"));
    EXPECT_TRUE(NetworkFetcher::IsHttpUrl("  \"HTTPS://example.com/a.glb\" \r\n"));
    EXPECT_FALSE(NetworkFetcher::IsHttpUrl("file:///tmp/a.glb"));

    EXPECT_TRUE(NetworkFetcher::IsFileUrl("file:///assets/ToyCar.glb"));
    EXPECT_TRUE(NetworkFetcher::IsFileUrl("<FILE:///tmp/test.gltf>"));
    EXPECT_FALSE(NetworkFetcher::IsFileUrl("/assets/ToyCar.glb"));

    EXPECT_TRUE(NetworkFetcher::IsDataUri("data:application/octet-stream;base64,AAAA"));
    EXPECT_TRUE(NetworkFetcher::IsDataUri("DATA:text/plain,hello"));
    EXPECT_FALSE(NetworkFetcher::IsDataUri("assets/ToyCar.glb"));

    EXPECT_TRUE(NetworkFetcher::IsUrl("https://khronos.org/model.glb"));
    EXPECT_TRUE(NetworkFetcher::IsUrl("file:///tmp/x.glb"));
    EXPECT_TRUE(NetworkFetcher::IsUrl("data:,abc"));
    EXPECT_FALSE(NetworkFetcher::IsUrl("assets/ToyCar.glb"));
}

TEST(NetworkFetcherTest, SanitizeAndDecodeUrlPercentHandleEdgeCases) {
    // RFC 2483 text/uri-list with comments and CRLF
    const std::string uriList =
        "# comment line\r\n  \"https://example.com/my%20model.glb\"  \r\nhttps://other.com/b.glb\r\n";
    EXPECT_EQ(NetworkFetcher::SanitizeUri(uriList), "https://example.com/my%20model.glb");
    EXPECT_EQ(NetworkFetcher::SanitizeUri("   "), "");
    EXPECT_EQ(NetworkFetcher::SanitizeUri("<assets/ToyCar.glb>"), "assets/ToyCar.glb");

    EXPECT_EQ(NetworkFetcher::DecodeUrlPercent("My%20Model%2FScene%201.glb"), "My Model/Scene 1.glb");
    EXPECT_EQ(NetworkFetcher::DecodeUrlPercent("100%ZZ_trailing%"), "100%ZZ_trailing%");
}

TEST(NetworkFetcherTest, ResolveRelativeUriSupportsHttpFileAndLocalPaths) {
    EXPECT_EQ(NetworkFetcher::ResolveRelativeUri("https://host.org/dir/sub/scene.gltf?v=2#frag", "../bin/data.bin"),
              "https://host.org/dir/bin/data.bin");
    EXPECT_EQ(NetworkFetcher::ResolveRelativeUri("https://host.org/models/car.gltf", "https://cdn.org/tex.png"),
              "https://cdn.org/tex.png");
    EXPECT_EQ(NetworkFetcher::ResolveRelativeUri("/tmp/models/sub/scene.gltf", "../textures/albedo.png"),
              "/tmp/models/textures/albedo.png");
    EXPECT_EQ(NetworkFetcher::ResolveRelativeUri("data:model/gltf-binary;base64,AA==", "extra.bin"), "extra.bin");
}

TEST(NetworkFetcherTest, ExtractDisplayNameStripsQueryAndDirectory) {
    EXPECT_EQ(NetworkFetcher::ExtractDisplayName("https://cdn.example.com/assets/Sponza.glb?token=123#mesh"),
              "Sponza.glb");
    EXPECT_EQ(NetworkFetcher::ExtractDisplayName("assets/ToyCar.glb"), "ToyCar.glb");
}

TEST(NetworkFetcherTest, FetchSyncReadsLocalFileWithProgressAndSupportsCancellation) {
    ASSERT_TRUE(std::filesystem::exists("assets/ToyCar.glb"));

    std::vector<FetchProgress> reports;
    auto okRes = NetworkFetcher::FetchSync("assets/ToyCar.glb", [&](const FetchProgress& p) {
        reports.push_back(p);
        return true;
    });
    ASSERT_TRUE(okRes.has_value()) << okRes.error();
    EXPECT_GT(okRes->size(), 1024u);
    ASSERT_FALSE(reports.empty());
    EXPECT_EQ(reports.back().bytesReceived, okRes->size());
    EXPECT_EQ(reports.back().totalBytes, okRes->size());

    // Cooperative cancellation returns error cleanly
    auto cancelRes = NetworkFetcher::FetchSync("assets/ToyCar.glb", [](const FetchProgress&) { return false; });
    EXPECT_FALSE(cancelRes.has_value());
}

TEST(NetworkFetcherTest, FetchSyncHandlesFileUrlDataUriAndRejectsInvalidTargets) {
    const auto absToyCar = std::filesystem::absolute("assets/ToyCar.glb").string();
    auto fileRes = NetworkFetcher::FetchSync("file://" + absToyCar);
    ASSERT_TRUE(fileRes.has_value()) << fileRes.error();
    EXPECT_GT(fileRes->size(), 1024u);

    // RFC 2397 base64 + plain data URIs ("Hello!" = SGVsbG8h)
    auto b64Res = NetworkFetcher::FetchSync("data:text/plain;base64,SGVsbG8h");
    ASSERT_TRUE(b64Res.has_value()) << b64Res.error();
    EXPECT_EQ(std::string(b64Res->begin(), b64Res->end()), "Hello!");

    auto plainRes = NetworkFetcher::FetchSync("data:text/plain,abc%20123");
    ASSERT_TRUE(plainRes.has_value()) << plainRes.error();
    EXPECT_EQ(std::string(plainRes->begin(), plainRes->end()), "abc 123");

    // Bad base64 character is rejected safely
    EXPECT_FALSE(NetworkFetcher::FetchSync("data:application/octet-stream;base64,@@@").has_value());

    // Non-existent path and directory target fail gracefully
    EXPECT_FALSE(NetworkFetcher::FetchSync("assets/does_not_exist_12345.glb").has_value());
    EXPECT_FALSE(NetworkFetcher::FetchSync("assets").has_value());
}

TEST(NetworkFetcherTest, FetchAsyncCompletesForValidAndMissingFiles) {
    {
        std::promise<bgl::core::Result<std::vector<uint8_t>>> prom;
        auto fut = prom.get_future();
        NetworkFetcher::FetchAsync("assets/ToyCar.glb",
                                   [&](bgl::core::Result<std::vector<uint8_t>> r) { prom.set_value(std::move(r)); });
        const auto res = fut.get();
        EXPECT_TRUE(res.has_value());
    }
    {
        std::promise<bgl::core::Result<std::vector<uint8_t>>> prom;
        auto fut = prom.get_future();
        NetworkFetcher::FetchAsync("assets/missing_async_target.glb",
                                   [&](bgl::core::Result<std::vector<uint8_t>> r) { prom.set_value(std::move(r)); });
        const auto res = fut.get();
        EXPECT_FALSE(res.has_value());
    }
}

TEST(MemoryTrackerTest, TracksGpuBytesSafelyAndPreventsUnderflow) {
    auto& tracker = MemoryTracker::Instance();
    const size_t baseBuf = tracker.GetGpuBufferBytes();
    const size_t baseTex = tracker.GetGpuTextureBytes();

    tracker.TrackBufferAlloc(4096);
    tracker.TrackTextureAlloc(8192);
    EXPECT_EQ(tracker.GetGpuBufferBytes(), baseBuf + 4096);
    EXPECT_EQ(tracker.GetGpuTextureBytes(), baseTex + 8192);

    const auto snap = tracker.Query();
    EXPECT_EQ(snap.TotalGpuBytes(), snap.gpuBufferBytes + snap.gpuTextureBytes);
    EXPECT_GE(snap.VramMb(), 0.0f);
    EXPECT_GE(snap.RamMb(), 0.0f);

    tracker.TrackBufferFree(4096);
    tracker.TrackTextureFree(8192);
    EXPECT_EQ(tracker.GetGpuBufferBytes(), baseBuf);
    EXPECT_EQ(tracker.GetGpuTextureBytes(), baseTex);

    // Saturated free does not underflow when baseline happens to be zero
    if (baseBuf == 0) {
        tracker.TrackBufferFree(123456);
        EXPECT_EQ(tracker.GetGpuBufferBytes(), 0u);
    }
}

TEST(NetworkFetcherTest, ResolveExternalAssetUriBlocksCrossSchemeAndControlInjection) {
    // 1. Remote HTTP(S) base allows same/remote HTTP(S) and inline data:, never file://
    auto okHttp = NetworkFetcher::ResolveExternalAssetUri("https://cdn.example.com/models/car.gltf", "../tex/a.png");
    ASSERT_TRUE(okHttp.has_value());
    EXPECT_EQ(*okHttp, "https://cdn.example.com/tex/a.png");

    auto okData = NetworkFetcher::ResolveExternalAssetUri("https://cdn.example.com/models/car.gltf",
                                                          "data:image/png;base64,AAAA");
    EXPECT_TRUE(okData.has_value());

    EXPECT_FALSE(
        NetworkFetcher::ResolveExternalAssetUri("https://cdn.example.com/models/car.gltf", "file:///etc/passwd")
            .has_value());

    // 2. Inline data: or empty base may only reference data: URIs (no local filesystem access)
    EXPECT_TRUE(NetworkFetcher::ResolveExternalAssetUri("", "data:application/octet-stream;base64,AA==").has_value());
    EXPECT_FALSE(NetworkFetcher::ResolveExternalAssetUri("", "/etc/passwd").has_value());
    EXPECT_FALSE(NetworkFetcher::ResolveExternalAssetUri("data:model/gltf+json,{}", "file:///etc/passwd").has_value());

    // 3. Reject NUL-byte (%00) and ASCII control character injection in subresources and FetchSync
    EXPECT_FALSE(NetworkFetcher::ResolveExternalAssetUri("assets/scene.gltf", "textures/albedo%00.png").has_value());
    EXPECT_FALSE(NetworkFetcher::FetchSync("file:///assets/ToyCar%00.glb").has_value());
    EXPECT_FALSE(NetworkFetcher::FetchSync("assets/ToyCar%00.glb").has_value());
    EXPECT_FALSE(NetworkFetcher::FetchSync(std::string("https://example.com/\x01/evil.glb")).has_value());
}

} // namespace
