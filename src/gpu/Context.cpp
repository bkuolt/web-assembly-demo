/**
 * @file Context.cpp
 * @brief Implementation of hardware native WebGPU context using wgpu-native with 4x MSAA support.
 */
#include "gpu/Context.hpp"
#include "core/Log.hpp"

#if defined(BGL_PLATFORM_NATIVE)
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <webgpu/wgpu.h>
#endif

namespace bgl::gpu {

Context::Context() = default;

Context::~Context() {
#if defined(BGL_PLATFORM_NATIVE)
    if (msaaColorView_) {
        wgpuTextureViewRelease(msaaColorView_);
        msaaColorView_ = nullptr;
    }
    if (msaaColorTexture_) {
        wgpuTextureRelease(msaaColorTexture_);
        msaaColorTexture_ = nullptr;
    }
    if (depthView_) {
        wgpuTextureViewRelease(depthView_);
        depthView_ = nullptr;
    }
    if (depthTexture_) {
        wgpuTextureRelease(depthTexture_);
        depthTexture_ = nullptr;
    }
    if (queue_) {
        wgpuQueueRelease(queue_);
        queue_ = nullptr;
    }
    if (device_) {
        wgpuDeviceRelease(device_);
        device_ = nullptr;
    }
    if (surface_) {
        wgpuSurfaceRelease(surface_);
        surface_ = nullptr;
    }
    if (adapter_) {
        wgpuAdapterRelease(adapter_);
        adapter_ = nullptr;
    }
    if (instance_) {
        wgpuInstanceRelease(instance_);
        instance_ = nullptr;
    }
#endif
    initialized_ = false;
    BGL_LOG_INFO("Native WebGPU Context cleanly shut down.");
}

void Context::CreateRenderBuffers() {
#if defined(BGL_PLATFORM_NATIVE)
    if (msaaColorView_) {
        wgpuTextureViewRelease(msaaColorView_);
        msaaColorView_ = nullptr;
    }
    if (msaaColorTexture_) {
        wgpuTextureRelease(msaaColorTexture_);
        msaaColorTexture_ = nullptr;
    }
    if (depthView_) {
        wgpuTextureViewRelease(depthView_);
        depthView_ = nullptr;
    }
    if (depthTexture_) {
        wgpuTextureRelease(depthTexture_);
        depthTexture_ = nullptr;
    }

    // 1. Create 4x MSAA Color Texture & View (if multisampling enabled)
    if (sampleCount_ > 1) {
        WGPUTextureDescriptor msaaDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        msaaDesc.usage = WGPUTextureUsage_RenderAttachment;
        msaaDesc.dimension = WGPUTextureDimension_2D;
        msaaDesc.size = WGPUExtent3D{ width_, height_, 1 };
        msaaDesc.format = format_;
        msaaDesc.mipLevelCount = 1;
        msaaDesc.sampleCount = sampleCount_;
        msaaDesc.label = WGPUStringView{ "MSAAColorTexture", strlen("MSAAColorTexture") };
        msaaColorTexture_ = wgpuDeviceCreateTexture(device_, &msaaDesc);
        if (msaaColorTexture_) {
            msaaColorView_ = wgpuTextureCreateView(msaaColorTexture_, nullptr);
        }
    }

    // 2. Create Depth Buffer (with matching sample count)
    WGPUTextureDescriptor depthDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    depthDesc.usage = WGPUTextureUsage_RenderAttachment;
    depthDesc.dimension = WGPUTextureDimension_2D;
    depthDesc.size = WGPUExtent3D{ width_, height_, 1 };
    depthDesc.format = WGPUTextureFormat_Depth24Plus;
    depthDesc.mipLevelCount = 1;
    depthDesc.sampleCount = sampleCount_;
    depthDesc.label = WGPUStringView{ "DepthBuffer", strlen("DepthBuffer") };
    depthTexture_ = wgpuDeviceCreateTexture(device_, &depthDesc);
    if (depthTexture_) {
        depthView_ = wgpuTextureCreateView(depthTexture_, nullptr);
    }

    BGL_LOG_INFO("WebGPU Render Buffers created ({}x{}, Depth: Depth24Plus, MSAA: {}x)", width_, height_, sampleCount_);
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

    // 4. Request Device
    WGPUDeviceDescriptor devDesc = WGPU_DEVICE_DESCRIPTOR_INIT;
    devDesc.label = WGPUStringView{ "BglDevice", strlen("BglDevice") };

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

        WGPUSurfaceConfiguration configSurface = {};
        configSurface.device = device_;
        configSurface.format = format_;
        configSurface.usage = WGPUTextureUsage_RenderAttachment;
        configSurface.width = width_;
        configSurface.height = height_;
        configSurface.presentMode = WGPUPresentMode_Fifo;
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
    if (surface_ && device_) {
        WGPUSurfaceConfiguration configSurface = {};
        configSurface.device = device_;
        configSurface.format = format_;
        configSurface.usage = WGPUTextureUsage_RenderAttachment;
        configSurface.width = width_;
        configSurface.height = height_;
        configSurface.presentMode = WGPUPresentMode_Fifo;
        configSurface.alphaMode = WGPUCompositeAlphaMode_Auto;
        wgpuSurfaceConfigure(surface_, &configSurface);
    }
    CreateRenderBuffers();
#endif
    BGL_LOG_INFO("WebGPU Surface resized to {}x{}", width, height);
}

WGPURenderPassEncoder Context::BeginFrame() {
#if defined(BGL_PLATFORM_NATIVE)
    if (!surface_ || !device_ || !queue_) return nullptr;

    surfaceTexture_ = {};
    wgpuSurfaceGetCurrentTexture(surface_, &surfaceTexture_);
    if (surfaceTexture_.status == WGPUSurfaceGetCurrentTextureStatus_Timeout ||
        surfaceTexture_.status == WGPUSurfaceGetCurrentTextureStatus_Outdated) {
        WGPUSurfaceConfiguration configSurface = {};
        configSurface.device = device_;
        configSurface.format = format_;
        configSurface.usage = WGPUTextureUsage_RenderAttachment;
        configSurface.width = width_;
        configSurface.height = height_;
        configSurface.presentMode = WGPUPresentMode_Fifo;
        configSurface.alphaMode = WGPUCompositeAlphaMode_Auto;
        wgpuSurfaceConfigure(surface_, &configSurface);
        return nullptr;
    }

    if (surfaceTexture_.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        surfaceTexture_.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        return nullptr;
    }

    currentView_ = wgpuTextureCreateView(surfaceTexture_.texture, nullptr);
    if (!currentView_) {
        wgpuTextureRelease(surfaceTexture_.texture);
        surfaceTexture_.texture = nullptr;
        return nullptr;
    }

    encoder_ = wgpuDeviceCreateCommandEncoder(device_, nullptr);

    WGPURenderPassColorAttachment colorAttachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    if (sampleCount_ > 1 && msaaColorView_) {
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
    colorAttachment.clearValue = WGPUColor{ 0.12, 0.15, 0.20, 1.0 }; // Studio neutral slate-gray

    WGPURenderPassDescriptor passDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    passDesc.colorAttachmentCount = 1;
    passDesc.colorAttachments = &colorAttachment;

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

void Context::EndFrame(WGPURenderPassEncoder pass) {
#if defined(BGL_PLATFORM_NATIVE)
    if (pass) {
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    }

    if (encoder_) {
        WGPUCommandBuffer cmdBuf = wgpuCommandEncoderFinish(encoder_, nullptr);
        wgpuCommandEncoderRelease(encoder_);
        encoder_ = nullptr;

        wgpuQueueSubmit(queue_, 1, &cmdBuf);
        wgpuCommandBufferRelease(cmdBuf);
    }

    if (surface_) {
        wgpuSurfacePresent(surface_);
    }

    if (currentView_) {
        wgpuTextureViewRelease(currentView_);
        currentView_ = nullptr;
    }

    if (surfaceTexture_.texture) {
        wgpuTextureRelease(surfaceTexture_.texture);
        surfaceTexture_.texture = nullptr;
    }
#endif
}

} // namespace bgl::gpu
