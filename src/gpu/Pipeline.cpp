/**
 * @file Pipeline.cpp
 * @brief Implementation of hardware native WebGPU RenderPipeline, Directional Shadow Mapping,
 *        Pre-filtered Environment Cubemap Reflections, Bump Mapping, and Trilinear Mipmapping.
 */
#include "gpu/Pipeline.hpp"
#include "gpu/Shaders.hpp"
#include "scene/Scene.hpp"
#include "scene/Quadtree.hpp"
#include "core/Log.hpp"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>

namespace bgl::gpu {

PipelineManager::~PipelineManager() {
    if (noMipSampler_) {
        wgpuSamplerRelease(noMipSampler_);
        noMipSampler_ = nullptr;
    }
    if (defaultSampler_) {
        wgpuSamplerRelease(defaultSampler_);
        defaultSampler_ = nullptr;
    }
    if (shadowSampler_) {
        wgpuSamplerRelease(shadowSampler_);
        shadowSampler_ = nullptr;
    }
    if (shadowDepthView_) {
        wgpuTextureViewRelease(shadowDepthView_);
        shadowDepthView_ = nullptr;
    }
    if (shadowDepthTexture_) {
        wgpuTextureRelease(shadowDepthTexture_);
        shadowDepthTexture_ = nullptr;
    }
    if (shadowPipeline_) {
        wgpuRenderPipelineRelease(shadowPipeline_);
        shadowPipeline_ = nullptr;
    }
    if (shadowBindGroupLayout_) {
        wgpuBindGroupLayoutRelease(shadowBindGroupLayout_);
        shadowBindGroupLayout_ = nullptr;
    }
    if (envCubemapView_) {
        wgpuTextureViewRelease(envCubemapView_);
        envCubemapView_ = nullptr;
    }
    if (envCubemapTexture_) {
        wgpuTextureRelease(envCubemapTexture_);
        envCubemapTexture_ = nullptr;
    }
    if (defaultBlackTextureView_) {
        wgpuTextureViewRelease(defaultBlackTextureView_);
        defaultBlackTextureView_ = nullptr;
    }
    if (defaultBlackTexture_) {
        wgpuTextureRelease(defaultBlackTexture_);
        defaultBlackTexture_ = nullptr;
    }
    if (defaultNormalTextureView_) {
        wgpuTextureViewRelease(defaultNormalTextureView_);
        defaultNormalTextureView_ = nullptr;
    }
    if (defaultNormalTexture_) {
        wgpuTextureRelease(defaultNormalTexture_);
        defaultNormalTexture_ = nullptr;
    }
    if (defaultWhiteTextureView_) {
        wgpuTextureViewRelease(defaultWhiteTextureView_);
        defaultWhiteTextureView_ = nullptr;
    }
    if (defaultWhiteTexture_) {
        wgpuTextureRelease(defaultWhiteTexture_);
        defaultWhiteTexture_ = nullptr;
    }
    if (aabbBindGroup_) {
        wgpuBindGroupRelease(aabbBindGroup_);
        aabbBindGroup_ = nullptr;
    }
    if (aabbUniformBuffer_) {
        wgpuBufferRelease(aabbUniformBuffer_);
        aabbUniformBuffer_ = nullptr;
    }
    if (aabbVertexBuffer_) {
        wgpuBufferRelease(aabbVertexBuffer_);
        aabbVertexBuffer_ = nullptr;
    }
    if (aabbPipeline_) {
        wgpuRenderPipelineRelease(aabbPipeline_);
        aabbPipeline_ = nullptr;
    }
    if (aabbBindGroupLayout_) {
        wgpuBindGroupLayoutRelease(aabbBindGroupLayout_);
        aabbBindGroupLayout_ = nullptr;
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

uint32_t PipelineManager::CalculateMipLevels(uint32_t width, uint32_t height) noexcept {
    uint32_t maxDim = std::max(width, height);
    if (maxDim == 0) return 1;
    uint32_t levels = 1;
    while ((maxDim >> levels) > 0) {
        levels++;
    }
    return levels;
}

std::vector<uint8_t> PipelineManager::GenerateNextMipLevel(const uint8_t* src, uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH, bool isNormalMap) {
    std::vector<uint8_t> dst(dstW * dstH * 4);
    if (!src || srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0) return dst;

    for (uint32_t y = 0; y < dstH; ++y) {
        for (uint32_t x = 0; x < dstW; ++x) {
            uint32_t srcX0 = std::min(x * 2, srcW - 1);
            uint32_t srcX1 = std::min(x * 2 + 1, srcW - 1);
            uint32_t srcY0 = std::min(y * 2, srcH - 1);
            uint32_t srcY1 = std::min(y * 2 + 1, srcH - 1);

            const uint8_t* p00 = src + (srcY0 * srcW + srcX0) * 4;
            const uint8_t* p10 = src + (srcY0 * srcW + srcX1) * 4;
            const uint8_t* p01 = src + (srcY1 * srcW + srcX0) * 4;
            const uint8_t* p11 = src + (srcY1 * srcW + srcX1) * 4;

            size_t dstIdx = (y * dstW + x) * 4;
            if (isNormalMap) {
                // Vector-renormalized normal map downsampling to preserve surface normal length
                auto unpackN = [](const uint8_t* p) -> glm::vec3 {
                    return glm::vec3(
                        (static_cast<float>(p[0]) / 255.0f) * 2.0f - 1.0f,
                        (static_cast<float>(p[1]) / 255.0f) * 2.0f - 1.0f,
                        (static_cast<float>(p[2]) / 255.0f) * 2.0f - 1.0f
                    );
                };
                glm::vec3 avgN = unpackN(p00) + unpackN(p10) + unpackN(p01) + unpackN(p11);
                float len = glm::length(avgN);
                if (len > 1e-5f) {
                    avgN /= len;
                } else {
                    avgN = glm::vec3(0.0f, 0.0f, 1.0f);
                }
                dst[dstIdx + 0] = static_cast<uint8_t>(std::clamp((avgN.x * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
                dst[dstIdx + 1] = static_cast<uint8_t>(std::clamp((avgN.y * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
                dst[dstIdx + 2] = static_cast<uint8_t>(std::clamp((avgN.z * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
                uint32_t sumA = static_cast<uint32_t>(p00[3]) + p10[3] + p01[3] + p11[3];
                dst[dstIdx + 3] = static_cast<uint8_t>((sumA + 2) / 4);
            } else {
                for (int c = 0; c < 4; ++c) {
                    uint32_t sum = static_cast<uint32_t>(p00[c]) +
                                   static_cast<uint32_t>(p10[c]) +
                                   static_cast<uint32_t>(p01[c]) +
                                   static_cast<uint32_t>(p11[c]);
                    dst[dstIdx + c] = static_cast<uint8_t>((sum + 2) / 4);
                }
            }
        }
    }
    return dst;
}

glm::mat4 PipelineManager::ComputeDirectionalLightMatrix(const core::AABB& sceneBounds, const glm::vec3& lightDir) noexcept {
    glm::vec3 center(0.0f);
    float radius = 20.0f;
    if (sceneBounds.minBound.x <= sceneBounds.maxBound.x) {
        center = sceneBounds.Center();
        radius = std::max(glm::length(sceneBounds.Extents()), 5.0f);
    }

    glm::vec3 dir = glm::length(lightDir) > 1e-5f ? glm::normalize(lightDir) : glm::normalize(glm::vec3(0.35f, 1.0f, 0.25f));
    glm::vec3 up = (std::abs(dir.y) < 0.99f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);

    glm::vec3 lightPos = center + dir * (radius * 1.5f);
    glm::mat4 lightView = glm::lookAtRH(lightPos, center, up);
    glm::mat4 lightProj = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.1f, radius * 3.5f);
    return lightProj * lightView;
}

std::vector<uint8_t> PipelineManager::GenerateEnvCubemapFaceMip(uint32_t faceIndex, uint32_t faceSize, float roughness, const glm::vec3& sunDir) {
    std::vector<uint8_t> pixels(faceSize * faceSize * 4);
    glm::vec3 sun = glm::length(sunDir) > 1e-5f ? glm::normalize(sunDir) : glm::normalize(glm::vec3(0.35f, 1.0f, 0.25f));
    float rough = std::clamp(roughness, 0.0f, 1.0f);

    const glm::vec3 zenithColor(0.14f, 0.38f, 0.78f);
    const glm::vec3 horizonColor(0.72f, 0.84f, 0.96f);
    const glm::vec3 groundColor(0.28f, 0.23f, 0.19f);
    const glm::vec3 warmBounce(0.42f, 0.34f, 0.26f);

    for (uint32_t y = 0; y < faceSize; ++y) {
        for (uint32_t x = 0; x < faceSize; ++x) {
            float u = ((static_cast<float>(x) + 0.5f) / static_cast<float>(faceSize)) * 2.0f - 1.0f;
            float v = ((static_cast<float>(y) + 0.5f) / static_cast<float>(faceSize)) * 2.0f - 1.0f;

            glm::vec3 dir(0.0f);
            switch (faceIndex) {
                case 0: dir = glm::vec3( 1.0f,   -v,   -u); break; // +X
                case 1: dir = glm::vec3(-1.0f,   -v,    u); break; // -X
                case 2: dir = glm::vec3(    u, 1.0f,    v); break; // +Y
                case 3: dir = glm::vec3(    u,-1.0f,   -v); break; // -Y
                case 4: dir = glm::vec3(    u,   -v, 1.0f); break; // +Z
                default:dir = glm::vec3(   -u,   -v,-1.0f); break; // -Z
            }
            dir = glm::normalize(dir);

            // Base sky / ground gradient blurred by roughness
            float elev = dir.y;
            glm::vec3 envColor;
            if (elev >= 0.0f) {
                float horizonBlend = std::pow(std::clamp(1.0f - elev, 0.0f, 1.0f), glm::mix(2.2f, 1.0f, rough));
                envColor = glm::mix(zenithColor, horizonColor, horizonBlend);
            } else {
                float groundBlend = std::clamp(-elev * glm::mix(3.5f, 1.2f, rough), 0.0f, 1.0f);
                envColor = glm::mix(horizonColor, groundColor, groundBlend);
                // Architectural warm courtyard bounce
                float azimuthWave = 0.5f + 0.5f * std::sin(dir.x * 4.0f) * std::cos(dir.z * 4.0f);
                envColor = glm::mix(envColor, warmBounce, azimuthWave * 0.25f * (1.0f - rough * 0.5f));
            }

            // Convolved sun specular highlight (sharp at mip 0, broad and soft at high roughness)
            float sunDot = std::max(glm::dot(dir, sun), 0.0f);
            float specPower = glm::mix(380.0f, 2.5f, std::sqrt(rough));
            float specScale = glm::mix(2.2f, 0.25f, rough);
            glm::vec3 sunContrib = glm::vec3(1.0f, 0.95f, 0.85f) * std::pow(sunDot, specPower) * specScale;

            // Secondary studio rim highlight for metallic reflections
            glm::vec3 rimDir = glm::normalize(glm::vec3(-sun.x, 0.45f, -sun.z));
            float rimDot = std::max(glm::dot(dir, rimDir), 0.0f);
            glm::vec3 rimContrib = glm::vec3(0.65f, 0.80f, 1.0f) * std::pow(rimDot, glm::mix(64.0f, 2.0f, rough)) * glm::mix(0.45f, 0.1f, rough);

            glm::vec3 finalCol = envColor + sunContrib + rimContrib;
            // Tone map into [0..1]
            finalCol = finalCol / (finalCol + glm::vec3(0.65f));

            size_t idx = (y * faceSize + x) * 4;
            pixels[idx + 0] = static_cast<uint8_t>(std::clamp(finalCol.r * 255.0f + 0.5f, 0.0f, 255.0f));
            pixels[idx + 1] = static_cast<uint8_t>(std::clamp(finalCol.g * 255.0f + 0.5f, 0.0f, 255.0f));
            pixels[idx + 2] = static_cast<uint8_t>(std::clamp(finalCol.b * 255.0f + 0.5f, 0.0f, 255.0f));
            pixels[idx + 3] = 255;
        }
    }
    return pixels;
}

core::Result<void> PipelineManager::Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat surfaceFormat, uint32_t sampleCount) {
    device_ = device;
    queue_ = queue;
    surfaceFormat_ = surfaceFormat;
    sampleCount_ = sampleCount;
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

    // 3. Compile WGSL Shadow Shader Module
    WGPUShaderSourceWGSL shadowWgslDesc = WGPU_SHADER_SOURCE_WGSL_INIT;
    shadowWgslDesc.code = WGPUStringView{ kShadowShaderWGSL.data(), kShadowShaderWGSL.size() };

    WGPUShaderModuleDescriptor shadowSmDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    shadowSmDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&shadowWgslDesc);
    shadowSmDesc.label = WGPUStringView{ "Shadow_WGSL", strlen("Shadow_WGSL") };

    WGPUShaderModule shadowShaderModule = wgpuDeviceCreateShaderModule(device_, &shadowSmDesc);

    // 4. Bind Group Layout (13 entries):
    // 0: Uniform buffer
    // 1: Default Sampler
    // 2..9: 2D Material Textures (BaseColor, MetallicRoughness, Normal, Emissive, Occlusion, Clearcoat, Transmission, Sheen)
    // 10: Environment Cubemap (texture_cube<f32>)
    // 11: Shadow Depth Map (texture_depth_2d)
    // 12: Shadow Comparison Sampler (sampler_comparison)
    WGPUBindGroupLayoutEntry bglEntries[13] = {};

    // Binding 0: Uniform buffer
    bglEntries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[0].binding = 0;
    bglEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    bglEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
    bglEntries[0].buffer.minBindingSize = sizeof(PrimitiveUniforms);

    // Binding 1: Default Sampler
    bglEntries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[1].binding = 1;
    bglEntries[1].visibility = WGPUShaderStage_Fragment;
    bglEntries[1].sampler.type = WGPUSamplerBindingType_Filtering;

    // Bindings 2..9: 2D Sampled Textures
    for (uint32_t b = 2; b < 10; ++b) {
        bglEntries[b] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        bglEntries[b].binding = b;
        bglEntries[b].visibility = WGPUShaderStage_Fragment;
        bglEntries[b].texture.sampleType = WGPUTextureSampleType_Float;
        bglEntries[b].texture.viewDimension = WGPUTextureViewDimension_2D;
        bglEntries[b].texture.multisampled = WGPU_FALSE;
    }

    // Binding 10: Environment Cubemap
    bglEntries[10] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[10].binding = 10;
    bglEntries[10].visibility = WGPUShaderStage_Fragment;
    bglEntries[10].texture.sampleType = WGPUTextureSampleType_Float;
    bglEntries[10].texture.viewDimension = WGPUTextureViewDimension_Cube;
    bglEntries[10].texture.multisampled = WGPU_FALSE;

    // Binding 11: Directional Shadow Map Depth Texture
    bglEntries[11] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[11].binding = 11;
    bglEntries[11].visibility = WGPUShaderStage_Fragment;
    bglEntries[11].texture.sampleType = WGPUTextureSampleType_Depth;
    bglEntries[11].texture.viewDimension = WGPUTextureViewDimension_2D;
    bglEntries[11].texture.multisampled = WGPU_FALSE;

    // Binding 12: Shadow Comparison Sampler
    bglEntries[12] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[12].binding = 12;
    bglEntries[12].visibility = WGPUShaderStage_Fragment;
    bglEntries[12].sampler.type = WGPUSamplerBindingType_Comparison;

    WGPUBindGroupLayoutDescriptor bglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bglDesc.entryCount = 13;
    bglDesc.entries = bglEntries;
    bindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(device_, &bglDesc);
    if (!bindGroupLayout_) {
        return std::unexpected("Failed to create WebGPU BindGroupLayout for PBR material");
    }

    // 4b. Shadow Pass Bind Group Layout (Binding 0 only to avoid binding shadowMap while writing to it)
    WGPUBindGroupLayoutEntry shadowBglEntry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    shadowBglEntry.binding = 0;
    shadowBglEntry.visibility = WGPUShaderStage_Vertex;
    shadowBglEntry.buffer.type = WGPUBufferBindingType_Uniform;
    shadowBglEntry.buffer.minBindingSize = sizeof(PrimitiveUniforms);

    WGPUBindGroupLayoutDescriptor shadowBglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    shadowBglDesc.entryCount = 1;
    shadowBglDesc.entries = &shadowBglEntry;
    shadowBindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(device_, &shadowBglDesc);

    // 5. Pipeline Layouts
    WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &bindGroupLayout_;
    WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(device_, &plDesc);

    WGPUPipelineLayoutDescriptor shadowPlDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    shadowPlDesc.bindGroupLayoutCount = 1;
    shadowPlDesc.bindGroupLayouts = &shadowBindGroupLayout_;
    WGPUPipelineLayout shadowPipelineLayout = wgpuDeviceCreatePipelineLayout(device_, &shadowPlDesc);

    // 6. Vertex Layout (Position, Normal, Tangent, UV)
    WGPUVertexAttribute vertAttrs[4] = {};
    vertAttrs[0].format = WGPUVertexFormat_Float32x3;
    vertAttrs[0].offset = offsetof(core::Vertex, position);
    vertAttrs[0].shaderLocation = 0;

    vertAttrs[1].format = WGPUVertexFormat_Float32x3;
    vertAttrs[1].offset = offsetof(core::Vertex, normal);
    vertAttrs[1].shaderLocation = 1;

    vertAttrs[2].format = WGPUVertexFormat_Float32x4;
    vertAttrs[2].offset = offsetof(core::Vertex, tangent);
    vertAttrs[2].shaderLocation = 2;

    vertAttrs[3].format = WGPUVertexFormat_Float32x2;
    vertAttrs[3].offset = offsetof(core::Vertex, uv);
    vertAttrs[3].shaderLocation = 3;

    WGPUVertexBufferLayout vbLayout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    vbLayout.arrayStride = sizeof(core::Vertex);
    vbLayout.stepMode = WGPUVertexStepMode_Vertex;
    vbLayout.attributeCount = 4;
    vbLayout.attributes = vertAttrs;

    // 7. Color Target & Depth State
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

    // 8. Create Opaque Render Pipeline
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
    rpDesc.multisample.count = sampleCount_;
    rpDesc.multisample.mask = 0xFFFFFFFF;
    rpDesc.multisample.alphaToCoverageEnabled = WGPU_FALSE;

    opaquePipeline_ = wgpuDeviceCreateRenderPipeline(device_, &rpDesc);

    // 8b. Create Shadow Depth Render Pipeline
    if (shadowShaderModule) {
        WGPUDepthStencilState shadowDepthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
        shadowDepthStencil.format = WGPUTextureFormat_Depth32Float;
        shadowDepthStencil.depthWriteEnabled = WGPUOptionalBool_True;
        shadowDepthStencil.depthCompare = WGPUCompareFunction_LessEqual;
        shadowDepthStencil.depthBias = 2;
        shadowDepthStencil.depthBiasSlopeScale = 2.0f;
        shadowDepthStencil.depthBiasClamp = 0.0f;

        WGPURenderPipelineDescriptor shadowRpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        shadowRpDesc.layout = shadowPipelineLayout;
        shadowRpDesc.vertex.module = shadowShaderModule;
        shadowRpDesc.vertex.entryPoint = WGPUStringView{ "vs_shadow", strlen("vs_shadow") };
        shadowRpDesc.vertex.bufferCount = 1;
        shadowRpDesc.vertex.buffers = &vbLayout;
        shadowRpDesc.fragment = nullptr; // Depth-only pass
        shadowRpDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        shadowRpDesc.primitive.cullMode = WGPUCullMode_None;
        shadowRpDesc.depthStencil = &shadowDepthStencil;
        shadowRpDesc.multisample.count = 1;
        shadowRpDesc.multisample.mask = 0xFFFFFFFF;
        shadowRpDesc.multisample.alphaToCoverageEnabled = WGPU_FALSE;

        shadowPipeline_ = wgpuDeviceCreateRenderPipeline(device_, &shadowRpDesc);
        wgpuShaderModuleRelease(shadowShaderModule);
    }
    wgpuPipelineLayoutRelease(shadowPipelineLayout);

    // 8c. Create Wireframe Render Pipeline
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
        wfRpDesc.multisample.count = sampleCount_;
        wfRpDesc.multisample.mask = 0xFFFFFFFF;
        wfRpDesc.multisample.alphaToCoverageEnabled = WGPU_FALSE;

        wireframePipeline_ = wgpuDeviceCreateRenderPipeline(device_, &wfRpDesc);
        wgpuShaderModuleRelease(wfShaderModule);
    }

    // 8d. Create AABB Wireframe Render Pipeline
    WGPUShaderSourceWGSL aabbWgslDesc = WGPU_SHADER_SOURCE_WGSL_INIT;
    aabbWgslDesc.code = WGPUStringView{ kAabbShaderWGSL.data(), kAabbShaderWGSL.size() };

    WGPUShaderModuleDescriptor aabbSmDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    aabbSmDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&aabbWgslDesc);
    aabbSmDesc.label = WGPUStringView{ "AABB_WGSL", strlen("AABB_WGSL") };
    WGPUShaderModule aabbShaderModule = wgpuDeviceCreateShaderModule(device_, &aabbSmDesc);

    WGPUBindGroupLayoutEntry aabbBglEntry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    aabbBglEntry.binding = 0;
    aabbBglEntry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    aabbBglEntry.buffer.type = WGPUBufferBindingType_Uniform;
    aabbBglEntry.buffer.minBindingSize = sizeof(AabbUniforms);

    WGPUBindGroupLayoutDescriptor aabbBglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    aabbBglDesc.entryCount = 1;
    aabbBglDesc.entries = &aabbBglEntry;
    aabbBindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(device_, &aabbBglDesc);

    WGPUPipelineLayoutDescriptor aabbPlDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    aabbPlDesc.bindGroupLayoutCount = 1;
    aabbPlDesc.bindGroupLayouts = &aabbBindGroupLayout_;
    WGPUPipelineLayout aabbPlLayout = wgpuDeviceCreatePipelineLayout(device_, &aabbPlDesc);

    WGPUVertexAttribute aabbAttrs[2] = {};
    aabbAttrs[0].format = WGPUVertexFormat_Float32x3;
    aabbAttrs[0].offset = 0;
    aabbAttrs[0].shaderLocation = 0;

    aabbAttrs[1].format = WGPUVertexFormat_Float32x4;
    aabbAttrs[1].offset = 16;
    aabbAttrs[1].shaderLocation = 1;

    WGPUVertexBufferLayout aabbVbLayout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    aabbVbLayout.arrayStride = sizeof(AabbVertex);
    aabbVbLayout.stepMode = WGPUVertexStepMode_Vertex;
    aabbVbLayout.attributeCount = 2;
    aabbVbLayout.attributes = aabbAttrs;

    WGPUFragmentState aabbFragState = WGPU_FRAGMENT_STATE_INIT;
    aabbFragState.module = aabbShaderModule;
    aabbFragState.entryPoint = WGPUStringView{ "fs_aabb", strlen("fs_aabb") };
    aabbFragState.targetCount = 1;
    aabbFragState.targets = &colorTarget;

    WGPUDepthStencilState aabbDepthStencil = depthStencil;
    aabbDepthStencil.depthWriteEnabled = WGPUOptionalBool_False;
    aabbDepthStencil.depthCompare = WGPUCompareFunction_LessEqual;

    WGPURenderPipelineDescriptor aabbRpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    aabbRpDesc.layout = aabbPlLayout;
    aabbRpDesc.vertex.module = aabbShaderModule;
    aabbRpDesc.vertex.entryPoint = WGPUStringView{ "vs_aabb", strlen("vs_aabb") };
    aabbRpDesc.vertex.bufferCount = 1;
    aabbRpDesc.vertex.buffers = &aabbVbLayout;
    aabbRpDesc.fragment = &aabbFragState;
    aabbRpDesc.primitive.topology = WGPUPrimitiveTopology_LineList;
    aabbRpDesc.primitive.cullMode = WGPUCullMode_None;
    aabbRpDesc.depthStencil = &aabbDepthStencil;
    aabbRpDesc.multisample.count = sampleCount_;
    aabbRpDesc.multisample.mask = 0xFFFFFFFF;
    aabbRpDesc.multisample.alphaToCoverageEnabled = WGPU_FALSE;

    aabbPipeline_ = wgpuDeviceCreateRenderPipeline(device_, &aabbRpDesc);

    wgpuPipelineLayoutRelease(aabbPlLayout);
    wgpuShaderModuleRelease(aabbShaderModule);

    aabbVertexBufferSize_ = 512 * 1024;
    WGPUBufferDescriptor boxVbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    boxVbDesc.size = aabbVertexBufferSize_;
    boxVbDesc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    boxVbDesc.label = WGPUStringView{ "AABB_DynamicVertexBuffer", strlen("AABB_DynamicVertexBuffer") };
    aabbVertexBuffer_ = wgpuDeviceCreateBuffer(device_, &boxVbDesc);

    WGPUBufferDescriptor aabbUbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    aabbUbDesc.size = sizeof(AabbUniforms);
    aabbUbDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    aabbUbDesc.label = WGPUStringView{ "AABB_Uniforms", strlen("AABB_Uniforms") };
    aabbUniformBuffer_ = wgpuDeviceCreateBuffer(device_, &aabbUbDesc);

    WGPUBindGroupEntry aabbBgEntry = WGPU_BIND_GROUP_ENTRY_INIT;
    aabbBgEntry.binding = 0;
    aabbBgEntry.buffer = aabbUniformBuffer_;
    aabbBgEntry.offset = 0;
    aabbBgEntry.size = sizeof(AabbUniforms);

    WGPUBindGroupDescriptor aabbBgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    aabbBgDesc.layout = aabbBindGroupLayout_;
    aabbBgDesc.entryCount = 1;
    aabbBgDesc.entries = &aabbBgEntry;
    aabbBindGroup_ = wgpuDeviceCreateBindGroup(device_, &aabbBgDesc);

    wgpuPipelineLayoutRelease(pipelineLayout);
    wgpuShaderModuleRelease(pbrShaderModule);

    // 9. Create Shadow Map 2048x2048 Depth32Float Texture & Comparison Sampler
    WGPUTextureDescriptor shadowTexDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    shadowTexDesc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    shadowTexDesc.dimension = WGPUTextureDimension_2D;
    shadowTexDesc.size = WGPUExtent3D{ kShadowMapSize, kShadowMapSize, 1 };
    shadowTexDesc.format = WGPUTextureFormat_Depth32Float;
    shadowTexDesc.mipLevelCount = 1;
    shadowTexDesc.sampleCount = 1;
    shadowTexDesc.label = WGPUStringView{ "DirectionalShadowMap", strlen("DirectionalShadowMap") };
    shadowDepthTexture_ = wgpuDeviceCreateTexture(device_, &shadowTexDesc);
    if (shadowDepthTexture_) {
        shadowDepthView_ = wgpuTextureCreateView(shadowDepthTexture_, nullptr);
    }

    WGPUSamplerDescriptor shadowSampDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    shadowSampDesc.addressModeU = WGPUAddressMode_ClampToEdge;
    shadowSampDesc.addressModeV = WGPUAddressMode_ClampToEdge;
    shadowSampDesc.addressModeW = WGPUAddressMode_ClampToEdge;
    shadowSampDesc.magFilter = WGPUFilterMode_Linear;
    shadowSampDesc.minFilter = WGPUFilterMode_Linear;
    shadowSampDesc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    shadowSampDesc.compare = WGPUCompareFunction_LessEqual;
    shadowSampDesc.maxAnisotropy = 1;
    shadowSampler_ = wgpuDeviceCreateSampler(device_, &shadowSampDesc);

    // Initialize shadow map with cleared depth (1.0f) so first frame has valid depth contents even if shadows are toggled off
    if (shadowDepthView_) {
        WGPUCommandEncoder initEnc = wgpuDeviceCreateCommandEncoder(device_, nullptr);
        WGPURenderPassDepthStencilAttachment clearDepth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
        clearDepth.view = shadowDepthView_;
        clearDepth.depthLoadOp = WGPULoadOp_Clear;
        clearDepth.depthStoreOp = WGPUStoreOp_Store;
        clearDepth.depthClearValue = 1.0f;

        WGPURenderPassDescriptor clearPassDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        clearPassDesc.colorAttachmentCount = 0;
        clearPassDesc.depthStencilAttachment = &clearDepth;
        WGPURenderPassEncoder clearPass = wgpuCommandEncoderBeginRenderPass(initEnc, &clearPassDesc);
        wgpuRenderPassEncoderEnd(clearPass);
        wgpuRenderPassEncoderRelease(clearPass);
        WGPUCommandBuffer initCmd = wgpuCommandEncoderFinish(initEnc, nullptr);
        wgpuCommandEncoderRelease(initEnc);
        wgpuQueueSubmit(queue_, 1, &initCmd);
        wgpuCommandBufferRelease(initCmd);
    }

    // 10. Create Pre-filtered Environment Cubemap (6 faces, 7 mip levels for roughness IBL)
    envMipLevels_ = CalculateMipLevels(kEnvCubemapSize, kEnvCubemapSize);
    WGPUTextureDescriptor cubeDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    cubeDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    cubeDesc.dimension = WGPUTextureDimension_2D;
    cubeDesc.size = WGPUExtent3D{ kEnvCubemapSize, kEnvCubemapSize, 6 };
    cubeDesc.format = WGPUTextureFormat_RGBA8Unorm;
    cubeDesc.mipLevelCount = envMipLevels_;
    cubeDesc.sampleCount = 1;
    cubeDesc.label = WGPUStringView{ "EnvironmentCubemap", strlen("EnvironmentCubemap") };
    envCubemapTexture_ = wgpuDeviceCreateTexture(device_, &cubeDesc);

    for (uint32_t mip = 0; mip < envMipLevels_; ++mip) {
        uint32_t mipSize = std::max(1u, kEnvCubemapSize >> mip);
        float roughness = (envMipLevels_ > 1) ? static_cast<float>(mip) / static_cast<float>(envMipLevels_ - 1) : 0.0f;

        for (uint32_t face = 0; face < 6; ++face) {
            std::vector<uint8_t> facePixels = GenerateEnvCubemapFaceMip(face, mipSize, roughness, currentLightDir_);

            WGPUTexelCopyTextureInfo copyDst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            copyDst.texture = envCubemapTexture_;
            copyDst.mipLevel = mip;
            copyDst.origin = WGPUOrigin3D{ 0, 0, face };
            copyDst.aspect = WGPUTextureAspect_All;

            WGPUTexelCopyBufferLayout dataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
            dataLayout.offset = 0;
            dataLayout.bytesPerRow = mipSize * 4;
            dataLayout.rowsPerImage = mipSize;

            WGPUExtent3D writeSize = { mipSize, mipSize, 1 };
            wgpuQueueWriteTexture(queue_, &copyDst, facePixels.data(), facePixels.size(), &dataLayout, &writeSize);
        }
    }

    WGPUTextureViewDescriptor cubeViewDesc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    cubeViewDesc.format = WGPUTextureFormat_RGBA8Unorm;
    cubeViewDesc.dimension = WGPUTextureViewDimension_Cube;
    cubeViewDesc.baseMipLevel = 0;
    cubeViewDesc.mipLevelCount = envMipLevels_;
    cubeViewDesc.baseArrayLayer = 0;
    cubeViewDesc.arrayLayerCount = 6;
    cubeViewDesc.aspect = WGPUTextureAspect_All;
    envCubemapView_ = wgpuTextureCreateView(envCubemapTexture_, &cubeViewDesc);

    // 11. Helper to create 1x1 fallback textures
    auto create1x1Texture = [&](const uint8_t rgba[4], const char* label) -> std::pair<WGPUTexture, WGPUTextureView> {
        WGPUTextureDescriptor texDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        texDesc.dimension = WGPUTextureDimension_2D;
        texDesc.size = WGPUExtent3D{ 1, 1, 1 };
        texDesc.format = WGPUTextureFormat_RGBA8Unorm;
        texDesc.mipLevelCount = 1;
        texDesc.sampleCount = 1;
        texDesc.label = WGPUStringView{ label, strlen(label) };
        WGPUTexture tex = wgpuDeviceCreateTexture(device_, &texDesc);

        WGPUTexelCopyTextureInfo copyDst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        copyDst.texture = tex;
        copyDst.mipLevel = 0;
        copyDst.origin = WGPUOrigin3D{ 0, 0, 0 };
        copyDst.aspect = WGPUTextureAspect_All;

        WGPUTexelCopyBufferLayout dataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        dataLayout.offset = 0;
        dataLayout.bytesPerRow = 4;
        dataLayout.rowsPerImage = 1;

        WGPUExtent3D writeSize = { 1, 1, 1 };
        wgpuQueueWriteTexture(queue_, &copyDst, rgba, 4, &dataLayout, &writeSize);

        WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);
        return { tex, view };
    };

    const uint8_t whitePixels[4] = { 255, 255, 255, 255 };
    auto [whiteTex, whiteView] = create1x1Texture(whitePixels, "DefaultWhiteTexture");
    defaultWhiteTexture_ = whiteTex;
    defaultWhiteTextureView_ = whiteView;

    const uint8_t normalPixels[4] = { 128, 128, 255, 255 };
    auto [normTex, normView] = create1x1Texture(normalPixels, "DefaultNormalTexture");
    defaultNormalTexture_ = normTex;
    defaultNormalTextureView_ = normView;

    const uint8_t blackPixels[4] = { 0, 0, 0, 255 };
    auto [blackTex, blackView] = create1x1Texture(blackPixels, "DefaultBlackTexture");
    defaultBlackTexture_ = blackTex;
    defaultBlackTextureView_ = blackView;

    // 12. Create Samplers: Trilinear 16x Anisotropic + Base-Mip-Only Sampler
    WGPUSamplerDescriptor sampDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sampDesc.addressModeU = WGPUAddressMode_Repeat;
    sampDesc.addressModeV = WGPUAddressMode_Repeat;
    sampDesc.addressModeW = WGPUAddressMode_Repeat;
    sampDesc.magFilter = WGPUFilterMode_Linear;
    sampDesc.minFilter = WGPUFilterMode_Linear;
    sampDesc.mipmapFilter = WGPUMipmapFilterMode_Linear;
    sampDesc.lodMinClamp = 0.0f;
    sampDesc.lodMaxClamp = 32.0f;
    sampDesc.maxAnisotropy = 16;
    defaultSampler_ = wgpuDeviceCreateSampler(device_, &sampDesc);

    WGPUSamplerDescriptor noMipDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    noMipDesc.addressModeU = WGPUAddressMode_Repeat;
    noMipDesc.addressModeV = WGPUAddressMode_Repeat;
    noMipDesc.addressModeW = WGPUAddressMode_Repeat;
    noMipDesc.magFilter = WGPUFilterMode_Linear;
    noMipDesc.minFilter = WGPUFilterMode_Linear;
    noMipDesc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    noMipDesc.lodMinClamp = 0.0f;
    noMipDesc.lodMaxClamp = 0.0f;
    noMipDesc.maxAnisotropy = 1;
    noMipSampler_ = wgpuDeviceCreateSampler(device_, &noMipDesc);

    initialized_ = (opaquePipeline_ != nullptr);
    BGL_LOG_INFO("Hardware WebGPU RenderPipeline initialized (PBR + Bump + Mipmapping + 2048x2048 PCF Shadows + Cubemap Reflections)!");
    return {};
}

void PipelineManager::SetMipmappingEnabled(bool enabled, scene::Scene* activeScene) {
    if (mipmappingEnabled_ == enabled) return;
    mipmappingEnabled_ = enabled;
    if (activeScene) {
        RebuildMaterialBindGroups(*activeScene);
    }
}

void PipelineManager::RebuildMaterialBindGroups(scene::Scene& scene) {
    if (!device_ || !bindGroupLayout_) return;

    auto getTexView = [&](int texIdx, WGPUTextureView fallback) -> WGPUTextureView {
        if (texIdx >= 0 && texIdx < static_cast<int>(scene.gpuTextures.size())) {
            if (scene.gpuTextures[texIdx].view) {
                return scene.gpuTextures[texIdx].view;
            }
        }
        return fallback;
    };

    WGPUSampler activeSampler = mipmappingEnabled_ ? defaultSampler_ : noMipSampler_;

    auto rebuildNode = [&](auto& self, scene::SceneNode& node) -> void {
        for (auto& prim : node.primitives) {
            if (!prim.gpuUniformBuffer) continue;
            if (prim.gpuBindGroup) {
                wgpuBindGroupRelease(prim.gpuBindGroup);
                prim.gpuBindGroup = nullptr;
            }

            int baseColorTexIdx = -1;
            int mrTexIdx = -1;
            int normalTexIdx = -1;
            int emissiveTexIdx = -1;
            int occlusionTexIdx = -1;
            int clearcoatTexIdx = -1;
            int transmissionTexIdx = -1;
            int sheenTexIdx = -1;

            if (prim.materialIndex >= 0 && prim.materialIndex < static_cast<int>(scene.materials.size())) {
                const auto& mat = scene.materials[prim.materialIndex];
                baseColorTexIdx = mat.baseColorTextureIndex;
                mrTexIdx = mat.metallicRoughnessTextureIndex;
                normalTexIdx = mat.normalTextureIndex;
                emissiveTexIdx = mat.emissiveTextureIndex;
                occlusionTexIdx = mat.occlusionTextureIndex;
                clearcoatTexIdx = mat.clearcoatTextureIndex;
                transmissionTexIdx = mat.transmissionTextureIndex;
                sheenTexIdx = mat.sheenColorTextureIndex;
            }

            WGPUBindGroupEntry bgEntries[13] = {};
            bgEntries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
            bgEntries[0].binding = 0;
            bgEntries[0].buffer = prim.gpuUniformBuffer;
            bgEntries[0].offset = 0;
            bgEntries[0].size = sizeof(PrimitiveUniforms);

            bgEntries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
            bgEntries[1].binding = 1;
            bgEntries[1].sampler = activeSampler;

            bgEntries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
            bgEntries[2].binding = 2;
            bgEntries[2].textureView = getTexView(baseColorTexIdx, defaultWhiteTextureView_);

            bgEntries[3] = WGPU_BIND_GROUP_ENTRY_INIT;
            bgEntries[3].binding = 3;
            bgEntries[3].textureView = getTexView(mrTexIdx, defaultWhiteTextureView_);

            bgEntries[4] = WGPU_BIND_GROUP_ENTRY_INIT;
            bgEntries[4].binding = 4;
            bgEntries[4].textureView = getTexView(normalTexIdx, defaultNormalTextureView_);

            bgEntries[5] = WGPU_BIND_GROUP_ENTRY_INIT;
            bgEntries[5].binding = 5;
            bgEntries[5].textureView = getTexView(emissiveTexIdx, defaultBlackTextureView_);

            bgEntries[6] = WGPU_BIND_GROUP_ENTRY_INIT;
            bgEntries[6].binding = 6;
            bgEntries[6].textureView = getTexView(occlusionTexIdx, defaultWhiteTextureView_);

            bgEntries[7] = WGPU_BIND_GROUP_ENTRY_INIT;
            bgEntries[7].binding = 7;
            bgEntries[7].textureView = getTexView(clearcoatTexIdx, defaultWhiteTextureView_);

            bgEntries[8] = WGPU_BIND_GROUP_ENTRY_INIT;
            bgEntries[8].binding = 8;
            bgEntries[8].textureView = getTexView(transmissionTexIdx, defaultWhiteTextureView_);

            bgEntries[9] = WGPU_BIND_GROUP_ENTRY_INIT;
            bgEntries[9].binding = 9;
            bgEntries[9].textureView = getTexView(sheenTexIdx, defaultWhiteTextureView_);

            bgEntries[10] = WGPU_BIND_GROUP_ENTRY_INIT;
            bgEntries[10].binding = 10;
            bgEntries[10].textureView = envCubemapView_;

            bgEntries[11] = WGPU_BIND_GROUP_ENTRY_INIT;
            bgEntries[11].binding = 11;
            bgEntries[11].textureView = shadowDepthView_;

            bgEntries[12] = WGPU_BIND_GROUP_ENTRY_INIT;
            bgEntries[12].binding = 12;
            bgEntries[12].sampler = shadowSampler_;

            WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            bgDesc.layout = bindGroupLayout_;
            bgDesc.entryCount = 13;
            bgDesc.entries = bgEntries;
            prim.gpuBindGroup = wgpuDeviceCreateBindGroup(device_, &bgDesc);
        }

        for (auto& child : node.children) {
            if (child) self(self, *child);
        }
    };

    for (auto& rootNode : scene.rootNodes) {
        if (rootNode) rebuildNode(rebuildNode, *rootNode);
    }
}

void PipelineManager::PrepareScene(scene::Scene& scene) {
    if (!device_ || !queue_ || !bindGroupLayout_) return;

    // Identify which textures are used as normal maps for vector-renormalized mipmap filtering
    std::vector<bool> isNormalTexture(scene.cpuImages.size(), false);
    for (const auto& mat : scene.materials) {
        if (mat.normalTextureIndex >= 0 && static_cast<size_t>(mat.normalTextureIndex) < isNormalTexture.size()) {
            isNormalTexture[mat.normalTextureIndex] = true;
        }
    }

    // 1. Upload CPU decoded images to WebGPU Textures with full mipmap pyramid
    for (size_t i = 0; i < scene.cpuImages.size(); ++i) {
        const auto& img = scene.cpuImages[i];
        if (img.width == 0 || img.height == 0 || img.rgba.empty()) continue;

        if (i < scene.gpuTextures.size() && scene.gpuTextures[i].view == nullptr) {
            uint32_t mipLevels = CalculateMipLevels(img.width, img.height);

            WGPUTextureDescriptor texDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
            texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
            texDesc.dimension = WGPUTextureDimension_2D;
            texDesc.size = WGPUExtent3D{ img.width, img.height, 1 };
            texDesc.format = WGPUTextureFormat_RGBA8Unorm;
            texDesc.mipLevelCount = mipLevels;
            texDesc.sampleCount = 1;
            texDesc.label = WGPUStringView{ "glTF_Texture", strlen("glTF_Texture") };
            WGPUTexture tex = wgpuDeviceCreateTexture(device_, &texDesc);

            // Mip 0 upload
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

            // Generate downsampled mips and upload to GPU
            std::vector<uint8_t> prevMip = img.rgba;
            uint32_t prevW = img.width;
            uint32_t prevH = img.height;

            for (uint32_t level = 1; level < mipLevels; ++level) {
                uint32_t curW = std::max(1u, prevW / 2);
                uint32_t curH = std::max(1u, prevH / 2);

                std::vector<uint8_t> curMip = GenerateNextMipLevel(prevMip.data(), prevW, prevH, curW, curH, isNormalTexture[i]);

                WGPUTexelCopyTextureInfo mipCopyDst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
                mipCopyDst.texture = tex;
                mipCopyDst.mipLevel = level;
                mipCopyDst.origin = WGPUOrigin3D{ 0, 0, 0 };
                mipCopyDst.aspect = WGPUTextureAspect_All;

                WGPUTexelCopyBufferLayout mipDataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
                mipDataLayout.offset = 0;
                mipDataLayout.bytesPerRow = curW * 4;
                mipDataLayout.rowsPerImage = curH;

                WGPUExtent3D mipWriteSize = { curW, curH, 1 };
                wgpuQueueWriteTexture(queue_, &mipCopyDst, curMip.data(), curMip.size(), &mipDataLayout, &mipWriteSize);

                prevMip = std::move(curMip);
                prevW = curW;
                prevH = curH;
            }

            WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);

            scene.gpuTextures[i].texture = tex;
            scene.gpuTextures[i].view = view;
            scene.gpuTextures[i].sampler = defaultSampler_;
            scene.gpuTextures[i].width = img.width;
            scene.gpuTextures[i].height = img.height;
            BGL_LOG_INFO("[WebGPU Upload] Texture #{}: Created 2D Texture ({}x{}, RGBA8Unorm, {} Mip Levels, NormalMap={}), wrote {} bytes to GPU",
                         i, img.width, img.height, mipLevels, isNormalTexture[i] ? "Yes" : "No", img.rgba.size());
        }
    }

    // 2. Prepare vertex buffers, index buffers, uniform buffers, and shadow/main bind groups for each primitive
    auto prepareNode = [&](auto& self, scene::SceneNode& node) -> void {
        for (size_t pIdx = 0; pIdx < node.primitives.size(); ++pIdx) {
            auto& prim = node.primitives[pIdx];
            if (prim.vertices.empty()) continue;

            size_t vbSize = prim.vertices.size() * sizeof(core::Vertex);
            size_t ibSize = prim.indices.size() * sizeof(uint32_t);

            if (!prim.gpuVertexBuffer) {
                WGPUBufferDescriptor vbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
                vbDesc.size = (vbSize + 3) & ~3;
                vbDesc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
                vbDesc.label = WGPUStringView{ "VertexBuffer", strlen("VertexBuffer") };
                prim.gpuVertexBuffer = wgpuDeviceCreateBuffer(device_, &vbDesc);
                wgpuQueueWriteBuffer(queue_, prim.gpuVertexBuffer, 0, prim.vertices.data(), vbSize);
            }

            if (!prim.gpuIndexBuffer && !prim.indices.empty()) {
                WGPUBufferDescriptor ibDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
                ibDesc.size = (ibSize + 3) & ~3;
                ibDesc.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
                ibDesc.label = WGPUStringView{ "IndexBuffer", strlen("IndexBuffer") };
                prim.gpuIndexBuffer = wgpuDeviceCreateBuffer(device_, &ibDesc);
                wgpuQueueWriteBuffer(queue_, prim.gpuIndexBuffer, 0, prim.indices.data(), ibSize);
            }

            if (!prim.gpuUniformBuffer) {
                WGPUBufferDescriptor ubDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
                ubDesc.size = sizeof(PrimitiveUniforms);
                ubDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
                ubDesc.label = WGPUStringView{ "PrimitiveUniforms", strlen("PrimitiveUniforms") };
                prim.gpuUniformBuffer = wgpuDeviceCreateBuffer(device_, &ubDesc);
            }

            if (!prim.gpuShadowBindGroup && shadowBindGroupLayout_) {
                WGPUBindGroupEntry shadowBgEntry = WGPU_BIND_GROUP_ENTRY_INIT;
                shadowBgEntry.binding = 0;
                shadowBgEntry.buffer = prim.gpuUniformBuffer;
                shadowBgEntry.offset = 0;
                shadowBgEntry.size = sizeof(PrimitiveUniforms);

                WGPUBindGroupDescriptor shadowBgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
                shadowBgDesc.layout = shadowBindGroupLayout_;
                shadowBgDesc.entryCount = 1;
                shadowBgDesc.entries = &shadowBgEntry;
                prim.gpuShadowBindGroup = wgpuDeviceCreateBindGroup(device_, &shadowBgDesc);
            }

            BGL_LOG_INFO("[WebGPU Upload] Node '{}' Prim #{}: VB={} bytes ({} verts), IB={} bytes ({} indices), UB={} bytes",
                         node.name, pIdx, vbSize, prim.vertices.size(), ibSize, prim.indices.size(), sizeof(PrimitiveUniforms));
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

    RebuildMaterialBindGroups(scene);
}

void PipelineManager::RenderShadowPass(WGPUCommandEncoder encoder, scene::Scene& scene, const glm::vec3& lightDir, const scene::Quadtree* spatialIndex) {
    (void)spatialIndex;
    currentLightDir_ = glm::length(lightDir) > 1e-5f ? glm::normalize(lightDir) : glm::normalize(glm::vec3(0.35f, 1.0f, 0.25f));
    currentLightViewProj_ = ComputeDirectionalLightMatrix(scene.sceneAABB, currentLightDir_);

    if (!shadowsEnabled_ || !encoder || !shadowPipeline_ || !shadowDepthView_) {
        return;
    }

    WGPURenderPassDepthStencilAttachment depthAttachment = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    depthAttachment.view = shadowDepthView_;
    depthAttachment.depthLoadOp = WGPULoadOp_Clear;
    depthAttachment.depthStoreOp = WGPUStoreOp_Store;
    depthAttachment.depthClearValue = 1.0f;
    depthAttachment.depthReadOnly = WGPU_FALSE;

    WGPURenderPassDescriptor passDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    passDesc.colorAttachmentCount = 0;
    passDesc.colorAttachments = nullptr;
    passDesc.depthStencilAttachment = &depthAttachment;
    passDesc.label = WGPUStringView{ "DirectionalShadowPass", strlen("DirectionalShadowPass") };

    WGPURenderPassEncoder shadowPass = wgpuCommandEncoderBeginRenderPass(encoder, &passDesc);
    if (!shadowPass) return;

    wgpuRenderPassEncoderSetPipeline(shadowPass, shadowPipeline_);

    for (const auto& rootNode : scene.rootNodes) {
        if (rootNode) {
            RenderShadowNode(shadowPass, scene, *rootNode, glm::mat4(1.0f));
        }
    }

    wgpuRenderPassEncoderEnd(shadowPass);
    wgpuRenderPassEncoderRelease(shadowPass);
}

void PipelineManager::RenderShadowNode(WGPURenderPassEncoder pass, scene::Scene& scene, scene::SceneNode& node, const glm::mat4& parentTransform) {
    glm::mat4 worldMat = parentTransform * node.localTransform;

    for (auto& prim : node.primitives) {
        if (!prim.gpuVertexBuffer || !prim.gpuShadowBindGroup || !prim.gpuUniformBuffer) continue;

        wgpuRenderPassEncoderSetBindGroup(pass, 0, prim.gpuShadowBindGroup, 0, nullptr);
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
            RenderShadowNode(pass, scene, *child, worldMat);
        }
    }
}

void PipelineManager::RenderScene(WGPURenderPassEncoder pass, scene::Scene& scene, const glm::mat4& viewProj, const glm::vec3& cameraPos, bool wireframe, const scene::Quadtree* spatialIndex) {
    if (!pass || !initialized_) return;

    WGPURenderPipeline activePipeline = wireframe && wireframePipeline_ ? wireframePipeline_ : opaquePipeline_;
    wgpuRenderPassEncoderSetPipeline(pass, activePipeline);

    core::Frustum frustum = core::Frustum::FromViewProjection(viewProj);
    lastVisiblePrimitiveCount_ = 0;

    if (spatialIndex) {
        auto visibleItems = spatialIndex->QueryVisible(frustum, cameraPos);
        lastVisiblePrimitiveCount_ = visibleItems.size();
        for (const auto& item : visibleItems) {
            if (!item.node || item.primitiveIndex >= item.node->primitives.size()) continue;
            auto& prim = item.node->primitives[item.primitiveIndex];
            RenderPrimitive(pass, scene, prim, item.node->worldTransform, viewProj, cameraPos, wireframe);
        }
    } else {
        for (const auto& rootNode : scene.rootNodes) {
            if (rootNode) {
                RenderNode(pass, scene, *rootNode, viewProj, glm::mat4(1.0f), cameraPos, wireframe, frustum);
            }
        }
    }
}

void PipelineManager::RenderNode(WGPURenderPassEncoder pass, scene::Scene& scene, scene::SceneNode& node, const glm::mat4& viewProj, const glm::mat4& parentTransform, const glm::vec3& cameraPos, bool wireframe, const core::Frustum& frustum) {
    glm::mat4 worldMat = parentTransform * node.localTransform;

    if (node.worldAABB.minBound.x <= node.worldAABB.maxBound.x && !frustum.IntersectsAABB(node.worldAABB)) {
        return; // Hierarchical frustum culling
    }

    lastVisiblePrimitiveCount_ += node.primitives.size();
    for (auto& prim : node.primitives) {
        RenderPrimitive(pass, scene, prim, worldMat, viewProj, cameraPos, wireframe);
    }

    for (const auto& child : node.children) {
        if (child) {
            RenderNode(pass, scene, *child, viewProj, worldMat, cameraPos, wireframe, frustum);
        }
    }
}

void PipelineManager::RenderPrimitive(WGPURenderPassEncoder pass, scene::Scene& scene, scene::MeshPrimitive& prim, const glm::mat4& worldMat, const glm::mat4& viewProj, const glm::vec3& cameraPos, bool wireframe) {
    (void)wireframe;
    if (!prim.gpuVertexBuffer || !prim.gpuBindGroup || !prim.gpuUniformBuffer) return;

    PrimitiveUniforms uniforms;
    uniforms.viewProj = viewProj;
    uniforms.model = worldMat;
    uniforms.lightViewProj = currentLightViewProj_;
    uniforms.cameraPos = glm::vec4(cameraPos, 1.0f);
    uniforms.lightDir = glm::vec4(currentLightDir_, 0.0f);
    uniforms.envMipCount = static_cast<float>(envMipLevels_);

    uint32_t flags = 0;
    if (bumpMappingEnabled_) flags |= MaterialFlags::EnableBumpMapping;
    if (mipmappingEnabled_)  flags |= MaterialFlags::EnableMipmapping;
    if (shadowsEnabled_)     flags |= MaterialFlags::EnableShadows;
    if (reflectionsEnabled_) flags |= MaterialFlags::EnableReflections;

    if (prim.materialIndex >= 0 && prim.materialIndex < static_cast<int>(scene.materials.size())) {
        const auto& mat = scene.materials[prim.materialIndex];
        uniforms.baseColorFactor = mat.baseColorFactor;
        uniforms.metallicFactor = mat.metallicFactor;
        uniforms.roughnessFactor = mat.roughnessFactor;
        uniforms.normalScale = mat.normalScale;
        uniforms.bumpScale = mat.bumpScale;
        uniforms.occlusionStrength = mat.occlusionStrength;
        uniforms.emissiveFactor = glm::vec4(mat.emissiveFactor, mat.emissiveStrength);
        uniforms.alphaCutoff = mat.alphaCutoff;
        uniforms.alphaMode = static_cast<uint32_t>(mat.alphaMode);
        uniforms.ior = mat.ior;
        uniforms.clearcoatFactor = mat.clearcoatFactor;
        uniforms.clearcoatRoughness = mat.clearcoatRoughnessFactor;
        uniforms.transmissionFactor = mat.transmissionFactor;
        uniforms.specularFactor = mat.specularFactor;
        uniforms.sheenColorFactor = glm::vec4(mat.sheenColorFactor, mat.sheenRoughnessFactor);
        uniforms.specularColorFactor = glm::vec4(mat.specularColorFactor, 0.0f);

        if (mat.baseColorTextureIndex >= 0) flags |= MaterialFlags::HasBaseColorTex;
        if (mat.metallicRoughnessTextureIndex >= 0) flags |= MaterialFlags::HasMetallicRoughnessTex;
        if (mat.normalTextureIndex >= 0) flags |= MaterialFlags::HasNormalTex;
        if (mat.emissiveTextureIndex >= 0) flags |= MaterialFlags::HasEmissiveTex;
        if (mat.occlusionTextureIndex >= 0) flags |= MaterialFlags::HasOcclusionTex;
        if (mat.clearcoatTextureIndex >= 0) flags |= MaterialFlags::HasClearcoatTex;
        if (mat.transmissionTextureIndex >= 0) flags |= MaterialFlags::HasTransmissionTex;
        if (mat.sheenColorTextureIndex >= 0) flags |= MaterialFlags::HasSheenTex;
        if (mat.unlit) flags |= MaterialFlags::Unlit;
        if (mat.doubleSided) flags |= MaterialFlags::DoubleSided;
    }
    uniforms.flags = flags;

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

void PipelineManager::AppendAABBLines(const core::AABB& aabb, const glm::vec4& color) {
    if (aabb.minBound.x > aabb.maxBound.x) return;
    const glm::vec3 minB = aabb.minBound;
    const glm::vec3 maxB = aabb.maxBound;

    const glm::vec3 c[8] = {
        glm::vec3(minB.x, minB.y, minB.z),
        glm::vec3(maxB.x, minB.y, minB.z),
        glm::vec3(maxB.x, maxB.y, minB.z),
        glm::vec3(minB.x, maxB.y, minB.z),
        glm::vec3(minB.x, minB.y, maxB.z),
        glm::vec3(maxB.x, minB.y, maxB.z),
        glm::vec3(maxB.x, maxB.y, maxB.z),
        glm::vec3(minB.x, maxB.y, maxB.z)
    };

    static constexpr size_t kIndices[24] = {
        0, 1,  1, 2,  2, 3,  3, 0,
        4, 5,  5, 6,  6, 7,  7, 4,
        0, 4,  1, 5,  2, 6,  3, 7
    };

    for (size_t idx : kIndices) {
        batchedAabbVertices_.push_back({ c[idx], 0.0f, color });
    }
}

void PipelineManager::FlushAABBLines(WGPURenderPassEncoder pass, const glm::mat4& viewProj) {
    if (!pass || !aabbPipeline_ || batchedAabbVertices_.empty()) {
        batchedAabbVertices_.clear();
        return;
    }

    const size_t requiredBytes = batchedAabbVertices_.size() * sizeof(AabbVertex);
    if (requiredBytes > aabbVertexBufferSize_ || !aabbVertexBuffer_) {
        if (aabbVertexBuffer_) {
            wgpuBufferRelease(aabbVertexBuffer_);
            aabbVertexBuffer_ = nullptr;
        }
        aabbVertexBufferSize_ = std::max(requiredBytes * 2, size_t(512 * 1024));
        WGPUBufferDescriptor vbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
        vbDesc.size = aabbVertexBufferSize_;
        vbDesc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        vbDesc.label = WGPUStringView{ "AABB_DynamicVertexBuffer", strlen("AABB_DynamicVertexBuffer") };
        aabbVertexBuffer_ = wgpuDeviceCreateBuffer(device_, &vbDesc);
    }

    wgpuQueueWriteBuffer(queue_, aabbVertexBuffer_, 0, batchedAabbVertices_.data(), requiredBytes);

    AabbUniforms uniforms;
    uniforms.viewProj = viewProj;
    wgpuQueueWriteBuffer(queue_, aabbUniformBuffer_, 0, &uniforms, sizeof(uniforms));

    wgpuRenderPassEncoderSetPipeline(pass, aabbPipeline_);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, aabbBindGroup_, 0, nullptr);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, aabbVertexBuffer_, 0, requiredBytes);
    wgpuRenderPassEncoderDraw(pass, static_cast<uint32_t>(batchedAabbVertices_.size()), 1, 0, 0);

    batchedAabbVertices_.clear();
}

void PipelineManager::RenderAABB(WGPURenderPassEncoder pass, const core::AABB& aabb, const glm::mat4& viewProj, const glm::vec4& color) {
    AppendAABBLines(aabb, color);
    FlushAABBLines(pass, viewProj);
}

void PipelineManager::RenderSceneAABBs(WGPURenderPassEncoder pass, const scene::Scene& scene, const glm::mat4& viewProj, bool showSceneAABB, bool showPrimitiveAABBs) {
    if (!pass || !aabbPipeline_) return;

    if (showSceneAABB && scene.sceneAABB.minBound.x <= scene.sceneAABB.maxBound.x) {
        AppendAABBLines(scene.sceneAABB, glm::vec4(1.0f, 0.84f, 0.0f, 1.0f));
    }

    if (showPrimitiveAABBs) {
        auto renderNodeAABBs = [&](auto& self, const scene::SceneNode& node, const glm::mat4& parentTransform) -> void {
            glm::mat4 worldMat = parentTransform * node.localTransform;
            for (const auto& prim : node.primitives) {
                if (prim.boundingBox.minBound.x <= prim.boundingBox.maxBound.x) {
                    core::AABB primWorldAABB;
                    glm::vec3 c = prim.boundingBox.Center();
                    glm::vec3 e = prim.boundingBox.Extents();
                    glm::vec3 corners[8] = {
                        c + glm::vec3(-e.x, -e.y, -e.z),
                        c + glm::vec3( e.x, -e.y, -e.z),
                        c + glm::vec3(-e.x,  e.y, -e.z),
                        c + glm::vec3( e.x,  e.y, -e.z),
                        c + glm::vec3(-e.x, -e.y,  e.z),
                        c + glm::vec3( e.x, -e.y,  e.z),
                        c + glm::vec3(-e.x,  e.y,  e.z),
                        c + glm::vec3( e.x,  e.y,  e.z)
                    };
                    for (const auto& corner : corners) {
                        glm::vec4 wc = worldMat * glm::vec4(corner, 1.0f);
                        primWorldAABB.Expand(glm::vec3(wc));
                    }
                    AppendAABBLines(primWorldAABB, glm::vec4(0.2f, 0.85f, 1.0f, 0.8f));
                }
            }
            for (const auto& child : node.children) {
                if (child) self(self, *child, worldMat);
            }
        };

        for (const auto& rootNode : scene.rootNodes) {
            if (rootNode) renderNodeAABBs(renderNodeAABBs, *rootNode, glm::mat4(1.0f));
        }
    }

    FlushAABBLines(pass, viewProj);
}

void PipelineManager::RenderQuadtreeBounds(WGPURenderPassEncoder pass, const scene::Quadtree& quadtree, const glm::mat4& viewProj) {
    if (!pass || !aabbPipeline_) return;

    std::vector<core::AABB> bounds;
    quadtree.GetNodeBounds(bounds);
    for (const auto& b : bounds) {
        AppendAABBLines(b, glm::vec4(0.25f, 0.95f, 0.45f, 0.7f));
    }

    FlushAABBLines(pass, viewProj);
}

} // namespace bgl::gpu
