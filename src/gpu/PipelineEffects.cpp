/**
 * @file PipelineEffects.cpp
 * @brief GPU/CPU mipmap chain generation, opaque transmission copy mips, and 3D selection silhouette outlines.
 */
#include "core/Log.hpp"
#include "core/MemoryTracker.hpp"
#include "gpu/DrawHelpers.hpp"
#include "gpu/Pipeline.hpp"
#include "gpu/PipelineHelpers.hpp"
#include "gpu/ShaderManager.hpp"
#include "scene/Scene.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <vector>

namespace bgl::gpu {
using namespace detail;

namespace {

void EncodeGpuMipChain(WGPUDevice device, WGPUBindGroupLayout mipGenBgl, WGPUCommandEncoder encoder,
                       WGPUTexture texture, uint32_t mipLevels, WGPUTextureFormat fmt, WGPURenderPipeline pipeline,
                       const char* passLabel) {
    if (!encoder || !device || !mipGenBgl || !pipeline || !texture || mipLevels <= 1) {
        return;
    }
    for (uint32_t level{1}; level < mipLevels; ++level) {
        WGPUTextureViewDescriptor srcDesc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        srcDesc.format = fmt;
        srcDesc.dimension = WGPUTextureViewDimension_2D;
        srcDesc.baseMipLevel = level - 1;
        srcDesc.mipLevelCount = 1;
        srcDesc.baseArrayLayer = 0;
        srcDesc.arrayLayerCount = 1;
        srcDesc.aspect = WGPUTextureAspect_All;
        TextureViewHandle srcView{wgpuTextureCreateView(texture, &srcDesc)};

        WGPUTextureViewDescriptor dstDesc = srcDesc;
        dstDesc.baseMipLevel = level;
        TextureViewHandle dstView{wgpuTextureCreateView(texture, &dstDesc)};
        if (!srcView || !dstView) {
            break;
        }

        WGPUBindGroupEntry bgEntry = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntry.binding = 0;
        bgEntry.textureView = srcView.Get();

        WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgDesc.layout = mipGenBgl;
        bgDesc.entryCount = 1;
        bgDesc.entries = &bgEntry;
        BindGroupHandle srcBg{wgpuDeviceCreateBindGroup(device, &bgDesc)};
        if (!srcBg) {
            break;
        }

        WGPURenderPassColorAttachment colorAtt = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        colorAtt.view = dstView.Get();
        colorAtt.loadOp = WGPULoadOp_Clear;
        colorAtt.storeOp = WGPUStoreOp_Store;
        colorAtt.clearValue = WGPUColor{0.0, 0.0, 0.0, 1.0};

        WGPURenderPassDescriptor rpDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rpDesc.colorAttachmentCount = 1;
        rpDesc.colorAttachments = &colorAtt;
        rpDesc.label = ToWgpuStringView(passLabel);

        if (RenderPassEncoderHandle pass{wgpuCommandEncoderBeginRenderPass(encoder, &rpDesc)}) {
            wgpuRenderPassEncoderSetPipeline(pass.Get(), pipeline);
            wgpuRenderPassEncoderSetBindGroup(pass.Get(), 0, srcBg.Get(), 0, nullptr);
            wgpuRenderPassEncoderDraw(pass.Get(), 3, 1, 0, 0);
            wgpuRenderPassEncoderEnd(pass.Get());
        }
    }
}

} // namespace

uint32_t PipelineManager::CalculateMipLevels(uint32_t width, uint32_t height) noexcept {
    const uint32_t maxDim{std::max(width, height)};
    return std::max(1u, static_cast<uint32_t>(std::bit_width(maxDim)));
}

std::vector<uint8_t> PipelineManager::GenerateNextMipLevel(const uint8_t* src, uint32_t srcW, uint32_t srcH,
                                                           uint32_t dstW, uint32_t dstH, bool isNormalMap) {
    if (!src || srcW == 0 || srcH == 0) {
        return std::vector<uint8_t>(static_cast<size_t>(dstW) * dstH * 4);
    }
    return GenerateNextMipLevel(std::span<const uint8_t>{src, static_cast<size_t>(srcW) * srcH * 4}, srcW, srcH, dstW,
                                dstH, isNormalMap);
}

std::vector<uint8_t> PipelineManager::GenerateNextMipLevel(std::span<const uint8_t> src, uint32_t srcW, uint32_t srcH,
                                                           uint32_t dstW, uint32_t dstH, bool isNormalMap) {
    std::vector<uint8_t> dst(static_cast<size_t>(dstW) * dstH * 4);
    if (src.empty() || srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0 ||
        src.size() < static_cast<size_t>(srcW) * srcH * 4) {
        return dst;
    }

    for (uint32_t y{0}; y < dstH; ++y) {
        for (uint32_t x{0}; x < dstW; ++x) {
            const uint32_t srcX0{std::min(x * 2, srcW - 1)};
            const uint32_t srcX1{std::min(x * 2 + 1, srcW - 1)};
            const uint32_t srcY0{std::min(y * 2, srcH - 1)};
            const uint32_t srcY1{std::min(y * 2 + 1, srcH - 1)};

            std::span<const uint8_t> p00{src.subspan((static_cast<size_t>(srcY0) * srcW + srcX0) * 4, 4)};
            std::span<const uint8_t> p10{src.subspan((static_cast<size_t>(srcY0) * srcW + srcX1) * 4, 4)};
            std::span<const uint8_t> p01{src.subspan((static_cast<size_t>(srcY1) * srcW + srcX0) * 4, 4)};
            std::span<const uint8_t> p11{src.subspan((static_cast<size_t>(srcY1) * srcW + srcX1) * 4, 4)};

            const size_t dstIdx{(static_cast<size_t>(y) * dstW + x) * 4};
            if (isNormalMap) {
                auto unpackN = [](std::span<const uint8_t> p) -> glm::vec3 {
                    return glm::vec3{(static_cast<float>(p[0]) / 255.0f) * 2.0f - 1.0f,
                                     (static_cast<float>(p[1]) / 255.0f) * 2.0f - 1.0f,
                                     (static_cast<float>(p[2]) / 255.0f) * 2.0f - 1.0f};
                };
                glm::vec3 avgN{unpackN(p00) + unpackN(p10) + unpackN(p01) + unpackN(p11)};
                const float len{glm::length(avgN)};
                avgN = (len > 1e-5f) ? (avgN / len) : glm::vec3{0.0f, 0.0f, 1.0f};
                dst[dstIdx + 0] =
                    static_cast<uint8_t>(std::clamp((avgN.x * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
                dst[dstIdx + 1] =
                    static_cast<uint8_t>(std::clamp((avgN.y * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
                dst[dstIdx + 2] =
                    static_cast<uint8_t>(std::clamp((avgN.z * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
                const uint32_t sumA{static_cast<uint32_t>(p00[3]) + p10[3] + p01[3] + p11[3]};
                dst[dstIdx + 3] = static_cast<uint8_t>((sumA + 2) / 4);
            } else {
                for (int c{0}; c < 4; ++c) {
                    const uint32_t sum{static_cast<uint32_t>(p00[c]) + static_cast<uint32_t>(p10[c]) +
                                       static_cast<uint32_t>(p01[c]) + static_cast<uint32_t>(p11[c])};
                    dst[dstIdx + c] = static_cast<uint8_t>((sum + 2) / 4);
                }
            }
        }
    }
    return dst;
}

void PipelineManager::InitMipGenPipelines() {
    if (!device_) {
        return;
    }

    WGPUBindGroupLayoutEntry bglEntry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntry.binding = 0;
    bglEntry.visibility = WGPUShaderStage_Fragment;
    bglEntry.texture.sampleType = WGPUTextureSampleType_Float;
    bglEntry.texture.viewDimension = WGPUTextureViewDimension_2D;
    bglEntry.texture.multisampled = WGPU_FALSE;

    WGPUBindGroupLayoutDescriptor bglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bglDesc.entryCount = 1;
    bglDesc.entries = &bglEntry;
    bglDesc.label = ToWgpuStringView("MipGen_BGL");
    mipGenBgl_ = wgpuDeviceCreateBindGroupLayout(device_, &bglDesc);
    if (!mipGenBgl_) {
        return;
    }

    WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = mipGenBgl_.AddressOf();
    plDesc.label = ToWgpuStringView("MipGen_PL");
    mipGenPipelineLayout_ = wgpuDeviceCreatePipelineLayout(device_, &plDesc);
    if (!mipGenPipelineLayout_) {
        return;
    }

    if (auto res = CreateMipGenPipelines(false); !res) {
        BGL_LOG_WARN("Failed to compile MipGen WGSL module ({}); uncompressed textures will skip GPU mips",
                     res.error());
    }
}

core::Result<void> PipelineManager::CreateMipGenPipelines(bool validateScope) {
    if (!device_ || !mipGenPipelineLayout_) {
        return std::unexpected("Cannot create MipGen pipelines without device or pipeline layout");
    }

    auto shaderModuleRes =
        ShaderManager::Instance().LoadAndCompile(device_, "mipgen.wgsl", "MipGen_WGSL", validateScope);
    if (!shaderModuleRes) {
        return std::unexpected(shaderModuleRes.error());
    }

    auto createMipPipeline = [&](WGPUTextureFormat fmt, std::string_view fsEntry,
                                 std::string_view label) -> RenderPipelineHandle {
        WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
        colorTarget.format = fmt;
        colorTarget.writeMask = WGPUColorWriteMask_All;

        WGPUFragmentState fragState = WGPU_FRAGMENT_STATE_INIT;
        fragState.module = shaderModuleRes->Get();
        fragState.entryPoint = ToWgpuStringView(fsEntry);
        fragState.targetCount = 1;
        fragState.targets = &colorTarget;

        WGPURenderPipelineDescriptor rpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        rpDesc.layout = mipGenPipelineLayout_.Get();
        rpDesc.vertex.module = shaderModuleRes->Get();
        rpDesc.vertex.entryPoint = ToWgpuStringView("vs_mip");
        rpDesc.fragment = &fragState;
        rpDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        rpDesc.primitive.cullMode = WGPUCullMode_None;
        rpDesc.multisample.count = 1;
        rpDesc.multisample.mask = 0xFFFFFFFF;
        rpDesc.label = ToWgpuStringView(label);
        return RenderPipelineHandle{wgpuDeviceCreateRenderPipeline(device_, &rpDesc)};
    };

    RenderPipelineHandle nextUnorm = createMipPipeline(WGPUTextureFormat_RGBA8Unorm, "fs_mip", "MipGen_RGBA8Unorm");
    RenderPipelineHandle nextSrgb =
        createMipPipeline(WGPUTextureFormat_RGBA8UnormSrgb, "fs_mip", "MipGen_RGBA8UnormSrgb");
    RenderPipelineHandle nextNormal = createMipPipeline(WGPUTextureFormat_RGBA8Unorm, "fs_mip_normal", "MipGen_Normal");
    RenderPipelineHandle nextHdr = createMipPipeline(WGPUTextureFormat_RGBA16Float, "fs_mip", "MipGen_RGBA16Float");

    if (!nextUnorm || !nextSrgb || !nextNormal || !nextHdr) {
        return std::unexpected("Failed to create MipGen render pipelines");
    }

    mipGenUnormPipeline_ = std::move(nextUnorm);
    mipGenSrgbPipeline_ = std::move(nextSrgb);
    mipGenNormalPipeline_ = std::move(nextNormal);
    mipGenHdrPipeline_ = std::move(nextHdr);
    return {};
}

void PipelineManager::GenerateTextureMipsGpu(WGPUCommandEncoder encoder, WGPUTexture texture, uint32_t width,
                                             uint32_t height, uint32_t mipLevels, bool isSrgb, bool isNormalMap) {
    (void)width;
    (void)height;
    if (!encoder || !device_ || !mipGenBgl_ || !texture || mipLevels <= 1) {
        return;
    }

    WGPURenderPipeline pipeline{nullptr};
    if (isNormalMap) {
        pipeline = mipGenNormalPipeline_.Get();
    } else if (isSrgb) {
        pipeline = mipGenSrgbPipeline_.Get();
    } else {
        pipeline = mipGenUnormPipeline_.Get();
    }
    if (!pipeline) {
        return;
    }

    const WGPUTextureFormat fmt{isSrgb ? WGPUTextureFormat_RGBA8UnormSrgb : WGPUTextureFormat_RGBA8Unorm};
    EncodeGpuMipChain(device_, mipGenBgl_.Get(), encoder, texture, mipLevels, fmt, pipeline, "GpuMipGenPass");
}

void PipelineManager::EnsureOpaqueCopyTexture(uint32_t width, uint32_t height) {
    width = std::max(1u, width);
    height = std::max(1u, height);
    if (opaqueColorCopyTexture_ && opaqueCopyWidth_ == width && opaqueCopyHeight_ == height) {
        return;
    }
    opaqueCopyWidth_ = width;
    opaqueCopyHeight_ = height;
    opaqueCopyMipCount_ = std::min(5u, CalculateMipLevels(width, height));
    opaqueColorCopyView_.Reset();
    opaqueColorCopyTexture_.Reset();

    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst | WGPUTextureUsage_RenderAttachment;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = WGPUExtent3D{opaqueCopyWidth_, opaqueCopyHeight_, 1};
    desc.format = WGPUTextureFormat_RGBA16Float;
    desc.mipLevelCount = opaqueCopyMipCount_;
    desc.sampleCount = 1;
    desc.label = ToWgpuStringView("OpaqueColorCopyTexture");
    opaqueColorCopyTexture_ = wgpuDeviceCreateTexture(device_, &desc);
    if (opaqueColorCopyTexture_) {
        opaqueColorCopyView_ = wgpuTextureCreateView(opaqueColorCopyTexture_, nullptr);
    }
    RebuildSceneGlobalsBindGroup();
}

void PipelineManager::GenerateOpaqueCopyMips(WGPUCommandEncoder encoder) {
    EncodeGpuMipChain(device_, mipGenBgl_.Get(), encoder, opaqueColorCopyTexture_.Get(), opaqueCopyMipCount_,
                      WGPUTextureFormat_RGBA16Float, mipGenHdrPipeline_.Get(), "OpaqueCopyHdrMipGenPass");
}

core::Result<void> PipelineManager::CreateSelectionOutlinePipelines(bool validateScope) {
    if (!device_ || !mainPipelineLayout_) {
        return std::unexpected("Cannot create selection outline pipelines without initialized main layout");
    }

    auto maskModRes =
        ShaderManager::Instance().LoadAndCompile(device_, "outline.wgsl", "OutlineMask_WGSL", validateScope);
    auto compModRes =
        ShaderManager::Instance().LoadAndCompile(device_, "outline_comp.wgsl", "OutlineComp_WGSL", validateScope);
    if (!maskModRes || !compModRes) {
        return std::unexpected(!maskModRes ? maskModRes.error() : compModRes.error());
    }

    if (!outlineCompBgl_) {
        std::array<WGPUBindGroupLayoutEntry, 2> entries{};
        entries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        entries[0].binding = 0;
        entries[0].visibility = WGPUShaderStage_Fragment;
        entries[0].buffer.type = WGPUBufferBindingType_Uniform;
        entries[0].buffer.minBindingSize = sizeof(glm::vec4) * 3u;

        entries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        entries[1].binding = 1;
        entries[1].visibility = WGPUShaderStage_Fragment;
        entries[1].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
        entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;

        WGPUBindGroupLayoutDescriptor bglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        bglDesc.entryCount = entries.size();
        bglDesc.entries = entries.data();
        bglDesc.label = ToWgpuStringView("OutlineComp_BGL");
        outlineCompBgl_ = wgpuDeviceCreateBindGroupLayout(device_, &bglDesc);
        if (!outlineCompBgl_) {
            return std::unexpected("Failed to create OutlineComp bind group layout");
        }
    }
    if (!outlineCompPl_) {
        const WGPUBindGroupLayout raw{outlineCompBgl_.Get()};
        WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        plDesc.bindGroupLayoutCount = 1;
        plDesc.bindGroupLayouts = &raw;
        plDesc.label = ToWgpuStringView("OutlineComp_PL");
        outlineCompPl_ = wgpuDeviceCreatePipelineLayout(device_, &plDesc);
    }
    if (!outlineCompUniformBuffer_) {
        WGPUBufferDescriptor ubDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
        ubDesc.size = sizeof(glm::vec4) * 3u;
        ubDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        ubDesc.label = ToWgpuStringView("OutlineComp_Uniforms");
        outlineCompUniformBuffer_ = wgpuDeviceCreateBuffer(device_, &ubDesc);
        if (outlineCompUniformBuffer_) {
            core::MemoryTracker::Instance().TrackBufferAlloc(ubDesc.size);
        }
    }

    VertexAndInstanceLayoutStorage layoutStorage;
    FillVertexAndInstanceLayouts(layoutStorage);

    WGPUColorTargetState maskTarget = WGPU_COLOR_TARGET_STATE_INIT;
    maskTarget.format = WGPUTextureFormat_RGBA8Unorm;
    maskTarget.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState maskFrag = WGPU_FRAGMENT_STATE_INIT;
    maskFrag.module = maskModRes->Get();
    maskFrag.entryPoint = ToWgpuStringView("fs_outline_mask");
    maskFrag.targetCount = 1;
    maskFrag.targets = &maskTarget;

    WGPURenderPipelineDescriptor maskDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    maskDesc.layout = mainPipelineLayout_.Get();
    maskDesc.vertex.module = maskModRes->Get();
    maskDesc.vertex.entryPoint = ToWgpuStringView("vs_outline_mask");
    maskDesc.vertex.bufferCount = static_cast<uint32_t>(layoutStorage.layouts.size());
    maskDesc.vertex.buffers = layoutStorage.layouts.data();
    maskDesc.fragment = &maskFrag;
    maskDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    maskDesc.primitive.cullMode = WGPUCullMode_None;
    maskDesc.multisample.count = 1;
    maskDesc.multisample.mask = 0xFFFFFFFF;
    maskDesc.label = ToWgpuStringView("OutlineMask_Pipeline");
    RenderPipelineHandle nextMask{wgpuDeviceCreateRenderPipeline(device_, &maskDesc)};

    WGPUBlendState compBlend = WGPU_BLEND_STATE_INIT;
    compBlend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
    compBlend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    compBlend.color.operation = WGPUBlendOperation_Add;
    compBlend.alpha.srcFactor = WGPUBlendFactor_One;
    compBlend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    compBlend.alpha.operation = WGPUBlendOperation_Add;

    WGPUColorTargetState compTarget = WGPU_COLOR_TARGET_STATE_INIT;
    compTarget.format = hdrFormat_;
    compTarget.blend = &compBlend;
    compTarget.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState compFrag = WGPU_FRAGMENT_STATE_INIT;
    compFrag.module = compModRes->Get();
    compFrag.entryPoint = ToWgpuStringView("fs_outline_comp");
    compFrag.targetCount = 1;
    compFrag.targets = &compTarget;

    WGPURenderPipelineDescriptor compDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    compDesc.layout = outlineCompPl_.Get();
    compDesc.vertex.module = compModRes->Get();
    compDesc.vertex.entryPoint = ToWgpuStringView("vs_outline_comp");
    compDesc.fragment = &compFrag;
    compDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    compDesc.primitive.cullMode = WGPUCullMode_None;
    compDesc.multisample.count = 1;
    compDesc.multisample.mask = 0xFFFFFFFF;
    compDesc.label = ToWgpuStringView("OutlineComposite_Pipeline");
    RenderPipelineHandle nextComp{wgpuDeviceCreateRenderPipeline(device_, &compDesc)};

    if (!nextMask || !nextComp || !outlineCompUniformBuffer_) {
        return std::unexpected("Failed to create selection outline WebGPU pipelines");
    }
    outlineMaskPipeline_ = std::move(nextMask);
    outlineCompPipeline_ = std::move(nextComp);
    return {};
}

void PipelineManager::EnsureSelectionMaskTexture(uint32_t width, uint32_t height) {
    width = std::max(1u, width);
    height = std::max(1u, height);
    if (selectionMaskTexture_ && selectionMaskWidth_ == width && selectionMaskHeight_ == height) {
        return;
    }
    selectionMaskWidth_ = width;
    selectionMaskHeight_ = height;
    outlineCompBindGroup_.Reset();
    selectionMaskView_.Reset();
    selectionMaskTexture_.Reset();

    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = WGPUExtent3D{width, height, 1};
    desc.format = WGPUTextureFormat_RGBA8Unorm;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    desc.label = ToWgpuStringView("SelectionOutlineMaskTexture");
    selectionMaskTexture_ = wgpuDeviceCreateTexture(device_, &desc);
    if (selectionMaskTexture_) {
        selectionMaskView_ = wgpuTextureCreateView(selectionMaskTexture_.Get(), nullptr);
    }

    if (selectionMaskView_ && outlineCompBgl_ && outlineCompUniformBuffer_) {
        std::array<WGPUBindGroupEntry, 2> bgEntries{};
        bgEntries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[0].binding = 0;
        bgEntries[0].buffer = outlineCompUniformBuffer_.Get();
        bgEntries[0].size = sizeof(glm::vec4) * 3u;
        bgEntries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[1].binding = 1;
        bgEntries[1].textureView = selectionMaskView_.Get();

        WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgDesc.layout = outlineCompBgl_.Get();
        bgDesc.entryCount = bgEntries.size();
        bgDesc.entries = bgEntries.data();
        outlineCompBindGroup_ = wgpuDeviceCreateBindGroup(device_, &bgDesc);
    }
}

void PipelineManager::RenderSelectionOutline(WGPUCommandEncoder encoder, scene::Scene& scene,
                                             const scene::SceneNode* selectedNode, const glm::mat4& viewProj,
                                             WGPUTextureView hdrResolveView, uint32_t width, uint32_t height) {
    if (!encoder || !selectedNode || !hdrResolveView || !outlineMaskPipeline_ || !outlineCompPipeline_ ||
        !sceneGlobalsBindGroup_) {
        return;
    }

    EnsureSelectionMaskTexture(width, height);
    if (!selectionMaskView_ || !outlineCompBindGroup_) {
        return;
    }

    mainPerDrawScratch_.clear();
    drawCallsScratch_.clear();
    frameInstanceTransformsScratch_.clear();

    auto collectNode = [&](auto& self, scene::SceneNode& node) -> void {
        auto prims = node.GetPrimitives();
        if (!prims.empty()) {
            const uint32_t instStart = static_cast<uint32_t>(frameInstanceTransformsScratch_.size());
            uint32_t instCount = 1u;
            if (node.instanceTransforms.empty()) {
                frameInstanceTransformsScratch_.emplace_back(1.0f);
            } else {
                instCount = static_cast<uint32_t>(node.instanceTransforms.size());
                frameInstanceTransformsScratch_.insert(frameInstanceTransformsScratch_.end(),
                                                       node.instanceTransforms.begin(), node.instanceTransforms.end());
            }

            for (scene::MeshPrimitive& prim : prims) {
                if (!prim.gpuVertexBuffer || !prim.gpuBindGroup) {
                    continue;
                }
                PreparedDrawBindings prepared{};
                PopulatePerDrawMorphAndSkin(queue_, prim, &node, node.worldTransform, viewProj, currentLightViewProj_,
                                            0u, 0.0f, scene.skins, scene.allNodes, skinFrameId_,
                                            dummySkinBindGroup_.Get(), prepared);
                const uint32_t dynOffset = static_cast<uint32_t>(mainPerDrawScratch_.size() * sizeof(PerDrawUniforms));
                mainPerDrawScratch_.push_back(prepared.uniforms);
                drawCallsScratch_.push_back(DrawDispatchItem{
                    .prim = &prim,
                    .skinBindGroup = prepared.skinBindGroup,
                    .pipeline = outlineMaskPipeline_.Get(),
                    .dynamicOffset = dynOffset,
                    .clampedLod = 0u,
                    .instanceStart = instStart,
                    .instanceCount = instCount,
                });
            }
        }
        for (const auto& child : node.children) {
            if (child) {
                self(self, *child);
            }
        }
    };
    collectNode(collectNode, *const_cast<scene::SceneNode*>(selectedNode));

    if (drawCallsScratch_.empty()) {
        return;
    }

    outlineInstanceBuffer_.Upload(queue_, frameInstanceTransformsScratch_);
    WGPUBuffer instBuf = outlineInstanceBuffer_.GetBuffer();
    WGPUBindGroup perDrawBg = outlinePerDrawRing_.Upload(mainPerDrawScratch_);
    if (!instBuf || !perDrawBg) {
        return;
    }

    // 1. Render selected primitives into 1x RGBA8Unorm silhouette mask
    {
        WGPURenderPassColorAttachment maskAtt = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        maskAtt.view = selectionMaskView_.Get();
        maskAtt.loadOp = WGPULoadOp_Clear;
        maskAtt.storeOp = WGPUStoreOp_Store;
        maskAtt.clearValue = WGPUColor{0.0, 0.0, 0.0, 0.0};

        WGPURenderPassDescriptor maskRp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        maskRp.colorAttachmentCount = 1;
        maskRp.colorAttachments = &maskAtt;
        maskRp.label = ToWgpuStringView("Pass_SelectionOutlineMask");

        if (RenderPassEncoderHandle pass{wgpuCommandEncoderBeginRenderPass(encoder, &maskRp)}) {
            wgpuRenderPassEncoderSetPipeline(pass.Get(), outlineMaskPipeline_.Get());
            wgpuRenderPassEncoderSetBindGroup(pass.Get(), 0, sceneGlobalsBindGroup_.Get(), 0, nullptr);
            for (const auto& dc : drawCallsScratch_) {
                scene::MeshPrimitive& prim = *dc.prim;
                wgpuRenderPassEncoderSetBindGroup(pass.Get(), 1, perDrawBg, 1, &dc.dynamicOffset);
                wgpuRenderPassEncoderSetBindGroup(pass.Get(), 2, prim.gpuBindGroup, 0, nullptr);
                wgpuRenderPassEncoderSetBindGroup(pass.Get(), 3, dc.skinBindGroup, 0, nullptr);
                wgpuRenderPassEncoderSetVertexBuffer(pass.Get(), 0, prim.gpuVertexBuffer, 0, prim.gpuVertexBufferSize);
                wgpuRenderPassEncoderSetVertexBuffer(pass.Get(), 1, instBuf,
                                                     DynamicInstanceBuffer::GetByteOffset(dc.instanceStart),
                                                     DynamicInstanceBuffer::GetSliceSize(dc.instanceCount));
                if (prim.gpuIndexBuffer && !prim.indices.empty()) {
                    const auto lod0 = prim.GetLodSlice(0u);
                    if (lod0.indexCount > 0u) {
                        wgpuRenderPassEncoderSetIndexBuffer(pass.Get(), prim.gpuIndexBuffer, prim.indexFormat, 0,
                                                            prim.gpuIndexBufferSize);
                        wgpuRenderPassEncoderDrawIndexed(pass.Get(), lod0.indexCount, std::max(1u, dc.instanceCount),
                                                         lod0.firstIndex, 0, 0);
                    }
                } else if (!prim.vertices.empty()) {
                    wgpuRenderPassEncoderDraw(pass.Get(), static_cast<uint32_t>(prim.vertices.size()),
                                              std::max(1u, dc.instanceCount), 0, 0);
                }
            }
            wgpuRenderPassEncoderEnd(pass.Get());
        }
    }

    // 2. Composite crisp golden rim onto 1x HDR resolved texture before PostProcessPass
    {
        const std::array<glm::vec4, 3> compUniforms{
            glm::vec4{2.35f, 1.55f, 0.35f, 0.96f}, // HDR warm gold rim
            glm::vec4{1.0f, 0.72f, 0.22f, 0.055f}, // Soft subtle interior selection wash
            glm::vec4{2.75f, 0.0f, 0.0f, 0.0f},    // Outline width in pixels
        };
        wgpuQueueWriteBuffer(queue_, outlineCompUniformBuffer_.Get(), 0, compUniforms.data(), sizeof(compUniforms));

        WGPURenderPassColorAttachment compAtt = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        compAtt.view = hdrResolveView;
        compAtt.loadOp = WGPULoadOp_Load;
        compAtt.storeOp = WGPUStoreOp_Store;

        WGPURenderPassDescriptor compRp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        compRp.colorAttachmentCount = 1;
        compRp.colorAttachments = &compAtt;
        compRp.label = ToWgpuStringView("Pass_SelectionOutlineComposite");

        if (RenderPassEncoderHandle pass{wgpuCommandEncoderBeginRenderPass(encoder, &compRp)}) {
            wgpuRenderPassEncoderSetPipeline(pass.Get(), outlineCompPipeline_.Get());
            wgpuRenderPassEncoderSetBindGroup(pass.Get(), 0, outlineCompBindGroup_.Get(), 0, nullptr);
            wgpuRenderPassEncoderDraw(pass.Get(), 3, 1, 0, 0);
            wgpuRenderPassEncoderEnd(pass.Get());
        }
    }
}

} // namespace bgl::gpu
