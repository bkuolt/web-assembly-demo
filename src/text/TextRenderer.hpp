/**
 * @file TextRenderer.hpp
 * @brief WebGPU 2D Batched Quad and Text Renderer with alpha blending and 4x MSAA support.
 */
#pragma once

#include <string_view>
#include <vector>
#include <glm/glm.hpp>
#include <webgpu/webgpu.h>
#include "core/Types.hpp"
#include "text/FontAtlas.hpp"
#include "text/TextShaper.hpp"

namespace bgl::text {

/**
 * @struct TextVertex
 * @brief 2D vertex layout for batched text and UI quads.
 */
struct TextVertex {
    glm::vec2 pos{0.0f};   ///< Location 0: Screen position (X, Y in pixels)
    glm::vec2 uv{0.0f};    ///< Location 1: UV coordinates in Font Atlas
    glm::vec4 color{1.0f}; ///< Location 2: RGBA color tint
};

/**
 * @class TextRenderer
 * @brief Batched WebGPU 2D text and UI renderer with orthographic projection.
 */
class TextRenderer {
public:
    TextRenderer();
    ~TextRenderer();

    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    /**
     * @brief Initializes WebGPU shaders, bind group layouts, and render pipeline.
     */
    core::Result<void> Initialize(WGPUDevice device,
                                  WGPUQueue queue,
                                  WGPUTextureFormat surfaceFormat,
                                  uint32_t sampleCount,
                                  FontAtlas& fontAtlas);

    /**
     * @brief Enqueues a solid or translucent rectangle.
     */
    void DrawRect(float x, float y, float width, float height, const glm::vec4& color);

    /**
     * @brief Enqueues a framed rectangle with outline border.
     */
    void DrawRectWithBorder(float x, float y, float width, float height,
                            const glm::vec4& fillColor, const glm::vec4& borderColor,
                            float borderWidth = 1.0f);

    /**
     * @brief Enqueues shaped text using HarfBuzz and FreeType font atlas.
     */
    void DrawText(std::string_view text, float x, float y,
                  const glm::vec4& color = glm::vec4(1.0f), float scale = 1.0f);

    /**
     * @brief Flushes batched quads and renders them in the current render pass encoder.
     */
    void Render(WGPURenderPassEncoder pass, uint32_t viewportWidth, uint32_t viewportHeight);

    [[nodiscard]] FontAtlas& GetFontAtlas() noexcept { return *fontAtlas_; }

private:
    void EnsureBuffers(size_t vertexCount, size_t indexCount);

    WGPUDevice device_ = nullptr;
    WGPUQueue queue_ = nullptr;
    WGPUTextureFormat surfaceFormat_ = WGPUTextureFormat_BGRA8Unorm;
    uint32_t sampleCount_ = 4;
    FontAtlas* fontAtlas_ = nullptr;

    WGPUBindGroupLayout bindGroupLayout_ = nullptr;
    WGPURenderPipeline pipeline_ = nullptr;
    WGPUBuffer uniformBuffer_ = nullptr;
    WGPUBindGroup bindGroup_ = nullptr;

    WGPUBuffer vertexBuffer_ = nullptr;
    WGPUBuffer indexBuffer_ = nullptr;
    size_t vertexBufferCapacity_ = 0;
    size_t indexBufferCapacity_ = 0;

    std::vector<TextVertex> vertexBatch_;
    std::vector<uint32_t> indexBatch_;
};

} // namespace bgl::text
