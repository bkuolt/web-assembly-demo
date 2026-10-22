/**
 * @file TextRenderer.cpp
 * @brief WebGPU 2D Batched Quad and Text Renderer implementation.
 */
#include "text/TextRenderer.hpp"
#include "core/Log.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <array>

namespace bgl::text {

namespace {

constexpr std::string_view kTextWGSL = R"(
struct Uniforms {
    proj : mat4x4<f32>,
};

@group(0) @binding(0) var<uniform> ubo : Uniforms;
@group(0) @binding(1) var atlasSampler : sampler;
@group(0) @binding(2) var atlasTexture : texture_2d<f32>;

struct VertexInput {
    @location(0) pos : vec2<f32>,
    @location(1) uv : vec2<f32>,
    @location(2) color : vec4<f32>,
};

struct VertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) uv : vec2<f32>,
    @location(1) color : vec4<f32>,
};

@vertex
fn vs_main(in : VertexInput) -> VertexOutput {
    var out : VertexOutput;
    out.clipPos = ubo.proj * vec4<f32>(in.pos, 0.0, 1.0);
    out.uv = in.uv;
    out.color = in.color;
    return out;
}

@fragment
fn fs_main(in : VertexOutput) -> @location(0) vec4<f32> {
    let tex = textureSample(atlasTexture, atlasSampler, in.uv);
    return in.color * tex;
}
)";

} // anonymous namespace

TextRenderer::TextRenderer() = default;

TextRenderer::~TextRenderer() = default;

core::Result<void> TextRenderer::Initialize(WGPUDevice device,
                                            WGPUQueue queue,
                                            WGPUTextureFormat surfaceFormat,
                                            uint32_t sampleCount,
                                            FontAtlas& fontAtlas) {
    device_ = device;
    queue_ = queue;
    surfaceFormat_ = surfaceFormat;
    sampleCount_ = sampleCount;
    fontAtlas_ = &fontAtlas;

    if (!device_ || !queue_) {
        return std::unexpected("Cannot initialize TextRenderer without valid WGPUDevice and WGPUQueue");
    }

    // Ensure atlas has uploaded initial texture and sampler to GPU
    fontAtlas_->SyncGPU(device_, queue_);

    // 1. Shader Module
    WGPUShaderSourceWGSL wgslDesc = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgslDesc.code = gpu::ToWgpuStringView(kTextWGSL);

    WGPUShaderModuleDescriptor smDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    smDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&wgslDesc);
    smDesc.label = gpu::ToWgpuStringView("TextShader_WGSL");
    gpu::ShaderModuleHandle shaderModule{wgpuDeviceCreateShaderModule(device_, &smDesc)};
    if (!shaderModule) {
        return std::unexpected("Failed to create TextRenderer WGSL shader module");
    }

    // 2. Uniform Buffer (Orthographic Matrix, 64 bytes)
    WGPUBufferDescriptor ubDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    ubDesc.size = sizeof(glm::mat4);
    ubDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    ubDesc.label = gpu::ToWgpuStringView("TextUniformBuffer");
    uniformBuffer_ = wgpuDeviceCreateBuffer(device_, &ubDesc);

    // 3. Bind Group Layout
    std::array<WGPUBindGroupLayoutEntry, 3> bglEntries{};
    // Binding 0: Uniform Buffer
    bglEntries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[0].binding = 0;
    bglEntries[0].visibility = WGPUShaderStage_Vertex;
    bglEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
    bglEntries[0].buffer.minBindingSize = sizeof(glm::mat4);

    // Binding 1: Sampler
    bglEntries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[1].binding = 1;
    bglEntries[1].visibility = WGPUShaderStage_Fragment;
    bglEntries[1].sampler.type = WGPUSamplerBindingType_Filtering;

    // Binding 2: Texture 2D
    bglEntries[2] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[2].binding = 2;
    bglEntries[2].visibility = WGPUShaderStage_Fragment;
    bglEntries[2].texture.sampleType = WGPUTextureSampleType_Float;
    bglEntries[2].texture.viewDimension = WGPUTextureViewDimension_2D;

    WGPUBindGroupLayoutDescriptor bglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bglDesc.entryCount = bglEntries.size();
    bglDesc.entries = bglEntries.data();
    bindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(device_, &bglDesc);

    // 4. Pipeline Layout
    WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = bindGroupLayout_.AddressOf();
    gpu::PipelineLayoutHandle pipelineLayout{wgpuDeviceCreatePipelineLayout(device_, &plDesc)};

    // 5. Vertex Layout
    std::array<WGPUVertexAttribute, 3> vertAttrs{};
    // Location 0: pos (vec2<f32>)
    vertAttrs[0].format = WGPUVertexFormat_Float32x2;
    vertAttrs[0].offset = offsetof(TextVertex, pos);
    vertAttrs[0].shaderLocation = 0;
    // Location 1: uv (vec2<f32>)
    vertAttrs[1].format = WGPUVertexFormat_Float32x2;
    vertAttrs[1].offset = offsetof(TextVertex, uv);
    vertAttrs[1].shaderLocation = 1;
    // Location 2: color (vec4<f32>)
    vertAttrs[2].format = WGPUVertexFormat_Float32x4;
    vertAttrs[2].offset = offsetof(TextVertex, color);
    vertAttrs[2].shaderLocation = 2;

    WGPUVertexBufferLayout vbLayout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    vbLayout.arrayStride = sizeof(TextVertex);
    vbLayout.stepMode = WGPUVertexStepMode_Vertex;
    vbLayout.attributeCount = vertAttrs.size();
    vbLayout.attributes = vertAttrs.data();

    // 6. Alpha Blend State
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

    // 7. Render Pipeline (Overlays draw over 2D swapchain pass without depth stencil attachment)
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

    // 8. Create Bind Group
    std::array<WGPUBindGroupEntry, 3> bgEntries{};
    bgEntries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntries[0].binding = 0;
    bgEntries[0].buffer = uniformBuffer_;
    bgEntries[0].offset = 0;
    bgEntries[0].size = sizeof(glm::mat4);

    bgEntries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntries[1].binding = 1;
    bgEntries[1].sampler = fontAtlas_->GetGPUSampler();

    bgEntries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntries[2].binding = 2;
    bgEntries[2].textureView = fontAtlas_->GetGPUTextureView();

    WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgDesc.layout = bindGroupLayout_;
    bgDesc.entryCount = bgEntries.size();
    bgDesc.entries = bgEntries.data();
    bindGroup_ = wgpuDeviceCreateBindGroup(device_, &bgDesc);

    BGL_LOG_INFO("TextRenderer initialized: format={}, sampleCount={}", static_cast<int>(surfaceFormat_), sampleCount_);
    return {};
}

void TextRenderer::EnsureBuffers(size_t vertexCount, size_t indexCount) {
    if (vertexCount > vertexBufferCapacity_) {
        vertexBufferCapacity_ = std::max(vertexCount * 2, size_t{1024});
        WGPUBufferDescriptor vbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
        vbDesc.size = (vertexBufferCapacity_ * sizeof(TextVertex) + 3) & ~3;
        vbDesc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        vbDesc.label = gpu::ToWgpuStringView("TextVertexBuffer");
        vertexBuffer_ = wgpuDeviceCreateBuffer(device_, &vbDesc);
    }

    if (indexCount > indexBufferCapacity_) {
        indexBufferCapacity_ = std::max(indexCount * 2, size_t{2048});
        WGPUBufferDescriptor ibDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
        ibDesc.size = (indexBufferCapacity_ * sizeof(uint32_t) + 3) & ~3;
        ibDesc.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
        ibDesc.label = gpu::ToWgpuStringView("TextIndexBuffer");
        indexBuffer_ = wgpuDeviceCreateBuffer(device_, &ibDesc);
    }
}

void TextRenderer::DrawRect(float x, float y, float width, float height, const glm::vec4& color) {
    if (!fontAtlas_ || width <= 0.0f || height <= 0.0f) return;

    glm::vec2 whiteUV = fontAtlas_->GetWhitePixelUV();
    uint32_t baseIdx = static_cast<uint32_t>(vertexBatch_.size());

    vertexBatch_.push_back(TextVertex{ .pos = glm::vec2(x, y),                 .uv = whiteUV, .color = color });
    vertexBatch_.push_back(TextVertex{ .pos = glm::vec2(x + width, y),         .uv = whiteUV, .color = color });
    vertexBatch_.push_back(TextVertex{ .pos = glm::vec2(x + width, y + height), .uv = whiteUV, .color = color });
    vertexBatch_.push_back(TextVertex{ .pos = glm::vec2(x, y + height),         .uv = whiteUV, .color = color });

    indexBatch_.push_back(baseIdx + 0);
    indexBatch_.push_back(baseIdx + 1);
    indexBatch_.push_back(baseIdx + 2);
    indexBatch_.push_back(baseIdx + 0);
    indexBatch_.push_back(baseIdx + 2);
    indexBatch_.push_back(baseIdx + 3);
}

void TextRenderer::DrawRectWithBorder(float x, float y, float width, float height,
                                      const glm::vec4& fillColor, const glm::vec4& borderColor,
                                      float borderWidth) {
    // Fill interior
    DrawRect(x, y, width, height, fillColor);

    // 4 Border strips
    DrawRect(x, y, width, borderWidth, borderColor);                                  // Top
    DrawRect(x, y + height - borderWidth, width, borderWidth, borderColor);           // Bottom
    DrawRect(x, y + borderWidth, borderWidth, height - 2.0f * borderWidth, borderColor); // Left
    DrawRect(x + width - borderWidth, y + borderWidth, borderWidth, height - 2.0f * borderWidth, borderColor); // Right
}

void TextRenderer::DrawQuads(std::span<const TextQuad> quads) {
    for (const auto& quad : quads) {
        const uint32_t baseIdx{static_cast<uint32_t>(vertexBatch_.size())};

        vertexBatch_.push_back(TextVertex{
            .pos = glm::vec2{quad.posMin.x, quad.posMin.y},
            .uv = glm::vec2{quad.uvMin.x, quad.uvMin.y},
            .color = quad.color
        });
        vertexBatch_.push_back(TextVertex{
            .pos = glm::vec2{quad.posMax.x, quad.posMin.y},
            .uv = glm::vec2{quad.uvMax.x, quad.uvMin.y},
            .color = quad.color
        });
        vertexBatch_.push_back(TextVertex{
            .pos = glm::vec2{quad.posMax.x, quad.posMax.y},
            .uv = glm::vec2{quad.uvMax.x, quad.uvMax.y},
            .color = quad.color
        });
        vertexBatch_.push_back(TextVertex{
            .pos = glm::vec2{quad.posMin.x, quad.posMax.y},
            .uv = glm::vec2{quad.uvMin.x, quad.uvMax.y},
            .color = quad.color
        });

        indexBatch_.push_back(baseIdx + 0);
        indexBatch_.push_back(baseIdx + 1);
        indexBatch_.push_back(baseIdx + 2);
        indexBatch_.push_back(baseIdx + 0);
        indexBatch_.push_back(baseIdx + 2);
        indexBatch_.push_back(baseIdx + 3);
    }
}

void TextRenderer::DrawText(std::string_view text, float x, float y,
                            const glm::vec4& color, float scale) {
    if (!fontAtlas_ || text.empty()) return;

    const TextLayout layout{TextShaper::Shape(*fontAtlas_, text, x, y, color, scale)};
    DrawQuads(std::span<const TextQuad>{layout.quads});
}

void TextRenderer::Render(WGPURenderPassEncoder pass, uint32_t viewportWidth, uint32_t viewportHeight) {
    if (!pass || !pipeline_ || indexBatch_.empty() || viewportWidth == 0 || viewportHeight == 0) {
        vertexBatch_.clear();
        indexBatch_.clear();
        return;
    }

    // 1. Sync atlas texture if any new glyphs were packed
    fontAtlas_->SyncGPU(device_, queue_);

    // 2. Upload orthographic projection matrix (top-left 0,0 to bottom-right W,H)
    glm::mat4 ortho = glm::ortho(0.0f, static_cast<float>(viewportWidth), static_cast<float>(viewportHeight), 0.0f, -1.0f, 1.0f);
    wgpuQueueWriteBuffer(queue_, uniformBuffer_, 0, &ortho, sizeof(glm::mat4));

    // 3. Ensure GPU vertex and index buffers are sized appropriately and upload
    EnsureBuffers(vertexBatch_.size(), indexBatch_.size());
    wgpuQueueWriteBuffer(queue_, vertexBuffer_, 0, vertexBatch_.data(), vertexBatch_.size() * sizeof(TextVertex));
    wgpuQueueWriteBuffer(queue_, indexBuffer_, 0, indexBatch_.data(), indexBatch_.size() * sizeof(uint32_t));

    // 4. Encode render commands
    wgpuRenderPassEncoderSetPipeline(pass, pipeline_);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bindGroup_, 0, nullptr);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, vertexBuffer_, 0, vertexBatch_.size() * sizeof(TextVertex));
    wgpuRenderPassEncoderSetIndexBuffer(pass, indexBuffer_, WGPUIndexFormat_Uint32, 0, indexBatch_.size() * sizeof(uint32_t));
    wgpuRenderPassEncoderDrawIndexed(pass, static_cast<uint32_t>(indexBatch_.size()), 1, 0, 0, 0);

    // 5. Clear batch for next frame
    vertexBatch_.clear();
    indexBatch_.clear();
}

} // namespace bgl::text

