/**
 * @file Context.hpp
 * @brief Native & WebAssembly WebGPU Context management with 4x MSAA support.
 */
#pragma once

#include "core/Types.hpp"
#include "gpu/Handle.hpp"
#include <string>
#include <webgpu/webgpu.h>

namespace bgl::gpu {

/**
 * @struct ContextConfig
 * @brief Window and surface configuration for WebGPU setup.
 */
struct ContextConfig {
    uint32_t width{1280};
    uint32_t height{720};
    uint32_t sampleCount{4}; // 4x MSAA by default
    const char* canvasId{"#canvas"};
    void* nativeWindowHandle{nullptr};
};

/**
 * @class Context
 * @brief RAII Manager for WebGPU Instance, Adapter, Device, Queue, Surface, MSAA, and RenderPass.
 */
class Context {
public:
    Context();
    ~Context();

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    /**
     * @brief Creates WebGPU instance, adapter, device, surface, and multisampled HDR render buffers.
     * @param config Window dimensions, MSAA sample count, and native handle.
     * @return Result indicating success or error description.
     */
    core::Result<void> Initialize(const ContextConfig& config);

    /**
     * @brief Releases all allocated WebGPU swapchain surfaces, views, and device handles.
     */
    void Shutdown() noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }
    [[nodiscard]] uint32_t GetWidth() const noexcept { return width_; }
    [[nodiscard]] uint32_t GetHeight() const noexcept { return height_; }
    [[nodiscard]] WGPUDevice GetDevice() const noexcept { return device_.Get(); }
    [[nodiscard]] WGPUQueue GetQueue() const noexcept { return queue_.Get(); }
    [[nodiscard]] WGPUTextureFormat GetPreferredFormat() const noexcept { return format_; }
    [[nodiscard]] static constexpr WGPUTextureFormat GetHdrFormat() noexcept { return WGPUTextureFormat_RGBA16Float; }
    [[nodiscard]] WGPUTextureView GetHdrResolveView() const noexcept { return hdrResolveView_.Get(); }
    [[nodiscard]] uint32_t GetSampleCount() const noexcept { return sampleCount_; }

    [[nodiscard]] bool SupportsBC() const noexcept { return supportsBC_; }
    [[nodiscard]] bool SupportsASTC() const noexcept { return supportsASTC_; }
    [[nodiscard]] bool SupportsETC2() const noexcept { return supportsETC2_; }

    /**
     * @brief Reconfigures swapchain surface and recreates multisampled HDR buffers upon window resize.
     * @param width New window width in pixels.
     * @param height New window height in pixels.
     */
    void Resize(uint32_t width, uint32_t height);

    /**
     * @brief Obtains the next swapchain presentation texture and begins command encoding.
     * @return true if presentation target was successfully acquired; false on device loss or retry.
     */
    bool PrepareFrame();

    [[nodiscard]] WGPUCommandEncoder GetCommandEncoder() const noexcept { return encoder_.Get(); }

    /**
     * @brief Begins the primary multisampled RGBA16Float HDR render pass.
     * @return Active render pass encoder targeting the HDR color and depth attachments.
     */
    WGPURenderPassEncoder BeginFrame();

    /**
     * @brief Transitions from the HDR pass to the single-sampled swapchain post-processing pass.
     * @param hdrPass Active HDR pass encoder that will be ended.
     * @return New render pass encoder targeting the presentation swapchain surface.
     */
    WGPURenderPassEncoder BeginPostProcessPass(WGPURenderPassEncoder hdrPass);

    /**
     * @brief Ends the active render pass, finishes command buffer recording, and submits to queue.
     * @param pass Active render pass encoder to finalize.
     */
    void EndFrame(WGPURenderPassEncoder pass);

private:
    void CreateRenderBuffers();

    bool initialized_{false};
    bool supportsBC_{false};
    bool supportsASTC_{false};
    bool supportsETC2_{false};
    uint32_t width_{1280};
    uint32_t height_{720};
    uint32_t sampleCount_{4};

    InstanceHandle instance_{};
    AdapterHandle adapter_{};
    DeviceHandle device_{};
    QueueHandle queue_{};
    SurfaceHandle surface_{};
    WGPUTextureFormat format_{WGPUTextureFormat_BGRA8Unorm};

    TextureHandle msaaColorTexture_{};
    TextureViewHandle msaaColorView_{};

    TextureHandle hdrResolveTexture_{};
    TextureViewHandle hdrResolveView_{};

    TextureHandle depthTexture_{};
    TextureViewHandle depthView_{};

    WGPUSurfaceTexture surfaceTexture_{};
    TextureViewHandle currentView_{};
    CommandEncoderHandle encoder_{};

    void* nativeWindow_{nullptr};
};

} // namespace bgl::gpu
