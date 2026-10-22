/**
 * @file webgpu.h
 * @brief WebGPU C API header definition for Dual-Target Emscripten & Native Builds
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WGPUInstanceImpl* WGPUInstance;
typedef struct WGPUAdapterImpl* WGPUAdapter;
typedef struct WGPUDeviceImpl* WGPUDevice;
typedef struct WGPUQueueImpl* WGPUQueue;
typedef struct WGPUSurfaceImpl* WGPUSurface;
typedef struct WGPUSwapChainImpl* WGPUSwapChain;
typedef struct WGPUTextureImpl* WGPUTexture;
typedef struct WGPUTextureViewImpl* WGPUTextureView;
typedef struct WGPUSamplerImpl* WGPUSampler;
typedef struct WGPUBufferImpl* WGPUBuffer;
typedef struct WGPUBindGroupLayoutImpl* WGPUBindGroupLayout;
typedef struct WGPUBindGroupImpl* WGPUBindGroup;
typedef struct WGPUPipelineLayoutImpl* WGPUPipelineLayout;
typedef struct WGPUShaderModuleImpl* WGPUShaderModule;
typedef struct WGPURenderPipelineImpl* WGPURenderPipeline;
typedef struct WGPUCommandEncoderImpl* WGPUCommandEncoder;
typedef struct WGPUCommandBufferImpl* WGPUCommandBuffer;
typedef struct WGPURenderPassEncoderImpl* WGPURenderPassEncoder;

typedef enum WGPUTextureFormat {
    WGPUTextureFormat_Undefined = 0x00000000,
    WGPUTextureFormat_RGBA8Unorm = 0x00000016,
    WGPUTextureFormat_RGBA8UnormSrgb = 0x00000017,
    WGPUTextureFormat_BGRA8Unorm = 0x00000018,
    WGPUTextureFormat_BGRA8UnormSrgb = 0x00000019,
    WGPUTextureFormat_Depth24Plus = 0x00000028,
    WGPUTextureFormat_Depth32Float = 0x0000002A,
} WGPUTextureFormat;

typedef enum WGPUBufferUsage {
    WGPUBufferUsage_None = 0x00000000,
    WGPUBufferUsage_MapRead = 0x00000001,
    WGPUBufferUsage_MapWrite = 0x00000002,
    WGPUBufferUsage_CopySrc = 0x00000004,
    WGPUBufferUsage_CopyDst = 0x00000008,
    WGPUBufferUsage_Index = 0x00000010,
    WGPUBufferUsage_Vertex = 0x00000020,
    WGPUBufferUsage_Uniform = 0x00000040,
    WGPUBufferUsage_Storage = 0x00000080,
    WGPUBufferUsage_Indirect = 0x00000100,
    WGPUBufferUsage_QueryResolve = 0x00000200,
} WGPUBufferUsage;

typedef enum WGPUTextureUsage {
    WGPUTextureUsage_None = 0x00000000,
    WGPUTextureUsage_CopySrc = 0x00000001,
    WGPUTextureUsage_CopyDst = 0x00000002,
    WGPUTextureUsage_TextureBinding = 0x00000004,
    WGPUTextureUsage_StorageBinding = 0x00000008,
    WGPUTextureUsage_RenderAttachment = 0x00000010,
} WGPUTextureUsage;

typedef enum WGPUPrimitiveTopology {
    WGPUPrimitiveTopology_PointList = 0x00000000,
    WGPUPrimitiveTopology_LineList = 0x00000001,
    WGPUPrimitiveTopology_LineStrip = 0x00000002,
    WGPUPrimitiveTopology_TriangleList = 0x00000003,
    WGPUPrimitiveTopology_TriangleStrip = 0x00000004,
} WGPUPrimitiveTopology;

typedef enum WGPUCullMode {
    WGPUCullMode_None = 0x00000000,
    WGPUCullMode_Front = 0x00000001,
    WGPUCullMode_Back = 0x00000002,
} WGPUCullMode;

typedef enum WGPUFrontFace {
    WGPUFrontFace_CCW = 0x00000000,
    WGPUFrontFace_CW = 0x00000001,
} WGPUFrontFace;

typedef struct WGPUColor {
    double r;
    double g;
    double b;
    double a;
} WGPUColor;

#ifdef __cplusplus
}
#endif

