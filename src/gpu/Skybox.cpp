/**
 * @file Skybox.cpp
 * @brief Implementation of HDR Environment Cubemap & Procedural Atmospheric Skybox with WebGPU 4x MSAA pipeline.
 */
#include "gpu/Skybox.hpp"
#include "core/Log.hpp"
#include <array>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <utility>

namespace bgl::gpu {

namespace {

constexpr std::string_view kSkyboxWGSL = R"(
struct SkyboxUniforms {
    invViewProj : mat4x4<f32>,
    sunDir : vec4<f32>,
    cameraPos : vec4<f32>,
    params : vec4<f32>, // x = envRotation, y = blurLod, z = useEnvCubemap, w = mode
};

@group(0) @binding(0) var<uniform> ubo : SkyboxUniforms;
@group(0) @binding(1) var envSampler : sampler;
@group(0) @binding(2) var envCubemap : texture_cube<f32>;

struct VertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) viewRay : vec3<f32>,
};

fn rotateY(v : vec3<f32>, angle : f32) -> vec3<f32> {
    let c = cos(angle);
    let s = sin(angle);
    return vec3<f32>(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

@vertex
fn vs_sky(@builtin(vertex_index) vIdx : u32) -> VertexOutput {
    var out : VertexOutput;
    // Generate fullscreen triangle covering NDC [-1, 1]
    let x = f32(i32(vIdx << 1u) & 2) * 2.0 - 1.0;
    let y = f32(i32(vIdx & 2u)) * 2.0 - 1.0;
    // Z = 1.0 places the triangle on the far plane (WebGPU 0..1 depth)
    out.clipPos = vec4<f32>(x, y, 1.0, 1.0);

    let unprojected = ubo.invViewProj * vec4<f32>(x, y, 1.0, 1.0);
    out.viewRay = unprojected.xyz / unprojected.w - ubo.cameraPos.xyz;
    return out;
}

@fragment
fn fs_sky(in : VertexOutput) -> @location(0) vec4<f32> {
    let dir = rotateY(normalize(in.viewRay), ubo.params.x);
    let lod = ubo.params.y;

    if (ubo.params.z > 0.5) {
        let envColor = textureSampleLevel(envCubemap, envSampler, dir, lod).rgb;
        return vec4<f32>(envColor, 1.0);
    }

    let sun = rotateY(normalize(ubo.sunDir.xyz), ubo.params.x);

    // Physically plausible HDR atmospheric sky gradient fallback
    let zenithColor = vec3<f32>(0.14, 0.36, 0.74);
    let horizonColor = vec3<f32>(0.66, 0.80, 0.94);
    let groundColor = vec3<f32>(0.24, 0.20, 0.18);

    let elevation = dir.y;
    var sky = vec3<f32>(0.0);
    if (elevation >= 0.0) {
        let t = pow(clamp(1.0 - elevation, 0.0, 1.0), 2.2);
        sky = mix(zenithColor, horizonColor, t);
    } else {
        let t = clamp(-elevation * 4.0, 0.0, 1.0);
        sky = mix(horizonColor, groundColor, t);
    }

    let sunDot = max(dot(dir, sun), 0.0);
    let sunDisk = pow(sunDot, 1500.0) * 10.0;
    let sunCorona = pow(sunDot, 24.0) * 0.55;
    let sunColor = vec3<f32>(1.0, 0.96, 0.88);
    let sunGlow = sunColor * (sunDisk + sunCorona);

    return vec4<f32>(sky + sunGlow, 1.0);
}
)";

} // anonymous namespace

Skybox::~Skybox() {
    Shutdown();
}

void Skybox::Shutdown() noexcept {
    bindGroup_.Reset();
    fallbackSampler_.Reset();
    fallbackCubeView_.Reset();
    fallbackCubeTexture_.Reset();
    uniformBuffer_.Reset();
    pipeline_.Reset();
    bindGroupLayout_.Reset();
    device_ = nullptr;
    queue_ = nullptr;
    initialized_ = false;
    hasCustomCubemap_ = false;
}

core::Result<void> Skybox::Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat hdrFormat,
                                      uint32_t sampleCount) {
    device_ = device;
    queue_ = queue;
    if (!device_ || !queue_) {
        return std::unexpected("Cannot initialize Skybox without valid WGPUDevice and WGPUQueue");
    }

    // 1. Compile WGSL Shader
    WGPUShaderSourceWGSL wgslDesc = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgslDesc.code = ToWgpuStringView(kSkyboxWGSL);

    WGPUShaderModuleDescriptor smDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    smDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&wgslDesc);
    smDesc.label = ToWgpuStringView("Skybox_WGSL");
    ShaderModuleHandle shaderModule{wgpuDeviceCreateShaderModule(device_, &smDesc)};
    if (!shaderModule) {
        return std::unexpected("Failed to compile WGSL HDR skybox shader");
    }

    // 2. Bind Group Layout (0: SkyboxUniforms, 1: Sampler, 2: Environment Cubemap)
    std::array<WGPUBindGroupLayoutEntry, 3> bglEntries{};
    bglEntries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[0].binding = 0;
    bglEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    bglEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
    bglEntries[0].buffer.minBindingSize = sizeof(SkyboxUniforms);

    bglEntries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[1].binding = 1;
    bglEntries[1].visibility = WGPUShaderStage_Fragment;
    bglEntries[1].sampler.type = WGPUSamplerBindingType_Filtering;

    bglEntries[2] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[2].binding = 2;
    bglEntries[2].visibility = WGPUShaderStage_Fragment;
    bglEntries[2].texture.sampleType = WGPUTextureSampleType_Float;
    bglEntries[2].texture.viewDimension = WGPUTextureViewDimension_Cube;
    bglEntries[2].texture.multisampled = WGPU_FALSE;

    WGPUBindGroupLayoutDescriptor bglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bglDesc.entryCount = bglEntries.size();
    bglDesc.entries = bglEntries.data();
    bindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(device_, &bglDesc);

    // 3. Pipeline Layout
    WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = bindGroupLayout_.AddressOf();
    PipelineLayoutHandle pipelineLayout{wgpuDeviceCreatePipelineLayout(device_, &plDesc)};

    // 4. Color Target & Depth State (renders into RGBA16Float 4x MSAA HDR target)
    WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
    colorTarget.format = hdrFormat;
    colorTarget.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragmentState = WGPU_FRAGMENT_STATE_INIT;
    fragmentState.module = shaderModule;
    fragmentState.entryPoint = ToWgpuStringView("fs_sky");
    fragmentState.targetCount = 1;
    fragmentState.targets = &colorTarget;

    WGPUDepthStencilState depthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    depthStencil.format = WGPUTextureFormat_Depth24Plus;
    depthStencil.depthWriteEnabled = WGPUOptionalBool_False;
    depthStencil.depthCompare = WGPUCompareFunction_LessEqual;

    // 5. Create Render Pipeline
    WGPURenderPipelineDescriptor rpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rpDesc.layout = pipelineLayout;
    rpDesc.vertex.module = shaderModule;
    rpDesc.vertex.entryPoint = ToWgpuStringView("vs_sky");
    rpDesc.vertex.bufferCount = 0;
    rpDesc.vertex.buffers = nullptr;
    rpDesc.fragment = &fragmentState;
    rpDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rpDesc.primitive.cullMode = WGPUCullMode_None;
    rpDesc.depthStencil = &depthStencil;
    rpDesc.multisample.count = sampleCount;
    rpDesc.multisample.mask = 0xFFFFFFFF;
    rpDesc.multisample.alphaToCoverageEnabled = WGPU_FALSE;

    pipeline_ = wgpuDeviceCreateRenderPipeline(device_, &rpDesc);

    // 6. Uniform Buffer
    WGPUBufferDescriptor ubDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    ubDesc.size = sizeof(SkyboxUniforms);
    ubDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    ubDesc.label = ToWgpuStringView("SkyboxUniformBuffer");
    uniformBuffer_ = wgpuDeviceCreateBuffer(device_, &ubDesc);

    // 7. Create 1x1x6 fallback cubemap and trilinear sampler
    WGPUTextureDescriptor cubeDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    cubeDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    cubeDesc.dimension = WGPUTextureDimension_2D;
    cubeDesc.size = WGPUExtent3D{1, 1, 6};
    cubeDesc.format = WGPUTextureFormat_RGBA8Unorm;
    cubeDesc.mipLevelCount = 1;
    cubeDesc.sampleCount = 1;
    cubeDesc.label = ToWgpuStringView("SkyboxFallbackCubemap");
    fallbackCubeTexture_ = wgpuDeviceCreateTexture(device_, &cubeDesc);

    WGPUTextureViewDescriptor cubeViewDesc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    cubeViewDesc.format = WGPUTextureFormat_RGBA8Unorm;
    cubeViewDesc.dimension = WGPUTextureViewDimension_Cube;
    cubeViewDesc.baseMipLevel = 0;
    cubeViewDesc.mipLevelCount = 1;
    cubeViewDesc.baseArrayLayer = 0;
    cubeViewDesc.arrayLayerCount = 6;
    cubeViewDesc.aspect = WGPUTextureAspect_All;
    fallbackCubeView_ = wgpuTextureCreateView(fallbackCubeTexture_, &cubeViewDesc);

    WGPUSamplerDescriptor sampDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sampDesc.addressModeU = WGPUAddressMode_ClampToEdge;
    sampDesc.addressModeV = WGPUAddressMode_ClampToEdge;
    sampDesc.addressModeW = WGPUAddressMode_ClampToEdge;
    sampDesc.magFilter = WGPUFilterMode_Linear;
    sampDesc.minFilter = WGPUFilterMode_Linear;
    sampDesc.mipmapFilter = WGPUMipmapFilterMode_Linear;
    sampDesc.lodMinClamp = 0.0f;
    sampDesc.lodMaxClamp = 16.0f;
    sampDesc.maxAnisotropy = 1;
    fallbackSampler_ = wgpuDeviceCreateSampler(device_, &sampDesc);

    hasCustomCubemap_ = false;
    RebuildBindGroup(fallbackCubeView_, fallbackSampler_);

    initialized_ = (pipeline_ != nullptr && uniformBuffer_ != nullptr && bindGroup_ != nullptr);
    BGL_LOG_INFO("HDR Skybox pipeline initialized successfully (RGBA16Float, 4x MSAA, Sharp/Blurred/Hidden modes)!");
    return {};
}

void Skybox::RebuildBindGroup(WGPUTextureView cubemapView, WGPUSampler sampler) {
    if (!device_ || !bindGroupLayout_ || !uniformBuffer_)
        return;

    std::array<WGPUBindGroupEntry, 3> entries{};
    entries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
    entries[0].binding = 0;
    entries[0].buffer = uniformBuffer_;
    entries[0].offset = 0;
    entries[0].size = sizeof(SkyboxUniforms);

    entries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
    entries[1].binding = 1;
    entries[1].sampler = sampler ? sampler : fallbackSampler_.Get();

    entries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
    entries[2].binding = 2;
    entries[2].textureView = cubemapView ? cubemapView : fallbackCubeView_.Get();

    WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgDesc.layout = bindGroupLayout_;
    bgDesc.entryCount = entries.size();
    bgDesc.entries = entries.data();
    bindGroup_ = wgpuDeviceCreateBindGroup(device_, &bgDesc);
}

void Skybox::SetEnvironmentCubemap(WGPUTextureView cubemapView, WGPUSampler sampler) {
    hasCustomCubemap_ = (cubemapView != nullptr);
    RebuildBindGroup(cubemapView, sampler);
}

void Skybox::Render(WGPURenderPassEncoder pass, const glm::mat4& viewProj, const glm::vec3& cameraPos,
                    const glm::vec3& sunDir) {
    if (!pass || !initialized_ || !enabled_ || mode_ == SkyboxMode::Hidden)
        return;

    SkyboxUniforms uniforms{};
    uniforms.invViewProj = glm::inverse(viewProj);
    uniforms.sunDir = glm::vec4{glm::normalize(sunDir), 0.0f};
    uniforms.cameraPos = glm::vec4{cameraPos, 1.0f};
    const float blurLod{(mode_ == SkyboxMode::Blurred) ? (blurRoughness_ * 5.0f) : 0.0f};
    uniforms.params =
        glm::vec4{rotation_, blurLod, hasCustomCubemap_ ? 1.0f : 0.0f, static_cast<float>(std::to_underlying(mode_))};

    wgpuQueueWriteBuffer(queue_, uniformBuffer_, 0, &uniforms, sizeof(uniforms));

    wgpuRenderPassEncoderSetPipeline(pass, pipeline_);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bindGroup_, 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
}

} // namespace bgl::gpu
