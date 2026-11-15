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
struct VertexInput {
    @location(0) pos : vec2<f32>,
    @location(1) uv : vec2<f32>,
    @location(2) color : vec4<f32>,
    @location(3) isSdf : f32,
};
struct VertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) uv : vec2<f32>,
    @location(1) color : vec4<f32>,
    @location(2) isSdf : f32,
};
@vertex
fn vs_main(in : VertexInput) -> VertexOutput {
    var out : VertexOutput;
    out.clipPos = ubo.proj * vec4<f32>(in.pos, 0.0, 1.0);
    out.uv = in.uv;
    out.color = in.color;
    out.isSdf = in.isSdf;
    return out;
}
@fragment
fn fs_main(in : VertexOutput) -> @location(0) vec4<f32> {
    let dist = textureSample(atlasTexture, atlasSampler, in.uv).a;
    let fw = max(fwidth(dist), 0.002) * max(ubo.sdfParams.x, 1.0);
    let sdfAlpha = smoothstep(ubo.sdfParams.y - fw, ubo.sdfParams.y + fw, dist);
    let alpha = select(in.color.a * dist, in.color.a * sdfAlpha, in.isSdf > 0.5);
    return vec4<f32>(in.color.rgb, alpha);
}
)";
#endif

struct alignas(16) UiUniformData {
    glm::mat4 proj{1.0f};
    glm::vec4 sdfParams{1.0f, 0.5f, 0.0f, 0.0f};
};

ImmediateUI* g_activeUiContext = nullptr;

} // anonymous namespace

ImmediateUI::ImmediateUI()
    : arenaBacking_(kArenaBytes), frameArena_(arenaBacking_.data(), arenaBacking_.size()), vertices_(&frameArena_),
      indices_(&frameArena_), overlayVertices_(&frameArena_), overlayIndices_(&frameArena_), hitRects_(&frameArena_) {}

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

    // 4. Bind Group Layout
    std::array<WGPUBindGroupLayoutEntry, 3> bglEntries{};
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

    // 5. Vertex Layout (pos, uv, color, isSdf)
    std::array<WGPUVertexAttribute, 4> vertAttrs{};
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

    // 7. Bind Group
    std::array<WGPUBindGroupEntry, 3> bgEntries{};
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

    WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgDesc.layout = bindGroupLayout_;
    bgDesc.entryCount = bgEntries.size();
    bgDesc.entries = bgEntries.data();
    bindGroup_ = wgpuDeviceCreateBindGroup(device_, &bgDesc);
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

    // Rebind pmr vectors after arena release (capacity must be dropped cleanly)
    vertices_ = std::pmr::vector<UiVertex>{&frameArena_};
    indices_ = std::pmr::vector<uint32_t>{&frameArena_};
    overlayVertices_ = std::pmr::vector<UiVertex>{&frameArena_};
    overlayIndices_ = std::pmr::vector<uint32_t>{&frameArena_};
    hitRects_ = std::pmr::vector<UiRect>{&frameArena_};

    input_ = input;
    viewportWidth_ = std::max(1u, viewportWidth);
    viewportHeight_ = std::max(1u, viewportHeight);
    inWindow_ = false;
    hasPendingOverlay_ = false;
    dropdownToggledThisFrame_ = false;
    wantsMouse_ = false;

    UI::SetContext(this);
}

void ImmediateUI::BeginWindow(std::string_view title, float x, float y, float width) {
    if (inWindow_) {
        EndWindow();
    }
    inWindow_ = true;
    currentWindowRect_ = UiRect{x, y, width, style_.headerHeight + style_.padding};
    cursorX_ = x + style_.padding;
    cursorY_ = y + style_.headerHeight + style_.padding;
    contentWidth_ = std::max(40.0f, width - 2.0f * style_.padding);

    // Reserve 40 vertices (bg quad + 4 border quads = 5 quads = 20 verts; exact match for EmitRectWithBorder)
    windowBgVertexOffset_ = vertices_.size();
    EmitRectWithBorder(vertices_, indices_, x, y, width, currentWindowRect_.h, style_.windowBg, style_.windowBorder,
                       1.0f);

    // Header bar
    EmitRect(vertices_, indices_, x + 1.0f, y + 1.0f, width - 2.0f, style_.headerHeight - 1.0f, style_.headerBg);
    EmitText(vertices_, indices_, title, x + style_.padding, y + 6.0f, style_.textPrimary, style_.textScale);
}

void ImmediateUI::EndWindow() {
    if (!inWindow_)
        return;

    const float finalHeight = (cursorY_ - currentWindowRect_.y) + style_.padding - style_.itemSpacing;
    currentWindowRect_.h = std::max(style_.headerHeight + style_.padding, finalHeight);

    // Patch background & border quad Y coordinates in-place (0 extra allocations)
    const float x = currentWindowRect_.x;
    const float y = currentWindowRect_.y;
    const float w = currentWindowRect_.w;
    const float h = currentWindowRect_.h;
    const float bw = 1.0f;

    auto patchRect = [&](size_t vOff, float rx, float ry, float rw, float rh) {
        if (vOff + 4 > vertices_.size())
            return;
        vertices_[vOff + 0].pos = {rx, ry};
        vertices_[vOff + 1].pos = {rx + rw, ry};
        vertices_[vOff + 2].pos = {rx + rw, ry + rh};
        vertices_[vOff + 3].pos = {rx, ry + rh};
    };

    patchRect(windowBgVertexOffset_ + 0, x, y, w, h);            // Fill
    patchRect(windowBgVertexOffset_ + 4, x, y, w, bw);           // Top border
    patchRect(windowBgVertexOffset_ + 8, x, y + h - bw, w, bw);  // Bottom border
    patchRect(windowBgVertexOffset_ + 12, x, y, bw, h);          // Left border
    patchRect(windowBgVertexOffset_ + 16, x + w - bw, y, bw, h); // Right border

    hitRects_.push_back(currentWindowRect_);
    inWindow_ = false;
}

void ImmediateUI::Label(std::string_view text, const glm::vec4& color) {
    const float h = 18.0f;
    EmitText(vertices_, indices_, text, cursorX_, cursorY_ + 2.0f, color, style_.textScale);
    cursorY_ += h + style_.itemSpacing;
}

bool ImmediateUI::IsBlockedByPopup() const noexcept {
    if (openDropdownId_ == 0) {
        return false;
    }
    if (hasPendingOverlay_ && pendingOverlay_.popupRect.Contains(input_.mouseX, input_.mouseY)) {
        return true;
    }
    return hasActivePopupRect_ && activePopupRect_.Contains(input_.mouseX, input_.mouseY);
}

bool ImmediateUI::Button(std::string_view label) {
    const UiRect rect{cursorX_, cursorY_, contentWidth_, style_.rowHeight};
    const bool hovered = !IsBlockedByPopup() && rect.Contains(input_.mouseX, input_.mouseY);
    const bool pressed = hovered && input_.mouseDown;
    const bool clicked = hovered && input_.mouseReleased;

    const glm::vec4 fill = pressed ? style_.widgetActive : (hovered ? style_.widgetHover : style_.widgetBg);
    const glm::vec4 border = hovered ? style_.accent : style_.windowBorder;
    EmitRectWithBorder(vertices_, indices_, rect.x, rect.y, rect.w, rect.h, fill, border, 1.0f);
    EmitText(vertices_, indices_, label, rect.x + 8.0f, rect.y + 4.0f, style_.textPrimary, style_.textScale);

    hitRects_.push_back(rect);
    cursorY_ += style_.rowHeight + style_.itemSpacing;
    return clicked;
}

bool ImmediateUI::Checkbox(std::string_view label, bool& state) {
    const UiRect rect{cursorX_, cursorY_, contentWidth_, style_.rowHeight};
    const bool hovered = !IsBlockedByPopup() && rect.Contains(input_.mouseX, input_.mouseY);
    const bool clicked = hovered && input_.mouseReleased;
    if (clicked) {
        state = !state;
    }

    const float boxSize = 14.0f;
    const float boxY = rect.y + (rect.h - boxSize) * 0.5f;
    const glm::vec4 boxFill = state ? style_.accent : (hovered ? style_.widgetHover : style_.widgetBg);
    const glm::vec4 boxBorder = hovered ? style_.accent : style_.windowBorder;
    EmitRectWithBorder(vertices_, indices_, rect.x, boxY, boxSize, boxSize, boxFill, boxBorder, 1.0f);

    if (state) {
        EmitRect(vertices_, indices_, rect.x + 3.0f, boxY + 3.0f, boxSize - 6.0f, boxSize - 6.0f, style_.textPrimary);
    }
    EmitText(vertices_, indices_, label, rect.x + boxSize + 8.0f, rect.y + 4.0f, style_.textPrimary, style_.textScale);

    hitRects_.push_back(rect);
    cursorY_ += style_.rowHeight + style_.itemSpacing;
    return clicked;
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
    const bool hoveredAnchor = anchorRect.Contains(input_.mouseX, input_.mouseY);

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

    if (hoveredAnchor && input_.mouseReleased && !dropdownToggledThisFrame_) {
        openDropdownId_ = isOpen ? 0 : id;
        dropdownToggledThisFrame_ = true;
    }

    const bool nowOpen = (openDropdownId_ == id);
    const glm::vec4 fill = (hoveredAnchor || nowOpen) ? style_.widgetHover : style_.widgetBg;
    const glm::vec4 border = nowOpen ? style_.accent : style_.windowBorder;
    EmitRectWithBorder(vertices_, indices_, anchorRect.x, anchorRect.y, anchorRect.w, anchorRect.h, fill, border, 1.0f);

    const std::string_view currentText = options[selectedIndex] ? std::string_view{options[selectedIndex]} : "";
    EmitText(vertices_, indices_, currentText, anchorRect.x + 8.0f, anchorRect.y + 4.0f, style_.textPrimary,
             style_.textScale);
    EmitText(vertices_, indices_, nowOpen ? "▲" : "▼", anchorRect.x + anchorRect.w - 18.0f, anchorRect.y + 4.0f,
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

void ImmediateUI::EndFrame() {
    if (inWindow_) {
        EndWindow();
    }

    // Emit deferred dropdown popup overlay so it stacks cleanly on top of all widgets
    if (hasPendingOverlay_ && openDropdownId_ == pendingOverlay_.id) {
        const auto& ov = pendingOverlay_;
        EmitRectWithBorder(overlayVertices_, overlayIndices_, ov.popupRect.x, ov.popupRect.y, ov.popupRect.w,
                           ov.popupRect.h, style_.popupBg, style_.accent, 1.0f);

        const float itemH = style_.rowHeight;
        for (size_t i = 0; i < ov.options.size(); ++i) {
            const UiRect itemRect{ov.popupRect.x + 1.0f, ov.popupRect.y + static_cast<float>(i) * itemH,
                                  ov.popupRect.w - 2.0f, itemH};
            const bool itemHovered = itemRect.Contains(input_.mouseX, input_.mouseY);
            const bool isSelected = (i == ov.selectedIndex);

            if (itemHovered || isSelected) {
                const glm::vec4 itemColor = itemHovered ? style_.widgetHover : style_.widgetActive;
                EmitRect(overlayVertices_, overlayIndices_, itemRect.x, itemRect.y, itemRect.w, itemRect.h, itemColor);
            }
            const std::string_view optText = ov.options[i] ? std::string_view{ov.options[i]} : "";
            EmitText(overlayVertices_, overlayIndices_, optText, itemRect.x + 8.0f, itemRect.y + 4.0f,
                     style_.textPrimary, style_.textScale);
        }
    }

    // Merge overlay batch into primary batch for single-draw-call submission
    if (!overlayVertices_.empty() && !overlayIndices_.empty()) {
        const uint32_t baseVertex = static_cast<uint32_t>(vertices_.size());
        vertices_.insert(vertices_.end(), overlayVertices_.begin(), overlayVertices_.end());
        for (uint32_t idx : overlayIndices_) {
            indices_.push_back(baseVertex + idx);
        }
    }

    wantsMouse_ = (openDropdownId_ != 0);
    if (!wantsMouse_) {
        for (const auto& r : hitRects_) {
            if (r.Contains(input_.mouseX, input_.mouseY)) {
                wantsMouse_ = true;
                break;
            }
        }
    }
}

void ImmediateUI::Render(WGPURenderPassEncoder pass) {
    if (!gpuInitialized_ || !pass || vertices_.empty() || indices_.empty()) {
        return;
    }

    if (fontReady_) {
        fontAtlas_.SyncGPU(device_, queue_);
    }

    EnsureGpuBuffers(vertices_.size(), indices_.size());

    // Upload uniform projection & SDF parameters
    UiUniformData ubo{};
    ubo.proj =
        glm::ortho(0.0f, static_cast<float>(viewportWidth_), static_cast<float>(viewportHeight_), 0.0f, -1.0f, 1.0f);
    ubo.sdfParams = glm::vec4{1.25f, 0.5f, 0.0f, 0.0f};
    wgpuQueueWriteBuffer(queue_, uniformBuffer_, 0, &ubo, sizeof(ubo));

    const size_t vBytes = vertices_.size() * sizeof(UiVertex);
    const size_t iBytes = indices_.size() * sizeof(uint32_t);
    const size_t alignedIBytes = (iBytes + 3u) & ~size_t{3u};
    wgpuQueueWriteBuffer(queue_, vertexBuffer_, 0, vertices_.data(), vBytes);
    wgpuQueueWriteBuffer(queue_, indexBuffer_, 0, indices_.data(), alignedIBytes);

    // Single Draw-Call Batch
    wgpuRenderPassEncoderSetPipeline(pass, pipeline_);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bindGroup_, 0, nullptr);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, vertexBuffer_, 0, vBytes);
    wgpuRenderPassEncoderSetIndexBuffer(pass, indexBuffer_, WGPUIndexFormat_Uint32, 0, alignedIBytes);
    wgpuRenderPassEncoderDrawIndexed(pass, static_cast<uint32_t>(indices_.size()), 1, 0, 0, 0);
}

void ImmediateUI::EmitQuad(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, float x, float y,
                           float w, float h, const glm::vec2& uvMin, const glm::vec2& uvMax, const glm::vec4& color,
                           float isSdf) const {
    const uint32_t base = static_cast<uint32_t>(verts.size());
    verts.push_back(UiVertex{{x, y}, {uvMin.x, uvMin.y}, color, isSdf});
    verts.push_back(UiVertex{{x + w, y}, {uvMax.x, uvMin.y}, color, isSdf});
    verts.push_back(UiVertex{{x + w, y + h}, {uvMax.x, uvMax.y}, color, isSdf});
    verts.push_back(UiVertex{{x, y + h}, {uvMin.x, uvMax.y}, color, isSdf});

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
    EmitRect(verts, idxs, x, y, w, h, fill);
    EmitRect(verts, idxs, x, y, w, borderWidth, border);
    EmitRect(verts, idxs, x, y + h - borderWidth, w, borderWidth, border);
    EmitRect(verts, idxs, x, y, borderWidth, h, border);
    EmitRect(verts, idxs, x + w - borderWidth, y, borderWidth, h, border);
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

void ImmediateUI::EnsureGpuBuffers(size_t vertexCount, size_t indexCount) {
    if (vertexCount > vertexCapacity_) {
        if (vertexBufferBytes_ > 0) {
            core::MemoryTracker::Instance().TrackBufferFree(vertexBufferBytes_);
            vertexBufferBytes_ = 0;
        }
        vertexCapacity_ = std::max(vertexCount * 2, size_t{2048});
        WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
        desc.size = vertexCapacity_ * sizeof(UiVertex);
        desc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        desc.label = gpu::ToWgpuStringView("ImmediateUIVertexBuffer");
        vertexBuffer_ = wgpuDeviceCreateBuffer(device_, &desc);
        if (vertexBuffer_) {
            vertexBufferBytes_ = desc.size;
            core::MemoryTracker::Instance().TrackBufferAlloc(vertexBufferBytes_);
        }
    }
    if (indexCount > indexCapacity_) {
        if (indexBufferBytes_ > 0) {
            core::MemoryTracker::Instance().TrackBufferFree(indexBufferBytes_);
            indexBufferBytes_ = 0;
        }
        indexCapacity_ = std::max(indexCount * 2, size_t{4096});
        WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
        desc.size = ((indexCapacity_ * sizeof(uint32_t)) + 3u) & ~size_t{3u};
        desc.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
        desc.label = gpu::ToWgpuStringView("ImmediateUIIndexBuffer");
        indexBuffer_ = wgpuDeviceCreateBuffer(device_, &desc);
        if (indexBuffer_) {
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

bool Button(std::string_view label) {
    return g_activeUiContext ? g_activeUiContext->Button(label) : false;
}

bool Checkbox(std::string_view label, bool& state) {
    return g_activeUiContext ? g_activeUiContext->Checkbox(label, state) : false;
}

bool Dropdown(std::string_view label, std::span<const char* const> options, size_t& selectedIndex) {
    return g_activeUiContext ? g_activeUiContext->Dropdown(label, options, selectedIndex) : false;
}

} // namespace UI

} // namespace bgl::ui
