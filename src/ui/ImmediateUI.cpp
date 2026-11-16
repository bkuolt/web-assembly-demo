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

ImmediateUI* g_activeUiContext = nullptr;

[[nodiscard]] constexpr glm::vec4 WithAlpha(const glm::vec4& c, float alpha) noexcept {
    return glm::vec4{c.r, c.g, c.b, alpha};
}

void FormatFloatLabel(char* dst, size_t dstSize, std::string_view fmt, float value) noexcept {
    int precision = 2;
    std::string_view suffix;
    if (const size_t dotPos = fmt.find('.'); dotPos != std::string_view::npos && dotPos + 1 < fmt.size()) {
        const char digit = fmt[dotPos + 1];
        if (digit >= '0' && digit <= '6') {
            precision = digit - '0';
        }
        const size_t endBrace = fmt.find('}', dotPos);
        if (endBrace != std::string_view::npos && endBrace + 1 < fmt.size()) {
            suffix = fmt.substr(endBrace + 1);
        } else if (const size_t fPos = fmt.find('f', dotPos); fPos != std::string_view::npos && fPos + 1 < fmt.size()) {
            suffix = fmt.substr(fPos + 1);
        }
    }
    if (suffix.empty()) {
        std::snprintf(dst, dstSize, "%.*f", precision, static_cast<double>(value));
    } else {
        std::snprintf(dst, dstSize, "%.*f%.*s", precision, static_cast<double>(value), static_cast<int>(suffix.size()),
                      suffix.data());
    }
}

} // anonymous namespace

ImmediateUI::ImmediateUI()
    : arenaBacking_(kArenaBytes), frameArena_(arenaBacking_.data(), arenaBacking_.size()), vertices_(&frameArena_),
      indices_(&frameArena_), overlayVertices_(&frameArena_), overlayIndices_(&frameArena_), hitRects_(&frameArena_),
      drawCmds_(&frameArena_) {}

ImmediateUI::~ImmediateUI() {
    if (g_activeUiContext == this) {
        g_activeUiContext = nullptr;
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
    wantsMouse_ = false;

    if (!input_.mouseDown) {
        activeDragWindowId_ = 0;
        activeSliderId_ = 0;
        activeScrollbarId_ = 0;
        activeColorSvId_ = 0;
        activeColorHueId_ = 0;
    }

    UI::SetContext(this);
}

void ImmediateUI::BeginWindow(std::string_view title, float x, float y, float width) {
    if (inWindow_) {
        EndWindow();
    }
    inWindow_ = true;
    currentWindowId_ = HashId(title);

    glm::vec2& dragOffset = windowOffsets_[currentWindowId_];
    float ax = x + dragOffset.x;
    float ay = y + dragOffset.y;

    const UiRect headerRect{ax, ay, width, style_.headerHeight};
    const bool headerHovered = !IsBlockedByPopup() && headerRect.Contains(input_.mouseX, input_.mouseY);
    if (activeDragWindowId_ == 0 && activeSliderId_ == 0 && activeScrollbarId_ == 0 && activeColorSvId_ == 0 &&
        activeColorHueId_ == 0 && input_.mousePressed && headerHovered) {
        activeDragWindowId_ = currentWindowId_;
        dragStartMouse_ = {input_.mouseX, input_.mouseY};
        dragStartOffset_ = dragOffset;
    }

    if (activeDragWindowId_ == currentWindowId_) {
        if (input_.mouseDown) {
            dragOffset = dragStartOffset_ + (glm::vec2{input_.mouseX, input_.mouseY} - dragStartMouse_);
            const float maxX = std::max(0.0f, static_cast<float>(viewportWidth_) - 80.0f);
            const float maxY = std::max(0.0f, static_cast<float>(viewportHeight_) - 40.0f);
            dragOffset.x = std::clamp(x + dragOffset.x, 0.0f, maxX) - x;
            dragOffset.y = std::clamp(y + dragOffset.y, 0.0f, maxY) - y;
            ax = x + dragOffset.x;
            ay = y + dragOffset.y;
        } else {
            activeDragWindowId_ = 0;
        }
    }

    currentWindowRect_ = UiRect{ax, ay, width, style_.headerHeight + style_.padding};
    cursorX_ = ax + style_.padding;
    cursorY_ = ay + style_.headerHeight + style_.padding;
    contentWidth_ = std::max(40.0f, width - 2.0f * style_.padding);

    // Reserve 2 SDF quads (soft drop shadow + frosted glass window body) patched in O(1) at EndWindow
    windowShadowVertexOffset_ = vertices_.size();
    EmitShadowBox(vertices_, indices_, ax, ay + 3.0f, width, currentWindowRect_.h, style_.cornerRadius,
                  style_.shadowBlur, style_.shadowColor);

    windowBgVertexOffset_ = vertices_.size();
    EmitRoundedBox(vertices_, indices_, ax, ay, width, currentWindowRect_.h, style_.cornerRadius, style_.windowBg,
                   style_.windowBorder, 1.0f, true);

    // Header bar + subtle accent indicator bar
    EmitRoundedBox(vertices_, indices_, ax + 1.0f, ay + 1.0f, width - 2.0f, style_.headerHeight - 1.0f,
                   std::max(1.0f, style_.cornerRadius - 1.0f), style_.headerBg, glm::vec4{0.0f}, 0.0f, false);
    EmitRoundedBox(vertices_, indices_, ax + 6.0f, ay + 7.0f, 3.0f, style_.headerHeight - 14.0f, 1.5f, style_.accent,
                   glm::vec4{0.0f}, 0.0f, false);
    EmitText(vertices_, indices_, title, ax + style_.padding + 4.0f, ay + 6.0f, style_.textPrimary, style_.textScale);

    if (activeDragWindowId_ == currentWindowId_) {
        hitRects_.push_back(UiRect{ax, ay, width, style_.headerHeight});
    }
}

void ImmediateUI::EndWindow() {
    if (!inWindow_) {
        return;
    }
    if (inScrollArea_) {
        EndScrollArea();
    }

    const float finalHeight = (cursorY_ - currentWindowRect_.y) + style_.padding - style_.itemSpacing;
    currentWindowRect_.h = std::max(style_.headerHeight + style_.padding, finalHeight);

    const float x = currentWindowRect_.x;
    const float y = currentWindowRect_.y;
    const float w = currentWindowRect_.w;
    const float h = currentWindowRect_.h;

    auto patchSdfBox = [&](size_t vOff, float rx, float ry, float rw, float rh, float pad, float radius, float param4) {
        if (vOff + 4 > vertices_.size()) {
            return;
        }
        const float hw = rw * 0.5f;
        const float hh = rh * 0.5f;
        const glm::vec4 sdfVec{hw, hh, radius, param4};
        vertices_[vOff + 0].pos = {rx - pad, ry - pad};
        vertices_[vOff + 1].pos = {rx + rw + pad, ry - pad};
        vertices_[vOff + 2].pos = {rx + rw + pad, ry + rh + pad};
        vertices_[vOff + 3].pos = {rx - pad, ry + rh + pad};
        vertices_[vOff + 0].uv = {-hw - pad, -hh - pad};
        vertices_[vOff + 1].uv = {hw + pad, -hh - pad};
        vertices_[vOff + 2].uv = {hw + pad, hh + pad};
        vertices_[vOff + 3].uv = {-hw - pad, hh + pad};
        for (size_t i = 0; i < 4; ++i) {
            vertices_[vOff + i].sdfRect = sdfVec;
        }
    };

    patchSdfBox(windowShadowVertexOffset_, x, y + 3.0f, w, h, style_.shadowBlur, style_.cornerRadius,
                style_.shadowBlur);
    patchSdfBox(windowBgVertexOffset_, x, y, w, h, 1.5f, style_.cornerRadius, 1.0f);

    lastCompletedWindowRect_ = currentWindowRect_;
    hitRects_.push_back(currentWindowRect_);
    inWindow_ = false;
}

void ImmediateUI::Label(std::string_view text, const glm::vec4& color) {
    const float h = 18.0f;
    EmitText(vertices_, indices_, text, cursorX_, cursorY_ + 2.0f, color, style_.textScale);
    cursorY_ += h + style_.itemSpacing;
}

void ImmediateUI::Separator() {
    EmitRect(vertices_, indices_, cursorX_, cursorY_ + 3.0f, contentWidth_, 1.0f,
             glm::vec4{style_.windowBorder.r, style_.windowBorder.g, style_.windowBorder.b, 0.45f});
    cursorY_ += 7.0f + style_.itemSpacing * 0.5f;
}

bool ImmediateUI::CollapsingHeader(std::string_view label, bool defaultOpen) {
    const uint64_t id = HashId(label);
    auto [it, inserted] = collapsingHeaders_.emplace(id, defaultOpen);
    bool& isOpen = it->second;

    const UiRect rect{cursorX_, cursorY_, contentWidth_, style_.rowHeight};
    const bool hovered = IsMouseInClipArea() && !IsBlockedByPopup() && rect.Contains(input_.mouseX, input_.mouseY);
    if (hovered && input_.mouseReleased && activeDragWindowId_ == 0) {
        isOpen = !isOpen;
    }

    const glm::vec4 fill = hovered ? style_.widgetHover : glm::vec4{0.13f, 0.155f, 0.21f, 0.92f};
    const glm::vec4 border = hovered ? style_.accent : WithAlpha(style_.windowBorder, 0.55f);
    EmitRoundedBox(vertices_, indices_, rect.x, rect.y, rect.w, rect.h, style_.widgetRadius, fill, border, 1.0f, false);

    // Accent strip when open
    if (isOpen) {
        EmitRoundedBox(vertices_, indices_, rect.x + 2.0f, rect.y + 4.0f, 3.0f, rect.h - 8.0f, 1.5f, style_.accent,
                       glm::vec4{0.0f}, 0.0f, false);
    }

    EmitText(vertices_, indices_, isOpen ? "v" : ">", rect.x + 9.0f, rect.y + 4.0f,
             isOpen ? style_.accent : style_.textMuted, style_.textScale * 0.9f);
    EmitText(vertices_, indices_, label, rect.x + 22.0f, rect.y + 4.0f, style_.textPrimary, style_.textScale);

    hitRects_.push_back(rect);
    cursorY_ += style_.rowHeight + style_.itemSpacing;
    return isOpen;
}

bool ImmediateUI::IsBlockedByPopup() const noexcept {
    if (openDropdownId_ != 0 || openColorId_ != 0) {
        if (hasPendingOverlay_ && pendingOverlay_.popupRect.Contains(input_.mouseX, input_.mouseY)) {
            return true;
        }
        if (hasPendingColorOverlay_ && pendingColorOverlay_.popupRect.Contains(input_.mouseX, input_.mouseY)) {
            return true;
        }
        if (hasActivePopupRect_ && activePopupRect_.Contains(input_.mouseX, input_.mouseY)) {
            return true;
        }
    }
    return false;
}

bool ImmediateUI::IsMouseInClipArea() const noexcept {
    return !inScrollArea_ || scrollRect_.Contains(input_.mouseX, input_.mouseY);
}

bool ImmediateUI::Button(std::string_view label) {
    const UiRect rect{cursorX_, cursorY_, contentWidth_, style_.rowHeight};
    const bool hovered = IsMouseInClipArea() && !IsBlockedByPopup() && rect.Contains(input_.mouseX, input_.mouseY);
    const bool pressed = hovered && input_.mouseDown && activeDragWindowId_ == 0;
    const bool clicked = hovered && input_.mouseReleased && activeDragWindowId_ == 0;

    const glm::vec4 fill = pressed ? style_.widgetActive : (hovered ? style_.widgetHover : style_.widgetBg);
    const glm::vec4 border = hovered ? style_.accent : style_.windowBorder;
    EmitRoundedBox(vertices_, indices_, rect.x, rect.y, rect.w, rect.h, style_.widgetRadius, fill, border, 1.0f, false);
    EmitText(vertices_, indices_, label, rect.x + 8.0f, rect.y + 4.0f, style_.textPrimary, style_.textScale);

    hitRects_.push_back(rect);
    cursorY_ += style_.rowHeight + style_.itemSpacing;
    return clicked;
}

bool ImmediateUI::Checkbox(std::string_view label, bool& state) {
    const UiRect rect{cursorX_, cursorY_, contentWidth_, style_.rowHeight};
    const bool hovered = IsMouseInClipArea() && !IsBlockedByPopup() && rect.Contains(input_.mouseX, input_.mouseY);
    const bool clicked = hovered && input_.mouseReleased && activeDragWindowId_ == 0;
    if (clicked) {
        state = !state;
    }

    const float boxSize = 14.0f;
    const float boxY = rect.y + (rect.h - boxSize) * 0.5f;
    const glm::vec4 boxFill = state ? style_.accent : (hovered ? style_.widgetHover : style_.widgetBg);
    const glm::vec4 boxBorder = hovered ? style_.accent : style_.windowBorder;
    EmitRoundedBox(vertices_, indices_, rect.x, boxY, boxSize, boxSize, 3.5f, boxFill, boxBorder, 1.0f, false);

    if (state) {
        EmitRoundedBox(vertices_, indices_, rect.x + 3.5f, boxY + 3.5f, boxSize - 7.0f, boxSize - 7.0f, 2.0f,
                       style_.textPrimary, glm::vec4{0.0f}, 0.0f, false);
    }
    EmitText(vertices_, indices_, label, rect.x + boxSize + 8.0f, rect.y + 4.0f, style_.textPrimary, style_.textScale);

    hitRects_.push_back(rect);
    cursorY_ += style_.rowHeight + style_.itemSpacing;
    return clicked;
}

bool ImmediateUI::SliderFloat(std::string_view label, float& value, float minVal, float maxVal,
                              std::string_view valueFormat) {
    if (maxVal <= minVal) {
        maxVal = minVal + 1e-4f;
    }
    value = std::clamp(value, minVal, maxVal);
    const uint64_t id = HashId(label);

    char valBuf[48]{};
    FormatFloatLabel(valBuf, sizeof(valBuf), valueFormat, value);
    const float valW = MeasureTextWidth(valBuf, style_.textScale * 0.9f);

    EmitText(vertices_, indices_, label, cursorX_, cursorY_, style_.textMuted, style_.textScale * 0.9f);
    EmitText(vertices_, indices_, valBuf, cursorX_ + std::max(0.0f, contentWidth_ - valW), cursorY_, style_.textPrimary,
             style_.textScale * 0.9f);
    cursorY_ += 17.0f;

    const float trackH = 16.0f;
    const UiRect trackHit{cursorX_, cursorY_, contentWidth_, trackH};
    const bool hovered = IsMouseInClipArea() && !IsBlockedByPopup() && trackHit.Contains(input_.mouseX, input_.mouseY);

    if (activeSliderId_ == 0 && activeDragWindowId_ == 0 && hovered && input_.mousePressed) {
        activeSliderId_ = id;
    }

    bool changed = false;
    if (activeSliderId_ == id) {
        if (input_.mouseDown || input_.mouseReleased) {
            const float padX = 5.0f;
            const float span = std::max(1.0f, trackHit.w - 2.0f * padX);
            const float t = std::clamp((input_.mouseX - (trackHit.x + padX)) / span, 0.0f, 1.0f);
            const float nextVal = minVal + t * (maxVal - minVal);
            if (std::abs(nextVal - value) > 1e-6f) {
                value = nextVal;
                changed = true;
            }
        }
        if (!input_.mouseDown || input_.mouseReleased) {
            activeSliderId_ = 0;
        }
    }

    const float t = std::clamp((value - minVal) / (maxVal - minVal), 0.0f, 1.0f);
    const bool activeOrHover = (activeSliderId_ == id) || hovered;
    const float barY = trackHit.y + 5.0f;
    const float barH = 6.0f;
    EmitRoundedBox(vertices_, indices_, trackHit.x, barY, trackHit.w, barH, 3.0f, style_.sliderTrack,
                   activeOrHover ? style_.accent : style_.windowBorder, 1.0f, false);

    const float fillW = std::max(4.0f, t * trackHit.w);
    EmitRoundedBox(vertices_, indices_, trackHit.x, barY, fillW, barH, 3.0f,
                   activeOrHover ? style_.accent : style_.widgetActive, glm::vec4{0.0f}, 0.0f, false);

    const float thumbSize = activeOrHover ? 13.0f : 11.0f;
    const float thumbX =
        std::clamp(trackHit.x + t * (trackHit.w - thumbSize), trackHit.x, trackHit.x + trackHit.w - thumbSize);
    const float thumbY = trackHit.y + (trackHit.h - thumbSize) * 0.5f;
    EmitRoundedBox(vertices_, indices_, thumbX, thumbY, thumbSize, thumbSize, thumbSize * 0.5f, style_.textPrimary,
                   style_.accent, activeOrHover ? 1.5f : 1.0f, false);

    hitRects_.push_back(trackHit);
    cursorY_ += trackH + style_.itemSpacing;
    return changed;
}

bool ImmediateUI::ColorEdit3(std::string_view label, glm::vec3& color) {
    color = glm::clamp(color, glm::vec3{0.0f}, glm::vec3{1.0f});
    const uint64_t id = HashId(label);
    const UiRect rowRect{cursorX_, cursorY_, contentWidth_, style_.rowHeight};
    const float swatchW = 48.0f;
    const float swatchH = 18.0f;
    const UiRect swatchRect{rowRect.x + rowRect.w - swatchW, rowRect.y + (rowRect.h - swatchH) * 0.5f, swatchW,
                            swatchH};

    const bool hovered = IsMouseInClipArea() && swatchRect.Contains(input_.mouseX, input_.mouseY);
    const bool isOpen = (openColorId_ == id);
    bool colorChanged = false;

    const float svSize = 132.0f;
    const float hueW = 18.0f;
    const float pad = 8.0f;
    const float popW = pad * 3.0f + svSize + hueW;
    const float popH = pad * 2.0f + svSize + 22.0f;
    const float popX = std::clamp(swatchRect.x + swatchRect.w - popW, 4.0f,
                                  std::max(4.0f, static_cast<float>(viewportWidth_) - popW - 4.0f));
    const float popY = (swatchRect.y + swatchRect.h + 4.0f + popH <= static_cast<float>(viewportHeight_))
                           ? (swatchRect.y + swatchRect.h + 4.0f)
                           : std::max(4.0f, swatchRect.y - popH - 4.0f);
    const UiRect popupRect{popX, popY, popW, popH};
    const UiRect svRect{popX + pad, popY + pad, svSize, svSize};
    const UiRect hueRect{popX + pad * 2.0f + svSize, popY + pad, hueW, svSize};

    glm::vec3& storedHsv = colorPickerHsv_.try_emplace(id, RgbToHsv(color)).first->second;
    if (!isOpen) {
        // Synchronize external color modifications cleanly without resetting hue on black/white
        const glm::vec3 extHsv = RgbToHsv(color);
        if (extHsv.y > 0.01f) {
            storedHsv.x = extHsv.x;
        }
        if (extHsv.z > 0.01f) {
            storedHsv.y = extHsv.y;
        }
        storedHsv.z = extHsv.z;
    }

    if (isOpen) {
        hitRects_.push_back(popupRect);
        if (input_.mousePressed) {
            if (svRect.Contains(input_.mouseX, input_.mouseY)) {
                activeColorSvId_ = id;
            } else if (hueRect.Contains(input_.mouseX, input_.mouseY)) {
                activeColorHueId_ = id;
            } else if (!popupRect.Contains(input_.mouseX, input_.mouseY) && !hovered) {
                openColorId_ = 0;
                colorToggledThisFrame_ = true;
            }
        }
        if (activeColorSvId_ == id && (input_.mouseDown || input_.mouseReleased)) {
            storedHsv.y = std::clamp((input_.mouseX - svRect.x) / svRect.w, 0.0f, 1.0f);
            storedHsv.z = 1.0f - std::clamp((input_.mouseY - svRect.y) / svRect.h, 0.0f, 1.0f);
            const glm::vec3 nextRgb = HsvToRgb(storedHsv);
            if (glm::length(nextRgb - color) > 1e-4f) {
                color = nextRgb;
                colorChanged = true;
            }
            if (!input_.mouseDown || input_.mouseReleased) {
                activeColorSvId_ = 0;
            }
        }
        if (activeColorHueId_ == id && (input_.mouseDown || input_.mouseReleased)) {
            storedHsv.x = std::clamp((input_.mouseY - hueRect.y) / hueRect.h, 0.0f, 0.9999f);
            const glm::vec3 nextRgb = HsvToRgb(storedHsv);
            if (glm::length(nextRgb - color) > 1e-4f) {
                color = nextRgb;
                colorChanged = true;
            }
            if (!input_.mouseDown || input_.mouseReleased) {
                activeColorHueId_ = 0;
            }
        }
    }

    if (hovered && input_.mouseReleased && !colorToggledThisFrame_ && !IsBlockedByPopup()) {
        openColorId_ = isOpen ? 0 : id;
        openDropdownId_ = 0;
        colorToggledThisFrame_ = true;
    }

    const bool nowOpen = (openColorId_ == id);
    EmitText(vertices_, indices_, label, rowRect.x, rowRect.y + 4.0f, style_.textPrimary, style_.textScale);
    EmitRoundedBox(vertices_, indices_, swatchRect.x, swatchRect.y, swatchRect.w, swatchRect.h, 4.0f,
                   glm::vec4{color, 1.0f}, (hovered || nowOpen) ? style_.accent : style_.windowBorder, 1.5f, false);

    if (nowOpen) {
        hasPendingColorOverlay_ = true;
        hasActivePopupRect_ = true;
        activePopupRect_ = popupRect;
        pendingColorOverlay_ = PendingColorOverlay{.id = id,
                                                   .anchorRect = swatchRect,
                                                   .popupRect = popupRect,
                                                   .svRect = svRect,
                                                   .hueRect = hueRect,
                                                   .hsv = storedHsv,
                                                   .rgb = color};
    }

    hitRects_.push_back(swatchRect);
    cursorY_ += style_.rowHeight + style_.itemSpacing;
    return colorChanged;
}

void ImmediateUI::Sparkline(std::string_view label, std::span<const float> values, float minVal, float maxVal,
                            float height, const glm::vec4& lineColor) {
    float curVal = 0.0f;
    float lo = minVal;
    float hi = maxVal;
    if (!values.empty()) {
        curVal = values.back();
        if (hi <= lo) {
            lo = values.front();
            hi = values.front();
            for (float v : values) {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            const float padSpan = std::max(1.0f, (hi - lo) * 0.15f);
            lo = std::max(0.0f, lo - padSpan * 0.25f);
            hi += padSpan * 0.25f;
        }
    } else {
        hi = 1.0f;
    }
    const float span = std::max(1e-4f, hi - lo);

    char valBuf[32]{};
    std::snprintf(valBuf, sizeof(valBuf), "%.1f", static_cast<double>(curVal));
    const float valW = MeasureTextWidth(valBuf, style_.textScale * 0.9f);
    EmitText(vertices_, indices_, label, cursorX_, cursorY_, style_.textMuted, style_.textScale * 0.9f);
    EmitText(vertices_, indices_, valBuf, cursorX_ + std::max(0.0f, contentWidth_ - valW), cursorY_, lineColor,
             style_.textScale * 0.9f);
    cursorY_ += 17.0f;

    const UiRect box{cursorX_, cursorY_, contentWidth_, std::max(24.0f, height)};
    EmitRoundedBox(vertices_, indices_, box.x, box.y, box.w, box.h, style_.widgetRadius,
                   glm::vec4{0.06f, 0.07f, 0.095f, 0.95f}, style_.windowBorder, 1.0f, false);

    // Subtle horizontal midline guide
    const float midY = box.y + box.h * 0.5f;
    EmitRect(vertices_, indices_, box.x + 4.0f, midY, std::max(0.0f, box.w - 8.0f), 1.0f,
             WithAlpha(style_.windowBorder, 0.25f));

    if (values.size() >= 2) {
        const float plotX = box.x + 4.0f;
        const float plotY = box.y + 4.0f;
        const float plotW = std::max(4.0f, box.w - 8.0f);
        const float plotH = std::max(4.0f, box.h - 8.0f);
        const float bottomY = plotY + plotH;
        const glm::vec4 topFill = WithAlpha(lineColor, 0.26f);
        const glm::vec4 botFill = WithAlpha(lineColor, 0.02f);

        for (size_t i = 0; i + 1 < values.size(); ++i) {
            const float t0 = static_cast<float>(i) / static_cast<float>(values.size() - 1);
            const float t1 = static_cast<float>(i + 1) / static_cast<float>(values.size() - 1);
            const float n0 = std::clamp((values[i] - lo) / span, 0.0f, 1.0f);
            const float n1 = std::clamp((values[i + 1] - lo) / span, 0.0f, 1.0f);
            const glm::vec2 p0{plotX + t0 * plotW, bottomY - n0 * plotH};
            const glm::vec2 p1{plotX + t1 * plotW, bottomY - n1 * plotH};

            // Shaded trapezoidal fill under graph segment (mode 0 gradient)
            const uint32_t base = static_cast<uint32_t>(vertices_.size());
            vertices_.push_back(UiVertex{p0, {0.0f, 0.0f}, topFill, glm::vec4{0.0f}, botFill, 0.0f});
            vertices_.push_back(UiVertex{p1, {1.0f, 0.0f}, topFill, glm::vec4{0.0f}, botFill, 0.0f});
            vertices_.push_back(UiVertex{{p1.x, bottomY}, {1.0f, 1.0f}, topFill, glm::vec4{0.0f}, botFill, 0.0f});
            vertices_.push_back(UiVertex{{p0.x, bottomY}, {0.0f, 1.0f}, topFill, glm::vec4{0.0f}, botFill, 0.0f});
            indices_.push_back(base + 0);
            indices_.push_back(base + 1);
            indices_.push_back(base + 2);
            indices_.push_back(base + 0);
            indices_.push_back(base + 2);
            indices_.push_back(base + 3);

            EmitLineSegment(vertices_, indices_, p0, p1, 1.75f, lineColor);
        }
    }

    hitRects_.push_back(box);
    cursorY_ += box.h + style_.itemSpacing;
}

bool ImmediateUI::Dropdown(std::string_view label, std::span<const char* const> options, size_t& selectedIndex) {
    if (options.empty()) {
        Label(label, style_.textMuted);
        return false;
    }
    if (selectedIndex >= options.size()) {
        selectedIndex = 0;
    }

    const uint64_t id = HashId(label);

    // Label above dropdown box
    EmitText(vertices_, indices_, label, cursorX_, cursorY_, style_.textMuted, style_.textScale * 0.9f);
    cursorY_ += 17.0f;

    const UiRect anchorRect{cursorX_, cursorY_, contentWidth_, style_.rowHeight};
    const bool isOpen = (openDropdownId_ == id);
    const bool hoveredAnchor = IsMouseInClipArea() && anchorRect.Contains(input_.mouseX, input_.mouseY);

    bool selectionChanged = false;

    const float itemH = style_.rowHeight;
    const UiRect popupRect{anchorRect.x, anchorRect.y + anchorRect.h + 2.0f, anchorRect.w,
                           itemH * static_cast<float>(options.size())};

    // Handle open dropdown popup interaction BEFORE anchor toggle
    if (isOpen) {
        hitRects_.push_back(popupRect);
        if (input_.mouseReleased) {
            if (popupRect.Contains(input_.mouseX, input_.mouseY)) {
                const int idx = static_cast<int>((input_.mouseY - popupRect.y) / itemH);
                if (idx >= 0 && static_cast<size_t>(idx) < options.size()) {
                    if (selectedIndex != static_cast<size_t>(idx)) {
                        selectedIndex = static_cast<size_t>(idx);
                        selectionChanged = true;
                    }
                }
                openDropdownId_ = 0;
                dropdownToggledThisFrame_ = true;
            } else if (!hoveredAnchor) {
                openDropdownId_ = 0;
                dropdownToggledThisFrame_ = true;
            }
        }
    }

    if (hoveredAnchor && input_.mouseReleased && !dropdownToggledThisFrame_ && activeDragWindowId_ == 0) {
        openDropdownId_ = isOpen ? 0 : id;
        openColorId_ = 0;
        dropdownToggledThisFrame_ = true;
    }

    const bool nowOpen = (openDropdownId_ == id);
    const glm::vec4 fill = (hoveredAnchor || nowOpen) ? style_.widgetHover : style_.widgetBg;
    const glm::vec4 border = nowOpen ? style_.accent : style_.windowBorder;
    EmitRoundedBox(vertices_, indices_, anchorRect.x, anchorRect.y, anchorRect.w, anchorRect.h, style_.widgetRadius,
                   fill, border, 1.0f, false);

    const std::string_view currentText = options[selectedIndex] ? std::string_view{options[selectedIndex]} : "";
    EmitText(vertices_, indices_, currentText, anchorRect.x + 8.0f, anchorRect.y + 4.0f, style_.textPrimary,
             style_.textScale);
    EmitText(vertices_, indices_, nowOpen ? "^" : "v", anchorRect.x + anchorRect.w - 18.0f, anchorRect.y + 4.0f,
             style_.textMuted, style_.textScale * 0.85f);

    if (nowOpen) {
        hasPendingOverlay_ = true;
        hasActivePopupRect_ = true;
        activePopupRect_ = popupRect;
        pendingOverlay_ = PendingDropdownOverlay{.id = id,
                                                 .anchorRect = anchorRect,
                                                 .popupRect = popupRect,
                                                 .options = options,
                                                 .selectedIndex = selectedIndex};
    } else if (isOpen) {
        hasActivePopupRect_ = false;
    }

    hitRects_.push_back(anchorRect);
    cursorY_ += style_.rowHeight + style_.itemSpacing;
    return selectionChanged;
}

void ImmediateUI::BeginScrollArea(std::string_view id, float maxHeight) {
    if (inScrollArea_) {
        EndScrollArea();
    }
    const uint64_t hash = HashId(id);
    const float clampedH = std::max(40.0f, maxHeight);
    scrollRect_ = UiRect{cursorX_, cursorY_, contentWidth_, clampedH};

    float& scrollOffset = scrollOffsets_[hash];
    const float prevContentH = scrollContentHeights_[hash];
    const float maxScroll = std::max(0.0f, prevContentH - clampedH);

    if (!IsBlockedByPopup() && scrollRect_.Contains(input_.mouseX, input_.mouseY) &&
        std::abs(input_.wheelDelta) > 1e-4f) {
        scrollOffset -= input_.wheelDelta * 26.0f;
    }

    // Handle optional scrollbar thumb drag when overflowing
    const UiRect trackRect{scrollRect_.x + scrollRect_.w - 8.0f, scrollRect_.y + 2.0f, 6.0f,
                           std::max(12.0f, clampedH - 4.0f)};
    if (maxScroll > 1.0f && activeScrollbarId_ == 0 && activeDragWindowId_ == 0 && input_.mousePressed &&
        trackRect.Contains(input_.mouseX, input_.mouseY)) {
        activeScrollbarId_ = hash;
    }
    if (activeScrollbarId_ == hash) {
        if (input_.mouseDown || input_.mouseReleased) {
            const float frac = std::clamp((input_.mouseY - trackRect.y) / trackRect.h, 0.0f, 1.0f);
            scrollOffset = frac * maxScroll;
        }
        if (!input_.mouseDown || input_.mouseReleased) {
            activeScrollbarId_ = 0;
        }
    }
    scrollOffset = std::clamp(scrollOffset, 0.0f, maxScroll);

    // Subtle sunken background container rendered unclipped before scissor sub-batch
    EmitRoundedBox(vertices_, indices_, scrollRect_.x, scrollRect_.y, scrollRect_.w, scrollRect_.h, style_.widgetRadius,
                   glm::vec4{0.055f, 0.065f, 0.088f, 0.85f}, WithAlpha(style_.windowBorder, 0.55f), 1.0f, false);

    FlushDrawCommand(false, UiRect{});

    inScrollArea_ = true;
    activeScrollId_ = hash;
    scrollSavedCursorX_ = cursorX_;
    scrollSavedCursorY_ = cursorY_;
    scrollSavedContentW_ = contentWidth_;
    scrollAppliedOffset_ = scrollOffset;

    cursorX_ = scrollRect_.x + 6.0f;
    cursorY_ = scrollRect_.y + 4.0f - scrollOffset;
    contentWidth_ = std::max(30.0f, scrollRect_.w - 18.0f);
    scrollContentStartY_ = scrollRect_.y + 4.0f;
}

void ImmediateUI::EndScrollArea() {
    if (!inScrollArea_) {
        return;
    }

    FlushDrawCommand(true, scrollRect_);

    const float measuredContentH = std::max(0.0f, (cursorY_ + scrollAppliedOffset_) - scrollContentStartY_ + 6.0f);
    scrollContentHeights_[activeScrollId_] = measuredContentH;

    cursorX_ = scrollSavedCursorX_;
    cursorY_ = scrollRect_.y + scrollRect_.h + style_.itemSpacing;
    contentWidth_ = scrollSavedContentW_;
    inScrollArea_ = false;

    const float maxScroll = std::max(0.0f, measuredContentH - scrollRect_.h);
    if (maxScroll > 1.0f) {
        const float trackX = scrollRect_.x + scrollRect_.w - 7.0f;
        const float trackY = scrollRect_.y + 4.0f;
        const float trackH = std::max(12.0f, scrollRect_.h - 8.0f);
        EmitRoundedBox(vertices_, indices_, trackX, trackY, 4.0f, trackH, 2.0f, style_.sliderTrack, glm::vec4{0.0f},
                       0.0f, false);
        const float visibleRatio = std::clamp(scrollRect_.h / std::max(scrollRect_.h, measuredContentH), 0.12f, 1.0f);
        const float thumbH = std::max(14.0f, trackH * visibleRatio);
        const float scrollFrac = std::clamp(scrollAppliedOffset_ / maxScroll, 0.0f, 1.0f);
        const float thumbY = trackY + scrollFrac * std::max(0.0f, trackH - thumbH);
        const bool thumbActive = (activeScrollbarId_ == activeScrollId_);
        EmitRoundedBox(vertices_, indices_, trackX, thumbY, 4.0f, thumbH, 2.0f,
                       thumbActive ? style_.accent : style_.windowBorder, glm::vec4{0.0f}, 0.0f, false);
    }

    hitRects_.push_back(scrollRect_);
}

void ImmediateUI::DrawFloatingBadge(float x, float y, std::string_view text, const glm::vec4& accentColor) {
    if (text.empty()) {
        return;
    }
    const float scale = style_.textScale * 0.85f;
    const float textW = MeasureTextWidth(text, scale);
    const float badgeW = std::max(36.0f, textW + 18.0f);
    const float badgeH = 20.0f;
    EmitShadowBox(overlayVertices_, overlayIndices_, x, y + 1.5f, badgeW, badgeH, 5.0f, 6.0f, style_.shadowColor);
    EmitRoundedBox(overlayVertices_, overlayIndices_, x, y, badgeW, badgeH, 5.0f, style_.popupBg, accentColor, 1.25f,
                   false);
    EmitRoundedBox(overlayVertices_, overlayIndices_, x + 5.0f, y + 6.0f, 6.0f, 8.0f, 3.0f, accentColor,
                   glm::vec4{0.0f}, 0.0f, false);
    EmitText(overlayVertices_, overlayIndices_, text, x + 14.0f, y + 3.0f, style_.textPrimary, scale);
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

    // Emit deferred dropdown popup overlay so it stacks cleanly on top of all widgets
    if (hasPendingOverlay_ && openDropdownId_ == pendingOverlay_.id) {
        const auto& ov = pendingOverlay_;
        EmitShadowBox(overlayVertices_, overlayIndices_, ov.popupRect.x, ov.popupRect.y + 3.0f, ov.popupRect.w,
                      ov.popupRect.h, style_.widgetRadius, 10.0f, style_.shadowColor);
        EmitRoundedBox(overlayVertices_, overlayIndices_, ov.popupRect.x, ov.popupRect.y, ov.popupRect.w,
                       ov.popupRect.h, style_.widgetRadius, style_.popupBg, style_.accent, 1.0f, false);

        const float itemH = style_.rowHeight;
        for (size_t i = 0; i < ov.options.size(); ++i) {
            const UiRect itemRect{ov.popupRect.x + 2.0f, ov.popupRect.y + static_cast<float>(i) * itemH + 1.0f,
                                  ov.popupRect.w - 4.0f, std::max(4.0f, itemH - 2.0f)};
            const bool itemHovered = itemRect.Contains(input_.mouseX, input_.mouseY);
            const bool isSelected = (i == ov.selectedIndex);

            if (itemHovered || isSelected) {
                const glm::vec4 itemColor = itemHovered ? style_.widgetHover : style_.widgetActive;
                EmitRoundedBox(overlayVertices_, overlayIndices_, itemRect.x, itemRect.y, itemRect.w, itemRect.h, 3.0f,
                               itemColor, glm::vec4{0.0f}, 0.0f, false);
            }
            const std::string_view optText = ov.options[i] ? std::string_view{ov.options[i]} : "";
            EmitText(overlayVertices_, overlayIndices_, optText, itemRect.x + 6.0f, itemRect.y + 3.0f,
                     style_.textPrimary, style_.textScale);
        }
    }

    // Emit deferred HSV color picker overlay cleanly on top
    if (hasPendingColorOverlay_ && openColorId_ == pendingColorOverlay_.id) {
        const auto& cp = pendingColorOverlay_;
        EmitShadowBox(overlayVertices_, overlayIndices_, cp.popupRect.x, cp.popupRect.y + 3.0f, cp.popupRect.w,
                      cp.popupRect.h, style_.cornerRadius, 12.0f, style_.shadowColor);
        EmitRoundedBox(overlayVertices_, overlayIndices_, cp.popupRect.x, cp.popupRect.y, cp.popupRect.w,
                       cp.popupRect.h, style_.cornerRadius, style_.popupBg, style_.accent, 1.0f, false);

        // Mode 5: 2D SV square
        EmitQuad(overlayVertices_, overlayIndices_, cp.svRect.x, cp.svRect.y, cp.svRect.w, cp.svRect.h, {0.0f, 0.0f},
                 {1.0f, 1.0f}, glm::vec4{1.0f}, 5.0f, glm::vec4{cp.hsv.x, 0.0f, 0.0f, 0.0f});
        // Crosshair inside SV square
        const float cx = cp.svRect.x + cp.hsv.y * cp.svRect.w;
        const float cy = cp.svRect.y + (1.0f - cp.hsv.z) * cp.svRect.h;
        EmitRoundedBox(overlayVertices_, overlayIndices_, cx - 4.0f, cy - 4.0f, 8.0f, 8.0f, 4.0f,
                       glm::vec4{cp.rgb, 1.0f}, glm::vec4{1.0f}, 1.5f, false);

        // Mode 5: 1D vertical Hue bar
        EmitQuad(overlayVertices_, overlayIndices_, cp.hueRect.x, cp.hueRect.y, cp.hueRect.w, cp.hueRect.h,
                 {0.0f, 0.0f}, {1.0f, 1.0f}, glm::vec4{1.0f}, 5.0f, glm::vec4{cp.hsv.x, 1.0f, 0.0f, 0.0f});
        const float hy = cp.hueRect.y + cp.hsv.x * cp.hueRect.h;
        EmitRect(overlayVertices_, overlayIndices_, cp.hueRect.x - 1.0f, hy - 1.5f, cp.hueRect.w + 2.0f, 3.0f,
                 glm::vec4{1.0f});

        char rgbBuf[40]{};
        std::snprintf(rgbBuf, sizeof(rgbBuf), "R %.2f  G %.2f  B %.2f", static_cast<double>(cp.rgb.r),
                      static_cast<double>(cp.rgb.g), static_cast<double>(cp.rgb.b));
        EmitText(overlayVertices_, overlayIndices_, rgbBuf, cp.svRect.x, cp.svRect.y + cp.svRect.h + 5.0f,
                 style_.textPrimary, style_.textScale * 0.85f);
    }

    // Merge overlay batch into primary batch and finalize draw commands
    if (!overlayVertices_.empty() && !overlayIndices_.empty()) {
        const uint32_t baseVertex = static_cast<uint32_t>(vertices_.size());
        vertices_.insert(vertices_.end(), overlayVertices_.begin(), overlayVertices_.end());
        for (uint32_t idx : overlayIndices_) {
            indices_.push_back(baseVertex + idx);
        }
    }
    FlushDrawCommand(false, UiRect{});

    wantsMouse_ = (openDropdownId_ != 0) || (openColorId_ != 0) || (activeDragWindowId_ != 0) ||
                  (activeSliderId_ != 0) || (activeScrollbarId_ != 0);
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

namespace UI {

void SetContext(ImmediateUI* ctx) noexcept {
    g_activeUiContext = ctx;
}

ImmediateUI* GetContext() noexcept {
    return g_activeUiContext;
}

void Label(std::string_view text, const glm::vec4& color) {
    if (g_activeUiContext) {
        g_activeUiContext->Label(text, color);
    }
}

void Separator() {
    if (g_activeUiContext) {
        g_activeUiContext->Separator();
    }
}

bool CollapsingHeader(std::string_view label, bool defaultOpen) {
    return g_activeUiContext ? g_activeUiContext->CollapsingHeader(label, defaultOpen) : defaultOpen;
}

bool Button(std::string_view label) {
    return g_activeUiContext ? g_activeUiContext->Button(label) : false;
}

bool Checkbox(std::string_view label, bool& state) {
    return g_activeUiContext ? g_activeUiContext->Checkbox(label, state) : false;
}

bool SliderFloat(std::string_view label, float& value, float minVal, float maxVal, std::string_view valueFormat) {
    return g_activeUiContext ? g_activeUiContext->SliderFloat(label, value, minVal, maxVal, valueFormat) : false;
}

bool ColorEdit3(std::string_view label, glm::vec3& color) {
    return g_activeUiContext ? g_activeUiContext->ColorEdit3(label, color) : false;
}

void Sparkline(std::string_view label, std::span<const float> values, float minVal, float maxVal, float height,
               const glm::vec4& lineColor) {
    if (g_activeUiContext) {
        g_activeUiContext->Sparkline(label, values, minVal, maxVal, height, lineColor);
    }
}

bool Dropdown(std::string_view label, std::span<const char* const> options, size_t& selectedIndex) {
    return g_activeUiContext ? g_activeUiContext->Dropdown(label, options, selectedIndex) : false;
}

void BeginScrollArea(std::string_view id, float maxHeight) {
    if (g_activeUiContext) {
        g_activeUiContext->BeginScrollArea(id, maxHeight);
    }
}

void EndScrollArea() {
    if (g_activeUiContext) {
        g_activeUiContext->EndScrollArea();
    }
}

} // namespace UI

} // namespace bgl::ui
