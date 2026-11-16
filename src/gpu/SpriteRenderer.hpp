/**
 * @file SpriteRenderer.hpp
 * @brief Batched WebGPU renderer for 2D screen sprites and depth-sorted 3D camera-facing HDR billboards.
 */
#ifndef BGL_GPU_SPRITERENDERER_HPP
#define BGL_GPU_SPRITERENDERER_HPP

#include "core/Sprite.hpp"
#include "core/Types.hpp"
#include "gpu/Handle.hpp"
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>
#include <vector>
#include <webgpu/webgpu.h>

namespace bgl::gpu {

/**
 * @struct SpriteUniforms
 * @brief Camera projection and color-encoding parameters bound at `@group(0)`.
 */
struct alignas(16) SpriteUniforms {
    glm::mat4 viewProj{1.0f};
    glm::vec4 params{0.0f}; ///< x: 1.0 to encode sRGB output (non-sRGB 2D swapchain), 0.0 for HDR/sRGB target
};

static_assert(sizeof(SpriteUniforms) == 80, "SpriteUniforms must be 80 bytes");

/**
 * @struct SpriteVertex
 * @brief Interleaved vertex attributes for batched 2D sprite and 3D billboard quads.
 */
struct SpriteVertex {
    glm::vec3 pos{0.0f};
    glm::vec2 uv{0.0f};
    glm::vec4 color{1.0f};
};

static_assert(sizeof(SpriteVertex) == 36, "SpriteVertex must be 36 bytes");

/**
 * @class SpriteRenderer
 * @brief Manages WebGPU pipelines, texture caching, sorting, and batched submission for `core::Sprite`
 *        (2D swapchain pass) and `core::Billboard` (3D HDR MSAA transparent pass).
 */
class SpriteRenderer {
public:
    SpriteRenderer() = default;
    ~SpriteRenderer();

    SpriteRenderer(const SpriteRenderer&) = delete;
    SpriteRenderer& operator=(const SpriteRenderer&) = delete;
    SpriteRenderer(SpriteRenderer&& other) noexcept;
    SpriteRenderer& operator=(SpriteRenderer&& other) noexcept;

    /**
     * @brief Initializes shader pipelines, samplers, fallback 1x1 white texture, and per-pass dynamic GPU buffers.
     */
    core::Result<void> Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat hdrFormat,
                                  WGPUTextureFormat swapchainFormat, uint32_t hdrSampleCount = 4,
                                  WGPUTextureFormat depthFormat = WGPUTextureFormat_Depth24Plus);

    /** @brief Hot-reloads `sprite.wgsl` and recreates render pipelines. */
    core::Result<void> ReloadShader();

    /** @brief Releases all pipelines, cached GPU textures, samplers, and dynamic buffers. */
    void Shutdown() noexcept;

    /** @brief Queues a 2D sprite for the current frame's swapchain pass. */
    void SubmitSprite(const core::Sprite& sprite);

    /** @brief Queues a 3D billboard for the current frame's HDR transparent pass. */
    void SubmitBillboard(const core::Billboard& billboard);

    /**
     * @brief Builds, depth-sorts, and draws all queued 3D billboards inside the active MSAA HDR transparent pass.
     */
    void Render3D(WGPURenderPassEncoder hdrTransparentPass, const glm::mat4& viewProj, const glm::vec3& cameraPos,
                  const glm::mat4& viewMatrix);

    /**
     * @brief Builds, layer-sorts, and draws all queued 2D sprites inside the active 1x swapchain pass.
     */
    void Render2D(WGPURenderPassEncoder swapchainPass, uint32_t viewportWidth, uint32_t viewportHeight);

    /** @brief Clears queued per-frame sprite/billboard submission lists and prunes expired cached textures. */
    void ResetFrame() noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept { return gpuInitialized_; }
    [[nodiscard]] size_t GetQueuedSpriteCount() const noexcept { return pendingSprites2D_.size(); }
    [[nodiscard]] size_t GetQueuedBillboardCount() const noexcept { return pendingBillboards3D_.size(); }
    [[nodiscard]] size_t GetLast3DBatchCount() const noexcept { return last3DBatchCount_; }
    [[nodiscard]] size_t GetLast2DBatchCount() const noexcept { return last2DBatchCount_; }
    [[nodiscard]] size_t GetCachedTextureCount() const noexcept { return textureCache_.size(); }

    /**
     * @brief CPU helper constructing 4 vertices for a rotated/anchored 2D sprite quad (exposed for tests).
     */
    static void BuildSpriteQuad(const core::Sprite& sprite, std::vector<SpriteVertex>& outVerts);

    /**
     * @brief CPU helper constructing 4 vertices for an oriented/anchored 3D billboard quad (exposed for tests).
     */
    static void BuildBillboardQuad(const core::Billboard& billboard, const glm::vec3& cameraPos,
                                   const glm::mat4& viewMatrix, std::vector<SpriteVertex>& outVerts);

private:
    struct CachedTexture {
        TextureHandle texture{};
        TextureViewHandle view{};
        BindGroupHandle bindGroup{};
        std::weak_ptr<const core::SpriteTexture> owner{};
        size_t trackedBytes{0};
        bool nearestFilter{false};
    };

    struct PassBuffers {
        BufferHandle vb{};
        BufferHandle ib{};
        BufferHandle ubo{};
        BindGroupHandle uboBg{};
        size_t vertexCap{0};
        size_t indexCap{0};
        size_t vbBytes{0};
        size_t ibBytes{0};
        bool uboTracked{false};

        void Release() noexcept;
    };

    struct DrawBatch {
        WGPURenderPipeline pipeline{nullptr};
        uint64_t textureKey{0};
        bool nearestFilter{false};
        uint32_t firstIndex{0};
        uint32_t indexCount{0};
    };

    core::Result<void> BuildAllPipelines(WGPUShaderModule shaderModule);
    [[nodiscard]] RenderPipelineHandle CreateSinglePipeline(WGPUShaderModule shaderModule,
                                                            WGPUTextureFormat colorFormat, uint32_t sampleCount,
                                                            bool useDepth, bool depthTest, core::SpriteBlendMode blend,
                                                            const char* label);
    core::Result<void> CreatePassUniformBuffer(PassBuffers& pass, const char* label);
    bool EnsurePassGeometryCapacity(PassBuffers& pass, size_t vertexCount, size_t indexCount);
    WGPUBindGroup ResolveTextureBindGroup(const std::shared_ptr<const core::SpriteTexture>& tex);
    void EmitBatches(WGPURenderPassEncoder pass, const PassBuffers& gpuBufs, const std::vector<SpriteVertex>& verts,
                     const std::vector<uint32_t>& idxs, const SpriteUniforms& uboData,
                     const std::vector<DrawBatch>& batches);
    void PruneExpiredTextures() noexcept;

    WGPUDevice device_{nullptr};
    WGPUQueue queue_{nullptr};
    WGPUTextureFormat hdrFormat_{WGPUTextureFormat_RGBA16Float};
    WGPUTextureFormat swapchainFormat_{WGPUTextureFormat_BGRA8Unorm};
    uint32_t hdrSampleCount_{4};
    WGPUTextureFormat depthFormat_{WGPUTextureFormat_Depth24Plus};
    bool gpuInitialized_{false};

    BindGroupLayoutHandle uboBgl_{};
    BindGroupLayoutHandle texBgl_{};
    PipelineLayoutHandle pipelineLayout_{};
    SamplerHandle linearSampler_{};
    SamplerHandle nearestSampler_{};

    RenderPipelineHandle hdrAlphaDepth_{};
    RenderPipelineHandle hdrAdditiveDepth_{};
    RenderPipelineHandle hdrAlphaNoDepth_{};
    RenderPipelineHandle hdrAdditiveNoDepth_{};
    RenderPipelineHandle swapAlpha_{};
    RenderPipelineHandle swapAdditive_{};

    PassBuffers pass3d_{};
    PassBuffers pass2d_{};
    std::unordered_map<uint64_t, CachedTexture> textureCache_{};

    std::vector<core::Sprite> pendingSprites2D_{};
    std::vector<core::Billboard> pendingBillboards3D_{};
    std::vector<SpriteVertex> scratchVerts_{};
    std::vector<uint32_t> scratchIndices_{};
    std::vector<DrawBatch> scratchBatches_{};

    size_t last3DBatchCount_{0};
    size_t last2DBatchCount_{0};
};

} // namespace bgl::gpu

#endif // BGL_GPU_SPRITERENDERER_HPP
