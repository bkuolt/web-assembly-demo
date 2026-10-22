/**
 * @file Pipeline.cpp
 * @brief Implementation of hardware native WebGPU RenderPipeline and glTF mesh rendering.
 */
#include "gpu/Pipeline.hpp"
#include "gpu/Shaders.hpp"
#include "scene/Scene.hpp"
#include "core/Log.hpp"
#include <cstring>
#include <algorithm>

namespace bgl::gpu {

PipelineManager::~PipelineManager() {
    if (defaultSampler_) {
        wgpuSamplerRelease(defaultSampler_);
        defaultSampler_ = nullptr;
    }
    if (defaultTextureView_) {
        wgpuTextureViewRelease(defaultTextureView_);
        defaultTextureView_ = nullptr;
    }
    if (defaultTexture_) {
        wgpuTextureRelease(defaultTexture_);
        defaultTexture_ = nullptr;
    }
    if (wireframePipeline_) {
        wgpuRenderPipelineRelease(wireframePipeline_);
        wireframePipeline_ = nullptr;
    }
    if (opaquePipeline_) {
        wgpuRenderPipelineRelease(opaquePipeline_);
        opaquePipeline_ = nullptr;
    }
    if (bindGroupLayout_) {
        wgpuBindGroupLayoutRelease(bindGroupLayout_);
        bindGroupLayout_ = nullptr;
    }
}

core::Result<void> PipelineManager::Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat surfaceFormat) {
    device_ = device;
    queue_ = queue;
    surfaceFormat_ = surfaceFormat;
    if (!device_ || !queue_) {
        return std::unexpected("Cannot initialize PipelineManager without valid WGPUDevice and WGPUQueue");
    }

    // 1. Compile WGSL PBR Shader Module
    WGPUShaderSourceWGSL wgslDesc = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgslDesc.code = WGPUStringView{ kPbrShaderWGSL.data(), kPbrShaderWGSL.size() };

    WGPUShaderModuleDescriptor smDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    smDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&wgslDesc);
    smDesc.label = WGPUStringView{ "PBR_WGSL", strlen("PBR_WGSL") };

    WGPUShaderModule pbrShaderModule = wgpuDeviceCreateShaderModule(device_, &smDesc);
    if (!pbrShaderModule) {
        return std::unexpected("Failed to compile embedded WGSL PBR shader module");
    }

    // 2. Compile WGSL Wireframe Shader Module
    WGPUShaderSourceWGSL wfWgslDesc = WGPU_SHADER_SOURCE_WGSL_INIT;
    wfWgslDesc.code = WGPUStringView{ kWireframeShaderWGSL.data(), kWireframeShaderWGSL.size() };

    WGPUShaderModuleDescriptor wfSmDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    wfSmDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&wfWgslDesc);
    wfSmDesc.label = WGPUStringView{ "Wireframe_WGSL", strlen("Wireframe_WGSL") };

    WGPUShaderModule wfShaderModule = wgpuDeviceCreateShaderModule(device_, &wfSmDesc);

    // 3. Bind Group Layout: (0: UniformBuffer, 1: Texture2D, 2: Sampler)
    WGPUBindGroupLayoutEntry bglEntries[3] = {};

    // Binding 0: Uniform buffer
    bglEntries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[0].binding = 0;
    bglEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    bglEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
    bglEntries[0].buffer.minBindingSize = sizeof(PrimitiveUniforms);

    // Binding 1: Base color texture
    bglEntries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[1].binding = 1;
    bglEntries[1].visibility = WGPUShaderStage_Fragment;
    bglEntries[1].texture.sampleType = WGPUTextureSampleType_Float;
    bglEntries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
    bglEntries[1].texture.multisampled = WGPU_FALSE;

    // Binding 2: Base color sampler
    bglEntries[2] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[2].binding = 2;
    bglEntries[2].visibility = WGPUShaderStage_Fragment;
    bglEntries[2].sampler.type = WGPUSamplerBindingType_Filtering;

    WGPUBindGroupLayoutDescriptor bglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bglDesc.entryCount = 3;
    bglDesc.entries = bglEntries;
    bindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(device_, &bglDesc);

    // 4. Pipeline Layout
    WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &bindGroupLayout_;
    WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(device_, &plDesc);

    // 5. Vertex Layout (Position, Normal, Tangent, UV)
    WGPUVertexAttribute vertAttrs[4] = {};
    // Position (vec3<f32>)
    vertAttrs[0].format = WGPUVertexFormat_Float32x3;
    vertAttrs[0].offset = offsetof(core::Vertex, position);
    vertAttrs[0].shaderLocation = 0;
    // Normal (vec3<f32>)
    vertAttrs[1].format = WGPUVertexFormat_Float32x3;
    vertAttrs[1].offset = offsetof(core::Vertex, normal);
    vertAttrs[1].shaderLocation = 1;
    // Tangent (vec4<f32>)
    vertAttrs[2].format = WGPUVertexFormat_Float32x4;
    vertAttrs[2].offset = offsetof(core::Vertex, tangent);
    vertAttrs[2].shaderLocation = 2;
    // UV (vec2<f32>)
    vertAttrs[3].format = WGPUVertexFormat_Float32x2;
    vertAttrs[3].offset = offsetof(core::Vertex, uv);
    vertAttrs[3].shaderLocation = 3;

    WGPUVertexBufferLayout vbLayout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    vbLayout.arrayStride = sizeof(core::Vertex);
    vbLayout.stepMode = WGPUVertexStepMode_Vertex;
    vbLayout.attributeCount = 4;
    vbLayout.attributes = vertAttrs;

    // 6. Color Target & Depth State
    WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
    colorTarget.format = surfaceFormat_;
    colorTarget.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragmentState = WGPU_FRAGMENT_STATE_INIT;
    fragmentState.module = pbrShaderModule;
    fragmentState.entryPoint = WGPUStringView{ "fs_main", strlen("fs_main") };
    fragmentState.targetCount = 1;
    fragmentState.targets = &colorTarget;

    WGPUDepthStencilState depthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    depthStencil.format = WGPUTextureFormat_Depth24Plus;
    depthStencil.depthWriteEnabled = WGPUOptionalBool_True;
    depthStencil.depthCompare = WGPUCompareFunction_LessEqual;

    // 7. Create Opaque Render Pipeline
    WGPURenderPipelineDescriptor rpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rpDesc.layout = pipelineLayout;
    rpDesc.vertex.module = pbrShaderModule;
    rpDesc.vertex.entryPoint = WGPUStringView{ "vs_main", strlen("vs_main") };
    rpDesc.vertex.bufferCount = 1;
    rpDesc.vertex.buffers = &vbLayout;
    rpDesc.fragment = &fragmentState;
    rpDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rpDesc.primitive.cullMode = WGPUCullMode_None;
    rpDesc.depthStencil = &depthStencil;

    opaquePipeline_ = wgpuDeviceCreateRenderPipeline(device_, &rpDesc);

    // 8. Create Wireframe Render Pipeline
    if (wfShaderModule) {
        WGPUFragmentState wfFragState = WGPU_FRAGMENT_STATE_INIT;
        wfFragState.module = wfShaderModule;
        wfFragState.entryPoint = WGPUStringView{ "fs_main", strlen("fs_main") };
        wfFragState.targetCount = 1;
        wfFragState.targets = &colorTarget;

        WGPUDepthStencilState wfDepthStencil = depthStencil;
        wfDepthStencil.depthWriteEnabled = WGPUOptionalBool_False;

        WGPURenderPipelineDescriptor wfRpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        wfRpDesc.layout = pipelineLayout;
        wfRpDesc.vertex.module = wfShaderModule;
        wfRpDesc.vertex.entryPoint = WGPUStringView{ "vs_main", strlen("vs_main") };
        wfRpDesc.vertex.bufferCount = 1;
        wfRpDesc.vertex.buffers = &vbLayout;
        wfRpDesc.fragment = &wfFragState;
        wfRpDesc.primitive.topology = WGPUPrimitiveTopology_LineList;
        wfRpDesc.primitive.cullMode = WGPUCullMode_None;
        wfRpDesc.depthStencil = &wfDepthStencil;

        wireframePipeline_ = wgpuDeviceCreateRenderPipeline(device_, &wfRpDesc);
        wgpuShaderModuleRelease(wfShaderModule);
    }

    wgpuPipelineLayoutRelease(pipelineLayout);
    wgpuShaderModuleRelease(pbrShaderModule);

    // 9. Create 1x1 White Fallback Texture
    WGPUTextureDescriptor texDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    texDesc.dimension = WGPUTextureDimension_2D;
    texDesc.size = WGPUExtent3D{ 1, 1, 1 };
    texDesc.format = WGPUTextureFormat_RGBA8Unorm;
    texDesc.mipLevelCount = 1;
    texDesc.sampleCount = 1;
    defaultTexture_ = wgpuDeviceCreateTexture(device_, &texDesc);

    uint32_t whitePixel = 0xFFFFFFFF;
    WGPUTexelCopyTextureInfo copyDst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    copyDst.texture = defaultTexture_;
    copyDst.mipLevel = 0;
    copyDst.origin = WGPUOrigin3D{ 0, 0, 0 };
    copyDst.aspect = WGPUTextureAspect_All;

    WGPUTexelCopyBufferLayout dataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    dataLayout.offset = 0;
    dataLayout.bytesPerRow = 4;
    dataLayout.rowsPerImage = 1;

    WGPUExtent3D writeSize = { 1, 1, 1 };
    wgpuQueueWriteTexture(queue_, &copyDst, &whitePixel, sizeof(whitePixel), &dataLayout, &writeSize);

    defaultTextureView_ = wgpuTextureCreateView(defaultTexture_, nullptr);

    // 10. Create Default Filtering Sampler
    WGPUSamplerDescriptor sampDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sampDesc.addressModeU = WGPUAddressMode_Repeat;
    sampDesc.addressModeV = WGPUAddressMode_Repeat;
    sampDesc.addressModeW = WGPUAddressMode_Repeat;
    sampDesc.magFilter = WGPUFilterMode_Linear;
    sampDesc.minFilter = WGPUFilterMode_Linear;
    sampDesc.mipmapFilter = WGPUMipmapFilterMode_Linear;
    defaultSampler_ = wgpuDeviceCreateSampler(device_, &sampDesc);

    initialized_ = (opaquePipeline_ != nullptr);
    BGL_LOG_INFO("Hardware WebGPU RenderPipeline initialized with WGSL PBR shader!");
    return {};
}

void PipelineManager::PrepareScene(scene::Scene& scene) {
    if (!device_ || !queue_ || !bindGroupLayout_) return;

    // 1. Upload CPU decoded images to WebGPU Textures
    for (size_t i = 0; i < scene.cpuImages.size(); ++i) {
        const auto& img = scene.cpuImages[i];
        if (img.width == 0 || img.height == 0 || img.rgba.empty()) continue;

        if (i < scene.gpuTextures.size() && scene.gpuTextures[i].view == nullptr) {
            WGPUTextureDescriptor texDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
            texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
            texDesc.dimension = WGPUTextureDimension_2D;
            texDesc.size = WGPUExtent3D{ img.width, img.height, 1 };
            texDesc.format = WGPUTextureFormat_RGBA8Unorm;
            texDesc.mipLevelCount = 1;
            texDesc.sampleCount = 1;
            WGPUTexture tex = wgpuDeviceCreateTexture(device_, &texDesc);

            WGPUTexelCopyTextureInfo copyDst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            copyDst.texture = tex;
            copyDst.mipLevel = 0;
            copyDst.origin = WGPUOrigin3D{ 0, 0, 0 };
            copyDst.aspect = WGPUTextureAspect_All;

            WGPUTexelCopyBufferLayout dataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
            dataLayout.offset = 0;
            dataLayout.bytesPerRow = img.width * 4;
            dataLayout.rowsPerImage = img.height;

            WGPUExtent3D writeSize = { img.width, img.height, 1 };
            wgpuQueueWriteTexture(queue_, &copyDst, img.rgba.data(), img.rgba.size(), &dataLayout, &writeSize);

            WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);

            scene.gpuTextures[i].texture = tex;
            scene.gpuTextures[i].view = view;
            scene.gpuTextures[i].sampler = defaultSampler_;
            scene.gpuTextures[i].width = img.width;
            scene.gpuTextures[i].height = img.height;
            BGL_LOG_INFO("[WebGPU Upload] Texture #{}: Created 2D Texture ({}x{}, RGBA8Unorm), wrote {} bytes to GPU (Handle: {:p})",
                         i, img.width, img.height, img.rgba.size(), static_cast<void*>(tex));
        }
    }

    // 2. Prepare vertex buffers, index buffers, uniform buffers, and bind groups for each primitive
    auto prepareNode = [&](auto& self, scene::SceneNode& node) -> void {
        for (size_t pIdx = 0; pIdx < node.primitives.size(); ++pIdx) {
            auto& prim = node.primitives[pIdx];
            if (prim.vertices.empty()) continue;

            size_t vbSize = prim.vertices.size() * sizeof(core::Vertex);
            size_t ibSize = prim.indices.size() * sizeof(uint32_t);

            // Vertex Buffer
            if (!prim.gpuVertexBuffer) {
                WGPUBufferDescriptor vbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
                vbDesc.size = (vbSize + 3) & ~3;
                vbDesc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
                vbDesc.label = WGPUStringView{ "VertexBuffer", strlen("VertexBuffer") };
                prim.gpuVertexBuffer = wgpuDeviceCreateBuffer(device_, &vbDesc);
                wgpuQueueWriteBuffer(queue_, prim.gpuVertexBuffer, 0, prim.vertices.data(), vbSize);
            }

            // Index Buffer
            if (!prim.gpuIndexBuffer && !prim.indices.empty()) {
                WGPUBufferDescriptor ibDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
                ibDesc.size = (ibSize + 3) & ~3;
                ibDesc.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
                ibDesc.label = WGPUStringView{ "IndexBuffer", strlen("IndexBuffer") };
                prim.gpuIndexBuffer = wgpuDeviceCreateBuffer(device_, &ibDesc);
                wgpuQueueWriteBuffer(queue_, prim.gpuIndexBuffer, 0, prim.indices.data(), ibSize);
            }

            // Uniform Buffer
            if (!prim.gpuUniformBuffer) {
                WGPUBufferDescriptor ubDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
                ubDesc.size = sizeof(PrimitiveUniforms);
                ubDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
                ubDesc.label = WGPUStringView{ "PrimitiveUniforms", strlen("PrimitiveUniforms") };
                prim.gpuUniformBuffer = wgpuDeviceCreateBuffer(device_, &ubDesc);
            }

            BGL_LOG_INFO("[WebGPU Upload] Node '{}' Prim #{}: VB={} bytes ({} verts), IB={} bytes ({} indices), UB={} bytes",
                         node.name, pIdx, vbSize, prim.vertices.size(), ibSize, prim.indices.size(), sizeof(PrimitiveUniforms));

            // Bind Group
            if (!prim.gpuBindGroup) {
                WGPUTextureView texView = defaultTextureView_;
                WGPUSampler sampler = defaultSampler_;

                if (prim.materialIndex >= 0 && prim.materialIndex < static_cast<int>(scene.materials.size())) {
                    const auto& mat = scene.materials[prim.materialIndex];
                    if (mat.baseColorTextureIndex >= 0 && mat.baseColorTextureIndex < static_cast<int>(scene.gpuTextures.size())) {
                        if (scene.gpuTextures[mat.baseColorTextureIndex].view) {
                            texView = scene.gpuTextures[mat.baseColorTextureIndex].view;
                        }
                    }
                }

                WGPUBindGroupEntry bgEntries[3] = {};
                bgEntries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
                bgEntries[0].binding = 0;
                bgEntries[0].buffer = prim.gpuUniformBuffer;
                bgEntries[0].offset = 0;
                bgEntries[0].size = sizeof(PrimitiveUniforms);

                bgEntries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
                bgEntries[1].binding = 1;
                bgEntries[1].textureView = texView;

                bgEntries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
                bgEntries[2].binding = 2;
                bgEntries[2].sampler = sampler;

                WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
                bgDesc.layout = bindGroupLayout_;
                bgDesc.entryCount = 3;
                bgDesc.entries = bgEntries;
                prim.gpuBindGroup = wgpuDeviceCreateBindGroup(device_, &bgDesc);
            }
        }

        for (auto& child : node.children) {
            if (child) {
                self(self, *child);
            }
        }
    };

    for (auto& rootNode : scene.rootNodes) {
        if (rootNode) {
            prepareNode(prepareNode, *rootNode);
        }
    }
}

void PipelineManager::RenderScene(WGPURenderPassEncoder pass, scene::Scene& scene, const glm::mat4& viewProj, const glm::vec3& cameraPos, bool wireframe) {
    if (!pass || !initialized_) return;

    WGPURenderPipeline activePipeline = wireframe && wireframePipeline_ ? wireframePipeline_ : opaquePipeline_;
    wgpuRenderPassEncoderSetPipeline(pass, activePipeline);

    for (const auto& rootNode : scene.rootNodes) {
        if (rootNode) {
            RenderNode(pass, scene, *rootNode, viewProj, glm::mat4(1.0f), cameraPos, wireframe);
        }
    }
}

void PipelineManager::RenderNode(WGPURenderPassEncoder pass, scene::Scene& scene, scene::SceneNode& node, const glm::mat4& viewProj, const glm::mat4& parentTransform, const glm::vec3& cameraPos, bool wireframe) {
    glm::mat4 worldMat = parentTransform * node.localTransform;

    for (auto& prim : node.primitives) {
        if (!prim.gpuVertexBuffer || !prim.gpuBindGroup || !prim.gpuUniformBuffer) continue;

        glm::vec4 baseColorFactor{ 1.0f };
        if (prim.materialIndex >= 0 && prim.materialIndex < static_cast<int>(scene.materials.size())) {
            baseColorFactor = scene.materials[prim.materialIndex].baseColorFactor;
        }

        PrimitiveUniforms uniforms;
        uniforms.viewProj = viewProj;
        uniforms.model = worldMat;
        uniforms.cameraPos = glm::vec4(cameraPos, 1.0f);
        uniforms.lightDir = glm::normalize(glm::vec4(0.5f, 1.0f, 0.8f, 0.0f));
        uniforms.baseColorFactor = baseColorFactor;

        wgpuQueueWriteBuffer(queue_, prim.gpuUniformBuffer, 0, &uniforms, sizeof(uniforms));

        wgpuRenderPassEncoderSetBindGroup(pass, 0, prim.gpuBindGroup, 0, nullptr);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, prim.gpuVertexBuffer, 0, prim.vertices.size() * sizeof(core::Vertex));

        if (prim.gpuIndexBuffer && !prim.indices.empty()) {
            wgpuRenderPassEncoderSetIndexBuffer(pass, prim.gpuIndexBuffer, WGPUIndexFormat_Uint32, 0, prim.indices.size() * sizeof(uint32_t));
            wgpuRenderPassEncoderDrawIndexed(pass, static_cast<uint32_t>(prim.indices.size()), 1, 0, 0, 0);
        } else {
            wgpuRenderPassEncoderDraw(pass, static_cast<uint32_t>(prim.vertices.size()), 1, 0, 0);
        }
    }

    for (const auto& child : node.children) {
        if (child) {
            RenderNode(pass, scene, *child, viewProj, worldMat, cameraPos, wireframe);
        }
    }
}

} // namespace bgl::gpu
