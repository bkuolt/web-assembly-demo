/**
 * @file Context.hpp
 * @brief RAII WebGPU Context and device management.
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
 * @brief RAII Manager for WebGPU Instance, Adapter, Device, Queue, and Surface.
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

    void Resize(uint32_t width, uint32_t height);

private:
    bool initialized_ = false;
    uint32_t width_ = 1280;
    uint32_t height_ = 720;
    WGPUInstance instance_ = nullptr;
    WGPUAdapter adapter_ = nullptr;
    WGPUDevice device_ = nullptr;
    WGPUQueue queue_ = nullptr;
    WGPUSurface surface_ = nullptr;
};

} // namespace bgl::gpu

