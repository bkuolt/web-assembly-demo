/**
 * @file Context.hpp
 * @brief Native & WebAssembly WebGPU Context management with 4x MSAA support.
 */
#ifndef BGL_GPU_CONTEXT_HPP
#define BGL_GPU_CONTEXT_HPP

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
    [[nodiscard]] WGPUTexture GetHdrResolveTexture() const noexcept { return hdrResolveTexture_.Get(); }
    [[nodiscard]] WGPUTextureView GetDepthReadView() const noexcept { return depthReadView_.Get(); }
    [[nodiscard]] uint32_t GetSampleCount() const noexcept { return sampleCount_; }

    /** @brief Returns true if BC7/BC compressed textures are supported by the adapter. */
    [[nodiscard]] bool SupportsBC() const noexcept { return supportsBC_; }
    /** @brief Returns true if ASTC 4x4 compressed textures are supported by the adapter. */
    [[nodiscard]] bool SupportsASTC() const noexcept { return supportsASTC_; }
    /** @brief Returns true if ETC2 compressed textures are supported by the adapter. */
    [[nodiscard]] bool SupportsETC2() const noexcept { return supportsETC2_; }
    /** @brief Returns true when compute-shader 2-level BVH ray tracing limits are met on desktop (`wgpu-native`). */
    [[nodiscard]] bool SupportsDesktopRayTracing() const noexcept { return supportsDesktopRayTracing_; }
    /** @brief Returns true when native bindless `binding_array` texture sampling is active. */
    [[nodiscard]] bool SupportsBindlessTextures() const noexcept { return supportsBindlessTextures_; }
    /** @brief Returns true when WebGPU subgroup operations are supported by the device. */
    [[nodiscard]] bool SupportsSubgroups() const noexcept { return supportsSubgroups_; }
    /** @brief Returns true when GPU timestamp queries are enabled on the device. */
    [[nodiscard]] bool SupportsTimestampQueries() const noexcept { return supportsTimestampQueries_; }
    /** @brief Returns the negotiated `maxStorageBuffersPerShaderStage` device limit. */
    [[nodiscard]] uint32_t GetMaxStorageBuffersPerShaderStage() const noexcept {
        return maxStorageBuffersPerShaderStage_;
    }
    /** @brief Returns the negotiated native bindless texture binding-array capacity. */
    [[nodiscard]] uint32_t GetMaxBindingArrayElements() const noexcept { return maxBindingArrayElements_; }

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
     * @brief Resolves and snapshots the opaque+skybox HDR buffer into Mip-0 of `opaqueCopyDst` and closes opaque pass.
     */
    void SnapshotOpaqueToCopy(WGPURenderPassEncoder opaquePass, WGPUTexture opaqueCopyDst, uint32_t copyW,
                              uint32_t copyH);

    /**
     * @brief Opens the transparent/transmissive HDR pass loading preserved MSAA color & depth.
     */
    WGPURenderPassEncoder OpenTransparentPass();

    /**
     * @brief Resolves and snapshots the opaque+skybox HDR buffer, then opens the transparent/transmissive pass.
     * @param opaquePass Active opaque HDR pass encoder that will be ended.
     * @param opaqueCopyDst Destination single-sampled RGBA16Float texture sampled during refraction.
     * @param copyW Destination texture width in pixels.
     * @param copyH Destination texture height in pixels.
     * @return New render pass encoder loading preserved MSAA color & depth for transparent/transmissive draws.
     */
    WGPURenderPassEncoder BeginTransparentPass(WGPURenderPassEncoder opaquePass, WGPUTexture opaqueCopyDst,
                                               uint32_t copyW, uint32_t copyH);

    /**
     * @brief Ends the active HDR pass so resolved color & depth views can be sampled by post-processing.
     */
    void FinishHdrPass(WGPURenderPassEncoder hdrPass);

    /**
     * @brief Opens the single-sampled presentation swapchain render pass.
     */
    WGPURenderPassEncoder BeginSwapchainPass();

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

    /**
     * @brief Releases an active in-progress frame's encoder/view/surface without submitting (e.g. on pass creation
     * failure).
     */
    void AbortFrame() noexcept;

private:
    void CreateRenderBuffers();

    bool initialized_{false};
    bool supportsBC_{false};
    bool supportsASTC_{false};
    bool supportsETC2_{false};
    bool supportsDesktopRayTracing_{false};
    bool supportsBindlessTextures_{false};
    bool supportsSubgroups_{false};
    bool supportsTimestampQueries_{false};
    uint32_t maxStorageBuffersPerShaderStage_{4};
    uint32_t maxBindingArrayElements_{0};
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
    TextureViewHandle depthReadView_{};

    WGPUSurfaceTexture surfaceTexture_{};
    TextureViewHandle currentView_{};
    CommandEncoderHandle encoder_{};

    void* nativeWindow_{nullptr};
};

} // namespace bgl::gpu

#endif // BGL_GPU_CONTEXT_HPP
