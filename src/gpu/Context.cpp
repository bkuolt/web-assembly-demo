/**
 * @file Context.cpp
 * @brief Implementation of hardware native WebGPU context using wgpu-native with 4x MSAA support.
 */
#include "gpu/Context.hpp"
#include "core/Log.hpp"
#include "scene/TextureDecoder.hpp"
#include <vector>

#if defined(BGL_PLATFORM_NATIVE)
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <webgpu/wgpu.h>
#endif

namespace bgl::gpu {

#if defined(BGL_PLATFORM_NATIVE)
namespace {

[[nodiscard]] WGPUPresentMode SelectPresentMode(WGPUSurface surface, WGPUAdapter adapter) noexcept {
    WGPUPresentMode chosen{WGPUPresentMode_Fifo};
    if (!surface || !adapter)
        return chosen;

    WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
    wgpuSurfaceGetCapabilities(surface, adapter, &caps);
    if (caps.presentModeCount > 0 && caps.presentModes != nullptr) {
        for (size_t i{0}; i < caps.presentModeCount; ++i) {
            if (caps.presentModes[i] == WGPUPresentMode_Immediate) {
                chosen = WGPUPresentMode_Immediate;
                break;
            }
            if (caps.presentModes[i] == WGPUPresentMode_Mailbox) {
                chosen = WGPUPresentMode_Mailbox;
            }
        }
    }
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    return chosen;
}

void ConfigureSwapchainSurface(WGPUSurface surface, WGPUAdapter adapter, WGPUDevice device, WGPUTextureFormat format,
                               uint32_t width, uint32_t height) {
    WGPUSurfaceConfiguration configSurface = {};
    configSurface.device = device;
    configSurface.format = format;
    configSurface.usage = WGPUTextureUsage_RenderAttachment;
    configSurface.width = width;
    configSurface.height = height;
    configSurface.presentMode = SelectPresentMode(surface, adapter);
    configSurface.alphaMode = WGPUCompositeAlphaMode_Auto;
    wgpuSurfaceConfigure(surface, &configSurface);
}

void CreateRenderTarget(WGPUDevice device, uint32_t width, uint32_t height, WGPUTextureFormat format,
                        WGPUTextureUsage usage, uint32_t sampleCount, const char* label, TextureHandle& outTexture,
                        TextureViewHandle& outView) {
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.usage = usage;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = WGPUExtent3D{width, height, 1};
    desc.format = format;
    desc.mipLevelCount = 1;
    desc.sampleCount = sampleCount;
    desc.label = ToWgpuStringView(label);
    outTexture = wgpuDeviceCreateTexture(device, &desc);
    if (outTexture) {
        outView = wgpuTextureCreateView(outTexture.Get(), nullptr);
    }
}

[[nodiscard]] WGPURenderPassColorAttachment BuildHdrColorAttachment(uint32_t sampleCount, WGPUTextureView msaaView,
                                                                    WGPUTextureView hdrResolveView,
                                                                    WGPUTextureView swapchainView, WGPULoadOp loadOp,
                                                                    bool preserveMsaa) noexcept {
    WGPURenderPassColorAttachment attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    if (sampleCount > 1 && msaaView && hdrResolveView) {
        attachment.view = msaaView;
        attachment.resolveTarget = hdrResolveView;
        attachment.storeOp = preserveMsaa ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
    } else if (hdrResolveView) {
        attachment.view = hdrResolveView;
        attachment.resolveTarget = nullptr;
        attachment.storeOp = WGPUStoreOp_Store;
    } else if (sampleCount > 1 && msaaView) {
        attachment.view = msaaView;
        attachment.resolveTarget = swapchainView;
        attachment.storeOp = WGPUStoreOp_Discard;
    } else {
        attachment.view = swapchainView;
        attachment.resolveTarget = nullptr;
        attachment.storeOp = WGPUStoreOp_Store;
    }
    attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    attachment.loadOp = loadOp;
    return attachment;
}

void EndAndReleasePass(WGPURenderPassEncoder pass) noexcept {
    if (pass) {
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    }
}

} // namespace
#endif

Context::Context() = default;

Context::~Context() {
    Shutdown();
}

void Context::Shutdown() noexcept {
    if (!initialized_ && !instance_)
        return;
#if defined(BGL_PLATFORM_NATIVE)
    encoder_.Reset();
    currentView_.Reset();
    if (surfaceTexture_.texture) {
        wgpuTextureRelease(surfaceTexture_.texture);
        surfaceTexture_.texture = nullptr;
    }
    hdrResolveView_.Reset();
    hdrResolveTexture_.Reset();
    msaaColorView_.Reset();
    msaaColorTexture_.Reset();
    depthReadView_.Reset();
    depthView_.Reset();
    depthTexture_.Reset();
    queue_.Reset();
    device_.Reset();
    surface_.Reset();
    adapter_.Reset();
    instance_.Reset();
    supportsBC_ = false;
    supportsASTC_ = false;
    supportsETC2_ = false;
    supportsDesktopRayTracing_ = false;
    supportsBindlessTextures_ = false;
    supportsSubgroups_ = false;
    supportsTimestampQueries_ = false;
    maxStorageBuffersPerShaderStage_ = 4;
    maxBindingArrayElements_ = 0;
#endif
    initialized_ = false;
    BGL_LOG_INFO("Native WebGPU Context cleanly shut down.");
}

void Context::CreateRenderBuffers() {
#if defined(BGL_PLATFORM_NATIVE)
    hdrResolveView_.Reset();
    hdrResolveTexture_.Reset();
    msaaColorView_.Reset();
    msaaColorTexture_.Reset();
    depthReadView_.Reset();
    depthView_.Reset();
    depthTexture_.Reset();

    // 1. Create 4x MSAA Linear HDR (RGBA16Float) Color Texture & View
    if (sampleCount_ > 1) {
        CreateRenderTarget(device_.Get(), width_, height_, WGPUTextureFormat_RGBA16Float,
                           WGPUTextureUsage_RenderAttachment, sampleCount_, "HDR_MSAAColorTexture_RGBA16F",
                           msaaColorTexture_, msaaColorView_);
    }

    // 1b. Create 1x Linear HDR (RGBA16Float) Resolve & Sampled Texture for Tonemapping Post-Process
    CreateRenderTarget(device_.Get(), width_, height_, WGPUTextureFormat_RGBA16Float,
                       WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc,
                       1, "HDR_ResolveTexture_RGBA16F", hdrResolveTexture_, hdrResolveView_);

    // 2. Create Depth Buffer (with matching sample count + TextureBinding for depth-based post-processing)
    CreateRenderTarget(device_.Get(), width_, height_, WGPUTextureFormat_Depth24Plus,
                       WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, sampleCount_, "DepthBuffer",
                       depthTexture_, depthView_);
    if (depthTexture_) {
        WGPUTextureViewDescriptor readViewDesc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        readViewDesc.format = WGPUTextureFormat_Depth24Plus;
        readViewDesc.dimension = WGPUTextureViewDimension_2D;
        readViewDesc.baseMipLevel = 0;
        readViewDesc.mipLevelCount = 1;
        readViewDesc.baseArrayLayer = 0;
        readViewDesc.arrayLayerCount = 1;
        readViewDesc.aspect = WGPUTextureAspect_DepthOnly;
        readViewDesc.label = ToWgpuStringView("DepthBuffer_ReadOnlyView");
        depthReadView_ = wgpuTextureCreateView(depthTexture_.Get(), &readViewDesc);
    }

    BGL_LOG_INFO("WebGPU HDR Render Buffers created ({}x{}, Color: RGBA16Float, Depth: Depth24Plus, MSAA: {}x)", width_,
                 height_, sampleCount_);
#endif
}

core::Result<void> Context::Initialize(const ContextConfig& config) {
    width_ = config.width;
    height_ = config.height;
    sampleCount_ = config.sampleCount;
    nativeWindow_ = config.nativeWindowHandle;

#if defined(BGL_PLATFORM_NATIVE)
    BGL_LOG_INFO("Initializing hardware native WebGPU runtime via wgpu-native (MSAA: {}x)...", sampleCount_);

    // 1. Create WebGPU Instance
    WGPUInstanceDescriptor instanceDesc = WGPU_INSTANCE_DESCRIPTOR_INIT;
    instance_ = wgpuCreateInstance(&instanceDesc);
    if (!instance_) {
        return std::unexpected("Failed to create WGPUInstance");
    }

    // 2. Create OS Surface
    if (nativeWindow_) {
        auto* sdlWindow = static_cast<SDL_Window*>(nativeWindow_);
        SDL_SysWMinfo wmInfo;
        SDL_VERSION(&wmInfo.version);
        if (!SDL_GetWindowWMInfo(sdlWindow, &wmInfo)) {
            return std::unexpected("Failed to get SDL window system info");
        }

        if (wmInfo.subsystem == SDL_SYSWM_X11) {
            WGPUSurfaceSourceXlibWindow fromXlib = WGPU_SURFACE_SOURCE_XLIB_WINDOW_INIT;
            fromXlib.display = wmInfo.info.x11.display;
            fromXlib.window = wmInfo.info.x11.window;

            WGPUSurfaceDescriptor surfDesc = WGPU_SURFACE_DESCRIPTOR_INIT;
            surfDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&fromXlib);
            surface_ = wgpuInstanceCreateSurface(instance_, &surfDesc);
            BGL_LOG_INFO("Native X11 WebGPU surface successfully created.");
        }
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
        else if (wmInfo.subsystem == SDL_SYSWM_WAYLAND) {
            WGPUSurfaceSourceWaylandSurface fromWayland = WGPU_SURFACE_SOURCE_WAYLAND_SURFACE_INIT;
            fromWayland.display = wmInfo.info.wl.display;
            fromWayland.surface = wmInfo.info.wl.surface;

            WGPUSurfaceDescriptor surfDesc = WGPU_SURFACE_DESCRIPTOR_INIT;
            surfDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&fromWayland);
            surface_ = wgpuInstanceCreateSurface(instance_, &surfDesc);
            BGL_LOG_INFO("Native Wayland WebGPU surface successfully created.");
        }
#endif
        else {
            return std::unexpected("Unsupported Linux windowing system (only X11/Wayland supported)");
        }
    }

    if (!surface_) {
        return std::unexpected("Failed to create WGPUSurface from native window handle");
    }

    // 3. Request Adapter
    WGPURequestAdapterOptions adapterOpts = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    adapterOpts.compatibleSurface = surface_;
    adapterOpts.powerPreference = WGPUPowerPreference_HighPerformance;

    struct AdapterData {
        WGPUAdapter adapter = nullptr;
        bool done = false;
    } adapterData;

    auto onAdapter = [](WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView msg, void* userdata1,
                        void* userdata2) {
        (void)userdata2;
        auto* data = static_cast<AdapterData*>(userdata1);
        if (status == WGPURequestAdapterStatus_Success) {
            data->adapter = adapter;
        } else {
            BGL_LOG_ERROR("Request adapter failed: {}", msg.data ? msg.data : "Unknown");
        }
        data->done = true;
    };

    WGPURequestAdapterCallbackInfo cbInfo = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    cbInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    cbInfo.callback = onAdapter;
    cbInfo.userdata1 = &adapterData;

    wgpuInstanceRequestAdapter(instance_, &adapterOpts, cbInfo);
    if (!adapterData.adapter) {
        return std::unexpected("Failed to acquire suitable hardware WebGPU adapter");
    }
    adapter_ = adapterData.adapter;

    WGPUAdapterInfo adapterInfo = WGPU_ADAPTER_INFO_INIT;
    wgpuAdapterGetInfo(adapter_, &adapterInfo);
    auto stringViewFromWgpu = [](WGPUStringView sv, std::string_view fallback) -> std::string_view {
        if (!sv.data)
            return fallback;
        if (sv.length == WGPU_STRLEN)
            return std::string_view{sv.data};
        return std::string_view{sv.data, sv.length};
    };
    const auto gpuDeviceName = stringViewFromWgpu(adapterInfo.device, "Unknown GPU");
    const auto gpuDriverDesc = stringViewFromWgpu(adapterInfo.description, "Unknown Driver");
    BGL_LOG_INFO("WebGPU Adapter: {} (Driver: {}, Backend: {})", gpuDeviceName, gpuDriverDesc,
                 static_cast<int>(adapterInfo.backendType));
#if defined(BGL_PLATFORM_NATIVE)
    wgpuAdapterInfoFreeMembers(adapterInfo);
#endif

    // 4. Query Hardware Texture Compression Features (BC7 / ASTC / ETC2) & Desktop RayTracing Capabilities
    std::vector<WGPUFeatureName> requestedFeatures{};
    if (wgpuAdapterHasFeature(adapter_, WGPUFeatureName_TextureCompressionBC)) {
        requestedFeatures.push_back(WGPUFeatureName_TextureCompressionBC);
        supportsBC_ = true;
    }
    if (wgpuAdapterHasFeature(adapter_, WGPUFeatureName_TextureCompressionASTC)) {
        requestedFeatures.push_back(WGPUFeatureName_TextureCompressionASTC);
        supportsASTC_ = true;
    }
    if (wgpuAdapterHasFeature(adapter_, WGPUFeatureName_TextureCompressionETC2)) {
        requestedFeatures.push_back(WGPUFeatureName_TextureCompressionETC2);
        supportsETC2_ = true;
    }

    const bool hasTimestampQuery = wgpuAdapterHasFeature(adapter_, WGPUFeatureName_TimestampQuery);
    if (hasTimestampQuery) {
        requestedFeatures.push_back(WGPUFeatureName_TimestampQuery);
        supportsTimestampQueries_ = true;
    }
    const auto subgroupFeat = static_cast<WGPUFeatureName>(WGPUNativeFeature_Subgroup);
    if (wgpuAdapterHasFeature(adapter_, subgroupFeat)) {
        requestedFeatures.push_back(subgroupFeat);
        supportsSubgroups_ = true;
    }

    const auto texArrayFeat = static_cast<WGPUFeatureName>(WGPUNativeFeature_TextureBindingArray);
    const auto nonUniformFeat =
        static_cast<WGPUFeatureName>(WGPUNativeFeature_SampledTextureAndStorageBufferArrayNonUniformIndexing);
    const auto partialArrayFeat = static_cast<WGPUFeatureName>(WGPUNativeFeature_PartiallyBoundBindingArray);
    const bool hasBindlessBase =
        wgpuAdapterHasFeature(adapter_, texArrayFeat) && wgpuAdapterHasFeature(adapter_, nonUniformFeat);
    const bool hasPartialBind = wgpuAdapterHasFeature(adapter_, partialArrayFeat);

    BGL_LOG_INFO("WebGPU Hardware Texture Compression Support: BC7={} | ASTC_4x4={} | ETC2={}",
                 supportsBC_ ? "YES" : "NO", supportsASTC_ ? "YES" : "NO", supportsETC2_ ? "YES" : "NO");

    if (supportsBC_) {
        scene::TextureDecoder::SetDefaultKtxTarget(scene::KtxTranscodeTarget::Bc7);
    } else if (supportsASTC_) {
        scene::TextureDecoder::SetDefaultKtxTarget(scene::KtxTranscodeTarget::Astc4x4);
    } else if (supportsETC2_) {
        scene::TextureDecoder::SetDefaultKtxTarget(scene::KtxTranscodeTarget::Etc2Rgba);
    } else {
        scene::TextureDecoder::SetDefaultKtxTarget(scene::KtxTranscodeTarget::Rgba32);
    }
    BGL_LOG_INFO("Configured KTX2 transcoding target to native GPU block format: {}",
                 scene::TextureDecoder::GetKtxTargetName(scene::TextureDecoder::GetDefaultKtxTarget()));

    WGPULimits supportedLimits = WGPU_LIMITS_INIT;
    WGPUNativeLimits supportedNativeLimits = WGPU_NATIVE_LIMITS_INIT;
    supportedLimits.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&supportedNativeLimits);

    WGPULimits requiredLimits = WGPU_LIMITS_INIT;
    WGPUNativeLimits requiredNativeLimits = WGPU_NATIVE_LIMITS_INIT;

    if (wgpuAdapterGetLimits(adapter_, &supportedLimits) == WGPUStatus_Success) {
        if (supportedLimits.maxBindGroups >= 4u) {
            requiredLimits.maxBindGroups = 4u;
        }
        if (supportedLimits.maxDynamicUniformBuffersPerPipelineLayout >= 2u) {
            requiredLimits.maxDynamicUniformBuffersPerPipelineLayout = 2u;
        }
        if (supportedLimits.maxStorageBuffersPerShaderStage >= 4u) {
            maxStorageBuffersPerShaderStage_ = std::min(supportedLimits.maxStorageBuffersPerShaderStage, 10u);
            requiredLimits.maxStorageBuffersPerShaderStage = maxStorageBuffersPerShaderStage_;
        }
        if (supportedLimits.maxStorageTexturesPerShaderStage >= 4u) {
            requiredLimits.maxStorageTexturesPerShaderStage =
                std::min(supportedLimits.maxStorageTexturesPerShaderStage, 8u);
        }
        if (supportedLimits.maxStorageBufferBindingSize >= (128ull * 1024ull * 1024ull)) {
            requiredLimits.maxStorageBufferBindingSize =
                std::min<uint64_t>(supportedLimits.maxStorageBufferBindingSize, 256ull * 1024ull * 1024ull);
        }
        if (supportedLimits.maxBufferSize >= (256ull * 1024ull * 1024ull)) {
            requiredLimits.maxBufferSize =
                std::min<uint64_t>(supportedLimits.maxBufferSize, 512ull * 1024ull * 1024ull);
        }
        if (supportedLimits.minUniformBufferOffsetAlignment <= 256u) {
            requiredLimits.minUniformBufferOffsetAlignment = 256u;
        }

        if (hasBindlessBase &&
            supportedNativeLimits.maxBindingArrayElementsPerShaderStage != WGPU_LIMIT_U32_UNDEFINED &&
            supportedNativeLimits.maxBindingArrayElementsPerShaderStage >= 128u) {
            maxBindingArrayElements_ = 128u;
            requiredNativeLimits.maxBindingArrayElementsPerShaderStage = maxBindingArrayElements_;
            requiredLimits.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&requiredNativeLimits);
            requestedFeatures.push_back(texArrayFeat);
            requestedFeatures.push_back(nonUniformFeat);
            if (hasPartialBind) {
                requestedFeatures.push_back(partialArrayFeat);
            }
            supportsBindlessTextures_ = true;
        }
    }

    WGPUDeviceDescriptor devDesc = WGPU_DEVICE_DESCRIPTOR_INIT;
    devDesc.label = ToWgpuStringView("BglDevice");
    devDesc.requiredFeatureCount = requestedFeatures.size();
    devDesc.requiredFeatures = requestedFeatures.empty() ? nullptr : requestedFeatures.data();
    devDesc.requiredLimits = &requiredLimits;

    supportsDesktopRayTracing_ = (maxStorageBuffersPerShaderStage_ >= 6u);
    BGL_LOG_INFO("Desktop RayTracing Capabilities: ComputeBVH={} (StorageSlots={}) | BindlessTextures={} ({} slots) "
                 "| Subgroups={} | TimestampQuery={}",
                 supportsDesktopRayTracing_ ? "YES" : "NO", maxStorageBuffersPerShaderStage_,
                 supportsBindlessTextures_ ? "YES" : "NO", maxBindingArrayElements_, supportsSubgroups_ ? "YES" : "NO",
                 supportsTimestampQueries_ ? "YES" : "NO");

    struct DeviceData {
        WGPUDevice device = nullptr;
        bool done = false;
    } devData;

    auto onDevice = [](WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView msg, void* userdata1,
                       void* userdata2) {
        (void)userdata2;
        auto* data = static_cast<DeviceData*>(userdata1);
        if (status == WGPURequestDeviceStatus_Success) {
            data->device = device;
        } else {
            BGL_LOG_ERROR("Request device failed: {}", msg.data ? msg.data : "Unknown");
        }
        data->done = true;
    };

    WGPURequestDeviceCallbackInfo devCbInfo = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    devCbInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    devCbInfo.callback = onDevice;
    devCbInfo.userdata1 = &devData;

    wgpuAdapterRequestDevice(adapter_, &devDesc, devCbInfo);
    if (!devData.device) {
        return std::unexpected("Failed to create WebGPU logical device");
    }
    device_ = devData.device;
    queue_ = wgpuDeviceGetQueue(device_);

    // 5. Configure Swapchain Surface
    if (surface_) {
        WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
        wgpuSurfaceGetCapabilities(surface_, adapter_, &caps);
        if (caps.formatCount > 0 && caps.formats != nullptr) {
            format_ = caps.formats[0];
        } else {
            format_ = WGPUTextureFormat_BGRA8Unorm;
        }
        wgpuSurfaceCapabilitiesFreeMembers(caps);

        ConfigureSwapchainSurface(surface_.Get(), adapter_.Get(), device_.Get(), format_, width_, height_);
        BGL_LOG_INFO("WebGPU Surface configured ({}x{}, format: {})", width_, height_, static_cast<int>(format_));
    }

    CreateRenderBuffers();
#endif

    initialized_ = true;
    BGL_LOG_INFO("Native WebGPU Context successfully ready!");
    return {};
}

void Context::Resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0)
        return;
    width_ = width;
    height_ = height;
#if defined(BGL_PLATFORM_NATIVE)
    if (surface_ && device_ && adapter_) {
        ConfigureSwapchainSurface(surface_.Get(), adapter_.Get(), device_.Get(), format_, width_, height_);
    }
    CreateRenderBuffers();
#endif
    BGL_LOG_INFO("WebGPU Surface resized to {}x{}", width, height);
}

bool Context::PrepareFrame() {
#if defined(BGL_PLATFORM_NATIVE)
    if (encoder_)
        return true;
    if (!surface_ || !device_ || !queue_)
        return false;

    surfaceTexture_ = {};
    wgpuSurfaceGetCurrentTexture(surface_, &surfaceTexture_);
    if (surfaceTexture_.status == WGPUSurfaceGetCurrentTextureStatus_Timeout ||
        surfaceTexture_.status == WGPUSurfaceGetCurrentTextureStatus_Outdated) {
        ConfigureSwapchainSurface(surface_.Get(), adapter_.Get(), device_.Get(), format_, width_, height_);
        return false;
    }

    if (surfaceTexture_.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        surfaceTexture_.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        return false;
    }

    currentView_ = wgpuTextureCreateView(surfaceTexture_.texture, nullptr);
    if (!currentView_) {
        wgpuTextureRelease(surfaceTexture_.texture);
        surfaceTexture_.texture = nullptr;
        return false;
    }

    encoder_ = wgpuDeviceCreateCommandEncoder(device_, nullptr);
    if (!encoder_) {
        currentView_.Reset();
        wgpuTextureRelease(surfaceTexture_.texture);
        surfaceTexture_.texture = nullptr;
        return false;
    }
    return true;
#else
    return false;
#endif
}

WGPURenderPassEncoder Context::BeginFrame() {
#if defined(BGL_PLATFORM_NATIVE)
    if (!encoder_ && !PrepareFrame()) {
        return nullptr;
    }

    WGPURenderPassColorAttachment colorAttachment =
        BuildHdrColorAttachment(sampleCount_, msaaColorView_.Get(), hdrResolveView_.Get(), currentView_.Get(),
                                WGPULoadOp_Clear, /*preserveMsaa=*/true);
    colorAttachment.clearValue = WGPUColor{0.04, 0.06, 0.09, 1.0}; // Linear HDR dark studio slate

    WGPURenderPassDescriptor passDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    passDesc.colorAttachmentCount = 1;
    passDesc.colorAttachments = &colorAttachment;
    passDesc.label = ToWgpuStringView("HDR_SceneRenderPass");

    WGPURenderPassDepthStencilAttachment depthAttachment = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    if (depthView_) {
        depthAttachment.view = depthView_;
        depthAttachment.depthLoadOp = WGPULoadOp_Clear;
        depthAttachment.depthStoreOp = WGPUStoreOp_Store;
        depthAttachment.depthClearValue = 1.0f;
        depthAttachment.depthReadOnly = WGPU_FALSE;
        passDesc.depthStencilAttachment = &depthAttachment;
    }

    return wgpuCommandEncoderBeginRenderPass(encoder_, &passDesc);
#else
    return nullptr;
#endif
}

void Context::SnapshotOpaqueToCopy(WGPURenderPassEncoder opaquePass, WGPUTexture opaqueCopyDst, uint32_t copyW,
                                   uint32_t copyH) {
#if defined(BGL_PLATFORM_NATIVE)
    EndAndReleasePass(opaquePass);
    if (!encoder_) {
        return;
    }

    if (hdrResolveTexture_ && opaqueCopyDst && copyW > 0 && copyH > 0) {
        const uint32_t copyWidth = std::min(width_, copyW);
        const uint32_t copyHeight = std::min(height_, copyH);
        if (copyWidth > 0 && copyHeight > 0) {
            WGPUTexelCopyTextureInfo srcInfo = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            srcInfo.texture = hdrResolveTexture_.Get();
            srcInfo.mipLevel = 0;
            srcInfo.origin = WGPUOrigin3D{0, 0, 0};
            srcInfo.aspect = WGPUTextureAspect_All;

            WGPUTexelCopyTextureInfo dstInfo = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            dstInfo.texture = opaqueCopyDst;
            dstInfo.mipLevel = 0;
            dstInfo.origin = WGPUOrigin3D{0, 0, 0};
            dstInfo.aspect = WGPUTextureAspect_All;

            const WGPUExtent3D extent{copyWidth, copyHeight, 1};
            wgpuCommandEncoderCopyTextureToTexture(encoder_, &srcInfo, &dstInfo, &extent);
        }
    }
#else
    (void)opaquePass;
    (void)opaqueCopyDst;
    (void)copyW;
    (void)copyH;
#endif
}

WGPURenderPassEncoder Context::OpenTransparentPass() {
#if defined(BGL_PLATFORM_NATIVE)
    if (!encoder_) {
        return nullptr;
    }

    WGPURenderPassColorAttachment colorAttachment =
        BuildHdrColorAttachment(sampleCount_, msaaColorView_.Get(), hdrResolveView_.Get(), currentView_.Get(),
                                WGPULoadOp_Load, /*preserveMsaa=*/false);

    WGPURenderPassDescriptor passDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    passDesc.colorAttachmentCount = 1;
    passDesc.colorAttachments = &colorAttachment;
    passDesc.label = ToWgpuStringView("HDR_TransparentRenderPass");

    WGPURenderPassDepthStencilAttachment depthAttachment = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    if (depthView_) {
        depthAttachment.view = depthView_;
        depthAttachment.depthLoadOp = WGPULoadOp_Load;
        depthAttachment.depthStoreOp = WGPUStoreOp_Store;
        depthAttachment.depthReadOnly = WGPU_FALSE;
        passDesc.depthStencilAttachment = &depthAttachment;
    }

    return wgpuCommandEncoderBeginRenderPass(encoder_, &passDesc);
#else
    return nullptr;
#endif
}

WGPURenderPassEncoder Context::BeginTransparentPass(WGPURenderPassEncoder opaquePass, WGPUTexture opaqueCopyDst,
                                                    uint32_t copyW, uint32_t copyH) {
    SnapshotOpaqueToCopy(opaquePass, opaqueCopyDst, copyW, copyH);
    return OpenTransparentPass();
}

void Context::FinishHdrPass(WGPURenderPassEncoder hdrPass) {
#if defined(BGL_PLATFORM_NATIVE)
    EndAndReleasePass(hdrPass);
#else
    (void)hdrPass;
#endif
}

WGPURenderPassEncoder Context::BeginSwapchainPass() {
#if defined(BGL_PLATFORM_NATIVE)
    if (!encoder_ || !currentView_) {
        return nullptr;
    }

    WGPURenderPassColorAttachment colorAttachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    colorAttachment.view = currentView_;
    colorAttachment.resolveTarget = nullptr;
    colorAttachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    colorAttachment.loadOp = WGPULoadOp_Clear;
    colorAttachment.storeOp = WGPUStoreOp_Store;
    colorAttachment.clearValue = WGPUColor{0.05, 0.07, 0.11, 1.0};

    WGPURenderPassDescriptor passDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    passDesc.colorAttachmentCount = 1;
    passDesc.colorAttachments = &colorAttachment;
    passDesc.depthStencilAttachment = nullptr;
    passDesc.label = ToWgpuStringView("SwapchainTonemapAndHudPass");

    return wgpuCommandEncoderBeginRenderPass(encoder_, &passDesc);
#else
    return nullptr;
#endif
}

WGPURenderPassEncoder Context::BeginPostProcessPass(WGPURenderPassEncoder hdrPass) {
    FinishHdrPass(hdrPass);
    return BeginSwapchainPass();
}

void Context::EndFrame(WGPURenderPassEncoder pass) {
#if defined(BGL_PLATFORM_NATIVE)
    EndAndReleasePass(pass);

    if (encoder_) {
        CommandBufferHandle cmdBuf{wgpuCommandEncoderFinish(encoder_, nullptr)};
        encoder_.Reset();

        if (cmdBuf && queue_) {
            WGPUCommandBuffer rawCmd{cmdBuf.Get()};
            wgpuQueueSubmit(queue_, 1, &rawCmd);
        }
    }

    if (surface_) {
        wgpuSurfacePresent(surface_);
    }

    currentView_.Reset();

    if (surfaceTexture_.texture) {
        wgpuTextureRelease(surfaceTexture_.texture);
        surfaceTexture_.texture = nullptr;
    }
#endif
}

} // namespace bgl::gpu
