/**
 * @file Context.cpp
 * @brief Implementation of hardware native WebGPU context using wgpu-native.
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

core::Result<void> Context::Initialize(const ContextConfig& config) {
    width_ = config.width;
    height_ = config.height;
    nativeWindow_ = config.nativeWindowHandle;

#if defined(BGL_PLATFORM_NATIVE)
    BGL_LOG_INFO("Initializing hardware native WebGPU runtime via wgpu-native...");

    WGPUInstanceDescriptor instanceDesc = {};
    instance_ = wgpuCreateInstance(&instanceDesc);
    if (!instance_) {
        return std::unexpected("Failed to create native WebGPU instance");
    }

    // Surface creation from SDL2 native X11 window
    if (nativeWindow_) {
        SDL_Window* window = static_cast<SDL_Window*>(nativeWindow_);
        SDL_SysWMinfo wmInfo;
        SDL_VERSION(&wmInfo.version);
        if (SDL_GetWindowWMInfo(window, &wmInfo)) {
#if defined(SDL_VIDEO_DRIVER_X11)
            if (wmInfo.subsystem == SDL_SYSWM_X11) {
                WGPUSurfaceSourceXlibWindow fromXlib = {};
                fromXlib.chain.sType = WGPUSType_SurfaceSourceXlibWindow;
                fromXlib.display = wmInfo.info.x11.display;
                fromXlib.window = wmInfo.info.x11.window;

                WGPUSurfaceDescriptor surfaceDesc = {};
                surfaceDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&fromXlib);
                surface_ = wgpuInstanceCreateSurface(instance_, &surfaceDesc);
                BGL_LOG_INFO("Native X11 WebGPU surface successfully created.");
            }
#endif
        }
    }

    // Request Adapter
    struct RequestAdapterData {
        WGPUAdapter adapter = nullptr;
        bool requestEnded = false;
    } adapterData;

    WGPURequestAdapterCallbackInfo adapterCbInfo = {};
    adapterCbInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    adapterCbInfo.userdata1 = &adapterData;
    adapterCbInfo.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void* userdata1, void* userdata2) {
        (void)userdata2;
        auto* data = static_cast<RequestAdapterData*>(userdata1);
        if (status == WGPURequestAdapterStatus_Success) {
            data->adapter = adapter;
        } else {
            BGL_LOG_WARN("Adapter request status: {}", static_cast<int>(status));
        }
        data->requestEnded = true;
    };

    WGPURequestAdapterOptions options = {};
    options.compatibleSurface = surface_;
    options.powerPreference = WGPUPowerPreference_HighPerformance;

    wgpuInstanceRequestAdapter(instance_, &options, adapterCbInfo);
    while (!adapterData.requestEnded) {
        wgpuInstanceProcessEvents(instance_);
    }

    adapter_ = adapterData.adapter;
    if (!adapter_) {
        return std::unexpected("Failed to acquire suitable native WebGPU adapter (Vulkan/Metal/DirectX)");
    }

    WGPUAdapterInfo info = {};
    wgpuAdapterGetInfo(adapter_, &info);
    BGL_LOG_INFO("WebGPU Adapter: {} (Driver: {}, Backend: {})",
                 info.device.data ? info.device.data : "GPU",
                 info.description.data ? info.description.data : "Native",
                 static_cast<int>(info.backendType));

    // Request Device
    struct RequestDeviceData {
        WGPUDevice device = nullptr;
        bool requestEnded = false;
    } deviceData;

    WGPURequestDeviceCallbackInfo deviceCbInfo = {};
    deviceCbInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    deviceCbInfo.userdata1 = &deviceData;
    deviceCbInfo.callback = [](WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* userdata1, void* userdata2) {
        (void)userdata2;
        auto* data = static_cast<RequestDeviceData*>(userdata1);
        if (status == WGPURequestDeviceStatus_Success) {
            data->device = device;
        } else {
            BGL_LOG_WARN("Device request status: {}", static_cast<int>(status));
        }
        data->requestEnded = true;
    };

    WGPUDeviceDescriptor deviceDesc = {};
    wgpuAdapterRequestDevice(adapter_, &deviceDesc, deviceCbInfo);
    while (!deviceData.requestEnded) {
        wgpuInstanceProcessEvents(instance_);
    }

    device_ = deviceData.device;
    if (!device_) {
        return std::unexpected("Failed to create native WebGPU device");
    }

    queue_ = wgpuDeviceGetQueue(device_);

    if (surface_) {
        WGPUSurfaceCapabilities caps = {};
        wgpuSurfaceGetCapabilities(surface_, adapter_, &caps);
        if (caps.formatCount > 0) {
            format_ = caps.formats[0];
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
#endif

    initialized_ = true;
    BGL_LOG_INFO("Native WebGPU Context successfully ready!");
    return {};
}

void Context::Resize(uint32_t width, uint32_t height) {
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
#endif
    BGL_LOG_INFO("WebGPU Surface resized to {}x{}", width, height);
}

void Context::BeginFrame() {
    // Frame setup performed in render encoder
}

void Context::EndFrame() {
#if defined(BGL_PLATFORM_NATIVE)
    if (surface_) {
        wgpuSurfacePresent(surface_);
    }
#endif
}

} // namespace bgl::gpu
