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

Context::Context() = default;

Context::~Context() {
    Shutdown();
}

void Context::Shutdown() noexcept {
    if (!initialized_ && !instance_) return;
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
    depthView_.Reset();
    depthTexture_.Reset();
    queue_.Reset();
    device_.Reset();
    surface_.Reset();
    adapter_.Reset();
    instance_.Reset();
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
    depthView_.Reset();
    depthTexture_.Reset();

    // 1. Create 4x MSAA Linear HDR (RGBA16Float) Color Texture & View
    if (sampleCount_ > 1) {
        WGPUTextureDescriptor msaaDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        msaaDesc.usage = WGPUTextureUsage_RenderAttachment;
        msaaDesc.dimension = WGPUTextureDimension_2D;
        msaaDesc.size = WGPUExtent3D{ width_, height_, 1 };
        msaaDesc.format = WGPUTextureFormat_RGBA16Float;
        msaaDesc.mipLevelCount = 1;
        msaaDesc.sampleCount = sampleCount_;
        msaaDesc.label = ToWgpuStringView("HDR_MSAAColorTexture_RGBA16F");
        msaaColorTexture_ = wgpuDeviceCreateTexture(device_, &msaaDesc);
        if (msaaColorTexture_) {
            msaaColorView_ = wgpuTextureCreateView(msaaColorTexture_, nullptr);
        }
    }

    // 1b. Create 1x Linear HDR (RGBA16Float) Resolve & Sampled Texture for Tonemapping Post-Process
    WGPUTextureDescriptor hdrResolveDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    hdrResolveDesc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc;
    hdrResolveDesc.dimension = WGPUTextureDimension_2D;
    hdrResolveDesc.size = WGPUExtent3D{ width_, height_, 1 };
    hdrResolveDesc.format = WGPUTextureFormat_RGBA16Float;
    hdrResolveDesc.mipLevelCount = 1;
    hdrResolveDesc.sampleCount = 1;
    hdrResolveDesc.label = ToWgpuStringView("HDR_ResolveTexture_RGBA16F");
    hdrResolveTexture_ = wgpuDeviceCreateTexture(device_, &hdrResolveDesc);
    if (hdrResolveTexture_) {
        hdrResolveView_ = wgpuTextureCreateView(hdrResolveTexture_, nullptr);
    }

    // 2. Create Depth Buffer (with matching sample count)
    WGPUTextureDescriptor depthDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    depthDesc.usage = WGPUTextureUsage_RenderAttachment;
    depthDesc.dimension = WGPUTextureDimension_2D;
    depthDesc.size = WGPUExtent3D{ width_, height_, 1 };
    depthDesc.format = WGPUTextureFormat_Depth24Plus;
    depthDesc.mipLevelCount = 1;
    depthDesc.sampleCount = sampleCount_;
    depthDesc.label = ToWgpuStringView("DepthBuffer");
    depthTexture_ = wgpuDeviceCreateTexture(device_, &depthDesc);
    if (depthTexture_) {
        depthView_ = wgpuTextureCreateView(depthTexture_, nullptr);
    }

    BGL_LOG_INFO("WebGPU HDR Render Buffers created ({}x{}, Color: RGBA16Float, Depth: Depth24Plus, MSAA: {}x)", width_, height_, sampleCount_);
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

    auto onAdapter = [](WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView msg, void* userdata1, void* userdata2) {
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
    BGL_LOG_INFO("WebGPU Adapter: {} (Driver: {}, Backend: {})",
                 adapterInfo.device.data ? adapterInfo.device.data : "Unknown GPU",
                 adapterInfo.description.data ? adapterInfo.description.data : "Unknown Driver",
                 static_cast<int>(adapterInfo.backendType));

    // 4. Query Hardware Texture Compression Features (BC7 / ASTC / ETC2) & Request Device
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

    BGL_LOG_INFO("WebGPU Hardware Texture Compression Support: BC7={} | ASTC_4x4={} | ETC2={}",
                 supportsBC_ ? "YES" : "NO",
                 supportsASTC_ ? "YES" : "NO",
                 supportsETC2_ ? "YES" : "NO");

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

    WGPUDeviceDescriptor devDesc = WGPU_DEVICE_DESCRIPTOR_INIT;
    devDesc.label = ToWgpuStringView("BglDevice");
    devDesc.requiredFeatureCount = requestedFeatures.size();
    devDesc.requiredFeatures = requestedFeatures.empty() ? nullptr : requestedFeatures.data();

    struct DeviceData {
        WGPUDevice device = nullptr;
        bool done = false;
    } devData;

    auto onDevice = [](WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView msg, void* userdata1, void* userdata2) {
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

        WGPUPresentMode chosenPresentMode{WGPUPresentMode_Fifo};
        if (caps.presentModeCount > 0 && caps.presentModes != nullptr) {
            for (size_t i{0}; i < caps.presentModeCount; ++i) {
                if (caps.presentModes[i] == WGPUPresentMode_Immediate) {
                    chosenPresentMode = WGPUPresentMode_Immediate;
                    break;
                } else if (caps.presentModes[i] == WGPUPresentMode_Mailbox) {
                    chosenPresentMode = WGPUPresentMode_Mailbox;
                }
            }
        }

        WGPUSurfaceConfiguration configSurface = {};
        configSurface.device = device_;
        configSurface.format = format_;
        configSurface.usage = WGPUTextureUsage_RenderAttachment;
        configSurface.width = width_;
        configSurface.height = height_;
        configSurface.presentMode = chosenPresentMode;
        configSurface.alphaMode = WGPUCompositeAlphaMode_Auto;

        wgpuSurfaceConfigure(surface_, &configSurface);
        BGL_LOG_INFO("WebGPU Surface configured ({}x{}, format: {})", width_, height_, static_cast<int>(format_));
    }

    CreateRenderBuffers();
#endif

    initialized_ = true;
    BGL_LOG_INFO("Native WebGPU Context successfully ready!");
    return {};
}

void Context::Resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    width_ = width;
    height_ = height;
#if defined(BGL_PLATFORM_NATIVE)
    if (surface_ && device_ && adapter_) {
        WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
        wgpuSurfaceGetCapabilities(surface_, adapter_, &caps);
        WGPUPresentMode chosenPresentMode{WGPUPresentMode_Fifo};
        if (caps.presentModeCount > 0 && caps.presentModes != nullptr) {
            for (size_t i{0}; i < caps.presentModeCount; ++i) {
                if (caps.presentModes[i] == WGPUPresentMode_Immediate) {
                    chosenPresentMode = WGPUPresentMode_Immediate;
                    break;
                } else if (caps.presentModes[i] == WGPUPresentMode_Mailbox) {
                    chosenPresentMode = WGPUPresentMode_Mailbox;
                }
            }
        }

        WGPUSurfaceConfiguration configSurface = {};
        configSurface.device = device_;
        configSurface.format = format_;
        configSurface.usage = WGPUTextureUsage_RenderAttachment;
        configSurface.width = width_;
        configSurface.height = height_;
        configSurface.presentMode = chosenPresentMode;
        configSurface.alphaMode = WGPUCompositeAlphaMode_Auto;
        wgpuSurfaceConfigure(surface_, &configSurface);
    }
    CreateRenderBuffers();
#endif
    BGL_LOG_INFO("WebGPU Surface resized to {}x{}", width, height);
}

bool Context::PrepareFrame() {
#if defined(BGL_PLATFORM_NATIVE)
    if (encoder_) return true;
    if (!surface_ || !device_ || !queue_) return false;

    surfaceTexture_ = {};
    wgpuSurfaceGetCurrentTexture(surface_, &surfaceTexture_);
    if (surfaceTexture_.status == WGPUSurfaceGetCurrentTextureStatus_Timeout ||
        surfaceTexture_.status == WGPUSurfaceGetCurrentTextureStatus_Outdated) {
        WGPUPresentMode chosenPresentMode{WGPUPresentMode_Fifo};
        if (adapter_) {
            WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
            wgpuSurfaceGetCapabilities(surface_, adapter_, &caps);
            if (caps.presentModeCount > 0 && caps.presentModes != nullptr) {
                for (size_t i{0}; i < caps.presentModeCount; ++i) {
                    if (caps.presentModes[i] == WGPUPresentMode_Immediate) {
                        chosenPresentMode = WGPUPresentMode_Immediate;
                        break;
                    } else if (caps.presentModes[i] == WGPUPresentMode_Mailbox) {
                        chosenPresentMode = WGPUPresentMode_Mailbox;
                    }
                }
            }
        }

        WGPUSurfaceConfiguration configSurface = {};
        configSurface.device = device_;
        configSurface.format = format_;
        configSurface.usage = WGPUTextureUsage_RenderAttachment;
        configSurface.width = width_;
        configSurface.height = height_;
        configSurface.presentMode = chosenPresentMode;
        configSurface.alphaMode = WGPUCompositeAlphaMode_Auto;
        wgpuSurfaceConfigure(surface_, &configSurface);
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
    return encoder_ != nullptr;
#else
    return false;
#endif
}

WGPURenderPassEncoder Context::BeginFrame() {
#if defined(BGL_PLATFORM_NATIVE)
    if (!encoder_ && !PrepareFrame()) {
        return nullptr;
    }

    WGPURenderPassColorAttachment colorAttachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    if (sampleCount_ > 1 && msaaColorView_ && hdrResolveView_) {
        colorAttachment.view = msaaColorView_;
        colorAttachment.resolveTarget = hdrResolveView_;
        colorAttachment.storeOp = WGPUStoreOp_Discard;
    } else if (hdrResolveView_) {
        colorAttachment.view = hdrResolveView_;
        colorAttachment.resolveTarget = nullptr;
        colorAttachment.storeOp = WGPUStoreOp_Store;
    } else if (sampleCount_ > 1 && msaaColorView_) {
        colorAttachment.view = msaaColorView_;
        colorAttachment.resolveTarget = currentView_;
        colorAttachment.storeOp = WGPUStoreOp_Discard;
    } else {
        colorAttachment.view = currentView_;
        colorAttachment.resolveTarget = nullptr;
        colorAttachment.storeOp = WGPUStoreOp_Store;
    }
    colorAttachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    colorAttachment.loadOp = WGPULoadOp_Clear;
    colorAttachment.clearValue = WGPUColor{ 0.04, 0.06, 0.09, 1.0 }; // Linear HDR dark studio slate

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

WGPURenderPassEncoder Context::BeginPostProcessPass(WGPURenderPassEncoder hdrPass) {
#if defined(BGL_PLATFORM_NATIVE)
    if (hdrPass) {
        wgpuRenderPassEncoderEnd(hdrPass);
        wgpuRenderPassEncoderRelease(hdrPass);
    }
    if (!encoder_ || !currentView_) {
        return nullptr;
    }

    WGPURenderPassColorAttachment colorAttachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    colorAttachment.view = currentView_;
    colorAttachment.resolveTarget = nullptr;
    colorAttachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    colorAttachment.loadOp = WGPULoadOp_Clear;
    colorAttachment.storeOp = WGPUStoreOp_Store;
    colorAttachment.clearValue = WGPUColor{ 0.05, 0.07, 0.11, 1.0 };

    WGPURenderPassDescriptor passDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    passDesc.colorAttachmentCount = 1;
    passDesc.colorAttachments = &colorAttachment;
    passDesc.depthStencilAttachment = nullptr;
    passDesc.label = ToWgpuStringView("SwapchainTonemapAndHudPass");

    return wgpuCommandEncoderBeginRenderPass(encoder_, &passDesc);
#else
    (void)hdrPass;
    return nullptr;
#endif
}

void Context::EndFrame(WGPURenderPassEncoder pass) {
#if defined(BGL_PLATFORM_NATIVE)
    if (pass) {
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    }

    if (encoder_) {
        CommandBufferHandle cmdBuf{wgpuCommandEncoderFinish(encoder_, nullptr)};
        encoder_.Reset();

        WGPUCommandBuffer rawCmd{cmdBuf.Get()};
        wgpuQueueSubmit(queue_, 1, &rawCmd);
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
