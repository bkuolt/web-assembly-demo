#include "core/Types.hpp"
#include "gpu/Handle.hpp"
#include "gpu/Pipeline.hpp"
#include "gpu/RayTracingPass.hpp"
#include "gpu/RayTracingScene.hpp"
#include "scene/Scene.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <vector>

#if defined(BGL_PLATFORM_NATIVE)
#include <webgpu/wgpu.h>
#endif

namespace bgl::gpu {
namespace {

TEST(RayTracingSceneTest, StructLayoutsMatchWgslAbi) {
    EXPECT_EQ(sizeof(RtVertex), 32u);
    EXPECT_EQ(sizeof(RtBvhNode), 32u);
    EXPECT_EQ(sizeof(RtInstance), 176u);
    EXPECT_EQ(sizeof(RtMaterial), 80u);
    EXPECT_EQ(sizeof(RtPassUniforms), 416u);
    EXPECT_EQ(sizeof(RtCompositeUniforms), 16u);
}

TEST(RayTracingSceneTest, BuildBinnedSahBlasHandlesEmptySingleAndLargeMeshes) {
    // 1. Empty index buffer produces zero BVH nodes
    {
        std::vector<RtVertex> verts(3);
        std::vector<uint32_t> emptyIdx;
        std::vector<RtBvhNode> nodes;
        std::vector<uint32_t> reordered;
        const uint32_t count = RayTracingScene::BuildBinnedSahBlas(verts, emptyIdx, nodes, reordered);
        EXPECT_EQ(count, 0u);
        EXPECT_TRUE(nodes.empty());
        EXPECT_TRUE(reordered.empty());
    }

    // 2. Single triangle produces 1 leaf node with tight AABB
    {
        std::vector<RtVertex> verts(3);
        verts[0].posU = glm::vec4{-1.0f, 0.0f, 2.0f, 0.0f};
        verts[1].posU = glm::vec4{3.0f, 4.0f, 2.0f, 0.5f};
        verts[2].posU = glm::vec4{0.0f, -2.0f, 5.0f, 1.0f};
        const std::vector<uint32_t> idx{0u, 1u, 2u};
        std::vector<RtBvhNode> nodes;
        std::vector<uint32_t> reordered;
        const uint32_t count = RayTracingScene::BuildBinnedSahBlas(verts, idx, nodes, reordered);
        ASSERT_EQ(count, 1u);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(nodes[0].triCount, 1u);
        EXPECT_EQ(nodes[0].leftFirst, 0u);
        EXPECT_NEAR(nodes[0].aabbMin.x, -1.0f, 1e-5f);
        EXPECT_NEAR(nodes[0].aabbMin.y, -2.0f, 1e-5f);
        EXPECT_NEAR(nodes[0].aabbMin.z, 2.0f, 1e-5f);
        EXPECT_NEAR(nodes[0].aabbMax.x, 3.0f, 1e-5f);
        EXPECT_NEAR(nodes[0].aabbMax.y, 4.0f, 1e-5f);
        EXPECT_NEAR(nodes[0].aabbMax.z, 5.0f, 1e-5f);
        EXPECT_EQ(reordered.size(), 3u);
    }

    // 3. Multi-triangle grid (> kMaxLeafTris) splits into internal + bounded leaf nodes
    {
        constexpr int kQuads = 16;
        std::vector<RtVertex> verts;
        std::vector<uint32_t> idx;
        verts.reserve(kQuads * 4);
        idx.reserve(kQuads * 6);
        for (int i = 0; i < kQuads; ++i) {
            const float x0 = static_cast<float>(i) * 2.0f;
            const float x1 = x0 + 1.0f;
            const uint32_t base = static_cast<uint32_t>(verts.size());
            RtVertex v0{}, v1{}, v2{}, v3{};
            v0.posU = glm::vec4{x0, 0.0f, 0.0f, 0.0f};
            v1.posU = glm::vec4{x1, 0.0f, 0.0f, 1.0f};
            v2.posU = glm::vec4{x1, 1.0f, 0.0f, 1.0f};
            v3.posU = glm::vec4{x0, 1.0f, 0.0f, 0.0f};
            verts.push_back(v0);
            verts.push_back(v1);
            verts.push_back(v2);
            verts.push_back(v3);
            idx.insert(idx.end(), {base + 0u, base + 1u, base + 2u, base + 0u, base + 2u, base + 3u});
        }

        std::vector<RtBvhNode> nodes;
        std::vector<uint32_t> reordered;
        const uint32_t count = RayTracingScene::BuildBinnedSahBlas(verts, idx, nodes, reordered);
        EXPECT_GT(count, 1u);
        EXPECT_EQ(count, nodes.size());
        EXPECT_EQ(reordered.size(), idx.size());
        EXPECT_EQ(nodes[0].triCount, 0u); // Root must be internal split for 32 tris
        uint32_t leafTriSum = 0u;
        for (const auto& n : nodes) {
            if (n.triCount > 0u) {
                EXPECT_LE(n.triCount, 4u);
                leafTriSum += n.triCount;
            }
        }
        EXPECT_EQ(leafTriSum, static_cast<uint32_t>(kQuads * 2));
    }
}

TEST(RayTracingSceneTest, BuildFromSceneAndCpuRayTracingTracksClosestHitAndTlasUpdates) {
    RayTracingScene rtScene;
    scene::Scene emptyScene;
    rtScene.BuildFromScene(emptyScene);
    EXPECT_FALSE(rtScene.IsReady());
    EXPECT_FALSE(rtScene.TraceRayCpu(core::Ray{glm::vec3{0.0f, 0.0f, 5.0f}, glm::vec3{0.0f, 0.0f, -1.0f}}).hit);

    auto scene = std::make_shared<scene::Scene>();
    auto mesh = std::make_shared<scene::Mesh>();
    auto& prim = mesh->primitives.emplace_back();
    prim.vertices.resize(3);
    prim.vertices[0].position = glm::vec3{-1.0f, -1.0f, 0.0f};
    prim.vertices[1].position = glm::vec3{1.0f, -1.0f, 0.0f};
    prim.vertices[2].position = glm::vec3{0.0f, 1.0f, 0.0f};
    for (auto& v : prim.vertices) {
        v.normal = glm::vec3{0.0f, 0.0f, 1.0f};
    }
    prim.indices = {0u, 1u, 2u};
    prim.boundingBox.minBound = glm::vec3{-1.0f, -1.0f, 0.0f};
    prim.boundingBox.maxBound = glm::vec3{1.0f, 1.0f, 0.0f};

    auto nearNode = std::make_shared<scene::SceneNode>();
    nearNode->mesh = mesh;
    nearNode->localTransform = glm::translate(glm::mat4{1.0f}, glm::vec3{0.0f, 0.0f, 1.0f});

    auto farNode = std::make_shared<scene::SceneNode>();
    farNode->mesh = mesh;
    farNode->localTransform = glm::translate(glm::mat4{1.0f}, glm::vec3{0.0f, 0.0f, -3.0f});

    scene->rootNodes.push_back(nearNode);
    scene->rootNodes.push_back(farNode);
    scene->allNodes.push_back(nearNode);
    scene->allNodes.push_back(farNode);
    scene->Update();

    rtScene.BuildFromScene(*scene);
    ASSERT_TRUE(rtScene.IsReady());
    const auto stats = rtScene.GetStats();
    EXPECT_EQ(stats.blasCount, 1u); // Shared primitive reuses single BLAS
    EXPECT_EQ(stats.instanceCount, 2u);
    EXPECT_EQ(stats.triangleCount, 1u); // 1 unique BLAS triangle shared by 2 instances
    EXPECT_GT(stats.tlasNodeCount, 0u);

    // Closest hit returns nearNode at z = 1 (distance = 4 from z = 5)
    const core::Ray centerRay{glm::vec3{0.0f, 0.0f, 5.0f}, glm::vec3{0.0f, 0.0f, -1.0f}};
    const RtCpuHit hit = rtScene.TraceRayCpu(centerRay);
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.distance, 4.0f, 1e-4f);
    EXPECT_NEAR(hit.worldPos.z, 1.0f, 1e-4f);
    EXPECT_NEAR(glm::length(hit.worldNormal), 1.0f, 1e-4f);

    // maxDist shorter than near hit returns miss
    EXPECT_FALSE(rtScene.TraceRayCpu(centerRay, 3.5f).hit);

    // Ray missing geometry altogether
    EXPECT_FALSE(rtScene.TraceRayCpu(core::Ray{glm::vec3{10.0f, 0.0f, 5.0f}, glm::vec3{0.0f, 0.0f, -1.0f}}).hit);

    // Move nearNode far away along +X, update scene + TLAS, verify CPU trace reflects new transform
    nearNode->localTransform = glm::translate(glm::mat4{1.0f}, glm::vec3{8.0f, 0.0f, 1.0f});
    scene->Update();
    rtScene.UpdateTlas(*scene);

    // Now center ray skips moved nearNode and hits farNode at z = -3 (distance = 8 from z = 5)
    const RtCpuHit updatedCenterHit = rtScene.TraceRayCpu(centerRay);
    ASSERT_TRUE(updatedCenterHit.hit);
    EXPECT_NEAR(updatedCenterHit.distance, 8.0f, 1e-4f);
    EXPECT_NEAR(updatedCenterHit.worldPos.z, -3.0f, 1e-4f);

    // Ray at x = 8 hits moved nearNode
    const RtCpuHit movedHit = rtScene.TraceRayCpu(core::Ray{glm::vec3{8.0f, 0.0f, 5.0f}, glm::vec3{0.0f, 0.0f, -1.0f}});
    ASSERT_TRUE(movedHit.hit);
    EXPECT_NEAR(movedHit.distance, 4.0f, 1e-4f);
    EXPECT_NEAR(movedHit.worldPos.x, 8.0f, 1e-4f);
}

TEST(RayTracingSceneTest, SmallScaleModelTransformsAreNotCulledFromTlas) {
    // Regression guard: millimeter-scaled glTF models (e.g. scale 0.0001 -> det = 1e-12) must remain traceable
    auto scene = std::make_shared<scene::Scene>();
    auto mesh = std::make_shared<scene::Mesh>();
    auto& prim = mesh->primitives.emplace_back();
    prim.vertices.resize(3);
    prim.vertices[0].position = glm::vec3{-100.0f, -100.0f, 0.0f};
    prim.vertices[1].position = glm::vec3{100.0f, -100.0f, 0.0f};
    prim.vertices[2].position = glm::vec3{0.0f, 100.0f, 0.0f};
    for (auto& v : prim.vertices) {
        v.normal = glm::vec3{0.0f, 0.0f, 1.0f};
    }
    prim.indices = {0u, 1u, 2u};
    prim.boundingBox.minBound = glm::vec3{-100.0f, -100.0f, -1.0f};
    prim.boundingBox.maxBound = glm::vec3{100.0f, 100.0f, 1.0f};

    auto tinyNode = std::make_shared<scene::SceneNode>();
    tinyNode->mesh = mesh;
    tinyNode->localTransform = glm::scale(glm::mat4{1.0f}, glm::vec3{0.0001f});
    scene->rootNodes.push_back(tinyNode);
    scene->allNodes.push_back(tinyNode);
    scene->Update();

    RayTracingScene rtScene;
    rtScene.BuildFromScene(*scene);
    ASSERT_TRUE(rtScene.IsReady());
    EXPECT_EQ(rtScene.GetStats().instanceCount, 1u);

    const RtCpuHit hit = rtScene.TraceRayCpu(core::Ray{glm::vec3{0.0f, 0.0f, 0.05f}, glm::vec3{0.0f, 0.0f, -1.0f}});
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.distance, 0.05f, 1e-4f);
}

TEST(RayTracingPassTest, PipelineRtConfigClampingAndTogglesWork) {
    PipelineManager pipeline;
    EXPECT_FALSE(pipeline.IsDesktopRayTracingSupported());
    EXPECT_TRUE(pipeline.IsRayTracingEnabled());
    EXPECT_TRUE(pipeline.IsRtShadowsEnabled());
    EXPECT_TRUE(pipeline.IsRtAoEnabled());
    EXPECT_TRUE(pipeline.IsRtReflectionsEnabled());
    EXPECT_FALSE(pipeline.IsRtPathTracingEnabled());
    EXPECT_TRUE(pipeline.IsRtHalfResEnabled());

    pipeline.SetRtAoRadius(-10.0f);
    EXPECT_FLOAT_EQ(pipeline.GetRtAoRadius(), 0.05f);
    pipeline.SetRtAoRadius(100.0f);
    EXPECT_FLOAT_EQ(pipeline.GetRtAoRadius(), 5.0f);

    pipeline.SetRtShadowSoftness(-1.0f);
    EXPECT_FLOAT_EQ(pipeline.GetRtShadowSoftness(), 0.0f);
    pipeline.SetRtShadowSoftness(2.0f);
    EXPECT_FLOAT_EQ(pipeline.GetRtShadowSoftness(), 0.15f);

    pipeline.SetRtReflectionIntensity(-0.5f);
    EXPECT_FLOAT_EQ(pipeline.GetRtReflectionIntensity(), 0.0f);
    pipeline.SetRtReflectionIntensity(9.0f);
    EXPECT_FLOAT_EQ(pipeline.GetRtReflectionIntensity(), 2.0f);

    pipeline.SetRtPathBounces(0u);
    EXPECT_EQ(pipeline.GetRtPathBounces(), 1u);
    pipeline.SetRtPathBounces(99u);
    EXPECT_EQ(pipeline.GetRtPathBounces(), 6u);

    pipeline.SetRayTracingEnabled(false);
    pipeline.SetRtShadowsEnabled(false);
    pipeline.SetRtAoEnabled(false);
    pipeline.SetRtReflectionsEnabled(false);
    pipeline.SetRtPathTracingEnabled(true);
    pipeline.SetRtHalfResEnabled(false);
    EXPECT_FALSE(pipeline.IsRayTracingEnabled());
    EXPECT_FALSE(pipeline.IsRtShadowsEnabled());
    EXPECT_FALSE(pipeline.IsRtAoEnabled());
    EXPECT_FALSE(pipeline.IsRtReflectionsEnabled());
    EXPECT_TRUE(pipeline.IsRtPathTracingEnabled());
    EXPECT_FALSE(pipeline.IsRtHalfResEnabled());

    // Without active GPU initialization, active queries remain false
    EXPECT_FALSE(pipeline.IsRtShadowsActive());
    EXPECT_FALSE(pipeline.IsRtAoActive());
    EXPECT_FALSE(pipeline.IsRtReflectionsActive());
    EXPECT_FALSE(pipeline.IsPathTracingActive());
}

#if defined(BGL_PLATFORM_NATIVE)
TEST(RayTracingPassTest, HeadlessWebGpuHybridAndPathTracePasses) {
    InstanceHandle instance{wgpuCreateInstance(nullptr)};
    if (!instance) {
        GTEST_SKIP() << "WebGPU instance unavailable";
    }

    WGPURequestAdapterOptions opts = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    opts.powerPreference = WGPUPowerPreference_HighPerformance;
    struct AdapterData {
        WGPUAdapter adapter{nullptr};
    } adapterData{};
    WGPURequestAdapterCallbackInfo adapterCb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    adapterCb.mode = WGPUCallbackMode_AllowProcessEvents;
    adapterCb.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView, void* u1, void*) {
        if (status == WGPURequestAdapterStatus_Success) {
            static_cast<AdapterData*>(u1)->adapter = adapter;
        }
    };
    adapterCb.userdata1 = &adapterData;
    wgpuInstanceRequestAdapter(instance, &opts, adapterCb);
    AdapterHandle adapter{adapterData.adapter};
    if (!adapter) {
        GTEST_SKIP() << "No hardware/software WebGPU adapter available";
    }

    WGPUDeviceDescriptor devDesc = WGPU_DEVICE_DESCRIPTOR_INIT;
    devDesc.label = ToWgpuStringView("RayTracingHeadlessDevice");
    struct DeviceData {
        WGPUDevice device{nullptr};
    } devData{};
    WGPURequestDeviceCallbackInfo devCb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    devCb.mode = WGPUCallbackMode_AllowProcessEvents;
    devCb.callback = [](WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView, void* u1, void*) {
        if (status == WGPURequestDeviceStatus_Success) {
            static_cast<DeviceData*>(u1)->device = device;
        }
    };
    devCb.userdata1 = &devData;
    wgpuAdapterRequestDevice(adapter, &devDesc, devCb);
    DeviceHandle device{devData.device};
    if (!device) {
        GTEST_SKIP() << "Could not create headless WebGPU device";
    }
    QueueHandle queue{wgpuDeviceGetQueue(device)};
    ASSERT_TRUE(queue);

    PipelineManager pipelineMgr;
    auto initRes = pipelineMgr.Initialize(device, queue, WGPUTextureFormat_BGRA8Unorm, 1);
    ASSERT_TRUE(initRes.has_value()) << initRes.error();

    // Passing false disables desktop RT cleanly
    auto noRtRes = pipelineMgr.InitializeDesktopRayTracing(false, false, false);
    ASSERT_TRUE(noRtRes.has_value());
    EXPECT_FALSE(pipelineMgr.IsDesktopRayTracingSupported());

    // Passing true initializes RT scene & compute/render pipelines
    auto rtRes = pipelineMgr.InitializeDesktopRayTracing(true, false, false);
    ASSERT_TRUE(rtRes.has_value()) << rtRes.error();
    EXPECT_TRUE(pipelineMgr.IsDesktopRayTracingSupported());
    EXPECT_TRUE(pipelineMgr.GetRayTracingScene().IsInitialized());
    EXPECT_TRUE(pipelineMgr.GetRayTracingPass().IsInitialized());

    auto scene = std::make_shared<scene::Scene>();
    auto node = std::make_shared<scene::SceneNode>();
    auto mesh = std::make_shared<scene::Mesh>();
    auto& prim = mesh->primitives.emplace_back();
    prim.vertices.resize(3);
    prim.vertices[0].position = glm::vec3{-1.0f, -1.0f, 0.0f};
    prim.vertices[1].position = glm::vec3{1.0f, -1.0f, 0.0f};
    prim.vertices[2].position = glm::vec3{0.0f, 1.0f, 0.0f};
    for (auto& v : prim.vertices) {
        v.normal = glm::vec3{0.0f, 0.0f, 1.0f};
    }
    prim.indices = {0u, 1u, 2u};
    prim.boundingBox.minBound = glm::vec3{-1.0f, -1.0f, -0.1f};
    prim.boundingBox.maxBound = glm::vec3{1.0f, 1.0f, 0.1f};
    node->mesh = mesh;
    scene->rootNodes.push_back(node);
    scene->allNodes.push_back(node);
    scene->Update();

    pipelineMgr.PrepareScene(*scene);
    EXPECT_TRUE(pipelineMgr.GetRayTracingScene().IsReady());
    EXPECT_FALSE(pipelineMgr.IsRtShadowsActive()); // Allocated lazily on first PrePass
    EXPECT_FALSE(pipelineMgr.IsRtAoActive());
    EXPECT_TRUE(pipelineMgr.IsRtReflectionsActive());

    WGPUTextureDescriptor hdrTd = WGPU_TEXTURE_DESCRIPTOR_INIT;
    hdrTd.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc;
    hdrTd.dimension = WGPUTextureDimension_2D;
    hdrTd.size = WGPUExtent3D{64, 64, 1};
    hdrTd.format = WGPUTextureFormat_RGBA16Float;
    hdrTd.mipLevelCount = 1;
    hdrTd.sampleCount = 1;
    TextureHandle hdrTex{wgpuDeviceCreateTexture(device, &hdrTd)};
    TextureViewHandle hdrView{hdrTex ? wgpuTextureCreateView(hdrTex.Get(), nullptr) : nullptr};
    ASSERT_TRUE(hdrView);

    const glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    const glm::mat4 view = glm::lookAt(glm::vec3{0.0f, 0.0f, 4.0f}, glm::vec3{0.0f}, glm::vec3{0.0f, 1.0f, 0.0f});
    const glm::mat4 vp = proj * view;
    const glm::vec3 camPos{0.0f, 0.0f, 4.0f};
    const glm::vec3 lightDir{0.35f, 1.0f, 0.25f};

    // 1. Exercise Hybrid RT PrePass + PostOpaque
    if (CommandEncoderHandle enc{wgpuDeviceCreateCommandEncoder(device, nullptr)}) {
        pipelineMgr.RenderRayTracingPrePass(enc, *scene, vp, camPos, lightDir, nullptr, view, proj, 64u, 64u, 0.1f,
                                            100.0f);
        EXPECT_TRUE(pipelineMgr.IsRtShadowsActive());
        EXPECT_TRUE(pipelineMgr.IsRtAoActive());
        EXPECT_NE(pipelineMgr.GetRayTracingPass().GetShadowView(), nullptr);
        EXPECT_NE(pipelineMgr.GetRayTracingPass().GetAoView(), nullptr);

        pipelineMgr.RenderRayTracingPostOpaque(enc, hdrView.Get(), vp, camPos, lightDir, 64u, 64u);
        if (CommandBufferHandle cmd{wgpuCommandEncoderFinish(enc, nullptr)}) {
            WGPUCommandBuffer rawCmd{cmd.Get()};
            wgpuQueueSubmit(queue, 1, &rawCmd);
        }
    }

    const auto hybridStats = pipelineMgr.GetRtPassStats();
    EXPECT_TRUE(hybridStats.active);
    EXPECT_FALSE(hybridStats.pathTracingActive);
    EXPECT_EQ(hybridStats.sceneStats.instanceCount, 1u);

    // 2. Exercise Progressive Path Tracer pass + sample reset
    pipelineMgr.SetRtPathTracingEnabled(true);
    EXPECT_TRUE(pipelineMgr.IsPathTracingActive());
    if (CommandEncoderHandle enc{wgpuDeviceCreateCommandEncoder(device, nullptr)}) {
        pipelineMgr.RenderPathTracePass(enc, *scene, hdrView.Get(), vp, camPos, lightDir, 64u, 64u);
        if (CommandBufferHandle cmd{wgpuCommandEncoderFinish(enc, nullptr)}) {
            WGPUCommandBuffer rawCmd{cmd.Get()};
            wgpuQueueSubmit(queue, 1, &rawCmd);
        }
    }
    EXPECT_EQ(pipelineMgr.GetRtPassStats().pathSamples, 1u);
    pipelineMgr.ResetRtPathAccumulation();
    EXPECT_EQ(pipelineMgr.GetRtPassStats().pathSamples, 0u);

    wgpuDevicePoll(device, WGPU_TRUE, nullptr);
    scene.reset();
    pipelineMgr.Shutdown();
}
#endif

} // namespace
} // namespace bgl::gpu
