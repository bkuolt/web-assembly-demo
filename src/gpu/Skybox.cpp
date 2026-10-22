/**
 * @file Skybox.cpp
 * @brief Implementation of Procedural Atmospheric Skybox with WebGPU 4x MSAA render pipeline.
 */
#include "gpu/Skybox.hpp"
#include "core/Log.hpp"
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>

namespace bgl::gpu {

namespace {

constexpr std::string_view kSkyboxWGSL = R"(
struct SkyboxUniforms {
    invViewProj : mat4x4<f32>,
    sunDir : vec4<f32>,
    cameraPos : vec4<f32>,
};

@group(0) @binding(0) var<uniform> ubo : SkyboxUniforms;

struct VertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) viewRay : vec3<f32>,
};

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
    let dir = normalize(in.viewRay);
    let sun = normalize(ubo.sunDir.xyz);

    // Physically plausible atmospheric sky gradient
    let zenithColor = vec3<f32>(0.14, 0.36, 0.74);   // Deep Mediterranean sky blue
    let horizonColor = vec3<f32>(0.66, 0.80, 0.94);  // Warm hazy horizon
    let groundColor = vec3<f32>(0.24, 0.20, 0.18);   // Warm stone/earth ground

    let elevation = dir.y;
    var sky = vec3<f32>(0.0);
    if (elevation >= 0.0) {
        let t = pow(clamp(1.0 - elevation, 0.0, 1.0), 2.2);
        sky = mix(zenithColor, horizonColor, t);
    } else {
        let t = clamp(-elevation * 4.0, 0.0, 1.0);
        sky = mix(horizonColor, groundColor, t);
    }

    // Direct sun disc and soft atmospheric halo corona
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
    uniformBuffer_.Reset();
    pipeline_.Reset();
    bindGroupLayout_.Reset();
    device_ = nullptr;
    queue_ = nullptr;
    initialized_ = false;
}

core::Result<void> Skybox::Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat surfaceFormat, uint32_t sampleCount) {
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
        return std::unexpected("Failed to compile WGSL atmospheric skybox shader");
    }

    // 2. Bind Group Layout (Binding 0: SkyboxUniforms)
    WGPUBindGroupLayoutEntry bglEntry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntry.binding = 0;
    bglEntry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    bglEntry.buffer.type = WGPUBufferBindingType_Uniform;
    bglEntry.buffer.minBindingSize = sizeof(SkyboxUniforms);

    WGPUBindGroupLayoutDescriptor bglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bglDesc.entryCount = 1;
    bglDesc.entries = &bglEntry;
    bindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(device_, &bglDesc);

    // 3. Pipeline Layout
    WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = bindGroupLayout_.AddressOf();
    PipelineLayoutHandle pipelineLayout{wgpuDeviceCreatePipelineLayout(device_, &plDesc)};

    // 4. Color Target & Depth State
    WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
    colorTarget.format = surfaceFormat;
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

    // 7. Bind Group
    WGPUBindGroupEntry bgEntry = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntry.binding = 0;
    bgEntry.buffer = uniformBuffer_;
    bgEntry.offset = 0;
    bgEntry.size = sizeof(SkyboxUniforms);

    WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgDesc.layout = bindGroupLayout_;
    bgDesc.entryCount = 1;
    bgDesc.entries = &bgEntry;
    bindGroup_ = wgpuDeviceCreateBindGroup(device_, &bgDesc);

    initialized_ = (pipeline_ != nullptr && uniformBuffer_ != nullptr && bindGroup_ != nullptr);
    BGL_LOG_INFO("Atmospheric Skybox pipeline initialized successfully (4x MSAA, far-plane depth clamping)!");
    return {};
}

void Skybox::Render(WGPURenderPassEncoder pass, const glm::mat4& viewProj, const glm::vec3& cameraPos, const glm::vec3& sunDir) {
    if (!pass || !initialized_ || !enabled_) return;

    SkyboxUniforms uniforms;
    uniforms.invViewProj = glm::inverse(viewProj);
    uniforms.sunDir = glm::vec4(glm::normalize(sunDir), 0.0f);
    uniforms.cameraPos = glm::vec4(cameraPos, 1.0f);

    wgpuQueueWriteBuffer(queue_, uniformBuffer_, 0, &uniforms, sizeof(uniforms));

    wgpuRenderPassEncoderSetPipeline(pass, pipeline_);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bindGroup_, 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
}

} // namespace bgl::gpu
