/**
 * @file Context.hpp
 * @brief Native & WebAssembly WebGPU Context management.
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
    uint32_t width = 1280;
    uint32_t height = 720;
    const char* canvasId = "#canvas";
    void* nativeWindowHandle = nullptr;
};

/**
 * @class Context
 * @brief RAII Manager for WebGPU Instance, Adapter, Device, Queue, Surface, and RenderPass.
 */
class Context {
public:
    Context();
    ~Context();

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    core::Result<void> Initialize(const ContextConfig& config);

    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }
    [[nodiscard]] uint32_t GetWidth() const noexcept { return width_; }
    [[nodiscard]] uint32_t GetHeight() const noexcept { return height_; }
    [[nodiscard]] WGPUDevice GetDevice() const noexcept { return device_; }
    [[nodiscard]] WGPUQueue GetQueue() const noexcept { return queue_; }
    [[nodiscard]] WGPUTextureFormat GetPreferredFormat() const noexcept { return format_; }

    void Resize(uint32_t width, uint32_t height);
    WGPURenderPassEncoder BeginFrame();
    void EndFrame(WGPURenderPassEncoder pass);

private:
    void CreateDepthBuffer();

    bool initialized_ = false;
    uint32_t width_ = 1280;
    uint32_t height_ = 720;

    WGPUInstance instance_ = nullptr;
    WGPUAdapter adapter_ = nullptr;
    WGPUDevice device_ = nullptr;
    WGPUQueue queue_ = nullptr;
    WGPUSurface surface_ = nullptr;
    WGPUTextureFormat format_ = WGPUTextureFormat_BGRA8Unorm;

    WGPUTexture depthTexture_ = nullptr;
    WGPUTextureView depthView_ = nullptr;

    WGPUSurfaceTexture surfaceTexture_ = {};
    WGPUTextureView currentView_ = nullptr;
    WGPUCommandEncoder encoder_ = nullptr;

    void* nativeWindow_ = nullptr;
};

} // namespace bgl::gpu
