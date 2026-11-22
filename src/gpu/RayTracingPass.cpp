/**
 * @file RayTracingPass.cpp
 * @brief Execution of Desktop WebGPU G-Buffer, RT Shadows, RTAO, RT Reflections, Denoisers, and Path Tracer.
 */
#include "gpu/RayTracingPass.hpp"
#include "core/Log.hpp"
#include "gpu/PipelineHelpers.hpp"
#include "scene/Scene.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>

#if defined(BGL_PLATFORM_NATIVE)
#include <webgpu/wgpu.h>
#endif

namespace bgl::gpu {

namespace {

[[nodiscard]] bool MatricesClose(const glm::mat4& a, const glm::mat4& b, float eps = 1e-5f) noexcept {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            if (std::abs(a[c][r] - b[c][r]) > eps) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

RayTracingPass::~RayTracingPass() {
    Shutdown();
}

void RayTracingPass::SetConfig(const RtConfig& cfg) noexcept {
    if (cfg.halfRes != config_.halfRes || cfg.enabled != config_.enabled || cfg.shadows != config_.shadows ||
        cfg.ambientOcclusion != config_.ambientOcclusion || cfg.reflections != config_.reflections) {
        frameIndex_ = 0;
    }
    if (cfg.pathTracing != config_.pathTracing || cfg.pathBounces != config_.pathBounces) {
        pathSampleIndex_ = 0;
    }
    config_ = cfg;
}

void RayTracingPass::SetEnvironmentCubemap(WGPUTextureView envCubemapView, uint32_t envMipCount) noexcept {
    const bool changed = (envCubemapView != envCubemapView_);
    envCubemapView_ = envCubemapView;
    envMipCount_ = std::max(1u, envMipCount);
    if (changed && initialized_) {
        RebuildPassBindGroups();
    }
}

void RayTracingPass::PopulateUniformTemplate(RtPassUniforms& out, const scene::Scene* scene, const glm::mat4& viewProj,
                                             const glm::vec3& cameraPos, const glm::vec3& sunDir, float solarElev,
                                             float envRotation, uint32_t fullW, uint32_t fullH) const noexcept {
    out = RtPassUniforms{};
    out.viewProj = viewProj;
    out.invViewProj = glm::inverse(viewProj);
    out.prevViewProj = (frameIndex_ > 1u) ? prevViewProj_ : viewProj;
    out.cameraPos = glm::vec4{cameraPos, static_cast<float>(frameIndex_)};
    const glm::vec3 safeSun =
        (glm::length(sunDir) > 1e-5f) ? glm::normalize(sunDir) : glm::normalize(glm::vec3{0.35f, 1.0f, 0.25f});
    out.sunDir = glm::vec4{safeSun, std::clamp(config_.shadowSoftness, 0.0f, 0.15f)};
    out.params0 = glm::vec4{static_cast<float>(fullW), static_cast<float>(fullH), static_cast<float>(traceWidth_),
                            static_cast<float>(traceHeight_)};
    out.params1 = glm::vec4{std::clamp(config_.aoRadius, 0.05f, 5.0f), 1.0f,
                            std::clamp(config_.reflectionIntensity, 0.0f, 2.0f), envRotation};
    out.params2 = glm::vec4{static_cast<float>(std::clamp(config_.pathBounces, 1u, 6u)),
                            static_cast<float>(pathSampleIndex_), config_.halfRes ? 1.0f : 0.0f, 1.0f};
    out.params3 = glm::vec4{0.0f, solarElev, static_cast<float>(envMipCount_), 1.0f};

    if (scene) {
        uint32_t pCount = 0u;
        bool hasExplicitDir = false;
        auto collect = [&](auto& self, const std::shared_ptr<scene::SceneNode>& node,
                           const glm::mat4& parentMat) -> void {
            if (!node || pCount >= 4u) {
                return;
            }
            const glm::mat4 worldMat = parentMat * node->localTransform;
            if (node->lightIndex >= 0 && static_cast<size_t>(node->lightIndex) < scene->lights.size()) {
                const auto& l = scene->lights[static_cast<size_t>(node->lightIndex)];
                const glm::vec3 worldPos = glm::vec3{worldMat[3]};
                const glm::vec3 rawDir = glm::vec3{worldMat * glm::vec4{0.0f, 0.0f, -1.0f, 0.0f}};
                const glm::vec3 dirToTarget =
                    (glm::length(rawDir) > 1e-5f) ? glm::normalize(rawDir) : glm::vec3{0.0f, -1.0f, 0.0f};
                if (l.type == scene::LightType::Directional) {
                    out.lightsPos[pCount] = glm::vec4{-dirToTarget, 0.0f};
                    hasExplicitDir = true;
                } else {
                    out.lightsPos[pCount] = glm::vec4{worldPos, (l.type == scene::LightType::Point) ? 1.0f : 2.0f};
                }
                out.lightsCol[pCount] = glm::vec4{l.color * l.intensity, l.range};
                ++pCount;
            }
            for (const auto& c : node->children) {
                self(self, c, worldMat);
            }
        };
        for (const auto& r : scene->rootNodes) {
            collect(collect, r, glm::mat4{1.0f});
        }
        if (pCount == 0u && !scene->lights.empty()) {
            for (const auto& l : scene->lights) {
                if (pCount >= 4u) {
                    break;
                }
                if (l.type == scene::LightType::Directional) {
                    out.lightsPos[pCount] = glm::vec4{safeSun, 0.0f};
                    hasExplicitDir = true;
                } else {
                    out.lightsPos[pCount] =
                        glm::vec4{0.0f, 2.0f, 0.0f, (l.type == scene::LightType::Point) ? 1.0f : 2.0f};
                }
                out.lightsCol[pCount] = glm::vec4{l.color * l.intensity, l.range};
                ++pCount;
            }
        }
        out.params3.x = static_cast<float>(pCount);
        out.params2.w = hasExplicitDir ? 0.0f : 1.0f;
    }
}

void RayTracingPass::WriteUniformSlot(uint32_t slotIdx, const RtPassUniforms& u) noexcept {
    if (slotIdx < kMaxUniformSlots) {
        std::memcpy(uniformStaging_.data() + static_cast<size_t>(slotIdx) * kUniformSlotStride, &u, sizeof(u));
    }
}

void RayTracingPass::DispatchComputePass(WGPUCommandEncoder encoder, WGPUComputePipeline pipe, WGPUBindGroup bg0,
                                         WGPUBindGroup bg1, WGPUBindGroup bg2, uint32_t slotIdx, uint32_t wgX,
                                         uint32_t wgY, const char* label, uint32_t startTsIdx,
                                         uint32_t endTsIdx) noexcept {
#if defined(BGL_PLATFORM_NATIVE)
    if (!encoder || !pipe || !bg0) {
        return;
    }
    WGPUComputePassDescriptor cpDesc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    cpDesc.label = ToWgpuStringView(label);
    WGPUPassTimestampWrites tsWrites = WGPU_PASS_TIMESTAMP_WRITES_INIT;
    const bool writeStart = (startTsIdx != 0xFFFFFFFFu);
    const bool writeEnd = (endTsIdx != 0xFFFFFFFFu);
    if (supportsTimestampQueries_ && timestampQuerySet_ && (writeStart || writeEnd)) {
        tsWrites.querySet = timestampQuerySet_.Get();
        tsWrites.beginningOfPassWriteIndex = writeStart ? startTsIdx : WGPU_QUERY_SET_INDEX_UNDEFINED;
        tsWrites.endOfPassWriteIndex = writeEnd ? endTsIdx : WGPU_QUERY_SET_INDEX_UNDEFINED;
        cpDesc.timestampWrites = &tsWrites;
        timestampDataWritten_ = true;
    }
    if (ComputePassEncoderHandle cpass{wgpuCommandEncoderBeginComputePass(encoder, &cpDesc)}) {
        const uint32_t dynOff = slotIdx * kUniformSlotStride;
        wgpuComputePassEncoderSetPipeline(cpass.Get(), pipe);
        wgpuComputePassEncoderSetBindGroup(cpass.Get(), 0, bg0, 1, &dynOff);
        if (bg1) {
            wgpuComputePassEncoderSetBindGroup(cpass.Get(), 1, bg1, 0, nullptr);
            if (bg2) {
                wgpuComputePassEncoderSetBindGroup(cpass.Get(), 2, bg2, 0, nullptr);
            }
        }
        wgpuComputePassEncoderDispatchWorkgroups(cpass.Get(), wgX, wgY, 1u);
        wgpuComputePassEncoderEnd(cpass.Get());
    }
#else
    (void)encoder;
    (void)pipe;
    (void)bg0;
    (void)bg1;
    (void)bg2;
    (void)slotIdx;
    (void)wgX;
    (void)wgY;
    (void)label;
    (void)startTsIdx;
    (void)endTsIdx;
#endif
}

void RayTracingPass::ExecuteDenoise8Chain(WGPUCommandEncoder encoder, WGPUComputePipeline tracePipe,
                                          WGPUBindGroup traceBg, WGPUBindGroup rtSceneBg,
                                          const DenoiseBindGroupSet& denoiseSet, uint32_t baseSlot, uint32_t traceWgX,
                                          uint32_t traceWgY, uint32_t fullWgX, uint32_t fullWgY,
                                          const char* labelPrefix, bool writeStartTs, bool writeEndTs) noexcept {
    const std::string pfx = std::string{"Pass_RT_"} + labelPrefix;
    const std::string lTrace = pfx + "_Trace";
    const std::string lTmp = pfx + "_Temporal";
    const std::string lA1 = pfx + "_Atrous1";
    const std::string lA2 = pfx + "_Atrous2";
    const std::string lA3 = pfx + "_Atrous3";
    const std::string lUp = pfx + "_Upsample";

    DispatchComputePass(encoder, tracePipe, traceBg, rtSceneBg, nullptr, baseSlot + 0u, traceWgX, traceWgY,
                        lTrace.c_str(), writeStartTs ? 0u : 0xFFFFFFFFu);
    DispatchComputePass(encoder, temporal8Pipeline_.Get(), denoiseSet.temporal[currHistIdx_].Get(), nullptr, nullptr,
                        baseSlot + 0u, traceWgX, traceWgY, lTmp.c_str());
    DispatchComputePass(encoder, atrous8Pipeline_.Get(), denoiseSet.atrousStep1[currHistIdx_].Get(), nullptr, nullptr,
                        baseSlot + 1u, traceWgX, traceWgY, lA1.c_str());
    DispatchComputePass(encoder, atrous8Pipeline_.Get(), denoiseSet.atrousStep2.Get(), nullptr, nullptr, baseSlot + 2u,
                        traceWgX, traceWgY, lA2.c_str());
    DispatchComputePass(encoder, atrous8Pipeline_.Get(), denoiseSet.atrousStep3.Get(), nullptr, nullptr, baseSlot + 3u,
                        traceWgX, traceWgY, lA3.c_str());
    DispatchComputePass(encoder, upsample8Pipeline_.Get(), denoiseSet.upsample.Get(), nullptr, nullptr, baseSlot + 3u,
                        fullWgX, fullWgY, lUp.c_str(), 0xFFFFFFFFu, writeEndTs ? 1u : 0xFFFFFFFFu);
}

void RayTracingPass::ResolveTimestampQueries(WGPUCommandEncoder encoder) noexcept {
#if defined(BGL_PLATFORM_NATIVE)
    if (supportsTimestampQueries_ && timestampQuerySet_ && timestampResolveBuf_ && timestampReadbackBuf_ &&
        timestampDataWritten_ && !timestampReadInFlight_) {
        wgpuCommandEncoderResolveQuerySet(encoder, timestampQuerySet_.Get(), 0u, 4u, timestampResolveBuf_.Get(), 0u);
        wgpuCommandEncoderCopyBufferToBuffer(encoder, timestampResolveBuf_.Get(), 0u, timestampReadbackBuf_.Get(), 0u,
                                             4u * sizeof(uint64_t));
        timestampDataWritten_ = false;
        timestampCopyQueued_ = true;
    }
#else
    (void)encoder;
#endif
}

void RayTracingPass::ExecutePrePass(WGPUCommandEncoder encoder, const RayTracingScene& rtScene, scene::Scene& scene,
                                    WGPUBindGroup sceneGlobalsBindGroup,
                                    std::span<const std::pair<const scene::SceneNode*, size_t>> opaqueItems,
                                    const glm::mat4& viewProj, const glm::mat4& lightViewProj,
                                    const glm::vec3& cameraPos, const glm::vec3& sunDir, float solarElev,
                                    float envRotation, uint32_t baseDrawFlags, uint64_t skinFrameId, float aoIntensity,
                                    uint32_t viewportW, uint32_t viewportH) {
#if defined(BGL_PLATFORM_NATIVE)
    if (!initialized_ || !config_.enabled || config_.pathTracing || !encoder || !rtScene.IsReady() ||
        (!config_.shadows && !config_.ambientOcclusion && !config_.reflections)) {
        return;
    }

    EnsureResolutionTargets(viewportW, viewportH);
    if (!gbufferDepthView_ || !gbufferNormalRoughnessView_ || !gbufferAlbedoMetallicView_ ||
        !rtScene.GetSceneBindGroup()) {
        return;
    }

    currHistIdx_ = 1u - currHistIdx_;
    ++frameIndex_;

    // 1. Render 1x Opaque G-Buffer (Normal+Roughness, Albedo+Metallic, Depth32Float)
    {
        gbufferPerDrawScratch_.clear();
        gbufferInstanceTransformsScratch_.clear();
        struct GbufDraw {
            scene::MeshPrimitive* prim{nullptr};
            WGPUBindGroup skinBg{nullptr};
            WGPURenderPipeline pipe{nullptr};
            uint32_t dynOffset{0};
            uint32_t instanceStart{0};
            uint32_t instanceCount{1};
        };
        std::vector<GbufDraw> draws;
        draws.reserve(opaqueItems.size());
        gbufferPerDrawScratch_.reserve(opaqueItems.size());

        for (const auto& [node, primIdx] : opaqueItems) {
            if (!node) {
                continue;
            }
            auto prims = const_cast<scene::SceneNode*>(node)->GetPrimitives();
            if (primIdx >= prims.size()) {
                continue;
            }
            scene::MeshPrimitive& prim = prims[primIdx];
            if (!prim.gpuVertexBuffer || !prim.gpuBindGroup) {
                continue;
            }

            PreparedDrawBindings prepared{};
            PopulatePerDrawMorphAndSkin(queue_, prim, node, node->worldTransform, viewProj, lightViewProj,
                                        baseDrawFlags, 0.0f, scene.skins, scene.allNodes, skinFrameId,
                                        dummySkinBindGroup_, prepared);

            uint32_t instStart = 0u;
            uint32_t instCount = 1u;
            if (!node->instanceTransforms.empty()) {
                instStart = static_cast<uint32_t>(1u + gbufferInstanceTransformsScratch_.size());
                instCount = static_cast<uint32_t>(node->instanceTransforms.size());
                gbufferInstanceTransformsScratch_.insert(gbufferInstanceTransformsScratch_.end(),
                                                         node->instanceTransforms.begin(),
                                                         node->instanceTransforms.end());
            }

            const bool doubleSided =
                (prim.materialIndex >= 0 && static_cast<size_t>(prim.materialIndex) < scene.materials.size())
                    ? scene.materials[static_cast<size_t>(prim.materialIndex)].doubleSided
                    : false;

            const uint32_t dynOff = static_cast<uint32_t>(gbufferPerDrawScratch_.size() * sizeof(PerDrawUniforms));
            gbufferPerDrawScratch_.push_back(prepared.uniforms);
            draws.push_back(GbufDraw{
                .prim = &prim,
                .skinBg = prepared.skinBindGroup,
                .pipe = doubleSided ? gbufferPipeline_.Get() : gbufferBackCulledPipeline_.Get(),
                .dynOffset = dynOff,
                .instanceStart = instStart,
                .instanceCount = instCount,
            });
        }

        gbufferInstanceBuffer_.Upload(queue_, gbufferInstanceTransformsScratch_);
        WGPUBindGroup perDrawBg = gbufferPerDrawRing_.Upload(gbufferPerDrawScratch_);

        std::array<WGPURenderPassColorAttachment, 2> colorAtts{};
        colorAtts[0] = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        colorAtts[0].view = gbufferNormalRoughnessView_.Get();
        colorAtts[0].loadOp = WGPULoadOp_Clear;
        colorAtts[0].storeOp = WGPUStoreOp_Store;
        colorAtts[0].clearValue = WGPUColor{0.0, 1.0, 0.0, 1.0};

        colorAtts[1] = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        colorAtts[1].view = gbufferAlbedoMetallicView_.Get();
        colorAtts[1].loadOp = WGPULoadOp_Clear;
        colorAtts[1].storeOp = WGPUStoreOp_Store;
        colorAtts[1].clearValue = WGPUColor{0.0, 0.0, 0.0, 0.0};

        WGPURenderPassDepthStencilAttachment depthAtt = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
        depthAtt.view = gbufferDepthView_.Get();
        depthAtt.depthLoadOp = WGPULoadOp_Clear;
        depthAtt.depthStoreOp = WGPUStoreOp_Store;
        depthAtt.depthClearValue = 1.0f;
        depthAtt.depthReadOnly = WGPU_FALSE;

        WGPURenderPassDescriptor rpDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rpDesc.colorAttachmentCount = colorAtts.size();
        rpDesc.colorAttachments = colorAtts.data();
        rpDesc.depthStencilAttachment = &depthAtt;
        rpDesc.label = ToWgpuStringView("RT_GBufferPass");

        if (RenderPassEncoderHandle pass{wgpuCommandEncoderBeginRenderPass(encoder, &rpDesc)}) {
            if (perDrawBg && sceneGlobalsBindGroup && gbufferInstanceBuffer_.GetBuffer()) {
                for (const auto& dc : draws) {
                    const auto lod0 = dc.prim->GetLodSlice(0);
                    wgpuRenderPassEncoderSetPipeline(pass.Get(), dc.pipe);
                    wgpuRenderPassEncoderSetBindGroup(pass.Get(), 0, sceneGlobalsBindGroup, 0, nullptr);
                    wgpuRenderPassEncoderSetBindGroup(pass.Get(), 1, perDrawBg, 1, &dc.dynOffset);
                    wgpuRenderPassEncoderSetBindGroup(pass.Get(), 2, dc.prim->gpuBindGroup, 0, nullptr);
                    wgpuRenderPassEncoderSetBindGroup(pass.Get(), 3, dc.skinBg, 0, nullptr);
                    wgpuRenderPassEncoderSetVertexBuffer(pass.Get(), 0, dc.prim->gpuVertexBuffer, 0,
                                                         dc.prim->gpuVertexBufferSize);
                    wgpuRenderPassEncoderSetVertexBuffer(pass.Get(), 1, gbufferInstanceBuffer_.GetBuffer(),
                                                         DynamicInstanceBuffer::GetByteOffset(dc.instanceStart),
                                                         DynamicInstanceBuffer::GetSliceSize(dc.instanceCount));
                    if (dc.prim->gpuIndexBuffer && lod0.indexCount > 0) {
                        const uint64_t idxByteOffset = static_cast<uint64_t>(lod0.firstIndex) *
                                                       ((dc.prim->indexFormat == WGPUIndexFormat_Uint16) ? 2u : 4u);
                        wgpuRenderPassEncoderSetIndexBuffer(pass.Get(), dc.prim->gpuIndexBuffer, dc.prim->indexFormat,
                                                            idxByteOffset, dc.prim->gpuIndexBufferSize - idxByteOffset);
                        wgpuRenderPassEncoderDrawIndexed(pass.Get(), lod0.indexCount, dc.instanceCount, 0, 0, 0);
                    } else if (!dc.prim->vertices.empty()) {
                        wgpuRenderPassEncoderDraw(pass.Get(), static_cast<uint32_t>(dc.prim->vertices.size()),
                                                  dc.instanceCount, 0, 0);
                    }
                }
            }
            wgpuRenderPassEncoderEnd(pass.Get());
        }
    }

    // 2. Stage dynamic uniform slots 0..7 for Shadows & RTAO
    RtPassUniforms baseUbo{};
    PopulateUniformTemplate(baseUbo, &scene, viewProj, cameraPos, sunDir, solarElev, envRotation, fullWidth_,
                            fullHeight_);
    baseUbo.params1.y = aoIntensity;

    auto writeStrides = [&](uint32_t baseSlot) {
        baseUbo.params3.w = 1.0f;
        WriteUniformSlot(baseSlot + 0u, baseUbo);
        WriteUniformSlot(baseSlot + 1u, baseUbo);
        baseUbo.params3.w = 2.0f;
        WriteUniformSlot(baseSlot + 2u, baseUbo);
        baseUbo.params3.w = 4.0f;
        WriteUniformSlot(baseSlot + 3u, baseUbo);
    };
    writeStrides(0u);
    writeStrides(4u);

    wgpuQueueWriteBuffer(queue_, passUniformBuffer_.Get(), 0, uniformStaging_.data(), 8u * kUniformSlotStride);

    const uint32_t traceWgX = (traceWidth_ + 7u) / 8u;
    const uint32_t traceWgY = (traceHeight_ + 7u) / 8u;
    const uint32_t fullWgX = (fullWidth_ + 7u) / 8u;
    const uint32_t fullWgY = (fullHeight_ + 7u) / 8u;
    const WGPUBindGroup rtSceneBg = rtScene.GetSceneBindGroup();

    if (config_.shadows) {
        ExecuteDenoise8Chain(encoder, shadowPipeline_.Get(), shadowTraceBg_.Get(), rtSceneBg, shadowBgs_, 0u,
                             traceWgX, traceWgY, fullWgX, fullWgY, "Shadow", true, !config_.ambientOcclusion);
    }
    if (config_.ambientOcclusion) {
        ExecuteDenoise8Chain(encoder, aoPipeline_.Get(), aoTraceBg_.Get(), rtSceneBg, aoBgs_, 4u, traceWgX, traceWgY,
                             fullWgX, fullWgY, "Ao", !config_.shadows, true);
    }

    timestampHasPrePass_ = (config_.shadows || config_.ambientOcclusion);
    if (!config_.reflections) {
        timestampHasPostPass_ = false;
        ResolveTimestampQueries(encoder);
        prevViewProj_ = viewProj;
    }
#else
    (void)encoder;
    (void)rtScene;
    (void)scene;
    (void)sceneGlobalsBindGroup;
    (void)opaqueItems;
    (void)viewProj;
    (void)lightViewProj;
    (void)cameraPos;
    (void)sunDir;
    (void)solarElev;
    (void)envRotation;
    (void)baseDrawFlags;
    (void)skinFrameId;
    (void)aoIntensity;
    (void)viewportW;
    (void)viewportH;
#endif
}

void RayTracingPass::ExecutePostOpaque(WGPUCommandEncoder encoder, const RayTracingScene& rtScene,
                                       WGPUTextureView hdrResolveView, const glm::mat4& viewProj,
                                       const glm::vec3& cameraPos, const glm::vec3& sunDir, float solarElev,
                                       float envRotation, uint32_t viewportW, uint32_t viewportH) {
#if defined(BGL_PLATFORM_NATIVE)
    if (!initialized_ || !config_.enabled || config_.pathTracing || !encoder || !hdrResolveView || !rtScene.IsReady()) {
        return;
    }

    if (config_.reflections && gbufferDepthView_ && reflTraceBg_) {
        EnsureResolutionTargets(viewportW, viewportH);

        RtPassUniforms reflUbo{};
        PopulateUniformTemplate(reflUbo, nullptr, viewProj, cameraPos, sunDir, solarElev, envRotation, fullWidth_,
                                fullHeight_);
        reflUbo.params3.w = 1.0f;
        WriteUniformSlot(8u, reflUbo);
        WriteUniformSlot(9u, reflUbo);
        reflUbo.params3.w = 2.0f;
        WriteUniformSlot(10u, reflUbo);
        reflUbo.params3.w = 4.0f;
        WriteUniformSlot(11u, reflUbo);

        wgpuQueueWriteBuffer(queue_, passUniformBuffer_.Get(), 8u * kUniformSlotStride,
                             uniformStaging_.data() + 8u * kUniformSlotStride, 4u * kUniformSlotStride);

        const uint32_t traceWgX = (traceWidth_ + 7u) / 8u;
        const uint32_t traceWgY = (traceHeight_ + 7u) / 8u;
        const WGPUBindGroup rtSceneBg = rtScene.GetSceneBindGroup();
        const WGPUBindGroup bindlessBg = bindlessActive_ ? rtScene.GetBindlessTexturesBindGroup() : nullptr;

        DispatchComputePass(encoder, reflPipeline_.Get(), reflTraceBg_.Get(), rtSceneBg, bindlessBg, 8u, traceWgX,
                            traceWgY, "Pass_RT_Reflections", 2u);
        DispatchComputePass(encoder, temporal16Pipeline_.Get(), reflBgs_.temporal[currHistIdx_].Get(), nullptr, nullptr,
                            8u, traceWgX, traceWgY, "Pass_RT_ReflTemporal");
        DispatchComputePass(encoder, atrous16Pipeline_.Get(), reflBgs_.atrousStep1[currHistIdx_].Get(), nullptr,
                            nullptr, 9u, traceWgX, traceWgY, "Pass_RT_ReflAtrous1");
        DispatchComputePass(encoder, atrous16Pipeline_.Get(), reflBgs_.atrousStep2.Get(), nullptr, nullptr, 10u,
                            traceWgX, traceWgY, "Pass_RT_ReflAtrous2");
        DispatchComputePass(encoder, atrous16Pipeline_.Get(), reflBgs_.atrousStep3.Get(), nullptr, nullptr, 11u,
                            traceWgX, traceWgY, "Pass_RT_ReflAtrous3", 0xFFFFFFFFu, 3u);
        timestampHasPostPass_ = true;

        // Composite denoised HDR reflections onto hdrResolveView
        if (compositeReflPipeline_ && compositeReflBg_) {
            const RtCompositeUniforms compU{
                .params = glm::vec4{static_cast<float>(fullWidth_), static_cast<float>(fullHeight_),
                                    static_cast<float>(traceWidth_), static_cast<float>(traceHeight_)},
            };
            wgpuQueueWriteBuffer(queue_, compositeUniformBuffer_.Get(), 0, &compU, sizeof(compU));

            WGPURenderPassColorAttachment colorAtt = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
            colorAtt.view = hdrResolveView;
            colorAtt.loadOp = WGPULoadOp_Load;
            colorAtt.storeOp = WGPUStoreOp_Store;
            colorAtt.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

            WGPURenderPassDescriptor rpDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
            rpDesc.colorAttachmentCount = 1;
            rpDesc.colorAttachments = &colorAtt;
            rpDesc.label = ToWgpuStringView("Pass_RT_CompositeReflections");

            if (RenderPassEncoderHandle pass{wgpuCommandEncoderBeginRenderPass(encoder, &rpDesc)}) {
                wgpuRenderPassEncoderSetPipeline(pass.Get(), compositeReflPipeline_.Get());
                wgpuRenderPassEncoderSetBindGroup(pass.Get(), 0, compositeReflBg_.Get(), 0, nullptr);
                wgpuRenderPassEncoderDraw(pass.Get(), 3u, 1u, 0u, 0u);
                wgpuRenderPassEncoderEnd(pass.Get());
            }
        }
    }

    ResolveTimestampQueries(encoder);

    prevViewProj_ = viewProj;
#else
    (void)encoder;
    (void)rtScene;
    (void)hdrResolveView;
    (void)viewProj;
    (void)cameraPos;
    (void)sunDir;
    (void)solarElev;
    (void)envRotation;
    (void)viewportW;
    (void)viewportH;
#endif
}

void RayTracingPass::ExecutePathTrace(WGPUCommandEncoder encoder, const RayTracingScene& rtScene,
                                      const scene::Scene& scene, WGPUTextureView hdrResolveView,
                                      const glm::mat4& viewProj, const glm::vec3& cameraPos, const glm::vec3& sunDir,
                                      float solarElev, float envRotation, uint32_t viewportW, uint32_t viewportH) {
#if defined(BGL_PLATFORM_NATIVE)
    if (!initialized_ || !config_.enabled || !config_.pathTracing || !encoder || !hdrResolveView ||
        !rtScene.IsReady() || !pathTracePipeline_ || !pathTraceBg_) {
        return;
    }

    EnsureResolutionTargets(viewportW, viewportH);

    const bool dirty = (&scene != lastPathScene_) || (scene.transformVersion_ != lastPathTransformVer_) ||
                       !MatricesClose(viewProj, lastPathViewProj_) || (glm::length(sunDir - lastPathSunDir_) > 1e-4f) ||
                       (std::abs(envRotation - lastPathEnvRot_) > 1e-4f) || (config_.pathBounces != lastPathBounces_);
    if (dirty) {
        pathSampleIndex_ = 0;
        lastPathScene_ = &scene;
        lastPathTransformVer_ = scene.transformVersion_;
        lastPathViewProj_ = viewProj;
        lastPathSunDir_ = sunDir;
        lastPathEnvRot_ = envRotation;
        lastPathBounces_ = config_.pathBounces;
    }

    RtPassUniforms ptUbo{};
    PopulateUniformTemplate(ptUbo, &scene, viewProj, cameraPos, sunDir, solarElev, envRotation, fullWidth_,
                            fullHeight_);
    WriteUniformSlot(12u, ptUbo);
    wgpuQueueWriteBuffer(queue_, passUniformBuffer_.Get(), 12u * kUniformSlotStride,
                         uniformStaging_.data() + 12u * kUniformSlotStride, kUniformSlotStride);

    const uint32_t fullWgX = (fullWidth_ + 7u) / 8u;
    const uint32_t fullWgY = (fullHeight_ + 7u) / 8u;
    const WGPUBindGroup rtSceneBg = rtScene.GetSceneBindGroup();
    const WGPUBindGroup bindlessBg = bindlessActive_ ? rtScene.GetBindlessTexturesBindGroup() : nullptr;

    timestampHasPrePass_ = false;
    timestampHasPostPass_ = true;
    DispatchComputePass(encoder, pathTracePipeline_.Get(), pathTraceBg_.Get(), rtSceneBg, bindlessBg, 12u, fullWgX,
                        fullWgY, "Pass_RT_PathTrace", 2u, 3u);

    if (pathSampleIndex_ < 4096u) {
        ++pathSampleIndex_;
    }

    if (pathTracePresentPipeline_ && pathPresentBg_) {
        const RtCompositeUniforms compU{
            .params = glm::vec4{static_cast<float>(fullWidth_), static_cast<float>(fullHeight_),
                                static_cast<float>(fullWidth_), static_cast<float>(fullHeight_)},
        };
        wgpuQueueWriteBuffer(queue_, compositeUniformBuffer_.Get(), 0, &compU, sizeof(compU));

        WGPURenderPassColorAttachment colorAtt = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        colorAtt.view = hdrResolveView;
        colorAtt.loadOp = WGPULoadOp_Clear;
        colorAtt.storeOp = WGPUStoreOp_Store;
        colorAtt.clearValue = WGPUColor{0.0, 0.0, 0.0, 1.0};
        colorAtt.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

        WGPURenderPassDescriptor rpDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rpDesc.colorAttachmentCount = 1;
        rpDesc.colorAttachments = &colorAtt;
        rpDesc.label = ToWgpuStringView("Pass_RT_PathTracePresent");

        if (RenderPassEncoderHandle pass{wgpuCommandEncoderBeginRenderPass(encoder, &rpDesc)}) {
            wgpuRenderPassEncoderSetPipeline(pass.Get(), pathTracePresentPipeline_.Get());
            wgpuRenderPassEncoderSetBindGroup(pass.Get(), 0, pathPresentBg_.Get(), 0, nullptr);
            wgpuRenderPassEncoderDraw(pass.Get(), 3u, 1u, 0u, 0u);
            wgpuRenderPassEncoderEnd(pass.Get());
        }
    }

    ResolveTimestampQueries(encoder);

    prevViewProj_ = viewProj;
#else
    (void)encoder;
    (void)rtScene;
    (void)scene;
    (void)hdrResolveView;
    (void)viewProj;
    (void)cameraPos;
    (void)sunDir;
    (void)solarElev;
    (void)envRotation;
    (void)viewportW;
    (void)viewportH;
#endif
}

void RayTracingPass::PollGpuTimestamps() noexcept {
#if defined(BGL_PLATFORM_NATIVE)
    if (!supportsTimestampQueries_ || !timestampReadbackBuf_ || !device_) {
        return;
    }
    if (timestampReadInFlight_) {
        wgpuDevicePoll(device_, WGPU_FALSE, nullptr);
        return;
    }
    if (!timestampCopyQueued_) {
        return;
    }
    timestampCopyQueued_ = false;

    WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.userdata1 = this;
    cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* ud1, void*) {
        auto* self = static_cast<RayTracingPass*>(ud1);
        if (!self) {
            return;
        }
        if (status == WGPUMapAsyncStatus_Success && self->timestampReadbackBuf_) {
            const void* ptr =
                wgpuBufferGetConstMappedRange(self->timestampReadbackBuf_.Get(), 0u, 4u * sizeof(uint64_t));
            if (ptr) {
                std::array<uint64_t, 4> ts{};
                std::memcpy(ts.data(), ptr, sizeof(ts));
                double totalNs = 0.0;
                if (self->timestampHasPrePass_ && ts[1] > ts[0]) {
                    totalNs += static_cast<double>(ts[1] - ts[0]) * static_cast<double>(self->timestampPeriodNs_);
                }
                if (self->timestampHasPostPass_ && ts[3] > ts[2]) {
                    totalNs += static_cast<double>(ts[3] - ts[2]) * static_cast<double>(self->timestampPeriodNs_);
                }
                const float ms = static_cast<float>(totalNs * 1e-6);
                if (std::isfinite(ms) && ms >= 0.0f && ms < 250.0f) {
                    self->lastGpuMs_ = self->lastGpuMs_ * 0.85f + ms * 0.15f;
                }
            }
            wgpuBufferUnmap(self->timestampReadbackBuf_.Get());
        }
        self->timestampReadInFlight_ = false;
    };
    timestampReadInFlight_ = true;
    wgpuBufferMapAsync(timestampReadbackBuf_.Get(), WGPUMapMode_Read, 0u, 4u * sizeof(uint64_t), cb);
#endif
}

RtPassStats RayTracingPass::GetStats(const RayTracingScene& rtScene) const noexcept {
    RtPassStats s{};
    s.active = initialized_ && config_.enabled && rtScene.IsReady();
    s.bindlessActive = bindlessActive_;
    s.pathTracingActive = IsPathTracingActive(rtScene);
    s.pathSamples = pathSampleIndex_;
    s.lastGpuMs = lastGpuMs_;
    s.sceneStats = rtScene.GetStats();
    return s;
}

} // namespace bgl::gpu
