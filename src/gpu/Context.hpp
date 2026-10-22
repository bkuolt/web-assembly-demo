/**
 * @file Context.hpp
 * @brief Native & WebAssembly WebGPU Context management with 4x MSAA support.
 */
#pragma once

#include <webgpu/webgpu.h>
#include <string>
#include "core/Types.hpp"

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

    core::Result<void> Initialize(const ContextConfig& config);
    void Shutdown() noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }
    [[nodiscard]] uint32_t GetWidth() const noexcept { return width_; }
    [[nodiscard]] uint32_t GetHeight() const noexcept { return height_; }
    [[nodiscard]] WGPUDevice GetDevice() const noexcept { return device_; }
    [[nodiscard]] WGPUQueue GetQueue() const noexcept { return queue_; }
    [[nodiscard]] WGPUTextureFormat GetPreferredFormat() const noexcept { return format_; }
    [[nodiscard]] uint32_t GetSampleCount() const noexcept { return sampleCount_; }

    void Resize(uint32_t width, uint32_t height);
    bool PrepareFrame();
    [[nodiscard]] WGPUCommandEncoder GetCommandEncoder() const noexcept { return encoder_; }
    WGPURenderPassEncoder BeginFrame();
    void EndFrame(WGPURenderPassEncoder pass);

private:
    void CreateRenderBuffers();

    bool initialized_{false};
    uint32_t width_{1280};
    uint32_t height_{720};
    uint32_t sampleCount_{4};

    WGPUInstance instance_{nullptr};
    WGPUAdapter adapter_{nullptr};
    WGPUDevice device_{nullptr};
    WGPUQueue queue_{nullptr};
    WGPUSurface surface_{nullptr};
    WGPUTextureFormat format_{WGPUTextureFormat_BGRA8Unorm};

    WGPUTexture msaaColorTexture_{nullptr};
    WGPUTextureView msaaColorView_{nullptr};

    WGPUTexture depthTexture_{nullptr};
    WGPUTextureView depthView_{nullptr};

    WGPUSurfaceTexture surfaceTexture_{};
    WGPUTextureView currentView_{nullptr};
    WGPUCommandEncoder encoder_{nullptr};

    void* nativeWindow_{nullptr};
};

} // namespace bgl::gpu
