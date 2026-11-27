/**
 * @file Uniforms.hpp
 * @brief Shared WebGPU uniform buffer contracts and dynamic UBO ring-buffer helper.
 */
#ifndef BGL_GPU_UNIFORMS_HPP
#define BGL_GPU_UNIFORMS_HPP

#include "gpu/Handle.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <string_view>

namespace bgl::gpu {

namespace MaterialFlags {
constexpr uint32_t HasBaseColorTex = 1u << 0;
constexpr uint32_t HasMetallicRoughnessTex = 1u << 1;
constexpr uint32_t HasNormalTex = 1u << 2;
constexpr uint32_t HasEmissiveTex = 1u << 3;
constexpr uint32_t HasOcclusionTex = 1u << 4;
constexpr uint32_t HasClearcoatTex = 1u << 5;
constexpr uint32_t HasTransmissionTex = 1u << 6;
constexpr uint32_t HasSheenTex = 1u << 7;
constexpr uint32_t Unlit = 1u << 8;
constexpr uint32_t DoubleSided = 1u << 9;
constexpr uint32_t EnableBumpMapping = 1u << 10;
constexpr uint32_t EnableMipmapping = 1u << 11;
constexpr uint32_t EnableShadows = 1u << 12;
constexpr uint32_t EnableReflections = 1u << 13;
constexpr uint32_t EnableAmbientOcclusion = 1u << 14;
constexpr uint32_t EnableParallaxOcclusion = 1u << 15;
constexpr uint32_t HasThicknessTex = 1u << 16;
constexpr uint32_t HasSkinning = 1u << 17;
constexpr uint32_t HasMorphTargets = 1u << 18;
constexpr uint32_t LodLevelShift = 19u;
constexpr uint32_t LodLevelMask = 3u << 19;
constexpr uint32_t DebugViewShift = 21u;
constexpr uint32_t DebugViewMask = 7u << 21;
constexpr uint32_t UseRtShadows = 1u << 24;
constexpr uint32_t UseRtReflections = 1u << 25;
} // namespace MaterialFlags

/**
 * @struct PerDrawUniforms
 * @brief Dynamic-offset per-draw uniform block (256-byte WebGPU dynamic stride).
 *        Supports independent per-instance transforms, skinning metadata, and 8 GPU morph weights.
 */
struct alignas(16) PerDrawUniforms {
    glm::mat4 viewProj{1.0f};      //   0.. 63
    glm::mat4 model{1.0f};         //  64..127
    glm::mat4 lightViewProj{1.0f}; // 128..191
    glm::vec4 morphWeights0{0.0f}; // 192..207 (morph weights 0..3)
    glm::vec4 morphWeights1{0.0f}; // 208..223 (morph weights 4..7)
    uint32_t jointCount{0};        // 224
    uint32_t morphTargetCount{0};  // 228
    uint32_t vertexCount{0};       // 232
    uint32_t drawFlags{0};         // 236 (global feature toggles + skin/morph + LOD + DebugView)
    glm::vec4 drawParams{0.0f};    // 240..255 (x = lodTransition)
};
static_assert(sizeof(PerDrawUniforms) == 256, "PerDrawUniforms must be exactly 256 bytes (dynamic UBO stride)");
static_assert(alignof(PerDrawUniforms) == 16, "PerDrawUniforms must be 16-byte aligned");

/**
 * @struct MaterialUniforms
 * @brief Static per-material uniform block uploaded once during scene preparation.
 *        Includes independent per-channel KHR_texture_transform scale, offset, and rotation.
 */
struct alignas(16) MaterialUniforms {
    glm::vec4 baseColorFactor{1.0f};                                //   0.. 15
    glm::vec4 emissiveFactor{0.0f, 0.0f, 0.0f, 1.0f};               //  16.. 31 (xyz = rgb, w = strength)
    glm::vec4 sheenColorFactor{0.0f};                               //  32.. 47 (xyz = color, w = roughness)
    glm::vec4 specularColorFactor{1.0f, 1.0f, 1.0f, 0.0f};          //  48.. 63
    glm::vec4 attenuationColor{1.0f};                               //  64.. 79
    float metallicFactor{1.0f};                                     //  80
    float roughnessFactor{1.0f};                                    //  84
    float normalScale{1.0f};                                        //  88
    float occlusionStrength{1.0f};                                  //  92
    float bumpScale{1.0f};                                          //  96
    float alphaCutoff{0.5f};                                        // 100
    uint32_t alphaMode{0};                                          // 104
    uint32_t materialFlags{0};                                      // 108
    float ior{1.5f};                                                // 112
    float clearcoatFactor{0.0f};                                    // 116
    float clearcoatRoughness{0.0f};                                 // 120
    float transmissionFactor{0.0f};                                 // 124
    float specularFactor{1.0f};                                     // 128
    float thicknessFactor{0.0f};                                    // 132
    float attenuationDistance{1e9f};                                // 136
    float reflectionIntensity{1.0f};                                // 140
    glm::vec4 baseColorUvTransform{1.0f, 1.0f, 0.0f, 0.0f};         // 144..159 (scale.xy, offset.xy)
    glm::vec4 metallicRoughnessUvTransform{1.0f, 1.0f, 0.0f, 0.0f}; // 160..175
    glm::vec4 normalUvTransform{1.0f, 1.0f, 0.0f, 0.0f};            // 176..191
    glm::vec4 occlusionUvTransform{1.0f, 1.0f, 0.0f, 0.0f};         // 192..207
    glm::vec4 emissiveUvTransform{1.0f, 1.0f, 0.0f, 0.0f};          // 208..223
    glm::vec4 uvRotations0{0.0f};                                   // 224..239 (baseColor, mr, normal, occlusion)
    glm::vec4 uvRotations1{0.0f};                                   // 240..255 (x = emissive)
};
static_assert(sizeof(MaterialUniforms) == 256, "MaterialUniforms must be exactly 256 bytes (16-byte aligned)");
static_assert(alignof(MaterialUniforms) == 16, "MaterialUniforms must be 16-byte aligned");

struct alignas(16) GpuPunctualLight {
    glm::vec4 posOrDir{0.0f}; // xyz = position or directionToLight, w = type (0=Dir, 1=Point, 2=Spot)
    glm::vec4 colorInt{0.0f}; // xyz = color * intensity, w = range (0 = infinite)
    glm::vec4 spotParams{0.0f, -1.0f, 0.0f, 0.0f}; // xyz = spotDirection, w = reserved
};
static_assert(sizeof(GpuPunctualLight) == 48, "GpuPunctualLight must be 48 bytes");

struct alignas(16) SceneLightingUniforms {
    std::array<glm::vec4, 9> shIrradiance{};
    std::array<GpuPunctualLight, 16> lights{};
    std::array<glm::vec4, 16> spotAngles{};
    std::array<glm::uvec4, 256> clusterMasks{};
    glm::vec4 gridParams{1280.0f, 720.0f, 0.1f, 500.0f};
    glm::vec4 envParams{0.0f, 0.0f, 1.0f, 1.0f};
    glm::mat4 viewMatrix{1.0f};
    glm::vec4 weatherParams{0.0f, 1.0f, 0.3f, 0.0f};
    glm::vec4 cameraPos{0.0f, 0.0f, 0.0f, 1.0f};
    glm::vec4 lightDir{0.35f, 1.0f, 0.25f, 0.866f};
    glm::vec4 renderParams{7.0f, 0.0015f, 1.0f, 0.0f}; // x = envMipCount, y = shadowBias, z = aoIntensity
};
static_assert(sizeof(SceneLightingUniforms) == 5424, "SceneLightingUniforms must be 5424 bytes");
static_assert(alignof(SceneLightingUniforms) == 16, "SceneLightingUniforms must be 16-byte aligned");

struct alignas(16) PrefilterComputeParams {
    float roughness{0.0f};
    uint32_t faceSize{64};
    float sourceSize{64.0f};
    uint32_t sampleCount{64};
};
static_assert(sizeof(PrefilterComputeParams) == 16, "PrefilterComputeParams must be 16 bytes");

/**
 * @class DynamicUniformRing
 * @brief Persistent, dynamically-resizable GPU ring buffer for 256-byte aligned per-draw uniforms.
 *        Batches all draw uniforms for a pass into one queue write and binds via dynamic offsets.
 */
template <typename T>
class DynamicUniformRing {
    static_assert(sizeof(T) % 256 == 0, "Dynamic uniform stride must be a multiple of 256 bytes");

public:
    void Initialize(WGPUDevice device, WGPUQueue queue, WGPUBindGroupLayout bgl, std::string_view label) noexcept {
        Shutdown();
        device_ = device;
        queue_ = queue;
        bgl_ = bgl;
        label_ = label;
    }

    void Shutdown() noexcept {
        bindGroup_.Reset();
        buffer_.Reset();
        capacityDraws_ = 0;
        device_ = nullptr;
        queue_ = nullptr;
        bgl_ = nullptr;
    }

    [[nodiscard]] bool EnsureCapacity(size_t drawCount) noexcept {
        if (!device_ || !bgl_ || drawCount == 0) {
            return false;
        }
        if (drawCount <= capacityDraws_ && buffer_ && bindGroup_) {
            return true;
        }
        constexpr size_t kMaxSafeDraws = (std::numeric_limits<size_t>::max() >> 1u);
        if (drawCount > kMaxSafeDraws || drawCount > (std::numeric_limits<uint64_t>::max() / sizeof(T))) {
            return false;
        }
        if (drawCount > (std::numeric_limits<size_t>::max() / sizeof(T) / 2u)) {
            return false;
        }
        const size_t newCapacity = std::max<size_t>(64, std::bit_ceil(drawCount));
        WGPUBufferDescriptor bufDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
        bufDesc.size = static_cast<uint64_t>(newCapacity) * sizeof(T);
        bufDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        bufDesc.label = ToWgpuStringView(label_);
        BufferHandle newBuf{wgpuDeviceCreateBuffer(device_, &bufDesc)};
        if (!newBuf) {
            return false;
        }

        WGPUBindGroupEntry bgEntry = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntry.binding = 0;
        bgEntry.buffer = newBuf.Get();
        bgEntry.offset = 0;
        bgEntry.size = sizeof(T);

        WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgDesc.layout = bgl_;
        bgDesc.entryCount = 1;
        bgDesc.entries = &bgEntry;
        bgDesc.label = ToWgpuStringView(label_);
        BindGroupHandle newBg{wgpuDeviceCreateBindGroup(device_, &bgDesc)};
        if (!newBg) {
            return false;
        }

        bindGroup_ = std::move(newBg);
        buffer_ = std::move(newBuf);
        capacityDraws_ = newCapacity;
        return true;
    }

    [[nodiscard]] WGPUBindGroup Upload(std::span<const T> items) noexcept {
        if (items.empty() || !queue_ || !EnsureCapacity(items.size())) {
            return nullptr;
        }
        wgpuQueueWriteBuffer(queue_, buffer_.Get(), 0, items.data(), items.size() * sizeof(T));
        return bindGroup_.Get();
    }

private:
    WGPUDevice device_{nullptr};
    WGPUQueue queue_{nullptr};
    WGPUBindGroupLayout bgl_{nullptr};
    std::string_view label_{"DynamicUniformRing"};
    BufferHandle buffer_{};
    BindGroupHandle bindGroup_{};
    size_t capacityDraws_{0};
};

} // namespace bgl::gpu

#endif // BGL_GPU_UNIFORMS_HPP
