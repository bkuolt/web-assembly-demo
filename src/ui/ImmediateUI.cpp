/**
 * @file ImmediateUI.cpp
 * @brief Implementation of C++23 Immediate Mode GUI with std::pmr arena, SDF font rendering,
 *        single-draw-call WebGPU batching, and #embed WGSL shader.
 */
#include "ui/ImmediateUI.hpp"
#include "core/Log.hpp"
#include "core/MemoryTracker.hpp"
#include "text/EmbeddedFont.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>

namespace bgl::ui {

namespace {

#if defined(__has_embed)
#if __has_embed("shaders/ui.wgsl")
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wc++26-extensions"
#endif
static constexpr char kEmbeddedUiWgslBytes[] = {
#embed "shaders/ui.wgsl"
};
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
static constexpr std::string_view kUiShaderWGSL{kEmbeddedUiWgslBytes, sizeof(kEmbeddedUiWgslBytes)};
#define BGL_UI_SHADER_EMBEDDED 1
#endif
#endif

#ifndef BGL_UI_SHADER_EMBEDDED
static constexpr std::string_view kUiShaderWGSL = R"(
struct UiUniforms {
    proj : mat4x4<f32>,
    sdfParams : vec4<f32>,
};
@group(0) @binding(0) var<uniform> ubo : UiUniforms;
@group(0) @binding(1) var atlasSampler : sampler;
@group(0) @binding(2) var atlasTexture : texture_2d<f32>;
@group(0) @binding(3) var backdropSampler : sampler;
@group(0) @binding(4) var backdropTexture : texture_2d<f32>;
struct VertexInput {
    @location(0) pos : vec2<f32>,
    @location(1) uv : vec2<f32>,
    @location(2) color : vec4<f32>,
    @location(3) isSdf : f32,
    @location(4) sdfRect : vec4<f32>,
    @location(5) auxColor : vec4<f32>,
};
struct VertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) uv : vec2<f32>,
    @location(1) color : vec4<f32>,
    @location(2) isSdf : f32,
    @location(3) sdfRect : vec4<f32>,
    @location(4) auxColor : vec4<f32>,
    @location(5) screenUv : vec2<f32>,
};
fn sdRoundedBox(p : vec2<f32>, halfSize : vec2<f32>, radius : f32) -> f32 {
    let safeHalf = max(halfSize, vec2<f32>(1.0));
    let r = clamp(radius, 0.0, min(safeHalf.x, safeHalf.y));
    let q = abs(p) - safeHalf + vec2<f32>(r);
    return length(max(q, vec2<f32>(0.0))) + min(max(q.x, q.y), 0.0) - r;
}
fn hsv2rgb(c : vec3<f32>) -> vec3<f32> {
    let k = vec4<f32>(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    let p = abs(fract(c.xxx + k.xyz) * 6.0 - k.www);
    return c.z * mix(k.xxx, clamp(p - k.xxx, vec3<f32>(0.0), vec3<f32>(1.0)), c.y);
}
@vertex
fn vs_main(in : VertexInput) -> VertexOutput {
    var out : VertexOutput;
    let clip = ubo.proj * vec4<f32>(in.pos, 0.0, 1.0);
    out.clipPos = clip;
    out.uv = in.uv;
    out.color = in.color;
    out.isSdf = in.isSdf;
    out.sdfRect = in.sdfRect;
    out.auxColor = in.auxColor;
    out.screenUv = vec2<f32>(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
    return out;
}
@fragment
fn fs_main(in : VertexOutput) -> @location(0) vec4<f32> {
    let dist = textureSample(atlasTexture, atlasSampler, in.uv).a;
    let fwDist = max(fwidth(dist), 0.002) * max(ubo.sdfParams.x, 1.0);
    let boxDist = sdRoundedBox(in.uv, in.sdfRect.xy, in.sdfRect.z);
    let aaBox = max(fwidth(boxDist), 0.75);
    let backdropSample = textureSampleLevel(
        backdropTexture, backdropSampler, clamp(in.screenUv, vec2<f32>(0.0), vec2<f32>(1.0)), 0.0);
    let mode = i32(floor(in.isSdf + 0.5));
    if (mode == 1) {
        let sdfAlpha = smoothstep(ubo.sdfParams.y - fwDist, ubo.sdfParams.y + fwDist, dist);
        return vec4<f32>(in.color.rgb, in.color.a * sdfAlpha);
    }
    if (mode == 2 || mode == 4) {
        let outerAlpha = 1.0 - smoothstep(-aaBox, aaBox, boxDist);
        let borderW = max(in.sdfRect.w, 0.0);
        let hasBorder = step(0.01, borderW);
        let borderFactor = smoothstep(-aaBox, aaBox, boxDist + borderW) * hasBorder;
        var fillRgb = in.color.rgb;
        var fillAlpha = in.color.a;
        if (mode == 4 && ubo.sdfParams.z > 0.5) {
            let blurHdr = max(backdropSample.rgb, vec3<f32>(0.0));
            let blurLdr = blurHdr / (vec3<f32>(1.0) + blurHdr);
            let tintedGlass = blurLdr * 0.42 + in.color.rgb * in.color.a;
            let topSheen = exp(-max(in.uv.y + in.sdfRect.y, 0.0) * 0.12) * 0.06;
            fillRgb = clamp(tintedGlass + vec3<f32>(topSheen), vec3<f32>(0.0), vec3<f32>(1.0));
            fillAlpha = max(in.color.a, 0.88);
        }
        return vec4<f32>(mix(fillRgb, in.auxColor.rgb, borderFactor),
                         mix(fillAlpha, in.auxColor.a, borderFactor) * outerAlpha);
    }
    if (mode == 3) {
        let blur = max(in.sdfRect.w, 1.0);
        let falloff = exp(-max(boxDist, 0.0) * 3.2 / blur) * (1.0 - smoothstep(0.0, blur, boxDist));
        return vec4<f32>(in.color.rgb, in.color.a * falloff);
    }
    if (mode == 5) {
        let isHueBar = in.sdfRect.y >= 0.5;
        let hsvCol = select(
            hsv2rgb(vec3<f32>(fract(in.sdfRect.x), clamp(in.uv.x, 0.0, 1.0), clamp(1.0 - in.uv.y, 0.0, 1.0))),
            hsv2rgb(vec3<f32>(clamp(in.uv.y, 0.0, 1.0), 1.0, 1.0)),
            isHueBar);
        return vec4<f32>(hsvCol, in.color.a);
    }
    let hasGrad = step(0.001, in.auxColor.a);
    return mix(in.color, mix(in.color, in.auxColor, clamp(in.uv.y, 0.0, 1.0)), hasGrad);
}
)";
#endif

struct alignas(16) UiUniformData {
    glm::mat4 proj{1.0f};
    glm::vec4 sdfParams{1.25f, 0.5f, 0.0f, 0.0f};
};

} // anonymous namespace

ImmediateUI::ImmediateUI()
    : arenaBacking_(kArenaBytes), frameArena_(arenaBacking_.data(), arenaBacking_.size()), vertices_(&frameArena_),
      indices_(&frameArena_), overlayVertices_(&frameArena_), overlayIndices_(&frameArena_), hitRects_(&frameArena_),
      drawCmds_(&frameArena_) {}

ImmediateUI::~ImmediateUI() {
    if (UI::GetContext() == this) {
        UI::SetContext(nullptr);
    }
    Shutdown();
}

std::string_view ImmediateUI::GetEmbeddedShaderSource() noexcept {
    return kUiShaderWGSL;
}

core::Result<void> ImmediateUI::Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat surfaceFormat,
                                           uint32_t sampleCount) {
    Shutdown();
    device_ = device;
    queue_ = queue;
    surfaceFormat_ = surfaceFormat;
    sampleCount_ = sampleCount;

    if (!device_ || !queue_) {
        return std::unexpected("Cannot initialize ImmediateUI without valid WGPUDevice and WGPUQueue");
    }

    // 1. Initialize FreeType SDF Font Atlas (24px base SDF glyphs for resolution-independent scaling)
    std::string fontPath;
    const std::array<std::string_view, 3> searchPaths = {
        "assets/fonts/LiberationMono-Regular.ttf",
        "../assets/fonts/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    };
    for (const auto path : searchPaths) {
        if (std::filesystem::exists(path)) {
            fontPath = std::string(path);
            break;
        }
    }

    core::Result<void> fontRes;
    if (!fontPath.empty()) {
        fontRes = fontAtlas_.InitializeFromFile(fontPath, 24, 1024, true);
    } else {
        fontRes = fontAtlas_.InitializeFromMemory(text::GetEmbeddedFontData(), 24, 1024, true);
    }
    if (!fontRes) {
        return fontRes;
    }
    fontAtlas_.SyncGPU(device_, queue_);
    fontReady_ = true;

    // 2. Compile embedded WGSL shader module
    WGPUShaderSourceWGSL wgslDesc = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgslDesc.code = gpu::ToWgpuStringView(kUiShaderWGSL);

    WGPUShaderModuleDescriptor smDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    smDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&wgslDesc);
    smDesc.label = gpu::ToWgpuStringView("ImmediateUI_Embedded_SDF_WGSL");
    gpu::ShaderModuleHandle shaderModule{wgpuDeviceCreateShaderModule(device_, &smDesc)};
    if (!shaderModule) {
        return std::unexpected("Failed to create ImmediateUI WGSL shader module");
    }

    // 3. Uniform Buffer
    WGPUBufferDescriptor ubDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    ubDesc.size = sizeof(UiUniformData);
    ubDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    ubDesc.label = gpu::ToWgpuStringView("ImmediateUIUniformBuffer");
    uniformBuffer_ = wgpuDeviceCreateBuffer(device_, &ubDesc);
    if (!uniformBuffer_) {
        Shutdown();
        return std::unexpected("Failed to create ImmediateUI uniform buffer");
    }
    core::MemoryTracker::Instance().TrackBufferAlloc(sizeof(UiUniformData));

    // 4. Bind Group Layout (0=UBO, 1=AtlasSampler, 2=AtlasTex, 3=BackdropSampler, 4=BackdropTex)
    std::array<WGPUBindGroupLayoutEntry, 5> bglEntries{};
    bglEntries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[0].binding = 0;
    bglEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    bglEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
    bglEntries[0].buffer.minBindingSize = sizeof(UiUniformData);

    bglEntries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[1].binding = 1;
    bglEntries[1].visibility = WGPUShaderStage_Fragment;
    bglEntries[1].sampler.type = WGPUSamplerBindingType_Filtering;

    bglEntries[2] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[2].binding = 2;
    bglEntries[2].visibility = WGPUShaderStage_Fragment;
    bglEntries[2].texture.sampleType = WGPUTextureSampleType_Float;
    bglEntries[2].texture.viewDimension = WGPUTextureViewDimension_2D;

    bglEntries[3] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[3].binding = 3;
    bglEntries[3].visibility = WGPUShaderStage_Fragment;
    bglEntries[3].sampler.type = WGPUSamplerBindingType_Filtering;

    bglEntries[4] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[4].binding = 4;
    bglEntries[4].visibility = WGPUShaderStage_Fragment;
    bglEntries[4].texture.sampleType = WGPUTextureSampleType_Float;
    bglEntries[4].texture.viewDimension = WGPUTextureViewDimension_2D;

    WGPUBindGroupLayoutDescriptor bglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bglDesc.entryCount = bglEntries.size();
    bglDesc.entries = bglEntries.data();
    bindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(device_, &bglDesc);

    WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = bindGroupLayout_.AddressOf();
    gpu::PipelineLayoutHandle pipelineLayout{wgpuDeviceCreatePipelineLayout(device_, &plDesc)};
    if (!bindGroupLayout_ || !pipelineLayout) {
        Shutdown();
        return std::unexpected("Failed to create ImmediateUI pipeline layout");
    }

    // 5. Vertex Layout (pos, uv, color, isSdf, sdfRect, auxColor)
    std::array<WGPUVertexAttribute, 6> vertAttrs{};
    vertAttrs[0].format = WGPUVertexFormat_Float32x2;
    vertAttrs[0].offset = offsetof(UiVertex, pos);
    vertAttrs[0].shaderLocation = 0;

    vertAttrs[1].format = WGPUVertexFormat_Float32x2;
    vertAttrs[1].offset = offsetof(UiVertex, uv);
    vertAttrs[1].shaderLocation = 1;

    vertAttrs[2].format = WGPUVertexFormat_Float32x4;
    vertAttrs[2].offset = offsetof(UiVertex, color);
    vertAttrs[2].shaderLocation = 2;

    vertAttrs[3].format = WGPUVertexFormat_Float32;
    vertAttrs[3].offset = offsetof(UiVertex, isSdf);
    vertAttrs[3].shaderLocation = 3;

    vertAttrs[4].format = WGPUVertexFormat_Float32x4;
    vertAttrs[4].offset = offsetof(UiVertex, sdfRect);
    vertAttrs[4].shaderLocation = 4;

    vertAttrs[5].format = WGPUVertexFormat_Float32x4;
    vertAttrs[5].offset = offsetof(UiVertex, auxColor);
    vertAttrs[5].shaderLocation = 5;

    WGPUVertexBufferLayout vbLayout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    vbLayout.arrayStride = sizeof(UiVertex);
    vbLayout.stepMode = WGPUVertexStepMode_Vertex;
    vbLayout.attributeCount = vertAttrs.size();
    vbLayout.attributes = vertAttrs.data();

    // 6. Alpha Blend & Render Pipeline
    WGPUBlendState blendState = WGPU_BLEND_STATE_INIT;
    blendState.color.srcFactor = WGPUBlendFactor_SrcAlpha;
    blendState.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blendState.color.operation = WGPUBlendOperation_Add;
    blendState.alpha.srcFactor = WGPUBlendFactor_One;
    blendState.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blendState.alpha.operation = WGPUBlendOperation_Add;

    WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
    colorTarget.format = surfaceFormat_;
    colorTarget.blend = &blendState;
    colorTarget.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragmentState = WGPU_FRAGMENT_STATE_INIT;
    fragmentState.module = shaderModule;
    fragmentState.entryPoint = gpu::ToWgpuStringView("fs_main");
    fragmentState.targetCount = 1;
    fragmentState.targets = &colorTarget;

    WGPURenderPipelineDescriptor rpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rpDesc.layout = pipelineLayout;
    rpDesc.vertex.module = shaderModule;
    rpDesc.vertex.entryPoint = gpu::ToWgpuStringView("vs_main");
    rpDesc.vertex.bufferCount = 1;
    rpDesc.vertex.buffers = &vbLayout;
    rpDesc.fragment = &fragmentState;
    rpDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rpDesc.primitive.cullMode = WGPUCullMode_None;
    rpDesc.depthStencil = nullptr;
    rpDesc.multisample.count = sampleCount_;
    rpDesc.multisample.mask = 0xFFFFFFFF;
    rpDesc.multisample.alphaToCoverageEnabled = WGPU_FALSE;

    pipeline_ = wgpuDeviceCreateRenderPipeline(device_, &rpDesc);
    if (!pipeline_) {
        return std::unexpected("Failed to create ImmediateUI WebGPU RenderPipeline");
    }

    // 7. Fallback 1x1 Backdrop Texture, Linear Sampler & Bind Group
    WGPUSamplerDescriptor bsDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    bsDesc.addressModeU = WGPUAddressMode_ClampToEdge;
    bsDesc.addressModeV = WGPUAddressMode_ClampToEdge;
    bsDesc.addressModeW = WGPUAddressMode_ClampToEdge;
    bsDesc.magFilter = WGPUFilterMode_Linear;
    bsDesc.minFilter = WGPUFilterMode_Linear;
    bsDesc.mipmapFilter = WGPUMipmapFilterMode_Linear;
    bsDesc.maxAnisotropy = 1;
    backdropSampler_ = wgpuDeviceCreateSampler(device_, &bsDesc);

    WGPUTextureDescriptor fbDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    fbDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    fbDesc.dimension = WGPUTextureDimension_2D;
    fbDesc.size = WGPUExtent3D{1, 1, 1};
    fbDesc.format = WGPUTextureFormat_RGBA8Unorm;
    fbDesc.mipLevelCount = 1;
    fbDesc.sampleCount = 1;
    fbDesc.label = gpu::ToWgpuStringView("ImmediateUI_FallbackBackdrop");
    fallbackBackdropTexture_ = wgpuDeviceCreateTexture(device_, &fbDesc);
    if (fallbackBackdropTexture_) {
        const std::array<uint8_t, 4> fallbackPx{18, 22, 28, 255};
        WGPUTexelCopyTextureInfo copyDst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        copyDst.texture = fallbackBackdropTexture_;
        copyDst.aspect = WGPUTextureAspect_All;
        WGPUTexelCopyBufferLayout dataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        dataLayout.bytesPerRow = 4;
        dataLayout.rowsPerImage = 1;
        const WGPUExtent3D writeSize{1, 1, 1};
        wgpuQueueWriteTexture(queue_, &copyDst, fallbackPx.data(), fallbackPx.size(), &dataLayout, &writeSize);
        fallbackBackdropView_ = wgpuTextureCreateView(fallbackBackdropTexture_, nullptr);
    }

    if (!backdropSampler_ || !fallbackBackdropView_) {
        Shutdown();
        return std::unexpected("Failed to create ImmediateUI backdrop fallback resources");
    }

    RebuildBindGroup();
    if (!bindGroup_) {
        Shutdown();
        return std::unexpected("Failed to create ImmediateUI bind group");
    }

    gpuInitialized_ = true;
    BGL_LOG_INFO("ImmediateUI initialized (SDF Font Atlas={}x{}, #embed WGSL shader={:.1f} KB)",
                 fontAtlas_.GetAtlasDimension(), fontAtlas_.GetAtlasDimension(),
                 static_cast<double>(kUiShaderWGSL.size()) / 1024.0);
    return {};
}

void ImmediateUI::RebuildBindGroup() {
    if (!device_ || !bindGroupLayout_ || !uniformBuffer_ || !backdropSampler_ || !fallbackBackdropView_) {
        return;
    }
    WGPUTextureView activeBackdrop = currentBackdropView_ ? currentBackdropView_ : fallbackBackdropView_.Get();

    std::array<WGPUBindGroupEntry, 5> bgEntries{};
    bgEntries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntries[0].binding = 0;
    bgEntries[0].buffer = uniformBuffer_;
    bgEntries[0].offset = 0;
    bgEntries[0].size = sizeof(UiUniformData);

    bgEntries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntries[1].binding = 1;
    bgEntries[1].sampler = fontAtlas_.GetGPUSampler();

    bgEntries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntries[2].binding = 2;
    bgEntries[2].textureView = fontAtlas_.GetGPUTextureView();

    bgEntries[3] = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntries[3].binding = 3;
    bgEntries[3].sampler = backdropSampler_;

    bgEntries[4] = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntries[4].binding = 4;
    bgEntries[4].textureView = activeBackdrop;

    WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgDesc.layout = bindGroupLayout_;
    bgDesc.entryCount = bgEntries.size();
    bgDesc.entries = bgEntries.data();
    bindGroup_ = wgpuDeviceCreateBindGroup(device_, &bgDesc);
}

void ImmediateUI::SetBackdropTextureView(WGPUTextureView view) noexcept {
    if (view == currentBackdropView_) {
        return;
    }
    currentBackdropView_ = view;
    if (gpuInitialized_) {
        RebuildBindGroup();
    }
}

void ImmediateUI::ReleaseDynamicBuffers() noexcept {
    if (vertexBufferBytes_ > 0) {
        core::MemoryTracker::Instance().TrackBufferFree(vertexBufferBytes_);
        vertexBufferBytes_ = 0;
    }
    if (indexBufferBytes_ > 0) {
        core::MemoryTracker::Instance().TrackBufferFree(indexBufferBytes_);
        indexBufferBytes_ = 0;
    }
    indexBuffer_.Reset();
    vertexBuffer_.Reset();
    vertexCapacity_ = 0;
    indexCapacity_ = 0;
}

void ImmediateUI::Shutdown() noexcept {
    ReleaseDynamicBuffers();
    bindGroup_.Reset();
    currentBackdropView_ = nullptr;
    fallbackBackdropView_.Reset();
    fallbackBackdropTexture_.Reset();
    backdropSampler_.Reset();
    pipeline_.Reset();
    if (uniformBuffer_) {
        core::MemoryTracker::Instance().TrackBufferFree(sizeof(UiUniformData));
        uniformBuffer_.Reset();
    }
    bindGroupLayout_.Reset();
    gpuInitialized_ = false;
    fontReady_ = false;
    device_ = nullptr;
    queue_ = nullptr;
}

uint64_t ImmediateUI::HashId(std::string_view label) noexcept {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : label) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h == 0 ? 1 : h;
}

void ImmediateUI::BeginFrame(const UiInputState& input, uint32_t viewportWidth, uint32_t viewportHeight) {
    // Release PMR monotonic buffer back to 0 offset in O(1) without freeing backing storage
    vertices_.clear();
    indices_.clear();
    overlayVertices_.clear();
    overlayIndices_.clear();
    hitRects_.clear();
    frameArena_.release();

    // Rebind pmr vectors after arena release and pre-reserve inside the frame arena
    vertices_ = std::pmr::vector<UiVertex>{&frameArena_};
    indices_ = std::pmr::vector<uint32_t>{&frameArena_};
    overlayVertices_ = std::pmr::vector<UiVertex>{&frameArena_};
    overlayIndices_ = std::pmr::vector<uint32_t>{&frameArena_};
    hitRects_ = std::pmr::vector<UiRect>{&frameArena_};
    drawCmds_ = std::pmr::vector<DrawCommand>{&frameArena_};
    vertices_.reserve(512);
    indices_.reserve(1024);
    hitRects_.reserve(64);
    drawCmds_.reserve(8);
    batchStartIndex_ = 0;

    input_ = input;
    viewportWidth_ = std::max(1u, viewportWidth);
    viewportHeight_ = std::max(1u, viewportHeight);
    inWindow_ = false;
    inScrollArea_ = false;
    hasPendingOverlay_ = false;
    hasPendingColorOverlay_ = false;
    dropdownToggledThisFrame_ = false;
    colorToggledThisFrame_ = false;
    popupConsumedInputThisFrame_ = false;
    wantsMouse_ = false;

    if (!input_.mouseDown) {
        activeDragWindowId_ = 0;
        activeSliderId_ = 0;
        activeScrollbarId_ = 0;
        activeDropdownScrollId_ = 0;
        activeColorSvId_ = 0;
        activeColorHueId_ = 0;
    }

    UI::SetContext(this);
}

void ImmediateUI::FlushDrawCommand(bool hasScissor, const UiRect& scissorRect) {
    const uint32_t currentIdxCount = static_cast<uint32_t>(indices_.size());
    if (currentIdxCount <= batchStartIndex_) {
        return;
    }
    drawCmds_.push_back(DrawCommand{.firstIndex = batchStartIndex_,
                                    .indexCount = currentIdxCount - batchStartIndex_,
                                    .hasScissor = hasScissor,
                                    .scissorRect = scissorRect});
    batchStartIndex_ = currentIdxCount;
}

void ImmediateUI::EndFrame() {
    if (inWindow_) {
        EndWindow();
    }

    // Merge floating badges & loading banner into main vertex stream BEFORE popups so popups stack above
    if (!overlayVertices_.empty() && !overlayIndices_.empty()) {
        const uint32_t baseVertex = static_cast<uint32_t>(vertices_.size());
        vertices_.insert(vertices_.end(), overlayVertices_.begin(), overlayVertices_.end());
        for (uint32_t idx : overlayIndices_) {
            indices_.push_back(baseVertex + idx);
        }
    }

    EmitColorPickerOverlay();
    EmitDropdownOverlay();

    FlushDrawCommand(false, UiRect{});

    wantsMouse_ = (openDropdownId_ != 0) || (openColorId_ != 0) || (activeDragWindowId_ != 0) ||
                  (activeSliderId_ != 0) || (activeScrollbarId_ != 0) || (activeDropdownScrollId_ != 0);
    if (!wantsMouse_) {
        for (const auto& r : hitRects_) {
            if (r.Contains(input_.mouseX, input_.mouseY)) {
                wantsMouse_ = true;
                break;
            }
        }
    }
    UI::SetContext(nullptr);
}

void ImmediateUI::Render(WGPURenderPassEncoder pass, uint32_t framebufferWidth, uint32_t framebufferHeight) {
    if (!gpuInitialized_ || !pass || vertices_.empty() || indices_.empty() || drawCmds_.empty()) {
        return;
    }

    if (fontReady_) {
        fontAtlas_.SyncGPU(device_, queue_);
    }

    EnsureGpuBuffers(vertices_.size(), indices_.size());
    if (!vertexBuffer_ || !indexBuffer_ || !uniformBuffer_) {
        return;
    }

    // Upload uniform projection & SDF parameters
    UiUniformData ubo{};
    ubo.proj =
        glm::ortho(0.0f, static_cast<float>(viewportWidth_), static_cast<float>(viewportHeight_), 0.0f, -1.0f, 1.0f);
    ubo.sdfParams = glm::vec4{1.25f, 0.5f, currentBackdropView_ ? 1.0f : 0.0f, 0.0f};
    wgpuQueueWriteBuffer(queue_, uniformBuffer_, 0, &ubo, sizeof(ubo));

    const size_t vBytes = vertices_.size() * sizeof(UiVertex);
    const size_t iBytes = indices_.size() * sizeof(uint32_t);
    const size_t alignedIBytes = (iBytes + 3u) & ~size_t{3u};
    wgpuQueueWriteBuffer(queue_, vertexBuffer_, 0, vertices_.data(), vBytes);
    wgpuQueueWriteBuffer(queue_, indexBuffer_, 0, indices_.data(), alignedIBytes);

    const uint32_t fbW = (framebufferWidth > 0) ? framebufferWidth : viewportWidth_;
    const uint32_t fbH = (framebufferHeight > 0) ? framebufferHeight : viewportHeight_;
    const float scaleX = static_cast<float>(fbW) / static_cast<float>(std::max(1u, viewportWidth_));
    const float scaleY = static_cast<float>(fbH) / static_cast<float>(std::max(1u, viewportHeight_));

    wgpuRenderPassEncoderSetPipeline(pass, pipeline_);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bindGroup_, 0, nullptr);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, vertexBuffer_, 0, vBytes);
    wgpuRenderPassEncoderSetIndexBuffer(pass, indexBuffer_, WGPUIndexFormat_Uint32, 0, alignedIBytes);

    const bool hasAnyScissor = drawCmds_.size() > 1 || (drawCmds_.size() == 1 && drawCmds_.front().hasScissor);

    for (const auto& cmd : drawCmds_) {
        if (cmd.indexCount == 0) {
            continue;
        }
        if (hasAnyScissor) {
            if (cmd.hasScissor) {
                const uint32_t sx =
                    static_cast<uint32_t>(std::clamp(cmd.scissorRect.x * scaleX, 0.0f, static_cast<float>(fbW)));
                const uint32_t sy =
                    static_cast<uint32_t>(std::clamp(cmd.scissorRect.y * scaleY, 0.0f, static_cast<float>(fbH)));
                const uint32_t ex = static_cast<uint32_t>(
                    std::clamp((cmd.scissorRect.x + cmd.scissorRect.w) * scaleX, 0.0f, static_cast<float>(fbW)));
                const uint32_t ey = static_cast<uint32_t>(
                    std::clamp((cmd.scissorRect.y + cmd.scissorRect.h) * scaleY, 0.0f, static_cast<float>(fbH)));
                if (ex <= sx || ey <= sy) {
                    continue;
                }
                wgpuRenderPassEncoderSetScissorRect(pass, sx, sy, ex - sx, ey - sy);
            } else {
                wgpuRenderPassEncoderSetScissorRect(pass, 0, 0, fbW, fbH);
            }
        }
        wgpuRenderPassEncoderDrawIndexed(pass, cmd.indexCount, 1, cmd.firstIndex, 0, 0);
    }
}

void ImmediateUI::EmitQuad(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, float x, float y,
                           float w, float h, const glm::vec2& uvMin, const glm::vec2& uvMax, const glm::vec4& color,
                           float isSdf, const glm::vec4& sdfRect, const glm::vec4& auxColor) const {
    const uint32_t base = static_cast<uint32_t>(verts.size());
    verts.push_back(UiVertex{{x, y}, {uvMin.x, uvMin.y}, color, sdfRect, auxColor, isSdf});
    verts.push_back(UiVertex{{x + w, y}, {uvMax.x, uvMin.y}, color, sdfRect, auxColor, isSdf});
    verts.push_back(UiVertex{{x + w, y + h}, {uvMax.x, uvMax.y}, color, sdfRect, auxColor, isSdf});
    verts.push_back(UiVertex{{x, y + h}, {uvMin.x, uvMax.y}, color, sdfRect, auxColor, isSdf});

    idxs.push_back(base + 0);
    idxs.push_back(base + 1);
    idxs.push_back(base + 2);
    idxs.push_back(base + 0);
    idxs.push_back(base + 2);
    idxs.push_back(base + 3);
}

void ImmediateUI::EmitRect(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, float x, float y,
                           float w, float h, const glm::vec4& color) const {
    const glm::vec2 whiteUV = fontReady_ ? fontAtlas_.GetWhitePixelUV() : glm::vec2{0.0f, 0.0f};
    EmitQuad(verts, idxs, x, y, w, h, whiteUV, whiteUV, color, 0.0f);
}

void ImmediateUI::EmitRectWithBorder(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, float x,
                                     float y, float w, float h, const glm::vec4& fill, const glm::vec4& border,
                                     float borderWidth) const {
    EmitRoundedBox(verts, idxs, x, y, w, h, 0.0f, fill, border, borderWidth, false);
}

void ImmediateUI::EmitRoundedBox(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, float x, float y,
                                 float w, float h, float radius, const glm::vec4& fill, const glm::vec4& border,
                                 float borderWidth, bool frostedGlass) const {
    const float hw = std::max(0.5f, w * 0.5f);
    const float hh = std::max(0.5f, h * 0.5f);
    const float pad = 1.5f;
    const glm::vec4 sdfRect{hw, hh, radius, borderWidth};
    EmitQuad(verts, idxs, x - pad, y - pad, w + 2.0f * pad, h + 2.0f * pad, {-hw - pad, -hh - pad},
             {hw + pad, hh + pad}, fill, frostedGlass ? 4.0f : 2.0f, sdfRect, border);
}

void ImmediateUI::EmitShadowBox(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, float x, float y,
                                float w, float h, float radius, float blurRadius, const glm::vec4& shadowColor) const {
    const float hw = std::max(0.5f, w * 0.5f);
    const float hh = std::max(0.5f, h * 0.5f);
    const float pad = std::max(2.0f, blurRadius);
    const glm::vec4 sdfRect{hw, hh, radius, pad};
    EmitQuad(verts, idxs, x - pad, y - pad, w + 2.0f * pad, h + 2.0f * pad, {-hw - pad, -hh - pad},
             {hw + pad, hh + pad}, shadowColor, 3.0f, sdfRect, glm::vec4{0.0f});
}

void ImmediateUI::EmitLineSegment(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs,
                                  const glm::vec2& a, const glm::vec2& b, float thickness,
                                  const glm::vec4& color) const {
    const glm::vec2 delta = b - a;
    const float len = glm::length(delta);
    if (len < 1e-4f) {
        return;
    }
    const glm::vec2 dir = delta / len;
    const glm::vec2 perp{-dir.y, dir.x};
    const glm::vec2 offset = perp * (thickness * 0.5f);

    const uint32_t base = static_cast<uint32_t>(verts.size());
    verts.push_back(UiVertex{a - offset, {0.0f, 0.0f}, color, glm::vec4{0.0f}, glm::vec4{0.0f}, 0.0f});
    verts.push_back(UiVertex{b - offset, {1.0f, 0.0f}, color, glm::vec4{0.0f}, glm::vec4{0.0f}, 0.0f});
    verts.push_back(UiVertex{b + offset, {1.0f, 1.0f}, color, glm::vec4{0.0f}, glm::vec4{0.0f}, 0.0f});
    verts.push_back(UiVertex{a + offset, {0.0f, 1.0f}, color, glm::vec4{0.0f}, glm::vec4{0.0f}, 0.0f});
    idxs.push_back(base + 0);
    idxs.push_back(base + 1);
    idxs.push_back(base + 2);
    idxs.push_back(base + 0);
    idxs.push_back(base + 2);
    idxs.push_back(base + 3);
}

float ImmediateUI::MeasureTextWidth(std::string_view text, float scale) {
    if (text.empty()) {
        return 0.0f;
    }
    if (!fontReady_) {
        return static_cast<float>(text.size()) * 9.0f * scale;
    }
    return text::TextShaper::Shape(fontAtlas_, text, 0.0f, 0.0f, style_.textPrimary, scale).bounds.x;
}

std::string ImmediateUI::EllipsizeText(std::string_view text, float maxWidth, float scale) {
    if (text.empty() || maxWidth <= 8.0f || MeasureTextWidth(text, scale) <= maxWidth) {
        return std::string{text};
    }
    size_t lo = 0;
    size_t hi = text.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo + 1) / 2;
        std::string probe{text.substr(0, mid)};
        probe.append("...");
        if (MeasureTextWidth(probe, scale) <= maxWidth) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    while (lo > 0 && (static_cast<unsigned char>(text[lo]) & 0xC0u) == 0x80u) {
        --lo;
    }
    if (lo == 0) {
        return "...";
    }
    std::string out{text.substr(0, lo)};
    out.append("...");
    return out;
}

void ImmediateUI::EmitText(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, std::string_view text,
                           float x, float y, const glm::vec4& color, float scale) {
    if (!fontReady_ || text.empty()) {
        return;
    }
    const text::TextLayout layout = text::TextShaper::Shape(fontAtlas_, text, x, y, color, scale);
    for (const auto& q : layout.quads) {
        EmitQuad(verts, idxs, q.posMin.x, q.posMin.y, q.posMax.x - q.posMin.x, q.posMax.y - q.posMin.y, q.uvMin,
                 q.uvMax, q.color, 1.0f);
    }
}

glm::vec3 ImmediateUI::RgbToHsv(const glm::vec3& rgb) noexcept {
    const float r = std::clamp(rgb.r, 0.0f, 1.0f);
    const float g = std::clamp(rgb.g, 0.0f, 1.0f);
    const float b = std::clamp(rgb.b, 0.0f, 1.0f);
    const float maxC = std::max({r, g, b});
    const float minC = std::min({r, g, b});
    const float d = maxC - minC;

    float h = 0.0f;
    if (d > 1e-5f) {
        if (maxC == r) {
            h = (g - b) / d + (g < b ? 6.0f : 0.0f);
        } else if (maxC == g) {
            h = (b - r) / d + 2.0f;
        } else {
            h = (r - g) / d + 4.0f;
        }
        h /= 6.0f;
    }
    const float s = (maxC > 1e-5f) ? (d / maxC) : 0.0f;
    return {h, s, maxC};
}

glm::vec3 ImmediateUI::HsvToRgb(const glm::vec3& hsv) noexcept {
    const float h = hsv.x - std::floor(hsv.x);
    const float s = std::clamp(hsv.y, 0.0f, 1.0f);
    const float v = std::clamp(hsv.z, 0.0f, 1.0f);
    const float i = std::floor(h * 6.0f);
    const float f = h * 6.0f - i;
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - f * s);
    const float t = v * (1.0f - (1.0f - f) * s);
    switch (static_cast<int>(i) % 6) {
    case 0:
        return {v, t, p};
    case 1:
        return {q, v, p};
    case 2:
        return {p, v, t};
    case 3:
        return {p, q, v};
    case 4:
        return {t, p, v};
    default:
        return {v, p, q};
    }
}

void ImmediateUI::EnsureGpuBuffers(size_t vertexCount, size_t indexCount) {
    if (vertexCount > vertexCapacity_) {
        const size_t nextCapacity = std::max(vertexCount * 2, size_t{2048});
        WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
        desc.size = nextCapacity * sizeof(UiVertex);
        desc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        desc.label = gpu::ToWgpuStringView("ImmediateUIVertexBuffer");
        gpu::BufferHandle nextVb{wgpuDeviceCreateBuffer(device_, &desc)};
        if (nextVb) {
            if (vertexBufferBytes_ > 0) {
                core::MemoryTracker::Instance().TrackBufferFree(vertexBufferBytes_);
            }
            vertexBuffer_ = std::move(nextVb);
            vertexCapacity_ = nextCapacity;
            vertexBufferBytes_ = desc.size;
            core::MemoryTracker::Instance().TrackBufferAlloc(vertexBufferBytes_);
        }
    }
    if (indexCount > indexCapacity_) {
        const size_t nextCapacity = std::max(indexCount * 2, size_t{4096});
        WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
        desc.size = ((nextCapacity * sizeof(uint32_t)) + 3u) & ~size_t{3u};
        desc.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
        desc.label = gpu::ToWgpuStringView("ImmediateUIIndexBuffer");
        gpu::BufferHandle nextIb{wgpuDeviceCreateBuffer(device_, &desc)};
        if (nextIb) {
            if (indexBufferBytes_ > 0) {
                core::MemoryTracker::Instance().TrackBufferFree(indexBufferBytes_);
            }
            indexBuffer_ = std::move(nextIb);
            indexCapacity_ = nextCapacity;
            indexBufferBytes_ = desc.size;
            core::MemoryTracker::Instance().TrackBufferAlloc(indexBufferBytes_);
        }
    }
}

} // namespace bgl::ui
