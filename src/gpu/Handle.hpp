/**
 * @file Handle.hpp
 * @brief Modern C++23 zero-overhead RAII wrapper for WebGPU C API handles.
 */
#pragma once

#include <webgpu/webgpu.h>
#include <string_view>
#include <utility>
#include <concepts>

namespace bgl::gpu {

/**
 * @brief Converts a C++ `std::string_view` to a `WGPUStringView` at compile time without `strlen`.
 */
[[nodiscard]] constexpr WGPUStringView ToWgpuStringView(std::string_view str) noexcept {
    return WGPUStringView{str.data(), str.size()};
}

/**
 * @class Handle
 * @brief Move-only RAII wrapper for opaque WebGPU handles (`WGPUBuffer`, `WGPUTexture`, etc.).
 * @tparam T The WebGPU pointer handle type.
 * @tparam Deleter Callable function pointer or stateless functor releasing the handle.
 */
template <typename T, auto Deleter>
    requires std::is_pointer_v<T> && std::invocable<decltype(Deleter), T>
class Handle {
public:
    constexpr Handle() noexcept = default;
    constexpr Handle(std::nullptr_t) noexcept : handle_{nullptr} {}
    explicit constexpr Handle(T handle) noexcept : handle_{handle} {}

    ~Handle() noexcept {
        Reset();
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    constexpr Handle(Handle&& other) noexcept
        : handle_{std::exchange(other.handle_, nullptr)} {}

    constexpr Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            Reset(std::exchange(other.handle_, nullptr));
        }
        return *this;
    }

    constexpr Handle& operator=(std::nullptr_t) noexcept {
        Reset();
        return *this;
    }

    constexpr Handle& operator=(T rawHandle) noexcept {
        Reset(rawHandle);
        return *this;
    }

    void Reset(T newHandle = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != newHandle) {
            Deleter(handle_);
        }
        handle_ = newHandle;
    }

    [[nodiscard]] constexpr T Release() noexcept {
        return std::exchange(handle_, nullptr);
    }

    [[nodiscard]] constexpr T Get() const noexcept {
        return handle_;
    }

    [[nodiscard]] constexpr const T* AddressOf() const noexcept {
        return &handle_;
    }

    [[nodiscard]] constexpr operator T() const noexcept {
        return handle_;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

    [[nodiscard]] constexpr bool operator==(std::nullptr_t) const noexcept {
        return handle_ == nullptr;
    }

    [[nodiscard]] constexpr bool operator==(T other) const noexcept {
        return handle_ == other;
    }

private:
    T handle_{nullptr};
};

using InstanceHandle          = Handle<WGPUInstance, wgpuInstanceRelease>;
using AdapterHandle           = Handle<WGPUAdapter, wgpuAdapterRelease>;
using DeviceHandle            = Handle<WGPUDevice, wgpuDeviceRelease>;
using QueueHandle             = Handle<WGPUQueue, wgpuQueueRelease>;
using SurfaceHandle           = Handle<WGPUSurface, wgpuSurfaceRelease>;
using BufferHandle            = Handle<WGPUBuffer, wgpuBufferRelease>;
using TextureHandle           = Handle<WGPUTexture, wgpuTextureRelease>;
using TextureViewHandle       = Handle<WGPUTextureView, wgpuTextureViewRelease>;
using SamplerHandle           = Handle<WGPUSampler, wgpuSamplerRelease>;
using BindGroupHandle         = Handle<WGPUBindGroup, wgpuBindGroupRelease>;
using BindGroupLayoutHandle   = Handle<WGPUBindGroupLayout, wgpuBindGroupLayoutRelease>;
using PipelineLayoutHandle     = Handle<WGPUPipelineLayout, wgpuPipelineLayoutRelease>;
using RenderPipelineHandle     = Handle<WGPURenderPipeline, wgpuRenderPipelineRelease>;
using ComputePipelineHandle    = Handle<WGPUComputePipeline, wgpuComputePipelineRelease>;
using ShaderModuleHandle       = Handle<WGPUShaderModule, wgpuShaderModuleRelease>;
using CommandEncoderHandle     = Handle<WGPUCommandEncoder, wgpuCommandEncoderRelease>;
using RenderPassEncoderHandle  = Handle<WGPURenderPassEncoder, wgpuRenderPassEncoderRelease>;
using ComputePassEncoderHandle = Handle<WGPUComputePassEncoder, wgpuComputePassEncoderRelease>;
using CommandBufferHandle      = Handle<WGPUCommandBuffer, wgpuCommandBufferRelease>;

} // namespace bgl::gpu

