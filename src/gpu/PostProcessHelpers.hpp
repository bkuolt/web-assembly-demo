/**
 * @file PostProcessHelpers.hpp
 * @brief Internal shared uniform structs and pass helpers for PostProcessPass translation units.
 */
#ifndef BGL_GPU_POSTPROCESSHELPERS_HPP
#define BGL_GPU_POSTPROCESSHELPERS_HPP

#include "gpu/Handle.hpp"
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <string_view>

namespace bgl::gpu::detail {

inline constexpr uint32_t kPostUniformSlotStride = 256u;
inline constexpr uint32_t kMaxBloomUniformSlots = 12u;

struct alignas(16) DepthLinearizeUniformData {
    glm::mat4 invProj{1.0f};
    glm::vec4 params{0.05f, 500.0f, 1280.0f, 720.0f};
};

struct alignas(16) GtaoUniformData {
    glm::mat4 invProj{1.0f};
    glm::vec4 params{0.02f, 0.75f, 0.005f, 2.0f};
    glm::vec4 texelInfo{1280.0f, 720.0f, 640.0f, 360.0f};
};

struct alignas(16) BloomUniformData {
    glm::vec4 params0{0.0f};
    glm::vec4 params1{0.0f};
};

struct alignas(16) AutoExposureUniformData {
    glm::vec4 params{0.016f, 2.5f, 0.25f, 4.0f};
    glm::vec4 keyParams{0.24f, 0.0f, 1.0f, 0.0f};
};

struct alignas(16) FxaaUniformData {
    glm::vec4 invScreenSize{1.0f / 1280.0f, 1.0f / 720.0f, 0.75f, 0.125f};
    glm::vec4 flags{0.0f, 0.0312f, 1.0f, 0.0f};
};

[[nodiscard]] inline bool IsSrgbSwapchainFormat(WGPUTextureFormat fmt) noexcept {
    return fmt == WGPUTextureFormat_BGRA8UnormSrgb || fmt == WGPUTextureFormat_RGBA8UnormSrgb;
}

[[nodiscard]] inline BindGroupHandle CreateBloomPassBindGroup(WGPUDevice device, WGPUBindGroupLayout bgl,
                                                              WGPUBuffer uniformBuffer, WGPUSampler linearSampler,
                                                              WGPUTextureView sourceView) {
    std::array<WGPUBindGroupEntry, 3> be{};
    be[0] = WGPU_BIND_GROUP_ENTRY_INIT;
    be[0].binding = 0;
    be[0].buffer = uniformBuffer;
    be[0].size = sizeof(BloomUniformData);
    be[1] = WGPU_BIND_GROUP_ENTRY_INIT;
    be[1].binding = 1;
    be[1].sampler = linearSampler;
    be[2] = WGPU_BIND_GROUP_ENTRY_INIT;
    be[2].binding = 2;
    be[2].textureView = sourceView;

    WGPUBindGroupDescriptor bDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bDesc.layout = bgl;
    bDesc.entryCount = be.size();
    bDesc.entries = be.data();
    return BindGroupHandle{wgpuDeviceCreateBindGroup(device, &bDesc)};
}

inline void DrawFullscreenPass(WGPUCommandEncoder encoder, WGPUTextureView targetView, WGPURenderPipeline pipeline,
                               WGPUBindGroup bindGroup, const char* label, bool loadExisting = false,
                               const uint32_t* dynOffset = nullptr) {
    if (!encoder || !targetView || !pipeline || !bindGroup) {
        return;
    }
    WGPURenderPassColorAttachment colorAtt = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    colorAtt.view = targetView;
    colorAtt.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    colorAtt.loadOp = loadExisting ? WGPULoadOp_Load : WGPULoadOp_Clear;
    colorAtt.storeOp = WGPUStoreOp_Store;
    colorAtt.clearValue = WGPUColor{0.0, 0.0, 0.0, 1.0};

    WGPURenderPassDescriptor rpDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    rpDesc.colorAttachmentCount = 1;
    rpDesc.colorAttachments = &colorAtt;
    rpDesc.label = ToWgpuStringView(label);

    if (RenderPassEncoderHandle pass{wgpuCommandEncoderBeginRenderPass(encoder, &rpDesc)}) {
        wgpuRenderPassEncoderSetPipeline(pass.Get(), pipeline);
        wgpuRenderPassEncoderSetBindGroup(pass.Get(), 0, bindGroup, dynOffset ? 1u : 0u, dynOffset);
        wgpuRenderPassEncoderDraw(pass.Get(), 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(pass.Get());
    }
}

} // namespace bgl::gpu::detail

#endif // BGL_GPU_POSTPROCESSHELPERS_HPP
